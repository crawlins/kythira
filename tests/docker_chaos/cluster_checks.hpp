// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// What the cairn checkers need from a running ChaosCluster: heal every
// fault, sample liveness, and read each node's state and elections. Shared
// by the scripted-scenario fixture (chaos_fixture.hpp), which reports
// through Boost.Test, and chaos_ctl (cmd/chaos_ctl/), which reports in its
// JSON results; each passes its own `note` for what it cannot read.

#include "checkers/liveness.hpp"
#include "checkers/raft_safety.hpp"
#include "harness.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace docker_chaos {

using note_fn = std::function<void(const std::string&)>;

/// Undoes every fault a scenario may have left behind, so liveness is
/// measured from a healed cluster. Best effort throughout: a node that is
/// already clean must not stop the others being healed.
inline void heal_all(ChaosCluster& cluster, const note_fn& note) {
    for (auto* n : cluster.all_nodes()) {
        try {
            n->unpause();
        } catch (...) {
        }
        n->clear_tc_netem();
        n->unpartition();
        try {
            n->disable_all_faults();
        } catch (...) {
        }
    }
    for (auto* n : cluster.all_nodes()) {
        if (!n->is_healthy()) {
            try {
                n->restart(true, std::chrono::seconds{30});
            } catch (const std::exception& e) {
                note("heal: node " + std::to_string(n->id()) + " did not come back: " + e.what());
            }
        }
    }
}

/// The highest commit_index any reachable node reports; 0 if none answers.
inline auto highest_commit(ChaosCluster& cluster) -> std::uint64_t {
    std::uint64_t out = 0;
    for (auto* n : cluster.all_nodes()) {
        try {
            out = std::max(out, n->status().at("commit_index").to_number<std::uint64_t>());
        } catch (...) {
        }
    }
    return out;
}

/// One liveness sample: the leader any node reports, and the highest
/// commit_index any reachable node reports. A leader is also given a write,
/// since a cluster with nothing to commit never advances.
inline auto liveness_sample(ChaosCluster& cluster, int& writes) -> checkers::liveness::sample {
    checkers::liveness::sample s;
    for (auto* n : cluster.all_nodes()) {
        try {
            auto st = n->status();
            s.commit_index =
                std::max(s.commit_index, st.at("commit_index").to_number<std::uint64_t>());
            if (st.at("role").as_string() == "leader") {
                s.leader = n->id();
            }
        } catch (...) {
        }
    }
    if (s.leader) {
        try {
            static_cast<void>(
                cluster.node(*s.leader).submit_command("cairn_liveness", std::to_string(++writes)));
        } catch (...) {
        }
    }
    return s;
}

/// One node's /status and whole /log, as node_state_from_json takes them.
/// Throws if the node cannot be read.
inline auto read_node(ChaosNode& n)
    -> std::pair<boost::json::object, std::vector<boost::json::object>> {
    auto status = n.status();
    const auto snapshot = status.at("snapshot_index").to_number<std::uint64_t>();
    const auto last = status.at("last_log_index").to_number<std::uint64_t>();
    std::vector<boost::json::object> ranges;
    // /log serves at most 1024 entries per call.
    for (std::uint64_t from = snapshot + 1; from <= last; from += 1024) {
        const auto to = std::min(last, from + 1023);
        ranges.push_back(n.log_range(from, to));
    }
    return {std::move(status), std::move(ranges)};
}

/// Every node's state, as the Raft safety checker takes it. A node that
/// cannot be read is left out and named. The leader is read last (below).
inline auto collect_node_states(ChaosCluster& cluster, const note_fn& note)
    -> std::vector<checkers::raft_safety::node_state> {
    std::vector<checkers::raft_safety::node_state> out;
    for (auto* n : cluster.all_nodes()) {
        try {
            auto [status, ranges] = read_node(*n);
            out.push_back(checkers::raft_safety::node_state_from_json(status, ranges));
        } catch (const std::exception& e) {
            note("raft_safety: node " + std::to_string(n->id()) + " not read: " + e.what());
        }
    }
    // The nodes are read one after another, not at one instant. While a
    // workload runs, a follower read after the leader can report an entry
    // committed after the leader's read, and leader completeness would
    // then flag the leader's (older) log for ending before it. Reading the
    // leader of the highest term again, after every other node, makes
    // every commit the others report precede the leader's sample.
    std::uint64_t top = 0;
    for (const auto& s : out) {
        top = std::max(top, s.term);
    }
    for (auto& s : out) {
        if (s.role != "leader" || s.term < top) {
            continue;
        }
        try {
            auto [status, ranges] = read_node(cluster.node(s.node));
            s = checkers::raft_safety::node_state_from_json(status, ranges);
        } catch (const std::exception& e) {
            // Unreachable now: its earlier sample can no longer be trusted
            // to postdate the others, so leave it out of the check.
            note("raft_safety: leader " + std::to_string(s.node) + " not re-read: " + e.what());
            s.role = "unknown";
        }
    }
    return out;
}

/// Every election the nodes report now, plus those each ChaosNode saved
/// before a kill or stop, plus `saved`: /elections bodies kept elsewhere
/// (chaos_ctl keeps them on disk, since each call is a new process).
inline auto collect_elections(ChaosCluster& cluster, const std::vector<boost::json::object>& saved,
                              const note_fn& note) -> std::vector<checkers::raft_safety::election> {
    std::vector<checkers::raft_safety::election> out;
    auto add = [&out](const boost::json::object& body) {
        auto e = checkers::raft_safety::elections_from_json(body);
        out.insert(out.end(), e.begin(), e.end());
    };
    for (const auto& body : saved) {
        add(body);
    }
    for (auto* n : cluster.all_nodes()) {
        for (const auto& body : n->remembered_elections()) {
            add(body);
        }
        try {
            add(n->elections());
        } catch (const std::exception& e) {
            note("raft_safety: node " + std::to_string(n->id()) +
                 " /elections not read: " + e.what());
        }
    }
    return out;
}

}  // namespace docker_chaos
