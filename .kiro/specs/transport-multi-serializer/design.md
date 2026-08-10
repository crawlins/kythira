# Design Document

## Overview

This design extends `transport_types` (`include/raft/types.hpp`) and the transports that
depend on it — `cpp_httplib_client`/`cpp_httplib_server`, `boost_beast_client`/
`boost_beast_server`, `proxygen_client`/`proxygen_server`, and `coap_client`/`coap_server`
— so that a single transport instance can hold 1–N `rpc_serializer`s instead of exactly
one fixed at compile time, and performs standard content negotiation over them:

- **Input** (decoding a received message): determined strictly by the format the message
  *declares itself to be* — the HTTP `Content-Type` header or CoAP `Content-Format`
  option already on the wire today.
- **Output** (encoding a message to send): determined by intersecting the local
  serializer registry against what the *recipient* has declared it can accept — the HTTP
  `Accept` header or CoAP `Accept` option(s), which are new on the request path and are
  read (not just set) for the first time by this feature.

The mechanism is additive over the existing single-serializer path: a registry of
exactly one serializer (`single_serializer_registry<S>`) reproduces today's behavior
exactly, so existing `Types` bundles migrate with a one-line addition.

Not touched: `simulator_network_client`/`simulator_network_server`
(`include/raft/simulator_network.hpp`). They take a single `Serializer` template
parameter directly (constrained by `rpc_serializer<Serializer, Data>`, not by
`transport_types`), and their simulated transport is a raw in-memory byte buffer with no
header/option channel — there is no `Content_Declaration`/`Accept_Declaration` to read or
set. `default_raft_types` and the bulk of the property-test suite run over this
simulator, so they remain single-serializer and unaffected by this feature; they serve
as the pre-existing regression backstop referenced in the Testing Strategy below, not as
a negotiation target.

## Architecture

### Current state (single serializer)

```
                    ┌──────────────────────────┐
                    │   Types::serializer_type  │   fixed at compile time
                    └────────────┬─────────────┘
                                 │
        ┌────────────────────────┴────────────────────────┐
        │                                                  │
┌───────▼────────┐                                ┌────────▼───────┐
│ cpp_httplib_    │   always encodes/decodes as    │ cpp_httplib_   │
│ client<Types>   │◄──────────────────────────────►│ server<Types>  │
└─────────────────┘   Types::serializer_type only  └────────────────┘
```

The server never inspects the request's `Content-Type`; the response `Content-Type` is a
hardcoded `"application/json"` constant regardless of the configured serializer.

### New state (serializer registry + content negotiation)

```
┌────────────────────────────────────────────────────────────────────┐
│                    Types::serializer_registry_type                 │
│   holds 1..N rpc_serializer instances, in preference order         │
│   S1 (= Default_Serializer), S2, ..., SN                           │
└───────────────────────────────┬──────────────────────────────────-─┘
                                 │ satisfies Serializer_Registry concept
        ┌────────────────────────┴────────────────────────┐
        │                                                  │
┌───────▼─────────────────┐                     ┌──────────▼─────────────┐
│ cpp_httplib_client       │   request:          │ cpp_httplib_server      │
│ /boost_beast_client      │   Content-Type: S?  │ /boost_beast_server     │
│ /coap_client             │   Accept: S1,..,SN  │ /coap_server            │
│                          │ ───────────────────►│                        │
│  reads Peer_Capability_  │                     │  decode by Content-Type │
│  Cache[target] to pick   │   response:         │  encode by best match   │
│  S? for next request     │   Content-Type: Sk  │  of Accept ∩ registry   │
│                          │ ◄───────────────────│                        │
│  decode by response      │                     │                        │
│  Content-Type; cache Sk  │                     │                        │
│  for target               │                     │                        │
└──────────────────────────┘                     └────────────────────────┘
```

CoAP mirrors this exactly using `Content-Format` (Option 12, single-valued) for
`Content_Declaration` and `Accept` (Option 17, repeatable) for `Accept_Declaration`.

## Components and Interfaces

### 1. `rpc_serializer` concept — add `media_type()`

```cpp
// include/raft/types.hpp
template<typename S, typename Data>
concept rpc_serializer = requires(const S& s) {
    requires serialized_data<Data>;
    typename S;
    { s.media_type() } -> std::convertible_to<std::string>;
};
```

