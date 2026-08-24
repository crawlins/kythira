# Design Document — Multi-Raft

## Overview

This design adds a sharding layer **above** Kythira's existing `node<Types>`
rather than inside it. One `node<Types>` continues to mean one Raft group; a
new host object, `multi_raft<Types>`, owns many of them, demultiplexes one
shared transport across them, drives them from one batched tick, and
orchestrates the split and merge of the shards they replicate.

Two properties of the current codebase make this layering possible with very
little surgery on `raft.hpp`:

1. **`node<Types>` is externally ticked.** It has no timer thread of its own;
   `check_election_timeout()`, `check_heartbeat_timeout()`, and
   `replicate_to_followers()` are called from outside (see
   `cmd/chaos_node/main.cpp:125`, `cmd/ca_cluster_node/main.cpp:956`,
   `examples/raft/basic_cluster.cpp:206`). A multi-group host is therefore just
   a better caller — exactly the shape TiKV's batched ready loop wants.
2. **Every dependency is a concept.** `network_client`, `network_server`,
   `persistence_engine`, `state_machine`, `metrics`, `logger`,
   `membership_manager`, `quorum_manager`, `peer_discovery` are all injected
   through `Types`. A group-scoped *view* of a transport or a store satisfies
   the same concept, so `node<Types>` cannot tell whether it is alone in the
   process.

The design is deliberately faithful to TiKV where TiKV has already paid for the
lesson — key-range shards, `RegionEpoch`, the three-entry merge, batch split,
advisory PD operators, connection reuse across groups — and faithful to
MicroRaft where MicroRaft has the cleaner abstraction — `[group id, node id]`
composite identity, narrow transport contract, store-does-not-persist-the-state-
machine, not-leader-carries-the-leader-endpoint, serial per-group execution.

The single largest section below is **§6, Signals**, because that is where a
user actually meets this feature.

---

## Architecture

```
                       ┌──────────────────────────────────────────┐
   application ───────▶│  multi_raft<Types>                       │
   submit_command(key) │                                          │
                       │  ┌────────────────────────────────────┐  │
                       │  │ shard_map<GroupId, Key>            │  │  routing
                       │  │  [-∞,b) → g1   [b,m) → g2  …       │  │
                       │  └────────────────────────────────────┘  │
                       │                                          │
                       │  ┌────────────────────────────────────┐  │
                       │  │ split_merge_arbiter                │  │  §6
                       │  │   admin ▸ pd operator ▸ policy     │  │
                       │  │   gates: state, cooldown, limit    │  │
                       │  └────────────────────────────────────┘  │
                       │                                          │
                       │  ┌────────────────────────────────────┐  │
                       │  │ group registry                     │  │
                       │  │  g1 → node<GroupTypes>             │  │
                       │  │  g2 → node<GroupTypes>   … gN      │  │
                       │  └────────────────────────────────────┘  │
                       │        │                    │            │
                       │  group_scoped_*       striped serial     │
                       │  transport / store       executor        │
                       └────────┼────────────────────┼────────────┘
                                │                    │
                    ┌───────────▼─────────┐   ┌──────▼─────────┐
                    │ multi_group_network │   │  tick() loop   │
                    │ _server / _client   │   │  ready→persist │
                    │ (one connection per │   │  →send→apply   │
                    │  node pair, demuxed │   └────────────────┘
                    │  on group_id)       │
                    └─────────────────────┘
                                │
                    ┌───────────▼─────────────────────────────┐
                    │ shard_placement_driver (optional, §7)   │
                    │  ids · heartbeats · advisory operators  │
                    └─────────────────────────────────────────┘
```

### What changes in existing files

| File | Change | Why |
|---|---|---|
| `include/raft/types.hpp` | `group_id()` accessor + `_group_id` field on the six RPC request/response structs; `entry_type` gains `split`, `merge_prepare`, `merge_commit`, `merge_rollback`, `merge_abandoned` | Requirement 4.1, 11.1, 13.3 |
| `include/raft/raft.hpp` | `set_admin_entry_handler()`; admin entries are **not** passed to `state_machine.apply()`; `propose_admin_entry()`; expose `match_index_of(node)` for merge `min_index` | Split/merge logic stays out of the consensus core |
| `include/raft/{json,cbor,ion,protobuf}_serializer.hpp`, `grpc_message_conversion.hpp`, `proto/raft*.proto` | round-trip `group_id`, absent ⇒ default | Requirement 4.2–4.3 |
| everything else | untouched | |

### New files

```
include/raft/shard_types.hpp          shard_key, shard_range, shard_epoch,
                                      shard_descriptor, shard_stats, decisions
include/raft/shard_map.hpp            ordered routing table + tiling invariant
include/raft/group_transport.hpp      multi_group_network_{server,client},
                                      group_scoped_{server,client}
include/raft/group_storage.hpp        group_scoped_persistence, batching ext,
                                      tombstone set
include/raft/split_merge_policy.hpp   the policy concept + threshold_* default
include/raft/load_split_sampler.hpp   RFC-0045 sampler
include/raft/shard_placement_driver.hpp   PD concept + no_op default
include/raft/multi_raft.hpp           the host: registry, tick, arbiter,
                                      split/merge orchestration
include/raft/multi_raft_impl.hpp      out-of-line template definitions
include/raft/shard_exceptions.hpp     typed errors
```

Style follows what `include/raft/` actually does — snake_case types,
`_leading_underscore` members, `auto f() -> T`, concepts as the extension
mechanism, `if constexpr` structural detection for optional capabilities.
(Note `.kiro/steering/cpp-coding-standards.md` prescribes PascalCase classes;
the Raft headers are uniformly snake_case and consistency with the surrounding
code wins.)

---

## 1. Shard model

### 1.1 Keys and ranges

```cpp
/// A routing key. Kythira only ever compares and serialises these.
template<typename K>
concept shard_key = std::totally_ordered<K> && std::copyable<K> &&
                    std::default_initializable<K>;

/// Half-open [start, end) with explicit unbounded ends, so that the initial
/// single shard is (-inf, +inf) and its first split needs no reserved
/// sentinel value from the application's key domain.
template<shard_key Key>
struct shard_range {
    std::optional<Key> _start;   ///< nullopt == unbounded below
    std::optional<Key> _end;     ///< nullopt == unbounded above

    [[nodiscard]] auto contains(const Key& k) const -> bool {
        if (_start && k < *_start) return false;
        if (_end   && !(k < *_end)) return false;
        return true;
    }
    [[nodiscard]] auto is_adjacent_left_of(const shard_range& other) const -> bool {
        return _end.has_value() && other._start.has_value() && *_end == *other._start;
    }
};
```

`std::optional<Key>` for the bounds is the whole reason a first split works
without the application reserving a minimum and maximum key. TiKV can use `""`
and `b"\xff…"` because its domain is bytes; Kythira's domain is whatever the
user chose.

### 1.2 Epoch

```cpp
struct shard_epoch {
    std::uint64_t _version{0};       ///< ++ on every range change
    std::uint64_t _conf_version{0};  ///< ++ on every membership change
    auto operator<=>(const shard_epoch&) const = default;
};
```

Exactly TiKV's `RegionEpoch`. The two counters are separate because a
membership change and a range change are independently stale-making: a client
holding an old `version` has the wrong *range*; a peer holding an old
`conf_version` has the wrong *voters*.

