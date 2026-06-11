# Requirements Document

## Introduction

This document specifies the requirements for integrating chaos testing into
the Kythira build and test infrastructure using **libfiu** (Fault Injection
Userspace). The goal is to verify that Raft's safety and liveness properties
hold under realistic fault conditions — network packet loss, intermittent
persistence failures, and state machine errors — without modifying production
code paths.

Chaos testing complements the existing property-based test suite: where those
tests verify protocol correctness under ideal conditions, chaos tests verify
**resilience** under adversarial conditions. The integration is deliberately
test-only: libfiu is a test-time dependency only, fault injection wrappers live
in `tests/chaos/`, and the production `kythira::node` template requires no
changes.

## Glossary

- **Fault_Point**: A named string registered with libfiu (e.g.,
  `"raft/network/send"`) at which a controlled failure can be injected.
- **Fault_Profile**: A named collection of Fault_Points and their injection
  parameters (probability, type, sequential vs. random) that models a specific
  failure scenario (e.g., network partition, disk degradation).
- **Chaos_Wrapper**: A C++ class template that decorates a kythira component
  (network client, persistence engine, or state machine) and intercepts
  operations to check libfiu Fault_Points before delegating to the underlying
  implementation.
- **Chaos_Node**: A `kythira::node` instantiated with Chaos_Wrapper types
  instead of real or simulator types, used exclusively in chaos tests.
- **Safety_Property**: A predicate over the distributed system state that must
  hold in all reachable states, regardless of faults. Violating a
  Safety_Property indicates a correctness bug.
- **Liveness_Property**: A predicate that must eventually become true once
  faults stop (or sufficiently subside). Liveness failures may indicate
  progress bugs or inadequate retry/recovery logic.
- **FIU_CHAOS_TESTS**: The CMake build option that enables libfiu discovery and
  compiles the chaos test targets. Defaults to `OFF` when libfiu is absent.

## Requirements

### Requirement 1: libfiu Library Integration

**User Story:** As a developer, I want libfiu to be detected at configure time
and the chaos test targets to be compiled only when the library is available,
so that the build does not break on machines where libfiu is not installed.

#### Acceptance Criteria

1. WHEN CMake configures the project AND `pkg-config` finds `fiu` (i.e.,
   `libfiu-dev` is installed) THEN the chaos test targets SHALL be defined and
   built.
2. WHEN `fiu` is not found via `pkg-config` THEN CMake SHALL emit a
   `message(STATUS ...)` explaining that chaos tests are disabled and how to
   install libfiu (`sudo apt install libfiu-dev`). No warning or error SHALL be
   emitted, so the build remains clean on machines without libfiu.
3. WHEN chaos tests are disabled THEN `cmake --build build --target chaos-tests`
   SHALL print an actionable message (`chaos tests require libfiu-dev`) and exit
   non-zero rather than silently succeeding.
4. WHEN chaos tests are enabled THEN the libfiu include path and link flags
   SHALL be applied only to chaos test targets, not to production targets or
   other tests.
5. WHEN `DEPENDENCIES.md` is updated THEN it SHALL document libfiu as a
   **test-only optional** dependency with installation instructions.

### Requirement 2: Chaos Wrapper Infrastructure

**User Story:** As a test author, I want decorator types that wrap kythira's
pluggable components and inject faults via libfiu, so I can write chaos tests
without modifying production code.

#### Acceptance Criteria

1. WHEN a `chaos_network_client<T>` wraps a network client `T` THEN every call
   to `send_request_vote`, `send_append_entries`, and `send_install_snapshot`
   SHALL check the corresponding Fault_Point before delegating to `T`. If the
   Fault_Point fires, the wrapper SHALL throw the network exception type
   appropriate for the transport (or return a failed future) instead of
   forwarding.
2. WHEN a `chaos_persistence_engine<T>` wraps a persistence engine `T` THEN
   calls to `save_current_term`, `save_voted_for`, `append_log_entry`,
   `truncate_log`, and `save_snapshot` SHALL each check a distinct Fault_Point.
   If the Fault_Point fires, the wrapper SHALL throw `std::runtime_error` before
   the write is forwarded, leaving the underlying state unchanged.
