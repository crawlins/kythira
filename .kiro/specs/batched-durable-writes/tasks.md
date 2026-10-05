# Implementation Plan

- [x] 1. Extension concepts on the persistence seam
  - `bulk_append_persistence_engine`, `hard_state_persistence_engine` in
    `include/raft/persistence.hpp`, with their contracts
  - _Requirements: 1.1, 3.1_

- [x] 2. Memory and file engines
  - `append_log_entries` and `save_hard_state` on both; the file engine skips
    unchanged slots and writes "none" for no vote
  - Forward both through `group_scoped_persistence`
  - _Requirements: 1.6, 3.2, 3.3, 3.4_

- [x] 3. Object-store engine
  - `save_hard_state` (term first, unchanged slots skipped)
  - `append_log_entries` with `append_concurrency` and the
    `concurrent_key_object_store` declaration on all five clients and the mock
  - Strays, holes at load, highest-first truncation, size check before send
  - _Requirements: 2.1–2.8, 3.2–3.4_

- [x] 4. `node`
  - `persist_and_append_run` on the follower path
  - `persist_term_and_vote_locked` / `persist_term_locked` at all four sites;
    term-first fallback
  - _Requirements: 1.4, 1.5, 3.5, 3.6_

- [x] 5. Tests
  - `tests/batched_durable_writes_unit_test.cpp`: 25 engine-level cases
  - `tests/raft_batched_durable_writes_test.cpp`: 3 cluster cases, all three
    failing against the previous `raft.hpp` (6 single appends and no run on
    each follower; vote written before its term; follower kept vote 3 after
    moving terms)
  - _Requirements: 4.1, 4.2_

- [ ] 6. Live measurement on a real object store
  - Re-run the real-tier append latency case with a multi-entry AppendEntries
    and record the throughput next to the `1 / p50(PUT)` table in
    `doc/cloud_object_persistence.md`. Needs cloud credentials; not run.
  - _Requirements: 2.1_
