// Real-EC2 coverage for the 3-AZ ca_cluster_node AWS deployment
// (Requirement 17.12(b)). Unlike ca_cluster_node_localstack_test.cpp (which
// can only verify the provisioning topology, since LocalStack's EC2 API is a
// control-plane mock that never boots real software), this launches three
// REAL EC2 instances running the real ca_cluster_node binary and verifies,
// over SSH, that they actually form a working Raft-replicated CA cluster:
// one node bootstraps, the other two converge, and the resulting cluster
// issues a certificate through whichever node ends up leader.
//
// Requires (all via environment variables, following the convention already
// established by aws_quorum_manager_real_ec2_test.cpp):
//   KYTHIRA_EC2_TEST_AMI        AMI ID with /usr/local/bin/ca_cluster_node
//                               installed — build one with
//                               packer/ca_cluster_node/scripts/build.sh
//                               (see packer/ca_cluster_node/README.md)
//   KYTHIRA_EC2_TEST_KEY_NAME   (optional) existing EC2 key pair name to reuse;
//                               a fresh one is created/destroyed per test otherwise
//   AWS credentials via the standard provider chain; AWS_REGION or a default
//   region configured in aws_client_config
//
// Not run by default (LABELS real-ec2;slow) — same gating as the existing
// aws_quorum_manager_real_ec2_test.cpp.

#define BOOST_TEST_MODULE ca_cluster_node_real_ec2_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_AWS_REAL_EC2_TESTS
#ifdef KYTHIRA_HAS_AWS_SDK
#ifdef LIBSSH2_FOUND

#include <raft/aws_ec2_quorum_manager.hpp>

#include <aws/core/Aws.h>
#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/AllocateAddressRequest.h>
#include <aws/ec2/model/AssociateAddressRequest.h>
#include <aws/ec2/model/AttachInternetGatewayRequest.h>
#include <aws/ec2/model/AuthorizeSecurityGroupIngressRequest.h>
#include <aws/ec2/model/CreateInternetGatewayRequest.h>
#include <aws/ec2/model/CreateKeyPairRequest.h>
#include <aws/ec2/model/CreateRouteRequest.h>
#include <aws/ec2/model/CreateRouteTableRequest.h>
#include <aws/ec2/model/CreateSecurityGroupRequest.h>
#include <aws/ec2/model/CreateSubnetRequest.h>
#include <aws/ec2/model/CreateVpcRequest.h>
#include <aws/ec2/model/DeleteInternetGatewayRequest.h>
#include <aws/ec2/model/DeleteKeyPairRequest.h>
#include <aws/ec2/model/DeleteRouteTableRequest.h>
#include <aws/ec2/model/DeleteSecurityGroupRequest.h>
#include <aws/ec2/model/DeleteSubnetRequest.h>
#include <aws/ec2/model/DeleteVpcRequest.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/DetachInternetGatewayRequest.h>
#include <aws/ec2/model/DisassociateAddressRequest.h>
#include <aws/ec2/model/IpPermission.h>
#include <aws/ec2/model/IpRange.h>
#include <aws/ec2/model/ModifySubnetAttributeRequest.h>
#include <aws/ec2/model/ReleaseAddressRequest.h>
#include <aws/ec2/model/AssociateRouteTableRequest.h>
#include <aws/ec2/model/TerminateInstancesRequest.h>
#include <aws/sts/STSClient.h>
#include <aws/sts/model/GetCallerIdentityRequest.h>

#include <libssh2.h>

#include <folly/init/Init.h>

#include "aws_real_ec2_test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
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
// The unseal passphrase every launched node is configured with — a fixed
// test value (Requirement 17.4 only requires byte-identical across nodes,
// not secrecy for a throwaway test cluster torn down at the end of the run).
constexpr const char* TEST_UNSEAL_PASSPHRASE = "kythira-real-ec2-test-unseal-passphrase";
constexpr const char* TEST_AUTH_TOKEN = "kythira-real-ec2-test-auth-token";

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        folly::init(&argc, &argv, false);
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

