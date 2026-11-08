# IAM module

An AWS IAM-compatible service for Kythira: users, groups, roles, managed
and inline policies, access keys and STS, hosted by its own Raft group, and
the policy evaluator that front ends embed to authorize requests locally.
The spec is `.kiro/specs/iam-compatible-auth/`; this directory follows
`services/README.md`.

Built when `CONFIG_IAM=y` (`services/iam/Kconfig`, off by default).
`configs/ci_full_defconfig` turns it on, so ci.yml builds and tests it;
`configs/ci_no_iam_defconfig` is the module-off configuration
`.github/workflows/iam-module.yml` checks.

## What is here

| Path | What | Task |
|---|---|---|
| `include/kythira/iam/iam_policy.hpp` | the policy parser, per-type validation, size quotas, and the action and ARN matchers | 10 |
| `tests/iam_policy_test.cpp` | its unit tests and the corpus runner | 10 |
| `tests/corpus/` | the policy corpus (format in its README) | 10 onward |
| `include/kythira/iam/iam_state_machine.hpp` | the IAM Raft group's state machine, its command set, and the leader-side stamping helpers | 8 |
| `include/kythira/iam/iam_seal.hpp` | KEK derivation and AES-256-GCM sealing of access-key and STS-key secrets | 8 |
| `tests/iam_state_machine_test.cpp` | their unit and property tests | 8 |
| `include/kythira/iam/iam_admin.hpp` | the `/v1/admin/*` operations (create and list accounts, rotate the STS key, register a replica) | 9 |
| `include/kythira/iam/iam_routing.hpp` | request routing by credential-scope service, and Query error XML | 9 |
| `cmd/iam_node/` | the daemon: the IAM Raft group over mutual-TLS RPC and the HTTPS listener | 9 |
| `cmd/kythira-iam/` | the admin CLI | 9 |
| `docker/iam_node/` | the image the compose scenario runs | 9 |
| `tests/iam_admin_test.cpp`, `tests/iam_node_test.cpp` | the admin operations, and the binaries as a three-node cluster with a replica | 9 |
| `tests/iam_test_pki.hpp`, `tests/iam_test_certs.cpp` | the throwaway test PKI, in process and as a tool | 9 |
| `include/kythira/iam/iam_validation.hpp` | AWS's IAM and STS request-parameter rules (names, tags, durations, session policies and tags, what each credential may call), for the Query and STS handlers | invariants |
| `tests/iam_validation_test.cpp` | one case per invariant group in `.kiro/specs/iam-compatible-auth/invariants.md` | invariants |
| `tests/docker/` | the compose scenario (three voters and a replica, by service name) | 9 |
| `tools/` | the pinned AWS-managed policies (`aws_managed_policies.json`), the configure-time generator for `<kythira/iam/aws_managed_policies_data.hpp>`, and the script that refreshes the JSON from AWS | 8 |

Condition operators and
variables (task 11), the evaluator (task 12) and the rest follow the plan in
the spec's `tasks.md`.

Shared pieces live in core `include/raft/`, because core or other modules
use them: `jwt.hpp`, the cloud credential sources, and later the SigV4
verifier, the Query codec and the `aws_request_authorizer` concept.

## Core targets and variables used

This is the list a split into its own repository would need a Kythira
package to export.

| From core | Used for |
|---|---|
| `kythira_core_headers` | `include/` and `generated/`, through `kythira_iam` |
| `OpenSSL::Crypto` | `iam_seal.hpp`, through `kythira_iam` |
| `Python3_EXECUTABLE` | running `tools/gen_aws_managed_policies.py` at configure time |
| `kythira_test_pch` | the test precompiled header and its links (Boost.Test, Boost.JSON), through `add_raft_completion_test` |
| `cmake/kythira_test_helpers.cmake` | `add_raft_completion_test` |
| `certificate_authority` | `iam_node`: `KYTHIRA_HAS_OPENSSL` for the TLS RPC headers, and the future backend through `network_simulator` |
| `kythira_host_node_id` | `KYTHIRA_NODE_ID_KIND` for `iam_node`, `iam_test_certs` and the tests |
| `boost_json`, `OpenSSL::SSL`, `Threads::Threads` | `iam_node` and `kythira-iam` |
| `certificate_authority` (tests) | the test PKI (`iam_test_certs`, `iam_node_test`) |
| `kythira_test_support` | `test_ports.hpp` for `iam_node_test` |
| `add_docker_chaos_scenario_test`, `kythira_detect_container_runtime`, `KYTHIRA_IMAGE_CONTEXT` | the compose scenario and its image (`cmake/kythira_test_helpers.cmake`) |
| `KYTHIRA_BUILD_IAM` | set by the top level from `CONFIG_IAM`, OpenSSL and Boost |

The top level names the module in two places: the `KYTHIRA_BUILD_IAM` gate
and `add_subdirectory(services/iam)`.
