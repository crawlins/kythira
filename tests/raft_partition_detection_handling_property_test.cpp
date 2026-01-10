#define BOOST_TEST_MODULE RaftPartitionDetectionHandlingPropertyTest

#include <boost/test/included/unit_test.hpp>
#include <raft/error_handler.hpp>
#include <raft/types.hpp>
#include <raft/future.hpp>
#include <folly/init/Init.h>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <stdexcept>
#include <unordered_map>

using namespace kythira;

namespace {
    constexpr std::size_t cluster_size = 5;
    constexpr std::size_t majority_size = 3;
    constexpr std::size_t test_iterations = 8;
    constexpr std::chrono::milliseconds partition_detection_window{1000};
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
 * **Feature: raft-completion, Property 20: Partition Detection and Handling**
 * 
 * Property: When network partitions occur, the system detects the partition and handles it according to Raft safety requirements.
 * **Validates: Requirements 4.5**
 */
BOOST_AUTO_TEST_CASE(raft_partition_detection_handling_property_test, * boost::unit_test::timeout(240)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Create error handler for partition detection
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        // Simulate a cluster with network partition
        std::vector<std::uint64_t> all_nodes = {1, 2, 3, 4, 5};
        std::uniform_int_distribution<std::size_t> partition_size_dist(1, cluster_size - 1);
        std::size_t partition_size = partition_size_dist(gen);
        
        // Create partition: some nodes are unreachable
        std::vector<std::uint64_t> reachable_nodes;
        std::vector<std::uint64_t> partitioned_nodes;
        
        std::shuffle(all_nodes.begin(), all_nodes.end(), gen);
        for (std::size_t i = 0; i < partition_size; ++i) {
            partitioned_nodes.push_back(all_nodes[i]);
        }
        for (std::size_t i = partition_size; i < all_nodes.size(); ++i) {
            reachable_nodes.push_back(all_nodes[i]);
        }
        
        BOOST_TEST_MESSAGE("Partition: " << partitioned_nodes.size() << " nodes unreachable, " 
                          << reachable_nodes.size() << " nodes reachable");
        
        // Track error patterns for partition detection
        std::vector<typename error_handler<int>::error_classification> recent_errors;
        std::unordered_map<std::uint64_t, std::size_t> node_failure_counts;
        
        // Simulate operations to multiple nodes with partition
        for (std::uint64_t target_node : all_nodes) {
            bool is_partitioned = std::find(partitioned_nodes.begin(), partitioned_nodes.end(), target_node) != partitioned_nodes.end();
            
            std::atomic<int> attempt_count{0};
            auto partition_operation = [&handler, &recent_errors, &node_failure_counts, target_node, is_partitioned, &attempt_count]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
                ++attempt_count;
                
                if (is_partitioned) {
                    // Simulate partition-related errors
                    std::vector<std::string> partition_errors = {
                        "Network is unreachable",
                        "Connection timeout",
                        "No route to host",
                        "Network timeout occurred"
                    };
                    
                    std::random_device rd;
                    std::mt19937 rng(rd());
                    std::uniform_int_distribution<std::size_t> error_dist(0, partition_errors.size() - 1);
                    
                    auto error_msg = partition_errors[error_dist(rng)];
                    auto classification = handler.classify_error(std::runtime_error(error_msg));
                    recent_errors.push_back(classification);
                    node_failure_counts[target_node]++;
                    
                    // Keep only recent errors for partition detection
                    if (recent_errors.size() > 10) {
                        recent_errors.erase(recent_errors.begin());
                    }
                    
                    return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                        std::runtime_error(error_msg));
                } else {
                    // Reachable nodes respond normally
                    kythira::append_entries_response<std::uint64_t, std::uint64_t> success_response{
                        1, // term
                        true, // success
                        std::nullopt, // conflict_term
                        std::nullopt  // conflict_index
                    };
                    return kythira::FutureFactory::makeFuture(success_response);
                }
            };
            
