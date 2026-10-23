// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Real-EC2 coverage for leader-driven liveness (aws-quorum-manager task 8,
// Requirements 19.7 and 16.19(j)-(k)). aws_ec2_quorum_manager decides
// liveness from instance state alone: a kythira process that has died on an
// instance that is still `running` looks perfectly healthy to it. Catching
// that case is the Raft leader's job (quorum_peer_dead_after, Req 13.7):
// once a voter has been silent that long the leader provisions a
// replacement, admits it through ClusterJoin as a learner, promotes it,
// removes the dead voter and terminates its instance. With one voter per
// AZ, the silent voter's group has no live member left, which the leader
// cannot tell apart from losing the AZ, so heal_lost_groups() places the
// replacement in a surviving group (Req 9.2), not the voter's own.
// quorum_leader_integration_test.cpp's crashed_process_on_running_vm_is_
// replaced models this in process; this test runs it on real instances:
//
//   1. launch three instances, one per AZ, through this test's own
//      aws_ec2_quorum_manager<std::uint64_t> (numeric ids 1..3, kythira:*
//      tags under the same cluster name the nodes are given);
//   2. once cloud-init has finished on all three, start chaos_node on each
//      over SSH with QUORUM_MANAGER=aws and QUORUM_PEER_DEAD_AFTER_MS=30000,
//      so whichever node wins the election owns a manager that can launch
//      and terminate instances;
//   3. SIGKILL chaos_node on one follower; its instance keeps running;
//   4. check that the test's manager still reports all three nodes live:
//      instance state alone does not see the crash;
//   5. wait for the leader to act: a new cluster-tagged instance in one of
//      the other two subnets reaches `running`, the killed instance is
//      terminated, the leader still answers /status, and the replacement's
//      chaos_node (started by the user data the leader's manager rendered)
//      catches up to the leader's commit index.
//
// The nodes are started detached (setsid nohup) and never under systemd, so
// no Restart= policy can undo the kill.
//
// Requires (all via environment variables):
//   KYTHIRA_EC2_TEST_AMI               AMI with /usr/local/bin/chaos_node built
//                                      with the AWS SDK and
//                                      -DKYTHIRA_NODE_ID_KIND=numeric (the
//                                      ca_cluster_node Packer AMI, Ubuntu
//                                      24.04, SSH user `ubuntu`)
//   KYTHIRA_TEST_IAM_INSTANCE_PROFILE  instance profile for every node. The
//                                      leader calls EC2 from its instance, so
//                                      the role needs ec2:RunInstances,
//                                      ec2:DescribeInstances,
//                                      ec2:DescribeInstanceStatus,
//                                      ec2:TerminateInstances, ec2:CreateTags
//                                      and iam:PassRole on itself (the
//                                      replacement is launched with the same
//                                      profile):
//                                      kythira-aws-chaos-node-profile, from
//                                      provision-quorum-test-node-role.sh
//                                      --leader. The plain node profile
//                                      grants none of these.
//   AWS credentials via the standard provider chain; AWS_REGION or a default
//   region configured in aws_client_config
//
// Not run by default (LABELS real-ec2;slow). Real per-run AWS cost: four
// t3.micro/t4g.micro instances (three bootstrap nodes plus the leader's
// replacement) for roughly fifteen minutes, well under US$0.05.

#define BOOST_TEST_MODULE aws_chaos_node_crash_real_ec2_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_AWS_REAL_EC2_TESTS
#ifdef KYTHIRA_HAS_AWS_SDK
#ifdef LIBSSH2_FOUND

#include <raft/aws_ec2_quorum_manager.hpp>

#include <aws/core/Aws.h>
#include <aws/core/utils/UUID.h>
#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/AssociateRouteTableRequest.h>
#include <aws/ec2/model/AttachInternetGatewayRequest.h>
#include <aws/ec2/model/AuthorizeSecurityGroupIngressRequest.h>
#include <aws/ec2/model/CreateInternetGatewayRequest.h>
#include <aws/ec2/model/CreateKeyPairRequest.h>
#include <aws/ec2/model/CreateRouteRequest.h>
#include <aws/ec2/model/CreateRouteTableRequest.h>
#include <aws/ec2/model/CreateSecurityGroupRequest.h>
#include <aws/ec2/model/CreateSubnetRequest.h>
#include <aws/ec2/model/CreateTagsRequest.h>
#include <aws/ec2/model/CreateVpcRequest.h>
#include <aws/ec2/model/DeleteInternetGatewayRequest.h>
#include <aws/ec2/model/DeleteKeyPairRequest.h>
#include <aws/ec2/model/DeleteRouteTableRequest.h>
#include <aws/ec2/model/DeleteSecurityGroupRequest.h>
#include <aws/ec2/model/DeleteSubnetRequest.h>
#include <aws/ec2/model/DeleteVpcRequest.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/DetachInternetGatewayRequest.h>
#include <aws/ec2/model/Filter.h>
#include <aws/ec2/model/IpPermission.h>
#include <aws/ec2/model/IpRange.h>
#include <aws/ec2/model/ModifySubnetAttributeRequest.h>
#include <aws/ec2/model/Tag.h>
#include <aws/ec2/model/TerminateInstancesRequest.h>
#include <aws/sts/STSClient.h>
#include <aws/sts/model/GetCallerIdentityRequest.h>

#include <libssh2.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#include <memory>

#endif
#include "aws_real_ec2_test_support.hpp"
#include "ordered_global_fixtures.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr const char* DUMMY_REGION_FALLBACK = "us-east-1";
constexpr const char* LOG_TAG = "chaos-crash-real-ec2";
constexpr std::uint16_t RPC_PORT = 7000;
constexpr std::uint16_t HTTP_PORT = 8080;

auto env(const char* name) -> std::string {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string{};
}

auto region() -> std::string {
    auto r = env("AWS_REGION");
    return r.empty() ? DUMMY_REGION_FALLBACK : r;
}

