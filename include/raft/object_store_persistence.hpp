#pragma once

/// @file object_store_persistence.hpp
/// @brief Raft persistent state in a cloud key-object store — one object per
///        state item, synchronous writes, an in-memory read mirror; generic
///        over any `kythira::key_object_store`.
///
/// This is `alibaba_oss_persistence_engine`'s body, hoisted. That engine was
/// live-verified end to end against a real OSS bucket (August 14, 2026 — all
/// four real cases, including a fresh engine reading back another engine's
/// writes), and everything below — the layout, the mirror, the ordering, the
/// parse-or-throw load, the single idempotent-PUT retry — is that working
/// reference made generic, not a redesign. `alibaba_oss_persistence_engine` is
/// now an instantiation of this template over `alibaba_oss_client`.
///
/// ## Object layout
///
/// Everything one engine instance owns lives under one operator-supplied
/// `prefix` in one bucket:
///
///     <prefix>/term                        decimal text, e.g. "42"
///     <prefix>/voted_for                   decimal text or the literal "none"
///     <prefix>/log/<20-digit index>        one JSON record per log entry
///     <prefix>/snapshot                    one JSON record, single slot
///     <prefix>/snapshots/<20-digit index>  retained predecessors, only when
///                                          `snapshot_retention > 1`
///
/// The log-entry and snapshot records use **the same one-line JSON codec
/// `file_persistence_engine` writes** (`{term, index, command, type}` with the
/// command base64-encoded, and `{last_included_index, last_included_term,
/// state, nodes, is_joint_consensus, old_nodes?}` for a snapshot), so the two
/// engines' records are byte-comparable and a bucket can be diffed against a
/// data directory during a migration.
///
/// ### Why one object per log entry
///
/// The file engine keeps its whole log in a single file and rewrites that file
/// on every truncation. That is the right shape for a local filesystem, where a
/// rewrite is one `write`+`rename` and costs microseconds. It is the wrong shape
/// for object storage, where a rewrite is a full re-upload of the entire log on
/// every conflict resolution — O(log size) network bytes for an O(1) logical
/// change. Keying one object per entry instead makes:
///
///   * `append_log_entry`            → exactly one PUT,
///   * `truncate_log` / `delete_log_entries_before`
///                                   → a bounded batch of DELETEs, one per
///                                     affected entry and no others,
///   * recovery                      → one List plus one GET per live object.
///
/// ### Why 20 zero-padded digits
///
/// Every one of these stores lists keys in lexicographic order. `2^64 - 1` is
/// 20 digits, so padding every index to exactly 20 digits makes lexicographic
/// key order identical to numeric index order for every representable
/// `LogIndex` — recovery, range scans, and any human reading a bucket listing
/// all see the log in order. Without the padding, `.../log/10` would sort
/// before `.../log/9`. Recovery correctness rests on this, which is why a key
/// whose suffix is not exactly 20 digits is treated as corruption rather than
/// as a key to skip.
///
/// ## Durability contract, stated head-on
///
/// **Every mutating method issues its PUT or DELETE synchronously on the
/// calling thread and returns only after the store has answered 2xx.** There is
/// no write buffer, no background flusher, and no asynchronous path anywhere in
/// this engine — the call stack of `save_current_term` contains the socket
/// write and the socket read of the response.
///
/// That is the fsync-equivalence argument, and it is per provider: each store
/// documents what a successful write response means (replication scope, storage
/// class and account-redundancy caveats), and `doc/cloud_object_persistence.md`
/// carries that evidence table with its `documentation-derived` /
/// `live-verified` column intact. Alibaba OSS is the one row live-verified in
/// this repo. Where the claim holds, "the method returned" implies "the bytes
/// survive the loss of this instance", which is precisely what Raft's §5
/// durability requirement asks of `currentTerm` and `votedFor` before a node
/// acts on them.
///
/// This is **stronger** than `file_persistence_engine`, which writes to a
/// temporary file and renames it. That idiom gives atomicity — a reader never
/// sees a half-written file — but it does *not* give durability: neither the
/// data write nor the directory entry is fsynced, so a power loss can lose an
/// acknowledged term. The file engine is honest about being a single-host
/// development/chaos-node backend; this engine is the one you can actually lose
/// the instance under.
///
/// ### The honest cost
///
/// `save_current_term` and `save_voted_for` sit on the **election hot path** — a
/// candidate persists its term and self-vote before sending a single
/// RequestVote, and a follower persists a term bump before replying. With this
/// engine each of those is a **network round trip** rather than a
/// sub-millisecond local write, so one election round costs four sequential
/// durable writes plus an RPC round trip, giving the sizing rule
///
///     election_timeout_min  ≥  4 × p99(PUT) + rpc_rtt + margin
///
/// and the same quantity as a floor on the randomized election-timeout *range
/// width*, or nodes re-time-out inside each other's elections and livelock.
/// Size from the measured figure, not from an estimate: the real tier records
/// per-operation latency with its measurement position (in-region instance vs.
/// out-of-region host), and the operator documentation quotes it that way. The
/// only figure this project has measured so far — ~2–3 s per round trip — is a
/// cross-ocean upper bound and is not a production number.
///
/// Sustained append throughput per node cannot exceed roughly one entry per
/// store round trip, because `kythira::persistence_engine` has no batch
/// operation and this engine deliberately adds no latency-hiding mechanism:
/// no batching, no write coalescing, no write-behind, no asynchronous flush,
/// no relaxed-durability mode. Recovering append throughput requires widening
/// the concept every engine implements, which is a raft-layer change and a
/// recorded follow-on rather than something to smuggle in under a
/// configuration flag.
///
/// ## Read path: an in-memory mirror
///
/// All state is mirrored in memory behind a mutex and loaded exactly once, in
/// the constructor, with one List and one GET per live object. Every read
/// method (`load_current_term`, `get_log_entry`, `get_log_entries`,
/// `get_last_log_index`, `load_voted_for`, `load_snapshot`) answers from that
/// mirror and performs **no** network I/O — the Raft hot loop reads the log far
/// more often than it writes it, and a GET per read would be unusable.
///
/// The mirror looks like a performance optimization and is also a correctness
/// simplification: because reads happen once per process lifetime, the engine
/// is exposed to a provider's read consistency exactly once instead of
/// continuously.
///
/// The mirror is updated **after** the store call succeeds, never before. A
/// write that throws therefore leaves memory exactly equal to the store: the
/// caller treats the operation as failed, and a retry re-executes the full PUT.
///
/// ## Retry policy
///
/// A failed mutating call is retried **once, and only when it is a full-object
/// PUT** (`object_persistence_options::write_retries`). Every PUT here is a
/// deterministic, keyed, full-object overwrite — `<prefix>/term`,
/// `<prefix>/voted_for`, `<prefix>/snapshot`, and `<prefix>/log/<index>` all
/// name their object from the value being written — so re-sending it is
/// idempotent by construction: the second attempt writes the identical bytes to
/// the identical key. DELETEs are *not* retried here; the engine leaves that to
/// the caller, which is free to re-run the whole truncation (itself
/// idempotent). After the retry fails the method throws, and the raft layer's
/// existing treatment of persistence exceptions applies unchanged.
///
/// ## Snapshot retention
///
/// `object_persistence_options::snapshot_retention` is the number of snapshot
/// **generations** kept, counting the live one, and its default of `1` is
/// today's behaviour to the byte: at `1` no `<prefix>/snapshots/` object is
/// written at all, `save_snapshot` is exactly one PUT, and it issues no LIST
/// and no DELETE. The point of retaining more is narrow and worth stating,
/// because it is not backup: a snapshot written from corrupted in-memory state
/// overwrites the single slot, and without a predecessor there is nothing left
/// to go back to.
///
/// Above `1`, `save_snapshot` is three steps in this order:
///
///   (a) PUT `<prefix>/snapshots/<20-digit last_included_index>` — the retained
///       copy, same codec and same padding rationale as the log keys;
///   (b) PUT `<prefix>/snapshot` — **the commit point**;
///   (c) prune retained copies beyond `snapshot_retention`, oldest first.
///
/// No ordering of failures across those three can lose the current snapshot: a
/// failure at (a) leaves the previous state entirely intact and throws; a
/// failure at (b) leaves an unreferenced retained copy, which nothing reads; a
/// failure at (c) leaves extra copies, which costs storage and nothing else.
///
/// **(c) is therefore best-effort and does not throw.** By the time it runs the
/// snapshot is committed and the mirror updated, so reporting a failed DELETE
/// as a failed `save_snapshot` would tell the caller its snapshot was lost when
/// it was not — and the raft layer's `install_snapshot` path would abandon a
/// successful RPC over a garbage-collection error. Silence is not the
/// alternative: the failure is recorded in `last_prune_error()`, the index stays
/// in the retained set, and the next `save_snapshot` prunes it again, so the
/// condition is self-healing as well as observable.
///
/// **Recovery reads `<prefix>/snapshot` and nothing else.** Retained copies are
/// for operators and for the backup/restore tooling; the load path never GETs
/// one, which keeps recovery a single GET and keeps a corrupt retained copy
/// from being able to break startup. Construction *notes* which retained
/// indices exist, from the listing it already performs, so pruning needs no LIST
/// of its own.
///
/// Pruning deletes only what the engine can prove it wrote: a key under
/// `<prefix>/snapshots/` whose suffix is not exactly 20 digits is left alone —
/// not deleted, and, unlike the same shape under `<prefix>/log/`, not treated as
/// corruption either. The asymmetry is deliberate: a log key that cannot be
/// ordered breaks recovery, whereas a retained copy is never read by the load
/// path at all, so the safe reading of an unrecognized one is "somebody else's
/// object".
///
/// Retention is **not a backup**. Retained snapshots share a bucket, a prefix, a
/// credential and a blast radius with the thing they would be recovering from;
/// an `rm -r` of the prefix or a compromised credential takes them with it. The
/// backup story is a separate catalog with its own manifest, in
/// `.kiro/specs/cloud-object-persistence/` Requirement 10.
///
/// ## Corruption is fatal at load
///
/// If any object under the prefix fails to parse, **construction throws** and
/// the message names the offending key. This is a deliberate contrast with
/// `file_persistence_engine`, whose `load_all` wraps every parse in
/// `catch (...) {}` and silently drops what it cannot read — a limitation its
/// own header records. A truncated or half-uploaded object in a cloud store
/// must not degrade into silent state loss: a Raft node that comes back with a
/// silently shortened log can violate the Log Matching property. Failing to
/// start is recoverable by an operator; starting with invented state is not.
///
/// ## Single-writer requirement, and fencing
///
/// **Exactly one process may own a `{bucket, prefix}` pair**, exactly as one
/// process owns a `file_persistence_engine`'s directory. Two nodes pointed at
/// the same prefix will corrupt each other's state. Nothing here *coordinates*
/// that: `fencing_mode::compare_and_swap` **detects** a second writer, it does
/// not implement a lease, arbitrate, or take over.
///
/// At `fencing_mode::none` — the default, and the fifth template argument's
/// default — single-writer is by assertion, exactly as it shipped: no
/// `<prefix>/owner` object, no conditional header, and not one extra request.
///
/// ### What `compare_and_swap` conditions, and what it does not
///
// clang-format off
/// | Write | Precondition | Why |
/// |---|---|---|
/// | `<prefix>/term`, `<prefix>/voted_for` | `If-Match: <version tracked in the mirror>`, or create-only on the first write | the **safety chokepoint** |
/// | `<prefix>/log/<index>` | **create-only** | catches the stale appender the chokepoint misses |
/// | `<prefix>/snapshot`, `<prefix>/snapshots/<index>`, every DELETE | **none** | a **stated residual**, below |
// clang-format on
///
/// The chokepoint argument: a second writer cannot cause a Raft *safety*
/// violation without first writing `term` or `voted_for`. A candidate persists
/// an incremented term and a self-vote before sending any RequestVote; a
/// follower persists a term bump before replying, and its vote before granting
/// one at a term it has already recorded. There is no path to a second leader,
/// a diverging committed log or a double vote that does not pass through one of
/// those two single-slot objects.
///
/// That argument covers safety and **not corruption**, and the difference is
/// why log-entry PUTs are create-only. A stale leader appends without ever
/// changing its term — it will never gather a quorum again, but nothing makes
/// it write `term` or `voted_for` before writing `<prefix>/log/<index>`, so
/// left unconditional it interleaves log objects with the rightful owner's for
/// as long as it runs and neither writer notices. The corruption would surface
/// only at the next recovery, which is the silent-short-log failure this engine
/// exists to prevent. Create-only costs nothing — a header on a PUT already in
/// flight — and legitimate re-use of an index is always preceded by
/// `truncate_log`'s DELETE, so it never rejects a legal write.
///
/// **The residual is bounded and stated rather than covered:** a second writer
/// whose only interaction with the prefix is a snapshot overwrite or a
/// truncation is **not detected**. Claiming a total fence here would be the
/// more dangerous error, so a conformance case pins the gap open.
///
/// ### The owner object
///
/// Construction writes `<prefix>/owner` — `{owner_id, epoch, started_at}` —
/// create-only when the prefix is unowned, and `If-Match` over the recorded
/// object otherwise. The rules:
///
///   * the recorded owner is **this** `owner_id` → a restart; the epoch is
///     read-incremented, which leaves an audit trail of restarts;
///   * the recorded owner is somebody else → construction **throws**
///     `persistence_fenced_error` naming them, unless `takeover_epoch` exceeds
///     the recorded epoch, which makes a takeover an explicit operator act
///     rather than a race;
///   * `takeover_epoch` at or below the recorded epoch is rejected as an
///     operator error rather than silently ignored — an epoch that does not
///     advance is not a takeover.
///
/// A duplicated deployment shares its `owner_id` with the original, so
/// construction cannot reject it: a crash-restart and a duplicate are
/// indistinguishable at that moment, and a restart must succeed. The duplicate
/// is caught by the first conditional write instead, which is what the fence is
/// for.
///
/// The **create-only PUT** decides whether the prefix was unowned, not the
/// listing that preceded it: a listing that lags would show an owner object as
/// absent, and the precondition catches exactly that.
///
/// ### The latch
///
/// A rejected precondition throws `persistence_fenced_error` — naming the key,
/// the expected version and the provider — and **latches the engine
/// permanently**: every subsequent mutating call throws the same error without
/// contacting the store, and the latch cannot be cleared at runtime. A node that
/// has provably lost ownership of its state cannot safely continue in consensus,
/// and an engine that lets it try is worse than one with no fencing at all.
/// Reads keep answering from the mirror; they report what this node last knew,
/// and the raft layer meets the latch at its next write.
///
/// A precondition failure is **never retried** (Requirement 9.4): a retry is
/// exactly the overwrite the fence exists to prevent. Neither is a *transient*
/// failure on a conditional write, and that is a separate decision worth the
/// sentence: the retry's precondition is stale by construction if the first
/// attempt actually landed, so retrying could only convert a lost response into
/// a false fence. One attempt, then throw — and a transient failure does **not**
/// latch, because only the service's own evaluated precondition is evidence
/// about ownership. This is why a client must map a benign conditional-request
/// race (S3's `409 ConditionalRequestConflict`) to an ordinary retryable
/// exception and only `412` to `object_precondition_failed`; mapping a bare
/// status would turn a retryable race into an unclearable latch.
///
/// **The residual that follows from it, stated because it is real:** a
/// conditional PUT whose response is lost after the service applied it leaves
/// the engine holding a stale version, so its next write to that key is refused
/// and the engine latches although it is the only writer. The failure is loud
/// and safe — a node stops rather than corrupting a log — but it is an
/// availability cost of `compare_and_swap` on a lossy path, and it is inherent
/// to predicating a write on a version this engine only learns from the
/// response. The identified mitigation is a read-repair on the failure path (GET
/// the key; if it already holds exactly the bytes this engine meant to write,
/// adopt the returned version instead of latching, since a writer cannot tell
/// its own landed write from an agreeing twin and detection is then merely
/// deferred to the first genuine divergence). It is recorded as future work
/// rather than implemented here, because it weakens "a rejected precondition
/// means you lost" and that sentence is what the safety argument above is
/// written against.
///
/// ## Provider independence
///
/// Nothing in this header names a provider, a vendor error code or a
/// `KYTHIRA_HAS_*` gate, and nothing in it behaves differently per provider.
/// Anything provider-specific belongs in the client or in
/// `object_persistence_options`. The fault points are named for the same
/// reason: `raft/objstore/{put,get,delete,list}_object`, at the store boundary,
/// provider-independent because the engine is.

