# Design Document — Elastic Shard Capacity

## 1. Where the seam is

Four facts from the tree decide this design, and every component below follows
from them.

**Fact 1 — a split needs no machine.** `multi_raft_impl.hpp:1682`:

```cpp
child._voters = parent._voters;
```

Children are created in-process on the machines that already hold the parent's
state, seeded from a synthetic snapshot before `node::start()` opens the store
(`multi_raft_impl.hpp:170-176`). A split multiplies groups, not hosts. So
capacity is never on the split's critical path — it is a *lagging* correction to
the density the split produced. Requirement 5.4 is that fact, stated as a rule.

**Fact 2 — provisioning already exists, one layer down.** `quorum_manager`
(`quorum_management.hpp:172`) has `provision_node` / `decommission_node` /
`assess_quorum` / `topology`, and ten conforming implementations across five
clouds plus Docker and the no-op. That concept *is* the portability layer; this
feature adds a caller for it in the multi-Raft world and adds nothing to it.

**Fact 3 — the host has exactly one cluster-scope control channel.** Kythira's
transport carries no client-command RPC and no host-to-host control RPC
(`multi_raft.hpp:757-765`). What does exist is the placement-driver channel:
each host calls `report_shard_heartbeat(reports)` once per interval and applies
the operators it gets back (`multi_raft_impl.hpp:3083-3104`). Anything that
wants to act on shards it does not lead must arrive as an operator on that
channel. That single fact determines that the controller is packaged as a
`shard_placement_driver`.

**Fact 4 — the new machine can already materialise its own replicas.** Lazy
replica creation (`multi_raft_impl.hpp:1240-1300`) turns an AppendEntries for an
unknown group into an uninitialised replica, provided the descriptor names this
node — consulting the local routing map first, then a rate-limited
`lookup_descriptor`, then InstallSnapshot. A freshly provisioned host therefore
needs no bootstrap protocol of its own: commit the membership change, and the
machine builds the replica. The controller's obligation is to serve
`lookup_descriptor`.

What follows is the design that adds no RPC, no concept method, and no field to
the wire types, and confines its changes to the consensus core to one type
alias, one enum member and one counter.

## 2. Architecture

```
      ┌────────────────────────── control plane (single writer) ─────────────────────────┐
      │                                                                                  │
      │   elastic_shard_placement_driver<Inner, Controller>        ← shard_placement_driver
      │        │  allocate_shard_ids → Inner                                             │
      │        │  report_shard_heartbeat(reports) ─┐         ┌─ operators ──┐            │
      │        │  report_node_heartbeat(report) ───┤         │              │            │
      │        │  report_split / report_merge ─────┤         │              │            │
      │        v                                   v         │              │            │
      │   elastic_capacity_controller<QuorumMgr, Policy, Ledger, Lease>     │            │
      │        ├─ inventory   (nodes, shards, density, split rate)          │            │
      │        ├─ capacity_policy  ── pure ──> capacity_decision            │            │
      │        ├─ bounds + kill switch + dry run                            │            │
      │        ├─ ledger      (intents, idempotency keys)  ── durable       │            │
      │        ├─ lease       (act / do not act)                            │            │
      │        └─ rebalance planner ───────────── operators ────────────────┘            │
      │                    │                                                             │
      │                    v  own executor, never a tick thread                          │
      │        quorum_manager: provision_node / decommission_node / assess_quorum        │
      └────────────────────┼─────────────────────────────────────────────────────────────┘
                           │  (AWS EC2 · AWS ASG · Azure VM · Azure VMSS · GCP Compute
                           │   GCP MIG · OCI Instance Pool · Alibaba ESS · Docker · no-op)
                           v
      ┌─────────────────── data plane (unchanged consensus core) ───────────────────┐
      │  multi_raft host ×N   tick → maybe_heartbeat → report_* → apply_operator     │
      │                       split → report_split (trigger edge)                    │
      │                       lazy replica creation materialises new replicas        │
      └─────────────────────────────────────────────────────────────────────────────┘
```

