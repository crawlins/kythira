# Design Document

## Overview

Transactions are added as a **state-machine wrapper plus a client-side
coordinator**, not as a change to consensus. `node<Types>` never learns that a
transaction exists. Every transaction step is an ordinary `normal` log entry
that the wrapper decodes. The multi-raft host gains three small things: it
envelopes plain commands for a wrapped state machine, it routes multi-key
transaction RPCs by key, and (tier 2) it hosts the timestamp oracle's group.

```
application / redis gateway
  └─ transaction_coordinator<Client, Key>          include/raft/txn_coordinator.hpp (new)
       ├─ begin(txn_options) ─────────────► timestamp_oracle::allocate(1)   (tier 2)
       ├─ put / get (buffer; tier 2-3 read at start_ts)
       └─ commit()
            ├─ 1 shard   → txn_one_phase                                  (Req 3.2)
            └─ N shards  → txn_prepare × N (primary first in the batch)    (Req 3.3)
                         → txn_commit_primary                 ← commit point
                         → txn_commit_secondary × (N-1), async
       Client = txn_participant_client: send(key, envelope) → future<bytes>
                (multi_raft::submit_txn locally; gateway forward link remotely)

multi_raft<Types, Key, GroupId>                     include/raft/multi_raft*.hpp
  ├─ submit_command(key, cmd)      → envelope(plain, cmd) if SM is transactional (Req 2.1)
  ├─ submit_txn(keys, envelope)    → every key in range, else epoch-mismatch   (Req 3.5)
  └─ reserved oracle group         → never routed, split, merged or placed     (Req 7.2)

transactional_state_machine<SM, Key, P, Mvcc>       include/raft/txn_state_machine.hpp (new)
  ├─ _inner        : SM                    (committed data; tier 2: versioned)
  ├─ _intents      : map<Key, intent>      (lock table)
  ├─ _records      : map<Key primary, map<txn_id, txn_record>>
  ├─ _oracle_hwm   : uint64                (used only in the oracle group)
  ├─ apply(envelope, index)  → decode → one of the operations below
  ├─ get_state / restore     → versioned format "kythira-txn-sm v1"
  └─ split_state / absorb    → partition all three maps by key            (Req 6)
```

### Key design decision: a wrapper, not a new entry type

The multi-raft admin entries (`split`, `merge_*`) are `entry_type` values with
a host-side handler. Transactions could follow that pattern with three or four
more values, but that would touch `node<Types>`'s admin path, every
serializer's `entry_type` range check (PR #500 added those deliberately), and
the snapshot of every deployment, transactional or not. Locks and records
also have to be *in the state machine's snapshot*. A follower that catches up
by `InstallSnapshot` must receive the lock table, or it would serve a key as
unlocked that its leader holds locked. Host-side state outside the state
machine is not in the snapshot.

A wrapper gets both for free. Its state is the inner state machine's state
plus the lock table and records, so snapshots, catch-up and split/absorb carry
locks with no new mechanism. Its operations are `normal` entries, so the log,
the serializers and `node<Types>` do not change (Requirement 1.5). An
unwrapped host is untouched by construction (Requirement 1.2).

### Key design decision: Percolator's primary-key commit point

The coordinator could be made durable instead: its own Raft group, or a
record in a designated coordinator shard. Both add a service that every
transaction depends on. Percolator's alternative makes the commit point a
single entry on the shard that owns one of the transaction's own keys, the
primary. Every intent names its primary. Anyone who finds an intent can learn
its fate from the primary's record and finish the job (Requirement 4). The
coordinator is then pure library code: it can die at any step and lose
nothing, because nothing it alone knows is needed to finish.

The cost is that a transaction's fate is unknown while its primary shard is
unavailable. That is the same availability any single-key write to that shard
already has.

### Key design decision: no-wait locking

A prepare that meets another transaction's intent fails immediately with
`write_conflict` rather than queueing. Waiting would need a wait-for graph and
deadlock detection across shards. Under the Redis workload the conflict rate
is expected to be low, and the caller retries (`txn_options::max_retries`).
Reads may wait (`read_committed_wait`), because a waiting reader holds nothing
and cannot be part of a cycle.

### Key design decision: the oracle is a group in the host, using the same wrapper

Tier 2 needs timestamps every shard can compare. A shard's log index cannot
serve: each group's log is independent, and splits and merges create groups
whose logs start fresh. The oracle is therefore a counter replicated by a
dedicated Raft group, the shape of TiKV's PD timestamp oracle, with no
dependence on wall clocks.

`multi_raft` instantiates exactly one state-machine type for all its groups
(`Types::state_machine_type`). A second type for one group would need a
variant or a second host. The oracle group instead uses the same
`transactional_state_machine`. Its `_oracle_hwm` field and `oracle_reserve`
operation are meaningful only there. The group has a reserved id, an empty
key range, and is excluded from routing, the split/merge policy and placement
operators. The cost is one unused `uint64` in every shard's snapshot.

