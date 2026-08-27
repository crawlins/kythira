// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_split_integration_test.cpp
/// @brief Shard split: proposal (task 17), apply (task 18), and lazy replica
///        creation (task 19) of `.kiro/specs/multi-raft/`.
///
/// Three claims are load-bearing, and the design names them as where the risk
/// concentrates:
///
/// 1. **Every replica produces byte-identical children.** The leader freezes
///    its decision into the entry and every replica applies the frozen answer.
///    A replica that recomputed anything — consulted its own statistics, asked
///    a policy, called the placement driver — would cut in a different place,
///    and nothing at the Raft level would notice because the logs would match.
/// 2. **Replaying a split entry changes nothing.** That is what makes the
///    "children first, then the parent" durability ordering safe without a
///    store that spans groups: a crash between the two replays the entry and
///    finds the children already there.
/// 3. **Group ids come from a cluster-scope authority or the split is
///    abandoned.** Inventing them locally is how two partitions end up with two
///    different shards sharing one group id.

#define BOOST_TEST_MODULE multi_raft_split_integration_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

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
#include <array>
#include <functional>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_split_integration_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::no_valid_split_key_exception;
using kythira::shard_busy_exception;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_not_leader_exception;
using kythira::shard_operation_state;
using kythira::split_key_out_of_range_exception;
using kythira::tombstone_reason;
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

using host_type = multi_raft<host_types, key_type, group_id_type>;
using config_type = multi_raft_config<host_types, key_type, group_id_type>;
using descriptor_type = shard_descriptor<group_id_type, key_type, node_id_t>;

constexpr std::size_t k_node_count = 3;

auto workload_keys() -> std::vector<key_type> {
    return {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india"};
}

/// A cluster-scope id authority. Handing out ids from one place is the whole
/// point — the test can also make it fail, which is the case that matters.
class id_authority {
public:
    auto allocate(std::size_t n) -> std::vector<group_id_type> {
        std::lock_guard lock(_mutex);
        ++_calls;
        if (_unavailable) {
            return {};
        }
        std::vector<group_id_type> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(_next++);
        }
        return out;
    }

    auto set_unavailable(bool v) -> void {
        std::lock_guard lock(_mutex);
        _unavailable = v;
    }
    [[nodiscard]] auto calls() -> std::size_t {
        std::lock_guard lock(_mutex);
        return _calls;
    }

private:
    std::mutex _mutex;
    group_id_type _next{100};
    bool _unavailable{false};
    std::size_t _calls{0};
};

