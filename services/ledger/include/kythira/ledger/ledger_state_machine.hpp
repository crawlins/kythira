// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The ledger's state machine (.kiro/specs/verifiable-ledger/ tasks 3-4,
// Requirements 2.1, 2.2, 3.1-3.4, 4.1-4.4, 5.1-5.5): tables, documents, the
// journal, and digests and proofs over it, on the persistent state machine contract
// (include/raft/persistent_state_machine.hpp), so the state lives in the
// engine and a snapshot is an engine checkpoint of the ledger's range.
//
// One ledger is one Raft group over the range `l/<ledger id>/`. Each Raft
// entry is one transaction (journal_types.hpp). apply() checks it against
// the current state, builds the block and writes revisions, current
// pointers, table records, change markers, the block, the block hash's new
// Merkle nodes and the applied index in one engine batch, so a crash can
// never separate a block from its revisions or its proof (Requirement 3.5).
//
// The Merkle accumulator (merkle.hpp) has block hashes as leaves, leaf i
// being block i. Its frontier is rebuilt on open from one stored node per
// set bit of the block count; the digest of the whole journal comes from
// the frontier, a digest at an older tip and every proof read O(log n)
// stored nodes (Requirement 4.4).
//
// Outcomes of apply, all deterministic on every replica:
//
// - committed: one block, sequence one above the last (Requirement 3.1).
// - rejected: no block. Codes: invalid_entry (malformed entry), stale_block
//   (the entry's seq, previous hash or timestamp no longer follow the last
//   block: the leader prepared it against a state another entry has since
//   changed, and retries), and per-statement codes table_exists,
//   table_not_found, table_inactive, table_active, document_exists,
//   document_not_found, version_mismatch, invalid_path, document_too_large.
//   The applied index still advances.
// - divergence: seq and previous hash match but the recomputed blockHash
//   differs from the leader's. apply throws ledger_divergence_error before
//   writing anything; it derives from persistent_storage_error, which the
//   node treats as fatal for the group (persistent-state-machine design,
//   "No fsync on apply"), so the replica never applies a block it
//   disagrees with (Requirement 3.3). Replay throws again.
//
// The leader calls prepare() to fill in seq, previousBlockHash, the clamped
// timestamp (Requirement 3.4) and blockHash from its own applied state.
//
// Splits and merges are vetoed (Requirement 2.2): split_at and adopt throw.
// The policy-hook veto that keeps a split from being proposed at all, and
// its metric, are wired with the multi-raft host in task 7; this is the
// backstop.
//
// Engine layout under the range's start P (design "Engine layout"):
//
//   meta  P meta/applied            applied log index (decimal)
//   meta  P meta/tip                {seq, hash, timestamp} of the last block
//   meta  P meta/table/<name>       {active, createdSeq}
//   data  P current/<table>/<id>    {version, seq, deleted}
//   data  P rev/<table>/<id>/<v>    revision (committed view), v zero-padded
//   data  P journal/<seq>           block, seq zero-padded
//   data  P tchg/<table>            seq of the last block that changed the table
//   data  P merkle/<level>/<index>  Merkle node, 32 bytes (engine_merkle_store.hpp)
//
// Everything is inside the range, so checkpoint, ingest and drop move the
// whole ledger.

#include <kythira/ledger/engine_merkle_store.hpp>
#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/ledger_hash.hpp>
#include <kythira/ledger/merkle.hpp>

#include <raft/persistent_state_machine.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::ledger {

/// The replica's block hash differs from the leader's for the same inputs.
class ledger_divergence_error : public kythira::persistent_storage_error {
public:
    using kythira::persistent_storage_error::persistent_storage_error;
};

/// A split or merge reached a ledger group.
class ledger_split_vetoed : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

/// The range of ledger `ledger_id`: every key starting `l/<ledger id>/`.
inline auto ledger_range(std::string_view ledger_id) -> kythira::key_range {
    std::string start = "l/" + std::string(ledger_id) + "/";
    std::string end = start;
    end.back() = '0';  // the byte after '/'
    return {std::move(start), std::move(end)};
}

/// A document written by a committed transaction.
struct document_ref {
    std::string table;
    std::string id;
    std::int64_t version = 0;
};

