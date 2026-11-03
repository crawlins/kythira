// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file jwt.hpp
/// @brief JOSE primitives for the OIDC issuer and for verifying identity
///        tokens: base64url (RFC 4648 §5), JWK encoding and decoding
///        (RFC 7517/7518), JWK thumbprints (RFC 7638), and JWS compact
///        signing and verification (RFC 7515) for RS256/384/512 and
///        ES256/384/512.
///
/// `.kiro/specs/iam-compatible-auth/` Requirement 1 (the CA cluster signs
/// identity tokens with it) and Requirement 11 (IAM verifies tokens from
/// external issuers with it). It lives in core because both the CA cluster
/// and the IAM module use it (Requirement 17.2).
///
/// Everything here is built on OpenSSL 3 EVP and provider-parameter APIs and
/// boost::json. `raft::testing::acme_jws` keeps its own ES256-only copy for
/// ACME; nothing here depends on it.
///
/// Verification is strict by design. `verify_compact` refuses `alg: none`,
/// any algorithm other than the one the caller expects, a key of the wrong
/// type or curve, RSA keys under 2048 bits, padded or non-alphabet base64url,
/// and a header that is not a JSON object. Callers pick the expected
/// algorithm from their own trust configuration (the JWKS entry), never from
/// the token, which is the classic algorithm-confusion hole.

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/objects.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <boost/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::jose {

// ---------------------------------------------------------------------------
// OpenSSL ownership helpers
// ---------------------------------------------------------------------------

namespace detail {

template<typename T, void (*Deleter)(T*)> struct openssl_deleter {
    void operator()(T* p) const noexcept {
        if (p != nullptr) {
            Deleter(p);
        }
    }
};

using evp_pkey_ctx_ptr =
    std::unique_ptr<EVP_PKEY_CTX, openssl_deleter<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using evp_md_ctx_ptr = std::unique_ptr<EVP_MD_CTX, openssl_deleter<EVP_MD_CTX, EVP_MD_CTX_free>>;
using bio_ptr = std::unique_ptr<BIO, openssl_deleter<BIO, BIO_free_all>>;
using bn_ptr = std::unique_ptr<BIGNUM, openssl_deleter<BIGNUM, BN_free>>;
using ecdsa_sig_ptr = std::unique_ptr<ECDSA_SIG, openssl_deleter<ECDSA_SIG, ECDSA_SIG_free>>;
using param_bld_ptr =
    std::unique_ptr<OSSL_PARAM_BLD, openssl_deleter<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free>>;
using param_ptr = std::unique_ptr<OSSL_PARAM, openssl_deleter<OSSL_PARAM, OSSL_PARAM_free>>;

[[noreturn]] inline void throw_openssl_error(const std::string& context) {
    std::ostringstream out;
    unsigned long code = 0;  // NOLINT(google-runtime-int) — matches OpenSSL's own typedef
    bool first = true;
    while ((code = ERR_get_error()) != 0) {
        std::array<char, 256> buf{};
        ERR_error_string_n(code, buf.data(), buf.size());
        if (!first) {
            out << "; ";
        }
        out << buf.data();
        first = false;
    }
    auto details = out.str();
    throw std::runtime_error(details.empty() ? "jose: " + context
                                             : "jose: " + context + ": " + details);
}

}  // namespace detail

/// An owned `EVP_PKEY`, public or private.
using evp_pkey_ptr = std::unique_ptr<EVP_PKEY, detail::openssl_deleter<EVP_PKEY, EVP_PKEY_free>>;

// ---------------------------------------------------------------------------
// base64url (RFC 4648 §5, unpadded as RFC 7515 §2 requires)
// ---------------------------------------------------------------------------

[[nodiscard]] inline auto base64url_encode(std::span<const unsigned char> in) -> std::string {
    static constexpr std::string_view k_alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((in.size() * 4 + 2) / 3);
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(in[i]) << 16) |
                                (static_cast<std::uint32_t>(in[i + 1]) << 8) |
                                static_cast<std::uint32_t>(in[i + 2]);
        out += k_alphabet[(v >> 18) & 0x3F];
        out += k_alphabet[(v >> 12) & 0x3F];
        out += k_alphabet[(v >> 6) & 0x3F];
        out += k_alphabet[v & 0x3F];
    }
    const std::size_t rest = in.size() - i;
    if (rest == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(in[i]) << 16;
        out += k_alphabet[(v >> 18) & 0x3F];
        out += k_alphabet[(v >> 12) & 0x3F];
    } else if (rest == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(in[i]) << 16) |
                                (static_cast<std::uint32_t>(in[i + 1]) << 8);
        out += k_alphabet[(v >> 18) & 0x3F];
        out += k_alphabet[(v >> 12) & 0x3F];
        out += k_alphabet[(v >> 6) & 0x3F];
    }
    return out;
}

