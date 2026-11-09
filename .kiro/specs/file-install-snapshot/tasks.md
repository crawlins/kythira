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

- [ ] 5. Leader send and checkpoint registry
  - Format-1 branch in `send_install_snapshot_to` / `_chunk`;
    `checkpoint_registry` with refcounts, age GC and metrics; release on
    leadership loss
  - _Requirements: 5.1-5.4_

- [ ] 6. Resume and pacing
  - `bytes_received` resume for a matching transfer; node-wide
    `snapshot_pacer`
  - _Requirements: 6.1-6.3_

- [ ] 7. Turn on compaction for persistent groups
  - Lift persistent-state-machine Requirement 10.2; the RSS-bounded catch-up
    integration test on a 1 GB group (RocksDB leg)
  - _Requirements: 7.1, 7.2_

- [ ] 8. Faults and chaos
  - Failpoints (corrupt chunk, fsync failure, kill after ingest), leader
    change and follower restart mid-transfer
  - docker-chaos scenario under Docker and rootless Podman
  - `doc/` update
  - _Requirements: 8.1-8.3_
