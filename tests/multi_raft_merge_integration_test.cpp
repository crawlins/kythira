// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_merge_integration_test.cpp
/// @brief Shard merge: prepare and commit (task 20), the abandon handshake
///        (task 21), and the preconditions (task 22) of
///        `.kiro/specs/multi-raft/`.
///
/// Merge is the only genuinely distributed operation in the design — it spans
/// two Raft groups — and the design names its abandon handshake as the second
/// place the whole feature's risk concentrates. Releasing a frozen source
/// *wrongly* is the one way this protocol corrupts data: a source that resumes
/// serving while some target replica has already applied the commit means two
/// shards own one range.
///
/// So the cases below are built around that. Commit and abandon are mutually
/// exclusive because both are decided by the target's single log, and the tests
/// drive each of them into the other's window.

#define BOOST_TEST_MODULE multi_raft_merge_integration_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"
#include "recording_metrics.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_merge_integration_test"), nullptr};
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
using kythira::merge_options;
using kythira::merge_reason;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::shard_alignment_required_exception;
using kythira::shard_busy_exception;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_merging_exception;
using kythira::shard_not_adjacent_exception;
using kythira::shard_not_leader_exception;
using kythira::shard_operation_state;
using kythira::shard_range;
using kythira::signal_channel;
using kythira::split_options;
using kythira::split_reason;
using kythira::tombstone_reason;
using kythira::testing::emitted_metric;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;
using kythira::testing::metric_recorder;

/// `recording_metrics` with each in-progress emission kept per thread.
///
/// A Raft node drives one metrics handle from replication continuations on
/// several threads at once, and `recording_metrics` builds each emission in a
/// member, so two threads interleaving `add_dimension` corrupt it — the
/// three-node failover case crashed on exactly that. Every emission is built
/// and emitted on a single thread, so a per-thread buffer keeps them apart.
class concurrent_recording_metrics {
public:
    concurrent_recording_metrics() : _recorder(std::make_shared<metric_recorder>()) {}
    explicit concurrent_recording_metrics(std::shared_ptr<metric_recorder> r)
        : _recorder(std::move(r)) {}

    auto set_metric_name(std::string_view name) -> void { pending().name = std::string(name); }
    auto add_dimension(std::string_view name, std::string_view value) -> void {
        pending().dimensions.emplace(std::string(name), std::string(value));
    }
    auto add_one() -> void {}
    auto add_count(std::int64_t) -> void {}
    auto add_duration(std::chrono::nanoseconds) -> void {}
    auto add_value(double) -> void {}
    auto emit() -> void {
        if (_recorder && !pending().name.empty()) {
            _recorder->record(pending());
        }
        pending() = {};
    }

private:
    static auto pending() -> emitted_metric& {
        thread_local emitted_metric p;
        return p;
    }
    std::shared_ptr<metric_recorder> _recorder;
};

static_assert(kythira::metrics<concurrent_recording_metrics>);

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
    using metrics_type = concurrent_recording_metrics;
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

using host_type = multi_raft<host_types, key_type, group_id_type>;
using config_type = multi_raft_config<host_types, key_type, group_id_type>;
using descriptor_type = shard_descriptor<group_id_type, key_type, node_id_t>;

constexpr std::size_t k_node_count = 3;
constexpr group_id_type k_left = 1;
constexpr group_id_type k_right = 2;

auto range_of(std::optional<key_type> start, std::optional<key_type> end) -> shard_range<key_type> {
    return shard_range<key_type>{._start = std::move(start), ._end = std::move(end)};
}

auto left_keys() -> std::vector<key_type> {
    return {"alpha", "bravo", "charlie"};
}
auto right_keys() -> std::vector<key_type> {
    return {"november", "oscar", "papa"};
}

/// Two adjacent shards, both replicated on all three nodes: `(-inf, m)` and
/// `[m, +inf)`. Colocated by construction, which is a merge precondition.
auto left_descriptor() -> descriptor_type {
    return descriptor_type{._group_id = k_left,
                           ._range = range_of(std::nullopt, key_type{"m"}),
                           ._epoch = shard_epoch{._version = 2, ._conf_version = 0},
                           ._voters = {1, 2, 3},
                           ._learners = {},
                           ._leader_hint = std::nullopt};
}
auto right_descriptor() -> descriptor_type {
    return descriptor_type{._group_id = k_right,
                           ._range = range_of(key_type{"m"}, std::nullopt),
                           ._epoch = shard_epoch{._version = 2, ._conf_version = 0},
                           ._voters = {1, 2, 3},
                           ._learners = {},
                           ._leader_hint = std::nullopt};
}

