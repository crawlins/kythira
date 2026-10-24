// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The three cairn checkers (cairn-chaos-testing task 5) against hand-written
// histories and node states. Every invariant has at least one known-bad input
// the checker must flag: a checker never shown failing has not been shown to
// detect anything.

#define BOOST_TEST_MODULE checkers_unit_test
#include <boost/test/unit_test.hpp>

#include "checkers/linearizability.hpp"
#include "checkers/liveness.hpp"
#include "checkers/raft_safety.hpp"
#include "history.hpp"

#include <algorithm>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace h = docker_chaos::history;
namespace c = docker_chaos::checkers;
namespace lin = docker_chaos::checkers::linearizability;
namespace rs = docker_chaos::checkers::raft_safety;
namespace live = docker_chaos::checkers::liveness;

using namespace std::chrono_literals;

namespace {

using opt = std::optional<std::string>;

// One operation as a test writes it: who, what, when it was invoked and,
// unless it never completed, when and how it completed.
struct spec {
    std::int64_t p{0};
    h::op_kind f{h::op_kind::read};
    std::string key{"k"};
    opt value;     // read: the value returned; write/cas: the new value
    opt expected;  // cas
    std::int64_t call{0};
    std::optional<std::int64_t> ret;
    h::event_type type{h::event_type::ok};
    std::string mode{"linearizable"};
    int node{1};
};

spec rd(std::int64_t p, opt v, std::int64_t call, std::int64_t ret) {
    return spec{.p = p,
                .f = h::op_kind::read,
                .key = "k",
                .value = std::move(v),
                .expected = std::nullopt,
                .call = call,
                .ret = ret};
}

spec wr(std::int64_t p, std::string v, std::int64_t call, std::optional<std::int64_t> ret,
        h::event_type type = h::event_type::ok) {
    return spec{.p = p,
                .f = h::op_kind::write,
                .key = "k",
                .value = std::move(v),
                .expected = std::nullopt,
                .call = call,
                .ret = ret,
                .type = type};
}

spec cas(std::int64_t p, opt expected, std::string v, std::int64_t call,
         std::optional<std::int64_t> ret, h::event_type type = h::event_type::ok) {
    return spec{.p = p,
                .f = h::op_kind::cas,
                .key = "k",
                .value = std::move(v),
                .expected = std::move(expected),
                .call = call,
                .ret = ret,
                .type = type};
}

spec on_key(spec s, std::string key) {
    s.key = std::move(key);
    return s;
}

// The history the workload would have written: invoke and completion events
// in time order, a read's value only on its completion.
std::vector<h::event> history_of(const std::vector<spec>& ops) {
    struct timed {
        std::int64_t t;
        int order;  // invokes before completions at equal times
        h::event e;
    };
    std::vector<timed> all;
    for (const auto& s : ops) {
        h::event inv;
        inv.process = s.p;
        inv.type = h::event_type::invoke;
        inv.f = s.f;
        inv.key = s.key;
        inv.value = s.f == h::op_kind::read ? std::nullopt : s.value;
        inv.expected = s.expected;
        inv.time_ns = s.call;
        if (s.f == h::op_kind::read) {
            inv.mode = s.mode;
        }
        all.push_back(timed{s.call, 0, inv});
        if (s.ret) {
            h::event done = inv;
            done.type = s.type;
            done.value = s.value;
            done.time_ns = *s.ret;
            done.node = s.node;
            all.push_back(timed{*s.ret, 1, done});
        }
    }
    std::ranges::stable_sort(all, [](const timed& a, const timed& b) {
        return a.t != b.t ? a.t < b.t : a.order < b.order;
    });
    std::vector<h::event> out;
    for (auto& t : all) {
        out.push_back(std::move(t.e));
    }
    return out;
}

c::verdict lin_verdict(const std::vector<spec>& ops) {
    return lin::check(history_of(ops)).overall;
}

}  // namespace

// ── Linearizability ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(linearizability)

BOOST_AUTO_TEST_CASE(empty_history_is_linearizable) {
    BOOST_TEST(lin::check({}).overall == c::verdict::ok);
}

BOOST_AUTO_TEST_CASE(sequential_write_then_read) {
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), rd(1, "1", 20, 30)}) == c::verdict::ok);
    BOOST_TEST(lin_verdict({rd(1, std::nullopt, 0, 5), wr(0, "1", 10, 20)}) == c::verdict::ok);
}

