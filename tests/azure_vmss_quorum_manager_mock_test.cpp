// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file azure_vmss_quorum_manager_mock_test.cpp
/// @brief `azure_vmss_quorum_manager`'s timeout rollback and scale-in
///        protection against an in-process ARM double
///        (`.kiro/specs/group-scale-up-rollback/`, task 7).
///
/// The double is an `HttpTransport`, so the manager's real pipeline, request
/// building and response parsing all run; only the socket is replaced. It
/// models a Flexible scale set whose scale-in policy is `OldestVM`, a legal
/// operator choice: a capacity decrease deletes the oldest member that is not
/// protected from scale-in. That is what makes a blind shrink cost a voter
/// here, and what the targeted rollback has to avoid.

#define BOOST_TEST_MODULE azure_vmss_quorum_manager_mock_test
#include <boost/test/unit_test.hpp>

#include <raft/azure_vmss_quorum_manager.hpp>

#include <azure/core/credentials/credentials.hpp>
#include <azure/core/http/raw_response.hpp>
#include <azure/core/http/transport.hpp>
#include <azure/core/io/body_stream.hpp>

#include <boost/json.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
BOOST_TEST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

const std::string subscription = "00000000-0000-0000-0000-000000000000";
const std::string resource_group = "test-rg";
const std::string scale_set = "test-vmss";
const std::string cluster = "mock-cluster";
const std::string compute = "/providers/Microsoft.Compute/";

/// Hands out a token without contacting anything.
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

/// One member of the modelled scale set.
struct member {
    std::string name;
    std::string state;  // provisioningState
    std::map<std::string, std::string> tags;
    bool protected_from_scale_in = false;
};

/// A Flexible scale set and the resource group's VM list, as far as
/// `azure_vmss_quorum_manager` uses them.
class ArmScaleSet : public Azure::Core::Http::HttpTransport {
public:
    void seed(const std::string& name, std::map<std::string, std::string> tags = {},
              bool is_protected = false, const std::string& state = "Succeeded") {
        std::lock_guard lock(_mu);
        _members.push_back({name, state, std::move(tags), is_protected});
        _capacity = static_cast<std::int64_t>(_members.size());
    }
    /// The state a new member reports, or empty to create nothing.
    void set_launch_state(std::string state) {
        std::lock_guard lock(_mu);
        _launch_state = std::move(state);
    }
    /// Every `Creating` member that existed before a grow finishes at the grow.
    void settle_creating_on_grow() {
        std::lock_guard lock(_mu);
        _settle_creating = true;
    }
    /// Answer every protection `PUT` with this HTTP status and error code.
    void set_protection_error(int status, std::string code) {
        std::lock_guard lock(_mu);
        _protection_status = status;
        _protection_code = std::move(code);
    }

    [[nodiscard]] auto names() const -> std::vector<std::string> {
        std::lock_guard lock(_mu);
        std::vector<std::string> out;
        for (const auto& m : _members) {
            out.push_back(m.name);
        }
        return out;
    }
    [[nodiscard]] auto capacity() const -> std::int64_t {
        std::lock_guard lock(_mu);
        return _capacity;
    }
    [[nodiscard]] auto find(const std::string& name) const -> std::optional<member> {
        std::lock_guard lock(_mu);
        const auto it = std::ranges::find(_members, name, &member::name);
        return it == _members.end() ? std::nullopt : std::optional<member>{*it};
    }
    [[nodiscard]] auto last_launch() const -> std::string {
        std::lock_guard lock(_mu);
        return _last_launch;
    }
    [[nodiscard]] auto capacity_patches() const -> int {
        std::lock_guard lock(_mu);
        return _capacity_patches;
    }
    [[nodiscard]] auto protection_puts() const -> int {
        std::lock_guard lock(_mu);
        return _protection_puts;
    }
    [[nodiscard]] auto blind_scale_ins() const -> int {
        std::lock_guard lock(_mu);
        return _blind_scale_ins;
    }

