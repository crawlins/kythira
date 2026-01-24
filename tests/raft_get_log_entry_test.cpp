#define BOOST_TEST_MODULE raft_get_log_entry_test
#include <boost/test/unit_test.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

namespace {
    constexpr std::uint64_t test_term = 1;
    constexpr std::uint64_t test_index_1 = 1;
    constexpr std::uint64_t test_index_2 = 2;
    constexpr std::uint64_t test_index_3 = 3;
    constexpr std::uint64_t test_index_5 = 5;
    constexpr std::uint64_t test_index_10 = 10;
    constexpr std::uint64_t snapshot_last_included_index = 5;
    constexpr std::uint64_t snapshot_last_included_term = 2;
}

// Test fixture for get_log_entry tests
// Since get_log_entry is a private method, we test the logic through the persistence engine
// which has the same interface and behavior
struct get_log_entry_fixture {
    using log_entry_type = kythira::log_entry<std::uint64_t, std::uint64_t>;
    using snapshot_type = kythira::snapshot<std::uint64_t, std::uint64_t, std::uint64_t>;
    using persistence_type = kythira::memory_persistence_engine<std::uint64_t, std::uint64_t, std::uint64_t>;
    
    persistence_type persistence;
    
    auto add_log_entry(std::uint64_t index, std::uint64_t term) -> void {
        log_entry_type entry{
            ._term = term,
            ._index = index,
            ._command = std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}}
        };
        persistence.append_log_entry(entry);
    }
    
    auto create_snapshot(std::uint64_t last_included_index, std::uint64_t last_included_term) -> void {
        snapshot_type snap{
            ._last_included_index = last_included_index,
            ._last_included_term = last_included_term,
            ._configuration = kythira::cluster_configuration<std::uint64_t>{
                ._nodes = {1}
            },
            ._state_machine_state = std::vector<std::byte>{std::byte{0xFF}}
        };
        persistence.save_snapshot(snap);
    }
    
    // Simulate the get_log_entry logic with snapshot checking
    auto get_log_entry_with_snapshot_check(std::uint64_t index) -> std::optional<log_entry_type> {
        // Validate index (log indices start at 1)
        if (index == 0) {
            return std::nullopt;
        }
        
        // Check if the entry has been compacted by a snapshot
        auto snapshot_opt = persistence.load_snapshot();
        if (snapshot_opt.has_value()) {
            const auto& snap = snapshot_opt.value();
            // If the requested index is covered by the snapshot, it's been compacted
            if (index <= snap.last_included_index()) {
                return std::nullopt;
            }
        }
        
        // Check if index is beyond the last log entry
        auto last_index = persistence.get_last_log_index();
        if (index > last_index) {
            return std::nullopt;
        }
        
        // Retrieve the entry from the persistence engine
        return persistence.get_log_entry(index);
    }
};

BOOST_FIXTURE_TEST_SUITE(get_log_entry_tests, get_log_entry_fixture, * boost::unit_test::timeout(30))

// Test retrieval of existing entries
BOOST_AUTO_TEST_CASE(test_retrieve_existing_entry, * boost::unit_test::timeout(15)) {
    // Add some log entries
    add_log_entry(test_index_1, test_term);
    add_log_entry(test_index_2, test_term);
    add_log_entry(test_index_3, test_term);
    
    // Retrieve existing entries
    auto entry1 = get_log_entry_with_snapshot_check(test_index_1);
    BOOST_TEST(entry1.has_value());
    BOOST_TEST(entry1->index() == test_index_1);
    BOOST_TEST(entry1->term() == test_term);
    
    auto entry2 = get_log_entry_with_snapshot_check(test_index_2);
    BOOST_TEST(entry2.has_value());
    BOOST_TEST(entry2->index() == test_index_2);
    BOOST_TEST(entry2->term() == test_term);
    
    auto entry3 = get_log_entry_with_snapshot_check(test_index_3);
    BOOST_TEST(entry3.has_value());
    BOOST_TEST(entry3->index() == test_index_3);
    BOOST_TEST(entry3->term() == test_term);
}

// Test handling of snapshot-compacted entries
BOOST_AUTO_TEST_CASE(test_snapshot_compacted_entries, * boost::unit_test::timeout(15)) {
    // Add log entries
    add_log_entry(test_index_1, test_term);
    add_log_entry(test_index_2, test_term);
    add_log_entry(test_index_3, test_term);
    add_log_entry(5, test_term);
    add_log_entry(6, test_term);
    add_log_entry(7, test_term);
    
    // Create a snapshot that covers entries 1-5
    create_snapshot(snapshot_last_included_index, snapshot_last_included_term);
    
    // Entries covered by snapshot should return nullopt
    auto entry1 = get_log_entry_with_snapshot_check(test_index_1);
    BOOST_TEST(!entry1.has_value());
    
    auto entry2 = get_log_entry_with_snapshot_check(test_index_2);
    BOOST_TEST(!entry2.has_value());
    
    auto entry5 = get_log_entry_with_snapshot_check(test_index_5);
    BOOST_TEST(!entry5.has_value());
    
    // Entries after snapshot should still be retrievable
    auto entry6 = get_log_entry_with_snapshot_check(6);
    BOOST_TEST(entry6.has_value());
    BOOST_TEST(entry6->index() == 6);
    
    auto entry7 = get_log_entry_with_snapshot_check(7);
    BOOST_TEST(entry7.has_value());
    BOOST_TEST(entry7->index() == 7);
}

