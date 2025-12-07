#define BOOST_TEST_MODULE RaftTypesTest
#include <boost/test/included/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/exceptions.hpp>
#include <string>
#include <cstdint>

BOOST_AUTO_TEST_SUITE(raft_types_test)

// Test that the node_id concept accepts unsigned integers
BOOST_AUTO_TEST_CASE(test_node_id_concept_unsigned_integers) {
    static_assert(raft::node_id<std::uint32_t>, "uint32_t should satisfy node_id");
    static_assert(raft::node_id<std::uint64_t>, "uint64_t should satisfy node_id");
    static_assert(raft::node_id<std::size_t>, "size_t should satisfy node_id");
    
    // Verify signed integers are rejected
    static_assert(!raft::node_id<int>, "int should not satisfy node_id");
    static_assert(!raft::node_id<std::int32_t>, "int32_t should not satisfy node_id");
}

// Test that the node_id concept accepts strings
BOOST_AUTO_TEST_CASE(test_node_id_concept_strings) {
    static_assert(raft::node_id<std::string>, "std::string should satisfy node_id");
    
    // Verify other types are rejected
    static_assert(!raft::node_id<const char*>, "const char* should not satisfy node_id");
    static_assert(!raft::node_id<double>, "double should not satisfy node_id");
}

// Test that the term_id concept accepts unsigned integers
BOOST_AUTO_TEST_CASE(test_term_id_concept) {
    static_assert(raft::term_id<std::uint32_t>, "uint32_t should satisfy term_id");
    static_assert(raft::term_id<std::uint64_t>, "uint64_t should satisfy term_id");
    static_assert(raft::term_id<std::size_t>, "size_t should satisfy term_id");
    
    // Verify signed integers and other types are rejected
    static_assert(!raft::term_id<int>, "int should not satisfy term_id");
    static_assert(!raft::term_id<std::string>, "std::string should not satisfy term_id");
}

// Test that the log_index concept accepts unsigned integers
BOOST_AUTO_TEST_CASE(test_log_index_concept) {
    static_assert(raft::log_index<std::uint32_t>, "uint32_t should satisfy log_index");
    static_assert(raft::log_index<std::uint64_t>, "uint64_t should satisfy log_index");
    static_assert(raft::log_index<std::size_t>, "size_t should satisfy log_index");
    
    // Verify signed integers and other types are rejected
    static_assert(!raft::log_index<int>, "int should not satisfy log_index");
    static_assert(!raft::log_index<std::string>, "std::string should not satisfy log_index");
}

// Test server_state enum values
BOOST_AUTO_TEST_CASE(test_server_state_enum) {
    raft::server_state state = raft::server_state::follower;
    BOOST_CHECK(state == raft::server_state::follower);
    
    state = raft::server_state::candidate;
    BOOST_CHECK(state == raft::server_state::candidate);
    
    state = raft::server_state::leader;
    BOOST_CHECK(state == raft::server_state::leader);
}

// Test exception hierarchy - raft_exception
BOOST_AUTO_TEST_CASE(test_raft_exception) {
    try {
        throw raft::raft_exception("Test raft exception");
    } catch (const raft::raft_exception& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), "Test raft exception");
    } catch (...) {
        BOOST_FAIL("Should catch raft_exception");
    }
}

// Test exception hierarchy - network_exception
BOOST_AUTO_TEST_CASE(test_network_exception) {
    try {
        throw raft::network_exception("Test network exception");
    } catch (const raft::network_exception& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), "Test network exception");
    } catch (const raft::raft_exception& e) {
        // Should also be catchable as base class
        BOOST_CHECK_EQUAL(std::string(e.what()), "Test network exception");
    } catch (...) {
        BOOST_FAIL("Should catch network_exception");
    }
}

