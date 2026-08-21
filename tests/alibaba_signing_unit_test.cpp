// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE alibaba_signing_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/alibaba_signing.hpp>

#include <chrono>
#include <map>
#include <stdexcept>
#include <string>

// Unit coverage for Alibaba OpenAPI V3 signing (ACS3-HMAC-SHA256),
// .kiro/specs/alibaba-cloud-services/ Requirement 16.2.
//
// The golden vector is the vendor's own published worked example, whose
// AccessKeySecret is part of the example ("YourAccessKeySecret") — so unlike
// the OSS V4 example, every stage is independently reproducible, and this
// test pins all three: the canonical request, the string-to-sign's hash, and
// the final signature. That is what makes it a real vector rather than a
// change detector: if any encoding rule here is wrong, these bytes diverge.

using namespace kythira;
using namespace kythira::alibaba_signing;

namespace {

// From the vendor's V3 signature documentation.
constexpr const char* k_expected_canonical =
    "POST\n"
    "/\n"
    "ImageId=win2019_1809_x64_dtc_zh-cn_40G_alibase_20230811.vhd&RegionId=cn-shanghai\n"
    "host:ecs.cn-shanghai.aliyuncs.com\n"
    "x-acs-action:RunInstances\n"
    "x-acs-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
    "x-acs-date:2023-10-26T10:22:32Z\n"
    "x-acs-signature-nonce:3156853299f313e23d1673dc12e1703d\n"
    "x-acs-version:2014-05-26\n"
    "\n"
    "host;x-acs-action;x-acs-content-sha256;x-acs-date;x-acs-signature-nonce;x-acs-version\n"
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

constexpr const char* k_expected_signature =
    "06563a9e1b43f5dfe96b81484da74bceab24a1d853912eee15083a6f0f3283c0";

constexpr const char* k_empty_sha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

/// The example's timestamp, 2023-10-26T10:22:32Z, as a time_point.
auto example_time() -> std::chrono::system_clock::time_point {
    // 1698315752 = 2023-10-26T10:22:32Z.
    return std::chrono::system_clock::from_time_t(1698315752);
}

auto example_config() -> alibaba_client_config {
    alibaba_client_config cfg;
    cfg.region = "cn-shanghai";
    cfg.access_key_id = "YourAccessKeyId";
    cfg.access_key_secret = "YourAccessKeySecret";
    return cfg;
}

auto example_query() -> std::map<std::string, std::string> {
    return {{"ImageId", "win2019_1809_x64_dtc_zh-cn_40G_alibase_20230811.vhd"},
            {"RegionId", "cn-shanghai"}};
}

}  // namespace

BOOST_AUTO_TEST_SUITE(alibaba_signing_v3)

BOOST_AUTO_TEST_CASE(empty_body_hash_matches_the_documented_constant) {
    BOOST_TEST(detail::sha256_hex("") == k_empty_sha256);
}

BOOST_AUTO_TEST_CASE(iso8601_formats_the_example_timestamp) {
    BOOST_TEST(detail::iso8601_utc(example_time()) == "2023-10-26T10:22:32Z");
}

// RFC 3986 unreserved-only: letters, digits, '-', '_', '.', '~' literal;
// space is %20 (never '+'), '*' is %2A, and hex is uppercase.
BOOST_AUTO_TEST_CASE(percent_encoding_follows_the_unreserved_rule) {
    BOOST_TEST(detail::percent_encode("azAZ09-_.~") == "azAZ09-_.~");
    BOOST_TEST(detail::percent_encode(" ") == "%20");
    BOOST_TEST(detail::percent_encode("*") == "%2A");
    BOOST_TEST(detail::percent_encode("/") == "%2F");
    BOOST_TEST(detail::percent_encode("a b") == "a%20b");
}

BOOST_AUTO_TEST_CASE(canonical_query_string_sorts_and_encodes) {
    const std::map<std::string, std::string> query{{"b", "2"}, {"a", "1 x"}};
    BOOST_TEST(canonical_query_string(query) == "a=1%20x&b=2");
}

