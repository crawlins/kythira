// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Replay and minimization of a schedule log (.kiro/specs/cairn-chaos-testing/
// Requirement 3.5, design §4), both without an agent or a model.
//
// Replay runs the logged calls against a fresh cluster at their logged
// offsets from the first call, then heals and runs every checker. It
// reproduces the schedule, not the interleaving inside one step, so
// `--repeat N` reports how many of N attempts saw the logged violation
// again; a finding is confirmed at one or more in a stated N.
//
// Minimization is ddmin over the calls that inject or undo faults; the
// workload and check calls are always kept, since without them nothing is
// measured. A kept call keeps its original offset, so dropping a call never
// compresses the time between the others. `--keep` judges one subset an
// agent proposes: the agent chooses, chaos_ctl judges, so no minimal
// schedule is declared that does not reproduce.

#include "schedule_log.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace chaos_ctl::replay {

/// Calls a replay re-executes: everything but the read-only views, which
/// change nothing and would only add their own latency.
[[nodiscard]] inline auto replayable(const call_record& c) -> bool {
    return c.tool != "cluster.status" && c.tool != "workload.history";
}

/// Calls minimization may drop: the ones that inject or undo a fault.
[[nodiscard]] inline auto droppable(const call_record& c) -> bool {
    return c.tool.starts_with("fault.") || c.tool == "cluster.add_member" ||
           c.tool == "cluster.remove_member" || c.tool == "run.reset";
}

/// Whether an attempt that saw `seen` reproduces a log that recorded
/// `recorded`. A log with no recorded violation is reproduced by any.
[[nodiscard]] inline auto reproduces(const std::set<std::string>& recorded,
                                     const std::set<std::string>& seen) -> bool {
    if (recorded.empty()) {
        return !seen.empty();
    }
    return std::ranges::any_of(recorded, [&seen](const auto& r) { return seen.contains(r); });
}

struct attempt_outcome {
    std::set<std::string> violated;
    boost::json::object summary;
};

/// Runs `calls` once against a fresh cluster. `base_mono_ms` is the logged
/// time offsets are measured from (the original log's first call).
using attempt_fn = std::function<attempt_outcome(const std::vector<call_record>& calls,
                                                 std::int64_t base_mono_ms, int attempt)>;

struct repeat_result {
    int attempts{0};
    int reproduced{0};
    boost::json::array per_attempt;

    [[nodiscard]] auto to_json(const std::set<std::string>& recorded) const -> boost::json::object {
        boost::json::array rec;
        for (const auto& r : recorded) {
            rec.emplace_back(r);
        }
        return {{"attempts", attempts},
                {"reproduced", reproduced},
                {"recorded_invariants", std::move(rec)},
                {"per_attempt", per_attempt}};
    }
};

[[nodiscard]] inline auto base_of(const std::vector<call_record>& calls) -> std::int64_t {
    return calls.empty() ? 0 : calls.front().started_mono_ms;
}

/// `n` attempts at `calls`; stops early after the first reproduction when
/// `stop_on_first` (what minimization needs: one reproduction is a yes).
[[nodiscard]] inline auto repeat(const std::vector<call_record>& calls, std::int64_t base,
                                 const std::set<std::string>& recorded, int n,
                                 const attempt_fn& attempt, bool stop_on_first = false,
                                 int first_attempt = 1) -> repeat_result {
    repeat_result r;
    for (int i = 0; i < n; ++i) {
        auto out = attempt(calls, base, first_attempt + i);
        const bool hit = reproduces(recorded, out.violated);
        ++r.attempts;
        r.reproduced += hit ? 1 : 0;
        boost::json::array seen;
        for (const auto& v : out.violated) {
            seen.emplace_back(v);
        }
        r.per_attempt.emplace_back(boost::json::object{{"attempt", first_attempt + i},
                                                       {"reproduced", hit},
                                                       {"violated", std::move(seen)},
                                                       {"summary", out.summary}});
        if (hit && stop_on_first) {
            break;
        }
    }
    return r;
}

/// Zeller's ddmin: a 1-minimal subset of `items` for which `test` holds,
/// given that it holds for `items`. `test` gets subsets in `items` order.
[[nodiscard]] inline auto ddmin(std::vector<std::size_t> items,
                                const std::function<bool(const std::vector<std::size_t>&)>& test)
    -> std::vector<std::size_t> {
    if (test({})) {
        return {};
    }
    std::size_t n = 2;
    while (items.size() >= 2) {
        n = std::min(n, items.size());
        std::vector<std::vector<std::size_t>> chunks(n);
        for (std::size_t i = 0; i < items.size(); ++i) {
            chunks[i * n / items.size()].push_back(items[i]);
        }
        bool reduced = false;
        for (const auto& chunk : chunks) {
            if (test(chunk)) {
                items = chunk;
                n = 2;
                reduced = true;
                break;
            }
        }
        if (!reduced && n > 2) {
            for (std::size_t skip = 0; skip < n; ++skip) {
                std::vector<std::size_t> complement;
                for (std::size_t c = 0; c < n; ++c) {
                    if (c != skip) {
                        complement.insert(complement.end(), chunks[c].begin(), chunks[c].end());
                    }
                }
                if (test(complement)) {
                    items = std::move(complement);
                    n = std::max<std::size_t>(n - 1, 2);
                    reduced = true;
                    break;
                }
            }
        }
        if (!reduced) {
            if (n >= items.size()) {
                break;
            }
            n = std::min(n * 2, items.size());
        }
    }
    return items;
}

/// The calls of `all` that a subset of the droppable ones leaves: every
/// call that is not droppable, plus the droppable calls whose positions in
/// `all` are in `keep`.
[[nodiscard]] inline auto with_droppable(const std::vector<call_record>& all,
                                         const std::vector<std::size_t>& keep)
    -> std::vector<call_record> {
    const std::set<std::size_t> k(keep.begin(), keep.end());
    std::vector<call_record> out;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (!droppable(all[i]) || k.contains(i)) {
            out.push_back(all[i]);
        }
    }
    return out;
}

/// Positions in `all` of the droppable calls.
[[nodiscard]] inline auto droppable_positions(const std::vector<call_record>& all)
    -> std::vector<std::size_t> {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (droppable(all[i])) {
            out.push_back(i);
        }
    }
    return out;
}

struct minimize_result {
    bool reproduced_in_full{false};
    std::vector<call_record> minimal;
    int attempts{0};
};

/// ddmin over the droppable calls, each candidate judged by up to `n`
/// attempts (one reproduction is enough).
[[nodiscard]] inline auto minimize(const std::vector<call_record>& all, int n,
                                   const attempt_fn& attempt) -> minimize_result {
    minimize_result out;
    const auto recorded = violated(all);
    const auto base = base_of(all);
    auto judge = [&](const std::vector<call_record>& calls) {
        auto r = repeat(calls, base, recorded, n, attempt, true, out.attempts + 1);
        out.attempts += r.attempts;
        return r.reproduced > 0;
    };
    if (!judge(all)) {
        return out;
    }
    out.reproduced_in_full = true;
    const auto keep = ddmin(droppable_positions(all), [&](const std::vector<std::size_t>& subset) {
        return judge(with_droppable(all, subset));
    });
    out.minimal = with_droppable(all, keep);
    return out;
}

}  // namespace chaos_ctl::replay
