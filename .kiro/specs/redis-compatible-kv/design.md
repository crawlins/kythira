# Design Document — Redis-Compatible KV on Multi-Raft (sccache Backend)

## Overview

This design adds a **protocol front-end and a state machine**, and nothing
else. It does not modify `raft.hpp`, `multi_raft.hpp`, or any transport. The
whole feature is:

1. `redis_kv_state_machine` — an ordinary Kythira `state_machine` that also
   satisfies `splittable_state_machine`, holding one shard's keys and values.
2. `redis_gateway<Types>` — a Boost.Asio RESP listener that authenticates,
   authorizes, routes and answers, using `multi_raft`'s existing client
   surface.
3. A small amount of glue: a binary command codec, an ACL, a forwarding
   client, and a daemon in `cmd/`.

The scope-control principle is stated once and then enforced everywhere below:
**the supported command set is the closure of what sccache's client stack puts
on the wire**, which was determined by reading that stack, not the Redis
manual.

### Where the evidence comes from

| Layer | What was read | What it settles |
|---|---|---|
| sccache | `src/cache/redis.rs`, `src/cache/cache.rs`, `src/cache/utils.rs`, `src/server.rs`, `docs/Redis.md` | Which backend (OpenDAL `services::Redis`), the config surface, the `.sccache_check` probe, the `a/b/c/<digest>` key layout, and that a write failure is counted and survived |
| OpenDAL | `core/services/redis/src/{core,reader,writer,deleter,config}.rs`, `core/core/src/raw/path.rs` | The exact command per operation: `GET`, `EXISTS`+`STRLEN`, `GETRANGE`, `SET`/`SETEX`, `DEL`, plus `PING` on pool recycle and the 10-connection default pool |
| redis-rs 1.2 | `src/connection.rs` (`connection_setup_pipeline`, `check_connection_setup`), `src/commands/mod.rs`, `src/types.rs` | The handshake is a **pipeline**; RESP2 is the default; `set_ex` is `SETEX`; `CLIENT SETINFO` replies are ignored but must still be *emitted* |

Two consequences of that last row drive the protocol layer and are easy to get
wrong: the server must handle **pipelined** input correctly from the very first
byte of a connection, and it must emit exactly one reply per command even for
commands it does not implement, or the client's reply-index accounting
desynchronizes.

---

## Architecture

```
   sccache (per developer / per CI runner)
     │  OpenDAL services::Redis  →  redis-rs 1.2 (pool of ≤10 connections)
     │  RESP2 over TCP or TLS
     ▼
┌──────────────────────────────────────────────────────────────────────┐
│ redis_gateway<Types>                     (one per Kythira node)      │
│                                                                      │
│  ┌────────────┐   ┌──────────────┐   ┌────────────────────────────┐  │
│  │ acceptor   │──▶│ session      │──▶│ resp_parser / resp_writer  │  │
│  │ (asio,     │   │ per conn:    │   │ RESP2 default, RESP3 on    │  │
│  │  TLS opt.) │   │ identity,    │   │ HELLO 3; pipelining        │  │
│  └────────────┘   │ proto, db    │   └────────────────────────────┘  │
│                   └──────┬───────┘                                   │
│                          ▼                                           │
│                   ┌──────────────┐   ┌────────────────────────────┐  │
│                   │ acl          │   │ command dispatch table     │  │
│                   │ authn: AUTH/ │──▶│ GET SET SETEX DEL EXISTS   │  │
│                   │  HELLO/mTLS  │   │ STRLEN GETRANGE PING …     │  │
│                   │ authz: role  │   └─────────┬──────────────────┘  │
│                   │  + prefixes  │             │                     │
│                   └──────────────┘             ▼                     │
│                                    ┌────────────────────────────┐    │
│                                    │ router                     │    │
│                                    │  resolve(key) → descriptor │    │
│                                    └───┬───────────────┬────────┘    │
│                                        │ local         │ remote      │
└────────────────────────────────────────┼───────────────┼─────────────┘
                                         ▼               ▼
                         ┌───────────────────────┐  ┌──────────────────┐
                         │ multi_raft<Types,      │  │ forward_client   │
                         │   std::string, u64>    │  │ one hop, pooled, │
                         │  submit_command(key,…) │  │ internal identity│
                         │  group_node(g)         │  └────────┬─────────┘
                         │   → with_state_machine │           │
                         └───────────┬────────────┘           ▼
                                     ▼                 peer gateway
                         ┌────────────────────────┐
                         │ redis_kv_state_machine │  (one per shard)
                         │  std::map<string,entry>│
                         │  apply / get_state /   │
                         │  split_state / absorb  │
                         └────────────────────────┘
```

