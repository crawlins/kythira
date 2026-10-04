// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

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

// The network/subnetwork qualifiers also live in gcp_client_config.hpp, so like
// the validators above they are covered on an SDK-less build too. A bare short
// name — which `network`'s "default" default value is — must come back as a
// reference, because `instances.insert` rejects an unqualified name.

BOOST_AUTO_TEST_SUITE(gcp_resource_reference_qualifiers)

BOOST_AUTO_TEST_CASE(zone_to_region_strips_the_zone_suffix) {
    BOOST_CHECK_EQUAL(kythira::gcp_zone_to_region("us-central1-a"), "us-central1");
    BOOST_CHECK_EQUAL(kythira::gcp_zone_to_region("europe-west4-b"), "europe-west4");
    // The input must be a zone: handed a region, this strips a component that
    // was never a zone suffix. Callers pass `subnetwork_by_group`'s key, which
    // is always a zone.
    BOOST_CHECK_EQUAL(kythira::gcp_zone_to_region("us-central1"), "us");
    // No `-` at all: nothing to strip, returned unchanged.
    BOOST_CHECK_EQUAL(kythira::gcp_zone_to_region("zone"), "zone");
}

BOOST_AUTO_TEST_CASE(is_gcp_resource_reference_detects_paths_and_urls) {
    BOOST_CHECK(kythira::is_gcp_resource_reference("global/networks/default"));
    BOOST_CHECK(kythira::is_gcp_resource_reference("projects/p/global/networks/default"));
    BOOST_CHECK(kythira::is_gcp_resource_reference(
        "https://www.googleapis.com/compute/v1/projects/p/global/networks/default"));
    BOOST_CHECK(!kythira::is_gcp_resource_reference("default"));
    BOOST_CHECK(!kythira::is_gcp_resource_reference(""));
}

BOOST_AUTO_TEST_CASE(qualify_network_expands_a_short_name) {
    BOOST_CHECK_EQUAL(kythira::gcp_qualify_network("default"), "global/networks/default");
    BOOST_CHECK_EQUAL(kythira::gcp_qualify_network("my-vpc"), "global/networks/my-vpc");
}

BOOST_AUTO_TEST_CASE(qualify_network_passes_references_through) {
    // Relative, partial-URL, and full-URL references are all left untouched —
    // notably the Shared VPC case, where the host project differs from the
    // request's project and rewriting the value would break it.
    const std::string relative = "global/networks/default";
    const std::string shared_vpc = "projects/host-project/global/networks/shared";
    const std::string full_url =
        "https://www.googleapis.com/compute/v1/projects/p/global/networks/default";
    BOOST_CHECK_EQUAL(kythira::gcp_qualify_network(relative), relative);
    BOOST_CHECK_EQUAL(kythira::gcp_qualify_network(shared_vpc), shared_vpc);
    BOOST_CHECK_EQUAL(kythira::gcp_qualify_network(full_url), full_url);
    // Empty stays empty: the API, not us, reports a missing required field.
    BOOST_CHECK_EQUAL(kythira::gcp_qualify_network(""), "");
}

BOOST_AUTO_TEST_CASE(qualify_subnetwork_expands_using_the_zones_region) {
    BOOST_CHECK_EQUAL(kythira::gcp_qualify_subnetwork("default", "us-central1-a"),
                      "regions/us-central1/subnetworks/default");
    BOOST_CHECK_EQUAL(kythira::gcp_qualify_subnetwork("app-subnet", "europe-west4-c"),
                      "regions/europe-west4/subnetworks/app-subnet");
}

BOOST_AUTO_TEST_CASE(qualify_subnetwork_passes_references_through) {
    const std::string shared_vpc = "projects/host-project/regions/us-central1/subnetworks/shared";
    BOOST_CHECK_EQUAL(kythira::gcp_qualify_subnetwork(shared_vpc, "us-central1-a"), shared_vpc);
    BOOST_CHECK_EQUAL(kythira::gcp_qualify_subnetwork("", "us-central1-a"), "");
}

BOOST_AUTO_TEST_SUITE_END()

// The idempotency-key label mapping is SDK-free for the same reason as the
// validators: a successor controller must derive the very label its
// predecessor wrote, so the mapping is worth covering on every build.

BOOST_AUTO_TEST_SUITE(gcp_idempotency_label)

BOOST_AUTO_TEST_CASE(controller_keys_are_used_verbatim) {
    // The shape `elastic_capacity_controller::new_key` mints.
    const std::string key = "cap-7-1759363200000-9f86d081884c7d65";
    BOOST_CHECK_EQUAL(kythira::gcp_idempotency_label_value(key), key);
}

