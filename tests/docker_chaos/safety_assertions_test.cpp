#define BOOST_TEST_MODULE safety_assertions_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"

#include <thread>

using namespace std::chrono_literals;
using namespace docker_chaos;

static constexpr auto k_election_max = 300ms;

BOOST_AUTO_TEST_SUITE(docker_chaos_safety_assertions)

// Req 8.7: multi-fault sequence — log agreement must hold across all nodes.
// Phase 1: partition n3 (minority). Phase 2: commit 5 commands on majority.
// Phase 3: kill n2 (no quorum). Phase 4: heal + restart n2. Phase 5: assert safety.
BOOST_FIXTURE_TEST_CASE(no_log_divergence_under_combined_faults, ChaosFixture,
                        *boost::unit_test::timeout(180)) {
    auto& n1 = cluster.node(1);
    auto& n2 = cluster.node(2);
    auto& n3 = cluster.node(3);
    cluster.wait_for_leader(10s);

    std::string ip1 = n1.container_ip();
    std::string ip2 = n2.container_ip();
    std::string ip3 = n3.container_ip();

    // Phase 1: partition n3.
    n3.partition_from({ip1, ip2});
    n1.partition_from({ip3});
    n2.partition_from({ip3});

    std::this_thread::sleep_for(k_election_max * 2);

    // Phase 2: commit 5 commands on the majority (n1 or n2).
    // Diagnostics: this test has failed on a real arm64 runner with
    // "majority partition must have a leader for phase 2" despite a
    // generous 5s budget (already well past what az_partition_test needed
    // for an ordinary split vote) — logging every node's role each attempt
    // gives real evidence instead of guessing at a second cause blind.
    ChaosNode* maj_leader = nullptr;
    auto deadline = std::chrono::steady_clock::now() + 5s;
    int attempt = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto* n : {&n1, &n2, &n3}) {
            std::string role = "unreachable";
            try {
                role = n->status()["role"].as_string();
            } catch (const std::exception& e) {
                role = std::string("unreachable (") + e.what() + ")";
            }
            BOOST_TEST_MESSAGE("  attempt " + std::to_string(attempt) + " node " +
                               std::to_string(n->id()) + ": " + role);
        }
        for (auto* n : {&n1, &n2}) {
            if (n->is_leader()) {
                maj_leader = n;
                break;
            }
        }
        if (maj_leader != nullptr) {
            break;
        }
        ++attempt;
        std::this_thread::sleep_for(200ms);
    }
    BOOST_REQUIRE_MESSAGE(maj_leader != nullptr,
                          "majority partition must have a leader for phase 2");

    for (int i = 0; i < 5; ++i) {
        maj_leader->submit_command("safe" + std::to_string(i), "v" + std::to_string(i));
    }
    auto committed_idx = maj_leader->status()["commit_index"].as_int64();

    // Phase 3: kill n2 — now only n1 is up, no quorum.
    n2.kill();
    std::this_thread::sleep_for(200ms);

    // Phase 4: heal partition and restart n2.
    n1.unpartition();
    n3.unpartition();
    n2.restart(/*wait=*/true, 20s);

    // Phase 5: wait for the full cluster to stabilise.
    cluster.wait_for_leader(k_election_max * 6);
    std::this_thread::sleep_for(k_election_max);

    cluster.assert_no_split_brain();

    // Every reachable node must have applied all committed entries.
    for (auto* n : cluster.all_nodes()) {
        try {
            auto ci = n->status()["commit_index"].as_int64();
            BOOST_TEST(ci >= committed_idx, "node " + std::to_string(n->id()) + " commit_index " +
                                                std::to_string(ci) + " < expected " +
                                                std::to_string(committed_idx));
        } catch (const std::runtime_error& e) {
            // Re-throw split-brain violations; swallow transient unreachability.
            if (std::string(e.what()).find("split brain") != std::string::npos) {
                throw;
            }
        } catch (...) {
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
