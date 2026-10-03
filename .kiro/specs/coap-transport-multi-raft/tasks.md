# Implementation Plan — CoAP under Multi-Raft

## Status: In progress — tasks 1–6 and 8–14 done; 7 implemented, unverified

**Last Updated**: October 2, 2026

This plan implements `.kiro/specs/coap-transport-multi-raft/design.md`. Seven
phases, 14 tasks. Phases 1–2 fix defects that exist today and are independent
of multi-Raft. Phase 3 is a gate: no test in the repository has ever run
`node<Types>` over a CoAP backend, and phases 4–6 have nothing to stand on
until one does.

## Overview

Group routing over CoAP needs no code — the demultiplexer reads a field the
serializer already carries, which is why the entire multi-Raft implementation
touches CoAP in six lines of logging. This plan covers what multi-Raft's load
profile *exposes*: a duplicate-detection key that omits the peer, a missing
`TimeoutNow`, and one OSCORE Security Context per peer whose 64-entry replay
window many groups exhaust in milliseconds.

One thing this plan deliberately does not do is restructure the libcoap
client's locking. Task 14 measures it and stops. The mutex is required by
libcoap's C API, so any remedy changes single-group behaviour too and earns
its own review.

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 1, "tasks": [1, 2], "description": "Concept parity across the three backends — no dependencies" },
    { "wave": 2, "tasks": [3, 4], "description": "Duplicate detection rekeyed and bounded" },
    { "wave": 3, "tasks": [5], "description": "GATE: the first node-over-CoAP integration test" },
    { "wave": 4, "tasks": [6, 7], "description": "TimeoutNow on the backends" },
    { "wave": 5, "tasks": [8, 9, 10, 11], "description": "Per-group OSCORE Security Contexts" },
    { "wave": 6, "tasks": [12, 13], "description": "Raft-rate timer profile and multi-Raft over CoAP" },
    { "wave": 7, "tasks": [14], "description": "Concurrency measurement and its record" }
  ]
}
```

---

## Tasks

---

## Phase 1: Concept Parity (Tasks 1–2)

- [x] 1. `static_assert` every backend against the same concept set
  - Add `network_client` / `network_server` assertions to the **libcoap**
    backend, which has none today while the two opt-in backends do — the
    default backend is currently the one whose conformance is unproven.
  - Add an assertion per optional extension to all three backends, including
    the **negative** ones (`static_assert(!network_client_with_pre_vote<…>)`)
    with a message pointing at design §2's table. A backend quietly gaining or
    losing an extension changes which multi-Raft features work on it, and that
    belongs in a build failure rather than a 3 a.m. runtime exception.
  - Fill in design §2's capability table and keep the assertions and the table
    in agreement.
  - Verify: the three assertion blocks compile; flipping any one of them to
    the wrong polarity fails the build (check by temporary edit, not by faith).
  - _Requirements: 2.1, 2.2, 2.3, 2.4_

- [x] 2. A conformance test per backend
  - One test per backend asserting the identical concept set, so a signature
    drift in one fails a test rather than silently narrowing that backend.
  - Follow the shape of the existing `coap_libnyoci_concept_conformance_test`
    and `coap_cantcoap_concept_conformance_test`; the libcoap one is the gap.
  - The tests must hold in a build where that backend's library is absent —
    the existing libnyoci test already documents this pattern.
  - Verify: three tests, each asserting client, server, and every extension in
    both directions.
  - _Requirements: 2.1, 2.2, 2.5, 8.1, 8.3_

---

## Phase 2: Duplicate Detection (Tasks 3–4)

- [x] 3. Key duplicate detection on (peer endpoint, Message ID)
  - `is_duplicate_message()` / `record_received_message()` exist twice — the
    client's copy at coap_transport_impl.hpp:1778 and the server's at `:3482`,
    the one on the request path — and both key on the bare Message ID today.
    Fix both; they are separate implementations of the same check.
    Every client starts at `_next_message_id{1}`, so in any cluster past two
    nodes two peers send Message ID 1 and the second is answered `2.03 Valid`
    with no payload.
  - Replace the arbitrary five-minute retention with RFC 7252 Section 4.8.2's
    `EXCHANGE_LIFETIME`, naming the RFC where the constant is defined.
  - The change is **strictly narrowing** — a duplicate now requires a match on
    both components — so it can only convert a wrongly-dropped message into a
    delivered one. Keep it that way; it is what makes this safe to land before
    the integration test of task 5 exists.
  - Verify: `coap_duplicate_detection_property_test` still passes unchanged
    (single-peer behaviour is not affected); a new case with two peers whose
    counters both start at 1 asserts both peers' requests are delivered; a new
    case wraps a counter through the 16-bit space inside the retention horizon
    and asserts no live message is discarded.
  - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 8.1, 8.2, 8.3_

- [x] 4. Bound the exchange table
  - `EXCHANGE_LIFETIME` is 247 s, and a process leading N groups emits roughly
    `40N` Message IDs per second per peer, so a plain map with a TTL sweep may
    not be the right structure — measure before assuming it is.
  - If it is not, replace it with a bounded structure over the recent window
    rather than raising or lowering the horizon, which is a protocol constant.
  - Verify: memory held by the table at N = 1, 8 and 64 groups, recorded in the
    task's commit message; the two regression cases from task 3 still pass
    against whatever structure replaces the map.
  - _Requirements: 5.3, 7.1_

---

## Phase 3: The Gate (Task 5)

- [x] 5. `node<Types>` over a real CoAP transport — the first one
  - No test in the repository instantiates a Raft node over any CoAP backend.
    The ~50 CoAP tests exercise the transport standalone: round-trips, DTLS,
    OSCORE vectors, block transfer.
  - A three-node cluster over the libcoap backend that elects a leader,
    replicates entries, and survives a leader kill.
  - Expect this to find something. It is the first time the transport meets
    the node's timing and concurrency assumptions, and everything in phases
    4–6 depends on it.
  - Numeric endpoints, a port that is not 5683 (ten CoAP test files already
    reference it), and both the Boost budget and
    the CTest `TIMEOUT` sized through `KYTHIRA_TEST_TIMEOUT_SCALE`. Read
    `doc/coap-flake-investigation.md` first, including "What was tried and
    failed"; raising a timeout is not the first response to instability.
  - Verify: election, replication, and leader-failover assertions, run under
    the Coverage profile as well as Release, since that is the profile the
    historic flake lived in.
  - _Requirements: 1.3, 8.1, 8.2, 8.3, 8.4, 8.5_

---

## Phase 4: Leadership Transfer (Tasks 6–7)

- [x] 6. `TimeoutNow` on the libcoap backend
  - `send_timeout_now` on the client and `register_timeout_now_handler` on the
    server, satisfying `network_client_with_timeout_now` /
    `network_server_with_timeout_now`; resource path `/raft/timeout_now`.
  - Always confirmable, whatever timer profile is in force (task 12): one rare
    message whose loss costs a full election timeout is the one case where
    CoAP's own retransmission earns its keep.
  - Encoding is `Types::serializer_type`'s, as for every other CoAP RPC. The
    serializers already carry this RPC: `feat(raft): carry TimeoutNow on every
    wire transport` added it to CBOR, Ion and protobuf, and JSON already had
    it. Only the transports were left out.
  - Update task 1's assertions from negative to positive for this extension,
    and design §2's table with them.
  - Verify: wire round-trip through each serializer; an end-to-end test over a
    real CoAP transport asserting leadership moves to a named target within
    one election timeout without an intervening term bump on a third node.
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.6, 2.2_

- [ ] 7. `TimeoutNow` on libnyoci and cantcoap, or a recorded gap
  - Both are opt-in backends that own their own reliability and block-wise
    machinery, so this is not one-third of task 6 each.
  - If either does not ship it, design §2's capability table and that
    backend's negative `static_assert` must say so — an unimplemented
    extension is not a defect, an undocumented one is.
  - Verify: for each backend that ships it, the same two tests as task 6; for
    each that does not, an assertion that the extension concept is *not*
    satisfied and a table row saying why.
  - **Status (October 2, 2026):** shipped on both, asserted positive, with a
    round-trip, CON-on-the-wire and 5.01 case in each backend's integration
    test. Those cases compile but have not run: both backends bind AF_INET6
    sockets, the machine they were written on has no IPv6, and CI builds
    neither backend. The node-over-CoAP leadership-transfer test exists for
    libcoap only. Tick this once both integration tests have passed on a host
    with IPv6.
  - _Requirements: 3.5, 2.2, 2.3_

---

## Phase 5: Per-Group Security Contexts (Tasks 8–11)

- [x] 8. Wire up the ID Context that the key schedule already consumes
  - `security_context::_id_context` (oscore.hpp:1156) is fed into all three
    HKDF derivations — Sender Key, Recipient Key, Common IV — and is never
    assigned. Add `id_context` to `oscore_credentials`
    (coap_security.hpp:96) and assign it in the constructor.
  - Set `fields.kid_context` / `has_kid_context` on egress when the ID Context
    is non-empty; the egress path sets `kid` alone today, while the option
    encoder and decoder have supported the field all along.
  - Where no ID Context is supplied, the derivation and the wire bytes must be
    **byte-identical to today's**. That is what keeps this additive for every
    existing deployment.
  - Verify: an RFC 8613 test vector with a non-empty ID Context derives the
    published keys; an empty ID Context reproduces the existing vectors
    unchanged (`oscore_rfc8613_vectors_test`); a round-trip asserts
    `kid context` appears on the wire when set and is absent when not.
  - _Requirements: 4.2, 4.7, 4.11_

- [x] 9. Derive a context per (peer, group)
  - `id_context = group_id_bytes || boot_nonce`, the boot nonce being at least
    8 random bytes generated once per process.
  - The boot nonce is the load-bearing part: the SSN lives in memory and
    restarts at zero, so without it a restarted node reissues Partial IV 1
    under a key it has already used. RFC 8613 Section 7.2.1 allows either
    persisting the SSN or establishing a new context; persisting one counter
    per group per peer on the path of every heartbeat is not viable. Put that
    reasoning where the constant is defined — the cheap-looking alternative is
    the dangerous one.
  - Do **not** use the shard epoch instead: it bumps on split and merge, not
    on restart, which is the case that matters.
  - One master secret per peer pair; every group context is an HKDF derivation
    from it. A thousand shards must cost a thousand HKDF calls, not a thousand
    handshakes.
  - Verify: two groups against one peer derive different Sender Keys,
    Recipient Keys and Common IVs from one master secret; a simulated restart
    yields a different ID Context and therefore different keys; a test asserts
    exactly one bootstrap occurs for N groups.
  - _Requirements: 4.1, 4.3, 4.4, 4.5, 4.6, 4.10_

- [x] 10. Recipient-side selection and bounded on-demand derivation
  - Select the context by (`kid`, `kid context`); derive a Recipient Context on
    first sight of an unknown `kid context`, under three bounds: only for a
    group this node hosts, under a configurable per-peer cap with LRU
    eviction, and counting every derivation.
  - All three bounds are required for the same reason: without them anyone who
    can reach the port forces unbounded HKDF work and unbounded memory with
    invented `kid context` values.
  - Eviction is LRU with a TTL so that a peer restarting under a new boot
    nonce does not have its in-flight messages under the old context dropped
    the instant the new one appears.
  - Verify: a `kid context` naming an unhosted group is rejected with no
    derivation performed; exceeding the cap evicts least-recently-used and not
    the newest; the derivation counter increments per on-demand derivation;
    a peer restart with in-flight old-context messages loses none of them
    inside the TTL.
  - _Requirements: 4.8, 4.9_

- [x] 11. Lifecycle, zeroization, and the replay-window property
  - Destroy a group's context when its replica is destroyed — merge,
    tombstone, or host shutdown — zeroing key material rather than letting it
    fall out of a map. Split creates a context lazily at first message.
  - Verify: the property this whole phase exists for — one group driven at a
    rate that would exhaust a shared 64-entry replay window does not cause any
    other group's messages to fail verification. Assert the window stays at
    RFC 8613's 64 entries: widening it or relaxing the replay check is
    forbidden by Requirement 4.12, and this test is what makes that
    unnecessary.
  - Verify also: a merged-away group's key material is zeroed; a group id is
    never reused (assert against the placement driver's allocator, since this
    design depends on it).
  - _Requirements: 4.9, 4.10, 4.12, 4.1_

- **Backend wiring (October 2, 2026), tasks 9–11.** The registry's own
  properties are pinned by `oscore_group_contexts_test`; the libcoap backend
  now carries them on the wire, checked by `coap_oscore_group_libcoap_test`.
  Turning it on is `oscore_groups.enabled` on the client and server config;
  a server must also supply `hosts_group`, or construction fails, since
  without it any invented `kid context` would be derived.
  - **libcoap is built without its own OSCORE.** libcoap with OSCORE compiled
    in decrypts every request carrying the OSCORE option inside
    `coap_dispatch()`, before any handler, and drops it when none of its fixed
    contexts matches. 4.3.5 has no hook to derive one for an unknown
    `kid context` (`coap_oscore_register_external_handlers()` is on
    `develop` only). `vcpkg-overlays/libcoap` builds it with
    `-DENABLE_OSCORE=OFF`, and the backend now protects and verifies with
    Kythira's own `raft/oscore.hpp`, as libnyoci and cantcoap already do. In
    OSCORE mode it refuses to start against a libcoap that has OSCORE
    compiled in rather than lose every request.
  - **Sender contexts are per group, not per (peer, group).** The provider
    has one credential set for every peer, so per-peer sender contexts would
    reuse (key, nonce) pairs across peers. One registry key stands in for all
    peers; a node's boot nonce still separates its contexts from every other
    node's, and the server checks `kid` against its recipient id before
    deriving anything.
  - **Group consistency.** The server answers 4.01 when the decoded RPC's
    `group_id` differs from the group of the context that verified it, so a
    member of group A cannot speak for group B under A's keys.
  - **Lifecycle.** `multi_raft_config::on_group_destroyed` fires after a
    replica is torn down, by `destroy_group()` or a merge commit, and the
    test wires it to `forget_oscore_group()` on both client and server.
    `multi_raft::is_member_of()` backs `hosts_group`, so a split child's
    first message is accepted once the split entry has applied.

---

## Phase 6: The Whole Thing (Tasks 12–13)

- [x] 12. A Raft-rate timer profile, opt-in
  - `raft_rate_profile()` returning a `coap_client_config` with shorter
    retransmission timers and non-confirmable heartbeats.
  - **Change no default.** `ack_timeout` 2000 ms, `max_retransmit` 4 and
    `use_confirmable_messages` true are RFC 7252's values, existing tests
    assert behaviour built on them, and the lossy constrained link CoAP exists
    for is where they are correct.
  - `InstallSnapshot` stays confirmable under every profile: large, rare, no
    cheap retry above it.
  - Not selected implicitly by multi-Raft's presence — "many groups" and
    "reliable link" are independent facts and the transport cannot tell which
    case it is in.
  - Verify: the profile's values differ from the defaults; the defaults are
    unchanged (assert the struct's defaults directly, so a later edit to them
    fails here); `coap_confirmable_message_property_test` passes unchanged;
    an `InstallSnapshot` under the profile is still confirmable.
  - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

- [x] 13. Multi-Raft over CoAP, end to end
  - Several groups across a three-node cluster over one shared CoAP client per
    node, asserting per-group isolation: each group's handler sees only its own
    traffic, and a split child's messages never reach its parent's handler.
  - Run it with OSCORE enabled so phases 5 and 6 are exercised together — the
    per-group contexts under real concurrency are the point.
  - Numeric endpoints, a distinct port, `KYTHIRA_TEST_TIMEOUT_SCALE` on both
    budgets, and no static IPs in any container scenario.
  - Verify: per-group isolation; a split and a merge complete over CoAP; a
    load split scatters (which needs task 6's `TimeoutNow`); the OSCORE
    derivation counter shows one context per (peer, group) and no more.
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 4.1, 8.1, 8.2, 8.3, 8.6_
  - **Progress (October 2, 2026):** `tests/multi_raft_coap_test.cpp` runs
    two groups over one shared libcoap client and server per host and passes
    isolation, split, merge and scatter. A fifth case,
    `a_split_and_merge_complete_under_per_group_oscore`, runs a split and a
    merge with per-group OSCORE on and checks every host derived at most one
    sender context per group and one recipient context per (peer, group),
    and that the merged-away group's keys were wiped. Standing it up found
    three defects, two fixed here:
    - **Fixed:** the libcoap client opened a new session (a new UDP socket)
      for every RPC and never released it; multi-Raft exhausted a
      20,000-descriptor limit in seconds. Sessions are now pooled per peer,
      with NSTART raised to `max_concurrent_requests` so a shared session
      does not serialize Raft behind RFC 7252's one-outstanding-CON default.
    - **Fixed:** `node<Types>` sent InstallSnapshot by blocking on each
      chunk's future, from inside an AppendEntries reply. On libcoap that
      reply runs on the client's I/O thread, so the first snapshot froze the
      client for every peer and every group. The transfer is now a chain of
      continuations, one in flight per follower.
    - **Fixed upstream, now asserted here:** after a split, writes to the
      *non-derived* child never committed, on the in-process fabric as much
      as on CoAP. Its replicas start from a snapshot with an empty log, and
      neither side of AppendEntries treated `prevLogIndex` equal to the
      snapshot's last included index as matching, so the leader re-sent the
      snapshot forever. `fix(raft): match on the snapshot boundary so split
      children replicate` (elastic shard capacity) fixed it in Raft core;
      the split case here now writes to both children and checks each lands
      only in its own.

---

## Phase 7: Measurement (Task 14)

- [x] 14. Quantify the shared client, and change nothing
  - Drive N groups over one shared CoAP client for N in {1, 8, 64}; report per
    group the send-path latency distribution and the time spent waiting on the
    client's `_mutex`.
  - Reuse the existing `KYTHIRA_COAP_SEND_PROBE` breakdown, which already
    separates lock acquisition, address resolution, session acquisition, PDU
    construction and `coap_send`. Do not add a second instrumentation scheme.
  - Run it as the **CoAP row of the shared matrix** in
    `.kiro/specs/multi-raft-performance/` (Requirement 17a there), not as a
    second harness: same key/value payload, same group-count sweep, same
    measurement-only rule, so the CoAP answer can be set beside the cpp-httplib
    and Beast ones instead of standing alone. Sequence it after this
    specification's transport tasks — the row consumes `multi_raft` over CoAP
    and cannot precede it.
  - Record the results in that matrix's own report
    (`doc/multi_raft_performance_comparison.md`), **including the hypotheses the
    data refutes** — that file exists because this investigation had been attempted
    several times from analysis alone, and each attempt produced a plausible
    diagnosis the data later contradicted.
  - Make **no** change to the client's locking, I/O thread, or context
    structure under this specification. The serialised `_mutex` is required by
    libcoap's C API — it is not safe to call concurrently on one context — so
    any remedy changes single-group behaviour too and earns its own review.
  - Verify: the benchmark runs at all three N values and the document exists
    with its measurements and its refuted hypotheses.
  - **Done (October 2, 2026).** `coap_transport` is a fixture in
    `tests/multi_raft_transport_harness.hpp` and `bench_rows/coap_json.cpp` the
    row; `write_latency_by_group_count` sweeps 1/8/64 groups beside
    cpp-httplib with per-group latency; the existing probe gained `group=`
    and microsecond fields and `scripts/coap-send-probe-summary.py` reads it.
    Results and five hypotheses in `doc/multi_raft_performance_comparison.md`,
    "The CoAP row". The short version: the `_mutex` is **not** the binding
    constraint (median wait 0 µs in every cell, tail not growing with N); the
    client's 5 ms reply pacing is, at every group count on the standard 2 ms
    tick; and one group is the worst case, because its batches go
    block-wise. Nothing in the client was changed.
  - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5_

---

## Notes

**Why the gate is where it is.** Task 5 is the first time a Raft node has ever
run over CoAP in this repository. Tasks 6–13 all assume it works. Sequencing it
before them is not caution, it is the difference between finding a transport
problem in a two-node test and finding it inside a multi-group OSCORE
integration test where four new mechanisms could each be the cause.

**Why tasks 3 and 8 are safe to land early.** Both are strictly narrowing.
Task 3's new duplicate key can only turn a wrongly-dropped message into a
delivered one. Task 8's ID Context is byte-identical to today's behaviour when
no ID Context is supplied, which is every existing deployment. Neither needs
the integration test to be trusted, which is why they sit ahead of it.

**The one thing not to do.** If phase 5 runs late or the per-group contexts
prove awkward, the shortcut that will suggest itself is widening OSCORE's
64-entry replay window or relaxing the replay check. That trades an RFC 8613
security property for throughput, permanently, in a file four specifications
depend on. Requirement 4.12 forbids it and task 11's test is what makes the
forbidding costless.

**Where the risk concentrates.** Three places, in order:
1. Task 5, because it is unexplored ground and everything depends on it.
2. Task 9's boot nonce. Nonce reuse under AES-CCM is the one failure an AEAD
   does not survive, and the mistake it guards against — per-group counters on
   a shared Common IV — looks like the simpler design right up until it is a
   confidentiality breach.
3. Every new test's timing budget. The CoAP suite has blocked merges of
   unrelated changes before, at a 20% CI pass rate, and the remedy is applied
   at the moment a test is written, not after it becomes flaky.
