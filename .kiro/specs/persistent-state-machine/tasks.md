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

- [x] 3. Node integration: restart, compaction, reads
  - `if constexpr` branches in `initialize_from_storage`,
    `create_snapshot_locked`, `compact_log`, `serve_read_after_apply`
  - Metadata-only snapshots (`external_state`); compaction bound by the
    durable applied index; the lag metric
  - `InstallSnapshot` refused for persistent groups and compaction held off
    until the companion spec lands (Requirement 10.2)
  - Config validation for the object-store engines (Requirement 10.1)
  - Crash tests with the held-back durable sequence
  - _Requirements: 3.1-3.4, 4.1-4.3, 5.1-5.3, 10.1, 10.2_
  - As built (November 9, 2026), in `include/raft/raft.hpp`: `node<Types>`
    picks the path with `k_persistent_state` (`storage_kind_of`).
    `raft_types` accepts either kind; it checks only the persistent
    *shape* (`persistent_state_machine_shape` in `types.hpp`, because
    `types.hpp` cannot include the full concept), and the node
    `static_assert`s the full one.
    - Restart (4.1-4.3): `initialize_from_storage` takes the snapshot index
      and term from the saved metadata and `_last_applied` from
      `applied_index()`. It refuses to start when the applied index is below
      the snapshot index, or when the saved snapshot holds byte-buffer state.
      When the applied index is above the log, it warns and starts with the
      commit index at the applied index.
    - Snapshots (3.1-3.3): `create_snapshot_locked` saves metadata at
      `min(durable_applied_index(), last_applied)`, with the configuration in
      force at that index, then compacts through it. The `external_state`
      flag is a fixed marker, `kythira-persistent-snapshot/1`, carried in
      `state_machine_state`. That avoids changing every persistence engine's
      snapshot encoding, and restart checks for it.
      `raft_configuration::_persistent_sync_lag_entries` calls the state
      machine's optional `sync_durable()` first when the durable index lags
      by more than that many entries.
    - Lag metric (3.4): `raft_persistent_apply_lag` is emitted after every
      apply batch.
    - Reads (5.1-5.2): `serve_read_after_apply` returns an empty buffer.
      `read_state` documents this as a read-index barrier. The one in-tree
      caller that forwards the bytes is `multi_raft::read_state`
      (`multi_raft_impl.hpp`); it passes the empty buffer through. 5.3 (scan
      bounds) is the engine's `scan_visitor`, which already stops when the
      caller returns false.
    - 10.2: `raft_configuration::_persistent_log_compaction` defaults to
      off. With it off, the group never snapshots and logs that once at
      start. A persistent follower refuses InstallSnapshot with
      `format_mismatch`, and a persistent leader sends none. Flip the
      default when file-install-snapshot's leader side lands.
    - 10.1: `object_store_persistence_engine` (and Alibaba's, which derives
      from it) declares `restores_state_from_snapshot_store`. `node<Types>`
      `static_assert`s against pairing it with a persistent state machine.
    - Tests: `tests/persistent_node_integration_test.cpp`, 10 cases over
      `tests/persistent_register_state_machine.hpp` (a minimal register
      machine on `memory_kv_engine`). They cover the read barrier, restart
      without replay, a crash with durability held back and then replay,
      compaction off by default, compaction stopping at the durable index
      and following it once released, the lag-bound sync, restart after
      compaction, both refusals at restart, and the InstallSnapshot refusal
      over the simulator.

