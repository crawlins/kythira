#pragma once

/// @file certificate_authority_impl.hpp
/// @brief OpenSSL-backed implementation details for `certificate_authority`.
///
/// Included only by `src/certificate_authority.cpp` (and other translation units
/// inside the `certificate_authority` library target that need direct access to
/// X509/EVP_PKEY internals, e.g. CSR signing). Never included transitively by
/// `certificate_authority.hpp`, so consumers of the public API never need OpenSSL
/// headers. Compiled only when `KYTHIRA_HAS_OPENSSL` is defined (the owning CMake
/// target is only built `if(TARGET OpenSSL::SSL)`).

#include <raft/certificate_authority.hpp>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace raft::testing::detail {

// ---------------------------------------------------------------------------
// RAII wrappers around OpenSSL's opaque C types.
// ---------------------------------------------------------------------------

template<typename T, void (*Deleter)(T*)> struct openssl_deleter {
    void operator()(T* p) const noexcept {
        if (p != nullptr) {
            Deleter(p);
        }
    }
};

using evp_pkey_ptr = std::unique_ptr<EVP_PKEY, openssl_deleter<EVP_PKEY, EVP_PKEY_free>>;
using evp_pkey_ctx_ptr =
    std::unique_ptr<EVP_PKEY_CTX, openssl_deleter<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using x509_ptr = std::unique_ptr<X509, openssl_deleter<X509, X509_free>>;
using x509_req_ptr = std::unique_ptr<X509_REQ, openssl_deleter<X509_REQ, X509_REQ_free>>;
using x509_crl_ptr = std::unique_ptr<X509_CRL, openssl_deleter<X509_CRL, X509_CRL_free>>;
using x509_name_ptr = std::unique_ptr<X509_NAME, openssl_deleter<X509_NAME, X509_NAME_free>>;
using bio_ptr = std::unique_ptr<BIO, openssl_deleter<BIO, BIO_free_all>>;
using x509_extension_ptr =
    std::unique_ptr<X509_EXTENSION, openssl_deleter<X509_EXTENSION, X509_EXTENSION_free>>;

/// Drains and formats the OpenSSL error queue; used to build exception messages.
[[nodiscard]] inline auto drain_openssl_errors() -> std::string {
    std::ostringstream out;
    unsigned long code = 0;  // NOLINT(google-runtime-int) — matches OpenSSL's own typedef
    bool first = true;
    while ((code = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(code, buf, sizeof(buf));
        if (!first) {
            out << "; ";
        }
        out << buf;
        first = false;
    }
    return out.str();
}

[[noreturn]] inline void throw_openssl_error(const std::string& context) {
    auto details = drain_openssl_errors();
    throw std::runtime_error(details.empty() ? context : context + ": " + details);
}

inline auto make_bio() -> bio_ptr {
    auto* bio = BIO_new(BIO_s_mem());
    if (bio == nullptr) {
        throw_openssl_error("BIO_new failed");
    }
    return bio_ptr{bio};
}

inline auto bio_to_string(BIO* bio) -> std::string {
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio, &mem);
    return std::string(mem->data, mem->length);
}

[[nodiscard]] inline auto serialize_cert(X509* cert) -> std::string {
    auto bio = make_bio();
    if (PEM_write_bio_X509(bio.get(), cert) != 1) {
        throw_openssl_error("PEM_write_bio_X509 failed");
    }
    return bio_to_string(bio.get());
}

[[nodiscard]] inline auto serialize_key(EVP_PKEY* key) -> std::string {
    auto bio = make_bio();
    if (PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        throw_openssl_error("PEM_write_bio_PrivateKey failed");
    }
    return bio_to_string(bio.get());
}

[[nodiscard]] inline auto serialize_crl(X509_CRL* crl) -> std::string {
    auto bio = make_bio();
    if (PEM_write_bio_X509_CRL(bio.get(), crl) != 1) {
        throw_openssl_error("PEM_write_bio_X509_CRL failed");
    }
    return bio_to_string(bio.get());
}