// Test exception hierarchy - persistence_exception
BOOST_AUTO_TEST_CASE(test_persistence_exception) {
    try {
        throw raft::persistence_exception("Test persistence exception");
    } catch (const raft::persistence_exception& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), "Test persistence exception");
    } catch (const raft::raft_exception& e) {
        // Should also be catchable as base class
        BOOST_CHECK_EQUAL(std::string(e.what()), "Test persistence exception");
    } catch (...) {
        BOOST_FAIL("Should catch persistence_exception");
    }
}

// Test exception hierarchy - serialization_exception
BOOST_AUTO_TEST_CASE(test_serialization_exception) {
    try {
        throw raft::serialization_exception("Test serialization exception");
    } catch (const raft::serialization_exception& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), "Test serialization exception");
    } catch (const raft::raft_exception& e) {
        // Should also be catchable as base class
        BOOST_CHECK_EQUAL(std::string(e.what()), "Test serialization exception");
    } catch (...) {
        BOOST_FAIL("Should catch serialization_exception");
    }
}

// Test exception hierarchy - election_exception
BOOST_AUTO_TEST_CASE(test_election_exception) {
    try {
        throw raft::election_exception("Test election exception");
    } catch (const raft::election_exception& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), "Test election exception");
    } catch (const raft::raft_exception& e) {
        // Should also be catchable as base class
        BOOST_CHECK_EQUAL(std::string(e.what()), "Test election exception");
    } catch (...) {
        BOOST_FAIL("Should catch election_exception");
    }
}

// Test that all exceptions inherit from std::runtime_error
BOOST_AUTO_TEST_CASE(test_exception_inheritance) {
    try {
        throw raft::network_exception("Test");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), "Test");
    } catch (...) {
        BOOST_FAIL("Should catch as std::runtime_error");
    }
}

// Test log_entry default implementation
BOOST_AUTO_TEST_CASE(test_log_entry_default_implementation) {
    using log_entry_t = raft::log_entry<std::uint64_t, std::uint64_t>;
    
    // Verify it satisfies the concept
    static_assert(raft::log_entry_type<log_entry_t, std::uint64_t, std::uint64_t>,
                  "log_entry should satisfy log_entry_type concept");
    
    // Create a log entry
    std::vector<std::byte> command = {std::byte{1}, std::byte{2}, std::byte{3}};
    log_entry_t entry{5, 10, command};
    
    BOOST_CHECK_EQUAL(entry.term(), 5);
    BOOST_CHECK_EQUAL(entry.index(), 10);
    BOOST_CHECK(entry.command() == command);
}

// Test request_vote_request default implementation
BOOST_AUTO_TEST_CASE(test_request_vote_request_default_implementation) {
    using request_t = raft::request_vote_request<std::uint64_t, std::uint64_t, std::uint64_t>;
    
    // Verify it satisfies the concept
    static_assert(raft::request_vote_request_type<request_t, std::uint64_t, std::uint64_t, std::uint64_t>,
                  "request_vote_request should satisfy request_vote_request_type concept");
    
    // Create a request
    request_t req{5, 123, 100, 4};
    
    BOOST_CHECK_EQUAL(req.term(), 5);
    BOOST_CHECK_EQUAL(req.candidate_id(), 123);
    BOOST_CHECK_EQUAL(req.last_log_index(), 100);
    BOOST_CHECK_EQUAL(req.last_log_term(), 4);
}

// Test request_vote_response default implementation
BOOST_AUTO_TEST_CASE(test_request_vote_response_default_implementation) {
    using response_t = raft::request_vote_response<std::uint64_t>;
    
    // Verify it satisfies the concept
    static_assert(raft::request_vote_response_type<response_t, std::uint64_t>,
                  "request_vote_response should satisfy request_vote_response_type concept");
    
    // Create a response
    response_t resp{5, true};
    
    BOOST_CHECK_EQUAL(resp.term(), 5);
    BOOST_CHECK_EQUAL(resp.vote_granted(), true);
    
    response_t resp_denied{6, false};
    BOOST_CHECK_EQUAL(resp_denied.term(), 6);
    BOOST_CHECK_EQUAL(resp_denied.vote_granted(), false);
}

