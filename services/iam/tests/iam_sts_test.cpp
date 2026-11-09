// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// STS core (`.kiro/specs/iam-compatible-auth/` task 17, Requirements
// 10.1-10.4, 10.7, 10.10): session tokens sealed under rotating STS keys,
// AssumeRole within and across accounts with ExternalId, GetSessionToken,
// GetCallerIdentity and GetAccessKeyInfo, and the authorizer's ASIA path,
// driven end to end with real SigV4 signatures.

#define BOOST_TEST_MODULE iam_sts_test

#include <boost/test/unit_test.hpp>

#include <kythira/iam/iam_authorizer.hpp>
#include <kythira/iam/iam_service_registry.hpp>
#include <kythira/iam/iam_state_machine.hpp>
#include <kythira/iam/iam_sts_api.hpp>
#include <kythira/iam/iam_sts_token.hpp>
#include <kythira/iam/services/iam_actions.hpp>
#include <kythira/iam/services/sts_actions.hpp>

#include <raft/sigv4_verifier.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <regex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace iam = kythira::iam;
namespace sts = kythira::iam::sts;
namespace aws_auth = kythira::aws_auth;
namespace sigv4 = kythira::sigv4;
namespace q = kythira::aws_query;
namespace leader = kythira::iam::iam_leader;
using iam::iam_command;

namespace {

constexpr std::string_view k_a = "111122223333";
constexpr std::string_view k_b = "444455556666";

auto test_kek() -> iam::seal_key {
    std::array<unsigned char, iam::seal_key::size> b{};
    for (std::size_t i = 0; i < b.size(); ++i) {
        b[i] = static_cast<unsigned char>(0x40 + i);
    }
    return iam::seal_key(b);
}

auto text(const std::vector<std::byte>& v) -> std::string {
    return {reinterpret_cast<const char*>(v.data()), v.size()};  // NOLINT
}

auto element(const std::string& xml, const std::string& tag) -> std::string {
    std::smatch m;
    const std::regex re("<" + tag + ">([^<]*)</" + tag + ">");
    return std::regex_search(xml, m, re) ? m[1].str() : std::string{};
}

auto code_of(const iam::query::reply& r) -> std::string {
    return element(r.body, "Code");
}

auto enc(std::string_view s) -> std::string {
    return q::url_encode(s);
}

/// Credentials from a reply's `Credentials` element.
struct creds {
    std::string id;
    std::string secret;
    std::string token;
    std::string expiration;
};

auto creds_of(const iam::query::reply& r) -> creds {
    return {element(r.body, "AccessKeyId"), element(r.body, "SecretAccessKey"),
            element(r.body, "SessionToken"), element(r.body, "Expiration")};
}

auto trust_for(const std::string& principal, const std::string& condition = "") -> std::string {
    return R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"AWS":")" +
           principal + R"("},"Action":"sts:AssumeRole")" +
           (condition.empty() ? "" : R"(,"Condition":)" + condition) + "}]}";
}

struct fixture {
    iam::iam_state_machine sm;
    std::shared_mutex mu;
    std::string current_kid;
    std::uint64_t index = 0;
    iam::seal_key kek = test_kek();
    iam::iam_service_registry registry;
    std::chrono::system_clock::time_point clock =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    iam::iam_authorizer authz{
        [this](const std::function<void(const iam::iam_state_machine&)>& body) {
            const std::shared_lock lock(mu);
            body(sm);
        },
        [this](std::string_view) -> const iam::seal_key& { return kek; }, registry, nullptr,
        [this] { return clock; }};

    struct cluster {
        fixture* f;
        void read(const std::function<void(const iam::iam_state_machine&)>& body) {
            const std::shared_lock lock(f->mu);
            body(f->sm);
        }
        auto kek(std::string_view) -> const iam::seal_key& { return f->kek; }
    } c{this};
    sts::sts_query_api<cluster> api{c, authz};

    fixture() {
        registry.add(iam::services::iam_actions());
        registry.add(iam::services::sts_actions());
        registry.add({.service_prefix = "s3", .actions = {{"GetObject", {"*"}, {}}}});
        run(iam_command::create_account,
            {{"account", k_a}, {"kek_salt", "c2FsdHNhbHRzYWx0c2FsdA"}});
        run(iam_command::create_account, {{"account", k_b}});
        current_kid = rotate();
    }

    [[nodiscard]] auto now_s() const -> std::int64_t {
        return std::chrono::duration_cast<std::chrono::seconds>(clock.time_since_epoch()).count();
    }

    auto run(iam_command type, boost::json::object body) -> std::string {
        if (!body.contains("account")) {
            body["account"] = k_a;
        }
        const std::unique_lock lock(mu);
        return text(sm.apply(leader::command(type, std::move(body), now_s()), ++index));
    }

    /// Records a new STS key and retires the others after `retire_in`, as
    /// the admin rotation does. Returns the new key's id.
    auto rotate(std::int64_t retire_in = 36 * 3600) -> std::string {
        static int n = 0;
        const auto kid = "sts-test-" + std::to_string(++n);
        auto raw = iam::random_bytes(32);
        const auto sealed = iam::seal_secret(kek, kythira::jose::base64url_encode(raw), kid);
        std::vector<std::string> old;
        {
            const std::shared_lock lock(mu);
            for (const auto& k : sm.sts_keys()) {
                if (!k.retire_after) {
                    old.push_back(k.kid);
                }
            }
        }
        run(iam_command::record_sts_key, {{"kid", kid}, {"sealed_key", sealed}});
        for (const auto& k : old) {
            run(iam_command::retire_sts_key, {{"kid", k}, {"retire_after", now_s() + retire_in}});
        }
        return kid;
    }

    void user(const std::string& name, std::string_view account = k_a,
              const std::string& statements = "") {
        run(iam_command::create_user,
            {{"account", account}, {"name", name}, {"id", leader::generate_unique_id("AIDA")}});
        if (!statements.empty()) {
            run(iam_command::put_inline_policy,
                {{"account", account},
                 {"kind", "user"},
                 {"name", name},
                 {"policy_name", "p"},
                 {"document", R"({"Version":"2012-10-17","Statement":[)" + statements + "]}"}});
        }
    }

