# Implementation Plan — Docker Chaos Testing Harness

## Status: Complete

**Last Updated**: June 22, 2026

## Overview

Build a Docker-based chaos testing harness for Kythira. The work is divided
into six phases: TCP transport and file persistence (parallel foundations),
the `chaos_node` standalone binary, Docker packaging, C++ orchestration
harness, chaos scenario tests, and CMake integration with documentation.

The result is a `docker-chaos-tests` CMake target that builds the image and
runs Boost.Test scenario tests against a real 3-node containerized Raft
cluster, exercising OS-layer faults (tc netem, iptables, docker kill) alongside
application-layer faults via `fiu_rc_tcp`.

The orchestration harness is implemented in C++ (`tests/docker_chaos/`) with
injectable executor and HTTP stubs so that harness logic can be unit-tested
without Docker. Two CTest-registered unit test executables cover command-string
construction, HTTP response parsing, and ChaosNode/ChaosCluster lifecycle
(Requirement 11).

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 2, 3, 4, 5],
      "description": "TCP transport (1–4) and file persistence (5) — independent, buildable in parallel"
    },
    {
      "wave": 2,
      "tasks": [6, 7, 8],
      "description": "chaos_node binary: config parsing, HTTP control plane, main wiring (depend on wave 1)"
    },
    {
      "wave": 3,
      "tasks": [9, 10, 11],
      "description": "Docker packaging: Dockerfile, entrypoint, docker-compose (depends on wave 2)"
    },
    {
      "wave": 4,
      "tasks": [12, 13, 14, 15, 25],
      "description": "C++ orchestration harness: ChaosCluster, ChaosNode, fault_control, os_faults, unit tests (depends on wave 3)"
    },
    {
      "wave": 5,
      "tasks": [16, 17, 18, 19, 20, 21],
      "description": "Chaos scenario tests (depend on wave 4 harness)"
    },
    {
      "wave": 6,
      "tasks": [22, 23, 24],
      "description": "CMake integration and documentation (depend on wave 5)"
    }
  ]
}
```

## Tasks

---

## Phase 1a: TCP Transport (Tasks 1–4)

### Build a real TCP RPC transport satisfying the network_client / network_server concepts

- [x] 1. Write `include/raft/tcp_rpc.hpp` — framing helpers and peer registry
  - Define `tcp_frame_send(int fd, std::string_view json)`: writes 4-byte
    big-endian length followed by the JSON bytes; returns false on error
  - Define `tcp_frame_recv(int fd) -> std::optional<std::string>`: reads 4-byte
    length then the payload; returns `std::nullopt` on EOF or error
  - Define `tcp_peer_registry<NodeId>`: stores `{host, port}` per node ID;
    `add_peer(id, host, port)`, `lookup(id) -> optional<pair<string,uint16_t>>`
  - Verify: `cmake --build build` compiles the header with no warnings
  - _Requirements: 2.1, 2.2_

- [x] 2. Write `tcp_rpc_client<Types>` in `include/raft/tcp_rpc.hpp`
  - Implements `network_client` concept: `send_request_vote`,
    `send_append_entries`, `send_install_snapshot`
  - Each method: `socket()`/`connect()` to target peer, `tcp_frame_send()`
    the JSON-serialized request, `tcp_frame_recv()` the response, `close()`
  - On any socket error: return `folly::makeFuture<R>(folly::exception_wrapper(
    NetworkException("tcp: ...")))`
  - Connect timeout: `SO_SNDTIMEO` and `SO_RCVTIMEO` set to 500ms
  - Verify: `static_assert(kythira::network_client<tcp_rpc_client<...>, ...>)`
    at bottom of header
  - _Requirements: 2.1, 2.2, 2.4_

- [x] 3. Write `tcp_rpc_server<Types>` in `include/raft/tcp_rpc.hpp`
  - Implements `network_server` concept: `register_request_vote_handler`,
    `register_append_entries_handler`, `register_install_snapshot_handler`,
    `start()`, `stop()`
  - `start()`: creates `SO_REUSEADDR` listen socket on `_port`, binds, listens,
    spawns `_accept_thread` running `accept_loop()`
  - `accept_loop()`: calls `accept()` in a loop; for each connection dispatches
    `handle_connection(fd)`
  - `handle_connection(fd)`: `tcp_frame_recv()` the request JSON, dispatch to
    registered handler, serialize response, `tcp_frame_send()`, close
  - `stop()`: sets `_running = false`, closes listen socket, joins accept thread
  - Verify: `static_assert(kythira::network_server<tcp_rpc_server<...>, ...>)`
  - _Requirements: 2.3, 2.4, 2.5_

- [x] 4. Write `include/raft/tcp_raft_types.hpp`
  - Defines `kythira::tcp_raft_types` composing `tcp_rpc_client`,
    `tcp_rpc_server`, `file_persistence_engine`, and
    `test_key_value_state_machine`
  - _Requirements: 2.4_

---

## Phase 1b: File Persistence Engine (Task 5)

- [x] 5. Write `include/raft/file_persistence.hpp`
  - Template parameters: `Types` (provides `term_id_type`, `node_id_type`,
    `log_index_type`, `log_entry_type`)
  - Constructor: `file_persistence_engine(std::filesystem::path data_dir)`;
    creates `data_dir` if absent; loads existing state into `_log_cache`
  - `save_current_term` / `load_current_term`: atomic write/read of
    `data_dir/term`
  - `save_voted_for` / `load_voted_for`: atomic write/read of
    `data_dir/voted_for`
  - `append_log_entry`: appends JSON-serialized entry to `data_dir/log`;
    `fsync` before returning; updates `_log_cache`
  - `truncate_log(index)`: rewrites `data_dir/log` keeping entries ≤ `index`
  - `fiu_do_on()` calls in each write method matching existing fault point names
  - Verify: `static_assert(kythira::persistence_engine<
    file_persistence_engine<...>, ...>)`
  - _Requirements: 4.1–4.7_

---

## Phase 2: chaos_node Binary (Tasks 6–8)

- [x] 6. Write `cmd/chaos_node/config.hpp` — environment variable parsing
  - `struct NodeConfig` with fields matching the environment variable table
  - `NodeConfig NodeConfig::from_env()`: reads env vars with defaults; throws
    `std::invalid_argument` if `NODE_ID` or `PEERS` are absent or malformed
  - `PEERS` format: `"id:host:port,..."` — parse into `vector<PeerInfo>`
  - _Requirements: 1.1, 1.2_

- [x] 7. Write `cmd/chaos_node/http_control.hpp` — HTTP control plane
  - Uses `httplib`
  - `class http_control` wraps a `kythira::node<tcp_raft_types>&` and port
  - Implements: `GET /health`, `GET /status`, `POST /command`, `GET /log/:index`
  - `start()` runs the httplib server in a background thread; `stop()` shuts down
  - _Requirements: 3.1–3.5_

- [x] 8. Write `cmd/chaos_node/main.cpp` — startup wiring
  - Follows startup sequence: fiu_init → fiu_rc_tcp → persistence → server →
    client → node → http → SIGTERM handler → wait
  - SIGTERM handler: calls `node.stop()`, closes listeners, exits 0
  - Logs startup milestones to stderr
  - _Requirements: 1.1–1.6_

---

## Phase 3: Docker Packaging (Tasks 9–11)

- [x] 9. Write `docker/chaos_node/Dockerfile`
  - Multi-stage build: Ubuntu 24.04 builder + minimal runtime
  - Runtime stage: libfiu0 + fiu-utils + iproute2 + iptables + curl
  - `HEALTHCHECK` using `curl http://localhost:${HTTP_PORT:-8080}/health`
  - `EXPOSE 7000 8080 9000`
  - _Requirements: 5.1_

