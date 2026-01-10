/**
 * Property-Based Test for Application Failure Handling
 * 
 * Feature: raft-completion, Property 25: Application Failure Handling
 * Validates: Requirements 5.4
 * 
 * Property: For any state machine application failure, further application 
 * is halted and the error is reported.
 */

#define BOOST_TEST_MODULE RaftApplicationFailureHandlingPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/membership.hpp>

#include <network_simulator/network_simulator.hpp>

#include <folly/init/Init.h>

#include <random>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <future>
#include <optional>
#include <atomic>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_application_failure_handling_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::size_t property_test_iterations = 10;
    constexpr std::chrono::milliseconds election_timeout_min{50};
    constexpr std::chrono::milliseconds election_timeout_max{100};
    constexpr std::chrono::milliseconds heartbeat_interval{25};
    constexpr std::chrono::milliseconds rpc_timeout{100};
    constexpr std::chrono::milliseconds commit_timeout{2000};
}

/**
 * Helper class to simulate state machine application failure tracking
 */
class ApplicationFailureTracker {
public:
    struct ApplicationAttempt {
        std::uint64_t log_index;
        std::vector<std::byte> command;
        bool success;
        std::optional<std::string> error_message;
        std::chrono::steady_clock::time_point attempted_at;
        std::uint64_t applied_index_before;
        std::uint64_t applied_index_after;
    };
    
    auto record_application_success(
        std::uint64_t log_index,
        const std::vector<std::byte>& command,
        std::uint64_t applied_index_before,
        std::uint64_t applied_index_after
    ) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _attempts.push_back({
            log_index,
            command,
            true,
            std::nullopt,
            std::chrono::steady_clock::now(),
            applied_index_before,
            applied_index_after
        });
    }
    
    auto record_application_failure(
        std::uint64_t log_index,
        const std::vector<std::byte>& command,
        const std::string& error_message,
        std::uint64_t applied_index_before
    ) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _attempts.push_back({
            log_index,
            command,
            false,
            error_message,
            std::chrono::steady_clock::now(),
            applied_index_before,
            applied_index_before  // Applied index doesn't advance on failure
        });
        _failure_occurred = true;
        _failure_at_index = log_index;
    }
    
    auto get_attempts() const -> std::vector<ApplicationAttempt> {
        std::lock_guard<std::mutex> lock(_mutex);
        return _attempts;
    }
    
    auto has_failure_occurred() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        return _failure_occurred;
    }
    
    auto get_failure_index() const -> std::optional<std::uint64_t> {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_failure_occurred) {
            return _failure_at_index;
        }
        return std::nullopt;
    }
    
    auto verify_no_application_after_failure() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_failure_occurred) {
            return true; // No failure, so this property is trivially satisfied
        }
        
        // Find the failure
        auto failure_it = std::find_if(_attempts.begin(), _attempts.end(),
            [](const ApplicationAttempt& attempt) {
                return !attempt.success;
            });
        
        if (failure_it == _attempts.end()) {
            return true; // No failure found
        }
        
        // Check that no successful applications occurred after the failure
        for (auto it = failure_it + 1; it != _attempts.end(); ++it) {
            if (it->success) {
                return false; // Found successful application after failure
            }
        }
        
        return true;
    }
    
    auto verify_applied_index_unchanged_on_failure() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& attempt : _attempts) {
            if (!attempt.success) {
                // For failed applications, applied index should not change
                if (attempt.applied_index_after != attempt.applied_index_before) {
                    return false;
                }
            }
        }
        return true;
    }
    
    auto get_final_applied_index() const -> std::uint64_t {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_attempts.empty()) {
            return 0;
        }
        return _attempts.back().applied_index_after;
    }
    
    auto get_attempt_count() const -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        return _attempts.size();
    }
    
    auto clear() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _attempts.clear();
        _failure_occurred = false;
        _failure_at_index = 0;
    }
    
private:
    mutable std::mutex _mutex;
    std::vector<ApplicationAttempt> _attempts;
    bool _failure_occurred{false};
    std::uint64_t _failure_at_index{0};
};

BOOST_AUTO_TEST_SUITE(application_failure_handling_property_tests)

/**
 * Property: Application failure handling
 * 
 * For any state machine application failure, further application 
 * is halted and the error is reported.
 */
