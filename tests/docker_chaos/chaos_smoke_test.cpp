#define BOOST_TEST_MODULE chaos_smoke_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"

using namespace std::chrono_literals;
using namespace docker_chaos;

BOOST_AUTO_TEST_SUITE(docker_chaos_smoke)

BOOST_AUTO_TEST_CASE(cluster_starts_elects_leader_accepts_command, *boost::unit_test::timeout(60)) {
    ChaosCluster cluster(default_compose_file(), 30s);
    cluster.start();

    auto& leader = cluster.wait_for_leader(15s);
    BOOST_TEST(leader.is_leader());

    auto resp = leader.submit_command("smoke", "ok");
    BOOST_TEST(resp["success"].as_bool() == true);

    cluster.assert_no_split_brain();
    cluster.stop();
}

BOOST_AUTO_TEST_SUITE_END()
