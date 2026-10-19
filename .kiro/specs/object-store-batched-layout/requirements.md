# Requirements Document

## Introduction

`object_store_persistence_engine` (`include/raft/object_store_persistence.hpp`)
still writes **one object per log entry** (`<prefix>/log/<20-digit index>`) and
keeps `currentTerm` and `votedFor` in **two single-slot objects**
(`<prefix>/term`, `<prefix>/voted_for`) written by two ordered PUTs, term
first. `.kiro/specs/batched-durable-writes/` added the optional
`append_log_entries` and `save_hard_state` extensions and overlapped the PUTs of
one AppendEntries, but deliberately left the layout alone, so `doc/TODO.md`
(open-tasks item 15) still records: *one request per entry, and term and vote as
two ordered PUTs — still caps object-store append rate and adds election
latency; persistence concept change; spec first.*

This spec changes the **on-bucket format**, opt-in, in two independent parts:

1. a **combined hard state**, so that `save_hard_state(term, vote)` is one PUT
   of one object that is still the compare-and-swap chokepoint; and
2. a **segmented log**, so that one `append_log_entries` run is one PUT of one
   object covering a contiguous index range.

It is not a change to `kythira::persistence_engine` or to either optional
extension: every new behaviour is an engine option. The "concept change" the
TODO line names turned out to be a *format* change, and the one node-level
change this spec makes (Requirement 1) is to the order of calls `node` already
makes, not to their signatures.

### What reading the code turned up

Three findings shaped the requirements below; each is argued in the design.

1. **The documented election floor is understated by one PUT in the common
   case.** `doc/cloud_object_persistence.md` and the engine header give
   `election_timeout_min ≥ 4 × p99(PUT) + rpc_rtt`. Since batched-durable-writes,
   a follower that learns a newer term from a RequestVote first runs
   `become_follower()` (`include/raft/raft.hpp:5534`), which persists
   `(new term, no vote)` (`raft.hpp:6558-6562`), and then, granting, persists
   `(new term, candidate)` (`raft.hpp:5594`). On today's layout a follower that
   voted in the previous term — the normal case after a leader dies — pays
   **three** sequential PUTs (term, `"none"`, vote), so one election round is
   five, not four. The redundant write is a node-level artefact, and removing it
   (Requirement 1) helps every engine, not only the object store.
2. **The segmented log does not lift the cluster's append ceiling on its own.**
   A leader's own appends go through `submit_command` (`raft.hpp:2742`), which
   holds `_mutex` across `persist_append` → `append_log_entry` → one PUT
   (`raft.hpp:2808`). A cluster therefore commits at most about
   `1 / p50(PUT)` entries per second whatever the followers do, and a follower
   in steady state receives about as many entries per AppendEntries as the
   leader appended during one follower round trip — `multi-raft-performance`
   measured **1.03 entries per AppendEntries at one proposal in flight**. What
   the segmented log buys is follower request count (cost), follower catch-up
   time, and recovery GETs; lifting the ceiling needs leader group commit, which
   is out of scope here and named as such.
3. **A naive new key would make a rollback silently unsafe.** The engine's load
   path ignores keys it does not recognise (`object_store_persistence.hpp:1649`)
   — that is what lets operators keep notes under a prefix. A combined record
   written to a *new* key such as `<prefix>/hard_state` would therefore be
   invisible to an older binary, which would read the stale legacy `term` and
   `voted_for` and could grant a second vote in a term it had already voted in.
   The rollback guard has to live in a key older binaries **do** read and parse
   strictly. Both `<prefix>/term` (`parse_unsigned`, `:1794`, whole-body decimal
   or throw) and `<prefix>/log/<index>` (`json_to_entry`, `:1845`, requires a
   top-level `"term"`) qualify, and every version of the engine in this
   repository's history parses them that way.

### Scope

- `node`: one hard-state write per RequestVote (Requirement 1).
- `object_store_persistence_engine`: the two layouts, their migration rules and
  their crash-safety and fencing arguments (Requirements 2-11).
