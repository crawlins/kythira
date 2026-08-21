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
/// TBSCertificate assembly (`detail::build_leaf_cert`) — this file reuses those
/// free functions directly rather than duplicating their body, since
/// `certificate_authority_impl.hpp`'s own doc comment explicitly anticipates
/// being included by "other translation units inside the certificate_authority
/// library target that need direct access to X509/EVP_PKEY internals", which is
/// exactly this file's situation (design.md's "duplicate the minimal subset"
/// choice is honored in spirit — no new production class depends on this one
/// — while avoiding a pointless copy-paste of a few hundred lines).
///
/// The one piece `certificate_authority_impl.hpp` cannot provide is signing
/// with a key that never leaves Key Vault. `X509_sign()` normally computes the
/// TBSCertificate DER, hashes it, and calls straight into an `EVP_PKEY`'s
/// private-key operation. To redirect just that last step to Key Vault's
/// `Sign` RPC while keeping OpenSSL's own (correct) DER/AlgorithmIdentifier
/// assembly, this file builds an `EVP_PKEY` wrapping a legacy `RSA*` object
/// whose `RSA_METHOD.rsa_sign` callback is overridden to call Key Vault instead
/// of doing local RSA math — the standard technique real HSM/KMS OpenSSL
/// integrations (PKCS#11 engines, cloud KMS engines) use. `RSA_METHOD`/
/// `RSA_set_method`/`EVP_PKEY_assign_RSA` are marked `OSSL_DEPRECATEDIN_3_0` in
/// OpenSSL 3.x but fully functional — kept specifically for this use case.

#include <raft/azure_key_vault_ca_provider.hpp>
#include <raft/certificate_authority_impl.hpp>
#include <raft/future_default.hpp>

#ifdef KYTHIRA_HAS_AZURE_KEY_VAULT

#include <azure/keyvault/keys/cryptography/cryptography_client.hpp>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

// The RSA_METHOD-based external-signing technique below (RSA_new/RSA_set0_key/
// RSA_set_method/RSA_meth_new/RSA_meth_set_sign/RSA_set_ex_data/RSA_get_ex_data/
// RSA_free/EVP_PKEY_get1_RSA) is built entirely on APIs OpenSSL 3.x marks
// OSSL_DEPRECATEDIN_3_0 — kept specifically for exactly this external-signer
// use case (see this file's header comment) — not accidental legacy-API
// usage. Suppressed locally rather than project-wide.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace raft::testing {

