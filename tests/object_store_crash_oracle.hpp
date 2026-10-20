// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file object_store_crash_oracle.hpp
/// @brief What a fresh engine may legally load after a crash part-way through
///        one operation — the oracle the crash-injection cases of
///        `.kiro/specs/object-store-batched-layout/` assert against.
///
/// The crash cases arm `mock_object_store::crash_after_requests(n)` for every n
/// from 0 to an operation's request count, run the operation, clear the crash,
/// and load a fresh engine. What that engine reads must be one of the states the
/// interrupted call's contract permits (Requirement 15.3). This header turns
/// "the contract permits" into a finite set: given the operation and the model
/// before and after an uninterrupted call, `legal_loaded_states` lists every
/// state recovery may produce.
///
/// The model is deliberately the engine's *observable* state — term, vote, and
/// the log as `get_log_entries` returns it — and not the bucket. Two layouts
/// that store the same state differently must satisfy the same oracle; that is
/// what makes it an oracle rather than a second implementation.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace kythira::crash_oracle {

/// One log entry as recovery must reproduce it: everything `log_entry` carries,
/// comparable.
struct entry_fact {
    std::uint64_t term{0};
    std::vector<std::byte> command;
    int type{0};

    auto operator==(const entry_fact&) const -> bool = default;
};

/// The observable persistent state, minus the snapshot (which no operation
/// here touches).
template<typename NodeId = std::uint64_t> struct model {
    std::uint64_t term{0};
    std::optional<NodeId> vote;
    std::map<std::uint64_t, entry_fact> log;

    auto operator==(const model&) const -> bool = default;
};

/// Read `eng`'s mirror into a model. Reads only — no request reaches the store.
template<typename NodeId = std::uint64_t, typename Engine>
auto capture(Engine& eng) -> model<NodeId> {
    model<NodeId> out;
    out.term = static_cast<std::uint64_t>(eng.load_current_term());
    out.vote = eng.load_voted_for();
    for (const auto& e : eng.get_log_entries(0, std::numeric_limits<std::uint64_t>::max())) {
        out.log[static_cast<std::uint64_t>(e.index())] = entry_fact{
            static_cast<std::uint64_t>(e.term()), e.command(), static_cast<int>(e.type())};
    }
    return out;
}

/// The operation that was interrupted, by the property its crash states have.
enum class operation {
    /// One request carries term and vote together (`hard_state_layout::combined`):
    /// the old pair or the new pair, nothing between.
    atomic_hard_state,
    /// Term, then vote, as two requests (`hard_state_layout::separate`): the
    /// `hard_state_persistence_engine` contract's third state — new term, old
    /// vote — is also legal; old term with new vote never is.
    term_then_vote,
    /// A run appended above the last index: the pre-call log plus any prefix
    /// of the run. (Any subset may land under concurrency; recovery keeps only
    /// the contiguous part, which is a prefix.)
    extend_log,
    /// `truncate_log`: any prefix of the pre-call log no shorter than the
    /// post-call log, because objects go highest first.
    truncate_log,
    /// `delete_log_entries_before`: any suffix of the pre-call log no shorter
    /// than the post-call log, because objects go lowest first.
    compact_log,
};

/// Every state a fresh engine may load after `op` was interrupted at any
/// request, given the state before the call and after an uninterrupted one.
/// Always contains `pre` and `post`.
template<typename NodeId>
auto legal_loaded_states(operation op, const model<NodeId>& pre, const model<NodeId>& post)
    -> std::vector<model<NodeId>> {
    std::vector<model<NodeId>> out;
    const auto add = [&out](model<NodeId> m) {
        for (const auto& seen : out) {
            if (seen == m) {
                return;
            }
        }
        out.push_back(std::move(m));
    };
    add(pre);
    switch (op) {
        case operation::atomic_hard_state:
            break;
        case operation::term_then_vote: {
            model<NodeId> torn = pre;
            torn.term = post.term;
            add(torn);
            break;
        }
        case operation::extend_log: {
            model<NodeId> grown = pre;
            for (auto it = post.log.begin(); it != post.log.end(); ++it) {
                if (pre.log.contains(it->first)) {
                    continue;
                }
                grown.log[it->first] = it->second;
                add(grown);
            }
            break;
        }
        case operation::truncate_log: {
            model<NodeId> shrinking = pre;
            while (!shrinking.log.empty() && shrinking.log.size() > post.log.size()) {
                shrinking.log.erase(std::prev(shrinking.log.end()));
                add(shrinking);
            }
            break;
        }
        case operation::compact_log: {
            model<NodeId> shrinking = pre;
            while (!shrinking.log.empty() && shrinking.log.size() > post.log.size()) {
                shrinking.log.erase(shrinking.log.begin());
                add(shrinking);
            }
            break;
        }
    }
    add(post);
    return out;
}

/// Whether `loaded` is one of `legal`.
template<typename NodeId>
auto is_legal(const model<NodeId>& loaded, const std::vector<model<NodeId>>& legal) -> bool {
    for (const auto& m : legal) {
        if (m == loaded) {
            return true;
        }
    }
    return false;
}

/// For a failing assertion's message: term, vote and the log's index range.
template<typename NodeId> auto describe(const model<NodeId>& m) -> std::string {
    std::string out = "term " + std::to_string(m.term) + ", vote ";
    if (m.vote) {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            out += *m.vote;
        } else {
            out += std::to_string(static_cast<unsigned long long>(*m.vote));
        }
    } else {
        out += "none";
    }
    out += ", log ";
    if (m.log.empty()) {
        out += "empty";
    } else {
        out += "[" + std::to_string(m.log.begin()->first) + ".." +
               std::to_string(m.log.rbegin()->first) + "] (" + std::to_string(m.log.size()) +
               " entries)";
    }
    return out;
}

}  // namespace kythira::crash_oracle
