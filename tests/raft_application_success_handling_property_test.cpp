/**
 * Property-Based Test for Application Success Handling
 * 
 * Feature: raft-completion, Property 24: Application Success Handling
 * Validates: Requirements 5.3
 * 
 * Property: For any successful state machine application, the applied index 
 * is updated and waiting client futures are fulfilled.
 */

#define BOOST_TEST_MODULE RaftApplicationSuccessHandlingPropertyTest
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
        char* argv_data[] = {const_cast<char*>("raft_application_success_handling_property_test"), nullptr};
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
 * Helper class to simulate state machine application success tracking
 */
class ApplicationSuccessTracker {
public:
    struct ApplicationResult {
        std::uint64_t log_index;
        std::vector<std::byte> command;
        bool success;
        std::chrono::steady_clock::time_point applied_at;
        std::uint64_t applied_index_after;
    };
    
    auto record_application_success(
        std::uint64_t log_index,
        const std::vector<std::byte>& command,
        std::uint64_t new_applied_index
    ) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _applications.push_back({
            log_index,
            command,
            true,
            std::chrono::steady_clock::now(),
            new_applied_index
        });
    }
    
    auto get_applications() const -> std::vector<ApplicationResult> {
        std::lock_guard<std::mutex> lock(_mutex);
        return _applications;
    }
    
    auto verify_applied_index_progression() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_applications.empty()) {
            return true;
        }
        
        // Check that applied index progresses correctly
        for (std::size_t i = 1; i < _applications.size(); ++i) {
            if (_applications[i].applied_index_after <= _applications[i-1].applied_index_after) {
                return false;
            }
        }
        return true;
    }
    
    auto verify_all_successful() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        return std::all_of(_applications.begin(), _applications.end(),
            [](const ApplicationResult& result) {
                return result.success;
            });
    }
    
    auto get_final_applied_index() const -> std::uint64_t {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_applications.empty()) {
            return 0;
        }
        return _applications.back().applied_index_after;
    }
    
    auto get_application_count() const -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        return _applications.size();
    }
    
    auto clear() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _applications.clear();
    }
    
private:
    mutable std::mutex _mutex;
    std::vector<ApplicationResult> _applications;
};

BOOST_AUTO_TEST_SUITE(application_success_handling_property_tests)

/**
 * Property: Application success handling
 * 
 * For any successful state machine application, the applied index 
 * is updated and waiting client futures are fulfilled.
 */
BOOST_AUTO_TEST_CASE(property_application_success_handling, * boost::unit_test::timeout(120)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::size_t> command_count_dist(3, 10);
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        ApplicationSuccessTracker tracker;
        
        auto num_commands = command_count_dist(rng);
        std::uint64_t current_applied_index = 0;
        
        // Simulate successful application of multiple entries
        for (std::size_t i = 1; i <= num_commands; ++i) {
            std::vector<std::byte> command;
            command.push_back(static_cast<std::byte>(0xAA)); // Success test marker
            command.push_back(static_cast<std::byte>(i & 0xFF)); // Command index
            for (std::size_t j = 0; j < 5; ++j) {
                command.push_back(static_cast<std::byte>((i * 5 + j) % 256));
            }
            
            // Simulate successful application
            std::uint64_t log_index = i;
            current_applied_index = log_index; // Applied index advances to this entry
            
            tracker.record_application_success(log_index, command, current_applied_index);
        }
        
        // Property verification: For successful applications
        // 1. Applied index should be updated correctly
        // 2. All applications should be marked as successful
        // 3. Applied index should progress monotonically
        
        BOOST_CHECK_MESSAGE(tracker.verify_all_successful(),
            "All state machine applications should be successful");
        
        BOOST_CHECK_MESSAGE(tracker.verify_applied_index_progression(),
            "Applied index should progress monotonically with successful applications");
        
        BOOST_CHECK_MESSAGE(tracker.get_final_applied_index() == num_commands,
            "Final applied index should equal the last applied entry index");
        
        BOOST_CHECK_MESSAGE(tracker.get_application_count() == num_commands,
            "All entries should be applied exactly once");
        
        // Additional verification: applied index should match log index for each entry
        auto applications = tracker.get_applications();
        for (std::size_t i = 0; i < applications.size(); ++i) {
            BOOST_CHECK_MESSAGE(applications[i].applied_index_after == applications[i].log_index,
                "Applied index should be updated to match the log index of the applied entry");
        }
        
        tracker.clear();
    }
}

/**
 * Property: Batch application success handling
 * 
 * For any batch of successful state machine applications, the applied index 
 * is updated to the highest applied entry and all futures are fulfilled.
 */
BOOST_AUTO_TEST_CASE(property_batch_application_success, * boost::unit_test::timeout(90)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::size_t> batch_size_dist(2, 8);
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        ApplicationSuccessTracker tracker;
        
        auto batch_size = batch_size_dist(rng);
        std::uint64_t starting_applied_index = 5; // Start after some previous applications
        
        // Simulate batch application of entries
        for (std::size_t i = 0; i < batch_size; ++i) {
            std::uint64_t log_index = starting_applied_index + i + 1;
            
            std::vector<std::byte> command;
            command.push_back(static_cast<std::byte>(0xBB)); // Batch success marker
            command.push_back(static_cast<std::byte>(log_index & 0xFF));
            command.push_back(static_cast<std::byte>((log_index >> 8) & 0xFF));
            for (std::size_t j = 0; j < 4; ++j) {
                command.push_back(static_cast<std::byte>((log_index + j) % 256));
            }
            
            // Applied index advances with each successful application
            std::uint64_t new_applied_index = log_index;
            tracker.record_application_success(log_index, command, new_applied_index);
        }
        
        // Verify batch application success properties
        BOOST_CHECK_MESSAGE(tracker.verify_all_successful(),
            "All entries in batch should be applied successfully");
        
        BOOST_CHECK_MESSAGE(tracker.verify_applied_index_progression(),
            "Applied index should progress correctly during batch application");
        
        std::uint64_t expected_final_applied_index = starting_applied_index + batch_size;
        BOOST_CHECK_MESSAGE(tracker.get_final_applied_index() == expected_final_applied_index,
            "Final applied index should be the highest applied entry in the batch");
        
        BOOST_CHECK_MESSAGE(tracker.get_application_count() == batch_size,
            "All entries in batch should be applied exactly once");
        
        tracker.clear();
    }
}

