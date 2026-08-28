# Design — CoAP under Multi-Raft

## Overview

Multi-Raft demultiplexes on a field the serializer already carries, so the
transport below it never learns that groups exist. That is the whole reason
this document is short in its first half: the routing work is *already done*,
and what remains is a set of defects and gaps that only a many-group workload
makes visible.

The document is organised by how much of the existing CoAP implementation each
change touches, least first:

| § | Change | Touches |
|---|---|---|
| 1 | Group routing | nothing — proof, not code |
| 2 | Concept parity | `static_assert`s in three headers |
| 3 | `TimeoutNow` | new methods beside the existing three |
| 4 | Per-group OSCORE contexts | `oscore.hpp` derivation and egress |
| 5 | Duplicate detection | one map key and one constant |
| 6 | Timers | a new profile, no default changed |
| 7 | The shared client's mutex | **measurement only** |

---

## 1. Group routing: what is already true

`coap_client::send_rpc()` serialises the whole request with
`_serializer.serialize(request)` (coap_transport_impl.hpp:393) and the server
deserialises it whole (`:7380`, `:7415`, `:7450`). `group_id` is a field on
those structs, so it round-trips with everything else.

The evidence is the multi-Raft implementation itself: across seventeen commits
and fifteen thousand added lines, its only change to any CoAP file is six
lines adding `group_id` to one log statement, whose comment states the
principle:

> the multi-Raft group selector rides inside the serializer's framing, not this
> transport's own

So `multi_group_network_server` can wrap a CoAP server unmodified, the three
`/raft/*` paths stay group-independent, and Requirement 4.6 of the multi-Raft
specification — one connection per node pair shared by every group — is
satisfied by the endpoint map `coap_client` already takes at construction.

What this section produces is a test, not a change: a CoAP client and server
driving three groups through `group_scoped_client` and
`multi_group_network_server`, asserting each group's handler sees only its own
traffic. It exists to keep the property true, not to make it true.

---

## 2. Concept parity across the three backends

Today the libcoap backend — the default, `CONFIG_COAP_TRANSPORT_LIBCOAP=y` —
is the only one with **no** `network_client` / `network_server`
`static_assert`, while the two opt-in backends have them. That is backwards,
and it means the transport most deployments use is the one whose conformance
is unproven at compile time.

Each backend gets the same block, listing satisfied and unsatisfied extensions
explicitly:

```cpp
static_assert(kythira::network_client<coap_client<coap_kythira_types>>);
static_assert(kythira::network_server<coap_server<coap_kythira_types>>);
static_assert(kythira::network_client_with_timeout_now<coap_client<coap_kythira_types>>);
static_assert(!kythira::network_client_with_pre_vote<coap_client<coap_kythira_types>>,
              "CoAP does not implement pre-vote; see design §2's table. If this "
              "fires, the table is stale, not the assertion.");
```

Asserting the *negative* is deliberate. A backend that quietly gains or loses
an extension changes which multi-Raft features work on it, and that should
fail a build rather than surface as
`leader_transfer_unsupported_exception` on a production node at 3 a.m.

The capability table, kept in this section and mirrored by the assertions:

| | libcoap | libnyoci | cantcoap |
|---|---|---|---|
| `network_client` / `network_server` | yes (asserted by this work) | yes | yes |
| pre-vote | no | no | no |
| log fetch | no | no | no |
| cluster join / leave | no | no | no |
| timeout-now | **added here** | **added here** | **added here** |

Absent extensions are not a defect: `node<Types>` detects them with
`if constexpr` and does without. They are only a defect when undocumented.

---

## 3. `TimeoutNow` over CoAP

Mechanically the smallest piece, and a direct transcription of what
`feat(raft): carry TimeoutNow on every wire transport` did for gRPC,
`tcp_rpc` and `tls_tcp_rpc` — the commit that added `TimeoutNow` to "every
wire transport" and left all three CoAP backends out. As of this writing the
string `timeout_now` does not appear in any of them.

