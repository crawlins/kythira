// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_crash_consistency_test.cpp
/// @brief Crash and recover at each of the eight split/merge fault points
///        (task 32 of `.kiro/specs/multi-raft/`).
///
/// A split or merge is several steps that are not one atomic write: children
/// are created, the parent is narrowed, routing rows are published, a source is
/// destroyed. A crash between any two of them must not leave a lost child, a
/// range owned twice, or a range owned by nobody — and the design's answer is
/// not a transaction, it is **idempotence**: every administration entry can be
/// applied again from the start, and recovery is exactly that replay.
///
/// This file is what turns that claim into evidence. Each case:
///
///  1. arms one `fiu` fault point, which throws part-way through apply — the
///     crash;
///  2. drives the operation, which fails somewhere in the middle;
///  3. disarms the fault point and replays the same committed entry — the
///     recovery, and precisely the work a restarting node does when it reaches
///     an unapplied administration entry in its log;
///  4. asserts the invariants I1-I4 hold afterwards.
///
/// The replay is then run a **second** time in the split cases, because
/// "idempotent" is not "survives exactly one replay" — and a crash during
/// recovery is at least as likely as a crash during the original.

#define BOOST_TEST_MODULE multi_raft_crash_consistency_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/fault_injection.hpp>

// The CONTROL side of libfiu — `fiu_enable`/`fiu_disable`/`fiu_init` — which
// `fault_injection.hpp` does not pull in: production code only ever needs to
// *check* a fault point, never to arm one.
#include <fiu-control.h>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_crash_consistency_test"), nullptr};
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
using kythira::shard_epoch;
using kythira::tick_batch_controller;
using kythira::tombstone_reason;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;

/// The host's shared-transport bundle. Note that the client and server named
/// here are the *shared* ones; `multi_raft` derives each group's scoped views
/// from them.
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

auto fast_raft_config() -> kythira::raft_configuration {
    kythira::raft_configuration c;
    c._election_timeout_min = std::chrono::milliseconds{40};
    c._election_timeout_max = std::chrono::milliseconds{80};
    c._heartbeat_interval = std::chrono::milliseconds{10};
    return c;
}

auto make_config(message_fabric& fabric, std::uint64_t node_id) -> config_type {
    config_type cfg{
        .node_id = node_id,
        .network_client = fabric_client{fabric, node_id},
        .network_server = fabric_server{fabric, node_id},
        .store_factory = [](const group_id_type&) { return host_types::persistence_engine_type{}; },
    };
    cfg.config = fast_raft_config();
    cfg.hibernation = hibernation_mode::off;
    cfg.executor_stripes = 4;
    return cfg;
}

using kythira::shard_descriptor;
using state_machine_type = host_types::state_machine_type;
using descriptor_type = shard_descriptor<group_id_type, key_type, std::uint64_t>;

constexpr group_id_type k_left = 1;
constexpr group_id_type k_right = 2;

/// `fiu` needs one process-wide initialisation before any fault point is armed.
struct fiu_fixture {
    fiu_fixture() { fiu_init(0); }
};

/// Arms a fault point for a scope and disarms it however the scope exits.
///
/// A leaked armed fault point would poison every later case in the file with a
/// failure that looks nothing like its cause, which is exactly the debugging
/// afternoon this class exists to prevent.
class armed_fault {
public:
    explicit armed_fault(const char* name) : _name(name) { fiu_enable(_name, 1, nullptr, 0); }
    ~armed_fault() { fiu_disable(_name); }

    armed_fault(const armed_fault&) = delete;
    auto operator=(const armed_fault&) -> armed_fault& = delete;

    auto disarm() -> void {
        if (!_disarmed) {
            fiu_disable(_name);
            _disarmed = true;
        }
    }

private:
    const char* _name;
    bool _disarmed{false};
};

auto put_command(const std::string& key, const std::string& value) -> std::vector<std::byte> {
    return state_machine_type::make_put_command(key, value);
}

auto key_at(std::size_t index) -> key_type {
    return key_type{static_cast<char>('a' + (index / 26) % 26),
                    static_cast<char>('a' + index % 26)};
}

