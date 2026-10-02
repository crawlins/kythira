# Design Document

## Overview

The gRPC transport moves from fixed `SslCredentials` and
`SslServerCredentials` to gRPC's certificate-provider TLS credentials
(`grpc::experimental::TlsServerCredentials` and `TlsCredentials`, built from
`TlsServerCredentialsOptions` and `TlsChannelCredentialsOptions`). Those
credentials do not hold a certificate. They hold a
`CertificateProviderInterface`, and gRPC asks the provider for the current
material on every new handshake. Changing what the provider returns is
therefore how a running server and existing channels pick up new material.
gRPC offers no API to swap the credentials of a built `grpc::Server`.

Everything above that is Kythira's own:

```
 operator files ──► file_tls_material_source ─┐
 PEM strings ─────► static_tls_material_source ├─► tls_material_source ──► grpc_tls_bridge ──► gRPC provider
 certificate_provider ► issuing_tls_material_source ┘   (validate, generation,   (one per transport)
                                                         subscribe)
```

- A **material source** (Requirement 5) is the one input.
  - The existing PEM fields become a `static_tls_material_source`.
  - The new `*_path` fields become a `file_tls_material_source`.
- `reload_tls_material()` and `enable_auto_reload()` (Requirements 1 to 3)
  are thin wrappers:
  - **Reload:** ask the source to re-read, then push the result through the
    bridge.
  - **Auto-reload:** let the file source poll.
- The **bridge** validates material with the existing
  `grpc_detail::validate_cert_key_pair` and `validate_pem_certificate`. It
  then hands the material to gRPC through whichever gRPC provider is
  available (below).

### Key design decision: which gRPC provider

gRPC 1.71.0 (pinned by `vcpkg-overlays/grpc`) is known to ship
`FileWatcherCertificateProvider` and `StaticDataCertificateProvider`. A
provider that can be updated in memory would let the bridge apply material
synchronously. Whether 1.71 has a public one is **unverified**, and Task 1
settles it against the installed headers.

- **Path A, in-memory provider available.** The bridge owns one and calls
  its update method. Apply is synchronous, so the Requirement 1.4 bound is
  zero, and no key touches disk.
- **Path B, file-watcher fallback.** The bridge owns a private staging
  directory, created with mode 0700 under `$XDG_RUNTIME_DIR` or the system
  temporary directory, and points a `FileWatcherCertificateProvider` at
  `<staging>/current/{key,chain,roots}.pem` with
  `refresh_interval_sec = tls_refresh_interval`. To publish generation N it:
  1. Writes `<staging>/gen-N/` (files mode 0600).
  2. `fsync`s.
  3. Atomically replaces the `current` symlink with `rename()` of a
     temporary symlink.
  4. Removes `gen-(N-2)`, keeping one generation back for a watcher that
     is mid-read.

  The bound is `tls_refresh_interval`, default 1 second, the smallest
  `refresh_interval_sec` accepts.

Both paths sit behind one class, so the rest of the transport cannot tell
which is in use. Path B exists so this spec does not depend on an
unverified API. If Task 1 finds Path A, Path B is not built.

### Key design decision: a new interface, not the `certificate_provider` concept

`certificate_provider` answers "sign this CSR". A transport needs "what is
my identity right now, and tell me when it changes". Folding the second
into the first would force every issuance backend to grow state and
threads. So `tls_material_source` is separate, and
`issuing_tls_material_source<P>` is the one adapter between them.

It is an abstract class rather than a concept because the gRPC config
structs are not templates, and holding a source in
`std::shared_ptr<tls_material_source>` keeps them that way.

### Key design decision: validate in Kythira, not in gRPC

gRPC's file watcher logs and skips bad files. That would make a failed
reload invisible to the caller and to metrics. The bridge validates before
anything reaches gRPC, so Requirement 1.2 (throw, keep old material) and
the `tls_reload.failed` metric work the same as on every other transport.
On Path B the watcher only ever sees material that has already passed.

## Components and Interfaces

