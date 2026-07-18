#pragma once

/// @file acme_jws.hpp
/// @brief JWS (RFC 7515) request signing and JWK (RFC 7517) encoding for
///        ACME (RFC 8555), shared by `acme_certificate_provider` (the
///        client, which signs every request) and `tests/acme_test_server.hpp`
///        (which verifies them). ES256 (ECDSA P-256 + SHA-256) only — the
///        same default key algorithm `certificate_authority` itself uses.
///
/// Implemented entirely with OpenSSL EVP primitives and boost::json
/// (Requirement 18.10: no new external dependency for ACME support).

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <boost/json.hpp>

#include <array>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace raft::testing::acme_jws {

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
using bio_ptr = std::unique_ptr<BIO, openssl_deleter<BIO, BIO_free_all>>;
using ecdsa_sig_ptr = std::unique_ptr<ECDSA_SIG, openssl_deleter<ECDSA_SIG, ECDSA_SIG_free>>;
using bn_ptr = std::unique_ptr<BIGNUM, openssl_deleter<BIGNUM, BN_free>>;

[[noreturn]] inline void throw_openssl_error(const std::string& context) {
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
    auto details = out.str();
    throw std::runtime_error(details.empty() ? context : context + ": " + details);
}

// ---------------------------------------------------------------------------
// base64url (RFC 4648 §5) — standard base64 via OpenSSL, then translated and
// unpadded per RFC 7515's requirement that JWS/JWK use base64url with no
// padding.
// ---------------------------------------------------------------------------

[[nodiscard]] inline auto base64url_encode(const std::vector<unsigned char>& in) -> std::string {
    std::string out;
    out.resize(4 * ((in.size() + 2) / 3) + 1);
    int len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), in.data(),
                              static_cast<int>(in.size()));
    out.resize(static_cast<std::size_t>(len));
    for (auto& c : out) {
        if (c == '+') {
            c = '-';
        } else if (c == '/') {
            c = '_';
        }
    }
    while (!out.empty() && out.back() == '=') {
        out.pop_back();
    }
    return out;
}

[[nodiscard]] inline auto base64url_encode(std::string_view in) -> std::string {
    return base64url_encode(std::vector<unsigned char>(in.begin(), in.end()));
}

[[nodiscard]] inline auto base64url_decode(std::string_view in) -> std::vector<unsigned char> {
    std::string padded(in);
    for (auto& c : padded) {
        if (c == '-') {
            c = '+';
        } else if (c == '_') {
            c = '/';
        }
    }
    while (padded.size() % 4 != 0) {
        padded.push_back('=');
    }
    std::vector<unsigned char> out(padded.size() / 4 * 3 + 1);
    int len = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(padded.data()),
                              static_cast<int>(padded.size()));
    if (len < 0) {
        throw std::invalid_argument("acme_jws: invalid base64url input");
    }
    // EVP_DecodeBlock always outputs a multiple of 3 bytes, over-counting for
    // the padding we just added back — trim by however many '=' we added.
    std::size_t added_padding = 0;
    for (auto it = padded.rbegin(); it != padded.rend() && *it == '='; ++it) {
        ++added_padding;
    }
    out.resize(static_cast<std::size_t>(len) - added_padding);
    return out;
}

[[nodiscard]] inline auto base64url_decode_string(std::string_view in) -> std::string {
    auto bytes = base64url_decode(in);
    return std::string(bytes.begin(), bytes.end());
}

// ---------------------------------------------------------------------------
// Key generation / JWK encoding — ES256 (ECDSA P-256) only.
// ---------------------------------------------------------------------------

[[nodiscard]] inline auto generate_p256_key() -> evp_pkey_ptr {
    evp_pkey_ctx_ptr ctx{EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr)};
    if (!ctx) {
        throw_openssl_error("acme_jws: EVP_PKEY_CTX_new_id failed");
    }
    if (EVP_PKEY_keygen_init(ctx.get()) <= 0) {
        throw_openssl_error("acme_jws: EVP_PKEY_keygen_init failed");
    }
    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) <= 0) {
        throw_openssl_error("acme_jws: set curve failed");
    }
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &raw) <= 0) {
        throw_openssl_error("acme_jws: EVP_PKEY_keygen failed");
    }
    return evp_pkey_ptr{raw};
}

