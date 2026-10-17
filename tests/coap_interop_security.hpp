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
//   oscore                     OSCORE with the static context below
//   edhoc                      OSCORE whose context EDHOC derives on the wire

#include <raft/coap_security.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

/// A static OSCORE context, mirrored: the client sends as 0x00 and the
/// server as 0x01. Sequence state is volatile because the keys are fixed
/// test values that every run reuses.
[[nodiscard]] inline auto oscore_security(bool is_client) -> kythira::coap_security_config {
    kythira::oscore_credentials creds;
    creds.volatile_sequence_state = true;
    creds.master_secret = std::vector<std::byte>(16, std::byte{0x2a});
    creds.master_salt = std::vector<std::byte>(8, std::byte{0x77});
    creds.sender_id = {is_client ? std::byte{0x00} : std::byte{0x01}};
    creds.recipient_id = {is_client ? std::byte{0x01} : std::byte{0x00}};
    kythira::coap_security_config config;
    config.mode = kythira::coap_auth_mode::oscore;
    config.credentials = std::move(creds);
    return config;
}

/// "a1b2..." as bytes.
[[nodiscard]] inline auto hex_bytes(std::string_view hex) -> std::vector<std::byte> {
    const auto nibble = [](char c) -> int { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    std::vector<std::byte> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::byte>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return out;
}

/// OSCORE whose context is derived by EDHOC over /.well-known/edhoc, with the
/// RFC 9529 Chapter 3 credentials the backends' own EDHOC suites use. The
/// client is the Initiator.
[[nodiscard]] inline auto edhoc_security(bool is_client) -> kythira::coap_security_config {
    const auto cred_i = hex_bytes(
        "A2027734322D35302D33312D46462D45462D33372D33322D333908A101A5010202412B2001215820AC75E9EC"
        "E3E50BFC8ED60399889522405C47BF16DF96660A41298CB4307F7EB62258206E5DE611388A4B8A8211334AC7D"
        "37ECB52A387D257E6DB3C2A93DF21FF3AFFC8");
    const auto cred_r = hex_bytes(
        "A2026008A101A5010202410A2001215820BBC34960526EA4D32E940CAD2A234148DDC21791A12AFBCBAC93622"
        "046DD44F02258204519E257236B2A0CE2023F0931F1F386CA7AFDA64FCDE0108C224C51EABF6072");
    kythira::oscore_credentials creds;
    creds.volatile_sequence_state = true;
    creds.bootstrap_method = kythira::oscore_bootstrap::edhoc;
    creds.edhoc.is_initiator = is_client;
    creds.edhoc.identity_credential = is_client ? cred_i : cred_r;
    creds.edhoc.identity_private_key =
        hex_bytes(is_client ? "fb13adeb6518cee5f88417660841142e830a81fe334380a953406a1305e8706b"
                            : "72cc4761dbd4c78f758931aa589d348d1ef874a7e303ede2f140dcf3e6aa4aac");
    creds.edhoc.peer_credential = is_client ? cred_r : cred_i;
    kythira::coap_security_config config;
    config.mode = kythira::coap_auth_mode::oscore;
    config.credentials = std::move(creds);
    return config;
}

/// What the peer answers an InstallSnapshot with, and what the client
/// expects back: depends on every byte and its position, so a block lost,
/// repeated or reordered on the way in changes it. Not security, but shared
/// by both halves like everything else here.
template<typename Bytes>
[[nodiscard]] inline auto snapshot_digest(const Bytes& data) -> std::uint64_t {
    std::uint64_t digest = data.size();
    for (std::size_t i = 0; i < data.size(); ++i) {
        digest = digest * 31 + std::to_integer<std::uint64_t>(data[i]) + i;
    }
    return digest;
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
    if (args.size() == 1 && args[0] == "oscore") {
        return oscore_security(false);
    }
    if (args.size() == 1 && args[0] == "edhoc") {
        return edhoc_security(false);
    }
    if (args.size() == 4 && args[0] == "pki") {
        return pki_security(args[1], args[2], args[3]);
    }
    throw std::invalid_argument(
        "usage: coap_interop_peer [psk | pki CERT KEY CA | oscore | edhoc]");
}

}  // namespace coap_interop
