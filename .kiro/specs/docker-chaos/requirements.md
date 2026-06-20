# Requirements Document

## Introduction

This document specifies the requirements for a Docker-based chaos testing
harness for Kythira. Where tests that parallel the in-process chaos tests
(`tests/chaos/`) exercise the Raft protocol under simulated faults injected by
the test harness through the fault locations defined by the libfiu macros. The
tests go on to include **real OS-layer and infrastructure failures**: process
crashes, network partitions at the kernel level, latency and packet loss via
`tc netem`, and VM-equivalent termination via `docker stop`/`docker kill`.

Fault injection at the Raft application layer is driven remotely via
`fiu_rc_tcp` — a TCP listener embedded in the `chaos_node` binary that accepts
`fiu-ctrl` commands from outside the container. OS-layer faults are applied via
`docker exec` commands issued by a Python test harness running on the host.
Container logs are collected and monitored in real time by the test harness,
enabling assertions on Raft state transitions and fault responses as they occur.

The result is two complementary layers:

| Layer | Mechanism | Scope |
|---|---|---|
| Application | `fiu_rc_tcp` + `fiu-ctrl` | Raft RPC send failures, disk write errors, state machine faults |
| OS / network | `tc netem`, `iptables`, `docker kill` | Packet loss, latency, partition, process crash |
| Infrastructure | `docker stop`, Docker network manipulation | Node termination, AZ-level outage simulation |

## Glossary

- **chaos_node**: A standalone C++ binary that runs a single `kythira::node<>`
  instance, exposes a TCP RPC port for Raft peer communication, an HTTP control
  plane for test assertions, and a `fiu_rc_tcp` port for remote fault injection.
- **ChaosCluster**: C++ class that manages the lifecycle (start, stop, wait
  for health) of a Docker Compose cluster of `chaos_node` containers.
- **ChaosNode**: C++ class wrapping a single container; provides methods for
  all fault types (fiu faults, tc netem, iptables, docker kill/stop).
- **fiu_rc_tcp**: A TCP listener embedded in `chaos_node` via
  `fiu_rc_tcp("0.0.0.0", port)` at startup; accepts `fiu-ctrl` commands from
  any host that can reach the container's mapped port.
- **TCP transport**: A purpose-built `tcp_rpc_client` / `tcp_rpc_server` pair
  in `include/raft/tcp_rpc.hpp` that carries Raft RPCs between containers over
  real TCP sockets, satisfying the `network_client` / `network_server` concepts.
- **File persistence engine**: A `file_persistence_engine` in
  `include/raft/file_persistence.hpp` that persists term, vote, and log entries
  to the container's `DATA_DIR`, satisfying the `persistence_engine` concept.
- **Orchestration harness**: C++ library in `tests/docker_chaos/` that
  wraps Docker Compose lifecycle, `fiu-ctrl`, `curl`, and `docker exec` calls
  into a test-friendly API.

## Requirements

### Requirement 1: chaos_node Binary

**User Story:** As a test author, I want a standalone binary that runs a
kythira Raft node using real TCP networking, so that I can deploy it in Docker
and test the full protocol stack end-to-end.

#### Acceptance Criteria

1. WHEN the binary starts THEN it SHALL read its configuration from environment
   variables: `NODE_ID`, `RPC_PORT`, `HTTP_PORT`, `FIU_PORT`, `DATA_DIR`,
   `PEERS` (comma-separated `id:host:port` triples), `ELECTION_TIMEOUT_MIN_MS`,
   `ELECTION_TIMEOUT_MAX_MS`, `HEARTBEAT_INTERVAL_MS`.
2. WHEN `NODE_ID` or `PEERS` are missing THEN the binary SHALL print a usage
   message to stderr and exit non-zero.
3. WHEN the binary starts successfully THEN it SHALL call `fiu_init(0)` and
   `fiu_rc_tcp("0.0.0.0", FIU_PORT)` before starting the Raft node, so that
   fault injection is available immediately.
4. WHEN the Raft node is running THEN the binary SHALL serve an HTTP control
   plane on `HTTP_PORT` with at minimum the endpoints defined in Requirement 3.
5. WHEN the binary receives `SIGTERM` THEN it SHALL stop the Raft node, close
   all listeners, and exit with code 0.
6. WHEN the binary is compiled with `KYTHIRA_FAULT_INJECTION=ON` THEN it SHALL
   link `fiu` and `fiu-rc` and call `fiu_rc_tcp`. WHEN compiled without this
   option THEN `fiu_init` and `fiu_rc_tcp` SHALL be omitted and the binary SHALL
   remain functional for non-chaos use.

### Requirement 2: TCP Transport