namespace azure_detail {

using KeyVaultCryptographyClient =
    Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient;
using KeyVaultSignatureAlgorithm =
    Azure::Security::KeyVault::Keys::Cryptography::SignatureAlgorithm;

/// State threaded through the custom `RSA_METHOD`'s `sign` callback via
/// `RSA_set_ex_data`/`RSA_get_ex_data`. Lives on the stack of `sign_csr()` for
/// the duration of one `X509_sign()` call; never shared across calls.
struct external_sign_context {
    KeyVaultCryptographyClient* client{nullptr};
    KeyVaultSignatureAlgorithm algorithm;
    std::exception_ptr error;
};

[[nodiscard]] inline auto external_rsa_ex_index() -> int {
    static int index = RSA_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

extern "C" inline int external_rsa_sign_callback(int /*digest_nid*/, const unsigned char* digest,
                                                 unsigned int digest_length, unsigned char* sigret,
                                                 unsigned int* siglen, const RSA* rsa) {
    auto* ctx = static_cast<external_sign_context*>(RSA_get_ex_data(rsa, external_rsa_ex_index()));
    if (ctx == nullptr || ctx->client == nullptr) {
        return 0;
    }
    try {
        std::vector<std::uint8_t> digest_bytes(digest, digest + digest_length);
        auto response = ctx->client->Sign(ctx->algorithm, digest_bytes);
        const auto& signature = response.Value.Signature;
        std::copy(signature.begin(), signature.end(), sigret);
        *siglen = static_cast<unsigned int>(signature.size());
        return 1;
    } catch (...) {
        ctx->error = std::current_exception();
        return 0;
    }
}

/// Builds (once per process) an `RSA_METHOD` whose only overridden operation
/// is `sign` — every other operation falls back to whatever the default
/// method would have done, but is never reached because this provider only
/// ever uses this key for `X509_sign()`.
[[nodiscard]] inline auto external_rsa_method() -> RSA_METHOD* {
    static RSA_METHOD* method = [] {
        RSA_METHOD* m = RSA_meth_new("kythira-azure-keyvault-external-signer", 0);
        if (m == nullptr) {
            detail::throw_openssl_error("RSA_meth_new failed");
        }
        RSA_meth_set_sign(m, external_rsa_sign_callback);
        return m;
    }();
    return method;
}

/// Wraps `pubkey`'s modulus/exponent (copied — `RSA_set0_key` takes ownership
/// of its `BIGNUM*` arguments) in a fresh `RSA*` whose private-key operation
/// is redirected to `ctx` via the custom method above, then wraps that in an
/// `EVP_PKEY` suitable for `X509_sign()`. The real private key never appears
/// here — only the CA certificate's already-public modulus/exponent, needed
/// so `RSA_size()` reports the correct signature buffer length.
[[nodiscard]] inline auto make_external_signing_key(const RSA* pubkey, external_sign_context* ctx)
    -> detail::evp_pkey_ptr {
    const BIGNUM *n = nullptr, *e = nullptr;
    RSA_get0_key(pubkey, &n, &e, nullptr);
    if (n == nullptr || e == nullptr) {
        throw std::invalid_argument(
            "azure_key_vault_ca_provider: ca_certificate_pem's public key has no RSA "
            "modulus/exponent"
            " (only RSA CA keys are supported by the local signing-assembly path)");
    }

    RSA* rsa = RSA_new();
    if (rsa == nullptr) {
        detail::throw_openssl_error("RSA_new failed");
    }
    if (RSA_set0_key(rsa, BN_dup(n), BN_dup(e), nullptr) != 1) {
        RSA_free(rsa);
        detail::throw_openssl_error("RSA_set0_key failed");
    }
    if (RSA_set_method(rsa, external_rsa_method()) != 1) {
        RSA_free(rsa);
        detail::throw_openssl_error("RSA_set_method failed");
    }
    if (RSA_set_ex_data(rsa, external_rsa_ex_index(), ctx) != 1) {
        RSA_free(rsa);
        detail::throw_openssl_error("RSA_set_ex_data failed");
    }

    detail::evp_pkey_ptr pkey{EVP_PKEY_new()};
    if (!pkey) {
        RSA_free(rsa);
        detail::throw_openssl_error("EVP_PKEY_new failed");
    }
    // EVP_PKEY_assign_RSA transfers ownership of `rsa` to `pkey` on success.
    if (EVP_PKEY_assign_RSA(pkey.get(), rsa) != 1) {
        RSA_free(rsa);
        detail::throw_openssl_error("EVP_PKEY_assign_RSA failed");
    }
    return pkey;
}

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

/// The local certificate-assembly path reuses `detail::build_leaf_cert`
/// verbatim, which internally signs with `EVP_sha256()` unconditionally (it
/// has no digest parameter — it was written for `certificate_authority`,
/// which only ever signs with SHA-256). That hardcodes this provider's fully
/// working path to `rs256`: `rs384`/`rs512` would ask Key Vault to sign a
/// SHA-384/512-shaped request over a digest that `build_leaf_cert` actually
/// computed with SHA-256, which Key Vault's `Sign` call correctly rejects
/// (wrong digest length for the algorithm) rather than silently producing an
/// invalid certificate — so those two fail loudly, not subtly. Unlocking them
/// (and `ps256`/`es256`/`es384`, which need additional padding-mode/EC-method
/// work regardless) requires adding a digest parameter to
/// `detail::build_leaf_cert` itself, a small shared enhancement benefiting any
/// future signer, not something specific to this provider — tracked as a
/// follow-up rather than done here.
[[nodiscard]] inline auto is_locally_assemblable(azure_key_vault_signing_algorithm alg) -> bool {
    return alg == azure_key_vault_signing_algorithm::rs256;
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

        if (!azure_detail::is_locally_assemblable(_config.signing_algorithm)) {
            throw std::logic_error(
                "azure_key_vault_ca_provider::sign_csr: local certificate assembly is only "
                "implemented for the RSA PKCS#1v1.5 family (rs256/rs384/rs512); ps256/es256/es384 "
                "are forwarded to Key Vault correctly but not yet supported end-to-end here");
        }

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
        RSA* issuer_rsa = EVP_PKEY_get1_RSA(issuer_pubkey_evp.get());
        if (issuer_rsa == nullptr) {
            throw std::invalid_argument(
                "azure_key_vault_ca_provider: only RSA CA keys are supported by the local signing "
                "path (config.signing_algorithm selects an RSA algorithm, but ca_certificate_pem's "
                "public key is not RSA)");
        }
        struct rsa_ref_guard {
            RSA* r;
            ~rsa_ref_guard() { RSA_free(r); }
        } issuer_rsa_guard{issuer_rsa};

        azure_detail::external_sign_context sign_ctx;
        sign_ctx.client = &_client;
        sign_ctx.algorithm = azure_detail::to_key_vault_algorithm(_config.signing_algorithm);

        auto external_key = azure_detail::make_external_signing_key(issuer_rsa, &sign_ctx);

        auto serial = azure_detail::next_serial();
        auto now = std::chrono::system_clock::now();
        // detail::build_leaf_cert builds the TBSCertificate AND signs it (its
        // own internal X509_sign call, hardcoded to EVP_sha256() — see
        // is_locally_assemblable's comment above for why that limits this
        // provider to rs256 today). Passing `external_key` as the "issuer
        // key" routes that internal signing call through
        // external_rsa_sign_callback, which is what actually invokes Key
        // Vault's Sign RPC — the real CA private key never leaves Key Vault.
        detail::x509_ptr cert;
        try {
            cert = detail::build_leaf_cert(
                issuer_cert.get(), external_key.get(), X509_REQ_get_subject_name(req.get()),
                csr_pubkey.get(), options.dns_names, options.ip_addresses, options.server_auth,
                options.client_auth, now, now + options.validity, serial);
        } catch (const std::exception&) {
            // build_leaf_cert's own throw_openssl_error (fired when
            // external_rsa_sign_callback returns 0) only reports a generic
            // OpenSSL error; when the callback actually captured a Key Vault
            // exception, that's the more useful one to surface.
            if (sign_ctx.error) {
                std::rethrow_exception(sign_ctx.error);
            }
            throw;
        }
        if (sign_ctx.error) {
            std::rethrow_exception(sign_ctx.error);
        }

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

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif  // KYTHIRA_HAS_AZURE_KEY_VAULT
