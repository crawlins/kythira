// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Linearizability checker for a chaos_workload history
// (.kiro/specs/cairn-chaos-testing/ Requirement 5.1, design §2.3).
//
// A Wing-Gong search with Lowe's memoization (the "WGL" algorithm porcupine
// and Knossos use), run once per key: a register history is linearizable iff
// each key's sub-history is (P-compositionality), and per-key searches are
// exponentially cheaper than one search over the whole history.
//
// The model is a register holding an optional string (absent until first
// written) with three operations:
//
//   read      legal iff it returned the current value (absent = not found)
//   write     sets the value
//   cas       ok: legal iff the value equals `expected`, then sets it
//             info: sets it iff the value equals `expected`, else no effect
//
// How each completion type enters the search:
//
//   ok    the operation took effect between its invoke and its completion.
//   fail  it definitely had no effect: left out. A cas whose expected value
//         did not match is a fail, so a no-op cas never constrains the search.
//   info  it may have taken effect at any point after its invoke, or never.
//         Its return time is +infinity, so the search may place it anywhere
//         from its invoke onwards; placing it after every other operation is
//         the same as it never happening.
//
// An invoke with no completion (a history cut short) is treated as info.
// Reads are only checked when their mode is "linearizable". "local" reads are
// served from one replica's applied state without a leadership check, so a
// stale one is expected behaviour, not a violation; they are counted
// separately with the node that served them so a stale-read finding is never
// pinned on the linearizable path. Reads that did not return ok are left out:
// a read has no effect, so an unanswered one constrains nothing.
//
// The search is exponential in the worst case. Each key's search has a step
// budget; a search that exhausts it reports `unknown`, never `ok`.

#include "../history.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace docker_chaos::checkers {

enum class verdict : std::uint8_t {
    ok,
    violation,
    unknown
};

[[nodiscard]] inline auto to_string(verdict v) -> std::string_view {
    switch (v) {
        case verdict::ok:
            return "ok";
        case verdict::violation:
            return "violation";
        case verdict::unknown:
            return "unknown";
    }
    return "unknown";
}

inline auto operator<<(std::ostream& os, verdict v) -> std::ostream& {
    return os << to_string(v);
}

namespace linearizability {

/// One operation of one key, paired from its invoke and completion.
struct operation {
    std::size_t invoke_event{0};      // index into the history
    std::size_t completion_event{0};  // == invoke_event when there is none
    std::int64_t process{0};
    history::op_kind f{history::op_kind::read};
    std::optional<std::string> value;     // read: value read; write/cas: new value
    std::optional<std::string> expected;  // cas only
    std::int64_t call_ns{0};
    std::int64_t return_ns{0};  // INT64_MAX for info
    bool indeterminate{false};  // info, or no completion
    int node{0};                // the node that answered, 0 if none
};

struct key_result {
    std::string key;
    verdict result{verdict::ok};
    std::size_t operations{0};
    std::uint64_t steps{0};
    /// On a violation: the operations of this key, in invoke order, which is
    /// the history window a violation record carries (Requirement 5.5).
    std::vector<operation> window;
    /// On a violation: the most operations any partial linearization placed.
    std::size_t longest_prefix{0};
};

struct local_read {
    std::size_t completion_event{0};
    std::string key;
    std::optional<std::string> value;
    int node{0};
    std::int64_t call_ns{0};
    std::int64_t return_ns{0};
};

struct result {
    verdict overall{verdict::ok};
    std::vector<key_result> keys;  // every key checked, sorted by key
    /// ok local-mode reads, kept apart from the linearizable check.
    std::vector<local_read> local_reads;
    std::size_t failed_operations{0};  // fail completions, left out

    [[nodiscard]] auto violations() const -> std::vector<const key_result*> {
        std::vector<const key_result*> out;
        for (const auto& k : keys) {
            if (k.result == verdict::violation) {
                out.push_back(&k);
            }
        }
        return out;
    }
};

struct options {
    /// Search steps (call entries tried) per key before giving up.
    std::uint64_t max_steps_per_key{2'000'000};
};

namespace detail {

using state = std::optional<std::string>;

/// The register model's step. Returns the next state, or nothing if the
/// operation cannot take effect in `s`.
inline auto step(const state& s, const operation& op) -> std::optional<state> {
    switch (op.f) {
        case history::op_kind::read:
            if (s == op.value) {
                return s;
            }
            return std::nullopt;
        case history::op_kind::write:
            return op.value;
        case history::op_kind::cas:
            if (s == op.expected) {
                return op.value;
            }
            if (op.indeterminate) {
                // It may have run and not matched: no effect.
                return s;
            }
            return std::nullopt;
    }
    return std::nullopt;
}

struct cache_key {
    std::vector<std::uint64_t> linearized;
    state s;

