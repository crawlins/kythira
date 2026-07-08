#define BOOST_TEST_MODULE acme_jws_unit_test

#include <boost/test/unit_test.hpp>

#include <raft/acme_jws.hpp>

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
