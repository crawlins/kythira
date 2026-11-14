// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The service module interface, group spaces and module registry
// (.kiro/specs/service-colocation/ task 1), with two fake modules on a fake
// host: a "store" that publishes an in-process interface, and a "front"
// that requires nothing, looks the interface up, and falls back to the
// network when it is not co-located.

#define BOOST_TEST_MODULE service_module_test
#include <boost/test/unit_test.hpp>

#include <raft/module_registry.hpp>
#include <raft/service_module.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace node = kythira::node;

namespace {

/// What the node's multi-raft host would be: here a record of the groups
/// modules bootstrap and the keys they write.
struct fake_host {
    std::vector<std::uint64_t> groups;
    std::map<std::string, std::string> keys;
};

/// The interface "store" publishes.
class store_interface {
public:
    store_interface() = default;
    store_interface(const store_interface&) = delete;
    auto operator=(const store_interface&) -> store_interface& = delete;
    store_interface(store_interface&&) = delete;
    auto operator=(store_interface&&) -> store_interface& = delete;
    virtual ~store_interface() = default;
    /// Refuses a caller with no principal, as the network API would.
    [[nodiscard]] virtual auto get(const node::caller& who, const std::string& key)
        -> std::optional<std::string> = 0;
};

/// Shared by the fakes so each test sees what happened in which order.
struct journal {
    std::vector<std::string> events;
};

class store_module final : public node::service_module<fake_host> {
public:
    explicit store_module(journal& j, std::uint8_t number = 6) : _j(j), _number(number) {}

    [[nodiscard]] auto name() const -> std::string_view override { return "store"; }
    [[nodiscard]] auto number() const -> std::uint8_t override { return _number; }
    [[nodiscard]] auto env_prefix() const -> std::string_view override { return "KYTHIRA_STORE_"; }
    auto configure(const node::env_lookup& env) -> std::optional<std::string> override {
        _greeting = env("KYTHIRA_STORE_GREETING").value_or("hello");
        if (_greeting == "bad") {
            return std::string("KYTHIRA_STORE_GREETING: bad is not a greeting");
        }
        return std::nullopt;
    }
    void register_groups(fake_host& host, const node::group_space& space) override {
        _host = &host;
        _space = space;
        host.groups.push_back(space.to_host(1));
        host.keys[space.to_host_key("greeting")] = _greeting;
        _j.events.emplace_back("store register");
    }
    void publish(node::module_registry& registry) override {
        registry.publish<store_interface>("store.client", std::make_shared<client>(*this));
        _j.events.emplace_back("store publish");
    }
    void start(node::module_registry& /*registry*/) override {
        _j.events.emplace_back("store start");
    }
    void drain() override { _j.events.emplace_back("store drain"); }
    void stop() override { _j.events.emplace_back("store stop"); }
    [[nodiscard]] auto health() const -> node::module_health override { return {true, {}}; }
    [[nodiscard]] auto metrics() const -> std::vector<node::module_metric> override {
        return {{"keys", static_cast<double>(_host == nullptr ? 0 : _host->keys.size()), {}}};
    }

private:
    class client final : public store_interface {
    public:
        explicit client(store_module& m) : _m(m) {}
        auto get(const node::caller& who, const std::string& key)
            -> std::optional<std::string> override {
            if (who.principal.empty()) {
                return std::nullopt;
            }
            const auto it = _m._host->keys.find(_m._space->to_host_key(key));
            if (it == _m._host->keys.end()) {
                return std::nullopt;
            }
            return it->second;
        }

    private:
        store_module& _m;
    };

    journal& _j;
    std::uint8_t _number;
    std::string _greeting;
    fake_host* _host = nullptr;
    std::optional<node::group_space> _space;
};

class front_module final : public node::service_module<fake_host> {
public:
    explicit front_module(journal& j, std::vector<std::string> requires_modules = {})
        : _j(j), _requires(std::move(requires_modules)) {}

