// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE generic_future_concept_validation_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <concepts/future.hpp>
#include <raft/network.hpp>
#include <raft/http_transport.hpp>
#include <raft/coap_transport.hpp>
#include <network_simulator/connection.hpp>
#include <network_simulator/listener.hpp>
#include <raft/json_serializer.hpp>
#include <raft/console_logger.hpp>

#include <random>
#include <string>
#include <chrono>
#include <type_traits>
#include <memory>

namespace {
constexpr std::size_t property_test_iterations = 100;
constexpr std::chrono::milliseconds test_timeout{30000};

// Test constants
constexpr const char* test_node_a = "node_a";
constexpr const char* test_node_b = "node_b";
constexpr std::uint64_t test_node_id_a = 1;
constexpr std::uint64_t test_node_id_b = 2;
constexpr const char* test_endpoint_a = "coap://127.0.0.1:5683";
constexpr const char* test_endpoint_b = "coap://127.0.0.1:5684";
constexpr const char* test_url_a = "http://127.0.0.1:8080";
constexpr const char* test_url_b = "http://127.0.0.1:8081";
}

BOOST_AUTO_TEST_SUITE(generic_future_concept_validation_tests)

// Test that core implementations work with different future types
BOOST_AUTO_TEST_CASE(test_core_implementations_with_different_future_types,
                     *boost::unit_test::timeout(120)) {
    // Test 1: Verify that kythira::Future satisfies the future concept for various types
    static_assert(kythira::future<kythira::future_default<int>, int>,
                  "kythira::future_default<int> must satisfy future concept");
    static_assert(kythira::future<kythira::future_default<std::string>, std::string>,
                  "kythira::future_default<std::string> must satisfy future concept");
    static_assert(kythira::future<kythira::future_default<void>, void>,
                  "kythira::future_default<void> must satisfy future concept");
    static_assert(kythira::future<kythira::future_default<kythira::request_vote_response<>>,
                                  kythira::request_vote_response<>>,
                  "kythira::future_default<request_vote_response> must satisfy future concept");
    static_assert(kythira::future<kythira::future_default<kythira::append_entries_response<>>,
                                  kythira::append_entries_response<>>,
                  "kythira::future_default<append_entries_response> must satisfy future concept");
    static_assert(kythira::future<kythira::future_default<kythira::install_snapshot_response<>>,
                                  kythira::install_snapshot_response<>>,
                  "kythira::future_default<install_snapshot_response> must satisfy future concept");

    // Test 2: Verify HTTP transport client template instantiation
    // Skip - HTTP transport uses Types parameter now, tested in dedicated HTTP tests
    BOOST_TEST_MESSAGE(
        "Skipping HTTP transport instantiation test - API changed to use Types parameter");

    // Test 3: Verify CoAP transport client template instantiation
    // Skip - CoAP transport uses Types parameter now, tested in dedicated CoAP tests
    BOOST_TEST_MESSAGE(
        "Skipping CoAP transport instantiation test - API changed to use Types parameter");

    // Test 4: Verify network simulator Connection template instantiation
    using SimulatorTypes = network_simulator::DefaultNetworkTypes;

    static_assert(std::is_constructible_v<network_simulator::Connection<SimulatorTypes>,
                                          network_simulator::Endpoint<SimulatorTypes>,
                                          network_simulator::Endpoint<SimulatorTypes>,
                                          network_simulator::NetworkSimulator<SimulatorTypes>*>,
                  "Connection should be constructible with kythira::Future");

    // Test 5: Property-based test - verify concept compliance across iterations
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> int_dist(-1000, 1000);

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Test future concept operations with random values
        int random_value = int_dist(rng);

        // Test basic future operations
        {
            auto future = kythira::future_factory_default::makeFuture(random_value);
            BOOST_CHECK(future.isReady());
            BOOST_CHECK_EQUAL(std::move(future).get(), random_value);
        }

        // Test future chaining
        {
            auto base_future = kythira::future_factory_default::makeFuture(random_value);
            auto chained = std::move(base_future).thenValue([](int val) { return val * 2; });
            BOOST_CHECK_EQUAL(std::move(chained).get(), random_value * 2);
        }

        // Test timeout operations
        {
            auto timeout_future = kythira::future_factory_default::makeFuture(random_value);
            BOOST_CHECK(timeout_future.wait(std::chrono::milliseconds(1)));
        }

        // Test error handling
        {
            auto error_future = kythira::future_factory_default::makeExceptionalFuture<int>(
                std::make_exception_ptr(std::runtime_error("test error")));
            auto recovered = std::move(error_future).thenError([random_value](std::exception_ptr) {
                return random_value;
            });
            BOOST_CHECK_EQUAL(std::move(recovered).get(), random_value);
        }
    }
}