#include <raft/fault_injection.hpp>
#include <raft/key_object_store.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#include <boost/json.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

/// @brief How the engine detects a second writer. Exactly two values, and the
///        choice is a **template argument** rather than a runtime field.
///
/// `compare_and_swap` requires `conditional_key_object_store` at compile time
/// (Requirement 9.8), and only a compile-time selector can deliver that: a
/// runtime field would have to sit behind `if constexpr`, which would silently
/// leave a provider that cannot express the precondition running *unconditional*
/// writes with fencing configured — the one behaviour Requirement 9 exists to
/// forbid. Asking for `compare_and_swap` over such a store is therefore a
/// compile error naming the unsatisfied concept, and
/// `alibaba_oss_persistence_engine` — whose provider has no overwrite CAS at
/// all, verified live — cannot be written any other way.
enum class fencing_mode {
    /// The shipped behaviour: single-writer by assertion, no `<prefix>/owner`
    /// object, no conditional header, no extra request.
    none,
    /// Conditional writes wherever they cost no extra round trip, plus the
    /// owner object and the latch. See the header's fencing section.
    compare_and_swap
};

/// @brief Thrown when this engine has provably lost ownership of its prefix,
///        and by every subsequent mutating call thereafter.
///
/// Raised only from a precondition the *service* evaluated and rejected, or from
/// construction over a prefix another owner holds. It is deliberately distinct
/// from the `std::runtime_error` an ordinary store failure produces: a transient
/// failure says nothing about ownership, and treating one as the other in either
/// direction is the mistake this type exists to prevent. The raft layer needs no
/// special handling — it sees an exception from persistence like any other — but
/// an operator's automation can catch this one to distinguish "split brain" from
/// "the network broke".
class persistence_fenced_error : public std::runtime_error {
public:
    persistence_fenced_error(std::string provider, std::string key, object_version expected,
                             const std::string& detail)
        : std::runtime_error(
              "object_store_persistence_engine: fenced on " + key + " (provider: " + provider +
              (expected.empty() ? ", expected: absent" : ", expected version: " + expected) + ")" +
              (detail.empty() ? "" : ": " + detail) +
              " — this engine is latched and every further mutating call fails"),
          _provider(std::move(provider)),
          _key(std::move(key)),
          _expected(std::move(expected)) {}