```cpp
auto send_timeout_now(std::uint64_t target, const timeout_now_request<>& request,
                      std::chrono::milliseconds timeout)
    -> future_template<timeout_now_response<>> {
    return send_rpc<timeout_now_request<>, timeout_now_response<>>(
        target, "/raft/timeout_now", request, timeout, force_confirmable);
}
```

Two decisions worth stating:

- **Always confirmable**, whatever Requirement 6's profile says about
  heartbeats. `TimeoutNow` is one rare message and losing it costs a full
  election timeout, so the one case where CoAP's own retransmission is worth
  paying for is exactly this one.
- **A path, not an option.** `/raft/timeout_now` follows the three existing
  paths so the server's dispatch table stays uniform, and so a packet capture
  reads the same way it does for the other RPCs.

Without this, the placement driver's `transfer_leader` operator and every load
split's `_scatter_children` throw on a CoAP deployment. A load split whose
children's leaders both stay on the machine that was already hot has, per the
multi-Raft specification's own words, accomplished nothing.

---

## 4. A Security Context per group

### 4.1 The problem, stated precisely

`security_context::check_and_record_replay()` (oscore.hpp:1079) is RFC 8613
Section 7.4's sliding window, implemented as a 64-bit bitmap:

```cpp
const auto age = _replay_high - sequence;
if (age >= 64) throw verification_error("Partial IV is older than the replay window");
```

Sixty-four is generous for one Raft group, whose messages to a peer are
pipelined and near-ordered. It is not generous for one context carrying every
group's traffic: with groups interleaved and reordered independently, a
sequence number that is 64 places behind the highest seen is ordinary at
multi-shard rates, and legitimate traffic starts failing verification.

### 4.2 The fix that must not be used

The tempting repair is a Sender Sequence Number per group on the shared
context. It is catastrophic. The nonce is a function of the Common IV, the
Sender ID and the Partial IV (oscore.hpp:887). One Common IV and one Sender ID
with two independent counters means two groups both issue Partial IV 1, and
AES-CCM is used twice under one key with one nonce. That is the single failure
mode an AEAD does not survive, and it converts a throughput complaint into a
confidentiality breach.

Per-group counters are safe only alongside per-group Common IVs — which is
precisely what a per-group Security Context provides, and why the remedy is a
context and not a counter.

### 4.3 The mechanism is already present

RFC 8613 Section 3.2 derives Sender Key, Recipient Key and Common IV from the
Master Secret and Master Salt through an `info` structure that includes the
**ID Context**. Kythira's `security_context` already does exactly this:

```cpp
_sender_key    = hkdf_sha256(salt, secret, build_info(_sender_id,   _id_context, _alg, "Key", 16), 16);
_recipient_key = hkdf_sha256(salt, secret, build_info(_recipient_id,_id_context, _alg, "Key", 16), 16);
_common_iv     = hkdf_sha256(salt, secret, build_info({},           _id_context, _alg, "IV",  13), 13);
```

`_id_context` is a member (oscore.hpp:1156), it is fed into all three
derivations — and **it is never assigned**. `oscore_credentials`
(coap_security.hpp:96) has no `id_context` field, so every Security Context in
the system today derives with an empty ID Context.

The wire half is likewise half-built. `option_fields` carries `kid_context`
and `has_kid_context`, the encoder emits them (oscore.hpp:434) and the decoder
parses them (`:476`) — but egress sets `fields.kid = _sender_id` and nothing
else, so no message Kythira sends has ever carried a `kid context`.

So this design adds no cryptography. It assigns a field that the key schedule
already consumes, and sets an option field the codec already supports.

### 4.4 What the ID Context contains

```
id_context = group_id_bytes || boot_nonce      // boot_nonce: >= 8 random bytes
```

**Why the group id** is obvious: it makes the derivation per-group, which is
the entire point.

