#define BOOST_TEST_MODULE gcp_quorum_manager_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/gcp_client_config.hpp>

// The GCP label / resource-name validators live in gcp_client_config.hpp, which
// is compiled unconditionally (no SDK dependency), so these tests run on every
// build — including one without google-cloud-cpp — giving real coverage of
// Requirement 23 AC 11 regardless of SDK availability.

BOOST_AUTO_TEST_SUITE(gcp_validators)

BOOST_AUTO_TEST_CASE(is_valid_gcp_label_accepts) {
    BOOST_CHECK(kythira::is_valid_gcp_label("kythira-cluster"));
    BOOST_CHECK(kythira::is_valid_gcp_label("a"));
    BOOST_CHECK(kythira::is_valid_gcp_label("prod_cluster-01"));
    BOOST_CHECK(kythira::is_valid_gcp_label("z9"));
    // Exactly 63 characters is allowed.
    BOOST_CHECK(kythira::is_valid_gcp_label(std::string("a") + std::string(62, 'b')));
}

BOOST_AUTO_TEST_CASE(is_valid_gcp_label_rejects) {
    BOOST_CHECK(!kythira::is_valid_gcp_label(""));                 // empty
    BOOST_CHECK(!kythira::is_valid_gcp_label("Cluster"));          // uppercase
    BOOST_CHECK(!kythira::is_valid_gcp_label("1cluster"));         // leading digit
    BOOST_CHECK(!kythira::is_valid_gcp_label("kythira:cluster"));  // colon
    BOOST_CHECK(!kythira::is_valid_gcp_label("-lead-dash"));       // leading dash
    // 64 characters is over the limit.
    BOOST_CHECK(!kythira::is_valid_gcp_label(std::string("a") + std::string(63, 'b')));
}

BOOST_AUTO_TEST_CASE(is_valid_gcp_resource_name_accepts) {
    BOOST_CHECK(kythira::is_valid_gcp_resource_name("kythira-test-42"));
    BOOST_CHECK(kythira::is_valid_gcp_resource_name("a"));
    BOOST_CHECK(kythira::is_valid_gcp_resource_name("node0"));
}

BOOST_AUTO_TEST_CASE(is_valid_gcp_resource_name_rejects) {
    BOOST_CHECK(!kythira::is_valid_gcp_resource_name(""));             // empty
    BOOST_CHECK(!kythira::is_valid_gcp_resource_name("Node"));         // uppercase
    BOOST_CHECK(!kythira::is_valid_gcp_resource_name("1node"));        // leading digit
    BOOST_CHECK(!kythira::is_valid_gcp_resource_name("node-"));        // trailing dash
    BOOST_CHECK(!kythira::is_valid_gcp_resource_name("under_score"));  // underscore not allowed
    // 64 characters is over the limit.
    BOOST_CHECK(!kythira::is_valid_gcp_resource_name(std::string("a") + std::string(63, 'b')));
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef KYTHIRA_HAS_GCP_SDK

#include <raft/gcp_compute_quorum_manager.hpp>
#include <raft/gcp_mig_quorum_manager.hpp>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

namespace {

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        folly::init(&argc, &argv, false);
    }
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

#ifdef FIU_ENABLE
struct FiuInitFixture {
    FiuInitFixture() { fiu_init(0); }
    // Skip the MIG autohealing-policy read (an instanceGroupManagers.get call)
    // during construction so mig_construction tests don't need live GCP.
};
BOOST_GLOBAL_FIXTURE(FiuInitFixture);
#endif

auto valid_compute_config() -> kythira::gcp_compute_quorum_manager_config<std::string> {
    kythira::gcp_compute_quorum_manager_config<std::string> cfg;
    cfg.gcp.project_id = "test-project";
    cfg.cluster_name = "test-cluster";
    cfg.boot_disk_image = "projects/debian-cloud/global/images/family/debian-12";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "us-central1-a", .target_count = 3});
    cfg.subnetwork_by_group["us-central1-a"] = "default";
    return cfg;
}

auto valid_mig_config() -> kythira::gcp_mig_quorum_manager_config<std::string> {
    kythira::gcp_mig_quorum_manager_config<std::string> cfg;
    cfg.gcp.project_id = "test-project";
    cfg.cluster_name = "test-cluster";
    cfg.mig_by_group["us-central1-a"] = "kythira-mig-a";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "us-central1-a", .target_count = 3});
    return cfg;
}

}  // namespace

