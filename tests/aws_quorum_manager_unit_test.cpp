// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE aws_quorum_manager_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_AWS_SDK

#include <raft/aws_asg_quorum_manager.hpp>
#include <raft/aws_ec2_quorum_manager.hpp>
#include <raft/elastic_capacity_controller.hpp>

#include <aws/core/Aws.h>

#include <map>
#include <string>
#include <vector>

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

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#include <memory>

#endif
namespace {

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
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif
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

// Composite and string mode map a node to its instance by reading fields:
// every EC2 id fits, including the 17-digit ids with a non-zero first digit
// that std::stoull overflowed on and the legacy 8-digit ids.
BOOST_AUTO_TEST_CASE(instance_id_node_id_round_trip) {
    using composite_t = kythira::aws_ec2_quorum_manager<kythira::aws_ec2_node_id, std::string>;
    using string_t = kythira::aws_ec2_quorum_manager<std::string, std::string>;
    for (const std::string ec2_id : {"i-0deadbeefcafe0001", "i-f0123456789abcdef", "i-1234abcd"}) {
        auto nid = composite_t::node_id_for_instance("us-east-1", ec2_id);
        BOOST_REQUIRE(nid.has_value());
        BOOST_CHECK_EQUAL(nid->native(), ec2_id);
        BOOST_CHECK_EQUAL(composite_t::instance_id_for_node("us-east-1", *nid).value_or(""),
                          ec2_id);
        BOOST_CHECK(!composite_t::instance_id_for_node("us-west-2", *nid));

        auto text = string_t::node_id_for_instance("us-east-1", ec2_id);
        BOOST_REQUIRE(text.has_value());
        BOOST_CHECK_EQUAL(*text, "aws-ec2:us-east-1:" + ec2_id);
        BOOST_CHECK_EQUAL(string_t::instance_id_for_node("us-east-1", *text).value_or(""), ec2_id);
    }
    BOOST_CHECK(!composite_t::node_id_for_instance("us-east-1", "i-xyz"));
    BOOST_CHECK(!string_t::instance_id_for_node("us-east-1", "12345"));
}

// Numeric mode's fallback for instances launched before ids were tags: the
// old derivation, only where it could have produced the instance.
BOOST_AUTO_TEST_CASE(legacy_derivation_is_bounded) {
    using mgr_t = kythira::aws_ec2_quorum_manager<std::uint64_t, std::string>;
    BOOST_CHECK_EQUAL(mgr_t::legacy_instance_id(0xdeadbeefcafe0001ULL), "i-0deadbeefcafe0001");
    BOOST_CHECK_EQUAL(mgr_t::legacy_node_id("i-0deadbeefcafe0001").value_or(0),
                      0xdeadbeefcafe0001ULL);
    BOOST_CHECK(!mgr_t::legacy_node_id("i-f0123456789abcdef"));  // does not fit
    BOOST_CHECK(!mgr_t::legacy_node_id("i-1234abcd"));           // never derived
    BOOST_CHECK(!mgr_t::legacy_node_id("i-0DEADBEEFCAFE0001"));
    BOOST_CHECK(!mgr_t::legacy_node_id("i-0deadbeefcafe000x"));
}

BOOST_AUTO_TEST_SUITE_END()

// ── EC2 idempotency keys (elastic-shard-capacity Requirement 8.2) ─────────────

namespace {

auto keyed_ec2_config() -> kythira::aws_ec2_quorum_manager_config {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-11";
    cfg.aws.region = "us-east-1";
    return cfg;
}

/// Every value `spec` carries for tag key `key`, in order.
auto tag_values(const Aws::EC2::Model::TagSpecification& spec, const std::string& key)
    -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& t : spec.GetTags()) {
        if (std::string(t.GetKey()) == key) {
            out.emplace_back(t.GetValue());
        }
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(ec2_idempotency_key)

using ec2_mgr_t = kythira::aws_ec2_quorum_manager<std::uint64_t, std::string>;

// The capacity controller detects keyed provisioning by this concept; a
// signature drift would silently drop the manager back to unkeyed matching.
static_assert(kythira::keyed_quorum_manager<ec2_mgr_t>);

BOOST_AUTO_TEST_CASE(launch_tags_carry_the_key) {
    ec2_mgr_t mgr{keyed_ec2_config()};
    const std::string key = "cap-7-1759363200000-9f86d081884c7d65";
    auto spec = mgr.launch_tag_specification("AZ1", "on-demand", key);
    BOOST_CHECK(spec.GetResourceType() == Aws::EC2::Model::ResourceType::instance);
    BOOST_CHECK(tag_values(spec, "kythira:idempotency-key") == std::vector<std::string>{key});
    // The rest of the launch set is still there alongside it.
    BOOST_CHECK(tag_values(spec, "kythira:cluster") == std::vector<std::string>{"test-cluster"});
}

BOOST_AUTO_TEST_CASE(unkeyed_launch_carries_no_key_tag) {
    ec2_mgr_t mgr{keyed_ec2_config()};
    auto spec = mgr.launch_tag_specification("AZ1", "on-demand");
    BOOST_CHECK(tag_values(spec, "kythira:idempotency-key").empty());
}

BOOST_AUTO_TEST_CASE(key_tag_wins_over_an_extra_tag_of_the_same_name) {
    auto cfg = keyed_ec2_config();
    cfg.extra_tags["kythira:idempotency-key"] = "stale";
    ec2_mgr_t mgr{cfg};
    auto spec = mgr.launch_tag_specification("AZ1", "on-demand", std::string("cap-1-2-3"));
    // Exactly once: EC2 rejects a request naming one tag key twice.
    BOOST_CHECK(tag_values(spec, "kythira:idempotency-key") ==
                std::vector<std::string>{"cap-1-2-3"});
}

BOOST_AUTO_TEST_CASE(lookup_filters_on_cluster_key_and_live_states) {
    auto req = ec2_mgr_t::idempotency_lookup_request("test-cluster", "cap-1-2-3");
    std::map<std::string, std::vector<std::string>> filters;
    for (const auto& f : req.GetFilters()) {
        auto& values = filters[std::string(f.GetName())];
        for (const auto& v : f.GetValues()) {
            values.emplace_back(v);
        }
    }
    BOOST_CHECK(filters["tag:kythira:cluster"] == std::vector<std::string>{"test-cluster"});
    BOOST_CHECK(filters["tag:kythira:idempotency-key"] == std::vector<std::string>{"cap-1-2-3"});
    BOOST_CHECK((filters["instance-state-name"] ==
                 std::vector<std::string>{"pending", "running", "stopping", "stopped"}));
}

BOOST_AUTO_TEST_CASE(stopped_counts_terminated_does_not) {
    using S = Aws::EC2::Model::InstanceStateName;
    BOOST_CHECK(ec2_mgr_t::counts_as_holding_key(S::pending));
    BOOST_CHECK(ec2_mgr_t::counts_as_holding_key(S::running));
    BOOST_CHECK(ec2_mgr_t::counts_as_holding_key(S::stopping));
    BOOST_CHECK(ec2_mgr_t::counts_as_holding_key(S::stopped));
    BOOST_CHECK(!ec2_mgr_t::counts_as_holding_key(S::shutting_down));
    BOOST_CHECK(!ec2_mgr_t::counts_as_holding_key(S::terminated));
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
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    cluster.push_back({.node_id = 1, .group_id = "AZ1"});

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

BOOST_AUTO_TEST_CASE(ec2_find_by_idempotency_key_fault_returns_exceptional_future) {
    kythira::aws_ec2_quorum_manager<> mgr{keyed_ec2_config()};

    fiu_enable("raft/aws/ec2/find_by_idempotency_key", 1, nullptr, 0);
    auto fut = mgr.find_by_idempotency_key("cap-1-2-3");
    fiu_disable("raft/aws/ec2/find_by_idempotency_key");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(ec2_keyed_provision_shares_the_run_instances_fault) {
    kythira::aws_ec2_quorum_manager<> mgr{keyed_ec2_config()};

    fiu_enable("raft/aws/ec2/run_instances", 1, nullptr, 0);
    auto fut = mgr.provision_node_keyed("AZ1", std::nullopt, "cap-1-2-3");
    fiu_disable("raft/aws/ec2/run_instances");

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
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    cluster.push_back({.node_id = 1, .group_id = "AZ1"});

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

BOOST_AUTO_TEST_CASE(asg_terminate_instance_fault_returns_exceptional_future) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.asg_by_group["AZ1"] = "asg-az1";
    cfg.aws.region = "us-east-1";
    fiu_enable("raft/aws/asg/skip_health_check_validation", 1, nullptr, 0);
    kythira::aws_asg_quorum_manager<> mgr{cfg};
    fiu_disable("raft/aws/asg/skip_health_check_validation");

    fiu_enable("raft/aws/asg/terminate_instance", 1, nullptr, 0);
    auto fut = mgr.decommission_node(std::uint64_t{1});
    fiu_disable("raft/aws/asg/terminate_instance");

    // Matched on the fault's own text: with no reachable AWS endpoint the real
    // TerminateInstanceInAutoScalingGroup call fails too, so a bare
    // "something was thrown" would pass with the fault point deleted.
    BOOST_CHECK_EXCEPTION(std::move(fut).get(), std::exception, [](const std::exception& ex) {
        return std::string(ex.what()).find("fault: raft/aws/asg/terminate_instance") !=
               std::string::npos;
    });
}

BOOST_AUTO_TEST_CASE(asg_maintain_quorum_fault_returns_exceptional_future) {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.asg_by_group["AZ1"] = "asg-az1";
    cfg.aws.region = "us-east-1";
    fiu_enable("raft/aws/asg/skip_health_check_validation", 1, nullptr, 0);
    kythira::aws_asg_quorum_manager<> mgr{cfg};
    fiu_disable("raft/aws/asg/skip_health_check_validation");

    fiu_enable("raft/aws/asg/maintain_quorum", 1, nullptr, 0);
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    auto fut = mgr.maintain_quorum(cluster);
    fiu_disable("raft/aws/asg/maintain_quorum");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE

#endif  // KYTHIRA_HAS_AWS_SDK

}  // namespace
