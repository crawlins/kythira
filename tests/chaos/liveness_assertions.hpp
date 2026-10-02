// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Progress checks for the chaos liveness tests (Requirement 6) and the
// committed-entry check in leader completeness (Requirement 5.3).
//
// The safety assertions in safety_assertions.hpp only say that nothing went
// wrong; every one of them passes on a cluster that never commits anything.
// These helpers say that something went right: a command committed, a node
// applied up to an index, a leader holds an entry. Each failure message names
// every node's term, role, commit and apply position, plus the fault points
// the scenario enabled.

#include "fault_profiles.hpp"

#include <raft/raft.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace kythira::chaos {

// "node id 1: term 2 leader commit 3 applied 3 log 3; node id 2: ...". Uses
// node ids, not vector positions, because callers pass subsets of a cluster.
template<typename Types>
auto describe_nodes(const std::vector<kythira::node<Types>*>& nodes) -> std::string {
    std::string out;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        auto s = nodes[i]->debug_state();
        if (i != 0) {
            out += "; ";
        }
        out += std::format("node id {}: term {} {} commit {} applied {} log {}",
                           nodes[i]->get_node_id(), s.current_term,
                           s.is_leader ? "leader" : "non-leader", s.commit_index, s.last_applied,
                           s.log.size());
    }
    return out;
}

// Poll `done` until it holds or `budget` elapses, calling `drive` (typically
// a leader's check_heartbeat_timeout) every `step` in between. The chaos
// nodes have no timer thread of their own, so nothing replicates unless the
// test drives it.
template<typename Drive, typename Done>
auto drive_until(std::chrono::milliseconds budget, std::chrono::milliseconds step, Drive&& drive,
                 Done&& done) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        drive();
        std::this_thread::sleep_for(step);
    }
    return true;
}

// Drive `candidate`'s election timer until it wins, or fail the test.
template<typename Types>
void require_elected(kythira::node<Types>& candidate,
                     const std::vector<kythira::node<Types>*>& nodes,
                     std::chrono::milliseconds budget, std::chrono::milliseconds step,
                     std::string_view what) {
    const bool elected = drive_until(
        budget, step, [&] { candidate.check_election_timeout(); },
        [&] { return candidate.is_leader(); });
    if (!elected) {
        BOOST_FAIL(std::format("{}: node {} was not elected within {} ms; {}; {}", what,
                               candidate.get_node_id(), budget.count(), describe_nodes(nodes),
                               describe_fault_points()));
    }
}

// The node, if any, that believes it leads the highest term.
template<typename Types>
auto current_leader(const std::vector<kythira::node<Types>*>& nodes) -> kythira::node<Types>* {
    kythira::node<Types>* leader = nullptr;
    typename Types::term_id_type leader_term{};
    for (auto* n : nodes) {
        auto s = n->debug_state();
        if (s.is_leader && (leader == nullptr || s.current_term > leader_term)) {
            leader = n;
            leader_term = s.current_term;
        }
    }
    return leader;
}

// Index of the last entry in `n`'s log, or 0 for an empty log.
template<typename Types>
auto last_log_index(const kythira::node<Types>& n) -> typename Types::log_index_type {
    auto s = n.debug_state();
    return s.log.empty() ? typename Types::log_index_type{0} : s.log.back().index();
}

// The entry at `index` in `n`'s log, copied out, or nullopt if absent.
template<typename Types>
auto entry_at(const kythira::node<Types>& n, typename Types::log_index_type index)
    -> std::optional<typename Types::log_entry_type> {
    auto s = n.debug_state();
    for (const auto& e : s.log) {
        if (e.index() == index) {
            return e;
        }
    }
    return std::nullopt;
}

// Wait for a submit_command future while driving `leader`, and fail the test
// if it times out or resolves with an exception. This is the check the old
// tests skipped: they wrapped submit_command in try/catch(...), but it reports
// failure through the returned future, which they discarded.
template<typename Types, typename Future>
void require_command_succeeds(kythira::node<Types>& leader, Future& result,
                              std::chrono::milliseconds budget, std::chrono::milliseconds step,
                              const std::vector<kythira::node<Types>*>& nodes,
                              std::string_view what) {
    const bool ready = drive_until(
        budget, step, [&] { leader.check_heartbeat_timeout(); },
        [&] { return result.wait(std::chrono::milliseconds{1}); });
    if (!ready) {
        BOOST_FAIL(std::format("{}: command did not complete within {} ms; {}; {}", what,
                               budget.count(), describe_nodes(nodes), describe_fault_points()));
    }
    try {
        std::ignore = std::move(result).get();
    } catch (const std::exception& e) {
        BOOST_FAIL(std::format("{}: command failed: {}; {}; {}", what, e.what(),
                               describe_nodes(nodes), describe_fault_points()));
    } catch (...) {
        BOOST_FAIL(std::format("{}: command failed with a non-std exception; {}; {}", what,
                               describe_nodes(nodes), describe_fault_points()));
    }
}

// Require every node in `nodes` to apply through `index` within `budget`,
// driving `leader` meanwhile. Followers learn a new commit index only from
// the leader's next AppendEntries, so they lag the leader by a heartbeat.
template<typename Types>
void require_applied_through(kythira::node<Types>& leader,
                             const std::vector<kythira::node<Types>*>& nodes,
                             typename Types::log_index_type index, std::chrono::milliseconds budget,
                             std::chrono::milliseconds step, std::string_view what) {
    const bool applied = drive_until(
        budget, step, [&] { leader.check_heartbeat_timeout(); },
        [&] {
            for (auto* n : nodes) {
                if (n->debug_state().last_applied < index) {
                    return false;
                }
            }
            return true;
        });
    if (!applied) {
        BOOST_FAIL(std::format("{}: not every node applied through index {} within {} ms; {}; {}",
                               what, index, budget.count(), describe_nodes(nodes),
                               describe_fault_points()));
    }
}

}  // namespace kythira::chaos
