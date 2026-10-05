# Implementation Plan

## Overview

Make the five group managers undo a timed-out scale-up by removing the
instance they launched, by id, and protect adopted members from the cloud's
scale-in choice where the cloud allows it. See `requirements.md` for the
gap and `design.md` for the planner and per-cloud calls.

## Tasks

- [x] 1. Confirm vendor behaviour before coding against it
  - [x] 1.1 Record each provider's exact lifecycle strings for the design's
    state-mapping table, from the API reference
  - [x] 1.2 ASG: does `TerminateInstanceInAutoScalingGroup` remove an
    instance with `ProtectedFromScaleIn=true`, and does it accept a
    `Pending` instance?
  - [x] 1.3 ESS: is `RemoveInstances` refused for a `Protected` instance,
    and while the group is scaling? Does `SetInstancesProtection` move the
    lifecycle to `Protected`?
  - [x] 1.4 VMSS (Flexible): is per-VM `protectFromScaleIn` supported, and
    does the scale set `delete` remove a protected or `Creating` member?
  - [x] 1.5 MIG: does `deleteInstances` accept a `CREATING` instance and
    lower `targetSize` by one?
  - [x] 1.6 Write the answers into the design's tables; where the answer
    changes a requirement, amend the requirement in the same commit
  - _Requirements: 2.4, 4.4, 4.6_

- [x] 2. Shared planner
  - [x] 2.1 Add `include/raft/group_scale_rollback.hpp` with
    `plan_scale_up_rollback`, `audit_after_shrink` and `describe`
  - [x] 2.2 Add `tests/group_scale_rollback_unit_test.cpp` with the design's
    cases and the disjointness property; register it outside every cloud
    gate in `tests/CMakeLists.txt`
  - _Requirements: 6.1, 6.2, 6.3_

- [x] 3. Mock servers model the cloud's choice
  - [x] 3.1 OCI and Alibaba mocks: a blind size decrease removes the oldest
    member; a launch can be held pending
  - [x] 3.2 Alibaba mock: `SetInstancesProtection`, the `Protected`
    lifecycle, and a `RemoveInstances` refusal for protected instances
    (as task 1.3 found)
  - _Requirements: 8.1_

- [x] 4. Alibaba ESS manager
  - [x] 4.1 Route the timeout path through the planner and the existing
    `best_effort_remove`; audit after a shrink
  - [x] 4.2 Protect on adoption; reconcile at construction; clear before
    `decommission_node` if required; accept `Protected` in `is_live()`
  - [x] 4.3 Mock tests: fresh instance removed and seeded members kept;
    empty listing shrinks; protection set and decommission still works
  - _Requirements: 1.1, 2.1-2.5, 3.1-3.4, 4.1-4.6, 5.1-5.2, 8.2-8.4_

- [x] 5. OCI instance pool manager
  - [x] 5.1 Snapshot every instance id before `set_pool_size`; require
    absence from it for adoption
  - [x] 5.2 Route the timeout path through the planner and
    `best_effort_detach`; audit after a shrink
  - [x] 5.3 Mock tests as 4.3, minus protection; keep
    `a_provision_timeout_rolls_the_pool_size_back` green
  - _Requirements: 1.1, 1.3, 2.1-2.5, 3.1-3.4, 4.7, 5.1-5.2, 8.2, 8.3_

- [x] 6. AWS ASG manager
  - [x] 6.1 Snapshot every lifecycle state, not only `InService`
  - [x] 6.2 Add `best_effort_remove`; route the timeout path through the
    planner; audit after a shrink
  - [x] 6.3 `SetInstanceProtection` on adoption; reconcile in the
    constructor's startup check; clear before terminate if task 1.2
    requires it
  - [x] 6.4 Add `autoscaling:SetInstanceProtection` to
    `scripts/ci-cloud-credentials/aws/policies/asg-quorum-manager.json`
    (lands with or after PR #381) and to the documented IAM policy
  - _Requirements: 1.1, 1.2, 2.1-2.5, 3.1-3.4, 4.1-4.5, 5.1-5.2, 7.1_

- [x] 7. Azure VMSS manager
  - [x] 7.1 Snapshot member names before the capacity `PATCH`; require
    absence from it for adoption
  - [x] 7.2 Add `best_effort_remove`; route the timeout path through the
    planner; audit after a shrink
  - [x] 7.3 Protection on adoption and reconcile, as task 1.4 allows
  - _Requirements: 1.1, 1.3, 2.1-2.5, 3.1-3.4, 4.1-4.5, 5.1-5.2, 7.3_

- [x] 8. GCP MIG manager
  - [x] 8.1 Snapshot managed instances before `resize`; require absence
    from it for adoption
  - [x] 8.2 Add `best_effort_remove` via `deleteInstances`; route the
    timeout path through the planner; audit after a shrink
  - [x] 8.3 Report the rollback `resize` result instead of discarding it
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
- Tasks 1-5 landed together (PR #459). The ESS mock refuses
  `RemoveInstances` during an in-progress scaling activity rather than for
  `Protected` members, per task 1.3. `describe()` takes a
  `rollback_outcome`, which carries the removals, the restore and the
  audit together, rather than the three separate arguments sketched in
  the design.
- Tasks 6-8 landed together. The timeout sequence (plan, remove or
  restore, audit) moved into `group_rollback::execute_rollback`, so the
  five managers supply only their cloud calls. ASG and VMSS gained mock
  tests over SDK client doubles (`aws_asg_quorum_manager_mock_test`,
  `azure_vmss_quorum_manager_mock_test`); the MIG cases extend
  `gcp_quorum_manager_unit_test` and its fakes. Each double removes the
  oldest unprotected member on a blind shrink, so the old path visibly
  costs a voter. No documented IAM policy lists the ASG actions beyond the
  CI bundle, which 6.4 updated; Azure's `Virtual Machine Contributor`
  already covers the protection `PUT`.
- Task 9's three cases are written; 9 stays open until each has passed a
  real-cloud run. ASG holds the fresh launch in `Pending:Wait` with a
  launch lifecycle hook, so its case is deterministic and the CI bundle
  gained `autoscaling:PutLifecycleHook`. VMSS (12s) and MIG (5s) rely on a
  `provision_timeout` shorter than the member's boot, with the fresh member
  already listed when it expires. Run 37257996571 measured a Flexible
  member listed about 6s after the capacity PATCH and adoptable 24-29s
  after it. The MIG cases have not run in CI yet: the workflow sets no
  `GCP_TEST_MIG_A`, and no script provisions the MIG.