// Known-bad: a read that starts after a write finished returns the value
// from before it.
BOOST_AUTO_TEST_CASE(stale_read_after_a_completed_write_is_a_violation) {
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), rd(1, std::nullopt, 20, 30)}) ==
               c::verdict::violation);
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), wr(0, "2", 20, 30), rd(1, "1", 40, 50)}) ==
               c::verdict::violation);
}

BOOST_AUTO_TEST_CASE(a_read_concurrent_with_a_write_may_see_either_value) {
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 30), rd(1, "1", 10, 20)}) == c::verdict::ok);
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 30), rd(1, std::nullopt, 10, 20)}) == c::verdict::ok);
}

// Known-bad: two reads that do not overlap see the write and then un-see it.
BOOST_AUTO_TEST_CASE(a_value_that_goes_back_is_a_violation) {
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 100), rd(1, "1", 10, 20), rd(2, std::nullopt, 30, 40)}) ==
               c::verdict::violation);
}

BOOST_AUTO_TEST_CASE(touching_operations_count_as_concurrent) {
    // The read is invoked at the instant the write returns: either order.
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), rd(1, std::nullopt, 10, 20)}) == c::verdict::ok);
}

BOOST_AUTO_TEST_CASE(cas_ok_requires_the_expected_value) {
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), cas(0, "1", "2", 20, 30), rd(1, "2", 40, 50)}) ==
               c::verdict::ok);
    BOOST_TEST(lin_verdict({cas(0, std::nullopt, "1", 0, 10), rd(1, "1", 20, 30)}) ==
               c::verdict::ok);
    // Known-bad: the cas reported applied though the value was 1, not 3.
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), cas(0, "3", "2", 20, 30)}) == c::verdict::violation);
}

BOOST_AUTO_TEST_CASE(failed_operations_have_no_effect) {
    auto r =
        lin::check(history_of({wr(0, "1", 0, 10), cas(0, "2", "3", 20, 30, h::event_type::fail),
                               wr(1, "9", 25, 35, h::event_type::fail), rd(2, "1", 40, 50)}));
    BOOST_TEST(r.overall == c::verdict::ok);
    BOOST_TEST(r.failed_operations == 2U);
    // Known-bad: a read of a value only a failed write wrote.
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), wr(1, "9", 15, 20, h::event_type::fail),
                            rd(2, "9", 40, 50)}) == c::verdict::violation);
}

BOOST_AUTO_TEST_CASE(an_info_write_may_take_effect_late_or_never) {
    // Took effect after its client gave up.
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), wr(1, "2", 5, 15, h::event_type::info),
                            rd(2, "2", 50, 60)}) == c::verdict::ok);
    // Never took effect.
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), wr(1, "2", 5, 15, h::event_type::info),
                            rd(2, "1", 50, 60)}) == c::verdict::ok);
    // Known-bad: it took effect, then was undone.
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), wr(1, "2", 5, 15, h::event_type::info),
                            rd(2, "2", 50, 60), rd(3, "1", 70, 80)}) == c::verdict::violation);
    // Known-bad: it cannot take effect before it was invoked.
    BOOST_TEST(lin_verdict({rd(2, "2", 0, 4), wr(1, "2", 5, 15, h::event_type::info)}) ==
               c::verdict::violation);
}

BOOST_AUTO_TEST_CASE(an_invoke_without_a_completion_is_indeterminate) {
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), wr(1, "2", 20, std::nullopt), rd(2, "2", 30, 40)}) ==
               c::verdict::ok);
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), wr(1, "2", 20, std::nullopt), rd(2, "1", 30, 40)}) ==
               c::verdict::ok);
}

BOOST_AUTO_TEST_CASE(an_info_cas_applies_only_on_a_match) {
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), cas(1, "1", "2", 20, 30, h::event_type::info),
                            rd(2, "2", 40, 50)}) == c::verdict::ok);
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), cas(1, "1", "2", 20, 30, h::event_type::info),
                            rd(2, "1", 40, 50)}) == c::verdict::ok);
    // Known-bad: the value it would install is read, but it could not have
    // matched (the register held 1, it expected 5).
    BOOST_TEST(lin_verdict({wr(0, "1", 0, 10), cas(1, "5", "2", 20, 30, h::event_type::info),
                            rd(2, "2", 40, 50)}) == c::verdict::violation);
}

