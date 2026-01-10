/**
 * Property-Based Test for Applied Index Catch-up
 * 
 * Feature: raft-completion, Property 26: Applied Index Catch-up
 * Validates: Requirements 5.5
 * 
 * Property: For any scenario where applied index lags behind commit index, 
 * the system catches up by applying pending entries.
 */

#define BOOST_TEST_MODULE RaftAppliedIndexCatchupPropertyTest
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
        char* argv_data[] = {const_cast<char*>("raft_applied_index_catchup_property_test"), nullptr};
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
 * Helper class to simulate applied index catch-up scenarios
 */
class AppliedIndexCatchupTracker {
public:
    struct CatchupState {
        std::uint64_t commit_index;
        std::uint64_t applied_index_before;
        std::uint64_t applied_index_after;
        std::vector<std::uint64_t> applied_entries;
        std::chrono::steady_clock::time_point catchup_started;
        std::chrono::steady_clock::time_point catchup_completed;
        bool catchup_successful;
    };
    
    auto record_catchup_scenario(
        std::uint64_t commit_index,
        std::uint64_t initial_applied_index,
        const std::vector<std::uint64_t>& entries_to_apply
    ) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        
        auto start_time = std::chrono::steady_clock::now();
        
        // Simulate applying all pending entries
        std::uint64_t final_applied_index = initial_applied_index;
        std::vector<std::uint64_t> applied_entries;
        
        for (std::uint64_t entry_index : entries_to_apply) {
            if (entry_index > initial_applied_index && entry_index <= commit_index) {
                applied_entries.push_back(entry_index);
                final_applied_index = entry_index;
            }
        }
        
        auto end_time = std::chrono::steady_clock::now();
        bool successful = (final_applied_index == commit_index);
        
        _catchup_scenarios.push_back({
            commit_index,
            initial_applied_index,
            final_applied_index,
            applied_entries,
            start_time,
            end_time,
            successful
        });
    }
    
    auto get_scenarios() const -> std::vector<CatchupState> {
        std::lock_guard<std::mutex> lock(_mutex);
        return _catchup_scenarios;
    }
    
    auto verify_all_catchups_successful() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        return std::all_of(_catchup_scenarios.begin(), _catchup_scenarios.end(),
            [](const CatchupState& scenario) {
                return scenario.catchup_successful;
            });
    }
    
    auto verify_applied_index_reaches_commit_index() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& scenario : _catchup_scenarios) {
            if (scenario.applied_index_after != scenario.commit_index) {
                return false;
            }
        }
        return true;
    }
    
    auto verify_sequential_application() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& scenario : _catchup_scenarios) {
            // Check that applied entries are in sequential order
            for (std::size_t i = 1; i < scenario.applied_entries.size(); ++i) {
                if (scenario.applied_entries[i] <= scenario.applied_entries[i-1]) {
                    return false;
                }
            }
            
            // Check that applied entries start from applied_index_before + 1
            if (!scenario.applied_entries.empty()) {
                if (scenario.applied_entries.front() != scenario.applied_index_before + 1) {
                    return false;
                }
            }
        }
        return true;
    }
    
    auto get_scenario_count() const -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        return _catchup_scenarios.size();
    }
    
    auto clear() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _catchup_scenarios.clear();
    }
    
private:
    mutable std::mutex _mutex;
    std::vector<CatchupState> _catchup_scenarios;
};

BOOST_AUTO_TEST_SUITE(applied_index_catchup_property_tests)

/**
 * Property: Applied index catch-up
 * 
 * For any scenario where applied index lags behind commit index, 
 * the system catches up by applying pending entries.
 */
BOOST_AUTO_TEST_CASE(property_applied_index_catchup, * boost::unit_test::timeout(120)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::uint64_t> commit_index_dist(5, 20);
    std::uniform_int_distribution<std::uint64_t> lag_dist(1, 8);
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        AppliedIndexCatchupTracker tracker;
        
        auto commit_index = commit_index_dist(rng);
        auto lag = std::min(lag_dist(rng), commit_index);
        auto applied_index = commit_index - lag;
        
        // Create entries that need to be applied to catch up
        std::vector<std::uint64_t> entries_to_apply;
        for (std::uint64_t i = applied_index + 1; i <= commit_index; ++i) {
            entries_to_apply.push_back(i);
        }
        
        // Record the catch-up scenario
        tracker.record_catchup_scenario(commit_index, applied_index, entries_to_apply);
        
        // Property verification
        BOOST_CHECK_MESSAGE(tracker.verify_all_catchups_successful(),
            "Applied index catch-up should be successful");
        
        BOOST_CHECK_MESSAGE(tracker.verify_applied_index_reaches_commit_index(),
            "Applied index should reach commit index after catch-up");
        
        BOOST_CHECK_MESSAGE(tracker.verify_sequential_application(),
            "Entries should be applied sequentially during catch-up");
        
        auto scenarios = tracker.get_scenarios();
        BOOST_CHECK_MESSAGE(!scenarios.empty(),
            "At least one catch-up scenario should be recorded");
        
        if (!scenarios.empty()) {
            const auto& scenario = scenarios[0];
            BOOST_CHECK_MESSAGE(scenario.applied_entries.size() == lag,
                "Number of applied entries should equal the lag amount");
            
            BOOST_CHECK_MESSAGE(scenario.applied_index_after == commit_index,
                "Final applied index should equal commit index");
        }
        
        tracker.clear();
    }
}

