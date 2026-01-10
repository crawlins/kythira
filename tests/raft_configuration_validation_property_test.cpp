#define BOOST_TEST_MODULE RaftConfigurationValidationPropertyTest

#include <boost/test/included/unit_test.hpp>
#include <raft/types.hpp>
#include <folly/init/Init.h>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <set>

using namespace raft;

namespace {
    constexpr std::size_t test_iterations = 15;
    constexpr std::chrono::milliseconds min_timeout{1};
    constexpr std::chrono::milliseconds max_timeout{60000};
    constexpr std::size_t min_size = 1;
    constexpr std::size_t max_size = 1000000;
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

/**
 * **Feature: raft-completion, Property 46: Configuration Validation**
 * 
 * Property: When timeout configurations are invalid, the system rejects them with clear error messages.
 * **Validates: Requirements 9.5**
 */
BOOST_AUTO_TEST_CASE(raft_configuration_validation_property_test, * boost::unit_test::timeout(180)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random configuration values
        std::uniform_int_distribution<int> timeout_dist(min_timeout.count(), max_timeout.count());
        std::uniform_int_distribution<std::size_t> size_dist(min_size, max_size);
        
        auto election_timeout_min = std::chrono::milliseconds{timeout_dist(gen)};
        auto election_timeout_max = std::chrono::milliseconds{std::max(static_cast<int>(election_timeout_min.count()), timeout_dist(gen))};
        auto heartbeat_interval = std::chrono::milliseconds{timeout_dist(gen)};
        auto rpc_timeout = std::chrono::milliseconds{timeout_dist(gen)};
        auto append_entries_timeout = std::chrono::milliseconds{timeout_dist(gen)};
        auto request_vote_timeout = std::chrono::milliseconds{timeout_dist(gen)};
        auto install_snapshot_timeout = std::chrono::milliseconds{timeout_dist(gen)};
        auto max_entries_per_append = size_dist(gen);
        auto snapshot_threshold_bytes = size_dist(gen);
        auto snapshot_chunk_size = size_dist(gen);
        
        BOOST_TEST_MESSAGE("Testing configuration validation with random values");
        
        // Create configuration with random values
        raft_configuration config;
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = heartbeat_interval;
        config._rpc_timeout = rpc_timeout;
        config._append_entries_timeout = append_entries_timeout;
        config._request_vote_timeout = request_vote_timeout;
        config._install_snapshot_timeout = install_snapshot_timeout;
        config._max_entries_per_append = max_entries_per_append;
        config._snapshot_threshold_bytes = snapshot_threshold_bytes;
        config._snapshot_chunk_size = snapshot_chunk_size;
        
        // Property: Configuration validation should always return a boolean result
        bool is_valid = config.validate();
        auto validation_errors = config.get_validation_errors();
        
        // Property: validate() result should match whether there are validation errors
        BOOST_CHECK_EQUAL(is_valid, validation_errors.empty());
        
        // Property: All validation errors should be non-empty strings
        for (const auto& error : validation_errors) {
            BOOST_CHECK(!error.empty());
        }
        
        BOOST_TEST_MESSAGE("✓ Configuration validation consistency verified - Valid: " 
                          << is_valid << ", Errors: " << validation_errors.size());
    }
    
    // Test 1: Default configuration validation
    {
        BOOST_TEST_MESSAGE("Test 1: Default configuration validation");
        raft_configuration default_config;
        
        // Property: Default configuration should be valid
        BOOST_CHECK(default_config.validate());
        
        // Property: Default configuration should have no validation errors
        auto errors = default_config.get_validation_errors();
        BOOST_CHECK(errors.empty());
        
        BOOST_TEST_MESSAGE("✓ Default configuration is valid");
    }
    
