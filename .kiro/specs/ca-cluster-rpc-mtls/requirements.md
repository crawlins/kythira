# CA Cluster RPC mTLS Requirements Document

## Introduction

`ca_cluster_node` (`.kiro/specs/certificate-authority/`, Requirement 17)
replicates a CA's root material, issuance ledger, and revocation list across a
Kythira Raft cluster. Its client-facing HTTP API (`/v1/certificates`, etc.) is
already TLS-capable and, per that spec's Requirement 19, bootstraps
first-contact trust via fingerprint pinning. Its Raft-internal RPC channel
between cluster peers, however, is plain, unauthenticated, unencrypted TCP
(`kythira::tcp_rpc_client`/`tcp_rpc_server`, `include/raft/tcp_rpc.hpp`) —
security today depends entirely on network-level isolation (VPC subnets/
security groups scoping the RPC port to just the three cluster peers).

This document specifies securing that RPC channel with mutual TLS, and —
because the cluster's own CA root does not exist until the first
`--bootstrap-ca`-flagged node's proposal commits via the very Raft traffic
being secured — a two-phase bootstrap: peers first mutually authenticate using
a small, static, out-of-band credential (mirroring how the unseal passphrase
and bearer token are already distributed), then, once the CA root is
established, each node obtains its own leaf certificate from the now-running
cluster and switches the RPC channel to trust that root instead. The static
bootstrap credential's trust is then dropped clusterwide for new connections,
so it is exercised only for the traffic that necessarily occurs before the CA
exists to secure itself.

This spec extends the certificate-authority framework's Requirement 17
(`ca_cluster_node`) and reuses its Requirement 9 (CSR-based signing),
Requirement 15 (renewal), Requirement 16 (hot-reload), and Requirement 19
(fingerprint pinning) primitives. It does not modify `tcp_rpc.hpp` itself —
`chaos_node`, `dns_discovery_node`, and other existing Raft binaries with no
CA dependency continue to build and run against it completely unchanged — and
it does not change the Raft consensus algorithm.

## Glossary