    /// The store that refused the write (`provider_name()`).
    [[nodiscard]] auto provider() const noexcept -> const std::string& { return _provider; }
    /// The object key whose precondition was refused.
    [[nodiscard]] auto key() const noexcept -> const std::string& { return _key; }
    /// The version the write was predicated on; empty for "must not exist".
    [[nodiscard]] auto expected_version() const noexcept -> const object_version& {
        return _expected;
    }

private:
    std::string _provider;
    std::string _key;
    object_version _expected;
};

/// @brief Engine-level options. Every default preserves the shipped engine's
///        behaviour exactly, so an operator who upgrades and changes nothing
///        sees a bucket with exactly the keys it had before.
///
/// The spec's remaining options (checksum verification, a single-PUT size cap)
/// are added field by field **with** the behaviour that honours them — a field
/// that is accepted and ignored is worse than an absent one. `owner_id` and
/// `takeover_epoch` follow the same rule from the other side: they are rejected
/// at construction unless the engine is actually fenced.
struct object_persistence_options {
    /// Snapshot generations kept, counting the live `<prefix>/snapshot`. `1` —
    /// the shipped behaviour — writes no `<prefix>/snapshots/` object at all.
    /// Must be at least 1; `0` is rejected at construction rather than silently
    /// read as "keep none", which would delete the retained copy of the
    /// snapshot just written.
    std::size_t snapshot_retention{1};

    /// Same-call retries, PUT-only, and **unconditional-PUT-only**: a
    /// conditional write is never retried, for the reason the header's fencing
    /// section gives. `1` is the shipped behaviour: one retry, then throw.
    unsigned write_retries{1};

    /// Who this engine says it is in `<prefix>/owner`. Required — and required
    /// to be non-empty — under `fencing_mode::compare_and_swap`; rejected
    /// otherwise, so a fencing knob can never be set on an engine that is not
    /// fencing.
    std::string owner_id;

    /// The epoch to claim, for an **explicit, auditable** takeover of a prefix
    /// another owner holds. Unset is the normal case: an unowned prefix is
    /// claimed at epoch 1, and a restart by the recorded owner read-increments.
    /// When set it must exceed the recorded epoch — an epoch that does not
    /// advance is not a takeover, and is rejected rather than ignored.
    std::optional<std::uint64_t> takeover_epoch;

    /// Verify each PUT's returned version against the digest computed here, and
    /// throw naming the key on a mismatch. On by default, and it costs nothing on
    /// a store that does not declare `content_md5_versioned_store` — there is no
    /// digest to compare against, so nothing is computed either.
    ///
    /// This is the **local** half of Requirement 7. The service-side half — a
    /// `Content-MD5` (or native) header the service evaluates and rejects on
    /// mismatch — is each client's own configuration, because the engine does not
    /// speak HTTP and cannot spell a header. The two halves are deliberately not
    /// one flag: a single knob spanning both layers would be honoured by one of
    /// them and silently ignored by the other.
    bool verify_checksums{true};

    /// Refuse any single PUT above this size. **64 MiB, deliberately far below
    /// every provider's documented single-request limit** (the smallest is S3's
    /// and OSS's 5 GB; spike-notes.md Finding 16): the binding constraint is this
    /// engine's shape, not the service's. One retry, no multipart, no resumption,
    /// no progress reporting, the mutex held for the whole round trip, and the
    /// only latency this project has measured is ~2-3 s per round trip — so a
    /// multi-gigabyte single PUT is an hours-long request that can only be
    /// retried whole. AWS's own guidance abandons single-PUT above 100 MB.
    ///
    /// The cap exists to turn "this deployment has outgrown a single-PUT
    /// persistence engine" into a loud error at the first snapshot that reaches
    /// it, rather than to predict any service's 413. Configurable upward for an
    /// operator who has measured their own case; `0` is rejected at construction,
    /// since it would refuse every write including the empty one.
    std::size_t max_object_bytes{64U * 1024U * 1024U};
};

namespace object_store_persistence_detail {

/// The owner object's `started_at`: ISO 8601 UTC at second precision,
/// `YYYY-MM-DDTHH:MM:SSZ`, formatted from the broken-down time explicitly so it
/// is locale-independent. Written locally rather than borrowed from one of the
/// provider signing headers, which already have this function — nothing in this
/// file may name a provider.
[[nodiscard]] inline auto iso8601_utc(std::chrono::system_clock::time_point when) -> std::string {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::array<char, 32> buf{};
    const int written =
        std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900,
                      utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    if (written <= 0) {
        return {};
    }
    return std::string(buf.data(), static_cast<std::size_t>(written));
}

/// @brief MD5 of `data`, lowercase hex — the content checksum every one of these
///        five services speaks (`Content-MD5`, and the ETag itself on S3 and
///        OSS).
///
/// **Used only as a checksum against transport corruption**, which is the one
/// job the services use it for. Nothing here is a security claim, and MD5 must
/// not be reached for as one.
///
/// Hand-rolled rather than taken from OpenSSL for the same reason this file
/// carries its own base64 codec: `object_store_persistence.hpp` must compile in
/// a build with **every** cloud gate off (Requirement 16.1), and OpenSSL reaches
/// this tree only through the gated provider signing headers. A hand-rolled
/// digest with no known-answer test would be worse than no digest at all, so the
/// unit test pins it against RFC 1321's full test suite **and** against the
/// three padding boundaries (55, 56 and 64 bytes), which is where an
/// implementation of this shape actually goes wrong.
[[nodiscard]] inline auto md5_hex(std::string_view data) -> std::string {
    // T[i] = floor(|sin(i + 1)| × 2^32), RFC 1321 §3.4.
    static constexpr std::array<std::uint32_t, 64> k_t = {
        0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U,
        0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U,
        0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU,
        0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U, 0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
        0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U,
        0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU,
        0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U, 0xf4292244U,
        0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
        0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU,
        0xeb86d391U};
    static constexpr std::array<unsigned, 64> k_shift = {
        7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
        14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
        4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};

    const auto rotl = [](std::uint32_t v, unsigned n) -> std::uint32_t {
        return (v << n) | (v >> (32U - n));
    };

    // The padded message is never materialised: `byte_at` synthesises the
    // 0x80 terminator, the zero fill and the little-endian bit length, so a
    // multi-megabyte snapshot is not copied to be hashed.
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    std::size_t padded = data.size() + 1;
    while (padded % 64U != 56U) {
        ++padded;
    }
    const std::size_t length_offset = padded;
    padded += 8;

    const auto byte_at = [&](std::size_t i) -> std::uint32_t {
        if (i < data.size()) {
            return static_cast<std::uint8_t>(data[i]);
        }
        if (i == data.size()) {
            return 0x80U;
        }
        if (i < length_offset) {
            return 0U;
        }
        return static_cast<std::uint32_t>((bit_length >> (8U * (i - length_offset))) & 0xFFU);
    };

    std::array<std::uint32_t, 4> h = {0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U};
    for (std::size_t offset = 0; offset < padded; offset += 64) {
        std::array<std::uint32_t, 16> m{};
        for (std::size_t j = 0; j < 16; ++j) {
            const std::size_t at = offset + (4U * j);
            m[j] = byte_at(at) | (byte_at(at + 1) << 8U) | (byte_at(at + 2) << 16U) |
                   (byte_at(at + 3) << 24U);
        }
        std::uint32_t a = h[0];
        std::uint32_t b = h[1];
        std::uint32_t c = h[2];
        std::uint32_t d = h[3];
        for (unsigned i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            unsigned g = 0;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = ((5U * i) + 1U) % 16U;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = ((3U * i) + 5U) % 16U;
            } else {
                f = c ^ (b | ~d);
                g = (7U * i) % 16U;
            }
            f += a + k_t[i] + m[g];
            a = d;
            d = c;
            c = b;
            b += rotl(f, k_shift[i]);
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }

    static constexpr std::string_view k_hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const std::uint32_t word : h) {
        // Little-endian, per RFC 1321 §3.5.
        for (unsigned byte = 0; byte < 4; ++byte) {
            const auto value = static_cast<std::uint8_t>((word >> (8U * byte)) & 0xFFU);
            out += k_hex[value >> 4U];
            out += k_hex[value & 0x0FU];
        }
    }
    return out;
}

