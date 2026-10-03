// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Real-EC2 coverage for the 3-AZ ca_cluster_node AWS deployment
// (Requirement 17.12(b), certificate-authority task 31). Unlike
// ca_cluster_node_localstack_test.cpp (which can only verify the
// provisioning API, since LocalStack's EC2 API is a control-plane mock that
// never boots real software), this launches three REAL EC2 instances
// running the real ca_cluster_node binary and drives them over SSH through
// the failure the 3-AZ layout exists for:
//
//   1. the three nodes form a cluster and bootstrap one CA root;
//   2. the leader issues a certificate;
//   3. the leader's instance is terminated outside the manager;
//   4. the two survivors keep quorum: a new leader with the same root, and
//      a second issuance that can only commit with both of them;
//   5. aws_ec2_quorum_manager::maintain_quorum() replaces the lost instance
//      in the same AZ (subnet and kythira:group tag checked);
//   6. the replacement takes over the lost node's Raft id with an empty
//      data dir and catches up by ordinary Raft replication — it is never
//      bootstrapped, so a root it serves can only have come from its peers;
//   7. every node, the replacement included, is made leader in turn and
//      must still hold both issuances in its own ledger and serve the
//      original root (Property 17 on real infrastructure).
//
// ca_cluster_node takes a static --peers list and the replacement comes up
// on a new address, so step 6 restarts each survivor (one at a time, from
// its own data dir) with the updated list. That is the operator action
// docker/ca_cluster_node/README.md's Path 3 needs after a replacement too.
//
// Ledger presence is read through POST /v1/certificates/revoke, the same
// probe ca_cluster_node_test.cpp uses: the leader answers 404
// serial_not_found iff its ledger lacks the serial, and revoking an
// already-revoked serial is a no-op success. The routes are leader-only, so
// reading a given node's ledger means making it leader first.
//
// Requires (all via environment variables, following the convention already
// established by aws_quorum_manager_real_ec2_test.cpp):
//   KYTHIRA_EC2_TEST_AMI        AMI ID with /usr/local/bin/ca_cluster_node
//                               installed — build one with
//                               packer/ca_cluster_node/scripts/build.sh
//                               (see packer/ca_cluster_node/README.md)
//   AWS credentials via the standard provider chain; AWS_REGION or a default
//   region configured in aws_client_config
//
// Not run by default (LABELS real-ec2;slow) — same gating as the existing
// aws_quorum_manager_real_ec2_test.cpp. Real per-run AWS cost: four
// t3.micro/t4g.micro instances for roughly twenty minutes.

#define BOOST_TEST_MODULE ca_cluster_node_real_ec2_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_AWS_REAL_EC2_TESTS
#ifdef KYTHIRA_HAS_AWS_SDK
#ifdef LIBSSH2_FOUND

#include <raft/aws_ec2_quorum_manager.hpp>

#include <aws/core/Aws.h>
#include <aws/core/utils/UUID.h>
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
#include <aws/ec2/model/CreateTagsRequest.h>
#include <aws/ec2/model/CreateVpcRequest.h>
#include <aws/ec2/model/DeleteInternetGatewayRequest.h>
#include <aws/ec2/model/DeleteKeyPairRequest.h>
#include <aws/ec2/model/DeleteRouteTableRequest.h>
#include <aws/ec2/model/DeleteSecurityGroupRequest.h>
#include <aws/ec2/model/DeleteSubnetRequest.h>
#include <aws/ec2/model/DeleteVpcRequest.h>
#include <aws/ec2/model/DescribeInstanceStatusRequest.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/DetachInternetGatewayRequest.h>
#include <aws/ec2/model/DisassociateAddressRequest.h>
#include <aws/ec2/model/IpPermission.h>
#include <aws/ec2/model/IpRange.h>
#include <aws/ec2/model/ModifySubnetAttributeRequest.h>
#include <aws/ec2/model/ReleaseAddressRequest.h>
#include <aws/ec2/model/AssociateRouteTableRequest.h>
#include <aws/ec2/model/Tag.h>
#include <aws/ec2/model/TerminateInstancesRequest.h>
#include <aws/sts/STSClient.h>
#include <aws/sts/model/GetCallerIdentityRequest.h>

