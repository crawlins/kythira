#define BOOST_TEST_MODULE PersistenceRoundTripPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/persistence.hpp>
#include <raft/types.hpp>

#include <random>
#include <vector>
#include <cstddef>

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr std::uint64_t max_term = 1000000;
    constexpr std::uint64_t max_index = 1000000;
    constexpr std::uint64_t max_node_id = 10000;
    constexpr std::size_t max_log_entries = 50;
    constexpr std::size_t max_command_size = 100;
    constexpr std::size_t max_snapshot_data_size = 1000;
    constexpr std::size_t max_config_nodes = 10;
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

// Helper to generate random node ID
auto generate_random_node_id(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_node_id);
    return dist(rng);
}

// Helper to generate random command
auto generate_random_command(std::mt19937& rng) -> std::vector<std::byte> {
    std::uniform_int_distribution<std::size_t> size_dist(1, max_command_size);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    
    std::size_t size = size_dist(rng);
    std::vector<std::byte> command;
    command.reserve(size);
    
    for (std::size_t i = 0; i < size; ++i) {
        command.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    
    return command;
}

// Helper to generate random log entry
auto generate_random_log_entry(std::mt19937& rng) -> raft::log_entry<> {
    raft::log_entry<> entry;
    entry._term = generate_random_term(rng);
    entry._index = generate_random_log_index(rng);
    entry._command = generate_random_command(rng);
    return entry;
}

// Helper to generate random cluster configuration
auto generate_random_cluster_configuration(std::mt19937& rng) -> raft::cluster_configuration<> {
    std::uniform_int_distribution<std::size_t> count_dist(1, max_config_nodes);
    std::uniform_int_distribution<int> bool_dist(0, 1);
    
    std::size_t node_count = count_dist(rng);
    std::vector<std::uint64_t> nodes;
    nodes.reserve(node_count);
    
    for (std::size_t i = 0; i < node_count; ++i) {
        nodes.push_back(generate_random_node_id(rng));
    }
    
    bool is_joint = bool_dist(rng) == 1;
    std::optional<std::vector<std::uint64_t>> old_nodes;
    
    if (is_joint) {
        std::size_t old_node_count = count_dist(rng);
        std::vector<std::uint64_t> old_nodes_vec;
        old_nodes_vec.reserve(old_node_count);
        
        for (std::size_t i = 0; i < old_node_count; ++i) {
            old_nodes_vec.push_back(generate_random_node_id(rng));
        }
        old_nodes = old_nodes_vec;
    }
    
    return raft::cluster_configuration<>{nodes, is_joint, old_nodes};
}

// Helper to generate random snapshot data
auto generate_random_snapshot_data(std::mt19937& rng) -> std::vector<std::byte> {
    std::uniform_int_distribution<std::size_t> size_dist(1, max_snapshot_data_size);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    
    std::size_t size = size_dist(rng);
    std::vector<std::byte> data;
    data.reserve(size);
    
    for (std::size_t i = 0; i < size; ++i) {
        data.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    
    return data;
}

// Helper to generate random snapshot
auto generate_random_snapshot(std::mt19937& rng) -> raft::snapshot<> {
    raft::snapshot<> snap;
    snap._last_included_index = generate_random_log_index(rng);
    snap._last_included_term = generate_random_term(rng);
    snap._configuration = generate_random_cluster_configuration(rng);
    snap._state_machine_state = generate_random_snapshot_data(rng);
    return snap;
}

// Helper to compare log entries
auto log_entries_equal(const raft::log_entry<>& a, const raft::log_entry<>& b) -> bool {
    return a.term() == b.term() && 
           a.index() == b.index() && 
           a.command() == b.command();
}

// Helper to compare cluster configurations
auto cluster_configurations_equal(
    const raft::cluster_configuration<>& a, 
    const raft::cluster_configuration<>& b
) -> bool {
    if (a.nodes() != b.nodes()) {
        return false;
    }
    if (a.is_joint_consensus() != b.is_joint_consensus()) {
        return false;
    }
    if (a.old_nodes() != b.old_nodes()) {
        return false;
    }
    return true;
}

// Helper to compare snapshots
auto snapshots_equal(const raft::snapshot<>& a, const raft::snapshot<>& b) -> bool {
    return a.last_included_index() == b.last_included_index() &&
           a.last_included_term() == b.last_included_term() &&
           cluster_configurations_equal(a.configuration(), b.configuration()) &&
           a.state_machine_state() == b.state_machine_state();
}

/**
 * Feature: raft-consensus, Property 10: Persistence Round-Trip
 * Validates: Requirements 5.6
 * 
 * Property: For any term value, saving then loading the term produces 
 * the same value.
 */
BOOST_AUTO_TEST_CASE(property_current_term_round_trip) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        raft::memory_persistence_engine<> engine;
        
        // Generate random term
        auto original_term = generate_random_term(rng);
        
        try {
            // Save then load
            engine.save_current_term(original_term);
            auto loaded_term = engine.load_current_term();
            
            // Verify they match
            if (loaded_term != original_term) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Term mismatch - original: " 
                    << original_term << ", loaded: " << loaded_term);
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("Current term round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: raft-consensus, Property 10: Persistence Round-Trip
 * Validates: Requirements 5.6
 * 
 * Property: For any node ID, saving then loading votedFor produces 
 * the same value.
 */
BOOST_AUTO_TEST_CASE(property_voted_for_round_trip) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        raft::memory_persistence_engine<> engine;
        
        // Generate random node ID
        auto original_node_id = generate_random_node_id(rng);
        
        try {
            // Save then load
            engine.save_voted_for(original_node_id);
            auto loaded_node_id = engine.load_voted_for();
            
            // Verify they match
            if (!loaded_node_id.has_value() || *loaded_node_id != original_node_id) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": VotedFor mismatch");
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("VotedFor round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: raft-consensus, Property 10: Persistence Round-Trip
 * Validates: Requirements 5.6
 * 
 * Property: For any log entry, appending then retrieving the entry produces 
 * an equivalent entry with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_log_entry_round_trip) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        raft::memory_persistence_engine<> engine;
        
        // Generate random log entry
        auto original_entry = generate_random_log_entry(rng);
        
        try {
            // Append then retrieve
            engine.append_log_entry(original_entry);
            auto loaded_entry = engine.get_log_entry(original_entry.index());
            
            // Verify they match
            if (!loaded_entry.has_value() || !log_entries_equal(*loaded_entry, original_entry)) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Log entry mismatch");
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("Log entry round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: raft-consensus, Property 10: Persistence Round-Trip
 * Validates: Requirements 5.6
 * 
 * Property: For any sequence of log entries, appending then retrieving 
 * the range produces equivalent entries with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_log_entries_range_round_trip) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t failures = 0;
    std::uniform_int_distribution<std::size_t> count_dist(1, max_log_entries);
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        raft::memory_persistence_engine<> engine;
        
        // Generate random log entries with sequential indices
        std::size_t entry_count = count_dist(rng);
        std::vector<raft::log_entry<>> original_entries;
        original_entries.reserve(entry_count);
        
        std::uint64_t start_index = generate_random_log_index(rng);
        for (std::size_t j = 0; j < entry_count; ++j) {
            raft::log_entry<> entry;
            entry._term = generate_random_term(rng);
            entry._index = start_index + j;
            entry._command = generate_random_command(rng);
            original_entries.push_back(entry);
        }
        
        try {
            // Append all entries
            for (const auto& entry : original_entries) {
                engine.append_log_entry(entry);
            }
            
            // Retrieve the range
            auto loaded_entries = engine.get_log_entries(start_index, start_index + entry_count - 1);
            
            // Verify they match
            if (loaded_entries.size() != original_entries.size()) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Entry count mismatch - expected: " 
                    << original_entries.size() << ", got: " << loaded_entries.size());
            } else {
                bool all_match = true;
                for (std::size_t j = 0; j < original_entries.size(); ++j) {
                    if (!log_entries_equal(loaded_entries[j], original_entries[j])) {
                        all_match = false;
                        break;
                    }
                }
                if (!all_match) {
                    ++failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Log entries content mismatch");
                }
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("Log entries range round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: raft-consensus, Property 10: Persistence Round-Trip
 * Validates: Requirements 5.6
 * 
 * Property: For any snapshot, saving then loading the snapshot produces 
 * an equivalent snapshot with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_snapshot_round_trip) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        raft::memory_persistence_engine<> engine;
        
        // Generate random snapshot
        auto original_snapshot = generate_random_snapshot(rng);
        
        try {
            // Save then load
            engine.save_snapshot(original_snapshot);
            auto loaded_snapshot = engine.load_snapshot();
            
            // Verify they match
            if (!loaded_snapshot.has_value() || !snapshots_equal(*loaded_snapshot, original_snapshot)) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Snapshot mismatch");
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("Snapshot round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: raft-consensus, Property 10: Persistence Round-Trip
 * Validates: Requirements 5.6
 * 
 * Property: For any complete Raft state (term, votedFor, log entries, snapshot),
 * saving then loading all state produces equivalent state.
 */
BOOST_AUTO_TEST_CASE(property_complete_state_round_trip) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t failures = 0;
    std::uniform_int_distribution<std::size_t> count_dist(1, max_log_entries);
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        raft::memory_persistence_engine<> engine;
        
        // Generate complete random state
        auto original_term = generate_random_term(rng);
        auto original_node_id = generate_random_node_id(rng);
        auto original_snapshot = generate_random_snapshot(rng);
        
        std::size_t entry_count = count_dist(rng);
        std::vector<raft::log_entry<>> original_entries;
        original_entries.reserve(entry_count);
        
        std::uint64_t start_index = generate_random_log_index(rng);
        for (std::size_t j = 0; j < entry_count; ++j) {
            raft::log_entry<> entry;
            entry._term = generate_random_term(rng);
            entry._index = start_index + j;
            entry._command = generate_random_command(rng);
            original_entries.push_back(entry);
        }
        
        try {
            // Save all state
            engine.save_current_term(original_term);
            engine.save_voted_for(original_node_id);
            engine.save_snapshot(original_snapshot);
            for (const auto& entry : original_entries) {
                engine.append_log_entry(entry);
            }
            
            // Load all state
            auto loaded_term = engine.load_current_term();
            auto loaded_node_id = engine.load_voted_for();
            auto loaded_snapshot = engine.load_snapshot();
            auto loaded_entries = engine.get_log_entries(start_index, start_index + entry_count - 1);
            
            // Verify everything matches
            bool state_matches = true;
            
            if (loaded_term != original_term) {
                state_matches = false;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Term mismatch");
            }
            
            if (!loaded_node_id.has_value() || *loaded_node_id != original_node_id) {
                state_matches = false;
                BOOST_TEST_MESSAGE("Iteration " << i << ": VotedFor mismatch");
            }
            
            if (!loaded_snapshot.has_value() || !snapshots_equal(*loaded_snapshot, original_snapshot)) {
                state_matches = false;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Snapshot mismatch");
            }
            
            if (loaded_entries.size() != original_entries.size()) {
                state_matches = false;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Entry count mismatch");
            } else {
                for (std::size_t j = 0; j < original_entries.size(); ++j) {
                    if (!log_entries_equal(loaded_entries[j], original_entries[j])) {
                        state_matches = false;
                        BOOST_TEST_MESSAGE("Iteration " << i << ": Entry " << j << " mismatch");
                        break;
                    }
                }
            }
            
            if (!state_matches) {
                ++failures;
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("Complete state round-trip: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    BOOST_CHECK_EQUAL(failures, 0);
}