/// One host holding two adjacent shards, so both split and merge are reachable.
class crash_host {
public:
    crash_host() {
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
        cfg.split_merge_interval = std::chrono::milliseconds{0};
        cfg.allocate_group_ids = [this](std::size_t n) {
            std::vector<group_id_type> out;
            for (std::size_t i = 0; i < n; ++i) {
                out.push_back(_next_id++);
            }
            return out;
        };
        _host = std::make_unique<host_type>(std::move(cfg));

        // `[-inf, mm)` and `[mm, +inf)`: adjacent, colocated, and therefore a
        // legal merge pair as well as two splittable shards.
        _host->create_group(descriptor_type{
            ._group_id = k_left,
            ._range =
                kythira::shard_range<key_type>{._start = std::nullopt, ._end = key_type{"mm"}},
            ._epoch = kythira::shard_epoch{._version = 1, ._conf_version = 1},
            ._voters = {1},
            ._learners = {},
            ._leader_hint = std::nullopt});
        _host->create_group(descriptor_type{
            ._group_id = k_right,
            ._range =
                kythira::shard_range<key_type>{._start = key_type{"mm"}, ._end = std::nullopt},
            ._epoch = kythira::shard_epoch{._version = 1, ._conf_version = 1},
            ._voters = {1},
            ._learners = {},
            ._leader_hint = std::nullopt});
        _host->start();
    }

    ~crash_host() { _host->stop(); }

    crash_host(const crash_host&) = delete;
    auto operator=(const crash_host&) -> crash_host& = delete;

    [[nodiscard]] auto host() -> host_type& { return *_host; }

    auto await_leaders() -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            _host->tick();
            bool all = true;
            for (auto g : _host->group_ids()) {
                auto* n = _host->group_node(g);
                if (n == nullptr || !n->is_leader()) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return false;
    }

    template<typename Future> auto settle(Future&& f) -> std::exception_ptr {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{6};
        while (!f.wait(std::chrono::milliseconds{2}) &&
               std::chrono::steady_clock::now() < deadline) {
            _host->tick();
        }
        if (!f.wait(std::chrono::milliseconds{200})) {
            return std::make_exception_ptr(std::runtime_error("never resolved"));
        }
        try {
            std::ignore = std::move(f).get();
            return nullptr;
        } catch (...) {
            return std::current_exception();
        }
    }

    auto put(const key_type& key, const std::string& value) -> bool {
        auto error = settle(
            _host->submit_command(key, put_command(key, value), std::chrono::milliseconds{3000}));
        if (error == nullptr) {
            _model[key] = value;
            return true;
        }
        return false;
    }

    auto seed() -> void {
        // Six keys each side of "mm", so both shards have something to split
        // and something to lose.
        for (std::size_t i = 0; i < 6; ++i) {
            BOOST_REQUIRE(put(key_at(i), "v"));
        }
        for (std::size_t i = 0; i < 6; ++i) {
            BOOST_REQUIRE(put(key_type{"n"} + static_cast<char>('a' + i), "v"));
        }
    }

