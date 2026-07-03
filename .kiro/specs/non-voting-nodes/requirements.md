# Requirements Document

## Introduction

Kythira's Raft implementation is assumed to already support a **learner**
node role (a "non-voting" server, per Ongaro's thesis §4.2.1) that replicates
the log and applies committed entries like a follower, but never votes and is
never counted toward quorum — reachable through an `add_learner()` entry
point on the leader — and a `promote_to_voter()` entry point that moves a
caught-up learner into the voting set via the existing joint-consensus
protocol. **Neither of these is what this document specifies, and this is
not about authorization or node identity.**

This document specifies a single **capacity criterion** — evaluated at two
points — that checks whether an action would keep the cluster's per-placement-
group node counts consistent with the **desired cluster placement**: the leader
already has a `quorum_manager` (`include/raft/quorum_management.hpp`) that
exposes `topology() -> desired_topology<GroupId>`, a target voting-node count
per placement group used today to decide when to `provision_node()`. This
document reuses that same target, rather than introducing any new
configuration knob, to gate two additional actions. A group MAY additionally
declare a separate, independent learner capacity (Requirement 2.4-2.6) so
that a group's voting target and its learner population can be sized
independently — e.g. a fixed 3-node voting core with room for thousands of
learners for read-scaling, without inflating the voting ceiling.

(a) **Admission capacity criterion**: whether a newly joining node may be
    accepted as a learner for its placement group — only when that group's
    current node count (voting members plus learners already in the
    pipeline for that group) is below the group's desired target. Admitting
    more learners than are needed to fill the group's deficit doesn't match
    the desired placement.

(b) **Promotion capacity criterion**: whether an existing learner may be
    promoted to a full voting member for its placement group — only when
    that group's current *voting* count is still below the group's desired
    target. Promoting past the target would over-grow that group's voting
    membership.

Both criteria are two evaluations of the same underlying check
(`target_count` for a group, from `desired_topology`, compared against a
live node count for that group), differing only in which count is compared:
admission compares voting+learner count, promotion compares voting-only
count.

## Glossary

- **Learner**: A node that replicates the log and applies committed entries
  but never votes and is never counted toward quorum. Assumed to already
  exist as a concept; out of scope here.
- **Placement group**: A failure domain (availability zone, rack, etc.)
  identified by a `GroupId` (`include/raft/quorum_management.hpp`), used to
  partition the desired topology and current node counts.
- **Desired topology**: `desired_topology<GroupId>` — the target voting-node
  count per placement group, already produced by
  `quorum_manager::topology()`.
- **Capacity criterion**: The check — "is the relevant node count for this
  group below its target?" — evaluated once for admission (voting+learner
  count vs. `target_count`, or learner-only count vs. `learner_capacity`
  when set) and once for promotion (voting-only count vs. `target_count`,
  always).
- **Learner capacity**: `placement_group_target.learner_capacity` — an
  optional, per-group ceiling on the number of learners, independent of
  `target_count` (the voting ceiling). Absent by default, in which case
  admission falls back to comparing against `target_count` like promotion
  does.

## Requirements

### Requirement 1: Placement-Aware Capacity Check (shared foundation)

**User Story:** As a cluster leader, I need one consistent way to ask "does
this placement group have room to grow?" so that admission and promotion
decisions are never computed by two different, potentially divergent
calculations.

#### Acceptance Criteria

1. The leader SHALL determine a group's target count by calling
   `_quorum_manager.topology()` and looking up the entry for that group in
   the returned `desired_topology<GroupId>.groups`.
2. WHEN a group ID has no corresponding entry in `desired_topology.groups`
   THEN the capacity check for that group SHALL fail closed (treated as
   zero remaining capacity, not as unbounded).
3. The check SHALL be evaluated using the leader's live `_configuration` and
   live `_quorum_manager.topology()` result at the moment of the call, never
   a cached count or a value captured earlier in the process.
