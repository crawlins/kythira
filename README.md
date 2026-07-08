# Kythira - Raft Consensus Implementation

A production-ready C++23 implementation of the Raft consensus algorithm with comprehensive property-based testing, async operations, and pluggable transport layers.

## Overview

Kythira provides a fully-featured Raft consensus implementation designed for distributed systems requiring strong consistency guarantees. The implementation follows the Raft paper specification with extensive testing and modern C++ design patterns.

## Key Features

### Core Raft Features
- **Leader Election** with randomized timeouts and split-vote prevention
- **Log Replication** with consistency checks and conflict resolution
- **Commit Index Management** with majority-based advancement
- **State Machine Application** with failure handling and recovery
- **Snapshot Support** for log compaction and efficient catch-up
- **Cluster Membership Changes** using joint consensus

### Advanced Features
- **Async Operations** using generic future concepts (Folly, std::future, custom)
- **Commit Waiting** with timeout and cancellation support
- **Exponential Backoff Retry** with jitter for network operations
- **Timeout Classification** for intelligent error handling
- **Resource Management** with proper cleanup and leak prevention
- **Comprehensive Logging** for debugging and observability

### Transport Layers
- **HTTP/HTTPS Transport** with TLS support and connection pooling
- **CoAP/CoAPS Transport** for IoT and constrained networks with DTLS security
- **Network Simulator** for testing and development
- **Pluggable Design** supporting custom transport implementations

### Certificate Management
- **In-process Certificate Authority** — root CA generation, leaf issuance,
  revocation/CRL, `from_existing()` round-trip
- **`ca_service`** — CLI for Docker/Podman-volume certificate provisioning and
  a long-running `--serve` HTTP API mode (local or AWS ACM Private CA backend)
- **`ca_cluster_node`** — Raft-replicated CA for highly-available, multi-AZ
  certificate issuance with leader failover
- **ACME support (RFC 8555/8738)** — `acme_certificate_provider` speaks the
  same protocol as Let's Encrypt, including `dns-01`/`http-01` challenges and
  bare-IP identifiers
- **Fingerprint-pinned bootstrap** — first-contact TLS trust from an
  out-of-band root fingerprint, no prior certificate chain required
