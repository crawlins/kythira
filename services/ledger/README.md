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
| `cmd/ledger_node/` | placeholder daemon (`--self-test`); the server is task 7 | 1 |
| `tests/ion_hash_test.cpp` | the amazon-ion/ion-hash-test suite (`tests/vectors/`) and the properties the ledger relies on | 2 |
| `tests/ledger_merkle_test.cpp` | `dot` and folding against the AWS QLDB tutorial's functions, the tree shape, every proof at every tip, reopening, tampering | 2 |
| `tests/tools/qldb_dot_vectors.py` | prints the vectors `ledger_merkle_test` checks, from the tutorial's published Python | 2 |
| `tests/vectors/` | `ion_hash_tests.ion`, copied unchanged from amazon-ion/ion-hash-test (Apache-2.0; see its README) | 2 |

The ledger state machine (task 3) and everything after it need the
persistent state machine (`.kiro/specs/persistent-state-machine/` tasks
1-3), and catch-up needs `.kiro/specs/file-install-snapshot/`.

## Core targets and variables used

This is the list a split into its own repository would need a Kythira
package to export.

| From core | Used for |
|---|---|
| `kythira_core_headers` | `include/` and `generated/`, through `kythira_ledger` |
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
