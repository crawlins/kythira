# Implementation Plan

Leader group commit for the object-store engine. Each task is one mergeable PR.
It builds on `.kiro/specs/object-store-batched-layout/` task 7 (the segmented
write path a flush calls). Clark chose on October 21, 2026 to build it as part
of open-tasks item 15, before that spec's real-cloud task 10, so the real-cloud
runs measure it.

- [ ] 1. `durability_class::synchronous`, and the object-store engine as a
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

- [ ] 2. Write-behind: staging, the flush, failure handling
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

- [ ] 3. Raft-level coalescing
  - A three-node cluster on write-behind mock engines with concurrent
    `submit_command` callers: fewer PUTs than commands on the leader, every
    command committed, no entry counted before its barrier (a recording engine
    asserting `durable_through` at the point `node` counts)
  - _Requirements: 7.1-7.3, 8.4_

- [ ] 4. Operating envelope
  - `doc/cloud_object_persistence.md`: the write-behind ceiling and how `B` is
    measured; the default unchanged
  - Add the write-behind commit-rate measurement to
    object-store-batched-layout task 10
  - _Requirements: 8.5, 8.6_
