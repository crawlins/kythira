#pragma once

#include "types.hpp"
#include <optional>
#include <vector>
#include <unordered_map>

namespace kythira {

// Persistence engine concept
// Defines the interface for durable storage of Raft state
template<typename P, typename NodeId, typename TermId, typename LogIndex, typename LogEntry, typename Snapshot>
concept persistence_engine = requires(
    P engine,
    const TermId& term,
    const NodeId& node,
    const LogEntry& entry,
    const LogIndex& index,
    const Snapshot& snap
) {
    // Type requirements
    requires node_id<NodeId>;
    requires term_id<TermId>;
    requires log_index<LogIndex>;
    requires log_entry_type<LogEntry, TermId, LogIndex>;
    requires snapshot_type<Snapshot, NodeId, TermId, LogIndex>;
    
    // Persistent state operations - currentTerm
    { engine.save_current_term(term) } -> std::same_as<void>;
    { engine.load_current_term() } -> std::same_as<TermId>;
    
    // Persistent state operations - votedFor
    { engine.save_voted_for(node) } -> std::same_as<void>;
    { engine.load_voted_for() } -> std::same_as<std::optional<NodeId>>;
    
    // Log operations - append and retrieve
    { engine.append_log_entry(entry) } -> std::same_as<void>;
    { engine.get_log_entry(index) } -> std::same_as<std::optional<LogEntry>>;
    { engine.get_log_entries(index, index) } -> std::same_as<std::vector<LogEntry>>;
    { engine.get_last_log_index() } -> std::same_as<LogIndex>;
    
    // Log operations - truncation
    { engine.truncate_log(index) } -> std::same_as<void>;
    
    // Snapshot operations
    { engine.save_snapshot(snap) } -> std::same_as<void>;
    { engine.load_snapshot() } -> std::same_as<std::optional<Snapshot>>;
    { engine.delete_log_entries_before(index) } -> std::same_as<void>;
};

// In-memory persistence engine for testing and development
// Stores all Raft state in memory without durability guarantees
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
class memory_persistence_engine {
public:
    using log_entry_t = log_entry<TermId, LogIndex>;
    using snapshot_t = snapshot<NodeId, TermId, LogIndex>;
    
    // Persistent state operations - currentTerm
    auto save_current_term(TermId term) -> void {
        _current_term = term;
    }
    
    auto load_current_term() -> TermId {
        return _current_term;
    }
    
    // Persistent state operations - votedFor
    auto save_voted_for(NodeId node) -> void {
        _voted_for = node;
    }
    
    auto load_voted_for() -> std::optional<NodeId> {
        return _voted_for;
    }
    
    // Log operations - append and retrieve
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
    
    // Log operations - truncation
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
    
    // Snapshot operations
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

} // namespace kythira
