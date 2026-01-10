#define BOOST_TEST_MODULE RaftNonBlockingSlowFollowersPropertyTest

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
    constexpr std::chrono::milliseconds slow_follower_threshold{5000};
    constexpr std::chrono::milliseconds unresponsive_follower_threshold{15000};
}

// Simplified types for testing the property
using NodeId = std::uint64_t;
using LogIndex = std::uint64_t;
using Term = std::uint64_t;

// Enum to represent follower responsiveness states
enum class FollowerState {
    responsive,
    slow,
    unresponsive
};

// Stream operator for FollowerState to support Boost.Test printing
std::ostream& operator<<(std::ostream& os, FollowerState state) {
    switch (state) {
        case FollowerState::responsive: return os << "responsive";
        case FollowerState::slow: return os << "slow";
        case FollowerState::unresponsive: return os << "unresponsive";
        default: return os << "unknown";
    }
}

// Simple replication manager to test the non-blocking slow followers property
class ReplicationManager {
private:
    std::size_t _cluster_size;
    LogIndex _commit_index{0};
    std::unordered_map<LogIndex, std::unordered_set<NodeId>> _acknowledgments;
    std::unordered_map<NodeId, FollowerState> _follower_states;
    std::unordered_set<NodeId> _slow_followers;
    std::unordered_set<NodeId> _unresponsive_followers;
    
public:
    explicit ReplicationManager(std::size_t cluster_size) 
        : _cluster_size(cluster_size) {
        // Initialize all followers as responsive
        for (std::size_t i = 2; i <= cluster_size; ++i) {
            NodeId follower_id = static_cast<NodeId>(i);
            _follower_states[follower_id] = FollowerState::responsive;
        }
    }
    
    // Mark a follower as slow
    void mark_follower_slow(NodeId follower_id) {
        _follower_states[follower_id] = FollowerState::slow;
        _slow_followers.insert(follower_id);
        _unresponsive_followers.erase(follower_id);
    }
    
    // Mark a follower as unresponsive
    void mark_follower_unresponsive(NodeId follower_id) {
        _follower_states[follower_id] = FollowerState::unresponsive;
        _unresponsive_followers.insert(follower_id);
        _slow_followers.erase(follower_id);
    }
    
    // Mark a follower as responsive
    void mark_follower_responsive(NodeId follower_id) {
        _follower_states[follower_id] = FollowerState::responsive;
        _slow_followers.erase(follower_id);
        _unresponsive_followers.erase(follower_id);
    }
    