`json_rpc_serializer` gains one method:

```cpp
// include/raft/json_serializer.hpp
[[nodiscard]] auto media_type() const -> std::string { return "application/json"; }
```

`name()` (used today by metrics and `coap_utils::get_content_format_for_serializer`) is
untouched — `media_type()` is the new canonical, negotiation-facing identifier;
`name()` stays a free-form, human-oriented label.

### 2. `Serializer_Registry` concept

```cpp
// include/raft/types.hpp
template<typename R, typename Data>
concept serializer_registry = requires(const R& r, const std::string& media_type,
                                        const std::vector<std::string>& accepted) {
    requires serialized_data<Data>;
    { r.default_media_type() } -> std::convertible_to<std::string>;
    { r.preferred_media_types() } -> std::same_as<std::vector<std::string>>;
    { r.supports(media_type) } -> std::same_as<bool>;
    { r.select_output_media_type(accepted) } -> std::same_as<std::optional<std::string>>;
};
```

`select_output_media_type` returns `std::nullopt` on negotiation failure (Requirement
1.6) rather than throwing, so callers on the hot path can choose the appropriate
protocol-specific error (415 vs. 406, 4.06 vs. 4.15) instead of catching a generic
exception. `encode_with`/`decode_with` (below) are *not* part of the concept body
because — like `transport_types::future_template` already does for futures — they are
template-on-message-type operations that cannot be named generically for "any type"; they
are instead validated the same way `transport_types` already validates `future_template`:
by instantiating them against the three core RPC types and checking the result compiles
and matches the expected shape.

### 3. Registry implementations

```cpp
// include/raft/serializer_registry.hpp (new header)
template<typename S>
requires rpc_serializer<S, std::vector<std::byte>>
class single_serializer_registry {
public:
    [[nodiscard]] auto default_media_type() const -> std::string { return _s.media_type(); }
    [[nodiscard]] auto preferred_media_types() const -> std::vector<std::string> {
        return {_s.media_type()};
    }
    [[nodiscard]] auto supports(const std::string& mt) const -> bool {
        return mt == _s.media_type();
    }
    [[nodiscard]] auto select_output_media_type(const std::vector<std::string>& accepted) const
        -> std::optional<std::string> {
        if (accepted.empty()) return default_media_type();
        for (const auto& mt : accepted) {
            if (supports(mt)) return mt;
        }
        return std::nullopt;
    }

    template<typename Request>
    [[nodiscard]] auto encode_with(const std::string& media_type, const Request& req) const
        -> std::vector<std::byte> {
        if (media_type != _s.media_type()) throw unsupported_media_type_error(media_type);
        return _s.serialize(req);
    }

    template<typename T>
    [[nodiscard]] auto decode_with(const std::string& media_type,
                                    const std::vector<std::byte>& data) const -> T {
        if (media_type != _s.media_type()) throw unsupported_media_type_error(media_type);
        return _s.template deserialize<T>(data);
    }

private:
    S _s;
};

template<typename... Serializers>
requires (sizeof...(Serializers) >= 2) &&
         (rpc_serializer<Serializers, std::vector<std::byte>> && ...)
class multi_serializer_registry {
public:
    [[nodiscard]] auto default_media_type() const -> std::string {
        return std::get<0>(_serializers).media_type();
    }
    [[nodiscard]] auto preferred_media_types() const -> std::vector<std::string> {
        std::vector<std::string> result;
        std::apply([&](const auto&... s) { (result.push_back(s.media_type()), ...); },
                   _serializers);
        return result;
    }
    [[nodiscard]] auto supports(const std::string& mt) const -> bool {
        return std::apply([&](const auto&... s) { return ((s.media_type() == mt) || ...); },
                           _serializers);
    }
    [[nodiscard]] auto select_output_media_type(const std::vector<std::string>& accepted) const
        -> std::optional<std::string> {
        if (accepted.empty()) return default_media_type();
        for (const auto& mt : accepted) {
            if (supports(mt)) return mt;
        }
        return std::nullopt;
    }

    template<typename Request>
    [[nodiscard]] auto encode_with(const std::string& media_type, const Request& req) const
        -> std::vector<std::byte> {
        std::optional<std::vector<std::byte>> result;
        std::apply(
            [&](const auto&... s) {
                ((s.media_type() == media_type ? (result = s.serialize(req), void()) : void()),
                 ...);
            },
            _serializers);
        if (!result) throw unsupported_media_type_error(media_type);
        return std::move(*result);
    }

    template<typename T>
    [[nodiscard]] auto decode_with(const std::string& media_type,
                                    const std::vector<std::byte>& data) const -> T {
        std::optional<T> result;
        std::apply(
            [&](const auto&... s) {
                ((s.media_type() == media_type
                      ? (result = s.template deserialize<T>(data), void())
                      : void()),
                 ...);
            },
            _serializers);
        if (!result) throw unsupported_media_type_error(media_type);
        return std::move(*result);
    }

private:
    std::tuple<Serializers...> _serializers;
};
```

