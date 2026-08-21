// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The CoAP transport's *library-neutral* configuration and message-tracking
// types, shared by every CoAP backend.
//
// WHY THIS FILE EXISTS
// --------------------
// These declarations used to live in coap_transport.hpp, which includes
// <coap3/coap.h> whenever LIBCOAP_AVAILABLE is defined. That is fine for the
// libcoap-backed coap_client/coap_server, but it makes the header unusable
// from the libnyoci-backed backend (coap_transport_libnyoci_impl.hpp): the two
// C libraries' headers cannot coexist in one translation unit. libcoap spells
// the option numbers as object-like macros (`#define COAP_OPTION_IF_MATCH 1`)
// while libnyoci spells them as enumerators (`COAP_OPTION_IF_MATCH = 1,`), so
// including libcoap first macro-expands the middle of libnyoci's enum into
// `1 = 1,` and the parse dies. There is no include order that fixes it.
//
// So anything both backends need — the config structs a caller fills in, and
// the promise-bridging record a pending request is tracked by — lives here,
// free of any libcoap type, and coap_transport.hpp includes it. That keeps one
// definition of coap_client_config/coap_server_config across backends, which
// is the point: a caller must be able to hand the same config to either one
// (.kiro/specs/coap-transport-libnyoci/ Requirement 1).
//
// What deliberately did NOT move: block_transfer_state (it holds a
// coap_session_t*), coap_error_info (it is keyed by coap_pdu_code_t), and the
// *_transport_types bundles — all still in coap_transport.hpp, because they
// are either libcoap-shaped or of no interest to another backend.

#include <raft/coap_security.hpp>
// For edhoc_transport, named by both configs' edhoc_transport_factory. Also
// libcoap-free, so it does not compromise this header's neutrality.
#include <raft/coap_edhoc.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kythira {

// Message tracking structures - using callbacks to work with generic future types
struct pending_message {
    std::string token;
    std::uint16_t message_id;
    std::chrono::steady_clock::time_point send_time;
    std::chrono::milliseconds timeout;
    std::size_t retransmission_count{0};
    /// Takes the media type the response actually arrived in alongside the
    /// bytes, because only `handle_response` can see the PDU's Content-Format
    /// option and only this callback knows the `Response` type to decode into.
    /// Passing it explicitly keeps that one fact from becoming mutable state
    /// shared between the two (Requirement 6.4).
    std::function<void(std::vector<std::byte>, const std::string&)> resolve_callback;
    std::function<void(std::exception_ptr)> reject_callback;
    std::vector<std::byte> original_payload;
    std::string target_endpoint;
    std::string resource_path;
    bool is_confirmable{true};

    pending_message(std::string tok, std::uint16_t msg_id, std::chrono::milliseconds to,
                    std::function<void(std::vector<std::byte>, const std::string&)> resolve_cb,
                    std::function<void(std::exception_ptr)> reject_cb,
                    std::vector<std::byte> payload, std::string endpoint, std::string path,
                    bool confirmable)
        : token(std::move(tok)),
          message_id(msg_id),
          send_time(std::chrono::steady_clock::now()),
          timeout(to),
          resolve_callback(std::move(resolve_cb)),
          reject_callback(std::move(reject_cb)),
          original_payload(std::move(payload)),
          target_endpoint(std::move(endpoint)),
          resource_path(std::move(path)),
          is_confirmable(confirmable) {}
};

struct received_message_info {
    std::uint16_t message_id;
    std::chrono::steady_clock::time_point received_time;

    received_message_info(std::uint16_t msg_id)
        : message_id(msg_id), received_time(std::chrono::steady_clock::now()) {}
};

// Configuration structures
struct coap_client_config {
    bool enable_dtls{false};
    bool enable_block_transfer{true};
    std::size_t max_block_size{1024};
    std::size_t max_sessions{100};
    std::chrono::milliseconds session_timeout{30000};
    std::chrono::milliseconds ack_timeout{2000};
    std::chrono::milliseconds ack_random_factor_ms{1000};
    std::size_t max_retransmit{4};
    bool use_confirmable_messages{true};
    std::size_t max_retransmissions{4};
    std::chrono::milliseconds retransmission_timeout{2000};
    double exponential_backoff_factor{2.0};

    // DTLS configuration
    std::string cert_file;
    std::string key_file;
    std::string ca_file;
    std::string psk_identity;
    std::vector<std::byte> psk_key;
    bool verify_peer_cert{true};
    std::vector<std::string> cipher_suites;  // Allowed cipher suites for DTLS
    bool enable_session_resumption{true};    // Enable DTLS session resumption

    // Multicast configuration
    bool enable_multicast{false};
    std::string multicast_address{"224.0.1.187"};
    std::uint16_t multicast_port{5683};