    void role(const std::string& name, const std::string& trust, std::int64_t max = 3600,
              const std::string& statements = "") {
        run(iam_command::create_role, {{"name", name},
                                       {"id", leader::generate_unique_id("AROA")},
                                       {"trust_policy", trust},
                                       {"max_session_duration", max}});
        if (!statements.empty()) {
            run(iam_command::put_inline_policy,
                {{"kind", "role"},
                 {"name", name},
                 {"policy_name", "p"},
                 {"document", R"({"Version":"2012-10-17","Statement":[)" + statements + "]}"}});
        }
    }

    [[nodiscard]] static auto principal(const std::string& name, std::string_view account = k_a)
        -> aws_auth::principal_context {
        aws_auth::principal_context p;
        p.type = aws_auth::principal_type::user;
        p.account = std::string(account);
        p.arn = "arn:aws:iam::" + std::string(account) + ":user/" + name;
        p.username = name;
        p.userid = "AIDA" + name;
        return p;
    }

    [[nodiscard]] static auto root(std::string_view account = k_a) -> aws_auth::principal_context {
        aws_auth::principal_context p;
        p.type = aws_auth::principal_type::root;
        p.account = std::string(account);
        p.arn = "arn:aws:iam::" + std::string(account) + ":root";
        p.userid = std::string(account);
        return p;
    }

    auto call(const aws_auth::principal_context& who, const std::string& body)
        -> iam::query::reply {
        const auto f = q::form::parse(body);
        BOOST_TEST_REQUIRE(f.has_value(), body);
        return api.handle(who, *f, {.request_id = "req-1"});
    }

    auto assume(const aws_auth::principal_context& who, const std::string& role_name,
                const std::string& extra = "", std::string_view account = k_a)
        -> iam::query::reply {
        return call(who, "Action=AssumeRole&RoleSessionName=s1&RoleArn=" +
                             enc("arn:aws:iam::" + std::string(account) + ":role/" + role_name) +
                             extra);
    }

    /// Authenticates a request signed with temporary credentials.
    auto authenticate(const creds& k, const std::string& body = "Action=GetCallerIdentity")
        -> aws_auth::authn_result {
        const auto amz =
            sigv4::detail::format_amz_date(std::chrono::floor<std::chrono::seconds>(clock));
        const auto date = amz.substr(0, 8);
        sigv4::request r;
        r.method = "POST";
        r.raw_path = "/";
        r.body = std::string_view(body);
        const auto auth = [&](std::string_view sig) {
            return "AWS4-HMAC-SHA256 Credential=" + k.id + "/" + date +
                   "/us-east-1/sts/aws4_request, "
                   "SignedHeaders=content-type;host;x-amz-date;x-amz-security-token, "
                   "Signature=" +
                   std::string(sig);
        };
        r.headers = {{"Host", "sts.amazonaws.com"},
                     {"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"},
                     {"X-Amz-Date", amz},
                     {"X-Amz-Security-Token", k.token},
                     {"Authorization", auth(std::string(64, '0'))}};
        const sigv4::service_rules rules{.service = "sts"};
        const auto parsed = sigv4::parse(r, rules, std::chrono::floor<std::chrono::seconds>(clock));
        BOOST_TEST_REQUIRE(parsed.has_value(), parsed.error().message);
        r.headers.back().second = auth(
            sigv4::compute_signature(*parsed, sigv4::derive_signing_key(k.secret, parsed->scope)));
        return authz.authenticate(r, rules);
    }
};

const std::string k_get_object = R"({"Effect":"Allow","Action":"s3:GetObject","Resource":"*"})";
const std::string k_alice = "arn:aws:iam::111122223333:user/alice";

/// A trust policy letting `principal` assume the role, tag the session
/// and set its source identity.
auto trust_tagging(const std::string& principal, const std::string& condition = "") -> std::string {
    return R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"AWS":")" +
           principal +
           R"("},"Action":["sts:AssumeRole","sts:TagSession","sts:SetSourceIdentity"])" +
           (condition.empty() ? "" : R"(,"Condition":)" + condition) + "}]}";
}

