# Requirements Document

## Introduction

This document specifies the requirements for the `quorum_manager` concept and its
supporting types in Kythira. The concept models an infrastructure-level component
that can observe cluster health and drive autonomous remediation: provisioning
replacement nodes and decommissioning broken ones.

The primary motivation is cloud-hosted deployments where auto-provisioning is
possible — for example, AWS EC2, GCP Compute Engine, or any platform with an
API for creating and terminating instances. In such environments a cluster can
self-heal from individual node failures and from an entire availability zone
going down, without operator intervention, as long as a component conforming to
`quorum_manager` is configured.

The concept is explicitly environment-agnostic. Concrete implementations are
provided by the user (e.g. `Ec2QuorumManager`, `GceQuorumManager`); this spec
defines only the interface that the Raft orchestration layer calls and the
supporting vocabulary types it works with.

The concept is defined in `include/raft/quorum_management.hpp`. A
`no_op_quorum_manager` default is provided for static clusters and development
environments where auto-provisioning is not available.

## Glossary

- **Quorum manager**: A component satisfying the `quorum_manager<Q, NodeId,
  Address, GroupId>` concept. Responsible for assessing cluster health and
  driving infrastructure changes.
- **Placement group**: A logical failure domain — availability zone, physical
  rack, network spine, etc. — that groups nodes sharing a common failure mode.
  Nodes in the same placement group are expected to fail together.
- **`GroupId`**: The type used to identify a placement group. Must satisfy
  `placement_group_id` (regular, totally ordered). Typical values: `std::string`
  for AZ names like `"us-east-1a"`, `std::uint32_t` for rack IDs.
- **`node_placement<NodeId, GroupId>`**: A struct pairing a node identity with
  its placement group. Passed to `assess_quorum` by the orchestrator so that the
  quorum manager can produce per-group health reports without maintaining internal
  state.
- **`desired_topology<GroupId>`**: A vector of `placement_group_target` records
  declaring the target node count for each placement group. The orchestrator
  compares this against live counts from `assess_quorum` to decide whether and
  where to provision replacements.
- **`quorum_health<NodeId, GroupId>`**: The assessment report produced by
  `assess_quorum`. Contains a global `quorum_status`, aggregate live/total
  counts, a list of unreachable node IDs, and a per-group health breakdown.
- **`quorum_status`**: A severity-ordered enum: `healthy` → `degraded` →
  `critical` → `lost`.
  - `healthy`: All nodes responsive, cluster at desired size.
  - `degraded`: Quorum intact but below desired size (replacement needed).
  - `critical`: One additional failure would lose quorum.
  - `lost`: Majority unreachable; cluster cannot make progress.
- **`no_op_quorum_manager`**: The default implementation for environments without
  auto-provisioning. Assumes all supplied nodes are live; rejects `provision_node`
  with an exceptional Future; treats `decommission_node` as a no-op.
- **Orchestration layer**: The component (outside the Raft node itself) that
  periodically calls `assess_quorum`, interprets the result, and invokes
  `provision_node` or `decommission_node` to heal the cluster.

## Requirements

### Requirement 1: `placement_group_id` Concept

**User Story:** As a library user, I want a clean compile-time constraint on
placement group identifier types so that I get a clear error if I accidentally
use an incompatible type.

#### Acceptance Criteria

1. A `placement_group_id<T>` concept SHALL require that `T` satisfies
   `std::regular<T>` (default constructible, copy/move constructible and
   assignable, equality comparable) and `std::totally_ordered<T>`.
2. Both `std::string` and `std::uint64_t` SHALL satisfy `placement_group_id`,
   verified by `static_assert`s in the header.
3. The concept SHALL be defined in `include/raft/quorum_management.hpp`.

### Requirement 2: Vocabulary Types

**User Story:** As a library user writing an orchestrator, I need concrete types
for expressing cluster topology and assessment results so that I can implement
the orchestration loop without defining my own structs.

#### Acceptance Criteria

1. `node_placement<NodeId, GroupId>` SHALL be a struct with `node_id` and
   `group_id` fields. It SHALL require `node_id<NodeId>` and
   `placement_group_id<GroupId>`.
2. `placement_group_target<GroupId>` SHALL be a struct with `group_id` and
   `target_count` fields. It SHALL require `placement_group_id<GroupId>`.
3. `desired_topology<GroupId>` SHALL be a struct containing a
   `std::vector<placement_group_target<GroupId>>` named `groups`, and a
   `total_size()` const method returning the sum of all `target_count` values.
4. `placement_group_health<NodeId, GroupId>` SHALL be a struct with:
   - `group_id`: the placement group this record describes.
   - `live_count`: nodes in this group that were found responsive.
   - `target_count`: the target from `desired_topology` for this group.
   - `unreachable_nodes`: node IDs in this group that did not respond.
5. `quorum_health<NodeId, GroupId>` SHALL be a struct with:
   - `status`: the overall `quorum_status`.
   - `live_node_count`: total live nodes across all groups.
   - `total_node_count`: total nodes supplied to `assess_quorum`.
   - `unreachable_nodes`: all non-responding node IDs regardless of group.
   - `groups`: a `std::vector<placement_group_health<NodeId, GroupId>>`
     providing the per-group breakdown.
