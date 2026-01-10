/**
 * Example: Error Handling in Raft
 * 
 * This example demonstrates:
 * 1. RPC retry behavior under network failures (Requirements 4.1, 4.2, 4.3, 4.4)
 * 2. Partition detection and recovery (Requirements 4.5)
 * 3. Timeout handling and classification (Requirements 4.6)
 * 
 * This example shows how the Raft implementation handles various error conditions
 * gracefully with appropriate retry mechanisms and recovery strategies.
 */

#include <raft/completion_exceptions.hpp>
#include <raft/types.hpp>

#include <folly/init/Init.h>

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <format>
#include <random>
#include <exception>
#include <unordered_map>

namespace {
    // Test configuration constants
    constexpr std::uint64_t leader_node_id = 1;
    constexpr std::uint64_t test_term = 5;
    constexpr std::uint64_t test_log_index = 10;
    constexpr std::chrono::milliseconds short_timeout{100};
    constexpr std::chrono::milliseconds medium_timeout{500};
    constexpr const char* test_node_a = "node_a";
    constexpr const char* test_node_b = "node_b";
    constexpr const char* test_node_c = "node_c";
    constexpr double high_failure_rate = 0.8;  // 80% failure rate
    constexpr double medium_failure_rate = 0.5;  // 50% failure rate
    constexpr std::size_t max_retry_attempts = 5;
}

// Simple error classification for demonstration
enum class error_type {
    network_timeout,
    network_unreachable,
    connection_refused,
    serialization_error,
    protocol_error,
    temporary_failure,
    unknown_error
};

struct error_classification {
    error_type type;
    bool should_retry;
    std::string description;
};

// Simple error classifier
auto classify_error(const std::exception& e) -> error_classification {
    const std::string error_msg = e.what();
    
    if (error_msg.find("timeout") != std::string::npos) {
        return {error_type::network_timeout, true, "Network timeout"};
    }   
 if (error_msg.find("unreachable") != std::string::npos) {
        return {error_type::network_unreachable, true, "Network unreachable"};
    }
    if (error_msg.find("refused") != std::string::npos) {
        return {error_type::connection_refused, true, "Connection refused"};
    }
    if (error_msg.find("serialization") != std::string::npos) {
        return {error_type::serialization_error, false, "Serialization error"};
    }
    if (error_msg.find("protocol") != std::string::npos) {
        return {error_type::protocol_error, false, "Protocol error"};
    }
    if (error_msg.find("temporary") != std::string::npos) {
        return {error_type::temporary_failure, true, "Temporary failure"};
    }
    
    return {error_type::unknown_error, true, "Unknown error"};
}

// Simple retry mechanism with exponential backoff
template<typename Operation>
auto execute_with_retry(Operation&& op, std::size_t max_attempts = 3) -> decltype(op()) {
    std::size_t attempt = 0;
    while (attempt < max_attempts) {
        try {
            return op();
        } catch (const std::exception& e) {
            ++attempt;
            auto classification = classify_error(e);
            
            if (!classification.should_retry || attempt >= max_attempts) {
                throw;
            }
            
            // Simple exponential backoff
            auto delay = std::chrono::milliseconds{50 * (1 << attempt)};
            std::this_thread::sleep_for(delay);
        }
    }
    throw std::runtime_error("Max attempts exceeded");
}

// Mock network client for simulating various error conditions
class mock_error_network_client {
public:
    enum class failure_mode {
        none,
        network_timeout,
        network_unreachable,
        connection_refused,
        temporary_failure,
        random_failures,
        deterministic_failures  // New mode for guaranteed failures
    };
    
    struct network_condition {
        failure_mode mode = failure_mode::none;
        double failure_rate = 0.0;
        std::chrono::milliseconds latency{10};
        bool partition_active = false;
        std::size_t guaranteed_failures = 0;  // Number of guaranteed failures before success
    };
    