- [x] 10. Write `docker/chaos_node/entrypoint.sh`
  - Creates `DATA_DIR` if absent
  - Runs `iptables -N CHAOS 2>/dev/null || true` to pre-create CHAOS chain
  - `exec chaos_node` so signals pass through to PID 1
  - _Requirements: 5.1, 6.3_

- [x] 11. Write `docker/docker-compose.yml`
  - Three services: `node1`, `node2`, `node3`
  - Port mappings: `700N:7000`, `808N:8080`, `900N:9000` for N ∈ {1,2,3}
  - HTTP ports published to host so orchestrator can reach control planes
  - `cap_add: [NET_ADMIN]` on all services
  - `networks: [raft_net]` with `driver: bridge`
  - _Requirements: 5.2, 5.3, 5.4, 5.5_

---

## Phase 4: C++ Orchestration Harness (Tasks 12–15, 25)

- [x] 12. Write `tests/docker_chaos/harness.hpp` — `ChaosCluster` and `ChaosNode`
  - `ChaosCluster(compose_file, startup_timeout, exec, http_get, http_post)`:
    injectable executor and HTTP stubs for unit testing; defaults to real
    implementations
  - `ChaosCluster::start()`: runs `docker compose up -d`, polls `/health`
  - `ChaosCluster::stop()`: runs `docker compose down --remove-orphans`
  - `ChaosCluster::node(id)`, `wait_for_leader(timeout)`,
    `assert_no_split_brain()`, `log_lines(id)`, `wait_for_log(id, pattern)`
  - `ChaosNode`: `status()`, `is_leader()`, `is_healthy()`, `submit_command()`,
    `container_ip()`, `enable_fault()`, `disable_all_faults()`,
    `apply_tc_netem()`, `clear_tc_netem()`, `partition_from()`, `unpartition()`,
    `kill()`, `stop()`, `restart()`
  - HTTP response parse functions (`parse_health`, `parse_status`,
    `parse_command_response`, `parse_log_entry`) are free functions so unit
    tests can exercise them directly
  - `ChaosFixture` per-test-case struct: starts cluster in ctor, stops in dtor
  - _Requirements: 7.1–7.5_

