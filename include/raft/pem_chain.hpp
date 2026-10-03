// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file pem_chain.hpp
/// @brief Splitting, comparing and assembling PEM certificate chains
///        (`.kiro/specs/oci-ca-chain-leaf/`).
///
/// `pem_material::chain_pem` is a *leaf-first* chain: the leaf, then each
/// issuer up to and including the root. Providers whose upstream API returns
/// the leaf and the issuer chain separately (OCI's `certificatePem` and
/// `certChainPem`) build it with `leaf_first`, and
/// `issuing_tls_material_source` uses the same helpers to check the shape of
/// whatever a provider hands back.
///
/// Certificates are compared by DER, not PEM text: two encodings of the same
/// certificate can differ in line width, line endings or trailing newline, and
/// a text comparison would miss a leaf that is really there and duplicate it.

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::pem_chain {

namespace detail {

inline constexpr std::string_view begin_marker = "-----BEGIN CERTIFICATE-----";
inline constexpr std::string_view end_marker = "-----END CERTIFICATE-----";

struct x509_deleter {
    void operator()(X509* p) const noexcept { X509_free(p); }
};
struct bio_deleter {
    void operator()(BIO* p) const noexcept { BIO_free(p); }
};

/// Parses exactly one PEM certificate block. Throws std::invalid_argument
/// when it does not parse.
inline auto parse(std::string_view block) -> std::unique_ptr<X509, x509_deleter> {
    std::unique_ptr<BIO, bio_deleter> bio(
        BIO_new_mem_buf(block.data(), static_cast<int>(block.size())));
    if (!bio) {
        throw std::runtime_error("pem_chain: BIO_new_mem_buf failed");
    }
    std::unique_ptr<X509, x509_deleter> cert(
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
    if (!cert) {
        throw std::invalid_argument("pem_chain: CERTIFICATE block does not parse");
    }
    return cert;
}

inline auto der(const X509* cert) -> std::string {
    unsigned char* out = nullptr;
    const int len = i2d_X509(cert, &out);
    if (len <= 0) {
        throw std::runtime_error("pem_chain: i2d_X509 failed");
    }
    std::string bytes(reinterpret_cast<const char*>(out), static_cast<std::size_t>(len));
    OPENSSL_free(out);
    return bytes;
}

inline auto is_blank(std::string_view text) -> bool {
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return std::isspace(c) != 0; });
}

}  // namespace detail

/// @brief Splits a PEM bundle into one string per certificate, each ending in
///        '\n', in the order they appear.
///
/// Non-certificate blocks (a private key, a CRL) and text between blocks are
/// ignored. Throws `std::invalid_argument` when a CERTIFICATE block fails to
/// parse or has no END line.
[[nodiscard]] inline auto split_certificates(std::string_view bundle) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while ((pos = bundle.find(detail::begin_marker, pos)) != std::string_view::npos) {
        const auto end = bundle.find(detail::end_marker, pos);
        if (end == std::string_view::npos) {
            throw std::invalid_argument("pem_chain: CERTIFICATE block has no END line");
        }
        std::string block(bundle.substr(pos, end + detail::end_marker.size() - pos));
        block += '\n';
        (void)detail::parse(block);
        out.push_back(std::move(block));
        pos = end + detail::end_marker.size();
    }
    return out;
}

/// @brief True when the first certificate of @p a and of @p b decode to
///        identical DER. Throws `std::invalid_argument` when either fails to
///        parse.
[[nodiscard]] inline auto same_certificate(std::string_view a, std::string_view b) -> bool {
    return detail::der(detail::parse(a).get()) == detail::der(detail::parse(b).get());
}

/// @brief Returns @p leaf followed by every certificate in @p issuers,
///        skipping a leading copy of the leaf if @p issuers already starts
///        with it. Each block ends in '\n'.
///
/// @p leaf must hold at least one certificate; only its first is used.
/// @p issuers may be empty or blank (the result is then the leaf alone), but
/// non-blank text holding no certificate throws `std::invalid_argument`
/// rather than silently yielding a chain with no issuer.
[[nodiscard]] inline auto leaf_first(std::string_view leaf, std::string_view issuers)
    -> std::string {
    const auto leaf_certs = split_certificates(leaf);
    if (leaf_certs.empty()) {
        throw std::invalid_argument("pem_chain: leaf holds no certificate");
    }
    auto chain = split_certificates(issuers);
    if (chain.empty() && !detail::is_blank(issuers)) {
        throw std::invalid_argument("pem_chain: issuer chain holds no certificate");
    }
    if (!chain.empty() && same_certificate(chain.front(), leaf_certs.front())) {
        chain.erase(chain.begin());
    }
    std::string out = leaf_certs.front();
    for (const auto& cert : chain) {
        out += cert;
    }
    return out;
}

}  // namespace kythira::pem_chain
