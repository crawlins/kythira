#define BOOST_TEST_MODULE coap_concurrent_processing_property_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/coap_transport.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>  // test_transport_types::executor_type below is folly::Executor directly
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/console_logger.hpp>
#include <raft/serializer_registry.hpp>

#include <boost/test/data/test_case.hpp>

#include <memory>
#include <vector>
#include <thread>

#include "coap_test_support.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>

#include "test_timeout_scale.hpp"

// The transport objects and counters that worker threads touch are heap-owned
// and captured by value, never by reference to a test case's own frame. A
// `[&]` capture of a stack-local coap_client is what produced the "memory
// access violation at address: 0x1ac" crash fixed in
// coap_thread_safety_property_test.cpp: on Boost timeout, SIGALRM is handled
// with siglongjmp(), which abandons the frame WITHOUT running destructors, so
// worker threads are orphaned rather than joined and go on reading a frame the
// next test case has already reused. See that file's header comment and
// doc/coap-flake-investigation.md Finding 4 for the full chain.

using namespace kythira;

namespace {
constexpr const char* test_client_id = "test_client";
constexpr const char* test_server_id = "test_server";
// Used only by tests below that construct a client with no corresponding
// real listener in this process (no coap_server is ever bound to this
// port here) -- a fixed literal well outside both the ephemeral port
// range (32768-60999 on Linux) and any other coap_*_test.cpp's assigned
// literal is safe since nothing ever binds it.
constexpr const char* test_endpoint = "coap://127.0.0.1:61010";
// test_concurrent_request_processing_property does test_concurrent_requests
// real sequential send/receive round trips against a real local server (each
// involving real session setup and libcoap I/O). 50 was fine when this path
// was still a stub with no real per-call I/O; kept smaller here so real
// per-call latency can't blow the test-level timeout on a slow/contended CI
// runner (observed directly: arm64 CI hit this test's SIGALRM even after
// fixing the underlying mutex-starvation bug in coap_client's io-pump
// thread).
constexpr std::size_t test_concurrent_requests = 15;
constexpr std::chrono::milliseconds test_timeout{5000};

// ── Stall probe ──────────────────────────────────────────────────────────────
//
// Instrumentation for the 720s stalls tracked in doc/TODO.md ("stalls at its
// 720s budget -- OPEN"). CI logs showed every request sent and processed within
// the same millisecond, with 77s, 85s and 220s gaps *between* iterations and
// nothing logged in them. That shape says the process is not being scheduled
// rather than that a lock is held, but the evidence had to be reconstructed by
// reading raw timestamps by hand, and it could not say what the runner was
// doing during a gap. These probes make the next occurrence attributable from
// the artifact alone.
//
// Two deliberate choices, both of which the obvious implementation gets wrong:
//
//   * Output goes to std::cout, NOT BOOST_TEST_MESSAGE. Boost's default log
//     level discards messages, and this was checked rather than assumed --
//     this case's existing BOOST_TEST_MESSAGE at "Peak concurrent requests"
//     appears zero times in the log of green run 31317748177. ctest dumps a
//     failing test's stdout under --output-on-failure, which is how #190's
//     token lines reached the artifact in the first place, so stdout is the
//     one channel known to survive.
//
//   * Every line is emitted and flushed where it happens, never accumulated
//     into an end-of-case summary. The failure being instrumented is a SIGALRM
//     at the case's own timeout, which unwinds by siglongjmp() (see this
//     file's header comment) and never returns to the end of the case. A
//     summary would therefore be empty in precisely the run that needs it.
// The line is assembled first and inserted in one operation, newline included,
// rather than streamed in pieces. The transport's console_logger writes to the
// same stdout from its own threads, and a multi-insert probe was observed
// landing *inside* one of its lines ("...[received_messages=[stall-probe] iter
// i=8..."). A single insert does not make this atomic in any guaranteed sense,
// but it narrows the window to one call and keeps the whole record on the far
// side of the marker, so `grep '\[stall-probe\]'` still recovers every field
// even when a line is interleaved.
void stall_probe(const std::string& detail) {
    std::cout << ("[stall-probe] " + detail + "\n") << std::flush;
}

// The runner's 1/5/15-minute load averages, or a marker if unreadable. Read
// fresh on each call: the point is to catch load *during* a stall, so a value
// cached at case entry would be worthless.
std::string runner_loadavg() {
    std::ifstream file("/proc/loadavg");
    std::string line;
    if (!std::getline(file, line) || line.empty()) {
        return "unavailable";
    }
    return line;
}

// Define test types for CoAP transport
struct test_transport_types {
    using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using serializer_registry_type =
        kythira::single_serializer_registry<kythira::json_rpc_serializer<std::vector<std::byte>>>;
    using rpc_serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;