    // Test 2: Invalid timeout configurations
    {
        BOOST_TEST_MESSAGE("Test 2: Invalid timeout configurations");
        
        // Test zero and negative timeouts
        std::vector<std::pair<std::string, std::function<void(raft_configuration&)>>> invalid_timeout_configs = {
            {"zero election_timeout_min", [](raft_configuration& config) {
                config._election_timeout_min = std::chrono::milliseconds{0};
            }},
            {"zero heartbeat_interval", [](raft_configuration& config) {
                config._heartbeat_interval = std::chrono::milliseconds{0};
            }},
            {"zero rpc_timeout", [](raft_configuration& config) {
                config._rpc_timeout = std::chrono::milliseconds{0};
            }},
            {"zero append_entries_timeout", [](raft_configuration& config) {
                config._append_entries_timeout = std::chrono::milliseconds{0};
            }},
            {"zero request_vote_timeout", [](raft_configuration& config) {
                config._request_vote_timeout = std::chrono::milliseconds{0};
            }},
            {"zero install_snapshot_timeout", [](raft_configuration& config) {
                config._install_snapshot_timeout = std::chrono::milliseconds{0};
            }},
            {"election_timeout_max less than min", [](raft_configuration& config) {
                config._election_timeout_min = std::chrono::milliseconds{300};
                config._election_timeout_max = std::chrono::milliseconds{200};
            }}
        };
        
        for (const auto& [description, modifier] : invalid_timeout_configs) {
            raft_configuration config;
            modifier(config);
            
            // Property: Invalid timeout configurations should fail validation
            BOOST_CHECK(!config.validate());
            
            auto errors = config.get_validation_errors();
            
            // Property: There should be at least one validation error
            BOOST_CHECK(!errors.empty());
            
            // Property: Error messages should be descriptive
            bool found_relevant_error = std::any_of(errors.begin(), errors.end(),
                [&description](const std::string& error) {
                    return error.find("timeout") != std::string::npos ||
                           error.find("positive") != std::string::npos ||
                           error.find("greater") != std::string::npos;
                });
            BOOST_CHECK(found_relevant_error);
            
            BOOST_TEST_MESSAGE("✓ Invalid timeout configuration rejected: " << description);
        }
    }
    
    // Test 3: Invalid size configurations
    {
        BOOST_TEST_MESSAGE("Test 3: Invalid size configurations");
        
        std::vector<std::pair<std::string, std::function<void(raft_configuration&)>>> invalid_size_configs = {
            {"zero max_entries_per_append", [](raft_configuration& config) {
                config._max_entries_per_append = 0;
            }},
            {"zero snapshot_threshold_bytes", [](raft_configuration& config) {
                config._snapshot_threshold_bytes = 0;
            }},
            {"zero snapshot_chunk_size", [](raft_configuration& config) {
                config._snapshot_chunk_size = 0;
            }},
            {"chunk_size greater than threshold", [](raft_configuration& config) {
                config._snapshot_threshold_bytes = 1000;
                config._snapshot_chunk_size = 2000;
            }}
        };
        
        for (const auto& [description, modifier] : invalid_size_configs) {
            raft_configuration config;
            modifier(config);
            
            // Property: Invalid size configurations should fail validation
            BOOST_CHECK(!config.validate());
            
            auto errors = config.get_validation_errors();
            
            // Property: There should be at least one validation error
            BOOST_CHECK(!errors.empty());
            
            // Property: Error messages should be descriptive
            bool found_relevant_error = std::any_of(errors.begin(), errors.end(),
                [](const std::string& error) {
                    return error.find("positive") != std::string::npos ||
                           error.find("exceed") != std::string::npos ||
                           error.find("chunk") != std::string::npos ||
                           error.find("threshold") != std::string::npos;
                });
            BOOST_CHECK(found_relevant_error);
            
            BOOST_TEST_MESSAGE("✓ Invalid size configuration rejected: " << description);
        }
    }
    