Three properties are worth naming because they are what make the diagram safe:

- The controller **never calls into a host**. It answers a call the host already
  makes. A controller that is down is a driver that returns no operators, which
  is the shipped default's behaviour.
- The controller **never touches Raft state**. It emits the same advisory
  operators the concept already defines, and the host's arbiter is free to skip
  every one of them.
- Provider I/O is on the controller's own executor. `step()` — the function the
  heartbeat calls — is non-blocking and reads only committed state.

## 3. Component 1 — `capacity_policy` (`include/raft/capacity_policy.hpp`)

The decision layer, in the shape of `split_merge_policy` because the constraints
are the same: consulted on one machine, its answer recorded before it is acted
on, so it may be non-deterministic but must not do I/O.

```cpp
enum class capacity_reason : std::uint8_t {
    density = 0,        ///< shards or leaders per node crossed the watermark
    storage = 1,        ///< used/capacity crossed the watermark
    load = 2,           ///< read/write rate per node crossed the watermark
    overload = 3,       ///< too many hosts asserting `_overloaded`
    split_pressure = 4, ///< split rate, projected density, or gate=capacity refusals
    topology_floor = 5, ///< below `topology()`'s declared target
    manual = 6,         ///< an operator asked
};

template<typename GroupId> struct capacity_decision {
    static auto hold() -> capacity_decision;
    static auto scale_out(GroupId group, std::size_t count, capacity_reason r,
                          capacity_evidence ev) -> capacity_decision;
    static auto scale_in(GroupId group, capacity_reason r, capacity_evidence ev)
        -> capacity_decision;
    // accessors only; no public aggregate init, so an inconsistent decision
    // (scale_out with count 0, scale_in naming a group with one node) is
    // unrepresentable rather than merely undocumented.
};

template<typename P, typename NodeId, typename GroupId, typename Key>
concept capacity_policy = requires(P& p, const cluster_capacity_snapshot<NodeId, GroupId, Key>& s) {
    { p.evaluate(s) }              -> std::same_as<capacity_decision<GroupId>>;
    { p.cooldown() }               -> std::same_as<std::chrono::milliseconds>;
    { p.validate() }               -> std::same_as<bool>;
    { p.get_validation_errors() }  -> std::same_as<std::vector<std::string>>;
};
```

`cluster_capacity_snapshot` is assembled by the controller from data already on
the wire (Requirement 2.1): the latest `node_report` per node with its age, the
latest `shard_report` per shard, per-placement-group live and target counts from
`assess_quorum` and `topology()`, the split and merge rate windows, the count of
`gate=capacity` refusals, and the in-flight intent count. `capacity_evidence` is
the subset the policy actually used, carried into the log record and the ledger
so that "why did this cluster grow?" is answerable from one line (Requirement
14.3).

### `threshold_capacity_policy`

Five signals, each with a **pair** of watermarks:

| Signal | Scale-out above | Scale-in below | Default out / in |
|---|---|---|---|
| shards per node | `shards_per_node_high` | `shards_per_node_low` | 200 / 80 |
| leaders per node | `leaders_per_node_high` | `leaders_per_node_low` | 80 / 30 |
| storage utilisation | `storage_high` | `storage_low` | 0.75 / 0.35 |
| write rate per node | `write_bps_high` | `write_bps_low` | disabled |
| overloaded fraction | `overloaded_high` | `overloaded_low` | 0.34 / 0.0 |

`validate()` rejects any pair closer than `min_hysteresis_margin` (default: the
low watermark must be at most 60% of the high). This is the same guard
`threshold_split_merge_policy::validate()` already applies to the split/merge
interval, adopted for the same reason — the failure mode of a bad pair is
unbounded, and a constructor is the cheapest place to catch it.

Two further gates apply to every signal: `sustained_for` (default 5 minutes) for
scale-out and `sustained_for_scale_in` (default 30 minutes) for scale-in. The
asymmetry is deliberate and is the policy's central trade: a machine added late
costs latency; a machine removed early costs a re-provision plus two snapshot
transfers per shard moved twice.

