#pragma once

/// @file persistence.hpp
/// @brief Persistence-engine concept and an in-memory implementation for testing.

#include "fault_injection.hpp"
#include "types.hpp"
#include <optional>
#include <vector>
#include <unordered_map>

namespace kythira {

/// @brief Concept for a durable Raft-state store.
///
/// The persistence engine is the only component allowed to survive a process
/// restart.  Implementations must flush `save_current_term` and `save_voted_for`
/// to stable storage synchronously before returning, as required by the Raft paper.
///
/// @tparam P        Concrete persistence engine type.
/// @tparam NodeId   Must satisfy `node_id`.
/// @tparam TermId   Must satisfy `term_id`.
/// @tparam LogIndex Must satisfy `log_index`.
/// @tparam LogEntry Must satisfy `log_entry_type<TermId, LogIndex>`.
/// @tparam Snapshot Must satisfy `snapshot_type<NodeId, TermId, LogIndex>`.
template<typename P, typename NodeId, typename TermId, typename LogIndex, typename LogEntry,
         typename Snapshot>
concept persistence_engine =
    requires(P engine, const TermId& term, const NodeId& node, const LogEntry& entry,
             const LogIndex& index, const Snapshot& snap) {
        requires node_id<NodeId>;
        requires term_id<TermId>;
        requires log_index<LogIndex>;
        requires log_entry_type<LogEntry, TermId, LogIndex>;
        requires snapshot_type<Snapshot, NodeId, TermId, LogIndex>;

        /// Durably record the current term.
        { engine.save_current_term(term) } -> std::same_as<void>;
        /// Restore the current term after a restart.
        { engine.load_current_term() } -> std::same_as<TermId>;

        /// Durably record the node voted for in this term.
        { engine.save_voted_for(node) } -> std::same_as<void>;
        /// Restore the voted-for record after a restart.
        { engine.load_voted_for() } -> std::same_as<std::optional<NodeId>>;

        /// Append a single log entry.
        { engine.append_log_entry(entry) } -> std::same_as<void>;
        /// Retrieve the entry at `index`, or `nullopt` if not present.
        { engine.get_log_entry(index) } -> std::same_as<std::optional<LogEntry>>;
        /// Retrieve all entries in the inclusive range `[start, end]`.
        { engine.get_log_entries(index, index) } -> std::same_as<std::vector<LogEntry>>;
        /// Return the index of the last stored entry (0 if the log is empty).
        { engine.get_last_log_index() } -> std::same_as<LogIndex>;

        /// Remove all entries with index ≥ `index` (used on leader conflict resolution).
        { engine.truncate_log(index) } -> std::same_as<void>;

        /// Save a snapshot, replacing any previously stored snapshot.
        { engine.save_snapshot(snap) } -> std::same_as<void>;
        /// Return the most recently saved snapshot, or `nullopt` if none exists.
        { engine.load_snapshot() } -> std::same_as<std::optional<Snapshot>>;
        /// Discard all log entries with index < `index` (log compaction after snapshotting).
        { engine.delete_log_entries_before(index) } -> std::same_as<void>;
    };

/// @brief In-memory persistence engine for testing and single-process development.
///
/// All state is lost when the process terminates.  Fault-injection hooks
/// (`fiu_do_on`) allow chaos tests to simulate individual storage failures.
///
/// @tparam NodeId   Node identifier type; defaults to `uint64_t`.
/// @tparam TermId   Term number type; defaults to `uint64_t`.
/// @tparam LogIndex Log index type; defaults to `uint64_t`.
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
class memory_persistence_engine {
public:
    using log_entry_t = log_entry<TermId, LogIndex>;
    using snapshot_t = snapshot<NodeId, TermId, LogIndex>;

    auto save_current_term(TermId term) -> void {
        fiu_do_on("raft/persistence/save_current_term",
                  throw std::runtime_error("chaos: save_current_term"););
        _current_term = term;
    }

    auto load_current_term() -> TermId { return _current_term; }

    auto save_voted_for(NodeId node) -> void {
        fiu_do_on("raft/persistence/save_voted_for",
                  throw std::runtime_error("chaos: save_voted_for"););
        _voted_for = node;
    }

    auto load_voted_for() -> std::optional<NodeId> { return _voted_for; }

    auto append_log_entry(const log_entry_t& entry) -> void {
        fiu_do_on("raft/persistence/append_log_entry",
                  throw std::runtime_error("chaos: append_log_entry"););
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
        fiu_do_on("raft/persistence/truncate_log",
                  throw std::runtime_error("chaos: truncate_log"););
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
        fiu_do_on("raft/persistence/save_snapshot",
                  throw std::runtime_error("chaos: save_snapshot"););
        _snapshot = snap;
    }

    auto load_snapshot() -> std::optional<snapshot_t> { return _snapshot; }

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

}  // namespace kythira
