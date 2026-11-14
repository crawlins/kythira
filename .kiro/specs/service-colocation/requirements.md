# Requirements Document

## Introduction

Every Kythira service today is its own daemon with its own Raft cluster:

| Service | Daemon | Raft | Listeners |
|---|---|---|---|
| Redis gateway and build cache | `redis_gateway_node` | multi-raft host | Redis, Redis TLS, cache HTTP/HTTPS |
| DynamoDB | `dynamodb_gateway_node` | multi-raft host, bootstrap groups 1 and 2, per-voter id ranges | DynamoDB HTTPS |
| Kinesis | `kinesis_gateway_node` | multi-raft host | Kinesis HTTPS |
| Verifiable ledger | `ledger_node` | multi-raft host | ledger API, metrics |
| IAM, STS and OIDC issuer | `iam_node` | one plain Raft group (not multi-raft) | one HTTPS listener |

Only one pairing shares a process: `redis_gateway_node` embeds a read
replica of the IAM group (`iam_auth.cpp`) so Redis AUTH makes no network
call. Nothing else can: each daemon owns its Raft port, its group id space
(DynamoDB and others start at group 1), its data directory layout, its
TLS identity, its metrics endpoint and its signal handling. Running
DynamoDB, Kinesis and IAM for one small deployment means three clusters,
three sets of nodes and three quorums, and DynamoDB's Kinesis destination
and every service's IAM check cross the network.

This spec lets any set of services run **co-located** in one node process,
sharing one Raft listener, while each can still run **stand-alone** as
today. It is the hosting model the virtual HSM
(`.kiro/specs/virtual-hsm/`) is specified against, and the composite node
is where task 28 of the DynamoDB spec wires IAM into the gateway.

**Scope**: a service module interface, a composite daemon, moving each
existing daemon onto the interface, shared configuration, resource limits,
in-process calls between co-located services, and tests. **Out of scope**:
running services in separate OS processes under one supervisor, dynamic
loading of modules as shared objects, and moving any service to its own
repository.

## Glossary

- **Service module**: a service (Redis, DynamoDB, Kinesis, ledger, IAM,
  HSM, ...) packaged behind the interface of Requirement 1.
- **Composite node**: `kythira_node`, one process that runs any set of
  service modules, each on its own multi-raft host, behind one Raft
  listener.
- **Stand-alone daemon**: today's per-service daemon, rebuilt as a
  composite node with one module enabled.
- **Group space**: the range of Raft group ids a module owns on the shared
  listener.

## Requirements

### Requirement 1: Service module interface

**User Story:** As a module author, I want one way to plug a service into a
node, so the node does not need to know which services exist.

#### Acceptance Criteria

1. A `service_module` interface SHALL let a module: name itself; declare
   its environment variable prefix and parse its configuration; declare
   the group space and data subdirectory it needs; register its state
   machines and bootstrap groups on a multi-raft host; open its listeners
   or register routes on shared ones; publish in-process interfaces for
   other modules; start, drain and stop; and report health and metrics
   under its own prefix.
2. The interface SHALL live in core public headers, so modules depend only
   on what any outside consumer could use (`services/README.md`).
3. A module SHALL be registered at build time by its Kconfig switch, with
   no change to the node's code to add one.

### Requirement 2: Composite node

**User Story:** As an operator, I want one daemon and one cluster that runs
the services I choose.

#### Acceptance Criteria

1. `kythira_node` SHALL run the modules named in `KYTHIRA_SERVICES`
   (comma-separated), from those compiled in; naming one not compiled in
   SHALL be a startup error that lists the available ones.
2. All enabled modules SHALL share one Raft listener and client, one node
   identity and TLS certificate, one peer list, one metrics and health
   endpoint, and one structured logger with a `service` field. Each module
   SHALL run its own multi-raft host on that listener, so it keeps its own
   state machine, storage and transactions.
3. Each module SHALL get a disjoint group space and its own subdirectory of
   the node's data directory; the node SHALL refuse to start if two
   modules claim overlapping spaces.
4. The node SHALL start modules in dependency order (IAM before its
   consumers, the HSM before key consumers) and stop them in reverse, and
   SHALL drain listeners before stopping Raft.
5. A module whose start fails SHALL stop the node with the module named;
   no partial node SHALL serve.

### Requirement 3: Stand-alone stays as it is

**User Story:** As an existing operator, I want nothing to change unless I
choose the composite node.

#### Acceptance Criteria