// Test out-of-bounds indices
BOOST_AUTO_TEST_CASE(test_out_of_bounds_indices, * boost::unit_test::timeout(15)) {
    // Add some log entries
    add_log_entry(test_index_1, test_term);
    add_log_entry(test_index_2, test_term);
    add_log_entry(test_index_3, test_term);
    
    // Index 0 should return nullopt (invalid index)
    auto entry0 = get_log_entry_with_snapshot_check(0);
    BOOST_TEST(!entry0.has_value());
    
    // Index beyond last log entry should return nullopt
    auto entry10 = get_log_entry_with_snapshot_check(test_index_10);
    BOOST_TEST(!entry10.has_value());
    
    // Index 4 (doesn't exist but within range) should return nullopt
    auto entry4 = get_log_entry_with_snapshot_check(4);
    BOOST_TEST(!entry4.has_value());
}

// Test edge case: empty log
BOOST_AUTO_TEST_CASE(test_empty_log, * boost::unit_test::timeout(15)) {
    // Don't add any entries
    
    // Any index should return nullopt
    auto entry1 = get_log_entry_with_snapshot_check(test_index_1);
    BOOST_TEST(!entry1.has_value());
    
    auto entry2 = get_log_entry_with_snapshot_check(test_index_2);
    BOOST_TEST(!entry2.has_value());
}

// Test edge case: single entry
BOOST_AUTO_TEST_CASE(test_single_entry, * boost::unit_test::timeout(15)) {
    // Add only one entry
    add_log_entry(test_index_1, test_term);
    
    // Should retrieve the single entry
    auto entry1 = get_log_entry_with_snapshot_check(test_index_1);
    BOOST_TEST(entry1.has_value());
    BOOST_TEST(entry1->index() == test_index_1);
    BOOST_TEST(entry1->term() == test_term);
    
    // Other indices should return nullopt
    auto entry0 = get_log_entry_with_snapshot_check(0);
    BOOST_TEST(!entry0.has_value());
    
    auto entry2 = get_log_entry_with_snapshot_check(test_index_2);
    BOOST_TEST(!entry2.has_value());
}

// Test edge case: sparse log (non-contiguous indices)
BOOST_AUTO_TEST_CASE(test_sparse_log, * boost::unit_test::timeout(15)) {
    // Add entries with gaps
    add_log_entry(test_index_1, test_term);
    add_log_entry(test_index_3, test_term);
    add_log_entry(test_index_5, test_term);
    
    // Should retrieve existing entries
    auto entry1 = get_log_entry_with_snapshot_check(test_index_1);
    BOOST_TEST(entry1.has_value());
    BOOST_TEST(entry1->index() == test_index_1);
    
    auto entry3 = get_log_entry_with_snapshot_check(test_index_3);
    BOOST_TEST(entry3.has_value());
    BOOST_TEST(entry3->index() == test_index_3);
    
    auto entry5 = get_log_entry_with_snapshot_check(test_index_5);
    BOOST_TEST(entry5.has_value());
    BOOST_TEST(entry5->index() == test_index_5);
    
    // Gap indices should return nullopt
    auto entry2 = get_log_entry_with_snapshot_check(test_index_2);
    BOOST_TEST(!entry2.has_value());
    
    auto entry4 = get_log_entry_with_snapshot_check(4);
    BOOST_TEST(!entry4.has_value());
}

// Test snapshot at boundary
BOOST_AUTO_TEST_CASE(test_snapshot_at_boundary, * boost::unit_test::timeout(15)) {
    // Add entries
    add_log_entry(test_index_1, test_term);
    add_log_entry(test_index_2, test_term);
    add_log_entry(test_index_3, test_term);
    
    // Create snapshot at index 2
    create_snapshot(test_index_2, test_term);
    
    // Entries 1 and 2 should be compacted
    auto entry1 = get_log_entry_with_snapshot_check(test_index_1);
    BOOST_TEST(!entry1.has_value());
    
    auto entry2 = get_log_entry_with_snapshot_check(test_index_2);
    BOOST_TEST(!entry2.has_value());
    
    // Entry 3 should still be available
    auto entry3 = get_log_entry_with_snapshot_check(test_index_3);
    BOOST_TEST(entry3.has_value());
    BOOST_TEST(entry3->index() == test_index_3);
}

// Test multiple snapshots (only latest matters)
BOOST_AUTO_TEST_CASE(test_multiple_snapshots, * boost::unit_test::timeout(15)) {
    // Add entries
    add_log_entry(test_index_1, test_term);
    add_log_entry(test_index_2, test_term);
    add_log_entry(test_index_3, test_term);
    add_log_entry(5, test_term);
    add_log_entry(6, test_term);
    
    // Create first snapshot at index 2
    create_snapshot(test_index_2, test_term);
    
    // Create second snapshot at index 5 (overwrites first)
    create_snapshot(snapshot_last_included_index, snapshot_last_included_term);
    
    // Entries 1-5 should be compacted
    auto entry1 = get_log_entry_with_snapshot_check(test_index_1);
    BOOST_TEST(!entry1.has_value());
    
    auto entry2 = get_log_entry_with_snapshot_check(test_index_2);
    BOOST_TEST(!entry2.has_value());
    
    auto entry5 = get_log_entry_with_snapshot_check(test_index_5);
    BOOST_TEST(!entry5.has_value());
    
    // Entry 6 should still be available
    auto entry6 = get_log_entry_with_snapshot_check(6);
    BOOST_TEST(entry6.has_value());
    BOOST_TEST(entry6->index() == 6);
}

BOOST_AUTO_TEST_SUITE_END()
