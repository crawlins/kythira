#define BOOST_TEST_MODULE raft_node_structure_test
#include <boost/test/included/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/membership.hpp>

#include <network_simulator/network_simulator.hpp>

namespace {
    constexpr std::uint64_t test_node_id = 1;
}

// Test that the node class template structure is well-formed
BOOST_AUTO_TEST_CASE(test_node_type_structure) {
    // Define the node type with all template parameters
    using network_client_t = raft::simulator_network_client<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >;
    
    using network_server_t = raft::simulator_network_server<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >;
    
    using persistence_t = raft::memory_persistence_engine<>;
    using logger_t = raft::console_logger;
    using metrics_t = raft::noop_metrics;
    using membership_t = raft::default_membership_manager<>;
    
    using node_t = raft::node<
        network_client_t,
        network_server_t,
        persistence_t,
        logger_t,
        metrics_t,
        membership_t
    >;
    
    // Verify that the node type is well-formed
    // This will fail to compile if the template is malformed
    [[maybe_unused]] node_t* ptr = nullptr;
    
    // Verify that node satisfies raft_node concept
    static_assert(raft::raft_node<node_t>, "node must satisfy raft_node concept");
    
    BOOST_CHECK(true);  // Test passes if it compiles
}

// Test with custom NodeId, TermId, and LogIndex types
BOOST_AUTO_TEST_CASE(test_node_with_custom_types) {
    using custom_node_id = std::uint32_t;
    using custom_term_id = std::uint32_t;
    using custom_log_index = std::uint32_t;
    
    using network_client_t = raft::simulator_network_client<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >;
    
    using network_server_t = raft::simulator_network_server<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >;
    
    using persistence_t = raft::memory_persistence_engine<custom_node_id, custom_term_id, custom_log_index>;
    using logger_t = raft::console_logger;
    using metrics_t = raft::noop_metrics;
    using membership_t = raft::default_membership_manager<custom_node_id>;
    
    using node_t = raft::node<
        network_client_t,
        network_server_t,
        persistence_t,
        logger_t,
        metrics_t,
        membership_t,
        custom_node_id,
        custom_term_id,
        custom_log_index
    >;
    
    // Verify that the node type with custom types is well-formed
    [[maybe_unused]] node_t* ptr = nullptr;
    
    // Verify that node satisfies raft_node concept
    static_assert(raft::raft_node<node_t>, "node with custom types must satisfy raft_node concept");
    
    BOOST_CHECK(true);  // Test passes if it compiles
}

// Test that all required member types are defined
BOOST_AUTO_TEST_CASE(test_node_member_types) {
    using network_client_t = raft::simulator_network_client<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >;
    
    using network_server_t = raft::simulator_network_server<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >;
    
    using persistence_t = raft::memory_persistence_engine<>;
    using logger_t = raft::console_logger;
    using metrics_t = raft::noop_metrics;
    using membership_t = raft::default_membership_manager<>;
    
    using node_t = raft::node<
        network_client_t,
        network_server_t,
        persistence_t,
        logger_t,
        metrics_t,
        membership_t
    >;
    
    // Verify that all required type aliases are defined
    [[maybe_unused]] typename node_t::log_entry_t* log_entry_ptr = nullptr;
    [[maybe_unused]] typename node_t::cluster_configuration_t* config_ptr = nullptr;
    [[maybe_unused]] typename node_t::snapshot_t* snapshot_ptr = nullptr;
    [[maybe_unused]] typename node_t::request_vote_request_t* rvr_ptr = nullptr;
    [[maybe_unused]] typename node_t::request_vote_response_t* rvrsp_ptr = nullptr;
    [[maybe_unused]] typename node_t::append_entries_request_t* aer_ptr = nullptr;
    [[maybe_unused]] typename node_t::append_entries_response_t* aersp_ptr = nullptr;
    [[maybe_unused]] typename node_t::install_snapshot_request_t* isr_ptr = nullptr;
    [[maybe_unused]] typename node_t::install_snapshot_response_t* isrsp_ptr = nullptr;
    
    BOOST_CHECK(true);  // Test passes if it compiles
}



