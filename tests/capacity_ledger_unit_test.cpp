// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file capacity_ledger_unit_test.cpp
/// @brief The provisioning ledger (task 5 of
///        `.kiro/specs/elastic-shard-capacity/`): the intent state machine,
///        record-before-act, the file ledger's survival of a crash, the
///        replicated ledger's tickets, and retention compaction.

#define BOOST_TEST_MODULE capacity_ledger_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/capacity_ledger.hpp>
#include <raft/future_default.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <tuple>
#include <unistd.h>
#include <string>
#include <utility>
#include <vector>

namespace {

using kythira::capacity_intent_kind;
using kythira::capacity_intent_state;
using kythira::capacity_ledger_write_status;
using kythira::capacity_reason;

using node_id_t = std::uint64_t;
using pg_t = std::string;
using intent_t = kythira::capacity_intent<node_id_t, pg_t>;
using patch_t = kythira::capacity_intent_patch<node_id_t, pg_t>;
using memory_ledger_t = kythira::memory_capacity_ledger<node_id_t, pg_t>;
using file_ledger_t = kythira::file_capacity_ledger<node_id_t, pg_t>;
using core_t = kythira::capacity_ledger_core<node_id_t, pg_t>;
using sm_t = kythira::capacity_ledger_state_machine<node_id_t, pg_t>;
using time_point = std::chrono::system_clock::time_point;

using namespace std::chrono_literals;

const auto k_t0 = time_point{} + std::chrono::hours{24 * 365 * 50};

auto all_states() -> std::vector<capacity_intent_state> {
    std::vector<capacity_intent_state> out;
    for (int s = 0; s <= static_cast<int>(capacity_intent_state::failed); ++s) {
        out.push_back(static_cast<capacity_intent_state>(s));
    }
    return out;
}

auto make_intent(std::string key, capacity_intent_kind kind = capacity_intent_kind::scale_out)
    -> intent_t {
    intent_t i;
    i._key = std::move(key);
    i._kind = kind;
    i._state = kind == capacity_intent_kind::scale_out ? capacity_intent_state::requested
                                                       : capacity_intent_state::draining;
    i._reason = capacity_reason::split_pressure;
    i._evidence._signals.push_back({._name = "shards_per_node", ._value = 250, ._threshold = 200});
    i._evidence._capacity_refusals = 1;
    i._evidence._projection = kythira::capacity_projection{._current_shards_per_node = 150,
                                                           ._split_rate_per_minute = 20,
                                                           ._rate_window = 15min,
                                                           ._horizon_minutes = 30,
                                                           ._live_node_count = 3,
                                                           ._projected_shards_per_node = 350};
    i._group = "zone-a";
    i._fencing_token = 7;
    i._observed_cluster_size = 3;
    i._created_at = k_t0;
    i._updated_at = k_t0;
    i._deadline = k_t0 + 10min;
    return i;
}

/// A scratch file removed on scope exit.
struct temp_path {
    std::filesystem::path _p;
    temp_path() {
        _p = std::filesystem::temp_directory_path() /
             ("capacity_ledger_" + std::to_string(::getpid()) + "_" +
              std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".json");
        std::filesystem::remove(_p);
    }
    ~temp_path() {
        std::error_code ec;
        std::filesystem::remove(_p, ec);
        std::filesystem::remove(std::filesystem::path{_p.string() + ".tmp"}, ec);
    }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(capacity_ledger_unit)

// ── the transition table ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_scale_out_transition_table_is_exactly_the_design) {
    using S = capacity_intent_state;
    const std::set<std::pair<S, S>> allowed{
        {S::decided, S::requested},        {S::decided, S::refused},
        {S::requested, S::provisioning},   {S::requested, S::abandoned},
        {S::requested, S::admitting},      {S::requested, S::completed},
        {S::requested, S::reaping},        {S::requested, S::failed},
        {S::provisioning, S::provisioned}, {S::provisioning, S::admitting},
        {S::provisioning, S::reaping},     {S::provisioning, S::failed},
        {S::provisioning, S::completed},   {S::provisioned, S::admitting},
        {S::provisioned, S::reaping},      {S::provisioned, S::completed},
        {S::admitting, S::completed},      {S::admitting, S::abandoned},
        {S::reaping, S::orphaned},         {S::reaping, S::failed},
    };
    for (const auto from : all_states()) {
        for (const auto to : all_states()) {
            BOOST_TEST_CONTEXT(kythira::to_string(from) << " -> " << kythira::to_string(to)) {
                BOOST_CHECK_EQUAL(kythira::capacity_intent_transition_allowed(
                                      capacity_intent_kind::scale_out, from, to),
                                  allowed.contains({from, to}));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(the_scale_in_transition_table_is_exactly_the_design) {
    using S = capacity_intent_state;
    const std::set<std::pair<S, S>> allowed{
        {S::decided, S::draining},          {S::decided, S::refused},
        {S::draining, S::drained},          {S::draining, S::abandoned},
        {S::drained, S::decommissioning},   {S::drained, S::abandoned},
        {S::decommissioning, S::completed}, {S::decommissioning, S::failed},
    };
    for (const auto from : all_states()) {
        for (const auto to : all_states()) {
            BOOST_TEST_CONTEXT(kythira::to_string(from) << " -> " << kythira::to_string(to)) {
                BOOST_CHECK_EQUAL(kythira::capacity_intent_transition_allowed(
                                      capacity_intent_kind::scale_in, from, to),
                                  allowed.contains({from, to}));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(every_deadline_expiry_has_a_legal_next_state) {
    // Design §4 rule 3: nothing waits forever. Each non-terminal state's expiry
    // target must be terminal-or-progressing AND permitted by the table for the
    // kind that can be in that state.
    using S = capacity_intent_state;
    const std::vector<std::pair<capacity_intent_kind, S>> open{
        {capacity_intent_kind::scale_out, S::decided},
        {capacity_intent_kind::scale_out, S::requested},
        {capacity_intent_kind::scale_out, S::provisioning},
        {capacity_intent_kind::scale_out, S::provisioned},
        {capacity_intent_kind::scale_out, S::admitting},
        {capacity_intent_kind::scale_out, S::reaping},
        {capacity_intent_kind::scale_in, S::draining},
        {capacity_intent_kind::scale_in, S::drained},
        {capacity_intent_kind::scale_in, S::decommissioning},
    };
    for (const auto& [kind, state] : open) {
        BOOST_TEST_CONTEXT(kythira::to_string(state)) {
            const auto next = kythira::capacity_intent_on_deadline(state);
            BOOST_CHECK(next != state);
            BOOST_CHECK(kythira::capacity_intent_transition_allowed(kind, state, next));
        }
    }
    // The specific choices the design names.
    BOOST_CHECK(kythira::capacity_intent_on_deadline(S::provisioning) == S::reaping);
    BOOST_CHECK(kythira::capacity_intent_on_deadline(S::provisioned) == S::reaping);
    BOOST_CHECK(kythira::capacity_intent_on_deadline(S::draining) == S::abandoned);
    // Terminal states stay put.
    for (const auto s : {S::completed, S::refused, S::abandoned, S::orphaned, S::failed}) {
        BOOST_CHECK(kythira::is_terminal(s));
        BOOST_CHECK(kythira::capacity_intent_on_deadline(s) == s);
    }
}

// ── record before act ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_token_exists_only_for_a_committed_record) {
    memory_ledger_t l;
    const auto rec = l.record(make_intent("k1"));
    BOOST_CHECK(l.status(rec) == capacity_ledger_write_status::committed);
    const auto token = l.token(rec);
    BOOST_REQUIRE(token.has_value());
    BOOST_CHECK_EQUAL(token->key(), "k1");
    BOOST_CHECK_EQUAL(token->fencing_token(), 7U);

    // A transition ticket is committed but mints no token: only a record
    // proves an intent was written down.
    const auto tr = l.transition("k1", capacity_intent_state::provisioning, {}, k_t0 + 1s);
    BOOST_CHECK(l.status(tr) == capacity_ledger_write_status::committed);
    BOOST_CHECK(!l.token(tr).has_value());

    // A duplicate key is rejected and mints nothing.
    const auto dup = l.record(make_intent("k1"));
    BOOST_CHECK(l.status(dup) == capacity_ledger_write_status::rejected);
    BOOST_CHECK(!l.token(dup).has_value());

    // So is an intent recorded in anything but its first state.
    auto late = make_intent("k2");
    late._state = capacity_intent_state::provisioning;
    BOOST_CHECK(l.status(l.record(late)) == capacity_ledger_write_status::rejected);

    // Forgotten tickets read as rejected, never as committed.
    l.forget(rec);
    BOOST_CHECK(l.status(rec) == capacity_ledger_write_status::rejected);
    BOOST_CHECK(!l.token(rec).has_value());
}

BOOST_AUTO_TEST_CASE(an_illegal_transition_is_rejected_and_changes_nothing) {
    memory_ledger_t l;
    std::ignore = l.record(make_intent("k"));
    const auto before = *l.find("k");
    const auto t = l.transition("k", capacity_intent_state::orphaned, {._note = "no"}, k_t0 + 1s);
    BOOST_CHECK(l.status(t) == capacity_ledger_write_status::rejected);
    BOOST_CHECK(*l.find("k") == before);
    BOOST_CHECK(l.status(l.transition("missing", capacity_intent_state::failed, {}, k_t0)) ==
                capacity_ledger_write_status::rejected);
}

BOOST_AUTO_TEST_CASE(a_patch_lands_with_its_transition) {
    memory_ledger_t l;
    std::ignore = l.record(make_intent("k"));
    patch_t p;
    p._node = 1001;
    p._address = "mock-1001:7000";
    p._deadline = k_t0 + 15min;
    p._note = "provider returned";
    p._attempt =
        kythira::capacity_placement_attempt<pg_t>{._group = "zone-b", ._outcome = "stock-out"};
    BOOST_REQUIRE(l.status(l.transition("k", capacity_intent_state::provisioning, p, k_t0 + 1s)) ==
                  capacity_ledger_write_status::committed);
    const auto i = *l.find("k");
    BOOST_CHECK(i._state == capacity_intent_state::provisioning);
    BOOST_CHECK(i._node == std::optional<node_id_t>{1001});
    BOOST_CHECK_EQUAL(i._address, "mock-1001:7000");
    BOOST_CHECK(i._deadline == k_t0 + 15min);
    BOOST_CHECK(i._updated_at == k_t0 + 1s);
    BOOST_REQUIRE_EQUAL(i._attempts.size(), 1U);
    BOOST_CHECK_EQUAL(i._attempts[0]._outcome, "stock-out");
}

// ── the file ledger ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_crash_between_record_and_the_provider_call_leaves_a_requested_intent) {
    // The crash-injection case of task 5: the process dies after the record is
    // durable and before `provision_node` is called. The successor must find
    // the intent, in `requested`, with everything reconciliation needs.
    temp_path tmp;
    const auto original = make_intent("intent-crash");
    {
        file_ledger_t l{tmp._p};
        const auto t = l.record(original);
        BOOST_REQUIRE(l.status(t) == capacity_ledger_write_status::committed);
        BOOST_REQUIRE(l.token(t).has_value());
        // ...and here the process dies: no transition, no provider call.
    }
    file_ledger_t successor{tmp._p};
    const auto found = successor.find("intent-crash");
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK(*found == original);
    BOOST_CHECK(found->_state == capacity_intent_state::requested);

    // Reconciliation resolves it — here, "not found at the provider" — and
    // that resolution is itself durable.
    BOOST_REQUIRE(
        successor.status(successor.transition("intent-crash", capacity_intent_state::failed,
                                              {._note = "reconcile: no machine"}, k_t0 + 1min)) ==
        capacity_ledger_write_status::committed);
    file_ledger_t third{tmp._p};
    BOOST_CHECK(third.find("intent-crash")->_state == capacity_intent_state::failed);
}

BOOST_AUTO_TEST_CASE(an_unreadable_ledger_file_refuses_to_open) {
    // Starting empty would forget every intent the ledger exists to remember.
    temp_path tmp;
    {
        std::ofstream out(tmp._p);
        out << "{ not json";
    }
    BOOST_CHECK_THROW(file_ledger_t{tmp._p}, std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_write_that_cannot_persist_is_rejected_and_rolled_back) {
    // The `.tmp` sibling is a directory, so the write cannot land. The ledger
    // must say so — and must not hand out a token for an intent that is not
    // on disk.
    temp_path tmp;
    const auto blocker = std::filesystem::path{tmp._p.string() + ".tmp"};
    std::filesystem::create_directories(blocker);
    {
        file_ledger_t l{tmp._p};
        const auto t = l.record(make_intent("k"));
        BOOST_CHECK(l.status(t) == capacity_ledger_write_status::rejected);
        BOOST_CHECK(!l.token(t).has_value());
        BOOST_CHECK(!l.find("k").has_value());
    }
    std::filesystem::remove_all(blocker);
}

// ── compaction ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(retention_keeps_the_ledger_bounded_over_a_long_run) {
    // A synthetic month: one intent per hour, each completing an hour later,
    // compaction every hour with a one-week retention. The ledger must settle
    // at about a week's worth of intents, never grow with the run, and never
    // drop an open intent however old.
    memory_ledger_t l;
    const auto retention = std::chrono::hours{24 * 7};

    auto stuck = make_intent("stuck");
    BOOST_REQUIRE(l.status(l.record(stuck)) == capacity_ledger_write_status::committed);

    std::size_t peak = 0;
    for (int h = 0; h < 24 * 30; ++h) {
        const auto now = k_t0 + std::chrono::hours{h};
        auto i = make_intent("i" + std::to_string(h));
        i._created_at = now;
        i._updated_at = now;
        BOOST_REQUIRE(l.status(l.record(i)) == capacity_ledger_write_status::committed);
        if (h > 0) {
            BOOST_REQUIRE(l.status(l.transition("i" + std::to_string(h - 1),
                                                capacity_intent_state::failed, {}, now)) ==
                          capacity_ledger_write_status::committed);
        }
        BOOST_REQUIRE(l.status(l.compact(now - retention)) ==
                      capacity_ledger_write_status::committed);
        peak = std::max(peak, l.size());
    }
    BOOST_CHECK_LE(peak, 24U * 7U + 3U);
    BOOST_CHECK_GE(l.size(), 24U * 7U - 2U);
    // A month-old open intent survives every compaction.
    BOOST_CHECK(l.find("stuck").has_value());
}

// ── the replicated ledger ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_state_machine_applies_commands_and_round_trips_its_snapshot) {
    sm_t sm;
    const auto r = sm.apply(core_t::encode_record(make_intent("a")), 1);
    BOOST_REQUIRE_EQUAL(r.size(), 1U);
    BOOST_CHECK(r[0] == std::byte{1});
    // Refused commands answer 0 and change nothing.
    BOOST_CHECK(sm.apply(core_t::encode_record(make_intent("a")), 2)[0] == std::byte{0});
    BOOST_CHECK(sm.apply(core_t::encode_transition("a", capacity_intent_state::orphaned, {}, k_t0),
                         3)[0] == std::byte{0});
    // Garbage is refused, never thrown: a replica must not crash on an entry.
    const std::vector<std::byte> junk{std::byte{'x'}, std::byte{'{'}};
    BOOST_CHECK(sm.apply(junk, 4)[0] == std::byte{0});
    BOOST_CHECK(
        sm.apply(core_t::encode_transition(
                     "a", capacity_intent_state::provisioning,
                     {._node = 9,
                      ._attempt = kythira::capacity_placement_attempt<pg_t>{._group = "zone-b",
                                                                            ._outcome = "quota"}},
                     k_t0 + 1s),
                 5)[0] == std::byte{1});

    sm_t restored;
    restored.restore_from_snapshot(sm.get_state(), 5);
    BOOST_CHECK(restored.intents() == sm.intents());
    BOOST_CHECK(restored.find("a")->_node == std::optional<node_id_t>{9});
    BOOST_CHECK_EQUAL(restored.last_applied(), 5U);

    BOOST_CHECK(sm.apply(core_t::encode_compact(k_t0 + 1h), 6)[0] == std::byte{1});
    BOOST_CHECK(sm.find("a").has_value());  // open, so kept
}

BOOST_AUTO_TEST_CASE(a_replicated_write_is_pending_until_it_applies) {
    // A fake coordination group: proposals queue, and the "log" applies them
    // only when the test says so — the way a Raft commit lands later than the
    // proposal that caused it.
    using future_t = kythira::future_default<std::vector<std::byte>>;
    using promise_t = kythira::promise_default<std::vector<std::byte>>;
    sm_t sm;
    std::deque<std::pair<std::vector<std::byte>, std::shared_ptr<promise_t>>> log;
    bool fail_next = false;
    kythira::replicated_capacity_ledger<node_id_t, pg_t, future_t> l{
        [&](std::vector<std::byte> bytes) -> future_t {
            if (fail_next) {
                fail_next = false;
                throw std::runtime_error("not leader");
            }
            auto p = std::make_shared<promise_t>();
            auto f = p->getFuture();
            log.emplace_back(std::move(bytes), p);
            return f;
        },
        [&] { return sm.intents(); }};
    std::uint64_t index = 0;
    const auto commit_one = [&] {
        auto [bytes, p] = std::move(log.front());
        log.pop_front();
        p->setValue(sm.apply(bytes, ++index));
    };

    const auto rec = l.record(make_intent("r"));
    BOOST_CHECK(l.status(rec) == capacity_ledger_write_status::pending);
    BOOST_CHECK(!l.token(rec).has_value());
    BOOST_CHECK(!l.find("r").has_value());

    commit_one();
    BOOST_CHECK(l.status(rec) == capacity_ledger_write_status::committed);
    const auto token = l.token(rec);
    BOOST_REQUIRE(token.has_value());
    BOOST_CHECK_EQUAL(token->key(), "r");
    BOOST_CHECK(l.find("r")->_state == capacity_intent_state::requested);

    // A refused transition is rejected once applied.
    const auto bad = l.transition("r", capacity_intent_state::orphaned, {}, k_t0);
    commit_one();
    BOOST_CHECK(l.status(bad) == capacity_ledger_write_status::rejected);

    // A proposal that fails outright (not leader) is rejected, never pending
    // forever.
    fail_next = true;
    const auto lost = l.transition("r", capacity_intent_state::provisioning, {}, k_t0);
    BOOST_CHECK(l.status(lost) == capacity_ledger_write_status::rejected);

    // A future that resolves with an exception (a timeout) is rejected too.
    const auto timed_out = l.transition("r", capacity_intent_state::provisioning, {}, k_t0);
    log.front().second->setException(std::make_exception_ptr(std::runtime_error("timeout")));
    log.pop_front();
    BOOST_CHECK(l.status(timed_out) == capacity_ledger_write_status::rejected);
}

BOOST_AUTO_TEST_SUITE_END()
