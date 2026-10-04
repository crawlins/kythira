// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// LocalStack tier of aws-quorum-manager Requirement 16.12-16.14. Start the
// emulator with docker/aws-localstack-compose.yml first. The managers no longer
// derive node ids from instance ids (cloud-composite-node-ids), so LocalStack's
// random 17-hex-digit ids need no patching. The ASG suite skips on the
// community edition, which has no autoscaling service.

#define BOOST_TEST_MODULE aws_quorum_manager_localstack_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_AWS_LOCALSTACK_TESTS
#ifdef KYTHIRA_HAS_AWS_SDK

#include <raft/aws_asg_quorum_manager.hpp>
#include <raft/aws_ec2_quorum_manager.hpp>

#include <aws/autoscaling/AutoScalingClient.h>
#include <aws/autoscaling/model/CreateAutoScalingGroupRequest.h>
#include <aws/autoscaling/model/CreateLaunchConfigurationRequest.h>
#include <aws/autoscaling/model/DeleteAutoScalingGroupRequest.h>
#include <aws/autoscaling/model/DeleteLaunchConfigurationRequest.h>
#include <aws/autoscaling/model/DescribeAutoScalingGroupsRequest.h>
#include <aws/autoscaling/model/UpdateAutoScalingGroupRequest.h>
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/CreateSecurityGroupRequest.h>
#include <aws/ec2/model/CreateSubnetRequest.h>
#include <aws/ec2/model/CreateVpcRequest.h>
#include <aws/ec2/model/DeleteSecurityGroupRequest.h>
#include <aws/ec2/model/DeleteSubnetRequest.h>
#include <aws/ec2/model/DeleteVpcRequest.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/InstanceStateName.h>
#include <aws/ec2/model/StopInstancesRequest.h>
#include <aws/ec2/model/TerminateInstancesRequest.h>
#include <aws/iam/IAMClient.h>
#include <aws/sts/STSClient.h>
#include <aws/sts/model/GetCallerIdentityRequest.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#include <memory>

#endif
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
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

// Cheap, AWS-SDK-free reachability probe: a plain non-blocking TCP connect
// to LocalStack's own port, bounded by `timeout`. Deliberately doesn't use
// the AWS SDK (unlike the fixture's own reachability check via STS
// GetCallerIdentity below) since Aws::InitAPI() hasn't run yet at
// init_unit_test_suite() time -- AwsSdkFixture only runs as a Boost.Test global
// fixture, which is set up *after* init_unit_test_suite() returns.
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

// Run once before any other global fixture constructs: if LocalStack isn't
// reachable (the per-case fixture's own STS-based reachability check would
// hit the same condition, just via a slower AWS-SDK round trip after
// AwsSdkFixture has already initialized), exit immediately with the
// reserved code tests/CMakeLists.txt registers via SKIP_RETURN_CODE, so
// `ctest` reports this test as "Not Run" instead of "Failed" when no
// LocalStack container is running -- which isn't a real bug.
//
// This has to be a BOOST_GLOBAL_FIXTURE, not a custom init_unit_test_suite:
// BOOST_TEST_MODULE makes unit_test_suite.hpp auto-generate its own
// init_unit_test_suite() at global scope, and a same-named function defined
// here would land inside this file's anonymous namespace instead -- a
// distinct, unrelated function with internal linkage that Boost's
// precompiled main() never calls (no redefinition error, just silently
// dead code; confirmed by disassembling the built binary, which only ever
// contained the auto-generated `return 0;` stub). Global fixtures run in
// registration order, so registering this one first still guarantees it
// runs before FollyInitFixture/AwsSdkFixture below.
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
        _init = std::make_unique<folly::Init>(&argc, &argv, false);
    }
    std::unique_ptr<folly::Init> _init;
};
#endif

// Whether LocalStack serves the autoscaling API, and if not, why. Set once by
// AwsSdkFixture and read by the ASG suite's precondition.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
bool g_autoscaling_available = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::string g_autoscaling_unavailable_reason;

