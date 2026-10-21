# Implementation Plan

Each task is one independently mergeable PR: it builds, its tests pass, and the
tree is safe to ship with any later task not yet merged. Tasks 1-5 deliver the
election-latency half (Parts 1-2 of the design); tasks 6-8 the segmented log
(Part 3), whose timing depended on open question 1 — scoped October 21, 2026
(design, open question 1): leader group commit is an engine-local write-behind
barrier that calls Part 3's segment write, so tasks 6-9 proceed in order and
group commit gets its own spec after task 7. Every task runs the
existing conformance suite unmodified at default options (design Property 1)
before and after.

- [x] 1. `node`: one hard-state write per RequestVote
  - Split `become_follower()` (`include/raft/raft.hpp:6519`) into
    `step_down_locked(new_term) -> bool` plus the persist; the ten other
    callers keep `become_follower()` and its behaviour
  - `handle_request_vote` (`raft.hpp:5464`): `step_down_locked` at Rule 2
    (`:5534`); one `persist_term_locked()` on each deny path after a bump, one
    `persist_term_and_vote_locked()` on the grant; the response is built after
    the persist on every path
  - Extend `tests/raft_batched_durable_writes_test.cpp` (`recording_engine`): a
    follower that voted at T-1 and grants at T records one hard-state call, and
    the same case fails against the current `raft.hpp` (three); a deny after a
    bump records one call storing no vote; the existing
    `a_refused_vote_from_a_newer_term_stores_the_term` and
    `a_follower_that_moves_terms_does_not_keep_its_previous_vote` stay green
  - Without the extension: grant is `save_current_term` then `save_voted_for`
  - Done October 20, 2026. `become_follower()` took a defaulted
    `step_down_write` argument rather than splitting out `step_down_locked`:
    the same behaviour with one function, and the other callers unchanged
  - `doc/cloud_object_persistence.md`: correct the sizing rule — five PUTs in
    the common case before this task, four after — and the engine header's
    "The honest cost" paragraph (`object_store_persistence.hpp:92-124`)
  - _Requirements: 1.1-1.6, 14.1, 15.6_

- [x] 2. Test infrastructure, no behaviour change
  - `tests/mock_object_store.hpp`: `crash_after_requests(n)` — every request
    after the n-th throws and changes nothing; cleared for the fresh engine
  - `tests/legacy_layout_reader.hpp`: today's `parse_unsigned`,
    `json_to_entry`, and the backup tool's `parse_u64` / `check_entry_term`
    predicates, copied verbatim with the commit hash they were taken from, and
    a comment saying why they must never be edited to match the engine
  - Conformance harness gains `default_options()`; layout-dependent cases (exact
    key set, request counts, key shapes) take their expectation from it; the
    three existing instantiations (`object_store_persistence_unit_test.cpp:208,
    215, 222`) are unchanged in result
  - A crash-injection oracle helper: given an operation and the pre-/post-call
    model, the set of legal loaded states
  - Done October 20, 2026. `crash_after_requests(n)` counts every request
    kind (conditional ones included) and a refused request is neither logged
    nor applied; the test disarms it with a new `clear_crash()` before the
    fresh engine, and `crashed()` reports whether it fired. The legacy reader
    copies the bodies verbatim from 92a1825 but not the wrappers: static
    members became free functions over the `std::uint64_t` instantiation,
    `base64_to_bytes` came along because `json_to_entry` calls it, and only
    the parse half of `check_entry_term` is kept, throwing instead of pushing
    into a `verification_report`. `default_options()` is detected on the
    harness like `fenced` (absent = `{}`), so no existing harness changed; the
    layout-dependent cases turned out to be the key set and the `term` /
    `voted_for` body assertions (no request count differs under `combined`),
    plus `the_none_sentinel_loads_as_no_vote`, which seeds a combined `null`
    under `combined`. The oracle (`tests/object_store_crash_oracle.hpp`) works
    on observable state (term, vote, log) for five operation kinds; the
    every-n driver lives in the new `tests/object_store_batched_layout_unit_test.cpp`,
    which also pins today's engine under it (hard state, batched append,
    truncation, compaction)
  - _Requirements: 15.1, 15.2, 15.5_

