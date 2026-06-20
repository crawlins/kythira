# Design Document

## Overview

This document describes the design for the Docker-based chaos testing harness.
The harness wraps each kythira Raft node in a container, exposes three ports per
node (RPC, HTTP control plane, fiu_rc_tcp), and provides a C++ orchestration
layer that can drive all failure modes from a Boost.Test test file.

The tests parallel the in-process chaos tests (`tests/chaos/`), exercising the
same Raft fault points defined by libfiu macros, and extend them with **real
process isolation and real OS-layer faults**: packet loss via `tc netem` is
genuine kernel-level packet loss, not a simulator parameter; a `docker kill` is
a real `SIGKILL` with no cleanup, not a simulated crash. Container logs are
collected and monitored in real time by the orchestration harness, enabling
assertions on Raft state transitions and fault responses as they occur.

## Architecture

```
  Test host
  ─────────────────────────────────────────────────────────────
  Boost.Test runner
    └── ChaosCluster (tests/docker_chaos/harness.hpp)
          ├── docker compose up / down
          ├── log stream thread (docker logs -f, real-time)
          └── ChaosNode(1)              ChaosNode(2)  ChaosNode(3)
                ├── fiu-ctrl →               ...            ...
                │   localhost:9001 (fiu_rc)
                ├── HTTP client →
                │   localhost:8081 (HTTP status/command)
                ├── docker exec tc netem    (OS network fault)
                ├── docker exec iptables    (OS partition)
                └── docker stop / kill      (node termination)

  Docker bridge network (raft_net)
  ─────────────────────────────────────────────────────────────
  ┌────────────────┐  ┌────────────────┐  ┌────────────────┐
  │  chaos_node_1  │  │  chaos_node_2  │  │  chaos_node_3  │
  │  NODE_ID=1     │  │  NODE_ID=2     │  │  NODE_ID=3     │
  │  :7000  (RPC)  │◄─┤  :7000  (RPC)  │◄─┤  :7000  (RPC)  │
  │  :8080  (HTTP) │  │  :8080  (HTTP) │  │  :8080  (HTTP) │
  │  :9000  (fiu)  │  │  :9000  (fiu)  │  │  :9000  (fiu)  │
  └────────────────┘  └────────────────┘  └────────────────┘
  ext 7001/8081/9001  ext 7002/8082/9002  ext 7003/8083/9003
```

### Fault injection paths

```
Application-layer fault (fiu_rc_tcp)
  host$ fiu-ctrl -c "enable name=raft/network/send_request_vote" localhost:9001
  → TCP → fiu_rc_tcp listener in chaos_node_1
  → fiu_do_on() fires inside simulator_network.hpp send_request_vote()
  → throws network_exception
  → kythira::node retry / stepdown logic exercises

OS-layer network fault (tc netem)
  host$ docker exec chaos_node_1 tc qdisc add dev eth0 root netem loss 30%
  → Linux kernel drops 30% of outgoing packets from node 1
  → Raft RPC responses time out → retry / election timeout fires

Network partition (iptables)
  host$ docker exec chaos_node_1 iptables -A INPUT -s <node3_ip> -j DROP
  → kernel drops all inbound TCP from node 3 to node 1
  → node 3 appears unreachable from node 1's perspective

Node termination (docker kill)
  host$ docker kill chaos_node_1
  → SIGKILL to PID 1 in container → immediate process death
  → no fsync, no graceful shutdown
  → file_persistence_engine's data on Docker volume is consistent up to
    the last completed write (atomic rename guarantees for term/voted_for;
    log tail may be incomplete)
```

## Component Design

### 1. `include/raft/tcp_rpc.hpp`

A minimal TCP transport that satisfies the `network_client` and
`network_server` concepts. Uses blocking POSIX sockets. Not intended for
production use — latency and throughput are secondary to simplicity and
debuggability.

**Wire protocol**

```
┌──────────────────┬──────────────────────────────────┐
│  4 bytes         │  N bytes                         │
│  big-endian uint │  UTF-8 JSON payload              │
│  (payload size)  │                                  │
└──────────────────┴──────────────────────────────────┘
```