[[nodiscard]] inline auto serialize_key(EVP_PKEY* key) -> std::string {
    bio_ptr bio{BIO_new(BIO_s_mem())};
    if (!bio) {
        throw_openssl_error("acme_jws: BIO_new failed");
    }
    if (PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        throw_openssl_error("acme_jws: PEM_write_bio_PrivateKey failed");
    }
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio.get(), &mem);
    return std::string(mem->data, mem->length);
}

[[nodiscard]] inline auto load_private_key(const std::string& pem) -> evp_pkey_ptr {
    bio_ptr bio{BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
    evp_pkey_ptr key{PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr)};
    if (!key) {
        throw_openssl_error("acme_jws: failed to parse account key PEM");
    }
    return key;
}

// Extracts the P-256 public key's x/y coordinates, each left-padded to 32
// bytes, base64url-encoded — the "x"/"y" members of an EC JWK (RFC 7518 §6.2).
inline auto ec_xy_base64url(EVP_PKEY* key, std::string& x_out, std::string& y_out) -> void {
    EC_KEY* ec_key = EVP_PKEY_get1_EC_KEY(key);
    if (ec_key == nullptr) {
        throw std::invalid_argument("acme_jws: key is not an EC key");
    }
    struct ec_key_guard {
        EC_KEY* k;
        ~ec_key_guard() { EC_KEY_free(k); }
    } guard{ec_key};

    const EC_GROUP* group = EC_KEY_get0_group(ec_key);
    const EC_POINT* point = EC_KEY_get0_public_key(ec_key);
    bn_ptr x{BN_new()};
    bn_ptr y{BN_new()};
    if (!x || !y) {
        throw_openssl_error("acme_jws: BN_new failed");
    }
    if (EC_POINT_get_affine_coordinates(group, point, x.get(), y.get(), nullptr) != 1) {
        throw_openssl_error("acme_jws: EC_POINT_get_affine_coordinates failed");
    }

    constexpr int k_coord_len = 32;  // P-256
    std::vector<unsigned char> xb(k_coord_len), yb(k_coord_len);
    if (BN_bn2binpad(x.get(), xb.data(), k_coord_len) < 0) {
        throw_openssl_error("acme_jws: BN_bn2binpad(x) failed");
    }
    if (BN_bn2binpad(y.get(), yb.data(), k_coord_len) < 0) {
        throw_openssl_error("acme_jws: BN_bn2binpad(y) failed");
    }
    x_out = base64url_encode(xb);
    y_out = base64url_encode(yb);
}

// RFC 7517 §4 EC JWK, with members in the fixed order RFC 7638 §3 requires
// for thumbprint computation (crv, kty, x, y — lexicographic).
[[nodiscard]] inline auto jwk_from_public_key(EVP_PKEY* key) -> boost::json::object {
    std::string x, y;
    ec_xy_base64url(key, x, y);
    boost::json::object jwk;
    jwk["crv"] = "P-256";
    jwk["kty"] = "EC";
    jwk["x"] = x;
    jwk["y"] = y;
    return jwk;
}

[[nodiscard]] inline auto public_key_from_jwk(const boost::json::object& jwk) -> evp_pkey_ptr {
    if (jwk.at("kty").as_string() != "EC" || jwk.at("crv").as_string() != "P-256") {
        throw std::invalid_argument("acme_jws: only EC P-256 JWKs are supported");
    }
    auto x = base64url_decode(jwk.at("x").as_string());
    auto y = base64url_decode(jwk.at("y").as_string());

    EC_KEY* ec_key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (ec_key == nullptr) {
        throw_openssl_error("acme_jws: EC_KEY_new_by_curve_name failed");
    }
    struct ec_key_guard {
        EC_KEY* k;
        ~ec_key_guard() { EC_KEY_free(k); }
    } guard{ec_key};

    bn_ptr bx{BN_bin2bn(x.data(), static_cast<int>(x.size()), nullptr)};
    bn_ptr by{BN_bin2bn(y.data(), static_cast<int>(y.size()), nullptr)};
    if (!bx || !by) {
        throw_openssl_error("acme_jws: BN_bin2bn failed");
    }
    if (EC_KEY_set_public_key_affine_coordinates(ec_key, bx.get(), by.get()) != 1) {
        throw_openssl_error("acme_jws: EC_KEY_set_public_key_affine_coordinates failed");
    }

    evp_pkey_ptr pkey{EVP_PKEY_new()};
    if (!pkey) {
        throw_openssl_error("acme_jws: EVP_PKEY_new failed");
    }
    if (EVP_PKEY_set1_EC_KEY(pkey.get(), ec_key) != 1) {
        throw_openssl_error("acme_jws: EVP_PKEY_set1_EC_KEY failed");
    }
    return pkey;
}

