// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Raft safety checker over the state chaos_node's control plane reports
// (.kiro/specs/cairn-chaos-testing/ Requirement 5.2, design §2.3). Four
// invariants from the Raft paper's Figure 3, each checked against what the
// nodes can actually show:
//
//   election safety      at most one leader per term, from every node's
//                        /elections: each node records every term it won,
//                        so this is exact over everything the caller kept,
//                        not a sample.
//   log matching         if two logs hold an entry with the same index and
//                        term, the entries are identical and so are all the
//                        entries before it that both still hold.
//   state-machine safety no two nodes have committed different entries at
//                        the same index (a node's log up to its own
//                        commit_index is what it has applied or will apply).
//   leader completeness  the leader of the highest sampled term holds every
//                        committed entry, at the same index and term, unless it has
//                        compacted that index into a snapshot.
//
// Leader completeness is about leaders at election time; the control plane
// only shows a leader's log as it is now. A leader's log never drops a
// committed entry once it holds it, so checking the leader of the highest
// sampled term at every sample point is the observable form of the property.
//
// Entries a node has compacted are absent from its /log and are skipped,
// never treated as a mismatch. A follower that holds an uncommitted entry
// from a deposed leader at an index another node has committed is legal
// until the follower catches up, so only committed prefixes are compared
// across nodes for state-machine safety.