/// What apply() returns, as Ion: {status: committed, seq, blockHash,
/// documents: [{table, id, version}]} or {status: rejected, code,
/// statement?, message}.
struct apply_result {
    bool committed = false;
    std::uint64_t seq = 0;
    hash256 block_hash{};
    std::vector<document_ref> documents;
    std::string code;
    std::optional<std::size_t> statement;
    std::string message;

    static auto rejected(std::string code, std::string message,
                         std::optional<std::size_t> statement = std::nullopt) -> apply_result {
        apply_result r;
        r.code = std::move(code);
        r.message = std::move(message);
        r.statement = statement;
        return r;
    }

    [[nodiscard]] auto to_ion() const -> ion_value {
        ion_value v = ion_value::make_struct();
        if (committed) {
            ion_value docs = ion_value::make_list();
            for (const auto& d : documents) {
                ion_value doc = ion_value::make_struct();
                doc.set("table", ion_value::string(d.table))
                    .set("id", ion_value::string(d.id))
                    .set("version", ion_value::integer(d.version));
                docs.push_back(std::move(doc));
            }
            v.set("status", ion_value::symbol("committed"))
                .set("seq", ion_value::integer(static_cast<std::int64_t>(seq)))
                .set("blockHash", hash_value(block_hash))
                .set("documents", std::move(docs));
        } else {
            v.set("status", ion_value::symbol("rejected")).set("code", ion_value::symbol(code));
            if (statement) {
                v.set("statement", ion_value::integer(static_cast<std::int64_t>(*statement)));
            }
            v.set("message", ion_value::string(message));
        }
        return v;
    }

    [[nodiscard]] auto encode() const -> std::vector<std::byte> {
        const auto bytes = to_ion().to_binary();
        std::vector<std::byte> out(bytes.size());
        std::memcpy(out.data(), bytes.data(), bytes.size());
        return out;
    }

