// Real-EC2 coverage for the RPC-TLS-enabled 3-AZ ca_cluster_node AWS
// deployment (.kiro/specs/ca-cluster-rpc-mtls-real-aws/). Extends
// ca_cluster_node_real_ec2_test.cpp's plain-TCP three_az_network_fixture
// shape (copied, not inherited — the two fixtures' provisioning code is
// deliberately independent so a change to one can never silently change
// the other) with RPC TLS enabled from first boot: bootstrap-credential
// generation and distribution via user-data, plus four test cases covering
// properties specific to RPC TLS that a loopback test cannot exercise on
// real, separate hosts.
//
// Directly motivated by ca-cluster-rpc-mtls's own history: its first
// implementation passed every local/loopback test, merged, and then
// deadlocked reliably on GitHub Actions' shared CI runners — a race
// loopback testing never exercised (see that spec's fix commit and
// doc/TODO.md's "What Changed" entry). This file is a second,
// environment-specific line of defense on real infrastructure.
//
// Requires the same environment variables as ca_cluster_node_real_ec2_test.cpp:
//   KYTHIRA_EC2_TEST_AMI        AMI ID with /usr/local/bin/ca_cluster_node
//                               installed (see docker/ca_cluster_node/Dockerfile)
//   AWS credentials via the standard provider chain; AWS_REGION or a default
//   region configured in aws_client_config
//
// Not run by default (LABELS real-ec2;slow) — same gating as the existing
// real-EC2 tests.

#define BOOST_TEST_MODULE ca_cluster_node_rpc_tls_real_ec2_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_AWS_REAL_EC2_TESTS
#ifdef KYTHIRA_HAS_AWS_SDK
#ifdef LIBSSH2_FOUND

#include <raft/aws_ec2_quorum_manager.hpp>
#include <raft/ca_bootstrap_client.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>

#include <aws/core/Aws.h>
#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/AllocateAddressRequest.h>
#include <aws/ec2/model/AssociateAddressRequest.h>
#include <aws/ec2/model/AssociateRouteTableRequest.h>
#include <aws/ec2/model/AttachInternetGatewayRequest.h>
#include <aws/ec2/model/AuthorizeSecurityGroupIngressRequest.h>
#include <aws/ec2/model/CreateInternetGatewayRequest.h>
#include <aws/ec2/model/CreateKeyPairRequest.h>
#include <aws/ec2/model/CreateNetworkAclEntryRequest.h>
#include <aws/ec2/model/CreateNetworkAclRequest.h>
#include <aws/ec2/model/CreateRouteRequest.h>
#include <aws/ec2/model/CreateRouteTableRequest.h>
#include <aws/ec2/model/CreateSecurityGroupRequest.h>
#include <aws/ec2/model/CreateSubnetRequest.h>
#include <aws/ec2/model/CreateVpcRequest.h>
#include <aws/ec2/model/DeleteInternetGatewayRequest.h>
#include <aws/ec2/model/DeleteKeyPairRequest.h>
#include <aws/ec2/model/DeleteNetworkAclRequest.h>
#include <aws/ec2/model/DeleteRouteTableRequest.h>
#include <aws/ec2/model/DeleteSecurityGroupRequest.h>
#include <aws/ec2/model/DeleteSubnetRequest.h>
#include <aws/ec2/model/DeleteVpcRequest.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/DescribeNetworkAclsRequest.h>
#include <aws/ec2/model/DetachInternetGatewayRequest.h>
#include <aws/ec2/model/DisassociateAddressRequest.h>
#include <aws/ec2/model/IpPermission.h>
#include <aws/ec2/model/IpRange.h>
#include <aws/ec2/model/ModifySubnetAttributeRequest.h>
#include <aws/ec2/model/ReleaseAddressRequest.h>
#include <aws/ec2/model/ReplaceNetworkAclAssociationRequest.h>
#include <aws/sts/STSClient.h>
#include <aws/sts/model/GetCallerIdentityRequest.h>

#include <libssh2.h>

#include <folly/init/Init.h>

#include "aws_real_ec2_test_support.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace raft::testing;

namespace {

constexpr const char* DUMMY_REGION_FALLBACK = "us-east-1";
// Requirement 2.4 (byte-identical across nodes) only, not secrecy — a
// throwaway test cluster torn down at the end of the run.
constexpr const char* TEST_UNSEAL_PASSPHRASE = "kythira-rpc-tls-real-ec2-test-unseal-passphrase";
constexpr const char* TEST_AUTH_TOKEN = "kythira-rpc-tls-real-ec2-test-auth-token";

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

// Identical to ca_cluster_node_real_ec2_test.cpp's own ssh_execute() — see
// that file for why this is duplicated rather than shared (design.md's
// Architecture rationale: the two fixtures' provisioning code is
// deliberately independent, and this ~45-line helper has no state of its
// own worth extracting on its own).
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
        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) break;
        ::close(sock);
        sock = -1;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    if (sock < 0)
        throw std::runtime_error("ssh_execute: could not connect to " + public_ip + ":22 in time");

    LIBSSH2_SESSION* session = libssh2_session_init();
    BOOST_REQUIRE(session != nullptr);
    BOOST_REQUIRE_GE(libssh2_session_handshake(session, sock), 0);

    int rc = libssh2_userauth_publickey_frommemory(session, "ec2-user", 8, nullptr, 0,
                                                   private_key_pem.data(), private_key_pem.size(),
                                                   nullptr);
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

