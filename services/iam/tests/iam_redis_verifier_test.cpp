// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// iam_redis_verifier (.kiro/specs/iam-compatible-auth/ task 21,
// Requirement 13): ElastiCache IAM auth tokens checked against a local IAM
// state machine, as the Redis gateway's AUTH asks.
//
//  - a token botocore made the way the ElastiCache documentation shows
//    (tools/gen_redis_iam_token.py) is accepted;
//  - a wrong user, another cache, an expired token, one presigned for too
//    long, an unknown key, a bad signature, a stale replica and a policy
//    that does not allow elasticache:Connect on both ARNs are all refused;
//  - temporary credentials are accepted until the earlier of 12 hours and
//    the session's own expiry.

#define BOOST_TEST_MODULE iam_redis_verifier_test
#include <boost/test/unit_test.hpp>

#include <kythira/iam/iam_authorizer.hpp>
#include <kythira/iam/iam_redis_verifier.hpp>
#include <kythira/iam/iam_service_registry.hpp>
#include <kythira/iam/iam_state_machine.hpp>
#include <kythira/iam/iam_sts_token.hpp>
#include <kythira/iam/services/elasticache_actions.hpp>

#include "iam_redis_token_test_support.hpp"

#include <raft/jwt.hpp>
#include <raft/sigv4_verifier.hpp>

#include <boost/json.hpp>

#include <array>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <variant>

namespace iam = kythira::iam;
namespace sigv4 = kythira::sigv4;
namespace sts = kythira::iam::sts;
namespace leader = kythira::iam::iam_leader;
using iam::iam_command;
using namespace std::chrono_literals;
using iam_redis_testing::presign;

namespace {

constexpr std::string_view k_account = "111122223333";
constexpr std::string_view k_cache = "kythira-cache";
constexpr std::string_view k_region = "us-east-1";

// tools/gen_redis_iam_token.py: botocore 1.35.0's RequestSigner, presigned
// at 2026-11-09T12:00:00Z for 900 s with a key that exists only here.
constexpr std::string_view k_botocore_key_id = "AKIAKYTHIRAREDIS2345";
constexpr std::string_view k_botocore_secret = "kythira/redis/test/secret/not/a/real/key";
constexpr std::string_view k_botocore_token =
    "kythira-cache/?Action=connect&User=farm&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential="
    "AKIAKYTHIRAREDIS2345%2F20261109%2Fus-east-1%2Felasticache%2Faws4_request&X-Amz-Date="
    "20261109T120000Z&X-Amz-Expires=900&X-Amz-SignedHeaders=host&X-Amz-Signature="
    "82bf73191a0939350a0ce9bcbee3d090ba0ad8f51bec87cc07c993f792b3d8a4";
constexpr std::int64_t k_signed_at = 1'794'225'600;  // 2026-11-09T12:00:00Z

auto test_kek() -> iam::seal_key {
    std::array<unsigned char, iam::seal_key::size> b{};
    b.fill(0x42);
    return iam::seal_key(b);
}

auto error_of(const std::vector<std::byte>& v) -> std::string {
    const std::string text(reinterpret_cast<const char*>(v.data()), v.size());
    const auto o = boost::json::parse(text).as_object();
    const auto* e = o.if_contains("error");
    return e == nullptr ? std::string() : std::string(e->as_string());
}

/// The policy that lets a principal connect to the cache as `user`.
auto connect_policy(const std::string& user = "farm") -> std::string {
    return R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"elasticache:Connect",)"
           R"("Resource":["arn:aws:elasticache:us-east-1:111122223333:replicationgroup:kythira-cache",)"
           R"("arn:aws:elasticache:us-east-1:111122223333:user:)" +
           user + R"("]}]})";
}

struct fixture {
    iam::iam_state_machine sm{iam::iam_config{}};
    std::shared_mutex mu;
    std::uint64_t index = 0;
    std::int64_t now = k_signed_at;
    iam::seal_key kek = test_kek();
    iam::iam_service_registry registry;
    std::chrono::system_clock::time_point clock{std::chrono::seconds{k_signed_at + 300}};
    iam::iam_authorizer authz{
        [this](const std::function<void(const iam::iam_state_machine&)>& body) {
            const std::shared_lock lock(mu);
            body(sm);
        },
        [this](std::string_view) -> const iam::seal_key& { return kek; }, registry, nullptr,
        [this] { return clock; }};
    iam::iam_redis_verifier verifier{authz,
                                     {.account = std::string(k_account),
                                      .region = std::string(k_region),
                                      .cache_name = std::string(k_cache)},
                                     [this] { return clock; }};