4. The check SHALL share one "find this group's declared target" lookup
   (Criteria 1-2) between the admission and promotion paths — both fail
   closed identically on an undeclared group. What each path compares
   against that lookup differs: promotion (Requirement 3) always compares a
   voting-only count against `target_count`; admission (Requirement 2)
   compares against `target_count` using a voting+learner count by default,
   or against a group's independent `learner_capacity` (Requirement 2.4-2.6)
   using a learner-only count when that field is set. This is still one
   shared foundation, not two divergent implementations of "look up a
   group's target."

### Requirement 2: Learner Admission Capacity Criterion

**User Story:** As a cluster leader, I need to admit a joining node as a
learner only when its placement group actually needs more capacity, so that
idle learners don't accumulate beyond what's needed to reach the desired
topology.

#### Acceptance Criteria

1. WHEN a leader evaluates whether to admit a joining node as a learner for
   placement group G THEN it SHALL apply the Requirement 1 check to G using
   the count of nodes in G that are either voting members
   (`_configuration.nodes()`) or learners (`_configuration.learners()`).
2. WHEN that count is already at or above G's `target_count` THEN the
   admission criterion SHALL fail and the node SHALL NOT be admitted as a
   learner.
3. WHEN that count is below G's `target_count` THEN the admission criterion
   SHALL be satisfied.
4. `placement_group_target<GroupId>` (`include/raft/quorum_management.hpp`)
   MAY declare an optional `learner_capacity`, independent of `target_count`
   — a separate ceiling on the number of learners in G, decoupled from G's
   voting target. This lets a group's voting size (a firm ceiling enforced
   unconditionally by Requirement 3, never affected by this field) and its
   learner population be sized independently, e.g. a 3-node voting core
   with room for thousands of learners for read-scaling.
5. WHEN G's `learner_capacity` is set THEN the admission criterion SHALL
   instead compare the count of nodes in G that are learners only
   (`_configuration.learners()`, excluding voting members) against
   `learner_capacity`, in place of Criteria 1-3's voting+learner-vs-
   `target_count` comparison.
6. WHEN G's `learner_capacity` is absent (the default) THEN the admission
   criterion SHALL behave exactly as Criteria 1-3 describe — existing
   deployments and configurations that don't set this field see no behavior
   change.

### Requirement 3: Promotion Capacity Criterion

**User Story:** As a cluster leader, I need to promote a learner to voting
status only when its placement group's voting membership is still below the
desired target, so that promotion cannot grow a group's voting count past
what the desired topology calls for.

#### Acceptance Criteria

1. WHEN a leader evaluates whether to promote a learner to voting status for
   placement group G THEN it SHALL apply the Requirement 1 check to G using
   the count of *voting* nodes in G only (`_configuration.nodes()`,
   excluding learners).
2. WHEN that count is already at or above G's `target_count` THEN the
   promotion criterion SHALL fail and the learner SHALL NOT be promoted.
3. WHEN that count is below G's `target_count` THEN the promotion criterion
   SHALL be satisfied.
4. The promotion criterion SHALL always compare against `target_count`,
   regardless of whether G declares a `learner_capacity` (Requirement 2.4) —
   `learner_capacity` bounds only how many learners may accumulate and has
   no effect on how many may be promoted to voters.

### Requirement 4: Leader Automatic Policy Uses the Same Criterion

**User Story:** As a cluster leader running its automatic quorum-maintenance
policy (choosing whether to promote an existing learner or provision a new
node when a placement group needs another voter), I need that choice to use
the exact same capacity criterion a manual/API-driven promotion would, so
the automatic and manual paths can never disagree about whether a group has
room.

#### Acceptance Criteria

1. WHEN the leader's quorum-maintenance policy selects a learner to promote
   as an alternative to provisioning a new node THEN it SHALL evaluate the
   promotion capacity criterion (Requirement 3) for that learner's group
   before proceeding, using the same check as a direct call to
   `promote_to_voter()`.
2. WHEN the promotion capacity criterion fails (the group is already at
   `target_count`) THEN the leader SHALL fall back to its existing
   provisioning path rather than promoting.
3. WHEN the leader's automatic policy admits a newly-joined node as a
   learner (via `ClusterJoin` or equivalent) THEN it SHALL evaluate the
   admission capacity criterion (Requirement 2) exactly as a direct call to
   `add_learner()` would.

