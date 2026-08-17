# Requirements Document

## Introduction

This spec makes durable Raft state in a cloud key-object store a
**cross-provider capability** rather than one provider's accident. It has
two halves, and they are separable in review but not in intent:

1. **Breadth** — a `kythira::persistence_engine` backed by each implemented
   cloud provider's key-object store: AWS S3, Azure Blob Storage, GCP Cloud
   Storage, OCI Object Storage, and the Alibaba OSS engine that already
   ships. Five engines, but **one implementation**: the engine body becomes
   generic over a small `kythira::key_object_store` concept and each
   provider contributes only a client.
2. **Depth** — the four questions `.kiro/specs/alibaba-cloud-services/`
   deliberately refused to answer alone, because answering them for one
   provider would have fixed a cross-provider format by accident
   (Requirements 15.7 and 15.8 there): **snapshot retention**, **backup
   catalogs**, **restore-into-a-fresh-cluster**, and **conditional-PUT
   fencing**. This spec is where they are settled, once, for all five.

The position this spec starts from is unusually good and should be used:
`alibaba_oss_persistence_engine` (`include/raft/alibaba_oss_persistence.hpp`)
is **live-verified end to end** against a real OSS bucket (August 14, 2026 —
all four real cases, including a fresh engine reading back another engine's
writes). Its layout, its in-memory mirror, its synchronous-write contract,
its parse-or-throw load path and its single idempotent-PUT retry are not
proposals; they are a working reference with a live run behind them. The
generic engine this spec specifies is that engine's body, hoisted — not a
redesign.

### The three facts this spec must confront head-on, not paper over

**Durability.** Raft §5 requires `currentTerm` and `votedFor` to reach stable
storage before the node acts on them. Each is a network PUT here. The
existing engine's argument — a 2xx PUT response means multi-replica durable,
which is *stronger* than `file_persistence_engine`'s unfsynced
write-then-rename — must be restated per provider with a per-provider
citation, because it is the load-bearing claim of the whole design and it is
currently documentation-level for four of the five.

**Latency.** The measured figure is **~2-3 s per object round trip**, from a
developer machine to `ap-southeast-1` (spike-notes.md Finding 7: ~9.6 s for
the term+vote case, ~34 s for 12 appended entries). That number is
**geography, not OSS** — it is an upper bound taken across an ocean, and the
in-region number, which is the one that matters, has never been measured.
This spec requires measuring it (Requirement 4.5) rather than quoting the
WAN figure as if it were representative. Either way the shape of the problem
is fixed: `save_current_term` + `save_voted_for` put **two sequential round
trips on the election hot path** and every `append_log_entry` puts one on the
commit path.

**Cost.** Object stores bill per request, and this engine's write pattern is
one request per log entry per node. At S3's order-of-magnitude PUT price a
sustained 100 entries/s costs on the order of **$1,000+/month per node** in
request charges alone, before any storage or egress. This is not a footnote:
it defines what the engine is *for* (low-write-rate control-plane clusters
where instance loss must not lose state) and what it is emphatically not for
(high-throughput replicated logs). Requirement 6 makes stating this envelope
a deliverable, not a README afterthought.

### What is deliberately not being invented

There is no vendor-neutral object-store abstraction library here, no
pluggable storage framework, and no attempt to make the five clients
identical below the concept. Each provider contributes the smallest client
that satisfies `key_object_store`, in whatever shape that provider's
existing footprint in this repo already established: SDK-backed where the SDK
is already a dependency and its retry policy is explicitly disableable,
hand-rolled over httplib where it is not. That decision is per provider,
recorded with its reasoning in Requirements 12–15, and refutable by Task 0.

## Glossary

- **Key-object store**: a flat key→bytes store with per-key atomic
  whole-object writes and no rename, no append, and no cross-key
  transaction. S3, Azure Blob, GCS, OCI Object Storage and Alibaba OSS are
  all this shape; the differences that matter to us are authentication,
  conditional-write spelling, and listing pagination.
- **Prefix**: the operator-supplied key namespace one engine instance owns,
  e.g. `raft/node-3`. Exactly one process may own a `{bucket, prefix}` pair
  (Requirement 9).
- **ETag / generation**: the per-object version token a conditional write is
  predicated on. S3/Azure/OCI/OSS express it as an HTTP ETag; GCS expresses
  it as a numeric `generation` with `x-goog-if-generation-match`. The
  concept abstracts both as an opaque `object_version` string.
- **CAS write**: a PUT or DELETE predicated on the target key's current
  version — `If-Match: <etag>` for overwrite, `If-None-Match: *` (or
  `x-goog-if-generation-match: 0`, or `x-oss-forbid-overwrite: true`) for
  create-only. The precondition is evaluated by the *service*, which is what
  makes it a fence rather than a check.
- **Fenced**: the state an engine enters when a CAS write fails its
  precondition — proof that another writer has touched this prefix. It is
  terminal (Requirement 9.5).
- **Clone restore / seed restore**: the two restore modes (Requirement 11).
  A clone reproduces one node's state byte for byte, identity included; a
  seed builds a *new* cluster's initial state from a backup's snapshot,
  deliberately discarding term, vote, log and the old membership.
- **Conformance suite**: one test body, written once against the
  `key_object_store` concept and the generic engine, instantiated for every
  store (Requirement 17.2). It is the mechanism that makes "five engines"
  cost roughly one engine's worth of test review.

## Requirements

### Requirement 1: The `kythira::key_object_store` Concept

**User Story:** As a developer, I want one small concept describing what the
persistence engine needs from an object store, so that adding a provider
means writing a client, not another engine.

#### Acceptance Criteria

1. `include/raft/key_object_store.hpp` SHALL define
   `template<typename S> concept key_object_store` requiring, on a
   `const S&`:

   | Operation | Signature | Semantics |
   |---|---|---|
   | put | `put_object(bucket, key, std::string_view bytes) -> put_result` | Whole-object overwrite; returns the stored object's version and never partially writes |
   | get | `get_object(bucket, key) -> std::optional<get_result>` | `nullopt` for absent — absence is an answer, not an error |
   | delete | `delete_object(bucket, key) -> void` | Idempotent: deleting an absent key succeeds |
   | list | `list_keys(bucket, prefix) -> std::vector<std::string>` | Fully paginated; a listing that cannot be completed throws rather than truncating |
   | name | `provider_name() -> std::string_view` | `"s3"`, `"azure-blob"`, `"gcs"`, `"oci-objectstorage"`, `"oss"` — used in error messages, metrics and the backup manifest |

