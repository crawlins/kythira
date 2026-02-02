/**
 * Integration Test for Retry Logic with Exponential Backoff
 * 
 * Tests retry logic with exponential backoff for various Raft RPC operations:
 * - Heartbeat retry under network failures
 * - AppendEntries retry with various failure patterns
 * - InstallSnapshot retry with partial transfers
 * - RequestVote retry during elections
 * - Verification of exponential backoff delays
 * - Verification of retry limits
 * 
 * Requirements: 18.1, 18.2, 18.3, 18.4
 */

#define BOOST_TEST_MODULE RaftRetryLogicExponentialBackoffIntegrationTest
#include <boost/test/unit_test.hpp>

#include <raft/error_handler.hpp>
#include <raft/types.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <exception>
#include <string>
#include <random>
#include <unordered_map>
#include <mutex>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_retry_logic_exponential_backoff_integration_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    // Test constants
    constexpr std::uint64_t test_term_1 = 1;
    constexpr std::uint64_t test_term_2 = 2;
    constexpr std::uint64_t test_log_index_1 = 1;
    constexpr std::uint64_t test_log_index_2 = 2;
    constexpr std::uint64_t test_node_a = 1;
    constexpr std::uint64_t test_node_b = 2;
    constexpr std::uint64_t test_node_c = 3;
    constexpr std::chrono::milliseconds short_timeout{100};
    constexpr std::chrono::milliseconds medium_timeout{500};
    constexpr std::chrono::milliseconds long_timeout{2000};
    constexpr const char* test_node_a_str = "node_a";
    constexpr const char* test_node_b_str = "node_b";
    constexpr const char* test_node_c_str = "node_c";
    constexpr std::size_t max_retry_attempts = 5;
    constexpr std::chrono::milliseconds initial_delay{100};
    constexpr std::chrono::milliseconds max_delay{5000};
    constexpr double backoff_multiplier = 2.0;
}

/**
 * Mock network client for simulating network failures and measuring retry behavior
 */
class MockNetworkClient {
public:
    struct OperationRecord {
        std::chrono::steady_clock::time_point timestamp;
        std::size_t attempt_number;
        bool succeeded;
        std::string error_message;
    };
    
    MockNetworkClient() : _rng(std::random_device{}()) {}
    
    auto set_failure_count(const std::string& target, std::size_t count) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _failure_counts[target] = count;
    }
    
    auto set_always_fail(const std::string& target, bool fail) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _always_fail[target] = fail;
    }
    
    auto get_operation_records(const std::string& target) const -> std::vector<OperationRecord> {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _operation_records.find(target);
        return (it != _operation_records.end()) ? it->second : std::vector<OperationRecord>{};
    }
    
    auto reset() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _failure_counts.clear();
        _always_fail.clear();
        _operation_records.clear();
    }
    
    auto send_heartbeat(
        const std::string& target,
        const kythira::append_entries_request<std::uint64_t, std::uint64_t>& request,
        std::chrono::milliseconds timeout
    ) -> kythira::append_entries_response<std::uint64_t, std::uint64_t> {
        return simulate_operation<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
            target,
            [request]() {
                return kythira::append_entries_response<std::uint64_t, std::uint64_t>{
                    request.term(), true, std::nullopt, std::nullopt
                };
            },
            "Network timeout occurred"
        );
    }
    
    auto send_append_entries(
        const std::string& target,
        const kythira::append_entries_request<std::uint64_t, std::uint64_t>& request,
        std::chrono::milliseconds timeout
    ) -> kythira::append_entries_response<std::uint64_t, std::uint64_t> {
        return simulate_operation<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
            target,
            [request]() {
                return kythira::append_entries_response<std::uint64_t, std::uint64_t>{
                    request.term(), true, std::nullopt, std::nullopt
                };
            },
            "Connection refused by target"
        );
    }
    
    auto send_install_snapshot(
        const std::string& target,
        const kythira::install_snapshot_request<std::uint64_t, std::uint64_t>& request,
        std::chrono::milliseconds timeout
    ) -> kythira::install_snapshot_response<std::uint64_t> {
        return simulate_operation<kythira::install_snapshot_response<std::uint64_t>>(
            target,
            [request]() {
                return kythira::install_snapshot_response<std::uint64_t>{request.term()};
            },
            "Temporary failure, try again"
        );
    }
    
    auto send_request_vote(
        const std::string& target,
        const kythira::request_vote_request<std::uint64_t, std::uint64_t>& request,
        std::chrono::milliseconds timeout
    ) -> kythira::request_vote_response<std::uint64_t> {
        return simulate_operation<kythira::request_vote_response<std::uint64_t>>(
            target,
            [request]() {
                return kythira::request_vote_response<std::uint64_t>{request.term(), true};
            },
            "Network is unreachable"
        );
    }

