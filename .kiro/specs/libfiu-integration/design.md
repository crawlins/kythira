# Design Document

## Overview

This document describes the design for integrating fault injection chaos
testing into Kythira using **libfiu** (Fault Injection Userspace). Fault
points are embedded directly in production source files using compile-time
macros that reduce to `((void)0)` in normal builds and expand to live libfiu
checks in chaos builds. This is the intended libfiu usage pattern: no
decorator wrapper types, no separate type bundles, no production overhead.

Chaos tests reuse the existing `test_raft_types` / simulator infrastructure
and are compiled with `-DFIU_ENABLE` and linked against libfiu. libfiu is an
**optional test-only** dependency: normal builds and non-chaos tests never
require it to be installed.

## Architecture

```
Normal build (FIU_ENABLE not defined)
──────────────────────────────────────
  fiu_do_on("raft/persistence/save_current_term", ...)
      └── expands to ((void)0)  — zero instructions emitted

Chaos build (FIU_ENABLE defined, libfiu linked)
──────────────────────────────────────
  Chaos test (Boost.Test)
      fiu_enable("raft/persistence/save_current_term", ...)
      │
      └── kythira::node<test_raft_types>
               ├── memory_persistence_engine::save_current_term()
               │        └── fiu_do_on() → runtime hash table lookup
               │        └── throws std::runtime_error if fault fires
               ├── simulator_network_client::send_append_entries()
               │        └── fiu_do_on() → throws network_exception if fires
               └── test_key_value_state_machine::apply()
                        └── fiu_do_on() → throws std::runtime_error if fires
      fiu_disable_all()
```

### What libfiu provides

`fiu_fail("name")` is a thread-safe function (libfiu ≥ 0.6) that returns
non-zero when a fault is registered for `"name"`, and 0 otherwise.

`fiu_do_on(name, action)` from `fiu-local.h` is a macro wrapping `fiu_fail`:
```c
#define fiu_do_on(name, action)  if (fiu_fail(name)) { action }
```
When `FIU_ENABLE` is not defined, the entire macro expands to `((void)0)`.

`fiu_enable("name", type, args, probability)` registers a fault:
- `type = 1` — fail always
- `type = 2` — fail randomly at `probability` in [0, 1]
- `type = 3` — fail once then disable itself

`fiu_disable("name")` and `fiu_disable_all()` clear registrations.

## Component Design

### 1. `include/raft/fault_injection.hpp`

A thin project-owned wrapper that makes `fiu_do_on` and `fiu_fail` available
everywhere without requiring libfiu to be installed for normal builds:

```cpp
#pragma once

#ifdef FIU_ENABLE
  #include <fiu-local.h>
#else
  // No-op stubs: compile to nothing when fault injection is disabled.
  // NOLINTNEXTLINE — these macros intentionally suppress their arguments
  #define fiu_do_on(name, action) ((void)0)
  #define fiu_fail(name)          (0)
#endif
```

This header has no link-time dependency on libfiu unless `FIU_ENABLE` is
defined. It is the only file in `include/` that references libfiu concepts.

### 2. Fault Point Instrumentation

Each fault point is a single `fiu_do_on()` call added to the relevant method.
The call precedes the actual operation so that, when the fault fires, the
underlying state is never modified (matching the atomicity expectation for
write failures).

#### `include/raft/persistence.hpp` — `memory_persistence_engine`

```cpp
#include "fault_injection.hpp"

auto save_current_term(TermId term) -> void {
    fiu_do_on("raft/persistence/save_current_term",
              throw std::runtime_error("chaos: save_current_term"););
    _current_term = term;
}

auto save_voted_for(NodeId node) -> void {
    fiu_do_on("raft/persistence/save_voted_for",
              throw std::runtime_error("chaos: save_voted_for"););
    _voted_for = node;
}

auto append_log_entry(const log_entry_t& entry) -> void {
    fiu_do_on("raft/persistence/append_log_entry",
              throw std::runtime_error("chaos: append_log_entry"););
    _log.push_back(entry);
}

auto truncate_log(LogIndex index) -> void {
    fiu_do_on("raft/persistence/truncate_log",
              throw std::runtime_error("chaos: truncate_log"););
    // ... existing truncation logic ...
}

auto save_snapshot(const snapshot_t& snap) -> void {
    fiu_do_on("raft/persistence/save_snapshot",
              throw std::runtime_error("chaos: save_snapshot"););
    _snapshot = snap;
}
```

