#define BOOST_TEST_MODULE aws_quorum_manager_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_AWS_SDK

#include <raft/aws_asg_quorum_manager.hpp>
#include <raft/aws_ec2_quorum_manager.hpp>

#include <aws/core/Aws.h>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#endif

// Enables the skip-health-check fault point for the duration of a test so
// that asg_construction tests don't need live AWS credentials.
struct AsgSkipHealthCheckFixture {
#ifdef FIU_ENABLE
    AsgSkipHealthCheckFixture() {
        fiu_enable("raft/aws/asg/skip_health_check_validation", 1, nullptr, 0);
    }
    ~AsgSkipHealthCheckFixture() { fiu_disable("raft/aws/asg/skip_health_check_validation"); }
#endif
};

#include <folly/init/Init.h>

namespace {

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
#ifdef FIU_ENABLE
        fiu_init(0);
#endif
    }
    ~AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::ShutdownAPI(opts);
    }
};

// Global fixtures — SDK must outlive all test suites.
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
BOOST_GLOBAL_FIXTURE(AwsSdkFixture);

// ── EC2 manager construction ───────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(ec2_construction)

BOOST_AUTO_TEST_CASE(valid_config_constructs) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-aabbccdd";
    cfg.aws.region = "us-east-1";
    BOOST_CHECK_NO_THROW((kythira::aws_ec2_quorum_manager<>{cfg}));
}