/// `Tags.member.N` parameters for `tags`.
auto tag_params(const std::vector<std::pair<std::string, std::string>>& tags) -> std::string {
    std::string out;
    for (std::size_t i = 0; i < tags.size(); ++i) {
        const auto n = std::to_string(i + 1);
        out += "&Tags.member." + n + ".Key=" + enc(tags[i].first) + "&Tags.member." + n +
               ".Value=" + enc(tags[i].second);
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(tokens)

BOOST_AUTO_TEST_CASE(cbor_round_trips_a_session_and_refuses_damage) {
    sts::session s;
    s.account = std::string(k_a);
    s.principal_arn = "arn:aws:sts::111122223333:assumed-role/r/s";
    s.issuer_kind = std::string(sts::k_issuer_role);
    s.iat = 1;
    s.exp = 4'000'000'000;
    s.session_policies = {R"({"Version":"2012-10-17"})"};
    s.tags = {{"team", "iam"}};
    s.transitive_tag_keys = {"team"};
    s.chained = true;
    const auto bytes = sts::cbor::encode(sts::to_json(s));
    const auto back = sts::cbor::decode(bytes);
    BOOST_REQUIRE(back.has_value());
    BOOST_TEST((sts::from_json(back->as_object()).value() == s));
    // Truncated, padded or deeply nested input is refused.
    BOOST_TEST(!sts::cbor::decode(bytes.substr(0, bytes.size() - 1)).has_value());
    BOOST_TEST(!sts::cbor::decode(bytes + "x").has_value());
    BOOST_TEST(!sts::cbor::decode(std::string(20, '\x81')).has_value());
    BOOST_TEST(!sts::cbor::decode("\x5f").has_value());  // indefinite byte string
    // A session missing a field does not parse.
    auto o = sts::to_json(s);
    o.erase("exp");
    BOOST_TEST(!sts::from_json(o).has_value());
}

BOOST_AUTO_TEST_CASE(a_token_opens_only_under_its_key_and_before_its_expiry) {
    std::array<unsigned char, 32> b{};
    b.fill(7);
    const sts::sts_key k1{"kid-one", iam::seal_key(b)};
    b.fill(8);
    const sts::sts_key k2{"kid-two", iam::seal_key(b)};
    sts::session s;
    s.exp = 100;
    const auto token = sts::seal(k1, s);
    BOOST_TEST(sts::open(token, {k2, k1}, 99).has_value());
    BOOST_TEST((sts::open(token, {k1}, 100).error() == sts::token_error::expired));
    // Its key gone (retired past its window): expired, not invalid.
    BOOST_TEST((sts::open(token, {k2}, 1).error() == sts::token_error::expired));
    // A key with the same id but other bytes cannot open it.
    const sts::sts_key imposter{"kid-one", k2.key};
    BOOST_TEST((sts::open(token, {imposter}, 1).error() == sts::token_error::invalid));
    auto damaged = token;
    damaged[damaged.size() / 2] = damaged[damaged.size() / 2] == 'A' ? 'B' : 'A';
    BOOST_TEST((sts::open(damaged, {k1}, 1).error() == sts::token_error::invalid));
    BOOST_TEST((sts::open("not a token", {k1}, 1).error() == sts::token_error::invalid));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(assume_role)

BOOST_FIXTURE_TEST_CASE(inv_sts_b8_assume_role_returns_the_assumed_role_arn_and_credentials,
                        fixture) {
    user("alice");
    role("app", trust_for(k_alice), 3600, k_get_object);
    const auto r = assume(principal("alice"), "app");
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(element(r.body, "Arn") == "arn:aws:sts::111122223333:assumed-role/app/s1");
    const auto id = element(r.body, "AssumedRoleId");
    BOOST_TEST(id.starts_with("AROA"));
    BOOST_TEST(id.ends_with(":s1"));
    const auto k = creds_of(r);
    BOOST_TEST(k.id.starts_with("ASIA"));
    BOOST_TEST(k.id.size() == 20U);
    BOOST_TEST(k.secret.size() == 40U);

    // The credentials authenticate as the session, and the role's policy
    // applies to it.
    const auto who = authenticate(k);
    BOOST_REQUIRE(who.has_value());
    BOOST_TEST((who->type == aws_auth::principal_type::assumed_role));
    BOOST_TEST(who->arn == "arn:aws:sts::111122223333:assumed-role/app/s1");
    BOOST_TEST(who->role_arn == "arn:aws:iam::111122223333:role/app");
    BOOST_TEST(who->userid == id);
    BOOST_TEST(who->token_issue_time.has_value());
    BOOST_TEST(
        authz.authorize(*who, {.action = "s3:GetObject", .resource = "arn:aws:s3:::b/k"}).allowed);
    const auto me = call(*who, "Action=GetCallerIdentity");
    BOOST_TEST(element(me.body, "Arn") == who->arn);
    BOOST_TEST(element(me.body, "Account") == k_a);
}

BOOST_FIXTURE_TEST_CASE(inv_sts_b1_cross_account_assume_role_needs_both_sides, fixture) {
    role("shared", trust_for("arn:aws:iam::444455556666:root"));
    role("private", trust_for("arn:aws:iam::999988887777:root"));
    // Account B's bob: trusted by the role, but his own account has not
    // allowed him to assume it.
    user("bob", k_b);
    const auto bob = principal("bob", k_b);
    BOOST_TEST(code_of(assume(bob, "shared")) == "AccessDenied");
    // B grants it: now both sides agree.
    run(iam_command::put_inline_policy,
        {{"account", k_b},
         {"kind", "user"},
         {"name", "bob"},
         {"policy_name", "assume"},
         {"document",
          R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"sts:AssumeRole","Resource":"*"}]})"}});
    const auto r = assume(bob, "shared");
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(element(r.body, "Arn") == "arn:aws:sts::111122223333:assumed-role/shared/s1");
    // The session belongs to the role's account.
    const auto who = authenticate(creds_of(r));
    BOOST_REQUIRE(who.has_value());
    BOOST_TEST(who->account == k_a);
    // A role whose trust policy names another account stays closed.
    BOOST_TEST(code_of(assume(bob, "private")) == "AccessDenied");
    // So does a role that does not exist, with the same answer.
    BOOST_TEST(code_of(assume(bob, "ghost")) == "AccessDenied");
}

BOOST_FIXTURE_TEST_CASE(same_account_trust_naming_the_account_needs_an_identity_grant, fixture) {
    role("app", trust_for("arn:aws:iam::111122223333:root"));
    user("carol");
    BOOST_TEST(code_of(assume(principal("carol"), "app")) == "AccessDenied");
    user(
        "dave", k_a,
        R"({"Effect":"Allow","Action":"sts:AssumeRole","Resource":"arn:aws:iam::111122223333:role/app"})");
    BOOST_TEST(assume(principal("dave"), "app").status == 200U);
}

BOOST_FIXTURE_TEST_CASE(inv_sts_b2_external_id_must_match, fixture) {
    user("alice");
    role("vendor", trust_for(k_alice, R"({"StringEquals":{"sts:ExternalId":"tenant-42"}})"));
    BOOST_TEST(code_of(assume(principal("alice"), "vendor")) == "AccessDenied");
    BOOST_TEST(code_of(assume(principal("alice"), "vendor", "&ExternalId=tenant-41")) ==
               "AccessDenied");
    BOOST_TEST(assume(principal("alice"), "vendor", "&ExternalId=tenant-42").status == 200U);
}

BOOST_FIXTURE_TEST_CASE(the_session_name_reaches_the_trust_policy, fixture) {
    user("alice");
    role("named", trust_for(k_alice, R"({"StringLike":{"sts:RoleSessionName":"ci-*"}})"));
    BOOST_TEST(code_of(assume(principal("alice"), "named")) == "AccessDenied");  // "s1"
    BOOST_TEST(call(principal("alice"), "Action=AssumeRole&RoleSessionName=ci-7&RoleArn=" +
                                            enc("arn:aws:iam::111122223333:role/named"))
                   .status == 200U);
}

BOOST_FIXTURE_TEST_CASE(inv_sts_1_role_session_names_are_checked, fixture) {
    user("alice");
    role("app", trust_for(k_alice));
    const auto with = [&](const std::string& name) {
        return call(principal("alice"),
                    "Action=AssumeRole&RoleSessionName=" + enc(name) +
                        "&RoleArn=" + enc("arn:aws:iam::111122223333:role/app"));
    };
    for (const auto& bad : {std::string("a"), std::string(65, 'a'), std::string("bad name"),
                            std::string("a/b"), std::string("a*")}) {
        BOOST_TEST(code_of(with(bad)) == "ValidationError", bad);
    }
    BOOST_TEST(with("ok").status == 200U);
    BOOST_TEST(with("a+=,.@-_" + std::string(56, 'x')).status == 200U);  // 64 characters
    BOOST_TEST(code_of(call(principal("alice"), "Action=AssumeRole&RoleArn=" +
                                                    enc("arn:aws:iam::111122223333:role/app"))) ==
               "ValidationError");
}

BOOST_FIXTURE_TEST_CASE(inv_sts_3_external_ids_are_checked, fixture) {
    user("alice");
    role("app", trust_for(k_alice));
    for (const auto& bad :
         {std::string("x"), std::string(1225, 'x'), std::string("a#b"), std::string("a b")}) {
        BOOST_TEST(code_of(assume(principal("alice"), "app", "&ExternalId=" + enc(bad))) ==
                       "ValidationError",
                   bad);
    }
    BOOST_TEST(
        assume(principal("alice"), "app", "&ExternalId=" + enc("arn:x/y=z,1.2@3-_+")).status ==
        200U);
    BOOST_TEST(assume(principal("alice"), "app", "&ExternalId=" + std::string(1224, 'x')).status ==
               200U);
}

BOOST_FIXTURE_TEST_CASE(inv_dur_1_and_2_assume_role_durations, fixture) {
    user("alice");
    role("short", trust_for(k_alice), 3600);
    role("long", trust_for(k_alice), 43200);
    const auto expires_in = [&](const iam::query::reply& r) {
        // Expiration is ISO 8601; compare against now as text-free seconds.
        std::tm tm{};
        const auto e = element(r.body, "Expiration");
        strptime(e.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return static_cast<std::int64_t>(timegm(&tm)) - now_s();
    };
    // INV-DUR-1: default 3600, within 900-43200.
    auto r = assume(principal("alice"), "long");
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(std::abs(expires_in(r) - 3600) <= 5);
    BOOST_TEST(code_of(assume(principal("alice"), "long", "&DurationSeconds=899")) ==
               "ValidationError");
    BOOST_TEST(code_of(assume(principal("alice"), "long", "&DurationSeconds=43201")) ==
               "ValidationError");
    r = assume(principal("alice"), "long", "&DurationSeconds=43200");
    BOOST_TEST(std::abs(expires_in(r) - 43200) <= 5);
    BOOST_TEST(assume(principal("alice"), "short", "&DurationSeconds=900").status == 200U);
    // INV-DUR-2: past the role's maximum the request is refused, not
    // shortened.
    r = assume(principal("alice"), "short", "&DurationSeconds=3601");
    BOOST_TEST(code_of(r) == "ValidationError");
    BOOST_TEST(r.body.find("MaxSessionDuration") != std::string::npos);
    BOOST_TEST(code_of(assume(principal("alice"), "long", "&DurationSeconds=1h")) ==
               "ValidationError");
}

BOOST_FIXTURE_TEST_CASE(inv_dur_3_role_chaining_is_capped_at_an_hour, fixture) {
    user("alice");
    role("first", trust_for(k_alice), 43200,
         R"({"Effect":"Allow","Action":"sts:AssumeRole","Resource":"*"})");
    role("second", trust_for("arn:aws:iam::111122223333:root"), 43200);
    const auto session = authenticate(creds_of(assume(principal("alice"), "first")));
    BOOST_REQUIRE(session.has_value());
    BOOST_TEST(code_of(assume(*session, "second", "&DurationSeconds=3601")) == "ValidationError");
    const auto r = assume(*session, "second");
    BOOST_TEST(r.status == 200U, r.body);
    const auto chained = authenticate(creds_of(r));
    BOOST_REQUIRE(chained.has_value());
    BOOST_TEST(chained->arn == "arn:aws:sts::111122223333:assumed-role/second/s1");
}

BOOST_FIXTURE_TEST_CASE(actions_of_later_tasks_are_not_implemented, fixture) {
    user("alice");
    for (const auto* action : {"AssumeRoleWithSAML", "AssumeRoot", "AssumeRoleWithWebIdentity",
                               "GetFederationToken", "DecodeAuthorizationMessage"}) {
        const auto r = call(principal("alice"), std::string("Action=") + action);
        BOOST_TEST(r.status == 501U, action);
    }
    BOOST_TEST(code_of(call(principal("alice"), "Action=GetCallerIdentity&Version=2010-05-08")) ==
               "InvalidParameterValue");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(session_policies_tags_and_identity)

BOOST_FIXTURE_TEST_CASE(inv_spol_1_to_4_session_policies_are_checked, fixture) {
    user("alice");
    role("app", trust_for(k_alice), 3600, k_get_object);
    // INV-SPOL-1: 1-2048 characters, then a policy that parses.
    BOOST_TEST(code_of(assume(principal("alice"), "app", "&Policy=" + std::string(2049, 'a'))) ==
               "MalformedPolicyDocument");
    BOOST_TEST(code_of(assume(principal("alice"), "app", "&Policy=" + enc("{not json"))) ==
               "MalformedPolicyDocument");
    // INV-SPOL-2: at most 10 ARNs.
    std::string eleven;
    for (int i = 1; i <= 11; ++i) {
        eleven += "&PolicyArns.member." + std::to_string(i) +
                  ".arn=" + enc("arn:aws:iam::aws:policy/P" + std::to_string(i));
    }
    BOOST_TEST(code_of(assume(principal("alice"), "app", eleven)) == "InvalidInput");
    // INV-SPOL-3: the role's own account, or AWS-managed.
    BOOST_TEST(code_of(assume(
                   principal("alice"), "app",
                   "&PolicyArns.member.1.arn=" + enc("arn:aws:iam::444455556666:policy/theirs"))) ==
               "InvalidInput");
    BOOST_TEST(assume(principal("alice"), "app",
                      "&PolicyArns.member.1.arn=" + enc("arn:aws:iam::111122223333:policy/ours"))
                   .status == 200U);
    // INV-SPOL-4: policy characters plus ARNs at most 2048, reported as a
    // percentage when accepted.
    const auto sid = std::string(1930, 'a');
    const auto big = R"({"Version":"2012-10-17","Statement":[{"Sid":")" + sid +
                     R"(","Effect":"Allow","Action":"s3:GetObject","Resource":"*"}]})";
    const auto ok = assume(principal("alice"), "app", "&Policy=" + enc(big));
    BOOST_TEST(ok.status == 200U, ok.body);
    BOOST_TEST(std::stoi(element(ok.body, "PackedPolicySize")) >= 95);
    BOOST_TEST(code_of(assume(principal("alice"), "app",
                              "&Policy=" + enc(big) + "&PolicyArns.member.1.arn=" +
                                  enc("arn:aws:iam::aws:policy/ReadOnlyAccess"))) ==
               "PackedPolicyTooLarge");
}

