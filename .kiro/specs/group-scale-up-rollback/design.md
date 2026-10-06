# Design Document

## Overview

A timed-out scale-up is undone by removing the instance it caused, by id,
with the cloud's decrementing removal call. A blind shrink survives only on
the path where the group holds no fresh instance, and there it is followed
by an audit. Where the cloud offers scale-in protection, adopted members
carry it, so even that path cannot pick a voter.

```
provision_node
  snapshot = list(group, all states)            # R1
  grow(desired + 1)
  poll until adoptable fresh instance or timeout
  on timeout:
    final = list(group, all states)
    plan  = plan_scale_up_rollback(snapshot, final)   # R6, pure
    if plan.remove not empty:
        for id in plan.remove: targeted_remove(id)    # R2
    else:
        set_desired(original)                         # R3.1
        audit = settle_and_diff(snapshot)             # R3.2-3.4
    throw timeout + rollback report                   # R5
  on adoption:
    tag; protect (ASG/ESS/VMSS)                       # R4.1
```

### Key design decision: remove by id rather than change the scale-in policy

Setting the group's termination policy to `NewestInstance` (ASG), the
equivalent `RemovalPolicies` (ESS) or `scaleInPolicy.rules=[NewestVM]`
(VMSS) would make a blind shrink pick the new instance. It is rejected:

- It rewrites operator-owned group configuration that other tooling may
  rely on.
- "Newest" is still a guess. A replacement launched by the cloud's own
  health repair can be newer than our launch.
- OCI and MIG have no such policy, so two of the five would still need the
  by-id path.

Targeted removal names the instance, so the cloud has nothing to choose.
Every group manager already uses the same call in `decommission_node`, so
no new API surface is needed for the main path.

### Key design decision: snapshot difference, not "untagged"

OCI, VMSS and MIG identify the new instance as "no node-id tag". That holds
only while this manager is the sole writer and nobody adds an untagged
instance by hand. Removing an instance is more destructive than adopting
one, so this path requires absence from the pre-growth snapshot, as ESS
already does. The untagged test stays as an extra filter for adoption, so
another cluster's tagged instances in a shared group are never adopted.

### Key design decision: shrink when nothing fresh exists

With no fresh instance listed, the group's actual membership equals the
snapshot and the desired size is one above it. Restoring the desired size
asks the cloud to cancel the launch it has not started, so there is
nothing for it to choose. The remaining risk is a race: the cloud lists
the instance after our final listing but before the shrink lands. The audit
(R3.2) catches that case and reports it, and protection (R4) removes it on
the three clouds that have protection. The alternative, holding the
desired size and polling longer, turns one timeout into an unbounded wait
and still needs a fallback.

### Key design decision: protection is non-fatal on adoption

A failed `SetInstanceProtection` after a successful launch and tag leaves a
working node without protection. Failing the provision would mean removing
a healthy node and launching another, which costs more and fails the same
way on a permission problem. The miss is logged and repaired by the next
startup reconciliation (R4.3).

## Components and Interfaces

### Shared planner (`include/raft/group_scale_rollback.hpp`)

No cloud SDK, no I/O. Header-only, in `namespace kythira::group_rollback`.

```cpp
enum class member_state { live, pending, terminal };

struct listed_member {
    std::string id;
    member_state state;
};

struct rollback_plan {
    // Fresh, non-terminal instances to remove by id (R2.1-2.2).
    std::vector<std::string> remove;
    // True when nothing fresh is removable and the desired size must be
    // restored by a capacity write (R3.1).
    bool restore_desired_size = false;
};

[[nodiscard]] auto plan_scale_up_rollback(
    const std::vector<std::string>& pre_growth_ids,
    const std::vector<listed_member>& final_listing) -> rollback_plan;

struct audit_result {
    std::vector<std::string> lost_members;   // R3.3
    std::vector<std::string> late_arrivals;  // R3.4
};

[[nodiscard]] auto audit_after_shrink(
    const std::vector<std::string>& pre_growth_ids,
    const std::vector<listed_member>& final_listing,
    const std::vector<listed_member>& after_shrink) -> audit_result;

// Formats the clause R5.2 appends to the timeout message.
[[nodiscard]] auto describe(const rollback_plan&, const audit_result*,
                            const std::vector<std::pair<std::string, std::string>>&
                                removal_failures) -> std::string;
```

