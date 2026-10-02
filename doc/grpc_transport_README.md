# gRPC Transport — Overview & Troubleshooting

The gRPC transport (`include/raft/grpc_transport.hpp`,
`include/raft/grpc_transport_impl.hpp`) implements the full
`network_client`/`network_server` concept family over gRPC and Protocol
Buffers. See [`.kiro/specs/grpc-transport/`](../.kiro/specs/grpc-transport/) for
the complete requirements/design/tasks, and the README's "gRPC Transport for
Data-Center Deployments" section for a usage walk-through.

This document collects the operational and build-time issues most likely to
trip you up.

## Build

The transport is an **optional dependency** that degrades gracefully, exactly
like the CoAP/libcoap transport:

- vcpkg installs `grpc` (declared in [`vcpkg.json`](../vcpkg.json), pinned to
  ≥ 1.71.0 for the stable callback API). Protobuf comes in transitively.
- CMake probes it only when the `GRPC_TRANSPORT` Kconfig symbol is wanted
  (`config GRPC_TRANSPORT`, default `y`), via
  `find_package(gRPC CONFIG)` / `find_package(Protobuf CONFIG)`.
- When gRPC/Protobuf are **not** found, the `raft_grpc_transport` target is
  skipped with a warning and the rest of the project configures unaffected.
- Under `-DKYTHIRA_KCONFIG_STRICT=ON`, selecting `CONFIG_GRPC_TRANSPORT=y`
  without gRPC/Protobuf present is a hard configure error (same as
  `COAP_TRANSPORT`).

The `.proto` file is checked in; the generated `raft.pb.{h,cc}` /
`raft.grpc.pb.{h,cc}` are **not** — they are produced at build time by
`protoc` + `grpc_cpp_plugin` into `${CMAKE_BINARY_DIR}/generated/raft/`.

### Common build issues

- **`gRPC::grpc_cpp_plugin` target not found.** The `Protobuf` CMake config was
  found but gRPC's was not, or vice versa. Both `find_package(gRPC CONFIG)` and
  `find_package(Protobuf CONFIG)` must succeed. With vcpkg, ensure the `grpc`
  port actually installed (check `vcpkg_installed/<triplet>/`); a partial
  install leaves `protoc` present but `grpc_cpp_plugin` absent.
- **`protoc` version skew.** Use the `protobuf::protoc` and
  `gRPC::grpc_cpp_plugin` from the *same* gRPC/Protobuf installation the target
  links against. Mixing a system `protoc` with vcpkg's gRPC runtime produces
  generated code that fails to compile or link. The CMake `add_custom_command`
  already references the imported targets rather than a bare `protoc`, so don't
  override it with a `PATH`-resolved binary.
- **Generated headers not found (`raft.pb.h: No such file`).** Anything that
  includes `grpc_message_conversion.hpp` / `grpc_transport_impl.hpp` must link
  the `raft_grpc_transport` target, which exports the generated-code include
  directory `PUBLIC`. Don't include the headers into a target that doesn't link
  it.
- **`raft_grpc_transport` target missing entirely.** It also requires a future
  backend (Folly or stdexec) to be available, since the transport fulfills
  `kythira::Promise`. With `CONFIG_FOLLY=n` and no alternate backend, the target
  is skipped even if gRPC is present.

## TLS / mTLS

- TLS is **off by default** (`enable_tls = false`) so the transport is usable
  for local development and the network simulator's trusted environments
  without certificates.
