# Requirements — CoAP under Multi-Raft

## Introduction

Multi-Raft (`.kiro/specs/multi-raft/`) routes every group over one shared
transport per node pair, demultiplexing on a `group_id` that rides inside the
serializer's framing. CoAP therefore needs **no framing change** to carry
sharded traffic — the entire multi-Raft implementation touches
`coap_transport_impl.hpp` in six lines, all of them a log statement.

What CoAP does need is the work multi-Raft's *load profile* exposes. Three
things are missing or wrong today and only become visible when one process
runs many groups over one CoAP endpoint:

- CoAP is the only wire transport without `TimeoutNow`, so leadership
  transfer — and therefore the placement driver's `transfer_leader` operator
  and every load split's `scatter` — is unavailable on it.
- One OSCORE Security Context per peer, shared by every group, has a 64-entry
  replay window that multi-shard concurrency exhausts in milliseconds.
- Duplicate detection keys on the bare 16-bit CoAP Message ID with no peer
  in the key, which is already wrong for a three-node cluster and becomes
  systematic once one Message ID counter serves every group.

Underneath all three sits a fourth fact this spec deliberately does **not**
fix: libcoap's C API is not safe to call concurrently on one context, so a
single `std::recursive_mutex` serialises every call the client makes. Sharing
one client across N groups funnels N groups through that lock. This spec
requires that effect be *measured* before anything is restructured, because
the shape of the fix (more contexts, a different I/O thread model, or
nothing) is not knowable from reading the code.

The governing constraint throughout: CoAP exists for constrained, lossy
networks, and the existing single-group behaviour is specified by
`coap-transport` and `coap-transport-security`. Nothing here may trade those
properties for multi-Raft's throughput.

## Glossary

- **Security Context** — RFC 8613 Section 3's triple of Common Context, Sender
  Context and Recipient Context, shared with exactly one peer.
- **ID Context** — RFC 8613's optional identifier mixed into key derivation and
  carried on the wire as the OSCORE option's `kid context`.
- **SSN** — Sender Sequence Number, the counter whose value becomes the Partial
  IV and, with the Common IV, the AEAD nonce.
- **Backend** — one of the three CoAP implementations: libcoap (default),
  libnyoci, cantcoap.

---

## Requirements

---

### Requirement 1: Group Routing Without a Framing Change

**User Story:** As a maintainer, I want CoAP to carry sharded traffic without
its own group-routing code, so that there is one demultiplexing mechanism in
the system rather than one per transport.

#### Acceptance Criteria

1. CoAP SHALL carry `group_id` inside the payload produced by
   `Types::serializer_type`, exactly as it carries every other message field.
   No CoAP-specific framing, URI path segment, or option SHALL encode the
   group.
2. The URI paths SHALL remain `/raft/request_vote`, `/raft/append_entries` and
   `/raft/install_snapshot`, unchanged and group-independent, so that one
   registered handler per RPC type continues to satisfy `network_server` and
   `multi_group_network_server` can do the dispatch.
3. A CoAP client and server SHALL be usable as the inner transport of
   `group_scoped_client` / `multi_group_network_server` with no changes to
   either adapter.
4. WHERE a group selector appears in CoAP logs or metrics it SHALL be for
   diagnosis only, and SHALL NOT be read back to make a routing decision.

---

### Requirement 2: Concept Conformance and Backend Parity

**User Story:** As an operator choosing a CoAP backend, I want to know which
Raft features that backend actually supports before I deploy it, not when a
placement-driver operator throws at three in the morning.

#### Acceptance Criteria

1. Each of the three backends SHALL carry `static_assert`s for
   `network_client` and `network_server`. The libcoap backend has none today
   while libnyoci and cantcoap do; the asymmetry SHALL be removed.
2. Each backend SHALL additionally `static_assert` its answer — satisfied or
   not — for every optional transport extension concept: pre-vote, log fetch,
   cluster join, cluster leave, and timeout-now.