6. All types SHALL be defined in `include/raft/quorum_management.hpp`.

### Requirement 3: `quorum_manager` Concept

**User Story:** As a library user, I want a compile-time concept that pins down
the exact interface a quorum manager must expose so that I can write an
implementation and have the compiler verify it is correct before wiring it into
my cluster.

#### Acceptance Criteria

1. `quorum_manager<Q, NodeId, Address, GroupId>` SHALL require that `Q` provides:
   - `typename Q::node_id_type` equal to `NodeId`.
   - `typename Q::address_type` equal to `Address`.
   - `typename Q::placement_group_id_type` equal to `GroupId`.
   - `assess_quorum(const std::vector<node_placement<NodeId, GroupId>>&)`
     returning `kythira::Future<quorum_health<NodeId, GroupId>>`.
   - `provision_node(GroupId target_group, std::optional<NodeId> replacing)`
     returning `kythira::Future<peer_info<NodeId, Address>>`.
   - `decommission_node(const NodeId&)` returning `kythira::Future<void>`.
   - `topology() const` returning `desired_topology<GroupId>`.
2. The concept SHALL be defined in `include/raft/quorum_management.hpp`.
3. A `static_assert` SHALL verify that `no_op_quorum_manager` satisfies the
   concept.

### Requirement 4: `assess_quorum` Semantics

**User Story:** As an orchestrator, I need to call `assess_quorum` with the
current cluster membership annotated with placement groups so that I receive a
complete picture of cluster health — both globally and per placement group —
which I can use to decide what remediation action to take.

#### Acceptance Criteria

1. `assess_quorum` SHALL accept the full cluster membership as
   `const std::vector<node_placement<NodeId, GroupId>>&`, where the vector
   contains every node the orchestrator considers part of the configuration,
   live and suspected-dead alike.
2. The orchestrator SHALL supply the `group_id` for each node because it tracked
   which `target_group` was specified when `provision_node` was called. The
   quorum manager SHALL NOT be required to maintain internal state about node
   placement.
3. The returned `quorum_health` SHALL include a `placement_group_health` entry
   for every distinct `group_id` that appears in the supplied cluster vector.
4. The `quorum_status` in the returned health SHALL reflect the global state of
   the cluster:
   - `healthy` when all nodes respond and every group meets its target count.
   - `degraded` when quorum is intact but one or more groups are below their
     target count (replacement needed, but no immediate risk).
   - `critical` when one additional failure would cause quorum loss.
   - `lost` when a majority of nodes are unreachable and the cluster cannot
     make progress.
5. The implementation decides how to probe liveness (health-check endpoint,
   cloud API describe-instance, ICMP ping, etc.); the concept does not constrain
   the mechanism.

### Requirement 5: `provision_node` Semantics

**User Story:** As an orchestrator that has detected a degraded placement group,
I need to request a new node in a specific placement group so that the cluster
returns to its target topology and failure-domain diversity is preserved.

#### Acceptance Criteria

1. `provision_node(GroupId target_group, std::optional<NodeId> replacing)` SHALL
   provision a new node and return its `peer_info` (node ID and reachable
   address) once the node is ready to receive a `ClusterJoin` request.
2. `target_group` SHALL be required (not optional): the orchestrator always knows
   which group needs a replacement because it obtained that information from
   `assess_quorum` and `topology()`.
3. The optional `replacing` parameter, when set, SHALL name the node being
   replaced. Implementations MAY use it to copy instance type, subnet, tags, or
   other properties of the replaced node. When `nullopt`, the implementation
   SHALL choose placement within `target_group` freely.
4. WHEN provisioning is not supported (e.g. `no_op_quorum_manager`) THEN
   `provision_node` SHALL return an exceptional Future. The orchestrator MUST
   handle this failure.
5. WHEN the infrastructure call fails (API error, quota exceeded, etc.) THEN
   `provision_node` SHALL return an exceptional Future with an appropriate
   exception.

### Requirement 6: `decommission_node` Semantics

**User Story:** As an orchestrator removing a broken node from the cluster, I
need to terminate or deregister the node through the quorum manager so that it
cannot rejoin as a ghost and corrupt the cluster state.

#### Acceptance Criteria

1. `decommission_node(const NodeId& node)` SHALL terminate and/or deregister the
   named node at the infrastructure level (e.g. terminate the EC2 instance,
   remove the GCE instance, revoke the TLS certificate).
2. `decommission_node` SHALL be idempotent: calling it on a node that is already
   gone or was never provisioned through this manager SHALL return a
   successfully-resolved Future, not an error.
3. `decommission_node` does not remove the node from the Raft cluster
   configuration — that is done by the Raft `remove_server()` / `ClusterLeave`
   path. `decommission_node` operates at the infrastructure layer only.

### Requirement 7: `topology()` Semantics

**User Story:** As an orchestrator, I need to query the quorum manager's target
topology so that I can compare it against the live topology from `assess_quorum`
and determine how many replacements are needed in each placement group.