// ---------------------------------------------------------------------------
// Key generation
// ---------------------------------------------------------------------------

[[nodiscard]] inline auto generate_rsa_key(int bits) -> evp_pkey_ptr {
    evp_pkey_ctx_ptr ctx{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr)};
    if (!ctx) {
        throw_openssl_error("EVP_PKEY_CTX_new_id(RSA) failed");
    }
    if (EVP_PKEY_keygen_init(ctx.get()) != 1) {
        throw_openssl_error("EVP_PKEY_keygen_init(RSA) failed");
    }
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), bits) != 1) {
        throw_openssl_error("EVP_PKEY_CTX_set_rsa_keygen_bits failed");
    }
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &raw) != 1) {
        throw_openssl_error("EVP_PKEY_keygen(RSA) failed");
    }
    return evp_pkey_ptr{raw};
}

[[nodiscard]] inline auto generate_ec_key(int nid) -> evp_pkey_ptr {
    evp_pkey_ctx_ptr ctx{EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr)};
    if (!ctx) {
        throw_openssl_error("EVP_PKEY_CTX_new_id(EC) failed");
    }
    if (EVP_PKEY_keygen_init(ctx.get()) != 1) {
        throw_openssl_error("EVP_PKEY_keygen_init(EC) failed");
    }
    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), nid) != 1) {
        throw_openssl_error("EVP_PKEY_CTX_set_ec_paramgen_curve_nid failed");
    }
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &raw) != 1) {
        throw_openssl_error("EVP_PKEY_keygen(EC) failed");
    }
    return evp_pkey_ptr{raw};
}

[[nodiscard]] inline auto generate_key(key_algorithm algorithm) -> evp_pkey_ptr {
    switch (algorithm) {
        case key_algorithm::rsa_2048:
            return generate_rsa_key(2048);
        case key_algorithm::rsa_4096:
            return generate_rsa_key(4096);
        case key_algorithm::ecdsa_p256:
            return generate_ec_key(NID_X9_62_prime256v1);
        case key_algorithm::ecdsa_p384:
            return generate_ec_key(NID_secp384r1);
    }
    throw std::invalid_argument("generate_key: unknown key_algorithm");
}

// ---------------------------------------------------------------------------
// Name / extension helpers
// ---------------------------------------------------------------------------

inline void set_name(X509_NAME* name, const distinguished_name& dn) {
    auto add = [&](const char* field, const std::string& value) {
        if (value.empty()) {
            return;
        }
        if (X509_NAME_add_entry_by_txt(name, field, MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>(value.c_str()), -1,
                                       -1, 0) != 1) {
            throw_openssl_error(std::string("X509_NAME_add_entry_by_txt(") + field + ") failed");
        }
    };
    add("C", dn.country);
    add("O", dn.organization);
    add("CN", dn.common_name);
}

/// Builds a standalone (not-yet-attached-to-a-certificate) X509_NAME from a
/// distinguished_name, so callers that need to hand a subject name to something
/// other than a fresh X509 (e.g. `X509_REQ_set_subject_name`) don't need one.
[[nodiscard]] inline auto build_name(const distinguished_name& dn) -> x509_name_ptr {
    x509_name_ptr name{X509_NAME_new()};
    if (!name) {
        throw_openssl_error("X509_NAME_new failed");
    }
    set_name(name.get(), dn);
    return name;
}

inline void set_validity_window(X509* cert, std::chrono::system_clock::time_point not_before,
                                std::chrono::system_clock::time_point not_after) {
    auto to_time_t = [](std::chrono::system_clock::time_point tp) {
        return std::chrono::system_clock::to_time_t(tp);
    };
    time_t nb = to_time_t(not_before);
    time_t na = to_time_t(not_after);
    if (X509_time_adj(X509_getm_notBefore(cert), 0, &nb) == nullptr) {
        throw_openssl_error("X509_time_adj(notBefore) failed");
    }
    if (X509_time_adj(X509_getm_notAfter(cert), 0, &na) == nullptr) {
        throw_openssl_error("X509_time_adj(notAfter) failed");
    }
}