struct AwsSdkFixture {
    AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::InitAPI(opts);
        probe_autoscaling();
    }
    ~AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::ShutdownAPI(opts);
    }

    /// Records whether LocalStack's **autoscaling** service is usable.
    ///
    /// `PreflightSkipFixture` can only open a TCP connection, because it has to
    /// run before `Aws::InitAPI()`. That proves LocalStack is listening and
    /// nothing more. Autoscaling is a Pro-only service in the community
    /// edition. Measured against `localstack/localstack:4.14.0`, the image
    /// pinned in `docker/cloudwatch-localstack-compose.yml`:
    ///
    ///     InternalFailure: Sorry, the autoscaling service is not included
    ///     within your LocalStack license, but is available in an upgraded
    ///     license.
    ///
    /// Note `InternalFailure` -- not a 501 and not an authorization error -- so
    /// the error *code* carries no signal; any autoscaling failure at all means
    /// the ASG suite cannot run, so the check does not try to match on either.
    ///
    /// This used to exit 77 for the whole binary, which also skipped the EC2
    /// cases on every community LocalStack -- that is, everywhere this suite
    /// is ever run. EC2 is emulated in the community edition, so now only the
    /// ASG suite is skipped (by its precondition, which reports the reason).
    ///
    /// `DescribeAutoScalingGroups` rather than the `CreateLaunchConfiguration`
    /// the ASG fixture actually needs: it fails identically on an unlicensed
    /// service and creates nothing, so the probe leaves no state behind.
    ///
    /// Called from this constructor rather than from a global fixture of its
    /// own, immediately after `Aws::InitAPI()`. A separate
    /// `BOOST_GLOBAL_FIXTURE` registered after this one crashed in "Test
    /// setup" with a memory access violation even though the registration
    /// order was right, so the ordering is made structural here instead of
    /// depending on Boost.Test's fixture sequencing.
    static void probe_autoscaling() {
        Aws::Client::ClientConfiguration c;
        c.region = DUMMY_REGION;
        c.endpointOverride = LOCALSTACK_ENDPOINT;
        c.requestTimeoutMs = 10000;
        c.connectTimeoutMs = 10000;
        Aws::AutoScaling::AutoScalingClient client{c};
        auto out = client.DescribeAutoScalingGroups(
            Aws::AutoScaling::Model::DescribeAutoScalingGroupsRequest{});
        g_autoscaling_available = out.IsSuccess();
        if (!g_autoscaling_available) {
            // Deliberately does not name a cause. The same failure covers an
            // unlicensed service and a LocalStack that stopped listening
            // between the TCP preflight and here, and the SDK's message says
            // which.
            g_autoscaling_unavailable_reason = "the autoscaling service is unusable at " +
                                               std::string(LOCALSTACK_ENDPOINT) + ": " +
                                               std::string(out.GetError().GetMessage());
            std::cerr << "NOTE: skipping the ASG suite: " << g_autoscaling_unavailable_reason
                      << "\n";
        }
    }
};

BOOST_GLOBAL_FIXTURE(PreflightSkipFixture);
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif
BOOST_GLOBAL_FIXTURE(AwsSdkFixture);

kythira::aws_client_config make_localstack_cfg() {
    kythira::aws_client_config cfg;
    cfg.region = DUMMY_REGION;
    cfg.endpoint_override = LOCALSTACK_ENDPOINT;
    cfg.api_timeout = std::chrono::seconds{10};
    return cfg;
}

auto make_client_cfg() -> Aws::Client::ClientConfiguration {
    Aws::Client::ClientConfiguration c;
    c.region = DUMMY_REGION;
    c.endpointOverride = LOCALSTACK_ENDPOINT;
    c.requestTimeoutMs = 10000;
    c.connectTimeoutMs = 10000;
    return c;
}

Aws::Auth::AWSCredentials dummy_creds() {
    return {DUMMY_KEY, DUMMY_SECRET};
}

// ── Ec2LocalstackFixture ──────────────────────────────────────────────────────

using ec2_manager = kythira::aws_ec2_quorum_manager<>;
using ec2_cluster = std::vector<kythira::node_placement<std::uint64_t, std::string>>;

auto tag_value(const Aws::Vector<Aws::EC2::Model::Tag>& tags, const std::string& key)
    -> std::optional<std::string> {
    for (const auto& t : tags) {
        if (std::string(t.GetKey()) == key) {
            return std::string(t.GetValue());
        }
    }
    return std::nullopt;
}

