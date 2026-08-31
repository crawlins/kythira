# Implementation Plan

Ordered so that the measurement exists before the change, and the change is
provably correct before it is made affordable. Every task names the
requirements it satisfies.

**The first task must produce a FAILING test.** A coverage test that passes
against today's code is testing something other than what
`.kiro/specs/multi-raft-performance/` task 19 measured.

- [x] 1. A failing coverage test
  - A test that appends N entries through a file-backed engine and asserts a
    barrier covered every one of them
  - Must **fail** on the current tree, at roughly the 20–25% task 19 measured;
    record the observed figure in the task when it does
  - **Observed: `entries=33 covered=0 barriers=0 fraction=0` — 0%, not 20–25%,
    and the difference is not a discrepancy.** Task 19 supplied a
    `tick_batch_controller`, whose barrier caught the fifth of the appends that
    raced into its window. There is no controller at this surface, and
    `sync_log_and_directory()` is reached from `commit_batch()` and nowhere
    else, so a node left to itself takes no barrier at all. 0% is the honest
    floor of the same defect. (33 = 32 commands plus the no-op entry a new
    leader appends.) After task 3: `entries=33 covered=33 fraction=1`
  - Drive it through the smallest surface that reproduces the defect — one
    `node<Types>` with `file_persistence_engine`, not a whole `multi_raft` host
  - _Requirements: 2.1, 2.2, 2.4_

- [x] 2. The barrier interface on the persistence seam
  - Decide and document the shape: the writer needs "append these bytes" and
    "barrier up to sequence S" as separable operations, which `commit_batch`
    currently fuses
  - `memory_persistence_engine`'s barrier is a no-op and must cost nothing
  - An engine that cannot barrier must be *detectable*, so a configuration
    using one can be refused the word durable rather than assumed optimistic
  - _Requirements: 5.1, 5.4_

- [x] 3. Move the barrier to the advertise boundary
  - One barrier per append. Slow by construction and correct by construction —
    this task exists so that correctness lands before performance, and so the
    group-commit task has a known-correct baseline to be measured against
  - **Delivered as one change with task 5 rather than two.** The barrier is
    taken by `node::settle_barrier` at both response boundaries, and the
    engine's `barrier_through` coalesces from the day it exists. Splitting them
    would have meant writing a deliberately non-coalescing `barrier_through`
    and then deleting it, and the "known-correct baseline" the split exists to
    provide is available anyway: `one_barrier_covers_a_whole_batch_of_appends`
    asserts the serial case exactly, and the leader row measured 33 barriers
    for 33 serial appends before any concurrency was introduced
  - Leader: `submit_command` releases `_mutex` and barriers before
    `replicate_to_followers()`; the entry counts toward `match_index` only via
    `advance_commit_index`'s self-ack, which is now gated on the new
    `_durable_log_index` watermark rather than being unconditional
  - Follower: `handle_append_entries` takes a `unique_lock`, runs the
    consistency check, releases it, barriers, and only then returns success.
    The peer-to-peer catch-up path gets the same treatment at its own
    boundary
  - The six configuration-change sites get the same treatment (Requirement 1.3)
  - `save_current_term` / `save_voted_for` keep writing synchronously and are
    NOT folded into any batch (Requirement 1.4)
  - Task 1's test must now pass at 100% coverage
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5_

- [x] 4. A crash test
  - Append, kill without clean shutdown, restart, assert every advertised entry
    is present. `fiu` is already wired into `file_persistence`
  - `entries_survive_a_process_death_with_no_clean_shutdown` forks a child that
    appends and barriers 20 entries and then leaves by `_exit` — no
    destructors, no stream flush, no shutdown of any kind — and the parent
    reopens the directory and finds all 20. **What it proves and does not
    prove is stated in the test**: it proves nothing an advertised append needs
    is stranded in the process's own memory, and that recovery reads it back.
    Only the `fsync` proves power-cut durability, and the coverage test is what
    asserts the `fsync` happened. Neither claim stands alone
  - `a_failed_barrier_is_surfaced_and_not_advertised` is the other half of
    Requirement 1.5, through `fiu`: the engine throws, the client's future
    fails, and the entry does **not** commit, because the leader withheld its
    own acknowledgement
  - This is the task that decides whether task 3 is actually done; a coverage
    counter is evidence and a restart is proof
  - _Requirements: 1.1, 1.2_

