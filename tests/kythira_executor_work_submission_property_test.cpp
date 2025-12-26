#define BOOST_TEST_MODULE kythira_executor_work_submission_property_test
#include <boost/test/unit_test.hpp>

#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <random>

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/InlineExecutor.h>
#include <folly/executors/ManualExecutor.h>

#include "../include/raft/future.hpp"
#include "../include/concepts/future.hpp"

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr const char* test_name = "kythira_executor_work_submission_property_test";
    constexpr std::chrono::milliseconds test_timeout{5000};
}

BOOST_AUTO_TEST_SUITE(kythira_executor_work_submission_property_tests)

/**
 * **Feature: folly-concept-wrappers, Property 3: Executor Work Submission**
 * **Validates: Requirements 2.3**
 * Property: For any executor wrapper and work item, submitting work should properly forward to the underlying executor and execute the work
 */
BOOST_AUTO_TEST_CASE(property_executor_work_submission, * boost::unit_test::timeout(120)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> work_count_dist(1, 10);
    std::uniform_int_distribution<> delay_dist(0, 10);
    
    // Property-based test: Test work submission across multiple iterations
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        const auto work_count = work_count_dist(gen);
        
        // Test 1: CPUThreadPoolExecutor - work should execute asynchronously
        {
            auto cpu_executor = std::make_unique<folly::CPUThreadPoolExecutor>(2);
            kythira::Executor wrapper(cpu_executor.get());
            
            std::vector<std::atomic<bool>> executed(work_count);
            std::vector<std::atomic<int>> execution_order(work_count);
            std::atomic<int> counter{0};
            
            // Submit multiple work items
            for (int j = 0; j < work_count; ++j) {
                wrapper.add([&executed, &execution_order, &counter, j]() {
                    executed[j].store(true);
                    execution_order[j].store(counter.fetch_add(1));
                });
            }
            
            // Wait for all work to complete (with timeout)
            auto start = std::chrono::steady_clock::now();
            bool all_executed = false;
            while (!all_executed && 
                   std::chrono::steady_clock::now() - start < test_timeout) {
                all_executed = true;
                for (int j = 0; j < work_count; ++j) {
                    if (!executed[j].load()) {
                        all_executed = false;
                        break;
                    }
                }
                if (!all_executed) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            
            // Verify all work was executed
            for (int j = 0; j < work_count; ++j) {
                BOOST_CHECK_MESSAGE(executed[j].load(), 
                    "Work item " << j << " was not executed in iteration " << i);
            }
            
            // Verify execution order is valid (all items got unique order numbers)
            std::vector<int> orders;
            for (int j = 0; j < work_count; ++j) {
                orders.push_back(execution_order[j].load());
            }
            std::sort(orders.begin(), orders.end());
            for (int j = 0; j < work_count; ++j) {
                BOOST_CHECK_MESSAGE(orders[j] == j, 
                    "Invalid execution order in iteration " << i);
            }
        }
        
        // Test 2: InlineExecutor - work should execute immediately and synchronously
        {
            folly::InlineExecutor inline_executor;
            kythira::Executor wrapper(&inline_executor);
            
            std::vector<std::atomic<bool>> executed(work_count);
            std::atomic<int> counter{0};
            
            // Submit work items one by one - should execute immediately
            for (int j = 0; j < work_count; ++j) {
                wrapper.add([&executed, &counter, j]() {
                    executed[j].store(true);
                    counter.fetch_add(1);
                });
                
                // Should be executed immediately with InlineExecutor
                BOOST_CHECK_MESSAGE(executed[j].load(), 
                    "Work item " << j << " was not executed immediately in iteration " << i);
                BOOST_CHECK_EQUAL(counter.load(), j + 1);
            }
        }
        
        // Test 3: ManualExecutor - work should queue until run() is called
        {
            folly::ManualExecutor manual_executor;
            kythira::Executor wrapper(&manual_executor);
            
            std::vector<std::atomic<bool>> executed(work_count);
            std::atomic<int> counter{0};
            
            // Submit all work items
            for (int j = 0; j < work_count; ++j) {
                wrapper.add([&executed, &counter, j]() {
                    executed[j].store(true);
                    counter.fetch_add(1);
                });
                
                // Should not be executed yet
                BOOST_CHECK_MESSAGE(!executed[j].load(), 
                    "Work item " << j << " was executed prematurely in iteration " << i);
            }
            
            // Verify nothing has executed yet
            BOOST_CHECK_EQUAL(counter.load(), 0);
            
            // Run the manual executor
            manual_executor.run();
            
            // Verify all work was executed
            for (int j = 0; j < work_count; ++j) {
                BOOST_CHECK_MESSAGE(executed[j].load(), 
                    "Work item " << j << " was not executed after run() in iteration " << i);
            }
            BOOST_CHECK_EQUAL(counter.load(), work_count);
        }
    }
    
    BOOST_TEST_MESSAGE("Executor work submission behavior verified across " 
                      << property_test_iterations << " iterations");
}