    mock_error_network_client() : _rng(std::random_device{}()) {}
    
    auto set_network_condition(const std::string& target, const network_condition& condition) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _network_conditions[target] = condition;
    }    
 
   auto simulate_partition(const std::vector<std::string>& partitioned_nodes) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& node : partitioned_nodes) {
            _network_conditions[node] = network_condition{
                .mode = failure_mode::network_unreachable,
                .failure_rate = 1.0,
                .partition_active = true
            };
        }
    }
    
    auto clear_partition() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& [node, condition] : _network_conditions) {
            condition.partition_active = false;
            if (condition.mode == failure_mode::network_unreachable && condition.failure_rate == 1.0) {
                condition.mode = failure_mode::none;
                condition.failure_rate = 0.0;
            }
        }
    }
    
    auto send_append_entries(const std::string& target) -> kythira::append_entries_response<std::uint64_t, std::uint64_t> {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _operation_counts[target]++;
        }
        
        return simulate_network_operation<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
            target,
            []() {
                return kythira::append_entries_response<std::uint64_t, std::uint64_t>{
                    test_term, true, std::nullopt, std::nullopt
                };
            }
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
    auto simulate_network_operation(const std::string& target, OperationFunc&& operation) -> ResponseType {
        network_condition condition;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _network_conditions.find(target);
            if (it != _network_conditions.end()) {
                condition = it->second;
            }
        } 
       
        if (condition.partition_active) {
            throw std::runtime_error("Network is unreachable");
        }
        
        if (condition.latency > std::chrono::milliseconds{0}) {
            std::this_thread::sleep_for(condition.latency);
        }
        
        bool should_fail = false;
        
        // Handle deterministic failures first
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _network_conditions.find(target);
            if (it != _network_conditions.end() && 
                it->second.mode == failure_mode::deterministic_failures && 
                it->second.guaranteed_failures > 0) {
                it->second.guaranteed_failures--;
                should_fail = true;
            } else if (it != _network_conditions.end()) {
                // Handle probabilistic failures
                std::uniform_real_distribution<double> failure_dist(0.0, 1.0);
                should_fail = failure_dist(_rng) < it->second.failure_rate;
            }
        }
        
        if (should_fail) {
            std::string error_message;
            switch (condition.mode) {
                case failure_mode::network_timeout:
                case failure_mode::deterministic_failures:  // Use timeout message for deterministic failures
                    error_message = "Network timeout occurred";
                    break;
                case failure_mode::network_unreachable:
                    error_message = "Network is unreachable";
                    break;
                case failure_mode::connection_refused:
                    error_message = "Connection refused by target";
                    break;
                case failure_mode::temporary_failure:
                    error_message = "temporary failure, try again";
                    break;
                case failure_mode::random_failures:
                    {
                        std::vector<std::string> random_errors = {
                            "Network timeout occurred",
                            "Connection refused by target",
                            "temporary failure, try again"
                        };
                        std::uniform_int_distribution<std::size_t> error_dist(0, random_errors.size() - 1);
                        std::size_t error_index;
                        {
                            std::lock_guard<std::mutex> lock(_mutex);
                            error_index = error_dist(_rng);
                        }
                        error_message = random_errors[error_index];
                    }
                    break;
                default:
                    error_message = "Unknown network error";
                    break;
            }
            
            throw std::runtime_error(error_message);
        }
        
        return operation();
    }
    
    mutable std::mutex _mutex;
    std::unordered_map<std::string, network_condition> _network_conditions;
    std::unordered_map<std::string, std::size_t> _operation_counts;
    std::mt19937 _rng;
};

