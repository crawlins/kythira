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
#include <aws/iam/IAMClient.h>
#include <aws/sts/STSClient.h>
#include <aws/sts/model/GetCallerIdentityRequest.h>

#include <folly/init/Init.h>

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* LOCALSTACK_ENDPOINT = "http://localhost:4566";
constexpr const char* DUMMY_REGION = "us-east-1";
constexpr const char* DUMMY_KEY = "test";
constexpr const char* DUMMY_SECRET = "test";

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

// ── LocalstackFixture ─────────────────────────────────────────────────────────

struct LocalstackFixture {
    std::string uuid;
    std::string vpc_id;
    std::string subnet_id;
    std::string sg_id;
    std::string asg_name;
    std::string launch_cfg_name;
    std::shared_ptr<Aws::EC2::EC2Client> ec2;
    std::shared_ptr<Aws::AutoScaling::AutoScalingClient> asg_client;

    LocalstackFixture() {
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
        asg_client = std::make_shared<Aws::AutoScaling::AutoScalingClient>(creds, cli_cfg);

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

        // Launch configuration for ASG tests
        launch_cfg_name = uuid + "-lc";
        asg_name = uuid + "-asg";
        Aws::AutoScaling::Model::CreateLaunchConfigurationRequest lc_req;
        lc_req.SetLaunchConfigurationName(launch_cfg_name);
        lc_req.SetImageId("ami-12345678");
        lc_req.SetInstanceType("t3.micro");
        auto lc_out = asg_client->CreateLaunchConfiguration(lc_req);
        BOOST_REQUIRE_MESSAGE(lc_out.IsSuccess(), "CreateLaunchConfiguration: " +
                                                      std::string(lc_out.GetError().GetMessage()));

        // Auto Scaling Group
        Aws::AutoScaling::Model::CreateAutoScalingGroupRequest asg_req;
        asg_req.SetAutoScalingGroupName(asg_name);
        asg_req.SetLaunchConfigurationName(launch_cfg_name);
        asg_req.SetMinSize(0);
        asg_req.SetMaxSize(9);
        asg_req.SetDesiredCapacity(0);
        asg_req.AddAvailabilityZones("us-east-1a");
        auto asg_out = asg_client->CreateAutoScalingGroup(asg_req);
        BOOST_REQUIRE_MESSAGE(
            asg_out.IsSuccess(),
            "CreateAutoScalingGroup: " + std::string(asg_out.GetError().GetMessage()));
    }

    ~LocalstackFixture() {
        // Teardown: best-effort; errors are logged but not rethrown.
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
};

// ── EC2 manager tests ─────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(ec2_localstack, LocalstackFixture)

BOOST_AUTO_TEST_CASE(provision_three_nodes_spot) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = uuid;
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = subnet_id;
    cfg.security_group_ids.push_back(sg_id);
    cfg.spot_options = kythira::ec2_spot_options{
        .max_price = "",
        .interruption_behavior = kythira::ec2_spot_interruption_behavior::terminate,
    };
    cfg.provision_timeout = std::chrono::seconds{60};
    cfg.poll_interval = std::chrono::seconds{2};
    cfg.aws = make_localstack_cfg();

    kythira::aws_ec2_quorum_manager<> mgr{cfg};

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    for (int i = 0; i < 3; ++i) {
        auto peer = mgr.provision_node("AZ1", std::nullopt).get();
        cluster.push_back({.node_id = peer.node_id, .group_id = "AZ1"});
    }
    BOOST_REQUIRE_EQUAL(cluster.size(), 3u);

    // All nodes tagged with kythira:cluster
    for (const auto& np : cluster) {
        Aws::EC2::Model::DescribeInstancesRequest req;
        {
            Aws::EC2::Model::Filter f;
            f.SetName("tag:kythira:node-id");
            f.AddValues(std::to_string(np.node_id));
            req.AddFilters(f);
        }
        auto out = ec2->DescribeInstances(req);
        BOOST_REQUIRE(out.IsSuccess());
        BOOST_REQUIRE(!out.GetResult().GetReservations().empty());
        const auto& inst = out.GetResult().GetReservations()[0].GetInstances()[0];
        bool found_cluster_tag = false;
        bool found_market_tag = false;
        for (const auto& t : inst.GetTags()) {
            if (std::string(t.GetKey()) == "kythira:cluster") {
                found_cluster_tag = true;
            }
            if (std::string(t.GetKey()) == "kythira:market") {
                found_market_tag = true;
            }
        }
        BOOST_CHECK(found_cluster_tag);
        BOOST_CHECK(found_market_tag);
    }

    // assess_quorum sees 3/3 healthy (LocalStack instances are in running state).
    auto health = mgr.assess_quorum(cluster).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 3u);

    // Decommission one node.
    auto nid = cluster[0].node_id;
    BOOST_CHECK_NO_THROW(mgr.decommission_node(nid).get());

    // Idempotent second decommission.
    BOOST_CHECK_NO_THROW(mgr.decommission_node(nid).get());
}

BOOST_AUTO_TEST_SUITE_END()

// ── ASG manager tests ─────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(asg_localstack, LocalstackFixture)

BOOST_AUTO_TEST_CASE(provision_increments_desired_capacity) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = uuid;
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.asg_by_group["AZ1"] = asg_name;
    cfg.provision_timeout = std::chrono::seconds{60};
    cfg.poll_interval = std::chrono::seconds{2};
    cfg.aws = make_localstack_cfg();

    kythira::aws_asg_quorum_manager<> mgr{cfg};

    Aws::AutoScaling::Model::DescribeAutoScalingGroupsRequest chk_req;
    chk_req.AddAutoScalingGroupNames(asg_name);
    auto before = asg_client->DescribeAutoScalingGroups(chk_req);
    BOOST_REQUIRE(before.IsSuccess());
    int cap_before = before.GetResult().GetAutoScalingGroups()[0].GetDesiredCapacity();

    // LocalStack will not actually launch a real instance but DesiredCapacity
    // increments, which is what the test verifies.
    BOOST_CHECK_NO_THROW(mgr.provision_node("AZ1", std::nullopt));

    auto after = asg_client->DescribeAutoScalingGroups(chk_req);
    BOOST_REQUIRE(after.IsSuccess());
    int cap_after = after.GetResult().GetAutoScalingGroups()[0].GetDesiredCapacity();
    BOOST_CHECK_EQUAL(cap_after, cap_before + 1);
}

BOOST_AUTO_TEST_CASE(assess_empty_asg_is_healthy_with_empty_cluster) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = uuid;
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.asg_by_group["AZ1"] = asg_name;
    cfg.aws = make_localstack_cfg();

    kythira::aws_asg_quorum_manager<> mgr{cfg};
    std::vector<kythira::node_placement<std::uint64_t, std::string>> empty_cluster;
    auto health = mgr.assess_quorum(empty_cluster).get();
    BOOST_CHECK_EQUAL(health.status, kythira::quorum_status::healthy);
    BOOST_CHECK_EQUAL(health.live_node_count, 0u);
}

BOOST_AUTO_TEST_SUITE_END()

}  // namespace

#endif  // KYTHIRA_HAS_AWS_SDK
#endif  // KYTHIRA_AWS_LOCALSTACK_TESTS