**Why a boot nonce** is not obvious and is the load-bearing part. The Sender
Sequence Number lives in memory (`_sender_sequence`, oscore.hpp:1074) and
starts at zero. A node that crashes and restarts would reissue Partial IV 1
under a key it had already used — nonce reuse again, this time across an
incarnation boundary. RFC 8613 Section 7.2.1 offers exactly two remedies:
persist the SSN, or establish a new Security Context. Persisting one counter
per group per peer, durably, on the path of every outbound Raft message, is
not viable at a 50 ms heartbeat; a fresh ID Context per process incarnation
costs one HKDF per group per peer at first use and nothing thereafter. The
constant's definition carries this reasoning, because the cheap-looking
alternative is the dangerous one.

**Why not the shard epoch.** Epoch bumps on split and merge (multi-Raft design
§1.2), which is not the case that needs covering: a restart leaves the epoch
exactly where it was, and a restart is when nonce reuse happens.

**Why group ids may not be reused.** The placement driver allocates shard ids
and never reissues them. This design depends on that: a recycled group id
under a surviving boot nonce would re-derive a key that has already been used
with a counter that has restarted. The dependency is stated here because it is
invisible from inside `oscore.hpp`.

### 4.5 Bootstrap cost

One master secret per peer pair, however it was established — statically
provisioned or through EDHOC — and every group context is an HKDF derivation
from it. A thousand shards is a thousand HKDF invocations, not a thousand
handshakes. This is the property that makes the design usable at multi-Raft
scale, and Requirement 4.4 pins it so that a later "simplification" toward
per-group provisioning is recognisable as a regression.

### 4.6 Lookup, and deriving on demand

The sender knows its group and derives eagerly at first send. The recipient
does not: it sees a message with a `kid` and a `kid context` and must find or
build the matching Recipient Context.

```
on inbound OSCORE message:
    key = (kid, kid_context)
    if a live context matches key            -> use it
    else if kid_context names a group this node hosts
         and live contexts for this peer < cap
                                             -> derive, insert, count, use it
    else                                     -> reject; increment a counter
```

Three bounds, all required, all for the same reason — an attacker who can
reach the port can otherwise force unbounded HKDF work and unbounded memory by
inventing `kid context` values:

1. **Only for a hosted group.** A `kid context` naming a group this node has no
   replica of is rejected without deriving anything.
2. **A cap per peer, with LRU eviction.** Bounded memory. At roughly a hundred
   bytes of key material and window state per context, a thousand groups
   against four peers is a few hundred kilobytes — the cap exists for the
   adversarial case, not the ordinary one.
3. **A counter on every on-demand derivation.** Cheap to emit, and the only way
   an operator sees the attack rather than merely paying for it.

Eviction is by least-recently-used with a TTL, so that a peer restarting under
a new boot nonce does not have its in-flight messages under the old context
dropped the instant the new one appears.

### 4.7 Lifecycle

A context is destroyed when its group's local replica is — merged away,
tombstoned, or shut down with the host — and destruction zeroes the key
material rather than letting it fall out of a map. Split creates a group and
therefore a context, lazily, at its first message.

### 4.8 When OSCORE is not configured

Nothing above happens. `_id_context` stays empty, no `kid context` is emitted,
and the derivation is byte-identical to today's. This is what keeps the change
additive for every existing deployment: the new behaviour is reachable only by
supplying an `id_context`, which only the multi-Raft host does.

---

## 5. Duplicate detection

`is_duplicate_message()` exists twice — on the client
(coap_transport_impl.hpp:1778) and on the server (`:3482`, the copy on the
request path, called at `:3852` and `:4156`). Both key on the bare 16-bit
Message ID, in one map, held for five minutes:

```cpp
auto it = _received_messages.find(message_id);   // no peer in the key
return it != _received_messages.end();
```

Every client's counter starts at `_next_message_id{1}`
(coap_transport.hpp:404), so in any cluster larger than two nodes, two peers
send Message ID 1 and the second is answered with a bare `2.03 Valid` carrying
no payload. That is wrong today, without multi-Raft.

Multi-Raft makes it systematic rather than occasional. One client per process
serves every group, so a process leading N groups in a three-node cluster
emits roughly `40N` Message IDs per second at a 50 ms heartbeat, lapping the
16-bit space in `1638/N` seconds. Against a five-minute retention that
collides at **six groups**, and multi-Raft targets a thousand.

The fix is the key and the horizon:

