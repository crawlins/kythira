// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file wide_serial_test_support.hpp
/// @brief Issues a certificate with a caller-chosen serial — in particular a
///        20-octet one, as a cloud CA would — signed by a test
///        `certificate_authority`'s root.
///
/// The in-house CA only issues 64-bit serials
/// (`.kiro/specs/cloud-composite-node-ids/` Requirement 12.5), so tests that
/// need a cloud-width serial build the certificate here with the same
/// `detail::build_leaf_cert` the CA uses.

#include <raft/certificate_authority.hpp>
#include <raft/certificate_authority_impl.hpp>
#include <raft/certificate_serial.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace kythira::testing {

/// A 20-octet serial with its top bit set, so its DER encoding needs the
/// 0x00 sign octet — the widest case RFC 5280 allows.
[[nodiscard]] inline auto twenty_octet_serial(std::uint8_t last = 0x14)
    -> raft::testing::certificate_serial {
    std::array<std::uint8_t, 20> octets{};
    for (std::size_t i = 0; i < octets.size(); ++i) {
        octets[i] = static_cast<std::uint8_t>(0x80 + i);
    }
    octets.back() = last;
    return *raft::testing::certificate_serial::from_bytes(octets);
}

/// Signs `csr_pem` with `ca`'s root key, applying `options`, under `serial`.
/// The result looks like a provider's `sign_csr()` output: no private key,
/// `chain_pem` leaf then root, `serial` filled in.
[[nodiscard]] inline auto sign_csr_with_serial(const raft::testing::certificate_authority& ca,
                                               const std::string& csr_pem,
                                               const raft::testing::csr_signing_options& options,
                                               const raft::testing::certificate_serial& serial)
    -> raft::testing::pem_material {
    namespace detail = raft::testing::detail;

    auto read = [](const std::string& pem) {
        auto bio = detail::make_bio();
        BIO_write(bio.get(), pem.data(), static_cast<int>(pem.size()));
        return bio;
    };

    const auto& root_pem = ca.root_certificate_pem();
    auto root_bio = read(root_pem);
    detail::x509_ptr root{PEM_read_bio_X509(root_bio.get(), nullptr, nullptr, nullptr)};
    auto key_bio = read(raft::testing::detail_testing::unsafe_extract_ca_private_key_pem(ca));
    detail::evp_pkey_ptr key{PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr)};
    auto csr_bio = read(csr_pem);
    detail::x509_req_ptr req{PEM_read_bio_X509_REQ(csr_bio.get(), nullptr, nullptr, nullptr)};
    if (!root || !key || !req) {
        throw std::invalid_argument("sign_csr_with_serial: unreadable CA material or CSR");
    }
    detail::evp_pkey_ptr pubkey{X509_REQ_get_pubkey(req.get())};

    const auto now = std::chrono::system_clock::now();
    auto cert = detail::build_leaf_cert(root.get(), key.get(), X509_REQ_get_subject_name(req.get()),
                                        pubkey.get(), options.dns_names, options.ip_addresses,
                                        options.uri_names, options.server_auth, options.client_auth,
                                        now, now + options.validity, serial);

    raft::testing::pem_material out;
    out.certificate_pem = detail::serialize_cert(cert.get());
    out.chain_pem = out.certificate_pem + root_pem;
    out.serial = serial;
    return out;
}

}  // namespace kythira::testing
