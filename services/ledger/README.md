# Ledger module

A native verifiable ledger for Kythira: ledgers of Ion documents whose every
committed change is a block in a SHA-256 hash-chained journal, with digests,
Merkle proofs, full document history and serializable optimistic
transactions. Each ledger is one Raft group on a persistent state machine.
The spec is `.kiro/specs/verifiable-ledger/`; this directory follows
`services/README.md`. QLDB wire compatibility is out of scope, but block and
revision hashing follow QLDB's published conventions, so its verification
procedure works on Kythira's proofs.

Built when `CONFIG_LEDGER=y` (`services/ledger/Kconfig`, off by default). It
needs ion-c (`CONFIG_ION_SERIALIZER`, the opt-in `ion` vcpkg feature), so
`configs/ci_ion_defconfig` turns it on and ci.yml's Ion Serializer Build job
builds and tests it; every other configuration leaves it off.

## What is here

| Path | What | Task |
|---|---|---|
| `include/kythira/ledger/ledger_hash.hpp` | `hash256` and SHA-256 | 2 |
| `include/kythira/ledger/ion_hash.hpp` | the Amazon Ion hash over ion-c's reader, with any hash function (SHA-256 for the ledger) | 2 |
| `include/kythira/ledger/merkle.hpp` | QLDB's `dot`, proof folding, in-memory Merkle roots and proofs, and the append-only accumulator over an abstract node store | 2 |
| `include/kythira/ledger/ion_value.hpp` | a small Ion value tree (containers parsed, scalars kept as their own binary) for taking entries apart, patching documents and building records, written as Ion binary, Ion text or JSON | 3, 8 |
| `include/kythira/ledger/journal_types.hpp` | the transaction entry, statements, patch paths, revisions and blocks in Ion, and their hashes | 3 |
| `include/kythira/ledger/engine_merkle_store.hpp` | the Merkle node store over a `kv_engine`, buffering writes so they join the block's batch | 4 |
| `include/kythira/ledger/ledger_state_machine.hpp` | `basic_ledger_state_machine<Engine>`: tables, document writes, the journal, the leader's block hash and the replica check, digests and block and revision proofs at any tip, snapshot reads and OCC, history, equality indexes and bounded scans, sealed segments and export job state, on the persistent state machine contract | 3-6, 8 |
| `include/kythira/ledger/ledger_sessions.hpp` | leader-side transactions: snapshot reads and index lookups, read sets, buffered statements, limits, expiry, leader change, and the disk floor | 5, 6, 8 |
| `include/kythira/ledger/journal_segments.hpp` | segment files, seal and offload entries, durable file writes, upload to and fetch from any `key_object_store` | 8 |
| `include/kythira/ledger/journal_export.hpp` | `ExportJournal` jobs: start and finish entries and the leader's export to an object-store prefix as Ion text, Ion binary or JSON with a manifest | 8 |
| `cmd/ledger_node/` | placeholder daemon (`--self-test`); the server is task 7 | 1 |
| `include/kythira/ledger/journal_verifier.hpp` | offline verification: `journal_verifier` (chain, revision, entries and block hashes, revision labels, digest at a saved tip), segment and export directory readers, proof documents and `check_proof` | 10 |
| `include/kythira/ledger/ledger_catalog.hpp` | the ledger catalog group: ledger records, create/update/delete/purge, deletion protection; refuses split and merge | 7 |
| `include/kythira/ledger/ledger_api.hpp` | transport-free native API: routing, roles, commit lock with `stale_block` retry, error mapping | 7 |
| `cmd/ledger_ctl/` | `ledger_ctl verify --export DIR \| --segments DIR [--digest FILE]` and `ledger_ctl prove --proof FILE --digest FILE`; exit 0 verified, 1 failed (first bad block named), 2 usage or I/O | 10 |
| `tests/ion_hash_test.cpp` | the amazon-ion/ion-hash-test suite (`tests/vectors/`) and the properties the ledger relies on | 2 |
| `tests/ledger_merkle_test.cpp` | `dot` and folding against the AWS QLDB tutorial's functions, the tree shape, every proof at every tip, reopening, tampering | 2 |
| `tests/ledger_state_machine_test.cpp` | the state machine on `memory_kv_engine`: every statement, rejections, stale and diverging entries, replicas, restart, checkpoint, the split veto, and every block and revision proof at every tip | 3, 4 |
| `tests/ledger_transactions_test.cpp` | snapshot reads, read-set validation, limits, sessions, and the serializability property test | 5 |
| `tests/ledger_verifier_test.cpp` | the verifier over a state machine, segments and exports; every single-byte change to a segment file caught; each kind of tampered block named; proofs offline; `ledger_ctl` exit codes | 10 |
| `tests/ledger_api_test.cpp` | the API over a local backend: catalog, roles, documents, sessions with OCC conflicts, history, digests and proofs, exports, error codes | 7 |
| `tests/ledger_segments_test.cpp` | sealing into identical files on every replica, proofs over sealed blocks, redone and rejected seals, damaged and offloaded segments, export in every format | 8 |
| `tests/ledger_history_index_test.cpp` | history by document and table with time bounds and pages, index creation, build steps and maintenance, snapshot lookups and their conflicts, filtered scans with a byte budget | 6 |
| `tests/tools/qldb_dot_vectors.py` | prints the vectors `ledger_merkle_test` checks, from the tutorial's published Python | 2 |
| `tests/vectors/` | `ion_hash_tests.ion`, copied unchanged from amazon-ion/ion-hash-test (Apache-2.0; see its README) | 2 |

Tasks 3 to 6 build on the persistent state machine contract and
`memory_kv_engine` (`.kiro/specs/persistent-state-machine/` task 1). Running
a ledger in a node (task 7) needs that spec's task 3, and catch-up (task 9)
needs `.kiro/specs/file-install-snapshot/`.

## Core targets and variables used

This is the list a split into its own repository would need a Kythira
package to export.

| From core | Used for |
|---|---|
| `kythira_core_headers` | `include/` and `generated/`, through `kythira_ledger` |
| `raft/persistent_state_machine.hpp` | the `persistent_state_machine` and `kv_engine` concepts, `write_batch`, `key_range`, `persistent_storage_error` |
| `raft/memory_kv_engine.hpp` | the engine the unit tests run on |
| `raft/ion_preflight.hpp` | the ion-c reader owner and the binary-Ion length preflight the Ion hash runs before reading untrusted input |
| `raft_ion_serializer` | ion-c's link and its `DECNUMDIGITS` definition, through `kythira_ledger` |
| `OpenSSL::Crypto` | SHA-256, through `kythira_ledger` |
| `kythira_test_pch` | the test precompiled header and its links (Boost.Test), through `add_raft_completion_test` |
| `cmake/kythira_test_helpers.cmake` | `add_raft_completion_test`, `kythira_scale_test_timeouts()` |
| `KYTHIRA_BUILD_LEDGER` | set by the top level from `CONFIG_LEDGER`, ion-c and OpenSSL |

## Splitting it out

As for the other modules: Kythira would have to export the targets above
from an installed CMake package with tagged releases. The ledger includes no
core `*_impl.hpp` (`check_module_boundaries.py` enforces that on every pull
request), so nothing else ties it to the tree.