// Test append_entries_request default implementation
BOOST_AUTO_TEST_CASE(test_append_entries_request_default_implementation) {
    using log_entry_t = raft::log_entry<std::uint64_t, std::uint64_t>;
    using request_t = raft::append_entries_request<std::uint64_t, std::uint64_t, std::uint64_t, log_entry_t>;
    
    // Verify it satisfies the concept
    static_assert(raft::append_entries_request_type<request_t, std::uint64_t, std::uint64_t, std::uint64_t, log_entry_t>,
                  "append_entries_request should satisfy append_entries_request_type concept");
    
    // Create entries
    std::vector<log_entry_t> entries;
    entries.push_back({5, 10, {std::byte{1}}});
    entries.push_back({5, 11, {std::byte{2}}});
    
    // Create a request
    request_t req{5, 123, 9, 4, entries, 8};
    
    BOOST_CHECK_EQUAL(req.term(), 5);
    BOOST_CHECK_EQUAL(req.leader_id(), 123);
    BOOST_CHECK_EQUAL(req.prev_log_index(), 9);
    BOOST_CHECK_EQUAL(req.prev_log_term(), 4);
    BOOST_CHECK_EQUAL(req.entries().size(), 2);
    BOOST_CHECK_EQUAL(req.leader_commit(), 8);
}

// Test append_entries_response default implementation
BOOST_AUTO_TEST_CASE(test_append_entries_response_default_implementation) {
    using response_t = raft::append_entries_response<std::uint64_t, std::uint64_t>;
    
    // Verify it satisfies the concept
    static_assert(raft::append_entries_response_type<response_t, std::uint64_t, std::uint64_t>,
                  "append_entries_response should satisfy append_entries_response_type concept");
    
    // Create a successful response
    response_t resp_success{5, true, std::nullopt, std::nullopt};
    
    BOOST_CHECK_EQUAL(resp_success.term(), 5);
    BOOST_CHECK_EQUAL(resp_success.success(), true);
    BOOST_CHECK(!resp_success.conflict_index().has_value());
    BOOST_CHECK(!resp_success.conflict_term().has_value());
    
    // Create a failed response with conflict info
    response_t resp_fail{5, false, std::uint64_t{10}, std::uint64_t{3}};
    
    BOOST_CHECK_EQUAL(resp_fail.term(), 5);
    BOOST_CHECK_EQUAL(resp_fail.success(), false);
    BOOST_CHECK(resp_fail.conflict_index().has_value());
    BOOST_CHECK_EQUAL(resp_fail.conflict_index().value(), 10);
    BOOST_CHECK(resp_fail.conflict_term().has_value());
    BOOST_CHECK_EQUAL(resp_fail.conflict_term().value(), 3);
}

// Test install_snapshot_request default implementation
BOOST_AUTO_TEST_CASE(test_install_snapshot_request_default_implementation) {
    using request_t = raft::install_snapshot_request<std::uint64_t, std::uint64_t, std::uint64_t>;
    
    // Verify it satisfies the concept
    static_assert(raft::install_snapshot_request_type<request_t, std::uint64_t, std::uint64_t, std::uint64_t>,
                  "install_snapshot_request should satisfy install_snapshot_request_type concept");
    
    // Create snapshot data
    std::vector<std::byte> data = {std::byte{1}, std::byte{2}, std::byte{3}};
    
    // Create a request
    request_t req{5, 123, 100, 4, 0, data, false};
    
    BOOST_CHECK_EQUAL(req.term(), 5);
    BOOST_CHECK_EQUAL(req.leader_id(), 123);
    BOOST_CHECK_EQUAL(req.last_included_index(), 100);
    BOOST_CHECK_EQUAL(req.last_included_term(), 4);
    BOOST_CHECK_EQUAL(req.offset(), 0);
    BOOST_CHECK(req.data() == data);
    BOOST_CHECK_EQUAL(req.done(), false);
    
    // Create a final chunk request
    request_t req_final{5, 123, 100, 4, 1000, data, true};
    BOOST_CHECK_EQUAL(req_final.done(), true);
}