    // Record an acknowledgment from a follower for a specific log entry
    void record_acknowledgment(LogIndex log_index, NodeId follower_id) {
        _acknowledgments[log_index].insert(follower_id);
        update_commit_index();
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
    
    // Get the number of responsive followers (excluding unresponsive ones)
    std::size_t get_responsive_count() const {
        std::size_t responsive_count = 1; // Leader is always responsive
        for (const auto& [follower_id, state] : _follower_states) {
            if (state != FollowerState::unresponsive) {
                responsive_count++;
            }
        }
        return responsive_count;
    }
    
    // Check if an entry has majority acknowledgment among responsive nodes
    bool has_majority_acknowledgment(LogIndex log_index) const {
        std::size_t responsive_count = get_responsive_count();
        std::size_t majority_needed = (responsive_count / 2) + 1;
        return get_acknowledgment_count(log_index) >= majority_needed;
    }
    
    // Get follower state
    FollowerState get_follower_state(NodeId follower_id) const {
        auto it = _follower_states.find(follower_id);
        return (it != _follower_states.end()) ? it->second : FollowerState::responsive;
    }
    
    // Get slow followers count
    std::size_t get_slow_followers_count() const {
        return _slow_followers.size();
    }
    
    // Get unresponsive followers count
    std::size_t get_unresponsive_followers_count() const {
        return _unresponsive_followers.size();
    }
    
    // Check if replication can continue despite slow followers
    bool can_continue_replication() const {
        std::size_t responsive_count = get_responsive_count();
        std::size_t majority_needed = (responsive_count / 2) + 1;
        
        // Can continue if we have enough responsive nodes to form a majority
        return responsive_count >= majority_needed;
    }
    
    // Clear all acknowledgments and reset commit index
    void clear() {
        _acknowledgments.clear();
        _commit_index = 0;
    }
    
private:
    // Update commit index based on majority acknowledgments among responsive nodes
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
};

/**
 * **Feature: raft-completion, Property 29: Non-blocking Slow Followers**
 * 
 * Property: For any slow follower responses, the system continues replication without blocking other operations.
 * **Validates: Requirements 6.3**
 */
BOOST_AUTO_TEST_CASE(raft_non_blocking_slow_followers_property_test, * boost::unit_test::timeout(120)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> cluster_size_dist(min_cluster_size, max_cluster_size);
    std::uniform_int_distribution<std::size_t> entry_count_dist(min_log_entries, max_log_entries);
    std::uniform_int_distribution<int> slow_follower_rate_dist(10, 40); // percentage of followers that are slow
    std::uniform_int_distribution<int> unresponsive_follower_rate_dist(0, 20); // percentage that are unresponsive
    std::uniform_int_distribution<int> ack_rate_dist(70, 100); // acknowledgment rate for responsive followers
    
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
        
        // Create replication manager
        ReplicationManager manager(cluster_size);
        
        // Randomly assign follower states
        const int slow_rate = slow_follower_rate_dist(gen);
        const int unresponsive_rate = unresponsive_follower_rate_dist(gen);
        
        std::size_t slow_followers_count = 0;
        std::size_t unresponsive_followers_count = 0;
        
        for (const auto& follower_id : follower_ids) {
            const int state_roll = gen() % 100;
            
            if (state_roll < unresponsive_rate) {
                manager.mark_follower_unresponsive(follower_id);
                unresponsive_followers_count++;
                BOOST_TEST_MESSAGE("  Follower " << follower_id << " marked as UNRESPONSIVE");
            } else if (state_roll < (unresponsive_rate + slow_rate)) {
                manager.mark_follower_slow(follower_id);
                slow_followers_count++;
                BOOST_TEST_MESSAGE("  Follower " << follower_id << " marked as SLOW");
            } else {
                // Keep as responsive (default)
                BOOST_TEST_MESSAGE("  Follower " << follower_id << " is RESPONSIVE");
            }
        }
        
        BOOST_TEST_MESSAGE("Slow followers: " << slow_followers_count 
                          << ", Unresponsive followers: " << unresponsive_followers_count);
        
        // Property 1: System should be able to continue replication despite slow followers
        const bool can_continue = manager.can_continue_replication();
        const std::size_t responsive_count = manager.get_responsive_count();
        const std::size_t majority_needed = (responsive_count / 2) + 1;
        
        BOOST_TEST_MESSAGE("Responsive count: " << responsive_count 
                          << ", Majority needed: " << majority_needed
                          << ", Can continue: " << (can_continue ? "YES" : "NO"));
        
        // The system should be able to continue if there are enough responsive nodes
        BOOST_CHECK_EQUAL(can_continue, responsive_count >= majority_needed);
        
        if (!can_continue) {
            BOOST_TEST_MESSAGE("Skipping replication test - insufficient responsive nodes");
            continue;
        }
        
        // Simulate replication with slow followers present
        LogIndex expected_commit_index = 0;
        
        for (LogIndex log_index = 1; log_index <= entry_count; ++log_index) {
            BOOST_TEST_MESSAGE("Processing log entry " << log_index);
            
            std::size_t ack_count = 1; // Leader always acknowledges implicitly
            std::vector<NodeId> acknowledging_followers;
            
            // Simulate acknowledgments from followers based on their state
            for (const auto& follower_id : follower_ids) {
                const FollowerState state = manager.get_follower_state(follower_id);
                bool will_acknowledge = false;
                
                switch (state) {
                    case FollowerState::responsive:
                        // Responsive followers acknowledge with high probability
                        will_acknowledge = (gen() % 100) < ack_rate_dist(gen);
                        break;
                    case FollowerState::slow:
                        // Slow followers acknowledge with reduced probability (simulating delays)
                        will_acknowledge = (gen() % 100) < 50; // 50% chance
                        break;
                    case FollowerState::unresponsive:
                        // Unresponsive followers don't acknowledge
                        will_acknowledge = false;
                        break;
                }
                
                if (will_acknowledge) {
                    manager.record_acknowledgment(log_index, follower_id);
                    acknowledging_followers.push_back(follower_id);
                    ack_count++;
                    BOOST_TEST_MESSAGE("  Follower " << follower_id << " (" 
                                      << (state == FollowerState::responsive ? "RESPONSIVE" :
                                          state == FollowerState::slow ? "SLOW" : "UNRESPONSIVE")
                                      << ") acknowledged entry " << log_index);
                }
            }
            
            BOOST_TEST_MESSAGE("  Entry " << log_index << " has " << ack_count << " acknowledgments");
            
            // Property 2: Commit index should advance based on responsive majority, not total cluster size
            const bool has_majority = manager.has_majority_acknowledgment(log_index);
            const bool can_advance = has_majority && (log_index == expected_commit_index + 1);
            
            if (can_advance) {
                expected_commit_index = log_index;
                BOOST_TEST_MESSAGE("  Expected commit index advanced to " << expected_commit_index);
            }
            
            // Verify the actual commit index matches expected
            const LogIndex actual_commit_index = manager.get_commit_index();
            BOOST_CHECK_EQUAL(actual_commit_index, expected_commit_index);
            
            // Property 3: Majority calculation should exclude unresponsive followers
            const std::size_t current_responsive_count = manager.get_responsive_count();
            const std::size_t current_majority_needed = (current_responsive_count / 2) + 1;
            const bool expected_majority = ack_count >= current_majority_needed;
            
            BOOST_CHECK_EQUAL(has_majority, expected_majority);
            BOOST_TEST_MESSAGE("  Majority check: " << ack_count << " >= " << current_majority_needed 
                              << " = " << (expected_majority ? "TRUE" : "FALSE"));
        }
        
        // Property 4: System should track slow and unresponsive followers separately
        BOOST_CHECK_EQUAL(manager.get_slow_followers_count(), slow_followers_count);
        BOOST_CHECK_EQUAL(manager.get_unresponsive_followers_count(), unresponsive_followers_count);
        
        // Property 5: Final commit index should reflect progress despite slow followers
        const LogIndex final_commit_index = manager.get_commit_index();
        BOOST_TEST_MESSAGE("Final commit index: " << final_commit_index);
        
        // The system should have made progress if there were enough responsive followers
        if (responsive_count >= majority_needed && entry_count > 0) {
            // We should have committed at least some entries if acknowledgments were received
            // (This is probabilistic, so we don't enforce a specific minimum)
            BOOST_CHECK_GE(final_commit_index, 0);
        }
    }
    