Read operations (`load_current_term`, `load_voted_for`, `get_log_entry`, etc.)
are not instrumented: a read failure during crash recovery would prevent node
restart, which is a distinct failure mode better handled by dedicated
crash-recovery tests.

#### `include/raft/simulator_network.hpp` — `simulator_network_client`

```cpp
#include "fault_injection.hpp"

auto send_request_vote(NodeId target, const request_vote_request_type& req)
    -> future_type {
    fiu_do_on("raft/network/send_request_vote",
              throw network_exception("chaos: send_request_vote"););
    return _inner.send(target, req);
}

auto send_append_entries(NodeId target, const append_entries_request_type& req)
    -> future_type {
    fiu_do_on("raft/network/send_append_entries",
              throw network_exception("chaos: send_append_entries"););
    return _inner.send(target, req);
}

auto send_install_snapshot(NodeId target, const install_snapshot_request_type& req)
    -> future_type {
    fiu_do_on("raft/network/send_install_snapshot",
              throw network_exception("chaos: send_install_snapshot"););
    return _inner.send(target, req);
}
```

#### `include/raft/test_state_machine.hpp` — `test_key_value_state_machine`

```cpp
#include "fault_injection.hpp"

auto apply(LogIndex index, const std::vector<std::byte>& command) -> void {
    fiu_do_on("raft/state_machine/apply",
              throw std::runtime_error("chaos: state_machine/apply"););
    // ... existing apply logic ...
}
```

### 3. Fault Point Catalogue

All registered fault points in alphabetical order.

| Fault point name                         | File                         | What it simulates                       |
|------------------------------------------|------------------------------|-----------------------------------------|
| `raft/network/send_append_entries`       | `simulator_network.hpp`      | AppendEntries RPC lost or rejected      |
| `raft/network/send_install_snapshot`     | `simulator_network.hpp`      | InstallSnapshot RPC lost or rejected    |
| `raft/network/send_request_vote`         | `simulator_network.hpp`      | RequestVote RPC lost or rejected        |
| `raft/persistence/append_log_entry`      | `persistence.hpp`            | Disk write failure during log append    |
| `raft/persistence/save_current_term`     | `persistence.hpp`            | Disk write failure when persisting term |
| `raft/persistence/save_snapshot`         | `persistence.hpp`            | Disk write failure during snapshot      |
| `raft/persistence/save_voted_for`        | `persistence.hpp`            | Disk write failure for vote persistence |
| `raft/persistence/truncate_log`          | `persistence.hpp`            | Disk write failure during log truncation|
| `raft/state_machine/apply`              | `test_state_machine.hpp`     | Application-layer command rejection     |

### 4. Fault Profiles (`tests/chaos/fault_profiles.hpp`)

RAII handles that enable a set of fault points on construction and disable
them on destruction. The profile side requires `<fiu-control.h>` and lives
only in `tests/chaos/`, keeping the `fiu_enable`/`fiu_disable` API out of
production headers.

```cpp
#pragma once
#include <fiu-control.h>
#include <string>
#include <vector>

namespace kythira::chaos {

class fault_profile {
public:
    virtual ~fault_profile() { disable(); }
    fault_profile(const fault_profile&) = delete;
    fault_profile& operator=(const fault_profile&) = delete;

    void disable() {
        for (const auto& name : _active_points) {
            fiu_disable(name.c_str());
        }
        _active_points.clear();
    }

protected:
    fault_profile() = default;

    void enable_always(const char* name) {
        fiu_enable(name, 1, nullptr, 0);
        _active_points.emplace_back(name);
    }
    void enable_random(const char* name, double probability) {
        fiu_enable_random(name, 1, nullptr, probability);
        _active_points.emplace_back(name);
    }
    void enable_once(const char* name) {
        fiu_enable(name, 3, nullptr, 0);
        _active_points.emplace_back(name);
    }

private:
    std::vector<std::string> _active_points;
};

// All sends from an isolated node fail — models a hard network partition.
class network_partition_profile : public fault_profile {
public:
    network_partition_profile() {
        enable_always("raft/network/send_request_vote");
        enable_always("raft/network/send_append_entries");
    }
};

// Intermittent write errors — models disk degradation (default 10%).
class disk_degradation_profile : public fault_profile {
public:
    explicit disk_degradation_profile(double failure_probability = 0.10) {
        enable_random("raft/persistence/append_log_entry", failure_probability);
        enable_random("raft/persistence/save_current_term", failure_probability);
    }
};

// Single crash-on-persist event: next save_current_term fails, then disables.
class leader_crash_profile : public fault_profile {
public:
    leader_crash_profile() {
        enable_once("raft/persistence/save_current_term");
    }
};

// Unreliable state machine — models degraded application layer (default 5%).
class state_machine_fault_profile : public fault_profile {
public:
    explicit state_machine_fault_profile(double failure_probability = 0.05) {
        enable_random("raft/state_machine/apply", failure_probability);
    }
};

} // namespace kythira::chaos
```