class merge_cluster {
public:
    explicit merge_cluster(std::function<void(config_type&)> tweak = {})
        : _tweak(std::move(tweak)) {
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
            _hosts.back()->create_group(left_descriptor());
            _hosts.back()->create_group(right_descriptor());
        }
        for (auto& h : _hosts) {
            h->start();
        }
        _running = true;
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            _paused.push_back(std::make_unique<std::atomic<bool>>(false));
            _idle.push_back(std::make_unique<std::atomic<bool>>(true));
        }
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            _drivers.emplace_back([this, i] { drive(i); });
        }
    }

    ~merge_cluster() {
        _running = false;
        for (auto& t : _drivers) {
            if (t.joinable()) {
                t.join();
            }
        }
        for (auto& h : _hosts) {
            h->stop();
        }
    }

    merge_cluster(const merge_cluster&) = delete;
    auto operator=(const merge_cluster&) -> merge_cluster& = delete;

    [[nodiscard]] auto host(node_id_t id) -> host_type& { return *_hosts.at(id - 1); }
    [[nodiscard]] auto fabric() -> message_fabric& { return _fabric; }

    [[nodiscard]] auto leader_of(group_id_type group) -> host_type* {
        for (auto& h : _hosts) {
            auto* n = h->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return h.get();
            }
        }
        return nullptr;
    }

    auto await(const std::function<bool()>& predicate, std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return predicate();
    }

    auto await_both_leaders(std::chrono::milliseconds budget) -> bool {
        return await([&] { return leader_of(k_left) != nullptr && leader_of(k_right) != nullptr; },
                     budget);
    }

    auto put(const key_type& key, const std::string& value,
             std::chrono::milliseconds budget = std::chrono::milliseconds{5000}) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& h : _hosts) {
                auto desc = h->resolve(key);
                if (!desc.has_value()) {
                    continue;
                }
                auto* n = h->group_node(desc->_group_id);
                if (n == nullptr || !n->is_leader()) {
                    continue;
                }
                auto f = h->submit_command(
                    key, host_types::state_machine_type::make_put_command(key, value),
                    std::chrono::milliseconds{2000});
                if (f.wait(std::chrono::milliseconds{2000})) {
                    try {
                        std::ignore = std::move(f).get();
                        return true;
                    } catch (...) {
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    [[nodiscard]] auto state_of(node_id_t node, group_id_type group) -> std::vector<std::byte> {
        auto* n = _hosts.at(node - 1)->group_node(group);
        if (n == nullptr) {
            return {};
        }
        return n->with_state_machine(
            [](host_types::state_machine_type& sm) { return sm.get_state(); });
    }

    [[nodiscard]] auto keys_of(node_id_t node, group_id_type group) -> std::string {
        auto* n = _hosts.at(node - 1)->group_node(group);
        if (n == nullptr) {
            return "<no replica>";
        }
        return n->with_state_machine([](host_types::state_machine_type& sm) {
            std::string out;
            for (const auto& k : left_keys()) {
                if (sm.contains(k)) {
                    out += (out.empty() ? "" : ",") + k;
                }
            }
            for (const auto& k : right_keys()) {
                if (sm.contains(k)) {
                    out += (out.empty() ? "" : ",") + k;
                }
            }
            return out.empty() ? std::string{"<empty>"} : out;
        });
    }

    auto quiesce() -> void {
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            *_paused[i] = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!_idle[i]->load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    auto resume() -> void {
        for (auto& p : _paused) {
            *p = false;
        }
    }

    [[nodiscard]] auto tiling_problem() -> std::optional<std::string> {
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            if (auto problem = _hosts.at(id - 1)->shard_map_snapshot().check_tiling()) {
                return "node " + std::to_string(id) + ": " + *problem;
            }
        }
        return std::nullopt;
    }

private:
    auto make_config(node_id_t id) -> config_type {
        config_type cfg{
            .node_id = id,
            .network_client = fabric_client{_fabric, id},
            .network_server = fabric_server{_fabric, id},
            .store_factory =
                [](const group_id_type&) { return host_types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{120};
        cfg.config._election_timeout_max = std::chrono::milliseconds{260};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{25};
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.route_retry_backoff = std::chrono::milliseconds{2};
        if (_tweak) {
            _tweak(cfg);
        }
        return cfg;
    }

    auto drive(std::size_t index) -> void {
        while (_running.load()) {
            if (_paused[index]->load()) {
                *_idle[index] = true;
            } else {
                *_idle[index] = false;
                _hosts[index]->tick();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        *_idle[index] = true;
    }

    std::function<void(config_type&)> _tweak;
    message_fabric _fabric{8};
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::vector<std::unique_ptr<std::atomic<bool>>> _paused;
    std::vector<std::unique_ptr<std::atomic<bool>>> _idle;
    std::atomic<bool> _running{false};
};

/// One host, two adjacent single-voter shards, ticked by hand.
///
/// The freeze is a *local* state transition applied on every replica, so one
/// replica proves it exactly as well as three — and removes the race that makes
/// the three-node version unusable for this. Because `merge_prepare`'s apply
/// DEFERS the commit proposal to the host's apply phase, not ticking after the
/// prepare leaves the source frozen for as long as the test wants, with no
/// partition to arrange and nothing to time.
class manual_host {
public:
    explicit manual_host(std::function<void(config_type&)> tweak = {}) {
        config_type cfg{
            .node_id = 1,
            .network_client = fabric_client{_fabric, 1},
            .network_server = fabric_server{_fabric, 1},
            .store_factory =
                [](const group_id_type&) { return host_types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{40};
        cfg.config._election_timeout_max = std::chrono::milliseconds{80};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{10};
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.route_retry_backoff = std::chrono::milliseconds{1};
        cfg.metrics = concurrent_recording_metrics{_recorder};
        if (tweak) {
            tweak(cfg);
        }

        auto left = left_descriptor();
        left._voters = {1};
        auto right = right_descriptor();
        right._voters = {1};

        _host = std::make_unique<host_type>(std::move(cfg));
        _host->create_group(left);
        _host->create_group(right);
        _host->start();
    }

    ~manual_host() { _host->stop(); }

    manual_host(const manual_host&) = delete;
    auto operator=(const manual_host&) -> manual_host& = delete;

    [[nodiscard]] auto host() -> host_type& { return *_host; }
    [[nodiscard]] auto recorder() -> metric_recorder& { return *_recorder; }

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

    auto await_both_leaders() -> bool {
        return tick_until([&] {
            return _host->group_node(k_left)->is_leader() &&
                   _host->group_node(k_right)->is_leader();
        });
    }

    /// @brief Write through the host, ticking so the entry can commit.
    auto put(const key_type& key, const std::string& value) -> std::exception_ptr {
        auto f =
            _host->submit_command(key, host_types::state_machine_type::make_put_command(key, value),
                                  std::chrono::milliseconds{2000});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (!f.wait(std::chrono::milliseconds{5}) &&
               std::chrono::steady_clock::now() < deadline) {
            _host->tick();
        }
        try {
            std::ignore = std::move(f).get();
            return nullptr;
        } catch (...) {
            return std::current_exception();
        }
    }

    /// @brief Propose a merge and tick only until the source is frozen.
    ///
    /// Stopping there is the point: the commit proposal is deferred to the
    /// apply phase, so the source stays frozen until the caller ticks again.
    auto freeze_source(merge_options options = {}) -> bool {
        auto f = _host->merge_shards(k_right, k_left, options, std::chrono::milliseconds{5000});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!f.wait(std::chrono::milliseconds{5}) &&
               std::chrono::steady_clock::now() < deadline) {
            _host->tick();
        }
        try {
            std::ignore = std::move(f).get();
        } catch (...) {
            return false;
        }
        return _host->operation_state(k_right) == shard_operation_state::merging_source;
    }

    /// @brief Resolve a future while ticking the host underneath it.
    template<typename Future>
    auto settle(Future&& f, std::chrono::milliseconds budget = std::chrono::milliseconds{6000})
        -> std::exception_ptr {
        const auto deadline = std::chrono::steady_clock::now() + budget;
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

private:
    std::shared_ptr<metric_recorder> _recorder{std::make_shared<metric_recorder>()};
    message_fabric _fabric{2};
    std::unique_ptr<host_type> _host;
};

template<typename Future>
auto settle(Future&& f, std::chrono::milliseconds budget = std::chrono::milliseconds{8000})
    -> std::exception_ptr {
    if (!f.wait(budget)) {
        return std::make_exception_ptr(std::runtime_error("settle: future never resolved"));
    }
    try {
        std::ignore = std::forward<Future>(f).get();
        return nullptr;
    } catch (...) {
        return std::current_exception();
    }
}

template<typename E> auto is_a(const std::exception_ptr& e) -> bool {
    if (!e) {
        return false;
    }
    try {
        std::rethrow_exception(e);
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

/// Seed both shards and wait for the writes to settle everywhere.
auto seed(merge_cluster& c) -> void {
    for (const auto& k : left_keys()) {
        BOOST_REQUIRE(c.put(k, "v-" + k));
    }
    for (const auto& k : right_keys()) {
        BOOST_REQUIRE(c.put(k, "v-" + k));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{600});
}

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_merge_integration)

// ── task 20: prepare and commit ──────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_merge_absorbs_the_source_into_the_survivor,
                     *boost::unit_test::timeout(240)) {
    merge_cluster c;
    BOOST_REQUIRE(c.await_both_leaders(std::chrono::seconds{20}));
    seed(c);

    auto* source_leader = c.leader_of(k_right);
    BOOST_REQUIRE(source_leader != nullptr);
    BOOST_REQUIRE(settle(source_leader->merge_shards(k_right, k_left,
                                                     std::chrono::milliseconds{8000})) == nullptr);

    // The commit is proposed by whichever host leads the target, which learns
    // of the merge by applying `merge_prepare` on its own local source replica.
    BOOST_REQUIRE(c.await(
        [&] {
            for (node_id_t id = 1; id <= k_node_count; ++id) {
                if (c.host(id).applied_merge_count() == 0) {
                    return false;
                }
            }
            return true;
        },
        std::chrono::seconds{30}));

    std::this_thread::sleep_for(std::chrono::milliseconds{600});
    c.quiesce();

    // Every target replica agrees, byte for byte: they all force-applied the
    // same source tail before reading its state.
    const auto reference = c.state_of(1, k_left);
    for (node_id_t id = 2; id <= k_node_count; ++id) {
        BOOST_CHECK_MESSAGE(c.state_of(id, k_left) == reference,
                            "survivor differs: node 1 holds [" << c.keys_of(1, k_left) << "], node "
                                                               << id << " holds ["
                                                               << c.keys_of(id, k_left) << "]");
    }

    // The survivor holds both key sets, the source is gone everywhere, and the
    // map still tiles.
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        auto* survivor = c.host(id).group_node(k_left);
        BOOST_REQUIRE(survivor != nullptr);
        survivor->with_state_machine([&](host_types::state_machine_type& sm) {
            for (const auto& k : left_keys()) {
                BOOST_CHECK_MESSAGE(sm.contains(k), "node " << id << " lost left key " << k);
            }
            for (const auto& k : right_keys()) {
                BOOST_CHECK_MESSAGE(sm.contains(k), "node " << id << " never absorbed " << k);
            }
        });
        BOOST_CHECK(c.host(id).group_node(k_right) == nullptr);
        BOOST_CHECK(c.host(id).is_tombstoned(k_right));
    }
    if (auto problem = c.tiling_problem()) {
        BOOST_FAIL("merge broke the tiling: " << *problem);
    }

    // The survivor's range covers everything, and its version exceeds both
    // inputs.
    const auto survivor_desc = c.host(1).local_descriptor(k_left);
    BOOST_REQUIRE(survivor_desc.has_value());
    BOOST_CHECK(!survivor_desc->_range._start.has_value());
    BOOST_CHECK(!survivor_desc->_range._end.has_value());
    BOOST_CHECK_GT(survivor_desc->_epoch._version, 2u);
    c.resume();
}

BOOST_AUTO_TEST_CASE(a_client_of_the_source_is_redirected_after_the_merge,
                     *boost::unit_test::timeout(240)) {
    merge_cluster c;
    BOOST_REQUIRE(c.await_both_leaders(std::chrono::seconds{20}));
    seed(c);

    auto* source_leader = c.leader_of(k_right);
    BOOST_REQUIRE(settle(source_leader->merge_shards(k_right, k_left,
                                                     std::chrono::milliseconds{8000})) == nullptr);
    BOOST_REQUIRE(
        c.await([&] { return c.host(1).applied_merge_count() > 0; }, std::chrono::seconds{30}));
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    // A key that used to belong to the source now routes to the survivor, and
    // still works.
    BOOST_CHECK(c.put("oscar", "after-merge"));
    for (const auto& id : {node_id_t{1}, node_id_t{2}, node_id_t{3}}) {
        const auto desc = c.host(id).resolve("oscar");
        BOOST_REQUIRE(desc.has_value());
        BOOST_CHECK_EQUAL(desc->_group_id, k_left);
    }
}

BOOST_AUTO_TEST_CASE(a_frozen_source_refuses_proposals_and_reads, *boost::unit_test::timeout(120)) {
    // Serving here would produce a write the survivor never absorbs, or a read
    // of state that is about to move.
    manual_host h;
    BOOST_REQUIRE(h.await_both_leaders());
    for (const auto& k : left_keys()) {
        BOOST_REQUIRE(h.put(k, "v-" + k) == nullptr);
    }
    for (const auto& k : right_keys()) {
        BOOST_REQUIRE(h.put(k, "v-" + k) == nullptr);
    }

    BOOST_REQUIRE(h.freeze_source());

    // A write to a key the source still nominally owns is refused, and the
    // error names the target so the caller knows what it is waiting for rather
    // than merely that something failed.
    using merging = shard_merging_exception<group_id_type>;
    const auto write_err = h.put("oscar", "x");
    BOOST_REQUIRE(is_a<merging>(write_err));
    try {
        std::rethrow_exception(write_err);
    } catch (const merging& e) {
        BOOST_CHECK_EQUAL(e.group_id(), k_right);
        BOOST_CHECK_EQUAL(e.target_group_id(), k_left);
        BOOST_CHECK(std::string{e.what()}.find("retry") != std::string::npos);
    }

    // A read is refused by the same path, for the same reason.
    auto read = h.host().read_state(key_type{"oscar"}, std::chrono::milliseconds{500});
    BOOST_REQUIRE(read.wait(std::chrono::milliseconds{2000}));
    bool read_refused = false;
    try {
        std::ignore = std::move(read).get();
    } catch (const merging&) {
        read_refused = true;
    } catch (...) {
    }
    BOOST_CHECK(read_refused);

    // The OTHER shard is untouched: freezing is per-shard, not per-host.
    BOOST_CHECK(h.put("alpha", "still-writable") == nullptr);
}

BOOST_AUTO_TEST_CASE(a_frozen_source_resumes_serving_after_a_rollback,
                     *boost::unit_test::timeout(120)) {
    manual_host h;
    BOOST_REQUIRE(h.await_both_leaders());
    for (const auto& k : right_keys()) {
        BOOST_REQUIRE(h.put(k, "v-" + k) == nullptr);
    }
    BOOST_REQUIRE(h.freeze_source());

    // Abandon: the target records `merge_abandoned` in its OWN log, and only
    // once that is committed does the source propose `merge_rollback`.
    auto f = h.host().abandon_merge(k_right, std::chrono::milliseconds{5000});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!f.wait(std::chrono::milliseconds{5}) && std::chrono::steady_clock::now() < deadline) {
        h.host().tick();
    }
    BOOST_REQUIRE(f.wait(std::chrono::milliseconds{100}));
    BOOST_CHECK_NO_THROW(std::ignore = std::move(f).get());

    BOOST_REQUIRE(h.tick_until(
        [&] { return h.host().operation_state(k_right) == shard_operation_state::stable; }));
    BOOST_CHECK_GE(h.host().rolled_back_merge_count(), 1u);

    // The source serves again, and the map still tiles.
    BOOST_CHECK(h.put("oscar", "after-rollback") == nullptr);
    BOOST_CHECK(!h.host().shard_map_snapshot().check_tiling().has_value());

    // And an abandoned target refuses to commit even if the merge is retried:
    // once abandoned, never committed — both decisions come from its one log.
    BOOST_CHECK(h.host().group_node(k_right) != nullptr);
}

BOOST_AUTO_TEST_CASE(a_frozen_source_refuses_a_second_operation, *boost::unit_test::timeout(120)) {
    // Only `stable` admits a new operation, and starting one IS the transition,
    // so a conflicting pair is impossible by construction rather than by
    // check-then-act.
    manual_host h;
    BOOST_REQUIRE(h.await_both_leaders());
    for (const auto& k : right_keys()) {
        BOOST_REQUIRE(h.put(k, "v-" + k) == nullptr);
    }
    BOOST_REQUIRE(h.freeze_source());

    using busy = shard_busy_exception<group_id_type>;
    BOOST_CHECK(
        is_a<busy>(settle(h.host().merge_shards(k_right, k_left, std::chrono::milliseconds{500}))));
    BOOST_CHECK(is_a<busy>(
        settle(h.host().split_shard(k_right, {"oscar"}, std::chrono::milliseconds{500}))));
}

// ── task 22: the preconditions ───────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(non_adjacent_shards_are_refused, *boost::unit_test::timeout(240)) {
    merge_cluster c;
    BOOST_REQUIRE(c.await_both_leaders(std::chrono::seconds{20}));

    // Make the two shards non-adjacent by moving the source's start bound away
    // from the target's end bound, without touching the replicas.
    auto detached = right_descriptor();
    detached._range._start = key_type{"z"};
    detached._epoch._version = 5;
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        BOOST_REQUIRE(c.host(id).set_local_descriptor(k_right, detached));
    }

    auto* source_leader = c.leader_of(k_right);
    BOOST_REQUIRE(source_leader != nullptr);
    using not_adjacent = shard_not_adjacent_exception<group_id_type, key_type>;
    const auto err =
        settle(source_leader->merge_shards(k_right, k_left, std::chrono::milliseconds{4000}));
    BOOST_CHECK(is_a<not_adjacent>(err));
    BOOST_CHECK(source_leader->operation_state(k_right) == shard_operation_state::stable);
}

BOOST_AUTO_TEST_CASE(non_colocated_replica_sets_are_refused_rather_than_shipped,
                     *boost::unit_test::timeout(240)) {
    // Each target replica absorbs from the source replica ON ITS OWN MACHINE.
    // A target replica with no local source peer cannot apply the commit, so
    // this fails fast rather than attempting a cross-network state transfer
    // mid-merge — the operation the whole design is built to avoid.
    merge_cluster c;
    BOOST_REQUIRE(c.await_both_leaders(std::chrono::seconds{20}));

    auto misaligned = right_descriptor();
    misaligned._voters = {1, 2};  // the target has three
    misaligned._epoch._version = 5;
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        BOOST_REQUIRE(c.host(id).set_local_descriptor(k_right, misaligned));
    }

    auto* source_leader = c.leader_of(k_right);
    BOOST_REQUIRE(source_leader != nullptr);
    using alignment_error = shard_alignment_required_exception<group_id_type, node_id_t>;
    const auto err =
        settle(source_leader->merge_shards(k_right, k_left, std::chrono::milliseconds{4000}));
    BOOST_REQUIRE(is_a<alignment_error>(err));
    try {
        std::rethrow_exception(err);
    } catch (const alignment_error& e) {
        BOOST_CHECK(std::string{e.what()}.find("_auto_align") != std::string::npos);
    }
    BOOST_CHECK(source_leader->operation_state(k_right) == shard_operation_state::stable);
}

BOOST_AUTO_TEST_CASE(a_non_leader_and_an_unknown_shard_are_both_refused,
                     *boost::unit_test::timeout(240)) {
    merge_cluster c;
    BOOST_REQUIRE(c.await_both_leaders(std::chrono::seconds{20}));

    auto* source_leader = c.leader_of(k_right);
    BOOST_REQUIRE(source_leader != nullptr);
    const auto leader_id = source_leader->node_id();

    using not_leader = shard_not_leader_exception<group_id_type, node_id_t>;
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        if (id == leader_id) {
            continue;
        }
        const auto err =
            settle(c.host(id).merge_shards(k_right, k_left, std::chrono::milliseconds{4000}));
        BOOST_CHECK(is_a<not_leader>(err));
        BOOST_CHECK(c.host(id).operation_state(k_right) == shard_operation_state::stable);
    }

    BOOST_CHECK(settle(source_leader->merge_shards(999, k_left, std::chrono::milliseconds{2000})) !=
                nullptr);
}

// ── task 21: the abandon handshake ───────────────────────────────────────────

BOOST_AUTO_TEST_CASE(abandoning_a_shard_that_is_not_merging_is_refused) {
    merge_cluster c;
    BOOST_REQUIRE(c.await_both_leaders(std::chrono::seconds{20}));

    auto* source_leader = c.leader_of(k_right);
    BOOST_REQUIRE(source_leader != nullptr);
    using busy = shard_busy_exception<group_id_type>;
    BOOST_CHECK(
        is_a<busy>(settle(source_leader->abandon_merge(k_right, std::chrono::milliseconds{2000}))));
    BOOST_CHECK(settle(source_leader->abandon_merge(999, std::chrono::milliseconds{2000})) !=
                nullptr);
}

BOOST_AUTO_TEST_CASE(once_the_commit_is_proposed_the_target_refuses_to_abandon,
                     *boost::unit_test::timeout(240)) {
    // Commit always wins, because both decisions are made by the target's one
    // log. A source that resumed while a target replica had already applied the
    // commit would mean two shards owning one range.
    merge_cluster c;
    BOOST_REQUIRE(c.await_both_leaders(std::chrono::seconds{20}));
    seed(c);

    auto* source_leader = c.leader_of(k_right);
    BOOST_REQUIRE(source_leader != nullptr);
    BOOST_REQUIRE(settle(source_leader->merge_shards(k_right, k_left,
                                                     std::chrono::milliseconds{8000})) == nullptr);

    // Let the commit reach at least the proposal stage.
    BOOST_REQUIRE(
        c.await([&] { return c.host(1).applied_merge_count() > 0; }, std::chrono::seconds{30}));

    // The source is gone, so there is nothing left to abandon — which is the
    // strongest possible form of "commit won".
    const auto err = settle(source_leader->abandon_merge(k_right, std::chrono::milliseconds{2000}));
    BOOST_CHECK(err != nullptr);
    BOOST_CHECK_EQUAL(source_leader->rolled_back_merge_count(), 0u);
}

BOOST_AUTO_TEST_CASE(the_merge_survives_a_target_leader_failover, *boost::unit_test::timeout(300)) {
    // A target leader failover mid-merge must not lose the decision: the new
    // leader replays the target's log and inherits whatever it says.
    merge_cluster c;
    BOOST_REQUIRE(c.await_both_leaders(std::chrono::seconds{20}));
    seed(c);

    auto* source_leader = c.leader_of(k_right);
    auto* target_leader = c.leader_of(k_left);
    BOOST_REQUIRE(source_leader != nullptr);
    BOOST_REQUIRE(target_leader != nullptr);
    const auto old_target_leader = target_leader->node_id();

    BOOST_REQUIRE(settle(source_leader->merge_shards(k_right, k_left,
                                                     std::chrono::milliseconds{8000})) == nullptr);
    // Kill the target's leader immediately; whichever replica takes over
    // finishes the merge, or the whole thing stays consistent.
    c.fabric().kill(old_target_leader);
    std::this_thread::sleep_for(std::chrono::milliseconds{1500});
    c.fabric().revive(old_target_leader);

    BOOST_REQUIRE(c.await(
        [&] {
            std::size_t done = 0;
            for (node_id_t id = 1; id <= k_node_count; ++id) {
                if (c.host(id).applied_merge_count() > 0) {
                    ++done;
                }
            }
            // A majority is enough to say the decision stuck; the third catches
            // up by replication.
            return done >= 2;
        },
        std::chrono::seconds{60}));

    std::this_thread::sleep_for(std::chrono::milliseconds{1000});
    c.quiesce();
    if (auto problem = c.tiling_problem()) {
        BOOST_FAIL("failover during merge broke the tiling: " << *problem);
    }
    // No node may hold both the survivor's extended range and a live source.
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        const auto survivor = c.host(id).local_descriptor(k_left);
        if (survivor.has_value() && !survivor->_range._end.has_value()) {
            BOOST_CHECK_MESSAGE(c.host(id).group_node(k_right) == nullptr,
                                "node " << id << " owns the merged range AND a live source");
        }
    }
    c.resume();
}

// ── task 21: the stalled-merge signal ────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_stalled_merge_is_emitted_as_a_metric_naming_the_target,
                     *boost::unit_test::timeout(120)) {
    // A source frozen past the threshold is unavailable but correct, and the
    // operator is told by name rather than left to guess: a warning AND
    // `merge.stalled{group, target}`, not merely an internal counter.
    manual_host h{[](config_type& cfg) {
        cfg.merge_stall_warning_after = std::chrono::milliseconds{0};
        cfg.policy_interval = std::chrono::milliseconds{0};
    }};
    BOOST_REQUIRE(h.await_both_leaders());
    for (const auto& k : right_keys()) {
        BOOST_REQUIRE(h.put(k, "v-" + k) == nullptr);
    }
    BOOST_REQUIRE(h.freeze_source());
    // Every tick that still sees the source frozen reports it.
    if (h.host().stalled_merge_report_count() == 0) {
        h.host().tick();
    }
    BOOST_REQUIRE_GT(h.host().stalled_merge_report_count(), 0u);

    const auto groups = h.recorder().dimension_values("kythira.multiraft.merge.stalled", "group");
    const auto targets = h.recorder().dimension_values("kythira.multiraft.merge.stalled", "target");
    BOOST_REQUIRE_EQUAL(groups.size(), h.host().stalled_merge_report_count());
    BOOST_CHECK_EQUAL(groups.front(), std::to_string(k_right));
    BOOST_REQUIRE(!targets.empty());
    BOOST_CHECK_EQUAL(targets.front(), std::to_string(k_left));
}

