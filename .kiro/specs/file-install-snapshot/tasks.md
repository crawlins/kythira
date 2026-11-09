# Implementation Plan

Depends on `.kiro/specs/persistent-state-machine/` task 1 (the manifest type)
for every task, and on its task 2 (`checkpoint`/`ingest`) from task 4 on.
Tasks 1-3 can proceed in parallel with that spec's tasks 2-4.

- [x] 1. Wire fields and serializers
  - `types.hpp` fields with defaults; JSON, CBOR, protobuf, Ion, gRPC
    conversion, both `.proto` files (new field numbers)
  - Round-trip, absent-field and bounds tests
  - _Requirements: 1.1-1.5_
  - As built (November 4, 2026): `install_snapshot_format`,
    `k_max_install_snapshot_manifest_bytes` (1 MiB, also the manifest
    encoder's own cap) and `install_snapshot_reject_reason` live in
    `types.hpp`. Request `_format`/`_manifest` sit after `_group_id`, so
    positional initializers keep their meaning. Every serializer writes
    the new fields only when non-default, so a format-0 message is
    byte-identical to before and decoders that refuse unknown keys (JSON,
    CBOR, Ion) still accept it. Field numbers: `raft_messages.proto`
    request 9/10, response 3/4; `raft.proto` request 10/11 (9 is
    `leader`), response 3/4. Decoders refuse a manifest over the cap
    before allocating and a reject code above 255; codes this build does
    not name are kept, not refused. Cases in
    `tests/install_snapshot_wire_extension_cases.hpp`, run by the four
    serializer concept tests, plus two gRPC conversion cases.

- [x] 2. Transports
  - Carry the fields on tcp_rpc (plain, TLS), httplib, Beast, Proxygen,
    libcoap, cantcoap, libnyoci, gRPC, group transport, simulator
  - Multi-chunk format-1 test per transport; size-limit review
  - _Requirements: 2.1-2.3_
  - As built (November 4, 2026): no transport code changed. Every one of
    them hands InstallSnapshot to the rpc serializer or the gRPC
    conversion, and the group layer copies requests whole, so task 1
    carried the fields everywhere. `tests/format1_snapshot_transfer.hpp`
    cuts a small checkpoint (zero-length files included) into format-1
    requests and feeds what arrives to a real `snapshot_stream_writer`;
    each transport's test sends a whole transfer through it, with at least
    one chunk spanning a file boundary. Size review: the worst default
    first chunk (1 MB data plus a 1 MiB manifest, base64 in JSON, about
    2.7 MB) fits HTTP's 10 MiB, gRPC's 16 MiB and tcp_rpc's 64 MiB, checked
    per serializer by the shared wire cases. CoAP's
    `coap_server_config::max_request_size` default was 64 KiB, which
    refused any default chunk on every CoAP backend, format 0 included; it
    is now 4 MiB.

- [x] 3. Stream reader and writer
  - `snapshot_stream_reader` (pread across files) and
    `snapshot_stream_writer` (staging, per-file fsync, SHA-256, manifest
    validation, disk check)
  - Unit tests incl. chunks spanning files and zero-length files
  - _Requirements: 3.1-3.3, 5.1_
  - As built (November 4, 2026): `include/raft/snapshot_stream.hpp`. The
    reader checks file sizes up front and opens, preads and closes per
    read, so it holds nothing between chunks and is thread-safe. The
    writer refuses a bad manifest (including sizes that overflow and the
    reserved name `MANIFEST.kythira`) or a disk without total plus margin
    (default 64 MiB) before creating the staging directory, writes each
    chunk straight to its file(s), hashes as it goes, and fsyncs and
    checks each file when complete; `finish()` fsyncs the directory.
    Every refusal is a `snapshot_stream_error` carrying the reject reason.
    An offset mismatch leaves the transfer usable; a checksum or I/O
    failure marks it failed. `reopen()` rebuilds `bytes_received` from
    disk (verified complete files plus the prefix of the next), ahead of
    task 6. `tests/snapshot_stream_test.cpp`, 18 cases.

- [x] 4. Follower receive and install
  - Format-1 branch in `handle_install_snapshot` and `install_snapshot`;
    staging abandonment and start-up cleanup; reject reasons as response
    codes
  - Node tests on `memory_kv_engine`
  - _Requirements: 3.4-3.6, 4.1-4.3_
  - As built (November 9, 2026), in `include/raft/raft.hpp`:
    - Configuration: `raft_configuration::_snapshot_staging_directory`
      (empty refuses format 1 with `io_error`; each group needs its own)
      and `_snapshot_disk_margin_bytes` (64 MiB). `multi_raft` does not set
      a per-group directory yet; that comes with the leader side.
    - `handle_install_snapshot` keeps the term rules for both formats, then
      hands a persistent group to `handle_format1_install_snapshot_locked`.
      A byte-buffer group refuses format 1 and a persistent group refuses
      format 0, both with `format_mismatch` (4.3).
    - Staging (3.1-3.4): the first chunk decodes the manifest and opens a
      `snapshot_stream_writer` in
      `kythira-staging-<index>-<term>-<manifest digest prefix>`. The node
      holds one transfer, keyed by the leader's term and the snapshot's
      (index, term). A first chunk from a newer leader, or the same
      leader's later snapshot, abandons the staged one and deletes it; the
      same transfer's first chunk again starts over (the leader restarts
      a failed transfer from 0 until task 6's resume). Chunks of an older
      transfer get `stale_snapshot`.
    - Failures: `checksum`, `io_error` and `manifest_invalid` delete the
      staged files; `offset_mismatch` keeps them and reports
      `bytes_received`. Every response to a format-1 chunk carries
      `bytes_received`.
    - 3.5: start deletes every `kythira-staging-*` entry in the staging
      directory and leaves anything else.
    - 3.6: stale term, missing first chunk and offset checks apply as
      before; the stale-snapshot check (at or below the commit index) runs
      on the first chunk, before anything is written, and again on `done`.
      Format 0's rejections now carry their reason codes too.
    - Install (4.1, 4.2): on `done` the writer's `finish()` fsyncs the
      directory, the state machine ingests under the node mutex, and the
      shared tail (`complete_snapshot_install_locked`, factored out of the
      format-0 path) runs the log rule, the indices and the metadata-only
      snapshot save. `install_snapshot` accepts the marker for a
      persistent group. The staging directory is deleted after install.
    - Tests: six new cases in `tests/persistent_node_integration_test.cpp`
      over the simulator: a multi-chunk install that survives restart;
      every reason code (missing first chunk, bad manifest, gap, checksum,
      format 0, stale snapshot, stale term); no staging directory and no
      disk space; abandonment by a later snapshot and by a new leader;
      start-up cleanup; and a crash between ingest and the metadata save.

- [x] 5. Leader send and checkpoint registry
  - Format-1 branch in `send_install_snapshot_to` / `_chunk`;
    `checkpoint_registry` with refcounts, age GC and metrics; release on
    leadership loss
  - _Requirements: 5.1-5.4_
  - As built (November 9, 2026):
    - `include/raft/checkpoint_registry.hpp`: one checkpoint per (index,
      term), in `kythira-checkpoint-<index>-<term>-<n>` under the node's
      `_snapshot_staging_directory`. Leases are counted; a checkpoint
      nobody leases is kept for
      `raft_configuration::_snapshot_checkpoint_max_unused_age` (10
      minutes) so a failed transfer starts again from the same files, and
      is deleted sooner when a newer checkpoint is taken. Deleting on the
      last release instead would make every retry take a new checkpoint.
    - `send_install_snapshot_to` sends a persistent group's snapshot with
      `send_format1_snapshot_to`: under the node mutex it leases a
      checkpoint at `applied_index()` (with that entry's term), then
      `send_format1_chunk` reads one `snapshot_chunk_size` chunk at a time
      with `snapshot_stream_reader` (5.1), the manifest on the first.
      Several followers at the same point share the checkpoint (5.2).
    - Metrics (5.3): `raft_snapshot_checkpoints` and
      `raft_snapshot_checkpoint_bytes` after each acquire and release
      pass; `raft_install_snapshot_refused{reason}` when a follower
      refuses a chunk. A refusal ends the transfer; the next heartbeat
      that finds the follower behind starts again (longer back-off for
      `disk_space` and `checksum` is left to task 6's pacing).
    - 5.4: a chunk reply that finds the node no longer leader, or in a
      later term, ends the transfer and drops its lease; stepping down
      deletes every unused checkpoint at once. Start deletes leftover
      `kythira-checkpoint-*` entries with the staging leftovers.
    - Compaction for persistent groups is still off by default; task 7
      turns it on.
    - Tests: `tests/checkpoint_registry_test.cpp` (sharing, leases, age,
      superseding, a failed checkpoint) and, in
      `persistent_node_integration_test`, a three-node cluster where a
      follower cut off while the leader compacts catches up through a
      format-1 snapshot and then AppendEntries.

- [x] 6. Resume and pacing
  - `bytes_received` resume for a matching transfer; node-wide
    `snapshot_pacer`
  - _Requirements: 6.1-6.3_
  - As built (November 9, 2026):
    - Follower (6.2): a first chunk for the staged (index, term, manifest
      digest) no longer starts over. If anything is staged, the follower
      answers `offset_mismatch` with `bytes_received` and writes nothing
      (metric `raft_install_snapshot_resumed`); the leader's term may be
      newer. A different digest at the same index is a different
      checkpoint and replaces the staged one.
    - Leader (6.1): each follower's latest transfer is kept after a failed
      chunk, with its checkpoint lease, and the next attempt sends the same
      checkpoint from offset 0 while the leader's term is unchanged and the
      log still continues from it (`index >= _snapshot_index`). An
      `offset_mismatch` reply with `bytes_received` makes it go on from
      there, so a resume costs one extra round trip and no new protocol
      field. Three replies in a row that move it nowhere end the transfer.
      The entry goes when the transfer completes or is refused, and on
      stepping down.
    - Pacing (6.3): `include/raft/snapshot_pacer.hpp`, a token bucket that
      runs into debt and returns how long to wait.
      `raft_configuration::_snapshot_pacer` holds it by `shared_ptr` (null,
      the default, is unlimited); `multi_raft` copies one configuration
      into every group, so one pacer is node-wide. The leader waits with a
      future delay before each attempt; no thread sleeps.
    - Found on the way: `raft_configuration::_install_snapshot_retry_policy`
      was never applied; the node used the error handler's built-in
      policy. `start()` now applies it. Its default equals the built-in
      one, so a default configuration is unchanged. The other three retry
      policies in the configuration have the same gap and are left as
      they are.
    - Tests: `tests/snapshot_pacer_test.cpp`; in
      `persistent_node_integration_test`, a follower reporting staged
      progress (same leader and a new one), and a paced three-node
      transfer whose follower is cut off mid-transfer and resumes.

- [x] 7. Turn on compaction for persistent groups
  - Lift persistent-state-machine Requirement 10.2; the RSS-bounded catch-up
    integration test on a 1 GB group (RocksDB leg)
  - _Requirements: 7.1, 7.2_
  - As built:
    - 7.1: `raft_configuration::_persistent_log_compaction` defaults to
      true (a configuration type without the field compacts too). Turned
      off, the node keeps the whole log and says so once at start.
    - Also changed: `_persistent_sync_lag_entries` defaults to 1 instead
      of 0 (never sync). With 0, an engine that makes writes durable only
      when asked (RocksDB with an unsynced WAL) never advanced its durable
      applied index, so compaction, which stops there (persistent-state-
      machine Requirement 3.1), never ran: the RocksDB build of the node
      suite showed it at once. At 1 the node syncs at nearly every
      compaction, once per `_snapshot_threshold_bytes` applied.
    - 7.2: `persistent_node_integration_test.cpp` builds a second time as
      `persistent_node_rocksdb_test` (`KYTHIRA_PERSISTENT_TEST_ROCKSDB`)
      on `rocksdb_kv_engine`. It runs every case except the three that
      hold durability back or simulate a crash, which only
      `memory_kv_engine` can do, and adds
      `follower_catches_up_a_1gb_group_with_bounded_rss`: three replicas,
      one cut off while 1024 incompressible 1 MiB values are written past
      compaction, healed, caught up by a format-1 snapshot, with the
      process's peak RSS growth during catch-up checked against 64 MB. All
      three replicas share the process, so the bound covers leader and
      follower together. RSS is read after `malloc_trim(0)`: without it,
      memory glibc caches in per-thread arenas after a free varied from 4
      to 70 MB between identical runs. The test compacts every 8 MB,
      because the follower takes the log tail after the snapshot into the
      harness's in-memory log store and that tail is resident. CI runs it
      in the Ion Serializer Build job, which has RocksDB. Measured locally
      (November 9, 2026, 4-core sandbox, unoptimized test build): 1024 MiB
      caught up with 0 MiB RSS growth (bound 64 MB), process peak RSS
      449 MB, 15 min 41 s wall clock for the whole case. At 64 MiB, before
      the trim and the 8 MB threshold, growth ranged 36-82 MB.
    - `rocksdb_kv_engine::checkpoint` reads with `fill_cache = false`, so
      a snapshot scan neither evicts the group's hot blocks nor grows the
      process by the block cache's size.
    - Found on the way: `node<Types>` emitted metrics from many threads
      without serializing them. Fixed in PR #735 with `serialized_metrics`.
    - The resume test now compacts once, at its last write: on RocksDB an
      earlier checkpoint, larger than the memory engine's, was still being
      sent at 150 B/s when the test cut the follower.

- [x] 8. Faults and chaos
  - Failpoints (corrupt chunk, fsync failure, kill after ingest), leader
    change and follower restart mid-transfer
  - docker-chaos scenario under Docker and rootless Podman
  - `doc/` update
  - _Requirements: 8.1-8.3_
  - Progress (November 9, 2026), left open for the chaos scenario:
    - 8.1: fault points `raft/snapshot/corrupt_chunk` (follower flips a
      byte of an incoming chunk), `raft/snapshot/fsync_file` (a staged
      file's fsync fails, in `snapshot_stream_writer`) and
      `raft/snapshot/crash_after_ingest` (throws between ingest and the
      metadata save). `persistent_node_fault_test` (the node suite with
      FIU_ENABLE, registered where libfiu is found) asserts `checksum`,
      `io_error` and engine-based recovery, and that a clean resend
      installs after each refusal.
    - 8.2: `follower_restarted_mid_transfer_starts_again_from_zero` (a
      restarted follower drops its staging and takes the transfer from 0)
      and `leader_change_mid_transfer_still_catches_the_follower_up` (the
      old leader is cut off mid-transfer; the new one, which compacted
      itself, finishes). Resume with the same leader was task 6's test.
      Departure from 8.2 as written: a new leader whose checkpoint has the
      same (index, term, digest) also resumes, because resume is keyed on
      the checkpoint, not on who sends it.
    - `doc/persistent_state_machines.md`: kinds, engines, what the node
      does differently, catch-up, configuration, refusals, metrics, fault
      points.
    - 8.3 (November 9, 2026): the second case of
      `services/redis/tests/docker/engine_chaos_test.cpp`
      (persistent-state-machine task 8's scenario). Node 3 is SIGKILLed
      while the other two take 600 incompressible 64 KiB values, so the
      leader's log is compacted past it many times
      (`KYTHIRA_CACHE_SNAPSHOT_THRESHOLD_BYTES` is 4 MiB there). It is
      started again; once its log says it started staging a format-1
      snapshot, it is cut off the network for 4 s, reconnected, then
      SIGKILLed and started again, all within the transfer, which
      `KYTHIRA_CACHE_SNAPSHOT_BYTES_PER_SEC` (new: the gateway's handle on
      `raft_configuration::_snapshot_pacer`, engine mode only) paces to
      4 MiB/s. It must then log a finished install, hold about the load in
      its `engine/` directory, and serve every value (and a write) with
      node 1 killed, so that it is one of the two voters left. Docker and
      rootless Podman, in `redis-module.yml`'s `engine-containers` job.