- [x] 4. Split and merge without copies
  - `split_at` / `adopt` in the reference state machine
  - `multi_raft_impl.hpp` split and colocated-merge branches; failpoint
    equivalents
  - Existing split/merge integration tests parameterised over both kinds
  - _Requirements: 6.1-6.5_
  - As built (November 9, 2026):
    - Engine: `multi_raft_config::state_engine` is a `shared_ptr` to the
      node's engine (`persistent_engine_of_t<SM>`). `create_group_impl`
      opens each group's state machine on its shard range of that engine
      and throws when the field is unset. Shard keys must be
      `std::string`.
    - Split (6.1): `apply_split` calls `split_at`, which narrows the parent
      and writes every child's metadata in one batch. Children are seeded
      with a metadata-only snapshot (the task 3 marker) at the parent's
      applied index, so `open` and the snapshot agree (4.2). No
      `split_state`, `restore_from_snapshot` or key copy. A replay after a
      crash finds the parent already cut to the derived child's range and
      skips the cut. `persistent_splittable_state_machine` gained `range()`
      for that check. `right_derive` is refused: `split_at` keeps the
      lowest part on the parent.
    - Merge (6.2, 6.3): the colocated target calls `adopt(source range)`
      instead of `get_state` and `absorb`. `adopt` takes a neighbour on
      either side, in both reference machines. The non-colocated refusal
      path is unchanged.
    - Teardown: `destroy_group` drops a persistent group's range, except a
      merged-away source, whose keys are the survivor's. A replica created
      again later would otherwise open a stale applied index.
    - 6.4: admin entries, idempotence and failpoints are unchanged; the
      crash-consistency suite runs over both kinds.
    - 6.5: the split policy hooks moved to their own concept,
      `split_policy_state_machine` (`splittable_state_machine.hpp`), which
      a persistent machine can satisfy without `split_state`/`absorb`.
      Every policy site in `multi_raft_impl.hpp` uses it.
    - Tests: `include/raft/persistent_test_state_machine.hpp`
      (`persistent_test_kv_state_machine`, the persistent twin of
      `test_key_value_state_machine`, same commands). The split, merge
      and crash-consistency suites build a second binary with
      `KYTHIRA_PERSISTENT_STATE_MACHINE` (`*_persistent_test`). One split
      case is excluded there: a lost child replica rebuilt from an inbound
      message needs snapshot transfer (Requirement 10.2), which
      file-install-snapshot brings.

- [x] 5. RocksDB engine
  - vcpkg feature and `KYTHIRA_ENGINE_ROCKSDB` Kconfig option (default off),
    `src/rocksdb_kv_engine.cpp`
  - Column families, `WriteBatch`, WAL-synced sequence to durable index,
    `Checkpoint`, `IngestExternalFile`, `DeleteRange`
  - Conformance suite on RocksDB; one CI leg with the option on
  - Record the build-time delta in this task
  - _Requirements: 7.2-7.5_
  - As built:
    - `include/raft/rocksdb_kv_engine.hpp` is a pimpl that names no
      RocksDB type; only `src/rocksdb_kv_engine.cpp` includes RocksDB.
      Kconfig `ENGINE_ROCKSDB` (default n, menu "Storage Engines"), vcpkg
      feature `rocksdb`, CMake library `raft_rocksdb_engine`, which defines
      `KYTHIRA_ENGINE_ROCKSDB_AVAILABLE`.
    - One database per node with column families `data`, `txn` and `meta`
      next to RocksDB's empty `default`. `write` is one `WriteBatch` under
      a write lock; its sequence is RocksDB's latest sequence after it.
      `erase_range` and `drop` are `DeleteRange`; an unbounded end is
      bounded at the column family's last key, read under the lock.
    - Writes go to the WAL unsynced. `sync` is `SyncWAL`, and
      `durable_sequence()` is the latest sequence it covered; what a reopen
      recovers counts as durable.
    - Deviation from 7.2: `checkpoint` writes the range's keys, read at one
      RocksDB snapshot, as one SST per non-empty column family instead of
      calling RocksDB's `Checkpoint`. A RocksDB checkpoint copies the whole
      database, which holds every group on the node, and could not be
      ingested into one range.
    - `ingest` verifies the manifest and every file's SHA-256, then hands a
      range-tombstone SST and the checkpoint's SST for each column family
      to one `IngestExternalFiles` call. That call is atomic, so a crash
      never leaves the range half replaced. Files are moved (hard links)
      where the filesystem allows, so the checkpoint directory is spent.
    - Tests: `tests/rocksdb_kv_engine_test.cpp` runs the shared conformance
      suite (15 cases) plus reopen, ingest between other groups' keys,
      ingest then reopen, `create_if_missing = false`, and
      `sync_every_write`.
    - CI: the RocksDB feature rides in the Ion Serializer Build job's tree
      (`configs/ci_ion_defconfig`, the only image with opt-in ports), which
      builds and runs `rocksdb_kv_engine_test`; clang-tidy covers both new
      files in its `ion` variant.
    - Build-time delta (Requirement 7.5), measured on PR #746's CI and
      merged November 10, 2026. With the option off nothing changes: no
      RocksDB port, header or library is built. With it on:
      - The vcpkg `rocksdb` port (10.4.2, `[core,zlib]`) took 8.5 minutes
        on a cold Ion Serializer Build job (run 42758893169, job
        129639742071), out of a 16-minute job. That is paid once per
        vcpkg cache key: the job's `actions/cache` entry restores it, and
        the next run's whole Ion job took 5.6 minutes. vcpkg's S3 binary
        cache did not take the package from a PR run: the upload is a
        multipart upload and the PR cache key was refused
        (`NoSuchBucket` on `CreateMultipartUpload`), so a run on `main`
        has to be the one that stores it there.
      - Kythira's own code is two translation units:
        `src/rocksdb_kv_engine.cpp` (11.9 s, 0.6 GB peak) and
        `tests/rocksdb_kv_engine_test.cpp` (10.2 s, 0.65 GB) with g++ 13
        at -O2 on a 4-vCPU VM, about 22 s of compile, plus linking the
        static RocksDB into the engine's users.