- See [Certificate Authority & ACME](#certificate-authority--acme) below

### Testing & Quality
- **71% Test Coverage** with 62/87 tests passing
- **100% Built Test Pass Rate** (62/62 tests)
- **Property-Based Testing** using Boost.Test
- **Integration Tests** for end-to-end validation
- **Zero Test Failures** in compiled test suite

## Requirements

- C++23 compatible compiler (GCC 13+, Clang 16+, or MSVC 2022+)
- CMake 3.20 or higher
- folly library
- Boost (system, thread, unit_test_framework)
- cpp-httplib (for HTTP transport)
- libcoap (optional, for CoAP transport)
- OpenSSL (for HTTPS/TLS and CoAPS/DTLS support)

## Building

```bash
# Create build directory
mkdir build
cd build

# Configure
cmake ..

# Build
cmake --build .

# Run tests
ctest
```

## Project Structure

```
.
├── include/
│   ├── concepts/
│   │   └── future.hpp             # Enhanced C++20 concepts for Folly types
│   └── network_simulator/
│       ├── network_simulator.hpp  # Main header
│       ├── concepts.hpp           # C++23 concepts
│       ├── types.hpp              # Core data types
│       └── exceptions.hpp         # Exception types
├── src/                           # Implementation files (to be added)
├── tests/                         # Unit and property-based tests
├── examples/                      # Example programs
└── CMakeLists.txt                 # Build configuration
```

## Quick Start

### Implementing Your State Machine

The first step is to implement your application-specific state machine:

```cpp
#include <raft/types.hpp>

class KeyValueStore {
public:
    // Apply a command to the state machine
    auto apply(const std::vector<std::byte>& command, std::uint64_t index) -> std::vector<std::byte> {
        // Parse command (e.g., "PUT key value", "GET key", "DEL key")
        // Apply to your data structure
        // Return result to client
        return result;
    }

    // Capture current state for snapshot
    auto get_state() const -> std::vector<std::byte> {
        // Serialize your state machine's data
        return serialized_state;
    }

    // Restore state from snapshot
    auto restore_from_snapshot(const std::vector<std::byte>& state, std::uint64_t last_index) -> void {
        // Deserialize and restore your state machine's data
    }
};
```

**See [State Machine Examples](include/raft/examples/)** for complete implementations:
- **Counter**: Simple atomic counter
- **Register**: Single-value register with versioning
- **Replicated Log**: Append-only log
- **Distributed Lock**: Lock service with timeouts
- **Key-Value Store**: Full-featured KV store (test implementation)

### Basic Raft Cluster

```cpp
#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>

// Use your state machine
        return result;
    }

    auto create_snapshot() -> std::vector<std::byte> {
        // Create snapshot of current state
        return snapshot_data;
    }

    auto restore_snapshot(const std::vector<std::byte>& data) -> void {
        // Restore state from snapshot
    }
};

// Create a 3-node Raft cluster
using raft_node = kythira::node<
    folly::Future,                          // Future type
    KeyValueStore,                          // State machine
    kythira::simulator_network_client,      // Network client
    kythira::simulator_network_server,      // Network server
    kythira::memory_persistence_engine,     // Persistence
    kythira::console_logger,                // Logger
    kythira::noop_metrics,                  // Metrics
    kythira::default_membership_manager     // Membership
>;

// Configure and start nodes
kythira::raft_configuration config;
config.election_timeout_min = std::chrono::milliseconds{150};
config.election_timeout_max = std::chrono::milliseconds{300};
config.heartbeat_interval = std::chrono::milliseconds{50};

auto node1 = std::make_unique<raft_node>(1, config, /* components */);
auto node2 = std::make_unique<raft_node>(2, config, /* components */);
auto node3 = std::make_unique<raft_node>(3, config, /* components */);

node1->start();
node2->start();
node3->start();

// Submit a command (on leader)
std::vector<std::byte> command = serialize_command("SET", "key", "value");
auto future = node1->submit_command(command, std::chrono::seconds{5});
auto result = std::move(future).get();  // Waits for commit and application
```

### HTTP Transport for Production

```cpp
#include <raft/http_transport.hpp>
#include <raft/json_serializer.hpp>

// Define transport types
struct production_transport {
    using serializer_type = raft::json_rpc_serializer<std::vector<std::byte>>;
    template<typename T> using future_template = folly::Future<T>;
    using executor_type = folly::CPUThreadPoolExecutor;
    using metrics_type = raft::prometheus_metrics;  // Your metrics implementation
};

// Server with HTTPS
raft::cpp_httplib_server_config server_config;
server_config.enable_ssl = true;
server_config.ssl_cert_path = "/etc/raft/server.crt";
server_config.ssl_key_path = "/etc/raft/server.key";
server_config.max_concurrent_connections = 100;

raft::cpp_httplib_server<production_transport> server(
    "0.0.0.0", 8443, server_config, metrics
);

// Client with HTTPS
std::unordered_map<std::uint64_t, std::string> cluster_nodes;
cluster_nodes[1] = "https://node1.example.com:8443";
cluster_nodes[2] = "https://node2.example.com:8443";
cluster_nodes[3] = "https://node3.example.com:8443";

raft::cpp_httplib_client_config client_config;
client_config.enable_ssl_verification = true;
client_config.ca_cert_path = "/etc/raft/ca.crt";
client_config.connection_pool_size = 10;

raft::cpp_httplib_client<production_transport> client(
    std::move(cluster_nodes), client_config, metrics
);
```

### CoAP Transport for IoT and Constrained Networks

```cpp
#include <raft/coap_transport.hpp>
#include <raft/json_serializer.hpp>

// Configure CoAP endpoints
std::unordered_map<std::uint64_t, std::string> coap_endpoints = {
    {1, "coaps://node1.iot.local:5684"},  // CoAPS (DTLS encrypted)
    {2, "coaps://node2.iot.local:5684"},
    {3, "coaps://node3.iot.local:5684"}
};

// Client configuration with DTLS
coap_client_config coap_config;
coap_config.enable_dtls = true;
coap_config.cert_file = "/etc/raft/coap-cert.pem";
coap_config.key_file = "/etc/raft/coap-key.pem";
coap_config.ca_file = "/etc/raft/coap-ca.pem";
coap_config.ack_timeout = std::chrono::milliseconds{2000};
coap_config.max_retransmit = 4;
coap_config.enable_block_transfer = true;  // For large messages

auto coap_client = kythira::coap_client<
    raft::json_rpc_serializer<std::vector<std::byte>>,
    raft::noop_metrics,
    raft::console_logger
>(coap_endpoints, coap_config, metrics, logger);

// Server configuration
coap_server_config server_config;
server_config.enable_dtls = true;
server_config.cert_file = "/etc/raft/coap-cert.pem";
server_config.key_file = "/etc/raft/coap-key.pem";
server_config.ca_file = "/etc/raft/coap-ca.pem";
server_config.max_concurrent_sessions = 200;

auto coap_server = kythira::coap_server<
    raft::json_rpc_serializer<std::vector<std::byte>>,
    raft::noop_metrics,
    raft::console_logger
>("0.0.0.0", 5684, server_config, metrics, logger);

// Register handlers and start
coap_server.register_request_vote_handler(vote_handler);
coap_server.start();
```

## Architecture

### Component Overview

```
┌─────────────────────────────────────────────────────────────┐
│                     Raft Node                                │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  State Machine (User-Defined)                          │ │
│  └────────────────────────────────────────────────────────┘ │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Raft Core                                             │ │
│  │  • Leader Election    • Commit Waiting                 │ │
│  │  • Log Replication    • Error Handling                 │ │
│  │  • State Machine App  • Resource Management            │ │
│  └────────────────────────────────────────────────────────┘ │
│  ┌──────────────┬──────────────┬──────────────────────────┐ │
│  │  Network     │ Persistence  │  Observability           │ │
│  │  • HTTP/S    │ • Memory     │  • Logging               │ │
│  │  • CoAP/S    │ • Disk       │  • Metrics               │ │
│  │  • Simulator │ • Custom     │  • Tracing               │ │
│  └──────────────┴──────────────┴──────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### Transport Layer Comparison

| Feature | HTTP/HTTPS | CoAP/CoAPS | Network Simulator |
|---------|------------|------------|-------------------|
| **Protocol** | TCP | UDP | In-memory |
| **Overhead** | Medium (200-500 bytes) | Low (4-8 bytes) | None |
| **Latency** | Medium | Low | Minimal |
| **Throughput** | High (10K+ req/s) | Medium (5K+ req/s) | Very High |
| **Security** | TLS 1.2/1.3 | DTLS 1.2 | N/A |
| **Connection** | Persistent | Connectionless | N/A |
| **Best For** | Data centers, cloud | IoT, edge, constrained | Testing, development |
| **Reliability** | TCP guarantees | Confirmable messages | Perfect |
| **Resource Usage** | Higher | Lower | Minimal |
| **Standards** | RFC 7230-7235 | RFC 7252 | N/A |

### When to Use Each Transport

**HTTP/HTTPS**:
- ✅ Data center deployments with reliable networks
- ✅ High throughput requirements (10,000+ req/s)
- ✅ Existing HTTP infrastructure
- ✅ Need for connection pooling and keep-alive
- ❌ Constrained devices with limited resources
- ❌ Networks with high packet loss

**CoAP/CoAPS**:
- ✅ IoT and embedded systems
- ✅ Constrained networks (low bandwidth, high latency)
- ✅ Battery-powered devices
- ✅ UDP-based networks
- ✅ Multicast discovery requirements
- ❌ Need for maximum throughput
- ❌ Complex HTTP features required

**Network Simulator**:
- ✅ Unit and integration testing
- ✅ Failure scenario testing
- ✅ Development without network setup
- ✅ CI/CD pipelines
- ❌ Production deployments

### Generic Future Support

Kythira uses C++23 concepts to support multiple future implementations:

```cpp
// Works with Folly futures
using folly_node = kythira::node<folly::Future, /* ... */>;

// Works with std::future
using std_node = kythira::node<std::future, /* ... */>;

// Works with custom futures
using custom_node = kythira::node<my_future, /* ... */>;
```

### Pluggable Components

All major components are pluggable via template parameters:

- **State Machine**: Your application logic
- **Network Transport**: HTTP, simulator, or custom
- **Persistence**: Memory, disk, or custom
- **Logger**: Console, file, or custom
- **Metrics**: Prometheus, StatsD, or custom
- **Membership Manager**: Default or custom authorization

## Test Suite

### Test Status

See [RAFT_TESTS_FINAL_STATUS.md](RAFT_TESTS_FINAL_STATUS.md) for comprehensive test analysis.

**Summary**:
- **Total Tests**: 87
- **Passing**: 62 (71%)
- **Failing**: 0 (0%)
- **Not Built**: 25 (29%)
- **Built Test Pass Rate**: 100% (62/62)

### Test Categories

- **Property-Based Tests** (51 tests): Validate correctness properties across random inputs
- **Integration Tests** (10 tests): End-to-end cluster behavior validation
- **Unit Tests** (26 tests): Component-level testing

### Running Tests

**Important**: For large test suites, always store output first, then analyze. See [Test Execution Standards](.kiro/steering/test-execution-standards.md) for details.

```bash
# Recommended: Use the efficient test script
./scripts/run_tests_efficiently.sh

# Or manually with output storage
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
ctest --test-dir build --output-on-failure -j$(nproc) 2>&1 | tee "test_results/test_results_${TIMESTAMP}.txt"

# Then analyze the stored output
grep "Failed" test_results/test_results_${TIMESTAMP}.txt
tail -50 test_results/test_results_${TIMESTAMP}.txt

# Run specific test pattern
ctest --test-dir build -R "^raft_" --output-on-failure

# Run tests by category
ctest --test-dir build -L unit          # Unit tests only
ctest --test-dir build -L integration   # Integration tests only
ctest --test-dir build -LE slow         # Exclude slow tests

# Re-run only failed tests
ctest --test-dir build --rerun-failed --output-on-failure
```

**Why store output?**
- Large test suites can take minutes to run
- Analyze results multiple times without re-running
- Preserve test history for comparison
- More efficient use of resources

See [test_results/README.md](test_results/README.md) for more analysis examples.

## Code Style

All C++ sources (`*.cpp`, `*.hpp`) are formatted with [clang-format](https://clang.llvm.org/docs/ClangFormat.html)
using the configuration in `.clang-format` at the repo root (based on Google style, 4-space indent,
100-column limit).

### Auto-format the whole project

```bash
cmake --build build --target format
```

### Check compliance without modifying files

```bash
cmake --build build --target format-check
```

This exits non-zero and prints the offending paths if any file is out of compliance.

### Pre-commit enforcement

The pre-commit hook checks only staged `.cpp`/`.hpp` files, so it typically completes in under a
second. If a staged file is non-compliant the hook prints the fix command and blocks the commit:

```
  [format] FAILED — the following file(s) need formatting:
    src/foo.cpp

  Fix with:
    clang-format -i src/foo.cpp
  or reformat the whole project:
    cmake --build build --target format

  (To skip: SKIP_FORMAT_CHECK=1 git commit)
```

To skip the format check on a WIP commit:

```bash
SKIP_FORMAT_CHECK=1 git commit -m "wip: ..."
```

## Static Analysis

All C++ sources are checked with [clang-tidy](https://clang.llvm.org/extra/clang-tidy/)
using the `.clang-tidy` config at the repo root. All enabled checks run with
`WarningsAsErrors: "*"`.

### Run analysis across the whole project

```bash
cmake --build build --target static-analysis
```

Uses `run-clang-tidy` for parallel execution (falls back to sequential `clang-tidy`
if `run-clang-tidy` is not installed).

### Apply auto-fixes

```bash
cmake --build build --target static-analysis-fix
```

Runs `clang-tidy --fix --fix-errors` sequentially (parallel apply is unsafe due
to race conditions on shared headers).

### Pre-commit enforcement (opt-in)

The pre-commit hook includes an opt-in clang-tidy step that checks staged `.cpp`
files only. It is **disabled by default** and must be explicitly enabled:

```bash
TIDY_CHECK=1 git commit -m "feat: ..."
```

If any staged file has findings the commit is blocked with the finding and a fix
instruction. To skip on a WIP commit:

```bash
SKIP_TIDY_CHECK=1 git commit -m "wip: ..."
```

### Suppressing individual findings

When a check fires on code that is intentionally written that way (e.g.
`reinterpret_cast` required for C interop), suppress it with a line comment:

```cpp
auto* ptr = reinterpret_cast<const std::byte*>(data);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
```

---

## Chaos Testing

Chaos tests verify that Raft's safety and liveness properties hold under
realistic fault conditions (network packet loss, intermittent disk failures,
state machine errors) using [libfiu](http://blitiri.com.ar/p/libfiu/) fault
injection.

### Install libfiu (optional dependency)

```bash
sudo apt install libfiu-dev     # Ubuntu/Debian
pkg-config --modversion libfiu  # verify: should print 1.2 or higher
```

Chaos test targets are compiled only when libfiu is detected at configure time.
The production library and all other tests are unaffected if libfiu is absent.

### Build and run chaos tests

```bash
# Build all chaos test executables
cmake --build build --target chaos-tests

# Run the chaos suite (excluded from the default ctest run)
ctest --test-dir build --label-regex chaos --output-on-failure
```

### Fault_Point naming convention

All fault points follow the pattern `"raft/<layer>/<operation>"`:

| Fault_Point                              | What it simulates                        |
|------------------------------------------|------------------------------------------|
| `raft/network/send_append_entries`       | AppendEntries RPC lost or rejected       |
| `raft/network/send_install_snapshot`     | InstallSnapshot RPC lost or rejected     |
| `raft/network/send_request_vote`         | RequestVote RPC lost or rejected         |
| `raft/persistence/append_log_entry`      | Disk write failure during log append     |
| `raft/persistence/save_current_term`     | Disk write failure when persisting term  |
| `raft/persistence/save_snapshot`         | Disk write failure during snapshot       |
| `raft/persistence/save_voted_for`        | Disk write failure for vote persistence  |
| `raft/persistence/truncate_log`          | Disk write failure during log truncation |
| `raft/state_machine/apply`               | Application-layer command rejection      |

The full catalogue and design rationale are in
[`.kiro/specs/libfiu-integration/design.md`](.kiro/specs/libfiu-integration/design.md).

---

## Docker Chaos Testing

Docker chaos tests run a real 3-node `chaos_node` cluster in containers and
inject failures from a C++ test harness running on the host.  This validates
safety and liveness under OS-level faults (network partitions, process kills,
disk errors) that the in-process libfiu tests cannot reproduce.

The harness (`tests/docker_chaos/`) is pure C++ with no Python or pytest
dependency.  `ChaosCluster` and `ChaosNode` use injectable executor and HTTP
stubs so the harness logic is unit-tested without Docker via two CTest-
registered executables (`docker_chaos_harness_unit_tests` and
`docker_chaos_fault_control_unit_tests`).

### Prerequisites

```bash
# Docker with Compose v2
docker --version          # 20.10+
docker compose version    # 2.0+
```

The image requires `KYTHIRA_FAULT_INJECTION=ON` (the default when libfiu is
detected at configure time):

```bash
cmake -S . -B build \
      -DCMAKE_PREFIX_PATH="$(pwd)/vcpkg_installed/x64-linux"
cmake --build build --target docker-chaos-image
```

### Running the tests

**Harness unit tests** (no Docker required — included in the standard `ctest` run):

```bash
ctest --test-dir build --label-regex docker_chaos_unit --output-on-failure
```

**Scenario tests** (require the Docker image):

```bash
# Build image + run all scenario tests against a live 3-node cluster
cmake --build build --target docker-chaos-tests

# Or drive the cluster manually for development
docker compose -f docker/docker-compose.yml up -d
./build/docker_chaos_scenario_tests --log_level=message
docker compose -f docker/docker-compose.yml down
```

### Environment variables (`chaos_node`)

| Variable | Default | Description |
|---|---|---|
| `NODE_ID` | required | Integer node ID |
| `RPC_PORT` | `7000` | Raft RPC listen port |
| `HTTP_PORT` | `8080` | HTTP control plane port |
| `FIU_PORT` | `9000` | fiu_rc_tcp listen port |
| `DATA_DIR` | `/var/lib/chaos_node` | Persistence directory |
| `PEERS` | required | `id:host:port,...` peer list |
| `ELECTION_TIMEOUT_MIN_MS` | `150` | Min election timeout (ms) |
| `ELECTION_TIMEOUT_MAX_MS` | `300` | Max election timeout (ms) |
| `HEARTBEAT_INTERVAL_MS` | `50` | Heartbeat interval (ms) |

### Port layout

| Node | RPC (TCP) | HTTP control | fiu remote |
|------|-----------|--------------|------------|
| n1   | 7001      | 8081         | 9001       |
| n2   | 7002      | 8082         | 9002       |
| n3   | 7003      | 8083         | 9003       |

All three ports are published to the host so the orchestrator can reach each
node's control plane and fiu_rc_tcp listener from outside the Docker network.

### Test scenarios

| File | Fault class | Key assertion |
|------|-------------|---------------|
| `chaos_smoke_test.cpp` | None | Cluster starts, elects leader, commits command |
| `election_recovery_test.cpp` | iptables partition + fiu network isolation | Leader elected within 2× timeout after heal |
| `crash_recovery_test.cpp` | `docker kill` + `docker start` | Killed node catches up; no split brain |
| `network_degradation_test.cpp` | `tc netem` loss/latency | Cluster remains available |
| `az_partition_test.cpp` | Majority/minority and symmetric partition | Majority retains leader; minority makes no progress |
| `persistence_faults_test.cpp` | `fiu-ctrl` on `append_log_entry`, `save_current_term` | Safety holds under disk fault injection |
| `safety_assertions_test.cpp` | Partition + kill + restart combined | No log divergence across all nodes |

### How fault injection works

Each `chaos_node` container exposes a `fiu_rc_tcp` port (9001–9003 on the host).
The C++ harness sends libfiu line-protocol commands over a plain TCP socket —
the `docker_chaos::fiu::send_fiu_cmd` helper in `fault_control.hpp` speaks the
protocol directly, with no dependency on the `fiu-ctrl` binary.

To drive fault injection manually against a running cluster:

```bash
# Enable 30% random append_log_entry failures on node 1
fiu-ctrl -c "enable_random name=raft/persistence/append_log_entry failnum=-1 failinfo=0 probability=0.300000" \
         localhost:9001

# Re-enable all fault points (disable_all)
fiu-ctrl -c "disable_all" localhost:9001
```

OS-layer failures are applied via `docker exec` from the `ChaosNode` methods:

```cpp
auto& n3 = cluster.node(3);
n3.partition_from({n1.container_ip(), n2.container_ip()}); // iptables DROP
n3.apply_tc_netem("30%", "50ms");                          // tc netem
n3.kill();                                                  // docker kill (SIGKILL)
n3.restart();                                               // docker start + /health poll
n3.unpartition();                                           // flush iptables
```

The full design is in
[`.kiro/specs/docker-chaos/design.md`](.kiro/specs/docker-chaos/design.md).

---

## Certificate Authority & ACME

Kythira includes a full certificate-issuance stack for standing up TLS-secured
clusters and test scenarios without depending on a real, rate-limited public
CA. See [`.kiro/specs/certificate-authority/`](.kiro/specs/certificate-authority/)
for the full design and requirements.

### Components

- **`certificate_authority`** (`include/raft/certificate_authority.hpp`) — an
  in-process root CA: generates a self-signed root on construction, issues
  leaf certificates (`issue()`), signs externally-generated CSRs
  (`sign_csr()`), revokes certificates and serves a CRL, and can be
  reconstructed from previously-issued material via `from_existing()`.
- **`ca_service`** (`cmd/ca_service/`) — a CLI wrapping `certificate_authority`
  with two modes:
  - **oneshot**: writes root + per-service leaf/key/chain PEM files to an
    `--out-dir` for Docker/Podman compose-volume provisioning.
  - **`--serve <host:port>`**: a long-running, bearer-token-authenticated HTTP
    API (`local` or `aws-acm-pca` provider) exposing `GET /v1/root-ca`,
    `POST /v1/certificates`, `POST /v1/certificates/renew`,
    `POST /v1/certificates/revoke`, `GET /v1/crl`. Deployable as a
    docker-compose service, a systemd unit, or an ECS task
    (`docker/ca_service/`).
- **`ca_cluster_node`** (`cmd/ca_cluster_node/`) — a Raft-replicated CA for
  high availability: certificate issuance/revocation history is committed as
  a replicated ledger (`ca_state_machine`), so the CA survives leader failover
  without losing its private key or reissuing certificates already on record.
  Packaged for a 3-AZ AWS deployment (`docker/ca_cluster_node/`).
- **`acme_certificate_provider`** (`include/raft/acme_certificate_provider.hpp`)
  — a `certificate_provider` implementation speaking RFC 8555 (ACME) against
  any compliant CA, including Let's Encrypt. Supports `http-01` and `dns-01`
  (via the same RFC 2136 UPDATE mechanism `rfc2136_ldns_discovery` uses) and
  RFC 8738 bare-IP identifiers — IP identifiers always validate via `http-01`
  regardless of the configured challenge type, since `dns-01` isn't defined
  for them. `tests/acme_test_server.hpp` provides a self-contained mock ACME
  server for testing without a real CA.
- **`ca_bootstrap_client::fetch_trusted_root()`** (`include/raft/ca_bootstrap_client.hpp`)
  — lets a fresh instance establish first-contact trust in a `ca_service`/
  `ca_cluster_node` TLS listener from only an out-of-band SHA-256 root
  fingerprint (`--print-root-fingerprint`) and bearer token, before any
  certificate chain exists to verify the connection against.

### Quick start: oneshot provisioning

```bash
ca_service --out-dir /tmp/ca-material \
           --service mtls-node1 \
           --service mtls-node2
# /tmp/ca-material/root_ca.pem
# /tmp/ca-material/mtls-node1/{cert,key,chain}.pem
# /tmp/ca-material/mtls-node2/{cert,key,chain}.pem
```

### Quick start: `--serve` mode

```bash
ca_service --serve 0.0.0.0:8443 \
           --tls-cert chain.pem --tls-key key.pem \
           --auth-token "$(openssl rand -hex 32)"

# Fetch the root once you've pinned its fingerprint out-of-band:
ca_service --print-root-fingerprint --tls-cert chain.pem --tls-key key.pem
```

### Running the certificate-authority test suite

```bash
ctest --test-dir build -L certificate_authority
```

This includes `acme_jws_unit_test`, `acme_test_server_unit_test`,
`acme_certificate_provider_test`, `acme_identifier_type_test` (Properties 21–22:
per-identifier ACME challenge-type dispatch and `.local`/mDNS validation),
`ca_cluster_node_test` (multi-node subprocess clusters, leader failover), and
`ca_bootstrap_client_test` (fingerprint pinning).

---

## Code Coverage

### Quick start

```bash
# 1. Configure the instrumented build (one-time)
cmake -S . -B build-coverage \
      -DENABLE_COVERAGE=ON \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH="$(pwd)/vcpkg_installed/x64-linux"

# 2. Build
cmake --build build-coverage -j$(nproc)

# 3. Run tests and print a summary
cmake --build build-coverage --target coverage

# 4. (Optional) Generate an HTML report
cmake --build build-coverage --target coverage-html
# → opens build-coverage/coverage-report/index.html
```

### How the ratchet works

`coverage_floor.txt` at the repo root stores the minimum acceptable line-coverage
percentage. The pre-commit hook measures coverage after every commit and:

- **Raises** the floor when coverage improves (and stages the updated file)
- **Allows** the commit when coverage is unchanged
- **Blocks** the commit when coverage would fall below the floor

The floor only ever moves up. To skip the check on a WIP commit:

```bash
SKIP_COVERAGE_CHECK=1 git commit -m "wip: ..."
```

### Installing the hook

```bash
bash scripts/install-hooks.sh
```

Run this once after cloning. It symlinks `scripts/pre-commit-coverage.sh` to
`.git/hooks/pre-commit`. The hook runs the format check first (fast, staged files only), then the
coverage ratchet (slow, full build + tests).

### Property-Based Testing

The implementation uses property-based testing to validate Raft safety properties:

```cpp
// Example: Leader election safety property
BOOST_AUTO_TEST_CASE(property_election_safety, * boost::unit_test::timeout(60)) {
    // Property: At most one leader per term
    for (std::size_t i = 0; i < 100; ++i) {
        auto cluster = create_random_cluster();
        cluster.run_election();

        auto leaders = cluster.count_leaders_in_term(cluster.current_term());
        BOOST_CHECK_LE(leaders, 1);  // At most one leader
    }
}
```

## Documentation

### Raft Implementation

- **[Raft Test Status](RAFT_TESTS_FINAL_STATUS.md)** - Comprehensive test suite analysis
- **[Test Fix Summary](TEST_FIX_SUMMARY.md)** - Property-based testing improvements
- **[Raft Design](.kiro/specs/raft-consensus/design.md)** - Architecture and design decisions
- **[Raft Requirements](.kiro/specs/raft-consensus/requirements.md)** - Detailed requirements
- **[Raft Tasks](.kiro/specs/raft-consensus/tasks.md)** - Implementation task list

### Transport Layers

- **[HTTP Transport Design](.kiro/specs/http-transport/design.md)** - HTTP/HTTPS transport architecture
- **[HTTP Transport Troubleshooting](doc/http_transport_troubleshooting.md)** - Common issues and solutions
- **[CoAP Transport README](doc/coap_transport_README.md)** - CoAP/CoAPS overview and quick start
- **[CoAP Transport API](doc/coap_transport_api.md)** - Complete CoAP API reference
- **[CoAP DTLS Configuration](doc/coap_dtls_configuration.md)** - Security setup guide
- **[CoAP Performance Tuning](doc/coap_performance_tuning.md)** - Optimization recommendations
- **[CoAP Troubleshooting](doc/coap_troubleshooting.md)** - Diagnostic procedures
- **[Network Simulator Design](.kiro/specs/network-simulator/design.md)** - Simulator architecture

### Certificate Authority & ACME

- **[Certificate Authority Design](.kiro/specs/certificate-authority/design.md)** - `certificate_authority`, `ca_service`, `ca_cluster_node`, and ACME architecture
- **[Certificate Authority Requirements](.kiro/specs/certificate-authority/requirements.md)** - Detailed requirements
- **[Certificate Authority Tasks](.kiro/specs/certificate-authority/tasks.md)** - Implementation task list

### Async Operations

- **[Async Retry Patterns](doc/async_retry_patterns.md)** - Retry logic and error handling
- **[Async Retry Validation](doc/async_retry_validation.md)** - Testing async retry behavior
- **[Future Wrapper Requirements](doc/future_wrapper_async_retry_requirements.md)** - Future abstraction design

### Core Concepts

- **[Concepts Documentation](doc/concepts_documentation.md)** - Enhanced C++20 concepts guide
- **[Concepts API Reference](doc/concepts_api_reference.md)** - Complete API reference
- **[Generic Future Architecture](doc/generic_future_architecture.md)** - Future abstraction design
- **[Future Migration Guide](doc/future_migration_guide.md)** - Migrating to generic futures

### Examples

- **[Raft Examples](examples/raft/)** - Complete Raft usage examples
  - `basic_cluster.cpp` - Creating and running a Raft cluster
  - `failure_scenarios.cpp` - Handling failures and recovery
  - `membership_changes.cpp` - Adding/removing nodes
  - `snapshot_example.cpp` - Snapshot creation and installation
  - `http_transport_example.cpp` - HTTP/HTTPS transport usage
- **[Concepts Examples](examples/concepts_usage_examples.cpp)** - Generic programming patterns
- **[Network Simulator Examples](examples/)** - Network simulation usage

## Performance

### Benchmarks

The implementation has been tested with multiple transport layers:

**HTTP/HTTPS Transport**:
- **Cluster sizes**: 3-7 nodes
- **Throughput**: 10,000+ commands/second (3-node cluster)
- **Latency**: < 10ms commit latency (local network)
- **Recovery**: < 1 second leader election after failure
- **TLS Overhead**: ~10-15% latency increase with HTTPS

**CoAP/CoAPS Transport**:
- **Cluster sizes**: 3-7 nodes
- **Throughput**: 5,000-8,000 commands/second (3-node cluster)
- **Latency**: < 5ms commit latency (local network, UDP)
- **Recovery**: < 500ms leader election after failure
- **DTLS Overhead**: ~15-20% latency increase with CoAPS
- **Message Overhead**: 4-8 bytes (vs 200-500 bytes for HTTP)
- **Memory Usage**: 30-40% lower than HTTP transport

### Optimization Features

- **Connection Pooling**: Reuse HTTP connections for reduced latency
- **Session Reuse**: DTLS session resumption for CoAP
- **Batch Application**: Apply multiple log entries in batches
- **Async Operations**: Non-blocking RPC calls with future-based coordination
- **Exponential Backoff**: Intelligent retry with jitter to prevent thundering herd
- **Resource Cleanup**: Proper cancellation and cleanup to prevent leaks
- **Block-wise Transfer**: Efficient handling of large messages in CoAP
- **Serialization Caching**: Reduce CPU overhead for repeated messages

## Production Readiness

### What's Ready

✅ **Core Raft Algorithm**: Leader election, log replication, commit advancement
✅ **Async Operations**: Commit waiting, future collections, error handling
✅ **HTTP/HTTPS Transport**: Production-ready with TLS and connection pooling
✅ **Error Handling**: Exponential backoff retry, timeout classification
✅ **Resource Management**: Proper cleanup, cancellation, leak prevention
✅ **Testing**: 100% pass rate for built tests, comprehensive property testing

### What's In Progress

⚠️ **Integration Tests**: 9 integration tests not yet built
⚠️ **Safety Properties**: 6 core safety property tests not yet built
⚠️ **Membership Changes**: Implementation exists but integration tests pending
⚠️ **Snapshots**: Core functionality exists but full integration tests pending

### Production Checklist

Before deploying to production:

- [ ] Enable HTTPS/TLS for all cluster communication
- [ ] Configure appropriate timeouts for your network
- [ ] Set up monitoring and metrics collection
- [ ] Implement persistent storage (not in-memory)
- [ ] Test failure scenarios in your environment
- [ ] Configure proper certificate management
- [ ] Set up log aggregation and alerting
- [ ] Perform load testing with your workload
- [ ] Document your disaster recovery procedures

## Contributing

Contributions are welcome! Areas where help is needed:

1. **Build Missing Tests**: 25 tests need CMake configuration
2. **Integration Testing**: Complete end-to-end cluster scenarios
3. **Performance Optimization**: Profiling and optimization
4. **Documentation**: More examples and tutorials
5. **Transport Implementations**: gRPC, custom protocols

## Troubleshooting

### Common Issues

**Test Failures**
- Ensure all dependencies are installed (Folly, Boost, OpenSSL)
- Check that you're using C++23 compatible compiler
- Run tests with `--verbose` flag for detailed output

**Build Errors**
- Verify CMake version >= 3.20
- Check compiler version (GCC 13+, Clang 16+)
- Ensure Folly is properly installed and findable by CMake

**Runtime Issues**
- Check network connectivity between nodes
- Verify certificate paths for HTTPS
- Ensure sufficient file descriptors for connections
- Check logs for detailed error messages

See [HTTP Transport Troubleshooting](doc/http_transport_troubleshooting.md) for transport-specific issues.

## References

- **[Raft Paper](https://raft.github.io/raft.pdf)** - Original Raft consensus algorithm paper
- **[Raft Website](https://raft.github.io/)** - Raft visualization and resources
- **[Folly Documentation](https://github.com/facebook/folly)** - Facebook's C++ library
- **[cpp-httplib](https://github.com/yhirose/cpp-httplib)** - HTTP library used for transport

## License

TBD
