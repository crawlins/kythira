# Requirements Document — Elastic Shard Capacity

## Introduction

This document specifies **elastic capacity for multi-Raft**: the machinery that
turns "this cluster is running out of room for the shards it is producing" into
"a new machine has been provisioned, has joined, and is carrying its share of
the shards" — and the reverse when the shard count falls.

The immediate motivation is the split path. A split does not need a new machine
to *succeed* — `multi_raft_impl.hpp:1682` gives each child the parent's voter
set, so splitting multiplies groups on the machines that already hold the
parent's state — but it does change the thing capacity is measured in. Every
split raises shards-per-node, raises the number of Raft state machines, logs and
snapshots on each machine, and consumes disk to seed each child's synthetic
snapshot. A cluster that splits without ever growing eventually reaches the
point where the next split is refused, or worse, is admitted onto a machine that
cannot write the child's snapshot.

### What exists today, and what does not

Kythira already has both halves of this feature, and no wire between them.

- **The infrastructure half.** `quorum_manager`
  (`include/raft/quorum_management.hpp:172`) models a cluster-scope authority
  with `assess_quorum` / `provision_node` / `decommission_node` / `topology` /
  `maintain_quorum`. Ten implementations satisfy it today — AWS EC2, AWS ASG,
  Azure VM, Azure VMSS, GCP Compute, GCP MIG, OCI Instance Pool, Alibaba ESS,
  Docker, and the no-op — each with a `static_assert` proving conformance, and
  most live-verified against the real service. This concept is the entire
  provider-portability story of this specification, and it already exists.
- **The shard half.** `shard_placement_driver`
  (`include/raft/shard_placement_driver.hpp:388`) is what `multi_raft` actually
  talks to: id allocation, batched shard and node heartbeats, and an advisory
  operator list (`add_replica`, `remove_replica`, `transfer_leader`, `split`,
  `merge`, `scatter`).
- **The gap.** `include/raft/multi_raft_impl.hpp` contains no reference to
  `quorum_manager`. Provisioning exists only in single-group `node<Types>`
  (`raft.hpp:1326` `run_quorum_assessment`, `raft.hpp:1465` `provision_node`),
  it fires on exactly one trigger — a placement group below the target declared
  in `topology()` — and shard count, shard density and split activity are not
  inputs to it. `shard_id_allocation::_suggested_voters` exists in the driver's
  reply and is read by nothing outside a unit test.

There is also a hard compile blocker. `multi_raft::create_group_impl`
(`multi_raft_impl.hpp:177-192`) builds each group's `node_config<group_types>`
without setting `.quorum_manager`, so that member is default-constructed
(`raft.hpp:98`). Every real provider manager has only an explicit
config-taking constructor that validates in its body
(e.g. `aws_ec2_quorum_manager.hpp:145`), so naming one as `Types::quorum_manager_type`
in a bundle handed to `multi_raft` is ill-formed today. Requirement 15 closes
that, and closes it in the direction that is also correct on the merits: a
machine is a host-scope resource, and a thousand groups in one process must not
each hold a provisioning authority.

### The shape of the answer

Provisioning is **asynchronous, off the tick, and never a precondition of a
split**. A provider call takes tens of seconds to minutes; a tick is measured in
milliseconds; and the split does not need the machine anyway. So the sequence is
split → observe the new density → provision → admit the machine → move replicas
onto it, with every step individually safe to abandon.

The decision layer is a new `capacity_policy` concept in the shape of the
existing `split_merge_policy`: a pure function from a snapshot to a decision,
with hysteresis and `validate()`. The actuation layer is a new
`elastic_capacity_controller` that owns a `quorum_manager` (any of the ten) and
a durable ledger. The delivery layer is an `elastic_shard_placement_driver` that
adapts the controller onto the `shard_placement_driver` contract `multi_raft`
already speaks, because that channel — a host calls out, gets operators back —
is the only cluster-scope control channel Kythira has: the transport carries no
client-command RPC and no host-to-host control RPC
(`multi_raft.hpp:757-765`).

