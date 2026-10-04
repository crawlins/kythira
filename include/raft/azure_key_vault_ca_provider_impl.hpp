// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file azure_key_vault_ca_provider_impl.hpp
/// @brief Method definitions for `azure_key_vault_ca_provider`. Included only by
///        consumers that actually construct/call the class (`cmd/ca_service/`,
///        its unit test) — mirroring `aws_acm_pca_provider_impl.hpp`'s split.
///        Compiled only when `KYTHIRA_HAS_AZURE_KEY_VAULT` is defined.
///
/// ## How the externally-signed certificate is assembled
///
/// `certificate_authority_impl.hpp` already implements CSR parsing and
/// TBSCertificate assembly (`detail::build_unsigned_leaf_cert`); this file
/// reuses it directly rather than duplicating it. What it cannot provide is a
/// signature from a key that never leaves Key Vault, so `sign_csr` signs the
/// TBSCertificate itself, the same way `X509_sign()` does internally:
///
/// 1. write the signature AlgorithmIdentifier for `config.signing_algorithm`
///    into both the TBSCertificate and the outer Certificate;
/// 2. DER-encode the TBSCertificate and hash it with the algorithm's digest;
/// 3. ask Key Vault's `Sign` operation to sign that digest;
/// 4. convert the result to X.509's encoding (Key Vault returns ECDSA
///    signatures as fixed-width `r || s`, X.509 wants a DER `ECDSA-Sig-Value`;
///    RSA signatures are used as returned) and store it as the signature;
/// 5. verify the finished certificate against `ca_certificate_pem`'s public
///    key, so a vault key that doesn't belong to the configured CA certificate
///    is an error instead of a certificate nobody can validate.
///
/// Doing the signing step by hand, instead of routing `X509_sign()` through a
/// custom `RSA_METHOD` as this file used to, is what makes RSA-PSS and ECDSA
/// work: OpenSSL computes PSS padding locally and only hands an RSA method the
/// raw private-key operation, which Key Vault does not offer, and every digest
/// but SHA-256 was unreachable through `build_leaf_cert`.

#include <raft/azure_key_vault_ca_provider.hpp>
#include <raft/certificate_authority_impl.hpp>
#include <raft/future_default.hpp>

#ifdef KYTHIRA_HAS_AZURE_KEY_VAULT