    static auto decode(std::span<const std::byte> bytes) -> apply_result {
        namespace jd = journal_detail;
        const ion_value v = ion_value::parse(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        apply_result r;
        r.committed = jd::text_field(v, "status") == "committed";
        if (r.committed) {
            r.seq = jd::uint_field(v, "seq");
            r.block_hash = jd::hash_field(v, "blockHash");
            for (const auto& d : jd::required(v, "documents").items()) {
                r.documents.push_back({jd::text_field(d, "table"), jd::text_field(d, "id"),
                                       jd::int_field(d, "version")});
            }
        } else {
            r.code = jd::text_field(v, "code");
            if (v.find("statement") != nullptr) {
                r.statement = static_cast<std::size_t>(jd::uint_field(v, "statement"));
            }
            r.message = jd::text_field(v, "message");
        }
        return r;
    }
};

/// The last block's address and hash.
struct block_tip {
    std::uint64_t seq = 0;
    hash256 hash{};
    std::int64_t timestamp = 0;
};

/// A table's catalog record.
struct table_info {
    std::string name;
    bool active = true;
    std::uint64_t created_seq = 0;
};

/// What prepare() gives the leader: the entry to propose, or why the
/// transaction cannot commit against the current state.
struct prepared_transaction {
    std::optional<transaction_entry> entry;
    std::vector<std::byte> command;  // the encoded entry
    apply_result rejection;
};

/// A query outside the journal, e.g. a tip beyond the last block.
class ledger_query_error : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

/// The digest of the journal through block `tip`.
struct ledger_digest {
    std::uint64_t tip = 0;
    hash256 digest{};
};

/// A block, the proof that its hash is under `digest`, and that digest.
struct proven_block {
    block blk;
    std::vector<hash256> proof;
    hash256 digest{};
};

/// A revision, the proof that its hash is under `digest`, and that digest.
struct proven_revision {
    revision rev;
    std::vector<hash256> proof;
    hash256 digest{};
};

struct ledger_options {
    std::size_t max_document_bytes = default_max_document_bytes;
};

template<kythira::kv_engine Engine> class basic_ledger_state_machine {
public:
    using engine_type = Engine;

    explicit basic_ledger_state_machine(ledger_options options = {}) : options_(options) {}

    // ── persistent_state_machine ────────────────────────────────────────────

    auto open(engine_type& engine, const kythira::key_range& range) -> void {
        const std::lock_guard lock(mutex_);
        engine_ = &engine;
        range_ = range;
        prefix_ = range.start;
        load_locked();
    }

    auto apply(const std::vector<std::byte>& command, std::uint64_t index)
        -> std::vector<std::byte> {
        const std::lock_guard lock(mutex_);
        if (index <= applied_) {
            return {};  // replay of an entry already in the engine
        }
        kythira::write_batch batch;
        apply_result result;
        std::optional<block_tip> new_tip;
        try {
            const auto entry = decode_entry(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(command.data()), command.size()));
            auto outcome = build_locked(entry);
            if (auto* b = std::get_if<built>(&outcome)) {
                if (b->blk.block_hash != entry.block_hash) {
                    throw ledger_divergence_error(
                        "ledger block " + std::to_string(entry.seq) + " hash mismatch: leader " +
                        to_hex(entry.block_hash) + ", replica " + to_hex(b->blk.block_hash));
                }
                batch = std::move(b->batch);
                result = std::move(b->result);
                new_tip = block_tip{b->blk.seq, b->blk.block_hash, b->blk.timestamp};
                if (accumulator_->size() != b->blk.seq) {
                    throw kythira::persistent_storage_error(
                        "ledger Merkle accumulator holds " + std::to_string(accumulator_->size()) +
                        " leaves before block " + std::to_string(b->blk.seq));
                }
                accumulator_->append(b->blk.block_hash);
                merkle_store_->take_pending(batch);
            } else {
                result = std::move(std::get<apply_result>(outcome));
            }
        } catch (const ledger_format_error& e) {
            result = apply_result::rejected("invalid_entry", e.what());
        } catch (const ion_value_error& e) {
            result = apply_result::rejected("invalid_entry", e.what());
        }
        batch.put(kythira::column_family::meta, key_applied(), std::to_string(index));
        kythira::engine_sequence seq{};
        try {
            seq = engine_->write(batch);
        } catch (...) {
            load_locked();  // the accumulator's frontier moved; the engine did not
            throw;
        }
        applied_ = index;
        if (new_tip) {
            tip_ = new_tip;
        }
        seqs_.emplace_back(index, seq);
        prune_locked();
        return result.encode();
    }

    [[nodiscard]] auto applied_index() const -> std::uint64_t {
        const std::lock_guard lock(mutex_);
        return applied_;
    }

    [[nodiscard]] auto durable_applied_index() const -> std::uint64_t {
        const std::lock_guard lock(mutex_);
        const auto durable = engine_->durable_sequence();
        auto best = durable_floor_;
        for (const auto& [index, seq] : seqs_) {
            if (seq <= durable) {
                best = index;
            }
        }
        return best;
    }

    auto checkpoint(const std::filesystem::path& dir) -> kythira::snapshot_manifest {
        const std::lock_guard lock(mutex_);
        return engine_->checkpoint(range_, dir);
    }

    auto ingest(const std::filesystem::path& dir, const kythira::snapshot_manifest& manifest,
                std::uint64_t index) -> void {
        const std::lock_guard lock(mutex_);
        engine_->ingest(range_, dir, manifest);
        const auto seq = engine_->write(kythira::write_batch{}.put(
            kythira::column_family::meta, key_applied(), std::to_string(index)));
        load_locked();
        seqs_.emplace_back(index, seq);
    }

    auto drop() -> void {
        const std::lock_guard lock(mutex_);
        engine_->drop(range_);
        load_locked();  // an empty range: no applied index, no tip, no leaves
    }

    // ── persistent_splittable_state_machine: always vetoed ──────────────────

    auto split_at(const std::vector<std::string>& /*keys*/) -> std::vector<kythira::key_range> {
        throw ledger_split_vetoed("a ledger group never splits");
    }

    auto adopt(const kythira::key_range& /*range*/) -> void {
        throw ledger_split_vetoed("a ledger group never merges");
    }

    // ── Leader side ─────────────────────────────────────────────────────────

