#include <raft/persistence.hpp>
#include <raft/types.hpp>

#define BOOST_TEST_MODULE persistence_concept_test
#include <boost/test/included/unit_test.hpp>

#include <unordered_map>

namespace {
    constexpr const char* test_name = "persistence_concept_test";
}

// Mock persistence engine for testing the concept
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
requires kythira::node_id<NodeId> && kythira::term_id<TermId> && kythira::log_index<LogIndex>
class mock_persistence_engine {
public:
    using log_entry_t = kythira::log_entry<TermId, LogIndex>;
    using snapshot_t = kythira::snapshot<NodeId, TermId, LogIndex>;
    
    auto save_current_term(TermId term) -> void {
        _current_term = term;
    }
    
    auto load_current_term() -> TermId {
        return _current_term;
    }
    
    auto save_voted_for(NodeId node) -> void {
        _voted_for = node;
    }
    
    auto load_voted_for() -> std::optional<NodeId> {
        return _voted_for;
    }
    
    auto append_log_entry(const log_entry_t& entry) -> void {
        _log[entry.index()] = entry;
    }
    
    auto get_log_entry(LogIndex index) -> std::optional<log_entry_t> {
        auto it = _log.find(index);
        if (it != _log.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
    auto get_log_entries(LogIndex start, LogIndex end) -> std::vector<log_entry_t> {
        std::vector<log_entry_t> result;
        for (LogIndex i = start; i <= end; ++i) {
            auto entry = get_log_entry(i);
            if (entry) {
                result.push_back(*entry);
            }
        }
        return result;
    }
    
    auto get_last_log_index() -> LogIndex {
        if (_log.empty()) {
            return LogIndex{0};
        }
        LogIndex max_index{0};
        for (const auto& [index, _] : _log) {
            if (index > max_index) {
                max_index = index;
            }
        }
        return max_index;
    }
    
    auto truncate_log(LogIndex index) -> void {
        auto it = _log.begin();
        while (it != _log.end()) {
            if (it->first >= index) {
                it = _log.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    auto save_snapshot(const snapshot_t& snap) -> void {
        _snapshot = snap;
    }
    
    auto load_snapshot() -> std::optional<snapshot_t> {
        return _snapshot;
    }
    
    auto delete_log_entries_before(LogIndex index) -> void {
        auto it = _log.begin();
        while (it != _log.end()) {
            if (it->first < index) {
                it = _log.erase(it);
            } else {
                ++it;
            }
        }
    }
    
private:
    TermId _current_term{0};
    std::optional<NodeId> _voted_for;
    std::unordered_map<LogIndex, log_entry_t> _log;
    std::optional<snapshot_t> _snapshot;
};

BOOST_AUTO_TEST_CASE(test_persistence_engine_concept, * boost::unit_test::timeout(30)) {
    // Test that mock_persistence_engine satisfies persistence_engine concept
    using engine_t = mock_persistence_engine<>;
    using node_id_t = std::uint64_t;
    using term_id_t = std::uint64_t;
    using log_index_t = std::uint64_t;
    using log_entry_t = kythira::log_entry<term_id_t, log_index_t>;
    using snapshot_t = kythira::snapshot<node_id_t, term_id_t, log_index_t>;
    
    static_assert(
        kythira::persistence_engine<engine_t, node_id_t, term_id_t, log_index_t, log_entry_t, snapshot_t>,
        "mock_persistence_engine should satisfy persistence_engine concept"
    );
    
    // Test that memory_persistence_engine satisfies persistence_engine concept
    using memory_engine_t = kythira::memory_persistence_engine<>;
    
    static_assert(
        kythira::persistence_engine<memory_engine_t, node_id_t, term_id_t, log_index_t, log_entry_t, snapshot_t>,
        "memory_persistence_engine should satisfy persistence_engine concept"
    );
}

BOOST_AUTO_TEST_CASE(test_mock_persistence_term_operations, * boost::unit_test::timeout(30)) {
    mock_persistence_engine<> engine;
    
    // Test term operations
    engine.save_current_term(5);
    BOOST_TEST(engine.load_current_term() == 5);
    
    engine.save_current_term(10);
    BOOST_TEST(engine.load_current_term() == 10);
}

BOOST_AUTO_TEST_CASE(test_mock_persistence_voted_for_operations, * boost::unit_test::timeout(30)) {
    mock_persistence_engine<> engine;
    
    // Initially no vote
    auto voted_for = engine.load_voted_for();
    BOOST_TEST(!voted_for.has_value());
    
    // Save a vote
    engine.save_voted_for(42);
    voted_for = engine.load_voted_for();
    BOOST_TEST(voted_for.has_value());
    BOOST_TEST(*voted_for == 42);
}

BOOST_AUTO_TEST_CASE(test_mock_persistence_log_operations, * boost::unit_test::timeout(30)) {
    mock_persistence_engine<> engine;
    
    // Create and append log entries
    kythira::log_entry<> entry1{1, 1, {std::byte{0x01}}};
    kythira::log_entry<> entry2{1, 2, {std::byte{0x02}}};
    kythira::log_entry<> entry3{2, 3, {std::byte{0x03}}};
    
    engine.append_log_entry(entry1);
    engine.append_log_entry(entry2);
    engine.append_log_entry(entry3);
    
    // Test get_last_log_index
    BOOST_TEST(engine.get_last_log_index() == 3);
    
    // Test get_log_entry
    auto retrieved = engine.get_log_entry(2);
    BOOST_TEST(retrieved.has_value());
    BOOST_TEST(retrieved->term() == 1);
    BOOST_TEST(retrieved->index() == 2);
    
    // Test get_log_entries range
    auto entries = engine.get_log_entries(1, 3);
    BOOST_TEST(entries.size() == 3);
    BOOST_TEST(entries[0].index() == 1);
    BOOST_TEST(entries[1].index() == 2);
    BOOST_TEST(entries[2].index() == 3);
}

BOOST_AUTO_TEST_CASE(test_mock_persistence_truncate_log, * boost::unit_test::timeout(30)) {
    mock_persistence_engine<> engine;
    
    // Add entries
    kythira::log_entry<> entry1{1, 1, {std::byte{0x01}}};
    kythira::log_entry<> entry2{1, 2, {std::byte{0x02}}};
    kythira::log_entry<> entry3{2, 3, {std::byte{0x03}}};
    kythira::log_entry<> entry4{2, 4, {std::byte{0x04}}};
    
    engine.append_log_entry(entry1);
    engine.append_log_entry(entry2);
    engine.append_log_entry(entry3);
    engine.append_log_entry(entry4);
    
    // Truncate from index 3
    engine.truncate_log(3);
    
    // Entries 3 and 4 should be gone
    BOOST_TEST(engine.get_last_log_index() == 2);
    BOOST_TEST(!engine.get_log_entry(3).has_value());
    BOOST_TEST(!engine.get_log_entry(4).has_value());
    BOOST_TEST(engine.get_log_entry(1).has_value());
    BOOST_TEST(engine.get_log_entry(2).has_value());
}

BOOST_AUTO_TEST_CASE(test_mock_persistence_delete_log_entries_before, * boost::unit_test::timeout(30)) {
    mock_persistence_engine<> engine;
    
    // Add entries
    kythira::log_entry<> entry1{1, 1, {std::byte{0x01}}};
    kythira::log_entry<> entry2{1, 2, {std::byte{0x02}}};
    kythira::log_entry<> entry3{2, 3, {std::byte{0x03}}};
    kythira::log_entry<> entry4{2, 4, {std::byte{0x04}}};
    
    engine.append_log_entry(entry1);
    engine.append_log_entry(entry2);
    engine.append_log_entry(entry3);
    engine.append_log_entry(entry4);
    
    // Delete entries before index 3
    engine.delete_log_entries_before(3);
    
    // Entries 1 and 2 should be gone
    BOOST_TEST(!engine.get_log_entry(1).has_value());
    BOOST_TEST(!engine.get_log_entry(2).has_value());
    BOOST_TEST(engine.get_log_entry(3).has_value());
    BOOST_TEST(engine.get_log_entry(4).has_value());
    BOOST_TEST(engine.get_last_log_index() == 4);
}

BOOST_AUTO_TEST_CASE(test_mock_persistence_snapshot_operations, * boost::unit_test::timeout(30)) {
    mock_persistence_engine<> engine;
    
    // Initially no snapshot
    auto snapshot = engine.load_snapshot();
    BOOST_TEST(!snapshot.has_value());
    
    // Create and save a snapshot
    kythira::cluster_configuration<> config{{1, 2, 3}, false, std::nullopt};
    kythira::snapshot<> snap{10, 5, config, {std::byte{0xAA}, std::byte{0xBB}}};
    
    engine.save_snapshot(snap);
    
    // Load and verify
    snapshot = engine.load_snapshot();
    BOOST_TEST(snapshot.has_value());
    BOOST_TEST(snapshot->last_included_index() == 10);
    BOOST_TEST(snapshot->last_included_term() == 5);
    BOOST_TEST(snapshot->configuration().nodes().size() == 3);
}

BOOST_AUTO_TEST_CASE(test_memory_persistence_engine_concept, * boost::unit_test::timeout(30)) {
    // Test that memory_persistence_engine satisfies persistence_engine concept
    using engine_t = kythira::memory_persistence_engine<>;
    using node_id_t = std::uint64_t;
    using term_id_t = std::uint64_t;
    using log_index_t = std::uint64_t;
    using log_entry_t = kythira::log_entry<term_id_t, log_index_t>;
    using snapshot_t = kythira::snapshot<node_id_t, term_id_t, log_index_t>;
    
    static_assert(
        kythira::persistence_engine<engine_t, node_id_t, term_id_t, log_index_t, log_entry_t, snapshot_t>,
        "memory_persistence_engine should satisfy persistence_engine concept"
    );
}