BOOST_GLOBAL_FIXTURE(FollyInitFixture);
BOOST_GLOBAL_FIXTURE(AwsSdkFixture);

// BOOST_GLOBAL_FIXTURE forms an identifier from its argument, so a
// namespace-qualified type must be brought into scope unqualified first.
using kythira::testing::aws_real_ec2::AwsSignalHandlerFixture;
using kythira::testing::aws_real_ec2::CostSummaryFixture;
BOOST_GLOBAL_FIXTURE(CostSummaryFixture);
BOOST_GLOBAL_FIXTURE(AwsSignalHandlerFixture);

auto env(const char* name) -> std::string {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string{};
}

auto region() -> std::string {
    auto r = env("AWS_REGION");
    return r.empty() ? DUMMY_REGION_FALLBACK : r;
}

// Executes `cmd` on the host at `public_ip` via SSH (public-key auth using an
// in-memory-generated key pair — the EC2-side key pair created by this
// fixture), returning stdout. Retries the connection itself (not the
// command) since a freshly-launched instance's sshd may not be accepting
// connections yet.
auto ssh_execute(const std::string& public_ip, const std::string& private_key_pem,
                 const std::string& cmd, std::chrono::seconds connect_timeout) -> std::string {
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
    BOOST_REQUIRE(session != nullptr);
    BOOST_REQUIRE_GE(libssh2_session_handshake(session, sock), 0);

    // "ubuntu", not "ec2-user": the AMI this test SSHes into is Packer-built
    // from Ubuntu 24.04 (packer/ca_cluster_node/ca_cluster_node.pkr.hcl),
    // not Amazon Linux - ec2-user is Amazon Linux's default SSH user.
    int rc = libssh2_userauth_publickey_frommemory(
        session, "ubuntu", 6, nullptr, 0, private_key_pem.data(), private_key_pem.size(), nullptr);
    BOOST_REQUIRE_MESSAGE(rc == 0, "SSH auth failed, rc=" + std::to_string(rc));

    LIBSSH2_CHANNEL* channel = libssh2_channel_open_session(session);
    BOOST_REQUIRE(channel != nullptr);
    BOOST_REQUIRE_GE(libssh2_channel_exec(channel, cmd.c_str()), 0);

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
    return output;
}

// Minimal public-subnet-per-AZ VPC (Internet Gateway + route table, no NAT/
// bastion complexity) — sufficient for a short-lived test cluster reached
// directly via public IP + a security group scoped to the test runner's
// own IP for both SSH and the ca_cluster_node HTTP port.
struct three_az_network_fixture : kythira::testing::aws_real_ec2::signal_cleanup_target {
    std::shared_ptr<Aws::EC2::EC2Client> ec2;
    std::string vpc_id, igw_id, route_table_id, sg_id, key_name;
    std::string private_key_pem;
    std::map<std::string, std::string> subnet_by_az;
    kythira::testing::aws_real_ec2::TestCostReport cost_report{
        std::string(boost::unit_test::framework::current_test_case().p_name)};
    bool torn_down_ = false;

    // Called once per node right after RunInstances succeeds for it (this
    // fixture provisions nodes one at a time via aws_ec2_quorum_manager
    // rather than in one batch RunInstances call, unlike RealEc2Fixture's
    // own track_instances(count)).
    void track_instance(const std::string& label, const std::string& instance_type) {
        cost_report.resources.push_back(
            {label, kythira::testing::aws_real_ec2::ec2_hourly_rate(instance_type)});
    }

