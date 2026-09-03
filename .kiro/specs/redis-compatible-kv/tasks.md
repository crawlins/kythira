# Implementation Plan — Redis-Compatible KV on Multi-Raft (sccache Backend)

## Status: 14/14 tasks complete

**Last Updated**: September 3, 2026 — implemented September 2–3, 2026. This
spec was written from a direct reading of the client stack (sccache, OpenDAL,
redis-rs 1.2) rather than from the Redis manual; the command closure in
requirements.md Requirement 1 is the scope boundary and every task below is
measured against it.

What the acceptance run established, beyond the checkboxes: real sccache
0.10.0 against the containerised three-node cluster under rootless Podman
(`make docker-sccache-e2e-tests`, six scenarios) misses then hits, takes the
`SETEX` path under `SCCACHE_REDIS_EXPIRATION`, builds successfully with every
gateway stopped, and — for the `read_only` user — builds successfully with
`cache_write_errors` non-zero and hits from what `read_write` populated. Two
sccache facts found on the way shape the test and the operator doc
(`doc/redis-gateway.md`): sccache's startup probe turns a failed *read* into
`Server startup failed`, so a CI job must gate `RUSTC_WRAPPER` on
`sccache --start-server` for the cache to be an accelerant rather than a
dependency; and sccache 0.10 caches only the library compile (`bin` crate
types are `CannotCache`), so the runner salts the library source to choose a
fresh cache key on demand. The serializer measurement in design.md replaced
the estimate with real numbers: at 8 MiB, JSON is 1.333× the payload and
158 ms of leader CPU per follower, CBOR 1.0000× and 5 ms.

## Overview

Build the smallest Redis-compatible server that sccache's `redis` backend will
talk to, on top of the multi-Raft sharding layer that already exists. Two real
components (a RESP gateway and a splittable KV state machine) plus glue; no
changes to the consensus core; no new third-party dependency.

Reference material to read before starting:

- `.kiro/specs/redis-compatible-kv/design.md` — the component breakdown, the
  binary log-entry format, the ACL design, the error table and the config
  surface.
- `.kiro/specs/multi-raft/design.md` §5 (split), §6 (signals) and the client
  surface — this feature is a *consumer* of that host, not an extension of it.
- `include/raft/multi_raft.hpp:745-795` — `submit_command`, `read_state`,
  `resolve`; `:742` — `group_node`.
- `include/raft/raft.hpp:440-472` — `with_state_machine`'s contract, which
  constrains the read path more than anything else in this design.
- `include/raft/splittable_state_machine.hpp` — the two laws the state machine
  must obey.