    /// Build the entry for a transaction against the applied state: the
    /// next seq, the last block's hash, `timestamp` clamped to the last
    /// block's, and the block hash every replica will recompute.
    [[nodiscard]] auto prepare(std::string tx_id, std::int64_t timestamp,
                               const std::vector<ion_value>& statement_ions) const
        -> prepared_transaction {
        const std::lock_guard lock(mutex_);
        prepared_transaction out;
        transaction_entry e;
        e.tx_id = std::move(tx_id);
        e.block_timestamp = tip_ ? std::max(timestamp, tip_->timestamp) : timestamp;
        e.seq = tip_ ? tip_->seq + 1 : 0;
        e.previous_block_hash = tip_ ? tip_->hash : genesis_previous_hash;
        try {
            if (!valid_id(e.tx_id)) {
                throw ledger_format_error("invalid transaction id");
            }
            if (statement_ions.empty()) {
                throw ledger_format_error("statements is a non-empty list");
            }
            for (const auto& s : statement_ions) {
                e.statements.push_back(parse_statement(s));
            }
            auto outcome = build_locked(e);
            if (auto* r = std::get_if<apply_result>(&outcome)) {
                out.rejection = std::move(*r);
                return out;
            }
            e.block_hash = std::get<built>(outcome).blk.block_hash;
        } catch (const ledger_format_error& ex) {
            out.rejection = apply_result::rejected("invalid_entry", ex.what());
            return out;
        } catch (const ion_value_error& ex) {
            out.rejection = apply_result::rejected("invalid_entry", ex.what());
            return out;
        }
        const auto bytes = encode_entry(e);
        out.command.resize(bytes.size());
        std::memcpy(out.command.data(), bytes.data(), bytes.size());
        out.entry = std::move(e);
        return out;
    }

    // ── Reads (Requirement 5.5) ─────────────────────────────────────────────

    [[nodiscard]] auto tip() const -> std::optional<block_tip> {
        const std::lock_guard lock(mutex_);
        return tip_;
    }

    [[nodiscard]] auto table(std::string_view name) const -> std::optional<table_info> {
        const std::lock_guard lock(mutex_);
        return stored_table(name);
    }

    [[nodiscard]] auto tables() const -> std::vector<table_info> {
        const std::lock_guard lock(mutex_);
        std::vector<table_info> out;
        const std::string from = prefix_ + "meta/table/";
        engine_->scan(kythira::column_family::meta, prefix_range(from),
                      kythira::scan_direction::forward,
                      [&](std::string_view key, std::string_view value) {
                          out.push_back(table_from(key.substr(from.size()), value));
                          return true;
                      });
        return out;
    }

    /// The current revision of a document; nullopt if it never existed or
    /// was deleted.
    [[nodiscard]] auto get_document(std::string_view table_name, std::string_view id) const
        -> std::optional<revision> {
        const std::lock_guard lock(mutex_);
        const auto cur = stored_current(table_name, id);
        if (!cur || cur->deleted) {
            return std::nullopt;
        }
        return stored_revision(table_name, id, cur->version);
    }

    /// One revision of a document, a delete's included.
    [[nodiscard]] auto get_revision(std::string_view table_name, std::string_view id,
                                    std::int64_t version) const -> std::optional<revision> {
        const std::lock_guard lock(mutex_);
        return stored_revision(table_name, id, version);
    }

    [[nodiscard]] auto get_block(std::uint64_t seq) const -> std::optional<block> {
        const std::lock_guard lock(mutex_);
        return stored_block(seq);
    }

    // ── Digests and proofs (Requirements 4.1-4.4) ───────────────────────────

    /// The digest of the whole journal and its tip; nullopt while empty.
    /// From the in-memory frontier, so no engine read.
    [[nodiscard]] auto get_digest() const -> std::optional<ledger_digest> {
        const std::lock_guard lock(mutex_);
        if (!tip_) {
            return std::nullopt;
        }
        return ledger_digest{tip_->seq, *accumulator_->digest()};
    }

    /// The digest as of block `tip` (one stored node per set bit of
    /// tip + 1). Throws ledger_query_error beyond the last block.
    [[nodiscard]] auto digest_at(std::uint64_t tip) const -> hash256 {
        const std::lock_guard lock(mutex_);
        check_tip(0, tip);
        return accumulator_->digest_at(tip + 1);
    }