- `object_store_backup` and `cmd/raft_object_backup` (Requirement 12).
- The five provider clients: **no code change is expected**, and the
  requirement is to prove it (Requirement 13).
- The operating envelope in `doc/cloud_object_persistence.md` (Requirement 14).
- Tests, including crash injection at every request boundary, and the opt-in
  real-cloud runs (Requirements 15-16).

### Out of scope

- **Leader group commit** — coalescing concurrent `submit_command` calls into
  one `append_log_entries`. It is the change that would lift the cluster's
  append ceiling (finding 2), and it is a `node` concurrency change affecting
  every engine, so it gets its own spec.
- Persisting in parallel with sending (a candidate sending RequestVote while its
  own hard-state PUT is in flight; Ongaro §10.2.1's leader-side equivalent).
- Merging small existing objects into larger ones (a background compactor).
  Segments are only ever written whole, by the operation that produces them.
- Any change to `file_persistence_engine` or `memory_persistence_engine` beyond
  what Requirement 1 does to the calls they receive.
- Multipart upload. `max_object_bytes` and its rationale stand.

## Glossary

- **Legacy layout** — today's format: `<prefix>/term` (decimal text),
  `<prefix>/voted_for` (decimal text or `none`), one `<prefix>/log/<index>` per
  entry. It remains the default.
- **Combined hard state** — `<prefix>/term` holding a JSON record of both term
  and vote, e.g. `{"term":42,"voted_for":7}`.
- **Retired vote slot** — `<prefix>/voted_for` overwritten with the literal
  `retired` once a bucket has migrated to the combined hard state.
- **Log object** — any object under `<prefix>/log/`. Its key names its **first**
  index, 20-digit zero-padded exactly as today.
- **Singleton** — a log object holding one entry, in today's body format. Every
  legacy log object is a singleton.
- **Segment** — a log object holding two or more consecutive entries.
- **Chain** — the sequence of log objects recovery reads as the log: start at
  the lowest first index, take the object starting there, continue at its last
  index plus one, stop when nothing starts there.
- **Stray** — a log object that is not on the chain.
- **Commit point** — the single request after which an operation's result is
  what recovery reads.

## Requirements

### Requirement 1: One hard-state write per RequestVote

**User Story:** As an operator sizing election timeouts on an object store, I
want a follower that learns a newer term from a RequestVote to persist its hard
state once before replying, so that the election path does not pay for a write
that the next write supersedes.

#### Acceptance Criteria

1. WHEN `handle_request_vote` receives a term greater than the node's THEN
   `node` SHALL update the term and clear the vote in memory without persisting,
   and SHALL persist the resulting hard state exactly once before the response
   is returned, on every return path after that point (grant and every deny).
2. WHEN the vote is granted THEN that one persist SHALL be
   `persist_term_and_vote_locked()`, carrying the new term and the candidate.
3. WHEN the vote is denied after a term bump THEN that one persist SHALL be
   `persist_term_locked()`, carrying the new term and no vote.
4. Every other `become_follower()` caller SHALL keep persisting inside
   `become_follower()`, unchanged.
5. THE change SHALL apply to every engine; on an engine without
   `hard_state_persistence_engine` the grant path SHALL still write the term
   before the vote.
6. WHEN the follower crashes during that one persist THEN the stored pair SHALL
   be one the `hard_state_persistence_engine` contract allows (old pair, new
   pair, or new term with old vote), and the design SHALL show that the third
   cannot produce a second vote in the new term.

### Requirement 2: Combined hard-state layout

**User Story:** As an operator, I want term and vote stored as one object, so
that persisting both is one PUT and the election path costs half as many
sequential round trips.

#### Acceptance Criteria

1. `object_persistence_options` SHALL gain
   `hard_state_layout` with values `separate` (default, today's behaviour) and
   `combined`.
2. UNDER `combined`, `save_hard_state`, `save_current_term` and
   `save_voted_for` SHALL each issue at most one PUT, to `<prefix>/term`, whose
   body is a JSON object carrying `term` and `voted_for` (`null` for no vote; a
   number or a string matching `NodeId`).
3. UNDER `combined`, `save_hard_state` SHALL issue no request when neither value
   changed, and `save_current_term` / `save_voted_for` SHALL write the value
   given together with the mirror's current value of the other.
4. THE mirror SHALL be updated only after the store acknowledges, as today.
5. UNDER `combined`, no request SHALL be sent to `<prefix>/voted_for` on a
   prefix that never held one.
6. THE body SHALL be rejected by the legacy `term` parser — it SHALL NOT be a
   bare decimal — and a combined body that does not parse, or whose fields are
   missing or of the wrong type, SHALL fail construction naming the key.

### Requirement 3: Hard-state layout transitions

**User Story:** As an operator upgrading a running cluster, I want an existing
bucket to move to the combined hard state in place, and I want an older binary
pointed at an upgraded bucket to refuse to start rather than read stale state.

#### Acceptance Criteria

1. AT load, a `<prefix>/term` body beginning with `{` SHALL be read as a
   combined record, and `<prefix>/voted_for` SHALL then be ignored whatever it
   holds; any other `<prefix>/term` body SHALL be read as legacy, exactly as
   today.
2. WHEN an engine configured `combined` loads a legacy prefix THEN construction
   SHALL write nothing for the migration (Requirement 4.4 completes only a
   retirement already pending), and the **first** hard-state write
   SHALL be the migration: PUT the combined record to `<prefix>/term`, then PUT
   `retired` to `<prefix>/voted_for` if it exists.
3. THE combined PUT SHALL be the commit point; a failure of the retirement PUT
   SHALL NOT fail the call, SHALL be recorded, and SHALL be retried before the
   next hard-state write.
4. AFTER a crash at any request boundary of a migrating write, a new-code load
   SHALL read either the pre-call legacy pair or the post-call pair, and an
   older binary SHALL either read the pre-call legacy pair or refuse to
   construct.
5. WHEN an engine configured `separate` loads a combined `<prefix>/term` THEN
   construction SHALL throw, naming the key and stating that an in-place
   downgrade is not supported.
6. THE design SHALL show that no request ordering lets any binary — this one at
   either setting, or any earlier one — read a `(term, vote)` pair older than
   one this node has acknowledged.

### Requirement 4: Fencing under the combined hard state

1. UNDER `fencing_mode::compare_and_swap`, every combined write SHALL be
   `If-Match` the version of `<prefix>/term` this engine last observed, or
   create-only when it has observed none — the existing `put_single_slot` rule.
2. THE migration's combined PUT SHALL be `If-Match` the legacy `term` object's
   version, and the retirement PUT SHALL be `If-Match` the legacy `voted_for`
   object's version, so that a pre-migration writer still running against the
   prefix is refused at its next write to either slot.
3. A refused precondition on either PUT SHALL latch, exactly as today.
4. UNDER `compare_and_swap`, a retirement pending at load SHALL be completed
   during construction, after `claim_ownership()`; under `none` it SHALL be
   completed before the next hard-state write.
5. THE chokepoint argument SHALL be restated for a single object, and the
   residual window between the combined PUT and the retirement PUT SHALL be
   stated.

### Requirement 5: Segmented log — keys and bodies

**User Story:** As an operator, I want one AppendEntries run stored as one
object, so that a follower's request count and recovery GETs scale with
AppendEntries calls rather than with entries.

1. `object_persistence_options` SHALL gain `log_layout` with values
   `per_entry` (default, today's behaviour) and `segmented`, plus
   `max_segment_entries` (default 100) and `max_segment_bytes` (default 1 MiB).
2. EVERY log object's key SHALL be `<prefix>/log/<20-digit first index>`, so
   listing order SHALL remain index order (cloud-object-persistence Property 2)
   and no two log objects SHALL share a first index.
3. A singleton SHALL be written in today's body format, byte for byte.
4. A segment's body SHALL be a JSON object with `first_index`, `last_index` and
   an `entries` array of today's per-entry records, and SHALL NOT carry a
   top-level `term`, `index` or `command` member.
5. AT load a segment whose key, `first_index`, `last_index`, entry count and
   per-entry indices do not all agree, or that holds fewer than two entries,
   SHALL fail construction naming the key.
6. `max_segment_entries < 2`, `max_segment_bytes == 0` or
   `max_segment_bytes > max_object_bytes` SHALL be rejected at construction, and
   a non-default `max_segment_*` under `per_entry` SHALL be rejected rather than
   ignored.

### Requirement 6: Segmented append

1. UNDER `segmented`, `append_log_entries` with a run that extends the log SHALL
   split it greedily into objects of at most `max_segment_entries` entries and
   at most `max_segment_bytes` of body, write each as one PUT, and keep up to
   `append_concurrency` of them in flight on a `concurrent_key_object_store`.
2. AN entry whose own encoding exceeds `max_segment_bytes` SHALL be written as a
   singleton, subject to `max_object_bytes` alone.
3. `append_log_entry`, and `append_log_entries` with a run that does not extend
   the log, SHALL write singletons, sending exactly the requests they send
   today.
4. THE all-or-nothing contract of `bulk_append_persistence_engine` SHALL hold
   per run, and a failed run SHALL record every object it sent as a stray, by
   object key.
5. THE set of strays a run may overwrite instead of deleting first (the
   `fencing_mode::none` optimisation in `clear_strays`) SHALL be the set of
   **keys** the run writes, not the set of entry indices it covers.

### Requirement 7: Segmented truncation

1. `truncate_log(i)` SHALL delete every log object whose first index is `≥ i`,
   highest first, and then, if one object straddles `i` (first `< i ≤` last),
   SHALL overwrite that object **in place** with its entries below `i` — as a
   singleton when one remains.
2. THE in-place overwrite SHALL be the commit point for the straddling object;
   every earlier request SHALL leave a store whose chain is a prefix of the
   pre-call log no shorter than the post-call log.
3. UNDER `compare_and_swap` the overwrite SHALL be `If-Match` the straddling
   object's last observed version.
4. A failure at any request SHALL throw and leave the mirror equal to what
   recovery would read.

### Requirement 8: Segmented compaction

1. `delete_log_entries_before(j)` SHALL first delete every log object whose last
   index is `< j`, **lowest first**; then, if one object straddles `j`, SHALL
   PUT a new object at `<prefix>/log/<j>` holding that object's entries `≥ j`,
   and then DELETE the straddling object.
2. THE DELETE of the straddling object SHALL be the commit point; until it is
   acknowledged the new object SHALL be a stray, and recovery SHALL read the
   pre-compaction suffix.
3. THE new object SHALL be create-only under `compare_and_swap`.
4. A failure at any request SHALL throw and leave the mirror equal to what
   recovery would read, with any written-but-uncommitted object recorded as a
   stray.

### Requirement 9: Recovery and strays

1. AT load the engine SHALL read the log as the chain, and SHALL record every
   other log object as a stray. Under the legacy layout this SHALL be exactly
   today's `keep_contiguous_log()` result.
2. EVERY log mutation — append, truncation **and compaction** — SHALL delete
   every recorded stray before its first write, and SHALL throw without writing
   if a stray's DELETE fails.
3. THE design SHALL show, for every request boundary of every operation in
   Requirements 2-8, what the store holds after a crash there and that the chain
   is a log the contract of the interrupted call permits.

### Requirement 10: Fencing under the segmented log

1. Every new-key log PUT SHALL be create-only under `compare_and_swap`; the
   in-place truncation overwrite SHALL be `If-Match`; DELETEs SHALL remain
   unconditional (the stated residual of cloud-object-persistence
   Requirement 9.3).
2. THE engine SHALL track a version for each segment it holds, and for no
   singleton.
3. THE design SHALL state which stale-appender writes the segmented layout
   still detects, which it now discards at the next load instead of detecting,
   and SHALL show that it loads no stale object the legacy layout would not
   also load.

### Requirement 11: Defaults and layout transitions for the log

1. AT default options the engine SHALL produce exactly the keys, bodies and
   request sequence it produces today (cloud-object-persistence Property 8),
   and the existing conformance suite SHALL pass unmodified against it.
2. AN engine configured `segmented` SHALL accept a prefix holding only
   singletons, so enabling it on an existing bucket needs no migration step.
3. AN engine configured `per_entry` that loads any segment SHALL throw at
   construction, naming the key and stating that an in-place downgrade is not
   supported.
4. AN older binary that loads a prefix holding a segment SHALL fail to
   construct, and the design SHALL show why for every released engine and for
   the released backup tool.
5. THE two layout options SHALL be independent: either may be enabled without
   the other.

### Requirement 12: Backup and restore

1. `object_store_backup::create` SHALL read a combined `term` for the
   manifest's `current_term` and `voted_for`, ignore a retired `voted_for`, and
   take `last_log_index` from a segment's body rather than its key.
2. `verify` SHALL check contiguity over the chain using the engine's rule, check
   term monotonicity for every entry inside every segment, and report strays
   without failing on them.
3. THE manifest SHALL record `format_version` 2 when the copy holds a combined
   `term` or a segment, and 1 otherwise, so a default bucket's backup is
   unchanged and an older tool refuses a backup it cannot read.
4. `restore_clone` SHALL keep copying byte for byte; `is_engine_owned` SHALL
   continue to cover every key either layout writes; `restore_seed` SHALL keep
   writing a legacy decimal `term`, which an engine at either setting accepts.
5. `cmd/raft_object_backup` SHALL need no new flag for any of the above.

### Requirement 13: Providers

1. THE layouts SHALL use only operations every shipped client already provides:
   `put_object`, `put_object_if` with `if_absent` and `if_version`,
   `get_object`, `delete_object` and `list_keys`.
2. `alibaba_oss_client`, which is not a `conditional_key_object_store`, SHALL
   support both layouts under `fencing_mode::none`.
3. THE design SHALL list, per provider, what each layout depends on that has
   been live-verified and what has not.

### Requirement 14: Operating envelope

1. `doc/cloud_object_persistence.md` SHALL correct the election sizing rule for
   today's code (five sequential PUTs in the common case), and state the rule
   after Requirement 1 (four) and under the combined hard state (two), worked
   against the measured p99s.
2. IT SHALL state the throughput ceiling as leader-bound and unchanged by the
   segmented log, and state what the segmented log changes instead.
3. IT SHALL rework the cost table per three-node cluster for each layout, with
   the entries-per-AppendEntries assumption stated.
4. IT SHALL carry the GCS per-object mutation limit's consequence for a
   combined hard state.

### Requirement 15: Tests

1. THE existing conformance suite SHALL pass unmodified at default options, and
   SHALL be instantiated additionally for each new layout, with
   layout-dependent expectations taken from the harness.
2. `tests/mock_object_store.hpp` SHALL gain crash injection: after the Nth
   request every further request fails, and a fresh engine over the same state
   reads what survived.
3. FOR every operation and every N, a crash-injection case SHALL assert that a
   fresh load reads a log and a hard state the interrupted call's contract
   permits, that the next mutation succeeds, and that a second fresh load equals
   the first engine's mirror.
4. A property case SHALL run random operation sequences against both layouts
   and a reference model, with crashes injected at random request boundaries.
5. A case SHALL pin that every body these layouts write to `<prefix>/term` and
   `<prefix>/log/` is rejected by the legacy parse rules, preserved verbatim in
   a test helper.
6. A Raft-level case SHALL fail against the current `raft.hpp` by observing
   three hard-state writes on a follower granting a vote at a newer term, and
   pass with one.

### Requirement 16: Real-cloud verification

1. Each layout SHALL be exercised on every provider by the opt-in real tier:
   fresh read-back, in-place migration from a prefix a default engine wrote,
   truncation and compaction across a segment, and backup-verify-restore.
2. On the four providers that fence, the real tier SHALL include a live negative
   control that a pre-migration writer's `If-Match` write to the retired vote
   slot is refused by the service.
3. THE real tier SHALL record p50/p99 for a combined hard-state write and for a
   full-size segment PUT, with measurement position.
4. No task requiring a live run SHALL be checked off against the mock tier.
