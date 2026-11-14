# Design Document

## Overview

```
kythira_node
├── node core: config (KYTHIRA_NODE_*), identity and TLS, peer list,
│              logger, metrics/health endpoint, signal handling
├── one Raft listener and client, routed by group id range
│     ├── module 1 group space ─► module 1's multi-raft host, data subdir
│     ├── module 2 group space ─► module 2's multi-raft host ...
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

### One listener, a host per module, partitioned ids

The services' multi-raft hosts differ in their state machine type, storage
(memory, the binary engine, RocksDB through `state_engine`), transactions
(the `transactional_state_machine` wrapper, DynamoDB's own) and policy
hooks, and `multi_raft` takes one state machine type. Hiding all of them
behind one type would mean a type-erased state machine that also carries
every service's transaction and persistent-storage hooks: a large core
change for no operator benefit. So each module keeps its own host, and the
node shares what an operator sees: one Raft listener and client, one
identity and certificate, one peer list, one health endpoint, one log.

Every Raft RPC carries its group id. A module number `m` (fixed per module
in core: redis 1, dynamodb 2, kinesis 3, ledger 4, iam 5, hsm 6, kms 7)
gives a composite node's module the group range `[m << 56, (m + 1) << 56)`,
and `group_range_mux` (`include/raft/group_range_mux.hpp`) routes each
message to the host whose range holds it; each host's own demultiplexer
then routes by group inside it. A message for a range no running host
holds gets the refusal a host gives a group it does not hold. The hosts
share the node's one client, so connections between two nodes are shared
by every module. `node_host` (`include/raft/node_host.hpp`) is what a
module gets: node id, client, and `server_for(space)`.

Keys need no prefix: each module's keys live only in its own host's shard
map, so the placement driver, split/merge policy and key lookups never see
another module's (Requirement 4.2, 4.3). `group_space::key_prefix` stays
for a module that writes several modules' data into one store, which none
does today. DynamoDB's per-voter split ranges (`(ordinal + 1) << 48`) fit
inside its 2^56 range. In a stand-alone daemon the offset is zero and the
range covers every id, so ids are exactly today's (Requirement 3.2, 4.4).
The cost is that a stand-alone data directory cannot be opened by a
composite node, which is why moving a service between layouts is a
backup-and-restore (3.3).

The trade: each host runs its own placement driver, policy tick and Raft
executor stripes rather than sharing one. Per-module executors are what
task 9 wants anyway, and a placement driver that only sees one module's
shards balances that module's load, which is the load that matters to it.

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

For an AWS-compatible API the call is the signed request itself
(`aws_json_endpoint`): the consumer signs it with the credentials it would
use over the network and the target's own front end checks the signature
and the signer's policies. `kinesis.put_records` takes only the producer
calls (PutRecords, PutRecord, DescribeStreamSummary), so it is not a way
round the target's listener for the rest of its API.

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
| `module_interfaces.hpp` | `include/raft/` | the interface types and names modules publish (`aws_json_endpoint` for `kinesis.put_records`) |
| `node_core.hpp` | `include/raft/` | node config, identity, logger, metrics |
| `group_range_mux.hpp`, `node_host.hpp` | `include/raft/` | one Raft listener routed by group range; what a module runs on |
| `kythira_node_host.hpp` | `include/raft/` | the node's Beast transport and the host type modules register against |
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
