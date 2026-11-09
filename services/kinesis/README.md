# Kinesis module

A Kinesis Data Streams-compatible service for Kythira. Unmodified AWS SDKs,
the AWS CLI, KPL and KCL point `--endpoint-url` at a Kythira cluster. A
Kinesis shard is a routing key in Kythira's Raft groups, not a group of its
own, and record bytes live in the core record segment store, outside the
Raft snapshot. The spec is `.kiro/specs/kinesis-compatible-streams/`; this
directory follows `services/README.md`.

Built when `CONFIG_KINESIS=y` (`services/kinesis/Kconfig`, off by default).
`configs/ci_full_defconfig` turns it on, so ci.yml builds and tests it;
`configs/ci_no_kinesis_defconfig` is the module-off configuration
`.github/workflows/kinesis-module.yml` checks.

## What is here

| Path | What | Task |
|---|---|---|
| `model/service-2.json` | the pinned botocore `kinesis` model (botocore 1.43.100, unchanged; Apache-2.0, `model/LICENSE` and `model/NOTICE` are botocore's) | 1 |
| `tools/gen_operations.py` | generates `include/kythira/kinesis/kinesis_operations.hpp` from the model; `--check` fails when the header is stale | 1 |
| `include/kythira/kinesis/kinesis_operations.hpp` | the generated operation table (checked in, so a model upgrade shows its diff) | 1 |
| `implemented-operations.txt` | operations the gateway implements | 1 onward |
| `doc/unsupported.md` | every other operation, with its planned task, reason and the exact error it returns | 1 onward |
| `tools/check_coverage.py`, `tools/test_check_coverage.py` | the coverage check (Requirement 2.2) and its fixture tests | 1 |
| `cmd/kinesis_gateway_node/` | the daemon; a placeholder with `--self-test` until task 6 | 1 |

Core pieces the module builds on live in `include/raft/`, because DynamoDB
uses them too: `record_segment_store.hpp` (task 3), and later the CBOR and
event-stream framing in `aws_json_protocol.hpp` and `aws_eventstream.hpp`
(task 2).

To upgrade the model, copy `botocore/data/kinesis/<version>/service-2.json`
from a newer botocore wheel over `model/service-2.json`, bump
`BOTOCORE_VERSION` in `tools/gen_operations.py`, rerun it, and place any new
operation in `implemented-operations.txt` or `doc/unsupported.md`. The
pull request carries the regenerated header's diff (Requirement 2.4).

## Core targets and variables used

This is the list a split into its own repository would need a Kythira
package to export.

| From core | Used for |
|---|---|
| `kythira_core_headers` | `include/` and `generated/`, through `kythira_kinesis` |
| `OpenSSL::Crypto` | SigV4, segment digests and iterator tokens, through `kythira_kinesis` |
| `Python3_EXECUTABLE` | the coverage and generated-header tests |
| `kythira_test_pch` | the guard the tests are entered under |
| `cmake/kythira_test_helpers.cmake` | `kythira_scale_test_timeouts` |

## What a split would still need

Nothing beyond the table above yet. The front end (task 4) adds the shared
Beast HTTPS host and the SigV4 verifier, and the daemon (task 6) the
multi-raft host.
