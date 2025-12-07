#define BOOST_TEST_MODULE RpcSerializationPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/types.hpp>
#include <raft/json_serializer.hpp>

#include <random>
#include <string>
#include <vector>
#include <cstddef>

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr std::uint64_t max_term = 1000000;
    constexpr std::uint64_t max_index = 1000000;
    constexpr std::uint64_t max_node_id = 10000;
    constexpr std::size_t max_entries = 10;
    constexpr std::size_t max_command_size = 100;
    constexpr std::size_t max_snapshot_data_size = 1000;
}

// Helper to generate random term
auto generate_random_term(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_term);
    return dist(rng);
}

// Helper to generate random log index
auto generate_random_log_index(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_index);
    return dist(rng);
}

// Helper to generate random node ID
auto generate_random_node_id(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_node_id);
    return dist(rng);
}

// Helper to generate random string node ID
auto generate_random_string_node_id(std::mt19937& rng) -> std::string {
    return "node_" + std::to_string(generate_random_node_id(rng));
}

// Helper to generate random command
auto generate_random_command(std::mt19937& rng) -> std::vector<std::byte> {
    std::uniform_int_distribution<std::size_t> size_dist(1, max_command_size);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    
    std::size_t size = size_dist(rng);
    std::vector<std::byte> command;
    command.reserve(size);
    
    for (std::size_t i = 0; i < size; ++i) {
        command.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    
    return command;
}

// Helper to generate random log entries
auto generate_random_log_entries(std::mt19937& rng) -> std::vector<raft::log_entry<>> {
    std::uniform_int_distribution<std::size_t> count_dist(0, max_entries);
    std::size_t count = count_dist(rng);
    
    std::vector<raft::log_entry<>> entries;
    entries.reserve(count);
    
    for (std::size_t i = 0; i < count; ++i) {
        raft::log_entry<> entry;
        entry._term = generate_random_term(rng);
        entry._index = generate_random_log_index(rng);
        entry._command = generate_random_command(rng);
        entries.push_back(entry);
    }
    
    return entries;
}

// Helper to generate random snapshot data
auto generate_random_snapshot_data(std::mt19937& rng) -> std::vector<std::byte> {
    std::uniform_int_distribution<std::size_t> size_dist(1, max_snapshot_data_size);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    
    std::size_t size = size_dist(rng);
    std::vector<std::byte> data;
    data.reserve(size);
    
    for (std::size_t i = 0; i < size; ++i) {
        data.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    
    return data;
}

// Helper to compare log entries
auto log_entries_equal(const raft::log_entry<>& a, const raft::log_entry<>& b) -> bool {
    return a.term() == b.term() && 
           a.index() == b.index() && 
           a.command() == b.command();
}

/**
 * Feature: raft-consensus, Property 6: RPC Serialization Round-Trip
 * Validates: Requirements 2.5
 * 
 * Property: For any valid RequestVote request, serializing then deserializing 
 * the message produces an equivalent message with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_request_vote_request_round_trip) {
    std::mt19937 rng(std::random_device{}());
    raft::json_rpc_serializer<> serializer;
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random RequestVote request
        raft::request_vote_request<> original;
        original._term = generate_random_term(rng);
        original._candidate_id = generate_random_node_id(rng);
        original._last_log_index = generate_random_log_index(rng);
        original._last_log_term = generate_random_term(rng);
        
        try {
            // Serialize then deserialize
            auto serialized = serializer.serialize(original);
            auto deserialized = serializer.deserialize_request_vote_request(serialized);
            
            // Verify all fields match
            if (deserialized.term() != original.term() ||
                deserialized.candidate_id() != original.candidate_id() ||
                deserialized.last_log_index() != original.last_log_index() ||
                deserialized.last_log_term() != original.last_log_term()) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": RequestVote request fields mismatch");
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("RequestVote request round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: raft-consensus, Property 6: RPC Serialization Round-Trip
 * Validates: Requirements 2.5
 * 
 * Property: For any valid RequestVote response, serializing then deserializing 
 * the message produces an equivalent message with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_request_vote_response_round_trip) {
    std::mt19937 rng(std::random_device{}());
    raft::json_rpc_serializer<> serializer;
    
    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random RequestVote response
        raft::request_vote_response<> original;
        original._term = generate_random_term(rng);
        original._vote_granted = bool_dist(rng) == 1;
        
        try {
            // Serialize then deserialize
            auto serialized = serializer.serialize(original);
            auto deserialized = serializer.deserialize_request_vote_response(serialized);
            
            // Verify all fields match
            if (deserialized.term() != original.term() ||
                deserialized.vote_granted() != original.vote_granted()) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": RequestVote response fields mismatch");
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("RequestVote response round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: raft-consensus, Property 6: RPC Serialization Round-Trip
 * Validates: Requirements 2.5
 * 
 * Property: For any valid AppendEntries request, serializing then deserializing 
 * the message produces an equivalent message with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_append_entries_request_round_trip) {
    std::mt19937 rng(std::random_device{}());
    raft::json_rpc_serializer<> serializer;
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random AppendEntries request
        raft::append_entries_request<> original;
        original._term = generate_random_term(rng);
        original._leader_id = generate_random_node_id(rng);
        original._prev_log_index = generate_random_log_index(rng);
        original._prev_log_term = generate_random_term(rng);
        original._entries = generate_random_log_entries(rng);
        original._leader_commit = generate_random_log_index(rng);
        
        try {
            // Serialize then deserialize
            auto serialized = serializer.serialize(original);
            auto deserialized = serializer.deserialize_append_entries_request(serialized);
            
            // Verify all fields match
            bool entries_match = true;
            if (deserialized.entries().size() != original.entries().size()) {
                entries_match = false;
            } else {
                for (std::size_t j = 0; j < original.entries().size(); ++j) {
                    if (!log_entries_equal(deserialized.entries()[j], original.entries()[j])) {
                        entries_match = false;
                        break;
                    }
                }
            }
            
            if (deserialized.term() != original.term() ||
                deserialized.leader_id() != original.leader_id() ||
                deserialized.prev_log_index() != original.prev_log_index() ||
                deserialized.prev_log_term() != original.prev_log_term() ||
                deserialized.leader_commit() != original.leader_commit() ||
                !entries_match) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": AppendEntries request fields mismatch");
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("AppendEntries request round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: raft-consensus, Property 6: RPC Serialization Round-Trip
 * Validates: Requirements 2.5
 * 
 * Property: For any valid AppendEntries response, serializing then deserializing 
 * the message produces an equivalent message with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_append_entries_response_round_trip) {
    std::mt19937 rng(std::random_device{}());
    raft::json_rpc_serializer<> serializer;
    
    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random AppendEntries response
        raft::append_entries_response<> original;
        original._term = generate_random_term(rng);
        original._success = bool_dist(rng) == 1;
        
        // Randomly include conflict info
        if (bool_dist(rng) == 1) {
            original._conflict_index = generate_random_log_index(rng);
        }
        if (bool_dist(rng) == 1) {
            original._conflict_term = generate_random_term(rng);
        }
        
        try {
            // Serialize then deserialize
            auto serialized = serializer.serialize(original);
            auto deserialized = serializer.deserialize_append_entries_response(serialized);
            
            // Verify all fields match
            if (deserialized.term() != original.term() ||
                deserialized.success() != original.success() ||
                deserialized.conflict_index() != original.conflict_index() ||
                deserialized.conflict_term() != original.conflict_term()) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": AppendEntries response fields mismatch");
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("AppendEntries response round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: raft-consensus, Property 6: RPC Serialization Round-Trip
 * Validates: Requirements 2.5
 * 
 * Property: For any valid InstallSnapshot request, serializing then deserializing 
 * the message produces an equivalent message with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_install_snapshot_request_round_trip) {
    std::mt19937 rng(std::random_device{}());
    raft::json_rpc_serializer<> serializer;
    
    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::uniform_int_distribution<std::size_t> offset_dist(0, 1000000);
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random InstallSnapshot request
        raft::install_snapshot_request<> original;
        original._term = generate_random_term(rng);
        original._leader_id = generate_random_node_id(rng);
        original._last_included_index = generate_random_log_index(rng);
        original._last_included_term = generate_random_term(rng);
        original._offset = offset_dist(rng);
        original._data = generate_random_snapshot_data(rng);
        original._done = bool_dist(rng) == 1;
        
        try {
            // Serialize then deserialize
            auto serialized = serializer.serialize(original);
            auto deserialized = serializer.deserialize_install_snapshot_request(serialized);
            
            // Verify all fields match
            if (deserialized.term() != original.term() ||
                deserialized.leader_id() != original.leader_id() ||
                deserialized.last_included_index() != original.last_included_index() ||
                deserialized.last_included_term() != original.last_included_term() ||
                deserialized.offset() != original.offset() ||
                deserialized.data() != original.data() ||
                deserialized.done() != original.done()) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": InstallSnapshot request fields mismatch");
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("InstallSnapshot request round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: raft-consensus, Property 6: RPC Serialization Round-Trip
 * Validates: Requirements 2.5
 * 
 * Property: For any valid InstallSnapshot response, serializing then deserializing 
 * the message produces an equivalent message with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_install_snapshot_response_round_trip) {
    std::mt19937 rng(std::random_device{}());
    raft::json_rpc_serializer<> serializer;
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random InstallSnapshot response
        raft::install_snapshot_response<> original;
        original._term = generate_random_term(rng);
        
        try {
            // Serialize then deserialize
            auto serialized = serializer.serialize(original);
            auto deserialized = serializer.deserialize_install_snapshot_response(serialized);
            
            // Verify all fields match
            if (deserialized.term() != original.term()) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": InstallSnapshot response fields mismatch");
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("InstallSnapshot response round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: raft-consensus, Property 6: RPC Serialization Round-Trip
 * Validates: Requirements 2.5
 * 
 * Property: For any valid RPC message with string node IDs, serializing then 
 * deserializing the message produces an equivalent message with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_string_node_id_round_trip) {
    std::mt19937 rng(std::random_device{}());
    raft::json_rpc_serializer<> serializer;
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Test RequestVote with string node IDs
        raft::request_vote_request<std::string> rv_original;
        rv_original._term = generate_random_term(rng);
        rv_original._candidate_id = generate_random_string_node_id(rng);
        rv_original._last_log_index = generate_random_log_index(rng);
        rv_original._last_log_term = generate_random_term(rng);
        
        try {
            auto serialized = serializer.serialize(rv_original);
            auto deserialized = serializer.deserialize_request_vote_request<std::string>(serialized);
            
            if (deserialized.term() != rv_original.term() ||
                deserialized.candidate_id() != rv_original.candidate_id() ||
                deserialized.last_log_index() != rv_original.last_log_index() ||
                deserialized.last_log_term() != rv_original.last_log_term()) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": String node ID RequestVote mismatch");
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("String node ID round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}
