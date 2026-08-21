// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE azure_quorum_manager_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_AZURE_SDK

#include <raft/azure_vm_quorum_manager.hpp>
#include <raft/azure_vmss_quorum_manager.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

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

// Lets a unit test construct a working azure_vmss_quorum_manager without a
// live scale set to query, mirroring aws_quorum_manager_unit_test.cpp's
// AsgSkipHealthCheckFixture for the analogous AWS ASG health-check probe.
struct VmssSkipUpgradePolicyFixture {
#ifdef FIU_ENABLE
    VmssSkipUpgradePolicyFixture() {
        fiu_enable("raft/azure/vmss/skip_upgrade_policy_validation", 1, nullptr, 0);
    }
    ~VmssSkipUpgradePolicyFixture() {
        fiu_disable("raft/azure/vmss/skip_upgrade_policy_validation");
    }
#endif
};

auto make_vm_config() -> kythira::azure_vm_quorum_manager_config {
    kythira::azure_vm_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.azure.subscription_id = "00000000-0000-0000-0000-000000000000";
    cfg.azure.resource_group = "test-rg";
    cfg.azure.location = "eastus";
    cfg.node_port = 7000;
    cfg.image_reference.publisher = "Canonical";
    cfg.image_reference.offer = "0001-com-ubuntu-server-jammy";
    cfg.image_reference.sku = "22_04-lts";
    cfg.image_reference.version = "latest";
    cfg.topology.groups.push_back({.group_id = "1", .target_count = 3});
    cfg.subnet_id_by_group["1"] =
        "/subscriptions/00000000-0000-0000-0000-000000000000/resourceGroups/test-rg/providers/"
        "Microsoft.Network/virtualNetworks/test-vnet/subnets/test-subnet";
    return cfg;
}

auto make_vmss_config() -> kythira::azure_vmss_quorum_manager_config {
    kythira::azure_vmss_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.azure.subscription_id = "00000000-0000-0000-0000-000000000000";
    cfg.azure.resource_group = "test-rg";
    cfg.azure.location = "eastus";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "1", .target_count = 3});
    cfg.scale_set_by_group["1"] = "test-vmss";
    return cfg;
}

}  // namespace

// ── azure_vm_quorum_manager construction ──────────────────────────────────────

BOOST_AUTO_TEST_SUITE(vm_construction)

BOOST_AUTO_TEST_CASE(valid_config_constructs) {
    auto cfg = make_vm_config();
    BOOST_CHECK_NO_THROW((kythira::azure_vm_quorum_manager<>{cfg}));
}