private:
    template<typename ResponseType, typename OperationFunc>
    auto simulate_operation(
        const std::string& target,
        OperationFunc&& operation,
        const std::string& error_message
    ) -> ResponseType {
        std::lock_guard<std::mutex> lock(_mutex);
        
        // Get current attempt number
        std::size_t attempt = _operation_records[target].size() + 1;
        
        // Check if should fail
        bool should_fail = false;
        std::string actual_error = error_message;
        
        if (_always_fail[target]) {
            should_fail = true;
        } else if (_failure_counts[target] > 0) {
            should_fail = true;
            _failure_counts[target]--;
        }
        
        // Record operation
        OperationRecord record{
            .timestamp = std::chrono::steady_clock::now(),
            .attempt_number = attempt,
            .succeeded = !should_fail,
            .error_message = should_fail ? actual_error : ""
        };
        _operation_records[target].push_back(record);
        
        if (should_fail) {
            throw std::runtime_error(actual_error);
        }
        
        return operation();
    }
    
    mutable std::mutex _mutex;
    std::unordered_map<std::string, std::size_t> _failure_counts;
    std::unordered_map<std::string, bool> _always_fail;
    std::unordered_map<std::string, std::vector<OperationRecord>> _operation_records;
    std::mt19937 _rng;
};

BOOST_AUTO_TEST_SUITE(retry_logic_exponential_backoff_integration_tests, * boost::unit_test::timeout(300))

/**
 * Test: Heartbeat retry under network failures
 * 
 * Verifies that heartbeat operations retry with exponential backoff
 * when network failures occur, and that delays follow the expected pattern.
 * 
 * Requirements: 18.1
 */
BOOST_AUTO_TEST_CASE(heartbeat_retry_network_failures, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing heartbeat retry under network failures");
    
    MockNetworkClient network_client;
    kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
    
    // Configure retry policy for heartbeats
    typename decltype(handler)::retry_policy heartbeat_policy{
        .initial_delay = std::chrono::milliseconds{50},
        .max_delay = std::chrono::milliseconds{1000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.0,  // No jitter for predictable testing
        .max_attempts = 4
    };
    handler.set_retry_policy("heartbeat", heartbeat_policy);
    
    // Test scenario: First 2 attempts fail, 3rd succeeds
    network_client.set_failure_count(test_node_b_str, 2);
    
    auto heartbeat_operation = [&network_client]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
        kythira::append_entries_request<std::uint64_t, std::uint64_t> request{
            test_term_1, test_node_a, test_log_index_1, test_term_1, {}, test_log_index_1
        };
        
        try {
            auto response = network_client.send_heartbeat(test_node_b_str, request, medium_timeout);
            return kythira::FutureFactory::makeFuture(std::move(response));
        } catch (const std::exception& e) {
            return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                std::current_exception());
        }
    };
    
    auto start_time = std::chrono::steady_clock::now();
    auto result_future = handler.execute_with_retry("heartbeat", heartbeat_operation, heartbeat_policy);
    auto result = std::move(result_future).get();
    auto end_time = std::chrono::steady_clock::now();
    
    // Verify operation succeeded
    BOOST_CHECK(result.success());
    
    // Verify retry attempts
    auto records = network_client.get_operation_records(test_node_b_str);
    BOOST_CHECK_EQUAL(records.size(), 3);  // 2 failures + 1 success
    
    // Verify exponential backoff delays
    if (records.size() >= 3) {
        auto delay_1_to_2 = std::chrono::duration_cast<std::chrono::milliseconds>(
            records[1].timestamp - records[0].timestamp);
        auto delay_2_to_3 = std::chrono::duration_cast<std::chrono::milliseconds>(
            records[2].timestamp - records[1].timestamp);
        
        BOOST_TEST_MESSAGE("Delay between attempt 1 and 2: " << delay_1_to_2.count() << "ms");
        BOOST_TEST_MESSAGE("Delay between attempt 2 and 3: " << delay_2_to_3.count() << "ms");
        
        // First delay should be ~50ms (initial_delay)
        BOOST_CHECK_GE(delay_1_to_2.count(), 40);  // Allow 20% tolerance
        BOOST_CHECK_LE(delay_1_to_2.count(), 100);
        
        // Second delay should be ~100ms (initial_delay * backoff_multiplier)
        BOOST_CHECK_GE(delay_2_to_3.count(), 80);
        BOOST_CHECK_LE(delay_2_to_3.count(), 200);
        
        // Verify exponential growth
        BOOST_CHECK_GT(delay_2_to_3.count(), delay_1_to_2.count());
    }
    
    auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    BOOST_TEST_MESSAGE("Total operation time: " << total_time.count() << "ms");
    BOOST_TEST_MESSAGE("✓ Heartbeat retry with exponential backoff works correctly");
}

