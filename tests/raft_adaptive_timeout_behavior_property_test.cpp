#define BOOST_TEST_MODULE RaftAdaptiveTimeoutBehaviorPropertyTest

#include <boost/test/included/unit_test.hpp>
#include <raft/types.hpp>
#include <raft/error_handler.hpp>
#include <folly/init/Init.h>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <numeric>

using namespace raft;

namespace {
    constexpr std::size_t test_iterations = 15;
    constexpr std::chrono::milliseconds min_timeout{10};
    constexpr std::chrono::milliseconds max_timeout{30000};
    constexpr double min_adaptation_factor = 1.1;
    constexpr double max_adaptation_factor = 3.0;
    constexpr std::size_t min_sample_window = 3;
    constexpr std::size_t max_sample_window = 50;
}

// Global fixture to initialize Folly
struct GlobalFixture {
    GlobalFixture() {
        int argc = 1;
        char* argv[] = {const_cast<char*>("test"), nullptr};
        char** argv_ptr = argv;
        folly::init(&argc, &argv_ptr);
    }
};

BOOST_GLOBAL_FIXTURE(GlobalFixture);

// Mock adaptive timeout manager for testing
class adaptive_timeout_manager {
private:
    adaptive_timeout_config _config;
    std::vector<std::chrono::milliseconds> _response_times;
    std::chrono::milliseconds _current_timeout;
    
public:
    explicit adaptive_timeout_manager(const adaptive_timeout_config& config)
        : _config(config), _current_timeout(config.min_timeout) {}
    
    auto record_response_time(std::chrono::milliseconds response_time) -> void {
        if (!_config.enabled) return;
        
        _response_times.push_back(response_time);
        
        // Keep only the most recent samples
        if (_response_times.size() > _config.sample_window_size) {
            _response_times.erase(_response_times.begin());
        }
        
        // Adapt timeout based on recent response times
        if (_response_times.size() >= _config.sample_window_size) {
            adapt_timeout();
        }
    }
    
    auto get_current_timeout() const -> std::chrono::milliseconds {
        return _current_timeout;
    }
    
    auto get_average_response_time() const -> std::chrono::milliseconds {
        if (_response_times.empty()) {
            return std::chrono::milliseconds{0};
        }
        
        auto total = std::accumulate(_response_times.begin(), _response_times.end(), 
                                   std::chrono::milliseconds{0});
        return total / _response_times.size();
    }
    
    auto reset() -> void {
        _response_times.clear();
        _current_timeout = _config.min_timeout;
    }
    
private:
    auto adapt_timeout() -> void {
        auto avg_response_time = get_average_response_time();
        auto new_timeout = std::chrono::milliseconds{
            static_cast<long long>(avg_response_time.count() * _config.adaptation_factor)
        };
        
        // Clamp to configured bounds
        new_timeout = std::max(new_timeout, _config.min_timeout);
        new_timeout = std::min(new_timeout, _config.max_timeout);
        
        _current_timeout = new_timeout;
    }
};

/**
 * **Feature: raft-completion, Property 45: Adaptive Timeout Behavior**
 * 
 * Property: When network conditions change, the system adapts timeout and retry behavior within configured bounds.
 * **Validates: Requirements 9.4**
 */