2. `put_result` SHALL carry the stored `object_version` (ETag or
   generation, as an opaque string) and `get_result` SHALL carry both the
   body and the `object_version` it was read at. A store that cannot report
   a version SHALL report an empty one, which disqualifies it from
   Requirement 9's CAS fencing mode and nothing else.
3. A separate refinement `template<typename S> concept
   conditional_key_object_store` SHALL additionally require
   `put_object_if(bucket, key, bytes, precondition)` and
   `delete_object_if(bucket, key, precondition)`, where `precondition` is
   either "the object does not exist" or "the object is exactly at version
   V", and SHALL require that a failed precondition is reported as a
   **distinguishable** `object_precondition_failed` exception type rather
   than a generic runtime error — the engine's fencing behaviour depends on
   telling "you lost the race" apart from "the network broke".
4. The concept SHALL NOT require multipart upload, copy, server-side
   encryption configuration, bucket creation, lifecycle policy management,
   tagging, or versioning. Buckets are operator-owned prerequisites in every
   sibling provider spec and remain so here.
5. Every client SHALL be **retry-free at the client layer** for mutating
   operations, or SHALL expose an explicit knob set to zero retries, because
   the engine owns write retries and its idempotency argument (Requirement
   4.4) depends on knowing exactly what was re-sent. Where a vendor SDK
   retries by default, disabling that policy SHALL be an acceptance criterion
   of that client's requirement, not a configuration suggestion.
6. `alibaba_oss_client` SHALL satisfy `key_object_store` **without
   behavioural change** — additively, by having `put_object` return the
   response ETag instead of `void` and `get_object` return a `get_result`.
   Its signing, addressing, pagination and error handling are unchanged and
   its existing tests SHALL pass unmodified except where they name those two
   return types.

---

### Requirement 2: `object_store_persistence_engine` — One Engine, Five Stores

**User Story:** As a maintainer, I want a single engine implementation
generic over the store, so that a durability bug is fixed once and a
correctness property is proved once.

#### Acceptance Criteria

1. `include/raft/object_store_persistence.hpp` SHALL define
   `template<key_object_store Store, typename NodeId = std::uint64_t,
   typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
   class object_store_persistence_engine` satisfying
   `kythira::persistence_engine` (persistence.hpp:26), with a file-scope
   `static_assert` per instantiation the header itself provides for the
   default parameters.
2. The engine body SHALL be `alibaba_oss_persistence_engine`'s current body,
   hoisted — the same in-memory mirror loaded once at construction, the same
   synchronous mutation path, the same parse-or-throw load, the same
   `<prefix>/term`, `<prefix>/voted_for`, `<prefix>/log/<20-digit index>`,
   `<prefix>/snapshot` layout, the same 20-digit padding rationale, the same
   JSON codec byte-compatible with `file_persistence_engine`. Requirement 8
   (retention) is the **only** layout change this spec makes, and it is
   additive.
3. Construction SHALL take `{Store, bucket, prefix, object_persistence_options}`
   with the same empty-field validation and trailing-slash normalisation as
   today.
4. `object_persistence_options` SHALL be a plain aggregate carrying, with
   **every default preserving today's shipped behaviour exactly**:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `snapshot_retention` | `std::size_t` | `1` | Retained snapshot copies (Requirement 8). `1` = today's single slot, and writes no `snapshots/` objects at all |
   | `fencing` | `fencing_mode` | `none` | `none` \| `compare_and_swap` (Requirement 9.2) |
   | `owner_id` | `std::string` | `""` | Identity written into `<prefix>/owner` under `compare_and_swap`; empty is rejected in that mode |
   | `takeover_epoch` | `std::optional<std::uint64_t>` | `nullopt` | Explicit, auditable takeover of a prefix owned by another writer (Requirement 9.6) |
   | `verify_checksums` | `bool` | `true` | End-to-end content checksum on every PUT (Requirement 7.1) |
   | `max_object_bytes` | `std::size_t` | Task 0-chosen | Single-PUT size cap; a snapshot exceeding it throws (Requirement 7.3) |
   | `write_retries` | `unsigned` | `1` | Same-call retries, PUT-only, as shipped (Requirement 4.4) |

   Validation SHALL throw `std::invalid_argument` naming the offending
   field: `snapshot_retention == 0`, `fencing == compare_and_swap` with an
   empty `owner_id`, and `max_object_bytes == 0`.
5. libfiu fault points SHALL be named
   `raft/objstore/{put,get,delete,list}_object` at the store boundary —
   provider-independent, because the engine is. The existing
   `raft/alibaba/oss/*` names SHALL be retained as additional fault points on
   the Alibaba client itself so no existing chaos configuration breaks.
6. The engine SHALL NOT log, retry, or otherwise behave differently by
   provider. Anything provider-specific belongs in the client or the
   options struct.

---

### Requirement 3: `alibaba_oss_persistence_engine` Becomes an Instantiation

**User Story:** As a maintainer, I want the shipped Alibaba engine to keep
its name, header and tests while gaining the shared body, so that a working,
live-verified component is not put at risk by a refactor.

#### Acceptance Criteria

1. `include/raft/alibaba_oss_persistence.hpp` SHALL continue to exist and to
   define `alibaba_oss_persistence_engine<NodeId, TermId, LogIndex>` — as a
   **type alias** for `object_store_persistence_engine<alibaba_oss_client,
   ...>`, preserving the constructor signature
   `{alibaba_client_config, bucket, prefix}` via a factory or a thin derived
   type if an alias alone cannot preserve it.
2. Every existing Alibaba test binary (`alibaba_oss_persistence_unit_test`,
   `alibaba_oss_persistence_mock_test`, `alibaba_oss_persistence_real_test`)
   SHALL pass **unmodified** apart from mechanical changes forced by
   Requirement 1.6's return types. A test change that alters an assertion is
   a signal the hoist changed behaviour and SHALL be treated as a defect in
   the hoist.