- [x] 13. Write `tests/docker_chaos/fault_control.hpp` — fiu-ctrl wrappers
  - `constexpr std::string_view` constants for all fault point names
  - Pure command-string builders: `build_enable_always_cmd`,
    `build_enable_random_cmd`, `build_enable_once_cmd`, `build_disable_cmd`,
    `build_disable_all_cmd` — testable without a live fiu_rc_tcp port
  - `send_fiu_cmd(host, port, cmd)`: raw TCP client; throws on non-zero reply
  - `enable_fault(host, port, name, mode, probability)`: dispatches to correct
    builder and sends over TCP
  - _Requirements: 7.6, 7.7_

- [x] 14. Write `tests/docker_chaos/os_faults.hpp` — tc, iptables, docker wrappers
  - `CmdResult { int code; std::string out; }` and
    `CmdExecutor = std::function<CmdResult(const std::vector<std::string>&)>`
  - `real_exec(argv)`: popen-based real implementation
  - `checked_exec(exec, argv)`: throws `std::runtime_error` on non-zero exit
  - Pure command-vector builders: `tc_netem_cmd`, `tc_clear_cmd`,
    `iptables_drop_src_cmd`, `iptables_drop_dst_cmd`, `iptables_flush_*_cmd`,
    `docker_kill_cmd`, `docker_stop_cmd`, `docker_start_cmd`,
    `compose_up_cmd`, `compose_down_cmd`
  - _Requirements: 6.1–6.7_

- [x] 15. (Merged into 12–14: C++ harness is a single coherent unit)

- [x] 25. Write harness unit tests (Requirement 11)
  - `tests/docker_chaos/harness_unit_test.cpp` (16 tests): `ChaosCluster`
    construction issues no commands; `start()` issues `docker compose up`;
    `stop()` issues `docker compose down`; `wait_for_leader` returns correct
    node; `wait_for_leader` throws on timeout; each `ChaosNode` fault method
    (`apply_tc_netem`, `clear_tc_netem`, `partition_from`, `unpartition`,
    `kill`, `stop`, `restart`) verified for correct command vector; non-zero
    exit raises `std::runtime_error`
  - `tests/docker_chaos/fault_control_unit_test.cpp` (16 tests): all five
    fiu-ctrl command builders verified by exact string; all nine fault-point
    constants start with `"raft/"`; `parse_health` for 200/503/0;
    `parse_status` extracts role and term; `parse_command_response` success and
    not-leader; `parse_log_entry` extracts fields, 404 throws `out_of_range`,
    500 throws `runtime_error`
  - Both executables registered in CTest with label `docker_chaos_unit`
  - _Requirements: 11.1–11.4_

---

## Phase 5: Chaos Scenario Tests (Tasks 16–21)

- [x] 16. `tests/docker_chaos/election_recovery_test.cpp`
  - `election_after_iptables_partition`: partition node 3 from majority via
    iptables; sleep 2 × election_timeout_max; unpartition; verify leader
    elected within 2 × election_timeout_max; `assert_no_split_brain()`
  - `election_after_fiu_network_isolation`: enable all network fault points on
    node 3 (always); trigger same flow; disable; verify recovery
  - _Requirements: 8.1_

