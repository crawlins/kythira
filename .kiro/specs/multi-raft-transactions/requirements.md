# Requirements Document

## Introduction

`multi_raft<Types, Key, GroupId>` routes every command to the one shard that
owns its key and refuses a command whose keys span shards
(`cross_shard_command_exception`, multi-raft Requirement 18.6). The multi-raft
spec put distributed transactions out of scope, and `doc/TODO.md` carried them
as a deliberately deferred feature. The only in-tree application, the Redis
gateway, is a build cache whose commands name exactly one key
(redis-compatible-kv, "Single-key operations only"), so nothing has needed
them yet.

This spec adds atomic multi-key writes across shards, with the isolation level
chosen by the application. It is built in tiers, each useful on its own and
each mergeable without the next:

| Tier | Delivers | Needs |
|---|---|---|
| 1 | Atomic multi-key writes (all or nothing across shards); reads at **read committed**, optionally waiting on in-flight locks | a lock table and a transaction record per shard; no clocks, no new service |
| 2 | **Snapshot isolation**: every read in a transaction sees one point in time across shards | a timestamp oracle (a counter replicated by its own Raft group) and a multi-version state machine |
| 3 | **Serializable**: snapshot isolation plus protection against write skew | read-set validation at commit |

The level is chosen in two places. The *build* decides which levels can be
offered (tier 2 and 3 need the multi-version state machine, which costs memory
and garbage collection, so it is opt-in at compile time). Each *transaction*
picks one of the levels its build offers at run time. A request for a level
the build cannot provide fails; it is never quietly downgraded.

Tier 1 follows Percolator's protocol shape (Peng and Dabek, OSDI 2010) as TiKV
uses it: the transaction's commit point is a single Raft entry on the shard
that owns its *primary* key, so the coordinator holds no durable state and a
crashed coordinator is recovered by whoever next meets one of its locks.

### Non-goals

- **Changing any existing deployment.** A host whose state machine is not
  wrapped in the transactional adapter behaves byte for byte and lock for lock
  as it does today, including Requirement 18.6's rejection of cross-shard
  commands. The build cache gateway does not turn transactions on.
- **Range reads and predicate locks.** The Redis KV state machine has no range
  scan, so tier 3 protects point reads only. Phantom protection is left to a
  later spec that adds scans.
- **Interactive transactions over the wire for arbitrary applications.** The
  coordinator is a library. The Redis gateway is the one wire surface this
  spec adds (`MSET`, multi-key `DEL`, `MULTI`/`EXEC`, and `WATCH` in tier 2).