3. The header's substantial documentation — the durability argument, the
   one-object-per-entry rationale, the 20-digit padding rationale, the
   corruption-is-fatal contrast with the file engine — SHALL move to the
   generic header rather than being deleted or duplicated, with the Alibaba
   header retaining only what is OSS-specific plus a pointer.
4. The Alibaba engine's live-verified status SHALL be preserved as a
   statement about the *generic* engine over the OSS client, and the real
   suite SHALL be re-run live after the hoist (Requirement 18.5). A hoist
   that is not re-verified against the one provider we can actually reach
   discards the spec's single strongest piece of evidence.

---

### Requirement 4: The Durability Contract, Per Provider

**User Story:** As an operator, I want to know exactly what "the write
returned" means on my provider, so that I can decide whether this engine is
safe for my deployment.

#### Acceptance Criteria

1. Every mutating method SHALL issue its PUT or DELETE synchronously on the
   calling thread and return only after the store has answered 2xx. There
   SHALL be no write buffer, no background flusher, and no asynchronous path
   anywhere in the engine — the call stack of `save_current_term` SHALL
   contain the socket write and the socket read of the response. This is the
   existing contract and it is non-negotiable.
2. The design document SHALL carry a **per-provider durability evidence
   table** with one row per store: what the vendor documents a successful
   write response to mean (replication scope, storage class caveats), the
   citation, and — honestly — whether that claim is *documentation-derived*
   or *live-verified in this repo*. As of this spec's writing exactly one
   row is live-verified (OSS, August 14, 2026).
3. WHERE a provider's default storage class or a configurable durability
   knob would weaken that claim (e.g. reduced-redundancy classes,
   zone-scoped buckets), the requirement SHALL name it and the operator
   documentation SHALL state which settings the engine assumes.
4. The single same-call retry SHALL remain **PUT-only**, justified as today
   by every PUT being a deterministic full-object overwrite at a key derived
   from the value being written. Under `fencing_mode::compare_and_swap` the
   retry SHALL re-send the *same precondition*, so a retry that lands after
   another writer intervened fails the precondition rather than silently
   overwriting (Requirement 9.4).
5. **The in-region latency number SHALL be measured, not estimated.** The
   real tier SHALL record per-operation latency for `save_current_term`,
   `append_log_entry` and `save_snapshot`, and the operator documentation
   SHALL quote the measured figure with its measurement position stated
   (in-region instance vs. out-of-region host). The existing ~2-3 s/round-trip
   figure SHALL be reproduced with its provenance intact — a cross-ocean
   upper bound — and SHALL NOT be quoted as a production number.
6. The documentation SHALL state the derived operational rule plainly:
   election timeouts MUST exceed several times the measured per-PUT latency,
   because a candidate performs two sequential durable writes before sending
   its first RequestVote and a follower performs the same before replying.
7. **Rejected alternative, recorded:** combining term and vote into one
   `hard_state` object — which would halve the election-path round trips and
   make the pair atomic — SHALL NOT be done in this spec, because
   `kythira::persistence_engine` exposes `save_current_term` and
   `save_voted_for` as separate calls and the engine cannot know a second
   call is coming; buffering the first to coalesce them would violate 4.1.
   Widening the concept with `save_hard_state(term, vote)` is recorded here
   as a candidate follow-on for the raft layer, with this spec as its
   motivation.

---

### Requirement 5: Consistency Model, and What the Engine Actually Needs

**User Story:** As a reviewer, I want the consistency argument stated in
terms of the engine's actual access pattern, so that a provider's marketing
term is not mistaken for a proof.

#### Acceptance Criteria

1. The design SHALL state the engine's access pattern exactly: **reads
   happen once, at construction** (one List plus one GET per live object);
   every subsequent read answers from the in-memory mirror. Writes are
   whole-object PUTs and DELETEs at deterministic keys. There is no
   read-modify-write on the hot path and no cross-key atomicity requirement
   except the snapshot commit (Requirement 8.4).
2. From that pattern the design SHALL derive the two properties the engine
   genuinely depends on, and name them separately because providers document
   them separately:
   - **Read-after-write across process lifetimes**: a fresh engine
     constructed after a crash must observe the last acknowledged write of
     its predecessor.
   - **List-after-write**: that same fresh engine's List must enumerate every
     acknowledged log-entry object and omit every acknowledged deletion. A
     store with a lagging index would silently shorten a recovered log,
     which is the exact failure `file_persistence_engine`'s silent
     `catch (...)` tolerance was criticised for.
3. A per-provider consistency table SHALL record both properties, with
   citations and with `documentation-derived` / `live-verified` status per
   cell, exactly as Requirement 4.2 does for durability.
4. WHERE a provider's listing is eventually consistent in any documented
   circumstance, that provider's engine SHALL NOT be marked production-ready
   in the documentation, and the design SHALL state what a
   listing-independent recovery would cost (a densely-scanned index range,
   or a manifest object) rather than leaving the reader to invent it.
5. The real tier SHALL, for every provider, run the load-bearing case the
   Alibaba tier already runs live: **a fresh engine sharing no memory with
   the writer reads back term, vote, a multi-entry log and a snapshot**.
   That case is what turns a documentation citation into evidence.

---

### Requirement 6: The Operating Envelope — Cost and Throughput, Stated Up Front

**User Story:** As an operator, I want to know before I deploy this what it
costs and what it can sustain, so that I do not discover the answer on an
invoice.

#### Acceptance Criteria

1. The design and the operator documentation SHALL both state the request
   model plainly: **one PUT per log entry per node**, plus one PUT per term
   change, one per vote, one per snapshot, and one DELETE per compacted or
   truncated entry. Reads cost one List plus N GETs per process start and
   nothing thereafter.
2. From that model the documentation SHALL carry a worked cost example per
   provider at, at minimum, 1 entry/s and 100 entries/s sustained on a
   3-node cluster, using each provider's published per-request price with
   the price and its retrieval date named. The conclusion — that sustained
   high append rates are financially prohibitive in this engine — SHALL be
   stated as a conclusion, not left as an exercise.
