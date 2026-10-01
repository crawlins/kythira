# Requirements Document

## Introduction

The gRPC Raft transport fixes its TLS material at construction:

- `grpc_client` calls `build_channel_credentials()` once in its constructor
  and reuses the result for every cached channel.
- `grpc_server` calls `build_server_credentials()` once in its constructor.
- Both take in-memory PEM strings only (`grpc_transport.hpp`).

Rotating a certificate therefore means restarting the node.

Every other transport can already rotate in place:

- **Requirements:**
  - certificate-authority Requirement 16 (cpp-httplib and CoAP).
  - boost-beast-http-transport Requirement 7.
  - proxygen-http-transport Requirement 7.
- **Shared surface:**
  - `reload_tls_material()` validates first, then applies. Bad material
    throws and the old material keeps serving.
  - `enable_auto_reload(poll_interval)` polls the certificate file's mtime
    on a `std::jthread`.
  - `disable_auto_reload()` stops and joins that thread.
  - A failed automatic reload emits a `*.tls_reload.failed` metric and is
    retried at the next poll.
- **Behaviour:**
  - Established connections survive a reload (certificate-authority design
    Property 11).
  - New handshakes present the new certificate (Property 12).
  - A reload is all or nothing (Property 13).

This spec brings the gRPC transport to the same surface, with two additions:

1. **A certificate-provider hook.** The project's `certificate_provider`
   concept (`include/raft/certificate_provider.hpp`, certificate-authority
   Requirement 9) is an *issuance* interface: a CSR goes in, a signed
   certificate comes out. No transport consumes it. Every reload path today
   reads files, so something outside the process has to write and renew
   them. This spec adds a transport-facing *TLS material source* that the
   gRPC transport pulls its key, chain and roots from. It also adds an
   adapter that drives any `certificate_provider` (local CA, ACME, AWS ACM
   PCA, Azure, GCP, OCI) to issue and renew the node's own certificate
   in-process.
2. **Root (trust bundle) rotation.** The CA bundle used to verify peers can
   change too, so a CA can roll over without restarting the cluster.

### Non-goals

- Retrofitting the material-source hook onto the HTTP and CoAP transports.
  The interface is defined transport-neutrally so they can adopt it later.
- Changing any default. A transport built from PEM strings with no source
  behaves exactly as it does today.
- Plaintext gating. That is `.kiro/specs/grpc-plaintext-opt-in/`.

## Glossary

- **TLS material**: one consistent set of identity certificate chain (PEM),
  private key (PEM), and trusted root bundle (PEM, optional on the client
  when system roots are used).
- **Material source**: an object that holds the current TLS material, can
  report a new version, and is the single place the transport reads material
  from.
- **Generation**: a counter a material source increments each time it
  publishes new material. It is used for metrics and tests.
- **gRPC certificate provider**: gRPC's own
  `grpc::experimental::CertificateProviderInterface`, consumed by
  `TlsServerCredentialsOptions` and `TlsChannelCredentialsOptions`. It is how
  gRPC changes the certificates of a *running* server and of existing
  channels' future handshakes without rebuilding them.
- **Issuance provider**: a type modelling the existing `certificate_provider`
  concept.

## Requirements

### Requirement 1: Explicit reload on the server

**User Story:** As an operator, I want to replace a running gRPC server's
certificate and key without restarting the node, so that rotation causes no
leader election and no downtime.

#### Acceptance Criteria

1. THE `grpc_server` SHALL provide `reload_tls_material()`. It re-reads the
   server's configured material (see Requirement 4) and validates the
   certificate, the key, the key-to-certificate match and, when
   `require_client_cert` is set, the root bundle, before applying any of
   it.
2. WHEN validation fails THEN `reload_tls_material()` SHALL throw
   `grpc_tls_configuration_error` and the server SHALL keep serving the
   previous material.
3. WHEN validation succeeds THEN every TLS handshake that starts after the
   new material takes effect SHALL present the new certificate and verify
   clients against the new roots. Connections and RPC streams already
   established SHALL NOT be closed.
4. THE time from a successful `reload_tls_material()` returning to the new
   material taking effect SHALL be bounded by
   `grpc_server_config::tls_refresh_interval` (default 1 second; see
   design). When the implementation can apply material synchronously, the
   bound SHALL be zero.