// ── task 25: precedence (Requirement 17.2, 17.3, 17.6) ──────────────────────

BOOST_AUTO_TEST_CASE(an_admin_merge_in_flight_preempts_both_automatic_channels,
                     *boost::unit_test::timeout(120)) {
    // An accepted admin command suspends the automatic channels for the shards
    // it affects until it resolves, and each loser is refused AS preempted,
    // naming the channel that outranked it: in the count, the log line and the
    // metric dimension. An operator debugging "why didn't my policy fire" has
    // to be able to see that they outranked it themselves.
    manual_host h;
    BOOST_REQUIRE(h.await_both_leaders());
    for (const auto& k : right_keys()) {
        BOOST_REQUIRE(h.put(k, "v-" + k) == nullptr);
    }
    BOOST_REQUIRE(h.freeze_source());  // an admin merge, frozen mid-protocol

    for (const auto channel : {signal_channel::policy, signal_channel::placement_driver}) {
        const auto would = h.host().would_admit(k_right, channel);
        BOOST_CHECK(would._gate == arbiter_gate::preempted);
        BOOST_REQUIRE(would._preempted_by.has_value());
        BOOST_CHECK(*would._preempted_by == signal_channel::admin);
    }

    const auto before = h.host().rejection_count(arbiter_gate::preempted);
    split_options policy_split{};
    policy_split._channel = signal_channel::policy;
    policy_split._reason = split_reason::size;
    using busy = shard_busy_exception<group_id_type>;
    BOOST_CHECK(is_a<busy>(settle(
        h.host().split_shard(k_right, {"oscar"}, policy_split, std::chrono::milliseconds{500}))));

    merge_options policy_merge{};
    policy_merge._channel = signal_channel::policy;
    policy_merge._reason = merge_reason::size;
    BOOST_CHECK(is_a<busy>(settle(
        h.host().merge_shards(k_right, k_left, policy_merge, std::chrono::milliseconds{500}))));
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::preempted), before + 2);

    const auto split_losers =
        h.recorder().dimension_values("kythira.multiraft.split.rejected", "preempted_by");
    const auto merge_losers =
        h.recorder().dimension_values("kythira.multiraft.merge.rejected", "preempted_by");
    BOOST_REQUIRE_EQUAL(split_losers.size(), 1u);
    BOOST_CHECK_EQUAL(split_losers.front(), "admin");
    BOOST_REQUIRE_EQUAL(merge_losers.size(), 1u);
    BOOST_CHECK_EQUAL(merge_losers.front(), "admin");

    // The operator's own second command is NOT a preemption — admin does not
    // outrank admin — and is refused by the state gate as before.
    BOOST_CHECK(is_a<busy>(
        settle(h.host().split_shard(k_right, {"oscar"}, std::chrono::milliseconds{500}))));
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::preempted), before + 2);

    // Once the merge resolves, the suspension lifts with it.
    BOOST_REQUIRE(h.tick_until([&] { return h.host().applied_merge_count() > 0; }));
    BOOST_CHECK(h.host().would_admit(k_left, signal_channel::policy)._gate !=
                arbiter_gate::preempted);
}