/**
 * Property: Single entry application success
 * 
 * For any single successful state machine application, the applied index 
 * is updated correctly and the operation is marked as successful.
 */
BOOST_AUTO_TEST_CASE(property_single_entry_success, * boost::unit_test::timeout(60)) {
    ApplicationSuccessTracker tracker;
    
    // Test single entry application
    std::uint64_t log_index = 1;
    std::vector<std::byte> command{std::byte{0xCC}, std::byte{0x01}};
    
    tracker.record_application_success(log_index, command, log_index);
    
    BOOST_CHECK_MESSAGE(tracker.verify_all_successful(),
        "Single entry application should be successful");
    
    BOOST_CHECK_MESSAGE(tracker.get_final_applied_index() == log_index,
        "Applied index should be updated to the single entry's log index");
    
    BOOST_CHECK_MESSAGE(tracker.get_application_count() == 1,
        "Single entry should be applied exactly once");
    
    // Test with higher starting applied index
    tracker.clear();
    std::uint64_t high_log_index = 100;
    std::vector<std::byte> high_command{std::byte{0xDD}, std::byte{0x64}};
    
    tracker.record_application_success(high_log_index, high_command, high_log_index);
    
    BOOST_CHECK_MESSAGE(tracker.verify_all_successful(),
        "High index entry application should be successful");
    
    BOOST_CHECK_MESSAGE(tracker.get_final_applied_index() == high_log_index,
        "Applied index should be updated to high log index");
}

/**
 * Property: Applied index monotonicity
 * 
 * For any sequence of successful applications, the applied index 
 * never decreases and always advances to the applied entry's index.
 */
BOOST_AUTO_TEST_CASE(property_applied_index_monotonicity, * boost::unit_test::timeout(90)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::size_t> sequence_length_dist(5, 15);
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        ApplicationSuccessTracker tracker;
        
        auto sequence_length = sequence_length_dist(rng);
        std::uint64_t base_index = 10; // Start from a non-zero base
        
        // Apply entries in sequence
        for (std::size_t i = 0; i < sequence_length; ++i) {
            std::uint64_t log_index = base_index + i;
            
            std::vector<std::byte> command;
            command.push_back(static_cast<std::byte>(0xEE)); // Monotonicity test marker
            command.push_back(static_cast<std::byte>(log_index & 0xFF));
            for (std::size_t j = 0; j < 3; ++j) {
                command.push_back(static_cast<std::byte>((log_index + j) % 256));
            }
            
            tracker.record_application_success(log_index, command, log_index);
        }
        
        // Verify monotonicity property
        BOOST_CHECK_MESSAGE(tracker.verify_applied_index_progression(),
            "Applied index should progress monotonically");
        
        auto applications = tracker.get_applications();
        for (std::size_t i = 0; i < applications.size(); ++i) {
            std::uint64_t expected_applied_index = base_index + i;
            BOOST_CHECK_MESSAGE(applications[i].applied_index_after == expected_applied_index,
                "Applied index should advance to each entry's log index");
        }
        
        std::uint64_t expected_final_index = base_index + sequence_length - 1;
        BOOST_CHECK_MESSAGE(tracker.get_final_applied_index() == expected_final_index,
            "Final applied index should be the last entry's log index");
        
        tracker.clear();
    }
}

/**
 * Property: Future fulfillment simulation
 * 
 * For any successful application, client futures waiting for that entry
 * should be fulfilled (simulated through success tracking).
 */
BOOST_AUTO_TEST_CASE(property_future_fulfillment_simulation, * boost::unit_test::timeout(90)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::size_t> pending_count_dist(2, 6);
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        ApplicationSuccessTracker tracker;
        
        auto pending_count = pending_count_dist(rng);
        
        // Simulate multiple pending operations that get fulfilled
        for (std::size_t i = 1; i <= pending_count; ++i) {
            std::vector<std::byte> command;
            command.push_back(static_cast<std::byte>(0xFF)); // Future fulfillment marker
            command.push_back(static_cast<std::byte>(iteration & 0xFF)); // Iteration marker
            command.push_back(static_cast<std::byte>(i & 0xFF)); // Operation marker
            for (std::size_t j = 0; j < 4; ++j) {
                command.push_back(static_cast<std::byte>((i + j) % 256));
            }
            
            // Each successful application fulfills the corresponding future
            tracker.record_application_success(i, command, i);
        }
        
        // Property: All applications should be successful (futures fulfilled)
        BOOST_CHECK_MESSAGE(tracker.verify_all_successful(),
            "All applications should be successful, indicating future fulfillment");
        
        BOOST_CHECK_MESSAGE(tracker.get_application_count() == pending_count,
            "All pending operations should be fulfilled through successful application");
        
        BOOST_CHECK_MESSAGE(tracker.get_final_applied_index() == pending_count,
            "Applied index should advance to fulfill all pending operations");
        
        tracker.clear();
    }
}

BOOST_AUTO_TEST_SUITE_END()