// Test that concept constraints are properly enforced
BOOST_AUTO_TEST_CASE(test_concept_constraints_enforcement, *boost::unit_test::timeout(60)) {
    // Test 1: Verify network_client concept with kythira::Future
    // Skip - HTTP and CoAP clients now use Types parameter, tested in dedicated tests
    BOOST_TEST_MESSAGE("Skipping network_client concept test - API changed to use Types parameter");

    // Test 2: Verify that the concept correctly validates required operations
    // Skip - depends on transport clients
    BOOST_TEST_MESSAGE("Skipping concept operations test - depends on transport clients");

    // Test 3: Verify future concept constraints in generic code
    auto test_future_concept = []<typename F, typename T>(F&& future_instance)
    requires kythira::future<std::remove_cvref_t<F>, T>
    {
        // Test that all required operations are available
        bool is_ready = future_instance.isReady();
        bool wait_result = future_instance.wait(std::chrono::milliseconds(1));

        // Return success if we can call the required operations
        return is_ready || wait_result;
    };

    // Test with various future types
    {
        auto int_future = kythira::future_factory_default::makeFuture(42);
        bool result = test_future_concept.template operator()<kythira::future_default<int>, int>(
            std::move(int_future));
        BOOST_CHECK(result);
    }

    {
        auto string_future = kythira::future_factory_default::makeFuture(std::string("test"));
        bool result = test_future_concept
                          .template operator()<kythira::future_default<std::string>, std::string>(
                              std::move(string_future));
        BOOST_CHECK(result);
    }

    {
        auto void_future = kythira::future_factory_default::makeFuture();
        bool result = test_future_concept.template operator()<kythira::future_default<void>, void>(
            std::move(void_future));
        BOOST_CHECK(result);
    }

    // Test 4: Verify that concept constraints prevent invalid instantiations
    // This is validated by the static_assert statements above - if the concepts
    // were not properly defined, these would fail to compile

    // Test 5: Property-based test for concept constraint validation
    std::random_device rd;
    std::mt19937 rng(rd());

    for (std::size_t i = 0; i < 50; ++i) {
        // Test that concept constraints work with various value types
        int random_int = static_cast<int>(rng() % 1000);
        double random_double = static_cast<double>(random_int) / 100.0;
        std::string random_string = "test_" + std::to_string(i);

        // Test int futures
        {
            auto future = kythira::future_factory_default::makeFuture(random_int);
            bool result =
                test_future_concept.template operator()<kythira::future_default<int>, int>(
                    std::move(future));
            BOOST_CHECK(result);
        }

        // Test double futures
        {
            auto future = kythira::future_factory_default::makeFuture(random_double);
            bool result =
                test_future_concept.template operator()<kythira::future_default<double>, double>(
                    std::move(future));
            BOOST_CHECK(result);
        }

        // Test string futures
        {
            auto future = kythira::future_factory_default::makeFuture(random_string);
            bool result =
                test_future_concept.template
                operator()<kythira::future_default<std::string>, std::string>(std::move(future));
            BOOST_CHECK(result);
        }
    }
}