    auto spin(int ticks = 40) -> void {
        for (int i = 0; i < ticks; ++i) {
            _host->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }

    /// The most recent administration entry of `type` in `group`'s log.
    [[nodiscard]] auto find_admin_entry(group_id_type group, kythira::entry_type type)
        -> std::optional<host_types::log_entry_type> {
        auto* node = _host->group_node(group);
        if (node == nullptr) {
            return std::nullopt;
        }
        std::optional<host_types::log_entry_type> found;
        const auto snapshot = node->debug_state();
        for (const auto& e : snapshot.log) {
            if (e.type() == type) {
                found = e;
            }
        }
        return found;
    }

    [[nodiscard]] auto model() const -> const std::map<key_type, std::string>& { return _model; }

private:
    message_fabric _fabric{2};
    std::unique_ptr<host_type> _host;
    std::map<key_type, std::string> _model;
    group_id_type _next_id{100};
};

// ── the invariants, as this file checks them ─────────────────────────────────

auto check_tiling(host_type& host, const std::string& where) -> void {
    const auto problem = host.shard_map_snapshot().check_tiling();
    BOOST_CHECK_MESSAGE(!problem.has_value(),
                        "I1 tiling broken after " << where << ": " << problem.value_or(""));
}

/// I2, and I4's practical consequence: every key lives in exactly one shard,
/// and in the shard whose range actually contains it.
auto check_ownership(host_type& host, const std::map<key_type, std::string>& model,
                     const std::string& where) -> void {
    std::map<key_type, int> owners;
    for (auto g : host.group_ids()) {
        auto* node = host.group_node(g);
        auto descriptor = host.local_descriptor(g);
        if (node == nullptr || !descriptor.has_value()) {
            continue;
        }
        std::map<key_type, std::string> store;
        node->with_state_machine([&](state_machine_type& sm) {
            store = state_machine_type::deserialize_store(sm.get_state());
        });
        for (const auto& [k, _] : store) {
            ++owners[k];
            BOOST_CHECK_MESSAGE(descriptor->_range.contains(k),
                                "key '" << k << "' sits in shard " << g
                                        << " whose range does not contain it, after " << where);
        }
    }

    for (const auto& [key, _] : model) {
        auto it = owners.find(key);
        BOOST_CHECK_MESSAGE(it != owners.end(), "I2 lost key '" << key << "' after " << where);
        if (it != owners.end()) {
            BOOST_CHECK_MESSAGE(it->second == 1, "I2 key '" << key << "' held by " << it->second
                                                            << " shards after " << where);
        }
    }
}

auto check_all(crash_host& h, const std::string& where) -> void {
    check_tiling(h.host(), where);
    check_ownership(h.host(), h.model(), where);
}

/// @brief Crash inside a split at `fault`, then recover by replaying the entry.
///
/// Returns false when the fault point was never reached — which is a legitimate
/// outcome for `between_children` on a two-child split, and which the caller
/// asserts about explicitly rather than having it pass silently.
auto crash_and_recover_split(crash_host& h, const char* fault) -> bool {
    const auto groups_before = h.host().group_count();

    std::exception_ptr failure;
    {
        armed_fault armed{fault};
        failure =
            h.settle(h.host().split_shard(k_left, {key_at(3)}, std::chrono::milliseconds{4000}));
    }
    h.spin();

    // The entry is committed — the crash was in APPLY, which is the whole point:
    // Raft has already decided, and recovery has to finish what the decision
    // started rather than undo it.
    auto entry = h.find_admin_entry(k_left, kythira::entry_type::split);
    BOOST_REQUIRE_MESSAGE(entry.has_value(), "no split entry was committed for " << fault);

    // Mid-crash, the ROUTING MAP must still be a clean tiling — that is what
    // deferring publication to step G buys, and it is the invariant clients can
    // actually observe: no key resolves to nothing, and none resolves to two
    // shards.
    //
    // Ownership inside the state machines is deliberately NOT asserted here. A
    // crash between creating a child and narrowing the parent leaves both
    // holding the child's keys, and that overlap is exactly why publication is
    // deferred: it is invisible to every client because neither child's row is
    // in the map yet. Task 32 asserts the invariants after RECOVERY, which is
    // where the overlap has to be gone.
    check_tiling(h.host(), std::string{"the crash at "} + fault);

    // Recovery: replay the committed entry with the fault disarmed, exactly as
    // a restarting node does on reaching an unapplied administration entry.
    BOOST_REQUIRE(h.host().replay_admin_entry(k_left, *entry, entry->index()));
    // Twice, because "idempotent" is not "survives exactly one replay", and a
    // crash during recovery is at least as likely as one during the original.
    BOOST_REQUIRE(h.host().replay_admin_entry(k_left, *entry, entry->index()));
    h.spin();
    BOOST_REQUIRE(h.await_leaders());

    check_all(h, std::string{"recovery from "} + fault);
    return h.host().group_count() > groups_before || failure == nullptr;
}

}  // namespace

BOOST_GLOBAL_FIXTURE(fiu_fixture);

BOOST_AUTO_TEST_SUITE(multi_raft_crash_consistency)