**Split pressure** is the signal this specification exists for. The policy
keeps a rate window over `report_split` (default 15 minutes) and projects:

```
projected_shards_per_node = current_shards_per_node
                          + split_rate_per_minute * horizon_minutes / live_node_count
```

with `horizon_minutes` defaulting to 30 — chosen because it comfortably exceeds
the p99 provisioning time of every shipped manager, which is the only thing the
horizon has to beat. Scale-out fires when the *projected* value crosses
`shards_per_node_high`, with the three projection inputs recorded verbatim in
the decision. A `gate=capacity` refusal (§5) short-circuits the projection: the
cluster is already out of room, so the signal is not a forecast.

Everything is off by default (Requirement 4.6): the shipped defaults describe
what the knobs mean, not what an unconfigured cluster does.

## 4. Component 2 — `elastic_capacity_controller`

```cpp
template<typename QuorumMgr, typename Policy, typename Ledger, typename Lease,
         typename NodeId, typename Address, typename GroupId, typename Key>
requires quorum_manager<QuorumMgr, NodeId, Address, std::string> &&
         capacity_policy<Policy, NodeId, GroupId, Key> &&
         capacity_ledger<Ledger, NodeId, GroupId> &&
         capacity_lease<Lease>
class elastic_capacity_controller;
```

Provider neutrality is this signature: the only thing the controller knows about
a cloud is that something satisfies `quorum_manager`. No provider header is
reachable from this file (Requirement 1.1).

### The intent state machine

One intent is one machine, added or removed. The ledger records the transition
*before* the action that causes it, which is the whole reason the ledger exists.

```
                     decided ──(record)──> requested ──(provision_node)──> provisioning
                        │                      │                               │
             bound/kill │           lease lost │                    deadline   │ joined
                        v                      v                               v
                     refused                abandoned <────── reap ────── orphaned
                                                                               ^
   admitting <──(operators: add_learner → promote)── provisioned ──────────────┘
       │                                                   (never joined)
       │ holds a voting replica
       v
   completed
```

Scale-in mirrors it: `decided → draining → drained → decommissioning →
completed`, with `abandoned` reachable from `draining` on deadline
(Requirement 11.5) and the machine returned to service.

Three rules make this recoverable:

1. **Record before act.** An intent reaches `requested` durably before
   `provision_node` is called. A controller that dies between the two leaves a
   `requested` intent whose provider resource may or may not exist —
   reconciliation resolves it, and reconciliation is the only thing that may.
2. **Idempotency key in provider metadata.** Every shipped manager already tags
   or labels what it creates (EC2 `CreateTags`, GCP labels, Azure tags, OCI
   tags, ESS tags, Docker labels). The key goes there. Where a manager exposes
   no metadata surface, reconciliation degrades to matching on
   `assess_quorum` + join deadline, and that degradation is documented per
   provider rather than hidden.
3. **Deadlines everywhere.** `provision_deadline` (default 10 minutes),
   `join_deadline` (default 15 minutes from `provisioning`), `admit_deadline`,
   `drain_deadline`. Expiry always has a defined next state; nothing waits
   forever.

### `step()`

Called from the driver adapter on the heartbeat path. Non-blocking by
construction:

```
step():
  if !lease.held()                      -> return no operators
  if kill_switch                        -> return no operators
  reconcile_if_needed()                 // bounded, off-thread; returns immediately if pending
  harvest_completed_provider_futures()  // non-blocking poll of the executor's results
  advance_intents()                     // ledger transitions only; may enqueue provider work
  if evaluation_interval elapsed:
      snapshot  = build_snapshot()
      decision  = policy.evaluate(snapshot)
      decision  = bounds.apply(decision)          // Requirement 12.2
      if decision != hold and !dry_run: ledger.record(intent); executor.enqueue(provider call)
  return planner.operators(snapshot, intents)     // add_replica / transfer_leader / remove_replica
```