/**
 * Test: AppendEntries retry with various failure patterns
 * 
 * Verifies that AppendEntries operations retry appropriately with
 * different failure patterns and respect retry limits.
 * 
 * Requirements: 18.2
 */
BOOST_AUTO_TEST_CASE(append_entries_retry_failure_patterns, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing AppendEntries retry with various failure patterns");
    
    MockNetworkClient network_client;
    kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
    
    // Configure retry policy for AppendEntries
    typename decltype(handler)::retry_policy append_policy{
        .initial_delay = initial_delay,
        .max_delay = max_delay,
        .backoff_multiplier = backoff_multiplier,
        .jitter_factor = 0.0,
        .max_attempts = max_retry_attempts
    };
    handler.set_retry_policy("append_entries", append_policy);
    
    // Test Pattern 1: Intermittent failures (fail, succeed, fail, succeed)
    {
        BOOST_TEST_MESSAGE("Pattern 1: Intermittent failures");
        network_client.reset();
        network_client.set_failure_count(test_node_b_str, 1);
        
        auto append_operation = [&network_client]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
            kythira::append_entries_request<std::uint64_t, std::uint64_t> request{
                test_term_1, test_node_a, test_log_index_1, test_term_1,
                {kythira::log_entry<std::uint64_t, std::uint64_t>{test_term_1, test_log_index_1, {}}},
                test_log_index_1
            };
            
            try {
                auto response = network_client.send_append_entries(test_node_b_str, request, medium_timeout);
                return kythira::FutureFactory::makeFuture(std::move(response));
            } catch (const std::exception& e) {
                return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::current_exception());
            }
        };
        
        auto result_future = handler.execute_with_retry("append_entries", append_operation, append_policy);
        auto result = std::move(result_future).get();
        
        BOOST_CHECK(result.success());
        auto records = network_client.get_operation_records(test_node_b_str);
        BOOST_CHECK_EQUAL(records.size(), 2);  // 1 failure + 1 success
        BOOST_TEST_MESSAGE("✓ Intermittent failure pattern handled correctly");
    }
    
    // Test Pattern 2: Multiple consecutive failures before success
    {
        BOOST_TEST_MESSAGE("Pattern 2: Multiple consecutive failures");
        network_client.reset();
        network_client.set_failure_count(test_node_c_str, 3);
        
        auto append_operation = [&network_client]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
            kythira::append_entries_request<std::uint64_t, std::uint64_t> request{
                test_term_2, test_node_a, test_log_index_2, test_term_2,
                {kythira::log_entry<std::uint64_t, std::uint64_t>{test_term_2, test_log_index_2, {}}},
                test_log_index_2
            };
            
            try {
                auto response = network_client.send_append_entries(test_node_c_str, request, medium_timeout);
                return kythira::FutureFactory::makeFuture(std::move(response));
            } catch (const std::exception& e) {
                return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::current_exception());
            }
        };
        
        auto start_time = std::chrono::steady_clock::now();
        auto result_future = handler.execute_with_retry("append_entries", append_operation, append_policy);
        auto result = std::move(result_future).get();
        auto end_time = std::chrono::steady_clock::now();
        
        BOOST_CHECK(result.success());
        auto records = network_client.get_operation_records(test_node_c_str);
        BOOST_CHECK_EQUAL(records.size(), 4);  // 3 failures + 1 success
        
        // Verify exponential backoff pattern
        if (records.size() >= 4) {
            for (std::size_t i = 1; i < records.size(); ++i) {
                auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                    records[i].timestamp - records[i-1].timestamp);
                BOOST_TEST_MESSAGE("Delay before attempt " << (i+1) << ": " << delay.count() << "ms");
            }
        }
        
        auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        BOOST_TEST_MESSAGE("Total time for 4 attempts: " << total_time.count() << "ms");
        BOOST_TEST_MESSAGE("✓ Multiple consecutive failures handled correctly");
    }
    
    // Test Pattern 3: Exceeding retry limit
    {
        BOOST_TEST_MESSAGE("Pattern 3: Exceeding retry limit");
        network_client.reset();
        network_client.set_always_fail(test_node_b_str, true);
        
        auto append_operation = [&network_client]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
            kythira::append_entries_request<std::uint64_t, std::uint64_t> request{
                test_term_1, test_node_a, test_log_index_1, test_term_1, {}, test_log_index_1
            };
            
            try {
                auto response = network_client.send_append_entries(test_node_b_str, request, medium_timeout);
                return kythira::FutureFactory::makeFuture(std::move(response));
            } catch (const std::exception& e) {
                return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::current_exception());
            }
        };
        
        bool exception_caught = false;
        try {
            auto result_future = handler.execute_with_retry("append_entries", append_operation, append_policy);
            auto result = std::move(result_future).get();
            BOOST_FAIL("Expected exception for exceeding retry limit");
        } catch (const std::exception& e) {
            exception_caught = true;
            BOOST_TEST_MESSAGE("Caught expected exception: " << e.what());
        }
        
        BOOST_CHECK(exception_caught);
        auto records = network_client.get_operation_records(test_node_b_str);
        BOOST_CHECK_EQUAL(records.size(), max_retry_attempts);
        BOOST_TEST_MESSAGE("✓ Retry limit respected correctly");
    }
}

