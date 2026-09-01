// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE aws_ec2_peer_discovery_localstack_test
#include <boost/test/unit_test.hpp>

/// @file aws_ec2_peer_discovery_localstack_test.cpp
/// @brief Functional coverage for the EC2 tag-scan discovery back-end.
///
/// `.kiro/specs/multi-machine-placement/` task 8, which asks that discovery be
/// **tested functionally, not through a measured row** — bring the hosts up
/// with no static list and assert the cluster forms. The reason is
/// Requirement 3.4's: discovery inside a measured window is a cost in that
/// window, so the thing that proves discovery works must not be the thing that
/// produces a number.
///
/// LocalStack rather than real EC2, and that is the point of running it here
/// at all: the whole subject of this file is `DescribeInstances` filter syntax
/// and tag round-tripping, and those are exactly what a mock of our own would
/// get wrong in the same direction as the code under test. LocalStack answers
/// the real API shape for free, so this can run on every change instead of
/// once when somebody funds it.
///
/// What LocalStack cannot check is IAM: it authorises everything. A principal
/// missing `ec2:DescribeInstances` or `ec2:CreateTags` passes here and fails in
/// the account. That is stated rather than papered over, and it is why
/// `test-audit-aws-leaks.sh --live` exists alongside its stub mode.

#ifdef KYTHIRA_AWS_LOCALSTACK_TESTS
#ifdef KYTHIRA_HAS_AWS_SDK

#include <raft/aws_ec2_peer_discovery.hpp>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/CreateTagsRequest.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/RunInstancesRequest.h>
#include <aws/ec2/model/Tag.h>
#include <aws/ec2/model/TagSpecification.h>
#include <aws/ec2/model/TerminateInstancesRequest.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* LOCALSTACK_ENDPOINT = "http://localhost:4566";
constexpr const char* LOCALSTACK_HOST = "localhost";
constexpr const char* LOCALSTACK_PORT = "4566";
constexpr const char* DUMMY_REGION = "us-east-1";
constexpr const char* DUMMY_KEY = "test";
constexpr const char* DUMMY_SECRET = "test";
constexpr const char* DUMMY_AMI = "ami-12345678";

// The same AWS-SDK-free probe aws_quorum_manager_localstack_test.cpp uses, and
// for the same reason: Aws::InitAPI() has not run at global-fixture
// construction time, so this cannot go through the SDK.
bool localstack_reachable(std::chrono::milliseconds timeout) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(LOCALSTACK_HOST, LOCALSTACK_PORT, &hints, &res) != 0) {
        return false;
    }
    bool connected = false;
    for (addrinfo* p = res; p != nullptr && !connected; p = p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc == 0) {
            connected = true;
        } else if (errno == EINPROGRESS) {
            pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
            if (poll(&pfd, 1, static_cast<int>(timeout.count())) > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
                    connected = true;
                }
            }
        }
        close(fd);
    }
    freeaddrinfo(res);
    return connected;
}

// Registered first so it runs before the SDK fixture; exits with the code
// tests/CMakeLists.txt maps to SKIP_RETURN_CODE so ctest says "Not Run"
// rather than "Failed" when no LocalStack is up.
struct PreflightSkipFixture {
    PreflightSkipFixture() {
        if (!localstack_reachable(std::chrono::milliseconds{2000})) {
            std::cerr << "SKIP: LocalStack not reachable at " << LOCALSTACK_ENDPOINT << "\n";
            std::exit(77);
        }
    }
};

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        folly::init(&argc, &argv, false);
    }
};
#endif

// LocalStack does not check credentials, but the SDK refuses to sign without
// any, so the default chain needs something to find.
struct DummyCredentialsFixture {
    DummyCredentialsFixture() {
        if (getenv("AWS_ACCESS_KEY_ID") == nullptr) {
            setenv("AWS_ACCESS_KEY_ID", DUMMY_KEY, 1);
        }
        if (getenv("AWS_SECRET_ACCESS_KEY") == nullptr) {
            setenv("AWS_SECRET_ACCESS_KEY", DUMMY_SECRET, 1);
        }
    }
};

struct AwsSdkFixture {
    AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::InitAPI(opts);
    }
    ~AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::ShutdownAPI(opts);
    }
};

}  // namespace

BOOST_GLOBAL_FIXTURE(PreflightSkipFixture);
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif
BOOST_GLOBAL_FIXTURE(DummyCredentialsFixture);
BOOST_GLOBAL_FIXTURE(AwsSdkFixture);

