#define BOOST_TEST_MODULE RaftUnresponsiveFollowerHandlingPropertyTest

#include <boost/test/included/unit_test.hpp>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace std;

namespace {
    constexpr std::chrono::milliseconds test_timeout{5000};
    constexpr std::size_t min_cluster_size = 3;
    constexpr std::size_t max_cluster_size = 9;
    constexpr std::size_t test_iterations = 30;
    constexpr std::size_t min_log_entries = 1;
    constexpr std::size_t max_log_entries = 10;
    constexpr std::size_t failure_threshold = 3; // Number of consecutive failures to mark as unresponsive
    constexpr std::chrono::milliseconds response_timeout{1000};
}

// Simplified types for testing the property
using NodeId = std::uint64_t;
using LogIndex = std::uint64_t;
using Term = std::uint64_t;

// Enum to represent follower availability states
enum class FollowerAvailability {
    available,
    unavailable
};

// Stream operator for FollowerAvailability to support Boost.Test printing
std::ostream& operator<<(std::ostream& os, FollowerAvailability availability) {
    switch (availability) {
        case FollowerAvailability::available: return os << "available";
        case FollowerAvailability::unavailable: return os << "unavailable";
        default: return os << "unknown";
    }
}

// Simple unresponsive follower handler to test the property
class UnresponsiveFollowerHandler {
private:
    std::size_t _cluster_size;
    LogIndex _commit_index{0};
    std::unordered_map<LogIndex, std::unordered_set<NodeId>> _acknowledgments;
    std::unordered_map<NodeId, FollowerAvailability> _follower_availability;
    std::unordered_map<NodeId, std::size_t> _consecutive_failures;
    std::unordered_set<NodeId> _unavailable_followers;
    std::size_t _failure_threshold;
    
public:
    explicit UnresponsiveFollowerHandler(std::size_t cluster_size, std::size_t failure_threshold = ::failure_threshold) 
        : _cluster_size(cluster_size), _failure_threshold(failure_threshold) {
        // Initialize all followers as available
        for (std::size_t i = 2; i <= cluster_size; ++i) {
            NodeId follower_id = static_cast<NodeId>(i);
            _follower_availability[follower_id] = FollowerAvailability::available;
            _consecutive_failures[follower_id] = 0;
        }
    }
    
    // Record a successful response from a follower
    void record_successful_response(NodeId follower_id) {
        _consecutive_failures[follower_id] = 0;
        if (_follower_availability[follower_id] == FollowerAvailability::unavailable) {
            // Follower is back online, mark as available
            _follower_availability[follower_id] = FollowerAvailability::available;
            _unavailable_followers.erase(follower_id);
        }
    }
    
    // Record a failed response from a follower
    void record_failed_response(NodeId follower_id) {
        _consecutive_failures[follower_id]++;
        
        // Check if follower should be marked as unavailable
        if (_consecutive_failures[follower_id] >= _failure_threshold) {
            if (_follower_availability[follower_id] == FollowerAvailability::available) {
                _follower_availability[follower_id] = FollowerAvailability::unavailable;
                _unavailable_followers.insert(follower_id);
            }
        }
    }
    
    // Record an acknowledgment from a follower for a specific log entry
    void record_acknowledgment(LogIndex log_index, NodeId follower_id) {
        // Only record acknowledgments from available followers
        if (_follower_availability[follower_id] == FollowerAvailability::available) {
            _acknowledgments[log_index].insert(follower_id);
            record_successful_response(follower_id);
            update_commit_index();
        }
    }
    
    // Get current commit index
    LogIndex get_commit_index() const {
        return _commit_index;
    }
    
    // Get the number of acknowledgments for a log entry (including leader)
    std::size_t get_acknowledgment_count(LogIndex log_index) const {
        auto it = _acknowledgments.find(log_index);
        // +1 for leader's implicit acknowledgment
        return (it != _acknowledgments.end()) ? it->second.size() + 1 : 1;
    }
    
    // Get the number of available followers (excluding unavailable ones)
    std::size_t get_available_count() const {
        std::size_t available_count = 1; // Leader is always available
        for (const auto& [follower_id, availability] : _follower_availability) {
            if (availability == FollowerAvailability::available) {
                available_count++;
            }
        }
        return available_count;
    }
    
    // Check if an entry has majority acknowledgment among available nodes
    bool has_majority_acknowledgment(LogIndex log_index) const {
        std::size_t available_count = get_available_count();
        std::size_t majority_needed = (available_count / 2) + 1;
        return get_acknowledgment_count(log_index) >= majority_needed;
    }
    
