#define BOOST_TEST_MODULE election_recovery_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"

#include <thread>

using namespace std::chrono_literals;
using namespace docker_chaos;

static constexpr auto k_election_max = 300ms;  // ELECTION_TIMEOUT_MAX_MS in docker-compose.yml

BOOST_AUTO_TEST_SUITE(docker_chaos_election_recovery)

// Req 8.1: partition minority (1 node) via iptables, heal, verify leader elected.
BOOST_FIXTURE_TEST_CASE(election_after_iptables_partition, ChaosFixture,
                        *boost::unit_test::timeout(120)) {
    auto& n1 = cluster.node(1);
    auto& n2 = cluster.node(2);
    auto& n3 = cluster.node(3);

    cluster.wait_for_leader(10s);

    std::string ip1 = n1.container_ip();
    std::string ip2 = n2.container_ip();

    // Isolate n3 (minority) from the majority.
    n3.partition_from({ip1, ip2});
    n1.partition_from({n3.container_ip()});
    n2.partition_from({n3.container_ip()});

    std::this_thread::sleep_for(k_election_max * 2);

    // Heal the partition.
    n1.unpartition();
    n2.unpartition();
    n3.unpartition();

    // A leader must emerge within 2 × election_timeout_max.
    cluster.wait_for_leader(k_election_max * 4);
    cluster.assert_no_split_brain();
}

// Application-layer equivalent: fault all outbound sends from node 3 then heal.
BOOST_FIXTURE_TEST_CASE(election_after_fiu_network_isolation, ChaosFixture,
                        *boost::unit_test::timeout(120)) {
    auto& n3 = cluster.node(3);
    cluster.wait_for_leader(10s);

    for (auto name :
         {fiu::SEND_REQUEST_VOTE, fiu::SEND_APPEND_ENTRIES, fiu::SEND_INSTALL_SNAPSHOT}) {
        n3.enable_fault(name, "always");
    }

    std::this_thread::sleep_for(k_election_max * 2);
    n3.disable_all_faults();

    cluster.wait_for_leader(k_election_max * 4);
    cluster.assert_no_split_brain();
}

BOOST_AUTO_TEST_SUITE_END()
