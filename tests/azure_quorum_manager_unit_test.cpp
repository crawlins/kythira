// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE azure_quorum_manager_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_AZURE_SDK

#include <raft/azure_vm_quorum_manager.hpp>
#include <raft/azure_vmss_quorum_manager.hpp>
#include <raft/elastic_capacity_controller.hpp>

#include <azure/core/credentials/credentials.hpp>
#include <azure/core/http/raw_response.hpp>
#include <azure/core/http/transport.hpp>
#include <azure/core/io/body_stream.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
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

// ── Idempotency keys (elastic-shard-capacity Requirement 8.2) ─────────────────
//
// Driven through an in-memory `HttpTransport` rather than a socket: the subject
// is what the manager puts in its ARM requests and how it reads ARM's answers,
// not the wire, and an in-process double needs neither the TLS server nor the
// certificate fixture that `azure_vm_quorum_manager_lro_timeout_test` brings in
// for the bearer-token policy's https check. The endpoint is still `https://`,
// so that policy runs exactly as in production.

namespace {

/// Hands out a token without contacting anything. Same approach as
/// `azure_vm_quorum_manager_lro_timeout_test.cpp`'s.
class FakeTokenCredential : public Azure::Core::Credentials::TokenCredential {
public:
    FakeTokenCredential() : TokenCredential("FakeTokenCredential") {}

    [[nodiscard]] auto GetToken(const Azure::Core::Credentials::TokenRequestContext& /*request*/,
                                const Azure::Core::Context& /*context*/) const
        -> Azure::Core::Credentials::AccessToken override {
        Azure::Core::Credentials::AccessToken token;
        token.Token = "fake-token";
        token.ExpiresOn = Azure::DateTime(std::chrono::system_clock::now() + std::chrono::hours{1});
        return token;
    }
};

/// One request the manager sent.
struct arm_call {
    std::string method;
    std::string url;
    std::string body;
};

/// ARM, as far as `provision_node` and `find_by_idempotency_key` need it: a
/// resource group whose VM list is `pages` (each page a JSON body), NIC and VM
/// PUTs that succeed, VMs that are running, and NICs whose IP is 10.0.0.5.
class ArmDouble : public Azure::Core::Http::HttpTransport {
public:
    std::vector<std::string> pages{R"({"value":[]})"};

    auto Send(Azure::Core::Http::Request& request, const Azure::Core::Context& context)
        -> std::unique_ptr<Azure::Core::Http::RawResponse> override {
        arm_call call{.method = request.GetMethod().ToString(),
                      .url = request.GetUrl().GetAbsoluteUrl(),
                      .body = {}};
        if (auto* stream = request.GetBodyStream(); stream != nullptr) {
            auto bytes = stream->ReadToEnd(context);
            call.body.assign(bytes.begin(), bytes.end());
        }
        std::string reply = "{}";
        const auto& url = call.url;
        if (call.method == "GET" && url.find("/page/") != std::string::npos) {
            reply = pages.at(std::stoul(url.substr(url.find("/page/") + 6)));
        } else if (call.method == "GET" && url.find("/virtualMachines?") != std::string::npos) {
            reply = pages.at(0);
        } else if (call.method == "GET" && url.find("/instanceView") != std::string::npos) {
            reply = R"({"statuses":[{"code":"PowerState/running"}]})";
        } else if (call.method == "GET" && url.find("/networkInterfaces/") != std::string::npos) {
            reply = R"({"properties":{"ipConfigurations":[)"
                    R"({"properties":{"privateIPAddress":"10.0.0.5"}}]}})";
        }
        auto response = std::make_unique<Azure::Core::Http::RawResponse>(
            1, 1, Azure::Core::Http::HttpStatusCode::Ok, "OK");
        response->SetHeader("Content-Type", "application/json");
        // A body *stream*, as a real transport hands back: the pipeline's
        // transport policy reads the stream into the body itself, and has
        // nothing to read if only SetBody was called. The stream does not own
        // its bytes, so they live in `_bodies` (a deque: growing it never
        // moves an existing element) for as long as the double does.
        std::lock_guard lock(_mu);
        _calls.push_back(std::move(call));
        const auto& bytes = _bodies.emplace_back(reply.begin(), reply.end());
        response->SetBodyStream(std::make_unique<Azure::Core::IO::MemoryBodyStream>(bytes));
        return response;
    }