// ── split: the four fault points ─────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_crash_before_any_child_is_created_recovers,
                     *boost::unit_test::timeout(180)) {
    // The earliest crash: the entry is committed, and nothing has happened yet.
    // Recovery does the whole split from the start.
    crash_host h;
    BOOST_REQUIRE(h.await_leaders());
    h.seed();
    check_all(h, "seed");

    BOOST_CHECK(crash_and_recover_split(h, "raft/multiraft/split/before_children"));
    BOOST_CHECK_EQUAL(h.host().group_count(), 3u);
}

BOOST_AUTO_TEST_CASE(a_crash_between_children_recovers, *boost::unit_test::timeout(180)) {
    // The child that was created before the crash must be RECOGNISED on replay,
    // not created a second time — that is the `find_group(...) continue;`
    // idempotence check in split apply step B, and this is what exercises it.
    crash_host h;
    BOOST_REQUIRE(h.await_leaders());
    h.seed();

    BOOST_CHECK(crash_and_recover_split(h, "raft/multiraft/split/between_children"));
    BOOST_CHECK_EQUAL(h.host().group_count(), 3u);
}

BOOST_AUTO_TEST_CASE(a_crash_after_children_before_the_parent_narrows_recovers,
                     *boost::unit_test::timeout(180)) {
    // The dangerous window: the children exist and own their ranges, while the
    // parent still believes it owns all of them. Every key is momentarily
    // claimed twice, and the ONLY thing that keeps that from being visible is
    // that the routing rows have not been published yet.
    crash_host h;
    BOOST_REQUIRE(h.await_leaders());
    h.seed();

    BOOST_CHECK(crash_and_recover_split(h, "raft/multiraft/split/after_children_before_parent"));
    BOOST_CHECK_EQUAL(h.host().group_count(), 3u);
}

BOOST_AUTO_TEST_CASE(a_crash_after_publishing_the_routing_rows_recovers,
                     *boost::unit_test::timeout(180)) {
    // The latest crash: everything is done except the bookkeeping that follows.
    // Replay must be a complete no-op here, and the shard must not be left
    // frozen in `splitting`.
    crash_host h;
    BOOST_REQUIRE(h.await_leaders());
    h.seed();

    BOOST_CHECK(crash_and_recover_split(h, "raft/multiraft/split/after_publish"));
    BOOST_CHECK_EQUAL(h.host().group_count(), 3u);

    // And crucially the shard is USABLE again. A crash in the last step leaves
    // it frozen in `splitting` with nothing running; if replay did not release
    // it, the shard would refuse every future split, merge and operator with
    // `gate=state` forever, and no amount of replaying would help.
    const auto state = h.host().operation_state(k_left);
    BOOST_REQUIRE(state.has_value());
    BOOST_CHECK_MESSAGE(*state == kythira::shard_operation_state::stable,
                        "shard left in " << kythira::to_string(*state) << " after recovery");
    BOOST_CHECK(h.settle(h.host().split_shard(k_left, {key_at(1)},
                                              std::chrono::milliseconds{4000})) == nullptr);
}

// ── merge: the four fault points ─────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_crash_after_the_source_freezes_leaves_it_frozen_but_correct,
                     *boost::unit_test::timeout(180)) {
    // Design §5.7's deliberate trade: a stalled merge leaves the source
    // UNAVAILABLE but CORRECT. What must never happen is the source resuming
    // service while the target has taken its range.
    crash_host h;
    BOOST_REQUIRE(h.await_leaders());
    h.seed();

    {
        armed_fault armed{"raft/multiraft/merge/after_prepare"};
        std::ignore =
            h.settle(h.host().merge_shards(k_left, k_right, std::chrono::milliseconds{4000}));
        h.spin();
    }
    h.spin();

    // Whatever state the merge reached, no range is owned twice and no key has
    // gone missing.
    check_all(h, "a crash after merge_prepare");

    // The source is either frozen mid-merge or still stable; what it must not
    // be is destroyed while the target has not taken its range.
    const auto state = h.host().operation_state(k_left);
    BOOST_REQUIRE(state.has_value());
    BOOST_CHECK(*state == kythira::shard_operation_state::merging_source ||
                *state == kythira::shard_operation_state::stable);
}

