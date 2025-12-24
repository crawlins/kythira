#define BOOST_TEST_MODULE coap_exponential_backoff_property_test
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
#include <cmath>

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr std::uint64_t max_node_id = 1000;
    constexpr std::chrono::milliseconds min_base_timeout{100};
    constexpr std::chrono::milliseconds max_base_timeout{5000};
    constexpr double min_backoff_factor = 1.1;
    constexpr double max_backoff_factor = 5.0;
    constexpr std::size_t max_retransmission_attempts = 10;
}

BOOST_AUTO_TEST_SUITE(coap_exponential_backoff_property_tests)

// **Feature: coap-transport, Property 7: Exponential backoff retransmission**
// **Validates: Requirements 2.4, 3.3, 8.4**
// Property: For any failed message transmission, retransmission intervals should 
// follow exponential backoff as specified in RFC 7252.
BOOST_AUTO_TEST_CASE(property_exponential_backoff_retransmission, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::uint64_t> node_dist(1, max_node_id);
    std::uniform_int_distribution<std::chrono::milliseconds::rep> timeout_dist(
        min_base_timeout.count(), max_base_timeout.count());
    std::uniform_real_distribution<double> backoff_dist(min_backoff_factor, max_backoff_factor);
    std::uniform_int_distribution<std::size_t> attempts_dist(1, max_retransmission_attempts);
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        try {
            // Generate random test parameters
            std::uint64_t target_node = node_dist(rng);
            auto base_timeout = std::chrono::milliseconds{timeout_dist(rng)};
            double backoff_factor = backoff_dist(rng);
            std::size_t max_attempts = attempts_dist(rng);
            
            // Create client configuration with exponential backoff
            raft::coap_client_config config;
            config.use_confirmable_messages = true;
            config.retransmission_timeout = base_timeout;
            config.exponential_backoff_factor = backoff_factor;
            config.max_retransmissions = max_attempts;
            
            // Create endpoint mapping
            std::unordered_map<std::uint64_t, std::string> endpoints;
            endpoints[target_node] = "coap://127.0.0.1:5683";
            
            // Create client
            raft::noop_metrics metrics;
            raft::console_logger logger;
            raft::coap_client<raft::json_rpc_serializer<std::vector<std::byte>>, raft::noop_metrics, raft::console_logger> 
                client(std::move(endpoints), config, metrics, std::move(logger));
            
            // Test exponential backoff calculation
            std::vector<std::chrono::milliseconds> timeouts;
            
            // Calculate timeouts for multiple retransmission attempts
            for (std::size_t attempt = 0; attempt <= max_attempts; ++attempt) {
                auto timeout = client.calculate_retransmission_timeout(attempt);
                timeouts.push_back(timeout);
                
                // Verify the timeout is positive
                BOOST_CHECK_GT(timeout.count(), 0);
                
                // For attempt 0, timeout should equal base timeout
                if (attempt == 0) {
                    BOOST_CHECK_EQUAL(timeout.count(), base_timeout.count());
                }
            }
            
            // Verify exponential growth property
            for (std::size_t attempt = 1; attempt < timeouts.size(); ++attempt) {
                auto current_timeout = timeouts[attempt];
                auto previous_timeout = timeouts[attempt - 1];
                
                // Current timeout should be >= previous timeout (monotonic increase)
                BOOST_CHECK_GE(current_timeout.count(), previous_timeout.count());
                
                // Calculate expected timeout based on exponential backoff
                auto expected_timeout = std::chrono::milliseconds{
                    static_cast<std::chrono::milliseconds::rep>(
                        base_timeout.count() * std::pow(backoff_factor, attempt))
                };
                
                // Allow for small floating-point rounding errors
                auto tolerance = std::chrono::milliseconds{1};
                auto diff = std::abs(current_timeout.count() - expected_timeout.count());
                BOOST_CHECK_LE(diff, tolerance.count());
            }
            
            // Test that backoff factor affects the growth rate
            if (backoff_factor > 1.0 && timeouts.size() >= 3) {
                // With exponential backoff, later timeouts should grow faster
                auto first_increase = timeouts[1].count() - timeouts[0].count();
                auto second_increase = timeouts[2].count() - timeouts[1].count();
                
                // Second increase should be larger than first (exponential growth)
                BOOST_CHECK_GT(second_increase, first_increase);
            }
            
            // Test edge cases
            {
                // Test with backoff factor of 1.0 (no exponential growth)
                raft::coap_client_config no_backoff_config;
                no_backoff_config.retransmission_timeout = base_timeout;
                no_backoff_config.exponential_backoff_factor = 1.0;
                
                std::unordered_map<std::uint64_t, std::string> no_backoff_endpoints;
                no_backoff_endpoints[1] = "coap://127.0.0.1:5683";
                
                raft::console_logger no_backoff_logger;
                raft::coap_client<raft::json_rpc_serializer<std::vector<std::byte>>, raft::noop_metrics, raft::console_logger> 
                    no_backoff_client(std::move(no_backoff_endpoints), no_backoff_config, metrics, std::move(no_backoff_logger));
                
                // All timeouts should be the same with backoff factor 1.0
                auto timeout1 = no_backoff_client.calculate_retransmission_timeout(0);
                auto timeout2 = no_backoff_client.calculate_retransmission_timeout(1);
                auto timeout3 = no_backoff_client.calculate_retransmission_timeout(2);
                
                BOOST_CHECK_EQUAL(timeout1.count(), base_timeout.count());
                BOOST_CHECK_EQUAL(timeout2.count(), base_timeout.count());
                BOOST_CHECK_EQUAL(timeout3.count(), base_timeout.count());
            }
            
            // Test with very large attempt numbers (should not overflow)
            {
                auto large_attempt_timeout = client.calculate_retransmission_timeout(100);
                BOOST_CHECK_GT(large_attempt_timeout.count(), 0);
                
                // Should be much larger than base timeout
                BOOST_CHECK_GT(large_attempt_timeout.count(), base_timeout.count());
            }
            
            // Test consistency - same attempt should give same result
            {
                std::size_t test_attempt = max_attempts / 2;
                auto timeout1 = client.calculate_retransmission_timeout(test_attempt);
                auto timeout2 = client.calculate_retransmission_timeout(test_attempt);
                BOOST_CHECK_EQUAL(timeout1.count(), timeout2.count());
            }
            
        } catch (const std::exception& e) {
            failures++;
            BOOST_TEST_MESSAGE("Exception during exponential backoff test " << i << ": " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("Exponential backoff retransmission: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    
    BOOST_CHECK_EQUAL(failures, 0);
}

BOOST_AUTO_TEST_SUITE_END()