The JSON payload uses the existing `json_rpc_serializer` format (the same
serializer used by the simulator and HTTP transports), so no new serialization
code is needed.

**`tcp_rpc_client`**

```cpp
template<typename Types>
class tcp_rpc_client {
public:
    // Peer registry: populated from PEERS env var at startup
    void add_peer(node_id_type id, std::string host, uint16_t port);

    // network_client concept methods
    auto send_request_vote(node_id_type target,
                           const request_vote_request_type& req)
        -> future_type;

    auto send_append_entries(node_id_type target,
                             const append_entries_request_type& req)
        -> future_type;

    auto send_install_snapshot(node_id_type target,
                               const install_snapshot_request_type& req)
        -> future_type;

private:
    // Opens a connection, sends req, reads response, closes.
    // Returns failed future on any socket error.
    template<typename Req, typename Resp>
    auto call(node_id_type target, const Req& req) -> folly::Future<Resp>;

    std::unordered_map<node_id_type, std::pair<std::string, uint16_t>> _peers;
};
```

**`tcp_rpc_server`**

```cpp
template<typename Types>
class tcp_rpc_server {
public:
    explicit tcp_rpc_server(uint16_t port);

    // network_server concept methods
    void register_request_vote_handler(handler_fn);
    void register_append_entries_handler(handler_fn);
    void register_install_snapshot_handler(handler_fn);
    void start();   // spawns accept thread
    void stop();    // closes listen socket, joins thread

private:
    void accept_loop();
    void handle_connection(int fd);

    uint16_t _port;
    int _listen_fd{-1};
    std::atomic<bool> _running{false};
    std::thread _accept_thread;
    // handlers...
};
```

**Transport compatibility note**: `tcp_rpc_client::call()` uses Folly's
`folly::makeFuture<T>(result)` to return a completed future synchronously
(blocking call wrapped in an already-resolved future). This is sufficient for
correctness — kythira's retry logic uses `.delay()` and `.thenTry()` on the
returned future, which works regardless of whether the future is pre-resolved
or asynchronous.

### 2. `include/raft/file_persistence.hpp`

Atomic writes use the write-then-rename pattern. Log entries are appended
to a sequential binary file; truncation rewrites the file up to the target
index. This is correct for testing purposes; a production engine would use a
more sophisticated append-only log format.

```cpp
template<typename Types>
class file_persistence_engine {
public:
    explicit file_persistence_engine(std::filesystem::path data_dir);

    // persistence_engine concept methods
    auto load_current_term() const -> term_id_type;
    auto save_current_term(term_id_type term) -> void;
    auto load_voted_for() const -> std::optional<node_id_type>;
    auto save_voted_for(std::optional<node_id_type> voted_for) -> void;
    auto get_log_entry(log_index_type index) const -> log_entry_type;
    auto get_log_size() const -> log_index_type;
    auto append_log_entry(const log_entry_type& entry) -> void;
    auto truncate_log(log_index_type index) -> void;
    // snapshot methods...

private:
    std::filesystem::path _data_dir;
    mutable std::mutex _log_mutex;
    // In-memory log cache loaded at startup, flushed on write.
    std::vector<log_entry_type> _log_cache;

    void atomic_write(const std::filesystem::path& path,
                      std::string_view content);
};
```

`fiu_do_on()` calls in each write method are inherited from the existing
`fault_injection.hpp` guard — the same fault points used by in-process tests
apply here, so `fiu-ctrl` commands against `fiu_rc_tcp` hit the same code paths
as the existing `fault_profiles.hpp` enable calls.

### 3. `cmd/chaos_node/main.cpp`

Startup sequence:

```
1. Parse environment variables → NodeConfig
2. fiu_init(0)
3. fiu_rc_tcp("0.0.0.0", FIU_PORT)         ← remote fault control live
4. Construct file_persistence_engine(DATA_DIR)
5. Construct tcp_rpc_server(RPC_PORT); register handlers; server.start()
6. Construct tcp_rpc_client; add_peer() for each entry in PEERS
7. Construct kythira::node<tcp_raft_types>
8. node.set_cluster_configuration(all_node_ids)
9. node.start()
10. Start HTTP control plane (httplib server, background thread)
11. Install SIGTERM handler → node.stop(); exit(0)
12. Main thread waits on condition variable (exits when signalled)
```