Rules, enforced at both admission and apply time (Requirement 3.7 — an entry
proposed under one epoch can commit after the epoch moved):

- split into N children: every child gets `version = parent.version + N`
  (using a single value keeps ordering total and makes the "did I miss a split"
  check a comparison rather than a set difference);
- merge: survivor gets `version = max(src.version, tgt.version) + 1`;
- any committed configuration entry: `conf_version + 1`.

### 1.3 Descriptor and map

```cpp
template<typename GroupId, shard_key Key, typename NodeId>
struct shard_descriptor {
    GroupId _group_id;
    shard_range<Key> _range;
    shard_epoch _epoch;
    std::vector<NodeId> _voters;
    std::vector<NodeId> _learners;
    std::optional<NodeId> _leader_hint;
};
```

`shard_map` is a `std::map<std::optional<Key>, descriptor>` keyed on range
start with a comparator that orders `nullopt` first. `lookup(key)` is
`upper_bound` then step back. It exposes:

```cpp
auto lookup(const Key&) const -> std::optional<descriptor>;
auto range_scan(const shard_range<Key>&) const -> std::vector<descriptor>;
auto apply_split(const descriptor& parent, const std::vector<descriptor>& children) -> void;
auto apply_merge(const descriptor& source, const descriptor& survivor) -> void;
[[nodiscard]] auto check_tiling() const -> std::optional<std::string>;  // test/debug
```

`check_tiling()` returns a description of the first gap or overlap, or
`nullopt`. It is the executable form of Requirement 2.3 and is what the
property tests in §10 assert on. Making it a first-class method rather than
test-only code means it can also be called under an assertion in debug builds
after every `apply_split`/`apply_merge`.

The map is durable: it is persisted alongside each local group's state, so a
restart reconstructs routing without a PD round trip (Requirement 2.6). The
authoritative copy of any *row* is the group that owns it; the local map is a
cache that epoch checks make safe to be stale.

### 1.4 Partitioner

```cpp
template<typename P, typename Key>
concept partitioner = requires(const P& p, const std::vector<std::byte>& command) {
    { p.key_of(command) } -> std::same_as<Key>;
};
```

One function. Kythira never parses a command; the application already has a
parser and hands over just the routing key. For a state machine whose commands
carry several keys, `key_of` returns the routing key and the state machine is
responsible for the rest — with `can_split_at` (§6.4) as the tool to keep a
multi-key command's keys inside one shard.

---

## 2. Group identity and transport demultiplexing

### 2.1 Composite identity

MicroRaft's `[group id, node id]` is adopted verbatim as the identity of a
replica. Concretely, `node<Types>` keeps taking a plain `node_id_type`; the
group half lives in the registry key and in the group-scoped transport view.
Nothing inside the consensus core needs to know its own group id, which is why
the core needs no change here.

```cpp
template<typename T>
concept raft_group_id = std::regular<T> && std::totally_ordered<T> &&
                        requires(const T& g) { { std::hash<T>{}(g) } -> std::convertible_to<std::size_t>; };
```

### 2.2 The wire field

Each RPC struct gains one field and one accessor:

```cpp
struct append_entries_request {
    GroupId _group_id{};        // NEW — default-constructed == "the single group"
    TermId _term;
    NodeId _leader_id;
    // …
    [[nodiscard]] auto group_id() const -> GroupId { return _group_id; }
};
```

Backward compatibility is by *default value*, not by a version flag:

- JSON/CBOR/Ion: emit the key always; on decode, a missing key yields
  `GroupId{}`. Existing recorded payloads decode unchanged.
- Protobuf: a new field number appended to each message. Old encoders omit it;
  proto3 decodes an absent scalar as zero, which is precisely `GroupId{}`.
- gRPC conversion: mirror the protobuf change.

Adding fields at the *front* of the aggregate would break every existing
designated-initialiser call site in tests. Append them at the **end** of each
struct instead, since these structs are initialised with designated
initialisers throughout `raft.hpp` and the test suite.

### 2.3 The demultiplexer

`node<Types>::register_rpc_handlers()` (raft.hpp:3050) registers exactly one
handler per RPC type with its server, and the server concept has one slot per
type. So the demux lives one level down:

```cpp
template<typename Server, typename GroupId, /* message types … */>
class multi_group_network_server {
public:
    explicit multi_group_network_server(Server inner);

    /// Called once per group by group_scoped_server.
    auto register_group(GroupId, group_handlers h) -> void;
    auto unregister_group(GroupId) -> void;

    /// Called by multi_raft for group ids with no local replica (Requirement 12).
    auto set_unknown_group_handler(std::function<unknown_group_action(GroupId)>) -> void;

    auto start() -> void;   // registers ONE handler per RPC type with `inner`
    auto stop() -> void;
private:
    Server _inner;
    kythira::synchronized<std::unordered_map<GroupId, group_handlers>> _groups;
};
```

`start()` installs a single lambda per RPC type on the inner server; that
lambda looks up `request.group_id()` and forwards. `synchronized<>` already
exists in `include/raft/synchronized.hpp`; the map is read-mostly, so a
shared-mutex wrapper or a copy-on-write pointer swap is appropriate — group
registration happens on split/merge, not per message.