[[nodiscard]] inline auto base64url_encode(std::string_view in) -> std::string {
    return base64url_encode(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(in.data()), in.size()));
}

/// Strict decode: only the base64url alphabet, no padding, no whitespace, and
/// no input whose length is 1 mod 4 (which no encoder produces). Returns
/// std::nullopt on any violation so that verifiers can reject a token without
/// a throw on a hostile path.
[[nodiscard]] inline auto base64url_decode(std::string_view in)
    -> std::optional<std::vector<unsigned char>> {
    static const auto table = [] {
        std::array<std::int8_t, 256> t{};
        t.fill(-1);
        constexpr std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (std::size_t k = 0; k < alphabet.size(); ++k) {
            t[static_cast<unsigned char>(alphabet[k])] = static_cast<std::int8_t>(k);
        }
        return t;
    }();
    if (in.size() % 4 == 1) {
        return std::nullopt;
    }
    std::vector<unsigned char> out;
    out.reserve(in.size() * 3 / 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        const std::int8_t v = table[static_cast<unsigned char>(c)];
        if (v < 0) {
            return std::nullopt;
        }
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> bits) & 0xFF));
        }
    }
    // Leftover bits must be zero, so every byte string has exactly one
    // encoding and a token cannot be altered without changing its bytes.
    if (bits > 0 && (acc & ((1U << bits) - 1U)) != 0) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] inline auto base64url_decode_string(std::string_view in)
    -> std::optional<std::string> {
    auto bytes = base64url_decode(in);
    if (!bytes) {
        return std::nullopt;
    }
    return std::string(bytes->begin(), bytes->end());
}

// ---------------------------------------------------------------------------
// Algorithms
// ---------------------------------------------------------------------------

enum class jws_alg : std::uint8_t {
    rs256,
    rs384,
    rs512,
    es256,
    es384,
    es512
};

[[nodiscard]] constexpr auto alg_name(jws_alg alg) -> std::string_view {
    switch (alg) {
        case jws_alg::rs256:
            return "RS256";
        case jws_alg::rs384:
            return "RS384";
        case jws_alg::rs512:
            return "RS512";
        case jws_alg::es256:
            return "ES256";
        case jws_alg::es384:
            return "ES384";
        case jws_alg::es512:
            return "ES512";
    }
    return "";
}