    fixture() {
        registry.add(iam::services::elasticache_actions());
        run(iam_command::create_account,
            {{"account", k_account}, {"kek_salt", "c2FsdHNhbHRzYWx0c2FsdA"}});
        run(iam_command::create_user, {{"name", "redis-client"}, {"id", "AIDAREDISCLIENT00001"}});
        run(iam_command::create_access_key,
            {{"user", "redis-client"},
             {"key_id", k_botocore_key_id},
             {"sealed_secret", iam::seal_secret(kek, std::string(k_botocore_secret),
                                                std::string(k_botocore_key_id))}});
    }

    void run(iam_command type, boost::json::object body) {
        if (!body.contains("account")) {
            body["account"] = k_account;
        }
        const std::unique_lock lock(mu);
        const auto r = sm.apply(leader::command(type, std::move(body), ++now), ++index);
        BOOST_TEST_REQUIRE(error_of(r).empty(), error_of(r));
    }

    void allow(const std::string& document, const std::string& user = "redis-client") {
        run(iam_command::put_inline_policy,
            {{"kind", "user"}, {"name", user}, {"policy_name", "redis"}, {"document", document}});
    }

    auto check(std::string_view token, std::string_view user = "farm")
        -> kythira::credential_result {
        return verifier.check({"redis", user, token, "198.51.100.7"});
    }
};

auto denied(const kythira::credential_result& r) -> std::string {
    const auto* d = std::get_if<kythira::credential_denied>(&r);
    return d == nullptr ? std::string() : d->reason;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(redis_auth_tokens)

// Requirement 13.2: the token an AWS SDK makes, as the ElastiCache
// documentation builds it, is accepted, and the connection it opens lasts
// 12 hours.
BOOST_FIXTURE_TEST_CASE(inv_redis_1_a_botocore_token_is_accepted, fixture) {
    allow(connect_policy());
    const auto r = check(k_botocore_token);
    const auto* ok = std::get_if<kythira::credential_accepted>(&r);
    BOOST_TEST_REQUIRE(ok != nullptr, denied(r));
    BOOST_TEST(ok->principal == "arn:aws:iam::111122223333:user/redis-client");
    BOOST_TEST((ok->expires == clock + 12h));
    // S3's UNSIGNED-PAYLOAD form of the same request is accepted too.
    BOOST_TEST(denied(check(presign(k_botocore_key_id, k_botocore_secret, k_signed_at, "farm", 900,
                                    k_cache, {}, true)))
                   .empty());
    // The same token through the concept's asynchronous form.
    auto f = verifier.verify({"redis", "farm", k_botocore_token, "198.51.100.7"});
    const auto answer = std::move(f).get();
    BOOST_TEST(std::holds_alternative<kythira::credential_accepted>(answer));
}

// Requirement 13.2: the token names its user and its cache, and lives at
// most 15 minutes from its date.
BOOST_FIXTURE_TEST_CASE(inv_redis_2_a_token_is_bound_to_its_user_cache_and_lifetime, fixture) {
    allow(connect_policy());
    allow(connect_policy("other"));
    BOOST_TEST(!denied(check(k_botocore_token, "other")).empty());
    // The user the token names must be the AUTH user even when both may
    // connect.
    BOOST_TEST(!denied(check(presign(k_botocore_key_id, k_botocore_secret, k_signed_at, "other")))
                    .empty());
    BOOST_TEST(
        denied(check(presign(k_botocore_key_id, k_botocore_secret, k_signed_at, "other"), "other"))
            .empty());
    BOOST_TEST(denied(check(presign(k_botocore_key_id, k_botocore_secret, k_signed_at, "farm", 900,
                                    "another-cache")))
                   .find("another cache") != std::string::npos);
    BOOST_TEST(
        denied(check(presign(k_botocore_key_id, k_botocore_secret, k_signed_at, "farm", 3600)))
            .find("15 minutes") != std::string::npos);
    clock = std::chrono::system_clock::time_point{std::chrono::seconds{k_signed_at + 901}};
    BOOST_TEST(!denied(check(k_botocore_token)).empty());
    // Not a token at all.
    BOOST_TEST(!denied(check("farm-secret")).empty());
    BOOST_TEST(!denied(check("/?Action=connect&User=farm")).empty());
}

// Requirement 14.3: an unknown key and a bad signature are both refused.
BOOST_FIXTURE_TEST_CASE(inv_redis_3_an_unknown_key_or_bad_signature_is_refused, fixture) {
    allow(connect_policy());
    BOOST_TEST(
        !denied(check(presign("AKIAKYTHIRAREDIS7777", k_botocore_secret, k_signed_at))).empty());
    BOOST_TEST(!denied(check(presign(k_botocore_key_id, "not-the-secret", k_signed_at))).empty());
    auto tampered = std::string(k_botocore_token);
    tampered.back() = tampered.back() == '4' ? '5' : '4';
    BOOST_TEST(!denied(check(tampered)).empty());
}

// Requirement 13.3: elasticache:Connect must be allowed on the user ARN and
// on the replication group ARN.
BOOST_FIXTURE_TEST_CASE(inv_redis_4_connect_is_authorized_on_both_arns, fixture) {
    BOOST_TEST(denied(check(k_botocore_token)).find("elasticache:Connect denied") !=
               std::string::npos);
    allow(
        R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"elasticache:Connect",)"
        R"("Resource":"arn:aws:elasticache:us-east-1:111122223333:user:farm"}]})");
    BOOST_TEST(denied(check(k_botocore_token)).find("replicationgroup:kythira-cache") !=
               std::string::npos);
    allow(
        R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"elasticache:Connect",)"
        R"("Resource":"arn:aws:elasticache:us-east-1:111122223333:replicationgroup:kythira-cache"}]})");
    BOOST_TEST(denied(check(k_botocore_token)).find("user:farm") != std::string::npos);
    allow(connect_policy());
    BOOST_TEST(denied(check(k_botocore_token)).empty());
    // An explicit deny on either wins.
    run(iam_command::put_inline_policy,
        {{"kind", "user"},
         {"name", "redis-client"},
         {"policy_name", "no-farm"},
         {"document",
          R"({"Version":"2012-10-17","Statement":[{"Effect":"Deny","Action":"elasticache:Connect",)"
          R"("Resource":"arn:aws:elasticache:*:*:user:farm"}]})"}});
    BOOST_TEST(!denied(check(k_botocore_token)).empty());
}