// Exits 77 before any AWS work when the environment cannot run this test,
// so ctest reports "Not Run". Must be first in SuiteGlobalFixtures below;
// see ca_cluster_node_real_ec2_test.cpp's PreflightSkipFixture for why it is
// a global fixture and not init_unit_test_suite.
struct PreflightSkipFixture {
    PreflightSkipFixture() {
        if (env("KYTHIRA_EC2_TEST_AMI").empty()) {
            std::cerr << "SKIP: KYTHIRA_EC2_TEST_AMI not set\n";
            std::exit(77);
        }
        if (env("KYTHIRA_TEST_IAM_INSTANCE_PROFILE").empty()) {
            std::cerr << "SKIP: KYTHIRA_TEST_IAM_INSTANCE_PROFILE not set (the leader calls "
                         "EC2 from its instance, so every node needs a profile with EC2 "
                         "launch/describe/terminate/tag rights and iam:PassRole on itself)\n";
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
#else
using FollyInitFixture = kythira::testing::no_global_fixture;
#endif

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

using kythira::testing::aws_real_ec2::AwsSignalHandlerFixture;
using kythira::testing::aws_real_ec2::CostSummaryFixture;

// One registration, constructed in this order and destroyed in reverse (see
// ordered_global_fixtures.hpp).
using SuiteGlobalFixtures =
    kythira::testing::ordered_global_fixtures<PreflightSkipFixture, FollyInitFixture, AwsSdkFixture,
                                              CostSummaryFixture, AwsSignalHandlerFixture>;
BOOST_GLOBAL_FIXTURE(SuiteGlobalFixtures);

// Copied from ca_cluster_node_real_ec2_test.cpp: runs `cmd` on `public_ip`
// as "ubuntu" with the fixture's key pair and returns stdout, retrying the
// TCP connect while a fresh instance's sshd comes up. Failures throw
// std::runtime_error rather than BOOST_REQUIRE, since step 2 runs it on
// worker threads and Boost.Test assertions are not thread-safe.
//
// Every libssh2 call after the connect is bounded by `exec_timeout`. Run
// 39537006735 hung in step 2 for over an hour in a blocking read, until
// ctest killed the process before teardown and leaked the cluster. When
// the read times out, the output so far is returned with *timed_out set
// if the caller passed it, and thrown otherwise.
auto ssh_execute(const std::string& public_ip, const std::string& private_key_pem,
                 const std::string& cmd, std::chrono::seconds connect_timeout,
                 std::chrono::seconds exec_timeout = std::chrono::minutes(5),
                 bool* timed_out = nullptr) -> std::string {
    auto deadline = std::chrono::steady_clock::now() + connect_timeout;
    int sock = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        sock = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(22);
        inet_pton(AF_INET, public_ip.c_str(), &addr.sin_addr);
        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            break;
        }
        ::close(sock);
        sock = -1;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    if (sock < 0) {
        throw std::runtime_error("ssh_execute: could not connect to " + public_ip + ":22 in time");
    }

    LIBSSH2_SESSION* session = libssh2_session_init();
    auto fail = [&](const std::string& what) {
        if (session != nullptr) {
            libssh2_session_free(session);
        }
        ::close(sock);
        throw std::runtime_error("ssh_execute: " + what + " on " + public_ip);
    };
    if (session == nullptr) {
        fail("libssh2_session_init failed");
    }
    libssh2_session_set_timeout(
        session, static_cast<long>(
                     std::chrono::duration_cast<std::chrono::milliseconds>(exec_timeout).count()));
    if (libssh2_session_handshake(session, sock) < 0) {
        fail("SSH handshake failed");
    }

    int rc = libssh2_userauth_publickey_frommemory(
        session, "ubuntu", 6, nullptr, 0, private_key_pem.data(), private_key_pem.size(), nullptr);
    if (rc != 0) {
        fail("SSH auth failed, rc=" + std::to_string(rc));
    }

    LIBSSH2_CHANNEL* channel = libssh2_channel_open_session(session);
    if (channel == nullptr) {
        fail("could not open an SSH channel");
    }
    if (libssh2_channel_exec(channel, cmd.c_str()) < 0) {
        libssh2_channel_free(channel);
        fail("could not exec the command");
    }

    std::string output;
    char buf[4096];
    ssize_t nread = 0;
    while ((nread = libssh2_channel_read(channel, buf, sizeof(buf))) > 0) {
        output.append(buf, static_cast<std::size_t>(nread));
    }
    libssh2_channel_free(channel);
    libssh2_session_disconnect(session, "bye");
    libssh2_session_free(session);
    ::close(sock);
    if (nread == LIBSSH2_ERROR_TIMEOUT) {
        if (timed_out != nullptr) {
            *timed_out = true;
            return output;
        }
        throw std::runtime_error("ssh_execute: command on " + public_ip + " still running after " +
                                 std::to_string(exec_timeout.count()) +
                                 "s; output so far: " + output);
    }
    return output;
}

auto make_tag(const std::string& k, const std::string& v) -> Aws::EC2::Model::Tag {
    Aws::EC2::Model::Tag t;
    t.SetKey(k);
    t.SetValue(v);
    return t;
}

auto make_filter(const std::string& name, const std::vector<std::string>& values)
    -> Aws::EC2::Model::Filter {
    Aws::EC2::Model::Filter f;
    f.SetName(name);
    for (const auto& v : values) {
        f.AddValues(v);
    }
    return f;
}

// One instance as DescribeInstances reports it.
struct instance_view {
    std::string instance_id;
    std::string subnet_id;
    std::string state;
    std::string public_ip;
    std::string private_ip;
    // StateTransitionReason, then StateReason's message: "User initiated"
    // means a TerminateInstances call, "Client.InstanceInitiatedShutdown"
    // the guest itself.
    std::string state_reason;
};

auto describe(Aws::EC2::EC2Client& ec2, const std::vector<Aws::EC2::Model::Filter>& filters)
    -> std::optional<std::vector<instance_view>> {
    Aws::EC2::Model::DescribeInstancesRequest req;
    for (const auto& f : filters) {
        req.AddFilters(f);
    }
    std::vector<instance_view> out;
    for (;;) {
        auto res = ec2.DescribeInstances(req);
        if (!res.IsSuccess()) {
            std::cerr << "[" << LOG_TAG
                      << "] DescribeInstances failed: " << res.GetError().GetExceptionName() << ": "
                      << res.GetError().GetMessage() << "\n";
            return std::nullopt;
        }
        for (const auto& r : res.GetResult().GetReservations()) {
            for (const auto& inst : r.GetInstances()) {
                out.push_back({
                    .instance_id = std::string(inst.GetInstanceId()),
                    .subnet_id = std::string(inst.GetSubnetId()),
                    .state = std::string(
                        Aws::EC2::Model::InstanceStateNameMapper::GetNameForInstanceStateName(
                            inst.GetState().GetName())),
                    .public_ip = std::string(inst.GetPublicIpAddress()),
                    .private_ip = std::string(inst.GetPrivateIpAddress()),
                    .state_reason = std::string(inst.GetStateTransitionReason()) + " / " +
                                    std::string(inst.GetStateReason().GetMessage()),
                });
            }
        }
        const auto& token = res.GetResult().GetNextToken();
        if (token.empty()) {
            break;
        }
        req.SetNextToken(token);
    }
    return out;
}

// Network shell for the run, modelled on ca_cluster_node_real_ec2_test.cpp's
// three_az_network_fixture: a VPC with one public subnet per AZ, an IGW, a
// security group open on SSH and the Raft port, and an in-memory key pair.
//
// Teardown differs in one way that matters here: a chaos_node leader on one
// of the instances owns a quorum manager and may launch a replacement at any
// moment until its own instance is gone, so a single terminate pass can miss
// an instance launched just after it. teardown() terminates every
// non-terminated instance in the VPC or carrying the cluster tag, repeatedly,
// until none is left.
struct chaos_crash_network_fixture : kythira::testing::aws_real_ec2::signal_cleanup_target {
    std::shared_ptr<Aws::EC2::EC2Client> ec2;
    std::string vpc_id, igw_id, route_table_id, sg_id, key_name;
    std::string uuid;
    std::string cluster_name;
    std::string private_key_pem;
    std::map<std::string, std::string> subnet_by_az;
    kythira::testing::aws_real_ec2::TestCostReport cost_report{
        std::string(boost::unit_test::framework::current_test_case().p_name)};
    bool torn_down_ = false;

    // A Boost timeout siglongjmps out of the case without running this
    // fixture's destructor, and a ctest TIMEOUT kills with SIGKILL, so
    // neither tears anything down: run 39537006735 hung, hit both at
    // 3900 s and leaked its VPC and three instances. The watchdog fires
    // first, at the case's budget less teardown's 13 minutes, tears down
    // from its own thread and exits the process.
    static constexpr std::chrono::seconds WATCHDOG_AFTER{3900 - 13 * 60};
    std::mutex watchdog_mu_;
    std::condition_variable watchdog_cv_;
    bool watchdog_stop_ = false;
    std::thread watchdog_;

    void start_watchdog() {
        watchdog_ = std::thread([this] {
            std::unique_lock lk(watchdog_mu_);
            if (watchdog_cv_.wait_for(lk, WATCHDOG_AFTER, [this] { return watchdog_stop_; })) {
                return;
            }
            lk.unlock();
            std::cerr << "[" << LOG_TAG << "] watchdog: still running after "
                      << WATCHDOG_AFTER.count()
                      << "s; tearing down before the Boost and ctest timeouts\n"
                      << std::flush;
            if (auto* f = kythira::testing::aws_real_ec2::g_active_aws_fixture.exchange(
                    nullptr, std::memory_order_acq_rel)) {
                f->teardown();
            }
            std::_Exit(EXIT_FAILURE);
        });
    }

    void track_instance(const std::string& label, const std::string& instance_type) {
        cost_report.resources.push_back(
            {label, kythira::testing::aws_real_ec2::ec2_hourly_rate(instance_type)});
    }

    void tag(const std::string& resource_id, const std::string& k, const std::string& v) {
        Aws::EC2::Model::CreateTagsRequest req;
        req.AddResources(resource_id);
        req.AddTags(make_tag(k, v));
        ec2->CreateTags(req);
    }

    chaos_crash_network_fixture() {
        Aws::Client::ClientConfiguration cli_cfg;
        cli_cfg.region = region();
        ec2 = std::make_shared<Aws::EC2::EC2Client>(cli_cfg);

        Aws::STS::STSClient sts{cli_cfg};
        auto id_out = sts.GetCallerIdentity(Aws::STS::Model::GetCallerIdentityRequest{});
        if (!id_out.IsSuccess()) {
            throw std::runtime_error("AWS not reachable / no credentials (skip): " +
                                     std::string(id_out.GetError().GetMessage()));
        }

        const Aws::String random_uuid = Aws::Utils::UUID::RandomUUID();
        uuid = "kyt-" + std::string(random_uuid);
        // Unique per run, so the leader's id allocation and the teardown
        // sweep only ever see this run's instances.
        cluster_name = "chaos-crash-" + std::string(random_uuid).substr(0, 8);

        kythira::testing::aws_real_ec2::g_active_aws_fixture.store(this, std::memory_order_release);

        // Same reasoning as three_az_network_fixture: a failure partway
        // through never runs the destructor, so tear down here and rethrow.
        try {
            Aws::EC2::Model::CreateVpcRequest vpc_req;
            vpc_req.SetCidrBlock("10.222.0.0/16");
            kythira::testing::aws_real_ec2::tag_vpc_with_github_run(vpc_req);
            auto vpc_out = ec2->CreateVpc(vpc_req);
            BOOST_REQUIRE_MESSAGE(vpc_out.IsSuccess(),
                                  "CreateVpc: " + std::string(vpc_out.GetError().GetMessage()));
            vpc_id = std::string(vpc_out.GetResult().GetVpc().GetVpcId());
            tag(vpc_id, "Name", uuid + "-vpc");
            tag(vpc_id, "kythira:managed-by", "aws_chaos_node_crash_real_ec2_test");

            auto igw_out = ec2->CreateInternetGateway({});
            BOOST_REQUIRE(igw_out.IsSuccess());
            igw_id = std::string(igw_out.GetResult().GetInternetGateway().GetInternetGatewayId());
            tag(igw_id, "Name", uuid + "-igw");
            Aws::EC2::Model::AttachInternetGatewayRequest attach_req;
            attach_req.SetVpcId(vpc_id);
            attach_req.SetInternetGatewayId(igw_id);
            ec2->AttachInternetGateway(attach_req);

            Aws::EC2::Model::CreateRouteTableRequest rt_req;
            rt_req.SetVpcId(vpc_id);
            auto rt_out = ec2->CreateRouteTable(rt_req);
            BOOST_REQUIRE(rt_out.IsSuccess());
            route_table_id = std::string(rt_out.GetResult().GetRouteTable().GetRouteTableId());
            tag(route_table_id, "Name", uuid + "-rtb");
            Aws::EC2::Model::CreateRouteRequest route_req;
            route_req.SetRouteTableId(route_table_id);
            route_req.SetDestinationCidrBlock("0.0.0.0/0");
            route_req.SetGatewayId(igw_id);
            ec2->CreateRoute(route_req);

            int octet = 1;
            for (const std::string& az : {region() + "a", region() + "b", region() + "c"}) {
                Aws::EC2::Model::CreateSubnetRequest sn_req;
                sn_req.SetVpcId(vpc_id);
                sn_req.SetCidrBlock("10.222." + std::to_string(octet++) + ".0/24");
                sn_req.SetAvailabilityZone(az);
                auto sn_out = ec2->CreateSubnet(sn_req);
                BOOST_REQUIRE_MESSAGE(sn_out.IsSuccess(), "CreateSubnet(" + az + ")");
                std::string subnet_id = std::string(sn_out.GetResult().GetSubnet().GetSubnetId());
                subnet_by_az[az] = subnet_id;
                tag(subnet_id, "Name", uuid + "-" + az);

                // The leader's replacement lands here too, and the test
                // reaches it over SSH, so every launch needs a public IP
                // without the manager asking for one.
                Aws::EC2::Model::ModifySubnetAttributeRequest map_public;
                map_public.SetSubnetId(subnet_id);
                Aws::EC2::Model::AttributeBooleanValue v;
                v.SetValue(true);
                map_public.SetMapPublicIpOnLaunch(v);
                ec2->ModifySubnetAttribute(map_public);

                Aws::EC2::Model::AssociateRouteTableRequest assoc_req;
                assoc_req.SetRouteTableId(route_table_id);
                assoc_req.SetSubnetId(subnet_id);
                ec2->AssociateRouteTable(assoc_req);
            }

            Aws::EC2::Model::CreateSecurityGroupRequest sg_req;
            sg_req.SetGroupName("kythira-chaos-crash-real-ec2-test-sg");
            sg_req.SetDescription("kythira chaos_node crash real-EC2 test");
            sg_req.SetVpcId(vpc_id);
            auto sg_out = ec2->CreateSecurityGroup(sg_req);
            BOOST_REQUIRE(sg_out.IsSuccess());
            sg_id = std::string(sg_out.GetResult().GetGroupId());
            tag(sg_id, "Name", uuid + "-sg");

            // SSH from the runner and Raft RPC between nodes. HTTP (8080)
            // is only ever read over SSH from localhost, so it stays closed.
            // Egress is the default allow-all, which the leader needs to
            // reach the EC2 API through the IGW.
            for (int port : {22, static_cast<int>(RPC_PORT)}) {
                Aws::EC2::Model::AuthorizeSecurityGroupIngressRequest ing_req;
                ing_req.SetGroupId(sg_id);
                Aws::EC2::Model::IpPermission perm;
                perm.SetIpProtocol("tcp");
                perm.SetFromPort(port);
                perm.SetToPort(port);
                Aws::EC2::Model::IpRange range;
                range.SetCidrIp("0.0.0.0/0");  // test-only; scope down for anything longer-lived
                perm.AddIpRanges(range);
                ing_req.AddIpPermissions(perm);
                ec2->AuthorizeSecurityGroupIngress(ing_req);
            }

            key_name = uuid + "-key";
            Aws::EC2::Model::CreateKeyPairRequest kp_req;
            kp_req.SetKeyName(key_name);
            auto kp_out = ec2->CreateKeyPair(kp_req);
            BOOST_REQUIRE_MESSAGE(kp_out.IsSuccess(), "CreateKeyPair");
            private_key_pem = std::string(kp_out.GetResult().GetKeyMaterial());
        } catch (...) {
            teardown();
            throw;
        }
        // Only once nothing can throw: a constructor that throws never
        // runs the destructor that joins this thread.
        start_watchdog();
    }

    ~chaos_crash_network_fixture() {
        {
            std::lock_guard lk(watchdog_mu_);
            watchdog_stop_ = true;
        }
        watchdog_cv_.notify_all();
        if (watchdog_.joinable()) {
            watchdog_.join();
        }
        teardown();
    }

    // Terminates every instance in the VPC or tagged with this run's
    // cluster that is not already terminated or shutting down, and repeats
    // until a pass finds none. Returns false when `budget` runs out.
    auto terminate_until_gone(std::chrono::seconds budget) noexcept -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        const std::vector<std::string> live_states{"pending", "running", "stopping", "stopped"};
        for (;;) {
            std::set<std::string> ids;
            bool listed = true;
            if (!vpc_id.empty()) {
                auto v = describe(*ec2, {make_filter("vpc-id", {vpc_id}),
                                         make_filter("instance-state-name", live_states)});
                listed = listed && v.has_value();
                for (const auto& i : v.value_or(std::vector<instance_view>{})) {
                    ids.insert(i.instance_id);
                }
            }
            if (!cluster_name.empty()) {
                auto c = describe(*ec2, {make_filter("tag:kythira:cluster", {cluster_name}),
                                         make_filter("instance-state-name", live_states)});
                listed = listed && c.has_value();
                for (const auto& i : c.value_or(std::vector<instance_view>{})) {
                    ids.insert(i.instance_id);
                }
            }
            if (listed && ids.empty()) {
                return true;
            }
            if (!ids.empty()) {
                Aws::EC2::Model::TerminateInstancesRequest term;
                for (const auto& id : ids) {
                    term.AddInstanceIds(id);
                }
                ec2->TerminateInstances(term);
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::cerr << "[" << LOG_TAG << "] teardown: instances still alive after "
                          << budget.count() << "s:";
                for (const auto& id : ids) {
                    std::cerr << " " << id;
                }
                std::cerr << "\n";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::seconds{10});
        }
    }

    void teardown() noexcept override {
        if (torn_down_) {
            return;
        }
        torn_down_ = true;
        kythira::testing::aws_real_ec2::g_active_aws_fixture.store(nullptr,
                                                                   std::memory_order_release);

        // The bootstrap nodes and the leader's replacement alike. The sweep
        // runs before the VPC wait so a replacement launched by a leader
        // that was still alive during the first pass is caught by a later
        // one.
        (void)terminate_until_gone(std::chrono::seconds{300});
        if (!vpc_id.empty()) {
            (void)kythira::testing::aws_real_ec2::wait_vpc_instances_terminated(
                *ec2, vpc_id, std::chrono::seconds{180}, LOG_TAG);
        }

        if (!key_name.empty()) {
            Aws::EC2::Model::DeleteKeyPairRequest req;
            req.SetKeyName(key_name);
            ec2->DeleteKeyPair(req);
        }
        namespace support = kythira::testing::aws_real_ec2;
        std::vector<support::teardown_delete> steps;
        if (!sg_id.empty()) {
            steps.push_back({"security group " + sg_id, [this] {
                                 Aws::EC2::Model::DeleteSecurityGroupRequest req;
                                 req.SetGroupId(sg_id);
                                 return support::delete_outcome_error(
                                     ec2->DeleteSecurityGroup(req));
                             }});
        }
        for (const auto& [az, subnet_id] : subnet_by_az) {
            (void)az;
            steps.push_back({"subnet " + subnet_id, [this, id = subnet_id] {
                                 Aws::EC2::Model::DeleteSubnetRequest req;
                                 req.SetSubnetId(id);
                                 return support::delete_outcome_error(ec2->DeleteSubnet(req));
                             }});
        }
        if (!route_table_id.empty()) {
            steps.push_back({"route table " + route_table_id, [this] {
                                 Aws::EC2::Model::DeleteRouteTableRequest req;
                                 req.SetRouteTableId(route_table_id);
                                 return support::delete_outcome_error(ec2->DeleteRouteTable(req));
                             }});
        }
        if (!igw_id.empty()) {
            steps.push_back({"internet gateway attachment " + igw_id, [this] {
                                 Aws::EC2::Model::DetachInternetGatewayRequest req;
                                 req.SetVpcId(vpc_id);
                                 req.SetInternetGatewayId(igw_id);
                                 return support::delete_outcome_error(
                                     ec2->DetachInternetGateway(req), {"Gateway.NotAttached"});
                             }});
            steps.push_back({"internet gateway " + igw_id, [this] {
                                 Aws::EC2::Model::DeleteInternetGatewayRequest req;
                                 req.SetInternetGatewayId(igw_id);
                                 return support::delete_outcome_error(
                                     ec2->DeleteInternetGateway(req));
                             }});
        }
        if (!vpc_id.empty()) {
            steps.push_back({"VPC " + vpc_id, [this] {
                                 Aws::EC2::Model::DeleteVpcRequest req;
                                 req.SetVpcId(vpc_id);
                                 return support::delete_outcome_error(ec2->DeleteVpc(req));
                             }});
        }
        (void)support::run_teardown_deletes(std::move(steps), std::chrono::minutes{5},
                                            std::chrono::seconds{15}, LOG_TAG);

        for (auto& r : cost_report.resources) {
            r.finalize();
        }
        BOOST_TEST_MESSAGE(cost_report.format());
        kythira::testing::aws_real_ec2::g_cost_accumulator.add(std::move(cost_report));
    }

    auto instance(const std::string& instance_id) -> std::optional<instance_view> {
        auto v = describe(*ec2, {make_filter("instance-id", {instance_id})});
        if (!v || v->empty()) {
            return std::nullopt;
        }
        return v->front();
    }
};

// The node's GET /status, read over SSH from localhost (the security group
// keeps the HTTP port closed). nullopt when curl or the node did not answer.
struct node_status {
    std::string role;
    std::uint64_t commit_index = 0;
    std::string raw;
};

// Value of "key": in chaos_node's flat /status object: the quoted string or
// the bare number after the colon.
auto json_field(const std::string& body, const std::string& key) -> std::optional<std::string> {
    auto k = body.find("\"" + key + "\":");
    if (k == std::string::npos) {
        return std::nullopt;
    }
    auto pos = k + key.size() + 3;
    if (pos < body.size() && body[pos] == '"') {
        auto end = body.find('"', pos + 1);
        if (end == std::string::npos) {
            return std::nullopt;
        }
        return body.substr(pos + 1, end - pos - 1);
    }
    auto end = body.find_first_of(",}", pos);
    return body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

auto status_of(const std::string& ip, const std::string& private_key_pem)
    -> std::optional<node_status> {
    std::string out;
    try {
        out = ssh_execute(
            ip, private_key_pem,
            "curl -s --max-time 5 http://localhost:" + std::to_string(HTTP_PORT) + "/status",
            std::chrono::seconds(30));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    auto role = json_field(out, "role");
    auto commit = json_field(out, "commit_index");
    if (!role || !commit) {
        return std::nullopt;
    }
    node_status s{.role = *role, .raw = out};
    try {
        s.commit_index = std::stoull(*commit);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return s;
}

// Index into `ips` of the node reporting role "leader", skipping `skip`.
auto find_leader(const std::vector<std::string>& ips, const std::string& private_key_pem,
                 const std::set<std::size_t>& skip, std::chrono::seconds timeout)
    -> std::optional<std::size_t> {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (std::size_t i = 0; i < ips.size(); ++i) {
            if (skip.contains(i)) {
                continue;
            }
            auto s = status_of(ips[i], private_key_pem);
            if (s && s->role == "leader") {
                return i;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    return std::nullopt;
}

struct node_launch {
    std::uint64_t node_id;
    std::string az;
    std::string private_ip;
};

// The chaos_node environment for one bootstrap member. Everything the
// leader's own aws_ec2_quorum_manager needs comes from here; the
// replacement's environment is rendered from it by chaos_node itself
// (aws_quorum.hpp's replacement_user_data).
auto node_environment(const node_launch& self, const std::vector<node_launch>& all,
                      const chaos_crash_network_fixture& fx, const std::string& ami,
                      const std::string& instance_type, const std::string& profile)
    -> std::vector<std::pair<std::string, std::string>> {
    std::string peers, placement, subnets;
    for (const auto& n : all) {
        placement += (placement.empty() ? "" : ",") + std::to_string(n.node_id) + "=" + n.az;
        if (n.node_id == self.node_id) {
            continue;
        }
        peers += (peers.empty() ? "" : ",") + std::to_string(n.node_id) + ":" + n.private_ip + ":" +
                 std::to_string(RPC_PORT);
    }
    for (const auto& [az, subnet] : fx.subnet_by_az) {
        subnets += (subnets.empty() ? "" : ",") + az + "=" + subnet;
    }
    return {
        {"NODE_ID", std::to_string(self.node_id)},
        {"PEERS", peers},
        {"SELF_ADDRESS", self.private_ip + ":" + std::to_string(RPC_PORT)},
        {"RPC_PORT", std::to_string(RPC_PORT)},
        {"HTTP_PORT", std::to_string(HTTP_PORT)},
        {"DATA_DIR", "/var/lib/chaos_node"},
        // Cross-AZ round trips are a few ms; these leave room for a slow
        // t3.micro without making the election itself slow.
        {"ELECTION_TIMEOUT_MIN_MS", "1000"},
        {"ELECTION_TIMEOUT_MAX_MS", "2000"},
        {"HEARTBEAT_INTERVAL_MS", "200"},
        {"RPC_TIMEOUT_MS", "1000"},
        {"QUORUM_MANAGER", "aws"},
        {"QUORUM_CLUSTER", fx.cluster_name},
        {"QUORUM_AWS_IMAGE", ami},
        {"QUORUM_AWS_SUBNETS", subnets},
        {"QUORUM_PLACEMENT", placement},
        {"QUORUM_TARGET", "1"},
        {"QUORUM_AWS_REGION", region()},
        {"QUORUM_AWS_INSTANCE_TYPE", instance_type},
        {"QUORUM_AWS_SECURITY_GROUPS", fx.sg_id},
        {"QUORUM_AWS_INSTANCE_PROFILE", profile},
        // So the test can SSH to the replacement the leader launches.
        {"QUORUM_AWS_KEY_NAME", fx.key_name},
        {"QUORUM_CHECK_INTERVAL_MS", "5000"},
        {"QUORUM_PEER_DEAD_AFTER_MS", "30000"},
    };
}

// Starts chaos_node as root, detached from the SSH session and outside
// systemd, so nothing restarts it after the kill. The env values carry no
// single quote (ids, addresses, AWS resource ids), so single-quoting them
// is enough. The log goes to /tmp because the redirect is opened by the
// unprivileged SSH shell before sudo runs (see
// ca_cluster_node_real_ec2_test.cpp's start_node_command).
auto start_node_command(const std::vector<std::pair<std::string, std::string>>& environment)
    -> std::string {
    std::ostringstream cmd;
    // Defensive: should the AMI ever ship an enabled chaos_node unit, its
    // Restart= policy would bring the killed process straight back and the
    // test would prove nothing.
    // The date lines come back as the command's output, so the test log
    // shows which part of a slow start was slow (run 39514236538 took about
    // 270 s per node here).
    //
    // The mkdir is its own statement, never `mkdir && sudo env ... &`. A
    // trailing `&` backgrounds the whole AND-list in a forked subshell, and
    // the redirects bind only to its last command, so that subshell keeps
    // the SSH channel's stdout and stderr open for as long as chaos_node
    // runs. sshd then never sends EOF: every start printed its "launched"
    // stamp within a second and then held the channel until the read
    // timeout (run 39563651006). Reproduced against a local sshd, where
    // `true && sleep 1000 >/dev/null 2>&1 </dev/null &` hangs the same way
    // and this command, with the mkdir split out, closes at once.
    cmd << "date +'begin %T.%N'; "
        << "sudo systemctl disable --now chaos_node.service >/dev/null 2>&1; "
        << "date +'systemctl %T.%N'; "
        << "sudo mkdir -p /var/lib/chaos_node || exit 1\n"
        << "sudo env";
    for (const auto& [k, v] : environment) {
        cmd << " " << k << "='" << v << "'";
    }
    cmd << " setsid nohup /usr/local/bin/chaos_node > /tmp/chaos_node.log 2>&1 < /dev/null &\n"
        << "date +'launched %T.%N'\n"
        << "disown\n";
    return cmd.str();
}

// SIGKILL by process name (-x), not -f, which would also match this shell.
// Exits 0 once the process is gone.
auto kill_node_process(const std::string& ip, const std::string& private_key_pem) -> bool {
    auto out = ssh_execute(ip, private_key_pem,
                           "sudo pkill -KILL -x chaos_node; "
                           "for i in $(seq 1 30); do pgrep -x chaos_node >/dev/null || "
                           "{ echo killed; exit 0; }; sleep 1; done; echo alive; exit 1",
                           std::chrono::seconds(60));
    return out.find("killed") != std::string::npos;
}

void dump_node_logs(const std::vector<std::string>& ips, const std::string& private_key_pem) {
    for (const auto& ip : ips) {
        try {
            // /tmp for a bootstrap node, /var/log for a replacement started
            // by its user data.
            auto log = ssh_execute(ip, private_key_pem,
                                   "sudo tail -n 80 /tmp/chaos_node.log /var/log/chaos_node.log "
                                   "2>&1; echo; echo '--- ps ---'; pgrep -a -x chaos_node",
                                   std::chrono::seconds(30));
            std::cerr << "=== " << ip << " chaos_node.log ===\n" << log << "\n";
        } catch (const std::exception& e) {
            std::cerr << "=== " << ip << ": could not fetch log: " << e.what() << "\n";
        }
    }
}

}  // namespace

// Task 8: see the file header for the five steps. The Boost timeout covers
// fixture setup and teardown too, so it is the sum of the bounded waits:
//   setup: network, 3 launches (180s each), cloud-init, start   ~18 min
//   leader election                                              3 min
//   detection (30s) + launch + boot + join + removal + term     25 min
//   replacement catch-up check                                   5 min
//   teardown: terminate sweep 300s, VPC wait 180s, deletes 300s 13 min
//   = 64 min -> 3900s
// A healthy run takes roughly fifteen minutes; the budget is dominated by
// waits a healthy run never exhausts.
BOOST_FIXTURE_TEST_CASE(process_crash_triggers_replacement, chaos_crash_network_fixture,
                        *boost::unit_test::timeout(3900)) {
    using manager_t = kythira::aws_ec2_quorum_manager<std::uint64_t>;
    const std::string ami = env("KYTHIRA_EC2_TEST_AMI");
    const std::string profile = env("KYTHIRA_TEST_IAM_INSTANCE_PROFILE");

    // The test's own manager. Same cluster name as QUORUM_CLUSTER, so the
    // kythira:cluster and kythira:node-id tags it writes are exactly what the
    // leader's manager reads to map ids 1..3 to instances.
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = cluster_name;
    cfg.image_id = ami;
    cfg.node_port = RPC_PORT;
    cfg.aws.region = region();
    cfg.security_group_ids = {sg_id};
    cfg.key_name = key_name;
    cfg.iam_instance_profile = profile;
    // instance_type defaults to t3.micro (x86_64) and must match the AMI's
    // architecture, which CI resolves to the build host's.
#if defined(__aarch64__) || defined(__arm64__)
    cfg.instance_type = "t4g.micro";
#endif
    // Groups are the AZ names, as in QUORUM_AWS_SUBNETS/QUORUM_PLACEMENT.
    for (const auto& [az, subnet_id] : subnet_by_az) {
        cfg.topology.groups.push_back({.group_id = az, .target_count = 1});
        cfg.subnet_by_group[az] = subnet_id;
    }
    // A freshly created instance profile can take a moment to be usable by
    // RunInstances; the default 120s is otherwise fine for t3.micro.
    cfg.provision_timeout = std::chrono::seconds{180};
    manager_t mgr{cfg};

    // Progress lines carry seconds since this point: ctest relays the test's
    // output in bursts, so the job log's own timestamps cannot order them.
    const auto test_start = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return "+" +
               std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - test_start)
                                  .count()) +
               "s";
    };

    // ── Step 1: three instances, one per AZ ─────────────────────────────
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    std::vector<node_launch> nodes;
    std::vector<std::string> public_ips;
    std::vector<std::string> instance_ids;
    for (const auto& [az, subnet_id] : subnet_by_az) {
        (void)subnet_id;
        auto peer = mgr.provision_node(az, std::nullopt).get();
        track_instance("node " + std::to_string(peer.node_id) + " (" + az + ")", cfg.instance_type);
        cluster.push_back({.node_id = peer.node_id, .group_id = az});
        auto ec2_id = mgr.instance_id_of(peer.node_id);
        BOOST_REQUIRE_MESSAGE(ec2_id.has_value(), "no instance for node " << peer.node_id);
        auto view = instance(*ec2_id);
        BOOST_REQUIRE_MESSAGE(view && !view->public_ip.empty() && !view->private_ip.empty(),
                              "no public/private IP for " << *ec2_id);
        nodes.push_back({.node_id = peer.node_id, .az = az, .private_ip = view->private_ip});
        public_ips.push_back(view->public_ip);
        instance_ids.push_back(*ec2_id);
        std::cerr << "[" << LOG_TAG << "] " << elapsed() << " launched node " << peer.node_id
                  << " (" << *ec2_id << ", " << az << ", " << view->public_ip << ")\n";
    }
    BOOST_REQUIRE_EQUAL(nodes.size(), 3u);

    // ── Step 2: chaos_node with QUORUM_MANAGER=aws on each ──────────────
    // Every node finishes booting before any chaos_node starts. Booting and
    // starting them one at a time lets the first two form a quorum while the
    // third is still in cloud-init, and their leader, after
    // QUORUM_PEER_DEAD_AFTER_MS of silence from it, replaces and terminates
    // that voter: the behaviour under test, set off by the setup. Run
    // 39465316239 failed in this step with "ssh_execute: could not connect
    // to <ip>:22 in time" after an earlier SSH had succeeded, which is what
    // that would look like (inferred; the log could not show which node).
    // Run 39514236538 then showed each SSH start taking about 270 s, so
    // starting the nodes one after another still left node 3 down long
    // enough for nodes 1 and 2 to replace it (the leader's log had
    // AppendEntries only to nodes 2 and 4). The starts therefore run
    // concurrently, one std::thread each, so however long one takes the three
    // chaos_node processes come up together. A failed SSH reports the
    // instance's state, so a terminated node is told apart from one whose
    // sshd never came up, and dumps every reachable node's log.
    auto report_failure = [&](std::size_t i, const std::runtime_error& e) {
        std::cerr << "[" << LOG_TAG << "] " << elapsed() << " SSH to node " << nodes[i].node_id
                  << " failed; dumping node logs\n";
        dump_node_logs(public_ips, private_key_pem);
        // A fourth cluster-tagged instance means a chaos_node leader
        // provisioned a replacement; none means something else
        // terminated the node.
        if (auto all = describe(*ec2, {make_filter("tag:kythira:cluster", {cluster_name})})) {
            for (const auto& v : *all) {
                std::cerr << "[" << LOG_TAG << "]   cluster instance " << v.instance_id << " in "
                          << v.subnet_id << ": " << v.state << " (" << v.state_reason << ")\n";
            }
        }
        auto view = instance(instance_ids[i]);
        throw std::runtime_error(std::string{e.what()} + " (node " +
                                 std::to_string(nodes[i].node_id) + ", " + instance_ids[i] +
                                 " is " + (view ? view->state : "not found") + ")");
    };
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        // SSH can answer before cloud-init has finished.
        try {
            ssh_execute(public_ips[i], private_key_pem, "sudo cloud-init status --wait",
                        std::chrono::minutes(3));
        } catch (const std::runtime_error& e) {
            report_failure(i, e);
        }
    }
    std::cerr << "[" << LOG_TAG << "] " << elapsed() << " cloud-init finished on all "
              << nodes.size() << " nodes; starting chaos_node\n";
    {
        // libssh2_session_init() initialises the library lazily, which is
        // not thread-safe, so do it once here before the threads start.
        libssh2_init(0);
        std::vector<std::string> commands;
        for (const auto& n : nodes) {
            commands.push_back(start_node_command(
                node_environment(n, nodes, *this, ami, cfg.instance_type, profile)));
        }
        // Each thread writes only its own slot; all are joined before any
        // failure is reported, so no thread is still running when the
        // dump does.
        std::vector<std::string> outputs(nodes.size());
        std::vector<std::optional<std::runtime_error>> errors(nodes.size());
        std::vector<std::thread> starts;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            starts.emplace_back([&, i] {
                try {
                    // The command only launches chaos_node in the
                    // background, so two minutes is generous, and the
                    // channel now closes as soon as the shell exits (see
                    // start_node_command). A timeout after the "launched"
                    // stamp still means chaos_node is up and only the
                    // channel failed to close, so it stays a pass rather
                    // than failing a billable run; one before it is a
                    // failed start.
                    bool timed_out = false;
                    outputs[i] =
                        ssh_execute(public_ips[i], private_key_pem, commands[i],
                                    std::chrono::minutes(1), std::chrono::minutes(2), &timed_out);
                    if (timed_out) {
                        if (outputs[i].find("launched ") == std::string::npos) {
                            throw std::runtime_error(
                                "ssh_execute: chaos_node start on " + public_ips[i] +
                                " did not finish in 120s; output so far: " + outputs[i]);
                        }
                        outputs[i] += "(SSH channel still open after 120s; closed it)\n";
                    }
                } catch (const std::runtime_error& e) {
                    errors[i].emplace(e);
                }
            });
        }
        for (auto& t : starts) {
            t.join();
        }
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (errors[i]) {
                report_failure(i, *errors[i]);
            }
        }
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            std::istringstream lines(outputs[i]);
            std::string stamps;
            for (std::string line; std::getline(lines, line);) {
                stamps += (stamps.empty() ? "" : ", ") + line;
            }
            std::cerr << "[" << LOG_TAG << "] " << elapsed() << " started chaos_node on node "
                      << nodes[i].node_id << " (" << instance_ids[i] << "; " << stamps << ")\n";
        }
    }

    auto leader = find_leader(public_ips, private_key_pem, {}, std::chrono::minutes(3));
    if (!leader) {
        dump_node_logs(public_ips, private_key_pem);
    }
    BOOST_REQUIRE_MESSAGE(leader.has_value(), "no chaos_node leader within the timeout");
    std::cerr << "[" << LOG_TAG << "] cluster " << cluster_name << " formed; leader is node "
              << nodes[*leader].node_id << " (" << nodes[*leader].az << ")\n";

    // ── Step 3: crash a follower's process, leave its instance running ──
    const std::size_t victim = (*leader + 1) % nodes.size();
    const std::string victim_instance = instance_ids[victim];
    const std::string victim_subnet = subnet_by_az.at(nodes[victim].az);
    BOOST_REQUIRE_MESSAGE(kill_node_process(public_ips[victim], private_key_pem),
                          "chaos_node on node " << nodes[victim].node_id << " survived SIGKILL");
    std::cerr << "[" << LOG_TAG << "] killed chaos_node on node " << nodes[victim].node_id << " ("
              << victim_instance << ", " << nodes[victim].az << ")\n";

    // ── Step 4: instance state alone does not see the crash ────────────
    {
        auto health = mgr.assess_quorum(cluster).get();
        BOOST_TEST(health.live_node_count == 3u);
        BOOST_TEST(health.unreachable_nodes.empty());
        auto view = instance(victim_instance);
        BOOST_TEST((view && view->state == "running"),
                   "killed node's instance is " << (view ? view->state : "unknown"));
    }

    // ── Step 5: the leader replaces, removes and terminates ────────────
    // A replacement is any cluster-tagged instance that is not one of the
    // three bootstrap nodes. The victim was its group's only voter, so the
    // leader treats that group as lost and provisions into a surviving one
    // (see the file header): the replacement must land in another of the
    // cluster's subnets. Run 39563651006 asserted the victim's own subnet
    // and failed on exactly this, with the rest of the heal passing.
    const std::set<std::string> bootstrap(instance_ids.begin(), instance_ids.end());
    std::map<std::string, std::string> az_by_subnet;
    for (const auto& [az, subnet] : subnet_by_az) {
        az_by_subnet.emplace(subnet, az);
    }
    auto in_surviving_group = [&](const std::string& subnet) {
        return subnet != victim_subnet && az_by_subnet.contains(subnet);
    };
    std::optional<instance_view> replacement;
    bool victim_gone = false;
    bool replacement_tracked = false;
    const auto heal_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(25);
    while (std::chrono::steady_clock::now() < heal_deadline) {
        auto tagged = describe(*ec2, {make_filter("tag:kythira:cluster", {cluster_name})});
        if (tagged) {
            std::optional<instance_view> found;
            for (const auto& i : *tagged) {
                if (i.instance_id == victim_instance) {
                    victim_gone = (i.state == "shutting-down" || i.state == "terminated");
                }
                if (bootstrap.contains(i.instance_id)) {
                    continue;
                }
                if (i.state == "pending" || i.state == "running") {
                    found = i;
                }
            }
            if (found) {
                replacement = found;
                if (!replacement_tracked) {
                    auto az = az_by_subnet.find(found->subnet_id);
                    track_instance("replacement (" +
                                       (az != az_by_subnet.end() ? az->second : found->subnet_id) +
                                       ")",
                                   cfg.instance_type);
                    replacement_tracked = true;
                    std::cerr << "[" << LOG_TAG << "] leader launched replacement "
                              << found->instance_id << "\n";
                }
            }
        }
        if (replacement && replacement->state == "running" && victim_gone) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    if (!(replacement && replacement->state == "running" && victim_gone)) {
        std::vector<std::string> ips;
        for (std::size_t i = 0; i < public_ips.size(); ++i) {
            if (i != victim) {
                ips.push_back(public_ips[i]);
            }
        }
        if (replacement && !replacement->public_ip.empty()) {
            ips.push_back(replacement->public_ip);
        }
        dump_node_logs(ips, private_key_pem);
    }
    BOOST_REQUIRE_MESSAGE(replacement.has_value(), "the leader never launched a replacement");
    BOOST_REQUIRE_MESSAGE(replacement->state == "running", "replacement "
                                                               << replacement->instance_id << " is "
                                                               << replacement->state);
    BOOST_REQUIRE_MESSAGE(
        victim_gone, "the killed node's instance " << victim_instance << " was never terminated");
    BOOST_TEST(in_surviving_group(replacement->subnet_id),
               "replacement " << replacement->instance_id << " launched in "
                              << replacement->subnet_id
                              << ", which is the killed node's subnet or not the cluster's");

    // The replacement carries a fresh numeric id from the leader's manager.
    auto replacement_id = mgr.node_id_of_instance(replacement->instance_id);
    BOOST_TEST(replacement_id.has_value(), replacement->instance_id << " carries no node id");
    if (replacement_id) {
        for (const auto& n : nodes) {
            BOOST_TEST(*replacement_id != n.node_id);
        }
    }

    // The leader survived its own repair (killing a follower moves no
    // leadership), and the replacement's chaos_node caught up with it.
    auto leader_status = status_of(public_ips[*leader], private_key_pem);
    BOOST_REQUIRE_MESSAGE(leader_status.has_value(), "the leader stopped answering /status");
    BOOST_TEST(leader_status->role == "leader", "leader /status: " << leader_status->raw);

    BOOST_REQUIRE_MESSAGE(!replacement->public_ip.empty(), "no public IP for the replacement");
    std::optional<node_status> replacement_status;
    const auto catch_up_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (std::chrono::steady_clock::now() < catch_up_deadline) {
        replacement_status = status_of(replacement->public_ip, private_key_pem);
        if (replacement_status && replacement_status->commit_index >= leader_status->commit_index) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    if (!replacement_status || replacement_status->commit_index < leader_status->commit_index) {
        dump_node_logs({public_ips[*leader], replacement->public_ip}, private_key_pem);
    }
    BOOST_TEST(replacement_status.has_value(), "the replacement never answered /status");
    if (replacement_status) {
        BOOST_TEST(replacement_status->role != "candidate",
                   "replacement /status: " << replacement_status->raw);
        BOOST_TEST(replacement_status->commit_index >= leader_status->commit_index,
                   "replacement commit_index " << replacement_status->commit_index
                                               << " behind the leader's "
                                               << leader_status->commit_index);
    }

    // The test's manager now sees the healed cluster as fully live.
    if (replacement_id) {
        auto healed = cluster;
        healed[victim] = {.node_id = *replacement_id, .group_id = nodes[victim].az};
        auto health = mgr.assess_quorum(healed).get();
        BOOST_TEST(health.live_node_count == 3u);
    }
    // Every instance, the replacement included, goes in teardown().
}

#endif  // LIBSSH2_FOUND
#endif  // KYTHIRA_HAS_AWS_SDK
#endif  // KYTHIRA_AWS_REAL_EC2_TESTS