/// Three hosts, one group over all three, plus a shared id authority.
class split_cluster {
public:
    explicit split_cluster(std::size_t node_count = k_node_count,
                           std::function<void(config_type&)> tweak = {})
        : _node_count(node_count), _tweak(std::move(tweak)) {
        for (node_id_t id = 1; id <= _node_count; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
        }
        for (node_id_t id = 1; id <= _node_count; ++id) {
            _hosts[id - 1]->create_group(root_descriptor());
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

    ~split_cluster() {
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

    split_cluster(const split_cluster&) = delete;
    auto operator=(const split_cluster&) -> split_cluster& = delete;

    [[nodiscard]] static auto root_descriptor() -> descriptor_type {
        return descriptor_type{._group_id = 1,
                               ._range = kythira::unbounded_shard_range<key_type>(),
                               ._epoch = shard_epoch{},
                               ._voters = {1, 2, 3},
                               ._learners = {},
                               ._leader_hint = std::nullopt};
    }

    [[nodiscard]] auto host(node_id_t id) -> host_type& { return *_hosts.at(id - 1); }
    [[nodiscard]] auto node_count() const -> std::size_t { return _node_count; }
    [[nodiscard]] auto ids() -> id_authority& { return _ids; }
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

    auto await_leader(group_id_type group, std::chrono::milliseconds budget) -> bool {
        return await([&] { return leader_of(group) != nullptr; }, budget);
    }

    /// @brief Write `key` through whichever host leads the shard owning it.
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

    /// @brief The keys `node` holds for `group`, for a failure message that
    /// says *what* diverged rather than only *that* something did.
    [[nodiscard]] auto keys_of(node_id_t node, group_id_type group) -> std::string {
        auto* n = _hosts.at(node - 1)->group_node(group);
        if (n == nullptr) {
            return "<no replica>";
        }
        return n->with_state_machine([](host_types::state_machine_type& sm) {
            std::string out;
            for (const auto& k : workload_keys()) {
                if (sm.contains(k)) {
                    out += (out.empty() ? "" : ",") + k;
                }
            }
            return out.empty() ? std::string{"<empty>"} : out;
        });
    }

    /// @brief One replica's state-machine bytes for `group`, or empty if absent.
    [[nodiscard]] auto state_of(node_id_t node, group_id_type group) -> std::vector<std::byte> {
        auto* n = _hosts.at(node - 1)->group_node(group);
        if (n == nullptr) {
            return {};
        }
        return n->with_state_machine(
            [](host_types::state_machine_type& sm) { return sm.get_state(); });
    }

    auto quiesce() -> void {
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            pause_and_await(i);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    auto resume() -> void {
        for (auto& p : _paused) {
            *p = false;
        }
    }

    auto pause_and_await(std::size_t index) -> void {
        *_paused[index] = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!_idle[index]->load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }

    [[nodiscard]] auto tiling_problem() -> std::optional<std::string> {
        for (node_id_t id = 1; id <= _node_count; ++id) {
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
        cfg.allocate_group_ids = [this](std::size_t n) { return _ids.allocate(n); };
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

    std::size_t _node_count;
    std::function<void(config_type&)> _tweak;
    message_fabric _fabric{8};
    id_authority _ids;
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::vector<std::unique_ptr<std::atomic<bool>>> _paused;
    std::vector<std::unique_ptr<std::atomic<bool>>> _idle;
    std::atomic<bool> _running{false};
};

template<typename Future>
auto settle(Future&& f, std::chrono::milliseconds budget = std::chrono::milliseconds{8000})
    -> std::exception_ptr {
    if (!f.wait(budget)) {
        return std::make_exception_ptr(std::runtime_error("settle: future never resolved"));
    }
    try {
        std::ignore = std::move(f).get();
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

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_split_integration)

// ── task 17: the proposal path ───────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_split_produces_children_carrying_exactly_the_parents_members,
                     *boost::unit_test::timeout(180)) {
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));
    for (const auto& k : workload_keys()) {
        BOOST_REQUIRE(c.put(k, "v"));
    }

    auto* leader = c.leader_of(1);
    BOOST_REQUIRE(leader != nullptr);
    const auto parent = *leader->local_descriptor(1);

    BOOST_REQUIRE(settle(leader->split_shard(1, {"echo"}, std::chrono::milliseconds{8000})) ==
                  nullptr);

    // Members one-for-one with the parent's: a child replica is created on
    // exactly the machines that already hold a parent replica, so no data moves.
    const auto rows = leader->shard_map_snapshot().descriptors();
    BOOST_REQUIRE_EQUAL(rows.size(), 2u);
    for (const auto& row : rows) {
        BOOST_CHECK(row._voters == parent._voters);
        BOOST_CHECK(row._learners == parent._learners);
        // version = parent.version + N, a single value for every child, so
        // epoch ordering stays total.
        BOOST_CHECK_EQUAL(row._epoch._version, parent._epoch._version + 2);
    }
    BOOST_CHECK(!leader->shard_map_snapshot().check_tiling().has_value());
}

BOOST_AUTO_TEST_CASE(a_vetoed_key_falls_back_to_the_state_machines_suggestion,
                     *boost::unit_test::timeout(180)) {
    // Requirement 9.4: a policy that says "split at K" cannot cut through an
    // entity the state machine says is indivisible. The veto comes first and
    // the suggestion is the fallback.
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));
    for (const auto& k : workload_keys()) {
        BOOST_REQUIRE(c.put(k, "v"));
    }

    auto* leader = c.leader_of(1);
    leader->group_node(1)->with_state_machine([](host_types::state_machine_type& sm) {
        sm.set_split_veto([](const std::string& k) { return k == "echo"; });
    });

    BOOST_REQUIRE(settle(leader->split_shard(1, {"echo"}, std::chrono::milliseconds{8000})) ==
                  nullptr);

    // It split — but not at the vetoed key. The fallback asks for up to
    // `batch_split_limit` suggestions, so the child count is whatever the state
    // machine offered, not the one key that was requested and refused.
    const auto rows = leader->shard_map_snapshot().descriptors();
    BOOST_REQUIRE_GT(rows.size(), 1u);
    if (auto problem = leader->shard_map_snapshot().check_tiling()) {
        BOOST_FAIL("fallback split broke the tiling: " << *problem);
    }
    for (const auto& row : rows) {
        if (row._range._end.has_value()) {
            BOOST_CHECK_MESSAGE(*row._range._end != "echo",
                                "the split cut at the vetoed key anyway");
        }
    }
}

BOOST_AUTO_TEST_CASE(the_fallback_is_bounded_by_the_batch_split_limit,
                     *boost::unit_test::timeout(180)) {
    // TiKV RFC 0006's batch split exists because one-key-at-a-time cannot keep
    // up with a fast sequential write. The limit is what keeps it from becoming
    // "shatter the shard into one key each".
    split_cluster c{k_node_count, [](config_type& cfg) { cfg.batch_split_limit = 2; }};
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));
    for (const auto& k : workload_keys()) {
        BOOST_REQUIRE(c.put(k, "v"));
    }

