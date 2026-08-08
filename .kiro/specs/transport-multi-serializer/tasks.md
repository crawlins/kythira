# Implementation Plan

**Status (August 8, 2026): all 17 top-level tasks are ticked, over both HTTP and
CoAP.** The wiring is complete across all four transports — cpp-httplib (9),
Beast (10), Proxygen (10a) and CoAP (11) — and the four test suites (13-16) are
in. One thing remains open inside a ticked task, stated rather than glossed: a
**Requirement 7.3 interop violation** that Task 15 found and pinned rather than
fixed, because fixing it is a design decision — see Task 15 and `doc/TODO.md`.

Writing the test suites to the requirements rather than to the code found two
defects, which is the main reason Tasks 13-16 were worth doing beyond the
checkbox. One is the 7.3 violation above. The other — **CoAP's 4.06 branch was
unreachable**, so a peer that could read none of our formats got a success
carrying a body it could not decode — was a local bug with no design question
attached, and is fixed under Task 16.

Count the tasks before quoting a denominator: this file has **17** top-level
tasks, not 16 — `10a` was inserted between 10 and 11 on August 7, 2026 and is
easy to miss when skimming the numbering. It also has 29 leaf items.

The original status note read "Tasks 1-4, 6 and 12 are implemented", describing
the protocol-independent core — the concepts, the two registries, the
exceptions, and their unit tests — which landed as one self-contained change.
Everything from Task 5 onward was transport wiring, deliberately left as
separate work because it touches every `Types` bundle and all four transports.

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