    auto operator==(const cache_key&) const -> bool = default;
};

struct cache_hash {
    auto operator()(const cache_key& k) const noexcept -> std::size_t {
        std::size_t h = 1469598103934665603ULL;
        for (auto w : k.linearized) {
            h = (h ^ std::hash<std::uint64_t>{}(w)) * 1099511628211ULL;
        }
        h ^= k.s ? std::hash<std::string>{}(*k.s) + 0x9e3779b97f4a7c15ULL : 0;
        return h;
    }
};

/// The WGL search over one key's operations. `steps` and `longest` report
/// effort and progress whatever the outcome.
inline auto search(const std::vector<operation>& ops, std::uint64_t budget, std::uint64_t& steps,
                   std::size_t& longest) -> verdict {
    const std::size_t n = ops.size();
    steps = 0;
    longest = 0;
    if (n == 0) {
        return verdict::ok;
    }

    // A doubly linked list of call and return entries ordered by time, with
    // a sentinel head at index 0. Entry 1 + 2i is op i's call, 2 + 2i its
    // return. At equal times calls sort first, so operations that touch at
    // an instant count as concurrent: the permissive reading of a clock tick.
    struct entry {
        std::size_t op{0};
        bool is_call{false};
        std::int64_t time{0};
        std::size_t prev{0};
        std::size_t next{0};
    };
    constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
    std::vector<entry> list(1 + 2 * n);
    std::vector<std::size_t> order;
    order.reserve(2 * n);
    for (std::size_t i = 0; i < n; ++i) {
        list[1 + 2 * i] = entry{.op = i, .is_call = true, .time = ops[i].call_ns};
        list[2 + 2 * i] = entry{.op = i, .is_call = false, .time = ops[i].return_ns};
        order.push_back(1 + 2 * i);
        order.push_back(2 + 2 * i);
    }
    std::ranges::stable_sort(order, [&list](std::size_t a, std::size_t b) {
        if (list[a].time != list[b].time) {
            return list[a].time < list[b].time;
        }
        return list[a].is_call && !list[b].is_call;
    });
    list[0].next = order.front();
    list[0].prev = none;
    for (std::size_t k = 0; k < order.size(); ++k) {
        auto& e = list[order[k]];
        e.prev = k == 0 ? 0 : order[k - 1];
        e.next = k + 1 == order.size() ? none : order[k + 1];
    }
    auto match = [](std::size_t call) { return call + 1; };

    auto lift = [&](std::size_t call) {
        auto& c = list[call];
        list[c.prev].next = c.next;
        list[c.next].prev = c.prev;  // a call is never last: its return follows
        auto& r = list[match(call)];
        list[r.prev].next = r.next;
        if (r.next != none) {
            list[r.next].prev = r.prev;
        }
    };
    auto unlift = [&](std::size_t call) {
        auto& r = list[match(call)];
        list[r.prev].next = match(call);
        if (r.next != none) {
            list[r.next].prev = match(call);
        }
        auto& c = list[call];
        list[c.prev].next = call;
        list[c.next].prev = call;
    };

    std::vector<std::uint64_t> linearized((n + 63) / 64, 0);
    auto set_bit = [&linearized](std::size_t i, bool on) {
        if (on) {
            linearized[i / 64] |= (std::uint64_t{1} << (i % 64));
        } else {
            linearized[i / 64] &= ~(std::uint64_t{1} << (i % 64));
        }
    };
    std::unordered_set<cache_key, cache_hash> cache;
    std::vector<std::pair<std::size_t, state>> calls;  // (call entry, state before)
    state s;

    std::size_t cur = list[0].next;
    while (list[0].next != none) {
        if (list[cur].is_call) {
            if (++steps > budget) {
                return verdict::unknown;
            }
            const auto& op = ops[list[cur].op];
            if (auto next = step(s, op)) {
                set_bit(list[cur].op, true);
                if (cache.insert(cache_key{linearized, *next}).second) {
                    calls.emplace_back(cur, s);
                    longest = std::max(longest, calls.size());
                    s = std::move(*next);
                    lift(cur);
                    cur = list[0].next;
                    continue;
                }
                set_bit(list[cur].op, false);
            }
            cur = list[cur].next;
        } else {
            // A return reached with its operation unplaced: every placement
            // from here fails, so undo the latest one and try the next call.
            if (calls.empty()) {
                return verdict::violation;
            }
            auto [call, before] = std::move(calls.back());
            calls.pop_back();
            s = std::move(before);
            set_bit(list[call].op, false);
            unlift(call);
            cur = list[call].next;
        }
    }
    return verdict::ok;
}

}  // namespace detail

/// Pairs invokes with completions and groups the checkable operations by
/// key. Throws std::invalid_argument on a structurally broken history, which
/// is a workload bug rather than a finding.
[[nodiscard]] inline auto partition(const std::vector<history::event>& h, result& out)
    -> std::map<std::string, std::vector<operation>> {
    if (auto err = history::structural_error(h); !err.empty()) {
        throw std::invalid_argument("linearizability: malformed history: " + err);
    }
    std::map<std::string, std::vector<operation>> by_key;
    std::map<std::int64_t, std::size_t> open;  // process → invoke index
    auto add = [&](std::size_t inv, std::optional<std::size_t> done) {
        const auto& i = h[inv];
        const bool linearizable_read = i.f != history::op_kind::read || i.mode != "local";
        if (!done) {
            if (i.f == history::op_kind::read) {
                return;  // never answered, no effect
            }
            by_key[i.key].push_back(operation{.invoke_event = inv,
                                              .completion_event = inv,
                                              .process = i.process,
                                              .f = i.f,
                                              .value = i.value,
                                              .expected = i.expected,
                                              .call_ns = i.time_ns,
                                              .return_ns = std::numeric_limits<std::int64_t>::max(),
                                              .indeterminate = true,
                                              .node = 0});
            return;
        }
        const auto& c = h[*done];
        if (c.type == history::event_type::fail) {
            ++out.failed_operations;
            return;
        }
        if (i.f == history::op_kind::read) {
            if (c.type != history::event_type::ok) {
                return;
            }
            if (!linearizable_read) {
                out.local_reads.push_back(local_read{.completion_event = *done,
                                                     .key = i.key,
                                                     .value = c.value,
                                                     .node = c.node,
                                                     .call_ns = i.time_ns,
                                                     .return_ns = c.time_ns});
                return;
            }
        }
        const bool info = c.type == history::event_type::info;
        by_key[i.key].push_back(
            operation{.invoke_event = inv,
                      .completion_event = *done,
                      .process = i.process,
                      .f = i.f,
                      // A read's value is what it returned; a write's what it wrote.
                      .value = i.f == history::op_kind::read ? c.value : i.value,
                      .expected = i.expected,
                      .call_ns = i.time_ns,
                      .return_ns = info ? std::numeric_limits<std::int64_t>::max() : c.time_ns,
                      .indeterminate = info,
                      .node = c.node});
    };
    for (std::size_t k = 0; k < h.size(); ++k) {
        if (h[k].type == history::event_type::invoke) {
            open[h[k].process] = k;
            continue;
        }
        auto it = open.find(h[k].process);
        add(it->second, k);
        open.erase(it);
    }
    for (const auto& [process, inv] : open) {
        add(inv, std::nullopt);
    }
    for (auto& [key, ops] : by_key) {
        std::ranges::sort(ops, {}, &operation::invoke_event);
    }
    return by_key;
}

[[nodiscard]] inline auto check(const std::vector<history::event>& h, options opts = {}) -> result {
    result out;
    for (auto& [key, ops] : partition(h, out)) {
        key_result kr;
        kr.key = key;
        kr.operations = ops.size();
        kr.result = detail::search(ops, opts.max_steps_per_key, kr.steps, kr.longest_prefix);
        if (kr.result == verdict::violation) {
            kr.window = std::move(ops);
            out.overall = verdict::violation;
        } else if (kr.result == verdict::unknown && out.overall == verdict::ok) {
            out.overall = verdict::unknown;
        }
        out.keys.push_back(std::move(kr));
    }
    return out;
}

/// The violation record for one key (Requirement 5.5): the invariant, the
/// history window as the original event indices, and the operations.
[[nodiscard]] inline auto violation_json(const key_result& k) -> boost::json::object {
    auto opt = [](const std::optional<std::string>& v) {
        return v ? boost::json::value(*v) : boost::json::value(nullptr);
    };
    boost::json::array ops;
    for (const auto& op : k.window) {
        boost::json::object o{
            {"p", op.process},
            {"f", history::to_string(op.f)},
            {"value", op.f == history::op_kind::cas
                          ? boost::json::value(boost::json::array{opt(op.expected), opt(op.value)})
                          : opt(op.value)},
            {"invoke", op.invoke_event},
            {"call_ns", op.call_ns},
        };
        if (op.indeterminate) {
            o["info"] = true;
        } else {
            o["completion"] = op.completion_event;
            o["return_ns"] = op.return_ns;
        }
        if (op.node != 0) {
            o["node"] = op.node;
        }
        ops.push_back(std::move(o));
    }
    return boost::json::object{
        {"invariant", "linearizability"}, {"key", k.key},
        {"operations", k.operations},     {"longest_linearizable_prefix", k.longest_prefix},
        {"window", std::move(ops)},
    };
}

}  // namespace linearizability
}  // namespace docker_chaos::checkers