struct Ec2LocalstackFixture {
    std::string uuid;
    std::string vpc_id;
    std::string subnet_id;
    std::string sg_id;
    std::shared_ptr<Aws::EC2::EC2Client> ec2;

    Ec2LocalstackFixture() {
        // Derive UUID from test ID for resource scoping.
        const auto& tc = boost::unit_test::framework::current_test_case();
        uuid = "kythira-ls-" + std::to_string(tc.p_id);

        // Ensure DefaultAWSCredentialsProviderChain finds credentials for LocalStack.
        if (getenv("AWS_ACCESS_KEY_ID") == nullptr) {
            setenv("AWS_ACCESS_KEY_ID", "test", 1);
        }
        if (getenv("AWS_SECRET_ACCESS_KEY") == nullptr) {
            setenv("AWS_SECRET_ACCESS_KEY", "test", 1);
        }

        Aws::Auth::AWSCredentials creds = dummy_creds();
        auto cli_cfg = make_client_cfg();
        ec2 = std::make_shared<Aws::EC2::EC2Client>(creds, cli_cfg);

        // Verify LocalStack is reachable; throw to abort test without a FAIL assertion.
        Aws::STS::STSClient sts{creds, cli_cfg};
        auto id_out = sts.GetCallerIdentity(Aws::STS::Model::GetCallerIdentityRequest{});
        if (!id_out.IsSuccess()) {
            throw std::runtime_error("LocalStack not reachable (skip): " +
                                     std::string(id_out.GetError().GetMessage()));
        }

        // VPC
        Aws::EC2::Model::CreateVpcRequest vpc_req;
        vpc_req.SetCidrBlock("10.200.0.0/16");
        auto vpc_out = ec2->CreateVpc(vpc_req);
        BOOST_REQUIRE_MESSAGE(vpc_out.IsSuccess(),
                              "CreateVpc: " + std::string(vpc_out.GetError().GetMessage()));
        vpc_id = std::string(vpc_out.GetResult().GetVpc().GetVpcId());

        // Subnet
        Aws::EC2::Model::CreateSubnetRequest sn_req;
        sn_req.SetVpcId(vpc_id);
        sn_req.SetCidrBlock("10.200.1.0/24");
        auto sn_out = ec2->CreateSubnet(sn_req);
        BOOST_REQUIRE_MESSAGE(sn_out.IsSuccess(),
                              "CreateSubnet: " + std::string(sn_out.GetError().GetMessage()));
        subnet_id = std::string(sn_out.GetResult().GetSubnet().GetSubnetId());

        // Security group
        Aws::EC2::Model::CreateSecurityGroupRequest sg_req;
        sg_req.SetGroupName(uuid + "-sg");
        sg_req.SetDescription("kythira localstack test");
        sg_req.SetVpcId(vpc_id);
        auto sg_out = ec2->CreateSecurityGroup(sg_req);
        BOOST_REQUIRE_MESSAGE(sg_out.IsSuccess(), "CreateSecurityGroup: " +
                                                      std::string(sg_out.GetError().GetMessage()));
        sg_id = std::string(sg_out.GetResult().GetGroupId());
    }

    ~Ec2LocalstackFixture() {
        // Teardown: best-effort; errors are logged but not rethrown. Instances
        // first: a VPC still holding one refuses deletion, which LocalStack
        // enforces like AWS does.
        terminate_cluster_instances();
        if (!sg_id.empty()) {
            Aws::EC2::Model::DeleteSecurityGroupRequest del;
            del.SetGroupId(sg_id);
            ec2->DeleteSecurityGroup(del);
        }
        if (!subnet_id.empty()) {
            Aws::EC2::Model::DeleteSubnetRequest del;
            del.SetSubnetId(subnet_id);
            ec2->DeleteSubnet(del);
        }
        if (!vpc_id.empty()) {
            Aws::EC2::Model::DeleteVpcRequest del;
            del.SetVpcId(vpc_id);
            ec2->DeleteVpc(del);
        }
    }

