// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Unit tests for include/raft/jwt.hpp (IAM spec task 1). Known-answer
// vectors come from RFC 7515 Appendix A.3, RFC 7520 §4.1 and §4.3 (the JOSE
// cookbook), RFC 7517 Appendix A.1 and RFC 7638 §3.1.

#define BOOST_TEST_MODULE jwt_unit_test

#include <boost/test/unit_test.hpp>

#include <raft/jwt.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace kythira::jose;

namespace {

constexpr std::array all_algs{jws_alg::rs256, jws_alg::rs384, jws_alg::rs512,
                              jws_alg::es256, jws_alg::es384, jws_alg::es512};

// RFC 7520 §4 shared payload ("It’s" with U+2019).
constexpr std::string_view cookbook_payload_b64 =
    "SXTigJlzIGEgZGFuZ2Vyb3VzIGJ1c2luZXNzLCBGcm9kbywgZ29pbmcgb3V0IHlvdXIgZG9vci4gWW91IHN0ZXAgb2"
    "50byB0aGUgcm9hZCwgYW5kIGlmIHlvdSBkb24ndCBrZWVwIHlvdXIgZmVldCwgdGhlcmXigJlzIG5vIGtub3dpbmcg"
    "d2hlcmUgeW91IG1pZ2h0IGJlIHN3ZXB0IG9mZiB0by4";

// RFC 7520 §3.4 RSA key (used by §4.1).
auto cookbook_rsa_jwk() -> boost::json::object {
    return boost::json::object{
        {"kty", "RSA"},
        {"kid", "bilbo.baggins@hobbiton.example"},
        {"use", "sig"},
        {"n",
         "n4EPtAOCc9AlkeQHPzHStgAbgs7bTZLwUBZdR8_KuKPEHLd4rHVTeT-O-XV2jRojdNhxJWTDvNd7nqQ0VEiZQHz_"
         "AJmSCpMaJMRBSFKrKb2wqVwGU_NsYOYL-QtiWN2lbzcEe6XC0dApr5ydQLrHqkHHig3RBordaZ6Aj-"
         "oBHqFEHYpPe7Tpe-"
         "OfVfHd1E6cS6M1FZcD1NNLYD5lFHpPI9bTwJlsde3uhGqC0ZCuEHg8lhzwOHrtIQbS0FVbb9k3-"
         "tVTU4fg_3L_vniUFAKwuCLqKnS2BYwdq_mzSnbLY7h_qixoR7jig3__kRhuaxwUkRz5iaiQkqgc5gHdrNP5zw"},
        {"e", "AQAB"},
        {"d",
         "bWUC9B-EFRIo8kpGfh0ZuyGPvMNKvYWNtB_ikiH9k20eT-O1q_I78eiZkpXxXQ0UTEs2LsNRS-8uJbvQ-"
         "A1irkwMSMkK1J3XTGgdrhCku9gRldY7sNA_AKZGh-Q661_42rINLRCe8W-nZ34ui_qOfkLnK9QWDDqpaIsA-"
         "bMwWWSDFu2MUBYwkHTMEzLYGqOe04noqeq1hExBTHBOBdkMXiuFhUq1BU6l-DqEiWxqg82sXt2h-"
         "LMnT3046AOYJoRioz75tSUQfGCshWTBnP5uDjd18kKhyv07lhfSJdrPdM5Plyl21hsFf4L_"
         "mHCuoFau7gdsPfHPxxjVOcOpBrQzwQ"},
        {"p",
         "3Slxg_DwTXJcb6095RoXygQCAZ5RnAvZlno1yhHtnUex_fp7AZ_9nRaO7HX_-"
         "SFfGQeutao2TDjDAWU4Vupk8rw9JR0AzZ0N2fvuIAmr_"
         "WCsmGpeNqQnev1T7IyEsnh8UMt-n5CafhkikzhEsrmndH6LxOrvRJlsPp6Zv8bUq0k"},
        {"q",
         "uKE2dh-cTf6ERF4k4e_jy78GfPYUIaUyoSSJuBzp3Cubk3OCqs6grT8bR_"
         "cu0Dm1MZwWmtdqDyI95HrUeq3MP15vMMON8lHTeZu2lmKvwqW7anV5UzhM1iZ7z4yMkuUwFWoBvyY898EXvRD-"
         "hdqRxHlSqAZ192zB3pVFJ0s7pFc"},
        {"dp",
         "B8PVvXkvJrj2L-GYQ7v3y9r6Kw5g9SahXBwsWUzp19TVlgI-YV85q1NIb1rxQtD-IsXXR3-"
         "TanevuRPRt5OBOdiMGQp8pbt26gljYfKU_E9xn-RULHz0-ed9E9gXLKD4VGngpz-PfQ_"
         "q29pk5xWHoJp009Qf1HvChixRX59ehik"},
        {"dq",
         "CLDmDGduhylc9o7r84rEUVn7pzQ6PF83Y-iBZx5NT-"
         "TpnOZKF1pErAMVeKzFEl41DlHHqqBLSM0W1sOFbwTxYWZDm6sI6og5iTbwQGIC3gnJKbi_7k_"
         "vJgGHwHxgPaX2PnvP-"
         "zyEkDERuf-ry4c_Z11Cq9AqC2yeL6kdKT1cYF8"},
        {"qi",
         "3PiqvXQN0zwMeE-sBvZgi289XP9XCQF3VWqPzMKnIgQp7_Tugo6-NZBKCQsMf3HaEGBjTVJs_jcK8-"
         "TRXvaKe-7ZMaQj8VfBdYkssbu0NKDDhjJ-"
         "GtiseaDVWt7dcH0cfwxgFUHpQh7FoCrjFJ6h6ZEpMF6xmujs4qMpPz8aaI4"},
    };
}

constexpr std::string_view cookbook_4_1_compact =
    "eyJhbGciOiJSUzI1NiIsImtpZCI6ImJpbGJvLmJhZ2dpbnNAaG9iYml0b24uZXhhbXBsZSJ9."
    "SXTigJlzIGEgZGFuZ2Vyb3VzIGJ1c2luZXNzLCBGcm9kbywgZ29pbmcgb3V0IHlvdXIgZG9vci4gWW91IHN0ZXAgb2"
    "50byB0aGUgcm9hZCwgYW5kIGlmIHlvdSBkb24ndCBrZWVwIHlvdXIgZmVldCwgdGhlcmXigJlzIG5vIGtub3dpbmcg"
    "d2hlcmUgeW91IG1pZ2h0IGJlIHN3ZXB0IG9mZiB0by4."
    "MRjdkly7_-"
    "oTPTS3AXP41iQIGKa80A0ZmTuV5MEaHoxnW2e5CZ5NlKtainoFmKZopdHM1O2U4mwzJdQx996ivp83xuglII7P"
    "NDi84wnB-BDkoBwA78185hX-Es4JIwmDLJK3lfWRa-XtL0RnltuYv746iYTh_"
    "qHRD68BNt1uSNCrUCTJDt5aAE6x8wW1Kt9e"
    "Ro4QPocSadnHXFxnt8Is9UzpERV0ePPQdLuW3IS_de3xyIrDaLGdjluPxUAhb6L2aXic1U12podGU0KLUQSE_oI-"
    "ZnmKJ3F4uOZDnd6QZWJushZ41Axf_fcIe8u9ipH84ogoree7vjbU5y18kDquDg";

// RFC 7520 §3.2 P-521 key (used by §4.3).
auto cookbook_ec_jwk() -> boost::json::object {
    return boost::json::object{
        {"kty", "EC"},
        {"kid", "bilbo.baggins@hobbiton.example"},
        {"use", "sig"},
        {"crv", "P-521"},
        {"x",
         "AHKZLLOsCOzz5cY97ewNUajB957y-C-U88c3v13nmGZx6sYl_oJXu9A5RkTKqjqvjyekWF-"
         "7ytDyRXYgCF5cj0Kt"},
        {"y",
         "AdymlHvOiLxXkEhayXQnNCvDX4h9htZaCJN34kfmC6pV5OhQHiraVySsUdaQkAgDPrwQrJmbnX9cwlGfP-"
         "HqHZR1"},
        {"d",
         "AAhRON2r9cqXX1hg-RoI6R1tX5p2rUAYdmpHZoC1XNM56KtscrX6zbKipQrCW9CGZH3T4ubpnoTKLDYJ_fF3_"
         "rJt"},
    };
}

constexpr std::string_view cookbook_4_3_compact =
    "eyJhbGciOiJFUzUxMiIsImtpZCI6ImJpbGJvLmJhZ2dpbnNAaG9iYml0b24uZXhhbXBsZSJ9."
    "SXTigJlzIGEgZGFuZ2Vyb3VzIGJ1c2luZXNzLCBGcm9kbywgZ29pbmcgb3V0IHlvdXIgZG9vci4gWW91IHN0ZXAgb2"
    "50byB0aGUgcm9hZCwgYW5kIGlmIHlvdSBkb24ndCBrZWVwIHlvdXIgZmVldCwgdGhlcmXigJlzIG5vIGtub3dpbmcg"
    "d2hlcmUgeW91IG1pZ2h0IGJlIHN3ZXB0IG9mZiB0by4."
    "AE_R_YZCChjn4791jSQCrdPZCNYqHXCTZH0-"
    "JZGYNlaAjP2kqaluUIIUnC9qvbu9Plon7KRTzoNEuT4Va2cmL1eJAQy3mtPB"
    "u_u_sDDyYjnAMDxXPn7XrT0lw-kvAD890jl8e2puQens_IEKBpHABlsbEPX6sFY8OcGDqoRuBomu9xQ2";

// RFC 7515 Appendix A.3 P-256 key and signature.
auto rfc7515_a3_jwk() -> boost::json::object {
    return boost::json::object{
        {"kty", "EC"},
        {"crv", "P-256"},
        {"x", "f83OJ3D2xF1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEU"},
        {"y", "x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0"},
        {"d", "jpsQnnGQmL-YBIffH1136cspYG6-0iY7X1fCE9-E9LI"},
    };
}

auto rfc7515_a3_compact() -> std::string {
    const std::vector<unsigned char> sig = {
        // R
        14, 209, 33, 83, 121, 99, 108, 72, 60, 47, 127, 21, 88, 7, 212, 2, 163, 178, 40, 3, 58, 249,
        124, 126, 23, 129, 154, 195, 22, 158, 166, 101,
        // S
        197, 10, 7, 211, 140, 60, 112, 229, 216, 241, 45, 175, 8, 74, 84, 128, 166, 101, 144, 197,
        242, 147, 80, 154, 143, 63, 127, 138, 131, 163, 84, 213};
    return base64url_encode(std::string_view{R"({"alg":"ES256"})"}) + "." +
           base64url_encode(std::string_view{"{\"iss\":\"joe\",\r\n \"exp\":1300819380,\r\n "
                                             "\"http://example.com/is_root\":true}"}) +
           "." + base64url_encode(sig);
}

auto signing_input_of(std::string_view compact) -> std::string_view {
    return compact.substr(0, compact.rfind('.'));
}

}  // namespace

