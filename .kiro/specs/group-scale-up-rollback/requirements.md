# Requirements Document

## Introduction

Five quorum managers grow a cloud-managed group to add a Raft node:

| Manager | Group | Grow call | Timeout rollback today |
|---|---|---|---|
| `aws_asg_quorum_manager` | Auto Scaling group | `UpdateAutoScalingGroup(DesiredCapacity+1)` | `UpdateAutoScalingGroup(DesiredCapacity=orig)` (`aws_asg_quorum_manager.hpp:383-390`) |
| `alibaba_ess_quorum_manager` | ESS scaling group | `ModifyScalingGroup(DesiredCapacity+1)` | `ModifyScalingGroup(DesiredCapacity=orig)` (`alibaba_ess_quorum_manager.hpp:576-587`) |
| `oci_instance_pool_quorum_manager` | Instance pool | `UpdateInstancePool(size+1)` | `UpdateInstancePool(size=orig)` (`oci_instance_pool_quorum_manager.hpp:587-597`) |
| `azure_vmss_quorum_manager` | Flexible scale set | `PATCH sku.capacity+1` | `PATCH sku.capacity=orig` (`azure_vmss_quorum_manager.hpp:393-405`) |
| `gcp_mig_quorum_manager` | Managed instance group | `instanceGroupManagers.resize(+1)` | `resize(orig)`, result ignored (`gcp_mig_quorum_manager.hpp:400-407`) |

When `provision_timeout` expires before a new instance becomes adoptable,
every one of them shrinks the group's desired size back by one and lets the
cloud decide which instance goes. That decision belongs to the group's own
scale-in policy (ASG `Default`/`OldestInstance`, ESS `RemovalPolicies`, the
VMSS `scaleInPolicy`, OCI's and the MIG's built-in order), none of which
knows which members are Raft voters. A slow launch is the usual reason the
timeout fires, and a slow launch leaves the new instance in the group in a
`Pending`/`PROVISIONING`/`CREATING` state. The shrink then removes one of the
group's members, and nothing guarantees that member is the new one. If the
cloud picks a healthy voter, a remediation step meant to restore quorum
removes a voter instead, and the caller is told only "timeout".

The single-instance managers do not have this problem: `aws_ec2`, Azure VM
and GCP compute delete the instance they launched, by id. The ESS manager
already identifies fresh instances by their absence from a pre-growth
snapshot (`alibaba_ess_quorum_manager.hpp:596-617`) and only uses that
knowledge for the error message.

This spec makes the group managers remove the instance they caused, by id,
and stops the cloud from picking a voter on the one path where no fresh
instance can be identified. It is the parity-audit gap Q2
(`audits/parallel-implementation-parity-audit.md`, item 9 of the summary).

### Verified against main

Checked on `origin/main` at `9c738d2` (2026-10-02). PR #381 (ASG real-cloud
tests) changes only `decommission_node` in the ASG manager; the provision
path and its rollback are untouched there, so this spec does not conflict
with it.

### Non-goals

- The single-instance managers (`aws_ec2`, `azure_vm`, `gcp_compute`) and
  `docker_quorum_manager`. They already remove their own launch.
- Changing a group's scale-in or termination policy. That is the operator's
  group configuration and other tooling may depend on it.
- The `decommission_node` paths. They already remove a named instance with a
  decrementing call on every group manager.
- Timeout diagnostics (audit Q11) and decommission wait semantics (audit
  Q5). They touch the same functions and can follow separately.

## Glossary

- **Group manager**: one of the five managers in the table above.
- **Pre-growth snapshot**: the set of every instance id in the group, in any
  lifecycle state, read immediately before the grow call.
- **Fresh instance**: an instance in the group's final listing whose id is
  not in the pre-growth snapshot.
- **Terminal state**: a lifecycle state from which the instance will not
  serve again (ASG `Terminating*`/`Terminated`, ESS `Removing`, OCI
  `TERMINATING`/`TERMINATED`, VMSS `Deleting`, MIG `DELETING`/`ABANDONING`).
- **Targeted removal**: removing a named instance with the cloud's call that
  also lowers the desired size by one (ASG
  `TerminateInstanceInAutoScalingGroup(ShouldDecrementDesiredCapacity=true)`,
  ESS `RemoveInstances(DecreaseDesiredCapacity=true)`, OCI
  `DetachInstancePoolInstance(isDecrementSize, isAutoTerminate)`, VMSS
  `POST .../delete` with `instanceIds`, MIG
  `instanceGroupManagers.deleteInstances`).