    Ec2LocalstackFixture(const Ec2LocalstackFixture&) = delete;
    Ec2LocalstackFixture& operator=(const Ec2LocalstackFixture&) = delete;
    Ec2LocalstackFixture(Ec2LocalstackFixture&&) = delete;
    Ec2LocalstackFixture& operator=(Ec2LocalstackFixture&&) = delete;

    // Requirement 16.13: the shared manager is built with spot options.
    [[nodiscard]] auto ec2_cfg(std::size_t target = 3) const
        -> kythira::aws_ec2_quorum_manager_config {
        kythira::aws_ec2_quorum_manager_config cfg;
        cfg.cluster_name = uuid;
        cfg.image_id = "ami-12345678";
        cfg.node_port = 7000;
        cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = target});
        cfg.subnet_by_group["AZ1"] = subnet_id;
        cfg.security_group_ids.push_back(sg_id);
        cfg.spot_options = kythira::ec2_spot_options{
            .max_price = "",
            .interruption_behavior = kythira::ec2_spot_interruption_behavior::terminate,
        };
        cfg.provision_timeout = std::chrono::seconds{60};
        cfg.poll_interval = std::chrono::seconds{1};
        cfg.aws = make_localstack_cfg();
        return cfg;
    }

    auto provision(ec2_manager& mgr, std::size_t n) -> ec2_cluster {
        ec2_cluster cluster;
        for (std::size_t i = 0; i < n; ++i) {
            auto peer = std::move(mgr.provision_node("AZ1", std::nullopt)).get();
            cluster.push_back({.node_id = peer.node_id, .group_id = "AZ1"});
        }
        return cluster;
    }

    // The instance carrying numeric node id `nid` in this fixture's cluster,
    // read from its kythira:node-id tag rather than asked of the manager. A
    // live instance wins over a terminated one that still lists.
    auto instance_id_for(std::uint64_t nid) -> std::string {
        Aws::EC2::Model::DescribeInstancesRequest req;
        Aws::EC2::Model::Filter cluster_f;
        cluster_f.SetName("tag:kythira:cluster");
        cluster_f.AddValues(uuid);
        Aws::EC2::Model::Filter id_f;
        id_f.SetName("tag:kythira:node-id");
        id_f.AddValues(std::to_string(nid));
        req.AddFilters(cluster_f);
        req.AddFilters(id_f);
        auto out = ec2->DescribeInstances(req);
        std::string found;
        if (!out.IsSuccess()) {
            return found;
        }
        for (const auto& r : out.GetResult().GetReservations()) {
            for (const auto& i : r.GetInstances()) {
                if (found.empty() ||
                    i.GetState().GetName() != Aws::EC2::Model::InstanceStateName::terminated) {
                    found = std::string(i.GetInstanceId());
                }
            }
        }
        return found;
    }

    auto describe(const std::string& ec2_id) -> std::optional<Aws::EC2::Model::Instance> {
        Aws::EC2::Model::DescribeInstancesRequest req;
        req.AddInstanceIds(ec2_id);
        auto out = ec2->DescribeInstances(req);
        if (!out.IsSuccess()) {
            return std::nullopt;
        }
        for (const auto& r : out.GetResult().GetReservations()) {
            for (const auto& i : r.GetInstances()) {
                return i;
            }
        }
        return std::nullopt;
    }

    auto state_of(std::uint64_t nid) -> std::optional<Aws::EC2::Model::InstanceStateName> {
        auto inst = describe(instance_id_for(nid));
        if (!inst) {
            return std::nullopt;
        }
        return inst->GetState().GetName();
    }

    // Polls until `nid` reports `want`, for up to 30s. LocalStack walks the
    // stop and terminate transitions asynchronously, like EC2 does.
    auto wait_state(std::uint64_t nid, Aws::EC2::Model::InstanceStateName want) -> bool {
        for (int i = 0; i < 60; ++i) {
            if (state_of(nid) == want) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }
        return false;
    }

    void stop(std::uint64_t nid) {
        Aws::EC2::Model::StopInstancesRequest req;
        req.AddInstanceIds(instance_id_for(nid));
        auto out = ec2->StopInstances(req);
        BOOST_REQUIRE_MESSAGE(out.IsSuccess(),
                              "StopInstances: " + std::string(out.GetError().GetMessage()));
        BOOST_REQUIRE_MESSAGE(wait_state(nid, Aws::EC2::Model::InstanceStateName::stopped),
                              "instance never reached `stopped`");
    }

    // The cluster as EC2 sees it: every pending or running instance tagged
    // with this fixture's cluster name, grouped by its kythira:group tag. Read
    // straight from EC2, so a manager that forgot to provision cannot vouch
    // for itself.
    auto live_cluster() -> ec2_cluster {
        Aws::EC2::Model::DescribeInstancesRequest req;
        Aws::EC2::Model::Filter f;
        f.SetName("tag:kythira:cluster");
        f.AddValues(uuid);
        req.AddFilters(f);
        auto out = ec2->DescribeInstances(req);
        BOOST_REQUIRE_MESSAGE(out.IsSuccess(), "DescribeInstances (live cluster): " +
                                                   std::string(out.GetError().GetMessage()));
        ec2_cluster live;
        for (const auto& r : out.GetResult().GetReservations()) {
            for (const auto& i : r.GetInstances()) {
                auto st = i.GetState().GetName();
                if (st != Aws::EC2::Model::InstanceStateName::pending &&
                    st != Aws::EC2::Model::InstanceStateName::running) {
                    continue;
                }
                auto nid = kythira::node_id_traits<std::uint64_t>::from_text(
                    tag_value(i.GetTags(), "kythira:node-id").value_or(""));
                BOOST_REQUIRE_MESSAGE(nid.has_value(), std::string(i.GetInstanceId())
                                                           << " has no numeric kythira:node-id");
                live.push_back({.node_id = *nid,
                                .group_id = tag_value(i.GetTags(), "kythira:group").value_or("")});
            }
        }
        return live;
    }

    void terminate_cluster_instances() {
        Aws::EC2::Model::DescribeInstancesRequest req;
        Aws::EC2::Model::Filter f;
        f.SetName("tag:kythira:cluster");
        f.AddValues(uuid);
        req.AddFilters(f);
        auto out = ec2->DescribeInstances(req);
        if (!out.IsSuccess()) {
            return;
        }
        Aws::EC2::Model::TerminateInstancesRequest term;
        for (const auto& r : out.GetResult().GetReservations()) {
            for (const auto& i : r.GetInstances()) {
                if (i.GetState().GetName() != Aws::EC2::Model::InstanceStateName::terminated) {
                    term.AddInstanceIds(i.GetInstanceId());
                }
            }
        }
        if (!term.GetInstanceIds().empty()) {
            ec2->TerminateInstances(term);
        }
    }
};

