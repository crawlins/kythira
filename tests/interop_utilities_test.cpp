#define BOOST_TEST_MODULE interop_utilities_test
#include <boost/test/unit_test.hpp>

#include <folly/futures/Future.h>
#include <folly/Try.h>
#include <folly/Unit.h>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include "../include/raft/future.hpp"

namespace {
    constexpr int test_value = 42;
    constexpr const char* test_string = "test";
}

BOOST_AUTO_TEST_CASE(test_future_conversion_utilities, * boost::unit_test::timeout(10)) {
    // Test folly::Future -> kythira::Future conversion
    {
        auto folly_future = folly::makeFuture(test_value);
        auto kythira_future = kythira::interop::from_folly_future(std::move(folly_future));
        
        BOOST_CHECK(kythira_future.isReady());
        BOOST_CHECK_EQUAL(kythira_future.get(), test_value);
    }
    
    // Test kythira::Future -> folly::Future conversion
    {
        kythira::Future<int> kythira_future(test_value);
        auto folly_future = kythira::interop::to_folly_future(std::move(kythira_future));
        
        BOOST_CHECK(folly_future.isReady());
        BOOST_CHECK_EQUAL(std::move(folly_future).get(), test_value);
    }
    
    // Test void/Unit conversion
    {
        auto folly_unit_future = folly::makeFuture();
        auto kythira_void_future = kythira::interop::from_folly_future_unit(std::move(folly_unit_future));
        
        BOOST_CHECK(kythira_void_future.isReady());
        BOOST_CHECK_NO_THROW(kythira_void_future.get());
    }
}

BOOST_AUTO_TEST_CASE(test_try_conversion_utilities, * boost::unit_test::timeout(10)) {
    // Test folly::Try -> kythira::Try conversion
    {
        folly::Try<int> folly_try(test_value);
        auto kythira_try = kythira::interop::from_folly_try(std::move(folly_try));
        
        BOOST_CHECK(kythira_try.hasValue());
        BOOST_CHECK_EQUAL(kythira_try.value(), test_value);
    }
    
    // Test kythira::Try -> folly::Try conversion
    {
        kythira::Try<int> kythira_try(test_value);
        auto folly_try = kythira::interop::to_folly_try(std::move(kythira_try));
        
        BOOST_CHECK(folly_try.hasValue());
        BOOST_CHECK_EQUAL(folly_try.value(), test_value);
    }
    
    // Test void/Unit conversion
    {
        folly::Try<folly::Unit> folly_unit_try(folly::Unit{});
        auto kythira_void_try = kythira::interop::from_folly_try_unit(std::move(folly_unit_try));
        
        BOOST_CHECK(kythira_void_try.hasValue());
        BOOST_CHECK_NO_THROW(kythira_void_try.value());
    }
}

BOOST_AUTO_TEST_CASE(test_promise_conversion_utilities, * boost::unit_test::timeout(10)) {
    // Test folly::Promise -> kythira::Promise conversion
    {
        folly::Promise<int> folly_promise;
        auto kythira_promise = kythira::interop::from_folly_promise(std::move(folly_promise));
        
        BOOST_CHECK(!kythira_promise.isFulfilled());
        kythira_promise.setValue(test_value);
        BOOST_CHECK(kythira_promise.isFulfilled());
    }
    
    // Test kythira::Promise -> folly::Promise conversion
    {
        kythira::Promise<int> kythira_promise;
        auto folly_promise = kythira::interop::to_folly_promise(std::move(kythira_promise));
        
        BOOST_CHECK(!folly_promise.isFulfilled());
        folly_promise.setValue(test_value);
        BOOST_CHECK(folly_promise.isFulfilled());
    }
    
    // Test void/Unit conversion
    {
        folly::Promise<folly::Unit> folly_unit_promise;
        auto kythira_void_promise = kythira::interop::from_folly_promise_unit(std::move(folly_unit_promise));
        
        BOOST_CHECK(!kythira_void_promise.isFulfilled());
        kythira_void_promise.setValue();
        BOOST_CHECK(kythira_void_promise.isFulfilled());
    }
}

BOOST_AUTO_TEST_CASE(test_executor_conversion_utilities, * boost::unit_test::timeout(10)) {
    // Test folly::Executor* -> kythira::Executor conversion
    {
        auto cpu_executor = std::make_shared<folly::CPUThreadPoolExecutor>(1);
        auto kythira_executor = kythira::interop::from_folly_executor(cpu_executor.get());
        
        BOOST_CHECK(kythira_executor.is_valid());
        BOOST_CHECK_EQUAL(kythira_executor.get(), cpu_executor.get());
    }
    
    // Test kythira::Executor -> folly::Executor* conversion
    {
        auto cpu_executor = std::make_shared<folly::CPUThreadPoolExecutor>(1);
        kythira::Executor kythira_executor(cpu_executor.get());
        auto folly_executor_ptr = kythira::interop::to_folly_executor(kythira_executor);
        
        BOOST_CHECK(folly_executor_ptr != nullptr);
        BOOST_CHECK_EQUAL(folly_executor_ptr, cpu_executor.get());
    }
}

BOOST_AUTO_TEST_CASE(test_backward_compatibility_aliases, * boost::unit_test::timeout(10)) {
    // Test that type aliases work correctly
    {
        kythira::interop::future_type<int> future(test_value);
        kythira::interop::promise_type<int> promise;
        kythira::interop::semi_promise_type<int> semi_promise;
        kythira::interop::try_type<int> try_value(test_value);
        
        BOOST_CHECK(future.isReady());
        BOOST_CHECK(!promise.isFulfilled());
        BOOST_CHECK(!semi_promise.isFulfilled());
        BOOST_CHECK(try_value.hasValue());
    }
    
    // Test that factory and collector aliases work
    {
        auto factory_future = kythira::interop::future_factory_type::makeFuture(test_value);
        BOOST_CHECK(factory_future.isReady());
        BOOST_CHECK_EQUAL(factory_future.get(), test_value);
        
        std::vector<kythira::Future<int>> futures;
        futures.push_back(kythira::Future<int>(test_value));
        auto collected = kythira::interop::future_collector_type::collectAll(std::move(futures));
        BOOST_CHECK(collected.isReady());
    }
}