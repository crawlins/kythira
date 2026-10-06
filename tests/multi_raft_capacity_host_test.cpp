// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_capacity_host_test.cpp
/// @brief The host-side half of `.kiro/specs/elastic-shard-capacity/` (tasks 1
///        and 2): the quorum-manager shadow, the split capacity gate, the
///        allocation-suggestion counter, operator feedback, and the
///        empty-registry heartbeat.
///
/// Each of these is small and each is load-bearing for something larger:
///
///  - The shadow is what lets a host bundle name a real provider manager at
///    all. Before it, such a bundle was ill-formed — so the first test here is
///    one that would not have compiled.
///  - The gate is the only thing standing between a full disk and a split that
///    cannot write its children. It must refuse on every channel, never on a
///    merge, and never at all when unconfigured.
///  - A machine that holds no shard yet is *exactly* the machine a capacity
///    controller just provisioned. If it did not heartbeat, it would be
///    invisible until something had already been placed on it, which nothing
///    would ever do.

#define BOOST_TEST_MODULE multi_raft_capacity_host_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/shard_placement_driver.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_capacity_host_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::arbiter_gate;
using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::node_report;
using kythira::operator_outcome;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_id_allocation;
using kythira::shard_operation;
using kythira::shard_operation_state;
using kythira::shard_report;
using kythira::signal_channel;
using kythira::skipped_operator_reason;
using kythira::split_options;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_t = std::uint64_t;

struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type = fabric_client;
    using network_server_type = fabric_server;

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kythira::test_key_value_state_machine<log_index_type>;

    using configuration_type = kythira::raft_configuration;

    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;

    using request_vote_request_type =
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type, group_id_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type, group_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type,
                                        group_id_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type, group_id_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type,
                                          group_id_type>;
    using install_snapshot_response_type =
        kythira::install_snapshot_response<term_id_type, group_id_type>;
};

// ── task 1: a bundle naming a manager that cannot be default-constructed ─────

/// @brief Shaped like every real provider manager: one explicit constructor
///        taking a validated config, and no default constructor.
///
/// A `quorum_manager` in full, so that the bundle below is exactly what an
/// operator would write when naming `aws_ec2_quorum_manager` or any of the
/// others — without pulling a provider SDK into a host test.
class config_only_quorum_manager {
public:
    using node_id_type = std::uint64_t;
    using address_type = std::string;
    using placement_group_id_type = std::string;

    struct config {
        std::string _region;
    };
    explicit config_only_quorum_manager(config cfg) : _cfg(std::move(cfg)) {
        if (_cfg._region.empty()) {
            throw std::invalid_argument("region must be non-empty");
        }
    }

    auto assess_quorum(const std::vector<kythira::node_placement<node_id_type, std::string>>&)
        -> kythira::future_default<kythira::quorum_health<node_id_type, std::string>> {
        return kythira::future_factory_default::makeFuture(
            kythira::quorum_health<node_id_type, std::string>{});
    }
    auto provision_node(std::string, std::optional<node_id_type>)
        -> kythira::future_default<kythira::peer_info<node_id_type, std::string>> {
        return kythira::future_factory_default::makeFuture(
            kythira::peer_info<node_id_type, std::string>{});
    }
    auto decommission_node(const node_id_type&) -> kythira::future_default<void> {
        return kythira::future_factory_default::makeFuture();
    }
    [[nodiscard]] auto topology() const -> kythira::desired_topology<std::string> { return {}; }
    auto maintain_quorum(const std::vector<kythira::node_placement<node_id_type, std::string>>& c)
        -> kythira::future_default<kythira::quorum_health<node_id_type, std::string>> {
        return assess_quorum(c);
    }

private:
    config _cfg;
};

static_assert(
    kythira::quorum_manager<config_only_quorum_manager, std::uint64_t, std::string, std::string>);
static_assert(!std::is_default_constructible_v<config_only_quorum_manager>);

