# CA Cluster RPC mTLS Design Document

## Overview

Today, `ca_cluster_node` peers exchange Raft RequestVote/AppendEntries/
InstallSnapshot RPCs over `kythira::tcp_rpc_client`/`tcp_rpc_server`
(`include/raft/tcp_rpc.hpp`) — plain TCP, no authentication, no encryption.
Its client-facing HTTP API is TLS-capable and, via `ca_bootstrap_client.hpp`,
already solves an analogous bootstrap-trust problem for *external* callers
using fingerprint pinning. This design applies the same idea one layer
deeper: to the traffic the cluster uses to create its own root of trust in
the first place.

The core difficulty is ordering. The CA's root certificate is Raft-replicated
state — it doesn't exist until the `bootstrap_ca` command commits, and that
command can't commit without RPC traffic between peers. So the RPC channel
needs *some* credential before the CA exists, and a separate mechanism to
switch to the CA's own root afterward. This design uses a small,
operator-provisioned, self-signed shared credential for the "before" phase
(literally the same trust model already used for the unseal passphrase and
bearer token — a byte-identical secret distributed out of band), then a
self-service, fully automatic cutover once the CA exists, coordinated purely
through the state the cluster already replicates.

## Architecture

```
Phase 0 — before bootstrap_ca commits
  Node A ──TLS (bootstrap credential, both directions)── Node B
  Node A ──TLS (bootstrap credential, both directions)── Node C
  (RequestVote/AppendEntries/InstallSnapshot traffic; this is the traffic
   that eventually carries the bootstrap_ca command itself)

Phase 1 — root exists, some nodes still mid-transition (dual-trust window)
  Node A (has peer cert)     ──TLS (peer cert OR bootstrap cred)── Node B (bootstrap cred only)
  Node A (has peer cert)     ──TLS (peer cert)──────────────────── Node C (has peer cert)
  Each node independently: sees root exists → requests own peer cert from
  leader's /v1/certificates → hot-reloads RPC transport to present it and
  accept it → submits record_rpc_tls_ready(self)

Phase 2 — cutover finalized (every node observed the full ready set)
  Node A ──TLS (peer cert only, chains to CA root)── Node B
  Node A ──TLS (peer cert only, chains to CA root)── Node C
  Bootstrap credential's fingerprint no longer accepted for new connections
  by any node; each node's own peer cert persisted under --data-dir and
  self-renews via POST /v1/certificates/renew before expiry.
```

```
include/raft/tls_tcp_rpc.hpp                  (new)
  │
  ├── tls_rpc_trust_policy ──────────► { pinned_fingerprint(hash) | ca_root(pem) | either(hash, pem) }
  │                                     Requirement 6's dual-trust window is
  │                                     represented as `either`, not a
  │                                     separate code path.
  │
  ├── tls_tcp_rpc_config ────────────► cert_path, key_path, trust_policy
  │
  ├── tls_tcp_rpc_client ────────────► satisfies network_client
  │     - connect_to() + SSL_connect(), present cert_path/key_path,
  │       verify peer under current trust_policy
  │     - reuses tcp_detail::frame_send/frame_recv shape, over SSL_read/write
  │
  └── tls_tcp_rpc_server ────────────► satisfies network_server
        - accept() + SSL_accept(), SSL_VERIFY_PEER|FAIL_IF_NO_PEER_CERT
        - reload_trust_policy(new_policy) / reload_identity(cert,key) —
          applied to the live SSL_CTX*, no listener restart (mirrors
          cpp_httplib_server::reload_tls_material())

include/raft/ca_state_machine.hpp             (extended)
  - ca_command_type::record_rpc_tls_ready(node_id)
  - rpc_tls_ready set, folded into get_state()/restore_from_snapshot()

cmd/ca_cluster_node/main.cpp                  (extended)
  - ca_cluster_raft_types::network_client_type/network_server_type
    → tls_tcp_rpc_client/tls_tcp_rpc_server (when RPC TLS enabled)
  - maintenance_thread gains two more per-tick checks, alongside the
    existing maybe_bootstrap()/ensure_signer():
      maybe_acquire_rpc_identity()   — Requirement 5
      maybe_finalize_rpc_tls_cutover() — Requirement 6
      maybe_renew_rpc_identity()     — Requirement 7
```