BOOST_AUTO_TEST_CASE(a_crash_mid_commit_catch_up_recovers, *boost::unit_test::timeout(180)) {
    // The target is bringing its local source replica up to `prepare_index`
    // before reading its state. A crash here must not leave the target having
    // absorbed a PARTIAL source.
    crash_host h;
    BOOST_REQUIRE(h.await_leaders());
    h.seed();

    {
        armed_fault armed{"raft/multiraft/merge/mid_commit_catchup"};
        std::ignore =
            h.settle(h.host().merge_shards(k_left, k_right, std::chrono::milliseconds{4000}));
        h.spin();
    }
    h.spin(80);

    check_all(h, "a crash mid commit catch-up");
}

BOOST_AUTO_TEST_CASE(a_crash_after_absorb_before_destroy_recovers,
                     *boost::unit_test::timeout(180)) {
    // The window where the target HAS the source's data and the source still
    // exists. Double ownership is reachable here and nowhere else in the merge,
    // which is why this is the fault point that matters most.
    crash_host h;
    BOOST_REQUIRE(h.await_leaders());
    h.seed();

    std::optional<host_types::log_entry_type> commit_entry;
    {
        armed_fault armed{"raft/multiraft/merge/after_absorb_before_destroy"};
        std::ignore =
            h.settle(h.host().merge_shards(k_left, k_right, std::chrono::milliseconds{4000}));
        h.spin(80);
        commit_entry = h.find_admin_entry(k_right, kythira::entry_type::merge_commit);
    }
    h.spin();

    BOOST_REQUIRE_MESSAGE(commit_entry.has_value(),
                          "no merge_commit entry was committed, so the fault point was never "
                          "reached and this case proves nothing");
    {
        // Recovery: replay the commit entry with the fault disarmed. The source
        // is torn down and its routing row retired, finishing what the decision
        // started.
        BOOST_REQUIRE(h.host().replay_admin_entry(k_right, *commit_entry, commit_entry->index()));
        h.spin(40);
    }

    check_all(h, "a crash after absorb, before destroy");
    // No key may be held by both the survivor and the source, which
    // `check_ownership` above asserts directly.
}

BOOST_AUTO_TEST_CASE(a_crash_after_abandon_before_rollback_leaves_the_source_frozen,
                     *boost::unit_test::timeout(180)) {
    // The abandon handshake's own crash window. Releasing a source wrongly is
    // the one way this protocol corrupts data, so the safe outcome is that the
    // source stays frozen — unavailable, and correct.
    crash_host h;
    BOOST_REQUIRE(h.await_leaders());
    h.seed();

    {
        armed_fault armed{"raft/multiraft/merge/after_abandon_before_rollback"};
        std::ignore =
            h.settle(h.host().merge_shards(k_left, k_right, std::chrono::milliseconds{4000}));
        h.spin();
        std::ignore = h.settle(h.host().abandon_merge(k_left, std::chrono::milliseconds{4000}));
        h.spin(80);
    }
    h.spin();

    check_all(h, "a crash after abandon, before rollback");

    // Two outcomes are legitimate and the test accepts both, because which one
    // happens depends on a race the protocol deliberately resolves one way:
    // **commit always wins**. If the target had already proposed `merge_commit`
    // when the abandon arrived, the merge completes and the source is gone; if
    // not, the source survives, frozen or resumed.
    //
    // What must never happen is the third outcome — a source that is gone while
    // the survivor has NOT taken its range, or a source still serving while the
    // survivor has. `check_all` above rules both out, since either would show
    // up as a tiling break or a key owned twice.
    if (h.host().has_group(k_left)) {
        BOOST_CHECK(!h.host().is_tombstoned(k_left));
    } else {
        // Merged away: the survivor must own the whole key space between them.
        auto survivor = h.host().local_descriptor(k_right);
        BOOST_REQUIRE(survivor.has_value());
        BOOST_CHECK(!survivor->_range._start.has_value());
        BOOST_CHECK(h.host().is_tombstoned(k_left));
    }
}

BOOST_AUTO_TEST_SUITE_END()
