# Requirements Document

## Introduction

Every performance row Kythira has ever produced is Tier A or Tier B — all hosts
inside one process, the load driver inside that same process, and the transport
either an in-process fabric or a loopback socket.
`.kiro/specs/multi-raft-performance/` Requirement 3.3 forbids publishing a
like-for-like comparison against an external number from any tier below C, so
its comparison document's like-for-like table is **empty**, and says so.

Three tiers are undelivered and all three are blocked on the same missing
thing: **a process that hosts `multi_raft` and accepts client traffic.**

| Tier | What it needs beyond today |
|---|---|
| C | one host process per node on one machine |
| D | Tier C plus a durable log — separately blocked on `.kiro/specs/durable-append-barrier/` |
| E | Tier C spread across machines or containers |

`cmd/chaos_node` is the nearest precedent and is not a substitute: it hosts a
single-group `kythira::node<RaftTypes>` over `tcp_rpc_server` with an HTTP
control plane for fault injection. It does not shard, it does not route by key,
and its HTTP surface is a control plane rather than a client-facing data path.

This spec also settles two of `.kiro/specs/multi-raft-performance/` Appendix B's
open questions, because they cannot be deferred any further:

- **Question 3, "does a benchmark host binary belong in `cmd/`?"** — yes.
- **Question 2, "how is the load driver deployed in Tiers C–E?"** — its own
  process. That is the more expensive answer and it is chosen deliberately: a
  driver sharing a process with a host makes the host's CPU and the client's
  indistinguishable in every number, which is precisely the confound these
  tiers exist to remove.

**Scope**: a `multi_raft` host binary, a client-facing key-value data path, a
separate load-driver binary, service discovery between them, and the
measurement plumbing that keeps Tier C rows comparable with the Tier B rows
already published.

**Out of scope**: durability (that is `.kiro/specs/durable-append-barrier/`),
any change to the Raft or multi-Raft algorithms, a production-grade client API
or its compatibility guarantees, and authentication on the data path beyond
what the chosen transport already provides.

## Glossary

- **Host** — one process running `multi_raft<Types, Key, GroupId>`, holding a
  replica of every shard it is assigned.
- **Driver** — the process that offers load and measures latency. Never in the
  same process as a host.
- **Data path** — the client-facing request surface: put, get, delete, routed
  by key. Distinct from `chaos_node`'s control plane.
- **Tier C/D/E** — as `.kiro/specs/multi-raft-performance/` Requirement 3.1
  defines them.

## Requirement 1 — A host binary in `cmd/`

**User Story:** As a maintainer, I want one process per node so that a
measurement can attribute CPU to a node rather than to a process containing
several, which is the whole difference between Tier B and Tier C.

### Acceptance Criteria

1. WHEN the host binary starts THEN the system SHALL run exactly one
   `multi_raft` instance and SHALL NOT host more than one node's replicas
2. WHEN the host is configured THEN the system SHALL take its node id, its
   listen address, its peers, its shard assignment and its persistence
   configuration from the command line or a configuration file, and SHALL NOT
   require a rebuild to change any of them
3. WHEN the host runs THEN the system SHALL drive `tick()` from its own thread
   at a configurable cadence, since `multi_raft` has no timer of its own and
   the tick period is a swept axis
4. WHEN the host is asked to stop THEN the system SHALL stop its groups before
   destroying them, honouring the ordering
   `tests/multi_raft_transport_harness.hpp` already documents — a host
   destroyed with a group still running terminates the process
5. WHEN the transport is selected THEN the system SHALL support the same three
   HTTP transports the Tier B matrix sweeps, chosen at runtime, so a Tier C row
   can be compared with the Tier B row that shares its transport

## Requirement 2 — A client-facing data path

**User Story:** As a driver, I want to submit a put or a get to a cluster and
get an answer, without being linked into the cluster.

### Acceptance Criteria

1. WHEN a client submits a put, get or delete THEN the system SHALL route it to
   the shard owning that key and SHALL return the result or a typed error
2. WHEN a client addresses a host that does not lead the target shard THEN the
   system SHALL return a not-leader response identifying the leader if known,
   rather than silently forwarding, so that the driver's routing cost is
   visible and measurable rather than hidden inside the cluster
