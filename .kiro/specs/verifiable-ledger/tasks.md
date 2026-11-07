# Implementation Plan

Depends on `.kiro/specs/persistent-state-machine/` tasks 1-3 (concepts,
`memory_kv_engine`, node integration) for task 3 on, its task 5 (RocksDB)
for the RocksDB CI leg, and `.kiro/specs/file-install-snapshot/` for
follower catch-up (task 9). Tasks 1-2 need neither and can start now. Each
task is one PR unless noted.

- [x] 1. Module skeleton
  - `KYTHIRA_LEDGER` Kconfig option (default off), `services/ledger/` and
    `cmd/ledger_node/` CMake targets, the no-`*_impl.hpp` include check
  - Off-by-default build proved unchanged
  - _Requirements: 1.1-1.4_
  - Done 2026-11-07. As built:
    - The symbol is `CONFIG_LEDGER` (`services/ledger/Kconfig`, in the
      "Service Modules" menu), `depends on ION_SERIALIZER && OPENSSL`; the
      in-memory engine is always built, so 1.1's engine condition holds.
      The top level sets `KYTHIRA_BUILD_LEDGER` and enters
      `services/ledger/` after `tests/`, like `services/iam/`.
    - The daemon lives at `services/ledger/cmd/ledger_node/`, per
      `services/README.md`, as a `--self-test` placeholder until task 7.
    - The include check is `scripts/check_module_boundaries.py`'s
      `impl-header` rule, which already runs over every `services/*/` on
      every pull request; no ledger-specific CMake regex was added.
    - Only `configs/ci_ion_defconfig` turns the module on (it needs
      ion-c); every other defconfig, and so every other CI build, is
      unchanged.

- [x] 2. Ion hash, `dot` and the Merkle accumulator
  - SHA-256 Ion hash over ion-c; amazon-ion-hash test vectors
  - `dot`, proof folding, accumulator with an abstract node store; test
    against the AWS QLDB verification tutorial example
  - _Requirements: 4.6, 12.1, 12.2_
  - Done 2026-11-07. As built:
    - `ion_hash.hpp` is header-only and parameterised over the hash
      function, so the suite's identity vectors check the exact bytes;
      `ion_hash()` is the SHA-256 form. All 166 identity and 5 MD5 cases of
      amazon-ion/ion-hash-test pass (`tests/vectors/`, copied unchanged).
    - ion-c quirks it works around: timestamps are local fields plus an
      offset, so they are converted to UTC; annotations are read before
      stepping into a container (stepping in drops them); strings and lobs
      are read in pieces, because a refused `get_string_length` on a long
      text value consumes part of it; and the walk has its own depth limit,
      since the text reader ignores `max_container_depth`.
    - The binary-Ion length preflight and the reader owner moved from
      `raft/ion_serializer.hpp` to core `raft/ion_preflight.hpp`, so the
      ledger reads Ion without the serializer or a future backend.
    - The AWS tutorial publishes the verification code but no hash values,
      so `tests/tools/qldb_dot_vectors.py` runs the tutorial's Python
      functions as published and prints the vectors the C++ test checks.
    - Level 0 of the node store holds the leaves (block hashes) as well as
      the interior nodes. The tree is RFC 6962's shape with `dot`; its
      roots for 1-9 leaves are pinned in the test.

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
