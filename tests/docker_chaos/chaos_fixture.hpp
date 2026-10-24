// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// ChaosFixture: the per-test-case fixture of the scripted docker_chaos
// scenarios. It starts a fresh 3-node cluster and stops it afterwards, and
// (cairn-chaos-testing task 6, Requirement 5.4) runs the cairn checkers over
// every case:
//
//   - While the case runs, a chaos_workload drives reads, writes and CAS
//     from the test process against keys the scenarios never use, and
//     records a history.
//   - When the case ends, every fault is healed, then the liveness checker
//     waits for a leader and a commit, the Raft safety checker reads every
//     node's /status, /log and /elections (plus the elections saved before
//     each kill or stop), and the linearizability checker runs over the
//     history.
//
// A scripted scenario is known-good, so any flag here is a checker or
// harness bug until shown otherwise. KYTHIRA_CHAOS_CHECKERS=0 turns the
// whole thing off; KYTHIRA_CHAOS_SEED fixes the workload's seed;
// KYTHIRA_CHAOS_ARTIFACT_DIR, if set, receives each case's history.

#include "checkers/linearizability.hpp"
#include "checkers/liveness.hpp"
#include "checkers/raft_safety.hpp"
#include "cluster_checks.hpp"
#include "harness.hpp"
#include "history.hpp"
#include "http_transport.hpp"
#include "runner.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace docker_chaos {

/// ELECTION_TIMEOUT_MAX_MS in docker/docker-compose.yml.
inline constexpr std::chrono::milliseconds k_compose_election_timeout_max{300};

/// Recovery bound for the scripted scenarios, in maximum election timeouts.
/// Twice liveness::default_k: healing restarts containers and flushes
/// iptables on a shared CI runner, often under rootless Podman, and the
/// first elections after that contend with the restarted node's own
/// timer. A finding here should be a stuck cluster, not a slow runner.
inline constexpr unsigned k_scripted_liveness_k = 2 * checkers::liveness::default_k;

/// The workload, run on threads of the test process for the length of a
/// case. Talks to the nodes through their published HTTP ports.
class background_workload {
public:
    background_workload(std::map<int, std::string> nodes, chaos_workload::run_options opts)
        : _start(std::chrono::steady_clock::now()) {
        _runner = std::thread([this, nodes = std::move(nodes), opts = std::move(opts)] {
            try {
                chaos_workload::run(
                    nodes,
                    [] { return chaos_workload::http_transport(std::chrono::milliseconds{2000}); },
                    opts, [this] { return _stop.load(); },
                    [this](const history::event& e) {
                        std::lock_guard<std::mutex> lock(_mutex);
                        _events.push_back(e);
                    },
                    _start);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(_mutex);
                _error = e.what();
            }
        });
    }

    ~background_workload() { static_cast<void>(stop()); }

    background_workload(const background_workload&) = delete;
    background_workload& operator=(const background_workload&) = delete;

    /// Stops the workers and returns the history. Each worker finishes the
    /// operation it has in flight, so this can take one request timeout.
    auto stop() -> std::vector<history::event> {
        _stop = true;
        if (_runner.joinable()) {
            _runner.join();
        }
        std::lock_guard<std::mutex> lock(_mutex);
        return _events;
    }

    [[nodiscard]] auto error() -> std::string {
        std::lock_guard<std::mutex> lock(_mutex);
        return _error;
    }

private:
    std::chrono::steady_clock::time_point _start;
    std::atomic<bool> _stop{false};
    std::mutex _mutex;
    std::vector<history::event> _events;
    std::string _error;
    std::thread _runner;
};

