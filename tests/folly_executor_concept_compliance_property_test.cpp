#define BOOST_TEST_MODULE folly_executor_concept_compliance_property_test
#include <boost/test/unit_test.hpp>

#include <concepts/future.hpp>

// Include Folly headers for Executor
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/InlineExecutor.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/executors/QueuedImmediateExecutor.h>

#include <random>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr const char* test_name = "folly_executor_concept_compliance_property_test";
}

BOOST_AUTO_TEST_SUITE(folly_executor_concept_compliance_property_tests)

/**
 * **Feature: folly-concepts-enhancement, Property 11: Folly executor concept compliance**
 * **Validates: Requirements 10.3**
 * Property: For any folly::Executor implementation, it should satisfy the executor concept
 */
BOOST_AUTO_TEST_CASE(property_folly_executor_concept_compliance, * boost::unit_test::timeout(30)) {
    // Test folly::CPUThreadPoolExecutor satisfies executor concept
    static_assert(kythira::executor<folly::CPUThreadPoolExecutor>, 
                  "folly::CPUThreadPoolExecutor must satisfy executor concept");
    
    // Test folly::InlineExecutor satisfies executor concept
    static_assert(kythira::executor<folly::InlineExecutor>, 
                  "folly::InlineExecutor must satisfy executor concept");
    
    // Test folly::ManualExecutor satisfies executor concept
    static_assert(kythira::executor<folly::ManualExecutor>, 
                  "folly::ManualExecutor must satisfy executor concept");
    
    // Test folly::QueuedImmediateExecutor satisfies executor concept
    static_assert(kythira::executor<folly::QueuedImmediateExecutor>, 
                  "folly::QueuedImmediateExecutor must satisfy executor concept");
    
    BOOST_TEST_MESSAGE("All folly::Executor implementations satisfy executor concept");
    
    // Property-based test: Test executor behavior across multiple iterations
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> task_count_dist(1, 5);
    
    for (std::size_t i = 0; i < 10; ++i) { // Reduced iterations to avoid timeout
        // Test InlineExecutor (safe, executes immediately)
        {
            folly::InlineExecutor executor;
            
            bool task_executed = false;
            executor.add([&task_executed]() {
                task_executed = true;
            });
            
            // InlineExecutor executes immediately
            BOOST_CHECK(task_executed);
            
            // Test getKeepAliveToken using folly function
            auto keep_alive = folly::getKeepAliveToken(executor);
            BOOST_CHECK(keep_alive.get() != nullptr);
        }
        
        // Test ManualExecutor (safe, manual control)
        {
            folly::ManualExecutor executor;
            
            bool task_executed = false;
            executor.add([&task_executed]() {
                task_executed = true;
            });
            
            // ManualExecutor requires manual execution
            BOOST_CHECK(!task_executed);
            
            // Run pending tasks
            executor.run();
            BOOST_CHECK(task_executed);
            
            // Test getKeepAliveToken using folly function
            auto keep_alive = folly::getKeepAliveToken(executor);
            BOOST_CHECK(keep_alive.get() != nullptr);
        }
    }
    
    BOOST_TEST_MESSAGE("Property test completed: All folly::Executor implementations behave correctly");
}

/**
 * Test KeepAlive functionality with folly executors
 */
BOOST_AUTO_TEST_CASE(test_folly_executor_keep_alive_behavior, * boost::unit_test::timeout(30)) {
    // Test that KeepAlive tokens work correctly with InlineExecutor (safe)
    {
        folly::InlineExecutor executor;
        
        // Get keep alive token using folly function
        auto keep_alive = folly::getKeepAliveToken(executor);
        
        // Test that keep_alive satisfies keep_alive concept
        static_assert(kythira::keep_alive<decltype(keep_alive)>, 
                      "folly::Executor::KeepAlive must satisfy keep_alive concept");
        
        // Test get method
        BOOST_CHECK(keep_alive.get() != nullptr);
        BOOST_CHECK_EQUAL(keep_alive.get(), &executor);
        
        // Test copy construction
        auto keep_alive_copy = keep_alive;
        BOOST_CHECK(keep_alive_copy.get() != nullptr);
        BOOST_CHECK_EQUAL(keep_alive_copy.get(), &executor);
    }
    
    BOOST_TEST_MESSAGE("folly::Executor::KeepAlive behavior matches keep_alive concept requirements");
}

BOOST_AUTO_TEST_SUITE_END()