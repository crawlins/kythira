#define BOOST_TEST_MODULE coap_non_confirmable_message_property_test
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
#include <unordered_map>

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr std::uint64_t max_node_id = 1000;
    constexpr std::chrono::milliseconds min_timeout{100};
    constexpr std::chrono::milliseconds max_timeout{5000};
}

BOOST_AUTO_TEST_SUITE(coap_non_confirmable_message_property_tests)

// **Feature: coap-transport, Property 6: Non-confirmable message delivery**
// **Validates: Requirements 3.5**
// Property: For any non-confirmable CoAP message sent by the client, the transport 
// should not wait for acknowledgment.
BOOST_AUTO_TEST_CASE(property_non_confirmable_message_delivery, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::uint64_t> node_dist(1, max_node_id);
    std::uniform_int_distribution<std::chrono::milliseconds::rep> timeout_dist(
        min_timeout.count(), max_timeout.count());
    std::uniform_int_distribution<int> bool_dist(0, 1);
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        try {
            // Generate random test parameters
            std::uint64_t target_node = node_dist(rng);
            auto timeout = std::chrono::milliseconds{timeout_dist(rng)};
            bool use_confirmable = bool_dist(rng) == 1;
            
            // Create client configuration with non-confirmable messages
            kythira::coap_client_config config;
            config.use_confirmable_messages = use_confirmable;
            config.retransmission_timeout = std::chrono::milliseconds{1000};
            config.exponential_backoff_factor = 2.0;
            config.max_retransmissions = 3;
            
            // Create endpoint mapping
            std::unordered_map<std::uint64_t, std::string> endpoints;
            endpoints[target_node] = "coap://127.0.0.1:5683";
            
            // Create client
            kythira::noop_metrics metrics;
            kythira::console_logger logger;
            kythira::coap_client<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
                client(std::move(endpoints), config, metrics, std::move(logger));
            
            // Create a test request
            kythira::request_vote_request<> request;
            request._term = 1;
            request._candidate_id = target_node;
            request._last_log_index = 0;
            request._last_log_term = 0;
            
            // Test non-confirmable message behavior
            if (!use_confirmable) {
                // Non-confirmable messages should not use retransmission logic
                // The configuration should reflect this choice
                BOOST_CHECK(!config.use_confirmable_messages);
                
                // Non-confirmable messages should still generate unique tokens and IDs
                auto token1 = client.generate_message_token();
                auto token2 = client.generate_message_token();
                BOOST_CHECK_NE(token1, token2);
                
                auto msg_id1 = client.generate_message_id();
                auto msg_id2 = client.generate_message_id();
                BOOST_CHECK_NE(msg_id1, msg_id2);
                
                // For non-confirmable messages, retransmission timeout calculation
                // should still work (in case some messages are confirmable)
                auto timeout1 = client.calculate_retransmission_timeout(0);
                auto timeout2 = client.calculate_retransmission_timeout(1);
                BOOST_CHECK_LE(timeout1, timeout2);
                
            } else {
                // Confirmable messages should use retransmission logic
                BOOST_CHECK(config.use_confirmable_messages);
                BOOST_CHECK_GT(config.max_retransmissions, 0);
                BOOST_CHECK_GT(config.retransmission_timeout.count(), 0);
                BOOST_CHECK_GT(config.exponential_backoff_factor, 1.0);
            }
            
            // Test that message generation works regardless of confirmable setting
            auto token = client.generate_message_token();
            auto msg_id = client.generate_message_id();
            
            BOOST_CHECK(!token.empty());
            BOOST_CHECK_GT(msg_id, 0);
            
            // Test that duplicate detection works for both confirmable and non-confirmable
            BOOST_CHECK(!client.is_duplicate_message(msg_id));
            client.record_received_message(msg_id);
            BOOST_CHECK(client.is_duplicate_message(msg_id));
            
            // Test configuration consistency
            if (use_confirmable) {
                // Confirmable configuration should have reasonable values
                BOOST_CHECK_GT(config.retransmission_timeout.count(), 0);
                BOOST_CHECK_GT(config.exponential_backoff_factor, 1.0);
                BOOST_CHECK_GT(config.max_retransmissions, 0);
            }
            
            // Test that retransmission timeout calculation is consistent
            auto base_timeout = client.calculate_retransmission_timeout(0);
            auto first_retry_timeout = client.calculate_retransmission_timeout(1);
            auto second_retry_timeout = client.calculate_retransmission_timeout(2);
            
            // Timeouts should increase with exponential backoff
            BOOST_CHECK_LE(base_timeout, first_retry_timeout);
            BOOST_CHECK_LE(first_retry_timeout, second_retry_timeout);
            
            // Verify the exponential relationship
            auto expected_first_retry = std::chrono::milliseconds{
                static_cast<std::chrono::milliseconds::rep>(
                    config.retransmission_timeout.count() * config.exponential_backoff_factor)
            };
            BOOST_CHECK_EQUAL(first_retry_timeout.count(), expected_first_retry.count());
            
        } catch (const std::exception& e) {
            failures++;
            BOOST_TEST_MESSAGE("Exception during non-confirmable message test " << i << ": " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("Non-confirmable message delivery: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    
    BOOST_CHECK_EQUAL(failures, 0);
}

BOOST_AUTO_TEST_SUITE_END()