// RFC 7638: SHA-256 over the UTF-8 bytes of the canonical (no whitespace,
// lexicographically-ordered-member) JSON encoding of the public JWK,
// base64url-encoded.
[[nodiscard]] inline auto jwk_thumbprint(EVP_PKEY* key) -> std::string {
    auto jwk = jwk_from_public_key(key);
    std::string canonical = boost::json::serialize(jwk);
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size(),
           digest.data());
    return base64url_encode(std::vector<unsigned char>(digest.begin(), digest.end()));
}

// ---------------------------------------------------------------------------
// JWS compact serialization (RFC 7515 §3.1): base64url(header) + "." +
// base64url(payload) + "." + base64url(signature). ES256 signatures use the
// raw R||S concatenation (RFC 7518 §3.4), NOT the ASN.1 DER encoding
// EVP_DigestSign produces by default for EC keys — the DER form must be
// decomposed and each of R/S re-encoded as a fixed 32-byte big-endian value.
// ---------------------------------------------------------------------------

[[nodiscard]] inline auto der_to_raw_ecdsa_signature(const std::vector<unsigned char>& der)
    -> std::vector<unsigned char> {
    const unsigned char* p = der.data();
    ecdsa_sig_ptr sig{
        d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der.size()))};  // NOLINT(google-runtime-int)
    if (!sig) {
        throw_openssl_error("acme_jws: d2i_ECDSA_SIG failed");
    }

    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(sig.get(), &r, &s);

    constexpr int k_coord_len = 32;
    std::vector<unsigned char> raw(2 * k_coord_len, 0);
    if (BN_bn2binpad(r, raw.data(), k_coord_len) < 0) {
        throw_openssl_error("acme_jws: BN_bn2binpad(r) failed");
    }
    if (BN_bn2binpad(s, raw.data() + k_coord_len, k_coord_len) < 0) {
        throw_openssl_error("acme_jws: BN_bn2binpad(s) failed");
    }
    return raw;
}

[[nodiscard]] inline auto raw_to_der_ecdsa_signature(const std::vector<unsigned char>& raw)
    -> std::vector<unsigned char> {
    constexpr int k_coord_len = 32;
    if (raw.size() != 2 * k_coord_len) {
        throw std::invalid_argument("acme_jws: malformed ES256 signature length");
    }

    bn_ptr r{BN_bin2bn(raw.data(), k_coord_len, nullptr)};
    bn_ptr s{BN_bin2bn(raw.data() + k_coord_len, k_coord_len, nullptr)};
    if (!r || !s) {
        throw_openssl_error("acme_jws: BN_bin2bn failed");
    }

    ecdsa_sig_ptr sig{ECDSA_SIG_new()};
    if (!sig) {
        throw_openssl_error("acme_jws: ECDSA_SIG_new failed");
    }
    // ECDSA_SIG_set0 takes ownership of r/s on success.
    if (ECDSA_SIG_set0(sig.get(), r.get(), s.get()) != 1) {
        throw_openssl_error("acme_jws: ECDSA_SIG_set0 failed");
    }
    r.release();
    s.release();

    unsigned char* der = nullptr;
    int len = i2d_ECDSA_SIG(sig.get(), &der);
    if (len < 0) {
        throw_openssl_error("acme_jws: i2d_ECDSA_SIG failed");
    }
    std::vector<unsigned char> out(der, der + len);
    OPENSSL_free(der);
    return out;
}