`encode_with`/`decode_with` fold over the tuple at compile time and dispatch at runtime
on `media_type()` equality — the same "enumerate known concrete types, `if constexpr`
your way through them" pattern the transports already use for RPC message dispatch
(`handle_rpc_endpoint`'s `if constexpr (std::is_same_v<Request, ...>)` chain).

### 4. `transport_types` concept — add `serializer_registry_type`

```cpp
// include/raft/types.hpp
template<typename T>
concept transport_types =
    requires {
        typename T::serializer_type;          // Default_Serializer's type (unchanged name)
        typename T::serializer_registry_type;  // NEW
        typename T::metrics_type;
        typename T::executor_type;
    } &&
    kythira::rpc_serializer<typename T::serializer_type, std::vector<std::byte>> &&
    kythira::serializer_registry<typename T::serializer_registry_type, std::vector<std::byte>> &&
    kythira::metrics<typename T::metrics_type> &&
    requires {
        typename T::template future_template<kythira::request_vote_response<>>;
        typename T::template future_template<kythira::append_entries_response<>>;
        typename T::template future_template<kythira::install_snapshot_response<>>;
    } &&
    future<typename T::template future_template<kythira::request_vote_response<>>,
           kythira::request_vote_response<>> &&
    future<typename T::template future_template<kythira::append_entries_response<>>,
           kythira::append_entries_response<>> &&
    future<typename T::template future_template<kythira::install_snapshot_response<>>,
           kythira::install_snapshot_response<>>;
```

Migration for an existing single-serializer bundle:

```cpp
// Before
struct http_transport_types { // include/raft/http_transport.hpp
    using serializer_type = json_serializer;
    template<typename T> using future_template = folly::Future<T>;
    using executor_type = folly::CPUThreadPoolExecutor;
    using metrics_type = noop_metrics;
};

// After — one line added, nothing else changes
struct http_transport_types {
    using serializer_type = json_serializer;
    using serializer_registry_type = single_serializer_registry<json_serializer>;
    template<typename T> using future_template = folly::Future<T>;
    using executor_type = folly::CPUThreadPoolExecutor;
    using metrics_type = noop_metrics;
};

// Opting into multiple serializers
struct http_multi_transport_types {
    using serializer_type = json_serializer;  // still the default
    using serializer_registry_type = multi_serializer_registry<json_serializer, cbor_serializer>;
    template<typename T> using future_template = folly::Future<T>;
    using executor_type = folly::CPUThreadPoolExecutor;
    using metrics_type = noop_metrics;
};
```

`raft_types` (the separate concept for the node's own `network_client_type`/
`network_server_type`/persistence bundle) is untouched — it names a single
`serializer_type` used for a conceptually different purpose (the node-internal
serializer contract that `network_client`/`network_server` implementations must satisfy)
and does not participate in wire-level content negotiation.

### 5. HTTP request/response flow

**Client (`cpp_httplib_client`/`boost_beast_client`/`proxygen_client`), sending:**

1. Look up `Peer_Capability_Cache[target]`. If present and still `registry.supports(...)`,
   use it as the output `Media_Type`; otherwise use `registry.default_media_type()`.
2. `body = registry.encode_with(media_type, request)`.
3. Set `Content-Type: <media_type>`.
4. Set `Accept: <registry.preferred_media_types() joined with ", ">`.
5. Send.

**Client, receiving a response:**

1. Read `Content-Type` from the response. If absent, treat as `registry.default_media_type()`.
   Call it `response_media_type`, and keep it distinct from the `media_type` the *request*
   was sent with — step 3 turns on the difference.
2. If `registry.supports(response_media_type)`:
   `response = registry.decode_with<Response>(response_media_type, body)`.
3. On a clean decode, update `Peer_Capability_Cache[target] = media_type` — the
   **request**'s type, not `response_media_type` (Requirement 6.4). The cache is read
   only by step 1 of *sending*, so what it must hold is what the peer decodes; a peer
   that answers in a type it will not accept is otherwise re-taught the same lesson on
   every call. See `peer_capability_cache.hpp`'s file comment.
4. Else: fail the future with `unsupported_media_type_error` and leave the cache
   untouched (Requirement 6.5).

**Server (`cpp_httplib_server`/`boost_beast_server`/`proxygen_server`), receiving a request:**

1. Read `Content-Type`. Absent → `registry.default_media_type()`.
2. If unsupported → HTTP 415, do not invoke the handler.
3. Else `request = registry.decode_with<Request>(content_type, body)`; invoke handler.

**Server, sending the response:**

1. Parse the request's `Accept` header into an ordered list (comma-split, trim
   whitespace, drop anything after a `;`).