3. The three backends' capability sets SHALL be recorded in one table in the
   design document, and a backend that does not implement an extension SHALL
   say so there rather than leaving it to be discovered at run time.
4. WHERE a backend does not satisfy an extension concept, `node<Types>`'s
   existing `if constexpr` detection SHALL remain the only consequence: the
   feature is unavailable, nothing fails to compile.
5. A conformance test per backend SHALL assert the same concept set, so that a
   signature drift in one backend fails a test rather than silently narrowing
   what that backend can do.

---

### Requirement 3: Leadership Transfer over CoAP

**User Story:** As an operator running shards over CoAP, I want load splits to
scatter and the placement driver to move leaders, both of which need
`TimeoutNow` on the wire.

#### Acceptance Criteria

1. The CoAP client SHALL implement `send_timeout_now` and the CoAP server
   SHALL implement `register_timeout_now_handler`, satisfying
   `network_client_with_timeout_now` / `network_server_with_timeout_now`.
2. The RPC SHALL use the resource path `/raft/timeout_now`, following the
   naming of the three existing paths.
3. The encoding SHALL be whatever `Types::serializer_type` produces, as for
   every other CoAP RPC; no new wire format SHALL be introduced.
4. `TimeoutNow` SHALL be sent as a confirmable message regardless of the
   heartbeat policy of Requirement 6, since it is a single rare message whose
   loss costs an election timeout.
5. The implementation SHALL cover all three backends, or SHALL record in the
   Requirement 2.3 table which backends lack it and why.
6. Verification SHALL include a wire round-trip test for the new RPC and an
   end-to-end test asserting that leadership moves to a named target over a
   real CoAP transport.

---

### Requirement 4: A Security Context Per Group

**User Story:** As an operator running many shards over OSCORE, I want each
group's traffic in its own Security Context, so that one group's message rate
cannot exhaust another group's replay window, and so that per-group sequence
numbers are safe rather than catastrophic.

#### Acceptance Criteria

1. Each (peer, group) pair SHALL have its own OSCORE Security Context, with
   its own Sender Key, Recipient Key, Common IV, Sender Sequence Number and
   replay window.
2. Contexts SHALL be derived by setting RFC 8613's **ID Context**, which the
   existing `security_context` already mixes into every `build_info()` call
   for the Sender Key, the Recipient Key and the Common IV. `oscore_credentials`
   SHALL gain an `id_context` field and the constructor SHALL assign
   `_id_context` from it; the member exists today and is never set, so every
   context in the system currently derives with an empty ID Context.
3. Per-group **counters without per-group Common IVs SHALL NOT** be used as an
   alternative. The nonce is a function of the Common IV, the Sender ID and
   the Partial IV, so two groups issuing Partial IV 1 under one Common IV and
   one Sender ID would reuse an AEAD nonce — the one catastrophic failure of
   AES-CCM. Deriving a distinct Common IV per group is what makes per-group
   counters safe, and is the reason this requirement is a per-group *context*
   and not a per-group *counter*.
4. Deriving N group contexts SHALL require exactly one bootstrap per peer
   pair. A deployment with a thousand shards SHALL NOT perform a thousand
   EDHOC handshakes or provision a thousand master secrets; every group
   context SHALL be an HKDF derivation from the single master secret already
   established for that peer.
5. The ID Context SHALL be the group id concatenated with a per-process boot
   nonce of at least 8 random bytes, generated once at startup. The boot nonce
   is required because an SSN restarts at zero after a crash: without it, a
   restarted node would reissue Partial IV 1 under a key it had already used.
   RFC 8613 Section 7.2.1 permits either persisting the SSN or establishing a
   new Security Context; persisting one counter per group per peer at Raft
   message rates is not viable, so this specification takes the second option
   and SHALL say so where the constant is defined.