```cpp
struct exchange_key { std::string peer_endpoint; std::uint16_t message_id; };
// retention: RFC 7252 Section 4.8.2 EXCHANGE_LIFETIME (247 s), not an arbitrary 5 min
```

The change is **strictly narrowing** — a message is a duplicate only if it
matches on both components — so it can turn a wrongly-dropped message into a
delivered one and never the reverse. That is what makes it safe to land ahead
of the integration tests that would otherwise be needed to trust it. The
existing `coap_duplicate_detection_property_test` asserts single-peer
behaviour, which is unchanged; two new cases cover the two-peer collision and
the wrap.

---

## 6. Timers, CON and NON

`ack_timeout` 2000 ms with `max_retransmit` 4 against a 50 ms heartbeat and a
150–300 ms election timeout means the transport is still retransmitting a
heartbeat several elections after Raft abandoned it. Raft has its own retry
loop, so confirmable delivery duplicates a mechanism that already exists one
layer up.

The answer is **not** to change the defaults. They are RFC 7252's values,
`coap_confirmable_message_property_test` asserts behaviour built on them, and
CoAP's reason for existing is the lossy constrained link where they are
correct. Instead:

```cpp
/// Retransmission tuned for Raft-rate traffic, where the consensus layer
/// retries on its own schedule and CON would duplicate it. Opt-in: a
/// deployment with few groups on a lossy link is better served by the
/// RFC 7252 defaults, and the transport cannot tell which case it is in.
auto raft_rate_profile() -> coap_client_config;
```

`InstallSnapshot` stays confirmable under every profile — large, rare, and
with no cheap retry above it.

Selection is explicit. Multi-Raft's presence is not a reason to infer the
profile, because "many groups" and "reliable link" are independent facts.

---

## 7. The shared client's mutex — measure, do not change

`coap_client` holds a `std::recursive_mutex` (coap_transport.hpp:402) across
every libcoap call, and the comment beside it explains that this is not a
choice:

> libcoap's C API is not safe to call concurrently from two threads on one
> context

Multi-Raft shares one client across every group, so the striped executor's
per-group parallelism ends at that lock. A previous investigation measured
`send_ms` median 19,881 and max 372,109 on an idle box when the I/O thread
starved it — a starvation since fixed in the I/O loop, but the shape of the
failure is the one this workload pushes on.

This specification therefore **measures and stops**. The measurement is the
**CoAP row of the shared benchmark matrix** in
`.kiro/specs/multi-raft-performance/` rather than a harness of its own: that
matrix drives the same key/value payload over every transport this project
ships, already sweeps group count across {1, 8, 64, 256, 1000}, and already
forbids changing a transport in order to measure it.

Reconciling the two was worth doing rather than letting both stand. A shared
client that serializes many groups into one queue is not a CoAP peculiarity —
cpp-httplib serializes concurrent RPCs behind one `httplib::Client` per target,
and Beast did the same behind one pooled connection until that was found to
crash under exactly this load. Asking the question once, in one place, is what
makes the three answers comparable instead of three unrelated numbers; asking it
twice would have produced two harnesses whose numbers could not be set beside
each other.

What stays CoAP-specific is the instrumentation and the discipline: the row
consumes the existing `KYTHIRA_COAP_SEND_PROBE` breakdown — lock acquisition,
resolution, session acquisition, PDU construction, `coap_send` — rather than
adding a second scheme, and the report carries the hypotheses the data refutes,
because `doc/coap-flake-investigation.md` exists precisely because this
investigation had been attempted several times from analysis alone.

Ordering: the transport work in this document comes first, the row second. The
row is a consumer of `multi_raft` running over CoAP at all, never a blocker on
it.

The reason for the discipline: every candidate remedy — a context per stripe,
a different I/O thread model, a lock-free send queue — is a change to the
existing transport's core, and would change single-group behaviour too. That
is a different risk class from everything else in this document, and it earns
its own review.

---

## 8. Test strategy