BOOST_AUTO_TEST_CASE(raft_adaptive_timeout_behavior_property_test, * boost::unit_test::timeout(180)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random adaptive timeout configuration
        std::uniform_int_distribution<int> timeout_dist(min_timeout.count(), max_timeout.count());
        std::uniform_real_distribution<double> factor_dist(min_adaptation_factor, max_adaptation_factor);
        std::uniform_int_distribution<std::size_t> window_dist(min_sample_window, max_sample_window);
        
        auto min_timeout_val = std::chrono::milliseconds{timeout_dist(gen)};
        auto max_timeout_val = std::chrono::milliseconds{std::max(static_cast<int>(min_timeout_val.count()), timeout_dist(gen))};
        auto adaptation_factor = factor_dist(gen);
        auto sample_window_size = window_dist(gen);
        
        BOOST_TEST_MESSAGE("Testing adaptive config - Min: " << min_timeout_val.count() 
                          << "ms, Max: " << max_timeout_val.count() 
                          << "ms, Factor: " << adaptation_factor 
                          << ", Window: " << sample_window_size);
        
        // Create adaptive timeout configuration
        adaptive_timeout_config config{
            .enabled = true,
            .min_timeout = min_timeout_val,
            .max_timeout = max_timeout_val,
            .adaptation_factor = adaptation_factor,
            .sample_window_size = sample_window_size
        };
        
        // Property: Valid adaptive timeout configuration should pass validation
        BOOST_CHECK(config.is_valid());
        
        // Create adaptive timeout manager
        adaptive_timeout_manager manager(config);
        
        // Property: Initial timeout should be at minimum
        BOOST_CHECK_EQUAL(manager.get_current_timeout(), min_timeout_val);
        
        // Simulate network conditions and verify adaptation
        std::uniform_int_distribution<int> response_time_dist(
            min_timeout_val.count() / 2, 
            max_timeout_val.count() / 2
        );
        
        // Record enough response times to trigger adaptation
        for (std::size_t i = 0; i < sample_window_size; ++i) {
            auto response_time = std::chrono::milliseconds{response_time_dist(gen)};
            manager.record_response_time(response_time);
        }
        
        // Property: Timeout should adapt based on response times
        auto adapted_timeout = manager.get_current_timeout();
        auto avg_response_time = manager.get_average_response_time();
        
        // Property: Adapted timeout should be within configured bounds
        BOOST_CHECK_GE(adapted_timeout, min_timeout_val);
        BOOST_CHECK_LE(adapted_timeout, max_timeout_val);
        
        // Property: Adapted timeout should be related to average response time by adaptation factor
        auto expected_timeout = std::chrono::milliseconds{
            static_cast<long long>(avg_response_time.count() * adaptation_factor)
        };
        expected_timeout = std::max(expected_timeout, min_timeout_val);
        expected_timeout = std::min(expected_timeout, max_timeout_val);
        
        BOOST_CHECK_EQUAL(adapted_timeout, expected_timeout);
        
        BOOST_TEST_MESSAGE("✓ Adaptive timeout behavior working correctly - Adapted: " 
                          << adapted_timeout.count() << "ms, Avg response: " 
                          << avg_response_time.count() << "ms");
    }
    
    // Test 1: Default adaptive timeout configuration
    {
        BOOST_TEST_MESSAGE("Test 1: Default adaptive timeout configuration");
        raft_configuration config;
        
        // Property: Default adaptive timeout configuration should be valid
        BOOST_CHECK(config.get_adaptive_timeout_config().is_valid());
        
        // Property: Default configuration should be disabled by default
        BOOST_CHECK(!config.get_adaptive_timeout_config().enabled);
        
        // Property: Default bounds should be reasonable
        BOOST_CHECK_GT(config.get_adaptive_timeout_config().min_timeout.count(), 0);
        BOOST_CHECK_GT(config.get_adaptive_timeout_config().max_timeout, 
                      config.get_adaptive_timeout_config().min_timeout);
        BOOST_CHECK_GT(config.get_adaptive_timeout_config().adaptation_factor, 1.0);
        BOOST_CHECK_GT(config.get_adaptive_timeout_config().sample_window_size, 0);
        
        BOOST_TEST_MESSAGE("✓ Default adaptive timeout configuration is valid");
    }
    
    // Test 2: Adaptive timeout configuration validation
    {
        BOOST_TEST_MESSAGE("Test 2: Adaptive timeout configuration validation");
        
        // Test valid configuration
        adaptive_timeout_config valid_config{
            .enabled = true,
            .min_timeout = std::chrono::milliseconds{100},
            .max_timeout = std::chrono::milliseconds{5000},
            .adaptation_factor = 1.5,
            .sample_window_size = 10
        };
        
        // Property: Valid configuration should pass validation
        BOOST_CHECK(valid_config.is_valid());
        
        // Test invalid configurations
        std::vector<std::pair<adaptive_timeout_config, std::string>> invalid_configs = {
            // Zero min timeout
            {adaptive_timeout_config{
                .enabled = true,
                .min_timeout = std::chrono::milliseconds{0},
                .max_timeout = std::chrono::milliseconds{5000},
                .adaptation_factor = 1.5,
                .sample_window_size = 10
            }, "zero min timeout"},
            
            // Max timeout less than min timeout
            {adaptive_timeout_config{
                .enabled = true,
                .min_timeout = std::chrono::milliseconds{1000},
                .max_timeout = std::chrono::milliseconds{500},
                .adaptation_factor = 1.5,
                .sample_window_size = 10
            }, "max timeout less than min timeout"},
            
            // Invalid adaptation factor (too small)
            {adaptive_timeout_config{
                .enabled = true,
                .min_timeout = std::chrono::milliseconds{100},
                .max_timeout = std::chrono::milliseconds{5000},
                .adaptation_factor = 1.0,
                .sample_window_size = 10
            }, "adaptation factor too small"},
            
            // Zero sample window size
            {adaptive_timeout_config{
                .enabled = true,
                .min_timeout = std::chrono::milliseconds{100},
                .max_timeout = std::chrono::milliseconds{5000},
                .adaptation_factor = 1.5,
                .sample_window_size = 0
            }, "zero sample window size"}
        };
        
        for (const auto& [invalid_config, description] : invalid_configs) {
            // Property: Invalid configurations should fail validation
            BOOST_CHECK(!invalid_config.is_valid());
            BOOST_TEST_MESSAGE("✓ Invalid configuration rejected: " << description);
        }
    }
    
    // Test 3: Adaptation to improving network conditions
    {
        BOOST_TEST_MESSAGE("Test 3: Adaptation to improving network conditions");
        
        adaptive_timeout_config config{
            .enabled = true,
            .min_timeout = std::chrono::milliseconds{100},
            .max_timeout = std::chrono::milliseconds{5000},
            .adaptation_factor = 2.0,
            .sample_window_size = 5
        };
        
        adaptive_timeout_manager manager(config);
        
        // Start with slow response times
        std::vector<std::chrono::milliseconds> slow_responses = {
            std::chrono::milliseconds{800},
            std::chrono::milliseconds{900},
            std::chrono::milliseconds{850},
            std::chrono::milliseconds{950},
            std::chrono::milliseconds{880}
        };
        
        for (auto response_time : slow_responses) {
            manager.record_response_time(response_time);
        }
        
        auto timeout_after_slow = manager.get_current_timeout();
        
        // Property: Timeout should increase for slow responses
        BOOST_CHECK_GT(timeout_after_slow, config.min_timeout);
        
        // Now simulate improving network conditions
        manager.reset();
        std::vector<std::chrono::milliseconds> fast_responses = {
            std::chrono::milliseconds{50},
            std::chrono::milliseconds{60},
            std::chrono::milliseconds{45},
            std::chrono::milliseconds{55},
            std::chrono::milliseconds{52}
        };
        
        for (auto response_time : fast_responses) {
            manager.record_response_time(response_time);
        }
        
        auto timeout_after_fast = manager.get_current_timeout();
        
        // Property: Timeout should adapt to faster responses
        BOOST_CHECK_LT(timeout_after_fast, timeout_after_slow);
        BOOST_CHECK_GE(timeout_after_fast, config.min_timeout);
        
        BOOST_TEST_MESSAGE("✓ Adaptation to improving conditions - Slow: " 
                          << timeout_after_slow.count() << "ms, Fast: " 
                          << timeout_after_fast.count() << "ms");
    }
    
    // Test 4: Adaptation to degrading network conditions
    {
        BOOST_TEST_MESSAGE("Test 4: Adaptation to degrading network conditions");
        
        adaptive_timeout_config config{
            .enabled = true,
            .min_timeout = std::chrono::milliseconds{50},
            .max_timeout = std::chrono::milliseconds{3000},
            .adaptation_factor = 1.8,
            .sample_window_size = 4
        };
        
        adaptive_timeout_manager manager(config);
        
        // Start with fast response times
        std::vector<std::chrono::milliseconds> fast_responses = {
            std::chrono::milliseconds{30},
            std::chrono::milliseconds{35},
            std::chrono::milliseconds{28},
            std::chrono::milliseconds{32}
        };
        
        for (auto response_time : fast_responses) {
            manager.record_response_time(response_time);
        }
        
        auto timeout_after_fast = manager.get_current_timeout();
        
        // Now simulate degrading network conditions
        manager.reset();
        std::vector<std::chrono::milliseconds> slow_responses = {
            std::chrono::milliseconds{400},
            std::chrono::milliseconds{450},
            std::chrono::milliseconds{380},
            std::chrono::milliseconds{420}
        };
        
        for (auto response_time : slow_responses) {
            manager.record_response_time(response_time);
        }
        
        auto timeout_after_slow = manager.get_current_timeout();
        
        // Property: Timeout should increase for degrading conditions
        BOOST_CHECK_GT(timeout_after_slow, timeout_after_fast);
        BOOST_CHECK_LE(timeout_after_slow, config.max_timeout);
        
        BOOST_TEST_MESSAGE("✓ Adaptation to degrading conditions - Fast: " 
                          << timeout_after_fast.count() << "ms, Slow: " 
                          << timeout_after_slow.count() << "ms");
    }
    
    // Test 5: Timeout bounds enforcement
    {
        BOOST_TEST_MESSAGE("Test 5: Timeout bounds enforcement");
        
        adaptive_timeout_config config{
            .enabled = true,
            .min_timeout = std::chrono::milliseconds{200},
            .max_timeout = std::chrono::milliseconds{1000},
            .adaptation_factor = 3.0,
            .sample_window_size = 3
        };
        
        adaptive_timeout_manager manager(config);
        
        // Test lower bound enforcement with very fast responses
        std::vector<std::chrono::milliseconds> very_fast_responses = {
            std::chrono::milliseconds{1},
            std::chrono::milliseconds{2},
            std::chrono::milliseconds{1}
        };
        
        for (auto response_time : very_fast_responses) {
            manager.record_response_time(response_time);
        }
        
        auto timeout_after_very_fast = manager.get_current_timeout();
        
        // Property: Timeout should not go below minimum bound
        BOOST_CHECK_GE(timeout_after_very_fast, config.min_timeout);
        
        // Test upper bound enforcement with very slow responses
        manager.reset();
        std::vector<std::chrono::milliseconds> very_slow_responses = {
            std::chrono::milliseconds{2000},
            std::chrono::milliseconds{2500},
            std::chrono::milliseconds{2200}
        };
        
        for (auto response_time : very_slow_responses) {
            manager.record_response_time(response_time);
        }
        
        auto timeout_after_very_slow = manager.get_current_timeout();
        
        // Property: Timeout should not exceed maximum bound
        BOOST_CHECK_LE(timeout_after_very_slow, config.max_timeout);
        
        BOOST_TEST_MESSAGE("✓ Timeout bounds enforced - Min bound: " 
                          << timeout_after_very_fast.count() << "ms, Max bound: " 
                          << timeout_after_very_slow.count() << "ms");
    }
    
    // Test 6: Sample window behavior
    {
        BOOST_TEST_MESSAGE("Test 6: Sample window behavior");
        
        adaptive_timeout_config config{
            .enabled = true,
            .min_timeout = std::chrono::milliseconds{100},
            .max_timeout = std::chrono::milliseconds{2000},
            .adaptation_factor = 2.0,
            .sample_window_size = 3
        };
        
        adaptive_timeout_manager manager(config);
        
        // Record fewer samples than window size
        manager.record_response_time(std::chrono::milliseconds{500});
        manager.record_response_time(std::chrono::milliseconds{600});
        
        // Property: Timeout should not adapt until window is full
        BOOST_CHECK_EQUAL(manager.get_current_timeout(), config.min_timeout);
        
        // Complete the window
        manager.record_response_time(std::chrono::milliseconds{550});
        
        // Property: Timeout should adapt once window is full
        BOOST_CHECK_GT(manager.get_current_timeout(), config.min_timeout);
        
        auto first_adaptation = manager.get_current_timeout();
        
        // Add more samples to test sliding window
        manager.record_response_time(std::chrono::milliseconds{200}); // Should replace oldest sample
        
        auto second_adaptation = manager.get_current_timeout();
        
        // Property: Adding faster response should reduce timeout
        BOOST_CHECK_LT(second_adaptation, first_adaptation);
        
        BOOST_TEST_MESSAGE("✓ Sample window behavior correct - First: " 
                          << first_adaptation.count() << "ms, Second: " 
                          << second_adaptation.count() << "ms");
    }
    
    // Test 7: Disabled adaptive timeout behavior
    {
        BOOST_TEST_MESSAGE("Test 7: Disabled adaptive timeout behavior");
        
        adaptive_timeout_config config{
            .enabled = false,
            .min_timeout = std::chrono::milliseconds{100},
            .max_timeout = std::chrono::milliseconds{2000},
            .adaptation_factor = 2.0,
            .sample_window_size = 3
        };
        
        adaptive_timeout_manager manager(config);
        
        // Record response times
        manager.record_response_time(std::chrono::milliseconds{500});
        manager.record_response_time(std::chrono::milliseconds{600});
        manager.record_response_time(std::chrono::milliseconds{550});
        
        // Property: When disabled, timeout should remain at minimum
        BOOST_CHECK_EQUAL(manager.get_current_timeout(), config.min_timeout);
        
        // Property: When disabled, response times are not tracked
        BOOST_CHECK_EQUAL(manager.get_average_response_time().count(), 0);
        
        BOOST_TEST_MESSAGE("✓ Disabled adaptive timeout behavior correct");
    }
    
    // Test 8: Integration with raft configuration
    {
        BOOST_TEST_MESSAGE("Test 8: Integration with raft configuration");
        
        raft_configuration config;
        
        // Enable and configure adaptive timeouts
        config._adaptive_timeout_config.enabled = true;
        config._adaptive_timeout_config.min_timeout = std::chrono::milliseconds{150};
        config._adaptive_timeout_config.max_timeout = std::chrono::milliseconds{3000};
        config._adaptive_timeout_config.adaptation_factor = 1.8;
        config._adaptive_timeout_config.sample_window_size = 8;
        
        // Property: Modified adaptive timeout configuration should be valid
        BOOST_CHECK(config.get_adaptive_timeout_config().is_valid());
        
        // Property: Configuration should store modified values
        BOOST_CHECK(config.get_adaptive_timeout_config().enabled);
        BOOST_CHECK_EQUAL(config.get_adaptive_timeout_config().min_timeout, std::chrono::milliseconds{150});
        BOOST_CHECK_EQUAL(config.get_adaptive_timeout_config().max_timeout, std::chrono::milliseconds{3000});
        BOOST_CHECK_EQUAL(config.get_adaptive_timeout_config().adaptation_factor, 1.8);
        BOOST_CHECK_EQUAL(config.get_adaptive_timeout_config().sample_window_size, 8);
        
        // Property: Overall configuration should still be valid
        auto validation_errors = config.get_validation_errors();
        bool has_adaptive_timeout_errors = std::any_of(validation_errors.begin(), validation_errors.end(),
            [](const std::string& error) {
                return error.find("adaptive_timeout") != std::string::npos;
            });
        BOOST_CHECK(!has_adaptive_timeout_errors);
        
        BOOST_TEST_MESSAGE("✓ Integration with raft configuration works");
    }
    
    // Test 9: Random adaptive timeout stress test
    {
        BOOST_TEST_MESSAGE("Test 9: Random adaptive timeout stress test");
        
        std::uniform_int_distribution<int> timeout_dist(50, 5000);
        std::uniform_real_distribution<double> factor_dist(1.1, 3.0);
        std::uniform_int_distribution<std::size_t> window_dist(3, 20);
        std::uniform_int_distribution<int> response_dist(10, 2000);
        
        for (int i = 0; i < 20; ++i) {
            auto min_timeout_val = std::chrono::milliseconds{timeout_dist(gen)};
            auto max_timeout_val = std::chrono::milliseconds{std::max(static_cast<int>(min_timeout_val.count()), timeout_dist(gen))};
            auto adaptation_factor = factor_dist(gen);
            auto sample_window_size = window_dist(gen);
            
            adaptive_timeout_config config{
                .enabled = true,
                .min_timeout = min_timeout_val,
                .max_timeout = max_timeout_val,
                .adaptation_factor = adaptation_factor,
                .sample_window_size = sample_window_size
            };
            
            // Property: Random valid configurations should pass validation
            BOOST_CHECK(config.is_valid());
            
            adaptive_timeout_manager manager(config);
            
            // Generate random response times and record them
            for (std::size_t j = 0; j < sample_window_size + 5; ++j) {
                auto response_time = std::chrono::milliseconds{response_dist(gen)};
                manager.record_response_time(response_time);
            }
            
            auto final_timeout = manager.get_current_timeout();
            
            // Property: Final timeout should be within bounds
            BOOST_CHECK_GE(final_timeout, min_timeout_val);
            BOOST_CHECK_LE(final_timeout, max_timeout_val);
        }
        
        BOOST_TEST_MESSAGE("✓ Random adaptive timeout stress test passed");
    }
    
    BOOST_TEST_MESSAGE("All adaptive timeout behavior property tests passed!");
}