# Design Document

## Overview

```
kythira_node
├── node core: config (KYTHIRA_NODE_*), identity and TLS, peer list,
│              logger, metrics/health endpoint, signal handling
├── multi-raft host (one Raft listener, one placement driver)
│     ├── module 1 group space: groups, key prefix, data subdir
│     ├── module 2 group space ...
├── module registry ── in-process interfaces by name
└── modules (each with its own executor and listeners)
      iam ─► publishes evaluator
      hsm ─► publishes native client
      dynamodb, kinesis, redis, ledger ─► look those up, else use the network
```

A stand-alone daemon is `kythira_node` compiled with one module and that
module fixed, plus the module's legacy aliases for node-wide settings.

## Key design decisions

### Build-time registration, not plug-ins

Modules register through a static registrar object in their library, kept
alive by linking the module's library whole-archive into the node when its
Kconfig switch is on. Loading modules as shared objects at run time would
need a stable C++ ABI across modules, and gives nothing an operator needs:
the set of services is a build choice today and stays one.

### One host, partitioned ids and keys

The multi-raft host already supports many groups and key ranges. A module
number `m` (fixed per module in core: redis 1, dynamodb 2, kinesis 3,
ledger 4, iam 5, hsm 6, kms 7) gives a composite node's module the group
range `[m << 56, (m + 1) << 56)` and the key prefix `\x00m/`. DynamoDB's
per-voter split ranges (`(ordinal + 1) << 48`) fit inside its 2^56 range.
In a stand-alone daemon the offset is zero and the prefix empty, so ids and
keys are exactly today's (Requirement 3.2, 4.4). The cost is that a
stand-alone data directory cannot be opened by a composite node, which is
why moving a service between layouts is a backup-and-restore (3.3).

### Executors per module, Raft on its own

Each module gets its own I/O and worker executors; the host keeps its
existing Raft threads. A module whose queue fills sheds load with its own
throttling error (ProvisionedThroughputExceeded, Redis BUSY, ...) instead
of blocking a shared pool. The test in 6.3 saturates DynamoDB and asserts
no Kinesis or IAM election starts.

### In-process interfaces through a registry

The registry maps a name (`iam.evaluator`, `hsm.client`,
`kinesis.put_records`) to a typed interface pointer. A consumer asks once
at start; a miss means "not co-located" and the consumer builds its network
client instead. Calls carry a `caller` struct (module, principal, source
request id) that the target authorizes exactly as it authorizes a network
request, so co-location never widens access.

### IAM moves onto the host

IAM is the only service on a single plain Raft group. It moves to one
group on the multi-raft host, with its state machine unchanged. Its current
data directory holds a plain Raft log and snapshots; `iam_node` gets a
one-shot `--convert-data-dir` that writes the latest snapshot into the
new layout, rather than keeping two Raft stacks alive.

### Redis's embedded IAM replica becomes the general case

`redis_gateway_node` already embeds an IAM read replica. In a composite
node that replica is simply the co-located IAM module; the
`KYTHIRA_IAM_*` replica settings stay for a stand-alone Redis gateway.

## Components

| Component | Where | Role |
|---|---|---|
| `service_module.hpp` | `include/raft/` | interface, `caller`, group space |
| `module_registry.hpp` | `include/raft/` | registrar, in-process interfaces |
| `node_core.hpp` | `include/raft/` | node config, identity, logger, metrics |
| `kythira_node` | `cmd/kythira_node/` | the composite daemon |
| one adapter per service | `services/<m>/module/` | wraps each service's existing gateway code |

## Error handling

- Overlapping group spaces, an unknown module name, or a missing
  dependency (a consumer configured for in-process IAM with no IAM module)
  stop the node at start with the reason.
- A module failing at run time is logged with its name; the node stops,
  since a half-running node misleads health checks.

## Testing strategy

Per-module adapter tests, the all-modules in-process cluster test, the
per-module and all-modules build legs, the composite compose file and
docker-chaos scenario, and every existing stand-alone test unchanged.

## Open questions

- Whether a composite node should allow a module to be enabled on only some
  nodes (for example the HSM on three of nine). The first version requires
  the same module set on every node.