namespace {

/// The comma in a two-parameter template-id makes a preprocessor macro see two
/// arguments, so BOOST_CHECK_THROW cannot take the type spelled out.
using discovery = kythira::aws_ec2_peer_discovery<std::uint64_t, std::string>;

auto make_discovery_cfg(const std::string& run_tag) -> kythira::aws_ec2_peer_discovery_config {
    kythira::aws_ec2_peer_discovery_config cfg;
    cfg.aws.region = DUMMY_REGION;
    cfg.aws.endpoint_override = LOCALSTACK_ENDPOINT;
    cfg.aws.api_timeout = std::chrono::seconds{10};
    // credentials_provider is left null so the SDK's default chain picks up
    // the dummy key DummyCredentialsFixture puts in the environment. The field
    // is a shared_ptr<AWSCredentialsProviderChain>, and a
    // SimpleAWSCredentialsProvider is not a chain -- the sibling LocalStack
    // test goes through the environment for the same reason.
    cfg.run_tag_value = run_tag;
    cfg.poll_interval = std::chrono::milliseconds{200};
    return cfg;
}

auto make_raw_client() -> Aws::EC2::EC2Client {
    Aws::Client::ClientConfiguration c;
    c.region = DUMMY_REGION;
    c.endpointOverride = LOCALSTACK_ENDPOINT;
    return Aws::EC2::EC2Client(Aws::Auth::AWSCredentials(DUMMY_KEY, DUMMY_SECRET), nullptr, c);
}

/// Launches one instance carrying the run tag and a role tag, and returns its
/// id. The node-id and address tags are deliberately NOT set here: that is
/// register_node's job, and a test that pre-set them would never exercise it.
auto launch_tagged(Aws::EC2::EC2Client& ec2, const std::string& run_tag, const std::string& role)
    -> std::string {
    Aws::EC2::Model::RunInstancesRequest req;
    req.SetImageId(DUMMY_AMI);
    req.SetMinCount(1);
    req.SetMaxCount(1);
    req.SetInstanceType(Aws::EC2::Model::InstanceType::t3_micro);

    Aws::EC2::Model::Tag run;
    run.SetKey("kythira-perf-run");
    run.SetValue(run_tag);
    Aws::EC2::Model::Tag role_tag;
    role_tag.SetKey("kythira-role");
    role_tag.SetValue(role);
    Aws::EC2::Model::TagSpecification spec;
    spec.SetResourceType(Aws::EC2::Model::ResourceType::instance);
    spec.AddTags(run);
    spec.AddTags(role_tag);
    req.AddTagSpecifications(spec);

    auto out = ec2.RunInstances(req);
    BOOST_REQUIRE_MESSAGE(out.IsSuccess(), "RunInstances failed: " << out.GetError().GetMessage());
    BOOST_REQUIRE(!out.GetResult().GetInstances().empty());
    return std::string(out.GetResult().GetInstances()[0].GetInstanceId());
}

void terminate_all(Aws::EC2::EC2Client& ec2, const std::vector<std::string>& ids) {
    if (ids.empty()) {
        return;
    }
    Aws::EC2::Model::TerminateInstancesRequest req;
    for (const auto& id : ids) {
        req.AddInstanceIds(id);
    }
    ec2.TerminateInstances(req);
}

auto unique_run_tag(const char* what) -> std::string {
    return std::string("disc-test-") + what + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

}  // namespace

// An empty run tag would filter on tag=="" , match nothing, and look exactly
// like a cluster whose peers have not booted. Refusing at construction turns a
// silent wrong answer into a loud one, which is the whole reason the check is
// in the constructor rather than in find_peers.
BOOST_AUTO_TEST_CASE(empty_run_tag_is_refused_at_construction) {
    auto cfg = make_discovery_cfg("");
    BOOST_CHECK_THROW(discovery{cfg}, std::invalid_argument);
}

// The round trip the whole back-end exists for: a node publishes its identity
// onto its own instance, and a DIFFERENT discovery object -- standing in for a
// different machine -- finds it with no static list anywhere.
BOOST_AUTO_TEST_CASE(register_then_find_round_trips_through_ec2_tags) {
    auto ec2 = make_raw_client();
    const auto run_tag = unique_run_tag("roundtrip");
    std::vector<std::string> launched;

    const auto id1 = launch_tagged(ec2, run_tag, "host-1");
    const auto id2 = launch_tagged(ec2, run_tag, "host-2");
    launched = {id1, id2};

    auto cfg1 = make_discovery_cfg(run_tag);
    cfg1.instance_id = id1;
    discovery node1{cfg1};

    auto cfg2 = make_discovery_cfg(run_tag);
    cfg2.instance_id = id2;
    discovery node2{cfg2};

    // Before either registers, the run tag matches two running instances and
    // discovery still returns nothing: the tag-key filter means "has published
    // an identity", not "exists". An instance that has booted and not yet
    // registered is a normal transient state, not a peer.
    auto before = node1.find_peers(std::chrono::milliseconds{5000}).get();
    BOOST_CHECK_EQUAL(before.size(), 0U);

    node1.register_node(1, "http://10.0.0.1:9001").get();
    node2.register_node(2, "http://10.0.0.2:9001").get();

    auto seen = node1.find_peers(std::chrono::milliseconds{5000}).get();
    BOOST_REQUIRE_EQUAL(seen.size(), 2U);
    // Sorted by node id, so the assertions can be positional rather than a
    // search -- and so two runs of the same cluster produce the same order.
    BOOST_CHECK_EQUAL(seen[0].node_id, 1U);
    BOOST_CHECK_EQUAL(seen[0].address, "http://10.0.0.1:9001");
    BOOST_CHECK_EQUAL(seen[1].node_id, 2U);
    BOOST_CHECK_EQUAL(seen[1].address, "http://10.0.0.2:9001");

    // The other machine sees the same set. Discovery that only worked from the
    // node that registered last would pass a single-object test.
    auto seen_from_2 = node2.find_peers(std::chrono::milliseconds{5000}).get();
    BOOST_CHECK_EQUAL(seen_from_2.size(), 2U);

    terminate_all(ec2, launched);
}

// Shape 2 launches N hosts and one driver under ONE run tag, and the driver is
// not a replica. Without the role filter the driver joins the peer set and the
// cluster believes it has N+1 voters, which is a quorum arithmetic bug that
// would present as elections that will not settle.
BOOST_AUTO_TEST_CASE(role_prefix_excludes_the_driver_from_the_peer_set) {
    auto ec2 = make_raw_client();
    const auto run_tag = unique_run_tag("role");

    const auto host_id = launch_tagged(ec2, run_tag, "host-1");
    const auto driver_id = launch_tagged(ec2, run_tag, "driver");

    auto host_cfg = make_discovery_cfg(run_tag);
    host_cfg.instance_id = host_id;
    discovery host{host_cfg};
    host.register_node(1, "http://10.0.0.1:9001").get();

    auto driver_cfg = make_discovery_cfg(run_tag);
    driver_cfg.instance_id = driver_id;
    discovery driver{driver_cfg};
    driver.register_node(99, "http://10.0.0.9:9001").get();

    // Unfiltered: both, because both registered.
    auto unfiltered = host.find_peers(std::chrono::milliseconds{5000}).get();
    BOOST_CHECK_EQUAL(unfiltered.size(), 2U);

    auto filtered_cfg = make_discovery_cfg(run_tag);
    filtered_cfg.instance_id = host_id;
    filtered_cfg.role_tag_prefix = "host";
    discovery filtered{filtered_cfg};
    auto only_hosts = filtered.find_peers(std::chrono::milliseconds{5000}).get();
    BOOST_REQUIRE_EQUAL(only_hosts.size(), 1U);
    BOOST_CHECK_EQUAL(only_hosts[0].node_id, 1U);

    terminate_all(ec2, {host_id, driver_id});
}

// Requirement 3.5. The alternative to failing here is measuring a cluster with
// a replica missing and reporting a number for it, which nothing downstream
// can distinguish from a healthy row.
BOOST_AUTO_TEST_CASE(await_peers_converges_when_every_host_registers) {
    auto ec2 = make_raw_client();
    const auto run_tag = unique_run_tag("converge");

    const auto id1 = launch_tagged(ec2, run_tag, "host-1");
    const auto id2 = launch_tagged(ec2, run_tag, "host-2");
    const auto id3 = launch_tagged(ec2, run_tag, "host-3");

    for (const auto& [idx, id] :
         std::vector<std::pair<std::uint64_t, std::string>>{{1, id1}, {2, id2}, {3, id3}}) {
        auto cfg = make_discovery_cfg(run_tag);
        cfg.instance_id = id;
        discovery d{cfg};
        d.register_node(idx, "http://10.0.0." + std::to_string(idx) + ":9001").get();
    }

    auto cfg = make_discovery_cfg(run_tag);
    cfg.instance_id = id1;
    discovery node1{cfg};
    auto peers = node1.await_peers(3, std::chrono::milliseconds{10000}, {1, 2, 3});
    BOOST_CHECK_EQUAL(peers.size(), 3U);

    terminate_all(ec2, {id1, id2, id3});
}

// The failure this is for is a host that never comes up. What matters is not
// that it throws but WHAT IT SAYS: "saw 2 of 3" sends a reader to the logs,
// "NEVER SEEN: 3" is already the diagnosis. Asserting on the message is
// deliberate -- the diagnostic is the deliverable here, not the exception.
BOOST_AUTO_TEST_CASE(await_peers_names_the_host_that_never_appeared) {
    auto ec2 = make_raw_client();
    const auto run_tag = unique_run_tag("missing");

    const auto id1 = launch_tagged(ec2, run_tag, "host-1");
    const auto id2 = launch_tagged(ec2, run_tag, "host-2");
    // Node 3 is never launched and never registers.

    for (const auto& [idx, id] :
         std::vector<std::pair<std::uint64_t, std::string>>{{1, id1}, {2, id2}}) {
        auto cfg = make_discovery_cfg(run_tag);
        cfg.instance_id = id;
        discovery d{cfg};
        d.register_node(idx, "http://10.0.0." + std::to_string(idx) + ":9001").get();
    }

    auto cfg = make_discovery_cfg(run_tag);
    cfg.instance_id = id1;
    discovery node1{cfg};

    std::string message;
    BOOST_CHECK_THROW(
        [&] {
            try {
                node1.await_peers(3, std::chrono::milliseconds{1500}, {1, 2, 3});
            } catch (const std::runtime_error& ex) {
                message = ex.what();
                throw;
            }
        }(),
        std::runtime_error);

    BOOST_CHECK_MESSAGE(message.find("NEVER SEEN") != std::string::npos,
                        "message must name what was missing, got: " << message);
    BOOST_CHECK_MESSAGE(message.find("3") != std::string::npos,
                        "message must name node 3, got: " << message);
    BOOST_CHECK_MESSAGE(message.find("Refusing to measure") != std::string::npos,
                        "message must say it is refusing rather than proceeding, got: " << message);

    terminate_all(ec2, {id1, id2});
}

// A terminated instance is not a peer. Without the state filter it would stay
// in the set until something else noticed, which is the same reason the leak
// audit excludes `terminated` and `shutting-down`.
BOOST_AUTO_TEST_CASE(terminated_instances_leave_the_peer_set) {
    auto ec2 = make_raw_client();
    const auto run_tag = unique_run_tag("terminated");

    const auto id1 = launch_tagged(ec2, run_tag, "host-1");
    const auto id2 = launch_tagged(ec2, run_tag, "host-2");

    for (const auto& [idx, id] :
         std::vector<std::pair<std::uint64_t, std::string>>{{1, id1}, {2, id2}}) {
        auto cfg = make_discovery_cfg(run_tag);
        cfg.instance_id = id;
        discovery d{cfg};
        d.register_node(idx, "http://10.0.0." + std::to_string(idx) + ":9001").get();
    }

    auto cfg = make_discovery_cfg(run_tag);
    cfg.instance_id = id1;
    discovery node1{cfg};
    BOOST_CHECK_EQUAL(node1.find_peers(std::chrono::milliseconds{5000}).get().size(), 2U);

    terminate_all(ec2, {id2});
    // LocalStack transitions synchronously enough for this, but the loop keeps
    // the case from being a race on a slower machine.
    std::size_t after = 2;
    for (int attempt = 0; attempt < 20; ++attempt) {
        after = node1.find_peers(std::chrono::milliseconds{5000}).get().size();
        if (after == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
    }
    BOOST_CHECK_EQUAL(after, 1U);

    terminate_all(ec2, {id1});
}

#else   // KYTHIRA_HAS_AWS_SDK
BOOST_AUTO_TEST_CASE(aws_sdk_not_available) {
    BOOST_TEST_MESSAGE("AWS SDK not available — aws_ec2_peer_discovery not compiled");
}
#endif  // KYTHIRA_HAS_AWS_SDK
#else   // KYTHIRA_AWS_LOCALSTACK_TESTS
BOOST_AUTO_TEST_CASE(localstack_tests_not_enabled) {
    BOOST_TEST_MESSAGE("LocalStack tests not enabled");
}
#endif  // KYTHIRA_AWS_LOCALSTACK_TESTS