#include <azure/keyvault/keys/cryptography/cryptography_client.hpp>

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/x509.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace raft::testing {

namespace azure_detail {

using KeyVaultCryptographyClient =
    Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient;
using KeyVaultSignatureAlgorithm =
    Azure::Security::KeyVault::Keys::Cryptography::SignatureAlgorithm;

[[nodiscard]] inline auto make_cryptography_client(const azure_key_vault_ca_provider_config& config)
    -> KeyVaultCryptographyClient {
    std::string key_id = config.vault_url + "/keys/" + config.key_name;
    if (!config.key_version.empty()) {
        key_id += "/" + config.key_version;
    }
    auto credential = config.azure.credential ? config.azure.credential
                                              : kythira::make_default_credential_chain();
    return KeyVaultCryptographyClient(key_id, credential, config.client_options);
}

[[nodiscard]] inline auto to_key_vault_algorithm(azure_key_vault_signing_algorithm alg)
    -> KeyVaultSignatureAlgorithm {
    switch (alg) {
        case azure_key_vault_signing_algorithm::rs256:
            return KeyVaultSignatureAlgorithm::RS256;
        case azure_key_vault_signing_algorithm::rs384:
            return KeyVaultSignatureAlgorithm::RS384;
        case azure_key_vault_signing_algorithm::rs512:
            return KeyVaultSignatureAlgorithm::RS512;
        case azure_key_vault_signing_algorithm::ps256:
            return KeyVaultSignatureAlgorithm::PS256;
        case azure_key_vault_signing_algorithm::es256:
            return KeyVaultSignatureAlgorithm::ES256;
        case azure_key_vault_signing_algorithm::es384:
            return KeyVaultSignatureAlgorithm::ES384;
    }
    throw std::invalid_argument("azure_key_vault_ca_provider: unknown signing_algorithm");
}

[[nodiscard]] inline auto digest_md(azure_key_vault_signing_algorithm alg) -> const EVP_MD* {
    switch (alg) {
        case azure_key_vault_signing_algorithm::rs256:
        case azure_key_vault_signing_algorithm::ps256:
        case azure_key_vault_signing_algorithm::es256:
            return EVP_sha256();
        case azure_key_vault_signing_algorithm::rs384:
        case azure_key_vault_signing_algorithm::es384:
            return EVP_sha384();
        case azure_key_vault_signing_algorithm::rs512:
            return EVP_sha512();
    }
    return EVP_sha256();
}

/// DER of the RSASSA-PSS-params (RFC 4055) that Key Vault's PS256 produces
/// (RFC 7518 section 3.5): hashAlgorithm SHA-256, maskGenAlgorithm MGF1 with
/// SHA-256, saltLength 32 (the digest length), trailerField default.
inline constexpr std::array<unsigned char, 54> ps256_params_der{
    0x30, 0x34,                                                        // SEQUENCE
    0xa0, 0x0f, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,  // [0] sha256
    0x03, 0x04, 0x02, 0x01, 0x05, 0x00,                                //
    0xa1, 0x1c, 0x30, 0x1a, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7,  // [1] mgf1
    0x0d, 0x01, 0x01, 0x08, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48,  //
    0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00,                    //     (sha256)
    0xa2, 0x03, 0x02, 0x01, 0x20,                                      // [2] salt 32
};

[[nodiscard]] inline auto is_ecdsa(azure_key_vault_signing_algorithm alg) -> bool {
    return alg == azure_key_vault_signing_algorithm::es256 ||
           alg == azure_key_vault_signing_algorithm::es384;
}

/// Writes the X.509 signature AlgorithmIdentifier for `alg` into `out`.
inline void set_signature_algorithm(X509_ALGOR* out, azure_key_vault_signing_algorithm alg) {
    int nid = NID_undef;
    int param_type = V_ASN1_NULL;  // RFC 4055: RSA PKCS#1 v1.5 carries NULL
    void* params = nullptr;
    switch (alg) {
        case azure_key_vault_signing_algorithm::rs256:
            nid = NID_sha256WithRSAEncryption;
            break;
        case azure_key_vault_signing_algorithm::rs384:
            nid = NID_sha384WithRSAEncryption;
            break;
        case azure_key_vault_signing_algorithm::rs512:
            nid = NID_sha512WithRSAEncryption;
            break;
        case azure_key_vault_signing_algorithm::ps256: {
            nid = NID_rsassaPss;
            ASN1_STRING* seq = ASN1_STRING_new();
            if (seq == nullptr || ASN1_STRING_set(seq, ps256_params_der.data(),
                                                  static_cast<int>(ps256_params_der.size())) != 1) {
                ASN1_STRING_free(seq);
                detail::throw_openssl_error("ASN1_STRING_set(RSASSA-PSS-params) failed");
            }
            param_type = V_ASN1_SEQUENCE;
            params = seq;
            break;
        }
        case azure_key_vault_signing_algorithm::es256:
            nid = NID_ecdsa_with_SHA256;
            param_type = V_ASN1_UNDEF;  // RFC 5758: ECDSA omits parameters
            break;
        case azure_key_vault_signing_algorithm::es384:
            nid = NID_ecdsa_with_SHA384;
            param_type = V_ASN1_UNDEF;
            break;
    }
    if (nid == NID_undef) {
        throw std::invalid_argument("azure_key_vault_ca_provider: unknown signing_algorithm");
    }
    if (X509_ALGOR_set0(out, OBJ_nid2obj(nid), param_type, params) != 1) {
        if (param_type == V_ASN1_SEQUENCE) {
            ASN1_STRING_free(static_cast<ASN1_STRING*>(params));
        }
        detail::throw_openssl_error("X509_ALGOR_set0 failed");
    }
}

/// Throws unless the CA certificate's key can produce `alg` signatures: an RSA
/// key for rs*/ps256, a P-256 key for es256, a P-384 key for es384 (Key Vault
/// pairs each ES algorithm with exactly one curve).
inline void check_ca_key_matches(EVP_PKEY* ca_key, azure_key_vault_signing_algorithm alg) {
    const int base_id = EVP_PKEY_get_base_id(ca_key);
    if (!is_ecdsa(alg)) {
        if (base_id != EVP_PKEY_RSA) {
            throw std::invalid_argument(
                "azure_key_vault_ca_provider: signing_algorithm is an RSA algorithm but "
                "ca_certificate_pem's public key is not RSA");
        }
        return;
    }
    const char* want = alg == azure_key_vault_signing_algorithm::es256 ? "prime256v1" : "secp384r1";
    std::array<char, 64> group{};
    std::size_t group_len = 0;
    if (base_id != EVP_PKEY_EC ||
        EVP_PKEY_get_group_name(ca_key, group.data(), group.size(), &group_len) != 1 ||
        std::string(group.data(), group_len) != want) {
        throw std::invalid_argument(std::string("azure_key_vault_ca_provider: signing_algorithm "
                                                "needs a CA certificate with an EC ") +
                                    want + " public key");
    }
}

/// Converts Key Vault's ECDSA signature (JWS encoding: `r || s`, each
/// left-padded to the curve's field size) to X.509's DER `ECDSA-Sig-Value`.
[[nodiscard]] inline auto ecdsa_jws_to_der(const std::vector<std::uint8_t>& raw,
                                           std::size_t field_bytes) -> std::vector<unsigned char> {
    if (raw.size() != 2 * field_bytes) {
        throw std::runtime_error("azure_key_vault_ca_provider: Key Vault returned a " +
                                 std::to_string(raw.size()) + "-byte ECDSA signature, expected " +
                                 std::to_string(2 * field_bytes));
    }
    ECDSA_SIG* sig = ECDSA_SIG_new();
    BIGNUM* r = BN_bin2bn(raw.data(), static_cast<int>(field_bytes), nullptr);
    BIGNUM* s = BN_bin2bn(raw.data() + field_bytes, static_cast<int>(field_bytes), nullptr);
    if (sig == nullptr || r == nullptr || s == nullptr || ECDSA_SIG_set0(sig, r, s) != 1) {
        BN_free(r);
        BN_free(s);
        ECDSA_SIG_free(sig);
        detail::throw_openssl_error("ECDSA_SIG construction failed");
    }
    // ECDSA_SIG_set0 now owns r and s.
    const int len = i2d_ECDSA_SIG(sig, nullptr);
    std::vector<unsigned char> der(len > 0 ? static_cast<std::size_t>(len) : 0);
    unsigned char* p = der.data();
    const bool ok = len > 0 && i2d_ECDSA_SIG(sig, &p) == len;
    ECDSA_SIG_free(sig);
    if (!ok) {
        detail::throw_openssl_error("i2d_ECDSA_SIG failed");
    }
    return der;
}

/// Signs `cert` (fully built, unsigned) with the Key Vault key behind
/// `client`, then checks the result against `ca_key`. See this file's header
/// comment for the steps.
inline void sign_with_key_vault(X509* cert, EVP_PKEY* ca_key, KeyVaultCryptographyClient& client,
                                azure_key_vault_signing_algorithm alg) {
    const ASN1_BIT_STRING* signature_const = nullptr;
    const X509_ALGOR* outer_alg_const = nullptr;
    X509_get0_signature(&signature_const, &outer_alg_const, cert);
    // X509_sign() writes these same two fields of a certificate it owns; OpenSSL
    // has no non-const accessor for them.
    auto* tbs_alg = const_cast<X509_ALGOR*>(X509_get0_tbs_sigalg(cert));
    auto* outer_alg = const_cast<X509_ALGOR*>(outer_alg_const);
    auto* signature = const_cast<ASN1_BIT_STRING*>(signature_const);
    set_signature_algorithm(tbs_alg, alg);
    set_signature_algorithm(outer_alg, alg);

    unsigned char* tbs_der = nullptr;
    const int tbs_len = i2d_re_X509_tbs(cert, &tbs_der);
    if (tbs_len <= 0) {
        detail::throw_openssl_error("i2d_re_X509_tbs failed");
    }
    std::vector<std::uint8_t> digest(EVP_MAX_MD_SIZE);
    unsigned int digest_len = 0;
    const int digest_ok = EVP_Digest(tbs_der, static_cast<std::size_t>(tbs_len), digest.data(),
                                     &digest_len, digest_md(alg), nullptr);
    OPENSSL_free(tbs_der);
    if (digest_ok != 1) {
        detail::throw_openssl_error("EVP_Digest(TBSCertificate) failed");
    }
    digest.resize(digest_len);

    auto raw = client.Sign(to_key_vault_algorithm(alg), digest).Value.Signature;
    std::vector<unsigned char> sig_bytes =
        is_ecdsa(alg)
            ? ecdsa_jws_to_der(raw, static_cast<std::size_t>((EVP_PKEY_get_bits(ca_key) + 7) / 8))
            : std::vector<unsigned char>(raw.begin(), raw.end());
    if (ASN1_BIT_STRING_set(signature, sig_bytes.data(), static_cast<int>(sig_bytes.size())) != 1) {
        detail::throw_openssl_error("ASN1_BIT_STRING_set(signature) failed");
    }
    // Mark the bit string as having zero unused bits. Without this flag OpenSSL
    // derives the unused-bit count by stripping trailing zero octets, which
    // corrupts any signature that happens to end in 0x00 (X509_sign does the
    // same).
    signature->flags &= ~(ASN1_STRING_FLAG_BITS_LEFT | 0x07);
    signature->flags |= ASN1_STRING_FLAG_BITS_LEFT;

    if (X509_verify(cert, ca_key) != 1) {
        ERR_clear_error();
        throw std::runtime_error(
            "the Key Vault signature does not verify against ca_certificate_pem's public key "
            "(is key_name/key_version the key that belongs to that certificate?)");
    }
}

/// Combines a process-wide random-ish seed (captured once) with an atomic
/// counter, the same scheme `certificate_authority::impl::next_serial` uses
/// per-instance — good enough for log/debug correlation; this provider has no
/// revoke()/CRL that depends on serial uniqueness guarantees beyond that.
[[nodiscard]] inline auto next_serial() -> std::uint64_t {
    static const std::uint64_t seed =
        static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()) &
        0xFFFFFFFFULL;
    static std::atomic<std::uint64_t> counter{0};
    return (seed << 32) | (++counter);
}

}  // namespace azure_detail