BOOST_FIXTURE_TEST_CASE(a_session_policy_limits_the_session_to_the_intersection, fixture) {
    user("alice");
    role("app", trust_for(k_alice), 3600,
         k_get_object + R"(,{"Effect":"Allow","Action":"iam:ListRoles","Resource":"*"})");
    const auto session_with = [&](const std::string& extra) {
        const auto r = assume(principal("alice"), "app", extra);
        BOOST_TEST_REQUIRE(r.status == 200U, r.body);
        auto who = authenticate(creds_of(r));
        BOOST_TEST_REQUIRE(who.has_value());
        return *who;
    };
    const auto can = [&](const aws_auth::principal_context& who, const char* action) {
        return authz.authorize(who, {.action = action, .resource = "arn:aws:s3:::b/k"}).allowed;
    };
    // No session policy: the role's permissions.
    const auto plain = session_with("");
    BOOST_TEST(can(plain, "s3:GetObject"));
    BOOST_TEST(can(plain, "iam:ListRoles"));
    // An inline session policy narrows them, and grants nothing the role lacks.
    const auto narrowed = session_with(
        "&Policy=" +
        enc(R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":["s3:GetObject","iam:ListUsers"],"Resource":"*"}]})"));
    BOOST_TEST(can(narrowed, "s3:GetObject"));
    BOOST_TEST(!can(narrowed, "iam:ListRoles"));
    BOOST_TEST(!can(narrowed, "iam:ListUsers"));
    // A managed policy by ARN is read at request time.
    run(iam_command::create_policy,
        {{"name", "Roles"},
         {"id", "ANPAROLES00000000001"},
         {"document",
          R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"iam:ListRoles","Resource":"*"}]})"}});
    const auto by_arn =
        session_with("&PolicyArns.member.1.arn=" + enc("arn:aws:iam::111122223333:policy/Roles"));
    BOOST_TEST(!can(by_arn, "s3:GetObject"));
    BOOST_TEST(can(by_arn, "iam:ListRoles"));
    // One that does not exist still limits the session, to nothing.
    const auto ghost =
        session_with("&PolicyArns.member.1.arn=" + enc("arn:aws:iam::111122223333:policy/ghost"));
    BOOST_TEST(!can(ghost, "s3:GetObject"));
    BOOST_TEST(!can(ghost, "iam:ListRoles"));
}

