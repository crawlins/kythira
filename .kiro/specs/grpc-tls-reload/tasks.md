# Implementation Plan

## Implementation Status

Implemented, on the design's Path B.

**Task 1 findings** (gRPC 1.71.0 headers and
`src/core/lib/security/credentials/tls/grpc_tls_certificate_provider.cc`):

- The only public providers are `StaticDataCertificateProvider` and
  `FileWatcherCertificateProvider`. Neither can be updated in memory, so
  Path A is not available and Path B is built.
- `FileWatcherCertificateProvider` clamps `refresh_interval_sec` to at least
  1 second, so that is the reload bound.
- It never checks that the key matches the certificate. It detects a torn
  identity read only by comparing each file's mtime before and after reading,
  as `time_t` (whole seconds). The bridge therefore stamps every
  generation's files with a whole-second mtime strictly greater than the
  last one's, so a symlink swap between reading the key and the chain is
  always noticed and retried. `grpc_tls_bridge_test` pins that invariant.
- It reads the roots and the identity separately, so a swap between those
  two reads can pair one generation's roots with the next generation's
  identity until the next pass, one refresh interval later. The key and
  chain still always match. This window is documented in the README. It
  only matters when a single reload changes both roots and identity.

**Where the code differs from the design text:**

- The bridge takes the initial `tls_material` rather than a source. The
  shared reload logic (resolving the one source, explicit reload,
  subscription, file polling) lives in `grpc_detail::grpc_tls_reloader`, in
  the same header, and both transports hold one.
- Configuration must use PEM fields only or path fields only. Mixing them
  (say, cert and key paths with `ca_cert_pem`) is refused rather than
  stitched together.
- A reload cannot add or remove an identity or roots, because gRPC fixes
  what the provider watches when credentials are built. Such a reload throws
  like any other invalid material.
- Client credentials always verify the server, as `SslCredentials` did.
  `enable_ssl_verification` remains unused, as before this spec.
- A TLS client with no material at all keeps `SslCredentials` with system
  roots, and its reload is a successful no-op.
- The transports unsubscribe from a self-refreshing source in their
  destructors, not in `stop()`, so a stopped and restarted server keeps
  following its source. `stop()` does stop the auto-reload poll thread.
- The transport-neutral validators live in `tls_material_source.hpp`. The
  old `grpc_detail` PEM validators were removed because nothing used them
  any more.
- Property 1 is tested with unary RPCs issued continuously on an existing
  channel across the reload. The transport has no streaming RPC.
- The torn-handshake stress (Property 4) runs at bridge level in
  `grpc_tls_bridge_test`, against a bare `grpc::Server`.

**Local verification:** the material-source and issuing-source tests ran
under ThreadSanitizer. The bridge and both gRPC integration binaries were
built and run against the system gRPC 1.51 with Folly shimmed out. CI runs
them against the pinned 1.71.0.

## Overview

Give the gRPC transport the reload surface every other transport has
(`reload_tls_material`, `enable_auto_reload`, `disable_auto_reload`). Add a
transport-neutral TLS material source with static, file and issuing
implementations, and route gRPC's credentials through gRPC's
certificate-provider API so a running server and its existing channels pick
up new material.

## Task Dependency Graph

```
1 ─► 2 ─► 3 ─► 4 ─► 5 ─► 6
          └──► 7 ───────┘
```

## Tasks

- [x] 1. Settle the gRPC provider mechanism
  - [x] 1.1 Inspect the installed gRPC 1.71.0 headers
    (`grpcpp/security/tls_certificate_provider.h`,
    `tls_credentials_options.h`) for a public provider that can be updated
    in memory. Record the result in this file's Implementation Status
  - [x] 1.2 For Path B, confirm `FileWatcherCertificateProvider`'s
    behaviour on a mismatched key and certificate, and on a read racing a
    symlink swap, and its minimum `refresh_interval_sec`. Adjust the design
    if either differs
  - _Requirements: 1.4, 7.1_

- [x] 2. Material source interface
  - [x] 2.1 Add `include/raft/tls_material_source.hpp`: `tls_material`,
    `tls_material_source`, and `subscription` with
    wait-for-in-flight-callback semantics
  - [x] 2.2 Implement `static_tls_material_source`
  - [x] 2.3 Implement `file_tls_material_source` (whole-file reads,
    validation, mtime polling thread, failure metric)
  - [x] 2.4 Unit tests for subscribe/unsubscribe races, failed refresh
    keeping old material, and polling stop/join
  - _Requirements: 4.3, 4.4, 5.1, 5.5, 5.6, 3.2, 3.3_

- [x] 3. gRPC bridge
  - [x] 3.1 Implement `grpc_detail::grpc_tls_bridge` on the mechanism chosen
    in Task 1. On Path B, the staging directory has modes 0700 and 0600,
    with generation directories, symlink swap, pruning, and cleanup on
    destruction
  - [x] 3.2 Validate in `apply()` with the existing `grpc_detail`
    validators. Throw before anything reaches gRPC
  - _Requirements: 1.1, 1.2, 6.5, 7.1, 7.2_

- [x] 4. Transport integration
  - [x] 4.1 Add the config fields (`*_path`, `material_source`,
    `tls_refresh_interval`) and the one-source-per-item construction check
  - [x] 4.2 Build server and channel credentials through the bridge. Keep
    the insecure path untouched
  - [x] 4.3 Add `reload_tls_material()` to `grpc_server` and `grpc_client`
    (serialised by a mutex, with succeeded and failed metrics)
  - [x] 4.4 Add `enable_auto_reload()` and `disable_auto_reload()`, with a
    `logic_error` for static or self-refreshing sources. Subscribe to
    self-refreshing sources
  - [x] 4.5 Join the poll thread and unsubscribe in `stop()` and the
    destructors
  - _Requirements: 1.1-1.5, 2.1-2.5, 3.1-3.5, 4.1, 4.2, 5.2, 5.3, 7.3, 7.4_

- [x] 5. Integration tests
  - [x] 5.1 Server explicit reload with a live stream (Properties 1 and 2)
  - [x] 5.2 Invalid reload keeps serving (Property 3)
  - [x] 5.3 Root rotation
  - [x] 5.4 Client identity reload under mTLS
  - [x] 5.5 Auto-reload via `replace_atomically`
  - [x] 5.6 Shared source feeding a server and a client
  - [x] 5.7 Torn-handshake stress (Property 4)
  - _Requirements: 1-5, 7_

- [x] 6. Documentation
  - [x] 6.1 `doc/grpc_transport_README.md`: file fields, reload,
    auto-reload, material sources, latency bound, atomic-replace expectation
  - [x] 6.2 Note in certificate-authority Requirement 16 pointing here
  - _Requirements: 8.1, 8.2, 8.3_

- [x] 7. Issuing source
  - [x] 7.1 Implement `issuing_tls_material_source<P>`: in-process key
    generation (P-256), CSR from `csr_signing_options`, `sign_csr` and
    `root_certificate_pem`, initial issuance before generation 1
  - [x] 7.2 Renewal at a configurable fraction (default two thirds) with a
    fresh key each time. Capped exponential backoff, plus `renewal.failed`
    and `expired` metrics
  - [x] 7.3 Tests with `local_certificate_provider` (short validity) and a
    failing provider stub
  - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

## Notes

- The material-source interface is transport-neutral on purpose. The HTTP
  and CoAP transports can adopt it later without changing it.