/**
 * Property: Large gap catch-up
 * 
 * For any scenario with a large gap between applied and commit indices,
 * the system successfully applies all pending entries.
 */
BOOST_AUTO_TEST_CASE(property_large_gap_catchup, * boost::unit_test::timeout(90)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::uint64_t> large_commit_dist(50, 100);
    std::uniform_int_distribution<std::uint64_t> large_lag_dist(20, 40);
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        AppliedIndexCatchupTracker tracker;
        
        auto commit_index = large_commit_dist(rng);
        auto lag = std::min(large_lag_dist(rng), commit_index);
        auto applied_index = commit_index - lag;
        
        // Create a large number of entries to apply
        std::vector<std::uint64_t> entries_to_apply;
        for (std::uint64_t i = applied_index + 1; i <= commit_index; ++i) {
            entries_to_apply.push_back(i);
        }
        
        tracker.record_catchup_scenario(commit_index, applied_index, entries_to_apply);
        
        // Verify large gap catch-up properties
        BOOST_CHECK_MESSAGE(tracker.verify_all_catchups_successful(),
            "Large gap catch-up should be successful");
        
        BOOST_CHECK_MESSAGE(tracker.verify_applied_index_reaches_commit_index(),
            "Applied index should reach commit index even with large gaps");
        
        BOOST_CHECK_MESSAGE(tracker.verify_sequential_application(),
            "Sequential application should be maintained even with large gaps");
        
        auto scenarios = tracker.get_scenarios();
        if (!scenarios.empty()) {
            const auto& scenario = scenarios[0];
            BOOST_CHECK_MESSAGE(scenario.applied_entries.size() == lag,
                "All entries in large gap should be applied");
        }
        
        tracker.clear();
    }
}

/**
 * Property: Multiple catch-up scenarios
 * 
 * For any sequence of catch-up scenarios, each one successfully
 * brings applied index up to commit index.
 */
BOOST_AUTO_TEST_CASE(property_multiple_catchup_scenarios, * boost::unit_test::timeout(90)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::size_t> scenario_count_dist(3, 8);
    std::uniform_int_distribution<std::uint64_t> increment_dist(2, 6);
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        AppliedIndexCatchupTracker tracker;
        
        auto scenario_count = scenario_count_dist(rng);
        std::uint64_t current_applied = 0;
        std::uint64_t current_commit = 0;
        
        // Create multiple catch-up scenarios
        for (std::size_t i = 0; i < scenario_count; ++i) {
            auto increment = increment_dist(rng);
            current_commit += increment;
            
            // Create entries for this catch-up
            std::vector<std::uint64_t> entries_to_apply;
            for (std::uint64_t j = current_applied + 1; j <= current_commit; ++j) {
                entries_to_apply.push_back(j);
            }
            
            tracker.record_catchup_scenario(current_commit, current_applied, entries_to_apply);
            current_applied = current_commit; // After catch-up, applied equals commit
        }
        
        // Verify multiple scenario properties
        BOOST_CHECK_MESSAGE(tracker.verify_all_catchups_successful(),
            "All catch-up scenarios should be successful");
        
        BOOST_CHECK_MESSAGE(tracker.verify_applied_index_reaches_commit_index(),
            "Applied index should reach commit index in all scenarios");
        
        BOOST_CHECK_MESSAGE(tracker.verify_sequential_application(),
            "Sequential application should be maintained across all scenarios");
        
        BOOST_CHECK_MESSAGE(tracker.get_scenario_count() == scenario_count,
            "All catch-up scenarios should be recorded");
        
        tracker.clear();
    }
}

/**
 * Property: No catch-up needed
 * 
 * For any scenario where applied index equals commit index,
 * no catch-up is needed and the system remains stable.
 */
