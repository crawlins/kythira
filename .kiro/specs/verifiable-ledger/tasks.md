# Implementation Plan

Depends on `.kiro/specs/persistent-state-machine/` tasks 1-3 (concepts,
`memory_kv_engine`, node integration) for task 3 on, its task 5 (RocksDB)
for the RocksDB CI leg, and `.kiro/specs/file-install-snapshot/` for
follower catch-up (task 9). Tasks 1-2 need neither and can start now. Each
task is one PR unless noted.

- [ ] 1. Module skeleton
  - `KYTHIRA_LEDGER` Kconfig option (default off), `services/ledger/` and
    `cmd/ledger_node/` CMake targets, the no-`*_impl.hpp` include check
  - Off-by-default build proved unchanged
  - _Requirements: 1.1-1.4_

- [ ] 2. Ion hash, `dot` and the Merkle accumulator
  - SHA-256 Ion hash over ion-c; amazon-ion-hash test vectors
  - `dot`, proof folding, accumulator with an abstract node store; test
    against the AWS QLDB verification tutorial example
  - _Requirements: 4.6, 12.1, 12.2_

- [ ] 3. Ledger state machine: tables, documents, journal
  - `ledger_state_machine` on the persistent concept: catalog keys, tables,
    insert/replace/patch/delete, revisions, block building and hashing,
    leader hash in the entry and replica check, split/merge veto
  - _Requirements: 2.1, 2.2, 3.1-3.4, 5.1-5.5_

- [ ] 4. Digests and proofs
  - Engine-backed Merkle store, frontier rebuild on open, `GetDigest`,
    `GetBlock`, `GetRevision` with proofs at any tip
  - _Requirements: 4.1-4.4_

- [ ] 5. Transactions and OCC
  - Leader session manager, reads at S, read sets, change markers,
    validation in apply, limits, leader-change failure, retryable errors
  - Serializability property test
  - _Requirements: 7.1-7.7, 12.3_

- [ ] 6. History and equality indexes
  - `history()` by document and time range; index definition, backfill,
    activation entry; bounded scans
  - _Requirements: 6.1-6.3_

- [ ] 7. Native HTTP API, client and `ledger_node`
  - HTTP/JSON and Ion API with mTLS roles and request limits; C++ client
    with OCC retry; the daemon with config and data-directory layout; the
    ledger catalog group
  - _Requirements: 2.3, 2.4, 8.1-8.4_

- [ ] 8. Segments, offload and export
  - Seal entries, deterministic segment files, object-store offload and
    fetch with SHA-256, disk floor, `ExportJournal` jobs
  - _Requirements: 3.5, 3.6, 10.2-10.4_

- [ ] 9. Snapshots and catch-up
  - Checkpoint plus segment list as the snapshot, file-install-snapshot
    catch-up test for a lagging follower
  - _Requirements: 10.1_

- [ ] 10. `ledger_ctl`, digest signing and observability
  - `verify` and `prove` (API, data directory, export); optional CA-signed
    digests; metrics and the hash-mismatch alert path; tamper tests
  - _Requirements: 4.5, 9.1-9.3, 11.1, 11.2, 12.3_

- [ ] 11. Crash, chaos and docs
  - Failpoint kills during apply, sealing and offload; identical journals
    after recovery
  - docker-chaos ledger scenario under Docker and rootless Podman, journal
    verified after
  - `doc/` page: model, API, limits, verification procedure
  - _Requirements: 12.4, 12.5_
