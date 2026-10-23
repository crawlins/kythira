// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file object_persistence_options.hpp
/// @brief `object_store_persistence_engine`'s runtime options and layout
///        enums, on their own.
///
/// Split out of `object_store_persistence.hpp` so a command-line parser can
/// hold the options without parsing the engine, and through it the future
/// and transport headers `persistence.hpp` brings in
/// (`.kiro/specs/multi-raft-node-s3-persistence/`, task 2). That header
/// includes this one, so nothing that already names these types changes.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace kythira {

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

    /// Leader group commit (`.kiro/specs/object-store-group-commit/`). Off —
    /// the default — every append is a PUT acknowledged before the call
    /// returns and the engine answers `durability_class::synchronous`, so
    /// `node` keeps its lock and its requests exactly as before. On, an append
    /// *stages* the entry with no request and `barrier_through` flushes every
    /// staged entry as one run (one PUT per segment under `segmented`, up to
    /// `append_concurrency` single-entry PUTs under `per_entry`); the engine
    /// answers `durability_class::barrier`, so `node` waits for that flush with
    /// its lock released and concurrent commands share it. Accepted under
    /// either log layout and either fencing mode.
    bool write_behind{false};
};

}  // namespace kythira