**Environment variables**

| Variable | Default | Description |
|---|---|---|
| `NODE_ID` | required | Integer node ID |
| `RPC_PORT` | `7000` | Raft RPC listen port |
| `HTTP_PORT` | `8080` | HTTP control plane port |
| `FIU_PORT` | `9000` | fiu_rc_tcp listen port |
| `DATA_DIR` | `/var/lib/chaos_node` | Persistence directory |
| `PEERS` | required | `id:host:port,...` |
| `ELECTION_TIMEOUT_MIN_MS` | `150` | Min election timeout |
| `ELECTION_TIMEOUT_MAX_MS` | `300` | Max election timeout |
| `HEARTBEAT_INTERVAL_MS` | `50` | Heartbeat interval |

**HTTP control plane** (`cmd/chaos_node/http_control.hpp`)

Built with the existing `httplib` dependency (already in `CMakeLists.txt` via
`find_package(httplib QUIET)`).

```
GET  /health          → 200 {"status":"running"} or 503
GET  /status          → 200 {"node_id":1,"role":"follower","term":3,
                             "leader_id":2,"commit_index":5,"last_applied":5}
POST /command         → body {"key":"k","value":"v"}
                         200 {"success":true,"commit_index":6}
                         503 {"error":"not_leader","leader_id":2}
GET  /log/:index      → 200 {"index":3,"term":2,"command":"..."}
                         404 if out of range
```

### 4. `docker/chaos_node/Dockerfile`

Multi-stage build. The builder stage uses the existing vcpkg toolchain;
the runtime stage is minimal.

```dockerfile
# ── Build stage ──────────────────────────────────────────────
FROM ubuntu:24.04 AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build clang pkg-config git curl zip unzip tar \
    libfiu-dev libboost-all-dev nlohmann-json3-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DKYTHIRA_FAULT_INJECTION=ON && \
    cmake --build build --target chaos_node

# ── Runtime stage ─────────────────────────────────────────────
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    libfiu0 fiu-utils \
    iproute2 \
    iptables \
    curl && \
    rm -rf /var/lib/apt/lists/*
COPY --from=builder /src/build/chaos_node /usr/local/bin/chaos_node
COPY docker/chaos_node/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
EXPOSE 7000 8080 9000
HEALTHCHECK --interval=2s --timeout=3s --retries=10 \
    CMD curl -sf http://localhost:${HTTP_PORT:-8080}/health || exit 1
ENTRYPOINT ["/entrypoint.sh"]
```

`entrypoint.sh` creates `DATA_DIR` if absent, then `exec chaos_node`. It also
installs a default `iptables` chain (`CHAOS`) for the harness to add rules to,
avoiding permission issues from outside the container.

### 5. `docker/docker-compose.yml`

```yaml
services:
  node1:
    image: kythira-chaos-node:dev
    container_name: chaos_node_1
    hostname: node1
    environment:
      NODE_ID: "1"
      PEERS: "2:node2:7000,3:node3:7000"
    ports:
      - "7001:7000"   # RPC    (host 7001 → container 7000)
      - "8081:8080"   # HTTP   (host 8081 → container 8080)
      - "9001:9000"   # fiu_rc (host 9001 → container 9000)
    cap_add: [NET_ADMIN]
    networks: [raft_net]

  node2:
    image: kythira-chaos-node:dev
    container_name: chaos_node_2
    hostname: node2
    environment:
      NODE_ID: "2"
      PEERS: "1:node1:7000,3:node3:7000"
    ports:
      - "7002:7000"
      - "8082:8080"
      - "9002:9000"
    cap_add: [NET_ADMIN]
    networks: [raft_net]

  node3:
    image: kythira-chaos-node:dev
    container_name: chaos_node_3
    hostname: node3
    environment:
      NODE_ID: "3"
      PEERS: "1:node1:7000,2:node2:7000"
    ports:
      - "7003:7000"
      - "8083:8080"
      - "9003:9000"
    cap_add: [NET_ADMIN]
    networks: [raft_net]

networks:
  raft_net:
    driver: bridge
```