BOOST_AUTO_TEST_CASE(a_driver_merge_preempts_the_policy_and_is_not_aborted_for_the_operator,
                     *boost::unit_test::timeout(120)) {
    // Admin outranks the driver, but an operation already proposed is never
    // aborted to make room (Requirement 17.4): the operator is refused by the
    // state gate. The policy, which the driver DOES outrank, is preempted.
    manual_host h;
    BOOST_REQUIRE(h.await_both_leaders());
    for (const auto& k : right_keys()) {
        BOOST_REQUIRE(h.put(k, "v-" + k) == nullptr);
    }
    merge_options driver{};
    driver._channel = signal_channel::placement_driver;
    driver._reason = merge_reason::placement_driver;
    BOOST_REQUIRE(h.freeze_source(driver));

    const auto policy = h.host().would_admit(k_right, signal_channel::policy);
    BOOST_CHECK(policy._gate == arbiter_gate::preempted);
    BOOST_REQUIRE(policy._preempted_by.has_value());
    BOOST_CHECK(*policy._preempted_by == signal_channel::placement_driver);

    const auto preempted_before = h.host().rejection_count(arbiter_gate::preempted);
    const auto state_before = h.host().rejection_count(arbiter_gate::state);
    using busy = shard_busy_exception<group_id_type>;
    BOOST_CHECK(is_a<busy>(
        settle(h.host().split_shard(k_right, {"oscar"}, std::chrono::milliseconds{500}))));
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::state), state_before + 1);
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::preempted), preempted_before);
}