- [x] 6. Transaction state in the engine
  - Persistent storage for `_intents`, `_records`, `_settled` and the
    deadline index in the `txn` column family, same batch as inner effects
  - Split/merge partition by range
  - Bank workload on a persistent host
  - _Requirements: 8.1-8.4_
  - As built:
    - New concept `persistent_staged_state_machine`
      (`persistent_state_machine.hpp`): `stage(command, batch)`,
      `commit_staged(batch, index)`, `discard_staged()` and `key_prefix()`.
      One transaction entry can apply several inner commands (a commit rolls
      every intent forward), and a second `apply` with the same index would
      be taken for a replay, so the wrapper stages them into one batch it
      owns. `persistent_kv_state_machine` and
      `persistent_test_kv_state_machine` implement it; later stages of an
      entry read earlier ones' writes.
    - `transactional_state_machine` over such a machine is itself a
      persistent splittable state machine (`k_persistent`), with no
      `get_state`, so `node<Types>` and `multi_raft` take the persistent
      path. Each entry writes the keys whose transaction state it changed
      into the batch that carries the inner writes and the applied index
      (8.1). A command that throws part way leaves nothing: the batch is
      discarded and the maps are read back.
    - Layout deviation: one `txn` value per key at the key's own engine
      key, holding its intent, the records it is primary of and its
      settled entries; the high-water mark and safe point ride in the value
      at the range's start. The design's suffixed keys (`<key>/i`, ...) can
      sort past a range end that extends the key, which would put them in
      the neighbouring group.
    - Deviation: the maps stay in memory as a cache of the column family,
      read back at `open`, `ingest` and `adopt`. Restart therefore reads
      the transaction state held (locks, and records not yet cleaned up),
      never the data, and the resolver keeps scanning the cache, so the
      deadline index is not built. Moving the maps out of memory is a
      follow-up if transaction state ever outgrows it.
    - The snapshot is the engine checkpoint, which covers every column
      family (8.2). `split_at` forgets the children's part; `adopt` reads
      the source's part back and merges the high-water marks and safe
      points by maximum, in a second batch whose loss is harmless (8.3).
    - `mvcc::on` over a persistent machine is refused at compile time.
    - Tests: `txn_state_machine_persistent_test` (a random workload held
      to the in-memory wrapper, reply by reply, with reopen checks; a power
      cut; a throwing command; split and merge against `split_state` and
      `absorb`; adopt from below; drop), staging cases in
      `persistent_kv_state_machine_test`, and
      `multi_raft_txn_bank_persistent_test` (8.4: 833 commits through 11
      splits, 10 merges and 11 leader moves on a local run, total kept).