### Provider neutrality is a requirement, not an aspiration

Requirement 1 states it as a testable property: no file added by this feature
may include a provider SDK header, the controller may depend on nothing but the
`quorum_manager` concept, and the acceptance suite must pass with
`docker_quorum_manager` and a deterministic mock — on a machine with no cloud
credentials at all. AWS, Azure, GCP, OCI and Alibaba then appear exactly once
each, as rows in a parity table of optional live tests.

## Glossary

- **Capacity** — the machines available to host shards, expressed in the
  placement-group vocabulary of `quorum_management.hpp`: `desired_topology`,
  `placement_group_target`, `placement_group_id`. Never in provider terms.
- **Scale-out / scale-in** — adding or removing *machines*. Distinct from
  split/merge, which adds or removes *shards*.
- **Density** — shards per node, and leaders per node. The primary scale-out
  signal, because it is the quantity a split changes.
- **Controller** — `elastic_capacity_controller`: the single-writer component
  that turns capacity decisions into `quorum_manager` calls and shard operators.
- **Ledger** — the controller's durable record of provisioning intents and
  their outcomes. The thing that makes a provider call idempotent across a
  controller failover.
- **Intent** — one recorded decision to add or remove one machine, with an
  idempotency key, a state, and a deadline.
- **Admission** — the sequence that turns a provisioned machine into a replica
  holder: `add_learner` → catch-up → promote → optional `transfer_leader`.
- **Drain** — the reverse: move leadership and replicas off a machine until it
  holds none, then `decommission_node`.
- **Orphan** — a machine the provider created that never joined the cluster
  within its deadline. Orphans cost money and must be reaped.

## Requirements

---

### Requirement 1: Provider Neutrality

**User Story:** As an operator who may run this on any of five clouds, in
Docker, or on bare metal, I want elastic capacity to depend on the cloud
abstraction Kythira already has, so that adopting it never means adopting a
specific vendor.

#### Acceptance Criteria

1. Every new header this feature adds SHALL depend on the `quorum_manager`
   concept only. No new file SHALL include a provider SDK header, name a
   provider type, or be guarded by a provider feature macro
   (`KYTHIRA_HAS_AWS_SDK` and its siblings).
2. The controller SHALL be a template parameterised on the quorum manager, and
   SHALL compile and pass its full unit suite when instantiated with
   `no_op_quorum_manager`, `docker_quorum_manager`, and a deterministic test
   double.
3. All ten existing `quorum_manager` implementations SHALL be usable unchanged.
   This feature SHALL NOT add a required method to the `quorum_manager` concept.
4. Capacity SHALL be expressed in placement-group terms — "one more node in
   group `X`" — and never in provider terms (instance type, subnet, zone,
   image). Those already live inside each manager's own configuration.
5. Where a provider can express a group's size natively (ASG, VMSS, MIG,
   Instance Pool, ESS), the controller MAY keep that native target in step via
   an **optional** concept refinement detected with `requires`, and SHALL behave
   correctly, with only a logged note, against managers that do not model it.
6. The acceptance suite SHALL pass on a machine with no cloud credentials.
   Provider-live verification SHALL be optional, per-provider, and gated exactly
   as the existing real-cloud tests are.
7. Documentation SHALL present the providers as a parity table, and SHALL NOT
   describe any provider as the default or the reference.

---

### Requirement 2: Capacity Signals

**User Story:** As the controller, I want the inputs I need to already be on the
wire, so that elastic capacity does not add a second telemetry path.

#### Acceptance Criteria

1. The controller SHALL derive its view of the cluster from the existing
   `node_report` and `shard_report` types
   (`shard_placement_driver.hpp:97,158`), which already carry capacity bytes,
   shard count, leader count, snapshot counts, the `_overloaded` flag,
   placement-group labels, per-shard approximate size and key count, and
   down/pending replicas.
