// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/exceptions.hpp>
#include <raft/fault_injection.hpp>
#include <raft/json_serializer.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kythira {

// File-backed persistence engine for use in standalone chaos_node processes.
//
// Storage layout under DATA_DIR/:
//   term        — decimal current term (plain text)
//   voted_for   — decimal node ID or "none"
//   log         — one JSON object per line, each with {term, index, command_b64}
//   snapshot    — JSON snapshot (optional)
//
// All writes use the atomic write-then-rename idiom so the file is never
// partially written from the reader's perspective.

template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
class file_persistence_engine {
public:
    using log_entry_t = log_entry<TermId, LogIndex>;
    using snapshot_t = snapshot<NodeId, TermId, LogIndex>;
    using ser_t = json_rpc_serializer<std::vector<std::byte>>;

    /// @param data_dir         Where this engine's state lives.
    /// @param barriers_enabled Whether `barrier_through()` actually reaches the
    ///                         disk. `false` is the benchmark's `buffered` arm
    ///                         and is **not durable**; `durability()` says so,
    ///                         and Requirement 5.4 of
    ///                         `.kiro/specs/durable-append-barrier/` makes that
    ///                         answer the one a caller must consult rather than
    ///                         assuming the optimistic reading.
    ///
    /// The flag exists because the measurement needs a file-backed arm that
    /// pays the encode-and-write cost and none of the fsync cost, to price the
    /// two separately — not because a deployment should ever choose it.
    explicit file_persistence_engine(std::filesystem::path data_dir, bool barriers_enabled = true)
        : _dir(std::move(data_dir)), _barriers_enabled(barriers_enabled) {
        std::filesystem::create_directories(_dir);
        load_all();
    }

    // Move-only: safe when the object is not concurrently accessed (pre-start).
    file_persistence_engine(file_persistence_engine&& other) noexcept
        : _dir(std::move(other._dir)),
          _current_term(other._current_term),
          _voted_for(std::move(other._voted_for)),
          _log(std::move(other._log)),
          _snapshot(std::move(other._snapshot)),
          _batching(other._batching),
          _batch_lines(std::move(other._batch_lines)),
          _batch_undo(std::move(other._batch_undo)),
          _batch_flushed(other._batch_flushed),
          _barriers_enabled(other._barriers_enabled),
          _write_seq(other._write_seq),
          _flushed_seq(other._flushed_seq),
          _durable_seq(other._durable_seq) {}

    file_persistence_engine& operator=(file_persistence_engine&&) = delete;
    file_persistence_engine(const file_persistence_engine&) = delete;
    file_persistence_engine& operator=(const file_persistence_engine&) = delete;

    // ── currentTerm ──────────────────────────────────────────────────────────

    auto save_current_term(TermId term) -> void {
        fiu_do_on("raft/persistence/save_current_term",
                  throw std::runtime_error("chaos: save_current_term"););
        std::lock_guard lock(_mu);
        atomic_write(_dir / "term", std::to_string(term));
        _current_term = term;
    }

    auto load_current_term() -> TermId {
        std::lock_guard lock(_mu);
        return _current_term;
    }

    // ── votedFor ─────────────────────────────────────────────────────────────

    auto save_voted_for(NodeId node) -> void {
        fiu_do_on("raft/persistence/save_voted_for",
                  throw std::runtime_error("chaos: save_voted_for"););
        std::lock_guard lock(_mu);
        if constexpr (std::is_same_v<NodeId, std::string>) {
            atomic_write(_dir / "voted_for", node);
        } else {
            atomic_write(_dir / "voted_for", std::to_string(node));
        }
        _voted_for = node;
    }

    auto load_voted_for() -> std::optional<NodeId> {
        std::lock_guard lock(_mu);
        return _voted_for;
    }

    // ── Log ──────────────────────────────────────────────────────────────────

    auto append_log_entry(const log_entry_t& entry) -> void {
        (void)append_log_entry_sequenced(entry);
    }

