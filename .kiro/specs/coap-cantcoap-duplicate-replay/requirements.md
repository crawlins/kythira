# Requirements — cantcoap duplicate-request replay

## Introduction

The cantcoap backend (`.kiro/specs/coap-transport-cantcoap/`) builds its own
reliability layer because cantcoap is only a codec. Its server suppresses a
retransmitted confirmable request (Requirement 4.4 of that spec) but stores
nothing about the reply it sent. When the reply is lost, every retransmission
the client sends is recognised as a duplicate and dropped, the client exhausts
`MAX_RETRANSMIT`, and the RPC fails with `coap_timeout_error`. One lost
datagram on the return path fails the call outright, which is exactly the case
CoAP's confirmable messages exist to survive.

RFC 7252 Section 4.5 is explicit about the remedy: a recipient that sees a
duplicate confirmable message SHOULD acknowledge each copy, and SHOULD process
any request only once. libcoap and libnyoci both do this inside the library by
resending the cached response. cantcoap has no library to do it, so the adapter
must.

Verified on `main` at `9c738d2`:

- `include/raft/coap_transport_cantcoap_impl.hpp:1605-1607` returns from
  `handle_datagram` on a duplicate without sending anything.
- `_seen` (`:2015`) maps a key to `received_message_info`, which holds a
  Message ID and a timestamp and no reply.
- The EDHOC resource drops duplicates the same way (`:1765-1767`), so a lost
  message_2 stalls the handshake until it times out.
- Duplicate detection runs **after** OSCORE verification (`:1577-1591`). A
  retransmitted OSCORE request repeats its Partial IV, so the replay window
  rejects it before the duplicate check is reached. Without EDHOC that is a
  silent drop; with EDHOC bootstrap the server answers 4.01 and the client is
  pushed into a fresh handshake (inferred from the code path, not run). Moving
  the reply cache after OSCORE would therefore not fix the OSCORE case.
- `test_duplicate_requests_are_suppressed`
  (`tests/coap_cantcoap_integration_test.cpp:335`) sends one clean request and
  checks the handler ran once. It never loses a reply, so it cannot see this.

PR #383 (merged) keyed duplicates by peer, and PR #385 (open) replaces the key
with (peer, Message ID, token) in a shared, bounded `coap_exchange_table` kept
for EXCHANGE_LIFETIME (247 s). Neither stores a response. This spec builds on
#385's table.

Source: `parallel-implementation-parity-audit.md`, finding C4.

## Glossary

