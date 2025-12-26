#define BOOST_TEST_MODULE kythira_executor_concept_compliance_property_test
#include <boost/test/unit_test.hpp>

#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/InlineExecutor.h>
#include <folly/executors/ManualExecutor.h>

#include "../include/raft/future.hpp"
#include "../include/concepts/future.hpp"

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr const char* test_name = "kythira_executor_concept_compliance_property_test";
}

BOOST_AUTO_TEST_SUITE(kythira_executor_concept_compliance_property_tests)

/**
 * **Feature: folly-concept-wrappers, Property 1: Concept Compliance**
 * **Validates: Requirements 2.1**
 * Property: For any kythira::Executor wrapper, it should satisfy the executor concept
 */
BOOST_AUTO_TEST_CASE(property_kythira_executor_concept_compliance, * boost::unit_test::timeout(60)) {
    // Test that kythira::Executor satisfies executor concept
    static_assert(kythira::executor<kythira::Executor>, 
                  "kythira::Executor must satisfy executor concept");
    
    BOOST_TEST_MESSAGE("kythira::Executor satisfies executor concept");
    
    // Property-based test: Test executor behavior across multiple iterations
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Test with different folly executor implementations
        
        // Test 1: CPUThreadPoolExecutor
        {
            auto cpu_executor = std::make_unique<folly::CPUThreadPoolExecutor>(2);
            kythira::Executor wrapper(cpu_executor.get());
            
            // Test that wrapper is valid
            BOOST_CHECK(wrapper.is_valid());
            
            // Test that get() returns the original executor
            BOOST_CHECK_EQUAL(wrapper.get(), cpu_executor.get());
            
            // Test add() method with simple function
            std::atomic<bool> executed{false};
            wrapper.add([&executed]() {
                executed.store(true);
            });
            
            // Wait for execution (with timeout)
            auto start = std::chrono::steady_clock::now();
            while (!executed.load() && 
                   std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            
            BOOST_CHECK(executed.load());
        }
        
        // Test 2: InlineExecutor
        {
            folly::InlineExecutor inline_executor;
            kythira::Executor wrapper(&inline_executor);
            
            // Test that wrapper is valid
            BOOST_CHECK(wrapper.is_valid());
            
            // Test that get() returns the original executor
            BOOST_CHECK_EQUAL(wrapper.get(), &inline_executor);
            
            // Test add() method - should execute immediately
            std::atomic<bool> executed{false};
            wrapper.add([&executed]() {
                executed.store(true);
            });
            
            // Should be executed immediately with InlineExecutor
            BOOST_CHECK(executed.load());
        }
        
        // Test 3: ManualExecutor
        {
            folly::ManualExecutor manual_executor;
            kythira::Executor wrapper(&manual_executor);
            
            // Test that wrapper is valid
            BOOST_CHECK(wrapper.is_valid());
            
            // Test that get() returns the original executor
            BOOST_CHECK_EQUAL(wrapper.get(), &manual_executor);
            
            // Test add() method - should not execute until run()
            std::atomic<bool> executed{false};
            wrapper.add([&executed]() {
                executed.store(true);
            });
            
            // Should not be executed yet
            BOOST_CHECK(!executed.load());
            
            // Run the manual executor
            manual_executor.run();
            
            // Should be executed now
            BOOST_CHECK(executed.load());
        }
    }
    
    BOOST_TEST_MESSAGE("kythira::Executor behavior matches executor concept requirements across " 
                      << property_test_iterations << " iterations");
}

/**
 * Test null executor handling and error conditions
 */
BOOST_AUTO_TEST_CASE(test_executor_error_conditions, * boost::unit_test::timeout(30)) {
    // Test 1: Default constructor creates invalid executor
    {
        kythira::Executor wrapper;
        BOOST_CHECK(!wrapper.is_valid());
        BOOST_CHECK_EQUAL(wrapper.get(), nullptr);
        
        // Test that add() throws with null executor
        BOOST_CHECK_THROW(wrapper.add([](){}), std::runtime_error);
        
        // Test that get_keep_alive() throws with null executor
        BOOST_CHECK_THROW(wrapper.get_keep_alive(), std::runtime_error);
    }
    
    // Test 2: Constructor with null pointer throws
    {
        BOOST_CHECK_THROW(kythira::Executor(nullptr), std::invalid_argument);
    }
    
    BOOST_TEST_MESSAGE("Executor error condition handling test passed");
}

/**
 * Test copy and move semantics
 */
BOOST_AUTO_TEST_CASE(test_executor_copy_move_semantics, * boost::unit_test::timeout(30)) {
    folly::InlineExecutor inline_executor;
    
    // Test copy constructor
    {
        kythira::Executor original(&inline_executor);
        kythira::Executor copied(original);
        
        BOOST_CHECK(copied.is_valid());
        BOOST_CHECK_EQUAL(copied.get(), &inline_executor);
        BOOST_CHECK_EQUAL(original.get(), copied.get());
    }
    
    // Test copy assignment
    {
        kythira::Executor original(&inline_executor);
        kythira::Executor assigned;
        assigned = original;
        
        BOOST_CHECK(assigned.is_valid());
        BOOST_CHECK_EQUAL(assigned.get(), &inline_executor);
        BOOST_CHECK_EQUAL(original.get(), assigned.get());
    }
    
    // Test move constructor
    {
        kythira::Executor original(&inline_executor);
        kythira::Executor moved(std::move(original));
        
        BOOST_CHECK(moved.is_valid());
        BOOST_CHECK_EQUAL(moved.get(), &inline_executor);
    }
    
    // Test move assignment
    {
        kythira::Executor original(&inline_executor);
        kythira::Executor assigned;
        assigned = std::move(original);
        
        BOOST_CHECK(assigned.is_valid());
        BOOST_CHECK_EQUAL(assigned.get(), &inline_executor);
    }
    
    BOOST_TEST_MESSAGE("Executor copy/move semantics test passed");
}

/**
 * Test KeepAlive functionality
 */
BOOST_AUTO_TEST_CASE(test_executor_keep_alive, * boost::unit_test::timeout(30)) {
    auto cpu_executor = std::make_unique<folly::CPUThreadPoolExecutor>(1);
    kythira::Executor wrapper(cpu_executor.get());
    
    // Test get_keep_alive() method
    auto keep_alive = wrapper.get_keep_alive();
    
    // Test that KeepAlive satisfies keep_alive concept
    static_assert(kythira::keep_alive<kythira::KeepAlive>, 
                  "kythira::KeepAlive must satisfy keep_alive concept");
    
    // Test that KeepAlive is valid
    BOOST_CHECK(keep_alive.is_valid());
    BOOST_CHECK_EQUAL(keep_alive.get(), cpu_executor.get());
    
    // Test that we can add work through KeepAlive
    std::atomic<bool> executed{false};
    keep_alive.add([&executed]() {
        executed.store(true);
    });
    
    // Wait for execution (with timeout)
    auto start = std::chrono::steady_clock::now();
    while (!executed.load() && 
           std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    BOOST_CHECK(executed.load());
    
    BOOST_TEST_MESSAGE("Executor KeepAlive functionality test passed");
}

BOOST_AUTO_TEST_SUITE_END()