`NET_ADMIN` capability is required for `tc` and `iptables` inside containers.
The HTTP control plane port is published to the host (e.g. `8081:8080`) so that
the orchestrator can reach each node's `/health`, `/status`, and `/command`
endpoints directly from outside the Docker network.

### 6. C++ Orchestration Harness (`tests/docker_chaos/`)

**File layout**

```
tests/docker_chaos/
├── harness.hpp               # ChaosCluster, ChaosNode
├── fault_control.hpp         # fiu-ctrl wrappers, fault point constants
├── os_faults.hpp             # tc netem, iptables, docker kill wrappers
├── harness_unit_test.cpp     # ChaosCluster / ChaosNode unit tests
├── fault_control_unit_test.cpp  # HTTP and fiu-ctrl parsing unit tests
└── *_test.cpp                # scenario tests (Boost.Test)
```

**`ChaosCluster`**

```cpp
class ChaosCluster {
public:
    explicit ChaosCluster(std::string compose_file,
                          std::chrono::seconds startup_timeout = 30s);
    void start();   // docker compose up -d + health poll
    void stop();    // docker compose down --remove-orphans
    ChaosNode& node(int node_id);
    ChaosNode& wait_for_leader(std::chrono::seconds timeout = 10s);
    void assert_no_split_brain();
        // Polls /status on all nodes; BOOST_FAIL if two claim leadership
        // in the same term.

    // Log streaming: started in background on start(), readable at any time.
    std::vector<std::string> log_lines(int node_id) const;
    void wait_for_log(int node_id, std::string_view pattern,
                      std::chrono::seconds timeout = 5s);
};
```

**`ChaosNode`**

```cpp
class ChaosNode {
public:
    // State queries via published HTTP port
    nlohmann::json status();              // GET /status
    bool is_leader();
    bool is_healthy();                    // GET /health → 200
    nlohmann::json submit_command(std::string key, std::string value);
                                          // POST /command

    // Application-layer faults (fiu_rc_tcp)
    void enable_fault(std::string name,
                      std::string mode = "always",
                      double probability = 1.0);
    void disable_fault(std::string name);
    void disable_all_faults();

    // OS-layer network faults (docker exec into container)
    void apply_tc_netem(std::string loss = "0%",
                        std::string delay = "0ms",
                        std::string corrupt = "0%");
    void clear_tc_netem();
    void partition_from(std::vector<std::string> peer_ips); // iptables DROP
    void unpartition();                                      // flush iptables

    // Node lifecycle
    void kill();                          // docker kill (SIGKILL)
    void stop();                          // docker stop (SIGTERM + wait)
    void restart(bool wait = true);       // docker start + /health poll

    std::string container_ip() const;
};
```

**Fault point constants (`fault_control.hpp`)**

```cpp
// Mirrors the names in include/raft/fault_injection.hpp
inline constexpr std::string_view SEND_REQUEST_VOTE    = "raft/network/send_request_vote";
inline constexpr std::string_view SEND_APPEND_ENTRIES  = "raft/network/send_append_entries";
inline constexpr std::string_view SEND_INSTALL_SNAPSHOT= "raft/network/send_install_snapshot";
inline constexpr std::string_view SAVE_CURRENT_TERM    = "raft/persistence/save_current_term";
inline constexpr std::string_view SAVE_VOTED_FOR       = "raft/persistence/save_voted_for";
inline constexpr std::string_view APPEND_LOG_ENTRY     = "raft/persistence/append_log_entry";
inline constexpr std::string_view TRUNCATE_LOG         = "raft/persistence/truncate_log";
inline constexpr std::string_view SAVE_SNAPSHOT        = "raft/persistence/save_snapshot";
inline constexpr std::string_view STATE_MACHINE_APPLY  = "raft/state_machine/apply";
```

**Scenario test anatomy**

