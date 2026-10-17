// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The DTLS settings both halves of the cross-backend interop suites agree on:
// coap_interop_peer.cpp builds its server's security from its command line
// through peer_security(), and coap_interop_cases.hpp builds the matching
// client side and that command line. Backend-neutral (raft/coap_security.hpp
// names no CoAP library), so either process can include it.
//
// The peer's command line is one of:
//
//   (nothing)                  plaintext
//   psk                        DTLS-PSK with the identity and key below
//   pki CERT KEY CA            DTLS-PKI, verifying the client against CA

#include <raft/coap_security.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace coap_interop {

inline constexpr const char* psk_identity = "kythira-interop";

[[nodiscard]] inline auto psk_security() -> kythira::coap_security_config {
    std::vector<std::byte> key;
    for (int i = 1; i <= 16; ++i) {
        key.push_back(static_cast<std::byte>(i));
    }
    kythira::coap_security_config config;
    config.mode = kythira::coap_auth_mode::dtls_psk;
    config.credentials = kythira::psk_credentials{psk_identity, std::move(key)};
    return config;
}

[[nodiscard]] inline auto pki_security(std::string cert_file, std::string key_file,
                                       std::string ca_file) -> kythira::coap_security_config {
    kythira::pki_credentials creds;
    creds.cert_file = std::move(cert_file);
    creds.key_file = std::move(key_file);
    creds.ca_file = std::move(ca_file);
    creds.verify_peer_cert = true;
    kythira::coap_security_config config;
    config.mode = kythira::coap_auth_mode::dtls_pki;
    config.credentials = std::move(creds);
    return config;
}

/// The server security a peer's command line asks for; throws on anything
/// else, so a typo fails the case instead of quietly running in the clear.
[[nodiscard]] inline auto peer_security(const std::vector<std::string>& args)
    -> kythira::coap_security_config {
    if (args.empty()) {
        return {};
    }
    if (args.size() == 1 && args[0] == "psk") {
        return psk_security();
    }
    if (args.size() == 4 && args[0] == "pki") {
        return pki_security(args[1], args[2], args[3]);
    }
    throw std::invalid_argument("usage: coap_interop_peer [psk | pki CERT KEY CA]");
}

}  // namespace coap_interop