    // Test 4: Invalid retry policy configurations
    {
        BOOST_TEST_MESSAGE("Test 4: Invalid retry policy configurations");
        
        std::vector<std::pair<std::string, std::function<void(raft_configuration&)>>> invalid_retry_configs = {
            {"invalid heartbeat retry policy", [](raft_configuration& config) {
                config._heartbeat_retry_policy.initial_delay = std::chrono::milliseconds{0};
            }},
            {"invalid append_entries retry policy", [](raft_configuration& config) {
                config._append_entries_retry_policy.max_delay = std::chrono::milliseconds{50};
                config._append_entries_retry_policy.initial_delay = std::chrono::milliseconds{100};
            }},
            {"invalid request_vote retry policy", [](raft_configuration& config) {
                config._request_vote_retry_policy.backoff_multiplier = 1.0;
            }},
            {"invalid install_snapshot retry policy", [](raft_configuration& config) {
                config._install_snapshot_retry_policy.jitter_factor = -0.1;
            }}
        };
        
        for (const auto& [description, modifier] : invalid_retry_configs) {
            raft_configuration config;
            modifier(config);
            
            // Property: Invalid retry policy configurations should fail validation
            BOOST_CHECK(!config.validate());
            
            auto errors = config.get_validation_errors();
            
            // Property: There should be at least one validation error
            BOOST_CHECK(!errors.empty());
            
            // Property: Error messages should mention retry policy
            bool found_retry_policy_error = std::any_of(errors.begin(), errors.end(),
                [](const std::string& error) {
                    return error.find("retry_policy") != std::string::npos;
                });
            BOOST_CHECK(found_retry_policy_error);
            
            BOOST_TEST_MESSAGE("✓ Invalid retry policy configuration rejected: " << description);
        }
    }
    
    // Test 5: Invalid adaptive timeout configurations
    {
        BOOST_TEST_MESSAGE("Test 5: Invalid adaptive timeout configurations");
        
        std::vector<std::pair<std::string, std::function<void(raft_configuration&)>>> invalid_adaptive_configs = {
            {"invalid adaptive timeout config - zero min_timeout", [](raft_configuration& config) {
                config._adaptive_timeout_config.min_timeout = std::chrono::milliseconds{0};
            }},
            {"invalid adaptive timeout config - max less than min", [](raft_configuration& config) {
                config._adaptive_timeout_config.min_timeout = std::chrono::milliseconds{1000};
                config._adaptive_timeout_config.max_timeout = std::chrono::milliseconds{500};
            }},
            {"invalid adaptive timeout config - bad adaptation factor", [](raft_configuration& config) {
                config._adaptive_timeout_config.adaptation_factor = 1.0;
            }},
            {"invalid adaptive timeout config - zero sample window", [](raft_configuration& config) {
                config._adaptive_timeout_config.sample_window_size = 0;
            }}
        };
        
        for (const auto& [description, modifier] : invalid_adaptive_configs) {
            raft_configuration config;
            modifier(config);
            
            // Property: Invalid adaptive timeout configurations should fail validation
            BOOST_CHECK(!config.validate());
            
            auto errors = config.get_validation_errors();
            
            // Property: There should be at least one validation error
            BOOST_CHECK(!errors.empty());
            
            // Property: Error messages should mention adaptive timeout
            bool found_adaptive_error = std::any_of(errors.begin(), errors.end(),
                [](const std::string& error) {
                    return error.find("adaptive_timeout") != std::string::npos;
                });
            BOOST_CHECK(found_adaptive_error);
            
            BOOST_TEST_MESSAGE("✓ Invalid adaptive timeout configuration rejected: " << description);
        }
    }
    
    // Test 6: Heartbeat interval compatibility validation
    {
        BOOST_TEST_MESSAGE("Test 6: Heartbeat interval compatibility validation");
        
        raft_configuration incompatible_config;
        incompatible_config._heartbeat_interval = std::chrono::milliseconds{200};
        incompatible_config._election_timeout_min = std::chrono::milliseconds{400}; // Ratio: 2.0 (should fail)
        
        // Property: Incompatible heartbeat/election timeout should fail validation
        BOOST_CHECK(!incompatible_config.validate());
        
        auto errors = incompatible_config.get_validation_errors();
        
        // Property: Should have compatibility error
        bool found_compatibility_error = std::any_of(errors.begin(), errors.end(),
            [](const std::string& error) {
                return error.find("heartbeat_interval") != std::string::npos &&
                       error.find("election_timeout") != std::string::npos;
            });
        BOOST_CHECK(found_compatibility_error);
        
        BOOST_TEST_MESSAGE("✓ Heartbeat interval compatibility validation works");
    }
    
