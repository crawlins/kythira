# Requirements Document

## Introduction

This document specifies the requirements for extending the `transport_types` concept
(`include/raft/types.hpp`) and its conforming transports — `cpp_httplib_client`/
`cpp_httplib_server` (`include/raft/http_transport.hpp`), `boost_beast_client`/
`boost_beast_server` (`include/raft/beast_http_transport.hpp`),
`proxygen_client`/`proxygen_server` (`include/raft/proxygen_http_transport.hpp`), and
`coap_client`/`coap_server` (`include/raft/coap_transport.hpp`) — from a single,
compile-time-fixed `rpc_serializer` to a registry of 1–N `rpc_serializer`s.

**Amended August 7, 2026 to add Proxygen.** This document was drafted around the two
HTTP transports that existed at the time and named them explicitly throughout; the
third landed separately and nobody revisited the enumeration, so `proxygen_client`/
`proxygen_server` appeared nowhere in this spec while Tasks 9 and 10 gave the other two
full negotiation. That was omission, not decision — Proxygen is a first-class HTTP
transport, conforms to `transport_types` (hence already carries a
`serializer_registry_type`), and still contains the exact defect Requirement 9.4 exists
to remove: it hardcodes `"application/json"` on both client requests and server
responses, so a Proxygen node configured with `cbor_rpc_serializer` labels every message
JSON and lies on the wire. Every "…`cpp_httplib_*`/`boost_beast_*`…" enumeration below
should be read as including the Proxygen pair, and the acceptance criteria that name
them individually have been amended to say so.

Today every transport is templated on exactly one `serializer_type`, chosen once at
compile time. A `cpp_httplib_server<Types>` always decodes request bodies with that one
serializer and always answers with that one serializer's format, regardless of what the
peer actually sent or can consume — the HTTP server does not even inspect the incoming
`Content-Type` header, and the response `Content-Type` is a hardcoded
`"application/json"` constant (`include/raft/http_transport_impl.hpp`) irrespective of
which serializer is actually configured. The CoAP transport is closer to correct — it
already derives a CoAP `Content-Format` option value from the configured serializer via
`coap_utils::get_content_format_for_serializer` (`include/raft/coap_utils.hpp`) — but
that function guesses the format from a fuzzy substring match on the serializer's
human-readable `name()` (e.g. `"json"` anywhere in the string) and, like HTTP, supports
only the one serializer the transport was compiled with.

This feature lets a single transport instance be configured with several serializers
(e.g. JSON and CBOR) at once and, at runtime, (a) pick which serializer to use for
*input* by reading the format actually named in the incoming message's headers
(`Content-Type` for HTTP, `Content-Format` option for CoAP), and (b) pick which
serializer to use for *output* by intersecting its own registered serializers against
what the message's recipient has declared it can accept (`Accept` header for HTTP,
`Accept` option for CoAP). This is standard HTTP/CoAP content negotiation, applied to
Raft's pluggable RPC serialization layer.

Out of scope: the gRPC transport (`.kiro/specs/grpc-transport/`) fixes its wire format
to Protocol Buffers and its `grpc_transport_types` concept deliberately has no
`serializer_type` at all (Requirement 14.2 of that spec) — nothing here applies to it.
The TCP transports (`tcp_rpc.hpp`, `tls_tcp_rpc.hpp`) do not use `rpc_serializer` or
`transport_types` and have no header/option concept to negotiate over — also unaffected.
`simulator_network_client`/`simulator_network_server` (`include/raft/simulator_network.hpp`),
used by `default_raft_types` and most of the property-test suite, are likewise
unaffected: they are templated directly on a single `Serializer` template parameter
(constrained only by `rpc_serializer<Serializer, Data>`, never by `transport_types`),
and their simulated "wire" is an in-memory byte buffer passed straight to
`serialize`/`deserialize` with no header, option, or other metadata channel to carry a
`Content_Declaration`/`Accept_Declaration` over — there is nothing for this feature to
attach to. A future spec could add a `serializer_registry_type` to
`raft_simulator_network_types` for symmetry (so simulator-driven tests can exercise
multi-serializer configurations without a real HTTP/CoAP server), but that is
explicitly not required by this spec.