BOOST_AUTO_TEST_CASE(unsafe_keys_map_to_a_valid_label) {
    for (const std::string key :
         {std::string("Cap-UPPER"), std::string("cap:with/colons"), std::string("9-leading-digit"),
          std::string("cap-18446744073709551615-1759363200000-ffffffffffffffff-x"),
          std::string(200, 'a'), std::string("")}) {
        const auto v = kythira::gcp_idempotency_label_value(key);
        BOOST_TEST_CONTEXT("key '" << key << "' -> '" << v << "'") {
            BOOST_CHECK(kythira::is_valid_gcp_label(v));
            BOOST_CHECK_LE(v.size(), 63u);
        }
    }
}

BOOST_AUTO_TEST_CASE(mapping_is_deterministic_and_keeps_keys_apart) {
    const std::string a(100, 'a');
    const std::string b = std::string(99, 'a') + "b";  // same sanitised prefix
    BOOST_CHECK_EQUAL(kythira::gcp_idempotency_label_value(a),
                      kythira::gcp_idempotency_label_value(a));
    BOOST_CHECK_NE(kythira::gcp_idempotency_label_value(a),
                   kythira::gcp_idempotency_label_value(b));
    BOOST_CHECK_NE(kythira::gcp_idempotency_label_value("Cap-X"),
                   kythira::gcp_idempotency_label_value("cap-x"));
}