- **Exchange_Table**: `kythira::coap_exchange_table`
  (`include/raft/coap_exchange_table.hpp`, added by PR #385), which records
  (peer, Message ID, token) for duplicate detection.
- **Cached_Reply**: the exact bytes the server sent in answer to one request,
  after OSCORE protection and before DTLS record protection.
- **Outer_Message**: the CoAP message as it arrives off the socket (after DTLS
  decryption, before OSCORE verification). Its Message ID and token are the
  ones the message layer uses.
- **Reply_Retention**: how long a Cached_Reply is kept. Shorter than the
  duplicate-detection lifetime; see Requirement 3.
- **Reply_Budget**: the cap on total Cached_Reply bytes held by one server.

## Requirements

### Requirement 1 — Replay the reply to a duplicate confirmable request

**User Story:** As a Raft node on the cantcoap backend, I want a request whose
reply was lost to succeed on retransmission, so one dropped datagram does not
fail an RPC.

#### Acceptance Criteria

1. WHEN the server receives a confirmable request that duplicates one it has
   already answered THEN it SHALL send the Cached_Reply for that request again,
   byte for byte before DTLS, without invoking any handler.
2. WHEN a duplicate is replayed THEN the handler for that request SHALL have
   run exactly once in total.
3. WHEN the original reply was a piggy-backed 2.05, a 2.31 Continue for a
   Block1 block, or an error response (4.xx or 5.xx) THEN each SHALL be cached
   and replayed the same way.
4. WHEN a duplicate Block1 block is replayed THEN the server's Block1
   reassembly state SHALL NOT change.
5. WHEN the server receives a duplicate non-confirmable request THEN it SHALL
   drop it silently, as RFC 7252 Section 4.5 permits, and SHALL NOT invoke the
   handler.
6. WHEN the server receives a duplicate of a request it is still processing
   THEN it SHALL drop that copy; the reply the original produces answers both.
   (The server loop is single-threaded and handlers run inline, so this case
   arises only if that changes; the requirement pins the behaviour if it does.)

### Requirement 2 — Detect duplicates on the outer message, before OSCORE

**User Story:** As an operator running OSCORE, I want retransmissions handled
at the message layer, so a lost reply neither trips OSCORE replay protection
nor forces an EDHOC re-bootstrap.

#### Acceptance Criteria

1. WHEN a datagram is received THEN the duplicate check SHALL run on the
   Outer_Message, keyed on (peer, Message ID, token), before OSCORE
   verification and before any EDHOC routing.
2. WHEN the duplicate is of an OSCORE-protected request THEN the replayed
   bytes SHALL be the protected response originally sent, so no new nonce is
   consumed and no plaintext is encrypted twice under one nonce.
3. WHEN the duplicate is of an EDHOC message on `/.well-known/edhoc` THEN the
   server SHALL replay the EDHOC reply it sent and SHALL NOT pass the message
   to the responder again.
4. WHEN a request fails OSCORE verification or is otherwise rejected THEN the
   rejection reply, if one was sent, SHALL be cached like any other reply, and
   a request that produced no reply SHALL still be recorded so its copies are
   dropped rather than re-verified.
5. WHEN DTLS is enabled THEN a replay SHALL go out through the DTLS session as
   a fresh record; only the plaintext CoAP bytes are cached.

### Requirement 3 — Bounded memory

**User Story:** As an operator of a node serving many Raft groups, I want the
reply cache bounded, so retransmission support cannot exhaust memory.

#### Acceptance Criteria

1. WHEN a reply is cached THEN it SHALL be retained for at most
   MAX_TRANSMIT_WAIT (93 s with the RFC 7252 Section 4.8 defaults); after that
   no client following those defaults is still waiting for it.
2. WHEN a Cached_Reply expires or is evicted THEN its duplicate-detection
   record SHALL remain for the full EXCHANGE_LIFETIME, so a late copy is still
   dropped rather than processed a second time.
3. WHEN total cached bytes would exceed the Reply_Budget THEN the oldest
   Cached_Replies SHALL be evicted first until the new one fits.
4. WHEN a duplicate arrives whose reply was evicted THEN the server SHALL drop
   it, as it does today, and SHALL count the event (Requirement 5.2).
5. WHEN the operator does not configure a Reply_Budget THEN a default SHALL
   apply that keeps every reply through at least a client's first two
   retransmissions (ACK_TIMEOUT x ACK_RANDOM_FACTOR x 3 = 9 s with RFC
   defaults) at 64 groups and two peers, the heaviest multi-Raft CoAP profile
   measured (~40 Message IDs per second per group, per
   `coap_exchange_table.hpp`).
6. WHEN the Reply_Budget is configured to 0 THEN no reply SHALL be cached and
   the server SHALL behave exactly as it does today.

### Requirement 4 — Shared table, no regression elsewhere

**User Story:** As a maintainer, I want the reply cache to live beside the
existing duplicate table rather than in a fourth copy, and not to change the
libcoap backend.

#### Acceptance Criteria

1. WHEN the reply cache is implemented THEN it SHALL reuse
   `coap_exchange_table`'s key, lifetime and sweep rather than keeping a
   parallel map keyed differently.
2. WHEN the libcoap backend uses `coap_exchange_table` THEN its behaviour and
   memory use SHALL be unchanged; caching SHALL be opt-in per owner.
3. WHEN `stop()` runs THEN every Cached_Reply SHALL be released along with the
   duplicate records.
4. WHEN a new `coap_server_config` field is added for the Reply_Budget THEN
   its comment SHALL say which backends honour it; libcoap and libnyoci SHALL
   ignore it (they cache inside the library).

### Requirement 5 — Tests that lose a reply

**User Story:** As a maintainer, I want a test that actually drops a reply, so
this cannot silently regress.

#### Acceptance Criteria

1. WHEN the cantcoap integration suite runs THEN a test SHALL send the same
   confirmable request twice from a raw UDP socket and assert two identical
   replies and one handler invocation.
2. WHEN the server replays, drops for want of a cached reply, or evicts THEN
   it SHALL count each in a counter that tests can read.
3. WHEN the cantcoap client's request reply is lost once (by a test-only drop
   hook or a lossy relay) THEN the RPC SHALL succeed and the handler SHALL have
   run once.
4. WHEN OSCORE is on THEN the raw-socket duplicate test SHALL also pass, and
   the second reply SHALL equal the first byte for byte.
5. WHEN a Block1 transfer has one 2.31 lost THEN the transfer SHALL complete
   and the reassembled body SHALL be correct.
6. WHEN `test_duplicate_requests_are_suppressed` is kept THEN its comment SHALL
   stop claiming it exercises a duplicate on the wire, or the test SHALL be
   replaced by Requirement 5.1's test.
7. WHEN `coap_exchange_table_test` runs THEN it SHALL cover reply storage,
   Reply_Retention expiry independent of the record, budget eviction order and
   the budget-0 case.
