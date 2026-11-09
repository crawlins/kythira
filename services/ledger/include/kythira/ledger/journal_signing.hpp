// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// CA-signed digests (.kiro/specs/verifiable-ledger/ task 10,
// Requirement 4.5): a node holding a key issued by the Kythira CA signs each
// digest it hands out, so a third party holding only the CA certificate can
// check that a digest came from this cluster, not merely that a journal is
// self-consistent.
//
// A signed digest is the saved digest plus who signed it:
//
//   {ledger, tip, digest, signedAt, signature, certificateChain}
//
// and is still a saved_digest to anything that reads only {tip, digest}.
// The signature covers the Ion hash of {ledger, tip, digest, signedAt}, so
// it does not depend on how the document was encoded on the way (JSON, Ion
// text, Ion binary), and binds the ledger name so a digest of one ledger
// cannot be passed off as another's. It is made with the certificate's key:
// ECDSA or RSA over SHA-256, or Ed25519 directly.
//
// check_signed_digest() verifies the chain against a CA bundle at signedAt
// (a digest signed while the certificate was valid stays verifiable after
// it expires), then the signature with the leaf's key, and returns the
// leaf's URI SANs so the caller can say which node signed.

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/journal_verifier.hpp>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::ledger {

class digest_signing_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct signed_digest {
    std::string ledger;
    saved_digest digest;
    std::int64_t signed_at = 0;  // ms since the epoch, UTC
    std::vector<std::uint8_t> signature;
    std::string certificate_chain;  // PEM, leaf first

    /// What the signature covers.
    [[nodiscard]] auto payload_hash() const -> hash256 {
        ion_value v = ion_value::make_struct();
        v.set("ledger", ion_value::string(ledger))
            .set("tip", ion_value::integer(static_cast<std::int64_t>(digest.tip)))
            .set("digest", hash_value(digest.digest))
            .set("signedAt", timestamp_value(signed_at));
        return v.hash();
    }

    [[nodiscard]] auto to_ion() const -> ion_value {
        ion_value v = digest.to_ion();
        v.set("ledger", ion_value::string(ledger))
            .set("signedAt", timestamp_value(signed_at))
            .set("signature", ion_value::blob(signature))
            .set("certificateChain", ion_value::string(certificate_chain));
        return v;
    }

    /// True when `v` carries a signature at all.
    static auto is_signed(const ion_value& v) -> bool { return v.find("signature") != nullptr; }

    static auto from_ion(const ion_value& v) -> signed_digest {
        namespace jd = journal_detail;
        signed_digest out;
        out.digest = saved_digest::from_ion(v);
        out.ledger = jd::text_field(v, "ledger");
        out.signed_at = jd::time_field(v, "signedAt");
        auto sig = jd::required(v, "signature").as_bytes();
        if (!sig || sig->empty()) {
            throw ledger_format_error("signature is not a blob");
        }
        out.signature = std::move(*sig);
        out.certificate_chain = jd::text_field(v, "certificateChain");
        return out;
    }
};

namespace signing_detail {

struct bio_free {
    auto operator()(BIO* b) const -> void { BIO_free(b); }
};
struct x509_free {
    auto operator()(X509* x) const -> void { X509_free(x); }
};
struct pkey_free {
    auto operator()(EVP_PKEY* k) const -> void { EVP_PKEY_free(k); }
};
struct md_ctx_free {
    auto operator()(EVP_MD_CTX* c) const -> void { EVP_MD_CTX_free(c); }
};
struct store_free {
    auto operator()(X509_STORE* s) const -> void { X509_STORE_free(s); }
};
struct store_ctx_free {
    auto operator()(X509_STORE_CTX* c) const -> void { X509_STORE_CTX_free(c); }
};
struct stack_free {
    auto operator()(STACK_OF(X509) * s) const -> void { sk_X509_pop_free(s, X509_free); }
};
struct names_free {
    auto operator()(GENERAL_NAMES* n) const -> void { GENERAL_NAMES_free(n); }
};

using x509_ptr = std::unique_ptr<X509, x509_free>;
using pkey_ptr = std::unique_ptr<EVP_PKEY, pkey_free>;

inline auto memory_bio(std::string_view pem) -> std::unique_ptr<BIO, bio_free> {
    std::unique_ptr<BIO, bio_free> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        throw digest_signing_error("out of memory");
    }
    return bio;
}

/// Every certificate in a PEM bundle, in order.
inline auto read_certificates(std::string_view pem) -> std::vector<x509_ptr> {
    auto bio = memory_bio(pem);
    std::vector<x509_ptr> out;
    for (;;) {
        x509_ptr cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
        if (!cert) {
            break;
        }
        out.push_back(std::move(cert));
    }
    ERR_clear_error();  // the read past the last certificate
    return out;
}

/// SHA-256 for ECDSA and RSA; none for Ed25519 and Ed448, which hash
/// internally.
inline auto digest_for(EVP_PKEY* key) -> const EVP_MD* {
    const int id = EVP_PKEY_base_id(key);
    return id == EVP_PKEY_ED25519 || id == EVP_PKEY_ED448 ? nullptr : EVP_sha256();
}

inline auto uri_sans(X509* cert) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::unique_ptr<GENERAL_NAMES, names_free> names(static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr)));
    if (!names) {
        return out;
    }
    for (int i = 0; i < sk_GENERAL_NAME_num(names.get()); ++i) {
        const GENERAL_NAME* n = sk_GENERAL_NAME_value(names.get(), i);
        if (n->type == GEN_URI) {
            const ASN1_IA5STRING* s = n->d.uniformResourceIdentifier;
            out.emplace_back(reinterpret_cast<const char*>(ASN1_STRING_get0_data(s)),
                             static_cast<std::size_t>(ASN1_STRING_length(s)));
        }
    }
    return out;
}