- [x] 9. Wire content negotiation into `cpp_httplib_client`/`cpp_httplib_server`
  - [x] 9.1 Client request path: media type from `peer_capability_cache` when the
    registry still supports it, else `default_media_type()`; `Content-Type` set to
    it; `Accept` set from `preferred_media_types()` on **every** request, not just
    the first. That last part is what makes the cache an optimisation rather than
    a protocol: a stale entry costs one re-choice by the peer, never a failure
    - _Requirements: 6.1, 6.2, 6.3_
  - [x] 9.2 Client response path: `Content-Type` read and parameter-stripped
    (absent or empty -> default); unsupported -> future fails with
    `unsupported_media_type_error` and the cache is left **untouched**; on a
    clean decode the cache records the peer's choice
    - _Requirements: 6.4, 6.5, 6.6_
  - [x] 9.3 Server request path: `Content-Type` read, unsupported -> 415 **before**
    the handler runs
    - _Requirements: 4.1, 4.2, 4.3, 4.4_
  - [x] 9.4 Server response path: `Accept` parsed, `select_output_media_type`,
    `std::nullopt` -> 406 with no body. **Also negotiated before the handler runs**,
    which the task did not specify: answering 406 afterwards produces the same
    status while having already done the work, and on a handler with side effects
    that difference is observable
    - _Requirements: 5.1, 5.2, 5.3, 5.4_
  - [x] 9.5 Response `Content-Type` is the negotiated type on the success path;
    error bodies stay `text/plain`, which is what they actually are.
    `content_type_for_serializer` is deleted, not merely unused — its premise
    ("client and server always share the same serializer_type, so this is
    advisory labeling only") is exactly what negotiation invalidates
    - _Requirements: 9.4_
  - [x] 9.6 `media_type` dimension added to client request/response and server
    response metrics, and `error_type=unsupported_media_type` on both sides.
    `http.server.request.received` deliberately carries **no** `media_type`: it is
    emitted before the header is read, and reading earlier would mean parsing a
    header before counting the request that carried it, losing the count entirely
    for a malformed request
    - _Requirements: 10.1, 10.2, 10.3_

- [x] 10. Wire content negotiation into `boost_beast_client`/`boost_beast_server`
  - Mirrors Task 9 through Beast's header API. The one structural difference:
    `boost_beast_server::dispatch()` previously took only the body, so the
    negotiated types are now threaded through it, and `success_content_type()` is
    replaced by `default_media_type()`. A single "success content type" stopped
    being a meaningful thing to ask a server for once the answer could differ per
    exchange; the session now learns what was encoded from `dispatch` itself,
    on every path including the error ones
  - The per-endpoint `deserialize` lambdas are gone — `decode_with<Request>` needs
    only the media type and the `Request` type parameter `handle` already has
  - Beast distinguishes 415 from 400 explicitly. They are different problems with
    different fixes: 415 means "we do not speak this encoding", 400 means "we speak
    it and your bytes were wrong". Collapsing them tells a peer to fix its payload
    when it needs to change its format
  - _Requirements: 4.1-4.4, 5.1-5.4, 6.1-6.6, 9.4, 10.1-10.3_

- [x] 10a. Wire content negotiation into `proxygen_client`/`proxygen_server`
  - **Checkboxes reconciled August 8, 2026.** The four subtasks below shipped
    with the task's own write-up in #175 but were never ticked, so this task
    read as not-started while its code was on `main` — which made the spec's
    own task denominator untrustworthy. Each box below was re-checked against
    the tree before ticking, with the verifying line named; none was ticked on
    the strength of the prose above it
  - Added August 7, 2026, after the fact. This spec was drafted around the two
    HTTP transports that existed then and enumerated them by name throughout;
    Proxygen landed separately and nobody revisited the enumeration, so Tasks 9
    and 10 gave httplib and Beast full negotiation while Proxygen kept the
    hardcoded `"application/json"` that Requirement 9.4 exists to remove — on the
    server response (`proxygen_http_transport_impl.hpp`, `rpc_request_handler::onEOM`)
    and on both client request paths (`send_on_session`, `send_on_session_folly`).
    Numbered `10a` rather than appended as `17` because it belongs with 9 and 10,
    and because 11-16 are referenced by number from `doc/TODO.md` and from Task
    10's own write-up — renumbering them to make room would break those
    references to buy nothing
  - [x] 10a.1 Client: add `serializer_registry_type _registry` and a
    `peer_capability_cache<std::uint64_t>`; pick the request `Media_Type` via
    `select_request_media_type`, encode through the registry, and set both
    `Content-Type` and the full `Accept` list
    - Members at `proxygen_http_transport.hpp:467,473`; the selection and both
      headers at `proxygen_http_transport_impl.hpp:837` (generic bridge) and
      `:1038` (Folly fast path) — both paths, per the note below
    - _Requirements: 6.1, 6.2, 6.3, 9.4_
  - [x] 10a.2 Client: read the response `Content-Type` (absent → registry default),
    reject an unsupported one with `unsupported_media_type_error` while leaving the
    cache untouched, decode through the registry, and record the cache entry only
    on a clean decode
    - `proxygen_http_transport_impl.hpp:932-961` and `:1104-1133`. The
      `_capability_cache.record` call sits *after* a successful
      `decode_with`, and the unsupported branch throws before reaching it, so
      "untouched" is structural rather than a comment
    - _Requirements: 6.4, 6.5, 6.6_
  - [x] 10a.3 Server: thread the request `Media_Type` and parsed `Accept` list into
    `proxygen_server::dispatch`, and report back the `Media_Type` actually encoded;
    415 before the handler on an unsupported `Content-Type`, 406 before the handler
    on an unsatisfiable `Accept`
    - Signature at `proxygen_http_transport_impl.hpp:1657`, call site at `:1259`.
      Both rejections precede the `handler(request)` call at `:1722`, and both
      precede the `decode_with` at `:1715` — so an unsatisfiable `Accept` also
      costs no decode
    - _Requirements: 4.1-4.4, 5.1-5.4_
  - [x] 10a.4 Add the `media_type` dimension to the `proxygen_http.*` metrics and
    emit `error_type=unsupported_media_type` on negotiation failure
    - Client request/response at `:854` and `:1054`, client error at `:942-943`
      and `:1114-1115`, server error at `:1692` (415) and `:1707` (406), server
      latency at `:1739`. The 406 metric carries the *rejected `Accept` list*
      rather than a single type, since no single type was unsupported — the
      whole list was unsatisfiable
    - _Requirements: 10.1, 10.2, 10.3_
  - **Both client paths, not one.** Unlike httplib and Beast, Proxygen has two
    client send paths — the generic bridge (Requirement 14) and the Folly-native
    fast path (Requirement 16) — chosen by an `if constexpr` on the future backend.
    They are separate function bodies with separate transaction bridges
    (`transaction_bridge` / `folly_transaction_bridge`), so negotiating in one and
    not the other would make a node's wire behaviour depend on which future backend
    it was compiled with. Both get the same treatment, and the shared
    `http_response` struct carries the response `Content-Type` so the two bridges
    cannot diverge on how they read it
  - **Pinned by `tests/proxygen_negotiation_integration_test.cpp`**, 10 cases,
    driving a live `proxygen_server` with a raw `httplib::Client` — a foreign
    client is the only way to send the headers that reach 415 and 406, which a
    Proxygen client would never emit. Covers the supported/labelled happy path,
    415 before the handler, 406 before the handler, an absent `Content-Type`
    falling back to the default, httplib's injected `text/plain` being rejected
    like any other unknown type, a `charset` parameter not defeating the match,
    `*/*` and `type/*` wildcards, an `Accept` list picking the first supported
    entry, and a malformed body in a *supported* type being 400 rather than 415
  - _Requirements: 4.1-4.4, 5.1-5.4, 6.1-6.6, 9.4, 10.1-10.3_

- [x] 11. Wire content negotiation into `coap_client`/`coap_server`
  - [x] 11.1 Client: set `Content-Format` from cached/default `Media_Type`; set repeated
    `Accept` options from `preferred_media_types()`
    - _Requirements: 6.1, 6.2, 6.3_
  - [x] 11.2 Client: read response `Content-Format` (absent → default); decode via
    registry; on mismatch set error state; on success update `Peer_Capability_Cache`
    - _Requirements: 6.4, 6.5, 6.6_
  - [x] 11.3 Server: read request `Content-Format` (absent → default); decode via
    registry; on mismatch respond 4.15 without invoking the handler
    - _Requirements: 4.5, 4.6_
  - [x] 11.4 Server: collect request's `Accept` options; `select_output_media_type`; on
    `std::nullopt` respond 4.06; else encode via registry and set one `Content-Format`
    option
    - _Requirements: 5.5_
  - [x] 11.5 Add `media_type` dimension to existing CoAP metrics; emit
    `error_type=unsupported_media_type` on negotiation failure
    - _Requirements: 10.1, 10.2, 10.3_

  - **Two things specific to CoAP that HTTP did not have**, and that will trap
    the next person editing this transport:
    1. **The Content-Format option was being written wrong, everywhere.** Every
       site used `coap_add_option(..., sizeof(uint16_t), (const uint8_t*)&value)`,
       which puts the host-order bytes of the integer on the wire — on a
       little-endian box Content-Format 60 (CBOR) went out as 15360. Nothing
       noticed because nothing ever *read* the option; both ends assumed their
       own single serializer. Reading it is exactly what negotiation does, so
       11.1/11.4 switch to `coap_encode_var_safe`. Note CoAP strips leading zero
       bytes, so Content-Format 0 (`text/plain`) is a legitimately *zero-length*
       option value — an assertion of `len > 0` is wrong.
    2. **The inverse mapping has to be resolved through the registry**, not by a
       second table. `media_type_to_coap_content_format` is not injective (Ion
       binary and text share 65000; protobuf and octet-stream share 42), so a
       standalone reverse table would have to invent an answer. Scanning the
       registry cannot, because `validate_registry_content_formats` has already
       rejected any registry holding a collision — within a validated registry
       the mapping is a bijection.
  - **Negotiation is scoped to unicast RPC.** The multicast paths and the
    serialization cache still use the fixed `_serializer`: multicast has no
    per-peer negotiation to do, and the cache is keyed by request content alone,
    so serving a cached body for a negotiated type would mislabel it. The cache
    is therefore consulted only for the default media type, which is every
    single-serializer deployment.
  - **Coverage, checked by mutation rather than assumed**: putting a bogus
    Content-Format on the client's request makes `coap_cbor_end_to_end_test`
    fail, so that suite really does round-trip the negotiation — while
    `coap_integration_test`, `coap_post_method_property_test` and
    `coap_content_format_property_test` all still pass, i.e. they do not
    negotiate. `coap_content_negotiation_unit_test` was added for the pieces
    those leave uncovered. The 4.15/4.06 wire branches remain uncovered by any
    test; they belong to Tasks 13-16.

  - **Interop note recorded by `http_negotiation_integration_test`**: a
    cpp-httplib peer that POSTs without an explicit `Content-Type` now gets 415.
    httplib's *client* injects `Content-Type: text/plain` into any POST with a
    body and no declared type, and this server does not speak text/plain. Before
    negotiation the server ignored the header and decoded with its own
    serializer, so such a request succeeded. Node-to-node traffic is unaffected
    (both clients always set the header explicitly); a hand-rolled third-party
    caller relying on the old leniency is not. Spec'd behaviour per Requirement
    4.3, pinned by a test so reversing it has to be deliberate

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

**Tasks 13-16, August 8, 2026: the HTTP half is done; the CoAP half is not.**
All four suites are implemented, mutation-tested and green over cpp-httplib.
Tasks 15 and 16 also name CoAP, and that remains open — see the note under Task
16. The four share one rig, `tests/negotiation_test_harness.hpp`, whose whole
job is to make the negotiated media type *observable*: it carries a recording
metrics backend and the suites read the `media_type` dimension back. A
round-trip that merely succeeds proves the two sides agreed on something, not
that they negotiated rather than both defaulting to the same thing and ignoring
the headers, and that distinction is the entire subject of these tasks.

- [x] 13. Regression property tests: single-serializer configurations unchanged
  - `tests/single_serializer_regression_test.cpp`, 4 cases. Driven by a raw
    `httplib::Client` rather than by ours, because a pre-negotiation peer is
    exactly what `cpp_httplib_client` can no longer imitate — it always sets
    both `Accept` and `Content-Type`, and the peers Requirement 7.3 is about set
    neither
  - **The load-bearing assertion is byte equality against the serializer called
    directly**, not a successful round-trip. A round-trip also passes for a
    registry that wrapped or re-framed the payload, provided it did so
    symmetrically at both ends — and such a node would be silently unable to
    talk to any unmodified peer, which is the precise regression Requirement 7
    exists to prevent
  - The CBOR case is what makes the suite able to fail: the JSON cases cannot
    distinguish "labelled with the configured serializer's type" from "always
    labelled `application/json`", because for them the two answers coincide.
    Mutation-tested — restoring the hardcoded `application/json` that
    Requirement 9.4 removed fails the CBOR case and *only* the CBOR case
  - Also pins the two ways a pre-negotiation peer can present: no `Accept` header
    at all, and `Accept: */*`. Both must be served rather than answered 406,
    since a 406 would mean every unmodified node in an existing cluster stopped
    being able to talk to an upgraded one
  - _Requirements: 7.1, 7.2, 7.3, 11.3_

- [x] 14. Property tests: two-serializer negotiation
  - `tests/multi_serializer_negotiation_property_test.cpp`, 5 cases. **The first
    test anywhere to run a `multi_serializer_registry` through a socket**: every
    shipped bundle hardcodes `single_serializer_registry`, so until this suite
    declared a test-local bundle, the feature the whole spec exists for had
    never been exercised end to end
  - All four client-order × server-order combinations, each over `request_vote`,
    `append_entries` and `install_snapshot`. Three RPCs rather than one because
    each has its own endpoint and its own `handle` instantiation in `dispatch`,
    so negotiation wired into one and not the others would round-trip perfectly
    on whichever one a single-RPC test happened to pick
  - The property asserted is **the negotiated type is the client's first
    preference, whatever the server's own order is** — `select_output_media_type`'s
    "the peer's order wins" rule, observed on the wire. The two mixed-order
    cells are what distinguish it from "our order wins"; the two same-order
    cells cannot, and mutation-testing confirms exactly that split
  - Cache convergence is asserted as *stability across three exchanges* plus
    agreement with the server-side type. "Request 2 matches request 1" alone
    would also hold for a cache never written to at all; the "our order wins"
    mutation proves the assertion is not vacuous, because under it the cache
    records the server's choice and requests 2 and 3 diverge from request 1
  - **A second mutation measured what the tree covered before this suite: with
    the client's `Accept` header suppressed entirely,
    `http_negotiation_integration_test`, `http_integration_test` and
    `http_client_test` all still pass.** Nothing protected that header, because
    every shipped bundle is single-serializer — where a missing `Accept` still
    yields the one type the server has. Worth knowing before trusting any
    existing suite to cover a negotiation change
  - ClusterJoin/ClusterLeave, RequestPreVote and `fetch_log_entries` are not
    covered: they are not part of the `cpp_httplib_client`/`server` RPC surface
    these suites drive, which is why the task text hedged them as "where
    applicable"
  - _Requirements: 11.4_

- [x] 15. Integration tests: multi-serializer ↔ single-serializer interoperability
  - `tests/multi_serializer_interop_test.cpp`, 5 cells, over HTTP. **One of them
    pins a genuine Requirement 7.3 violation rather than a pass** — see below
  - The two directions are not symmetric, which is why this is a separate suite
    rather than more cells in Task 14. HTTP negotiates the *response* through
    `Accept`, which the server reads before answering, so a single-serializer
    server can always satisfy a multi-serializer client's response leg — the
    three passing cells. The *request* carries whatever the client chose before
    it had heard anything from the server; there is no mechanism by which a
    client learns a peer's formats in advance, so the request leg is not
    negotiated at all. It is guessed, from the registry default
  - **Defect found and since fixed: a multi-serializer client whose default the
    single-serializer server does not speak was permanently broken.** Measured —
    the client sent `application/cbor`, the JSON-only server answered 415, the
    handler was never entered, and nothing recovered: no retry, and the
    capability cache is deliberately not written on failure, so every subsequent
    request repeated the identical mistake. Requirement 7.3 says these two SHALL
    interoperate
  - **Fixed August 8, 2026 by a blind retry**: on 415/4.15 the client walks the
    rest of `preferred_media_types()` and caches whichever works. `Accept-Post`
    (W3C Linked Data Platform 1.0 §7.1) would converge in one retry instead of
    up to N, but it requires the *peer* to emit a header and 7.3 promises
    interoperation without the peer changing — and it has no CoAP analogue,
    while this policy has to hold across all four transports. It is an
    optimisation to layer on later, not the mechanism. Full reasoning in
    `doc/TODO.md`
  - The cell is now `..._interoperates_with_a_single_json_server` and asserts
    the *shape* of the recovery rather than a uniform success: four attempts for
    three RPCs — CBOR refused, JSON accepted, then two more straight out in JSON
    from the cache the retry populated. A client that retried on every request
    would show six, and one that never retried would show three failures
  - Mutation-tested with a mutation the other suites' mutations do not catch —
    the server ignoring the request's `Accept` entirely. It kills the two cells
    where the client's and server's preferences differ and leaves the three that
    cannot distinguish it passing
  - _Requirements: 7.3, 11.5_

- [x] 16. Negative tests for content-negotiation failures
  - `tests/negotiation_failure_test.cpp`, 4 cases, covering **the one failure
    class of the four that had no test anywhere**: a client receiving a response
    labelled with a media type its registry does not know. The other three are
    server-side and were already pinned — 415 and 406 by
    `http_negotiation_integration_test` and `proxygen_negotiation_integration_test`,
    both of which reach those branches by driving our server with a raw
    `httplib::Client`
  - Reaching the client-side branch needs the mirror-image trick: a **server**
    that answers something ours never would. `cpp_httplib_server` cannot be made
    to emit an unregistered response label — it only ever answers with a type its
    own registry selected, which is correct, and is exactly why the client's
    handling of a non-conforming peer is untestable through it. So the suite
    stands a raw `httplib::Server` in front of a real `cpp_httplib_client`
  - Cases: an unregistered response type fails the future with the *typed*
    `unsupported_media_type_error` naming the offending type (a generic
    `runtime_error` would be indistinguishable from a transport fault, and the
    two call for opposite responses); an **absent** response `Content-Type`
    falls back to the client default rather than erroring (Requirement 6.4's
    client-side half — the server-side half was covered, and they are separate
    code paths); a *supported* label over foreign bytes surfaces as a decode
    error rather than a media-type error (Requirement 8.3); and a failed
    negotiation leaves the client usable for the next request
  - **"Cache untouched" is asserted as its observable consequence, not directly.**
    The cache is private, and probing it through the request label would prove
    nothing: `select_request_media_type` ignores a cached type the registry no
    longer supports, so a wrongly-recorded bogus entry gets filtered out on the
    way back and looks identical to never having been recorded. What is
    genuinely observable is that the client still works afterwards, which the
    last case checks against the same client instance
  - Requirement 11.6's "no crash, hang, or unresolved future" holds structurally:
    every case takes a future and completes it, with a client request timeout
    well under ctest's, so a hang fails the case by name instead of killing the
    run with no attribution
  - Mutation-tested: suppressing the client's `supports()` guard fails the two
    cases that depend on it and correctly leaves the other two passing
  - **The CoAP half of Tasks 15 and 16 is done too**, as
    `tests/coap_negotiation_failure_test.cpp` — 5 cases driving a real
    `coap_server` with a raw libcoap client. That peer is necessary rather than
    stylistic, exactly as on the HTTP side: `coap_client` always sends a
    `Content-Format` and `Accept` drawn from its own registry, so 4.15 and 4.06
    cannot be reached from it at all. Covers 4.15 for an unregistered format
    *and* for a format the codebase knows but this server's registry lacks (the
    pair proves the server rejects on its own registry rather than on the global
    table), an absent `Content-Format` falling back to the default, a
    multi-serializer server honouring the peer's `Accept` over its own
    preference, and 4.06
  - **Writing the 4.06 case to the requirement found that the branch was
    unreachable**, and it is fixed in the same change. The `Accept` loop dropped
    options it could not resolve through the registry, so an `Accept` naming
    only unsupported formats collapsed to an empty list — which correctly means
    "no preference stated" and yields the default. Every surviving entry had
    also been resolved *through the registry*, so `select_output_media_type`
    could never reject it. The server therefore answered `2.05 Content` with a
    body the peer had just said it could not decode: worse than a wrong status
    code, because the client then reports a deserialization failure pointing at
    the payload rather than at the negotiation. HTTP never had this because
    `parse_accept_header` keeps unsupported entries verbatim, so its list stays
    non-empty and the registry does the rejecting
  - The CoAP-specific trap recorded under Task 11 held up in practice: options
    in the new test are written with `coap_encode_var_safe`, and nothing asserts
    a non-zero option length, since CoAP strips leading zeros and Content-Format
    0 (`text/plain`) is legitimately zero-length
  - _Requirements: 8.2, 8.3, 8.4, 8.5, 11.6_