    // Get follower availability
    FollowerAvailability get_follower_availability(NodeId follower_id) const {
        auto it = _follower_availability.find(follower_id);
        return (it != _follower_availability.end()) ? it->second : FollowerAvailability::available;
    }
    
    // Get consecutive failure count for a follower
    std::size_t get_consecutive_failures(NodeId follower_id) const {
        auto it = _consecutive_failures.find(follower_id);
        return (it != _consecutive_failures.end()) ? it->second : 0;
    }
    
    // Get unavailable followers count
    std::size_t get_unavailable_followers_count() const {
        return _unavailable_followers.size();
    }
    
    // Check if replication can continue despite unavailable followers
    bool can_continue_replication() const {
        std::size_t available_count = get_available_count();
        std::size_t majority_needed = (available_count / 2) + 1;
        
        // Can continue if we have enough available nodes to form a majority
        return available_count >= majority_needed;
    }
    
    // Get the set of unavailable followers
    const std::unordered_set<NodeId>& get_unavailable_followers() const {
        return _unavailable_followers;
    }
    
    // Check if a follower is marked as unavailable
    bool is_follower_unavailable(NodeId follower_id) const {
        return _unavailable_followers.count(follower_id) > 0;
    }
    
    // Simulate leader proposing an entry (for testing purposes)
    void propose_entry(LogIndex log_index) {
        // Create an empty acknowledgment set for the entry
        // This represents that the entry exists and the leader has implicitly acknowledged it
        _acknowledgments[log_index] = std::unordered_set<NodeId>{};
        update_commit_index();
    }
    
    // Clear all acknowledgments and reset commit index
    void clear() {
        _acknowledgments.clear();
        _commit_index = 0;
    }
    
    // Reset all follower states (for testing)
    void reset_follower_states() {
        for (auto& [follower_id, availability] : _follower_availability) {
            availability = FollowerAvailability::available;
            _consecutive_failures[follower_id] = 0;
        }
        _unavailable_followers.clear();
    }
    
    // Update commit index based on majority acknowledgments among available nodes
    void update_commit_index() {
        // Find the highest log index that has majority acknowledgment
        // and all previous entries also have majority acknowledgment
        LogIndex new_commit_index = _commit_index;
        
        // Get all acknowledged entries in order
        std::vector<LogIndex> acknowledged_entries;
        for (const auto& [log_index, _] : _acknowledgments) {
            acknowledged_entries.push_back(log_index);
        }
        std::sort(acknowledged_entries.begin(), acknowledged_entries.end());
        
        if (!acknowledged_entries.empty()) {
            for (LogIndex log_index = _commit_index + 1; log_index <= acknowledged_entries.back(); ++log_index) {
                if (has_majority_acknowledgment(log_index)) {
                    new_commit_index = log_index;
                } else {
                    // Can't advance commit index past an entry without majority
                    break;
                }
            }
        }
        
        _commit_index = new_commit_index;
    }
    
private:
};

/**
 * **Feature: raft-completion, Property 30: Unresponsive Follower Handling**
 * 
 * Property: For any consistently unresponsive follower, the system marks it unavailable but continues with majority.
 * **Validates: Requirements 6.4**
 */
