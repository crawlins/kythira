# Implementation Plan

Leader group commit for the object-store engine. Each task is one mergeable PR.
It builds on `.kiro/specs/object-store-batched-layout/` task 7 (the segmented
write path a flush calls). Clark chose on October 21, 2026 to build it as part
of open-tasks item 15, before that spec's real-cloud task 10, so the real-cloud
runs measure it.

- [x] 1. `durability_class::synchronous`, and the object-store engine as a
  barriered engine with write-behind off
  - `include/raft/persistence.hpp`: the enumerator, `to_string`, and
    `describes_durable_storage` accepting it
  - Engine: `append_log_entry_sequenced` (today's PUT, plus a sequence),
    `barrier_through` (returns), `durable_through`, `durability()` answering
    `synchronous`; `write_behind` option, rejected nowhere yet but unread
  - Tests: the conformance suite unmodified; the engine satisfies
    `barriered_persistence_engine`; `node`'s request log and lock behaviour on
    the mock are unchanged (`raft_batched_durable_writes_test` unmodified)
  - _Requirements: 1.1-1.4, 2.1-2.4_
  - Done October 21, 2026. Departures:
    - `raft.hpp` changes in one place (Requirement 7.1 as amended, and the
      design's "The one `node` change"). Without it, a follower's run on a
      synchronous engine was one PUT per entry.
      `raft_batched_durable_writes_test` gains a synchronous barriered
      recording engine. Its case fails with 6 per-entry appends and no run
      when the change is reverted.
    - The `write_behind` option arrives with task 2, not here. An option
      that is accepted but ignored would let a configuration claim
      write-behind it does not get.
    - `durable_through()` advances on every append, plain, sequenced or
      bulk, and a failed append assigns no sequence. The tests are in the
      new `object_store_group_commit_unit_test.cpp`.

- [x] 2. Write-behind: staging, the flush, failure handling
  - Staging, the single-flight flush with sample-before / publish-after,
    `_mu` released across the requests, `flushes_issued()`
  - Failure: acknowledged prefix published, remainder strayed and kept staged,
    latch on a refused precondition, Requirement 5.4's discarded-sequence throw
  - Truncation and compaction wait for the flush and drop staged entries
  - Tests: conformance instantiated with write-behind over both layouts,
    unfenced and fenced; concurrent stagers coalesce into ≤ 2 flushes; partial
    flush failure; truncation of staged entries fails their waiter; crash
    injection at every request of a flush while appends stage
  - _Requirements: 3.1-3.4, 4.1-4.5, 5.1-5.4, 6.1-6.4, 8.1-8.3_
  - Done October 21, 2026, in the same PR as tasks 1, 3 and 4. Departures:
    - A waiter whose sequence a failed flush covered throws that flush's
      message instead of starting another flush. A retry would PUT
      create-only keys the failed flush may have landed, and its own object
      would refuse it.
    - A plain `append_log_entry` under write-behind stages, then barriers.
      If the barrier throws, the entry and anything staged above it are
      discarded, so a failed plain append leaves the mirror as it found it
      (the conformance suite's failure cases require this).
    - `append_log_entries` drains the stage, then keeps the synchronous
      all-or-nothing path. A bulk run is already one flush.
    - `staged_entry_count()` joins `flushes_issued()` as a test observer.
    - Tests are in `object_store_group_commit_unit_test.cpp` (suite
      `object_store_group_commit_write_behind`) and four new conformance
      instantiations (per-entry and segmented, unfenced and fenced).
      Disabling the failed-flush check or the discarded-sequence check fails
      them.

- [x] 3. Raft-level coalescing
  - A three-node cluster on write-behind mock engines with concurrent
    `submit_command` callers: fewer PUTs than commands on the leader, every
    command committed, no entry counted before its barrier (a recording engine
    asserting `durable_through` at the point `node` counts)
  - _Requirements: 7.1-7.3, 8.4_
  - Done October 21, 2026, in `tests/raft_object_store_group_commit_test.cpp`.
    24 concurrent commands over a 60 ms PUT cost the leader 2 log PUTs. On
    every completion the test checks that a majority holds that many entries
    durably, rather than recording `durable_through` inside `node`. The
    check is the same claim seen from outside, and it needs no hook. The
    third node is isolated so the majority must include the leader. With
    `barrier_through` returning while a flush runs, the test fails.

- [x] 4. Operating envelope
  - `doc/cloud_object_persistence.md`: the write-behind ceiling and how `B` is
    measured; the default unchanged
  - Add the write-behind commit-rate measurement to
    object-store-batched-layout task 10
  - _Requirements: 8.5, 8.6_
  - Done October 21, 2026. The measured ceiling waits for
    object-store-batched-layout task 10.