    [[nodiscard]] auto calls() const -> std::vector<arm_call> {
        std::lock_guard lock(_mu);
        return _calls;
    }

    /// The body of the one VM PUT the manager sent.
    [[nodiscard]] auto vm_put() const -> boost::json::value {
        for (const auto& c : calls()) {
            if (c.method == "PUT" && c.url.find("/virtualMachines/") != std::string::npos) {
                return boost::json::parse(c.body);
            }
        }
        BOOST_FAIL("no VM PUT was sent");
        return {};
    }

private:
    mutable std::mutex _mu;
    std::vector<arm_call> _calls;
    std::deque<std::vector<std::uint8_t>> _bodies;
};

auto keyed_vm_config(const std::shared_ptr<ArmDouble>& arm)
    -> kythira::azure_vm_quorum_manager_config {
    auto cfg = make_vm_config();
    cfg.azure.arm_endpoint_override = "https://arm.test";
    cfg.azure.credential = std::make_shared<FakeTokenCredential>();
    cfg.azure.transport = arm;
    cfg.poll_interval = std::chrono::milliseconds{1};
    cfg.provision_timeout = std::chrono::seconds{5};
    return cfg;
}

/// One VM-list entry: `name`, its tags, and its provisioningState.
auto vm_entry(const std::string& name, const std::string& cluster, const std::string& key,
              const std::string& state = "Succeeded") -> std::string {
    return R"({"name":")" + name + R"(","tags":{"kythira:cluster":")" + cluster +
           R"(","kythira:node-id":")" + name.substr(name.rfind('-') + 1) +
           R"(","kythira:idempotency-key":")" + key + R"("},"properties":{"provisioningState":")" +
           state + R"("}})";
}

}  // namespace

BOOST_AUTO_TEST_SUITE(vm_idempotency_key)

using vm_mgr_t = kythira::azure_vm_quorum_manager<std::uint64_t, std::string>;

// The capacity controller detects keyed provisioning by this concept; a
// signature drift would silently drop the manager back to unkeyed matching.
static_assert(kythira::keyed_quorum_manager<vm_mgr_t>);

BOOST_AUTO_TEST_CASE(keyed_provision_puts_the_key_on_the_vm) {
    auto arm = std::make_shared<ArmDouble>();
    vm_mgr_t mgr{keyed_vm_config(arm)};
    const std::string key = "cap-7-1759363200000-9f86d081884c7d65";

    auto peer = mgr.provision_node_keyed("1", std::nullopt, key).get();
    BOOST_CHECK_EQUAL(peer.address, "10.0.0.5:7000");

    const auto body = arm->vm_put();
    const auto& tags = body.at("tags").as_object();
    BOOST_REQUIRE(tags.contains("kythira:idempotency-key"));
    BOOST_CHECK_EQUAL(std::string(tags.at("kythira:idempotency-key").as_string()), key);
    BOOST_CHECK_EQUAL(std::string(tags.at("kythira:cluster").as_string()), "test-cluster");
}

BOOST_AUTO_TEST_CASE(unkeyed_provision_carries_no_key_tag) {
    auto arm = std::make_shared<ArmDouble>();
    vm_mgr_t mgr{keyed_vm_config(arm)};
    static_cast<void>(mgr.provision_node("1", std::nullopt).get());
    BOOST_CHECK(!arm->vm_put().at("tags").as_object().contains("kythira:idempotency-key"));
}

BOOST_AUTO_TEST_CASE(key_tag_wins_over_an_extra_tag_of_the_same_name) {
    auto arm = std::make_shared<ArmDouble>();
    auto cfg = keyed_vm_config(arm);
    cfg.extra_tags["kythira:idempotency-key"] = "stale";
    vm_mgr_t mgr{cfg};
    static_cast<void>(mgr.provision_node_keyed("1", std::nullopt, "cap-1-2-3").get());
    BOOST_CHECK_EQUAL(
        std::string(arm->vm_put().at("tags").at("kythira:idempotency-key").as_string()),
        "cap-1-2-3");
}

