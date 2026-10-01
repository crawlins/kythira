# Implementation Plan

## Implementation Status

Not started.

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

- [ ] 1. Settle the gRPC provider mechanism
  - [ ] 1.1 Inspect the installed gRPC 1.71.0 headers
    (`grpcpp/security/tls_certificate_provider.h`,
    `tls_credentials_options.h`) for a public provider that can be updated
    in memory. Record the result in this file's Implementation Status
  - [ ] 1.2 For Path B, confirm `FileWatcherCertificateProvider`'s
    behaviour on a mismatched key and certificate, and on a read racing a
    symlink swap, and its minimum `refresh_interval_sec`. Adjust the design
    if either differs
  - _Requirements: 1.4, 7.1_

- [ ] 2. Material source interface
  - [ ] 2.1 Add `include/raft/tls_material_source.hpp`: `tls_material`,
    `tls_material_source`, and `subscription` with
    wait-for-in-flight-callback semantics
  - [ ] 2.2 Implement `static_tls_material_source`
  - [ ] 2.3 Implement `file_tls_material_source` (whole-file reads,
    validation, mtime polling thread, failure metric)
  - [ ] 2.4 Unit tests for subscribe/unsubscribe races, failed refresh
    keeping old material, and polling stop/join
  - _Requirements: 4.3, 4.4, 5.1, 5.5, 5.6, 3.2, 3.3_

- [ ] 3. gRPC bridge
  - [ ] 3.1 Implement `grpc_detail::grpc_tls_bridge` on the mechanism chosen
    in Task 1. On Path B, the staging directory has modes 0700 and 0600,
    with generation directories, symlink swap, pruning, and cleanup on
    destruction
  - [ ] 3.2 Validate in `apply()` with the existing `grpc_detail`
    validators. Throw before anything reaches gRPC
  - _Requirements: 1.1, 1.2, 6.5, 7.1, 7.2_

- [ ] 4. Transport integration
  - [ ] 4.1 Add the config fields (`*_path`, `material_source`,
    `tls_refresh_interval`) and the one-source-per-item construction check
  - [ ] 4.2 Build server and channel credentials through the bridge. Keep
    the insecure path untouched
  - [ ] 4.3 Add `reload_tls_material()` to `grpc_server` and `grpc_client`
    (serialised by a mutex, with succeeded and failed metrics)
  - [ ] 4.4 Add `enable_auto_reload()` and `disable_auto_reload()`, with a
    `logic_error` for static or self-refreshing sources. Subscribe to
    self-refreshing sources
  - [ ] 4.5 Join the poll thread and unsubscribe in `stop()` and the
    destructors
  - _Requirements: 1.1-1.5, 2.1-2.5, 3.1-3.5, 4.1, 4.2, 5.2, 5.3, 7.3, 7.4_

- [ ] 5. Integration tests
  - [ ] 5.1 Server explicit reload with a live stream (Properties 1 and 2)
  - [ ] 5.2 Invalid reload keeps serving (Property 3)
  - [ ] 5.3 Root rotation
  - [ ] 5.4 Client identity reload under mTLS
  - [ ] 5.5 Auto-reload via `replace_atomically`
  - [ ] 5.6 Shared source feeding a server and a client
  - [ ] 5.7 Torn-handshake stress (Property 4)
  - _Requirements: 1-5, 7_

- [ ] 6. Documentation
  - [ ] 6.1 `doc/grpc_transport_README.md`: file fields, reload,
    auto-reload, material sources, latency bound, atomic-replace expectation
  - [ ] 6.2 Note in certificate-authority Requirement 16 pointing here
  - _Requirements: 8.1, 8.2, 8.3_

- [ ] 7. Issuing source
  - [ ] 7.1 Implement `issuing_tls_material_source<P>`: in-process key
    generation (P-256), CSR from `csr_signing_options`, `sign_csr` and
    `root_certificate_pem`, initial issuance before generation 1
  - [ ] 7.2 Renewal at a configurable fraction (default two thirds) with a
    fresh key each time. Capped exponential backoff, plus `renewal.failed`
    and `expired` metrics
  - [ ] 7.3 Tests with `local_certificate_provider` (short validity) and a
    failing provider stub
  - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

## Notes

- The material-source interface is transport-neutral on purpose. The HTTP
  and CoAP transports can adopt it later without changing it.