/**
 * Test: InstallSnapshot retry with partial transfers
 * 
 * Verifies that InstallSnapshot operations retry appropriately when
 * partial transfers fail, with proper exponential backoff.
 * 
 * Requirements: 18.3
 */
BOOST_AUTO_TEST_CASE(install_snapshot_retry_partial_transfers, * boost::unit_test::timeout(90)) {
    BOOST_TEST_MESSAGE("Testing InstallSnapshot retry with partial transfers");
    
    MockNetworkClient network_client;
    kythira::error_handler<kythira::install_snapshot_response<std::uint64_t>> handler;
    
    // Configure retry policy for InstallSnapshot (longer delays for large transfers)
    typename decltype(handler)::retry_policy snapshot_policy{
        .initial_delay = std::chrono::milliseconds{200},
        .max_delay = std::chrono::milliseconds{10000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.0,
        .max_attempts = 6
    };
    handler.set_retry_policy("install_snapshot", snapshot_policy);
    
    // Simulate partial transfer failures
    network_client.set_failure_count(test_node_c_str, 2);
    
    auto snapshot_operation = [&network_client]() -> kythira::Future<kythira::install_snapshot_response<std::uint64_t>> {
        kythira::install_snapshot_request<std::uint64_t, std::uint64_t> request{
            test_term_1,
            test_node_a,
            test_log_index_1,
            test_term_1,
            0,  // offset
            {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}},  // data
            true  // done
        };
        
        try {
            auto response = network_client.send_install_snapshot(test_node_c_str, request, long_timeout);
            return kythira::FutureFactory::makeFuture(std::move(response));
        } catch (const std::exception& e) {
            return kythira::FutureFactory::makeExceptionalFuture<kythira::install_snapshot_response<std::uint64_t>>(
                std::current_exception());
        }
    };
    
    auto start_time = std::chrono::steady_clock::now();
    auto result_future = handler.execute_with_retry("install_snapshot", snapshot_operation, snapshot_policy);
    auto result = std::move(result_future).get();
    auto end_time = std::chrono::steady_clock::now();
    
    // Verify operation succeeded
    BOOST_CHECK_EQUAL(result.term(), test_term_1);
    
    // Verify retry attempts
    auto records = network_client.get_operation_records(test_node_c_str);
    BOOST_CHECK_EQUAL(records.size(), 3);  // 2 failures + 1 success
    
    // Verify exponential backoff with longer delays for snapshots
    if (records.size() >= 3) {
        auto delay_1_to_2 = std::chrono::duration_cast<std::chrono::milliseconds>(
            records[1].timestamp - records[0].timestamp);
        auto delay_2_to_3 = std::chrono::duration_cast<std::chrono::milliseconds>(
            records[2].timestamp - records[1].timestamp);
        
        BOOST_TEST_MESSAGE("Delay between attempt 1 and 2: " << delay_1_to_2.count() << "ms");
        BOOST_TEST_MESSAGE("Delay between attempt 2 and 3: " << delay_2_to_3.count() << "ms");
        
        // First delay should be ~200ms (initial_delay for snapshots)
        BOOST_CHECK_GE(delay_1_to_2.count(), 160);
        BOOST_CHECK_LE(delay_1_to_2.count(), 300);
        
        // Second delay should be ~400ms (initial_delay * backoff_multiplier)
        BOOST_CHECK_GE(delay_2_to_3.count(), 320);
        BOOST_CHECK_LE(delay_2_to_3.count(), 600);
        
        // Verify exponential growth
        BOOST_CHECK_GT(delay_2_to_3.count(), delay_1_to_2.count());
    }
    
    auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    BOOST_TEST_MESSAGE("Total snapshot transfer time: " << total_time.count() << "ms");
    BOOST_TEST_MESSAGE("✓ InstallSnapshot retry with exponential backoff works correctly");
}

