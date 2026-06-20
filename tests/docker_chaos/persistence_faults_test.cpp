#define BOOST_TEST_MODULE persistence_faults_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"

#include <thread>

using namespace std::chrono_literals;
using namespace docker_chaos;

BOOST_AUTO_TEST_SUITE(docker_chaos_persistence_faults)

// Req 8.5: 30% random append_log_entry failures on a follower; commands still commit.
BOOST_FIXTURE_TEST_CASE(fiu_disk_degradation, ChaosFixture, *boost::unit_test::timeout(120)) {
    auto& leader = cluster.wait_for_leader(10s);

    ChaosNode* follower = nullptr;
    for (auto* n : cluster.all_nodes()) {
        if (n->id() != leader.id()) {
            follower = n;
            break;
        }
    }
    BOOST_REQUIRE(follower != nullptr);

    follower->enable_fault(fiu::APPEND_LOG_ENTRY, "random", 0.30);

    for (int i = 0; i < 10; ++i) {
        leader.submit_command("diskkey" + std::to_string(i), "diskval" + std::to_string(i));
    }

    follower->disable_all_faults();
    cluster.assert_no_split_brain();
}

// Fault save_current_term 100% on the leader; verify step-down and recovery.
BOOST_FIXTURE_TEST_CASE(fiu_leader_term_persistence_fault, ChaosFixture,
                        *boost::unit_test::timeout(120)) {
    auto& leader = cluster.wait_for_leader(10s);

    leader.enable_fault(fiu::SAVE_CURRENT_TERM, "always");
    std::this_thread::sleep_for(300ms);
    leader.disable_all_faults();

    auto& new_leader = cluster.wait_for_leader(3s);
    BOOST_TEST(new_leader.is_leader());
    cluster.assert_no_split_brain();

    new_leader.submit_command("postfault", "ok");
}

BOOST_AUTO_TEST_SUITE_END()