The CoAP suite was once the repository's merge blocker: a stretch of fifteen
CI runs at a 20% pass rate, with pull requests that touched no CoAP code
failing on rotating subsets of CoAP tests — one of them adding a single
workflow file and no code at all. The cause was per-case timeouts sized for a
Release build running under a 4× slower Coverage build, and the fix was
`KYTHIRA_TEST_TIMEOUT_SCALE` plus numeric endpoints. `doc/coap-flake-
investigation.md` records it, including what was tried and failed.

Every test this specification adds is therefore, from its first commit:

- sized through `KYTHIRA_TEST_TIMEOUT_SCALE`, both the Boost budget and the
  CTest `TIMEOUT` — scaling one alone moves the kill from one to the other;
- addressed by numeric endpoint, never a hostname, because `send_rpc()`
  resolves inside the recursive mutex and one lookup serialises every
  concurrent request behind it;
- bound to a port that is not 5683, which ten existing CoAP test files already
  contend for.

Raising a timeout is not the first response to an unstable new test, and the
"what was tried and failed" section is required reading before proposing one.

New coverage, in dependency order:

| Test | Asserts |
|---|---|
| backend conformance × 3 | the same concept set per backend, positives and negatives |
| `timeout_now` wire round-trip | encode/decode through each serializer |
| **`node` over CoAP** | a three-node cluster elects and replicates over a real CoAP transport — *this has never existed* |
| multi-Raft over CoAP | several groups over one client; per-group isolation |
| two-peer Message ID collision | both peers' requests delivered |
| Message ID wrap | a wrapped counter does not discard a live message |
| per-group OSCORE isolation | one group's rate does not exhaust another's replay window |
| on-demand derivation bound | an invented `kid context` is rejected and counted |
| leadership transfer over CoAP | leadership moves to a named target |

The third row is the one to watch. No test in the repository instantiates
`node<Types>` over any CoAP backend — the fifty-odd CoAP tests exercise the
transport standalone. Everything after it in the table depends on it working,
and it is the most likely place for this work to discover that something else
is wrong.

---

## 9. Phasing

| Phase | Content | Usable outcome |
|---|---|---|
| 1 | concept parity, the three `static_assert` blocks, the capability table | which backend can do what is a compile-time fact |
| 2 | duplicate detection rekeyed, two regression tests | a three-node CoAP cluster stops dropping a peer |
| 3 | `node` over CoAP integration test | CoAP is proven under Raft at all |
| 4 | `TimeoutNow` on three backends, wire and end-to-end tests | leader transfer and load-split scatter work |
| 5 | per-group OSCORE contexts | many groups over OSCORE stop exhausting the window |
| 6 | multi-Raft over CoAP integration test | the whole thing, together |
| 7 | the concurrency measurement and its document | a decision can be made about the mutex |

Phases 1–2 are independent of multi-Raft and fix defects that exist today.
Phase 3 is the gate: if a Raft node cannot run over CoAP, phases 4–6 have
nothing to stand on.

---

## 10. Open questions

1. **Which backends get `TimeoutNow`.** All three is the clean answer, but
   libnyoci and cantcoap are opt-in and own their own reliability and
   block-wise machinery, so the cost is not one-third each. If only libcoap
   ships it in phase 4, the capability table must say so and the negative
   `static_assert`s must record it.
2. **The per-peer context cap's default.** A thousand groups against a handful
   of peers is a few hundred kilobytes, so the cap is an adversarial bound
   rather than a capacity one. Its default should probably be well above any
   real shard count and well below memory exhaustion, and the right number
   wants the phase 5 measurements.
3. **Block-wise transfer under coalesced payloads.** Multi-Raft's deferred
   batching extension would make payloads larger, and CoAP answers larger
   payloads with more blocks. Whether the existing block transfer pipelines or
   serialises per block was not established here, and it decides whether
   batching helps CoAP or hurts it.
4. **`EXCHANGE_LIFETIME` retention cost.** 247 s of (peer, Message ID) pairs at
   multi-Raft rates is a large map — 40N per second per peer. A bounded
   structure keyed on the recent window may be needed rather than a plain map
   with a TTL sweep; phase 2's tests should measure the memory before this is
   assumed fine.