2. This feature SHALL NOT add a field to `node_report` or `shard_report` unless
   a stated decision in design.md cannot be made without it.
3. A host with **zero** registered groups SHALL still emit node heartbeats, so
   that a freshly provisioned machine is visible as capacity before it holds any
   shard. A regression test SHALL cover the empty-registry heartbeat path.
4. The controller SHALL treat a node it has not heard from within a configurable
   `node_report_staleness` as unknown rather than absent, and SHALL NOT scale
   out on the strength of stale reports alone.
5. The controller SHALL cross-check its inventory against
   `quorum_manager::assess_quorum`, and SHALL prefer the quorum manager's answer
   about liveness when the two disagree, because that is the party that can see
   a machine that never booted.

---

### Requirement 3: The `capacity_policy` Concept

**User Story:** As a library user, I want to state my own scale-out rule, so
that the shipped thresholds are a default and not a ceiling.

#### Acceptance Criteria

1. A new `capacity_policy` concept SHALL be introduced in
   `include/raft/capacity_policy.hpp`, in the shape of `split_merge_policy`:
   `evaluate(snapshot) -> capacity_decision`, `cooldown()`, `validate()`,
   `get_validation_errors()`.
2. A policy SHALL be a pure function of its argument: no I/O, no mutation of
   cluster state, no blocking. Everything it needs SHALL be in the snapshot it
   is handed.
3. A policy SHALL NOT be required to be deterministic; it runs on one controller
   and its answer is recorded in the ledger before it is acted on.
4. `capacity_decision` SHALL express: hold; scale out by `N` nodes in a named
   placement group; scale in by naming a specific node. It SHALL carry a
   `capacity_reason` (density, storage, load, overload, split_pressure,
   topology_floor, manual) and SHALL be constructible only through factory
   functions that make an inconsistent decision unrepresentable.
5. A policy SHALL be consulted at most once per `evaluation_interval` and SHALL
   NOT be consulted at all while the cluster is in a state where the controller
   would refuse to act (Requirement 13).
6. `validate()` SHALL be called once at construction, and a controller SHALL
   refuse to start on a policy that fails it, reporting every error.

---

### Requirement 4: The Default Threshold Policy

**User Story:** As an operator, I want a default policy whose knobs are the
quantities a split actually changes, and whose defaults cannot oscillate.

#### Acceptance Criteria

1. `threshold_capacity_policy` SHALL ship as the default and SHALL evaluate, at
   minimum: shards per node, leaders per node, storage utilisation
   (`_used_bytes / _capacity_bytes`), the fraction of nodes reporting
   `_overloaded`, and split pressure (Requirement 5).
2. Every signal SHALL have a **separate** scale-out watermark and scale-in
   watermark, and `validate()` SHALL REJECT any configuration whose two
   watermarks are not separated by at least `min_hysteresis_margin`. This
   mirrors the oscillation guard `threshold_split_merge_policy::validate()`
   already applies to split and merge thresholds, on the same reasoning: the
   cost of a misconfigured pair is unbounded.
3. Scale-out SHALL require the triggering signal to have been continuously above
   its watermark for a configurable `sustained_for` duration. A single
   heartbeat's spike SHALL NOT provision a machine.
4. Scale-in SHALL require `sustained_for_scale_in`, defaulting to a materially
   longer duration than `sustained_for`, because the cost asymmetry is real: a
   machine added late costs latency, a machine removed early costs a
   re-provision and two snapshot transfers.
5. The policy SHALL respect `min_cluster_size` and `max_cluster_size`, and
   SHALL NOT propose scaling in below the size implied by
   `quorum_manager::topology()` (Requirement 9.2).
6. The policy SHALL be disabled by default: an unconfigured deployment behaves
   exactly as it does today.