Rules:

- `remove` = ids in `final_listing`, not in `pre_growth_ids`, state not
  `terminal`, in listing order, deduplicated.
- `restore_desired_size` = `remove.empty()`.
- `lost_members` = pre-growth ids that were non-terminal in
  `final_listing` and are absent from `after_shrink` or `terminal` in it.
  A member that was already leaving before the shrink is not the
  rollback's doing.
- `late_arrivals` = non-terminal ids in `after_shrink` that are not
  pre-growth.

### Per-manager state mapping

| Manager | live | pending | terminal |
|---|---|---|---|
| ASG | `InService`, `Standby` | `Pending*`, `Warmed:*` | `Terminating*`, `Terminated`, `Detach*` |
| ESS | `InService`, `Protected`, `Standby` | `Pending`, `Adding` | `Removing` |
| OCI | `RUNNING` | `PROVISIONING`, `STARTING` | `TERMINATING`, `TERMINATED`, `STOPPING`, `STOPPED` |
| VMSS | `provisioningState` `Succeeded` | `Creating`, `Updating` | `Deleting` |
| MIG | `currentAction` `NONE` | `CREATING`, `CREATING_WITHOUT_RETRIES`, `VERIFYING`, `RECREATING` | `DELETING`, `ABANDONING` |

`STOPPED` is terminal for OCI because a stopped pool member does not serve
and OCI pools do not restart it. Any string a manager does not recognise
maps to `pending`: still a member, removable only when fresh. ESS's
`Removing:Wait` is terminal (`Removing*`), and its `Pending:Wait` and
`Stopped` fall to `pending`. Task 1's full state lists are below.

### Vendor behaviour (task 1, 2026-10-03)

Read from the API references, or for Alibaba and OCI from the vendors'
generated SDK docstrings (`alibabacloud_ess20220222`, `oci` 2.187.1), whose
help pages the sandbox proxy blocks.