// ── AsgLocalstackFixture ──────────────────────────────────────────────────────

struct AsgLocalstackFixture : Ec2LocalstackFixture {
    std::string asg_name;
    std::string launch_cfg_name;
    std::shared_ptr<Aws::AutoScaling::AutoScalingClient> asg_client;

    AsgLocalstackFixture() {
        asg_client =
            std::make_shared<Aws::AutoScaling::AutoScalingClient>(dummy_creds(), make_client_cfg());

        launch_cfg_name = uuid + "-lc";
        Aws::AutoScaling::Model::CreateLaunchConfigurationRequest lc_req;
        lc_req.SetLaunchConfigurationName(launch_cfg_name);
        lc_req.SetImageId("ami-12345678");
        lc_req.SetInstanceType("t3.micro");
        auto lc_out = asg_client->CreateLaunchConfiguration(lc_req);
        BOOST_REQUIRE_MESSAGE(lc_out.IsSuccess(), "CreateLaunchConfiguration: " +
                                                      std::string(lc_out.GetError().GetMessage()));

        asg_name = uuid + "-asg";
        Aws::AutoScaling::Model::CreateAutoScalingGroupRequest asg_req;
        asg_req.SetAutoScalingGroupName(asg_name);
        asg_req.SetLaunchConfigurationName(launch_cfg_name);
        asg_req.SetMinSize(0);
        asg_req.SetMaxSize(9);
        asg_req.SetDesiredCapacity(0);
        asg_req.SetHealthCheckType("EC2");
        asg_req.SetVPCZoneIdentifier(subnet_id);
        auto asg_out = asg_client->CreateAutoScalingGroup(asg_req);
        BOOST_REQUIRE_MESSAGE(
            asg_out.IsSuccess(),
            "CreateAutoScalingGroup: " + std::string(asg_out.GetError().GetMessage()));
    }

