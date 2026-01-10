#define BOOST_TEST_MODULE RaftHeartbeatIntervalCompatibilityPropertyTest

#include <boost/test/included/unit_test.hpp>
#include <raft/types.hpp>
#include <folly/init/Init.h>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <stdexcept>
#include <string>

using namespace raft;

namespace {
    constexpr std::size_t test_iterations = 15;
    constexpr std::chrono::milliseconds min_heartbeat{10};
    constexpr std::chrono::milliseconds max_heartbeat{1000};
    constexpr std::chrono::milliseconds min_election{50};
    constexpr std::chrono::milliseconds max_election{10000};
    constexpr double recommended_ratio = 3.0; // Election timeout should be at least 3x heartbeat interval
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
 * **Feature: raft-completion, Property 44: Heartbeat Interval Compatibility**
 * 
 * Property: When configuring heartbeat intervals, the system ensures the interval is compatible with election timeouts.
 * **Validates: Requirements 9.3**
 */
BOOST_AUTO_TEST_CASE(raft_heartbeat_interval_compatibility_property_test, * boost::unit_test::timeout(180)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random heartbeat and election timeout values
        std::uniform_int_distribution<int> heartbeat_dist(min_heartbeat.count(), max_heartbeat.count());
        std::uniform_int_distribution<int> election_dist(min_election.count(), max_election.count());
        
        auto heartbeat_interval = std::chrono::milliseconds{heartbeat_dist(gen)};
        auto election_timeout_min = std::chrono::milliseconds{election_dist(gen)};
        auto election_timeout_max = std::chrono::milliseconds{
            std::max(static_cast<int>(election_timeout_min.count()), election_dist(gen))
        };
        
        BOOST_TEST_MESSAGE("Testing compatibility - Heartbeat: " << heartbeat_interval.count() 
                          << "ms, Election min: " << election_timeout_min.count() 
                          << "ms, Election max: " << election_timeout_max.count() << "ms");
        
        // Create configuration with these values
        raft_configuration config;
        config._heartbeat_interval = heartbeat_interval;
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        
        // Property: Configuration should detect compatibility issues
        auto validation_errors = config.get_validation_errors();
        
        // Calculate the ratio
        double ratio = static_cast<double>(election_timeout_min.count()) / heartbeat_interval.count();
        
        if (ratio < recommended_ratio) {
            // Property: When heartbeat interval is too large relative to election timeout, validation should fail
            bool has_compatibility_error = std::any_of(validation_errors.begin(), validation_errors.end(),
                [](const std::string& error) {
                    return error.find("heartbeat_interval") != std::string::npos &&
                           error.find("election_timeout") != std::string::npos;
                });
            BOOST_CHECK(has_compatibility_error);
            BOOST_TEST_MESSAGE("✓ Compatibility issue detected (ratio: " << ratio << ")");
        } else {
            // Property: When heartbeat interval is appropriately sized, no compatibility error should occur
            bool has_compatibility_error = std::any_of(validation_errors.begin(), validation_errors.end(),
                [](const std::string& error) {
                    return error.find("heartbeat_interval") != std::string::npos &&
                           error.find("election_timeout") != std::string::npos;
                });
            BOOST_CHECK(!has_compatibility_error);
            BOOST_TEST_MESSAGE("✓ No compatibility issue (ratio: " << ratio << ")");
        }
    }
    
    // Test 1: Default configuration compatibility
    {
        BOOST_TEST_MESSAGE("Test 1: Default configuration compatibility");
        raft_configuration default_config;
        
        // Property: Default configuration should have compatible heartbeat and election timeouts
        auto validation_errors = default_config.get_validation_errors();
        bool has_compatibility_error = std::any_of(validation_errors.begin(), validation_errors.end(),
            [](const std::string& error) {
                return error.find("heartbeat_interval") != std::string::npos &&
                       error.find("election_timeout") != std::string::npos;
            });
        BOOST_CHECK(!has_compatibility_error);
        
        // Property: Default configuration should follow recommended ratio
        double default_ratio = static_cast<double>(default_config.election_timeout_min().count()) / 
                              default_config.heartbeat_interval().count();
        BOOST_CHECK_GE(default_ratio, recommended_ratio);
        
        BOOST_TEST_MESSAGE("✓ Default configuration ratio: " << default_ratio);
    }
    
