# Design Document

## Overview

This design adds a single placement-capacity criterion, evaluated at two
points, on top of Kythira's (assumed) learner mechanism: whether admitting a
joining node as a learner, or promoting an existing learner to a full voting
member, would keep the affected placement group's node count consistent with
the **desired topology** the leader already tracks via
`_quorum_manager.topology() -> desired_topology<GroupId>`
(`include/raft/quorum_management.hpp`). It does not design the learner
mechanism itself (the `learners` set on `cluster_configuration`, election
exclusion, replication, or the `add_learner()`/`promote_to_voter()` entry
points) — those are treated as an existing prerequisite. It is not about
authorization or node identity — no `membership_manager` change is involved.

Both criteria share one "find this group's target" lookup, but compare
against it differently — and, for admission, MAY compare against an
independent field instead:

```
find_group_target(group_id) :=
    lookup(_quorum_manager.topology().groups, group_id)   // std::nullopt if absent

promotion_criterion(learner) :=
    target := find_group_target(group_of(learner))
    if target is absent: false                            // fail closed
    else: voting_count_in_group(group) < target.target_count

admission_criterion(candidate) :=
    target := find_group_target(group_of(candidate))
    if target is absent: false                             // fail closed
    else if target.learner_capacity has a value:
        learner_count_in_group(group) < *target.learner_capacity
    else:
        voting_and_learner_count_in_group(group) < target.target_count
```

`learner_capacity` is a new, optional field on `placement_group_target<GroupId>`
(`include/raft/quorum_management.hpp`) — see Component 1 and Data Models. It
lets a group's voting ceiling (`target_count`, always enforced by promotion)
and its learner population be sized completely independently: a 3-node
voting core can declare `learner_capacity = 3000` to admit up to 3000
read-scaling learners without loosening the 3-voter ceiling, which the old
shared-`target_count` design (where voters and learners competed for the
same slots) could not express — with 3 voters already occupying a
`target_count = 3` group, the shared-target comparison always evaluates
`3 < 3 = false` for every learner, regardless of how many are wanted.

## Architecture

```
   add_learner(candidate)                      promote_to_voter(learner)
          │                                              │
          ▼                                              ▼
  group_has_admission_capacity(group)           group_has_promotion_capacity(group)
    learner_capacity set?                         always: voting_count < target_count
      yes → learner_count < learner_capacity
      no  → voting+learner count < target_count
          │                                              │
          ▼ pass/fail                                    ▼ pass/fail
  (existing admission mechanism,                (existing promotion mechanism,
   out of scope here)                            out of scope here)

Both read the same source of truth:

  _quorum_manager.topology() -> desired_topology<GroupId>
        { groups: [{group_id, target_count, learner_capacity?}, ...] }
  _placement_map: NodeId -> GroupId               (existing, per-node placement)
  _configuration.nodes() / _configuration.learners()   (existing, live membership)
```

## Components and Interfaces

### 1. Capacity checks on `node<Types>`

A shared private helper looks up a group's declared target (or `nullopt` if
undeclared); two small, distinctly-named functions build on it, each
encapsulating its own comparison:

```cpp
template<raft_types Types>
auto node<Types>::find_group_target(placement_group_id_type group) const
    -> std::optional<kythira::placement_group_target<placement_group_id_type>> {
    auto desired = _quorum_manager.topology();
    auto it = std::find_if(desired.groups.begin(), desired.groups.end(),
                            [&](const auto& g) { return g.group_id == group; });
    if (it == desired.groups.end()) return std::nullopt;
    return *it;
}

template<raft_types Types>
auto node<Types>::group_has_admission_capacity(placement_group_id_type group) const -> bool {
    auto target = find_group_target(group);
    if (!target) return false;  // fail closed: no declared target for this group
    if (target->learner_capacity) {
        return learner_count_in_group(group) < *target->learner_capacity;
    }
    return voting_and_learner_count_in_group(group) < target->target_count;
}

template<raft_types Types>
auto node<Types>::group_has_promotion_capacity(placement_group_id_type group) const -> bool {
    auto target = find_group_target(group);
    if (!target) return false;
    return voting_count_in_group(group) < target->target_count;
}
```

Two of the three live-count helpers predate this extension; `learner_count_in_group`
is new, sitting alongside them, all derived from `_placement_map` (existing:
maps `node_id_type` to `placement_group_id_type`) and `_configuration`:

```cpp
template<raft_types Types>
auto node<Types>::voting_count_in_group(placement_group_id_type group) const -> std::size_t {
    return std::count_if(_configuration.nodes().begin(), _configuration.nodes().end(),
                          [&](const auto& id) { return placement_of(id) == group; });
}

template<raft_types Types>
auto node<Types>::learner_count_in_group(placement_group_id_type group) const -> std::size_t {
    return std::count_if(_configuration.learners().begin(), _configuration.learners().end(),
                          [&](const auto& id) { return placement_of(id) == group; });
}

template<raft_types Types>
auto node<Types>::voting_and_learner_count_in_group(placement_group_id_type group) const
    -> std::size_t {
    return voting_count_in_group(group) + learner_count_in_group(group);
}
```

(`placement_of(id)` looks up `_placement_map`; the pattern already used
inside `build_quorum_cluster_vector()`.)

### 2. Wiring into `add_learner()` (admission capacity criterion)

Near the top of `add_learner()`'s precondition block, before any log entry is
appended:

```cpp
auto group = placement_of(candidate);
if (!group_has_admission_capacity(group)) {
    return make_failed_future(learner_capacity_exceeded_exception{group});
}
```

### 3. Wiring into `promote_to_voter()` (promotion capacity criterion)

Symmetrically, before any log entry is appended:

```cpp
auto group = placement_of(learner);
if (!group_has_promotion_capacity(group)) {
    return make_failed_future(voting_capacity_exceeded_exception{group});
}
```

### 4. Leader automatic policy integration

Wherever the leader's quorum-maintenance loop currently decides to prefer
promoting a learner over provisioning a new node for a placement group, it
SHALL call the same `promote_to_voter()` entry point (Component 3) rather
than recomputing the comparison separately. When that call fails on the
capacity criterion, the loop falls back to its existing `provision_node()`
path for that group's remaining deficit — the learner it declined to
promote is left untouched: it is not removed, marked, or otherwise recorded
as "blocked." Automatic learner admission via `ClusterJoin` goes through
`add_learner()` (Component 2) unchanged, so it picks up the admission
criterion with no separate wiring.

**No "blocked learner" bookkeeping is introduced.** Because
`group_has_promotion_capacity()` (Component 1) is a pure function of the
leader's *current* `_configuration` and *current* `_quorum_manager.topology()`, a
learner that failed the promotion criterion on one assessment cycle needs no
special tracking to be reconsidered later: every cycle, the leader's
quorum-maintenance loop simply re-scans learners in each group with a
voting deficit and re-evaluates the criterion for them from scratch. A
learner that was blocked because its group was full becomes an ordinary
promotion candidate again the moment that same, live check passes — e.g.
because a voter departed the group, or the group's `target_count` grew.
This also means a vacancy in group B can never make a learner in group A
eligible: the re-scan is always scoped to the learner's own group, because
`group_has_promotion_capacity()` takes that group's own target and own live
count.

## Data Models

`placement_group_target<GroupId>` (`include/raft/quorum_management.hpp`)
gains one new field:

```cpp
template<typename GroupId>
struct placement_group_target {
    GroupId group_id;
    std::size_t target_count;
    std::optional<std::size_t> learner_capacity = std::nullopt;  // NEW
};
```

Default `std::nullopt` is backward compatible: every existing construction
site (`no_op_quorum_manager`, `docker_quorum_manager`,
`aws_ec2_quorum_manager`, `aws_asg_quorum_manager`, and all tests that
predate this field) uses designated initializers or default member
initialization, none of which break when a new defaulted field is added —
they simply get the fallback admission behavior described in Requirement
2.6.

Two exception types, each carrying the offending `placement_group_id_type`
so callers/logs can identify which group was at capacity:

- `learner_capacity_exceeded_exception{group}` — admission rejected because
  `group` has no room: either its voting+learner count already meets
  `target_count` (no `learner_capacity` declared), or its learner-only count
  already meets `learner_capacity` (declared).
- `voting_capacity_exceeded_exception{group}` — promotion rejected because
  `group`'s voting count is already at or above its `target_count`
  (`learner_capacity`, if any, plays no role here — Requirement 3.4).

Both exceptions are also raised (with the same group) when the group is
absent from `desired_topology` — fail-closed is indistinguishable from
"already at target" from the caller's point of view; both mean "no room in
this group right now."

## Correctness Properties

### Property 1: Admission and promotion share one target lookup

