# Elastic shard capacity

A controller that adds a machine to a multi-Raft cluster when its shards need
one, admits the new machine onto shards through the placement-driver channel the
hosts already apply, and, if you allow it, drains and removes a machine the
cluster no longer needs. It provisions through whichever `quorum_manager` the
deployment already uses, and no provider is the default or the reference.

The spec is `.kiro/specs/elastic-shard-capacity/`. The headers are:

| Header | What it holds |
|---|---|
| `include/raft/capacity_policy.hpp` | the snapshot, the decision, the `capacity_policy` concept, `threshold_capacity_policy` |
| `include/raft/capacity_ledger.hpp` | the intent record and its state machine; memory, file and replicated ledgers |
| `include/raft/capacity_lease.hpp` | `raft_leadership_lease`, `single_process_capacity_lease` |
| `include/raft/elastic_capacity_controller.hpp` | the controller and `elastic_capacity_config` |
| `include/raft/elastic_shard_placement_driver.hpp` | the decorator that puts the controller on a host's driver channel |

## Read this first: the operating envelope

The envelope decides whether this feature fits your cluster, so it comes before
any instructions.

- **A split never waits for capacity, and never fails for lack of it.** A split
  completes as soon as the arbiter admits it. The controller learns of it
  afterwards, as a trigger edge, and the machine that relieves it arrives
  minutes later. The only capacity-related way a split can be refused is the
  optional disk floor (`split_capacity_floor_bytes`), and that refuses the
  split. It never queues it.
- **Elasticity lags by 10 to 30 minutes.** The default policy needs a signal
  above its watermark for `_sustained_for` (5 min) before deciding. Providers
  take 1 to 10 min to hand over a machine. Admission is then bounded by
  snapshot transfer. So with defaults, the gap between crossing a watermark
  and the new machine carrying load is 10 to 30 minutes of wall clock. Size the
  watermarks so that gap is affordable; `_horizon_minutes` exists to project
  split pressure across it.
- **Every move is a snapshot transfer.** Moving one replica onto the new machine
  costs one full state transfer of that shard, plus catch-up. A rebalance costs
  roughly `shards_moved × shard_size` of network and disk on both ends. The
  move caps (`max_moves_per_target`, `max_moves_cluster_wide`) exist because the
  uncapped version is an outage.
- **Provider API calls are cheap; machines are not.** A provision is a handful
  of API calls. The machine bills until it is decommissioned. The real
  protections are `max_cluster_size` and the budget guard
  (`max_provisions_per_window`). Dry run is how you find out what a policy would
  have done before it does it.
