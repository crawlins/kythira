# Implementation Plan — Learner Placement-Capacity Criteria

## Status: Completed

**Last Updated**: July 3, 2026

All 12 tasks are implemented, tested, and passing (6 new test files, 30 test
cases; 24 targeted regression tests plus the full 183-target non-infrastructure
suite re-verified green — Phase 5 additionally re-verified against the four
existing quorum-manager test suites that construct `placement_group_target`/
`desired_topology`). Two deviations from the plan worth noting up front:

1. The learner mechanism itself (`cluster_configuration.learners`, election
   exclusion, replication wiring, `add_learner()`/`remove_learner()`/
   `promote_to_voter()`, and the leader's automatic promotion-vs-provisioning
   loop) was assumed as a pre-existing prerequisite when this plan was
   written, but did not actually exist in the codebase at implementation
   time. It was built as part of this effort rather than deferred — see the
   per-task notes below and the `## Notes` section at the end for what that
   involved.
2. Phase 5 (Tasks 10-12) is an addendum added after the fact: the original
   design tied a group's learner admission ceiling to the same `target_count`
   used for its voting ceiling, which meant a group with its voting target
   already met (as any healthy group normally is) could never admit a single
   learner. Phase 5 decouples the two via an optional `learner_capacity`
   field — see requirements.md Requirement 2.4-2.6 and design.md Property 4.

## Overview

