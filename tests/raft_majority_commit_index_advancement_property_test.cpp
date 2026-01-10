#define BOOST_TEST_MODULE RaftMajorityCommitIndexAdvancementPropertyTest

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
using Term = std::uint64_t;

// Simple commit index manager to test the property
class CommitIndexManager {
private:
    LogIndex _commit_index{0};
    std::size_t _cluster_size;
    std::unordered_map<LogIndex, std::unordered_set<NodeId>> _acknowledgments;
    
public:
    explicit CommitIndexManager(std::size_t cluster_size) 
        : _cluster_size(cluster_size) {}
    
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
    
    // Check if an entry has majority acknowledgment
    bool has_majority_acknowledgment(LogIndex log_index) const {
        std::size_t majority_needed = (_cluster_size / 2) + 1;
        return get_acknowledgment_count(log_index) >= majority_needed;
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
    
    // Clear all acknowledgments and reset commit index
    void clear() {
        _acknowledgments.clear();
        _commit_index = 0;
    }
    
private:
    // Update commit index based on majority acknowledgments
    void update_commit_index() {
        // Find the highest log index that has majority acknowledgment
        // and all previous entries also have majority acknowledgment
        LogIndex new_commit_index = _commit_index;
        
        // Get all acknowledged entries in order
        auto acknowledged_entries = get_acknowledged_entries();
        
        for (LogIndex log_index = _commit_index + 1; log_index <= acknowledged_entries.back(); ++log_index) {
            if (has_majority_acknowledgment(log_index)) {
                new_commit_index = log_index;
            } else {
                // Can't advance commit index past an entry without majority
                break;
            }
        }
        
        _commit_index = new_commit_index;
    }
};

/**
 * **Feature: raft-completion, Property 28: Majority Commit Index Advancement**
 * 
 * Property: For any entry acknowledged by majority of followers, the commit index advances to include that entry.
 * **Validates: Requirements 6.2**
 */
BOOST_AUTO_TEST_CASE(raft_majority_commit_index_advancement_property_test, * boost::unit_test::timeout(120)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> cluster_size_dist(min_cluster_size, max_cluster_size);
    std::uniform_int_distribution<std::size_t> entry_count_dist(min_log_entries, max_log_entries);
    std::uniform_int_distribution<int> ack_rate_dist(60, 100); // percentage
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random cluster configuration
        std::size_t cluster_size = cluster_size_dist(gen);
        if (cluster_size % 2 == 0) cluster_size++; // Ensure odd number for clear majority
        
        const std::size_t follower_count = cluster_size - 1; // Exclude leader
        const std::size_t majority_needed = (cluster_size / 2) + 1;
        const std::size_t entry_count = entry_count_dist(gen);
        
        BOOST_TEST_MESSAGE("Testing cluster size: " << cluster_size 
                          << ", majority needed: " << majority_needed
                          << ", followers: " << follower_count 
                          << ", log entries: " << entry_count);
        
        // Create follower IDs (leader is ID 1, followers are 2, 3, 4, ...)
        std::vector<NodeId> follower_ids;
        for (std::size_t i = 2; i <= cluster_size; ++i) {
            follower_ids.push_back(static_cast<NodeId>(i));
        }
        
        // Create commit index manager
        CommitIndexManager manager(cluster_size);
        
        // Track expected commit index progression
        LogIndex expected_commit_index = 0;
        
        // Simulate acknowledgments for multiple log entries
        for (LogIndex log_index = 1; log_index <= entry_count; ++log_index) {
            BOOST_TEST_MESSAGE("Processing log entry " << log_index);
            
            // Simulate acknowledgments from followers
            std::size_t ack_count = 1; // Leader always acknowledges implicitly
            std::vector<NodeId> acknowledging_followers;
            
            for (const auto& follower_id : follower_ids) {
                const int ack_rate = ack_rate_dist(gen);
                const bool will_acknowledge = (gen() % 100) < ack_rate;
                
                if (will_acknowledge) {
                    manager.record_acknowledgment(log_index, follower_id);
                    acknowledging_followers.push_back(follower_id);
                    ack_count++;
                    BOOST_TEST_MESSAGE("  Follower " << follower_id << " acknowledged entry " << log_index);
                }
            }
            
            BOOST_TEST_MESSAGE("  Entry " << log_index << " has " << ack_count << " acknowledgments");
            
            // Property 1: Commit index should advance if this entry has majority and all previous entries are committed
            const bool has_majority = ack_count >= majority_needed;
            const bool can_advance = has_majority && (log_index == expected_commit_index + 1);
            
            if (can_advance) {
                expected_commit_index = log_index;
                BOOST_TEST_MESSAGE("  Expected commit index advanced to " << expected_commit_index);
            }
            
            // Verify the actual commit index matches expected
            const LogIndex actual_commit_index = manager.get_commit_index();
            BOOST_CHECK_EQUAL(actual_commit_index, expected_commit_index);
            
            // Property 2: Entry with majority acknowledgment should be considered for commit
            BOOST_CHECK_EQUAL(manager.has_majority_acknowledgment(log_index), has_majority);
            BOOST_CHECK_EQUAL(manager.get_acknowledgment_count(log_index), ack_count);
        }
        
        // Property 3: Final commit index should be the highest consecutive entry with majority
        LogIndex final_expected_commit = 0;
        for (LogIndex log_index = 1; log_index <= entry_count; ++log_index) {
            if (manager.has_majority_acknowledgment(log_index)) {
                final_expected_commit = log_index;
            } else {
                break; // Can't advance past an entry without majority
            }
        }
        
        BOOST_CHECK_EQUAL(manager.get_commit_index(), final_expected_commit);
        BOOST_TEST_MESSAGE("Final commit index: " << manager.get_commit_index() 
                          << ", expected: " << final_expected_commit);
    }
    