### What changes in existing files

Nothing, in the consensus core. The design uses only public API that already
exists:

- `multi_raft::submit_command(key, command, timeout)` for writes.
- `multi_raft::resolve(key)` for routing.
- `multi_raft::group_node(group)` → `node::with_state_machine(f)` for reads.
- `shard_not_leader_exception::leader_hint()` for forwarding.
- `splittable_state_machine`'s hooks, already discovered structurally by the
  host with `if constexpr`.

New files only:

```
include/raft/resp_protocol.hpp            RESP2/RESP3 codec (no I/O)
include/raft/redis_kv_commands.hpp        binary log-entry codec
include/raft/redis_kv_state_machine.hpp   the replicated data
include/raft/redis_acl.hpp                users, secrets, roles, scopes
include/raft/redis_gateway.hpp            declaration
include/raft/redis_gateway_impl.hpp       definition (this tree's convention)
cmd/redis_gateway_node/{main.cpp,config.hpp,CMakeLists.txt}
tests/resp_protocol_unit_test.cpp
tests/redis_kv_state_machine_unit_test.cpp
tests/redis_acl_unit_test.cpp
tests/redis_gateway_integration_test.cpp
tests/docker_chaos/sccache_e2e/…          real-sccache acceptance test
```

---

## Component 1 — `resp_protocol.hpp`: the wire codec

Pure value code: parses bytes into a command, serializes a reply into bytes. No
sockets, no allocation policy beyond `std::vector`, so it is exhaustively
unit-testable without a network.

```cpp
enum class resp_protocol_version { resp2, resp3 };

// A parsed request: RESP arrays of bulk strings, which is the only request
// shape the client stack produces.
struct resp_command {
    std::vector<std::string> _argv;   // argv[0] is the command name
};

class resp_parser {
public:
    // Feeds bytes; returns as many complete commands as the buffer holds.
    // Incomplete trailing input stays buffered. This is the pipelining
    // contract: `redis-rs` writes AUTH + SELECT + CLIENT SETINFO ×2 in one
    // write and only then reads, so a parser that assumed one command per
    // read would hang the handshake.
    auto consume(std::span<const std::byte> bytes) -> std::vector<resp_command>;

    [[nodiscard]] auto buffered_bytes() const -> std::size_t;
};

class resp_writer {   // version-aware; only the null encoding differs here
public:
    auto simple_string(std::string_view) -> void;   // +OK
    auto error(std::string_view) -> void;           // -NOAUTH …
    auto integer(std::int64_t) -> void;             // :1
    auto bulk(std::span<const std::byte>) -> void;  // $n\r\n…
    auto null() -> void;                            // RESP2 $-1  / RESP3 _
    auto map(std::span<const std::pair<std::string, resp_value>>) -> void;
};
```

### Limits, enforced in the parser rather than above it

| Limit | Default | Why |
|---|---|---|
| `proto_max_bulk_len` | 32 MiB | Above the 8 MiB value ceiling so an oversize write is *rejected with a message* rather than killed as a protocol error |
| max multibulk elements | 64 | Every supported command has ≤ 4 arguments |
| max buffered unparsed bytes | 64 MiB | Bounds a slow-loris that opens a bulk header and stops |
| max pipelined in-flight | 128 | Backpressure by not reading, per Requirement 14.3 |

A violated limit produces `-ERR Protocol error: …` and closes the connection —
Redis's own behaviour, and the only safe one, since a desynchronized stream
cannot be resynchronized by continuing to read.

### Reply shapes for the supported commands