`tcp_rpc.hpp` is not touched. `tls_tcp_rpc.hpp` is a sibling header reusing
its wire-framing constants and `json_rpc_serializer`, not a fork of its
logic — the only thing that changes is what carries the framed bytes (a raw
fd vs. an `SSL*`).

## Components and Interfaces

### 1. `tls_rpc_trust_policy` and `tls_tcp_rpc_config`

```cpp
namespace kythira {

// Requirement 6.1/6.2: represents the dual-trust window as a first-class
// state rather than a boolean flag plus two code paths — verification logic
// only ever asks "does this policy accept fingerprint F / chain-to-root C",
// so adding/removing an accepted credential is one call site.
struct tls_rpc_trust_policy {
    std::optional<std::string> bootstrap_fingerprint_hex;  // Requirement 2.2
    std::optional<std::string> ca_root_pem;                // Requirement 6.2

    [[nodiscard]] auto accepts(X509* presented) const -> bool;
};

struct tls_tcp_rpc_config {
    std::string cert_path;
    std::string key_path;
    tls_rpc_trust_policy trust_policy;
};

}  // namespace kythira
```

`accepts()` is the single place both bootstrap-fingerprint comparison
(Requirement 2.2 — no chain construction, just a SHA-256 comparison against
the presented leaf) and normal `X509_verify_cert` chain verification against
`ca_root_pem` are implemented; `tls_tcp_rpc_client`/`server` never inspect a
certificate directly.

### 2. `tls_tcp_rpc_client` / `tls_tcp_rpc_server`

Same public surface as `tcp_rpc_client`/`tcp_rpc_server`
(`send_request_vote`/`send_append_entries`/`send_install_snapshot`;
`register_*_handler`/`start`/`stop`/`is_running`), so `ca_cluster_raft_types`
only needs a `using network_client_type = ...` / `using network_server_type
= ...` change — nothing in `node<Types>` needs to know its transport is
TLS-wrapped (satisfying `network_client`/`network_server` is exactly this
guarantee).

Internally, `connect_to()` and the accept loop are unchanged from
`tcp_rpc.hpp`; immediately after the raw TCP handshake, both sides wrap the
fd in an `SSL*` from a shared `SSL_CTX*` and complete the TLS handshake
before any framed payload is sent. Verification happens in the standard
OpenSSL verify callback, delegating to `tls_rpc_trust_policy::accepts()`
(Requirement 1.2).

```cpp
class tls_tcp_rpc_server {
public:
    tls_tcp_rpc_server(std::uint16_t port, tls_tcp_rpc_config config);

    // Requirement 1.3 / 6.2 / 7.2: applied to the live SSL_CTX*, no
    // listener restart — mirrors cpp_httplib_server::reload_tls_material().
    auto reload_identity(std::string cert_path, std::string key_path) -> void;
    auto reload_trust_policy(tls_rpc_trust_policy policy) -> void;

    // ... register_*_handler / start / stop / is_running, as tcp_rpc_server
};
```