`group_scoped_client<Client, GroupId>` wraps the shared client, stamping
`_group_id` on every outbound request, and satisfies `network_client` exactly.
`group_scoped_server<GroupId>` satisfies `network_server` by forwarding
`register_*_handler` calls into `multi_group_network_server::register_group`
and making `start()`/`stop()` no-ops (the shared server's lifecycle is the
host's).

**Consequence:** `node<Types>` is instantiated with a `Types` bundle whose
`network_client_type` / `network_server_type` are the scoped views. Nothing in
`raft.hpp` changes. This is the design's main leverage.

**Connection reuse** (Requirement 4.6) falls out for free: there is one inner
client and one inner server per process, so the existing transports' connection
pools are already shared across every group — which is TiKV's stated behaviour,
"TiKV reuses the connection between two nodes for multiple Raft groups."

Optional message batching — coalescing many groups' `AppendEntries` to the same
destination into one payload — is a further win but needs a transport-side
extension concept (`network_client_with_batch`). It is deferred to Phase 6 and
detected with `if constexpr`, in the same style as
`network_client_with_pre_vote`.

---

## 3. Storage

### 3.1 Namespacing

```cpp
template<typename Engine, typename GroupId>
class group_scoped_persistence {  // satisfies persistence_engine
    Engine _inner;      // or a reference to a shared engine
    GroupId _group;
    // key/path prefixing applied to every method
};
```

For the three engines that exist:

- `file_persistence_engine` takes a `std::filesystem::path data_dir`
  (file_persistence.hpp:43) — scoping is `data_dir / "groups" / to_string(group)`
  and needs no wrapper at all.
- `object_store_persistence` — prefix the object key.
- `memory_persistence_engine` — one instance per group; already isolated.

So the generic wrapper exists mainly for third-party engines and for the
batching extension below.

### 3.2 Batched writes

TiKV's ready loop "uses RocksDB's `WriteBatch` to handle all appending data and
persist the corresponding result" for every ready group at once. Without that,
N groups cost N `fsync`s per tick, which is the dominant cost at scale.

```cpp
template<typename P>
concept batched_persistence_engine = requires(P& p) {
    { p.begin_batch() } -> std::same_as<void>;
    { p.commit_batch() } -> std::same_as<void>;   // one durability barrier
    { p.abort_batch()  } -> std::same_as<void>;
};
```

Optional; detected with `if constexpr` in the tick. `file_persistence_engine`
gains an implementation that buffers appends and issues one `fsync` on the
directory at `commit_batch()`.

This concept is not only a performance feature — §5.4 relies on it for the
atomicity of split apply.

### 3.3 Tombstones

A durable `tombstone_set<GroupId>` records groups destroyed on this node (by
merge, or by replica removal). `multi_group_network_server`'s unknown-group
path consults it first (Requirement 12.4). Without it, a partitioned peer's
stale `AppendEntries` for a merged-away group resurrects a replica whose range
is now owned by someone else — a silent double-ownership bug and the exact
failure the tiling invariant would catch too late.

---

## 4. The host: `multi_raft<Types>`

```cpp
template<raft_types Types,
         shard_key Key,
         raft_group_id GroupId = std::uint64_t>
class multi_raft {
public:
    using group_node_type = node<group_scoped_types<Types, GroupId>>;

    explicit multi_raft(multi_raft_config<Types, Key, GroupId> cfg);

    // ── client surface (Requirement 16) ────────────────────────────────────
    auto submit_command(const Key&, const std::vector<std::byte>&,
                        std::chrono::milliseconds) -> future_type;
    auto submit_command(GroupId, shard_epoch, const std::vector<std::byte>&,
                        std::chrono::milliseconds) -> future_type;
    auto read_state(const Key&, std::chrono::milliseconds) -> future_type;

    // ── admin signal surface (Requirement 10, §6.2) ────────────────────────
    auto split_shard(GroupId, std::vector<Key>, split_options)  -> future<std::vector<descriptor>>;
    auto merge_shards(GroupId source, GroupId target, merge_options) -> future<descriptor>;
    auto pre_split(std::vector<Key> boundaries)                 -> future<std::vector<descriptor>>;
    auto freeze_shard(GroupId) -> void;
    auto thaw_shard(GroupId)   -> void;
    auto set_automatic_split_merge_enabled(bool) -> void;

    // ── lifecycle / driving ────────────────────────────────────────────────
    auto start() -> void;
    auto stop()  -> void;
    auto tick()  -> tick_report;

    // ── observability ──────────────────────────────────────────────────────
    [[nodiscard]] auto shard_map_snapshot() const -> shard_map<GroupId, Key, node_id_type>;
    [[nodiscard]] auto shard_report(GroupId) const -> std::optional<shard_report_type>;
    auto set_report_listener(std::function<void(const group_report&)>) -> void;
};
```

### 4.1 Registry and executor

The registry is `std::unordered_map<GroupId, std::shared_ptr<group_state>>`,
where `group_state` holds the `node`, its scoped store, its operation state
(§6.6), its statistics accumulators, and its serial-executor stripe index.

Following MicroRaft's `RaftNodeExecutor` — "enforces serial task execution
using the Actor model … maintaining happens-before relationships for
deterministic protocol execution" — each group is bound to one stripe of a
fixed-size pool: `stripe = hash(group_id) % pool_size`. All work for a group
(inbound RPC handling, tick phases, policy evaluation) runs on its stripe, so a
group is never concurrently entered, and the pool size is a machine property,
not a shard-count property. `node<Types>`'s own `_mutex` then becomes
uncontended rather than being relied on for correctness under fan-out.

One trap worth stating: a split creates a new group and must place it on a
stripe; a merge destroys one. Neither may run on the destroyed group's own
stripe while that stripe is executing the group — teardown is posted to the
host's control stripe after the group's queue is drained, reusing the existing
`async_scope::close_and_drain()` discipline from `node<Types>::stop()`
(raft.hpp:2188) rather than inventing a second shutdown protocol.

### 4.2 The tick

```
tick():
  ready   = groups with pending appends, expired timers, or pending applies
  hibernating groups are skipped unless woken

  PHASE 1  persist
     if constexpr (batched_persistence_engine<store>) store.begin_batch()
     for g in ready:  g.check_election_timeout(); g.check_heartbeat_timeout()
     if constexpr (batched)                        store.commit_batch()

  PHASE 2  send
     for g in ready:  g.replicate_to_followers()
     (Phase 6: coalesce per destination node into one payload)

  PHASE 3  apply
     for g in ready:  drive g's apply loop; admin entries dispatch to §5

  PHASE 4  policy   (every policy_interval, leaders only)
     for g in local leaders not frozen:  arbiter.evaluate(g)     // §6
```

Ordering matters and is TiKV's: persist before send (never advertise an append
you have not durably taken), and apply after send (a follower's copy is on the
wire before the leader spends time in the state machine).

`tick_report` returns ready/hibernating counts, batch size, and per-phase
durations for Requirement 5.7.

### 4.3 Hibernation

A group is hibernation-eligible when it is a leader with all followers at
`match_index == last_log_index` and no pending proposals for
`hibernate_after` (default: 10 × `heartbeat_interval`), or a follower that has
heard from its leader within the election timeout and has been told the leader
is hibernating. Wake conditions: any client request, any inbound RPC, any
configuration change, any placement-driver operator.

TiKV RFC 0082 is candid that hibernation "is not always working as expected"
under random access, and that it complicates tooling. The mitigation adopted
here is to make it a *policy knob* (`hibernation_mode:
{off, on, auto_above_group_count}`, default `auto_above_group_count = 64`)
rather than always-on, and to always report hibernating count so an operator
can see when it stops helping.

---

## 5. Split and merge mechanics

### 5.1 Admin entries — the one change to the consensus core

`entry_type` gains:

```cpp
enum class entry_type : std::uint8_t {
    normal = 0, configuration = 1, no_op = 2,
    split = 3, merge_prepare = 4, merge_commit = 5,
    merge_rollback = 6, merge_abandoned = 7,
};
```

and `node<Types>` gains:

```cpp
auto set_admin_entry_handler(
    std::function<void(const log_entry_type&, log_index_type)> h) -> void;

auto propose_admin_entry(entry_type, std::vector<std::byte> payload,
                         std::chrono::milliseconds) -> future_type;

[[nodiscard]] auto match_index_of(node_id_type) const -> std::optional<log_index_type>;
```

`apply_committed_entries()` routes entries whose type is one of the admin types
to the handler and **never** to `state_machine.apply()`. This mirrors how
`no_op` is already excluded, and keeps every line of split/merge logic out of
`raft.hpp`. `match_index_of` exists solely so the merge coordinator can compute
`min_index` (§5.5) without reaching into private state.

The handler runs **on the group's stripe, inside the apply loop, on every
replica** — it is not a leader-only callback. That is the property that makes
split and merge deterministic.

### 5.2 Split: the entry payload

```cpp
template<typename GroupId, shard_key Key, typename NodeId>
struct split_command {
    GroupId _parent_group;
    shard_epoch _parent_epoch;            // re-checked at apply time
    std::vector<Key> _at_keys;            // ordered, all inside the parent range
    std::vector<shard_descriptor<GroupId, Key, NodeId>> _children;  // fully derived
    bool _right_derive;                   // which child inherits the parent's id/log/term
    split_reason _reason;                 // for logs and metrics
    std::optional<std::uint64_t> _pd_operation_id;
};
```