    // Test 2: Explicit compatibility violations
    {
        BOOST_TEST_MESSAGE("Test 2: Explicit compatibility violations");
        
        std::vector<std::pair<std::chrono::milliseconds, std::chrono::milliseconds>> incompatible_pairs = {
            {std::chrono::milliseconds{100}, std::chrono::milliseconds{150}}, // Ratio: 1.5
            {std::chrono::milliseconds{200}, std::chrono::milliseconds{400}}, // Ratio: 2.0
            {std::chrono::milliseconds{300}, std::chrono::milliseconds{600}}, // Ratio: 2.0
            {std::chrono::milliseconds{500}, std::chrono::milliseconds{1000}}, // Ratio: 2.0
            {std::chrono::milliseconds{1000}, std::chrono::milliseconds{2000}} // Ratio: 2.0
        };
        
        for (const auto& [heartbeat, election_min] : incompatible_pairs) {
            raft_configuration config;
            config._heartbeat_interval = heartbeat;
            config._election_timeout_min = election_min;
            config._election_timeout_max = election_min + std::chrono::milliseconds{100};
            
            // Property: Incompatible configurations should fail validation
            auto validation_errors = config.get_validation_errors();
            bool has_compatibility_error = std::any_of(validation_errors.begin(), validation_errors.end(),
                [](const std::string& error) {
                    return error.find("heartbeat_interval") != std::string::npos &&
                           error.find("election_timeout") != std::string::npos;
                });
            BOOST_CHECK(has_compatibility_error);
            
            double ratio = static_cast<double>(election_min.count()) / heartbeat.count();
            BOOST_TEST_MESSAGE("✓ Incompatible pair rejected - Heartbeat: " << heartbeat.count() 
                              << "ms, Election: " << election_min.count() << "ms, Ratio: " << ratio);
        }
    }
    
    // Test 3: Compatible configurations
    {
        BOOST_TEST_MESSAGE("Test 3: Compatible configurations");
        
        std::vector<std::pair<std::chrono::milliseconds, std::chrono::milliseconds>> compatible_pairs = {
            {std::chrono::milliseconds{50}, std::chrono::milliseconds{200}}, // Ratio: 4.0
            {std::chrono::milliseconds{100}, std::chrono::milliseconds{400}}, // Ratio: 4.0
            {std::chrono::milliseconds{200}, std::chrono::milliseconds{800}}, // Ratio: 4.0
            {std::chrono::milliseconds{300}, std::chrono::milliseconds{1200}}, // Ratio: 4.0
            {std::chrono::milliseconds{500}, std::chrono::milliseconds{2000}} // Ratio: 4.0
        };
        
        for (const auto& [heartbeat, election_min] : compatible_pairs) {
            raft_configuration config;
            config._heartbeat_interval = heartbeat;
            config._election_timeout_min = election_min;
            config._election_timeout_max = election_min + std::chrono::milliseconds{100};
            
            // Property: Compatible configurations should pass validation
            auto validation_errors = config.get_validation_errors();
            bool has_compatibility_error = std::any_of(validation_errors.begin(), validation_errors.end(),
                [](const std::string& error) {
                    return error.find("heartbeat_interval") != std::string::npos &&
                           error.find("election_timeout") != std::string::npos;
                });
            BOOST_CHECK(!has_compatibility_error);
            
            double ratio = static_cast<double>(election_min.count()) / heartbeat.count();
            BOOST_TEST_MESSAGE("✓ Compatible pair accepted - Heartbeat: " << heartbeat.count() 
                              << "ms, Election: " << election_min.count() << "ms, Ratio: " << ratio);
        }
    }
    
    // Test 4: Edge case ratios
    {
        BOOST_TEST_MESSAGE("Test 4: Edge case ratios");
        
        // Test exactly at the boundary (ratio = 3.0)
        raft_configuration boundary_config;
        boundary_config._heartbeat_interval = std::chrono::milliseconds{100};
        boundary_config._election_timeout_min = std::chrono::milliseconds{300}; // Exactly 3x
        boundary_config._election_timeout_max = std::chrono::milliseconds{400};
        
        // Property: Boundary case should pass validation (>= 3.0 is acceptable)
        auto boundary_errors = boundary_config.get_validation_errors();
        bool has_boundary_error = std::any_of(boundary_errors.begin(), boundary_errors.end(),
            [](const std::string& error) {
                return error.find("heartbeat_interval") != std::string::npos &&
                       error.find("election_timeout") != std::string::npos;
            });
        BOOST_CHECK(!has_boundary_error);
        
        // Test just below the boundary (ratio = 2.99)
        raft_configuration below_boundary_config;
        below_boundary_config._heartbeat_interval = std::chrono::milliseconds{100};
        below_boundary_config._election_timeout_min = std::chrono::milliseconds{299}; // Just below 3x
        below_boundary_config._election_timeout_max = std::chrono::milliseconds{400};
        
        // Property: Just below boundary should fail validation
        auto below_boundary_errors = below_boundary_config.get_validation_errors();
        bool has_below_boundary_error = std::any_of(below_boundary_errors.begin(), below_boundary_errors.end(),
            [](const std::string& error) {
                return error.find("heartbeat_interval") != std::string::npos &&
                       error.find("election_timeout") != std::string::npos;
            });
        BOOST_CHECK(has_below_boundary_error);
        
        BOOST_TEST_MESSAGE("✓ Edge case ratios handled correctly");
    }
    
