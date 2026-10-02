# Implementation Plan

## Overview

Make the five group managers undo a timed-out scale-up by removing the
instance they launched, by id, and protect adopted members from the cloud's
scale-in choice where the cloud allows it. See `requirements.md` for the
gap and `design.md` for the planner and per-cloud calls.

## Tasks

- [ ] 1. Confirm vendor behaviour before coding against it
  - [ ] 1.1 Record each provider's exact lifecycle strings for the design's
    state-mapping table, from the API reference
  - [ ] 1.2 ASG: does `TerminateInstanceInAutoScalingGroup` remove an
    instance with `ProtectedFromScaleIn=true`, and does it accept a
    `Pending` instance?
  - [ ] 1.3 ESS: is `RemoveInstances` refused for a `Protected` instance,
    and while the group is scaling? Does `SetInstancesProtection` move the
    lifecycle to `Protected`?
  - [ ] 1.4 VMSS (Flexible): is per-VM `protectFromScaleIn` supported, and
    does the scale set `delete` remove a protected or `Creating` member?
  - [ ] 1.5 MIG: does `deleteInstances` accept a `CREATING` instance and
    lower `targetSize` by one?
  - [ ] 1.6 Write the answers into the design's tables; where the answer
    changes a requirement, amend the requirement in the same commit
  - _Requirements: 2.4, 4.4, 4.6_

- [ ] 2. Shared planner
  - [ ] 2.1 Add `include/raft/group_scale_rollback.hpp` with
    `plan_scale_up_rollback`, `audit_after_shrink` and `describe`
  - [ ] 2.2 Add `tests/group_scale_rollback_unit_test.cpp` with the design's
    cases and the disjointness property; register it outside every cloud
    gate in `tests/CMakeLists.txt`
  - _Requirements: 6.1, 6.2, 6.3_

- [ ] 3. Mock servers model the cloud's choice
  - [ ] 3.1 OCI and Alibaba mocks: a blind size decrease removes the oldest
    member; a launch can be held pending
  - [ ] 3.2 Alibaba mock: `SetInstancesProtection`, the `Protected`
    lifecycle, and a `RemoveInstances` refusal for protected instances
    (as task 1.3 found)
  - _Requirements: 8.1_

- [ ] 4. Alibaba ESS manager
  - [ ] 4.1 Route the timeout path through the planner and the existing
    `best_effort_remove`; audit after a shrink
  - [ ] 4.2 Protect on adoption; reconcile at construction; clear before
    `decommission_node` if required; accept `Protected` in `is_live()`
  - [ ] 4.3 Mock tests: fresh instance removed and seeded members kept;
    empty listing shrinks; protection set and decommission still works
  - _Requirements: 1.1, 2.1-2.5, 3.1-3.4, 4.1-4.6, 5.1-5.2, 8.2-8.4_

- [ ] 5. OCI instance pool manager
  - [ ] 5.1 Snapshot every instance id before `set_pool_size`; require
    absence from it for adoption
  - [ ] 5.2 Route the timeout path through the planner and
    `best_effort_detach`; audit after a shrink
  - [ ] 5.3 Mock tests as 4.3, minus protection; keep
    `a_provision_timeout_rolls_the_pool_size_back` green
  - _Requirements: 1.1, 1.3, 2.1-2.5, 3.1-3.4, 4.7, 5.1-5.2, 8.2, 8.3_

- [ ] 6. AWS ASG manager
  - [ ] 6.1 Snapshot every lifecycle state, not only `InService`
  - [ ] 6.2 Add `best_effort_remove`; route the timeout path through the
    planner; audit after a shrink
  - [ ] 6.3 `SetInstanceProtection` on adoption; reconcile in the
    constructor's startup check; clear before terminate if task 1.2
    requires it
  - [ ] 6.4 Add `autoscaling:SetInstanceProtection` to
    `scripts/ci-cloud-credentials/aws/policies/asg-quorum-manager.json`
    (lands with or after PR #381) and to the documented IAM policy
  - _Requirements: 1.1, 1.2, 2.1-2.5, 3.1-3.4, 4.1-4.5, 5.1-5.2, 7.1_

- [ ] 7. Azure VMSS manager
  - [ ] 7.1 Snapshot member names before the capacity `PATCH`; require
    absence from it for adoption
  - [ ] 7.2 Add `best_effort_remove`; route the timeout path through the
    planner; audit after a shrink
  - [ ] 7.3 Protection on adoption and reconcile, as task 1.4 allows
  - _Requirements: 1.1, 1.3, 2.1-2.5, 3.1-3.4, 4.1-4.5, 5.1-5.2, 7.3_

- [ ] 8. GCP MIG manager
  - [ ] 8.1 Snapshot managed instances before `resize`; require absence
    from it for adoption
  - [ ] 8.2 Add `best_effort_remove` via `deleteInstances`; route the
    timeout path through the planner; audit after a shrink
  - [ ] 8.3 Report the rollback `resize` result instead of discarding it
  - _Requirements: 1.1, 1.3, 2.1-2.5, 3.1-3.4, 4.7, 5.1-5.4, 7.4_

- [ ] 9. Real-cloud timeout cases
  - [ ] 9.1 Add one timeout-rollback case each to the ASG, VMSS and MIG
    real-cloud suites; dispatch under the real-cloud cost rules
  - _Requirements: 8.5_

- [ ] 10. Documentation
  - [ ] 10.1 Provider READMEs: rollback behaviour, scale-in protection and
    how to clear it, one manager per group
  - [ ] 10.2 Point the five provider specs' timeout-rollback criteria to
    this spec; update the Alibaba RAM, Azure role and GCP permission docs
  - _Requirements: 4.7, 7.2, 7.3, 7.4, 7.5_

## Notes

- Tasks 4-8 are independent once 2 and 3 land and can ship as separate
  PRs. ESS and OCI go first because their mocks make the change testable
  offline.
- The ASG policy change in 6.4 edits a file PR #381 adds; rebase on main
  after #381 merges.
