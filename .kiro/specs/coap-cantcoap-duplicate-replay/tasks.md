# Implementation Plan — cantcoap duplicate-request replay

## Status: Not started

**Last Updated**: October 2, 2026

This plan implements `.kiro/specs/coap-cantcoap-duplicate-replay/design.md`.
Six tasks. Task 1 is a prerequisite check: the design builds on
`coap_exchange_table`, which PR #385 adds and which is not on `main` yet.

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 1, "tasks": [1], "description": "Prerequisite: coap_exchange_table on the base" },
    { "wave": 2, "tasks": [2, 3], "description": "Reply storage in the table; config field" },
    { "wave": 3, "tasks": [4], "description": "Server: outer-message check, capture in finish_reply" },
    { "wave": 4, "tasks": [5], "description": "Tests that actually lose a reply" },
    { "wave": 5, "tasks": [6], "description": "Docs and the cantcoap spec's record" }
  ]
}
```

---

## Tasks

- [ ] 1. Put `coap_exchange_table` under the branch
  - If PR #385 has merged, branch from `main`. If not, stack on its branch and
    say so in the PR; do not copy the table into this change.
  - Confirm the cantcoap server's `_seen` is a `coap_exchange_table` keyed on
    (peer, Message ID, token) before starting task 4.
  - _Requirements: 4.1_

- [ ] 2. Add optional reply storage to `coap_exchange_table`
  - `coap_reply_cache_limits`, `classify`, `attach_reply` and `replies()` as
    in the design. The default limits store nothing.
  - Eviction deque with the `replied` stamp guard, so evicting an old entry
    never clears the reply of a newer exchange on a wrapped Message ID.
  - Reply expiry at `retention` in `classify` and in `sweep`; the record keeps
    its own 247 s lifetime.
  - Extend `tests/coap_exchange_table_test.cpp`: store and fetch, retention
    expiry with the record still a duplicate, oldest-first eviction, a reply
    larger than the whole budget is not stored, wrapped-MID guard, budget 0
    stores nothing, `clear()` releases bytes.
  - Check the libcoap callers compile and behave unchanged (they keep the
    default limits).
  - _Requirements: 3.1, 3.2, 3.3, 3.6, 4.1, 4.2, 4.3, 5.7_

- [ ] 3. Add `coap_server_config::duplicate_reply_cache_bytes`
  - Default 16 MiB, with the comment from the design naming which backends
    honour it.
  - _Requirements: 3.5, 4.4_

- [ ] 4. Move the cantcoap server's duplicate check to the outer message
  - In `handle_datagram`, parse the outer PDU first, ignore ACK/RST, and
    `classify` under `_mutex` before EDHOC routing or OSCORE verification.
  - `replay`: send the cached bytes through DTLS or the socket, outside the
    table's critical section, and count it. `drop`: count and return.
  - Remember the current exchange and attach the bytes in `finish_reply`,
    only when the request was CON.
  - Delete the two inner `is_duplicate` calls (after OSCORE and in
    `handle_edhoc_request`) and the member function itself.
  - Construct `_seen` with limits from `duplicate_reply_cache_bytes`.
  - Add `duplicate_stats()`.
  - Update the file's header comment, which lists "retransmit/dedup" under
    reused scaffolding, to say replies are replayed.
  - _Requirements: 1.1-1.6, 2.1-2.5, 3.4, 5.2_

- [ ] 5. Tests that lose a reply
  - A test-only lossy UDP relay (drop the Nth datagram in one direction), free
    of CoAP library headers so libnyoci can reuse it.
  - Cases in `tests/coap_cantcoap_integration_test.cpp`, per the design's
    table: raw CON twice; raw CON twice under OSCORE with byte-identical
    replies; raw NON twice; client RPC with its first reply dropped; Block1
    with one 2.31 dropped; EDHOC with message_2 dropped; budget 0.
  - Fix or replace `test_duplicate_requests_are_suppressed`, whose comment
    describes a response duplicated on the wire that the test never produces.
  - CI builds only the stub (audit C1). Run the suite locally with
    `CONFIG_COAP_TRANSPORT_CANTCOAP=y` and the `coap-cantcoap` vcpkg feature and record where it ran in this task.
  - _Requirements: 5.1-5.6_

- [ ] 6. Record it
  - `.kiro/specs/coap-transport-cantcoap/requirements.md` 4.4 and the tasks
    table: duplicates are now answered, not only discarded; link here.
  - `doc/coap_library_alternatives.md`, if it describes cantcoap's
    reliability layer, says the same.
  - Mark this plan complete with the commit and where the tests ran.
  - _Requirements: none (record keeping)_

## Out of scope

- Server metrics for cantcoap generally (audit, CoAP "Low").
- Per-peer Block1 reassembly (audit C9). Task 4 keeps the shared buffer as it
  is; the replay only guarantees a duplicate block does not touch it.
- Building the cantcoap backend in CI (audit C1).
