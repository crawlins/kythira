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