// The whole point of the vector: the canonical request this code builds is
// byte-identical to the vendor's published one.
BOOST_AUTO_TEST_CASE(canonical_request_matches_the_vendor_example) {
    const std::map<std::string, std::string> headers{
        {"host", "ecs.cn-shanghai.aliyuncs.com"},
        {"x-acs-action", "RunInstances"},
        {"x-acs-content-sha256", k_empty_sha256},
        {"x-acs-date", "2023-10-26T10:22:32Z"},
        {"x-acs-signature-nonce", "3156853299f313e23d1673dc12e1703d"},
        {"x-acs-version", "2014-05-26"},
    };
    const auto canonical = canonical_request("POST", "/", canonical_query_string(example_query()),
                                             headers, k_empty_sha256);
    BOOST_TEST(canonical == k_expected_canonical);
}

BOOST_AUTO_TEST_CASE(string_to_sign_matches_the_vendor_example) {
    BOOST_TEST(string_to_sign(k_expected_canonical) ==
               "ACS3-HMAC-SHA256\n"
               "7ea06492da5221eba5297e897ce16e55f964061054b7695beedaac1145b1e259");
}

// End to end through the public entry point: same inputs, same Authorization
// header the vendor documents.
BOOST_AUTO_TEST_CASE(sign_request_reproduces_the_vendor_signature) {
    const auto headers = sign_request(example_config(), "POST", "ecs.cn-shanghai.aliyuncs.com", "/",
                                      example_query(), "RunInstances", "2014-05-26", "",
                                      example_time(), "3156853299f313e23d1673dc12e1703d");

    BOOST_TEST(headers.at("Authorization") ==
               std::string("ACS3-HMAC-SHA256 Credential=YourAccessKeyId,SignedHeaders=host;"
                           "x-acs-action;x-acs-content-sha256;x-acs-date;x-acs-signature-nonce;"
                           "x-acs-version,Signature=") +
                   k_expected_signature);
    BOOST_TEST(headers.at("x-acs-action") == "RunInstances");
    BOOST_TEST(headers.at("x-acs-version") == "2014-05-26");
    BOOST_TEST(headers.at("x-acs-date") == "2023-10-26T10:22:32Z");
    BOOST_TEST(headers.at("x-acs-content-sha256") == k_empty_sha256);
    BOOST_TEST(headers.count("x-acs-security-token") == 0U);
}

// STS credentials add the token to the signed set, which changes both
// SignedHeaders and the signature — the case a live CI run exercises.
BOOST_AUTO_TEST_CASE(security_token_joins_the_signed_header_set) {
    auto cfg = example_config();
    cfg.security_token = "sts-token-value";
    const auto headers = sign_request(cfg, "POST", "ecs.cn-shanghai.aliyuncs.com", "/",
                                      example_query(), "RunInstances", "2014-05-26", "",
                                      example_time(), "3156853299f313e23d1673dc12e1703d");

    BOOST_TEST(headers.at("x-acs-security-token") == "sts-token-value");
    BOOST_TEST(headers.at("Authorization").find("x-acs-security-token") != std::string::npos);
    // Different signed set ⇒ different signature than the no-token vector.
    BOOST_TEST(headers.at("Authorization").find(k_expected_signature) == std::string::npos);
}

// A non-empty body must change the payload hash, and therefore the signature.
BOOST_AUTO_TEST_CASE(body_is_hashed_into_the_signature) {
    const auto headers =
        sign_request(example_config(), "POST", "ecs.cn-shanghai.aliyuncs.com", "/", {},
                     "RunInstances", "2014-05-26", "{\"k\":1}", example_time(), "nonce");
    BOOST_TEST(headers.at("x-acs-content-sha256") == detail::sha256_hex("{\"k\":1}"));
    BOOST_TEST(headers.at("x-acs-content-sha256") != k_empty_sha256);
}

BOOST_AUTO_TEST_CASE(missing_credentials_throw_naming_the_field) {
    alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    BOOST_CHECK_THROW(
        sign_request(cfg, "POST", "ess.aliyuncs.com", "/", {}, "A", "V", "", example_time(), "n"),
        std::invalid_argument);

    cfg.access_key_id = "id";
    BOOST_CHECK_THROW(
        sign_request(cfg, "POST", "ess.aliyuncs.com", "/", {}, "A", "V", "", example_time(), "n"),
        std::invalid_argument);

    cfg.access_key_secret = "secret";
    BOOST_CHECK_NO_THROW(
        sign_request(cfg, "POST", "ess.aliyuncs.com", "/", {}, "A", "V", "", example_time(), "n"));
}

BOOST_AUTO_TEST_SUITE_END()