    auto* leader = c.leader_of(1);
    leader->group_node(1)->with_state_machine([](host_types::state_machine_type& sm) {
        sm.set_split_veto([](const std::string& k) { return k == "echo"; });
    });
    BOOST_REQUIRE(settle(leader->split_shard(1, {"echo"}, std::chrono::milliseconds{8000})) ==
                  nullptr);

    // At most `batch_split_limit` cut keys, so at most limit + 1 children.
    BOOST_CHECK_LE(leader->shard_map_snapshot().size(), 3u);
    BOOST_CHECK_GT(leader->shard_map_snapshot().size(), 1u);
}

BOOST_AUTO_TEST_CASE(every_key_vetoed_yields_no_valid_split_key_and_no_log_entry,
                     *boost::unit_test::timeout(180)) {
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));
    for (const auto& k : workload_keys()) {
        BOOST_REQUIRE(c.put(k, "v"));
    }

    auto* leader = c.leader_of(1);
    leader->group_node(1)->with_state_machine([](host_types::state_machine_type& sm) {
        sm.set_split_veto([](const std::string&) { return true; });
    });

    const auto err = settle(leader->split_shard(1, {"echo"}, std::chrono::milliseconds{4000}));
    BOOST_REQUIRE(is_a<no_valid_split_key_exception<group_id_type>>(err));

    // No entry was proposed and the shard is back to stable rather than wedged.
    BOOST_CHECK_EQUAL(leader->shard_map_snapshot().size(), 1u);
    BOOST_CHECK_EQUAL(leader->applied_split_count(), 0u);
    BOOST_CHECK(leader->operation_state(1) == shard_operation_state::stable);
}

BOOST_AUTO_TEST_CASE(an_unavailable_id_authority_abandons_the_split_rather_than_inventing_ids,
                     *boost::unit_test::timeout(180)) {
    // Inventing ids locally is how two partitions end up with two different
    // shards sharing one group id. Abandoning is the correct answer, and the
    // policy will simply try again next tick.
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));
    for (const auto& k : workload_keys()) {
        BOOST_REQUIRE(c.put(k, "v"));
    }

    c.ids().set_unavailable(true);
    auto* leader = c.leader_of(1);
    const auto err = settle(leader->split_shard(1, {"echo"}, std::chrono::milliseconds{4000}));
    BOOST_REQUIRE(err != nullptr);
    BOOST_CHECK_EQUAL(leader->shard_map_snapshot().size(), 1u);
    BOOST_CHECK_EQUAL(leader->applied_split_count(), 0u);
    BOOST_CHECK(leader->operation_state(1) == shard_operation_state::stable);

    // And it succeeds once the authority is back — the shard was left usable,
    // not wedged in `splitting`.
    c.ids().set_unavailable(false);
    BOOST_REQUIRE(settle(leader->split_shard(1, {"echo"}, std::chrono::milliseconds{8000})) ==
                  nullptr);
    BOOST_CHECK_EQUAL(leader->shard_map_snapshot().size(), 2u);
}