A deployment that already runs a control plane can supply its own oracle
through `multi_raft_config::allocate_timestamps`, the same way
`allocate_shard_ids` is supplied today (Requirement 7.6).

### Key design decision: all writes carry timestamps under `mvcc::on`

Under snapshot isolation a plain single-key write that bypassed the oracle
would have no commit timestamp, and a snapshot read could not place it before
or after its read point. Under `mvcc::on` a plain write therefore becomes a
1PC transaction with an oracle timestamp. That costs one oracle round trip per
write, amortised by batching. That cost is why `mvcc` is a compile-time
choice and not a per-transaction one.

## Components and Interfaces

### Envelope (`include/raft/txn_envelope.hpp`, new)

A versioned binary format, first byte `0x4B` ("K") and a format version.

| Op | Fields | Applied by |
|---|---|---|
| `plain` | command | any shard |
| `one_phase` | txn_id, writes `[(key, command)]`, (tier 2: start_ts, commit_ts) | the one owning shard |
| `prepare` | txn_id, primary_key, writes, read_keys (tier 3), start_ts, ttl_deadline_ms, `is_primary` | each participant |
| `commit_primary` | txn_id, primary_key, commit_ts | primary's shard |
| `commit_secondary` | txn_id, keys, commit_ts | each secondary's shard |
| `abort` | txn_id, primary_key, keys | any participant |
| `heartbeat` | txn_id, primary_key, new_deadline_ms | primary's shard |
| `check_and_resolve` | txn_id, primary_key, now_ms | primary's shard |
| `cleanup` | `[(primary_key, txn_id)]` | primary's shard (leader-proposed) |
| `gc` | safe_point | any shard (leader-proposed, tier 2) |
| `oracle_reserve` | new_hwm | oracle group |

`txn_id` is 128 bits drawn from `RAND_bytes` at `begin()`. In tier 2 it is
also tagged with the start timestamp. Randomness at the coordinator is safe
because the value is frozen into the entry before any replica applies it.
`now_ms` and `ttl_deadline_ms` are stamped by the proposing leader, so
`apply()` stays a pure function of the log.

Every result is a one-byte status plus fields: `ok`, `write_conflict{holder,
primary}`, `key_locked{holder, primary}`, `record{status, commit_ts}`,
`read_conflict`, `snapshot_too_old`, `already_aborted`.

### State machine wrapper (`include/raft/txn_state_machine.hpp`, new)

```cpp
enum class mvcc : bool { off = false, on = true };

template<typename SM, shard_key Key, partitioner<Key> P, mvcc Mvcc = mvcc::off>
requires state_machine<SM> && (Mvcc == mvcc::off || multi_version_state_machine<SM, Key>)
class transactional_state_machine {
public:
    auto apply(const std::vector<std::byte>& envelope, log_index_type index)
        -> std::vector<std::byte>;
    auto get_state() const -> std::vector<std::byte>;
    auto restore_from_snapshot(const std::vector<std::byte>&, log_index_type) -> void;

    // present iff SM is splittable (requires-clause on each member)
    auto split_state(const std::vector<Key>&) -> std::vector<std::vector<std::byte>>;
    auto absorb(const std::vector<std::byte>&, const shard_range<Key>&) -> void;
    // approximate_size_bytes etc. forward to SM, plus the lock table's size

    auto inner() -> SM&;
    auto inner() const -> const SM&;
    auto intent_on(const Key&) const -> std::optional<intent_view>;
    auto intents_older_than(std::uint64_t now_ms) const -> std::vector<intent_view>;
};
```

The wrapper needs one more thing from the application: the key of each
buffered command. It takes the multi-raft `partitioner` as the template
parameter `P` (`P::key_of(command)`), which every multi-raft host already
has.

**Apply rules** (all deterministic, all idempotent by `txn_id`):

- `plain`: if `_intents` holds the command's key, return `key_locked`.
  Otherwise forward to `_inner.apply` (`apply_at` with the entry's timestamp
  under `mvcc::on`).
- `one_phase`: if any key holds an intent, or (tier 2) has a version newer
  than start_ts, return the conflict. Otherwise apply every write in key
  order.
- `prepare`: on a conflict return it, and install nothing. If `is_primary`
  and `_records` holds `aborted` for this txn, return `already_aborted`.
  Otherwise install every intent (and read intent, tier 3) and, if
  `is_primary`, the `pending` record with its deadline. A repeated prepare
  for intents already held by the same txn returns `ok`.
- `commit_primary`: `pending` becomes `committed` (with commit_ts), and this
  txn's intents in the primary's shard are applied and removed. `committed` returns `ok`.
  `aborted` returns `record{aborted}`; the coordinator then reports
  `aborted`, never `committed`.