5. WHEN `reload_tls_material()` is called on a server without TLS
   (`enable_tls == false`) THEN it SHALL throw `std::logic_error`, as the
   cpp-httplib server does.

### Requirement 2: Explicit reload on the client

**User Story:** As an operator using mutual TLS, I want a running gRPC
client to start presenting a renewed client certificate and trusting a
rotated CA bundle, so that peers keep accepting it after rotation.

#### Acceptance Criteria

1. THE `grpc_client` SHALL provide `reload_tls_material()`, validating
   before applying as in Requirement 1.1. It covers the client certificate
   and key (when mTLS is configured) and the root bundle.
2. WHEN validation fails THEN it SHALL throw `grpc_tls_configuration_error`
   and keep the previous material.
3. WHEN it succeeds THEN new handshakes on every channel, cached or new,
   SHALL use the new material. RPCs in flight SHALL complete on their
   existing connections.
4. THE cached channels (`_channels`, `_address_channels`) SHALL NOT need to
   be discarded for the new material to apply. When the chosen mechanism
   does require new channels, the old ones SHALL be retired rather than
   destroyed while RPCs may still reference them, mirroring the cpp-httplib
   client's `_retired_clients`.
5. WHEN called on a client without TLS THEN it SHALL throw
   `std::logic_error`.

### Requirement 3: Automatic reload

**User Story:** As an operator whose certificates are renewed by an external
agent (certbot, cert-manager, a cloud agent), I want the transport to notice
new files on its own, so that I don't have to signal the process.

#### Acceptance Criteria

1. THE `grpc_server` and `grpc_client` SHALL provide
   `enable_auto_reload(std::chrono::seconds poll_interval)` and
   `disable_auto_reload()`. Their semantics SHALL match the other transports:
   - Poll the certificate file's `last_write_time`.
   - Call `reload_tls_material()` when it differs from the last successfully
     loaded value.
   - Record the new value only on success.
2. WHEN an automatic reload fails THEN the transport SHALL emit
   `grpc.server.tls_reload.failed` or `grpc.client.tls_reload.failed` and
   retry at the next poll. The poll thread SHALL NOT terminate.
3. `disable_auto_reload()` SHALL stop and join the poll thread, and SHALL be
   called by the destructor and by `stop()`.
4. WHEN the transport's material comes from a material source with its own
   change notification (Requirement 5) THEN `enable_auto_reload()` SHALL
   throw `std::logic_error`. The source owns change detection.
5. THE transport SHALL emit `grpc.server.tls_reload.succeeded` or
   `grpc.client.tls_reload.succeeded` with a `generation` dimension on every
   successful reload, explicit or automatic.

### Requirement 4: File-backed configuration

**User Story:** As an operator, I want to point the gRPC transport at
certificate files instead of pasting PEM into config, so that reload has
something to re-read.

#### Acceptance Criteria

1. `grpc_server_config` SHALL gain `server_cert_path`, `server_key_path` and
   `ca_cert_path`. `grpc_client_config` SHALL gain `client_cert_path`,
   `client_key_path` and `ca_cert_path`. All are `std::string` and empty by
   default.
2. WHEN both a `*_pem` field and its `*_path` field are set THEN
   construction SHALL throw `grpc_tls_configuration_error`. Exactly one
   source per item is allowed.
3. WHEN only `*_pem` fields are set THEN the material is static, and
   `reload_tls_material()` SHALL re-apply the same strings. It is a no-op
   that still succeeds, and `enable_auto_reload()` SHALL throw
   `std::logic_error` because there is no file to watch.
4. Files SHALL be read whole on each load. Writers are expected to replace
   them atomically (write a temporary file, then `rename`), as
   `certificate_authority::replace_atomically()` does. A read that yields a
   mismatched certificate and key SHALL fail validation, keeping the old
   material, rather than apply half an update.

### Requirement 5: TLS material source hook

**User Story:** As a developer embedding the transport, I want to hand it an
object that supplies TLS material and announces changes, so that I can
source certificates from a secrets manager, an HSM-backed agent or my own
renewal logic without writing files.

#### Acceptance Criteria

1. THE system SHALL define a transport-neutral interface
   `kythira::tls_material_source` in `include/raft/tls_material_source.hpp`
   with:
   - `current()`, returning the current TLS material as an immutable
     snapshot.
   - `generation()`.
   - `subscribe(callback)`, returning a handle whose destruction
     unsubscribes. The callback is invoked after each publish.
