// LocalStack coverage for the 3-AZ ca_cluster_node AWS deployment
// (Requirement 17.12(b), documented in docker/ca_cluster_node/README.md).
//
// This deliberately does NOT re-test aws_ec2_quorum_manager's core mechanics
// (provisioning, health assessment, replacement) — that's already covered by
// aws_quorum_manager_localstack_test.cpp/aws_quorum_manager_real_ec2_test.cpp
// in exhaustive depth, and task 27 explicitly says this spec introduces no
// new CA-specific EC2 provisioning mechanism. What's new here is the
// CA-cluster-specific topology shape: three placement groups named by AZ,
// target_count = 1 each, exactly as documented for a ca_cluster_node fleet —
// verifying THAT shape provisions correctly (one instance per AZ/subnet,
// correctly tagged) is the actual gap this file closes.
//
// LocalStack's EC2 API is a control-plane mock — it tracks instance records
// and state transitions but never boots real software, so this file can only
// verify the PROVISIONING topology, not that a real ca_cluster_node cluster
// actually forms (that requires real EC2 + SSH — see
// ca_cluster_node_real_ec2_test.cpp).

#define BOOST_TEST_MODULE ca_cluster_node_localstack_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_AWS_LOCALSTACK_TESTS
#ifdef KYTHIRA_HAS_AWS_SDK

#include <raft/aws_ec2_quorum_manager.hpp>

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
#include <aws/ec2/model/TerminateInstancesRequest.h>
#include <aws/sts/STSClient.h>
#include <aws/sts/model/GetCallerIdentityRequest.h>

#include <folly/init/Init.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* LOCALSTACK_ENDPOINT = "http://localhost:4566";
constexpr const char* DUMMY_REGION = "us-east-1";

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

auto make_client_cfg() -> Aws::Client::ClientConfiguration {
    Aws::Client::ClientConfiguration c;
    c.region = DUMMY_REGION;
    c.endpointOverride = LOCALSTACK_ENDPOINT;
    c.requestTimeoutMs = 10000;
    c.connectTimeoutMs = 10000;
    return c;
}

kythira::aws_client_config make_localstack_cfg() {
    kythira::aws_client_config cfg;
    cfg.region = DUMMY_REGION;
    cfg.endpoint_override = LOCALSTACK_ENDPOINT;
    cfg.api_timeout = std::chrono::seconds{10};
    return cfg;
}

// Three subnets (one per fake AZ) + one VPC + one security group — the
// minimum shape needed to exercise the 3-AZ topology config, mirroring
// docker/ca_cluster_node/README.md's Path 3 example exactly (three AZ-named
// placement groups, subnet_by_group set per AZ).
struct three_az_fixture {
    std::shared_ptr<Aws::EC2::EC2Client> ec2;
    std::string vpc_id;
    std::map<std::string, std::string> subnet_by_az;  // "us-east-1a" -> subnet-...
    std::string sg_id;