- **Cluster bootstrap credential**: a single self-signed X.509
  certificate/key pair, generated once by the operator and copied
  byte-identical to every `ca_cluster_node` instance — analogous to the
  existing unseal passphrase and bearer token. Its only job is proving "you
  hold the shared secret distributed to legitimate cluster members," not
  distinguishing which peer is which (that's already established by the
  static `--peers` config's node-id↔host:port mapping).
- **Cutover**: the point at which a node stops accepting the bootstrap
  credential's fingerprint as valid for *new* RPC connections, relying
  solely on the CA's own root for peer authentication going forward.
- **Dual-trust window**: the period, after a node has obtained its own
  CA-issued peer certificate but before every configured peer has confirmed
  cutover-readiness, during which that node's RPC transport accepts either
  credential as fully trusted.
- **Peer certificate**: a leaf certificate, issued by the cluster's own CA
  once bootstrapped, that a node presents on its RPC transport — both as TLS
  server accepting peers' connections and as TLS client dialing peers —
  after obtaining it.
- **`tls_tcp_rpc_client`/`tls_tcp_rpc_server`**: new classes, satisfying the
  existing `kythira::network_client`/`network_server` concepts, wrapping
  `tcp_rpc.hpp`'s wire framing in mutually-authenticated TLS.

## Requirements

### Requirement 1: TLS-wrapped Raft RPC transport

**User Story:** As an operator running `ca_cluster_node` outside a fully
trusted network boundary, I want Raft consensus traffic between cluster
peers to be mutually authenticated and encrypted, so that the CA's ledger and
key material are not exposed to anyone who can merely reach the RPC port,
independent of whatever network-level isolation also happens to be in place.

#### Acceptance Criteria

1. `tls_tcp_rpc_client` and `tls_tcp_rpc_server` SHALL be provided in a new
   `include/raft/tls_tcp_rpc.hpp`, satisfying `kythira::network_client`/
   `network_server` (verified by `static_assert`), reusing `tcp_rpc.hpp`'s
   wire-framing protocol (`[4-byte big-endian length][JSON payload]`) and
   `json_rpc_serializer`, but transporting frames over an OpenSSL `SSL*` per
   connection instead of a raw file descriptor.
2. Both classes SHALL require mutual authentication: `tls_tcp_rpc_server`
   SHALL set `SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT` on accept;
   `tls_tcp_rpc_client` SHALL present its own configured certificate on
   connect and SHALL verify the accepted peer's certificate before
   completing the handshake. A connection whose peer presents no
   certificate, or one that fails verification under the currently active
   trust policy (Requirement 6), SHALL be rejected before any RPC payload is
   exchanged.
3. `tls_tcp_rpc_config` SHALL carry `cert_path`/`key_path` (this node's
   currently presented identity) and a trust-policy value that can be
   updated at runtime without reconstructing the client/server (Requirement
   6.2's finalization step reconfigures a live instance, not a fresh one).
4. `tcp_rpc.hpp`, `tcp_rpc_client`, and `tcp_rpc_server` SHALL remain
   byte-for-byte unmodified by this spec.
5. Existing OpenSSL helpers already used elsewhere in the project for
   certificate loading and chain verification (`raft::testing` helpers in
   `include/raft/ca_http_helpers.hpp`) SHALL be reused rather than
   duplicated.

### Requirement 2: Cluster bootstrap credential

**User Story:** As an operator standing up a fresh 3-node `ca_cluster_node`
cluster, I want a simple, static credential the nodes can use to mutually
authenticate before any of them has a certificate from the CA they are about
to create, so the chicken-and-egg problem of "the traffic that creates the
root can't yet be secured by that root" has a documented, minimal-machinery
answer.

#### Acceptance Criteria

1. An operator-generated, self-signed X.509 certificate/key pair (the
   cluster bootstrap credential) SHALL be the initial trust material for
   every node's RPC transport, distributed byte-identical to all nodes via
   the same out-of-band channel already used for the unseal passphrase
   (certificate-authority spec Requirement 17.4) — e.g. a file installed
   alongside `/etc/ca_cluster_node/unseal.key`.
2. Because the credential is self-signed and identical everywhere,
   verifying a peer's presented certificate under this trust policy SHALL
   reduce to comparing its SHA-256 fingerprint against the locally
   configured credential's own fingerprint — no certificate chain
   construction is required for this trust policy.
3. `ca_cluster_node` SHALL fail to start if RPC TLS is enabled (Requirement
   3) and neither a bootstrap credential nor an already-obtained peer
   certificate (Requirement 7) is available — fail closed, matching the
   existing `--auth-token`/`--unseal-key-file` requirement pattern.
4. Generating this credential SHALL require no new tooling — a documented
   one-line `openssl req -x509 -newkey ec ...` invocation is sufficient; no
   code in this spec generates it.

### Requirement 3: `ca_cluster_node` config and deployment wiring

**User Story:** As an operator, I want to enable RPC TLS with the same kind
of flags and environment-file conventions `ca_cluster_node` already uses for
its client-facing TLS listener, so adopting this feature doesn't require
learning a new configuration shape.

#### Acceptance Criteria

1. `ca_cluster_node_config` SHALL gain `rpc_tls_cert_path`/`rpc_tls_key_path`
   fields (initially pointing at the bootstrap credential); `--rpc-tls-cert`/
   `--rpc-tls-key` CLI flags SHALL set them, following the existing
   `--tls-cert`/`--tls-key` paired-argument validation (both or neither).
2. `ca_cluster_raft_types` (`cmd/ca_cluster_node/main.cpp`) SHALL set
   `network_client_type = tls_tcp_rpc_client` and
   `network_server_type = tls_tcp_rpc_server` when RPC TLS is enabled,
   replacing the plain-TCP types it inherits today from
   `kythira::tcp_raft_types`.
3. WHEN `--rpc-tls-cert`/`--rpc-tls-key` are omitted, `ca_cluster_node` SHALL
   fall back to plain-TCP `tcp_rpc_client`/`tcp_rpc_server` and print the
   same "WARNING: running without TLS ... suitable only for a private
   network" style message already used for the client-facing HTTP listener
   — this spec makes RPC TLS available and documented as the recommended
   default, not mandatory, matching how the client-facing listener's own TLS
   is optional today.
4. `docker/ca_cluster_node/ca_cluster_node.env.example`,
   `ca_cluster_node.service`, and `ecs-task-definitions/*.json` SHALL be
   updated to show the bootstrap credential provisioned alongside
   `unseal.key` and to pass the new flags — no new provisioning mechanism
   (Secrets Manager entry, EFS path, etc.) beyond what is already used for
   the unseal key SHALL be introduced.
5. The `aws_ec2_quorum_manager`-based automated path
   (`docker/ca_cluster_node/README.md`'s "Path 3") SHALL require no code
   change — the bootstrap credential is baked into the AMI exactly like the
   unseal key already is, consistent with that path's existing "no
   CA-specific EC2 provisioning mechanism" design.

### Requirement 4: `ca_state_machine` cutover tracking

**User Story:** As a `ca_cluster_node` instance, I want to know — via the
same replicated state every other cluster fact already lives in — whether
every configured peer has already switched to presenting a CA-issued RPC
certificate, so I can safely stop accepting the bootstrap credential without
a separate coordination mechanism.

#### Acceptance Criteria

1. A new `ca_command_type::record_rpc_tls_ready` command SHALL be added,
   carrying a single `node_id`. `apply()` SHALL record that node id in an
   `rpc_tls_ready` set; committing the same node id twice SHALL be
   idempotent (no error, no duplicate entry).
2. `ca_state_machine` SHALL expose `rpc_tls_ready_node_ids()`, returning the
   current set, (de)serialized by `get_state()`/`restore_from_snapshot()`
   alongside the existing ledger and bootstrap material — a newly joined or
   restarted node obtains this fact via the same snapshot-installation path
   as everything else, with no CA-specific transfer mechanism.
3. `encode_record_rpc_tls_ready_command(node_id)` SHALL follow the existing
   boost::json command-encoding convention used by
   `encode_bootstrap_ca_command`/`encode_record_issuance_command`.

### Requirement 5: Self-service peer certificate acquisition

**User Story:** As a `ca_cluster_node` instance, once my cluster's CA root
exists, I want to obtain my own certificate from it without any operator
action, so the transition off the static bootstrap credential is automatic
rather than a manual per-node step.

#### Acceptance Criteria

1. Once a node observes (via `read_state()`, the same linearizable-read path
   `ensure_signer()`/`maybe_bootstrap()` already use) that CA root material
   exists, it SHALL — if it does not already hold a locally persisted,
   still-valid CA-issued peer certificate (Requirement 7) — generate a key
   pair and CSR (`generate_key_and_csr`) for its own node identity and
   obtain a signed certificate via the cluster's own `/v1/certificates`
   route: directly, in-process, if this node is currently leader; otherwise
   via an HTTPS call to the leader's client-facing address (the same
   redirect-target resolution already used for ordinary client requests),
   authenticated with the same bearer token the node was configured with.
2. The obtained peer certificate's subject/SAN SHALL identify the node
   unambiguously (e.g. common name `ca-cluster-node-<node_id>`) — this
   SHALL be sufficient for Requirement 1.2's mutual verification and is not
   intended for any external client-facing use.
3. Once obtained, the node SHALL hot-reload its RPC transport
   (`tls_tcp_rpc_client`/`tls_tcp_rpc_server`, Requirement 1.3) to
   additionally present this certificate and to additionally trust peer
   certificates chaining to the CA root — without dropping its existing
   bootstrap-credential trust yet (Requirement 6's dual-trust window) — and
   SHALL then submit a `record_rpc_tls_ready` command (Requirement 4) for
   its own node id.
4. Failure to obtain a certificate (leader unreachable, no quorum, bearer
   token mismatch) SHALL be retried on the same maintenance-thread tick
   cadence already used for `maybe_bootstrap()`/`ensure_signer()`, and SHALL
   NOT block Raft consensus itself — a node still using only the bootstrap
   credential continues to participate normally in elections and
   replication throughout.

### Requirement 6: Dual-trust window and safe cutover finalization

**User Story:** As an operator, I want the static bootstrap credential to
stop being usable for new connections once it is no longer needed, without
any node risking a self-inflicted partition from a peer that hasn't yet
caught up, so "the initial traffic is the only traffic on the static
credential" holds in practice, not just in the common case.

#### Acceptance Criteria

1. From the moment a node has obtained its own peer certificate (Requirement
   5) until cutover finalizes (this requirement), its RPC transport SHALL
   accept a peer connection authenticated by EITHER the bootstrap
   credential's pinned fingerprint OR a certificate chaining to the CA root
   — both SHALL be treated as fully trusted, not one as merely provisional.
2. Each node SHALL, on the same maintenance-thread tick, check whether
   `rpc_tls_ready_node_ids()` (Requirement 4.2) contains every node id in
   the cluster's configured membership. WHEN it does, the node SHALL
   reconfigure its RPC transport's trust policy (Requirement 1.3) to accept
   only certificates chaining to the CA root, ceasing to accept the
   bootstrap credential's fingerprint for any NEW connection —
   already-established connections SHALL NOT be forcibly dropped.
3. Finalization (6.2) SHALL be safe without any cluster-wide coordination
   beyond the replicated `rpc_tls_ready` set itself: because a node only
   commits its own `record_rpc_tls_ready` entry after it has already
   switched to presenting a CA-issued certificate (Requirement 5.3), by the
   time any node observes the full set, every peer is already independently
   reachable via CA-chain trust — so no node that finalizes early can strand
   a peer that finalizes later. This property SHALL be covered by a test
   that finalizes nodes in a deliberately staggered order and confirms the
   cluster remains fully connected throughout.
4. WHEN a bootstrap credential is later needed again (e.g. onboarding a
   wholly new node before it has any CA-issued certificate of its own),
   operators MAY continue using it — this requirement governs an individual
   already-cutover node's own trust policy, not a cluster-wide, irreversible
   revocation of the credential's validity elsewhere. A dedicated
   revocation mechanism is explicitly out of scope for this spec.

### Requirement 7: Persisted peer identity and renewal

**User Story:** As an operator restarting an already-cutover
`ca_cluster_node` instance, I want it to rejoin using its previously
obtained certificate rather than requiring the bootstrap credential again,
and I want that certificate to renew itself before expiry, so long-term
operation needs no recurring manual step.

#### Acceptance Criteria

1. A node's obtained peer certificate/key (Requirement 5) SHALL be
   persisted under its `--data-dir`, alongside the existing Raft log/
   snapshot storage, so a restart does not require re-requesting one from
   the (possibly still-electing) leader, and — combined with Requirement
   6.4 — does not require the bootstrap credential to still be present for
   an already-cutover node to rejoin after a restart.
2. Before expiry, a node SHALL renew its peer certificate via the existing
   `POST /v1/certificates/renew` mTLS-authenticated route
   (certificate-authority spec Requirement 15), presenting its current peer
   certificate as proof of identity, and SHALL hot-reload the result into
   its running RPC transport (reusing the `reload_tls_material()`/
   `enable_auto_reload()` pattern already established for
   `cpp_httplib_server`/`coap_server`, certificate-authority spec
   Requirement 16) without a restart.
3. A renewal failure (leader unreachable, certificate already expired with
   no grace period remaining) SHALL be logged and retried on the existing
   maintenance-thread cadence; it SHALL NOT crash the node or forcibly close
   already-established RPC connections still authenticated under the
   not-yet-expired previous certificate.