    ~AsgLocalstackFixture() {
        if (!asg_name.empty()) {
            Aws::AutoScaling::Model::DeleteAutoScalingGroupRequest del;
            del.SetAutoScalingGroupName(asg_name);
            del.SetForceDelete(true);
            asg_client->DeleteAutoScalingGroup(del);
        }
        if (!launch_cfg_name.empty()) {
            Aws::AutoScaling::Model::DeleteLaunchConfigurationRequest del;
            del.SetLaunchConfigurationName(launch_cfg_name);
            asg_client->DeleteLaunchConfiguration(del);
        }
    }

    AsgLocalstackFixture(const AsgLocalstackFixture&) = delete;
    AsgLocalstackFixture& operator=(const AsgLocalstackFixture&) = delete;
    AsgLocalstackFixture(AsgLocalstackFixture&&) = delete;
    AsgLocalstackFixture& operator=(AsgLocalstackFixture&&) = delete;

    [[nodiscard]] auto asg_cfg() const -> kythira::aws_asg_quorum_manager_config {
        kythira::aws_asg_quorum_manager_config cfg;
        cfg.cluster_name = uuid;
        cfg.node_port = 7000;
        cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
        cfg.asg_by_group["AZ1"] = asg_name;
        cfg.provision_timeout = std::chrono::seconds{60};
        cfg.poll_interval = std::chrono::seconds{2};
        cfg.aws = make_localstack_cfg();
        return cfg;
    }

    auto desired_capacity() -> int {
        Aws::AutoScaling::Model::DescribeAutoScalingGroupsRequest req;
        req.AddAutoScalingGroupNames(asg_name);
        auto out = asg_client->DescribeAutoScalingGroups(req);
        BOOST_REQUIRE_MESSAGE(out.IsSuccess() && !out.GetResult().GetAutoScalingGroups().empty(),
                              "DescribeAutoScalingGroups " + asg_name);
        return out.GetResult().GetAutoScalingGroups()[0].GetDesiredCapacity();
    }
};

// Skips the ASG suite, with the probe's reason, where LocalStack has no
// autoscaling service. Boost reports the cases as skipped rather than passed.
struct autoscaling_available {
    auto operator()(boost::unit_test::test_unit_id) -> boost::test_tools::assertion_result {
        boost::test_tools::assertion_result r(g_autoscaling_available);
        r.message() << g_autoscaling_unavailable_reason;
        return r;
    }
};

// ── EC2 manager tests (Requirement 16.13) ─────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(ec2_localstack, Ec2LocalstackFixture)

// 16.13a: every node carries this cluster's tags, including the spot market
// tag the shared spot configuration must produce.
BOOST_AUTO_TEST_CASE(ec2_provision_three_nodes) {
    ec2_manager mgr{ec2_cfg()};
    auto cluster = provision(mgr, 3);
    BOOST_REQUIRE_EQUAL(cluster.size(), 3u);

    for (const auto& np : cluster) {
        auto inst = describe(instance_id_for(np.node_id));
        BOOST_REQUIRE(inst.has_value());
        BOOST_CHECK_EQUAL(tag_value(inst->GetTags(), "kythira:cluster").value_or(""), uuid);
        BOOST_CHECK_EQUAL(tag_value(inst->GetTags(), "kythira:group").value_or(""), "AZ1");
        BOOST_CHECK_EQUAL(tag_value(inst->GetTags(), "kythira:node-id").value_or(""),
                          std::to_string(np.node_id));
        BOOST_CHECK_EQUAL(tag_value(inst->GetTags(), "kythira:market").value_or(""), "spot");
    }

    auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 3u);
    BOOST_CHECK_EQUAL(health.status, kythira::quorum_status::healthy);
}