/// @brief A returned version reduced to a comparable digest: surrounding quotes
///        dropped, hex lowercased.
///
/// Both normalisations are load-bearing and both come from measurements. S3
/// spells the ETag's hex **lowercase** and OSS **uppercase**, and OSS's client
/// carries the ETag through verbatim — quotes included — because it is an opaque
/// token that goes back to the service unmodified in a later precondition.
/// Normalising at the comparison instead of at the client is what keeps those two
/// facts from having to agree.
[[nodiscard]] inline auto normalise_content_digest(std::string_view version) -> std::string {
    if (version.size() >= 2 && version.front() == '"' && version.back() == '"') {
        version.remove_prefix(1);
        version.remove_suffix(1);
    }
    std::string out(version);
    for (char& c : out) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

}  // namespace object_store_persistence_detail

/// @brief Raft persistent state stored as individual objects in a key-object
///        store.
///
/// @tparam Store    Must satisfy `kythira::key_object_store`, and
///                  `kythira::conditional_key_object_store` as well when
///                  `Fencing` is `fencing_mode::compare_and_swap`.
/// @tparam NodeId   Node identifier type; defaults to `std::uint64_t`.
/// @tparam TermId   Term number type; defaults to `std::uint64_t`.
/// @tparam LogIndex Log index type; defaults to `std::uint64_t`.
/// @tparam Fencing  Second-writer detection; defaults to `fencing_mode::none`,
///                  which is the shipped behaviour to the request. Last in the
///                  list so every existing instantiation is unaffected.
template<key_object_store Store, typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t, fencing_mode Fencing = fencing_mode::none>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex> &&
         (Fencing == fencing_mode::none || conditional_key_object_store<Store>)
class object_store_persistence_engine {
public:
    using store_t = Store;
    using log_entry_t = log_entry<TermId, LogIndex>;
    using snapshot_t = snapshot<NodeId, TermId, LogIndex>;

    /// Number of digits every log index is zero-padded to: `2^64 - 1` is 20
    /// digits, so this is the width at which lexicographic key order and
    /// numeric index order coincide for every representable index.
    static constexpr std::size_t k_index_digits = 20;

    /// Whether this engine detects a second writer, readable without naming the
    /// template argument again.
    static constexpr fencing_mode fencing = Fencing;

    /// @brief Construct over `{store, bucket, prefix}`, load the mirror, and —
    ///        under `compare_and_swap` — claim the prefix.
    ///
    /// @throws std::invalid_argument      if a required field is empty, an option
    ///                                    is out of range, or a fencing option is
    ///                                    set on an unfenced engine.
    /// @throws std::runtime_error         if the listing or any GET fails, or if
    ///                                    any object under the prefix fails to
    ///                                    parse — the message names the offending
    ///                                    key.
    /// @throws persistence_fenced_error   under `compare_and_swap`, if
    ///                                    `<prefix>/owner` names a different
    ///                                    owner and no adequate `takeover_epoch`
    ///                                    was supplied, or if another writer
    ///                                    claimed the prefix during construction.
    ///
    /// An empty prefix is the normal cold-start case: the standard empty state
    /// (term 0, no vote, empty log, no snapshot) is established and **no object
    /// is written** until the first mutation — except the owner object, which is
    /// the whole construction-time cost of fencing.
    object_store_persistence_engine(Store store, std::string bucket, std::string prefix,
                                    object_persistence_options opts = {})
        : _store(std::move(store)),
          _bucket(std::move(bucket)),
          _prefix(std::move(prefix)),
          _opts(std::move(opts)) {
        if (_bucket.empty()) {
            throw std::invalid_argument("object_store_persistence_engine: bucket is empty");
        }
        // A trailing slash is accepted and normalised away so that
        // `"raft"` and `"raft/"` name the same objects rather than two
        // near-identical key spaces (`raft/term` vs `raft//term`).
        while (!_prefix.empty() && _prefix.back() == '/') {
            _prefix.pop_back();
        }
        if (_prefix.empty()) {
            throw std::invalid_argument("object_store_persistence_engine: prefix is empty");
        }
        if (_opts.snapshot_retention == 0) {
            throw std::invalid_argument(
                "object_store_persistence_engine: snapshot_retention must be at least 1");
        }
        if (_opts.max_object_bytes == 0) {
            throw std::invalid_argument(
                "object_store_persistence_engine: max_object_bytes must be at least 1 — 0 would "
                "refuse every write, including the empty one");
        }
        if constexpr (Fencing == fencing_mode::compare_and_swap) {
            if (_opts.owner_id.empty()) {
                throw std::invalid_argument(
                    "object_store_persistence_engine: owner_id is required "
                    "under fencing_mode::compare_and_swap — the owner "
                    "object has to name somebody");
            }
        } else {
            // The mirror image of "no field that is accepted and ignored": a
            // fencing knob on an unfenced engine reads as fencing being on.
            if (!_opts.owner_id.empty() || _opts.takeover_epoch) {
                throw std::invalid_argument(
                    "object_store_persistence_engine: owner_id / takeover_epoch are set but this "
                    "engine is not fenced — instantiate it with "
                    "fencing_mode::compare_and_swap (which requires a "
                    "conditional_key_object_store) rather than leaving the options ignored");
            }
        }
        load_all();
        if constexpr (Fencing == fencing_mode::compare_and_swap) {
            // After the load, so a corrupt prefix fails construction without
            // this engine having first written its name into it.
            claim_ownership();
        }
    }

    /// Move-only, and only safe before the engine is shared with the Raft loop —
    /// the same restriction, for the same reason, as `file_persistence_engine`.
    object_store_persistence_engine(object_store_persistence_engine&& other) noexcept
        : _store(std::move(other._store)),
          _bucket(std::move(other._bucket)),
          _prefix(std::move(other._prefix)),
          _opts(other._opts),
          _current_term(other._current_term),
          _voted_for(std::move(other._voted_for)),
          _log(std::move(other._log)),
          _snapshot(std::move(other._snapshot)),
          _retained(std::move(other._retained)),
          _last_prune_error(std::move(other._last_prune_error)),
          _versions(std::move(other._versions)),
          _owner_seen(std::move(other._owner_seen)),
          _owner_epoch(other._owner_epoch),
          _fenced(std::move(other._fenced)) {}

    auto operator=(object_store_persistence_engine&&) -> object_store_persistence_engine& = delete;
    object_store_persistence_engine(const object_store_persistence_engine&) = delete;
    auto operator=(const object_store_persistence_engine&)
        -> object_store_persistence_engine& = delete;
    ~object_store_persistence_engine() = default;

    // ── Key layout ───────────────────────────────────────────────────────────

    /// @brief The object key holding `currentTerm`.
    [[nodiscard]] auto term_key() const -> std::string { return _prefix + "/term"; }

    /// @brief The object key holding `votedFor`.
    [[nodiscard]] auto voted_for_key() const -> std::string { return _prefix + "/voted_for"; }

    /// @brief The single snapshot slot's object key.
    [[nodiscard]] auto snapshot_key() const -> std::string { return _prefix + "/snapshot"; }

    /// @brief The object key recording who owns this prefix. Written and read
    ///        only under `fencing_mode::compare_and_swap`; under
    ///        `fencing_mode::none` an object at this key is a foreign object like
    ///        any other, neither read nor written.
    [[nodiscard]] auto owner_key() const -> std::string { return _prefix + "/owner"; }

    /// @brief The object key holding the entry at `index`, zero-padded to 20
    ///        digits so lexicographic listing order is numeric index order.
    ///        Public because the padding is a documented, test-pinned part of
    ///        the on-bucket format.
    [[nodiscard]] auto log_key(LogIndex index) const -> std::string {
        return log_prefix() + pad_index(index);
    }

    /// @brief The common prefix of every log-entry key, trailing slash included.
    [[nodiscard]] auto log_prefix() const -> std::string { return _prefix + "/log/"; }

    /// @brief The common prefix of every retained snapshot copy, trailing slash
    ///        included. Nothing is written under it at
    ///        `snapshot_retention == 1`.
    [[nodiscard]] auto snapshots_prefix() const -> std::string { return _prefix + "/snapshots/"; }

    /// @brief The key a retained copy of the snapshot ending at `index` is
    ///        written to, padded to the same 20 digits as a log key so a
    ///        listing reads in index order.
    [[nodiscard]] auto retained_snapshot_key(LogIndex index) const -> std::string {
        return snapshots_prefix() + pad_index(index);
    }

    /// @brief The store this engine writes through, for tests and for tooling
    ///        that needs the provider's name.
    [[nodiscard]] auto store() const -> const Store& { return _store; }

    // ── Fencing state (Requirement 9) ────────────────────────────────────────

    /// @brief Whether this engine has been fenced out and latched.
    ///
    /// Always `false` under `fencing_mode::none`. Once `true` it stays `true` for
    /// the life of the object: the latch is not clearable at runtime, by design.
    [[nodiscard]] auto is_fenced() const -> bool {
        std::lock_guard lock(_mu);
        return _fenced.has_value();
    }

    /// @brief The epoch this engine recorded in `<prefix>/owner`, or `0` when
    ///        unfenced. An operator reading two nodes' epochs can tell which
    ///        claim is the newer one.
    [[nodiscard]] auto owner_epoch() const -> std::uint64_t {
        std::lock_guard lock(_mu);
        return _owner_epoch;
    }