Everything the apply step needs is *in the entry*. No replica recomputes
anything from local statistics, consults a policy, or calls the placement
driver at apply time. This is the single most important rule in the design: the
policy is allowed to be non-deterministic (Requirement 6.3) precisely because
its output is frozen here.

`_children` carries fully derived descriptors, including each child's member
list. Members are the parent's members, one-for-one: a child replica is created
on exactly the machines that already hold a parent replica. No data moves,
which is TiKV's stated advantage of range partitioning — "region splits and
merges require only metadata changes".

### 5.3 Split: proposing (leader only)

```
1. gate       arbiter admits (state == stable, cooldown ok, concurrency ok)   §6.6
2. keys       candidate keys from the channel; drop any k where
              !state_machine.can_split_at(k);  if empty, ask
              state_machine.suggest_split_keys(batch_split_limit);
              if still empty → no_valid_split_key_exception, cooldown, done
3. ids        pd.allocate_shard_ids(children_count)   ── may fail ⇒ abandon
4. derive     build children descriptors; version = parent.version + N
5. state      arbiter marks the shard `splitting`
6. propose    propose_admin_entry(split, serialize(split_command), timeout)
```

Step 3 is not optional and not local. Every replica must derive identical group
ids and identical replica ids; only a cluster-scope authority can guarantee
uniqueness. TiKV made the same call with `AskBatchSplit`. If the PD is
unavailable the split is *abandoned*, not queued with locally invented ids —
inventing ids locally is how you get two different shards with the same group
id in two partitions.

Step 2's ordering — the veto first, the suggestion as fallback — is what
Requirement 9.4 buys the application: a policy that says "split at key K"
cannot cut through an entity the state machine says is indivisible.

### 5.4 Split: applying (every replica)

```
apply_split(cmd, at_index):
  A. if local_epoch != cmd._parent_epoch            → skip (already applied / stale)
  B. if every child already exists at cmd's epoch   → return  (idempotent replay)
  C. freeze parent: reject client ops with shard_epoch_mismatch(children)
  D. blobs = state_machine.split_state(cmd._at_keys)     // deterministic
  E. begin_batch()                                        // if supported
       for each non-derived child c:
           write c's synthetic snapshot:
             { last_included_index = at_index,
               last_included_term  = term_of(at_index),
               configuration       = parent config restricted to c's members,
               state_machine_state = blob(c) }
           write c's descriptor row
       derived child: narrow range; restore_from_snapshot(blob(derived), at_index)
       write parent's new descriptor row + advanced apply index
     commit_batch()
  F. instantiate non-derived children in the registry, register with the
     transport demux, assign stripes
  G. publish shard_map rows; assert check_tiling() in debug builds
  H. unfreeze
  I. stagger children's election timers; the child colocated with the parent's
     leader may campaign immediately
  J. (leader only) pd.report_split(parent, children)
```

**Step E is where crash safety lives.** The child's durable initial state and
the parent's advanced apply index must land together. With
`batched_persistence_engine` that is one batch and one barrier. Without it, the
order is: children first, then the parent's apply index — and step B makes
replay a no-op, so a crash between the two replays the split entry and finds
the children already present. Getting this backwards — parent index first —
loses a child permanently on a crash, and the loss is silent until a client
asks for a key in that range.

**Step E's synthetic snapshot is the reason a split moves no data.** The child
does not copy the parent's log. It begins at the parent's apply index with an
empty log and a snapshot that *is* its share of the parent's state. TiKV does
the same thing with `RAFT_INIT_LOG_INDEX`; here it is expressed through the
existing `snapshot_type` and `restore_from_snapshot()` contract, so no new
persistence primitive is needed.

**Step I** matters more than it looks. N children electing simultaneously across
the same three machines produces a burst of `RequestVote` traffic and a
correlated leader-election latency spike on every child. Staggering, plus
letting the child on the old leader's machine campaign immediately, keeps the
unavailability window down to roughly one election timeout for one child and
near zero for the derived one.

### 5.5 Merge: the protocol

A merge spans two Raft groups, so unlike a split it is genuinely a distributed
operation. The protocol is TiKV's, with one deliberate deviation (§5.7).

**Preconditions**, checked at proposal and re-checked at apply:

| Precondition | Failure |
|---|---|
| ranges adjacent | `shard_not_adjacent_exception` |
| both epochs match the coordinator's view | `shard_epoch_mismatch_exception` |
| replica sets colocated on the same node set | `shard_alignment_required_exception` |
| neither in joint consensus | `shard_busy_exception` |
| neither splitting/merging/frozen | `shard_busy_exception` |
| neither split within `split_merge_interval` | cooldown rejection |

Colocation is the demanding one. Each target replica absorbs state from the
source replica *on its own machine*; a target replica with no local source peer
cannot apply `merge_commit`. When alignment is missing the merge fails fast and
the PD is asked to align by add/remove replica operators — never by shipping
state across the network mid-merge.

```
 SOURCE group                                      TARGET group
 ────────────                                      ────────────
 (1) propose merge_prepare
     { src desc+epoch, tgt desc+epoch,
       min_index = min match_index over src voters }
     ── committed & applied on all src replicas ──▶
     state := merging_source
     rejects new proposals + reads with shard_merging_exception
     rejects further conf changes
                                                   (2) source leader notifies target leader
                                                       target validates preconditions
                                                       state := merging_target
                                                       propose merge_commit
                                                       { src desc,
                                                         entries (min_index, prepare_index] }
                                                   ── committed; applied on all tgt replicas:
                                                        a. force-append + force-apply the
                                                           carried entries to the LOCAL source
                                                           replica → it stands exactly at
                                                           prepare_index
                                                        b. tgt_sm.absorb(src_sm.get_state(),
                                                                          src_range)
                                                        c. extend tgt range over src range
                                                        d. version = max(src,tgt).version + 1
                                                        e. destroy + tombstone local src replica
                                                        f. publish shard_map; check_tiling()
                                                   (3) pd.report_merge(src, tgt)
```

`min_index` bounds what travels inside `merge_commit`: every source voter is
known to hold everything up to `min_index`, so only the tail
`(min_index, prepare_index]` must be carried. Without it, `merge_commit` would
have to carry the source's whole log or its whole state.

Step (2)(a) is subtle and worth naming: the target replica does not *ask* its
local source replica to catch up and wait — it force-feeds it the entries that
came inside `merge_commit`, synchronously, inside the apply. That is what makes
the merge deterministic across target replicas: every one of them applies the
same source tail before reading `get_state()`.

### 5.6 Merge: `absorb` and the round-trip law

```cpp
{ sm.absorb(other_state, other_range) } -> std::same_as<void>;
```

`absorb` must be deterministic and must be the exact inverse of `split_state`:
splitting at `k` and then absorbing the right part back must restore the
original state byte-for-byte under `get_state()`. This is stated as a law in
the concept's documentation and checked directly by a property test (§10), not
left as an implied convention — a state machine that violates it produces
replicas that silently diverge, which no Raft-level invariant will catch.

### 5.7 Rollback, and the deviation from TiKV