// Requirement 7.4: a replica past its staleness bound fails closed.
BOOST_FIXTURE_TEST_CASE(inv_redis_5_a_stale_replica_refuses, fixture) {
    allow(connect_policy());
    authz.set_freshness([] { return false; });
    BOOST_TEST(!denied(check(k_botocore_token)).empty());
}

// Requirement 13.5: temporary credentials open a connection until the
// earlier of 12 hours and the session's expiry.
BOOST_FIXTURE_TEST_CASE(inv_redis_6_a_session_bounds_the_connection, fixture) {
    // An STS key, a role that may connect, and a one-hour session of it.
    std::array<unsigned char, iam::seal_key::size> raw{};
    raw.fill(0x17);
    run(iam_command::record_sts_key,
        {{"kid", "redis-test-kid"},
         {"sealed_key",
          iam::seal_secret(kek,
                           kythira::jose::base64url_encode(std::string(raw.begin(), raw.end())),
                           "redis-test-kid")}});
    run(iam_command::create_role,
        {{"name", "cache-role"},
         {"id", "AROACACHEROLE0000001"},
         {"trust_policy",
          R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"AWS":"111122223333"},"Action":"sts:AssumeRole"}]})"},
         {"max_session_duration", 3600}});
    run(iam_command::put_inline_policy, {{"kind", "role"},
                                         {"name", "cache-role"},
                                         {"policy_name", "redis"},
                                         {"document", connect_policy()}});
    const auto at = k_signed_at + 300;
    sts::session s;
    s.account = std::string(k_account);
    s.issuer_kind = std::string(sts::k_issuer_role);
    s.role_arn = "arn:aws:iam::111122223333:role/cache-role";
    s.role_name = "cache-role";
    s.session_name = "builder";
    s.principal_arn = "arn:aws:sts::111122223333:assumed-role/cache-role/builder";
    s.userid = "AROACACHEROLE0000001:builder";
    s.iat = at;
    s.exp = at + 3600;
    s.access_key_id = "ASIAKYTHIRAREDIS0001";
    s.secret_access_key = "session/secret/for/the/redis/verifier/te";
    const auto token = sts::seal({"redis-test-kid", iam::seal_key(raw)}, s);
    const auto auth_token =
        presign(s.access_key_id, s.secret_access_key, at, "farm", 900, k_cache, token);
    const auto r = check(auth_token);
    const auto* ok = std::get_if<kythira::credential_accepted>(&r);
    BOOST_TEST_REQUIRE(ok != nullptr, denied(r));
    BOOST_TEST(ok->principal == s.principal_arn);
    BOOST_TEST((ok->expires == std::chrono::system_clock::time_point{std::chrono::seconds{s.exp}}));
    // With the session token left off, the ASIA key alone is nothing.
    BOOST_TEST(
        !denied(check(presign(s.access_key_id, s.secret_access_key, at, "farm", 900, k_cache)))
             .empty());
}