2. `out = registry.select_output_media_type(accepted)`.
3. If `std::nullopt` → HTTP 406, do not send a body.
4. Else `body = registry.encode_with(*out, response)`; set `Content-Type: *out`; send 200.

`Accept` header parsing and `q`-value stripping is implemented once as a small free
function (e.g. `parse_accept_header(std::string_view) -> std::vector<std::string>` in a
shared header, since `cpp_httplib_server`, `boost_beast_server` and `proxygen_server`
all need it) rather than duplicated per transport.

### 6. CoAP request/response flow

Same logic, different wire encoding:

- `Content_Declaration` ⇔ the single CoAP `Content-Format` option (Option 12), whose
  numeric value is looked up via the new `Media_Type`-keyed table in `coap_utils.hpp`
  (Requirement 9), not the old `name()`-substring guess.
- `Accept_Declaration` ⇔ zero or more repeated CoAP `Accept` options (Option 17), each
  carrying one `coap_content_format` code; a request's `Accept` options list is built
  from `registry.preferred_media_types()` mapped through the same table.
- Negotiation failure on input → CoAP response code 4.15 (Unsupported Content-Format).
- Negotiation failure on output → CoAP response code 4.06 (Not Acceptable).

Because CoAP's Content-Format registry is a small closed set of IANA-registered integers,
a `Serializer_Registry` used by `coap_client`/`coap_server` must be constructed only from
serializers whose `media_type()` maps to a known `coap_content_format` — enforced at
construction time (Requirement 9.2), not discovered lazily on the first mismatched call.

```cpp
// include/raft/coap_utils.hpp — new, media_type-keyed
inline auto media_type_to_coap_content_format(const std::string& media_type)
    -> std::optional<coap_content_format> {
    static const std::unordered_map<std::string, coap_content_format> table{
        {"application/json", coap_content_format::application_json},
        {"application/cbor", coap_content_format::application_cbor},
        {"application/xml", coap_content_format::application_xml},
        {"text/plain", coap_content_format::text_plain},
    };
    auto it = table.find(media_type);
    return it == table.end() ? std::nullopt : std::optional{it->second};
}
```

### 7. Error handling

```cpp
// include/raft/http_exceptions.hpp — new, alongside http_client_error/http_server_error
class unsupported_media_type_error : public http_transport_error {
public:
    explicit unsupported_media_type_error(const std::string& media_type)
        : http_transport_error("Unsupported media type: " + media_type), _media_type(media_type) {}

    [[nodiscard]] auto media_type() const -> const std::string& { return _media_type; }

private:
    std::string _media_type;
};
```

The CoAP transport gains the analogous `coap_unsupported_content_format_error` in
`coap_exceptions.hpp`, following the existing CoAP exception hierarchy's naming.

`cpp_httplib_server::handle_rpc_endpoint`/`boost_beast_server`'s equivalent catch this
specifically (before the generic `catch (const std::exception&)` that today maps
everything to 400) and translate it to 415 (input path) or 406 (output-negotiation path)
per Requirement 8.2/8.3, instead of falling through to the generic 400 Bad Request the
current catch-all produces.