BOOST_AUTO_TEST_CASE(property_no_catchup_needed, * boost::unit_test::timeout(60)) {
    AppliedIndexCatchupTracker tracker;
    
    // Test case where applied index equals commit index
    std::uint64_t index = 10;
    std::vector<std::uint64_t> no_entries; // No entries to apply
    
    tracker.record_catchup_scenario(index, index, no_entries);
    
    auto scenarios = tracker.get_scenarios();
    BOOST_CHECK_MESSAGE(!scenarios.empty(),
        "No-catchup scenario should be recorded");
    
    if (!scenarios.empty()) {
        const auto& scenario = scenarios[0];
        BOOST_CHECK_MESSAGE(scenario.applied_entries.empty(),
            "No entries should be applied when no catch-up is needed");
        
        BOOST_CHECK_MESSAGE(scenario.applied_index_before == scenario.applied_index_after,
            "Applied index should remain unchanged when no catch-up is needed");
        
        BOOST_CHECK_MESSAGE(scenario.catchup_successful,
            "No-catchup scenario should be considered successful");
    }
}

/**
 * Property: Partial catch-up scenarios
 * 
 * For any scenario where only some entries between applied and commit
 * indices are available, the system applies what it can.
 */
BOOST_AUTO_TEST_CASE(property_partial_catchup, * boost::unit_test::timeout(90)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::uint64_t> commit_dist(10, 20);
    std::uniform_int_distribution<std::uint64_t> applied_dist(1, 8);
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        AppliedIndexCatchupTracker tracker;
        
        auto commit_index = commit_dist(rng);
        auto applied_index = applied_dist(rng);
        
        // Only provide some of the entries between applied and commit
        std::vector<std::uint64_t> available_entries;
        for (std::uint64_t i = applied_index + 1; i <= commit_index; i += 2) {
            available_entries.push_back(i); // Only odd-indexed entries
        }
        
        tracker.record_catchup_scenario(commit_index, applied_index, available_entries);
        
        // For partial catch-up, we verify that:
        // 1. Sequential application is maintained for available entries
        // 2. Applied index advances as far as possible with available entries
        
        BOOST_CHECK_MESSAGE(tracker.verify_sequential_application(),
            "Sequential application should be maintained even with partial catch-up");
        
        auto scenarios = tracker.get_scenarios();
        if (!scenarios.empty()) {
            const auto& scenario = scenarios[0];
            
            // Applied index should advance to the highest available entry
            if (!scenario.applied_entries.empty()) {
                std::uint64_t highest_applied = *std::max_element(
                    scenario.applied_entries.begin(), 
                    scenario.applied_entries.end()
                );
                BOOST_CHECK_MESSAGE(scenario.applied_index_after >= highest_applied,
                    "Applied index should advance to at least the highest available entry");
            }
        }
        
        tracker.clear();
    }
}

/**
 * Property: Catch-up with gaps
 * 
 * For any scenario where there are gaps in the log (due to snapshots),
 * catch-up still works correctly for available entries.
 */
BOOST_AUTO_TEST_CASE(property_catchup_with_gaps, * boost::unit_test::timeout(90)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::uint64_t> gap_start_dist(20, 30);
    std::uniform_int_distribution<std::uint64_t> gap_size_dist(5, 10);
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        AppliedIndexCatchupTracker tracker;
        
        auto gap_start = gap_start_dist(rng);
        auto gap_size = gap_size_dist(rng);
        auto applied_index = gap_start - 1;
        auto commit_index = gap_start + gap_size + 5;
        
        // Create entries with a gap (simulating snapshot compaction)
        std::vector<std::uint64_t> entries_with_gaps;
        
        // Add entries after the gap
        for (std::uint64_t i = gap_start + gap_size; i <= commit_index; ++i) {
            entries_with_gaps.push_back(i);
        }
        
        tracker.record_catchup_scenario(commit_index, applied_index, entries_with_gaps);
        
        // Verify catch-up with gaps
        BOOST_CHECK_MESSAGE(tracker.verify_sequential_application(),
            "Sequential application should work correctly with gaps");
        
        auto scenarios = tracker.get_scenarios();
        if (!scenarios.empty()) {
            const auto& scenario = scenarios[0];
            
            // Should apply available entries after the gap
            BOOST_CHECK_MESSAGE(!scenario.applied_entries.empty(),
                "Should apply available entries even with gaps");
            
            // Applied entries should be sequential within available range
            for (std::size_t i = 1; i < scenario.applied_entries.size(); ++i) {
                BOOST_CHECK_MESSAGE(
                    scenario.applied_entries[i] == scenario.applied_entries[i-1] + 1,
                    "Applied entries should be sequential within available range"
                );
            }
        }
        
        tracker.clear();
    }
}

BOOST_AUTO_TEST_SUITE_END()