BOOST_AUTO_TEST_CASE(a_policy_merge_is_arbitrated_as_the_policy, *boost::unit_test::timeout(120)) {
    // The policy channel used to reach `merge_shards` as if it were the
    // operator, which bypassed every gate that exists only for the automatic
    // channels. A frozen shard proves it: the operator may merge it, the
    // policy may not.
    manual_host h;
    BOOST_REQUIRE(h.await_both_leaders());
    BOOST_REQUIRE(h.host().freeze_shard(k_right));

    merge_options policy{};
    policy._channel = signal_channel::policy;
    policy._reason = merge_reason::size;
    using busy = shard_busy_exception<group_id_type>;
    BOOST_CHECK(is_a<busy>(
        settle(h.host().merge_shards(k_right, k_left, policy, std::chrono::milliseconds{500}))));
    BOOST_CHECK(h.host().operation_state(k_right) == shard_operation_state::frozen);
}

// ── task 22: `_auto_align` ───────────────────────────────────────────────────

namespace {

/// The right shard as seen with an extra voter the left one lacks: not
/// colocated, so a merge needs alignment first.
auto misaligned_right() -> descriptor_type {
    auto d = right_descriptor();
    d._voters = {1, 2};
    d._epoch._version = 5;
    return d;
}
auto realigned_right() -> descriptor_type {
    auto d = right_descriptor();
    d._voters = {1};
    d._epoch._version = 6;
    return d;
}

}  // namespace

