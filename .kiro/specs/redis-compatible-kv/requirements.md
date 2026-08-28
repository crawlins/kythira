# Requirements Document — Redis-Compatible KV on Multi-Raft (sccache Backend)

## Introduction

This document specifies a **minimal** Redis-compatible server built on top of
Kythira's existing multi-Raft sharding layer (`include/raft/multi_raft.hpp`,
`.kiro/specs/multi-raft/`), scoped to exactly one workload:
[sccache](https://github.com/mozilla/sccache)'s `redis` storage backend.

The motivation is concrete. This repository already caches its C++ build with
ccache (`.kiro/specs/ccache-adoption/`, measured 59% wall-clock reduction on a
no-change rebuild). Its Rust build — the `lakers` overlay port, which shells
out to `cargo build` from `vcpkg-overlays/lakers/portfile.cmake:44` — has no
compiler cache at all, because **ccache cannot cache rustc**: it is built
around the C/C++ preprocessor model and has no rustc backend. The de-facto
Rust equivalent is sccache, which is not a drop-in ccache replacement so much
as a different tool with a different storage model: where ccache is a local
directory, sccache's value proposition is a *shared* cache — S3, GCS, Azure,
memcached, webdav, or **Redis** — so that a cache entry produced by one CI
runner is a hit for every other runner and every developer.

Kythira is a replicated, sharded, consistent key-value substrate. A shared
compile cache is a replicated, sharded key-value workload. The gap between
them is a wire protocol. This spec closes that gap with the smallest server
that sccache will actually talk to, and nothing more.

**This is deliberately not "implement Redis."** Redis has ~240 commands, RDB
and AOF persistence, replication, pub/sub, scripting, streams, modules and
transactions. The requirements below are derived by reading what sccache's
client stack actually puts on the wire, and implementing that closure. The
evidence table in Requirement 1 is the whole scope-control mechanism: a
command that is not in that table is not in this spec.

### Why this workload is unusually well-suited to Kythira

Four properties of the sccache workload make it a good first Redis-shaped
consumer rather than an arbitrary one:

1. **Keys are uniformly-distributed hex digests.** sccache normalizes a cache
   key `abcdef…` into the path `a/b/c/abcdef…`
   (`sccache/src/cache/utils.rs`, `normalize_key`), and OpenDAL turns that into
   the Redis key `<root>a/b/c/abcdef…` (`opendal/core/core/src/raw/path.rs`,
   `build_abs_path`). The leading characters are hex nibbles of a digest, so
   the keyspace is uniform under lexicographic order — which is exactly the
   order `shard_map<GroupId, Key>` ranges on. Range splits land evenly with no
   hot-spotting and no hash-vs-range impedance mismatch.
2. **Values are immutable and content-derived.** A cache entry is the compiler
   output for a specific set of inputs. It is written once and never modified.
   That removes the usual reason a KV read must be linearizable: a stale read
   can only ever return the correct value or a miss, never a *wrong* value.
3. **A miss is not an error.** sccache treats every read failure as a cache
   miss and every write failure as a logged, counted non-event
   (`sccache/src/server.rs`: a cache-write error increments
   `cache_write_errors` and the compilation result is still returned). Every
   degradation this server can suffer — unavailable shard, oversized value,
   expired entry, revoked credential — costs a recompile, not a broken build.
4. **Single-key operations only.** Every command in Requirement 1's table
   names exactly one key. There is never a cross-shard operation, so
   `cross_shard_command_exception` is unreachable by construction and this spec
   needs no distributed transaction.

Property 3 is what makes a *minimal* implementation honest rather than
reckless: the failure modes of a partial Redis are all absorbed by the client.

### What this spec does not cover

- **Adopting sccache in this repository's own build.** Wiring
  `RUSTC_WRAPPER=sccache` into the `lakers` port and into CI is the motivating
  consumer, but it is separable work with its own measurement and rollout, and
  belongs in its own spec. This spec delivers the server; it does not change
  how this repository builds.
- **General Redis compatibility.** See the Non-Goals section.

## Glossary

- **RESP**: the REdis Serialization Protocol. **RESP2** is the default
  (`redis-rs` `ProtocolVersion::RESP2` is `#[default]`, `redis/src/types.rs`);
  **RESP3** is negotiated by a `HELLO 3` handshake and differs from RESP2 in
  its null, map and set encodings.
- **Gateway**: the process-level component this spec adds — a RESP listener
  that terminates client connections, authenticates and authorizes them, and
  turns commands into `multi_raft` operations. `redis_gateway<Types>`.
- **KV state machine**: `redis_kv_state_machine`, the replicated `state_machine`
  (`include/raft/types.hpp:199`) plus `splittable_state_machine`
  (`include/raft/splittable_state_machine.hpp`) implementation that holds the
  key-value data for one shard.
- **Owning shard**: the Raft group whose `shard_range<Key>` contains a key, per
  the host's `shard_map`.
- **Client stack**: sccache → OpenDAL `services::Redis` → the `redis` crate
  (`redis-rs`) v1.2 with features `cluster-async`, `tokio-comp`,
  `connection-manager`. Every wire-level claim in this document is sourced from
  that stack, not from the Redis manual.
- **ACL**: the gateway's access-control list — the set of users, their
  secrets, their command roles and their key-prefix scopes.
- **Identity**: the authenticated principal on a connection, established
  either by `AUTH`/`HELLO … AUTH` or by a verified client certificate.
- **Immutable-value rule**: the gateway's default policy of refusing a write
  that would change the bytes stored under an existing key (Requirement 11.4).

## Requirements

### Requirement 1: Implement exactly the command closure sccache's client stack uses

**User Story:** As the maintainer of a small server, I want the supported
command set fixed by evidence rather than by guesswork, so that "minimal" is a
verifiable claim and not an aspiration.

#### Acceptance Criteria

1. The gateway SHALL implement the following commands, and this table SHALL be
   the definition of scope:

   | Command | Issued by | Source |
   |---|---|---|
   | `GET key` | every cache read (full read) | `opendal` `RedisCore::get` → `conn.get(key)`; `reader.rs` full-range path |
   | `SET key value` | cache write with no TTL configured | `RedisCore::set` → `conn.set(key, value)` |
   | `SETEX key seconds value` | cache write when `SCCACHE_REDIS_EXPIRATION` is set | `RedisCore::set` → `conn.set_ex(...)`; `redis-rs` `set_ex` expands to `SETEX key seconds value` (`redis/src/commands/mod.rs`) |
   | `DEL key` | deletion path | `RedisCore::delete` → `conn.del(key)` |
   | `EXISTS key` | `stat`, and the first half of a ranged read | `RedisCore::len` → `conn.exists(key)` |
   | `STRLEN key` | content length for `stat` and ranged reads | `RedisCore::len` → `redis::cmd("STRLEN")` |
   | `GETRANGE key start end` | ranged read | `RedisCore::get_range` → `conn.getrange(key, start, end)` |
   | `PING` | connection-pool liveness check on every recycle | `RedisConnectionManager::is_recyclable` → `o.ping::<String>()`, asserting the reply is exactly `PONG` |
   | `AUTH [username] password` | RESP2 handshake when credentials are configured | `redis-rs` `connection_setup_pipeline` → `authenticate_cmd` |
   | `HELLO [3 [AUTH user pass]]` | RESP3 handshake (`?protocol=resp3`) | `redis-rs` `connection_setup_pipeline` → `resp3_hello` |
   | `SELECT db` | handshake when `SCCACHE_REDIS_DB` is non-zero | `redis-rs` `connection_setup_pipeline`, emitted only when `db != 0` |
   | `CLIENT SETINFO LIB-NAME\|LIB-VER value` | every handshake, unconditionally | `redis-rs` `connection_setup_pipeline`, both marked `.ignore()` |
   | `QUIT` | orderly close | RESP convention |

2. The gateway SHALL additionally implement `COMMAND`, `INFO`, `DBSIZE`,
   `TTL` and `ECHO` as **operator conveniences only**, so that `redis-cli`
   connects and a human can inspect the server; these SHALL be documented as
   diagnostic surface, not as a compatibility claim.
3. WHEN any other command is received THEN the gateway SHALL reply
   `-ERR unknown command '<name>', with args beginning with: …` and SHALL keep
   the connection open — a client that pipelines an unsupported command
   alongside supported ones SHALL still receive one reply per command, in
   order.
4. The gateway SHALL NOT implement `MULTI`/`EXEC`, `SCAN`, `KEYS`, `EVAL`,
   `SUBSCRIBE`, `MSET`/`MGET`, `SETNX`, `GETSET`, `APPEND`, `EXPIRE`,
   `PERSIST`, `FLUSHDB` or any hash, list, set, stream or module command in
   this spec's scope.
5. `CLIENT SETINFO`'s reply SHALL NOT be load-bearing: because `redis-rs`
   marks both `CLIENT SETINFO` commands `.ignore()` and
   `check_connection_setup` inspects only the `AUTH`/`HELLO` and `SELECT`
   reply indices (`redis/src/connection.rs`), an error reply to `CLIENT` does
   not fail the handshake. The gateway SHALL nevertheless answer it `+OK`, and
   SHALL in all cases emit **exactly one reply per received command** so that a
   pipelined handshake stays framed.

---

### Requirement 2: RESP wire protocol

**User Story:** As a client library that pipelines its handshake, I need a
server that frames replies correctly under pipelining, so that the connection
does not desynchronize on the first request.

#### Acceptance Criteria

1. The gateway SHALL accept RESP2 arrays of bulk strings as the request
   encoding, and SHALL support **pipelining**: multiple complete commands
   arriving in one TCP segment SHALL each be executed in arrival order with
   one reply each. This is not optional — `redis-rs` sends its entire
   connection-setup sequence as a single pipeline before reading any reply.
2. The gateway SHALL emit RESP2 replies by default: simple strings (`+OK`,
   `+PONG`), errors (`-ERR …`), integers (`:1`), bulk strings (`$n`), and the
   RESP2 null bulk string (`$-1\r\n`) for a missing key.
3. WHEN a `HELLO 3` is received THEN the gateway SHALL switch that connection
   to RESP3 for subsequent replies — which for this command set means only the
   null encoding changes (`_\r\n`) and the `HELLO` reply itself is a map — and
   SHALL reply to `HELLO` with a map carrying at least `server`, `version`,
   `proto`, `id`, `mode`, `role` and `modules`.
4. WHEN a `HELLO` with an unsupported protocol version is received THEN the
   gateway SHALL reply `-NOPROTO unsupported protocol version`.
5. The gateway SHALL enforce configurable limits on the inline request:
   maximum multibulk element count, maximum bulk-string length
   (`proto_max_bulk_len`, default 32 MiB), and maximum accumulated unparsed
   buffer per connection. A request exceeding any limit SHALL be answered with
   an error and the connection SHALL be closed, rather than growing the buffer.
6. The gateway SHALL reject a malformed frame with
   `-ERR Protocol error: <reason>` and close the connection, matching Redis's
   own behaviour, because a desynchronized stream cannot be recovered by
   continuing.
7. Inline (non-RESP, space-separated) commands MAY be supported for `redis-cli`
   convenience; if unsupported, the gateway SHALL answer them as a protocol
   error rather than misparsing them.

---

### Requirement 3: Key routing onto multi-Raft shards

**User Story:** As an operator, I want a Redis key to land on the shard that
owns it, using the sharding layer that already exists, so that this feature
inherits split, merge, placement and rebalancing rather than reimplementing
them.

#### Acceptance Criteria

1. The gateway SHALL use `multi_raft<Types, std::string, std::uint64_t>` with
   the Redis key, verbatim and unmodified, as the routing `Key`. The keyspace
   ordering SHALL therefore be `std::string`'s lexicographic byte order, which
   `shard_range<std::string>` already implements.
2. The gateway SHALL NOT hash, prefix, or otherwise transform the key before
   routing. sccache's own `a/b/c/<digest>` layout already provides uniform
   distribution under this ordering (see Introduction, property 1).
3. Every command in Requirement 1's table that names a key SHALL be routed by
   that single key; the gateway SHALL NOT implement any operation spanning two
   shards.
4. WHEN a key resolves to no shard THEN the gateway SHALL answer
   `-ERR no shard owns this key` and SHALL count the event, since this
   indicates a routing-map gap rather than a client error.
5. The gateway SHALL tolerate a shard split or merge occurring between
   resolution and submission by using `multi_raft`'s existing epoch-checked
   submission path and its stale-routing retry, and SHALL surface a persistent
   failure as a retryable error rather than a silent wrong-shard write.

---

### Requirement 4: The replicated KV state machine

**User Story:** As a Raft replica, I need the key-value data to be a
deterministic state machine that can be snapshotted, split and merged, so that
it is a first-class Kythira state machine and not an exception to the model.

#### Acceptance Criteria

1. `redis_kv_state_machine` SHALL satisfy `kythira::state_machine<SM,
   std::uint64_t>` and `kythira::splittable_state_machine<SM, std::string>`.
2. The primary index SHALL be an **ordered** associative container keyed on the
   Redis key. An unordered container SHALL NOT be used, because
   `split_state()`, `get_state()` and the eviction scan all iterate it and
   `splittable_state_machine`'s first law requires that iteration to be
   identical on every replica.
3. `apply()` SHALL be a pure function of the command bytes and the prior state.
   It SHALL NOT read any clock, random source, environment variable, file, or
   network — the determinism defect already found and fixed in
   `distributed_lock_state_machine` (`.kiro/specs/state-machine-examples/`) is
   the precedent this requirement exists to prevent repeating.
4. `get_state()` SHALL serialize every live entry — key, value, and expiry
   deadline — in key order, and `restore_from_snapshot()` SHALL reproduce a
   byte-identical `get_state()`.
5. `split_state(keys)` SHALL partition entries by the same lexicographic
   ordering used for routing, and `absorb(blob, range)` SHALL be its exact
   inverse; a round-trip property test SHALL assert
   `split → absorb → get_state()` equals the original `get_state()` byte for
   byte.
6. `can_split_at(key)` SHALL return `true` for every key: this data model has
   no multi-key invariant, no secondary index, and no transaction to protect.
   This SHALL be stated in a comment rather than left implicit, because a
   future feature that adds one must revisit it.
7. `approximate_size_bytes()` and `approximate_key_count()` SHALL be maintained
   incrementally, not computed by scanning, so that the split policy's tick
   cost is independent of shard size.
8. The command encoding written into the Raft log SHALL be a **versioned,
   length-prefixed binary format** — not JSON and not a text protocol — with a
   leading format-version byte, so that a value's bytes survive the log
   unmodified and a future format change is detectable rather than silently
   misparsed.

---

### Requirement 5: The write path

**User Story:** As sccache storing a compilation result, I want `+OK` to mean
the entry is durably replicated, so that a runner that reports a cache write
is not lying to the next runner.

#### Acceptance Criteria

1. `SET`, `SETEX` and `DEL` SHALL be submitted through
   `multi_raft::submit_command(key, command, timeout)` and SHALL reply only
   after the entry is committed **and applied** on the local leader replica.
2. WHEN the local replica is not the leader of the owning shard THEN the
   gateway SHALL either forward the command per Requirement 12 or, if
   forwarding is disabled or fails, reply with a retryable error carrying the
   leader hint from `shard_not_leader_exception::leader_hint()`.
3. `SET` SHALL reply `+OK`; `DEL` SHALL reply `:1` when a key was removed and
   `:0` when it was already absent; `SETEX` SHALL reply `+OK`.
4. WHEN `SETEX` is called with `seconds` less than or equal to zero THEN the
   gateway SHALL reply `-ERR invalid expire time in 'setex' command` and SHALL
   NOT propose a log entry.
5. The command timeout SHALL be configurable and SHALL default to a value below
   the client's own read timeout, so that a slow shard surfaces as a clean
   error rather than a client-side connection reset.
6. A write that exceeds the value-size limit of Requirement 6 SHALL be rejected
   **before** a log entry is proposed.

---

### Requirement 6: Value size limits, and why the Raft log needs them

**User Story:** As a cluster operator, I want a hard ceiling on how large a
single cache entry may be, so that one 200 MB rlib cannot stall replication for
every other key on the shard.

#### Acceptance Criteria

1. The gateway SHALL enforce a configurable maximum value size, defaulting to
   **8 MiB**, and SHALL reject a larger `SET`/`SETEX` with
   `-ERR value exceeds the configured maximum of <n> bytes`.
2. The rejection SHALL be documented as a *safe* degradation with its exact
   consequence: sccache increments `cache_write_errors` and returns the
   compilation result (`sccache/src/server.rs`), so the entry is simply not
   cached and the build proceeds.
3. The default SHALL be justified in the design against the actual cost of a
   large entry in this codebase: the default `json_serializer`
   (`include/raft/json_serializer.hpp`) base64-encodes each entry's command
   bytes into a JSON document, so an 8 MiB value costs roughly 11 MiB of
   AppendEntries payload **per follower**, and the same bytes again in every
   snapshot that includes it.
4. The design SHALL recommend a binary RPC serializer (`cbor_serializer`,
   `protobuf_serializer` or `ion_serializer`, all already in-tree) for any
   deployment carrying this workload, and SHALL state the measured or estimated
   difference rather than asserting a preference.
5. A shard's target size SHALL be documented as the second, independent
   ceiling: `get_state()` materializes the whole shard for a snapshot, so the
   split policy's size threshold — not the value limit — is what keeps snapshot
   cost bounded.

---

### Requirement 7: The read path, and the consistency it actually offers

**User Story:** As sccache looking up a cache key, I want a fast lookup that
does not cost a Raft round trip, and I want the server's consistency guarantee
stated plainly rather than implied.

#### Acceptance Criteria

1. `GET`, `EXISTS`, `STRLEN` and `GETRANGE` SHALL be served from applied state
   without proposing a log entry in the default configuration.
2. The gateway SHALL NOT use `node::read_state()` for these commands:
   `read_state()` returns the **entire** state machine's serialized state
   (`include/raft/raft.hpp:253`), so using it for a single-key lookup would
   serialize the whole shard per request.
3. The state machine SHALL expose a lookup reachable through
   `node::with_state_machine()`. Because that helper runs under the node's
   `_mutex` and its contract forbids blocking (`include/raft/raft.hpp:460`),
   the lookup SHALL return a **shared handle** to the value (an
   O(1) reference-count copy), and the value bytes SHALL be written to the
   socket after the lock is released. Copying a multi-megabyte value under the
   node lock SHALL be treated as a defect, not an implementation detail.
4. Read consistency SHALL be a configurable mode with three values:
   - `leader` (**default**) — serve from the applied state of the local leader
     replica of the owning shard. Gives read-your-writes for a client whose
     writes went to the same leader, at no extra round trip.
   - `any_replica` — serve from any local replica, including followers. Scales
     reads; may return a miss for a very recently written key.
   - `linearizable` — propose a read command through the log. Correct at the
     cost of a Raft round trip per lookup; provided for completeness and not
     recommended for this workload.
5. The documentation SHALL state the safety argument for the default rather
   than burying it: because values are immutable and content-derived
   (Introduction, property 2), a stale read can only ever produce a **miss**,
   whose cost is one recompile — it can never produce a wrong object.
6. WHEN the owning shard has no local replica and forwarding is unavailable
   THEN the gateway SHALL reply with a retryable error, which sccache treats as
   a miss.
7. `GETRANGE` SHALL implement Redis's negative-index and clamping semantics
   over the stored value, since OpenDAL computes `start`/`end` from a content
   length it obtained via `STRLEN` and will pass an inclusive end index.

---

### Requirement 8: Deterministic expiration

**User Story:** As a replica, I need TTLs that cannot make me diverge from my
peers, because a state machine that expires a key at a slightly different
moment on each replica is a silently corrupt cluster.

#### Acceptance Criteria

1. `SETEX` SHALL be converted to an **absolute deadline** by the gateway on the
   leader, before the command is proposed: the proposed entry SHALL carry
   `expire_at_ms` (a wall-clock instant in milliseconds), never a relative
   duration.
2. `apply()` SHALL store `expire_at_ms` verbatim and SHALL NOT consult a clock.
   Every replica therefore holds the identical deadline for the identical key.
3. Expiry SHALL be applied **lazily at read time** by comparing the stored
   deadline against the reading node's clock, and this comparison SHALL NOT
   mutate state. Two replicas whose clocks differ may therefore disagree, for
   the width of that skew, about whether a key is visible — which for this
   workload is a miss, not a divergence, and SHALL be documented as such.
4. Reclamation SHALL be performed by the **leader proposing an explicit sweep
   command** naming the keys to delete together with the `expire_at_ms` value
   each key was observed to carry. `apply()` SHALL delete a named key only if
   its currently-stored deadline equals the one named in the sweep, so that a
   key rewritten between proposal and apply is not destroyed by a stale sweep.
5. The sweep SHALL be bounded per invocation (a configurable maximum key count)
   and SHALL run on the leader's existing policy tick rather than on a new
   thread.
6. `get_state()` and `restore_from_snapshot()` SHALL round-trip `expire_at_ms`,
   so that a replica restored from a snapshot expires keys identically to one
   that replayed the log.
7. `TTL` (diagnostic, Requirement 1.2) SHALL report the remaining time derived
   from the stored deadline, returning `-1` for no expiry and `-2` for a
   missing or already-expired key, matching Redis.

---

### Requirement 9: Bounded capacity and deterministic eviction

**User Story:** As an operator, I want the cache to stay within a size budget
without me pruning it by hand, and without eviction becoming a source of
replica divergence.

#### Acceptance Criteria

1. Each shard SHALL have a configurable maximum size in bytes and a maximum key
   count; exceeding either SHALL make the shard *eligible* for eviction, not
   trigger an immediate synchronous purge.
2. Eviction SHALL follow the same shape as expiry reclamation: the **leader**
   selects victims using whatever local, advisory information it has (approximate
   least-recently-used, oldest deadline, largest value) and proposes an
   explicit `EVICT` command naming them. `apply()` SHALL delete exactly the
   named keys and SHALL NOT re-run the selection, so the selection heuristic is
   free to be non-deterministic while the applied result is not.
3. Access-recency bookkeeping SHALL be leader-local and SHALL NOT be part of
   the replicated state, because a read served on a follower would otherwise
   mutate state that only some replicas see.
4. Eviction SHALL be bounded per invocation and SHALL emit a metric carrying
   the number of keys and bytes reclaimed.
5. WHEN a shard is over budget and eviction cannot keep up THEN writes SHALL
   continue to be accepted and the condition SHALL be surfaced as a metric and
   a log line — refusing writes would convert a capacity problem into a build
   failure for every client at once, and the workload's own tolerance for
   misses makes over-budget-and-shrinking the safer state.
6. Interaction with the split policy SHALL be documented: a shard growing past
   its size budget is normally a signal to **split**, not to evict; eviction is
   the response to the *cluster* being full, not to one shard being large.

---

### Requirement 10: Authentication

**User Story:** As the owner of a shared compile cache, I want every connection
to prove who it is before it can touch a key, so that "on the network" is not
the same as "trusted."

#### Acceptance Criteria

1. Authentication SHALL be **required by default**. A gateway configured with
   no ACL SHALL refuse to start rather than start open, and anonymous access
   SHALL require an explicit opt-in flag that is named for what it does
   (e.g. `--allow-anonymous`) and logged as a warning at every startup.
2. The gateway SHALL support both authentication paths the client stack can
   produce:
   - RESP2: `AUTH password` and `AUTH username password`. `redis-rs` emits the
     two-argument form when a username is configured and retries without the
     username if the server's error indicates it (`check_resp2_auth` →
     `AuthResult::ShouldRetryWithoutUsername`), so the single-argument form
     SHALL also be accepted and SHALL map to the configured default user.
   - RESP3: `HELLO 3 AUTH username password`, authenticated as part of the
     handshake.