inline void add_extension(X509* cert, X509V3_CTX* ctx, int nid, const std::string& value) {
    X509_EXTENSION* raw = X509V3_EXT_conf_nid(nullptr, ctx, nid, value.c_str());
    if (raw == nullptr) {
        throw_openssl_error("X509V3_EXT_conf_nid failed for extension " + std::to_string(nid));
    }
    x509_extension_ptr ext{raw};
    if (X509_add_ext(cert, ext.get(), -1) != 1) {
        throw_openssl_error("X509_add_ext failed");
    }
}

[[nodiscard]] inline auto build_san_value(const std::vector<std::string>& dns_names,
                                          const std::vector<std::string>& ip_addresses)
    -> std::string {
    std::ostringstream out;
    bool first = true;
    auto append = [&](const char* prefix, const std::string& v) {
        if (!first) {
            out << ',';
        }
        out << prefix << v;
        first = false;
    };
    for (const auto& d : dns_names) {
        append("DNS:", d);
    }
    for (const auto& ip : ip_addresses) {
        append("IP:", ip);
    }
    return out.str();
}

/// Builds an unsigned leaf X509 (subject, validity, basicConstraints=CA:FALSE,
/// keyUsage/EKU per server_auth/client_auth, subjectAltName, public key) whose
/// issuer is `issuer_cert`. Shared by `issue_with_window()` (fresh key, subject
/// built from `leaf_certificate_options::subject`) and `sign_csr()` (key and
/// subject both extracted from a caller-supplied CSR) so the two entry points
/// never duplicate extension-setting logic. `subject_name` is copied into the
/// new certificate by `X509_set_subject_name`, so it may be a borrowed pointer
/// (e.g. `X509_REQ_get_subject_name()`) or a standalone one built via
/// `build_name()` — ownership after the call is unaffected either way.
[[nodiscard]] inline auto build_leaf_cert(
    X509* issuer_cert, EVP_PKEY* issuer_key, X509_NAME* subject_name, EVP_PKEY* pubkey,
    const std::vector<std::string>& dns_names, const std::vector<std::string>& ip_addresses,
    bool server_auth, bool client_auth, std::chrono::system_clock::time_point not_before,
    std::chrono::system_clock::time_point not_after, std::uint64_t serial) -> x509_ptr {
    if (dns_names.empty() && ip_addresses.empty()) {
        throw std::invalid_argument(
            "certificate_authority: leaf certificate requires at least one DNS or IP SAN entry");
    }

    x509_ptr cert{X509_new()};
    if (!cert) {
        throw_openssl_error("X509_new failed");
    }
    if (X509_set_version(cert.get(), 2) != 1) {
        throw_openssl_error("X509_set_version failed");  // v3
    }

    ASN1_INTEGER* serial_asn1 = X509_get_serialNumber(cert.get());
    if (ASN1_INTEGER_set_uint64(serial_asn1, serial) != 1) {
        throw_openssl_error("ASN1_INTEGER_set_uint64 failed");
    }

    set_validity_window(cert.get(), not_before, not_after);

    if (X509_set_subject_name(cert.get(), subject_name) != 1) {
        throw_openssl_error("X509_set_subject_name failed");
    }
    if (X509_set_issuer_name(cert.get(), X509_get_subject_name(issuer_cert)) != 1) {
        throw_openssl_error("X509_set_issuer_name failed");
    }

    if (X509_set_pubkey(cert.get(), pubkey) != 1) {
        throw_openssl_error("X509_set_pubkey failed");
    }

    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer_cert, cert.get(), nullptr, nullptr, 0);

    add_extension(cert.get(), &ctx, NID_basic_constraints, "critical,CA:FALSE");

    std::string key_usage = "critical,";
    std::string eku;
    if (server_auth) {
        key_usage += "digitalSignature,keyEncipherment,";
        eku += "serverAuth,";
    }
    if (client_auth) {
        if (!server_auth) {
            key_usage += "digitalSignature,";
        }
        eku += "clientAuth,";
    }
    if (!key_usage.empty() && key_usage.back() == ',') {
        key_usage.pop_back();
    }
    if (!eku.empty() && eku.back() == ',') {
        eku.pop_back();
    }
    add_extension(cert.get(), &ctx, NID_key_usage, key_usage);
    if (!eku.empty()) {
        add_extension(cert.get(), &ctx, NID_ext_key_usage, eku);
    }

    add_extension(cert.get(), &ctx, NID_subject_key_identifier, "hash");
    add_extension(cert.get(), &ctx, NID_subject_alt_name, build_san_value(dns_names, ip_addresses));

    if (X509_sign(cert.get(), issuer_key, EVP_sha256()) == 0) {
        throw_openssl_error("X509_sign(leaf) failed");
    }
    return cert;
}

}  // namespace raft::testing::detail

