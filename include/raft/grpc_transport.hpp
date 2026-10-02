// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file grpc_transport.hpp
/// @brief `grpc_transport_types` concept, an example implementation, and the
/// client/server configuration structs for the gRPC Raft transport
/// (.kiro/specs/grpc-transport/, Task 3).

#include <raft/types.hpp>
#include <raft/metrics.hpp>
#include <raft/future.hpp>
#include <concepts/future.hpp>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <chrono>
#include <cstddef>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace kythira {

class tls_material_source;  // raft/tls_material_source.hpp

// ============================================================================
// grpc_transport_types concept (Requirement 14)
// ============================================================================

/// @brief Concept for the gRPC transport's single template-parameter bundle.
///
/// Mirrors `transport_types` (`include/raft/types.hpp`) but **omits**
/// `serializer_type` (Requirement 14.2): Protocol Buffers is the transport's
/// fixed wire format, so there is no pluggable `rpc_serializer` layer — see the
/// design doc's "Protobuf replaces `rpc_serializer`" decision. A conforming
/// bundle supplies a `metrics_type` (satisfying `metrics`), an `executor_type`
/// (onto which every promise fulfillment is posted), and a `future_template`
/// template-template parameter that yields a conforming `future` for each of
/// the three core response types (Requirement 14.3).
template<typename T>
concept grpc_transport_types =
    requires {
        typename T::metrics_type;
        typename T::executor_type;
    } && kythira::metrics<typename T::metrics_type> &&
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

/// @brief Example `grpc_transport_types` using the project's default
/// `kythira::Future` backend and a Folly CPU thread pool as the executor
/// (Requirement 14.4). Analogous to `http_transport_types`.
struct grpc_kythira_transport_types {
    template<typename T> using future_template = kythira::Future<T>;
    using metrics_type = kythira::noop_metrics;
    using executor_type = folly::CPUThreadPoolExecutor;
};

// ============================================================================
// Configuration (Requirements 9, 12)
// ============================================================================

/// @brief Reads a PEM file into a string, throwing `std::runtime_error` if the
/// file cannot be opened. Convenience for callers that hold certificate
/// material on disk rather than in memory — the config structs themselves take
/// in-memory PEM strings first (matching `certificate_authority::issue()`'s
/// output), with this helper layered on top (design doc, "Certificate
/// Integration").
inline auto grpc_read_pem_file(const std::string& path) -> std::string {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("grpc_read_pem_file: cannot open " + path);
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

/// @brief gRPC client configuration (Requirements 9, 12.2, 12.3).
///
/// TLS is off by default (Requirement 9.7), but plaintext reaches only this
/// host unless `allow_plaintext` is set; certificate material is supplied as
/// in-memory PEM strings (use `grpc_read_pem_file()` for file-backed material).
struct grpc_client_config {
    std::size_t max_send_message_size{16 * 1024 * 1024};     ///< 16 MB.
    std::size_t max_receive_message_size{16 * 1024 * 1024};  ///< 16 MB.
    std::chrono::seconds keepalive_time{30};                 ///< HTTP/2 ping interval.
    std::chrono::seconds keepalive_timeout{10};              ///< Ping ack deadline.
    bool keepalive_permit_without_calls{true};

    bool enable_tls{false};              ///< Off by default (Requirement 9.7).
    bool enable_ssl_verification{true};  ///< Validate the server certificate.
    std::string ca_cert_pem{};           ///< Trusted root(s), PEM.
    std::string client_cert_pem{};       ///< Mutual TLS client certificate, PEM.
    std::string client_key_pem{};        ///< Mutual TLS client private key, PEM.
    /// File-backed alternatives to the three PEM fields above, re-read by
    /// `reload_tls_material()` and watched by `enable_auto_reload()`. Each
    /// item comes from its `*_pem` field or its `*_path` field, never both,
    /// and PEM and path fields are not mixed. Replace the files atomically
    /// (write, then rename). See .kiro/specs/grpc-tls-reload/.
    std::string ca_cert_path{};
    std::string client_cert_path{};
    std::string client_key_path{};
    /// When set, the only TLS input: no `*_pem` or `*_path` field may be set.
    /// May be shared with a `grpc_server` so a node presents one identity both
    /// ways. Must have published (generation >= 1) before construction.
    std::shared_ptr<tls_material_source> material_source{};
    /// How often gRPC re-reads applied material, which bounds how long a
    /// reload takes to reach new handshakes. gRPC's minimum is 1 second.
    std::chrono::seconds tls_refresh_interval{1};
    std::string target_name_override{};  ///< Test-only: bypass SAN hostname check.
    /// Permit plaintext channels to targets that can reach beyond this host.
    /// Without it, a client with TLS off refuses any target that is not
    /// loopback-only or a local socket (grpc_plaintext_refused_error).
    /// Ignored when enable_tls is true. See .kiro/specs/grpc-plaintext-opt-in/.
    bool allow_plaintext{false};

    std::string user_agent{"kythira-grpc-transport/1.0"};
};

/// @brief gRPC server configuration (Requirements 9, 12.4, 12.5).
struct grpc_server_config {
    std::size_t max_send_message_size{16 * 1024 * 1024};
    std::size_t max_receive_message_size{16 * 1024 * 1024};
    std::size_t max_concurrent_rpcs{200};
    /// Number of gRPC-internal completion-queue-servicing threads, independent
    /// of `Types::executor_type` (Requirement 12.5). 0 lets gRPC choose.
    std::size_t sync_server_thread_count{0};
    std::chrono::seconds keepalive_time{30};
    std::chrono::seconds keepalive_timeout{10};
    /// Grace period given to in-flight RPCs on `stop()` before a hard shutdown.
    std::chrono::milliseconds shutdown_grace_period{5000};

    bool enable_tls{false};           ///< Off by default (Requirement 9.7).
    std::string server_cert_pem{};    ///< Server certificate, PEM.
    std::string server_key_pem{};     ///< Server private key, PEM.
    std::string ca_cert_pem{};        ///< Trusted root for client certs (mTLS).
    bool require_client_cert{false};  ///< Enforce mutual TLS. Fixed for life.
    /// File-backed alternatives to the PEM fields above; same rules as the
    /// client's (see grpc_client_config::ca_cert_path).
    std::string server_cert_path{};
    std::string server_key_path{};
    std::string ca_cert_path{};
    /// When set, the only TLS input (see grpc_client_config::material_source).
    std::shared_ptr<tls_material_source> material_source{};
    /// Bound on how long a reload takes to reach new handshakes; >= 1 second.
    std::chrono::seconds tls_refresh_interval{1};
    /// Permit a plaintext listener on a bind address that is not
    /// loopback-only. Without it, a server with TLS off and such an address
    /// fails construction with grpc_plaintext_refused_error. Ignored when
    /// enable_tls is true. See .kiro/specs/grpc-plaintext-opt-in/.
    bool allow_plaintext{false};

    bool enable_health_check_service{true};  ///< Standard grpc.health.v1.Health.
    bool enable_reflection{false};           ///< Opt-in: exposes raft.proto shape to grpcurl.
};

}  // namespace kythira
