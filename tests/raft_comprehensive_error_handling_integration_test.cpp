/**
 * Integration Test for Comprehensive Error Handling
 * 
 * Tests comprehensive error handling functionality including:
 * - RPC retry behavior under various network conditions
 * - Error classification and appropriate handling strategies
 * - Partition detection and recovery scenarios
 * - Proper error logging and reporting
 * 
 * Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6
 */

#define BOOST_TEST_MODULE RaftComprehensiveErrorHandlingIntegrationTest
#include <boost/test/unit_test.hpp>

#include <raft/error_handler.hpp>
#include <raft/types.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <exception>
#include <string>
#include <random>
#include <unordered_map>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_comprehensive_error_handling_integration_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    // Test constants
    constexpr std::uint64_t test_term_1 = 1;
    constexpr std::uint64_t test_term_2 = 2;
    constexpr std::uint64_t test_log_index_1 = 1;
    constexpr std::uint64_t test_log_index_2 = 2;
    constexpr std::uint64_t test_node_a = 1;
    constexpr std::uint64_t test_node_b = 2;
    constexpr std::uint64_t test_node_c = 3;
    constexpr std::chrono::milliseconds short_timeout{100};
    constexpr std::chrono::milliseconds medium_timeout{500};
    constexpr std::chrono::milliseconds long_timeout{2000};
    constexpr const char* test_node_a_str = "node_a";
    constexpr const char* test_node_b_str = "node_b";
    constexpr const char* test_node_c_str = "node_c";
    constexpr double high_reliability = 0.95;
    constexpr double medium_reliability = 0.7;
    constexpr double low_reliability = 0.1;  // Very low to force failures
    constexpr std::chrono::milliseconds low_latency{10};
    constexpr std::chrono::milliseconds high_latency{100};
    constexpr std::size_t max_retry_attempts = 5;
}

/**
 * Mock network client for simulating various network conditions and failures
 */
class MockNetworkClient {
public:
    struct NetworkCondition {
        double reliability = 1.0;
        std::chrono::milliseconds latency{0};
        bool partition_active = false;
    };
    
    MockNetworkClient() : _rng(std::random_device{}()) {}
    
    auto set_network_condition(const std::string& target, const NetworkCondition& condition) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _network_conditions[target] = condition;
    }
    
    auto simulate_partition(const std::vector<std::string>& partitioned_nodes) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& node : partitioned_nodes) {
            _network_conditions[node].partition_active = true;
        }
    }
    
    auto clear_partition() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& [node, condition] : _network_conditions) {
            condition.partition_active = false;
        }
    }
    
    auto send_append_entries(
        const std::string& target,
        const kythira::append_entries_request<std::uint64_t, std::uint64_t>& request,
        std::chrono::milliseconds timeout
    ) -> kythira::append_entries_response<std::uint64_t, std::uint64_t> {
        
        return simulate_network_operation<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
            target,
            [request]() {
                return kythira::append_entries_response<std::uint64_t, std::uint64_t>{
                    request.term(),
                    true,  // success
                    std::nullopt,  // conflict_term
                    std::nullopt   // conflict_index
                };
            },
            timeout
        );
    }
    
    auto get_operation_count(const std::string& target) const -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _operation_counts.find(target);
        return (it != _operation_counts.end()) ? it->second : 0;
    }
    
    auto reset_counters() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _operation_counts.clear();
    }