BOOST_AUTO_TEST_CASE(a_key_outside_the_range_and_a_non_leader_are_both_refused,
                     *boost::unit_test::timeout(180)) {
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));
    for (const auto& k : workload_keys()) {
        BOOST_REQUIRE(c.put(k, "v"));
    }

    auto* leader = c.leader_of(1);
    BOOST_REQUIRE(settle(leader->split_shard(1, {"echo"}, std::chrono::milliseconds{8000})) ==
                  nullptr);

    // Group 1 now owns only `(-inf, echo)`; a key above that is out of range.
    using out_of_range_error = split_key_out_of_range_exception<group_id_type, key_type>;
    const auto err = settle(leader->split_shard(1, {"india"}, std::chrono::milliseconds{4000}));
    BOOST_CHECK(is_a<out_of_range_error>(err));

    // A replica that does not lead the shard refuses, with the hint attached.
    for (node_id_t id = 1; id <= c.node_count(); ++id) {
        auto* n = c.host(id).group_node(1);
        if (n == nullptr || n->is_leader()) {
            continue;
        }
        using not_leader_error = shard_not_leader_exception<group_id_type, node_id_t>;
        const auto not_leader =
            settle(c.host(id).split_shard(1, {"bravo"}, std::chrono::milliseconds{4000}));
        BOOST_CHECK(is_a<not_leader_error>(not_leader));
    }

    // And an unknown group is not invented.
    BOOST_CHECK(settle(leader->split_shard(999, {"a"}, std::chrono::milliseconds{2000})) !=
                nullptr);
}

// ── task 18: the apply path ──────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(every_replica_produces_byte_identical_children,
                     *boost::unit_test::timeout(180)) {
    // The single most important assertion in this file. A replica that
    // recomputed anything at apply time would cut in a different place, and
    // nothing at the Raft level would notice.
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));
    for (const auto& k : workload_keys()) {
        BOOST_REQUIRE(c.put(k, "v-" + k));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    auto* leader = c.leader_of(1);
    BOOST_REQUIRE(settle(leader->split_shard(1, {"charlie", "golf"},
                                             std::chrono::milliseconds{8000})) == nullptr);

    const auto rows = leader->shard_map_snapshot().descriptors();
    BOOST_REQUIRE_EQUAL(rows.size(), 3u);

    // Wait for every replica to have applied the split and created its children.
    BOOST_REQUIRE(c.await(
        [&] {
            for (node_id_t id = 1; id <= c.node_count(); ++id) {
                for (const auto& row : rows) {
                    if (c.host(id).group_node(row._group_id) == nullptr) {
                        return false;
                    }
                }
                if (c.host(id).applied_split_count() == 0) {
                    return false;
                }
            }
            return true;
        },
        std::chrono::seconds{20}));

    std::this_thread::sleep_for(std::chrono::milliseconds{500});
    c.quiesce();

    for (const auto& row : rows) {
        const auto reference = c.state_of(1, row._group_id);
        for (node_id_t id = 2; id <= c.node_count(); ++id) {
            BOOST_CHECK_MESSAGE(c.state_of(id, row._group_id) == reference,
                                "shard " << row._group_id << " differs: node 1 holds ["
                                         << c.keys_of(1, row._group_id) << "], node " << id
                                         << " holds [" << c.keys_of(id, row._group_id) << "]");
        }
    }

    // Together the children hold every key and no key twice.
    std::size_t total = 0;
    for (const auto& row : rows) {
        total +=
            c.host(1)
                .group_node(row._group_id)
                ->with_state_machine([](host_types::state_machine_type& sm) { return sm.size(); });
    }
    if (total != workload_keys().size()) {
        std::string dump;
        for (node_id_t id = 1; id <= c.node_count(); ++id) {
            for (const auto& row : rows) {
                dump += "\n  node " + std::to_string(id) + " shard " +
                        std::to_string(row._group_id) + ": " + c.keys_of(id, row._group_id);
            }
        }
        BOOST_FAIL("children hold " << total << " keys, expected " << workload_keys().size()
                                    << dump);
    }

    if (auto problem = c.tiling_problem()) {
        BOOST_FAIL("split broke the tiling: " << *problem);
    }
    c.resume();
}