    auto Send(Azure::Core::Http::Request& request, const Azure::Core::Context& context)
        -> std::unique_ptr<Azure::Core::Http::RawResponse> override {
        const std::string method = request.GetMethod().ToString();
        const std::string url = request.GetUrl().GetAbsoluteUrl();
        std::string body;
        if (auto* stream = request.GetBodyStream(); stream != nullptr) {
            auto bytes = stream->ReadToEnd(context);
            body.assign(bytes.begin(), bytes.end());
        }
        std::lock_guard lock(_mu);
        const auto [status, reply] = route(method, url, body);
        auto response = std::make_unique<Azure::Core::Http::RawResponse>(
            1, 1, static_cast<Azure::Core::Http::HttpStatusCode>(status), "status");
        response->SetHeader("Content-Type", "application/json");
        // The stream does not own its bytes, so they live in `_bodies` (a
        // deque: growing it never moves an element) for the double's lifetime.
        const auto& bytes = _bodies.emplace_back(reply.begin(), reply.end());
        response->SetBodyStream(std::make_unique<Azure::Core::IO::MemoryBodyStream>(bytes));
        return response;
    }

private:
    mutable std::mutex _mu;
    std::vector<member> _members;
    std::int64_t _capacity = 0;
    int _next = 100;
    std::string _last_launch;
    std::string _launch_state = "Succeeded";
    bool _settle_creating = false;
    int _protection_status = 200;
    std::string _protection_code;
    int _capacity_patches = 0;
    int _protection_puts = 0;
    int _blind_scale_ins = 0;
    std::deque<std::vector<std::uint8_t>> _bodies;

    static auto error(int status, const std::string& code, const std::string& message)
        -> std::pair<int, std::string> {
        return {status, boost::json::serialize(boost::json::object{
                            {"error", boost::json::object{{"code", code}, {"message", message}}}})};
    }

    [[nodiscard]] auto vm_json(const member& m) const -> boost::json::object {
        boost::json::object tags;
        for (const auto& [k, v] : m.tags) {
            tags[k] = v;
        }
        boost::json::object properties{
            {"provisioningState", m.state},
            {"virtualMachineScaleSet",
             boost::json::object{{"id", "/subscriptions/" + subscription + "/resourceGroups/" +
                                            resource_group + compute + "virtualMachineScaleSets/" +
                                            scale_set}}},
            {"networkProfile",
             boost::json::object{{"networkInterfaces",
                                  boost::json::array{boost::json::object{
                                      {"id", "/subscriptions/" + subscription + "/resourceGroups/" +
                                                 resource_group +
                                                 "/providers/Microsoft.Network/networkInterfaces/" +
                                                 m.name + "-nic"}}}}}}};
        if (m.protected_from_scale_in) {
            properties["protectionPolicy"] = boost::json::object{{"protectFromScaleIn", true}};
        }
        return {{"name", m.name}, {"tags", std::move(tags)}, {"properties", std::move(properties)}};
    }

    void resize(std::int64_t target) {
        if (target > _capacity && _settle_creating) {
            for (auto& m : _members) {
                if (m.state == "Creating") {
                    m.state = "Succeeded";
                }
            }
        }
        while (static_cast<std::int64_t>(_members.size()) < target && !_launch_state.empty()) {
            _last_launch = "kythira_" + std::to_string(++_next);
            _members.push_back({_last_launch, _launch_state, {}, false});
        }
        // OldestVM scale-in policy: oldest first, skipping protected members.
        while (static_cast<std::int64_t>(_members.size()) > target) {
            const auto victim = std::ranges::find_if(
                _members, [](const member& m) { return !m.protected_from_scale_in; });
            if (victim == _members.end()) {
                break;
            }
            _members.erase(victim);
            ++_blind_scale_ins;
        }
        _capacity = target;
    }