- [x] 17. `tests/docker_chaos/crash_recovery_test.cpp`
  - `follower_crash_and_catch_up`: kill one non-leader; submit 5 commands to
    surviving majority; restart killed node; poll `commit_index` until it
    matches cluster's within timeout
  - `leader_crash_and_reelection`: kill current leader; verify new leader
    elected with higher term; `assert_no_split_brain()`
  - _Requirements: 8.2, 8.3_

- [x] 18. `tests/docker_chaos/network_degradation_test.cpp`
  - `tc_netem_packet_loss`: apply 30% packet loss to a follower; submit 10
    commands; remove netem; verify `commit_index >= 10`
  - `tc_netem_high_latency`: apply 200ms delay; verify cluster still commits
  - _Requirements: 8.4_

- [x] 19. `tests/docker_chaos/persistence_faults_test.cpp`
  - `fiu_disk_degradation`: enable `append_log_entry` (random, 30%) on a
    follower; submit 10 commands; disable; `assert_no_split_brain()`
  - `fiu_leader_term_persistence_fault`: enable `save_current_term` (always)
    on leader; observe step-down; disable; verify new leader commits
  - _Requirements: 8.5_

- [x] 20. `tests/docker_chaos/az_partition_test.cpp`
  - `majority_partition_continues_progress`: bidirectional partition isolating
    node 3; verify majority commits; minority makes no progress; heal; verify
    node 3 catches up
  - `symmetric_full_partition_no_leader`: all three nodes isolated; verify no
    leadership; heal; verify recovery
  - _Requirements: 8.6_

- [x] 21. `tests/docker_chaos/safety_assertions_test.cpp`
  - `no_log_divergence_under_combined_faults`: partition + 5 commits + kill +
    heal + restart; verify all nodes reach committed index; `assert_no_split_brain()`
  - _Requirements: 8.7_

---

## Phase 6: CMake Integration and Documentation (Tasks 22–24)

- [x] 22. Add `docker-chaos-image` and `docker-chaos-tests` targets
  - `tests/docker_chaos/CMakeLists.txt`: `add_docker_chaos_unit_test` helper
    builds each unit test as a separate executable and registers in CTest;
    `docker_chaos_scenario_tests` executable linked against `Boost::unit_test_framework`
    and `httplib`; `docker-chaos-image` custom target runs `docker build`;
    `docker-chaos-tests` custom target depends on image + scenario binary
  - Root `CMakeLists.txt` replaced pytest block with
    `include(tests/docker_chaos/CMakeLists.txt)`
  - _Requirements: 9.1–9.5_

- [x] 23. Update `README.md` — Docker Chaos Testing section
  - Add after the "Chaos Testing" section
  - Cover: Docker prerequisites, build image target, `docker compose up` for
    manual inspection, running C++ scenario tests via CMake target, manually
    driving `fiu-ctrl` against a running cluster
  - Document the environment variable table
  - Document the port mapping (host 7001/8081/9001 → node1 RPC/HTTP/fiu)
  - _Requirements: 10.1, 10.2_

- [x] 24. Update `doc/TODO.md`
  - Mark docker chaos testing item done with accurate C++ harness description,
    unit test counts, and 25-task completion status
  - _Requirements: 10.3_

---

---

## Phase 7: Expansion — Discovery Scenario Tests and Podman Support (Tasks 26–30)

- [x] 26. Add `poco_peer_discovery` Docker scenario test
  - `tests/docker_chaos/poco_discovery_test.cpp` (3 cases): `all_nodes_healthy`,
    `all_nodes_discover_peers`, `node_deregisters_on_stop`.
  - Compose: `docker/poco-discovery-compose.yml` — 3 `poco_discovery_node`
    containers sharing an Avahi socket from the host.
  - Node binary: `cmd/poco_discovery_node/` — registers via `poco_peer_discovery`,
    serves `/health` and `/peers` HTTP endpoints.
  - CMake target: `docker-poco-discovery-tests`; image target:
    `docker-poco-discovery-image`.