    /// @brief Append `entry` and return the sequence assigned to its bytes.
    ///
    /// **The sequence is assigned under the same lock that writes the bytes.**
    /// That is not an implementation detail — it is the first half of the
    /// ordering rule `barrier_through()` depends on. A sequence handed out
    /// before the bytes were written would let a barrier that sampled it claim
    /// to have covered them.
    auto append_log_entry_sequenced(const log_entry_t& entry) -> write_sequence {
        fiu_do_on("raft/persistence/append_log_entry",
                  throw std::runtime_error("chaos: append_log_entry"););
        std::lock_guard lock(_mu);

        if (_batching) {
            // Remember what this index held so abort_batch() can put it back.
            // Only the FIRST observation per index is recorded: a batch that
            // appends twice at one index must roll back to the pre-batch value,
            // not to the intermediate one.
            if (!_batch_undo.contains(entry.index())) {
                auto existing = _log.find(entry.index());
                _batch_undo.emplace(entry.index(), existing == _log.end()
                                                       ? std::nullopt
                                                       : std::optional{existing->second});
            }
            _log[entry.index()] = entry;
            _batch_lines += entry_to_json(entry) + "\n";
            // Assigned, but NOT flushed: these bytes are in `_batch_lines` and
            // not in the file, so `_flushed_seq` stays where it was. A barrier
            // asked for this sequence flushes the batch first — "flush, then
            // one barrier", which is what `commit_batch()` already is, taken
            // apart so the two halves can happen on different threads.
            return ++_write_seq;
        }

        _log[entry.index()] = entry;
        // Append one JSON line to the log file
        auto line = entry_to_json(entry) + "\n";
        append_to_log_file(line);
        _flushed_seq = ++_write_seq;
        return _write_seq;
    }

    // ── barriered_persistence_engine (include/raft/persistence.hpp) ──────────

    /// @brief Block until a durability barrier has covered every write through
    ///        `seq`, coalescing with any concurrent request for the same.
    ///
    /// **The ordering rule, restated in code because it is the whole of this
    /// design's subtlety** (`.kiro/specs/durable-append-barrier/` design,
    /// §Group commit; Requirement 3.4):
    ///
    /// - the sequence is assigned under `_mu`, by the same critical section
    ///   that writes the bytes (`append_log_entry_sequenced`);
    /// - the barrier samples the highest sequence it will claim **before** it
    ///   calls `fsync`, while still holding `_mu`;
    /// - it publishes that sample **after** the syscall returns.
    ///
    /// Sampling after would credit this barrier with writes that raced in
    /// during the syscall — bytes the `fsync` never saw. Doing it in this
    /// order makes a waiter woken by a barrier older than its own write
    /// impossible rather than unlikely, which is what Requirement 3.4 asks
    /// for.
    ///
    /// `_mu` is **released across the syscall** (Requirement 3.2). That is
    /// what lets other appends land, and other waiters queue, while one fsync
    /// is in flight — the coalescing, in one line.
    ///
    /// @throws persistence_exception if the barrier could not be taken. The
    ///         caller must not advertise the append (Requirement 1.5).
    auto barrier_through(write_sequence seq) -> void {
        if (!_barriers_enabled) {
            // The `buffered` arm. Deliberately not silent about what it is:
            // `durability()` answers `buffered`, and every consumer that asks
            // whether this configuration is durable gets `false`.
            return;
        }

        std::unique_lock lock(_mu);
        while (_durable_seq < seq) {
            if (_barrier_running) {
                _barrier_cv.wait(lock);
                continue;
            }

            // "Flush, then one barrier." Only reachable when a batch is open
            // and holds this caller's bytes; the append path flushes as it
            // writes.
            if (_flushed_seq < seq && !_batch_lines.empty()) {
                append_to_log_file(_batch_lines);
                _batch_lines.clear();
                _batch_flushed = true;
                _flushed_seq = _write_seq;
            }

            const auto sampled = _flushed_seq;  // BEFORE the syscall.
            _barrier_running = true;
            ++_barriers_issued;
            lock.unlock();
            const bool ok = sync_log_and_directory();
            lock.lock();
            _barrier_running = false;
            if (ok && sampled > _durable_seq) {
                _durable_seq = sampled;  // AFTER it.
            }
            _barrier_cv.notify_all();
            if (!ok) {
                throw persistence_exception("file_persistence: durability barrier failed for " +
                                            (_dir / "log").string());
            }
            if (sampled < seq) {
                // This barrier could not have covered the caller: its bytes
                // were not in the file when the sample was taken.
                //
                // There is nothing left to flush — the branch above would have
                // flushed it — so no future barrier can cover this sequence
                // either. The write was discarded: `abort_batch()` threw its
                // buffered lines away, or a truncation removed them. Looping
                // would fsync forever waiting for bytes that no longer exist,
                // so this reports instead, and Requirement 1.5 makes the
                // caller's response to that "do not advertise the append".
                throw persistence_exception(
                    "file_persistence: sequence " + std::to_string(seq) +
                    " can never be made durable; its bytes were discarded before any barrier "
                    "reached them");
            }
        }
    }