3. The documentation SHALL state the throughput ceiling as a latency
   consequence: with no batching in `kythira::persistence_engine`'s API,
   sustained append throughput per node cannot exceed roughly one entry per
   store round trip. The measured latency from Requirement 4.5 SHALL be used
   to give that ceiling a number.
4. The documentation SHALL name the deployment shape this engine is for —
   low-write-rate clusters (membership/configuration/coordination state)
   where surviving instance loss matters more than throughput — and the shape
   it is not for, without hedging.
5. **Log-entry batching is a non-goal of this spec** and SHALL be recorded as
   such with its reason: it requires widening `kythira::persistence_engine`
   with a batch-append operation, which is a raft-layer change affecting
   every engine, not an object-store change.

---

### Requirement 7: Object Integrity and Size Limits

**User Story:** As an operator, I want a corrupted upload rejected by the
service rather than discovered at recovery, so that a bad network path
cannot become silent state loss.

#### Acceptance Criteria

1. WHEN `verify_checksums` is enabled (the default) THEN every PUT SHALL
   carry an end-to-end content checksum the service verifies and rejects on
   mismatch — `Content-MD5` where universally supported, or the provider's
   native checksum header where one is preferred. The per-provider spelling
   SHALL be recorded in the design's client table.
2. WHERE a provider returns a checksum or ETag that is a deterministic
   function of the content for single-part uploads, the client SHALL verify
   the returned value against the locally computed one and throw on
   mismatch, naming the key. **Measured, August 16-17, 2026: the ETag is the
   content MD5 on only two of the five providers** — S3 (lowercase) and OSS
   (uppercase), so that comparison must be case-insensitive. Azure's ETag is
   an opaque timestamp token, OCI's is a **UUID** and GCS's is an opaque
   token, so on those three the locally verifiable value is the returned
   `Content-MD5` / `opc-content-md5` / `md5Hash` metadata field instead — all
   three confirmed to match the content. A client that assumed the S3 shape
   would compare an opaque token against an MD5 and fail every write.
   The **service-side** check of criterion 1 is available on all five, in five
   different spellings: `BadDigest` (S3), `InvalidDigest` (OSS),
   `Md5Mismatch` (Azure), `invalid` (GCS), `UnmatchedContentMD5` (OCI).
3. `max_object_bytes` SHALL cap the size of any single PUT, defaulting to a
   conservative value below every provider's documented single-request
   limit (Task 0 confirms the limits; the default SHALL be chosen from the
   smallest). A `save_snapshot` whose serialised size exceeds the cap SHALL
   throw with a message naming the size, the cap, and the fact that
   multipart upload is a documented non-goal — a snapshot silently truncated
   at a provider limit is the worst outcome available here.
4. **Multipart/chunked upload is out of scope**, recorded with its reason:
   it multiplies each provider's client surface and its failure modes for a
   case (a snapshot larger than the cap) no current state machine in this
   repo produces. The error in 7.3 is the honest handling until one does.

---

### Requirement 8: Snapshot Retention

**User Story:** As an operator, I want more than one snapshot retained, so
that a snapshot written from corrupted in-memory state does not immediately
destroy the only recoverable one.

#### Acceptance Criteria

1. The retention layout SHALL be **additive to the shipped format**:
   `<prefix>/snapshot` remains exactly what it is today — the single current
   snapshot object, same key, same codec, same single-GET recovery path —
   and retained predecessors accumulate under
   `<prefix>/snapshots/<20-digit last_included_index>` using the same codec
   and the same padding rationale as log keys.
2. `save_snapshot` SHALL, in this order: (a) PUT the retained copy at
   `<prefix>/snapshots/<index>`, (b) PUT `<prefix>/snapshot` — **the commit
   point**, (c) prune retained copies beyond `snapshot_retention`, oldest
   first. A failure at (a) leaves the previous state fully intact; a failure
   at (b) leaves an unreferenced retained copy, which is inert; a failure at
   (c) leaves extra copies, which costs storage and nothing else. No
   ordering of these three failures can lose the current snapshot.
   **Steps (a) and (b) SHALL throw on failure and step (c) SHALL NOT**
   (settled in implementation, August 17, 2026): (c) runs after the commit
   point, so failing the call there would report a lost snapshot that is not
   lost, and would abandon `node::install_snapshot`'s term write and RPC
   reply over a garbage-collection error. It SHALL instead record the failure
   where an operator can read it (`last_prune_error()`), keep the
   undeleted index in its retained set, and retry it on the next
   `save_snapshot` — observable and self-healing, which is the property
   "costs storage and nothing else" actually requires.
3. `snapshot_retention` SHALL count snapshot **generations including the live
   `<prefix>/snapshot`**, and `snapshot_retention = 1` SHALL mean today's
   behaviour exactly: no `<prefix>/snapshots/` objects are written at all, so
   a default-configured engine's bucket is byte-identical to what ships
   today — **and its request pattern is too**: one PUT per `save_snapshot`,
   no LIST and no DELETE. `0` SHALL be rejected at construction rather than
   read as "keep none", which would delete the retained copy of the snapshot
   just written.
4. Recovery SHALL read `<prefix>/snapshot` and nothing else. Retained copies
   are for operators and for Requirement 11's restore, and are never
   consulted by the engine's own load path — which keeps the recovery path a
   single GET and keeps a corrupt retained copy from being able to break
   startup.
5. WHEN pruning encounters a retained copy whose key does not parse as a
   20-digit index THEN it SHALL be left alone rather than deleted. The
   engine deletes only what it can prove it wrote. Such a key SHALL also
   **not** fail construction — deliberately the opposite of the same shape
   under `<prefix>/log/`, because a log key that cannot be ordered breaks
   recovery whereas a retained copy is never read by the load path at all.
6. Retention SHALL be documented as **not** a backup: retained snapshots
   live in the same bucket, under the same prefix, subject to the same
   credentials and the same accidental `rm -r`. Requirement 10 is the
   backup story; conflating them is how an operator discovers at 3am that
   their "backups" shared a blast radius with the thing that failed.

---

### Requirement 9: Fencing — Detecting a Second Writer

**User Story:** As an operator, I want a node that has lost ownership of its
prefix to fail loudly instead of corrupting state, so that a failed failover
or a duplicated deployment cannot silently destroy a Raft log.

#### Acceptance Criteria