**User Story:** As a developer, I want Raft RPCs carried over real TCP sockets
between `chaos_node` processes, so that OS-level network faults (latency,
packet loss, partition) are observable at the Raft layer.

#### Acceptance Criteria

1. WHEN `tcp_rpc_client` sends a RequestVote, AppendEntries, or InstallSnapshot
   RPC THEN it SHALL open a TCP connection to the target's `address:rpc_port`,
   write the JSON-serialized request with a 4-byte big-endian length prefix,
   read the response with the same framing, and close the connection.
2. WHEN the TCP connection or read/write fails THEN `tcp_rpc_client` SHALL
   return a failed future (or throw) equivalent to a network timeout, so that
   the Raft node's retry and stepdown logic is exercised.
3. WHEN `tcp_rpc_server` starts THEN it SHALL listen on `rpc_port` and accept
   connections in a background thread, dispatch received messages to the
   registered RequestVote, AppendEntries, and InstallSnapshot handlers, and
   write the response with the same 4-byte framing before closing the connection.
4. WHEN `tcp_rpc_client` and `tcp_rpc_server` are assembled into a `raft_types`
   struct THEN `static_assert` SHALL confirm both satisfy the `network_client`
   and `network_server` concepts respectively.
5. WHEN a message arrives from an unknown peer THEN `tcp_rpc_server` SHALL
   dispatch it to the registered handler anyway (peer authentication is out of
   scope for chaos testing).

### Requirement 3: HTTP Control Plane

**User Story:** As a test author, I want to query each node's Raft state and
submit commands over HTTP, so that the Python harness can assert cluster
correctness without requiring a Raft-specific client library.

#### Acceptance Criteria

1. WHEN `GET /health` is called THEN it SHALL return HTTP 200 with body
   `{"status":"running"}` if the node is started, or HTTP 503 if not yet ready.
2. WHEN `GET /status` is called THEN it SHALL return HTTP 200 with a JSON body
   containing at minimum: `node_id`, `role` (`"leader"`, `"follower"`,
   `"candidate"`), `term`, `leader_id` (or `null`), `commit_index`,
   `last_applied`.
3. WHEN `POST /command` is called with body `{"key":"<k>","value":"<v>"}` THEN
   it SHALL submit a put command to the Raft node and return HTTP 200 with
   `{"success":true,"commit_index":<n>}` once committed, or HTTP 503 if the
   node is not the leader (with body `{"error":"not_leader","leader_id":<id>}`).
4. WHEN `GET /log/<index>` is called THEN it SHALL return the log entry at that
   index as JSON, or HTTP 404 if the index is out of range.
5. WHEN the control plane cannot be bound (port in use) THEN the binary SHALL
   log an error and exit non-zero rather than silently starting without it.
6. WHEN the container is started THEN the HTTP control plane port SHALL be
   published to the host so that the orchestrator can reach each node's control
   plane from outside the Docker network.

### Requirement 4: File Persistence Engine

**User Story:** As a developer, I want a persistence engine that stores Raft
state to the container filesystem, so that node crash-and-restart scenarios can
verify recovery from durable state.

#### Acceptance Criteria

1. WHEN `save_current_term(t)` is called THEN it SHALL atomically write `t` to
   `DATA_DIR/term` (write to a temp file, then rename).
2. WHEN `save_voted_for(n)` is called THEN it SHALL atomically write `n` to
   `DATA_DIR/voted_for`.
3. WHEN `append_log_entry(entry)` is called THEN it SHALL append the serialized
   entry to `DATA_DIR/log` and flush before returning.
4. WHEN `truncate_log(index)` is called THEN it SHALL truncate `DATA_DIR/log`
   to contain only entries with index ≤ `index`.
5. WHEN `load_current_term()` is called on a fresh `DATA_DIR` THEN it SHALL
   return term 0. On a previously written directory it SHALL return the last
   saved term.
6. WHEN the `DATA_DIR` does not exist THEN `file_persistence_engine` SHALL
   create it on first write.
7. WHEN `file_persistence_engine` is used in a `raft_types` struct THEN
   `static_assert` SHALL confirm it satisfies the `persistence_engine` concept.

### Requirement 5: Docker Packaging

**User Story:** As a developer, I want a Dockerfile and Docker Compose
configuration that can stand up a 3-node kythira cluster locally with a single
command, so that chaos scenarios can be reproduced without manual setup.

#### Acceptance Criteria

1. WHEN `docker build` runs on the provided Dockerfile THEN it SHALL produce an
   image containing the `chaos_node` binary, `fiu-ctrl`, `tc` (iproute2),
   `iptables`, and `curl`. No development headers or build tools SHALL remain in
   the runtime image.
