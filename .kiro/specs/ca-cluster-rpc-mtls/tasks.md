# Implementation Plan — CA Cluster RPC mTLS

## Status: Not Started

**Last Updated**: July 9, 2026

## Overview

Secure `ca_cluster_node`'s Raft-internal RPC channel (today plain TCP via
`tcp_rpc_client`/`tcp_rpc_server`) with mutual TLS, bootstrapped by a static,
operator-provisioned shared credential and automatically cut over to the
cluster's own CA root once it exists — so the static credential is exercised
only for the traffic that necessarily predates the CA it helps create.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "tls_rpc_trust_policy + tls_tcp_rpc_config — no dependents yet, foundational data model"
    },
    {
      "wave": 2,
      "tasks": [2],
      "description": "tls_tcp_rpc_client/server built against wave 1's config/policy types"
    },
    {
      "wave": 3,
      "tasks": [3, 4],
      "description": "ca_state_machine's record_rpc_tls_ready command (independent of the transport) and ca_cluster_node config/CLI wiring for the new flags (independent of each other)"
    },
    {
      "wave": 4,
      "tasks": [5],
      "description": "Swap ca_cluster_raft_types' network_client_type/network_server_type to wave 2's types, gated on wave 4's config flags"
    },
    {
      "wave": 5,
      "tasks": [6, 7, 8],
      "description": "The three maintenance-thread behaviors (acquire identity, finalize cutover, renew) — each depends on task 3 (state) and task 5 (transport reload hooks); independent of each other"
    },
    {
      "wave": 6,
      "tasks": [9],
      "description": "Deployment doc/packaging updates — depends on the final CLI surface from task 4"
    },
    {
      "wave": 7,
      "tasks": [10, 11, 12, 13],
      "description": "Test suite — depends on all implementation waves; the four test files are independent of each other"
    }
  ]
}
```

## Tasks

## Phase 1: Trust Policy and Transport (Tasks 1-2)

- [ ] 1. Add `tls_rpc_trust_policy` and `tls_tcp_rpc_config`
  - New `include/raft/tls_tcp_rpc.hpp`. `tls_rpc_trust_policy` with
    `bootstrap_fingerprint_hex`/`ca_root_pem` optionals and `accepts(X509*)`;
    factory helpers `pinned_fingerprint(hex)`, `ca_root_only(pem)`,
    `either(hex, pem)` matching the design's three named states.
  - `accepts()` for the fingerprint case: SHA-256 over the presented cert's
    DER encoding, hex-compared — reuse the existing fingerprint helper
    already implemented for `ca_bootstrap_client.hpp`
    (`sha256_fingerprint_hex_bare`) rather than reimplementing it.
  - `accepts()` for the chain case: `X509_STORE` built from `ca_root_pem`,
    ordinary `X509_verify_cert`.
  - _Requirements: 1.3, 2.2_

- [ ] 2. Add `tls_tcp_rpc_client` / `tls_tcp_rpc_server`
  - Same public surface as `tcp_rpc_client`/`tcp_rpc_server`
    (`send_request_vote`/`send_append_entries`/`send_install_snapshot`;
    `register_*_handler`/`start`/`stop`/`is_running`), `static_assert`ed
    against `network_client`/`network_server`.
  - Reuse `tcp_detail::connect_to`/accept-loop shape from `tcp_rpc.hpp`
    unmodified (do not touch that file — import what's needed, or
    duplicate the ~10-line socket setup if the existing helpers aren't
    reusable across headers without exposing them publicly — prefer
    reuse).
  - Wrap the accepted/connected fd in an `SSL*` from a per-instance
    `SSL_CTX*`; `SSL_CTX_set_verify` callback delegates to
    `tls_rpc_trust_policy::accepts()`.
  - `reload_identity(cert_path, key_path)` / `reload_trust_policy(policy)`
    on both classes, applied to the live `SSL_CTX*` without a listener
    restart (client: applied to the config used by the *next* call).
  - _Requirements: 1.1, 1.2, 1.4, 1.5_

## Phase 2: Replicated Cutover State and Config Wiring (Tasks 3-4)

- [ ] 3. Add `ca_command_type::record_rpc_tls_ready`
  - `ca_state_machine.hpp`: new enumerator, `encode_record_rpc_tls_ready_command(node_id)`
    following the existing `encode_*_command` shape, `apply()` case
    inserting into an idempotent `std::set<std::uint64_t> _rpc_tls_ready`,
    `rpc_tls_ready_node_ids()` accessor, folded into
    `get_state()`/`restore_from_snapshot()`.
  - _Requirements: 4.1, 4.2, 4.3_

- [ ] 4. `ca_cluster_node_config` / CLI flags for RPC TLS
  - `config.hpp`: `rpc_tls_cert_path`/`rpc_tls_key_path` fields;
    `--rpc-tls-cert`/`--rpc-tls-key` argument parsing with the same
    both-or-neither validation as `--tls-cert`/`--tls-key`.
  - _Requirements: 3.1_

## Phase 3: Wire the Transport into `ca_cluster_node` (Task 5)

- [ ] 5. Swap `ca_cluster_raft_types`'s network types when RPC TLS is enabled
  - `main.cpp`: when `--rpc-tls-cert`/`--rpc-tls-key` are given, construct
    `tls_tcp_rpc_client`/`tls_tcp_rpc_server` (initial trust policy:
    `pinned_fingerprint` of the configured bootstrap credential) instead of
    plain `tcp_rpc_client`/`tcp_rpc_server`; otherwise fall back to today's
    plain-TCP construction with the existing "running without TLS" warning,
    now phrased to cover the RPC channel too.
  - Since `network_client_type`/`network_server_type` are compile-time type
    aliases (not a runtime choice) in the current `tcp_raft_types`/
    `ca_cluster_raft_types` design, this task resolves the "TLS or not" flag
    to a template parameter (e.g. a small `ca_cluster_raft_types<UseTls>`
    or two sibling `Types` structs selected by a runtime `if`/factory at
    startup) — see design.md's note that `node<Types>` never needs to know
    which; pick whichever of the two shapes keeps `main.cpp`'s existing
    control flow simplest, since both are equally valid under the
    `network_client`/`network_server` concept.
  - _Requirements: 3.2, 3.3_

## Phase 4: Maintenance-Thread Behaviors (Tasks 6-8)

- [ ] 6. `maybe_acquire_rpc_identity()`
  - New maintenance-thread closure: checks for an already-persisted, valid
    peer certificate under `--data-dir` first (no-op if present); otherwise,
    once `read_ca_state()` shows root material exists, generates a key/CSR
    (`generate_key_and_csr`), obtains a signed certificate (in-process
    `sign_csr()` if leader, else an HTTPS call to the leader's client-facing
    `/v1/certificates` with the configured bearer token), persists it under
    `--data-dir`, calls `reload_identity()` and `reload_trust_policy(either(...))`
    on both the local `tls_tcp_rpc_client` and `tls_tcp_rpc_server`
    instances, then submits `record_rpc_tls_ready(self)`.
  - Retried on failure at the existing 200ms maintenance-tick cadence; never
    blocks the election/heartbeat timer threads.
  - _Requirements: 5.1, 5.2, 5.3, 5.4, 7.1_

- [ ] 7. `maybe_finalize_rpc_tls_cutover()`
  - New maintenance-thread closure: once `rpc_tls_ready_node_ids()` (via
    `read_ca_state()`) is a superset of `cfg.all_node_ids()`, calls
    `reload_trust_policy(ca_root_only(...))` on both local transport
    instances and latches `cutover_finalized` so this only happens once.
  - _Requirements: 6.1, 6.2_

- [ ] 8. `maybe_renew_rpc_identity()`
  - New maintenance-thread closure: when the persisted peer certificate is
    within its renewal window, calls `POST /v1/certificates/renew`
    (mTLS-authenticated with the current peer certificate, matching
    `ca_test_fixture::renew()`'s existing in-process equivalent), persists
    the result, and hot-reloads it via `reload_identity()`.
  - _Requirements: 7.2, 7.3_

## Phase 5: Deployment Packaging (Task 9)

- [ ] 9. Update deployment docs/packaging for the bootstrap credential
  - `docker/ca_cluster_node/ca_cluster_node.env.example`: new
    `CA_CLUSTER_RPC_TLS_CERT`/`_KEY` entries alongside the unseal-key
    guidance, and the one-line `openssl req -x509 ...` generation command
    in a comment.
  - `docker/ca_cluster_node/ca_cluster_node.service`: pass the new flags via
    `CA_CLUSTER_NODE_EXTRA_ARGS` or promote them to first-class
    `ExecStart` arguments (match whichever convention the existing
    `--tls-cert`/`--tls-key` client-facing flags use there today).
  - `docker/ca_cluster_node/ecs-task-definitions/*.json`: equivalent
    Secrets Manager-sourced entries.
  - `docker/ca_cluster_node/README.md`: new section documenting the
    two-phase bootstrap and pointing at this spec for the underlying
    design; explicit confirmation that Path 3 (`aws_ec2_quorum_manager`)
    needs no code change.
  - _Requirements: 3.4, 3.5_

## Phase 6: Tests (Tasks 10-13)

- [ ] 10. `tests/tls_tcp_rpc_unit_test.cpp` (new file)
  - `tls_rpc_trust_policy::accepts()` under all three policy shapes;
    fingerprint match/mismatch; chain verifies/doesn't; `either` accepts
    what either alone would.
  - _Requirements: 1.2, 2.2_

- [ ] 11. `tests/tls_tcp_rpc_integration_test.cpp` (new file)
  - Real sockets, real OpenSSL: 2-node `tls_tcp_rpc_client`/`server` pair,
    one real mutual-TLS handshake + RPC round trip per trust policy; a
    missing/untrusted client cert rejected.
  - _Requirements: 1.1, 1.2, 1.4_

- [ ] 12. `tests/ca_cluster_node_rpc_tls_test.cpp` (new file, multi-process)
  - Extends the existing `ca_cluster_node` multi-process test pattern: 3
    real processes started with only the bootstrap credential; confirms
    election/replication over TLS, `bootstrap_ca` committing, all three
    reaching `rpc_tls_ready`, cutover finalizing, and the cluster still
    issuing certificates correctly after the bootstrap credential file is
    deleted post-cutover.
  - Staggered-finalization sub-case (Property 3 / Requirement 6.3): delay
    one node's maintenance tick artificially and confirm no connectivity
    gap opens.
  - _Requirements: 5.1-5.4, 6.1-6.3_

- [ ] 13. `tests/ca_cluster_node_rpc_tls_restart_test.cpp` (new file, multi-process)
  - Cutover a 3-node cluster, delete the bootstrap credential, restart one
    node without it configured, confirm it rejoins using its persisted
    peer certificate (Requirement 7.1); separately, force a peer
    certificate close to expiry and confirm `maybe_renew_rpc_identity()`
    renews it without a restart (Requirement 7.2).
  - _Requirements: 7.1, 7.2, 7.3_

## Notes

- No new external dependency: OpenSSL is already a hard dependency of the
  `certificate_authority`/`ca_cluster_node` build (`KYTHIRA_HAS_OPENSSL`,
  certificate-authority spec Requirement 8.1). This spec introduces no new
  `vcpkg.json` entries.
- `tcp_rpc.hpp`/`tcp_raft_types.hpp` are read-only references throughout —
  no task in this plan edits either file (design.md Property 1).
