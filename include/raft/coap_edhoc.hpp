#pragma once

// EDHOC-to-OSCORE credential bootstrap (coap-transport-security spec,
// Requirement 5), via the `lakers` vcpkg overlay port
// (vcpkg-overlays/lakers/). Gated by LAKERS_AVAILABLE, defined by
// CMakeLists.txt only when `find_package(lakers CONFIG)` succeeds — which
// itself only happens when vcpkg was run with the optional "edhoc" feature
// enabled (see vcpkg.json), since lakers requires a Rust toolchain to build
// from source. When LAKERS_AVAILABLE is undefined, run_edhoc_handshake()
// throws coap_credential_bootstrap_error rather than silently behaving as
// oscore_bootstrap::static_provisioned (Requirement 5.4, Property 8).

#include <raft/coap_security.hpp>

#include <cstdint>
#include <functional>
#include <vector>

#ifdef LAKERS_AVAILABLE
extern "C" {
#include <lakers_kythira.h>
}
#endif

namespace kythira {

// Carries EDHOC protocol messages between the two peers. Both roles
// alternate send()/receive() in the order dictated by who speaks first
// (Initiator sends message_1 first; Responder waits to receive it first);
// see run_edhoc_handshake() below for the exact sequence. This is
// transport-agnostic on purpose — a real coap_client/coap_server can
// implement it over a `.well-known/edhoc` CoAP exchange, and tests can
// implement it directly over an in-process channel.
class edhoc_transport {
public:
    virtual ~edhoc_transport() = default;
    virtual auto send(const std::vector<std::byte>& message) -> void = 0;
    virtual auto receive() -> std::vector<std::byte> = 0;
};

#ifdef LAKERS_AVAILABLE

namespace detail {

inline auto to_lakers_bytes(const std::vector<std::byte>& v) -> const std::uint8_t* {
    return reinterpret_cast<const std::uint8_t*>(v.data());
}

// Grows buf to `len` bytes without changing capacity semantics elsewhere.
inline auto resize_bytes(std::vector<std::byte>& buf, std::size_t len) -> void {
    buf.resize(len);
}

}  // namespace detail

#endif  // LAKERS_AVAILABLE

// Runs the 3-message EDHOC exchange (RFC 9528, no message_4 — see
// vcpkg-overlays/lakers/README.md) in the role given by params.is_initiator,
// deriving OSCORE sender_id/recipient_id/master_secret/master_salt via
// edhoc_exporter(). Throws coap_credential_bootstrap_error on any failure:
// peer authentication failure, malformed message, or (when LAKERS_AVAILABLE
// is undefined) EDHOC simply not being compiled in. Never falls back to
// static_provisioned credentials (Requirement 5.4, Property 8).
inline auto run_edhoc_handshake(const edhoc_params& params, edhoc_transport& transport)
    -> oscore_credentials {
#ifndef LAKERS_AVAILABLE
    (void)params;
    (void)transport;
    throw coap_credential_bootstrap_error("edhoc",
                                          "lakers not compiled in (LAKERS_AVAILABLE unset)");
#else
    if (params.identity_private_key.size() != 32) {
        throw coap_credential_bootstrap_error(
            "edhoc", "identity_private_key must be exactly 32 bytes (raw P-256 scalar)");
    }

    auto fail = [](const std::string& reason) -> oscore_credentials {
        throw coap_credential_bootstrap_error("edhoc", reason);
    };

    oscore_credentials result;
    result.bootstrap_method = oscore_bootstrap::edhoc;
    result.edhoc = params;

    constexpr std::size_t kMasterSecretLen = 16;
    constexpr std::size_t kMasterSaltLen = 8;
    constexpr std::size_t kConnIdLen = 1;
    constexpr std::size_t kMsgBufLen = 256;
    // RFC 9528/draft-ietf-core-oscore-edhoc map OSCORE Sender/Recipient ID
    // to the EDHOC connection identifiers C_I/C_R; this FFI's parse
    // functions discard those (see lib.rs's _c_i/_c_r), so instead this
    // derives two additional exporter values (labels 2 and 3, beyond the
    // standard 0=secret/1=salt) and assigns them by role so they still
    // mirror correctly (initiator's sender_id == responder's recipient_id
    // and vice versa) — cryptographically sound (both derived from the
    // shared PRK via the same edhoc_exporter/HKDF-Expand as the secret and
    // salt), just not the RFC's literal C_I/C_R values.
    constexpr std::uint8_t kIdLabelA = 2;
    constexpr std::uint8_t kIdLabelB = 3;

    if (params.is_initiator) {
        LakersInitiator* h = lakers_initiator_new();
        if (!h) fail("failed to allocate initiator");
        struct Guard {
            LakersInitiator* h;
            ~Guard() { lakers_initiator_free(h); }
        } guard{h};

        if (lakers_initiator_set_identity(h, detail::to_lakers_bytes(params.identity_private_key),
                                          detail::to_lakers_bytes(params.identity_credential),
                                          params.identity_credential.size()) != LAKERS_OK) {
            return fail("set_identity failed (bad key length or credential)");
        }

        std::vector<std::byte> msg1(kMsgBufLen);
        std::size_t msg1_len = 0;
        if (lakers_initiator_prepare_message_1(h, reinterpret_cast<std::uint8_t*>(msg1.data()),
                                               msg1.size(), &msg1_len) != LAKERS_OK) {
            return fail("prepare_message_1 failed");
        }
        detail::resize_bytes(msg1, msg1_len);

        transport.send(msg1);
        auto msg2 = transport.receive();

        if (lakers_initiator_parse_message_2(h, detail::to_lakers_bytes(msg2), msg2.size()) !=
            LAKERS_OK) {
            return fail("parse_message_2 failed (malformed message)");
        }
        if (lakers_initiator_verify_message_2(h, detail::to_lakers_bytes(params.peer_credential),
                                              params.peer_credential.size()) != LAKERS_OK) {
            return fail("verify_message_2 failed (peer authentication failure)");
        }

        std::vector<std::byte> msg3(kMsgBufLen);
        std::size_t msg3_len = 0;
        if (lakers_initiator_prepare_message_3(h, reinterpret_cast<std::uint8_t*>(msg3.data()),
                                               msg3.size(), &msg3_len) != LAKERS_OK) {
            return fail("prepare_message_3 failed");
        }
        detail::resize_bytes(msg3, msg3_len);
        transport.send(msg3);

        if (lakers_initiator_complete(h) != LAKERS_OK) {
            return fail("handshake completion failed");
        }

        result.master_secret.resize(kMasterSecretLen);
        result.master_salt.resize(kMasterSaltLen);
        result.sender_id.resize(kConnIdLen);
        result.recipient_id.resize(kConnIdLen);
        if (lakers_initiator_exporter(h, 0,
                                      reinterpret_cast<std::uint8_t*>(result.master_secret.data()),
                                      kMasterSecretLen) != LAKERS_OK ||
            lakers_initiator_exporter(h, 1,
                                      reinterpret_cast<std::uint8_t*>(result.master_salt.data()),
                                      kMasterSaltLen) != LAKERS_OK ||
            lakers_initiator_exporter(h, kIdLabelA,
                                      reinterpret_cast<std::uint8_t*>(result.sender_id.data()),
                                      kConnIdLen) != LAKERS_OK ||
            lakers_initiator_exporter(h, kIdLabelB,
                                      reinterpret_cast<std::uint8_t*>(result.recipient_id.data()),
                                      kConnIdLen) != LAKERS_OK) {
            return fail("exporter failed");
        }
    } else {
        LakersResponder* h = lakers_responder_new(
            detail::to_lakers_bytes(params.identity_private_key),
            detail::to_lakers_bytes(params.identity_credential), params.identity_credential.size());
        if (!h) fail("failed to construct responder (bad key length or credential)");
        struct Guard {
            LakersResponder* h;
            ~Guard() { lakers_responder_free(h); }
        } guard{h};

        auto msg1 = transport.receive();
        if (lakers_responder_process_message_1(h, detail::to_lakers_bytes(msg1), msg1.size()) !=
            LAKERS_OK) {
            return fail("process_message_1 failed (malformed message)");
        }

        std::vector<std::byte> msg2(kMsgBufLen);
        std::size_t msg2_len = 0;
        if (lakers_responder_prepare_message_2(h, reinterpret_cast<std::uint8_t*>(msg2.data()),
                                               msg2.size(), &msg2_len) != LAKERS_OK) {
            return fail("prepare_message_2 failed");
        }
        detail::resize_bytes(msg2, msg2_len);
        transport.send(msg2);

        auto msg3 = transport.receive();
        if (lakers_responder_parse_message_3(h, detail::to_lakers_bytes(msg3), msg3.size()) !=
            LAKERS_OK) {
            return fail("parse_message_3 failed (malformed message)");
        }
        if (lakers_responder_verify_message_3(h, detail::to_lakers_bytes(params.peer_credential),
                                              params.peer_credential.size()) != LAKERS_OK) {
            return fail("verify_message_3 failed (peer authentication failure)");
        }

        result.master_secret.resize(kMasterSecretLen);
        result.master_salt.resize(kMasterSaltLen);
        result.sender_id.resize(kConnIdLen);
        result.recipient_id.resize(kConnIdLen);
        // Swapped relative to the initiator: A -> initiator's sender_id ==
        // this responder's recipient_id, and vice versa for B.
        if (lakers_responder_exporter(h, 0,
                                      reinterpret_cast<std::uint8_t*>(result.master_secret.data()),
                                      kMasterSecretLen) != LAKERS_OK ||
            lakers_responder_exporter(h, 1,
                                      reinterpret_cast<std::uint8_t*>(result.master_salt.data()),
                                      kMasterSaltLen) != LAKERS_OK ||
            lakers_responder_exporter(h, kIdLabelA,
                                      reinterpret_cast<std::uint8_t*>(result.recipient_id.data()),
                                      kConnIdLen) != LAKERS_OK ||
            lakers_responder_exporter(h, kIdLabelB,
                                      reinterpret_cast<std::uint8_t*>(result.sender_id.data()),
                                      kConnIdLen) != LAKERS_OK) {
            return fail("exporter failed");
        }
    }

    return result;
#endif  // LAKERS_AVAILABLE
}

}  // namespace kythira