// 16.13b: a stopped instance is unreachable.
//
// The spec says 2 of 3 live is `degraded`, but its own status definition
// (and the manager's compute_status) says live == majority is `critical`:
// one more loss and quorum is gone. 5 of 9 is `critical` in 16.19m for the
// same reason, so this asserts the definition rather than the stray word.
BOOST_AUTO_TEST_CASE(ec2_assess_detects_stopped_node) {
    ec2_manager mgr{ec2_cfg()};
    auto cluster = provision(mgr, 3);
    stop(cluster[1].node_id);

    auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 2u);
    BOOST_CHECK_EQUAL(health.status, kythira::quorum_status::critical);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1u);
    BOOST_CHECK_EQUAL(health.unreachable_nodes[0], cluster[1].node_id);
}

// 16.13c: decommission terminates, as EC2 reports it, not merely "did not throw".
BOOST_AUTO_TEST_CASE(ec2_decommission_all_nodes) {
    ec2_manager mgr{ec2_cfg()};
    auto cluster = provision(mgr, 3);
    for (const auto& np : cluster) {
        BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(np.node_id)).get());
    }
    for (const auto& np : cluster) {
        BOOST_CHECK_MESSAGE(wait_state(np.node_id, Aws::EC2::Model::InstanceStateName::terminated),
                            instance_id_for(np.node_id) << " never reached `terminated`");
    }
    auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 0u);
    BOOST_CHECK_EQUAL(health.status, kythira::quorum_status::lost);
}

// 16.13d: a second decommission of the same node resolves.
BOOST_AUTO_TEST_CASE(ec2_decommission_idempotent) {
    ec2_manager mgr{ec2_cfg()};
    auto cluster = provision(mgr, 1);
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(cluster[0].node_id)).get());
    BOOST_CHECK(wait_state(cluster[0].node_id, Aws::EC2::Model::InstanceStateName::terminated));
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(cluster[0].node_id)).get());
}

