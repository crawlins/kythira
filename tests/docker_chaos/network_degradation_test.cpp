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

    // Diagnostics: this test has failed on a real arm64 runner with
    // "cluster commit_index did not reach 10 after netem cleared", and
    // none of the 10 submit_command() responses below were being checked
    // — if even one silently failed (e.g. a transient "not_leader" or
    // chaos_node's 5s internal commit-wait timing out), commit_index would
    // permanently fall short of 10 with no visibility into why.
    int successes = 0;
    for (int i = 0; i < 10; ++i) {
        auto resp =
            leader.submit_command("losskey" + std::to_string(i), "lossval" + std::to_string(i));
        bool ok = resp.contains("success");
        if (ok) {
            ++successes;
        } else {
            BOOST_TEST_MESSAGE("submit_command " + std::to_string(i) +
                               " failed: " + boost::json::serialize(resp));
        }
    }
    BOOST_TEST_MESSAGE(std::to_string(successes) + "/10 submit_command calls reported success");

    follower->clear_tc_netem();

    auto deadline = std::chrono::steady_clock::now() + k_heartbeat * 10 + 5s;
    bool all_committed = false;
    std::int64_t last_seen_commit_index = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            last_seen_commit_index = leader.status()["commit_index"].as_int64();
            if (last_seen_commit_index >= 10) {
                all_committed = true;
                break;
            }
        } catch (const std::exception& e) {
            BOOST_TEST_MESSAGE(std::string("leader.status() failed: ") + e.what());
        }
        std::this_thread::sleep_for(200ms);
    }
    BOOST_TEST_MESSAGE("final commit_index seen: " + std::to_string(last_seen_commit_index));
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
