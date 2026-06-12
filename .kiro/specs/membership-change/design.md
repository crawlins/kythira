# Design Document

## Overview

This document describes the design for completing the joint consensus membership
change protocol in `kythira::node`. The infrastructure (API, `configuration_synchronizer`,
`cluster_configuration`) already exists; the work is wiring the Raft log path
through it.

Five changes are required:

1. Add an `entry_type` discriminant to `log_entry` and update serialization.
2. In `add_server()` / `remove_server()`, append a C_old+new configuration entry
   and update `_configuration` immediately on the leader.
3. In `handle_append_entries_response()`, use joint-quorum logic when the active
   configuration is a joint consensus configuration.
4. In `apply_committed_entries()`, detect configuration entries, update
   `_configuration`, and call `notify_configuration_committed()`.
5. After committing C_old+new, append C_new and handle leader step-down on
   self-removal.

## Architecture

```
add_server(new_node)
  │
  ├── validate preconditions (leader, no in-flight change, uniqueness)
  ├── _config_synchronizer.start_configuration_change(C_new, timeout)
  ├── append C_old+new config entry to log
  ├── _configuration ← C_old+new (immediate, pre-commit per §6)
  └── return future (resolved by configuration_synchronizer)

AppendEntries replication loop (unchanged except new quorum helper)
  ├── replicate to all nodes in _configuration.nodes()  ← covers C_old ∪ C_new
  └── advance_commit_index_with_joint_quorum()
        ├── if _configuration.is_joint_consensus():
        │     advance only if majority(C_old) ∧ majority(C_new) both acked
        └── else: standard majority check (unchanged)

apply_committed_entries() for a configuration entry E:
  ├── if E.is_joint_consensus():
  │     ├── update _configuration ← E.config()
  │     ├── _config_synchronizer.notify_configuration_committed(E.config(), E.index())
  │     └── append C_new config entry (leader only)
  └── else (C_new final entry):
        ├── update _configuration ← E.config()
        ├── _config_synchronizer.notify_configuration_committed(E.config(), E.index())
        │     └── resolves promise → fulfills add_server()/remove_server() future
        └── if self was removed: step down to follower
```

## Components and Interfaces

### 1. `include/raft/types.hpp` — `entry_type` enum and `log_entry` update

Add a scoped enum and extend the `log_entry` struct:

```cpp
enum class entry_type : std::uint8_t {
    normal = 0,         // State machine command
    configuration = 1,  // Cluster membership change
};

template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
struct log_entry {
    TermId _term;
    LogIndex _index;
    std::vector<std::byte> _command;
    entry_type _type = entry_type::normal;  // new field

    [[nodiscard]] auto term() const -> TermId { return _term; }
    [[nodiscard]] auto index() const -> LogIndex { return _index; }
    [[nodiscard]] auto command() const -> const std::vector<std::byte>& { return _command; }
    [[nodiscard]] auto type() const -> entry_type { return _type; }  // new accessor
};
```

The `log_entry_type` concept gains a `type()` requirement:

```cpp
concept log_entry_type = requires(const T& entry) {
    // ... existing term, index, command requirements ...
    { entry.type() } -> std::same_as<entry_type>;
};
```

Configuration entries encode the `cluster_configuration` payload as JSON bytes
in `_command` (reusing the existing JSON serializer infrastructure).

### 2. `include/raft/json_serializer.hpp` — configuration entry serialization

`log_entry` serialization gains a `"type"` field (defaults to `0` = `normal`
for backwards compatibility). A new helper pair:

```cpp
// Serialize a configuration entry's command payload
auto serialize_configuration(const cluster_configuration<NodeId>& config)
    -> std::vector<std::byte>;

// Deserialize a configuration entry's command payload
auto deserialize_configuration(const std::vector<std::byte>& payload)
    -> cluster_configuration<NodeId>;
```

Backwards compatibility: existing entries without a `"type"` key deserialize
as `entry_type::normal`.

### 3. `include/raft/raft.hpp` — `add_server()` / `remove_server()` changes

After `start_configuration_change()` succeeds, the leader:

```cpp
cluster_configuration_type joint_config{
    C_new_nodes,
    true,               // is_joint_consensus
    _configuration.nodes()  // old_nodes
};

// Encode joint config as a configuration log entry
auto payload = _serializer.serialize_configuration(joint_config);
log_entry_type config_entry{_current_term, get_last_log_index() + 1,
                             payload, entry_type::configuration};
append_log_entry(config_entry);

_configuration = joint_config;  // take effect immediately (§6)
```

