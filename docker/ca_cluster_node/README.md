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
docker build -f docker/ca_cluster_node/Dockerfile -t kythira-ca-cluster-node:latest .
```

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
becomes unrecoverable).

## Path 2 — manual, ECS Fargate

See `ecs-task-definitions/README.md` for the full walkthrough (Cloud Map DNS
addressing, EFS-backed persistence, Secrets Manager-sourced auth token and
unseal passphrase, IAM roles).

## Path 3 — automated EC2 replacement via `aws_ec2_quorum_manager`

For operators who want Kythira to detect and replace a failed node's EC2
instance automatically rather than hand-managing three long-lived instances
or ECS services: run `ca_cluster_node` on EC2 via Path 1's systemd unit (baked
into an AMI, e.g. via Packer), and configure the project's existing
`aws_ec2_quorum_manager` with one placement group per AZ:

```cpp
kythira::aws_ec2_quorum_manager_config cfg;
cfg.cluster_name = "ca-cluster";
cfg.image_id = "ami-...";                 // AMI running ca_cluster_node
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
connectivity between the three nodes' `rpc_port`s) or, if only the
`--bootstrap-ca`-flagged node's own answer matters, that node may not have won
the cluster's first election yet (Requirement 17.10 — non-flagged nodes never
self-bootstrap, by design).

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

`POST /v1/certificates/renew` (bearer-token authenticated, leader-only —
followers redirect like every other `/v1/*` route) re-issues a certificate
for the same identifiers as an existing one, ahead of expiry. See
`ca_test_fixture::renew()` (`tests/ca_test_fixture.hpp`) for the equivalent
in-process pattern, and `reload_tls_material()`/`enable_auto_reload()`
(`include/raft/http_transport.hpp`, `coap_transport.hpp`) for hot-reloading
the renewed material into a running server/client without a restart.