#### Acceptance Criteria

1. `topology() const` SHALL return a `desired_topology<GroupId>` that describes
   the target number of nodes per placement group.
2. `topology()` SHALL be synchronous: it is a pure policy value with no I/O.
3. The `desired_topology` returned by `topology()` SHALL be consistent with the
   `target_count` values in `placement_group_health` records returned by
   `assess_quorum`, allowing the orchestrator to compute per-group deficits as
   `target_count - live_count` without a separate lookup.

### Requirement 8: `no_op_quorum_manager` Default

**User Story:** As a developer running a static cluster or a test environment
where auto-provisioning is not available, I want a default quorum manager that
compiles and satisfies the concept without requiring any infrastructure
integration.

#### Acceptance Criteria

1. `no_op_quorum_manager<NodeId, Address, GroupId>` SHALL satisfy
   `quorum_manager<no_op_quorum_manager<NodeId, Address, GroupId>, NodeId,
   Address, GroupId>`, verified by a `static_assert` in the header.
2. `no_op_quorum_manager` SHALL be constructible from a `desired_topology<GroupId>`;
   default construction (empty topology) SHALL be permitted.
3. `assess_quorum` in `no_op_quorum_manager` SHALL assume all supplied nodes are
   live and compute per-group counts from the supplied `node_placement` vector.
   It SHALL NOT attempt any network probing.
4. `provision_node` in `no_op_quorum_manager` SHALL always return an exceptional
   Future with an `std::runtime_error` indicating provisioning is not supported.
5. `decommission_node` in `no_op_quorum_manager` SHALL return an
   immediately-resolved Future (no-op).
6. `topology()` in `no_op_quorum_manager` SHALL return the `desired_topology`
   passed at construction.

### Requirement 9: AZ-Loss Healing

**User Story:** As an operator of a multi-AZ cluster, I want the orchestration
layer to recover from an entire availability zone going down by provisioning
replacement nodes in the surviving AZs so that quorum is restored without manual
intervention.

This requirement constrains the orchestrator's use of the quorum manager rather
than the quorum manager interface itself, but it defines the scenarios the
interface must support.

#### Acceptance Criteria

1. WHEN `assess_quorum` returns `quorum_status::degraded` or `critical` AND one
   or more groups have `live_count < target_count` THEN the orchestrator SHALL
   call `provision_node` once for each missing node, targeting the group with the
   deficit.
2. WHEN an entire placement group's nodes are unreachable (a complete AZ
   failure) AND the remaining groups still hold a quorum majority THEN the
   orchestrator SHALL provision replacements in the surviving groups rather than
   in the failed group, to avoid provisioning into a zone that may itself be
   unavailable.
3. WHEN `quorum_status::lost` is returned THEN the orchestrator SHALL NOT
   attempt to provision new nodes autonomously, because doing so risks creating
   a split-brain cluster. Operator intervention is required.
4. WHEN a replacement node is successfully provisioned THEN the orchestrator
   SHALL record the `(NodeId, GroupId)` pair so that it can be supplied to
   future `assess_quorum` calls.

### Requirement 10: Tests

**User Story:** As a developer, I need automated tests for the `quorum_manager`
concept and the `no_op_quorum_manager` implementation so that regressions are
caught before they reach production.

#### Acceptance Criteria

1. A concept property test SHALL verify that `no_op_quorum_manager<uint64_t,
   std::string, std::string>` satisfies `quorum_manager`.
2. A unit test SHALL verify that `no_op_quorum_manager::assess_quorum` returns
   `quorum_status::healthy` and a `quorum_health` whose `live_node_count` and
   `total_node_count` equal the size of the supplied cluster vector.
3. A unit test SHALL verify that `no_op_quorum_manager::assess_quorum` produces
   a `placement_group_health` entry for each distinct `group_id` in the supplied
   `node_placement` vector, with `live_count` equal to the number of nodes in
   that group.
4. A unit test SHALL verify that `no_op_quorum_manager::provision_node` returns
   an exceptional Future.
5. A unit test SHALL verify that `no_op_quorum_manager::decommission_node`
   returns a successfully-resolved Future (no-op).
6. A unit test SHALL verify that `no_op_quorum_manager::topology().total_size()`
   equals the sum of `target_count` values across all groups in the constructed
   topology.
7. All existing tests SHALL pass without modification after adding
   `include/raft/quorum_management.hpp`.

### Requirement 11: `quorum_manager_type` in `raft_types` and `raft_configuration`

**User Story:** As a library user, I want to wire a quorum manager into my Raft
node the same way I wire in a `membership_manager` or `peer_discovery`, so that
the node drives cluster healing automatically without any external orchestration
process.

#### Acceptance Criteria

1. The `raft_types` concept SHALL gain a `quorum_manager_type` member.  The
   concept SHALL require that `quorum_manager_type` satisfies
   `quorum_manager<quorum_manager_type, node_id_type, address_type,
   quorum_manager_type::placement_group_id_type>`.
2. `default_raft_types` SHALL set `quorum_manager_type` to
   `no_op_quorum_manager<node_id_type, address_type, std::string>`.