- **External consistency across clusters**, clock-bound commit waits
  (Spanner's TrueTime), and hybrid logical clocks. The timestamp oracle is
  purely logical.
- **Deadlock detection.** Locking is no-wait: a conflicting lock aborts the
  newer transaction, which the caller retries. There is never a wait-for
  cycle to detect.

## Glossary

- **Transaction (txn)**: a set of writes, and in tiers 2-3 reads, that commit
  or abort as one.
- **Coordinator**: the library object driving one transaction. It holds no
  durable state.
- **Participant shard**: any shard owning at least one key the transaction
  writes.
- **Primary key**: one key the coordinator picks from the write set. Its
  shard holds the transaction record, and committing that record is the
  transaction's commit point.
- **Secondary key**: every other written key.
- **Intent (lock)**: a prepared but uncommitted write held in a shard's lock
  table, naming its transaction and primary key. Invisible to readers.
- **Transaction record**: the primary shard's durable status of a
  transaction: `pending`, `committed` (with a commit timestamp in tier 2) or
  `aborted`.
- **Resolve**: finishing a transaction found by its lock. Look up the primary's
  record; roll the lock forward if committed, remove it if aborted, and abort
  the record first if it is pending and its TTL has expired.
- **One-phase commit (1PC)**: a transaction whose keys all live in one shard,
  committed with one Raft entry and no lock table round trip.
- **Timestamp oracle (TSO)**: the service that hands out strictly increasing
  transaction timestamps (tier 2).
- **Start timestamp / commit timestamp**: the TSO values a tier-2 transaction
  reads at and commits at.
- **Safe point**: the timestamp below which old versions may be garbage
  collected.
- **Transactional adapter**: `transactional_state_machine<SM, Key, P>`, the
  wrapper that gives a shard's state machine a lock table and transaction
  records.

## Requirements

### Requirement 1: Opt-in at compile time, zero cost when off

**User Story:** As an operator of an existing deployment, I want transactions
to cost me nothing unless my application turns them on, so that the build
cache and every current test keep their behaviour.

#### Acceptance Criteria

1. Transactions SHALL be enabled per host by choosing
   `transactional_state_machine<SM, Key, P>` (or its multi-version variant) as
   the type bundle's state machine. No runtime flag SHALL enable them on a
   host whose state machine is not wrapped.
2. A host whose state machine is not wrapped SHALL produce the same log
   entries, snapshots and lock acquisitions as before this spec, and SHALL
   keep rejecting cross-shard commands with `cross_shard_command_exception`.
3. The adapter SHALL satisfy `state_machine`, and SHALL satisfy
   `splittable_state_machine<_, Key>` exactly when the wrapped `SM` does.
4. A wrapped state machine's snapshot format SHALL be versioned, distinct from
   the unwrapped format, and SHALL refuse to restore an unwrapped snapshot
   (and vice versa) with an error naming both formats.
5. The adapter SHALL need no change to `node<Types>`, to `entry_type`, or to
   any RPC serializer: transaction operations travel as ordinary `normal`
   entries inside the adapter's own envelope.

### Requirement 2: Plain commands under the adapter

**User Story:** As an application developer, I want single-key commands to
keep working unchanged on a transactional host, and to never silently
overwrite a key a transaction has locked.

#### Acceptance Criteria

1. `multi_raft::submit_command` on a wrapped host SHALL envelope the command as
   a plain command, so the application's command encoding does not change.
2. A plain command whose key holds an intent SHALL NOT apply. It SHALL return
   a `key_locked` result naming the holding transaction and its primary key,
   surfaced to the caller as `key_locked_exception`.
3. A plain command on a key with no intent SHALL apply exactly as the wrapped
   state machine applies it.

### Requirement 3: Atomic multi-key writes (tier 1)

**User Story:** As an application developer, I want to write several keys on
different shards so that either every write lands or none does.

#### Acceptance Criteria

1. The system SHALL provide `transaction_coordinator<Client, Key>` with
   `begin(txn_options)`, `put(key, command)`, `get(key)`, `commit()` and
   `rollback()`. `put` buffers locally; nothing reaches a shard before
   `commit()`.
2. When every buffered key resolves to one shard, `commit()` SHALL use one
   1PC entry that applies all writes atomically or none (if any key holds
   another transaction's intent).
3. Otherwise `commit()` SHALL: choose a primary key; prepare every participant
   shard with its keys' writes (the primary's prepare also creating the
   transaction record as `pending`); and, once every prepare has succeeded,
   commit the primary record. The transaction is committed exactly when that
   commit entry applies. Secondary intents SHALL then be committed
   asynchronously; `commit()` SHALL resolve when the primary commits.
4. A prepare SHALL fail with `write_conflict` on any key that holds another
   transaction's intent (no-wait locking). The coordinator SHALL then abort:
   write the record as `aborted` and remove its own intents, best effort.
5. A prepare that names a key outside the receiving shard's range SHALL be
   refused with the existing epoch-mismatch path, and the coordinator SHALL
   re-partition that shard's keys against the refreshed map and retry.
6. Committing a prepared intent SHALL apply its buffered command to the
   wrapped state machine exactly once, at the commit entry's log index.
   Replaying the commit (a duplicate RPC, a resolve racing the coordinator)
   SHALL be a no-op.
7. The transaction record SHALL be keyed by the primary key, so it lives in
   whichever shard owns that key after any split or merge.
8. `commit()` SHALL report one of `committed`, `aborted(reason)` or
   `unknown`. `unknown` means the primary commit was proposed but its outcome
   was not observed (timeout, lost leader). The caller can learn the outcome
   with `transaction_status(txn_id, primary_key)`.

### Requirement 4: Crash recovery without a coordinator

**User Story:** As an operator, I want a crashed coordinator's transaction to
finish or vanish on its own, never leaving keys locked forever.

#### Acceptance Criteria

1. Every transaction record SHALL carry a TTL deadline, stamped by the
   primary shard's leader into the prepare entry as an absolute millisecond
   value, the same way `redis_kv_state_machine` stamps `expire_at_ms`.
   `apply()` SHALL never read a clock.
2. A coordinator SHALL be able to extend its TTL with a heartbeat entry while
   `commit()` is in progress.
3. Any party that meets an intent (a plain write, a prepare, a waiting read,
   the background resolver) SHALL be able to resolve it with a
   `check_and_resolve(txn_id, primary_key, now_ms)` entry on the primary's
   shard. Apply SHALL, deterministically:
   - leave a `committed` or `aborted` record as it is and return it;
   - turn a `pending` record whose deadline is at or before `now_ms` into
     `aborted`;
   - if no record exists, write an `aborted` record (a rollback marker), so
     that a late prepare of the primary cannot then succeed.
4. A prepare of the primary SHALL fail if an `aborted` record (rollback
   marker) already exists for that transaction.
5. The resolver SHALL then commit or remove the intent on the shard where it
   was found.
6. Each shard leader SHALL run a background resolver that, every resolver
   interval, resolves intents older than their TTL. No intent SHALL outlive
   its TTL plus two resolver intervals while the primary shard is available.
7. Transaction records SHALL be removed after every intent is known to be
   resolved, or after a configurable retention (default 10 minutes), by a
   leader-proposed cleanup entry. Records SHALL NOT be removed by `apply()`
   noticing a deadline.

### Requirement 5: Read committed, and waiting on locks (tier 1)

**User Story:** As an application developer, I want reads that never show me
a write that might still abort, and I want to choose whether a read waits for
an in-flight transaction.

#### Acceptance Criteria

1. Intents SHALL be invisible to every read path. A read SHALL return the
   wrapped state machine's last committed value for the key.
2. `txn_isolation::read_committed` SHALL read the committed value and never
   wait. This is the default.
3. `txn_isolation::read_committed_wait` SHALL, on finding an intent for the
   key, wait until it is resolved (polling with backoff, bounded by the
   read's timeout) and resolve it once its TTL has expired, then read.
4. Freshness SHALL stay a separate, orthogonal setting: the existing gateway
   modes `leader`, `any_replica` and `linearizable` SHALL keep their meaning
   for transactional reads.
5. The adapter SHALL expose the wrapped state machine to read paths through
   `inner()` (and a lock-table query `intent_on(key)`), so
   `node::with_state_machine` callers can reach both.

### Requirement 6: Split and merge with transactions in flight

**User Story:** As an operator, I want splits and merges to keep running on a
transactional host, and never to lose or duplicate a lock.

#### Acceptance Criteria

1. The lock table and the transaction records SHALL be part of the adapter's
   replicated state, and SHALL be partitioned by `split_state` and re-joined
   by `absorb` by key: an intent goes with its key, a record with its primary
   key.
2. The adapter SHALL keep both multi-raft laws: `split_state`/`absorb`
   deterministic across replicas, and `absorb(split_state(s))` reproducing
   `get_state()` byte for byte, for any mix of committed data, intents and
   records.
3. Transaction RPCs to a frozen (merging) shard SHALL back off and retry
   exactly as `submit_command` does today.
4. A commit or resolve addressed by key SHALL find its intent after a split or
   merge by re-resolving the key, never by remembering a group id.

### Requirement 7: Timestamp oracle (tier 2)

**User Story:** As an application developer using snapshot isolation, I want
timestamps that every shard agrees on, without depending on wall clocks.

#### Acceptance Criteria

1. The system SHALL define a `timestamp_oracle` concept with
   `allocate(std::size_t n) -> future<timestamp_range>`, returning `n`
   strictly increasing 64-bit values.
2. The system SHALL provide `raft_timestamp_oracle`: a counter whose
   high-water mark is replicated by a dedicated Raft group, hosted by
   `multi_raft` under a reserved group id excluded from split, merge and
   placement.
3. The oracle's leader SHALL reserve values in windows (default 10 000) with
   one Raft entry per window and serve allocations from memory.
4. A new leader SHALL propose a window starting above the committed high-water
   mark before serving. A value SHALL never be served twice, across leader
   changes and restarts.
5. Before answering an allocation batch, the leader SHALL confirm it is still
   leader with a heartbeat round (the read-index check). Concurrent requests
   SHALL share one round. Two clients SHALL therefore never see timestamps
   that run backwards in real time.
6. The oracle SHALL be reachable through a `std::function` hook in
   `multi_raft_config` (as `allocate_shard_ids` is), so a deployment can use
   its own oracle instead.

### Requirement 8: Multi-version state machine and snapshot isolation (tier 2)

**User Story:** As an application developer, I want every read in a
transaction to see the same point in time across shards, and I want to choose
that per transaction.

#### Acceptance Criteria

1. The system SHALL define a `multi_version_state_machine<SM, Key>` concept:
   `apply_at(command, commit_ts)`, `read_at(key, ts)`,
   `latest_commit_ts(key)` and `collect_garbage(safe_point)`.
   `redis_kv_state_machine` SHALL gain a multi-version variant satisfying it.
2. `transactional_state_machine<SM, Key, P, mvcc::on>` SHALL be valid only when
   `SM` satisfies the concept. With `mvcc::on`, every write, plain or
   transactional, carries a commit timestamp. A plain write is a 1PC
   transaction.
3. `txn_isolation::snapshot` SHALL take a start timestamp from the oracle at
   `begin()`, read every key at that timestamp, and take a commit timestamp
   at commit. Prepare SHALL fail with `write_conflict` if any written key has
   a committed version newer than the start timestamp (first committer wins).
4. A snapshot read that finds an intent with a start timestamp at or below
   its read timestamp SHALL wait for or resolve it, as in Requirement 5.3,
   before reading. It SHALL never return a version from an uncommitted
   transaction or skip a committed one below its read timestamp.
5. Snapshot reads SHALL use the linearizable read path (a read-index
   confirmation on the shard leader), whatever the gateway's default
   freshness mode.
6. Versions older than the safe point SHALL be removed by a leader-proposed
   garbage-collection entry, with the newest version at or below the safe
   point always kept. The safe point SHALL be the oracle's current value
   minus a configurable lifetime (default 10 minutes). A transaction whose
   start timestamp is below the safe point SHALL fail with
   `snapshot_too_old`.

### Requirement 9: Serializable (tier 3)

**User Story:** As an application developer, I want to opt into full
serializability for transactions where write skew would be a bug.

#### Acceptance Criteria

1. `txn_isolation::serializable` SHALL behave as `snapshot` and additionally
   record the transaction's read set (point reads only).
2. Prepare SHALL validate each read key on its shard: if a version committed
   after the start timestamp exists, or another transaction holds an intent on
   it, the transaction SHALL abort with `read_conflict`. A validated read key
   SHALL hold a read intent until the commit or abort, which blocks writers
   exactly as a write intent does.
3. A read-only serializable transaction SHALL commit without a prepare.

### Requirement 10: Runtime isolation choice, no silent downgrade

**User Story:** As an application developer, I want to pick isolation per
transaction, and to be told when my build cannot give me what I asked for.

#### Acceptance Criteria

1. `txn_options` SHALL carry `isolation`, `lock_ttl`, `timeout` and
   `max_retries`, with defaults `read_committed`, 3 s, 10 s and 0.
2. `begin()` SHALL throw `isolation_unsupported_exception` naming the
   requested level and the levels the build offers, when the level needs
   `mvcc::on` and the host does not have it.
3. `multi_raft_config` SHALL carry the default `txn_options` for transactions
   that do not name one.

### Requirement 11: Redis gateway surface

**User Story:** As a Redis client, I want the multi-key commands I already use
to be atomic on a transactional gateway.

#### Acceptance Criteria

1. With `--transactions on` (refused unless the gateway was built with the
   adapter), the gateway SHALL accept `MSET`, multi-key `DEL`, `MGET` and
   `MULTI`/`EXEC`/`DISCARD`, each running as one transaction.
2. Without `--transactions on`, those commands SHALL keep today's behaviour
   (unknown command, or single-key only).
3. A connection SHALL be able to set its isolation with
   `KYTHIRA.ISOLATION <level>`, defaulting to `--txn-isolation`.
4. `WATCH`/`UNWATCH` SHALL be accepted only under `mvcc::on`. A watched key
   committed after its `WATCH` SHALL make `EXEC` return a null reply, as in
   Redis.
5. The coordinator's per-shard RPCs SHALL travel over the existing
   authenticated forward links (`redis_forward_link`) as internal commands
   that the gateway refuses from ordinary clients.
6. `doc/redis-gateway.md` SHALL document every isolation level with the
   anomaly it allows, including the read-skew example.

### Requirement 12: Observability

**User Story:** As an operator, I want to see transaction outcomes, conflicts
and stuck locks.

#### Acceptance Criteria

1. The host SHALL emit metrics for: transactions by outcome and isolation,
   1PC versus 2PC count, conflicts by kind, resolves by outcome, intents
   currently held, the oldest intent's age, commit latency by phase, oracle
   allocations and windows, garbage-collected versions, and
   `snapshot_too_old` aborts.
2. Every resolve that aborts another coordinator's transaction SHALL log the
   transaction id, the primary key and the age of the lock.

### Requirement 13: Testing

**User Story:** As a maintainer, I want proof that atomicity and each
isolation level hold under crashes, splits and leader changes.

#### Acceptance Criteria

1. Unit tests SHALL cover the adapter's apply for every envelope operation,
   including duplicate and out-of-order delivery.
2. A property test SHALL check the split/absorb round-trip law for random
   mixes of data, intents and records.
3. An in-process multi-host test SHALL crash the coordinator after every
   protocol step (before any prepare, after some prepares, after all
   prepares, after the primary commit, after some secondary commits) and
   SHALL assert that every transaction ends all-committed or all-absent once
   the resolver has run.
4. A bank-transfer workload SHALL check, under concurrent transfers, leader
   changes and splits:
   - committed state: the total is constant (all tiers);
   - `read_committed`: no read returns an uncommitted value;
   - `snapshot`: every read of all accounts in one transaction sums to the
     total;
   - `serializable`: a write-skew workload (two accounts with a combined
     minimum balance) never violates its invariant.
5. A `docker_chaos` scenario SHALL run the bank workload through the Redis
   gateway with node crashes and partitions, under Docker and rootless Podman
   per `CLAUDE.md`.
6. The `multi-raft-performance` harness SHALL gain rows for 1PC, 2PC with two
   and four shards, and snapshot reads, so each tier's cost is measured.
