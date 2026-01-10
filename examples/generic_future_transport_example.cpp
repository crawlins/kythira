/**
 * @file generic_future_transport_example.cpp
 * @brief Example demonstrating generic future architecture concepts
 * 
 * This example shows how to use the new generic future architecture,
 * demonstrating:
 * 1. Generic future concept usage
 * 2. Template instantiation with kythira::Future
 * 3. Transport layer integration concepts (API structure)
 * 4. Error handling with generic futures
 * 5. Collective operations
 */

#include <raft/future.hpp>
#include <concepts/future.hpp>
#include <raft/types.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <unordered_map>

namespace {
    constexpr std::uint64_t node_1_id = 1;
    constexpr std::uint64_t node_2_id = 2;
    constexpr std::uint64_t node_3_id = 3;
    constexpr std::chrono::milliseconds rpc_timeout{5000};
    constexpr std::uint64_t example_term = 5;
    constexpr std::uint64_t example_candidate_id = 1;
    constexpr std::uint64_t example_last_log_index = 10;
    constexpr std::uint64_t example_last_log_term = 4;
}

// Define our future types for different RPC responses
using RequestVoteFuture = kythira::Future<kythira::request_vote_response<>>;
using AppendEntriesFuture = kythira::Future<kythira::append_entries_response<>>;
using InstallSnapshotFuture = kythira::Future<kythira::install_snapshot_response<>>;