3. The `node` constructor SHALL accept a `quorum_manager_type` instance.  When
   none is supplied, a default-constructed `no_op_quorum_manager` SHALL be used,
   preserving backwards compatibility.
4. `raft_configuration` and `raft_configuration_type` SHALL gain two new fields:
   - `quorum_check_interval` (`std::chrono::milliseconds`, default `30 s`): how
     often the leader proactively calls `assess_quorum` when no heartbeat
     failures have been observed.
   - `quorum_heartbeat_failure_threshold` (`std::size_t`, default `3`): the
     number of consecutive heartbeat failures to a single peer that triggers an
     immediate out-of-cycle `assess_quorum` call.
5. Both new fields SHALL be validated by `raft_configuration::validate()`:
   `quorum_check_interval` must be positive; `quorum_heartbeat_failure_threshold`
   must be ≥ 1.
6. All existing tests SHALL pass without modification after these additions.

### Requirement 12: Initial Placement Group Assignment

**User Story:** As a library user configuring a multi-AZ cluster, I need a way
to tell the node which placement group each bootstrap member belongs to so that
the leader can produce accurate per-group health reports from the very first
assessment cycle.

#### Acceptance Criteria

1. The `node` constructor SHALL accept an optional initial placement map:
   `std::unordered_map<node_id_type, placement_group_id_type>` (or equivalent).
   When omitted, all bootstrap members are treated as belonging to a single
   default placement group whose `group_id` is `placement_group_id_type{}`.
2. The placement map SHALL be mutable between construction and the first call to
   `start()`.  A method `set_placement(node_id_type, placement_group_id_type)`
   (or equivalent) SHALL allow entries to be added or updated before the node
   starts.
3. WHEN `add_server()` completes successfully for a node that was provisioned via
   `provision_node(target_group, replacing)` THEN the node SHALL record
   `(new_node_id, target_group)` in the placement map automatically; no external
   caller is required to register the placement.
4. WHEN `remove_server()` completes successfully THEN the corresponding entry
   SHALL be removed from the placement map.
5. Placement map entries for nodes that are not in the current cluster
   configuration SHALL be silently ignored when building the cluster vector for
   `assess_quorum`.

### Requirement 13: Leader Quorum Assessment Loop

**User Story:** As a Raft leader, I need to periodically assess quorum health
and react to peer failures so that cluster degradation is detected and remediated
automatically while I hold leadership.

#### Acceptance Criteria

1. WHEN a node transitions to leader state (via `become_leader()`) THEN it SHALL
   start the quorum assessment loop.
2. WHEN a node transitions out of leader state (to follower or candidate) THEN
   it SHALL stop the quorum assessment loop and cancel any in-flight
   `assess_quorum` futures.
3. The assessment loop SHALL call `assess_quorum` on two independent triggers:
   a. **Timer trigger**: when `quorum_check_interval` elapses since the last
      assessment, whether or not any heartbeat failures have occurred.
   b. **Failure trigger**: when the per-peer consecutive-heartbeat-failure count
      for any single peer reaches `quorum_heartbeat_failure_threshold`, the loop
      SHALL call `assess_quorum` immediately without waiting for the timer.
4. WHEN `assess_quorum` is called THEN the leader SHALL build the cluster vector
   by iterating `_configuration.nodes()` and pairing each node ID with its entry
   from the placement map.  Nodes absent from the placement map SHALL be assigned
   the default group `placement_group_id_type{}`.
5. WHEN `assess_quorum` returns an exceptional Future THEN the leader SHALL log
   the error and schedule a retry after `quorum_check_interval`.  It SHALL NOT
   change cluster membership or call `provision_node` on the basis of a failed
   assessment.
6. WHEN `quorum_manager_type` is `no_op_quorum_manager` THEN `assess_quorum`
   always resolves to `quorum_status::healthy`.  The assessment loop SHALL still
   run (it is not suppressed) but no remediation actions will be triggered.

### Requirement 14: Leader-Driven Node Provisioning

**User Story:** As a Raft leader that has received a `degraded` or `critical`
quorum health report, I need to provision replacement nodes automatically so
that the cluster returns to its target topology without operator intervention.

#### Acceptance Criteria

1. WHEN `assess_quorum` returns `quorum_status::degraded` or `critical` AND one
   or more `placement_group_health` entries have `live_count < target_count`
   THEN for each such group, the leader SHALL call
   `provision_node(group_id, replacing)` once per missing node slot, where
   `replacing` is the `node_id` of one of the group's `unreachable_nodes` if
   any exist, or `std::nullopt` otherwise.
2. WHEN `quorum_status::lost` is returned THEN the leader SHALL NOT call
   `provision_node`.  A leader that cannot maintain quorum has already or will
   shortly step down; autonomous provisioning in this state risks split-brain.
3. The leader SHALL track pending provisions to avoid over-provisioning: a
   (group, slot) pair for which a `provision_node` call is already in flight or
   has returned a `peer_info` that has not yet joined SHALL NOT trigger a second
   `provision_node` call, even if the next assessment cycle still shows that
   slot as missing.