- **Blind shrink**: lowering the desired size without naming an instance,
  which is what every group manager does today.
- **Scale-in protection**: a per-instance flag the cloud honours when it
  chooses scale-in victims (ASG `ProtectedFromScaleIn`, ESS
  `SetInstancesProtection`, VMSS `protectionPolicy.protectFromScaleIn`). OCI
  instance pools and GCP MIGs have no equivalent.
- **Adopted member**: an instance this manager has tagged or labelled with a
  `kythira` node id for its cluster.

## Requirements

### Requirement 1: Snapshot every lifecycle state before growing

**User Story:** As an operator, I want the manager to know exactly which
instances existed before it grew the group, so that it can tell the
instance it caused apart from everything else.

#### Acceptance Criteria

1. Before its grow call, every group manager SHALL record the pre-growth
   snapshot: every instance id the group lists, in every lifecycle state.
2. The ASG manager SHALL stop restricting its snapshot to `InService`
   instances. Today a pre-existing `Pending` instance, such as one left by
   an earlier timed-out provision, is absent from `existing_ids` and can be
   adopted as if this call had launched it.
3. The OCI, VMSS and MIG managers SHALL require a candidate to be absent
   from the pre-growth snapshot in addition to their existing "no node-id
   tag or label" test, so an untagged instance that predates the call is
   never adopted or removed by this call.
4. IF the pre-growth listing fails THEN `provision_node` SHALL fail before
   growing the group.

### Requirement 2: Remove fresh instances by id on timeout

**User Story:** As an operator, I want a timed-out scale-up to remove the
instance it launched, so that a healthy voter is never terminated to undo
it.

#### Acceptance Criteria

1. WHEN `provision_timeout` expires without an adoptable instance THEN the
   group manager SHALL list the group once more and compute the fresh
   instances that are not in a terminal state.
2. FOR each such fresh instance the manager SHALL perform a targeted
   removal. It SHALL NOT also lower the desired size, because the targeted
   removal already does.
3. The manager SHALL NOT perform a targeted removal on any instance in the
   pre-growth snapshot from this path.
4. WHERE the cloud refuses a targeted removal while the group is still
   scaling (OCI returns `409 IncorrectState` while the pool is `SCALING`),
   the manager SHALL wait for the group to settle, bounded by
   `provision_timeout`, before removing. The OCI manager's existing
   `best_effort_detach` already does this and SHALL be reused.
5. WHEN a targeted removal reports the instance already gone THEN the
   manager SHALL treat it as removed, using the same idempotency rules as
   that manager's `decommission_node`.

### Requirement 3: No blind shrink while a fresh instance may still appear

**User Story:** As an operator, I want the rollback to stay safe when the
cloud has not yet listed the new instance, so that a late launch cannot
turn the rollback into a voter termination.

#### Acceptance Criteria

1. WHEN the final listing shows no non-terminal fresh instance THEN the
   manager SHALL lower the desired size back to its pre-growth value, as
   today, because the group holds no extra member for the cloud to choose.
2. On that path, after lowering the desired size, the manager SHALL list
   the group again, bounded by a settle window, and SHALL record any
   pre-growth member that has left the group or entered a terminal state.
3. WHEN step 2 finds such a member THEN the exceptional Future SHALL name it
   (instance id and, when tagged, node id) so the caller knows the
   rollback cost a member. This is the race in which an instance appeared
   between the final listing and the shrink.
4. WHEN step 2 finds a fresh instance that appeared after the shrink THEN
   the manager SHALL leave it alone and name it in the error. Removing it
   with a decrementing call would take the group below its pre-growth size.
5. WHERE the cloud offers scale-in protection, Requirement 4 SHALL make the
   race in AC 3 unable to choose an adopted member.

### Requirement 4: Scale-in protection on adopted members

**User Story:** As an operator, I want the cloud's own scale-in logic to
skip Raft members, so that no blind capacity change, from this manager or
anything else, can terminate a voter.

#### Acceptance Criteria

1. WHERE the group supports scale-in protection (ASG, ESS, VMSS), the
   manager SHALL set it on an instance as part of adoption, after the node
   id tag is written and before `provision_node` returns success.