`harvest` polls; it never `get()`s a pending future. The provider calls run on
the controller's executor and deposit their results where the next `step()`
finds them (Requirement 13.1).

### The rebalance planner

Once a machine is joined, the planner decides which shards move onto it. Inputs
are the same snapshot; output is a bounded list of operators.

- **How many at once**: `min(max_moves_per_target, max_moves_cluster_wide -
  in_flight)`, further reduced by the target's `_receiving_snapshot_count` and
  each source's `_sending_snapshot_count` from `node_report`. Each move is a
  snapshot transfer; an unbounded rebalance is a self-inflicted outage
  (Requirement 10.4).
- **Which shards**: the largest contributors to the imbalance the decision
  named — for `density`, the shards on the machine with the highest count; for
  `storage`, the biggest shards on the fullest machine; for `load`, the busiest.
  Ties break deterministically on group id.
- **Which shards never**: any not `stable`, any with a down or pending replica,
  any inside its split/merge cooldown, and any whose group would drop below its
  quorum requirement mid-move (Requirement 10.5).
- **Sequence per shard**: `add_replica{as_learner=true}` → wait for the leader
  to report the learner caught up → promote → `remove_replica` of the displaced
  replica → optionally `transfer_leader` if the move was for leader balance.
  Never remove before promote (Requirement 10.3), so a group is never below
  strength because of a rebalance.

## 5. Component 3 — `elastic_shard_placement_driver`

A decorator, so that a deployment with its own driver keeps it:

```cpp
template<typename Inner, typename Controller, typename GroupId, typename Key, typename NodeId>
requires shard_placement_driver<Inner, GroupId, Key, NodeId>
class elastic_shard_placement_driver {
    auto allocate_shard_ids(std::size_t n) -> future<std::vector<shard_id_allocation<GroupId, NodeId>>>;
    auto report_shard_heartbeat(const std::vector<shard_report<...>>&) -> future<std::vector<shard_operation<...>>>;
    auto report_node_heartbeat(const node_report<NodeId>&) -> future<void>;
    auto report_split(const descriptor&, const std::vector<descriptor>&) -> future<void>;
    auto report_merge(const descriptor&, const descriptor&) -> future<void>;
    auto lookup_descriptor(const GroupId&) -> std::optional<descriptor>;   // beyond the concept
};
```

- `allocate_shard_ids` delegates to `Inner` untouched. Ids are the inner
  driver's business and a capacity controller has no opinion about them.
- `report_shard_heartbeat` feeds the controller, then returns
  `inner_operators ∪ controller_operators`, with the inner driver's operators
  first: a deployment's own driver outranks the capacity controller on any shard
  both name, and the duplicate is dropped rather than sent twice.
- `report_split` / `report_merge` feed the rate windows and mark the snapshot
  dirty, which is what makes the split a *trigger edge* rather than something
  noticed up to a heartbeat later (Requirement 5.1).
- `lookup_descriptor` is not part of the concept but is what the host's lazy
  replica creation consults (`multi_raft_impl.hpp:1268`); the controller already
  holds every descriptor it has seen in a heartbeat, so serving it is
  bookkeeping, and without it a newly provisioned machine cannot materialise a
  replica for a group whose descriptor it has never seen (Requirement 10.2).

**Deployment shapes.** The adapter is a plain object, so it works in both
shapes the repository already has: in-process (embedded/test deployments and the
Docker chaos suite instantiate it directly and wire the hooks to it), and
out-of-process (a control-plane binary owns it; each host's `std::function`
hooks are the application's own RPC to that binary — the hooks are
`std::function` precisely so Kythira never picks that RPC).

## 6. Component 4 — host-side changes

Deliberately three small things.

**6.1 `group_scoped_types` shadows the quorum manager** (`multi_raft.hpp:75`):

