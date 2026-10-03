// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/exceptions.hpp>

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kythira {

// Base exception class for CoAP transport errors
class coap_transport_error : public std::runtime_error {
public:
    explicit coap_transport_error(const std::string& message) : std::runtime_error(message) {}
};

// Exception for CoAP client errors (4.xx response codes)
class coap_client_error : public coap_transport_error {
public:
    coap_client_error(std::uint8_t response_code, const std::string& message)
        : coap_transport_error(message), _response_code(response_code) {}

    [[nodiscard]] auto response_code() const -> std::uint8_t { return _response_code; }

private:
    std::uint8_t _response_code;
};

// Exception for CoAP server errors (5.xx response codes)
class coap_server_error : public coap_transport_error {
public:
    coap_server_error(std::uint8_t response_code, const std::string& message)
        : coap_transport_error(message), _response_code(response_code) {}

    [[nodiscard]] auto response_code() const -> std::uint8_t { return _response_code; }

private:
    std::uint8_t _response_code;
};

// Exception for CoAP timeout errors
class coap_timeout_error : public coap_transport_error {
public:
    explicit coap_timeout_error(const std::string& message) : coap_transport_error(message) {}
};

// Exception for CoAP security/DTLS errors
class coap_security_error : public coap_transport_error {
public:
    explicit coap_security_error(const std::string& message) : coap_transport_error(message) {}
};

// Exception for CoAP protocol errors
class coap_protocol_error : public coap_transport_error {
public:
    explicit coap_protocol_error(const std::string& message) : coap_transport_error(message) {}
};

// Exception for CoAP network errors
class coap_network_error : public coap_transport_error {
public:
    explicit coap_network_error(const std::string& message) : coap_transport_error(message) {}
};

/// @brief A peer asked for, or sent, a media type no registered serializer
///        handles — the CoAP analogue of `unsupported_media_type_error`.
///
/// Separate from the HTTP type rather than shared, because the two hierarchies
/// are separate all the way up (`coap_transport_error` vs
/// `http_transport_error`) and a CoAP handler catching
/// `const coap_transport_error&` must not miss this one. It carries the media
/// type rather than a `coap_content_format` for the same reason the registry is
/// keyed that way: the media type is what the registry dispatches on, and the
/// Content-Format number is a CoAP-side encoding of it that not every media
/// type has (see `media_type_to_coap_content_format`).
///
/// Distinct from `coap_protocol_error`, which means a malformed or
/// unintelligible PDU. This one means a well-formed request naming an encoding
/// this node does not speak, and the two get different responses: 4.15
/// Unsupported Content-Format / 4.06 Not Acceptable for this, 4.00 Bad Request
/// for that.
class coap_unsupported_content_format_error : public coap_transport_error {
public:
    explicit coap_unsupported_content_format_error(const std::string& media_type)
        : coap_transport_error("unsupported content format: " + media_type),
          _media_type(media_type) {}

    [[nodiscard]] auto media_type() const -> const std::string& { return _media_type; }

private:
    std::string _media_type;
};

namespace coap_detail {

/// The optional extension RPC served at `resource_path`, or empty for a
/// mandatory one (.kiro/specs/http-coap-pre-vote-timeout-now/ Requirement 3).
[[nodiscard]] inline auto extension_rpc_name(std::string_view resource_path) -> std::string_view {
    if (resource_path == "/raft/request_pre_vote") {
        return "request_pre_vote";
    }
    if (resource_path == "/raft/timeout_now") {
        return "timeout_now";
    }
    return {};
}

/// Turns a 4.04 (no such resource: an older build) or 5.01 (resource but no
/// handler) on an extension RPC into `rpc_not_implemented_exception`, which
/// the core treats as "this peer cannot serve the RPC" rather than as a
/// failure. Every other error, and every error on a mandatory RPC, passes
/// through unchanged: a peer missing RequestVote is misconfigured, not older.
///
/// Codes are compared in the on-wire class/detail byte every backend's
/// `coap_client_error` and `coap_server_error` carry.
[[nodiscard]] inline auto map_extension_not_implemented(std::exception_ptr error,
                                                        std::string_view resource_path,
                                                        std::uint64_t target)
    -> std::exception_ptr {
    constexpr std::uint8_t not_found = (4U << 5U) | 4U;        // 4.04
    constexpr std::uint8_t not_implemented = (5U << 5U) | 1U;  // 5.01
    const auto rpc = extension_rpc_name(resource_path);
    if (rpc.empty() || !error) {
        return error;
    }
    try {
        std::rethrow_exception(error);
    } catch (const coap_client_error& e) {
        if (e.response_code() == not_found) {
            return std::make_exception_ptr(
                kythira::rpc_not_implemented_exception(std::string{rpc}, target));
        }
    } catch (const coap_server_error& e) {
        if (e.response_code() == not_implemented) {
            return std::make_exception_ptr(
                kythira::rpc_not_implemented_exception(std::string{rpc}, target));
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Not a CoAP status: passed through below.
    }
    return error;
}

}  // namespace coap_detail

}  // namespace kythira
