#define BOOST_TEST_MODULE network_degradation_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"

#include <thread>

using namespace std::chrono_literals;
using namespace docker_chaos;

static constexpr auto k_heartbeat = 50ms;

BOOST_AUTO_TEST_SUITE(docker_chaos_network_degradation)

// Req 8.4: apply 30% packet loss to a follower, submit 10 commands, remove, verify commit.
BOOST_FIXTURE_TEST_CASE(tc_netem_packet_loss, ChaosFixture, *boost::unit_test::timeout(120)) {
    auto& leader = cluster.wait_for_leader(10s);

    ChaosNode* follower = nullptr;
    for (auto* n : cluster.all_nodes()) {
        if (n->id() != leader.id()) {
            follower = n;
            break;
        }
    }
    BOOST_REQUIRE(follower != nullptr);

    follower->apply_tc_netem("30%");
    std::this_thread::sleep_for(100ms);

    for (int i = 0; i < 10; ++i) {
        leader.submit_command("losskey" + std::to_string(i), "lossval" + std::to_string(i));
    }

    follower->clear_tc_netem();

    auto deadline = std::chrono::steady_clock::now() + k_heartbeat * 10 + 5s;
    bool all_committed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            if (leader.status()["commit_index"].as_int64() >= 10) {
                all_committed = true;
                break;
            }
        } catch (...) {
        }
        std::this_thread::sleep_for(200ms);
    }
    BOOST_TEST(all_committed, "cluster commit_index did not reach 10 after netem cleared");
    cluster.assert_no_split_brain();
}

BOOST_FIXTURE_TEST_CASE(tc_netem_high_latency, ChaosFixture, *boost::unit_test::timeout(120)) {
    auto& leader = cluster.wait_for_leader(10s);

    ChaosNode* follower = nullptr;
    for (auto* n : cluster.all_nodes()) {
        if (n->id() != leader.id()) {
            follower = n;
            break;
        }
    }
    BOOST_REQUIRE(follower != nullptr);

    follower->apply_tc_netem("0%", "200ms");
    leader.submit_command("latkey", "latval");

    follower->clear_tc_netem();
    BOOST_TEST(leader.status()["commit_index"].as_int64() >= 1,
               "leader made no progress under 200ms latency");
    cluster.assert_no_split_brain();
}

BOOST_AUTO_TEST_SUITE_END()