7. Defaults SHALL be documented with their derivation, and SHALL be conservative
   enough that a first adopter's failure mode is "did not scale out soon
   enough", never "provisioned a fleet".

---

### Requirement 5: Split-Driven Scale-Out

**User Story:** As an operator whose shard count is growing, I want the cluster
to acquire machines because splitting is producing shards, not only after the
disks are nearly full.

#### Acceptance Criteria

1. `report_split(parent, children)` — which the host already calls at apply
   time rather than deferring to the next heartbeat
   (`shard_placement_driver.hpp:409-413`) — SHALL be a trigger edge: the
   controller SHALL re-evaluate capacity on it rather than waiting for the next
   evaluation interval.
2. The controller SHALL maintain a split-rate signal per placement group
   (splits per unit time, and net shard growth), and the default policy SHALL be
   able to scale out on **projected** density: the density that the current
   split rate implies at a configurable horizon, not only the density observed
   now.
3. Projection SHALL be bounded and explainable: the horizon, the rate window and
   the resulting projected value SHALL all appear in the decision's log record,
   so an operator can see why a machine was provisioned before any watermark was
   visibly crossed.
4. A split SHALL NEVER block on, wait for, or fail because of capacity
   provisioning. The split path SHALL make no provider call and SHALL take no
   lock held across one.
5. WHEN a split is refused by the capacity gate (Requirement 6) THEN that
   refusal SHALL itself be a scale-out signal of reason `split_pressure`,
   counted and surfaced, so that the cluster acquires the room the split needs
   rather than refusing splits indefinitely.
6. Merges SHALL feed the same signal with the opposite sign, via
   `report_merge`, and SHALL be an input to scale-in only (never to scale-out
   suppression, which hysteresis already handles).

---

### Requirement 6: The Split Capacity Gate

**User Story:** As an operator, I want a split refused rather than admitted onto
a machine that cannot store its children.

#### Acceptance Criteria

1. `arbiter_gate` SHALL gain a `capacity` member, with `to_string` and the
   metric dimension updated alongside, following the existing gate taxonomy
   (`multi_raft.hpp:210`).
2. WHEN the host's `capacity_probe` reports available bytes below a configured
   `split_capacity_floor_bytes`, or reports a projected post-split shortfall for
   the children's synthetic snapshots, THEN the arbiter SHALL refuse the split
   with `gate=capacity` and SHALL count `split.rejected{gate=capacity}`.
3. The floor SHALL be unset by default, and an unset floor SHALL disable the
   gate entirely — today's behaviour, unchanged.
4. The gate SHALL apply to policy-, admin-, hint- and driver-channel splits
   alike. A machine that cannot write a child's snapshot cannot write it for an
   administrator either.
5. The gate SHALL NOT apply to merges, which reduce consumption.
6. The refusal SHALL be visible in the shard report so the controller sees it
   without a second channel.

---

### Requirement 7: Single-Writer Control

**User Story:** As an operator paying for the machines, I want exactly one party
able to provision, so that a partition cannot produce N copies of a fleet.

#### Acceptance Criteria

1. The controller SHALL act only while it holds a lease it can *prove*, and
   SHALL expose the proof mechanism as a concept so that a deployment can supply
   its own (a Raft group's leadership, a lock service, or a single-process
   assertion for embedded use).
2. The shipped lease implementation SHALL be Raft leadership of a nominated
   coordination group, which is a mechanism this repository already has and
   already trusts for exactly this purpose (`raft.hpp:1498` starts the
   single-group quorum loop on `become_leader` and stops it on any transition
   away).
3. WHEN the lease is lost THEN the controller SHALL stop issuing new provider
   calls before its next step, SHALL let in-flight calls complete or time out
   without acting on their results, and SHALL NOT mark any intent complete on
   the strength of a result observed after the lease was lost.
4. A controller that cannot determine whether it holds the lease SHALL behave as
   though it does not.