/// The host bundle an operator writes once a controller owns provisioning.
/// Note it declares no `address_type`: the shadow must not need one.
struct provisioning_host_types : host_types {
    using quorum_manager_type = config_only_quorum_manager;
};

using provisioning_host_type = multi_raft<provisioning_host_types, key_type, group_id_type>;

// What every group's `node` actually sees: the no-op, whatever the bundle says.
// A thousand groups must not hold a thousand provisioning authorities.
static_assert(
    std::is_same_v<typename provisioning_host_type::group_node_type::quorum_manager_type,
                   kythira::no_op_quorum_manager<std::uint64_t, std::string, std::string>>);
static_assert(
    std::is_same_v<typename provisioning_host_type::group_types::quorum_manager_type,
                   kythira::no_op_quorum_manager<std::uint64_t, std::string, std::string>>);

// ── the single-host harness ──────────────────────────────────────────────────

using host_type = multi_raft<host_types, key_type, group_id_type>;
using config_type = multi_raft_config<host_types, key_type, group_id_type>;
using descriptor_type = shard_descriptor<group_id_type, key_type, node_id_t>;
using report_type = shard_report<group_id_type, key_type, node_id_t>;
using operation_type = shard_operation<group_id_type, key_type, node_id_t>;

constexpr group_id_type k_group = 1;

