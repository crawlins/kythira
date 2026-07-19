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

    // Minority must not claim leadership.
    BOOST_TEST(!n3.is_leader(), "minority node must not claim leadership");

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

    // No node should be able to claim leadership without quorum.
    for (auto* n : cluster.all_nodes()) {
        try {
            BOOST_TEST(!n->is_leader(), "node " + std::to_string(n->id()) +
                                            " claimed leadership under full partition");
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