    [[nodiscard]] auto name() const -> std::string_view override { return "front"; }
    [[nodiscard]] auto number() const -> std::uint8_t override { return 2; }
    [[nodiscard]] auto env_prefix() const -> std::string_view override { return "KYTHIRA_FRONT_"; }
    [[nodiscard]] auto requires_modules() const -> std::vector<std::string> override {
        return _requires;
    }
    [[nodiscard]] auto starts_after() const -> std::vector<std::string> override {
        return {"store"};
    }
    auto configure(const node::env_lookup& /*env*/) -> std::optional<std::string> override {
        return std::nullopt;
    }
    void register_groups(fake_host& host, const node::group_space& space) override {
        host.groups.push_back(space.to_host(1));
        host.groups.push_back(space.to_host(std::uint64_t{1} << 48));
        _j.events.emplace_back("front register");
    }
    void start(node::module_registry& registry) override {
        _store = registry.lookup<store_interface>("store.client");
        _j.events.emplace_back(_store ? "front start in-process" : "front start network");
    }
    void drain() override { _j.events.emplace_back("front drain"); }
    void stop() override { _j.events.emplace_back("front stop"); }
    [[nodiscard]] auto health() const -> node::module_health override { return {true, {}}; }

    /// What a request through this module reads from the store.
    auto read(const std::string& principal, const std::string& key) -> std::string {
        if (!_store) {
            return "network";
        }
        return _store->get({"front", principal, "123456789012", "req-1"}, key).value_or("denied");
    }

private:
    journal& _j;
    std::vector<std::string> _requires;
    std::shared_ptr<store_interface> _store;
};

auto env_of(std::map<std::string, std::string> vars) -> node::env_lookup {
    return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(std::string(name));
        if (it == vars.end() || it->second.empty()) {
            return std::nullopt;
        }
        return it->second;
    };
}

/// A catalog with the two fakes, registered as a module's library would.
struct two_modules {
    journal j;
    node::module_catalog<fake_host> catalog;
    node::module_registrar<fake_host> store{
        "store", [this] { return std::make_unique<store_module>(j); }, catalog};
    node::module_registrar<fake_host> front{
        "front", [this] { return std::make_unique<front_module>(j); }, catalog};
};

auto plan_of(const node::module_catalog<fake_host>& c, const std::vector<std::string>& names,
             bool composite = true) -> node::module_plan<fake_host> {
    auto r = node::plan_modules(c, names, composite);
    const auto* error = std::get_if<std::string>(&r);
    BOOST_REQUIRE_MESSAGE(error == nullptr, (error != nullptr ? *error : std::string()));
    return std::move(std::get<node::module_plan<fake_host>>(r));
}

auto error_of(const node::module_catalog<fake_host>& c, const std::vector<std::string>& names,
              bool composite = true) -> std::string {
    auto r = node::plan_modules(c, names, composite);
    const auto* error = std::get_if<std::string>(&r);
    return error == nullptr ? std::string("ok") : *error;
}

/// What a node does with a plan, in the order design.md gives.
void run(node::module_plan<fake_host>& plan, fake_host& host, node::module_registry& registry,
         const node::env_lookup& env) {
    for (auto& m : plan.modules) {
        const auto e = m->configure(env);
        BOOST_REQUIRE_MESSAGE(!e, e.value_or(""));
    }
    for (std::size_t i = 0; i < plan.modules.size(); ++i) {
        plan.modules[i]->register_groups(host, plan.spaces[i]);
    }
    for (auto& m : plan.modules) {
        m->publish(registry);
    }
    for (auto& m : plan.modules) {
        m->start(registry);
    }
}

