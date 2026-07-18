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
- **Peer-to-Peer Log Replication** (opt-in) — lagging followers can catch up
  from any peer instead of only the leader, via a gossip-based transport; see
  [Peer-to-Peer Log Replication & Gossip Transport](#peer-to-peer-log-replication--gossip-transport)

### Advanced Features
- **Async Operations** using generic future concepts (Folly, std::future, custom)
- **Commit Waiting** with timeout and cancellation support
- **Exponential Backoff Retry** with jitter for network operations
- **Timeout Classification** for intelligent error handling
- **Resource Management** with proper cleanup and leak prevention
- **Comprehensive Logging** for debugging and observability
- **stdexec-Backed Future Implementation** (optional, opt-in) — a second,
  sender/receiver-based `Future`/`Promise`/`Executor` family alongside the
  default Folly one, for new code that wants direct access to `stdexec`
  schedulers/algorithms; see
  [stdexec Future Backend](#stdexec-future-backend-optional), including a
  [Folly-vs-stdexec performance comparison](#comparing-folly-vs-stdexec-performance)

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
- **385 Tests, 100% Pass Rate** — 0 failing, 0 disabled
- **88.6%+ Line Coverage**, enforced by a non-decreasing ratchet (see [Code Coverage](#code-coverage))
- **Property-Based Testing** using Boost.Test
- **Integration, Chaos, and Docker-Chaos Tests** for end-to-end and fault-injected validation
- **Zero Test Failures** across the full test suite

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

## ARM (arm64) Support

Kythira is built and tested natively on 64-bit ARM (`aarch64`, vcpkg triplet
`arm64-linux`) Linux, alongside the existing x86_64 matrix. CI runs both
architectures on GitHub-hosted runners — `ubuntu-24.04` (x86_64) and
`ubuntu-24.04-arm` (arm64) — compiling natively on each, not cross-compiling
or emulating under QEMU. See
[`.kiro/specs/arm64-ci-verification/`](.kiro/specs/arm64-ci-verification/)
for the full design.

**Known limitations:**

- **PocoDNSSD is x86_64-only in this repository.** The `poco_peer_discovery`
  DNSSD backend depends on manually-built `libPocoDNSSD.a`/
  `libPocoDNSSDAvahi.a` static archives (Poco's DNSSD component isn't a
  standard vcpkg feature) that are only provided for the `x64-linux` triplet.
  On `arm64-linux`, CMake configuration falls through to
  `POCO_DNSSD_FOUND=FALSE` and `poco_peer_discovery` builds with that backend
  disabled — the same graceful degradation an x86_64 host without the
  prebuilt archives already sees today. rfc1035/rfc2136-based discovery
  (`libldns`) is unaffected on either architecture.
- **32-bit ARM (`armv7`/`armhf`) and non-Linux ARM (macOS, Windows) are out
  of scope.** This project targets server-class 64-bit ARM Linux (e.g. AWS
  Graviton), matching the architectures already selected by
  `tests/aws_quorum_manager_real_ec2_test.cpp`'s EC2 provisioning logic.

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

`kythira::node<Types>` is parameterized by a single `Types` bundle (a
`raft_types`-satisfying struct), not a list of positional template
arguments — this lets you swap any component (transport, persistence,
state machine, ...) independently. For simulator-based development and
testing, define your bundle in terms of `kythira::simulator_network_client`/
`simulator_network_server`; see [`examples/raft/basic_cluster.cpp`](examples/raft/basic_cluster.cpp)
for a complete, runnable version of this example, and
[HTTP Transport for Production](#http-transport-for-production) below for a
transport suited to a real deployment.

```cpp
#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>       // or your own KeyValueStore from above
#include <network_simulator/network_simulator.hpp>

// A Types bundle satisfies the raft_types concept by providing every
// component's concrete type as a nested alias.
struct my_raft_types {
    using future_type = kythira::Future<std::vector<std::byte>>;
    using promise_type = kythira::Promise<std::vector<std::byte>>;
    using try_type = kythira::Try<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using raft_network_types = kythira::raft_simulator_network_types<std::string>;
    using network_client_type =
        kythira::simulator_network_client<raft_network_types, serializer_type, serialized_data_type>;
    using network_server_type =
        kythira::simulator_network_server<raft_network_types, serializer_type, serialized_data_type>;

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = KeyValueStore;  // your state machine from above

    using configuration_type = kythira::raft_configuration;

    // node<Types> also needs these compound aliases (not checked by the
    // raft_types concept itself, but required for node<Types> to instantiate).
    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;
    using request_vote_request_type =
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type>;
    using install_snapshot_response_type = kythira::install_snapshot_response<term_id_type>;
};

using raft_node = kythira::node<my_raft_types>;

// Wire up a 3-node network (the simulator needs an explicit edge between
// every pair of nodes that should be able to reach each other).
network_simulator::NetworkSimulator<my_raft_types::raft_network_types> sim;
sim.start();
auto net1 = sim.create_node("1");
auto net2 = sim.create_node("2");
auto net3 = sim.create_node("3");
for (const auto& from : {"1", "2", "3"}) {
    for (const auto& to : {"1", "2", "3"}) {
        if (from != std::string_view{to}) sim.add_edge(from, to, {});
    }
}

// Configure timing (the private-member-plus-accessor fields shown here
// follow raft_configuration's naming; see include/raft/types.hpp).
kythira::raft_configuration config;
config._election_timeout_min = std::chrono::milliseconds{150};
config._election_timeout_max = std::chrono::milliseconds{300};
config._heartbeat_interval = std::chrono::milliseconds{50};

// Preferred construction: node_config<Types> with designated initializers —
// every component is supplied explicitly, nothing is implicit or positional.
auto make_node = [&](std::uint64_t id, auto net) {
    return raft_node{kythira::node_config<my_raft_types>{
        .node_id = id,
        .network_client = {net, my_raft_types::serializer_type{}},
        .network_server = {net, my_raft_types::serializer_type{}},
        .persistence = {},
        .logger = kythira::console_logger{kythira::log_level::info},
        .metrics = {},
        .membership = {},
        .config = config,
    }};
};
auto node1 = make_node(1, net1);
auto node2 = make_node(2, net2);
auto node3 = make_node(3, net3);

node1.set_cluster_configuration({1, 2, 3});
node2.set_cluster_configuration({1, 2, 3});
node3.set_cluster_configuration({1, 2, 3});

node1.start();
node2.start();
node3.start();

// Wait for an election, then find the leader (any node — a non-leader
// redirects/rejects a submit_command call rather than forwarding it).
std::this_thread::sleep_for(config.election_timeout_max() + std::chrono::milliseconds{50});
auto& leader = node1.is_leader() ? node1 : (node2.is_leader() ? node2 : node3);

// Submit a command on the leader.
std::vector<std::byte> command = serialize_command("SET", "key", "value");
auto future = leader.submit_command(command, std::chrono::seconds{5});
auto result = std::move(future).get();  // Waits for commit and application
```

### HTTP Transport for Production

```cpp
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

// http_transport_types<Serializer, Metrics, Executor> is a ready-made Types
// bundle satisfying the transport_types concept — swap in your own metrics
// implementation (anything satisfying kythira::metrics<T>) in place of
// kythira::noop_metrics below.
using production_transport = kythira::http_transport_types<
    kythira::json_rpc_serializer<std::vector<std::byte>>,
    kythira::noop_metrics,
    folly::CPUThreadPoolExecutor>;

// Server with HTTPS
kythira::cpp_httplib_server_config server_config;
server_config.enable_ssl = true;
server_config.ssl_cert_path = "/etc/raft/server.crt";
server_config.ssl_key_path = "/etc/raft/server.key";
server_config.max_concurrent_connections = 100;

kythira::cpp_httplib_server<production_transport> server(
    "0.0.0.0", 8443, server_config, kythira::noop_metrics{}
);

// Client with HTTPS
std::unordered_map<std::uint64_t, std::string> cluster_nodes;
cluster_nodes[1] = "https://node1.example.com:8443";
cluster_nodes[2] = "https://node2.example.com:8443";
cluster_nodes[3] = "https://node3.example.com:8443";

kythira::cpp_httplib_client_config client_config;
client_config.enable_ssl_verification = true;
client_config.ca_cert_path = "/etc/raft/ca.crt";
client_config.connection_pool_size = 10;

kythira::cpp_httplib_client<production_transport> client(
    std::move(cluster_nodes), client_config, kythira::noop_metrics{}
);
```

### CoAP Transport for IoT and Constrained Networks

```cpp
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>

// coap_transport_types<Serializer, Metrics, Executor> is CoAP's equivalent
// of http_transport_types above.
using iot_transport = kythira::coap_transport_types<
    kythira::json_rpc_serializer<std::vector<std::byte>>,
    kythira::noop_metrics,
    folly::Executor>;

// Configure CoAP endpoints
std::unordered_map<std::uint64_t, std::string> coap_endpoints = {
    {1, "coaps://node1.iot.local:5684"},  // CoAPS (DTLS encrypted)
    {2, "coaps://node2.iot.local:5684"},
    {3, "coaps://node3.iot.local:5684"}
};

// Client configuration with DTLS
kythira::coap_client_config coap_config;
coap_config.enable_dtls = true;
coap_config.cert_file = "/etc/raft/coap-cert.pem";
coap_config.key_file = "/etc/raft/coap-key.pem";
coap_config.ca_file = "/etc/raft/coap-ca.pem";
coap_config.ack_timeout = std::chrono::milliseconds{2000};
coap_config.max_retransmit = 4;
coap_config.enable_block_transfer = true;  // For large messages

kythira::coap_client<iot_transport> client(
    coap_endpoints, coap_config, kythira::noop_metrics{}
);

// Server configuration
kythira::coap_server_config server_config;
server_config.enable_dtls = true;
server_config.cert_file = "/etc/raft/coap-cert.pem";
server_config.key_file = "/etc/raft/coap-key.pem";
server_config.ca_file = "/etc/raft/coap-ca.pem";
server_config.max_concurrent_sessions = 200;

kythira::coap_server<iot_transport> server(
    "0.0.0.0", 5684, server_config, kythira::noop_metrics{}
);

// Register handlers and start
server.register_request_vote_handler(vote_handler);
server.start();
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

See [doc/RAFT_TESTS_FINAL_STATUS.md](doc/RAFT_TESTS_FINAL_STATUS.md) for comprehensive test analysis, [doc/TODO.md](doc/TODO.md) for full task-by-task project status, and [doc/CHANGELOG.md](doc/CHANGELOG.md) for a dated history of what changed and why.

**Summary**:
- **Total Tests**: 378 (registered in CTest)
- **Passing**: 378 (100%)
- **Failing**: 0 (0%)
- **Line Coverage**: 88.6%+ (non-decreasing ratchet, see [Code Coverage](#code-coverage))

### Test Categories

- **Property-Based Tests**: safety and liveness properties validated across randomized inputs, including dedicated determinism tests for state machines
- **Integration Tests**: end-to-end cluster behavior, including multi-process Raft/CA clusters with leader failover
- **Unit Tests**: component-level testing across transports, persistence, peer discovery, and peer-to-peer replication
- **Chaos & Docker-Chaos Tests**: libfiu fault injection and real multi-node OS-level fault scenarios (see [Chaos Testing](#chaos-testing) and [Docker Chaos Testing](#docker-chaos-testing))

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
  Packaged for a 3-AZ AWS deployment (`docker/ca_cluster_node/`). Its
  Raft-internal RPC channel supports optional mutual TLS
  (`--rpc-tls-cert`/`--rpc-tls-key`, `include/raft/tls_tcp_rpc.hpp`),
  bootstrapped by a static operator-provisioned credential and
  automatically cut over to the cluster's own CA root once it exists — see
  `docker/ca_cluster_node/README.md`'s "Securing the Raft-internal RPC
  channel" section.
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

## Peer-to-Peer Log Replication & Gossip Transport

By default, log replication in `node<Types>` is a strict star topology: only
the leader can supply missing entries to a follower that has fallen behind.
That's fine in steady state, but it makes the leader's own CPU/bandwidth the
bottleneck for how fast a cluster converges when many members fall behind at
once — a rolling restart, a healed network partition, or several nodes
joining in a burst. This is opt-in and off by default; the leader remains the
sole commit authority in all cases. See
[`.kiro/specs/peer2peer-log-replication/`](.kiro/specs/peer2peer-log-replication/)
and [`.kiro/specs/peer2peer-gossip-transport/`](.kiro/specs/peer2peer-gossip-transport/)
for the full design and requirements.

### Components

- **`peer2peer_replicator`** (`include/raft/peer2peer_replication.hpp`) — an
  opt-in concept, shaped like `peer_discovery`. `no_op_peer2peer_replicator`
  is the default `peer2peer_replicator_type` and guarantees zero behavioral
  change for any `Types` bundle that doesn't opt in;
  `static_peer2peer_replicator` is an in-memory reference/test
  implementation.
- **`fetch_log_entries_request`/`response`** (`include/raft/types.hpp`) — the
  RPC pair a peer-to-peer fetch uses, with optional
  `network_client_with_log_fetch`/`network_server_with_log_fetch` extension
  concepts (`network.hpp`), wired into `json_serializer.hpp` and
  `simulator_network.hpp` the same way the existing `ClusterJoin`/
  `ClusterLeave` optional extensions are.
- **`tcp_gossip_peer2peer_replicator`** (`include/raft/tcp_gossip_transport.hpp`)
  — a real anti-entropy gossip implementation: a self-contained TCP listener
  plus a background thread running periodic randomized push-pull digest
  exchange (Cassandra/Dynamo-style — not SWIM, since Raft's own election
  timeouts already cover liveness detection), entirely independent of
  whatever `network_client_type`/`network_server_type` the node uses for
  Raft RPCs — a gossip-layer bug or overload can never touch consensus
  traffic.

A peer-to-peer fetch reuses the exact same
`append_entries_with_consistency_check()` conflict/truncation guarantees as
leader-driven replication, so a bad or stale source peer can only cause
wasted local work, never a committed entry being lost or altered. A
replicator's peer set tracks the replicated log's own cluster membership
(`node<Types>::cluster_members()`, kept current at every `_configuration`
mutation site, including joint-consensus transitions) automatically — it is
never separately configured; only node-ID-to-address resolution
(`tcp_gossip_config::address_book`) remains static, since addresses aren't
log data.

### Enabling gossip catch-up

```cpp
#include <raft/tcp_gossip_transport.hpp>

kythira::tcp_gossip_config<node_id_type, std::string> gossip_config;
gossip_config.listen_port = 9500;
gossip_config.address_book = {{2, "10.0.0.2:9500"}, {3, "10.0.0.3:9500"}};
gossip_config.fanout = 3;
gossip_config.gossip_round_interval = std::chrono::milliseconds{500};

// Add `peer2peer_replicator_type = kythira::tcp_gossip_peer2peer_replicator<
//     node_id_type, std::string, log_index_type>;` to your Types bundle, then:
auto node_cfg = kythira::node_config<my_raft_types>{
    // ... every other field as in the Basic Raft Cluster example above ...
    .peer2peer_replicator = my_raft_types::peer2peer_replicator_type{gossip_config},
};
```

Leave `peer2peer_replicator_type` as the default `no_op_peer2peer_replicator`
to preserve today's leader-only replication behavior exactly.

---

## stdexec Future Backend (Optional)

A second, [`stdexec`](https://github.com/NVIDIA/stdexec) (P2300
sender/receiver) backed implementation of the `Future`/`Promise`/`Try`/
`Executor` family, alongside the default Folly one. See
[`.kiro/specs/stdexec-future-backend/`](.kiro/specs/stdexec-future-backend/)
for the full design.

**Scope** — see
[`include/raft/future_stdexec_README.md`](include/raft/future_stdexec_README.md)
for the complete picture, but in short: no existing production call site is
converted by this feature, Folly is not removed or made optional-only, and
GPU execution (`nvexec`) is out of scope. This is a second, independent
implementation for new code that specifically wants direct access to
`stdexec` schedulers/algorithms — not a replacement or a migration path.

### Components

- **`kythira::stdexec_backend::{Try, SemiPromise, Promise, Future}`**
  (`include/raft/future_stdexec.hpp`) — satisfy the same backend-neutral
  concepts (`include/concepts/future.hpp`) as the Folly backend, in their
  own namespace so the two are never silently interchangeable. `Future<T>`
  supports the full continuation/transformation/scheduling surface:
  `thenValue`/`thenTry`/`thenError`, `ensure`, `via(scheduler_handle)`,
  `delay`/`within`.
- **`scheduler_handle`** — a small type erasure over any concrete `stdexec`
  scheduler, letting `via()` accept `exec::single_thread_context`,
  `exec::timed_thread_context`, or any other scheduler type uniformly.
- **`FutureFactory`/`FutureCollector`** — `makeFuture`/`makeReadyFuture`/
  `makeExceptionalFuture` and `collectAll`/`collectAny`/
  `collectAnyWithoutException`/`collectN`, matching the Folly backend's
  semantics exactly (verified by
  [`tests/stdexec_concept_wrappers_interoperability_property_test.cpp`](tests/stdexec_concept_wrappers_interoperability_property_test.cpp),
  which runs equivalent operations through both backends and compares
  results).
- **`scheduler_executor_shim`** — a compatibility shim satisfying the
  (Folly-shaped) `executor`/`keep_alive` concepts by wrapping
  `stdexec::sync_wait(schedule(scheduler) | then(func))` inside `.add()`;
  documented overhead (blocks the calling thread of `.add()`) — new code
  should use `via(scheduler)` directly instead.

### Enabling the stdexec backend

Requires the optional `stdexec` vcpkg dependency (present by default in
this project's `vcpkg.json`, gated at the CMake level):

```cpp
#include <raft/future_stdexec.hpp>
#include <exec/single_thread_context.hpp>

exec::single_thread_context ctx;
kythira::stdexec_backend::scheduler_handle handle(ctx.get_scheduler());

auto future = kythira::stdexec_backend::FutureFactory::makeFuture(42)
                  .via(&handle)
                  .thenValue([](int x) { return x + 1; });
int result = std::move(future).get();  // 43
```

For non-templated call sites that just want "the project's chosen
backend" without spelling out which, `kythira::future_default<T>`
(`include/raft/future_default.hpp`) resolves to either backend based on
the `KYTHIRA_DEFAULT_FUTURE_BACKEND` CMake option (`folly`, the default,
or `stdexec`):

```bash
cmake -S . -B build -DKYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec \
      -DCMAKE_PREFIX_PATH="$(pwd)/vcpkg_installed/x64-linux"
```

Templated core code that is already generic over a future type never
references `future_default` and is completely unaffected by this option
either way.

### Running the stdexec backend test suite

```bash
ctest --test-dir build -L stdexec
```

Covers concept compliance (`Try`/`Promise`/`SemiPromise`/`Future`/
`Executor`), the hand-rolled `single_shot_channel` primitive's exactly-once
completion and broken-promise semantics, factory/continuation/collector
operation fidelity, cross-backend interoperability, and backend
non-interference (compile-time checks that the two backends' types cannot
be silently mixed). See
[`examples/stdexec-backend/migration_guide_example.cpp`](examples/stdexec-backend/migration_guide_example.cpp)
for a runnable side-by-side comparison of both backends.

> **Note**: a GCC 13 miscompilation of `exec::any_sender`'s
> small-buffer-optimized move constructor at `-O2`/`-O3` was found and
> fixed during implementation (`-fno-strict-aliasing` for GCC builds in
> `CMakeLists.txt`) — see
> [`spike-notes.md`](.kiro/specs/stdexec-future-backend/spike-notes.md)'s
> "Phase 3 findings" for the full diagnosis if you hit unexplained
> heap corruption while working in this area on GCC.

### Comparing Folly vs. stdexec performance

A dedicated benchmark suite compares the two backends across a fixed
catalog of scenarios (creation/resolution, promise fulfillment, `thenValue`
chains, `thenError`, `via(scheduler)`, `collectAll`/`collectAny`,
`delay`/`within` overhead) without changing either implementation or the
default backend:

```bash
# Fast, CI-registered sanity-floor checks (per-backend only, no
# cross-backend assertions)
ctest --test-dir build -L future-backend

# Full, report-quality comparison — prints a side-by-side table and writes
# timestamped CSV/JSON to test_results/
./build/examples/future_backend_benchmark_report
```

See [`doc/future_backend_performance_comparison.md`](doc/future_backend_performance_comparison.md)
for the full scenario catalog, methodology, known structural asymmetries,
and reference numbers.

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

- **[Raft Test Status](doc/RAFT_TESTS_FINAL_STATUS.md)** - Comprehensive test suite analysis
- **[Raft Implementation Status](doc/RAFT_IMPLEMENTATION_STATUS.md)** - Per-component implementation status
- **[Performance Validation](doc/PERFORMANCE_VALIDATION.md)** - Benchmark methodology and results
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
✅ **CoAP/CoAPS Transport**: DTLS, block-wise transfer, EDHOC/OSCORE, ACE-OAuth
✅ **Error Handling**: Exponential backoff retry, timeout classification
✅ **Resource Management**: Proper cleanup, cancellation, leak prevention
✅ **Cluster Membership Changes**: Joint consensus (Raft §6) add/remove server,
  with property tests and node-recovery-on-restart coverage
✅ **Snapshots**: Log compaction and restart recovery, covered by dedicated
  integration and property tests
✅ **Certificate Authority**: In-process CA, `ca_service`/`ca_cluster_node`,
  ACME (RFC 8555/8738), fingerprint-pinned bootstrap
✅ **`ca_cluster_node` RPC mTLS**: mutual TLS on the Raft-internal RPC
  channel between `ca_cluster_node` peers, bootstrapped by a static
  operator-provisioned credential and automatically cut over to the
  cluster's own CA root once it exists — no operator action beyond
  initial provisioning
✅ **Peer-to-Peer Log Replication**: opt-in gossip-based catch-up
  (`tcp_gossip_peer2peer_replicator`) so lagging followers can pull missing
  entries from any peer, not just the leader; leader remains sole commit
  authority; off by default
✅ **stdexec Future Backend**: optional, opt-in second `Future`/`Promise`/
  `Executor` implementation for new `stdexec`-specific code; Folly stays
  the default and is unaffected either way
✅ **Testing**: 100% pass rate (385 tests), comprehensive property/integration/chaos testing

See [`doc/TODO.md`](doc/TODO.md) for the full task-by-task status, or
[`doc/CHANGELOG.md`](doc/CHANGELOG.md) for a dated history of what changed and why.

### What's In Progress

⚠️ **Additional cloud providers**: Azure, GCP, OCI, and Alibaba Cloud quorum
  managers / certificate providers — AWS is implemented today
⚠️ **Alternative HTTP transports**: Boost.Beast and Proxygen as optional
  alternatives to the current httplib-based transport

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

Contributions are welcome! See [`doc/TODO.md`](doc/TODO.md) for the full
outstanding-work list. Areas where help is needed:

1. **Additional cloud providers**: Azure, GCP, OCI, and Alibaba Cloud quorum
   managers / certificate providers
2. **Alternative HTTP transports**: Boost.Beast and Proxygen as optional
   alternatives to the current httplib-based transport
3. **Performance Optimization**: Profiling and memory usage optimization
4. **Documentation**: More examples and tutorials

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