    // Test 5: Very small and very large values
    {
        BOOST_TEST_MESSAGE("Test 5: Very small and very large values");
        
        // Test very small values
        raft_configuration small_config;
        small_config._heartbeat_interval = std::chrono::milliseconds{1};
        small_config._election_timeout_min = std::chrono::milliseconds{5}; // Ratio: 5.0
        small_config._election_timeout_max = std::chrono::milliseconds{10};
        
        // Property: Small compatible values should pass validation
        auto small_errors = small_config.get_validation_errors();
        bool has_small_compatibility_error = std::any_of(small_errors.begin(), small_errors.end(),
            [](const std::string& error) {
                return error.find("heartbeat_interval") != std::string::npos &&
                       error.find("election_timeout") != std::string::npos;
            });
        BOOST_CHECK(!has_small_compatibility_error);
        
        // Test very large values
        raft_configuration large_config;
        large_config._heartbeat_interval = std::chrono::milliseconds{10000};
        large_config._election_timeout_min = std::chrono::milliseconds{40000}; // Ratio: 4.0
        large_config._election_timeout_max = std::chrono::milliseconds{50000};
        
        // Property: Large compatible values should pass validation
        auto large_errors = large_config.get_validation_errors();
        bool has_large_compatibility_error = std::any_of(large_errors.begin(), large_errors.end(),
            [](const std::string& error) {
                return error.find("heartbeat_interval") != std::string::npos &&
                       error.find("election_timeout") != std::string::npos;
            });
        BOOST_CHECK(!has_large_compatibility_error);
        
        BOOST_TEST_MESSAGE("✓ Very small and large values handled correctly");
    }
    
    // Test 6: Practical Raft timing scenarios
    {
        BOOST_TEST_MESSAGE("Test 6: Practical Raft timing scenarios");
        
        // Scenario 1: Fast local network
        raft_configuration fast_network_config;
        fast_network_config._heartbeat_interval = std::chrono::milliseconds{25};
        fast_network_config._election_timeout_min = std::chrono::milliseconds{100}; // Ratio: 4.0
        fast_network_config._election_timeout_max = std::chrono::milliseconds{200};
        
        // Property: Fast network configuration should be compatible
        auto fast_errors = fast_network_config.get_validation_errors();
        bool has_fast_compatibility_error = std::any_of(fast_errors.begin(), fast_errors.end(),
            [](const std::string& error) {
                return error.find("heartbeat_interval") != std::string::npos &&
                       error.find("election_timeout") != std::string::npos;
            });
        BOOST_CHECK(!has_fast_compatibility_error);
        
        // Scenario 2: Slow WAN network
        raft_configuration slow_network_config;
        slow_network_config._heartbeat_interval = std::chrono::milliseconds{500};
        slow_network_config._election_timeout_min = std::chrono::milliseconds{2000}; // Ratio: 4.0
        slow_network_config._election_timeout_max = std::chrono::milliseconds{4000};
        
        // Property: Slow network configuration should be compatible
        auto slow_errors = slow_network_config.get_validation_errors();
        bool has_slow_compatibility_error = std::any_of(slow_errors.begin(), slow_errors.end(),
            [](const std::string& error) {
                return error.find("heartbeat_interval") != std::string::npos &&
                       error.find("election_timeout") != std::string::npos;
            });
        BOOST_CHECK(!has_slow_compatibility_error);
        
        // Scenario 3: Conservative configuration
        raft_configuration conservative_config;
        conservative_config._heartbeat_interval = std::chrono::milliseconds{100};
        conservative_config._election_timeout_min = std::chrono::milliseconds{1000}; // Ratio: 10.0
        conservative_config._election_timeout_max = std::chrono::milliseconds{2000};
        
        // Property: Conservative configuration should be compatible
        auto conservative_errors = conservative_config.get_validation_errors();
        bool has_conservative_compatibility_error = std::any_of(conservative_errors.begin(), conservative_errors.end(),
            [](const std::string& error) {
                return error.find("heartbeat_interval") != std::string::npos &&
                       error.find("election_timeout") != std::string::npos;
            });
        BOOST_CHECK(!has_conservative_compatibility_error);
        
        BOOST_TEST_MESSAGE("✓ Practical Raft timing scenarios validated");
    }
    
