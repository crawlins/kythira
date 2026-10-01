# Requirements Document

## Introduction

The gRPC Raft transport (`.kiro/specs/grpc-transport/`) speaks plaintext by
default. `grpc_client_config::enable_tls` and `grpc_server_config::enable_tls`
both default to `false` (grpc-transport Requirement 9.7). When they are left
off, `build_channel_credentials()` returns `grpc::InsecureChannelCredentials()`
and `build_server_credentials()` returns `grpc::InsecureServerCredentials()`.
Nothing warns or refuses, whatever address the server listens on or the
client dials.

Plaintext Raft RPC is unauthenticated: anyone who can reach the port can send
`AppendEntries` or `InstallSnapshot` and rewrite the replicated log. The TCP
RPC transport closed the same hole in the "plaintext Raft RPC opt-in" work
(PR #368). There, a node with no RPC TLS material refuses to serve plaintext
off loopback unless the operator opts in explicitly.

This spec gives the gRPC transport the same rule:

- Plaintext on a loopback-only address is still allowed without asking.
  Loopback means 127.0.0.0/8, `::1`, a `unix:` socket, or a name that
  `/etc/hosts` maps only to loopback addresses.
- Plaintext anywhere else is refused unless the operator sets
  `allow_plaintext`.
- TLS stays off by default, so loopback-only deployments and the test suite
  keep working unchanged.

### Prerequisites

The loopback test reuses the bind-name rules from PR #368 and PR #369:

- `kythira::net_bind::is_loopback_bind_address()` (moved out of `tcp_rpc.hpp`
  into `include/raft/net_bind.hpp` by #369).
- `kythira::net_bind::resolve_bind_addresses()`. It accepts an IP literal,
  `*`, or a `/etc/hosts` name, and never consults DNS.

Implementation starts once both PRs have merged.

### Non-goals

- Making TLS the default (`enable_tls = true`). That would break every
  loopback test and example for no security gain on loopback.
- The HTTP transports (cpp-httplib, Beast, Proxygen) and CoAP. They have
  the same plaintext default and can follow this pattern in their own
  specs.
- Choosing an opt-in flag name for a command-line binary. No binary under
  `cmd/` uses the gRPC transport today, so the gate lives in the library
  config. Whichever binary adopts gRPC first exposes it as a flag, the way
  `ca_cluster_node --allow-plaintext-rpc` does.

## Glossary

- **Plaintext**: a channel or listener built with
  `grpc::InsecureChannelCredentials` or `grpc::InsecureServerCredentials`
  (`enable_tls == false`).
- **Loopback-only address**: an address string for which every address it
  names is a loopback address. This means a numeric IPv4 address in
  127.0.0.0/8, `::1`, or a host name that `/etc/hosts` maps only to such
  addresses (`localhost`, or `localhost` with no entry, per RFC 6761). It is
  decided by `net_bind::is_loopback_bind_address()` and never by DNS.
  `0.0.0.0`, `::` and `*` are not loopback-only.
- **Local socket target**: a gRPC target using the `unix:`, `unix-abstract:`
  or `vsock:` scheme. It is reachable only from this host, or this VM's
  hypervisor for vsock, and is treated as loopback-only for this gate.
- **gRPC target string**: what `grpc::CreateCustomChannel` takes. Accepted
  forms are `host:port`, `[v6]:port`, `dns:[//authority/]host[:port]`,
  `ipv4:addr:port[,addr:port...]`, `ipv6:[addr]:port[,...]`, and the local
  socket schemes above.
- **Opt-in**: `allow_plaintext = true` on the relevant config struct.

## Requirements

### Requirement 1: Opt-in configuration field

**User Story:** As an operator, I want an explicit switch to accept
plaintext gRPC Raft RPC off loopback, so that plaintext is a decision I made
and not something I got by default.

#### Acceptance Criteria

1. THE `grpc_server_config` struct SHALL gain `bool allow_plaintext{false}`.
2. THE `grpc_client_config` struct SHALL gain `bool allow_plaintext{false}`.
3. WHEN `enable_tls` is `true` THEN `allow_plaintext` SHALL have no effect.
   TLS is never downgraded by it, and grpc-transport Property 7 still
   holds.
4. THE default of `enable_tls` SHALL remain `false`.

### Requirement 2: Server refuses plaintext off loopback

**User Story:** As a security-conscious operator, I want a gRPC server that
would listen in plaintext on a reachable address to refuse to start, so
that a missing certificate cannot silently expose the Raft log to the
network.

#### Acceptance Criteria

1. WHEN a `grpc_server` is constructed with `enable_tls == false`,
   `allow_plaintext == false`, and a bind address that is not loopback-only
   THEN the constructor SHALL throw `grpc_plaintext_refused_error`, and no
   socket SHALL be opened.
2. THE error message SHALL name the bind address and both ways out:
   configure TLS (`enable_tls` with `server_cert_pem` and `server_key_pem`),
   or set `allow_plaintext` on a network the operator trusts.
3. WHEN the bind address is loopback-only THEN the server SHALL start in
   plaintext without the opt-in, exactly as today.
4. WHEN `allow_plaintext == true` THEN the server SHALL start in plaintext on
   any bind address. It SHALL emit the metric `grpc.server.plaintext.enabled`
   once at `start()`, with dimensions `bind_address` and
   `loopback_only` (`"true"` or `"false"`).
5. THE loopback decision SHALL use `net_bind::is_loopback_bind_address()`
   and SHALL NOT perform a DNS lookup. A bind name that is not an IP literal
   and is not in `/etc/hosts` SHALL count as not loopback-only.
6. THE check SHALL run in the constructor, before `build_server_credentials()`
   returns, so the failure surfaces where grpc-transport Requirement 9.6
   already surfaces TLS misconfiguration.

### Requirement 3: Client refuses plaintext to non-loopback targets

**User Story:** As an operator, I want a gRPC client with no TLS to refuse
to send Raft RPCs to a remote peer in plaintext, so that a misconfigured
node does not leak or accept forged Raft traffic over the network.

#### Acceptance Criteria

1. WHEN a `grpc_client` is constructed with `enable_tls == false` and
   `allow_plaintext == false` THEN the constructor SHALL check every target
   in `node_id_to_target_map`. It SHALL throw `grpc_plaintext_refused_error`
   naming the first target that is neither loopback-only nor a local socket
   target.
2. WHEN the address-keyed `get_or_create_channel(const std::string&)` is
   asked for a plaintext channel to a target that fails the same check, as on
   the bootstrap path (grpc-transport Requirement 15.3), THEN no channel
   SHALL be created. The RPC SHALL fail with `grpc_plaintext_refused_error`,
   surfaced the same way an unknown node ID's `NOT_FOUND`
   `grpc_client_error` is surfaced today.
3. THE target check SHALL parse every gRPC target form in the Glossary:
   - For `dns:` targets, the host part SHALL be checked as a name.
   - For `ipv4:` and `ipv6:` lists, every listed address SHALL be loopback.
   - An unrecognised scheme SHALL count as not loopback-only (fail closed).
4. THE target check SHALL NOT perform DNS resolution. A `dns:` target or bare
   host name SHALL pass only when `/etc/hosts` maps it exclusively to
   loopback addresses.
5. WHEN `allow_plaintext == true` THEN the client SHALL build plaintext
   channels to any target. It SHALL emit `grpc.client.plaintext.enabled`
   once at construction.

### Requirement 4: Error type

**User Story:** As a developer embedding the transport, I want a distinct
exception for the plaintext refusal, so that callers can tell it apart from
malformed certificates.

#### Acceptance Criteria

1. THE system SHALL add `grpc_plaintext_refused_error` to
   `include/raft/grpc_exceptions.hpp`. It SHALL derive from
   `grpc_tls_configuration_error`, so existing `catch` sites still handle it,
   and SHALL carry `grpc::StatusCode::FAILED_PRECONDITION`.
2. THE error SHALL expose the offending address through
   `const std::string& address() const`.

### Requirement 5: Compatibility and documentation

**User Story:** As a maintainer, I want the change to break only setups that
were actually exposed, and to say so plainly, so that upgrading is
predictable.

#### Acceptance Criteria

1. Existing tests, examples and benchmarks that run plaintext on loopback
   addresses SHALL pass unchanged. Any that bind `0.0.0.0`, `::` or `*`
   SHALL set `allow_plaintext = true` explicitly, with a comment.
2. `doc/grpc_transport_README.md` SHALL document the rule, the opt-in field,
   the error, and the loopback definition. It SHALL state that the decision
   never uses DNS.
3. THE grpc-transport spec's Requirement 9.7 SHALL gain a note pointing to
   this spec.