// Test install_snapshot_response default implementation
BOOST_AUTO_TEST_CASE(test_install_snapshot_response_default_implementation) {
    using response_t = raft::install_snapshot_response<std::uint64_t>;
    
    // Verify it satisfies the concept
    static_assert(raft::install_snapshot_response_type<response_t, std::uint64_t>,
                  "install_snapshot_response should satisfy install_snapshot_response_type concept");
    
    // Create a response
    response_t resp{5};
    
    BOOST_CHECK_EQUAL(resp.term(), 5);
}

// Test RPC message types with string node IDs
BOOST_AUTO_TEST_CASE(test_rpc_messages_with_string_node_ids) {
    using request_vote_req_t = raft::request_vote_request<std::string, std::uint64_t, std::uint64_t>;
    using append_entries_req_t = raft::append_entries_request<std::string, std::uint64_t, std::uint64_t>;
    using install_snapshot_req_t = raft::install_snapshot_request<std::string, std::uint64_t, std::uint64_t>;
    
    // Verify they satisfy the concepts
    static_assert(raft::request_vote_request_type<request_vote_req_t, std::string, std::uint64_t, std::uint64_t>,
                  "request_vote_request should work with string node IDs");
    static_assert(raft::append_entries_request_type<append_entries_req_t, std::string, std::uint64_t, std::uint64_t, raft::log_entry<>>,
                  "append_entries_request should work with string node IDs");
    static_assert(raft::install_snapshot_request_type<install_snapshot_req_t, std::string, std::uint64_t, std::uint64_t>,
                  "install_snapshot_request should work with string node IDs");
    
    // Create and test with string IDs
    request_vote_req_t req{5, "node-123", 100, 4};
    BOOST_CHECK_EQUAL(req.candidate_id(), "node-123");
    
    append_entries_req_t ae_req{5, "leader-1", 9, 4, {}, 8};
    BOOST_CHECK_EQUAL(ae_req.leader_id(), "leader-1");
    
    install_snapshot_req_t is_req{5, "leader-1", 100, 4, 0, {}, false};
    BOOST_CHECK_EQUAL(is_req.leader_id(), "leader-1");
}

// Test cluster_configuration default implementation
BOOST_AUTO_TEST_CASE(test_cluster_configuration_default_implementation) {
    using config_t = raft::cluster_configuration<std::uint64_t>;
    
    // Verify it satisfies the concept
    static_assert(raft::cluster_configuration_type<config_t, std::uint64_t>,
                  "cluster_configuration should satisfy cluster_configuration_type concept");
    
    // Create a simple configuration
    std::vector<std::uint64_t> nodes = {1, 2, 3};
    config_t config{nodes, false, std::nullopt};
    
    BOOST_CHECK(config.nodes() == nodes);
    BOOST_CHECK_EQUAL(config.is_joint_consensus(), false);
    BOOST_CHECK(!config.old_nodes().has_value());
    
    // Create a joint consensus configuration
    std::vector<std::uint64_t> old_nodes = {1, 2};
    std::vector<std::uint64_t> new_nodes = {1, 2, 3, 4};
    config_t joint_config{new_nodes, true, old_nodes};
    
    BOOST_CHECK(joint_config.nodes() == new_nodes);
    BOOST_CHECK_EQUAL(joint_config.is_joint_consensus(), true);
    BOOST_CHECK(joint_config.old_nodes().has_value());
    BOOST_CHECK(joint_config.old_nodes().value() == old_nodes);
}