3. Before an identity is established, the gateway SHALL accept only `AUTH`,
   `HELLO`, `QUIT` and `RESET`, and SHALL answer every other command with
   `-NOAUTH Authentication required.` — the exact Redis error string, because
   client libraries match on it.
4. WHEN credentials are invalid THEN the gateway SHALL reply
   `-WRONGPASS invalid username-password pair or user is disabled.` and SHALL
   return the **identical** error for an unknown username and for a wrong
   password, so that the error cannot be used to enumerate users.
5. Secrets SHALL be stored as salted, iterated hashes — never as plaintext and
   never as a bare digest. The design SHALL specify the KDF (PBKDF2-HMAC-SHA256
   via the already-linked OpenSSL, a per-user random salt of at least 16 bytes,
   and an iteration count that is recorded in the stored record so it can be
   raised without invalidating existing entries).
6. Secret comparison SHALL be constant-time (`CRYPTO_memcmp`).
7. Secrets SHALL never appear in logs, metrics, traces, error messages, or
   command-echo diagnostics. Any command-tracing facility SHALL redact the
   argument list of `AUTH` and of `HELLO … AUTH`.
8. The gateway SHALL rate-limit failed authentication attempts per source
   address and SHALL emit a metric for each failure, so that a credential-
   stuffing attempt is visible rather than merely unsuccessful.
