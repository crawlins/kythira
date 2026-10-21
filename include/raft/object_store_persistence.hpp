// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

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
///     <prefix>/log/<20-digit index>        one JSON record per log entry, or
///                                          a segment of them (see below)
///     <prefix>/snapshot                    one JSON record, single slot
///     <prefix>/snapshots/<20-digit index>  retained predecessors, only when
///                                          `snapshot_retention > 1`
///
/// That is the default, `hard_state_layout::separate`. Under
/// `hard_state_layout::combined` `<prefix>/term` holds both values as one JSON
/// record and `<prefix>/voted_for` is never written (see `hard_state_layout`).
/// Under `log_layout::segmented` a run of appended entries may share one log
/// object, `{"first_index":f,"last_index":l,"entries":[…]}` at
/// `<prefix>/log/<f>`, each element exactly the record a per-entry object
/// holds (see `log_layout`). The two knobs are independent.
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
///   * `append_log_entries`          → one PUT per entry, several in flight
///                                     at once when the store allows it,
///   * `truncate_log` / `delete_log_entries_before`
///                                   → a bounded batch of DELETEs, one per
///                                     affected entry and no others,
///   * recovery                      → one List plus one GET per live object.
///
/// `log_layout::segmented` keeps every one of those properties for a
/// *segment*, a bounded run of entries (`max_segment_entries`,
/// `max_segment_bytes`) that one `append_log_entries` call wrote together. A
/// run then costs one PUT per segment rather than per entry, and recovery one
/// GET per segment. A truncation or compaction rewrites at most the one
/// segment it cuts, never the log, so the full re-upload this layout exists to
/// avoid is still impossible. Objects are keyed by their first index, so
/// listing order is still log order, and a bucket of per-entry objects is
/// already a valid segmented log.
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
/// Under `hard_state_layout::combined` the term and vote are one object, so
/// each side pays one PUT and the rule becomes `2 × p99(PUT)`. On GCS keep the
/// floor at about one second regardless: that service throttles mutations of a
/// single object to about one per second, and `<prefix>/term` is one object.
///
/// and the same quantity as a floor on the randomized election-timeout *range
/// width*, or nodes re-time-out inside each other's elections and livelock.
/// Size from the measured figure, not from an estimate: the real tier records
/// per-operation latency with its measurement position (in-region instance vs.
/// out-of-region host), and the operator documentation quotes it that way. The
/// only figure this project has measured so far — ~2–3 s per round trip — is a
/// cross-ocean upper bound and is not a production number.
///
/// `save_hard_state` (`.kiro/specs/batched-durable-writes/`) trims that: it
/// writes only the slots that changed, so granting a vote at a term already on
/// the store is one PUT rather than two. A candidate still pays two — the term
/// and the vote are separate objects, and they are written term first — and so
/// does a follower that learns the term from the RequestVote it grants, which
/// `node` stores once rather than first on stepping down.
///
/// Append throughput is the other cost. With only `append_log_entry`, a
/// follower can take at most about one entry per store round trip.
/// `append_log_entries` sends the entries of one AppendEntries concurrently
/// (`object_persistence_options::append_concurrency`, on stores that declare
/// `concurrent_key_object_store`), so a run of N entries costs about
/// N / `append_concurrency` round trips of wall time. Under
/// `log_layout::segmented` the run is one PUT per segment instead, so a
/// follower's catch-up and a restart's recovery cost about N / B requests
/// rather than N, where B is the entries a segment holds. Either way the
/// write is synchronous before the call returns: no write-behind and no
/// relaxed-durability mode. A leader's own appends are one per client
/// command, so a cluster still commits at most about `1 / p50(PUT)` entries a
/// second under either layout; `.kiro/specs/object-store-group-commit/` is
/// the change that lifts that.
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
/// Under `hard_state_layout::combined` the chokepoint is one object: a second
/// writer cannot change this node's term or vote without writing
/// `<prefix>/term`. The vote slot then matters only as somewhere a
/// *pre-migration* writer can still write, and the migration's retirement of it
/// — `If-Match` its loaded version — is what detects that writer: one that
/// writes the slot before the retirement makes the retirement refused and the
/// migrating engine latches; one that writes it after is refused itself. **The
/// residual is the window between the two migration PUTs**: while a transient
/// failure keeps the retirement pending, a stale writer's same-term vote goes
/// undetected. Fenced construction completes a pending retirement, which bounds
/// that window to one process lifetime.
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
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

/// @brief Where `currentTerm` and `votedFor` live on the bucket
///        (`.kiro/specs/object-store-batched-layout/`, Requirement 2).
///
/// A runtime option rather than a template argument like `fencing_mode`: the
/// rule that made fencing a template argument — a capability the store may lack
/// must be a compile error — does not apply, because both layouts use only
/// `put_object` / `put_object_if`, which every shipped client has.
enum class hard_state_layout : std::uint8_t {
    /// The shipped format: `<prefix>/term` holds the decimal term and
    /// `<prefix>/voted_for` the decimal vote or `none`, written by two ordered
    /// PUTs, term first.
    separate,
    /// Both in one object, `<prefix>/term` ←
    /// `{"term":<uint64>,"voted_for":<NodeId>|null}`: every hard-state write is
    /// one PUT, atomic because every one of these stores writes a whole object
    /// atomically per key, and still the single compare-and-swap chokepoint
    /// under fencing. The body lives in the key an older binary already reads
    /// and is not a bare decimal, so an older binary — engine or backup tool —
    /// refuses the prefix rather than reading a stale pair from it.
    ///
    /// A prefix written by the `separate` layout is migrated **in place and
    /// lazily**: construction loads the legacy pair and writes nothing for it,
    /// and the first hard-state write is (m1) a PUT of the combined record to
    /// `<prefix>/term` — `If-Match` the legacy object's version under fencing,
    /// and the commit point — then (m2) a PUT of `retired` to
    /// `<prefix>/voted_for`, `If-Match` its loaded version under fencing, if
    /// that object exists. m2 runs after the commit point, so a transient
    /// failure of it does not fail the call: it is retried before the next
    /// hard-state write, and under `compare_and_swap` at the next construction
    /// too. A crash at any boundary leaves either the legacy pair, which every
    /// binary reads, or a combined `<prefix>/term`, which this binary reads as
    /// the new pair and every older one refuses — never an older pair than one
    /// this node acknowledged.
    combined
};

/// @brief How log entries are grouped into objects
///        (`.kiro/specs/object-store-batched-layout/`, Requirement 5).
///
/// A runtime option for the same reason as `hard_state_layout`: both layouts
/// use only operations every shipped client has.
enum class log_layout : std::uint8_t {
    /// The shipped format: one object per entry, `<prefix>/log/<index>` holding
    /// that entry's record.
    per_entry,
    /// An extending `append_log_entries` run is written as **segments**: objects
    /// keyed by their first index, `<prefix>/log/<first>` ←
    /// `{"first_index":f,"last_index":l,"entries":[<record f>,…,<record l>]}`,
    /// each element byte-identical to a singleton's body, each segment at most
    /// `max_segment_entries` entries and `max_segment_bytes` of body. A chunk of
    /// one entry is a singleton, and every other write (`append_log_entry`, a
    /// truncation or compaction that leaves one entry of a segment) is too.
    ///
    /// A prefix of singletons is a valid segmented log, so enabling this needs
    /// no migration. Going back does: an engine configured `per_entry` refuses a
    /// prefix holding any segment, and so does every older binary, whose entry
    /// parser requires a top-level `term` — so a rollback onto a segmented
    /// bucket is refused rather than served a short log.
    segmented
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

    /// Log-entry PUTs `append_log_entries` keeps in flight at once
    /// (`.kiro/specs/batched-durable-writes/`). Honoured only when the store
    /// declares `concurrent_key_object_store`; any other store gets one request
    /// at a time whatever this says, because the engine cannot know whether the
    /// client is safe to call from several threads. The request count is the
    /// same at every setting — one PUT per entry, the same keys and the same
    /// bytes — only how many wait on the network together changes. `0` is
    /// rejected at construction rather than read as "unlimited".
    std::size_t append_concurrency{8};