    // Test 7: Multiple validation errors
    {
        BOOST_TEST_MESSAGE("Test 7: Multiple validation errors");
        
        raft_configuration multi_error_config;
        
        // Introduce multiple errors
        multi_error_config._election_timeout_min = std::chrono::milliseconds{0}; // Error 1
        multi_error_config._heartbeat_interval = std::chrono::milliseconds{0}; // Error 2
        multi_error_config._max_entries_per_append = 0; // Error 3
        multi_error_config._snapshot_chunk_size = 2000; // Error 4 (with threshold = 1000)
        multi_error_config._snapshot_threshold_bytes = 1000;
        multi_error_config._heartbeat_retry_policy.max_attempts = 0; // Error 5
        
        // Property: Configuration with multiple errors should fail validation
        BOOST_CHECK(!multi_error_config.validate());
        
        auto errors = multi_error_config.get_validation_errors();
        
        // Property: Should have multiple validation errors
        BOOST_CHECK_GE(errors.size(), 3); // At least 3 errors
        
        // Property: Each error should be unique
        std::set<std::string> unique_errors(errors.begin(), errors.end());
        BOOST_CHECK_EQUAL(unique_errors.size(), errors.size());
        
        BOOST_TEST_MESSAGE("✓ Multiple validation errors detected: " << errors.size());
    }
    
    // Test 8: Error message clarity and specificity
    {
        BOOST_TEST_MESSAGE("Test 8: Error message clarity and specificity");
        
        std::vector<std::pair<std::string, std::function<void(raft_configuration&)>>> specific_error_tests = {
            {"election_timeout_min must be positive", [](raft_configuration& config) {
                config._election_timeout_min = std::chrono::milliseconds{0};
            }},
            {"heartbeat_interval must be positive", [](raft_configuration& config) {
                config._heartbeat_interval = std::chrono::milliseconds{0};
            }},
            {"max_entries_per_append must be positive", [](raft_configuration& config) {
                config._max_entries_per_append = 0;
            }},
            {"snapshot_chunk_size should not exceed threshold", [](raft_configuration& config) {
                config._snapshot_threshold_bytes = 1000;
                config._snapshot_chunk_size = 2000;
            }}
        };
        
        for (const auto& [expected_error_content, modifier] : specific_error_tests) {
            raft_configuration config;
            modifier(config);
            
            auto errors = config.get_validation_errors();
            
            // Property: Should have at least one error
            BOOST_CHECK(!errors.empty());
            
            // Property: Error message should contain expected content
            bool found_expected_content = std::any_of(errors.begin(), errors.end(),
                [&expected_error_content](const std::string& error) {
                    return error.find("positive") != std::string::npos ||
                           error.find("exceed") != std::string::npos ||
                           error.find("greater") != std::string::npos;
                });
            BOOST_CHECK(found_expected_content);
            
            BOOST_TEST_MESSAGE("✓ Error message contains expected content for: " << expected_error_content);
        }
    }
    