// ---------------------------------------------------------------------------
// base64url
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(base64url_rfc4648_vectors) {
    // RFC 4648 §10 vectors, unpadded.
    BOOST_TEST(base64url_encode(std::string_view{""}) == "");
    BOOST_TEST(base64url_encode(std::string_view{"f"}) == "Zg");
    BOOST_TEST(base64url_encode(std::string_view{"fo"}) == "Zm8");
    BOOST_TEST(base64url_encode(std::string_view{"foo"}) == "Zm9v");
    BOOST_TEST(base64url_encode(std::string_view{"foob"}) == "Zm9vYg");
    BOOST_TEST(base64url_encode(std::string_view{"fooba"}) == "Zm9vYmE");
    BOOST_TEST(base64url_encode(std::string_view{"foobar"}) == "Zm9vYmFy");
    BOOST_TEST(*base64url_decode_string("Zm9vYmE") == "fooba");
    // The URL-safe alphabet, not + and /.
    const std::vector<unsigned char> high = {0xfb, 0xff, 0xbf};
    BOOST_TEST(base64url_encode(high) == "-_-_");
}

BOOST_AUTO_TEST_CASE(base64url_round_trips_every_length) {
    std::vector<unsigned char> data;
    for (int n = 0; n < 70; ++n) {
        auto decoded = base64url_decode(base64url_encode(data));
        BOOST_REQUIRE(decoded.has_value());
        BOOST_TEST(*decoded == data, boost::test_tools::per_element());
        data.push_back(static_cast<unsigned char>(n * 37 + 11));
    }
}