2. `grpc_server_config` and `grpc_client_config` SHALL gain
   `std::shared_ptr<tls_material_source> material_source`. When set, it SHALL
   be the only material input: setting it together with any `*_pem` or
   `*_path` identity field SHALL throw `grpc_tls_configuration_error`.
3. WHEN a source publishes new material THEN the transport SHALL validate it
   (Requirement 1.1) and apply it within the bound of Requirement 1.4. When
   validation fails, the transport SHALL keep the previous material and emit
   the `tls_reload.failed` metric.
4. A source SHALL be shareable: one source MAY feed a `grpc_server` and a
   `grpc_client` in the same node, so the node presents one identity in both
   directions.
5. THE system SHALL provide three source implementations:
   - `static_tls_material_source`, built from PEM strings.
   - `file_tls_material_source`, which polls the paths' mtimes on its own
     thread with the same semantics as Requirement 3.
   - `issuing_tls_material_source<P>`, described in Requirement 6.
6. THE transport SHALL hold the source by `shared_ptr` and SHALL
   unsubscribe before destruction, so a source outliving the transport never
   calls into a destroyed object.

### Requirement 6: Issuance-provider adapter

**User Story:** As an operator, I want a node to obtain and renew its own
gRPC certificate from the project's existing certificate providers, so that
rotation needs no external agent.

#### Acceptance Criteria

1. `issuing_tls_material_source<P>`, where `P` models `certificate_provider`,
   SHALL:
   - Generate a private key in-process (ECDSA P-256 by default).
   - Build a CSR from configured `csr_signing_options` (subject, SANs,
     validity).
   - Call `P::sign_csr`, fetch `P::root_certificate_pem` for the trust
     bundle, and publish the result.
2. IT SHALL schedule renewal when a configured fraction of the certificate's
   validity has elapsed (default two thirds, matching ca-cluster-rpc-mtls
   Requirement 7). Each renewal SHALL generate a fresh key.
3. WHEN issuance or renewal fails THEN it SHALL keep publishing the current
   material. It SHALL retry with capped exponential backoff and emit
   `tls_material_source.renewal.failed`. When the current certificate
   expires without a successful renewal, it SHALL emit
   `tls_material_source.expired` and keep retrying. It SHALL NOT publish
   empty material.
4. THE initial issuance SHALL complete before the source reports a
   generation of at least 1. A transport constructed from a source at
   generation 0 SHALL throw `grpc_tls_configuration_error` rather than
   start without material.
5. THE private key SHALL never be written to disk by the adapter. When the
   chosen gRPC mechanism needs files (see design), the key SHALL be written
   only to a directory created with mode 0700, as a file with mode 0600, and
   removed on destruction.

### Requirement 7: Atomicity and safety

**User Story:** As a security-conscious operator, I want rotation to be
impossible to observe half-done and impossible to turn into a downgrade, so
that a bad renewal can't expose or break the cluster.

#### Acceptance Criteria

1. A handshake SHALL observe either the complete previous material or the
   complete new material, never a mix. For example, it must never see a new
   certificate with an old key, or new roots with an old identity.
2. NO reload path SHALL fall back to insecure credentials. grpc-transport
   Property 7 SHALL hold across reloads.
3. A reload SHALL NOT change whether client certificates are required.
   `require_client_cert` stays fixed for the server's lifetime.
4. Concurrent calls to `reload_tls_material()`, from an operator and from
   the poll thread or a source callback, SHALL serialise. The last one to
   validate successfully wins.

### Requirement 8: Documentation and compatibility

**User Story:** As a maintainer, I want the change to be additive and
documented, so that existing users see no difference unless they opt in.

#### Acceptance Criteria

1. Existing code constructing the transport from PEM strings SHALL compile
   and behave unchanged.
2. `doc/grpc_transport_README.md` SHALL document:
   - The file-backed fields.
   - `reload_tls_material()` and auto-reload.
   - The material-source hook and its three implementations.
   - The reload latency bound.
   - The atomic-replace expectation for files.
3. THE certificate-authority spec's Requirement 16 SHALL gain a note that
   the gRPC transport implements the same surface via this spec.
