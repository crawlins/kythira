/**
 * Example: Async Operations in Raft
 * 
 * This example demonstrates:
 * 1. Heartbeat collection for linearizable reads (Requirements 2.1, 7.1, 7.2)
 * 2. Election process with vote collection (Requirements 2.2)
 * 3. Replication with acknowledgment tracking (Requirements 2.3, 6.1, 6.2)
 * 4. Future collection timeout handling (Requirements 2.4)
 * 5. Future collection cancellation cleanup (Requirements 2.5)
 * 
 * This example shows how the Raft implementation uses async operations
 * to coordinate distributed consensus operations efficiently.
 */

#include <raft/future_collector.hpp>
#include <raft/commit_waiter.hpp>
#include <raft/types.hpp>
#include <raft/future.hpp>

#include <folly/init/Init.h>

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <memory>
#include <format>

namespace {
    // Test configuration constants
    constexpr std::uint64_t leader_node_id = 1;
    constexpr std::uint64_t follower_node_1_id = 2;
    constexpr std::uint64_t follower_node_2_id = 3;
    constexpr std::chrono::milliseconds rpc_timeout{1000};
    constexpr std::chrono::milliseconds collection_timeout{2000};
    constexpr const char* test_command_payload = "SET key=async_value";
    constexpr const char* test_read_operation = "GET key";
}

// Helper function to convert string to bytes
auto string_to_bytes(const std::string& str) -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    bytes.reserve(str.size());
    for (char c : str) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

// Helper function to convert bytes to string
auto bytes_to_string(const std::vector<std::byte>& bytes) -> std::string {
    std::string str;
    str.reserve(bytes.size());
    for (auto byte : bytes) {
        str += static_cast<char>(byte);
    }
    return str;
}

// Mock network client for demonstrating async operations
class mock_async_network_client {
private:
    std::uint64_t _node_id;
    bool _simulate_failures;
    std::chrono::milliseconds _simulated_latency;
    
public:
    explicit mock_async_network_client(
        std::uint64_t node_id, 
        bool simulate_failures = false,
        std::chrono::milliseconds latency = std::chrono::milliseconds{10}
    ) : _node_id(node_id)
      , _simulate_failures(simulate_failures)
      , _simulated_latency(latency) {}
    
    // Simulate sending heartbeat (AppendEntries with empty entries)
    auto send_heartbeat(
        std::uint64_t target_node,
        std::uint64_t term,
        std::chrono::milliseconds timeout
    ) -> kythira::Future<kythira::append_entries_response<>> {
        
        // Simulate network delay
        return kythira::FutureFactory::makeFuture()
            .delay(_simulated_latency)
            .thenValue([this, target_node, term]() {
                if (_simulate_failures && target_node == follower_node_2_id) {
                    // Simulate network failure for one follower
                    throw std::runtime_error(std::format("Network failure to node {}", target_node));
                }
                
                // Return successful heartbeat response
                return kythira::append_entries_response<>{
                    ._term = term,
                    ._success = true,
                    ._conflict_index = std::nullopt,
                    ._conflict_term = std::nullopt
                };
            });
    }
    
    // Simulate sending vote request
    auto send_vote_request(
        std::uint64_t target_node,
        std::uint64_t term,
        std::uint64_t candidate_id,
        std::chrono::milliseconds timeout
    ) -> kythira::Future<kythira::request_vote_response<>> {
        
        return kythira::FutureFactory::makeFuture()
            .delay(_simulated_latency)
            .thenValue([this, target_node, term, candidate_id]() {
                if (_simulate_failures && target_node == follower_node_2_id) {
                    // Simulate network failure
                    throw std::runtime_error(std::format("Vote request failed to node {}", target_node));
                }
                
                // Grant vote (simplified logic)
                return kythira::request_vote_response<>{
                    ._term = term,
                    ._vote_granted = true
                };
            });
    }
    
    // Simulate sending replication request
    auto send_replication(
        std::uint64_t target_node,
        std::uint64_t term,
        const std::vector<std::byte>& entry_data,
        std::chrono::milliseconds timeout
    ) -> kythira::Future<kythira::append_entries_response<>> {
        
        return kythira::FutureFactory::makeFuture()
            .delay(_simulated_latency)
            .thenValue([this, target_node, term, entry_data]() {
                if (_simulate_failures && target_node == follower_node_2_id) {
                    // Simulate replication failure
                    throw std::runtime_error(std::format("Replication failed to node {}", target_node));
                }
                
                // Return successful replication response
                return kythira::append_entries_response<>{
                    ._term = term,
                    ._success = true,
                    ._conflict_index = std::nullopt,
                    ._conflict_term = std::nullopt
                };
            });
    }
    