BOOST_FIXTURE_TEST_CASE(inv_stag_4_session_tags_need_tag_session_in_the_trust_policy, fixture) {
    user("alice");
    const std::string blue_only =
        R"({"Effect":"Allow","Action":"s3:GetObject","Resource":"*","Condition":{"StringEquals":{"aws:PrincipalTag/team":"blue"}}})";
    role("plain", trust_for(k_alice), 3600, blue_only);
    role("tagging", trust_tagging(k_alice), 3600, blue_only);
    const auto r = assume(principal("alice"), "plain", tag_params({{"team", "blue"}}));
    BOOST_TEST(code_of(r) == "AccessDenied");
    BOOST_TEST(r.body.find("sts:TagSession") != std::string::npos, r.body);
    // The tags reach the session as principal tags, and conditions on
    // aws:RequestTag in the trust policy see them.
    role("only-blue-requests",
         trust_tagging(k_alice, R"({"StringEquals":{"aws:RequestTag/team":"blue"}})"), 3600,
         blue_only);
    const auto can_get = [&](const std::string& role_name, const std::string& team) {
        const auto rr = assume(principal("alice"), role_name, tag_params({{"team", team}}));
        BOOST_TEST_REQUIRE(rr.status == 200U, rr.body);
        const auto who = authenticate(creds_of(rr));
        BOOST_TEST_REQUIRE(who.has_value());
        return authz.authorize(*who, {.action = "s3:GetObject", .resource = "arn:aws:s3:::b/k"})
            .allowed;
    };
    BOOST_TEST(can_get("tagging", "blue"));
    BOOST_TEST(!can_get("tagging", "red"));
    BOOST_TEST(can_get("only-blue-requests", "blue"));
    BOOST_TEST(code_of(assume(principal("alice"), "only-blue-requests",
                              tag_params({{"team", "red"}}))) == "AccessDenied");
}

BOOST_FIXTURE_TEST_CASE(inv_stag_5_session_tags_override_the_roles_tags, fixture) {
    user("alice");
    role(
        "app", trust_tagging(k_alice), 3600,
        R"({"Effect":"Allow","Action":"s3:GetObject","Resource":"*","Condition":{"StringEquals":{"aws:PrincipalTag/Team":"blue"}}})");
    run(iam_command::tag_entity,
        {{"kind", "role"},
         {"name", "app"},
         {"tags", boost::json::array{{{"Key", "Team"}, {"Value", "red"}}}}});
    const auto can = [&](const std::string& extra) {
        const auto r = assume(principal("alice"), "app", extra);
        BOOST_TEST_REQUIRE(r.status == 200U, r.body);
        const auto who = authenticate(creds_of(r));
        BOOST_TEST_REQUIRE(who.has_value());
        return authz.authorize(*who, {.action = "s3:GetObject", .resource = "arn:aws:s3:::b/k"})
            .allowed;
    };
    BOOST_TEST(!can(""));                             // the role's own Team=red
    BOOST_TEST(can(tag_params({{"team", "blue"}})));  // overridden, case-insensitively
}