#include "linearizability.hpp"  // verdict

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace docker_chaos::checkers::raft_safety {

struct log_entry {
    std::uint64_t index{0};
    std::uint64_t term{0};
    std::string type;
    std::string command_b64;

    auto operator==(const log_entry&) const -> bool = default;
};

struct election {
    int node{0};
    std::uint64_t term{0};
    std::int64_t at_ms{0};
};

/// One node's state at one sample point.
struct node_state {
    int node{0};
    std::string role;
    std::uint64_t term{0};
    std::uint64_t commit_index{0};
    std::uint64_t snapshot_index{0};
    /// Entries the node still holds, by index. Need not be contiguous: the
    /// caller may fetch only a range.
    std::map<std::uint64_t, log_entry> log;
};

struct violation {
    std::string invariant;  // election_safety, log_matching, state_machine_safety,
                            // leader_completeness
    std::string detail;
    std::vector<int> nodes;
    std::optional<std::uint64_t> index;
    std::optional<std::uint64_t> term;
};

struct result {
    verdict overall{verdict::ok};
    std::vector<violation> violations;
    std::size_t terms_with_a_leader{0};
    std::uint64_t committed_entries_compared{0};
};

namespace detail {

inline auto node_id(const boost::json::value& v) -> int {
    if (v.is_string()) {
        return std::stoi(std::string(v.as_string()));
    }
    return static_cast<int>(v.to_number<std::int64_t>());
}

inline auto u64(const boost::json::object& o, std::string_view key) -> std::uint64_t {
    return o.at(key).to_number<std::uint64_t>();
}

inline void add(result& r, violation v) {
    r.overall = verdict::violation;
    r.violations.push_back(std::move(v));
}

}  // namespace detail

// ── Adapters from the control-plane JSON ─────────────────────────────────────

/// GET /elections → {node_id, elections:[{term, at_ms}]}.
[[nodiscard]] inline auto elections_from_json(const boost::json::object& body)
    -> std::vector<election> {
    std::vector<election> out;
    const int node = detail::node_id(body.at("node_id"));
    for (const auto& e : body.at("elections").as_array()) {
        const auto& o = e.as_object();
        out.push_back(election{.node = node,
                               .term = detail::u64(o, "term"),
                               .at_ms = o.at("at_ms").to_number<std::int64_t>()});
    }
    return out;
}

/// GET /status plus any number of GET /log?from=&to= bodies from the same
/// node, taken together.
[[nodiscard]] inline auto node_state_from_json(const boost::json::object& status,
                                               const std::vector<boost::json::object>& log_ranges)
    -> node_state {
    node_state s;
    s.node = detail::node_id(status.at("node_id"));
    s.role = std::string(status.at("role").as_string());
    s.term = detail::u64(status, "term");
    s.commit_index = detail::u64(status, "commit_index");
    s.snapshot_index = detail::u64(status, "snapshot_index");
    for (const auto& range : log_ranges) {
        s.snapshot_index = std::max(s.snapshot_index, detail::u64(range, "snapshot_index"));
        for (const auto& e : range.at("entries").as_array()) {
            const auto& o = e.as_object();
            log_entry entry{.index = detail::u64(o, "index"),
                            .term = detail::u64(o, "term"),
                            .type = std::string(o.at("type").as_string()),
                            .command_b64 = std::string(o.at("command_b64").as_string())};
            s.log[entry.index] = std::move(entry);
        }
    }
    return s;
}

// ── The invariants ───────────────────────────────────────────────────────────

/// At most one leader per term. The same node recorded twice for a term is
/// one election seen through two reads, not a violation.
inline void check_election_safety(const std::vector<election>& elections, result& r) {
    std::map<std::uint64_t, std::set<int>> winners;
    for (const auto& e : elections) {
        winners[e.term].insert(e.node);
    }
    r.terms_with_a_leader = winners.size();
    for (const auto& [term, nodes] : winners) {
        if (nodes.size() > 1) {
            detail::add(r, violation{.invariant = "election_safety",
                                     .detail = std::to_string(nodes.size()) + " nodes won term " +
                                               std::to_string(term),
                                     .nodes = {nodes.begin(), nodes.end()},
                                     .index = std::nullopt,
                                     .term = term});
        }
    }
}

/// Log matching over every pair of nodes, on the indices both still hold.
inline void check_log_matching(const std::vector<node_state>& nodes, result& r) {
    for (std::size_t a = 0; a < nodes.size(); ++a) {
        for (std::size_t b = a + 1; b < nodes.size(); ++b) {
            const auto& la = nodes[a].log;
            const auto& lb = nodes[b].log;
            // Highest common index where the terms differ so far: no entry
            // above it may share a term while one at or below it differs.
            std::optional<std::uint64_t> last_term_mismatch;
            for (const auto& [index, ea] : la) {
                auto it = lb.find(index);
                if (it == lb.end()) {
                    continue;
                }
                const auto& eb = it->second;
                if (ea.term != eb.term) {
                    last_term_mismatch = index;
                    continue;
                }
                const std::vector<int> pair{nodes[a].node, nodes[b].node};
                if (ea != eb) {
                    detail::add(r, violation{.invariant = "log_matching",
                                             .detail = "same index and term, different entry",
                                             .nodes = pair,
                                             .index = index,
                                             .term = ea.term});
                } else if (last_term_mismatch) {
                    detail::add(r, violation{.invariant = "log_matching",
                                             .detail = "entries agree at this index and term but "
                                                       "disagree on the term at earlier index " +
                                                       std::to_string(*last_term_mismatch),
                                             .nodes = pair,
                                             .index = index,
                                             .term = ea.term});
                    last_term_mismatch.reset();  // one report per divergence
                }
            }
        }
    }
}

/// The committed entry at each index as some node holds it, after checking
/// that no two nodes committed different entries there.
inline auto check_state_machine_safety(const std::vector<node_state>& nodes, result& r)
    -> std::map<std::uint64_t, std::pair<int, log_entry>> {
    std::map<std::uint64_t, std::pair<int, log_entry>> committed;
    std::set<std::uint64_t> reported;
    for (const auto& n : nodes) {
        for (const auto& [index, e] : n.log) {
            if (index > n.commit_index) {
                break;
            }
            auto [it, inserted] = committed.emplace(index, std::make_pair(n.node, e));
            if (!inserted && it->second.second != e && reported.insert(index).second) {
                detail::add(r, violation{.invariant = "state_machine_safety",
                                         .detail = "nodes committed different entries (terms " +
                                                   std::to_string(it->second.second.term) +
                                                   " and " + std::to_string(e.term) + ")",
                                         .nodes = {it->second.first, n.node},
                                         .index = index,
                                         .term = std::nullopt});
            }
        }
    }
    r.committed_entries_compared = committed.size();
    return committed;
}

inline void check_leader_completeness(
    const std::vector<node_state>& nodes,
    const std::map<std::uint64_t, std::pair<int, log_entry>>& committed, result& r) {
    std::uint64_t max_term = 0;
    for (const auto& n : nodes) {
        max_term = std::max(max_term, n.term);
    }
    for (const auto& n : nodes) {
        // A leader behind the highest sampled term is deposed and has not
        // heard yet (a paused leader, say); entries committed since need not
        // be in its log. The leader of the highest term must hold them all.
        if (n.role != "leader" || n.term < max_term) {
            continue;
        }
        for (const auto& [index, held] : committed) {
            const auto& [from, e] = held;
            if (index <= n.snapshot_index) {
                continue;
            }
            auto it = n.log.find(index);
            if (it == n.log.end()) {
                // The caller fetched a range; an index outside it says
                // nothing. Only an index past the leader's last entry is
                // known to be missing.
                if (!n.log.empty() && index > n.log.rbegin()->first) {
                    detail::add(r, violation{.invariant = "leader_completeness",
                                             .detail = "leader's log ends before a committed "
                                                       "entry",
                                             .nodes = {n.node, from},
                                             .index = index,
                                             .term = e.term});
                    break;
                }
                continue;
            }
            if (it->second != e) {
                detail::add(
                    r,
                    violation{.invariant = "leader_completeness",
                              .detail = "leader holds term " + std::to_string(it->second.term) +
                                        " where term " + std::to_string(e.term) + " was committed",
                              .nodes = {n.node, from},
                              .index = index,
                              .term = e.term});
            }
        }
    }
}

/// Every invariant over one sample of every node, plus the elections the
/// caller has accumulated over the run (a node's /elections is lost when it
/// restarts, so the caller reads it before each kill and keeps the union).
[[nodiscard]] inline auto check(const std::vector<node_state>& nodes,
                                const std::vector<election>& elections) -> result {
    result r;
    check_election_safety(elections, r);
    check_log_matching(nodes, r);
    auto committed = check_state_machine_safety(nodes, r);
    check_leader_completeness(nodes, committed, r);
    return r;
}

/// The violation record (Requirement 5.5) minus the schedule-log offset,
/// which the caller that owns the schedule adds.
[[nodiscard]] inline auto violation_json(const violation& v) -> boost::json::object {
    boost::json::array nodes;
    for (int n : v.nodes) {
        nodes.push_back(n);
    }
    boost::json::object out{
        {"invariant", v.invariant},
        {"detail", v.detail},
        {"nodes", std::move(nodes)},
    };
    if (v.index) {
        out["index"] = *v.index;
    }
    if (v.term) {
        out["term"] = *v.term;
    }
    return out;
}

}  // namespace docker_chaos::checkers::raft_safety