3. WHEN a `chaos_state_machine<T>` wraps a state machine `T` THEN each call to
   `apply` SHALL check `"raft/state_machine/apply"`. If the Fault_Point fires,
   the wrapper SHALL throw `std::runtime_error` before forwarding, so the
   underlying machine's state is not modified.
4. WHEN the underlying component method succeeds THEN the wrapper SHALL return
   the same value as the underlying call, with no observable difference.
5. WHEN no libfiu Fault_Point is enabled for a wrapper THEN every wrapped call
   SHALL behave identically to calling the underlying component directly
   (zero-overhead pass-through).
6. WHEN all three wrapper types are assembled into a `kythira::node` THEN the
   resulting `Chaos_Node` SHALL satisfy the same `raft_types` concept as any
   other node instantiation.

### Requirement 3: Fault Point Naming Convention

**User Story:** As a test author, I want a consistent and documented
Fault_Point naming scheme so that fault enablement calls in tests are
predictable and searchable.

#### Acceptance Criteria

1. WHEN Fault_Points are named THEN they SHALL follow the pattern
   `"raft/<layer>/<operation>"`, where `<layer>` is one of `network`,
   `persistence`, or `state_machine`, and `<operation>` is the method name.
2. WHEN a network client wrapper defines Fault_Points THEN the following names
   SHALL be used:
   - `"raft/network/send_request_vote"`
   - `"raft/network/send_append_entries"`
   - `"raft/network/send_install_snapshot"`
3. WHEN a persistence engine wrapper defines Fault_Points THEN the following
   names SHALL be used:
   - `"raft/persistence/save_current_term"`
   - `"raft/persistence/save_voted_for"`
   - `"raft/persistence/append_log_entry"`
   - `"raft/persistence/truncate_log"`
   - `"raft/persistence/save_snapshot"`
4. WHEN a state machine wrapper defines Fault_Points THEN the following name
   SHALL be used:
   - `"raft/state_machine/apply"`
5. WHEN a new Fault_Point is added THEN it SHALL be registered in the
   `Fault_Point Catalogue` section of `design.md`.

### Requirement 4: Fault Profiles

**User Story:** As a test author, I want pre-defined Fault_Profiles that model
realistic failure scenarios, so I can construct repeatable chaos scenarios
without manually configuring individual Fault_Points.

#### Acceptance Criteria

1. WHEN a `network_partition_profile` is applied THEN it SHALL enable
   `"raft/network/send_append_entries"` and
   `"raft/network/send_request_vote"` on a specified minority subset of nodes
   with probability 1.0 (all sends fail), simulating a hard network partition.
2. WHEN a `disk_degradation_profile` is applied THEN it SHALL enable
   `"raft/persistence/append_log_entry"` and
   `"raft/persistence/save_current_term"` with a configurable failure
   probability (default 10%), simulating intermittent write errors.
3. WHEN a `leader_crash_profile` is applied THEN it SHALL enable
   `"raft/persistence/save_current_term"` on the current leader with
   probability 1.0 on the next call, then disable itself, simulating a single
   crash-on-persist event.
4. WHEN a `state_machine_fault_profile` is applied THEN it SHALL enable
   `"raft/state_machine/apply"` on a specified node with a configurable failure
   probability, simulating a degraded state machine.
5. WHEN any profile's `disable()` method is called THEN all Fault_Points
   registered by that profile SHALL be disabled, and subsequent operations on
   the affected nodes SHALL proceed without fault injection.
6. WHEN a Fault_Profile is applied THEN it SHALL use RAII to guarantee that
   `disable()` is called on destruction, preventing fault state from leaking
   across test cases.

### Requirement 5: Raft Safety Property Tests

**User Story:** As a maintainer, I want chaos tests that verify Raft's four
safety properties under fault injection, so that correctness regressions are
caught automatically.