// Requirement 13.5, with an injected clock: every accepted AUTH opens 12
// hours from its own moment, so a client that re-AUTHs on the open
// connection with a fresh token moves its deadline; a token from before the
// first AUTH cannot, being past its 15 minutes.
BOOST_FIXTURE_TEST_CASE(inv_redis_10_reauth_with_a_fresh_token_extends_the_deadline, fixture) {
    allow(connect_policy());
    const auto first = check(k_botocore_token);
    const auto* opened = std::get_if<kythira::credential_accepted>(&first);
    BOOST_TEST_REQUIRE(opened != nullptr, denied(first));
    BOOST_TEST((opened->expires == clock + 12h));

    // Eleven hours on, the old token is long expired; a fresh one is not.
    const auto later = k_signed_at + std::int64_t{11 * 3600};
    clock = std::chrono::system_clock::time_point{std::chrono::seconds{later + 60}};
    BOOST_TEST(!denied(check(k_botocore_token)).empty());
    const auto again = check(presign(k_botocore_key_id, k_botocore_secret, later));
    const auto* extended = std::get_if<kythira::credential_accepted>(&again);
    BOOST_TEST_REQUIRE(extended != nullptr, denied(again));
    BOOST_TEST((extended->expires == clock + 12h));
    BOOST_TEST((extended->expires > opened->expires));

    // The re-AUTH is authorized afresh: with the policy gone it is refused,
    // and the connection keeps only its first deadline.
    run(iam_command::delete_inline_policy,
        {{"kind", "user"}, {"name", "redis-client"}, {"policy_name", "redis"}});
    BOOST_TEST(denied(check(presign(k_botocore_key_id, k_botocore_secret, later)))
                   .find("elasticache:Connect denied") != std::string::npos);
}

// Requirement 3.8, with an injected clock: failed signatures are charged to
// the token's access key id wherever they come from; once its budget is
// spent even a good token for that key is refused unchecked, other keys are
// not, and the budget returns when the window ends. A refusal for a stale
// replica is not the signer's fault and is not charged.
BOOST_AUTO_TEST_CASE(inv_redis_11_failed_signatures_spend_the_key_budget) {
    fixture f;
    f.allow(connect_policy());
    std::chrono::steady_clock::time_point steady{};
    iam::iam_redis_verifier limited{f.authz,
                                    {.account = std::string(k_account),
                                     .region = std::string(k_region),
                                     .cache_name = std::string(k_cache),
                                     .per_key_failure_limit = 3,
                                     .failure_window = 60s},
                                    [&f] { return f.clock; },
                                    [&steady] { return steady; }};
    const auto check = [&](const std::string& token) {
        return limited.check({"redis", "farm", token, "198.51.100.7"});
    };
    const auto forged = presign(k_botocore_key_id, "not-the-secret", k_signed_at);
    BOOST_TEST(denied(check(std::string(k_botocore_token))).empty());
    for (int i = 0; i < 3; ++i) {
        BOOST_TEST(denied(check(forged)).find("authentication failed") != std::string::npos);
    }
    BOOST_TEST(denied(check(std::string(k_botocore_token))).find("too many failed signatures") !=
               std::string::npos);
    // Another key id is not held up by this one's failures.
    BOOST_TEST(denied(check(presign("AKIAKYTHIRAREDIS7777", k_botocore_secret, k_signed_at)))
                   .find("authentication failed") != std::string::npos);
    steady += 61s;
    BOOST_TEST(denied(check(std::string(k_botocore_token))).empty());

    // Stale: refused, but the key keeps its whole budget.
    bool fresh = false;
    f.authz.set_freshness([&fresh] { return fresh; });
    for (int i = 0; i < 5; ++i) {
        BOOST_TEST(denied(check(forged)).find("Unavailable") != std::string::npos);
    }
    fresh = true;
    BOOST_TEST(denied(check(std::string(k_botocore_token))).empty());
}

BOOST_AUTO_TEST_SUITE_END()
