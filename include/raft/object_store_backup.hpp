// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file object_store_backup.hpp
/// @brief Point-in-time copies of a node's Raft state, with a manifest that
///        makes a torn or smeared copy **detectable** rather than merely
///        warned about.
///
/// ## It is not part of the engine, structurally
///
/// `object_store_backup` takes a store and a prefix. It never instantiates
/// `object_store_persistence_engine`, holds no engine state and has no path
/// back into one, so it cannot be reached from the Raft hot path
/// (Requirement 10.7). That is a property of the code rather than a convention
/// to be respected: there is no engine type named anywhere below.
///
/// It does reuse two free helpers from `object_store_persistence.hpp` —
/// `md5_hex` and `iso8601_utc`. Including that header does not instantiate the
/// engine template, and the alternative is a second hand-rolled MD5 with its
/// own separate known-answer test, which is worse in every way.
///
/// ## The manifest is the commit point
///
/// A backup lives at `<backup_prefix>/<backup_id>/`, with the copied objects
/// under `objects/` and the manifest at `manifest.json`. **The manifest is
/// written last**, so a backup that exists is a backup that finished: a run
/// that dies partway leaves object copies and no manifest, and `list` ignores
/// any directory without one. There is no cross-key atomicity in any of these
/// five services, so this is the only commit point available — and it is the
/// reason the manifest cannot simply be written first and patched later.
///
/// ## What the manifest is *for*: a smear is detectable, not merely disclosed
///
/// A backup taken from a running node is a **smear** across the copy window.
/// Objects are read one at a time over seconds or minutes, and the node keeps
/// writing, so the copy can hold a `term` read early and a log entry written
/// late. `backup_options::source_quiesced` records the caller's claim about
/// which kind of source this was, and the manifest records it — but a claim is
/// not evidence, so `verify` checks the copy against itself:
///
/// - **Index contiguity.** Every index from the snapshot's
///   `last_included_index + 1` through the highest copied entry must be
///   present. A gap means an entry was truncated out from under the copy.
/// - **Checksums.** Every object's bytes must still hash to what the manifest
///   recorded, which catches corruption in the destination as well as a copy
///   that was rewritten after its checksum was taken.
/// - **Term monotonicity.** The recorded `current_term` must be ≥ every copied
///   entry's term. This is the check that catches the *classic* smear: `term`
///   is read early at 5, the node advances to 7 and appends, and the copy ends
///   up holding an entry from a term the manifest says never happened.
///
/// Each is reported as a named problem rather than a boolean, because "this
/// backup is bad" is not actionable and "log index 42 is missing between the
/// snapshot at 40 and the last entry at 45" is.
///
/// ## Reading the engine's state without the engine
///
/// The manifest has to report term, vote, log range and snapshot metadata, and
/// it gets them by parsing the on-store format directly — the key layout
/// (`<prefix>/term`, `/voted_for`, `/snapshot`, `/log/<20-digit index>`) and
/// the JSON bodies the engine writes. That is a real coupling and it is stated
/// rather than hidden: **if the engine's on-bucket format changes, this file
/// changes with it.** The format is documented and test-pinned on the engine
/// side (the 20-digit padding especially), which is what makes the coupling
/// tolerable, and the conformance suite would catch a divergence.
///
/// The parse rule matches the engine's own load path: an **absent** metadata
/// object is normal and recorded as absent (a node that has never voted has no
/// `voted_for`), while an object that is **present and unparseable** throws,
/// naming the key. A manifest that guessed at a corrupt source would be worse
/// than no backup, because its whole purpose is to be the thing you trust in a
/// recovery window.
///
/// ## Write it to a different bucket
///
/// Requirement 10.5, and the reasoning is Requirement 8.6's: a backup in the
/// same bucket as its source shares that bucket's blast radius — one bad
/// lifecycle rule, one mistaken prefix delete, one compromised credential
/// takes both. Nothing here enforces a different bucket, because a same-bucket
/// backup is still better than none and an operator may have exactly one, but
/// a different bucket is the recommendation and a cross-account or
/// cross-project destination is better still.

#include <raft/key_object_store.hpp>
#include <raft/object_store_persistence.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira {

/// @brief Where a backup reads from: a bucket and one node's prefix, with no
///        trailing slash (the same shape the engine is constructed with).
struct backup_source {
    std::string bucket;
    std::string prefix;
};

/// @brief Where a backup is written: a bucket and the root prefix under which
///        each backup gets its own `<backup_id>/` directory.
struct backup_destination {
    std::string bucket;
    std::string prefix;
};

struct backup_options {
    /// Empty means "derive one from the current UTC time". A supplied id must
    /// contain no `/` — it names a single directory level, and one containing a
    /// separator would silently restructure the layout that `list` and `verify`
    /// depend on.
    std::string backup_id;

    /// The caller's claim that the source was not being written during the
    /// copy. Recorded in the manifest and **not** believed by `verify`, which
    /// checks the copy against itself either way.
    bool source_quiesced{false};
};

/// @brief One copied object: its key relative to the source prefix, its size,
///        and the MD5 of the bytes that were copied.
struct backup_object_entry {
    std::string relative_key;
    std::uint64_t size_bytes{0};
    std::string checksum_md5;
};

/// @brief Everything needed to judge a backup without reading the objects.
struct backup_manifest {
    /// Bumped when the manifest schema changes incompatibly. A reader that does
    /// not recognise the version must refuse rather than guess.
    int format_version{1};

    std::string provider;
    std::string source_bucket;
    std::string source_prefix;
    std::string backup_id;
    /// UTC, ISO 8601, second precision.
    std::string created_at;
    bool source_quiesced{false};

