// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Leader-side transactions for a ledger (.kiro/specs/verifiable-ledger/
// task 5, Requirements 7.1-7.7; design "Read set and conflict markers" and
// "Sessions").
//
// begin() records the snapshot S, the number of blocks applied, and every
// read in the session sees the newest revision in a block below S. Reads go
// into the read set (documents with the version seen, tables scanned);
// writes are buffered as statements. commit() hands the statements and the
// read set to the state machine's prepare(), which checks them against the
// applied state and returns the Raft entry to propose. Every replica checks
// the read set again in apply and rejects the transaction with
// occ_conflict if anything it read changed in a block at or after S, so
// the outcome is serializable in commit order, phantoms included for table
// scans (Requirement 7.4).
//
// Sessions live only in the leader's memory. A token names the leader term
// it began in; leadership_changed() fails every open session, and a token
// from an older term is session_expired, which the client retries
// (Requirement 7.6). A session past its timeout is expired the same way.
//
// A session does not read its own buffered writes: reads always come from
// the snapshot, and the statements apply in order at commit.
//
// Proposing the entry, and keeping one transaction per ledger in flight so
// prepare's block hash still matches when it applies, is the node's job
// (task 7); a stale_block result means prepare again.

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace kythira::ledger {

/// A session operation that cannot proceed. `retryable` errors are worth
/// running the whole transaction again (Requirement 7.6).
class ledger_session_error : public std::runtime_error {
public:
    ledger_session_error(std::string code, const std::string& message, bool retryable)
        : std::runtime_error(code + ": " + message),
          code_(std::move(code)),
          retryable_(retryable) {}

    [[nodiscard]] auto code() const -> const std::string& { return code_; }
    [[nodiscard]] auto retryable() const -> bool { return retryable_; }

private:
    std::string code_;
    bool retryable_;
};

/// Session limits (Requirement 7.5). The statement limits must not exceed
/// the state machine's ledger_options, which apply enforces again.
struct session_limits {
    std::chrono::milliseconds timeout{std::chrono::seconds{30}};
    std::size_t max_changed_documents = 40;
    std::size_t max_statement_bytes = std::size_t{4} << 20U;
    std::size_t max_open_sessions = 10'000;
};

/// The disk floor (Requirement 10.3): transactions stop with
/// insufficient_storage before the data directory's free space falls below
/// `floor_bytes`. Sealing, offload and reads keep running, since they are
/// how space comes back.
struct disk_guard {
    std::filesystem::path path;
    std::uint64_t floor_bytes = std::uint64_t{1} << 30U;
    /// Free bytes at a path; std::filesystem::space by default, a stub in
    /// tests.
    std::function<std::uint64_t(const std::filesystem::path&)> free_bytes =
        [](const std::filesystem::path& p) { return std::filesystem::space(p).available; };

    /// Why transactions must stop, or nullopt if there is room.
    [[nodiscard]] auto check() const -> std::optional<std::string> {
        const auto free = free_bytes(path);
        if (free < floor_bytes) {
            return path.string() + " has " + std::to_string(free) + " bytes free, below the " +
                   std::to_string(floor_bytes) + " byte floor";
        }
        return std::nullopt;
    }
};

/// A finished session's statements and read set, not yet prepared.
struct pending_transaction {
    std::string tx_id;
    std::vector<ion_value> statements;
    std::optional<read_set> reads;

    /// The entry against `sm`'s applied state now.
    template<typename StateMachine>
    [[nodiscard]] auto prepare(const StateMachine& sm, std::int64_t timestamp) const
        -> prepared_transaction {
        return sm.prepare(tx_id, timestamp, statements, reads);
    }
};

/// True for apply/prepare rejection codes a client should retry by running
/// the transaction again (Requirement 7.6).
inline auto retryable_rejection(std::string_view code) -> bool {
    return code == "occ_conflict" || code == "stale_block" || code == "session_expired";
}

template<typename StateMachine> class ledger_session_manager {
public:
    using clock = std::chrono::steady_clock;
    using clock_fn = std::function<clock::time_point()>;

    explicit ledger_session_manager(
        const StateMachine& sm, session_limits limits = {},
        clock_fn now = [] { return clock::now(); })
        : sm_(&sm), limits_(limits), now_(std::move(now)) {}

    /// Refuse new transactions and commits while `guard` says the disk is
    /// too full (Requirement 10.3).
    auto set_disk_guard(disk_guard guard) -> void {
        const std::lock_guard lock(mutex_);
        guard_ = std::move(guard);
    }

    /// Start a transaction on the leader of `term`; returns its token.
    auto begin(std::uint64_t term) -> std::string {
        const std::lock_guard lock(mutex_);
        expire_locked();
        check_disk_locked();
        if (term < term_) {
            throw ledger_session_error("session_expired",
                                       "term " + std::to_string(term) + " is not the current term",
                                       true);
        }
        term_ = term;
        if (sessions_.size() >= limits_.max_open_sessions) {
            throw ledger_session_error("too_many_sessions", "open session limit reached", true);
        }
        const std::string token = std::to_string(term) + ":" + new_id();
        session s;
        s.term = term;
        s.reads.blocks = sm_->block_count();
        s.deadline = now_() + limits_.timeout;
        sessions_.emplace(token, std::move(s));
        return token;
    }

    /// The snapshot S the session reads at.
    [[nodiscard]] auto snapshot(const std::string& token) -> std::uint64_t {
        const std::lock_guard lock(mutex_);
        return find_locked(token).reads.blocks;
    }

    /// Read a document at the session's snapshot; recorded in the read set
    /// with the version seen (-1 for none).
    auto get(const std::string& token, std::string_view table, std::string_view id)
        -> std::optional<revision> {
        const std::lock_guard lock(mutex_);
        auto& s = find_locked(token);
        auto r = sm_->get_document_at(table, id, s.reads.blocks);
        const auto key = std::make_pair(std::string(table), std::string(id));
        if (s.documents_read.insert(key).second) {
            s.reads.documents.push_back(
                {std::string(table), std::string(id), r ? r->version : std::int64_t{-1}});
        }
        return r;
    }

    /// Scan a table at the session's snapshot, one page at a time; the
    /// table goes into the read set, so any later change to it, an insert
    /// included, conflicts.
    auto scan(const std::string& token, std::string_view table,
              std::optional<std::string> after = std::nullopt, std::size_t limit = 100)
        -> scan_page {
        const std::lock_guard lock(mutex_);
        auto& s = find_locked(token);
        if (s.tables_read.insert(std::string(table)).second) {
            s.reads.tables.emplace_back(table);
        }
        return sm_->scan_table_at(table, s.reads.blocks, std::move(after),
                                  std::max<std::size_t>(limit, 1));
    }

    /// Look documents up by an active equality index at the session's
    /// snapshot (Requirement 6.2); the looked-up key goes into the read
    /// set, so a document moving into or out of it later conflicts.
    /// Throws ledger_query_error if the index is missing or still building.
    auto lookup(const std::string& token, std::string_view table, std::string_view index,
                const ion_value& value, std::optional<std::string> after = std::nullopt,
                std::size_t limit = 100) -> scan_page {
        const std::lock_guard lock(mutex_);
        auto& s = find_locked(token);
        auto page = sm_->find_by_index(table, index, value, s.reads.blocks, std::move(after),
                                       std::max<std::size_t>(limit, 1));
        // An index created at or after the snapshot has no change markers
        // for what happened before it existed, so the lookup is recorded as
        // a table read instead: any change since the snapshot conflicts.
        const auto info = sm_->get_index(table, index);
        if (info && info->created_seq >= s.reads.blocks) {
            if (s.tables_read.insert(std::string(table)).second) {
                s.reads.tables.emplace_back(table);
            }
            return page;
        }
        const auto key = std::make_tuple(std::string(table), std::string(index), value.hash());
        if (s.index_keys_read.insert(key).second) {
            s.reads.indexes.push_back({std::string(table), std::string(index), value.hash()});
        }
        return page;
    }

    /// Buffer a statement (journal_types.hpp `statements::` builders).
    auto add(const std::string& token, const ion_value& statement_ion) -> void {
        const std::lock_guard lock(mutex_);
        auto& s = find_locked(token);
        const auto st = parse_statement(statement_ion);  // shape errors surface now
        const auto bytes = statement_ion.to_binary().size();
        const std::size_t docs = s.changed_documents + (st.is_document_op() ? 1U : 0U);
        if (docs > limits_.max_changed_documents ||
            s.statement_bytes + bytes > limits_.max_statement_bytes) {
            throw ledger_session_error("transaction_too_large",
                                       "the transaction is over its statement limits", false);
        }
        s.changed_documents = docs;
        s.statement_bytes += bytes;
        s.statements.push_back(statement_ion);
    }

    /// Buffer an insert of `value` into `table` under a new id; returns it.
    auto insert(const std::string& token, std::string_view table, ion_value value) -> std::string {
        std::string id = new_id();
        add(token, statements::insert(table, id, std::move(value)));
        return id;
    }

    /// End the session and hand back what it buffered, for a caller that
    /// prepares (and re-prepares after stale_block) under its own commit
    /// lock. Throws empty_transaction if nothing was buffered.
    auto finish(const std::string& token) -> pending_transaction {
        session s;
        {
            const std::lock_guard lock(mutex_);
            s = std::move(find_locked(token));
            sessions_.erase(token);
            check_disk_locked();
        }
        if (s.statements.empty()) {
            throw ledger_session_error("empty_transaction", "nothing to commit", false);
        }
        pending_transaction out;
        out.tx_id = new_id();
        out.statements = std::move(s.statements);
        if (!s.reads.empty()) {
            out.reads = std::move(s.reads);
        }
        return out;
    }

    /// End the session and build the entry to propose, or the rejection.
    /// `timestamp` is the leader's wall clock in ms (prepare clamps it).
    auto commit(const std::string& token, std::int64_t timestamp) -> prepared_transaction {
        return finish(token).prepare(*sm_, timestamp);
    }

    /// Discard a session.
    auto abort(const std::string& token) -> void {
        const std::lock_guard lock(mutex_);
        sessions_.erase(token);
    }

    /// A one-call transaction (Requirement 7.7): blind writes, no reads.
    auto execute(std::uint64_t term, const std::vector<ion_value>& statements_to_run,
                 std::int64_t timestamp) -> prepared_transaction {
        return execute_pending(term, statements_to_run).prepare(*sm_, timestamp);
    }

    /// execute() without the prepare, for a caller with its own commit
    /// lock; the statements are checked against the session limits.
    auto execute_pending(std::uint64_t term, const std::vector<ion_value>& statements_to_run)
        -> pending_transaction {
        const auto token = begin(term);
        try {
            for (const auto& st : statements_to_run) {
                add(token, st);
            }
        } catch (...) {
            abort(token);
            throw;
        }
        return finish(token);
    }

    /// This node lost leadership, or a new term began: every open session
    /// fails with session_expired (Requirement 7.6).
    auto leadership_changed(std::uint64_t new_term) -> void {
        const std::lock_guard lock(mutex_);
        sessions_.clear();
        term_ = std::max(term_, new_term);
    }

    [[nodiscard]] auto open_sessions() -> std::size_t {
        const std::lock_guard lock(mutex_);
        expire_locked();
        return sessions_.size();
    }

private:
    struct session {
        std::uint64_t term = 0;
        clock::time_point deadline{};
        read_set reads;
        std::set<std::pair<std::string, std::string>> documents_read;
        std::set<std::string> tables_read;
        std::set<std::tuple<std::string, std::string, hash256>> index_keys_read;
        std::vector<ion_value> statements;
        std::size_t changed_documents = 0;
        std::size_t statement_bytes = 0;
    };

    auto check_disk_locked() const -> void {
        if (guard_) {
            if (auto why = guard_->check()) {
                throw ledger_session_error("insufficient_storage", *why, false);
            }
        }
    }

    auto expire_locked() -> void {
        const auto now = now_();
        std::erase_if(sessions_, [&](const auto& kv) { return kv.second.deadline <= now; });
    }

    auto find_locked(const std::string& token) -> session& {
        expire_locked();
        const auto it = sessions_.find(token);
        if (it == sessions_.end() || it->second.term < term_) {
            throw ledger_session_error("session_expired",
                                       "no open session for this token on this leader", true);
        }
        return it->second;
    }

    const StateMachine* sm_;
    session_limits limits_;
    clock_fn now_;
    std::mutex mutex_;
    std::uint64_t term_ = 0;
    std::optional<disk_guard> guard_;
    std::map<std::string, session, std::less<>> sessions_;
};

}  // namespace kythira::ledger