namespace fixture_detail {

inline auto checkers_enabled() -> bool {
    const char* v = std::getenv("KYTHIRA_CHAOS_CHECKERS");
    return v == nullptr || std::string(v) != "0";
}

inline auto workload_seed() -> std::uint64_t {
    const char* v = std::getenv("KYTHIRA_CHAOS_SEED");
    if (v != nullptr && *v != 0) {
        return std::stoull(v);
    }
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

inline auto case_name() -> std::string {
    return boost::unit_test::framework::current_test_case().p_name;
}

}  // namespace fixture_detail

/// The cluster_checks.hpp helpers, reporting through Boost.Test.
inline void fixture_note(const std::string& msg) {
    BOOST_TEST_MESSAGE(msg);
}

/// The three checkers over one finished case, reported through Boost.Test.
inline void check_invariants(ChaosCluster& cluster, const std::vector<history::event>& history) {
    const auto name = fixture_detail::case_name();

    if (const char* dir = std::getenv("KYTHIRA_CHAOS_ARTIFACT_DIR"); dir != nullptr && *dir != 0) {
        std::ofstream out(std::string(dir) + "/" + name + ".history.jsonl");
        for (const auto& e : history) {
            out << history::to_line(e) << '\n';
        }
    }

    heal_all(cluster, fixture_note);

    int writes = 0;
    const std::uint64_t baseline = highest_commit(cluster);
    const auto bound =
        checkers::liveness::bound(k_compose_election_timeout_max, k_scripted_liveness_k);
    auto live = checkers::liveness::wait(
        [&cluster, &writes] { return liveness_sample(cluster, writes); }, baseline, bound);
    BOOST_TEST_MESSAGE("liveness: " + std::string(checkers::to_string(live.overall)) +
                       " (leader at " +
                       (live.leader_at ? std::to_string(live.leader_at->count()) + "ms" : "never") +
                       ", commit at " +
                       (live.commit_at ? std::to_string(live.commit_at->count()) + "ms" : "never") +
                       ", bound " + std::to_string(bound.count()) + "ms)");
    BOOST_TEST(live.overall == checkers::verdict::ok, "liveness after heal: " << live.detail);

    auto safety = checkers::raft_safety::check(collect_node_states(cluster, fixture_note),
                                               collect_elections(cluster, {}, fixture_note));
    BOOST_TEST_MESSAGE("raft_safety: " + std::string(checkers::to_string(safety.overall)) + ", " +
                       std::to_string(safety.terms_with_a_leader) + " terms with a leader, " +
                       std::to_string(safety.committed_entries_compared) +
                       " committed entries compared");
    for (const auto& v : safety.violations) {
        BOOST_ERROR("raft_safety violation: "
                    << boost::json::serialize(checkers::raft_safety::violation_json(v)));
    }

    const auto structural = history::structural_error(history);
    BOOST_TEST(structural.empty(), "workload history malformed: " << structural);
    if (!structural.empty()) {
        return;
    }
    auto lin = checkers::linearizability::check(history);
    std::size_t ops = 0;
    for (const auto& k : lin.keys) {
        ops += k.operations;
    }
    BOOST_TEST_MESSAGE("linearizability: " + std::string(checkers::to_string(lin.overall)) + ", " +
                       std::to_string(history.size()) + " events, " + std::to_string(ops) +
                       " checked operations over " + std::to_string(lin.keys.size()) + " keys, " +
                       std::to_string(lin.failed_operations) + " failed, " +
                       std::to_string(lin.local_reads.size()) + " local reads");
    for (const auto* k : lin.violations()) {
        BOOST_ERROR("linearizability violation: "
                    << boost::json::serialize(checkers::linearizability::violation_json(*k)));
    }
    if (lin.overall == checkers::verdict::unknown) {
        // Not a pass: the search gave up. Reported, not failed, because a
        // budget is a property of the checker, not of the cluster.
        BOOST_TEST_MESSAGE("linearizability: search budget exhausted, result unknown");
    }
}

// Per-test-case fixture: starts a fresh cluster in the constructor, runs the
// checkers in teardown(), stops the cluster in the destructor.
struct ChaosFixture {
    ChaosCluster cluster;
    std::optional<background_workload> workload;

    ChaosFixture() : cluster(default_compose_file(), std::chrono::seconds{30}) {
        cluster.start();
        cluster.wait_for_leader(std::chrono::seconds{15});
        if (fixture_detail::checkers_enabled()) {
            std::map<int, std::string> nodes;
            for (auto* n : cluster.all_nodes()) {
                nodes[n->id()] = "localhost:" + std::to_string(n->http_port());
            }
            const auto seed = fixture_detail::workload_seed();
            BOOST_TEST_MESSAGE("cairn workload seed " + std::to_string(seed));
            // Light enough not to change what the scenario measures; keys
            // the scenarios never touch.
            workload.emplace(std::move(nodes),
                             chaos_workload::run_options{.keys = 3,
                                                         .values = 5,
                                                         .concurrency = 3,
                                                         .rate = 30,
                                                         .weights = {},
                                                         .seed = seed,
                                                         .key_prefix = "cairn_k"});
        }
    }

    // Boost.Test calls this after a case body that did not throw; assertions
    // here count against the case.
    void teardown() {
        if (!workload) {
            return;
        }
        auto history = workload->stop();
        auto error = workload->error();
        workload.reset();
        BOOST_TEST(error.empty(), "workload stopped with an error: " << error);
        check_invariants(cluster, history);
    }

    ~ChaosFixture() {
        workload.reset();  // join the workers before their cluster goes away
        try {
            cluster.stop();
        } catch (...) {
        }
    }

    ChaosFixture(const ChaosFixture&) = delete;
    ChaosFixture& operator=(const ChaosFixture&) = delete;
};

}  // namespace docker_chaos