```cpp
template<typename Types, typename GroupId, typename Demux> struct group_scoped_types : Types {
    using network_client_type = group_scoped_client<...>;
    using network_server_type = group_scoped_server<Demux>;
    // A machine is host-scope. A thousand groups in one process must not each
    // hold a provisioning authority, and `create_group_impl` default-constructs
    // whatever this names — which no real provider manager supports.
    using quorum_manager_type = no_op_quorum_manager<
        typename Types::node_id_type, address_of_types<Types>, std::string>;
};
```

The address parameter is not `Types::address_type`: `node_config` derives it
through a traits helper (`raft.hpp:74`, `raft.hpp:176`) precisely because a
bundle need not declare one. The shadow must reuse that same helper rather than
assume the alias exists, or it reintroduces the requirement it is there to
remove.

This is what unblocks a host bundle naming `aws_ec2_quorum_manager` (or any
other) today — such a bundle is currently ill-formed, because
`create_group_impl` (`multi_raft_impl.hpp:177-192`) omits `.quorum_manager` and
every real manager has only an explicit config constructor
(`aws_ec2_quorum_manager.hpp:145`).

**6.2 `arbiter_gate::capacity`** — a new member on the existing enum
(`multi_raft.hpp:210`), its `to_string`, and the floor knob on
`multi_raft_config`. Unset by default; unset disables the gate. The gate is
evaluated where the other gates are, from `capacity_probe`, and the refusal
appears in the shard report so the controller sees it without a second channel.

**6.3 `shard.allocation.suggestion_ignored`** — a counter. The host cannot
honour `_suggested_voters` for split children (§9, rejected alternative 2), and
silently dropping a field the concept advertises is how driver authors lose an
afternoon.

Nothing else in `multi_raft` or `raft.hpp` changes. Single-group `node<Types>`
quorum management is untouched.

## 7. Single-writer control and reconciliation

The lease is a concept so the mechanism is a deployment choice:

```cpp
template<typename L>
concept capacity_lease = requires(L& l) {
    { l.held() }        -> std::same_as<bool>;   // false when unknown (Req 7.4)
    { l.fencing_token() } -> std::same_as<std::uint64_t>;
};
```

The shipped implementation is Raft leadership of a nominated coordination group
— the mechanism this repository already trusts for exactly this decision
(`raft.hpp:1498`: the single-group quorum loop starts on `become_leader` and
stops on any transition away). The shipped ledger is a replicated state machine
on that same group, so authority and durability come from one consensus
decision rather than two systems that can disagree.

**Failover sequence.** New leader → lease held → reconcile *before* any
decision:

1. Read open intents from the ledger.
2. `assess_quorum(cluster)` for the provider's view of what exists.
3. Match by idempotency key (provider metadata), else by node id, else by
   join deadline.
4. Live and joined → `completed`. Live, not joined, deadline passed →
   `decommission_node`, `orphaned`. Not found → `failed`, feeding the backoff.
5. Only then is the policy consulted.

The residual failure mode, stated rather than papered over (Requirement 7.5): a
controller whose lease expires *during* `provision_node` may leave a machine no
successor can attribute if the manager could not carry the idempotency key. The
join deadline plus reaping is what bounds that cost — the lease does not, and
cannot.

## 8. Placement selection

Scale-out names a placement group. `topology()` is read as **floor and shape**,
never mutated (Requirement 9.2 — mutating it would mean extending a concept with
ten implementations for something the controller can hold itself):

```
score(g) = (target_ratio(g) - live_ratio(g))      // most under-represented first
         + under_target_bonus(g)                  // groups below topology() floor first
         - recent_refusal_penalty(g)              // stock-out / quota, decays
tie-break: lowest group id
```

`provision_node(group, nullopt)` is then called for the winner. On a refusal —
stock-out, quota, a manager's own escalation exhausted — the next-best group is
tried, and both the refusal and the fallback are recorded (Requirement 9.3),
because "we grew in the wrong AZ" is a question an operator will ask later.

## 9. Rejected alternatives

