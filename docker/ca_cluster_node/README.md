# ca_cluster_node deployment (Requirement 17)

`ca_cluster_node` replicates a CA's root material, issuance ledger, and
revocation list across a Kythira Raft cluster and persists them to disk
(`include/raft/ca_state_machine.hpp`, `cmd/ca_cluster_node/main.cpp`). This
directory packages two ways to deploy the recommended 3-node, 3-AZ topology
(Requirement 17.11/17.12), plus documents a third, code-free automated
alternative.

| Path | Files | When to use |
|---|---|---|
| Manual, 3 EC2 instances | `ca_cluster_node.service`, `ca_cluster_node.env.example` | Simplest — no ECS/Fargate dependency, direct control over each instance |
| Manual, ECS Fargate | `ecs-task-definitions/*.json`, `ecs-execution-role-policy.json` | Already-containerized infrastructure, want ECS-managed restarts |
| Automated EC2 replacement | (no new files — see below) | Want Kythira to detect and replace a failed node's instance automatically |

All three place one node per Availability Zone — `us-east-1a`/`us-east-1b`/`us-east-1c`
in the examples below, substitute your own three AZs — so a single AZ outage
never costs the cluster its quorum (2 of 3).

## Building the image

```
docker build -f docker/ca_cluster_node/Dockerfile -t kythira-ca-cluster-node:VERSION .
```

Tag each build with a version (a release number or the commit hash), not
`:latest`. The ECS task definitions go further and pin the image by digest;
see `ecs-task-definitions/README.md`'s "Pinning the image".

(Requires `vcpkg_installed/` already present in the build context, same as
`docker/ca_service/Dockerfile`.)

## Path 1 — manual, 3 EC2 instances (systemd)

See `ca_cluster_node.service`'s header comment for the install steps. Copy
the SAME unit file to all three instances; each instance's
`/etc/default/ca_cluster_node` (from `ca_cluster_node.env.example`) differs
only in `NODE_ID` and whether `BOOTSTRAP_CA_FLAG` is set — `PEERS` and
`CA_SERVICE_AUTH_TOKEN` are identical across all three, as is the unseal
passphrase installed separately at `/etc/ca_cluster_node/unseal.key`
(Requirement 17.4: byte-identical on every node, or the persisted CA key
becomes unrecoverable), and — if RPC TLS is enabled, see below —
`rpc_bootstrap.crt`/`rpc_bootstrap.key`. Each instance also has its own
client-API TLS pair, `http_tls.crt`/`http_tls.key` (see below).

The bearer token is read from `CA_SERVICE_AUTH_TOKEN`, never the command
line: `--auth-token` still works but logs a warning, because any local user
can read a process's arguments from `/proc/<pid>/cmdline` or `ps`.

## Securing the client HTTP API

The bearer token and every issued certificate cross the client-facing API, so
**a plaintext API is opt-in**, under the same rule as plaintext Raft RPC
below. Without `--tls-cert`/`--tls-key` a node refuses to start unless
`--http-address` is loopback, or `--allow-plaintext-http` /
`CA_CLUSTER_ALLOW_PLAINTEXT_HTTP=1` is given for a network you trust end to
end.

Nodes also call each other's client API (enrollment and the RPC trust state,
below), and those calls now verify TLS: an `https://` address in `--peers` is
checked, chain and hostname, against the root in the node's own `--tls-cert`
bundle (or `--peer-tls-ca <bundle>`). So every node's listener certificate
must chain to one shared root and carry a SAN for the host or IP its peers
dial. An `http://` peer address must be loopback unless plaintext HTTP is
opted in, and a peer address without a scheme is a startup error.

**Upgrading:** a node that served plaintext HTTP off loopback exits at startup
on this release. Provision `HTTP_TLS_CERT`/`HTTP_TLS_KEY` (see
`ca_cluster_node.env.example`) and switch `PEERS` to `https://`, or add
`--allow-plaintext-http` to keep the previous behaviour.

## Securing the Raft-internal RPC channel (RPC TLS, `.kiro/specs/ca-cluster-rpc-mtls/`)