5. The design SHALL state the failure mode that remains after this: a machine
   provisioned by a controller whose lease expired mid-call is an orphan, and
   Requirement 8's reaping — not the lease — is what bounds its cost.
6. Two controllers configured against one cluster SHALL be a *detected*
   misconfiguration where the lease mechanism can detect it, and SHALL be
   documented as unsupported where it cannot.

---

### Requirement 8: The Provisioning Ledger

**User Story:** As an operator, I want a controller failover mid-provision to
cost me nothing, and to leave nothing behind.

#### Acceptance Criteria

1. Every intent SHALL be recorded durably **before** the provider call that
   enacts it, and SHALL carry: an idempotency key, the decision and its reason,
   the target placement group, the deadline, and the observed cluster state that
   justified it.
2. The idempotency key SHALL be propagated to the provider as resource metadata
   wherever the manager supports it — every shipped manager already tags or
   labels what it creates — so that reconciliation can match a live machine to
   the intent that created it.
3. On start, and on acquiring the lease, the controller SHALL reconcile before
   deciding anything: ledger intents against `assess_quorum` and against the
   nodes visible in heartbeats. It SHALL NOT issue a new provider call until
   reconciliation completes or its own bounded deadline expires.
4. An intent whose machine is live and joined SHALL be completed. An intent
   whose machine is live but has not joined by its deadline SHALL be reaped:
   `decommission_node`, then the intent is closed as `orphaned` and counted.
5. An intent whose machine cannot be found SHALL be closed as `failed` and SHALL
   feed the failure backoff (Requirement 13), never a silent retry loop.
6. Reaping SHALL be safe to run repeatedly: `decommission_node` is required to
   be idempotent by the concept, and the controller SHALL depend on that rather
   than on its own bookkeeping.
7. The ledger SHALL be pluggable behind a small concept, with the shipped
   implementation being a replicated state machine on the coordination group, so
   that the ledger's durability and the lease's authority come from the same
   consensus decision. A file-backed ledger SHALL be acceptable for
   single-controller deployments and SHALL be documented as such.
8. Ledger growth SHALL be bounded: completed intents older than a configurable
   retention SHALL be compacted away, and the retention SHALL exceed the longest
   provisioning deadline by a stated margin.

---

### Requirement 9: Placement of New Capacity

**User Story:** As an operator running across three failure domains, I want the
new machine to land where it improves my failure tolerance.

#### Acceptance Criteria

1. Scale-out SHALL name a placement group, and the controller SHALL choose the
   group that leaves the cluster most balanced across the groups declared in
   `quorum_manager::topology()`, breaking ties deterministically.
2. `topology()` SHALL be treated as the **floor and the shape**: the controller
   SHALL NOT reduce any group below its declared `target_count`, and SHALL
   preserve the declared ratios between groups when adding capacity above the
   floor. The concept SHALL NOT be extended with a topology mutator
   (Requirement 1.3).
3. WHEN a placement group is unavailable for provisioning — a stock-out, a quota
   refusal, a manager-specific escalation exhausted — THEN the controller SHALL
   attempt the next-best group rather than failing the decision outright, and
   SHALL record which group was tried and why it was refused.
4. The controller SHALL NOT place capacity in a placement group not named in
   `topology()`.
5. Where a manager reports per-group health in `assess_quorum`, the controller
   SHALL prefer groups whose live count is furthest below their target, so that
   elastic scale-out and quorum repair do not fight each other.

---

### Requirement 10: Admitting a New Machine to Shards

**User Story:** As an operator, I want the new machine to actually take load,
and I want the process of giving it load never to endanger a shard.

#### Acceptance Criteria

1. Admission SHALL use the existing operator vocabulary and no new RPC:
   `add_replica{as_learner=true}` → wait for catch-up → promotion → optional
   `transfer_leader`, delivered through the heartbeat response the host already
   applies (`multi_raft_impl.hpp:3021`).