9. An authenticated identity SHALL be scoped to its connection and SHALL be
   re-established on reconnect; the gateway SHALL NOT cache an identity by
   source address.
10. `RESET` SHALL clear the connection's identity, returning it to the
    unauthenticated state.

---

### Requirement 11: Authorization

**User Story:** As a security-conscious operator, I want write access to the
cache to be strictly narrower than read access, because an entry written into a
compile cache becomes object code inside everybody's binaries.

#### Acceptance Criteria

1. The design SHALL state the threat model explicitly: **a compile cache is an
   execution-authority store.** An attacker who can write an entry that a
   victim's build later reads has achieved arbitrary code execution in that
   build. The server cannot defend itself by verifying content, because
   sccache's key is a hash of the compilation *inputs*, not of the stored
   output — nothing about a value can be checked against its key. Authorization
   is therefore the only control, and it must be designed as one.
2. Every user SHALL have a **command role**, deny-by-default:
   - `read_only` — the read commands of Requirement 1 plus the handshake and
     diagnostic commands. This SHALL be the role for untrusted or partially
     trusted builds (pull-request CI, contributor machines), and pairs with
     sccache's own client-side `SCCACHE_REDIS_RW_MODE=READ_ONLY`, which SHALL
     be documented as a convenience, not a control — the server-side role is
     the control.
   - `read_write` — additionally `SET`, `SETEX` and `DEL`. This SHALL be the
     role for trusted producers (protected-branch CI).
   - `admin` — additionally the operator/diagnostic surface.