inline azure_key_vault_ca_provider::azure_key_vault_ca_provider(
    azure_key_vault_ca_provider_config config)
    : _client(azure_detail::make_cryptography_client(config)), _config(std::move(config)) {
    if (_config.vault_url.empty()) {
        throw std::invalid_argument("azure_key_vault_ca_provider: vault_url must be non-empty");
    }
    if (_config.key_name.empty()) {
        throw std::invalid_argument("azure_key_vault_ca_provider: key_name must be non-empty");
    }
    if (_config.ca_certificate_pem.empty()) {
        throw std::invalid_argument(
            "azure_key_vault_ca_provider: ca_certificate_pem must be non-empty");
    }
}

inline auto azure_key_vault_ca_provider::root_certificate_pem()
    -> kythira::future_default<std::string> {
    try {
        fiu_do_on("raft/azure/keyvault/get_key",
                  throw std::runtime_error("fault: raft/azure/keyvault/get_key"););
        return kythira::future_factory_default::makeReadyFuture(
            std::string(_config.ca_certificate_pem));
    } catch (const std::exception& ex) {
        return kythira::future_factory_default::makeExceptionalFuture<std::string>(
            std::make_exception_ptr(std::runtime_error(
                std::string("azure_key_vault_ca_provider::root_certificate_pem: ") + ex.what())));
    }
}

