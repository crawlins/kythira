#define BOOST_TEST_MODULE FutureTest
#include <boost/test/included/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

using namespace network_simulator;

// Test constants
namespace {
    constexpr int test_value = 42;
    constexpr const char* test_string = "test";
    constexpr auto test_timeout = std::chrono::milliseconds{100};
}

// Test Try wrapper with value
BOOST_AUTO_TEST_CASE(test_try_with_value, * boost::unit_test::timeout(30)) {
    Try<int> t(test_value);
    
    BOOST_TEST(t.hasValue());
    BOOST_TEST(!t.hasException());
    BOOST_TEST(t.value() == test_value);
    
    // Verify it satisfies the try_type concept
    static_assert(kythira::try_type<Try<int>, int>, "Try<int> should satisfy try_type concept");
}

// Test Try wrapper with exception
BOOST_AUTO_TEST_CASE(test_try_with_exception, * boost::unit_test::timeout(30)) {
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    Try<int> t(ex);
    
    BOOST_TEST(!t.hasValue());
    BOOST_TEST(t.hasException());
    
    // Accessing value should throw
    BOOST_CHECK_THROW(t.value(), std::exception);
}

// Test Try wrapper with folly::Try
BOOST_AUTO_TEST_CASE(test_try_from_folly_try, * boost::unit_test::timeout(30)) {
    folly::Try<int> folly_try(test_value);
    Try<int> t(std::move(folly_try));
    
    BOOST_TEST(t.hasValue());
    BOOST_TEST(t.value() == test_value);
}

// Test Future wrapper with value
BOOST_AUTO_TEST_CASE(test_future_with_value, * boost::unit_test::timeout(30)) {
    Future<int> f(test_value);
    
    BOOST_TEST(f.isReady());
    BOOST_TEST(f.get() == test_value);
}

// Test Future wrapper with exception
BOOST_AUTO_TEST_CASE(test_future_with_exception, * boost::unit_test::timeout(30)) {
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    Future<int> f(ex);
    
    BOOST_TEST(f.isReady());
    BOOST_CHECK_THROW(f.get(), std::runtime_error);
}

// Test Future then() chaining
BOOST_AUTO_TEST_CASE(test_future_then, * boost::unit_test::timeout(30)) {
    Future<int> f(test_value);
    
    auto f2 = f.then([](int val) { return val * 2; });
    
    BOOST_TEST(f2.get() == test_value * 2);
}

// Test Future onError() handling
BOOST_AUTO_TEST_CASE(test_future_on_error, * boost::unit_test::timeout(30)) {
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    Future<int> f(ex);
    
    auto f2 = f.onError([](folly::exception_wrapper) { return test_value; });
    
    BOOST_TEST(f2.get() == test_value);
}

// Test Future wait() with timeout
BOOST_AUTO_TEST_CASE(test_future_wait, * boost::unit_test::timeout(60)) {
    folly::Promise<int> promise;
    Future<int> f(promise.getFuture());
    
    // Should not be ready yet
    BOOST_TEST(!f.isReady());
    
    // Wait with short timeout should return false
    BOOST_TEST(!f.wait(std::chrono::milliseconds{10}));
    
    // Fulfill the promise
    promise.setValue(test_value);
    
    // Now should be ready
    BOOST_TEST(f.wait(test_timeout));
    BOOST_TEST(f.isReady());
}

// Test wait_for_any with multiple futures
BOOST_AUTO_TEST_CASE(test_wait_for_any, * boost::unit_test::timeout(90)) {
    folly::Promise<int> promise1;
    folly::Promise<int> promise2;
    folly::Promise<int> promise3;
    
    std::vector<Future<int>> futures;
    futures.push_back(Future<int>(promise1.getFuture()));
    futures.push_back(Future<int>(promise2.getFuture()));
    futures.push_back(Future<int>(promise3.getFuture()));
    
    // Fulfill the second promise in a separate thread
    std::thread t([&promise2]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        promise2.setValue(test_value);
    });
    
    // Wait for any future to complete
    auto result_future = wait_for_any(std::move(futures));
    auto [index, try_result] = result_future.get();
    
    // Should be the second future (index 1)
    BOOST_TEST(index == 1);
    BOOST_TEST(try_result.hasValue());
    BOOST_TEST(try_result.value() == test_value);
    
    t.join();
    
    // Clean up remaining promises
    promise1.setValue(0);
    promise3.setValue(0);
}