namespace raft::testing {

/// Private implementation state for `certificate_authority`. Guards all
/// non-reentrant OpenSSL sequences with `_mutex` so construction and issuance are
/// safe to call concurrently on one instance.
struct certificate_authority::impl {
    detail::evp_pkey_ptr ca_key;
    detail::x509_ptr ca_cert;
    std::string root_pem;

    mutable std::mutex mutex;
    std::uint64_t instance_seed{0};
    std::uint64_t serial_counter{0};

    std::set<std::uint64_t> issued_serials;
    std::vector<std::pair<std::uint64_t, std::time_t>> revoked;

    explicit impl(const ca_options& options) {
        instance_seed = (static_cast<std::uint64_t>(
                             std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
                         reinterpret_cast<std::uintptr_t>(this)) &
                        0xFFFFFFFFULL;

        ca_key = detail::generate_key(options.algorithm);

        ca_cert.reset(X509_new());
        if (!ca_cert) {
            detail::throw_openssl_error("X509_new(root) failed");
        }
        if (X509_set_version(ca_cert.get(), 2) != 1) {
            detail::throw_openssl_error("X509_set_version(root) failed");
        }

        ASN1_INTEGER* serial_asn1 = X509_get_serialNumber(ca_cert.get());
        if (ASN1_INTEGER_set_uint64(serial_asn1, next_serial()) != 1) {
            detail::throw_openssl_error("ASN1_INTEGER_set_uint64(root) failed");
        }

        auto now = std::chrono::system_clock::now();
        detail::set_validity_window(ca_cert.get(), now, now + options.validity);

        X509_NAME* name = X509_get_subject_name(ca_cert.get());
        detail::set_name(name, options.subject);
        if (X509_set_issuer_name(ca_cert.get(), name) != 1) {
            detail::throw_openssl_error("X509_set_issuer_name(root) failed");
        }
        if (X509_set_pubkey(ca_cert.get(), ca_key.get()) != 1) {
            detail::throw_openssl_error("X509_set_pubkey(root) failed");
        }

        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, ca_cert.get(), ca_cert.get(), nullptr, nullptr, 0);
        detail::add_extension(ca_cert.get(), &ctx, NID_basic_constraints, "critical,CA:TRUE");
        detail::add_extension(ca_cert.get(), &ctx, NID_key_usage, "critical,keyCertSign,cRLSign");
        detail::add_extension(ca_cert.get(), &ctx, NID_subject_key_identifier, "hash");

        if (X509_sign(ca_cert.get(), ca_key.get(), EVP_sha256()) == 0) {
            detail::throw_openssl_error("X509_sign(root) failed");
        }

        root_pem = detail::serialize_cert(ca_cert.get());
    }