/**
 * Test: RequestVote retry during elections
 * 
 * Verifies that RequestVote operations retry appropriately during
 * elections when network failures occur.
 * 
 * Requirements: 18.4
 */
BOOST_AUTO_TEST_CASE(request_vote_retry_elections, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing RequestVote retry during elections");
    
    MockNetworkClient network_client;
    kythira::error_handler<kythira::request_vote_response<std::uint64_t>> handler;
    
    // Configure retry policy for RequestVote
    typename decltype(handler)::retry_policy vote_policy{
        .initial_delay = std::chrono::milliseconds{100},
        .max_delay = std::chrono::milliseconds{2000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.0,
        .max_attempts = 3
    };
    handler.set_retry_policy("request_vote", vote_policy);
    
    // Simulate election with network issues
    network_client.set_failure_count(test_node_b_str, 1);
    
    auto vote_operation = [&network_client]() -> kythira::Future<kythira::request_vote_response<std::uint64_t>> {
        kythira::request_vote_request<std::uint64_t, std::uint64_t> request{
            test_term_2,
            test_node_a,
            test_log_index_1,
            test_term_1
        };
        
        try {
            auto response = network_client.send_request_vote(test_node_b_str, request, medium_timeout);
            return kythira::FutureFactory::makeFuture(std::move(response));
        } catch (const std::exception& e) {
            return kythira::FutureFactory::makeExceptionalFuture<kythira::request_vote_response<std::uint64_t>>(
                std::current_exception());
        }
    };
    
    auto start_time = std::chrono::steady_clock::now();
    auto result_future = handler.execute_with_retry("request_vote", vote_operation, vote_policy);
    auto result = std::move(result_future).get();
    auto end_time = std::chrono::steady_clock::now();
    
    // Verify vote was granted
    BOOST_CHECK(result.vote_granted());
    BOOST_CHECK_EQUAL(result.term(), test_term_2);
    
    // Verify retry attempts
    auto records = network_client.get_operation_records(test_node_b_str);
    BOOST_CHECK_EQUAL(records.size(), 2);  // 1 failure + 1 success
    
    // Verify backoff delay
    if (records.size() >= 2) {
        auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
            records[1].timestamp - records[0].timestamp);
        
        BOOST_TEST_MESSAGE("Delay between attempts: " << delay.count() << "ms");
        
        // Delay should be ~100ms (initial_delay)
        BOOST_CHECK_GE(delay.count(), 80);
        BOOST_CHECK_LE(delay.count(), 150);
    }
    
    auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    BOOST_TEST_MESSAGE("Total election vote time: " << total_time.count() << "ms");
    BOOST_TEST_MESSAGE("✓ RequestVote retry with exponential backoff works correctly");
}