// Test scenario 1: RPC retry behavior under network failures
auto test_rpc_retry_behavior() -> bool {
    std::cout << "Test 1: RPC Retry Behavior Under Network Failures\n";
    
    try {
        mock_error_network_client network_client;
        
        std::cout << "  Testing AppendEntries retry with network timeouts...\n";
        
        network_client.set_network_condition(test_node_b, {
            .mode = mock_error_network_client::failure_mode::deterministic_failures,
            .failure_rate = 0.0,  // Not used for deterministic failures
            .latency = std::chrono::milliseconds{50},
            .partition_active = false,
            .guaranteed_failures = 2  // Fail first 2 attempts, succeed on 3rd
        });
        
        network_client.reset_counters();
        
        auto start_time = std::chrono::steady_clock::now();
        bool operation_succeeded = false;
        
        try {
            auto result = execute_with_retry([&network_client]() -> kythira::append_entries_response<std::uint64_t, std::uint64_t> {
                return network_client.send_append_entries(test_node_b);
            }, max_retry_attempts);
            operation_succeeded = result.success();
            
            auto end_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            std::cout << std::format("    AppendEntries completed in {}ms\n", elapsed.count());
            
        } catch (const std::exception& e) {
            auto end_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            std::cout << std::format("    AppendEntries failed after {}ms: {}\n", elapsed.count(), e.what());
        }
        
        auto final_attempt_count = network_client.get_operation_count(test_node_b);
        std::cout << std::format("    Total attempts made: {}\n", final_attempt_count);
        
        if (final_attempt_count > 1) {
            std::cout << "  ✓ RPC retry behavior working correctly\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Expected multiple retry attempts\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 2: Error classification and handling strategies
auto test_error_classification() -> bool {
    std::cout << "\nTest 2: Error Classification and Handling Strategies\n";
    
    try {
        std::cout << "  Testing different error types and their classifications...\n";
        
        struct error_test_case {
            std::string error_message;
            error_type expected_type;
            bool should_retry;
            std::string description;
        };
        
        std::vector<error_test_case> test_cases = {
            {"Network timeout occurred", error_type::network_timeout, true, "Network timeout"},
            {"Connection refused by target", error_type::connection_refused, true, "Connection refused"},
            {"Network is unreachable", error_type::network_unreachable, true, "Network unreachable"},
            {"serialization error in message", error_type::serialization_error, false, "Serialization error"},
            {"protocol violation detected", error_type::protocol_error, false, "Protocol error"},
            {"temporary failure, try again", error_type::temporary_failure, true, "Temporary failure"},
            {"unknown error occurred", error_type::unknown_error, true, "Unknown error"}
        };
        
        bool all_classifications_correct = true;
        
        for (const auto& test_case : test_cases) {
            std::runtime_error test_error(test_case.error_message);
            auto classification = classify_error(test_error);
            
            bool type_correct = (classification.type == test_case.expected_type);
            bool retry_correct = (classification.should_retry == test_case.should_retry);
            
            if (type_correct && retry_correct) {
                std::cout << std::format("    ✓ {}: Classified correctly\n", test_case.description);
            } else {
                std::cout << std::format("    ✗ {}: Classification incorrect\n", test_case.description);
                all_classifications_correct = false;
            }
        }
        
        if (all_classifications_correct) {
            std::cout << "  ✓ Error classification working correctly\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Some error classifications were incorrect\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 3: Partition detection and recovery
auto test_partition_detection_recovery() -> bool {
    std::cout << "\nTest 3: Partition Detection and Recovery\n";
    
    try {
        mock_error_network_client network_client;
        
        std::cout << "  Testing network partition detection...\n";
        
        // Phase 1: Normal operation
        std::cout << "    Phase 1: Normal operation\n";
        network_client.set_network_condition(test_node_b, {
            .mode = mock_error_network_client::failure_mode::none,
            .failure_rate = 0.0,
            .latency = std::chrono::milliseconds{10}
        });
        
        network_client.set_network_condition(test_node_c, {
            .mode = mock_error_network_client::failure_mode::none,
            .failure_rate = 0.0,
            .latency = std::chrono::milliseconds{10}
        });
        
        std::atomic<int> normal_successes{0};
        for (const auto& target : {test_node_b, test_node_c}) {
            try {
                auto result = network_client.send_append_entries(target);
                if (result.success()) {
                    normal_successes++;
                }
            } catch (const std::exception& e) {
                std::cout << std::format("      Unexpected failure to {}: {}\n", target, e.what());
            }
        }
        
        std::cout << std::format("    Normal operation: {}/2 operations succeeded\n", normal_successes.load());
        
        // Phase 2: Simulate network partition
        std::cout << "    Phase 2: Network partition\n";
        network_client.simulate_partition({test_node_c});
        
        std::vector<error_classification> recent_errors;
        
        for (int attempt = 0; attempt < 5; ++attempt) {
            try {
                auto result = network_client.send_append_entries(test_node_c);
                std::cout << "      Unexpected success during partition\n";
            } catch (const std::exception& e) {
                auto classification = classify_error(e);
                recent_errors.push_back(classification);
                std::cout << std::format("      Partition attempt {}: {}\n", attempt + 1, e.what());
            }
        }        

        // Test partition detection (simple heuristic)
        std::size_t network_errors = 0;
        for (const auto& error : recent_errors) {
            if (error.type == error_type::network_timeout ||
                error.type == error_type::network_unreachable ||
                error.type == error_type::connection_refused) {
                network_errors++;
            }
        }
        
        bool partition_detected = network_errors >= (recent_errors.size() * 2 / 3);
        std::cout << std::format("    Partition detected: {}\n", partition_detected ? "YES" : "NO");
        
        // Operations to non-partitioned nodes should still work
        try {
            auto result = network_client.send_append_entries(test_node_b);
            if (result.success()) {
                std::cout << "    ✓ Operations to non-partitioned nodes still work\n";
            }
        } catch (const std::exception& e) {
            std::cout << std::format("    Unexpected failure to non-partitioned node: {}\n", e.what());
        }
        
        // Phase 3: Partition recovery
        std::cout << "    Phase 3: Partition recovery\n";
        network_client.clear_partition();
        
        std::atomic<int> recovery_successes{0};
        for (int attempt = 0; attempt < 3; ++attempt) {
            try {
                auto result = network_client.send_append_entries(test_node_c);
                if (result.success()) {
                    recovery_successes++;
                }
                std::cout << std::format("      Recovery attempt {}: SUCCESS\n", attempt + 1);
            } catch (const std::exception& e) {
                std::cout << std::format("      Recovery attempt {}: {}\n", attempt + 1, e.what());
            }
        }
        
        std::cout << std::format("    Recovery: {}/3 operations succeeded\n", recovery_successes.load());
        
        if (partition_detected && recovery_successes > 0) {
            std::cout << "  ✓ Partition detection and recovery working correctly\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Partition detection or recovery not working\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto main(int argc, char* argv[]) -> int {
    // Initialize Folly
    folly::Init init(&argc, &argv);
    
    std::cout << "========================================\n";
    std::cout << "  Error Handling Example\n";
    std::cout << "========================================\n\n";
    
    std::cout << "This example demonstrates error handling in Raft:\n";
    std::cout << "- RPC retry behavior under network failures\n";
    std::cout << "- Error classification and handling strategies\n";
    std::cout << "- Partition detection and recovery\n\n";
    
    int failed_scenarios = 0;
    
    // Run all test scenarios
    if (!test_rpc_retry_behavior()) failed_scenarios++;
    if (!test_error_classification()) failed_scenarios++;
    if (!test_partition_detection_recovery()) failed_scenarios++;
    
    // Print summary
    std::cout << "\n========================================\n";
    if (failed_scenarios > 0) {
        std::cout << std::format("  {} scenario(s) failed\n", failed_scenarios);
        std::cout << "========================================\n";
        return 1;
    }
    
    std::cout << "  All scenarios passed!\n";
    std::cout << "  Error handling working correctly.\n";
    std::cout << "========================================\n";
    return 0;
}