# Design Document

## Overview

This adds one gate to each side of the gRPC transport. When TLS is off, a
loopback-only address passes, `allow_plaintext` passes, and anything else is
refused at construction with `grpc_plaintext_refused_error`. The loopback
decision is the one the TCP RPC transport already uses
(`net_bind::is_loopback_bind_address()`, from PR #368 and PR #369), so "what
counts as loopback" is defined once for the whole project and never involves
DNS.

### Key design decision: gate in the library, not a binary

The TCP RPC opt-in lives in each binary's config (`ca_cluster_node`,
`redis_gateway_node`), because the TCP transport is used by binaries whose
flags differ. No binary uses the gRPC transport yet. Putting the gate in
`grpc_server_config` and `grpc_client_config` means every future embedder
gets it, and a binary only has to map its own flag onto `allow_plaintext`.

### Key design decision: keep `enable_tls = false` as the default

Flipping the default would make every loopback test, example and benchmark
supply certificates, while adding nothing on loopback. Refusing only the
exposed case gives the security property, plaintext never reaching the
network by accident, without that churn.

### Key design decision: fail closed on unparseable targets

A gRPC target string can name a resolver scheme this code does not
understand, such as `xds:` or a custom resolver. The check cannot know
where such a target leads, so it counts as non-loopback. An operator who
really wants plaintext there sets `allow_plaintext`.

## Components and Interfaces

### Configuration (`include/raft/grpc_transport.hpp`)

```cpp
struct grpc_client_config {
    // ... existing fields ...
    bool enable_tls{false};
    /// Permit plaintext channels to non-loopback targets. Ignored when
    /// enable_tls is true. See .kiro/specs/grpc-plaintext-opt-in/.
    bool allow_plaintext{false};
};

struct grpc_server_config {
    // ... existing fields ...
    bool enable_tls{false};
    /// Permit a plaintext listener on a non-loopback bind address. Ignored
    /// when enable_tls is true.
    bool allow_plaintext{false};
};
```

### Error (`include/raft/grpc_exceptions.hpp`)

`grpc_tls_configuration_error` gains a protected constructor taking a status
code, so the subclass can carry `FAILED_PRECONDITION`:

```cpp
class grpc_tls_configuration_error : public grpc_transport_error {
public:
    explicit grpc_tls_configuration_error(const std::string& message)
        : grpc_transport_error(grpc::StatusCode::INVALID_ARGUMENT, message) {}
protected:
    grpc_tls_configuration_error(grpc::StatusCode code, const std::string& message)
        : grpc_transport_error(code, message) {}
};

class grpc_plaintext_refused_error : public grpc_tls_configuration_error {
public:
    grpc_plaintext_refused_error(std::string address, const std::string& message)
        : grpc_tls_configuration_error(grpc::StatusCode::FAILED_PRECONDITION, message),
          _address(std::move(address)) {}
    [[nodiscard]] auto address() const -> const std::string& { return _address; }
private:
    std::string _address;
};
```

### Target classification (`grpc_detail`)

```cpp
namespace grpc_detail {
// True when every endpoint a gRPC target string can reach is on this host:
// loopback IPs, /etc/hosts names that map only to loopback, or a local
// socket scheme. Never resolves through DNS. Unknown schemes are false.
auto target_is_local(std::string_view target) -> bool;
}
```

Parsing, in order:

| Target form | Rule |
|---|---|
| `unix:…`, `unix-abstract:…`, `vsock:…` | local |
| `ipv4:a:p[,a:p…]` | every `a` in 127.0.0.0/8 |
| `ipv6:[a]:p[,…]` | every `a` is `::1` |
| `dns:[//auth/]host[:port]` | `is_loopback_bind_address(host)` |
| `[v6]:port` | `is_loopback_bind_address(v6)` |
| `host:port` (no scheme) | `is_loopback_bind_address(host)` |
| anything else | not local |

`host:port` is told apart from `scheme:rest` the way gRPC itself does it: a
string whose prefix before the first `:` is a registered scheme name (`dns`,
`ipv4`, `ipv6`, `unix`, `unix-abstract`, `vsock`) is a URI. Any other string
is a bare `host:port`. gRPC falls back to the `dns` resolver for those.

### Server gate

At the top of `build_server_credentials()`:

```cpp
if (!_config.enable_tls) {
    if (!_config.allow_plaintext &&
        !kythira::net_bind::is_loopback_bind_address(_bind_address)) {
        throw grpc_plaintext_refused_error(_bind_address, std::format(
            "grpc_server: refusing plaintext, unauthenticated Raft RPC on '{}' "
            "(enable TLS with server_cert_pem/server_key_pem, bind a loopback "
            "address, or set allow_plaintext on a network you trust)",
            _bind_address));
    }
    return grpc::InsecureServerCredentials();
}
```

`start()` emits `grpc.server.plaintext.enabled` when `allow_plaintext` was
what admitted the listener.

### Client gate

- In the constructor, after `build_channel_credentials()`: when the
  credentials are insecure and `allow_plaintext` is false, check every value
  of `_node_id_to_target` with `target_is_local()`.
- The address-keyed `get_or_create_channel()` runs the same check before
  `CreateCustomChannel`.
- The node-keyed overload needs no runtime check, because its targets were
  all vetted at construction.

## Error Handling

- Construction-time failures throw, consistent with
  `grpc_tls_configuration_error` today.
- The bootstrap path fails the single RPC and caches nothing, so a later
  call with a corrected address is not poisoned.

## Testing Strategy

`tests/grpc_transport_integration_test.cpp` (CI builds it on every leg):

1. Server on `0.0.0.0` with no TLS and no opt-in: the constructor throws
   `grpc_plaintext_refused_error`, `address() == "0.0.0.0"`, and the port is
   still free afterwards.
2. Server on `*` with no TLS and no opt-in: throws.
3. Server on `127.0.0.1`, `::1` (skipped without IPv6) and `localhost`: starts
   and serves plaintext.
4. Server on `0.0.0.0` with `allow_plaintext`: starts, and the metric is
   recorded.
5. Server on `0.0.0.0` with TLS: unaffected by the gate.
6. Client with a `10.0.0.1:5000` target and no TLS: the constructor throws.
7. Client with `allow_plaintext`: constructs.
8. Bootstrap call to a non-loopback address with no TLS: the RPC fails with
   the new error, and no channel is cached
   (`grpc.client.channel.created` is not emitted).

New `tests/grpc_target_classification_unit_test.cpp` (no network, table
driven): every row of the target table above, plus edge cases. These are
`127.255.255.254`, `::ffff:127.0.0.1` (counts as loopback, IPv4-mapped),
`dns://8.8.8.8/localhost:50051` (the host is `localhost`, so local), a
mixed `ipv4:` list (not local), an empty string, and `xds:///svc`. The
`/etc/hosts` cases use a temporary hosts file, as `tcp_rpc_unit_test` does.

## Correctness Properties

### Property 1: Plaintext never leaves the host without opt-in

*For any* server config with `enable_tls == false` and
`allow_plaintext == false`, the server either listens only on loopback-only
addresses or fails construction. *For any* client config with the same
settings, every channel it creates targets a local endpoint.
**Validates: Requirements 2.1, 3.1, 3.2**

### Property 2: The gate never weakens TLS

*For any* config with `enable_tls == true`, behaviour is identical with
`allow_plaintext` true or false.
**Validates: Requirement 1.3**

### Property 3: No DNS in the decision

*For any* target or bind address, classification performs no network
I/O. Tests run it with an unreachable resolver configuration and a
hosts-file fixture.
**Validates: Requirements 2.5, 3.4**