**Validates: Requirements 1.1, 1.3, 1.4**

`find_group_target()` is the single function that reads
`_quorum_manager.topology()`; both `group_has_admission_capacity()` and
`group_has_promotion_capacity()` (Component 1) call it with identical
target-lookup logic and only diverge in what they compare against the
result. There is no second, divergent implementation of "look up this
group's declared target."

### Property 2: Fail-closed on undeclared groups

**Validates: Requirements 1.2**

A placement group with no entry in `desired_topology.groups` always fails
both criteria — there is no code path that treats an absent target as
"unbounded."

### Property 3: Admission and promotion counts differ intentionally

**Validates: Requirements 2.1, 3.1**

Admission counts voting+learner nodes in the group by default (so learners
already staged to fill the deficit block further admission once the deficit
is covered); promotion counts voting-only nodes in the group (so a group can
still promote a caught-up learner even while other learners for the same
group exist, as long as the *voting* count hasn't reached target). This
difference is deliberate, not an oversight — a test should specifically
assert both counts are computed differently (Requirement 6.3).

### Property 4: `learner_capacity` decouples learner population from voting ceiling

**Validates: Requirements 2.4, 2.5, 2.6, 3.4**

When a group declares `learner_capacity`, admission compares a learner-only
count against it instead of voting+learner against `target_count` — so a
fully-staffed voting group (voting count already at `target_count`) can
still admit learners up to `learner_capacity`, independent of how many
voters exist. Promotion is never affected: `group_has_promotion_capacity()`
always reads `target_count`, never `learner_capacity`, so a group's voting
ceiling holds regardless of how large its learner population grows.

### Property 5: Automatic and manual paths share one criterion evaluation

**Validates: Requirements 4.1, 4.3**

Because the leader's automatic quorum-maintenance policy calls the same
`add_learner()` / `promote_to_voter()` entry points a manual/API caller
would use, there is exactly one place where each criterion is evaluated —
no duplicated or divergent logic between automatic and manual paths.

### Property 6: A blocked learner's eligibility is recomputed, never stored

**Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5**

Because `group_has_promotion_capacity()` reads only the leader's current
`_configuration` and current `_quorum_manager.topology()` (Property 1), a
learner that fails the promotion criterion carries no persistent "blocked"
marker. The next assessment cycle evaluates it exactly as if it were being
considered for the first time. This means: (a) a learner is never removed or
altered by a failed attempt, (b) it automatically becomes eligible the
instant its own group's live count drops below target, and (c) a vacancy in
a different group cannot affect it, since the check only ever reads that
learner's own group's target and live count.

## Error Handling

- Both entry points fail fast, before appending any log entry, when the
  capacity criterion fails — consistent with their existing
  precondition-check structure for other failure modes (not-leader,
  already-present, etc.).
- The leader's automatic policy treats a capacity-criterion failure as an
  ordinary, expected outcome, not a fault: it falls back to provisioning
  when promotion is blocked, logging at a level appropriate for a routine
  capacity decision rather than an error.

## Testing Strategy

- `tests/learner_admission_capacity_test.cpp` — admission blocked at/above
  target (voting+learner count), admission allowed below target, fail-closed
  on an undeclared group (Requirements 6.1, 6.2, 6.4).
- `tests/learner_promotion_capacity_test.cpp` — promotion blocked at/above
  target (voting-only count) even with additional learners present in the
  group, promotion allowed below target, fail-closed on an undeclared group
  (Requirements 6.3, 6.4).
- `tests/quorum_promotion_capacity_fallback_test.cpp` — leader's automatic
  policy falls back to provisioning when the promotion capacity criterion
  blocks the only eligible learner in a group (Requirement 6.5).
- `tests/learner_promotion_retry_test.cpp` — a learner blocked by capacity
  stays in `_configuration.learners()` unaffected; once a voter departs its
  group (opening a vacancy), a later assessment cycle promotes that same
  learner with no re-admission step; a vacancy in a different group does
  not affect it (Requirements 6.6, 6.7).
- `tests/learner_capacity_decoupling_test.cpp` — `target_count = 3` already
  met by 3 voters, `learner_capacity` set independently (e.g. 5): learners
  admit successfully up to `learner_capacity` despite the voting group being
  full; the next admission attempt past `learner_capacity` fails; promotion
  in the same group still enforces `target_count`, unaffected by
  `learner_capacity` (Requirement 6.8).