BOOST_AUTO_TEST_CASE(auto_align_asks_the_driver_then_merges_once_colocated,
                     *boost::unit_test::timeout(120)) {
    std::vector<std::pair<group_id_type, group_id_type>> asked;
    manual_host h{[&](config_type& cfg) {
        cfg.request_merge_alignment = [&](const descriptor_type& source,
                                          const descriptor_type& target) {
            asked.emplace_back(source._group_id, target._group_id);
            return true;
        };
    }};
    BOOST_REQUIRE(h.await_both_leaders());
    for (const auto& k : right_keys()) {
        BOOST_REQUIRE(h.put(k, "v-" + k) == nullptr);
    }
    BOOST_REQUIRE(h.host().set_local_descriptor(k_right, misaligned_right()));

    merge_options options{};
    options._auto_align = true;
    options._align_timeout = std::chrono::seconds{10};
    auto merge = h.host().merge_shards(k_right, k_left, options, std::chrono::milliseconds{5000});

    // Asked exactly once, with the pair the operator named, and parked.
    BOOST_REQUIRE_EQUAL(asked.size(), 1u);
    BOOST_CHECK(asked.front() == std::make_pair(k_right, k_left));
    for (int i = 0; i < 10; ++i) {
        h.host().tick();
    }
    BOOST_CHECK(!merge.isReady());
    BOOST_CHECK_EQUAL(h.host().pending_alignment_count(), 1u);
    BOOST_CHECK(h.host().operation_state(k_right) == shard_operation_state::stable);

    // The wait is an accepted admin command: it holds BOTH shards against the
    // automatic channels (Requirement 17.3).
    for (const auto group : {k_right, k_left}) {
        const auto would = h.host().would_admit(group, signal_channel::policy);
        BOOST_CHECK(would._gate == arbiter_gate::preempted);
        BOOST_CHECK(would._preempted_by == std::optional{signal_channel::admin});
    }
    BOOST_CHECK(h.host().would_admit(k_left, signal_channel::admin)._admitted);

    // The driver's operators land; the next tick sees colocation and proposes.
    BOOST_REQUIRE(h.host().set_local_descriptor(k_right, realigned_right()));
    BOOST_CHECK(h.settle(std::move(merge)) == nullptr);
    BOOST_CHECK_EQUAL(h.host().pending_alignment_count(), 0u);
    BOOST_REQUIRE(h.tick_until([&] { return h.host().applied_merge_count() > 0; }));
    BOOST_CHECK(h.host().would_admit(k_left, signal_channel::policy)._gate !=
                arbiter_gate::preempted);
}