/// Case-sensitive, as RFC 7515 §4.1.1 requires. "none" and every HMAC
/// algorithm are deliberately absent.
[[nodiscard]] constexpr auto parse_alg(std::string_view name) -> std::optional<jws_alg> {
    if (name == "RS256") {
        return jws_alg::rs256;
    }
    if (name == "RS384") {
        return jws_alg::rs384;
    }
    if (name == "RS512") {
        return jws_alg::rs512;
    }
    if (name == "ES256") {
        return jws_alg::es256;
    }
    if (name == "ES384") {
        return jws_alg::es384;
    }
    if (name == "ES512") {
        return jws_alg::es512;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr auto is_ecdsa(jws_alg alg) -> bool {
    return alg == jws_alg::es256 || alg == jws_alg::es384 || alg == jws_alg::es512;
}

namespace detail {

[[nodiscard]] inline auto digest_for(jws_alg alg) -> const EVP_MD* {
    switch (alg) {
        case jws_alg::rs256:
        case jws_alg::es256:
            return EVP_sha256();
        case jws_alg::rs384:
        case jws_alg::es384:
            return EVP_sha384();
        case jws_alg::rs512:
        case jws_alg::es512:
            return EVP_sha512();
    }
    return nullptr;
}

struct curve_info {
    std::string_view jwk_crv;     // "P-256"
    std::string_view group_name;  // OpenSSL group name
    int nid;
    std::size_t coord_len;  // bytes per coordinate and per signature half
};

[[nodiscard]] inline auto curve_for(jws_alg alg) -> std::optional<curve_info> {
    switch (alg) {
        case jws_alg::es256:
            return curve_info{"P-256", SN_X9_62_prime256v1, NID_X9_62_prime256v1, 32};
        case jws_alg::es384:
            return curve_info{"P-384", SN_secp384r1, NID_secp384r1, 48};
        case jws_alg::es512:
            return curve_info{"P-521", SN_secp521r1, NID_secp521r1, 66};
        default:
            return std::nullopt;
    }
}

[[nodiscard]] inline auto curve_for_crv(std::string_view crv) -> std::optional<curve_info> {
    for (auto alg : {jws_alg::es256, jws_alg::es384, jws_alg::es512}) {
        auto c = curve_for(alg);
        if (c && c->jwk_crv == crv) {
            return c;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline auto key_curve_nid(EVP_PKEY* key) -> int {
    std::array<char, 64> group{};
    if (EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group.data(), group.size(),
                                       nullptr) != 1) {
        return NID_undef;
    }
    return OBJ_txt2nid(group.data());
}

/// Whether `key` is usable with `alg`: an RSA key of at least 2048 bits for
/// RS*, or an EC key on the matching curve for ES*.
[[nodiscard]] inline auto key_matches_alg(EVP_PKEY* key, jws_alg alg) -> bool {
    if (key == nullptr) {
        return false;
    }
    if (is_ecdsa(alg)) {
        auto c = curve_for(alg);
        return EVP_PKEY_is_a(key, "EC") == 1 && c && key_curve_nid(key) == c->nid;
    }
    return EVP_PKEY_is_a(key, "RSA") == 1 && EVP_PKEY_get_bits(key) >= 2048;
}

[[nodiscard]] inline auto bn_to_b64url(const BIGNUM* bn, std::size_t pad_to = 0) -> std::string {
    const auto natural = static_cast<std::size_t>(BN_num_bytes(bn));
    const std::size_t len = pad_to == 0 ? natural : pad_to;
    if (natural > len) {
        throw std::invalid_argument("jose: big number wider than its encoding");
    }
    std::vector<unsigned char> buf(len == 0 ? 1 : len, 0);
    if (BN_bn2binpad(bn, buf.data(), static_cast<int>(buf.size())) < 0) {
        throw_openssl_error("BN_bn2binpad failed");
    }
    return base64url_encode(buf);
}

[[nodiscard]] inline auto get_bn(EVP_PKEY* key, const char* name) -> bn_ptr {
    BIGNUM* raw = nullptr;
    if (EVP_PKEY_get_bn_param(key, name, &raw) != 1) {
        throw_openssl_error(std::string("EVP_PKEY_get_bn_param(") + name + ") failed");
    }
    return bn_ptr{raw};
}

[[nodiscard]] inline auto jwk_string(const boost::json::object& jwk, std::string_view member)
    -> std::string_view {
    const auto* v = jwk.if_contains(member);
    if (v == nullptr || !v->is_string()) {
        throw std::invalid_argument("jose: JWK member \"" + std::string(member) +
                                    "\" missing or not a string");
    }
    return v->as_string();
}

[[nodiscard]] inline auto jwk_bytes(const boost::json::object& jwk, std::string_view member)
    -> std::vector<unsigned char> {
    auto bytes = base64url_decode(jwk_string(jwk, member));
    if (!bytes || bytes->empty()) {
        throw std::invalid_argument("jose: JWK member \"" + std::string(member) +
                                    "\" is not valid base64url");
    }
    return std::move(*bytes);
}

[[nodiscard]] inline auto bn_from(const std::vector<unsigned char>& bytes) -> bn_ptr {
    bn_ptr bn{BN_bin2bn(bytes.data(), static_cast<int>(bytes.size()), nullptr)};
    if (!bn) {
        throw_openssl_error("BN_bin2bn failed");
    }
    return bn;
}

[[nodiscard]] inline auto key_from_params(const char* type, int selection, OSSL_PARAM_BLD* bld)
    -> evp_pkey_ptr {
    param_ptr params{OSSL_PARAM_BLD_to_param(bld)};
    if (!params) {
        throw_openssl_error("OSSL_PARAM_BLD_to_param failed");
    }
    evp_pkey_ctx_ptr ctx{EVP_PKEY_CTX_new_from_name(nullptr, type, nullptr)};
    if (!ctx || EVP_PKEY_fromdata_init(ctx.get()) != 1) {
        throw_openssl_error("EVP_PKEY_fromdata_init failed");
    }
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_fromdata(ctx.get(), &raw, selection, params.get()) != 1) {
        throw std::invalid_argument("jose: JWK does not describe a valid " + std::string(type) +
                                    " key");
    }
    return evp_pkey_ptr{raw};
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

/// A fresh signing key for `alg`: RSA-2048 for RS*, or an EC key on the
/// algorithm's curve for ES*.
[[nodiscard]] inline auto generate_signing_key(jws_alg alg) -> evp_pkey_ptr {
    EVP_PKEY* raw = nullptr;
    if (is_ecdsa(alg)) {
        auto c = detail::curve_for(alg);
        raw = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", std::string(c->group_name).c_str());
    } else {
        raw = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<std::size_t>(2048));
    }
    if (raw == nullptr) {
        detail::throw_openssl_error("EVP_PKEY_Q_keygen failed");
    }
    return evp_pkey_ptr{raw};
}

/// PKCS#8 PEM of a private key.
[[nodiscard]] inline auto private_key_to_pem(EVP_PKEY* key) -> std::string {
    detail::bio_ptr bio{BIO_new(BIO_s_mem())};
    if (!bio ||
        PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        detail::throw_openssl_error("PEM_write_bio_PrivateKey failed");
    }
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio.get(), &mem);
    return std::string(mem->data, mem->length);
}

[[nodiscard]] inline auto private_key_from_pem(std::string_view pem) -> evp_pkey_ptr {
    detail::bio_ptr bio{BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
    evp_pkey_ptr key{PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr)};
    if (!key) {
        detail::throw_openssl_error("cannot parse private key PEM");
    }
    return key;
}

/// The public JWK of `key` (RFC 7517/7518), holding only the members RFC 7638
/// §3.2 requires, in lexicographic order: `e, kty, n` for RSA and
/// `crv, kty, x, y` for EC. Callers add `kid`, `alg` and `use` after
/// computing the thumbprint.
[[nodiscard]] inline auto public_jwk(EVP_PKEY* key) -> boost::json::object {
    boost::json::object jwk;
    if (EVP_PKEY_is_a(key, "RSA") == 1) {
        auto n = detail::get_bn(key, OSSL_PKEY_PARAM_RSA_N);
        auto e = detail::get_bn(key, OSSL_PKEY_PARAM_RSA_E);
        jwk["e"] = detail::bn_to_b64url(e.get());
        jwk["kty"] = "RSA";
        jwk["n"] = detail::bn_to_b64url(n.get());
        return jwk;
    }
    if (EVP_PKEY_is_a(key, "EC") == 1) {
        const int nid = detail::key_curve_nid(key);
        std::optional<detail::curve_info> curve;
        for (auto alg : {jws_alg::es256, jws_alg::es384, jws_alg::es512}) {
            auto c = detail::curve_for(alg);
            if (c->nid == nid) {
                curve = c;
            }
        }
        if (!curve) {
            throw std::invalid_argument("jose: EC key is not on P-256, P-384 or P-521");
        }
        auto x = detail::get_bn(key, OSSL_PKEY_PARAM_EC_PUB_X);
        auto y = detail::get_bn(key, OSSL_PKEY_PARAM_EC_PUB_Y);
        jwk["crv"] = std::string(curve->jwk_crv);
        jwk["kty"] = "EC";
        jwk["x"] = detail::bn_to_b64url(x.get(), curve->coord_len);
        jwk["y"] = detail::bn_to_b64url(y.get(), curve->coord_len);
        return jwk;
    }
    throw std::invalid_argument("jose: only RSA and EC keys are supported");
}

namespace detail {

inline void push_ec_public(OSSL_PARAM_BLD* bld, const boost::json::object& jwk,
                           const curve_info& curve, std::vector<unsigned char>& point_storage) {
    auto x = jwk_bytes(jwk, "x");
    auto y = jwk_bytes(jwk, "y");
    // RFC 7518 §6.2.1.2-3: each coordinate is exactly the curve's width.
    if (x.size() != curve.coord_len || y.size() != curve.coord_len) {
        throw std::invalid_argument("jose: EC JWK coordinate has the wrong length");
    }
    point_storage.assign(1, 0x04);  // uncompressed SEC1 point
    point_storage.insert(point_storage.end(), x.begin(), x.end());
    point_storage.insert(point_storage.end(), y.begin(), y.end());
    // The builder keeps the pointer until OSSL_PARAM_BLD_to_param, so it must
    // outlive this call: group_name views a NUL-terminated SN_* literal.
    if (OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, curve.group_name.data(),
                                        0) != 1 ||
        OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point_storage.data(),
                                         point_storage.size()) != 1) {
        throw_openssl_error("OSSL_PARAM_BLD_push (EC) failed");
    }
}

}  // namespace detail

/// Builds a public key from a JWK. Throws std::invalid_argument for anything
/// malformed, an unsupported `kty` or `crv`, or an EC point not on its curve.
[[nodiscard]] inline auto public_key_from_jwk(const boost::json::object& jwk) -> evp_pkey_ptr {
    const auto kty = detail::jwk_string(jwk, "kty");
    detail::param_bld_ptr bld{OSSL_PARAM_BLD_new()};
    if (!bld) {
        detail::throw_openssl_error("OSSL_PARAM_BLD_new failed");
    }
    if (kty == "RSA") {
        auto n = detail::bn_from(detail::jwk_bytes(jwk, "n"));
        auto e = detail::bn_from(detail::jwk_bytes(jwk, "e"));
        if (OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_N, n.get()) != 1 ||
            OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_E, e.get()) != 1) {
            detail::throw_openssl_error("OSSL_PARAM_BLD_push (RSA) failed");
        }
        return detail::key_from_params("RSA", EVP_PKEY_PUBLIC_KEY, bld.get());
    }
    if (kty == "EC") {
        auto curve = detail::curve_for_crv(detail::jwk_string(jwk, "crv"));
        if (!curve) {
            throw std::invalid_argument("jose: unsupported EC curve");
        }
        std::vector<unsigned char> point;
        detail::push_ec_public(bld.get(), jwk, *curve, point);
        return detail::key_from_params("EC", EVP_PKEY_PUBLIC_KEY, bld.get());
    }
    throw std::invalid_argument("jose: unsupported JWK kty");
}