    /// Block `seq` with the proof that its hash folds with `dot` to the
    /// digest at `tip`. Throws ledger_query_error when tip < seq or tip is
    /// beyond the last block.
    [[nodiscard]] auto get_block_proof(std::uint64_t seq, std::uint64_t tip) const -> proven_block {
        const std::lock_guard lock(mutex_);
        check_tip(seq, tip);
        auto blk = stored_block(seq);
        if (!blk) {
            throw kythira::persistent_storage_error("ledger block " + std::to_string(seq) +
                                                    " is missing");
        }
        proven_block out{std::move(*blk), accumulator_->proof(seq, tip + 1),
                         accumulator_->digest_at(tip + 1)};
        return out;
    }

    /// A revision with the proof that its hash folds with `dot` to the
    /// digest at `tip`: its path in its block's entries tree, the block's
    /// header hash and previous hash, then the block's accumulator path.
    /// nullopt for a revision that does not exist.
    [[nodiscard]] auto get_revision_proof(std::string_view table_name, std::string_view id,
                                          std::int64_t version, std::uint64_t tip) const
        -> std::optional<proven_revision> {
        const std::lock_guard lock(mutex_);
        auto rev = stored_revision(table_name, id, version);
        if (!rev) {
            return std::nullopt;
        }
        check_tip(rev->seq, tip);
        const auto blk = stored_block(rev->seq);
        if (!blk) {
            throw kythira::persistent_storage_error("ledger block " + std::to_string(rev->seq) +
                                                    " is missing");
        }
        const auto it = std::ranges::find_if(blk->revisions, [&](const revision& r) {
            return r.table == table_name && r.id == id && r.version == version;
        });
        if (it == blk->revisions.end()) {
            throw kythira::persistent_storage_error("ledger block " + std::to_string(rev->seq) +
                                                    " does not hold the revision");
        }
        auto proof = blk->revision_proof(static_cast<std::size_t>(it - blk->revisions.begin()));
        const auto path = accumulator_->proof(rev->seq, tip + 1);
        proof.insert(proof.end(), path.begin(), path.end());
        return proven_revision{std::move(*rev), std::move(proof), accumulator_->digest_at(tip + 1)};
    }

    [[nodiscard]] auto range() const -> kythira::key_range {
        const std::lock_guard lock(mutex_);
        return range_;
    }

private:
    struct current_state {
        std::int64_t version = 0;
        std::uint64_t seq = 0;
        bool deleted = false;
    };

    /// A document as the transaction being built sees it.
    struct doc_state {
        current_state current;
        std::optional<ion_value> data;  // loaded on first need; nullopt when deleted
        bool data_loaded = false;
    };

    struct built {
        block blk;
        kythira::write_batch batch;
        apply_result result;
    };

    // ── Keys ────────────────────────────────────────────────────────────────

    static auto pad(std::uint64_t n) -> std::string {
        std::array<char, 24> buf{};
        std::snprintf(buf.data(), buf.size(), "%020llu", static_cast<unsigned long long>(n));
        return buf.data();
    }

    static auto prefix_range(const std::string& prefix) -> kythira::key_range {
        std::string end = prefix;
        end.back() = static_cast<char>(end.back() + 1);
        return {prefix, std::move(end)};
    }

    [[nodiscard]] auto key_applied() const -> std::string { return prefix_ + "meta/applied"; }
    [[nodiscard]] auto key_tip() const -> std::string { return prefix_ + "meta/tip"; }
    [[nodiscard]] auto key_table(std::string_view name) const -> std::string {
        return prefix_ + "meta/table/" + std::string(name);
    }
    [[nodiscard]] auto key_current(std::string_view table_name, std::string_view id) const
        -> std::string {
        return prefix_ + "current/" + std::string(table_name) + "/" + std::string(id);
    }
    [[nodiscard]] auto key_revision(std::string_view table_name, std::string_view id,
                                    std::int64_t version) const -> std::string {
        return prefix_ + "rev/" + std::string(table_name) + "/" + std::string(id) + "/" +
               pad(static_cast<std::uint64_t>(version));
    }
    [[nodiscard]] auto key_journal(std::uint64_t seq) const -> std::string {
        return prefix_ + "journal/" + pad(seq);
    }
    [[nodiscard]] auto key_table_change(std::string_view table_name) const -> std::string {
        return prefix_ + "tchg/" + std::string(table_name);
    }

