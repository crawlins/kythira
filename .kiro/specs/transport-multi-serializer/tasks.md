# Implementation Plan

- [ ] 1. Strengthen the `rpc_serializer` concept and add `media_type()`
  - Add `{ s.media_type() } -> std::convertible_to<std::string>;` to `rpc_serializer`
    (`include/raft/types.hpp`)
  - Add `media_type()` returning `"application/json"` to `json_rpc_serializer`
    (`include/raft/json_serializer.hpp`), leaving `name()` unchanged
  - Update `tests/rpc_serializer_concept_test.cpp` to assert `media_type()` presence
  - _Requirements: 2.1, 2.2, 2.3_

- [ ] 2. Define the `Serializer_Registry` concept
  - Add `serializer_registry<R, Data>` concept to `include/raft/types.hpp`:
    `default_media_type()`, `preferred_media_types()`, `supports(media_type)`,
    `select_output_media_type(accepted) -> std::optional<std::string>`
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6_

- [ ] 3. Implement `single_serializer_registry<S>` and `multi_serializer_registry<S...>`
  - New header `include/raft/serializer_registry.hpp`
  - `single_serializer_registry<S>`: wraps one serializer; `encode_with`/`decode_with`
    dispatch trivially, throwing `unsupported_media_type_error` on mismatch
  - `multi_serializer_registry<S1, ..., SN>` (`N` ≥ 2): `std::tuple<S1,...,SN>` storage;
    `S1` is the default; `encode_with`/`decode_with` fold over the tuple matching
    `media_type()` at runtime
  - `static_assert` both satisfy `serializer_registry`
  - _Requirements: 1.7, 1.8, 1.9, 1.10_

- [ ] 4. Add a minimal test-only second serializer
  - `tagged_rpc_serializer` (or similar) under `tests/` wrapping
    `json_rpc_serializer`'s wire bytes with a distinct `media_type()`
    (e.g. `"application/x-test-alt"`), used only to exercise `N` ≥ 2 registries in tests
  - _Requirements: 11.1_

- [ ] 5. Extend `transport_types` with `serializer_registry_type`
  - Add `typename T::serializer_registry_type` + `serializer_registry<...>` constraint to
    `transport_types` (`include/raft/types.hpp`), keeping `serializer_type` as the
    `Default_Serializer`'s type
  - Update `http_transport_types`, `std_http_transport_types`,
    `simple_http_transport_types` (`include/raft/http_transport.hpp`),
    `coap_transport_types`, `std_coap_transport_types`, `simple_coap_transport_types`,
    `default_transport_types` (`include/raft/coap_transport.hpp`), and
    `future_default_http_transport_types` (`include/raft/beast_http_transport.hpp`) to
    add `using serializer_registry_type = single_serializer_registry<RPC_Serializer>;`
  - Verify `raft_types` (node-internal bundle) is untouched
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 7.1, 7.2_

- [ ] 6. Add `unsupported_media_type_error` and its CoAP analogue
  - `include/raft/http_exceptions.hpp`: `unsupported_media_type_error : http_transport_error`
  - `include/raft/coap_exceptions.hpp`: matching CoAP exception type
  - _Requirements: 8.1_

- [ ] 7. Add `Media_Type`-keyed CoAP content-format lookup
  - `include/raft/coap_utils.hpp`: `media_type_to_coap_content_format(media_type) ->
    std::optional<coap_content_format>`
  - Document `get_content_format_for_serializer` as superseded, do not extend it further
  - Reject CoAP registry construction when a registered serializer's `media_type()` has
    no corresponding `coap_content_format` entry
  - _Requirements: 9.1, 9.2, 9.3_

- [ ] 8. Implement shared `Accept`/`Content-Type` header parsing helper
  - `parse_accept_header(std::string_view) -> std::vector<std::string>`: comma-split,
    trim, drop `;`-delimited parameters (e.g. `q=`), preserve order
  - Used by both `cpp_httplib_server` and `boost_beast_server`
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

- [ ] 12. Unit tests for the registry and negotiation logic
  - `select_output_media_type` empty/overlap/no-overlap cases
  - `encode_with`/`decode_with` dispatch and `unsupported_media_type_error` cases for
    both `single_serializer_registry` and `multi_serializer_registry`
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