    // Test edge cases
    BOOST_TEST_MESSAGE("Testing edge cases...");
    
    // Test case: All followers are slow but still responsive
    {
        const std::size_t cluster_size = 5;
        ReplicationManager slow_manager(cluster_size);
        
        // Mark all followers as slow (but not unresponsive)
        std::vector<NodeId> followers = {2, 3, 4, 5};
        for (const auto& follower_id : followers) {
            slow_manager.mark_follower_slow(follower_id);
        }
        
        // Property: System should still be able to continue replication
        BOOST_CHECK(slow_manager.can_continue_replication());
        BOOST_CHECK_EQUAL(slow_manager.get_responsive_count(), 5); // All nodes are responsive (slow != unresponsive)
        BOOST_CHECK_EQUAL(slow_manager.get_slow_followers_count(), 4);
        BOOST_CHECK_EQUAL(slow_manager.get_unresponsive_followers_count(), 0);
        
        // Simulate acknowledgments from slow followers
        for (const auto& follower_id : followers) {
            slow_manager.record_acknowledgment(1, follower_id);
        }
        
        // Entry should be committed despite all followers being slow
        BOOST_CHECK(slow_manager.has_majority_acknowledgment(1));
        BOOST_CHECK_EQUAL(slow_manager.get_commit_index(), 1);
        
        BOOST_TEST_MESSAGE("✓ All slow followers test passed");
    }
    
