# Design — cantcoap duplicate-request replay

## Overview

The cantcoap server learns to answer a retransmitted confirmable request by
resending the reply it already sent. Three changes carry it:

1. `coap_exchange_table` (PR #385) gains optional, budgeted storage for one
   reply per recorded exchange. Owners that never attach a reply, which is the
   libcoap backend, pay nothing.
2. The cantcoap server moves its duplicate check to the top of
   `handle_datagram`, onto the outer message, ahead of OSCORE and EDHOC.
3. Every reply already leaves through one function, `finish_reply`. That is
   where the bytes are captured and attached to the exchange being served.

Nothing changes on the client or on the wire for a request whose reply
arrives. The only observable difference is that a retransmission now gets an
answer.

## Where the gap comes from

The backend's reliability layer was specified as "detect duplicates via
`Message_ID` and discard" (cantcoap Requirement 4.4). That is half of RFC 7252
Section 4.5, which also says each duplicate confirmable message SHOULD be
acknowledged. libcoap and libnyoci do both inside the library, so the
requirement read as complete there; cantcoap is the one backend where the
adapter owns the message layer, and the second half was never written.

The OSCORE ordering is a separate mistake that the missing half hid. The
check runs after `unprotect_request`, but a retransmission carries the same
Partial IV, so OSCORE's replay window rejects it first. Today that is invisible
because the copy would have been dropped anyway. Once copies are answered it
matters: a cache consulted after OSCORE would never be reached.

## Components

### `coap_exchange_table`: optional reply storage

The table already keys on (peer, Message ID, token), keeps a record for
EXCHANGE_LIFETIME and sweeps in amortised batches. The reply goes in the same
slot, so there is one key, one lookup and one sweep.

```cpp
struct coap_reply_cache_limits {
    std::size_t budget_bytes{0};                    // 0: never store a reply
    clock::duration retention{std::chrono::seconds{93}};  // MAX_TRANSMIT_WAIT
};

enum class coap_duplicate_kind { fresh, replay, drop };

struct coap_duplicate_lookup {
    coap_duplicate_kind kind;
    std::shared_ptr<const std::vector<std::byte>> reply;  // set only for replay
};

class coap_exchange_table {
public:
    explicit coap_exchange_table(clock::duration lifetime = coap_exchange_lifetime,
                                 coap_reply_cache_limits limits = {});

    // Existing API unchanged: is_duplicate, record, check_and_record, sweep, clear.

    /// One step for owners that cache: fresh (now recorded), replay (with the
    /// stored reply) or drop (a duplicate with no reply left to send).
    auto classify(std::string_view peer, std::uint16_t message_id,
                  std::string_view token, clock::time_point now = clock::now())
        -> coap_duplicate_lookup;

    /// Stores the reply sent for a recorded exchange. No-op when the budget
    /// is 0, the exchange is not recorded, or the reply alone exceeds the budget.
    auto attach_reply(std::string_view peer, std::uint16_t message_id,
                      std::string_view token, std::vector<std::byte> reply,
                      clock::time_point now = clock::now()) -> void;

    struct reply_stats { std::uint64_t evicted; std::size_t bytes; std::size_t count; };
    [[nodiscard]] auto replies() const -> reply_stats;
};
```

The slot gains `std::shared_ptr<const std::vector<std::byte>> reply` and
`clock::time_point replied`. A `std::deque` of (peer, Message ID, replied) in
insertion order drives eviction: while `bytes + new_size > budget_bytes`, pop
the front and clear that slot's reply if it is still the one the deque entry
names (a wrapped Message ID may have replaced it; the `replied` stamp tells).
Expiry against `retention` happens lazily in `classify` and in the existing
sweep. The record itself is untouched by either, so it lives the full 247 s.

The default `coap_reply_cache_limits{}` stores nothing, which keeps the libcoap
backend's behaviour and memory identical (Requirement 4.2).

**Decision: extend the table, not a second map.** A separate cache keyed the
same way would need its own sweep and its own view of "same exchange", and the
two could disagree after a Message ID wraps. The table already resolves that
with the token comparison, so the reply rides along. Cost: the table header
grows by about 80 lines, and libcoap compiles code it never calls.

**Decision: `shared_ptr` to the bytes.** The replay is sent outside the
table's critical section in the DTLS case (DTLS send takes its own locks), and a
shared pointer lets it outlive a concurrent eviction without copying the reply.

### Retention and budget

- **Retention 93 s (MAX_TRANSMIT_WAIT).** After that, a client using RFC 7252
  defaults has given up, so the reply is useless. Keeping the record for
  247 s still drops very late copies rather than re-running the handler
  (Requirement 3.2).
- **Default budget 16 MiB.** Raft responses are small: a RequestVote or
  AppendEntries response encodes to well under 100 bytes, plus about 20 bytes
  of CoAP header and up to ~15 of OSCORE. Allowing ~192 bytes per reply with
  overhead, 64 groups x 40 Message IDs/s x 2 peers x 9 s is about 8.8 MB, so
  16 MiB covers the first two retransmissions with room to spare at the
  heaviest measured profile (Requirement 3.5). At lighter load the 93 s
  retention, not the budget, is what bounds the cache.
- **Eviction drops, never re-runs.** A duplicate whose reply was evicted is
  dropped and counted, which is exactly today's behaviour for every duplicate.
  Re-running the handler was rejected below.

New field, in `coap_server_config`:

```cpp
// Bytes of sent replies the cantcoap server keeps so it can answer a
// retransmitted request whose reply was lost (RFC 7252 Section 4.5). 0
// disables replay. libcoap and libnyoci ignore this: they replay inside the
// library.
std::size_t duplicate_reply_cache_bytes{16 * 1024 * 1024};
```

### Server flow

```
run_loop
  -> DTLS decrypt (if on)            peer known, plaintext CoAP bytes
  -> handle_datagram
       parse outer PDU; drop if invalid; ignore ACK/RST
       lock _mutex
       classify(peer, MID, token)
         replay -> unlock; send cached bytes (DTLS or socket); ++replayed; return
         drop   -> ++dropped_without_reply; return
         fresh  -> _current = {peer, MID, token}; continue
       [EDHOC routing | OSCORE verify | method check | Block1 | handler]
       every reply -> finish_reply -> bytes -> attach_reply(_current, bytes)
```

- The new check replaces both existing `is_duplicate` calls (`:1605` after
  OSCORE, `:1767` in `handle_edhoc_request`). One check on the outer message
  covers both paths, and EDHOC duplicates now get message_2 replayed instead of
  a stalled handshake (Requirement 2.3).
- `finish_reply` already serialises the reply and, under OSCORE, protects it;
  it attaches those bytes before handing them to DTLS or the socket. So the
  cache holds exactly what OSCORE produced, and a replay never re-encrypts
  under OSCORE (Requirement 2.2). DTLS re-encrypts each send as a new record,
  which it must anyway (Requirement 2.5).
- A request that is rejected without a reply (an unverifiable OSCORE message
  outside EDHOC mode, a malformed PDU after the outer parse) stays recorded
  with no reply, so its copies classify as `drop` and are not re-verified
  (Requirement 2.4).
- NON requests are recorded but never cached: `attach_reply` is called only
  when the outer request was CON, so a NON duplicate classifies as `drop`
  (Requirement 1.5). The server only answers NON with NON, and RFC 7252 does
  not ask for those to be repeated.
- The Block1 reassembly buffer is only touched after `classify` returns
  `fresh`, so a replayed 2.31 cannot reset or extend it (Requirement 1.4).
- `_current` is a member set under `_mutex` and read by `finish_reply` on the
  same thread. The server loop is single-threaded and handlers run inline, so
  there is never a second exchange in flight. If that ever changes, the
  in-progress case of Requirement 1.6 is already covered: the record exists
  with no reply, so copies drop until the reply is attached.
- `stop()` already calls `_seen.clear()`; that now releases the replies too
  (Requirement 4.3).

### Counters

The cantcoap server has no metrics today (a separate parity gap, out of scope
here). It gains a small read-only accessor, enough for tests and logs:

```cpp
struct duplicate_reply_stats {
    std::uint64_t replayed;
    std::uint64_t dropped_without_reply;
    std::uint64_t evicted;
};
[[nodiscard]] auto duplicate_stats() const -> duplicate_reply_stats;
```

`replayed` and `dropped_without_reply` are `std::atomic` members; `evicted`
comes from the table under `_mutex`.

## Alternatives rejected

- **Re-run the handler on a duplicate.** Raft's handlers tolerate a repeated
  AppendEntries, but the reply could differ (a term moved on in between), the
  EDHOC responder cannot be re-run at all, and under OSCORE a re-run response
  would need a fresh protection that the client's original request binding no
  longer matches. RFC 7252 asks for one execution per request.
- **Cache after OSCORE verification.** Unreachable for retransmissions, as
  shown above.
- **Bound by count instead of bytes.** Replies vary from a 2.04 with no body to
  a 1 KiB Block2 slice; a byte budget is what an operator can reason about.
- **Retain replies for the full 247 s.** About 2.6 times the memory, for
  copies no client following RFC defaults is still waiting on.

## Testing

| Test | Where | Proves |
|---|---|---|
| reply storage, retention expiry separate from record, eviction order, wrapped-MID eviction guard, budget 0 | `tests/coap_exchange_table_test.cpp` | table contract (Req 3, 4.2) |
| raw UDP: send one CON twice; two identical replies, handler once, `replayed == 1` | `tests/coap_cantcoap_integration_test.cpp` | Req 1.1, 1.2, 5.1 |
| same over OSCORE, replies byte-identical | same file, OSCORE fixture | Req 2.1, 2.2, 5.4 |
| raw UDP: NON sent twice; one reply, handler once | same | Req 1.5 |
| client RPC with first reply dropped by a lossy UDP relay; succeeds, handler once | same | Req 5.3 |
| Block1 transfer with one 2.31 dropped by the relay; body correct | same | Req 1.3, 1.4, 5.5 |
| EDHOC with message_2 dropped once; handshake completes | `tests/coap_cantcoap_integration_test.cpp` | Req 2.3 |
| `duplicate_reply_cache_bytes = 0`; duplicate dropped, `dropped_without_reply == 1` | integration file | Req 3.6 |

The lossy relay is a test-only UDP forwarder between client and server that
drops the Nth datagram in one direction. It is free of CoAP library headers,
like `tests/coap_wire_probe.hpp`, so libnyoci can reuse it for its own missing
duplicate and retransmission tests (audit "Test gaps by backend").

CI builds only the cantcoap stub (audit C1), so these suites skip there. The
tasks record where they were actually run.