- **What it cannot do.**
  - It cannot make a split wait for capacity.
  - It cannot rescue a cluster that is already out of disk: the floor refuses
    the split, and the machine still arrives minutes later.
  - It cannot choose instance types. That is the quorum manager's
    configuration.
  - With a manager that carries no idempotency metadata, it cannot attribute a
    machine created by a controller that died mid-call. It can only reap that
    machine after the join deadline. See [Residual failure modes](#residual-failure-modes).

## Residual failure modes

The controller is built to recover from each of these, but none of them is free.

- **The orphan window.** An intent is recorded durably before the provider call
  (record before act). A controller that dies after the call went out but
  before it learned the machine's id leaves a machine nobody has attributed.
  Its successor reconciles. With a keyed manager it finds the machine by its
  idempotency key and either admits it or reaps it. With an unkeyed manager it
  attributes by arrival, or reaps whatever is still unattributed and unjoined
  once `provision_deadline + join_deadline` has passed. **Until then that
  machine bills.** With defaults, that is up to 25 minutes of a machine doing
  nothing.
- **A lease lost mid-call.** A provider result that arrives after the lease is
  lost is discarded, and counted as `provider_result_discarded`. The machine it
  describes is found again by whichever controller holds the lease next,
  through reconciliation. A controller never acts on a result it got under a
  lease it no longer holds.
- **A machine that never joins** is reaped after `join_deadline`
  (`orphan_reaped`). Reaping is a `decommission_node` call, and the quorum
  manager concept makes that idempotent, so a successor that reaps it a second
  time does no harm.
- **A premature scale-in is expensive.** Removing a machine costs a drain:
  leadership moves first, then each replica moves off as a snapshot transfer.
  If the cluster grows back, the replacement costs a provision plus the same
  transfers again. That is why scale-in is off by default, separately from
  scale-out, and why the default policy waits six times longer to shrink
  (`_sustained_for_scale_in`, 30 min) than to grow.
- **A move off a shard's leader needs a leadership transfer.** On a transport
  without TimeoutNow (cpp-httplib), the host refuses the transfer as
  `unsupported` and the move is abandoned with the new replica already voting.
  The shard is left one voter over, which is safe but uses an extra replica.
  To make this rare, the controller prefers shards the source does not lead
  whenever weights tie.
- **A provider call that never returns** holds its intent in `provisioning`
  until `provision_deadline`. The intent then moves to `reaping`, and the
  machine the lost call may have created is found by key (or by the unkeyed
  rules above) and torn down. Nothing in `step()` waits on a provider future,
  so the hosts' heartbeats never stall behind one.

## Provider parity

The controller is the same over every manager. This table describes the
managers underneath it.

| Manager | Grows by | Idempotency key | Placement group | Group-target refinement |
|---|---|---|---|---|
| `aws_ec2_quorum_manager` | `RunInstances` | EC2 tag `kythira:idempotency-key` | subnet / AZ | no |
| `aws_asg_quorum_manager` | ASG desired capacity | not carried (group-capacity); unkeyed reconciliation | ASG per group | no |
| `azure_vm_quorum_manager` | VM create | resource tag `kythira:idempotency-key` | zone / availability set | no |
| `azure_vmss_quorum_manager` | VMSS capacity | not carried (group-capacity); unkeyed reconciliation | scale set per group | no |
| `gcp_compute_quorum_manager` | instance insert | label `kythira-idempotency-key` | zone | no |
| `gcp_mig_quorum_manager` | MIG target size | not carried (group-capacity); unkeyed reconciliation | MIG per group | no |
| `oci_instance_pool_quorum_manager` | pool size | not carried (group-capacity); unkeyed reconciliation | AD / pool | no |
| `alibaba_ess_quorum_manager` | ESS capacity | not carried (group-capacity); unkeyed reconciliation | scaling group | no |
| `docker_quorum_manager` | container run | container label `kythira.idempotency-key` | logical label | no |
| `no_op_quorum_manager` | refuses | — | — | — |

"Idempotency key" means the manager satisfies the controller's
`keyed_quorum_manager` refinement (`provision_node_keyed`,
`find_by_idempotency_key`). A manager without it is still fully supported, but
its reconciliation degrades exactly as
[Residual failure modes](#residual-failure-modes) describes. "Group-target
refinement" is `resizable_quorum_manager` (`set_group_target`). It lets a
group-capacity manager's own notion of a group's size follow the controller's.
The controller detects both refinements with concepts and needs neither.

The keyed managers attach the key in the create call itself — RunInstances'
TagSpecifications, the VM's `PUT`, the `instances.insert` resource — so it
exists from the instant the machine does, which is what lets a successor find a
machine whose creator died before learning its id. Their lookups count a
stopped machine (a stopped EC2 instance, a GCE `TERMINATED` instance, a
deallocated VM, a stopped container): it still exists, still bills for its
disk, and still has to be reaped. They skip one already being destroyed.

GCP label values allow only lowercase letters, digits, `-` and `_`, up to 63
characters. The controller's own keys fit and are written verbatim. A key that
does not is mapped deterministically (`gcp_idempotency_label_value`: a hash of
the key plus a sanitised prefix), so the label is still derivable from the
ledger alone.

The group-capacity managers do not carry the key, and that is a limit of how
they grow rather than unfinished work: they raise a target size and the
provider launches the instance, so there is no create call of theirs to attach
it to. Tagging the instance afterwards would reopen exactly the window the key
exists to close. They reconcile by node id and join deadline instead, as
[Residual failure modes](#residual-failure-modes) describes, and the table says
so rather than implying otherwise.

## Wiring

### In-process, one controller for the hosts in this process

```cpp
#include <raft/capacity_lease.hpp>
#include <raft/capacity_ledger.hpp>
#include <raft/capacity_policy.hpp>
#include <raft/elastic_capacity_controller.hpp>
#include <raft/elastic_shard_placement_driver.hpp>

using policy_t = kythira::threshold_capacity_policy<std::uint64_t, std::uint64_t, std::string>;
using ledger_t = kythira::file_capacity_ledger<std::uint64_t, std::string>;
using ctl_t = kythira::elastic_capacity_controller<my_quorum_manager, policy_t, ledger_t,
                                                   kythira::raft_leadership_lease>;
using adapter_t = kythira::elastic_shard_placement_driver<
    kythira::no_op_shard_placement_driver<std::uint64_t, std::string, std::uint64_t>,
    ctl_t, std::uint64_t, std::string, std::uint64_t>;

kythira::elastic_capacity_config cfg;
cfg.enabled = true;  // dry_run stays true: see "Adoption" below

ctl_t ctl{manager, policy_t{policy_cfg}, ledger, lease, cfg,
          [&](std::function<void()> w) { pool.add(std::move(w)); }};
adapter_t adapter{ctl};

host_cfg.allocate_shard_ids       = [&](auto n) { return adapter.allocate_shard_ids(n).get(); };
host_cfg.report_shard_heartbeat   = [&](const auto& r) { return adapter.report_shard_heartbeat(r).get(); };
host_cfg.report_node_heartbeat    = [&](const auto& r) { adapter.report_node_heartbeat(r).get(); };
host_cfg.report_operator_outcomes = [&](const auto& o) { adapter.report_operator_outcomes(o); };
host_cfg.lookup_descriptor        = [&](const auto& g) { return adapter.lookup_descriptor(g); };
```

The executor only *starts* provider calls. The controller polls their futures
from `step()`, never waits on them, and never holds its own lock while a
provider call runs.

### Out of process

A control-plane binary owns the controller and the adapter. Each host's
`multi_raft_config` hooks are then the application's own RPC to it. The hooks
are `std::function` precisely so that Kythira never chooses that RPC.

`multi_raft_node` carries one such RPC as a worked example
(`cmd/multi_raft_node/capacity_plane.hpp`). It is a JSON-over-HTTP plane on the
cpp-httplib/JSON stack only.

- **One host is the controller.** It runs with `--capacity-role controller`
  and serves `/capacity/*` on `--capacity-port` (default 7003). It owns a
  `docker_quorum_manager`, the threshold policy (shards per node only), a
  memory ledger and a single-process lease.
- **Every other host is a member.** It runs with `--capacity-role member
  --capacity-controller HOST:PORT`, and its placement hooks call the
  controller.
- **`GET /capacity/status`** returns the controller's whole view: nodes,
  shards, intents and counters.
- **A machine the controller creates** is started with `--groups 0` as a
  member. It receives the environment passed with `--capacity-join-env`, so
  it has the cluster's shape without a compose file of its own.

`docker/elastic-capacity-compose.yml` wires three hosts this way. The flags
are listed in `multi_raft_node --help`.

**The plane can create and destroy machines, so it is authenticated.** It
listens on `--bind` like the host's other surfaces. Every host takes a shared
bearer token from `$KYTHIRA_CAPACITY_TOKEN` or `--capacity-token-file` (at
least 16 characters; never a command-line flag, where it would show in
`/proc/<pid>/cmdline`). The controller answers 401 to any request without
`Authorization: Bearer <token>`, members send it, and the controller passes it
to every machine it creates in the same environment variable. A controller
refuses to start on a non-loopback `--bind` without a token. The token
authenticates; it does not encrypt. The plane is plaintext HTTP, like the Raft
port, so it still belongs on a network you trust, where the token keeps any
other process on that network from resizing the cluster.

### Deployment requirement: `lookup_descriptor`

**`lookup_descriptor` is required, not optional.** A newly provisioned machine
holds no replicas. The first AppendEntries for a group it has never seen
creates a replica lazily, and that needs the group's descriptor. Without the
hook, an admission stalls at its first learner. The adapter answers from the
controller's view of the groups it is moving, then from the inner driver if
that driver has a `lookup_descriptor` of its own.

### The decorator's contract

- Ids pass straight through to the inner driver.
- The inner driver's operators go first. A controller operator for a group the
  inner driver already named in the same heartbeat is dropped, never sent
  twice.
- Each host receives only the operators for groups it currently leads, because
  those are the only ones it can apply.
- If the controller throws, the channel degrades to the inner driver alone.
  The controller only ever adds operators; it never removes the inner
  driver's.

### Host-side behaviour the controller relies on

- The leader copies its Raft membership into the shard's descriptor on every
  heartbeat (`multi_raft::sync_leader_membership`), **without bumping the
  epoch**. Bumping it there would be leader-local, and the split apply step
  compares the parent epoch on every replica, so a leader-only bump would fail
  every split that followed.
- A shard report lists each voter or learner whose match index trails the
  leader's last log index by more than `replica_catch_up_lag` (default 64) as
  pending. It also lists a learner the leader does not track yet. Promotion
  waits until the learner is off that list.
- Promotion is `add_replica{as_learner=false}` naming a current learner. No
  new operator was added for it.
- Group nodes carry no provisioning authority: their quorum manager is a no-op,
  and growing the cluster is the controller's job alone.

## Single-writer control

Exactly one controller may provision at a time. The lease decides which one.

- `raft_leadership_lease`: held while this process leads a chosen Raft group.
  Its fencing token is that group's term, so a successor's token is always
  larger.
- `single_process_capacity_lease`: always held, and it checks nothing. Use it
  only where this process is by construction the only one that could provision
  (an embedded appliance, a test). To replace it, start the replacement with a
  larger fencing token.

Every ledger record carries the fencing token of the controller that wrote it.
A controller that sees a newer token in the ledger stops at once and logs
`capacity_second_controller`.

On acquiring the lease, a controller **reconciles before deciding anything**.
It matches each open intent to a machine:

1. by idempotency key, when the manager is keyed;
2. by node id;
3. by arrival: the first machine that booted after the intent was created.

It then completes, admits, reaps or fails each intent. No new provider call goes
out until reconciliation finishes or `reconcile_deadline` passes.

## Configuration

### `elastic_capacity_config`

The defaults are chosen so that turning the feature on and configuring nothing
else cannot take the cluster past three times its starting size. That follows
from one machine at a time, at most four an hour, a ceiling of 3× the floor,
and dry run on.

| Knob | Default | Why that default |
|---|---|---|
| `enabled` | `false` | Off: `step()` returns nothing and calls nothing. |
| `scale_in_enabled` | `false` | Separate from scale-out; a wrong scale-in is the expensive mistake. |
| `dry_run` | `true` | Decide, log and count, but call no provider, write no ledger and send no operator. This is the first step of adoption. |
| `kill_switch` | `false` | Stops every provider call and every operator from the next step on. It can also be set at run time with `set_kill_switch`. |
| `evaluation_interval` | 60 s | The policy is consulted at most this often, and immediately after a split. |
| `min_cluster_size` | 0 → topology floor | 0 means derive it from `topology().total_size()`. |
| `max_cluster_size` | 0 → 3× floor | 0 means derive: 3× the floor, or 3× the first observed size when the topology declares no floor. |
| `max_in_flight_intents` | 1 | One machine changing at a time. |
| `min_provider_call_interval` | 5 min | A rate limit that holds whatever the policy says. It is the `_sustained_for` of the default policy, so a policy that never stops asking still gets one machine per plateau. |
| `max_provisions_per_window` / `provision_window` | 4 / 1 h | The budget guard. With the call interval this caps one hour of runaway at four machines. |
| `provision_deadline` | 10 min | Beyond the p99 provisioning time of every shipped manager. On expiry the intent goes to `reaping`. |
| `join_deadline` | 15 min | Boot plus first heartbeat, with margin. On expiry the machine is reaped and the intent `orphaned`. |
| `admit_deadline` | 30 min | Bounded by snapshot transfer. On expiry the intent is `abandoned` and the machine kept. |
| `drain_deadline` | 60 min | A drain moves every replica. On expiry the machine returns to service and the intent is `abandoned`. |
| `reconcile_deadline` | 2 min | How long reconciliation may hold up deciding after the lease is acquired. |
| `max_moves_per_target` | 2 | Concurrent snapshot transfers onto one machine. |
| `max_moves_cluster_wide` | 8 | Concurrent snapshot transfers in the whole cluster. |
| `operator_retry_interval` | 30 s | How long before an unacknowledged move step is sent again. |
| `operator_busy_backoff` | 2 min | Back-off after a host refuses an operator as `shard_busy`. |
| `move_cooldown_after_split` | 5 min | A shard touched by a split or a merge is not moved for this long. |
| `node_report_staleness` | 30 s | Three default heartbeat intervals. Past this a machine is *unknown*, not absent, and an unknown machine is never a reason to grow. |
| `split_rate_window` | 10 min | The window for the split and merge rates the snapshot reports. |
| `assess_interval` | 60 s | How often `assess_quorum` is asked for the provider's view. |
| `ledger_retention` | 7 d | Terminal intents older than this are compacted. Validation requires at least 2× the longest deadline. |
| `provider_backoff` | 1 min, ×2, 30 min max, 0.2 jitter, 3 attempts | The repository's own `retry_policy_config`. `max_attempts` consecutive failures open the circuit. |
| `circuit_cool_off` | 30 min | How long an open circuit suspends scale-out before one trial call. |
| `placement_refusal_penalty` / `placement_refusal_decay` | 2.0 / 30 min | A placement group that refused a call ranks lower, and the penalty decays over this time. |
| `jitter_seed` | unset | Seeds the back-off jitter so that tests are reproducible. |

The constructor throws `std::invalid_argument` listing every validation error at
once, together with the policy's own errors.

### `threshold_capacity_policy_config`

| Knob | Default | Why that default |
|---|---|---|
| `_enabled` | `false` | Off: `evaluate` always holds. |
| `_shards_per_node` | out 200 / in 80 | Comfortably within one host's tick budget. The in-mark is low enough that a cluster which just grew by a third does not shrink straight back. |
| `_leaders_per_node` | out 80 / in 30 | The share of the write path one machine owns. |
| `_storage` | out 0.75 / in 0.35 | Σused / Σcapacity. 75% leaves room for the snapshots a rebalance writes before it frees anything. |
| `_write_bytes_per_sec` | disabled | No figure is right for every workload, and a wrong default would provision machines for a benchmark. |
| `_overloaded` | out 0.34 / in 0.0 | The fraction of machines asserting overload: scale out above a third, and scale in only when none are. |
| `_min_hysteresis_margin` | 0.6 | Each low watermark must be at most 0.6× its high one. This is the guard `threshold_split_merge_policy` uses, for the same reason. |
| `_sustained_for` | 5 min | A spike lasting one heartbeat must not provision a machine. |
| `_sustained_for_scale_in` | 30 min | 6× the scale-out wait, because removing too early costs a re-provision plus two transfers per shard. |
| `_split_pressure_enabled` | `true` | Scale out on *projected* density. |
| `_horizon_minutes` | 30 | Longer than the p99 provisioning time of every shipped manager, which is the only thing the horizon has to beat. |
| `_repair_topology_floor` | `false` | A deployment that already repairs its floor (a single-group quorum loop, a cloud autoscaler) would otherwise have two repairers fighting. |
| `_scale_out_step` | 1 | Machines one decision asks for. The controller's bounds still apply. |
| `_cooldown` | 10 min | Minimum time between two of this policy's own decisions. |

### Host (`multi_raft_config`)

| Knob | Default | Meaning |
|---|---|---|
| `split_capacity_floor_bytes` | unset | Refuse a split whose children would leave a machine below this many free bytes, according to `capacity_probe`. Unset, or with no probe, the gate is off. |
| `capacity_probe` | unset | Returns `{used, capacity}` bytes for this machine. |
| `replica_catch_up_lag` | 64 | Log entries a replica may trail by and still count as caught up. |
| `report_operator_outcomes` | unset | Where the host reports each operator it accepted or skipped, and why. |
| `lookup_descriptor` | unset | Required for admission; see [the deployment requirement](#deployment-requirement-lookup_descriptor). |

## Metrics and logs

### Counters (`kythira.multiraft.capacity.*`)

| Counter | Dimensions |
|---|---|
| `decision` | `action`, `reason` |
| `refused` | `bound`: `quorum_lost`, `stale_inventory`, `circuit_open`, `backoff`, `max_cluster_size`, `max_in_flight_intents`, `min_provider_call_interval`, `provision_budget`, `scale_in_disabled`, `quorum_not_healthy`, `drain_in_progress`, `intent_in_flight`, `min_cluster_size`, `unknown_node`, `topology_floor`, `kill_switch`, `group_not_in_topology`, `no_placement_group` |
| `intent` | `terminal_state` |
| `provider_call` | `op`, `outcome` (plus a `provider_call.latency` histogram) |
| `provider_result_discarded` | — |
| `admission`, `drain` | `outcome` |
| `move` | `outcome` |
| `shards_moved` | — |
| `operator_skipped` | `reason`, as the host reported it |
| `reconcile` | `outcome` |
| `orphan_reaped` | — |
| `placement` | `outcome` |
| `circuit`, `lease` | `event` |
| `ledger_write` | `outcome` |
| `fencing_conflict`, `policy_error` | — |

### Gauges

`cluster_size`, `cluster_size_floor`, `cluster_size_ceiling`,
`intents_in_flight`, `group_size{group}` against `group_target{group}`, and
`shards_per_node{stat}` and `leaders_per_node{stat}` for `max`, `mean` and
`spread`.

The host adds `split.rejected{gate=capacity}` to its existing split counter
(rather than a new metric) and `kythira.multiraft.shard.allocation.suggestion_ignored`.

### Logs

Every decision is **one** structured record, `capacity_decision`. It carries the
reason, each signal's value and threshold, the projection inputs, the chosen
placement group, the idempotency key and every bound evaluated. An incident
review should not have to join records to read one decision. The other events
are `capacity_refused`, `capacity_lease_acquired` / `capacity_lease_lost`,
`capacity_second_controller`, `capacity_circuit_open`,
`capacity_move_planned` / `capacity_move_done` / `capacity_move_abandoned`,
`capacity_placement_fallback`, `capacity_assess_failed`,
`capacity_group_target_failed`, `capacity_ledger_rejected`,
`capacity_policy_threw` and `capacity_note`.

## Adoption: dry run first

1. **Wire it with `enabled = true` and leave `dry_run` on.** The controller
   decides, logs and counts, and does nothing else.
2. **Watch `decision`, `refused{bound}` and the `capacity_decision` records for
   a few days of real load.** Count how many machines it would have added, and
   when. If that number surprises you, tune the policy, not the bounds.
3. **Set `max_cluster_size` explicitly.** The derived 3× is a safety net, not a
   capacity plan.
4. **Turn `dry_run` off, with scale-in still off.** Watch `admission{outcome}`,
   `shards_moved` and `provider_call{outcome}` through the first few
   provisions.
5. **Only then consider `scale_in_enabled`.** Start with a long
   `_sustained_for_scale_in`.

The kill switch (`set_kill_switch(true)`) stops everything from the next step
on, without a restart.

## Verification

- **Deterministic suites** (no sleeps, manual clock, manual executor):
  - `elastic_capacity_controller_test`: every bound, reconciliation,
    placement, admission, drain and observability.
  - `elastic_shard_placement_driver_test`: the decorator's contract.
  - `elastic_capacity_failover_test`: 24 seeded property runs. After every
    round they assert size within [floor, ceiling], no voter loss, no live
    machine without an intent, and no operator sent to a shard mid-split. It
    also kills the lease holder in each intent state, and covers chaos (a call
    that never returns, a machine that never joins, the lease lost
    mid-admission).
- **Real hosts in process:** `elastic_capacity_fabric_test` admits a new
  machine onto real `multi_raft` hosts over the in-process message fabric,
  through lazy replica creation and learner promotion.
- **Real containers:** `elastic_capacity_docker_test`
  (`tests/docker_chaos/`) runs the end-to-end scenario with no cloud
  credentials:
  1. Three `multi_raft_node` hosts start from
     `docker/elastic-capacity-compose.yml`.
  2. 240 writes split the two starting shards past the high watermark.
  3. The controller creates a fourth container through the container API,
     and admits it as a learner that is promoted to voter.
  4. The test asserts that every shard kept three voters throughout, that
     the container carries the intent's idempotency key, and that nothing is
     left behind afterwards.

  Run it with `cmake --build build --target docker-elastic-capacity-tests`.
  That target builds `kythira-multi-raft-node:dev` and sets
  `KYTHIRA_DOCKER_INTEGRATION_TESTS=1`; without that variable the test skips.
  It honours `KYTHIRA_CONTAINER_RUNTIME`. Under rootless Podman it mounts the
  user's Podman socket in place of Docker's, because Podman serves the
  Docker-compatible API. The arm64 Docker smoke workflow (manual dispatch)
  runs it.
- **Snapshot-boundary replication:** `raft_snapshot_boundary_replication_test`
  covers the core Raft fix the Docker scenario exposed (a split child that
  could never replicate past its starting snapshot). It is described in the
  design's §15.