3. Every user SHALL have a **key scope**: a list of allowed key prefixes, with
   the empty list meaning "no keys." Every key argument of every command SHALL
   be checked against the scope before the command is routed. This composes
   directly with sccache's `SCCACHE_REDIS_KEY_PREFIX`, letting one cluster host
   several mutually-untrusting projects.
4. The gateway SHALL implement an **immutable-value rule**, on by default: a
   write whose key already exists with different bytes SHALL be refused with
   `-ERR value conflict for an existing key` and counted under a dedicated
   metric. A write of *identical* bytes SHALL succeed as a no-op, because
   sccache legitimately rewrites its `.sccache_check` probe object with the
   same content on every server start. This turns cache poisoning of an
   already-populated key from a silent overwrite into a refused write and an
   alertable counter.
5. WHEN a command is refused for lack of permission THEN the gateway SHALL
   reply with the corresponding Redis error class — `-NOPERM` for a command or
   key that the identity may not use — and SHALL NOT reveal whether the key
   exists.
6. Authorization SHALL be evaluated on every command, not once at connection
   time, so that an ACL reload takes effect on live connections.
7. The ACL SHALL be reloadable without restarting the gateway or dropping
   established connections, and a reload that fails to parse SHALL leave the
   previous ACL in force and SHALL be logged as an error.
