#pragma once

/// @file ca_http_helpers.hpp
/// @brief JSON/TLS-peer-certificate helpers shared by the `/v1/*` HTTP route
///        handlers of `cmd/ca_service/main.cpp` (--serve mode) and
///        `cmd/ca_cluster_node/main.cpp` — the same wire format and mTLS
///        renewal semantics apply to both (Requirement 17.6: "the same
///        client-facing HTTP API as ca_service --serve").

#include <raft/certificate_authority.hpp>

#include <boost/json.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#endif

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace raft::testing {

namespace ca_http_helpers_detail {
struct x509_deleter {
    void operator()(X509* c) const noexcept {
        if (c != nullptr) {
            X509_free(c);
        }
    }
};
}  // namespace ca_http_helpers_detail

using x509_ptr_generic = std::unique_ptr<X509, ca_http_helpers_detail::x509_deleter>;

inline auto pem_material_to_json(const pem_material& mat) -> boost::json::object {
    boost::json::object obj;
    obj["certificate_pem"] = mat.certificate_pem;
    obj["chain_pem"] = mat.chain_pem;
    return obj;
}

inline auto parse_csr_signing_options(const boost::json::object& obj) -> csr_signing_options {
    csr_signing_options opts;
    if (const auto* v = obj.if_contains("dns_names"); (v != nullptr) && v->is_array()) {
        for (const auto& e : v->as_array()) {
            opts.dns_names.push_back(std::string(e.as_string()));
        }
    }
    if (const auto* v = obj.if_contains("ip_addresses"); (v != nullptr) && v->is_array()) {
        for (const auto& e : v->as_array()) {
            opts.ip_addresses.push_back(std::string(e.as_string()));
        }
    }
    if (const auto* v = obj.if_contains("server_auth"); (v != nullptr) && v->is_bool()) {
        opts.server_auth = v->as_bool();
    }
    if (const auto* v = obj.if_contains("client_auth"); (v != nullptr) && v->is_bool()) {
        opts.client_auth = v->as_bool();
    }
    if (const auto* v = obj.if_contains("validity_days"); (v != nullptr) && v->is_number()) {
        opts.validity = std::chrono::hours(24 * v->to_number<int>());
    }
    return opts;
}

// Computes the colon-separated, uppercase-hex SHA-256 fingerprint of `cert`
// (Requirement 19.1) — the standard "openssl x509 -fingerprint -sha256"
// format, e.g. "AA:BB:CC:...". Uses X509_digest rather than hashing the DER
// bytes manually, matching how OpenSSL's own CLI computes it.
[[nodiscard]] inline auto sha256_fingerprint_hex(X509* cert) -> std::string {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (X509_digest(cert, EVP_sha256(), digest, &digest_len) != 1) {
        throw std::runtime_error("ca_http_helpers: X509_digest failed");
    }
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned int i = 0; i < digest_len; ++i) {
        if (i > 0) {
            out << ':';
        }
        out.width(2);
        out.fill('0');
        out << static_cast<unsigned int>(digest[i]);
    }
    return out.str();
}

// Parses every PEM certificate block in `pem_bundle` (a --tls-cert file may
// contain just a leaf, or a leaf+intermediates+root chain) and returns the
// LAST one — by PEM bundle convention the root/topmost issuer — unless an
// earlier block is self-signed (issuer == subject), in which case that one
// is returned instead, since a self-signed cert is unambiguously a root
// regardless of its position in the file. Throws if the bundle contains no
// parseable certificate.
[[nodiscard]] inline auto root_cert_from_pem_bundle(const std::string& pem_bundle)
    -> x509_ptr_generic {
    BIO* bio = BIO_new_mem_buf(pem_bundle.data(), static_cast<int>(pem_bundle.size()));
    if (bio == nullptr) {
        throw std::runtime_error("ca_http_helpers: BIO_new_mem_buf failed");
    }
    std::vector<x509_ptr_generic> certs;
    while (true) {
        X509* c = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (c == nullptr) {
            break;
        }
        certs.emplace_back(c);
    }
    BIO_free(bio);
    if (certs.empty()) {
        throw std::runtime_error("ca_http_helpers: no certificate found in PEM bundle");
    }
    for (auto& c : certs) {
        if (X509_NAME_cmp(X509_get_subject_name(c.get()), X509_get_issuer_name(c.get())) == 0) {
            return std::move(c);
        }
    }
    return std::move(certs.back());
}

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT

// Extracts dns_names/ip_addresses/server_auth/client_auth from a presented
// certificate's subjectAltName/extendedKeyUsage extensions, for
// POST /v1/certificates/renew (Requirement 15.3): "the returned certificate
// SHALL carry the same subject and SAN entries as the presented certificate."
inline auto options_from_presented_cert(X509* cert) -> csr_signing_options {
    csr_signing_options opts;

    auto* san_ext =
        static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (san_ext != nullptr) {
        for (int i = 0; i < sk_GENERAL_NAME_num(san_ext); ++i) {
            GENERAL_NAME* name = sk_GENERAL_NAME_value(san_ext, i);
            if (name->type == GEN_DNS) {
                auto* s = name->d.dNSName;
                opts.dns_names.emplace_back(reinterpret_cast<const char*>(ASN1_STRING_get0_data(s)),
                                            static_cast<std::size_t>(ASN1_STRING_length(s)));
            } else if (name->type == GEN_IPADD) {
                auto* s = name->d.iPAddress;
                const unsigned char* bytes = ASN1_STRING_get0_data(s);
                int len = ASN1_STRING_length(s);
                char buf[INET6_ADDRSTRLEN] = {};
                if (len == 4 && inet_ntop(AF_INET, bytes, buf, sizeof(buf)) != nullptr) {
                    opts.ip_addresses.emplace_back(buf);
                } else if (len == 16 && inet_ntop(AF_INET6, bytes, buf, sizeof(buf)) != nullptr) {
                    opts.ip_addresses.emplace_back(buf);
                }
            }
        }
        GENERAL_NAMES_free(san_ext);
    }

    bool has_eku = (X509_get_extension_flags(cert) & EXFLAG_XKUSAGE) != 0u;
    if (has_eku) {
        std::uint32_t xkusage = X509_get_extended_key_usage(cert);
        opts.server_auth = (xkusage & XKU_SSL_SERVER) != 0;
        opts.client_auth = (xkusage & XKU_SSL_CLIENT) != 0;
    } else {
        opts.server_auth = true;
        opts.client_auth = true;
    }
    return opts;
}

// Chain-verifies `cert` against `root_pem`. Returns true iff it verifies.
inline auto cert_chains_to_root(X509* cert, const std::string& root_pem) -> bool {
    BIO* bio = BIO_new_mem_buf(root_pem.data(), static_cast<int>(root_pem.size()));
    X509* root = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (root == nullptr) {
        return false;
    }

    X509_STORE* store = X509_STORE_new();
    X509_STORE_add_cert(store, root);
    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, store, cert, nullptr);
    int rc = X509_verify_cert(ctx);
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    X509_free(root);
    return rc == 1;
}

// The TLS layer only *requests* a client certificate (Requirement 15.3's
// design: SSL_VERIFY_PEER without SSL_VERIFY_FAIL_IF_NO_PEER_CERT) so that
// bearer-token routes keep accepting connections with no client certificate
// at all. OpenSSL's default verify logic would otherwise abort the handshake
// for ANY presented certificate, since no CA trust store is configured on
// this SSL_CTX — the real chain-to-root check happens at the application
// level in the /v1/certificates/renew handler via cert_chains_to_root().
// This callback simply lets the handshake complete so that check can run.
inline auto accept_any_peer_certificate(int, X509_STORE_CTX*) -> int {
    return 1;
}

#endif  // CPPHTTPLIB_OPENSSL_SUPPORT

}  // namespace raft::testing