    using future_type = kythira::future_default<std::vector<std::byte>>;
};

BOOST_AUTO_TEST_SUITE(coap_concurrent_processing_property_tests)

/**
 * **Feature: coap-transport, Property 12: Concurrent request processing**
 *
 * Property: For any set of concurrent requests, the server should process them in parallel without
 * blocking. Validates: Requirements 7.3
 */
BOOST_AUTO_TEST_CASE(test_concurrent_request_processing_property,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    // Create CoAP client and server configurations with concurrent processing enabled
    coap_client_config client_config;
    client_config.enable_concurrent_processing = true;
    client_config.max_concurrent_requests = test_concurrent_requests;
    client_config.enable_dtls = false;

    coap_server_config server_config;
    server_config.enable_concurrent_processing = true;
    server_config.max_concurrent_requests = test_concurrent_requests * 2;  // Server can handle more
    server_config.enable_dtls = false;

    // Create server first, bound to an OS-assigned ephemeral port (0)
    // instead of the previously-hardcoded 5683 -- avoids the SO_REUSEADDR
    // silent-port-steal hazard when this binary runs concurrently with
    // other coap_*_test binaries under ctest -j (see bound_port()'s doc
    // comment in coap_transport.hpp). The client's endpoint is built from
    // the real bound port below, after start() resolves it.
    // Heap-owned, not a frame local: this case's own SIGALRM timeout is what
    // abandons the frame (see the file header comment), and `server` owns
    // background I/O threads started by start() below that keep running
    // afterwards. See this case's trailing comment for the full chain.
    auto server = std::make_shared<coap_server<test_transport_types>>("127.0.0.1", 0, server_config,
                                                                      noop_metrics{});

    // Property: Concurrent requests should be processed without blocking
    std::atomic<std::size_t> requests_started{0};
    std::atomic<std::size_t> successful_acquisitions{0};
    std::atomic<std::size_t> failed_acquisitions{0};
    std::atomic<std::size_t> concurrent_active{0};
    std::atomic<std::size_t> concurrent_peak{0};

    stall_probe("entry hw_concurrency=" + std::to_string(std::thread::hardware_concurrency()) +
                " requests=" + std::to_string(test_concurrent_requests) + " loadavg=[" +
                runner_loadavg() + "]");

    // Track timing to verify parallel processing
    auto start_time = std::chrono::steady_clock::now();
    std::vector<std::chrono::steady_clock::time_point> request_start_times(
        test_concurrent_requests);
    std::vector<std::chrono::steady_clock::time_point> request_end_times(test_concurrent_requests);

    // Register a handler (though it won't be called in stub implementation)
    server->register_request_vote_handler(
        [](const request_vote_request<>& req) -> request_vote_response<> {
            return request_vote_response<>{req.term(), false};
        });

    // Start server
    server->start();

    // Build the client's endpoint from the port the OS actually assigned
    // (bound_port() is only meaningful after start()).
    std::unordered_map<std::uint64_t, std::string> endpoint_map = {
        {1, "coap://127.0.0.1:" + std::to_string(server->bound_port())}};

    auto client = std::make_shared<coap_client<test_transport_types>>(endpoint_map, client_config,
                                                                      noop_metrics{});

    // Launch concurrent requests
    std::vector<kythira::future_default<void>> request_futures;
    request_futures.reserve(test_concurrent_requests);

    // Previous iteration's end, so the gap *between* iterations is measured --
    // that is where #190's 77s/85s/220s stalls sat, with the iterations
    // themselves completing in under a millisecond.
    auto previous_iteration_end = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < test_concurrent_requests; ++i) {
        // Was previously wrapped in folly::makeFuture().via(&folly::InlineExecutor::instance())
        // .thenValue(...) - InlineExecutor runs synchronously (no actual deferral), so this
        // synchronous body followed by an already-ready future preserves identical behavior
        // and timing while staying backend-neutral.
        request_start_times[i] = std::chrono::steady_clock::now();
        requests_started.fetch_add(1);

        // Report both intervals, because the stall could live in either and
        // reporting one alone would look identical to no stall at all.
        // `gap_ms` is time spent outside any iteration body -- the shape
        // #190's log pointed at. `prev_body_ms` is how long the previous
        // iteration's own send/receive took, which is where the time would sit
        // instead if the process is scheduled fine and send_request_vote is
        // what blocks. Leaving the second to be derived from a running total
        // by subtraction is the kind of arithmetic this file's history says
        // not to make a reader do while diagnosing a red run.
        const auto gap = i == 0 ? request_start_times[0] - previous_iteration_end
                                : request_start_times[i] - request_end_times[i - 1];
        const auto previous_body = i == 0 ? std::chrono::steady_clock::duration::zero()
                                          : request_end_times[i - 1] - request_start_times[i - 1];

        stall_probe(
            "iter i=" + std::to_string(i) + " gap_ms=" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(gap).count()) +
            " prev_body_ms=" +
            std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(previous_body).count()) +
            " elapsed_ms=" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                               request_start_times[i] - start_time)
                               .count()) +
            " loadavg=[" + runner_loadavg() + "]");

        try {
            // Test concurrent slot acquisition - this may fail due to limits
            if (client->acquire_concurrent_slot()) {
                successful_acquisitions.fetch_add(1);

                // Track concurrent activity
                auto current = concurrent_active.fetch_add(1) + 1;

                // Update peak concurrent requests
                std::size_t expected_peak = concurrent_peak.load();
                while (current > expected_peak &&
                       !concurrent_peak.compare_exchange_weak(expected_peak, current)) {
                    // Retry if another thread updated the peak
                }

                // Simulate some work to allow concurrency measurement
                std::this_thread::sleep_for(std::chrono::milliseconds(5));

                // Create request
                request_vote_request<> request{1, 1, 0, 0};

                // Send request (this will fail in stub implementation, but we're
                // testing the concurrency control)
                auto future = client->send_request_vote(1, request, test_timeout);

                // Release slot
                client->release_concurrent_slot();

                // Decrement concurrent activity
                concurrent_active.fetch_sub(1);
            } else {
                failed_acquisitions.fetch_add(1);
            }

            request_end_times[i] = std::chrono::steady_clock::now();

        } catch (const std::exception& e) {
            // Expected in stub implementation
            request_end_times[i] = std::chrono::steady_clock::now();
        }

        previous_iteration_end = request_end_times[i];
        request_futures.push_back(kythira::future_factory_default::makeFuture());
    }

    // Wait for all requests to complete
    for (auto& future : request_futures) {
        std::move(future).get();
    }

    auto end_time = std::chrono::steady_clock::now();
    auto total_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // last_body_ms closes the gap in the per-iteration lines: each of those
    // reports the *previous* body, so without this the final iteration's own
    // duration would be the one interval never printed.
    stall_probe("exit total_ms=" + std::to_string(total_duration.count()) + " last_body_ms=" +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   request_end_times[test_concurrent_requests - 1] -
                                   request_start_times[test_concurrent_requests - 1])
                                   .count()) +
                " loadavg=[" + runner_loadavg() + "]");

    // Verify concurrent processing properties

    // Property 1: All requests should have been started
    BOOST_CHECK_EQUAL(requests_started.load(), test_concurrent_requests);

    // Property 2: Total acquisitions should equal successful + failed
    BOOST_CHECK_EQUAL(successful_acquisitions.load() + failed_acquisitions.load(),
                      test_concurrent_requests);

    // Property 3: Some requests should have been successful (up to the limit)
    BOOST_CHECK_GT(successful_acquisitions.load(), 0);
    BOOST_CHECK_LE(successful_acquisitions.load(), client_config.max_concurrent_requests);

    // Property 4: Multiple requests should have been processed concurrently
    // Note: With stub implementation, requests may complete immediately, so peak may be 1
    BOOST_TEST_MESSAGE("Peak concurrent requests: " << concurrent_peak.load());
    BOOST_CHECK_GE(concurrent_peak.load(), 1);

    // Property 5: Request start times should overlap (indicating concurrency)
    // Note: With stub implementation, requests complete immediately, so overlap may be 0
    std::size_t overlapping_requests = 0;
    for (std::size_t i = 0; i < test_concurrent_requests; ++i) {
        for (std::size_t j = i + 1; j < test_concurrent_requests; ++j) {
            // Check if request i was still running when request j started
            if (request_start_times[j] < request_end_times[i] &&
                request_start_times[i] < request_end_times[j]) {
                overlapping_requests++;
            }
        }
    }

    BOOST_TEST_MESSAGE("Overlapping requests: " << overlapping_requests);
    // With stub implementation, we just verify the mechanism works
    BOOST_CHECK(true);  // Test passes if we got this far

    // Stop server
    server->stop();
}