// Signs `payload` (already-serialized JSON, or an empty string for
// POST-as-GET) under `protected_header` (caller supplies alg/nonce/url and
// either "jwk" or "kid" per RFC 8555 §6.2) with `key`, returning the RFC
// 7515 compact serialization.
[[nodiscard]] inline auto sign(const std::string& payload, boost::json::object protected_header,
                               EVP_PKEY* key) -> std::string {
    protected_header["alg"] = "ES256";
    std::string header_b64 = base64url_encode(boost::json::serialize(protected_header));
    std::string payload_b64 = base64url_encode(payload);
    std::string signing_input = header_b64 + "." + payload_b64;

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (mdctx == nullptr) {
        throw_openssl_error("acme_jws: EVP_MD_CTX_new failed");
    }
    struct mdctx_guard {
        EVP_MD_CTX* c;
        ~mdctx_guard() { EVP_MD_CTX_free(c); }
    } guard{mdctx};

    if (EVP_DigestSignInit(mdctx, nullptr, EVP_sha256(), nullptr, key) != 1) {
        throw_openssl_error("acme_jws: EVP_DigestSignInit failed");
    }
    std::size_t der_len = 0;
    if (EVP_DigestSign(mdctx, nullptr, &der_len,
                       reinterpret_cast<const unsigned char*>(signing_input.data()),
                       signing_input.size()) != 1) {
        throw_openssl_error("acme_jws: EVP_DigestSign (sizing) failed");
    }
    std::vector<unsigned char> der(der_len);
    if (EVP_DigestSign(mdctx, der.data(), &der_len,
                       reinterpret_cast<const unsigned char*>(signing_input.data()),
                       signing_input.size()) != 1) {
        throw_openssl_error("acme_jws: EVP_DigestSign failed");
    }
    der.resize(der_len);

    auto raw_sig = der_to_raw_ecdsa_signature(der);
    return signing_input + "." + base64url_encode(raw_sig);
}

struct verified_jws {
    boost::json::object protected_header;
    std::string payload;  // raw (un-parsed) payload bytes as a string — "" for POST-as-GET
};

// Verifies `compact` (header.payload.signature) against `key` and returns
// the decoded header/payload. Throws std::invalid_argument on ANY failure —
// malformed input, wrong algorithm, or (most importantly) an invalid
// signature — the caller (acme_test_server) maps this to RFC 8555's
// "urn:ietf:params:acme:error:malformed"/"badSignature" problem document.
[[nodiscard]] inline auto verify(const std::string& compact, EVP_PKEY* key) -> verified_jws {
    auto first_dot = compact.find('.');
    auto second_dot = compact.find('.', first_dot + 1);
    if (first_dot == std::string::npos || second_dot == std::string::npos) {
        throw std::invalid_argument("acme_jws: malformed compact JWS");
    }
    std::string header_b64 = compact.substr(0, first_dot);
    std::string payload_b64 = compact.substr(first_dot + 1, second_dot - first_dot - 1);
    std::string sig_b64 = compact.substr(second_dot + 1);
    std::string signing_input = header_b64 + "." + payload_b64;

    auto header_json = boost::json::parse(base64url_decode_string(header_b64));
    if (!header_json.is_object() || header_json.as_object().at("alg").as_string() != "ES256") {
        throw std::invalid_argument(
            "acme_jws: unsupported or missing alg (only ES256 is supported)");
    }

    auto raw_sig = base64url_decode(sig_b64);
    auto der_sig = raw_to_der_ecdsa_signature(raw_sig);

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (mdctx == nullptr) {
        throw_openssl_error("acme_jws: EVP_MD_CTX_new failed");
    }
    struct mdctx_guard {
        EVP_MD_CTX* c;
        ~mdctx_guard() { EVP_MD_CTX_free(c); }
    } guard{mdctx};

    if (EVP_DigestVerifyInit(mdctx, nullptr, EVP_sha256(), nullptr, key) != 1) {
        throw_openssl_error("acme_jws: EVP_DigestVerifyInit failed");
    }
    int rc = EVP_DigestVerify(mdctx, der_sig.data(), der_sig.size(),
                              reinterpret_cast<const unsigned char*>(signing_input.data()),
                              signing_input.size());
    if (rc != 1) {
        throw std::invalid_argument("acme_jws: JWS signature verification failed");
    }

    verified_jws result;
    result.protected_header = header_json.as_object();
    result.payload = base64url_decode_string(payload_b64);
    return result;
}

}  // namespace raft::testing::acme_jws