// Test template instantiation with kythira::Future as default
BOOST_AUTO_TEST_CASE(test_template_instantiation_with_default_future,
                     *boost::unit_test::timeout(90)) {
    // Test 1: Verify that transport implementations can be instantiated with kythira::Future
    // Skip - HTTP and CoAP clients now use Types parameter, tested in dedicated tests
    BOOST_TEST_MESSAGE(
        "Skipping transport instantiation tests - API changed to use Types parameter");

    // Test 2: Verify that network simulator components work with kythira::Future
    using SimulatorTypes = network_simulator::DefaultNetworkTypes;

    // Test Connection instantiation (we can't actually create a NetworkSimulator here,
    // but we can verify the types are compatible)
    static_assert(
        std::is_same_v<
            decltype(std::declval<network_simulator::Connection<SimulatorTypes>>().read()),
            typename SimulatorTypes::future_bytes_type>,
        "Connection read() should return the correct future type");

    static_assert(
        std::is_same_v<decltype(std::declval<network_simulator::Connection<SimulatorTypes>>().write(
                           std::vector<std::byte>{})),
                       typename SimulatorTypes::future_bool_type>,
        "Connection write() should return the correct future type");

    // Test 3: Property-based test for template instantiation
    std::random_device rd;
    std::mt19937 rng(rd());

    for (std::size_t i = 0; i < 20; ++i) {
        // Test that we can create futures with various response types
        {
            kythira::request_vote_response<> rv_response;
            // Use constructor instead of direct field access
            rv_response = kythira::request_vote_response<>(rng() % 1000, (rng() % 2) == 1);

            auto future = kythira::future_factory_default::makeFuture(rv_response);
            BOOST_CHECK(future.isReady());
            auto result = std::move(future).get();
            BOOST_CHECK_EQUAL(result.term(), rv_response.term());
            BOOST_CHECK_EQUAL(result.vote_granted(), rv_response.vote_granted());
        }

        {
            kythira::append_entries_response<> ae_response;
            // Use constructor instead of direct field access
            ae_response = kythira::append_entries_response<>(rng() % 1000, (rng() % 2) == 1);

            auto future = kythira::future_factory_default::makeFuture(ae_response);
            BOOST_CHECK(future.isReady());
            auto result = std::move(future).get();
            BOOST_CHECK_EQUAL(result.term(), ae_response.term());
            BOOST_CHECK_EQUAL(result.success(), ae_response.success());
        }

        {
            kythira::install_snapshot_response<> is_response;
            // Use constructor instead of direct field access
            is_response = kythira::install_snapshot_response<>(rng() % 1000);

            auto future = kythira::future_factory_default::makeFuture(is_response);
            BOOST_CHECK(future.isReady());
            auto result = std::move(future).get();
            BOOST_CHECK_EQUAL(result.term(), is_response.term());
        }
    }

    // Test 4: Verify that generic algorithms work with kythira::Future
    auto process_any_future = []<typename F, typename T>(F future)
    requires kythira::future<F, T>
    {
        if (!future.isReady()) {
            future.wait(std::chrono::milliseconds(1000));
        }
        return std::move(future).get();
    };

    // Test with different future types
    {
        auto int_future = kythira::future_factory_default::makeFuture(42);
        int result = process_any_future.template operator()<kythira::future_default<int>, int>(
            std::move(int_future));
        BOOST_CHECK_EQUAL(result, 42);
    }

    {
        auto string_future = kythira::future_factory_default::makeFuture(std::string("test"));
        std::string result =
            process_any_future.template
            operator()<kythira::future_default<std::string>, std::string>(std::move(string_future));
        BOOST_CHECK_EQUAL(result, "test");
    }

    {
        auto void_future = kythira::future_factory_default::makeFuture();
        process_any_future.template operator()<kythira::future_default<void>, void>(
            std::move(void_future));
        // If we reach here without throwing, the test passed
        BOOST_CHECK(true);
    }
}