### Requirement 5: A Learner Blocked by Capacity Remains Active and Is Reconsidered Later

**User Story:** As a cluster leader, when a learner is otherwise ready for
promotion but its placement group's voting capacity is already full, I need
the learner to remain an ordinary, active learner — not be removed,
decommissioned, or otherwise penalized — and to be automatically reconsidered
for promotion the next time its group has room, so that a temporarily-blocked
learner is never wasted or permanently passed over.

#### Acceptance Criteria

1. WHEN the promotion capacity criterion (Requirement 3) fails for a learner
   THEN that failure alone SHALL have no side effect on the learner: it
   SHALL remain in `_configuration.learners()` and continue replicating and
   applying committed entries exactly as it did before the attempt.
2. The promotion capacity criterion SHALL be a pure function of the leader's
   live `_configuration` and live `_quorum_manager.topology()` at the moment
   it is evaluated (per Requirement 1.3) — no "blocked" or "rejected" state
   SHALL be recorded for a learner anywhere, so no separate bookkeeping is
   needed to make it eligible again later.
3. WHEN the leader's automatic quorum-maintenance policy runs an assessment
   cycle for a placement group THEN it SHALL evaluate the promotion capacity
   criterion for learners in that group regardless of whether an earlier
   cycle already evaluated (and rejected) the same learner — every cycle is
   an independent, full re-evaluation, not a one-time attempt.
4. WHEN a placement group's voting count later drops below its target (a
   voter is removed, fails, or the desired target increases) THEN any
   learner already present in that group SHALL become eligible for
   promotion on the next assessment cycle that considers it, with no
   re-admission or other explicit action required to restore its
   candidacy.
5. A learner blocked by capacity in group G SHALL only become eligible via a
   vacancy in G itself — a vacancy in a different group SHALL NOT make it
   eligible, since the criterion is always evaluated against the learner's
   own group (Requirement 3.1).
6. The only two ways a learner's candidacy for promotion SHALL end are:
   (a) it is promoted once its group has room, or (b) it is explicitly
   removed/decommissioned via the existing learner-removal entry point
   (assumed prerequisite, out of scope here). The leader's automatic policy
   SHALL NOT otherwise give up on, remove, or stop retrying a learner
   purely because an earlier promotion attempt was blocked by capacity.

### Requirement 6: Tests

**User Story:** As a developer, I need automated tests for both capacity
criteria — including the retry behavior for blocked learners — so that
placement-group target counts are enforced correctly and consistently
across manual and automatic paths.

#### Acceptance Criteria

1. A unit test SHALL verify that admission fails when a group's
   voting+learner count already meets or exceeds `target_count`.
2. A unit test SHALL verify that admission succeeds when a group's
   voting+learner count is below `target_count`.
3. A unit test SHALL verify that promotion fails when a group's voting-only
   count already meets or exceeds `target_count`, even if learners in that
   group are below the voting+learner threshold used for admission.
4. A unit test SHALL verify that a group absent from `desired_topology`
   fails closed for both admission and promotion.
5. A property/integration test SHALL verify that the leader's automatic
   quorum-maintenance policy falls back to provisioning when the promotion
   capacity criterion blocks the only eligible learner in a group.
6. A property/integration test SHALL verify the retry behavior: a learner
   blocked by capacity remains in `_configuration.learners()` and unaffected
   after the blocked attempt; when a voting vacancy subsequently opens in
   the same group (e.g. a voter is removed), a later assessment cycle
   promotes that same learner with no re-admission step.
7. A test SHALL verify that a vacancy opening in a *different* placement
   group does not make a blocked learner in another group eligible.
8. A test SHALL verify Requirement 2.4-2.6: with `target_count` small (e.g.
   3, already met by 3 voters) and `learner_capacity` set independently
   larger (e.g. 10), learners can still be admitted up to `learner_capacity`
   even though the group's voting target is already fully met; the
   `learner_capacity + 1`th admission attempt fails; and promotion in that
   same group still enforces `target_count` unaffected by `learner_capacity`.