```cpp
// tests/docker_chaos/election_recovery_test.cpp
BOOST_FIXTURE_TEST_SUITE(docker_chaos_election, ChaosFixture)

BOOST_AUTO_TEST_CASE(election_after_iptables_partition) {
    ChaosCluster cluster("docker/docker-compose.yml");
    cluster.start();
    cluster.wait_for_leader(10s);

    auto& minority = cluster.node(3);
    minority.partition_from({cluster.node(1).container_ip(),
                             cluster.node(2).container_ip()});

    std::this_thread::sleep_for(400ms);  // 2 × election_timeout_max

    minority.unpartition();

    cluster.wait_for_leader(2s);
    cluster.assert_no_split_brain();
}

BOOST_AUTO_TEST_SUITE_END()
```

### 7. CMake Integration

```cmake
# ── Harness unit tests (no Docker required) ───────────────────
add_executable(docker_chaos_unit_tests
    tests/docker_chaos/harness_unit_test.cpp
    tests/docker_chaos/fault_control_unit_test.cpp)
target_link_libraries(docker_chaos_unit_tests
    PRIVATE Boost::unit_test_framework)
add_test(NAME docker_chaos_unit_tests
    COMMAND docker_chaos_unit_tests)

# ── Docker chaos scenario tests ───────────────────────────────
find_program(DOCKER_EXECUTABLE docker)

if(DOCKER_EXECUTABLE)
    set(CHAOS_NODE_COMPOSE_FILE
        "${CMAKE_SOURCE_DIR}/docker/docker-compose.yml")

    add_custom_target(docker-chaos-image
        COMMAND ${DOCKER_EXECUTABLE} build
                -t kythira-chaos-node:dev
                -f ${CMAKE_SOURCE_DIR}/docker/chaos_node/Dockerfile
                ${CMAKE_SOURCE_DIR}
        COMMENT "Building kythira chaos node Docker image"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_executable(docker_chaos_scenario_tests
        tests/docker_chaos/chaos_smoke_test.cpp
        tests/docker_chaos/election_recovery_test.cpp
        tests/docker_chaos/crash_recovery_test.cpp
        tests/docker_chaos/network_degradation_test.cpp
        tests/docker_chaos/az_partition_test.cpp
        tests/docker_chaos/persistence_faults_test.cpp
        tests/docker_chaos/safety_assertions_test.cpp)
    target_link_libraries(docker_chaos_scenario_tests
        PRIVATE Boost::unit_test_framework)

    add_custom_target(docker-chaos-tests
        COMMAND ${CMAKE_COMMAND} -E env
                KYTHIRA_COMPOSE_FILE=${CHAOS_NODE_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_scenario_tests>
        DEPENDS docker-chaos-image docker_chaos_scenario_tests
        COMMENT "Running Docker chaos scenario tests"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
else()
    foreach(_t docker-chaos-image docker-chaos-tests)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo
                    "docker-chaos requires docker"
            COMMAND ${CMAKE_COMMAND} -E false)
    endforeach()
endif()
```

## Trade-offs

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| TCP transport model | Blocking per-call | Async (asio/folly IOThreadPoolExecutor) | Sufficient for chaos testing; avoids new async dependency; simpler to debug |
| Persistence engine | File + atomic rename | SQLite, RocksDB | No new dependencies; atomic rename is correct and debuggable; sufficient throughput for test workloads |
| HTTP control plane | cpp-httplib (already in vcpkg) | gRPC, custom TCP | Already a project dependency; single-header; adequate for test-rate traffic |
| fiu_rc port | Separate from RPC and HTTP | Multiplexed on HTTP | Clean separation; `fiu-ctrl` CLI expects a dedicated port; no protocol conflicts |
| OS-layer faults | docker exec tc/iptables | Dedicated network emulation container (e.g., Pumba) | Fewer moving parts; `NET_ADMIN` + `docker exec` is sufficient; no third-party chaos agent required |
| C++ harness | `popen` / `system` + cpp-httplib client | Separate Python/Ansible test driver | Single language across harness and production code; same toolchain; no Python runtime dependency in CI |
| Node config | Environment variables | Config file | Natural fit for Docker Compose; no volume mount needed for configuration |
| Log truncation | Rewrite on truncate | Sparse file / segment files | Correctness over performance; test workloads are small; simplifies implementation |
| HTTP port exposure | Published to host via `ports:` in Compose | Internal network only | Orchestrator runs on host and must reach each node's control plane without entering the Docker network |