BOOST_AUTO_TEST_CASE(base64url_decode_is_strict) {
    BOOST_TEST(!base64url_decode("Zm9vY").has_value());     // length % 4 == 1
    BOOST_TEST(!base64url_decode("Zm9vYg==").has_value());  // padding
    BOOST_TEST(!base64url_decode("Zm9v+g").has_value());    // standard alphabet
    BOOST_TEST(!base64url_decode("Zm9v/g").has_value());
    BOOST_TEST(!base64url_decode("Zm9 v").has_value());  // whitespace
    BOOST_TEST(!base64url_decode("Zh").has_value());     // non-zero leftover bits
    BOOST_TEST(!base64url_decode("Zm9").has_value());
    BOOST_TEST(base64url_decode("Zg").has_value());
    BOOST_TEST(base64url_decode("").has_value());
}

// ---------------------------------------------------------------------------
// Algorithms
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(alg_names_round_trip_and_reject_none_and_hmac) {
    for (auto alg : all_algs) {
        BOOST_TEST((parse_alg(alg_name(alg)) == alg));
    }
    BOOST_TEST(!parse_alg("none").has_value());
    BOOST_TEST(!parse_alg("HS256").has_value());
    BOOST_TEST(!parse_alg("rs256").has_value());  // case-sensitive
    BOOST_TEST(!parse_alg("PS256").has_value());
}