inline auto azure_key_vault_ca_provider::sign_csr(std::string csr_pem, csr_signing_options options)
    -> kythira::future_default<pem_material> {
    try {
        fiu_do_on("raft/azure/keyvault/sign",
                  throw std::runtime_error("fault: raft/azure/keyvault/sign"););

        auto csr_bio = detail::make_bio();
        BIO_write(csr_bio.get(), csr_pem.data(), static_cast<int>(csr_pem.size()));
        detail::x509_req_ptr req{PEM_read_bio_X509_REQ(csr_bio.get(), nullptr, nullptr, nullptr)};
        if (!req) {
            throw std::invalid_argument(
                "azure_key_vault_ca_provider::sign_csr: unparseable CSR PEM");
        }
        detail::evp_pkey_ptr csr_pubkey{X509_REQ_get_pubkey(req.get())};
        if (!csr_pubkey) {
            throw std::invalid_argument(
                "azure_key_vault_ca_provider::sign_csr: CSR has no public key");
        }
        if (X509_REQ_verify(req.get(), csr_pubkey.get()) != 1) {
            throw std::invalid_argument(
                "azure_key_vault_ca_provider::sign_csr: CSR self-signature does not verify");
        }

        auto ca_bio = detail::make_bio();
        BIO_write(ca_bio.get(), _config.ca_certificate_pem.data(),
                  static_cast<int>(_config.ca_certificate_pem.size()));
        detail::x509_ptr issuer_cert{PEM_read_bio_X509(ca_bio.get(), nullptr, nullptr, nullptr)};
        if (!issuer_cert) {
            throw std::invalid_argument(
                "azure_key_vault_ca_provider: unparseable ca_certificate_pem");
        }
        detail::evp_pkey_ptr issuer_pubkey_evp{X509_get_pubkey(issuer_cert.get())};
        if (!issuer_pubkey_evp) {
            throw std::invalid_argument(
                "azure_key_vault_ca_provider: CA certificate has no public key");
        }
        azure_detail::check_ca_key_matches(issuer_pubkey_evp.get(), _config.signing_algorithm);

        auto serial = azure_detail::next_serial();
        auto now = std::chrono::system_clock::now();
        auto cert = detail::build_unsigned_leaf_cert(
            issuer_cert.get(), X509_REQ_get_subject_name(req.get()), csr_pubkey.get(),
            options.dns_names, options.ip_addresses, options.server_auth, options.client_auth, now,
            now + options.validity, serial);
        azure_detail::sign_with_key_vault(cert.get(), issuer_pubkey_evp.get(), _client,
                                          _config.signing_algorithm);

        pem_material out;
        out.certificate_pem = detail::serialize_cert(cert.get());
        out.private_key_pem.clear();
        out.chain_pem = out.certificate_pem + _config.ca_certificate_pem;
        out.serial = serial;
        return kythira::future_factory_default::makeReadyFuture(std::move(out));
    } catch (const std::exception& ex) {
        return kythira::future_factory_default::makeExceptionalFuture<pem_material>(
            std::make_exception_ptr(std::runtime_error(
                std::string("azure_key_vault_ca_provider::sign_csr: ") + ex.what())));
    }
}

}  // namespace raft::testing

#endif  // KYTHIRA_HAS_AZURE_KEY_VAULT