| Command | Hit | Miss / absent |
|---|---|---|
| `GET` | bulk string | RESP2 `$-1\r\n`, RESP3 `_\r\n` |
| `EXISTS` | `:1` | `:0` |
| `STRLEN` | `:<len>` | `:0` |
| `GETRANGE` | bulk string (possibly empty) | empty bulk `$0\r\n\r\n` |
| `SET` / `SETEX` | `+OK` | — |
| `DEL` | `:1` | `:0` |
| `PING` | `+PONG` (exactly; `redis-rs` compares the bytes) | — |
| `SELECT` | `+OK` for db 0, `-ERR DB index is out of range` otherwise | — |
| `CLIENT SETINFO` | `+OK` | — |
| `HELLO` | map (Requirement 2.3) | `-NOPROTO …` |

`SELECT` deserves a note: the gateway presents a **single logical database**.
`redis-rs` emits `SELECT` only when `SCCACHE_REDIS_DB` is non-zero, so
accepting db 0 and refusing the rest is both honest and sufficient; the refusal
is a clear startup-time misconfiguration message rather than a silent
shared-namespace collision.

---

## Component 2 — `redis_kv_commands.hpp`: what goes in the log

A versioned, length-prefixed binary encoding. Explicitly **not** JSON: the
default `json_serializer` already base64-encodes each entry's command bytes
into the AppendEntries document (`include/raft/json_serializer.hpp:152`), and
encoding the payload as JSON *inside* that would pay the expansion twice.

```
byte  0        format version (0x01)
byte  1        opcode
bytes 2..5     key length     (u32, big-endian)
bytes …        key
bytes …        opcode-specific payload
```

| Opcode | Name | Payload |
|---|---|---|
| `0x01` | `set` | `expire_at_ms` (u64 BE, 0 = never) + value length (u32 BE) + value |
| `0x02` | `del` | — |
| `0x03` | `sweep` | count (u32 BE) then `count` × { key length, key, `expire_at_ms` } |
| `0x04` | `evict` | count (u32 BE) then `count` × { key length, key } |

Big-endian throughout, matching `default_shard_key_codec`'s existing choice and
for the same stated reason: an entry written on one machine is decoded on
another, and native-endian encoding would make replicas disagree.

`sweep` carries the deadline each key was observed to hold. That field is what
makes expiry safe under concurrency — see Component 4.

---

## Component 3 — `redis_kv_state_machine`

```cpp
class redis_kv_state_machine {
public:
    // state_machine
    auto apply(const std::vector<std::byte>& command, std::uint64_t index)
        -> std::vector<std::byte>;
    [[nodiscard]] auto get_state() const -> std::vector<std::byte>;
    auto restore_from_snapshot(const std::vector<std::byte>&, std::uint64_t) -> void;

    // splittable_state_machine
    [[nodiscard]] auto approximate_size_bytes() const -> std::size_t;
    [[nodiscard]] auto approximate_key_count() const -> std::size_t;
    auto suggest_split_keys(std::size_t max) -> std::vector<std::string>;
    [[nodiscard]] auto can_split_at(const std::string&) const -> bool { return true; }
    auto split_state(const std::vector<std::string>& keys)
        -> std::vector<std::vector<std::byte>>;
    auto absorb(const std::vector<std::byte>& blob,
                const shard_range<std::string>& range) -> void;

    // Read path (see Component 5): O(1), lock-friendly.
    [[nodiscard]] auto lookup(const std::string& key) const
        -> std::shared_ptr<const value_entry>;

private:
    struct value_entry {
        std::vector<std::byte> _value;
        std::uint64_t _expire_at_ms;   // 0 = no expiry
    };
    // ORDERED, deliberately. split_state(), get_state() and the eviction scan
    // all iterate this; splittable_state_machine's first law makes that
    // iteration order part of the replicated contract.
    std::map<std::string, std::shared_ptr<const value_entry>> _entries;
    std::size_t _bytes{0};
};
```

Design points worth defending:

**`shared_ptr<const value_entry>` rather than a value.** The read path runs
inside `node::with_state_machine()`, which holds the node's `_mutex` and
forbids blocking (`include/raft/raft.hpp:460`). A lookup that copied a 4 MiB
value under that lock would stall the whole group's tick loop for the duration
of a memcpy. Returning a shared handle makes the in-lock cost a refcount
increment; the bytes are written to the socket after the lock is released. The
entries are immutable once constructed, so sharing them out is safe.

**`can_split_at` is unconditionally `true`, with a comment.** There is no
secondary index, no multi-key transaction, and no key group to keep together.
That is a property of this data model, not a shortcut — and a future feature
that adds any of the three must revisit this function specifically.