auto demonstrate_generic_future_concepts() -> bool {
    std::cout << "=== Generic Future Concepts ===\n";
    
    try {
        // Verify that kythira::Future satisfies the future concept
        static_assert(kythira::future<RequestVoteFuture, kythira::request_vote_response<>>, 
                      "RequestVoteFuture must satisfy future concept");
        static_assert(kythira::future<AppendEntriesFuture, kythira::append_entries_response<>>, 
                      "AppendEntriesFuture must satisfy future concept");
        static_assert(kythira::future<InstallSnapshotFuture, kythira::install_snapshot_response<>>, 
                      "InstallSnapshotFuture must satisfy future concept");
        
        std::cout << "  ✓ All future types satisfy the generic future concept\n";
        
        // Demonstrate basic future operations
        kythira::request_vote_response<> response;
        response._term = example_term;
        response._vote_granted = true;
        
        auto future = RequestVoteFuture(response);
        
        // Test concept interface methods
        if (future.isReady()) {
            std::cout << "  ✓ Future isReady() works correctly\n";
        }
        
        if (future.wait(std::chrono::milliseconds{100})) {
            std::cout << "  ✓ Future wait() works correctly\n";
        }
        
        auto result = future.get();
        if (result.term() == example_term && result.vote_granted()) {
            std::cout << "  ✓ Future get() returns correct value\n";
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Generic future concepts failed: " << e.what() << "\n";
        return false;
    }
}

auto demonstrate_transport_api_structure() -> bool {
    std::cout << "\n=== Transport API Structure ===\n";
    
    try {
        std::cout << "  Generic transport client API structure:\n";
        std::cout << "  - Template parameters: <FutureType, Serializer, Metrics>\n";
        std::cout << "  - send_request_vote() -> FutureType\n";
        std::cout << "  - send_append_entries() -> FutureType\n";
        std::cout << "  - send_install_snapshot() -> FutureType\n";
        
        std::cout << "  ✓ Transport layer designed for generic future types\n";
        
        // Demonstrate the concept of generic transport usage
        std::cout << "  Example usage pattern:\n";
        std::cout << "    using MyFuture = kythira::Future<ResponseType>;\n";
        std::cout << "    using MyClient = kythira::transport_client<MyFuture, Serializer, Metrics>;\n";
        std::cout << "    auto future = client.send_rpc(target, request, timeout);\n";
        std::cout << "    auto response = future.get();\n";
        
        std::cout << "  ✓ Generic transport API structure demonstrated\n";
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Transport API structure failed: " << e.what() << "\n";
        return false;
    }
}

auto demonstrate_future_chaining() -> bool {
    std::cout << "\n=== Future Chaining ===\n";
    
    try {
        // Create a successful vote response
        kythira::request_vote_response<> vote_response;
        vote_response._term = example_term;
        vote_response._vote_granted = true;
        
        auto vote_future = RequestVoteFuture(vote_response);
        
        // Chain operations based on vote result
        auto chained_result = vote_future.then([](const kythira::request_vote_response<>& response) {
            std::cout << "  Processing vote response for term " << response.term() << "\n";
            if (response.vote_granted()) {
                std::cout << "  ✓ Vote was granted, proceeding with leadership\n";
                return std::string("leadership_established");
            } else {
                std::cout << "  ✗ Vote was denied, remaining follower\n";
                return std::string("remain_follower");
            }
        });
        
        auto final_result = chained_result.get();
        std::cout << "  Final result: " << final_result << "\n";
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Future chaining failed: " << e.what() << "\n";
        return false;
    }
}

auto demonstrate_error_handling() -> bool {
    std::cout << "\n=== Error Handling ===\n";
    
    try {
        // Create a future with an exception using folly::exception_wrapper
        auto error_future = RequestVoteFuture(
            folly::exception_wrapper(std::runtime_error("Network timeout"))
        );
        
        // Handle the error gracefully
        auto safe_future = error_future.onError([](folly::exception_wrapper ex) {
            std::cout << "  Caught exception: " << ex.what() << "\n";
            // Return a default response
            kythira::request_vote_response<> default_response;
            default_response._term = 0;
            default_response._vote_granted = false;
            return default_response;
        });
        
        auto result = safe_future.get();
        if (!result.vote_granted() && result.term() == 0) {
            std::cout << "  ✓ Error handled correctly with default response\n";
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Error handling failed: " << e.what() << "\n";
        return false;
    }
}

auto demonstrate_collective_operations() -> bool {
    std::cout << "\n=== Collective Operations ===\n";
    
    try {
        // Create multiple vote futures representing responses from different nodes
        std::vector<RequestVoteFuture> vote_futures;
        
        // Node 1 grants vote
        kythira::request_vote_response<> response1;
        response1._term = example_term;
        response1._vote_granted = true;
        vote_futures.emplace_back(RequestVoteFuture(response1));
        
        // Node 2 grants vote
        kythira::request_vote_response<> response2;
        response2._term = example_term;
        response2._vote_granted = true;
        vote_futures.emplace_back(RequestVoteFuture(response2));
        
        // Node 3 denies vote
        kythira::request_vote_response<> response3;
        response3._term = example_term + 1; // Higher term
        response3._vote_granted = false;
        vote_futures.emplace_back(RequestVoteFuture(response3));
        
        std::cout << "  Created " << vote_futures.size() << " vote futures\n";
        
        // Wait for all responses
        auto all_results = kythira::wait_for_all(std::move(vote_futures));
        auto results = all_results.get();
        
        // Count votes
        int votes_granted = 0;
        std::uint64_t highest_term = 0;
        
        for (const auto& result : results) {
            if (result.has_value()) {
                const auto& response = result.value();
                if (response.vote_granted()) {
                    votes_granted++;
                }
                highest_term = std::max(highest_term, response.term());
            }
        }
        
        std::cout << "  Votes granted: " << votes_granted << "/" << results.size() << "\n";
        std::cout << "  Highest term seen: " << highest_term << "\n";
        
        // Check if we have majority (2 out of 3)
        if (votes_granted >= 2) {
            std::cout << "  ✓ Majority achieved, can become leader\n";
        } else {
            std::cout << "  ✗ No majority, remain follower\n";
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Collective operations failed: " << e.what() << "\n";
        return false;
    }
}

auto demonstrate_wait_for_any() -> bool {
    std::cout << "\n=== Wait for Any Operation ===\n";
    
    try {
        // Create futures representing different types of responses
        std::vector<RequestVoteFuture> mixed_futures;
        
        // Fast response
        kythira::request_vote_response<> fast_response;
        fast_response._term = example_term;
        fast_response._vote_granted = true;
        mixed_futures.emplace_back(RequestVoteFuture(fast_response));
        
        // Another response (would be slower in real scenario)
        kythira::request_vote_response<> slow_response;
        slow_response._term = example_term;
        slow_response._vote_granted = false;
        mixed_futures.emplace_back(RequestVoteFuture(slow_response));
        
        // Wait for the first one to complete
        auto any_result = kythira::wait_for_any(std::move(mixed_futures));
        auto [index, try_result] = any_result.get();
        
        std::cout << "  First response came from future at index " << index << "\n";
        
        if (try_result.has_value()) {
            const auto& response = try_result.value();
            std::cout << "  Response: term=" << response.term() 
                      << ", vote_granted=" << (response.vote_granted() ? "true" : "false") << "\n";
            std::cout << "  ✓ Wait for any operation completed successfully\n";
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Wait for any operation failed: " << e.what() << "\n";
        return false;
    }
}

auto demonstrate_concept_benefits() -> bool {
    std::cout << "\n=== Concept-Based Benefits ===\n";
    
    try {
        std::cout << "  Benefits of the generic future architecture:\n";
        std::cout << "  1. ✓ Type safety through concept constraints\n";
        std::cout << "  2. ✓ Flexible template instantiation\n";
        std::cout << "  3. ✓ Consistent API across all transport layers\n";
        std::cout << "  4. ✓ Easy testing with mock future implementations\n";
        std::cout << "  5. ✓ Performance preservation with zero-cost abstractions\n";
        
        // Demonstrate concept checking at compile time
        static_assert(kythira::future<kythira::Future<int>, int>, 
                      "kythira::Future<int> must satisfy future concept");
        static_assert(kythira::future<kythira::Future<std::string>, std::string>, 
                      "kythira::Future<std::string> must satisfy future concept");
        
        std::cout << "  ✓ Compile-time concept validation ensures correctness\n";
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Concept benefits demonstration failed: " << e.what() << "\n";
        return false;
    }
}

int main() {
    std::cout << "Generic Future Architecture Concepts Example\n";
    std::cout << "===========================================\n";
    
    int failed_scenarios = 0;
    
    // Run all demonstration scenarios
    if (!demonstrate_generic_future_concepts()) failed_scenarios++;
    if (!demonstrate_transport_api_structure()) failed_scenarios++;
    if (!demonstrate_future_chaining()) failed_scenarios++;
    if (!demonstrate_error_handling()) failed_scenarios++;
    if (!demonstrate_collective_operations()) failed_scenarios++;
    if (!demonstrate_wait_for_any()) failed_scenarios++;
    if (!demonstrate_concept_benefits()) failed_scenarios++;
    
    // Report results
    std::cout << "\n=== Summary ===\n";
    if (failed_scenarios > 0) {
        std::cerr << failed_scenarios << " scenario(s) failed\n";
        std::cout << "Exit code: 1\n";
        return 1;
    }
    
    std::cout << "All scenarios passed!\n";
    std::cout << "This example demonstrates the generic future architecture concepts\n";
    std::cout << "and how they enable flexible, type-safe asynchronous programming.\n";
    std::cout << "Exit code: 0\n";
    return 0;
}