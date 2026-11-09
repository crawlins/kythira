// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The static access-key authorizer (`include/raft/static_key_authorizer.hpp`,
// DynamoDB spec Requirement 5.3, reused by Kinesis): SigV4 against the key
// table, the three roles over per-service pattern tables, refusal of
// unregistered services, temporary credentials and cross-account access.

#define BOOST_TEST_MODULE static_key_authorizer_test

#include <boost/test/unit_test.hpp>

#include <raft/static_key_authorizer.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sv4 = kythira::sigv4;
namespace az = kythira::aws_auth;

namespace {

constexpr std::string_view k_secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
const auto k_now = sv4::detail::parse_amz_date("20261109T120000Z").value();

struct http_request {
    std::string method = "POST";
    std::string path = "/";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body = "{}";

    [[nodiscard]] auto view() const -> sv4::request { return {method, path, "", headers, body}; }
};

auto rules() -> sv4::service_rules {
    sv4::service_rules r;
    r.service = "dynamodb";
    r.protocol = sv4::error_protocol::json;
    return r;
}

// A DynamoDB-shaped request signed by `akid` with `secret`.
auto signed_request(std::string_view akid, std::string_view secret = k_secret) -> http_request {
    http_request r;
    r.headers = {{"Host", "localhost:8000"},
                 {"Content-Type", "application/x-amz-json-1.0"},
                 {"X-Amz-Date", "20261109T120000Z"},
                 {"X-Amz-Target", "DynamoDB_20120810.ListTables"}};
    const sv4::credential_scope scope{"20261109", "us-east-1", "dynamodb"};
    r.headers.emplace_back("Authorization",
                           "AWS4-HMAC-SHA256 Credential=" + std::string(akid) + "/" + scope.text() +
                               ", SignedHeaders=content-type;host;x-amz-date;x-amz-target"
                               ", Signature=" +
                               std::string(64, '0'));
    const auto parsed = sv4::parse(r.view(), rules(), k_now);
    BOOST_REQUIRE_MESSAGE(parsed.has_value(), parsed.error().message);
    const auto sig = sv4::compute_signature(*parsed, sv4::derive_signing_key(secret, scope));
    r.headers.back().second.replace(r.headers.back().second.size() - 64, 64, sig);
    return r;
}

// Not copyable (it holds a mutex), so handed out by pointer.
auto make_authorizer() -> std::unique_ptr<az::static_key_authorizer> {
    auto a = std::make_unique<az::static_key_authorizer>(
        std::vector<az::static_key>{
            {"AKIDREAD", std::string(k_secret), "111122223333", "reader",
             az::static_role::read_only},
            {"AKIDWRITE", std::string(k_secret), "111122223333", "", az::static_role::read_write},
            {"AKIDADMIN", std::string(k_secret), "111122223333", "ops", az::static_role::admin},
        },
        "aws", [] { return std::chrono::system_clock::time_point(k_now); });
    a->register_service({"dynamodb",
                         {"Get*", "BatchGetItem", "Query", "Scan", "Describe*", "List*"},
                         {"PutItem", "UpdateItem", "DeleteItem", "BatchWriteItem"}});
    return a;
}

auto allowed(az::static_key_authorizer& a, std::string_view akid, std::string action) -> bool {
    const auto who = a.authenticate(signed_request(akid).view(), rules());
    BOOST_REQUIRE_MESSAGE(who.has_value(), who.error().code);
    az::authz_request r;
    r.action = std::move(action);
    r.resource = "arn:aws:dynamodb:us-east-1:111122223333:table/t";
    return a.authorize(*who, r).allowed;
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_configured_key_authenticates_as_an_iam_user) {
    auto ap = make_authorizer();
    auto& a = *ap;
    const auto who = a.authenticate(signed_request("AKIDREAD").view(), rules());
    BOOST_REQUIRE(who.has_value());
    BOOST_TEST(who->account == "111122223333");
    BOOST_TEST(who->arn == "arn:aws:iam::111122223333:user/reader");
    BOOST_TEST(who->access_key_id == "AKIDREAD");
    BOOST_TEST((who->type == az::principal_type::user));
    // No name: the key id stands in.
    BOOST_TEST(a.authenticate(signed_request("AKIDWRITE").view(), rules())->username ==
               "AKIDWRITE");
}

BOOST_AUTO_TEST_CASE(unknown_keys_bad_signatures_and_sessions_are_refused) {
    auto ap = make_authorizer();
    auto& a = *ap;
    const auto unknown = a.authenticate(signed_request("AKIDNOBODY").view(), rules());
    BOOST_REQUIRE(!unknown.has_value());
    BOOST_TEST(unknown.error().code == "UnrecognizedClientException");

    const auto wrong = a.authenticate(signed_request("AKIDREAD", "not-the-secret").view(), rules());
    BOOST_REQUIRE(!wrong.has_value());
    BOOST_TEST(wrong.error().code == sv4::signature_mismatch(sv4::error_protocol::json).code);

    auto tampered = signed_request("AKIDREAD");
    tampered.body = R"({"Limit":1})";
    BOOST_TEST(!a.authenticate(tampered.view(), rules()).has_value());

    const auto session = a.authenticate(signed_request("ASIATEMPORARY").view(), rules());
    BOOST_REQUIRE(!session.has_value());
    BOOST_TEST(session.error().code == "UnrecognizedClientException");
}

BOOST_AUTO_TEST_CASE(roles_grant_their_patterns) {
    auto ap = make_authorizer();
    auto& a = *ap;
    BOOST_TEST(allowed(a, "AKIDREAD", "dynamodb:GetItem"));
    BOOST_TEST(allowed(a, "AKIDREAD", "dynamodb:DescribeTable"));
    BOOST_TEST(allowed(a, "AKIDREAD", "DynamoDB:query"));  // case-insensitive
    BOOST_TEST(!allowed(a, "AKIDREAD", "dynamodb:PutItem"));
    BOOST_TEST(!allowed(a, "AKIDREAD", "dynamodb:CreateTable"));

    BOOST_TEST(allowed(a, "AKIDWRITE", "dynamodb:GetItem"));
    BOOST_TEST(allowed(a, "AKIDWRITE", "dynamodb:PutItem"));
    BOOST_TEST(!allowed(a, "AKIDWRITE", "dynamodb:CreateTable"));
    BOOST_TEST(!allowed(a, "AKIDWRITE", "dynamodb:DeleteTable"));

    BOOST_TEST(allowed(a, "AKIDADMIN", "dynamodb:CreateTable"));
    BOOST_TEST(allowed(a, "AKIDADMIN", "dynamodb:DeleteTable"));
}

BOOST_AUTO_TEST_CASE(unregistered_services_grant_nothing_even_to_admin) {
    auto ap = make_authorizer();
    auto& a = *ap;
    BOOST_TEST(!allowed(a, "AKIDADMIN", "kinesis:PutRecord"));
    BOOST_TEST(!allowed(a, "AKIDADMIN", "not-an-action"));
    a.register_service({"kinesis", {"Describe*", "Get*", "List*"}, {"PutRecord", "PutRecords"}});
    BOOST_TEST(allowed(a, "AKIDADMIN", "kinesis:PutRecord"));
    BOOST_TEST(allowed(a, "AKIDWRITE", "kinesis:PutRecords"));
    BOOST_TEST(!allowed(a, "AKIDREAD", "kinesis:PutRecord"));
    BOOST_TEST(allowed(a, "AKIDREAD", "kinesis:GetRecords"));
}

BOOST_AUTO_TEST_CASE(cross_account_and_forged_principals_are_denied) {
    auto ap = make_authorizer();
    auto& a = *ap;
    const auto who = a.authenticate(signed_request("AKIDADMIN").view(), rules());
    BOOST_REQUIRE(who.has_value());
    az::authz_request r;
    r.action = "dynamodb:GetItem";
    r.resource_account = "999988887777";
    BOOST_TEST(!a.authorize(*who, r).allowed);

    auto forged = *who;
    forged.account = "999988887777";
    r.resource_account.clear();
    BOOST_TEST(!a.authorize(forged, r).allowed);
}

BOOST_AUTO_TEST_CASE(bad_configuration_is_refused) {
    using keys = std::vector<az::static_key>;
    BOOST_CHECK_THROW(az::static_key_authorizer(keys{{"", "s", "1", "", az::static_role::admin}}),
                      std::invalid_argument);
    BOOST_CHECK_THROW(az::static_key_authorizer(keys{{"A", "s", "1", "", az::static_role::admin},
                                                     {"A", "t", "1", "", az::static_role::admin}}),
                      std::invalid_argument);
    auto ap = make_authorizer();
    auto& a = *ap;
    BOOST_CHECK_THROW(a.register_service({"dynamodb", {"Get*"}, {}}), std::invalid_argument);
    BOOST_CHECK_THROW(a.register_service({"Bad Prefix", {}, {}}), std::invalid_argument);
    BOOST_CHECK_THROW(a.register_service({"s3", {""}, {}}), std::invalid_argument);
    // The refused s3 table left nothing behind.
    BOOST_TEST(!allowed(a, "AKIDADMIN", "s3:GetObject"));
}

BOOST_AUTO_TEST_CASE(action_globs_match_like_iam) {
    namespace d = az::detail;
    BOOST_TEST(d::glob_match("*", ""));
    BOOST_TEST(d::glob_match("get*", "getitem"));
    BOOST_TEST(d::glob_match("get*", "get"));
    BOOST_TEST(!d::glob_match("get*", "batchgetitem"));
    BOOST_TEST(d::glob_match("*item", "batchgetitem"));
    BOOST_TEST(d::glob_match("describe*s", "describelimits"));
    BOOST_TEST(d::glob_match("g?titem", "getitem"));
    BOOST_TEST(!d::glob_match("g?titem", "gtitem"));
    BOOST_TEST(d::glob_match("*a*b*", "xxaxxbxx"));
    BOOST_TEST(!d::glob_match("*a*b", "xxaxxbxx"));
}
