#define BOOST_TEST_MODULE RpcMalformedMessagePropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/types.hpp>
#include <raft/json_serializer.hpp>
#include <raft/exceptions.hpp>

#include <random>
#include <string>
#include <vector>
#include <cstddef>

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr std::size_t max_random_bytes = 1000;
}

// Helper to generate random byte sequence
auto generate_random_bytes(std::mt19937& rng, std::size_t size) -> std::vector<std::byte> {
    std::uniform_int_distribution<int> byte_dist(0, 255);
    
    std::vector<std::byte> data;
    data.reserve(size);
    
    for (std::size_t i = 0; i < size; ++i) {
        data.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    
    return data;
}

// Helper to convert string to bytes
auto string_to_bytes(const std::string& str) -> std::vector<std::byte> {
    std::vector<std::byte> result;
    result.reserve(str.size());
    for (char c : str) {
        result.push_back(static_cast<std::byte>(c));
    }
    return result;
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 * 
 * Property: For any byte sequence that does not represent a valid RequestVote 
 * request, the deserializer rejects it with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_malformed_request_vote_request_rejection) {
    std::mt19937 rng(std::random_device{}());
    raft::json_rpc_serializer<> serializer;
    
    std::size_t rejection_count = 0;
    std::uniform_int_distribution<std::size_t> size_dist(1, max_random_bytes);
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random byte sequence
        std::size_t size = size_dist(rng);
        auto random_data = generate_random_bytes(rng, size);
        
        try {
            // Attempt to deserialize - should throw
            auto result = serializer.deserialize_request_vote_request(random_data);
            // If we get here, deserialization succeeded (unexpected for random data)
        } catch (const raft::serialization_exception&) {
            // Expected - malformed data rejected
            ++rejection_count;
        } catch (const std::exception&) {
            // Also acceptable - any exception indicates rejection
            ++rejection_count;
        }
    }
    
    BOOST_TEST_MESSAGE("Malformed RequestVote request rejection: " 
        << rejection_count << "/" << property_test_iterations << " rejected");
    
    // We expect most random byte sequences to be rejected
    // Allow a small margin for the unlikely case of valid JSON
    BOOST_CHECK_GE(rejection_count, property_test_iterations * 95 / 100);
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 * 
 * Property: For any JSON with incorrect message type, the deserializer 
 * rejects it with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_wrong_message_type_rejection) {
    raft::json_rpc_serializer<> serializer;
    
    std::size_t rejection_count = 0;
    
    // Test cases with wrong message types
    std::vector<std::string> wrong_type_messages = {
        R"({"type":"wrong_type","term":1,"candidate_id":1,"last_log_index":1,"last_log_term":1})",
        R"({"type":"append_entries_request","term":1,"candidate_id":1,"last_log_index":1,"last_log_term":1})",
        R"({"type":"request_vote_response","term":1,"candidate_id":1,"last_log_index":1,"last_log_term":1})",
        R"({"type":"","term":1,"candidate_id":1,"last_log_index":1,"last_log_term":1})",
    };
    
    for (const auto& msg : wrong_type_messages) {
        auto data = string_to_bytes(msg);
        
        try {
            auto result = serializer.deserialize_request_vote_request(data);
            // Should not reach here
        } catch (const raft::serialization_exception&) {
            ++rejection_count;
        } catch (const std::exception&) {
            ++rejection_count;
        }
    }
    
    BOOST_TEST_MESSAGE("Wrong message type rejection: " 
        << rejection_count << "/" << wrong_type_messages.size() << " rejected");
    BOOST_CHECK_EQUAL(rejection_count, wrong_type_messages.size());
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 * 
 * Property: For any JSON with missing required fields, the deserializer 
 * rejects it with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_missing_fields_rejection) {
    raft::json_rpc_serializer<> serializer;
    
    std::size_t rejection_count = 0;
    
    // Test cases with missing required fields
    std::vector<std::string> missing_field_messages = {
        R"({"type":"request_vote_request"})",
        R"({"type":"request_vote_request","term":1})",
        R"({"type":"request_vote_request","term":1,"candidate_id":1})",
        R"({"type":"request_vote_request","term":1,"candidate_id":1,"last_log_index":1})",
        R"({"type":"request_vote_request","candidate_id":1,"last_log_index":1,"last_log_term":1})",
    };
    
    for (const auto& msg : missing_field_messages) {
        auto data = string_to_bytes(msg);
        
        try {
            auto result = serializer.deserialize_request_vote_request(data);
            // Should not reach here
        } catch (const raft::serialization_exception&) {
            ++rejection_count;
        } catch (const std::exception&) {
            ++rejection_count;
        }
    }
    
    BOOST_TEST_MESSAGE("Missing fields rejection: " 
        << rejection_count << "/" << missing_field_messages.size() << " rejected");
    BOOST_CHECK_EQUAL(rejection_count, missing_field_messages.size());
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 * 
 * Property: For any invalid JSON syntax, the deserializer rejects it 
 * with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_invalid_json_syntax_rejection) {
    raft::json_rpc_serializer<> serializer;
    
    std::size_t rejection_count = 0;
    
    // Test cases with invalid JSON syntax
    std::vector<std::string> invalid_json_messages = {
        R"({invalid json})",
        R"({"type":"request_vote_request",})",
        R"({"type":"request_vote_request")",
        R"(not json at all)",
        R"({"type":"request_vote_request","term":"not a number","candidate_id":1,"last_log_index":1,"last_log_term":1})",
        R"()",
        R"(null)",
        R"([])",
    };
    
    for (const auto& msg : invalid_json_messages) {
        auto data = string_to_bytes(msg);
        
        try {
            auto result = serializer.deserialize_request_vote_request(data);
            // Should not reach here
        } catch (const raft::serialization_exception&) {
            ++rejection_count;
        } catch (const std::exception&) {
            ++rejection_count;
        }
    }
    
    BOOST_TEST_MESSAGE("Invalid JSON syntax rejection: " 
        << rejection_count << "/" << invalid_json_messages.size() << " rejected");
    BOOST_CHECK_EQUAL(rejection_count, invalid_json_messages.size());
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 * 
 * Property: For any malformed AppendEntries request, the deserializer 
 * rejects it with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_malformed_append_entries_request_rejection) {
    std::mt19937 rng(std::random_device{}());
    raft::json_rpc_serializer<> serializer;
    
    std::size_t rejection_count = 0;
    std::uniform_int_distribution<std::size_t> size_dist(1, max_random_bytes);
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random byte sequence
        std::size_t size = size_dist(rng);
        auto random_data = generate_random_bytes(rng, size);
        
        try {
            auto result = serializer.deserialize_append_entries_request(random_data);
        } catch (const raft::serialization_exception&) {
            ++rejection_count;
        } catch (const std::exception&) {
            ++rejection_count;
        }
    }
    
    BOOST_TEST_MESSAGE("Malformed AppendEntries request rejection: " 
        << rejection_count << "/" << property_test_iterations << " rejected");
    BOOST_CHECK_GE(rejection_count, property_test_iterations * 95 / 100);
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 * 
 * Property: For any malformed InstallSnapshot request, the deserializer 
 * rejects it with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_malformed_install_snapshot_request_rejection) {
    std::mt19937 rng(std::random_device{}());
    raft::json_rpc_serializer<> serializer;
    
    std::size_t rejection_count = 0;
    std::uniform_int_distribution<std::size_t> size_dist(1, max_random_bytes);
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random byte sequence
        std::size_t size = size_dist(rng);
        auto random_data = generate_random_bytes(rng, size);
        
        try {
            auto result = serializer.deserialize_install_snapshot_request(random_data);
        } catch (const raft::serialization_exception&) {
            ++rejection_count;
        } catch (const std::exception&) {
            ++rejection_count;
        }
    }
    
    BOOST_TEST_MESSAGE("Malformed InstallSnapshot request rejection: " 
        << rejection_count << "/" << property_test_iterations << " rejected");
    BOOST_CHECK_GE(rejection_count, property_test_iterations * 95 / 100);
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 * 
 * Property: For any AppendEntries request with invalid entry data, 
 * the deserializer rejects it with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_invalid_entry_data_rejection) {
    raft::json_rpc_serializer<> serializer;
    
    std::size_t rejection_count = 0;
    
    // Test cases with invalid entry data
    std::vector<std::string> invalid_entry_messages = {
        R"({"type":"append_entries_request","term":1,"leader_id":1,"prev_log_index":1,"prev_log_term":1,"leader_commit":1,"entries":[{"term":1,"index":1}]})",
        R"({"type":"append_entries_request","term":1,"leader_id":1,"prev_log_index":1,"prev_log_term":1,"leader_commit":1,"entries":"not an array"})",
        R"({"type":"append_entries_request","term":1,"leader_id":1,"prev_log_index":1,"prev_log_term":1,"leader_commit":1,"entries":[{"term":"not a number","index":1,"command":"AQID"}]})",
    };
    
    for (const auto& msg : invalid_entry_messages) {
        auto data = string_to_bytes(msg);
        
        try {
            auto result = serializer.deserialize_append_entries_request(data);
            // Should not reach here
        } catch (const raft::serialization_exception&) {
            ++rejection_count;
        } catch (const std::exception&) {
            ++rejection_count;
        }
    }
    
    BOOST_TEST_MESSAGE("Invalid entry data rejection: " 
        << rejection_count << "/" << invalid_entry_messages.size() << " rejected");
    BOOST_CHECK_EQUAL(rejection_count, invalid_entry_messages.size());
}

