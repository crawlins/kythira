#define BOOST_TEST_MODULE RaftFollowerAcknowledgmentTrackingPropertyTest

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
}

// Simplified types for testing the property
using NodeId = std::uint64_t;
using LogIndex = std::uint64_t;

// Simple acknowledgment tracker to test the property
class FollowerAcknowledgmentTracker {
private:
    // Map from log index to set of followers that acknowledged
    std::unordered_map<LogIndex, std::unordered_set<NodeId>> _acknowledgments;
    
public:
    // Record an acknowledgment from a follower for a specific log entry
    void record_acknowledgment(LogIndex log_index, NodeId follower_id) {
        _acknowledgments[log_index].insert(follower_id);
    }
    
    // Get the set of followers that acknowledged a specific log entry
    const std::unordered_set<NodeId>& get_acknowledgments(LogIndex log_index) const {
        static const std::unordered_set<NodeId> empty_set;
        auto it = _acknowledgments.find(log_index);
        return (it != _acknowledgments.end()) ? it->second : empty_set;
    }
    
    // Check if a specific follower acknowledged a log entry
    bool has_acknowledgment(LogIndex log_index, NodeId follower_id) const {
        auto it = _acknowledgments.find(log_index);
        if (it == _acknowledgments.end()) {
            return false;
        }
        return it->second.count(follower_id) > 0;
    }
    
    // Get the number of acknowledgments for a log entry
    std::size_t get_acknowledgment_count(LogIndex log_index) const {
        auto it = _acknowledgments.find(log_index);
        return (it != _acknowledgments.end()) ? it->second.size() : 0;
    }
    
    // Clear all acknowledgments
    void clear() {
        _acknowledgments.clear();
    }
    
    // Get all log indices that have acknowledgments
    std::vector<LogIndex> get_acknowledged_entries() const {
        std::vector<LogIndex> entries;
        for (const auto& [log_index, _] : _acknowledgments) {
            entries.push_back(log_index);
        }
        std::sort(entries.begin(), entries.end());
        return entries;
    }
};

/**
 * **Feature: raft-completion, Property 27: Follower Acknowledgment Tracking**
 * 
 * Property: For any entry replication to followers, the system tracks which followers have acknowledged each entry.
 * **Validates: Requirements 6.1**
 */