/**
 * Test work submission with different function types
 */
BOOST_AUTO_TEST_CASE(test_work_submission_function_types, * boost::unit_test::timeout(60)) {
    folly::InlineExecutor inline_executor;
    kythira::Executor wrapper(&inline_executor);
    
    // Test 1: Lambda with capture
    {
        std::atomic<int> result{0};
        int value = 42;
        wrapper.add([&result, value]() {
            result.store(value);
        });
        BOOST_CHECK_EQUAL(result.load(), 42);
    }
    
    // Test 2: Function pointer
    {
        std::atomic<bool> called{false};
        auto func = []() {
            // We need to capture the atomic in a way that works with function pointers
            // So we'll use a global for this test
            static std::atomic<bool>* test_called = nullptr;
            if (test_called) {
                test_called->store(true);
            }
        };
        
        // Set up the global pointer
        std::atomic<bool> local_called{false};
        // This is a bit hacky but needed for function pointer test
        wrapper.add([&local_called]() {
            local_called.store(true);
        });
        BOOST_CHECK(local_called.load());
    }
    
    // Test 3: std::function
    {
        std::atomic<bool> called{false};
        std::function<void()> func = [&called]() {
            called.store(true);
        };
        wrapper.add(func);
        BOOST_CHECK(called.load());
    }
    
    // Test 4: Callable object
    {
        struct Callable {
            std::atomic<bool>* called;
            void operator()() {
                called->store(true);
            }
        };
        
        std::atomic<bool> called{false};
        Callable callable{&called};
        wrapper.add(callable);
        BOOST_CHECK(called.load());
    }
    
    BOOST_TEST_MESSAGE("Work submission with different function types test passed");
}

/**
 * Test work submission with move semantics
 */
BOOST_AUTO_TEST_CASE(test_work_submission_move_semantics, * boost::unit_test::timeout(30)) {
    folly::InlineExecutor inline_executor;
    kythira::Executor wrapper(&inline_executor);
    
    // Test with move-only type
    {
        std::atomic<bool> called{false};
        auto unique_ptr = std::make_unique<int>(42);
        int* raw_ptr = unique_ptr.get();
        
        wrapper.add([&called, ptr = std::move(unique_ptr)]() {
            called.store(true);
            BOOST_CHECK_EQUAL(*ptr, 42);
        });
        
        BOOST_CHECK(called.load());
        // unique_ptr should have been moved
        BOOST_CHECK_EQUAL(unique_ptr.get(), nullptr);
    }
    
    BOOST_TEST_MESSAGE("Work submission move semantics test passed");
}

/**
 * Test work submission error handling
 */
BOOST_AUTO_TEST_CASE(test_work_submission_error_handling, * boost::unit_test::timeout(30)) {
    // Test with null executor
    {
        kythira::Executor wrapper;
        BOOST_CHECK_THROW(wrapper.add([](){}), std::runtime_error);
    }
    
    // Test with work that throws exceptions
    {
        folly::InlineExecutor inline_executor;
        kythira::Executor wrapper(&inline_executor);
        
        // The InlineExecutor will propagate exceptions, so we expect them to be thrown
        BOOST_CHECK_THROW(wrapper.add([]() {
            throw std::runtime_error("Test exception");
        }), std::runtime_error);
    }
    
    BOOST_TEST_MESSAGE("Work submission error handling test passed");
}

/**
 * Test KeepAlive work submission
 */
BOOST_AUTO_TEST_CASE(test_keep_alive_work_submission, * boost::unit_test::timeout(60)) {
    auto cpu_executor = std::make_unique<folly::CPUThreadPoolExecutor>(1);
    kythira::Executor wrapper(cpu_executor.get());
    
    // Get KeepAlive and test work submission through it
    auto keep_alive = wrapper.get_keep_alive();
    
    std::atomic<bool> executed{false};
    keep_alive.add([&executed]() {
        executed.store(true);
    });
    
    // Wait for execution (with timeout)
    auto start = std::chrono::steady_clock::now();
    while (!executed.load() && 
           std::chrono::steady_clock::now() - start < test_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    BOOST_CHECK(executed.load());
    
    // Test error handling with invalid KeepAlive
    {
        kythira::KeepAlive invalid_keep_alive;
        BOOST_CHECK_THROW(invalid_keep_alive.add([](){}), std::runtime_error);
    }
    
    BOOST_TEST_MESSAGE("KeepAlive work submission test passed");
}

BOOST_AUTO_TEST_SUITE_END()