    /// Absent when the source has no such object — a fresh node has no term,
    /// no vote and no snapshot, and that is a legal thing to back up.
    std::optional<std::uint64_t> current_term;
    std::optional<std::string> voted_for;
    std::optional<std::uint64_t> first_log_index;
    std::optional<std::uint64_t> last_log_index;
    std::optional<std::uint64_t> snapshot_last_included_index;
    std::optional<std::uint64_t> snapshot_last_included_term;

    /// The cluster configuration recorded in the snapshot, in display form.
    /// Node ids are `std::string` or an unsigned integer depending on the
    /// engine's instantiation, and this class is generic over the *store* and
    /// not over `NodeId`, so they are rendered rather than typed.
    std::vector<std::string> configuration_nodes;
    bool configuration_is_joint{false};
    std::vector<std::string> configuration_old_nodes;

    std::vector<backup_object_entry> objects;
};

struct restore_options {
    /// Restore into a target prefix that already holds objects. Without it a
    /// non-empty target is refused; with it, the target's **engine-owned** keys
    /// are deleted before anything is written.
    ///
    /// It never merges, and that is not a policy this flag can relax — see
    /// `object_store_backup::restore_clone`.
    bool force{false};
};

/// @brief What a restore wrote, so the caller can print it. Requirement 11.3
///        asks for exactly this from seed restore: the operator has to
///        configure the new cluster by hand afterwards.
struct restored_node {
    std::string node_id;
    std::string prefix;
};

struct restore_report {
    std::string backup_id;
    /// One entry for a clone restore, one per new node for a seed restore.
    std::vector<restored_node> nodes;
    std::size_t objects_written{0};
    /// Engine-owned keys deleted from the target because `force` was set.
    std::vector<std::string> keys_deleted;
    /// Set by clone restore when the backup carried an owner object: the epoch
    /// the restored prefix now claims, which is strictly higher than the
    /// original's so a returning original fences itself out on its next write.
    std::optional<std::uint64_t> owner_epoch;
};

/// @brief One thing wrong with a backup, named specifically enough to act on.
struct verification_problem {
    /// A stable machine-readable tag: `"missing_object"`, `"checksum_mismatch"`,
    /// `"size_mismatch"`, `"log_gap"`, `"term_regression"`,
    /// `"unreadable_entry"`, `"manifest_version"`.
    std::string kind;
    /// The object this is about, where there is one.
    std::string subject;
    /// A sentence naming the actual values.
    std::string detail;
};

struct verification_report {
    bool ok{false};
    backup_manifest manifest;
    std::vector<verification_problem> problems;
};

namespace object_store_backup_detail {

inline constexpr int k_manifest_format_version = 1;
inline constexpr const char* k_manifest_name = "manifest.json";
inline constexpr const char* k_objects_dir = "objects/";

/// The engine's own log-key padding. Duplicated deliberately rather than
/// reached for through the engine template: this file names no engine type, and
/// the padding is a documented, test-pinned part of the on-bucket format that
/// the conformance suite would catch a divergence in.
inline constexpr std::size_t k_index_digits = 20;

/// A `backup_id` derived from the time, in a form that sorts chronologically
/// under a lexicographic listing — which is the only ordering these stores
/// offer.
[[nodiscard]] inline auto timestamp_backup_id(std::chrono::system_clock::time_point when)
    -> std::string {
    // `YYYY-MM-DDTHH:MM:SSZ` with the punctuation removed: `20260817T221530Z`.
    // Fixed width and zero-padded, so lexicographic order is chronological
    // order — the property Requirement 10.2 asks for.
    std::string iso = object_store_persistence_detail::iso8601_utc(when);
    std::string out;
    out.reserve(iso.size());
    for (const char c : iso) {
        if (c != '-' && c != ':') {
            out += c;
        }
    }
    return out;
}

/// Parses a decimal that must consume the whole field, naming @p key on
/// failure. The engine's `term` and `voted_for` objects are bare decimals.
[[nodiscard]] inline auto parse_u64(std::string_view text, const std::string& key)
    -> std::uint64_t {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string_view::npos) {
        throw std::runtime_error("object_store_backup: corrupt object " + key + ": expected a" +
                                 " decimal integer, got \"" + std::string(text) + "\"");
    }
    return std::stoull(std::string(text));
}

/// A JSON value rendered for display: a string without its quotes, anything
/// else serialized. Used for node ids, which are strings on one instantiation
/// and integers on another.
[[nodiscard]] inline auto render_node(const boost::json::value& value) -> std::string {
    if (value.is_string()) {
        return std::string(value.as_string());
    }
    return boost::json::serialize(value);
}

}  // namespace object_store_backup_detail