// ---------------------------------------------------------------------------
// Known-answer vectors
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(rfc7520_4_1_rs256_signature_is_byte_exact) {
    // PKCS#1 v1.5 is deterministic, so signing the cookbook's signing input
    // with its key must reproduce the published signature exactly.
    auto key = private_key_from_jwk(cookbook_rsa_jwk());
    BOOST_REQUIRE(key);
    const auto input = signing_input_of(cookbook_4_1_compact);
    const auto sig = sign_bytes(input, key.get(), jws_alg::rs256);
    const std::string expected_sig(cookbook_4_1_compact.substr(input.size() + 1));
    BOOST_TEST(base64url_encode(sig) == expected_sig);

    auto verified = verify_compact(cookbook_4_1_compact, key.get(), jws_alg::rs256);
    BOOST_REQUIRE(verified.has_value());
    BOOST_TEST(verified->header.at("kid").as_string() == "bilbo.baggins@hobbiton.example");
    BOOST_TEST(base64url_encode(verified->payload) == cookbook_payload_b64);

    // The public half alone verifies too.
    auto pub = public_key_from_jwk(public_jwk(key.get()));
    BOOST_TEST(verify_compact(cookbook_4_1_compact, pub.get(), jws_alg::rs256).has_value());
}

BOOST_AUTO_TEST_CASE(rfc7520_4_3_es512_verifies) {
    auto key = private_key_from_jwk(cookbook_ec_jwk());
    BOOST_REQUIRE(key);
    BOOST_TEST(verify_compact(cookbook_4_3_compact, key.get(), jws_alg::es512).has_value());

    // P-521 coordinates are 66 bytes, so the exported JWK must keep the
    // leading zero byte of x ("AH..."), matching the cookbook.
    auto jwk = public_jwk(key.get());
    BOOST_TEST(jwk.at("crv").as_string() == "P-521");
    BOOST_TEST(jwk.at("x").as_string() == cookbook_ec_jwk().at("x").as_string());
    BOOST_TEST(jwk.at("y").as_string() == cookbook_ec_jwk().at("y").as_string());
}