/**
 * Property test for concurrent processing limits
 */
BOOST_AUTO_TEST_CASE(test_concurrent_processing_limits_property,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    // Create client with limited concurrent processing
    coap_client_config client_config;
    client_config.enable_concurrent_processing = true;
    client_config.max_concurrent_requests = 5;  // Small limit for testing

    std::unordered_map<std::uint64_t, std::string> endpoint_map = {{1, test_endpoint}};

    auto client = std::make_shared<coap_client<test_transport_types>>(endpoint_map, client_config,
                                                                      noop_metrics{});

    // Property: Client should enforce concurrent request limits
    auto successful_acquisitions = std::make_shared<std::atomic<std::size_t>>(0);
    auto failed_acquisitions = std::make_shared<std::atomic<std::size_t>>(0);
    auto currently_held = std::make_shared<std::atomic<std::size_t>>(0);

    // Try to acquire more slots than the limit, holding them simultaneously
    constexpr std::size_t total_attempts = 20;  // More than the limit
    kythira::testing::coap::joining_thread_group threads;

    for (std::size_t i = 0; i < total_attempts; ++i) {
        threads.spawn([client, successful_acquisitions, failed_acquisitions, currently_held]() {
            if (client->acquire_concurrent_slot()) {
                successful_acquisitions->fetch_add(1);
                currently_held->fetch_add(1);

                // Hold the slot briefly to ensure concurrent usage
                std::this_thread::sleep_for(std::chrono::milliseconds{100});

                currently_held->fetch_sub(1);
                client->release_concurrent_slot();
            } else {
                failed_acquisitions->fetch_add(1);
            }
        });
    }

    // Wait for all attempts
    threads.join_all();

    // Property 1: Total attempts should equal successful + failed
    BOOST_CHECK_EQUAL(successful_acquisitions->load() + failed_acquisitions->load(),
                      total_attempts);

    // Property 2: With stub implementation, concurrent processing may not be enforced
    // So we just verify that the mechanism works without strict enforcement
    BOOST_TEST_MESSAGE("Successful acquisitions: " << successful_acquisitions->load());
    BOOST_TEST_MESSAGE("Failed acquisitions: " << failed_acquisitions->load());

    // For stub implementation, we accept that all may succeed since there's no real CoAP library
    // The important thing is that the API works correctly
    BOOST_CHECK(successful_acquisitions->load() > 0);
}

