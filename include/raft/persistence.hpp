// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file persistence.hpp
/// @brief Persistence-engine concept and an in-memory implementation for testing.

#include "fault_injection.hpp"
#include "types.hpp"
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace kythira {

// ─────────────────────────────────────────────────────────────────────────────
// The durability barrier (`.kiro/specs/durable-append-barrier/`)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A monotonically increasing count of writes an engine has accepted.
///
/// Not a log index. Two different entries can be written at one index (a
/// follower overwriting a conflicting suffix) and both writes have to be
/// orderable against a barrier, which an index cannot express. It is also not
/// per-group: the whole point of group commit is that one `fsync` covers every
/// write an engine took, whoever asked for it.
using write_sequence = std::uint64_t;

/// @brief What a configuration is entitled to call itself.
///
/// Requirement 5.4 of `.kiro/specs/durable-append-barrier/`: an engine that
/// cannot take a barrier must be *detectable*, so that a configuration using
/// one is refused the word durable rather than assumed optimistic. The three
/// values are exactly the three the benchmark's durability axis reports, and
/// two of them are not durable.
enum class durability_class : std::uint8_t {
    /// Nothing survives the process. `memory_persistence_engine`.
    none = 0,
    /// Bytes reach the operating system and no barrier is ever taken. Survives
    /// a process crash, loses everything to a power cut. **Not durable**, and
    /// labelled so wherever it appears.
    buffered = 1,
    /// Every advertised append is covered by a barrier before it is advertised.
    barrier = 2,
};

[[nodiscard]] inline auto to_string(durability_class c) -> std::string_view {
    switch (c) {
        case durability_class::none:
            return "none";
        case durability_class::buffered:
            return "buffered";
        case durability_class::barrier:
            return "barrier";
    }
    return "unknown";
}

/// @brief Optional extension: an engine that can separate "write these bytes"
///        from "make everything through sequence S durable".
///
/// `commit_batch()` fuses the two, which is why it cannot serve the append
/// path: the writer needs to hand over bytes on one thread and wait for
/// somebody's barrier on another. Splitting them is what makes group commit
/// possible at all.
///
/// Detected with `if constexpr`, never required. An engine without it keeps
/// exactly the behaviour it has today — `node` calls plain
/// `append_log_entry()` — and `describes_durable_storage()` then refuses to
/// call a configuration using it durable.
///
/// **The ordering rule an implementation must honour**, restated wherever this
/// is implemented: the sequence is assigned under the same lock that writes
/// the bytes, and a barrier samples the highest sequence it will claim
/// **before** it makes the syscall and publishes it **after**. Sampling after
/// would credit the barrier with writes that raced in while it was running —
/// writes the syscall never saw.
template<typename P>
concept barriered_persistence_engine =
    requires(P& p, const typename P::log_entry_t& entry, write_sequence seq) {
        /// Append `entry` and return the sequence assigned to its bytes.
        { p.append_log_entry_sequenced(entry) } -> std::same_as<write_sequence>;
        /// Block until a barrier has covered every write through `seq`.
        /// Throws if the barrier fails; see Requirement 1.5.
        { p.barrier_through(seq) } -> std::same_as<void>;
        /// The highest sequence a completed barrier has covered.
        { p.durable_through() } -> std::same_as<write_sequence>;
        /// What this engine, as configured, is entitled to claim.
        { p.durability() } -> std::same_as<durability_class>;
    };

/// @brief Whether a configuration built on `engine` may be described as durable.
///
/// Requirement 5.4: the answer for an engine that does not implement the
/// extension is **no**, not "probably". A missing barrier is not an omission
/// to be read charitably — it is the defect this spec exists to remove.
template<typename P> [[nodiscard]] auto describes_durable_storage(P& engine) -> bool {
    if constexpr (barriered_persistence_engine<P>) {
        return engine.durability() == durability_class::barrier;
    } else {
        (void)engine;
        return false;
    }
}

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
        (void)append_log_entry_sequenced(entry);
    }

    // ── barriered_persistence_engine ─────────────────────────────────────────
    //
    // Implemented so that `node` takes the same code path for a memory engine
    // as for a file-backed one, and Requirement 5.1 is met by construction
    // rather than by a branch: the barrier here is an empty function over an
    // integer compare, so a memory-backed row pays nothing for a durability
    // feature it cannot use.
    //
    // `durability()` still answers `none`. A no-op barrier is not durability,
    // and Requirement 5.4 forbids reading it as though it were — this engine
    // reports 100% barrier *coverage* and no durability at all, which are two
    // different questions and both answers are true.

    auto append_log_entry_sequenced(const log_entry_t& entry) -> write_sequence {
        fiu_do_on("raft/persistence/append_log_entry",
                  throw std::runtime_error("chaos: append_log_entry"););
        _log[entry.index()] = entry;
        return ++_write_seq;
    }

    auto barrier_through(write_sequence) -> void {}

    [[nodiscard]] auto durable_through() const -> write_sequence { return _write_seq; }

    [[nodiscard]] auto durability() const -> durability_class { return durability_class::none; }

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
    /// Assigned under the caller's serialisation (`node`'s mutex), like every
    /// other member here — this engine has never been internally synchronised.
    write_sequence _write_seq{0};
};

}  // namespace kythira
