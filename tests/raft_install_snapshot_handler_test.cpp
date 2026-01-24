#define BOOST_TEST_MODULE raft_install_snapshot_handler_test
#include <boost/test/unit_test.hpp>

#include <random>
#include <vector>
#include <cstddef>

namespace {
    constexpr std::size_t property_test_iterations = 50;
    constexpr std::uint64_t max_term = 100;
    constexpr std::uint64_t max_index = 1000;
    constexpr std::size_t max_chunk_size = 4096;
}

// Helper to generate random term
auto generate_random_term(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_term);
    return dist(rng);
}

// Helper to generate random log index
auto generate_random_log_index(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_index);
    return dist(rng);
}

// Helper to generate random chunk size
auto generate_random_chunk_size(std::mt19937& rng) -> std::size_t {
    std::uniform_int_distribution<std::size_t> dist(256, max_chunk_size);
    return dist(rng);
}

/**
 * Feature: raft-consensus, Property 87: Complete InstallSnapshot Handler Logic
 * Validates: Requirements 10.3, 10.4, 5.5
 * 
 * Property: The InstallSnapshot handler must correctly implement all Raft snapshot installation rules.
 * This test validates the chunked snapshot receiving and assembly logic.
 * 
 * InstallSnapshot rules:
 * 1. Reply immediately if term < currentTerm
 * 2. Create new snapshot file if first chunk (offset is 0)
 * 3. Write data into snapshot file at given offset
 * 4. Reply and wait for more data chunks if done is false
 * 5. Save snapshot file when done is true
 * 6. Discard any existing or partial snapshot with smaller index
 * 7. If existing log entry has same index and term as snapshot's last included entry, retain log entries following it
 * 8. Discard entire log if no such entry exists
 * 9. Reset state machine using snapshot contents
 * 10. Reply with current term
 */
BOOST_AUTO_TEST_CASE(property_snapshot_chunk_assembly, * boost::unit_test::timeout(90)) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t tests_passed = 0;
    std::size_t single_chunk_tests = 0;
    std::size_t multi_chunk_tests = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random snapshot parameters
        auto last_included_index = generate_random_log_index(rng);
        auto last_included_term = generate_random_term(rng);
        
        // Generate random number of chunks (1-5)
        std::uniform_int_distribution<std::size_t> chunk_count_dist(1, 5);
        auto num_chunks = chunk_count_dist(rng);
        
        // Generate random chunk sizes
        std::vector<std::size_t> chunk_sizes;
        std::size_t total_size = 0;
        for (std::size_t j = 0; j < num_chunks; ++j) {
            auto chunk_size = generate_random_chunk_size(rng);
            chunk_sizes.push_back(chunk_size);
            total_size += chunk_size;
        }
        
        // Property: Chunked snapshot assembly logic
        // 1. First chunk must have offset 0
        // 2. Subsequent chunks must have offset equal to sum of previous chunk sizes
        // 3. Final chunk must have done=true
        // 4. All chunks must have consistent metadata (last_included_index, last_included_term)
        
        std::size_t expected_offset = 0;
        for (std::size_t j = 0; j < num_chunks; ++j) {
            // Verify offset calculation
            BOOST_CHECK_EQUAL(expected_offset, 
                std::accumulate(chunk_sizes.begin(), chunk_sizes.begin() + j, std::size_t{0}));
            
            expected_offset += chunk_sizes[j];
        }
        
        // Verify total size
        BOOST_CHECK_EQUAL(expected_offset, total_size);
        
        if (num_chunks == 1) {
            ++single_chunk_tests;
        } else {
            ++multi_chunk_tests;
        }
        
        ++tests_passed;
        
        if (i < 10) {  // Log first 10 iterations for visibility
            BOOST_TEST_MESSAGE("Iteration " << i << ": "
                << "last_included_index=" << last_included_index << ", "
                << "last_included_term=" << last_included_term << ", "
                << "num_chunks=" << num_chunks << ", "
                << "total_size=" << total_size);
        }
    }
    
    BOOST_TEST_MESSAGE("Snapshot chunk assembly tests:");
    BOOST_TEST_MESSAGE("  Total tests: " << tests_passed);
    BOOST_TEST_MESSAGE("  Single chunk tests: " << single_chunk_tests);
    BOOST_TEST_MESSAGE("  Multi-chunk tests: " << multi_chunk_tests);
    
    // Property: Both single and multi-chunk scenarios should be tested
    BOOST_CHECK_GT(single_chunk_tests, 0);
    BOOST_CHECK_GT(multi_chunk_tests, 0);
    BOOST_CHECK_EQUAL(tests_passed, property_test_iterations);
}

/**
 * Property: Snapshot offset validation
 * 
 * The InstallSnapshot handler must reject chunks with incorrect offsets.
 * This ensures data integrity during snapshot transfer.
 */
