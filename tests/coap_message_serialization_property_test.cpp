#define BOOST_TEST_MODULE coap_message_serialization_property_test
#include <boost/test/unit_test.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT 30

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/types.hpp>

#include <random>
#include <vector>
#include <string>
#include <chrono>

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr std::uint64_t max_term = 1000000;
    constexpr std::uint64_t max_index = 1000000;
    constexpr std::uint64_t max_node_id = 1000;
    constexpr std::size_t max_data_size = 10000;
    constexpr std::size_t max_entries = 100;
}

BOOST_AUTO_TEST_SUITE(coap_message_serialization_property_tests)

// **Feature: coap-transport, Property 1: Message serialization round-trip consistency**
// **Validates: Requirements 1.2, 1.3, 7.2**
// Property: For any valid Raft RPC message (request or response), serializing then 
// deserializing should produce an equivalent message.
BOOST_AUTO_TEST_CASE(property_message_serialization_round_trip, * boost::unit_test::timeout(60)) {
    kythira::json_rpc_serializer<std::vector<std::byte>> serializer;
    
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::uint64_t> term_dist(1, max_term);
    std::uniform_int_distribution<std::uint64_t> index_dist(1, max_index);
    std::uniform_int_distribution<std::uint64_t> node_dist(1, max_node_id);
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::uniform_int_distribution<std::size_t> data_size_dist(0, max_data_size);
    std::uniform_int_distribution<std::size_t> entries_count_dist(0, max_entries);
    std::uniform_int_distribution<std::uint8_t> byte_dist(0, 255);
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        try {
            // Test RequestVote request round-trip
            {
                kythira::request_vote_request<> original;
                original._term = term_dist(rng);
                original._candidate_id = node_dist(rng);
                original._last_log_index = index_dist(rng);
                original._last_log_term = term_dist(rng);
                
                auto serialized = serializer.serialize(original);
                auto deserialized = serializer.deserialize_request_vote_request(serialized);
                
                if (original._term != deserialized._term ||
                    original._candidate_id != deserialized._candidate_id ||
                    original._last_log_index != deserialized._last_log_index ||
                    original._last_log_term != deserialized._last_log_term) {
                    failures++;
                    BOOST_TEST_MESSAGE("RequestVote request round-trip failed at iteration " << i);
                }
            }
            
            // Test RequestVote response round-trip
            {
                kythira::request_vote_response<> original;
                original._term = term_dist(rng);
                original._vote_granted = bool_dist(rng) == 1;
                
                auto serialized = serializer.serialize(original);
                auto deserialized = serializer.deserialize_request_vote_response(serialized);
                
                if (original._term != deserialized._term ||
                    original._vote_granted != deserialized._vote_granted) {
                    failures++;
                    BOOST_TEST_MESSAGE("RequestVote response round-trip failed at iteration " << i);
                }
            }
            
            // Test AppendEntries request round-trip
            {
                kythira::append_entries_request<> original;
                original._term = term_dist(rng);
                original._leader_id = node_dist(rng);
                original._prev_log_index = index_dist(rng);
                original._prev_log_term = term_dist(rng);
                original._leader_commit = index_dist(rng);
                
                // Generate random entries
                std::size_t entry_count = entries_count_dist(rng);
                for (std::size_t j = 0; j < entry_count; ++j) {
                    kythira::log_entry<> entry;
                    entry._term = term_dist(rng);
                    entry._index = index_dist(rng);
                    
                    // Generate random command data
                    std::size_t cmd_size = data_size_dist(rng);
                    entry._command.resize(cmd_size);
                    for (std::size_t k = 0; k < cmd_size; ++k) {
                        entry._command[k] = static_cast<std::byte>(byte_dist(rng));
                    }
                    
                    original._entries.push_back(entry);
                }
                
                auto serialized = serializer.serialize(original);
                auto deserialized = serializer.deserialize_append_entries_request(serialized);
                
                bool entries_match = true;
                if (original._entries.size() != deserialized._entries.size()) {
                    entries_match = false;
                } else {
                    for (std::size_t j = 0; j < original._entries.size(); ++j) {
                        const auto& orig_entry = original._entries[j];
                        const auto& deser_entry = deserialized._entries[j];
                        
                        if (orig_entry._term != deser_entry._term ||
                            orig_entry._index != deser_entry._index ||
                            orig_entry._command != deser_entry._command) {
                            entries_match = false;
                            break;
                        }
                    }
                }
                
                if (original._term != deserialized._term ||
                    original._leader_id != deserialized._leader_id ||
                    original._prev_log_index != deserialized._prev_log_index ||
                    original._prev_log_term != deserialized._prev_log_term ||
                    original._leader_commit != deserialized._leader_commit ||
                    !entries_match) {
                    failures++;
                    BOOST_TEST_MESSAGE("AppendEntries request round-trip failed at iteration " << i);
                }
            }
            
            // Test AppendEntries response round-trip
            {
                kythira::append_entries_response<> original;
                original._term = term_dist(rng);
                original._success = bool_dist(rng) == 1;
                
                // Randomly include conflict information
                if (bool_dist(rng) == 1) {
                    original._conflict_index = index_dist(rng);
                }
                if (bool_dist(rng) == 1) {
                    original._conflict_term = term_dist(rng);
                }
                
                auto serialized = serializer.serialize(original);
                auto deserialized = serializer.deserialize_append_entries_response(serialized);
                
                if (original._term != deserialized._term ||
                    original._success != deserialized._success ||
                    original._conflict_index != deserialized._conflict_index ||
                    original._conflict_term != deserialized._conflict_term) {
                    failures++;
                    BOOST_TEST_MESSAGE("AppendEntries response round-trip failed at iteration " << i);
                }
            }
            
            // Test InstallSnapshot request round-trip
            {
                kythira::install_snapshot_request<> original;
                original._term = term_dist(rng);
                original._leader_id = node_dist(rng);
                original._last_included_index = index_dist(rng);
                original._last_included_term = term_dist(rng);
                original._offset = data_size_dist(rng);
                original._done = bool_dist(rng) == 1;
                
                // Generate random snapshot data
                std::size_t data_size = data_size_dist(rng);
                original._data.resize(data_size);
                for (std::size_t j = 0; j < data_size; ++j) {
                    original._data[j] = static_cast<std::byte>(byte_dist(rng));
                }
                
                auto serialized = serializer.serialize(original);
                auto deserialized = serializer.deserialize_install_snapshot_request(serialized);
                
                if (original._term != deserialized._term ||
                    original._leader_id != deserialized._leader_id ||
                    original._last_included_index != deserialized._last_included_index ||
                    original._last_included_term != deserialized._last_included_term ||
                    original._offset != deserialized._offset ||
                    original._done != deserialized._done ||
                    original._data != deserialized._data) {
                    failures++;
                    BOOST_TEST_MESSAGE("InstallSnapshot request round-trip failed at iteration " << i);
                }
            }
            
            // Test InstallSnapshot response round-trip
            {
                kythira::install_snapshot_response<> original;
                original._term = term_dist(rng);
                
                auto serialized = serializer.serialize(original);
                auto deserialized = serializer.deserialize_install_snapshot_response(serialized);
                
                if (original._term != deserialized._term) {
                    failures++;
                    BOOST_TEST_MESSAGE("InstallSnapshot response round-trip failed at iteration " << i);
                }
            }
            
        } catch (const std::exception& e) {
            failures++;
            BOOST_TEST_MESSAGE("Exception during serialization round-trip test " << i << ": " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("Message serialization round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    
    BOOST_CHECK_EQUAL(failures, 0);
}

BOOST_AUTO_TEST_SUITE_END()