- `include/raft/json_serializer.hpp:152` — why the log-entry codec is binary.
- `CLAUDE.md` — commit-message, copyright-header and container-runtime rules.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 2],
      "description": "Pure value code with no dependencies on each other: the RESP codec and the binary log-entry codec"
    },
    {
      "wave": 2,
      "tasks": [3, 4],
      "description": "The replicated state machine (needs task 2's codec) and the ACL (independent, but wave 2 keeps review batches coherent)"
    },
    {
      "wave": 3,
      "tasks": [5, 6],
      "description": "The gateway skeleton: sessions, dispatch, authn/authz enforcement (needs 1, 3, 4); then the write and read paths onto multi_raft"
    },
    {
      "wave": 4,
      "tasks": [7, 8, 9],
      "description": "Expiry/eviction maintenance, one-hop forwarding, and TLS/mTLS — independent of each other, all need wave 3"
    },
    {
      "wave": 5,
      "tasks": [10, 11],
      "description": "The daemon and its configuration, and observability"
    },
    {
      "wave": 6,
      "tasks": [12, 13, 14],
      "description": "Acceptance: real sccache end-to-end, the authorization end-to-end case, build isolation and documentation"
    }
  ]
}
```

---

- [x] 1. RESP codec (`include/raft/resp_protocol.hpp`)
  - Implement `resp_parser::consume()` returning every complete command in the
    buffer, retaining an incomplete tail — pipelining is not optional, it is
    the very first thing the client does.
  - Implement `resp_writer` for simple strings, errors, integers, bulk strings,
    the RESP2 and RESP3 null encodings, and the `HELLO` map.
  - Enforce all four limits from design.md Component 1's table, each producing
    `-ERR Protocol error: <reason>` and a connection close.
  - No sockets in this header: it takes and returns bytes so the tests need no
    network.
  - Verify: unit tests replay the literal `redis-rs` handshake as one write
    (`AUTH`, `SELECT 3`, `CLIENT SETINFO LIB-NAME`, `CLIENT SETINFO LIB-VER`)
    and assert four replies come back, in order; frames split at every byte
    boundary parse identically to the whole frame.
  - _Requirements: 1.3, 1.5, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7_

- [x] 2. Binary log-entry codec (`include/raft/redis_kv_commands.hpp`)
  - Implement encode/decode for the four opcodes in design.md Component 2
    (`set`, `del`, `sweep`, `evict`), big-endian, length-prefixed, with a
    leading format-version byte.
  - Decoding SHALL reject a truncated, over-long or unknown-version buffer with
    a distinct error rather than reading past the end.
  - Verify: round-trip property tests over random keys and values including
    empty values, zero-length keys rejected, and a corrupted-length-field case
    that must not read out of bounds.
  - _Requirements: 4.8_

- [x] 3. `redis_kv_state_machine` (`include/raft/redis_kv_state_machine.hpp`)
  - Implement `apply`, `get_state`, `restore_from_snapshot`, and every
    `splittable_state_machine` hook, over an **ordered** map of
    `std::shared_ptr<const value_entry>`.
  - `apply` reads no clock, no environment, no randomness — the determinism
    rule this repository already had to fix once in
    `distributed_lock_state_machine`.
  - `lookup()` returns a shared handle, never a copy of the value bytes: the
    caller runs under the node mutex.
  - Maintain `approximate_size_bytes` / `approximate_key_count` incrementally.
  - `can_split_at` returns `true` with the comment design.md specifies about
    why, and what a future feature would have to revisit.
  - Verify: a `BOOST_AUTO_TEST_CASE` (two-argument timeout form, per
    `.kiro/steering/test-execution-standards.md`) for each of — determinism
    across two independently built instances, `split → absorb → get_state()`
    byte equality, snapshot round trip including `expire_at_ms`, and the
    stale-sweep rejection (propose sweep, rewrite key, apply sweep, key
    survives).
  - Confirm the type satisfies both concepts with a `static_assert`.
  - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7, 8.2, 8.4, 8.6_

- [x] 4. ACL (`include/raft/redis_acl.hpp`)
  - Implement `redis_acl_user`, `redis_role`, `redis_identity`, and
    `redis_acl::{authenticate, authenticate_certificate, authorize, reload}`.
  - PBKDF2-HMAC-SHA256 via OpenSSL `PKCS5_PBKDF2_HMAC`; 16-byte salt from
    `RAND_bytes`; iteration count stored in the record; default 600,000;
    `CRYPTO_memcmp` for comparison.
  - Parse the line-oriented ACL file of design.md Component 6, `#` comments
    supported; a parse failure leaves the previous ACL in force.
  - Unknown user and wrong password take the same code path, do the same KDF
    work, and produce the same error.
  - Verify: unit tests for record round trip, role enforcement per command,
    prefix enforcement per key, empty-prefix-list means no keys, reload
    atomicity, and the no-enumeration property (same reply, comparable timing).
  - _Requirements: 10.4, 10.5, 10.6, 11.2, 11.3, 11.5, 11.6, 11.7_

