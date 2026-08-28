# Implementation Plan — Elastic Shard Capacity

## Status: 0/17 tasks complete — specification only, no implementation commits

**Last Updated**: August 28, 2026. Written against the tree at
`multi_raft_impl.hpp:1682` (children inherit the parent's voters),
`multi_raft_impl.hpp:3083` (the heartbeat/operator channel),
`multi_raft_impl.hpp:1240` (lazy replica creation), `quorum_management.hpp:172`
(the ten-implementation provider seam) and `raft.hpp:1326` (the single-group
provisioning loop this feature deliberately does *not* reuse for shards).

## Overview

Add elastic capacity to multi-Raft: a policy layer, a single-writer controller
that drives any `quorum_manager`, and a placement-driver adapter that delivers
the result through the channel the host already speaks. Three small changes to
the consensus core (one type alias, one enum member, one counter); no new
concept method on `quorum_manager`; no new RPC; no provider SDK anywhere in the
new code.

Read before starting:

- `.kiro/specs/elastic-shard-capacity/requirements.md` — Requirement 1 is the
  provider-neutrality contract every task is measured against.
- `.kiro/specs/elastic-shard-capacity/design.md` — §1 (the four facts), §4 (the
  intent state machine), §9 (rejected alternatives; read before "improving" the
  split path).
- `.kiro/specs/multi-raft/design.md` §5 (split), §6 (signals), §7 (placement
  driver).
- `include/raft/quorum_management.hpp` — the concept, and
  `no_op_quorum_manager` as the shape every default in this feature copies.
- `include/raft/split_merge_policy.hpp` — the policy shape `capacity_policy`
  mirrors, including why `validate()` rejects a non-hysteretic configuration.
- `CLAUDE.md` — commit messages, copyright headers, and the
  Docker/rootless-Podman rule that binds task 15.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 2, 3],
      "description": "Unblock the composition in the type system, add the split capacity gate and host counters, and land the pure value types the rest of the feature is written against"
    },
    {
      "wave": 2,
      "tasks": [4, 5, 6, 7],
      "description": "The pieces with no dependency on each other: the default policy, the ledger, the lease, and the deterministic mock manager every later test needs"
    },
    {
      "wave": 3,
      "tasks": [8],
      "description": "The controller core: inventory, step(), bounds, kill switch, dry run, executor and backoff"
    },
    {
      "wave": 4,
      "tasks": [9, 10],
      "description": "Reconciliation and orphan reaping; placement-group selection"
    },
    {
      "wave": 5,
      "tasks": [11, 12, 13],
      "description": "Acting on the cluster: admission, scale-in drain, and the placement-driver adapter that delivers both"
    },
    {
      "wave": 6,
      "tasks": [14, 15],
      "description": "Observability, then the property and failover suites that depend on everything above"
    },
    {
      "wave": 7,
      "tasks": [16, 17],
      "description": "End-to-end acceptance on Docker, optional per-provider live verification, and documentation"
    }
  ]
}
```

---

- [ ] 1. Unblock host composition (`include/raft/multi_raft.hpp`)
  - Shadow `quorum_manager_type` in `group_scoped_types` with
    `no_op_quorum_manager<node_id_type, address_type, std::string>`, with the
    comment stating both reasons: `create_group_impl` default-constructs it
    (`multi_raft_impl.hpp:177-192`) and no real manager is default-constructible
    (`aws_ec2_quorum_manager.hpp:145`); and a thousand groups must not hold a
    thousand provisioning authorities.
  - Derive the address parameter through the same traits helper `node_config`
    uses (`raft.hpp:74`, `raft.hpp:176`), never `Types::address_type` — a
    bundle is not required to declare one.
  - Do **not** add a `quorum_manager` member to `multi_raft_config`. Hosts do
    not provision.
  - Verify: a compile-only test instantiates `multi_raft` with a `Types` bundle
    naming a non-default-constructible mock manager — which fails to compile
    today — and a `static_assert` pins that the per-group node sees the no-op.
    The existing multi-raft suites pass unmodified.
  - _Requirements: 15.1, 15.6, 1.3_

- [ ] 2. Split capacity gate and host accounting (`multi_raft.hpp`, `multi_raft_impl.hpp`)
  - Add `arbiter_gate::capacity` with its `to_string` arm, and
    `split_capacity_floor_bytes` (`std::optional`, unset ⇒ gate off) to
    `multi_raft_config`.
  - Evaluate the gate where the other gates are evaluated, from
    `capacity_probe`, for every channel — policy, admin, hint and driver alike.
    Merges are exempt.
  - Surface the refusal in the shard report so a controller sees it without a
    second channel, and count `split.rejected{gate=capacity}` on the existing
    counter.
  - Add `shard.allocation.suggestion_ignored`, incremented when an allocation
    carries `_suggested_voters`/`_suggested_learners` the host cannot apply
    (design.md §9, rejected alternative 2).
  - Verify: unit tests for gate on/off, each channel, the merge exemption, and
    the counter; a regression test that a host with **zero** groups still emits
    node heartbeats.
  - _Requirements: 6.1-6.6, 15.2, 15.4, 15.5, 2.3_

- [ ] 3. Capacity value types and the `capacity_policy` concept (`include/raft/capacity_policy.hpp`)
  - `capacity_reason`, `capacity_evidence`, `capacity_decision<GroupId>` with
    factory-only construction so an inconsistent decision is unrepresentable,
    and `cluster_capacity_snapshot<NodeId, GroupId, Key>` assembled purely from
    existing `node_report` / `shard_report` / `quorum_health` / `desired_topology`
    data.
  - The `capacity_policy` concept: `evaluate`, `cooldown`, `validate`,
    `get_validation_errors`. Document that a policy is leader-only, may be
    non-deterministic, and may not do I/O — with the reasoning, as
    `split_merge_policy` does.
  - No new field on `node_report` or `shard_report`. If a decision cannot be
    made without one, stop and amend design.md first.
  - Verify: `static_assert`s for a conforming and a deliberately
    non-conforming policy; round-trip and ordering tests on the value types.
  - _Requirements: 2.1, 2.2, 3.1-3.6_

- [ ] 4. `threshold_capacity_policy` (`include/raft/capacity_policy.hpp`)
  - Five signals, each with a scale-out and a scale-in watermark (design.md §3),
    `sustained_for` / `sustained_for_scale_in`, `min_cluster_size` /
    `max_cluster_size` awareness, and disabled-by-default configuration.
  - `validate()` **rejects** watermark pairs closer than
    `min_hysteresis_margin`, in the shape of
    `threshold_split_merge_policy::validate()`'s oscillation guard, and reports
    every error.
  - Split pressure: the split/merge rate window, the projection
    (`current + rate × horizon / nodes`), and the short circuit that skips
    projection when `gate=capacity` refusals are present. Record the three
    projection inputs in the evidence.
  - Verify: table-driven tests for each signal's out/in thresholds; a test that
    a single spike does not trigger while a sustained crossing does; explicit
    tests that non-hysteretic and inverted configurations are rejected with
    named errors; projection arithmetic against hand-computed values.
  - _Requirements: 4.1-4.7, 5.2, 5.3, 5.5, 5.6_

- [ ] 5. The provisioning ledger (`include/raft/capacity_ledger.hpp`)
  - The `capacity_ledger` concept and the intent record: idempotency key,
    decision and evidence, target placement group, deadlines, state.
  - The intent state machine of design.md §4 as an explicit transition table,
    with every deadline expiry having a defined next state, and record-before-act
    enforced by the API shape (the call that returns a token is the one that
    persisted it).
  - Two implementations: a replicated state machine on the coordination group
    (shipped default) and a file-backed one for single-controller deployments,
    plus retention-based compaction.
  - Verify: every transition and every expiry path unit-tested; a crash-injection
    test that a process killed between `record` and the provider call leaves a
    `requested` intent that reconciliation can resolve; retention compaction
    keeps the ledger bounded under a long synthetic run.
  - _Requirements: 8.1, 8.2, 8.5-8.8_

- [ ] 6. The lease (`include/raft/capacity_lease.hpp`)
  - The `capacity_lease` concept (`held()`, `fencing_token()`), with `held()`
    returning `false` whenever the answer is unknown.
  - The shipped implementation over Raft leadership of a nominated coordination
    group, following `raft.hpp:1498`'s start-on-`become_leader`,
    stop-on-anything-else discipline; plus a single-process implementation for
    embedded use that documents its assumption instead of pretending to check it.
  - Verify: unit tests that leadership loss flips `held()` before the next step,
    that an indeterminate state reads `false`, and that the fencing token is
    monotonic across failovers.
  - _Requirements: 7.1-7.4, 7.6_

- [ ] 7. Deterministic mock quorum manager (`tests/mock_capacity_quorum_manager.hpp`)
  - Satisfies `quorum_manager` with a `static_assert`; programmable per call:
    latency, failure, stock-out per placement group, partial success (created
    but unreachable), and never-joins; injectable clock; a record of every call
    for assertions.
  - Verify: its own unit test; no `sleep` anywhere in it or its users.
  - _Requirements: 16.1, 1.2_

- [ ] 8. Controller core (`include/raft/elastic_capacity_controller.hpp`)
  - The class template of design.md §4 with its four concept constraints, the
    inventory (latest report per node with age, per-shard reports, rate
    windows), `build_snapshot()`, and the non-blocking `step()` sequence.
  - Bounds enforced in the controller, not the policy: `max_cluster_size`,
    `max_in_flight_intents`, `min_provider_call_interval`, the rolling-window
    budget — each refusal counted and logged by name. Kill switch honoured
    within one evaluation interval. `dry_run` decides, logs and counts without
    calling anything.
  - Provider I/O on the controller's own executor with deadlines, exponential
    backoff with jitter reusing the existing retry machinery, and a circuit that
    suspends scale-out after consecutive failures. Never provision on
    `quorum_status::lost`; never scale in unless `healthy`. Stale node reports
    are `unknown`, not absent.
  - Verify: unit tests with the task-7 mock for each bound, the kill switch, dry
    run, the `lost`/`healthy` rules, staleness handling, backoff and circuit
    opening; a test asserting `step()` never blocks on a pending provider future
    and never calls a provider inline.
  - _Requirements: 2.4, 2.5, 3.5, 12.1-12.6, 13.1-13.4, 13.7_

- [ ] 9. Reconciliation and orphan reaping
  - The startup/failover sequence of design.md §7: open intents, `assess_quorum`,
    match by idempotency key then node id then join deadline, resolve each to
    `completed` / `orphaned` / `failed`; no new provider call until it finishes
    or its own deadline expires.
  - Reaping calls `decommission_node` and relies on the concept's idempotency
    guarantee rather than local bookkeeping.
  - Verify: with the mock, every match path and every mismatch path; a test that
    a manager carrying no metadata still reaps by join deadline; a test that
    reaping twice is harmless.
  - _Requirements: 7.5, 8.3, 8.4, 8.6, 13.5_

- [ ] 10. Placement-group selection
  - The scoring function of design.md §8 over `topology()` as floor and shape,
    with the under-target bonus, the decaying refusal penalty, and a
    deterministic tie-break; never a group absent from `topology()`; never
    reducing a group below its declared target.
  - Refusal fallback to the next-best group, with both the refusal and the
    fallback recorded.
  - Optional `resizable_quorum_manager` refinement detected with `requires`, so
    group-capacity managers can be kept in step, with a logged note (not a
    failure) where it is absent.
  - Verify: table-driven scoring tests including ties, stock-out fallback across
    all groups, and a `static_assert`-backed test that the refinement is used
    when present and skipped when not.
  - _Requirements: 1.5, 9.1-9.5_

- [ ] 11. Admission and the rebalance planner
  - Plan generation: candidate selection by the decision's reason, exclusion of
    non-`stable` shards, shards with down/pending replicas, and shards in
    cooldown; move caps per target and cluster-wide, further reduced by the
    snapshot counters in `node_report`.
  - Per-shard sequence `add_replica{as_learner=true}` → catch-up → promote →
    `remove_replica` → optional `transfer_leader`, never removing before
    promoting; abandonment at any step without leaving joint configuration.
  - Treat `skipped_operator_reason` as feedback: back off on `shard_busy`,
    re-plan on `stale_epoch` / `not_leader`.
  - Mark an intent `completed` only when the machine holds a voting replica.
  - Verify: unit tests for selection and every exclusion; a multi-group
    integration test on the existing multi-raft test fabric that a planned move
    ends with the target holding a voting replica and the group never below
    quorum; a test that operator skips produce backoff or re-plan, not resend.
  - _Requirements: 10.1, 10.3-10.8, 13.6_

- [ ] 12. Scale-in and drain
  - Drain: transfer leadership away, remove replicas one group at a time, then
    `decommission_node`; at most one machine draining cluster-wide; every
    refusal rule of Requirement 11.3-11.4 enforced before each removal, not once
    at the start.
  - Deadline expiry returns the machine to service and closes the intent as
    `abandoned`; a half-drained machine is never decommissioned.
  - Disabled by default, independently of scale-out.
  - Verify: unit and fabric tests for each refusal rule, the one-at-a-time
    bound, deadline abandonment and return to service, and that a group is never
    taken below quorum by a drain.
  - _Requirements: 11.1-11.6_

- [ ] 13. `elastic_shard_placement_driver` (`include/raft/elastic_shard_placement_driver.hpp`)
  - The decorator of design.md §5: `allocate_shard_ids` straight through to the
    inner driver; heartbeats fed to the controller with
    `inner ∪ controller` operators, inner first, duplicates dropped;
    `report_split` / `report_merge` as trigger edges; `lookup_descriptor` served
    from the descriptors the controller has already seen.
  - `static_assert` that the adapter satisfies `shard_placement_driver` when
    wrapping `no_op_shard_placement_driver`.
  - Verify: unit tests that a split re-evaluates immediately rather than at the
    next interval; that an inner driver's operator wins a conflict; that a
    controller failure degrades to the inner driver's behaviour;
    that `lookup_descriptor` lets a host with no local row materialise a replica
    (exercising `multi_raft_impl.hpp:1240-1300`).
  - _Requirements: 5.1, 10.2, 13.7_

- [ ] 14. Observability
  - Counters, gauges and the one-record-per-decision structured log of
    design.md §11, under the existing `kythira.multiraft.*` taxonomy; the split
    gate reuses `split.rejected{gate}` rather than adding a metric; no provider
    identity in metric names.
  - Verify: a test asserting every terminal intent state, every bound refusal
    and every decision reason emits its counter; a test that the decision record
    contains reason, signals, projection inputs, chosen group, idempotency key
    and bounds in one line.
  - _Requirements: 14.1-14.5_

- [ ] 15. Property and failover suites
  - Properties over a simulated cluster with the task-7 mock: size within
    [floor, ceiling]; no group loses quorum through an admission or a drain;
    live provider machines ≤ recorded intents; no non-`stable` shard is moved.
  - Failover: kill the lease holder in each intent state and assert the
    successor reconciles to exactly one machine per intent with orphans reaped.
  - Chaos: provider never returns; machine boots but never joins; lease lost
    mid-admission.
  - Verify: the suites run deterministically under the injectable clock, with no
    wall-clock sleeps, and are wired into CTest with the existing labels.
  - _Requirements: 16.2, 16.3, 16.4, 16.7_

- [ ] 16. End-to-end acceptance on Docker
  - A scenario test using `docker_quorum_manager`: a loaded cluster splits,
    crosses the density watermark, provisions a container, admits it, and ends
    with shards on it — with no cloud credentials present.
  - Must run under both Docker and rootless Podman: `container_runtime()` /
    `compose_prefix()`, service names rather than static IPs, no `--privileged`
    (CLAUDE.md).
  - Optional per-provider live tests, one per provider, behind the existing
    credential and label gating, each running the same scenario and each ending
    with a post-run leak audit that fails the job if any resource it created
    still exists.
  - Verify: the Docker leg passes in CI on a credential-free runner; each live
    leg, where run, ends with a clean audit.
  - _Requirements: 1.6, 16.5, 16.6_

- [ ] 17. Documentation
  - `doc/elastic_shard_capacity.md`: the operating envelope first (design.md
    §14 — the 10–30 minute lag, the snapshot-transfer cost of every move, what
    the feature cannot do), then the provider parity table, the full
    configuration surface with defaults and their derivation, the metric and log
    taxonomy, and a dry-run-first adoption procedure.
  - State plainly that a split never waits for capacity and never fails for lack
    of it, and document the residual failure modes — the orphan window, a lease
    lost mid-call, the cost of a premature scale-in — without softening them.
  - One README entry pointing at it, in the shape of the cloud-object-persistence
    entry, with the same "read the envelope before adopting" framing.
  - Verify: every knob in the document exists in code and every code knob is in
    the document; the parity table names no provider as default or reference.
  - _Requirements: 1.7, 17.1-17.5_