Separate from the client-facing HTTPS listener's own TLS
(`--tls-cert`/`--tls-key`, fingerprint-pinned per the section below),
`ca_cluster_node` also supports mutual TLS on the Raft-internal RPC channel
between the three cluster peers themselves (`--rpc-tls-cert`/`--rpc-tls-key`).
It is required unless you opt out explicitly.

**Plaintext Raft RPC is opt-in.** Anyone who can reach a plaintext Raft port
can send `AppendEntries` or `InstallSnapshot` and so rewrite the CA's
replicated state. A node with neither the RPC TLS flags nor a persisted peer
certificate under `--data-dir` therefore refuses to start, unless one of these
holds:

- `--rpc-address` is loopback (`127.0.0.0/8`, `::1`, or a name such as
  `localhost` that `/etc/hosts` maps only to those), for single-host development and
  tests. The RPC listener then binds loopback only.
- `--allow-plaintext-rpc` is given, or `CA_CLUSTER_ALLOW_PLAINTEXT_RPC=1` is
  set, for a network you trust end to end.

Either way the node logs a warning that its Raft RPC is plaintext.
`--rpc-address` (default `0.0.0.0`) now selects what the RPC listener binds,
plaintext or TLS. It takes an IPv4 or IPv6 address, or a host name. A name is
looked up once at startup in `/etc/hosts` only, never in DNS, so a resolver
can't influence what the node listens on. The listener binds every address
the name is listed under (so `localhost` usually covers both `127.0.0.1` and
`::1`; if `/etc/hosts` doesn't list `localhost`, those two are used). Every
one of those addresses must belong to this host, loopback or an interface
address. A name missing from `/etc/hosts`, or listed with another host's
address, stops the node at startup.

`--http-address` (default `0.0.0.0`) selects what the client HTTP API listens
on, and takes the same forms as `--rpc-address`, plus `*`. `*` listens on
both `0.0.0.0` and `::`, on separate sockets. `0.0.0.0` on its own stays IPv4
only, so an upgrade never opens an IPv6 port unless you ask for one.

**Upgrading a plaintext cluster:** a node that ran plaintext across hosts on
an earlier release will exit at startup on this one. Either provision the RPC
bootstrap credential below, or add `--allow-plaintext-rpc` to keep the
previous behaviour.

**Two-phase bootstrap, entirely automatic after initial setup:**

1. **Before the CA root exists**, all three nodes mutually authenticate
   using a small, static, self-signed credential the operator generates
   once and copies byte-identical to all three nodes — the same
   distribution model as `unseal.key`:
   ```
   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
     -nodes -keyout rpc_bootstrap.key -out rpc_bootstrap.crt -days 3650 \
     -subj "/CN=ca-cluster-rpc-bootstrap"
   ```
   Install both files (mode `0600`) alongside `unseal.key` and pass
   `--rpc-tls-cert`/`--rpc-tls-key` (see `ca_cluster_node.env.example`'s
   `RPC_TLS_CERT`/`RPC_TLS_KEY`).
2. **Once the CA root exists** (after `--bootstrap-ca` commits), each node
   automatically requests its own certificate from the now-running cluster,
   hot-reloads its RPC transport to present it, and starts a dual-trust
   window accepting either credential. No operator action is required.
3. **Once every configured node has completed step 2**, each node
   independently finalizes cutover, no longer accepting the bootstrap
   credential's fingerprint for new RPC connections. From this point on, a
   restarted node rejoins using its own persisted certificate
   (`--data-dir`) — the bootstrap credential is never needed again.

**Peer identity is separate from client identity.** The cluster root signs
both Raft peer certificates and ordinary client certificates, so RPC TLS does
not trust "any certificate chaining to the root": a CA-issued certificate is
accepted as a Raft peer only if it carries a configured node's reserved DNS
name, `ca-cluster-node-<node_id>`, and it may then act only as that node: an
RPC claiming a different sender (`leader_id`/`candidate_id`) is dropped, and a
node refuses a peer answering in place of the one it dialled. Those names can only be obtained through
peer enrollment, which is authenticated with an HMAC key derived from the
unseal passphrase — `POST /v1/certificates` answers `403` to a request for a
`ca-cluster-node-*` name (or one carrying `rpc_tls_ready_node_id`) that holds
only the client bearer token. The same key authenticates the RPC trust state
a node fetches from the leader's peer-only `GET /v1/peer/rpc-trust`: the
cluster root (its RPC trust anchor), whether every node has cut over, and the
serials of revoked peer certificates. These node-to-node calls carry only peer
MACs, never the client bearer token, and a bearer token gets nothing from
`/v1/peer/rpc-trust`. The key is derived with PBKDF2-HMAC-SHA256 (200,000
iterations, the same cost as the at-rest CA key encryption), so a MAC seen on
the wire is no shortcut to guessing the passphrase.

**Every node finalizes, and remembers it.** Followers learn that the cutover
is complete from the trust state and drop the bootstrap credential too, not
only whichever node led when the last peer enrolled, and each node records it
as `rpc_cutover_finalized` under `--data-dir`. A restarted node with that
record trusts the cluster root alone even if `--rpc-tls-cert` is still
passed. A replacement node that still holds only the bootstrap credential
enrolls through the peers' client API (it tries every peer, since it cannot
learn the leader over RPC), then joins with its new identity.