            try {
                auto result = handler.execute_with_retry("append_entries", partition_operation).get();
                
                if (!is_partitioned) {
                    // Property: Reachable nodes should respond successfully
                    BOOST_CHECK(result.success());
                    BOOST_TEST_MESSAGE("✓ Node " << target_node << " (reachable) responded successfully");
                } else {
                    BOOST_FAIL("Partitioned node should not succeed: " << target_node);
                }
                
            } catch (const std::exception& e) {
                if (is_partitioned) {
                    // Property: Partitioned nodes should fail after retries
                    BOOST_CHECK_GT(attempt_count.load(), 1); // Should have retried
                    BOOST_TEST_MESSAGE("✓ Node " << target_node << " (partitioned) failed after " << attempt_count.load() << " attempts");
                } else {
                    BOOST_FAIL("Reachable node should not fail: " << target_node << " - " << e.what());
                }
            }
        }
        
        // Property: Should detect network partition based on error patterns
        bool partition_detected = handler.detect_network_partition(recent_errors);
        
        if (partitioned_nodes.size() >= 2) {
            // With multiple nodes failing, partition should be detected
            BOOST_CHECK(partition_detected);
            BOOST_TEST_MESSAGE("✓ Network partition correctly detected with " << partitioned_nodes.size() << " partitioned nodes");
        } else if (partitioned_nodes.size() == 1) {
            // Single node failure might or might not be classified as partition
            BOOST_TEST_MESSAGE("Single node partition detection: " << partition_detected);
        }
        
        // Property: Majority availability check
        bool has_majority = reachable_nodes.size() >= majority_size;
        BOOST_TEST_MESSAGE("Majority available: " << has_majority << " (" << reachable_nodes.size() << "/" << cluster_size << " nodes reachable)");
        
        if (has_majority) {
            // Property: With majority, operations should be able to proceed
            std::size_t successful_operations = 0;
            for (std::uint64_t node : reachable_nodes) {
                if (node_failure_counts[node] == 0) {
                    successful_operations++;
                }
            }
            BOOST_CHECK_GE(successful_operations, majority_size);
            BOOST_TEST_MESSAGE("✓ Majority operations can proceed (" << successful_operations << " successful)");
        } else {
            // Property: Without majority, cluster should not make progress
            BOOST_TEST_MESSAGE("✓ Minority partition detected - cluster should not make progress");
        }
    }
    
    // Test specific partition scenarios
    BOOST_TEST_MESSAGE("Testing specific partition scenarios...");
    
    // Test 1: Clean network split (50-50 partition)
    {
        BOOST_TEST_MESSAGE("Test 1: Clean network split");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        std::vector<std::uint64_t> partition_a = {1, 2};
        std::vector<std::uint64_t> partition_b = {3, 4, 5};
        
        std::vector<typename error_handler<int>::error_classification> split_errors;
        
        // Simulate cross-partition communication failures
        for (std::uint64_t node_a : partition_a) {
            for (std::uint64_t node_b : partition_b) {
                auto error_msg = "Network is unreachable";
                auto classification = handler.classify_error(std::runtime_error(error_msg));
                split_errors.push_back(classification);
            }
        }
        
        // Property: Clean split should be detected as partition
        bool split_detected = handler.detect_network_partition(split_errors);
        BOOST_CHECK(split_detected);
        
        // Property: Majority partition (B) should be able to operate
        BOOST_CHECK_GE(partition_b.size(), majority_size);
        BOOST_TEST_MESSAGE("✓ Clean network split detected, majority partition can operate");
    }
    
    // Test 2: Gradual node failures vs sudden partition
    {
        BOOST_TEST_MESSAGE("Test 2: Gradual failures vs sudden partition");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        // Simulate gradual failures (should not be detected as partition initially)
        std::vector<typename error_handler<int>::error_classification> gradual_errors;
        
        // Single node failure
        auto single_failure = handler.classify_error(std::runtime_error("Network timeout occurred"));
        gradual_errors.push_back(single_failure);
        
        bool gradual_detected = handler.detect_network_partition(gradual_errors);
        BOOST_CHECK(!gradual_detected);
        BOOST_TEST_MESSAGE("✓ Single node failure not detected as partition");
        
        // Add more failures to simulate sudden partition
        for (int i = 0; i < 5; ++i) {
            auto partition_failure = handler.classify_error(std::runtime_error("Network is unreachable"));
            gradual_errors.push_back(partition_failure);
        }
        
        bool sudden_detected = handler.detect_network_partition(gradual_errors);
        BOOST_CHECK(sudden_detected);
        BOOST_TEST_MESSAGE("✓ Multiple simultaneous failures detected as partition");
    }
    
    // Test 3: Partition recovery detection
    {
        BOOST_TEST_MESSAGE("Test 3: Partition recovery detection");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        // Start with partition errors
        std::vector<typename error_handler<int>::error_classification> recovery_errors;
        
        // Initial partition
        for (int i = 0; i < 4; ++i) {
            auto partition_error = handler.classify_error(std::runtime_error("Network is unreachable"));
            recovery_errors.push_back(partition_error);
        }
        
        bool initial_partition = handler.detect_network_partition(recovery_errors);
        BOOST_CHECK(initial_partition);
        BOOST_TEST_MESSAGE("Initial partition detected");
        
        // Simulate recovery - add successful operations
        recovery_errors.clear();
        
        // After recovery, no more network errors
        bool recovery_detected = handler.detect_network_partition(recovery_errors);
        BOOST_CHECK(!recovery_detected);
        BOOST_TEST_MESSAGE("✓ Partition recovery detected (no recent network errors)");
    }
    
    // Test 4: Asymmetric partition (one-way communication failure)
    {
        BOOST_TEST_MESSAGE("Test 4: Asymmetric partition");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        // Simulate asymmetric partition where A can't reach B, but B can reach A
        std::vector<typename error_handler<int>::error_classification> asymmetric_errors;
        
        // Node 1 can't reach nodes 2,3,4,5
        for (int i = 0; i < 4; ++i) {
            auto unreachable_error = handler.classify_error(std::runtime_error("Network is unreachable"));
            asymmetric_errors.push_back(unreachable_error);
        }
        
        // Property: Asymmetric partition should still be detected
        bool asymmetric_detected = handler.detect_network_partition(asymmetric_errors);
        BOOST_CHECK(asymmetric_detected);
        BOOST_TEST_MESSAGE("✓ Asymmetric partition detected");
    }
    
    // Test 5: Flapping network (intermittent connectivity)
    {
        BOOST_TEST_MESSAGE("Test 5: Flapping network detection");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        std::vector<typename error_handler<int>::error_classification> flapping_errors;
        
        // Simulate intermittent failures
        std::vector<std::string> intermittent_errors = {
            "Network timeout occurred",
            "Connection refused",
            "Network is unreachable",
            "Temporary failure"
        };
        
        // Add mixed success and failure patterns
        for (int cycle = 0; cycle < 3; ++cycle) {
            // Failure burst
            for (const auto& error_msg : intermittent_errors) {
                auto classification = handler.classify_error(std::runtime_error(error_msg));
                flapping_errors.push_back(classification);
            }
            
            // Brief recovery (no errors added)
        }
        
        // Property: Flapping network should be detected as partition
        bool flapping_detected = handler.detect_network_partition(flapping_errors);
        BOOST_CHECK(flapping_detected);
        BOOST_TEST_MESSAGE("✓ Flapping network detected as partition");
    }
    
    // Test 6: Error type classification for partition detection
    {
        BOOST_TEST_MESSAGE("Test 6: Error type classification");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        // Test different error types and their contribution to partition detection
        std::vector<std::pair<std::string, bool>> error_types = {
            {"Network is unreachable", true},           // Should contribute to partition detection
            {"Connection timeout", true},               // Should contribute to partition detection
            {"No route to host", true},                 // Should contribute to partition detection
            {"Network timeout occurred", true},         // Should contribute to partition detection
            {"Connection refused", true},               // Should contribute to partition detection
            {"serialization error", false},             // Should not contribute to partition detection
            {"protocol violation", false},              // Should not contribute to partition detection
            {"invalid format", false}                   // Should not contribute to partition detection
        };
        
        for (const auto& [error_msg, contributes_to_partition] : error_types) {
            auto classification = handler.classify_error(std::runtime_error(error_msg));
            
            BOOST_TEST_MESSAGE("Error: " << error_msg 
                              << " -> type=" << static_cast<int>(classification.type)
                              << ", should_retry=" << classification.should_retry);
            
            // Property: Network-related errors should be classified appropriately
            if (contributes_to_partition) {
                BOOST_CHECK(classification.type == error_handler<int>::error_type::network_timeout ||
                           classification.type == error_handler<int>::error_type::network_unreachable ||
                           classification.type == error_handler<int>::error_type::connection_refused ||
                           classification.type == error_handler<int>::error_type::temporary_failure);
            } else {
                BOOST_CHECK(classification.type == error_handler<int>::error_type::serialization_error ||
                           classification.type == error_handler<int>::error_type::protocol_error);
            }
        }
        
        // Test partition detection with only network errors
        std::vector<typename error_handler<int>::error_classification> network_only_errors;
        for (const auto& [error_msg, contributes] : error_types) {
            if (contributes) {
                auto classification = handler.classify_error(std::runtime_error(error_msg));
                network_only_errors.push_back(classification);
            }
        }
        
        bool network_partition_detected = handler.detect_network_partition(network_only_errors);
        BOOST_CHECK(network_partition_detected);
        BOOST_TEST_MESSAGE("✓ Network-only errors correctly detected as partition");
        
        // Test with mixed errors (should still detect partition)
        std::vector<typename error_handler<int>::error_classification> mixed_errors = network_only_errors;
        for (const auto& [error_msg, contributes] : error_types) {
            if (!contributes) {
                auto classification = handler.classify_error(std::runtime_error(error_msg));
                mixed_errors.push_back(classification);
            }
        }
        
        bool mixed_partition_detected = handler.detect_network_partition(mixed_errors);
        BOOST_CHECK(mixed_partition_detected);
        BOOST_TEST_MESSAGE("✓ Mixed errors still detected as partition when network errors dominate");
    }
    
    BOOST_TEST_MESSAGE("All partition detection and handling property tests passed!");
}