BOOST_AUTO_TEST_CASE(auto_align_gives_up_after_align_timeout_and_releases_the_shards,
                     *boost::unit_test::timeout(120)) {
    manual_host h{[](config_type& cfg) {
        cfg.request_merge_alignment = [](const descriptor_type&, const descriptor_type&) {
            return true;  // accepted, and then nothing ever moves
        };
    }};
    BOOST_REQUIRE(h.await_both_leaders());
    BOOST_REQUIRE(h.host().set_local_descriptor(k_right, misaligned_right()));

    merge_options options{};
    options._auto_align = true;
    options._align_timeout = std::chrono::milliseconds{300};
    const auto before = h.host().rejection_count(arbiter_gate::alignment_required);
    const auto err =
        h.settle(h.host().merge_shards(k_right, k_left, options, std::chrono::milliseconds{5000}));
    using alignment_error = shard_alignment_required_exception<group_id_type, node_id_t>;
    BOOST_CHECK(is_a<alignment_error>(err));
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::alignment_required), before + 1);
    BOOST_CHECK_EQUAL(h.host().pending_alignment_count(), 0u);
    for (const auto group : {k_right, k_left}) {
        BOOST_CHECK(h.host().would_admit(group, signal_channel::policy)._gate !=
                    arbiter_gate::preempted);
    }
}

