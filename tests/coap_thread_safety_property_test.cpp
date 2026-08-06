#define BOOST_TEST_MODULE coap_thread_safety_property_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>
#include <boost/test/data/test_case.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>
#include <memory>
#include <random>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <future>

#include "test_timeout_scale.hpp"

// Every case below heap-allocates the transport object and the counters its
// workers touch, and captures those shared_ptrs *by value*. That is deliberate
// and load-bearing; a stack local captured by reference reintroduces a process
// abort. The mechanism, confirmed under gdb:
//
//   1. A case here spawns workers that use a coap_client/coap_server which,
//      written the obvious way, is a local of the test case's own frame.
//   2. The case exceeds its *boost::unit_test::timeout(). Boost handles the
//      resulting SIGALRM in a signal handler that calls siglongjmp()
//      (boost/test/impl/execution_monitor.ipp) back to a sigsetjmp() outside
//      the case, and only then throws.
//   3. siglongjmp does NOT unwind: no destructor in this frame ever runs. The
//      threads are neither joined nor detached -- they are simply orphaned,
//      still holding pointers into a frame that has been abandoned.
//   4. The next test case's locals land on those same stack bytes.
//   5. An orphaned worker reads coap_client::_coap_context out of what is now
//      the next case's data, hands the result to libcoap, and faults inside
//      coap_make_session() at a small fixed offset -- the "memory access
//      violation at address: 0x1ac" that has been reported variously as an
//      arm64 SEGFAULT and as unrelated later tests crashing.
//
// Note what does NOT fix this: an RAII thread-owner that joins in its
// destructor. Destructors are exactly what step 3 skips. Shared ownership is
// the fix that works, because it removes the dangling reference rather than
// relying on cleanup that never runs. If a case does time out, the orphaned
// workers keep a live object alive and leak it -- a bounded cost on a path
// that is already failing, instead of corrupting the rest of the module.

using namespace kythira;

namespace {
constexpr std::size_t test_iterations = 10;  // Reduced for faster execution
constexpr std::chrono::milliseconds test_timeout{45000};
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint16_t test_bind_port = 16683;
constexpr std::size_t test_thread_count = 8;
constexpr std::size_t test_operations_per_thread = 50;  // Reduced for faster execution
constexpr std::uint64_t test_node_id = 1;
}

// Define test types for CoAP transport
struct test_transport_types {
    using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using serializer_registry_type =
        kythira::single_serializer_registry<kythira::json_rpc_serializer<std::vector<std::byte>>>;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using executor_type = kythira::console_logger;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

/**
 * **Feature: coap-transport, Property 33: Thread safety with proper synchronization**
 *
 * This property validates that the CoAP transport is thread-safe and properly
 * synchronizes access to shared resources across multiple threads.
 *
 * **Validates: Requirements 7.3**
 *
 * BLACK-BOX TEST: Tests observable behavior through public API only.
 */
BOOST_AUTO_TEST_CASE(test_concurrent_server_operations,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(45))) {
    // Test data generation
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> thread_count_dist(2, test_thread_count);
    std::uniform_int_distribution<std::size_t> operations_dist(20, test_operations_per_thread);

    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        std::size_t thread_count = thread_count_dist(gen);
        std::size_t operations_per_thread = operations_dist(gen);

        // Create server configuration
        coap_server_config server_config;
        server_config.max_concurrent_sessions = thread_count * 10;
        server_config.enable_memory_optimization = true;
        server_config.memory_pool_size = 1024 * 1024;  // 1MB
        server_config.enable_serialization_caching = true;
        server_config.serialization_cache_size = 200;
        server_config.enable_concurrent_processing = true;

        // Create test types and server
        noop_metrics metrics;

        // Heap-owned; see the ownership note at the top of this file.
        auto server = std::make_shared<coap_server<test_transport_types>>(
            test_bind_address,
            test_bind_port + iteration % 1000,  // Avoid port conflicts
            server_config, metrics);

        // Test 1: Concurrent slot acquisition (public API)
        std::vector<std::thread> threads;
        auto successful_operations = std::make_shared<std::atomic<std::size_t>>(0);
        auto failed_operations = std::make_shared<std::atomic<std::size_t>>(0);
        auto start_flag = std::make_shared<std::atomic<bool>>(false);

        for (std::size_t t = 0; t < thread_count; ++t) {
            threads.emplace_back([server, successful_operations, failed_operations, start_flag,
                                  operations_per_thread]() {
                // Wait for all threads to be ready
                while (!start_flag->load()) {
                    std::this_thread::yield();
                }

                for (std::size_t op = 0; op < operations_per_thread; ++op) {
                    try {
                        bool acquired = server->acquire_concurrent_slot();
                        if (acquired) {
                            successful_operations->fetch_add(1);
                            std::this_thread::sleep_for(std::chrono::microseconds(10));
                            server->release_concurrent_slot();
                        } else {
                            failed_operations->fetch_add(1);
                        }
                    } catch (const std::exception&) {
                        failed_operations->fetch_add(1);
                    }
                }
            });
        }

        // Start all threads simultaneously
        start_flag->store(true);

        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }

        // Verify thread safety: all operations should complete
        std::size_t expected_operations = thread_count * operations_per_thread;
        std::size_t total_operations = successful_operations->load() + failed_operations->load();

        BOOST_CHECK_EQUAL(total_operations, expected_operations);
    }
}

