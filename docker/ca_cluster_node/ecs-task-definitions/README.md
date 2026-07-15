# ca_cluster_node — 3-AZ ECS Fargate task definitions

Three sample task definitions (`node-1-us-east-1a.json`, `node-2-us-east-1b.json`,
`node-3-us-east-1c.json`), one per Availability Zone, implementing
Requirement 17.12(a): each pinned to a distinct subnet/AZ via its own ECS
**service** (not three tasks of one service — each node needs a stable,
individually-addressable identity), with `--peers` listing all three nodes'
Raft RPC and client-facing HTTP addresses.

Diff the three files and you'll find they differ **only** in:

- `family` / container `name` (`kythira-ca-cluster-node-{1,2,3}`)
- `NODE_ID` (`1`, `2`, `3`) in the `command` wrapper script
- `BOOTSTRAP_CA_FLAG` — `--bootstrap-ca` on node 1 only, empty on nodes 2/3
  (Requirement 17.10: at most one node submits the bootstrap command; the
  other two wait for replication)
- the AZ/subnet the owning ECS **service** places it in (set on the service,
  not the task definition itself — see below)

Everything else — image, CPU/memory, RPC/HTTP ports, EFS mount, secrets,
health check — is identical across all three, matching the "same systemd
unit / ECS task definition ... replicated three times with different
`--peers` values and one distinct AZ/subnet each" pattern from task 14's
artifacts (`docker/ca_service/ecs-task-definition.json`).

## Why Cloud Map DNS names, not static IPs

Per this project's container-runtime-compatibility rules (`CLAUDE.md`), no
static IP addresses are baked into configuration — Fargate's `awsvpc` mode
assigns each task a fresh ENI/IP on every restart anyway, so a static IP
would break on the very first task replacement. Each node is registered
under **ECS Service Discovery** (AWS Cloud Map) with a stable DNS name —
`ca-node-1.ca-cluster.internal`, `ca-node-2.ca-cluster.internal`,
`ca-node-3.ca-cluster.internal` — and `--peers` references those names, not
IPs. Kythira's own `tcp_rpc_client`/`httplib::Client` resolve hostnames at
connection time, so no application-level change is needed.

## Prerequisites (fill in before deploying)

1. An ECR repository holding the image built from `docker/ca_cluster_node/Dockerfile`
   (`ACCOUNT_ID.dkr.ecr.REGION.amazonaws.com/kythira-ca-cluster-node:latest`).
2. A private Cloud Map namespace `ca-cluster.internal` and one ECS Service
   Discovery-enabled service per node, each in its own AZ's subnet.
3. An EFS file system + access point per node (or one file system, three
   access points — one per node's `/var/lib/ca_cluster_node`), so Raft's
   `file_persistence` survives task replacement (Requirement 17: a restarted
   node recovers from disk).
4. A Secrets Manager secret `kythira/ca-cluster-node/auth-token` (bearer
   token, identical across all three nodes),
   `kythira/ca-cluster-node/unseal-key` (the unseal passphrase, byte-identical
   across all three nodes per Requirement 17.4), and — for RPC-internal mTLS
   (`.kiro/specs/ca-cluster-rpc-mtls/`, optional but recommended) —
   `kythira/ca-cluster-node/rpc-tls-cert`/`rpc-tls-key` (the RPC bootstrap
   credential, likewise byte-identical across all three nodes; see
   `../README.md`'s "Securing the Raft-internal RPC channel" section for how
   to generate it, and note it is only needed for each node's very first
   cutover, not its ongoing operation).
5. IAM execution role with `secretsmanager:GetSecretValue` for the secrets
   above and standard ECS/EFS execution permissions; IAM task role —
   `ca_cluster_node` itself makes no AWS API calls in the manual path, so an
   empty/minimal task role is sufficient (contrast with `ca_service
   --provider aws-acm-pca`'s ACM-PCA permissions in `docker/ca_service/ecs-task-role-policy.json`,
   which don't apply here since `ca_cluster_node` only ever uses the local,
   Raft-replicated CA).

## The unseal-key-file requirement

`--unseal-key-file` (and, likewise, `--rpc-tls-cert`/`--rpc-tls-key`) expect
**file paths**, not environment variables, but ECS `secrets` only injects
environment variables. Each task definition's `command` is a small `sh -c`
wrapper that writes the injected `CA_CLUSTER_UNSEAL_KEY`,
`CA_CLUSTER_RPC_TLS_CERT`, and `CA_CLUSTER_RPC_TLS_KEY` secrets to
`/tmp/unseal.key`, `/tmp/rpc_bootstrap.crt`, and `/tmp/rpc_bootstrap.key`
respectively (`chmod 600`, ephemeral container filesystem — never touches
the EFS-backed persistent volume) before exec'ing `ca_cluster_node
--unseal-key-file /tmp/unseal.key --rpc-tls-cert /tmp/rpc_bootstrap.crt
--rpc-tls-key /tmp/rpc_bootstrap.key`.

## Deploying

```
aws ecs register-task-definition --cli-input-json file://node-1-us-east-1a.json
aws ecs register-task-definition --cli-input-json file://node-2-us-east-1b.json
aws ecs register-task-definition --cli-input-json file://node-3-us-east-1c.json
# then create one ECS service per task definition, each targeting its own
# AZ's subnet and Cloud Map service — see the automated alternative below for
# a way to avoid hand-managing three services.
```

## Automated alternative: `aws_ec2_quorum_manager`

Operators who want Kythira to detect and replace a failed node's EC2 instance
automatically (rather than the manual ECS services above) can instead run
`ca_cluster_node` directly on EC2 (via `ca_cluster_node.service`, this
directory's sibling systemd unit) and point Kythira's existing
`aws_ec2_quorum_manager` at three placement groups named by AZ — this reuses
already-implemented, already-tested code
(`tests/aws_quorum_manager_unit_test.cpp`'s `ec2_construction` suite exercises
exactly this shape); this task adds no new provisioning mechanism:

```cpp
kythira::aws_ec2_quorum_manager_config cfg;
cfg.cluster_name = "ca-cluster";
cfg.image_id = "ami-...";                 // built via packer/ca_cluster_node/scripts/build.sh —
                                           // see ../../../packer/ca_cluster_node/README.md
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

`target_count = 1` per AZ, `subnet_by_group` set to a distinct per-AZ subnet —
matching Requirement 17.12(b) exactly. See
`include/raft/aws_ec2_quorum_manager.hpp` for the full config surface.