| Question | Answer | Effect here |
|---|---|---|
| ASG: does `TerminateInstanceInAutoScalingGroup` remove a protected instance? | Yes. The scale-in protection page lists it among what protection does not block. | No clear-before-terminate (task 6.3). |
| ASG: is it accepted on `Pending`? | Not documented. It can fail with `ScalingActivityInProgress` and does not apply to warm-pool instances. | Task 6 retries on `ScalingActivityInProgress` within `provision_timeout`. |
| ASG: terminate a launch a lifecycle hook holds in `Pending:Wait` | Refused with `ScalingActivityInProgress` until the hook completes or its heartbeat expires (default one hour). Observed in real run 37475165001 (2026-10-06), where retrying for `provision_timeout` left the desired size grown. | Lower the desired capacity by one, then `CompleteLifecycleAction(ABANDON)` on every launch hook. Lowering first keeps the group from replacing the abandoned launch. |
| ASG: `SetInstanceProtection` batch | 50 ids. | Batches of 50. |
| ASG lifecycle | `Pending*`, `Quarantined`, `InService`, `Terminating*`, `Terminated`, `Detaching`, `Detached`, `EnteringStandby`, `Standby`, `ReplacingRootVolume*`, `RootVolumeReplaced`, `Warmed:*` | Matches the table above; `Quarantined` and `ReplacingRootVolume*` fall to `pending`. |
| ESS: `RemoveInstances` on `Protected` | Allowed. It is the documented way to remove a protected member by hand. | No clear before decommission; Requirement 4.4 needs nothing more. |
| ESS: `RemoveInstances` during a scaling activity | Refused. The prerequisite is "no scaling activity is in progress". The error code is not documented. | `try_remove` first waits for no `InProgress` activity, bounded by `provision_timeout` (Requirement 2.4). |
| ESS: `SetInstancesProtection` | Moves the instance to `Protected`. Only `InService` or `Stopped` instances qualify. Version 2014-08-28 takes `InstanceId.N`. The batch limit is not documented. | Batches of 20; reconcile protects `InService` members only. |
| ESS lifecycle | `InService`, `Pending`, `Pending:Wait`, `Protected`, `Standby`, `Stopped`, `Removing`, `Removing:Wait` | As mapped above. |
| VMSS Flexible: per-VM protection | Supported from API 2023-09-01. It is set with PUT `.../virtualMachineScaleSets/{vmss}/virtualMachines/{id}` and `properties.protectionPolicy.protectFromScaleIn`. | Task 7.3 uses that call and API version. |
| VMSS: delete a protected VM | "User-initiated instance operations (including instance delete) are not blocked." Deleting a `Creating` VM is not documented. | No clear before delete. |
| MIG: `deleteInstances` | Lowers `targetSize` by the number deleted. Rejects only non-members and instances already being deleted or abandoned. `CREATING` is not listed as refused. | Task 8 as designed, with `skipInstancesOnValidationError=true`. |
| MIG `currentAction` | `ABANDONING, CREATING, CREATING_WITHOUT_RETRIES, DELETING, NONE, RECREATING, REFRESHING, RESTARTING, RESUMING, STARTING, STOPPING, SUSPENDING, VERIFYING` | Unlisted ones fall to `pending`. |
| OCI: detach while `SCALING`, or on `PROVISIONING` | Not documented. The `409 IncorrectState` while `SCALING` was observed live and is handled by `await_pool_running`. | Unchanged. |
| OCI: victim of a size decrease | Balanced across ADs, then fault domains; within a fault domain, the **oldest** goes first. | The mock's oldest-first model matches the documented order. |
| OCI lifecycle | `MOVING, PROVISIONING, RUNNING, STARTING, STOPPING, STOPPED, CREATING_IMAGE, TERMINATING, TERMINATED` | As mapped above. |

### Per-manager rollback calls

| Manager | Final listing | Targeted removal | Settle before removal |
|---|---|---|---|
| ASG | `DescribeAutoScalingGroups` instances | `TerminateInstanceInAutoScalingGroup(decrement=true)` | none |
| ESS | `describe_members()` | `RemoveInstances(DecreaseDesiredCapacity=true)` (`try_remove`) | wait until no `DescribeScalingActivities` entry is `InProgress` (task 1: refused during a scaling activity) |
| OCI | `describe_pool_instances()` | existing `best_effort_detach` (`isDecrementSize`, `isAutoTerminate`) | `await_pool_running` (already inside `best_effort_detach`) |
| VMSS | `scale_set_vms()` | `POST .../delete` with `instanceIds` | none known; task 1 checks a delete on a `Creating` member |
| MIG | `listManagedInstances` | `deleteInstances` with `skipInstancesOnValidationError=true` | wait for the resize operation (already awaited) |

ESS and OCI already have best-effort removal helpers for the post-launch
tag-failure path, so their change is mostly routing the timeout path
through them. ASG, VMSS and MIG gain a small `best_effort_remove(id)` with
the same shape: one call, idempotent on "already gone", never throws.

### Settle window for the shrink audit

`settle = min(provision_timeout, max(3 * poll_interval, 30 s))`. The audit
polls at `poll_interval` and stops early once two consecutive listings
match. It only reports; it never mutates.

### Scale-in protection