inline auto read_text_file(const std::string& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw digest_signing_error("cannot read " + path);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace signing_detail

/// Signs digests with a CA-issued key.
class digest_signer {
public:
    digest_signer(std::string_view key_pem, std::string chain_pem) : chain_(std::move(chain_pem)) {
        namespace sd = signing_detail;
        auto bio = sd::memory_bio(key_pem);
        key_.reset(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
        if (!key_) {
            ERR_clear_error();
            throw digest_signing_error("the digest signing key is not a PEM private key");
        }
        const auto certs = sd::read_certificates(chain_);
        if (certs.empty()) {
            throw digest_signing_error("the digest signing chain holds no certificate");
        }
        if (X509_check_private_key(certs.front().get(), key_.get()) != 1) {
            ERR_clear_error();
            throw digest_signing_error("the digest signing key does not match the leaf");
        }
    }

    static auto from_files(const std::string& key_path, const std::string& chain_path)
        -> digest_signer {
        return {signing_detail::read_text_file(key_path),
                signing_detail::read_text_file(chain_path)};
    }

    [[nodiscard]] auto sign(std::string_view ledger, const saved_digest& digest,
                            std::int64_t now_ms) const -> signed_digest {
        namespace sd = signing_detail;
        signed_digest out;
        out.ledger = std::string(ledger);
        out.digest = digest;
        out.signed_at = now_ms;
        out.certificate_chain = chain_;
        const auto msg = out.payload_hash();
        std::unique_ptr<EVP_MD_CTX, sd::md_ctx_free> ctx(EVP_MD_CTX_new());
        std::size_t len = 0;
        if (!ctx ||
            EVP_DigestSignInit(ctx.get(), nullptr, sd::digest_for(key_.get()), nullptr,
                               key_.get()) != 1 ||
            EVP_DigestSign(ctx.get(), nullptr, &len, msg.data(), msg.size()) != 1) {
            ERR_clear_error();
            throw digest_signing_error("signing the digest failed");
        }
        out.signature.resize(len);
        if (EVP_DigestSign(ctx.get(), out.signature.data(), &len, msg.data(), msg.size()) != 1) {
            ERR_clear_error();
            throw digest_signing_error("signing the digest failed");
        }
        out.signature.resize(len);
        return out;
    }

private:
    signing_detail::pkey_ptr key_;
    std::string chain_;
};

struct signature_check {
    std::optional<std::string> failure;  // nullopt: verified
    std::vector<std::string> signer;     // the leaf's URI SANs
};

/// Verifies a signed digest against a PEM bundle of trusted CA
/// certificates (see the file comment).
inline auto check_signed_digest(const signed_digest& sd, std::string_view ca_bundle_pem)
    -> signature_check {
    namespace d = signing_detail;
    signature_check out;
    auto fail = [&out](std::string why) {
        ERR_clear_error();
        out.failure = std::move(why);
        return out;
    };
    auto chain = d::read_certificates(sd.certificate_chain);
    if (chain.empty()) {
        return fail("the signed digest carries no certificate");
    }
    auto roots = d::read_certificates(ca_bundle_pem);
    if (roots.empty()) {
        return fail("the CA bundle holds no certificate");
    }
    std::unique_ptr<X509_STORE, d::store_free> store(X509_STORE_new());
    std::unique_ptr<STACK_OF(X509), d::stack_free> untrusted(sk_X509_new_null());
    std::unique_ptr<X509_STORE_CTX, d::store_ctx_free> ctx(X509_STORE_CTX_new());
    if (!store || !untrusted || !ctx) {
        return fail("out of memory");
    }
    for (auto& r : roots) {
        if (X509_STORE_add_cert(store.get(), r.get()) != 1) {
            return fail("cannot load the CA bundle");
        }
    }
    for (std::size_t i = 1; i < chain.size(); ++i) {
        if (sk_X509_push(untrusted.get(), chain[i].get()) == 0) {
            return fail("out of memory");
        }
        (void)chain[i].release();  // the stack owns it now
    }
    if (X509_STORE_CTX_init(ctx.get(), store.get(), chain.front().get(), untrusted.get()) != 1) {
        return fail("cannot set up chain verification");
    }
    X509_VERIFY_PARAM* param = X509_STORE_CTX_get0_param(ctx.get());
    X509_VERIFY_PARAM_set_time(param, static_cast<time_t>(sd.signed_at / 1000));
    if (X509_verify_cert(ctx.get()) != 1) {
        return fail(std::string("the signing certificate does not chain to the CA: ") +
                    X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx.get())));
    }
    EVP_PKEY* key = X509_get0_pubkey(chain.front().get());
    if (key == nullptr) {
        return fail("the signing certificate has no usable public key");
    }
    const auto msg = sd.payload_hash();
    std::unique_ptr<EVP_MD_CTX, d::md_ctx_free> md(EVP_MD_CTX_new());
    if (!md || EVP_DigestVerifyInit(md.get(), nullptr, d::digest_for(key), nullptr, key) != 1) {
        return fail("cannot set up signature verification");
    }
    if (EVP_DigestVerify(md.get(), sd.signature.data(), sd.signature.size(), msg.data(),
                         msg.size()) != 1) {
        return fail("the signature does not match the digest");
    }
    out.signer = d::uri_sans(chain.front().get());
    return out;
}

}  // namespace kythira::ledger