4. WHEN `provision_node` returns a successfully-resolved Future with a
   `peer_info` THEN the leader SHALL do nothing further itself: the provisioned
   node is expected to contact a seed peer, discover the cluster via
   `peer_discovery`, and join via the standard `ClusterJoin` → `add_server` flow
   (Requirement 3 of the node-bootstrap spec).
5. WHEN the `add_server()` that corresponds to a pending provision completes
   successfully THEN the pending provision record for that slot SHALL be cleared.
6. WHEN `provision_node` returns an exceptional Future THEN the leader SHALL log
   the failure, clear the pending provision for that slot, and allow the next
   assessment cycle to retry.
7. WHEN the leader steps down while a `provision_node` Future is in flight THEN
   the returned `peer_info` (if any) SHALL be discarded.  The provisioned
   node will still attempt to join; whichever node is leader at that point will
   handle its `ClusterJoin` request normally.

### Requirement 15: Leader-Driven Node Decommissioning

**User Story:** As a Raft leader that has successfully added a replacement for a
failed node, I need to remove the failed node from both the Raft cluster
configuration and the underlying infrastructure so that it cannot rejoin as a
ghost and the infrastructure is not paying for a permanently broken instance.

#### Acceptance Criteria

1. WHEN `add_server()` for a replacement node completes successfully AND
   `provision_node` was called with a non-`nullopt` `replacing` argument THEN
   the leader SHALL call `remove_server(replacing)` to remove the failed node
   from the Raft cluster configuration.
2. WHEN `remove_server(replacing)` completes successfully THEN the leader SHALL
   call `decommission_node(replacing)` on the quorum manager.
3. The order SHALL be `remove_server` first, then `decommission_node`: the
   node must be out of the Raft configuration before its infrastructure
   resource is terminated, to ensure it cannot receive AppendEntries and
   corrupt its log after the decommission.
4. WHEN `decommission_node` returns an exceptional Future THEN the leader SHALL
   log the failure.  The Raft configuration change has already completed and
   SHALL NOT be reversed.  The operator may need to clean up the infrastructure
   resource manually.
5. WHEN a node is removed from the cluster via an operator-initiated
   `remove_server()` call (i.e., not as part of an auto-provisioning flow)
   THEN the leader SHALL NOT call `decommission_node` automatically.
   Decommissioning in the auto-provisioning flow is coupled exclusively to the
   provisioning path in Requirements 14–15.

### Requirement 16: Tests for Leader Integration

**User Story:** As a developer, I need automated tests that exercise the leader's
interaction with the quorum manager so that regressions in the healing loop are
caught before they reach production.

#### Acceptance Criteria

1. A unit test SHALL verify that `become_leader()` starts the assessment loop
   and that stepping down to follower stops it, using a mock quorum manager
   that records calls to `assess_quorum`.
2. A unit test SHALL verify that the leader calls `assess_quorum` after
   `quorum_check_interval` elapses with a cluster vector containing every
   current configuration member paired with its correct placement group.
3. A unit test SHALL verify that when the per-peer consecutive-heartbeat-failure
   count reaches `quorum_heartbeat_failure_threshold`, `assess_quorum` is called
   immediately without waiting for the next timer tick.
4. A unit test SHALL verify that when `assess_quorum` returns `degraded` with
   one group below target count, the leader calls `provision_node` exactly once
   for that group, and does not call it again on the next assessment while the
   provision is pending.
5. A unit test SHALL verify that when `assess_quorum` returns `lost`, the leader
   does not call `provision_node`.
6. A unit test SHALL verify that after a provisioned node completes `add_server`,
   the leader records `(node_id, group_id)` in the placement map so that
   subsequent `assess_quorum` calls include the new node with the correct group.
7. A unit test SHALL verify that the leader calls `remove_server` and then
   `decommission_node` after a replacement joins, in that order, and that a
   failure of `decommission_node` does not reverse the `remove_server`.
8. A unit test SHALL verify that an operator-initiated `remove_server()` call
   does not trigger `decommission_node`.
9. A property test SHALL verify that a three-node cluster with one node
   permanently failing (simulated via the network simulator) self-heals to three
   live nodes when configured with a mock quorum manager that can provision
   nodes, without any operator intervention.
10. All existing tests SHALL pass without modification.

### Requirement 18: `docker_quorum_manager` Example Implementation

**User Story:** As a developer running a Kythira cluster inside Docker (local
development, CI, or a single-host deployment), I want a ready-made quorum
manager that talks to the local Docker daemon so that the leader can
automatically replace crashed containers without any external orchestration
tool.

The Docker manager uses a **single placement group**: all containers belong to
the same failure domain.  It communicates with the Docker Engine REST API
through the Unix domain socket at `/var/run/docker.sock` (or a configurable
TCP endpoint) using the cpp-httplib client already present in the project.
Containers are identified by a Docker label (`kythira.node_id`) so the manager
can enumerate and inspect them independently of node IDs known to the Raft
layer.

#### Acceptance Criteria

##### Configuration