| Manager | Set | Clear before decommission | Startup reconcile |
|---|---|---|---|
| ASG | `SetInstanceProtection(ids, ProtectedFromScaleIn=true)` | not needed: task 1 found protection does not block `TerminateInstanceInAutoScalingGroup` | in the constructor block that checks `HealthCheckType`; adopted means the EC2 instance carries this cluster's `kythira:cluster` and a `kythira:node-id` tag |
| ESS | `SetInstancesProtection(ProtectedFromScaleIn=true)` | not needed: task 1 found `RemoveInstances` is the documented way to remove a `Protected` member, and clearing first would expose it to scale-in | in the constructor |
| VMSS | `PUT .../virtualMachineScaleSets/{vmss}/virtualMachines/{name}` at api-version 2023-09-01 with `properties.protectionPolicy.protectFromScaleIn=true` (task 1.4) | not needed: task 1 found user-initiated deletes are not blocked | in the constructor's Flexible-mode check; a member the VM list already shows protected is skipped |

Startup reconcile reads the group once, filters to members tagged for
`cluster_name`, and protects those that are not protected. It runs the
call in batches where the API takes a list (ASG up to 50 ids, ESS up to 20).
Reconcile failure throws `std::invalid_argument` from the constructor only
when the cause is a permission error, matching how the other startup checks
fail; other errors are logged.

ESS consequence: a protected ESS instance reports lifecycle `Protected`, not
`InService`. `is_live()` (`alibaba_ess_quorum_manager.hpp:817-819`) accepts
only `InService` today and would mark every protected node dead. It must
accept `Protected` in the same change that sets protection.

ESS also skips health checks on `Protected` instances. That matches what
the ASG manager requires of its group (`HealthCheckType == EC2`, no
load-balancer-driven replacement) and closes part of audit Q8 for ESS as a
side effect. The README says so.

## Error message shape

```
<existing timeout text>; rollback: removed i-0abc (fresh, Pending)
<existing timeout text>; rollback: desired size restored to 3; lost member i-0def (node 7) during rollback
<existing timeout text>; rollback: removal of i-0abc failed: <cloud error>; desired size left at 4
```

The prefix each manager adds today (`aws_asg_quorum_manager::provision_node:`
and so on) is unchanged.

## Testing Strategy

1. `tests/group_scale_rollback_unit_test.cpp`, registered unconditionally:
   - fresh pending instance → remove it, no shrink;
   - fresh terminal plus nothing else → shrink;
   - two fresh (cloud replaced a failed launch) → remove both non-terminal;
   - pre-growth pending instance (the ASG `existing_ids` bug) → never removed;
   - empty final listing (list failed) → shrink;
   - audit: a pre-growth member gone → reported; a late arrival → reported,
     not removed;
   - property: `remove` never intersects `pre_growth_ids`.
2. Mock servers (`tests/oci_mock_server.hpp`, `tests/alibaba_mock_server.hpp`):
   a size decrease without a named instance removes the **oldest** member,
   and a launch can be held in a pending state. The existing
   `a_provision_timeout_rolls_the_pool_size_back` keeps passing; new cases
   assert the seeded members survive.
3. ESS mock: protection state and the `Protected` lifecycle, and a
   `RemoveInstances` refusal for protected instances so the clear-first
   path is exercised.
4. Real cloud, one case per ASG, VMSS and MIG suite: a launch template whose
   instance cannot pass the adoption bar within a short
   `provision_timeout`, then assert the voter instances are untouched and
   the desired size is back. These follow the project's real-cloud dispatch
   and cost rules and are not run on every PR.

## Risks

- **A fresh instance that is really another actor's.** If an operator or
  another automation scales the same group during our provision, its
  instance is fresh by our definition and would be removed. Today's blind
  shrink has the same effect plus the voter risk, so this is no worse. The
  README documents one manager per group.
- **Protection blocks the operator.** Manual scale-in of a protected group
  will skip Raft members. That is the intent; the README explains how to
  clear it.
- **OCI and MIG keep the race.** Without protection, R3.2's audit is a
  report, not a prevention. The window is one listing-to-shrink round trip.