- `commit_secondary`: for each key whose intent belongs to txn, apply its
  write and remove it. Keys whose intent is gone are skipped (already
  resolved).
- `abort`: remove this txn's intents. On the primary, `pending` becomes
  `aborted`, and an absent record becomes an `aborted` marker.
- `check_and_resolve`: Requirement 4.3's table.
- `cleanup`, `gc`, `oracle_reserve`: as named.

**Snapshot format:** a header `kythira-txn-sm`, a version, the three maps in
key order, then the inner state as an opaque blob. `restore_from_snapshot`
refuses a blob without the header (Requirement 1.4).

**Split/absorb:** `split_state(keys)` cuts each map at the same keys as
`_inner.split_state(keys)` and returns, per child, the header, the three
partitioned maps and the inner child blob. `absorb` reverses it. Records go
by primary key, intents by their own key, and `_oracle_hwm` stays with the
left child (it is zero outside the oracle group). The round-trip property is
tested over random mixes (Requirement 6.2).

### Host changes (`include/raft/multi_raft.hpp`, `multi_raft_impl.hpp`)

- `if constexpr (is_transactional_state_machine_v<SM>)` in `submit_command`
  envelopes the command as `plain`. Unwrapped hosts compile the old path
  unchanged.
- `submit_txn(const std::vector<Key>& keys, std::vector<std::byte> envelope,
  timeout)`: the multi-key analogue of `route_and_run`. It resolves the first
  key, checks *every* key against the local descriptor's range, and refuses
  with `shard_epoch_mismatch_exception` carrying the current descriptors when
  any key falls outside, so the coordinator re-partitions (Requirement 3.5).
  The frozen-shard backoff and the not-leader hint are reused.
- `multi_raft_config` gains `txn_defaults`, `oracle_group` (optional reserved
  id), `allocate_timestamps` (hook) and `resolver_interval`.
- The tick's apply phase gains the leader-side resolver
  (`intents_older_than`), cleanup and GC proposals, rate-limited per group.
- The split/merge policy and operator paths skip `oracle_group`. Its
  descriptor has an empty range, so routing never resolves to it.

### Coordinator (`include/raft/txn_coordinator.hpp`, new)

```cpp
template<typename C, typename Key>
concept txn_participant_client = requires(C& c, const Key& k,
                                          std::vector<Key> keys,
                                          std::vector<std::byte> env,
                                          std::chrono::milliseconds t) {
    { c.send(std::move(keys), std::move(env), t) } -> /* future<std::vector<std::byte>> */;
    { c.resolve(k) } -> /* std::optional<shard descriptor> */;
};

template<txn_participant_client<Key> Client, shard_key Key, partitioner<Key> P>
class transaction_coordinator { /* begin, put, get, commit, rollback */ };
```

`commit()` groups the buffered writes by the descriptor each key resolves to.
It picks the primary as the smallest key in the largest group, which makes the
primary's prepare carry the most work and leaves fewer secondaries. It then
sends all prepares concurrently. Sending the primary's prepare concurrently
rather than first is safe because of the rollback marker (Requirement 4.3-4.4).
On any conflict it aborts. On all-`ok` it sends `commit_primary`, resolves
`commit()` from that result, and fires the secondaries without waiting. A
`shard_epoch_mismatch` on any send re-partitions just that group's keys and
retries up to the timeout.

For `read_committed_wait` and snapshot reads, `get(key)` uses
`txn_participant_client::send` with a read envelope. That envelope is not
logged; the host serves it through `with_state_machine` on the leader, or
through the linearizable path for snapshot reads. If an intent is found, the
coordinator backs off, and after the TTL sends `check_and_resolve`.

### Timestamp oracle (`include/raft/raft_timestamp_oracle.hpp`, new)

Leader-side state is `[next, window_end)`. `allocate(n)`:

1. Join the current batch. One batch is in flight at a time, and requests
   that arrive meanwhile wait for the next one.
2. Confirm leadership with a heartbeat round (the existing read-index
   machinery). If it fails, fail the batch with not-leader.
3. If `next + total > window_end`, propose `oracle_reserve(window_end +
   max(window, total))` and wait for it to apply.
4. Hand out consecutive ranges.

A new leader starts with `next = window_end = 0` and so must reserve before
serving. Its first reserve is computed from the applied `_oracle_hwm`, so it
starts above anything any previous leader could have served (Requirement
7.4). The heartbeat confirmation in step 2 stops a deposed leader from serving
from a stale window after a new leader has served larger values
(Requirement 7.5). A leader lease could later replace the per-batch heartbeat;
that is an optimisation, not part of this spec.

### Multi-version Redis KV (`include/raft/redis_kv_mvcc_state_machine.hpp`, new)