BOOST_AUTO_TEST_CASE(the_derived_child_keeps_the_parents_group_id_and_term,
                     *boost::unit_test::timeout(180)) {
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));
    for (const auto& k : workload_keys()) {
        BOOST_REQUIRE(c.put(k, "v"));
    }

    auto* leader = c.leader_of(1);
    const auto term_before = leader->group_node(1)->get_current_term();
    BOOST_REQUIRE(settle(leader->split_shard(1, {"echo"}, std::chrono::milliseconds{8000})) ==
                  nullptr);

    // The parent's group id survives on one child — the derived one — which is
    // why a split costs no leader election for that half.
    auto derived = leader->local_descriptor(1);
    BOOST_REQUIRE(derived.has_value());
    BOOST_CHECK_EQUAL(derived->_group_id, 1u);
    BOOST_CHECK(!derived->_range._start.has_value());
    BOOST_REQUIRE(derived->_range._end.has_value());
    BOOST_CHECK_EQUAL(*derived->_range._end, "echo");
    BOOST_CHECK_EQUAL(leader->group_node(1)->get_current_term(), term_before);
    BOOST_CHECK(leader->group_node(1)->is_leader());
}

BOOST_AUTO_TEST_CASE(a_non_derived_child_starts_from_a_snapshot_with_an_empty_log,
                     *boost::unit_test::timeout(180)) {
    // This is why a split moves no data. The child does not copy the parent's
    // log; it begins at the parent's apply index with an empty log and a
    // snapshot that IS its share of the parent's state.
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));
    for (const auto& k : workload_keys()) {
        BOOST_REQUIRE(c.put(k, "v"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    auto* leader = c.leader_of(1);
    const auto parent_log_length = leader->group_node(1)->debug_state().log.size();
    BOOST_REQUIRE_GT(parent_log_length, workload_keys().size());

    BOOST_REQUIRE(settle(leader->split_shard(1, {"echo"}, std::chrono::milliseconds{8000})) ==
                  nullptr);

    const auto rows = leader->shard_map_snapshot().descriptors();
    group_id_type child_id = 0;
    for (const auto& row : rows) {
        if (row._group_id != 1) {
            child_id = row._group_id;
        }
    }
    BOOST_REQUIRE_NE(child_id, 0u);

    BOOST_REQUIRE(
        c.await([&] { return leader->group_node(child_id) != nullptr; }, std::chrono::seconds{10}));
    c.quiesce();

    auto* child = leader->group_node(child_id);
    BOOST_REQUIRE(child != nullptr);
    const auto snapshot = child->debug_state();

    // The child's log does NOT contain the parent's entries. It may hold its
    // own no-op barrier once it elects, which is a handful of entries at most,
    // never the parent's whole history.
    BOOST_CHECK_LT(snapshot.log.size(), parent_log_length);
    BOOST_CHECK_GE(snapshot.last_applied, 1u);
    c.resume();
}

BOOST_AUTO_TEST_CASE(replaying_the_split_entry_changes_nothing, *boost::unit_test::timeout(180)) {
    // The idempotence check is what makes "children first, then the parent"
    // safe without a store that spans groups: a crash between the two replays
    // the entry and finds the children already present.
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));
    for (const auto& k : workload_keys()) {
        BOOST_REQUIRE(c.put(k, "v"));
    }

    auto* leader = c.leader_of(1);
    BOOST_REQUIRE(settle(leader->split_shard(1, {"echo"}, std::chrono::milliseconds{8000})) ==
                  nullptr);

    const auto rows_before = leader->shard_map_snapshot().descriptors();
    const auto splits_before = leader->applied_split_count();
    BOOST_REQUIRE_EQUAL(rows_before.size(), 2u);

    // Re-deliver the very same entry to the same replica: the replay a
    // crash-and-recover between "children written" and "parent advanced"
    // produces, without needing to corrupt a log to reach it.
    c.quiesce();
    const auto state_before = c.state_of(1, 1);
    {
        auto* n = leader->group_node(1);
        const auto snapshot = n->debug_state();
        std::optional<host_types::log_entry_type> split_entry;
        for (const auto& e : snapshot.log) {
            if (e.type() == kythira::entry_type::split) {
                split_entry = e;
            }
        }
        BOOST_REQUIRE(split_entry.has_value());
        BOOST_REQUIRE(leader->replay_admin_entry(1, *split_entry, split_entry->index()));
        // Twice, because "idempotent" is not "survives exactly one replay".
        BOOST_REQUIRE(leader->replay_admin_entry(1, *split_entry, split_entry->index()));
    }

    BOOST_CHECK_EQUAL(leader->applied_split_count(), splits_before);
    BOOST_CHECK_EQUAL(leader->shard_map_snapshot().size(), rows_before.size());
    BOOST_CHECK(c.state_of(1, 1) == state_before);
    BOOST_CHECK(!leader->shard_map_snapshot().check_tiling().has_value());
    c.resume();
}