## Components and Interfaces

| Component | Location | Interface |
|---|---|---|
| `chaos_node` binary | `cmd/chaos_node/` | Reads env vars; exposes RPC `:7000`, HTTP `:8080`, fiu `:9000` |
| `tcp_rpc_client` | `include/raft/tcp_rpc.hpp` | Satisfies `kythira::network_client` concept |
| `tcp_rpc_server` | `include/raft/tcp_rpc.hpp` | Satisfies `kythira::network_server` concept |
| `file_persistence_engine` | `include/raft/file_persistence.hpp` | Satisfies `kythira::persistence_engine` concept |
| HTTP control plane | `cmd/chaos_node/http_control.hpp` | `GET /health`, `GET /status`, `POST /command`, `GET /log/:index` |
| `ChaosCluster` | `tests/docker_chaos/harness.hpp` | `start()`, `stop()`, `node(id)`, `wait_for_leader()`, `assert_no_split_brain()`, `log_lines()`, `wait_for_log()` |
| `ChaosNode` | `tests/docker_chaos/harness.hpp` | `status()`, `is_leader()`, `enable_fault()`, `apply_tc_netem()`, `partition_from()`, `kill()`, `restart()` |
| Fault point constants | `tests/docker_chaos/fault_control.hpp` | `constexpr std::string_view` names mirroring `fault_injection.hpp` |

### Key interface invariants

- `ChaosCluster::start()` does not return until all nodes report `GET /health` → 200 or the startup timeout elapses (throws on timeout).
- `ChaosNode::enable_fault()` sends a `fiu-ctrl` command to the node's published fiu port; the fault is active before the function returns.
- `ChaosNode::partition_from()` installs `iptables DROP` rules via `docker exec`; the partition is in effect before the function returns.
- All HTTP calls in `ChaosNode` target the published host port (e.g. `localhost:8081`), not the container-internal port.

## Data Models

### Node status response (`GET /status`)

```json
{
  "node_id": 1,
  "role": "leader",
  "term": 4,
  "leader_id": 1,
  "commit_index": 12,
  "last_applied": 12
}
```

### Command request/response (`POST /command`)

Request body:
```json
{ "key": "x", "value": "42" }
```

Success response (HTTP 200):
```json
{ "success": true, "commit_index": 13 }
```

Not-leader response (HTTP 503):
```json
{ "error": "not_leader", "leader_id": 2 }
```

### Log entry response (`GET /log/:index`)

```json
{ "index": 3, "term": 2, "command": "<base64 or JSON payload>" }
```

### `NodeConfig` (parsed from environment at startup)

```cpp
struct NodeConfig {
    uint64_t    node_id;
    uint16_t    rpc_port          = 7000;
    uint16_t    http_port         = 8080;
    uint16_t    fiu_port          = 9000;
    std::string data_dir          = "/var/lib/chaos_node";
    std::vector<PeerConfig> peers; // {id, host, port}
    std::chrono::milliseconds election_timeout_min{150};
    std::chrono::milliseconds election_timeout_max{300};
    std::chrono::milliseconds heartbeat_interval{50};
};
```

## Correctness Properties

These properties are asserted by the scenario tests and enforced by `assert_no_split_brain()`. They mirror the properties exercised by the in-process `tests/chaos/` suite; the docker harness verifies they hold under real OS-layer faults where the simulator cannot reach.

### Property 1: Election Safety

At most one leader per term across all nodes at any point in time. `assert_no_split_brain()` polls `/status` on all nodes and fails if two report `"role":"leader"` with the same term value.

**Validates: Requirements 8.1, 8.7**

### Property 2: Leader Completeness

A newly elected leader holds all committed log entries from prior terms. Verified after each leadership transition by comparing `commit_index` and spot-checking log entries via `GET /log/:index`.

