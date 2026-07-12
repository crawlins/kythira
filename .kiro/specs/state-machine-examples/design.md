# Design Document

## Overview

This spec closes out `doc/TODO.md`'s "State machine examples" item by
bringing `replicated_log_state_machine` and `distributed_lock_state_machine`
to the same standard as `counter_state_machine` and `register_state_machine`:
a Boost.Test unit test file, a CMake target, and a `ctest` entry. No new
production abstractions are introduced — the work is a determinism bug fix
confined to one header, plus two new test files and a `tests/CMakeLists.txt`
addition, following patterns that already exist in the tree.

The one design decision of substance is how to fix
`distributed_lock_state_machine`'s use of wall-clock time inside `apply()`.

## Component Design

### 1. Distributed lock determinism fix — `include/raft/examples/distributed_lock_state_machine.hpp`

**Problem.** Current code (lines 72-83, 94-106):

```cpp
auto acquire(...) -> std::vector<std::byte> {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto& lock = _locks[lock_id];
    if (lock.owner.empty() || lock.expiry_ms < static_cast<std::uint64_t>(now)) {
        lock.owner = owner;
        lock.expiry_ms = now + timeout_ms * 1000000;
        return to_bytes("OK");
    }
    return to_bytes("LOCKED");
}
```

`steady_clock::now()` is per-process and per-machine. Two replicas applying
the identical `ACQUIRE` command at the identical log index can observe
different `now()` values (clock drift, GC/scheduling jitter, or simply
running on different physical machines), so one replica may see the lock as
expired while another does not. That's a `state_machine` concept violation
in spirit even though it type-checks: `apply()` must be a pure function of
`(current_state, command, index)`.

**Fix: Log_Index_Based_Expiry.** Replace wall-clock expiry with the `index`
argument that Raft already guarantees is identical across every replica for
a given committed command — that's the entire point of the log. The
`timeout_ms` command argument is reinterpreted as `timeout_entries`: the
number of subsequently-applied log entries after which the lock auto-expires.

```cpp
struct lock_info {
    std::string owner;
    std::uint64_t expiry_index;   // was: expiry_ms
};

auto apply(const std::vector<std::byte>& command, std::uint64_t index)
    -> std::vector<std::byte> {
    // ... dispatch unchanged, but acquire/release/query now take `index` ...
}

auto acquire(const std::string& lock_id, const std::string& owner,
             std::uint64_t timeout_entries, std::uint64_t index)
    -> std::vector<std::byte> {
    auto& lock = _locks[lock_id];
    if (lock.owner.empty() || index >= lock.expiry_index) {
        lock.owner = owner;
        lock.expiry_index = index + timeout_entries;
        return to_bytes("OK");
    }
    return to_bytes("LOCKED");
}

auto query(const std::string& lock_id, std::uint64_t index) -> std::vector<std::byte> {
    auto it = _locks.find(lock_id);
    if (it == _locks.end()) {
        return to_bytes("FREE");
    }
    if (index >= it->second.expiry_index) {
        _locks.erase(it);
        return to_bytes("FREE");
    }
    return to_bytes("LOCKED:" + it->second.owner);
}
```

`release()` is already deterministic (pure ownership check) and needs no
change beyond the field rename propagating through `get_state()` /
`restore_from_snapshot()`, which serialize `expiry_index` instead of
`expiry_ms` — same wire format shape (`lock_id:owner:number;`), just a
different number.

This keeps the change minimal: no new dependencies, no interface change to
`apply(command, index)` / `get_state()` / `restore_from_snapshot(state,
index)`, and no change to `counter`/`register`/`replicated_log`. The command
grammar's third `ACQUIRE` argument changes meaning (entries instead of
milliseconds); `README.md`'s worked example is updated accordingly (see
Requirement 1.5 / Requirement 6.2).

**Why not inject a clock via the command instead?** An alternative would
have the leader stamp a wall-clock timestamp into the command string before
proposing it (a standard Raft pattern for deterministic time), preserving
millisecond semantics. That's more faithful to "timeout in ms" but adds a
second concept — client-supplied timestamps — that the other three examples
don't need and that would require its own validation story (a malicious or
clock-skewed client could pass an arbitrary timestamp). Log-index-based
expiry is simpler, requires no extra trust assumption, and is still a
realistic pattern (e.g., "lease expires after N terms of inactivity").
Given this is a teaching example, the simpler and more obviously-correct
design wins.