6. The shard epoch SHALL NOT be used in place of the boot nonce. Epoch changes
   on split and merge, which is not the case that matters — a restart leaves
   the epoch untouched and is exactly when nonce reuse would occur.
7. Outbound messages SHALL carry the ID Context in the OSCORE option's `kid
   context` field whenever it is non-empty. The option encoder and decoder
   already support the field; only the egress path, which sets `kid` alone,
   needs to set it.
8. The recipient SHALL select the Security Context by the pair (`kid`,
   `kid context`) and SHALL derive a Recipient Context on demand for a
   `kid context` it has not seen. On-demand derivation SHALL be bounded:
   1. only for a group id this node actually hosts;
   2. under a configurable cap on live contexts per peer, with least-recently-
      used eviction;
   3. and each derivation SHALL be counted, so that an attacker forcing HKDF
      work with invented `kid context` values is visible rather than merely
      expensive.
9. A context SHALL be destroyed when its group's replica is destroyed — by
   merge, by tombstone, or by the host shutting the group down — and the
   destruction SHALL zero the key material.
10. Group ids SHALL NOT be reused, which the placement driver's allocation
    already guarantees; the design SHALL state this dependency explicitly,
    since a reused group id under a surviving boot nonce would reuse a key.
11. Where no OSCORE credentials are configured, nothing in this requirement
    SHALL take effect and the transport SHALL behave exactly as it does today.
12. The per-group replay window SHALL remain RFC 8613's 64-entry sliding
    window. Widening the window, or relaxing the replay check, SHALL NOT be
    used to address throughput; this requirement exists precisely so that it
    does not have to be.

---

### Requirement 5: Duplicate Detection Keyed by Peer

**User Story:** As a node receiving Raft RPCs from several peers, I want to
recognise a retransmission from one peer without mistaking a different peer's
message for it.

#### Acceptance Criteria

1. Duplicate detection SHALL key on the pair (peer endpoint, CoAP Message ID),
   not on the Message ID alone, in **both** copies of the logic — the client's
   and the server's — which are independent implementations of the same check.
2. This change SHALL be strictly narrowing: a message SHALL be treated as a
   duplicate only when it matches on both components, so the change can only
   convert a wrongly-dropped message into a delivered one and never the
   reverse.
3. The retention horizon SHALL be CoAP's `EXCHANGE_LIFETIME` (RFC 7252
   Section 4.8.2) rather than the present arbitrary five minutes, and the
   constant SHALL name the RFC where it is defined.
4. The existing single-peer behaviour asserted by
   `coap_duplicate_detection_property_test` SHALL continue to hold.
5. A regression test SHALL assert that two peers whose Message ID counters
   coincide — which they do from the first message, since every client starts
   at 1 — both have their requests delivered.
6. A regression test SHALL assert that a Message ID counter wrapping its
   16-bit space within the retention horizon does not cause a live message to
   be discarded, this being the failure a single shared counter across many
   groups produces within seconds.

---

### Requirement 6: Timers and Reliability Policy

**User Story:** As an existing CoAP user on a lossy constrained link, I want
multi-Raft's latency needs to arrive as an opt-in profile, not as a change to
the defaults I already depend on.

#### Acceptance Criteria

1. The shipped defaults — `ack_timeout` 2000 ms, `max_retransmit` 4,
   `use_confirmable_messages` true — SHALL NOT change. They are RFC 7252's
   values and existing tests assert behaviour built on them.
2. A named configuration profile SHALL be added for Raft-rate traffic,
   selecting shorter retransmission timers and non-confirmable heartbeats,
   and SHALL be opt-in.
3. The profile's rationale SHALL be recorded where it is defined: Raft already
   retries `AppendEntries` on its own schedule, so CON semantics duplicate a
   reliability mechanism that exists one layer up, and a 2000 ms ACK timeout
   against a 50 ms heartbeat and a 150–300 ms election timeout means the
   transport is still retransmitting a message the consensus layer abandoned
   several elections ago.
4. `InstallSnapshot` SHALL remain confirmable under every profile: it is large,
   rare, and has no cheap retry above it.
5. The profile SHALL NOT be selected implicitly by the presence of multi-Raft.
   An operator running few groups on a lossy link may reasonably want the
   defaults, and the transport is not the layer that knows which case it is in.

---

### Requirement 7: Measure the Serialisation Point Before Changing It

**User Story:** As a maintainer, I want the cost of sharing one CoAP client
across many groups quantified before anyone restructures the transport, because
a structural change here changes single-group behaviour too.

#### Acceptance Criteria

1. A benchmark SHALL drive N groups over one shared CoAP client for N in at
   least {1, 8, 64} and report, per group, the send-path latency distribution
   and the time spent waiting on the client's `_mutex`.
2. The benchmark SHALL reuse the existing `KYTHIRA_COAP_SEND_PROBE` breakdown,
   which already separates lock acquisition, address resolution, session
   acquisition, PDU construction and `coap_send`, rather than adding a second
   instrumentation scheme.
3. The results SHALL be recorded in a document under `doc/`, in the manner of
   `doc/coap-flake-investigation.md`, including the measurements that refute
   any hypothesis considered.
4. No change to the client's locking, I/O thread, or context structure SHALL
   be made under this specification until 7.1–7.3 are complete. The serialised
   `_mutex` is required by libcoap's C API, which is not safe to call
   concurrently on one context; a change here is a change to the existing
   transport's core, not an addition beside it.
5. WHERE the measurement shows the shared client is the binding constraint,
   the remedy SHALL be specified as its own work item with its own review,
   and SHALL NOT be folded into this specification's other tasks.

---

### Requirement 8: Test Hygiene

**User Story:** As a contributor whose change touches no CoAP code, I want the
CoAP suite not to fail my pull request.

#### Acceptance Criteria

1. Every test added by this specification SHALL size its Boost per-case budget
   and its CTest `TIMEOUT` through `KYTHIRA_TEST_TIMEOUT_SCALE`
   (`tests/test_timeout_scale.hpp`). Scaling one alone moves the kill from
   Boost to CTest and fixes nothing.
2. Every test added SHALL address peers by numeric endpoint, never by
   hostname. `send_rpc()` resolves inside a recursive mutex, so a name lookup
   serialises every concurrent request behind it — this is what made
   `coap_connection_reuse_property_test` flaky against names that did not
   resolve.
3. Every test added SHALL bind a port distinct from 5683, which ten existing
   CoAP test files already reference, and the port SHALL be recorded alongside
   them.
4. `doc/coap-flake-investigation.md`, including its record of what was tried
   and failed, SHALL be read before any timing-related change to a CoAP test;
   raising a timeout SHALL NOT be the first response to an unstable test.
5. Tests SHALL land in the same change as the code they cover, so that the
   coverage ratchet in `coverage_floor.txt` is satisfied by construction
   rather than by a follow-up.
6. Every container-based scenario SHALL use `container_runtime()` /
   `compose_prefix()` per the project's container-runtime rules, SHALL use no
   static IP addresses, and SHALL NOT pipe multi-process output through
   `tail`/`head`.

---

## Out of Scope

- **Restructuring the libcoap client's locking or I/O thread.** Requirement 7
  measures it and stops. Any remedy is separate work with its own review.
- **A message-batching transport extension.** `network_client_with_batch` is
  unimplemented for every transport; coalescing several groups' traffic into
  one datagram would help CoAP more than most, but it is a multi-Raft-wide
  feature and belongs to that specification.
- **Changing the shipped CoAP defaults.** Requirement 6 adds a profile beside
  them.
- **Widening or relaxing OSCORE replay protection.** Requirement 4.12 forbids
  it; the per-group context is the remedy.
- **Group-aware CoAP framing.** Requirement 1 forbids it. The demultiplexer
  reads a field the serializer already carries.