8. Every authentication and authorization decision that denies access SHALL be
   auditable: a structured log line naming the identity (or `unknown`), the
   source address, the command, and the reason — never the secret and never the
   value.

---

### Requirement 12: Transport security and cluster-internal identity

**User Story:** As an operator running this over a network wider than a rack, I
want the connection encrypted and the peers authenticated, using the
certificate machinery this project already has.

#### Acceptance Criteria

1. The gateway SHALL support TLS on its listener so that sccache's
   `rediss://host:port` endpoint form works, and SHALL be able to obtain its
   server certificate through this project's existing certificate machinery
   (`certificate_provider`, the CA cluster, and the ACME provider) rather than
   introducing a second certificate story.
2. TLS SHALL enforce a minimum protocol version of TLS 1.2 (defaulting to 1.3
   where the peer supports it) and SHALL reject weaker negotiation.
3. The gateway SHALL optionally require a client certificate (mTLS). WHEN mTLS
   is required THEN the certificate's subject or SAN SHALL be mapped to an ACL
   user, and that mapping SHALL be sufficient to establish identity without
   `AUTH` — but SHALL be subject to the same authorization rules.
4. The documentation SHALL note that sccache's `#insecure` URL suffix disables
   hostname verification on the client, that this defeats the purpose of TLS,
   and that it exists for self-signed development setups only.
