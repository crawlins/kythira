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

- [x] 7. Leader-side resolver, cleanup, metrics
  - Resolver, record cleanup and their rate limits in the tick's apply phase
  - Requirement 12's metrics and the resolve log line
  - Tests: an abandoned transaction's locks clear within TTL + two
    intervals; records cleaned up after resolution or retention
  - _Requirements: 4.5, 4.6, 4.7, 12.1, 12.2_
  - Done. The tick marks a pass due every `resolver_interval`. The pass
    runs on a resolver thread the host starts and joins with itself,
    because resolving means proposing entries and waiting for them to
    commit, which the tick must never do.
    - **Each pass, per led group:** the pass reads the wrapper's new
      `open_transactions()` and `finished_transactions()` views through
      `with_leader_state_machine`. That way a new leader that has not yet
      applied its own term's entry acts on nothing.
    - **Expired intents:** for each transaction whose earliest intent
      deadline has passed (oldest first, at most `resolver_batch`), it
      proposes `check_and_resolve` on the primary's shard. That happens
      locally, or through `set_txn_forwarder` when another host leads it.
      It then applies the fate here: `commit_secondary` or `abort`.
      `pending` leaves the lock, because a secondary's intent keeps its
      prepare's deadline and only the primary's record is heartbeated.
    - **Cleanup:** finished transactions get one `txn_cleanup` per batch
      once this leader has seen them finished for `txn_record_retention`
      (default 10 minutes, counted per leader, so it restarts on a leader
      change). Cleanup apply now skips an item whose record is still
      pending.
    - **Metrics:**
      - `kythira.multiraft.txn.op{op,result}` and `.op.latency{op}` (commit
        latency by phase);
      - `txn.outcome{outcome,path}` (1PC versus 2PC);
      - `txn.conflict{kind}`;
      - `txn.resolve{group,outcome}`;
      - `txn.intents.held{group}` and `txn.intents.oldest_age_ms{group}`;
      - `txn.cleanup{group}`.
    - **Log line:** every resolver abort logs a warning with the txn id,
      primary, lock age and time past deadline.
    - **Clock:** `multi_raft_config::txn_clock_ms` (default system clock)
      is the transaction clock; `multi_raft_txn_client::now_ms()` now reads
      it from the host, so deadlines are stamped and judged on one clock.
    - **Deviations:**
      - Isolation is not a metric dimension, because the host never sees
        it: the envelope does not carry it.
      - A lock's age is measured from when the current leader first saw
        it, a lower bound after a leader change, because intents carry no
        prepare time and adding one would change the snapshot format.
      - Records are dropped by retention only. "Every intent known
        resolved" would need the primary to hear from every secondary.
        That is why retention must comfortably exceed the TTL plus the
        resolve time: a committed record cleaned up while a crashed
        coordinator's secondary still holds its intent would be answered
        `aborted`.
      - Resolves by a coordinator's own `read_committed_wait` reader are
        logged by neither side yet.
    - **Tests:**
      - `tests/txn_state_machine_test.cpp`: both views, and that cleanup
        never drops a pending record.
      - `tests/multi_raft_txn_test.cpp`, with nobody but the resolver
        acting:
        - a coordinator crashed before and after its commit point has all
          locks cleared within TTL + two intervals, ending all-absent and
          all-committed respectively;
        - a transaction kept alive by heartbeats past its TTL keeps its
          secondary's lock (`pending`), and is rolled forward by the
          resolver once it commits;
        - a finished transaction's record and settled entries are kept
          through the retention and dropped after it.

- [x] 8. Bank workload, tier 1
  - `tests/multi_raft_txn_bank_test.cpp`: concurrent transfers with leader
    transfers, splits and merges mid-run; constant committed total; no
    uncommitted read under `read_committed`
  - _Requirements: 6.1, 6.3, 13.4_
  - Done.
    - **Setup:** sixteen accounts over two shards and three hosts. Four
      workers transfer between random pairs while a chaos thread, every
      250 ms, moves a shard's leadership, or splits the lower shard and
      then merges the child back. The run lasts at least 8 s, and until
      each kind of chaos has happened twice.
    - **Transfers are commutative `add` commands.** They come from
      `bank_sm`, a splittable wrapper over the key/value test machine.
      Read-then-put transfers would lose updates under tier 1 by design,
      since read validation is tier 3.
    - **Poison transactions:** every eighth transaction writes a marker
      and credits an account with money that does not exist, then its
      coordinator dies at `prepared`. The resolver must abort it. A
      `read_committed` reader polls the markers and must never see one,
      and a poison credit that ever landed would break the total.
    - **Typical run:** about 650 committed, 900 aborted (mostly
      conflicts), 40 poison, 11 leader moves, 10 splits and 10 merges.
    - **Bug found and fixed in the host.** The first runs lost money, 3 of
      4 times. `route_and_run` refused writes to a merging source, but
      not to a shard with a split in flight. A write accepted after the
      split was proposed was appended after the split entry, so it
      applied to the narrowed parent after the cut, for a key the child
      now owns. It was acknowledged and never read again; a merge back
      overwrote it. This was not specific to transactions: any plain
      write could be lost.
      - **The fix:** a write now backs off while its shard is
        `splitting`, as it already did for `merging_source`. A per-group
        `_proposal_gate` (shared across a write's check and append,
        exclusive across `admit()` into `splitting` or `merging_source`)
        closes the window between a write's check and the claim.
      - **Regression test:**
        `multi_raft_split_integration_test/a_write_racing_a_split_is_never_lost`
        lost 1-3 of about 250 acknowledged plain writes on every run
        before the fix, and none after.
      - **Residual:** a leader elected while an uncommitted split entry
        is in its log has not admitted the split itself, so it still
        accepts writes until that entry applies. Closing that needs an
        apply-time range check, which `node` does not offer for normal
        entries.

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