// Test cluster_configuration with string node IDs
BOOST_AUTO_TEST_CASE(test_cluster_configuration_with_string_node_ids) {
    using config_t = raft::cluster_configuration<std::string>;
    
    // Verify it satisfies the concept
    static_assert(raft::cluster_configuration_type<config_t, std::string>,
                  "cluster_configuration should work with string node IDs");
    
    // Create a configuration with string IDs
    std::vector<std::string> nodes = {"node-1", "node-2", "node-3"};
    config_t config{nodes, false, std::nullopt};
    
    BOOST_CHECK(config.nodes() == nodes);
    BOOST_CHECK_EQUAL(config.nodes().size(), 3);
    BOOST_CHECK_EQUAL(config.nodes()[0], "node-1");
}

// Test snapshot default implementation
BOOST_AUTO_TEST_CASE(test_snapshot_default_implementation) {
    using snapshot_t = raft::snapshot<std::uint64_t, std::uint64_t, std::uint64_t>;
    using config_t = raft::cluster_configuration<std::uint64_t>;
    
    // Verify it satisfies the concept
    static_assert(raft::snapshot_type<snapshot_t, std::uint64_t, std::uint64_t, std::uint64_t>,
                  "snapshot should satisfy snapshot_type concept");
    
    // Create a configuration
    std::vector<std::uint64_t> nodes = {1, 2, 3};
    config_t config{nodes, false, std::nullopt};
    
    // Create state machine state
    std::vector<std::byte> state = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    
    // Create a snapshot
    snapshot_t snap{100, 5, config, state};
    
    BOOST_CHECK_EQUAL(snap.last_included_index(), 100);
    BOOST_CHECK_EQUAL(snap.last_included_term(), 5);
    BOOST_CHECK(snap.configuration().nodes() == nodes);
    BOOST_CHECK(snap.state_machine_state() == state);
}

// Test snapshot with joint consensus configuration
BOOST_AUTO_TEST_CASE(test_snapshot_with_joint_consensus) {
    using snapshot_t = raft::snapshot<std::uint64_t, std::uint64_t, std::uint64_t>;
    using config_t = raft::cluster_configuration<std::uint64_t>;
    
    // Create a joint consensus configuration
    std::vector<std::uint64_t> old_nodes = {1, 2};
    std::vector<std::uint64_t> new_nodes = {1, 2, 3, 4};
    config_t joint_config{new_nodes, true, old_nodes};
    
    // Create state machine state
    std::vector<std::byte> state = {std::byte{5}, std::byte{6}};
    
    // Create a snapshot with joint consensus
    snapshot_t snap{200, 10, joint_config, state};
    
    BOOST_CHECK_EQUAL(snap.last_included_index(), 200);
    BOOST_CHECK_EQUAL(snap.last_included_term(), 10);
    BOOST_CHECK_EQUAL(snap.configuration().is_joint_consensus(), true);
    BOOST_CHECK(snap.configuration().old_nodes().has_value());
    BOOST_CHECK(snap.configuration().old_nodes().value() == old_nodes);
}

// Test snapshot with string node IDs
BOOST_AUTO_TEST_CASE(test_snapshot_with_string_node_ids) {
    using snapshot_t = raft::snapshot<std::string, std::uint64_t, std::uint64_t>;
    using config_t = raft::cluster_configuration<std::string>;
    
    // Verify it satisfies the concept
    static_assert(raft::snapshot_type<snapshot_t, std::string, std::uint64_t, std::uint64_t>,
                  "snapshot should work with string node IDs");
    
    // Create a configuration with string IDs
    std::vector<std::string> nodes = {"node-1", "node-2", "node-3"};
    config_t config{nodes, false, std::nullopt};
    
    // Create state machine state
    std::vector<std::byte> state = {std::byte{7}, std::byte{8}};
    
    // Create a snapshot
    snapshot_t snap{150, 7, config, state};
    
    BOOST_CHECK_EQUAL(snap.last_included_index(), 150);
    BOOST_CHECK_EQUAL(snap.last_included_term(), 7);
    BOOST_CHECK_EQUAL(snap.configuration().nodes().size(), 3);
    BOOST_CHECK_EQUAL(snap.configuration().nodes()[0], "node-1");
}

BOOST_AUTO_TEST_SUITE_END()