1. The single-writer requirement stands: exactly one process owns a
   `{bucket, prefix}` pair. What this requirement adds is **detection**, not
   coordination. The engine still does not implement leases, does not
   arbitrate, and does not attempt to take over from another writer.
2. `fencing_mode` SHALL offer exactly two values:
   - `none` (default) — today's behaviour, unchanged, with no extra request
     cost and no conditional headers. Documented as single-writer-by-assertion.
   - `compare_and_swap` — the writes that can carry a precondition **at no
     extra round trip** do carry one, and the mode requires
     `conditional_key_object_store` (Requirement 1.3) at compile time.
3. Under `compare_and_swap` the engine SHALL:
   - track the `object_version` returned by each successful PUT, per key, in
     the in-memory mirror;
   - write `<prefix>/term` and `<prefix>/voted_for` with `If-Match: <tracked
     version>` (create-only on first write). These two are the **safety
     chokepoint**: no second writer can produce a double vote, a second
     leader or a diverging committed log without first writing one of them,
     because a candidate persists an incremented term and a self-vote before
     sending any RequestVote and a follower persists a term bump before
     replying;
   - write each `<prefix>/log/<index>` object with the **create-only**
     precondition. This costs nothing — it is a header on a PUT already
     being sent — and it closes the one corruption path the chokepoint
     argument alone does not: a **stale leader appends without changing its
     term**, so it can interleave log objects with the rightful owner's
     indefinitely while never touching `term` or `voted_for`. Legitimate
     re-use of an index is always preceded by `truncate_log`'s DELETE, so
     create-only never rejects a legal write;
   - write `<prefix>/snapshot`, `<prefix>/snapshots/<index>` and every
     DELETE **unconditionally**, and the design SHALL state this as a
     **bounded residual rather than a covered case**: a second writer whose
     only interaction with the prefix is a snapshot overwrite or a
     truncation is not detected. Claiming a total fence here would be the
     more dangerous error.
4. WHEN a conditional write fails its precondition THEN the engine SHALL
   raise a distinguished `persistence_fenced_error` naming the key, the
   expected version, and the provider — and SHALL NOT retry it. A
   precondition failure is not a transient error; retrying it is exactly the
   overwrite the fence exists to prevent.
5. WHEN a `persistence_fenced_error` occurs THEN the engine SHALL **latch
   permanently**: every subsequent mutating call SHALL throw the same error
   without contacting the store, and the latch SHALL NOT be clearable at
   runtime. A node that has provably lost ownership of its state cannot
   safely continue participating in consensus, and an engine that lets it
   try is worse than one that has no fencing at all.
6. An **owner object** SHALL be written at construction under
   `compare_and_swap`: `<prefix>/owner`, create-only, carrying the owning
   node's identity, a startup timestamp and a monotonically increasing
   `epoch` supplied by the operator or read-incremented from the existing
   object. Construction over a prefix whose owner object names a different
   owner SHALL throw unless `takeover_epoch` is set higher than the recorded
   epoch — which makes takeover an explicit, auditable operator act rather
   than a race.
7. The per-provider conditional-write support matrix SHALL be recorded in
   the design with `documentation-derived` / `spike-verified` status per
   cell — create-only precondition, If-Match overwrite, conditional delete,
   and the exact header spelling — and Task 0 SHALL verify each cell against
   the live service. A fence that is documented but untested is a fence
   nobody should rely on.
8. WHERE a provider cannot express a needed precondition, that provider's
   client SHALL fail to satisfy `conditional_key_object_store` and
   `compare_and_swap` SHALL be a **compile-time** unavailable option for it,
   not a runtime degradation. Silently degrading a fence to a no-op is the
   one behaviour this requirement exists to forbid.
9. The fencing conformance tests SHALL include a genuine two-engine race
   against every mock store and, in the real tier, against every real store:
   engine A writes, engine B constructed over the same prefix writes, and
   A's next write SHALL fail with `persistence_fenced_error` and SHALL leave
   A latched.

---

### Requirement 10: Backup Catalog

**User Story:** As an operator, I want a point-in-time copy of a node's Raft
state outside its live prefix, with a manifest I can inspect, so that
recovery does not depend on the live prefix being intact.

#### Acceptance Criteria

1. `include/raft/object_store_backup.hpp` SHALL define
   `template<key_object_store Store> class object_store_backup` with
   `create(source, destination, options) -> backup_manifest`,
   `list(destination) -> std::vector<backup_manifest>`, and
   `verify(destination, backup_id) -> verification_report`.
2. A backup SHALL be a **copy of objects plus a manifest**, written under
   `<backup_prefix>/<backup_id>/`, where `backup_id` is a
   caller-supplied-or-timestamp-derived identifier that sorts
   chronologically. The manifest SHALL be the last object written, making
   its presence the backup's commit point — a torn backup is one without a
   manifest, and `list` SHALL ignore those.
3. `backup_manifest` SHALL be JSON carrying at least: format version,
   `provider_name()`, source bucket and prefix, backup id, creation
   timestamp (UTC, ISO 8601), the source's term and voted-for, the log index
   range, the snapshot's `last_included_index`/`last_included_term`, the
   cluster configuration recorded in that snapshot, and one entry per copied
   object with its key, byte size and checksum.
4. `create` SHALL operate against a **quiesced or accepted-fuzzy** source and
   SHALL say which: because there is no cross-key atomicity, a backup taken
   from a running node is a smear across the copy window. The manifest SHALL
   record whether the source was declared quiesced by the caller, and
   `verify` SHALL check internal consistency (every log index between the
   snapshot's `last_included_index` and the recorded last index is present,
   checksums match, the manifest's term is ≥ every log entry's term) so that
   a smeared backup is *detectable* rather than merely warned about.
5. Backups SHALL be writable to a **different bucket** from the source, and
   the documentation SHALL recommend exactly that, with the blast-radius
   reasoning from Requirement 8.6 spelled out — including cross-account or
   cross-project destinations where the provider supports it.
6. A CLI SHALL expose create/list/verify/restore: `cmd/raft_object_backup`,
   following the existing `cmd/` binary conventions, with the provider
   selected by a `--provider` flag and credentials taken from the same
   configuration the engines use. Operators do not have a C++ compiler in a
   recovery window.
