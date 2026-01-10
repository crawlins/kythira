#define BOOST_TEST_MODULE RaftStepDownOperationCancellationPropertyTest

#include <boost/test/included/unit_test.hpp>
#include <raft/commit_waiter.hpp>
#include <raft/future_collector.hpp>
#include <raft/error_handler.hpp>
#include <raft/types.hpp>
#include <raft/future.hpp>
#include <folly/init/Init.h>
#include <random>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_step_down_operation_cancellation_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::size_t min_operations = 5;
    constexpr std::size_t max_operations = 50;
    constexpr std::size_t min_futures = 3;
    constexpr std::size_t max_futures = 30;
    constexpr std::chrono::milliseconds test_timeout{30000};
    constexpr std::chrono::milliseconds operation_timeout{5000};
    constexpr const char* step_down_reason = "Leadership lost";
    constexpr const char* term_change_reason = "Higher term detected";
}

/**
 * **Feature: raft-completion, Property 38: Step-down Operation Cancellation**
 * 
 * Property: For any leader step-down, all pending client operations are cancelled with appropriate errors.
 * **Validates: Requirements 8.2**
 */
BOOST_AUTO_TEST_CASE(raft_step_down_operation_cancellation_property_test, * boost::unit_test::timeout(120)) {
    BOOST_TEST_MESSAGE("Testing step-down operation cancellation property...");
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> operation_count_dist(min_operations, max_operations);
    std::uniform_int_distribution<std::size_t> future_count_dist(min_futures, max_futures);
    std::uniform_int_distribution<std::uint64_t> index_dist(1, 1000);
    std::uniform_int_distribution<std::uint64_t> term_dist(1, 100);
    
    // Test multiple scenarios with different step-down triggers
    for (int test_iteration = 0; test_iteration < 10; ++test_iteration) {
        BOOST_TEST_MESSAGE("Test iteration " << (test_iteration + 1) << "/10");
        
        const std::size_t operation_count = operation_count_dist(gen);
        const std::size_t future_count = future_count_dist(gen);
        const std::uint64_t current_term = term_dist(gen);
        const std::uint64_t higher_term = current_term + 1 + (gen() % 5);
        
        BOOST_TEST_MESSAGE("Testing step-down cancellation with " << operation_count 
                          << " pending operations, " << future_count << " futures, "
                          << "current term: " << current_term << ", higher term: " << higher_term);
        
        // Test 1: Step-down due to higher term discovery
        {
            BOOST_TEST_MESSAGE("Test 1: Step-down due to higher term discovery");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::atomic<std::size_t> fulfilled_count{0};
            std::atomic<std::size_t> rejected_count{0};
            std::atomic<std::size_t> leadership_lost_count{0};
            
            // Register pending client operations (simulating leader state)
            std::vector<std::uint64_t> operation_indices;
            for (std::size_t i = 0; i < operation_count; ++i) {
                const std::uint64_t index = index_dist(gen);
                operation_indices.push_back(index);
                
                auto fulfill_callback = [&fulfilled_count](std::vector<std::byte> result) {
                    fulfilled_count.fetch_add(1);
                };
                
                auto reject_callback = [&rejected_count, &leadership_lost_count](std::exception_ptr ex) {
                    rejected_count.fetch_add(1);
                    try {
                        std::rethrow_exception(ex);
                    } catch (const std::exception& e) {
                        std::string error_msg = e.what();
                        if (error_msg.find("Leadership lost") != std::string::npos ||
                            error_msg.find("Higher term") != std::string::npos ||
                            error_msg.find("Not the leader") != std::string::npos) {
                            leadership_lost_count.fetch_add(1);
                        }
                        BOOST_TEST_MESSAGE("Operation cancelled due to step-down: " << e.what());
                    }
                };
                
                commit_waiter.register_operation(
                    index,
                    std::move(fulfill_callback),
                    std::move(reject_callback),
                    operation_timeout
                );
            }
            
            // Verify operations are pending (leader state)
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), operation_count);
            BOOST_CHECK(commit_waiter.has_pending_operations());
            
            // Simulate step-down due to higher term discovery
            commit_waiter.cancel_all_operations(term_change_reason);
            
            // Give callbacks time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            
            // Property: All operations should be cancelled after step-down
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK(!commit_waiter.has_pending_operations());
            
            // Property: All operations should be rejected with leadership-related errors
            BOOST_CHECK_EQUAL(fulfilled_count.load(), 0);
            BOOST_CHECK_EQUAL(rejected_count.load(), operation_count);
            BOOST_CHECK_EQUAL(leadership_lost_count.load(), operation_count);
            
            BOOST_TEST_MESSAGE("✓ Step-down due to higher term: " << operation_count 
                              << " operations cancelled with leadership errors");
        }
        
        // Test 2: Step-down due to network partition detection
        {
            BOOST_TEST_MESSAGE("Test 2: Step-down due to network partition detection");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> heartbeat_futures;
            
            std::atomic<std::size_t> operation_rejected_count{0};
            std::atomic<std::size_t> partition_error_count{0};
            
            // Register pending operations
            const std::size_t partition_operations = operation_count / 2;
            for (std::size_t i = 0; i < partition_operations; ++i) {
                const std::uint64_t index = index_dist(gen);
                
                auto reject_callback = [&operation_rejected_count, &partition_error_count](std::exception_ptr ex) {
                    operation_rejected_count.fetch_add(1);
                    try {
                        std::rethrow_exception(ex);
                    } catch (const std::exception& e) {
                        std::string error_msg = e.what();
                        if (error_msg.find("partition") != std::string::npos ||
                            error_msg.find("majority") != std::string::npos ||
                            error_msg.find("unreachable") != std::string::npos) {
                            partition_error_count.fetch_add(1);
                        }
                    }
                };
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    operation_timeout
                );
            }
            
            // Create heartbeat futures that would fail (simulating partition)
            for (std::size_t i = 0; i < future_count; ++i) {
                auto promise = std::make_shared<kythira::Promise<kythira::append_entries_response<std::uint64_t, std::uint64_t>>>();
                heartbeat_futures.push_back(promise->getFuture().within(std::chrono::milliseconds{100})); // Short timeout
            }
            
            // Verify initial state
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), partition_operations);
            BOOST_CHECK_EQUAL(heartbeat_futures.size(), future_count);
            
            // Simulate step-down due to partition detection
            commit_waiter.cancel_all_operations("Network partition detected - stepping down");
            kythira::raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::cancel_collection(heartbeat_futures);
            
            // Give callbacks time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds{150});
            
            // Property: All resources should be cleaned up after partition-induced step-down
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK(heartbeat_futures.empty());
            BOOST_CHECK_EQUAL(operation_rejected_count.load(), partition_operations);
            
            BOOST_TEST_MESSAGE("✓ Step-down due to partition: " << partition_operations 
                              << " operations + " << future_count << " futures cleaned up");
        }
        
        // Test 3: Step-down during active replication
        {
            BOOST_TEST_MESSAGE("Test 3: Step-down during active replication");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> replication_futures;
            
            std::atomic<std::size_t> replication_cancelled_count{0};
            std::atomic<std::size_t> client_cancelled_count{0};
            
            // Register operations that are being replicated
            const std::size_t replication_operations = operation_count / 3;
            for (std::size_t i = 0; i < replication_operations; ++i) {
                const std::uint64_t index = index_dist(gen);
                
                auto reject_callback = [&client_cancelled_count](std::exception_ptr ex) {
                    client_cancelled_count.fetch_add(1);
                };
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    operation_timeout
                );
            }
            
            // Create replication futures (simulating ongoing AppendEntries RPCs)
            for (std::size_t i = 0; i < future_count; ++i) {
                auto promise = std::make_shared<kythira::Promise<kythira::append_entries_response<std::uint64_t, std::uint64_t>>>();
                replication_futures.push_back(promise->getFuture().within(operation_timeout));
            }
            
            // Verify replication is in progress
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), replication_operations);
            BOOST_CHECK_EQUAL(replication_futures.size(), future_count);
            
            // Simulate step-down during active replication (e.g., due to election timeout)
            commit_waiter.cancel_all_operations("Election timeout - stepping down");
            kythira::raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::cancel_collection(replication_futures);
            
            // Give callbacks time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            
            // Property: Step-down should cancel both client operations and ongoing replication
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK(replication_futures.empty());
            BOOST_CHECK_EQUAL(client_cancelled_count.load(), replication_operations);
            
            BOOST_TEST_MESSAGE("✓ Step-down during replication: " << replication_operations 
                              << " client ops + " << future_count << " replication futures cancelled");
        }
        
        // Test 4: Step-down with mixed operation states
        {
            BOOST_TEST_MESSAGE("Test 4: Step-down with mixed operation states");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            
            std::atomic<std::size_t> pending_cancelled{0};
            std::atomic<std::size_t> timeout_cancelled{0};
            
            // Add operations with different timeouts to simulate mixed states
            const std::size_t mixed_operations = operation_count / 4;
            
            // Some operations with short timeouts (would timeout soon)
            for (std::size_t i = 0; i < mixed_operations; ++i) {
                const std::uint64_t index = index_dist(gen);
                
                auto reject_callback = [&timeout_cancelled](std::exception_ptr ex) {
                    timeout_cancelled.fetch_add(1);
                };
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    std::chrono::milliseconds{50} // Short timeout
                );
            }
            
            // Some operations with long timeouts (would be pending)
            for (std::size_t i = 0; i < mixed_operations; ++i) {
                const std::uint64_t index = index_dist(gen) + 1000; // Different index range
                
                auto reject_callback = [&pending_cancelled](std::exception_ptr ex) {
                    pending_cancelled.fetch_add(1);
                };
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    std::chrono::milliseconds{10000} // Long timeout
                );
            }
            
            // Let some operations timeout naturally
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            auto timed_out_count = commit_waiter.cancel_timed_out_operations();
            
            // Give timeout callbacks time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            
            // Now step down (should cancel remaining operations)
            auto remaining_before_step_down = commit_waiter.get_pending_count();
            commit_waiter.cancel_all_operations(step_down_reason);
            
            // Give step-down callbacks time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            
            // Property: Step-down should handle mixed operation states correctly
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK_GT(timed_out_count, 0); // Some operations should have timed out
            BOOST_CHECK_GT(timeout_cancelled.load(), 0);
            BOOST_CHECK_GT(pending_cancelled.load(), 0);
            
            // Total cancelled should equal total operations
            auto total_cancelled = timeout_cancelled.load() + pending_cancelled.load();
            BOOST_CHECK_EQUAL(total_cancelled, mixed_operations * 2);
            
            BOOST_TEST_MESSAGE("✓ Mixed state step-down: " << timed_out_count << " timed out, "
                              << remaining_before_step_down << " cancelled by step-down");
        }
    }
    
    // Test edge cases for step-down operation cancellation
    BOOST_TEST_MESSAGE("Testing step-down operation cancellation edge cases...");
    
    // Test 5: Rapid step-down/step-up cycles
    {
        BOOST_TEST_MESSAGE("Test 5: Rapid step-down/step-up cycles");
        
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        std::atomic<std::size_t> total_cancelled{0};
        
        // Simulate rapid leadership changes
        for (int cycle = 0; cycle < 5; ++cycle) {
            // Add operations (become leader)
            const std::size_t cycle_operations = 3;
            for (std::size_t i = 0; i < cycle_operations; ++i) {
                const std::uint64_t index = (cycle * 100) + i + 1;
                
                auto reject_callback = [&total_cancelled](std::exception_ptr ex) {
                    total_cancelled.fetch_add(1);
                };
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    operation_timeout
                );
            }
            
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), cycle_operations);
            
            // Step down immediately
            commit_waiter.cancel_all_operations("Rapid leadership change " + std::to_string(cycle));
            
            // Brief pause
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
        }
        
        // Give all callbacks time to execute
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Property: Rapid cycles should handle all operations correctly
        BOOST_CHECK_EQUAL(total_cancelled.load(), 5 * 3); // 5 cycles * 3 operations each
        
        BOOST_TEST_MESSAGE("✓ Rapid step-down cycles: " << total_cancelled.load() << " operations handled");
    }
    
    // Test 6: Step-down with concurrent operations
    {
        BOOST_TEST_MESSAGE("Test 6: Step-down with concurrent operations");
        
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        std::atomic<std::size_t> concurrent_cancelled{0};
        std::atomic<bool> step_down_triggered{false};
        
        // Start adding operations concurrently
        std::vector<std::thread> operation_threads;
        const std::size_t thread_count = 3;
        const std::size_t ops_per_thread = 5;
        
        for (std::size_t t = 0; t < thread_count; ++t) {
            operation_threads.emplace_back([&, t]() {
                for (std::size_t i = 0; i < ops_per_thread; ++i) {
                    if (step_down_triggered.load()) {
                        break; // Stop adding operations after step-down
                    }
                    
                    const std::uint64_t index = (t * 1000) + i + 1;
                    
                    auto reject_callback = [&concurrent_cancelled](std::exception_ptr ex) {
                        concurrent_cancelled.fetch_add(1);
                    };
                    
                    try {
                        commit_waiter.register_operation(
                            index,
                            [](std::vector<std::byte>) {},
                            std::move(reject_callback),
                            operation_timeout
                        );
                    } catch (...) {
                        // May fail if step-down happens during registration
                    }
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
            });
        }
        
        // Let some operations get registered
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        // Trigger step-down
        step_down_triggered.store(true);
        auto operations_before_step_down = commit_waiter.get_pending_count();
        commit_waiter.cancel_all_operations("Concurrent step-down");
        
        // Wait for threads to complete
        for (auto& thread : operation_threads) {
            thread.join();
        }
        
        // Give callbacks time to execute
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Property: Concurrent step-down should be safe and cancel all registered operations
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
        BOOST_CHECK_GT(operations_before_step_down, 0);
        BOOST_CHECK_EQUAL(concurrent_cancelled.load(), operations_before_step_down);
        
        BOOST_TEST_MESSAGE("✓ Concurrent step-down: " << operations_before_step_down 
                          << " operations cancelled safely");
    }
    
    // Test 7: Step-down error message validation
    {
        BOOST_TEST_MESSAGE("Test 7: Step-down error message validation");
        
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        std::vector<std::string> error_messages;
        std::mutex error_mutex;
        
        // Add operations with different step-down reasons
        const std::vector<std::string> step_down_reasons = {
            "Higher term detected: 42",
            "Network partition detected",
            "Election timeout exceeded",
            "Heartbeat majority lost",
            "Manual step-down requested"
        };
        
        for (std::size_t i = 0; i < step_down_reasons.size(); ++i) {
            const std::uint64_t index = i + 1;
            
            auto reject_callback = [&error_messages, &error_mutex](std::exception_ptr ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    error_messages.push_back(e.what());
                }
            };
            
            commit_waiter.register_operation(
                index,
                [](std::vector<std::byte>) {},
                std::move(reject_callback),
                operation_timeout
            );
        }
        
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), step_down_reasons.size());
        
        // Step down with specific reason
        const std::string test_reason = "Test step-down with detailed reason";
        commit_waiter.cancel_all_operations(test_reason);
        
        // Give callbacks time to execute
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Property: Step-down should provide appropriate error messages
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
        BOOST_CHECK_EQUAL(error_messages.size(), step_down_reasons.size());
        
        // All error messages should contain the step-down reason
        for (const auto& error_msg : error_messages) {
            BOOST_CHECK(error_msg.find(test_reason) != std::string::npos);
            BOOST_TEST_MESSAGE("Step-down error: " << error_msg);
        }
        
        BOOST_TEST_MESSAGE("✓ Step-down error messages validated");
    }
    
    BOOST_TEST_MESSAGE("All step-down operation cancellation property tests passed!");
}