BOOST_AUTO_TEST_CASE(empty_cluster_name_throws) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-aabbccdd";
    BOOST_CHECK_THROW((kythira::aws_ec2_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_image_id_throws) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-aabbccdd";
    BOOST_CHECK_THROW((kythira::aws_ec2_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(zero_node_port_throws) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 0;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-aabbccdd";
    BOOST_CHECK_THROW((kythira::aws_ec2_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(missing_subnet_for_group_throws) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.topology.groups.push_back({.group_id = "AZ2", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-aabbccdd";
    // No subnet for AZ2 — must throw.
    BOOST_CHECK_THROW((kythira::aws_ec2_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(topology_returns_configured_groups) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.topology.groups.push_back({.group_id = "AZ2", .target_count = 3});
    cfg.topology.groups.push_back({.group_id = "AZ3", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-11";
    cfg.subnet_by_group["AZ2"] = "subnet-22";
    cfg.subnet_by_group["AZ3"] = "subnet-33";
    cfg.aws.region = "us-east-1";
    kythira::aws_ec2_quorum_manager<> mgr{cfg};
    auto topo = mgr.topology();
    BOOST_REQUIRE_EQUAL(topo.groups.size(), 3u);
    BOOST_CHECK_EQUAL(topo.total_size(), 9u);
}

BOOST_AUTO_TEST_CASE(provision_unknown_group_returns_exceptional_future) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-11";
    cfg.aws.region = "us-east-1";
    kythira::aws_ec2_quorum_manager<> mgr{cfg};
    auto fut = mgr.provision_node("AZ-UNKNOWN", std::nullopt);
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(spot_config_accepted) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-11";
    cfg.spot_options = kythira::ec2_spot_options{
        .max_price = "0.05",
        .interruption_behavior = kythira::ec2_spot_interruption_behavior::terminate,
    };
    cfg.aws.region = "us-east-1";
    BOOST_CHECK_NO_THROW((kythira::aws_ec2_quorum_manager<>{cfg}));
}

BOOST_AUTO_TEST_CASE(placement_group_config_accepted) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-11";
    cfg.placement_by_group["AZ1"] = {
        .name = "kythira-spread",
        .strategy = kythira::ec2_placement_group_strategy::spread,
    };
    cfg.aws.region = "us-east-1";
    BOOST_CHECK_NO_THROW((kythira::aws_ec2_quorum_manager<>{cfg}));
}

BOOST_AUTO_TEST_CASE(ec2_id_node_id_round_trip) {
    using mgr_t = kythira::aws_ec2_quorum_manager<std::uint64_t, std::string>;
    const std::string ec2_id = "i-0deadbeefcafe0001";
    auto nid = mgr_t::ec2_id_to_node_id(ec2_id);
    auto back = mgr_t::node_id_to_ec2_id(nid);
    BOOST_CHECK_EQUAL(back, ec2_id);
}

BOOST_AUTO_TEST_SUITE_END()

// ── ASG manager construction ───────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(asg_construction, AsgSkipHealthCheckFixture)

BOOST_AUTO_TEST_CASE(valid_config_constructs) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.asg_by_group["AZ1"] = "kythira-test-asg-az1";
    cfg.aws.region = "us-east-1";
    BOOST_CHECK_NO_THROW((kythira::aws_asg_quorum_manager<>{cfg}));
}

BOOST_AUTO_TEST_CASE(empty_cluster_name_throws) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.asg_by_group["AZ1"] = "kythira-test-asg-az1";
    BOOST_CHECK_THROW((kythira::aws_asg_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_asg_by_group_throws) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    // No asg_by_group entries.
    BOOST_CHECK_THROW((kythira::aws_asg_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(zero_node_port_throws) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.node_port = 0;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.asg_by_group["AZ1"] = "kythira-test-asg-az1";
    BOOST_CHECK_THROW((kythira::aws_asg_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(missing_asg_for_group_throws) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.topology.groups.push_back({.group_id = "AZ2", .target_count = 3});
    cfg.asg_by_group["AZ1"] = "kythira-test-asg-az1";
    // No ASG for AZ2 — must throw.
    BOOST_CHECK_THROW((kythira::aws_asg_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(topology_returns_configured_groups) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.topology.groups.push_back({.group_id = "AZ2", .target_count = 3});
    cfg.topology.groups.push_back({.group_id = "AZ3", .target_count = 3});
    cfg.asg_by_group["AZ1"] = "asg-az1";
    cfg.asg_by_group["AZ2"] = "asg-az2";
    cfg.asg_by_group["AZ3"] = "asg-az3";
    cfg.aws.region = "us-east-1";
    kythira::aws_asg_quorum_manager<> mgr{cfg};
    auto topo = mgr.topology();
    BOOST_REQUIRE_EQUAL(topo.groups.size(), 3u);
    BOOST_CHECK_EQUAL(topo.total_size(), 9u);
}

BOOST_AUTO_TEST_CASE(provision_unknown_group_returns_exceptional_future) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.asg_by_group["AZ1"] = "asg-az1";
    cfg.aws.region = "us-east-1";
    kythira::aws_asg_quorum_manager<> mgr{cfg};
    auto fut = mgr.provision_node("AZ-UNKNOWN", std::nullopt);
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Fault injection ────────────────────────────────────────────────────────────

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(fault_injection)

BOOST_AUTO_TEST_CASE(ec2_describe_instance_status_fault_returns_exceptional_future) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-11";
    cfg.aws.region = "us-east-1";
    kythira::aws_ec2_quorum_manager<> mgr{cfg};

    // Non-empty cluster so assess_quorum proceeds past the early-exit guard.
    using mgr_t = kythira::aws_ec2_quorum_manager<std::uint64_t, std::string>;
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    cluster.push_back(
        {.node_id = mgr_t::ec2_id_to_node_id("i-0deadbeefcafe0001"), .group_id = "AZ1"});

    fiu_enable("raft/aws/ec2/describe_instance_status", 1, nullptr, 0);
    auto fut = mgr.assess_quorum(cluster);
    fiu_disable("raft/aws/ec2/describe_instance_status");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(ec2_run_instances_fault_returns_exceptional_future) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-11";
    cfg.aws.region = "us-east-1";
    kythira::aws_ec2_quorum_manager<> mgr{cfg};

    fiu_enable("raft/aws/ec2/run_instances", 1, nullptr, 0);
    auto fut = mgr.provision_node("AZ1", std::nullopt);
    fiu_disable("raft/aws/ec2/run_instances");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(ec2_terminate_instances_fault_returns_exceptional_future) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-11";
    cfg.aws.region = "us-east-1";
    kythira::aws_ec2_quorum_manager<> mgr{cfg};

    fiu_enable("raft/aws/ec2/terminate_instances", 1, nullptr, 0);
    auto fut = mgr.decommission_node(std::uint64_t{1});
    fiu_disable("raft/aws/ec2/terminate_instances");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(ec2_maintain_quorum_fault_returns_exceptional_future) {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-11";
    cfg.aws.region = "us-east-1";
    kythira::aws_ec2_quorum_manager<> mgr{cfg};

    fiu_enable("raft/aws/ec2/maintain_quorum", 1, nullptr, 0);
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    auto fut = mgr.maintain_quorum(cluster);
    fiu_disable("raft/aws/ec2/maintain_quorum");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(asg_describe_instance_status_fault_returns_exceptional_future) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.asg_by_group["AZ1"] = "asg-az1";
    cfg.aws.region = "us-east-1";
    fiu_enable("raft/aws/asg/skip_health_check_validation", 1, nullptr, 0);
    kythira::aws_asg_quorum_manager<> mgr{cfg};
    fiu_disable("raft/aws/asg/skip_health_check_validation");

    // Non-empty cluster so assess_quorum proceeds past the early-exit guard.
    using ec2_mgr_t = kythira::aws_ec2_quorum_manager<std::uint64_t, std::string>;
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    cluster.push_back(
        {.node_id = ec2_mgr_t::ec2_id_to_node_id("i-0deadbeefcafe0001"), .group_id = "AZ1"});

    fiu_enable("raft/aws/asg/describe_instance_status", 1, nullptr, 0);
    auto fut = mgr.assess_quorum(cluster);
    fiu_disable("raft/aws/asg/describe_instance_status");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(asg_update_asg_fault_returns_exceptional_future) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.asg_by_group["AZ1"] = "asg-az1";
    cfg.aws.region = "us-east-1";
    fiu_enable("raft/aws/asg/skip_health_check_validation", 1, nullptr, 0);
    kythira::aws_asg_quorum_manager<> mgr{cfg};
    fiu_disable("raft/aws/asg/skip_health_check_validation");

    fiu_enable("raft/aws/asg/update_asg", 1, nullptr, 0);
    auto fut = mgr.provision_node("AZ1", std::nullopt);
    fiu_disable("raft/aws/asg/update_asg");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE

#endif  // KYTHIRA_HAS_AWS_SDK

}  // namespace