    /// Builds around already-existing root CA material instead of generating a
    /// fresh root (`certificate_authority::from_existing`, Requirement 17.9).
    impl(std::string ca_cert_pem, std::string ca_key_pem) {
        instance_seed = (static_cast<std::uint64_t>(
                             std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
                         reinterpret_cast<std::uintptr_t>(this)) &
                        0xFFFFFFFFULL;

        auto cert_bio = detail::make_bio();
        BIO_write(cert_bio.get(), ca_cert_pem.data(), static_cast<int>(ca_cert_pem.size()));
        ca_cert.reset(PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr));
        if (!ca_cert) {
            throw std::invalid_argument(
                "certificate_authority::from_existing: unparseable CA certificate PEM");
        }

        auto key_bio = detail::make_bio();
        BIO_write(key_bio.get(), ca_key_pem.data(), static_cast<int>(ca_key_pem.size()));
        ca_key.reset(PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr));
        if (!ca_key) {
            throw std::invalid_argument(
                "certificate_authority::from_existing: unparseable CA private key PEM");
        }

        if (X509_check_private_key(ca_cert.get(), ca_key.get()) != 1) {
            throw std::invalid_argument(
                "certificate_authority::from_existing: private key does not match certificate");
        }

        root_pem = detail::serialize_cert(ca_cert.get());
    }

    // `ctest -jN` runs many test binaries — and multiple `certificate_authority`
    // instances within one binary — concurrently; combining a high-resolution
    // clock reading with `this`'s address keeps serials unique across instances
    // without any shared/global state.
    [[nodiscard]] auto next_serial() -> std::uint64_t {
        return (instance_seed << 32) | (++serial_counter);
    }

    [[nodiscard]] auto issue_with_window(leaf_certificate_options options,
                                         std::chrono::system_clock::time_point not_before,
                                         std::chrono::system_clock::time_point not_after)
        -> pem_material {
        std::lock_guard<std::mutex> lock(mutex);

        auto leaf_key = detail::generate_key(options.algorithm);
        auto serial = next_serial();
        auto subject_name = detail::build_name(options.subject);
        auto cert =
            detail::build_leaf_cert(ca_cert.get(), ca_key.get(), subject_name.get(), leaf_key.get(),
                                    options.dns_names, options.ip_addresses, options.server_auth,
                                    options.client_auth, not_before, not_after, serial);

        pem_material out;
        out.certificate_pem = detail::serialize_cert(cert.get());
        out.private_key_pem = detail::serialize_key(leaf_key.get());
        out.chain_pem = out.certificate_pem + root_pem;
        out.serial = serial;

        issued_serials.insert(serial);
        return out;
    }

    auto revoke(const pem_material& cert) -> void {
        std::lock_guard<std::mutex> lock(mutex);
        if (issued_serials.find(cert.serial) == issued_serials.end()) {
            throw std::invalid_argument("certificate_authority::revoke: serial " +
                                        std::to_string(cert.serial) +
                                        " was not issued by this instance");
        }
        revoked.emplace_back(cert.serial, std::time(nullptr));
    }