5. Cluster-internal connections created by Requirement 13's forwarding SHALL
   authenticate as a dedicated internal identity, SHALL NOT reuse a client's
   credentials, and SHALL be distinguishable in the audit log from
   client-originated commands.
6. Plaintext listeners SHALL remain supported (sccache's documented default
   endpoint form is `redis://`), and the gateway SHALL log at startup, once,
   which listeners are plaintext.

---

### Requirement 13: Any node answers any key (single-endpoint operation)

**User Story:** As someone configuring `SCCACHE_REDIS_ENDPOINT` in CI, I want
one address to work, because the client I am configuring does not follow
cluster redirects.

#### Acceptance Criteria

1. Every gateway SHALL accept a command for **any** key, regardless of which
   shards its own process replicates.
2. WHEN the owning shard's leader is another node THEN the gateway SHALL
   forward the command to that node's gateway over an internal connection and
   relay the reply, so that the client sees a single, complete answer.
3. Forwarding SHALL be limited to **one hop**: a gateway SHALL NOT forward a
   command that arrived on an internal connection, and SHALL instead answer
   with an error, so that a stale routing map cannot produce a forwarding loop.
4. Forwarded commands SHALL carry a deadline derived from the client's
   remaining budget, and a forwarding failure SHALL surface as a retryable
   error rather than as a hang.