2. A failure to set protection SHALL NOT fail the provision. The instance is
   up and tagged, and failing would churn a working node. It SHALL be
   logged with the instance id, and Requirement 4.3 repairs it.
3. At construction, alongside the existing startup checks (ASG health check
   type, VMSS orchestration mode), the manager SHALL set scale-in
   protection on every adopted member of its cluster that lacks it. This
   covers clusters adopted before this spec.
4. `decommission_node` SHALL still remove a protected member. WHERE the
   cloud refuses to remove a protected instance by id, the manager SHALL
   clear the protection immediately before the removal call.
5. Protection SHALL never be set on an instance this manager has not
   adopted.
6. WHERE setting protection changes how the cloud reports the instance's
   lifecycle (ESS moves it to the `Protected` lifecycle state), the
   manager's liveness and adoption tests SHALL accept that state.
7. OCI instance pools and GCP MIGs have no scale-in protection. Their
   managers SHALL rely on Requirements 2 and 3, and their documentation
   SHALL say so.

### Requirement 5: Error reporting

**User Story:** As an on-call engineer, I want the timeout error to say
what the rollback did, so that I do not have to reconstruct it from the
console.

#### Acceptance Criteria

1. The rollback SHALL stay best-effort: no rollback failure replaces the
   timeout as the reason the Future is exceptional.
2. The error message SHALL state which rollback ran: the instance ids
   removed by id, or "desired size restored to N", plus any member
   Requirement 3.3 found missing and any removal that failed with its
   cloud error.
3. The GCP MIG manager SHALL stop discarding the rollback `resize` result
   (`(void)rollback;`) and SHALL report its failure under AC 2.
4. Each manager SHALL keep its current exception type on this path
   (`gcp_operation_timeout` for MIG, `std::runtime_error` elsewhere).

### Requirement 6: Shared rollback planning

**User Story:** As a maintainer, I want the rollback decision written once
and tested without a cloud, so that five managers cannot drift apart again.

#### Acceptance Criteria

1. The decision from Requirements 2 and 3 (which ids to remove by id, or
   whether to shrink) SHALL be a pure function over the pre-growth
   snapshot and the final listing, in a header with no cloud SDK
   dependency.
2. Each group manager SHALL map its own lifecycle strings to the function's
   state classes (live, pending, terminal) and call it.
3. The function SHALL have unit tests registered outside every cloud
   Kconfig gate, so they run on every build leg.

### Requirement 7: Permissions and documentation

**User Story:** As an operator provisioning credentials, I want the new
calls listed, so that the rollback does not fail on a missing permission.

#### Acceptance Criteria

1. The AWS CI permission bundle and the ASG manager's documented IAM
   policy SHALL add `autoscaling:SetInstanceProtection`.
2. The Alibaba RAM policy documentation SHALL add `ess:SetInstancesProtection`
   (and `ess:RemoveInstances` if any doc omits it).
3. The Azure role documentation SHALL cover writing
   `protectionPolicy` on scale set members.
4. The GCP documentation SHALL cover `compute.instanceGroupManagers.deleteInstances`.
5. The timeout-rollback acceptance criteria in the five provider specs
   (aws-quorum-manager 13.6, azure-cloud-services 13.6,
   gcp-cloud-services 16.7, oci-cloud-provider 6.7 and
   alibaba-cloud-services 7.2) SHALL point to this spec.

### Requirement 8: Tests

**User Story:** As a maintainer, I want the victim choice tested, so that
a regression that terminates a voter fails CI.

#### Acceptance Criteria

1. The OCI and Alibaba mock servers SHALL model a blind shrink by removing
   the **oldest** member, which is the worst case for a voter, so the
   current behaviour fails the new tests.
2. For OCI and Alibaba, a mock test SHALL launch a fresh instance that
   never becomes adoptable, let `provision_timeout` expire, and assert that
   the fresh instance was removed, every seeded member is still present,
   and the desired size equals its pre-growth value.
3. For OCI and Alibaba, a mock test SHALL cover the empty-listing path
   (Requirement 3.1) and assert the size is restored.
4. For ESS (mock) and ASG, a test SHALL assert that adoption sets scale-in
   protection and that `decommission_node` still removes a protected member.
5. The ASG, VMSS and MIG managers have no offline fake that launches
   instances (audit Q12). Their coverage SHALL be the Requirement 6 unit
   tests plus one real-cloud timeout case each, added to the existing
   real-cloud suites and run under the project's real-cloud cost rules.