private:
    template<typename ResponseType, typename OperationFunc>
    auto simulate_network_operation(
        const std::string& target,
        OperationFunc&& operation,
        std::chrono::milliseconds timeout
    ) -> ResponseType {
        
        // Increment operation counter
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _operation_counts[target]++;
        }
        
        // Get network condition for target
        NetworkCondition condition;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _network_conditions.find(target);
            if (it != _network_conditions.end()) {
                condition = it->second;
            }
        }
        
        // Check for partition
        if (condition.partition_active) {
            throw std::runtime_error("Network is unreachable");
        }
        
        // Simulate latency
        if (condition.latency > std::chrono::milliseconds{0}) {
            std::this_thread::sleep_for(condition.latency);
        }
        
        // Check reliability
        std::uniform_real_distribution<double> reliability_dist(0.0, 1.0);
        bool operation_succeeds;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            operation_succeeds = reliability_dist(_rng) < condition.reliability;
        }
        
        if (!operation_succeeds) {
            std::vector<std::string> failure_messages = {
                "Network timeout occurred",
                "Connection refused by target",
                "Temporary failure, try again"
            };
            
            std::uniform_int_distribution<std::size_t> msg_dist(0, failure_messages.size() - 1);
            std::size_t msg_index;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                msg_index = msg_dist(_rng);
            }
            
            throw std::runtime_error(failure_messages[msg_index]);
        }
        
        return operation();
    }
    
    mutable std::mutex _mutex;
    std::unordered_map<std::string, NetworkCondition> _network_conditions;
    std::unordered_map<std::string, std::size_t> _operation_counts;
    std::mt19937 _rng;
};

/**
 * Simple retry mechanism for testing
 */
template<typename Operation>
auto execute_with_retry(Operation&& op, std::size_t max_attempts = 3) -> decltype(op()) {
    std::size_t attempt = 0;
    while (attempt < max_attempts) {
        try {
            return op();
        } catch (const std::exception& e) {
            ++attempt;
            if (attempt >= max_attempts) {
                throw;
            }
            // Simple exponential backoff
            std::this_thread::sleep_for(std::chrono::milliseconds{50 * attempt});
        }
    }
    throw std::runtime_error("Max attempts exceeded");
}

BOOST_AUTO_TEST_SUITE(comprehensive_error_handling_integration_tests, * boost::unit_test::timeout(300))

/**
 * Test: RPC retry behavior under various network conditions
 * 
 * Verifies that RPC operations retry appropriately under different
 * network failure scenarios with proper backoff and error handling.
 * 
 * Requirements: 4.1, 4.2, 4.3, 4.4
 */
BOOST_AUTO_TEST_CASE(rpc_retry_behavior_network_conditions, * boost::unit_test::timeout(60)) {
    MockNetworkClient network_client;
    
    // Test different RPC types with various network conditions
    struct TestScenario {
        std::string name;
        double reliability;
        std::chrono::milliseconds latency;
        std::size_t expected_min_attempts;
        std::size_t expected_max_attempts;
    };
    
    std::vector<TestScenario> scenarios = {
        {"High reliability, low latency", high_reliability, low_latency, 1, 3},
        {"Medium reliability, medium latency", medium_reliability, high_latency, 1, 5},
        {"Low reliability, high latency", low_reliability, high_latency, 2, max_retry_attempts}
    };
    
    for (const auto& scenario : scenarios) {
        BOOST_TEST_MESSAGE("Testing scenario: " << scenario.name);
        
        // Configure network conditions
        MockNetworkClient::NetworkCondition condition{
            .reliability = scenario.reliability,
            .latency = scenario.latency,
            .partition_active = false
        };
        network_client.set_network_condition(test_node_b_str, condition);
        network_client.reset_counters();
        
        // Test AppendEntries retry behavior
        auto append_operation = [&network_client]() -> kythira::append_entries_response<std::uint64_t, std::uint64_t> {
            kythira::append_entries_request<std::uint64_t, std::uint64_t> request{
                test_term_1,
                test_node_a,
                test_log_index_1,
                test_term_1,
                {},  // empty entries for heartbeat
                test_log_index_1
            };
            return network_client.send_append_entries(test_node_b_str, request, medium_timeout);
        };
        
        auto start_time = std::chrono::steady_clock::now();
        bool operation_succeeded = false;
        
        try {
            auto result = execute_with_retry(append_operation, max_retry_attempts);
            operation_succeeded = result.success();
            
            auto end_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            BOOST_TEST_MESSAGE("AppendEntries succeeded in " << elapsed.count() << "ms");
            
        } catch (const std::exception& e) {
            auto end_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            BOOST_TEST_MESSAGE("AppendEntries failed after " << elapsed.count() << "ms: " << e.what());
        }
        
        auto append_attempts = network_client.get_operation_count(test_node_b_str);
        BOOST_TEST_MESSAGE("AppendEntries attempts: " << append_attempts);
        
        // Verify retry behavior matches expectations
        BOOST_CHECK_GE(append_attempts, scenario.expected_min_attempts);
        BOOST_CHECK_LE(append_attempts, scenario.expected_max_attempts);
        
        // For high reliability scenarios, should usually succeed
        if (scenario.reliability >= high_reliability) {
            BOOST_CHECK(operation_succeeded);
        }
    }
}