5. Internal connections SHALL be pooled and reused, and SHALL be subject to the
   same limits as client connections.
6. The gateway SHALL resolve node identity to an internal gateway endpoint
   using the cluster's existing membership/peer-discovery information rather
   than a second, hand-maintained address list.
7. The Redis Cluster protocol (`CLUSTER SLOTS`, `MOVED`/`ASK` redirection)
   SHALL be out of scope for this spec; the design SHALL record how it would
   map onto multi-Raft if it is ever wanted (see Non-Goals).

---

### Requirement 14: Connection management and resource limits

**User Story:** As an operator, I want a fleet of CI runners to be unable to
exhaust the gateway's memory or file descriptors.

#### Acceptance Criteria

1. The gateway SHALL enforce a configurable maximum concurrent client
   connection count and SHALL reject an over-limit connection with
   `-ERR max number of clients reached` before closing it, rather than
   accepting and starving it.
2. The gateway SHALL apply an idle timeout to client connections, and the
   default SHALL be documented against the client's behaviour: OpenDAL pools up
   to 10 connections per sccache process by default
   (`connection_pool_max_size`) and validates each on reuse with `PING`, so a
   timeout that is too short costs a reconnect, not a failure.
3. Each connection SHALL have a bounded read buffer and a bounded number of
   in-flight pipelined commands; exceeding the latter SHALL apply backpressure
   by ceasing to read from the socket, not by unbounded queueing.
4. The gateway SHALL bound total in-flight command memory across all
   connections, and SHALL shed load with a retryable error when the bound is
   reached.
5. Shutdown SHALL be graceful: stop accepting, finish in-flight commands within
   a deadline, then close.

---

### Requirement 15: Error taxonomy

**User Story:** As a client library, I need errors whose prefixes mean what
Redis says they mean, because I dispatch on them.

#### Acceptance Criteria

1. The design SHALL contain a complete table mapping each internal failure to a
   wire error, and the implementation SHALL match it.
2. The following prefixes SHALL be used with their standard meanings:
   `-NOAUTH`, `-WRONGPASS`, `-NOPERM`, `-ERR`, `-LOADING`, and `-NOPROTO`.
3. WHEN a replica is still catching up and cannot serve reads THEN the gateway
   SHALL reply `-LOADING Kythira is loading the dataset in memory`, which the
   client stack surfaces as an error and sccache converts to a miss.
4. Kythira's shard exceptions SHALL be mapped rather than leaked: a
   `shard_not_leader_exception` SHALL become a forward (Requirement 13) or a
   retryable `-ERR`, an `unrouted_key_exception` SHALL become the error of
   Requirement 3.4, and a timeout SHALL become a distinct retryable error.
5. No error message SHALL contain a secret, a value, or a stack trace.

---

### Requirement 16: Observability

**User Story:** As an operator, I want to know the cache's hit rate and its
denial rate, because those two numbers are the whole point of running it.

#### Acceptance Criteria

1. The gateway SHALL emit metrics through the existing `metrics` concept
   (`include/raft/metrics.hpp`), with a dimension for the command name, and
   SHALL cover at minimum: hits, misses, writes, deletes, bytes in, bytes out,
   per-command latency, rejected-oversize writes, value conflicts, evictions,
   expired-key reclamations, authentication failures, authorization denials,
   forwarded commands, and connections accepted/rejected.
