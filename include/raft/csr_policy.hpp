// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file csr_policy.hpp
/// @brief Checks that a CSR asks for nothing beyond what the caller approved.
///
/// The local and Azure signers build every extension themselves and ignore
/// whatever the CSR requests. The cloud CAs do not: AWS ACM Private CA's
/// `EndEntityCertificate/V1` template, Google CAS without a restrictive
/// template or pool issuance policy, and OCI Certificates all take the
/// subject alternative names (and, for some, other extensions) straight from
/// the CSR. Their `sign_csr()` therefore never applied the vetted
/// `csr_signing_options` at all — so a caller approved for
/// `svc.example.com` could put `URI:spiffe://prod/admin` or `*.corp` in its
/// CSR and get it signed. That is the same class of bug as OpenBao's ACME
/// "unvalidated SAN" issuance (GHSA-x8fg-h69x-p28f). Every CSR-authoritative
/// provider calls `enforce_csr_matches_options()` before submitting.

#include <raft/certificate_authority.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace raft::testing {

namespace csr_policy_detail {

[[noreturn]] inline void reject(const std::string& why) {
    throw std::invalid_argument("CSR rejected: " + why);
}

[[nodiscard]] inline auto ip_bytes(const std::string& ip) -> std::vector<unsigned char> {
    unsigned char buf[16] = {};
    if (inet_pton(AF_INET, ip.c_str(), buf) == 1) {
        return {buf, buf + 4};
    }
    if (inet_pton(AF_INET6, ip.c_str(), buf) == 1) {
        return {buf, buf + 16};
    }
    return {};
}

[[nodiscard]] inline auto lower(std::string s) -> std::string {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return s;
}

}  // namespace csr_policy_detail

/// Throws std::invalid_argument unless `csr_pem` is a well-formed CSR whose
/// requested extensions stay within `options`:
///   - subjectAltName: only DNS names and IP addresses, each one listed in
///     `options.dns_names` / `options.ip_addresses` (DNS compared
///     case-insensitively, IPs by address);
///   - basicConstraints: if present, must not assert CA:TRUE;
///   - keyUsage: must not include keyCertSign or cRLSign;
///   - extendedKeyUsage: only serverAuth/clientAuth, and only those
///     `options` enables;
///   - subjectKeyIdentifier is tolerated; any other requested extension
///     (nameConstraints, policies, AIA, custom OIDs, ...) is refused.
inline void enforce_csr_matches_options(const std::string& csr_pem,
                                        const csr_signing_options& options) {
    using namespace csr_policy_detail;

    BIO* bio = BIO_new_mem_buf(csr_pem.data(), static_cast<int>(csr_pem.size()));
    X509_REQ* req =
        bio == nullptr ? nullptr : PEM_read_bio_X509_REQ(bio, nullptr, nullptr, nullptr);
    if (bio != nullptr) {
        BIO_free(bio);
    }
    if (req == nullptr) {
        reject("not a PEM certificate request");
    }
    STACK_OF(X509_EXTENSION)* exts = X509_REQ_get_extensions(req);
    X509_REQ_free(req);

    std::vector<std::string> allowed_dns;
    for (const auto& d : options.dns_names) {
        allowed_dns.push_back(lower(d));
    }
    std::vector<std::vector<unsigned char>> allowed_ips;
    for (const auto& ip : options.ip_addresses) {
        allowed_ips.push_back(ip_bytes(ip));
    }

    std::string failure;
    for (int i = 0; exts != nullptr && i < sk_X509_EXTENSION_num(exts) && failure.empty(); ++i) {
        X509_EXTENSION* ext = sk_X509_EXTENSION_value(exts, i);
        int nid = OBJ_obj2nid(X509_EXTENSION_get_object(ext));
        switch (nid) {
            case NID_subject_alt_name: {
                auto* names = static_cast<GENERAL_NAMES*>(X509V3_EXT_d2i(ext));
                if (names == nullptr) {
                    failure = "unparseable subjectAltName";
                    break;
                }
                for (int j = 0; j < sk_GENERAL_NAME_num(names) && failure.empty(); ++j) {
                    GENERAL_NAME* gn = sk_GENERAL_NAME_value(names, j);
                    if (gn->type == GEN_DNS) {
                        std::string v(
                            reinterpret_cast<const char*>(ASN1_STRING_get0_data(gn->d.dNSName)),
                            static_cast<std::size_t>(ASN1_STRING_length(gn->d.dNSName)));
                        if (std::find(allowed_dns.begin(), allowed_dns.end(), lower(v)) ==
                            allowed_dns.end()) {
                            failure = "requests unapproved DNS name \"" + v + "\"";
                        }
                    } else if (gn->type == GEN_IPADD) {
                        const unsigned char* data = ASN1_STRING_get0_data(gn->d.iPAddress);
                        std::vector<unsigned char> v(data,
                                                     data + ASN1_STRING_length(gn->d.iPAddress));
                        if (std::find(allowed_ips.begin(), allowed_ips.end(), v) ==
                            allowed_ips.end()) {
                            failure = "requests an unapproved IP address";
                        }
                    } else {
                        failure = "requests a subjectAltName type other than DNS/IP";
                    }
                }
                GENERAL_NAMES_free(names);
                break;
            }
            case NID_basic_constraints: {
                auto* bc = static_cast<BASIC_CONSTRAINTS*>(X509V3_EXT_d2i(ext));
                if (bc == nullptr || bc->ca != 0) {
                    failure = "requests a CA certificate";
                }
                BASIC_CONSTRAINTS_free(bc);
                break;
            }
            case NID_key_usage: {
                auto* ku = static_cast<ASN1_BIT_STRING*>(X509V3_EXT_d2i(ext));
                // Bit 5 = keyCertSign, bit 6 = cRLSign (RFC 5280 §4.2.1.3).
                if (ku == nullptr || ASN1_BIT_STRING_get_bit(ku, 5) != 0 ||
                    ASN1_BIT_STRING_get_bit(ku, 6) != 0) {
                    failure = "requests certificate- or CRL-signing key usage";
                }
                ASN1_BIT_STRING_free(ku);
                break;
            }
            case NID_ext_key_usage: {
                auto* eku = static_cast<EXTENDED_KEY_USAGE*>(X509V3_EXT_d2i(ext));
                if (eku == nullptr) {
                    failure = "unparseable extendedKeyUsage";
                    break;
                }
                for (int j = 0; j < sk_ASN1_OBJECT_num(eku) && failure.empty(); ++j) {
                    int usage = OBJ_obj2nid(sk_ASN1_OBJECT_value(eku, j));
                    bool ok = (usage == NID_server_auth && options.server_auth) ||
                              (usage == NID_client_auth && options.client_auth);
                    if (!ok) {
                        failure = "requests an unapproved extended key usage";
                    }
                }
                EXTENDED_KEY_USAGE_free(eku);
                break;
            }
            case NID_subject_key_identifier:
                break;
            default:
                failure = "requests unsupported extension " + std::to_string(nid);
                break;
        }
    }
    sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
    if (!failure.empty()) {
        reject(failure);
    }
}

}  // namespace raft::testing