    // ── currentTerm ──────────────────────────────────────────────────────────

    auto save_current_term(TermId term) -> void {
        std::lock_guard lock(_mu);
        throw_if_fenced();
        put_single_slot(term_key(), std::to_string(static_cast<unsigned long long>(term)));
        _current_term = term;
    }

    auto load_current_term() -> TermId {
        std::lock_guard lock(_mu);
        return _current_term;
    }

    // ── votedFor ─────────────────────────────────────────────────────────────

    auto save_voted_for(NodeId node) -> void {
        std::lock_guard lock(_mu);
        throw_if_fenced();
        if constexpr (std::is_same_v<NodeId, std::string>) {
            put_single_slot(voted_for_key(), node);
        } else {
            put_single_slot(voted_for_key(), std::to_string(static_cast<unsigned long long>(node)));
        }
        _voted_for = std::move(node);
    }

    auto load_voted_for() -> std::optional<NodeId> {
        std::lock_guard lock(_mu);
        return _voted_for;
    }

    // ── Log ──────────────────────────────────────────────────────────────────

    /// @brief One PUT — **create-only** under `compare_and_swap`, which is what
    ///        catches a stale leader that appends without ever changing its term.
    auto append_log_entry(const log_entry_t& entry) -> void {
        std::lock_guard lock(_mu);
        throw_if_fenced();
        const std::string key = log_key(entry.index());
        const std::string body = entry_to_json(entry);
        if constexpr (Fencing == fencing_mode::compare_and_swap) {
            // Create-only, always: legitimate re-use of an index is always
            // preceded by `truncate_log`'s DELETE (the follower path only appends
            // beyond its last index, and a conflicting index is truncated first),
            // so this never refuses a legal write. The returned version is *not*
            // tracked — a log object is never conditionally rewritten, and
            // keeping a version per entry would grow the mirror with the log for
            // nothing.
            conditional_put(key, body, precondition{if_absent{}});
        } else {
            put_object(key, body);
        }
        _log[entry.index()] = entry;
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
        for (auto it = _log.lower_bound(start); it != _log.end() && it->first <= end; ++it) {
            result.push_back(it->second);
        }
        return result;
    }

    auto get_last_log_index() -> LogIndex {
        std::lock_guard lock(_mu);
        if (_log.empty()) {
            return LogIndex{0};
        }
        // `_log` is ordered, so the greatest index is the last element.
        return _log.rbegin()->first;
    }

    /// @brief Delete every entry with index ≥ `index` — a bounded batch of
    ///        DELETEs, one per affected object.
    ///
    /// Each object is erased from the mirror only after its DELETE is
    /// acknowledged, so a failure part-way through leaves the mirror equal to
    /// the store and the caller free to re-run the (idempotent) truncation.
    auto truncate_log(LogIndex index) -> void {
        std::lock_guard lock(_mu);
        throw_if_fenced();
        auto it = _log.lower_bound(index);
        while (it != _log.end()) {
            delete_object(log_key(it->first));
            it = _log.erase(it);
        }
    }

    /// @brief Delete every entry with index &lt; `index` (compaction after a
    ///        snapshot), with the same delete-then-forget ordering.
    auto delete_log_entries_before(LogIndex index) -> void {
        std::lock_guard lock(_mu);
        throw_if_fenced();
        auto it = _log.begin();
        while (it != _log.end() && it->first < index) {
            delete_object(log_key(it->first));
            it = _log.erase(it);
        }
    }

    // ── Snapshot ─────────────────────────────────────────────────────────────

    /// @brief Overwrite the single `<prefix>/snapshot` object, retaining
    ///        predecessors when `snapshot_retention > 1`.
    ///
    /// At the default retention of 1 this is exactly one PUT, as it has always
    /// been. Above 1 it is retained-copy PUT, then the commit-point PUT, then a
    /// best-effort prune — see the header's retention section for why a failure
    /// at each of the three steps is survivable and why only the first two
    /// throw.
    auto save_snapshot(const snapshot_t& snap) -> void {
        std::lock_guard lock(_mu);
        throw_if_fenced();
        // Both PUTs and the prune's DELETEs stay unconditional even under
        // `compare_and_swap` — the bounded residual the header states rather than
        // covers. A snapshot is not a single-slot value with a version this engine
        // tracks across writers, and predicating the commit point on one would
        // make a legitimate `install_snapshot` fail against a prefix an operator
        // had restored.
        const std::string body = snapshot_to_json(snap);
        const LogIndex index = snap.last_included_index();
        const bool retaining = _opts.snapshot_retention > 1;

        if (retaining) {
            // (a) The retained copy first. If this throws, nothing has changed:
            // the live slot still holds the previous snapshot.
            put_object(retained_snapshot_key(index), body);
            // Recorded even though (b) may still fail — the object exists, and
            // an unreferenced retained copy is inert but prunable.
            _retained.insert(index);
        }

        // (b) The commit point.
        put_object(snapshot_key(), body);
        _snapshot = snap;

        if (retaining) {
            // (c) Housekeeping, after the commit point and deliberately unable
            // to fail the call.
            prune_retained();
        }
    }

    auto load_snapshot() -> std::optional<snapshot_t> {
        std::lock_guard lock(_mu);
        return _snapshot;
    }

    /// @brief Why the last prune stopped early, if it did.
    ///
    /// Pruning retained snapshot copies runs after the commit point and cannot
    /// fail `save_snapshot` (header, retention section), so this is where a
    /// failed DELETE becomes visible rather than silent. It is cleared by the
    /// next prune that completes, and the copies it could not delete are
    /// retried by that prune — an operator seeing this persist is looking at a
    /// standing permissions or connectivity problem, not a lost snapshot.
    [[nodiscard]] auto last_prune_error() const -> std::optional<std::string> {
        std::lock_guard lock(_mu);
        return _last_prune_error;
    }

private:
    /// What `<prefix>/owner` says, plus the version it was read at — the version
    /// the takeover PUT is predicated on.
    struct owner_record {
        std::string owner_id;
        std::uint64_t epoch{0};
        object_version version;
    };

    // ── Store boundary ───────────────────────────────────────────────────────
    //
    // Every store call is wrapped by exactly one libfiu fault point, following
    // `memory_persistence_engine`'s `raft/persistence/...` precedent but placed
    // at the store boundary so a chaos run can fail a single storage operation
    // rather than a whole persistence method. The names carry no provider:
    // a chaos configuration written against this engine works for every store
    // it is instantiated over.

    /// Refuse an oversized PUT **before** sending it (Requirement 7.3).
    ///
    /// Both PUT paths call this; the owner object does not, because its size is
    /// fixed by this engine rather than by caller data — there is no input that
    /// can make it large, so a cap on it would only be a cap that never fires.
    ///
    /// The message carries all three facts an operator needs: what was written,
    /// what the cap is, and that multipart is a **documented non-goal** rather
    /// than a missing feature. A snapshot silently truncated at a provider limit
    /// is the worst outcome available here, so this error is the deliverable.
    auto check_object_size(const std::string& key, std::string_view bytes) const -> void {
        if (bytes.size() <= _opts.max_object_bytes) {
            return;
        }
        throw std::runtime_error(
            "object_store_persistence_engine: refusing to PUT " + key + ": " +
            std::to_string(bytes.size()) + " bytes exceeds max_object_bytes of " +
            std::to_string(_opts.max_object_bytes) +
            ". This engine issues one whole-object PUT with no multipart upload, no resumption and "
            "no progress reporting, and multipart is a documented non-goal of this design — raise "
            "max_object_bytes only if you have measured that a single PUT of this size completes "
            "within your election timeout");
    }

    /// Compare what the store reported against the digest of what was sent, where
    /// the store declares its version **is** that digest (Requirement 7.2).
    ///
    /// A no-op — not even a digest computed — on the three providers whose version
    /// is an opaque token, and on any store that says nothing. The two arguments
    /// are named for what they are rather than `a` and `b`, because a check whose
    /// inputs can be transposed without a type error eventually is.
    auto verify_returned_digest(const std::string& key, std::string_view bytes,
                                const object_version& reported_by_store) const -> void {
        if constexpr (content_md5_versioned_store<Store>) {
            if (!_opts.verify_checksums) {
                return;
            }
            const std::string computed_from_content =
                object_store_persistence_detail::md5_hex(bytes);
            const std::string reported =
                object_store_persistence_detail::normalise_content_digest(reported_by_store);
            if (reported != computed_from_content) {
                throw std::runtime_error(
                    "object_store_persistence_engine: checksum mismatch on " + key + ": sent " +
                    std::to_string(bytes.size()) + " bytes with MD5 " + computed_from_content +
                    " but " + std::string(_store.provider_name()) + " reported " +
                    (reported.empty() ? std::string("nothing") : reported) +
                    " — the stored object does not match what was written and must not be treated "
                    "as persisted");
            }
        } else {
            (void)key;
            (void)bytes;
            (void)reported_by_store;
        }
    }