    /// `separate` — the shipped format, to the byte and the request — or
    /// `combined`, which makes every hard-state write one PUT. See
    /// `hard_state_layout`. An engine configured `separate` refuses a prefix
    /// whose `<prefix>/term` holds a combined record: an in-place downgrade is
    /// not supported, and reading the stale vote slot beside it would be the
    /// silent rollback the combined layout exists to prevent.
    hard_state_layout hard_state{hard_state_layout::separate};

    /// `per_entry` — the shipped format — or `segmented`. See `log_layout`.
    log_layout log{log_layout::per_entry};

    /// The most entries one segment holds. The default, 100, is `node`'s
    /// default `max_entries_per_append`, so a full AppendEntries is one segment.
    /// Must be at least 2 (one entry is always a singleton), and is rejected at
    /// any value but the default under `log_layout::per_entry`.
    std::size_t max_segment_entries{100};

    /// The largest segment body. It bounds the in-place rewrite a truncation
    /// pays and the GET recovery pays per object. Must be at least 1 and at most
    /// `max_object_bytes`, and is rejected at any value but the default under
    /// `log_layout::per_entry`. An entry whose own record is larger is written
    /// as a singleton, bounded by `max_object_bytes` alone.
    std::size_t max_segment_bytes{1024U * 1024U};
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

/// The vote slot's body once a migrated prefix has retired it
/// (`hard_state_layout::combined`). Never read as a vote: the engine's load
/// path checks for it before parsing one, and `object_store_backup` ignores it.
inline constexpr std::string_view k_retired_vote = "retired";

/// A combined `<prefix>/term` record with its `voted_for` still JSON.
struct combined_hard_state_record {
    std::uint64_t term{0};
    /// `null`, or a node id in whichever JSON type the writer's `NodeId` uses.
    boost::json::value voted_for;
};

/// The half of the combined hard-state codec that does not depend on `NodeId`:
/// exactly `term` then `voted_for`, `term` an unsigned JSON integer read through
/// `to_number<std::uint64_t>` so the whole `TermId` range round-trips. Anything
/// else — a missing or extra member, a float, a string term — throws.
///
/// Shared by the engine, which narrows `voted_for` to its own `NodeId`, and by
/// `object_store_backup`, which is generic over the store rather than `NodeId`
/// and so renders it. One parser for the record's shape means the backup tool
/// cannot accept a record the engine refuses, or the other way round.
[[nodiscard]] inline auto parse_combined_hard_state(std::string_view s)
    -> combined_hard_state_record {
    const boost::json::value parsed = boost::json::parse(s);
    const boost::json::object& obj = parsed.as_object();
    if (obj.size() != 2 || obj.begin()->key() != "term" ||
        std::next(obj.begin())->key() != "voted_for") {
        throw std::runtime_error(
            "a combined hard-state record holds exactly \"term\" then \"voted_for\"");
    }
    const boost::json::value& term = obj.begin()->value();
    if (!term.is_int64() && !term.is_uint64()) {
        throw std::runtime_error("the combined hard-state record's term is not an integer");
    }
    return combined_hard_state_record{term.to_number<std::uint64_t>(),
                                      std::next(obj.begin())->value()};
}

/// A log segment's range, as `parse_segment` checked it.
struct segment_range {
    std::uint64_t first{0};
    std::uint64_t last{0};
};

/// Whether a parsed `<prefix>/log/<index>` body is a segment rather than a
/// singleton record (`log_layout::segmented`): an object with a `first_index`
/// member. A singleton has none — it is one entry's record — and nothing else
/// is either.
[[nodiscard]] inline auto is_segment(const boost::json::value& parsed) -> bool {
    const boost::json::object* obj = parsed.if_object();
    return obj != nullptr && obj->contains("first_index");
}

/// The half of the segment codec that does not depend on the entry type:
/// exactly `first_index`, `last_index`, `entries`, in that order;
/// `first_index` equal to the key's index; at least two entries; as many
/// elements as the range names, each an object carrying its own `index` in
/// order. Shared by the engine, which then decodes each element, and by
/// `object_store_backup`, which reads each element's term — one parser for
/// the shape, so neither accepts a segment the other refuses. Strict for the
/// reason the combined record is: it makes this binary refuse a *future*
/// shape the way older binaries refuse this one.
[[nodiscard]] inline auto parse_segment(const boost::json::object& obj, std::uint64_t key_index)
    -> segment_range {
    if (obj.size() != 3 || obj.begin()->key() != "first_index" ||
        std::next(obj.begin())->key() != "last_index" ||
        std::next(obj.begin(), 2)->key() != "entries") {
        throw std::runtime_error(
            "a log segment holds exactly \"first_index\", \"last_index\" then \"entries\"");
    }
    const auto as_index = [](const boost::json::value& v, const char* what) {
        if (!v.is_uint64() && (!v.is_int64() || v.as_int64() < 0)) {
            throw std::runtime_error(std::string("the segment's ") + what +
                                     " is not an unsigned integer");
        }
        return v.to_number<std::uint64_t>();
    };
    const segment_range range{as_index(obj.at("first_index"), "first_index"),
                              as_index(obj.at("last_index"), "last_index")};
    if (range.first != key_index) {
        throw std::runtime_error("segment starts at index " + std::to_string(range.first) +
                                 " but its key names index " + std::to_string(key_index));
    }
    if (range.last <= range.first) {
        throw std::runtime_error(
            "a segment's last_index must exceed its first_index — one entry "
            "is always a singleton");
    }
    const boost::json::array& elements = obj.at("entries").as_array();
    if (elements.empty() || elements.size() - 1 != range.last - range.first) {
        throw std::runtime_error("segment [" + std::to_string(range.first) + ".." +
                                 std::to_string(range.last) + "] holds " +
                                 std::to_string(elements.size()) + " entries");
    }
    for (std::size_t k = 0; k < elements.size(); ++k) {
        const auto index = elements[k].as_object().at("index").to_number<std::uint64_t>();
        if (index != range.first + k) {
            throw std::runtime_error("segment element " + std::to_string(k) + " carries index " +
                                     std::to_string(index) + ", expected " +
                                     std::to_string(range.first + k));
        }
    }
    return range;
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
        if (_opts.append_concurrency == 0) {
            throw std::invalid_argument(
                "object_store_persistence_engine: append_concurrency must be at least 1");
        }
        if (_opts.max_object_bytes == 0) {
            throw std::invalid_argument(
                "object_store_persistence_engine: max_object_bytes must be at least 1 — 0 would "
                "refuse every write, including the empty one");
        }
        validate_log_layout_options();
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
            // A migration whose m2 a previous process did not finish: completed
            // now rather than at the first hard-state write, which bounds the
            // window in which a pre-migration writer's vote goes undetected to
            // one process lifetime. Fenced construction already writes the
            // owner object, so this adds no new kind of startup write; a
            // refused precondition fails construction like a refused owner
            // claim, and a transient failure stays pending.
            if (_term_holds_combined) {
                retire_vote_slot_if_pending();
            }
        }
    }

    /// Move-only, and only safe before the engine is shared with the Raft loop —
    /// the same restriction, for the same reason, as `file_persistence_engine`.
    object_store_persistence_engine(object_store_persistence_engine&& other) noexcept
        : _store(std::move(other._store)),
          _bucket(std::move(other._bucket)),
          _prefix(std::move(other._prefix)),
          _opts(other._opts),
          _write_seq(other._write_seq),
          _current_term(other._current_term),
          _voted_for(std::move(other._voted_for)),
          _log(std::move(other._log)),
          _snapshot(std::move(other._snapshot)),
          _retained(std::move(other._retained)),
          _last_prune_error(std::move(other._last_prune_error)),
          _strays(std::move(other._strays)),
          _segments(std::move(other._segments)),
          _versions(std::move(other._versions)),
          _owner_seen(std::move(other._owner_seen)),
          _owner_epoch(other._owner_epoch),
          _fenced(std::move(other._fenced)),
          _term_holds_combined(other._term_holds_combined),
          _vote_slot_to_retire(std::move(other._vote_slot_to_retire)),
          _last_retirement_error(std::move(other._last_retirement_error)) {}

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

    /// Under `hard_state_layout::combined`, one PUT of `{term, the mirror's
    /// vote}` to `<prefix>/term`.
    auto save_current_term(TermId term) -> void {
        std::lock_guard lock(_mu);
        throw_if_fenced();
        if (combined_hard_state()) {
            put_combined_hard_state(term, _voted_for);
            return;
        }
        put_single_slot(term_key(), std::to_string(static_cast<unsigned long long>(term)));
        _current_term = term;
    }

    auto load_current_term() -> TermId {
        std::lock_guard lock(_mu);
        return _current_term;
    }

    // ── votedFor ─────────────────────────────────────────────────────────────

    /// Under `hard_state_layout::combined`, one PUT of `{the mirror's term,
    /// node}` to `<prefix>/term`, and nothing to `<prefix>/voted_for`.
    auto save_voted_for(NodeId node) -> void {
        std::lock_guard lock(_mu);
        throw_if_fenced();
        if (combined_hard_state()) {
            put_combined_hard_state(_current_term, std::move(node));
            return;
        }
        put_single_slot(voted_for_key(), render_vote(node));
        _voted_for = std::move(node);
    }

    auto load_voted_for() -> std::optional<NodeId> {
        std::lock_guard lock(_mu);
        return _voted_for;
    }

    /// @brief `hard_state_persistence_engine` (include/raft/persistence.hpp):
    ///        `term` then `voted_for`, each only if it changed.
    ///
    /// The order is the concept's: a crash between the two PUTs leaves the new
    /// term with the old vote, never the old term with the new vote, which would
    /// be a second vote in a term this node already voted in. Skipping an
    /// unchanged slot is the saving — a vote granted at a term this engine
    /// already holds is one PUT, not the two that `save_voted_for` followed by
    /// `save_current_term` cost — and "no vote" is the `none` sentinel the load
    /// path has always read back as no vote.
    ///
    /// Under `compare_and_swap` both stay the CAS chokepoint they are in the
    /// single-value methods: this is the same `put_single_slot`.
    ///
    /// Under `hard_state_layout::combined` it is **one** PUT of both values to
    /// `<prefix>/term`, or none when neither changed. There is no intermediate
    /// state for a crash to leave: the store writes the object whole, so a
    /// reader sees the old pair or the new one.
    auto save_hard_state(TermId term, std::optional<NodeId> vote) -> void {
        std::lock_guard lock(_mu);
        throw_if_fenced();
        if (combined_hard_state()) {
            if (term == _current_term && vote == _voted_for) {
                return;
            }
            put_combined_hard_state(term, std::move(vote));
            return;
        }
        if (term != _current_term) {
            put_single_slot(term_key(), std::to_string(static_cast<unsigned long long>(term)));
            _current_term = term;
        }
        if (vote != _voted_for) {
            put_single_slot(voted_for_key(), vote ? render_vote(*vote) : std::string("none"));
            _voted_for = std::move(vote);
        }
    }

    // ── Log ──────────────────────────────────────────────────────────────────

    /// @brief One PUT — **create-only** under `compare_and_swap`, which is what
    ///        catches a stale leader that appends without ever changing its term.
    ///
    /// Under `log_layout::segmented` it is still one singleton, and an index a
    /// segment covers is refused with `std::invalid_argument` before any
    /// request: a singleton there would sit inside the segment, off the chain,
    /// while the mirror believed it was the log.
    auto append_log_entry(const log_entry_t& entry) -> void {
        std::lock_guard lock(_mu);
        append_log_entry_locked(entry);
    }

    // ── barriered_persistence_engine (include/raft/persistence.hpp) ─────────
    //
    // `.kiro/specs/object-store-group-commit/` task 1. Every append is a PUT
    // the store acknowledged before the call returns, so a barrier has nothing
    // to wait for and the engine answers `durability_class::synchronous`:
    // durable, and never blocking, so `node` keeps its lock exactly as it did
    // when this engine had no barrier, and still hands a follower's run to
    // `append_log_entries` as one call. The requests are unchanged.

    /// @brief `append_log_entry`, returning the sequence assigned to it under
    ///        the same lock that wrote it.
    auto append_log_entry_sequenced(const log_entry_t& entry) -> write_sequence {
        std::lock_guard lock(_mu);
        append_log_entry_locked(entry);
        return _write_seq;
    }

    /// @brief Returns at once: every append through @p seq is already on the
    ///        store.
    auto barrier_through(write_sequence /*seq*/) -> void {}

    /// @brief The last sequence assigned, which is also the last one durable.
    [[nodiscard]] auto durable_through() const -> write_sequence {
        std::lock_guard lock(_mu);
        return _write_seq;
    }

    [[nodiscard]] static auto durability() -> durability_class {
        return durability_class::synchronous;
    }