Once `merge_prepare` is applied the source is frozen. If the merge cannot
proceed — the target split, the target's epoch moved, the PD cancelled, the
alignment broke — the source must be released, and releasing it wrongly is the
one way this protocol can corrupt data: a source that resumes serving while
some target replica has already applied `merge_commit` means two shards own the
same range.

TiKV resolves this with a timing argument. This design does not, because
Kythira has no clock-synchronisation guarantee anywhere in the codebase today
and introducing an unstated one here would be a silent correctness dependency.

**Default: explicit handshake.**

```
source leader ── abandon_request(src desc, src epoch) ──▶ target leader
                                                           if merge_commit already proposed:
                                                               refuse  (commit always wins)
                                                           else:
                                                               propose merge_abandoned in the
                                                               TARGET's log
                                                           ◀── committed record
source leader observes the committed merge_abandoned record,
then proposes merge_rollback in the SOURCE's log
    → state := stable, source resumes serving
```

The decision to abandon is thereby a *committed fact in the target's log*, so a
target leader failover cannot lose it: the new target leader replays
`merge_abandoned` and will refuse to propose `merge_commit`. Symmetrically,
once `merge_commit` is proposed the target refuses to abandon. Commit and
abandon are mutually exclusive because both are decided by the same single
log — the target's.

**Escape hatch: `merge_lease_mode`.** A configuration option enabling the
timing-based variant (source rolls back unilaterally after a deadline carried
in `merge_prepare`). It is off by default and its documentation states the
bounded-clock-skew assumption in the first line, because an operator who turns
it on is taking on an assumption the rest of Kythira does not make.

A stuck merge with the handshake unavailable (target leader unreachable) leaves
the source frozen — unavailable but *correct*. That is the right trade, and it
is surfaced as a `shard_merge_stalled` metric plus a warning log naming the
target, so an operator can act rather than guess.

---

## 6. Signals — how users say *when* to split and merge

Four independent channels feed one arbiter. Every channel produces a
**proposal**; only the arbiter enacts. No channel can touch Raft state.

```
  (a) split_merge_policy        declarative, per-leader, per policy interval
  (b) admin API                 imperative, operator-driven, right now
  (c) state machine hints       data-derived: where to cut, and where NOT to
  (d) placement driver          cluster-scope, advisory operators
                    │
                    ▼
        ┌────────────────────────────┐
        │  split_merge_arbiter       │
        │  precedence  b ▸ d ▸ a     │
        │  (c) never initiates       │
        │  gates:                    │
        │    per-shard op state      │
        │    split_merge_interval    │
        │    max_concurrent_split_merge
        │    frozen / global kill    │
        └────────────────────────────┘
                    │ enact
                    ▼
             §5 split / merge
```

### 6.1 Channel (a) — the declarative policy

```cpp
template<typename P, typename GroupId, shard_key Key>
concept split_merge_policy =
    requires(P& p,
             const shard_stats<GroupId, Key>& self,
             const shard_stats<GroupId, Key>& sibling) {
        { p.evaluate_split(self) }           -> std::same_as<split_decision<Key>>;
        { p.evaluate_merge(self, sibling) }  -> std::same_as<merge_decision>;
        { p.cooldown() }                     -> std::same_as<std::chrono::milliseconds>;
        { p.validate() }                     -> std::same_as<bool>;
        { p.get_validation_errors() }        -> std::same_as<std::vector<std::string>>;
    };
```

Three properties, each of which is a design decision worth defending:

1. **Leader-only, and *not* required to be deterministic.** The policy runs on
   one leader; its answer is frozen into the split entry (§5.2) and every
   replica applies the frozen answer. So a policy may read a wall clock, sample
   randomly, consult a cache, or change its mind between calls — none of it can
   diverge replicas. The concept's doc comment says this explicitly, because
   the opposite assumption ("policies must be deterministic") is both a natural
   guess and a needless constraint.
2. **No I/O, no mutation.** It receives a value and returns a value. A policy
   that wants external input gets it via the placement driver channel, which is
   asynchronous and already has a failure story.
3. **Vector of split keys.** Batch split, straight from TiKV RFC 0006's
   motivation: "Current split only splits one Region at a time. It may be very
   slow when a sequential write is too fast, namely, the split speed cannot
   keep up with write speed." A one-key-at-a-time API cannot express the fix.

```cpp
enum class split_reason : std::uint8_t {
    size, key_count, read_load, write_load, admin, placement_driver, pre_split,
};

template<shard_key Key>
struct split_decision {
    bool _split{false};
    std::vector<Key> _at_keys;      // empty + _split ⇒ "you choose" → channel (c)
    split_reason _reason{split_reason::size};
};

enum class merge_direction : std::uint8_t { into_left_sibling, into_right_sibling };

struct merge_decision {
    bool _merge{false};
    merge_direction _direction{merge_direction::into_left_sibling};
    merge_reason _reason{merge_reason::size};
};
```

`merge_direction` is not cosmetic: the survivor's replicas do the absorbing, so
the direction determines which group's state machine runs `absorb` and which
group is destroyed. A `bool merge` alone would leave that to the host and make
"merge this shard into its left neighbour, not its right one" inexpressible.

### 6.1.1 `shard_stats` — what a policy gets to see

```cpp
template<typename GroupId, shard_key Key>
struct shard_stats {
    shard_descriptor<GroupId, Key, node_id_type> _descriptor;

    // size — from the splittable_state_machine extension (§6.4)
    std::size_t _approximate_size_bytes{0};
    std::size_t _approximate_key_count{0};
    bool        _size_available{false};   // false ⇒ SM has no sizing hooks

    // log / apply
    std::size_t      _log_size_bytes{0};
    log_index_type   _last_applied_index{0};
    double           _applied_entries_per_sec{0.0};
    std::chrono::nanoseconds _p99_apply_latency{};

    // load — measured at the routing layer, no SM support needed
    double _read_qps{0.0};
    double _write_qps{0.0};
    double _read_bytes_per_sec{0.0};
    double _write_bytes_per_sec{0.0};

    // history — the anti-oscillation inputs
    std::chrono::milliseconds _time_since_last_split{};
    std::chrono::milliseconds _time_since_last_merge{};
    std::chrono::milliseconds _leader_since{};

    // membership
    std::size_t _voter_count{0};
    std::size_t _learner_count{0};
    std::size_t _down_replica_count{0};

    // load-split sampler output (§6.3); empty when sampling is off/inconclusive
    std::vector<hot_key_sample<Key>> _hot_key_samples;
};
```

`_size_available` is deliberate. A state machine without the sizing hooks makes
size-based split impossible, and the host says so **once at construction**
(Requirement 9.6) rather than leaving an operator to wonder for a week why
nothing ever splits. Silent no-ops are the worst failure mode a policy layer
can have.

Load figures are measured by `multi_raft` at the routing layer — it already
sees every `submit_command` and every `read_state` — so load-based split works
for *any* state machine, including one with no sizing hooks at all.

### 6.1.2 The default policy and the oscillation guard