### `include/raft/tls_material_source.hpp` (transport-neutral)

```cpp
namespace kythira {

struct tls_material {
    std::string certificate_chain_pem;  // identity, leaf first; empty = no identity
    std::string private_key_pem;        // empty iff certificate_chain_pem is empty
    std::string root_certificates_pem;  // empty = use system/default roots (client only)
};

class tls_material_source {
public:
    using callback = std::function<void(std::shared_ptr<const tls_material>, std::uint64_t generation)>;
    class subscription;  // RAII; destructor unsubscribes and waits for an in-progress callback

    virtual ~tls_material_source() = default;
    [[nodiscard]] virtual auto current() const -> std::shared_ptr<const tls_material> = 0;
    [[nodiscard]] virtual auto generation() const -> std::uint64_t = 0;
    [[nodiscard]] virtual auto subscribe(callback cb) -> subscription = 0;
    // Re-read the backing store now and publish if it changed. Throws on
    // invalid material (the previous material stays current).
    virtual auto refresh() -> void = 0;
    // True when the source detects changes itself (file polling, renewal).
    [[nodiscard]] virtual auto self_refreshing() const -> bool = 0;
};

class static_tls_material_source;  // refresh() re-publishes the same material
class file_tls_material_source;    // paths + optional poll interval
template<certificate_provider P> class issuing_tls_material_source;

}  // namespace kythira
```

`subscription`'s destructor taking a lock that callbacks also hold is what
satisfies Requirement 5.6. After it returns, no callback is running or will
start.

### `include/raft/grpc_tls_bridge.hpp` (gRPC-specific, private)

```cpp
namespace kythira::grpc_detail {
class grpc_tls_bridge {
public:
    enum class role { server, client };
    grpc_tls_bridge(std::shared_ptr<tls_material_source> source, role r,
                    bool require_peer_cert, std::chrono::seconds refresh_interval);
    auto server_credentials() -> std::shared_ptr<grpc::ServerCredentials>;
    auto channel_credentials(bool verify_server) -> std::shared_ptr<grpc::ChannelCredentials>;
    auto apply(const tls_material&) -> std::uint64_t;  // validate, publish; throws
};
}
```

### Transport changes (`grpc_transport.hpp` and `grpc_transport_impl.hpp`)

- **Config:**
  - New fields: `*_path`, `material_source`, `tls_refresh_interval{1}`.
  - Construction resolves exactly one source (Requirements 4.2 and 5.2).
- **Credentials:**
  - `build_server_credentials()` returns `bridge.server_credentials()`.
    The mutual-TLS mode is fixed from `require_client_cert`
    (`GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY` or
    `GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE`).
  - `build_channel_credentials()` returns
    `bridge.channel_credentials()`; the server is always verified.
    `target_name_override` keeps working through the existing channel
    argument.
- **`reload_tls_material()`:**
  1. Takes `_reload_mutex`.
  2. Calls `source->refresh()`, then `bridge.apply(*source->current())`.
  3. Emits `tls_reload.succeeded` or `failed`.
- **Auto-reload:**
  - `enable_auto_reload(interval)` throws when the source is self-refreshing
    or static. Otherwise it is the same mtime poll loop as the cpp-httplib
    transport, calling `reload_tls_material()`.
  - The transport subscribes to self-refreshing sources and calls
    `bridge.apply()` from the callback.
- **Channels:** the existing caches stay. No channel is recreated, because
  the provider serves new material to every future handshake.

### The insecure path

`enable_tls == false` keeps returning `InsecureServerCredentials` and
`InsecureChannelCredentials` and creates no bridge. Reload calls throw
`std::logic_error`. The plaintext gate from
`.kiro/specs/grpc-plaintext-opt-in/` is unaffected.

## Error Handling