BOOST_AUTO_TEST_CASE(rfc7515_a3_es256_verifies) {
    auto key = public_key_from_jwk(rfc7515_a3_jwk());
    BOOST_REQUIRE(key);
    const auto compact = rfc7515_a3_compact();
    BOOST_TEST(compact.substr(0, 20) == "eyJhbGciOiJFUzI1NiJ9");
    auto verified = verify_compact(compact, key.get(), jws_alg::es256);
    BOOST_REQUIRE(verified.has_value());
    BOOST_TEST(verified->payload.find("\"iss\":\"joe\"") != std::string::npos);

    // Flipping one bit of S breaks it.
    auto tampered = compact;
    tampered.back() = tampered.back() == 'A' ? 'B' : 'A';
    BOOST_TEST(!verify_compact(tampered, key.get(), jws_alg::es256).has_value());
}

BOOST_AUTO_TEST_CASE(rfc7638_thumbprints) {
    // RFC 7638 §3.1 (the RFC 7517 Appendix A.1 RSA key).
    const boost::json::object rsa{
        {"kty", "RSA"},
        {"n",
         "0vx7agoebGcQSuuPiLJXZptN9nndrQmbXEps2aiAFbWhM78LhWx4cbbfAAtVT86zwu1RK7aPFFxuhDR1L6tSoc_"
         "BJECPebWKRXjBZCiFV4n3oknjhMstn64tZ_2W-5JsGY4Hc5n9yBXArwl93lqt7_RN5w6Cf0h4QyQ5v-"
         "65YGjQR0_"
         "FDW2QvzqY368QQMicAtaSqzs8KJZgnYb9c7d0zgdAZHzu6qMQvRL5hajrn1n91CbOpbISD08qNLyrdkt-"
         "bFTWhAI4vMQFh6WeZu0fM4lFd2NcRwr3XPksINHaQ-G_xBniIqbw0Ls1jF44-csFCur-kEgU8awapJzKnqDKgw"},
        {"e", "AQAB"},
        {"alg", "RS256"},
        {"kid", "2011-04-29"},
    };
    BOOST_TEST(jwk_thumbprint(rsa) == "NzbLsXh8uDCcd-6MNwXF4W_7noWXFZAfHkxZsRGC9Xs");

    const boost::json::object ec{
        {"kty", "EC"},
        {"crv", "P-256"},
        {"x", "MKBCTNIcKUSDii11ySs3526iDZ8AiTo7Tu6KPAqv7D4"},
        {"y", "4Etl6SRW2YiLUrN5vfvVHuhp7x8PxltmWWlbbM4IFyM"},
        {"use", "enc"},
        {"kid", "1"},
    };
    BOOST_TEST(jwk_thumbprint(ec) == "cn-I_WNMClehiVp51i_0VpOENW1upEerA8sEam5hn-s");

    // A private JWK has the same thumbprint as its public half.
    auto cookbook = cookbook_rsa_jwk();
    auto key = private_key_from_jwk(cookbook);
    BOOST_TEST(jwk_thumbprint(cookbook) == jwk_thumbprint(public_jwk(key.get())));
}

// ---------------------------------------------------------------------------
// Round trips and rejection
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(sign_and_verify_round_trip_every_alg) {
    for (auto alg : all_algs) {
        BOOST_TEST_CONTEXT("alg " << alg_name(alg)) {
            auto key = generate_signing_key(alg);
            BOOST_REQUIRE(key);
            const std::string payload = R"({"sub":"system:serviceaccount:ns:sa"})";
            auto token = sign_compact(boost::json::object{{"kid", "k1"}, {"alg", "none"}}, payload,
                                      key.get(), alg);
            auto parts = parse_compact(token);
            BOOST_REQUIRE(parts.has_value());
            // The caller's alg was overwritten.
            BOOST_TEST(parts->header.at("alg").as_string() == alg_name(alg));
            if (is_ecdsa(alg)) {
                BOOST_TEST(parts->signature.size() == 2 * detail::curve_for(alg)->coord_len);
            }

            auto verified = verify_compact(token, key.get(), alg);
            BOOST_REQUIRE(verified.has_value());
            BOOST_TEST(verified->payload == payload);

            // PEM and JWK round trips keep the key usable.
            auto from_pem = private_key_from_pem(private_key_to_pem(key.get()));
            BOOST_TEST(
                verify_compact(sign_compact({}, payload, from_pem.get(), alg), key.get(), alg)
                    .has_value());
            auto pub = public_key_from_jwk(public_jwk(key.get()));
            BOOST_TEST(verify_compact(token, pub.get(), alg).has_value());
        }
    }
}