**`suggest_split_keys`** walks the ordered map accumulating bytes and returns
the keys at even byte-quantiles. Because sccache keys are hex digests with a
`a/b/c/` prefix layout, quantiles by bytes and quantiles by count converge, and
splits land evenly.

**`get_state()` is the shard-size ceiling.** It materializes every entry. That
is the reason the split policy's size threshold, not the per-value limit,
governs snapshot cost — recorded here because it is the non-obvious half of
Requirement 6.

**`apply()` reads no clock.** `expire_at_ms` arrives in the entry, already
absolute. This is the same defect class that
`.kiro/specs/state-machine-examples/` found in `distributed_lock_state_machine`
(`steady_clock::now()` inside `apply()`), and the reason this design states the
rule rather than assuming it.

---

## Component 4 — Expiry and eviction

Two maintenance operations, one shape: **the leader decides, the log records
the decision, every replica applies the recorded decision.** The heuristic may
be sloppy; the applied result cannot be.

### Expiry

- `SETEX key seconds value` → the gateway computes
  `expire_at_ms = now_ms() + seconds × 1000` **before proposing** and puts the
  absolute instant in the entry.
- A read compares the stored deadline with the reading node's clock and treats
  an expired entry as absent. This comparison never mutates state, so replicas
  with skewed clocks disagree only about *visibility*, for the width of the
  skew, and only in the direction of a miss.
- Reclamation runs on the leader's policy tick: scan up to `sweep_batch` (default
  1024) entries, collect those past their deadline, propose
  `sweep{(key, expire_at_ms), …}`.
- `apply(sweep)` deletes a named key **only if its currently-stored
  `expire_at_ms` equals the value named in the sweep.** Without that check, a
  key rewritten between proposal and commit would be destroyed by a sweep that
  was about the old entry. With it, the operation is deterministic, idempotent,
  and safe to replay.

### Eviction

- Budget per shard: `max_shard_bytes` and `max_shard_keys`.
- Victim selection is **leader-local and advisory**: an approximate LRU
  maintained only on the node serving reads, plus size and deadline as
  tie-breakers. It is never replicated, because a read served on a follower
  would otherwise mutate state that only some replicas observe.
- The leader proposes `evict{key, …}`; `apply` deletes exactly those keys.
- Over budget with eviction lagging is a **metric and a log line, not a write
  refusal**. Refusing writes turns a capacity problem into a simultaneous
  slowdown for every client; the workload's tolerance for misses makes
  over-budget-and-shrinking the safer state.
- A shard growing past its budget is usually a signal to **split**, not to
  evict. Eviction is the answer to the cluster being full.

---

## Component 5 — The read path

```
GET key
  ├─ acl.authorize(identity, "GET", key)          → -NOPERM on failure
  ├─ multi_raft.resolve(key)                      → descriptor or -ERR (3.4)
  ├─ mode == leader ? require local leader
  │  mode == any_replica ? any local replica
  ├─ node->with_state_machine([&](auto& sm){ return sm.lookup(key); })
  │      ↑ under the node mutex: refcount copy only
  ├─ release lock; check expire_at_ms; write bulk reply from the handle
  └─ no local replica → forward (Component 7) or -ERR retryable
```

`node::read_state()` is deliberately unused. It returns the entire state
machine's serialized state (`include/raft/raft.hpp:253`), which for a single-key
lookup would serialize the whole shard per request.

### The consistency argument, stated rather than implied

The default (`leader`) is not linearizable: a leader that has been deposed but
does not know it yet can serve a stale read. For this workload that is
acceptable, and the reason is a property of the data, not an appeal to
convenience:

> sccache values are immutable and content-derived. A stale read returns the
> correct value or nothing. It cannot return a *different* value, because no
> different value is ever written under an existing key — and Requirement 11.4's
> immutable-value rule enforces that at the server as well.

The cost of the failure mode is one recompile. `linearizable` mode exists for
anyone who wants the guarantee anyway, and pays a Raft round trip per lookup
for it.

---

## Component 6 — Authentication and authorization

### Threat model, first