    [[nodiscard]] auto durable_through() const -> write_sequence {
        std::lock_guard lock(_mu);
        return _durable_seq;
    }

    [[nodiscard]] auto durability() const -> durability_class {
        return _barriers_enabled ? durability_class::barrier : durability_class::buffered;
    }

    /// @brief How many durability barriers this engine has actually issued.
    ///
    /// The one number about a barrier that only the engine can know, and the
    /// only way to show that group commit coalesces at all: "N appends, fewer
    /// than N fsyncs" is not observable from outside the syscall.
    ///
    /// It is deliberately **not** the coverage counter.
    /// `.kiro/specs/durable-append-barrier/` Requirement 2.3 keeps that one
    /// outside production code, because coverage is a property of how a caller
    /// uses the engine and belongs on the caller's side of the seam. This is a
    /// syscall count, it costs one increment inside a critical section already
    /// held, and without it Requirement 3.1 has no test.
    [[nodiscard]] auto barriers_issued() const -> std::uint64_t {
        std::lock_guard lock(_mu);
        return _barriers_issued;
    }

    // ── batched_persistence_engine (include/raft/group_storage.hpp) ──────────
    //
    // Buffers appends so that N of them cost one durability barrier instead of
    // N. At 1000 groups per process that difference is the whole reason a
    // batched tick is worth having (design §3.2), and split apply relies on it
    // for the atomicity of "children's state and parent's apply index land
    // together" (design §5.4 step E).
    //
    // Deliberately appends ONLY. `save_current_term` and `save_voted_for` keep
    // writing synchronously even inside a batch, because Raft requires both
    // durable *before* the node responds to the RPC that changed them —
    // deferring them into a batch would break the algorithm's own ordering
    // requirement to save a syscall.

    /// @brief Begin buffering log appends. Nested batches are not supported.
    auto begin_batch() -> void {
        std::lock_guard lock(_mu);
        if (_batching) {
            throw std::runtime_error("file_persistence: begin_batch inside an open batch");
        }
        _batching = true;
        _batch_lines.clear();
        _batch_undo.clear();
        _batch_flushed = false;
    }

    /// @brief Flush every buffered append and issue exactly one durability barrier.
    auto commit_batch() -> void {
        std::lock_guard lock(_mu);
        if (!_batching) {
            throw std::runtime_error("file_persistence: commit_batch with no open batch");
        }
        _batching = false;
        _batch_undo.clear();
        const bool anything_written = _batch_flushed || !_batch_lines.empty();
        _batch_flushed = false;
        if (!_batch_lines.empty()) {
            append_to_log_file(_batch_lines);
            _batch_lines.clear();
            _flushed_seq = _write_seq;
        }
        if (!anything_written || !_barriers_enabled) {
            return;
        }
        const auto sampled = _flushed_seq;
        ++_barriers_issued;
        if (!sync_log_and_directory()) {
            throw persistence_exception("file_persistence: durability barrier failed for " +
                                        (_dir / "log").string());
        }
        if (sampled > _durable_seq) {
            _durable_seq = sampled;
        }
        _barrier_cv.notify_all();
    }

