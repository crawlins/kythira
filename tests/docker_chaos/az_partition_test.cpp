#define BOOST_TEST_MODULE az_partition_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"

#include <thread>

using namespace std::chrono_literals;
using namespace docker_chaos;

static constexpr auto k_election_max = 300ms;

BOOST_AUTO_TEST_SUITE(docker_chaos_az_partition)

// Req 8.6: majority partition (n1+n2) retains leader; minority (n3) makes no progress.
BOOST_FIXTURE_TEST_CASE(majority_partition_continues_progress, ChaosFixture,
                        *boost::unit_test::timeout(120)) {
    auto& n1 = cluster.node(1);
    auto& n2 = cluster.node(2);
    auto& n3 = cluster.node(3);
    auto& pre_partition_leader = cluster.wait_for_leader(10s);
    BOOST_TEST_MESSAGE("pre-partition leader: node " + std::to_string(pre_partition_leader.id()));

    std::string ip1 = n1.container_ip();
    std::string ip2 = n2.container_ip();
    std::string ip3 = n3.container_ip();
    BOOST_TEST_MESSAGE("ip1=" + ip1 + " ip2=" + ip2 + " ip3=" + ip3);

    // Bidirectional partition: isolate n3 from n1 and n2.
    n3.partition_from({ip1, ip2});
    n1.partition_from({ip3});
    n2.partition_from({ip3});

    // Diagnostics: poll every 100ms instead of a single fixed sleep, and
    // log every node's role/reachability each time — this test has failed
    // on a real arm64 runner with "majority partition must have a leader"
    // and the raw job log alone hasn't been enough to tell whether the
    // majority never re-elects, elects but this loop somehow misses it, or
    // something else (e.g. partition_from() itself not taking effect in
    // time) is going on.
    ChaosNode* majority_leader = nullptr;
    auto leader_deadline = std::chrono::steady_clock::now() + k_election_max * 2;
    int attempt = 0;
    while (std::chrono::steady_clock::now() < leader_deadline) {
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
                majority_leader = n;
            }
        }
        if (majority_leader != nullptr) {
            break;
        }
        ++attempt;
        std::this_thread::sleep_for(100ms);
    }
    BOOST_REQUIRE_MESSAGE(majority_leader != nullptr, "majority partition must have a leader");

    for (int i = 0; i < 5; ++i) {
        majority_leader->submit_command("azkey" + std::to_string(i), "azval" + std::to_string(i));
    }
    auto committed = majority_leader->status()["commit_index"].as_int64();

    // Minority must not be able to make progress. Checking is_leader() here
    // would be testing the wrong invariant: n3 was not the pre-partition
    // leader in this run (see BOOST_TEST_MESSAGE above), but in general a
    // node that WAS leader right before losing connectivity keeps
    // believing it still is — Raft leaders only step down on hearing a
    // higher term, which an isolated node can never receive, and this
    // implementation has no separate leadership-lease/self-eviction
    // mechanism for Raft leadership itself (the quorum_check_interval /
    // run_quorum_assessment() machinery in raft.hpp is for the unrelated
    // cloud-provisioning quorum_manager subsystem — it never touches
    // _state). The actual safety guarantee "8.6" needs is that an isolated
    // node cannot COMMIT, regardless of what it locally believes its role
    // is: submitting a command to it must not succeed, since it can never
    // reach a majority to acknowledge the entry.
    auto minority_resp = n3.submit_command("minoritykey", "minorityval");
    BOOST_TEST(!minority_resp.contains("success"),
               "minority node must not be able to commit new entries");

    // Heal all partitions.
    n1.unpartition();
    n2.unpartition();
    n3.unpartition();

    std::this_thread::sleep_for(k_election_max * 2);

    // n3 must catch up.
    auto deadline = std::chrono::steady_clock::now() + 10s;
    bool caught_up = false;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            if (n3.status()["commit_index"].as_int64() >= committed) {
                caught_up = true;
                break;
            }
        } catch (...) {
        }
        std::this_thread::sleep_for(300ms);
    }
    BOOST_TEST(caught_up, "node 3 did not catch up after partition healed");
    cluster.assert_no_split_brain();
}

// Symmetric full partition — no node can form quorum, no leader should exist.
BOOST_FIXTURE_TEST_CASE(symmetric_full_partition_no_leader, ChaosFixture,
                        *boost::unit_test::timeout(120)) {
    auto& n1 = cluster.node(1);
    auto& n2 = cluster.node(2);
    auto& n3 = cluster.node(3);
    cluster.wait_for_leader(10s);

    std::string ip1 = n1.container_ip();
    std::string ip2 = n2.container_ip();
    std::string ip3 = n3.container_ip();

    n1.partition_from({ip2, ip3});
    n2.partition_from({ip1, ip3});
    n3.partition_from({ip1, ip2});

    std::this_thread::sleep_for(k_election_max * 3);

    // No node should be able to make progress without quorum. Same
    // reasoning as majority_partition_continues_progress above: a node
    // that was already leader right before this full symmetric partition
    // took effect keeps locally believing it's leader (it has no way to
    // learn otherwise while fully isolated), so checking is_leader() would
    // be testing the wrong thing. What full partition actually guarantees
    // is that nobody can commit: every node's submit_command() must fail —
    // fast (503 not_leader) for a follower/candidate, or after chaos_node's
    // own 5s internal commit-wait for whichever node still believes itself
    // leader, since it can never reach a majority to acknowledge the entry.
    for (auto* n : cluster.all_nodes()) {
        try {
            auto resp = n->submit_command("fullpartitionkey", "fullpartitionval");
            BOOST_TEST(!resp.contains("success"), "node " + std::to_string(n->id()) +
                                                      " must not be able to commit under "
                                                      "full partition");
        } catch (...) {
        }  // unreachable is acceptable
    }

    // Heal and verify recovery.
    n1.unpartition();
    n2.unpartition();
    n3.unpartition();

    auto& leader = cluster.wait_for_leader(k_election_max * 6);
    leader.submit_command("postpartition", "ok");
    cluster.assert_no_split_brain();
}

BOOST_AUTO_TEST_SUITE_END()