7. The backup path SHALL NOT be part of the engine. `object_store_backup`
   takes a store and a prefix; it never instantiates
   `object_store_persistence_engine` and cannot be invoked from the Raft hot
   path.

---

### Requirement 11: Restore

**User Story:** As an operator, I want two clearly-separated restore modes,
so that "move this node's state to a new instance" and "start a new cluster
from this backup" cannot be confused for each other.

#### Acceptance Criteria

1. **Clone restore** SHALL reproduce a backup into a target prefix byte for
   byte — term, voted-for, every log entry, the snapshot, retained snapshots
   if present. It is for replacing the storage or the instance under an
   otherwise-unchanged node identity, and the restored state is safe to
   start **only if the original node is definitively gone**. The
   documentation SHALL state that starting both is a split-brain, and under
   `compare_and_swap` the owner object (Requirement 9.6) SHALL be restored
   with a *higher* epoch so the original, if it returns, fences itself out
   on its next write.
2. **Seed restore** SHALL build the initial state of a **new** cluster from
   a backup's snapshot: the snapshot's state-machine bytes are preserved,
   its configuration is **replaced** by an operator-supplied node set, term
   is reset to the snapshot's `last_included_term`, the vote is cleared, and
   the log is empty. It SHALL refuse to run if the backup has no snapshot,
   because there is nothing to seed from.
3. Seed restore SHALL write **one prefix per new node**, all seeded from the
   same snapshot with the same configuration, and SHALL emit exactly what
   the operator must configure (node ids, prefixes, and the fact that the
   new cluster's identities are unrelated to the old one's).
4. Both modes SHALL refuse to write into a non-empty target prefix unless an
   explicit `--force` is given, and `--force` SHALL delete the target's
   existing engine-owned keys before restoring rather than merging into
   them. A merge of two nodes' Raft state is not a recoverable state and the
   tool SHALL NOT be able to produce one.
5. Restore SHALL verify the manifest (Requirement 10.4) **before** writing
   anything, and SHALL abort on the first inconsistency, naming it.
6. The documentation SHALL carry a runbook for each mode with the exact
   command sequence, the safety checks, and the failure modes — the
   restore-into-fresh-cluster flow being the one that today "does not exist
   at all" (doc/TODO.md).

---

### Requirement 12: `aws_s3_client`

**User Story:** As an operator on AWS, I want Raft state in S3 using the SDK
already in this build, so that no new dependency and no hand-rolled SigV4
appear for a well-covered case.

#### Acceptance Criteria

1. `include/raft/aws_s3_client.hpp` SHALL define `aws_s3_client` satisfying
   `key_object_store` and `conditional_key_object_store`, built on
   `Aws::S3::S3Client`, gated by `KYTHIRA_HAS_AWS_SDK`. **No vcpkg.json
   change is required**: the `s3` feature is already listed on the
   `aws-sdk-cpp` dependency and is currently unused.
2. The client SHALL construct its `Aws::Client::ClientConfiguration` with an
   explicitly **zero-retry** retry strategy, per Requirement 1.5, and the
   header SHALL record why: the engine's single retry is the only retry, and
   an SDK retry underneath it makes the idempotency argument unverifiable.
3. It SHALL honour `aws_client_config`'s `region`, `endpoint_override` (for
   a local mock or LocalStack), `api_timeout` and credentials provider chain
   — the same struct every other AWS component takes, with no S3-specific
   parallel config.
4. Conditional writes SHALL use `If-None-Match: *` for create-only and
   `If-Match: <etag>` for overwrite, mapping S3's precondition-failure
   status to `object_precondition_failed`. Task 0 SHALL verify both against
   live S3, including which status code each produces.
5. Listing SHALL page through `ListObjectsV2` continuation tokens to
   completion and SHALL throw rather than return a truncated result.

---

### Requirement 13: `azure_blob_client`

**User Story:** As an operator on Azure, I want Raft state in Blob Storage
without adding a storage SDK to a build that already carries three Azure
packages.

#### Acceptance Criteria

1. `include/raft/azure_blob_client.hpp` SHALL define `azure_blob_client`
   satisfying `key_object_store` and `conditional_key_object_store`, speaking
   the Blob REST API over httplib with `Authorization: Bearer <token>` from
   `azure_client_config`'s existing `TokenCredential` chain — **no
   `azure-storage-blobs-cpp` dependency**.
2. The decision in 13.1 SHALL be recorded with its reasoning and marked
   refutable by Task 0: the Blob surface needed here is four operations,
   AAD bearer tokens are what CI federation already produces (so no
   SharedKey signing is written at all), and adding a storage SDK to the
   default dependency set costs every build. IF Task 0 finds the REST
   surface materially harder than documented — container-level auth quirks,
   an undocumented required header, listing pagination surprises — THEN
   `azure-storage-blobs-cpp` is the recorded fallback.
3. The client SHALL send `x-ms-version` (pinned to a named, dated API
   version) and `x-ms-blob-type: BlockBlob` on PUT, and the header SHALL
   record that an unpinned version is how a working client breaks on a
   service update.
4. Conditional writes SHALL use standard `If-Match` / `If-None-Match: *`
   semantics, which Blob supports natively. **CORRECTED against the live
   service, August 17, 2026 (spike-notes.md Finding 11): Azure answers two
   different statuses for the two preconditions** — **409
   `BlobAlreadyExists`** for `If-None-Match: *` and **412 `ConditionNotMet`**
   for `If-Match` — so the client SHALL map **both** to
   `object_precondition_failed`. Mapping only 412, as this criterion
   originally said, would let a create-only collision — the very case that
   catches a stale leader appending log entries — pass as an ordinary error.
   The 409 SHALL be distinguished by its error code and never by its status
   alone: S3's 409 `ConditionalRequestConflict` means the opposite (a benign
   race to retry). The client SHALL also treat **201 on PUT and 202 on
   DELETE** as success, and read the machine-readable error code from the
   `x-ms-error-code` response header.
5. Listing SHALL use `List Blobs` with the prefix parameter and follow
   `NextMarker` to completion, parsing the XML with the same bounded
   element-scanning approach `alibaba_oss_client` uses rather than adding an
   XML dependency.