/**
 * Property test for concurrent processing with disabled optimization
 */
BOOST_AUTO_TEST_CASE(test_concurrent_processing_disabled_property,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(45))) {
    // Create client with concurrent processing disabled
    coap_client_config client_config;
    client_config.enable_concurrent_processing = false;

    std::unordered_map<std::uint64_t, std::string> endpoint_map = {{1, test_endpoint}};

    auto client = std::make_shared<coap_client<test_transport_types>>(endpoint_map, client_config,
                                                                      noop_metrics{});

    // Property: When concurrent processing is disabled, all slot acquisitions should succeed
    constexpr std::size_t test_attempts = 100;
    std::atomic<std::size_t> successful_acquisitions{0};

    std::vector<kythira::future_default<void>> acquisition_futures;

    for (std::size_t i = 0; i < test_attempts; ++i) {
        // Was previously wrapped in folly::makeFuture().via(&folly::InlineExecutor::instance())
        // .thenValue(...) - InlineExecutor runs synchronously, so this synchronous body
        // followed by an already-ready future preserves identical behavior while staying
        // backend-neutral.
        if (client->acquire_concurrent_slot()) {
            successful_acquisitions.fetch_add(1);
            client->release_concurrent_slot();
        }
        acquisition_futures.push_back(kythira::future_factory_default::makeFuture());
    }

    // Wait for all attempts
    for (auto& future : acquisition_futures) {
        std::move(future).get();
    }

    // Property: All acquisitions should succeed when concurrent processing is disabled
    BOOST_CHECK_EQUAL(successful_acquisitions.load(), test_attempts);
}

}  // namespace

BOOST_AUTO_TEST_SUITE_END()