2. The controller SHALL rely on the host's existing lazy replica creation for
   materialisation on the new machine (`multi_raft_impl.hpp:1240-1300`): once
   the membership change commits, the descriptor names the new node, and the
   first AppendEntries creates an uninitialised replica populated by
   InstallSnapshot. The controller SHALL therefore serve `lookup_descriptor` for
   the groups it moves, and this SHALL be stated as a deployment requirement.
3. A replica SHALL be added as a learner first and promoted only after it is
   caught up, and the controller SHALL NOT issue a `remove_replica` for the
   replica being displaced until the replacement is a voter.
4. The number of shards moved concurrently SHALL be bounded by a configurable
   cap, per target machine and cluster-wide, because each move is a snapshot
   transfer and an unbounded rebalance is a self-inflicted outage. Snapshot
   counters already present in `node_report` SHALL be an input to that cap.
5. The controller SHALL NOT move a shard that is not `stable`, whose group has
   any down or pending replica, or that is inside its own split/merge cooldown.
6. Selection SHALL prefer the largest contributors to the imbalance the decision
   named — the busiest or biggest shards on the most loaded machine — and SHALL
   be explainable in the log record.
7. Admission SHALL be abandonable at every step: an operator not applied by a
   host (the host reports it skipped, with a reason) SHALL be retried under
   backoff or dropped, and SHALL never leave a group in joint configuration.
8. A machine that has been admitted SHALL be reported as complete in the ledger
   only once it holds at least one voting replica.

---

### Requirement 11: Scale-In

**User Story:** As an operator whose workload shrank, I want the machines back,
and I want the shards off them first.

#### Acceptance Criteria

1. Scale-in SHALL drain before it decommissions: transfer leadership away, then
   remove the machine's replicas one group at a time, then `decommission_node`.
2. The controller SHALL drain at most one machine at a time, cluster-wide.
3. A removal SHALL be refused WHEN it would take any group below its
   configuration's quorum requirement, WHEN the group has a down or pending
   replica, or WHEN the group is mid-split or mid-merge.
4. Scale-in SHALL be refused entirely WHEN overall quorum health is anything but
   `healthy`, WHEN any provisioning intent is in flight, or WHEN the cluster
   would fall below `min_cluster_size` or the `topology()` floor.
5. A drain that cannot complete within a configurable deadline SHALL be
   abandoned: the machine is returned to service, the intent is closed as
   `abandoned`, and the event is counted. A half-drained machine SHALL NOT be
   decommissioned.
6. Scale-in SHALL be disabled by default, independently of scale-out, so that an
   operator can adopt growth without adopting shrink.

---

### Requirement 12: Bounds, Budget and the Kill Switch

**User Story:** As the person who pays the bill, I want hard limits that a
policy bug cannot exceed.

#### Acceptance Criteria

1. The controller SHALL enforce, independently of any policy: `max_cluster_size`,
   `max_in_flight_intents`, a minimum interval between provider calls, and a
   maximum number of provisioning calls per rolling window.
2. These bounds SHALL be enforced in the controller, not in the policy, so that
   a custom policy cannot exceed them.
3. Exceeding a bound SHALL be a counted, logged refusal naming the bound, never
   a silent no-op.
4. A single configuration flag SHALL disable all provisioning and all
   decommissioning immediately, in the shape of the existing global split/merge
   kill switch, and SHALL be honoured within one evaluation interval.
5. The controller SHALL support a `dry_run` mode that evaluates, decides, logs
   and counts, but makes no provider call and issues no operator. Dry run SHALL
   be the documented first step of adoption.
6. Every default SHALL be chosen so that a deployment that enables the feature
   and configures nothing else cannot exceed a small, stated multiple of its
   starting size.

---

### Requirement 13: Failure Handling

**User Story:** As an operator, I want a cloud API having a bad day to be a
non-event.

#### Acceptance Criteria