    // ── Stored records ──────────────────────────────────────────────────────

    static auto as_string(const std::vector<std::uint8_t>& bytes) -> std::string {
        return {bytes.begin(), bytes.end()};
    }

    static auto parse_stored(std::string_view value) -> ion_value {
        return ion_value::parse(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    }

    static auto table_from(std::string_view name, std::string_view value) -> table_info {
        const ion_value v = parse_stored(value);
        const auto active = journal_detail::required(v, "active").as_bool();
        if (!active) {
            throw ledger_format_error("table record: active is not a bool");
        }
        return {std::string(name), *active, journal_detail::uint_field(v, "createdSeq")};
    }

    static auto table_record(const table_info& t) -> std::string {
        ion_value v = ion_value::make_struct();
        v.set("active", ion_value::boolean(t.active))
            .set("createdSeq", ion_value::integer(static_cast<std::int64_t>(t.created_seq)));
        return as_string(v.to_binary());
    }

    static auto current_record(const current_state& c) -> std::string {
        ion_value v = ion_value::make_struct();
        v.set("version", ion_value::integer(c.version))
            .set("seq", ion_value::integer(static_cast<std::int64_t>(c.seq)))
            .set("deleted", ion_value::boolean(c.deleted));
        return as_string(v.to_binary());
    }

    [[nodiscard]] auto stored_table(std::string_view name) const -> std::optional<table_info> {
        const auto v = engine_->get(kythira::column_family::meta, key_table(name));
        if (!v) {
            return std::nullopt;
        }
        return table_from(name, *v);
    }

    [[nodiscard]] auto stored_current(std::string_view table_name, std::string_view id) const
        -> std::optional<current_state> {
        const auto v = engine_->get(kythira::column_family::data, key_current(table_name, id));
        if (!v) {
            return std::nullopt;
        }
        const ion_value c = parse_stored(*v);
        const auto deleted = journal_detail::required(c, "deleted").as_bool();
        if (!deleted) {
            throw ledger_format_error("current record: deleted is not a bool");
        }
        return current_state{journal_detail::int_field(c, "version"),
                             journal_detail::uint_field(c, "seq"), *deleted};
    }

    [[nodiscard]] auto stored_revision(std::string_view table_name, std::string_view id,
                                       std::int64_t version) const -> std::optional<revision> {
        const auto v =
            engine_->get(kythira::column_family::data, key_revision(table_name, id, version));
        if (!v) {
            return std::nullopt;
        }
        return revision::from_ion(parse_stored(*v));
    }

    auto load_locked() -> void {
        const auto applied = engine_->get(kythira::column_family::meta, key_applied());
        applied_ = applied ? std::stoull(*applied) : 0U;
        // What open() reads back was recovered, so it is durable.
        seqs_.clear();
        durable_floor_ = applied_;
        tip_.reset();
        if (const auto t = engine_->get(kythira::column_family::meta, key_tip())) {
            const ion_value v = parse_stored(*t);
            tip_ = block_tip{journal_detail::uint_field(v, "seq"),
                             journal_detail::hash_field(v, "hash"),
                             journal_detail::time_field(v, "timestamp")};
        }
        // The frontier, from one stored node per set bit of the leaf count.
        accumulator_.reset();
        merkle_store_ =
            std::make_unique<engine_merkle_store<Engine>>(*engine_, prefix_ + "merkle/");
        accumulator_.emplace(*merkle_store_, tip_ ? tip_->seq + 1 : 0);
    }

    /// Forget sequence pairs the engine has made durable.
    auto prune_locked() -> void {
        const auto durable = engine_->durable_sequence();
        std::size_t drop = 0;
        while (drop < seqs_.size() && seqs_[drop].second <= durable) {
            durable_floor_ = seqs_[drop].first;
            ++drop;
        }
        seqs_.erase(seqs_.begin(), seqs_.begin() + static_cast<std::ptrdiff_t>(drop));
    }

    // ── Building a block ────────────────────────────────────────────────────

    /// Check `e` against the applied state and build its block and batch,
    /// or say why it cannot commit. Reads only; the caller writes.
    [[nodiscard]] auto build_locked(const transaction_entry& e) const
        -> std::variant<built, apply_result> {
        const std::uint64_t next_seq = tip_ ? tip_->seq + 1 : 0;
        const hash256 previous = tip_ ? tip_->hash : genesis_previous_hash;
        if (e.seq != next_seq || e.previous_block_hash != previous ||
            (tip_ && e.block_timestamp < tip_->timestamp)) {
            return apply_result::rejected("stale_block",
                                          "entry prepared for block " + std::to_string(e.seq) +
                                              ", next block is " + std::to_string(next_seq));
        }

        std::map<std::string, table_info, std::less<>> tables;
        std::map<std::pair<std::string, std::string>, doc_state> docs;
        std::map<std::string, bool, std::less<>> changed_tables;

        const auto find_table = [&](const std::string& name) -> std::optional<table_info> {
            if (const auto it = tables.find(name); it != tables.end()) {
                return it->second;
            }
            return stored_table(name);
        };
        const auto find_doc = [&](const std::string& table_name,
                                  const std::string& id) -> doc_state* {
            const auto key = std::make_pair(table_name, id);
            if (auto it = docs.find(key); it != docs.end()) {
                return &it->second;
            }
            const auto cur = stored_current(table_name, id);
            if (!cur) {
                return nullptr;
            }
            return &docs.emplace(key, doc_state{*cur, std::nullopt, false}).first->second;
        };
        const auto too_large = [&](const ion_value& v) {
            return v.to_binary().size() > options_.max_document_bytes;
        };

        block blk;
        blk.seq = e.seq;
        blk.tx_id = e.tx_id;
        blk.timestamp = e.block_timestamp;
        blk.previous_block_hash = previous;
        apply_result result;

        for (std::size_t i = 0; i < e.statements.size(); ++i) {
            const statement& s = e.statements[i];
            const auto reject = [&](const char* code, std::string message) {
                return apply_result::rejected(code, std::move(message), i);
            };
            blk.statements.push_back(s.ion);
            auto t = find_table(s.table);

            if (!s.is_document_op()) {
                switch (s.op) {
                    case statement_op::create_table:
                        if (t) {
                            return reject("table_exists", "table " + s.table + " exists");
                        }
                        tables[s.table] = table_info{s.table, true, e.seq};
                        break;
                    case statement_op::drop_table:
                        if (!t) {
                            return reject("table_not_found", "no table " + s.table);
                        }
                        if (!t->active) {
                            return reject("table_inactive", "table " + s.table + " is dropped");
                        }
                        t->active = false;
                        tables[s.table] = *t;
                        break;
                    case statement_op::undrop_table:
                        if (!t) {
                            return reject("table_not_found", "no table " + s.table);
                        }
                        if (t->active) {
                            return reject("table_active", "table " + s.table + " is active");
                        }
                        t->active = true;
                        tables[s.table] = *t;
                        break;
                    default:
                        break;
                }
                continue;
            }

            if (!t) {
                return reject("table_not_found", "no table " + s.table);
            }
            if (!t->active) {
                return reject("table_inactive", "table " + s.table + " is dropped");
            }
            doc_state* doc = find_doc(s.table, s.id);
            revision r;
            r.table = s.table;
            r.id = s.id;
            r.seq = e.seq;
            r.tx_id = e.tx_id;
            r.tx_time = e.block_timestamp;

            if (s.op == statement_op::insert) {
                if (doc != nullptr) {
                    return reject("document_exists", "document " + s.id + " exists");
                }
                if (too_large(s.value)) {
                    return reject("document_too_large", "document over the size limit");
                }
                r.version = 0;
                r.data = s.value;
                doc = &docs[{s.table, s.id}];
            } else {
                if (doc == nullptr || doc->current.deleted) {
                    return reject("document_not_found", "no document " + s.id);
                }
                if (s.if_version && *s.if_version != doc->current.version) {
                    return reject("version_mismatch", "document " + s.id + " is at version " +
                                                          std::to_string(doc->current.version));
                }
                r.version = doc->current.version + 1;
                if (s.op == statement_op::replace) {
                    if (too_large(s.value)) {
                        return reject("document_too_large", "document over the size limit");
                    }
                    r.data = s.value;
                } else if (s.op == statement_op::patch) {
                    if (!doc->data_loaded) {
                        const auto stored = stored_revision(s.table, s.id, doc->current.version);
                        if (!stored) {
                            throw ledger_format_error("missing revision for current document");
                        }
                        doc->data = stored->data;
                        doc->data_loaded = true;
                    }
                    ion_value patched = doc->data.value_or(ion_value{});
                    try {
                        for (const auto& set : s.sets) {
                            patch_set_path(patched, set.path, set.value);
                        }
                        for (const auto& path : s.removes) {
                            patch_remove_path(patched, path);
                        }
                    } catch (const patch_error& ex) {
                        return reject("invalid_path", ex.what());
                    } catch (const ion_value_error& ex) {
                        return reject("invalid_path", ex.what());
                    }
                    if (too_large(patched)) {
                        return reject("document_too_large", "document over the size limit");
                    }
                    r.data = std::move(patched);
                }
                // delete: no data
            }
            r.hash = r.compute_hash();
            doc->current = current_state{r.version, e.seq, !r.data.has_value()};
            doc->data = r.data;
            doc->data_loaded = true;
            changed_tables[s.table] = true;
            result.documents.push_back({r.table, r.id, r.version});
            blk.revisions.push_back(std::move(r));
        }

        blk.entries_hash = blk.compute_entries_hash();
        blk.block_hash = blk.compute_block_hash();

        built out;
        auto& batch = out.batch;
        using kythira::column_family;
        for (const auto& [name, info] : tables) {
            batch.put(column_family::meta, key_table(name), table_record(info));
        }
        for (const auto& r : blk.revisions) {
            batch.put(column_family::data, key_revision(r.table, r.id, r.version),
                      as_string(r.to_ion().to_binary()));
        }
        for (const auto& [key, doc] : docs) {
            if (doc.current.seq == e.seq) {
                batch.put(column_family::data, key_current(key.first, key.second),
                          current_record(doc.current));
            }
        }
        for (const auto& [name, changed] : changed_tables) {
            batch.put(column_family::data, key_table_change(name), std::to_string(e.seq));
        }
        batch.put(column_family::data, key_journal(e.seq), as_string(blk.to_ion().to_binary()));
        ion_value tip = ion_value::make_struct();
        tip.set("seq", ion_value::integer(static_cast<std::int64_t>(blk.seq)))
            .set("hash", hash_value(blk.block_hash))
            .set("timestamp", timestamp_value(blk.timestamp));
        batch.put(column_family::meta, key_tip(), as_string(tip.to_binary()));

        result.committed = true;
        result.seq = blk.seq;
        result.block_hash = blk.block_hash;
        out.result = std::move(result);
        out.blk = std::move(blk);
        return out;
    }

    auto check_tip(std::uint64_t seq, std::uint64_t tip) const -> void {
        if (!tip_ || tip > tip_->seq) {
            throw ledger_query_error("tip " + std::to_string(tip) + " is beyond the last block");
        }
        if (tip < seq) {
            throw ledger_query_error("tip " + std::to_string(tip) + " is below block " +
                                     std::to_string(seq));
        }
    }

    [[nodiscard]] auto stored_block(std::uint64_t seq) const -> std::optional<block> {
        const auto v = engine_->get(kythira::column_family::data, key_journal(seq));
        if (!v) {
            return std::nullopt;
        }
        return block::from_ion(parse_stored(*v));
    }

    ledger_options options_;
    mutable std::mutex mutex_;
    engine_type* engine_ = nullptr;
    kythira::key_range range_;
    std::string prefix_;
    std::uint64_t applied_ = 0;
    std::uint64_t durable_floor_ = 0;
    std::vector<std::pair<std::uint64_t, kythira::engine_sequence>> seqs_;
    std::optional<block_tip> tip_;
    std::unique_ptr<engine_merkle_store<Engine>> merkle_store_;
    std::optional<merkle_accumulator<engine_merkle_store<Engine>>> accumulator_;
};

}  // namespace kythira::ledger