// ── task 19: lazy replica creation ───────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_node_offline_through_a_split_learns_it_from_the_entry_itself,
                     *boost::unit_test::timeout(180)) {
    // The ORDINARY recovery, and worth pinning down because it is the reason
    // lazy creation is a fallback rather than the main path: a node that merely
    // missed the window still has the split entry waiting in its log, and
    // applying it creates the child exactly as it did everywhere else.
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));
    for (const auto& k : workload_keys()) {
        BOOST_REQUIRE(c.put(k, "v-" + k));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    auto* leader = c.leader_of(1);
    const auto leader_id = leader->node_id();
    const node_id_t absent = leader_id == 3 ? node_id_t{2} : node_id_t{3};
    c.fabric().kill(absent);
    c.pause_and_await(absent - 1);

    BOOST_REQUIRE(settle(leader->split_shard(1, {"echo"}, std::chrono::milliseconds{8000})) ==
                  nullptr);
    const auto rows = leader->shard_map_snapshot().descriptors();
    group_id_type child_id = 0;
    for (const auto& row : rows) {
        if (row._group_id != 1) {
            child_id = row._group_id;
        }
    }
    BOOST_REQUIRE_NE(child_id, 0u);
    BOOST_CHECK(c.host(absent).group_node(child_id) == nullptr);

    c.fabric().revive(absent);
    c.resume();

    BOOST_REQUIRE(c.await([&] { return c.host(absent).group_node(child_id) != nullptr; },
                          std::chrono::seconds{20}));
    // It came from the split entry, not from the lazy path.
    BOOST_CHECK_EQUAL(c.host(absent).lazily_created_replica_count(), 0u);
    BOOST_CHECK_GE(c.host(absent).applied_split_count(), 1u);

    BOOST_REQUIRE(
        c.await([&] { return c.state_of(absent, child_id) == c.state_of(leader_id, child_id); },
                std::chrono::seconds{20}));
}