- **Plaintext stays on this host unless you opt in.** Plaintext Raft RPC is
  unauthenticated: anyone who can reach the port can send `AppendEntries` and
  rewrite the log. So with TLS off:
  - A `grpc_server` whose bind address is not loopback-only (`0.0.0.0`, `::`,
    `*`, a routable IP, or a name mapped to one) fails construction with
    `grpc_plaintext_refused_error`, before any socket is opened.
  - A `grpc_client` refuses, at construction, any configured target that is
    neither loopback-only nor a local socket (`unix:`, `unix-abstract:`,
    `vsock:`). The address-keyed bootstrap calls (`send_cluster_join_request`,
    `send_cluster_leave_request`) check their address per call and throw the
    same error without caching a channel.
  - Set `allow_plaintext = true` on `grpc_server_config` or
    `grpc_client_config` to accept plaintext anyway, on a network you trust.
    The server then emits `grpc.server.plaintext.enabled` at `start()` (with
    `bind_address` and `loopback_only` dimensions) and the client emits
    `grpc.client.plaintext.enabled` at construction (with `loopback_only`).
    `allow_plaintext` has no effect when `enable_tls` is set.

  *Loopback-only* means 127.0.0.0/8, `::1`, or a name that `/etc/hosts` maps
  only to such addresses (`localhost` counts even when the file omits it, per
  RFC 6761). A client target may also be the IPv4-mapped `::ffff:127.x.y.z`.
  **The decision never uses DNS**: a name the hosts file does not list counts
  as not loopback-only, whatever a resolver would say. Client targets are read
  in every gRPC form (`host:port`, `[v6]:port`, `dns:[//authority/]host:port`,
  `ipv4:`/`ipv6:` lists, where every address must be loopback); any other
  scheme, such as `xds:`, counts as not loopback-only.

  `grpc_plaintext_refused_error` derives from `grpc_tls_configuration_error`,
  so existing handlers still catch it. It carries `FAILED_PRECONDITION` and
  names the refused address in `address()`.
- Certificate/key material is accepted as **in-memory PEM strings**
  (`ca_cert_pem`, `server_cert_pem`/`server_key_pem`,
  `client_cert_pem`/`client_key_pem`). Use `kythira::grpc_read_pem_file(path)`
  to load file-backed material. This matches `certificate_authority::issue()`'s
  in-memory output — no filesystem round-trip required.
- **TLS is never silently downgraded.** Invalid PEM, a mismatched cert/key
  pair, a half-configured mTLS pair (cert without key), or `require_client_cert`
  with an empty `ca_cert_pem` all throw `grpc_tls_configuration_error` at
  construction rather than falling back to an insecure channel/listener.
- **`target_name_override` is test-only.** gRPC verifies the server
  certificate's SAN against the target host. When connecting to `127.0.0.1`
  against a certificate whose SAN is `localhost`, set
  `client_cfg.target_name_override = "localhost"`. **Never set this outside test
  builds** — it bypasses hostname verification.
- **`UNAVAILABLE` on every call with TLS on.** Usually a trust-chain problem:
  the client's `ca_cert_pem` does not chain to the server's certificate, or
  (with `require_client_cert`) the server's `ca_cert_pem` does not chain to the
  client's certificate. Verify both sides were issued by the same CA.

## Certificate reload

TLS material can change while a node runs, without dropping connections or
forcing a leader election (`.kiro/specs/grpc-tls-reload/`).

- **Where material comes from.** Each side takes exactly one of:
  - the `*_pem` fields (static: a reload re-applies the same strings and
    succeeds; there is nothing to watch);
  - the `*_path` fields (`server_cert_path`, `server_key_path`,
    `ca_cert_path`; `client_cert_path`, `client_key_path`, `ca_cert_path`),
    read whole on every load;
  - `material_source`, a `std::shared_ptr<kythira::tls_material_source>`.

  Setting an item's `*_pem` and `*_path` together, mixing PEM and path
  fields, or combining `material_source` with either, throws
  `grpc_tls_configuration_error` at construction. So does a source that has
  not published yet (generation 0).
- **`reload_tls_material()`** on `grpc_server` and `grpc_client` re-reads the
  material, validates it (parseable certificate and key, key matches the
  certificate, roots present and parseable where they are used), and applies
  it. Invalid material throws `grpc_tls_configuration_error` and the old
  material keeps being served. Without TLS it throws `std::logic_error`.
  Emits `grpc.{server,client}.tls_reload.succeeded` or `.failed`, with a
  `generation` dimension.
- **What a reload changes.** Every handshake that starts afterwards uses the
  new material: new connections to the server, and new connections made by
  every client channel, cached or not. Established connections and RPCs are
  never closed. A reload cannot add or remove an identity or trusted roots,
  and `require_client_cert` is fixed for the server's lifetime.
- **Latency bound.** New material reaches handshakes within
  `tls_refresh_interval` (default and minimum 1 second). gRPC 1.71 has no
  certificate provider that can be updated in memory, so the transport stages
  each validated generation in a private directory (mode 0700, files 0600,
  under `$XDG_RUNTIME_DIR` or the temporary directory, removed on
  destruction) that gRPC's file-watching provider re-reads on that interval.
  The key and certificate a handshake uses always match. For up to one
  interval after a reload that changes both identity and roots, the roots may
  still be the previous generation's.
