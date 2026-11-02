# Implementation Plan

Depends on `.kiro/specs/persistent-state-machine/` task 1 (the manifest type)
for every task, and on its task 2 (`checkpoint`/`ingest`) from task 4 on.
Tasks 1-3 can proceed in parallel with that spec's tasks 2-4.

- [ ] 1. Wire fields and serializers
  - `types.hpp` fields with defaults; JSON, CBOR, protobuf, Ion, gRPC
    conversion, both `.proto` files (new field numbers)
  - Round-trip, absent-field and bounds tests
  - _Requirements: 1.1-1.5_

- [ ] 2. Transports
  - Carry the fields on tcp_rpc (plain, TLS), httplib, Beast, Proxygen,
    libcoap, cantcoap, libnyoci, gRPC, group transport, simulator
  - Multi-chunk format-1 test per transport; size-limit review
  - _Requirements: 2.1-2.3_

- [ ] 3. Stream reader and writer
  - `snapshot_stream_reader` (pread across files) and
    `snapshot_stream_writer` (staging, per-file fsync, SHA-256, manifest
    validation, disk check)
  - Unit tests incl. chunks spanning files and zero-length files
  - _Requirements: 3.1-3.3, 5.1_

- [ ] 4. Follower receive and install
  - Format-1 branch in `handle_install_snapshot` and `install_snapshot`;
    staging abandonment and start-up cleanup; reject reasons as response
    codes
  - Node tests on `memory_kv_engine`
  - _Requirements: 3.4-3.6, 4.1-4.3_

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