// ── Compute manager construction ────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(gcp_compute_construction)

BOOST_AUTO_TEST_CASE(valid_config_constructs) {
    BOOST_CHECK_NO_THROW((kythira::gcp_compute_quorum_manager<>{valid_compute_config()}));
}

BOOST_AUTO_TEST_CASE(empty_cluster_name_throws) {
    auto cfg = valid_compute_config();
    cfg.cluster_name.clear();
    BOOST_CHECK_THROW((kythira::gcp_compute_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_boot_disk_image_throws) {
    auto cfg = valid_compute_config();
    cfg.boot_disk_image.clear();
    BOOST_CHECK_THROW((kythira::gcp_compute_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(zero_node_port_throws) {
    auto cfg = valid_compute_config();
    cfg.node_port = 0;
    BOOST_CHECK_THROW((kythira::gcp_compute_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(invalid_cluster_name_label_throws) {
    auto cfg = valid_compute_config();
    cfg.cluster_name = "Test:Cluster";  // uppercase + colon → not a valid GCP label
    BOOST_CHECK_THROW((kythira::gcp_compute_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(missing_subnetwork_for_group_throws) {
    auto cfg = valid_compute_config();
    cfg.topology.groups.push_back({.group_id = "us-central1-b", .target_count = 3});
    // No subnetwork for us-central1-b — must throw.
    BOOST_CHECK_THROW((kythira::gcp_compute_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(invalid_extra_label_throws) {
    auto cfg = valid_compute_config();
    cfg.extra_labels["Bad-Key"] = "value";  // uppercase key
    BOOST_CHECK_THROW((kythira::gcp_compute_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(topology_returns_configured_groups) {
    auto cfg = valid_compute_config();
    cfg.topology.groups.push_back({.group_id = "us-central1-b", .target_count = 3});
    cfg.subnetwork_by_group["us-central1-b"] = "default";
    kythira::gcp_compute_quorum_manager<> mgr{cfg};
    BOOST_REQUIRE_EQUAL(mgr.topology().groups.size(), 2u);
    BOOST_CHECK_EQUAL(mgr.topology().total_size(), 6u);
}

BOOST_AUTO_TEST_CASE(node_id_instance_name_round_trip) {
    using mgr_t = kythira::gcp_compute_quorum_manager<std::uint64_t, std::string>;
    const std::uint64_t id = 1234567890123456789ULL;
    auto name = mgr_t::node_id_to_instance_name("test-cluster", id);
    auto back = mgr_t::instance_name_to_node_id("test-cluster", name);
    BOOST_REQUIRE(back.has_value());
    BOOST_CHECK_EQUAL(*back, id);
}

BOOST_AUTO_TEST_CASE(instance_name_to_node_id_rejects_foreign_names) {
    using mgr_t = kythira::gcp_compute_quorum_manager<std::uint64_t, std::string>;
    BOOST_CHECK(!mgr_t::instance_name_to_node_id("test-cluster", "some-other-vm").has_value());
    BOOST_CHECK(!mgr_t::instance_name_to_node_id("test-cluster", "kythira-test-cluster-notanumber")
                     .has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ── MIG manager construction ────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(gcp_mig_construction)

BOOST_AUTO_TEST_CASE(empty_mig_by_group_throws) {
    auto cfg = valid_mig_config();
    cfg.mig_by_group.clear();
    BOOST_CHECK_THROW((kythira::gcp_mig_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_cluster_name_throws) {
    auto cfg = valid_mig_config();
    cfg.cluster_name.clear();
    BOOST_CHECK_THROW((kythira::gcp_mig_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(zero_node_port_throws) {
    auto cfg = valid_mig_config();
    cfg.node_port = 0;
    BOOST_CHECK_THROW((kythira::gcp_mig_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(missing_mig_for_group_throws) {
    auto cfg = valid_mig_config();
    cfg.topology.groups.push_back({.group_id = "us-central1-b", .target_count = 3});
    BOOST_CHECK_THROW((kythira::gcp_mig_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Fault injection ─────────────────────────────────────────────────────────

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(gcp_fault_injection)

// The MIG constructor reads each MIG's autoHealingPolicies via
// instanceGroupManagers.get; skip that read so these tests need no live GCP.
struct MigSkipAutohealing {
    MigSkipAutohealing() { fiu_enable("raft/gcp/mig/skip_autohealing_validation", 1, nullptr, 0); }
    ~MigSkipAutohealing() { fiu_disable("raft/gcp/mig/skip_autohealing_validation"); }
};

BOOST_AUTO_TEST_CASE(compute_list_instances_fault_returns_exceptional_future) {
    kythira::gcp_compute_quorum_manager<> mgr{valid_compute_config()};
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    cluster.push_back({.node_id = 42, .group_id = "us-central1-a"});

    fiu_enable("raft/gcp/compute/list_instances", 1, nullptr, 0);
    auto fut = mgr.assess_quorum(cluster);
    fiu_disable("raft/gcp/compute/list_instances");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(compute_insert_instance_fault_returns_exceptional_future) {
    kythira::gcp_compute_quorum_manager<> mgr{valid_compute_config()};
    fiu_enable("raft/gcp/compute/insert_instance", 1, nullptr, 0);
    auto fut = mgr.provision_node("us-central1-a", std::nullopt);
    fiu_disable("raft/gcp/compute/insert_instance");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(compute_delete_instance_fault_returns_exceptional_future) {
    kythira::gcp_compute_quorum_manager<> mgr{valid_compute_config()};
    fiu_enable("raft/gcp/compute/delete_instance", 1, nullptr, 0);
    auto fut = mgr.decommission_node(std::uint64_t{42});
    fiu_disable("raft/gcp/compute/delete_instance");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(compute_maintain_quorum_fault_returns_exceptional_future) {
    kythira::gcp_compute_quorum_manager<> mgr{valid_compute_config()};
    fiu_enable("raft/gcp/compute/maintain_quorum", 1, nullptr, 0);
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    auto fut = mgr.maintain_quorum(cluster);
    fiu_disable("raft/gcp/compute/maintain_quorum");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_FIXTURE_TEST_CASE(mig_list_instances_fault_returns_exceptional_future, MigSkipAutohealing) {
    kythira::gcp_mig_quorum_manager<> mgr{valid_mig_config()};
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    cluster.push_back({.node_id = 42, .group_id = "us-central1-a"});

    fiu_enable("raft/gcp/mig/list_instances", 1, nullptr, 0);
    auto fut = mgr.assess_quorum(cluster);
    fiu_disable("raft/gcp/mig/list_instances");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_FIXTURE_TEST_CASE(mig_resize_fault_returns_exceptional_future, MigSkipAutohealing) {
    kythira::gcp_mig_quorum_manager<> mgr{valid_mig_config()};
    fiu_enable("raft/gcp/mig/resize", 1, nullptr, 0);
    auto fut = mgr.provision_node("us-central1-a", std::nullopt);
    fiu_disable("raft/gcp/mig/resize");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_FIXTURE_TEST_CASE(mig_delete_instances_fault_returns_exceptional_future, MigSkipAutohealing) {
    kythira::gcp_mig_quorum_manager<> mgr{valid_mig_config()};
    fiu_enable("raft/gcp/mig/delete_instances", 1, nullptr, 0);
    auto fut = mgr.decommission_node(std::uint64_t{42});
    fiu_disable("raft/gcp/mig/delete_instances");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_FIXTURE_TEST_CASE(mig_maintain_quorum_fault_returns_exceptional_future, MigSkipAutohealing) {
    kythira::gcp_mig_quorum_manager<> mgr{valid_mig_config()};
    fiu_enable("raft/gcp/mig/maintain_quorum", 1, nullptr, 0);
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    auto fut = mgr.maintain_quorum(cluster);
    fiu_disable("raft/gcp/mig/maintain_quorum");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE

// ── provision_node target-group rejection (no API call needed) ──────────────

BOOST_AUTO_TEST_SUITE(gcp_target_group_rejection)

BOOST_AUTO_TEST_CASE(compute_provision_unknown_group_returns_exceptional_future) {
    kythira::gcp_compute_quorum_manager<> mgr{valid_compute_config()};
    auto fut = mgr.provision_node("no-such-zone", std::nullopt);
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_GCP_SDK

BOOST_AUTO_TEST_CASE(skipped_no_gcp_sdk) {
    BOOST_TEST_MESSAGE(
        "KYTHIRA_HAS_GCP_SDK not defined — quorum-manager tests skipped "
        "(validator tests above still ran)");
}

#endif  // KYTHIRA_HAS_GCP_SDK
