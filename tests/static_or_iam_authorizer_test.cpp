// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The static-or-IAM authorizer (`include/raft/static_or_iam_authorizer.hpp`,
// DynamoDB spec task 28, reused by Kinesis): each mode routes requests to
// the static keys or to IAM, `both` splits them by access key id, and a key
// id both know is refused at start and per request.

#define BOOST_TEST_MODULE static_or_iam_authorizer_test

#include <boost/test/unit_test.hpp>

#include <raft/static_or_iam_authorizer.hpp>

#include <chrono>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

// IAM as hooks: it knows AKIDIAM and whatever ids `known` holds, signs
// every key in with the same secret, and allows only GetItem.
struct fake_iam {
    std::shared_ptr<std::set<std::string>> known = std::make_shared<std::set<std::string>>();
    std::shared_ptr<int> calls = std::make_shared<int>(0);

    [[nodiscard]] auto hooks() const -> az::iam_hooks {
        az::iam_hooks h;
        h.authenticate = [known = known, calls = calls](
                             const sv4::request& req,
                             const sv4::service_rules& r) -> az::authn_result {
            ++*calls;
            const auto parsed = sv4::parse(req, r, k_now);
            if (!parsed) {
                return parsed.error();
            }
            if (parsed->access_key_id != "AKIDIAM" && !known->contains(parsed->access_key_id)) {
                return sv4::invalid_client_token_id(r.protocol);
            }
            az::principal_context p;
            p.account = "444455556666";
            p.access_key_id = parsed->access_key_id;
            p.arn = "arn:aws:iam::444455556666:user/from-iam";
            return p;
        };
        h.authorize = [](const az::principal_context&, const az::authz_request& r) {
            az::authz_decision d;
            d.allowed = r.action == "dynamodb:GetItem";
            return d;
        };
        h.has_key = [known = known](std::string_view id) {
            return id == "AKIDIAM" || known->contains(std::string(id));
        };
        return h;
    }
};

auto make_authorizer() -> std::unique_ptr<az::static_or_iam_authorizer> {
    auto a = std::make_unique<az::static_or_iam_authorizer>(
        std::vector<az::static_key>{
            {"AKIDADMIN", std::string(k_secret), "111122223333", "ops", az::static_role::admin}},
        "aws", [] { return std::chrono::system_clock::time_point(k_now); });
    a->register_service({"dynamodb", {"Get*"}, {"PutItem"}});
    return a;
}

auto action(std::string name) -> az::authz_request {
    az::authz_request r;
    r.action = std::move(name);
    r.resource = "arn:aws:dynamodb:us-east-1:111122223333:table/t";
    return r;
}

}  // namespace

BOOST_AUTO_TEST_CASE(static_mode_uses_only_the_keys) {
    auto a = make_authorizer();
    BOOST_TEST((a->mode() == az::auth_mode::static_keys));
    const auto who = a->authenticate(signed_request("AKIDADMIN").view(), rules());
    BOOST_REQUIRE(who.has_value());
    BOOST_TEST(a->authorize(*who, action("dynamodb:PutItem")).allowed);
    BOOST_TEST(!a->authenticate(signed_request("AKIDIAM").view(), rules()).has_value());
}

BOOST_AUTO_TEST_CASE(iam_and_both_need_iam) {
    auto a = make_authorizer();
    BOOST_CHECK_THROW(a->set_mode(az::auth_mode::iam, std::nullopt), std::invalid_argument);
    BOOST_CHECK_THROW(a->set_mode(az::auth_mode::both, std::nullopt), std::invalid_argument);
    BOOST_TEST((a->mode() == az::auth_mode::static_keys));
}

BOOST_AUTO_TEST_CASE(iam_mode_sends_everything_to_iam) {
    auto a = make_authorizer();
    fake_iam iam;
    a->set_mode(az::auth_mode::iam, iam.hooks());
    // A static key's id means nothing to IAM.
    BOOST_TEST(!a->authenticate(signed_request("AKIDADMIN").view(), rules()).has_value());
    const auto who = a->authenticate(signed_request("AKIDIAM").view(), rules());
    BOOST_REQUIRE(who.has_value());
    BOOST_TEST(who->account == "444455556666");
    BOOST_TEST(a->authorize(*who, action("dynamodb:GetItem")).allowed);
    BOOST_TEST(!a->authorize(*who, action("dynamodb:PutItem")).allowed);
    BOOST_TEST(*iam.calls == 2);
}

BOOST_AUTO_TEST_CASE(both_splits_by_access_key_id) {
    auto a = make_authorizer();
    fake_iam iam;
    a->set_mode(az::auth_mode::both, iam.hooks());
    BOOST_TEST(a->conflicting_keys().empty());

    const auto ops = a->authenticate(signed_request("AKIDADMIN").view(), rules());
    BOOST_REQUIRE(ops.has_value());
    BOOST_TEST(ops->account == "111122223333");
    BOOST_TEST(*iam.calls == 0);  // a static key never reaches IAM
    BOOST_TEST(a->authorize(*ops, action("dynamodb:PutItem")).allowed);  // admin role

    const auto user = a->authenticate(signed_request("AKIDIAM").view(), rules());
    BOOST_REQUIRE(user.has_value());
    BOOST_TEST(user->account == "444455556666");
    BOOST_TEST(!a->authorize(*user, action("dynamodb:PutItem")).allowed);  // IAM's policy
    BOOST_TEST(a->authorize(*user, action("dynamodb:GetItem")).allowed);

    // A wrong secret on a static key is the static authorizer's refusal.
    const auto wrong =
        a->authenticate(signed_request("AKIDADMIN", "not-the-secret").view(), rules());
    BOOST_REQUIRE(!wrong.has_value());
    BOOST_TEST(wrong.error().code == sv4::signature_mismatch(sv4::error_protocol::json).code);
}

BOOST_AUTO_TEST_CASE(a_key_id_both_know_is_refused) {
    auto a = make_authorizer();
    fake_iam iam;
    iam.known->insert("AKIDADMIN");
    a->set_mode(az::auth_mode::both, iam.hooks());
    BOOST_TEST(a->conflicting_keys() == std::vector<std::string>{"AKIDADMIN"});

    // Made in IAM after start: refused at each request, by either secret.
    auto b = make_authorizer();
    fake_iam later;
    b->set_mode(az::auth_mode::both, later.hooks());
    BOOST_TEST(b->conflicting_keys().empty());
    BOOST_TEST(b->authenticate(signed_request("AKIDADMIN").view(), rules()).has_value());
    later.known->insert("AKIDADMIN");
    const auto refused = b->authenticate(signed_request("AKIDADMIN").view(), rules());
    BOOST_REQUIRE(!refused.has_value());
    BOOST_TEST(refused.error().code == "UnrecognizedClientException");
    BOOST_TEST(*later.calls == 0);

    // Static mode never asks IAM.
    BOOST_TEST(make_authorizer()->conflicting_keys().empty());
}