### 8. Metrics

Every existing `http.client.request.*`, `http.server.request.*`,
`coap.client.request.*`, `coap.server.request.*` metric gains a `media_type` dimension
set to the negotiated `Media_Type` for that call (Requirement 10.1). A negotiation
failure emits the existing `*.error` metric with `error_type=unsupported_media_type`
(Requirement 10.2) — no new metric names, only a new dimension value and a new
`error_type` value on metrics that already exist.

### 9. `Peer_Capability_Cache`

A small `std::unordered_map<NodeId, std::string>` (or `Address` for bootstrap-only
calls that address by contact address rather than node id) owned by the client, guarded
by the same mutex the client already uses for its connection map. Read before encoding a
request (Requirement 6.3), written after successfully decoding a response (Requirement
6.4), never written on a negotiation failure (Requirement 6.5).

**What it stores is the request `Media_Type` the peer accepted, not the one it answered
in.** The map is read on exactly one path — choosing how to encode the next request — so
the only fact that belongs in it is a fact about what the peer decodes. The response's
`Content-Type` is a different fact wearing the same shape: the two coincide for our own
server, and a foreign peer may answer in a type it would reject on the way in. Storing
the response's type makes such a peer permanently miscached, and the miss is re-paid on
every call rather than once, which is precisely the bound the paragraph below claims.

No expiry/TTL — a peer that changes what it decodes will simply have its cache entry
corrected by the next exchange that succeeds, and in the meantime the request will still
succeed anyway, since the client always also advertises its full `Accept` list on every
request, giving the peer a fresh chance to pick a still-mutually-supported format even if
the cached guess is stale.

## Testing Strategy

- **Concept `static_assert`s** (mirroring `tests/rpc_serializer_concept_test.cpp`):
  `single_serializer_registry<json_rpc_serializer<...>>` and
  `multi_serializer_registry<json_rpc_serializer<...>, <test-only second serializer>>`
  both satisfy `serializer_registry`; `json_rpc_serializer` satisfies the strengthened
  `rpc_serializer` (now requiring `media_type()`).
- **A minimal second test serializer**: since the repository currently ships only
  `json_rpc_serializer`, exercising `multi_serializer_registry` (`N` ≥ 2) requires a
  lightweight test-only `rpc_serializer` (e.g. a `tagged_rpc_serializer` that wraps
  `json_rpc_serializer`'s bytes with a distinct `media_type()` such as
  `"application/x-test-alt"`) added under `tests/`, not a real second production codec.
- **Unit tests**: `select_output_media_type`'s three branches (empty → default,
  overlap → requester's earliest match, no overlap → `std::nullopt`); registry
  `encode_with`/`decode_with` dispatch and their `unsupported_media_type_error` cases.
- **Property-based regression tests**: a single-serializer `Types` bundle run through the
  existing HTTP/CoAP property test suites (`tests/http_transport_return_types_property_test.cpp`,
  `tests/coap_*_property_test.cpp`) must continue to pass unchanged — this is the
  guarantee behind Requirement 7.
- **Property-based negotiation tests**: two-serializer client × two-serializer server,
  all four registry-order combinations, verifying request/response round-trips and that
  the `Peer_Capability_Cache` converges after the first exchange.
- **An asymmetric peer** — one that accepts requests in a `Media_Type` it never answers
  in (Requirement 6.7). A real server cannot be configured this way (its decodable set
  comes from its registry *type*), so this needs a hand-rolled peer, and it is the only
  shape that tells a cache of accepted types from a cache of answered ones. Asserted as
  an **attempt count**, since both converge-immediately and never-converge complete every
  RPC.
- **Integration tests**: multi-serializer node ↔ single-serializer node, both directions,
  over both HTTP and CoAP.
- **Negative tests**: unsupported `Content-Type` on input (415/4.15), unsatisfiable
  `Accept` on output (406/4.06), and the client-side symmetric cases on a malformed or
  unrecognized response `Content_Declaration` (Requirement 6.5), all asserting a
  well-typed error rather than a crash, hang, or silently wrong deserialization.