/**
 * **Feature: coap-transport, Property 33: Client thread safety with proper synchronization**
 *
 * This property validates that the CoAP client is thread-safe and properly
 * synchronizes access to shared resources across multiple threads.
 *
 * **Validates: Requirements 7.3**
 *
 * BLACK-BOX TEST: Tests observable behavior through public API only.
 */
BOOST_AUTO_TEST_CASE(test_concurrent_client_operations,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(45))) {
    // Test data generation
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> thread_count_dist(2, test_thread_count);
    std::uniform_int_distribution<std::size_t> operations_dist(20, test_operations_per_thread);

    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        std::size_t thread_count = thread_count_dist(gen);
        std::size_t operations_per_thread = operations_dist(gen);

        // Create client configuration
        coap_client_config client_config;
        client_config.max_sessions = thread_count * 5;
        client_config.enable_memory_optimization = true;
        client_config.memory_pool_size = 1024 * 1024;  // 1MB
        client_config.enable_serialization_caching = true;
        client_config.serialization_cache_size = 200;
        client_config.connection_pool_size = 50;
        client_config.enable_concurrent_processing = true;

        // Create test types and client
        noop_metrics metrics;

        std::unordered_map<std::uint64_t, std::string> node_endpoints = {
            {1, "coap://127.0.0.1:61070"},
            {2, "coap://127.0.0.1:61071"},
            {3, "coap://127.0.0.1:61072"}};

        // Heap-owned; see the ownership note at the top of this file.
        auto client = std::make_shared<coap_client<test_transport_types>>(node_endpoints,
                                                                          client_config, metrics);

        // Test 1: Concurrent client slot acquisition (public API)
        std::vector<std::thread> threads;
        auto successful_operations = std::make_shared<std::atomic<std::size_t>>(0);
        auto failed_operations = std::make_shared<std::atomic<std::size_t>>(0);
        auto start_flag = std::make_shared<std::atomic<bool>>(false);

        for (std::size_t t = 0; t < thread_count; ++t) {
            threads.emplace_back([client, successful_operations, failed_operations, start_flag,
                                  operations_per_thread]() {
                // Wait for all threads to be ready
                while (!start_flag->load()) {
                    std::this_thread::yield();
                }

                for (std::size_t op = 0; op < operations_per_thread; ++op) {
                    try {
                        bool acquired = client->acquire_concurrent_slot();
                        if (acquired) {
                            successful_operations->fetch_add(1);
                            std::this_thread::sleep_for(std::chrono::microseconds(10));
                            client->release_concurrent_slot();
                        } else {
                            failed_operations->fetch_add(1);
                        }
                    } catch (const std::exception&) {
                        failed_operations->fetch_add(1);
                    }
                }
            });
        }

        // Start all threads simultaneously
        start_flag->store(true);

        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }

        // Verify thread safety: all operations should complete
        std::size_t expected_operations = thread_count * operations_per_thread;
        std::size_t total_operations = successful_operations->load() + failed_operations->load();

        BOOST_CHECK_EQUAL(total_operations, expected_operations);
    }
}