3. WHEN the data path encodes a request THEN the system SHALL reuse the
   existing serializer registry rather than inventing a wire format
4. WHEN the data path is exposed THEN the system SHALL keep it separate from
   any control or fault-injection surface, so that a measurement cannot
   accidentally include control traffic
5. WHEN a read is requested THEN the system SHALL let the caller choose among
   the three read kinds `.kiro/specs/multi-raft-performance/` Requirement 2
   defines, since their costs differ by three orders of magnitude and a single
   default would make the read rows meaningless

## Requirement 3 — A driver binary, in its own process

**User Story:** As a maintainer, I want the load generator's CPU to be somebody
else's, so that a throughput number is the cluster's.

### Acceptance Criteria

1. WHEN the driver runs THEN the system SHALL NOT host any Raft node in the
   same process
2. WHEN the driver offers load THEN the system SHALL support both load modes
   `.kiro/specs/multi-raft-performance/` Requirement 4.1 defines — closed loop
   with an in-flight cap, and open loop against a fixed rate — and SHALL report
   the mode with its controlling parameter
3. WHEN the driver measures latency in open loop THEN the system SHALL measure
   from the operation's **intended** start time, preserving the
   coordinated-omission correction the in-process driver already makes, and
   SHALL report the mean schedule lag
4. WHEN the driver reports a result THEN the system SHALL emit the same fields
   the Tier B rows carry, so the two are readable side by side
5. WHEN the driver is deployed on a separate machine THEN the system SHALL
   record which machine, since a Tier E number without the driver's placement
   is not reproducible

## Requirement 4 — The measurement stays comparable

**User Story:** As a reader, I want a Tier C row and a Tier B row to differ only
in the tier, so the delta means something.

### Acceptance Criteria

1. WHEN workload generation is implemented THEN the system SHALL share the key
   sampler, value-size handling, distribution and command mix with
   `tests/multi_raft_kv_workload.hpp` rather than reimplementing them — a
   second implementation of the workload makes every cross-tier comparison a
   comparison of two workloads
2. WHEN a result is judged THEN the system SHALL use the same statistical
   method: five repetitions, the spread rule, and a verdict that refuses a
   headline to an unstable row
3. WHEN a row is emitted THEN the system SHALL label it with its tier, and
   SHALL state at the point of the number whether that tier admits a
   like-for-like external comparison
4. WHEN the shard space is created THEN the system SHALL pre-split into N
   ranges rather than growing by automatic split, answering Appendix B's
   question 4 the reproducible way, and SHALL record that choice on the row
5. IF the driver and the in-process harness ever disagree on a row they both
   can run THEN the system SHALL treat that as a defect in one of them rather
   than as a tier effect

## Requirement 5 — Service discovery and placement

**User Story:** As an operator standing up Tier E, I want the hosts to find each
other without me editing a file on each one.

### Acceptance Criteria

1. WHEN hosts start THEN the system SHALL support a static peer list, since it
   is the reproducible baseline and Tier C needs nothing more
2. WHEN Tier E runs THEN the system SHALL support discovery that does not
   require every host to know every address in advance, reusing the discovery
   mechanisms this project already has rather than adding one
3. WHEN hosts are placed THEN the system SHALL honour `CLAUDE.md`'s container
   rules — no static IPs in compose files, no hardcoded `docker`, no privileged
   networking — since Tier E's harness is `tests/docker_chaos/`
4. WHEN a Tier E measurement is taken THEN the system SHALL measure and report
   inter-node round-trip time and bandwidth **before** the measured window, and
   the placement, as
   `.kiro/specs/multi-raft-performance/` Requirement 18.8 requires

## Requirement 6 — It does not become a product

**User Story:** As a maintainer, I want a measurement tool, not an unowned
server that someone deploys.

### Acceptance Criteria

1. WHEN the host binary is documented THEN the system SHALL state that it is a
   measurement and testing host, not a supported server
2. WHEN the data path is designed THEN the system SHALL NOT add authentication,
   authorisation, multi-tenancy or rate limiting, and SHALL say why not
3. WHEN the binary is registered THEN the system SHALL keep it out of any
   default install target