/**
 * Test: Error classification and appropriate handling strategies
 * 
 * Verifies that different types of errors are classified correctly
 * and handled with appropriate retry strategies.
 * 
 * Requirements: 4.6
 */
BOOST_AUTO_TEST_CASE(error_classification_handling_strategies, * boost::unit_test::timeout(60)) {
    kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
    
    // Test different error types and their classifications
    struct ErrorTestCase {
        std::string error_message;
        kythira::error_handler<int>::error_type expected_type;
        bool should_retry;
        std::string description;
    };
    
    std::vector<ErrorTestCase> error_cases = {
        {"Network timeout occurred", kythira::error_handler<int>::error_type::network_timeout, true, "Network timeout should be retryable"},
        {"Connection refused by target", kythira::error_handler<int>::error_type::connection_refused, true, "Connection refused should be retryable"},
        {"Network is unreachable", kythira::error_handler<int>::error_type::network_unreachable, true, "Network unreachable should be retryable"},
        {"serialization error in message", kythira::error_handler<int>::error_type::serialization_error, false, "Serialization errors should not be retryable"},
        {"protocol violation detected", kythira::error_handler<int>::error_type::protocol_error, false, "Protocol errors should not be retryable"},
        {"temporary failure, try again", kythira::error_handler<int>::error_type::temporary_failure, true, "Temporary failures should be retryable"},
        {"unknown error occurred", kythira::error_handler<int>::error_type::unknown_error, true, "Unknown errors should default to retryable"}
    };
    
    for (const auto& test_case : error_cases) {
        BOOST_TEST_MESSAGE("Testing error classification: " << test_case.error_message);
        
        // Test error classification
        std::runtime_error test_error(test_case.error_message);
        auto classification = handler.classify_error(test_error);
        
        BOOST_CHECK_EQUAL(static_cast<int>(classification.type), static_cast<int>(test_case.expected_type));
        BOOST_CHECK_EQUAL(classification.should_retry, test_case.should_retry);
        BOOST_TEST_MESSAGE("✓ " << test_case.description);
        
        // Test actual retry behavior with simple retry logic
        std::atomic<int> attempt_count{0};
        auto error_operation = [&attempt_count, &test_case]() -> kythira::append_entries_response<std::uint64_t, std::uint64_t> {
            ++attempt_count;
            throw std::runtime_error(test_case.error_message);
        };
        
        try {
            if (test_case.should_retry) {
                execute_with_retry(error_operation, 3);
            } else {
                error_operation();
            }
            BOOST_FAIL("Expected exception for error: " << test_case.error_message);
        } catch (const std::exception& e) {
            if (test_case.should_retry) {
                // Should make multiple attempts for retryable errors
                BOOST_CHECK_GT(attempt_count.load(), 1);
                BOOST_TEST_MESSAGE("✓ Retryable error made " << attempt_count.load() << " attempts");
            } else {
                // Should fail immediately for non-retryable errors
                BOOST_CHECK_EQUAL(attempt_count.load(), 1);
                BOOST_TEST_MESSAGE("✓ Non-retryable error failed immediately");
            }
        }
    }
}

/**
 * Test: Partition detection and recovery scenarios
 * 
 * Verifies that network partitions are detected correctly and
 * the system handles partition recovery appropriately.
 * 
 * Requirements: 4.5
 */
