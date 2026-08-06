# Implementation Plan

**Status (August 6, 2026): Tasks 1-4, 6 and 12 are implemented.** They form the
protocol-independent core — the concepts, the two registries, the exception, and
their unit tests — and land as one self-contained change. Everything from Task 5
onward is transport wiring, which touches every `Types` bundle and all four
transports and is deliberately left as separate work.

- [x] 1. Strengthen the `rpc_serializer` concept and add `media_type()`
  - Added `{ s.media_type() } -> std::convertible_to<std::string>;` to
    `rpc_serializer` (`include/raft/types.hpp`) — note this required adding a
    `requires(const S& s)` parameter, since the concept had been effectively
    vacuous (`typename S;` only)
  - Added `media_type()` to **all four** shipped serializers, not just JSON:
    strengthening the concept makes it mandatory for every one of them.
    `application/json`, `application/cbor` (RFC 8949 §9.1),
    `application/x-protobuf` (what the ecosystem actually emits, over the later
    formal `application/protobuf` registration), and — uniquely — an
    encoding-dependent `application/x-amzn-ion` / `text/x-amzn-ion` for Ion,
    since its two encodings are not interchangeable on the wire
  - `name()` left unchanged throughout: it is a metrics label, not a wire token
  - _Requirements: 2.1, 2.2, 2.3_

- [x] 2. Define the `Serializer_Registry` concept
  - Added `serializer_registry<R, Data>` to `include/raft/types.hpp`:
    `default_media_type()`, `preferred_media_types()`, `supports(media_type)`,
    `select_output_media_type(accepted) -> std::optional<std::string>`
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6_

- [x] 3. Implement `single_serializer_registry<S>` and `multi_serializer_registry<S...>`
  - New header `include/raft/serializer_registry.hpp`, as designed
  - Two decisions worth recording, both tested:
    - An **empty** `accepted` list yields the default rather than `nullopt`. A
      missing `Accept` means "no preference", not "accepts nothing", and it is
      the common case for a peer predating negotiation.
    - `select_output_media_type` scans in the **peer's** order, not ours.
      Otherwise the negotiated type would depend on which side asked first.
  - _Requirements: 1.7, 1.8, 1.9, 1.10_

- [x] 4. Add a minimal test-only second serializer
  - Implemented as `tagged_serializer<Tag, MediaType>` inside
    `tests/serializer_registry_unit_test.cpp` rather than a shared `tests/`
    header, and it does **not** wrap `json_rpc_serializer`. Wrapping JSON would
    have made every registry's output identical bytes, which cannot distinguish
    "dispatched to the right serializer" from "dispatched to any serializer" —
    the one property the registry is responsible for. The tagged serializer
    emits its own tag byte and rejects a foreign tag on decode, so a
    mis-dispatch fails loudly instead of returning a plausible value.
    Promote it to a shared header if Tasks 13-16 need it.
  - _Requirements: 11.1_

- [x] 5. Extend `transport_types` with `serializer_registry_type`
  - `transport_types` (`include/raft/types.hpp`) now requires
    `typename T::serializer_registry_type` *and* checks it against
    `serializer_registry<..., std::vector<std::byte>>`. `serializer_type` is
    retained beside it as the default serializer's type, per Requirement 3.2 —
    dropping it would have turned an additive change into a migration for
    `raft.hpp`, every transport's own `using serializer_type = ...`, and ~20 test
    bundles, with nothing gained
  - Bundles updated: the three in `http_transport.hpp`, the four in
    `coap_transport.hpp`, `future_default_http_transport_types`
    (`beast_http_transport.hpp`), **`future_default_proxygen_transport_types`
    (`proxygen_http_transport.hpp`)** and **`test_transport_types`
    (`test_types.hpp`)** — the last two are not in the original task text but
    model `transport_types` and so had to move with it
  - **Also updated: 16 test-local `Types` bundles.** The blast radius of a hard
    concept requirement is every model of it, not just the ones the plan named.
    Worth knowing before Tasks 9-11 add more members: a defaulting trait (the
    `_peer2peer_replicator_type_traits` idiom already in `types.hpp`) would have
    avoided this, and was not chosen only because Requirement 3.3 specifies the
    explicit-alias migration
  - `raft_types` verified untouched, and that verification is now a
    `static_assert` rather than an assertion in a commit message
  - New `tests/transport_types_registry_conformance_test.cpp` pins all of it.
    Its negative cases are the load-bearing ones and were **mutation-tested**:
    deleting the requirement fails it, and so does weakening it to a bare
    `typename T::serializer_registry_type;` — a concept that silently stops
    constraining anything is exactly the "green while doing nothing" shape this
    repo keeps hitting, and it leaves no other trace
  - **Finding, unrelated to this change but exposed by it**:
    `std_http_transport_types` and `std_coap_transport_types` have *never*
    satisfied `transport_types`. Both pin `future_template` to `std::future`,
    which has no `wait(duration)` returning a testable value and so does not
    model `future`. Confirmed by asserting it against the pre-change tree, not
    inferred. They are unreachable configurations; the conformance test omits
    them on purpose and says why. Deleting them is a separate cleanup
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 7.1, 7.2_