`tls_tcp_rpc_client` gets the identical two reload methods — a client
connection is short-lived (one RPC call, then closed, matching
`tcp_rpc_client::call()`'s existing per-call-connect model), so "reload" here
just means "the *next* call presents/verifies under the updated
config" — there's no long-lived client-side `SSL_CTX*` session to migrate
mid-flight.

### 3. `ca_state_machine` additions (Requirement 4)

Follows the existing `ca_command_type` pattern exactly:

```cpp
enum class ca_command_type : std::uint8_t {
    bootstrap_ca,
    record_issuance,
    record_revocation,
    noop,
    record_rpc_tls_ready,   // new
};

[[nodiscard]] inline auto encode_record_rpc_tls_ready_command(std::uint64_t node_id)
    -> std::vector<std::byte> {
    boost::json::object obj;
    obj["type"] = static_cast<int>(ca_command_type::record_rpc_tls_ready);
    obj["node_id"] = node_id;
    return /* ...boost::json::serialize(obj) as bytes, matching existing encode_* ... */;
}
```

`apply()` gains one more `case`, inserting into a `std::set<std::uint64_t>
_rpc_tls_ready` member; `get_state()`/`restore_from_snapshot()` gain one more
field alongside the existing ledger/bootstrap-material serialization.
`rpc_tls_ready_node_ids()` returns a copy of the set.

### 4. `ca_cluster_node` maintenance-thread additions

Three new closures, each following the exact shape of the existing
`maybe_bootstrap`/`ensure_signer` (read `ca_state_machine` via
`read_state()`, act if applicable, tolerate a failed read by retrying next
tick):

```cpp
auto maybe_acquire_rpc_identity = [&] {                    // Requirement 5
    if (have_valid_persisted_peer_cert()) return;
    auto state = read_ca_state(raft_node, 5s);
    if (!state.has_root_material()) return;
    auto [key_pem, csr_pem] = raft::testing::generate_key_and_csr(peer_identity_options(cfg.node_id));
    auto material = raft_node.is_leader()
        ? sign_locally(csr_pem)                              // reuses the leader's in-memory `signer`
        : request_via_leader_http(csr_pem, cfg, k_command_timeout);
    persist_peer_cert(cfg.data_dir, material, key_pem);
    rpc_server_ptr->reload_identity(...); rpc_client_ptr->reload_identity(...);
    rpc_server_ptr->reload_trust_policy(either(bootstrap_fp, state.root_certificate_pem()));
    raft_node.submit_command(encode_record_rpc_tls_ready_command(cfg.node_id), k_command_timeout).get();
};

auto maybe_finalize_rpc_tls_cutover = [&] {                 // Requirement 6
    if (cutover_finalized) return;
    auto state = read_ca_state(raft_node, 5s);
    auto ready = state.rpc_tls_ready_node_ids();
    for (auto id : cfg.all_node_ids()) if (!ready.contains(id)) return;
    rpc_server_ptr->reload_trust_policy(ca_root_only(state.root_certificate_pem()));
    rpc_client_ptr->reload_trust_policy(ca_root_only(state.root_certificate_pem()));
    cutover_finalized = true;
};

auto maybe_renew_rpc_identity = [&] { /* Requirement 7.2, expiry check + /v1/certificates/renew */ };
```

These run only when RPC TLS is enabled (Requirement 3.3's opt-in); with
plain `tcp_rpc_client`/`server`, they're simply never installed.

## Correctness Properties

### Property 1: `tcp_rpc.hpp` is unaffected

No line in `tcp_rpc.hpp` changes. `chaos_node`, `dns_discovery_node`, and any
other consumer of `tcp_raft_types` continue to build and behave identically.
Verified by: this spec's tasks touch only `tls_tcp_rpc.hpp` (new file),
`ca_state_machine.hpp` (additive), and `cmd/ca_cluster_node/` — never
`tcp_rpc.hpp` or `tcp_raft_types.hpp`.

### Property 2: The bootstrap credential is never required to establish the CA root's own trust

Requirement 2.2's fingerprint-comparison trust policy is self-contained — it
never reads `ca_state_machine`'s root material, so `bootstrap_ca` can commit
using only the static credential, before any CA root exists. This is the
resolution of the ordering problem described in the Overview.

### Property 3: Finalization can never strand a peer (Requirement 6.3)

Let `ready(t)` be the `rpc_tls_ready` set at logical time `t`. A node `N`
finalizes (drops bootstrap-credential trust) only when it observes
`ready(t) ⊇ all_node_ids`. By construction (Requirement 5.3), `id ∈
ready(t)` implies node `id` had already reconfigured its own transport to
*also* present and accept a CA-chain-verifiable identity at some earlier
time `t' < t`. So the instant any node observes the full set, every member
of the cluster is already independently reachable via CA-chain trust —
*regardless* of how much observation lag exists between nodes, since the
fact "peer X supports CA-chain trust" only ever gets *more* true over time
(a node's own identity, once switched, is never switched back). A node that
finalizes "early" relative to a slower peer's own observation is therefore
still safe: the slower peer hasn't dropped its bootstrap trust yet, so it
remains reachable by everyone regardless of what it itself has observed, and
the early-finalizing node only needed the *slower peer's* CA-chain identity
to already exist — which Property 3's premise guarantees.

### Property 4: Dual-trust never accepts a weaker credential than either alone

`either(fingerprint, root_pem).accepts(cert)` is `accepts_fingerprint(cert)
OR accepts_chain(cert)` — strictly more permissive than either policy alone,
by construction, never less. Nothing in this design ever narrows trust
*during* the dual-trust window; narrowing only happens at the single,
well-defined finalization point (Requirement 6.2), and only by removing the
bootstrap-fingerprint disjunct, never the chain-verification one.

### Property 5: A restarted, already-cutover node needs no bootstrap credential

Requirement 7.1's persisted peer certificate means `have_valid_persisted_peer_cert()`
is true immediately on restart, so `maybe_acquire_rpc_identity` is a no-op
and the node's RPC transport starts directly under `ca_root_only` trust — it
never touches `rpc_tls_cert_path`/`rpc_tls_key_path` (the bootstrap
credential) at all after its first successful cutover, satisfying "the
initial traffic is the only traffic on the static credential" at the level
of an individual already-cutover node's entire post-cutover lifetime,
restarts included.

## Error Handling

- **Bootstrap credential missing/unreadable at startup with RPC TLS
  enabled**: fail closed (Requirement 2.3) — same posture as the existing
  `--auth-token`/`--unseal-key-file` checks.
- **Peer certificate acquisition fails** (leader unreachable, no quorum,
  token mismatch): logged, retried next maintenance tick; Raft consensus
  itself is unaffected since the node still has a working (bootstrap-cred)
  RPC identity throughout (Requirement 5.4).
- **TLS handshake failure on an RPC connection** (untrusted peer cert,
  protocol mismatch): the connection attempt fails exactly as
  `tcp_detail::connect_to`/`accept()` failing today already does — the
  caller (`tcp_rpc_client::call()`'s equivalent) surfaces a
  `network_exception`, handled by the existing Raft retry/timeout logic
  with no new failure mode introduced at that layer.
- **Renewal failure** (Requirement 7.3): logged via `metrics_type`, retried;
  never forces a live connection closed or crashes the node — mirrors
  Requirement 16.7 of the certificate-authority spec exactly.

## Testing Strategy

- **Unit**: `tls_rpc_trust_policy::accepts()` — fingerprint match/mismatch,
  chain-verifies/doesn't, `either` unions both correctly.
- **Integration** (real sockets, real OpenSSL, no libcoap/lakers
  dependency): a 2-node `tls_tcp_rpc_client`/`server` pair completing a
  real mutual-TLS handshake and one RPC round trip under each trust policy
  (`bootstrap-fingerprint-only`, `either`, `ca-root-only`); a mismatched/
  absent client cert rejected under `SSL_VERIFY_FAIL_IF_NO_PEER_CERT`.
- **Multi-node subprocess** (extending the existing `ca_cluster_node`
  multi-process test pattern used for Requirement 17's leader-failover
  coverage): 3 real `ca_cluster_node` processes started with only the
  bootstrap credential, confirming election/replication succeeds over TLS,
  `bootstrap_ca` committing, each node reaching `rpc_tls_ready` for itself,
  cutover finalizing on all three, and — the property this whole spec exists
  to prove — the cluster continuing to operate normally (submit a
  certificate issuance, confirm it commits) after the bootstrap credential
  file is deleted or corrupted on disk post-cutover.
- **Staggered finalization** (Property 3): drive the three nodes' maintenance
  ticks out of lockstep (e.g. one node's tick artificially delayed) and
  confirm no connectivity gap opens at any point — the direct test of
  Requirement 6.3.
- **Restart without bootstrap credential** (Property 5): cutover a 3-node
  cluster, delete the bootstrap credential file, restart one node with
  `--rpc-tls-cert`/`--rpc-tls-key` pointing at nothing, confirm it still
  rejoins using its persisted peer certificate (Requirement 7.1).

## Non-Goals

- **Automatic revocation of the bootstrap credential everywhere it was
  copied.** Once cutover, this design makes it *unnecessary*, not *invalid*
  — an operator who wants it fully retired deletes the files themselves
  (Requirement 6.4). A CA-driven revocation-at-a-distance mechanism is out
  of scope.
- **Securing `tcp_rpc.hpp`'s other consumers** (`chaos_node`,
  `dns_discovery_node`). This spec's transport is additive and
  `ca_cluster_node`-specific; nothing here migrates other Raft binaries.
- **Rotating the bootstrap credential itself** on a schedule. It is a
  one-time, pre-cluster-existence secret by design (Property 5); ordinary
  cert rotation (Requirement 7.2) only applies to CA-issued peer
  certificates.