- [x] 27. Add DNS discovery Docker scenario test (A-record / RFC 2136)
  - `tests/docker_chaos/dns_discovery_test.cpp` (3 cases): `all_nodes_healthy`,
    `all_nodes_discover_peers`, `stopped_node_absent_after_deregister`.
  - Compose: `docker/dns-discovery-compose.yml` — BIND9 + 3 `dns_discovery_node`
    containers. SIGTERM on a node triggers clean deregistration; surviving nodes
    stop reporting the stopped node within 3 s.
  - BIND9 image: `docker/bind9/Dockerfile` — multi-stage Ubuntu 24.04 build with
    RFC 2136 enabled on a private `example.local.` zone.
  - CMake targets: `docker-bind9-image`, `docker-dns-discovery-image`,
    `docker-dns-discovery-tests`.

- [x] 28. Add DNS-SD discovery Docker scenario test (PTR/SRV/TXT / RFC 2136)
  - `tests/docker_chaos/dns_sd_discovery_test.cpp` (3 cases): `all_nodes_healthy`,
    `all_nodes_discover_peers`, `dead_node_absent_after_freshness_expiry`.
  - Compose: `docker/dns-sd-discovery-compose.yml` — BIND9 + 3
    `dns_sd_discovery_node` containers. The freshness expiry test kills node1
    with SIGKILL and waits 25 s (freshness interval = 20 s) to verify the dead
    node is no longer reported by the surviving nodes.
  - CMake targets: `docker-dns-sd-discovery-image`, `docker-dns-sd-discovery-tests`.

- [x] 29. Podman support in test harness
  - `tests/docker_chaos/os_faults.hpp`: added `container_runtime()` (reads
    `$KYTHIRA_CONTAINER_RUNTIME`, default `"docker"`) and `compose_prefix()`
    (reads `$KYTHIRA_COMPOSE_COMMAND`, defaults to `[runtime, "compose"]`). All
    command-vector builders use these instead of the hardcoded `"docker"` string.
  - `tests/docker_chaos/CMakeLists.txt`: replaced `find_program(DOCKER_EXECUTABLE
    docker)` with auto-detection of `docker` then `podman`; exposes
    `CONTAINER_RUNTIME` and `COMPOSE_COMMAND` as CMake cache variables; forwards
    both as `KYTHIRA_CONTAINER_RUNTIME` / `KYTHIRA_COMPOSE_COMMAND` env vars into
    every scenario-test invocation.

- [x] 30. Rootless Podman compatibility for DNS compose files
  - `docker/dns-discovery-compose.yml`: removed fixed-subnet IPAM and
    `ipv4_address` from BIND9 service (rootless Podman ignores `ipv4_address`);
    `DNS_SERVER` changed from `"172.26.0.10"` to `"dns-test-bind9"`.
  - `docker/dns-sd-discovery-compose.yml`: same change — `DNS_SERVER` is now
    `"dns-sd-test-bind9"`.
  - `cmd/dns_discovery_node/main.cpp` and `cmd/dns_sd_discovery_node/main.cpp`:
    added `resolve_to_ip()` (`getaddrinfo(AF_INET)` wrapper) that resolves the
    service name to an IP inside the container network before handing it to ldns,
    which only accepts IP literals.

## Summary

| Phase | Tasks | Status |
|---|---|---|
| 1a | 1–4 (TCP transport) | Complete |
| 1b | 5 (file persistence) | Complete |
| 2 | 6–8 (chaos_node binary) | Complete |
| 3 | 9–11 (Docker packaging) | Complete |
| 4 | 12–15, 25 (C++ harness + unit tests) | Complete |
| 5 | 16–21 (scenario tests) | Complete |
| 6 | 22–24 (CMake + docs) | Complete |
| 7 | 26–30 (discovery scenario tests + Podman) | Complete |

**Total**: 30 tasks (all complete)

## Notes

- `fiu_rc_tcp` must be called before `node.start()` so that fault points are
  active from the first Raft message.
- `NET_ADMIN` capability is required in each container for `tc` and `iptables`.
- `SO_SNDTIMEO`/`SO_RCVTIMEO` on TCP transport sockets must be shorter than
  `election_timeout_min` so that a failed peer causes a timeout exception
  rather than blocking the election indefinitely.
- The `CHAOS` iptables chain (created by entrypoint.sh) means the harness never
  needs to touch the `INPUT` chain directly, avoiding accidental lockout of
  container management traffic.
- The C++ harness uses an injectable `CmdExecutor` and HTTP client stubs so
  that harness unit tests (Requirement 11) run without Docker. The
  `docker_chaos_harness_unit_tests` and `docker_chaos_fault_control_unit_tests`
  executables are registered in CTest with label `docker_chaos_unit` and run
  as part of the standard `ctest` invocation.
