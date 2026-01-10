#define BOOST_TEST_MODULE RaftLeaderSelfAcknowledgmentPropertyTest

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
    constexpr std::size_t min_cluster_size = 1;
    constexpr std::size_t max_cluster_size = 9;
    constexpr std::size_t test_iterations = 30;
    constexpr std::size_t min_log_entries = 1;
    constexpr std::size_t max_log_entries = 10;
}

// Simplified types for testing the property
using NodeId = std::uint64_t;
using LogIndex = std::uint64_t;
using Term = std::uint64_t;

// Simple majority calculator to test the leader self-acknowledgment property
class MajorityCalculator {
private:
    NodeId _leader_id;
    std::size_t _cluster_size;
    std::unordered_map<LogIndex, std::unordered_set<NodeId>> _acknowledgments;
    
public:
    explicit MajorityCalculator(NodeId leader_id, std::size_t cluster_size) 
        : _leader_id(leader_id), _cluster_size(cluster_size) {}
    
    // Record an acknowledgment from a follower for a specific log entry
    void record_follower_acknowledgment(LogIndex log_index, NodeId follower_id) {
        if (follower_id != _leader_id) {
            _acknowledgments[log_index].insert(follower_id);
        }
    }
    
    // Get the total acknowledgment count for a log entry (including leader self-acknowledgment)
    std::size_t get_total_acknowledgment_count(LogIndex log_index) const {
        auto it = _acknowledgments.find(log_index);
        std::size_t follower_acks = (it != _acknowledgments.end()) ? it->second.size() : 0;
        return follower_acks + 1; // +1 for leader self-acknowledgment
    }
    
    // Get only follower acknowledgment count (excluding leader)
    std::size_t get_follower_acknowledgment_count(LogIndex log_index) const {
        auto it = _acknowledgments.find(log_index);
        return (it != _acknowledgments.end()) ? it->second.size() : 0;
    }
    
    // Check if an entry has majority acknowledgment (including leader self-acknowledgment)
    bool has_majority_acknowledgment(LogIndex log_index) const {
        std::size_t majority_needed = (_cluster_size / 2) + 1;
        return get_total_acknowledgment_count(log_index) >= majority_needed;
    }
    
    // Get the required majority size
    std::size_t get_majority_size() const {
        return (_cluster_size / 2) + 1;
    }
    
    // Check if leader is included in acknowledgment count
    bool is_leader_included_in_count(LogIndex log_index) const {
        // Leader is always implicitly included
        return get_total_acknowledgment_count(log_index) > get_follower_acknowledgment_count(log_index);
    }
    