**Revocation applies to Raft peers.** Peer certificates, the leader's own
included, are recorded in the issuance ledger, and revoking one
(`POST /v1/certificates/revoke`) makes every node refuse it on new RPC
connections within one trust refresh (about five seconds). A node whose own
peer certificate is revoked enrolls a fresh key.

**Peer certificates renew themselves.** Each node renews its peer
certificate once it is within `--rpc-renewal-window-secs` of expiry (default
604800, seven days, against a 30-day validity) and hot-reloads it into the
running RPC transport. A follower renews through the leader's mTLS
`POST /v1/certificates/renew`, so renewal needs the client API on `https://`;
on a plaintext API the node logs once that renewal cannot run. The leader
signs its own renewal in-process.

All nodes must run a release with these checks before any of them is
restarted onto it: the peer key derivation and the trust-state route changed,
so a node on the new release and one on an older release cannot enroll
through each other (both retry harmlessly, staying on whatever credential they
hold, until the peer is upgraded). An already cut-over cluster keeps running
on its persisted peer certificates throughout a rolling upgrade.

If RPC TLS is enabled, consider raising the Raft timing flags beyond their
plain-TCP defaults — every RPC call now pays a full TLS handshake, which is
measurably slower under real host load:
```
--election-timeout-min-ms 1000 --election-timeout-max-ms 2000 \
--heartbeat-interval-ms 300 --rpc-timeout-ms 2000
```

See `.kiro/specs/ca-cluster-rpc-mtls/design.md` for the full design and
`ca_cluster_node.env.example`'s `RPC_TLS_CERT`/`RPC_TLS_KEY` comment for the
exact provisioning steps. **Path 3** (`aws_ec2_quorum_manager`, below) needs
no additional code or configuration beyond what Path 1 already needs — the
bootstrap credential is baked into the AMI exactly like `unseal.key`
already is.

## Path 2 — manual, ECS Fargate

See `ecs-task-definitions/README.md` for the full walkthrough (Cloud Map DNS
addressing, EFS-backed persistence, Secrets Manager-sourced auth token and
unseal passphrase, IAM roles).

## Path 3 — automated EC2 replacement via `aws_ec2_quorum_manager`

For operators who want Kythira to detect and replace a failed node's EC2
instance automatically rather than hand-managing three long-lived instances
or ECS services: run `ca_cluster_node` on EC2 via Path 1's systemd unit,
pre-baked into an AMI with `packer/ca_cluster_node/scripts/build.sh --arch
amd64 --region us-east-1` (see [`packer/ca_cluster_node/README.md`](../../packer/ca_cluster_node/README.md)
for the full build pipeline — no secrets are baked in; per-node
configuration is still supplied at launch time exactly as in Path 1), and
configure the project's existing `aws_ec2_quorum_manager` with one placement
group per AZ:

```cpp
kythira::aws_ec2_quorum_manager_config cfg;
cfg.cluster_name = "ca-cluster";
cfg.image_id = "ami-...";                 // from `build.sh`'s stdout, or packer-manifest.json
cfg.node_port = 7000;                     // Raft RPC port
cfg.topology.groups = {
    {.group_id = "us-east-1a", .target_count = 1},
    {.group_id = "us-east-1b", .target_count = 1},
    {.group_id = "us-east-1c", .target_count = 1},
};
cfg.subnet_by_group = {
    {"us-east-1a", "subnet-aaa..."},
    {"us-east-1b", "subnet-bbb..."},
    {"us-east-1c", "subnet-ccc..."},
};
```

This is exactly the `group_id` = AZ-name / `subnet_by_group` shape already
exercised by `tests/aws_quorum_manager_unit_test.cpp`'s `ec2_construction`
suite — `aws_ec2_quorum_manager` needed no CA-specific change to provision a
3-AZ-spread `ca_cluster_node` fleet, so this path introduces no new code, only
this configuration. See `include/raft/aws_ec2_quorum_manager.hpp` for the full
config surface.

## Verifying a deployment

Once all three nodes are healthy (`GET /healthz` on each returns `200`):

```
curl -H "Authorization: Bearer $TOKEN" https://<any-node>:8443/v1/root-ca
```

A follower answers `308` with a `Location` pointing at the current leader; the
leader answers `200` with the root CA certificate PEM. If every node answers
`503 {"error":"no_known_leader"}`, no leader has been elected yet (check RPC
connectivity between the three nodes' `rpc_port`s) or the
`--bootstrap-ca`-flagged node is not running yet. On a fresh cluster the other
two nodes never campaign while their logs are empty (Requirement 17.10), so
the first leader is always the flagged node, whatever order the three start in.

## Bootstrapping a new client's trust (fingerprint pinning, Requirement 19)

A fresh instance requesting its first certificate has no prior certificate
chain to verify the cluster's TLS listener against. Print each node's root
fingerprint once (any node — they all serve the same root):

```
ca_cluster_node --print-root-fingerprint --tls-cert chain.pem --tls-key key.pem
```

Distribute the printed SHA-256 fingerprint through the SAME out-of-band
channel already used for `CA_SERVICE_AUTH_TOKEN` (e.g. as
`CA_CLUSTER_ROOT_FINGERPRINT` in `ca_cluster_node.env.example`, or the
equivalent Secrets Manager entry for Path 2). The new instance then calls
`raft::testing::fetch_trusted_root()` (`include/raft/ca_bootstrap_client.hpp`)
with that fingerprint before trusting any response over TLS — it connects
with normal chain verification disabled, checks the ACTUAL presented root
against the pinned fingerprint, and only then fetches `GET /v1/root-ca` for
use on every subsequent, ordinary chain-verified connection. `--tls-cert`
MUST point at a full leaf+root chain, not a leaf-only certificate, or there
is no root in the presented chain to pin against.

## Certificate renewal

`POST /v1/certificates/renew` (authenticated by the caller's own mTLS client
certificate rather than the bearer token; leader-only — followers redirect
like every other `/v1/*` route) re-issues a certificate for the same
identifiers as an existing one, ahead of expiry. The new certificate copies
its SANs and key usage from the presented certificate, and the CSR's subject
must equal the presented certificate's subject (`400` otherwise). A revoked
certificate cannot renew (`401`), and `validity_days` — here and on
`POST /v1/certificates` — must be an integer from 1 to 825 (`400` otherwise). See
`ca_test_fixture::renew()` (`tests/ca_test_fixture.hpp`) for the equivalent
in-process pattern, and `reload_tls_material()`/`enable_auto_reload()`
(`include/raft/http_transport.hpp`, `coap_transport.hpp`) for hot-reloading
the renewed material into a running server/client without a restart.