BOOST_FIXTURE_TEST_CASE(inv_stag_1_to_3_session_tags_are_checked_and_pass_down_a_chain, fixture) {
    user("alice");
    role("first", trust_tagging(k_alice));
    // INV-STAG-1: at most 50, keys unique case-insensitively.
    std::vector<std::pair<std::string, std::string>> many;
    for (int i = 0; i < 51; ++i) {
        many.emplace_back("k" + std::to_string(i), "v");
    }
    BOOST_TEST(code_of(assume(principal("alice"), "first", tag_params(many))) == "ValidationError");
    BOOST_TEST(code_of(assume(principal("alice"), "first",
                              tag_params({{"Team", "a"}, {"team", "b"}}))) == "ValidationError");
    // INV-STAG-2: at most 50 transitive keys of 1-128 characters, each a tag.
    BOOST_TEST(code_of(assume(principal("alice"), "first",
                              tag_params({{"team", "blue"}}) + "&TransitiveTagKeys.member.1=" +
                                  std::string(129, 'k'))) == "ValidationError");
    BOOST_TEST(
        code_of(assume(principal("alice"), "first",
                       tag_params({{"team", "blue"}}) + "&TransitiveTagKeys.member.1=project")) ==
        "InvalidParameterValue");
    // A transitive tag passes to the next role in the chain...
    const auto r =
        assume(principal("alice"), "first",
               tag_params({{"team", "blue"}, {"cost", "1"}}) + "&TransitiveTagKeys.member.1=team");
    BOOST_TEST_REQUIRE(r.status == 200U, r.body);
    const auto first = authenticate(creds_of(r));
    BOOST_TEST_REQUIRE(first.has_value());
    role(
        "second", trust_tagging("arn:aws:iam::111122223333:role/first"), 3600,
        R"({"Effect":"Allow","Action":"s3:GetObject","Resource":"*","Condition":{"StringEquals":{"aws:PrincipalTag/team":"blue"},"Null":{"aws:PrincipalTag/cost":"true"}}})");
    const auto r2 = assume(*first, "second");
    BOOST_TEST_REQUIRE(r2.status == 200U, r2.body);
    const auto second = authenticate(creds_of(r2));
    BOOST_TEST_REQUIRE(second.has_value());
    BOOST_TEST(authz.authorize(*second, {.action = "s3:GetObject", .resource = "arn:aws:s3:::b/k"})
                   .allowed);
    // ...needs sts:TagSession there...
    role("untagged", trust_for("arn:aws:iam::111122223333:role/first"));
    BOOST_TEST(code_of(assume(*first, "untagged")) == "AccessDenied");
    // ...and (INV-STAG-3) cannot be overridden on the way.
    BOOST_TEST(code_of(assume(*first, "second", tag_params({{"TEAM", "red"}}))) ==
               "InvalidParameterValue");
    // A non-transitive tag may be.
    BOOST_TEST(assume(*first, "second", tag_params({{"cost", "2"}})).status == 200U);
}

BOOST_FIXTURE_TEST_CASE(inv_sts_6_source_identity_is_checked_and_kept_down_a_chain, fixture) {
    user("alice");
    role("plain", trust_for(k_alice));
    role("first", trust_tagging(k_alice));
    for (const auto* bad : {"x", "aws:me", "has space"}) {
        BOOST_TEST(code_of(assume(principal("alice"), "first", "&SourceIdentity=" + enc(bad))) ==
                       "ValidationError",
                   bad);
    }
    BOOST_TEST(code_of(assume(principal("alice"), "plain", "&SourceIdentity=alice-ci")) ==
               "AccessDenied");
    const auto r = assume(principal("alice"), "first", "&SourceIdentity=alice-ci");
    BOOST_TEST_REQUIRE(r.status == 200U, r.body);
    BOOST_TEST(element(r.body, "SourceIdentity") == "alice-ci");
    const auto first = authenticate(creds_of(r));
    BOOST_TEST_REQUIRE(first.has_value());
    BOOST_TEST(first->source_identity == "alice-ci");
    // Down the chain it is kept, needs sts:SetSourceIdentity, and cannot change.
    role("second", trust_tagging("arn:aws:iam::111122223333:role/first"));
    role("strict", trust_for("arn:aws:iam::111122223333:role/first"));
    BOOST_TEST(code_of(assume(*first, "strict")) == "AccessDenied");
    BOOST_TEST(code_of(assume(*first, "second", "&SourceIdentity=bob-ci")) ==
               "InvalidParameterValue");
    const auto r2 = assume(*first, "second");
    BOOST_TEST_REQUIRE(r2.status == 200U, r2.body);
    const auto second = authenticate(creds_of(r2));
    BOOST_TEST_REQUIRE(second.has_value());
    BOOST_TEST(second->source_identity == "alice-ci");
}

BOOST_FIXTURE_TEST_CASE(older_sessions_are_revoked_by_a_token_issue_time_deny, fixture) {
    // AWS's "Revoke active sessions": an inline deny on the role for any
    // token issued before a time. aws:TokenIssueTime comes from the token.
    user("alice");
    role("app", trust_for(k_alice), 3600, k_get_object);
    const auto r = assume(principal("alice"), "app");
    BOOST_TEST_REQUIRE(r.status == 200U, r.body);
    const auto old_session = authenticate(creds_of(r));
    BOOST_TEST_REQUIRE(old_session.has_value());
    BOOST_TEST_REQUIRE(old_session->token_issue_time.has_value());
    const auto issued = std::chrono::duration_cast<std::chrono::seconds>(
                            old_session->token_issue_time->time_since_epoch())
                            .count();
    const auto get = [&](const aws_auth::principal_context& who) {
        return authz.authorize(who, {.action = "s3:GetObject", .resource = "arn:aws:s3:::b/k"})
            .allowed;
    };
    BOOST_TEST(get(*old_session));
    run(iam_command::put_inline_policy,
        {{"kind", "role"},
         {"name", "app"},
         {"policy_name", "AWSRevokeOlderSponsoredSessions"},
         {"document",
          R"({"Version":"2012-10-17","Statement":[{"Effect":"Deny","Action":"*","Resource":"*","Condition":{"DateLessThan":{"aws:TokenIssueTime":")" +
              iam::query::iso8601(issued + 5) + R"("}}}]})"}});
    BOOST_TEST(!get(*old_session));
    // A session issued after the cut-off is unaffected.
    auto newer = *old_session;
    newer.token_issue_time = *old_session->token_issue_time + std::chrono::seconds(10);
    BOOST_TEST(get(newer));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(session_tokens_and_keys)