    auto route(const std::string& method, const std::string& url, const std::string& body)
        -> std::pair<int, std::string> {
        const auto path = url.substr(0, url.find('?'));
        const auto query = url.find('?') == std::string::npos ? "" : url.substr(url.find('?'));
        const auto at = path.find(compute);

        if (path.find("/networkInterfaces/") != std::string::npos) {
            return {200, R"({"properties":{"ipConfigurations":[)"
                         R"({"properties":{"privateIPAddress":"10.0.0.5"}}]}})"};
        }
        if (at == std::string::npos) {
            return error(404, "NotFound", "no route for " + url);
        }
        const auto rest = path.substr(at + compute.size());
        const std::string ss_prefix = "virtualMachineScaleSets/" + scale_set;

        if (rest == ss_prefix) {
            if (method == "GET") {
                return {200, boost::json::serialize(boost::json::object{
                                 {"sku", boost::json::object{{"capacity", _capacity}}},
                                 {"properties", boost::json::object{
                                                    {"orchestrationMode", "Flexible"},
                                                    {"upgradePolicy",
                                                     boost::json::object{{"mode", "Manual"}}}}}})};
            }
            if (method == "PATCH") {
                ++_capacity_patches;
                resize(boost::json::parse(body).at("sku").at("capacity").as_int64());
                return {200, "{}"};
            }
        }
        if (rest == ss_prefix + "/delete" && method == "POST") {
            const auto request = boost::json::parse(body);
            for (const auto& id : request.at("instanceIds").as_array()) {
                const auto it =
                    std::ranges::find(_members, std::string(id.as_string()), &member::name);
                if (it == _members.end()) {
                    return error(400, "InvalidParameter", "The VM was not found");
                }
                // ARM documents that protection does not block a user delete.
                _members.erase(it);
                --_capacity;
            }
            return {200, "{}"};
        }
        if (rest.starts_with(ss_prefix + "/virtualMachines/") && method == "PUT") {
            ++_protection_puts;
            if (query.find("api-version=2023-09-01") == std::string::npos) {
                return error(400, "InvalidApiVersion", "protection needs 2023-09-01");
            }
            if (_protection_status != 200) {
                return error(_protection_status, _protection_code, "refused by the double");
            }
            const auto name = rest.substr(rest.rfind('/') + 1);
            const auto it = std::ranges::find(_members, name, &member::name);
            if (it == _members.end()) {
                return error(404, "NotFound", name);
            }
            it->protected_from_scale_in = boost::json::parse(body)
                                              .at("properties")
                                              .at("protectionPolicy")
                                              .at("protectFromScaleIn")
                                              .as_bool();
            return {200, "{}"};
        }
        if (rest == "virtualMachines" && method == "GET") {
            boost::json::array value;
            for (const auto& m : _members) {
                value.push_back(vm_json(m));
            }
            return {200, boost::json::serialize(boost::json::object{{"value", std::move(value)}})};
        }
        if (rest.starts_with("virtualMachines/")) {
            const auto name = rest.substr(std::string("virtualMachines/").size());
            const auto it = std::ranges::find(_members, name, &member::name);
            if (it == _members.end()) {
                return error(404, "NotFound", name);
            }
            if (method == "GET") {
                auto vm = vm_json(*it);
                vm["properties"].as_object()["instanceView"] = boost::json::object{
                    {"statuses",
                     boost::json::array{boost::json::object{{"code", "PowerState/running"}}}}};
                return {200, boost::json::serialize(vm)};
            }
            if (method == "PATCH") {
                const auto patch = boost::json::parse(body);
                for (const auto& [k, v] : patch.at("tags").as_object()) {
                    it->tags[std::string(k)] = std::string(v.as_string());
                }
                return {200, "{}"};
            }
        }
        return error(404, "NotFound", "no route for " + method + " " + url);
    }
};

using manager = kythira::azure_vmss_quorum_manager<std::uint64_t, std::string>;

auto node_tags(const std::string& node, const std::string& owner = cluster)
    -> std::map<std::string, std::string> {
    return {{"kythira:cluster", owner}, {"kythira:node-id", node}};
}

/// Three tagged voters in one scale set, and a manager over the double.
struct Cloud {
    std::shared_ptr<ArmScaleSet> arm = std::make_shared<ArmScaleSet>();
    std::vector<std::string> voters{"kythira_1", "kythira_2", "kythira_3"};