BOOST_AUTO_TEST_CASE(auto_align_with_no_driver_to_ask_fails_at_once,
                     *boost::unit_test::timeout(120)) {
    // Nobody to ask means nothing will ever align them; waiting out the
    // timeout would only delay the same answer.
    manual_host h;
    BOOST_REQUIRE(h.await_both_leaders());
    BOOST_REQUIRE(h.host().set_local_descriptor(k_right, misaligned_right()));

    merge_options options{};
    options._auto_align = true;
    auto merge = h.host().merge_shards(k_right, k_left, options, std::chrono::milliseconds{5000});
    BOOST_REQUIRE(merge.isReady());
    using alignment_error = shard_alignment_required_exception<group_id_type, node_id_t>;
    BOOST_CHECK(is_a<alignment_error>(settle(std::move(merge))));
    BOOST_CHECK_EQUAL(h.host().pending_alignment_count(), 0u);
}

BOOST_AUTO_TEST_CASE(auto_align_does_not_wait_on_a_refusal_alignment_cannot_fix,
                     *boost::unit_test::timeout(120)) {
    bool asked = false;
    manual_host h{[&](config_type& cfg) {
        cfg.request_merge_alignment = [&](const descriptor_type&, const descriptor_type&) {
            asked = true;
            return true;
        };
    }};
    BOOST_REQUIRE(h.await_both_leaders());
    auto detached = misaligned_right();
    detached._range._start = key_type{"z"};  // no longer adjacent, as well
    BOOST_REQUIRE(h.host().set_local_descriptor(k_right, detached));

    merge_options options{};
    options._auto_align = true;
    using not_adjacent = shard_not_adjacent_exception<group_id_type, key_type>;
    BOOST_CHECK(is_a<not_adjacent>(
        settle(h.host().merge_shards(k_right, k_left, options, std::chrono::milliseconds{5000}))));
    BOOST_CHECK(!asked);
}

BOOST_AUTO_TEST_SUITE_END()