// Public-subnet-per-AZ VPC (Internet Gateway + route table, no NAT/bastion
// complexity), plus a deny-all NACL for Requirement 5's isolation test —
// otherwise structurally identical to ca_cluster_node_real_ec2_test.cpp's
// three_az_network_fixture. Network isolation here uses the same
// subnet-level deny-all-NACL-swap technique aws_quorum_manager_real_ec2_test.cpp
// already implements and exercises (CreateNetworkAcl + CreateNetworkAclEntry
// + ReplaceNetworkAclAssociation), not per-instance security-group
// reassignment — this project's own real-EC2 test suite has a working,
// exercised example of the former and none of the latter, and the two are
// equally valid ways to isolate one AZ's single node in this fixture's
// one-node-per-AZ/one-subnet-per-AZ topology.
struct rpc_tls_three_az_network_fixture : kythira::testing::aws_real_ec2::signal_cleanup_target {
    std::shared_ptr<Aws::EC2::EC2Client> ec2;
    std::string vpc_id, igw_id, route_table_id, sg_id, key_name;
    std::string private_key_pem;
    std::map<std::string, std::string> subnet_by_az;

    // Requirement 1.2: the shared bootstrap credential, generated once in
    // memory per fixture instance (i.e. once per test case).
    certificate_authority bootstrap_cred;

    // Requirement 5: deny-all NACL + saved original association, for
    // whichever AZ a test case chooses to isolate.
    std::string deny_all_nacl_id;
    std::map<std::string, std::string> original_nacl_assoc_by_az;

    kythira::testing::aws_real_ec2::TestCostReport cost_report{
        std::string(boost::unit_test::framework::current_test_case().p_name)};
    bool torn_down_ = false;

    void track_instance(const std::string& label, const std::string& instance_type) {
        cost_report.resources.push_back(
            {label, kythira::testing::aws_real_ec2::ec2_hourly_rate(instance_type)});
    }

    rpc_tls_three_az_network_fixture() {
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
        // created so a signal arriving mid-setup still invokes teardown().
        kythira::testing::aws_real_ec2::g_active_aws_fixture.store(this, std::memory_order_release);

        Aws::EC2::Model::CreateVpcRequest vpc_req;
        vpc_req.SetCidrBlock("10.221.0.0/16");
        auto vpc_out = ec2->CreateVpc(vpc_req);
        BOOST_REQUIRE_MESSAGE(vpc_out.IsSuccess(),
                              "CreateVpc: " + std::string(vpc_out.GetError().GetMessage()));
        vpc_id = std::string(vpc_out.GetResult().GetVpc().GetVpcId());

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
            sn_req.SetCidrBlock("10.221." + std::to_string(octet++) + ".0/24");
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
        sg_req.SetGroupName("kythira-ca-cluster-rpc-tls-real-ec2-test-sg");
        sg_req.SetDescription("kythira ca_cluster_node RPC-TLS real-EC2 test");
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

        key_name = "kythira-ca-cluster-rpc-tls-test-key-" + std::to_string(::getpid());
        Aws::EC2::Model::CreateKeyPairRequest kp_req;
        kp_req.SetKeyName(key_name);
        auto kp_out = ec2->CreateKeyPair(kp_req);
        BOOST_REQUIRE_MESSAGE(kp_out.IsSuccess(), "CreateKeyPair");
        private_key_pem = std::string(kp_out.GetResult().GetKeyMaterial());

        create_deny_all_nacl();
    }

    // signal_cleanup_target's destructor is deliberately non-virtual
    // (protected, never deleted through a base pointer), so this
    // destructor doesn't `override` anything — only teardown() does.
    ~rpc_tls_three_az_network_fixture() { teardown(); }