## Glossary

- **Serializer_Registry**: The runtime object, owned by a transport client or server,
  that holds an ordered collection of 1–N `rpc_serializer`-conforming serializer
  instances and answers content-negotiation questions against them (what to decode with,
  what to encode with, what to advertise).
- **Media_Type**: A short, stable, machine-comparable string identifying a wire format —
  e.g. `"application/json"`, `"application/cbor"` — distinct from a serializer's existing
  free-form `name()` (used today only for logs/metrics and CoAP's fuzzy substring match).
- **Content_Negotiation**: The process of choosing a `Media_Type` to encode an outgoing
  message with, given the recipient's declared acceptable `Media_Type`s, and the process
  of choosing which registered serializer decodes an incoming message, given the
  `Media_Type` it declares itself to be.
- **Accept_Declaration**: The set of `Media_Type`s a message's eventual recipient has
  said it can consume — carried on the HTTP `Accept` header of a request, or as one or
  more CoAP `Accept` options (Option 17) on a request.
- **Content_Declaration**: The single `Media_Type` a message actually is, as sent —
  carried on the HTTP `Content-Type` header, or a single CoAP `Content-Format` option
  (Option 12).
- **Default_Serializer**: The first serializer in a `Serializer_Registry`'s configured
  order; used whenever a peer has made no `Accept_Declaration` (absent header/option),
  preserving today's single-serializer behavior byte-for-byte.
- **Peer_Capability_Cache**: A per-target-node-id record, held by a `network_client`,
  of the `Media_Type` most recently observed in a response's `Content_Declaration` from
  that target — used to pick the output serializer for the *next* request to the same
  target without re-negotiating from scratch every time.
- **Unsupported_Media_Type_Error**: The error raised when an incoming message's
  `Content_Declaration` names a `Media_Type` no serializer in the local registry
  supports, or when a peer's `Accept_Declaration` and the local registry have no
  `Media_Type` in common.
- **single_serializer_registry**: A `Serializer_Registry` implementation wrapping exactly
  one `rpc_serializer`, provided so existing single-serializer `Types` bundles migrate by
  adding one type alias rather than rewriting transport code.
- **multi_serializer_registry**: A variadic `Serializer_Registry` implementation over
  `Serializer_1, ..., Serializer_N` (`N` ≥ 2), constructed in preference order.

## Requirements

### Requirement 1

**User Story:** As a systems architect, I want a serializer registry abstraction, so that
a transport can be configured with more than one `rpc_serializer` at a time instead of
exactly one fixed at compile time.

#### Acceptance Criteria

1. WHEN a `Serializer_Registry` type is defined THEN the system SHALL require it to
   expose the `Media_Type` of its `Default_Serializer` (`default_media_type()`).
2. WHEN a `Serializer_Registry` type is defined THEN the system SHALL require it to
   expose every configured serializer's `Media_Type`, in registration/preference order
   (`preferred_media_types()`).
3. WHEN a `Serializer_Registry` type is defined THEN the system SHALL require a
   `supports(media_type)` query that reports whether a given `Media_Type` string matches
   a registered serializer.
4. WHEN a `Serializer_Registry` type is defined THEN the system SHALL require a
   `select_output_media_type(accepted)` operation that, given an ordered
   `Accept_Declaration`, returns the first `Media_Type` in that declaration that a
   registered serializer also supports.
5. WHEN `select_output_media_type` is called with an empty `Accept_Declaration` THEN the
   system SHALL return `default_media_type()`.
6. WHEN `select_output_media_type` is called with a non-empty `Accept_Declaration` that
   shares no `Media_Type` with the registry THEN the system SHALL report a negotiation
   failure (see Requirement 8) rather than silently falling back to the default.
7. WHEN a `Serializer_Registry` is asked to encode a message for a chosen `Media_Type`
   THEN the system SHALL dispatch to the one registered serializer whose `Media_Type`
   matches, without the caller needing to know which index/type that serializer is.
8. WHEN a `Serializer_Registry` is asked to decode a message declared as a given
   `Media_Type` THEN the system SHALL dispatch to the one registered serializer whose
   `Media_Type` matches, and SHALL raise `Unsupported_Media_Type_Error` if none matches.
9. WHEN `single_serializer_registry<S>` is instantiated THEN the system SHALL satisfy the
   `Serializer_Registry` concept using exactly `S` as both the default and only
   serializer, reproducing today's single-serializer behavior exactly.
10. WHEN `multi_serializer_registry<S1, ..., SN>` is instantiated with `N` ≥ 2 conforming
    serializer types THEN the system SHALL satisfy the `Serializer_Registry` concept,
    treating `S1` as the `Default_Serializer` and `S1, ..., SN` as the preference order
    used to populate `preferred_media_types()`.

### Requirement 2

**User Story:** As a developer implementing a new `rpc_serializer`, I want a stable,
machine-comparable format identifier on the serializer itself, so that transports and
registries can key content negotiation on it instead of guessing from a free-form name.

#### Acceptance Criteria

1. WHEN the `rpc_serializer` concept is defined THEN the system SHALL additionally
   require a `media_type()` member returning a value convertible to `std::string`,
   alongside the existing `serialized_data`-based `Data` constraint.
2. WHEN `json_rpc_serializer` is updated THEN the system SHALL add a `media_type()`
   method returning `"application/json"`, leaving its existing `name()` method (used by
   metrics and the pre-existing CoAP content-format lookup) unchanged.
3. WHEN a serializer's `media_type()` is queried twice for the same instance THEN the
   system SHALL return the same value both times (`media_type()` is a pure function of
   the serializer's format, not its content).
4. WHEN two distinct serializer types are registered in the same `Serializer_Registry`
   THEN the system SHALL NOT require their `media_type()` values to be distinct, but a
   registry SHOULD be constructed with distinct `Media_Type`s per entry so that
   `select_output_media_type` and `supports` behave unambiguously; constructing a
   registry with a duplicate `Media_Type` is a caller error the system need not detect at
   compile time.

### Requirement 3

**User Story:** As a developer maintaining `transport_types`, I want the concept itself
to require a `Serializer_Registry` instead of (or in addition to) a single
`serializer_type`, so that HTTP, Beast, and CoAP transports can be templated on 1–N
serializers through one consistent interface.

#### Acceptance Criteria

1. WHEN the `transport_types` concept (`include/raft/types.hpp`) is updated THEN the
   system SHALL require a `serializer_registry_type` member satisfying the
   `Serializer_Registry` concept over `std::vector<std::byte>`.
2. WHEN the `transport_types` concept is updated THEN the system SHALL retain the
   existing `serializer_type` member, redefined as the `Default_Serializer`'s type,
   satisfying `rpc_serializer<serializer_type, std::vector<std::byte>>` as before, so
   that existing code paths that name `Types::serializer_type` directly keep compiling.
3. WHEN an existing single-serializer `Types` bundle (e.g. `http_transport_types<S, M,
   E>`) is migrated THEN the system SHALL allow it to satisfy the updated
   `transport_types` concept by adding `using serializer_registry_type =
   single_serializer_registry<S>;` with no other changes to the bundle.
4. WHEN the `transport_types` concept's `future_template` requirements are checked THEN
   the system SHALL leave them unchanged from the current definition (this feature does
   not touch future/executor/metrics validation).
5. WHEN `raft_types` (`include/raft/types.hpp`) — the separate concept used for the
   Raft node's own `network_client_type`/`network_server_type` bundle — is reviewed THEN
   the system SHALL leave it unmodified, since it names `serializer_type` for a single,
   node-internal persistence/log-entry serializer unrelated to inter-node wire
   negotiation.

### Requirement 4

**User Story:** As a Raft server receiving an RPC, I want to decode the request body
using whichever serializer the request's headers actually declare, so that a server
configured with several serializers can accept requests encoded in any of them.

#### Acceptance Criteria

1. WHEN `cpp_httplib_server`/`boost_beast_server`/`proxygen_server` receives a request THEN the system
   SHALL read the request's `Content-Type` header and treat it as the `Content_Declaration`.
2. WHEN the request has no `Content-Type` header THEN the system SHALL decode using the
   registry's `Default_Serializer`, matching today's behavior for single-serializer
   configurations.
3. WHEN the request's `Content-Type` names a `Media_Type` present in the server's
   `Serializer_Registry` THEN the system SHALL decode the request body with the matching
   serializer before invoking the registered RPC handler.
4. WHEN the request's `Content-Type` names a `Media_Type` absent from the server's
   `Serializer_Registry` THEN the system SHALL respond with HTTP status 415 Unsupported
   Media Type and SHALL NOT invoke the registered RPC handler.
5. WHEN `coap_server` receives a request THEN the system SHALL read the request's
   `Content-Format` option as the `Content_Declaration`, decode with the matching
   registered serializer, and respond with CoAP response code 4.15 (Unsupported Content-
   Format) without invoking the handler if no registered serializer matches.
6. WHEN a request carries no `Content-Format` option THEN `coap_server` SHALL decode
   using the registry's `Default_Serializer`, matching today's behavior.

### Requirement 5

**User Story:** As a Raft node responding to an RPC, I want to encode the response using
a serializer the original requester can actually consume, so that nodes with
heterogeneous but overlapping serializer support can interoperate.

#### Acceptance Criteria

1. WHEN `cpp_httplib_server`/`boost_beast_server`/`proxygen_server` builds a response THEN the system
   SHALL read the originating request's `Accept` header as the `Accept_Declaration`,
   parsed as an ordered, comma-separated list of `Media_Type` tokens (any `;`-delimited
   parameters, e.g. `q=`, SHALL be ignored for ordering purposes in this version).
2. WHEN the request has no `Accept` header THEN the system SHALL encode the response
   with the registry's `Default_Serializer` and set `Content-Type` accordingly, matching
   today's single-serializer behavior.
3. WHEN the request's `Accept` header names at least one `Media_Type` the server's
   registry supports THEN the system SHALL encode the response using the first such
   `Media_Type`, in the order it appears in the `Accept` header, and SHALL set the
   response's `Content-Type` header to that `Media_Type`.
4. WHEN the request's `Accept` header names only `Media_Type`s absent from the server's
   registry THEN the system SHALL respond with HTTP status 406 Not Acceptable instead of
   guessing a format the requester cannot parse.
5. WHEN `coap_server` builds a response THEN the system SHALL apply the same selection
   logic against the request's repeated `Accept` options (Option 17) and set exactly one
   `Content-Format` option (Option 12) on the response, using CoAP response code 4.06
   (Not Acceptable) when no requested format is supported.

### Requirement 6

**User Story:** As a Raft client sending an RPC, I want to advertise every serializer
format I can parse a response in, and to encode my own request in a format the target
is likely to accept, so that repeat calls to the same peer converge on a working format
without renegotiating from scratch on every call.

#### Acceptance Criteria

1. WHEN `cpp_httplib_client`/`boost_beast_client`/`proxygen_client`/`coap_client` sends a request THEN the
   system SHALL set an `Accept` header (HTTP) or repeated `Accept` options (CoAP) listing
   every `Media_Type` in `preferred_media_types()`, in that order.
2. WHEN the client has no prior `Peer_Capability_Cache` entry for the target node THEN
   the system SHALL encode the outgoing request body with the registry's
   `Default_Serializer` and set `Content-Type`/`Content-Format` accordingly.
3. WHEN the client has a `Peer_Capability_Cache` entry for the target node whose cached
   `Media_Type` is still present in the client's own registry THEN the system SHALL
   encode the outgoing request body with that cached `Media_Type`'s serializer instead
   of the default.
4. WHEN a response is received THEN the system SHALL read the response's
   `Content-Type`/`Content-Format` `Content_Declaration` and use the matching registered
   serializer to decode it; and WHEN that decode succeeds THEN the system SHALL update
   the `Peer_Capability_Cache` entry for that target node (creating it on first contact)
   to the `Media_Type` the **request** was encoded in — the attempt the peer did not
   answer with 415/4.15 — and SHALL NOT set it from the `Media_Type` of the response.
   The two are different facts. This cache is read only to pick a *request* encoding, so
   the evidence it needs is evidence about what the peer decodes; a peer is nowhere
   obliged to accept the `Media_Type` it replies in, and one that decodes cbor while
   always answering json would otherwise be cached as json and pay a rejected first
   attempt on every subsequent call rather than converging after one.
5. WHEN a response's `Content_Declaration` names a `Media_Type` absent from the client's
   own registry THEN the system SHALL set the pending future/promise to an error state
   with `Unsupported_Media_Type_Error` and SHALL NOT update the `Peer_Capability_Cache`.
6. WHEN a response has no `Content_Declaration` at all (header/option absent) THEN the
   system SHALL decode using the registry's `Default_Serializer`, matching today's
   behavior. The cache update in 6.4 still applies: it is drawn from the request, so an
   absent response declaration leaves nothing about it to be uncertain of.
7. WHEN a peer accepts requests in one `Media_Type` and answers in a different one, both
   of which the client supports, THEN the system SHALL issue at most one rejected
   attempt across all calls to that peer — not one per call. This is the property 6.4's
   choice of `Media_Type` exists to hold, and it is observable only as an attempt count:
   a client that never converges completes every RPC too, one round trip slower each.

### Requirement 7

**User Story:** As an operator running a mixed-version or mixed-configuration cluster, I
want single-serializer transports to behave exactly as they do today, so that adopting
this feature is opt-in and never a breaking change for existing deployments.

#### Acceptance Criteria

1. WHEN a `Types` bundle's `serializer_registry_type` is `single_serializer_registry<S>`
   THEN the system SHALL never emit an `Accept` header/option listing more than one
   `Media_Type`, and SHALL never receive a negotiable choice among more than one
   `Media_Type` on that transport instance.
2. WHEN two nodes both run single-serializer transports of the same serializer type THEN
   the system SHALL produce identical wire traffic (headers, options, and bodies) to the
   pre-existing implementation, modulo the previously-hardcoded-JSON `Content-Type` on
   `cpp_httplib_server` now correctly reflecting the configured serializer's
   `media_type()` (this is a bug fix bundled with this feature, not a behavior this
   requirement freezes — see Requirement 9).
3. WHEN a node running a multi-serializer transport talks to a node running a
   single-serializer transport of a serializer the multi-serializer node also supports
   THEN the system SHALL interoperate correctly in both directions using the mechanisms
   in Requirements 4–6, without requiring the single-serializer node to change.

### Requirement 8

**User Story:** As a reliability engineer, I want unresolvable content-negotiation
failures to surface as a distinct, well-defined error rather than a generic
deserialization failure or a silently wrong format, so that I can diagnose serializer
mismatches quickly.

#### Acceptance Criteria

1. WHEN a content-negotiation failure occurs (unsupported `Content_Declaration` on
   input, or no mutually supported `Media_Type` for output) THEN the system SHALL raise
   a dedicated `unsupported_media_type_error` exception (extending `http_transport_error`
   for HTTP/Beast; the CoAP-transport analogue extending the existing CoAP exception
   hierarchy) distinct from `serialization_error`.
2. WHEN `cpp_httplib_server`/`boost_beast_server`/`proxygen_server` raises this error on the input path
   THEN the system SHALL translate it to HTTP status 415 as specified in Requirement 4.
3. WHEN `cpp_httplib_server`/`boost_beast_server`/`proxygen_server` raises this error on the output-
   negotiation path THEN the system SHALL translate it to HTTP status 406 as specified in
   Requirement 5.
4. WHEN a client-side (`cpp_httplib_client`/`boost_beast_client`/`proxygen_client`/`coap_client`) content-
   negotiation failure occurs on a received response THEN the system SHALL set the
   pending future/promise to an error state carrying `unsupported_media_type_error`
   rather than a generic parse exception.
5. WHEN any content-negotiation failure occurs THEN the system SHALL NOT crash the
   process and SHALL NOT leave a request unanswered/a future unresolved.

### Requirement 9

**User Story:** As a developer relying on the CoAP content-format mapping, I want the
existing fuzzy `name()`-substring lookup replaced by a direct `Media_Type`-keyed lookup,
so that content-format selection is unambiguous once serializers expose `media_type()`.

#### Acceptance Criteria

1. WHEN `coap_utils` is updated THEN the system SHALL add a lookup from `Media_Type`
   string (e.g. `"application/json"`, `"application/cbor"`) to `coap_content_format`
   enumerator, used in place of `get_content_format_for_serializer`'s substring matching
   wherever a `Serializer_Registry`-based transport needs to map a chosen `Media_Type` to
   a CoAP Content-Format option value.
2. WHEN a registered serializer's `media_type()` has no corresponding
   `coap_content_format` enumerator THEN the system SHALL reject that serializer's
   registration in a CoAP `Serializer_Registry` at construction time with a descriptive
   exception, rather than silently sending an undefined or wrong option value.
3. WHEN the pre-existing `get_content_format_for_serializer`/`name()`-based path is kept
   for backward compatibility THEN the system SHALL document it as superseded by the
   `Media_Type`-keyed lookup and not extend it further.
4. WHEN `cpp_httplib_server` is fixed to stop hardcoding `content_type_json` (Requirement
   7.2's bundled bug fix) THEN the system SHALL source the response `Content-Type` from
   the negotiated `Media_Type` exclusively, for both single- and multi-serializer
   registries.

### Requirement 10

**User Story:** As a system operator, I want content-negotiation outcomes visible in
existing transport metrics, so that I can see which serializer formats peers are
actually using and how often negotiation fails.

#### Acceptance Criteria

1. WHEN an HTTP/Beast/CoAP client sends a request or a server sends a response THEN the
   system SHALL add a `media_type` dimension (carrying the negotiated `Media_Type`) to
   the existing per-RPC request/response metrics already emitted by that transport.
2. WHEN a content-negotiation failure occurs (Requirement 8) THEN the system SHALL emit
   an error metric on the existing `*.error` metric name with `error_type` set to
   `"unsupported_media_type"`, dimensioned the same way as other errors from that
   transport.
3. WHEN metrics are added THEN the system SHALL NOT change the name, dimensions, or
   semantics of any existing metric beyond adding the new `media_type` dimension.

### Requirement 11

**User Story:** As a testing engineer, I want the multi-serializer registry and its
content-negotiation behavior to be verifiable through concept `static_assert`s, unit
tests, and property-based tests, so that regressions are caught before they reach a
running cluster.

#### Acceptance Criteria

1. WHEN unit tests are executed THEN the system SHALL verify via `static_assert` that
   `single_serializer_registry<json_rpc_serializer<...>>` and a second, minimal
   test-only serializer type combined in `multi_serializer_registry<...>` both satisfy
   the `Serializer_Registry` concept.
2. WHEN unit tests are executed THEN the system SHALL verify
   `select_output_media_type`'s three cases: empty `Accept_Declaration` (falls back to
   default), non-empty with overlap (returns the requester's earliest mutually-supported
   `Media_Type`), and non-empty with no overlap (reports negotiation failure).
3. WHEN property-based tests are executed for HTTP and CoAP THEN the system SHALL verify
   that a single-serializer `Types` configuration negotiates trivially and reproduces
   today's wire behavior for every RPC type (`request_vote`, `append_entries`,
   `install_snapshot`, and, where applicable, the ClusterJoin/ClusterLeave,
   RequestPreVote, and `fetch_log_entries` extensions).
4. WHEN property-based tests are executed for a two-serializer configuration THEN the
   system SHALL verify that requests/responses round-trip correctly for every
   combination of client-registry-order × server-registry-order, and that the
   `Peer_Capability_Cache` converges to a stable `Media_Type` after the first successful
   exchange with a given target.
5. WHEN integration tests are executed THEN the system SHALL verify that a
   multi-serializer node interoperates with a single-serializer node configured with one
   of the shared formats, in both the multi-serializer-as-client and
   multi-serializer-as-server directions.
6. WHEN tests are executed THEN the system SHALL verify that an
   `unsupported_media_type_error` is raised (not a crash, hang, or silent misparse) for
   every case enumerated in Requirement 8.