- [x] 5. Group commit
  - One barrier serving several concurrent appends
  - **The ordering rule is the whole of the subtlety and must be restated in
    the code**: the sequence is assigned under the same lock that appends the
    bytes; the barrier samples the highest sequence *before* it calls `fsync`
    and publishes it *after*. Sampling after would credit the barrier with
    writes that raced in during the syscall
  - No lock may be held across the wait, or appends cannot join the same
    barrier (Requirement 3.2). Two locks matter and both are released: the
    engine's `_mu` across the syscall, and `node`'s `_mutex` across the wait at
    both response boundaries. The configuration-change and administration-entry
    sites keep the node lock, which costs no coalescing that matters — at most
    one such entry per election or membership change — and is documented at
    `persist_and_barrier_locked`
  - Measured: 8 concurrent appends, **2 barriers**, every append covered
  - _Requirements: 3.1, 3.2, 3.4_

- [x] 6. An ordering test for task 5's rule
  - Writer and barrier on separate threads with an injected delay inside the
    barrier; assert no waiter is released by a barrier that began before its
    write landed
  - A group-commit test alongside it: N concurrent appends, fewer than N
    barriers, and every append still covered — both halves, because either
    alone is satisfiable by a broken implementation
  - _Requirements: 3.1, 3.4_

- [x] 7. Decide `tick_batch_controller`'s future
  - Remove it, or narrow its documented contract to what it provides
  - **Removed.** The reasoning is in `multi_raft.hpp` where the type used to be
    declared, and in `multi_raft_impl.hpp` where `tick()` used to call it. Two
    cases in `multi_raft_host_unit_test.cpp` asserted its behaviour and are
    deleted rather than rewritten, with the justification Requirement 5.2 asks
    for recorded where they were; a third — the persist/send/apply ordering
    test, for which the controller was incidentally the only witness — is
    rewritten around a store that records when it is written to, because that
    ordering claim is still true and still worth a test
  - `batched_persistence_engine` is expected to stay — the group-commit writer
    still needs "flush, then one barrier" — but its caller changes
  - _Requirements: 4.1, 4.2, 4.3_

- [x] 8. Move the coverage counter to the new question
  - `benchmark_persistence_engine` in `tests/multi_raft_transport_harness.hpp`
    infers coverage from "was a batch open when this append passed through",
    which is the right question for the old design and the wrong one for this
  - Under group commit it becomes "did a barrier covering this entry's sequence
    complete before the advertise"
  - Keep it in the test harness; Requirement 8.2 of the multi-raft-performance
    spec keeps it out of `include/`
  - _Requirements: 2.1, 2.3, 3.3_

- [x] 9. Regression surface
  - Every existing test passes unmodified, or each modification is justified
    here (Requirement 5.2)
  - `multi_raft_regression_tier` still fits its budget — it runs on
    `memory_persistence_engine`, so Requirement 5.1 should make this free, and
    if it is not free that is a finding about task 2
  - Build under all three future backends
  - _Requirements: 5.2, 5.3_

- [x] 10. Hand Tier D back to the multi-raft-performance spec
  - That spec's task 19 is blocked on this one and says so. When this lands,
    its durability axis can be re-run and should report **100% barriered**
  - Report the cost against this design's stated prediction, not against a hope
  - Do **not** widen the multi-raft-performance spec's scope here; this task is
    a handover note, not a measurement
  - _Requirements: 2.2_