```cpp
struct threshold_split_merge_policy_config {
    std::size_t _shard_max_size_bytes      {144ull * 1024 * 1024};
    std::size_t _shard_split_size_bytes    { 96ull * 1024 * 1024};
    std::size_t _shard_max_keys            {1'440'000};
    std::size_t _shard_split_keys          {  960'000};

    std::size_t _shard_merge_max_size_bytes{ 20ull * 1024 * 1024};
    std::size_t _shard_merge_max_keys      {  200'000};

    std::chrono::milliseconds _split_merge_interval{std::chrono::hours{1}};
    std::size_t _batch_split_limit{10};

    // load split (§6.3)
    bool   _load_split_enabled{false};
    double _load_split_qps_threshold{3000.0};
    double _load_split_bytes_threshold{30ull * 1024 * 1024};
    std::chrono::milliseconds _load_split_duration{std::chrono::seconds{10}};
    std::size_t _load_split_sample_keys{20};
    double _load_split_one_sided_fraction{0.99};
    std::chrono::milliseconds _load_split_backoff{std::chrono::minutes{10}};
};
```

Split key generation follows TiKV RFC 0006's `SizeChecker`: walk the shard
accumulating size, record a split key every `_shard_split_size_bytes`, stop at
`_shard_split_size_bytes * (_batch_split_limit - 1) + _shard_max_size_bytes`,
and discard the trailing key if the remainder is not larger than
`_shard_max_size_bytes - _shard_split_size_bytes`. With
`_batch_split_limit == 1` this degenerates exactly to single-key split, which
makes the batch path testable against the simple path.

**`validate()` rejects oscillating configurations.** The rule:

```
2 * _shard_merge_max_size_bytes  <  _shard_split_size_bytes      (required)
2 * _shard_merge_max_keys        <  _shard_split_keys            (required)
```

The failure it prevents, spelled out in the error message: two adjacent shards
each just under the merge threshold merge into one shard just over the split
threshold, which splits, producing two shards just under the merge threshold,
forever. TiKV's RFC 0045 states the same rule of thumb from the other side —
"to avoid back-and-forth splitting and merging, the merging load threshold
should be slightly lower than splitting load threshold (e.g., 20% lower)". The
defaults above sit at 20 MiB versus 96 MiB, comfortably inside the bound.

`validate()`/`get_validation_errors()` mirror `raft_configuration`'s existing
shape (types.hpp:594), so this is a familiar object, not a new idiom.

**The interval is enforced by the host, not the policy** (Requirement 7.6). A
custom policy that forgets the cooldown still cannot oscillate, because the
arbiter refuses a merge on a shard whose `_time_since_last_split <
split_merge_interval`. Defence in depth on the one knob whose misconfiguration
is unbounded.

### 6.2 Channel (b) — the imperative admin API

Direct operator control, highest precedence, always available even on a frozen
shard.

```cpp
struct split_options {
    bool _wait_for_apply{true};        // resolve on apply, not merely on commit
    bool _scatter_children{false};     // ask the PD to spread the new leaders
    bool _override_cooldown{false};    // skip split_merge_interval — operator's call
    bool _allow_state_machine_veto{true};  // false ⇒ hard-fail instead of falling back
};

struct merge_options {
    bool _wait_for_apply{true};
    bool _auto_align{false};           // let the PD colocate replicas first, then retry
    std::chrono::milliseconds _align_timeout{std::chrono::minutes{5}};
};
```

Design notes:

- **`_wait_for_apply` defaults to true.** Resolving on commit would return
  success before the children exist, and an operator scripting a pre-split
  followed by a bulk load would race their own split. Commit is not the
  interesting event here; apply is.
- **`_override_cooldown` exists and is off.** The cooldown is a guard against
  *automatic* oscillation. An operator who has diagnosed a hot shard and wants
  it split now should not have to wait an hour or edit config, but they should
  have to say so.
- **`_allow_state_machine_veto` is on by default.** An operator naming a key
  the state machine forbids gets a nearby valid key and a log line, not a
  failure — unless they explicitly ask for the strict behaviour.
- **`_auto_align` is off by default.** Colocating replicas moves data. An
  operator asking for a merge should not silently trigger replica movement
  across the cluster; they should be told alignment is needed
  (`shard_alignment_required_exception`) and opt in.
- **`pre_split(boundaries)`** requires every affected shard to be empty. It
  exists for the bulk-load case TiKV RFC 0082 names — "in the very beginning,
  writes will only happen in a single region, the problem can be solved by
  pre-split + scatter" — and refusing it on non-empty shards keeps it from
  becoming a second, weaker `split_shard`.
- **`freeze_shard`** removes a shard from channels (a), (c), and (d) but not
  (b). Freezing an operator out of their own escape hatch would be a bad joke
  at 3 a.m.

### 6.3 Channel (c′) — the load-based split sampler

Implements TiKV RFC 0045, whose reasoning transfers exactly: a shard can be
small and still be the bottleneck, and the fix is to split it so its children's
leaderships can be scattered.

```
if read_qps  < threshold and write_qps < threshold: sampler idle, cost = one branch
enter sampling:
  pick up to _load_split_sample_keys candidate keys by probability sampling
    of live requests
  for _load_split_duration:
     for each candidate k: count accesses strictly left of k, and at-or-right of k
     if load drops below threshold  → abandon, no proposal
        (RFC 0045: "splitting is meaningless for momentary and short loads (<10s)")
  if for every candidate, one side holds > _load_split_one_sided_fraction of
     accesses → the load is one hot key; a split cannot help.
     Mark the shard ineligible for _load_split_backoff and stop.
  else → emit the most balanced candidate as a hot_key_sample; the default
     policy proposes a split there with reason read_load / write_load, and
     sets split_options::_scatter_children
```

Two of these branches are the interesting ones:

- **Abandoning on a short spike.** A split costs an election per child and a PD
  scatter; paying that for a five-second burst is a net loss. The window is a
  knob because the right value depends on how expensive elections are in a
  given deployment.
- **The single-hot-key detection.** Without it, a workload hammering one key
  makes the sampler propose a split, the split does not help, the shard is
  still hot, and it proposes again — a split storm that shrinks shards toward
  one key each. The back-off marks the shard and moves on.

`_scatter_children` is not optional for load splits (Requirement 8.6): a load
split whose children's leaders both land on the machine that was already hot
has accomplished exactly nothing.

### 6.4 Channel (c) — state-machine hints and the veto

```cpp
template<typename SM, shard_key Key>
concept splittable_state_machine = requires(SM& sm, const SM& csm,
                                            const Key& k,
                                            const std::vector<Key>& keys,
                                            const std::vector<std::byte>& blob,
                                            const shard_range<Key>& r) {
    { csm.approximate_size_bytes() } -> std::same_as<std::size_t>;
    { csm.approximate_key_count()  } -> std::same_as<std::size_t>;

    /// Leader-only, advisory: up to `max` good places to cut.
    { sm.suggest_split_keys(std::size_t{}) } -> std::same_as<std::vector<Key>>;

    /// Leader-only, authoritative: the application's veto.
    { csm.can_split_at(k) } -> std::same_as<bool>;

    /// Every replica, at apply time. MUST be deterministic.
    { sm.split_state(keys) } -> std::same_as<std::vector<std::vector<std::byte>>>;

    /// Every replica, at apply time. MUST be deterministic and MUST be the
    /// exact inverse of split_state (see the round-trip law, §5.6).
    { sm.absorb(blob, r) } -> std::same_as<void>;
};
```

Detected structurally with `if constexpr`, so `test_key_value_state_machine`,
`ca_state_machine`, and the four example state machines compile and run
unchanged.

The split of responsibilities is the point:

| Question | Answered by | When |
|---|---|---|
| *Should* this shard split? | policy / PD / admin | leader, policy tick |
| *Where* would be a good cut? | `suggest_split_keys` | leader, before proposing |
| Is this cut *forbidden*? | `can_split_at` | leader, before proposing |
| *Do* the cut | `split_state` | every replica, at apply |
| *Undo* the cut | `absorb` | every replica, at apply |

`can_split_at` is the feature an application actually reaches for. Concrete
uses: refusing to cut between a row and its secondary-index entries; refusing
to cut inside a key group an application-level transaction spans; keeping a
tenant's keys in one shard so its operations stay single-shard. TiKV has the
same idea in a narrower form (splitting on table boundaries); here it is a
predicate the application owns.

The fallback chain, in order, is: requested keys minus vetoed keys → if empty,
`suggest_split_keys` minus vetoed → if still empty,
`no_valid_split_key_exception`, rate-limited log, cooldown. A state machine
that vetoes everything gets a visible complaint, not silence.

### 6.5 Channel (d) — placement-driver operators

See §7. The property that belongs here is TiKV's, and it is adopted verbatim:
**operators are advisory**. TiKV's own docs are explicit — "operators are only
suggestions to the Region leader, which can be skipped by Regions" based on
current status. A leader that receives `split at K` while it is merging, or
frozen, or over the concurrency limit, drops the operator and logs
`skipped_operator`; the PD notices from the next heartbeat and reissues.

Each operator carries `operation_id` and the epoch it was computed against; an
operator whose epoch is stale is discarded on receipt (Requirement 14.6). This
is what keeps a PD decision computed from a 30-second-old heartbeat from
undoing a split that happened 5 seconds ago.

### 6.6 The arbiter: per-shard state, precedence, gates

Conflicting operations are made impossible **by construction**, not by
check-then-act. Each shard has one operation state, and starting an operation
*is* a transition:

```
                   ┌──────────────────────────────────────┐
                   │                                      │
        split ok   ▼                     split applied    │
   ┌───────────▶ splitting ──────────────────────────────▶│
   │                                                      │
   │  prepare ok                     rollback applied     │
stable ────────▶ merging_source ─────────────────────────▶│
   │                  │  commit applied                   │
   │                  └──────────────▶ tombstoned         │
   │  targeted                                            │
   ├───────────▶ merging_target ─────────────────────────▶│
   │                                     commit applied   │
   │  freeze_shard()                                      │
   └───────────▶ frozen ─────────────────────────────────▶┘
                        thaw_shard()
```

Only `stable` admits a new operation. `frozen` admits nothing automatic but
still admits explicit admin commands (which transition it directly to
`splitting`/`merging_*` and back to `frozen` on completion).

**Precedence**, when more than one channel speaks in the same interval:

1. explicit admin command — and it *suspends* automatic channels for the
   affected shards until it resolves;
2. placement-driver operator;
3. local policy.

Channel (c) never initiates; it only chooses and vetoes keys for an operation
one of the three above started.

A loser is logged with reason `preempted_by={channel}` and counted — never
silently dropped (Requirement 15.6). An operator debugging "why didn't my
policy fire" needs to see that the PD outranked it.

**Gates applied to every channel:**

| Gate | Default | Rejection reason string |
|---|---|---|
| shard state != `stable` (or `frozen` + admin) | — | `state` |
| `split_merge_interval` since last split | 1 h | `cooldown` |
| `max_concurrent_split_merge` cluster-wide | 4 | `concurrency_limit` |
| global kill switch | enabled | `globally_disabled` |
| epoch changed since the decision was computed | — | `epoch` |
| state-machine veto exhausted | — | `no_valid_split_key` |
| merge alignment missing | — | `alignment_required` |

`max_concurrent_split_merge` is enforced **before proposing**, never by
aborting something already committed — the same discipline as TiKV's
schedule-limit design, and for the same reason: a rebalancing storm that
saturates the network is worse than a slow rebalance.

### 6.7 Observability of signals

Every decision, accepted or rejected, emits both a log line and a metric with
the reason as a dimension:

```
kythira.multiraft.split.proposed{group, reason, channel}
kythira.multiraft.split.rejected{group, gate}
kythira.multiraft.split.applied{group}          + duration
kythira.multiraft.merge.proposed{group, reason, channel}
kythira.multiraft.merge.rejected{group, gate}
kythira.multiraft.merge.applied{group}          + duration
kythira.multiraft.merge.rolled_back{group, reason}
kythira.multiraft.merge.stalled{group, target}
kythira.multiraft.operator.skipped{group, operator_type}
kythira.multiraft.shards.by_state{state}
kythira.multiraft.epoch_mismatch{group, direction}
```

Thresholds are untunable without this. An operator who cannot see
`split.rejected{gate=cooldown}` will conclude the feature is broken.

---

## 7. Placement driver

The shard-aware extension of the existing `quorum_manager`, which already
models a cluster-scope authority (`topology()`, `provision_node()`,
`assess_quorum()`).

```cpp
template<typename D, typename GroupId, shard_key Key, typename NodeId>
concept shard_placement_driver = requires(D& d, std::size_t n,
                                          const shard_report<GroupId, Key, NodeId>& sr,
                                          const node_report<NodeId>& nr,
                                          const shard_descriptor<GroupId, Key, NodeId>& desc,
                                          const std::vector<shard_descriptor<GroupId, Key, NodeId>>& descs) {
    { d.allocate_shard_ids(n) }        -> kythira::future<std::vector<shard_id_allocation<GroupId, NodeId>>>;
    { d.report_shard_heartbeat(sr) }   -> kythira::future<std::vector<shard_operation<GroupId, Key, NodeId>>>;
    { d.report_node_heartbeat(nr) }    -> kythira::future<void>;
    { d.report_split(desc, descs) }    -> kythira::future<void>;
    { d.report_merge(desc, desc) }     -> kythira::future<void>;
};
```

`shard_report` and `node_report` mirror TiKV's region and store heartbeats
field for field where Kythira can measure the field:

- **shard**: group id, epoch, range, leader, all replicas, down-replica count,
  approximate size, approximate key count, read/write throughput.
- **node**: total and available capacity, local shard count, local leader
  count, read/write rates, snapshot send/receive counts, overload flag,
  placement-group labels (reusing the existing `placement_group_id` from
  `quorum_management.hpp`).

`shard_operation` is the variant:

```cpp
add_replica{node} | remove_replica{node} | transfer_leader{node} |
split{at_keys} | merge{into_group} | scatter{}
```

The first three map onto existing machinery: `add_server`, `remove_server`, and
a new leader-transfer path (Raft's TimeoutNow / leadership transfer, which
Kythira does not yet have and which is scoped as its own task in §11).

`no_op_shard_placement_driver` ships as the default, in the exact shape of
`no_op_quorum_manager`: it allocates ids from a locally configured reserved
range (so static, pre-split deployments work with no control plane at all) and
returns no operators. Requirement 14.7.

Heartbeats are batched — one call carrying every local shard's report per
interval, not one call per shard. At 1000 shards and a 10-second interval the
difference is 100 RPS versus 0.1 RPS.

---

## 8. Client routing