// Test that collective operations work with generic future types
BOOST_AUTO_TEST_CASE(test_collective_operations_with_generic_futures,
                     *boost::unit_test::timeout(60)) {
    // Test 1: Verify collectAll works with kythira::future_default
    {
        std::vector<kythira::future_default<int>> futures;
        for (int i = 0; i < 5; ++i) {
            futures.push_back(kythira::future_factory_default::makeFuture(i * 10));
        }

        // Not asserting isReady() immediately after the call: unlike
        // Folly's collectAll (which resolves synchronously/inline for
        // already-ready inputs), boost_backend's collectAll runs its
        // boost::when_all(...).then(...) continuation asynchronously (no
        // synchronous-if-ready guarantee in Boost.Thread's default launch
        // policy), so there's a real window where the result isn't ready
        // yet even though every input was. get() blocks until ready
        // regardless, so it's the portable way to observe the result.
        auto all_results = kythira::future_collector_default::collectAll(std::move(futures));
        auto results = std::move(all_results).get();
        BOOST_CHECK_EQUAL(results.size(), 5);

        for (std::size_t i = 0; i < results.size(); ++i) {
            BOOST_CHECK(results[i].hasValue());
            BOOST_CHECK_EQUAL(results[i].value(), static_cast<int>(i * 10));
        }
    }

    // Test 2: Verify collectAny works with kythira::future_default
    {
        std::vector<kythira::future_default<std::string>> futures;
        futures.push_back(kythira::future_factory_default::makeFuture(std::string("first")));
        futures.push_back(kythira::future_factory_default::makeFuture(std::string("second")));
        futures.push_back(kythira::future_factory_default::makeFuture(std::string("third")));

        // Not asserting isReady() immediately - see the collectAll comment
        // above; the same asynchronous-continuation caveat applies to
        // collectAny under boost_backend.
        auto any_result = kythira::future_collector_default::collectAny(std::move(futures));
        auto [index, result] = std::move(any_result).get();
        BOOST_CHECK(index < 3);
        BOOST_CHECK(result.hasValue());

        // The result should be one of our expected values
        std::string value = result.value();
        BOOST_CHECK(value == "first" || value == "second" || value == "third");
    }

    // Test 3: Property-based test for collective operations
    std::random_device rd;
    std::mt19937 rng(rd());

    for (std::size_t iteration = 0; iteration < 10; ++iteration) {
        std::size_t num_futures = 3 + (rng() % 5);  // 3-7 futures

        // Test wait_for_all with random values
        {
            std::vector<kythira::future_default<int>> futures;
            std::vector<int> expected_values;

            for (std::size_t i = 0; i < num_futures; ++i) {
                int value = static_cast<int>(rng() % 1000);
                expected_values.push_back(value);
                futures.push_back(kythira::future_factory_default::makeFuture(value));
            }

            auto all_results = kythira::future_collector_default::collectAll(std::move(futures));
            auto results = std::move(all_results).get();

            BOOST_CHECK_EQUAL(results.size(), expected_values.size());
            for (std::size_t i = 0; i < results.size(); ++i) {
                BOOST_CHECK(results[i].hasValue());
                BOOST_CHECK_EQUAL(results[i].value(), expected_values[i]);
            }
        }

        // Test wait_for_any with random values
        {
            std::vector<kythira::future_default<double>> futures;
            std::vector<double> expected_values;

            for (std::size_t i = 0; i < num_futures; ++i) {
                double value = static_cast<double>(rng() % 1000) / 100.0;
                expected_values.push_back(value);
                futures.push_back(kythira::future_factory_default::makeFuture(value));
            }

            auto any_result = kythira::future_collector_default::collectAny(std::move(futures));
            auto [index, result] = std::move(any_result).get();

            BOOST_CHECK(index < expected_values.size());
            BOOST_CHECK(result.hasValue());
            BOOST_CHECK_EQUAL(result.value(), expected_values[index]);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()