- [x] 5. Gateway skeleton: sessions, dispatch, authn/authz enforcement
  - `redis_gateway<Types>` + `_impl.hpp`: a Boost.Asio acceptor, a session per
    connection holding identity, protocol version and selected db, and a
    dispatch table covering every command in Requirement 1's table.
  - Pre-authentication, permit only `AUTH`, `HELLO`, `QUIT`, `RESET`; everything
    else answers `-NOAUTH Authentication required.`
  - `AUTH` (one- and two-argument forms) and `HELLO 3 AUTH …` both establish an
    identity; `RESET` clears it.
  - Authorize **every** command, not once per connection.
  - Refuse to start with no ACL configured unless `--allow-anonymous` is given,
    and log a warning on every start when it is.
  - Rate-limit failed authentication per source address; emit a metric per
    failure; redact `AUTH`/`HELLO … AUTH` arguments from any trace.
  - Connection limits, idle timeout, bounded read buffer, bounded in-flight
    pipelined commands with read-side backpressure, graceful shutdown.
  - Verify: integration tests for the NOAUTH gate, WRONGPASS uniformity, NOPERM
    on both the command and the key axis, ACL reload reaching a live
    connection, and the max-clients rejection reply.
  - _Requirements: 10.1, 10.2, 10.3, 10.7, 10.8, 10.9, 10.10, 11.5, 11.6, 11.8, 14.1, 14.2, 14.3, 14.4, 14.5, 15.1, 15.2, 15.5_

- [x] 6. Write and read paths onto `multi_raft`
  - Writes: `SET`, `SETEX`, `DEL` through `submit_command(key, command,
    timeout)`, replying only after commit **and** apply; `SETEX` converted to
    an absolute `expire_at_ms` before proposing; `seconds <= 0` rejected without
    proposing; value-size limit checked before proposing.
  - Reads: `GET`, `EXISTS`, `STRLEN`, `GETRANGE` through
    `group_node(...)->with_state_machine(...)`, taking a shared handle under the
    lock and writing bytes after releasing it. `read_state()` is not used —
    add a comment saying why, since the wrong choice is the obvious one.
  - Implement the three read-consistency modes; default `leader`.
  - `GETRANGE` implements Redis's inclusive-end and negative-index clamping,
    because OpenDAL derives its indices from a `STRLEN` it just issued.
  - Implement the immutable-value rule (default on): identical bytes are a
    no-op `+OK`, different bytes are `-ERR value conflict for an existing key`
    plus a counter.
  - Map every Kythira shard exception per design.md Component 8's table.
  - Verify: integration tests against a small in-process cluster for hit, miss,
    delete, oversize rejection, value conflict, identical-rewrite no-op (the
    `.sccache_check` case), and each consistency mode.
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 6.1, 6.2, 7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7, 8.1, 11.4, 15.3, 15.4_

- [x] 7. Expiry and eviction maintenance
  - Leader-side sweep on the existing policy tick: bounded batch, proposes
    `sweep{(key, expire_at_ms), …}`; no new thread.
  - Leader-local advisory LRU (never replicated) feeding an `evict{key, …}`
    proposal when a shard is over `max_shard_bytes` / `max_shard_keys`.
  - Over budget is a metric and a log line, never a write refusal.
  - Implement `TTL` returning `-1` / `-2` per Redis semantics.
  - Verify: a key set with a short TTL becomes invisible on read before any
    sweep runs; the sweep then removes it and `approximate_size_bytes` drops;
    eviction reclaims to budget; a key rewritten mid-sweep survives.
  - _Requirements: 8.3, 8.5, 8.7, 9.1, 9.2, 9.3, 9.4, 9.5, 9.6_

- [x] 8. One-hop forwarding
  - Forward a command whose owning shard's leader is elsewhere to that node's
    gateway; relay the reply verbatim.
  - Enforce one hop: a command that arrived on an internal connection is never
    forwarded again.
  - Internal connections authenticate as the internal identity, are pooled per
    peer, carry the client's remaining deadline, and are marked in the audit
    log.
  - Resolve peer endpoints from existing membership/peer-discovery data.
  - Verify: a three-node cluster answers every key on every node; killing the
    leader of one shard produces a retryable error and then recovery; a
    deliberately poisoned routing map does not produce a forwarding loop.
  - _Requirements: 12.5, 13.1, 13.2, 13.3, 13.4, 13.5, 13.6_