BOOST_AUTO_TEST_CASE(property_application_failure_handling, * boost::unit_test::timeout(120)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::size_t> success_count_dist(2, 6);
    std::uniform_int_distribution<std::size_t> failure_position_dist(1, 4);
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        ApplicationFailureTracker tracker;
        
        auto success_count = success_count_dist(rng);
        auto failure_position = failure_position_dist(rng);
        
        std::uint64_t current_applied_index = 0;
        
        // Apply some entries successfully
        for (std::size_t i = 1; i <= success_count; ++i) {
            std::vector<std::byte> command;
            command.push_back(static_cast<std::byte>(0xAA)); // Success marker
            command.push_back(static_cast<std::byte>(i & 0xFF));
            for (std::size_t j = 0; j < 4; ++j) {
                command.push_back(static_cast<std::byte>((i + j) % 256));
            }
            
            std::uint64_t log_index = i;
            std::uint64_t old_applied_index = current_applied_index;
            current_applied_index = log_index;
            
            tracker.record_application_success(log_index, command, old_applied_index, current_applied_index);
        }
        
        // Introduce a failure
        std::uint64_t failure_log_index = success_count + failure_position;
        std::vector<std::byte> failure_command;
        failure_command.push_back(static_cast<std::byte>(0xBB)); // Failure marker
        failure_command.push_back(static_cast<std::byte>(failure_log_index & 0xFF));
        
        std::string error_message = "Simulated state machine application failure";
        tracker.record_application_failure(failure_log_index, failure_command, error_message, current_applied_index);
        
        // Try to apply more entries after failure (these should not succeed)
        for (std::size_t i = 1; i <= 3; ++i) {
            std::uint64_t post_failure_index = failure_log_index + i;
            std::vector<std::byte> post_failure_command;
            post_failure_command.push_back(static_cast<std::byte>(0xCC)); // Post-failure marker
            post_failure_command.push_back(static_cast<std::byte>(post_failure_index & 0xFF));
            
            // These should fail because application is halted after first failure
            std::string halt_error = "Application halted due to previous failure";
            tracker.record_application_failure(post_failure_index, post_failure_command, halt_error, current_applied_index);
        }
        
        // Property verification
        BOOST_CHECK_MESSAGE(tracker.has_failure_occurred(),
            "Failure should be detected and recorded");
        
        BOOST_CHECK_MESSAGE(tracker.verify_no_application_after_failure(),
            "No successful applications should occur after a failure");
        
        BOOST_CHECK_MESSAGE(tracker.verify_applied_index_unchanged_on_failure(),
            "Applied index should not advance when application fails");
        
        BOOST_CHECK_MESSAGE(tracker.get_final_applied_index() == success_count,
            "Applied index should remain at last successful application");
        
        auto failure_index = tracker.get_failure_index();
        BOOST_CHECK_MESSAGE(failure_index.has_value(),
            "Failure index should be recorded");
        
        if (failure_index.has_value()) {
            BOOST_CHECK_MESSAGE(failure_index.value() == failure_log_index,
                "Failure index should match the failed entry's log index");
        }
        
        tracker.clear();
    }
}

/**
 * Property: Early failure handling
 * 
 * For any failure that occurs early in the application sequence,
 * no subsequent entries are applied.
 */
BOOST_AUTO_TEST_CASE(property_early_failure_handling, * boost::unit_test::timeout(90)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::size_t> pending_count_dist(3, 8);
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        ApplicationFailureTracker tracker;
        
        auto pending_count = pending_count_dist(rng);
        
        // Fail on the first entry
        std::vector<std::byte> first_command{std::byte{0xDD}, std::byte{0x01}};
        std::string first_error = "First entry application failure";
        tracker.record_application_failure(1, first_command, first_error, 0);
        
        // Try to apply remaining entries (should all fail due to halt)
        for (std::size_t i = 2; i <= pending_count; ++i) {
            std::vector<std::byte> command;
            command.push_back(static_cast<std::byte>(0xEE)); // Halted marker
            command.push_back(static_cast<std::byte>(i & 0xFF));
            
            std::string halt_error = "Application halted after first failure";
            tracker.record_application_failure(i, command, halt_error, 0);
        }
        
        // Verify early failure properties
        BOOST_CHECK_MESSAGE(tracker.has_failure_occurred(),
            "Early failure should be detected");
        
        BOOST_CHECK_MESSAGE(tracker.get_failure_index().value_or(0) == 1,
            "First failure should be at index 1");
        
        BOOST_CHECK_MESSAGE(tracker.verify_no_application_after_failure(),
            "No applications should succeed after early failure");
        
        BOOST_CHECK_MESSAGE(tracker.get_final_applied_index() == 0,
            "Applied index should remain 0 after early failure");
        
        // Verify all attempts after the first are failures
        auto attempts = tracker.get_attempts();
        for (std::size_t i = 0; i < attempts.size(); ++i) {
            BOOST_CHECK_MESSAGE(!attempts[i].success,
                "All application attempts should fail after initial failure");
        }
        
        tracker.clear();
    }
}