- [x] 6. Add `unsupported_media_type_error` and its CoAP analogue
  - `include/raft/http_exceptions.hpp`: `unsupported_media_type_error :
    http_transport_error`, carrying the offending media type so a handler can
    build its 415/406 response without re-parsing the header it just read
  - Deliberately distinct from the existing `serialization_error`, which means a
    payload of a type we *do* support failed to decode. The two map to different
    HTTP statuses (415 vs 400), so a transport must be able to tell them apart
    without inspecting message text
  - CoAP analogue added: `coap_unsupported_content_format_error :
    coap_transport_error` (`include/raft/coap_exceptions.hpp`). A separate type
    rather than a shared one, because the two hierarchies are disjoint all the
    way up and a CoAP handler catching `const coap_transport_error&` must not
    miss it. It carries the *media type*, not a `coap_content_format`, because
    that is what the registry dispatches on and not every media type has a
    Content-Format number — which is precisely the condition it reports
  - Distinct from `coap_protocol_error` (malformed PDU → 4.00) for the same
    reason the HTTP pair is distinct: different response code, and telling them
    apart must not require inspecting message text
  - _Requirements: 8.1_

- [x] 7. Add `Media_Type`-keyed CoAP content-format lookup
  - `media_type_to_coap_content_format(media_type) ->
    std::optional<coap_content_format>` in `include/raft/coap_utils.hpp`: an
    exact-match table over the media types the shipped serializers actually
    report
  - `std::optional`, and an exact table, are both reactions to specific
    behaviours of the function it supersedes.
    `get_content_format_for_serializer` substring-matches `name()` — a
    free-form metrics label — so `"json-lines"` silently claims Content-Format
    50; and it defaults *unknown* serializers to `application_cbor`, meaning an
    unrecognised encoding does not fail but puts a confidently wrong number on
    the wire. `std::optional` makes "no mapping" a value the caller must handle
    instead of a plausible-looking answer
  - `get_content_format_for_serializer` marked `@deprecated` with its three
    remaining call sites named. Not removed here: those sites are Task 9/11's to
    convert, and deleting it now would mean rewriting them blind
  - `validate_registry_content_formats(registry)` rejects a registry CoAP cannot
    represent, throwing `coap_unsupported_content_format_error`
  - **The task text asked only for the "no entry" case, which is not sufficient
    on its own.** The mapping is not injective: Ion binary and Ion text both map
    to 65000 (one private-use number covers both — see the enumerator), and
    protobuf shares 42 with any raw octet-stream serializer. CoAP negotiates by
    *number*, so a receiver handed 65000 by a registry holding both Ion
    encodings cannot tell which was meant and decodes the wrong one. The
    validator therefore also rejects colliding mappings, and names both sides of
    the collision since the fix is to the pair. HTTP has neither problem — it
    negotiates on the media-type string itself — which is why this check belongs
    to CoAP rather than to the registry
  - Open for Task 11: the server's Content-Format → media-type direction cannot
    be a plain inverse of this table for the same non-injectivity reason. It has
    to resolve against the registry's own `preferred_media_types()`, which
    `validate_registry_content_formats` has already guaranteed is unambiguous
  - _Requirements: 9.1, 9.2, 9.3_

- [x] 8. Implement shared `Accept`/`Content-Type` header parsing helper
  - `parse_accept_header(std::string_view) -> std::vector<std::string>` in the
    new `include/raft/http_content_negotiation.hpp`: comma-split, trim, drop
    `;`-delimited parameters, preserve source order
  - **`q`-values are stripped, not honoured** — source order is preserved as-is.
    A deliberate limitation, pinned by its own test so changing it cannot be
    silent: RFC 9110 §12.5.1 ranks by `q`, but this codebase's clients emit
    `preferred_media_types()` in preference order with no `q` at all, so ranking
    would change nothing for them while adding a parser that could disagree
    between the two servers. A `q`-ranking peer still gets a type it accepts,
    just possibly not its first choice
  - Wildcards (`*/*`, `type/*`) pass through the parser verbatim; matching them
    is the registry's job, not the parser's (`accept_entry_matches`). This
    bullet previously claimed they "match nothing, falling through to the
    default" — true of the first version of the parser, and wrong from the
    wildcard fix onward. The outcome for `*/*` is the same either way, but the
    mechanism is not, and `type/*` behaves differently: it selects the first
    *matching* registered type rather than the default
  - Shared rather than per-server precisely because a `q`-handling bug fixed in
    one copy and not the other would stay invisible until a peer used the
    transport that still had it
  - _Requirements: 5.1_