6. The account/container/endpoint SHALL be configurable, including an
   `endpoint_override` for a local mock and for Azurite, and the design
   SHALL state whether Azurite is used as a mock tier or whether a
   hand-written mock server is (the OCI/Alibaba precedent).

---

### Requirement 14: `gcp_gcs_client`

**User Story:** As an operator on GCP, I want Raft state in Cloud Storage,
consistent with how every other GCP component in this repo is built.

#### Acceptance Criteria

1. `include/raft/gcp_gcs_client.hpp` SHALL define `gcp_gcs_client` satisfying
   `key_object_store` and `conditional_key_object_store`, built on
   `google-cloud-cpp`'s `storage` component, gated by a new
   `KYTHIRA_HAS_GCP_STORAGE`.
2. `vcpkg.json`'s existing **opt-in `gcp` feature** SHALL gain the `storage`
   component, and Kconfig SHALL gain `GCP_STORAGE` alongside `GCP_SDK` and
   `GCP_PRIVATECA`, found independently for the same reason those two are.
   GCS persistence is therefore unavailable in a default build, exactly like
   the GCP quorum managers — consistent, and honest about the compute
   component's build cost being why that feature is opt-in at all.
3. The client SHALL disable the storage client's automatic retry policy
   (`LimitedErrorCountRetryPolicy(0)` or equivalent) per Requirement 1.5.
4. Conditional writes SHALL use GCS **generation preconditions** —
   `IfGenerationMatch(0)` for create-only and `IfGenerationMatch(g)` for
   overwrite — with the numeric generation carried as the opaque
   `object_version`. The design SHALL note that GCS's generation model is
   strictly stronger than an ETag for this purpose and that the concept's
   opaque-string version is what lets both fit one interface.
5. It SHALL honour `gcp_client_config`'s `project_id`, `credentials_json`
   (ADC when empty), `endpoint_override` and `api_timeout`, with no
   GCS-specific parallel config.
6. The hand-rolled-REST alternative (JSON API over httplib with an
   ADC/metadata bearer token, which would work in a default build) SHALL be
   recorded as the considered-and-rejected option with its trade-off — it
   avoids the opt-in feature but requires implementing service-account JWT
   signing — so a future maintainer sees the fork rather than re-deriving it.

---

### Requirement 15: `oci_object_storage_client`

**User Story:** As an operator on OCI, I want Raft state in Object Storage,
reusing the signing and HTTP layer this repo already has.

#### Acceptance Criteria

1. `include/raft/oci_object_storage_client.hpp` SHALL define
   `oci_object_storage_client` satisfying `key_object_store` and
   `conditional_key_object_store`, reusing `oci_signing` and
   `oci_client_config` — including **Instance Principal** auth, which is
   what a node running on an OCI instance will actually use.
2. `oci_http_client` currently returns a parsed `boost::json::value` from
   every request, which cannot carry object bytes. A **raw-bytes request
   path** SHALL be added — returning status, headers and body unparsed —
   without changing the existing JSON-returning `request()`'s behaviour or
   any existing caller. The header SHALL record that object storage is a
   data-plane API and that parsing every response as JSON was a control-plane
   assumption.
3. Keys SHALL be addressed as `/n/<namespace>/b/<bucket>/o/<key>` on the
   `objectstorage` regional endpoint, with the namespace taken from
   configuration or resolved once via the namespace API and cached. The
   design SHALL record the per-service endpoint lesson `oci_http_client`
   already carries: the domain is per service and is not derivable from one
   template.
4. Conditional writes SHALL use OCI's `if-match` / `if-none-match` headers,
   mapping 412 to `object_precondition_failed`. **Verified live, August 17,
   2026** (Finding 13): create-only rejects **412 `IfNoneMatchFailed`**,
   `if-match` rejects **412 `IfMatchFailed`**, and the **conditional DELETE
   works** — a stale `if-match` is refused and the object survives. One
   signing consequence the same run produced: `oci_signing.hpp` signs
   `content-type: application/json` as a constant because every existing
   caller is a control-plane API, so the raw-bytes path SHALL make the signed
   content type a **parameter** — signing `application/json` while sending
   `application/octet-stream` yields a 401 that says nothing about content
   types.
5. Listing SHALL use `ListObjects` with `prefix` and follow the `nextStartWith`
   pagination cursor to completion.
6. The existing `kythira-ci-artifacts` bucket (Requirement 4.4 heartbeat
   work; `scripts/ci-cloud-credentials/oci/README.md`) SHALL be reused for
   the real tier rather than provisioning a second one, with the tests
   confined to their own prefix.

---

### Requirement 16: Build Integration

**User Story:** As a maintainer, I want each provider's engine
independently gateable, so that a build without an SDK, or an operator
without a provider, is unaffected.

#### Acceptance Criteria

1. Kconfig SHALL gain, in the existing per-provider menus:
   `AWS_S3_PERSISTENCE` (depends on `AWS_SDK`), `AZURE_BLOB_PERSISTENCE`
   (depends on `HTTP_TRANSPORT_TLS`, mirroring the OCI/Alibaba
   no-SDK-to-find shape), `GCP_STORAGE` + `GCP_STORAGE_PERSISTENCE`
   (depends on the SDK component), and `OCI_OBJECT_PERSISTENCE` (depends on
   `HTTP_TRANSPORT_TLS`). `ALIBABA_OSS_PERSISTENCE` is unchanged.
2. Each symbol's help text SHALL follow the existing menus' convention of
   saying whether it gates a `find_package` or only selects what is *built*,
   and SHALL NOT invent a `KYTHIRA_HAS_*` counterpart where nothing is being
   found.
3. `object_store_persistence.hpp`, `key_object_store.hpp` and
   `object_store_backup.hpp` SHALL compile unconditionally — they depend on
   no provider — so that a build with zero cloud providers still compiles
   the concept, the engine and the backup logic, and still runs the
   conformance suite against the in-memory mock store.
4. The `cmd/raft_object_backup` binary SHALL build with whichever providers
   are enabled and SHALL report the unavailable ones by name when asked for
   one that was not compiled in.
5. `DEPENDENCIES.md` SHALL be updated only where a dependency actually
   changes — the `storage` component on the opt-in `gcp` feature. The S3,
   Azure and OCI clients add nothing.