    // Test 7: Compatibility with different election timeout ranges
    {
        BOOST_TEST_MESSAGE("Test 7: Compatibility with different election timeout ranges");
        
        std::chrono::milliseconds heartbeat{100};
        
        std::vector<std::pair<std::chrono::milliseconds, std::chrono::milliseconds>> election_ranges = {
            {std::chrono::milliseconds{300}, std::chrono::milliseconds{400}}, // Min ratio: 3.0
            {std::chrono::milliseconds{400}, std::chrono::milliseconds{600}}, // Min ratio: 4.0
            {std::chrono::milliseconds{500}, std::chrono::milliseconds{800}}, // Min ratio: 5.0
            {std::chrono::milliseconds{1000}, std::chrono::milliseconds{1500}}, // Min ratio: 10.0
            {std::chrono::milliseconds{200}, std::chrono::milliseconds{300}}, // Min ratio: 2.0 (should fail)
            {std::chrono::milliseconds{250}, std::chrono::milliseconds{350}}  // Min ratio: 2.5 (should fail)
        };
        
        for (const auto& [election_min, election_max] : election_ranges) {
            raft_configuration config;
            config._heartbeat_interval = heartbeat;
            config._election_timeout_min = election_min;
            config._election_timeout_max = election_max;
            
            auto validation_errors = config.get_validation_errors();
            bool has_compatibility_error = std::any_of(validation_errors.begin(), validation_errors.end(),
                [](const std::string& error) {
                    return error.find("heartbeat_interval") != std::string::npos &&
                           error.find("election_timeout") != std::string::npos;
                });
            
            double ratio = static_cast<double>(election_min.count()) / heartbeat.count();
            
            if (ratio >= recommended_ratio) {
                // Property: Adequate ratios should pass validation
                BOOST_CHECK(!has_compatibility_error);
                BOOST_TEST_MESSAGE("✓ Adequate ratio accepted: " << ratio);
            } else {
                // Property: Inadequate ratios should fail validation
                BOOST_CHECK(has_compatibility_error);
                BOOST_TEST_MESSAGE("✓ Inadequate ratio rejected: " << ratio);
            }
        }
    }
    
    // Test 8: Random compatibility testing
    {
        BOOST_TEST_MESSAGE("Test 8: Random compatibility testing");
        
        std::uniform_int_distribution<int> heartbeat_dist(10, 1000);
        std::uniform_real_distribution<double> ratio_dist(1.0, 10.0);
        
        for (int i = 0; i < 30; ++i) {
            auto heartbeat = std::chrono::milliseconds{heartbeat_dist(gen)};
            auto ratio = ratio_dist(gen);
            auto election_min = std::chrono::milliseconds{
                static_cast<int>(heartbeat.count() * ratio)
            };
            auto election_max = election_min + std::chrono::milliseconds{100};
            
            raft_configuration config;
            config._heartbeat_interval = heartbeat;
            config._election_timeout_min = election_min;
            config._election_timeout_max = election_max;
            
            auto validation_errors = config.get_validation_errors();
            bool has_compatibility_error = std::any_of(validation_errors.begin(), validation_errors.end(),
                [](const std::string& error) {
                    return error.find("heartbeat_interval") != std::string::npos &&
                           error.find("election_timeout") != std::string::npos;
                });
            
            if (ratio >= recommended_ratio) {
                // Property: Random configurations with adequate ratios should pass
                BOOST_CHECK(!has_compatibility_error);
            } else {
                // Property: Random configurations with inadequate ratios should fail
                BOOST_CHECK(has_compatibility_error);
            }
        }
        
        BOOST_TEST_MESSAGE("✓ Random compatibility testing completed");
    }
    
    // Test 9: Compatibility error message validation
    {
        BOOST_TEST_MESSAGE("Test 9: Compatibility error message validation");
        
        raft_configuration incompatible_config;
        incompatible_config._heartbeat_interval = std::chrono::milliseconds{200};
        incompatible_config._election_timeout_min = std::chrono::milliseconds{400}; // Ratio: 2.0
        incompatible_config._election_timeout_max = std::chrono::milliseconds{500};
        
        auto validation_errors = incompatible_config.get_validation_errors();
        
        // Property: Compatibility error message should be informative
        bool found_informative_error = false;
        for (const auto& error : validation_errors) {
            if (error.find("heartbeat_interval") != std::string::npos &&
                error.find("election_timeout") != std::string::npos &&
                (error.find("less than") != std::string::npos || error.find("prevent") != std::string::npos)) {
                found_informative_error = true;
                BOOST_TEST_MESSAGE("✓ Informative error message: " << error);
                break;
            }
        }
        BOOST_CHECK(found_informative_error);
    }
    
    BOOST_TEST_MESSAGE("All heartbeat interval compatibility property tests passed!");
}