// Test wait_for_all with multiple futures
BOOST_AUTO_TEST_CASE(test_wait_for_all, * boost::unit_test::timeout(90)) {
    folly::Promise<int> promise1;
    folly::Promise<int> promise2;
    folly::Promise<int> promise3;
    
    std::vector<Future<int>> futures;
    futures.push_back(Future<int>(promise1.getFuture()));
    futures.push_back(Future<int>(promise2.getFuture()));
    futures.push_back(Future<int>(promise3.getFuture()));
    
    // Fulfill all promises in separate threads
    std::thread t1([&promise1]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        promise1.setValue(1);
    });
    
    std::thread t2([&promise2]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        promise2.setValue(2);
    });
    
    std::thread t3([&promise3]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        promise3.setValue(3);
    });
    
    // Wait for all futures to complete
    auto result_future = wait_for_all(std::move(futures));
    auto results = result_future.get();
    
    // Should have 3 results
    BOOST_TEST(results.size() == 3);
    
    // All should have values
    BOOST_TEST(results[0].hasValue());
    BOOST_TEST(results[1].hasValue());
    BOOST_TEST(results[2].hasValue());
    
    // Check values
    BOOST_TEST(results[0].value() == 1);
    BOOST_TEST(results[1].value() == 2);
    BOOST_TEST(results[2].value() == 3);
    
    t1.join();
    t2.join();
    t3.join();
}

// Test wait_for_all with mixed success and failure
BOOST_AUTO_TEST_CASE(test_wait_for_all_with_exceptions, * boost::unit_test::timeout(60)) {
    folly::Promise<int> promise1;
    folly::Promise<int> promise2;
    folly::Promise<int> promise3;
    
    std::vector<Future<int>> futures;
    futures.push_back(Future<int>(promise1.getFuture()));
    futures.push_back(Future<int>(promise2.getFuture()));
    futures.push_back(Future<int>(promise3.getFuture()));
    
    // Fulfill promises with mix of values and exceptions
    promise1.setValue(test_value);
    promise2.setException(std::runtime_error(test_string));
    promise3.setValue(test_value * 2);
    
    // Wait for all futures to complete
    auto result_future = wait_for_all(std::move(futures));
    auto results = result_future.get();
    
    // Should have 3 results
    BOOST_TEST(results.size() == 3);
    
    // First should have value
    BOOST_TEST(results[0].hasValue());
    BOOST_TEST(results[0].value() == test_value);
    
    // Second should have exception
    BOOST_TEST(results[1].hasException());
    
    // Third should have value
    BOOST_TEST(results[2].hasValue());
    BOOST_TEST(results[2].value() == test_value * 2);
}

// Test Message with empty payload
BOOST_AUTO_TEST_CASE(test_message_empty_payload, * boost::unit_test::timeout(30)) {
    Message<std::string, unsigned short> msg(
        "src",
        8080,
        "dst",
        9090
    );
    
    BOOST_TEST(msg.payload().empty());
}

// Test Message with non-empty payload
BOOST_AUTO_TEST_CASE(test_message_with_payload, * boost::unit_test::timeout(30)) {
    std::vector<std::byte> payload;
    for (char c : std::string(test_string)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    Message<std::string, unsigned short> msg(
        "src",
        8080,
        "dst",
        9090,
        payload
    );
    
    BOOST_TEST(msg.payload().size() == payload.size());
    
    // Verify payload contents by converting to int for comparison
    for (std::size_t i = 0; i < payload.size(); ++i) {
        BOOST_TEST(static_cast<int>(msg.payload()[i]) == static_cast<int>(payload[i]));
    }
}

// Test Message with various address/port types
BOOST_AUTO_TEST_CASE(test_message_various_types, * boost::unit_test::timeout(30)) {
    // Test with unsigned long address and string port
    Message<unsigned long, std::string> msg1(
        0xC0A80101UL,  // 192.168.1.1
        "http",
        0xC0A80102UL,  // 192.168.1.2
        "https"
    );
    
    BOOST_TEST(msg1.source_address() == 0xC0A80101UL);
    BOOST_TEST(msg1.source_port() == "http");
    
    // Test with IPv4Address
    in_addr addr1{};
    addr1.s_addr = htonl(0xC0A80101);
    
    in_addr addr2{};
    addr2.s_addr = htonl(0xC0A80102);
    
    Message<IPv4Address, unsigned short> msg2(
        IPv4Address(addr1),
        8080,
        IPv4Address(addr2),
        9090
    );
    
    BOOST_TEST(msg2.source_port() == 8080);
    BOOST_TEST(msg2.destination_port() == 9090);
}