A compile cache is an **execution-authority store**. An entry written into it
becomes object code linked into every consumer's binaries. The server cannot
defend itself by inspecting content: sccache's key is a hash of the compilation
*inputs*, not of the stored output, so nothing about a value can be verified
against its key. Authorization is therefore not a hardening measure layered on
top of the feature — it is the only control there is, and it is designed as
one.

That produces three rules that shape the design:

1. **Write access is narrower than read access.** Untrusted producers (pull
   requests, contributor machines) get `read_only`; only trusted, protected-branch
   CI gets `read_write`.
2. **Existing entries are immutable.** A write that would change the bytes
   under an existing key is refused and counted, so poisoning an already-populated
   key becomes an alertable event instead of a silent overwrite.
3. **Deny by default, everywhere.** No ACL means the gateway does not start.

### `redis_acl`

```cpp
enum class redis_role { read_only, read_write, admin };

struct redis_acl_user {
    std::string _name;
    std::string _secret;        // "pbkdf2-sha256$<iters>$<b64 salt>$<b64 dk>"
    bool _enabled{true};
    redis_role _role{redis_role::read_only};
    std::vector<std::string> _key_prefixes;   // empty ⇒ no keys
    std::vector<std::string> _cert_subjects;  // mTLS identity mapping
};

class redis_acl {
public:
    [[nodiscard]] auto authenticate(std::string_view user, std::string_view secret) const
        -> std::optional<redis_identity>;
    [[nodiscard]] auto authenticate_certificate(std::string_view subject) const
        -> std::optional<redis_identity>;
    [[nodiscard]] auto authorize(const redis_identity&, std::string_view command,
                                 std::span<const std::string> keys) const -> acl_decision;
    auto reload(const std::filesystem::path&) -> void;   // atomic swap or no change
};
```

**Secret storage.** PBKDF2-HMAC-SHA256 via OpenSSL's `PKCS5_PBKDF2_HMAC` —
already linked, no new dependency. Per-user random salt of 16 bytes from
`RAND_bytes`; iteration count stored *in the record* so it can be raised later
without invalidating existing users; 600,000 as the shipped default.
Verification uses `CRYPTO_memcmp`. A companion subcommand
(`redis_gateway_node --hash-secret`) generates records so that no operator
workflow requires writing a plaintext secret into a file.

**Uniform failure.** An unknown user and a wrong password produce the identical
`-WRONGPASS invalid username-password pair or user is disabled.` reply, and the
verification path performs the same KDF work in both cases, so neither the
message nor the timing enumerates users.

**ACL file format.** Line-oriented with `#` comments, modelled on `redis.conf`'s
`user` directive, because an operator edits this file by hand and JSON has no
comments:

```
# user <name> <secret-record|nopass> <role> [prefix …] [cert=<subject>]
user ci-main    pbkdf2-sha256$600000$…$…  read_write  kythira/main/
user ci-pr      pbkdf2-sha256$600000$…$…  read_only   kythira/main/
user dev        pbkdf2-sha256$600000$…$…  read_only   kythira/main/
user internal   pbkdf2-sha256$600000$…$…  read_write  ""            cert=CN=kythira-node
```

Note the shape that example encodes: PR builds read the same prefix that
trunk builds write. That is the whole security posture of a shared compile
cache in three lines.

**Reload** parses into a fresh `redis_acl` and swaps it atomically; a parse
failure logs an error and leaves the previous ACL in force. Authorization is
evaluated per command, not per connection, so a reload reaches live
connections.

### Where the checks sit

```
connection accepted
  ├─ TLS handshake (optional; mTLS ⇒ authenticate_certificate(subject))
  ├─ state = unauthenticated  (unless mTLS established an identity)
  │    permitted: AUTH, HELLO, QUIT, RESET      everything else → -NOAUTH
  ├─ AUTH / HELLO … AUTH → authenticate() → identity, or -WRONGPASS + rate-limit
  └─ per command: authorize(identity, name, keys)
        role check   → -NOPERM User <u> has no permissions to run the '<cmd>' command
        prefix check → -NOPERM No permissions to access a key
```

`AUTH password` (one argument) maps to the configured default user, because
`redis-rs` falls back to the single-argument form when the two-argument form is
rejected in a way that suggests the server predates ACL users
(`check_resp2_auth` → `AuthResult::ShouldRetryWithoutUsername`).