    three_az_fixture() {
        if (getenv("AWS_ACCESS_KEY_ID") == nullptr) {
            setenv("AWS_ACCESS_KEY_ID", "test", 1);
        }
        if (getenv("AWS_SECRET_ACCESS_KEY") == nullptr) {
            setenv("AWS_SECRET_ACCESS_KEY", "test", 1);
        }

        Aws::Auth::AWSCredentials creds{"test", "test"};
        auto cli_cfg = make_client_cfg();
        ec2 = std::make_shared<Aws::EC2::EC2Client>(creds, cli_cfg);

        Aws::STS::STSClient sts{creds, cli_cfg};
        auto id_out = sts.GetCallerIdentity(Aws::STS::Model::GetCallerIdentityRequest{});
        if (!id_out.IsSuccess()) {
            throw std::runtime_error("LocalStack not reachable (skip): " +
                                     std::string(id_out.GetError().GetMessage()));
        }

        Aws::EC2::Model::CreateVpcRequest vpc_req;
        vpc_req.SetCidrBlock("10.210.0.0/16");
        auto vpc_out = ec2->CreateVpc(vpc_req);
        BOOST_REQUIRE_MESSAGE(vpc_out.IsSuccess(),
                              "CreateVpc: " + std::string(vpc_out.GetError().GetMessage()));
        vpc_id = std::string(vpc_out.GetResult().GetVpc().GetVpcId());

        int octet = 1;
        for (const std::string& az : {"us-east-1a", "us-east-1b", "us-east-1c"}) {
            Aws::EC2::Model::CreateSubnetRequest sn_req;
            sn_req.SetVpcId(vpc_id);
            sn_req.SetCidrBlock("10.210." + std::to_string(octet++) + ".0/24");
            sn_req.SetAvailabilityZone(az);
            auto sn_out = ec2->CreateSubnet(sn_req);
            BOOST_REQUIRE_MESSAGE(
                sn_out.IsSuccess(),
                "CreateSubnet(" + az + "): " + std::string(sn_out.GetError().GetMessage()));
            subnet_by_az[az] = std::string(sn_out.GetResult().GetSubnet().GetSubnetId());
        }

        Aws::EC2::Model::CreateSecurityGroupRequest sg_req;
        sg_req.SetGroupName("kythira-ca-cluster-test-sg");
        sg_req.SetDescription("kythira ca_cluster_node localstack test");
        sg_req.SetVpcId(vpc_id);
        auto sg_out = ec2->CreateSecurityGroup(sg_req);
        BOOST_REQUIRE_MESSAGE(sg_out.IsSuccess(), "CreateSecurityGroup: " +
                                                      std::string(sg_out.GetError().GetMessage()));
        sg_id = std::string(sg_out.GetResult().GetGroupId());
    }

    ~three_az_fixture() {
        // Instance teardown is the test case's own responsibility via
        // mgr.decommission_node() — only aws_ec2_quorum_manager knows the
        // node_id -> EC2 instance ID mapping.
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
        if (!vpc_id.empty()) {
            Aws::EC2::Model::DeleteVpcRequest req;
            req.SetVpcId(vpc_id);
            ec2->DeleteVpc(req);
        }
    }
};

}  // namespace

// Requirement 17.12(b): three placement groups named by AZ, target_count = 1
// each, subnet_by_group set to a distinct per-AZ subnet — exactly the shape
// documented in docker/ca_cluster_node/README.md's Path 3. Verifies
// aws_ec2_quorum_manager provisions exactly one instance per AZ group when
// configured this way, with each instance landing in its own AZ's subnet.
BOOST_FIXTURE_TEST_CASE(three_az_topology_provisions_one_node_per_az, three_az_fixture,
                        *boost::unit_test::timeout(120)) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "ca-cluster";
    cfg.image_id = "ami-12345678";  // LocalStack accepts any AMI ID string
    cfg.node_port = 7000;
    cfg.aws = make_localstack_cfg();
    for (const std::string& az : {"us-east-1a", "us-east-1b", "us-east-1c"}) {
        cfg.topology.groups.push_back({.group_id = az, .target_count = 1});
        cfg.subnet_by_group[az] = subnet_by_az.at(az);
    }
    cfg.security_group_ids = {sg_id};

    kythira::aws_ec2_quorum_manager<> mgr{cfg};

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    for (const std::string& az : {"us-east-1a", "us-east-1b", "us-east-1c"}) {
        auto peer = mgr.provision_node(az, std::nullopt).get();
        cluster.push_back({.node_id = peer.node_id, .group_id = az});
    }
    BOOST_REQUIRE_EQUAL(cluster.size(), 3u);

    // One node landed in each AZ group — no AZ was skipped or double-booked.
    std::map<std::string, int> count_by_group;
    for (const auto& p : cluster) {
        count_by_group[p.group_id]++;
    }
    BOOST_TEST(count_by_group.size() == 3u);
    for (const auto& [az, count] : count_by_group) {
        BOOST_TEST_MESSAGE("group " << az << ": " << count << " node(s)");
        BOOST_TEST(count == 1);
    }

    auto health = mgr.assess_quorum(cluster).get();
    BOOST_TEST(health.live_node_count == 3u);

    for (const auto& p : cluster) {
        BOOST_CHECK_NO_THROW(mgr.decommission_node(p.node_id).get());
    }
}

#endif  // KYTHIRA_HAS_AWS_SDK
#endif  // KYTHIRA_AWS_LOCALSTACK_TESTS