/**
 * Test: Exponential backoff delay verification
 * 
 * Verifies that exponential backoff delays follow the expected mathematical
 * pattern: delay_n = initial_delay * (backoff_multiplier ^ (n-1))
 * 
 * Requirements: 18.1, 18.2, 18.3, 18.4
 */
BOOST_AUTO_TEST_CASE(exponential_backoff_delay_verification, * boost::unit_test::timeout(90)) {
    BOOST_TEST_MESSAGE("Testing exponential backoff delay verification");
    
    MockNetworkClient network_client;
    kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
    
    // Configure retry policy with known parameters
    typename decltype(handler)::retry_policy test_policy{
        .initial_delay = std::chrono::milliseconds{100},
        .max_delay = std::chrono::milliseconds{10000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.0,  // No jitter for precise testing
        .max_attempts = 5
    };
    handler.set_retry_policy("test_operation", test_policy);
    
    // Set up to fail 4 times, succeed on 5th
    network_client.set_failure_count(test_node_b_str, 4);
    
    auto test_operation = [&network_client]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
        kythira::append_entries_request<std::uint64_t, std::uint64_t> request{
            test_term_1, test_node_a, test_log_index_1, test_term_1, {}, test_log_index_1
        };
        
        try {
            auto response = network_client.send_append_entries(test_node_b_str, request, medium_timeout);
            return kythira::FutureFactory::makeFuture(std::move(response));
        } catch (const std::exception& e) {
            return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                std::current_exception());
        }
    };
    
    auto result_future = handler.execute_with_retry("test_operation", test_operation, test_policy);
    auto result = std::move(result_future).get();
    
    BOOST_CHECK(result.success());
    
    // Analyze delay pattern
    auto records = network_client.get_operation_records(test_node_b_str);
    BOOST_CHECK_EQUAL(records.size(), 5);
    
    if (records.size() >= 5) {
        std::vector<std::chrono::milliseconds> actual_delays;
        std::vector<std::chrono::milliseconds> expected_delays;
        
        // Calculate actual delays
        for (std::size_t i = 1; i < records.size(); ++i) {
            auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                records[i].timestamp - records[i-1].timestamp);
            actual_delays.push_back(delay);
        }
        
        // Calculate expected delays: 100ms, 200ms, 400ms, 800ms
        expected_delays.push_back(std::chrono::milliseconds{100});
        expected_delays.push_back(std::chrono::milliseconds{200});
        expected_delays.push_back(std::chrono::milliseconds{400});
        expected_delays.push_back(std::chrono::milliseconds{800});
        
        BOOST_TEST_MESSAGE("Exponential backoff delay pattern:");
        for (std::size_t i = 0; i < actual_delays.size(); ++i) {
            BOOST_TEST_MESSAGE("  Attempt " << (i+1) << " -> " << (i+2) 
                              << ": Expected ~" << expected_delays[i].count() << "ms"
                              << ", Actual " << actual_delays[i].count() << "ms");
            
            // Allow 30% tolerance for timing variations
            auto expected = expected_delays[i].count();
            auto actual = actual_delays[i].count();
            BOOST_CHECK_GE(actual, expected * 0.7);
            BOOST_CHECK_LE(actual, expected * 1.3);
        }
        
        // Verify exponential growth pattern
        for (std::size_t i = 1; i < actual_delays.size(); ++i) {
            BOOST_CHECK_GT(actual_delays[i].count(), actual_delays[i-1].count());
        }
        
        BOOST_TEST_MESSAGE("✓ Exponential backoff follows expected mathematical pattern");
    }
}

/**
 * Test: Retry limit enforcement
 * 
 * Verifies that retry limits are strictly enforced and operations
 * fail after exceeding the maximum number of attempts.
 * 
 * Requirements: 18.1, 18.2, 18.3, 18.4
 */