Per key, a vector of `(commit_ts, shared_ptr<const value_entry>)` sorted
descending, where a deletion is a tombstone version. `read_at` is a binary
search. `collect_garbage(sp)` keeps the newest version at or below `sp` and
everything above it. Expiry sweeps become versioned deletions at the sweep's
commit timestamp. `split_state` and `absorb` follow the existing
implementation's key order, so the laws carry over. The existing unversioned
state machine is untouched; the gateway picks one at compile time.

### Redis gateway (`include/raft/redis_gateway*.hpp`, `cmd/redis_gateway_node`)

- `--transactions on|off` (default off; refused when the binary lacks the
  adapter) and `--txn-isolation <level>`.
- Commands: `MSET`, multi-key `DEL`, `MGET` (a read-only transaction, so
  snapshot under `snapshot`), and `MULTI`/`EXEC`/`DISCARD`. A queued `GET`
  inside `MULTI` reads at `EXEC` time under the transaction's isolation.
  `WATCH` under `mvcc::on` records the oracle's current value per key, and
  `EXEC` adds those keys to the read set with that timestamp.
- `KYTHIRA.ISOLATION <level>` per connection.
- Internal `KYTHIRA.TXN <op> <envelope>` commands over the forward link. The
  gateway accepts them only on a connection that authenticated with the
  internal secret.
- `txn_participant_client` for the gateway calls `multi_raft::submit_txn` when
  the local replica leads the shard, and otherwise forwards `KYTHIRA.TXN` to
  the leader's gateway.

## Error Handling

| Condition | Surfaced as | Caller action |
|---|---|---|
| Intent held by another txn at prepare | `write_conflict` → `aborted(write_conflict)` | retry the txn |
| Plain write on a locked key | `key_locked_exception` | retry after backoff (gateway: resolves, then retries once) |
| Primary commit unobserved | `unknown` | `transaction_status()`; never assume either way |
| Primary record aborted by a resolver before commit | `aborted(expired)` | retry; raise `lock_ttl` if frequent |
| Key moved between partition and send | epoch mismatch, handled inside `commit()` | none |
| Snapshot older than the safe point | `snapshot_too_old` | retry with a fresh start timestamp |
| Isolation not built | `isolation_unsupported_exception` at `begin()` | rebuild with `mvcc::on`, or pick a lower level |
| Oracle unavailable | `begin()` fails with the oracle's error | retry; tier-1 isolation keeps working |

## Testing Strategy

- **Unit (`tests/txn_state_machine_test.cpp`)**: every op's apply, idempotent
  replays, out-of-order commit/abort, the rollback marker, the TTL table, and
  snapshot round trips and format refusal.
- **Property (`tests/txn_split_absorb_property_test.cpp`)**: random data,
  intents and records; split at random keys; absorb; byte equality.
- **In-process cluster (`tests/multi_raft_txn_test.cpp`)**: three hosts in
  one process (the existing multi-raft test harness), a coordinator with a
  crash hook after each step, and a resolver tick. After the resolver has run,
  every transaction must be all-applied or all-absent.
- **Bank workload (`tests/multi_raft_txn_bank_test.cpp`)**: N accounts over
  M shards, concurrent transfers, leader transfers, and splits and merges
  driven mid-run. Requirement 13.4's invariants, checked per level.
- **Oracle (`tests/raft_timestamp_oracle_test.cpp`)**: monotonic across
  leader changes and restarts, no value served twice, and a deposed leader
  refused.
- **Chaos (`tests/docker_chaos/txn_bank_chaos_test.cpp`,
  `docker/txn-chaos-compose.yml`)**: the bank workload through
  `redis_gateway_node`. Peers by service name, `container_runtime()` and
  `compose_prefix()` only, and no privileged features (`CLAUDE.md`).
- **Performance**: new rows in `multi-raft-performance` (1PC, 2PC across 2
  and 4 shards, and snapshot reads at 1/8/64 concurrency).

## Open Questions

1. **Default isolation for the gateway.** `read_committed` is proposed: it
   needs no oracle and matches what a Redis user expects of `MSET`. Should
   `--transactions on` instead default to `snapshot` when built with
   `mvcc::on`?
2. **Record retention.** Ten minutes is a guess. Long retention makes
   `transaction_status()` useful after an `unknown` result, while short
   retention keeps snapshots small.
3. **Oracle placement.** In-host reserved group (proposed) versus requiring
   an external control plane. The in-host group keeps a single-binary
   deployment working; the hook keeps the other option open.
4. **Tier 3 scope.** Read intents block writers for the transaction's
   lifetime. If write skew protection is wanted only for specific keys, a
   cheaper `SELECT ... FOR UPDATE`-style explicit lock on read could replace
   full read-set validation.
