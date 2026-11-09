# DynamoDB module

A DynamoDB-compatible API for Kythira: tables, items, expressions, Query
and Scan, transactions, TTL and DynamoDB Streams, served over the AWS JSON
1.0 protocol on Kythira's Raft groups. The spec is
`.kiro/specs/dynamodb-compatible-api/`; this directory follows
`services/README.md`.

Built when `CONFIG_DYNAMODB=y` (`services/dynamodb/Kconfig`, off by
default). `configs/ci_full_defconfig` turns it on, so ci.yml builds and
tests it; `configs/ci_no_dynamodb_defconfig` is the module-off
configuration `.github/workflows/dynamodb-module.yml` checks.

## What is here

| Path | What | Task |
|---|---|---|
| `model/` | the pinned botocore `service-2.json` models for DynamoDB and DynamoDB Streams, and how to upgrade them (its README) | 1 |
| `include/kythira/dynamodb/model/` | the shape tables `scripts/gen_aws_json_shapes.py` generates from them (checked in; a ctest fails when stale) | 1 |
| `include/kythira/dynamodb/operations.hpp` | target prefixes, the error namespace, and the implemented-operation lists | 1 onward |
| `doc/unsupported.md` | every modeled operation not implemented yet, why, and the task that adds it | 1 onward |
| `evidence/` | what the AWS CLI, boto3, aws-sdk-java-v2, the lock client, KCL 2 and 3 and the Streams adapter send (its README) | 1 |
| `tools/evidence/` | the recording stub, the client programs, `record.sh` (Docker, rootless Podman or native) and `summarize.py` | 1 |
| `tools/fetch_models.sh`, `tools/regen_models.sh` | fetch a botocore tag's models; regenerate the shape tables | 1 |
| `tests/dynamodb_model_coverage_test.cpp` | every modeled operation is implemented or listed as unsupported, and the shape tables load | 1 |
| `tests/dynamodb_wire_evidence_test.cpp` | every recorded request frames and validates against the pinned model, unknown members refused | 1 |

The rest follows the plan in the spec's `tasks.md`.

Shared pieces live in core `include/raft/`, because the Kinesis module uses
them too: the AWS JSON protocol codec (`aws_json_protocol.hpp`) and its
model generator (`scripts/gen_aws_json_shapes.py`).

## Core targets and variables used

This is the list a split into its own repository would need a Kythira
package to export.

| From core | Used for |
|---|---|
| `kythira_core_headers` | `include/` (the AWS JSON codec), through `kythira_dynamodb` |
| `boost_json` | the codec and the tests, through `kythira_dynamodb` |
| `Python3_EXECUTABLE` | the `*_model_header_fresh` ctests |
| `KYTHIRA_AWS_JSON_SHAPES_GENERATOR` | the same ctests (core's `scripts/gen_aws_json_shapes.py`) |
| `kythira_test_pch` | the test precompiled header and its links (Boost.Test, Boost.JSON), through `add_raft_completion_test` |
| `cmake/kythira_test_helpers.cmake` | `add_raft_completion_test`, `kythira_scale_test_timeouts` |
| `KYTHIRA_BUILD_DYNAMODB` | set by the top level from `CONFIG_DYNAMODB`, OpenSSL and Boost |

The top level names the module in two places: the `KYTHIRA_BUILD_DYNAMODB`
gate and `add_subdirectory(services/dynamodb)`.