/// Builds a private key from a JWK carrying its private members (RSA: `d`,
/// `p`, `q`, `dp`, `dq`, `qi`; EC: `d`). Used to load published test vectors;
/// the issuer itself stores keys as sealed PEM.
[[nodiscard]] inline auto private_key_from_jwk(const boost::json::object& jwk) -> evp_pkey_ptr {
    const auto kty = detail::jwk_string(jwk, "kty");
    detail::param_bld_ptr bld{OSSL_PARAM_BLD_new()};
    if (!bld) {
        detail::throw_openssl_error("OSSL_PARAM_BLD_new failed");
    }
    if (kty == "RSA") {
        std::vector<detail::bn_ptr> keep;
        const std::array<std::pair<const char*, const char*>, 8> members{{
            {"n", OSSL_PKEY_PARAM_RSA_N},
            {"e", OSSL_PKEY_PARAM_RSA_E},
            {"d", OSSL_PKEY_PARAM_RSA_D},
            {"p", OSSL_PKEY_PARAM_RSA_FACTOR1},
            {"q", OSSL_PKEY_PARAM_RSA_FACTOR2},
            {"dp", OSSL_PKEY_PARAM_RSA_EXPONENT1},
            {"dq", OSSL_PKEY_PARAM_RSA_EXPONENT2},
            {"qi", OSSL_PKEY_PARAM_RSA_COEFFICIENT1},
        }};
        for (const auto& [member, param] : members) {
            keep.push_back(detail::bn_from(detail::jwk_bytes(jwk, member)));
            if (OSSL_PARAM_BLD_push_BN(bld.get(), param, keep.back().get()) != 1) {
                detail::throw_openssl_error("OSSL_PARAM_BLD_push (RSA private) failed");
            }
        }
        return detail::key_from_params("RSA", EVP_PKEY_KEYPAIR, bld.get());
    }
    if (kty == "EC") {
        auto curve = detail::curve_for_crv(detail::jwk_string(jwk, "crv"));
        if (!curve) {
            throw std::invalid_argument("jose: unsupported EC curve");
        }
        std::vector<unsigned char> point;
        detail::push_ec_public(bld.get(), jwk, *curve, point);
        auto d = detail::bn_from(detail::jwk_bytes(jwk, "d"));
        if (OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_PRIV_KEY, d.get()) != 1) {
            detail::throw_openssl_error("OSSL_PARAM_BLD_push (EC private) failed");
        }
        return detail::key_from_params("EC", EVP_PKEY_KEYPAIR, bld.get());
    }
    throw std::invalid_argument("jose: unsupported JWK kty");
}