1. Each existing daemon (`redis_gateway_node`, `dynamodb_gateway_node`,
   `kinesis_gateway_node`, `ledger_node`, `iam_node`) SHALL keep its name,
   environment variables, flags, ports, data directory layout and on-disk
   format, and SHALL become a composite node with its one module fixed.
2. A data directory written by a stand-alone daemon SHALL be readable by
   the same daemon after the move, with no migration step.
3. Moving a service from stand-alone to co-located SHALL be documented as a
   data copy per service (backup and restore, or export and import),
   because group ids and key prefixes change; in-place conversion is out of
   scope.
4. Every existing test, compose file and docker-chaos scenario SHALL pass
   unchanged against the rebuilt daemons.

### Requirement 4: Group spaces and the shared host

#### Acceptance Criteria

1. Group ids SHALL be partitioned by module: each module gets a fixed
   module number, and its bootstrap groups and split-child id ranges
   (DynamoDB's per-voter ranges included) SHALL sit inside that module's
   range in a composite node.
2. The placement driver, split/merge policy hooks and key-range lookups
   SHALL never mix two modules' keys: each module's keys live only in its
   own host's shard map.
3. A module's split/merge policy hook SHALL apply only to its own groups.
4. In a stand-alone daemon the module's range and prefix SHALL be the ones
   it uses today, which is what keeps 3.2 true.

### Requirement 5: IAM on the multi-raft host

**User Story:** As an operator, I want IAM to co-locate like the others.

#### Acceptance Criteria

1. IAM's state machine SHALL run as a group on the multi-raft host, so it
   can share a node.
2. `iam_node` SHALL keep its current plain-Raft data format readable, or
   provide a one-shot conversion tool, decided in the design.
3. Co-located modules SHALL authorize through the in-process IAM evaluator
   (Requirement 7), which is the IAM wiring the DynamoDB task 28 decision
   (`KYTHIRA_DYNAMODB_AUTH=static|iam|both`) and Kinesis need.

### Requirement 6: Resource isolation

**User Story:** As an operator, I want one busy service not to starve the
others in the same process.

#### Acceptance Criteria

1. Each module SHALL run its request handling on its own executor with a
   configurable thread count, so one module's backlog cannot hold another's
   threads.
2. Each module SHALL have its own disk floor and memory budget settings,
   enforced the way each module enforces them today.
3. Raft heartbeats and elections SHALL keep running under a saturated
   module; a test SHALL show it.
4. The docs SHALL state what co-location does not isolate: a crash or
   memory corruption in one module takes the whole node, and secrets in one
   module are readable from another's memory. Stand-alone is the answer
   when that matters (the virtual HSM's Requirement 2.5).

### Requirement 7: In-process calls between modules

#### Acceptance Criteria

1. A module SHALL be able to look up another co-located module's published
   interface (for example IAM's evaluator, the HSM's native client,
   Kinesis's put-records) and call it without a network hop.
2. An in-process call SHALL carry the calling module's identity and the
   original caller's principal, and SHALL be authorized as a network call
   would be.
3. When the target module is not co-located, the caller SHALL fall back to
   its configured network endpoint with no code change in the caller.
4. DynamoDB's Kinesis streaming destination SHALL use the in-process path
   when Kinesis is co-located.

### Requirement 8: Configuration

#### Acceptance Criteria

1. Node-wide settings (node id, peers, Raft bind, TLS, data directory,
   metrics) SHALL use one `KYTHIRA_NODE_*` set, and each module SHALL keep
   its own prefix for its own settings.
2. In a stand-alone daemon the module's existing variables for node-wide
   settings SHALL keep working as aliases.
3. `kythira_node --help` SHALL list the node settings and each compiled-in
   module's settings.

### Requirement 9: Packaging

#### Acceptance Criteria

1. One container image SHALL carry `kythira_node` with every module the
   build enables; the per-service images SHALL stay.
2. A compose file SHALL run a three-node composite cluster with DynamoDB,
   Kinesis and IAM, by service name, under Docker and rootless Podman.

### Requirement 10: Testing

#### Acceptance Criteria

1. An in-process test SHALL run a three-node composite cluster with every
   module compiled in, and drive each service's own smoke test against it.
2. A build matrix leg SHALL build `kythira_node` with each module alone and
   with all of them, so no module comes to depend on another being present.
3. A docker-chaos scenario SHALL kill and partition composite nodes under a
   mixed DynamoDB, Kinesis and IAM workload, under Docker and rootless
   Podman.
4. Each stand-alone daemon's existing tests SHALL run unchanged.