// Requirement 19.2: maintain_quorum replaces an unreachable node in its own
// group. The one tier where this runs offline: it returns the pre-remediation
// health and none of what it provisioned, so the replacement is read back
// from EC2.
BOOST_AUTO_TEST_CASE(ec2_maintain_quorum_replaces_stopped_node) {
    ec2_manager mgr{ec2_cfg(3)};
    auto cluster = provision(mgr, 3);
    const auto lost = cluster[2].node_id;
    stop(lost);

    auto returned = std::move(mgr.maintain_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(returned.live_node_count, 2u);
    BOOST_REQUIRE_EQUAL(returned.unreachable_nodes.size(), 1u);
    BOOST_CHECK_EQUAL(returned.unreachable_nodes[0], lost);

    // The stopped node was decommissioned, not just left stopped.
    BOOST_CHECK(wait_state(lost, Aws::EC2::Model::InstanceStateName::terminated));

    auto after = live_cluster();
    BOOST_CHECK_EQUAL(after.size(), 3u);
    std::size_t replacements = 0;
    for (const auto& np : after) {
        BOOST_CHECK_EQUAL(np.group_id, "AZ1");
        BOOST_CHECK_NE(np.node_id, lost);
        if (std::ranges::none_of(cluster, [&](const auto& m) { return m.node_id == np.node_id; })) {
            ++replacements;
        }
    }
    BOOST_CHECK_EQUAL(replacements, 1u);

    auto health = std::move(mgr.assess_quorum(after)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 3u);
    BOOST_CHECK_EQUAL(health.status, kythira::quorum_status::healthy);
}

// Requirement 19: a cluster already at target is left alone. Guards the
// replacement case above against passing because maintain_quorum provisions
// unconditionally.
BOOST_AUTO_TEST_CASE(ec2_maintain_quorum_leaves_healthy_cluster_alone) {
    ec2_manager mgr{ec2_cfg(3)};
    auto cluster = provision(mgr, 3);

    auto returned = std::move(mgr.maintain_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(returned.status, kythira::quorum_status::healthy);
    BOOST_CHECK_EQUAL(live_cluster().size(), 3u);
}

// elastic-shard-capacity Requirement 8.2: the key rides in RunInstances'
// TagSpecifications, and DescribeInstances' tag filters find it again. The
// subject is filter syntax and tag round-tripping, which is what LocalStack
// models faithfully.
BOOST_AUTO_TEST_CASE(keyed_provision_is_found_by_its_key) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = uuid;
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 1});
    cfg.subnet_by_group["AZ1"] = subnet_id;
    cfg.security_group_ids.push_back(sg_id);
    cfg.provision_timeout = std::chrono::seconds{60};
    cfg.poll_interval = std::chrono::seconds{2};
    cfg.aws = make_localstack_cfg();

    kythira::aws_ec2_quorum_manager<> mgr{cfg};

    const std::string key = "cap-1-" + uuid;
    auto peer = mgr.provision_node_keyed("AZ1", std::nullopt, key).get();

    auto found = mgr.find_by_idempotency_key(key).get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(found->node_id, peer.node_id);
    BOOST_CHECK_EQUAL(found->address, peer.address);

    BOOST_CHECK(!mgr.find_by_idempotency_key(key + "-other").get().has_value());

    // Terminated no longer counts as holding the key.
    mgr.decommission_node(peer.node_id).get();
    BOOST_CHECK(!mgr.find_by_idempotency_key(key).get().has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ── ASG manager tests (Requirement 16.14) ─────────────────────────────────────
//
// Needs LocalStack's autoscaling service, which only the Pro edition has, so
// these skip on the community image. The real-AWS suite
// (aws_asg_quorum_manager_real_test.cpp) covers the same behaviour against
// Auto Scaling itself.

BOOST_FIXTURE_TEST_SUITE(asg_localstack, AsgLocalstackFixture,
                         *boost::unit_test::precondition(autoscaling_available{}))

BOOST_AUTO_TEST_CASE(asg_provision_increments_desired_capacity) {
    kythira::aws_asg_quorum_manager<> mgr{asg_cfg()};
    const int before = desired_capacity();
    auto peer = std::move(mgr.provision_node("AZ1", std::nullopt)).get();
    BOOST_CHECK_NE(peer.node_id, 0u);
    BOOST_CHECK_EQUAL(desired_capacity(), before + 1);
}

BOOST_AUTO_TEST_CASE(asg_assess_detects_not_inservice) {
    kythira::aws_asg_quorum_manager<> mgr{asg_cfg()};
    auto peer = std::move(mgr.provision_node("AZ1", std::nullopt)).get();
    ec2_cluster cluster{{.node_id = peer.node_id, .group_id = "AZ1"}};
    BOOST_CHECK_EQUAL(std::move(mgr.assess_quorum(cluster)).get().live_node_count, 1u);

    stop(peer.node_id);
    auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 0u);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1u);
    BOOST_CHECK_EQUAL(health.unreachable_nodes[0], peer.node_id);
}

BOOST_AUTO_TEST_CASE(asg_decommission_decrements_desired_capacity) {
    kythira::aws_asg_quorum_manager<> mgr{asg_cfg()};
    auto peer = std::move(mgr.provision_node("AZ1", std::nullopt)).get();
    const int before = desired_capacity();
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(peer.node_id)).get());
    BOOST_CHECK_EQUAL(desired_capacity(), before - 1);
}

BOOST_AUTO_TEST_CASE(asg_decommission_idempotent) {
    kythira::aws_asg_quorum_manager<> mgr{asg_cfg()};
    auto peer = std::move(mgr.provision_node("AZ1", std::nullopt)).get();
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(peer.node_id)).get());
    const int after_first = desired_capacity();
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(peer.node_id)).get());
    BOOST_CHECK_EQUAL(desired_capacity(), after_first);
}

BOOST_AUTO_TEST_CASE(assess_empty_asg_is_healthy_with_empty_cluster) {
    kythira::aws_asg_quorum_manager<> mgr{asg_cfg()};
    ec2_cluster empty_cluster;
    auto health = std::move(mgr.assess_quorum(empty_cluster)).get();
    BOOST_CHECK_EQUAL(health.status, kythira::quorum_status::healthy);
    BOOST_CHECK_EQUAL(health.live_node_count, 0u);
}

BOOST_AUTO_TEST_SUITE_END()

}  // namespace

#endif  // KYTHIRA_HAS_AWS_SDK
#endif  // KYTHIRA_AWS_LOCALSTACK_TESTS