/// RFC 7638 thumbprint: base64url(SHA-256) of the required public members in
/// lexicographic order with no whitespace. Members other than the required
/// ones (`kid`, `alg`, `use`, private members) are ignored.
[[nodiscard]] inline auto jwk_thumbprint(const boost::json::object& jwk) -> std::string {
    const auto kty = detail::jwk_string(jwk, "kty");
    boost::json::object canonical;
    if (kty == "RSA") {
        canonical["e"] = detail::jwk_string(jwk, "e");
        canonical["kty"] = "RSA";
        canonical["n"] = detail::jwk_string(jwk, "n");
    } else if (kty == "EC") {
        canonical["crv"] = detail::jwk_string(jwk, "crv");
        canonical["kty"] = "EC";
        canonical["x"] = detail::jwk_string(jwk, "x");
        canonical["y"] = detail::jwk_string(jwk, "y");
    } else {
        throw std::invalid_argument("jose: unsupported JWK kty");
    }
    // boost::json keeps insertion order and writes no whitespace.
    const std::string text = boost::json::serialize(canonical);
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest.data());
    return base64url_encode(digest);
}

// ---------------------------------------------------------------------------
// JWS
// ---------------------------------------------------------------------------

/// The raw JWS signature (RFC 7518 §3.3-3.4) over `signing_input`: PKCS#1
/// v1.5 bytes for RS*, the fixed-width R||S concatenation for ES*.
[[nodiscard]] inline auto sign_bytes(std::string_view signing_input, EVP_PKEY* key, jws_alg alg)
    -> std::vector<unsigned char> {
    if (!detail::key_matches_alg(key, alg)) {
        throw std::invalid_argument("jose: key does not match " + std::string(alg_name(alg)));
    }
    detail::evp_md_ctx_ptr ctx{EVP_MD_CTX_new()};
    if (!ctx ||
        EVP_DigestSignInit(ctx.get(), nullptr, detail::digest_for(alg), nullptr, key) != 1) {
        detail::throw_openssl_error("EVP_DigestSignInit failed");
    }
    const auto* data = reinterpret_cast<const unsigned char*>(signing_input.data());
    std::size_t len = 0;
    if (EVP_DigestSign(ctx.get(), nullptr, &len, data, signing_input.size()) != 1) {
        detail::throw_openssl_error("EVP_DigestSign (size) failed");
    }
    std::vector<unsigned char> sig(len);
    if (EVP_DigestSign(ctx.get(), sig.data(), &len, data, signing_input.size()) != 1) {
        detail::throw_openssl_error("EVP_DigestSign failed");
    }
    sig.resize(len);
    if (!is_ecdsa(alg)) {
        return sig;
    }
    // EVP produces DER for ECDSA; JWS wants R||S, each the curve's width.
    const unsigned char* p = sig.data();
    detail::ecdsa_sig_ptr der{
        d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(sig.size()))};  // NOLINT(google-runtime-int)
    if (!der) {
        detail::throw_openssl_error("d2i_ECDSA_SIG failed");
    }
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(der.get(), &r, &s);
    const auto width = detail::curve_for(alg)->coord_len;
    std::vector<unsigned char> raw(2 * width, 0);
    if (BN_bn2binpad(r, raw.data(), static_cast<int>(width)) < 0 ||
        BN_bn2binpad(s, raw.data() + width, static_cast<int>(width)) < 0) {
        detail::throw_openssl_error("BN_bn2binpad (R||S) failed");
    }
    return raw;
}