**Audit.** Every denial emits a structured line — identity or `unknown`, source
address, command, key prefix, reason — on a stream that can be routed
separately from operational logging. Secrets and values never appear; `AUTH`
and `HELLO … AUTH` argument lists are redacted in any command trace.

### Transport security

- TLS on the listener so `rediss://` works, with the server certificate
  obtained through the existing `certificate_provider` / CA-cluster / ACME
  machinery rather than a second certificate story.
- TLS 1.2 floor, 1.3 preferred.
- Optional mTLS: the peer certificate's SAN or CN is matched against
  `cert_subjects` to yield an identity, which is then subject to exactly the
  same authorization rules as a password identity.
- sccache's `#insecure` URL suffix disables hostname verification client-side.
  It is documented here as a development-only affordance that defeats the
  purpose of TLS.

---

## Component 7 — Routing and one-hop forwarding

sccache's non-cluster client does not follow `MOVED`. So every gateway must be
able to answer for every key, and the way it does that is to forward.

```
resolve(key) → descriptor
  ├─ local leader   → serve
  ├─ local follower → serve (reads, any_replica mode) / forward (writes)
  └─ not local      → forward to leader_hint's gateway endpoint
```

- **One hop, enforced.** A command that arrived on an internal connection is
  never forwarded again; it is answered with an error instead. That is what
  makes a stale routing map incapable of producing a loop.
- **Internal identity.** Forwarded commands authenticate as a dedicated
  internal user, never by replaying the client's credentials, and are
  distinguishable in the audit log.
- **Deadline propagation.** The forwarded command carries what remains of the
  client's budget, so a slow hop surfaces as a timeout at the client's own
  deadline rather than after two full ones.
- **Endpoint resolution** uses the cluster's existing membership and
  peer-discovery information; there is no second address list to maintain.
- **Pooling.** Internal connections are pooled per peer and subject to the same
  connection limits as client connections.

---

## Component 8 — Error mapping

| Internal condition | Wire reply |
|---|---|
| No identity yet | `-NOAUTH Authentication required.` |
| Bad user or password | `-WRONGPASS invalid username-password pair or user is disabled.` |
| Role forbids the command | `-NOPERM User <u> has no permissions to run the '<cmd>' command` |
| Key outside the identity's scope | `-NOPERM No permissions to access a key` |
| `unrouted_key_exception` | `-ERR no shard owns this key` |
| `shard_not_leader_exception`, forwarding unavailable | `-ERR shard has no reachable leader, retry` |
| Replica behind / catching up | `-LOADING Kythira is loading the dataset in memory` |
| Submission timeout | `-ERR timeout submitting to shard, retry` |
| Value over `max_value_bytes` | `-ERR value exceeds the configured maximum of <n> bytes` |
| Existing key, different bytes (immutable-value rule) | `-ERR value conflict for an existing key` |
| `SETEX seconds <= 0` | `-ERR invalid expire time in 'setex' command` |
| `SELECT` with db ≠ 0 | `-ERR DB index is out of range` |
| `HELLO` with unsupported version | `-NOPROTO unsupported protocol version` |
| Unknown command | `-ERR unknown command '<name>', with args beginning with: …` |
| Framing violation / limit exceeded | `-ERR Protocol error: <reason>` then close |

Every one of these, at the client, becomes either a miss or a counted write
error. None of them fails a build.

---

## Component 9 — Configuration

Environment variables, matching `cmd/chaos_node/config.hpp`'s `from_env()`
convention, plus a file for the ACL because it holds a list.