The existing `config_future.thenTry(...)` callback remains — it fires when
`configuration_synchronizer` resolves the promise after C_new commits.

### 4. `include/raft/raft.hpp` — joint quorum helper

A private method replaces the existing `_configuration.nodes().size()`
majority check in `handle_append_entries_response()`:

```cpp
auto calculate_new_commit_index() const -> log_index_type;
```

Logic:
- For each candidate index N > `_commit_index` (latest to oldest):
  - If `_configuration.is_joint_consensus()`:
    - Count ACKs among `_configuration.old_nodes()` → majority in C_old?
    - Count ACKs among `_configuration.nodes()` → majority in C_new?
    - If both: return N
  - Else:
    - Count ACKs among `_configuration.nodes()` → standard majority check
    - If satisfied: return N
- Return `_commit_index` if no candidate qualifies.

The leader's own `_match_index[_node_id]` is always set to `get_last_log_index()`
(already the case) and counts toward quorum in whichever sets it belongs to.

### 5. `include/raft/raft.hpp` — `apply_committed_entries()` changes

After fetching `entry` inside the apply loop:

```cpp
if (entry.type() == entry_type::configuration) {
    // Deserialize configuration from payload
    auto config = _serializer.deserialize_configuration(entry.command());

    // Update live configuration
    _configuration = config;

    // Advance configuration_synchronizer phase
    _config_synchronizer.notify_configuration_committed(config, entry.index());

    // If this is the joint entry and we are leader, append C_new immediately
    if (config.is_joint_consensus() && _state == server_state::leader) {
        cluster_configuration_type final_config{config.nodes(), false, std::nullopt};
        auto payload = _serializer.serialize_configuration(final_config);
        log_entry_type cnew_entry{_current_term, get_last_log_index() + 1,
                                  payload, entry_type::configuration};
        append_log_entry(cnew_entry);
    }

    // If this is C_new and we were removed, step down
    if (!config.is_joint_consensus() && _state == server_state::leader) {
        bool self_in_new = std::find(config.nodes().begin(),
                                     config.nodes().end(),
                                     _node_id) != config.nodes().end();
        if (!self_in_new) {
            _state = server_state::follower;
        }
    }

    _last_applied = next_index;
    continue;  // skip state_machine.apply()
}
```

### 6. Follower configuration update on log append

In `handle_append_entries()`, after storing an entry in the persistence engine,
detect configuration entries and apply them to `_configuration` immediately:

```cpp
for (auto& entry : req.entries()) {
    // ... existing conflict-resolution and truncation logic ...
    _persistence.append_log_entry(entry);
    if (entry.type() == entry_type::configuration) {
        _configuration = _serializer.deserialize_configuration(entry.command());
    }
}
```

On log truncation, revert `_configuration` by scanning backwards through the
persisted log for the most recent configuration entry, falling back to the boot
configuration if none is found.

### 7. `initialize_from_storage()` — full state recovery on restart

Currently only `current_term` and `voted_for` are loaded. The complete recovery
sequence is:

```cpp
auto node<Types>::initialize_from_storage() -> void {
    // Step 1: hard Raft state (already present)
    _current_term = _persistence.load_current_term();
    _voted_for    = _persistence.load_voted_for();

    // Step 2: snapshot (if any)
    if (const auto snap_opt = _persistence.load_snapshot()) {
        const auto& snap = *snap_opt;
        _state_machine.restore_from_snapshot(snap.state_machine_state(),
                                             snap.last_included_index());
        _last_applied  = snap.last_included_index();
        _commit_index  = snap.last_included_index();
        _configuration = snap.configuration();
    }

    // Step 3: log entries appended after the snapshot
    const auto first_log_index = _last_applied + 1;
    const auto last_log_index  = _persistence.get_last_log_index();
    if (last_log_index >= first_log_index) {
        _log = _persistence.get_log_entries(first_log_index, last_log_index);
    }

    // Step 4: restore configuration from the most recent config entry in the log
    for (auto it = _log.rbegin(); it != _log.rend(); ++it) {
        if (it->type() == entry_type::configuration) {
            _configuration = _serializer.deserialize_configuration(it->command());
            break;
        }
    }
}
```

