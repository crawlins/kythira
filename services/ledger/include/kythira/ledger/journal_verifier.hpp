// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Offline verification (.kiro/specs/verifiable-ledger/ task 10,
// Requirements 9.1-9.3): re-hash a journal end to end without trusting
// the cluster, and check a block or revision proof against a saved digest.
//
// journal_verifier takes blocks in order and checks, for each: the
// sequence number follows, previousBlockHash is the previous block's hash
// (32 zero bytes before block 0), the timestamp does not run backwards,
// every revision hash matches its data and metadata, every revision names
// this block and a table and id one of its statements wrote (those labels
// are outside the revision hash), entriesHash matches
// the statements and revisions, and blockHash matches its fields. It keeps
// only the accumulator's frontier, so memory does not grow with the
// journal, and checks a saved digest when it reaches that digest's tip.
// The first failure is kept and names the block (Requirement 9.3).
//
// Sources: blocks from any state machine (a data directory opened with
// its engine), segment files, and export directories (Ion text or Ion
// binary; a JSON export has lost the Ion types, so it cannot be re-hashed
// and is refused).
//
// Proof files are the Ion of what GetBlock and GetRevision return:
// {block | revision, proof: [blob...], digest: blob, tip: int}.

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_export.hpp>
#include <kythira/ledger/journal_segments.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/ledger_hash.hpp>
#include <kythira/ledger/merkle.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::ledger {

/// A digest a client saved: the digest of blocks 0..tip.
struct saved_digest {
    std::uint64_t tip = 0;
    hash256 digest{};

    [[nodiscard]] auto to_ion() const -> ion_value {
        ion_value v = ion_value::make_struct();
        v.set("tip", ion_value::integer(static_cast<std::int64_t>(tip)))
            .set("digest", hash_value(digest));
        return v;
    }

    static auto from_ion(const ion_value& v) -> saved_digest {
        return {journal_detail::uint_field(v, "tip"), journal_detail::hash_field(v, "digest")};
    }
};

/// Where and why verification failed.
struct verify_failure {
    std::optional<std::uint64_t> seq;  // the first bad block, when one is to blame
    std::string reason;
};

/// An accumulator node store that keeps nothing: appending only writes
/// nodes, so the frontier, and with it the digest, needs no stored ones.
class discarding_merkle_store {
public:
    [[nodiscard]] auto get(std::uint32_t /*level*/, std::uint64_t /*index*/) const
        -> std::optional<hash256> {
        return std::nullopt;
    }
    auto put(std::uint32_t /*level*/, std::uint64_t /*index*/, const hash256& /*h*/) -> void {}
};

class journal_verifier {
public:
    /// Verify from block `first` on. Only a verifier that starts at block 0
    /// can compute digests; a later start checks the blocks and the chain
    /// between them only.
    explicit journal_verifier(std::uint64_t first = 0, std::optional<saved_digest> saved = {})
        : next_(first), from_genesis_(first == 0), saved_(saved) {
        if (from_genesis_) {
            accumulator_.emplace(store_);
        }
    }

    journal_verifier(const journal_verifier&) = delete;
    journal_verifier(journal_verifier&&) = delete;
    auto operator=(const journal_verifier&) -> journal_verifier& = delete;
    auto operator=(journal_verifier&&) -> journal_verifier& = delete;
    ~journal_verifier() = default;

    /// Check the next block; false (and failure() set) on the first bad one.
    auto add(const block& b) -> bool {
        if (failure_) {
            return false;
        }
        if (b.seq != next_) {
            return fail(next_, "expected block " + std::to_string(next_) + ", found block " +
                                   std::to_string(b.seq));
        }
        const hash256 expected_previous =
            previous_ ? *previous_ : (b.seq == 0 ? genesis_previous_hash : b.previous_block_hash);
        if (b.previous_block_hash != expected_previous) {
            return fail(b.seq, "previousBlockHash " + to_hex(b.previous_block_hash) +
                                   " is not the previous block's hash " +
                                   to_hex(expected_previous));
        }
        if (last_timestamp_ && b.timestamp < *last_timestamp_) {
            return fail(b.seq, "block timestamp runs backwards");
        }
        for (std::size_t i = 0; i < b.revisions.size(); ++i) {
            const auto& r = b.revisions[i];
            if (r.compute_hash() != r.hash) {
                return fail(b.seq, "revision " + r.table + "/" + r.id + " version " +
                                       std::to_string(r.version) +
                                       " does not match its data and metadata");
            }
        }
        if (auto why = b.label_problem()) {
            return fail(b.seq, std::move(*why));
        }
        if (b.compute_entries_hash() != b.entries_hash) {
            return fail(b.seq, "entriesHash does not match the statements and revisions");
        }
        if (b.compute_block_hash() != b.block_hash) {
            return fail(b.seq, "blockHash " + to_hex(b.block_hash) + " does not match its fields");
        }
        previous_ = b.block_hash;
        last_timestamp_ = b.timestamp;
        ++next_;
        ++blocks_;
        if (accumulator_) {
            accumulator_->append(b.block_hash);
            if (saved_ && saved_->tip == b.seq) {
                const auto d = *accumulator_->digest();
                if (d != saved_->digest) {
                    return fail(std::nullopt, "digest at block " + std::to_string(b.seq) + " is " +
                                                  to_hex(d) + ", not the saved " +
                                                  to_hex(saved_->digest));
                }
                digest_checked_ = true;
            }
        }
        return true;
    }