- [x] 7. Gateway wiring
  - `redis_gateway_node` build option selecting `persistent_kv_state_machine`
    on RocksDB; data directory layout and disk budget in config
  - _Requirements: 9.1_
  - As built:
    - Kconfig `REDIS_GATEWAY_ENGINE` (default n; depends on `REDIS_GATEWAY`,
      `ENGINE_ROCKSDB` and not `REDIS_GATEWAY_SNAPSHOT_ISOLATION`) adds one
      translation unit, `run_cbor_engine.cpp`, and links
      `raft_rocksdb_engine`. The state machine is a build choice, as the
      requirement says; which storage a node runs is still the runtime
      `KYTHIRA_CACHE_PERSISTENCE`, now with a third value, `engine`, which
      a build without the option refuses.
    - With `CONFIG_REDIS_GATEWAY_TRANSACTIONS=y` the engine store is
      wrapped in `transactional_state_machine` (task 6), so transactions
      are allowed with `engine` as with `memory`.
    - Layout under `KYTHIRA_CACHE_DATA_DIR`: `engine/` (the database),
      `raft/<group>` (binary segmented Raft logs; the log must be durable
      because engine writes are not synced) and `snapshots/` (the staging
      directory, on the database's filesystem).
    - Disk budget: the file mode's `cache_disk_guard`, with the size of
      `engine/` as the store bytes, so `KYTHIRA_CACHE_MAX_DISK_BYTES`
      evicts least-recently-read keys and the free-space floors refuse
      writes, voters' reports included. `KYTHIRA_CACHE_MIN_FREE_BYTES` is
      also `_snapshot_disk_margin_bytes`. The chunk store's and the HTTP
      front end's variables are refused in this mode.
    - `redis_gateway_value_access` gains a specialization for
      `persistent_kv_state_machine`.
    - Tests: `redis_gateway_engine_store_integration_test` runs all 32
      gateway integration cases over the persistent machine with one
      `memory_kv_engine` per host. The Ion Serializer Build job turns the
      option on (`configs/ci_ion_defconfig`) and builds
      `redis_gateway_node`, the only tree with RocksDB.
    - Smoke run (local, RocksDB 8.9): three nodes with
      `KYTHIRA_CACHE_PERSISTENCE=engine` and two shards took SETs, were all
      killed and restarted, and served every value again from their
      databases.

- [x] 8. Crash and chaos
  - Failpoints between batch commit and sync, and between checkpoint and
    compaction; kill tests
  - docker-chaos scenario: `SIGKILL` replicas under write load, Redis
    linearizability checker; Docker and rootless Podman
  - _Requirements: 9.2, 9.3_
  - Progress (November 9, 2026):
    - Fault points `raft/persistent/before_sync` (compaction, just before
      the node asks the engine to sync) and `raft/persistent/before_compact`
      (after the snapshot point is saved, before the log is truncated), in
      `node<Types>::create_persistent_snapshot_locked`.
    - Kill tests in `persistent_node_fault_test`: every entry is a counter
      increment (`+key` in the register test machine), so a lost entry
      reads 0 and a doubled one 2. Killing before the sync with durability
      held loses every batch after the first; the restarted node replays
      them from the log, once each. Killing between the snapshot and the
      truncation leaves log entries the engine already holds; none applies
      again.
    - docker-chaos scenario (November 9, 2026):
      `services/redis/tests/docker/engine_chaos_test.cpp` over
      `services/redis/docker/engine-chaos-compose.yml`, three
      `redis_gateway_node` replicas with `KYTHIRA_CACHE_PERSISTENCE=engine`
      (target `docker-engine-chaos-tests`). Six clients GET and SET 30 keys
      across both shards with linearizable reads while the test SIGKILLs and
      restarts each node in turn (twice), two at once (no quorum until they
      return), then all three. Every operation goes into a Jepsen-style
      history (`tests/docker_chaos/history.hpp`); errors after a write was
      sent count as indeterminate. The final reads go through every node,
      and the WGL checker must answer `ok` for every key (`unknown` fails
      too). It runs in `redis-module.yml`'s new `engine-containers` job,
      under Docker and rootless Podman, on the Ion Serializer Build job's
      tree (`configs/ci_ion_defconfig`, g++-14, the same vcpkg cache key).
      Locally both cases pass under Docker (about 2,650 acknowledged
      writes and as many reads, 101 indeterminate) and rootless Podman
      (2,879 writes, 2,877 reads, 135 indeterminate); every key `ok`, each
      search under 310 steps.
    - Found by it: `persistent_kv_state_machine` had no `sync_durable()`,
      so on RocksDB (which syncs only when asked) its durable applied index
      never moved, and a gateway on engine storage never compacted a log:
      the scenario's 38 MB bulk load logged "snapshotting" on every entry
      and kept the whole log. The node tests had not seen it because their
      register machine has `sync_durable()`. `persistent_kv_state_machine`
      and `persistent_test_kv_state_machine` now have it, and
      `transactional_state_machine` forwards it when its inner machine
      does, with unit tests for each.
    - Seen on the way, not fixed here: the daemon built on the Boost future
      backend segfaults when a peer is SIGKILLed: the backtrace ends in an
      asio strand enqueue, run from a failed pre-vote response's
      continuation. It matches the unfiled
      "Boost-backend pre-vote crash" in TODO.md's disk-backed-build-cache
      row. CI's gateway is the Folly build, which the local runs used.

- [x] 9. Measurement and docs
  - Bench rows: in-memory vs RocksDB (proposals/s, p99), restart time for a
    10 GB group
  - `doc/` page for the persistent path and its limits
  - _Requirements: 4.4, 9.4_
  - As built:
    - A `persistence` axis in `multi_raft_performance_report`
      (`services/redis/tests/multi_raft_persistence_bench_rows.hpp`, one
      translation unit per arm in `services/redis/tests/bench_rows/`):
      `redis_kv_state_machine`, `persistent_kv_state_machine` over
      `memory_kv_engine`, and over `rocksdb_kv_engine` where
      `raft_rocksdb_engine` is built. Every arm logs to the fsynced
      `binary_file_persistence_engine`, so only the state machine differs.
    - Write rows (three hosts, two shards, 16 in flight, 128 B and 4 KiB)
      report proposals/s and p50/p95/p99. Restart rows (one host) load
      `--state-bytes` of 64 KiB values, then stop and start the host five
      times over the same directories, timing each to a leader, a new
      commit and a read of the last loaded value on every shard.
      `benchmark_result` gained `_state_machine`, `_restart_time` and
      `_state_bytes`, written to the CSV and JSON; `--data-dir` puts the
      rows on a chosen disk.
    - Measured November 9, 2026 (4 vCPU, 15 GiB, ext4): a 10 GB RocksDB
      group restarts in 0.92-1.02 s (median of five, two runs) with a
      303 MiB peak RSS; the in-memory machine takes 4.5 s at 1 GiB and
      8.6 s at 2 GiB, peaks at 12.1 GiB of RAM at 2 GiB, and ran out of
      memory loading 4 GiB. Write rows run at 1,900-2,050 SETs/s on every
      arm, bound by the fsynced log. Table in
      `doc/persistent_state_machines.md`, *Measurements*.
    - The measurement found RocksDB's default `max_total_wal_size` let the
      `meta` column family's small writes keep about 1 GiB of WAL alive,
      replayed on the first open after a load (2.4 s at 1 GiB).
      `rocksdb_kv_engine_options::max_total_wal_bytes` (256 MiB) cuts that
      to 0.9 s.
    - `multi_raft_performance_report_persistence` (label `performance`)
      runs the axis at a tenth of its budget.

Tasks 1-4 need no new dependency. Task 3's compaction and catch-up become
fully usable only with `.kiro/specs/file-install-snapshot/` (which needs
task 1's manifest type and task 2's `checkpoint`/`ingest`).