The boot configuration (`set_cluster_configuration()`) is applied before
`initialize_from_storage()` is called (current constructor order is preserved).
If a snapshot or log config entry exists, it overwrites the boot configuration —
which is correct because the stored configuration is authoritative after the
node has participated in the cluster.

### 8. `install_snapshot()` — restore `_configuration` (pre-existing gap)

`install_snapshot()` already restores `_state_machine`, `_last_applied`,
`_commit_index`, and truncates `_log`, but never reads `snap.configuration()`.
Add one line after the existing state machine restore:

```cpp
_configuration = snap.configuration();
```

## Data Models

### `entry_type` encoding in JSON

```json
{ "term": 3, "index": 12, "command": "..base64..", "type": 1 }
```

`"type": 0` (or absent) = `normal`. `"type": 1` = `configuration`. The
`"command"` field for a configuration entry contains the JSON-encoded
`cluster_configuration`:

```json
{
  "nodes": [1, 2, 3, 4],
  "is_joint_consensus": true,
  "old_nodes": [1, 2, 3]
}
```

## Correctness Properties

### Property 1: Single leader during membership change
**Validates: Requirements 3.1, 3.3**

At no point during or after a membership change can two nodes both believe
they are leader for the same term. Demonstrated by: joint quorum requires
majority of C_old to elect in phase 1; majority of C_new suffices only in
phase 2 after C_old+new is committed.

### Property 2: Configuration monotonicity
**Validates: Requirements 4.2, 4.3**

Every node's `_configuration` can only change to configurations that appear
at strictly increasing log indices. A node never regresses to an older
configuration except on log truncation (Requirement 6.2).

### Property 3: New member log catch-up
**Validates: Requirements 7.2**

After `add_server()` resolves, the new node has a `match_index` equal to the
leader's `last_log_index` at the time C_new committed, meaning it has received
all log entries up to and including C_new.

### Property 4: Removed member quorum exclusion
**Validates: Requirements 7.3**

After `remove_server()` resolves, entries that achieve majority in C_new
(which excludes the removed node) are considered committed. The removed node's
`match_index` is no longer consulted.

### Property 5: Recovery idempotence
**Validates: Requirements 8.1, 8.2, 8.3**

A node that restarts after applying N entries (with or without a snapshot)
arrives at the same `_last_applied`, `_log`, and `_configuration` as a node
that never restarted. The cluster cannot tell the difference.

## Error Handling

- If `start_configuration_change()` fails (already in progress), `add_server()` /
  `remove_server()` return an exceptional future immediately — no log entry is
  appended. This is the existing behaviour and is preserved.
- If `append_log_entry()` throws (persistence failure), the configuration change
  is cancelled via `_config_synchronizer.cancel_configuration_change()` and the
  exception propagates through the future.
- If the leader loses leadership during a membership change,
  `cancel_all_operations_leadership_lost()` fires as it does today for normal
  entries. `configuration_synchronizer.cancel_configuration_change()` is called
  from the same leadership-change path.
- If a follower truncates entries that include a C_old+new or C_new entry, it
  scans backwards to restore the correct `_configuration`.

## Testing Strategy

New tests live in `tests/membership_change_*.cpp` following the naming
conventions of the existing test suite. They use `test_raft_types` and the
network simulator directly.

- `membership_change_single_node_unit_test.cpp` — unit tests for the type
  discriminant, serialization round-trips, `calculate_new_commit_index()`,
  and `configuration_synchronizer` phase transitions.
- `membership_change_add_server_property_test.cpp` — property test: add a
  fourth node to a 3-node cluster, verify C_old+new then C_new commit, verify
  the new node reaches the same `last_applied` as the others.
- `membership_change_remove_server_property_test.cpp` — property test: remove
  one node from a 3-node cluster, verify the cluster continues to make progress
  as a 2-node cluster, verify the removed node stops receiving heartbeats.
- `membership_change_leader_crash_property_test.cpp` — property test: crash
  the leader between appending C_old+new and committing C_new, elect a new
  leader, verify the change eventually completes or is cleanly rolled back.
- `node_recovery_unit_test.cpp` — unit tests for `initialize_from_storage()`:
  recover with no data, recover from term/vote only, recover from snapshot only,
  recover from snapshot + trailing log entries, recover with a configuration
  entry in the log overriding the boot configuration, and verify that
  `install_snapshot()` restores `_configuration`.