2. WHEN `docker compose up` runs in the repository root THEN a 3-node cluster
   SHALL start; all three nodes SHALL respond to `GET /health` with HTTP 200
   within 10 seconds.
3. WHEN the cluster starts THEN a leader SHALL be elected within
   `3 × ELECTION_TIMEOUT_MAX_MS` of all nodes being healthy.
4. WHEN a container's `NET_ADMIN` capability is set (as configured in
   `docker-compose.yml`) THEN `docker exec <container> tc` and
   `docker exec <container> iptables` SHALL succeed without error.
5. WHEN the `fiu_rc_tcp` port is mapped in `docker-compose.yml` THEN
   `fiu-ctrl -c "enable name=raft/network/send_request_vote" localhost:<port>`
   SHALL succeed from the host machine.

### Requirement 6: OS-Layer Fault Injection

**User Story:** As a test author, I want to apply OS-level network faults and
process termination to individual containers, so that I can test Raft's
behaviour under failures that are invisible to application-layer fault points.

#### Acceptance Criteria

1. WHEN `ChaosNode.apply_tc_netem(loss, delay, corrupt)` is called THEN it
   SHALL apply the corresponding `tc qdisc` rule to the container's primary
   network interface, and subsequent Raft RPCs from that node SHALL exhibit the
   specified characteristics.
2. WHEN `ChaosNode.clear_tc_netem()` is called THEN it SHALL remove the
   previously applied netem rule; subsequent RPCs SHALL proceed without
   artificial degradation.
3. WHEN `ChaosNode.partition_from(peers)` is called with a list of node IDs
   THEN it SHALL install `iptables DROP` rules blocking all TCP traffic to those
   peers' addresses; the node SHALL be unable to receive Raft RPCs from those
   peers.
4. WHEN `ChaosNode.unpartition()` is called THEN all previously installed
   `iptables DROP` rules for this node SHALL be removed.
5. WHEN `ChaosNode.kill()` is called THEN it SHALL send `SIGKILL` to the
   container process (`docker kill`); the container SHALL stop immediately
   without flushing state.
6. WHEN `ChaosNode.stop()` is called THEN it SHALL send `SIGTERM` and wait up
   to 10 seconds for a clean shutdown (`docker stop`).
7. WHEN `ChaosNode.restart()` is called after `kill()` or `stop()` THEN it
   SHALL restart the container (`docker start`) and wait for `GET /health` to
   return 200 before returning.

### Requirement 7: Python Orchestration Harness

**User Story:** As a test author, I want a Python API that wraps Docker,
`fiu-ctrl`, and `curl` calls into a clean, pytest-friendly interface, so that
I can write scenario tests in ~50 lines without shell scripting.

#### Acceptance Criteria

1. WHEN `ChaosCluster(compose_file)` is constructed THEN it SHALL NOT start
   containers. Containers SHALL start only when `ChaosCluster.start()` is
   called.
2. WHEN `ChaosCluster.start()` is called THEN it SHALL run `docker compose up
   -d`, poll each node's `/health` endpoint, and raise `TimeoutError` if any
   node does not become healthy within the configured timeout (default 30s).
3. WHEN `ChaosCluster.wait_for_leader(timeout)` is called THEN it SHALL poll
   all nodes' `/status` endpoints and return the `ChaosNode` for the current
   leader, or raise `TimeoutError` if no leader is elected within `timeout`.
4. WHEN `ChaosCluster.stop()` is called THEN it SHALL run
   `docker compose down --remove-orphans`.
5. WHEN any `ChaosNode` fault method is called THEN it SHALL raise
   `subprocess.CalledProcessError` if the underlying command fails, so that
   test failures surface clearly.
6. WHEN `ChaosNode.enable_fault(name, mode, probability)` is called THEN it
   SHALL invoke `fiu-ctrl` with the appropriate `-c "enable..."` command
   targeting the node's mapped `FIU_PORT`. `mode` SHALL accept `"always"`,
   `"random"`, and `"once"`.
7. WHEN `ChaosNode.disable_all_faults()` is called THEN it SHALL invoke
   `fiu-ctrl -c "disable_all"` targeting the node's `FIU_PORT`.

### Requirement 8: Chaos Scenario Tests

**User Story:** As a maintainer, I want scenario tests that verify Raft's
safety and liveness properties under real OS-layer and VM-level failures, so
that regressions in distributed behaviour are caught even when in-process chaos
tests pass.

#### Acceptance Criteria

1. WHEN `test_election_after_partition_heals` runs THEN it SHALL: partition
   the minority (1 node) at the iptables level; wait 2 × election_timeout_max;
   unpartition; verify a leader is elected within 2 × election_timeout_max.