private:
    auto append_log_entry_locked(const log_entry_t& entry) -> void {
        throw_if_fenced();
        if (!_segments.empty() && object_of(entry.index()).second != entry.index()) {
            throw std::invalid_argument(
                "object_store_persistence_engine: append_log_entry at index " +
                std::to_string(static_cast<unsigned long long>(entry.index())) +
                ", which a segment holds — truncate the log first");
        }
        clear_strays({});
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
        ++_write_seq;
    }

public:
    /// @brief `bulk_append_persistence_engine` (include/raft/persistence.hpp):
    ///        one PUT per entry, up to `append_concurrency` of them in flight.
    ///
    /// The request count is unchanged — this engine still writes one object per
    /// entry, for the reasons the header gives — but N entries cost about
    /// N / `append_concurrency` round trips of wall time instead of N. Each PUT
    /// is exactly the one `append_log_entry` would send: same key, same body,
    /// the same single retry when unconditional, create-only under
    /// `compare_and_swap`.
    ///
    /// **All or nothing, as seen through this engine's reads.** The mirror is
    /// updated only after every PUT has succeeded. If any fails, every entry of
    /// the run that was sent is recorded as a **stray** — an object that may be
    /// on the store but is not in the log — and the call throws. Strays are
    /// deleted before this engine next writes or truncates the log, which is
    /// what keeps a later, shorter log from ending up adjacent to an old stray
    /// that a restart would then read back as part of it; and it is what keeps
    /// a create-only re-PUT of the same index from being refused as a second
    /// writer. A crash before that cleanup is covered by the load path, which
    /// keeps only the chain of log objects from the lowest one
    /// (`walk_log_chain`).
    ///
    /// A refused precondition latches, exactly as in `append_log_entry`, and
    /// nothing is cleaned up after it: a latched engine does not write.
    ///
    /// A run that does not extend the log — not consecutive, or not entirely
    /// above the last index — is written one entry at a time on this thread,
    /// with the mirror again updated only after the last PUT. `node` never
    /// sends one.
    ///
    /// Under `log_layout::segmented` the run is split greedily into segments
    /// (`max_segment_entries`, `max_segment_bytes`), one PUT each, the same
    /// workers and the same all-or-nothing contract per run; a failure records
    /// every object it sent as a stray by its key. A run that does not continue
    /// the log at its last index + 1 is refused with `std::invalid_argument`
    /// before any request.
    auto append_log_entries(std::span<const log_entry_t> entries) -> void {
        if (entries.empty()) {
            return;
        }
        std::lock_guard lock(_mu);
        throw_if_fenced();

        const bool extends = extends_log(entries);
        if (segmented_log() &&
            !(extends && (_log.empty() || entries.front().index() == _log.rbegin()->first + 1))) {
            // Before any request (Requirement 6.3 and the design's append path):
            // a run that does not continue the log would put singletons inside
            // or beside segments the mirror holds. `node` never sends one.
            throw std::invalid_argument(
                "object_store_persistence_engine: under log_layout::segmented a batched append "
                "must continue the log at its last index + 1");
        }

        std::vector<std::string> records;
        records.reserve(entries.size());
        for (const auto& entry : entries) {
            records.push_back(entry_to_json(entry));
        }
        // One object per entry, or under `segmented` the run's chunks.
        std::vector<std::pair<std::size_t, std::size_t>> chunks;
        if (segmented_log()) {
            chunks = chunk_run(entries.front().index(), records);
        } else {
            chunks.reserve(entries.size());
            for (std::size_t i = 0; i < entries.size(); ++i) {
                chunks.emplace_back(i, i + 1);
            }
        }

        std::vector<std::string> keys;
        std::vector<std::string> bodies;
        keys.reserve(chunks.size());
        bodies.reserve(chunks.size());
        for (const auto& [begin, end] : chunks) {
            keys.push_back(log_key(entries[begin].index()));
            bodies.push_back(
                log_object_body(entries[begin].index(),
                                std::span<const std::string>(records).subspan(begin, end - begin)));
            // Before anything is sent, so an oversized entry fails the run
            // without leaving the rest of it behind.
            check_object_size(keys.back(), bodies.back());
        }

        // The keep-set is the objects this run writes, by key (each key's
        // first index), not the entry indices it covers: a stray is spared only
        // when the PUT will overwrite that very object (Requirement 6.5).
        std::set<LogIndex> written;
        for (const auto& chunk : chunks) {
            written.insert(entries[chunk.first].index());
        }
        clear_strays(written);

        const std::size_t workers =
            extends && concurrent_key_object_store<Store> ? _opts.append_concurrency : 1;
        auto outcomes = put_log_objects(keys, bodies, workers);

        const put_outcome* first_failure = nullptr;
        for (const auto& outcome : outcomes) {
            if (outcome.state == put_state::precondition_failed) {
                latch(outcome.key, {}, outcome.detail);
            }
            if (outcome.state == put_state::failed && first_failure == nullptr) {
                first_failure = &outcome;
            }
        }
        if (first_failure != nullptr) {
            std::size_t sent = 0;
            for (std::size_t i = 0; i < outcomes.size(); ++i) {
                // An index the log still holds is not a stray: deleting it
                // would remove an entry the mirror says is there. Only a run
                // that does not extend the log can reach one, and it is left
                // as a failed `append_log_entry` would leave it.
                const LogIndex first = entries[chunks[i].first].index();
                if (outcomes[i].state != put_state::not_sent && !_log.contains(first)) {
                    _strays.insert(first);
                    ++sent;
                }
            }
            throw std::runtime_error(
                "object_store_persistence_engine: append of " + std::to_string(entries.size()) +
                " log entries failed (" + std::to_string(sent) +
                " objects sent, none recorded in the log; they are deleted before the next log "
                "write): " +
                first_failure->detail);
        }
        for (const auto& entry : entries) {
            _log[entry.index()] = entry;
        }
        _write_seq += entries.size();
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            const auto& [begin, end] = chunks[i];
            if (end - begin > 1) {
                _segments[entries[begin].index()] =
                    segment_meta{entries[end - 1].index(), std::move(outcomes[i].version)};
            }
        }
    }

    /// @brief Log indices that may have an object on the store but are not in
    ///        the log: what a failed `append_log_entries` sent, and what the load
    ///        path found past a gap. Empty again after the next log write.
    [[nodiscard]] auto stray_log_indices() const -> std::vector<LogIndex> {
        std::lock_guard lock(_mu);
        return {_strays.begin(), _strays.end()};
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
    ///
    /// Highest index first, so that whatever a crash part-way through leaves is
    /// still a contiguous log rather than one with a hole in it: the load path
    /// keeps only the contiguous run, and deleting from the bottom up would hand
    /// it a hole with the entries this call meant to remove sitting above it.
    /// Strays go first of all, for the same reason.
    ///
    /// Under `log_layout::segmented` the unit is the object: every object
    /// starting at or above `index` is deleted, highest first, and then a
    /// segment straddling `index` is overwritten **in place** with its entries
    /// below `index` (a singleton when one remains) — `If-Match` its version
    /// under `compare_and_swap`. That overwrite is the straddler's commit
    /// point: every earlier request leaves a chain that is a prefix of the
    /// pre-call log no shorter than the post-call one (Requirement 7).
    auto truncate_log(LogIndex index) -> void {
        std::lock_guard lock(_mu);
        throw_if_fenced();
        clear_strays({});
        while (!_log.empty() && object_of(_log.rbegin()->first).first >= index) {
            const LogIndex first = object_of(_log.rbegin()->first).first;
            delete_object(log_key(first));
            _log.erase(_log.lower_bound(first), _log.end());
            _segments.erase(first);
        }
        if (_log.empty() || _log.rbegin()->first < index) {
            return;
        }
        // A segment [first..last] with first < index <= last.
        const LogIndex first = object_of(index).first;
        const std::string key = log_key(first);
        const auto records = log_records(first, index - 1);
        const std::string body = log_object_body(first, records);
        object_version version;
        if constexpr (Fencing == fencing_mode::compare_and_swap) {
            version =
                conditional_put(key, body, precondition{if_version{_segments.at(first).version}});
        } else {
            put_object(key, body);
        }
        _log.erase(_log.lower_bound(index), _log.end());
        if (records.size() > 1) {
            _segments[first] = segment_meta{index - 1, std::move(version)};
        } else {
            _segments.erase(first);
        }
    }

    /// @brief Delete every entry with index &lt; `index` (compaction after a
    ///        snapshot), with the same delete-then-forget ordering.
    ///
    /// Strays go first, as on every other log mutation. Without that a stray
    /// left past a gap becomes the lowest log object once the entries below it
    /// are deleted, and the next load would start the log there, at entries
    /// this node never acknowledged. No request changes unless a stray exists.
    ///
    /// Under `log_layout::segmented` every object ending below `index` is
    /// deleted, lowest first; then a segment `S` straddling `index` is split:
    /// its entries from `index` up are PUT to `log/<index>` (create-only under
    /// `compare_and_swap`), which is a stray until `S`'s DELETE — the commit
    /// point — is acknowledged. A crash before it reads the pre-compaction
    /// suffix, with the new object off the chain (Requirement 8).
    auto delete_log_entries_before(LogIndex index) -> void {
        std::lock_guard lock(_mu);
        throw_if_fenced();
        clear_strays({});
        while (!_log.empty()) {
            const auto [first, last] = object_of(_log.begin()->first);
            if (last >= index) {
                break;
            }
            delete_object(log_key(first));
            _log.erase(_log.begin(), _log.upper_bound(last));
            _segments.erase(first);
        }
        if (_log.empty() || _log.begin()->first >= index) {
            return;
        }
        // A segment [first..last] with first < index <= last.
        const auto [first, last] = object_of(_log.begin()->first);
        const auto records = log_records(index, last);
        const std::string key = log_key(index);
        const std::string body = log_object_body(index, records);
        // A stray from before its PUT is sent: if the PUT fails it may still
        // have landed, and until the straddler's DELETE it is off the chain.
        _strays.insert(index);
        object_version version;
        if constexpr (Fencing == fencing_mode::compare_and_swap) {
            version = conditional_put(key, body, precondition{if_absent{}});
        } else {
            put_object(key, body);
        }
        delete_object(log_key(first));
        _strays.erase(index);
        _log.erase(_log.begin(), _log.lower_bound(index));
        _segments.erase(first);
        if (records.size() > 1) {
            _segments[index] = segment_meta{last, std::move(version)};
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

    /// @brief Whether `<prefix>/voted_for` still holds a legacy value that this
    ///        engine owes a `retired` overwrite (hard_state_layout::combined).
    ///
    /// True from load until m2 lands on a legacy prefix that has a vote slot,
    /// and after a migrating write whose m2 failed transiently — the case
    /// `last_vote_slot_retirement_error()` explains. The state is safe either
    /// way: beside a combined `<prefix>/term` the vote slot is never read. What
    /// a pending retirement costs is detection, under `compare_and_swap`: a
    /// pre-migration writer that writes only the vote slot is not refused until
    /// the retirement lands.
    [[nodiscard]] auto vote_slot_retirement_pending() const -> bool {
        std::lock_guard lock(_mu);
        return _vote_slot_to_retire.has_value();
    }

    /// @brief Why the last attempt to retire `<prefix>/voted_for` failed, if
    ///        it did. Cleared when a retirement lands.
    ///
    /// The retirement runs after the migration's commit point and cannot fail
    /// the call that committed it, so — like `last_prune_error()` — this is
    /// where its failure becomes visible rather than silent.
    [[nodiscard]] auto last_vote_slot_retirement_error() const -> std::optional<std::string> {
        std::lock_guard lock(_mu);
        return _last_retirement_error;
    }

private:
    // ── Batched log writes ───────────────────────────────────────────────────

    enum class put_state : std::uint8_t {
        not_sent,
        ok,
        failed,
        precondition_failed
    };

    /// What happened to one PUT of a run. Written by exactly one worker and read
    /// only after every worker has joined.
    struct put_outcome {
        put_state state{put_state::not_sent};
        std::string key;
        std::string detail;
        /// The version the store returned, under `compare_and_swap` only: what
        /// a segment's later in-place overwrite is predicated on.
        object_version version;
    };

    [[nodiscard]] auto combined_hard_state() const -> bool {
        return _opts.hard_state == hard_state_layout::combined;
    }

    [[nodiscard]] auto segmented_log() const -> bool { return _opts.log == log_layout::segmented; }

    /// Requirement 5.6, at construction. Under `per_entry` a non-default bound
    /// is refused rather than ignored, the rule `owner_id` follows on an
    /// unfenced engine.
    auto validate_log_layout_options() const -> void {
        const object_persistence_options defaults;
        if (!segmented_log()) {
            if (_opts.max_segment_entries != defaults.max_segment_entries ||
                _opts.max_segment_bytes != defaults.max_segment_bytes) {
                throw std::invalid_argument(
                    "object_store_persistence_engine: max_segment_entries / max_segment_bytes are "
                    "set but the log layout is log_layout::per_entry, which writes no segment — "
                    "set log_layout::segmented rather than leaving them ignored");
            }
            return;
        }
        if (_opts.max_segment_entries < 2) {
            throw std::invalid_argument(
                "object_store_persistence_engine: max_segment_entries must be at least 2 — one "
                "entry is always written as a singleton");
        }
        if (_opts.max_segment_bytes == 0) {
            throw std::invalid_argument(
                "object_store_persistence_engine: max_segment_bytes must be at least 1");
        }
        if (_opts.max_segment_bytes > _opts.max_object_bytes) {
            throw std::invalid_argument(
                "object_store_persistence_engine: max_segment_bytes must not exceed "
                "max_object_bytes, which bounds every PUT");
        }
    }

    /// The first and last index of the log object holding `index`, which the
    /// log holds: the segment covering it, or the singleton at it. Callers hold
    /// `_mu`.
    [[nodiscard]] auto object_of(LogIndex index) const -> std::pair<LogIndex, LogIndex> {
        auto it = _segments.upper_bound(index);
        if (it != _segments.begin()) {
            --it;
            if (index <= it->second.last) {
                return {it->first, it->second.last};
            }
        }
        return {index, index};
    }

    /// The body of a log object holding `records` — already-encoded entries,
    /// first index `first` — as a singleton when there is one record and as a
    /// segment otherwise. Each element of a segment's `entries` is the record
    /// itself, so it is byte-identical to the singleton body for that entry.
    [[nodiscard]] static auto log_object_body(LogIndex first, std::span<const std::string> records)
        -> std::string {
        if (records.size() == 1) {
            return records.front();
        }
        const auto last = first + static_cast<LogIndex>(records.size() - 1);
        std::string body =
            "{\"first_index\":" + std::to_string(static_cast<unsigned long long>(first)) +
            ",\"last_index\":" + std::to_string(static_cast<unsigned long long>(last)) +
            ",\"entries\":[";
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (i > 0) {
                body += ',';
            }
            body += records[i];
        }
        body += "]}";
        return body;
    }

    /// The bodies of the entries of the log from `from` to `to`, inclusive.
    /// Callers hold `_mu`.
    [[nodiscard]] auto log_records(LogIndex from, LogIndex to) const -> std::vector<std::string> {
        std::vector<std::string> out;
        for (auto it = _log.lower_bound(from); it != _log.end() && it->first <= to; ++it) {
            out.push_back(entry_to_json(it->second));
        }
        return out;
    }

    /// Split an extending run's encoded `records` greedily into chunks, as
    /// `[begin, end)` positions: at most `max_segment_entries` records and
    /// `max_segment_bytes` of segment body each. A record larger than
    /// `max_segment_bytes` is a chunk of its own (a singleton, bounded by
    /// `max_object_bytes` alone), and so is any record no neighbour fits beside.
    [[nodiscard]] auto chunk_run(LogIndex first, const std::vector<std::string>& records) const
        -> std::vector<std::pair<std::size_t, std::size_t>> {
        const auto digits = [](std::size_t value) {
            return std::to_string(static_cast<unsigned long long>(value)).size();
        };
        // `{"first_index":` `,"last_index":` `,"entries":[` `]}`
        constexpr std::size_t k_segment_overhead = 15 + 14 + 12 + 2;
        std::vector<std::pair<std::size_t, std::size_t>> chunks;
        std::size_t begin = 0;
        while (begin < records.size()) {
            std::size_t end = begin + 1;
            std::size_t sum = records[begin].size();
            if (sum <= _opts.max_segment_bytes) {
                const std::size_t first_digits = digits(static_cast<std::size_t>(first) + begin);
                while (end < records.size() && end - begin < _opts.max_segment_entries) {
                    const std::size_t grown = sum + records[end].size();
                    const std::size_t body = k_segment_overhead + first_digits +
                                             digits(static_cast<std::size_t>(first) + end) + grown +
                                             (end - begin);
                    if (body > _opts.max_segment_bytes) {
                        break;
                    }
                    sum = grown;
                    ++end;
                }
            }
            chunks.emplace_back(begin, end);
            begin = end;
        }
        return chunks;
    }

    /// Every combined hard-state write: one PUT of `(term, vote)` to
    /// `<prefix>/term`, and the in-place migration when that object does not
    /// yet hold a combined record. Callers hold `_mu`.
    ///
    /// On a legacy prefix this PUT is m1: `put_single_slot` predicates it on
    /// the legacy `<prefix>/term`'s loaded version under `compare_and_swap`
    /// (create-only when there was none), which is exactly the migration's
    /// precondition — no create-only against an object that exists, and no
    /// unconditional overwrite of a second writer's term. It is the commit
    /// point, so the mirror moves as soon as it is acknowledged, and m2
    /// follows.
    ///
    /// **m1 strictly before m2.** Retiring the vote slot first would leave,
    /// after a crash between the two, a decimal `<prefix>/term` beside a vote
    /// slot that no longer holds the vote — an older binary would read "no
    /// vote" for a term this node voted in. So a retirement owed by a legacy
    /// prefix waits for m1, and only a retirement left pending *after* m1 is
    /// retried ahead of the next PUT.
    auto put_combined_hard_state(TermId term, std::optional<NodeId> vote) -> void {
        const bool migrating = !_term_holds_combined;
        if (!migrating) {
            retire_vote_slot_if_pending();
        }
        put_single_slot(term_key(), hard_state_to_json(term, vote));
        _current_term = term;
        _voted_for = std::move(vote);
        _term_holds_combined = true;
        if (migrating) {
            retire_vote_slot_if_pending();
        }
    }

    /// m2: PUT `retired` to `<prefix>/voted_for`, `If-Match` the version it was
    /// loaded at under `compare_and_swap`. Called only once `<prefix>/term`
    /// holds a combined record. Callers hold `_mu`.
    ///
    /// Retire rather than delete: the slot has to change *version*, so that a
    /// pre-migration writer still running against the prefix is refused at its
    /// next conditional write to it — and `If-Match` against an existing
    /// object's stale version is the cell every fencing provider has been
    /// verified on, where `If-Match` against a missing key is not.
    ///
    /// A refused precondition latches and throws: another writer has written
    /// the vote slot since this engine loaded it. Any other failure is
    /// recorded and leaves the retirement pending — the call that reached here
    /// has already committed, so failing it would report a durable write as
    /// lost. Under `compare_and_swap` a retirement whose response was lost
    /// after it landed is refused at its retry and latches, which is the
    /// header's lost-response residual applied to this one write.
    auto retire_vote_slot_if_pending() -> void {
        if (!_vote_slot_to_retire) {
            return;
        }
        const std::string key = voted_for_key();
        try {
            if constexpr (Fencing == fencing_mode::compare_and_swap) {
                _versions[key] = conditional_put(key, k_retired_vote,
                                                 precondition{if_version{*_vote_slot_to_retire}});
            } else {
                put_object(key, k_retired_vote);
            }
        } catch (const persistence_fenced_error&) {
            throw;
        } catch (const std::exception& e) {
            _last_retirement_error = "object_store_persistence_engine: retiring " + key +
                                     " failed after " + term_key() +
                                     " was committed; it is retried before the next hard-state "
                                     "write: " +
                                     e.what();
            return;
        }
        _vote_slot_to_retire.reset();
        _last_retirement_error.reset();
    }

    [[nodiscard]] static auto render_vote(const NodeId& node) -> std::string {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return node;
        } else {
            return std::to_string(static_cast<unsigned long long>(node));
        }
    }

    /// Whether `entries` is a consecutive ascending run lying entirely above the
    /// last index in the mirror — the only shape whose objects are all new, and
    /// so the only shape that may be sent concurrently and recorded as strays on
    /// failure without touching an object the log still holds. Callers hold `_mu`.
    [[nodiscard]] auto extends_log(std::span<const log_entry_t> entries) const -> bool {
        for (std::size_t i = 1; i < entries.size(); ++i) {
            if (entries[i].index() != entries[i - 1].index() + 1) {
                return false;
            }
        }
        return _log.empty() || entries.front().index() > _log.rbegin()->first;
    }

    /// One log-object PUT, reported rather than thrown, and touching no member
    /// but the immutable ones — the workers in `put_log_objects` call it
    /// without `_mu`'s protection being needed for anything it reads.
    auto put_log_object(const std::string& key, const std::string& body) const -> put_outcome {
        put_outcome out;
        out.key = key;
        try {
            fiu_do_on("raft/objstore/put_object",
                      throw std::runtime_error("chaos: raft/objstore/put_object " + key););
            if constexpr (Fencing == fencing_mode::compare_and_swap) {
                object_version version;
                try {
                    version =
                        _store.put_object_if(_bucket, key, body, precondition{if_absent{}}).version;
                } catch (const object_precondition_failed& e) {
                    out.state = put_state::precondition_failed;
                    out.detail = e.what();
                    return out;
                }
                verify_returned_digest(key, body, version);
                out.version = std::move(version);
            } else {
                put_with_retry(key, body);
            }
            out.state = put_state::ok;
        } catch (const std::exception& e) {
            out.state = put_state::failed;
            out.detail = e.what();
        }
        return out;
    }

    /// PUT every `(keys[i], bodies[i])` with up to `workers` in flight, and say
    /// what happened to each. Once one fails no further PUT is started — the run
    /// is going to throw, so sending the rest would only make more strays — but
    /// the ones already in flight finish. One worker is the sequential path, on
    /// the calling thread. Callers hold `_mu`; the workers never take it.
    auto put_log_objects(const std::vector<std::string>& keys,
                         const std::vector<std::string>& bodies, std::size_t workers) const
        -> std::vector<put_outcome> {
        std::vector<put_outcome> outcomes(keys.size());
        std::atomic<std::size_t> next{0};
        std::atomic<bool> failed{false};
        auto work = [&] {
            while (!failed.load()) {
                const std::size_t i = next.fetch_add(1);
                if (i >= keys.size()) {
                    return;
                }
                outcomes[i] = put_log_object(keys[i], bodies[i]);
                if (outcomes[i].state != put_state::ok) {
                    failed.store(true);
                }
            }
        };
        workers = std::min(workers, keys.size());
        if (workers <= 1) {
            work();
        } else {
            std::vector<std::jthread> pool;
            pool.reserve(workers - 1);
            for (std::size_t w = 1; w < workers; ++w) {
                pool.emplace_back(work);
            }
            work();
        }
        return outcomes;
    }

    /// DELETE every stray not in `keep`, highest first, forgetting each only once
    /// its DELETE is acknowledged. Throws on the first failure, which fails the
    /// log write that asked: writing on top of an undeleted stray is exactly what
    /// strays are tracked to prevent. Callers hold `_mu`.
    ///
    /// `keep` is the first indices of the objects about to be written — their
    /// keys, not the entry indices they cover. Under `fencing_mode::none` those
    /// objects are overwritten by the PUT anyway, so deleting them first would
    /// be a wasted request; under `compare_and_swap` the PUT is create-only and
    /// would be refused by its own stray, so nothing is kept.
    auto clear_strays(const std::set<LogIndex>& keep) -> void {
        std::vector<LogIndex> doomed;
        for (auto it = _strays.rbegin(); it != _strays.rend(); ++it) {
            if (Fencing == fencing_mode::compare_and_swap || !keep.contains(*it)) {
                doomed.push_back(*it);
            }
        }
        for (const LogIndex index : doomed) {
            delete_object(log_key(index));
            _strays.erase(index);
        }
        // Kept indices are about to be overwritten. If that write fails they are
        // recorded again; if it succeeds they are in the log.
        for (const LogIndex index : keep) {
            _strays.erase(index);
        }
    }

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
        put_with_retry(key, bytes);
    }

    /// The store call and retry loop of `put_object`, without the size check or
    /// the fault point, which `put_log_object` applies itself. Reads only
    /// immutable members, so it is safe on a worker thread.
    auto put_with_retry(const std::string& key, std::string_view bytes) const -> void {
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
        hard_state_slots slots;
        log_objects objects;

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
                    load_term_object(got->body, key, slots);
                }
            } else if (key == vote) {
                auto got = get_object(key);
                if (got) {
                    // The version is remembered whatever the body says: the
                    // `"none"` sentinel is an object that exists, so a fenced
                    // engine's first vote must be an If-Match and not a
                    // create-only.
                    remember_version(key, got->version);
                    load_vote_object(got->body, slots);
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
                objects[index] = load_log_object(got->body, key, index, std::move(got->version));
            }
            // Any other key under the prefix belongs to something else (an
            // operator's note, a future format's object). It is not this
            // engine's state, so it is neither read nor written — only keys
            // this format defines are parsed, and those must parse.
        }
        resolve_hard_state(slots);
        walk_log_chain(std::move(objects));
    }

    /// What the load path found in `<prefix>/term` and `<prefix>/voted_for`,
    /// held until the listing loop ends so that how the two are read together
    /// does not depend on the order the store lists them in.
    struct hard_state_slots {
        /// A decimal `<prefix>/term`, parsed.
        std::optional<TermId> legacy_term;
        /// A combined `<prefix>/term`, parsed.
        std::optional<std::pair<TermId, std::optional<NodeId>>> combined;
        /// `<prefix>/voted_for` holds a legacy value — a vote, `none`, or
        /// empty — unparsed: beside a combined `<prefix>/term` it is not state
        /// and is not read, whatever it holds (Requirement 3.1).
        std::optional<std::string> legacy_vote_body;
        /// `<prefix>/voted_for` holds `retired`.
        bool vote_retired{false};
    };

    /// Parse `<prefix>/term`, by its first byte: `{` is the combined record,
    /// anything else goes to `parse_unsigned` exactly as it always has. A
    /// `separate` engine refuses a combined record here, whatever the vote slot
    /// says, so the refusal cannot depend on listing order either.
    auto load_term_object(const std::string& body, const std::string& key,
                          hard_state_slots& slots) const -> void {
        if (!body.starts_with('{')) {
            slots.legacy_term = static_cast<TermId>(parse_unsigned(body, key));
            return;
        }
        if (!combined_hard_state()) {
            throw std::runtime_error(
                "object_store_persistence_engine: " + key +
                " holds a combined hard-state record, but this engine is configured "
                "hard_state_layout::separate. An in-place downgrade is not supported: set "
                "hard_state_layout::combined, or restore this prefix from a backup taken before "
                "it was upgraded — reading the legacy vote slot beside this record would serve a "
                "stale vote");
        }
        slots.combined = wrap_parse<std::pair<TermId, std::optional<NodeId>>>(
            key, [&body] { return json_to_hard_state(body); });
    }

    /// Note `<prefix>/voted_for` for `resolve_hard_state`. Nothing is parsed
    /// here: the `retired` marker is valid only beside a combined
    /// `<prefix>/term`, and a legacy value is read only beside a legacy one,
    /// and which of those it is cannot be known until both slots are read.
    static auto load_vote_object(const std::string& body, hard_state_slots& slots) -> void {
        if (body == k_retired_vote) {
            slots.vote_retired = true;
            return;
        }
        slots.legacy_vote_body = body;
    }

    /// A legacy vote slot's value: a node id, or no vote for `none` (or an
    /// empty body), exactly as the load path has always read it.
    static auto parse_legacy_vote(const std::string& body, const std::string& key)
        -> std::optional<NodeId> {
        if (body == "none" || body.empty()) {
            return std::nullopt;
        }
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return body;
        } else {
            return static_cast<NodeId>(parse_unsigned(body, key));
        }
    }

    /// The load precedence of `.kiro/specs/object-store-batched-layout/`
    /// design Part 2, after the listing loop:
    ///
    ///   * `retired` beside anything but a combined `term` is corruption —
    ///     it is written only after the combined record;
    ///   * a combined `term` (reachable only under `combined`: a `separate`
    ///     engine refused it in `load_term_object`) is the pair, and a legacy
    ///     vote slot beside it is ignored and owed a retirement — the state a
    ///     crash between m1 and m2 leaves;
    ///   * otherwise the legacy pair, exactly as today. Under `combined` that
    ///     is a legacy prefix: nothing is written for it here, its first
    ///     hard-state write migrates it, and a vote slot it holds is retired
    ///     after that write's commit point.
    auto resolve_hard_state(const hard_state_slots& slots) -> void {
        if (slots.vote_retired && !slots.combined) {
            throw std::runtime_error("object_store_persistence_engine: corrupt object " +
                                     voted_for_key() + ": \"retired\" is written only after " +
                                     term_key() + " holds a combined hard-state record, but " +
                                     term_key() +
                                     (slots.legacy_term ? " holds a decimal term" : " is absent"));
        }
        if (slots.combined) {
            _current_term = slots.combined->first;
            _voted_for = slots.combined->second;
            _term_holds_combined = true;
        } else {
            if (slots.legacy_term) {
                _current_term = *slots.legacy_term;
            }
            if (slots.legacy_vote_body) {
                _voted_for = parse_legacy_vote(*slots.legacy_vote_body, voted_for_key());
            }
        }
        if (combined_hard_state() && slots.legacy_vote_body) {
            _vote_slot_to_retire = tracked_version(voted_for_key());
        }
    }

    /// The version the load path remembered for `key`; empty under
    /// `fencing_mode::none`, which tracks none.
    [[nodiscard]] auto tracked_version(const std::string& key) const -> object_version {
        auto it = _versions.find(key);
        return it == _versions.end() ? object_version{} : it->second;
    }

    /// The log objects the load path found, by key: each one's first index and
    /// the entries it holds, in index order. Every object is a singleton today;
    /// the walk below reads an object only by its first and last index, so it
    /// is the same walk whatever an object holds.
    struct loaded_log_object {
        std::vector<log_entry_t> entries;
        /// As read; kept for a segment under `compare_and_swap` only.
        object_version version;
    };
    using log_objects = std::map<LogIndex, loaded_log_object>;

    /// Parse one `<prefix>/log/<index>` body: a singleton, or under
    /// `log_layout::segmented` a segment. A segment under `per_entry` is the
    /// downgrade refusal, not corruption — the bucket is fine, the setting is
    /// older than it (Requirement 11.3).
    auto load_log_object(const std::string& body, const std::string& key, LogIndex index,
                         object_version version) const -> loaded_log_object {
        const auto parsed =
            wrap_parse<boost::json::value>(key, [&body] { return boost::json::parse(body); });
        loaded_log_object out;
        const boost::json::object* obj = parsed.if_object();
        if (object_store_persistence_detail::is_segment(parsed)) {
            if (!segmented_log()) {
                throw std::runtime_error(
                    "object_store_persistence_engine: " + key +
                    " holds a log segment, but this engine is configured log_layout::per_entry. "
                    "An in-place downgrade is not supported: set log_layout::segmented, or "
                    "restore this prefix from a backup taken before it was upgraded — skipping "
                    "the segment would load a log missing the entries it holds");
            }
            out.entries = wrap_parse<std::vector<log_entry_t>>(
                key, [obj, index] { return object_to_segment(*obj, index); });
            if constexpr (Fencing == fencing_mode::compare_and_swap) {
                out.version = std::move(version);
            }
            return out;
        }
        auto entry =
            wrap_parse<log_entry_t>(key, [&parsed] { return object_to_entry(parsed.as_object()); });
        if (entry.index() != index) {
            // The key *is* the index; a record that disagrees with its own key
            // means the log's ordering guarantee no longer holds, which is
            // exactly the silent-loss case this engine exists to prevent.
            throw std::runtime_error(
                "object_store_persistence_engine: corrupt object " + key +
                ": record carries index " +
                std::to_string(static_cast<unsigned long long>(entry.index())) +
                " but its key names index " +
                std::to_string(static_cast<unsigned long long>(index)));
        }
        out.entries.push_back(std::move(entry));
        return out;
    }

    /// Recover the log as the **chain** of objects from the lowest first index:
    /// start at the lowest object, take it, continue at the object whose first
    /// index is its last + 1, and stop at the first position nothing starts
    /// at. Everything not on the chain is recorded as a stray.
    ///
    /// A gap is what a crash leaves part-way through a concurrent
    /// `append_log_entries` (any subset of the run may have landed) or part-way
    /// through a truncation written before truncations went highest-first. In
    /// both cases the objects past the gap are entries this node never
    /// acknowledged or was in the middle of removing, so dropping them is safe,
    /// and loading them is not: `node` keeps its log as a vector and indexes it
    /// by position, so a hole silently shifts every entry above it. They are
    /// not deleted here — construction writes nothing — but before the next
    /// log write, like any other stray.
    ///
    /// Over singletons this is exactly the contiguous run from the lowest
    /// index, which is what the load path kept before the walk replaced it
    /// (`.kiro/specs/object-store-batched-layout/` task 6); the walk is the
    /// form that stays correct once an object can hold more than one entry and
    /// another object can begin inside it.
    auto walk_log_chain(log_objects objects) -> void {
        if (objects.empty()) {
            return;
        }
        LogIndex next = objects.begin()->first;
        for (auto& [first, object] : objects) {
            if (first != next) {
                _strays.insert(first);
                continue;
            }
            const std::size_t count = object.entries.size();
            for (auto& entry : object.entries) {
                _log[entry.index()] = std::move(entry);
            }
            next = first + static_cast<LogIndex>(count);
            if (count > 1) {
                _segments[first] = segment_meta{next - 1, std::move(object.version)};
            }
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
    ///
    /// **Keep it strict for a second reason.** Every binary that has shipped
    /// refuses a combined `<prefix>/term` because this parse refuses anything
    /// but digits; that refusal is the whole rollback guard of the combined
    /// hard state (`.kiro/specs/object-store-batched-layout/` design Property
    /// 3). A "lenient" version — leading digits, or skipping a `{` — would turn
    /// that refusal into a silent stale read for every binary built after it.
    /// `tests/legacy_layout_reader.hpp` freezes the rule as shipped.
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

    // ── Combined hard state (hard_state_layout::combined) ─────────────────────

    /// The vote slot's body once a migrated prefix has retired it. Never read
    /// as a vote: the load path checks for it before parsing one.
    static constexpr std::string_view k_retired_vote =
        object_store_persistence_detail::k_retired_vote;

    /// `{"term":<uint64>,"voted_for":<NodeId>|null}`, members in that order.
    static auto hard_state_to_json(TermId term, const std::optional<NodeId>& vote) -> std::string {
        boost::json::object obj;
        obj["term"] = static_cast<std::uint64_t>(term);
        if (!vote) {
            obj["voted_for"] = nullptr;
        } else if constexpr (std::is_same_v<NodeId, std::string>) {
            obj["voted_for"] = boost::json::string(*vote);
        } else {
            obj["voted_for"] = static_cast<std::uint64_t>(*vote);
        }
        return boost::json::serialize(obj);
    }

    /// Strict, deliberately: the record's shape through the shared
    /// `object_store_persistence_detail::parse_combined_hard_state`, then
    /// `voted_for` `null` or the `NodeId`'s own JSON type. Anything else — a
    /// missing or extra member, a float, a string term — throws, which is what
    /// makes this binary refuse a *future* format the way older binaries
    /// refuse this one.
    static auto json_to_hard_state(const std::string& s)
        -> std::pair<TermId, std::optional<NodeId>> {
        const auto record = object_store_persistence_detail::parse_combined_hard_state(s);
        std::pair<TermId, std::optional<NodeId>> out;
        out.first = static_cast<TermId>(record.term);
        const boost::json::value& vote = record.voted_for;
        if (vote.is_null()) {
            return out;
        }
        if constexpr (std::is_same_v<NodeId, std::string>) {
            if (!vote.is_string()) {
                throw std::runtime_error(
                    "the combined hard-state record's voted_for is neither null nor a string");
            }
            out.second = std::string(vote.as_string());
        } else {
            if (!vote.is_int64() && !vote.is_uint64()) {
                throw std::runtime_error(
                    "the combined hard-state record's voted_for is neither null nor an integer");
            }
            out.second = vote.to_number<NodeId>();
        }
        return out;
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
        return object_to_entry(boost::json::parse(s).as_object());
    }

    static auto object_to_entry(const boost::json::object& obj) -> log_entry_t {
        log_entry_t e;
        e._term = static_cast<TermId>(obj.at("term").as_int64());
        e._index = static_cast<LogIndex>(obj.at("index").as_int64());
        e._command = base64_to_bytes(std::string(obj.at("command").as_string()));
        e._type = obj.contains("type") ? static_cast<entry_type>(obj.at("type").as_int64())
                                       : entry_type::normal;
        return e;
    }

    /// A segment record (Requirement 5.5): the shape by the shared
    /// `parse_segment`, then each element decoded as a singleton's record is.
    static auto object_to_segment(const boost::json::object& obj, LogIndex key_index)
        -> std::vector<log_entry_t> {
        (void)object_store_persistence_detail::parse_segment(obj,
                                                             static_cast<std::uint64_t>(key_index));
        const boost::json::array& elements = obj.at("entries").as_array();
        std::vector<log_entry_t> out;
        out.reserve(elements.size());
        for (const auto& element : elements) {
            out.push_back(object_to_entry(element.as_object()));
        }
        return out;
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
                snap._configuration._nodes.push_back(n.to_number<NodeId>());
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
                    old_nodes.push_back(n.to_number<NodeId>());
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
    /// One per entry appended, by any append call; the barrier concept's
    /// sequence. Every one is durable when it is assigned.
    write_sequence _write_seq{0};
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

    /// Log indices that may have an object on the store and are not in `_log`:
    /// sent by a failed `append_log_entries`, or found past a gap at load.
    /// Deleted before the next log write, truncation or compaction
    /// (`clear_strays`).
    std::set<LogIndex> _strays;

    /// What a segment on the chain covers, and — under `compare_and_swap` only —
    /// the version it was last written or read at, which truncation's in-place
    /// overwrite is `If-Match`ed on.
    struct segment_meta {
        LogIndex last{0};
        object_version version;
    };

    /// The segments on the chain, by first index (`log_layout::segmented`). A
    /// log index no segment covers is a singleton, so singletons need no entry
    /// here, and under `per_entry` this stays empty: the default engine's
    /// memory is unchanged. No version is kept for a singleton, which is never
    /// conditionally rewritten (Requirement 10.2).
    std::map<LogIndex, segment_meta> _segments;

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

    // ── Migration to the combined hard state (hard_state_layout::combined) ────

    /// Whether `<prefix>/term` holds a combined record: loaded as one, or
    /// written by this engine. While false, the next hard-state write is the
    /// migration's m1 (or, on a prefix with no hard state, the record's
    /// creation). Never set under `separate`.
    bool _term_holds_combined{false};

    /// A pending retirement of `<prefix>/voted_for` — m2 — with the version it
    /// was loaded at (empty under `fencing_mode::none`). Set by the load path
    /// on a `combined` engine whenever the vote slot holds a legacy value;
    /// sent only once `_term_holds_combined`.
    std::optional<object_version> _vote_slot_to_retire;

    /// Why the last retirement attempt failed; see
    /// `last_vote_slot_retirement_error()`.
    std::optional<std::string> _last_retirement_error;
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
    [[nodiscard]] auto put_object(const std::string&, const std::string&, std::string_view) const
        -> put_result;
    [[nodiscard]] auto get_object(const std::string&, const std::string&) const
        -> std::optional<get_result>;
    auto delete_object(const std::string&, const std::string&) const -> void;
    [[nodiscard]] auto list_keys(const std::string&, const std::string&) const
        -> std::vector<std::string>;
    [[nodiscard]] auto provider_name() const -> std::string_view;
};

static_assert(key_object_store<concept_check_store>);
static_assert(!conditional_key_object_store<concept_check_store>,
              "the unconditional check store must stay unconditional — it is what pins that "
              "fencing_mode::compare_and_swap is unavailable for such a store");

/// The same, refined: enough to check that the fenced engine compiles at file
/// scope, again without naming a provider.
struct conditional_concept_check_store : concept_check_store {
    [[nodiscard]] auto put_object_if(const std::string&, const std::string&, std::string_view,
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

/// …and the barrier extension, so `node` takes its barriered path for this
/// engine and a group-scoped wrapper forwards it
/// (`.kiro/specs/object-store-group-commit/` Requirement 1.3).
static_assert(
    barriered_persistence_engine<
        object_store_persistence_engine<object_store_persistence_detail::concept_check_store>>,
    "object_store_persistence_engine must satisfy barriered_persistence_engine");
static_assert(
    bulk_append_persistence_engine<
        object_store_persistence_engine<object_store_persistence_detail::concept_check_store>>,
    "object_store_persistence_engine must keep bulk_append_persistence_engine, "
    "which node uses for a synchronous engine's follower runs");

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