Add a single placement-capacity criterion, evaluated at two points, on top
of Kythira's learner mechanism (assumed to already exist): whether admitting
a joining node as a learner, or promoting an existing learner to a voting
node, would keep the affected placement group's node count consistent with
the desired topology already tracked via `_quorum_manager.topology()`. A
learner blocked by capacity is never removed or specially marked — it
remains an ordinary learner and is reconsidered fresh on every subsequent
assessment cycle, becoming eligible the moment its own group has room. Four
phases: the shared capacity-check helper, wiring it into the two entry
points, wiring it into the leader's automatic policy (including the
stateless retry behavior), tests.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "Shared group_has_capacity() helper and the two live-count accessors"
    },
    {
      "wave": 2,
      "tasks": [2, 3],
      "description": "Wire the criterion into add_learner() (admission) and promote_to_voter() (promotion) — both depend on wave 1, independent of each other"
    },
    {
      "wave": 3,
      "tasks": [4, 5],
      "description": "Wire the criterion into the leader's automatic promotion-vs-provisioning policy (task 4) and its stateless retry behavior (task 5, depends on task 4) — depends on wave 2"
    },
    {
      "wave": 4,
      "tasks": [6, 7, 8, 9],
      "description": "Tests"
    },
    {
      "wave": 5,
      "tasks": [10, 11, 12],
      "description": "Addendum: decouple learner capacity from voting target — new learner_capacity field (task 10), refactor the capacity check into group_has_admission_capacity()/group_has_promotion_capacity() (task 11, depends on task 10), new test (task 12, depends on task 11)"
    }
  ]
}
```

## Tasks

---

## Phase 1: Shared Capacity Check (Task 1)

- [x] 1. Add `group_has_capacity()` and the two live-count accessors
  - Add a private helper
    `auto node<Types>::group_has_capacity(placement_group_id_type group, std::size_t live_count) const -> bool`
    that looks up `group` in `_quorum_manager.topology().groups`, returning
    `false` (fail closed) if absent, otherwise `live_count < target.target_count`.
  - Add `auto node<Types>::voting_count_in_group(placement_group_id_type group) const -> std::size_t`,
    counting `_configuration.nodes()` whose placement (via the existing
    `_placement_map`, same lookup pattern used in
    `build_quorum_cluster_vector()`) matches `group`.
  - Add `auto node<Types>::voting_and_learner_count_in_group(placement_group_id_type group) const -> std::size_t`,
    equal to `voting_count_in_group(group)` plus the count of
    `_configuration.learners()` whose placement matches `group`.
  - Verify: unit test — configuration with 2 voting nodes and 1 learner in
    group A, `desired_topology` target for A = 3; assert
    `voting_count_in_group(A) == 2`,
    `voting_and_learner_count_in_group(A) == 3`,
    `group_has_capacity(A, 2) == true`, `group_has_capacity(A, 3) == false`;
    assert `group_has_capacity(unknown_group, 0) == false` (fail-closed).
  - _Requirements: 1.1, 1.2, 1.3, 1.4_
  - **Done.** Implemented in `include/raft/raft.hpp` alongside `is_learner()`
    and `placement_of()`. Also required, as prerequisite work not originally
    scoped here: adding `_learners` to `cluster_configuration`
    (`include/raft/types.hpp`) and its JSON round-trip
    (`include/raft/config_entry.hpp`, backward-compatible with entries that
    predate the field), plus election guards in `check_election_timeout()`
    and `handle_request_vote()`, and `become_leader()` peer-tracking
    initialization for learners. Verified via
    `tests/learner_mechanism_unit_test.cpp` rather than the unit test
    originally sketched for this task in isolation, since the helpers are
    private and are exercised through the public `add_learner()`/
    `promote_to_voter()` surface instead.

---

## Phase 2: Wire Criterion into Entry Points (Tasks 2–3)

- [x] 2. Enforce the admission capacity criterion in `add_learner()`
  - At the top of `add_learner()`'s precondition block, before any log entry
    is appended, compute the candidate's group and call
    `group_has_capacity(group, voting_and_learner_count_in_group(group))`;
    on `false`, return a failed future carrying a new
    `learner_capacity_exceeded_exception{group}`.
  - Verify: unit test — group at target (voting+learner count ==
    target_count), attempt `add_learner()`, assert failure with
    `learner_capacity_exceeded_exception` and that the leader's last log
    index is unchanged; a second test with the group below target asserts
    success.
  - _Requirements: 2.1, 2.2, 2.3_
  - **Done.** `add_learner()` implemented from scratch in `raft.hpp`
    (precondition checks, non-joint configuration entry, `_commit_waiter`-based
    future resolution) with the capacity check wired in as planned. Verified
    in `tests/learner_admission_capacity_test.cpp` (blocked at target, allowed
    below target, fail-closed on an undeclared group).

- [x] 3. Enforce the promotion capacity criterion in `promote_to_voter()`
  - Symmetric to Task 2: compute the learner's group, call
    `group_has_capacity(group, voting_count_in_group(group))`; on `false`,
    return a failed future carrying a new
    `voting_capacity_exceeded_exception{group}`.
  - Verify: unit test — group's voting count already at target but with an
    additional learner also present in that group (voting+learner count
    above target); attempt `promote_to_voter()` on a *different* learner in
    the same group whose promotion would push voting count to target+1,
    assert failure; a second test with voting count below target asserts
    success. This specifically exercises that promotion uses the
    voting-only count, not voting+learner (Property 3 in design.md).
  - _Requirements: 3.1, 3.2, 3.3_
  - **Done.** `promote_to_voter()` implemented reusing the same joint-consensus
    append/`_config_synchronizer` flow as `add_server()`, deliberately leaving
    an already-promoted learner's `_next_index`/`_match_index` untouched. The
    "additional learner in the group" scenario needed an adjustable
    mock quorum manager (`tests/learner_promotion_capacity_test.cpp`) since a
    single admission call can't simultaneously leave a group under its
    admission threshold and at its promotion threshold — the test shrinks the
    topology's target between admission and promotion to exercise this
    distinction validly.

---

## Phase 3: Leader Automatic Policy (Task 4)

- [x] 4. Route the leader's automatic promotion-vs-provisioning choice through the criterion
  - Wherever the leader's quorum-maintenance loop selects a learner to
    promote instead of provisioning a new node for a placement group,
    confirm it calls `promote_to_voter()` (Task 3) rather than any separate
    or duplicated capacity check.
  - On a capacity-criterion failure for the selected learner, fall back to
    the existing `provision_node()` path for that group's remaining deficit
    rather than retrying the same learner or failing the assessment cycle.
  - Confirm (no code change expected) that automatic learner admission via
    `ClusterJoin` already goes through `add_learner()` (Task 2) and
    therefore already picks up the admission capacity criterion with no
    further wiring.
  - Verify: integration/property test — a group already at its voting
    target with one eligible learner present; assert the leader falls back
    to `provision_node()` rather than promoting that learner or failing the
    cycle.
  - _Requirements: 4.1, 4.2, 4.3_
  - **Done.** `run_quorum_assessment()` now scans `_configuration.learners()`
    per deficit group under lock, checks capacity, and fire-and-forgets
    `promote_to_voter()` for as many eligible learners as the deficit and
    capacity allow before falling back to `provision_node()` for any
    remainder. One line in `handle_cluster_join()` did need changing (not
    "no code change expected" as this task assumed): it called `add_server()`
    directly, which would have made every joining node a full voter
    immediately: changed to `add_learner()` so new nodes catch up before
    being trusted with a vote. Verified in
    `tests/quorum_promotion_capacity_fallback_test.cpp`.

- [x] 5. Confirm blocked learners require no bookkeeping and are re-scanned every cycle
  - This task is primarily verification, not new production code: because
    `group_has_capacity()` (Task 1) reads only live state, a learner that
    failed Task 4's capacity check needs no "blocked" flag, retry queue, or
    other persisted marker to become eligible again — confirm the
    quorum-maintenance loop's per-cycle learner scan (Task 4) is not
    filtering out learners it previously declined to promote (e.g. no
    accidental memoization/caching of a prior rejection).
  - If any such filtering exists (e.g. a `_pending_promotions`-style set that
    is never cleared on failure), remove it or ensure it is cleared
    immediately when the `promote_to_voter()` future resolves (success or
    failure), so the *next* cycle always re-evaluates from scratch.
  - Verify: unit/integration test — attempt promotion for a learner in a
    full group (fails, per Task 4), assert the learner is still present and
    unmodified in `_configuration.learners()`; then reduce the group's
    voting count by one (simulate a voter departure) and run another
    assessment cycle; assert the same learner is now promoted, with no
    intervening re-admission call. A second test asserts that instead
    opening a vacancy in a *different* group does not promote the blocked
    learner.
  - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 5.6_
  - **Done, confirmed clean.** No `_pending_promotions`-style tracking was
    introduced or found — Task 4's implementation never filters learners by
    prior outcome, so this task required no code change, only the
    confirming tests: `tests/learner_promotion_retry_test.cpp` shows a
    learner blocked in a full group is left untouched, is promoted on a
    later cycle once a same-group voter departs (no re-admission), and stays
    blocked when the vacancy instead opens in a different placement group.

---

## Phase 4: Tests (Tasks 6–9)

- [x] 6. `tests/learner_admission_capacity_test.cpp` (new file)
  - Formalizes Task 2's verification: blocked at/above target, allowed below
    target, fail-closed on a group absent from `desired_topology`.
  - _Requirements: 6.1, 6.2, 6.4_
  - **Done.** 3 test cases, all passing.

- [x] 7. `tests/learner_promotion_capacity_test.cpp` (new file)
  - Formalizes Task 3's verification: blocked at/above target using the
    voting-only count even when additional learners are present in the same
    group, allowed below target, fail-closed on an absent group.
  - _Requirements: 6.3, 6.4_
  - **Done.** 3 test cases, all passing, using the adjustable-topology mock
    noted under Task 3.

- [x] 8. `tests/quorum_promotion_capacity_fallback_test.cpp` (new file)
  - Formalizes Task 4's verification as a repeatable property/integration
    test.
  - _Requirements: 6.5_
  - **Done.** 2 test cases (prefers promotion; falls back when capacity-blocked),
    both passing. Needed a dynamic mock `quorum_manager` — a health report that
    stays static after a promotion actually succeeds otherwise makes the
    leader perceive a permanent, stale deficit and loop on `provision_node()`
    indefinitely, which is a mock-realism issue rather than a production bug.

- [x] 9. `tests/learner_promotion_retry_test.cpp` (new file)
  - Formalizes Task 5's verification: a blocked learner is left unaffected,
    becomes eligible once its own group's voting count drops below target,
    and is unaffected by a vacancy opening in a different group.
  - _Requirements: 6.6, 6.7_
  - **Done.** 2 test cases, both passing, using a "filler voter added via
    `add_server()`" pattern to reach a blocked state validly (a single
    `add_learner()` call can't leave a group simultaneously admissible and
    promotion-blocked, since both checks read the same underlying counts).

---

## Phase 5: Decouple Learner Capacity from Voting Target (Tasks 10–12)

Addendum, added after a user asked for a "3 voters + 3000 learners in one
group" example: with the original design, admission and promotion shared
`target_count` as the only per-group number, so a group with its voting
target already met (as 3-of-3 always is) rejected every learner admission
outright — there was no way to express "small fixed voting core, large
learner population." See requirements.md Requirement 2.4-2.6, design.md
Property 4.

- [x] 10. Add `learner_capacity` to `placement_group_target` and a
      `learner_count_in_group()` helper
  - In `include/raft/quorum_management.hpp`, add
    `std::optional<std::size_t> learner_capacity = std::nullopt;` to
    `placement_group_target<GroupId>`. Default `std::nullopt` is
    backward-compatible — every existing construction site uses designated
    initializers, none of which break when a new defaulted field is added.
  - In `include/raft/raft.hpp`, add
    `auto node<Types>::learner_count_in_group(placement_group_id_type group) const -> std::size_t`,
    counting `_configuration.learners()` whose placement matches `group`
    (mirrors the existing `voting_count_in_group()`).
  - Verify: unit test constructing a `placement_group_target` without
    setting `learner_capacity` asserts it's `std::nullopt`; a second test
    setting it asserts the value round-trips.
  - _Requirements: 2.4_
  - **Done.** Field and helper added as planned. Confirmed the backward-compat
    claim concretely: rebuilt and re-ran `quorum_management_test`,
    `quorum_leader_integration_test`, `docker_quorum_manager_test`, and
    `aws_quorum_manager_unit_test` (the four suites the research turned up as
    constructing `placement_group_target`/`desired_topology`) — all pass
    unchanged, no source edits needed in any of them.

- [x] 11. Refactor the capacity check into
      `group_has_admission_capacity()` / `group_has_promotion_capacity()`
  - Replace the single `group_has_capacity(group, live_count)` helper with:
    a shared private `find_group_target(group) -> std::optional<placement_group_target<...>>`
    lookup; `group_has_admission_capacity(group)`, which uses
    `learner_count_in_group(group) < *target.learner_capacity` when
    `learner_capacity` is set, else falls back to
    `voting_and_learner_count_in_group(group) < target.target_count`; and
    `group_has_promotion_capacity(group)`, which always uses
    `voting_count_in_group(group) < target.target_count` regardless of
    `learner_capacity`.
  - Update the three call sites: `add_learner()` (use
    `group_has_admission_capacity()`), `promote_to_voter()` (use
    `group_has_promotion_capacity()`), and `run_quorum_assessment()`'s
    learner-candidate scan (use `group_has_promotion_capacity()`).
  - Verify: re-run `tests/learner_admission_capacity_test.cpp`,
    `tests/learner_promotion_capacity_test.cpp`,
    `tests/quorum_promotion_capacity_fallback_test.cpp`, and
    `tests/learner_promotion_retry_test.cpp` unchanged — none of them set
    `learner_capacity`, so the fallback path must reproduce every existing
    assertion exactly.
  - _Requirements: 2.5, 2.6, 3.4_
  - **Done.** All four pre-existing learner test files pass unchanged after
    the refactor, confirming the fallback path is byte-for-byte equivalent
    to the old `group_has_capacity()` for every caller that doesn't set
    `learner_capacity`.

- [x] 12. `tests/learner_capacity_decoupling_test.cpp` (new file)
  - Formalizes Requirement 6.8: `target_count = 3` met by 3 voters,
    `learner_capacity` set independently and larger; learners admit
    successfully up to `learner_capacity` despite the voting group being
    full; the next admission past `learner_capacity` fails; promotion in
    the same group still enforces `target_count`, unaffected by
    `learner_capacity`.
  - _Requirements: 6.8_
  - **Done.** 1 test case, passing. Uses 3 real, networked voters (needed
    since commit requires an actual voting majority) but plain, never-started
    integer IDs for the learners — a learner never needs to be reachable for
    its admission entry to commit, since learners are never counted toward
    quorum, so the test demonstrates the "3 voters + many learners" scenario
    without spinning up thousands of node processes.

---

## Notes

- This plan originally assumed `add_learner()`, `promote_to_voter()`, the
  `_configuration.learners()` set, `_placement_map`, `_quorum_manager`, and
  the leader's automatic promotion-vs-provisioning policy loop already
  existed, treating them as separate, prerequisite work. They did not exist
  in the codebase at implementation time (only `add_server()`/
  `remove_server()` and a 3-value `server_state` did) and were built as part
  of executing this plan rather than deferred. No new `server_state` value
  was added — learner status is purely a `cluster_configuration` membership
  property, so a learner runs the ordinary follower code paths
  (`handle_append_entries()`, `apply_committed_entries()`) unmodified.
- A correctness issue surfaced while building the prerequisite and was fixed
  before it could ship: every place that reconstructs a `cluster_configuration`
  during a joint-consensus transition (`add_server()`, `remove_server()`, and
  the C_new append inside `apply_committed_entries()`) had to be updated to
  carry the current `_learners` set forward explicitly. Aggregate
  initialization silently defaults a field it isn't given, so without this
  fix any unrelated `add_server()`/`remove_server()` call would have wiped
  out all learners cluster-wide the moment it committed.
- Task 1 has no dependency on anything else in this plan and is a good
  first task.
- Tasks 2 and 3 are independent of each other (different entry points) but
  each depends on Task 1.
- Task 5 is deliberately about *removing or avoiding* state, not adding it —
  the natural implementation of Task 4 (a fresh per-cycle scan over current
  learners) already satisfies the retry requirement for free. Task 5 exists
  to catch the tempting-but-wrong alternative: tracking "already tried and
  failed" per learner, which would need explicit invalidation logic and is
  unnecessary complexity here. Confirmed clean — see Task 5's outcome note.
- Validation: 5 new test files / 29 test cases all pass, alongside 24
  targeted regression tests (membership-change, elections, quorum, bootstrap,
  snapshots) and the full 183-target non-infrastructure suite. One unrelated,
  pre-existing flaky performance benchmark
  (`folly_concept_wrappers_integration_test`, no dependency on any file this
  plan touched) tripped once under the parallel build/test load and passed
  3/3 in isolation.
- Do not add `## Notes` content to the per-task entries; keep task bodies
  focused on what to do and how to verify it.