BOOST_FIXTURE_TEST_CASE(inv_sts_b3_credentials_stop_working_at_their_expiration, fixture) {
    user("alice");
    role("app", trust_for(k_alice));
    const auto k = creds_of(assume(principal("alice"), "app", "&DurationSeconds=900"));
    BOOST_TEST(authenticate(k).has_value());
    clock += std::chrono::seconds(899);
    BOOST_TEST(authenticate(k).has_value());
    clock += std::chrono::seconds(2);
    const auto late = authenticate(k);
    BOOST_REQUIRE(!late.has_value());
    BOOST_TEST(late.error().code == "ExpiredToken");
}

BOOST_FIXTURE_TEST_CASE(a_token_outlives_a_key_rotation_until_its_key_is_dropped, fixture) {
    user("alice");
    role("app", trust_for(k_alice), 43200);
    const auto before = creds_of(assume(principal("alice"), "app", "&DurationSeconds=43200"));
    rotate(/*retire_in=*/600);
    const auto after = creds_of(assume(principal("alice"), "app", "&DurationSeconds=43200"));
    // Both keys are held: both tokens work, and the new one uses the new key.
    BOOST_TEST(authenticate(before).has_value());
    BOOST_TEST(authenticate(after).has_value());
    BOOST_TEST(before.token.substr(0, 12) != after.token.substr(0, 12));
    // Once the old key's window passes, the next STS-key command drops it,
    // and its token reads as expired though its own Expiration is hours off.
    clock += std::chrono::seconds(601);
    rotate();
    const auto old = authenticate(before);
    BOOST_REQUIRE(!old.has_value());
    BOOST_TEST(old.error().code == "ExpiredToken");
    BOOST_TEST(authenticate(after).has_value());
}

BOOST_FIXTURE_TEST_CASE(a_forged_or_borrowed_token_does_not_authenticate, fixture) {
    user("alice");
    role("app", trust_for(k_alice));
    const auto k = creds_of(assume(principal("alice"), "app"));
    auto forged = k;
    forged.token[forged.token.size() / 2] =
        forged.token[forged.token.size() / 2] == 'A' ? 'B' : 'A';
    BOOST_TEST(authenticate(forged).error().code == "InvalidClientTokenId");
    // Another session's key id with this token: refused.
    const auto other = creds_of(assume(principal("alice"), "app"));
    auto borrowed = k;
    borrowed.id = other.id;
    BOOST_TEST(authenticate(borrowed).error().code == "InvalidClientTokenId");
    // The right token with the wrong secret: the signature fails.
    auto wrong = k;
    wrong.secret = other.secret;
    BOOST_TEST(authenticate(wrong).error().code == "SignatureDoesNotMatch");
    // A token sealed under a key from another cluster is refused.
    std::array<unsigned char, 32> b{};
    b.fill(1);
    sts::session s;
    s.exp = now_s() + 600;
    s.access_key_id = k.id;
    s.secret_access_key = k.secret;
    auto alien = k;
    alien.token = sts::seal({current_kid, iam::seal_key(b)}, s);
    BOOST_TEST(authenticate(alien).error().code == "InvalidClientTokenId");
}