    explicit Cloud(bool protect_voters = true) {
        for (std::size_t i = 0; i < voters.size(); ++i) {
            arm->seed(voters[i], node_tags(std::to_string(i + 1)), protect_voters);
        }
    }

    [[nodiscard]] auto make() const -> manager {
        kythira::azure_vmss_quorum_manager_config cfg;
        cfg.cluster_name = cluster;
        cfg.azure.subscription_id = subscription;
        cfg.azure.resource_group = resource_group;
        cfg.azure.location = "eastus";
        cfg.azure.arm_endpoint_override = "https://arm.test";
        cfg.azure.credential = std::make_shared<FakeTokenCredential>();
        cfg.azure.transport = arm;
        cfg.scale_set_by_group["1"] = scale_set;
        cfg.topology.groups.push_back({.group_id = "1", .target_count = 3});
        cfg.provision_timeout = std::chrono::seconds{1};
        cfg.decommission_timeout = std::chrono::seconds{1};
        cfg.poll_interval = std::chrono::milliseconds{1};
        return manager{cfg};
    }
};

auto provision_error(manager& mgr) -> std::string {
    try {
        std::move(mgr.provision_node("1", std::nullopt)).get();
    } catch (const std::exception& ex) {
        return ex.what();
    }
    BOOST_FAIL("provision_node was expected to time out");
    return {};
}

}  // namespace

// ── Timeout rollback (Requirements 2-3) ──────────────────────────────────────

BOOST_AUTO_TEST_SUITE(vmss_timeout_rollback)

// The new member never finishes creating. The old path lowered the capacity
// and the scale set deleted its oldest unprotected member: a voter.
BOOST_AUTO_TEST_CASE(a_creating_member_is_deleted_by_name_and_every_voter_kept) {
    // Unprotected, as a cluster adopted before protection existed would be,
    // and kept so by a reconcile that fails transiently.
    Cloud cloud(/*protect_voters=*/false);
    cloud.arm->set_protection_error(409, "OperationNotAllowed");
    cloud.arm->set_launch_state("Creating");
    auto mgr = cloud.make();

    const auto error = provision_error(mgr);
    BOOST_TEST(error.find("rollback: removed " + cloud.arm->last_launch() + " (fresh, Creating)") !=
               std::string::npos);
    BOOST_TEST(cloud.arm->names() == cloud.voters);
    BOOST_TEST(cloud.arm->capacity() == 3);
    BOOST_TEST(cloud.arm->blind_scale_ins() == 0);
    BOOST_TEST(cloud.arm->capacity_patches() == 1);  // The grow; no blind restore.
}

// An untagged member that predates the grow and finishes creating during the
// wait is not this provision's: the old untagged-only test adopted it.
BOOST_AUTO_TEST_CASE(a_pre_existing_untagged_member_is_neither_adopted_nor_removed) {
    Cloud cloud;
    cloud.arm->seed("kythira_early", {}, false, "Creating");
    cloud.arm->set_launch_state("Creating");
    cloud.arm->settle_creating_on_grow();
    auto mgr = cloud.make();

    const auto error = provision_error(mgr);
    BOOST_TEST(error.find("rollback: removed " + cloud.arm->last_launch()) != std::string::npos);
    const auto early = cloud.arm->find("kythira_early");
    BOOST_REQUIRE(early.has_value());
    BOOST_TEST(early->tags.empty());
}

