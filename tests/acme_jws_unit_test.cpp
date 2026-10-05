// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE acme_jws_unit_test

#include <boost/test/unit_test.hpp>

#include <raft/acme_jws.hpp>

#include <openssl/core_names.h>

#include <array>
#include <stdexcept>
#include <vector>

using namespace raft::testing::acme_jws;

BOOST_AUTO_TEST_CASE(base64url_round_trip) {
    std::vector<unsigned char> data = {0x00, 0xFF, 0x10, 0x20, 0x30, 0xAB, 0xCD, 0xEF};
    auto encoded = base64url_encode(data);
    BOOST_TEST(encoded.find('+') == std::string::npos);
    BOOST_TEST(encoded.find('/') == std::string::npos);
    BOOST_TEST(encoded.find('=') == std::string::npos);
    auto decoded = base64url_decode(encoded);
    BOOST_TEST(decoded == data, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(base64url_string_round_trip) {
    std::string s = "hello, ACME world! {\"key\":\"value\"}";
    auto encoded = base64url_encode(s);
    auto decoded = base64url_decode_string(encoded);
    BOOST_TEST(decoded == s);
}

BOOST_AUTO_TEST_CASE(jwk_from_key_has_expected_shape) {
    auto key = generate_p256_key();
    auto jwk = jwk_from_public_key(key.get());
    BOOST_TEST(jwk.at("kty").as_string() == "EC");
    BOOST_TEST(jwk.at("crv").as_string() == "P-256");
    BOOST_TEST(!jwk.at("x").as_string().empty());
    BOOST_TEST(!jwk.at("y").as_string().empty());
}

BOOST_AUTO_TEST_CASE(jwk_round_trips_through_public_key_reconstruction) {
    auto key = generate_p256_key();
    auto jwk = jwk_from_public_key(key.get());
    auto reconstructed = public_key_from_jwk(jwk);
    auto jwk2 = jwk_from_public_key(reconstructed.get());
    BOOST_TEST(jwk.at("x").as_string() == jwk2.at("x").as_string());
    BOOST_TEST(jwk.at("y").as_string() == jwk2.at("y").as_string());
}

// Golden values produced by the EC_KEY-based encoder before the OpenSSL 3
// parameter-API port, for a key whose x coordinate has a leading zero byte
// (so the 32-byte left padding is exercised). An existing ACME account is
// identified by this JWK and thumbprint, so they must not change.
namespace {
// Public half only (SubjectPublicKeyInfo): no private key is committed.
constexpr const char* k_golden_public_pem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEAMh/i2OCA3yJ/GzOBvvArAmqdVa2\n"
    "OuddgyxjTiSIiKaDXhQ7hSKwbySHXiNSjLmMv7reg3riLTT3nJLbal/vhg==\n"
    "-----END PUBLIC KEY-----\n";
constexpr const char* k_golden_x = "AMh_i2OCA3yJ_GzOBvvArAmqdVa2OuddgyxjTiSIiKY";
constexpr const char* k_golden_y = "g14UO4UisG8kh14jUoy5jL-63oN64i0095yS22pf74Y";
constexpr const char* k_golden_thumbprint = "Ug_mPknpvnZyMozbJbeRSDAPF5o3UFcSyNMFpUW2i6c";

auto generate_key(const char* algorithm, const char* param_name, const char* param_value)
    -> evp_pkey_ptr {
    evp_pkey_ctx_ptr ctx{EVP_PKEY_CTX_new_from_name(nullptr, algorithm, nullptr)};
    BOOST_REQUIRE(ctx);
    BOOST_REQUIRE_EQUAL(EVP_PKEY_keygen_init(ctx.get()), 1);
    if (param_name != nullptr) {
        std::array<OSSL_PARAM, 2> params{
            OSSL_PARAM_construct_utf8_string(param_name, const_cast<char*>(param_value), 0),
            OSSL_PARAM_construct_end()};
        BOOST_REQUIRE_EQUAL(EVP_PKEY_CTX_set_params(ctx.get(), params.data()), 1);
    }
    EVP_PKEY* raw = nullptr;
    BOOST_REQUIRE_EQUAL(EVP_PKEY_generate(ctx.get(), &raw), 1);
    return evp_pkey_ptr{raw};
}

auto load_golden_public_key() -> evp_pkey_ptr {
    bio_ptr bio{BIO_new_mem_buf(k_golden_public_pem, -1)};
    BOOST_REQUIRE(bio);
    evp_pkey_ptr key{PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr)};
    BOOST_REQUIRE(key);
    return key;
}
}  // namespace

BOOST_AUTO_TEST_CASE(jwk_and_thumbprint_match_pre_openssl3_port_golden_values) {
    auto key = load_golden_public_key();
    auto jwk = jwk_from_public_key(key.get());
    BOOST_TEST(jwk.at("x").as_string() == k_golden_x);
    BOOST_TEST(jwk.at("y").as_string() == k_golden_y);
    BOOST_TEST(jwk_thumbprint(key.get()) == k_golden_thumbprint);

    auto reconstructed = public_key_from_jwk(jwk);
    BOOST_TEST(EVP_PKEY_eq(key.get(), reconstructed.get()) == 1);
    BOOST_TEST(jwk_thumbprint(reconstructed.get()) == k_golden_thumbprint);
}

BOOST_AUTO_TEST_CASE(reconstructed_public_key_verifies_a_signature) {
    auto key = generate_p256_key();
    auto public_key = public_key_from_jwk(jwk_from_public_key(key.get()));
    boost::json::object header;
    header["nonce"] = "n";
    header["url"] = "https://acme.example/new-account";
    auto compact = sign(R"({"a":1})", header, key.get());
    auto result = verify(compact, public_key.get());
    BOOST_TEST(result.payload == R"({"a":1})");
}

BOOST_AUTO_TEST_CASE(public_key_from_jwk_accepts_short_coordinate) {
    // The golden x has a leading zero byte; dropping it must still decode to
    // the same key, as BN_bin2bn accepted before the port.
    auto key = load_golden_public_key();
    auto jwk = jwk_from_public_key(key.get());
    auto x = base64url_decode(jwk.at("x").as_string());
    BOOST_REQUIRE_EQUAL(x.front(), 0);
    jwk["x"] = base64url_encode(std::vector<unsigned char>(x.begin() + 1, x.end()));
    auto reconstructed = public_key_from_jwk(jwk);
    BOOST_TEST(EVP_PKEY_eq(key.get(), reconstructed.get()) == 1);
}

BOOST_AUTO_TEST_CASE(public_key_from_jwk_rejects_point_off_the_curve) {
    auto jwk = jwk_from_public_key(load_golden_public_key().get());
    auto y = base64url_decode(jwk.at("y").as_string());
    y.back() ^= 0x01;
    jwk["y"] = base64url_encode(y);
    BOOST_CHECK_THROW(static_cast<void>(public_key_from_jwk(jwk)), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(public_key_from_jwk_rejects_oversized_coordinate) {
    auto jwk = jwk_from_public_key(load_golden_public_key().get());
    auto x = base64url_decode(jwk.at("x").as_string());
    x.insert(x.begin(), 0x00);
    jwk["x"] = base64url_encode(x);
    BOOST_CHECK_THROW(static_cast<void>(public_key_from_jwk(jwk)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(public_key_from_jwk_rejects_other_curves) {
    auto jwk = jwk_from_public_key(load_golden_public_key().get());
    jwk["crv"] = "P-384";
    BOOST_CHECK_THROW(static_cast<void>(public_key_from_jwk(jwk)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(jwk_from_public_key_rejects_non_p256_keys) {
    auto p384 = generate_key("EC", OSSL_PKEY_PARAM_GROUP_NAME, "P-384");
    BOOST_CHECK_THROW(static_cast<void>(jwk_from_public_key(p384.get())), std::invalid_argument);
    auto ed25519 = generate_key("ED25519", nullptr, nullptr);
    BOOST_CHECK_THROW(static_cast<void>(jwk_from_public_key(ed25519.get())), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(thumbprint_is_deterministic_and_key_specific) {
    auto key_a = generate_p256_key();
    auto key_b = generate_p256_key();
    auto tp_a1 = jwk_thumbprint(key_a.get());
    auto tp_a2 = jwk_thumbprint(key_a.get());
    auto tp_b = jwk_thumbprint(key_b.get());
    BOOST_TEST(tp_a1 == tp_a2);
    BOOST_TEST(tp_a1 != tp_b);
}

BOOST_AUTO_TEST_CASE(sign_and_verify_round_trip_with_embedded_jwk) {
    auto key = generate_p256_key();
    auto jwk = jwk_from_public_key(key.get());

    boost::json::object header;
    header["jwk"] = jwk;
    header["nonce"] = "test-nonce-123";
    header["url"] = "https://acme.example/new-account";

    std::string payload =
        boost::json::serialize(boost::json::object{{"termsOfServiceAgreed", true}});
    auto compact = sign(payload, header, key.get());

    // Three dot-separated base64url segments.
    BOOST_TEST(std::count(compact.begin(), compact.end(), '.') == 2);

    auto verified = verify(compact, key.get());
    BOOST_TEST(verified.payload == payload);
    BOOST_TEST(verified.protected_header.at("nonce").as_string() == "test-nonce-123");
    BOOST_TEST(verified.protected_header.at("url").as_string() ==
               "https://acme.example/new-account");
}

BOOST_AUTO_TEST_CASE(verify_rejects_signature_from_different_key) {
    auto signer_key = generate_p256_key();
    auto other_key = generate_p256_key();

    boost::json::object header;
    header["kid"] = "https://acme.example/acct/1";
    header["nonce"] = "n1";
    header["url"] = "https://acme.example/new-order";
    auto compact =
        sign(boost::json::serialize(boost::json::object{{"identifiers", boost::json::array{}}}),
             header, signer_key.get());

    BOOST_CHECK_THROW(verify(compact, other_key.get()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(verify_rejects_tampered_payload) {
    auto key = generate_p256_key();
    boost::json::object header;
    header["kid"] = "https://acme.example/acct/1";
    header["nonce"] = "n1";
    header["url"] = "https://acme.example/new-order";
    auto compact = sign(boost::json::serialize(boost::json::object{{"a", 1}}), header, key.get());

    // Flip the payload segment to a different (still validly base64url,
    // still validly-JSON-decodable) value without re-signing.
    auto first_dot = compact.find('.');
    auto second_dot = compact.find('.', first_dot + 1);
    std::string tampered = compact.substr(0, first_dot + 1) +
                           base64url_encode(std::string("{\"a\":2}")) + compact.substr(second_dot);

    BOOST_CHECK_THROW(verify(tampered, key.get()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(empty_payload_round_trips_for_post_as_get) {
    auto key = generate_p256_key();
    boost::json::object header;
    header["kid"] = "https://acme.example/acct/1";
    header["nonce"] = "n1";
    header["url"] = "https://acme.example/order/1";
    auto compact = sign("", header, key.get());
    auto verified = verify(compact, key.get());
    BOOST_TEST(verified.payload.empty());
}
