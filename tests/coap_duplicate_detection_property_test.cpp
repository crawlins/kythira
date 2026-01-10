#define BOOST_TEST_MODULE coap_duplicate_detection_property_test
#include <boost/test/unit_test.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT 30

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/types.hpp>
#include <raft/console_logger.hpp>

#include <random>
#include <vector>
#include <string>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr std::uint64_t max_node_id = 1000;
    constexpr std::uint16_t max_message_id = 65535;
    constexpr std::size_t max_duplicate_count = 10;
}

BOOST_AUTO_TEST_SUITE(coap_duplicate_detection_property_tests)

// **Feature: coap-transport, Property 5: Duplicate message detection**
// **Validates: Requirements 3.2**
// Property: For any CoAP message with the same Message ID received multiple times, 
// only the first occurrence should be processed.
BOOST_AUTO_TEST_CASE(property_duplicate_message_detection, * boost::unit_test::timeout(45)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::uint64_t> node_dist(1, max_node_id);
    std::uniform_int_distribution<std::uint16_t> msg_id_dist(1, max_message_id);
    std::uniform_int_distribution<std::size_t> duplicate_count_dist(2, max_duplicate_count);
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        try {
            // Test client-side duplicate detection
            {
                // Create client configuration
                kythira::coap_client_config config;
                
                // Create endpoint mapping
                std::unordered_map<std::uint64_t, std::string> endpoints;
                std::uint64_t target_node = node_dist(rng);
                endpoints[target_node] = "coap://127.0.0.1:5683";
                
                // Create client
                kythira::noop_metrics metrics;
                kythira::console_logger logger;
                kythira::coap_client<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
                    client(std::move(endpoints), config, metrics, std::move(logger));
                
                // Generate random message IDs and test duplicate detection
                std::unordered_set<std::uint16_t> seen_message_ids;
                std::vector<std::uint16_t> test_message_ids;
                
                // Generate some unique message IDs
                for (std::size_t j = 0; j < 20; ++j) {
                    std::uint16_t msg_id = msg_id_dist(rng);
                    test_message_ids.push_back(msg_id);
                    seen_message_ids.insert(msg_id);
                }
                
                // Test that initially no messages are duplicates
                for (auto msg_id : test_message_ids) {
                    BOOST_CHECK(!client.is_duplicate_message(msg_id));
                }
                
                // Record messages as received
                for (auto msg_id : test_message_ids) {
                    client.record_received_message(msg_id);
                }
                
                // Now all messages should be detected as duplicates
                for (auto msg_id : test_message_ids) {
                    BOOST_CHECK(client.is_duplicate_message(msg_id));
                }
                
                // Test that new message IDs are not duplicates
                std::uint16_t new_msg_id = msg_id_dist(rng);
                while (seen_message_ids.count(new_msg_id) > 0) {
                    new_msg_id = msg_id_dist(rng);
                }
                BOOST_CHECK(!client.is_duplicate_message(new_msg_id));
            }
            
            // Test server-side duplicate detection
            {
                // Create server configuration
                kythira::coap_server_config config;
                
                // Create server
                kythira::noop_metrics metrics;
                kythira::console_logger logger;
                kythira::coap_server<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
                    server("127.0.0.1", 5683, config, metrics, std::move(logger));
                
                // Generate random message IDs and test duplicate detection
                std::unordered_set<std::uint16_t> seen_message_ids;
                std::vector<std::uint16_t> test_message_ids;
                
                // Generate some unique message IDs
                for (std::size_t j = 0; j < 20; ++j) {
                    std::uint16_t msg_id = msg_id_dist(rng);
                    test_message_ids.push_back(msg_id);
                    seen_message_ids.insert(msg_id);
                }
                
                // Test that initially no messages are duplicates
                for (auto msg_id : test_message_ids) {
                    BOOST_CHECK(!server.is_duplicate_message(msg_id));
                }
                
                // Record messages as received
                for (auto msg_id : test_message_ids) {
                    server.record_received_message(msg_id);
                }
                
                // Now all messages should be detected as duplicates
                for (auto msg_id : test_message_ids) {
                    BOOST_CHECK(server.is_duplicate_message(msg_id));
                }
                
                // Test that new message IDs are not duplicates
                std::uint16_t new_msg_id = msg_id_dist(rng);
                while (seen_message_ids.count(new_msg_id) > 0) {
                    new_msg_id = msg_id_dist(rng);
                }
                BOOST_CHECK(!server.is_duplicate_message(new_msg_id));
            }
            
            // Test duplicate detection with multiple occurrences of same message ID
            {
                kythira::coap_client_config config;
                std::unordered_map<std::uint64_t, std::string> endpoints;
                endpoints[1] = "coap://127.0.0.1:5683";
                
                kythira::noop_metrics metrics;
                kythira::console_logger logger;
                kythira::coap_client<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
                    client(std::move(endpoints), config, metrics, std::move(logger));
                
                std::uint16_t msg_id = msg_id_dist(rng);
                std::size_t duplicate_count = duplicate_count_dist(rng);
                
                // First occurrence should not be a duplicate
                BOOST_CHECK(!client.is_duplicate_message(msg_id));
                
                // Record the message
                client.record_received_message(msg_id);
                
                // All subsequent occurrences should be duplicates
                for (std::size_t j = 0; j < duplicate_count; ++j) {
                    BOOST_CHECK(client.is_duplicate_message(msg_id));
                }
                
                // Recording again should not change duplicate status
                client.record_received_message(msg_id);
                BOOST_CHECK(client.is_duplicate_message(msg_id));
            }
            
        } catch (const std::exception& e) {
            failures++;
            BOOST_TEST_MESSAGE("Exception during duplicate detection test " << i << ": " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("Duplicate message detection: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    
    BOOST_CHECK_EQUAL(failures, 0);
}

BOOST_AUTO_TEST_SUITE_END()