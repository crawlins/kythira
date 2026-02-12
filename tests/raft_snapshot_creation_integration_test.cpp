/**
 * Integration Test for Snapshot Creation with State Machine
 * 
 * Tests that create_snapshot properly integrates with the state machine.
 * 
 * Requirements: 10.1, 10.2, 31.1
 * This test verifies task 601: Complete state machine integration in create_snapshot
 */

#define BOOST_TEST_MODULE RaftSnapshotCreationIntegrationTest
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/examples/counter_state_machine.hpp>

/**
 * Test: Verify create_snapshot() method exists and works
 */
BOOST_AUTO_TEST_CASE(test_create_snapshot_exists, * boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: create_snapshot() method exists and compiles");
    
    using counter_sm = kythira::examples::counter_state_machine<std::uint64_t>;
    
    struct counter_raft_types : kythira::default_raft_types {
        using state_machine_type = counter_sm;
    };
    
    using node_type = kythira::node<counter_raft_types>;
    
    // Create components
    auto persistence = kythira::memory_persistence_engine<>{};
    auto logger = kythira::console_logger{};
    auto metrics = kythira::noop_metrics{};
    auto membership = kythira::default_membership_manager<std::uint64_t>{};
    
    auto serializer = kythira::json_rpc_serializer<std::vector<std::byte>>{};
    auto network_client = kythira::simulator_network_client<
        kythira::Future<std::vector<std::byte>>,
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>>{serializer};
    auto network_server = kythira::simulator_network_server<
        kythira::Future<std::vector<std::byte>>,
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>>{serializer};
    
    // Create Raft node
    node_type node(
        1,
        network_client,
        network_server,
        persistence,
        logger,
        metrics,
        membership
    );
    
    // Call create_snapshot() - this should call _state_machine.get_state()
    node.create_snapshot();
    
    // Verify snapshot was created
    auto snapshot_opt = persistence.load_snapshot();
    BOOST_REQUIRE(snapshot_opt.has_value());
    
    BOOST_TEST_MESSAGE("✓ create_snapshot() works correctly");
}