    // Performance optimization settings
    bool enable_session_reuse{true};
    bool enable_connection_pooling{true};
    std::size_t connection_pool_size{10};
    bool enable_concurrent_processing{true};
    std::size_t max_concurrent_requests{50};
    bool enable_memory_optimization{false};
    std::size_t memory_pool_size{1024 * 1024};      // 1MB
    std::size_t memory_pool_block_size{4096};       // 4KB blocks
    bool enable_periodic_pool_reset{false};         // Enable periodic memory pool reset
    std::chrono::seconds pool_reset_interval{300};  // Reset interval (5 minutes default)
    bool enable_serialization_caching{false};
    std::size_t serialization_cache_size{100};
    std::size_t max_cache_entries{100};
    std::chrono::milliseconds cache_ttl{60000};  // 1 minute
    bool enable_certificate_validation{true};

    // Explicit channel-security mode (coap-transport-security spec). Left at
    // its default (mode == none), the legacy DTLS fields above continue to
    // drive behavior via translate_legacy_fields() (Requirement 8). Setting
    // security.mode explicitly selects one of the five coap_auth_mode
    // values without any field inference (Requirement 1.2).
    coap_security_config security{};

    // Required when security.mode == oscore and its oscore_credentials
    // have bootstrap_method == edhoc: constructs the edhoc_transport used
    // to carry EDHOC's messages to/from the peer (Requirement 5.2). Left
    // null, EDHOC bootstrap fails construction with
    // coap_credential_bootstrap_error (Requirement 5.4) rather than
    // guessing at a transport.
    std::function<std::unique_ptr<edhoc_transport>()> edhoc_transport_factory;
};

struct coap_server_config {
    bool enable_dtls{false};
    bool enable_block_transfer{true};
    std::size_t max_block_size{1024};
    std::size_t max_concurrent_sessions{100};
    std::chrono::milliseconds session_timeout{30000};
    std::size_t max_request_size{65536};

    // DTLS configuration
    std::string cert_file;
    std::string key_file;
    std::string ca_file;
    std::string psk_identity;
    std::vector<std::byte> psk_key;
    bool verify_peer_cert{true};
    std::vector<std::string> cipher_suites;  // Allowed cipher suites for DTLS
    bool enable_session_resumption{true};    // Enable DTLS session resumption

    // Multicast configuration
    bool enable_multicast{false};
    std::string multicast_address{"224.0.1.187"};  // CoAP multicast address
    std::uint16_t multicast_port{5683};

    // Performance optimization settings
    bool enable_concurrent_processing{true};
    std::size_t max_concurrent_requests{100};
    bool enable_memory_optimization{false};
    std::size_t memory_pool_size{1024 * 1024};      // 1MB
    std::size_t memory_pool_block_size{4096};       // 4KB blocks
    bool enable_periodic_pool_reset{false};         // Enable periodic memory pool reset
    std::chrono::seconds pool_reset_interval{300};  // Reset interval (5 minutes default)
    bool enable_serialization_caching{false};
    std::size_t serialization_cache_size{100};

    // Explicit channel-security mode (coap-transport-security spec). See
    // coap_client_config::security for the same semantics.
    coap_security_config security{};

    // See coap_client_config::edhoc_transport_factory for the same
    // semantics (Requirement 5.2).
    std::function<std::unique_ptr<edhoc_transport>()> edhoc_transport_factory;
};

// ── legacy field translation (coap-transport-security Requirement 8) ──────
// Reproduces today's setup_dtls_context() field-inference exactly when
// security.mode is left at its default (coap_auth_mode::none): cert_file
// populated selects dtls_pki, psk_identity populated selects dtls_psk,
// neither selects none. When security.mode is explicitly set, the legacy
// fields must be empty (Requirement 2.3) — populating both is rejected as a
// configuration error rather than guessed at.
//
// Lives here rather than in coap_security_impl.hpp (its original home)
// because it is a pure function of the config structs above and touches no
// libcoap type, and every CoAP backend has to reach the same verdict about a
// given config — including the ones that cannot include coap_security_impl.hpp
// at all. coap_security_impl.hpp still re-exports it by including this header.
template<typename LegacyConfig>
inline auto translate_legacy_fields(const LegacyConfig& cfg) -> coap_security_config {
    const bool legacy_pki = !cfg.cert_file.empty();
    const bool legacy_psk = !cfg.psk_identity.empty();

    if (cfg.security.mode != coap_auth_mode::none) {
        if (legacy_pki || legacy_psk) {
            throw coap_security_config_error(
                "both security.mode and legacy DTLS fields (cert_file/psk_identity) are "
                "set; populate only one");
        }
        return cfg.security;
    }

    if (legacy_pki) {
        pki_credentials creds;
        creds.cert_file = cfg.cert_file;
        creds.key_file = cfg.key_file;
        creds.ca_file = cfg.ca_file;
        creds.verify_peer_cert = cfg.verify_peer_cert;
        creds.cipher_suites = cfg.cipher_suites;
        return coap_security_config{coap_auth_mode::dtls_pki, creds, std::nullopt};
    }
    if (legacy_psk) {
        psk_credentials creds{cfg.psk_identity, cfg.psk_key};
        return coap_security_config{coap_auth_mode::dtls_psk, creds, std::nullopt};
    }
    return coap_security_config{coap_auth_mode::none, std::monostate{}, std::nullopt};
}

}  // namespace kythira