auto workload_keys() -> std::vector<key_type> {
    return {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot"};
}

/// @brief What the machine's disk says, adjustable mid-test.
struct disk {
    std::atomic<std::uint64_t> _capacity{1'000'000'000};
    std::atomic<std::uint64_t> _available{1'000'000'000};

    auto probe() -> std::function<std::pair<std::uint64_t, std::uint64_t>()> {
        return [this] {
            return std::pair<std::uint64_t, std::uint64_t>{_capacity.load(), _available.load()};
        };
    }
};

/// One host, one single-voter shard, ticked by hand.
template<typename Types = host_types> class capacity_host {
public:
    using host_t = multi_raft<Types, key_type, group_id_type>;
    using config_t = multi_raft_config<Types, key_type, group_id_type>;

    explicit capacity_host(std::function<void(config_t&)> tweak = {}, bool create_group = true) {
        config_t cfg{
            .node_id = 1,
            .network_client = fabric_client{_fabric, 1},
            .network_server = fabric_server{_fabric, 1},
            .store_factory =
                [](const group_id_type&) { return typename Types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{40};
        cfg.config._election_timeout_max = std::chrono::milliseconds{80};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{10};
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.allocate_group_ids = [this](std::size_t n) {
            std::vector<group_id_type> out;
            for (std::size_t i = 0; i < n; ++i) {
                out.push_back(_next_id++);
            }
            return out;
        };
        cfg.split_merge_interval = std::chrono::milliseconds{0};
        // Heartbeats are driven by hand where a test cares about them.
        cfg.heartbeat_interval = std::chrono::milliseconds{0};
        if (tweak) {
            tweak(cfg);
        }
        _host = std::make_unique<host_t>(std::move(cfg));
        if (create_group) {
            _host->create_group(
                descriptor_type{._group_id = k_group,
                                ._range = kythira::unbounded_shard_range<key_type>(),
                                ._epoch = shard_epoch{},
                                ._voters = {1},
                                ._learners = {},
                                ._leader_hint = std::nullopt});
        }
        _host->start();
    }

    ~capacity_host() { _host->stop(); }

    capacity_host(const capacity_host&) = delete;
    auto operator=(const capacity_host&) -> capacity_host& = delete;

    [[nodiscard]] auto host() -> host_t& { return *_host; }

    auto tick_until(const std::function<bool()>& predicate,
                    std::chrono::milliseconds budget = std::chrono::milliseconds{5000}) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            _host->tick();
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return predicate();
    }

    auto await_leader() -> bool {
        return tick_until([&] { return _host->group_node(k_group)->is_leader(); });
    }

    template<typename Future> auto settle(Future&& f) -> std::exception_ptr {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{6};
        while (!f.wait(std::chrono::milliseconds{5}) &&
               std::chrono::steady_clock::now() < deadline) {
            _host->tick();
        }
        if (!f.wait(std::chrono::milliseconds{100})) {
            return std::make_exception_ptr(std::runtime_error("never resolved"));
        }
        try {
            std::ignore = std::forward<Future>(f).get();
            return nullptr;
        } catch (...) {
            return std::current_exception();
        }
    }

    auto seed() -> void {
        for (const auto& k : workload_keys()) {
            auto f =
                _host->submit_command(k, Types::state_machine_type::make_put_command(k, "v-" + k),
                                      std::chrono::milliseconds{2000});
            BOOST_REQUIRE(settle(std::move(f)) == nullptr);
        }
    }

private:
    message_fabric _fabric{2};
    std::unique_ptr<host_t> _host;
    group_id_type _next_id{100};
};

auto split_on(signal_channel channel) -> split_options {
    split_options o{};
    o._channel = channel;
    return o;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_capacity_host)

// ── task 1 ───────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_bundle_naming_a_config_only_manager_runs_a_host,
                     *boost::unit_test::timeout(120)) {
    // Before the shadow this did not compile: `create_group_impl` default-
    // constructs `node_config::quorum_manager`, and this manager has no default
    // constructor. Constructing, electing and serving proves more than the
    // static_asserts above: the shadow is not merely declared, it is the type
    // the running node was built with.
    capacity_host<provisioning_host_types> h;
    BOOST_REQUIRE(h.await_leader());
    h.seed();
    BOOST_CHECK_EQUAL(h.host().group_count(), 1U);
}

// ── task 2: the capacity gate ────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_gate_is_off_unless_a_floor_is_set, *boost::unit_test::timeout(120)) {
    // Requirement 6.3: unset floor, today's behaviour — even with a probe that
    // says the disk is completely full.
    disk d;
    d._available = 0;
    capacity_host<> h{[&](config_type& cfg) { cfg.capacity_probe = d.probe(); }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    BOOST_CHECK(h.settle(h.host().split_shard(k_group, {"delta"},
                                              std::chrono::milliseconds{5000})) == nullptr);
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::capacity), 0U);
}

BOOST_AUTO_TEST_CASE(a_floor_without_a_probe_leaves_the_gate_open,
                     *boost::unit_test::timeout(120)) {
    // A host that does not know its free space must not refuse every split on
    // a guess.
    capacity_host<> h{[](config_type& cfg) { cfg.split_capacity_floor_bytes = 1'000'000; }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();
    BOOST_CHECK(h.settle(h.host().split_shard(k_group, {"delta"},
                                              std::chrono::milliseconds{5000})) == nullptr);
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::capacity), 0U);
}

BOOST_AUTO_TEST_CASE(a_split_below_the_floor_is_refused_counted_and_reported,
                     *boost::unit_test::timeout(120)) {
    disk d;
    capacity_host<> h{[&](config_type& cfg) {
        cfg.capacity_probe = d.probe();
        cfg.split_capacity_floor_bytes = 1'000'000;
    }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    d._available = 999'999;
    const auto err =
        h.settle(h.host().split_shard(k_group, {"delta"}, std::chrono::milliseconds{5000}));
    BOOST_CHECK(err != nullptr);
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::capacity), 1U);
    BOOST_CHECK_EQUAL(h.host().group_count(), 1U);
    // Released, not wedged in `splitting`.
    BOOST_CHECK(h.host().operation_state(k_group) == shard_operation_state::stable);

    // Requirement 6.6: the controller sees it in the shard report, once.
    const auto first = h.host().build_shard_reports();
    BOOST_REQUIRE_EQUAL(first.size(), 1U);
    BOOST_CHECK_EQUAL(first[0]._capacity_refusals, 1U);
    const auto second = h.host().build_shard_reports();
    BOOST_REQUIRE_EQUAL(second.size(), 1U);
    BOOST_CHECK_EQUAL(second[0]._capacity_refusals, 0U);

    // And the gate opens again once the disk has room.
    d._available = 1'000'000'000;
    BOOST_CHECK(h.settle(h.host().split_shard(k_group, {"delta"},
                                              std::chrono::milliseconds{5000})) == nullptr);
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::capacity), 1U);
}