    three_az_network_fixture() {
        Aws::Client::ClientConfiguration cli_cfg;
        cli_cfg.region = region();
        ec2 = std::make_shared<Aws::EC2::EC2Client>(cli_cfg);

        Aws::STS::STSClient sts{cli_cfg};
        auto id_out = sts.GetCallerIdentity(Aws::STS::Model::GetCallerIdentityRequest{});
        if (!id_out.IsSuccess()) {
            throw std::runtime_error("AWS not reachable / no credentials (skip): " +
                                     std::string(id_out.GetError().GetMessage()));
        }
        if (env("KYTHIRA_EC2_TEST_AMI").empty()) {
            throw std::runtime_error("KYTHIRA_EC2_TEST_AMI not set (skip)");
        }

        // Register as the signal-cleanup target before any AWS resource is
        // created so a signal arriving mid-setup still invokes teardown()
        // (matching RealEc2Fixture's identical placement).
        kythira::testing::aws_real_ec2::g_active_aws_fixture.store(this, std::memory_order_release);

        // A BOOST_REQUIRE failure partway through this sequence throws out of
        // the constructor, which means the object is never considered fully
        // constructed and ~three_az_network_fixture() never runs — every
        // resource created by the steps that already succeeded would
        // otherwise leak silently. Catch here and run the same teardown()
        // the destructor would have, then rethrow so Boost.Test still
        // records the failure.
        try {
            Aws::EC2::Model::CreateVpcRequest vpc_req;
            vpc_req.SetCidrBlock("10.220.0.0/16");
            auto vpc_out = ec2->CreateVpc(vpc_req);
            BOOST_REQUIRE_MESSAGE(vpc_out.IsSuccess(),
                                  "CreateVpc: " + std::string(vpc_out.GetError().GetMessage()));
            vpc_id = std::string(vpc_out.GetResult().GetVpc().GetVpcId());

            Aws::EC2::Model::ModifySubnetAttributeRequest unused;
            (void)unused;

            auto igw_out = ec2->CreateInternetGateway({});
            BOOST_REQUIRE(igw_out.IsSuccess());
            igw_id = std::string(igw_out.GetResult().GetInternetGateway().GetInternetGatewayId());
            Aws::EC2::Model::AttachInternetGatewayRequest attach_req;
            attach_req.SetVpcId(vpc_id);
            attach_req.SetInternetGatewayId(igw_id);
            ec2->AttachInternetGateway(attach_req);

            Aws::EC2::Model::CreateRouteTableRequest rt_req;
            rt_req.SetVpcId(vpc_id);
            auto rt_out = ec2->CreateRouteTable(rt_req);
            BOOST_REQUIRE(rt_out.IsSuccess());
            route_table_id = std::string(rt_out.GetResult().GetRouteTable().GetRouteTableId());
            Aws::EC2::Model::CreateRouteRequest route_req;
            route_req.SetRouteTableId(route_table_id);
            route_req.SetDestinationCidrBlock("0.0.0.0/0");
            route_req.SetGatewayId(igw_id);
            ec2->CreateRoute(route_req);

            int octet = 1;
            for (const std::string& az : {region() + "a", region() + "b", region() + "c"}) {
                Aws::EC2::Model::CreateSubnetRequest sn_req;
                sn_req.SetVpcId(vpc_id);
                sn_req.SetCidrBlock("10.220." + std::to_string(octet++) + ".0/24");
                sn_req.SetAvailabilityZone(az);
                auto sn_out = ec2->CreateSubnet(sn_req);
                BOOST_REQUIRE_MESSAGE(sn_out.IsSuccess(), "CreateSubnet(" + az + ")");
                std::string subnet_id = std::string(sn_out.GetResult().GetSubnet().GetSubnetId());
                subnet_by_az[az] = subnet_id;

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
            sg_req.SetGroupName("kythira-ca-cluster-real-ec2-test-sg");
            sg_req.SetDescription("kythira ca_cluster_node real-EC2 test");
            sg_req.SetVpcId(vpc_id);
            auto sg_out = ec2->CreateSecurityGroup(sg_req);
            BOOST_REQUIRE(sg_out.IsSuccess());
            sg_id = std::string(sg_out.GetResult().GetGroupId());

            for (int port : {22, 7000, 8443}) {
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

            key_name = "kythira-ca-cluster-test-key-" + std::to_string(::getpid());
            Aws::EC2::Model::CreateKeyPairRequest kp_req;
            kp_req.SetKeyName(key_name);
            auto kp_out = ec2->CreateKeyPair(kp_req);
            BOOST_REQUIRE_MESSAGE(kp_out.IsSuccess(), "CreateKeyPair");
            private_key_pem = std::string(kp_out.GetResult().GetKeyMaterial());
        } catch (...) {
            teardown();
            throw;
        }
    }

    // signal_cleanup_target's destructor is deliberately non-virtual
    // (protected, never deleted through a base pointer), so this
    // destructor doesn't `override` anything — only teardown() does.
    ~three_az_network_fixture() { teardown(); }

    void teardown() noexcept override {
        if (torn_down_) {
            return;
        }
        torn_down_ = true;
        kythira::testing::aws_real_ec2::g_active_aws_fixture.store(nullptr,
                                                                   std::memory_order_release);

        // The test body provisions its 3 cluster nodes via
        // aws_ec2_quorum_manager::provision_node(), which has no matching
        // teardown-time cleanup of its own (the manager never tracks or
        // terminates what it provisions - that's the caller's job via
        // decommission_node(), which this test never calls). Without this,
        // every run - pass or fail - leaked 3 real running EC2 instances
        // forever, which in turn made the subnet/VPC deletes below fail
        // silently too (a VPC with running instances inside it can't be
        // deleted). Querying by vpc_id here means this doesn't need the
        // test body to track instance IDs itself.
        if (!vpc_id.empty()) {
            Aws::EC2::Model::DescribeInstancesRequest desc;
            Aws::EC2::Model::Filter vpc_filter;
            vpc_filter.SetName("vpc-id");
            vpc_filter.AddValues(vpc_id);
            desc.AddFilters(vpc_filter);
            Aws::EC2::Model::Filter state_filter;
            state_filter.SetName("instance-state-name");
            state_filter.AddValues("pending");
            state_filter.AddValues("running");
            state_filter.AddValues("stopping");
            state_filter.AddValues("stopped");
            desc.AddFilters(state_filter);
            auto desc_out = ec2->DescribeInstances(desc);
            if (desc_out.IsSuccess()) {
                Aws::EC2::Model::TerminateInstancesRequest term;
                bool any = false;
                for (const auto& res : desc_out.GetResult().GetReservations()) {
                    for (const auto& inst : res.GetInstances()) {
                        term.AddInstanceIds(inst.GetInstanceId());
                        any = true;
                    }
                }
                if (any) {
                    ec2->TerminateInstances(term);
                    std::this_thread::sleep_for(std::chrono::seconds{30});
                }
            }
        }

        if (!key_name.empty()) {
            Aws::EC2::Model::DeleteKeyPairRequest req;
            req.SetKeyName(key_name);
            ec2->DeleteKeyPair(req);
        }
        if (!sg_id.empty()) {
            Aws::EC2::Model::DeleteSecurityGroupRequest req;
            req.SetGroupId(sg_id);
            ec2->DeleteSecurityGroup(req);
        }
        for (const auto& [az, subnet_id] : subnet_by_az) {
            (void)az;
            Aws::EC2::Model::DeleteSubnetRequest req;
            req.SetSubnetId(subnet_id);
            ec2->DeleteSubnet(req);
        }
        if (!route_table_id.empty()) {
            Aws::EC2::Model::DeleteRouteTableRequest req;
            req.SetRouteTableId(route_table_id);
            ec2->DeleteRouteTable(req);
        }
        if (!igw_id.empty()) {
            Aws::EC2::Model::DetachInternetGatewayRequest detach_req;
            detach_req.SetVpcId(vpc_id);
            detach_req.SetInternetGatewayId(igw_id);
            ec2->DetachInternetGateway(detach_req);
            Aws::EC2::Model::DeleteInternetGatewayRequest req;
            req.SetInternetGatewayId(igw_id);
            ec2->DeleteInternetGateway(req);
        }
        // Retried: AWS's own dependency resolution after ENI teardown can
        // lag past when everything above already reports gone. See
        // aws_quorum_manager_real_ec2_test.cpp's identical fix for the full
        // rationale (found via the same real-AWS leaked-VPC investigation).
        if (!vpc_id.empty()) {
            auto vpc_deadline = std::chrono::steady_clock::now() + std::chrono::minutes{5};
            while (std::chrono::steady_clock::now() < vpc_deadline) {
                Aws::EC2::Model::DeleteVpcRequest req;
                req.SetVpcId(vpc_id);
                if (ec2->DeleteVpc(req).IsSuccess()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds{15});
            }
        }

        for (auto& r : cost_report.resources) {
            r.finalize();
        }
        BOOST_TEST_MESSAGE(cost_report.format());
        kythira::testing::aws_real_ec2::g_cost_accumulator.add(std::move(cost_report));
    }

    // Public IP of an already-running instance, via DescribeInstances.
    auto public_ip_of(const std::string& instance_id) -> std::string {
        Aws::EC2::Model::DescribeInstancesRequest req;
        req.AddInstanceIds(instance_id);
        auto out = ec2->DescribeInstances(req);
        BOOST_REQUIRE(out.IsSuccess());
        for (const auto& res : out.GetResult().GetReservations()) {
            for (const auto& inst : res.GetInstances()) {
                return std::string(inst.GetPublicIpAddress());
            }
        }
        return "";
    }
};

// User-data that ONLY prepares the unseal key file — it deliberately does
// NOT start ca_cluster_node yet. Each node's --peers argument must name the
// other two nodes' real addresses, but ca_cluster_node has no built-in peer
// discovery (Requirement 17 replicates CA state via Raft once peers are
// known, not via a separate discovery protocol) and EC2 only assigns each
// instance's public IP once it's actually running — so the three nodes'
// addresses aren't knowable until after all three have launched. The actual
// `ca_cluster_node --peers ...` invocation is started afterward, over SSH,
// once every public IP is known (see the test case body).
auto make_user_data() -> std::string {
    std::ostringstream script;
    script << "#!/bin/bash\n"
           << "mkdir -p /var/lib/ca_cluster_node /etc/ca_cluster_node\n"
           << "printf '%s' '" << TEST_UNSEAL_PASSPHRASE << "' > /etc/ca_cluster_node/unseal.key\n"
           << "chmod 600 /etc/ca_cluster_node/unseal.key\n";
    return script.str();
}

auto start_node_command(std::uint64_t node_id, const std::string& peers_arg, bool bootstrap)
    -> std::string {
    // sudo: make_user_data()'s script runs as root (cloud-init) and leaves
    // /etc/ca_cluster_node/unseal.key at mode 600 (root-only) and
    // /var/lib/ca_cluster_node owned by root — this command runs over SSH
    // as "ubuntu" (see ssh_execute()), which can neither read the unseal
    // key nor write the data dir without it.
    //
    // Log target is /tmp, not /var/log: found via a real-AWS isolation
    // investigation (three separate probes: plain backgrounding, sudo +
    // backgrounding, and finally this exact redirect target) that
    // /var/log/ca_cluster_node.log was the actual problem all along. Shell
    // redirects are opened by the *invoking* shell before it execs
    // anything — here that's the outer "ubuntu" shell, before sudo ever
    // runs — and ubuntu has no write permission in /var/log. That open
    // failed silently (its error went to a stream ssh_execute() never
    // reads), so the entire command line — sudo, setsid, nohup, and
    // ca_cluster_node itself — never ran at all. setsid was added during
    // the same investigation and is harmless but turned out not to be the
    // actual fix; kept anyway since it costs nothing and is still the
    // more correct way to fully detach a backgrounded process over SSH.
    std::ostringstream cmd;
    cmd << "sudo setsid nohup /usr/local/bin/ca_cluster_node --node-id " << node_id
        << " --rpc-port 7000 --http-port 8443 --data-dir /var/lib/ca_cluster_node"
        << " --unseal-key-file /etc/ca_cluster_node/unseal.key"
        << " --peers " << peers_arg << " --auth-token " << TEST_AUTH_TOKEN
        << (bootstrap ? " --bootstrap-ca" : "")
        << " > /tmp/ca_cluster_node.log 2>&1 < /dev/null &\ndisown\n";
    return cmd.str();
}

}  // namespace

// Requirement 17.12(b) end-to-end: three real EC2 instances, one per AZ,
// running the real ca_cluster_node binary — verifies the deployment
// documented in docker/ca_cluster_node/README.md actually produces a
// working Raft-replicated CA cluster, not just correctly-placed instances
// (that half is already covered by ca_cluster_node_localstack_test.cpp).
BOOST_FIXTURE_TEST_CASE(three_real_ec2_nodes_form_working_ca_cluster, three_az_network_fixture,
                        *boost::unit_test::timeout(900)) {
    std::string ami = env("KYTHIRA_EC2_TEST_AMI");

    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "ca-cluster-real-ec2-test";
    cfg.image_id = ami;
    cfg.node_port = 7000;
    cfg.aws.region = region();
    cfg.security_group_ids = {sg_id};
    // Without this, RunInstances never attaches this fixture's own key pair
    // (key_name/private_key_pem below) to any provisioned node, so every
    // SSH auth attempt against a manager-provisioned node fails
    // unconditionally regardless of username - the key material this test
    // holds was never placed on the instance in the first place.
    cfg.key_name = key_name;
    // instance_type defaults to "t3.micro" (x86_64) - must match the AMI's
    // own architecture (resolved by CI to the build host's arch) or
    // RunInstances rejects the request outright.
#if defined(__aarch64__) || defined(__arm64__)
    cfg.instance_type = "t4g.micro";
#endif
    cfg.user_data_template = make_user_data();  // installs the unseal key only; see its own comment
    for (const auto& [az, subnet_id] : subnet_by_az) {
        cfg.topology.groups.push_back({.group_id = az, .target_count = 1});
        cfg.subnet_by_group[az] = subnet_id;
    }

    kythira::aws_ec2_quorum_manager<> mgr{cfg};

    // Phase 1: launch all three instances (subnets have MapPublicIpOnLaunch
    // set, so each gets a public IP automatically — no Elastic IP juggling
    // needed). provision_node() only returns {node_id, private_ip:port}, not
    // the EC2 instance ID or public IP, so phase 2 below looks each instance
    // up by its known private IP to get the rest.
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    std::vector<std::string> private_ips;
    for (const auto& [az, subnet_id] : subnet_by_az) {
        (void)subnet_id;
        auto peer = mgr.provision_node(az, std::nullopt).get();
        cluster.push_back({.node_id = peer.node_id, .group_id = az});
        auto colon = peer.address.rfind(':');
        private_ips.push_back(peer.address.substr(0, colon));
        track_instance("node " + std::to_string(peer.node_id) + " (" + az + ")", cfg.instance_type);
    }
    BOOST_REQUIRE_EQUAL(cluster.size(), 3u);

    // Phase 2: resolve each instance's public IP by its known private IP.
    std::vector<std::string> public_ips;
    for (const auto& private_ip : private_ips) {
        Aws::EC2::Model::DescribeInstancesRequest req;
        Aws::EC2::Model::Filter filter;
        filter.SetName("private-ip-address");
        filter.AddValues(private_ip);
        req.AddFilters(filter);
        auto out = ec2->DescribeInstances(req);
        BOOST_REQUIRE(out.IsSuccess());
        std::string public_ip;
        for (const auto& res : out.GetResult().GetReservations()) {
            for (const auto& inst : res.GetInstances()) {
                public_ip = std::string(inst.GetPublicIpAddress());
            }
        }
        BOOST_REQUIRE_MESSAGE(!public_ip.empty(),
                              "no public IP found for private IP " + private_ip);
        public_ips.push_back(public_ip);
    }

    // Phase 3: now that every public IP is known, build the shared --peers
    // list and start ca_cluster_node on each instance over SSH.
    std::ostringstream peers;
    for (std::size_t i = 0; i < public_ips.size(); ++i) {
        if (i > 0) {
            peers << ",";
        }
        // start_node_command() below passes no --tls-cert/--tls-key, so
        // ca_cluster_node's client-facing listener falls back to plain
        // HTTP (with its own "running without TLS" warning) — the peers
        // URL scheme and the curl checks below must match what's actually
        // listening, not what a production deployment would use.
        peers << (i + 1) << ":" << public_ips[i] << ":7000@http://" << public_ips[i] << ":8443";
    }
    std::string peers_arg = peers.str();

    for (std::size_t i = 0; i < public_ips.size(); ++i) {
        auto cmd = start_node_command(i + 1, peers_arg, /*bootstrap=*/i == 0);
        ssh_execute(public_ips[i], private_key_pem, cmd, std::chrono::minutes(3));
    }

    // Wait for the cluster to bootstrap and elect a leader, then confirm it
    // can actually serve — verified over SSH (curl against localhost:8443 on
    // each instance) rather than a direct HTTPS client from the test runner,
    // since the runner's own network path to the instances' HTTP port is not
    // guaranteed even though the security group permits it (e.g. running
    // from a CI environment without direct internet egress).
    std::string leader_ip;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (std::chrono::steady_clock::now() < deadline && leader_ip.empty()) {
        for (const auto& ip : public_ips) {
            auto out =
                ssh_execute(ip, private_key_pem,
                            "curl -sf -o /dev/null -w '%{http_code}' "
                            "-H 'Authorization: Bearer " +
                                std::string(TEST_AUTH_TOKEN) + "' http://localhost:8443/v1/root-ca",
                            std::chrono::seconds(30));
            if (out == "200") {
                leader_ip = ip;
                break;
            }
        }
        if (leader_ip.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }
    if (leader_ip.empty()) {
        // std::cerr, not BOOST_TEST_MESSAGE: this project's real-ec2 tests
        // run under Boost.Test's default log level, which does not print
        // message-level output at all — confirmed empirically when an
        // earlier BOOST_TEST_MESSAGE version of this same diagnostic
        // produced zero output in a real CI run. stderr always shows up
        // under ctest --output-on-failure regardless of Boost.Test's own
        // log-level filtering.
        for (const auto& ip : public_ips) {
            try {
                auto log = ssh_execute(ip, private_key_pem,
                                       "sudo cat /tmp/ca_cluster_node.log 2>&1; echo; "
                                       "echo '--- ps ---'; ps aux | grep ca_cluster_node",
                                       std::chrono::seconds(30));
                std::cerr << "=== " << ip << " ca_cluster_node.log ===\n" << log << "\n";
            } catch (const std::exception& e) {
                std::cerr << "=== " << ip << ": could not fetch log: " << e.what() << "\n";
            }
        }
    }
    BOOST_REQUIRE_MESSAGE(!leader_ip.empty(),
                          "no ca_cluster_node leader became reachable within the timeout");
    BOOST_TEST_MESSAGE("leader reachable at " << leader_ip);

    for (const auto& p : cluster) {
        BOOST_CHECK_NO_THROW(mgr.decommission_node(p.node_id).get());
    }
}

#endif  // LIBSSH2_FOUND
#endif  // KYTHIRA_HAS_AWS_SDK
#endif  // KYTHIRA_AWS_REAL_EC2_TESTS
