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
| `include/kythira/iam/iam_conditions.hpp` | condition operators, policy variables, the request context and the global condition keys | 11 |
| `include/kythira/iam/iam_evaluator.hpp` | `evaluate()`, the one place an allow or deny is decided | 12 |
| `tests/iam_evaluator_test.cpp` | one case per evaluation invariant (the corpus holds the rest) | 12 |
| `include/kythira/iam/iam_service_registry.hpp` | the actions each served AWS API declares; an action no table names is denied | 13 |
| `include/kythira/iam/services/iam_actions.hpp`, `sts_actions.hpp`, `elasticache_actions.hpp` | the `iam`, `sts` and `elasticache` action tables, from the Service Authorization Reference | 13 |
| `include/kythira/iam/iam_authorizer.hpp` | `iam_authorizer` (SigV4 authentication, policy gathering, `authorize`) and the asynchronous `audit_log` | 13 |
| `tests/iam_authorizer_test.cpp` | authentication, authorization, partition, staleness and audit-line cases | 13 |
| `include/kythira/iam/iam_replica.hpp` | `iam_replica<Types>`, a learner of the IAM group a front end embeds, and `replica_freshness` (lag, the metric, the fail-closed bound) | 13 |
| `tests/iam_replica_test.cpp` | an embedded replica over the network simulator: 5 s propagation, failing closed when cut off | 13 |
| `include/kythira/iam/iam_query_api.hpp` | the IAM Query API: user, group, access-key, account-alias, role, managed-policy, attachment, inline-policy, boundary and instance-profile actions, OIDC providers, policy simulation and the account calls, each authorized as `iam:<Action>` | 14-16 |
| `tests/iam_query_api_test.cpp` | validation, authorization, error codes and pagination of those actions | 14-16 |
| `include/kythira/iam/iam_sts_token.hpp` | the sealed STS session token (CBOR payload, AES-256-GCM, `kid`) and the STS key ring | 17 |
| `include/kythira/iam/iam_sts_api.hpp` | the STS Query API: AssumeRole (with session policies, session tags and source identity), AssumeRoleWithWebIdentity, GetSessionToken, GetFederationToken, GetCallerIdentity, GetAccessKeyInfo and DecodeAuthorizationMessage | 17-19, 26 |
| `include/kythira/iam/iam_oidc_jwks.hpp` | the JWKS cache for web identity tokens, its HTTPS fetcher with the allowlist and private-address refusal, and the direct JWKS URL for a local Kythira issuer | 19-20 |
| `tests/iam_sts_test.cpp` | those actions, cross-account trust, `ExternalId`, key rotation, expiry, forged tokens, session policies and tags, session revocation, web identity tokens, the JWKS fetch rules, the `kythira:*` claim keys, the certificate exchange, federated sessions, encoded authorization messages and the temporary-credential limits | 17-20, 26 |
| `include/kythira/iam/iam_redis_verifier.hpp` | the Redis gateway's `credential_verifier`: ElastiCache IAM auth tokens, `elasticache:Connect` on the user and replication group, a per-access-key failure budget | 21, 22 |
| `include/kythira/iam/iam_embedded_replica.hpp` | a front end's IAM side in one object: the replica over mutual-TLS RPC, the unsealing key, the registry and the authorizer | 21 |
| `tests/iam_redis_verifier_test.cpp`, `tests/iam_redis_token_test_support.hpp`, `tests/tools/gen_redis_iam_token.py` | tokens from botocore (a fixed vector the script regenerates) and presigned in the test | 21 |
| `tests/iam_embedded_replica_test.cpp` | three `iam_node` processes and an embedded replica in process checking tokens as policies change | 21 |
| `tests/iam_kythira_issuer_test.cpp` | a ca_cluster_node and `iam_node` as processes: a Kythira workload token and certificate exchanged for role credentials (`--kythira-issuer`, `--kythira-issuer-local`, `POST /v1/iam/credentials`) | 20 |
| `include/kythira/iam/iam_state_machine.hpp` | the IAM Raft group's state machine, its command set, and the leader-side stamping helpers | 8 |
| `include/kythira/iam/iam_seal.hpp` | KEK derivation and AES-256-GCM sealing of access-key and STS-key secrets | 8 |
| `tests/iam_state_machine_test.cpp` | their unit and property tests | 8 |
| `include/kythira/iam/iam_admin.hpp` | the `/v1/admin/*` operations (create and list accounts, rotate the STS key, register a replica) | 9 |
| `include/kythira/iam/iam_routing.hpp` | request routing by credential-scope service, and Query error XML | 9 |
| `cmd/iam_node/` | the daemon: the IAM Raft group over mutual-TLS RPC and the HTTPS listener | 9 |
| `cmd/kythira-iam/` | the admin CLI | 9 |
| `docker/iam_node/` | the image the compose scenario runs | 9 |
| `tests/iam_admin_test.cpp`, `tests/iam_node_test.cpp`, `tests/iam_node_test_support.hpp` | the admin operations, and the binaries as a three-node cluster with a replica (the helpers are shared with `iam_kythira_issuer_test`) | 9 |
| `tests/iam_test_pki.hpp`, `tests/iam_test_certs.cpp` | the throwaway test PKI, in process and as a tool | 9 |
| `include/kythira/iam/iam_validation.hpp` | AWS's IAM and STS request-parameter rules (names, tags, durations, session policies and tags, what each credential may call), for the Query and STS handlers | invariants |
| `tests/iam_validation_test.cpp` | one case per invariant group in `.kiro/specs/iam-compatible-auth/invariants.md` | invariants |
| `tests/docker/` | the compose scenario (three voters and a replica, by service name) | 9 |
| `tests/docker/conformance.yml`, `tests/docker/conformance/`, `tests/docker/iam_conformance_test.cpp` | the conformance job: the voters, the Kythira CA and two IAM-enabled Redis gateways, driven by pinned boto3 (`driver.py`) and the AWS CLI v2 | 23 |
| `tests/tools/simulate_corpus_on_aws.py` | sends every corpus `evaluate` case SimulateCustomPolicy can express to real AWS and writes each disagreement as a corpus case (`corpus/evaluate/aws_simulator.json`), on Clark's go only | 24 |
| `tests/docker/conformance/terraform/` | the Terraform configuration the conformance job applies, re-plans and destroys (Terraform and the AWS provider pinned) | 24 |
| `docker/conformance/` | the conformance stack's image (`iam_node`, `kythira-iam`, `iam_test_certs`, `ca_cluster_node`, `redis_gateway_node`) | 23 |
| `tools/` | the pinned AWS-managed policies (`aws_managed_policies.json`), the configure-time generator for `<kythira/iam/aws_managed_policies_data.hpp>`, and the script that refreshes the JSON from AWS | 8 |

The authorizer (task 13) and the rest follow the plan in the spec's
`tasks.md`.

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
| `kythira_test_support` | `test_ports.hpp` for `iam_node_test`, and `ca_cluster_node_harness.hpp` for `iam_kythira_issuer_test` |
| `ca_cluster_node`, `httplib::httplib` (tests) | `iam_kythira_issuer_test` runs the CA cluster binary and talks to it |
| `add_docker_chaos_scenario_test`, `kythira_detect_container_runtime`, `KYTHIRA_IMAGE_CONTEXT` | the compose scenario and its image (`cmake/kythira_test_helpers.cmake`) |
| `KYTHIRA_BUILD_IAM` | set by the top level from `CONFIG_IAM`, OpenSSL and Boost |

The top level names the module in two places: the `KYTHIRA_BUILD_IAM` gate
and `add_subdirectory(services/iam)`.