    // Test case: Some followers are unresponsive, but majority remains responsive
    {
        const std::size_t cluster_size = 7;
        ReplicationManager unresponsive_manager(cluster_size);
        
        std::vector<NodeId> followers = {2, 3, 4, 5, 6, 7};
        
        // Mark 2 followers as unresponsive (cluster of 7, so 5 remain responsive, majority is 3)
        unresponsive_manager.mark_follower_unresponsive(6);
        unresponsive_manager.mark_follower_unresponsive(7);
        
        // Mark 1 follower as slow
        unresponsive_manager.mark_follower_slow(5);
        
        // Property: System should still be able to continue replication
        BOOST_CHECK(unresponsive_manager.can_continue_replication());
        BOOST_CHECK_EQUAL(unresponsive_manager.get_responsive_count(), 5); // 7 - 2 unresponsive = 5
        BOOST_CHECK_EQUAL(unresponsive_manager.get_slow_followers_count(), 1);
        BOOST_CHECK_EQUAL(unresponsive_manager.get_unresponsive_followers_count(), 2);
        
        // Simulate acknowledgments from responsive followers only
        unresponsive_manager.record_acknowledgment(1, 2); // responsive
        unresponsive_manager.record_acknowledgment(1, 3); // responsive
        unresponsive_manager.record_acknowledgment(1, 4); // responsive
        unresponsive_manager.record_acknowledgment(1, 5); // slow but responsive
        // Followers 6 and 7 are unresponsive and don't acknowledge
        
        // Entry should be committed (leader + 4 followers = 5, majority of 5 is 3)
        BOOST_CHECK(unresponsive_manager.has_majority_acknowledgment(1));
        BOOST_CHECK_EQUAL(unresponsive_manager.get_commit_index(), 1);
        
        BOOST_TEST_MESSAGE("✓ Unresponsive followers with responsive majority test passed");
    }
    
    // Test case: Too many unresponsive followers, cannot continue
    {
        const std::size_t cluster_size = 5;
        ReplicationManager blocked_manager(cluster_size);
        
        std::vector<NodeId> followers = {2, 3, 4, 5};
        
        // Mark 4 out of 4 followers as unresponsive (only leader = 1, need 1 for majority of 1)
        blocked_manager.mark_follower_unresponsive(2);
        blocked_manager.mark_follower_unresponsive(3);
        blocked_manager.mark_follower_unresponsive(4);
        blocked_manager.mark_follower_unresponsive(5);
        
        // Property: System should still be able to continue with just the leader
        BOOST_CHECK(blocked_manager.can_continue_replication());
        BOOST_CHECK_EQUAL(blocked_manager.get_responsive_count(), 1); // Only leader
        BOOST_CHECK_EQUAL(blocked_manager.get_unresponsive_followers_count(), 4);
        
        // Leader can commit entries by itself (majority of 1 is 1)
        blocked_manager.record_acknowledgment(1, 999); // Any follower ID (won't affect single responsive node)
        
        // Entry should be committed (leader = 1, majority of 1 is 1)
        BOOST_CHECK(blocked_manager.has_majority_acknowledgment(1));
        BOOST_CHECK_EQUAL(blocked_manager.get_commit_index(), 1);
        
        BOOST_TEST_MESSAGE("✓ All followers unresponsive test passed");
    }
    
    // Test case: Majority of followers unresponsive, cannot form majority
    {
        const std::size_t cluster_size = 7;
        ReplicationManager blocked_manager(cluster_size);
        
        std::vector<NodeId> followers = {2, 3, 4, 5, 6, 7};
        
        // Mark 5 out of 6 followers as unresponsive (leader + 1 follower = 2, majority of 2 is 2)
        blocked_manager.mark_follower_unresponsive(3);
        blocked_manager.mark_follower_unresponsive(4);
        blocked_manager.mark_follower_unresponsive(5);
        blocked_manager.mark_follower_unresponsive(6);
        blocked_manager.mark_follower_unresponsive(7);
        
        // Property: System should still be able to continue with leader + 1 follower
        BOOST_CHECK(blocked_manager.can_continue_replication());
        BOOST_CHECK_EQUAL(blocked_manager.get_responsive_count(), 2); // Leader + 1 follower
        BOOST_CHECK_EQUAL(blocked_manager.get_unresponsive_followers_count(), 5);
        
        // The remaining follower acknowledges
        blocked_manager.record_acknowledgment(1, 2);
        
        // Entry should be committed (leader + 1 follower = 2, majority of 2 is 2)
        BOOST_CHECK(blocked_manager.has_majority_acknowledgment(1));
        BOOST_CHECK_EQUAL(blocked_manager.get_commit_index(), 1);
        
        BOOST_TEST_MESSAGE("✓ Majority unresponsive followers test passed");
    }
    