BOOST_FIXTURE_TEST_CASE(inv_dur_5_and_6_session_token_durations, fixture) {
    user("alice");
    const auto expires_in = [&](const iam::query::reply& r) {
        std::tm tm{};
        strptime(element(r.body, "Expiration").c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return static_cast<std::int64_t>(timegm(&tm)) - now_s();
    };
    // INV-DUR-5: an IAM user gets 900-129600 seconds, default 43200.
    auto r = call(principal("alice"), "Action=GetSessionToken");
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(std::abs(expires_in(r) - 43200) <= 5);
    r = call(principal("alice"), "Action=GetSessionToken&DurationSeconds=129600");
    BOOST_TEST(std::abs(expires_in(r) - 129600) <= 5);
    BOOST_TEST(code_of(call(principal("alice"), "Action=GetSessionToken&DurationSeconds=129601")) ==
               "ValidationError");
    BOOST_TEST(code_of(call(principal("alice"), "Action=GetSessionToken&DurationSeconds=899")) ==
               "ValidationError");
    // INV-DUR-6: the root defaults to 3600, and longer is shortened, not
    // refused.
    r = call(root(), "Action=GetSessionToken");
    BOOST_TEST(std::abs(expires_in(r) - 3600) <= 5);
    r = call(root(), "Action=GetSessionToken&DurationSeconds=7200");
    BOOST_TEST(r.status == 200U);
    BOOST_TEST(std::abs(expires_in(r) - 3600) <= 5);
}

BOOST_FIXTURE_TEST_CASE(session_token_credentials_act_as_the_user_with_limits, fixture) {
    user("alice", k_a, k_get_object);
    role("app", trust_for(k_alice));
    const auto st = authenticate(creds_of(call(principal("alice"), "Action=GetSessionToken")));
    BOOST_REQUIRE(st.has_value());
    BOOST_TEST((st->type == aws_auth::principal_type::user));
    BOOST_TEST(st->session_token);
    BOOST_TEST(st->arn == k_alice);
    // The user's policies apply.
    BOOST_TEST(
        authz.authorize(*st, {.action = "s3:GetObject", .resource = "arn:aws:s3:::b/k"}).allowed);
    // Requirement 10.10: only AssumeRole and GetCallerIdentity in STS.
    BOOST_TEST(call(*st, "Action=GetCallerIdentity").status == 200U);
    BOOST_TEST(assume(*st, "app").status == 200U);
    const auto again = call(*st, "Action=GetSessionToken");
    BOOST_TEST(code_of(again) == "AccessDenied");
    BOOST_TEST(again.body.find("Cannot call GetSessionToken with session credentials") !=
               std::string::npos);
    // A role session cannot call GetSessionToken either.
    const auto rs = authenticate(creds_of(assume(principal("alice"), "app")));
    BOOST_REQUIRE(rs.has_value());
    BOOST_TEST(code_of(call(*rs, "Action=GetSessionToken")) == "AccessDenied");
    // The root's session token names the root.
    const auto rt = authenticate(creds_of(call(root(), "Action=GetSessionToken")));
    BOOST_REQUIRE(rt.has_value());
    BOOST_TEST((rt->type == aws_auth::principal_type::root));
}

BOOST_FIXTURE_TEST_CASE(inv_sts_4_and_5_mfa_parameters_are_checked_then_refused, fixture) {
    user("alice");
    const auto get = [&](const std::string& extra) {
        return call(principal("alice"), "Action=GetSessionToken" + extra);
    };
    // INV-STS-4: 9-256 characters of [\w+=/:,.@-].
    BOOST_TEST(code_of(get("&SerialNumber=short")) == "ValidationError");
    BOOST_TEST(code_of(get("&SerialNumber=" + std::string(257, 'a'))) == "ValidationError");
    BOOST_TEST(code_of(get("&SerialNumber=" + enc("arn:aws:iam::1:mfa/a b"))) == "ValidationError");
    // INV-STS-5: exactly 6 digits.
    BOOST_TEST(code_of(get("&TokenCode=12345")) == "ValidationError");
    BOOST_TEST(code_of(get("&TokenCode=1234567")) == "ValidationError");
    BOOST_TEST(code_of(get("&TokenCode=12345a")) == "ValidationError");
    // Well formed, they are still refused: no MFA device can exist.
    BOOST_TEST(code_of(get("&SerialNumber=" + enc("arn:aws:iam::111122223333:mfa/alice") +
                           "&TokenCode=123456")) == "AccessDenied");
    BOOST_TEST(code_of(get("&TokenCode=123456")) == "AccessDenied");
}

BOOST_FIXTURE_TEST_CASE(inv_sts_7_minimum_session_token_size_is_checked, fixture) {
    user("alice");
    role("app", trust_for(k_alice));
    BOOST_TEST(code_of(assume(principal("alice"), "app", "&MinimumSessionTokenSize=4097")) ==
               "ValidationError");
    BOOST_TEST(code_of(assume(principal("alice"), "app", "&MinimumSessionTokenSize=-1")) ==
               "ValidationError");
    BOOST_TEST(assume(principal("alice"), "app", "&MinimumSessionTokenSize=4096").status == 200U);
    BOOST_TEST(assume(principal("alice"), "app", "&MinimumSessionTokenSize=0").status == 200U);
}

BOOST_FIXTURE_TEST_CASE(no_sts_key_means_no_credentials, fixture) {
    user("alice");
    role("app", trust_for(k_alice));
    {
        const std::shared_lock lock(mu);
        for (const auto& k : sm.sts_keys()) {
            BOOST_TEST_REQUIRE(!k.retire_after.has_value());
        }
    }
    // Retire every key at once: nothing is left to sign with.
    for (const auto& k : std::vector<iam::sts_key_record>(sm.sts_keys())) {
        run(iam_command::retire_sts_key, {{"kid", k.kid}, {"retire_after", now_s() + 60}});
    }
    BOOST_TEST(code_of(assume(principal("alice"), "app")) == "ServiceFailure");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(identity_calls)

BOOST_FIXTURE_TEST_CASE(inv_sts_b4_get_caller_identity_is_never_denied, fixture) {
    user("mallory", k_a, R"({"Effect":"Deny","Action":"*","Resource":"*"})");
    const auto r = call(principal("mallory"), "Action=GetCallerIdentity");
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(element(r.body, "Arn") == "arn:aws:iam::111122223333:user/mallory");
    BOOST_TEST(element(r.body, "UserId") == "AIDAmallory");
    BOOST_TEST(element(r.body, "Account") == k_a);
    // Nor for a role session whose role denies everything. The trust
    // policy names bob, who has no policies, so it alone allows the call.
    user("bob");
    role("locked", trust_for("arn:aws:iam::111122223333:user/bob"), 3600,
         R"({"Effect":"Deny","Action":"*","Resource":"*"})");
    const auto r2 = assume(principal("bob"), "locked");
    BOOST_TEST(r2.status == 200U, r2.body);  // the trust policy alone allows it
    const auto who = authenticate(creds_of(r2));
    BOOST_REQUIRE(who.has_value());
    BOOST_TEST(call(*who, "Action=GetCallerIdentity").status == 200U);
}

BOOST_FIXTURE_TEST_CASE(get_access_key_info_names_the_keys_account, fixture) {
    user("alice", k_a, R"({"Effect":"Allow","Action":"sts:GetAccessKeyInfo","Resource":"*"})");
    role("app", trust_for(k_alice));
    auto k = leader::make_access_key(kek, k_b, "", now_s());
    {
        const std::unique_lock lock(mu);
        (void)sm.apply(k.command, ++index);
    }
    const auto alice = principal("alice");
    auto r = call(alice, "Action=GetAccessKeyInfo&AccessKeyId=" + k.key_id);
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(element(r.body, "Account") == k_b);
    // A temporary key carries its account in its id.
    const auto temp = creds_of(assume(alice, "app"));
    BOOST_TEST(element(call(alice, "Action=GetAccessKeyInfo&AccessKeyId=" + temp.id).body,
                       "Account") == k_a);
    BOOST_TEST(code_of(call(alice, "Action=GetAccessKeyInfo&AccessKeyId=AKIANOTAKEYATALL0000")) ==
               "InvalidParameterValue");
    user("eve");
    BOOST_TEST(code_of(call(principal("eve"), "Action=GetAccessKeyInfo&AccessKeyId=" + k.key_id)) ==
               "AccessDenied");
}

BOOST_AUTO_TEST_SUITE_END()