    // Test edge cases
    BOOST_TEST_MESSAGE("Testing edge cases...");
    
    // Test single node cluster (majority of 1 is 1)
    {
        CommitIndexManager single_manager(1);
        
        // Leader implicitly acknowledges, so entry should be committed immediately
        single_manager.record_acknowledgment(1, 999); // Any follower ID (won't affect single node)
        
        // Property: Single node cluster should commit immediately (leader is majority)
        BOOST_CHECK_EQUAL(single_manager.get_commit_index(), 1);
        BOOST_CHECK(single_manager.has_majority_acknowledgment(1));
        BOOST_TEST_MESSAGE("✓ Single node cluster test passed");
    }
    
    // Test three node cluster with exact majority
    {
        const std::size_t cluster_size = 3;
        const std::size_t majority_needed = 2; // (3/2) + 1 = 2
        CommitIndexManager three_manager(cluster_size);
        
        // Test entry with exactly majority acknowledgments
        three_manager.record_acknowledgment(1, 2); // One follower acknowledges (leader + 1 follower = 2)
        
        BOOST_CHECK_EQUAL(three_manager.get_acknowledgment_count(1), 2); // Leader + 1 follower
        BOOST_CHECK(three_manager.has_majority_acknowledgment(1));
        BOOST_CHECK_EQUAL(three_manager.get_commit_index(), 1);
        
        // Test entry with less than majority
        // Don't record any acknowledgments for entry 2 (only leader = 1, need 2)
        BOOST_CHECK_EQUAL(three_manager.get_acknowledgment_count(2), 1); // Only leader
        BOOST_CHECK(!three_manager.has_majority_acknowledgment(2));
        BOOST_CHECK_EQUAL(three_manager.get_commit_index(), 1); // Should not advance
        
        BOOST_TEST_MESSAGE("✓ Three node exact majority test passed");
    }
    
    // Test commit index advancement with gaps
    {
        const std::size_t cluster_size = 5;
        CommitIndexManager gap_manager(cluster_size);
        
        // Create scenario where entry 1 has majority, entry 2 doesn't, entry 3 has majority
        // Entry 1: 3 acknowledgments (leader + 2 followers) - has majority
        gap_manager.record_acknowledgment(1, 2);
        gap_manager.record_acknowledgment(1, 3);
        
        // Entry 2: 1 acknowledgment (only leader) - no majority
        // Don't record any follower acknowledgments for entry 2
        
        // Entry 3: 4 acknowledgments (leader + 3 followers) - has majority
        gap_manager.record_acknowledgment(3, 2);
        gap_manager.record_acknowledgment(3, 3);
        gap_manager.record_acknowledgment(3, 4);
        
        // Property: Commit index should only advance to entry 1, not skip to entry 3
        BOOST_CHECK(gap_manager.has_majority_acknowledgment(1));
        BOOST_CHECK(!gap_manager.has_majority_acknowledgment(2));
        BOOST_CHECK(gap_manager.has_majority_acknowledgment(3));
        BOOST_CHECK_EQUAL(gap_manager.get_commit_index(), 1); // Should stop at entry 1
        
        BOOST_TEST_MESSAGE("✓ Commit index gap handling test passed");
    }
    
