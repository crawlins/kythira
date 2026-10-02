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

1. An ECR repository holding the image built from `docker/ca_cluster_node/Dockerfile`,
   and that push's digest substituted for `IMAGE_DIGEST` in all three files
   (`ACCOUNT_ID.dkr.ecr.REGION.amazonaws.com/kythira-ca-cluster-node@sha256:IMAGE_DIGEST`
   — see "Pinning the image" below).
2. A private Cloud Map namespace `ca-cluster.internal` and one ECS Service
   Discovery-enabled service per node, each in its own AZ's subnet.
3. An EFS file system + access point per node (or one file system, three
   access points — one per node's `/var/lib/ca_cluster_node`), so Raft's
   `file_persistence` survives task replacement (Requirement 17: a restarted
   node recovers from disk). The image runs as the unprivileged
   `ca-cluster-node` user, UID/GID `10002:10002`, so each access point must
   hand it a directory it owns: set the access point's POSIX user to
   `10002:10002` and its root-directory creation info to owner
   `10002:10002`, permissions `0750`. An access point that leaves the
   directory owned by root makes the node fail at startup, unable to write
   its data directory.
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

## Pinning the image

The task definitions name the image by **digest**, never by a tag such as
`:latest`. A tag is mutable: anyone who can push to the repository can move
it, and every task ECS starts afterwards — a restart, a scale-out, a
replacement after an AZ failure — silently runs the new image, which for a
CA holding the unseal key and the RPC TLS key is a key-exfiltration path.
A digest names exactly the bytes you reviewed, and it also keeps the three
nodes on the same build: with a tag, a push between two task replacements
leaves the quorum running mixed versions.

Push the image under a version tag, read back its digest, and substitute it
for `IMAGE_DIGEST` (the value after `sha256:`, 64 hex characters):

```
docker build -f docker/ca_cluster_node/Dockerfile \
    -t ACCOUNT_ID.dkr.ecr.REGION.amazonaws.com/kythira-ca-cluster-node:VERSION .
docker push ACCOUNT_ID.dkr.ecr.REGION.amazonaws.com/kythira-ca-cluster-node:VERSION
aws ecr describe-images --repository-name kythira-ca-cluster-node \
    --image-ids imageTag=VERSION --query 'imageDetails[0].imageDigest' --output text
```

Also turn on tag immutability for the repository
(`aws ecr put-image-tag-mutability --repository-name kythira-ca-cluster-node
--image-tag-mutability IMMUTABLE`), so a version tag cannot be re-pointed
either. Upgrading is then a new digest in a new task-definition revision,
rolled one node at a time.

## The unseal-key-file requirement

`--unseal-key-file` (and, likewise, `--tls-cert`/`--tls-key` and
`--rpc-tls-cert`/`--rpc-tls-key`) expect **file paths**, not environment
variables, but ECS `secrets` only injects environment variables. Each task
definition's `command` is a small `sh -c` wrapper that writes the injected
`CA_CLUSTER_UNSEAL_KEY`, `CA_CLUSTER_RPC_TLS_CERT`/`_KEY` and
`CA_CLUSTER_HTTP_TLS_CERT`/`_KEY` secrets to `unseal.key`,
`rpc_bootstrap.{crt,key}` and `http_tls.{crt,key}` under
`/run/ca_cluster_node/` (`umask 077`, so mode 0600) before exec'ing
`ca_cluster_node` with those paths.

The container's root filesystem is read-only (`readonlyRootFilesystem`), so
`/run/ca_cluster_node` is the one writable path besides the data directory:
a task-storage volume (`ca-cluster-node-run`, a volume with no EFS or host
configuration) that lives and dies with the task and never touches the
EFS-backed persistent volume. `TMPDIR` points there too. It is not a tmpfs
because Fargate rejects `linuxParameters.tmpfs`; and it is not `/tmp`
because Fargate creates a task-storage volume owned by root, mode 0755,
unless the image declares the path as a `VOLUME`, in which case it copies the
image directory's ownership. The Dockerfile declares
`/run/ca_cluster_node` that way, owned by `10002:10002`, mode 0700, so the
non-root node can write it and nothing else in the task can read it.

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