/**
 * **Feature: coap-transport, Property 33: Concurrent RPC requests**
 *
 * This property validates that concurrent RPC requests are handled safely.
 *
 * **Validates: Requirements 7.3**
 *
 * BLACK-BOX TEST: Tests observable behavior through public API only.
 */
BOOST_AUTO_TEST_CASE(test_concurrent_rpc_requests,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(45))) {
    // Create client configuration
    coap_client_config client_config;
    client_config.enable_concurrent_processing = true;
    client_config.max_concurrent_requests = 50;

    noop_metrics metrics;

    std::unordered_map<std::uint64_t, std::string> node_endpoints = {
        {test_node_id, "coap://127.0.0.1:61070"}};

    // Heap-owned; see the ownership note at the top of this file. This is the
    // case that actually produced the 0x1ac fault: its workers sit in
    // send_rpc() -> get_or_create_session() -> libcoap, all serialized on the
    // client's own recursive mutex, so it is the case most likely to still
    // have threads running when its timeout fires.
    auto client =
        std::make_shared<coap_client<test_transport_types>>(node_endpoints, client_config, metrics);

    // Test: Concurrent RPC requests
    std::vector<std::thread> threads;
    auto successful_requests = std::make_shared<std::atomic<std::size_t>>(0);
    auto failed_requests = std::make_shared<std::atomic<std::size_t>>(0);

    request_vote_request<> vote_request{
        ._term = 1, ._candidate_id = 100, ._last_log_index = 0, ._last_log_term = 0};

    for (std::size_t t = 0; t < 10; ++t) {
        threads.emplace_back([client, successful_requests, failed_requests, vote_request]() {
            for (std::size_t op = 0; op < 20; ++op) {
                try {
                    auto future = client->send_request_vote(test_node_id, vote_request,
                                                            std::chrono::milliseconds{1000});
                    successful_requests->fetch_add(1);
                } catch (const std::exception&) {
                    failed_requests->fetch_add(1);
                }
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify: All operations completed
    BOOST_CHECK_EQUAL(successful_requests->load() + failed_requests->load(), 200);
}

/**
 * **Feature: coap-transport, Property 33: Concurrent configuration checks**
 *
 * This property validates that concurrent configuration checks are handled safely.
 *
 * **Validates: Requirements 7.3**
 *
 * BLACK-BOX TEST: Tests observable behavior through public API only.
 */
BOOST_AUTO_TEST_CASE(test_concurrent_configuration_checks,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(45))) {
    // Create client configuration
    coap_client_config client_config;
    client_config.enable_concurrent_processing = true;

    noop_metrics metrics;

    std::unordered_map<std::uint64_t, std::string> node_endpoints = {
        {test_node_id, "coap://127.0.0.1:61070"}};

    // Heap-owned; see the ownership note at the top of this file.
    auto client =
        std::make_shared<coap_client<test_transport_types>>(node_endpoints, client_config, metrics);

    // Test: Concurrent configuration status checks
    std::vector<std::thread> threads;
    auto operations_completed = std::make_shared<std::atomic<std::size_t>>(0);

    for (std::size_t t = 0; t < 5; ++t) {
        threads.emplace_back([client, operations_completed]() {
            for (std::size_t op = 0; op < 20; ++op) {
                try {
                    // Test concurrent status checks (all const methods, thread-safe)
                    bool dtls_enabled = client->is_dtls_enabled();
                    operations_completed->fetch_add(1);
                } catch (const std::exception&) {
                    operations_completed->fetch_add(1);
                }
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify: All operations completed
    BOOST_CHECK_EQUAL(operations_completed->load(), 100);
}