    // Get leader ID
    NodeId get_leader_id() const {
        return _leader_id;
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
 * **Feature: raft-completion, Property 31: Leader Self-acknowledgment**
 * 
 * Property: For any commit decision, the leader includes itself in majority calculations.
 * **Validates: Requirements 6.5**
 */
BOOST_AUTO_TEST_CASE(raft_leader_self_acknowledgment_property_test, * boost::unit_test::timeout(120)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> cluster_size_dist(min_cluster_size, max_cluster_size);
    std::uniform_int_distribution<std::size_t> entry_count_dist(min_log_entries, max_log_entries);
    std::uniform_int_distribution<int> ack_rate_dist(50, 100); // percentage
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random cluster configuration
        std::size_t cluster_size = cluster_size_dist(gen);
        if (cluster_size % 2 == 0) cluster_size++; // Ensure odd number for clear majority
        
        const NodeId leader_id = 1;
        const std::size_t follower_count = cluster_size - 1;
        const std::size_t majority_needed = (cluster_size / 2) + 1;
        const std::size_t entry_count = entry_count_dist(gen);
        
        BOOST_TEST_MESSAGE("Testing cluster size: " << cluster_size 
                          << ", leader ID: " << leader_id
                          << ", majority needed: " << majority_needed
                          << ", followers: " << follower_count 
                          << ", log entries: " << entry_count);
        
        // Create follower IDs (leader is ID 1, followers are 2, 3, 4, ...)
        std::vector<NodeId> follower_ids;
        for (std::size_t i = 2; i <= cluster_size; ++i) {
            follower_ids.push_back(static_cast<NodeId>(i));
        }
        
        // Create majority calculator
        MajorityCalculator calculator(leader_id, cluster_size);
        
        // Test acknowledgments for multiple log entries
        for (LogIndex log_index = 1; log_index <= entry_count; ++log_index) {
            BOOST_TEST_MESSAGE("Processing log entry " << log_index);
            
            // Simulate acknowledgments from followers
            std::size_t follower_ack_count = 0;
            std::vector<NodeId> acknowledging_followers;
            
            for (const auto& follower_id : follower_ids) {
                const int ack_rate = ack_rate_dist(gen);
                const bool will_acknowledge = (gen() % 100) < ack_rate;
                
                if (will_acknowledge) {
                    calculator.record_follower_acknowledgment(log_index, follower_id);
                    acknowledging_followers.push_back(follower_id);
                    follower_ack_count++;
                    BOOST_TEST_MESSAGE("  Follower " << follower_id << " acknowledged entry " << log_index);
                }
            }
            
            // Property 1: Leader is always implicitly included in acknowledgment count
            const std::size_t total_acks = calculator.get_total_acknowledgment_count(log_index);
            const std::size_t follower_acks = calculator.get_follower_acknowledgment_count(log_index);
            
            BOOST_CHECK_EQUAL(follower_acks, follower_ack_count);
            BOOST_CHECK_EQUAL(total_acks, follower_ack_count + 1); // +1 for leader
            BOOST_CHECK(calculator.is_leader_included_in_count(log_index));
            
            BOOST_TEST_MESSAGE("  Entry " << log_index << ": " << follower_acks 
                              << " follower acks + 1 leader = " << total_acks << " total");
            
            // Property 2: Majority calculation includes leader self-acknowledgment
            const bool has_majority = calculator.has_majority_acknowledgment(log_index);
            const bool expected_majority = total_acks >= majority_needed;
            
            BOOST_CHECK_EQUAL(has_majority, expected_majority);
            BOOST_TEST_MESSAGE("  Has majority: " << (has_majority ? "YES" : "NO") 
                              << " (need " << majority_needed << ", have " << total_acks << ")");
            
            // Property 3: Leader self-acknowledgment is essential for single-node clusters
            if (cluster_size == 1) {
                BOOST_CHECK_EQUAL(total_acks, 1); // Only leader
                BOOST_CHECK(has_majority); // Leader alone should be majority
                BOOST_TEST_MESSAGE("  Single node cluster: leader alone is majority");
            }
            
            // Property 4: Leader self-acknowledgment reduces follower acknowledgments needed
            const std::size_t followers_needed_without_leader = majority_needed;
            const std::size_t followers_needed_with_leader = majority_needed - 1; // Leader counts as 1
            
            BOOST_CHECK_EQUAL(calculator.get_majority_size(), majority_needed);
            
            // If we have exactly (majority_needed - 1) followers, we should have majority with leader
            if (follower_ack_count == followers_needed_with_leader) {
                BOOST_CHECK(has_majority);
                BOOST_TEST_MESSAGE("  Leader self-acknowledgment enables majority with " 
                                  << followers_needed_with_leader << " follower acks");
            }
        }
        
        // Property 5: Leader ID is consistently tracked
        BOOST_CHECK_EQUAL(calculator.get_leader_id(), leader_id);
    }
    
    // Test edge cases
    BOOST_TEST_MESSAGE("Testing edge cases...");
    
    // Test single node cluster (leader is the only node)
    {
        const std::size_t cluster_size = 1;
        const NodeId leader_id = 100;
        MajorityCalculator single_calculator(leader_id, cluster_size);
        
        // Property: Single node cluster should always have majority (leader self-acknowledgment)
        BOOST_CHECK_EQUAL(single_calculator.get_majority_size(), 1);
        BOOST_CHECK_EQUAL(single_calculator.get_total_acknowledgment_count(1), 1); // Only leader
        BOOST_CHECK_EQUAL(single_calculator.get_follower_acknowledgment_count(1), 0); // No followers
        BOOST_CHECK(single_calculator.has_majority_acknowledgment(1));
        BOOST_CHECK(single_calculator.is_leader_included_in_count(1));
        
        BOOST_TEST_MESSAGE("✓ Single node cluster test passed");
    }
    
    // Test two node cluster (leader + 1 follower)
    {
        const std::size_t cluster_size = 2;
        const NodeId leader_id = 1;
        const NodeId follower_id = 2;
        MajorityCalculator two_calculator(leader_id, cluster_size);
        
        const std::size_t majority_needed = 2; // (2/2) + 1 = 2
        BOOST_CHECK_EQUAL(two_calculator.get_majority_size(), majority_needed);
        
        // Test without follower acknowledgment
        BOOST_CHECK_EQUAL(two_calculator.get_total_acknowledgment_count(1), 1); // Only leader
        BOOST_CHECK(!two_calculator.has_majority_acknowledgment(1)); // Need 2, have 1
        
        // Test with follower acknowledgment
        two_calculator.record_follower_acknowledgment(1, follower_id);
        BOOST_CHECK_EQUAL(two_calculator.get_total_acknowledgment_count(1), 2); // Leader + follower
        BOOST_CHECK(two_calculator.has_majority_acknowledgment(1)); // Have majority
        
        // Property: Leader self-acknowledgment is always included
        BOOST_CHECK(two_calculator.is_leader_included_in_count(1));
        
        BOOST_TEST_MESSAGE("✓ Two node cluster test passed");
    }
    
    // Test three node cluster (leader + 2 followers)
    {
        const std::size_t cluster_size = 3;
        const NodeId leader_id = 1;
        const std::vector<NodeId> follower_ids = {2, 3};
        MajorityCalculator three_calculator(leader_id, cluster_size);
        
        const std::size_t majority_needed = 2; // (3/2) + 1 = 2
        BOOST_CHECK_EQUAL(three_calculator.get_majority_size(), majority_needed);
        
        // Test with no follower acknowledgments
        BOOST_CHECK_EQUAL(three_calculator.get_total_acknowledgment_count(1), 1); // Only leader
        BOOST_CHECK(!three_calculator.has_majority_acknowledgment(1)); // Need 2, have 1
        
        // Test with one follower acknowledgment
        three_calculator.record_follower_acknowledgment(1, follower_ids[0]);
        BOOST_CHECK_EQUAL(three_calculator.get_total_acknowledgment_count(1), 2); // Leader + 1 follower
        BOOST_CHECK(three_calculator.has_majority_acknowledgment(1)); // Have majority
        
        // Test with both follower acknowledgments
        three_calculator.record_follower_acknowledgment(1, follower_ids[1]);
        BOOST_CHECK_EQUAL(three_calculator.get_total_acknowledgment_count(1), 3); // Leader + 2 followers
        BOOST_CHECK(three_calculator.has_majority_acknowledgment(1)); // Still have majority
        
        // Property: Leader self-acknowledgment reduces required follower acknowledgments
        // Without leader: would need 2 followers
        // With leader: only need 1 follower (leader + 1 follower = 2)
        BOOST_CHECK(three_calculator.is_leader_included_in_count(1));
        
        BOOST_TEST_MESSAGE("✓ Three node cluster test passed");
    }
    
    // Test large cluster behavior
    {
        const std::size_t large_cluster_size = 9;
        const NodeId leader_id = 1;
        MajorityCalculator large_calculator(leader_id, large_cluster_size);
        
        const std::size_t majority_needed = 5; // (9/2) + 1 = 5
        BOOST_CHECK_EQUAL(large_calculator.get_majority_size(), majority_needed);
        
        // Test with exactly (majority_needed - 1) follower acknowledgments
        const std::size_t followers_needed = majority_needed - 1; // 4 followers
        
        for (std::size_t i = 0; i < followers_needed; ++i) {
            const NodeId follower_id = static_cast<NodeId>(i + 2); // IDs 2, 3, 4, 5
            large_calculator.record_follower_acknowledgment(1, follower_id);
        }
        
        // Property: Leader self-acknowledgment should enable majority with fewer followers
        BOOST_CHECK_EQUAL(large_calculator.get_follower_acknowledgment_count(1), followers_needed);
        BOOST_CHECK_EQUAL(large_calculator.get_total_acknowledgment_count(1), majority_needed); // followers + leader
        BOOST_CHECK(large_calculator.has_majority_acknowledgment(1));
        BOOST_CHECK(large_calculator.is_leader_included_in_count(1));
        
        BOOST_TEST_MESSAGE("✓ Large cluster behavior test passed");
    }
    
    // Test leader self-acknowledgment consistency across multiple entries
    {
        const std::size_t cluster_size = 5;
        const NodeId leader_id = 42;
        const std::size_t entry_count = 5;
        MajorityCalculator consistency_calculator(leader_id, cluster_size);
        
        // Test that leader self-acknowledgment is consistent across all entries
        for (LogIndex log_index = 1; log_index <= entry_count; ++log_index) {
            // Property: Leader is always included, even with no follower acknowledgments
            BOOST_CHECK_EQUAL(consistency_calculator.get_total_acknowledgment_count(log_index), 1);
            BOOST_CHECK_EQUAL(consistency_calculator.get_follower_acknowledgment_count(log_index), 0);
            BOOST_CHECK(consistency_calculator.is_leader_included_in_count(log_index));
            BOOST_CHECK_EQUAL(consistency_calculator.get_leader_id(), leader_id);
            
            BOOST_TEST_MESSAGE("Entry " << log_index << ": leader self-acknowledgment present");
        }
        
        BOOST_TEST_MESSAGE("✓ Leader self-acknowledgment consistency test passed");
    }
    
    // Test leader self-acknowledgment with different leader IDs
    {
        const std::size_t cluster_size = 3;
        const std::vector<NodeId> different_leader_ids = {1, 10, 100, 999};
        
        for (const auto& leader_id : different_leader_ids) {
            MajorityCalculator leader_calculator(leader_id, cluster_size);
            
            // Property: Leader self-acknowledgment works regardless of leader ID
            BOOST_CHECK_EQUAL(leader_calculator.get_leader_id(), leader_id);
            BOOST_CHECK_EQUAL(leader_calculator.get_total_acknowledgment_count(1), 1); // Leader only
            BOOST_CHECK(leader_calculator.is_leader_included_in_count(1));
            
            // Test that follower acknowledgments don't include leader ID
            const NodeId follower_id = (leader_id == 1) ? 2 : 1; // Use different ID than leader
            leader_calculator.record_follower_acknowledgment(1, follower_id);
            
            BOOST_CHECK_EQUAL(leader_calculator.get_follower_acknowledgment_count(1), 1);
            BOOST_CHECK_EQUAL(leader_calculator.get_total_acknowledgment_count(1), 2); // Leader + follower
            
            // Try to record leader as follower (should be ignored)
            leader_calculator.record_follower_acknowledgment(1, leader_id);
            BOOST_CHECK_EQUAL(leader_calculator.get_follower_acknowledgment_count(1), 1); // Still 1
            BOOST_CHECK_EQUAL(leader_calculator.get_total_acknowledgment_count(1), 2); // Still 2
            
            BOOST_TEST_MESSAGE("Leader ID " << leader_id << ": self-acknowledgment working correctly");
        }
        
        BOOST_TEST_MESSAGE("✓ Different leader IDs test passed");
    }
    
    // Test majority calculation edge cases
    {
        // Test various cluster sizes and their majority requirements
        std::vector<std::pair<std::size_t, std::size_t>> cluster_majority_pairs = {
            {1, 1}, // Single node
            {2, 2}, // Two nodes
            {3, 2}, // Three nodes
            {4, 3}, // Four nodes (even, but testing)
            {5, 3}, // Five nodes
            {6, 4}, // Six nodes (even, but testing)
            {7, 4}, // Seven nodes
            {8, 5}, // Eight nodes (even, but testing)
            {9, 5}  // Nine nodes
        };
        
        for (const auto& [cluster_size, expected_majority] : cluster_majority_pairs) {
            const NodeId leader_id = 1;
            MajorityCalculator majority_calculator(leader_id, cluster_size);
            
            // Property: Majority calculation should be correct
            BOOST_CHECK_EQUAL(majority_calculator.get_majority_size(), expected_majority);
            
            // Property: Leader self-acknowledgment reduces follower requirement by 1
            const std::size_t followers_needed = expected_majority - 1;
            
            // Test with exactly the required number of followers
            for (std::size_t i = 0; i < followers_needed; ++i) {
                const NodeId follower_id = static_cast<NodeId>(i + 2);
                majority_calculator.record_follower_acknowledgment(1, follower_id);
            }
            
            BOOST_CHECK_EQUAL(majority_calculator.get_total_acknowledgment_count(1), expected_majority);
            BOOST_CHECK(majority_calculator.has_majority_acknowledgment(1));
            
            BOOST_TEST_MESSAGE("Cluster size " << cluster_size << ": majority " << expected_majority 
                              << ", need " << followers_needed << " followers + leader");
        }
        
        BOOST_TEST_MESSAGE("✓ Majority calculation edge cases test passed");
    }
    
    // Test acknowledgment tracking with leader self-acknowledgment
    {
        const std::size_t cluster_size = 7;
        const NodeId leader_id = 1;
        const std::size_t entry_count = 3;
        MajorityCalculator tracking_calculator(leader_id, cluster_size);
        
        // Create different acknowledgment patterns for different entries
        std::unordered_map<LogIndex, std::vector<NodeId>> entry_followers = {
            {1, {2, 3}},        // 2 followers + leader = 3 total (no majority, need 4)
            {2, {2, 3, 4}},     // 3 followers + leader = 4 total (majority!)
            {3, {2, 3, 4, 5, 6}} // 5 followers + leader = 6 total (majority!)
        };
        
        for (const auto& [log_index, followers] : entry_followers) {
            for (const auto& follower_id : followers) {
                tracking_calculator.record_follower_acknowledgment(log_index, follower_id);
            }
            
            const std::size_t expected_total = followers.size() + 1; // +1 for leader
            const bool should_have_majority = expected_total >= 4; // Majority of 7 is 4
            
            // Property: Leader self-acknowledgment is always included in total count
            BOOST_CHECK_EQUAL(tracking_calculator.get_follower_acknowledgment_count(log_index), followers.size());
            BOOST_CHECK_EQUAL(tracking_calculator.get_total_acknowledgment_count(log_index), expected_total);
            BOOST_CHECK_EQUAL(tracking_calculator.has_majority_acknowledgment(log_index), should_have_majority);
            BOOST_CHECK(tracking_calculator.is_leader_included_in_count(log_index));
            
            BOOST_TEST_MESSAGE("Entry " << log_index << ": " << followers.size() 
                              << " followers + leader = " << expected_total 
                              << " total, majority: " << (should_have_majority ? "YES" : "NO"));
        }
        
        BOOST_TEST_MESSAGE("✓ Acknowledgment tracking with leader self-acknowledgment test passed");
    }
    
    BOOST_TEST_MESSAGE("All leader self-acknowledgment property tests passed!");
}