**Validates: Requirements 8.3, 8.7**

### Property 3: Log Matching

If two nodes have a log entry with the same index and term, all preceding entries are identical. Checked after fault injection by querying each node's log tail and asserting prefix equality.

**Validates: Requirements 8.7**

### Property 4: Liveness After Partition Heal

Within `2 × election_timeout_max` after a network partition heals, a leader is elected. `wait_for_leader()` enforces this bound and fails the test if it elapses without a leader.

**Validates: Requirements 7.3, 8.1**

### Property 5: Crash Recovery

After a node is killed (`SIGKILL`) and restarted, it recovers its term, voted-for, and log from `DATA_DIR` and rejoins the cluster without violating safety. Verified by submitting commands before and after the kill and confirming commit continuity.

**Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 8.2, 8.3**

## Error Handling

| Failure | Harness response |
|---|---|
| `start()` health poll timeout | Throws `std::runtime_error`; `stop()` is called in RAII destructor to clean up containers |
| `wait_for_leader()` timeout | Throws; test fails with a descriptive message including each node's last-known status |
| HTTP call failure (connection refused, 5xx) | `ChaosNode` method throws; test fails; containers left running for post-mortem log inspection |
| `fiu-ctrl` TCP connection refused | `enable_fault()` throws; indicates `chaos_node` fiu port is not reachable (startup bug) |
| `docker exec` non-zero exit | `apply_tc_netem()` / `partition_from()` throw; indicates missing `NET_ADMIN` or missing tool in container |
| Container exits unexpectedly | Detected on next HTTP call (connection refused); `ChaosNode::is_healthy()` returns `false` |

The harness destructor always runs `docker compose down --remove-orphans` regardless of test outcome to prevent container leaks across test runs.

## Testing Strategy

The docker chaos tests are organised in three layers:

**Harness unit tests** (`harness_unit_test.cpp`, `fault_control_unit_test.cpp`): exercise `ChaosCluster`, `ChaosNode`, and the HTTP/fiu-ctrl parsing logic in isolation — no Docker required. Subprocess calls and HTTP calls are replaced with injectable stubs so that lifecycle transitions, fault command construction, and JSON response parsing are verified without starting any containers. Registered as a CTest target (`docker_chaos_unit_tests`) and included in the standard `ctest` run.

| File | What is tested |
|---|---|
| `harness_unit_test.cpp` | `ChaosCluster` construction (no containers started), `start()`/`stop()` lifecycle, `wait_for_leader()` returning the correct node, `TimeoutError` on missing leader; `ChaosNode` fault methods issue correct commands and propagate non-zero exit errors |
| `fault_control_unit_test.cpp` | HTTP response parsing for `/health`, `/status`, `/command`, `/log/<index>`; fiu-ctrl command construction for `"always"`, `"random"`, `"once"` modes |

**Smoke test** (`chaos_smoke_test.cpp`): verifies the harness itself end-to-end — that a cluster can start, elect a leader, accept a command, and shut down cleanly. Must pass before any fault-injection scenario tests are run.

**Scenario tests** (one file per fault class):

| File | Fault class | Key assertion |
|---|---|---|
| `election_recovery_test.cpp` | iptables partition + heal | Leader elected within 2× timeout after heal |
| `crash_recovery_test.cpp` | `docker kill` + `docker start` | Killed node rejoins; no split brain; prior commits visible |
| `network_degradation_test.cpp` | `tc netem` loss/latency | Cluster remains available; leader unchanged or re-elected |
| `az_partition_test.cpp` | Two-node partition (minority isolated) | Majority partition retains leader; minority makes no progress |
| `persistence_faults_test.cpp` | `fiu-ctrl` on `save_current_term`, `append_log_entry` | Node handles errors gracefully; safety properties hold |
| `safety_assertions_test.cpp` | Mixed faults | Election safety and log matching hold throughout |

Each test file uses a `ChaosFixture` that starts and stops a fresh cluster per test case, ensuring test isolation. The log stream captured by `ChaosCluster` is written to a per-test artefact directory on failure to aid post-mortem analysis.