---

### Requirement 17: Tests

**User Story:** As a maintainer, I want one conformance suite proving every
store and every engine instantiation behaves identically, so that five
providers do not cost five reviews.

#### Acceptance Criteria

1. `tests/mock_object_store.hpp` SHALL provide an in-memory
   `key_object_store` + `conditional_key_object_store` implementation with
   injectable failures (fail the Nth PUT, fail a precondition, return a
   corrupt body, lag a listing) — the substrate for every test below that
   needs no network.
2. `tests/object_store_conformance.hpp` SHALL define the **shared
   conformance suite as a template**, instantiated per store: round-trip and
   reload-survival of every field, binary command bytes, the 20-digit
   padding boundary, truncation and compaction, corrupt-object → constructor
   throw, PUT-failure → mirror-unchanged (the existing Property 1 case),
   durability ordering (the PUT completed before the method returned,
   observable in the store's request log), retention behaviour at
   `snapshot_retention` 1/2/N, and the full fencing race from Requirement
   9.9. Instantiation for a new provider SHALL be a single line.
3. `tests/object_store_persistence_unit_test.cpp` SHALL run the conformance
   suite against `mock_object_store` and SHALL additionally cover the
   options struct's validation and the `persistence_fenced_error` latch.
4. Per-provider unit tests SHALL cover only what is provider-specific:
   request shape, conditional-header spelling, listing pagination,
   error-to-exception mapping, checksum handling, and endpoint/override
   derivation — against a local `httplib::Server` or the SDK's endpoint
   override, the established no-seam idiom.
5. Per-provider mock-server tests SHALL run the conformance suite against
   that provider's mock server, registered with CTest under labels
   `integration;<provider>;mock;object-persistence;cloud`. Where a provider's
   mock server already exists (Alibaba, OCI) it SHALL be extended, not
   duplicated, and SHALL keep **verifying signatures from the bytes that
   actually arrived** — the rule whose absence previously shipped two real
   defects.
6. Per-provider real tests SHALL be compiled under a
   `KYTHIRA_<PROVIDER>_REAL_TESTS` definition, **never CTest-registered**,
   exit-77 on missing configuration with each missing value named, with a
   read-only pre-flight whose failure skips rather than fails, and with the
   cost-reporting and signal-teardown fixtures from `oci_real_test_support.hpp`.
7. Every real suite SHALL include the four load-bearing cases: fresh-engine
   read-back (Requirement 5.5), measured per-operation latency (4.5), the
   fencing race (9.9), and a backup → verify → clone-restore → fresh-engine
   read-back round trip (10, 11).
8. A task requiring live verification SHALL NOT be checked off against the
   mock tier. As of this spec's writing, live cloud access exists for AWS,
   Azure, GCP, OCI and Alibaba accounts already provisioned for real-cloud
   tests; where a bucket does not yet exist, provisioning it is an explicit
   task, not an assumption.

---

### Requirement 18: CI Wiring

**User Story:** As a maintainer, I want each provider's object-persistence
suite to run in the existing real-cloud-tests workflow under the existing
federation, so that this capability is verified on the same cadence as
everything else.

#### Acceptance Criteria

1. `.github/workflows/real-cloud-tests.yml` SHALL gain a per-provider bundle
   toggle `REAL_CLOUD_TESTS_<PROVIDER>_OBJECT_PERSISTENCE_ENABLED` with a
   matching `workflow_dispatch` input, inside each provider's existing job
   rather than as new jobs — the credentials, federation steps and
   zero-bundle guards already exist there.
2. Each bundle SHALL fail loudly on exit 77: a real-cloud job that silently
   skips is indistinguishable from one that passes, which the existing jobs
   already guard against and this one SHALL not regress.
3. Bucket prerequisites SHALL be provisioned by the existing
   `scripts/ci-cloud-credentials/<provider>/` scripts, idempotently, with a
   least-privilege policy per provider scoped to the test bucket and prefix
   — object get/put/delete/list only, no bucket administration.
4. The OCI bundle SHALL reuse the existing `kythira-ci-artifacts` bucket
   (15.6 — the one `real-cloud-tests.yml` already uploads the heartbeat
   writer to), and the Alibaba bundle SHALL reuse whatever bucket the
   existing `ALIBABA_OSS_BUCKET` repository variable already names for
   `alibaba_oss_persistence_real_test`. Only AWS, Azure and GCP need a new
   bucket, and each provisioning script SHALL create it with public access
   blocked and a lifecycle rule expiring test prefixes.
5. **The Alibaba real suite SHALL be re-run live after the Requirement 3
   hoist**, and its result recorded in the spec's notes. This is the
   verification that the refactor preserved a live-verified component.
6. Costs SHALL be estimated in each provisioning README, including the fact
   that these suites' request counts are small enough to be rounding errors
   against storage minimums — the honest counterpart to Requirement 6's
   production cost warning.

---

### Requirement 19: Documentation

**User Story:** As an operator, I want one document that tells me whether to
use this, on which provider, at what cost, and how to recover.

#### Acceptance Criteria

1. `doc/cloud_object_persistence.md` SHALL be written, covering: the
   durability contract and its per-provider evidence table (4.2), the
   consistency table (5.3), the measured latency figures with their
   measurement positions (4.5), election-timeout sizing (4.6), the cost and
   throughput envelope with worked examples (6.2–6.4), the object layout
   including retention (8), fencing modes and the takeover procedure (9),
   and the backup/restore runbooks for both modes (10, 11).
2. Example configuration SHALL be provided per provider, in the same shape
   as the existing operator docs under `docker/*/README.md`.
3. The document SHALL state plainly which provider engines are
   live-verified and which are documentation-derived at time of writing, and
   SHALL be updated as that changes rather than being written aspirationally.
4. `doc/TODO.md`'s "Cloud key-object persistence engines — write the spec"
   entry SHALL be closed out with the spec's location and its verification
   status, and `CHANGELOG.md` SHALL record the capability.
5. `.kiro/specs/alibaba-cloud-services/` Requirements 15.7 and 15.8, which
   defer retention/backup/restore/fencing to this spec, SHALL be
   cross-referenced from here so the deferral chain is traceable in both
   directions.