BOOST_AUTO_TEST_CASE(a_node_that_lost_a_child_replica_reacquires_it_from_an_inbound_message,
                     *boost::unit_test::timeout(180)) {
    // Requirement 12.1: a node with no replica of a group it is a member of
    // acquires one from the first message that names it, and `InstallSnapshot`
    // populates it. This is the case the split entry cannot cover — the entry
    // is long gone from this node's log.
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));
    for (const auto& k : workload_keys()) {
        BOOST_REQUIRE(c.put(k, "v-" + k));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    auto* leader = c.leader_of(1);
    const auto leader_id = leader->node_id();
    BOOST_REQUIRE(settle(leader->split_shard(1, {"echo"}, std::chrono::milliseconds{8000})) ==
                  nullptr);

    const auto rows = leader->shard_map_snapshot().descriptors();
    descriptor_type child{};
    for (const auto& row : rows) {
        if (row._group_id != 1) {
            child = row;
        }
    }
    BOOST_REQUIRE_NE(child._group_id, 0u);

    const node_id_t victim = leader_id == 3 ? node_id_t{2} : node_id_t{3};
    BOOST_REQUIRE(c.await([&] { return c.host(victim).group_node(child._group_id) != nullptr; },
                          std::chrono::seconds{20}));

    // Lose the replica, then clear the tombstone: this node is still a member,
    // it simply has no copy. A tombstone would (correctly) keep it that way.
    BOOST_REQUIRE(c.host(victim).destroy_group(child._group_id, tombstone_reason::admin));
    // Past the horizon, not merely at it: the record is only collectable once
    // it is strictly older than the cutoff, which is what stops a tombstone
    // written this instant from being collected in the same instant.
    BOOST_REQUIRE(c.host(victim).gc_tombstones(std::chrono::system_clock::now() +
                                               std::chrono::hours{25}) >= 1u);
    BOOST_REQUIRE(!c.host(victim).is_tombstoned(child._group_id));
    BOOST_CHECK(c.host(victim).group_node(child._group_id) == nullptr);

    // The routing row goes back — that is what tells the lazy path this node is
    // a member. The leader's next AppendEntries does the rest.
    c.host(victim).learn_descriptors({child});

    BOOST_REQUIRE(c.await([&] { return c.host(victim).group_node(child._group_id) != nullptr; },
                          std::chrono::seconds{20}));
    BOOST_CHECK_GE(c.host(victim).lazily_created_replica_count(), 1u);

    // And it catches up — the one snapshot transfer design §9 prices this at.
    BOOST_REQUIRE(c.await(
        [&] {
            return !c.state_of(victim, child._group_id).empty() &&
                   c.state_of(victim, child._group_id) == c.state_of(leader_id, child._group_id);
        },
        std::chrono::seconds{25}));
}

BOOST_AUTO_TEST_CASE(a_message_naming_a_non_member_never_creates_a_replica,
                     *boost::unit_test::timeout(180)) {
    // Creating one would place a replica the cluster's configuration does not
    // know about, and no membership change would ever remove it.
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));

    // A routing row for a shard whose members exclude node 1.
    descriptor_type elsewhere{._group_id = 500,
                              ._range = kythira::unbounded_shard_range<key_type>(),
                              ._epoch = shard_epoch{._version = 9, ._conf_version = 0},
                              ._voters = {7, 8, 9},
                              ._learners = {},
                              ._leader_hint = std::nullopt};
    c.host(1).learn_descriptors({elsewhere});

    BOOST_CHECK(c.host(1).handle_unknown_group(500) == kythira::unknown_group_action::drop);
    BOOST_CHECK(c.host(1).group_node(500) == nullptr);
    BOOST_CHECK_EQUAL(c.host(1).lazily_created_replica_count(), 0u);
}