- [ ] 9. Wire content negotiation into `cpp_httplib_client`/`cpp_httplib_server`
  - [ ] 9.1 Client request path: pick output `Media_Type` from `Peer_Capability_Cache`
    or `default_media_type()`; set `Content-Type`; set `Accept` from
    `preferred_media_types()`
    - _Requirements: 6.1, 6.2, 6.3_
  - [ ] 9.2 Client response path: read `Content-Type` (absent → default); decode via
    registry; on mismatch fail the future with `unsupported_media_type_error`; on
    success update `Peer_Capability_Cache`
    - _Requirements: 6.4, 6.5, 6.6_
  - [ ] 9.3 Server request path: read `Content-Type` (absent → default); decode via
    registry; on mismatch return HTTP 415 without invoking the handler
    - _Requirements: 4.1, 4.2, 4.3, 4.4_
  - [ ] 9.4 Server response path: parse `Accept`; `select_output_media_type`; on
    `std::nullopt` return HTTP 406; else encode via registry and set `Content-Type`
    - _Requirements: 5.1, 5.2, 5.3, 5.4_
  - [ ] 9.5 Replace the hardcoded `content_type_json` response header with the
    negotiated `Media_Type` on every response path (success and error)
    - _Requirements: 9.4_
  - [ ] 9.6 Add `media_type` dimension to existing request/response metrics; emit
    `error_type=unsupported_media_type` on negotiation failure
    - _Requirements: 10.1, 10.2, 10.3_

- [ ] 10. Wire content negotiation into `boost_beast_client`/`boost_beast_server`
  - Mirror task 9's client/server request/response paths using Boost.Beast's header API
  - _Requirements: 4.1-4.4, 5.1-5.4, 6.1-6.6, 9.4, 10.1-10.3_

- [ ] 11. Wire content negotiation into `coap_client`/`coap_server`
  - [ ] 11.1 Client: set `Content-Format` from cached/default `Media_Type`; set repeated
    `Accept` options from `preferred_media_types()`
    - _Requirements: 6.1, 6.2, 6.3_
  - [ ] 11.2 Client: read response `Content-Format` (absent → default); decode via
    registry; on mismatch set error state; on success update `Peer_Capability_Cache`
    - _Requirements: 6.4, 6.5, 6.6_
  - [ ] 11.3 Server: read request `Content-Format` (absent → default); decode via
    registry; on mismatch respond 4.15 without invoking the handler
    - _Requirements: 4.5, 4.6_
  - [ ] 11.4 Server: collect request's `Accept` options; `select_output_media_type`; on
    `std::nullopt` respond 4.06; else encode via registry and set one `Content-Format`
    option
    - _Requirements: 5.5_
  - [ ] 11.5 Add `media_type` dimension to existing CoAP metrics; emit
    `error_type=unsupported_media_type` on negotiation failure
    - _Requirements: 10.1, 10.2, 10.3_

- [x] 12. Unit tests for the registry and negotiation logic
  - `tests/serializer_registry_unit_test.cpp`, 16 cases, linking no transport —
    the negotiation rules are protocol-independent and only their *rendering*
    (HTTP 406 vs CoAP 4.06) is not, so they are pinned here once rather than
    re-tested per transport
  - Covers: concept conformance for both registries; `select_output_media_type`
    empty / overlap / partial-overlap / no-overlap; declaration order preserved
    as preference order; the peer's `Accept` order deciding the result;
    `encode_with`/`decode_with` dispatching to the *named* serializer (proved by
    tag byte, not just by round-trip success); decoding under the wrong
    registered type failing loudly; `unsupported_media_type_error` on both paths
    for both registries and carrying the offending type; and one real
    `request_vote_request` round-trip through JSON
  - _Requirements: 11.1, 11.2_

- [ ] 13. Regression property tests: single-serializer configurations unchanged
  - Re-run existing HTTP/CoAP property test suites against `Types` bundles using
    `single_serializer_registry`, asserting identical wire behavior to pre-change
    (Requirement 7)
  - _Requirements: 7.1, 7.2, 7.3, 11.3_

- [ ] 14. Property tests: two-serializer negotiation
  - All four client-order × server-order combinations round-trip correctly for
    `request_vote`, `append_entries`, `install_snapshot` (and, where applicable,
    ClusterJoin/ClusterLeave, RequestPreVote, `fetch_log_entries`)
  - `Peer_Capability_Cache` converges to a stable `Media_Type` after first exchange
  - _Requirements: 11.4_

- [ ] 15. Integration tests: multi-serializer ↔ single-serializer interoperability
  - Both directions (multi-as-client/single-as-server and vice versa), over HTTP and CoAP
  - _Requirements: 7.3, 11.5_

- [ ] 16. Negative tests for content-negotiation failures
  - Unsupported `Content-Type`/`Content-Format` on input → 415/4.15
  - Unsatisfiable `Accept` on output → 406/4.06
  - Client-side unrecognized response `Content_Declaration` → error future, cache
    untouched
  - Assert no crash, hang, or unresolved future/promise in any case
  - _Requirements: 8.2, 8.3, 8.4, 8.5, 11.6_