BOOST_AUTO_TEST_CASE(filter_selects_cluster_and_key) {
    BOOST_CHECK_EQUAL(
        kythira::gcp_idempotency_key_filter("test-cluster", "cap-1-2-3"),
        R"((labels.kythira-cluster = "test-cluster") (labels.kythira-idempotency-key = "cap-1-2-3"))");
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef KYTHIRA_HAS_GCP_SDK

#include "gcp_fake_compute_clients.hpp"

#include <raft/elastic_capacity_controller.hpp>
#include <raft/gcp_compute_quorum_manager.hpp>
#include <raft/gcp_mig_quorum_manager.hpp>
#include <raft/gcp_operation_wait.hpp>

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

namespace fakes = kythira::test::gcp_fakes;
using compute_mgr = kythira::gcp_compute_quorum_manager<>;
using mig_mgr = kythira::gcp_mig_quorum_manager<>;
using placements = std::vector<kythira::node_placement<std::uint64_t, std::string>>;

/// Shrinks every wait so a fake-backed call that polls returns in milliseconds.
template<typename Config> auto fast(Config cfg) -> Config {
    cfg.gcp.api_timeout = std::chrono::seconds{1};
    cfg.gcp.operation_poll_interval = std::chrono::milliseconds{1};
    cfg.poll_interval = std::chrono::milliseconds{1};
    cfg.provision_timeout = std::chrono::seconds{1};
    return cfg;
}

auto make_compute(const fakes::fake_compute& f,
                  kythira::gcp_compute_quorum_manager_config<std::string> cfg =
                      valid_compute_config()) -> compute_mgr {
    return compute_mgr{fast(std::move(cfg)), f.instances_client(), f.zone_operations_client()};
}

/// Registers every MIG in @p cfg with the fake (no autohealing policy), then
/// builds the manager over it — the constructor reads each MIG.
auto make_mig(const fakes::fake_compute& f,
              kythira::gcp_mig_quorum_manager_config<std::string> cfg = valid_mig_config())
    -> mig_mgr {
    for (const auto& [zone, name] : cfg.mig_by_group) {
        if (f.migs->migs.find(name) == f.migs->migs.end()) {
            f.migs->add(name, 3);
        }
    }
    return mig_mgr{fast(std::move(cfg)), f.migs_client(), f.instances_client(),
                   f.zone_operations_client()};
}

auto what_of(auto&& fut) -> std::string {
    try {
        std::move(fut).get();
    } catch (const std::exception& ex) {
        return ex.what();
    }
    return {};
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

// The MIG constructor reads each MIG over the injected fake, so no live GCP.
BOOST_AUTO_TEST_CASE(mig_list_instances_fault_returns_exceptional_future) {
    fakes::fake_compute f;
    auto mgr = make_mig(f);
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    cluster.push_back({.node_id = 42, .group_id = "us-central1-a"});

    fiu_enable("raft/gcp/mig/list_instances", 1, nullptr, 0);
    auto fut = mgr.assess_quorum(cluster);
    fiu_disable("raft/gcp/mig/list_instances");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(mig_resize_fault_returns_exceptional_future) {
    fakes::fake_compute f;
    auto mgr = make_mig(f);
    fiu_enable("raft/gcp/mig/resize", 1, nullptr, 0);
    auto fut = mgr.provision_node("us-central1-a", std::nullopt);
    fiu_disable("raft/gcp/mig/resize");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(mig_delete_instances_fault_returns_exceptional_future) {
    fakes::fake_compute f;
    auto mgr = make_mig(f);
    fiu_enable("raft/gcp/mig/delete_instances", 1, nullptr, 0);
    auto fut = mgr.decommission_node(std::uint64_t{42});
    fiu_disable("raft/gcp/mig/delete_instances");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(mig_maintain_quorum_fault_returns_exceptional_future) {
    fakes::fake_compute f;
    auto mgr = make_mig(f);
    fiu_enable("raft/gcp/mig/maintain_quorum", 1, nullptr, 0);
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    auto fut = mgr.maintain_quorum(cluster);
    fiu_disable("raft/gcp/mig/maintain_quorum");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE

// ── Idempotency keys (elastic-shard-capacity Requirement 8.2) ───────────────
//
// The instance resource and the per-instance match are pure, so these need no
// API call; the list/insert round trips themselves are what the fault cases
// above and the real-GCE suite cover.

BOOST_AUTO_TEST_SUITE(gcp_compute_idempotency_key)

using compute_mgr_t = kythira::gcp_compute_quorum_manager<std::uint64_t, std::string>;

// The capacity controller detects keyed provisioning by this concept; a
// signature drift would silently drop the manager back to unkeyed matching.
static_assert(kythira::keyed_quorum_manager<compute_mgr_t>);

BOOST_AUTO_TEST_CASE(insert_resource_carries_the_key_label) {
    compute_mgr_t mgr{valid_compute_config()};
    auto inst = mgr.build_instance("kythira-test-cluster-42", 42, "us-central1-a", "default",
                                   std::string("cap-1-2-3"));
    BOOST_REQUIRE(inst.labels().count("kythira-idempotency-key") == 1);
    BOOST_CHECK_EQUAL(inst.labels().at("kythira-idempotency-key"), "cap-1-2-3");

    auto unkeyed = mgr.build_instance("kythira-test-cluster-42", 42, "us-central1-a", "default");
    BOOST_CHECK(unkeyed.labels().count("kythira-idempotency-key") == 0);
}

BOOST_AUTO_TEST_CASE(key_label_wins_over_an_extra_label_of_the_same_name) {
    auto cfg = valid_compute_config();
    cfg.extra_labels["kythira-idempotency-key"] = "stale";
    compute_mgr_t mgr{cfg};
    auto inst = mgr.build_instance("kythira-test-cluster-42", 42, "us-central1-a", "default",
                                   std::string("cap-1-2-3"));
    BOOST_CHECK_EQUAL(inst.labels().at("kythira-idempotency-key"), "cap-1-2-3");
}

BOOST_AUTO_TEST_CASE(keyed_instance_peer_matches_cluster_and_key) {
    compute_mgr_t mgr{valid_compute_config()};
    auto inst = mgr.build_instance("kythira-test-cluster-42", 42, "us-central1-a", "default",
                                   std::string("cap-1-2-3"));
    inst.mutable_network_interfaces(0)->set_network_ip("10.128.0.7");
    // TERMINATED is GCE's "stopped": it still exists, so it still counts.
    inst.set_status("TERMINATED");

    auto peer = compute_mgr_t::keyed_instance_peer(inst, "test-cluster", "cap-1-2-3", 7000);
    BOOST_REQUIRE(peer.has_value());
    BOOST_CHECK_EQUAL(peer->node_id, 42u);
    BOOST_CHECK_EQUAL(peer->address, "10.128.0.7:7000");

    BOOST_CHECK(!compute_mgr_t::keyed_instance_peer(inst, "test-cluster", "cap-9", 7000));
    BOOST_CHECK(!compute_mgr_t::keyed_instance_peer(inst, "other-cluster", "cap-1-2-3", 7000));
}

#ifdef FIU_ENABLE
BOOST_AUTO_TEST_CASE(find_by_idempotency_key_fault_returns_exceptional_future) {
    compute_mgr_t mgr{valid_compute_config()};
    fiu_enable("raft/gcp/compute/find_by_idempotency_key", 1, nullptr, 0);
    auto fut = mgr.find_by_idempotency_key("cap-1-2-3");
    fiu_disable("raft/gcp/compute/find_by_idempotency_key");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(keyed_provision_shares_the_insert_fault) {
    compute_mgr_t mgr{valid_compute_config()};
    fiu_enable("raft/gcp/compute/insert_instance", 1, nullptr, 0);
    auto fut = mgr.provision_node_keyed("us-central1-a", std::nullopt, "cap-1-2-3");
    fiu_disable("raft/gcp/compute/insert_instance");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}
#endif  // FIU_ENABLE

BOOST_AUTO_TEST_SUITE_END()

// ── provision_node target-group rejection (no API call needed) ──────────────

BOOST_AUTO_TEST_SUITE(gcp_target_group_rejection)

BOOST_AUTO_TEST_CASE(compute_provision_unknown_group_returns_exceptional_future) {
    kythira::gcp_compute_quorum_manager<> mgr{valid_compute_config()};
    auto fut = mgr.provision_node("no-such-zone", std::nullopt);
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

// Requirement 23 AC 8, MIG half: a group with no `mig_by_group` entry is
// rejected before any instanceGroupManagers call is made.
BOOST_AUTO_TEST_CASE(mig_provision_unknown_group_returns_exceptional_future) {
    fakes::fake_compute f;
    auto mgr = make_mig(f);
    const int gets_after_construction = f.migs->get_calls;

    auto msg = what_of(mgr.provision_node("no-such-zone", std::nullopt));
    BOOST_CHECK_NE(msg.find("no MIG for group: no-such-zone"), std::string::npos);
    BOOST_CHECK_EQUAL(f.migs->get_calls, gets_after_construction);
    BOOST_CHECK_EQUAL(f.migs->resize_calls, 0);
    BOOST_CHECK_EQUAL(f.zone_ops->get_calls, 0);
}

BOOST_AUTO_TEST_SUITE_END()

// ── wait_for_zone_operation against a fake zoneOperations ──────────────────

BOOST_AUTO_TEST_SUITE(gcp_zone_operation_wait)

constexpr auto kTimeout = std::chrono::seconds{5};
constexpr auto kPoll = std::chrono::milliseconds{1};

BOOST_AUTO_TEST_CASE(done_operation_resolves_after_one_poll) {
    fakes::fake_compute f;
    auto client = f.zone_operations_client();
    BOOST_CHECK_NO_THROW(kythira::wait_for_zone_operation(client, "test-project", "us-central1-a",
                                                          "op-1", kTimeout, kPoll)
                             .get());
    BOOST_CHECK_EQUAL(f.zone_ops->get_calls, 1);
}

BOOST_AUTO_TEST_CASE(polls_until_done) {
    fakes::fake_compute f;
    f.zone_ops->on_get = [&](const auto& req) -> google::cloud::StatusOr<fakes::cv1::Operation> {
        return fakes::make_operation(req.operation(),
                                     f.zone_ops->get_calls < 3 ? "RUNNING" : "DONE");
    };
    auto client = f.zone_operations_client();
    BOOST_CHECK_NO_THROW(kythira::wait_for_zone_operation(client, "test-project", "us-central1-a",
                                                          "op-1", kTimeout, kPoll)
                             .get());
    BOOST_CHECK_EQUAL(f.zone_ops->get_calls, 3);
}

BOOST_AUTO_TEST_CASE(done_with_error_is_exceptional_and_names_the_error) {
    fakes::fake_compute f;
    f.zone_ops->on_get = [](const auto& req) -> google::cloud::StatusOr<fakes::cv1::Operation> {
        return fakes::make_failed_operation(req.operation(), "QUOTA_EXCEEDED", "no CPUs left");
    };
    auto client = f.zone_operations_client();
    auto msg = what_of(kythira::wait_for_zone_operation(client, "test-project", "us-central1-a",
                                                        "op-1", kTimeout, kPoll));
    BOOST_CHECK_NE(msg.find("[QUOTA_EXCEEDED] no CPUs left"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(get_failure_is_exceptional) {
    fakes::fake_compute f;
    f.zone_ops->on_get = [](const auto&) -> google::cloud::StatusOr<fakes::cv1::Operation> {
        return google::cloud::Status(google::cloud::StatusCode::kPermissionDenied, "denied");
    };
    auto client = f.zone_operations_client();
    auto msg = what_of(kythira::wait_for_zone_operation(client, "test-project", "us-central1-a",
                                                        "op-1", kTimeout, kPoll));
    BOOST_CHECK_NE(msg.find("zoneOperations.get: denied"), std::string::npos);
}

// Requirement 4 AC 4: running out of time is a `gcp_operation_timeout`, so a
// caller can tell "we gave up waiting" from "GCP said no".
BOOST_AUTO_TEST_CASE(never_done_times_out_with_gcp_operation_timeout) {
    fakes::fake_compute f;
    f.zone_ops->on_get = [](const auto& req) -> google::cloud::StatusOr<fakes::cv1::Operation> {
        return fakes::make_operation(req.operation(), "RUNNING");
    };
    auto client = f.zone_operations_client();
    BOOST_CHECK_THROW(kythira::wait_for_zone_operation(client, "test-project", "us-central1-a",
                                                       "op-1", std::chrono::seconds{0}, kPoll)
                          .get(),
                      kythira::gcp_operation_timeout);
}

#ifdef FIU_ENABLE

// Requirement 20 AC 9 / Requirement 23 AC 9: the poll fault point fires before
// zoneOperations.get, so no request reaches the API.
BOOST_AUTO_TEST_CASE(poll_fault_returns_exceptional_future_without_polling) {
    fakes::fake_compute f;
    auto client = f.zone_operations_client();
    fiu_enable("raft/gcp/zone_operation/poll", 1, nullptr, 0);
    auto fut = kythira::wait_for_zone_operation(client, "test-project", "us-central1-a", "op-1",
                                                kTimeout, kPoll);
    fiu_disable("raft/gcp/zone_operation/poll");
    auto msg = what_of(std::move(fut));
    BOOST_CHECK_NE(msg.find("fault: raft/gcp/zone_operation/poll"), std::string::npos);
    BOOST_CHECK_EQUAL(f.zone_ops->get_calls, 0);
}

#endif  // FIU_ENABLE

BOOST_AUTO_TEST_SUITE_END()

// ── gcp_compute_quorum_manager over injected fakes ──────────────────────────

BOOST_AUTO_TEST_SUITE(gcp_compute_fake_clients)

BOOST_AUTO_TEST_CASE(assess_quorum_counts_only_running_instances) {
    fakes::fake_compute f;
    const std::map<std::string, std::string> labels{{"kythira-cluster", "test-cluster"}};
    f.instances->add("us-central1-a", compute_mgr::node_id_to_instance_name("test-cluster", 1),
                     "RUNNING", labels);
    f.instances->add("us-central1-a", compute_mgr::node_id_to_instance_name("test-cluster", 2),
                     "TERMINATED", labels);
    // Right name, wrong cluster label: the label filter must exclude it.
    f.instances->add("us-central1-a", compute_mgr::node_id_to_instance_name("test-cluster", 3),
                     "RUNNING", {{"kythira-cluster", "other-cluster"}});
    auto mgr = make_compute(f);

    placements cluster{{.node_id = 1, .group_id = "us-central1-a"},
                       {.node_id = 2, .group_id = "us-central1-a"},
                       {.node_id = 3, .group_id = "us-central1-a"}};
    auto health = mgr.assess_quorum(cluster).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);
    BOOST_CHECK_EQUAL(health.total_node_count, 3u);
    BOOST_CHECK(health.status == kythira::quorum_status::lost);
    BOOST_CHECK((health.unreachable_nodes == std::vector<std::uint64_t>{2, 3}));
    BOOST_CHECK_EQUAL(f.instances->list_calls, 1);
}

BOOST_AUTO_TEST_CASE(assess_quorum_list_error_is_exceptional) {
    fakes::fake_compute f;
    f.instances->list_error =
        google::cloud::Status(google::cloud::StatusCode::kUnavailable, "backend down");
    auto mgr = make_compute(f);
    placements cluster{{.node_id = 1, .group_id = "us-central1-a"}};
    auto msg = what_of(mgr.assess_quorum(cluster));
    BOOST_CHECK_NE(msg.find("backend down"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(provision_node_inserts_a_labelled_instance_and_returns_its_address) {
    fakes::fake_compute f;
    auto mgr = make_compute(f);
    auto peer = mgr.provision_node("us-central1-a", std::nullopt).get();

    BOOST_REQUIRE_EQUAL(f.instances->instances.size(), 1u);
    const auto& inst = f.instances->instances.front();
    BOOST_CHECK_EQUAL(inst.name(),
                      compute_mgr::node_id_to_instance_name("test-cluster", peer.node_id));
    BOOST_CHECK_EQUAL(inst.labels().at("kythira-cluster"), "test-cluster");
    BOOST_CHECK_EQUAL(peer.address, inst.network_interfaces(0).network_ip() + ":7000");
    // The insert operation was awaited before the RUNNING poll.
    BOOST_REQUIRE(!f.zone_ops->polled.empty());
    BOOST_CHECK_EQUAL(f.zone_ops->polled.front(), "op-insert-" + inst.name());
}

BOOST_AUTO_TEST_CASE(provision_node_insert_operation_error_is_exceptional) {
    fakes::fake_compute f;
    f.zone_ops->on_get = [](const auto& req) -> google::cloud::StatusOr<fakes::cv1::Operation> {
        return fakes::make_failed_operation(req.operation(), "ZONE_RESOURCE_POOL_EXHAUSTED",
                                            "no capacity");
    };
    auto mgr = make_compute(f);
    auto msg = what_of(mgr.provision_node("us-central1-a", std::nullopt));
    BOOST_CHECK_NE(msg.find("ZONE_RESOURCE_POOL_EXHAUSTED"), std::string::npos);
    BOOST_CHECK_EQUAL(f.instances->insert_calls, 1);  // Not a collision: no retry.
}

BOOST_AUTO_TEST_CASE(decommission_node_deletes_and_awaits_the_operation) {
    fakes::fake_compute f;
    const auto name = compute_mgr::node_id_to_instance_name("test-cluster", 42);
    f.instances->add("us-central1-a", name, "RUNNING");
    auto mgr = make_compute(f);

    BOOST_CHECK_NO_THROW(mgr.decommission_node(42).get());
    BOOST_CHECK(f.instances->instances.empty());
    BOOST_CHECK((f.zone_ops->polled == std::vector<std::string>{"op-delete-" + name}));
}

BOOST_AUTO_TEST_CASE(decommission_node_missing_instance_is_idempotent_success) {
    fakes::fake_compute f;
    auto mgr = make_compute(f);
    BOOST_CHECK_NO_THROW(mgr.decommission_node(42).get());
    BOOST_CHECK_EQUAL(f.zone_ops->get_calls, 0);
}

#ifdef FIU_ENABLE

// Requirement 23 AC 9 for `raft/gcp/zone_operation/poll` through the manager:
// the delete is issued, then awaiting its operation faults.
BOOST_AUTO_TEST_CASE(decommission_zone_operation_poll_fault_returns_exceptional_future) {
    fakes::fake_compute f;
    f.instances->add("us-central1-a", compute_mgr::node_id_to_instance_name("test-cluster", 42),
                     "RUNNING");
    auto mgr = make_compute(f);

    fiu_enable("raft/gcp/zone_operation/poll", 1, nullptr, 0);
    auto fut = mgr.decommission_node(42);
    fiu_disable("raft/gcp/zone_operation/poll");
    auto msg = what_of(std::move(fut));
    BOOST_CHECK_NE(msg.find("fault: raft/gcp/zone_operation/poll"), std::string::npos);
    BOOST_CHECK_EQUAL(f.instances->delete_calls, 1);
    BOOST_CHECK_EQUAL(f.zone_ops->get_calls, 0);
}

#endif  // FIU_ENABLE

BOOST_AUTO_TEST_SUITE_END()

// ── gcp_mig_quorum_manager over injected fakes ──────────────────────────────

BOOST_AUTO_TEST_SUITE(gcp_mig_fake_clients)

BOOST_AUTO_TEST_CASE(construction_reads_every_configured_mig) {
    fakes::fake_compute f;
    auto cfg = valid_mig_config();
    cfg.mig_by_group["us-central1-b"] = "kythira-mig-b";
    BOOST_CHECK_NO_THROW(make_mig(f, cfg));
    BOOST_CHECK_EQUAL(f.migs->get_calls, 2);
}

// Requirement 23 AC 7 / Requirement 18 AC 1: an autohealer racing kythira's own
// remediation risks split-brain, so a MIG with a policy is refused.
BOOST_AUTO_TEST_CASE(autohealing_policy_is_rejected_at_construction) {
    fakes::fake_compute f;
    f.migs->add("kythira-mig-a", 3, /*autohealing=*/true);
    try {
        make_mig(f);
        BOOST_FAIL("expected std::invalid_argument");
    } catch (const std::invalid_argument& ex) {
        BOOST_CHECK_NE(std::string(ex.what()).find("autoHealingPolicies"), std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(autohealing_policy_on_any_mig_is_rejected) {
    fakes::fake_compute f;
    auto cfg = valid_mig_config();
    cfg.mig_by_group["us-central1-b"] = "kythira-mig-b";
    f.migs->add("kythira-mig-b", 3, /*autohealing=*/true);
    BOOST_CHECK_THROW(make_mig(f, cfg), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(unreadable_mig_is_rejected_at_construction) {
    fakes::fake_compute f;
    auto cfg = fast(valid_mig_config());
    // Not registered with the fake: instanceGroupManagers.get reports NOT_FOUND.
    BOOST_CHECK_THROW(
        (mig_mgr{cfg, f.migs_client(), f.instances_client(), f.zone_operations_client()}),
        std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(assess_quorum_resolves_nodes_by_node_id_label) {
    fakes::fake_compute f;
    f.instances->add("us-central1-a", "kythira-mig-a-abcd", "RUNNING",
                     {{"kythira-cluster", "test-cluster"}, {"kythira-node-id", "7"}});
    f.instances->add("us-central1-a", "kythira-mig-a-efgh", "STOPPING",
                     {{"kythira-cluster", "test-cluster"}, {"kythira-node-id", "8"}});
    auto mgr = make_mig(f);

    placements cluster{{.node_id = 7, .group_id = "us-central1-a"},
                       {.node_id = 8, .group_id = "us-central1-a"}};
    auto health = mgr.assess_quorum(cluster).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);
    BOOST_CHECK((health.unreachable_nodes == std::vector<std::uint64_t>{8}));
}

#ifdef FIU_ENABLE

// Requirement 23 AC 9 for `raft/gcp/zone_operation/poll` through the MIG
// manager: deleteInstances is issued, then awaiting its operation faults.
BOOST_AUTO_TEST_CASE(decommission_zone_operation_poll_fault_returns_exceptional_future) {
    fakes::fake_compute f;
    f.instances->add("us-central1-a", "kythira-mig-a-abcd", "RUNNING",
                     {{"kythira-cluster", "test-cluster"}, {"kythira-node-id", "42"}});
    auto mgr = make_mig(f);

    fiu_enable("raft/gcp/zone_operation/poll", 1, nullptr, 0);
    auto fut = mgr.decommission_node(42);
    fiu_disable("raft/gcp/zone_operation/poll");
    auto msg = what_of(std::move(fut));
    BOOST_CHECK_NE(msg.find("fault: raft/gcp/zone_operation/poll"), std::string::npos);
    BOOST_CHECK_EQUAL(f.migs->delete_instances_calls, 1);
    BOOST_CHECK_EQUAL(f.zone_ops->get_calls, 0);
}

#endif  // FIU_ENABLE

BOOST_AUTO_TEST_SUITE_END()

// ── MIG timeout rollback (group-scale-up-rollback task 8) ───────────────────

namespace {

/// A MIG of three labelled voters whose `resize` behaves like the real one:
/// growing creates members in `launch_action`, and shrinking deletes members
/// the MIG chooses itself, modelled as oldest first. That choice is what made
/// the old blind restore cost a voter.
struct mig_cloud {
    fakes::fake_compute f;
    const std::string zone = "us-central1-a";
    const std::string mig = "kythira-mig-a";
    std::vector<std::string> voters{"kythira-mig-a-v1", "kythira-mig-a-v2", "kythira-mig-a-v3"};
    std::string launch_action = "CREATING";  ///< Empty: create nothing.
    bool finish_creating_on_grow = false;
    std::string last_launch;
    int blind_deletes = 0;
    int launches = 0;

    mig_cloud() {
        f.migs->add(mig, 0);
        for (std::size_t i = 0; i < voters.size(); ++i) {
            seed(voters[i], "NONE",
                 {{"kythira-cluster", "test-cluster"}, {"kythira-node-id", std::to_string(i + 1)}});
        }
        f.migs->on_resize = [this](const std::string& name, std::int32_t from, std::int32_t to) {
            auto& members = f.migs->managed[name];
            if (to > from && finish_creating_on_grow) {
                for (auto& mi : members) {
                    if (mi.current_action() == "CREATING") {
                        mi.set_current_action("NONE");
                    }
                }
            }
            for (auto n = from; n < to && !launch_action.empty(); ++n) {
                last_launch = mig + "-n" + std::to_string(++launches);
                f.instances->add(zone, last_launch, "RUNNING");
                members.push_back(managed(last_launch, launch_action));
            }
            while (static_cast<std::int32_t>(members.size()) > to) {
                members.erase(members.begin());
                ++blind_deletes;
            }
        };
    }

    [[nodiscard]] static auto url(const std::string& name) -> std::string {
        return "https://www.googleapis.com/compute/v1/projects/test-project/zones/"
               "us-central1-a/instances/" +
               name;
    }
    [[nodiscard]] static auto managed(const std::string& name, const std::string& action)
        -> fakes::cv1::ManagedInstance {
        fakes::cv1::ManagedInstance mi;
        mi.set_instance(url(name));
        mi.set_current_action(action);
        return mi;
    }

    void seed(const std::string& name, const std::string& action,
              std::map<std::string, std::string> labels = {}) {
        f.instances->add(zone, name, "RUNNING", std::move(labels));
        f.migs->managed[mig].push_back(managed(name, action));
        f.migs->migs[mig].set_target_size(f.migs->migs[mig].target_size() + 1);
    }

    [[nodiscard]] auto members() const -> std::vector<std::string> {
        std::vector<std::string> out;
        for (const auto& mi : f.migs->managed.at(mig)) {
            out.push_back(kythira::gcp_mig_detail::last_path_segment(mi.instance()));
        }
        return out;
    }
    [[nodiscard]] auto target_size() const -> std::int32_t {
        return f.migs->migs.at(mig).target_size();
    }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(gcp_mig_timeout_rollback)

// The new instance never finishes CREATING. The old path resized back and
// the MIG deleted a member of its choosing; here, the oldest: a voter.
BOOST_AUTO_TEST_CASE(a_creating_instance_is_deleted_by_url_and_every_voter_kept) {
    mig_cloud cloud;
    auto mgr = make_mig(cloud.f);

    const auto msg = what_of(mgr.provision_node("us-central1-a", std::nullopt));
    BOOST_CHECK_NE(msg.find("rollback: removed " + cloud.last_launch + " (fresh, CREATING)"),
                   std::string::npos);
    BOOST_CHECK(cloud.members() == cloud.voters);
    BOOST_CHECK_EQUAL(cloud.target_size(), 3);
    BOOST_CHECK_EQUAL(cloud.blind_deletes, 0);
    BOOST_CHECK_EQUAL(cloud.f.migs->resize_calls, 1);  // The grow; no blind restore.
    BOOST_CHECK(cloud.f.migs->deleted ==
                std::vector<std::string>{mig_cloud::url(cloud.last_launch)});
    BOOST_CHECK(cloud.f.migs->every_delete_skipped_validation_errors);
}

// An unlabelled member the MIG was still creating before the resize, which
// finishes during the wait, is not this provision's: the old unlabelled-only
// test adopted it.
BOOST_AUTO_TEST_CASE(a_pre_existing_creating_instance_is_neither_adopted_nor_deleted) {
    mig_cloud cloud;
    cloud.seed("kythira-mig-a-early", "CREATING");
    cloud.finish_creating_on_grow = true;
    auto mgr = make_mig(cloud.f);

    const auto msg = what_of(mgr.provision_node("us-central1-a", std::nullopt));
    BOOST_CHECK_NE(msg.find("rollback: removed " + cloud.last_launch), std::string::npos);
    const auto members = cloud.members();
    BOOST_CHECK(std::ranges::find(members, "kythira-mig-a-early") != members.end());
    const auto* early = cloud.f.instances->find("us-central1-a", "kythira-mig-a-early");
    BOOST_REQUIRE(early != nullptr);
    BOOST_CHECK(early->labels().find("kythira-node-id") == early->labels().end());
}

BOOST_AUTO_TEST_CASE(nothing_created_resizes_back_and_audits) {
    mig_cloud cloud;
    cloud.launch_action.clear();
    auto mgr = make_mig(cloud.f);

    const auto msg = what_of(mgr.provision_node("us-central1-a", std::nullopt));
    BOOST_CHECK_NE(msg.find("rollback: desired size restored to 3"), std::string::npos);
    BOOST_CHECK_EQUAL(msg.find("lost member"), std::string::npos);
    BOOST_CHECK_EQUAL(cloud.f.migs->resize_calls, 2);
    BOOST_CHECK(cloud.members() == cloud.voters);
}

// Task 8.3: the restoring resize's result used to be discarded.
BOOST_AUTO_TEST_CASE(a_failed_restore_is_reported) {
    mig_cloud cloud;
    cloud.launch_action.clear();
    cloud.f.zone_ops->on_get =
        [&](const auto& req) -> google::cloud::StatusOr<fakes::cv1::Operation> {
        if (cloud.f.migs->resize_calls >= 2) {
            return fakes::make_failed_operation(req.operation(), "QUOTA_EXCEEDED", "no room");
        }
        return fakes::make_operation(req.operation());
    };
    auto mgr = make_mig(cloud.f);

    const auto msg = what_of(mgr.provision_node("us-central1-a", std::nullopt));
    BOOST_CHECK_NE(msg.find("restoring the desired size failed"), std::string::npos);
    BOOST_CHECK_NE(msg.find("no room"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(current_actions_map_onto_the_planners_classes) {
    using kythira::gcp_mig_detail::rollback_state;
    using kythira::group_rollback::member_state;
    BOOST_CHECK(rollback_state("NONE") == member_state::live);
    BOOST_CHECK(rollback_state("CREATING") == member_state::pending);
    BOOST_CHECK(rollback_state("VERIFYING") == member_state::pending);
    BOOST_CHECK(rollback_state("RECREATING") == member_state::pending);
    BOOST_CHECK(rollback_state("DELETING") == member_state::terminal);
    BOOST_CHECK(rollback_state("ABANDONING") == member_state::terminal);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_GCP_SDK

BOOST_AUTO_TEST_CASE(skipped_no_gcp_sdk) {
    BOOST_TEST_MESSAGE(
        "KYTHIRA_HAS_GCP_SDK not defined — quorum-manager tests skipped "
        "(validator tests above still ran)");
}

#endif  // KYTHIRA_HAS_GCP_SDK