1. A `docker_quorum_manager_config` struct SHALL be provided with the following
   fields and defaults:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `daemon_url` | `std::string` | `"unix:///var/run/docker.sock"` | Docker daemon endpoint |
   | `image` | `std::string` | *(required)* | Image to run for new nodes |
   | `cluster_name` | `std::string` | *(required)* | Scopes labels and container names to this cluster |
   | `network_name` | `std::string` | *(required)* | Docker network containers are attached to |
   | `node_port` | `std::uint16_t` | `7000` | Port the Raft node process listens on inside the container |
   | `group_id` | `std::string` | `"default"` | The single placement group name returned by `topology()` |
   | `target_count` | `std::size_t` | `3` | Desired number of nodes (fed into `desired_topology`) |
   | `api_timeout` | `std::chrono::milliseconds` | `5000 ms` | Timeout for each Docker API call |
   | `extra_env` | `std::vector<std::string>` | `{}` | Additional `KEY=VALUE` environment variables injected into every new container |
   | `extra_args` | `std::vector<std::string>` | `{}` | Additional command-line arguments appended to the container `CMD` |

2. `docker_quorum_manager_config` SHALL be validated at construction of
   `docker_quorum_manager`: `image`, `cluster_name`, and `network_name` must be
   non-empty; `target_count` must be ≥ 1; `node_port` must be non-zero.
   Violations SHALL throw `std::invalid_argument`.

##### Class interface

3. `docker_quorum_manager<NodeId, Address>` SHALL satisfy
   `quorum_manager<docker_quorum_manager<NodeId, Address>, NodeId, Address,
   std::string>` with `placement_group_id_type = std::string`, verified by a
   `static_assert` in its header.
4. The class SHALL be defined in `include/raft/docker_quorum_manager.hpp`.

##### Container naming and labelling

5. Every container managed by an instance SHALL be named
   `kythira-{cluster_name}-{node_id}` and SHALL carry the Docker labels:
   - `kythira.cluster={cluster_name}`
   - `kythira.node_id={node_id}` (serialised with `std::to_string` for integer
     `NodeId`; used as-is for `std::string` `NodeId`)
6. These labels allow the manager to enumerate all nodes it owns with a single
   filtered `GET /containers/json` call, and to resolve a `NodeId` to a
   container name without maintaining any in-process state that would be lost on
   manager restart.

##### `assess_quorum`

7. `assess_quorum` SHALL, for each `node_placement` in the supplied `cluster`
   vector:
   - Send `GET /containers/kythira-{cluster_name}-{node_id}/json` to the Docker
     daemon.
   - Consider the node **live** when the response is HTTP 200 and
     `State.Status == "running"`.
   - Consider the node **unreachable** when the response is HTTP 404 (container
     gone), when `State.Status` is anything other than `"running"`, or when the
     API call exceeds `api_timeout`.
8. All nodes SHALL be placed in the single group named `config.group_id`;
   `target_count` for that group SHALL equal `config.target_count`.
9. The overall `quorum_status` SHALL be derived from the ratio of live to total
   nodes using the standard mapping from Requirement 4.4, applied to the single
   group.
10. WHEN the Docker daemon is unreachable (connection refused, socket not found)
    THEN `assess_quorum` SHALL return an exceptional Future rather than silently
    reporting all nodes as healthy.

##### `provision_node`

11. `provision_node` SHALL determine the new node's ID by querying
    `GET /containers/json?filters={"label":["kythira.cluster={cluster_name}"]}`,
    finding the highest numeric `kythira.node_id` label value among returned
    containers, and using that value plus one.  If no containers exist yet, the
    starting ID SHALL be `1`.
12. `provision_node` SHALL create the container via
    `POST /containers/create?name=kythira-{cluster_name}-{new_id}` with:
    - `Image`: `config.image`
    - `Labels`: the two labels from Acceptance Criterion 5.
    - `Env`: at minimum `KYTHIRA_NODE_ID={new_id}`,
      `KYTHIRA_NODE_PORT={node_port}`, `KYTHIRA_CLUSTER={cluster_name}`, plus
      any entries from `config.extra_env`.
    - `NetworkingConfig.EndpointsConfig.{network_name}`: attaches the container
      to the configured Docker network.
13. `provision_node` SHALL start the container via
    `POST /containers/kythira-{cluster_name}-{new_id}/start` and return
    `peer_info{new_id, "kythira-{cluster_name}-{new_id}:{node_port}"}`.
    The address uses the container hostname, which Docker's embedded DNS
    resolves to the container IP for other containers on the same network.
14. WHEN `create` or `start` fails (non-2xx response) THEN `provision_node`
    SHALL attempt to remove any partially created container via
    `DELETE /containers/kythira-{cluster_name}-{new_id}?force=true` before
    returning an exceptional Future, to avoid leaving orphaned containers.
15. The `target_group` and `replacing` arguments SHALL be accepted but the
    single-group implementation does not change behaviour based on their values.
    `replacing` MAY be used for logging only.

##### `decommission_node`

