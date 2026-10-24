# Implementation Plan

A real-tier sweep of `max_segment_bytes` for the object-store segmented log
(`.kiro/specs/object-store-batched-layout/`), and the default it supports.
Tasks 1–3 are code and mock-tier tests and need no approval (done). Task 4 is a paid
real-cloud run and needs Clark's go; task 5 depends on its numbers.

- [x] 1. Follower liveness under a slow run
  - `tests/raft_object_store_segment_liveness_test.cpp` (mock, CTest): a
    three-node cluster whose follower's engine sleeps longer than
    `election_timeout_max` inside one `append_log_entries`, with a healthy
    leader heartbeating it; assert on the follower's term and on whether the
    leader resent the run
  - Record in design.md which bound applies (campaign or not), and replace
    the "Requirement 7" section's two branches with the one that holds
  - If the follower campaigns, say so in the PR and raise it with Clark as
    open question 1; do not change `node` in this spec
  - _Requirements: 7.1, 7.2_
  - Done: the follower's timer fires on release, but its PreVote round is
    refused and its term never moves; no resend. Bound: run persist time <
    `append_entries_timeout`. Open question 1 closed

- [x] 2. Sweep cases and the mock smoke run
  - `sweep_grid`, the body-size helper that encodes with the engine's own
    format, `KYTHIRA_SEGMENT_SWEEP` reporting, `sweep_enabled()`
  - Cases 12–15 in `tests/object_persistence_real_cases.hpp`; case 11's driver
    factored to take options and a payload size, with case 11's own output
    unchanged
  - Wire the cases into all five provider suites (Alibaba unfenced for case
    14, as it is for the existing fenced cases)
  - `object_persistence_real_cases_mock_test` runs them with the reduced grid;
    a run with the opt-in unset passes every sweep case with the skip message
  - _Requirements: 1.1-1.4, 2.1, 3.1, 3.2, 4.1, 5.1, 5.2, 6.1, 9.2, 9.5, 10.1_

- [ ] 3. Workflow opt-in
  - `KYTHIRA_SEGMENT_SWEEP: ${{ vars.REAL_CLOUD_SEGMENT_SWEEP }}` on each
    object-persistence bundle step in `.github/workflows/real-cloud-tests.yml`;
    the run plan job prints whether the sweep is on
  - No new `workflow_dispatch` input (the workflow is at GitHub's limit of 25)
  - _Requirements: 9.1, 9.2_

- [ ] 4. Real-cloud sweep — needs Clark's go
  - S3 first, both architectures, with `REAL_CLOUD_SEGMENT_SWEEP=true` set for
    that dispatch only and cleared after; ask again before Azure, GCS, OCI and
    Alibaba
  - Check the run's request count and bytes against the design's cost table
  - Not checked off against the mock tier
  - _Requirements: 9.3, 9.4, 10.3_

- [ ] 5. Decision and documentation
  - `doc/cloud_object_persistence.md`: the sweep table beside the existing
    latency tables, with run ids; the limits-table row and the "Future work"
    bullet for `max_segment_bytes` rewritten from the measurements; a
    per-provider recommendation table if Requirement 8.5 applies
  - Apply Requirement 8. A changed default is its own commit, citing the run
    ids, with a `doc/CHANGELOG.md` entry and the engine header's comment on
    `max_segment_bytes` updated to match
  - Tick object-store-batched-layout task 10's "revisit the
    `max_segment_bytes` default" line by pointing to this task
  - _Requirements: 8.1-8.5, 10.2_