BOOST_AUTO_TEST_CASE(algorithm_confusion_is_rejected) {
    auto rsa = generate_signing_key(jws_alg::rs256);
    auto ec = generate_signing_key(jws_alg::es256);
    const auto rs_token = sign_compact({}, "x", rsa.get(), jws_alg::rs256);
    const auto es_token = sign_compact({}, "x", ec.get(), jws_alg::es256);

    // Header alg must be exactly the expected one.
    BOOST_TEST(!verify_compact(rs_token, rsa.get(), jws_alg::rs384).has_value());
    BOOST_TEST(!verify_compact(es_token, ec.get(), jws_alg::es384).has_value());
    // A key of the wrong type never verifies.
    BOOST_TEST(!verify_compact(rs_token, ec.get(), jws_alg::rs256).has_value());
    BOOST_TEST(!verify_compact(es_token, rsa.get(), jws_alg::es256).has_value());
    // Signing with a mismatched key throws instead of producing a token.
    BOOST_CHECK_THROW((void)sign_bytes("x", ec.get(), jws_alg::rs256), std::invalid_argument);
    BOOST_CHECK_THROW((void)sign_bytes("x", ec.get(), jws_alg::es384), std::invalid_argument);

    // An "alg":"none" token with an empty signature is refused.
    const auto none = base64url_encode(std::string_view{R"({"alg":"none"})"}) + "." +
                      base64url_encode(std::string_view{"x"}) + ".";
    BOOST_TEST(!verify_compact(none, rsa.get(), jws_alg::rs256).has_value());
}

BOOST_AUTO_TEST_CASE(weak_rsa_keys_are_refused) {
    auto small =
        evp_pkey_ptr{EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<size_t>(1024))};
    BOOST_REQUIRE(small);
    BOOST_CHECK_THROW((void)sign_bytes("x", small.get(), jws_alg::rs256), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(crit_header_and_malformed_tokens_are_rejected) {
    auto key = generate_signing_key(jws_alg::es256);
    const auto crit = sign_compact(boost::json::object{{"crit", boost::json::array{"exp"}}}, "x",
                                   key.get(), jws_alg::es256);
    BOOST_TEST(!verify_compact(crit, key.get(), jws_alg::es256).has_value());

    const auto good = sign_compact({}, "x", key.get(), jws_alg::es256);
    BOOST_TEST(!parse_compact("a.b").has_value());
    BOOST_TEST(!parse_compact(good + ".extra").has_value());
    BOOST_TEST(!parse_compact("W10.eA.").has_value());   // header is [] not an object
    BOOST_TEST(!parse_compact("e30=.eA.").has_value());  // padded header
    // Truncated ES256 signature.
    BOOST_TEST(
        !verify_compact(good.substr(0, good.size() - 4), key.get(), jws_alg::es256).has_value());
    // Payload swapped after signing.
    auto parts = parse_compact(good);
    BOOST_REQUIRE(parts.has_value());
    const auto swapped = good.substr(0, good.find('.')) + "." +
                         base64url_encode(std::string_view{"y"}) + good.substr(good.rfind('.'));
    BOOST_TEST(!verify_compact(swapped, key.get(), jws_alg::es256).has_value());
}

BOOST_AUTO_TEST_CASE(jwk_import_rejects_bad_input) {
    // Unknown kty, unknown curve, missing member, and a point not on the curve.
    BOOST_CHECK_THROW((void)public_key_from_jwk({{"kty", "oct"}, {"k", "AA"}}),
                      std::invalid_argument);
    auto bad_crv = rfc7515_a3_jwk();
    bad_crv["crv"] = "P-999";
    BOOST_CHECK_THROW((void)public_key_from_jwk(bad_crv), std::invalid_argument);
    auto missing = rfc7515_a3_jwk();
    missing.erase("y");
    BOOST_CHECK_THROW((void)public_key_from_jwk(missing), std::invalid_argument);
    auto off_curve = rfc7515_a3_jwk();
    off_curve["y"] = off_curve["x"];
    BOOST_CHECK_THROW((void)public_key_from_jwk(off_curve), std::exception);
}