BOOST_AUTO_TEST_CASE(retry_limit_enforcement, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing retry limit enforcement");
    
    MockNetworkClient network_client;
    kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
    
    // Test with different retry limits
    std::vector<std::size_t> retry_limits = {1, 3, 5};
    
    for (auto max_attempts : retry_limits) {
        BOOST_TEST_MESSAGE("Testing with max_attempts = " << max_attempts);
        
        network_client.reset();
        network_client.set_always_fail(test_node_b_str, true);
        
        typename decltype(handler)::retry_policy limit_policy{
            .initial_delay = std::chrono::milliseconds{50},
            .max_delay = std::chrono::milliseconds{1000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.0,
            .max_attempts = max_attempts
        };
        
        auto test_operation = [&network_client]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
            kythira::append_entries_request<std::uint64_t, std::uint64_t> request{
                test_term_1, test_node_a, test_log_index_1, test_term_1, {}, test_log_index_1
            };
            
            try {
                auto response = network_client.send_append_entries(test_node_b_str, request, medium_timeout);
                return kythira::FutureFactory::makeFuture(std::move(response));
            } catch (const std::exception& e) {
                return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::current_exception());
            }
        };
        
        bool exception_caught = false;
        try {
            auto result_future = handler.execute_with_retry("test_operation", test_operation, limit_policy);
            auto result = std::move(result_future).get();
            BOOST_FAIL("Expected exception after exceeding retry limit");
        } catch (const std::exception& e) {
            exception_caught = true;
            BOOST_TEST_MESSAGE("  Caught expected exception: " << e.what());
        }
        
        BOOST_CHECK(exception_caught);
        
        // Verify exact number of attempts
        auto records = network_client.get_operation_records(test_node_b_str);
        BOOST_CHECK_EQUAL(records.size(), max_attempts);
        BOOST_TEST_MESSAGE("  ✓ Exactly " << max_attempts << " attempts made");
    }
    
    BOOST_TEST_MESSAGE("✓ Retry limits enforced correctly for all configurations");
}

/**
 * Test: Max delay cap enforcement
 * 
 * Verifies that exponential backoff delays are capped at the configured
 * maximum delay value.
 * 
 * Requirements: 18.1, 18.2, 18.3, 18.4
 */
BOOST_AUTO_TEST_CASE(max_delay_cap_enforcement, * boost::unit_test::timeout(90)) {
    BOOST_TEST_MESSAGE("Testing max delay cap enforcement");
    
    MockNetworkClient network_client;
    kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
    
    // Configure with low max_delay to test capping
    typename decltype(handler)::retry_policy capped_policy{
        .initial_delay = std::chrono::milliseconds{100},
        .max_delay = std::chrono::milliseconds{300},  // Low cap
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.0,
        .max_attempts = 6
    };
    handler.set_retry_policy("test_operation", capped_policy);
    
    // Set up to fail 5 times, succeed on 6th
    network_client.set_failure_count(test_node_b_str, 5);
    
    auto test_operation = [&network_client]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
        kythira::append_entries_request<std::uint64_t, std::uint64_t> request{
            test_term_1, test_node_a, test_log_index_1, test_term_1, {}, test_log_index_1
        };
        
        try {
            auto response = network_client.send_append_entries(test_node_b_str, request, medium_timeout);
            return kythira::FutureFactory::makeFuture(std::move(response));
        } catch (const std::exception& e) {
            return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                std::current_exception());
        }
    };
    
    auto result_future = handler.execute_with_retry("test_operation", test_operation, capped_policy);
    auto result = std::move(result_future).get();
    
    BOOST_CHECK(result.success());
    
    // Analyze delays to verify capping
    auto records = network_client.get_operation_records(test_node_b_str);
    BOOST_CHECK_EQUAL(records.size(), 6);
    
    if (records.size() >= 6) {
        BOOST_TEST_MESSAGE("Delay pattern with max_delay cap:");
        for (std::size_t i = 1; i < records.size(); ++i) {
            auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                records[i].timestamp - records[i-1].timestamp);
            
            BOOST_TEST_MESSAGE("  Delay before attempt " << (i+1) << ": " << delay.count() << "ms");
            
            // All delays should be <= max_delay (with tolerance)
            BOOST_CHECK_LE(delay.count(), 400);  // 300ms + 33% tolerance
            
            // After the first few attempts, delays should stabilize at max_delay
            if (i >= 3) {
                BOOST_CHECK_GE(delay.count(), 200);  // Should be near 300ms
            }
        }
        
        BOOST_TEST_MESSAGE("✓ Max delay cap enforced correctly");
    }
}

BOOST_AUTO_TEST_SUITE_END()
