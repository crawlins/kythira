// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file grpc_exceptions.hpp
/// @brief Exception hierarchy for the gRPC Raft transport
/// (.kiro/specs/grpc-transport/, Requirement 11 + 9.6).
///
/// Every RPC that completes with a non-`OK` `grpc::Status` sets its returned
/// future to an error state carrying one of these exception types, chosen by
/// the status-code mapping table in the design doc's Error Handling section.
/// TLS/mTLS misconfiguration fails construction with
/// `grpc_tls_configuration_error` rather than silently downgrading to an
/// insecure channel/listener (Requirement 9.6, Property 7).

#include <grpcpp/support/status.h>

#include <chrono>
#include <stdexcept>
#include <string>

namespace kythira {

/// @brief Base of the gRPC transport exception hierarchy.
///
/// Carries the `grpc::StatusCode` that produced it so callers can inspect the
/// underlying failure mode without string-matching `what()`.
class grpc_transport_error : public std::runtime_error {
public:
    grpc_transport_error(grpc::StatusCode code, const std::string& message)
        : std::runtime_error(message), _status_code(code) {}

    /// @brief The `grpc::StatusCode` that this error was mapped from.
    [[nodiscard]] auto status_code() const -> grpc::StatusCode { return _status_code; }

private:
    grpc::StatusCode _status_code;
};

/// @brief Peer unreachable — mapped from `grpc::StatusCode::UNAVAILABLE`
/// (Requirement 11.1).
class grpc_connection_error : public grpc_transport_error {
public:
    explicit grpc_connection_error(const std::string& message)
        : grpc_transport_error(grpc::StatusCode::UNAVAILABLE, message) {}
};

/// @brief RPC exceeded its deadline — mapped from
/// `grpc::StatusCode::DEADLINE_EXCEEDED` (Requirements 10.2, 10.3, 11.2). Carries
/// the configured timeout so diagnostics can report what deadline was set,
/// not merely that one was exceeded.
class grpc_timeout_error : public grpc_transport_error {
public:
    grpc_timeout_error(const std::string& message, std::chrono::milliseconds configured_timeout)
        : grpc_transport_error(grpc::StatusCode::DEADLINE_EXCEEDED, message),
          _configured_timeout(configured_timeout) {}

    /// @brief The `timeout` value passed to the `send_*` call that failed.
    [[nodiscard]] auto configured_timeout() const -> std::chrono::milliseconds {
        return _configured_timeout;
    }

private:
    std::chrono::milliseconds _configured_timeout;
};

/// @brief Caller-side fault — mapped from `INVALID_ARGUMENT`, `UNIMPLEMENTED`,
/// `NOT_FOUND`, `FAILED_PRECONDITION` (Requirement 11.3). The originating
/// status code is preserved via `status_code()`.
class grpc_client_error : public grpc_transport_error {
public:
    grpc_client_error(grpc::StatusCode code, const std::string& message)
        : grpc_transport_error(code, message) {}
};

/// @brief Server-side fault — mapped from `INTERNAL`, `UNKNOWN`, `DATA_LOSS`
/// (Requirement 11.4). The originating status code is preserved via
/// `status_code()`.
class grpc_server_error : public grpc_transport_error {
public:
    grpc_server_error(grpc::StatusCode code, const std::string& message)
        : grpc_transport_error(code, message) {}
};

/// @brief TLS/mTLS configuration is invalid (missing/unreadable certificate or
/// key material, mismatched cert/key pair, TLS enabled with no material)
/// (Requirements 9.6). Thrown from client/server construction so TLS is never
/// silently downgraded to insecure (Property 7).
class grpc_tls_configuration_error : public grpc_transport_error {
public:
    explicit grpc_tls_configuration_error(const std::string& message)
        : grpc_transport_error(grpc::StatusCode::INVALID_ARGUMENT, message) {}
};

}  // namespace kythira