void shut_down(node::module_plan<fake_host>& plan) {
    for (auto& m : std::views::reverse(plan.modules)) {
        m->drain();
    }
    for (auto& m : std::views::reverse(plan.modules)) {
        m->stop();
    }
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_composite_group_space_partitions_ids_and_keys) {
    const auto dynamodb = node::group_space::composite(node::module_numbers::dynamodb);
    BOOST_TEST(dynamodb.first() == std::uint64_t{2} << 56);
    BOOST_TEST(dynamodb.last() == (std::uint64_t{3} << 56) - 1);
    // DynamoDB's bootstrap groups and per-voter split ranges fit inside.
    BOOST_TEST(dynamodb.to_host(1) == (std::uint64_t{2} << 56) + 1);
    const auto voter_range = std::uint64_t{5} << 48;
    BOOST_TEST(dynamodb.contains(dynamodb.to_host(voter_range)));
    BOOST_TEST(dynamodb.to_local(dynamodb.to_host(voter_range)).value_or(0) == voter_range);
    BOOST_CHECK_THROW((void)dynamodb.to_host(std::uint64_t{1} << 56), std::out_of_range);

    const auto kinesis = node::group_space::composite(node::module_numbers::kinesis);
    BOOST_TEST(!dynamodb.overlaps(kinesis));
    BOOST_TEST(!kinesis.contains(dynamodb.to_host(1)));
    BOOST_TEST(!kinesis.to_local(dynamodb.to_host(1)).has_value());

    // Keys: each module's under its own prefix, never another's.
    const auto key = dynamodb.to_host_key("orders/pk");
    BOOST_TEST(key == std::string("\0\x02/orders/pk", 12));
    BOOST_TEST(dynamodb.to_local_key(key).value_or("") == "orders/pk");
    BOOST_TEST(!kinesis.to_local_key(key).has_value());

    // The last allowed number still fits under 2^64; 0 and 255 do not.
    const auto top = node::group_space::composite(node::module_numbers::max);
    BOOST_TEST(top.last() == (std::uint64_t{255} << 56) - 1);
    BOOST_CHECK_THROW((void)node::group_space::composite(0), std::invalid_argument);
    BOOST_CHECK_THROW((void)node::group_space::composite(255), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(a_standalone_group_space_keeps_todays_ids_and_keys) {
    const auto s = node::group_space::standalone(node::module_numbers::dynamodb);
    BOOST_TEST(s.to_host(1) == 1U);
    BOOST_TEST(s.to_host(std::uint64_t{3} << 48) == std::uint64_t{3} << 48);
    BOOST_TEST(s.key_prefix().empty());
    BOOST_TEST(s.to_host_key("orders/pk") == "orders/pk");
    BOOST_TEST(s.to_local_key("orders/pk").value_or("") == "orders/pk");
    BOOST_TEST(s.contains(~std::uint64_t{0}));
    // Every id is its, so two stand-alone spaces always collide.
    BOOST_TEST(s.overlaps(node::group_space::standalone(node::module_numbers::kinesis)));
}

BOOST_AUTO_TEST_CASE(co_located_modules_call_in_process_as_the_caller) {
    two_modules t;
    // Named front-first; the store starts first because front starts
    // after it.
    auto plan = plan_of(t.catalog, node::split_services(" front , store,"));
    BOOST_REQUIRE_EQUAL(plan.modules.size(), 2U);
    BOOST_TEST(plan.modules[0]->name() == "store");
    BOOST_TEST(plan.modules[1]->name() == "front");

    fake_host host;
    node::module_registry registry;
    run(plan, host, registry, env_of({{"KYTHIRA_STORE_GREETING", "hi"}}));
    BOOST_TEST(registry.names() == std::vector<std::string>{"store.client"});

    // Every module published before any started.
    const std::vector<std::string> started{"store register", "front register", "store publish",
                                           "store start", "front start in-process"};
    BOOST_TEST(t.j.events == started, boost::test_tools::per_element());

    // Each module's groups and keys sit in its own space.
    BOOST_TEST(host.groups.size() == 3U);
    for (std::size_t i = 0; i < plan.modules.size(); ++i) {
        const auto& space = plan.spaces[i];
        const auto mine =
            std::ranges::count_if(host.groups, [&](std::uint64_t g) { return space.contains(g); });
        BOOST_TEST(mine == (plan.modules[i]->name() == "store" ? 1 : 2));
    }
    BOOST_TEST(host.keys.contains(std::string("\0\x06/greeting", 11)));

    // The in-process call is authorized as the caller: a request with a
    // principal reads, one without is refused, as over the network.
    auto& front = dynamic_cast<front_module&>(*plan.modules[1]);
    BOOST_TEST(front.read("arn:aws:iam::123456789012:user/app", "greeting") == "hi");
    BOOST_TEST(front.read("", "greeting") == "denied");
    BOOST_TEST(plan.modules[0]->metrics().at(0).value == 1.0);

    shut_down(plan);
    const std::vector<std::string> stopped{"front drain", "store drain", "front stop",
                                           "store stop"};
    BOOST_TEST(std::vector<std::string>(t.j.events.end() - 4, t.j.events.end()) == stopped,
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_module_falls_back_to_the_network_when_alone) {
    two_modules t;
    auto plan = plan_of(t.catalog, {"front"}, false);
    BOOST_TEST(plan.spaces.at(0).key_prefix().empty());
    fake_host host;
    node::module_registry registry;
    run(plan, host, registry, env_of({}));
    BOOST_TEST(t.j.events.back() == "front start network");
    BOOST_TEST(dynamic_cast<front_module&>(*plan.modules[0]).read("arn:x", "greeting") ==
               "network");
    // Stand-alone ids are today's.
    BOOST_TEST(host.groups.at(0) == 1U);
}

BOOST_AUTO_TEST_CASE(plans_that_cannot_run_are_refused_with_the_reason) {
    two_modules t;
    BOOST_TEST(error_of(t.catalog, {"front", "hsm"}) ==
               "service 'hsm' is not compiled in; compiled in: front, store");
    BOOST_TEST(error_of(t.catalog, {}) == "no services named; compiled in: front, store");
    BOOST_TEST(error_of(t.catalog, {"store", "store"}) == "service 'store' is named twice");
    BOOST_TEST(error_of(t.catalog, {"front", "store"}, false) ==
               "a stand-alone daemon runs one service, not front, store");

    // A required module that is not named.
    journal j;
    node::module_catalog<fake_host> needy;
    const node::module_registrar<fake_host> r1(
        "front",
        [&j] { return std::make_unique<front_module>(j, std::vector<std::string>{"store"}); },
        needy);
    const node::module_registrar<fake_host> r2(
        "store", [&j] { return std::make_unique<store_module>(j); }, needy);
    BOOST_TEST(error_of(needy, {"front"}) == "service 'front' needs 'store' in KYTHIRA_SERVICES");
    BOOST_TEST(error_of(needy, {"front", "store"}) == "ok");

    // Two modules with one number.
    node::module_catalog<fake_host> clash;
    const node::module_registrar<fake_host> r3(
        "front", [&j] { return std::make_unique<front_module>(j); }, clash);
    const node::module_registrar<fake_host> r4(
        "store", [&j] { return std::make_unique<store_module>(j, 2); }, clash);
    BOOST_TEST(error_of(clash, {"front", "store"}) ==
               "services 'store' and 'front' claim the same group ids (module number 2)");

    // A registrar name that disagrees with the module's own.
    node::module_catalog<fake_host> misnamed;
    const node::module_registrar<fake_host> r5(
        "cache", [&j] { return std::make_unique<store_module>(j); }, misnamed);
    BOOST_TEST(error_of(misnamed, {"cache"}) ==
               "module registered as 'cache' names itself 'store'");

    // A bad setting is the module's to report.
    store_module bad(j);
    BOOST_TEST(bad.configure(env_of({{"KYTHIRA_STORE_GREETING", "bad"}})).value_or("") ==
               "KYTHIRA_STORE_GREETING: bad is not a greeting");
}

BOOST_AUTO_TEST_CASE(the_registry_and_catalog_refuse_mistakes) {
    node::module_registry registry;
    registry.publish<int>("answer", std::make_shared<int>(42));
    BOOST_TEST(*registry.lookup<int>("answer") == 42);
    BOOST_TEST(registry.lookup<int>("question") == nullptr);
    BOOST_CHECK_THROW(registry.publish<int>("answer", std::make_shared<int>(1)), std::logic_error);
    BOOST_CHECK_THROW((void)registry.lookup<double>("answer"), std::logic_error);
    BOOST_CHECK_THROW(registry.publish<int>("null", nullptr), std::invalid_argument);

    journal j;
    node::module_catalog<fake_host> catalog;
    const node::module_registrar<fake_host> r(
        "store", [&j] { return std::make_unique<store_module>(j); }, catalog);
    BOOST_CHECK_THROW(node::module_registrar<fake_host>(
                          "store", [&j] { return std::make_unique<store_module>(j); }, catalog),
                      std::logic_error);
    BOOST_TEST(catalog.make("front") == nullptr);
    BOOST_TEST(catalog.names() == std::vector<std::string>{"store"});
}

BOOST_AUTO_TEST_CASE(services_lists_are_split_on_commas) {
    BOOST_TEST(node::split_services("dynamodb,iam") ==
               (std::vector<std::string>{"dynamodb", "iam"}));
    BOOST_TEST(node::split_services(" dynamodb , , iam ,") ==
               (std::vector<std::string>{"dynamodb", "iam"}));
    BOOST_TEST(node::split_services("").empty());
}