BOOST_AUTO_TEST_CASE(local_reads_are_recorded_apart_and_never_flagged) {
    auto stale = rd(1, std::nullopt, 20, 30);
    stale.mode = "local";
    stale.node = 3;
    auto r = lin::check(history_of({wr(0, "1", 0, 10), stale}));
    BOOST_TEST(r.overall == c::verdict::ok);
    BOOST_REQUIRE(r.local_reads.size() == 1U);
    BOOST_TEST(r.local_reads[0].node == 3);
    BOOST_TEST(!r.local_reads[0].value.has_value());
    BOOST_TEST(r.local_reads[0].key == "k");
}

BOOST_AUTO_TEST_CASE(keys_are_checked_independently) {
    auto r = lin::check(
        history_of({on_key(wr(0, "1", 0, 10), "a"), on_key(rd(1, std::nullopt, 20, 30), "b"),
                    on_key(rd(2, "1", 20, 30), "a"), on_key(wr(3, "1", 40, 50), "c"),
                    on_key(rd(4, std::nullopt, 60, 70), "c")}));
    BOOST_TEST(r.overall == c::verdict::violation);
    BOOST_REQUIRE(r.keys.size() == 3U);
    auto bad = r.violations();
    BOOST_REQUIRE(bad.size() == 1U);
    BOOST_TEST(bad[0]->key == "c");
    BOOST_TEST(bad[0]->window.size() == 2U);

    auto rec = lin::violation_json(*bad[0]);
    BOOST_TEST(rec.at("invariant").as_string() == "linearizability");
    BOOST_TEST(rec.at("key").as_string() == "c");
    BOOST_TEST(rec.at("window").as_array().size() == 2U);
}

BOOST_AUTO_TEST_CASE(an_exhausted_budget_is_unknown_never_ok) {
    // Ten concurrent writes and a read of a value nobody wrote: the search
    // has to try many orders before it can conclude anything.
    std::vector<spec> ops;
    for (int i = 0; i < 10; ++i) {
        ops.push_back(wr(i, std::to_string(i), 0, 100));
    }
    ops.push_back(rd(20, "x", 0, 100));
    auto r = lin::check(history_of(ops), lin::options{.max_steps_per_key = 5});
    BOOST_TEST(r.overall == c::verdict::unknown);
    BOOST_TEST(lin::check(history_of(ops)).overall == c::verdict::violation);
}

BOOST_AUTO_TEST_CASE(a_malformed_history_is_rejected) {
    auto bad = history_of({wr(0, "1", 0, 10)});
    bad.push_back(bad.back());  // a second completion
    BOOST_CHECK_THROW(static_cast<void>(lin::check(bad)), std::invalid_argument);
}