- **Auto-reload.** `enable_auto_reload(poll_interval)` polls the `*_path`
  files' modification times and reloads when one changes. A failed reload
  emits `.failed` and is retried at the next poll. `disable_auto_reload()`
  joins the thread, and `stop()` and the destructors call it. It throws
  `std::logic_error` for PEM fields (no files) and for a material source
  (which detects its own changes). **Replace files atomically** (write a
  temporary file, then `rename()`), as
  `certificate_authority::replace_atomically()` does. A read that catches a
  certificate and key from different writes fails validation and is retried
  rather than applied.
- **Material sources** (`include/raft/tls_material_source.hpp`) are
  transport-neutral:
  - `static_tls_material_source`: fixed PEM strings.
  - `file_tls_material_source`: PEM files, optionally polling them on its own
    thread.
  - `issuing_tls_material_source<P>`
    (`include/raft/issuing_tls_material_source.hpp`): obtains its certificate
    from any `certificate_provider` (local CA, ACME, AWS ACM PCA, GCP Private
    CA, OCI). Each issuance generates a fresh ECDSA P-256 key in memory and
    renews at two thirds of the validity by default. On failure it keeps the
    current material, retries with capped exponential backoff, and emits
    `tls_material_source.renewal.failed`, plus `tls_material_source.expired`
    if the certificate lapses.

  A source can be shared, so one node presents the same identity as a server
  and as a client. A transport subscribes to a self-refreshing source and
  unsubscribes before it is destroyed.

## Deadlines, timeouts, and message sizes

- Every `send_*` call takes a `timeout`; the transport sets the gRPC
  `ClientContext` deadline to `now() + timeout`. On expiry the returned future
  resolves to `grpc_timeout_error`, whose `configured_timeout()` reports the
  deadline that was set. Raft's own retry policies own retry decisions — the
  transport never retries internally.
- **Status → exception mapping** (inspect `status_code()` on any
  `grpc_transport_error`): `DEADLINE_EXCEEDED` → `grpc_timeout_error`;
  `UNAVAILABLE` → `grpc_connection_error`;
  `INVALID_ARGUMENT`/`UNIMPLEMENTED`/`NOT_FOUND`/`FAILED_PRECONDITION` →
  `grpc_client_error`; `INTERNAL`/`UNKNOWN`/`DATA_LOSS` → `grpc_server_error`;
  anything else → `grpc_transport_error`.
- **Snapshot chunk size vs. max message size.** The base transport keeps the
  existing offset-based `InstallSnapshot` chunking contract (one unary RPC per
  chunk) rather than gRPC client-streaming. A single chunk's serialized
  `InstallSnapshotRequest` must fit under the configured
  `max_receive_message_size` (default 16 MB). Choose
  `raft_configuration::_snapshot_chunk_size` small enough to stay under that
  limit; the transport does **not** fragment one chunk across multiple RPCs. A
  chunk that exceeds the limit surfaces to the caller as `RESOURCE_EXHAUSTED`.
- **`RESOURCE_EXHAUSTED` on AppendEntries.** Too many/too-large entries in one
  RPC. Size `max_receive_message_size` consistently with
  `raft_configuration::_max_entries_per_append`.

## Behavioral notes

- **Unregistered optional services return `UNIMPLEMENTED`.** Only services whose
  handlers were registered before `start()` are added to the `ServerBuilder`, so
  a client calling (say) `RequestPreVote` against a server that never registered
  a pre-vote handler gets a clean `UNIMPLEMENTED` (surfaced as
  `grpc_client_error`) rather than a hang. Register the handler *before*
  `start()`.
- **Channel reuse.** A single `grpc::Channel` (and its HTTP/2 connection(s)) is
  created and reused per target node across repeated calls; the bootstrap
  ClusterJoin/ClusterLeave path keys its channel by contact-address string
  instead, since the joining node is not yet a known node ID.
- **Executor discipline.** The `executor_type` is caller-owned and passed by
  reference (mirroring the Beast transport's caller-owned `io_context`). It must
  outlive the client/server. Every promise fulfillment and every registered
  handler runs on it, never on a gRPC internal thread.