BOOST_AUTO_TEST_CASE(a_projected_post_split_shortfall_is_refused, *boost::unit_test::timeout(120)) {
    // Above the floor now, but not after the children's snapshots are written.
    disk d;
    capacity_host<> h{[&](config_type& cfg) {
        cfg.capacity_probe = d.probe();
        cfg.split_capacity_floor_bytes = 1'000'000;
    }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    std::size_t parent_size = 0;
    h.host().group_node(k_group)->with_state_machine(
        [&](host_types::state_machine_type& sm) { parent_size = sm.approximate_size_bytes(); });
    BOOST_REQUIRE_GT(parent_size, 1U);

    d._available = 1'000'000 + parent_size - 1;
    BOOST_CHECK(h.settle(h.host().split_shard(k_group, {"delta"},
                                              std::chrono::milliseconds{5000})) != nullptr);
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::capacity), 1U);

    d._available = 1'000'000 + parent_size;
    BOOST_CHECK(h.settle(h.host().split_shard(k_group, {"delta"},
                                              std::chrono::milliseconds{5000})) == nullptr);
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::capacity), 1U);
}

BOOST_AUTO_TEST_CASE(the_gate_applies_to_every_channel, *boost::unit_test::timeout(120)) {
    // Requirement 6.4. Admin is the interesting one: every other gate that
    // exists to protect the cluster from automation lets admin through, and
    // this one must not, because the disk is no larger for an administrator.
    disk d;
    capacity_host<> h{[&](config_type& cfg) {
        cfg.capacity_probe = d.probe();
        cfg.split_capacity_floor_bytes = 1'000'000;
    }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();
    d._available = 0;

    std::uint64_t expected = 0;
    for (const auto channel :
         {signal_channel::admin, signal_channel::policy, signal_channel::placement_driver}) {
        BOOST_CHECK(h.settle(h.host().split_shard(k_group, {"delta"}, split_on(channel),
                                                  std::chrono::milliseconds{5000})) != nullptr);
        BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::capacity), ++expected);
    }

    // The driver's split operator goes through the same gate. It is accepted
    // as an operator — the host tried — and refused by the arbiter, which is
    // the distinction `apply_operator` draws for every asynchronous outcome.
    const auto epoch = h.host().local_descriptor(k_group)->_epoch;
    operation_type op;
    op._group_id = k_group;
    op._operation_id = 9;
    op._epoch = epoch;
    op._operator = kythira::split_operator<key_type>{._at_keys = {"delta"}};
    std::ignore = h.host().apply_operator(op);
    BOOST_CHECK(h.tick_until(
        [&] { return h.host().rejection_count(arbiter_gate::capacity) == expected + 1; }));
    BOOST_CHECK_EQUAL(h.host().group_count(), 1U);
}

BOOST_AUTO_TEST_CASE(a_merge_is_never_refused_for_capacity, *boost::unit_test::timeout(120)) {
    // Requirement 6.5: a merge reduces consumption.
    disk d;
    capacity_host<> h{[&](config_type& cfg) {
        cfg.capacity_probe = d.probe();
        cfg.split_capacity_floor_bytes = 1'000'000;
    }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();
    BOOST_REQUIRE(h.settle(h.host().split_shard(k_group, {"delta"},
                                                std::chrono::milliseconds{5000})) == nullptr);
    BOOST_REQUIRE(h.tick_until([&] { return h.host().group_count() == 2U; }));
    BOOST_REQUIRE(h.tick_until([&] {
        auto* n = h.host().group_node(100);
        return n != nullptr && n->is_leader();
    }));

    d._available = 0;
    BOOST_CHECK(h.settle(h.host().merge_shards(100, k_group, std::chrono::milliseconds{5000})) ==
                nullptr);
    BOOST_CHECK(h.tick_until([&] { return h.host().applied_merge_count() == 1U; }));
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::capacity), 0U);
}