#### Acceptance Criteria

1. WHEN the `chaos_election_safety` test runs THEN for any sequence of faults
   that kills at most a minority of nodes, the test SHALL verify that at most
   one node considers itself leader in any term after the fault resolves.
2. WHEN the `chaos_log_matching` test runs THEN for any sequence of network
   faults that drop or delay messages, the test SHALL verify that if two nodes
   have a log entry at the same index with the same term, all preceding entries
   are identical between those two nodes.
3. WHEN the `chaos_leader_completeness` test runs THEN for any scenario where
   a committed entry exists in the cluster, the test SHALL verify that no future
   leader can be elected without that entry in its log.
4. WHEN the `chaos_state_machine_safety` test runs THEN for any combination of
   faults that allows at least two nodes to apply entries, the test SHALL verify
   that both nodes apply the same command bytes at the same log index.
5. WHEN any safety property is violated THEN the test SHALL fail with a
   diagnostic message identifying the nodes involved, the term/index at which
   the violation was detected, and the Fault_Points that were active.

### Requirement 6: Liveness Tests

**User Story:** As a maintainer, I want chaos tests that verify the cluster
recovers and makes progress after transient fault periods, so that I can detect
regressions in the retry and recovery logic.

#### Acceptance Criteria

1. WHEN the `chaos_leader_election_recovery` test runs THEN after a
   `network_partition_profile` is lifted from a minority partition, the test
   SHALL verify that a leader is elected within `2 × election_timeout_max`
   milliseconds.
2. WHEN the `chaos_commit_recovery` test runs THEN after all network faults
   stop, the test SHALL verify that a pending client command is committed
   within `10 × heartbeat_interval` milliseconds.
3. WHEN the `chaos_persistence_degradation_recovery` test runs THEN after a
   `disk_degradation_profile` is lifted, the test SHALL verify that the leader
   successfully persists log entries and the cluster commits a new command.
4. WHEN the `chaos_state_machine_recovery` test runs THEN after a
   `state_machine_fault_profile` is lifted, the test SHALL verify that the
   affected node resumes applying entries from the point it stopped.

### Requirement 7: Test Infrastructure

**User Story:** As a developer, I want the chaos tests to integrate cleanly
with the existing Boost.Test framework and CMake test targets, so that they
appear in `ctest` output alongside the other test suites.

#### Acceptance Criteria

1. WHEN chaos tests are compiled THEN they SHALL use `BOOST_AUTO_TEST_SUITE` /
   `BOOST_AUTO_TEST_CASE` consistent with all other test files in the project.
2. WHEN `ctest --label-regex chaos` is run THEN all chaos test executables
   SHALL be returned.
3. WHEN `cmake --build build --target chaos-tests` is run THEN only the chaos
   test executables SHALL be (re)compiled, without rebuilding unrelated tests.
4. WHEN a chaos test starts THEN it SHALL call `fiu_init(0)` exactly once and
   `fiu_disable_all()` in its teardown to prevent fault state from leaking
   across test cases within the same executable.
5. WHEN chaos tests are run via `ctest` THEN each test SHALL have a
   `*boost::unit_test::timeout(120)` decorator so that hung tests (e.g., a
   cluster that never recovers liveness) do not block CI indefinitely.

### Requirement 8: Documentation

**User Story:** As a contributor, I want the chaos testing integration
documented so that I understand how to add new fault points, write new chaos
tests, and run the suite locally.

#### Acceptance Criteria

1. WHEN the spec is implemented THEN `README.md` SHALL include a "Chaos
   Testing" section explaining: how to install libfiu, the `chaos-tests` CMake
   target, and how to run the chaos suite via `ctest --label-regex chaos`.
2. WHEN the "Chaos Testing" section is written THEN it SHALL document the
   Fault_Point naming convention and reference the catalogue in `design.md`.
3. WHEN the spec is complete THEN `doc/TODO.md` SHALL mark the chaos testing
   item done and add it to the "What Changed" summary.
