// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE azure_quorum_manager_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_AZURE_SDK

#include <raft/azure_vm_quorum_manager.hpp>
#include <raft/azure_vmss_quorum_manager.hpp>
#include <raft/elastic_capacity_controller.hpp>
#include <raft/generated_node_name.hpp>

#include <azure/core/base64.hpp>
#include <azure/core/credentials/credentials.hpp>
#include <azure/core/http/raw_response.hpp>
#include <azure/core/http/transport.hpp>
#include <azure/core/io/body_stream.hpp>

#include <boost/json.hpp>

#include <concepts>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#endif

#include "ordered_global_fixtures.hpp"

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
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
#else
using FollyInitFixture = kythira::testing::no_global_fixture;
#endif

#ifdef FIU_ENABLE
struct FiuInitFixture {
    FiuInitFixture() { fiu_init(0); }
};
#else
using FiuInitFixture = kythira::testing::no_global_fixture;
#endif

// One registration, constructed in this order and destroyed in reverse:
// separately registered global fixtures run in link-address order, not
// declaration order (see ordered_global_fixtures.hpp).
// folly::Init first, then fiu_init.
using SuiteGlobalFixtures =
    kythira::testing::ordered_global_fixtures<FollyInitFixture, FiuInitFixture>;
BOOST_GLOBAL_FIXTURE(SuiteGlobalFixtures);

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
    BOOST_CHECK_NO_THROW((kythira::azure_vm_quorum_manager<std::uint64_t>{cfg}));
}