    /// PUT with the single idempotent-overwrite retry.
    auto put_object(const std::string& key, std::string_view bytes) -> void {
        check_object_size(key, bytes);
        fiu_do_on("raft/objstore/put_object",
                  throw std::runtime_error("chaos: raft/objstore/put_object " + key););
        unsigned attempts_left = _opts.write_retries;
        std::string first_what;
        while (true) {
            try {
                // The digest check is inside the retry, deliberately: a mismatch
                // is a corrupted transfer, which is exactly what re-sending the
                // identical bytes to the identical key repairs.
                const put_result result = _store.put_object(_bucket, key, bytes);
                verify_returned_digest(key, bytes, result.version);
                return;
            } catch (const std::exception& e) {
                if (attempts_left == 0) {
                    if (first_what.empty()) {
                        throw std::runtime_error("object_store_persistence_engine: PUT " + key +
                                                 " failed: " + e.what());
                    }
                    throw std::runtime_error("object_store_persistence_engine: PUT " + key +
                                             " failed twice — first: " + first_what +
                                             "; retry: " + e.what());
                }
                // Safe to re-send: this is a full-object overwrite at a
                // deterministic key, so the retry writes the identical bytes to
                // the identical place whether or not the first attempt landed.
                if (first_what.empty()) {
                    first_what = e.what();
                }
                --attempts_left;
            }
        }
    }

    /// A single-slot value (`term`, `voted_for`): the CAS chokepoint under
    /// `compare_and_swap`, an ordinary retried PUT otherwise. Callers hold `_mu`.
    auto put_single_slot(const std::string& key, std::string_view bytes) -> void {
        if constexpr (Fencing == fencing_mode::compare_and_swap) {
            _versions[key] = conditional_put(key, bytes, precondition_for(key));
        } else {
            put_object(key, bytes);
        }
    }

    /// What this engine believes about `key`: the version it last saw, or
    /// "must not exist" if it has never seen the object at all.
    ///
    /// The parameters of the comparison this feeds are named `expected` and
    /// `actual` at the store, never two versions in a row — a check whose two
    /// inputs can be transposed without a type error eventually is.
    [[nodiscard]] auto precondition_for(const std::string& key) const -> precondition {
        auto it = _versions.find(key);
        if (it == _versions.end()) {
            return precondition{if_absent{}};
        }
        return precondition{if_version{it->second}};
    }

    /// A conditional PUT: **one attempt, never retried**, and the only place a
    /// fence latches. Callers hold `_mu`.
    ///
    /// Only `object_precondition_failed` — a precondition the service itself
    /// evaluated and rejected — latches. Anything else is an ordinary store
    /// failure that says nothing about ownership, and is reported as one; that
    /// asymmetry is what keeps a provider's benign conditional-request race (S3's
    /// `409`) from becoming an unclearable latch, and it is why the retry is
    /// absent here (see the header).
    auto conditional_put(const std::string& key, std::string_view bytes, const precondition& pre)
        -> object_version {
        check_object_size(key, bytes);
        fiu_do_on("raft/objstore/put_object",
                  throw std::runtime_error("chaos: raft/objstore/put_object " + key););
        object_version version;
        try {
            version = _store.put_object_if(_bucket, key, bytes, pre).version;
        } catch (const object_precondition_failed& e) {
            latch(key, expected_version_of(pre), e.what());
        } catch (const std::exception& e) {
            throw std::runtime_error("object_store_persistence_engine: conditional PUT " + key +
                                     " failed: " + e.what());
        }
        // Outside the catch, so a checksum mismatch keeps its own message rather
        // than being re-wrapped as a conditional-write failure — and so it cannot
        // be mistaken for a precondition failure, which would latch.
        verify_returned_digest(key, bytes, version);
        return version;
    }

    /// Record the fence and raise it. Callers hold `_mu`.
    [[noreturn]] auto latch(const std::string& key, object_version expected,
                            const std::string& detail) -> void {
        _fenced.emplace(std::string(_store.provider_name()), key, std::move(expected), detail);
        throw *_fenced;
    }

    /// Throw the recorded fence, without contacting the store. Callers hold `_mu`.
    auto throw_if_fenced() const -> void {
        if constexpr (Fencing == fencing_mode::compare_and_swap) {
            if (_fenced) {
                throw *_fenced;
            }
        }
    }

    static auto expected_version_of(const precondition& pre) -> object_version {
        if (const auto* at = std::get_if<if_version>(&pre)) {
            return at->expected;
        }
        return {};
    }

    /// DELETE, no retry: only full-overwrite PUTs are retried here. These stores
    /// answer success for a key that was never there, so a caller-level re-run
    /// of the whole truncation is the idempotent recovery.
    auto delete_object(const std::string& key) -> void {
        fiu_do_on("raft/objstore/delete_object",
                  throw std::runtime_error("chaos: raft/objstore/delete_object " + key););
        _store.delete_object(_bucket, key);
    }

    /// The full `get_result`, version included: the load path is where a fenced
    /// engine learns the versions its first conditional writes are predicated on,
    /// so discarding them here would make the first write of every restart a
    /// create-only one against an object that exists.
    auto get_object(const std::string& key) -> std::optional<get_result> {
        fiu_do_on("raft/objstore/get_object",
                  throw std::runtime_error("chaos: raft/objstore/get_object " + key););
        return _store.get_object(_bucket, key);
    }

    auto list_keys(const std::string& prefix) -> std::vector<std::string> {
        fiu_do_on("raft/objstore/list_object",
                  throw std::runtime_error("chaos: raft/objstore/list_object " + prefix););
        return _store.list_keys(_bucket, prefix);
    }

    // ── Retention ────────────────────────────────────────────────────────────

    /// Delete retained copies beyond `snapshot_retention`, oldest first.
    /// Callers hold `_mu`.
    ///
    /// Best-effort by design: it runs after the commit point, so the first
    /// failed DELETE stops it, is recorded, and leaves the remaining copies for
    /// the next `save_snapshot` to try again. An index is dropped from
    /// `_retained` only after its DELETE is acknowledged, so the in-memory set
    /// never claims to have removed an object that is still there.
    auto prune_retained() -> void {
        while (_retained.size() > _opts.snapshot_retention) {
            const LogIndex oldest = *_retained.begin();
            try {
                delete_object(retained_snapshot_key(oldest));
            } catch (const std::exception& e) {
                _last_prune_error = "object_store_persistence_engine: DELETE " +
                                    retained_snapshot_key(oldest) +
                                    " failed while pruning retained snapshots (the current "
                                    "snapshot is committed): " +
                                    e.what();
                return;
            }
            _retained.erase(_retained.begin());
        }
        _last_prune_error.reset();
    }

    // ── Initialisation ───────────────────────────────────────────────────────

    /// One List plus one GET per live object. Anything that will not parse
    /// throws with its key named.
    auto load_all() -> void {
        const std::string term = term_key();
        const std::string vote = voted_for_key();
        const std::string snap = snapshot_key();
        const std::string owner = owner_key();
        const std::string log_pfx = log_prefix();
        const std::string snaps_pfx = snapshots_prefix();

        for (const auto& key : list_keys(_prefix + "/")) {
            // A key that vanished between the List and the GET is a
            // concurrent-writer symptom (there is not supposed to be one) —
            // but it is also what a not-yet-consistent listing would look
            // like, so treat absent as absent rather than inventing state
            // for it.
            if (key == term) {
                auto got = get_object(key);
                if (got) {
                    remember_version(key, got->version);
                    _current_term = static_cast<TermId>(parse_unsigned(got->body, key));
                }
            } else if (key == vote) {
                auto got = get_object(key);
                if (got) {
                    // The version is remembered whatever the body says: the
                    // `"none"` sentinel is an object that exists, so a fenced
                    // engine's first vote must be an If-Match and not a
                    // create-only.
                    remember_version(key, got->version);
                    if (got->body != "none" && !got->body.empty()) {
                        if constexpr (std::is_same_v<NodeId, std::string>) {
                            _voted_for = got->body;
                        } else {
                            _voted_for = static_cast<NodeId>(parse_unsigned(got->body, key));
                        }
                    }
                }
            } else if (key == snap) {
                auto got = get_object(key);
                if (got && !got->body.empty()) {
                    _snapshot =
                        wrap_parse<snapshot_t>(key, [&got] { return json_to_snapshot(got->body); });
                }
            } else if (key == owner) {
                // Read only when fencing; under `none` this falls through as a
                // foreign object, which is what "no extra request cost" means.
                if constexpr (Fencing == fencing_mode::compare_and_swap) {
                    auto got = get_object(key);
                    if (got) {
                        _owner_seen = wrap_parse<owner_record>(
                            key, [&got] { return json_to_owner(got->body); });
                        _owner_seen->version = got->version;
                    }
                }
            } else if (key.starts_with(snaps_pfx)) {
                // Noted, never read: recovery is `<prefix>/snapshot` and
                // nothing else, so a corrupt retained copy cannot break
                // startup. What the listing buys here is a prune that needs no
                // LIST of its own.
                //
                // A suffix that is not exactly 20 digits is *not* corruption —
                // the opposite of the same shape under `<prefix>/log/`, and for
                // the reason that asymmetry exists: this object is never read,
                // so the safe reading of an unrecognized one is that it belongs
                // to somebody else. Leaving it out of `_retained` is what makes
                // "the engine deletes only what it can prove it wrote"
                // structural rather than a convention.
                const std::string_view suffix = std::string_view(key).substr(snaps_pfx.size());
                if (suffix.size() == k_index_digits &&
                    suffix.find_first_not_of("0123456789") == std::string_view::npos) {
                    _retained.insert(static_cast<LogIndex>(parse_unsigned(suffix, key)));
                }
            } else if (key.starts_with(log_pfx)) {
                const LogIndex index =
                    index_from_key(std::string_view(key).substr(log_pfx.size()), key);
                auto got = get_object(key);
                if (!got) {
                    continue;
                }
                auto entry =
                    wrap_parse<log_entry_t>(key, [&got] { return json_to_entry(got->body); });
                if (entry.index() != index) {
                    // The key *is* the index; a record that disagrees with its
                    // own key means the log's ordering guarantee no longer
                    // holds, which is exactly the silent-loss case this engine
                    // exists to prevent.
                    throw std::runtime_error(
                        "object_store_persistence_engine: corrupt object " + key +
                        ": record carries index " +
                        std::to_string(static_cast<unsigned long long>(entry.index())) +
                        " but its key names index " +
                        std::to_string(static_cast<unsigned long long>(index)));
                }
                _log[index] = std::move(entry);
            }
            // Any other key under the prefix belongs to something else (an
            // operator's note, a future format's object). It is not this
            // engine's state, so it is neither read nor written — only keys
            // this format defines are parsed, and those must parse.
        }
    }