**1. Block the split until capacity exists.** Rejected: a split does not need
the machine (§1 Fact 1), and a provider call is 30 s–10 min against a tick
budget of milliseconds. The capacity *gate* (§6.2) is the bounded version of the
same worry — refuse a split that cannot write its children's snapshots — and it
consults a local probe, not a cloud.

**2. Honour `shard_id_allocation::_suggested_voters` when creating split
children.** Rejected on correctness. A child's state is derived locally from the
parent's on each replica that holds it; a machine that does not hold the parent
cannot construct the child at split time, only receive it later by snapshot. So
suggestions can only ever be a post-split placement plan, which is exactly what
the planner emits. Hence Requirement 15.3-15.4: keep inheriting the parent's
voters, and *count* the suggestions the host cannot apply.

**3. Give each group's `node<Types>` a real quorum manager.** Rejected: it is
the single-group design applied where it does not fit. A thousand groups would
hold a thousand provisioning authorities, each seeing only its own membership,
each able to call `provision_node`. §6.1 forecloses it in the type system.

**4. Extend `quorum_manager` with `set_topology`.** Rejected as a *required*
method (Requirement 1.3): ten implementations, most live-verified, would need
new code and new live runs for something the controller can express by holding
its own target. Kept as an optional refinement detected with `requires`, so
group-capacity providers can stay in step where they model it.

**5. Let the cloud's autoscaler own it.** Rejected as the primary mechanism: a
CPU- or queue-depth-based autoscaler cannot see shards per node, cannot admit a
machine into a Raft group, and cannot drain one safely. Where such an autoscaler
exists, alternative 4's optional refinement keeps its target consistent.

**6. A new host-to-host control RPC.** Rejected: the heartbeat/operator channel
already reaches every host once per interval, and adding a control RPC would
duplicate it while contradicting the transport's deliberate refusal to carry
client-command traffic (`multi_raft.hpp:757-765`).

## 10. Configuration surface

| Knob | Default | Note |
|---|---|---|
| `enabled` | `false` | the feature, whole |
| `scale_in_enabled` | `false` | independent of scale-out (Req 11.6) |
| `dry_run` | `true` on first enable | decides, logs, counts; calls nothing |
| `evaluation_interval` | 60 s | policy consulted at most this often |
| `min_cluster_size` / `max_cluster_size` | `topology().total_size()` / 3× | hard bounds |
| `max_in_flight_intents` | 1 | one machine at a time by default |
| `min_provider_call_interval` | 5 min | rate limit independent of policy |
| `max_provisions_per_window` / `window` | 4 / 1 h | budget guard |
| `provision_deadline` | 10 min | → `orphaned` |
| `join_deadline` | 15 min | → reap |
| `admit_deadline` / `drain_deadline` | 30 min / 60 min | → `abandoned` |
| `max_moves_per_target` / `_cluster_wide` | 2 / 8 | snapshot-transfer bound |
| `node_report_staleness` | 3× heartbeat interval | unknown, not absent |
| `ledger_retention` | 7 d | > longest deadline, by a wide margin |
| `split_capacity_floor_bytes` | unset | unset ⇒ gate off (Req 6.3) |

Policy knobs are §3's table plus `sustained_for`, `sustained_for_scale_in`,
`horizon_minutes`, `split_rate_window`, `min_hysteresis_margin`.

## 11. Observability

Counters (`kythira.multiraft.capacity.*`): `decision{reason}`,
`refused{bound}`, `intent{terminal_state}`, `provider_call{op,outcome}` plus
latency, `admission{outcome}`, `drain{outcome}`, `shards_moved`,
`reconcile{outcome}`, `orphan_reaped`.

Gauges: `cluster_size`, `cluster_size_floor`, `cluster_size_ceiling`,
`group_size{group}` against `group_target{group}`, `shards_per_node{stat}` for
max/mean/spread, `leaders_per_node{stat}`, `intents_in_flight`.

The split gate reuses the existing counter as `split.rejected{gate=capacity}`
rather than adding a metric (Requirement 14.4), and the host's new counter is
`shard.allocation.suggestion_ignored`.