| Variable | Default | Meaning |
|---|---|---|
| `KYTHIRA_REDIS_LISTEN` | `0.0.0.0:6379` | RESP listener |
| `KYTHIRA_REDIS_TLS_LISTEN` | unset | TLS listener for `rediss://` |
| `KYTHIRA_REDIS_TLS_CERT` / `_KEY` / `_CA` | unset | Certificate material; may instead be provisioned by the CA client |
| `KYTHIRA_REDIS_REQUIRE_CLIENT_CERT` | `false` | mTLS |
| `KYTHIRA_REDIS_ACL_FILE` | *required* | ACL path; absent ⇒ refuse to start |
| `KYTHIRA_REDIS_ALLOW_ANONYMOUS` | `false` | Explicit opt-out of Requirement 10.1 |
| `KYTHIRA_REDIS_READ_CONSISTENCY` | `leader` | `leader` / `any_replica` / `linearizable` |
| `KYTHIRA_REDIS_MAX_VALUE_BYTES` | `8388608` | Requirement 6 |
| `KYTHIRA_REDIS_MAX_CLIENTS` | `1024` | Requirement 14 |
| `KYTHIRA_REDIS_IDLE_TIMEOUT_MS` | `300000` | Above the client's pool churn |
| `KYTHIRA_REDIS_COMMAND_TIMEOUT_MS` | `5000` | Below the client's read timeout |
| `KYTHIRA_REDIS_MAX_SHARD_BYTES` | `1073741824` | Eviction budget |
| `KYTHIRA_REDIS_SWEEP_BATCH` | `1024` | Expiry/eviction batch bound |
| `KYTHIRA_REDIS_IMMUTABLE_VALUES` | `true` | Requirement 11.4 |
| `KYTHIRA_REDIS_FORWARDING` | `true` | Requirement 13 |

### The matching client configuration

```bash
export SCCACHE_REDIS_ENDPOINT=rediss://cache.example.internal:6379
export SCCACHE_REDIS_USERNAME=ci-main
export SCCACHE_REDIS_PASSWORD="$CI_CACHE_SECRET"
export SCCACHE_REDIS_KEY_PREFIX=kythira/main/
export SCCACHE_REDIS_EXPIRATION=1209600        # 14 days ⇒ SETEX, not SET
export RUSTC_WRAPPER=sccache
```

`SCCACHE_REDIS_RW_MODE=READ_ONLY` on a PR runner is a courtesy that saves a
round trip; the `read_only` role on the `ci-pr` user is what actually enforces
it.

---

## Component 10 — Observability

Metrics through the existing `metrics` concept, one metric name per family with
a `command` dimension: hits, misses, writes, deletes, bytes in/out, latency,
oversize rejections, value conflicts, evictions, expirations reclaimed, auth
failures, authz denials, forwards, connections accepted/rejected, connections
current. `INFO` reports version, uptime, connected clients, local shard count,
key count and approximate bytes so `redis-cli info` is useful to a human.
Per-command logging is off by default; a healthy server is quiet.

---

## Serializer guidance (and the number behind it)

The default `json_serializer` base64-encodes each log entry's command bytes
into a JSON document (`include/raft/json_serializer.hpp:152`). Base64 is a 4/3
expansion, and the tree ships three binary alternatives — `cbor_serializer`,
`protobuf_serializer`, `ion_serializer` — that carry byte strings natively.

Measured (task 14): one AppendEntries request carrying one `SET` of a
sccache-shaped key (`sccache/h/h/h/<64 hex>`) and an incompressible random
value, serialized by `json_rpc_serializer` and `cbor_rpc_serializer` on the
same host (g++ -O2, single thread; the time is serialize only):

| value   | log command | JSON AppendEntries | ratio  | JSON time | CBOR AppendEntries | ratio   | CBOR time |
|---------|-------------|--------------------|--------|-----------|--------------------|---------|-----------|
| 4 KiB   | 4,192 B     | 5,783 B            | 1.380× | 80 µs     | 4,343 B            | 1.036×  | 4 µs      |
| 64 KiB  | 65,632 B    | 87,703 B           | 1.336× | 1.7 ms    | 65,785 B           | 1.0023× | 12 µs     |
| 1 MiB   | 1,048,672 B | 1,398,423 B        | 1.334× | 19 ms     | 1,048,825 B        | 1.0001× | 0.5 ms    |
| 8 MiB   | 8,388,704 B | 11,185,131 B       | 1.333× | 158 ms    | 8,388,857 B        | 1.0000× | 4.9 ms    |

So the estimate this section used to carry was right on the volume: JSON is
the base64 4/3 plus a fixed ~150 B of framing, CBOR is the payload plus
~150 B. An 8 MiB entry costs 10.7 MiB per follower under JSON and 8.0 MiB
under CBOR, and the same again inside every snapshot that carries it. The
time column is the part the estimate missed: at 8 MiB the JSON serializer
spends 158 ms of leader CPU per follower on base64 and JSON escaping against
5 ms for CBOR, which on a busy shard is a leader-side stall of the same order
as the network transfer itself.