16. `decommission_node` SHALL send
    `DELETE /containers/kythira-{cluster_name}-{node_id}?force=true` to the
    Docker daemon.
17. WHEN the response is HTTP 404 THEN `decommission_node` SHALL return a
    successfully-resolved Future (idempotent — container already gone).
18. WHEN the response is any other non-2xx code THEN `decommission_node` SHALL
    return an exceptional Future.

##### `topology()`

19. `topology()` SHALL return
    `desired_topology<std::string>{ .groups = {{ .group_id = config.group_id,
    .target_count = config.target_count }} }`.

##### Tests

20. A unit test SHALL verify that `docker_quorum_manager` satisfies the
    `quorum_manager` concept (`static_assert`).
21. An integration test using a real Docker daemon (guarded by
    `KYTHIRA_DOCKER_INTEGRATION_TESTS=1`) SHALL:
    a. Construct a `docker_quorum_manager` targeting a test cluster name.
    b. Call `provision_node` three times and verify three containers appear with
       the correct labels and names.
    c. Stop one container manually and call `assess_quorum`; verify
       `quorum_status::degraded` and one unreachable node.
    d. Call `decommission_node` on each provisioned node and verify the
       containers are removed.
    e. Call `decommission_node` on an already-removed node and verify success
       (idempotency).
22. A unit test using a mock HTTP server SHALL verify that `assess_quorum`
    returns an exceptional Future when the Docker daemon socket is unreachable.

### Requirement 19: Docker Quorum Manager Host Failure Simulation Tests

**User Story:** As a maintainer, I want end-to-end tests that kill, stop, and
pause real containers while a `docker_quorum_manager`-equipped leader is
running, so that the full self-healing loop — failure detection, provisioning,
membership change, and decommissioning — is verified against real Docker
infrastructure rather than mocks.

These tests extend the existing Docker chaos harness in
`tests/docker_chaos/` and follow the same patterns: Boost.Test with a
`ChaosFixture`-derived fixture, `docker kill`/`stop`/`pause`/`unpause`
via the `ChaosNode` API, and `cluster.assert_no_split_brain()` for safety
assertions.

#### Acceptance Criteria

##### Infrastructure additions

1. A new Docker Compose variant `docker/docker-compose.quorum.yml` SHALL
   define a 3-node cluster in which each `chaos_node` container:
   - Mounts `/var/run/docker.sock:/var/run/docker.sock` so that the container
     running the Raft leader can call the Docker daemon directly.
   - Receives the additional environment variables:
     `QUORUM_MANAGER=docker`, `QUORUM_IMAGE=kythira-chaos-node:dev`,
     `QUORUM_CLUSTER=kythira-quorum-test`,
     `QUORUM_NETWORK=kythira-quorum-net`, `QUORUM_TARGET=3`.
2. The `chaos_node` binary SHALL read these variables at startup and, when
   `QUORUM_MANAGER=docker`, construct a `docker_quorum_manager` and pass it
   as `quorum_manager` in `node_config`. When `QUORUM_MANAGER` is absent or
   any other value, the existing `no_op_quorum_manager` default SHALL be used.
3. A `QuorumHealingFixture` SHALL be provided in a new header
   `tests/docker_chaos/quorum_harness.hpp` that:
   - Wraps `ChaosFixture` and starts the cluster from
     `docker-compose.quorum.yml`.
   - Adds `pause(ChaosNode&)` and `unpause(ChaosNode&)` methods that run
     `docker pause` / `docker unpause` via the `CmdExecutor` in `os_faults.hpp`.
   - Adds `wait_for_cluster_size(std::size_t n, std::chrono::milliseconds timeout)`
     that polls the Docker daemon (via `docker ps --filter label=kythira.cluster`)
     until exactly `n` containers are in `running` state or the timeout expires.
   - Adds `assert_container_absent(node_id_type)` that verifies
     `kythira-kythira-quorum-test-{node_id}` does not exist (i.e., was
     decommissioned).

##### Test scenarios

The tests SHALL be placed in `tests/docker_chaos/quorum_healing_test.cpp`,
guarded by `KYTHIRA_DOCKER_INTEGRATION_TESTS=1`, and registered as a CTest
target `quorum_healing_tests`.

4. **`follower_kill_heals_to_target`**
   - Start a 3-node cluster; wait for a stable leader.
   - `kill()` one follower container.
   - Wait up to `quorum_check_interval + 30 s` for `wait_for_cluster_size(3)`.
   - Assert:
     - Three containers are running.
     - The killed container's node ID is absent from the cluster
       (`assert_container_absent`).
     - A new container with a different node ID is present.
     - `cluster.assert_no_split_brain()` passes.

5. **`follower_stop_heals_to_target`**
   - Start a 3-node cluster; wait for a stable leader.
   - `stop()` one follower (graceful `SIGTERM`).
   - Wait up to `quorum_check_interval + 30 s` for `wait_for_cluster_size(3)`.
   - Assert the stopped container was decommissioned (force-removed) and a new
     container replaced it.
   - Assert no split brain.