#include <libssh2.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

#endif
#include "aws_real_ec2_test_support.hpp"

#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>

#include <boost/json.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
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
// The unseal passphrase every launched node is configured with — a fixed
// test value (Requirement 17.4 only requires byte-identical across nodes,
// not secrecy for a throwaway test cluster torn down at the end of the run).
constexpr const char* TEST_UNSEAL_PASSPHRASE = "kythira-real-ec2-test-unseal-passphrase";
constexpr const char* TEST_AUTH_TOKEN = "kythira-real-ec2-test-auth-token";

auto env(const char* name) -> std::string {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string{};
}

auto region() -> std::string {
    auto r = env("AWS_REGION");
    return r.empty() ? DUMMY_REGION_FALLBACK : r;
}

// Run once before any other global fixture constructs: if
// KYTHIRA_EC2_TEST_AMI isn't set (three_az_network_fixture's own second
// "skip: ..." condition, thrown from its constructor), exit immediately
// with the reserved code tests/CMakeLists.txt registers via
// SKIP_RETURN_CODE, so `ctest` reports this test as "Not Run" instead of
// "Failed" when no real AWS environment is configured. Doesn't replace the
// fixture's own checks (kept as-is, including its STS reachability check,
// which needs a real network round trip this cheap env-var-only check
// deliberately avoids).
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
        if (env("KYTHIRA_EC2_TEST_AMI").empty()) {
            std::cerr << "SKIP: KYTHIRA_EC2_TEST_AMI not set\n";
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

BOOST_GLOBAL_FIXTURE(PreflightSkipFixture);
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif
BOOST_GLOBAL_FIXTURE(AwsSdkFixture);

// BOOST_GLOBAL_FIXTURE forms an identifier from its argument, so a
// namespace-qualified type must be brought into scope unqualified first.
using kythira::testing::aws_real_ec2::AwsSignalHandlerFixture;
using kythira::testing::aws_real_ec2::CostSummaryFixture;
BOOST_GLOBAL_FIXTURE(CostSummaryFixture);
BOOST_GLOBAL_FIXTURE(AwsSignalHandlerFixture);

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

// Tag helper mirroring aws_quorum_manager_real_ec2_test.cpp's. The post-run
// leak audit in .github/workflows/real-cloud-tests.yml attributes a leaked
// network by its `kyt-*-vpc` Name tag; until this fixture tagged anything that
// filter was blind to every VPC the test leaked, and the audit had to carry a
// duplicate of the hardcoded CIDR below to see them at all.
auto make_tag(const std::string& k, const std::string& v) -> Aws::EC2::Model::Tag {
    Aws::EC2::Model::Tag t;
    t.SetKey(k);
    t.SetValue(v);
    return t;
}

// Minimal public-subnet-per-AZ VPC (Internet Gateway + route table, no NAT/
// bastion complexity) — sufficient for a short-lived test cluster reached
// directly via public IP + a security group scoped to the test runner's
// own IP for both SSH and the ca_cluster_node HTTP port.
struct three_az_network_fixture : kythira::testing::aws_real_ec2::signal_cleanup_target {
    std::shared_ptr<Aws::EC2::EC2Client> ec2;
    std::string vpc_id, igw_id, route_table_id, sg_id, key_name;
    // `kyt-` prefix is what the post-run leak audit's Name filter matches.
    std::string uuid;
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

    // Tag every network resource as it is created, so a leak the audit finds
    // names the test that produced it instead of an anonymous VPC. Best-effort:
    // a CreateTags failure must not fail the test, and the audit's CIDR filter
    // remains the backstop for a VPC that dies between CreateVpc and CreateTags.
    void tag(const std::string& resource_id, const std::string& k, const std::string& v) {
        Aws::EC2::Model::CreateTagsRequest req;
        req.AddResources(resource_id);
        req.AddTags(make_tag(k, v));
        ec2->CreateTags(req);
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

        const Aws::String random_uuid = Aws::Utils::UUID::RandomUUID();
        uuid = "kyt-" + std::string(random_uuid.c_str());

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
            tag(vpc_id, "Name", uuid + "-vpc");
            tag(vpc_id, "kythira:managed-by", "ca_cluster_node_real_ec2_test");

            Aws::EC2::Model::ModifySubnetAttributeRequest unused;
            (void)unused;

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
                sn_req.SetCidrBlock("10.220." + std::to_string(octet++) + ".0/24");
                sn_req.SetAvailabilityZone(az);
                auto sn_out = ec2->CreateSubnet(sn_req);
                BOOST_REQUIRE_MESSAGE(sn_out.IsSuccess(), "CreateSubnet(" + az + ")");
                std::string subnet_id = std::string(sn_out.GetResult().GetSubnet().GetSubnetId());
                subnet_by_az[az] = subnet_id;
                tag(subnet_id, "Name", uuid + "-" + az);

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
            tag(sg_id, "Name", uuid + "-sg");

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
        << " --peers " << peers_arg << " --auth-token "
        << TEST_AUTH_TOKEN
        // Plaintext across hosts needs the explicit opt-in, for Raft RPC and
        // for the client API alike (the test VPC is private to the run); the
        // RPC TLS variant of this deployment is
        // ca_cluster_node_rpc_tls_real_ec2_test.
        << " --allow-plaintext-rpc --allow-plaintext-http" << (bootstrap ? " --bootstrap-ca" : "")
        << " > /tmp/ca_cluster_node.log 2>&1 < /dev/null &\ndisown\n";
    return cmd.str();
}

// Builds the shared --peers list. Raft node ids are 1-based positions in
// `public_ips`, so a replacement that takes over id k only changes entry k.
//
// start_node_command() passes no --tls-cert/--tls-key, so ca_cluster_node's
// client-facing listener falls back to plain HTTP (with its own "running
// without TLS" warning) — the peers URL scheme and the curl checks below
// must match what's actually listening, not what a production deployment
// would use.
auto build_peers_arg(const std::vector<std::string>& public_ips) -> std::string {
    std::ostringstream peers;
    for (std::size_t i = 0; i < public_ips.size(); ++i) {
        if (i > 0) {
            peers << ",";
        }
        peers << (i + 1) << ":" << public_ips[i] << ":7000@http://" << public_ips[i] << ":8443";
    }
    return peers.str();
}

struct http_reply {
    int status = 0;
    std::string body;
};

// Runs curl against the node's own HTTP port over SSH and splits off the
// status code curl appends on its own last line. Over SSH rather than from
// the runner for the reason given at the leader wait in the test body.
auto curl_on(const std::string& ip, const std::string& private_key_pem,
             const std::string& curl_args) -> http_reply {
    auto out = ssh_execute(ip, private_key_pem,
                           "curl -s -w '\\n%{http_code}' -H 'Authorization: Bearer " +
                               std::string(TEST_AUTH_TOKEN) + "' " + curl_args,
                           std::chrono::seconds(30));
    http_reply r;
    auto nl = out.rfind('\n');
    if (nl == std::string::npos) {
        return r;  // curl itself failed; status 0
    }
    r.body = out.substr(0, nl);
    try {
        r.status = std::stoi(out.substr(nl + 1));
    } catch (const std::exception&) {
        r.status = 0;
    }
    return r;
}

// The leader is the one node that answers /v1/root-ca with 200; followers
// redirect (308) and leaderless nodes answer 503. Returns the leader's index
// into `ips` and the root PEM it served.
struct leader_probe {
    std::size_t index = 0;
    std::string root_pem;
};

auto find_leader(const std::vector<std::string>& ips, const std::string& private_key_pem,
                 const std::set<std::size_t>& skip, std::chrono::seconds timeout)
    -> std::optional<leader_probe> {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (std::size_t i = 0; i < ips.size(); ++i) {
            if (skip.contains(i)) {
                continue;
            }
            auto r = curl_on(ips[i], private_key_pem, "http://localhost:8443/v1/root-ca");
            if (r.status == 200 && !r.body.empty()) {
                return leader_probe{i, r.body};
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    return std::nullopt;
}

// POSTs to a leader-only route, retrying while the answer is transient: 503
// (a just-elected leader whose signer isn't rebuilt yet, or no leader) and
// 502/0 (commit timeout, dropped connection). Returns the last reply.
auto post_json_with_retry(const std::string& ip, const std::string& private_key_pem,
                          const std::string& path, const std::string& json_body,
                          std::chrono::seconds timeout) -> http_reply {
    // A serialized boost::json body is one line (PEM newlines become the
    // two characters \n) and carries no single quote, so it rides the SSH
    // command line inside single quotes.
    const std::string args = "-X POST -H 'Content-Type: application/json' --data-binary '" +
                             json_body + "' http://localhost:8443" + path;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    http_reply r;
    do {
        r = curl_on(ip, private_key_pem, args);
        if (r.status != 0 && r.status != 502 && r.status != 503) {
            return r;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    return r;
}

// Issues a certificate through the leader at `ip` and returns its serial.
auto issue_certificate(const std::string& ip, const std::string& private_key_pem,
                       const std::string& common_name) -> std::uint64_t {
    raft::testing::leaf_certificate_options opts;
    opts.subject.common_name = common_name;
    opts.dns_names = {common_name + ".example.com"};
    auto csr = raft::testing::generate_key_and_csr(opts);

    boost::json::object body;
    body["csr_pem"] = csr.csr_pem;
    body["dns_names"] = boost::json::array{boost::json::string(common_name + ".example.com")};
    auto r = post_json_with_retry(ip, private_key_pem, "/v1/certificates",
                                  boost::json::serialize(body), std::chrono::seconds(180));
    BOOST_REQUIRE_MESSAGE(r.status == 200,
                          "issuing " << common_name << " failed: " << r.status << " " << r.body);

    auto pem =
        std::string(boost::json::parse(r.body).as_object().at("certificate_pem").as_string());
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free);
    BOOST_REQUIRE(cert != nullptr);
    std::uint64_t serial = 0;
    BOOST_REQUIRE(ASN1_INTEGER_get_uint64(&serial, X509_get_serialNumber(cert.get())) == 1);
    return serial;
}

// Revoke-by-serial against the leader at `ip`: 200 iff that leader's ledger
// holds the serial, 404 iff it doesn't (see the file header).
auto ledger_probe(const std::string& ip, const std::string& private_key_pem, std::uint64_t serial)
    -> http_reply {
    boost::json::object body;
    body["serial"] = std::to_string(serial);
    return post_json_with_retry(ip, private_key_pem, "/v1/certificates/revoke",
                                boost::json::serialize(body), std::chrono::seconds(120));
}

// SIGKILL, not SIGTERM: the point is a crash. Matched by process name (-x),
// not -f on the path, because -f would also match the sudo and shell
// running this very command line. The process runs as root (sudo in
// start_node_command), so the kill needs sudo as well.
void kill_node_process(const std::string& ip, const std::string& private_key_pem) {
    ssh_execute(ip, private_key_pem,
                "sudo pkill -KILL -x ca_cluster_node; "
                "for i in $(seq 1 30); do pgrep -x ca_cluster_node >/dev/null || exit 0; "
                "sleep 1; done; exit 1",
                std::chrono::seconds(60));
}

auto wait_healthy(const std::string& ip, const std::string& private_key_pem,
                  std::chrono::seconds timeout) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (curl_on(ip, private_key_pem, "http://localhost:8443/healthz").status == 200) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    return false;
}

// std::cerr, not BOOST_TEST_MESSAGE: this project's real-ec2 tests run
// under Boost.Test's default log level, which does not print message-level
// output at all — confirmed empirically when an earlier BOOST_TEST_MESSAGE
// version of this same diagnostic produced zero output in a real CI run.
// stderr always shows up under ctest --output-on-failure regardless of
// Boost.Test's own log-level filtering.
void dump_node_logs(const std::vector<std::string>& ips, const std::string& private_key_pem) {
    for (const auto& ip : ips) {
        try {
            auto log = ssh_execute(ip, private_key_pem,
                                   "sudo tail -n 80 /tmp/ca_cluster_node.log 2>&1; echo; "
                                   "echo '--- ps ---'; pgrep -a -x ca_cluster_node",
                                   std::chrono::seconds(30));
            std::cerr << "=== " << ip << " ca_cluster_node.log ===\n" << log << "\n";
        } catch (const std::exception& e) {
            std::cerr << "=== " << ip << ": could not fetch log: " << e.what() << "\n";
        }
    }
}

}  // namespace

// Requirement 17.12(b) end-to-end, task 31: see the file header for the
// seven steps. The Boost timeout covers fixture setup and teardown too.
BOOST_FIXTURE_TEST_CASE(three_az_cluster_survives_instance_loss_and_replacement,
                        three_az_network_fixture, *boost::unit_test::timeout(1800)) {
    using manager_t = kythira::aws_ec2_quorum_manager<>;
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

    manager_t mgr{cfg};

    // ── Step 1: form the cluster ────────────────────────────────────────
    // Launch all three instances (subnets have MapPublicIpOnLaunch set, so
    // each gets a public IP automatically). The manager's node identity is
    // the EC2 instance ID, so the public IP comes from DescribeInstances on
    // that ID. Index i here is Raft node id i + 1.
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    std::vector<std::string> public_ips;
    for (const auto& [az, subnet_id] : subnet_by_az) {
        (void)subnet_id;
        auto peer = mgr.provision_node(az, std::nullopt).get();
        cluster.push_back({.node_id = peer.node_id, .group_id = az});
        track_instance("node " + std::to_string(cluster.size()) + " (" + az + ")",
                       cfg.instance_type);
        auto ip = public_ip_of(mgr.instance_id_of(peer.node_id).value());
        BOOST_REQUIRE_MESSAGE(!ip.empty(), "no public IP for " << az);
        public_ips.push_back(ip);
    }
    BOOST_REQUIRE_EQUAL(cluster.size(), 3u);

    // Every public IP is known only now, so the --peers list is too; start
    // ca_cluster_node on each instance over SSH.
    const std::string peers_arg = build_peers_arg(public_ips);
    for (std::size_t i = 0; i < public_ips.size(); ++i) {
        // "running" only means the VM has booted, not that user-data has
        // finished - cloud-init's final stage (which runs
        // make_user_data()'s script, creating the unseal key file) can
        // still be in progress after SSH is already accepting connections.
        // Observed directly on a real run: node 3 hit "cannot open
        // --unseal-key-file" while nodes 1/2 happened to have already
        // finished by the time they were started - a genuine race, not
        // reproducible every time. cloud-init status --wait blocks until
        // the boot-finished stage completes.
        ssh_execute(public_ips[i], private_key_pem, "sudo cloud-init status --wait",
                    std::chrono::minutes(3));
        // --bootstrap-ca on every node, not just node 1: main.cpp's
        // maybe_bootstrap() only does real work on whichever node
        // is_leader() returns true for, and is idempotent afterward
        // (checks has_root_material() before submitting) - Raft guarantees
        // at most one leader at a time, so this can't double-bootstrap.
        // Flagging only node 1 assumed it would win the election, which
        // Raft's own randomized election timeout does not guarantee -
        // observed directly on a real run: node 2 won, node 1 (the only
        // flagged node) never became leader, and the CA was never created
        // at all, so /v1/root-ca never started responding.
        ssh_execute(public_ips[i], private_key_pem,
                    start_node_command(i + 1, peers_arg, /*bootstrap=*/true),
                    std::chrono::minutes(3));
    }

    // Checked over SSH (curl against localhost:8443 on each instance)
    // rather than from the test runner, since the runner's own network path
    // to the instances' HTTP port is not guaranteed even though the
    // security group permits it (e.g. a CI environment without direct
    // internet egress).
    auto leader = find_leader(public_ips, private_key_pem, {}, std::chrono::minutes(5));
    if (!leader) {
        dump_node_logs(public_ips, private_key_pem);
    }
    BOOST_REQUIRE_MESSAGE(leader.has_value(),
                          "no ca_cluster_node leader became reachable within the timeout");
    const std::string root_pem = leader->root_pem;
    std::cerr << "[ca_cluster_node_real_ec2_test] cluster formed; leader is node "
              << leader->index + 1 << " (" << cluster[leader->index].group_id << ")\n";

    // ── Step 2: issue a certificate ─────────────────────────────────────
    const std::uint64_t serial_before =
        issue_certificate(public_ips[leader->index], private_key_pem, "failover-before");

    // ── Step 3: lose the leader's instance ──────────────────────────────
    // Terminated directly, not via mgr.decommission_node(): the manager has
    // to discover the loss through its own assessment, as it would in
    // production. The leader is the harder case for Property 17: the
    // certificate must survive on nodes that only ever followed.
    const std::size_t lost = leader->index;
    const auto lost_placement = cluster[lost];
    const std::string lost_ec2_id = mgr.instance_id_of(lost_placement.node_id).value();
    {
        Aws::EC2::Model::TerminateInstancesRequest term;
        term.AddInstanceIds(lost_ec2_id);
        auto out = ec2->TerminateInstances(term);
        BOOST_REQUIRE_MESSAGE(out.IsSuccess(),
                              "TerminateInstances: " + std::string(out.GetError().GetMessage()));
    }
    auto lost_still_running = [&] {
        Aws::EC2::Model::DescribeInstanceStatusRequest req;
        req.AddInstanceIds(lost_ec2_id);
        req.SetIncludeAllInstances(true);
        auto out = ec2->DescribeInstanceStatus(req);
        if (!out.IsSuccess()) {
            return true;  // unknown; keep waiting
        }
        for (const auto& st : out.GetResult().GetInstanceStatuses()) {
            if (st.GetInstanceState().GetName() == Aws::EC2::Model::InstanceStateName::running) {
                return true;
            }
        }
        return false;
    };
    auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (lost_still_running() && std::chrono::steady_clock::now() < stop_deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    BOOST_REQUIRE_MESSAGE(!lost_still_running(), "terminated leader instance still running");

    // ── Step 4: quorum survives on the remaining two ───────────────────
    auto survivor_leader =
        find_leader(public_ips, private_key_pem, {lost}, std::chrono::minutes(3));
    if (!survivor_leader) {
        dump_node_logs(public_ips, private_key_pem);
    }
    BOOST_REQUIRE_MESSAGE(survivor_leader.has_value(), "no leader among the two survivors");
    BOOST_TEST(survivor_leader->root_pem == root_pem);
    // With one of three gone, this commit needs both survivors.
    const std::uint64_t serial_during =
        issue_certificate(public_ips[survivor_leader->index], private_key_pem, "failover-during");

    // ── Step 5: the manager replaces the lost instance in the same AZ ──
    auto pre = mgr.maintain_quorum(cluster).get();
    BOOST_REQUIRE_EQUAL(pre.unreachable_nodes.size(), 1u);
    BOOST_TEST(pre.unreachable_nodes.front() == lost_placement.node_id);

    std::set<std::string> known;
    for (const auto& p : cluster) {
        known.insert(mgr.instance_id_of(p.node_id).value());
    }
    std::vector<std::string> fresh;
    std::string fresh_subnet, fresh_az, fresh_group;
    {
        Aws::EC2::Model::DescribeInstancesRequest req;
        Aws::EC2::Model::Filter vpc_filter;
        vpc_filter.SetName("vpc-id");
        vpc_filter.AddValues(vpc_id);
        req.AddFilters(vpc_filter);
        Aws::EC2::Model::Filter state_filter;
        state_filter.SetName("instance-state-name");
        state_filter.AddValues("pending");
        state_filter.AddValues("running");
        req.AddFilters(state_filter);
        auto out = ec2->DescribeInstances(req);
        BOOST_REQUIRE(out.IsSuccess());
        for (const auto& res : out.GetResult().GetReservations()) {
            for (const auto& inst : res.GetInstances()) {
                std::string id(inst.GetInstanceId());
                if (known.contains(id)) {
                    continue;
                }
                fresh.push_back(id);
                fresh_subnet = std::string(inst.GetSubnetId());
                fresh_az = std::string(inst.GetPlacement().GetAvailabilityZone());
                for (const auto& tag : inst.GetTags()) {
                    if (tag.GetKey() == "kythira:group") {
                        fresh_group = std::string(tag.GetValue());
                    }
                }
            }
        }
    }
    BOOST_REQUIRE_MESSAGE(fresh.size() == 1u,
                          "expected exactly one replacement instance, found " << fresh.size());
    track_instance(
        "replacement node " + std::to_string(lost + 1) + " (" + lost_placement.group_id + ")",
        cfg.instance_type);
    BOOST_TEST(fresh_az == lost_placement.group_id);
    BOOST_TEST(fresh_subnet == subnet_by_az.at(lost_placement.group_id));
    BOOST_TEST(fresh_group == lost_placement.group_id);

    auto fresh_id = mgr.node_id_of_instance(fresh[0]);
    BOOST_REQUIRE_MESSAGE(fresh_id.has_value(), fresh[0] << " carries no node id");
    cluster[lost] = {.node_id = *fresh_id, .group_id = lost_placement.group_id};
    auto after = mgr.assess_quorum(cluster).get();
    BOOST_TEST(after.live_node_count == 3u);

    // ── Step 6: the replacement rejoins as the lost node's Raft id ─────
    public_ips[lost] = public_ip_of(fresh[0]);
    BOOST_REQUIRE_MESSAGE(!public_ips[lost].empty(), "no public IP for the replacement");
    const std::string new_peers_arg = build_peers_arg(public_ips);
    ssh_execute(public_ips[lost], private_key_pem, "sudo cloud-init status --wait",
                std::chrono::minutes(3));
    // No --bootstrap-ca: any root this node serves must come from its peers.
    ssh_execute(public_ips[lost], private_key_pem,
                start_node_command(lost + 1, new_peers_arg, /*bootstrap=*/false),
                std::chrono::minutes(1));
    BOOST_REQUIRE_MESSAGE(wait_healthy(public_ips[lost], private_key_pem, std::chrono::minutes(2)),
                          "replacement never became healthy");
    // Survivors still address Raft id lost+1 at the dead IP. Restart them
    // one at a time from their own data dirs with the new list; the
    // current leader goes last so the first restart doesn't force an
    // election on top of it.
    std::vector<std::size_t> roll;
    for (std::size_t i = 0; i < public_ips.size(); ++i) {
        if (i != lost && i != survivor_leader->index) {
            roll.push_back(i);
        }
    }
    roll.push_back(survivor_leader->index);
    for (auto i : roll) {
        kill_node_process(public_ips[i], private_key_pem);
        ssh_execute(public_ips[i], private_key_pem,
                    start_node_command(i + 1, new_peers_arg, /*bootstrap=*/false),
                    std::chrono::minutes(1));
        BOOST_REQUIRE_MESSAGE(wait_healthy(public_ips[i], private_key_pem, std::chrono::minutes(2)),
                              "survivor node " << i + 1 << " never became healthy after restart");
    }

    // ── Step 7: every node, as leader, still holds both issuances ──────
    auto current = find_leader(public_ips, private_key_pem, {}, std::chrono::minutes(3));
    if (!current) {
        dump_node_logs(public_ips, private_key_pem);
    }
    BOOST_REQUIRE_MESSAGE(current.has_value(), "no leader after the replacement rejoined");

    // The probe must be able to fail: a serial nobody issued is a 404.
    std::uint64_t never_issued = 1;
    while (never_issued == serial_before || never_issued == serial_during) {
        ++never_issued;
    }
    {
        auto r = ledger_probe(public_ips[current->index], private_key_pem, never_issued);
        BOOST_TEST(r.status == 404, "never-issued serial probe: " << r.status << " " << r.body);
    }

    // Leadership moves by killing the leader's process; whichever of the
    // other two wins is random, so this is bounded by rounds, not
    // scripted. Each round has a 1/2 chance of reaching the last unchecked
    // node, so 12 rounds miss it with probability 1/2048.
    std::set<std::size_t> verified;
    for (int round = 0; round < 12; ++round) {
        // Re-resolved every round: a probe sent to a node that has since
        // lost leadership would get a 308, not a ledger answer.
        current = find_leader(public_ips, private_key_pem, {}, std::chrono::minutes(2));
        if (!current) {
            dump_node_logs(public_ips, private_key_pem);
        }
        BOOST_REQUIRE_MESSAGE(current.has_value(), "no leader in rotation round " << round);
        const auto idx = current->index;
        if (!verified.contains(idx)) {
            BOOST_TEST(current->root_pem == root_pem,
                       "node " << idx + 1 << " serves a different root");
            for (auto serial : {serial_before, serial_during}) {
                auto r = ledger_probe(public_ips[idx], private_key_pem, serial);
                BOOST_TEST(r.status == 200, "node " << idx + 1 << " ledger probe for " << serial
                                                    << ": " << r.status << " " << r.body);
            }
            verified.insert(idx);
            std::cerr << "[ca_cluster_node_real_ec2_test] node " << idx + 1
                      << (idx == lost ? " (replacement)" : "") << " verified as leader\n";
        }
        if (verified.size() == public_ips.size()) {
            break;
        }
        kill_node_process(public_ips[idx], private_key_pem);
        auto next = find_leader(public_ips, private_key_pem, {idx}, std::chrono::minutes(2));
        if (!next) {
            dump_node_logs(public_ips, private_key_pem);
        }
        BOOST_REQUIRE_MESSAGE(next.has_value(), "no new leader after killing node " << idx + 1);
        // Restart only after the new leader is in place, so the restarted
        // node can't simply win the election back.
        ssh_execute(public_ips[idx], private_key_pem,
                    start_node_command(idx + 1, new_peers_arg, /*bootstrap=*/false),
                    std::chrono::minutes(1));
        BOOST_REQUIRE_MESSAGE(
            wait_healthy(public_ips[idx], private_key_pem, std::chrono::minutes(2)),
            "node " << idx + 1 << " never became healthy after restart");
    }
    BOOST_TEST(verified.size() == public_ips.size(),
               "only " << verified.size() << " of 3 nodes were checked as leader");
    BOOST_TEST(verified.contains(lost), "the replacement was never checked as leader");

    for (const auto& p : cluster) {
        BOOST_CHECK_NO_THROW(mgr.decommission_node(p.node_id).get());
    }
}

#endif  // LIBSSH2_FOUND
#endif  // KYTHIRA_HAS_AWS_SDK
#endif  // KYTHIRA_AWS_REAL_EC2_TESTS