BOOST_AUTO_TEST_CASE(empty_cluster_name_throws) {
    auto cfg = make_vm_config();
    cfg.cluster_name.clear();
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<std::uint64_t>{cfg}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_subscription_id_throws) {
    auto cfg = make_vm_config();
    cfg.azure.subscription_id.clear();
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<std::uint64_t>{cfg}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_resource_group_throws) {
    auto cfg = make_vm_config();
    cfg.azure.resource_group.clear();
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<std::uint64_t>{cfg}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_location_throws) {
    auto cfg = make_vm_config();
    cfg.azure.location.clear();
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<std::uint64_t>{cfg}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(zero_node_port_throws) {
    auto cfg = make_vm_config();
    cfg.node_port = 0;
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<std::uint64_t>{cfg}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(image_reference_both_forms_set_throws) {
    auto cfg = make_vm_config();
    cfg.image_reference.shared_gallery_image_id =
        "/subscriptions/00000000-0000-0000-0000-000000000000/resourceGroups/test-rg/providers/"
        "Microsoft.Compute/galleries/test-gallery/images/test-image/versions/1.0.0";
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<std::uint64_t>{cfg}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(image_reference_neither_form_set_throws) {
    auto cfg = make_vm_config();
    cfg.image_reference = kythira::azure_image_reference{};
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<std::uint64_t>{cfg}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(missing_subnet_for_topology_group_throws) {
    auto cfg = make_vm_config();
    cfg.topology.groups.push_back({.group_id = "2", .target_count = 1});
    BOOST_CHECK_THROW((kythira::azure_vm_quorum_manager<std::uint64_t>{cfg}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

// ── azure_vmss_quorum_manager construction ────────────────────────────────────

BOOST_AUTO_TEST_SUITE(vmss_construction)

BOOST_FIXTURE_TEST_CASE(valid_config_constructs, VmssSkipUpgradePolicyFixture) {
    auto cfg = make_vmss_config();
    BOOST_CHECK_NO_THROW((kythira::azure_vmss_quorum_manager<std::uint64_t>{cfg}));
}

BOOST_AUTO_TEST_CASE(empty_cluster_name_throws) {
    auto cfg = make_vmss_config();
    cfg.cluster_name.clear();
    BOOST_CHECK_THROW((kythira::azure_vmss_quorum_manager<std::uint64_t>{cfg}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_scale_set_by_group_throws) {
    auto cfg = make_vmss_config();
    cfg.scale_set_by_group.clear();
    BOOST_CHECK_THROW((kythira::azure_vmss_quorum_manager<std::uint64_t>{cfg}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(missing_scale_set_for_topology_group_throws) {
    auto cfg = make_vmss_config();
    cfg.topology.groups.push_back({.group_id = "2", .target_count = 1});
    BOOST_CHECK_THROW((kythira::azure_vmss_quorum_manager<std::uint64_t>{cfg}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Unknown-group provision futures ───────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(unknown_group_provision)

BOOST_AUTO_TEST_CASE(vm_provision_unknown_group_returns_exceptional_future) {
    kythira::azure_vm_quorum_manager<std::uint64_t> mgr{make_vm_config()};
    auto fut = mgr.provision_node("no-such-group", std::nullopt);
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_FIXTURE_TEST_CASE(vmss_provision_unknown_group_returns_exceptional_future,
                        VmssSkipUpgradePolicyFixture) {
    kythira::azure_vmss_quorum_manager<std::uint64_t> mgr{make_vmss_config()};
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

// cloud-composite-node-ids Requirement 7.1: only the name this manager would
// give an id maps back to it. "-07" is not node 7's VM, and "-1" (which
// std::stoull accepted as the largest uint64) is no node's.
BOOST_AUTO_TEST_CASE(vm_name_to_node_id_rejects_non_canonical_suffixes) {
    kythira::azure_vm_quorum_manager<std::uint64_t, std::string> mgr{make_vm_config()};
    for (const char* name :
         {"kythira-test-cluster-07", "kythira-test-cluster--1", "kythira-test-cluster-+1",
          "kythira-test-cluster-1x", "kythira-test-cluster-18446744073709551616"}) {
        BOOST_CHECK_MESSAGE(!mgr.vm_name_to_node_id(name).has_value(), "accepted " << name);
    }
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
/// PUTs that create the named resource (or, under `If-None-Match: *`, answer
/// 412 when it already exists), VMs that are running, and NICs whose IP is
/// 10.0.0.5. A single NIC or VM that was never created reads as 404.
class ArmDouble : public Azure::Core::Http::HttpTransport {
public:
    std::vector<std::string> pages{R"({"value":[]})"};
    /// Names (VM or NIC) that already exist, e.g. created by another process.
    std::set<std::string> existing;
    /// When set, a VM's instanceView reflects `existing` and `stopped`: 404
    /// for a VM that does not exist, deallocated for one in `stopped`. When
    /// clear, every instanceView reads running, as the older cases expect.
    bool strict_instance_view = false;
    std::set<std::string> stopped;

    /// The VM an instanceView URL addresses, or "".
    static auto instance_view_vm(const std::string& url) -> std::string {
        const std::string kind = "/virtualMachines/";
        const auto at = url.find(kind);
        const auto view = url.find("/instanceView");
        if (at == std::string::npos || view == std::string::npos || view < at) {
            return "";
        }
        return url.substr(at + kind.size(), view - at - kind.size());
    }

    /// The resource name a single-resource URL addresses, or "" for a list
    /// or a sub-resource such as instanceView.
    static auto resource_name(const std::string& url) -> std::string {
        for (const std::string kind : {"/virtualMachines/", "/networkInterfaces/"}) {
            auto at = url.find(kind);
            if (at == std::string::npos) {
                continue;
            }
            auto rest = url.substr(at + kind.size());
            auto end = rest.find_first_of("/?");
            if (end != std::string::npos && rest[end] == '/') {
                return "";
            }
            return rest.substr(0, end);
        }
        return "";
    }

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
        auto status = Azure::Core::Http::HttpStatusCode::Ok;
        const auto& url = call.url;
        const auto name = resource_name(url);
        std::lock_guard lock(_mu);
        if (call.method == "GET" && url.find("/page/") != std::string::npos) {
            reply = pages.at(std::stoul(url.substr(url.find("/page/") + 6)));
        } else if (call.method == "GET" && url.find("/virtualMachines?") != std::string::npos) {
            reply = pages.at(0);
        } else if (call.method == "GET" && url.find("/instanceView") != std::string::npos) {
            const auto vm = instance_view_vm(url);
            if (strict_instance_view && !existing.contains(vm)) {
                status = Azure::Core::Http::HttpStatusCode::NotFound;
                reply = R"({"error":{"code":"ResourceNotFound","message":"not found"}})";
            } else if (strict_instance_view && stopped.contains(vm)) {
                reply = R"({"statuses":[{"code":"PowerState/deallocated"}]})";
            } else {
                reply = R"({"statuses":[{"code":"PowerState/running"}]})";
            }
        } else if (call.method == "GET" && !name.empty() && !existing.contains(name)) {
            status = Azure::Core::Http::HttpStatusCode::NotFound;
            reply = R"({"error":{"code":"ResourceNotFound","message":"not found"}})";
        } else if (call.method == "GET" && url.find("/networkInterfaces/") != std::string::npos) {
            reply = R"({"properties":{"ipConfigurations":[)"
                    R"({"properties":{"privateIPAddress":"10.0.0.5"}}]}})";
        } else if (call.method == "PUT" && !name.empty()) {
            if (existing.contains(name)) {
                if (const auto inm = request.GetHeader("If-None-Match");
                    inm.HasValue() && inm.Value() == "*") {
                    status = Azure::Core::Http::HttpStatusCode::PreconditionFailed;
                    reply = R"({"error":{"code":"PreconditionFailed","message":"exists"}})";
                }
            } else {
                existing.insert(name);
                status = Azure::Core::Http::HttpStatusCode::Created;
            }
        } else if (call.method == "DELETE" && !name.empty()) {
            existing.erase(name);
        }
        auto response = std::make_unique<Azure::Core::Http::RawResponse>(1, 1, status, "OK");
        response->SetHeader("Content-Type", "application/json");
        // A body *stream*, as a real transport hands back: the pipeline's
        // transport policy reads the stream into the body itself, and has
        // nothing to read if only SetBody was called. The stream does not own
        // its bytes, so they live in `_bodies` (a deque: growing it never
        // moves an existing element) for as long as the double does.
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

    /// Every VM PUT the manager sent, as (VM name, body), in order.
    [[nodiscard]] auto vm_puts() const -> std::vector<std::pair<std::string, boost::json::value>> {
        std::vector<std::pair<std::string, boost::json::value>> out;
        for (const auto& c : calls()) {
            if (c.method == "PUT" && c.url.find("/virtualMachines/") != std::string::npos) {
                out.emplace_back(resource_name(c.url), boost::json::parse(c.body));
            }
        }
        return out;
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
    arm->existing = {"kythira-test-cluster-4", "kythira-test-cluster-4-nic"};
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

// Requirement 7.1-7.3: a tag this manager could not have written is skipped.
// Read through std::stoull, "-1" was the largest uint64 and every provision
// after it failed with overflow_error.
BOOST_AUTO_TEST_CASE(unparseable_node_id_tags_do_not_steer_allocation) {
    auto arm = std::make_shared<ArmDouble>();
    auto stray = [](const std::string& name, const std::string& tag) {
        return R"({"name":")" + name +
               R"(","tags":{"kythira:cluster":"test-cluster","kythira:node-id":")" + tag +
               R"("},"properties":{"provisioningState":"Succeeded"}})";
    };
    arm->pages = {R"({"value":[)" + vm_entry("kythira-test-cluster-2", "test-cluster", "k") + "," +
                  stray("odd-a", "-1") + "," + stray("odd-b", "7x") + "," +
                  stray("odd-c", "18446744073709551616") + "]}"};
    vm_mgr_t mgr{keyed_vm_config(arm)};
    BOOST_CHECK_EQUAL(mgr.provision_node("1", std::nullopt).get().node_id, 3u);
}

// ── Claiming an id across processes ───────────────────────────────────────────
//
// next_node_id() is a candidate: another leader's manager may have listed the
// same VMs and created the same id first. Its VM and NIC are not in this
// manager's listing yet, but their names are taken, and provision_node must
// move on without updating or deleting either.

namespace {

auto puts_to(const ArmDouble& arm, const std::string& name) -> std::size_t {
    std::size_t n = 0;
    for (const auto& c : arm.calls()) {
        if (c.method == "PUT" && ArmDouble::resource_name(c.url) == name) {
            ++n;
        }
    }
    return n;
}

auto deletes_of(const ArmDouble& arm, const std::string& name) -> std::size_t {
    std::size_t n = 0;
    for (const auto& c : arm.calls()) {
        if (c.method == "DELETE" && ArmDouble::resource_name(c.url) == name) {
            ++n;
        }
    }
    return n;
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_vm_name_another_process_holds_is_skipped_untouched) {
    auto arm = std::make_shared<ArmDouble>();
    arm->existing = {"kythira-test-cluster-1", "kythira-test-cluster-1-nic"};
    vm_mgr_t mgr{keyed_vm_config(arm)};

    auto peer = mgr.provision_node("1", std::nullopt).get();
    BOOST_CHECK_EQUAL(peer.node_id, 2u);
    BOOST_CHECK_EQUAL(puts_to(*arm, "kythira-test-cluster-1"), 0u);
    BOOST_CHECK_EQUAL(puts_to(*arm, "kythira-test-cluster-1-nic"), 0u);
    BOOST_CHECK_EQUAL(deletes_of(*arm, "kythira-test-cluster-1"), 0u);
    BOOST_CHECK_EQUAL(deletes_of(*arm, "kythira-test-cluster-1-nic"), 0u);
}

// Every create is conditional, so a VM that appears between the existence
// check and the PUT (the other leader's create landing in that gap) is
// refused by ARM rather than updated in place.
BOOST_AUTO_TEST_CASE(every_create_put_is_create_only) {
    auto arm = std::make_shared<ArmDouble>();
    vm_mgr_t mgr{keyed_vm_config(arm)};
    static_cast<void>(mgr.provision_node("1", std::nullopt).get());
    BOOST_CHECK_EQUAL(puts_to(*arm, "kythira-test-cluster-1-nic"), 1u);
    BOOST_CHECK_EQUAL(puts_to(*arm, "kythira-test-cluster-1"), 1u);
}

// A VM PUT refused because the name was taken after this call created its NIC:
// the NIC is this call's own and is cleaned up; the VM is not touched.
class LateVmArm : public ArmDouble {
public:
    auto Send(Azure::Core::Http::Request& request, const Azure::Core::Context& context)
        -> std::unique_ptr<Azure::Core::Http::RawResponse> override {
        if (request.GetMethod() == Azure::Core::Http::HttpMethod::Put &&
            resource_name(request.GetUrl().GetAbsoluteUrl()) == "kythira-test-cluster-1-nic") {
            // The other process's VM lands just after this call's checks.
            existing.insert("kythira-test-cluster-1");
        }
        return ArmDouble::Send(request, context);
    }
};

BOOST_AUTO_TEST_CASE(a_vm_name_taken_after_the_nic_was_created_cleans_up_only_the_nic) {
    auto arm = std::make_shared<LateVmArm>();
    vm_mgr_t mgr{keyed_vm_config(arm)};

    auto peer = mgr.provision_node("1", std::nullopt).get();
    BOOST_CHECK_EQUAL(peer.node_id, 2u);
    BOOST_CHECK_EQUAL(deletes_of(*arm, "kythira-test-cluster-1-nic"), 1u);
    BOOST_CHECK_EQUAL(deletes_of(*arm, "kythira-test-cluster-1"), 0u);
}

BOOST_AUTO_TEST_CASE(provision_gives_up_after_repeated_collisions) {
    auto arm = std::make_shared<ArmDouble>();
    for (int i = 1; i <= 10; ++i) {
        arm->existing.insert("kythira-test-cluster-" + std::to_string(i));
    }
    vm_mgr_t mgr{keyed_vm_config(arm)};
    BOOST_CHECK_THROW(mgr.provision_node("1", std::nullopt).get(), std::runtime_error);
    for (const auto& c : arm->calls()) {
        BOOST_CHECK(c.method != "PUT" && c.method != "DELETE");
    }
}

// An id listed only on a later page of the VM list still counts, as does a VM
// that carries this cluster's name but lost its tags.
BOOST_AUTO_TEST_CASE(next_node_id_reads_every_page_and_vm_names) {
    auto arm = std::make_shared<ArmDouble>();
    arm->pages = {R"({"value":[)" + vm_entry("kythira-test-cluster-2", "test-cluster", "k") +
                      R"(],"nextLink":"https://arm.test/page/1"})",
                  R"({"value":[)" + vm_entry("kythira-test-cluster-8", "test-cluster", "k") +
                      R"(,{"name":"kythira-test-cluster-11","tags":{}}],)"
                      R"("nextLink":"https://arm.test/page/2"})",
                  R"({"value":[{"name":"kythira-other-cluster-40","tags":{}}]})"};
    vm_mgr_t mgr{keyed_vm_config(arm)};
    BOOST_CHECK_EQUAL(mgr.provision_node("1", std::nullopt).get().node_id, 12u);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Composite and string mode (cloud-composite-node-ids Requirement 5) ────────
//
// The node id is the VM: {resource group, VM name}. The manager generates the
// name before the create, so nothing is listed or allocated, and {NODE_ID} in
// the custom data and the kythira:node-id tag carry canonical text.

namespace {

static_assert(kythira::quorum_manager<kythira::azure_vm_quorum_manager<kythira::azure_vm_node_id>,
                                      kythira::azure_vm_node_id, std::string, std::string>);
static_assert(kythira::quorum_manager<kythira::azure_vm_quorum_manager<std::string>, std::string,
                                      std::string, std::string>);
static_assert(kythira::azure_vm_quorum_manager<kythira::azure_vm_node_id>::name_is_node_id);
static_assert(kythira::azure_vm_quorum_manager<std::string>::name_is_node_id);
static_assert(!kythira::azure_vm_quorum_manager<std::uint64_t>::name_is_node_id);

/// `kythira-<cluster part>-<8 lower-case base32>`.
auto is_generated_vm_name(const std::string& name, const std::string& cluster_part) -> bool {
    const std::string prefix = "kythira-" + cluster_part + "-";
    return name.size() == prefix.size() + kythira::generated_name_suffix_length &&
           name.starts_with(prefix) && std::ranges::all_of(name.substr(prefix.size()), [](char c) {
               return kythira::generated_name_alphabet.find(c) != std::string_view::npos;
           });
}

template<typename NodeId> auto vm_id(const std::string& rg, const std::string& name) -> NodeId {
    kythira::azure_vm_node_id id{rg, name};
    if constexpr (std::same_as<NodeId, std::string>) {
        return id.to_string();
    } else {
        return id;
    }
}

auto decoded_custom_data(const boost::json::value& vm_body) -> std::string {
    const auto bytes = Azure::Core::Convert::Base64Decode(
        std::string(vm_body.at("properties").at("osProfile").at("customData").as_string()));
    return {bytes.begin(), bytes.end()};
}

auto vm_list_reads(const ArmDouble& arm) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(arm.calls(), [](const arm_call& c) {
        return c.method == "GET" && c.url.find("/virtualMachines?") != std::string::npos;
    }));
}

template<typename NodeId> auto vm_placements(const std::vector<NodeId>& ids) {
    std::vector<kythira::node_placement<NodeId, std::string>> out;
    for (const auto& id : ids) {
        out.push_back({.node_id = id, .group_id = "1"});
    }
    return out;
}

/// Provision three VMs, check what each create carried, assess them, stop
/// one, decommission one: the same walk for composite and string mode.
template<typename NodeId> void run_vm_lifecycle() {
    auto arm = std::make_shared<ArmDouble>();
    arm->strict_instance_view = true;
    auto cfg = keyed_vm_config(arm);
    cfg.custom_data_template = "node={NODE_ID} cluster={CLUSTER}";
    kythira::azure_vm_quorum_manager<NodeId> mgr{cfg};

    std::vector<NodeId> ids;
    for (std::size_t i = 0; i < 3; ++i) {
        auto peer = mgr.provision_node("1", std::nullopt).get();
        const auto puts = arm->vm_puts();
        BOOST_REQUIRE_EQUAL(puts.size(), i + 1);
        const auto& [name, body] = puts.back();
        BOOST_CHECK_MESSAGE(is_generated_vm_name(name, "test-cluster"), "name " << name);
        BOOST_CHECK(peer.node_id == vm_id<NodeId>("test-rg", name));
        BOOST_CHECK_EQUAL(peer.address, "10.0.0.5:7000");
        BOOST_CHECK_EQUAL(mgr.vm_name_of(peer.node_id).value_or(""), name);
        BOOST_CHECK(mgr.vm_name_to_node_id(name) == std::optional<NodeId>{peer.node_id});

        // The id is known before the create, so it rides in the create.
        const auto text = "azure-vm:test-rg:" + name;
        BOOST_CHECK_EQUAL(body.at("tags").at("kythira:node-id").as_string(), text);
        BOOST_CHECK_EQUAL(body.at("properties").at("osProfile").at("computerName").as_string(),
                          name);
        BOOST_CHECK_EQUAL(decoded_custom_data(body), "node=" + text + " cluster=test-cluster");
        BOOST_CHECK(arm->existing.contains(name + "-nic"));
        ids.push_back(peer.node_id);
    }
    // Nothing was allocated: no VM list was read, and the names differ.
    BOOST_CHECK_EQUAL(vm_list_reads(*arm), 0u);
    BOOST_CHECK(ids[0] != ids[1] && ids[1] != ids[2] && ids[0] != ids[2]);

    auto health = mgr.assess_quorum(vm_placements(ids)).get();
    BOOST_CHECK(health.status == kythira::quorum_status::healthy);
    BOOST_CHECK_EQUAL(health.live_node_count, 3u);

    arm->stopped.insert(*mgr.vm_name_of(ids[0]));
    health = mgr.assess_quorum(vm_placements(ids)).get();
    BOOST_CHECK(health.status == kythira::quorum_status::critical);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1u);
    BOOST_CHECK(health.unreachable_nodes[0] == ids[0]);

    // Decommission deletes that VM and its NIC, and nothing else.
    const auto victim = *mgr.vm_name_of(ids[1]);
    mgr.decommission_node(ids[1]).get();
    BOOST_CHECK_EQUAL(vm_node_id_floor::deletes_of(*arm, victim), 1u);
    BOOST_CHECK_EQUAL(vm_node_id_floor::deletes_of(*arm, victim + "-nic"), 1u);
    BOOST_CHECK_EQUAL(vm_node_id_floor::deletes_of(*arm, *mgr.vm_name_of(ids[2])), 0u);
    health = mgr.assess_quorum(vm_placements(ids)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);
    BOOST_CHECK(health.status == kythira::quorum_status::lost);
}

/// Refuses the first `refuse` VM names the manager checks, as if another
/// process held them: the existence GET finds the VM.
class TakenNameArm : public ArmDouble {
public:
    std::size_t refuse = 1;
    std::vector<std::string> refused;

    auto Send(Azure::Core::Http::Request& request, const Azure::Core::Context& context)
        -> std::unique_ptr<Azure::Core::Http::RawResponse> override {
        const auto url = request.GetUrl().GetAbsoluteUrl();
        const auto name = resource_name(url);
        if (refuse > 0 && request.GetMethod() == Azure::Core::Http::HttpMethod::Get &&
            url.find("/virtualMachines/") != std::string::npos && !name.empty()) {
            --refuse;
            refused.push_back(name);
            existing.insert(name);
        }
        return ArmDouble::Send(request, context);
    }
};

}  // namespace

// Composite mode is the default (task 4.4); numeric mode must be asked for.
static_assert(
    std::same_as<kythira::azure_vm_quorum_manager<>::node_id_type, kythira::azure_vm_node_id>);

BOOST_AUTO_TEST_SUITE(vm_composite_node_id)

BOOST_AUTO_TEST_CASE(composite_mode_lifecycle) {
    run_vm_lifecycle<kythira::azure_vm_node_id>();
}

BOOST_AUTO_TEST_CASE(string_mode_lifecycle) {
    run_vm_lifecycle<std::string>();
}

// Azure compares VM and resource group names case-insensitively, and the id
// folds both, so the generated name is lower case and is exactly the id's
// native part; a long cluster name is cut to keep the name within 64.
BOOST_AUTO_TEST_CASE(composite_mode_name_is_lower_case_and_within_the_limit) {
    auto arm = std::make_shared<ArmDouble>();
    auto cfg = keyed_vm_config(arm);
    cfg.azure.resource_group = "Test-RG";
    cfg.cluster_name = "Prod-" + std::string(60, 'C');
    kythira::azure_vm_quorum_manager<kythira::azure_vm_node_id> mgr{cfg};

    auto peer = mgr.provision_node("1", std::nullopt).get();
    const auto name = arm->vm_puts().back().first;
    BOOST_CHECK_EQUAL(name.size(), 64u);
    // 64 - "kythira-" - "-" - 8 leaves 47 characters of the cluster name.
    BOOST_CHECK_MESSAGE(is_generated_vm_name(name, "prod-" + std::string(42, 'c')),
                        "name " << name);
    BOOST_CHECK_EQUAL(peer.node_id.scope(), "test-rg");
    BOOST_CHECK_EQUAL(peer.node_id.native(), name);
    // The tag keeps the cluster name as configured.
    BOOST_CHECK_EQUAL(arm->vm_put().at("tags").at("kythira:cluster").as_string(), cfg.cluster_name);
}

// A resource group or cluster name that cannot form an id is refused up
// front in the textual modes; numeric mode does not use them as one.
BOOST_AUTO_TEST_CASE(textual_modes_require_a_valid_scope_and_name) {
    auto cfg = make_vm_config();
    cfg.cluster_name = "bad/name";
    BOOST_CHECK_THROW(kythira::azure_vm_quorum_manager<kythira::azure_vm_node_id>{cfg},
                      std::invalid_argument);
    BOOST_CHECK_THROW(kythira::azure_vm_quorum_manager<std::string>{cfg}, std::invalid_argument);
    BOOST_CHECK_NO_THROW(kythira::azure_vm_quorum_manager<std::uint64_t>{cfg});

    cfg = make_vm_config();
    cfg.azure.resource_group = "rg with spaces";
    BOOST_CHECK_THROW(kythira::azure_vm_quorum_manager<kythira::azure_vm_node_id>{cfg},
                      std::invalid_argument);
}

// A name some other VM already holds is retried under a fresh name, and the
// holder is neither updated nor deleted.
BOOST_AUTO_TEST_CASE(composite_mode_retries_a_name_collision) {
    auto arm = std::make_shared<TakenNameArm>();
    kythira::azure_vm_quorum_manager<kythira::azure_vm_node_id> mgr{keyed_vm_config(arm)};

    auto peer = mgr.provision_node("1", std::nullopt).get();
    BOOST_REQUIRE_EQUAL(arm->refused.size(), 1u);
    const auto& taken = arm->refused.front();
    BOOST_CHECK_NE(peer.node_id.native(), taken);
    BOOST_CHECK_EQUAL(vm_node_id_floor::puts_to(*arm, taken), 0u);
    BOOST_CHECK_EQUAL(vm_node_id_floor::deletes_of(*arm, taken), 0u);

    // Every attempt colliding gives up, still touching nothing.
    arm->refuse = 100;
    const auto puts_before = arm->vm_puts().size();
    BOOST_CHECK_THROW(mgr.provision_node("1", std::nullopt).get(), std::runtime_error);
    BOOST_CHECK_EQUAL(arm->refused.size(), 1u + decltype(mgr)::max_id_claim_attempts);
    BOOST_CHECK_EQUAL(arm->vm_puts().size(), puts_before);
}

// An id in another resource group is unreachable without an ARM call and
// does not fail the assessment; decommission refuses it.
BOOST_AUTO_TEST_CASE(composite_mode_reports_foreign_ids_unreachable) {
    auto arm = std::make_shared<ArmDouble>();
    arm->strict_instance_view = true;
    arm->existing = {"kythira-test-cluster-aaaaaaaa", "kythira-test-cluster-aaaaaaaa-nic"};
    kythira::azure_vm_quorum_manager<kythira::azure_vm_node_id> mgr{keyed_vm_config(arm)};

    std::vector<kythira::azure_vm_node_id> ids{{"test-rg", "kythira-test-cluster-aaaaaaaa"},
                                               {"other-rg", "kythira-test-cluster-aaaaaaaa"}};
    auto health = mgr.assess_quorum(vm_placements(ids)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1u);
    BOOST_CHECK(health.unreachable_nodes[0] == ids[1]);
    BOOST_CHECK_EQUAL(arm->calls().size(), 1u);  // one instanceView, for ids[0]

    BOOST_CHECK_THROW(mgr.decommission_node(ids[1]).get(), std::runtime_error);
    BOOST_CHECK_EQUAL(vm_node_id_floor::deletes_of(*arm, "kythira-test-cluster-aaaaaaaa"), 0u);
}

BOOST_AUTO_TEST_CASE(string_mode_rejects_non_canonical_text) {
    auto arm = std::make_shared<ArmDouble>();
    arm->strict_instance_view = true;
    arm->existing = {"kythira-test-cluster-aaaaaaaa", "kythira-test-cluster-1"};
    kythira::azure_vm_quorum_manager<std::string> mgr{keyed_vm_config(arm)};

    std::vector<std::string> ids{"azure-vm:test-rg:kythira-test-cluster-aaaaaaaa",
                                 "azure-vm:TEST-RG:kythira-test-cluster-aaaaaaaa",
                                 "kythira-test-cluster-aaaaaaaa", "1"};
    auto health = mgr.assess_quorum(vm_placements(ids)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);
    BOOST_CHECK_EQUAL(health.unreachable_nodes.size(), 3u);
    BOOST_CHECK_THROW(mgr.decommission_node("1").get(), std::runtime_error);
    BOOST_CHECK_EQUAL(vm_node_id_floor::deletes_of(*arm, "kythira-test-cluster-1"), 0u);
}

// The keyed lookup takes the id from the VM's own name.
BOOST_AUTO_TEST_CASE(composite_mode_find_by_idempotency_key_reads_the_name) {
    auto arm = std::make_shared<ArmDouble>();
    arm->pages = {R"({"value":[)" +
                  vm_entry("kythira-test-cluster-abcdefgh", "test-cluster", "cap-1-2-3") + "]}"};
    arm->existing = {"kythira-test-cluster-abcdefgh-nic"};
    kythira::azure_vm_quorum_manager<kythira::azure_vm_node_id> mgr{keyed_vm_config(arm)};

    auto found = mgr.find_by_idempotency_key("cap-1-2-3").get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK(found->node_id ==
                kythira::azure_vm_node_id("test-rg", "kythira-test-cluster-abcdefgh"));
    BOOST_CHECK_EQUAL(found->address, "10.0.0.5:7000");
}

BOOST_AUTO_TEST_SUITE_END()

// ── Fault injection ────────────────────────────────────────────────────────────

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(fault_injection)

BOOST_AUTO_TEST_CASE(vm_find_by_idempotency_key_fault) {
    kythira::azure_vm_quorum_manager<std::uint64_t> mgr{make_vm_config()};
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
    kythira::azure_vm_quorum_manager<std::uint64_t> mgr{make_vm_config()};
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{1, "1"}};
    BOOST_CHECK_THROW(std::move(mgr.assess_quorum(cluster)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vm_provision_node_fault) {
    FaultPointFixture fp("raft/azure/vm/create_vm");
    kythira::azure_vm_quorum_manager<std::uint64_t> mgr{make_vm_config()};
    BOOST_CHECK_THROW(std::move(mgr.provision_node("1", std::nullopt)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vm_decommission_node_fault) {
    FaultPointFixture fp("raft/azure/vm/delete_vm");
    kythira::azure_vm_quorum_manager<std::uint64_t> mgr{make_vm_config()};
    BOOST_CHECK_THROW(std::move(mgr.decommission_node(1)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vm_maintain_quorum_fault) {
    FaultPointFixture fp("raft/azure/vm/maintain_quorum");
    kythira::azure_vm_quorum_manager<std::uint64_t> mgr{make_vm_config()};
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{1, "1"}};
    BOOST_CHECK_THROW(std::move(mgr.maintain_quorum(cluster)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vmss_assess_quorum_fault) {
    VmssSkipUpgradePolicyFixture skip;
    FaultPointFixture fp("raft/azure/vmss/list_instances");
    kythira::azure_vmss_quorum_manager<std::uint64_t> mgr{make_vmss_config()};
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{1, "1"}};
    BOOST_CHECK_THROW(std::move(mgr.assess_quorum(cluster)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vmss_provision_node_fault) {
    VmssSkipUpgradePolicyFixture skip;
    FaultPointFixture fp("raft/azure/vmss/update_capacity");
    kythira::azure_vmss_quorum_manager<std::uint64_t> mgr{make_vmss_config()};
    BOOST_CHECK_THROW(std::move(mgr.provision_node("1", std::nullopt)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vmss_decommission_node_fault) {
    VmssSkipUpgradePolicyFixture skip;
    FaultPointFixture fp("raft/azure/vmss/delete_instance");
    kythira::azure_vmss_quorum_manager<std::uint64_t> mgr{make_vmss_config()};
    BOOST_CHECK_THROW(std::move(mgr.decommission_node(1)).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(vmss_maintain_quorum_fault) {
    VmssSkipUpgradePolicyFixture skip;
    FaultPointFixture fp("raft/azure/vmss/maintain_quorum");
    kythira::azure_vmss_quorum_manager<std::uint64_t> mgr{make_vmss_config()};
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