Any deployment carrying this workload should use a binary serializer. The
daemon (`cmd/redis_gateway_node`) is built on `cbor_serializer` for exactly
this reason; the design does not assert a winner among the three binary
options, only that JSON is not one.

---

## Testing Strategy

1. **RESP codec unit tests.** Pipelining (the literal `redis-rs` handshake
   sequence: `AUTH`, `SELECT`, `CLIENT SETINFO ×2` in one write, four replies
   back in order), partial frames split across reads, RESP2 vs RESP3 null,
   every limit in Component 1's table, and malformed-frame closure.
2. **State machine unit tests.** Determinism (same command sequence ⇒ identical
   `get_state()` across independently constructed instances), the
   `split → absorb → get_state()` byte-for-byte round trip required by
   `splittable_state_machine`'s second law, snapshot round trip including
   deadlines, and the stale-sweep rejection: propose a sweep, rewrite the key,
   apply the sweep, assert the key survives.
3. **ACL unit tests.** KDF record round trip, constant-time verification,
   identical reply and comparable timing for unknown-user vs wrong-password,
   role and prefix enforcement per command, reload atomicity, and a
   parse-failure leaving the prior ACL in force.
4. **Gateway integration tests.** Against a small in-process multi-Raft
   cluster: hit/miss, TTL expiry visibility, oversize rejection, immutable-value
   conflict, forwarding to a non-local leader, one-hop enforcement, and
   `-LOADING` while a replica catches up.
5. **Real-sccache acceptance test.** A real `sccache` binary against a real
   gateway, compiling a small Rust crate twice from a clean target directory:
   zero hits then non-zero hits, per `sccache --show-stats`. Plus the negative
   case — gateway stopped, both builds still succeed — which is what proves the
   cache is an accelerant and not a build dependency. Plus the authorization
   case: `ci-pr` reads what `ci-main` wrote and is refused a write with
   `-NOPERM`.
6. **Container-runtime compliance.** Every containerized test follows
   `CLAUDE.md`: no static IPs, service-name addressing, `container_runtime()` /
   `compose_prefix()` instead of a hardcoded `docker`, no privileged or
   host-network features — verified under both Docker and rootless Podman.
7. **Optional-dependency isolation.** Configure and build with the feature ON
   and OFF and diff the target lists, following the method already used for the
   stdexec backend and for ccache.

---

## Deferred, with the design recorded

**Redis Cluster protocol.** sccache exposes `SCCACHE_REDIS_CLUSTER_ENDPOINTS`
and OpenDAL wires it to `redis-rs`'s `ClusterClient`, so this is a real option
rather than a hypothetical. The mapping that would make it work is worth
recording because it is unusually clean: use the **CRC16 slot number** (0–16383)
as the `multi_raft` routing `Key` instead of the key string. A contiguous run of
slots is then exactly a `shard_range<std::uint16_t>`, `CLUSTER SLOTS` is a
direct rendering of the shard map, and a split becomes a slot-range split.
`MOVED` replaces forwarding entirely. The costs are a CRC16 implementation,
hash-tag parsing, `ASK` semantics during migration, and losing the property that
the routing key is the user's key. Requirement 13's forwarding makes none of
that necessary for sccache, so it is deferred rather than designed in.

**Large values outside the log.** The natural next step for this workload: keep
the blob in the object store this project already speaks to (S3, GCS, Azure,
OSS, OCI — see `object_store_persistence.hpp` and the per-cloud clients) and put
only a digest, a length and a location in the Raft log. That removes the value
size limit, shrinks snapshots to metadata, and makes replication cost
independent of object size. It also changes the durability story — an entry
would then be durable only once the object store acknowledges it, and orphaned
objects need a reaper — which is why it is Phase 2 with its own requirements
rather than a footnote here.

**Replicated ACL.** The ACL is a per-gateway file in this design. Storing it in
a dedicated Raft group would make credential rollout atomic across the cluster
instead of a deployment-ordering concern. Deferred as its own change, because
it introduces a bootstrap-ordering problem (the gateway needs an ACL before it
can serve, and the ACL would live behind the thing it gates) that deserves
proper treatment.

**Cache administration surface.** `SCAN`, key listing, bulk invalidation by
prefix, and per-project usage reporting. No client in the target stack needs
them; a human operator eventually will.