    /// After the last block: fails if a saved digest was never reached.
    auto finish() -> bool {
        if (failure_) {
            return false;
        }
        if (saved_ && !digest_checked_) {
            if (!from_genesis_) {
                return fail(std::nullopt,
                            "a digest can only be checked from block 0; this journal starts later");
            }
            return fail(std::nullopt, "the saved digest's tip " + std::to_string(saved_->tip) +
                                          " is beyond the journal's last block");
        }
        return true;
    }

    /// Record a failure found outside add(), e.g. an unreadable block.
    auto record_failure(std::optional<std::uint64_t> seq, std::string reason) -> void {
        if (!failure_) {
            failure_ = verify_failure{seq, std::move(reason)};
        }
    }

    [[nodiscard]] auto failure() const -> const std::optional<verify_failure>& { return failure_; }
    [[nodiscard]] auto blocks() const -> std::uint64_t { return blocks_; }
    [[nodiscard]] auto digest_checked() const -> bool { return digest_checked_; }
    /// The digest over every block so far; nullopt unless started at 0.
    [[nodiscard]] auto digest() const -> std::optional<hash256> {
        return accumulator_ ? accumulator_->digest() : std::nullopt;
    }

private:
    auto fail(std::optional<std::uint64_t> seq, std::string reason) -> bool {
        record_failure(seq, std::move(reason));
        return false;
    }

    std::uint64_t next_;
    bool from_genesis_;
    std::optional<saved_digest> saved_;
    std::optional<hash256> previous_;
    std::optional<std::int64_t> last_timestamp_;
    std::uint64_t blocks_ = 0;
    bool digest_checked_ = false;
    std::optional<verify_failure> failure_;
    discarding_merkle_store store_;
    std::optional<merkle_accumulator<discarding_merkle_store>> accumulator_;
};

// ── Sources ─────────────────────────────────────────────────────────────────

/// Feed blocks 0..tip of a state machine (a data directory opened with its
/// engine) to `v`. A block that cannot be read is a failure at that block.
template<typename StateMachine>
auto verify_state_machine(const StateMachine& sm, journal_verifier& v) -> bool {
    const auto tip = sm.tip();
    if (tip) {
        for (std::uint64_t seq = 0; seq <= tip->seq; ++seq) {
            std::optional<block> b;
            try {
                b = sm.get_block(seq);
            } catch (const std::exception& e) {
                v.record_failure(seq, std::string("cannot read the block: ") + e.what());
                return false;
            }
            if (!b) {
                v.record_failure(seq, "the block is missing");
                return false;
            }
            if (!v.add(*b)) {
                return false;
            }
        }
    }
    return v.finish();
}

/// Every block of the segment files in `dir`, in order (files named by
/// segment_info::file_name sort by first block).
inline auto read_segment_directory(const std::filesystem::path& dir) -> std::vector<block> {
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const auto name = e.path().filename().string();
        if (name.starts_with("seg-") && name.ends_with(".kyl")) {
            files.push_back(e.path());
        }
    }
    std::ranges::sort(files);
    std::vector<block> out;
    for (const auto& f : files) {
        auto blocks = decode_segment(*read_file(f));
        out.insert(out.end(), std::make_move_iterator(blocks.begin()),
                   std::make_move_iterator(blocks.end()));
    }
    return out;
}