6. **`leader_kill_new_leader_heals`**
   - Start a 3-node cluster; identify the leader.
   - `kill()` the leader container.
   - Wait for a new leader to be elected (up to `3 × election_timeout_max`).
   - Wait up to `quorum_check_interval + 30 s` for `wait_for_cluster_size(3)`.
   - Assert the new leader is one of the original surviving followers.
   - Assert the killed container's node ID was decommissioned.
   - Assert no split brain.

7. **`transient_pause_below_threshold_no_replacement`**
   - Start a 3-node cluster; wait for a stable leader.
   - Record the current running container IDs.
   - `pause()` one follower.
   - Wait for `(quorum_heartbeat_failure_threshold - 1) × heartbeat_interval`.
   - `unpause()` the follower.
   - Wait `2 × heartbeat_interval` for reconnection.
   - Assert `wait_for_cluster_size(3)` with the **same three container IDs** as
     at the start — no new container was provisioned.
   - Assert no split brain.

8. **`sustained_pause_triggers_replacement`**
   - Start a 3-node cluster; wait for a stable leader.
   - `pause()` one follower.
   - Wait for `quorum_check_interval + quorum_heartbeat_failure_threshold ×
     heartbeat_interval + 10 s` (long enough for assessment and provisioning).
   - Assert `wait_for_cluster_size(3)`: a replacement container is running.
   - `unpause()` the original container.
   - Assert `wait_for_cluster_size(3)` still holds: the cluster handles the
     returning node gracefully (either absorbs it as a 4th member and then
     removes an excess, or rejects its ClusterJoin; either outcome is valid as
     long as quorum is not lost and no split brain is detected).

9. **`dual_follower_kill_5_node_cluster`**
   - Start a 5-node variant of the cluster (override `QUORUM_TARGET=5`).
   - Wait for a stable leader.
   - `kill()` two different followers simultaneously.
   - Verify quorum is maintained throughout (cluster continues to commit
     commands submitted during the healing interval).
   - Wait up to `quorum_check_interval + 60 s` for `wait_for_cluster_size(5)`.
   - Assert both killed containers were decommissioned.
   - Assert no split brain.

10. **`quorum_loss_no_autonomous_provisioning`**
    - Start a 3-node cluster; wait for a stable leader.
    - Record all container IDs.
    - `kill()` two nodes (including the leader if necessary to ensure majority
      loss).
    - Wait `quorum_check_interval + 15 s`.
    - Assert `wait_for_cluster_size(3)` **times out** — no new containers are
      automatically provisioned when quorum is lost.
    - Assert the container count remains at 1 (the sole survivor).

##### Safety and cleanup

11. Each test SHALL use a unique `QUORUM_CLUSTER` value (e.g. the test name
    as a suffix) so that containers from concurrent or interrupted test runs
    do not interfere with each other.
12. The `QuorumHealingFixture` destructor SHALL call
    `docker rm --force $(docker ps -aq --filter label=kythira.cluster={cluster_name})`
    to remove all containers for this test's cluster name, ensuring a clean
    slate even when a test fails mid-scenario.

### Requirement 17: Replace Positional Constructor with `node_config` Struct

**User Story:** As a library user constructing a Raft node, I want to supply
configuration through a named struct rather than a positional argument list so
that call sites are self-documenting, argument order is never ambiguous, and
adding future optional parameters does not require touching any existing caller.

The node constructor has grown to twelve parameters as bootstrap, peer
discovery, quorum management, and placement mapping have been added.  At that
size, positional construction requires the reader to look up the declaration to
understand each argument.  A `node_config<Types>` aggregate struct with C++20
designated initializers eliminates this problem and permanently decouples
optional-parameter additions from callsite churn.

#### Acceptance Criteria

1. A `node_config<Types>` aggregate struct SHALL be defined (in
   `include/raft/raft.hpp` or a dedicated `include/raft/node_config.hpp`) with
   the following members in order:
   - **Required** (no in-struct default; omitting them in a designated
     initializer is a compile error):
     `node_id`, `network_client`, `network_server`, `persistence`, `logger`,
     `metrics`, `membership`, `state_machine`.
   - **Optional** (in-struct defaults; may be omitted from the initializer):
     `config`, `self_address`, `peer_discovery`, `quorum_manager`,
     `initial_placement`.
2. The `node` constructor SHALL be replaced with
   `explicit node(node_config<Types> cfg)`.
3. The old positional constructor SHALL be retained as a deprecated delegation
   target for the duration of the migration:
   ```cpp
   [[deprecated("use node_config<Types>")]]
   node(node_id_type, network_client_type, ...) : node(node_config<Types>{...}) {}
   ```
   It SHALL be removed once all in-tree call sites have been migrated.
4. All existing test files that construct a `node` SHALL be migrated to
   designated-initializer form in the same commit that introduces
   `node_config`.  No call site SHALL use the deprecated positional
   constructor after the migration commit.
5. Adding a new optional member to `node_config` in a future commit SHALL
   require no changes to any existing `node_config` initializer, provided the
   new member has an in-struct default.
6. `node_config` SHALL be an aggregate (no user-declared constructors, no
   private members) so that C++20 designated initializers work without
   restriction.