```
submit_command(key, cmd, timeout):
  attempt = 0
  loop:
    desc = shard_map.lookup(key)            or refresh from PD if absent
    target = desc.leader_hint or any voter
    try  submit to target with (desc.group_id, desc.epoch)
    on   not_leader(hint)        → target = hint;                retry
    on   epoch_mismatch(descs)   → shard_map.merge(descs);       retry
    on   shard_merging           → backoff;                      retry
    on   timeout                 → backoff;                      retry
    until attempt == max_route_retries (default 5)
```

The not-leader path takes MicroRaft's contract directly — "when clients contact
non-leaders, responses include the leader's endpoint, enabling client-side
routing" — so a leader change costs one extra hop, not a control-plane round
trip. The epoch-mismatch path is TiKV's `EpochNotMatch`, with the refinement
that the rejection carries the *current descriptors for the range that was
targeted*, so the client repairs its map from the rejection itself rather than
going to the PD.

Cross-shard commands are rejected, not silently mis-applied
(Requirement 16.6): if the partitioner yields a key outside the resolved
shard's range at apply admission, the command fails with
`cross_shard_command_exception`. There is no distributed transaction here and
pretending otherwise would be the worst possible failure mode.

---

## 9. Failure modes and what each one costs

| Failure | Behaviour | Cost |
|---|---|---|
| Crash mid-split, after children written, before parent index advanced | replay of the split entry is a no-op (idempotence check, §5.4 step B) | none |
| Crash mid-split, before children written | split entry replays from the start | none |
| Node missed a split entirely (log compacted past it) | lazy replica creation (§Requirement 12) on first message; `InstallSnapshot` populates it | one snapshot transfer |
| Stale `AppendEntries` for a merged-away group | tombstone set drops it | none |
| Merge target leader unreachable after `merge_prepare` | source stays frozen; `merge.stalled` metric + warning | source unavailable until resolved — correct but visible |
| Merge target splits mid-merge | abandon handshake; source rolls back | one wasted round trip |
| PD unreachable at split time | split abandoned, retried next policy tick | delayed split |
| PD unreachable persistently, no `no_op` driver | no splits at all; `split.rejected{gate=pd_unavailable}` fires every tick | visible, not silent |
| Policy misconfigured to oscillate | `validate()` refuses it at construction | none |
| Custom policy oscillates anyway | host-level `split_merge_interval` gate caps it at one operation per interval | bounded |
| State machine's `absorb` is not the inverse of `split_state` | replicas diverge silently at the Raft level | **caught only by the §10 round-trip property test** — this is the sharpest edge in the design and is called out in the concept documentation |

---

## 10. Testing

Built on the existing `network_simulator`, the `fiu_do_on` fault-injection
machinery, and the existing property-test style
(`tests/raft_leader_completeness_property_test.cpp` and siblings).

**Invariant properties**, asserted after every operation in a randomised
workload of splits, merges, membership changes, and client commands:

- **I1 Tiling** — `shard_map::check_tiling()` returns `nullopt` on every node.
  The single most important assertion in the suite.
- **I2 No loss, no duplication** — a shadow model of `key → value` is
  reconciled against the union of every shard's `get_state()`.
- **I3 Epoch monotonicity** — per group, `version` and `conf_version` never
  decrease; a split's children exceed their parent; a merge's survivor exceeds
  both inputs.
- **I4 Stale rejection** — a request replayed with a captured old epoch is
  always rejected, never served.
- **I5 Round-trip law** — for a random state and a random valid key `k`:
  `split_state({k})` then `absorb(right, right_range)` reproduces the original
  `get_state()` byte-for-byte (§5.6).

**Crash consistency** — new fault points under `raft/multiraft/`:

```
raft/multiraft/split/before_children
raft/multiraft/split/between_children
raft/multiraft/split/after_children_before_parent
raft/multiraft/split/after_publish
raft/multiraft/merge/after_prepare
raft/multiraft/merge/mid_commit_catchup
raft/multiraft/merge/after_absorb_before_destroy
raft/multiraft/merge/after_abandon_before_rollback
```

Each is exercised with a crash-and-recover, then I1–I4 are asserted.

**Oscillation** — drive `threshold_split_merge_policy` for 10 000 simulated
ticks with shard sizes deliberately parked between the merge and split
thresholds; assert total split count and total merge count both stay under a
small bound. Then run the same test with `validate()`-rejected knobs forced in
and assert the host-level cooldown still bounds it.

**Scale** — 1000 groups across three simulated nodes; assert `tick()` duration
tracks *ready* group count rather than total, and that hibernating count
converges to near 1000 under an idle workload.

**Container rules** — the docker-chaos scenarios use `container_runtime()` /
`compose_prefix()` from `tests/docker_chaos/os_faults.hpp` per the project's
Docker/rootless-Podman rule; no static IPs; and no piping multi-process test
output through `tail`/`head`.

---

## 11. Phasing

The phases are chosen so each one lands something usable and testable.

| Phase | Content | Usable outcome |
|---|---|---|
| 1 | `shard_types`, `shard_map`, exceptions, `group_id` on the wire + all five serializers | routing types exist; wire is ready |
| 2 | `multi_group_network_server`, `group_scoped_*`, group-scoped storage, tombstones | many groups in one process, static shard map, no split/merge |
| 3 | `multi_raft` registry, striped executor, batched `tick()`, hibernation, client routing | a working static multi-Raft cluster |
| 4 | admin entries in `raft.hpp`, split protocol, lazy replica creation | `split_shard()` works; channel (b) live |
| 5 | merge protocol, abandon handshake, alignment checks | `merge_shards()` works |
| 6 | `split_merge_policy`, `threshold_*` default, arbiter, gates, metrics | channel (a) live — automatic split/merge |
| 7 | `shard_placement_driver`, heartbeats, operators, leader transfer, scatter | channel (d) live |
| 8 | load-split sampler | channel (c′) live |
| 9 | transport message batching, `buckets`-style sub-shard stats | scale refinements |

Phases 1–3 are strictly additive and cannot regress single-group behaviour.
Phase 4 is the first one that touches `raft.hpp`, and its change there is two
methods and one enum.

---

## 12. Open questions

1. **Leader transfer.** Kythira has no Raft leadership-transfer (TimeoutNow)
   path. The PD's `transfer_leader` operator and the load-split scatter both
   need it. It is scoped inside Phase 7 here, but it is genuinely a separate
   feature and might deserve its own spec.
2. **Read-index across a split.** A read-index issued against the parent that
   returns after the split applied must be rejected with epoch mismatch. The
   admission-time and apply-time epoch checks (Requirement 3.7) cover it, but
   the interaction with the existing `commit_waiter` path needs a close read
   during Phase 4.
3. **`min_index` when a source voter is down.** `min match_index` over voters
   includes a down voter's stale index, making the carried entry tail large. An
   alternative is min over *live* voters plus a rule that a source replica
   behind `min_index` must take a snapshot instead — deferred to Phase 5's
   detailed design.
4. **Buckets.** TiKV RFC 0082's sub-shard statistics unit would sharpen the
   load signal considerably. `shard_stats` is shaped so it can be added as a
   field without breaking the policy concept, and it is deliberately out of
   scope for this pass.
5. **Shard map durability format.** Storing it per-group (each group persists
   its own row) is simplest and reconstructs the map on restart, but recovering
   rows for groups this node does *not* host requires a PD query. An operator
   running `no_op_shard_placement_driver` would need a static map file. Decide
   in Phase 3.