    // Records `serial` as revoked without requiring that this instance issued
    // it — for callers (ca_cluster_node) replaying revocation history from an
    // externally-persisted/replicated ledger into a signer reconstructed via
    // from_existing(), which has no `issued_serials` record of certificates
    // issued by a different (e.g. prior-term-leader) instance. Idempotent: a
    // serial already present in `revoked` is not duplicated.
    auto mark_revoked_externally(std::uint64_t serial,
                                 std::chrono::system_clock::time_point revoked_at) -> void {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [existing_serial, _] : revoked) {
            if (existing_serial == serial) {
                return;
            }
        }
        revoked.emplace_back(serial, std::chrono::system_clock::to_time_t(revoked_at));
    }

    [[nodiscard]] auto sign_csr(const std::string& csr_pem, const csr_signing_options& options)
        -> pem_material {
        auto bio = detail::make_bio();
        BIO_write(bio.get(), csr_pem.data(), static_cast<int>(csr_pem.size()));
        detail::x509_req_ptr req{PEM_read_bio_X509_REQ(bio.get(), nullptr, nullptr, nullptr)};
        if (!req) {
            throw std::invalid_argument("certificate_authority::sign_csr: unparseable CSR PEM");
        }

        detail::evp_pkey_ptr pubkey{X509_REQ_get_pubkey(req.get())};
        if (!pubkey) {
            throw std::invalid_argument("certificate_authority::sign_csr: CSR has no public key");
        }
        if (X509_REQ_verify(req.get(), pubkey.get()) != 1) {
            throw std::invalid_argument(
                "certificate_authority::sign_csr: CSR self-signature does not verify");
        }

        std::lock_guard<std::mutex> lock(mutex);

        auto serial = next_serial();
        auto now = std::chrono::system_clock::now();
        auto cert = detail::build_leaf_cert(
            ca_cert.get(), ca_key.get(), X509_REQ_get_subject_name(req.get()), pubkey.get(),
            options.dns_names, options.ip_addresses, options.server_auth, options.client_auth, now,
            now + options.validity, serial);

        pem_material out;
        out.certificate_pem = detail::serialize_cert(cert.get());
        out.chain_pem = out.certificate_pem + root_pem;
        out.serial = serial;

        issued_serials.insert(serial);
        return out;
    }

    [[nodiscard]] auto crl_pem() const -> std::string {
        std::lock_guard<std::mutex> lock(mutex);

        detail::x509_crl_ptr crl{X509_CRL_new()};
        if (!crl) {
            detail::throw_openssl_error("X509_CRL_new failed");
        }
        if (X509_CRL_set_version(crl.get(), 1) != 1) {
            detail::throw_openssl_error("X509_CRL_set_version failed");
        }
        if (X509_CRL_set_issuer_name(crl.get(), X509_get_subject_name(ca_cert.get())) != 1) {
            detail::throw_openssl_error("X509_CRL_set_issuer_name failed");
        }

        for (const auto& [serial, revoked_at] : revoked) {
            X509_REVOKED* rev = X509_REVOKED_new();
            if (rev == nullptr) {
                detail::throw_openssl_error("X509_REVOKED_new failed");
            }
            ASN1_INTEGER* serial_asn1 = ASN1_INTEGER_new();
            ASN1_INTEGER_set_uint64(serial_asn1, serial);
            X509_REVOKED_set_serialNumber(rev, serial_asn1);
            ASN1_INTEGER_free(serial_asn1);
            ASN1_TIME* rev_date = ASN1_TIME_set(nullptr, revoked_at);
            X509_REVOKED_set_revocationDate(rev, rev_date);
            ASN1_TIME_free(rev_date);
            // X509_CRL_add0_revoked takes ownership of `rev`.
            X509_CRL_add0_revoked(crl.get(), rev);
        }

        time_t now = std::time(nullptr);
        time_t next = now + std::chrono::seconds(std::chrono::hours(24)).count();
        ASN1_TIME* last_update = ASN1_TIME_set(nullptr, now);
        ASN1_TIME* next_update = ASN1_TIME_set(nullptr, next);
        X509_CRL_set1_lastUpdate(crl.get(), last_update);
        X509_CRL_set1_nextUpdate(crl.get(), next_update);
        ASN1_TIME_free(last_update);
        ASN1_TIME_free(next_update);

        X509_CRL_sort(crl.get());
        if (X509_CRL_sign(crl.get(), ca_key.get(), EVP_sha256()) == 0) {
            detail::throw_openssl_error("X509_CRL_sign failed");
        }

        return detail::serialize_crl(crl.get());
    }
};

}  // namespace raft::testing