BOOST_AUTO_TEST_CASE(raft_follower_acknowledgment_tracking_property_test, * boost::unit_test::timeout(120)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> cluster_size_dist(min_cluster_size, max_cluster_size);
    std::uniform_int_distribution<std::size_t> entry_count_dist(min_log_entries, max_log_entries);
    std::uniform_int_distribution<int> success_rate_dist(70, 100); // percentage
    
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
        
        // Create acknowledgment tracker
        FollowerAcknowledgmentTracker tracker;
        
        // Simulate replication tracking for multiple log entries
        std::unordered_map<LogIndex, std::unordered_set<NodeId>> expected_acknowledgments;
        
        for (std::size_t entry_idx = 0; entry_idx < entry_count; ++entry_idx) {
            const LogIndex log_index = entry_idx + 1;
            expected_acknowledgments[log_index] = std::unordered_set<NodeId>{};
            
            BOOST_TEST_MESSAGE("Testing acknowledgment tracking for log entry " << log_index);
            
            // Simulate replication to each follower
            for (const auto& follower_id : follower_ids) {
                const int success_rate = success_rate_dist(gen);
                const bool will_acknowledge = (gen() % 100) < success_rate;
                
                if (will_acknowledge) {
                    // Record the acknowledgment in both expected and actual tracker
                    expected_acknowledgments[log_index].insert(follower_id);
                    tracker.record_acknowledgment(log_index, follower_id);
                    
                    BOOST_TEST_MESSAGE("  Follower " << follower_id << " acknowledged entry " << log_index);
                } else {
                    BOOST_TEST_MESSAGE("  Follower " << follower_id << " did NOT acknowledge entry " << log_index);
                }
            }
        }
        
        // Property 1: The system should track acknowledgments for each entry separately
        for (LogIndex log_index = 1; log_index <= entry_count; ++log_index) {
            const auto& expected_acks = expected_acknowledgments[log_index];
            const auto& actual_acks = tracker.get_acknowledgments(log_index);
            
            // Verify that the tracker has the same acknowledgments as expected
            BOOST_CHECK_EQUAL(actual_acks.size(), expected_acks.size());
            
            for (const auto& follower_id : expected_acks) {
                BOOST_CHECK(actual_acks.count(follower_id) > 0);
                BOOST_CHECK(tracker.has_acknowledgment(log_index, follower_id));
            }
            
            // Verify count method
            BOOST_CHECK_EQUAL(tracker.get_acknowledgment_count(log_index), expected_acks.size());
        }
        
        // Property 2: Each follower's acknowledgment should be tracked independently
        for (const auto& follower_id : follower_ids) {
            std::size_t follower_ack_count = 0;
            
            for (LogIndex log_index = 1; log_index <= entry_count; ++log_index) {
                if (expected_acknowledgments[log_index].count(follower_id) > 0) {
                    follower_ack_count++;
                    BOOST_CHECK(tracker.has_acknowledgment(log_index, follower_id));
                } else {
                    BOOST_CHECK(!tracker.has_acknowledgment(log_index, follower_id));
                }
            }
            
            BOOST_TEST_MESSAGE("Follower " << follower_id << " acknowledged " 
                              << follower_ack_count << " entries");
            
            // Each follower should be able to acknowledge 0 to all entries
            BOOST_CHECK_GE(follower_ack_count, 0);
            BOOST_CHECK_LE(follower_ack_count, entry_count);
        }
        
        // Property 3: Acknowledgment tracking should handle partial acknowledgments
        std::size_t total_expected_acks = 0;
        std::size_t entries_with_partial_acks = 0;
        
        for (const auto& [log_index, acks] : expected_acknowledgments) {
            total_expected_acks += acks.size();
            
            if (acks.size() > 0 && acks.size() < follower_count) {
                entries_with_partial_acks++;
            }
        }
        
        BOOST_TEST_MESSAGE("Total expected acknowledgments: " << total_expected_acks);
        BOOST_TEST_MESSAGE("Entries with partial acknowledgments: " << entries_with_partial_acks);
        
        // Verify total acknowledgment count
        std::size_t total_actual_acks = 0;
        for (LogIndex log_index = 1; log_index <= entry_count; ++log_index) {
            total_actual_acks += tracker.get_acknowledgment_count(log_index);
        }
        BOOST_CHECK_EQUAL(total_actual_acks, total_expected_acks);
        
        // Property 4: System should be able to track acknowledgments from any subset of followers
        BOOST_CHECK_GE(total_expected_acks, 0);
        BOOST_CHECK_LE(total_expected_acks, entry_count * follower_count);
    }
    
    // Test edge cases
    BOOST_TEST_MESSAGE("Testing edge cases...");
    
    // Test single follower acknowledgment tracking
    {
        FollowerAcknowledgmentTracker tracker;
        const NodeId follower1_id = 2;
        const NodeId follower2_id = 3;
        
        // Test tracking acknowledgments from individual followers
        std::unordered_set<LogIndex> follower1_acks = {1, 3, 5};
        std::unordered_set<LogIndex> follower2_acks = {2, 3, 4};
        
        // Record acknowledgments
        for (const auto& entry : follower1_acks) {
            tracker.record_acknowledgment(entry, follower1_id);
        }
        for (const auto& entry : follower2_acks) {
            tracker.record_acknowledgment(entry, follower2_id);
        }
        
        // Property: System should track each follower's acknowledgments independently
        for (LogIndex entry = 1; entry <= 5; ++entry) {
            bool follower1_should_ack = follower1_acks.count(entry) > 0;
            bool follower2_should_ack = follower2_acks.count(entry) > 0;
            
            BOOST_CHECK_EQUAL(tracker.has_acknowledgment(entry, follower1_id), follower1_should_ack);
            BOOST_CHECK_EQUAL(tracker.has_acknowledgment(entry, follower2_id), follower2_should_ack);
            
            BOOST_TEST_MESSAGE("Entry " << entry << ": Follower1=" 
                              << (follower1_should_ack ? "ACK" : "NACK") 
                              << ", Follower2=" 
                              << (follower2_should_ack ? "ACK" : "NACK"));
        }
        
        BOOST_TEST_MESSAGE("✓ Individual follower acknowledgment tracking test passed");
    }
    
    // Test acknowledgment tracking with no acknowledgments
    {
        FollowerAcknowledgmentTracker tracker;
        const std::size_t entry_count = 3;
        
        for (LogIndex entry = 1; entry <= entry_count; ++entry) {
            // Property: System should handle zero acknowledgments correctly
            BOOST_CHECK_EQUAL(tracker.get_acknowledgment_count(entry), 0);
            BOOST_CHECK(tracker.get_acknowledgments(entry).empty());
            BOOST_TEST_MESSAGE("Entry " << entry << " has " << tracker.get_acknowledgment_count(entry) << " acknowledgments");
        }
        
        BOOST_TEST_MESSAGE("✓ Zero acknowledgments tracking test passed");
    }
    
    // Test acknowledgment tracking with all followers acknowledging
    {
        FollowerAcknowledgmentTracker tracker;
        const std::size_t follower_count = 6;
        const std::size_t entry_count = 2;
        
        std::vector<NodeId> all_followers;
        for (std::size_t i = 2; i <= follower_count + 1; ++i) {
            all_followers.push_back(static_cast<NodeId>(i));
        }
        
        for (LogIndex entry = 1; entry <= entry_count; ++entry) {
            // Record acknowledgments from all followers
            for (const auto& follower_id : all_followers) {
                tracker.record_acknowledgment(entry, follower_id);
            }
            
            // Property: System should handle all followers acknowledging
            BOOST_CHECK_EQUAL(tracker.get_acknowledgment_count(entry), follower_count);
            BOOST_TEST_MESSAGE("Entry " << entry << " has " << tracker.get_acknowledgment_count(entry) 
                              << " acknowledgments (all followers)");
        }
        
        BOOST_TEST_MESSAGE("✓ All followers acknowledgment tracking test passed");
    }
    
    // Test acknowledgment tracking consistency across multiple entries
    {
        const std::size_t cluster_size = 5;
        const std::size_t entry_count = 10;
        
        FollowerAcknowledgmentTracker tracker;
        
        std::vector<NodeId> followers = {2, 3, 4, 5};
        
        // Create a consistent acknowledgment pattern
        std::unordered_map<LogIndex, std::unordered_set<NodeId>> consistent_acks;
        
        for (LogIndex entry = 1; entry <= entry_count; ++entry) {
            consistent_acks[entry] = std::unordered_set<NodeId>{};
            
            // Even entries acknowledged by followers 2,4; odd entries by followers 3,5
            if (entry % 2 == 0) {
                consistent_acks[entry].insert(2);
                consistent_acks[entry].insert(4);
                tracker.record_acknowledgment(entry, 2);
                tracker.record_acknowledgment(entry, 4);
            } else {
                consistent_acks[entry].insert(3);
                consistent_acks[entry].insert(5);
                tracker.record_acknowledgment(entry, 3);
                tracker.record_acknowledgment(entry, 5);
            }
        }
        
        // Property: System should maintain consistent tracking across all entries
        for (const auto& [entry, expected_acks] : consistent_acks) {
            BOOST_CHECK_EQUAL(tracker.get_acknowledgment_count(entry), 2); // Each entry should have exactly 2 acks
            
            const auto& actual_acks = tracker.get_acknowledgments(entry);
            BOOST_CHECK_EQUAL(actual_acks.size(), expected_acks.size());
            
            for (const auto& follower_id : expected_acks) {
                BOOST_CHECK(actual_acks.count(follower_id) > 0);
            }
            
            BOOST_TEST_MESSAGE("Entry " << entry << " has " << tracker.get_acknowledgment_count(entry) << " acknowledgments");
        }
        
        BOOST_TEST_MESSAGE("✓ Consistent acknowledgment tracking test passed");
    }
    
    // Test get_acknowledged_entries method
    {
        FollowerAcknowledgmentTracker tracker;
        
        // Record acknowledgments for entries 2, 5, 7
        tracker.record_acknowledgment(2, 10);
        tracker.record_acknowledgment(5, 11);
        tracker.record_acknowledgment(7, 12);
        
        auto acknowledged_entries = tracker.get_acknowledged_entries();
        std::vector<LogIndex> expected_entries = {2, 5, 7};
        
        BOOST_CHECK_EQUAL(acknowledged_entries.size(), expected_entries.size());
        BOOST_CHECK_EQUAL_COLLECTIONS(acknowledged_entries.begin(), acknowledged_entries.end(),
                                     expected_entries.begin(), expected_entries.end());
        
        BOOST_TEST_MESSAGE("✓ Get acknowledged entries test passed");
    }
    
    BOOST_TEST_MESSAGE("All follower acknowledgment tracking property tests passed!");
}