### 5. CMake Integration

The production library targets are built without `-DFIU_ENABLE` and without
linking libfiu, so the macro stubs apply and there is zero runtime cost.
Chaos test executables are built with `-DFIU_ENABLE` and linked against libfiu.

```cmake
# ── Optional chaos testing (libfiu) ──────────────────────────────────────────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FIU QUIET fiu)
endif()

if(FIU_FOUND)
    message(STATUS "libfiu found — chaos test targets enabled")
    set(CHAOS_TESTS_ENABLED TRUE)
else()
    message(STATUS "libfiu not found — chaos test targets disabled")
    message(STATUS "  Install with: sudo apt install libfiu-dev")
    set(CHAOS_TESTS_ENABLED FALSE)
endif()
```

Per chaos test executable:

```cmake
add_executable(${name} ${src})
target_link_libraries(${name} PRIVATE kythira_test_common
                                       Boost::unit_test_framework
                                       ${FIU_LIBRARIES})
target_include_directories(${name} PRIVATE ${FIU_INCLUDE_DIRS}
                                            "${CMAKE_SOURCE_DIR}/tests/chaos")
target_compile_definitions(${name} PRIVATE FIU_ENABLE)
target_compile_options(${name} PRIVATE ${FIU_CFLAGS_OTHER})
set_tests_properties(${name} PROPERTIES LABELS "chaos")
```

### 6. Test File Structure

```
include/raft/
└── fault_injection.hpp          # FIU_ENABLE guard + fiu-local.h include

tests/chaos/
├── fault_profiles.hpp           # RAII fault profile handles (fiu-control.h)
├── safety_assertions.hpp        # Property verification helpers
├── chaos_smoke_test.cpp
├── chaos_profiles_test.cpp
├── chaos_election_safety_test.cpp
├── chaos_log_matching_test.cpp
├── chaos_leader_completeness_test.cpp
├── chaos_state_machine_safety_test.cpp
├── chaos_leader_election_recovery_test.cpp
├── chaos_commit_recovery_test.cpp
├── chaos_persistence_degradation_recovery_test.cpp
└── chaos_state_machine_recovery_test.cpp
```

### 7. Test Anatomy

```cpp
#define BOOST_TEST_MODULE ChaosElectionSafetyTest
#include <boost/test/unit_test.hpp>
#include <fiu.h>
#include "fault_profiles.hpp"
#include "safety_assertions.hpp"
#include <raft/raft.hpp>             // test_raft_types, standard node
#include <folly/init/Init.h>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv[] = {const_cast<char*>("chaos_test"), nullptr};
        _init = std::make_unique<folly::Init>(&argc, &argv[0]);
        fiu_init(0);
    }
    ~FollyInitFixture() { fiu_disable_all(); }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);
BOOST_AUTO_TEST_SUITE(chaos_election_safety)

BOOST_AUTO_TEST_CASE(partition_does_not_split_leadership,
                     *boost::unit_test::timeout(120))
{
    fiu_disable_all();   // belt-and-suspenders reset

    // 1. Build 3-node cluster with standard test_raft_types
    //    (compiled with FIU_ENABLE, so fiu_do_on() calls are live)
    // 2. Elect a leader
    // 3. { network_partition_profile profile; ... trigger elections ... }
    // 4. assert_election_safety(nodes)
}

BOOST_AUTO_TEST_SUITE_END()
```