// A register simulated with a real linearization point inside each
// operation's interval yields a linearizable history by construction; the
// checker must accept every one. Replacing one read's value with one nobody
// wrote must then be flagged.
BOOST_AUTO_TEST_CASE(simulated_histories_are_accepted_and_a_corrupted_read_is_not) {
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        std::mt19937_64 rng(seed);
        auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
        struct planned {
            spec s;
            std::int64_t at;
        };
        std::vector<planned> plan;
        std::vector<std::int64_t> free_at(4, 0);  // 4 processes
        for (int i = 0; i < 40; ++i) {
            const int p = pick(4);
            const std::int64_t call = free_at[p] + pick(20);
            const std::int64_t ret = call + 1 + pick(60);
            free_at[p] = ret + 1;
            const std::int64_t at = call + pick(static_cast<int>(ret - call) + 1);
            const std::string key = "k" + std::to_string(pick(2));
            const int kind = pick(3);
            spec s = kind == 0 ? rd(p, std::nullopt, call, ret)
                     : kind == 1
                         ? wr(p, std::to_string(pick(3)), call, ret)
                         : cas(p, std::to_string(pick(3)), std::to_string(pick(3)), call, ret);
            s.key = key;
            plan.push_back(planned{s, at});
        }
        // Apply at the linearization points, in order, to fill in results.
        std::vector<std::size_t> by_point(plan.size());
        for (std::size_t i = 0; i < plan.size(); ++i) {
            by_point[i] = i;
        }
        std::ranges::stable_sort(by_point, {}, [&plan](std::size_t i) { return plan[i].at; });
        std::map<std::string, opt> reg;
        for (auto i : by_point) {
            auto& s = plan[i].s;
            auto& cur = reg[s.key];
            if (s.f == h::op_kind::read) {
                s.value = cur;
            } else if (s.f == h::op_kind::cas && cur != s.expected) {
                s.type = h::event_type::fail;
            } else {
                cur = s.value;  // a write, or a cas that matched
            }
        }
        std::vector<spec> ops;
        for (auto& pl : plan) {
            ops.push_back(pl.s);
        }
        BOOST_TEST(lin_verdict(ops) == c::verdict::ok, "seed " << seed);

        for (auto& s : ops) {
            if (s.f == h::op_kind::read) {
                s.value = "never-written";
                BOOST_TEST(lin_verdict(ops) == c::verdict::violation, "seed " << seed);
                break;
            }
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ── Raft safety ──────────────────────────────────────────────────────────────

namespace {

rs::log_entry entry(std::uint64_t index, std::uint64_t term, std::string cmd = {}) {
    return rs::log_entry{.index = index,
                         .term = term,
                         .type = "command",
                         .command_b64 = cmd.empty() ? "c" + std::to_string(index) : cmd};
}

rs::node_state node(int id, std::string role, std::uint64_t term, std::uint64_t commit,
                    std::vector<rs::log_entry> log, std::uint64_t snapshot = 0) {
    rs::node_state s{.node = id,
                     .role = std::move(role),
                     .term = term,
                     .commit_index = commit,
                     .snapshot_index = snapshot,
                     .log = {}};
    for (auto& e : log) {
        s.log[e.index] = std::move(e);
    }
    return s;
}

std::vector<rs::log_entry> log_of(std::initializer_list<std::uint64_t> terms) {
    std::vector<rs::log_entry> out;
    std::uint64_t i = 0;
    for (auto t : terms) {
        ++i;
        out.push_back(entry(i, t));
    }
    return out;
}

bool flags(const rs::result& r, const std::string& invariant) {
    return std::ranges::any_of(r.violations,
                               [&](const rs::violation& v) { return v.invariant == invariant; });
}

}  // namespace

BOOST_AUTO_TEST_SUITE(raft_safety)

BOOST_AUTO_TEST_CASE(agreeing_nodes_are_safe) {
    auto r = rs::check(
        {node(1, "leader", 2, 3, log_of({1, 1, 2})), node(2, "follower", 2, 3, log_of({1, 1, 2})),
         node(3, "follower", 2, 2, log_of({1, 1}))},
        {{.node = 1, .term = 1, .at_ms = 0}, {.node = 1, .term = 2, .at_ms = 9}});
    BOOST_TEST(r.overall == c::verdict::ok);
    BOOST_TEST(r.terms_with_a_leader == 2U);
    BOOST_TEST(r.committed_entries_compared == 3U);
}

BOOST_AUTO_TEST_CASE(two_leaders_in_one_term_is_flagged) {
    auto r =
        rs::check({}, {{.node = 1, .term = 5, .at_ms = 0}, {.node = 2, .term = 5, .at_ms = 3}});
    BOOST_TEST(flags(r, "election_safety"));
    BOOST_REQUIRE(r.violations.size() == 1U);
    BOOST_TEST(r.violations[0].term.value() == 5U);
    BOOST_TEST((r.violations[0].nodes == std::vector<int>{1, 2}));
    // The same election read twice (before and after a restart) is one.
    BOOST_TEST(
        rs::check({}, {{.node = 1, .term = 5, .at_ms = 0}, {.node = 1, .term = 5, .at_ms = 0}})
            .overall == c::verdict::ok);
}

BOOST_AUTO_TEST_CASE(same_index_and_term_with_a_different_entry_is_flagged) {
    auto a = log_of({1, 1});
    auto b = log_of({1, 1});
    b[1].command_b64 = "other";
    auto r = rs::check({node(1, "follower", 1, 0, a), node(2, "follower", 1, 0, b)}, {});
    BOOST_TEST(flags(r, "log_matching"));
    BOOST_TEST(r.violations[0].index.value() == 2U);
}

BOOST_AUTO_TEST_CASE(matching_entry_over_a_different_prefix_is_flagged) {
    auto r = rs::check(
        {node(1, "follower", 3, 0, log_of({1, 3})), node(2, "follower", 3, 0, log_of({2, 3}))}, {});
    BOOST_TEST(flags(r, "log_matching"));
}

// A follower holding a deposed leader's uncommitted tail is normal Raft.
BOOST_AUTO_TEST_CASE(a_divergent_uncommitted_tail_is_legal) {
    auto r = rs::check(
        {node(1, "leader", 3, 3, log_of({1, 1, 3})), node(2, "follower", 3, 3, log_of({1, 1, 3})),
         node(3, "follower", 3, 2, log_of({1, 1, 2, 2}))},
        {{.node = 1, .term = 3, .at_ms = 0}, {.node = 3, .term = 2, .at_ms = 0}});
    BOOST_TEST(r.overall == c::verdict::ok);
}

BOOST_AUTO_TEST_CASE(different_committed_entries_at_one_index_are_flagged) {
    auto r = rs::check(
        {node(1, "follower", 3, 2, log_of({1, 2})), node(2, "follower", 3, 2, log_of({1, 3}))}, {});
    BOOST_TEST(flags(r, "state_machine_safety"));
    BOOST_TEST(!flags(r, "log_matching"));  // different terms: log matching says nothing
}

BOOST_AUTO_TEST_CASE(a_leader_missing_a_committed_entry_is_flagged) {
    // Node 2 committed index 3; the current leader's log ends at 2.
    auto r = rs::check(
        {node(1, "leader", 4, 2, log_of({1, 1})), node(2, "follower", 4, 3, log_of({1, 1, 2}))},
        {});
    BOOST_TEST(flags(r, "leader_completeness"));

    // ...or holds another term there, uncommitted on its side.
    auto r2 = rs::check(
        {node(1, "leader", 4, 2, log_of({1, 1, 4})), node(2, "follower", 4, 3, log_of({1, 1, 2}))},
        {});
    BOOST_TEST(flags(r2, "leader_completeness"));
}

// A paused leader of term 2 that has not heard of term 3 may lack what term
// 3 committed.
BOOST_AUTO_TEST_CASE(a_deposed_leader_behind_the_latest_term_is_not_held_to_it) {
    auto r = rs::check(
        {node(1, "leader", 2, 2, log_of({1, 2})), node(2, "leader", 3, 3, log_of({1, 2, 3})),
         node(3, "follower", 3, 3, log_of({1, 2, 3}))},
        {{.node = 1, .term = 2, .at_ms = 0}, {.node = 2, .term = 3, .at_ms = 0}});
    BOOST_TEST(r.overall == c::verdict::ok);
}

BOOST_AUTO_TEST_CASE(compacted_and_unfetched_indices_are_skipped) {
    // The leader compacted 1-2 and the caller fetched only 3 from it.
    auto leader = node(1, "leader", 2, 3, {entry(3, 2)}, 2);
    auto r = rs::check({leader, node(2, "follower", 2, 3, log_of({1, 1, 2}))}, {});
    BOOST_TEST(r.overall == c::verdict::ok);
    // A follower fetched only from index 3 while the leader holds 1-3.
    auto r2 = rs::check(
        {node(1, "leader", 2, 3, log_of({1, 1, 2})), node(2, "follower", 2, 3, {entry(3, 2)})}, {});
    BOOST_TEST(r2.overall == c::verdict::ok);
}

BOOST_AUTO_TEST_CASE(adapters_read_the_control_plane_json) {
    // Shapes from cmd/chaos_node/control_codec.hpp, with a text node id as
    // the image's KYTHIRA_NODE_ID_KIND=text build reports it.
    auto status = boost::json::parse(R"({"node_id":"2","role":"leader","term":4,"leader_id":"2",
        "commit_index":2,"last_applied":2,"last_log_index":2,"last_log_term":4,
        "snapshot_index":0,"config":{"voters":["1","2"],"learners":[],"joint":false}})")
                      .as_object();
    auto range = boost::json::parse(R"({"from":1,"to":2,"snapshot_index":0,"last_log_index":2,
        "entries":[{"index":1,"term":1,"type":"command","command_b64":"AQ=="},
                   {"index":2,"term":4,"type":"no_op","command_b64":""}]})")
                     .as_object();
    auto s = rs::node_state_from_json(status, {range});
    BOOST_TEST(s.node == 2);
    BOOST_TEST(s.role == "leader");
    BOOST_TEST(s.term == 4U);
    BOOST_TEST(s.commit_index == 2U);
    BOOST_REQUIRE(s.log.size() == 2U);
    BOOST_TEST(s.log.at(2).type == "no_op");
    BOOST_TEST(s.log.at(1).command_b64 == "AQ==");

    auto el = rs::elections_from_json(
        boost::json::parse(
            R"({"node_id":3,"elections":[{"term":1,"at_ms":10},{"term":4,"at_ms":99}]})")
            .as_object());
    BOOST_REQUIRE(el.size() == 2U);
    BOOST_TEST(el[1].node == 3);
    BOOST_TEST(el[1].term == 4U);
    BOOST_TEST(el[1].at_ms == 99);

    auto rec = rs::violation_json(rs::violation{
        .invariant = "log_matching", .detail = "d", .nodes = {1, 2}, .index = 7, .term = 3});
    BOOST_TEST(rec.at("invariant").as_string() == "log_matching");
    BOOST_TEST(rec.at("index").to_number<std::uint64_t>() == 7U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Liveness ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(liveness)

BOOST_AUTO_TEST_CASE(the_bound_scales_with_k_and_the_build) {
    BOOST_TEST(live::bound(300ms, 10).count() == 3000 * KYTHIRA_TEST_TIMEOUT_SCALE);
    BOOST_TEST(live::bound(300ms).count() == 300 * live::default_k * KYTHIRA_TEST_TIMEOUT_SCALE);
}

BOOST_AUTO_TEST_CASE(a_leader_and_a_commit_within_the_bound_pass) {
    auto r = live::check({{.at = 100ms, .leader = std::nullopt, .commit_index = 5},
                          {.at = 400ms, .leader = 2, .commit_index = 5},
                          {.at = 600ms, .leader = 2, .commit_index = 6}},
                         5, 1000ms);
    BOOST_TEST(r.overall == c::verdict::ok);
    BOOST_TEST(r.leader_at.value().count() == 400);
    BOOST_TEST(r.commit_at.value().count() == 600);
}

BOOST_AUTO_TEST_CASE(no_leader_is_flagged) {
    auto r = live::check({{.at = 100ms, .leader = std::nullopt, .commit_index = 5},
                          {.at = 900ms, .leader = std::nullopt, .commit_index = 5}},
                         5, 1000ms);
    BOOST_TEST(r.overall == c::verdict::violation);
    BOOST_TEST(r.detail.find("no leader") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_leader_that_commits_nothing_is_flagged) {
    auto r = live::check({{.at = 100ms, .leader = 1, .commit_index = 5},
                          {.at = 900ms, .leader = 1, .commit_index = 5}},
                         5, 1000ms);
    BOOST_TEST(r.overall == c::verdict::violation);
    BOOST_TEST(r.detail.find("commit_index") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(progress_after_the_bound_is_flagged) {
    auto r = live::check({{.at = 100ms, .leader = 1, .commit_index = 5},
                          {.at = 1200ms, .leader = 1, .commit_index = 9}},
                         5, 1000ms);
    BOOST_TEST(r.overall == c::verdict::violation);
}

BOOST_AUTO_TEST_CASE(wait_stops_at_progress_or_the_bound) {
    int calls = 0;
    auto r = live::wait(
        [&calls]() -> live::sample {
            ++calls;
            return {.at = 0ms,
                    .leader = calls >= 3 ? std::optional<int>(1) : std::nullopt,
                    .commit_index = calls >= 4 ? 8U : 7U};
        },
        7, 5s, 1ms);
    BOOST_TEST(r.overall == c::verdict::ok);
    BOOST_TEST(calls == 4);

    auto stuck = live::wait(
        []() -> live::sample { return {.at = 0ms, .leader = std::nullopt, .commit_index = 0}; }, 0,
        30ms, 1ms);
    BOOST_TEST(stuck.overall == c::verdict::violation);
}

BOOST_AUTO_TEST_SUITE_END()