/// Whether `signature` is a valid raw JWS signature over `signing_input`.
/// Never throws for a bad signature; returns false.
[[nodiscard]] inline auto verify_bytes(std::string_view signing_input,
                                       std::span<const unsigned char> signature, EVP_PKEY* key,
                                       jws_alg alg) -> bool {
    if (!detail::key_matches_alg(key, alg)) {
        return false;
    }
    std::vector<unsigned char> to_check;
    if (is_ecdsa(alg)) {
        const auto width = detail::curve_for(alg)->coord_len;
        if (signature.size() != 2 * width) {
            return false;
        }
        detail::bn_ptr r{BN_bin2bn(signature.data(), static_cast<int>(width), nullptr)};
        detail::bn_ptr s{BN_bin2bn(signature.data() + width, static_cast<int>(width), nullptr)};
        detail::ecdsa_sig_ptr der{ECDSA_SIG_new()};
        if (!r || !s || !der || ECDSA_SIG_set0(der.get(), r.get(), s.get()) != 1) {
            ERR_clear_error();
            return false;
        }
        (void)r.release();  // owned by `der` now
        (void)s.release();
        unsigned char* out = nullptr;
        const int len = i2d_ECDSA_SIG(der.get(), &out);
        if (len <= 0) {
            ERR_clear_error();
            return false;
        }
        to_check.assign(out, out + len);
        OPENSSL_free(out);
    } else {
        to_check.assign(signature.begin(), signature.end());
    }
    detail::evp_md_ctx_ptr ctx{EVP_MD_CTX_new()};
    if (!ctx ||
        EVP_DigestVerifyInit(ctx.get(), nullptr, detail::digest_for(alg), nullptr, key) != 1) {
        ERR_clear_error();
        return false;
    }
    const int rc = EVP_DigestVerify(ctx.get(), to_check.data(), to_check.size(),
                                    reinterpret_cast<const unsigned char*>(signing_input.data()),
                                    signing_input.size());
    ERR_clear_error();
    return rc == 1;
}