| Situation | Behaviour |
|---|---|
| Invalid material on explicit reload | `grpc_tls_configuration_error`, old material stays, `tls_reload.failed` |
| Invalid material from a source callback or poll | old material stays, `tls_reload.failed`, retried next change or poll |
| Both `*_pem` and `*_path`, or a source plus either | `grpc_tls_configuration_error` at construction |
| Source at generation 0 | `grpc_tls_configuration_error` at construction |
| Staging directory cannot be created (Path B) | `grpc_tls_configuration_error` at construction |
| Issuance or renewal failure | keep current material, back off, `tls_material_source.renewal.failed` |

## Testing Strategy

The gRPC tests run in CI (`grpc_transport_integration_test`), so all of
these go there or into new binaries registered next to it.

1. **Server explicit reload** (Properties 1 and 2):
   - Start mTLS server and client on gen-1 material.
   - Open a server-streaming RPC (`RaftPeerReplicationService`).
   - Reload to gen-2 signed by the same CA.
   - Check that the open stream keeps delivering, and that a *new* client
     connection observes the gen-2 leaf. Observe the leaf through a raw
     OpenSSL handshake against the port, comparing serial numbers.
2. **Invalid reload keeps serving** (Property 3): write a mismatched key and
   certificate. `reload_tls_material()` throws, a new handshake still sees
   gen-1, and the failure metric is recorded once.
3. **Root rotation:**
   - The server's trust bundle switches from CA-1 to CA-2.
   - A client holding a CA-1 certificate is then refused on new
     connections.
   - A client reloaded to a CA-2 certificate is accepted.
4. **Client reload:** with a server requiring client certificates, the
   client reloads its identity, and the server's auth context on the next
   RPC shows the new client serial.
5. **Auto-reload:** atomically replace the files with
   `certificate_authority::replace_atomically`. Within
   `poll_interval + tls_refresh_interval` new handshakes see the new serial.
   Then disable auto-reload and check that the thread is joined.
6. **Shared source:** one `static_tls_material_source` feeds a server and a
   client, and both pick up one `apply`.
7. **Issuing source:**
   - Use `local_certificate_provider` with a short validity.
   - Check the first issuance publishes generation 1.
   - Check renewal at two thirds of the validity publishes generation 2 with
     a different key.
   - Use a failing provider stub to check that the backoff keeps the old
     material and emits the metric.
8. **No torn handshakes** (Property 4):
   - Run 200 back-to-back reloads alternating two identities while 8
     threads open fresh connections.
   - Every handshake's leaf must verify against its own key, so no handshake
     fails with a key mismatch, and every observed serial is one of the
     two.
9. **Static source unchanged:** the existing mTLS tests pass untouched,
   exercising the PEM-string path through `static_tls_material_source`.

## Correctness Properties

### Property 1: Established connections survive reload

*For any* reload, explicit or automatic, of a server or client, RPCs and
streams on connections established before the reload complete normally.
**Validates: Requirements 1.3, 2.3**

### Property 2: New handshakes see new material within the bound

*For any* successful reload completing at time t, every handshake starting
after `t + tls_refresh_interval` uses the new material. On Path A the bound
is zero.
**Validates: Requirements 1.3, 1.4, 2.3, 5.3**

### Property 3: Failed reload is invisible to peers

*For any* reload whose material fails validation, the material served
afterwards is identical to before, and a failure metric is emitted.
**Validates: Requirements 1.2, 2.2, 3.2, 5.3**

### Property 4: No torn material

*For any* handshake, the presented certificate, the private key used, and
the root bundle used all belong to the same generation.
**Validates: Requirement 7.1**

### Property 5: Never downgraded

*For any* reload sequence on a TLS-enabled transport, credentials stay
TLS credentials, and `require_client_cert` keeps its constructed value.
**Validates: Requirements 7.2, 7.3**

## Implementation Notes

- The `grpc::experimental` TLS API is marked experimental upstream. Keeping
  all of it inside `grpc_tls_bridge` means an upstream rename touches one
  file.
- The CA cluster's RPC mTLS renewal (ca-cluster-rpc-mtls Requirement 7) is a
  natural first user of `issuing_tls_material_source` once a binary adopts
  the gRPC transport. No binary does today.
