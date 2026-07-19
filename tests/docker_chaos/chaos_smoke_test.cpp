#define BOOST_TEST_MODULE chaos_smoke_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"

using namespace std::chrono_literals;
using namespace docker_chaos;

BOOST_AUTO_TEST_SUITE(docker_chaos_smoke)

BOOST_AUTO_TEST_CASE(cluster_starts_elects_leader_accepts_command, *boost::unit_test::timeout(60)) {
    ChaosCluster cluster(default_compose_file(), 30s);
    cluster.start();
    BOOST_TEST_MESSAGE("cluster.start() returned (all nodes healthy)");

    auto& leader = cluster.wait_for_leader(15s);
    BOOST_TEST_MESSAGE("wait_for_leader() returned node " + std::to_string(leader.id()));
    BOOST_TEST(leader.is_leader());

    // A freshly-elected leader can still lose leadership again within
    // milliseconds during a cold cluster's very first election —
    // docker-compose.yml's ELECTION_TIMEOUT_MIN/MAX_MS (150-300ms) is
    // deliberately aggressive for fast test iteration, so every node's
    // timer fires close together at boot. Retry against a freshly
    // re-resolved leader rather than treating one "not_leader"/
    // "commit_failed" response as fatal — PUT is idempotent here, so a
    // retry is always safe.
    boost::json::object resp;
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto& current_leader = cluster.wait_for_leader(15s);
        resp = current_leader.submit_command("smoke", "ok");
        BOOST_TEST_MESSAGE("attempt " + std::to_string(attempt) + " against node " +
                           std::to_string(current_leader.id()) + ": " +
                           boost::json::serialize(resp));
        if (resp.contains("success")) {
            break;
        }
        std::this_thread::sleep_for(200ms);
    }
    BOOST_TEST(resp["success"].as_bool() == true);

    cluster.assert_no_split_brain();
    cluster.stop();
}

BOOST_AUTO_TEST_SUITE_END()