// ── task 2: allocation suggestions ───────────────────────────────────────────

BOOST_AUTO_TEST_CASE(allocation_suggestions_the_host_cannot_apply_are_counted,
                     *boost::unit_test::timeout(120)) {
    // Design §9, rejected alternative 2: children inherit the parent's
    // replicas. A driver that suggests otherwise is told — by a counter — not
    // silently ignored.
    std::vector<shard_id_allocation<group_id_type, node_id_t>> script;
    std::mutex script_mutex;
    group_id_type next = 200;
    capacity_host<> h{[&](config_type& cfg) {
        cfg.allocate_shard_ids = [&](std::size_t n) {
            std::lock_guard lock(script_mutex);
            std::vector<shard_id_allocation<group_id_type, node_id_t>> out;
            for (std::size_t i = 0; i < n; ++i) {
                auto a = script.empty() ? shard_id_allocation<group_id_type, node_id_t>{}
                                        : script.front();
                if (!script.empty()) {
                    script.erase(script.begin());
                }
                a._group_id = next++;
                out.push_back(a);
            }
            return out;
        };
    }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    // Elsewhere: counted.
    {
        std::lock_guard lock(script_mutex);
        script.push_back({._suggested_voters = {2, 3, 4}});
    }
    BOOST_REQUIRE(h.settle(h.host().split_shard(k_group, {"delta"},
                                                std::chrono::milliseconds{5000})) == nullptr);
    BOOST_CHECK_EQUAL(h.host().suggestion_ignored_count(), 1U);
    BOOST_REQUIRE(h.tick_until([&] { return h.host().group_count() == 2U; }));
    // ...and the child really did inherit the parent's voters.
    BOOST_CHECK(h.host().local_descriptor(200)->_voters == std::vector<node_id_t>{1});

    // The parent's own set, or none at all: honoured trivially, not counted.
    {
        std::lock_guard lock(script_mutex);
        script.push_back({._suggested_voters = {1}});
    }
    BOOST_REQUIRE(h.settle(h.host().split_shard(k_group, {"bravo"},
                                                std::chrono::milliseconds{5000})) == nullptr);
    BOOST_REQUIRE(h.tick_until([&] {
        auto* n = h.host().group_node(200);
        return n != nullptr && n->is_leader();
    }));
    BOOST_REQUIRE(h.settle(h.host().split_shard(200, {"echo"}, std::chrono::milliseconds{5000})) ==
                  nullptr);
    BOOST_CHECK_EQUAL(h.host().suggestion_ignored_count(), 1U);

    // A learner suggestion is a suggestion too.
    {
        std::lock_guard lock(script_mutex);
        script.push_back({._suggested_learners = {9}});
    }
    BOOST_REQUIRE(h.tick_until([&] { return h.host().group_count() == 4U; }));
    BOOST_REQUIRE(h.settle(h.host().split_shard(k_group, {"alpha1"},
                                                std::chrono::milliseconds{5000})) == nullptr);
    BOOST_CHECK_EQUAL(h.host().suggestion_ignored_count(), 2U);
}

// ── the heartbeat path ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_host_with_no_groups_still_heartbeats, *boost::unit_test::timeout(60)) {
    // Requirement 2.3 / 15.5. A freshly provisioned machine holds no shard; it
    // must still be visible as capacity, or nothing would ever place one on it.
    disk d;
    std::mutex m;
    std::vector<node_report<node_id_t>> seen;
    std::size_t shard_calls = 0;
    capacity_host<> h{[&](config_type& cfg) {
                          cfg.heartbeat_interval = std::chrono::milliseconds{1};
                          cfg.capacity_probe = d.probe();
                          cfg.node_labels = {"zone-b"};
                          cfg.report_node_heartbeat = [&](const node_report<node_id_t>& r) {
                              std::lock_guard lock(m);
                              seen.push_back(r);
                          };
                          cfg.report_shard_heartbeat = [&](const std::vector<report_type>& r) {
                              std::lock_guard lock(m);
                              ++shard_calls;
                              BOOST_CHECK(r.empty());
                              return std::vector<operation_type>{};
                          };
                      },
                      /*create_group=*/false};

    BOOST_CHECK_EQUAL(h.host().group_count(), 0U);
    BOOST_REQUIRE(h.tick_until([&] {
        std::lock_guard lock(m);
        return seen.size() >= 2;
    }));
    std::lock_guard lock(m);
    BOOST_CHECK_EQUAL(seen.back()._node_id, 1U);
    BOOST_CHECK_EQUAL(seen.back()._shard_count, 0U);
    BOOST_CHECK_EQUAL(seen.back()._leader_count, 0U);
    BOOST_CHECK_EQUAL(seen.back()._capacity_bytes, 1'000'000'000U);
    BOOST_REQUIRE_EQUAL(seen.back()._labels.size(), 1U);
    BOOST_CHECK_EQUAL(seen.back()._labels[0], "zone-b");
    BOOST_CHECK_GE(shard_calls, 2U);
}

BOOST_AUTO_TEST_CASE(operator_outcomes_are_reported_back_in_order, *boost::unit_test::timeout(60)) {
    // Requirement 13.6 needs the driver to *see* a skip. The host's counters
    // are on another machine as far as a control plane is concerned.
    std::mutex m;
    std::vector<operation_type> to_send;
    std::vector<operator_outcome> outcomes;
    capacity_host<> h{[&](config_type& cfg) {
        cfg.report_shard_heartbeat = [&](const std::vector<report_type>&) {
            std::lock_guard lock(m);
            return std::exchange(to_send, {});
        };
        cfg.report_operator_outcomes = [&](const std::vector<operator_outcome>& o) {
            std::lock_guard lock(m);
            outcomes.insert(outcomes.end(), o.begin(), o.end());
        };
    }};
    BOOST_REQUIRE(h.await_leader());

    const auto epoch = h.host().local_descriptor(k_group)->_epoch;
    auto stale = epoch;
    stale._version += 7;
    {
        std::lock_guard lock(m);
        operation_type a;
        a._group_id = k_group;
        a._operation_id = 1;
        a._epoch = stale;
        a._operator = kythira::add_replica_operator<node_id_t>{._node = 2};
        operation_type b = a;
        b._operation_id = 2;
        b._epoch = epoch;
        operation_type c = a;
        c._operation_id = 3;
        c._group_id = 77;
        to_send = {a, b, c};
    }
    BOOST_CHECK_EQUAL(h.host().heartbeat(), 3U);

    std::lock_guard lock(m);
    BOOST_REQUIRE_EQUAL(outcomes.size(), 3U);
    BOOST_CHECK_EQUAL(outcomes[0]._operation_id, 1U);
    BOOST_CHECK(!outcomes[0]._accepted);
    BOOST_CHECK(outcomes[0]._reason == skipped_operator_reason::stale_epoch);
    BOOST_CHECK_EQUAL(outcomes[1]._operation_id, 2U);
    BOOST_CHECK(outcomes[1]._accepted);
    BOOST_CHECK_EQUAL(outcomes[2]._operation_id, 3U);
    BOOST_CHECK(outcomes[2]._reason == skipped_operator_reason::unknown_shard);
}

BOOST_AUTO_TEST_SUITE_END()