/// An export directory read back: the manifest and its blocks in order.
/// Each object is checked against the manifest's size and SHA-256 first.
/// Throws ledger_format_error for a JSON export, which cannot be
/// re-hashed, and ledger_segment_error for a missing or altered object.
inline auto read_export_directory(const std::filesystem::path& dir)
    -> std::pair<ion_value, std::vector<block>> {
    const auto manifest_bytes = read_file(dir / "manifest.json");
    if (!manifest_bytes) {
        throw ledger_segment_error("no manifest.json in " + dir.string());
    }
    ion_value manifest = ion_value::parse(std::string_view(*manifest_bytes));
    const auto format = export_format_from(journal_detail::text_field(manifest, "format"));
    if (!format) {
        throw ledger_format_error("unknown export format in the manifest");
    }
    if (*format == export_format::json) {
        throw ledger_format_error(
            "a JSON export has lost the Ion types and cannot be re-hashed; export as ion_text or "
            "ion_binary to verify");
    }
    std::vector<block> out;
    const ion_value* objects = manifest.find("objects");
    if (objects == nullptr || !objects->is_sequence()) {
        throw ledger_format_error("the manifest has no objects list");
    }
    for (const auto& o : objects->items()) {
        const auto key = journal_detail::text_field(o, "key");
        const auto name = std::filesystem::path(key).filename();
        const auto body = read_file(dir / name);
        if (!body) {
            throw ledger_segment_error("export object " + name.string() + " is missing");
        }
        if (body->size() != journal_detail::uint_field(o, "size") ||
            to_hex(sha256_of(*body)) != journal_detail::text_field(o, "sha256")) {
            throw ledger_segment_error("export object " + name.string() +
                                       " does not match the manifest's size and SHA-256");
        }
        for (const auto& v : ion_value::parse_all(std::span<const std::uint8_t>(
                 reinterpret_cast<const std::uint8_t*>(body->data()), body->size()))) {
            out.push_back(block::from_ion(v));
        }
    }
    return {std::move(manifest), std::move(out)};
}

// ── Proofs ──────────────────────────────────────────────────────────────────

/// A block or revision with its proof, as GetBlock and GetRevision return
/// it (Requirement 4.3) and as `ledger_ctl prove` reads it.
struct proof_document {
    std::optional<block> blk;
    std::optional<revision> rev;
    std::vector<hash256> proof;
    hash256 digest{};
    std::uint64_t tip = 0;

    [[nodiscard]] auto to_ion() const -> ion_value {
        ion_value v = ion_value::make_struct();
        if (blk) {
            v.set("block", blk->to_ion());
        }
        if (rev) {
            v.set("revision", rev->to_ion());
        }
        ion_value list = ion_value::make_list();
        for (const auto& h : proof) {
            list.push_back(hash_value(h));
        }
        v.set("proof", std::move(list))
            .set("digest", hash_value(digest))
            .set("tip", ion_value::integer(static_cast<std::int64_t>(tip)));
        return v;
    }

    static auto from_ion(const ion_value& v) -> proof_document {
        proof_document p;
        if (const ion_value* b = v.find("block"); b != nullptr) {
            p.blk = block::from_ion(*b);
        }
        if (const ion_value* r = v.find("revision"); r != nullptr) {
            p.rev = revision::from_ion(*r);
        }
        if (p.blk.has_value() == p.rev.has_value()) {
            throw ledger_format_error("a proof holds exactly one block or revision");
        }
        const ion_value* list = v.find("proof");
        if (list == nullptr || !list->is_sequence()) {
            throw ledger_format_error("a proof needs a proof list");
        }
        for (const auto& h : list->items()) {
            const auto hash = hash_from(&h);
            if (!hash) {
                throw ledger_format_error("a proof step is not a 32-byte blob");
            }
            p.proof.push_back(*hash);
        }
        p.digest = journal_detail::hash_field(v, "digest");
        p.tip = journal_detail::uint_field(v, "tip");
        return p;
    }
};

/// Check a proof offline (Requirement 9.2): the block or revision is
/// re-hashed from its contents, folded up the proof, and must land on the
/// saved digest at the same tip. Returns why it does not, or nullopt.
inline auto check_proof(const proof_document& p, const saved_digest& saved)
    -> std::optional<std::string> {
    if (p.tip != saved.tip || p.digest != saved.digest) {
        return "the proof is for the digest at tip " + std::to_string(p.tip) + " (" +
               to_hex(p.digest) + "), not the saved one at tip " + std::to_string(saved.tip);
    }
    hash256 leaf{};
    if (p.blk) {
        if (p.blk->seq > saved.tip) {
            return "block " + std::to_string(p.blk->seq) + " is after the digest's tip";
        }
        if (!p.blk->verify()) {
            return "block " + std::to_string(p.blk->seq) + " does not match its hashes";
        }
        leaf = p.blk->block_hash;
    } else {
        if (p.rev->seq > saved.tip) {
            return "the revision's block is after the digest's tip";
        }
        if (p.rev->compute_hash() != p.rev->hash) {
            return "the revision does not match its data and metadata";
        }
        leaf = p.rev->hash;
    }
    if (!verify_proof(leaf, p.proof, saved.digest)) {
        return "the proof does not fold to the saved digest";
    }
    return std::nullopt;
}

}  // namespace kythira::ledger