    /// Remember the version of a single-slot key, and only when fencing needs it.
    auto remember_version(const std::string& key, const object_version& version) -> void {
        if constexpr (Fencing == fencing_mode::compare_and_swap) {
            _versions[key] = version;
        } else {
            (void)key;
            (void)version;
        }
    }

    // ── Ownership (Requirement 9.6) ──────────────────────────────────────────

    /// Claim `<prefix>/owner`. Called from the constructor, after `load_all`, only
    /// under `compare_and_swap`.
    ///
    /// The decision is made from what the load path already read, but it is
    /// **enforced by the precondition on the PUT**, not by that reading: a listing
    /// that lagged would show an owner object as absent, and the create-only
    /// precondition is what refuses to be fooled by it.
    auto claim_ownership() -> void {
        if (!_owner_seen) {
            _owner_epoch = _opts.takeover_epoch.value_or(1);
            write_owner(precondition{if_absent{}});
            return;
        }

        const owner_record& recorded = *_owner_seen;
        if (_opts.takeover_epoch) {
            if (*_opts.takeover_epoch <= recorded.epoch) {
                throw std::invalid_argument(
                    "object_store_persistence_engine: takeover_epoch " +
                    std::to_string(*_opts.takeover_epoch) + " does not advance past " +
                    owner_key() + "'s recorded epoch " + std::to_string(recorded.epoch) +
                    " (owner \"" + recorded.owner_id +
                    "\") — an epoch that does not advance is not a takeover");
            }
            _owner_epoch = *_opts.takeover_epoch;
        } else if (recorded.owner_id == _opts.owner_id) {
            // A restart by the recorded owner. Read-incremented, so consecutive
            // starts are distinguishable in the bucket. A duplicated deployment
            // shares the owner id and reaches here too — indistinguishable from a
            // restart at this moment, which is why the fence lives on the writes
            // rather than on construction.
            _owner_epoch = recorded.epoch + 1;
        } else {
            throw persistence_fenced_error(
                std::string(_store.provider_name()), owner_key(), {},
                "the prefix is owned by \"" + recorded.owner_id + "\" at epoch " +
                    std::to_string(recorded.epoch) + " and this engine is \"" + _opts.owner_id +
                    "\" — set takeover_epoch above " + std::to_string(recorded.epoch) +
                    " to take it over deliberately");
        }
        write_owner(precondition{if_version{recorded.version}});
    }

    /// PUT the owner object under `pre`. A refused precondition means another
    /// writer claimed the prefix between the load and this PUT, which is a
    /// construction failure rather than a latch — there is no engine yet to latch.
    auto write_owner(const precondition& pre) -> void {
        boost::json::object obj;
        obj["owner_id"] = _opts.owner_id;
        obj["epoch"] = _owner_epoch;
        obj["started_at"] =
            object_store_persistence_detail::iso8601_utc(std::chrono::system_clock::now());
        const std::string body = boost::json::serialize(obj);

        const std::string key = owner_key();
        fiu_do_on("raft/objstore/put_object",
                  throw std::runtime_error("chaos: raft/objstore/put_object " + key););
        try {
            _store.put_object_if(_bucket, key, body, pre);
        } catch (const object_precondition_failed&) {
            // One extra GET, on the failure path only, so the error can name who
            // won rather than only that somebody did.
            std::string holder;
            try {
                if (auto got = get_object(key)) {
                    const owner_record now = json_to_owner(got->body);
                    holder = " it is now owned by \"" + now.owner_id + "\" at epoch " +
                             std::to_string(now.epoch) + ".";
                }
            } catch (const std::exception&) {
                // Naming the winner is a courtesy; failing to is not a second
                // failure to report.
            }
            throw persistence_fenced_error(
                std::string(_store.provider_name()), key, expected_version_of(pre),
                "another writer claimed this prefix while this engine was starting." + holder);
        } catch (const std::exception& e) {
            throw std::runtime_error("object_store_persistence_engine: PUT " + key +
                                     " failed while claiming the prefix: " + e.what());
        }
    }

    // ── Parsing helpers, all of which name the key they choked on ────────────

    template<typename T, typename Fn>
    static auto wrap_parse(const std::string& key, Fn&& parse) -> T {
        try {
            return parse();
        } catch (const std::exception& e) {
            throw std::runtime_error("object_store_persistence_engine: corrupt object " + key +
                                     ": " + e.what());
        }
    }