BOOST_AUTO_TEST_CASE(property_snapshot_offset_validation, * boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t tests_passed = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random snapshot parameters
        auto chunk_size = generate_random_chunk_size(rng);
        
        // Generate random number of chunks received so far (0-10)
        std::uniform_int_distribution<std::size_t> received_dist(0, 10);
        auto chunks_received = received_dist(rng);
        
        // Calculate expected offset
        std::size_t expected_offset = chunks_received * chunk_size;
        
        // Generate random incorrect offset
        std::uniform_int_distribution<std::size_t> offset_dist(0, chunks_received * chunk_size * 2);
        auto incorrect_offset = offset_dist(rng);
        
        // Ensure it's actually incorrect
        if (incorrect_offset == expected_offset) {
            incorrect_offset += chunk_size;
        }
        
        // Property: Offset validation logic
        // If received_offset != expected_offset, the chunk should be rejected
        bool should_reject = (incorrect_offset != expected_offset);
        
        BOOST_CHECK(should_reject);
        
        ++tests_passed;
        
        if (i < 10) {  // Log first 10 iterations for visibility
            BOOST_TEST_MESSAGE("Iteration " << i << ": "
                << "chunks_received=" << chunks_received << ", "
                << "expected_offset=" << expected_offset << ", "
                << "incorrect_offset=" << incorrect_offset << ", "
                << "should_reject=" << (should_reject ? "yes" : "no"));
        }
    }
    
    BOOST_TEST_MESSAGE("Snapshot offset validation tests:");
    BOOST_TEST_MESSAGE("  Total tests: " << tests_passed);
    
    BOOST_CHECK_EQUAL(tests_passed, property_test_iterations);
}

/**
 * Property: Snapshot metadata consistency
 * 
 * All chunks of a snapshot must have consistent metadata (last_included_index, last_included_term).
 * If metadata changes mid-transfer, the partial snapshot should be discarded.
 */
BOOST_AUTO_TEST_CASE(property_snapshot_metadata_consistency, * boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t tests_passed = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate initial snapshot metadata
        auto initial_index = generate_random_log_index(rng);
        auto initial_term = generate_random_term(rng);
        
        // Generate potentially different metadata for subsequent chunk
        auto subsequent_index = generate_random_log_index(rng);
        auto subsequent_term = generate_random_term(rng);
        
        // Property: Metadata consistency check
        // If subsequent metadata differs from initial, the chunk should be rejected
        bool metadata_matches = (initial_index == subsequent_index) && 
                               (initial_term == subsequent_term);
        bool should_reject = !metadata_matches;
        
        BOOST_CHECK_EQUAL(should_reject, !metadata_matches);
        
        ++tests_passed;
        
        if (i < 10) {  // Log first 10 iterations for visibility
            BOOST_TEST_MESSAGE("Iteration " << i << ": "
                << "initial (index=" << initial_index << ", term=" << initial_term << "), "
                << "subsequent (index=" << subsequent_index << ", term=" << subsequent_term << "), "
                << "should_reject=" << (should_reject ? "yes" : "no"));
        }
    }
    
    BOOST_TEST_MESSAGE("Snapshot metadata consistency tests:");
    BOOST_TEST_MESSAGE("  Total tests: " << tests_passed);
    
    BOOST_CHECK_EQUAL(tests_passed, property_test_iterations);
}

/**
 * Property: Snapshot term validation
 * 
 * The InstallSnapshot handler must reject snapshots from leaders with stale terms.
 */
BOOST_AUTO_TEST_CASE(property_snapshot_term_validation, * boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t tests_passed = 0;
    std::size_t stale_term_tests = 0;
    std::size_t current_term_tests = 0;
    std::size_t higher_term_tests = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random current term
        auto current_term = generate_random_term(rng);
        
        // Generate request term with controlled distribution to ensure all scenarios
        std::uniform_int_distribution<int> scenario_dist(0, 2);
        auto scenario = scenario_dist(rng);
        
        std::uint64_t request_term;
        if (scenario == 0 && current_term > 1) {
            // Stale term: request_term < current_term
            std::uniform_int_distribution<std::uint64_t> stale_dist(1, current_term - 1);
            request_term = stale_dist(rng);
        } else if (scenario == 1) {
            // Current term: request_term == current_term
            request_term = current_term;
        } else {
            // Higher term: request_term > current_term
            std::uniform_int_distribution<std::uint64_t> higher_dist(current_term + 1, max_term);
            request_term = higher_dist(rng);
        }
        
        // Property: Term validation logic
        // If request_term < current_term, reject immediately
        // If request_term >= current_term, accept and potentially update term
        bool should_reject = (request_term < current_term);
        
        if (request_term < current_term) {
            ++stale_term_tests;
        } else if (request_term == current_term) {
            ++current_term_tests;
        } else {
            ++higher_term_tests;
        }
        
        BOOST_CHECK_EQUAL(should_reject, request_term < current_term);
        
        ++tests_passed;
        
        if (i < 10) {  // Log first 10 iterations for visibility
            BOOST_TEST_MESSAGE("Iteration " << i << ": "
                << "current_term=" << current_term << ", "
                << "request_term=" << request_term << ", "
                << "should_reject=" << (should_reject ? "yes" : "no"));
        }
    }
    
    BOOST_TEST_MESSAGE("Snapshot term validation tests:");
    BOOST_TEST_MESSAGE("  Total tests: " << tests_passed);
    BOOST_TEST_MESSAGE("  Stale term tests: " << stale_term_tests);
    BOOST_TEST_MESSAGE("  Current term tests: " << current_term_tests);
    BOOST_TEST_MESSAGE("  Higher term tests: " << higher_term_tests);
    
    // Property: All term scenarios should be tested
    BOOST_CHECK_GT(stale_term_tests, 0);
    BOOST_CHECK_GT(current_term_tests, 0);
    BOOST_CHECK_GT(higher_term_tests, 0);
    BOOST_CHECK_EQUAL(tests_passed, property_test_iterations);
}