### 8. Design Trade-offs

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| Fault injection site | Macros in production sources | Decorator wrapper types | No wrapper boilerplate; fault points auditable in-situ; same `test_raft_types` used everywhere |
| Normal-build overhead | Zero (macro expands to `((void)0)`) | Runtime flag check | Compiler eliminates dead branch; no measurable cost |
| Read path injection | None | Inject reads too | Read failure during recovery prevents restart — a distinct failure mode |
| `fiu-local.h` in production headers | Behind `fault_injection.hpp` guard | Direct include | Single chokepoint; works without libfiu installed |
| Fault profile / fiu_enable location | `tests/chaos/` only | Production headers | `fiu-control.h` (the enable/disable API) is test-only; only `fiu-local.h` (the check side) touches production |

---

## Components and Interfaces

### `include/raft/fault_injection.hpp`

**Role**: Conditional include gate. Owned by production code; has no link-time
dependency on libfiu unless `FIU_ENABLE` is defined at compile time.

**Public interface**:
- `fiu_do_on(name, action)` — execute `action` if the named fault is enabled;
  expands to `((void)0)` when `FIU_ENABLE` is not defined.
- `fiu_fail(name)` — return non-zero if the named fault is enabled; expands to
  `(0)` when `FIU_ENABLE` is not defined.

**Consumers**: `persistence.hpp`, `simulator_network.hpp`,
`test_state_machine.hpp` — each adds one `#include "fault_injection.hpp"` and
one `fiu_do_on()` call per write operation.

### `tests/chaos/fault_profiles.hpp`

**Role**: Test-side fault control. Provides RAII handles that call
`fiu_enable` / `fiu_disable` from `<fiu-control.h>`. Never included in
production headers.

**Public interface** (all types in `kythira::chaos`):

| Type | Constructor arguments | Fault points activated |
|---|---|---|
| `fault_profile` (base) | — | — (provides `disable()`) |
| `network_partition_profile` | none | `send_request_vote`, `send_append_entries` (always) |
| `disk_degradation_profile` | `double probability = 0.10` | `append_log_entry`, `save_current_term` (random) |
| `leader_crash_profile` | none | `save_current_term` (once) |
| `state_machine_fault_profile` | `double probability = 0.05` | `state_machine/apply` (random) |

All profiles disable their fault points on destruction.

### `tests/chaos/safety_assertions.hpp`

**Role**: Shared predicates for safety property verification across test files.

**Public interface** (all functions in `kythira::chaos`):

```cpp
// Fails the test if any two nodes claim leadership in the same term.
void assert_election_safety(
    const std::vector<kythira::node<test_raft_types>*>& nodes);

// Fails the test if any two nodes disagree on log content at any shared index
// up to min(last_applied) across the cluster.
void assert_log_matching(
    const std::vector<kythira::node<test_raft_types>*>& nodes);

// Fails the test if two nodes have applied different commands at the same index.
void assert_state_machine_safety(
    const std::vector<kythira::node<test_raft_types>*>& nodes,
    std::uint64_t up_to_index);
```

These helpers require a `debug_state()` accessor on `kythira::node` returning
a `debug_snapshot` struct with fields: `current_term`, `commit_index`,
`last_applied`, `log` (read-only span), `is_leader`. This accessor is added
to `raft.hpp` as part of the implementation (task 9 in `tasks.md`).

---

## Data Models

### `debug_snapshot` (added to `include/raft/raft.hpp`)

A read-only view of a node's internal state for assertion helpers. Returned by
`node::debug_state()`.

```cpp
struct debug_snapshot {
    term_id_type    current_term;
    log_index_type  commit_index;
    log_index_type  last_applied;
    bool            is_leader;
    // Read-only view of the persistent log.
    // Valid only while the node is running and not being mutated.
    std::span<const log_entry_type> log;
};
```

### `fault_profile::_active_points` (internal to `fault_profiles.hpp`)

A `std::vector<std::string>` holding the names of all fault points currently
enabled by a profile instance. Used by `disable()` to call `fiu_disable()` on
each point. The vector is cleared after disabling so that double-disable is
safe (a subsequent `disable()` call is a no-op).

### libfiu global fault table (owned by libfiu)