2. WHEN `test_crash_recovery` runs THEN it SHALL: kill one non-leader node;
   submit 5 commands to the surviving majority; restart the killed node; verify
   the restarted node's `commit_index` reaches the cluster's within
   5 × heartbeat_interval.
3. WHEN `test_leader_crash_and_reelection` runs THEN it SHALL: kill the current
   leader; verify a new leader is elected within 3 × election_timeout_max;
   verify no split brain (at most one leader per term) using `/status` across
   all surviving nodes.
4. WHEN `test_network_degradation` runs THEN it SHALL: apply 30% packet loss
   via `tc netem` to one follower; submit 10 commands; remove the netem rule;
   verify all 10 commands commit across the majority within 10 × heartbeat_interval.
5. WHEN `test_fiu_persistence_fault_recovery` runs THEN it SHALL: enable
   `raft/persistence/append_log_entry` (30% random) on one node via `fiu-ctrl`;
   submit 10 commands; disable the fault; verify all commands commit.
6. WHEN `test_az_partition` runs THEN it SHALL: simulate an AZ outage by
   partitioning 2 nodes from 1 (2-node majority retains connectivity); verify
   the majority partition continues to commit commands; verify the minority node
   does not claim leadership; heal; verify full cluster reconvergence.
7. WHEN any safety assertion fails (two leaders in same term, log mismatch)
   THEN the test SHALL fail with a diagnostic showing the `/status` output of
   each node at the time of detection.

### Requirement 9: Build Integration

**User Story:** As a developer, I want a CMake target that builds the Docker
image so that the chaos node binary and image are always built from the current
source, and CI can run docker-chaos tests without manual steps.

#### Acceptance Criteria

1. WHEN `cmake --build build --target docker-chaos-image` is run THEN it SHALL
   invoke `docker build` and tag the resulting image `kythira-chaos-node:dev`.
2. WHEN Docker is not found on the host THEN the `docker-chaos-image` target
   SHALL print an actionable message and exit non-zero; the default `all` target
   SHALL be unaffected.
3. WHEN `cmake --build build --target docker-chaos-tests` is run THEN it SHALL
   build the image (if stale) and run `pytest tests/docker_chaos/` with the
   cluster compose file set to `docker/docker-compose.yml`.
4. WHEN the `docker-chaos-tests` target runs THEN it SHALL set the
   `KYTHIRA_COMPOSE_FILE` environment variable so that `conftest.py` picks up
   the correct compose file without hardcoding.
5. WHEN `KYTHIRA_FAULT_INJECTION` is `OFF` THEN the Docker image target SHALL
   emit a warning that fiu_rc_tcp will be absent, but SHALL still build and
   produce a functional (non-chaos) node image.

### Requirement 10: Documentation

**User Story:** As a contributor, I want the docker chaos harness documented so
that I understand how to add new scenarios, run the suite locally, and interpret
failures.

#### Acceptance Criteria

1. WHEN the spec is implemented THEN `README.md` SHALL include a "Docker Chaos
   Testing" section explaining: Docker prerequisites, `docker compose up`,
   `pytest tests/docker_chaos/`, and how to drive `fiu-ctrl` manually against a
   running cluster.
2. WHEN the section is written THEN it SHALL document all environment variables
   accepted by `chaos_node`, and the mapping of host ports to container services
   used by the default `docker-compose.yml`.
3. WHEN the spec is complete THEN `doc/TODO.md` SHALL mark the docker chaos
   testing item done.

### Requirement 11: Harness Unit Tests

**User Story:** As a developer, I want unit tests for the orchestration harness
classes so that I can verify their logic in isolation without spinning up Docker
containers.

#### Acceptance Criteria

1. WHEN `ChaosCluster` is tested THEN it SHALL have unit tests covering: cluster
   construction without starting containers, `start()` / `stop()` lifecycle
   transitions, `wait_for_leader()` returning the correct node, and
   `TimeoutError` when no leader is elected within the configured timeout.
2. WHEN `ChaosNode` is tested THEN it SHALL have unit tests covering: each fault
   method (`apply_tc_netem`, `clear_tc_netem`, `partition_from`, `unpartition`,
   `kill`, `stop`, `restart`, `enable_fault`, `disable_all_faults`), verifying
   the correct underlying command is issued and that a non-zero exit raises an
   error.
3. WHEN the orchestration harness is tested THEN it SHALL have unit tests
   covering: HTTP control plane request/response parsing for `/health`,
   `/status`, `/command`, and `/log/<index>`; and fiu-ctrl command construction
   for each fault mode (`"always"`, `"random"`, `"once"`).
4. WHEN the harness unit tests are built THEN they SHALL be registered as a
   CTest target and included in the standard `ctest` run alongside the existing
   test suite.
