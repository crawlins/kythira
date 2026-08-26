# Implementation Plan — Multi-Raft

## Status: In progress — Phases 1-9 complete (tasks 1-26)

**Last Updated**: August 26, 2026

This plan implements `.kiro/specs/multi-raft/design.md`. Twelve phases, 36 tasks.
Phases 1–3 are strictly additive and cannot regress single-group behaviour;
Phase 4 contains the only changes to `include/raft/raft.hpp` (two methods, one
enum extension, one apply-loop branch).

## Overview

Sharding is added as a layer **above** `node<Types>`, not inside it. A new host
object `multi_raft<Types, Key, GroupId>` owns many groups, demultiplexes one
shared transport across them by `group_id`, drives them from one batched tick,
and orchestrates split and merge. `node<Types>` participates unmodified because
the transport and store it is handed are *group-scoped views* satisfying the
existing concepts.

The user-facing signal surface — four channels (declarative policy, admin API,
state-machine hints, placement driver) arbitrated by one gatekeeper — lands in
Phases 6–8, after the mechanics it drives.

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 1, "tasks": [1, 2, 3], "description": "Shard value types, routing table, typed exceptions — no dependencies" },
    { "wave": 2, "tasks": [4, 5], "description": "group_id on the wire and through all five serializers" },
    { "wave": 3, "tasks": [6, 7, 8], "description": "Transport demux, group-scoped storage, tombstones" },
    { "wave": 4, "tasks": [9, 10, 11, 12], "description": "Host skeleton: registry, striped executor, batched tick, hibernation" },
    { "wave": 5, "tasks": [13, 14], "description": "Client routing and static multi-group integration test" },
    { "wave": 6, "tasks": [15, 16], "description": "Admin-entry hook in raft.hpp and the splittable_state_machine extension" },
    { "wave": 7, "tasks": [17, 18, 19], "description": "Split: propose, apply, lazy replica creation" },
    { "wave": 8, "tasks": [20, 21, 22], "description": "Merge: prepare/commit, abandon handshake, alignment" },
    { "wave": 9, "tasks": [23, 24, 25, 26, 35, 36], "description": "Signals: policy concept, default policy, composition, arbiter, metrics, latency digest" },
    { "wave": 10, "tasks": [27, 28, 29], "description": "Placement driver, operators, leader transfer" },
    { "wave": 11, "tasks": [30], "description": "Load-split sampler" },
    { "wave": 12, "tasks": [31, 32, 33, 34], "description": "Property tests, crash consistency, oscillation, scale" }
  ]
}
```

---

## Tasks

---

## Phase 1: Shard Value Types (Tasks 1–3)

- [x] 1. Add `include/raft/shard_types.hpp`
  - Define `shard_key` concept (`std::totally_ordered && std::copyable &&
    std::default_initializable`) and `raft_group_id` concept (`std::regular &&
    std::totally_ordered &&` hashable), the latter defaulting to
    `std::uint64_t` at every use site.
  - Define `shard_range<Key>` with `std::optional<Key> _start` / `_end`
    (`nullopt` = unbounded), plus `contains()`, `is_adjacent_left_of()`,
    `is_empty()`, and a `operator<=>` on the bounds that orders `nullopt` first
    for `_start` and last for `_end`.
  - Define `shard_epoch{_version, _conf_version}` with defaulted `<=>`.
  - Define `shard_descriptor<GroupId, Key, NodeId>` per design §1.3.
  - Define `shard_stats<GroupId, Key>` per design §6.1.1, including the
    `_size_available` flag.
  - Define `split_reason`, `merge_reason`, `split_decision<Key>`,
    `merge_direction`, `merge_decision`, `hot_key_sample<Key>`.
  - Verify: unit test covering `contains()` on all four boundedness
    combinations; `is_adjacent_left_of()` true only for `*_end == *other._start`
    and false when either side is unbounded; epoch ordering.
  - _Requirements: 1.2, 2.1, 2.2, 3.1, 6.5, 6.6, 6.8_

- [x] 2. Add `include/raft/shard_map.hpp`
  - `shard_map<GroupId, Key, NodeId>` over
    `std::map<std::optional<Key>, descriptor, start_bound_less>`.
  - `lookup(key)` via `upper_bound` then step back; `range_scan(range)`;
    `apply_split(parent, children)`; `apply_merge(source, survivor)`;
    `upsert(descriptor)` that ignores a row whose epoch is not newer.
  - `check_tiling() -> std::optional<std::string>` returning a human-readable
    description of the first gap or overlap. Call it under `assert` in debug
    builds at the end of `apply_split`/`apply_merge`.
  - Verify: unit test — single `(-inf,+inf)` shard tiles; split into 3 tiles;
    merge back tiles; a hand-constructed gap and a hand-constructed overlap are
    each detected with the offending bound named; `upsert` of a stale-epoch row
    is a no-op.
  - _Requirements: 2.3, 2.4_

- [x] 3. Add `include/raft/shard_exceptions.hpp`
  - `shard_epoch_mismatch_exception` (carries the current descriptors for the
    targeted range), `shard_epoch_ahead_exception`,
    `shard_not_adjacent_exception`, `shard_busy_exception`,
    `shard_alignment_required_exception`, `split_key_out_of_range_exception`,
    `no_valid_split_key_exception`, `shard_merging_exception`,
    `cross_shard_command_exception`, `unknown_shard_exception`.
  - Derive from the existing `raft_exception` hierarchy in
    `include/raft/exceptions.hpp`; each carries the group id and, where
    applicable, the epoch.
  - Verify: unit test asserting `what()` text names the group and epoch, and
    that `shard_epoch_mismatch_exception` round-trips its descriptor payload.
  - _Requirements: 3.5, 3.6, 12.2, 15.1, 18.6_

---

## Phase 2: Group Id on the Wire (Tasks 4–5)

- [x] 4. Add `_group_id` to the six RPC structs
  - Append (never prepend — every call site uses designated initialisers) a
    `GroupId _group_id{}` field and a `group_id()` accessor to
    `request_vote_request/response`, `request_pre_vote_request/response`,
    `append_entries_request/response`, `install_snapshot_request/response`,
    `fetch_log_entries_request/response` in `include/raft/types.hpp`.
  - Add `GroupId` as a trailing template parameter defaulting to
    `std::uint64_t` on each, and thread it through `default_raft_types`.
  - Extend `entry_type` with `split = 3, merge_prepare = 4, merge_commit = 5,
    merge_rollback = 6, merge_abandoned = 7`.
  - Verify: the full existing test suite compiles and passes unchanged —
    this task's success criterion is *no behavioural diff*.
  - _Requirements: 4.1, 4.2, 5.1 (entry types), 11.1, 13.3_

- [x] 5. Round-trip `group_id` through all five serializers
  - `json_serializer.hpp`, `cbor_serializer.hpp`, `ion_serializer.hpp`: emit
    the key unconditionally; on decode, a missing key yields `GroupId{}`.
  - `proto/raft_messages.proto` and `proto/raft.proto`: add `group_id` as a new
    field number at the end of each message; never renumber.
  - `protobuf_serializer.hpp` and `grpc_message_conversion.hpp`: map the field.
  - `coap_transport_impl.hpp` encodes `candidate_id` directly — audit and
    update its framing too.
  - Verify: for each serializer, a round-trip test with a non-zero group id; and
    a decode test against a payload recorded *before* this change asserting the
    group id comes back as `GroupId{}`. The second half is the backward-compat
    guarantee and must not be skipped.
  - _Requirements: 4.2, 4.3_

---

## Phase 3: Demux, Storage, Tombstones (Tasks 6–8)

- [x] 6. Add `include/raft/group_transport.hpp`
  - `multi_group_network_server<Server, GroupId, …>`: holds the inner server,
    a `synchronized<std::unordered_map<GroupId, group_handlers>>`, and an
    unknown-group callback. `start()` installs exactly one handler per RPC type
    on the inner server that dispatches on `request.group_id()`.
  - `group_scoped_client<Client, GroupId>` satisfying `network_client`: stamps
    `_group_id` on every outbound request; forwards the rest verbatim. Must also
    conditionally satisfy `network_client_with_pre_vote`,
    `network_client_with_log_fetch`, `network_client_with_cluster_join`, and
    `network_client_with_cluster_leave` when the inner client does — use
    `if constexpr`-guarded method definitions so detection propagates.
  - `group_scoped_server<GroupId>` satisfying `network_server`: forwards
    `register_*_handler` into `multi_group_network_server::register_group`;
    `start()`/`stop()` are no-ops; `is_running()` delegates to the shared server.
  - Verify: unit test over `simulator_network` — three groups sharing one
    server; assert each group's handler sees only its own messages, that an
    unknown group id invokes the callback exactly once, and that
    `network_client<group_scoped_client<simulator_client>>` and the four
    optional extension concepts all hold via `static_assert`.
  - _Requirements: 4.4, 4.5, 4.6, 4.7_

- [x] 7. Add `include/raft/group_storage.hpp`
  - `group_scoped_persistence<Engine, GroupId>` satisfying `persistence_engine`,
    prefixing every key/path with the group id.
  - `batched_persistence_engine` optional concept (`begin_batch` /
    `commit_batch` / `abort_batch`), plus an implementation on
    `file_persistence_engine` that buffers appends and issues one directory
    `fsync` per `commit_batch()`.
  - Note in the header that `file_persistence_engine` already takes a
    `data_dir` (file_persistence.hpp:43), so its scoping is
    `data_dir / "groups" / to_string(group)` and it needs no wrapper.
  - Verify: unit test — two groups writing through scoped engines over one
    directory tree do not observe each other's term, `voted_for`, log, or
    snapshot; destroying one group's tree leaves the other intact;
    `begin_batch`/`commit_batch` produce one durability barrier for N appends
    (assert via a counting mock).
  - _Requirements: 1.3, 1.5, 5.3, 5.4_

- [x] 8. Add the durable tombstone set
  - `tombstone_set<GroupId>` persisted alongside the host's own state, with
    `insert(group, reason, when)`, `contains(group)`, and a GC pass on a
    configurable horizon.
  - Wire it into `multi_group_network_server`'s unknown-group path: a
    tombstoned group id drops the message and increments
    `stale_group_message`.
  - Verify: unit test — a message for a tombstoned group is dropped and never
    reaches the unknown-group callback; the tombstone survives a simulated
    restart; GC removes an entry past the horizon and not before.
  - _Requirements: 14.4, 14.5_

---

## Phase 4: Host Skeleton (Tasks 9–12)

- [x] 9. Add `multi_raft<Types, Key, GroupId>` registry and lifecycle
  - `include/raft/multi_raft.hpp` + `multi_raft_impl.hpp` (declaration/definition
    split follows `raft.hpp`'s own pattern).
  - `multi_raft_config<…>` named-parameter aggregate mirroring
    `node_config<Types>`: required transport/store/logger/metrics, optional
    policy, placement driver, partitioner, hibernation and tick knobs. The
    policy slot is **one** concrete type with an in-struct default, exactly as
    `node_config` treats every component — not a list, and not
    `std::optional`. Combining policies is Task 35's composite, per design
    §6.1.3.
  - Registry of `GroupId -> shared_ptr<group_state>`, where `group_state` holds
    the `node`, its scoped store, its operation state, its statistics
    accumulators, and its stripe index.
  - `create_group()`, `destroy_group()`, `start()`, `stop()`. `destroy_group()`
    posts teardown to the host control stripe *after* the group's queue drains,
    reusing `async_scope::close_and_drain()` rather than a second shutdown
    protocol.
  - Verify: unit test — create 5 groups, assert independent terms/leaders;
    `stop()` is synchronous and leaves no joinable threads; a stop/start/stop
    sequence works (the same regression `node::stop()` guards against).
  - _Requirements: 1.1, 1.3, 1.4, 1.7_

- [x] 10. Add the striped serial executor
  - Fixed-size pool with per-group serial queues, `stripe = hash(group) %
    pool_size`. All work for a group — inbound RPC dispatch, tick phases, policy
    evaluation — runs on its stripe.
  - Default pool size `min(hardware_concurrency, 8)`, configurable. Never one
    thread per group.
  - Verify: unit test — 200 groups on a 4-thread pool; assert no group is ever
    entered concurrently (instrument with a per-group re-entrance detector) and
    that thread count is independent of group count.
  - _Requirements: 5.6_

- [x] 11. Add the batched `tick()`
  - Four phases per design §4.2: persist (batched via `if constexpr` when the
    store advertises it), send, apply, policy. Persist strictly before send;
    apply strictly after send.
  - `tick_report{ready_count, hibernating_count, batch_size, phase_durations}`.
  - Replaces the two-timer-threads-per-node pattern in `cmd/chaos_node/main.cpp`
    and `cmd/ca_cluster_node/main.cpp` for multi-group callers; the single-group
    callers keep working as they are.
  - Verify: unit test — with a counting mock store, N ready groups produce one
    `commit_batch()` and one durability barrier per tick; assert the phase
    ordering by recording a call trace.
  - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.7_

- [x] 12. Add hibernation
  - `hibernation_mode{off, on, auto_above_group_count}` defaulting to
    `auto_above_group_count = 64`; `hibernate_after` defaulting to
    10 × `heartbeat_interval`.
  - Eligibility: leader with every follower at `match_index == last_log_index`
    and no pending proposals; follower that heard from a hibernating leader
    within its election timeout. Wake on any client request, inbound RPC,
    configuration change, or placement-driver operator.
  - Verify: unit test — 100 idle groups converge to hibernating; a single
    `submit_command` wakes exactly one group; a leader failure still triggers
    an election within the expected bound (hibernation must not break
    liveness).
  - _Requirements: 5.5, 5.7_

---

## Phase 5: Client Routing (Tasks 13–14)

- [x] 13. Add client routing and the `partitioner` concept
  - `partitioner<P, Key>` concept: `key_of(command) -> Key`.
  - `submit_command(Key, …)`, `submit_command(GroupId, shard_epoch, …)`,
    `read_state(Key, …)` per design §8, with the retry loop: not-leader →
    follow the hint; epoch mismatch → merge the carried descriptors into the
    local map and retry; merging → backoff; bounded by `max_route_retries`
    (default 5).
  - Reject a command whose partitioner key falls outside the resolved shard's
    range with `cross_shard_command_exception`.
  - Verify: unit test — a command routed against a deliberately stale map gets
    exactly one epoch-mismatch rejection then succeeds; a not-leader response
    retries against the hint without a PD call; exceeding
    `max_route_retries` surfaces the last error, not a generic timeout.
  - _Requirements: 2.5, 18.1, 18.2, 18.3, 18.4, 18.5, 18.6_

- [x] 14. Static multi-group integration test
  - Three simulated nodes, four statically configured shards, no split/merge.
    Drive a mixed read/write workload through `submit_command(key, …)`; kill and
    restart nodes; assert every command commits and the tiling invariant holds
    throughout.
  - This is the milestone that proves Phases 1–5 stand on their own: a working
    multi-Raft cluster with a fixed shard map.
  - Verify: the test above, plus `check_tiling()` asserted on every node after
    every operation.
  - _Requirements: 1.1, 1.4, 2.3, 2.6, 4.6, 5.1_

---

## Phase 6: Admin Entries and Splittable State Machines (Tasks 15–16)

- [x] 15. Add the admin-entry hook to `node<Types>`
  - `set_admin_entry_handler(std::function<void(const log_entry_type&,
    log_index_type)>)`; `propose_admin_entry(entry_type, payload, timeout) ->
    future_type`; `match_index_of(node_id_type) const ->
    std::optional<log_index_type>`.
  - In `apply_committed_entries()`, route entries whose type is one of the five
    admin types to the handler and **never** to `state_machine.apply()`,
    mirroring how `no_op` is already excluded.
  - The handler runs on every replica inside the apply loop, not leader-only.
    Document that explicitly in the header — it is the property that makes
    split and merge deterministic.
  - Verify: unit test — propose an admin entry on a 3-node simulated group;
    assert the handler fires exactly once on each replica at the same index,
    that `state_machine.apply()` was not called for it, and that a follower
    that receives the entry via `AppendEntries` fires it too.
  - _Requirements: 13.1, 15.3_

- [x] 16. Add the `splittable_state_machine` extension concept
  - Concept per design §6.4: `approximate_size_bytes`, `approximate_key_count`,
    `suggest_split_keys(max)`, `can_split_at(key)`, `split_state(keys)`,
    `absorb(blob, range)`. Detected structurally with `if constexpr`.
  - Document the two laws in the header: `split_state`/`absorb` must be
    deterministic across replicas, and `absorb` must be the exact inverse of
    `split_state`.
  - Implement it on `test_key_value_state_machine` (ordered by key, so all six
    are straightforward) as the reference implementation and test vehicle.
  - Verify: `static_assert` that `test_key_value_state_machine` satisfies it and
    that `ca_state_machine` does not (and still compiles); a round-trip unit
    test asserting `split_state({k})` then `absorb(right)` reproduces
    `get_state()` byte-for-byte.
  - _Requirements: 11.1, 11.3, 11.6, 11.7_

---

## Phase 7: Split (Tasks 17–19)

- [x] 17. Split proposal path
  - `split_command<GroupId, Key, NodeId>` payload per design §5.2, with a
    serializer.
  - Leader-side sequence per design §5.3: arbiter gate → candidate keys minus
    `can_split_at` vetoes → fallback to `suggest_split_keys` → PD
    `allocate_shard_ids` → derive children (`version = parent.version + N`,
    members one-for-one with the parent's) → mark `splitting` → propose.
  - Abandon (do not queue) when the PD is unavailable; never invent ids locally.
  - Verify: unit test — a vetoed key falls back to the SM's suggestion; all keys
    vetoed yields `no_valid_split_key_exception` and no log entry; a failing
    PD yields no log entry and a `split.rejected{gate=pd_unavailable}` metric;
    the proposed entry's children carry exactly the parent's member set.
  - _Requirements: 11.2, 11.4, 11.5, 13.2, 13.3, 13.10_

- [x] 18. Split apply path
  - Steps A–J of design §5.4 in the admin-entry handler, on every replica.
  - Idempotence check (step B) before anything else, so replay after a crash is
    a no-op.
  - The synthetic snapshot for each non-derived child:
    `{last_included_index = at_index, last_included_term = term_of(at_index),
    configuration = parent config restricted to the child's members,
    state_machine_state = blob}` with an **empty log** — the parent's log is
    never copied.
  - Durability ordering: with `batched_persistence_engine`, children + parent
    apply index in one batch; without it, children **first**, then the parent
    index. Comment the reason inline — the reverse order silently loses a child
    on crash.
  - Staggered child election timers; the child colocated with the parent's
    leader may campaign immediately.
  - Verify: unit test on a 3-node simulated group — split into 3, assert all
    three replicas produce byte-identical child states, tiling holds, the
    derived child kept the parent's group id and term, and each non-derived
    child's log is empty with `last_included_index == split index`. Plus a
    replay test: re-deliver the split entry and assert nothing changes.
  - _Requirements: 13.4, 13.5, 13.6, 13.7, 13.8, 13.9_

- [x] 19. Lazy replica creation
  - `multi_group_network_server`'s unknown-group callback: tombstone check →
    local shard map → rate-limited single PD query → create an *uninitialised*
    replica if and only if this node is a listed member; otherwise drop and
    increment `stale_group_message`.
  - Verify: unit test — a node held offline through a split acquires the child
    on the first inbound message and populates it via `InstallSnapshot`; a
    message naming a node that is not a member never creates a replica; a
    message for a tombstoned group never creates one; the PD is queried at most
    once per rate-limit window regardless of message rate.
  - _Requirements: 14.1, 14.2, 14.3_

---

## Phase 8: Merge (Tasks 20–22)

- [x] 20. `merge_prepare` and `merge_commit`
  - Payloads and serializers for both; `min_index` computed via
    `match_index_of` over the source's voters.
  - Source apply of `merge_prepare`: enter `merging_source`; reject proposals
    and reads with `shard_merging_exception`; refuse configuration changes.
  - Target apply of `merge_commit`, on every target replica: force-append and
    force-apply the carried tail to the **local** source replica until it stands
    at the prepare index → `absorb(source_sm.get_state(), source_range)` →
    extend range → `version = max(src, tgt).version + 1` → destroy and tombstone
    the local source replica → publish the map.
  - Verify: unit test on 3 nodes with two colocated groups — merge succeeds;
    all three target replicas produce byte-identical post-absorb state; the
    source group is tombstoned everywhere; tiling holds; a client of the source
    receives `shard_epoch_mismatch` carrying the survivor's descriptor.
  - _Requirements: 15.3, 15.4, 15.7_

- [x] 21. Abandon handshake and rollback
  - Source leader → target leader `abandon_request`; target refuses if
    `merge_commit` is already proposed, else commits `merge_abandoned` in the
    **target's** log; source observes that committed record, then proposes
    `merge_rollback` and returns to `stable`.
  - `merge_lease_mode` escape hatch, **off by default**, whose header
    documentation opens with its bounded-clock-skew assumption.
  - A stuck merge (target unreachable) leaves the source frozen and emits
    `merge.stalled{group, target}` plus a warning naming the target.
  - Verify: unit test — target epoch changes mid-merge → source rolls back and
    resumes; target leader fails over after committing `merge_abandoned` → the
    new target leader still refuses to commit; a race where `merge_commit` is
    proposed just before the abandon request arrives → commit wins, no
    rollback, no double ownership.
  - _Requirements: 15.5, 15.6, 15.8_

- [x] 22. Merge preconditions and alignment
  - Adjacency, epoch match, colocation, joint-consensus, and operation-state
    checks at proposal **and** re-checked at apply.
  - `shard_alignment_required_exception` when replica sets are not colocated;
    `merge_options::_auto_align` (off by default) asks the PD to align first and
    retries within `_align_timeout`.
  - Verify: unit test for each precondition failing independently with the
    right typed exception; a non-colocated pair fails fast rather than
    attempting a cross-network state transfer; `_auto_align = true` issues the
    expected PD operators and then succeeds.
  - _Requirements: 15.1, 15.2, 12.2_

---

## Phase 9: Signals (Tasks 23–26, 35–36)

- [x] 23. `split_merge_policy` concept
  - `include/raft/split_merge_policy.hpp`: the concept per design §6.1, with
    header documentation stating in the first paragraph that the policy is
    leader-only and **not required to be deterministic**, because its output is
    frozen into the split entry that every replica applies.
  - Statistics collection into `shard_stats`: sizes from the
    `splittable_state_machine` extension (with `_size_available = false` when
    absent), load measured at the routing layer, history from the operation
    state, and the two latency percentiles from Task 36's digest.
  - Log once at construction when a state machine has no sizing hooks, naming
    the consequence: size-based split is unavailable for this state machine.
  - Verify: unit test — `shard_stats` is populated correctly for a sizing-capable
    and a sizing-incapable state machine; the construction-time warning fires
    exactly once.
  - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5, 11.6_

- [x] 24. `threshold_split_merge_policy`
  - Config struct and defaults per design §6.1.2.
  - Split key generation following TiKV RFC 0006's `SizeChecker`: record a key
    every `_shard_split_size_bytes`, stop at
    `_shard_split_size_bytes * (_batch_split_limit - 1) + _shard_max_size_bytes`,
    discard the trailing key when the remainder is not larger than
    `_shard_max_size_bytes - _shard_split_size_bytes`.
  - `validate()` **rejects** `2 * _shard_merge_max_size_bytes >=
    _shard_split_size_bytes` (and the key-count equivalent), with an error
    message that names the oscillation it prevents.
  - Verify: unit test — `_batch_split_limit == 1` reproduces single-key split
    exactly; a shard at 3× the split size yields the expected key count;
    `validate()` rejects `merge_max = 60 MiB` against `split_size = 96 MiB` and
    the message mentions oscillation; the shipped defaults validate.
  - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5, 7.7, 6.6, 6.7_

- [x] 25. The arbiter
  - Per-shard operation state machine (`stable | splitting | merging_source |
    merging_target | frozen | tombstoned`) as the *only* way an operation
    starts, per design §6.6.
  - Precedence: admin ▸ placement driver ▸ policy; state-machine hints never
    initiate. An accepted admin command suspends the automatic channels for the
    affected shards until it resolves.
  - Gates: state, `split_merge_interval` (enforced by the host, not the policy),
    `batch_split_limit` (likewise host-enforced, truncating and logging rather
    than refusing — a composition can exceed every member's own limit),
    `max_concurrent_split_merge` (default 4, enforced before proposing, never by
    aborting), global kill switch, epoch, veto exhaustion, alignment.
  - Admin API: `split_shard`, `merge_shards`, `pre_split`, `freeze_shard`,
    `thaw_shard`, `set_automatic_split_merge_enabled`, with `split_options` /
    `merge_options` per design §6.2. `_wait_for_apply` defaults true;
    `freeze_shard` blocks automatic channels but not admin commands.
  - Verify: unit test — concurrent proposals from two channels resolve by
    precedence with the loser logged `preempted_by`; a merge within
    `split_merge_interval` of a split is refused even with a custom policy that
    ignores the interval; the 5th concurrent operation is refused before
    proposing; `pre_split` on a non-empty shard is refused; a frozen shard
    refuses policy but accepts `split_shard`.
  - _Requirements: 6.9, 7.6, 12.1, 12.2, 12.3, 12.4, 12.5, 12.6, 17.1–17.7_

- [ ] 26. Signal metrics and structured logs
  - The metric set from design §6.7, with `reason`, `gate` and `policy` as
    dimensions; `merge.vetoed{group, policy}` and
    `split.truncated{group, proposed_keys, kept_keys}` included, since under a
    composition the first question is always *which member*.
  - A structured log entry for every split and merge — proposed, applied,
    rejected, rolled back — carrying group id, epoch before and after, the
    deciding channel, and the reason.
  - `set_report_listener()` per MicroRaft's `RaftNodeReportListener`: per-group
    role, term, commit index, and membership changes.
  - Verify: unit test asserting a rejected split emits
    `split.rejected{gate=cooldown}` with the group dimension set, and that the
    listener fires on role and membership transitions.
  - _Requirements: 6.9, 17.7, 19.1, 19.2, 19.3, 19.4_

- [ ] 35. `composite_split_merge_policy`
  - `include/raft/split_merge_policy.hpp`: a variadic composite holding
    `std::tuple<Ps...>` that itself satisfies `split_merge_policy`, per design
    §6.1.3. The config slot stays singular (Task 9); this is how several
    policies run at once.
  - `merge_decision` becomes tri-state (`merge_verdict{propose, abstain,
    veto}`, default `abstain`) — the one type change composition forces, and
    the reason it is forced: under unanimity a two-state answer would make a
    load-only policy veto every merge in the cluster while looking correct in
    isolation.
  - Combination: split any-wins with a sorted de-duplicated union of `at_keys`;
    merge unanimous (≥1 `propose`, 0 `veto`); a member deferring with empty
    `at_keys` yields to any member naming concrete keys; opposite merge
    directions are a mutual veto, logged. Every rule commutative and
    associative — member order must not be observable.
  - `validate()` checks the §6.1.2 oscillation bound *across* members via
    optional `split_floor()` / `merge_ceiling()` detected with `if constexpr`;
    a member exposing neither is reported *uncheckable*, not assumed safe.
    `get_validation_errors()` names the member each error came from.
  - `cooldown()` is the max over members. An empty composition is legal, equals
    "no policy", and logs once at construction.
  - Verify: unit test — a composition of the threshold default with a
    load-only stub proposes splits from either member and unions their keys;
    the load-only stub abstaining does not block a merge the threshold policy
    proposes, while the same stub returning `veto` does; two members proposing
    opposite directions produce no merge and a log line; shuffling the member
    order changes no decision across a randomised sweep; `validate()` rejects a
    pair whose thresholds interleave and names both members; a member without
    the floor/ceiling accessors yields an *uncheckable* error string rather
    than silence; an empty composition proposes nothing and logs once.
  - _Requirements: 8.1–8.10, 6.8, 6.10, 6.11, 7.8_

- [ ] 36. `latency_digest` and the two latency percentiles
  - `include/raft/latency_digest.hpp`: fixed-bucket logarithmic histogram —
    bounded memory, no allocation on the record path, O(1) record, mergeable,
    bucket walk to read — behind a ring of sub-windows spanning a configured
    multiple of `policy_interval`, rotated on the policy tick. Percentiles must
    be computed in-process: the `metrics` concept ships samples to a back-end
    that a policy on the group's stripe cannot read back.
  - Sample read latency in `multi_raft::read_state`, into the same per-group
    accumulators that produce `_read_qps` / `_read_bytes_per_sec`, so the
    sample covers shard-map lookup and epoch validation. Not in
    `node<Types>::read_state` — it sees neither, and the node stays closed.
  - Sample apply latency where the tick's apply phase drives each group.
    Nothing measures it today; `_p99_apply_latency` has been an unpopulated
    field since the first draft, and it shares this digest so the two fields
    cannot drift apart in meaning.
  - Count a completed read and a timed-out read (clamped at its deadline);
    do not count `not_leader` or `shard_epoch_mismatch`, which are routing
    misses rather than load.
  - Fix the millisecond truncation in `node::read_state`'s existing
    `raft_read_latency` emission (raft.hpp:1745-1770, 1944-1966), which records
    every sub-millisecond read as zero. Additive and confined to the metric
    call — see the Notes below on the layering rule this brushes against.
  - `split_reason::latency` is added for policies that act on backpressure; the
    default policy never produces it (design §6.1.4 explains why read latency
    is a size proxy under whole-blob reads and so is not a default trigger).
  - Verify: unit test — a known sample set yields a p99 within the digest's
    documented bucket error; samples older than the window are dropped, so a
    burst then silence decays to the idle value within one window; a
    sub-millisecond read produces a non-zero sample; a timed-out read is
    counted and an epoch-mismatch rejection is not; the digest allocates
    nothing on the record path (assert via an instrumented allocator).
  - _Requirements: 10.1–10.10, 6.5_

---

## Phase 10: Placement Driver (Tasks 27–29)

- [ ] 27. `shard_placement_driver` concept and `no_op` default
  - Concept per design §7; `shard_report` and `node_report` mirroring TiKV's
    region and store heartbeats, reusing the existing `placement_group_id` from
    `quorum_management.hpp` for labels.
  - `no_op_shard_placement_driver` in the shape of `no_op_quorum_manager`:
    allocates ids from a locally configured reserved range, returns no
    operators, so a static pre-split deployment needs no control plane.
  - Verify: `static_assert` the no-op satisfies the concept; a static
    three-shard cluster runs end-to-end against it with splits disabled.
  - _Requirements: 16.1, 16.2, 16.3, 16.7_

- [ ] 28. Heartbeats and advisory operators
  - Batched shard heartbeats — one call per interval carrying every local
    shard's report, not one call per shard.
  - `shard_operation` variant (`add_replica`, `remove_replica`,
    `transfer_leader`, `split`, `merge`, `scatter`) delivered in the heartbeat
    response, each carrying `operation_id` and the epoch it was computed
    against.
  - Operators are **advisory**: a leader whose preconditions no longer hold
    drops the operator, logs `skipped_operator`, and the PD reissues next
    heartbeat. Stale-epoch operators are discarded on receipt.
  - Verify: unit test — 100 shards produce 1 heartbeat call per interval; a
    stale-epoch operator is discarded without side effects; an operator arriving
    for a shard that is mid-merge is skipped and counted, and the same operator
    is accepted once the shard returns to `stable`.
  - _Requirements: 16.4, 16.5, 16.6, 16.8_

- [ ] 29. Leader transfer and scatter
  - Raft leadership transfer (TimeoutNow) on `node<Types>`, needed by the PD's
    `transfer_leader` operator and by load-split scatter.
  - `scatter` operator implementation: request leader transfers so that the
    children of a split do not all lead from the same machine.
  - See design §12 open question 1 — this is arguably its own spec; if it grows
    past this task, split it out rather than letting it swallow Phase 10.
  - Verify: unit test — leadership transfers to a named target within one
    election timeout without an intervening term bump on a third node; after a
    load split with `_scatter_children`, the two children's leaders are on
    different nodes.
  - _Requirements: 16.4, 9.6_

---

## Phase 11: Load-Based Split (Task 30)

- [ ] 30. Load-split sampler
  - `include/raft/load_split_sampler.hpp` implementing TiKV RFC 0045 per design
    §6.3: threshold entry, up to `_load_split_sample_keys` candidates by
    probability sampling, `_load_split_duration` of left/right access counting,
    abandon on load drop, single-hot-key detection with
    `_load_split_backoff`, most-balanced candidate emitted as
    `shard_stats::_hot_key_samples`.
  - Off by default; one predictable branch per request when off.
  - A load split always sets `split_options::_scatter_children`.
  - Verify: unit test — a 5-second spike produces no proposal; a sustained
    balanced-access load produces a split near the median key; a load
    concentrated on one key produces no proposal and marks the shard ineligible
    for `_load_split_backoff`; the disabled path costs one branch (assert via
    a call-count instrument, not a timing measurement).
  - _Requirements: 9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 9.7_

---

## Phase 12: Verification (Tasks 31–34)

- [ ] 31. Invariant property tests
  - Randomised workload of splits, merges, membership changes, and client
    commands over the simulator network, asserting after every operation:
    I1 tiling (`check_tiling()` on every node), I2 no loss/duplication against
    a shadow `key → value` model reconciled with the union of every shard's
    `get_state()`, I3 epoch monotonicity, I4 stale-epoch requests always
    rejected, I5 the `split_state`/`absorb` round-trip law.
  - Verify: the property tests above, seeded reproducibly.
  - _Requirements: 20.1, 20.2, 20.3, 5.6 (round-trip law from 9.7)_

- [ ] 32. Crash-consistency tests
  - New `fiu_do_on` fault points: `raft/multiraft/split/before_children`,
    `.../between_children`, `.../after_children_before_parent`,
    `.../after_publish`, `raft/multiraft/merge/after_prepare`,
    `.../mid_commit_catchup`, `.../after_absorb_before_destroy`,
    `.../after_abandon_before_rollback`.
  - Crash-and-recover at each point, then assert I1–I4.
  - Verify: the eight scenarios above, each asserting recovery to a consistent
    state and no lost child / no double ownership.
  - _Requirements: 20.4, 13.6_

- [ ] 33. Oscillation test
  - Drive `threshold_split_merge_policy` for 10 000 simulated ticks with shard
    sizes parked between the merge and split thresholds; assert total split and
    merge counts stay under a small bound.
  - Repeat with `validate()`-rejected knobs forced past the check and assert the
    host-level `split_merge_interval` gate still bounds the count — the
    defence-in-depth claim in design §6.1.2 must be demonstrated, not asserted.
  - Repeat once more against a `composite_split_merge_policy` (Task 35) whose
    members each validate alone but whose thresholds interleave: assert
    `validate()` rejects the composition, and that forcing past it still leaves
    the count bounded by the host interval gate. Cross-member oscillation is
    the failure the single-policy check cannot see.
  - Verify: all three runs above.
  - _Requirements: 20.5, 20.8, 7.5, 7.6, 8.6_

- [ ] 34. Scale test
  - 1000 groups across three simulated nodes; assert `tick()` duration tracks
    *ready* group count rather than total, and that hibernating count converges
    to near 1000 under an idle workload.
  - Any container-based scenario uses `container_runtime()` / `compose_prefix()`
    from `tests/docker_chaos/os_faults.hpp`, no static IPs, and does not pipe
    multi-process output through `tail`/`head`.
  - Verify: the scale test above plus a Podman run of any container scenario it
    adds.
  - _Requirements: 20.6, 20.7, 5.5_

---

## Notes

**The one change to the consensus core.** Only Task 15 touches
`include/raft/raft.hpp` structurally, and only additively: two public methods,
one accessor, and one branch in `apply_committed_entries()` that routes admin
entry types to a handler instead of to the state machine. Task 36 also edits
the file, but only to stop an existing metric emission from truncating its
duration to milliseconds — no new state, no new call, and the read path itself
is untouched. Everything else about split, merge, routing, and policy lives in
new headers. If a reviewer finds Phase 7 or 8
reaching back into `raft.hpp`, the layering has slipped and the design should be
revisited rather than the boundary widened.

**Why `group_id` is a defaulted field rather than a wire-format version.**
Task 4 appends the field with a default so that every existing serializer test,
every recorded payload, and every designated-initialiser call site keeps
working. A format version would have forced a coordinated upgrade of all five
serializers and both `.proto` files at once. Task 5's backward-compat decode
test is the guarantee that this holds.

**Where the risk concentrates.** Three places, in order:
1. Task 18's durability ordering. Writing the parent's apply index before the
   children's state loses a child permanently on a crash, silently, until a
   client asks for a key in that range. Task 32's
   `after_children_before_parent` fault point exists specifically for this.
2. Task 21's abandon handshake. Any weakening toward a timing argument
   reintroduces the possibility of a source resuming service while a target
   replica has already applied the commit — two shards owning one range.
3. Task 16's round-trip law. A state machine whose `absorb` is not the exact
   inverse of `split_state` diverges replicas in a way no Raft-level invariant
   catches. Task 31's I5 is the only thing standing between that bug and
   production.

**Deliberately deferred.** Cross-shard transactions (out of scope entirely);
TiKV RFC 0082's buckets as a sharper load signal (`shard_stats` is shaped to
accept them later without breaking the policy concept); transport-level message
batching across groups to the same destination (design §11 Phase 9).