BOOST_AUTO_TEST_CASE(nothing_created_restores_the_capacity_by_patch) {
    Cloud cloud;
    cloud.arm->set_launch_state("");
    auto mgr = cloud.make();

    const auto error = provision_error(mgr);
    BOOST_TEST(error.find("rollback: desired size restored to 3") != std::string::npos);
    BOOST_TEST(error.find("lost member") == std::string::npos);
    BOOST_TEST(cloud.arm->capacity_patches() == 2);
    BOOST_TEST(cloud.arm->capacity() == 3);
    BOOST_TEST(cloud.arm->names() == cloud.voters);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Scale-in protection (Requirement 4) ──────────────────────────────────────

BOOST_AUTO_TEST_SUITE(vmss_scale_in_protection)

BOOST_AUTO_TEST_CASE(an_adopted_member_is_tagged_and_protected) {
    Cloud cloud;
    auto mgr = cloud.make();
    const auto peer = std::move(mgr.provision_node("1", std::nullopt)).get();
    BOOST_TEST(peer.node_id == 4U);
    const auto adopted = cloud.arm->find(cloud.arm->last_launch());
    BOOST_REQUIRE(adopted.has_value());
    BOOST_TEST(adopted->protected_from_scale_in);
    BOOST_TEST(adopted->tags.at("kythira:node-id") == "4");
}

// A node this manager has assessed keeps its id after its instance leaves
// the scale set (evicted, or deleted by hand): the tag scan alone would see
// only 1-3 and hand out 4, then 5, ... up to a vanished 7's id; the floor
// starts the next provision above it instead.
BOOST_AUTO_TEST_CASE(an_assessed_id_no_longer_listed_is_not_reassigned) {
    Cloud cloud;
    auto mgr = cloud.make();
    std::vector<kythira::node_placement<std::uint64_t, std::string>> members;
    for (std::uint64_t id : {1U, 2U, 3U, 7U}) {
        members.push_back({.node_id = id, .group_id = "1"});
    }
    const auto health = std::move(mgr.assess_quorum(members)).get();
    BOOST_TEST(health.live_node_count == 3U);
    const auto peer = std::move(mgr.provision_node("1", std::nullopt)).get();
    BOOST_TEST(peer.node_id == 8U);
    BOOST_TEST(cloud.arm->find(cloud.arm->last_launch())->tags.at("kythira:node-id") == "8");
}

BOOST_AUTO_TEST_CASE(a_protection_failure_does_not_fail_the_provision) {
    Cloud cloud;
    auto mgr = cloud.make();
    cloud.arm->set_protection_error(409, "OperationNotAllowed");
    BOOST_CHECK_NO_THROW(std::move(mgr.provision_node("1", std::nullopt)).get());
    BOOST_TEST(!cloud.arm->find(cloud.arm->last_launch())->protected_from_scale_in);
}

// Only this cluster's adopted members: an untagged member and another
// cluster's node are left alone.
BOOST_AUTO_TEST_CASE(construction_protects_this_clusters_unprotected_members) {
    Cloud cloud;
    cloud.arm->seed("kythira_stray");
    cloud.arm->seed("kythira_foreign", node_tags("9", "other"));
    cloud.arm->seed("kythira_4", node_tags("4"));

    auto mgr = cloud.make();
    BOOST_TEST(cloud.arm->protection_puts() == 1);
    BOOST_TEST(cloud.arm->find("kythira_4")->protected_from_scale_in);
    BOOST_TEST(!cloud.arm->find("kythira_stray")->protected_from_scale_in);
    BOOST_TEST(!cloud.arm->find("kythira_foreign")->protected_from_scale_in);
}

BOOST_AUTO_TEST_CASE(construction_without_the_protection_permission_throws) {
    Cloud cloud;
    cloud.arm->seed("kythira_4", node_tags("4"));
    cloud.arm->set_protection_error(403, "AuthorizationFailed");
    BOOST_CHECK_THROW((void)cloud.make(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(a_protected_node_is_decommissioned_without_clearing_it) {
    Cloud cloud;
    auto mgr = cloud.make();
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(std::uint64_t{1})).get());
    BOOST_TEST(!cloud.arm->find("kythira_1").has_value());
    BOOST_TEST(cloud.arm->capacity() == 2);
    BOOST_TEST(cloud.arm->protection_puts() == 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_CASE(provisioning_states_map_onto_the_planners_classes) {
    using kythira::azure_vmss_detail::rollback_state;
    using kythira::group_rollback::member_state;
    BOOST_TEST((rollback_state("Succeeded") == member_state::live));
    BOOST_TEST((rollback_state("Creating") == member_state::pending));
    BOOST_TEST((rollback_state("Updating") == member_state::pending));
    BOOST_TEST((rollback_state("Failed") == member_state::pending));
    BOOST_TEST((rollback_state("Deleting") == member_state::terminal));
}