- [x] 3. Combined hard state, fresh prefixes
  - `hard_state_layout { separate, combined }` on `object_persistence_options`
  - Combined codec (exactly `term`, `voted_for`; `to_number<uint64_t>`; JSON
    string or number for `NodeId`; strict)
  - `save_hard_state` / `save_current_term` / `save_voted_for` under `combined`:
    one PUT to `term_key()` through `put_single_slot`, skip-if-unchanged,
    mirror after acknowledgement
  - Load precedence by first byte, resolved after the listing loop; `separate`
    over a combined `term` refused with the downgrade message; decimal `term`
    beside `retired` refused as corruption
  - A `combined` engine over a **legacy** prefix is refused at construction in
    this task ("migration not yet supported") — relaxed by task 4, so no build
    of this task can half-migrate a bucket
  - Tests: conformance instantiated with `combined` over the mock and fenced
    mock; request-log case (one PUT to `term`, none to `voted_for`); every
    malformed body; the legacy reader rejects every body written; crash
    injection at n = 0, 1 for each of the three methods
  - Done October 20, 2026. Departures: the downgrade message does not name
    `raft_object_backup relayout`, which does not exist (task 11 is
    optional) — it says to set `combined` or restore from a pre-upgrade
    backup, and task 11 should add the verb if it lands. Skip-if-unchanged
    applies to `save_hard_state` only; `save_current_term` / `save_voted_for`
    always PUT, as they do under `separate` and as the design's write path
    reads. Besides a decimal `term`, a legacy vote slot with no `term`, and a
    combined `term` beside a legacy vote slot (the design's "retirement
    pending" row, which only task 4 code can produce), are refused as
    "migration not yet supported" too. `retired` beside an absent `term` (not
    in the design's table) is corruption like `retired` beside a decimal one,
    at both settings — so a string-`NodeId` `separate` engine no longer reads
    a vote slot holding `retired` as a node id. The codec also refuses a
    float-typed term or vote (`1.0`), which `to_number` alone would accept;
    Boost 1.83 keeps the last of duplicate members, so a record with `term`
    twice is not detected — a residual. A `separate` engine refuses a
    combined `term` as soon as it parses it, since that refusal does not
    depend on the vote slot; the rest resolves after the listing loop. Every
    refusal is a `std::runtime_error`
  - _Requirements: 2.1-2.6, 3.1, 3.5, 4.1, 4.3, 15.3, 15.5_

- [x] 4. Combined hard state, in-place migration
  - First hard-state write on a legacy prefix: m1 (`term` ← combined, `If-Match`
    the legacy version under fencing) then m2 (`voted_for` ← `retired`,
    `If-Match` under fencing)
  - `_vote_slot_to_retire`: transient m2 failure recorded and retried before the
    next hard-state write; completed at construction after `claim_ownership()`
    under `compare_and_swap`; refused precondition latches
  - Tests: crash injection at every boundary of the migration, asserting the
    design's four-row table, including "older binary refuses" through the
    legacy reader; fenced migration with a second writer touching `voted_for`
    before m2 (migrating engine latches) and after (second writer latches);
    a migration on a prefix with no `voted_for` sends no m2
  - Docs: the runbook paragraph that an older binary's
    `corrupt object <prefix>/term` on an upgraded bucket is the rollback guard
  - Done October 20, 2026. Task 3's "migration not yet supported" refusals
    are gone; what stays refused is `separate` over a combined `term`,
    `retired` beside a decimal or absent `term`, a malformed record, and an
    unparseable legacy vote beside a legacy or absent `term`. Departures:
    a refused precondition on the retirement completed at fenced construction
    fails construction with `persistence_fenced_error`, as a refused owner
    claim does, since there is no engine yet to latch. A retry ahead of the
    next hard-state write that fails transiently again does not fail that
    write (the design does not say): the record is correct without it, so
    it stays pending and is tried again. A refused retry latches before the
    write is sent. Retries run only on hard-state writes, not log writes.
    Beside a combined `term`, a vote slot
    that does not parse is now ignored and retired (Requirement 3.1, "whatever
    it holds") where task 3 refused it, so the legacy vote is parsed after the
    listing loop. A legacy prefix with a vote slot and no `term` gets a
    create-only m1 under fencing, there being no legacy version to match.
    `save_hard_state` of the pair already loaded sends nothing and so does
    not migrate. Two accessors not in the design,
    `vote_slot_retirement_pending()` and `last_vote_slot_retirement_error()`,
    report a pending retirement like `last_prune_error()`; the mock gained
    `lose_next_put_responses(n)` to test the "m1 landed, response lost" row.
    The migration is covered in `object_store_batched_layout_unit_test.cpp`,
    fenced and unfenced, not by a new conformance case. The runbook also says
    to take a backup *before* switching. Until task 5 this build's
    `raft_object_backup` refuses an upgraded prefix too, so the design's
    "back up an upgraded bucket with the upgraded tool" cannot be followed
    yet
  - _Requirements: 3.2-3.4, 3.6, 4.2, 4.4, 4.5, 15.3_

- [x] 5. Backup and restore understand the combined hard state
  - `interpret_metadata` (`object_store_backup.hpp:730`): combined `term` →
    `current_term` and `voted_for` (`null` → `"none"`); `retired` ignored;
    resolved after the loop
  - `format_version` 2 iff the copy holds a combined `term`; 1 otherwise, with a
    case asserting a default bucket's manifest is byte-identical to today's
  - Tests in `object_store_backup_unit_test.cpp` and
    `object_store_backup_cli_unit_test.cpp`: create → verify → restore-clone →
    a `combined` engine reads back; `restore_seed` into a target a `combined`
    engine then migrates; the older tool's `parse_u64` (legacy reader) refuses
  - Done October 20, 2026. The codec is shared: the `NodeId`-independent half
    (`parse_combined_hard_state`, the record's shape and its `term`) and
    `k_retired_vote` moved into `object_store_persistence_detail`; the
    engine's `json_to_hard_state` narrows `voted_for` to its `NodeId` as
    before, with the same messages, and the backup tool renders it. One
    parser for the shape, so neither side accepts a record the other refuses.
    `interpret_metadata` only notes `term` and `voted_for`; the new
    `resolve_hard_state` reads them after the loop. Departures: `retired` is
    ignored only beside a combined `term`. Beside a decimal or absent one it
    throws and no manifest is written, as the engine refuses that pair — on a
    live source it is a migration landing between the two reads, and the
    message says to take the backup again. A legacy vote beside a combined
    `term` (retirement pending) is ignored whatever it holds. The backup tool
    is generic over the store, not `NodeId`, so a combined vote may be a
    string or an unsigned integer (rendered in decimal), and anything else
    (bool, negative, float) is corruption. `verify` accepts versions 1 and 2,
    and its `manifest_version` message now lists both. It does not re-derive
    the version from the copied `term`, so a hand-edited manifest claiming 1
    over a combined copy is not caught — a residual. Whether an older tool
    refuses a version 2 manifest is now tested: `legacy_layout_reader.hpp`
    gained `verify`'s version gate, copied from 92a1825 like the rest (no
    existing rule changed), and the runbook names the message. The engine
    round trip runs fenced and unfenced, over a fresh and a migrated source;
    the CLI cases use the string-`NodeId` engine. The October 20 CHANGELOG
    bullet for task 4, which said the tool could not read an upgraded bucket
    yet, was corrected as well as a bullet added
  - _Requirements: 12.1, 12.3, 12.4, 12.5_

- [x] 6. Chain-walk recovery and stray hygiene, layout-neutral
  - Replace `keep_contiguous_log()` (`object_store_persistence.hpp:1669`) with
    the chain walk; on singletons it must produce the same log and strays
  - `delete_log_entries_before` calls `clear_strays({})` first (it does not
    today); state in the commit body that this changes the request sequence only
    when strays exist
  - The stray keep-set in `append_log_entries` (`:995`) becomes the set of
    object keys written; identical to today's under `per_entry`
  - Tests: existing conformance and batched-durable-writes suites unmodified; a
    case seeding a stray that a compaction without `clear_strays` would have made
    the chain's start
  - Done October 21, 2026. The load path collects log objects by key (first
    index → entries) and `walk_log_chain` replaces `keep_contiguous_log`; an
    object is read only by its first index and entry count, so task 7 feeds
    segments into the same map. The keep-set is computed per object written
    (one per entry until task 7) and `clear_strays`' contract now says keys.
    The compaction case is a real latent bug on today's layout, not only a
    segmented-log precondition: a stray past a gap survived a compaction
    that deleted everything below it, and the next load started the log at
    the stray. The new cases (`chain_walk_recovery` in
    `object_store_batched_layout_unit_test.cpp`) fail against the previous
    engine on exactly that
  - _Requirements: 9.1, 9.2, 6.5, 11.1_

- [ ] 7. Segmented log
  - `log_layout { per_entry, segmented }`, `max_segment_entries` (100),
    `max_segment_bytes` (1 MiB), validated at construction; non-default bounds
    under `per_entry` rejected
  - Segment codec (`first_index`, `last_index`, `entries` of today's records,
    byte-identical elements); load validation; `_objects` mirror; versions for
    segments under fencing only
  - Append: greedy chunking, oversized entry as a singleton, size check before
    any request, chunks through `put_log_objects` with `append_concurrency`;
    non-extending append refused under `segmented`; singletons from
    `append_log_entry` unchanged
  - Truncation: DELETE objects at or above `i` highest first, then overwrite the
    straddler in place (`If-Match` under fencing)
  - Compaction: DELETE objects below `j` lowest first, PUT `log/<j>`
    create-only (stray until committed), DELETE the straddler (commit point)
  - `per_entry` over any segment refused with the downgrade message
  - Tests: conformance instantiated with `segmented` and with both layouts over
    the mock, fenced mock, Alibaba mock server and OCI mock server; crash
    injection at every n for a multi-chunk append, a truncation across a
    segment, a compaction across a segment and `clear_strays`, asserting the
    design's tables; the model-based property case over random sequences with
    random crashes; the stale-appender table's three rows under fencing;
    the legacy reader rejects every segment body; a segment body element is
    byte-identical to `entry_to_json`
  - _Requirements: 5.1-5.6, 6.1-6.4, 7.1-7.4, 8.1-8.4, 9.3, 10.1-10.3,
    11.2-11.5, 13.1, 13.2, 15.1, 15.3, 15.4, 15.5_

- [ ] 8. Backup and restore understand segments
  - `create`: `last_log_index` from a segment's body; unparseable segment
    throws, no manifest
  - `verify`: per-entry term check inside segments; contiguity by the chain
    rule; non-chain objects in a new `verification_report::notes` as
    `"stray_object"`, not as problems
  - `format_version` 2 iff a segment (or a combined `term`) is present
  - Tests: backup of a torn compaction state verifies with one note and
    clone-restores to an engine that deletes the stray before its first write;
    the older tool's `check_entry_term` (legacy reader) reports the segment
    unreadable, so its restore aborts
  - _Requirements: 12.1-12.5_

- [ ] 9. Operating envelope and documentation
  - `doc/cloud_object_persistence.md`: layout table for both knobs; the three
    election rules worked against the measured p99s; throughput stated as
    leader-bound and unchanged, with what the segmented log changes instead;
    the per-cluster cost table with `B` stated; the GCS per-object mutation
    consequence; the transitions table; replace the "Future work" bullets that
    this spec closes and name leader group commit as what remains
  - Engine header layout and cost sections (`object_store_persistence.hpp:19-65,
    92-124`) and `object_store_backup.hpp`'s format-coupling note (`:60-66`)
  - `doc/TODO.md`: close open-tasks item 15 to the extent landed, with the
    residual gaps from the design copied rather than summarised away
  - _Requirements: 14.1-14.4_

- [ ] 10. Real-cloud verification — needs Clark's go
  - New cases in `tests/object_persistence_real_cases.hpp`: migration read-back,
    truncation and compaction across a real segment, the live negative control
    on the retired vote slot (four fencing providers), p50/p99 of a combined
    write and of 100-entry and `max_segment_bytes` segment PUTs with measurement
    position, backup → verify → clone-restore → read-back
  - Run on S3, Azure Blob, GCS, OCI (fenced and unfenced) and Alibaba OSS
    (unfenced) through the existing `object-persistence` CI bundles; GCS spaces
    hard-state writes at 1100 ms
  - Record results beside the existing latency tables and revisit the
    `max_segment_bytes` default against the measured segment PUT p99
  - Not checked off against the mock tier
  - _Requirements: 16.1-16.4, 13.3_

- [ ] 11. (Optional, pending open question 2) `raft_object_backup relayout`
  - Offline, against a stopped node's prefix: combined → separate (PUT
    `voted_for` legacy value, then PUT decimal `term` — commit point) and
    segmented → per_entry (for each segment `[f..l]`, highest first: PUT
    singletons `f+1..l`, which are strays inside the segment until the next
    step, then overwrite `log/f` with the singleton for `f` — commit point),
    each crash-safe in that order
  - Offline only; under fencing it claims the prefix through the owner
    object's takeover rule first, so an engine restarted mid-relayout is fenced
    out rather than racing it
  - Tests: crash injection at every boundary; round trip upgrade → relayout →
    a default engine loads the same state
  - _Requirements: 3.5, 11.3_