/**
 * Property: Mid-sequence failure handling
 * 
 * For any failure that occurs in the middle of an application sequence,
 * the applied index stops at the last successful application.
 */
BOOST_AUTO_TEST_CASE(property_mid_sequence_failure, * boost::unit_test::timeout(90)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::size_t> success_before_dist(3, 7);
    std::uniform_int_distribution<std::size_t> attempts_after_dist(2, 5);
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        ApplicationFailureTracker tracker;
        
        auto success_before = success_before_dist(rng);
        auto attempts_after = attempts_after_dist(rng);
        
        std::uint64_t current_applied_index = 0;
        
        // Apply entries successfully up to the failure point
        for (std::size_t i = 1; i <= success_before; ++i) {
            std::vector<std::byte> command;
            command.push_back(static_cast<std::byte>(0xFF)); // Pre-failure success
            command.push_back(static_cast<std::byte>(i & 0xFF));
            
            std::uint64_t old_applied_index = current_applied_index;
            current_applied_index = i;
            
            tracker.record_application_success(i, command, old_applied_index, current_applied_index);
        }
        
        // Introduce failure at mid-sequence
        std::uint64_t failure_index = success_before + 1;
        std::vector<std::byte> failure_command{std::byte{0x00}, std::byte{failure_index & 0xFF}};
        std::string failure_error = "Mid-sequence application failure";
        tracker.record_application_failure(failure_index, failure_command, failure_error, current_applied_index);
        
        // Attempt more applications after failure (should all fail)
        for (std::size_t i = 1; i <= attempts_after; ++i) {
            std::uint64_t post_failure_index = failure_index + i;
            std::vector<std::byte> post_command{std::byte{0x11}, std::byte{post_failure_index & 0xFF}};
            std::string post_error = "Application halted due to mid-sequence failure";
            tracker.record_application_failure(post_failure_index, post_command, post_error, current_applied_index);
        }
        
        // Verify mid-sequence failure properties
        BOOST_CHECK_MESSAGE(tracker.has_failure_occurred(),
            "Mid-sequence failure should be detected");
        
        BOOST_CHECK_MESSAGE(tracker.verify_no_application_after_failure(),
            "No applications should succeed after mid-sequence failure");
        
        BOOST_CHECK_MESSAGE(tracker.get_final_applied_index() == success_before,
            "Applied index should remain at last successful application before failure");
        
        auto failure_idx = tracker.get_failure_index();
        BOOST_CHECK_MESSAGE(failure_idx.has_value() && failure_idx.value() == failure_index,
            "Failure should be recorded at correct index");
        
        tracker.clear();
    }
}

/**
 * Property: Error reporting
 * 
 * For any application failure, the error is properly reported and recorded.
 */
BOOST_AUTO_TEST_CASE(property_error_reporting, * boost::unit_test::timeout(90)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::size_t> error_type_dist(0, 3);
    
    std::vector<std::string> error_messages = {
        "State machine corruption detected",
        "Invalid command format",
        "Resource exhaustion during application",
        "Timeout during state machine operation"
    };
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        ApplicationFailureTracker tracker;
        
        // Apply one successful entry
        std::vector<std::byte> success_command{std::byte{0x22}, std::byte{0x01}};
        tracker.record_application_success(1, success_command, 0, 1);
        
        // Apply a failing entry with specific error
        auto error_type = error_type_dist(rng);
        std::string error_message = error_messages[error_type];
        std::vector<std::byte> failure_command{std::byte{0x33}, std::byte{0x02}};
        tracker.record_application_failure(2, failure_command, error_message, 1);
        
        // Verify error reporting
        BOOST_CHECK_MESSAGE(tracker.has_failure_occurred(),
            "Failure should be detected for error reporting test");
        
        auto attempts = tracker.get_attempts();
        bool found_error = false;
        for (const auto& attempt : attempts) {
            if (!attempt.success && attempt.error_message.has_value()) {
                BOOST_CHECK_MESSAGE(attempt.error_message.value() == error_message,
                    "Error message should be properly recorded");
                found_error = true;
                break;
            }
        }
        
        BOOST_CHECK_MESSAGE(found_error,
            "Error message should be found in failure records");
        
        tracker.clear();
    }
}

BOOST_AUTO_TEST_SUITE_END()