- [x] 9. TLS and mTLS
  - TLS listener so `rediss://` works, certificate material either configured
    or provisioned through the existing certificate machinery; TLS 1.2 floor.
  - Optional required client certificate, with SAN/CN mapped to an ACL user via
    `cert_subjects`, subject to the same authorization rules.
  - Log once at startup which listeners are plaintext.
  - Verify: sccache with a `rediss://` endpoint completes a build; an mTLS-only
    listener refuses a client with no certificate and admits one whose subject
    maps to a user; a client presenting a certificate for a disabled user is
    refused.
  - _Requirements: 12.1, 12.2, 12.3, 12.4, 12.6_

- [x] 10. Daemon and configuration (`cmd/redis_gateway_node/`)
  - `from_env()` configuration following `cmd/chaos_node/config.hpp`'s
    convention, covering design.md Component 9's table, with a clear error
    naming the missing variable.
  - A `--hash-secret` subcommand that emits an ACL secret record, so no
    operator workflow requires writing a plaintext secret anywhere.
  - Signal handling and graceful shutdown matching the existing daemons.
  - Verify: the daemon starts against a local multi-Raft cluster, serves
    `redis-cli ping` and `redis-cli info`, and refuses to start with no ACL.
  - _Requirements: 10.1, 14.5, 18.3_

- [x] 11. Observability
  - Metrics through the `metrics` concept with a `command` dimension, covering
    the full list in Requirement 16.1.
  - `INFO` reporting version, uptime, clients, local shards, key count,
    approximate bytes.
  - Audit stream separable from operational logging; per-command logging off by
    default.
  - Verify: a scripted workload produces non-zero hit, miss, denial and
    eviction counters; `redis-cli info` output is human-readable; no metric or
    log line contains a secret or a value.
  - _Requirements: 16.1, 16.2, 16.3, 16.4_

- [x] 12. Real-sccache acceptance test
  - Compose file and harness running a real `sccache` against a real gateway:
    compile a small Rust crate twice from a clean target directory, assert zero
    hits then non-zero hits via `sccache --show-stats`.
  - The negative case in the same test: with the gateway stopped, both builds
    still succeed. This is what proves the cache is an accelerant, not a build
    dependency.
  - Exercise both `SET` (no expiration configured) and `SETEX`
    (`SCCACHE_REDIS_EXPIRATION` set) paths, and a non-empty
    `SCCACHE_REDIS_KEY_PREFIX`.
  - Container-runtime rules: no static IPs, service-name addressing,
    `container_runtime()` / `compose_prefix()`, no privileged or host-network
    features; run the suite under both Docker and rootless Podman.
  - _Requirements: 17.1, 17.2, 17.4_

- [x] 13. Authorization acceptance test
  - `ci-main` (`read_write`) populates the cache; `ci-pr` (`read_only`) gets
    hits from it and is refused a write with `-NOPERM`; a user scoped to a
    different prefix is refused both.
  - Assert the sccache-visible consequence, not just the wire error: the
    read-only runner's build succeeds, its `cache_write_errors` is non-zero, and
    its hit count is non-zero.
  - Assert the audit line exists for each denial and contains no secret.
  - _Requirements: 11.1, 11.2, 11.3, 11.8, 17.3_

- [x] 14. Build isolation, serializer measurement, and documentation
  - CMake option gating the feature; verify the ON/OFF target lists match the
    method already used for the stdexec backend and for ccache.
  - Confirm no new third-party dependency entered `vcpkg.json`.
  - Measure the log/AppendEntries volume for a representative value size under
    `json_serializer` versus `cbor_serializer` and record the real numbers in
    design.md's serializer section, replacing the estimate there.
  - Document the operator-facing surface: the env-var table, the ACL file
    format, the matching sccache client configuration, and the explicit
    statement that `SCCACHE_REDIS_RW_MODE` is a client-side convenience and the
    server-side role is the control.
  - _Requirements: 6.3, 6.4, 6.5, 18.1, 18.2, 18.3_
