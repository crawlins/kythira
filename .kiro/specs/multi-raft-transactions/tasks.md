# Implementation Plan

## Implementation Status

Tasks 1-6 done (October 28, 2026): `include/raft/txn_envelope.hpp`,
`include/raft/txn_state_machine.hpp`, the host's `submit_txn`,
`include/raft/txn_coordinator.hpp` and the in-process
`include/raft/multi_raft_txn_client.hpp`.

Each task is one independently mergeable PR: it builds, its tests pass, and
an unwrapped host is byte- and lock-identical before and after it
(Requirement 1.2). Tasks 1-9 are tier 1 (atomic writes, read committed),
10-15 tier 2 (oracle, MVCC, snapshot isolation), and 16-17 tier 3
(serializable). A later tier never blocks an earlier one from shipping.

The open questions at the end of `design.md` do not block tasks 1-6.
Question 1 (gateway default) must be answered before task 9, and question 3
(oracle placement) before task 10.

## Tasks

### Tier 1: atomic multi-key writes, read committed

- [x] 1. Envelope
  - `include/raft/txn_envelope.hpp`: encode/decode for every op in the
    design's table, versioned, with length and count caps applied before any
    allocation (the untrusted-input rule of PR #507)
  - Result encoding and the exception types
    (`key_locked_exception`, `isolation_unsupported_exception`)
  - Tests: round trip and truncation/garbage refusal for every op
  - _Requirements: 1.5, 2.2_
  - Notes: magic `0x4B`, version 1, then an op code that is the
    `txn_envelope<Key>` variant index plus one. Keys go through the host's
    `shard_key_codec`. Caps: 65,536 entries per list, 64 KiB per key,
    64 MiB per command; every count is also checked against the bytes left
    before `reserve()`, and the length check compares against the bytes
    remaining rather than `offset + n`, which a forged length would wrap.
    Unknown ops and enum values, booleans other than 0/1 and trailing
    bytes are refused. Results carry only the fields their status uses;
    the conflict statuses name the key and, when an intent caused it, the
    holder and its primary. `txn_isolation` and `txn_record_status` live
    here too, since the exceptions name them.

- [x] 2. Wrapper: plain, one-phase, snapshot format
  - `include/raft/txn_state_machine.hpp` with `_inner`, `_intents`,
    `_records`; `plain` and `one_phase` apply; `inner()`, `intent_on()`;
    versioned `get_state`/`restore_from_snapshot` and the format refusal
  - Static assertions that the wrapper satisfies `state_machine`, and
    `splittable_state_machine` exactly when `SM` does
  - Tests: `tests/txn_state_machine_test.cpp` (plain, 1PC, conflicts, format)
  - _Requirements: 1.1, 1.3, 1.4, 2.2, 2.3, 3.2_
  - Notes: the snapshot opens with `kythira-txn-sm` and a version byte,
    then the oracle high-water mark, the three maps in key order and the
    wrapped state as a length-prefixed blob. A snapshot without that magic
    is refused with a message naming both formats. Snapshot table counts
    are bounded by the bytes left, not the envelope's 65,536 cap, since a
    shard may hold more intents than one transaction may name. The
    partitioner is a requires-clause on `key_of` rather than the
    `partitioner` concept, because `multi_raft.hpp` will include this
    header. `mvcc::on` is a static_assert until tasks 11-12.

- [x] 3. Wrapper: prepare, commit, abort, resolve
  - `prepare`, `commit_primary`, `commit_secondary`, `abort`, `heartbeat`,
    `check_and_resolve` (including the rollback marker) and `cleanup`
  - Every op idempotent by `txn_id`
  - Tests: every op, duplicate and out-of-order delivery, the TTL table
  - _Requirements: 3.3, 3.4, 3.6, 3.7, 4.1, 4.2, 4.3, 4.4, 4.7_
  - Notes: a third map, `_settled` (written key, then `txn_id`, to
    committed or aborted), makes a prepare or 1PC that arrives after its
    transaction finished on this shard a no-op. Without it a retried
    secondary prepare would install a fresh intent and the resolver would
    apply the write twice. `txn_cleanup` drops settled entries with the
    record. A `commit_primary` or `check_and_resolve` that finds no record
    writes the rollback marker; an `abort` after the commit point reports
    the committed record and changes nothing.

- [x] 4. Wrapper: split and absorb
  - Partition and re-join the lock table and records with the inner state
  - Tests: `tests/txn_split_absorb_property_test.cpp`, the round-trip law
    over random data, intents and records
  - _Requirements: 6.1, 6.2_
  - Notes: intents and settled entries move with their own key, records
    with their primary key, and the oracle high-water mark with the
    leftmost child. The property test also checks each child holds only
    its own range's locks, since the law alone would pass a split that
    left everything in one child; dropping the settled map from the right
    children fails it.

- [x] 5. Host: envelope plain commands, `submit_txn`
  - `if constexpr` envelope in `submit_command`; `submit_txn` with every-key
    range check, epoch-mismatch refusal, frozen backoff and leader hint
  - `multi_raft_config::txn_defaults`, `resolver_interval`
  - Prove the unwrapped path unchanged: existing multi-raft suites pass
    unmodified, and a test asserts identical log bytes for an unwrapped host
  - _Requirements: 1.2, 2.1, 3.5, 6.3, 6.4, 10.3_
  - Notes: the envelope is added at submit time, after the cross-shard
    admission check, so the partitioner still reads the application's own
    command. A `key_locked` result becomes `key_locked_exception`; any
    other plain result is the wrapped machine's payload, as before.
    `submit_txn` shares `route_and_run` through a `txn_keys` argument: a
    stray key is refused with `shard_epoch_mismatch_exception` carrying the
    local descriptor plus the routing rows for every stray key, and nothing
    is proposed. `txn_options` lives in `txn_state_machine.hpp` because the
    config carries the defaults. `resolver_interval` is in the config but
    unused until task 7. Tests: `tests/multi_raft_txn_host_test.cpp`
    records each store's `normal` appends and asserts an unwrapped host
    logs a plain command byte for byte.

- [x] 6. Coordinator
  - `include/raft/txn_coordinator.hpp`: `txn_participant_client` concept,
    `begin/put/get/commit/rollback`, the 1PC path, primary choice,
    concurrent prepares, abort on conflict, async secondaries,
    re-partition on epoch mismatch, `unknown`, `transaction_status()`
  - `read_committed` and `read_committed_wait` reads
  - An in-process `txn_participant_client` over `multi_raft::submit_txn`
  - Tests: `tests/multi_raft_txn_test.cpp`: three in-process hosts, a crash
    hook after every protocol step, all-or-nothing after resolution
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.8, 5.1, 5.2, 5.3, 5.5, 10.1, 13.3_
  - Done. `transaction_coordinator<Client, Key, KeyCodec>` buffers writes
    per key and, at `commit()`, partitions them through `resolve()`. One
    group sends a `txn_one_phase`; a routing refusal there falls through to
    two phases over the repaired map, since the host refuses a stray key
    before proposing. Otherwise the primary is the smallest key of the
    largest group, every prepare goes out at once, and the primary is
    heartbeated every TTL/3 while they are outstanding. A group whose
    prepare comes back with a stale-routing refusal is re-partitioned and
    re-sent alone. Any conflict or failure aborts best effort; once the
    primary commits, the secondaries' commits are fire-and-forget and
    `drain()` waits for them. A crash hook fires after each `txn_step`.
    Deviations: the TTL deadline is stamped from `client.now_ms()`, the
    participants' clock, not the coordinator's; and a transaction does
    not read its own buffered writes, which stay opaque commands until
    they apply. Sends return any `kythira::future`, never `std::future`
    (which `complete_conversion_validation_property_test` forbids).
    `multi_raft_txn_client` is the in-process client: each send runs on a
    thread the client joins, fulfils a `future_default`, and skips hosts
    that do not lead the group; `refresh()`
    teaches every host the descriptors an epoch-mismatch refusal carried,
    and reads go to the leader through `with_state_machine`. Tests:
    `tests/multi_raft_txn_test.cpp`, three hosts and two shards: 1PC,
    cross-shard 2PC, a conflict abort, both `read_committed` modes, a stale
    route repaired and re-partitioned, every crash point all-or-nothing
    after resolution, and an isolation the build lacks refused at `begin`.

- [ ] 7. Leader-side resolver, cleanup, metrics
  - Resolver, record cleanup and their rate limits in the tick's apply phase
  - Requirement 12's metrics and the resolve log line
  - Tests: an abandoned transaction's locks clear within TTL + two
    intervals; records cleaned up after resolution or retention
  - _Requirements: 4.5, 4.6, 4.7, 12.1, 12.2_

- [ ] 8. Bank workload, tier 1
  - `tests/multi_raft_txn_bank_test.cpp`: concurrent transfers with leader
    transfers, splits and merges mid-run; constant committed total; no
    uncommitted read under `read_committed`
  - _Requirements: 6.1, 6.3, 13.4_

- [ ] 9. Redis gateway: tier-1 surface
  - `--transactions`, `--txn-isolation`, `KYTHIRA.ISOLATION`; `MSET`,
    multi-key `DEL`, `MGET`, `MULTI`/`EXEC`/`DISCARD`
  - `KYTHIRA.TXN` internal commands over the forward link, refused from
    ordinary clients; the gateway's `txn_participant_client`
  - Plain `SET`/`DEL` on a locked key: resolve, then retry once
  - `doc/redis-gateway.md`: the commands and each level's allowed anomaly
  - Tests: gateway integration suite, with transactions off proving today's
    behaviour unchanged
  - _Requirements: 2.2, 11.1, 11.2, 11.3, 11.5, 11.6_

### Tier 2: timestamp oracle, MVCC, snapshot isolation

- [ ] 10. Timestamp oracle
  - `timestamp_oracle` concept, `raft_timestamp_oracle`, `oracle_reserve` in
    the wrapper, the reserved group (empty range; skipped by routing, the
    policy and operators), `allocate_timestamps` hook
  - Batched allocation with a heartbeat confirmation per batch
  - Tests: `tests/raft_timestamp_oracle_test.cpp`: monotonic across leader
    changes and restarts, never served twice, deposed leader refused
  - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5, 7.6_

- [ ] 11. Multi-version Redis KV state machine
  - `multi_version_state_machine` concept;
    `redis_kv_mvcc_state_machine` with versioned values, tombstones,
    versioned expiry sweeps, `collect_garbage`, and split/absorb
  - Tests: version reads, GC keeps the newest at or below the safe point,
    and the split/absorb round-trip law
  - _Requirements: 8.1, 8.6_

- [ ] 12. Wrapper and coordinator under `mvcc::on`
  - Timestamps on every write (plain as 1PC); first-committer-wins at
    prepare; snapshot reads at start_ts through the linearizable path;
    waiting on or resolving lower-timestamp intents
  - `isolation_unsupported_exception` on `mvcc::off`
  - _Requirements: 8.2, 8.3, 8.4, 8.5, 10.2_

- [ ] 13. Garbage collection and `snapshot_too_old`
  - Safe-point computation, leader-proposed `gc` entries, rejection of old
    start timestamps
  - _Requirements: 8.6, 12.1_

- [ ] 14. Bank workload, tier 2, and gateway `WATCH`
  - Snapshot invariant: every all-accounts read sums to the total
  - Gateway built with `mvcc::on`: `WATCH`/`UNWATCH`, snapshot `MGET`
  - _Requirements: 11.4, 13.4_

- [ ] 15. Chaos and performance
  - `docker/txn-chaos-compose.yml`, `tests/docker_chaos/txn_bank_chaos_test.cpp`
    through `redis_gateway_node`, with peers by service name, under Docker and
    rootless Podman
  - `multi-raft-performance` rows: 1PC, 2PC over 2 and 4 shards, and
    snapshot reads
  - _Requirements: 13.5, 13.6_

### Tier 3: serializable

- [ ] 16. Read-set validation
  - `read_keys` in `prepare`, read intents, `read_conflict`, and read-only
    commit without prepare
  - _Requirements: 9.1, 9.2, 9.3_

- [ ] 17. Write-skew workload
  - Two-account minimum-balance invariant under `serializable` (and shown
    violable under `snapshot`, so the test proves it can fail)
  - _Requirements: 13.4_

## Notes

- The multi-raft spec's "Out of Scope: Cross-shard transactions" and
  Requirement 18.6 stay true for unwrapped hosts. This spec does not edit
  them. It adds a second, opt-in path.
- The build cache gateway gains nothing from this spec and keeps
  `--transactions off`. Its keys are content-addressed and written once.