1. No provider call SHALL be made on a tick thread, a Raft callback, or with any
   host lock held. All provider I/O SHALL run on the controller's own executor.
2. Every provider call SHALL have a deadline, and its expiry SHALL produce the
   ledger's `deadline_exceeded` path (Requirement 8.4), not an indefinite wait.
3. Provider failures SHALL back off exponentially with jitter, using the
   existing retry machinery rather than a new one, and consecutive failures
   SHALL open a circuit that suspends scale-out until a success or a
   configurable cool-off.
4. The controller SHALL NOT provision WHEN quorum health is `lost`, mirroring
   the rule the single-group path already enforces (`raft.hpp` Req 14.2), and
   SHALL NOT scale in unless health is `healthy`.
5. A partially-successful provider call — a machine created but not reachable,
   or reachable but not joined — SHALL be handled by reaping (Requirement 8.4)
   and SHALL NOT be retried in place.
6. A host that skips an operator SHALL be respected: the controller SHALL treat
   `skipped_operator_reason` as feedback, SHALL back off on `shard_busy`, and
   SHALL re-plan on `stale_epoch` or `not_leader` rather than resending.
7. The controller failing entirely SHALL degrade the cluster to today's
   behaviour — static capacity — and SHALL NOT impair routing, splits, merges,
   or consensus.

---

### Requirement 14: Observability

**User Story:** As an operator, I want to answer "why did this cluster grow?"
from metrics and logs alone.

#### Acceptance Criteria

1. The controller SHALL emit counters for: decisions by reason, refusals by
   bound, intents by terminal state (completed, orphaned, failed, abandoned),
   provider call latency, admissions and drains, and shards moved.
2. It SHALL emit gauges for: cluster size against floor and ceiling, per-group
   size against target, shards and leaders per node (max, mean, spread), and
   in-flight intents.
3. Every decision SHALL produce one structured log record carrying the reason,
   the signal values that triggered it, the projection inputs where projection
   was used, the chosen placement group, the idempotency key, and the bounds
   evaluated. One record, so that an incident review reads a decision without
   joining lines.
4. The metric names SHALL follow the existing `kythira.multiraft.*` taxonomy,
   and the new arbiter gate SHALL appear as a dimension value on the existing
   `split.rejected{gate}` counter rather than as a new metric.
5. Provider-identifying details SHALL NOT be embedded in metric *names*; where a
   provider must be identified it SHALL be a dimension.

---

### Requirement 15: Host-Side Changes to `multi_raft`

**User Story:** As a maintainer, I want the consensus core changed as little as
this feature can manage, and every change it does make to be justified on its
own.

#### Acceptance Criteria

1. `group_scoped_types` SHALL shadow `quorum_manager_type` with
   `no_op_quorum_manager`, so that a host bundle naming a real provider manager
   compiles, and so that N groups in a process cannot each provision. A
   compile-time test SHALL prove a bundle naming a non-default-constructible
   manager now instantiates `multi_raft`.
2. `multi_raft_config` SHALL gain the capacity-gate floor (Requirement 6) and
   the controller-facing hooks it needs, following the existing
   `std::function` hook style of the placement-driver channel. The host SHALL
   NOT gain a `quorum_manager` member: hosts do not provision, controllers do.
3. The host SHALL continue to give split children the parent's replica set.
   Honouring `shard_id_allocation::_suggested_voters` at child creation is
   forbidden by state locality — a child's state is derived on the machines that
   hold the parent's — and design.md SHALL record this as a rejected
   alternative.
4. Because the host cannot honour them, allocation suggestions SHALL be
   *accounted for* rather than silently dropped: the host SHALL count
   `shard.allocation.suggestion_ignored` when a driver supplies voters or
   learners it cannot apply, so a driver author is told rather than left
   guessing.
5. The empty-registry heartbeat path (Requirement 2.3) SHALL be covered by a
   regression test.
6. No change SHALL alter the behaviour of a deployment that does not configure
   this feature, and the existing multi-raft suites SHALL pass unmodified.