/// RFC 7515 compact serialization of `payload` under `header`. The header's
/// `alg` is set to `alg`, overwriting any value the caller left there, so the
/// header can never disagree with the key that signed it.
[[nodiscard]] inline auto sign_compact(boost::json::object header, std::string_view payload,
                                       EVP_PKEY* key, jws_alg alg) -> std::string {
    header["alg"] = std::string(alg_name(alg));
    std::string input = base64url_encode(boost::json::serialize(header));
    input += '.';
    input += base64url_encode(payload);
    const auto sig = sign_bytes(input, key, alg);
    input += '.';
    input += base64url_encode(sig);
    return input;
}

struct compact_parts {
    boost::json::object header;
    std::string payload;
    std::string signing_input;  // "<b64 header>.<b64 payload>"
    std::vector<unsigned char> signature;
};

/// Splits and decodes a compact JWS without verifying it, for choosing the
/// verification key by `kid`. Returns std::nullopt for anything that is not
/// three strict base64url segments with a JSON-object header.
[[nodiscard]] inline auto parse_compact(std::string_view compact) -> std::optional<compact_parts> {
    const auto dot1 = compact.find('.');
    if (dot1 == std::string_view::npos) {
        return std::nullopt;
    }
    const auto dot2 = compact.find('.', dot1 + 1);
    if (dot2 == std::string_view::npos || compact.find('.', dot2 + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    auto header_text = base64url_decode_string(compact.substr(0, dot1));
    auto payload = base64url_decode_string(compact.substr(dot1 + 1, dot2 - dot1 - 1));
    auto signature = base64url_decode(compact.substr(dot2 + 1));
    if (!header_text || !payload || !signature) {
        return std::nullopt;
    }
    boost::system::error_code ec;
    auto header = boost::json::parse(*header_text, ec);
    if (ec || !header.is_object()) {
        return std::nullopt;
    }
    return compact_parts{std::move(header.as_object()), std::move(*payload),
                         std::string(compact.substr(0, dot2)), std::move(*signature)};
}

struct verified_jws {
    boost::json::object header;
    std::string payload;
};

/// Verifies `compact` with `key` under the algorithm the caller expects.
/// Returns std::nullopt if the token is malformed, if its header's `alg` is
/// not exactly `expected`, if it uses `crit` (no extensions are understood
/// here, so RFC 7515 §4.1.11 requires rejection), or if the signature fails.
[[nodiscard]] inline auto verify_compact(std::string_view compact, EVP_PKEY* key, jws_alg expected)
    -> std::optional<verified_jws> {
    auto parts = parse_compact(compact);
    if (!parts) {
        return std::nullopt;
    }
    const auto* alg = parts->header.if_contains("alg");
    if (alg == nullptr || !alg->is_string() || alg->as_string() != alg_name(expected)) {
        return std::nullopt;
    }
    if (parts->header.contains("crit")) {
        return std::nullopt;
    }
    if (!verify_bytes(parts->signing_input, parts->signature, key, expected)) {
        return std::nullopt;
    }
    return verified_jws{std::move(parts->header), std::move(parts->payload)};
}

}  // namespace kythira::jose