### 2. Test files

Both new test files mirror `tests/counter_state_machine_test.cpp` /
`tests/register_state_machine_test.cpp` structurally: `BOOST_TEST_MODULE`,
a `make_command()` helper in an anonymous namespace, one `BOOST_AUTO_TEST_CASE`
per behavior, `*boost::unit_test::timeout(10)` on each case.

`tests/replicated_log_state_machine_test.cpp` — see Requirement 2 acceptance
criteria for the exact case list. No design novelty; entries are compared by
decoding `get_state()`'s length-prefixed byte layout back into
`(index, data)` pairs the same way `restore_from_snapshot()` does, so the
embedded-null-byte case (2.4) actually stresses the encoding rather than the
test's own string handling (`std::vector<std::byte>` throughout, no
`std::string` conversions that would truncate at `'\0'`).

`tests/distributed_lock_state_machine_test.cpp` — see Requirement 3.
Because `_locks` is an `unordered_map`, the determinism test (3.6) can't
compare `get_state()` byte-for-byte across arbitrary insertion orders in
general, but since both instances receive commands in the same order and
`unordered_map` iteration order is a pure function of hash + insertion/erase
history for a given libstdc++ build, two instances built identically and fed
identical command sequences do produce identical iteration order in
practice. The test asserts this directly rather than assuming it, so if it
ever proves flaky, that's a real signal that `get_state()` needs a
canonical (sorted) serialization — a possible follow-up, not in scope here
since counter/register/replicated_log's snapshot formats are already
order-independent by construction (single scalar / append-only vector).

### 3. CMake wiring — `tests/CMakeLists.txt`

Inserted immediately after the existing `register_state_machine_test` block
(around line 1671), before the "State machine integration test" comment,
using the identical five-call pattern already used for counter/register:

```cmake
add_executable(replicated_log_state_machine_test replicated_log_state_machine_test.cpp)
target_link_libraries(replicated_log_state_machine_test PRIVATE
    network_simulator
    Boost::unit_test_framework
)
target_compile_features(replicated_log_state_machine_test PRIVATE cxx_std_23)
add_test(NAME replicated_log_state_machine_test COMMAND replicated_log_state_machine_test)
set_tests_properties(replicated_log_state_machine_test PROPERTIES
    TIMEOUT 30
    LABELS "unit;state_machine;example"
)

# (same shape for distributed_lock_state_machine_test)
```

`network_simulator` is linked because that's what counter/register link
against — checked against `tests/CMakeLists.txt`, it's a project-wide
convenience target for test binaries rather than something these
state-machine-only tests actually call; kept for consistency with the
existing two rather than introducing an inconsistent third pattern.

### 4. Concept static_asserts

Each new test file adds, near the top after includes:

```cpp
static_assert(kythira::state_machine<replicated_log_state_machine, std::uint64_t>,
              "replicated_log_state_machine must satisfy state_machine concept");
```

and the distributed-lock equivalent. This requires `#include <raft/types.hpp>`
in each new test file (currently only `tests/raft_state_machine_concept_test.cpp`
does this for the built-in test state machine; the two existing example
tests don't assert this at all — adding it here is new but low-risk, and
directly protects against Requirement 1's fix accidentally breaking the
concept-required signatures).

## Non-Goals

- No changes to `counter_state_machine.hpp` or `register_state_machine.hpp`
  — both are already tested and correct.
- No change to `distributed_lock_state_machine`'s command dispatch structure,
  error message text, or `RELEASE`/ownership semantics — only the expiry
  mechanism changes.
- Not attempting to make `distributed_lock_state_machine`'s `get_state()`
  serialization order-canonical (see Component Design §2) unless the
  determinism test proves it necessary.
- Not adding these examples to `examples/raft/` as standalone runnable
  programs — `doc/TODO.md`'s item scope is "documentation/demonstration,"
  which the existing `include/raft/examples/README.md` plus tests already
  satisfy, matching how counter/register were closed out.
