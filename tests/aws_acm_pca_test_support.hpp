// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_acm_pca_test_support.hpp
/// @brief OpenSSL helpers shared by the ACM Private CA LocalStack and real
///        tests: build a CSR the provider's CSR policy accepts, read a
///        certificate's serial in the hex form `RevokeCertificate` expects,
///        and verify an issued chain against the CA's root.

#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <cctype>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kythira::testing::acm_pca {

/// A CSR plus the matching signing options, so `sign_csr`'s CSR policy check
/// passes. One-day validity keeps the request legal on a short-lived-mode CA
/// (which caps validity at seven days) as well as a general-purpose one.
struct leaf_request {
    raft::testing::csr_material csr;
    raft::testing::csr_signing_options options;
};

inline auto make_leaf_request(const std::string& common_name) -> leaf_request {
    raft::testing::leaf_certificate_options leaf;
    leaf.subject.common_name = common_name;
    leaf.dns_names = {common_name + ".internal"};
    leaf.server_auth = true;
    leaf.client_auth = true;

    leaf_request out;
    out.csr = raft::testing::generate_key_and_csr(leaf);
    out.options.dns_names = leaf.dns_names;
    out.options.server_auth = true;
    out.options.client_auth = true;
    out.options.validity = std::chrono::hours(24);
    return out;
}

using x509_ptr = std::unique_ptr<X509, decltype(&X509_free)>;

/// Every certificate in `pem`, in order. Throws when there is none.
inline auto read_certificates(const std::string& pem) -> std::vector<x509_ptr> {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
    std::vector<x509_ptr> certs;
    while (X509* cert = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)) {
        certs.emplace_back(cert, &X509_free);
    }
    if (certs.empty()) {
        throw std::runtime_error("no certificate in PEM input");
    }
    return certs;
}

/// The first certificate's serial as colon-separated lowercase hex
/// (`4c:6d:...`), the form `openssl x509 -text` prints and AWS documents for
/// `RevokeCertificate`'s `CertificateSerial`.
inline auto hex_serial(const std::string& certificate_pem) -> std::string {
    auto certs = read_certificates(certificate_pem);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> bn(
        ASN1_INTEGER_to_BN(X509_get0_serialNumber(certs.front().get()), nullptr), &BN_free);
    if (!bn) {
        throw std::runtime_error("unreadable certificate serial");
    }
    std::unique_ptr<char, void (*)(char*)> hex(BN_bn2hex(bn.get()),
                                               [](char* p) { OPENSSL_free(p); });
    std::string digits(hex.get());
    if (digits.size() % 2 != 0) {
        digits.insert(digits.begin(), '0');
    }
    std::string out;
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        if (!out.empty()) {
            out += ':';
        }
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(digits[i])));
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(digits[i + 1])));
    }
    return out;
}

/// True when the first certificate in `chain_pem` verifies up to `root_pem`,
/// with the rest of `chain_pem` as untrusted intermediates. Covers both a root
/// CA (the chain is just the root) and a subordinate one.
inline auto chain_verifies(const std::string& chain_pem, const std::string& root_pem) -> bool {
    auto chain = read_certificates(chain_pem);
    auto roots = read_certificates(root_pem);

    std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(X509_STORE_new(),
                                                                  &X509_STORE_free);
    for (auto& root : roots) {
        X509_STORE_add_cert(store.get(), root.get());
    }
    std::unique_ptr<STACK_OF(X509), void (*)(STACK_OF(X509)*)> untrusted(
        sk_X509_new_null(), [](STACK_OF(X509) * s) { sk_X509_free(s); });
    for (std::size_t i = 1; i < chain.size(); ++i) {
        sk_X509_push(untrusted.get(), chain[i].get());
    }
    std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> ctx(X509_STORE_CTX_new(),
                                                                        &X509_STORE_CTX_free);
    if (X509_STORE_CTX_init(ctx.get(), store.get(), chain.front().get(), untrusted.get()) != 1) {
        return false;
    }
    return X509_verify_cert(ctx.get()) == 1;
}

}  // namespace kythira::testing::acm_pca