libfiu maintains a process-global hash table mapping fault point names to their
activation parameters (type, probability, call count). This table is shared
across all threads; libfiu ≥ 0.6 protects it with internal locks. Each chaos
test resets the table via `fiu_disable_all()` at the start of each test case
to prevent state leakage between cases that run in the same process.

---

## Correctness Properties

The chaos tests verify these four Raft safety invariants. These are properties
of the distributed system state, not of libfiu itself — libfiu is only the
mechanism by which faults are induced.

### Property 1: Election Safety

**Validates: Requirements 5.1**

At most one node may consider itself leader in any given term. Formally: for
any two nodes `a` and `b`, if `a.is_leader()` and `b.is_leader()`, then
`a.current_term() ≠ b.current_term()`.

### Property 2: Log Matching

**Validates: Requirements 5.2**

If two log entries in any two nodes' logs share the same (term, index) pair,
they contain identical command bytes, and all preceding entries are also
identical. This is a consequence of Raft's leader append-only and log matching
invariants.

### Property 3: Leader Completeness

**Validates: Requirements 5.3**

If a log entry is committed in term `t`, it must appear in the log of every
node that becomes leader in any term `t' > t`. The chaos tests verify this by
committing an entry, forcing a new election, and confirming the new leader's
log contains the committed entry.

### Property 4: State Machine Safety

**Validates: Requirements 5.4**

If two nodes have both applied entries through index `i`, they have applied the
same sequence of commands at every index `1..i`. This follows from Log Matching
plus the determinism of `test_key_value_state_machine`.

---

## Error Handling

### Fault fires during Raft operation

When `fiu_do_on()` throws inside a persistence or network operation, the
exception propagates up through `kythira::node`'s internal call stack. The
node catches component-level exceptions in the same places it already handles
network timeouts and persistence errors. No special handling is needed in the
chaos test harness: the node's existing error recovery paths (retry, stepdown,
etc.) are exactly what the tests exercise.

### libfiu not initialised

Calling `fiu_fail()` before `fiu_init(0)` is undefined in libfiu. The
`FollyInitFixture` global fixture calls `fiu_init(0)` once per test process,
before any test case body runs, ensuring it is always initialised.

### Fault state leaking across test cases

`fiu_disable_all()` is called at the top of every test case body (in addition
to the fixture destructor) as a belt-and-suspenders reset. This guards against
a test that exits early via an exception before its profile's destructor runs.

### libfiu absent at configure time

When `pkg_check_modules(FIU ...)` finds nothing, no chaos test target is
defined. The `chaos-tests` stub target prints an actionable install message and
exits non-zero. Normal build targets (`all`, `tests`) are unaffected.

---

## Testing Strategy

The chaos test suite is self-validating at two levels:

### Level 1 — Infrastructure validation (`chaos_smoke_test`, `chaos_profiles_test`)

These tests verify that the fault injection machinery itself works before
relying on it to test Raft:
- `chaos_smoke_test`: calls `fiu_init`, enables a synthetic point, asserts
  `fiu_fail()` returns non-zero, disables, asserts returns zero.
- `chaos_profiles_test`: for each `fault_profile` subclass, constructs the
  profile, triggers the instrumented operation on a single-node cluster,
  confirms it throws, destroys the profile, confirms the operation succeeds.

### Level 2 — Raft property tests

Eight property tests, each focused on one safety or liveness invariant
(see `tasks.md` tasks 11–18). Each test:
1. Calls `fiu_disable_all()` at entry to start from a clean fault state.
2. Builds a small cluster (3–5 nodes) using `test_raft_types` compiled with
   `FIU_ENABLE`.
3. Applies one or more `fault_profile` instances for a bounded period.
4. Removes faults and waits for the cluster to stabilise.
5. Calls one or more assertion helpers to verify the relevant invariant.
6. Has a `*boost::unit_test::timeout(120)` decorator to prevent indefinite
   hangs if liveness is not restored.

Tests run via `ctest --label-regex chaos` and are excluded from the default
`ctest` run (which would fail on machines without libfiu).

### Interaction with the existing property-based test suite

The chaos tests reuse the same `NetworkSimulator`, `memory_persistence_engine`,
and `test_key_value_state_machine` infrastructure as the existing property
tests. The only addition is `FIU_ENABLE` at compile time, which activates the
`fiu_do_on()` calls. The existing property tests are not recompiled with
`FIU_ENABLE` and are unaffected.