    /// @brief Discard every buffered append, restoring the pre-batch state.
    auto abort_batch() -> void {
        std::lock_guard lock(_mu);
        if (!_batching) {
            throw std::runtime_error("file_persistence: abort_batch with no open batch");
        }
        _batching = false;
        _batch_lines.clear();
        for (const auto& [index, previous] : _batch_undo) {
            if (previous.has_value()) {
                _log[index] = *previous;
            } else {
                _log.erase(index);
            }
        }
        _batch_undo.clear();
        // A `barrier_through()` that arrived mid-batch forced this batch's
        // buffered lines into the file to be able to cover them. Undoing the
        // in-memory log is then not enough — the file has lines the log no
        // longer has — so rewrite it from what the log now holds.
        if (_batch_flushed) {
            _batch_flushed = false;
            rewrite_log_file();
        }
    }

    /// @brief Whether a batch is currently open. Test and diagnostic use only.
    [[nodiscard]] auto batch_open() const -> bool {
        std::lock_guard lock(_mu);
        return _batching;
    }

    auto get_log_entry(LogIndex index) -> std::optional<log_entry_t> {
        std::lock_guard lock(_mu);
        auto it = _log.find(index);
        if (it == _log.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    auto get_log_entries(LogIndex start, LogIndex end) -> std::vector<log_entry_t> {
        std::lock_guard lock(_mu);
        std::vector<log_entry_t> result;
        for (LogIndex i = start; i <= end; ++i) {
            auto it = _log.find(i);
            if (it != _log.end()) {
                result.push_back(it->second);
            }
        }
        return result;
    }

    auto get_last_log_index() -> LogIndex {
        std::lock_guard lock(_mu);
        if (_log.empty()) {
            return LogIndex{0};
        }
        LogIndex max{0};
        for (const auto& [idx, _] : _log) {
            if (idx > max) {
                max = idx;
            }
        }
        return max;
    }

    auto truncate_log(LogIndex index) -> void {
        fiu_do_on("raft/persistence/truncate_log",
                  throw std::runtime_error("chaos: truncate_log"););
        std::lock_guard lock(_mu);
        auto it = _log.begin();
        while (it != _log.end()) {
            it = (it->first >= index) ? _log.erase(it) : std::next(it);
        }
        rewrite_log_file();
    }

    auto delete_log_entries_before(LogIndex index) -> void {
        std::lock_guard lock(_mu);
        auto it = _log.begin();
        while (it != _log.end()) {
            it = (it->first < index) ? _log.erase(it) : std::next(it);
        }
        rewrite_log_file();
    }

    // ── Snapshot ─────────────────────────────────────────────────────────────

    auto save_snapshot(const snapshot_t& snap) -> void {
        fiu_do_on("raft/persistence/save_snapshot",
                  throw std::runtime_error("chaos: save_snapshot"););
        std::lock_guard lock(_mu);
        atomic_write(_dir / "snapshot", snapshot_to_json(snap));
        _snapshot = snap;
    }

    auto load_snapshot() -> std::optional<snapshot_t> {
        std::lock_guard lock(_mu);
        return _snapshot;
    }

private:
    // ── Initialisation ───────────────────────────────────────────────────────

    void load_all() {
        // term
        if (auto s = read_file(_dir / "term"); s) {
            try {
                _current_term = static_cast<TermId>(std::stoull(*s));
            } catch (...) {
            }
        }

        // voted_for
        if (auto s = read_file(_dir / "voted_for"); s && *s != "none" && !s->empty()) {
            try {
                if constexpr (std::is_same_v<NodeId, std::string>) {
                    _voted_for = *s;
                } else {
                    _voted_for = static_cast<NodeId>(std::stoull(*s));
                }
            } catch (...) {
            }
        }

        // log
        auto log_path = _dir / "log";
        if (std::filesystem::exists(log_path)) {
            std::ifstream f(log_path);
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty()) {
                    continue;
                }
                try {
                    auto entry = json_to_entry(line);
                    _log[entry.index()] = entry;
                } catch (...) {
                }
            }
        }

        // snapshot
        if (auto s = read_file(_dir / "snapshot"); s && !s->empty()) {
            try {
                _snapshot = json_to_snapshot(*s);
            } catch (...) {
            }
        }
    }

