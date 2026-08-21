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
#include <sstream>
#include <string>

namespace kythira {

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
/// TLS is off by default (Requirement 9.7); certificate material is supplied as
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
    std::string target_name_override{};  ///< Test-only: bypass SAN hostname check.

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
    bool require_client_cert{false};  ///< Enforce mutual TLS.

    bool enable_health_check_service{true};  ///< Standard grpc.health.v1.Health.
    bool enable_reflection{false};           ///< Opt-in: exposes raft.proto shape to grpcurl.
};

}  // namespace kythira