    // Test 9: Valid configuration edge cases
    {
        BOOST_TEST_MESSAGE("Test 9: Valid configuration edge cases");
        
        // Test minimum valid values
        raft_configuration min_valid_config;
        min_valid_config._election_timeout_min = std::chrono::milliseconds{3};
        min_valid_config._election_timeout_max = std::chrono::milliseconds{4};
        min_valid_config._heartbeat_interval = std::chrono::milliseconds{1}; // 3/1 = 3.0 (exactly at boundary)
        min_valid_config._rpc_timeout = std::chrono::milliseconds{1};
        min_valid_config._append_entries_timeout = std::chrono::milliseconds{1};
        min_valid_config._request_vote_timeout = std::chrono::milliseconds{1};
        min_valid_config._install_snapshot_timeout = std::chrono::milliseconds{1};
        min_valid_config._max_entries_per_append = 1;
        min_valid_config._snapshot_threshold_bytes = 1;
        min_valid_config._snapshot_chunk_size = 1;
        
        // Property: Minimum valid configuration should pass validation
        BOOST_CHECK(min_valid_config.validate());
        
        // Test large valid values
        raft_configuration large_valid_config;
        large_valid_config._election_timeout_min = std::chrono::milliseconds{30000};
        large_valid_config._election_timeout_max = std::chrono::milliseconds{60000};
        large_valid_config._heartbeat_interval = std::chrono::milliseconds{10000}; // 30000/10000 = 3.0
        large_valid_config._rpc_timeout = std::chrono::milliseconds{30000};
        large_valid_config._append_entries_timeout = std::chrono::milliseconds{60000};
        large_valid_config._request_vote_timeout = std::chrono::milliseconds{30000};
        large_valid_config._install_snapshot_timeout = std::chrono::milliseconds{300000};
        large_valid_config._max_entries_per_append = 10000;
        large_valid_config._snapshot_threshold_bytes = 1000000000;
        large_valid_config._snapshot_chunk_size = 100000000;
        
        // Property: Large valid configuration should pass validation
        BOOST_CHECK(large_valid_config.validate());
        
        BOOST_TEST_MESSAGE("✓ Valid configuration edge cases handled correctly");
    }
    
    // Test 10: Random configuration validation stress test
    {
        BOOST_TEST_MESSAGE("Test 10: Random configuration validation stress test");
        
        std::uniform_int_distribution<int> timeout_dist(1, 10000);
        std::uniform_int_distribution<std::size_t> size_dist(1, 100000);
        std::uniform_real_distribution<double> multiplier_dist(1.1, 5.0);
        std::uniform_real_distribution<double> jitter_dist(0.0, 1.0);
        std::uniform_int_distribution<std::size_t> attempts_dist(1, 20);
        
        int valid_configs = 0;
        int invalid_configs = 0;
        
        for (int i = 0; i < 100; ++i) {
            raft_configuration config;
            
            // Generate random values
            auto election_min = std::chrono::milliseconds{timeout_dist(gen)};
            auto election_max = std::chrono::milliseconds{std::max(static_cast<int>(election_min.count()), timeout_dist(gen))};
            auto heartbeat = std::chrono::milliseconds{timeout_dist(gen)};
            
            config._election_timeout_min = election_min;
            config._election_timeout_max = election_max;
            config._heartbeat_interval = heartbeat;
            config._rpc_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            config._append_entries_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            config._request_vote_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            config._install_snapshot_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            config._max_entries_per_append = size_dist(gen);
            config._snapshot_threshold_bytes = size_dist(gen);
            config._snapshot_chunk_size = std::min(config._snapshot_threshold_bytes, size_dist(gen));
            
            // Configure retry policies
            config._heartbeat_retry_policy.initial_delay = std::chrono::milliseconds{timeout_dist(gen) / 10};
            config._heartbeat_retry_policy.max_delay = std::chrono::milliseconds{timeout_dist(gen)};
            config._heartbeat_retry_policy.backoff_multiplier = multiplier_dist(gen);
            config._heartbeat_retry_policy.jitter_factor = jitter_dist(gen);
            config._heartbeat_retry_policy.max_attempts = attempts_dist(gen);
            
            bool is_valid = config.validate();
            auto errors = config.get_validation_errors();
            
            // Property: validate() result should match error list emptiness
            BOOST_CHECK_EQUAL(is_valid, errors.empty());
            
            if (is_valid) {
                valid_configs++;
            } else {
                invalid_configs++;
                
                // Property: Invalid configurations should have descriptive errors
                BOOST_CHECK(!errors.empty());
                for (const auto& error : errors) {
                    BOOST_CHECK(!error.empty());
                }
            }
        }
        
        BOOST_TEST_MESSAGE("✓ Random validation stress test - Valid: " << valid_configs 
                          << ", Invalid: " << invalid_configs);
    }
    
    BOOST_TEST_MESSAGE("All configuration validation property tests passed!");
}