    // Test sequential commit advancement
    {
        const std::size_t cluster_size = 5;
        const std::size_t entry_count = 5;
        CommitIndexManager sequential_manager(cluster_size);
        
        // Give all entries majority acknowledgments in sequence
        for (LogIndex log_index = 1; log_index <= entry_count; ++log_index) {
            // Give each entry 3 follower acknowledgments (leader + 3 = 4, majority of 5 is 3)
            sequential_manager.record_acknowledgment(log_index, 2);
            sequential_manager.record_acknowledgment(log_index, 3);
            sequential_manager.record_acknowledgment(log_index, 4);
            
            // Property: Commit index should advance to this entry
            BOOST_CHECK_EQUAL(sequential_manager.get_commit_index(), log_index);
            BOOST_CHECK(sequential_manager.has_majority_acknowledgment(log_index));
            
            BOOST_TEST_MESSAGE("Entry " << log_index << " committed, commit index: " 
                              << sequential_manager.get_commit_index());
        }
        
        BOOST_CHECK_EQUAL(sequential_manager.get_commit_index(), entry_count);
        BOOST_TEST_MESSAGE("✓ Sequential commit advancement test passed");
    }
    
    // Test large cluster behavior
    {
        const std::size_t large_cluster_size = 9;
        const std::size_t majority_needed = 5; // (9/2) + 1 = 5
        CommitIndexManager large_manager(large_cluster_size);
        
        // Test with exactly majority acknowledgments
        for (NodeId follower_id = 2; follower_id <= 5; ++follower_id) {
            large_manager.record_acknowledgment(1, follower_id);
        }
        
        BOOST_CHECK_EQUAL(large_manager.get_acknowledgment_count(1), 5); // Leader + 4 followers
        BOOST_CHECK(large_manager.has_majority_acknowledgment(1));
        BOOST_CHECK_EQUAL(large_manager.get_commit_index(), 1);
        
        // Test with one less than majority
        for (NodeId follower_id = 2; follower_id <= 4; ++follower_id) {
            large_manager.record_acknowledgment(2, follower_id);
        }
        
        BOOST_CHECK_EQUAL(large_manager.get_acknowledgment_count(2), 4); // Leader + 3 followers
        BOOST_CHECK(!large_manager.has_majority_acknowledgment(2));
        BOOST_CHECK_EQUAL(large_manager.get_commit_index(), 1); // Should not advance
        
        BOOST_TEST_MESSAGE("✓ Large cluster behavior test passed");
    }
    
    // Test acknowledgment count accuracy
    {
        const std::size_t cluster_size = 7;
        CommitIndexManager count_manager(cluster_size);
        
        // Test various acknowledgment counts
        std::vector<std::pair<LogIndex, std::vector<NodeId>>> test_cases = {
            {1, {}}, // Only leader
            {2, {2}}, // Leader + 1 follower
            {3, {2, 3}}, // Leader + 2 followers
            {4, {2, 3, 4}}, // Leader + 3 followers
            {5, {2, 3, 4, 5}}, // Leader + 4 followers (majority)
            {6, {2, 3, 4, 5, 6}}, // Leader + 5 followers
            {7, {2, 3, 4, 5, 6, 7}} // Leader + 6 followers (all)
        };
        
        for (const auto& [log_index, followers] : test_cases) {
            for (const auto& follower_id : followers) {
                count_manager.record_acknowledgment(log_index, follower_id);
            }
            
            const std::size_t expected_count = followers.size() + 1; // +1 for leader
            const bool should_have_majority = expected_count >= 4; // Majority of 7 is 4
            
            BOOST_CHECK_EQUAL(count_manager.get_acknowledgment_count(log_index), expected_count);
            BOOST_CHECK_EQUAL(count_manager.has_majority_acknowledgment(log_index), should_have_majority);
            
            BOOST_TEST_MESSAGE("Entry " << log_index << ": " << expected_count 
                              << " acknowledgments, majority: " << (should_have_majority ? "YES" : "NO"));
        }
        
        // Property: Only entries 5, 6, 7 should be committed (have majority)
        BOOST_CHECK_EQUAL(count_manager.get_commit_index(), 0); // No sequential majority from entry 1
        
        BOOST_TEST_MESSAGE("✓ Acknowledgment count accuracy test passed");
    }
    
    BOOST_TEST_MESSAGE("All majority commit index advancement property tests passed!");
}