BOOST_AUTO_TEST_CASE(a_message_for_a_tombstoned_group_never_creates_a_replica,
                     *boost::unit_test::timeout(180)) {
    // Asserted through the REAL transport, not by calling the host's handler:
    // the tombstone check lives upstream in the transport precisely so that a
    // tombstoned group's message never reaches replica creation, and a test
    // that called the handler directly would be bypassing the thing under test.
    //
    // This is what stops a partitioned peer's stale AppendEntries from
    // resurrecting a merged-away group whose range someone else now owns.
    split_cluster c;
    BOOST_REQUIRE(c.await_leader(1, std::chrono::seconds{20}));

    const descriptor_type doomed{._group_id = 600,
                                 ._range = kythira::unbounded_shard_range<key_type>(),
                                 ._epoch = shard_epoch{._version = 1},
                                 ._voters = {1, 2, 3},
                                 ._learners = {},
                                 ._leader_hint = std::nullopt};
    c.host(1).create_group(doomed);
    BOOST_REQUIRE(c.host(1).destroy_group(600, tombstone_reason::merged_away));
    BOOST_REQUIRE(c.host(1).is_tombstoned(600));

    // The routing row is put back, so the ONLY thing standing between this
    // message and a new replica is the tombstone.
    c.host(1).learn_descriptors({doomed});
    const auto stale_before = c.host(1).stale_group_message_count();

    fabric_client stale_peer{c.fabric(), 2};
    auto f =
        stale_peer.send_append_entries(1,
                                       host_types::append_entries_request_type{._term = 1,
                                                                               ._leader_id = 2,
                                                                               ._prev_log_index = 0,
                                                                               ._prev_log_term = 0,
                                                                               ._entries = {},
                                                                               ._leader_commit = 0,
                                                                               ._group_id = 600},
                                       std::chrono::milliseconds{2000});
    BOOST_REQUIRE(f.wait(std::chrono::milliseconds{2000}));
    BOOST_CHECK(!std::move(f).get().success());

    BOOST_CHECK(c.host(1).group_node(600) == nullptr);
    BOOST_CHECK_GT(c.host(1).stale_group_message_count(), stale_before);
    BOOST_CHECK_EQUAL(c.host(1).lazily_created_replica_count(), 0u);
}

BOOST_AUTO_TEST_CASE(the_external_lookup_is_rate_limited, *boost::unit_test::timeout(180)) {
    // An unknown group id arriving at message rate must not become a
    // control-plane query at message rate: a partitioned peer retrying an
    // AppendEntries would otherwise be a denial of service against the
    // placement driver.
    message_fabric fabric{2};
    std::atomic<int> lookups{0};

    config_type cfg{
        .node_id = 1,
        .network_client = fabric_client{fabric, 1},
        .network_server = fabric_server{fabric, 1},
        .store_factory = [](const group_id_type&) { return host_types::persistence_engine_type{}; },
    };
    cfg.hibernation = hibernation_mode::off;
    cfg.executor_stripes = 1;
    cfg.unknown_group_lookup_interval = std::chrono::seconds{30};
    cfg.lookup_descriptor = [&lookups](const group_id_type&) -> std::optional<descriptor_type> {
        lookups.fetch_add(1);
        return std::nullopt;  // the authority does not know it either
    };

    host_type host{std::move(cfg)};
    host.start();
    for (int i = 0; i < 50; ++i) {
        BOOST_CHECK(host.handle_unknown_group(777) == kythira::unknown_group_action::drop);
    }
    BOOST_CHECK_EQUAL(lookups.load(), 1);
    BOOST_CHECK_EQUAL(host.descriptor_lookup_count(), 1u);

    // A different group id is a different budget.
    BOOST_CHECK(host.handle_unknown_group(778) == kythira::unknown_group_action::drop);
    BOOST_CHECK_EQUAL(lookups.load(), 2);
    host.stop();
}

BOOST_AUTO_TEST_CASE(lazy_creation_can_be_turned_off_entirely, *boost::unit_test::timeout(180)) {
    message_fabric fabric{2};
    config_type cfg{
        .node_id = 1,
        .network_client = fabric_client{fabric, 1},
        .network_server = fabric_server{fabric, 1},
        .store_factory = [](const group_id_type&) { return host_types::persistence_engine_type{}; },
    };
    cfg.hibernation = hibernation_mode::off;
    cfg.executor_stripes = 1;
    cfg.lazy_replica_creation = false;
    cfg.lookup_descriptor = [](const group_id_type&) -> std::optional<descriptor_type> {
        BOOST_FAIL("lookup must not be consulted when lazy creation is off");
        return std::nullopt;
    };

    host_type host{std::move(cfg)};
    host.start();
    BOOST_CHECK(host.handle_unknown_group(900) == kythira::unknown_group_action::drop);
    BOOST_CHECK_EQUAL(host.descriptor_lookup_count(), 0u);
    host.stop();
}

BOOST_AUTO_TEST_SUITE_END()
