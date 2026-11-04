# Implementation Plan

Ordered so the contract and the in-memory engine exist before any node code
changes, and so every node change is tested on the in-memory engine before
RocksDB is linked. Each task is one PR unless noted.

- [x] 1. Concepts and the in-memory engine
  - `persistent_state_machine.hpp`: `kv_engine`, `persistent_state_machine`,
    `persistent_splittable_state_machine`, `snapshot_manifest`
  - `memory_kv_engine` with a held-back durable sequence
  - Engine conformance suite (parameterised, so task 5 reuses it)
  - Static-assert tests that every existing state machine still selects the
    byte-buffer path
  - _Requirements: 1.1-1.5, 7.1, 7.4_
  - Done 2026-11-04. As built:
    - Keys and values are `std::string` (unsigned byte order); a
      `key_range` is `[start, end)` with an optional end. `write_batch`
      holds put, erase and erase-range operations; each non-empty batch
      takes one `engine_sequence`.
    - `kv_engine` has `get`, `scan` (a visitor that returns false to stop,
      which is how callers bound a scan for Requirement 5.3), `write`,
      `latest_sequence`, `durable_sequence`, `sync(seq)`, `checkpoint`,
      `ingest`, `drop` and `format_tag`. `ingest` and `drop` are writes and
      return their sequence.
    - `storage_kind_of<SM>` picks the path; a type satisfying both kinds is
      persistent (1.5). `byte_buffer_state_machine` names the old path.
    - `snapshot_manifest` has a bounded binary encoding and a digest, since
      it is also file-install-snapshot's first-chunk payload. Decode checks
      every length against the input and caps (1 MiB, 16 Ki files, 255-byte
      names) before allocating, and refuses names that could leave a
      staging directory (`..`, separators, NUL).
    - `memory_kv_engine` scans in chunks of 256 with its mutex released
      between chunks, so a visitor may call back into the engine. Its
      checkpoint is one file, `range.kvs`.
    - The redis and build-cache state machines are type-checked only where
      the gateway is built (`KYTHIRA_PSM_TEST_HAS_REDIS`), so the test
      builds with the gateway off and with no Folly.
    - Tests: `tests/persistent_state_machine_test.cpp` (32 cases: the
      conformance suite in `tests/kv_engine_conformance.hpp`, durability
      hold and crash, manifest bounds, concept selection, and a toy
      persistent state machine showing atomic apply, replay after a crash
      and the durable applied index).

- [x] 2. Reference state machine on the engine
  - `persistent_kv_state_machine`: the Redis command set, atomic apply with
    the applied index, replay no-op at or below it, TTL deadlines and sweeps
  - Unit and property tests against the in-memory `redis_kv_state_machine`
    as the oracle
  - _Requirements: 2.1-2.4, 9.1_
  - As built (November 4, 2026):
    `services/redis/include/kythira/redis/persistent_kv_state_machine.hpp`,
    templated on the engine. It lives in the Redis module, not core,
    because it reuses the module's command parser and reply types. Data keys are `ns + key` holding the deadline
    then the value. The applied index and the counters (bytes, keys,
    expiring) live in one `meta` record at `ns + range.start`, inside the
    group's own range rather than at the design's
    `<ns>/group/<id>/applied_index`. That way checkpoint, ingest and drop
    carry or remove it with the range, and no other group's range can
    contain it. Every apply is one batch with that record; a replay at or
    below the applied index writes nothing. Later commands of one entry
    see earlier ones' effects through a pending overlay. `split_at` writes
    each child's record in one batch (counters recounted by a scan of each
    part, no key moves); `adopt` folds and erases the source's record.
    The read API (`lookup`, `collect_expired`, `for_each_key`, the size
    hints) matches the in-memory machine's so the gateway can switch, but
    selecting it in `redis_gateway_node` needs the node path of task 3.
    `services/redis/tests/persistent_kv_state_machine_test.cpp`, 9 cases: oracle
    equivalence over 40 seeded random workloads (replies, values,
    deadlines, counters, expiry scans, restart), replay, one batch per
    apply, crash with held-back durability and replay, checkpoint/ingest
    between engines next to another group, split/adopt, namespaces.

- [ ] 3. Node integration: restart, compaction, reads
  - `if constexpr` branches in `initialize_from_storage`,
    `create_snapshot_locked`, `compact_log`, `serve_read_after_apply`
  - Metadata-only snapshots (`external_state`); compaction bound by the
    durable applied index; the lag metric
  - `InstallSnapshot` refused for persistent groups and compaction held off
    until the companion spec lands (Requirement 10.2)
  - Config validation for the object-store engines (Requirement 10.1)
  - Crash tests with the held-back durable sequence
  - _Requirements: 3.1-3.4, 4.1-4.3, 5.1-5.3, 10.1, 10.2_

- [ ] 4. Split and merge without copies
  - `split_at` / `adopt` in the reference state machine
  - `multi_raft_impl.hpp` split and colocated-merge branches; failpoint
    equivalents
  - Existing split/merge integration tests parameterised over both kinds
  - _Requirements: 6.1-6.5_

- [ ] 5. RocksDB engine
  - vcpkg feature and `KYTHIRA_ENGINE_ROCKSDB` Kconfig option (default off),
    `src/rocksdb_kv_engine.cpp`
  - Column families, `WriteBatch`, WAL-synced sequence to durable index,
    `Checkpoint`, `IngestExternalFile`, `DeleteRange`
  - Conformance suite on RocksDB; one CI leg with the option on
  - Record the build-time delta in this task
  - _Requirements: 7.2-7.5_

- [ ] 6. Transaction state in the engine
  - Persistent storage for `_intents`, `_records`, `_settled` and the
    deadline index in the `txn` column family, same batch as inner effects
  - Split/merge partition by range
  - Bank workload on a persistent host
  - _Requirements: 8.1-8.4_

- [ ] 7. Gateway wiring
  - `redis_gateway_node` build option selecting `persistent_kv_state_machine`
    on RocksDB; data directory layout and disk budget in config
  - _Requirements: 9.1_

- [ ] 8. Crash and chaos
  - Failpoints between batch commit and sync, and between checkpoint and
    compaction; kill tests
  - docker-chaos scenario: `SIGKILL` replicas under write load, Redis
    linearizability checker; Docker and rootless Podman
  - _Requirements: 9.2, 9.3_

- [ ] 9. Measurement and docs
  - Bench rows: in-memory vs RocksDB (proposals/s, p99), restart time for a
    10 GB group
  - `doc/` page for the persistent path and its limits
  - _Requirements: 4.4, 9.4_

Tasks 1-4 need no new dependency. Task 3's compaction and catch-up become
fully usable only with `.kiro/specs/file-install-snapshot/` (which needs
task 1's manifest type and task 2's `checkpoint`/`ingest`).