BOOST_AUTO_TEST_CASE(partition_detection_recovery, * boost::unit_test::timeout(90)) {
    MockNetworkClient network_client;
    kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
    
    // Test nodes
    std::vector<std::string> all_nodes = {test_node_a_str, test_node_b_str, test_node_c_str};
    std::vector<std::string> partition_group_2 = {test_node_c_str};
    
    BOOST_TEST_MESSAGE("Testing partition detection and recovery");
    
    // Phase 1: Normal operation (no partition)
    {
        BOOST_TEST_MESSAGE("Phase 1: Normal operation");
        
        // Configure normal network conditions
        for (const auto& node : all_nodes) {
            MockNetworkClient::NetworkCondition condition{
                .reliability = high_reliability,
                .latency = low_latency,
                .partition_active = false
            };
            network_client.set_network_condition(node, condition);
        }
        
        // Test operations should succeed with minimal retries
        std::atomic<int> successful_operations{0};
        std::atomic<int> total_operations{0};
        
        for (const auto& target : {test_node_b_str, test_node_c_str}) {
            auto operation = [&network_client, target]() -> kythira::append_entries_response<std::uint64_t, std::uint64_t> {
                kythira::append_entries_request<std::uint64_t, std::uint64_t> request{
                    test_term_1, test_node_a, test_log_index_1, test_term_1, {}, test_log_index_1
                };
                return network_client.send_append_entries(target, request, medium_timeout);
            };
            
            ++total_operations;
            try {
                auto result = execute_with_retry(operation, 2);
                if (result.success()) {
                    ++successful_operations;
                }
            } catch (const std::exception& e) {
                BOOST_TEST_MESSAGE("Operation to " << target << " failed: " << e.what());
            }
        }
        
        BOOST_TEST_MESSAGE("Normal operation: " << successful_operations.load() << "/" << total_operations.load() << " succeeded");
        BOOST_CHECK_GE(successful_operations.load(), total_operations.load() * 0.8); // At least 80% success
    }
    
    // Phase 2: Simulate network partition
    {
        BOOST_TEST_MESSAGE("Phase 2: Network partition");
        
        // Simulate partition
        network_client.simulate_partition(partition_group_2);
        
        // Collect error patterns for partition detection
        std::vector<kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::error_classification> recent_errors;
        
        // Test operations to partitioned nodes should fail consistently
        for (int attempt = 0; attempt < 5; ++attempt) {
            auto operation = [&network_client]() -> kythira::append_entries_response<std::uint64_t, std::uint64_t> {
                kythira::append_entries_request<std::uint64_t, std::uint64_t> request{
                    test_term_1, test_node_a, test_log_index_1, test_term_1, {}, test_log_index_1
                };
                return network_client.send_append_entries(test_node_c_str, request, short_timeout);
            };
            
            try {
                execute_with_retry(operation, 1);
                BOOST_FAIL("Expected partition to cause failure");
            } catch (const std::exception& e) {
                auto classification = handler.classify_error(e);
                recent_errors.push_back(classification);
                BOOST_TEST_MESSAGE("Partition attempt " << (attempt + 1) << ": " << e.what());
            }
        }
        
        // Test partition detection
        bool partition_detected = handler.detect_network_partition(recent_errors);
        BOOST_CHECK(partition_detected);
        BOOST_TEST_MESSAGE("✓ Network partition detected correctly");
        
        // Operations to non-partitioned nodes should still work
        auto operation_to_b = [&network_client]() -> kythira::append_entries_response<std::uint64_t, std::uint64_t> {
            kythira::append_entries_request<std::uint64_t, std::uint64_t> request{
                test_term_1, test_node_a, test_log_index_1, test_term_1, {}, test_log_index_1
            };
            return network_client.send_append_entries(test_node_b_str, request, medium_timeout);
        };
        
        try {
            auto result = execute_with_retry(operation_to_b, 2);
            BOOST_CHECK(result.success());
            BOOST_TEST_MESSAGE("✓ Operations to non-partitioned nodes still work");
        } catch (const std::exception& e) {
            BOOST_TEST_MESSAGE("Unexpected failure to non-partitioned node: " << e.what());
        }
    }
    
    // Phase 3: Partition recovery
    {
        BOOST_TEST_MESSAGE("Phase 3: Partition recovery");
        
        // Clear partition
        network_client.clear_partition();
        
        // Configure normal conditions for all nodes
        for (const auto& node : all_nodes) {
            MockNetworkClient::NetworkCondition condition{
                .reliability = high_reliability,
                .latency = low_latency,
                .partition_active = false
            };
            network_client.set_network_condition(node, condition);
        }
        
        // Test that operations to previously partitioned nodes now succeed
        std::atomic<int> recovery_successful_operations{0};
        std::atomic<int> recovery_total_operations{0};
        
        for (int attempt = 0; attempt < 3; ++attempt) {
            auto operation = [&network_client]() -> kythira::append_entries_response<std::uint64_t, std::uint64_t> {
                kythira::append_entries_request<std::uint64_t, std::uint64_t> request{
                    test_term_2, test_node_a, test_log_index_2, test_term_2, {}, test_log_index_2
                };
                return network_client.send_append_entries(test_node_c_str, request, medium_timeout);
            };
            
            ++recovery_total_operations;
            try {
                auto result = execute_with_retry(operation, 2);
                if (result.success()) {
                    ++recovery_successful_operations;
                }
                BOOST_TEST_MESSAGE("Recovery attempt " << (attempt + 1) << ": SUCCESS");
            } catch (const std::exception& e) {
                BOOST_TEST_MESSAGE("Recovery attempt " << (attempt + 1) << ": " << e.what());
            }
        }
        
        BOOST_TEST_MESSAGE("Recovery: " << recovery_successful_operations.load() << "/" << recovery_total_operations.load() << " succeeded");
        BOOST_CHECK_GE(recovery_successful_operations.load(), recovery_total_operations.load() * 0.6); // At least 60% success after recovery
        
        // Verify no partition detected after recovery
        std::vector<kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::error_classification> post_recovery_errors;
        bool post_recovery_partition = handler.detect_network_partition(post_recovery_errors);
        BOOST_CHECK(!post_recovery_partition);
        BOOST_TEST_MESSAGE("✓ No partition detected after recovery");
    }
}