BOOST_AUTO_TEST_CASE(find_returns_this_clusters_vm_carrying_the_key) {
    auto arm = std::make_shared<ArmDouble>();
    arm->pages = {R"({"value":[)" +
                  // Same key, another cluster.
                  vm_entry("kythira-other-cluster-1", "other-cluster", "cap-1-2-3") + "," +
                  // This cluster, another key.
                  vm_entry("kythira-test-cluster-2", "test-cluster", "cap-9-9-9") + "," +
                  // This cluster and key, but already being deleted.
                  vm_entry("kythira-test-cluster-3", "test-cluster", "cap-1-2-3", "Deleting") +
                  "," +
                  // The one: a deallocated VM still has to be found, so it can be reaped.
                  vm_entry("kythira-test-cluster-4", "test-cluster", "cap-1-2-3") + "]}"};
    vm_mgr_t mgr{keyed_vm_config(arm)};

    auto found = mgr.find_by_idempotency_key("cap-1-2-3").get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(found->node_id, 4u);
    BOOST_CHECK_EQUAL(found->address, "10.0.0.5:7000");
}

BOOST_AUTO_TEST_CASE(find_returns_nullopt_when_no_vm_carries_the_key) {
    auto arm = std::make_shared<ArmDouble>();
    arm->pages = {R"({"value":[)" + vm_entry("kythira-test-cluster-2", "test-cluster", "cap-9") +
                  "]}"};
    vm_mgr_t mgr{keyed_vm_config(arm)};
    BOOST_CHECK(!mgr.find_by_idempotency_key("cap-1-2-3").get().has_value());
}

BOOST_AUTO_TEST_CASE(find_follows_next_link) {
    auto arm = std::make_shared<ArmDouble>();
    arm->pages = {
        R"({"value":[],"nextLink":"https://arm.test/page/1"})",
        R"({"value":[)" + vm_entry("kythira-test-cluster-5", "test-cluster", "cap-1-2-3") + "]}"};
    vm_mgr_t mgr{keyed_vm_config(arm)};

    auto found = mgr.find_by_idempotency_key("cap-1-2-3").get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(found->node_id, 5u);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Node id floor ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(vm_node_id_floor)

using vm_mgr_t = kythira::azure_vm_quorum_manager<std::uint64_t, std::string>;

// Only VM 2 is still listed; 5 was assessed but its VM is gone (evicted, or
// deleted by hand). The tag scan alone would hand out 3, then 3 again; the
// floor starts above every id assessed or allocated instead.
BOOST_AUTO_TEST_CASE(ids_assessed_or_allocated_are_never_reassigned) {
    auto arm = std::make_shared<ArmDouble>();
    arm->pages = {R"({"value":[)" + vm_entry("kythira-test-cluster-2", "test-cluster", "k") + "]}"};
    vm_mgr_t mgr{keyed_vm_config(arm)};

    std::vector<kythira::node_placement<std::uint64_t, std::string>> members{
        {.node_id = 2, .group_id = "1"}, {.node_id = 5, .group_id = "1"}};
    static_cast<void>(mgr.assess_quorum(members).get());

    BOOST_CHECK_EQUAL(mgr.provision_node("1", std::nullopt).get().node_id, 6u);
    BOOST_CHECK_EQUAL(std::string(arm->vm_put().at("tags").at("kythira:node-id").as_string()), "6");
    // The double's VM list still shows only 2.
    BOOST_CHECK_EQUAL(mgr.provision_node("1", std::nullopt).get().node_id, 7u);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Fault injection ────────────────────────────────────────────────────────────

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(fault_injection)

BOOST_AUTO_TEST_CASE(vm_find_by_idempotency_key_fault) {
    kythira::azure_vm_quorum_manager<> mgr{make_vm_config()};
    fiu_enable("raft/azure/vm/find_by_idempotency_key", 1, nullptr, 0);
    auto fut = mgr.find_by_idempotency_key("cap-1-2-3");
    fiu_disable("raft/azure/vm/find_by_idempotency_key");
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

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