    auto set_failure_simulation(bool enable) -> void {
        _simulate_failures = enable;
    }
};

// Test scenario 1: Heartbeat collection for linearizable reads
auto test_heartbeat_collection() -> bool {
    std::cout << "Test 1: Heartbeat Collection for Linearizable Reads\n";
    
    try {
        // Create mock network client
        auto network_client = mock_async_network_client{leader_node_id};
        
        // Simulate sending heartbeats to followers
        std::vector<kythira::Future<kythira::append_entries_response<>>> heartbeat_futures;
        
        std::cout << "  Sending heartbeats to followers...\n";
        
        // Send heartbeat to follower 1
        auto heartbeat_1 = network_client.send_heartbeat(
            follower_node_1_id, 
            1, // term
            rpc_timeout
        );
        heartbeat_futures.push_back(std::move(heartbeat_1));
        
        // Send heartbeat to follower 2
        auto heartbeat_2 = network_client.send_heartbeat(
            follower_node_2_id, 
            1, // term
            rpc_timeout
        );
        heartbeat_futures.push_back(std::move(heartbeat_2));
        
        // Use future collector to wait for majority response
        auto majority_future = kythira::raft_future_collector<kythira::append_entries_response<>>::collect_majority(
            std::move(heartbeat_futures),
            collection_timeout
        );
        
        // Wait for majority heartbeat responses
        auto responses = std::move(majority_future).get();
        
        std::cout << std::format("  Received {} heartbeat responses\n", responses.size());
        
        // Verify we got majority (2 out of 2 followers + leader = majority of 3)
        std::size_t successful_responses = 0;
        for (const auto& response : responses) {
            if (response.success()) {
                successful_responses++;
            }
        }
        
        std::cout << std::format("  {} successful heartbeat responses\n", successful_responses);
        
        // With leader's own acknowledgment, we have majority
        std::size_t total_acknowledgments = successful_responses + 1; // +1 for leader
        std::size_t majority_needed = 2; // majority of 3 nodes
        
        if (total_acknowledgments >= majority_needed) {
            std::cout << "  ✓ Linearizable read can proceed (majority heartbeat success)\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Insufficient heartbeat responses for linearizable read\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 2: Election process with vote collection
auto test_election_vote_collection() -> bool {
    std::cout << "\nTest 2: Election Process with Vote Collection\n";
    
    try {
        // Create mock network client for candidate
        auto network_client = mock_async_network_client{follower_node_1_id}; // Node 2 becomes candidate
        
        std::cout << "  Starting election process...\n";
        
        // Simulate sending vote requests to other nodes
        std::vector<kythira::Future<kythira::request_vote_response<>>> vote_futures;
        
        // Send vote request to node 1 (leader)
        auto vote_request_1 = network_client.send_vote_request(
            leader_node_id,
            2, // new term
            follower_node_1_id, // candidate id
            rpc_timeout
        );
        vote_futures.push_back(std::move(vote_request_1));
        
        // Send vote request to node 3
        auto vote_request_3 = network_client.send_vote_request(
            follower_node_2_id,
            2, // new term
            follower_node_1_id, // candidate id
            rpc_timeout
        );
        vote_futures.push_back(std::move(vote_request_3));
        
        // Use future collector to wait for majority votes
        auto vote_collection_future = kythira::raft_future_collector<kythira::request_vote_response<>>::collect_majority(
            std::move(vote_futures),
            collection_timeout
        );
        
        // Wait for vote responses
        auto vote_responses = std::move(vote_collection_future).get();
        
        std::cout << std::format("  Received {} vote responses\n", vote_responses.size());
        
        // Count granted votes
        std::size_t votes_granted = 1; // candidate votes for itself
        for (const auto& response : vote_responses) {
            if (response.vote_granted()) {
                votes_granted++;
            }
        }
        
        std::cout << std::format("  {} votes granted (including self-vote)\n", votes_granted);
        
        // Check if we have majority
        std::size_t majority_needed = 2; // majority of 3 nodes
        if (votes_granted >= majority_needed) {
            std::cout << "  ✓ Election successful (majority votes received)\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Insufficient votes for election victory\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 3: Replication with acknowledgment tracking
auto test_replication_acknowledgment_tracking() -> bool {
    std::cout << "\nTest 3: Replication with Acknowledgment Tracking\n";
    
    try {
        // Create mock network client for leader
        auto network_client = mock_async_network_client{leader_node_id};
        
        std::cout << "  Replicating log entry to followers...\n";
        
        auto entry_data = string_to_bytes(test_command_payload);
        
        // Simulate sending replication requests to followers
        std::vector<kythira::Future<kythira::append_entries_response<>>> replication_futures;
        
        // Send replication to follower 1
        auto replication_1 = network_client.send_replication(
            follower_node_1_id,
            1, // term
            entry_data,
            rpc_timeout
        );
        replication_futures.push_back(std::move(replication_1));
        
        // Send replication to follower 2
        auto replication_2 = network_client.send_replication(
            follower_node_2_id,
            1, // term
            entry_data,
            rpc_timeout
        );
        replication_futures.push_back(std::move(replication_2));
        
        // Use future collector to wait for majority acknowledgment
        auto replication_future = kythira::raft_future_collector<kythira::append_entries_response<>>::collect_majority(
            std::move(replication_futures),
            collection_timeout
        );
        
        // Wait for replication acknowledgments
        auto replication_responses = std::move(replication_future).get();
        
        std::cout << std::format("  Received {} replication responses\n", replication_responses.size());
        
        // Count successful acknowledgments
        std::size_t successful_replications = 1; // leader's own entry
        for (const auto& response : replication_responses) {
            if (response.success()) {
                successful_replications++;
            }
        }
        
        std::cout << std::format("  {} successful replications (including leader)\n", successful_replications);
        
        // Check if we can commit (majority replication)
        std::size_t majority_needed = 2; // majority of 3 nodes
        if (successful_replications >= majority_needed) {
            std::cout << "  ✓ Entry can be committed (majority replication achieved)\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Insufficient replication for commit\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 4: Future collection timeout handling
auto test_future_collection_timeout_handling() -> bool {
    std::cout << "\nTest 4: Future Collection Timeout Handling\n";
    
    try {
        // Create mock network client with failure simulation
        auto network_client = mock_async_network_client{
            leader_node_id, 
            true, // enable failure simulation
            std::chrono::milliseconds{50} // latency
        };
        
        std::cout << "  Testing timeout handling with network failures...\n";
        
        // Simulate sending operations that will partially fail
        std::vector<kythira::Future<kythira::append_entries_response<>>> operation_futures;
        
        // This will succeed
        auto operation_1 = network_client.send_heartbeat(
            follower_node_1_id,
            1, // term
            rpc_timeout
        );
        operation_futures.push_back(std::move(operation_1));
        
        // This will fail (simulated network failure)
        auto operation_2 = network_client.send_heartbeat(
            follower_node_2_id,
            1, // term
            rpc_timeout
        );
        operation_futures.push_back(std::move(operation_2));
        
        // Use collect_all_with_timeout to handle partial failures
        auto timeout_future = kythira::raft_future_collector<kythira::append_entries_response<>>::collect_all_with_timeout(
            std::move(operation_futures),
            std::chrono::milliseconds{500} // short timeout
        );
        
        // Wait for all operations (including failures)
        auto results = std::move(timeout_future).get();
        
        std::cout << std::format("  Processed {} operations\n", results.size());
        
        // Count successful and failed operations
        std::size_t successful_operations = 0;
        std::size_t failed_operations = 0;
        
        for (const auto& result : results) {
            if (result.hasValue()) {
                successful_operations++;
                std::cout << "    Operation succeeded\n";
            } else {
                failed_operations++;
                std::cout << "    Operation failed (timeout or network error)\n";
            }
        }
        
        std::cout << std::format("  {} successful, {} failed operations\n", 
                                successful_operations, failed_operations);
        
        // Verify we handled both success and failure cases
        if (successful_operations > 0 && failed_operations > 0) {
            std::cout << "  ✓ Timeout handling working correctly (mixed results)\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Expected mixed success/failure results\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 5: Future collection cancellation cleanup
auto test_future_collection_cancellation() -> bool {
    std::cout << "\nTest 5: Future Collection Cancellation Cleanup\n";
    
    try {
        // Create mock network client
        auto network_client = mock_async_network_client{leader_node_id};
        
        std::cout << "  Testing future collection cancellation...\n";
        
        // Create a collection of futures
        std::vector<kythira::Future<kythira::append_entries_response<>>> futures_to_cancel;
        
        // Add some long-running operations
        for (std::uint64_t i = 0; i < 3; ++i) {
            auto future = network_client.send_heartbeat(
                follower_node_1_id + i,
                1, // term
                std::chrono::milliseconds{5000} // long timeout
            );
            futures_to_cancel.push_back(std::move(future));
        }
        
        std::cout << std::format("  Created {} futures for cancellation test\n", futures_to_cancel.size());
        
        // Simulate cancellation by clearing the futures
        std::size_t futures_count_before = futures_to_cancel.size();
        kythira::raft_future_collector<kythira::append_entries_response<>>::cancel_collection(futures_to_cancel);
        std::size_t futures_count_after = futures_to_cancel.size();
        
        std::cout << std::format("  Futures before cancellation: {}\n", futures_count_before);
        std::cout << std::format("  Futures after cancellation: {}\n", futures_count_after);
        
        // Verify cancellation cleaned up the futures
        if (futures_count_before > 0 && futures_count_after == 0) {
            std::cout << "  ✓ Future collection cancellation successful\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Future collection not properly cancelled\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 6: Commit waiting demonstration
auto test_commit_waiting_mechanism() -> bool {
    std::cout << "\nTest 6: Commit Waiting Mechanism\n";
    
    try {
        // Create commit waiter
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        
        std::cout << "  Testing commit waiting for client operations...\n";
        
        // Simulate registering a client operation
        std::uint64_t log_index = 5;
        bool operation_completed = false;
        std::vector<std::byte> operation_result;
        std::exception_ptr operation_error;
        
        // Register operation with commit waiter
        commit_waiter.register_operation(
            log_index,
            [&operation_completed, &operation_result](std::vector<std::byte> result) {
                operation_completed = true;
                operation_result = std::move(result);
                std::cout << "    Client operation fulfilled\n";
            },
            [&operation_completed, &operation_error](std::exception_ptr error) {
                operation_completed = true;
                operation_error = error;
                std::cout << "    Client operation rejected\n";
            },
            std::chrono::milliseconds{1000} // timeout
        );
        
        std::cout << std::format("  Registered operation for log index {}\n", log_index);
        std::cout << std::format("  Pending operations: {}\n", commit_waiter.get_pending_count());
        
        // Simulate commit and state machine application
        auto test_result = string_to_bytes("operation_result");
        commit_waiter.notify_committed_and_applied(log_index, [&test_result](std::uint64_t) {
            return test_result;
        });
        
        // Verify operation was completed
        if (operation_completed && !operation_error) {
            std::cout << std::format("  ✓ Commit waiting completed successfully\n");
            std::cout << std::format("  Operation result: {}\n", bytes_to_string(operation_result));
            return true;
        } else {
            std::cerr << "  ✗ Failed: Operation not completed or completed with error\n";
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
    std::cout << "  Async Operations Example\n";
    std::cout << "========================================\n\n";
    
    std::cout << "This example demonstrates async operations in Raft:\n";
    std::cout << "- Heartbeat collection for linearizable reads\n";
    std::cout << "- Election process with vote collection\n";
    std::cout << "- Replication with acknowledgment tracking\n";
    std::cout << "- Future collection timeout handling\n";
    std::cout << "- Future collection cancellation cleanup\n";
    std::cout << "- Commit waiting mechanism\n\n";
    
    int failed_scenarios = 0;
    
    // Run all test scenarios
    if (!test_heartbeat_collection()) failed_scenarios++;
    if (!test_election_vote_collection()) failed_scenarios++;
    if (!test_replication_acknowledgment_tracking()) failed_scenarios++;
    if (!test_future_collection_timeout_handling()) failed_scenarios++;
    if (!test_future_collection_cancellation()) failed_scenarios++;
    if (!test_commit_waiting_mechanism()) failed_scenarios++;
    
    // Print summary
    std::cout << "\n========================================\n";
    if (failed_scenarios > 0) {
        std::cout << std::format("  {} scenario(s) failed\n", failed_scenarios);
        std::cout << "========================================\n";
        return 1;
    }
    
    std::cout << "  All scenarios passed!\n";
    std::cout << "  Async operations working correctly.\n";
    std::cout << "========================================\n";
    return 0;
}