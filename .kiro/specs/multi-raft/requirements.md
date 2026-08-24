# Requirements Document

## Introduction

Kythira today hosts exactly one Raft group per `node<Types>` instance. One
`node` owns one log, one state machine, one persistence engine, one network
server (with a single handler slot per RPC type), and one cluster
configuration. Capacity is therefore bounded by what a single Raft group can
sustain: one leader serialises every write, one log absorbs every append, one
state machine applies every command, and one snapshot must be shipped whole to
a lagging follower.

**Multi-Raft** removes that bound by partitioning the application's data into
*shards*, giving each shard its own independent Raft group, and hosting many
groups per process. Writes to different shards commit in parallel on different
leaders; a snapshot is one shard's worth of state, not the cluster's; and
adding machines adds throughput because shards (and their leaderships) spread
across them.

The two reference designs this document draws on:

- **TiKV Multi-Raft** (<https://tikv.org/deep-dive/scalability/multi-raft/> and
  <https://tikv.org/deep-dive/scalability/data-sharding/>) — shards are
  contiguous *key ranges* ("Regions"); a range that grows past a threshold
  **splits** (`[a, c) -> [a, b) + [b, c)`), adjacent small ranges **merge**
  (`[a, b) + [b, c) -> [a, c)`); split and merge are metadata operations that
  move no data; the store batches all ready groups into one write batch per
  loop iteration and *reuses one connection between two nodes for many Raft
  groups*; a central Placement Driver collects store and region heartbeats and
  hands back advisory operators (add replica, remove replica, transfer leader,
  split, merge).
- **MicroRaft main abstractions**
  (<https://microraft.io/docs/main-abstractions/>) — a node is identified by a
  `[group id, node id]` pair precisely so that "multiple independent Raft
  groups [can operate] in a single JVM"; the consensus core is separated by
  narrow contracts from transport, persistence, and the state machine; the
  store deliberately *does not* persist state-machine state; a
  not-leader response carries the current leader's endpoint so the client can
  re-route; a per-node report feeds discovery and monitoring.

Kythira already has most of the seams both designs rely on: pluggable
`network_client`/`network_server`, `persistence_engine`, `state_machine`,
`metrics`, and `logger` concepts; joint-consensus membership changes; learners;
snapshots; and a `quorum_manager` that already models a cluster-wide policy
authority (`desired_topology`, `provision_node`, `assess_quorum`). What it does
not have is group identity on the wire, a shard-to-group routing table, a
per-process host that owns many groups, or any split/merge protocol.

**The emphasis of this specification is the signal surface**: how a *user* —
an application developer, an operator, or an external control plane — tells
Kythira when to split and when to merge. Requirement 6 through Requirement 10
cover that surface; the rest of the document specifies the machinery those
signals drive.

## Glossary

- **Shard** — a contiguous, half-open range `[start, end)` over an
  application-defined, totally ordered key domain. Every key in the domain
  belongs to exactly one live shard.
- **Shard key (`Key`)** — the application's routing key type. Totally ordered
  and serialisable. Not required to be `std::string`; the *only* thing Kythira
  does with it is compare and serialise it.
- **Group / Raft group** — the Raft consensus instance that replicates one
  shard. One group ⇔ one shard, one-to-one, for the lifetime of the shard.
- **Group id (`GroupId`)** — cluster-unique identifier for a group. Together
  with a `NodeId` it forms the `[group id, node id]` composite identity
  MicroRaft describes.
- **Replica / peer** — one `node<Types>` instance belonging to one group,
  hosted on one machine.
- **Shard epoch** — `{version, conf_version}`, mirroring TiKV's `RegionEpoch`.
  `version` increments on every range change (split, merge); `conf_version`
  increments on every membership change. Requests carry the epoch they were
  routed against; a mismatch is rejected rather than mis-served.
- **Shard descriptor** — `{group_id, range, epoch, members, leader_hint}`; the
  routing-table row for one shard.
- **Shard map** — the routing table: an ordered map from range start to shard
  descriptor, covering the whole key domain.
- **Host (`multi_raft<Types>`)** — the per-process object that owns every local
  group replica, the shared transport demultiplexer, the tick driver, the shard
  map, and the split/merge orchestration.
- **Placement driver (PD)** — an optional cluster-scope authority that observes
  every shard and issues advisory operators. The direct analogue of TiKV's PD,
  and the natural extension of Kythira's existing `quorum_manager`.
- **Split/merge policy** — a user-supplied object, consulted on the leader of
  each group, that turns a statistics snapshot into a split or merge
  *proposal*. The primary user signal channel.
- **Derived child** — on a split, the child that inherits the parent's group
  id, log, and term. The other children are new groups starting from a
  synthetic snapshot.
- **Colocated replicas** — two groups whose replica sets occupy the same set of
  physical nodes. A precondition for merge.

## Requirements

---

### Requirement 1: Group Identity and Isolation

**User Story:** As a library user, I want many independent Raft groups in one
process, each with its own log, state machine, and durable state, so that
throughput scales with the number of shards rather than being capped by one
leader.

#### Acceptance Criteria

1. Every Raft replica SHALL be identified by the pair `[group_id, node_id]`.
   `node_id` alone SHALL NOT be assumed unique within a process.
2. `GroupId` SHALL be a template parameter constrained by a new
   `raft_group_id` concept requiring `std::regular`, `std::totally_ordered`,
   and hashability, defaulting to `std::uint64_t`.
3. Each group SHALL own a private log, `voted_for` record, `current_term`
   record, snapshot, and state-machine instance. No two groups SHALL share
   mutable Raft state.
4. Two groups hosted in the same process SHALL be able to hold different terms,
   different leaders, and different cluster configurations simultaneously,
   with no interaction between their elections.
5. A group's durable state SHALL be namespaced by `group_id` such that
   destroying one group cannot delete or corrupt another's state.
6. Following MicroRaft's separation, the Raft store SHALL NOT be responsible for
   persisting state-machine-internal state; snapshotting remains the existing
   `get_state()` / `restore_from_snapshot()` contract, evaluated per group.
7. The existing single-group `node<Types>` API SHALL remain usable, unchanged
   and uncompiled-against-`multi_raft`, for callers that want exactly one group.

---

### Requirement 2: Shard Model and the Shard Map

**User Story:** As an application developer, I want to define my own key domain
and have Kythira route commands to the right group, so that I am not forced
into a byte-string keyspace I do not use.

#### Acceptance Criteria

1. A shard's range SHALL be a half-open interval `[start, end)` over a
   user-supplied `Key` type satisfying a new `shard_key` concept
   (`std::totally_ordered`, copyable, serialisable via the existing
   `rpc_serializer` machinery).
2. The range type SHALL represent unbounded ends explicitly, so that a
   single-shard cluster is `(-∞, +∞)` and the first split of it is expressible
   without a sentinel key value the application must reserve.
3. At every instant, across all live shards, the union of shard ranges SHALL
   cover the key domain exactly once — no gaps, no overlaps. This is the
   system's primary safety invariant and SHALL be assertable in tests.
4. The host SHALL maintain a shard map: an ordered container from range start
   to shard descriptor, supporting `lookup(key) -> descriptor` in
   `O(log n)` and `range_scan(lo, hi) -> [descriptor]`.
5. Command routing SHALL be derived from a user-supplied `partitioner` that
   extracts a `Key` from a serialised command, so that Kythira never needs to
   understand command payloads.
6. The shard map SHALL be maintained on every node, not only on leaders, and
   SHALL be reconstructible after a restart from durable state without
   contacting a placement driver.

---

### Requirement 3: Shard Epoch and Stale-Request Rejection

**User Story:** As an operator, I want a request routed against an out-of-date
shard map to be *rejected*, not served against the wrong shard, so that a split
or merge racing a client cannot produce a lost or misplaced write.

#### Acceptance Criteria

1. Every shard descriptor SHALL carry an epoch `{version, conf_version}`, both
   monotonically non-decreasing for a given `group_id`.
2. `version` SHALL increment on every range change. On a split into N children,
   each child's `version` SHALL exceed the parent's. On a merge, the surviving
   shard's `version` SHALL be `max(source.version, target.version) + 1`.
3. `conf_version` SHALL increment on every committed membership change,
   including learner add/remove and promotion.
4. Every client request that names a shard SHALL carry the epoch it was routed
   against, and every cross-group control RPC SHALL carry the epoch it was
   issued against.
5. WHEN a replica receives a request whose epoch is behind its own THEN it
   SHALL reject the request with a `shard_epoch_mismatch_exception` that
   carries the current descriptors for the range the request targeted, so the
   caller can refresh and retry without a placement-driver round trip.
6. WHEN a replica receives a request whose epoch is *ahead* of its own THEN it
   SHALL reject it as `shard_epoch_ahead_exception` and SHALL NOT adopt the
   epoch from the request. Epoch advances only by applying a committed entry.
7. Epoch checks SHALL be evaluated at apply time as well as at admission time,
   because an entry proposed under one epoch may commit after that epoch has
   advanced.

---

### Requirement 4: Shared Transport, Demultiplexed by Group

**User Story:** As an operator running a thousand shards on three machines, I
want one connection per machine pair rather than a thousand, so that file
descriptors, TLS handshakes, and syscall overhead do not scale with shard
count.

#### Acceptance Criteria

1. All Raft RPC message types (`request_vote_request`,
   `request_pre_vote_request`, `append_entries_request`,
   `install_snapshot_request`, `fetch_log_entries_request`, and their
   responses) SHALL carry a `group_id` field.
2. The `group_id` field SHALL default to a zero/empty value so that existing
   single-group deployments and every existing serializer round-trip remain
   wire-compatible; a decoder that encounters no `group_id` SHALL treat it as
   the default group.
3. All five serializers (`json`, `cbor`, `ion`, `protobuf`, and the gRPC
   message conversion) SHALL round-trip the new field, and the protobuf schema
   SHALL add it as a new field number rather than renumbering existing fields.
4. A `multi_group_network_server<Server>` adapter SHALL register exactly one
   handler per RPC type with the underlying transport and dispatch each
   received request to the handler registered for `request.group_id()`.
5. A `group_scoped_client<Client>` / `group_scoped_server<Server>` view SHALL
   satisfy the existing `network_client` / `network_server` concepts, stamping
   the group id on egress and having it already stripped on ingress, so that
   `node<Types>` requires **no transport-related change** to participate in
   multi-Raft.
6. Per TiKV's stated design, the transport SHALL reuse one connection between a
   pair of nodes across all groups.
7. WHEN a request arrives for a `group_id` with no local replica THEN the
   adapter SHALL apply Requirement 12's lazy-replica-creation rule rather than
   silently dropping the message.

---

### Requirement 5: Batched Multi-Group Tick

**User Story:** As an operator, I want per-tick cost to scale with the number
of *active* groups rather than the number of *existing* groups, so that idle
shards are nearly free.

#### Acceptance Criteria

1. The host SHALL expose a single `tick()` entry point that advances every
   local group, replacing the current pattern of one election-timer thread and
   one heartbeat-timer thread per `node`.
2. `tick()` SHALL process groups in three phases mirroring TiKV's ready loop:
   (a) collect ready groups and persist all their pending appends, (b) send
   outbound messages, (c) apply committed entries.
3. WHERE the persistence engine advertises an optional batching extension
   (`begin_batch()` / `commit_batch()`), the host SHALL wrap phase (a) in one
   batch so that N groups cost one `fsync` per tick rather than N.
4. WHERE the persistence engine does not advertise batching, behaviour SHALL be
   correct but unbatched, detected structurally via `if constexpr` in the
   established codebase idiom.
5. The host SHALL support *hibernation*: a group with a stable leader, no
   in-flight proposals, and all followers caught up MAY skip heartbeats until
   woken by a client request, an incoming RPC, or a configuration change.
   Hibernation SHALL be configurable and default to enabled above a
   configurable group count.
6. Each group SHALL be driven serially — following MicroRaft's
   `RaftNodeExecutor` actor model — by a striped executor with a bounded thread
   count, never one thread per group.
7. Metrics SHALL report ready-group count, hibernating-group count, batch size,
   and tick duration per phase.

---

### Requirement 6: Declarative Split/Merge Policy — the Primary Signal Channel

**User Story:** As an application developer, I want to express *when* my shards
should split and merge as a small policy object evaluated against measured
statistics, so that I do not have to write a control loop.

#### Acceptance Criteria

1. A `split_merge_policy` concept SHALL be defined, requiring:
   - `evaluate_split(const shard_stats&) -> split_decision<Key>`
   - `evaluate_merge(const shard_stats& self, const shard_stats& sibling) -> merge_decision`
   - `cooldown() -> std::chrono::milliseconds`
2. The policy SHALL be consulted **only on the leader** of a group, on a
   configurable `policy_interval` tick, and SHALL be given a read-only
   `shard_stats` snapshot.
3. The policy SHALL NOT be required to be deterministic across replicas. Its
   output is a *proposal* made by one leader; the resulting decision is frozen
   into a log entry and every replica applies that frozen decision. This
   SHALL be stated in the concept's documentation, because the alternative
   assumption is a correctness trap.
4. The policy SHALL NOT perform I/O and SHALL NOT be able to mutate Raft state
   directly. It returns values; the host validates and enacts them.
5. `shard_stats` SHALL carry at minimum: the shard descriptor; approximate size
   in bytes and approximate key count; log size in bytes and last-applied
   index; read and write QPS; read and write bytes/sec; applied-entries/sec;
   p99 apply latency; time since last split; time since last merge; voting and
   learner member counts; time since this replica became leader; and the
   hot-key sample set from Requirement 8.
6. `split_decision<Key>` SHALL be `{bool split; std::vector<Key> at_keys;
   split_reason reason;}`. A vector — not a single key — so that batch split
   (one entry producing N children, per TiKV RFC 0006) is expressible and a
   fast-filling shard cannot be outrun by one-split-at-a-time.
7. WHEN `split == true` and `at_keys` is empty THEN the host SHALL request keys
   from the state machine per Requirement 9, treating the policy as having said
   "split, you choose where".
8. `merge_decision` SHALL identify the merge direction (into the left sibling
   or the right sibling), since the two are not equivalent — the surviving
   shard's replicas are the ones that must absorb state.
9. Every policy decision, accepted or rejected, SHALL emit a structured log
   entry and a metric carrying the reason. A rejected split that is invisible
   is untunable.

---

### Requirement 7: Default Threshold Policy and Oscillation Safety

**User Story:** As an operator, I want a working default policy with knobs
shaped like the ones I already know from TiKV, and I want the system to refuse
a configuration that would make shards split and merge forever.

#### Acceptance Criteria

1. A `threshold_split_merge_policy` SHALL ship as the default, with knobs:
   `shard_max_size_bytes`, `shard_split_size_bytes`, `shard_max_keys`,
   `shard_split_keys`, `shard_merge_max_size_bytes`, `shard_merge_max_keys`,
   `split_merge_interval`, `batch_split_limit`, and the load-split knobs from
   Requirement 8.
2. Defaults SHALL be `shard_max_size_bytes = 144 MiB`,
   `shard_split_size_bytes = 96 MiB`, `shard_max_keys = 1'440'000`,
   `shard_split_keys = 960'000`, `shard_merge_max_size_bytes = 20 MiB`,
   `shard_merge_max_keys = 200'000`, `split_merge_interval = 1h`,
   `batch_split_limit = 10`.
3. The policy SHALL split when approximate size exceeds `shard_max_size_bytes`
   or approximate key count exceeds `shard_max_keys`, producing split keys
   every `shard_split_size_bytes` (respectively `shard_split_keys`) up to
   `batch_split_limit`.
4. The policy SHALL propose a merge only when *both* the shard and its chosen
   sibling are below `shard_merge_max_size_bytes` **and**
   `shard_merge_max_keys`, and neither has split within `split_merge_interval`.
5. `validate()` SHALL **reject** a configuration in which
   `2 * shard_merge_max_size_bytes >= shard_split_size_bytes` (respectively for
   keys), with an error message naming the oscillation it prevents: two shards
   each just under the merge threshold merge into one shard that is
   immediately over the split threshold, and the pair oscillates forever.
   TiKV's load-split RFC states the same rule of thumb — the merge threshold
   must sit well below the split threshold.
6. `split_merge_interval` SHALL be enforced by the host as a hard gate on every
   channel, not only inside the default policy, so that a custom policy cannot
   accidentally remove the anti-oscillation guard.
7. The policy's `validate()` and `get_validation_errors()` SHALL follow the
   shape already established by `raft_configuration`.

---

### Requirement 8: Load-Based Split Signal

**User Story:** As an operator whose hot shard is small but overwhelmed, I want
it split by *load*, not only by size, so that the resulting shards' leaderships
can be scattered across machines.

#### Acceptance Criteria

1. The host SHALL sample routing keys on the leader of each shard whose read or
   write rate exceeds `load_split_qps_threshold` or
   `load_split_bytes_threshold`.
2. Sampling SHALL follow TiKV RFC 0045: select a bounded number of candidate
   keys (default 20) by probability sampling, then for
   `load_split_duration` (default 10 s) count, for each candidate, accesses
   falling strictly left of it versus at-or-right of it.
3. IF load falls below the threshold during the sampling window THEN sampling
   SHALL be abandoned and no split proposed. Splitting for a load spike shorter
   than the window costs more than it saves.
4. IF more than a configurable fraction (default 99 %) of accesses to every
   candidate fall on one side THEN the load is concentrated on a single key,
   a split cannot help, and the shard SHALL be marked ineligible for
   load-based split for a configurable back-off period.
5. Otherwise the candidate with the most balanced left/right access counts
   SHALL be offered to the policy in `shard_stats.hot_key_samples`, and the
   default policy SHALL propose a split at it.
6. A shard split for load reasons SHALL additionally request a *scatter* from
   the placement driver, since a load split that leaves both children's leaders
   on the same machine has accomplished nothing.
7. Sampling SHALL be off by default and its per-request cost SHALL be a single
   predictable branch when off.

---

### Requirement 9: State-Machine-Derived Split Hints and Veto

**User Story:** As an application developer, I want Kythira to ask *my* state
machine where its data actually sits, and I want to be able to forbid a split
that would break an invariant of mine.

#### Acceptance Criteria

1. An optional `splittable_state_machine` extension concept SHALL be defined,
   detected structurally so that existing state machines are unaffected:
   - `approximate_size_bytes() -> std::size_t`
   - `approximate_key_count() -> std::size_t`
   - `suggest_split_keys(std::size_t max) -> std::vector<Key>`
   - `can_split_at(const Key&) -> bool`
   - `split_state(const std::vector<Key>& at) -> std::vector<std::vector<std::byte>>`
   - `absorb(const std::vector<std::byte>& other_state, const shard_range<Key>& other_range) -> void`
2. `suggest_split_keys` and `can_split_at` SHALL be called **only on the
   leader**, before proposing, and their results SHALL be frozen into the
   proposed entry.
3. `split_state` and `absorb` SHALL be called on **every replica**, at apply
   time, and SHALL be deterministic: given the same prior state and the same
   split keys, every replica SHALL produce byte-identical outputs.
4. WHEN the policy or an operator names a split key for which
   `can_split_at(key)` returns `false` THEN the host SHALL discard that key and
   fall back to `suggest_split_keys`. This is the application's veto — the
   place to refuse a split that would cut through a composite entity or an
   in-flight multi-key operation.
5. WHEN every candidate key is vetoed THEN the split SHALL be abandoned with
   `no_valid_split_key_exception`, logged at a rate-limited level, and retried
   no sooner than the policy cooldown.
6. WHERE the state machine does not implement the extension, size- and
   key-count-based split SHALL be unavailable for that state machine and the
   host SHALL say so once at construction rather than silently never splitting.
7. `absorb` SHALL be the merge counterpart of `split_state`, and the two SHALL
   be documented as required to round-trip: splitting at `k` then absorbing the
   right part back SHALL restore the original state exactly.

---

### Requirement 10: Imperative Admin Signals

**User Story:** As an operator, I want to force a split or merge right now,
pre-split an empty keyspace before a bulk load, and freeze automatic decisions
for one shard while I investigate.

#### Acceptance Criteria

1. The host SHALL expose, returning futures that resolve when the operation is
   committed **and applied**:
   - `split_shard(GroupId, std::vector<Key> at_keys, split_options)`
   - `merge_shards(GroupId source, GroupId target, merge_options)`
   - `pre_split(std::vector<Key> boundaries)` for an empty keyspace
   - `freeze_shard(GroupId)` / `thaw_shard(GroupId)`
   - `set_automatic_split_merge_enabled(bool)` as a global kill switch
2. Failures SHALL be typed, never a bare `std::runtime_error`:
   `shard_epoch_mismatch_exception`, `shard_not_adjacent_exception`,
   `shard_busy_exception`, `shard_alignment_required_exception`,
   `split_key_out_of_range_exception`, `no_valid_split_key_exception`,
   `shard_merging_exception`, and the existing not-leader error.
3. An admin command SHALL be routed to the shard's leader; when issued against
   a follower it SHALL fail with the leader hint attached, following MicroRaft's
   not-leader contract.
4. A frozen shard SHALL be excluded from every automatic channel — policy, load
   split, and placement-driver operators — but SHALL still accept explicit
   admin commands, so that freezing does not lock an operator out of their own
   escape hatch.
5. `set_automatic_split_merge_enabled(false)` SHALL take effect within one
   policy interval and SHALL NOT abort a split or merge already in flight;
   in-flight operations always run to completion or rollback.
6. `pre_split` SHALL be rejected unless every affected shard is empty, since it
   exists to avoid the split storm of a bulk load into one shard, not as an
   alternative to `split_shard`.

---

### Requirement 11: Split Protocol

**User Story:** As a library user, I want a split to be atomic, data-movement-
free, and crash-safe, so that no committed command is lost or duplicated and no
key is briefly served by two shards or none.

#### Acceptance Criteria

1. A split SHALL be enacted by a single log entry of a new
   `entry_type::split`, proposed by the parent group's leader and applied
   deterministically by every parent replica.
2. Before proposing, the leader SHALL obtain the new group ids and the new
   replica ids from the placement driver (TiKV's `AskBatchSplit`). IF the
   placement driver is unavailable THEN the split SHALL be abandoned and
   retried later. The leader SHALL NEVER invent ids locally, because every
   replica must derive identical ids and only a cluster-scope authority can
   guarantee uniqueness.
3. The split entry SHALL carry: the parent's group id and epoch; the ordered
   split keys; the full derived descriptor for every child (id, range, epoch,
   members); and a `right_derive` flag naming which child inherits the parent's
   group id, log, and term.
4. Applying the split entry SHALL, on every replica, in order: freeze the
   parent; call `state_machine.split_state(at_keys)`; durably create each
   non-derived child's initial state; narrow the derived child's range and
   restore its state blob; bump epochs; publish the new shard map rows; unfreeze.
5. A non-derived child SHALL start from a **synthetic snapshot**
   `{last_included_index = parent_apply_index, last_included_term = parent_term,
   configuration = derived from the parent's, state_machine_state = child_blob}`
   with an empty log. The parent's log SHALL NOT be copied. This is what makes
   split a metadata operation, per TiKV's stated advantage of range
   partitioning.
6. The child's durable initial state and the parent's advanced apply index
   SHALL be made durable atomically. WHERE the persistence engine advertises
   batching this SHALL be one batch; otherwise the child SHALL be written
   first and split application SHALL be idempotent, so that replaying the split
   entry after a crash is a no-op when the child already exists at that epoch.
7. Children SHALL start as followers with staggered randomised election
   timeouts, EXCEPT the child replica colocated with the parent's leader, which
   MAY campaign immediately, minimising the unavailability window.
8. The parent SHALL reject client requests between freeze and unfreeze with
   `shard_epoch_mismatch_exception` carrying the new descriptors, so a client
   caught by the split re-routes on its own.
9. After the split is applied, the leader SHALL report the new descriptors to
   the placement driver (TiKV's `ReportBatchSplit`).
10. A split SHALL be refused while the group is in joint consensus, while it is
    a merge source or target, or while another split is in flight for it.

---

### Requirement 12: Lazy Replica Creation

**User Story:** As a library user, I want a node that missed a split — because
it was down, or because the parent compacted past the split entry — to acquire
the child replica correctly rather than dropping its messages.

#### Acceptance Criteria

1. WHEN an RPC arrives for a `group_id` with no local replica THEN the host
   SHALL consult the shard map; IF the descriptor lists this node as a member
   THEN it SHALL create an *uninitialised* replica (empty log, empty state,
   epoch zero) and let the normal `InstallSnapshot` path populate it.
2. IF the descriptor is unknown locally THEN the host SHALL query the placement
   driver once, rate-limited, before deciding.
3. IF the shard map or the placement driver says this node is *not* a member of
   that group THEN the message SHALL be dropped and a `stale_group_message`
   metric incremented. A node SHALL NEVER create a replica purely because a
   message named it.
4. A group id that has been merged away or whose replica was removed SHALL be
   recorded in a durable tombstone set, and messages for a tombstoned group
   SHALL be dropped without recreating the replica. Without this, a lagging
   peer's stale `AppendEntries` resurrects a destroyed replica.
5. Tombstones SHALL be garbage-collected on a configurable horizon well beyond
   the maximum expected partition duration.

---

### Requirement 13: Merge Protocol

**User Story:** As an operator whose data has been deleted, I want small
adjacent shards to combine so that shard count — and its per-shard overhead —
comes back down.

#### Acceptance Criteria

1. A merge SHALL require, checked at proposal time and re-checked at apply
   time: the two ranges are adjacent (`source.end == target.start` or
   `target.end == source.start`); both epochs match the coordinator's view;
   both shards' replica sets are **colocated on the same set of nodes**;
   neither is in joint consensus; neither is splitting, merging, or frozen.
2. WHERE the replica sets are not colocated, the merge SHALL fail with
   `shard_alignment_required_exception`, and the placement driver SHALL be
   asked to align them by add/remove replica operations before the merge is
   retried. Colocation is required because each target replica absorbs state
   from its *local* source replica; a target replica with no local source peer
   cannot apply the commit.
3. The merge SHALL be a three-entry protocol, following TiKV:
   - **`entry_type::merge_prepare`**, proposed on the **source** leader,
     carrying both descriptors, both epochs, and `min_index` = the minimum
     `match_index` across all source voting replicas. On apply the source
     enters the `merging_source` state: it rejects new proposals and reads with
     `shard_merging_exception` and refuses further configuration changes.
   - **`entry_type::merge_commit`**, proposed on the **target** leader,
     carrying the source descriptor and the source's log entries from
     `min_index + 1` through the `merge_prepare` index. On apply each target
     replica force-appends and force-applies those entries to its local source
     replica so it stands exactly at the prepare index, then calls
     `target_sm.absorb(source_sm.get_state(), source_range)`, extends its range
     to cover the source's, sets `version = max(source.version,
     target.version) + 1`, destroys and tombstones the local source replica,
     and publishes the updated shard map.
   - **`entry_type::merge_rollback`**, proposed on the **source** leader,
     returning the source to `stable` and resuming service.
4. `min_index` exists so that the carried entry tail is bounded: every source
   replica is known to hold everything up to `min_index`, so only the remainder
   need travel inside `merge_commit`.
5. Rollback SHALL be safe by construction, not by timing. The source SHALL NOT
   roll back unilaterally. The default protocol SHALL be an explicit
   handshake: the source leader asks the target leader to abandon; the target
   leader commits a `merge_abandoned` record in the **target's** log; only on
   observing that committed record does the source propose `merge_rollback`.
   A lease-based variant MAY be offered as an escape hatch, and IF offered it
   SHALL document its bounded-clock-skew assumption explicitly, since Kythira
   has no clock-synchronisation guarantee today.
6. IF the target leader has already proposed `merge_commit` THEN it SHALL
   refuse the abandon request. Commit wins over rollback, always.
7. Clients of the source shard SHALL receive `shard_epoch_mismatch_exception`
   carrying the surviving target descriptor, and SHALL re-route to the target.
8. A merge SHALL be abandoned — not retried in place — if the target splits
   while the merge is in flight.

---

### Requirement 14: Placement Driver

**User Story:** As an operator, I want a cluster-scope authority that sees every
shard on every machine and rebalances replicas, leaderships, and shard
boundaries, because no single group's leader has the information to do it.

#### Acceptance Criteria

1. A `shard_placement_driver` concept SHALL be defined as the shard-aware
   extension of the existing `quorum_manager`, requiring:
   - `allocate_shard_ids(count) -> future<std::vector<shard_id_allocation>>`
   - `report_shard_heartbeat(shard_report) -> future<std::vector<shard_operation>>`
   - `report_split(parent, children) -> future<void>`
   - `report_merge(source, target) -> future<void>`
   - `report_node_heartbeat(node_report) -> future<void>`
2. `shard_report` SHALL carry, following TiKV's region heartbeat: group id,
   epoch, range, leader identity, all replica identities, count of replicas
   believed down, approximate size, approximate key count, and read/write
   throughput.
3. `node_report` SHALL carry, following TiKV's store heartbeat: total and
   available capacity, local shard count, local leader count, read/write rates,
   snapshot send/receive counts, an overload flag, and placement-group labels.
4. `shard_operation` SHALL be a variant of: `add_replica`, `remove_replica`,
   `transfer_leader`, `split{at_keys}`, `merge{into}`, `scatter`. The first
   three map directly onto Kythira's existing `add_server`, `remove_server`,
   and a new leader-transfer path.
5. Operators SHALL be **advisory**, matching TiKV's explicit contract that
   "operators are only suggestions to the Region leader, which can be skipped".
   A leader SHALL skip an operator whose preconditions no longer hold, and the
   driver SHALL reissue on the next heartbeat.
6. Each operator SHALL carry an `operation_id` and the epoch it was computed
   against; an operator whose epoch is stale SHALL be discarded on receipt.
7. A `no_op_shard_placement_driver` SHALL ship as the default for static,
   pre-split deployments, allocating ids from a locally configured reserved
   range and returning no operators — the same shape as
   `no_op_quorum_manager`.
8. Heartbeat cadence SHALL be configurable, and shard heartbeats SHALL be
   batched into one call per interval rather than one call per shard.

---

### Requirement 15: Signal Arbitration and Concurrency Limits

**User Story:** As an operator, I want the four signal channels to have a
defined precedence and a bounded blast radius, so that a policy, a placement
driver, and my own admin command cannot fight each other or stampede the
cluster.

#### Acceptance Criteria

1. Each shard SHALL have an explicit operation state:
   `stable | splitting | merging_source | merging_target | frozen | tombstoned`.
   Transitions SHALL be the only way a split or merge starts, making
   conflicting concurrent operations impossible by construction rather than by
   check-then-act.
2. Signal precedence SHALL be, highest first: (1) explicit admin command,
   (2) placement-driver operator, (3) local policy. State-machine hints
   (Requirement 9) SHALL never initiate an operation; they only choose or veto
   keys for an operation another channel initiated.
3. An accepted admin command SHALL suspend the automatic channels for the
   affected shards until it resolves.
4. A cluster-wide `max_concurrent_split_merge` limit SHALL bound how many
   operations run at once, defaulting conservatively, following the intent of
   TiKV's schedule-limit design. The limit SHALL be enforced on the leader
   *before* proposing, never by aborting a committed operation.
5. `split_merge_interval` SHALL gate every channel: no shard SHALL be merged
   within that interval of its own creation by split.
6. WHEN two channels propose contradictory operations for the same shard in the
   same interval THEN the higher-precedence one SHALL win and the loser SHALL
   be logged with the reason `preempted_by`, never silently discarded.
7. Every rejection SHALL name its gate — cooldown, concurrency limit, state,
   epoch, veto, alignment — in both the log line and the metric dimension.

---

### Requirement 16: Client Routing

**User Story:** As an application developer, I want to submit a command with a
key and have it reach the right leader, retrying correctly through splits,
merges, and leader changes.

#### Acceptance Criteria

1. The host SHALL expose `submit_command(Key, command, timeout)` which resolves
   the key through the shard map, routes to that shard's leader, and returns
   the state-machine result.
2. It SHALL also expose `submit_command(GroupId, shard_epoch, command, timeout)`
   for clients holding a cached route, enforcing the supplied epoch.
3. A not-leader response SHALL carry the current leader's identity, per
   MicroRaft's contract, and the client SHALL retry against it without a
   placement-driver round trip.
4. A `shard_epoch_mismatch_exception` SHALL cause the shard map to be refreshed
   from the descriptors carried in the response and the request retried, up to
   a bounded retry count with backoff.
5. `read_state` SHALL be available per shard, honouring the existing read-index
   semantics.
6. Commands spanning more than one shard SHALL be explicitly out of scope. The
   host SHALL reject a command whose partitioner yields keys in more than one
   shard with `cross_shard_command_exception` rather than silently applying it
   to one of them.

---

### Requirement 17: Observability

**User Story:** As an operator, I want to see why the system split or merged,
and to diagnose an oscillation or a stuck merge without attaching a debugger.

#### Acceptance Criteria

1. The host SHALL emit metrics for: live shard count, per-node leader count,
   split and merge attempts and outcomes by reason, split/merge duration,
   shards in each operation state, epoch-mismatch rejections, lazy replica
   creations, tombstone hits, hibernating group count, and tick phase durations.
2. Every split and merge — proposed, applied, rejected, rolled back — SHALL
   produce a structured log entry carrying group id, epoch before and after,
   the deciding channel, and the reason.
3. A `shard_report`-shaped debug snapshot SHALL be obtainable for any local
   shard without taking the group's lock for longer than a read, extending the
   existing `debug_state()` pattern.
4. Following MicroRaft's `RaftNodeReport` / `RaftNodeReportListener`, a
   listener interface SHALL let embedders subscribe to per-group role, term,
   commit index, and membership changes for feeding discovery and monitoring.

---

### Requirement 18: Testing and Verification

**User Story:** As a maintainer, I want the safety invariants of sharding
checked mechanically, because a split/merge bug corrupts data silently.

#### Acceptance Criteria

1. A property test SHALL assert, after every operation in a randomised
   split/merge/membership workload, that live shard ranges tile the key domain
   exactly once (Requirement 2.3).
2. A property test SHALL assert that no committed command is lost or applied
   twice across a split or a merge, by reconciling a shadow model against the
   union of all shards' state.
3. A property test SHALL assert epoch monotonicity per group and that a request
   carrying a stale epoch is always rejected, never served.
4. Crash-consistency tests SHALL inject a failure at each step of split apply
   and merge apply using the existing `fiu_do_on` fault-injection machinery,
   with new fault points under `raft/multiraft/`, and assert recovery to a
   consistent state.
5. An oscillation test SHALL drive the default policy with a workload sitting
   between the merge and split thresholds and assert that split and merge
   counts stay bounded over a long run.
6. A scale test SHALL run at least 1000 groups across a simulated three-node
   cluster and assert that per-tick CPU tracks the *active* group count, not
   the total.
7. All tests SHALL run under both Docker and rootless Podman per the project's
   container-runtime rules, and SHALL avoid piping multi-process test output
   through `tail`/`head`.

---

## Out of Scope

- **Cross-shard transactions.** No two-phase commit, no distributed snapshot
  isolation. Requirement 16.6 makes the boundary explicit and enforced.
- **Automatic key-domain discovery.** The application supplies the partitioner
  and the ordering; Kythira does not infer them.
- **Follower/learner reads as a hot-spot remedy.** Orthogonal, and TiKV's own
  analysis in RFC 0045 argues split-plus-scatter is the better primitive for a
  single hot shard.
- **Buckets** (TiKV RFC 0082's sub-region statistics unit). A worthwhile later
  refinement of the load signal; the `shard_stats` structure is designed so it
  can be added without breaking the policy concept.
- **Rewriting `node<Types>`.** Multi-Raft is built as a layer above it. The
  only changes to `node<Types>` are the admin-entry hook (Requirement 11) and
  the group-id field on messages (Requirement 4), both additive.