    // ── Helpers ──────────────────────────────────────────────────────────────

    static auto read_file(const std::filesystem::path& p) -> std::optional<std::string> {
        if (!std::filesystem::exists(p)) {
            return std::nullopt;
        }
        std::ifstream f(p);
        if (!f) {
            return std::nullopt;
        }
        return std::string(std::istreambuf_iterator<char>(f), {});
    }

    void append_to_log_file(std::string_view content) {
        auto path = _dir / "log";
        std::ofstream f(path, std::ios::app | std::ios::binary);
        if (!f) {
            throw std::runtime_error("file_persistence: cannot open log for append");
        }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        f.flush();
    }

    /// @brief One durability barrier for the log file and the directory entry.
    ///
    /// Both are needed: fsync on the file makes the bytes durable, and fsync on
    /// the containing directory makes the *name* durable. An engine that syncs
    /// only the file can still come back from a crash with no `log` at all the
    /// first time it is created.
    ///
    /// A platform without POSIX fsync degrades to the stream flush
    /// `append_to_log_file` already did, which is what this engine did
    /// everywhere before batching existed — weaker, but not a regression.
    /// @return `true` if the barrier was taken (or the platform has none, in
    ///         which case this degrades to the stream flush the append already
    ///         did, exactly as before). `false` means the bytes are NOT
    ///         durable and the caller must not advertise them — Requirement
    ///         1.5 of `.kiro/specs/durable-append-barrier/` is the reason this
    ///         reports rather than logs and continues.
    [[nodiscard]] auto sync_log_and_directory() -> bool {
#if defined(__unix__) || defined(__APPLE__)
        fiu_do_on("raft/persistence/barrier", return false;);
        // Widens the window a real `fsync` opens, so that the ordering rule
        // above can be tested rather than merely asserted in a comment
        // (Requirement 3.4 asks for a rule made impossible to break, and a
        // rule with no test is only unlikely to be broken). `fiu_fail` returns
        // the enabled fail number, so the injected delay carries its own
        // duration in milliseconds; with FIU_ENABLE off this is `(0)` and the
        // branch compiles away.
        if (const int delay_ms = fiu_fail("raft/persistence/barrier_delay"); delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{delay_ms});
        }
        auto sync_path = [](const std::filesystem::path& p, int flags) {
            const int fd = ::open(p.c_str(), flags);
            if (fd < 0) {
                return false;
            }
            const bool ok = ::fsync(fd) == 0;
            ::close(fd);
            return ok;
        };
        const bool log_ok = sync_path(_dir / "log", O_WRONLY);
        // The directory entry matters as much as the bytes: an engine that
        // syncs only the file can still come back from a crash with no `log`
        // at all the first time it is created. A directory that cannot be
        // opened is not fatal on every filesystem, so it is not allowed to
        // fail the barrier on its own.
        (void)sync_path(_dir, O_RDONLY | O_DIRECTORY);
        return log_ok;
#else
        return true;
#endif
    }

    void atomic_write(const std::filesystem::path& path, std::string_view content) {
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
            if (!f) {
                throw std::runtime_error("file_persistence: cannot write " + tmp.string());
            }
            f.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        std::filesystem::rename(tmp, path);
    }

    /// Rewrites the whole file from `_log`. Every assigned sequence is in the
    /// file afterwards, so `_flushed_seq` catches up to `_write_seq` —
    /// `_durable_seq` deliberately does not, because `atomic_write` renames
    /// without an fsync and a barrier is still owed.
    void rewrite_log_file() {
        _flushed_seq = _write_seq;
        std::string content;
        // Write in index order
        std::vector<LogIndex> indices;
        indices.reserve(_log.size());
        for (const auto& [idx, _] : _log) {
            indices.push_back(idx);
        }
        std::sort(indices.begin(), indices.end());
        for (auto idx : indices) {
            content += entry_to_json(_log.at(idx)) + "\n";
        }
        atomic_write(_dir / "log", content);
    }

    // ── Serialisation ────────────────────────────────────────────────────────

    static auto entry_to_json(const log_entry_t& e) -> std::string {
        boost::json::object obj;
        obj["term"] = e.term();
        obj["index"] = e.index();
        obj["command"] = bytes_to_base64(e.command());
        obj["type"] = static_cast<int>(e.type());
        return boost::json::serialize(obj);
    }

    static auto json_to_entry(const std::string& s) -> log_entry_t {
        auto obj = boost::json::parse(s).as_object();
        log_entry_t e;
        e._term = static_cast<TermId>(obj["term"].as_int64());
        e._index = static_cast<LogIndex>(obj["index"].as_int64());
        e._command = base64_to_bytes(std::string(obj["command"].as_string()));
        e._type = obj.contains("type") ? static_cast<entry_type>(obj["type"].as_int64())
                                       : entry_type::normal;
        return e;
    }

    static auto snapshot_to_json(const snapshot_t& snap) -> std::string {
        boost::json::object obj;
        obj["last_included_index"] = snap.last_included_index();
        obj["last_included_term"] = snap.last_included_term();
        obj["state"] = bytes_to_base64(snap.state_machine_state());
        // primary (new) configuration nodes
        boost::json::array nodes;
        for (auto n : snap.configuration().nodes()) {
            if constexpr (std::is_same_v<NodeId, std::string>) {
                nodes.push_back(boost::json::string(n));
            } else {
                nodes.push_back(static_cast<std::uint64_t>(n));
            }
        }
        obj["nodes"] = nodes;
        obj["is_joint_consensus"] = snap.configuration().is_joint_consensus();
        if (snap.configuration().is_joint_consensus() && snap.configuration().old_nodes()) {
            boost::json::array old_nodes;
            for (auto n : *snap.configuration().old_nodes()) {
                if constexpr (std::is_same_v<NodeId, std::string>) {
                    old_nodes.push_back(boost::json::string(n));
                } else {
                    old_nodes.push_back(static_cast<std::uint64_t>(n));
                }
            }
            obj["old_nodes"] = old_nodes;
        }
        return boost::json::serialize(obj);
    }

    static auto json_to_snapshot(const std::string& s) -> snapshot_t {
        auto obj = boost::json::parse(s).as_object();
        snapshot_t snap;
        snap._last_included_index = static_cast<LogIndex>(obj["last_included_index"].as_int64());
        snap._last_included_term = static_cast<TermId>(obj["last_included_term"].as_int64());
        snap._state_machine_state = base64_to_bytes(std::string(obj["state"].as_string()));
        for (const auto& n : obj["nodes"].as_array()) {
            if constexpr (std::is_same_v<NodeId, std::string>) {
                snap._configuration._nodes.emplace_back(n.as_string());
            } else {
                snap._configuration._nodes.push_back(static_cast<NodeId>(n.as_int64()));
            }
        }
        snap._configuration._is_joint_consensus =
            obj.contains("is_joint_consensus") ? obj["is_joint_consensus"].as_bool() : false;
        if (snap._configuration._is_joint_consensus && obj.contains("old_nodes")) {
            std::vector<NodeId> old_nodes;
            for (const auto& n : obj["old_nodes"].as_array()) {
                if constexpr (std::is_same_v<NodeId, std::string>) {
                    old_nodes.emplace_back(n.as_string());
                } else {
                    old_nodes.push_back(static_cast<NodeId>(n.as_int64()));
                }
            }
            snap._configuration._old_nodes = std::move(old_nodes);
        }
        return snap;
    }

    // Base64 helpers (delegated to json_rpc_serializer's internal implementation
    // by serialising/deserialising a dummy single-byte request — instead, copy
    // the minimal codec here to avoid coupling to private methods).

    static constexpr std::string_view k_b64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    static auto bytes_to_base64(const std::vector<std::byte>& in) -> std::string {
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        for (std::size_t i = 0; i < in.size(); i += 3) {
            std::uint32_t v = static_cast<std::uint8_t>(in[i]) << 16;
            if (i + 1 < in.size()) {
                v |= static_cast<std::uint8_t>(in[i + 1]) << 8;
            }
            if (i + 2 < in.size()) {
                v |= static_cast<std::uint8_t>(in[i + 2]);
            }
            out += k_b64[(v >> 18) & 0x3F];
            out += k_b64[(v >> 12) & 0x3F];
            out += (i + 1 < in.size()) ? k_b64[(v >> 6) & 0x3F] : '=';
            out += (i + 2 < in.size()) ? k_b64[(v) & 0x3F] : '=';
        }
        return out;
    }

    static auto base64_to_bytes(const std::string& in) -> std::vector<std::byte> {
        static const auto tbl = [] {
            std::array<int8_t, 256> t{};
            t.fill(-1);
            for (int i = 0; i < 64; ++i) {
                t[static_cast<uint8_t>(k_b64[i])] = static_cast<int8_t>(i);
            }
            return t;
        }();
        std::vector<std::byte> out;
        out.reserve(in.size() * 3 / 4);
        std::uint32_t v = 0;
        int bits = 0;
        for (char c : in) {
            if (c == '=') {
                break;
            }
            int8_t b = tbl[static_cast<uint8_t>(c)];
            if (b < 0) {
                continue;
            }
            v = (v << 6) | static_cast<std::uint32_t>(b);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<std::byte>((v >> bits) & 0xFF));
            }
        }
        return out;
    }

    std::filesystem::path _dir;
    mutable std::mutex _mu;
    TermId _current_term{0};
    std::optional<NodeId> _voted_for;
    std::unordered_map<LogIndex, log_entry_t> _log;
    std::optional<snapshot_t> _snapshot;

    // ── batching state (see begin_batch) ─────────────────────────────────────
    bool _batching{false};
    std::string _batch_lines;
    /// Whether a `barrier_through()` forced this batch's lines into the file
    /// before it closed. `abort_batch()` has to rewrite when it did.
    bool _batch_flushed{false};
    /// Pre-batch value of every index the open batch has touched, so
    /// `abort_batch()` can restore the in-memory log exactly. `nullopt` means
    /// the index did not exist before the batch.
    std::unordered_map<LogIndex, std::optional<log_entry_t>> _batch_undo;

    // ── barrier state (see barrier_through) ──────────────────────────────────
    /// Whether `barrier_through()` reaches the disk at all. `false` is the
    /// benchmark's `buffered` arm and is not durable.
    bool _barriers_enabled{true};
    /// Sequences assigned to appends, in the order the appends took `_mu`.
    write_sequence _write_seq{0};
    /// The highest sequence whose bytes have been handed to the file. Equal to
    /// `_write_seq` except while a batch holds lines back.
    write_sequence _flushed_seq{0};
    /// The highest sequence a completed barrier has covered. Only ever
    /// published *after* the syscall it belongs to returned.
    write_sequence _durable_seq{0};
    /// Whether a barrier is in flight. A second caller waits for it rather
    /// than starting another — that is the coalescing.
    bool _barrier_running{false};
    /// Barriers this engine has actually issued. See `barriers_issued()`.
    std::uint64_t _barriers_issued{0};
    std::condition_variable _barrier_cv;
};

}  // namespace kythira