One structured log record per decision, carrying reason, signal values,
projection inputs, chosen group, idempotency key and bounds evaluated — one
line, because an incident review should not have to join them (Requirement
14.3).

## 12. Provider parity

The controller is identical across all of them; the table is what an operator
needs to know about the manager underneath it.

| Manager | Grows by | Idempotency metadata | Placement group means | Live test |
|---|---|---|---|---|
| `aws_ec2_quorum_manager` | `RunInstances` | EC2 tags | subnet / AZ | optional |
| `aws_asg_quorum_manager` | ASG desired capacity | EC2 tags | ASG per group | optional |
| `azure_vm_quorum_manager` | VM create | resource tags | zone / availability set | optional |
| `azure_vmss_quorum_manager` | VMSS capacity | resource tags | scale set per group | optional |
| `gcp_compute_quorum_manager` | instance insert | labels | zone | optional |
| `gcp_mig_quorum_manager` | MIG target size | labels | MIG per group | optional |
| `oci_instance_pool_quorum_manager` | pool size | freeform tags | AD / pool | optional |
| `alibaba_ess_quorum_manager` | ESS capacity | tags | scaling group | optional |
| `docker_quorum_manager` | container run | labels | logical label | **required (CI)** |
| `no_op_quorum_manager` | refuses | — | — | unit |

Group-capacity managers (rows 2, 4, 6, 7, 8) are the candidates for the optional
`set_topology` refinement; instance-level managers ignore it.

## 13. Test strategy

- **Deterministic mock manager** with programmable latency, failure, stock-out,
  partial success and never-joins outcomes, plus an injectable clock. No unit
  test sleeps.
- **Unit**: policy hysteresis and its `validate()` rejection; projection
  arithmetic; placement scoring including refusal fallback; every bound; every
  ledger transition including each deadline expiry.
- **Property**: size never exceeds the ceiling nor falls below the floor; no
  group loses quorum through an admission or a drain; live provider machines ≤
  recorded intents; no non-`stable` shard is ever moved.
- **Failover**: kill the lease holder in each intent state; assert the successor
  reconciles to exactly one machine per intent and reaps orphans.
- **Integration (`docker_quorum_manager`, in CI, no credentials)**: a cluster
  under load splits, crosses the density watermark, provisions a container,
  admits it, and ends with shards on it — the end-to-end proof of the feature.
  Must work under both Docker and rootless Podman, per CLAUDE.md.
- **Chaos**: provider never returns; machine boots but never joins; lease lost
  mid-admission.
- **Live, per provider, optional**: the same scenario behind existing credential
  gating, each ending with a leak audit that fails the job if anything it
  created still exists.

## 14. Operating envelope

State this before the instructions, as `doc/cloud_object_persistence.md` does
for its write path.

- **Elasticity lags.** A split completes immediately; the machine that relieves
  it arrives minutes later. With defaults (`sustained_for` 5 min, provisioning
  1–10 min, admission bounded by snapshot transfer), the wall-clock gap between
  crossing a watermark and carrying load is **10–30 minutes**. Size the
  watermarks so that gap is affordable; that is what `horizon_minutes` is for.
- **Every move is a snapshot transfer.** Moving a shard costs one full state
  transfer of that shard, plus catch-up. Rebalancing onto a new machine costs
  roughly `shards_moved × shard_size` of network and disk. The move caps exist
  because the untuned version is an outage.
- **Provider API cost is small; machine cost is not.** One provision is a
  handful of API calls; the machine bills until it is decommissioned. The
  budget guard and `max_cluster_size` are the real protections, and dry run is
  the way to find out what a policy would have done before it does it.
- **What it cannot do.** It cannot make a split wait for capacity; it cannot
  rescue a cluster that is already out of disk (the gate refuses splits, the
  machine still arrives minutes later); it cannot choose instance types; and
  with a manager that carries no metadata, it cannot attribute a machine created
  by a controller that died mid-call — only reap it after its join deadline.