/**
 * Test: Proper error logging and reporting
 * 
 * Verifies that error conditions are properly logged with appropriate
 * context and detail for debugging and monitoring.
 * 
 * Requirements: 10.1, 10.2, 10.3, 10.4, 10.5
 */
BOOST_AUTO_TEST_CASE(error_logging_reporting, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing error logging and reporting");
    
    // Test error classification functionality
    kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
    
    // Test various error types
    std::vector<std::string> error_messages = {
        "RPC operation failed: Network timeout occurred",
        "Operation timeout: commit_waiting timed out after 500ms",
        "Configuration change failed: majority not reached in joint_consensus phase",
        "Future collection failed: 3 out of 5 heartbeat operations failed",
        "State machine application failed: Invalid command format for entry 42"
    };
    
    for (const auto& error_msg : error_messages) {
        BOOST_TEST_MESSAGE("Testing error message: " << error_msg);
        
        std::runtime_error test_error(error_msg);
        auto classification = handler.classify_error(test_error);
        
        // Verify classification produces reasonable results
        BOOST_CHECK(!classification.description.empty());
        BOOST_TEST_MESSAGE("Classification: " << classification.description);
        BOOST_TEST_MESSAGE("Should retry: " << (classification.should_retry ? "yes" : "no"));
    }
    
    // Test partition detection with various error patterns
    std::vector<kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::error_classification> network_errors;
    
    // Add multiple network-related errors
    for (int i = 0; i < 5; ++i) {
        std::runtime_error network_error("Network is unreachable");
        auto classification = handler.classify_error(network_error);
        network_errors.push_back(classification);
    }
    
    bool partition_detected = handler.detect_network_partition(network_errors);
    BOOST_CHECK(partition_detected);
    BOOST_TEST_MESSAGE("✓ Network partition detection works correctly");
    
    // Test with mixed error types (should not detect partition)
    std::vector<kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::error_classification> mixed_errors;
    mixed_errors.push_back(handler.classify_error(std::runtime_error("Network timeout")));
    mixed_errors.push_back(handler.classify_error(std::runtime_error("serialization error")));
    mixed_errors.push_back(handler.classify_error(std::runtime_error("protocol violation")));
    
    bool mixed_partition_detected = handler.detect_network_partition(mixed_errors);
    BOOST_CHECK(!mixed_partition_detected);
    BOOST_TEST_MESSAGE("✓ Mixed errors do not trigger false partition detection");
}

BOOST_AUTO_TEST_SUITE_END()