    /// Strict decimal parse: the *whole* body must be digits. `std::stoull`
    /// would happily read "12" out of "12garbage"; a persistence engine that
    /// does that turns a truncated upload into a plausible-looking term.
    static auto parse_unsigned(std::string_view body, const std::string& key)
        -> unsigned long long {
        // Tolerate a single trailing newline: an operator repairing state with
        // a shell writes one whether they mean to or not.
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
            body.remove_suffix(1);
        }
        unsigned long long value = 0;
        const char* first = body.data();
        const char* last = body.data() + body.size();
        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc{} || ptr != last || body.empty()) {
            throw std::runtime_error("object_store_persistence_engine: corrupt object " + key +
                                     ": expected a decimal number, got \"" + std::string(body) +
                                     "\"");
        }
        return value;
    }

    /// The inverse of `log_key`'s padding, and strict about it: a log key whose
    /// suffix is not exactly `k_index_digits` digits cannot be ordered against
    /// the others, so it is corruption rather than a key to skip.
    static auto index_from_key(std::string_view suffix, const std::string& key) -> LogIndex {
        if (suffix.size() != k_index_digits ||
            suffix.find_first_not_of("0123456789") != std::string_view::npos) {
            throw std::runtime_error("object_store_persistence_engine: corrupt object " + key +
                                     ": log key suffix must be exactly " +
                                     std::to_string(k_index_digits) + " decimal digits");
        }
        return static_cast<LogIndex>(parse_unsigned(suffix, key));
    }

    static auto pad_index(LogIndex index) -> std::string {
        std::string digits = std::to_string(static_cast<unsigned long long>(index));
        if (digits.size() >= k_index_digits) {
            return digits;
        }
        return std::string(k_index_digits - digits.size(), '0') + digits;
    }

    // ── Serialisation — byte-identical to file_persistence_engine's codec ────

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
        e._term = static_cast<TermId>(obj.at("term").as_int64());
        e._index = static_cast<LogIndex>(obj.at("index").as_int64());
        e._command = base64_to_bytes(std::string(obj.at("command").as_string()));
        e._type = obj.contains("type") ? static_cast<entry_type>(obj.at("type").as_int64())
                                       : entry_type::normal;
        return e;
    }

    static auto snapshot_to_json(const snapshot_t& snap) -> std::string {
        boost::json::object obj;
        obj["last_included_index"] = snap.last_included_index();
        obj["last_included_term"] = snap.last_included_term();
        obj["state"] = bytes_to_base64(snap.state_machine_state());
        boost::json::array nodes;
        for (const auto& n : snap.configuration().nodes()) {
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
            for (const auto& n : *snap.configuration().old_nodes()) {
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

    /// Strict, because a corrupt owner object under fencing must fail
    /// construction rather than be read as "unowned" — which would hand the
    /// prefix to a second writer at exactly the moment the fence was asked for.
    /// The `version` field is filled in by the caller from the GET.
    static auto json_to_owner(const std::string& s) -> owner_record {
        auto obj = boost::json::parse(s).as_object();
        owner_record rec;
        rec.owner_id = std::string(obj.at("owner_id").as_string());
        rec.epoch = static_cast<std::uint64_t>(obj.at("epoch").to_number<std::uint64_t>());
        if (rec.owner_id.empty()) {
            throw std::runtime_error("owner object names an empty owner_id");
        }
        return rec;
    }

    static auto json_to_snapshot(const std::string& s) -> snapshot_t {
        auto obj = boost::json::parse(s).as_object();
        snapshot_t snap;
        snap._last_included_index = static_cast<LogIndex>(obj.at("last_included_index").as_int64());
        snap._last_included_term = static_cast<TermId>(obj.at("last_included_term").as_int64());
        snap._state_machine_state = base64_to_bytes(std::string(obj.at("state").as_string()));
        for (const auto& n : obj.at("nodes").as_array()) {
            if constexpr (std::is_same_v<NodeId, std::string>) {
                snap._configuration._nodes.emplace_back(n.as_string());
            } else {
                snap._configuration._nodes.push_back(static_cast<NodeId>(n.as_int64()));
            }
        }
        snap._configuration._is_joint_consensus =
            obj.contains("is_joint_consensus") && obj.at("is_joint_consensus").as_bool();
        if (snap._configuration._is_joint_consensus && obj.contains("old_nodes")) {
            std::vector<NodeId> old_nodes;
            for (const auto& n : obj.at("old_nodes").as_array()) {
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

    // ── Base64 (same codec and alphabet as file_persistence_engine) ──────────

    static constexpr std::string_view k_b64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    static auto bytes_to_base64(const std::vector<std::byte>& in) -> std::string {
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        for (std::size_t i = 0; i < in.size(); i += 3) {
            std::uint32_t v = static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i])) << 16U;
            if (i + 1 < in.size()) {
                v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i + 1])) << 8U;
            }
            if (i + 2 < in.size()) {
                v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i + 2]));
            }
            out += k_b64[(v >> 18U) & 0x3FU];
            out += k_b64[(v >> 12U) & 0x3FU];
            out += (i + 1 < in.size()) ? k_b64[(v >> 6U) & 0x3FU] : '=';
            out += (i + 2 < in.size()) ? k_b64[v & 0x3FU] : '=';
        }
        return out;
    }

    /// Decodes strictly: a character outside the alphabet throws rather than
    /// being skipped the way the file engine skips it. Skipping is how a
    /// corrupted command silently decodes to *some* byte string, which the
    /// state machine would then apply.
    static auto base64_to_bytes(const std::string& in) -> std::vector<std::byte> {
        static const auto tbl = [] {
            std::array<std::int8_t, 256> t{};
            t.fill(-1);
            for (int i = 0; i < 64; ++i) {
                t[static_cast<std::uint8_t>(k_b64[static_cast<std::size_t>(i)])] =
                    static_cast<std::int8_t>(i);
            }
            return t;
        }();
        std::vector<std::byte> out;
        out.reserve(in.size() * 3 / 4);
        std::uint32_t v = 0;
        int bits = 0;
        for (const char c : in) {
            if (c == '=') {
                break;
            }
            const std::int8_t b = tbl[static_cast<std::uint8_t>(c)];
            if (b < 0) {
                throw std::runtime_error("base64: invalid character in encoded command");
            }
            v = (v << 6U) | static_cast<std::uint32_t>(b);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<std::byte>((v >> static_cast<unsigned>(bits)) & 0xFFU));
            }
        }
        return out;
    }

    Store _store;
    std::string _bucket;
    std::string _prefix;
    object_persistence_options _opts;

    mutable std::mutex _mu;
    TermId _current_term{0};
    std::optional<NodeId> _voted_for;
    /// Ordered, not hashed: `get_log_entries`, `truncate_log`,
    /// `delete_log_entries_before`, and `get_last_log_index` are all range
    /// operations, and the order is the same one the key padding gives the
    /// bucket.
    std::map<LogIndex, log_entry_t> _log;
    std::optional<snapshot_t> _snapshot;

    /// The indices of the retained snapshot copies this engine knows it wrote
    /// or found under `<prefix>/snapshots/`. Ordered, because pruning is
    /// oldest-first. Never holds an index whose key shape the engine did not
    /// recognize, which is what keeps pruning from touching a foreign object.
    std::set<LogIndex> _retained;
    std::optional<std::string> _last_prune_error;

    // ── Fencing (Requirement 9), all inert under `fencing_mode::none` ─────────

    /// The version this engine last saw for each **single-slot** key —
    /// `<prefix>/term` and `<prefix>/voted_for`, and only those. Log objects are
    /// written create-only and never conditionally rewritten, so tracking a
    /// version per entry would grow this map with the log for no reader.
    std::unordered_map<std::string, object_version> _versions;

    /// What the load path found at `<prefix>/owner`, before this engine claimed
    /// it. Consumed by `claim_ownership` and then only of historical interest.
    std::optional<owner_record> _owner_seen;

    /// The epoch this engine claimed; `0` when unfenced.
    std::uint64_t _owner_epoch{0};

    /// Set once when a precondition is refused, and never cleared: the latch is
    /// not clearable at runtime, because a node that has provably lost its state
    /// cannot safely rejoin consensus by being asked nicely.
    std::optional<persistence_fenced_error> _fenced;
};

/// @brief `object_store_persistence_engine` with second-writer detection on.
///
/// The constraint sits on the alias's own parameter, so asking for a fenced
/// engine over a store that cannot express a precondition fails at the name
/// rather than deep inside the template — which is the difference between a
/// legible compile error and a page of them.
template<conditional_key_object_store Store, typename NodeId = std::uint64_t,
         typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
using fenced_object_store_persistence_engine =
    object_store_persistence_engine<Store, NodeId, TermId, LogIndex,
                                    fencing_mode::compare_and_swap>;

namespace object_store_persistence_detail {

/// The smallest thing that satisfies `key_object_store`, so the concept
/// assertion below can be made at file scope without naming a provider. It is
/// never instantiated — only its signatures are read.
struct concept_check_store {
    auto put_object(const std::string&, const std::string&, std::string_view) const -> put_result;
    auto get_object(const std::string&, const std::string&) const -> std::optional<get_result>;
    auto delete_object(const std::string&, const std::string&) const -> void;
    auto list_keys(const std::string&, const std::string&) const -> std::vector<std::string>;
    auto provider_name() const -> std::string_view;
};

static_assert(key_object_store<concept_check_store>);
static_assert(!conditional_key_object_store<concept_check_store>,
              "the unconditional check store must stay unconditional — it is what pins that "
              "fencing_mode::compare_and_swap is unavailable for such a store");

/// The same, refined: enough to check that the fenced engine compiles at file
/// scope, again without naming a provider.
struct conditional_concept_check_store : concept_check_store {
    auto put_object_if(const std::string&, const std::string&, std::string_view,
                       const precondition&) const -> put_result;
    auto delete_object_if(const std::string&, const std::string&, const precondition&) const
        -> void;
};

static_assert(conditional_key_object_store<conditional_concept_check_store>);

}  // namespace object_store_persistence_detail

/// The concept is checked here, at file scope, so a signature drift is a
/// compile error in every translation unit that includes this header — not just
/// in whichever test happens to instantiate it.
static_assert(
    persistence_engine<
        object_store_persistence_engine<object_store_persistence_detail::concept_check_store>,
        std::uint64_t, std::uint64_t, std::uint64_t, log_entry<>, snapshot<>>,
    "object_store_persistence_engine must satisfy the persistence_engine concept");

/// …and a fenced engine is the same engine: fencing changes what it sends, never
/// the interface the raft layer holds it through.
static_assert(
    persistence_engine<fenced_object_store_persistence_engine<
                           object_store_persistence_detail::conditional_concept_check_store>,
                       std::uint64_t, std::uint64_t, std::uint64_t, log_entry<>, snapshot<>>,
    "a fenced object_store_persistence_engine must satisfy the persistence_engine "
    "concept too");

namespace object_store_persistence_detail {

/// @brief Whether a fenced engine can be instantiated over `S` at all.
///
/// A concept rather than a bare requires-expression at the assertion site: a
/// *non-dependent* requires-expression naming an ill-formed specialization is a
/// hard error, because there is no substitution to fail. Written once here so
/// both this header and the unit test's negative compile test read the same way.
template<typename S>
concept fenced_engine_instantiable = requires {
    typename object_store_persistence_engine<S, std::uint64_t, std::uint64_t, std::uint64_t,
                                             fencing_mode::compare_and_swap>;
};

}  // namespace object_store_persistence_detail

/// Requirement 9.8, as a compile-time fact rather than a promise: a store that
/// cannot express a precondition has no fenced engine to instantiate at all.
/// This is the assertion that would fail if somebody "made it work" by dropping
/// the constraint and letting `compare_and_swap` fall back to unconditional
/// writes.
static_assert(
    !object_store_persistence_detail::fenced_engine_instantiable<
        object_store_persistence_detail::concept_check_store>,
    "fencing_mode::compare_and_swap must be unavailable for a store that is not a "
    "conditional_key_object_store — a silently unconditional fence is worse than no fence");
static_assert(object_store_persistence_detail::fenced_engine_instantiable<
                  object_store_persistence_detail::conditional_concept_check_store>,
              "…and available for one that is, or the assertion above proves nothing");

}  // namespace kythira