2. The gateway SHALL log through the existing `logger` concept, at a level that
   makes a healthy server quiet: per-command logging SHALL be off by default.
3. `INFO` SHALL report at least the server version, uptime, connected clients,
   the number of local shards, key count and approximate bytes, so that
   `redis-cli info` is useful to a human debugging a cache.
4. The audit stream of Requirement 11.8 SHALL be separable from ordinary
   operational logging so that it can be routed to a different sink.

---

### Requirement 17: Verification against real sccache

**User Story:** As a reviewer, I want the acceptance test to be "real sccache
got a cache hit," not "our unit test says our parser works."

#### Acceptance Criteria

1. There SHALL be an end-to-end test that runs a real `sccache` binary
   configured with `SCCACHE_REDIS_ENDPOINT` pointing at this gateway, compiles
   a small Rust crate twice from a clean target directory, and asserts that
   `sccache --show-stats` reports zero hits on the first build and a non-zero
   hit count on the second.
2. The same test SHALL assert the negative case that the whole design rests on:
   with the gateway stopped, both builds SHALL still succeed (slowly), proving
   the cache is an accelerant and never a build dependency.
3. There SHALL be a test that exercises the authenticated path end to end: a
   `read_only` user SHALL be shown to read hits and to be refused writes with
   `-NOPERM`, and a `read_write` user SHALL be shown to populate the cache the
   read-only user then hits.
4. The containerized parts of these tests SHALL comply with this project's
   container-runtime rules (`CLAUDE.md`): no static IPs in compose files,
   service-name addressing, `container_runtime()` / `compose_prefix()` rather
   than a hardcoded `docker`, and no privileged or host-network features — they
   SHALL run under both Docker and rootless Podman.
5. There SHALL be protocol-level unit tests for the RESP codec, including
   pipelining, the exact `redis-rs` handshake sequence of Requirement 1, the
   RESP2 and RESP3 null encodings, and every limit of Requirement 2.5.
6. There SHALL be state-machine unit tests covering determinism, the
   `split → absorb` round-trip of Requirement 4.5, snapshot round-trip
   including deadlines, and the stale-sweep rejection of Requirement 8.4.
7. Tests SHALL follow this project's test standards
   (`.kiro/steering/test-execution-standards.md`), including the two-argument
   `BOOST_AUTO_TEST_CASE` timeout form, and SHALL be registered with CTest.

---

### Requirement 18: Optionality and build isolation

**User Story:** As a maintainer, I want this feature to be as skippable as
every other optional component in this tree.

#### Acceptance Criteria

1. The feature SHALL be behind a CMake option defaulting to the same posture as
   comparable optional components, and a build with it disabled SHALL produce
   the identical build graph it produces today.
2. The feature SHALL introduce **no new third-party dependency**: the listener
   uses Boost.Asio, TLS uses OpenSSL, and the log-entry encoding is
   hand-rolled binary — all already required by this project (`vcpkg.json`).
3. Header, source and test files SHALL follow this project's existing naming,
   include-ordering, copyright-header and coding-standard conventions
   (`CLAUDE.md`, `.kiro/steering/cpp-coding-standards.md`).

---

## Non-Goals

The following are explicitly out of scope, recorded so that "we did not build
that" is a decision rather than an omission:

- **Redis Cluster protocol.** sccache does expose `SCCACHE_REDIS_CLUSTER_ENDPOINTS`,
  and OpenDAL wires it to `redis-rs`'s `ClusterClient`, so cluster mode is a
  real option — but it requires `CLUSTER SLOTS`, `MOVED`/`ASK` redirection, and
  CRC16 slot hashing, which is a different routing model from Kythira's key
  ranges. The design records the mapping that would make it work (CRC16 slot as
  the routing key, so a contiguous slot range is exactly a `shard_range`); the
  implementation is deferred. Requirement 13's single-endpoint forwarding makes
  it unnecessary for the target workload.
- **Redis persistence semantics** (RDB, AOF, `BGSAVE`). Durability here is the
  Raft log and Kythira's snapshots.
- **Pub/sub, streams, scripting, transactions, modules, keyspace notifications.**
- **Redis's own eviction policies** (`maxmemory-policy`, `allkeys-lru`). This
  spec's eviction is deterministic and leader-proposed (Requirement 9); it is
  not bug-compatible with Redis's sampling LRU.
- **`SCAN`/`KEYS`.** No client in the target stack lists keys. Cache
  administration tooling would need them and is separate work.
- **Storing large values outside the Raft log.** The natural next step for this
  workload — put the blob in the object store this project already speaks to
  (S3/GCS/Azure/OSS/OCI) and keep only a digest and location in the log — is
  recorded in the design as deferred Phase 2, because it changes the durability
  story and deserves its own requirements.
- **Adopting sccache in this repository's build.** See the Introduction.
