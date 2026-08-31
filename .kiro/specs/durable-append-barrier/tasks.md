# Implementation Plan

Ordered so that the measurement exists before the change, and the change is
provably correct before it is made affordable. Every task names the
requirements it satisfies.

**The first task must produce a FAILING test.** A coverage test that passes
against today's code is testing something other than what
`.kiro/specs/multi-raft-performance/` task 19 measured.

- [ ] 1. A failing coverage test
  - A test that appends N entries through a file-backed engine and asserts a
    barrier covered every one of them
  - Must **fail** on the current tree, at roughly the 20–25% task 19 measured;
    record the observed figure in the task when it does
  - Drive it through the smallest surface that reproduces the defect — one
    `node<Types>` with `file_persistence_engine`, not a whole `multi_raft` host
  - _Requirements: 2.1, 2.2, 2.4_

- [ ] 2. The barrier interface on the persistence seam
  - Decide and document the shape: the writer needs "append these bytes" and
    "barrier up to sequence S" as separable operations, which `commit_batch`
    currently fuses
  - `memory_persistence_engine`'s barrier is a no-op and must cost nothing
  - An engine that cannot barrier must be *detectable*, so a configuration
    using one can be refused the word durable rather than assumed optimistic
  - _Requirements: 5.1, 5.4_

- [ ] 3. Move the barrier to the advertise boundary, WITHOUT group commit
  - One barrier per append. Slow by construction and correct by construction —
    this task exists so that correctness lands before performance, and so the
    group-commit task has a known-correct baseline to be measured against
  - Leader: between `raft.hpp:1638` and the entry counting toward `match_index`
  - Follower: before `handle_append_entries` returns success
  - The six configuration-change sites get the same treatment (Requirement 1.3)
  - `save_current_term` / `save_voted_for` keep writing synchronously and are
    NOT folded into any batch (Requirement 1.4)
  - Task 1's test must now pass at 100% coverage
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5_

- [ ] 4. A crash test
  - Append, kill without clean shutdown, restart, assert every advertised entry
    is present. `fiu` is already wired into `file_persistence`
  - This is the task that decides whether task 3 is actually done; a coverage
    counter is evidence and a restart is proof
  - _Requirements: 1.1, 1.2_

- [ ] 5. Group commit
  - One barrier serving several concurrent appends
  - **The ordering rule is the whole of the subtlety and must be restated in
    the code**: the sequence is assigned under the same lock that appends the
    bytes; the barrier samples the highest sequence *before* it calls `fsync`
    and publishes it *after*. Sampling after would credit the barrier with
    writes that raced in during the syscall
  - No lock may be held across the wait, or appends cannot join the same
    barrier (Requirement 3.2)
  - _Requirements: 3.1, 3.2, 3.4_

- [ ] 6. An ordering test for task 5's rule
  - Writer and barrier on separate threads with an injected delay inside the
    barrier; assert no waiter is released by a barrier that began before its
    write landed
  - A group-commit test alongside it: N concurrent appends, fewer than N
    barriers, and every append still covered — both halves, because either
    alone is satisfiable by a broken implementation
  - _Requirements: 3.1, 3.4_

- [ ] 7. Decide `tick_batch_controller`'s future
  - Remove it, or narrow its documented contract to what it provides
  - The design recommends removal, and the reasoning must end up in the header
    beside whatever survives, replacing the comment
    `.kiro/specs/multi-raft-performance/` corrected
  - `batched_persistence_engine` is expected to stay — the group-commit writer
    still needs "flush, then one barrier" — but its caller changes
  - _Requirements: 4.1, 4.2, 4.3_

- [ ] 8. Move the coverage counter to the new question
  - `benchmark_persistence_engine` in `tests/multi_raft_transport_harness.hpp`
    infers coverage from "was a batch open when this append passed through",
    which is the right question for the old design and the wrong one for this
  - Under group commit it becomes "did a barrier covering this entry's sequence
    complete before the advertise"
  - Keep it in the test harness; Requirement 8.2 of the multi-raft-performance
    spec keeps it out of `include/`
  - _Requirements: 2.1, 2.3, 3.3_

- [ ] 9. Regression surface
  - Every existing test passes unmodified, or each modification is justified
    here (Requirement 5.2)
  - `multi_raft_regression_tier` still fits its budget — it runs on
    `memory_persistence_engine`, so Requirement 5.1 should make this free, and
    if it is not free that is a finding about task 2
  - Build under all three future backends
  - _Requirements: 5.2, 5.3_

- [ ] 10. Hand Tier D back to the multi-raft-performance spec
  - That spec's task 19 is blocked on this one and says so. When this lands,
    its durability axis can be re-run and should report **100% barriered**
  - Report the cost against this design's stated prediction, not against a hope
  - Do **not** widen the multi-raft-performance spec's scope here; this task is
    a handover note, not a measurement
  - _Requirements: 2.2_