---

### Requirement 16: Testing and Acceptance

**User Story:** As a reviewer, I want the safety properties demonstrated, not
asserted.

#### Acceptance Criteria

1. A deterministic mock quorum manager SHALL be provided that can be programmed
   with latency, failure, stock-out, partial success, and never-joins outcomes,
   and every controller unit test SHALL use it. Wall-clock sleeps SHALL NOT
   appear in unit tests.
2. Unit tests SHALL cover: policy hysteresis and its `validate()` rejection,
   projection arithmetic, placement-group selection including refusal
   fallback, bound enforcement, and every ledger intent transition.
3. Property tests SHALL assert the invariants that make this feature safe to
   run: cluster size never exceeds its ceiling nor falls below its floor; no
   group ever loses quorum through an admission or a drain; the number of live
   provider-created machines never exceeds recorded intents; and no shard is
   moved while non-`stable`.
4. Failover tests SHALL kill the lease holder mid-intent and assert that the
   successor reconciles to exactly one machine per intent, with orphans reaped.
5. An end-to-end integration test SHALL use `docker_quorum_manager`: a cluster
   splits under load, crosses the density watermark, provisions a container,
   admits it, and ends with shards on it — with no cloud credentials involved.
6. Optional per-provider live tests SHALL exist behind the existing credential
   and label gating, one per provider, each performing the same scenario and
   each ending with a post-run leak audit that fails the job if any resource it
   created still exists. The audit is not optional where the live test runs.
7. Chaos coverage SHALL include a provider that never returns, a machine that
   boots but never joins, and a lease lost mid-admission.

---

### Requirement 17: Documentation and Operating Envelope

**User Story:** As someone deciding whether to turn this on, I want the costs
stated before the instructions.

#### Acceptance Criteria

1. `doc/elastic_shard_capacity.md` SHALL be added, and SHALL open with the
   operating envelope: what this costs in machines, in snapshot transfer, and in
   provider API calls, and what it cannot do.
2. It SHALL state plainly that a split never waits for capacity and never fails
   for lack of it, and that elasticity is therefore a *lagging* correction.
3. It SHALL carry the provider parity table (Requirement 1.7), the full
   configuration surface with defaults and their derivation, the metric and log
   taxonomy, and a dry-run-first adoption procedure.
4. It SHALL document the residual failure modes without softening them: the
   orphan window, the cost of a lease lost mid-call, and the rebalance cost of a
   scale-in that was premature.
5. README SHALL gain one entry pointing at it, in the shape of the existing
   cloud-object-persistence entry, including the same "read the envelope before
   adopting" framing.

---

### Requirement 18: Non-Goals

The following are explicitly out of scope, and each is out of scope for a
reason worth stating.

1. **Provisioning as a precondition of a split.** Splits do not need machines
   (Requirement 5.4). Making them wait would put a minutes-long provider call
   inside a path that must complete in milliseconds.
2. **Extending the `quorum_manager` concept.** Ten implementations satisfy it,
   most verified against live services. A required new method would invalidate
   all of them for a capability an optional refinement can express
   (Requirement 1.5).
3. **A new host-to-host control RPC.** The heartbeat/operator channel already
   reaches every host, and Kythira's transport deliberately carries no
   client-command RPC (`multi_raft.hpp:757-765`).
4. **Cost optimisation across instance types, spot bidding, or reservation
   planning.** Those are provider-specific and already partly expressed inside
   individual managers (e.g. spot options). The controller decides *how many
   nodes, where* — never *what kind*.
5. **Autoscaling the state machine's own storage**, e.g. growing a volume rather
   than adding a machine. A legitimate alternative in some deployments, and a
   different feature.
6. **Replacing the cloud's own autoscaler.** Where one exists, this feature
   keeps its target in step where the concept allows (Requirement 1.5) and
   otherwise stays out of its way.