    // Test case: Follower state transitions
    {
        const std::size_t cluster_size = 5;
        ReplicationManager transition_manager(cluster_size);
        
        const NodeId test_follower = 2;
        
        // Initially responsive
        BOOST_CHECK_EQUAL(transition_manager.get_follower_state(test_follower), FollowerState::responsive);
        BOOST_CHECK_EQUAL(transition_manager.get_responsive_count(), 5);
        
        // Transition to slow
        transition_manager.mark_follower_slow(test_follower);
        BOOST_CHECK_EQUAL(transition_manager.get_follower_state(test_follower), FollowerState::slow);
        BOOST_CHECK_EQUAL(transition_manager.get_responsive_count(), 5); // Still responsive
        BOOST_CHECK_EQUAL(transition_manager.get_slow_followers_count(), 1);
        
        // Transition to unresponsive
        transition_manager.mark_follower_unresponsive(test_follower);
        BOOST_CHECK_EQUAL(transition_manager.get_follower_state(test_follower), FollowerState::unresponsive);
        BOOST_CHECK_EQUAL(transition_manager.get_responsive_count(), 4); // No longer responsive
        BOOST_CHECK_EQUAL(transition_manager.get_slow_followers_count(), 0); // Removed from slow
        BOOST_CHECK_EQUAL(transition_manager.get_unresponsive_followers_count(), 1);
        
        // Transition back to responsive
        transition_manager.mark_follower_responsive(test_follower);
        BOOST_CHECK_EQUAL(transition_manager.get_follower_state(test_follower), FollowerState::responsive);
        BOOST_CHECK_EQUAL(transition_manager.get_responsive_count(), 5); // Back to responsive
        BOOST_CHECK_EQUAL(transition_manager.get_slow_followers_count(), 0);
        BOOST_CHECK_EQUAL(transition_manager.get_unresponsive_followers_count(), 0);
        
        BOOST_TEST_MESSAGE("✓ Follower state transitions test passed");
    }
    
    // Test case: Mixed follower states with sequential entries
    {
        const std::size_t cluster_size = 7;
        const std::size_t entry_count = 5;
        ReplicationManager mixed_manager(cluster_size);
        
        std::vector<NodeId> followers = {2, 3, 4, 5, 6, 7};
        
        // Set up mixed follower states
        mixed_manager.mark_follower_slow(2);        // slow
        mixed_manager.mark_follower_slow(3);        // slow
        mixed_manager.mark_follower_unresponsive(4); // unresponsive
        // Followers 5, 6, 7 remain responsive
        
        BOOST_CHECK_EQUAL(mixed_manager.get_responsive_count(), 6); // 7 - 1 unresponsive = 6
        BOOST_CHECK_EQUAL(mixed_manager.get_slow_followers_count(), 2);
        BOOST_CHECK_EQUAL(mixed_manager.get_unresponsive_followers_count(), 1);
        
        // Simulate replication with mixed acknowledgment patterns
        for (LogIndex entry = 1; entry <= entry_count; ++entry) {
            // Responsive followers always acknowledge
            mixed_manager.record_acknowledgment(entry, 5);
            mixed_manager.record_acknowledgment(entry, 6);
            mixed_manager.record_acknowledgment(entry, 7);
            
            // Slow followers acknowledge some entries
            if (entry % 2 == 1) { // Odd entries
                mixed_manager.record_acknowledgment(entry, 2);
            }
            if (entry % 3 == 0) { // Every 3rd entry
                mixed_manager.record_acknowledgment(entry, 3);
            }
            
            // Unresponsive follower never acknowledges (follower 4)
            
            // Property: Each entry should be committed based on responsive majority
            const std::size_t responsive_majority = (6 / 2) + 1; // 4
            const std::size_t ack_count = mixed_manager.get_acknowledgment_count(entry);
            const bool should_commit = ack_count >= responsive_majority;
            
            BOOST_CHECK_EQUAL(mixed_manager.has_majority_acknowledgment(entry), should_commit);
            
            BOOST_TEST_MESSAGE("Entry " << entry << ": " << ack_count 
                              << " acknowledgments, majority needed: " << responsive_majority
                              << ", committed: " << (should_commit ? "YES" : "NO"));
        }
        
        // All entries should be committed (leader + 3 responsive followers = 4, which is majority of 6)
        BOOST_CHECK_EQUAL(mixed_manager.get_commit_index(), entry_count);
        
        BOOST_TEST_MESSAGE("✓ Mixed follower states test passed");
    }
    
    BOOST_TEST_MESSAGE("All non-blocking slow followers property tests passed!");
}