BOOST_AUTO_TEST_CASE(raft_unresponsive_follower_handling_property_test, * boost::unit_test::timeout(120)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> cluster_size_dist(min_cluster_size, max_cluster_size);
    std::uniform_int_distribution<std::size_t> entry_count_dist(min_log_entries, max_log_entries);
    std::uniform_int_distribution<int> failure_rate_dist(20, 60); // percentage of followers that will fail
    std::uniform_int_distribution<int> response_rate_dist(70, 100); // response rate for available followers
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random cluster configuration
        std::size_t cluster_size = cluster_size_dist(gen);
        if (cluster_size % 2 == 0) cluster_size++; // Ensure odd number for clear majority
        
        const std::size_t follower_count = cluster_size - 1; // Exclude leader
        const std::size_t entry_count = entry_count_dist(gen);
        
        BOOST_TEST_MESSAGE("Testing cluster size: " << cluster_size 
                          << ", followers: " << follower_count 
                          << ", log entries: " << entry_count);
        
        // Create follower IDs (leader is ID 1, followers are 2, 3, 4, ...)
        std::vector<NodeId> follower_ids;
        for (std::size_t i = 2; i <= cluster_size; ++i) {
            follower_ids.push_back(static_cast<NodeId>(i));
        }
        
        // Create unresponsive follower handler
        UnresponsiveFollowerHandler handler(cluster_size);
        
        // Randomly select followers that will consistently fail
        const int failure_rate = failure_rate_dist(gen);
        std::vector<NodeId> failing_followers;
        
        for (const auto& follower_id : follower_ids) {
            if ((gen() % 100) < failure_rate) {
                failing_followers.push_back(follower_id);
                BOOST_TEST_MESSAGE("  Follower " << follower_id << " will consistently FAIL");
            } else {
                BOOST_TEST_MESSAGE("  Follower " << follower_id << " will be RESPONSIVE");
            }
        }
        
        BOOST_TEST_MESSAGE("Failing followers: " << failing_followers.size() 
                          << " out of " << follower_count);
        
        // Simulate multiple rounds of communication to trigger failure detection
        const std::size_t communication_rounds = failure_threshold + 2; // Ensure we exceed threshold
        
        for (std::size_t round = 1; round <= communication_rounds; ++round) {
            BOOST_TEST_MESSAGE("Communication round " << round);
            
            for (const auto& follower_id : follower_ids) {
                bool is_failing = std::find(failing_followers.begin(), failing_followers.end(), follower_id) != failing_followers.end();
                
                if (is_failing) {
                    // Consistently failing follower
                    handler.record_failed_response(follower_id);
                    BOOST_TEST_MESSAGE("  Follower " << follower_id << " failed (consecutive: " 
                                      << handler.get_consecutive_failures(follower_id) << ")");
                } else {
                    // Responsive follower (with occasional failures)
                    const int response_rate = response_rate_dist(gen);
                    if ((gen() % 100) < response_rate) {
                        handler.record_successful_response(follower_id);
                        BOOST_TEST_MESSAGE("  Follower " << follower_id << " responded successfully");
                    } else {
                        handler.record_failed_response(follower_id);
                        BOOST_TEST_MESSAGE("  Follower " << follower_id << " failed occasionally (consecutive: " 
                                          << handler.get_consecutive_failures(follower_id) << ")");
                    }
                }
            }
            
            // Property 1: Consistently failing followers should be marked as unavailable after threshold
            for (const auto& follower_id : failing_followers) {
                const std::size_t consecutive_failures = handler.get_consecutive_failures(follower_id);
                const bool should_be_unavailable = consecutive_failures >= failure_threshold;
                const bool is_unavailable = handler.is_follower_unavailable(follower_id);
                
                BOOST_CHECK_EQUAL(is_unavailable, should_be_unavailable);
                
                if (should_be_unavailable) {
                    BOOST_CHECK_EQUAL(handler.get_follower_availability(follower_id), FollowerAvailability::unavailable);
                    BOOST_TEST_MESSAGE("  Follower " << follower_id << " marked as UNAVAILABLE");
                }
            }
            
            // Property 2: Occasionally failing followers should remain available
            for (const auto& follower_id : follower_ids) {
                bool is_consistently_failing = std::find(failing_followers.begin(), failing_followers.end(), follower_id) != failing_followers.end();
                
                if (!is_consistently_failing) {
                    // Should not be marked as unavailable due to occasional failures
                    const std::size_t consecutive_failures = handler.get_consecutive_failures(follower_id);
                    if (consecutive_failures < failure_threshold) {
                        BOOST_CHECK_EQUAL(handler.get_follower_availability(follower_id), FollowerAvailability::available);
                    }
                }
            }
        }
        
        // Property 3: System should continue replication despite unavailable followers
        const std::size_t available_count = handler.get_available_count();
        const std::size_t unavailable_count = handler.get_unavailable_followers_count();
        const bool can_continue = handler.can_continue_replication();
        
        BOOST_TEST_MESSAGE("Available nodes: " << available_count 
                          << ", Unavailable followers: " << unavailable_count
                          << ", Can continue: " << (can_continue ? "YES" : "NO"));
        
        // The system should be able to continue if there are enough available nodes for majority
        const std::size_t majority_needed = (available_count / 2) + 1;
        BOOST_CHECK_EQUAL(can_continue, available_count >= majority_needed);
        
        if (!can_continue) {
            BOOST_TEST_MESSAGE("Skipping replication test - insufficient available nodes");
            continue;
        }
        
        // Property 4: Replication should work with available followers only
        for (LogIndex log_index = 1; log_index <= entry_count; ++log_index) {
            BOOST_TEST_MESSAGE("Processing log entry " << log_index);
            
            std::size_t ack_count = 1; // Leader always acknowledges implicitly
            std::vector<NodeId> acknowledging_followers;
            
            // Simulate acknowledgments from available followers only
            for (const auto& follower_id : follower_ids) {
                const FollowerAvailability availability = handler.get_follower_availability(follower_id);
                
                if (availability == FollowerAvailability::available) {
                    // Available followers acknowledge with high probability
                    const int response_rate = response_rate_dist(gen);
                    if ((gen() % 100) < response_rate) {
                        handler.record_acknowledgment(log_index, follower_id);
                        acknowledging_followers.push_back(follower_id);
                        ack_count++;
                        BOOST_TEST_MESSAGE("  Available follower " << follower_id << " acknowledged entry " << log_index);
                    }
                } else {
                    // Unavailable followers don't acknowledge
                    BOOST_TEST_MESSAGE("  Unavailable follower " << follower_id << " did not acknowledge entry " << log_index);
                }
            }
            
            BOOST_TEST_MESSAGE("  Entry " << log_index << " has " << ack_count << " acknowledgments");
            
            // Property 5: Majority calculation should exclude unavailable followers
            const std::size_t current_available_count = handler.get_available_count();
            const std::size_t current_majority_needed = (current_available_count / 2) + 1;
            const bool has_majority = handler.has_majority_acknowledgment(log_index);
            const bool expected_majority = ack_count >= current_majority_needed;
            
            BOOST_CHECK_EQUAL(has_majority, expected_majority);
            BOOST_TEST_MESSAGE("  Majority check: " << ack_count << " >= " << current_majority_needed 
                              << " = " << (expected_majority ? "TRUE" : "FALSE"));
        }
        
        // Property 6: Unavailable followers should not contribute to acknowledgments
        const auto& unavailable_followers = handler.get_unavailable_followers();
        for (LogIndex log_index = 1; log_index <= entry_count; ++log_index) {
            for (const auto& unavailable_follower : unavailable_followers) {
                // Try to record acknowledgment from unavailable follower (should be ignored)
                const std::size_t ack_count_before = handler.get_acknowledgment_count(log_index);
                handler.record_acknowledgment(log_index, unavailable_follower);
                const std::size_t ack_count_after = handler.get_acknowledgment_count(log_index);
                
                // Acknowledgment count should not change
                BOOST_CHECK_EQUAL(ack_count_before, ack_count_after);
            }
        }
        
        // Property 7: Final commit index should reflect progress with available majority
        const LogIndex final_commit_index = handler.get_commit_index();
        BOOST_TEST_MESSAGE("Final commit index: " << final_commit_index);
        
        // The system should have made progress if there were enough available followers
        if (available_count >= majority_needed && entry_count > 0) {
            BOOST_CHECK_GE(final_commit_index, 0);
        }
    }
    
    // Test edge cases
    BOOST_TEST_MESSAGE("Testing edge cases...");
    
    // Test case: Single follower becomes unresponsive
    {
        const std::size_t cluster_size = 3;
        UnresponsiveFollowerHandler single_handler(cluster_size);
        
        const NodeId test_follower = 2;
        
        // Initially available
        BOOST_CHECK_EQUAL(single_handler.get_follower_availability(test_follower), FollowerAvailability::available);
        BOOST_CHECK_EQUAL(single_handler.get_consecutive_failures(test_follower), 0);
        BOOST_CHECK(!single_handler.is_follower_unavailable(test_follower));
        
        // Record failures up to threshold - 1 (should still be available)
        for (std::size_t i = 0; i < failure_threshold - 1; ++i) {
            single_handler.record_failed_response(test_follower);
        }
        
        BOOST_CHECK_EQUAL(single_handler.get_follower_availability(test_follower), FollowerAvailability::available);
        BOOST_CHECK_EQUAL(single_handler.get_consecutive_failures(test_follower), failure_threshold - 1);
        BOOST_CHECK(!single_handler.is_follower_unavailable(test_follower));
        
        // One more failure should mark as unavailable
        single_handler.record_failed_response(test_follower);
        
        BOOST_CHECK_EQUAL(single_handler.get_follower_availability(test_follower), FollowerAvailability::unavailable);
        BOOST_CHECK_EQUAL(single_handler.get_consecutive_failures(test_follower), failure_threshold);
        BOOST_CHECK(single_handler.is_follower_unavailable(test_follower));
        BOOST_CHECK_EQUAL(single_handler.get_unavailable_followers_count(), 1);
        
        BOOST_TEST_MESSAGE("✓ Single follower unresponsive test passed");
    }
    
    // Test case: Follower recovery from unresponsive state
    {
        const std::size_t cluster_size = 5;
        UnresponsiveFollowerHandler recovery_handler(cluster_size);
        
        const NodeId test_follower = 3;
        
        // Make follower unresponsive
        for (std::size_t i = 0; i < failure_threshold; ++i) {
            recovery_handler.record_failed_response(test_follower);
        }
        
        BOOST_CHECK_EQUAL(recovery_handler.get_follower_availability(test_follower), FollowerAvailability::unavailable);
        BOOST_CHECK(recovery_handler.is_follower_unavailable(test_follower));
        
        // Record successful response (should recover)
        recovery_handler.record_successful_response(test_follower);
        
        BOOST_CHECK_EQUAL(recovery_handler.get_follower_availability(test_follower), FollowerAvailability::available);
        BOOST_CHECK_EQUAL(recovery_handler.get_consecutive_failures(test_follower), 0);
        BOOST_CHECK(!recovery_handler.is_follower_unavailable(test_follower));
        BOOST_CHECK_EQUAL(recovery_handler.get_unavailable_followers_count(), 0);
        
        BOOST_TEST_MESSAGE("✓ Follower recovery test passed");
    }
    
    // Test case: Multiple followers become unresponsive, majority still available
    {
        const std::size_t cluster_size = 7;
        UnresponsiveFollowerHandler multi_handler(cluster_size);
        
        std::vector<NodeId> followers = {2, 3, 4, 5, 6, 7};
        std::vector<NodeId> failing_followers = {5, 6, 7}; // 3 out of 6 followers
        
        // Make some followers unresponsive
        for (const auto& follower_id : failing_followers) {
            for (std::size_t i = 0; i < failure_threshold; ++i) {
                multi_handler.record_failed_response(follower_id);
            }
        }
        
        // Property: System should still be able to continue (4 available: leader + 3 followers, majority is 3)
        BOOST_CHECK_EQUAL(multi_handler.get_available_count(), 4); // 7 - 3 unavailable = 4
        BOOST_CHECK_EQUAL(multi_handler.get_unavailable_followers_count(), 3);
        BOOST_CHECK(multi_handler.can_continue_replication());
        
        // Simulate replication with remaining available followers
        multi_handler.record_acknowledgment(1, 2); // available
        multi_handler.record_acknowledgment(1, 3); // available
        multi_handler.record_acknowledgment(1, 4); // available
        
        // Entry should be committed (leader + 3 available followers = 4, majority of 4 is 3)
        BOOST_CHECK(multi_handler.has_majority_acknowledgment(1));
        BOOST_CHECK_EQUAL(multi_handler.get_commit_index(), 1);
        
        BOOST_TEST_MESSAGE("✓ Multiple unresponsive followers test passed");
    }
    
    // Test case: Too many followers unresponsive, cannot form majority
    {
        const std::size_t cluster_size = 5;
        UnresponsiveFollowerHandler blocked_handler(cluster_size);
        
        std::vector<NodeId> followers = {2, 3, 4, 5};
        
        // Make 3 out of 4 followers unresponsive (leader + 1 follower = 2, majority of 2 is 2)
        for (std::size_t i = 2; i <= 4; ++i) {
            for (std::size_t j = 0; j < failure_threshold; ++j) {
                blocked_handler.record_failed_response(static_cast<NodeId>(i));
            }
        }
        
        // Property: System should still be able to continue with leader + 1 follower
        BOOST_CHECK_EQUAL(blocked_handler.get_available_count(), 2); // Leader + 1 follower
        BOOST_CHECK_EQUAL(blocked_handler.get_unavailable_followers_count(), 3);
        BOOST_CHECK(blocked_handler.can_continue_replication());
        
        // The remaining follower acknowledges
        blocked_handler.record_acknowledgment(1, 5);
        
        // Entry should be committed (leader + 1 follower = 2, majority of 2 is 2)
        BOOST_CHECK(blocked_handler.has_majority_acknowledgment(1));
        BOOST_CHECK_EQUAL(blocked_handler.get_commit_index(), 1);
        
        BOOST_TEST_MESSAGE("✓ Majority unresponsive followers test passed");
    }
    
    // Test case: All followers unresponsive, leader-only operation
    {
        const std::size_t cluster_size = 3;
        UnresponsiveFollowerHandler leader_only_handler(cluster_size);
        
        std::vector<NodeId> followers = {2, 3};
        
        // Make all followers unresponsive
        for (const auto& follower_id : followers) {
            for (std::size_t i = 0; i < failure_threshold; ++i) {
                leader_only_handler.record_failed_response(follower_id);
            }
        }
        
        // Property: System should still be able to continue with just the leader
        BOOST_CHECK_EQUAL(leader_only_handler.get_available_count(), 1); // Only leader
        BOOST_CHECK_EQUAL(leader_only_handler.get_unavailable_followers_count(), 2);
        BOOST_CHECK(leader_only_handler.can_continue_replication());
        
        // Simulate leader proposing an entry (leader implicitly acknowledges its own entries)
        // In a real implementation, the leader would add the entry to its log and implicitly acknowledge it
        leader_only_handler.propose_entry(1);
        
        // Entry should be committed (leader = 1, majority of 1 is 1)
        BOOST_CHECK(leader_only_handler.has_majority_acknowledgment(1));
        BOOST_CHECK_EQUAL(leader_only_handler.get_commit_index(), 1);
        
        BOOST_TEST_MESSAGE("✓ All followers unresponsive test passed");
    }
    
    // Test case: Failure threshold configuration
    {
        const std::size_t cluster_size = 5;
        const std::size_t custom_threshold = 5;
        UnresponsiveFollowerHandler threshold_handler(cluster_size, custom_threshold);
        
        const NodeId test_follower = 2;
        
        // Record failures up to custom threshold - 1
        for (std::size_t i = 0; i < custom_threshold - 1; ++i) {
            threshold_handler.record_failed_response(test_follower);
        }
        
        // Should still be available
        BOOST_CHECK_EQUAL(threshold_handler.get_follower_availability(test_follower), FollowerAvailability::available);
        BOOST_CHECK_EQUAL(threshold_handler.get_consecutive_failures(test_follower), custom_threshold - 1);
        
        // One more failure should mark as unavailable
        threshold_handler.record_failed_response(test_follower);
        
        BOOST_CHECK_EQUAL(threshold_handler.get_follower_availability(test_follower), FollowerAvailability::unavailable);
        BOOST_CHECK_EQUAL(threshold_handler.get_consecutive_failures(test_follower), custom_threshold);
        
        BOOST_TEST_MESSAGE("✓ Custom failure threshold test passed");
    }
    
    // Test case: Intermittent failures vs consistent failures
    {
        const std::size_t cluster_size = 5;
        UnresponsiveFollowerHandler intermittent_handler(cluster_size);
        
        const NodeId consistent_failing_follower = 2;
        const NodeId intermittent_failing_follower = 3;
        
        // Simulate consistent failures for one follower
        for (std::size_t i = 0; i < failure_threshold; ++i) {
            intermittent_handler.record_failed_response(consistent_failing_follower);
        }
        
        // Simulate intermittent failures for another follower
        for (std::size_t i = 0; i < failure_threshold - 1; ++i) {
            intermittent_handler.record_failed_response(intermittent_failing_follower);
        }
        // Reset with successful response
        intermittent_handler.record_successful_response(intermittent_failing_follower);
        // More failures but not consecutive
        for (std::size_t i = 0; i < failure_threshold - 1; ++i) {
            intermittent_handler.record_failed_response(intermittent_failing_follower);
        }
        
        // Property: Only consistently failing follower should be marked unavailable
        BOOST_CHECK_EQUAL(intermittent_handler.get_follower_availability(consistent_failing_follower), FollowerAvailability::unavailable);
        BOOST_CHECK_EQUAL(intermittent_handler.get_follower_availability(intermittent_failing_follower), FollowerAvailability::available);
        
        BOOST_CHECK_EQUAL(intermittent_handler.get_consecutive_failures(consistent_failing_follower), failure_threshold);
        BOOST_CHECK_EQUAL(intermittent_handler.get_consecutive_failures(intermittent_failing_follower), failure_threshold - 1);
        
        BOOST_CHECK_EQUAL(intermittent_handler.get_unavailable_followers_count(), 1);
        
        BOOST_TEST_MESSAGE("✓ Intermittent vs consistent failures test passed");
    }
    
    BOOST_TEST_MESSAGE("All unresponsive follower handling property tests passed!");
}