BOOST_AUTO_TEST_CASE(empty_cluster_name_throws) {
    auto cfg = make_vm_config();
    cfg.cluster_name.clear();
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_subscription_id_throws) {
    auto cfg = make_vm_config();
    cfg.azure.subscription_id.clear();
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_resource_group_throws) {
    auto cfg = make_vm_config();
    cfg.azure.resource_group.clear();
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_location_throws) {
    auto cfg = make_vm_config();
    cfg.azure.location.clear();
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(zero_node_port_throws) {
    auto cfg = make_vm_config();
    cfg.node_port = 0;
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(image_reference_both_forms_set_throws) {
    auto cfg = make_vm_config();
    cfg.image_reference.shared_gallery_image_id =
        "/subscriptions/00000000-0000-0000-0000-000000000000/resourceGroups/test-rg/providers/"
        "Microsoft.Compute/galleries/test-gallery/images/test-image/versions/1.0.0";
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(image_reference_neither_form_set_throws) {
    auto cfg = make_vm_config();
    cfg.image_reference = kythira::azure_image_reference{};
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(missing_subnet_for_topology_group_throws) {
    auto cfg = make_vm_config();
    cfg.topology.groups.push_back({.group_id = "2", .target_count = 1});
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

// ── azure_vmss_quorum_manager construction ────────────────────────────────────

BOOST_AUTO_TEST_SUITE(vmss_construction)

BOOST_FIXTURE_TEST_CASE(valid_config_constructs, VmssSkipUpgradePolicyFixture) {
    auto cfg = make_vmss_config();
    BOOST_CHECK_NO_THROW((kythira::azure_vmss_quorum_manager<>{cfg}));
}

BOOST_AUTO_TEST_CASE(empty_cluster_name_throws) {
    auto cfg = make_vmss_config();
    cfg.cluster_name.clear();
    BOOST_CHECK_THROW((kythira::azure_vmss_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_scale_set_by_group_throws) {
    auto cfg = make_vmss_config();
    cfg.scale_set_by_group.clear();
    BOOST_CHECK_THROW((kythira::azure_vmss_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(missing_scale_set_for_topology_group_throws) {
    auto cfg = make_vmss_config();
    cfg.topology.groups.push_back({.group_id = "2", .target_count = 1});
    BOOST_CHECK_THROW((kythira::azure_vmss_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Unknown-group provision futures ───────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(unknown_group_provision)

BOOST_AUTO_TEST_CASE(vm_provision_unknown_group_returns_exceptional_future) {
    kythira::azure_vm_quorum_manager<> mgr{make_vm_config()};
    auto fut = mgr.provision_node("no-such-group", std::nullopt);
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_FIXTURE_TEST_CASE(vmss_provision_unknown_group_returns_exceptional_future,
                        VmssSkipUpgradePolicyFixture) {
    kythira::azure_vmss_quorum_manager<> mgr{make_vmss_config()};
    auto fut = mgr.provision_node("no-such-group", std::nullopt);
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

// ── NodeId <-> VM name round trip ─────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(node_id_vm_name)

BOOST_AUTO_TEST_CASE(node_id_to_vm_name_round_trip) {
    kythira::azure_vm_quorum_manager<std::uint64_t, std::string> mgr{make_vm_config()};
    for (std::uint64_t id :
         {std::uint64_t{0}, std::uint64_t{1}, std::numeric_limits<std::uint64_t>::max()}) {
        auto name = mgr.node_id_to_vm_name(id);
        auto round_tripped = mgr.vm_name_to_node_id(name);
        BOOST_REQUIRE(round_tripped.has_value());
        BOOST_CHECK_EQUAL(*round_tripped, id);
    }
}

BOOST_AUTO_TEST_CASE(vm_name_to_node_id_rejects_foreign_names) {
    kythira::azure_vm_quorum_manager<std::uint64_t, std::string> mgr{make_vm_config()};
    BOOST_CHECK(!mgr.vm_name_to_node_id("not-a-kythira-vm").has_value());
    BOOST_CHECK(!mgr.vm_name_to_node_id("kythira-other-cluster-1").has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Placement/priority/spot config structs ────────────────────────────────────

BOOST_AUTO_TEST_SUITE(placement_priority_config)

BOOST_AUTO_TEST_CASE(placement_config_ppg_valid) {
    kythira::azure_placement_config cfg{
        .kind = kythira::azure_placement_kind::proximity_placement_group,
        .resource_id =
            "/subscriptions/00000000-0000-0000-0000-000000000000/resourceGroups/test-rg/"
            "providers/Microsoft.Compute/proximityPlacementGroups/test-ppg"};
    BOOST_CHECK(cfg.kind == kythira::azure_placement_kind::proximity_placement_group);
    BOOST_CHECK(!cfg.resource_id.empty());
}

BOOST_AUTO_TEST_CASE(spot_options_default_is_regular) {
    kythira::azure_vm_quorum_manager_config cfg = make_vm_config();
    BOOST_CHECK(cfg.priority == kythira::azure_vm_priority::regular);
    BOOST_CHECK(!cfg.spot_options.has_value());
}

BOOST_AUTO_TEST_CASE(spot_options_populates_correctly) {
    kythira::azure_spot_options spot{.max_price = 0.05,
                                     .eviction_policy = kythira::azure_eviction_policy::delete_vm};
    BOOST_CHECK_CLOSE(spot.max_price, 0.05, 0.0001);
    BOOST_CHECK(spot.eviction_policy == kythira::azure_eviction_policy::delete_vm);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Fault injection ────────────────────────────────────────────────────────────

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(fault_injection)

struct FaultPointFixture {
    explicit FaultPointFixture(std::string name) : _name(std::move(name)) {
        fiu_enable(_name.c_str(), 1, nullptr, 0);
    }
    ~FaultPointFixture() { fiu_disable(_name.c_str()); }
    std::string _name;
};

BOOST_AUTO_TEST_CASE(vm_assess_quorum_fault) {
    FaultPointFixture fp("raft/azure/vm/get_instance_view");
    kythira::azure_vm_quorum_manager<> mgr{make_vm_config()};
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{1, "1"}};
    BOOST_CHECK_THROW(std::move(mgr.assess_quorum(cluster)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vm_provision_node_fault) {
    FaultPointFixture fp("raft/azure/vm/create_vm");
    kythira::azure_vm_quorum_manager<> mgr{make_vm_config()};
    BOOST_CHECK_THROW(std::move(mgr.provision_node("1", std::nullopt)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vm_decommission_node_fault) {
    FaultPointFixture fp("raft/azure/vm/delete_vm");
    kythira::azure_vm_quorum_manager<> mgr{make_vm_config()};
    BOOST_CHECK_THROW(std::move(mgr.decommission_node(1)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vm_maintain_quorum_fault) {
    FaultPointFixture fp("raft/azure/vm/maintain_quorum");
    kythira::azure_vm_quorum_manager<> mgr{make_vm_config()};
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{1, "1"}};
    BOOST_CHECK_THROW(std::move(mgr.maintain_quorum(cluster)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vmss_assess_quorum_fault) {
    VmssSkipUpgradePolicyFixture skip;
    FaultPointFixture fp("raft/azure/vmss/list_instances");
    kythira::azure_vmss_quorum_manager<> mgr{make_vmss_config()};
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{1, "1"}};
    BOOST_CHECK_THROW(std::move(mgr.assess_quorum(cluster)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vmss_provision_node_fault) {
    VmssSkipUpgradePolicyFixture skip;
    FaultPointFixture fp("raft/azure/vmss/update_capacity");
    kythira::azure_vmss_quorum_manager<> mgr{make_vmss_config()};
    BOOST_CHECK_THROW(std::move(mgr.provision_node("1", std::nullopt)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vmss_decommission_node_fault) {
    VmssSkipUpgradePolicyFixture skip;
    FaultPointFixture fp("raft/azure/vmss/delete_instance");
    kythira::azure_vmss_quorum_manager<> mgr{make_vmss_config()};
    BOOST_CHECK_THROW(std::move(mgr.decommission_node(1)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vmss_maintain_quorum_fault) {
    VmssSkipUpgradePolicyFixture skip;
    FaultPointFixture fp("raft/azure/vmss/maintain_quorum");
    kythira::azure_vmss_quorum_manager<> mgr{make_vmss_config()};
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{1, "1"}};
    BOOST_CHECK_THROW(std::move(mgr.maintain_quorum(cluster)).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE

#else  // !KYTHIRA_HAS_AZURE_SDK

BOOST_AUTO_TEST_CASE(azure_sdk_not_available) {
    BOOST_TEST_MESSAGE("Azure SDK not available at build time; azure quorum manager tests skipped");
}

#endif  // KYTHIRA_HAS_AZURE_SDK