/// @brief Create, list and verify point-in-time copies of one node's prefix.
///
/// @tparam Store any `key_object_store`. The refinement
///         `conditional_key_object_store` is deliberately **not** required: a
///         backup writes to keys nobody else is writing, so it needs no
///         preconditions, and requiring them would exclude Alibaba OSS from
///         being a backup destination for no benefit.
template<key_object_store Store> class object_store_backup {
public:
    explicit object_store_backup(Store store) : _store(std::move(store)) {}

    /// @brief Copy every object under `src.prefix` to
    ///        `<dst.prefix>/<backup_id>/`, then write the manifest.
    ///
    /// Returns the manifest that was written. Throws if any object cannot be
    /// read or written, or if a metadata object is present but unparseable —
    /// in which case the manifest is never written, so the partial copy is
    /// invisible to `list` and cannot be mistaken for a backup.
    [[nodiscard]] auto create(const backup_source& src, const backup_destination& dst,
                              const backup_options& opts = {}) const -> backup_manifest {
        namespace detail = object_store_backup_detail;

        const std::string backup_id =
            opts.backup_id.empty() ? detail::timestamp_backup_id(std::chrono::system_clock::now())
                                   : opts.backup_id;
        if (backup_id.find('/') != std::string::npos) {
            throw std::invalid_argument(
                "object_store_backup: backup_id \"" + backup_id +
                "\" contains '/' — an id names one directory level, and a separator in it would"
                " silently restructure the layout list() and verify() depend on");
        }

        backup_manifest manifest;
        manifest.format_version = detail::k_manifest_format_version;
        manifest.provider = std::string(_store.provider_name());
        manifest.source_bucket = src.bucket;
        manifest.source_prefix = src.prefix;
        manifest.backup_id = backup_id;
        manifest.created_at =
            object_store_persistence_detail::iso8601_utc(std::chrono::system_clock::now());
        manifest.source_quiesced = opts.source_quiesced;

        const std::string source_root = src.prefix + "/";
        const std::string backup_root = backup_prefix(dst, backup_id);

        // The listing is taken once, and everything below is relative to it.
        // Objects that appear after it are simply not in this backup — which is
        // what "point in time" means and is exactly the smear `verify` exists
        // to detect.
        for (const auto& key : _store.list_keys(src.bucket, source_root)) {
            if (!key.starts_with(source_root)) {
                // A store that answered a prefixed LIST with an unprefixed key
                // is not one whose listing can be trusted to be complete
                // either, so this is refused rather than skipped.
                throw std::runtime_error("object_store_backup: listing of " + source_root +
                                         " returned " + key + ", which is not under that prefix");
            }
            const std::string relative = key.substr(source_root.size());

            auto got = _store.get_object(src.bucket, key);
            if (!got) {
                // Listed, then gone. On a quiesced source this cannot happen; on
                // a live one it is a truncation racing the copy. Either way the
                // object is not in this backup, and skipping it silently is what
                // would make the smear undetectable — so it is skipped *and* the
                // gap it leaves is what `verify` reports.
                continue;
            }

            interpret_metadata(relative, got->body, key, manifest);

            const std::string destination = backup_root + detail::k_objects_dir + relative;
            _store.put_object(dst.bucket, destination, got->body);

            manifest.objects.push_back(
                backup_object_entry{relative, static_cast<std::uint64_t>(got->body.size()),
                                    object_store_persistence_detail::md5_hex(got->body)});
        }

        // Written last, and that ordering is the whole commit protocol — see the
        // file comment. Everything above can be re-run; nothing above is visible
        // to `list` until this line succeeds.
        _store.put_object(dst.bucket, backup_root + detail::k_manifest_name,
                          serialize_manifest(manifest));
        return manifest;
    }

    /// @brief Every finished backup under `dst.prefix`, in listing order —
    ///        which, for a timestamp-derived id, is chronological order.
    ///
    /// Directories without a manifest are **ignored**, not reported: an
    /// unfinished or abandoned copy is not a backup, and surfacing it as one
    /// would invite restoring from it.
    [[nodiscard]] auto list(const backup_destination& dst) const -> std::vector<backup_manifest> {
        namespace detail = object_store_backup_detail;
        const std::string root = dst.prefix + "/";
        const std::string suffix = std::string("/") + detail::k_manifest_name;

        std::vector<backup_manifest> out;
        for (const auto& key : _store.list_keys(dst.bucket, root)) {
            if (!key.ends_with(suffix)) {
                continue;
            }
            auto got = _store.get_object(dst.bucket, key);
            if (!got) {
                // Listed and then deleted between the LIST and the GET. Not a
                // backup any more, and not an error either.
                continue;
            }
            out.push_back(parse_manifest(got->body, key));
        }
        return out;
    }

    /// @brief Check a backup against itself and report every problem found.
    ///
    /// Does **not** stop at the first problem: an operator in a recovery window
    /// wants the whole list, not one at a time.
    [[nodiscard]] auto verify(const backup_destination& dst, std::string_view backup_id) const
        -> verification_report {
        namespace detail = object_store_backup_detail;
        const std::string root = backup_prefix(dst, std::string(backup_id));
        const std::string manifest_key = root + detail::k_manifest_name;

        auto got = _store.get_object(dst.bucket, manifest_key);
        if (!got) {
            throw std::runtime_error("object_store_backup: no manifest at " + manifest_key +
                                     " — either the id is wrong or that copy never finished");
        }

        verification_report report;
        report.manifest = parse_manifest(got->body, manifest_key);

        if (report.manifest.format_version != detail::k_manifest_format_version) {
            report.problems.push_back(verification_problem{
                "manifest_version", manifest_key,
                "manifest format version " + std::to_string(report.manifest.format_version) +
                    " is not the " + std::to_string(detail::k_manifest_format_version) +
                    " this build understands — refusing to interpret it rather than guessing"});
            report.ok = false;
            return report;
        }

        // Contiguity is checked over the entries that actually **verified**,
        // not over what the manifest merely claims. Requirement 10.4 asks
        // whether every index is *present*, and an object listed in the
        // manifest but absent or corrupt in the destination is not present —
        // it is a hole a restore would reproduce. So a deleted or rewritten log
        // object reports twice, once as the specific object and once as the gap
        // it leaves, which is deliberate: "…objects/log/…05 is missing" says
        // what broke, and "log index 5 is missing between 4 and 6" says what it
        // costs.
        const auto restorable = check_objects(dst, root, report);
        check_log_contiguity(report, restorable);
        report.ok = report.problems.empty();
        return report;
    }

    /// @brief Reproduce a backup into @p target byte for byte.
    ///
    /// For replacing the storage or the instance under an **otherwise
    /// unchanged node identity**. The restored state is safe to start only if
    /// the original node is definitively gone: starting both is a split-brain,
    /// and no amount of care here prevents that — it is an operational fact
    /// about running two copies of one node, not something a restore tool can
    /// check.
    ///
    /// What this *can* do, and does, is make the split-brain **loud** where the
    /// engine is fenced. If the backup carried an owner object, it is restored
    /// with a strictly **higher epoch**, so a returning original finds its own
    /// epoch stale and fences itself out on its next write (Requirement 11.1)
    /// rather than writing alongside the restored node.
    ///
    /// Verifies the backup **before writing anything** and aborts on the first
    /// inconsistency, naming it. That is deliberately stricter than `verify`,
    /// which collects every problem: `verify` is a diagnostic and this is a
    /// gate.
    auto restore_clone(const backup_destination& src, std::string_view backup_id,
                       const backup_source& target, const restore_options& opts = {}) const
        -> restore_report {
        namespace detail = object_store_backup_detail;

        const auto manifest = verify_or_abort(src, backup_id);
        prepare_target(target, opts);

        restore_report report;
        report.backup_id = manifest.backup_id;
        report.nodes.push_back(restored_node{std::string{}, target.prefix});

        const std::string root = backup_prefix(src, std::string(backup_id));
        for (const auto& entry : manifest.objects) {
            auto got =
                _store.get_object(src.bucket, root + detail::k_objects_dir + entry.relative_key);
            if (!got) {
                // verify_or_abort just read every one of these, so this is a
                // concurrent deletion of the *backup* mid-restore. Refusing
                // leaves a partial target, which is why the message says so.
                throw std::runtime_error(
                    "object_store_backup: " + entry.relative_key +
                    " vanished from the backup during the restore — the target prefix " +
                    target.prefix + " is now partially written and must not be started");
            }

            std::string body = std::move(got->body);
            if (entry.relative_key == "owner") {
                body = with_higher_epoch(body, root + detail::k_objects_dir + entry.relative_key,
                                         report);
            }
            _store.put_object(target.bucket, target.prefix + "/" + entry.relative_key, body);
            ++report.objects_written;
        }
        report.keys_deleted = std::move(_deleted_scratch);
        _deleted_scratch.clear();
        return report;
    }

    /// @brief Build the initial state of a **new** cluster from a backup's
    ///        snapshot.
    ///
    /// The state-machine bytes are preserved and everything that makes the
    /// state belong to the *old* cluster is discarded: the configuration is
    /// **replaced** by @p new_nodes, the term is reset to the snapshot's
    /// `last_included_term`, the vote is cleared, and the log is empty.
    ///
    /// One prefix per new node, `<target.prefix>/<node_id>`, all seeded
    /// identically — which is correct precisely because they are a *new*
    /// cluster with no history to disagree about. The returned report names
    /// every prefix written, because the operator has to configure the cluster
    /// by hand afterwards and the new identities are unrelated to the old ones.
    ///
    /// **Refuses when the backup has no snapshot.** There is nothing to seed
    /// from: a log without a snapshot is a history of a cluster whose
    /// configuration is exactly what seed restore exists to discard.
    auto restore_seed(const backup_destination& src, std::string_view backup_id,
                      const backup_source& target, const std::vector<std::string>& new_nodes,
                      const restore_options& opts = {}) const -> restore_report {
        namespace detail = object_store_backup_detail;

        if (new_nodes.empty()) {
            throw std::invalid_argument(
                "object_store_backup: seed restore needs at least one node id — it is building a"
                " new cluster's membership, and an empty one has no meaning");
        }

        const auto manifest = verify_or_abort(src, backup_id);
        if (!manifest.snapshot_last_included_index || !manifest.snapshot_last_included_term) {
            throw std::runtime_error(
                "object_store_backup: backup " + manifest.backup_id +
                " has no snapshot, so there is nothing to seed a new cluster from — a log without"
                " a snapshot is the history of a cluster whose configuration is exactly what seed"
                " restore discards");
        }

        const std::string root = backup_prefix(src, std::string(backup_id));
        auto snapshot = _store.get_object(src.bucket, root + detail::k_objects_dir + "snapshot");
        if (!snapshot) {
            throw std::runtime_error("object_store_backup: backup " + manifest.backup_id +
                                     " lists a snapshot in its manifest but has none stored");
        }
        const std::string seeded_snapshot =
            reseat_snapshot(snapshot->body, root + detail::k_objects_dir + "snapshot", new_nodes);
        const std::string seeded_term = std::to_string(*manifest.snapshot_last_included_term);

        restore_report report;
        report.backup_id = manifest.backup_id;

        // Every target is prepared *before* any is written, so a refusal on the
        // third node does not leave the first two seeded into a cluster that
        // will never have a quorum.
        std::vector<std::string> prefixes;
        prefixes.reserve(new_nodes.size());
        for (const auto& node : new_nodes) {
            prefixes.push_back(target.prefix + "/" + node);
        }
        for (const auto& prefix : prefixes) {
            prepare_target(backup_source{target.bucket, prefix}, opts);
        }

        for (std::size_t i = 0; i < new_nodes.size(); ++i) {
            const auto& prefix = prefixes[i];
            _store.put_object(target.bucket, prefix + "/snapshot", seeded_snapshot);
            _store.put_object(target.bucket, prefix + "/term", seeded_term);
            report.objects_written += 2;
            report.nodes.push_back(restored_node{new_nodes[i], prefix});
            // No `voted_for` and no log entries, deliberately and by omission
            // rather than by writing sentinels: the vote is cleared, and an
            // absent object is how this engine spells "never voted". Writing
            // `"none"` would make a fenced engine's first vote an If-Match
            // against an object it never wrote.
        }
        report.keys_deleted = std::move(_deleted_scratch);
        _deleted_scratch.clear();
        return report;
    }

private:
    /// Requirement 11.5: verify first, and abort on the **first** inconsistency
    /// rather than collecting them. A restore that proceeded past a named
    /// problem would be the one operation where a warning is worthless.
    [[nodiscard]] auto verify_or_abort(const backup_destination& src,
                                       std::string_view backup_id) const -> backup_manifest {
        const auto report = verify(src, backup_id);
        if (!report.problems.empty()) {
            const auto& first = report.problems.front();
            throw std::runtime_error(
                "object_store_backup: refusing to restore backup " + std::string(backup_id) +
                " — " + first.kind + " on " + first.subject + ": " + first.detail +
                (report.problems.size() > 1
                     ? " (and " + std::to_string(report.problems.size() - 1) + " more; run verify)"
                     : ""));
        }
        return report.manifest;
    }

    /// Requirement 11.4, and the reason a merge has no code path: this either
    /// throws or leaves the target with no engine-owned keys. Nothing calls a
    /// write until it has returned.
    auto prepare_target(const backup_source& target, const restore_options& opts) const -> void {
        const std::string root = target.prefix + "/";
        const auto existing = _store.list_keys(target.bucket, root);
        if (existing.empty()) {
            return;
        }
        if (!opts.force) {
            throw std::runtime_error(
                "object_store_backup: target prefix " + target.prefix + " already holds " +
                std::to_string(existing.size()) +
                " object(s) — refusing to restore into it without force. A restore that merged"
                " into existing Raft state would produce a node whose log is two nodes' histories"
                " interleaved, which is not a recoverable state and which no later repair can"
                " untangle");
        }
        for (const auto& key : existing) {
            if (!is_engine_owned(key.substr(root.size()))) {
                // Foreign objects are the operator's, not the engine's, and
                // this tool has no business deleting them — the engine itself
                // neither reads, writes nor deletes them. Leaving them is also
                // safe: they cannot merge with Raft state.
                continue;
            }
            _store.delete_object(target.bucket, key);
            _deleted_scratch.push_back(key);
        }
    }

    /// The keys the engine owns under a node prefix. Anything else under that
    /// prefix belongs to whoever put it there.
    [[nodiscard]] static auto is_engine_owned(std::string_view relative) -> bool {
        return relative == "term" || relative == "voted_for" || relative == "snapshot" ||
               relative == "owner" || relative.starts_with("log/") ||
               relative.starts_with("snapshots/");
    }

    /// Requirement 11.1's epoch bump. Parsed and re-serialized rather than
    /// patched textually, so a differently-formatted owner object cannot
    /// silently pass through unchanged.
    [[nodiscard]] static auto with_higher_epoch(const std::string& body, const std::string& key,
                                                restore_report& report) -> std::string {
        boost::json::value parsed;
        try {
            parsed = boost::json::parse(body);
        } catch (const std::exception& err) {
            throw std::runtime_error("object_store_backup: corrupt object " + key +
                                     ": owner record is not valid JSON: " + err.what());
        }
        if (!parsed.is_object() || !parsed.as_object().contains("epoch")) {
            throw std::runtime_error(
                "object_store_backup: corrupt object " + key +
                ": owner record has no epoch, so it cannot be restored with a higher one —"
                " restoring it unchanged would leave a returning original able to write");
        }
        auto obj = parsed.as_object();
        const auto epoch =
            static_cast<std::uint64_t>(obj.at("epoch").to_number<std::uint64_t>()) + 1;
        obj["epoch"] = epoch;
        report.owner_epoch = epoch;
        return boost::json::serialize(obj);
    }

    /// The seeded snapshot: the state-machine bytes and the index/term the
    /// snapshot covers are preserved verbatim, the configuration is replaced.
    [[nodiscard]] static auto reseat_snapshot(const std::string& body, const std::string& key,
                                              const std::vector<std::string>& new_nodes)
        -> std::string {
        boost::json::value parsed;
        try {
            parsed = boost::json::parse(body);
        } catch (const std::exception& err) {
            throw std::runtime_error("object_store_backup: corrupt object " + key +
                                     ": snapshot is not valid JSON: " + err.what());
        }
        if (!parsed.is_object()) {
            throw std::runtime_error("object_store_backup: corrupt object " + key +
                                     ": snapshot is not a JSON object");
        }
        auto obj = parsed.as_object();

        // **The node-id shape has to be preserved, and it is not a formatting
        // detail.** The engine reads this array with `as_string()` when its
        // `NodeId` is `std::string` and `as_int64()` when it is an integer, and
        // boost::json *throws* on the wrong one. Writing strings unconditionally
        // would seed a numeric-id cluster with a snapshot that parses fine here
        // and explodes at the moment an operator starts the new cluster — the
        // worst possible time to find out.
        //
        // This class is generic over the *store*, not over `NodeId`, so the
        // shape is inferred from the backup's own snapshot rather than guessed
        // from the supplied ids. Guessing from the ids would get "1", "2", "3"
        // wrong for a string-id cluster, which is an entirely ordinary way to
        // name nodes.
        const auto* old_nodes = obj.if_contains("nodes");
        if (old_nodes == nullptr || !old_nodes->is_array() || old_nodes->as_array().empty()) {
            throw std::runtime_error(
                "object_store_backup: the snapshot in " + key +
                " records no cluster configuration, so the node-id representation this cluster"
                " uses cannot be determined — seeding it would have to guess between \"n1\" and 1,"
                " and the engine throws on the wrong one when it next starts");
        }
        const bool numeric_ids = old_nodes->as_array().front().is_number();

        boost::json::array nodes;
        for (const auto& node : new_nodes) {
            if (!numeric_ids) {
                nodes.push_back(boost::json::string(node));
                continue;
            }
            if (node.empty() || node.find_first_not_of("0123456789") != std::string::npos) {
                throw std::runtime_error(
                    "object_store_backup: this cluster's node ids are numbers, but \"" + node +
                    "\" is not one — seeding it as a string would produce a snapshot the engine"
                    " cannot load");
            }
            nodes.push_back(static_cast<std::int64_t>(std::stoull(node)));
        }
        obj["nodes"] = nodes;
        // The old cluster may have been mid-reconfiguration. A new cluster
        // never is, and carrying a joint-consensus marker across would seed
        // every node with a membership change nobody proposed.
        obj["is_joint_consensus"] = false;
        obj.erase("old_nodes");
        return boost::json::serialize(obj);
    }

    [[nodiscard]] static auto backup_prefix(const backup_destination& dst,
                                            const std::string& backup_id) -> std::string {
        return dst.prefix + "/" + backup_id + "/";
    }

    /// Reads what the manifest reports about the node's state out of the object
    /// currently being copied. Called with the object's key relative to the
    /// source prefix, so the layout knowledge is all in one place.
    static auto interpret_metadata(const std::string& relative, const std::string& body,
                                   const std::string& key, backup_manifest& manifest) -> void {
        namespace detail = object_store_backup_detail;

        if (relative == "term") {
            manifest.current_term = detail::parse_u64(body, key);
            return;
        }
        if (relative == "voted_for") {
            // `"none"` is the engine's sentinel for "voted for nobody", and it
            // is an object that exists — recording it as absent would lose the
            // distinction between "never voted" and "explicitly voted for
            // nobody in this term".
            manifest.voted_for = body;
            return;
        }
        if (relative == "snapshot") {
            interpret_snapshot(body, key, manifest);
            return;
        }
        if (relative.starts_with("log/")) {
            interpret_log_key(relative.substr(4), key, manifest);
            return;
        }
        // Anything else — `owner`, retained snapshots under `snapshots/`, and
        // foreign objects the engine never touches — is copied byte for byte
        // and contributes nothing to the manifest's interpretation. Copying an
        // object this file does not understand is correct: a backup that
        // silently dropped keys would restore an incomplete prefix.
    }

    static auto interpret_snapshot(const std::string& body, const std::string& key,
                                   backup_manifest& manifest) -> void {
        boost::json::value parsed;
        try {
            parsed = boost::json::parse(body);
        } catch (const std::exception& err) {
            throw std::runtime_error("object_store_backup: corrupt object " + key +
                                     ": snapshot is not valid JSON: " + err.what());
        }
        if (!parsed.is_object()) {
            throw std::runtime_error("object_store_backup: corrupt object " + key +
                                     ": snapshot is not a JSON object");
        }
        const auto& obj = parsed.as_object();
        try {
            manifest.snapshot_last_included_index =
                static_cast<std::uint64_t>(obj.at("last_included_index").to_number<std::int64_t>());
            manifest.snapshot_last_included_term =
                static_cast<std::uint64_t>(obj.at("last_included_term").to_number<std::int64_t>());
        } catch (const std::exception& err) {
            throw std::runtime_error("object_store_backup: corrupt object " + key +
                                     ": snapshot is missing its index/term: " + err.what());
        }
        if (const auto* nodes = obj.if_contains("nodes"); nodes != nullptr && nodes->is_array()) {
            for (const auto& node : nodes->as_array()) {
                manifest.configuration_nodes.push_back(
                    object_store_backup_detail::render_node(node));
            }
        }
        if (const auto* joint = obj.if_contains("is_joint_consensus");
            joint != nullptr && joint->is_bool()) {
            manifest.configuration_is_joint = joint->as_bool();
        }
        if (const auto* old_nodes = obj.if_contains("old_nodes");
            old_nodes != nullptr && old_nodes->is_array()) {
            for (const auto& node : old_nodes->as_array()) {
                manifest.configuration_old_nodes.push_back(
                    object_store_backup_detail::render_node(node));
            }
        }
    }

    static auto interpret_log_key(const std::string& suffix, const std::string& key,
                                  backup_manifest& manifest) -> void {
        namespace detail = object_store_backup_detail;
        if (suffix.size() != detail::k_index_digits ||
            suffix.find_first_not_of("0123456789") != std::string::npos) {
            // The engine's own load path treats this as corruption rather than
            // as a key to skip, for the reason it documents: a log key that
            // cannot be ordered against the others cannot be reasoned about at
            // all. A backup that quietly excluded it would produce a manifest
            // asserting a contiguity it never checked.
            throw std::runtime_error("object_store_backup: corrupt object " + key +
                                     ": log key suffix must be exactly " +
                                     std::to_string(detail::k_index_digits) + " decimal digits");
        }
        const auto index = detail::parse_u64(suffix, key);
        manifest.first_log_index =
            manifest.first_log_index ? std::min(*manifest.first_log_index, index) : index;
        manifest.last_log_index =
            manifest.last_log_index ? std::max(*manifest.last_log_index, index) : index;
    }

    /// Every manifest entry must still be present, the right size, and hash to
    /// what was recorded. Term monotonicity is checked here too, because it
    /// needs each log entry's body and this is the pass that reads them.
    ///
    /// Returns the indices of the log entries that survived every check — the
    /// log as it could actually be restored, which is what contiguity is then
    /// judged against.
    [[nodiscard]] auto check_objects(const backup_destination& dst, const std::string& root,
                                     verification_report& report) const
        -> std::vector<std::uint64_t> {
        namespace detail = object_store_backup_detail;
        std::vector<std::uint64_t> restorable;
        for (const auto& entry : report.manifest.objects) {
            const std::string key = root + detail::k_objects_dir + entry.relative_key;
            const bool is_log = entry.relative_key.starts_with("log/");

            // Shape first, and independently of whether the object is readable:
            // a log key that cannot be ordered against the others cannot be
            // reasoned about at all, and saying so is more useful than reporting
            // it as a mysterious gap.
            std::optional<std::uint64_t> index;
            if (is_log) {
                index = log_index_of(entry.relative_key, report);
            }

            auto got = _store.get_object(dst.bucket, key);
            if (!got) {
                report.problems.push_back(verification_problem{
                    "missing_object", key,
                    "the manifest lists " + entry.relative_key + " but no object is stored there"});
                continue;
            }
            if (got->body.size() != entry.size_bytes) {
                report.problems.push_back(verification_problem{
                    "size_mismatch", key,
                    "the manifest records " + std::to_string(entry.size_bytes) +
                        " bytes but the stored object is " + std::to_string(got->body.size())});
                continue;
            }
            const auto actual = object_store_persistence_detail::md5_hex(got->body);
            if (actual != entry.checksum_md5) {
                report.problems.push_back(
                    verification_problem{"checksum_mismatch", key,
                                         "the manifest records MD5 " + entry.checksum_md5 +
                                             " but the stored object" + " hashes to " + actual});
                continue;
            }
            if (is_log) {
                check_entry_term(key, got->body, report);
                if (index) {
                    restorable.push_back(*index);
                }
            }
        }
        return restorable;
    }

    /// The index a log entry's relative key names, or `nullopt` having recorded
    /// why not.
    [[nodiscard]] static auto log_index_of(const std::string& relative_key,
                                           verification_report& report)
        -> std::optional<std::uint64_t> {
        const auto suffix = relative_key.substr(4);
        if (suffix.size() != object_store_backup_detail::k_index_digits ||
            suffix.find_first_not_of("0123456789") != std::string::npos) {
            report.problems.push_back(verification_problem{
                "unreadable_entry", relative_key,
                "log key suffix is not exactly " +
                    std::to_string(object_store_backup_detail::k_index_digits) +
                    " digits, so this entry cannot be ordered against the others"});
            return std::nullopt;
        }
        return std::stoull(suffix);
    }

    /// The check that catches the classic smear: `term` read early at 5, the
    /// node advances to 7 and appends, and the copy holds an entry from a term
    /// the manifest says never happened.
    static auto check_entry_term(const std::string& key, const std::string& body,
                                 verification_report& report) -> void {
        std::uint64_t entry_term = 0;
        try {
            const auto parsed = boost::json::parse(body);
            entry_term =
                static_cast<std::uint64_t>(parsed.as_object().at("term").to_number<std::int64_t>());
        } catch (const std::exception& err) {
            report.problems.push_back(verification_problem{
                "unreadable_entry", key,
                std::string("the copied log entry could not be parsed for its term: ") +
                    err.what()});
            return;
        }
        if (!report.manifest.current_term) {
            // No `term` object was copied at all, so there is nothing to compare
            // against. Reported rather than passed over: a backup carrying log
            // entries but no term cannot have its monotonicity checked, and
            // silence would read as "checked and fine".
            report.problems.push_back(verification_problem{
                "term_regression", key,
                "the backup holds log entries but no term object, so the manifest's term could"
                " not be checked against entry term " +
                    std::to_string(entry_term)});
            return;
        }
        if (entry_term > *report.manifest.current_term) {
            report.problems.push_back(verification_problem{
                "term_regression", key,
                "log entry is from term " + std::to_string(entry_term) +
                    " but the manifest records current_term " +
                    std::to_string(*report.manifest.current_term) +
                    " — the source advanced its term during the copy, so this backup is a smear"});
        }
    }

    /// Every index from the snapshot's `last_included_index + 1` through the
    /// highest restorable entry must be present.
    static auto check_log_contiguity(verification_report& report,
                                     std::vector<std::uint64_t> indices) -> void {
        if (indices.empty()) {
            return;
        }
        std::sort(indices.begin(), indices.end());

        // The first index the log is required to hold. With a snapshot, that is
        // one past what the snapshot already covers; without one, the log
        // speaks for itself and only its own internal gaps are checkable.
        const std::uint64_t expected_first = report.manifest.snapshot_last_included_index
                                                 ? *report.manifest.snapshot_last_included_index + 1
                                                 : indices.front();

        if (indices.front() > expected_first) {
            report.problems.push_back(verification_problem{
                "log_gap", "log/",
                "the snapshot covers through index " +
                    std::to_string(*report.manifest.snapshot_last_included_index) +
                    " and the first copied entry is " + std::to_string(indices.front()) +
                    ", so indices " + std::to_string(expected_first) + ".." +
                    std::to_string(indices.front() - 1) + " are in neither"});
        }
        for (std::size_t i = 1; i < indices.size(); ++i) {
            if (indices[i] != indices[i - 1] + 1) {
                report.problems.push_back(verification_problem{
                    "log_gap", "log/",
                    "log index " + std::to_string(indices[i - 1] + 1) +
                        " is missing between the copied entries at " +
                        std::to_string(indices[i - 1]) + " and " + std::to_string(indices[i])});
            }
        }
    }

    [[nodiscard]] static auto serialize_manifest(const backup_manifest& manifest) -> std::string {
        boost::json::object obj;
        obj["format_version"] = manifest.format_version;
        obj["provider"] = manifest.provider;
        obj["source_bucket"] = manifest.source_bucket;
        obj["source_prefix"] = manifest.source_prefix;
        obj["backup_id"] = manifest.backup_id;
        obj["created_at"] = manifest.created_at;
        obj["source_quiesced"] = manifest.source_quiesced;

        emit_optional_u64(obj, "current_term", manifest.current_term);
        if (manifest.voted_for) {
            obj["voted_for"] = *manifest.voted_for;
        }
        emit_optional_u64(obj, "first_log_index", manifest.first_log_index);
        emit_optional_u64(obj, "last_log_index", manifest.last_log_index);
        emit_optional_u64(obj, "snapshot_last_included_index",
                          manifest.snapshot_last_included_index);
        emit_optional_u64(obj, "snapshot_last_included_term", manifest.snapshot_last_included_term);

        boost::json::array nodes;
        for (const auto& node : manifest.configuration_nodes) {
            nodes.push_back(boost::json::string(node));
        }
        obj["configuration_nodes"] = nodes;
        obj["configuration_is_joint"] = manifest.configuration_is_joint;
        boost::json::array old_nodes;
        for (const auto& node : manifest.configuration_old_nodes) {
            old_nodes.push_back(boost::json::string(node));
        }
        obj["configuration_old_nodes"] = old_nodes;

        boost::json::array objects;
        for (const auto& entry : manifest.objects) {
            boost::json::object one;
            one["key"] = entry.relative_key;
            one["size"] = entry.size_bytes;
            one["md5"] = entry.checksum_md5;
            objects.push_back(one);
        }
        obj["objects"] = objects;
        return boost::json::serialize(obj);
    }

    static auto emit_optional_u64(boost::json::object& obj, const char* name,
                                  const std::optional<std::uint64_t>& value) -> void {
        if (value) {
            obj[name] = *value;
        }
    }

    [[nodiscard]] static auto parse_manifest(const std::string& body, const std::string& key)
        -> backup_manifest {
        boost::json::value parsed;
        try {
            parsed = boost::json::parse(body);
        } catch (const std::exception& err) {
            throw std::runtime_error("object_store_backup: corrupt manifest " + key + ": " +
                                     err.what());
        }
        if (!parsed.is_object()) {
            throw std::runtime_error("object_store_backup: corrupt manifest " + key +
                                     ": not a JSON object");
        }
        const auto& obj = parsed.as_object();

        backup_manifest manifest;
        try {
            manifest.format_version =
                static_cast<int>(obj.at("format_version").to_number<std::int64_t>());
            manifest.provider = std::string(obj.at("provider").as_string());
            manifest.source_bucket = std::string(obj.at("source_bucket").as_string());
            manifest.source_prefix = std::string(obj.at("source_prefix").as_string());
            manifest.backup_id = std::string(obj.at("backup_id").as_string());
            manifest.created_at = std::string(obj.at("created_at").as_string());
            manifest.source_quiesced = obj.at("source_quiesced").as_bool();
        } catch (const std::exception& err) {
            throw std::runtime_error("object_store_backup: corrupt manifest " + key +
                                     ": missing or ill-typed required field: " + err.what());
        }

        read_optional_u64(obj, "current_term", manifest.current_term);
        if (const auto* voted = obj.if_contains("voted_for");
            voted != nullptr && voted->is_string()) {
            manifest.voted_for = std::string(voted->as_string());
        }
        read_optional_u64(obj, "first_log_index", manifest.first_log_index);
        read_optional_u64(obj, "last_log_index", manifest.last_log_index);
        read_optional_u64(obj, "snapshot_last_included_index",
                          manifest.snapshot_last_included_index);
        read_optional_u64(obj, "snapshot_last_included_term", manifest.snapshot_last_included_term);

        read_string_array(obj, "configuration_nodes", manifest.configuration_nodes);
        if (const auto* joint = obj.if_contains("configuration_is_joint");
            joint != nullptr && joint->is_bool()) {
            manifest.configuration_is_joint = joint->as_bool();
        }
        read_string_array(obj, "configuration_old_nodes", manifest.configuration_old_nodes);

        const auto* objects = obj.if_contains("objects");
        if (objects == nullptr || !objects->is_array()) {
            throw std::runtime_error("object_store_backup: corrupt manifest " + key +
                                     ": no objects array — a manifest with no inventory cannot"
                                     " verify anything, which is worse than an absent manifest");
        }
        for (const auto& element : objects->as_array()) {
            try {
                const auto& one = element.as_object();
                manifest.objects.push_back(backup_object_entry{
                    std::string(one.at("key").as_string()),
                    static_cast<std::uint64_t>(one.at("size").to_number<std::int64_t>()),
                    std::string(one.at("md5").as_string())});
            } catch (const std::exception& err) {
                throw std::runtime_error("object_store_backup: corrupt manifest " + key +
                                         ": bad object entry: " + err.what());
            }
        }
        return manifest;
    }

    static auto read_optional_u64(const boost::json::object& obj, const char* name,
                                  std::optional<std::uint64_t>& out) -> void {
        if (const auto* value = obj.if_contains(name); value != nullptr && value->is_number()) {
            out = static_cast<std::uint64_t>(value->to_number<std::int64_t>());
        }
    }

    static auto read_string_array(const boost::json::object& obj, const char* name,
                                  std::vector<std::string>& out) -> void {
        if (const auto* value = obj.if_contains(name); value != nullptr && value->is_array()) {
            for (const auto& element : value->as_array()) {
                if (element.is_string()) {
                    out.push_back(std::string(element.as_string()));
                }
            }
        }
    }

    Store _store;
    /// Scratch for `prepare_target` to report deletions back through, since it
    /// is called once per node by seed restore.
    mutable std::vector<std::string> _deleted_scratch;
};

}  // namespace kythira