    void teardown() noexcept override {
        if (torn_down_) {
            return;
        }
        torn_down_ = true;
        kythira::testing::aws_real_ec2::g_active_aws_fixture.store(nullptr,
                                                                   std::memory_order_release);

        // Restore any AZ still isolated before deleting the NACL itself.
        for (const auto& [az, assoc_id] : original_nacl_assoc_by_az) {
            (void)assoc_id;
            restore_az(az);
        }
        if (!deny_all_nacl_id.empty()) {
            Aws::EC2::Model::DeleteNetworkAclRequest req;
            req.SetNetworkAclId(deny_all_nacl_id);
            ec2->DeleteNetworkAcl(req);
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
        if (!vpc_id.empty()) {
            Aws::EC2::Model::DeleteVpcRequest req;
            req.SetVpcId(vpc_id);
            ec2->DeleteVpc(req);
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

    void create_deny_all_nacl() {
        Aws::EC2::Model::CreateNetworkAclRequest req;
        req.SetVpcId(vpc_id);
        auto out = ec2->CreateNetworkAcl(req);
        BOOST_REQUIRE(out.IsSuccess());
        deny_all_nacl_id = std::string(out.GetResult().GetNetworkAcl().GetNetworkAclId());

        auto add_deny = [&](bool egress) {
            Aws::EC2::Model::CreateNetworkAclEntryRequest e;
            e.SetNetworkAclId(deny_all_nacl_id);
            e.SetRuleNumber(1);
            e.SetProtocol("-1");
            e.SetCidrBlock("0.0.0.0/0");
            e.SetEgress(egress);
            e.SetRuleAction(Aws::EC2::Model::RuleAction::deny);
            ec2->CreateNetworkAclEntry(e);
        };
        add_deny(false);
        add_deny(true);
    }

    // Requirement 5.2: swap `az`'s subnet onto the deny-all NACL, saving
    // the original association for restore_az().
    void isolate_az(const std::string& az) {
        const std::string& subnet_id = subnet_by_az.at(az);
        Aws::EC2::Model::DescribeNetworkAclsRequest req;
        Aws::EC2::Model::Filter f;
        f.SetName("association.subnet-id");
        f.AddValues(subnet_id);
        req.AddFilters(f);
        auto out = ec2->DescribeNetworkAcls(req);
        BOOST_REQUIRE(out.IsSuccess());
        const auto& acls = out.GetResult().GetNetworkAcls();
        BOOST_REQUIRE(!acls.empty());
        std::string assoc_id;
        for (const auto& assoc : acls[0].GetAssociations()) {
            if (std::string(assoc.GetSubnetId()) == subnet_id) {
                assoc_id = std::string(assoc.GetNetworkAclAssociationId());
                break;
            }
        }
        BOOST_REQUIRE_MESSAGE(!assoc_id.empty(),
                              "could not find original NACL association for " + az);

        Aws::EC2::Model::ReplaceNetworkAclAssociationRequest replace;
        replace.SetAssociationId(assoc_id);
        replace.SetNetworkAclId(deny_all_nacl_id);
        auto replace_out = ec2->ReplaceNetworkAclAssociation(replace);
        BOOST_REQUIRE(replace_out.IsSuccess());
        original_nacl_assoc_by_az[az] = std::string(replace_out.GetResult().GetNewAssociationId());
    }

    // Requirement 5.4: restore `az`'s subnet to the VPC's default NACL.
    void restore_az(const std::string& az) {
        auto it = original_nacl_assoc_by_az.find(az);
        if (it == original_nacl_assoc_by_az.end()) {
            return;
        }
        const std::string& subnet_id = subnet_by_az.at(az);

        Aws::EC2::Model::DescribeNetworkAclsRequest req;
        {
            Aws::EC2::Model::Filter f;
            f.SetName("vpc-id");
            f.AddValues(vpc_id);
            req.AddFilters(f);
        }
        {
            Aws::EC2::Model::Filter f;
            f.SetName("default");
            f.AddValues("true");
            req.AddFilters(f);
        }
        auto out = ec2->DescribeNetworkAcls(req);
        if (!out.IsSuccess() || out.GetResult().GetNetworkAcls().empty()) {
            original_nacl_assoc_by_az.erase(it);
            return;
        }
        std::string default_nacl_id(out.GetResult().GetNetworkAcls()[0].GetNetworkAclId());
        (void)subnet_id;

        Aws::EC2::Model::ReplaceNetworkAclAssociationRequest replace;
        replace.SetAssociationId(it->second);
        replace.SetNetworkAclId(default_nacl_id);
        ec2->ReplaceNetworkAclAssociation(replace);
        original_nacl_assoc_by_az.erase(it);
    }
};

// Requirement 1.3: writes the unseal key AND the bootstrap credential's
// cert/key to disk via user-data, before ca_cluster_node ever starts —
// extends ca_cluster_node_real_ec2_test.cpp's make_user_data() (which only
// installs the unseal key) with the two extra files this spec's two-phase
// bootstrap needs from boot.
auto make_rpc_tls_user_data(const rpc_tls_three_az_network_fixture& fx) -> std::string {
    std::ostringstream script;
    script << "#!/bin/bash\n"
           << "mkdir -p /var/lib/ca_cluster_node /etc/ca_cluster_node\n"
           << "printf '%s' '" << TEST_UNSEAL_PASSPHRASE << "' > /etc/ca_cluster_node/unseal.key\n"
           << "chmod 600 /etc/ca_cluster_node/unseal.key\n"
           << "cat > /etc/ca_cluster_node/rpc_bootstrap.crt <<'RPC_TLS_CERT_EOF'\n"
           << fx.bootstrap_cred.root_certificate_pem() << "RPC_TLS_CERT_EOF\n"
           << "cat > /etc/ca_cluster_node/rpc_bootstrap.key <<'RPC_TLS_KEY_EOF'\n"
           << detail_testing::unsafe_extract_ca_private_key_pem(fx.bootstrap_cred)
           << "RPC_TLS_KEY_EOF\n"
           << "chmod 600 /etc/ca_cluster_node/rpc_bootstrap.crt "
              "/etc/ca_cluster_node/rpc_bootstrap.key\n";
    return script.str();
}

// Requirement 1.4: extends ca_cluster_node_real_ec2_test.cpp's
// start_node_command() with --rpc-tls-cert/--rpc-tls-key (when
// use_rpc_tls_flags) and generous Raft timing (Requirement 2.5) —
// cross-AZ EC2 round trips plus a real TLS handshake per RPC call are not
// faster than the contended CI container ca_cluster_node_rpc_tls_test.cpp
// was originally tuned against.
auto start_node_command(std::uint64_t node_id, const std::string& peers_arg, bool bootstrap,
                        bool use_rpc_tls_flags) -> std::string {
    std::ostringstream cmd;
    cmd << "nohup /usr/local/bin/ca_cluster_node --node-id " << node_id
        << " --rpc-port 7000 --http-port 8443 --data-dir /var/lib/ca_cluster_node"
        << " --unseal-key-file /etc/ca_cluster_node/unseal.key"
        << " --peers " << peers_arg << " --auth-token " << TEST_AUTH_TOKEN
        << " --election-timeout-min-ms 3000 --election-timeout-max-ms 5000"
        << " --heartbeat-interval-ms 500 --rpc-timeout-ms 5000"
        << (bootstrap ? " --bootstrap-ca" : "");
    if (use_rpc_tls_flags) {
        cmd << " --rpc-tls-cert /etc/ca_cluster_node/rpc_bootstrap.crt"
            << " --rpc-tls-key /etc/ca_cluster_node/rpc_bootstrap.key";
    }
    cmd << " > /var/log/ca_cluster_node.log 2>&1 < /dev/null &\ndisown\n";
    return cmd.str();
}

// Requirement 4.2: stop just the process, not the instance — a plain
// SIGTERM-by-name kill, mirroring the local multi-process test's
// posix_spawn-tracked SIGTERM but over SSH since there's no local pid.
void stop_node_process(const std::string& public_ip, const std::string& private_key_pem) {
    ssh_execute(public_ip, private_key_pem, "pkill -TERM -f /usr/local/bin/ca_cluster_node || true",
                std::chrono::seconds(30));
    std::this_thread::sleep_for(std::chrono::seconds(2));
}

auto wait_healthy(const std::string& public_ip, const std::string& private_key_pem,
                  std::chrono::seconds timeout) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto out = ssh_execute(public_ip, private_key_pem,
                               "curl -sf -o /dev/null -w '%{http_code}' "
                               "http://localhost:8443/healthz",
                               std::chrono::seconds(15));
        if (out == "200") return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

// Polls each node's /v1/root-ca until one answers 200 (i.e. is currently
// leader) — mirrors the local test's find_leader(), executed over SSH.
auto find_leader_ip(const std::vector<std::string>& public_ips, const std::string& private_key_pem,
                    std::chrono::seconds timeout) -> std::optional<std::string> {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& ip : public_ips) {
            auto out =
                ssh_execute(ip, private_key_pem,
                            "curl -sf -o /dev/null -w '%{http_code}' "
                            "-H 'Authorization: Bearer " +
                                std::string(TEST_AUTH_TOKEN) + "' http://localhost:8443/v1/root-ca",
                            std::chrono::seconds(15));
            if (out == "200") return ip;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return std::nullopt;
}

// Issues one certificate against whichever node is currently leader,
// re-resolving the leader on each attempt in case of an in-flight
// failover — returns true iff an issuance ultimately succeeded (200).
// The CSR is generated locally (this test process); the request body,
// once boost::json-serialized, is a single line (JSON string-escapes
// embedded PEM newlines as literal `\n`, not raw newlines, and PEM/JSON
// content here never contains a single quote), so it can be smuggled
// through the SSH command line via a single-quoted `printf '%s'` without
// needing base64 or a heredoc.
auto try_issue_certificate(const std::vector<std::string>& public_ips,
                           const std::string& private_key_pem, std::chrono::seconds timeout)
    -> bool {
    auto leader_ip = find_leader_ip(public_ips, private_key_pem, timeout);
    if (!leader_ip.has_value()) return false;

    leaf_certificate_options opts;
    opts.subject.common_name = "rpc-tls-real-ec2-test-client";
    opts.dns_names = {"rpc-tls-real-ec2-test-client.example.com"};
    auto csr = generate_key_and_csr(opts);

    boost::json::object body;
    body["csr_pem"] = csr.csr_pem;
    body["dns_names"] =
        boost::json::array{boost::json::string("rpc-tls-real-ec2-test-client.example.com")};
    auto json_body = boost::json::serialize(body);

    auto cmd = "printf '%s' '" + json_body +
               "' | curl -sf -o /dev/null -w '%{http_code}' -X POST "
               "-H 'Authorization: Bearer " +
               std::string(TEST_AUTH_TOKEN) +
               "' -H 'Content-Type: application/json' --data-binary @- "
               "http://localhost:8443/v1/certificates";
    auto out = ssh_execute(*leader_ip, private_key_pem, cmd, std::chrono::seconds(30));
    return out == "200";
}

// Polls try_issue_certificate() repeatedly over `timeout` and returns true
// once any attempt succeeds — used where the cluster's convergence state
// isn't otherwise directly observable (no HTTP-exposed
// rpc_tls_ready_node_ids(), matching the local test's identical approach).
auto wait_issuance_capable(const std::vector<std::string>& public_ips,
                           const std::string& private_key_pem, std::chrono::seconds timeout)
    -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (try_issue_certificate(public_ips, private_key_pem, std::chrono::seconds(20))) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    return false;
}

// Deletes the bootstrap credential's cert/key on `public_ip` — the
// property this whole spec exists to prove: the cluster keeps operating
// normally once these files are gone, provided cutover already finalized.
void delete_bootstrap_credential(const std::string& public_ip, const std::string& private_key_pem) {
    ssh_execute(public_ip, private_key_pem,
                "rm -f /etc/ca_cluster_node/rpc_bootstrap.crt "
                "/etc/ca_cluster_node/rpc_bootstrap.key",
                std::chrono::seconds(30));
}

// Builds the shared --peers list for however many public IPs are given so
// far — node ids are 1-based in launch order, matching the local test's
// convention.
auto build_peers_arg(const std::vector<std::string>& public_ips) -> std::string {
    std::ostringstream peers;
    for (std::size_t i = 0; i < public_ips.size(); ++i) {
        if (i > 0) peers << ",";
        peers << (i + 1) << ":" << public_ips[i] << ":7000@http://" << public_ips[i] << ":8443";
    }
    return peers.str();
}

}  // namespace

// Requirement 2: core end-to-end property — three real EC2 instances, one
// per AZ, launched with only the bootstrap credential; confirms the
// cluster forms, bootstraps its CA root, and issues a certificate, then
// deletes the bootstrap credential on all three instances and confirms a
// further issuance still succeeds.
BOOST_FIXTURE_TEST_CASE(bootstrap_and_cutover_survives_bootstrap_credential_deletion,
                        rpc_tls_three_az_network_fixture, *boost::unit_test::timeout(1700)) {
    std::string ami = env("KYTHIRA_EC2_TEST_AMI");

    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "ca-cluster-rpc-tls-real-ec2-test";
    cfg.image_id = ami;
    cfg.node_port = 7000;
    cfg.aws.region = region();
    cfg.security_group_ids = {sg_id};
    cfg.user_data_template = make_rpc_tls_user_data(*this);
    for (const auto& [az, subnet_id] : subnet_by_az) {
        cfg.topology.groups.push_back({.group_id = az, .target_count = 1});
        cfg.subnet_by_group[az] = subnet_id;
    }

    kythira::aws_ec2_quorum_manager<> mgr{cfg};

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    std::vector<std::string> public_ips;
    for (const auto& [az, subnet_id] : subnet_by_az) {
        (void)subnet_id;
        auto peer = mgr.provision_node(az, std::nullopt).get();
        cluster.push_back({.node_id = peer.node_id, .group_id = az});
        auto ip = public_ip_of(kythira::aws_ec2_quorum_manager<>::node_id_to_ec2_id(peer.node_id));
        BOOST_REQUIRE_MESSAGE(!ip.empty(), "no public IP found for node " << peer.node_id);
        public_ips.push_back(ip);
        track_instance("node " + std::to_string(peer.node_id) + " (" + az + ")", cfg.instance_type);
    }
    BOOST_REQUIRE_EQUAL(cluster.size(), 3u);

    std::string peers_arg = build_peers_arg(public_ips);
    for (std::size_t i = 0; i < public_ips.size(); ++i) {
        auto cmd = start_node_command(i + 1, peers_arg, /*bootstrap=*/i == 0,
                                      /*use_rpc_tls_flags=*/true);
        ssh_execute(public_ips[i], private_key_pem, cmd, std::chrono::minutes(3));
    }

    for (const auto& ip : public_ips) {
        BOOST_REQUIRE_MESSAGE(wait_healthy(ip, private_key_pem, std::chrono::minutes(3)),
                              "node at " << ip << " never became healthy");
    }

    BOOST_REQUIRE_MESSAGE(
        try_issue_certificate(public_ips, private_key_pem, std::chrono::minutes(3)),
        "certificate issuance failed before bootstrap credential deletion");

    // Give the cluster time to fully converge (all three rpc_tls_ready,
    // cutover finalized) before deleting the credential — no direct API for
    // this, so poll via repeated successful issuances, matching the local
    // test's approach.
    BOOST_REQUIRE_MESSAGE(
        wait_issuance_capable(public_ips, private_key_pem, std::chrono::minutes(5)),
        "cluster never reached a stable, issuance-capable state");

    for (const auto& ip : public_ips) {
        delete_bootstrap_credential(ip, private_key_pem);
    }

    BOOST_REQUIRE_MESSAGE(
        try_issue_certificate(public_ips, private_key_pem, std::chrono::minutes(3)),
        "certificate issuance failed after deleting the bootstrap credential post-cutover");

    for (const auto& p : cluster) {
        BOOST_CHECK_NO_THROW(mgr.decommission_node(p.node_id).get());
    }
}

// Requirement 3 (Property 3, staggered finalization): all three EC2
// instances are provisioned upfront (so every address is known and every
// node's --peers lists all three from the start — ca_cluster_node has no
// built-in peer discovery, and an unreachable peer address is simply
// retried, not an error), but the third instance's ca_cluster_node
// *process* is deliberately not started until the first two have already
// reached a 2-of-3 issuance-capable state. This mirrors the local
// ca_cluster_node_rpc_tls_test.cpp's own approach exactly (there, ports —
// this test's analogue of "address" — are likewise reserved upfront via
// find_free_port() before any process starts, and only the third
// *process's* start is staggered).
BOOST_FIXTURE_TEST_CASE(staggered_third_node_join_maintains_connectivity,
                        rpc_tls_three_az_network_fixture, *boost::unit_test::timeout(1700)) {
    std::string ami = env("KYTHIRA_EC2_TEST_AMI");

    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "ca-cluster-rpc-tls-staggered-test";
    cfg.image_id = ami;
    cfg.node_port = 7000;
    cfg.aws.region = region();
    cfg.security_group_ids = {sg_id};
    cfg.user_data_template = make_rpc_tls_user_data(*this);
    std::vector<std::string> azs;
    for (const auto& [az, subnet_id] : subnet_by_az) {
        cfg.topology.groups.push_back({.group_id = az, .target_count = 1});
        cfg.subnet_by_group[az] = subnet_id;
        azs.push_back(az);
    }
    BOOST_REQUIRE_EQUAL(azs.size(), 3u);

    kythira::aws_ec2_quorum_manager<> mgr{cfg};

    // Provision all three instances upfront — user-data installs the
    // unseal key and bootstrap credential and returns, but does not start
    // ca_cluster_node itself (matching make_rpc_tls_user_data()'s own
    // contract, identical to the base real-EC2 test's make_user_data()).
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    std::vector<std::string> public_ips;
    for (const auto& az : azs) {
        auto peer = mgr.provision_node(az, std::nullopt).get();
        cluster.push_back({.node_id = peer.node_id, .group_id = az});
        auto ip = public_ip_of(kythira::aws_ec2_quorum_manager<>::node_id_to_ec2_id(peer.node_id));
        BOOST_REQUIRE_MESSAGE(!ip.empty(), "no public IP for node " << peer.node_id);
        public_ips.push_back(ip);
        track_instance("node " + std::to_string(peer.node_id) + " (" + az + ")", cfg.instance_type);
    }
    std::string peers_arg = build_peers_arg(public_ips);

    // Start only nodes 1 and 2 — node 3's instance is running (its own
    // maintenance/Raft-irrelevant boot already happened) but its
    // ca_cluster_node process has not.
    for (std::size_t i = 0; i < 2; ++i) {
        auto cmd = start_node_command(i + 1, peers_arg, /*bootstrap=*/i == 0,
                                      /*use_rpc_tls_flags=*/true);
        ssh_execute(public_ips[i], private_key_pem, cmd, std::chrono::minutes(3));
    }
    for (std::size_t i = 0; i < 2; ++i) {
        BOOST_REQUIRE_MESSAGE(wait_healthy(public_ips[i], private_key_pem, std::chrono::minutes(3)),
                              "first two nodes never became healthy");
    }

    std::vector<std::string> first_two_ips{public_ips[0], public_ips[1]};
    BOOST_REQUIRE_MESSAGE(
        try_issue_certificate(first_two_ips, private_key_pem, std::chrono::minutes(3)),
        "certificate issuance failed with only 2 of 3 nodes up");

    // Now start the third, staggered node.
    auto cmd3 = start_node_command(3, peers_arg, /*bootstrap=*/false, /*use_rpc_tls_flags=*/true);
    ssh_execute(public_ips[2], private_key_pem, cmd3, std::chrono::minutes(3));
    BOOST_REQUIRE_MESSAGE(wait_healthy(public_ips[2], private_key_pem, std::chrono::minutes(3)),
                          "third (staggered) node never became healthy");

    // Connectivity must hold throughout the staggered window.
    for (int i = 0; i < 3; ++i) {
        BOOST_REQUIRE_MESSAGE(
            try_issue_certificate(public_ips, private_key_pem, std::chrono::minutes(2)),
            "certificate issuance failed during staggered finalization window, attempt " << i);
    }

    BOOST_REQUIRE_MESSAGE(
        wait_issuance_capable(public_ips, private_key_pem, std::chrono::minutes(5)),
        "cluster never reached a stable, issuance-capable state after staggered join");

    for (const auto& p : cluster) {
        BOOST_CHECK_NO_THROW(mgr.decommission_node(p.node_id).get());
    }
}

// Requirement 4 (Property 5): a fully-cutover node restarts with neither
// --rpc-tls-cert nor --rpc-tls-key, relying solely on its persisted peer
// certificate under --data-dir, and rejoins successfully.
BOOST_FIXTURE_TEST_CASE(restarted_node_rejoins_without_bootstrap_credential,
                        rpc_tls_three_az_network_fixture, *boost::unit_test::timeout(1700)) {
    std::string ami = env("KYTHIRA_EC2_TEST_AMI");

    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "ca-cluster-rpc-tls-restart-test";
    cfg.image_id = ami;
    cfg.node_port = 7000;
    cfg.aws.region = region();
    cfg.security_group_ids = {sg_id};
    cfg.user_data_template = make_rpc_tls_user_data(*this);
    std::vector<std::string> azs;
    for (const auto& [az, subnet_id] : subnet_by_az) {
        cfg.topology.groups.push_back({.group_id = az, .target_count = 1});
        cfg.subnet_by_group[az] = subnet_id;
        azs.push_back(az);
    }

    kythira::aws_ec2_quorum_manager<> mgr{cfg};

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    std::vector<std::string> public_ips;
    for (const auto& az : azs) {
        auto peer = mgr.provision_node(az, std::nullopt).get();
        cluster.push_back({.node_id = peer.node_id, .group_id = az});
        auto ip = public_ip_of(kythira::aws_ec2_quorum_manager<>::node_id_to_ec2_id(peer.node_id));
        BOOST_REQUIRE_MESSAGE(!ip.empty(), "no public IP for node " << peer.node_id);
        public_ips.push_back(ip);
        track_instance("node " + std::to_string(peer.node_id) + " (" + az + ")", cfg.instance_type);
    }

    std::string peers_arg = build_peers_arg(public_ips);
    for (std::size_t i = 0; i < public_ips.size(); ++i) {
        auto cmd = start_node_command(i + 1, peers_arg, /*bootstrap=*/i == 0,
                                      /*use_rpc_tls_flags=*/true);
        ssh_execute(public_ips[i], private_key_pem, cmd, std::chrono::minutes(3));
    }
    for (const auto& ip : public_ips) {
        BOOST_REQUIRE_MESSAGE(wait_healthy(ip, private_key_pem, std::chrono::minutes(3)),
                              "node never became healthy");
    }

    BOOST_REQUIRE_MESSAGE(
        wait_issuance_capable(public_ips, private_key_pem, std::chrono::minutes(5)),
        "cluster never reached a stable, issuance-capable state");

    // Target the third node (index 2) — confirm its persisted peer
    // certificate exists before proceeding, proving cutover actually
    // happened for it, not merely that the cluster as a whole is healthy.
    const std::string& target_ip = public_ips[2];
    auto peer_cert_check =
        ssh_execute(target_ip, private_key_pem,
                    "test -f /var/lib/ca_cluster_node/rpc_peer_cert.pem && echo present || echo "
                    "missing",
                    std::chrono::seconds(30));
    BOOST_REQUIRE_MESSAGE(peer_cert_check.find("present") != std::string::npos,
                          "target node's persisted peer certificate not found before restart");

    stop_node_process(target_ip, private_key_pem);
    delete_bootstrap_credential(target_ip, private_key_pem);

    auto restart_cmd = start_node_command(3, peers_arg, /*bootstrap=*/false,
                                          /*use_rpc_tls_flags=*/false);
    ssh_execute(target_ip, private_key_pem, restart_cmd, std::chrono::minutes(3));

    BOOST_REQUIRE_MESSAGE(wait_healthy(target_ip, private_key_pem, std::chrono::minutes(3)),
                          "restarted node never became healthy again");
    BOOST_REQUIRE_MESSAGE(
        try_issue_certificate(public_ips, private_key_pem, std::chrono::minutes(3)),
        "certificate issuance failed after restarting the target node without the bootstrap "
        "credential");

    for (const auto& p : cluster) {
        BOOST_CHECK_NO_THROW(mgr.decommission_node(p.node_id).get());
    }
}

// Requirement 5: one non-leader node's AZ subnet is isolated (deny-all
// NACL) mid-cutover; confirms the remaining two nodes retain majority and
// keep issuing certificates, then restores the subnet and confirms the
// isolated node rejoins on its own — no process restart — once
// connectivity returns.
BOOST_FIXTURE_TEST_CASE(network_isolation_during_cutover_recovers, rpc_tls_three_az_network_fixture,
                        *boost::unit_test::timeout(1700)) {
    std::string ami = env("KYTHIRA_EC2_TEST_AMI");

    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "ca-cluster-rpc-tls-isolation-test";
    cfg.image_id = ami;
    cfg.node_port = 7000;
    cfg.aws.region = region();
    cfg.security_group_ids = {sg_id};
    cfg.user_data_template = make_rpc_tls_user_data(*this);
    std::vector<std::string> azs;
    for (const auto& [az, subnet_id] : subnet_by_az) {
        cfg.topology.groups.push_back({.group_id = az, .target_count = 1});
        cfg.subnet_by_group[az] = subnet_id;
        azs.push_back(az);
    }

    kythira::aws_ec2_quorum_manager<> mgr{cfg};

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    std::vector<std::string> public_ips;
    for (const auto& az : azs) {
        auto peer = mgr.provision_node(az, std::nullopt).get();
        cluster.push_back({.node_id = peer.node_id, .group_id = az});
        auto ip = public_ip_of(kythira::aws_ec2_quorum_manager<>::node_id_to_ec2_id(peer.node_id));
        BOOST_REQUIRE_MESSAGE(!ip.empty(), "no public IP for node " << peer.node_id);
        public_ips.push_back(ip);
        track_instance("node " + std::to_string(peer.node_id) + " (" + az + ")", cfg.instance_type);
    }

    std::string peers_arg = build_peers_arg(public_ips);
    for (std::size_t i = 0; i < public_ips.size(); ++i) {
        auto cmd = start_node_command(i + 1, peers_arg, /*bootstrap=*/i == 0,
                                      /*use_rpc_tls_flags=*/true);
        ssh_execute(public_ips[i], private_key_pem, cmd, std::chrono::minutes(3));
    }
    for (const auto& ip : public_ips) {
        BOOST_REQUIRE_MESSAGE(wait_healthy(ip, private_key_pem, std::chrono::minutes(3)),
                              "node never became healthy");
    }
    BOOST_REQUIRE_MESSAGE(
        wait_issuance_capable(public_ips, private_key_pem, std::chrono::minutes(5)),
        "cluster never reached a stable, issuance-capable state before isolation");

    // Isolate the third node's AZ (never the bootstrap node in azs[0], so
    // a majority — nodes 1/2 — always remains regardless of which of the
    // three is currently leader).
    const std::string& isolated_az = azs[2];
    isolate_az(isolated_az);

    std::vector<std::string> surviving_ips{public_ips[0], public_ips[1]};
    BOOST_REQUIRE_MESSAGE(
        try_issue_certificate(surviving_ips, private_key_pem, std::chrono::minutes(2)),
        "certificate issuance failed using only the two non-isolated nodes");
    // Issue a second one to confirm the majority stays healthy, not just
    // survives one lucky attempt.
    BOOST_REQUIRE_MESSAGE(
        try_issue_certificate(surviving_ips, private_key_pem, std::chrono::minutes(2)),
        "certificate issuance failed on a second attempt during isolation");

    restore_az(isolated_az);

    // No process restart on the previously-isolated node — its own
    // maintenance thread's existing retry behavior is what's under test
    // here, distinct from restarted_node_rejoins_without_bootstrap_credential's
    // restart path.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
    bool rejoined = false;
    while (std::chrono::steady_clock::now() < deadline) {
        auto out =
            ssh_execute(public_ips[2], private_key_pem,
                        "curl -sf -o /dev/null -w '%{http_code}' "
                        "-H 'Authorization: Bearer " +
                            std::string(TEST_AUTH_TOKEN) + "' http://localhost:8443/v1/root-ca",
                        std::chrono::seconds(15));
        if (out == "200") {
            rejoined = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    BOOST_REQUIRE_MESSAGE(rejoined,
                          "previously-isolated node's own /v1/root-ca never became reachable "
                          "again after restoring its network");

    for (const auto& p : cluster) {
        BOOST_CHECK_NO_THROW(mgr.decommission_node(p.node_id).get());
    }
}

#endif  // LIBSSH2_FOUND
#endif  // KYTHIRA_HAS_AWS_SDK
#endif  // KYTHIRA_AWS_REAL_EC2_TESTS
