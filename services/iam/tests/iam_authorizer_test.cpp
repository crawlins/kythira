// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The authorizer hook and service registry (`.kiro/specs/iam-compatible-auth/`
// task 13, Requirement 7): authenticating AKIA keys against a state
// machine, authorizing through evaluate(), refusing unregistered actions,
// and an audit trail that holds no secrets.

#define BOOST_TEST_MODULE iam_authorizer_test

#include <boost/test/unit_test.hpp>

#include <kythira/iam/iam_authorizer.hpp>
#include <kythira/iam/iam_replica.hpp>
#include <kythira/iam/iam_service_registry.hpp>
#include <kythira/iam/iam_state_machine.hpp>
#include <kythira/iam/services/elasticache_actions.hpp>
#include <kythira/iam/services/iam_actions.hpp>
#include <kythira/iam/services/sts_actions.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace iam = kythira::iam;
namespace aws_auth = kythira::aws_auth;
namespace sigv4 = kythira::sigv4;
namespace leader = kythira::iam::iam_leader;
using iam::iam_command;

namespace {

constexpr std::string_view k_account = "111122223333";
constexpr std::int64_t k_t0 = 1'780'000'000;

auto test_kek() -> iam::seal_key {
    std::array<unsigned char, iam::seal_key::size> b{};
    for (std::size_t i = 0; i < b.size(); ++i) {
        b[i] = static_cast<unsigned char>(0x40 + i);
    }
    return iam::seal_key(b);
}

auto text(const std::vector<std::byte>& v) -> std::string {
    return {reinterpret_cast<const char*>(
                v.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            v.size()};
}

auto error_of(const std::vector<std::byte>& v) -> std::string {
    const auto o = boost::json::parse(text(v)).as_object();
    const auto* e = o.if_contains("error");
    return e == nullptr ? std::string() : std::string(e->as_string());
}

/// A state machine behind a shared mutex, an authorizer over it, and the
/// audit lines it wrote.
struct fixture {
    iam::iam_state_machine sm;
    std::shared_mutex mu;
    std::uint64_t index = 0;
    std::int64_t now = k_t0;
    iam::seal_key kek = test_kek();
    iam::iam_service_registry registry;
    std::mutex lines_mu;
    std::vector<std::string> lines;
    iam::audit_log audit{[this](std::string_view l) {
        const std::lock_guard lock(lines_mu);
        lines.emplace_back(l);
    }};
    std::chrono::system_clock::time_point clock{std::chrono::seconds{k_t0}};
    iam::iam_authorizer authz{
        [this](const std::function<void(const iam::iam_state_machine&)>& body) {
            const std::shared_lock lock(mu);
            body(sm);
        },
        [this](std::string_view) -> const iam::seal_key& { return kek; }, registry, &audit,
        [this] { return clock; }};

    explicit fixture(std::string partition = "aws")
        : sm(iam::iam_config{.partition = std::move(partition)}) {
        registry.add(iam::services::iam_actions());
        registry.add(iam::services::sts_actions());
        registry.add({.service_prefix = "s3", .actions = {{"GetObject", {"*"}, {}}}});
        BOOST_TEST_REQUIRE(
            error_of(run(iam_command::create_account,
                         {{"account", k_account}, {"kek_salt", "c2FsdHNhbHRzYWx0c2FsdA"}}))
                .empty());
    }

    auto run(iam_command type, boost::json::object body) -> std::vector<std::byte> {
        if (!body.contains("account")) {
            body["account"] = k_account;
        }
        const std::unique_lock lock(mu);
        return sm.apply(leader::command(type, std::move(body), ++now), ++index);
    }

    auto key_for(std::string_view user) -> leader::new_access_key {
        auto k = leader::make_access_key(kek, k_account, user, ++now);
        const std::unique_lock lock(mu);
        BOOST_TEST_REQUIRE(error_of(sm.apply(k.command, ++index)).empty());
        return k;
    }

    auto audit_text() -> std::string {
        audit.flush();
        const std::lock_guard lock(lines_mu);
        std::string all;
        for (const auto& l : lines) {
            all += l + "\n";
        }
        return all;
    }
};

const sigv4::service_rules k_iam_rules{.service = "iam"};

/// A signed IAM Query request (POST, form body `body`, which must outlive
/// it) from `key_id`/`secret`.
auto signed_request(std::string_view key_id, std::string_view secret, const std::string& body,
                    std::chrono::system_clock::time_point at) -> sigv4::request {
    const auto amz = sigv4::detail::format_amz_date(std::chrono::floor<std::chrono::seconds>(at));
    const auto date = amz.substr(0, 8);
    sigv4::request r;
    r.method = "POST";
    r.raw_path = "/";
    r.raw_query = "";
    r.body = std::string_view(body);
    const auto auth = [&](std::string_view sig) {
        return "AWS4-HMAC-SHA256 Credential=" + std::string(key_id) + "/" + date +
               "/us-east-1/iam/aws4_request, SignedHeaders=content-type;host;x-amz-date, "
               "Signature=" +
               std::string(sig);
    };
    r.headers = {{"Host", "iam.amazonaws.com"},
                 {"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"},
                 {"X-Amz-Date", amz},
                 {"Authorization", auth(std::string(64, '0'))}};
    const auto parsed = sigv4::parse(r, k_iam_rules, std::chrono::floor<std::chrono::seconds>(at));
    BOOST_TEST_REQUIRE(parsed.has_value(), parsed.error().message);
    const auto sig =
        sigv4::compute_signature(*parsed, sigv4::derive_signing_key(secret, parsed->scope));
    r.headers.back().second = auth(sig);
    return r;
}

const std::string k_list_users = "Action=ListUsers";

}  // namespace

BOOST_AUTO_TEST_SUITE(service_registry)

BOOST_AUTO_TEST_CASE(finds_registered_actions_in_any_case) {
    iam::iam_service_registry reg;
    reg.add(iam::services::iam_actions());
    BOOST_TEST(reg.find("iam:CreateUser") != nullptr);
    BOOST_TEST(reg.find("IAM:createuser") != nullptr);
    BOOST_TEST(reg.find("iam:NoSuchAction") == nullptr);
    BOOST_TEST(reg.find("kinesis:PutRecord") == nullptr);
    BOOST_TEST(reg.find("CreateUser") == nullptr);
    BOOST_TEST(reg.serves("iam"));
    BOOST_TEST(!reg.serves("sts"));
    const auto* pass = reg.find("iam:PassRole");
    BOOST_REQUIRE(pass != nullptr);
    BOOST_TEST(pass->condition_keys.size() == 2U);
}

BOOST_AUTO_TEST_CASE(the_shipped_tables_register_together) {
    iam::iam_service_registry reg;
    reg.add(iam::services::iam_actions());
    reg.add(iam::services::sts_actions());
    reg.add(iam::services::elasticache_actions());
    const auto* connect = reg.find("elasticache:Connect");
    BOOST_REQUIRE(connect != nullptr);
    BOOST_TEST(connect->resource_templates.size() == 2U);
    BOOST_TEST(reg.find("sts:AssumeRole") != nullptr);
    BOOST_TEST(reg.find("elasticache:CreateReplicationGroup") == nullptr);
}

BOOST_AUTO_TEST_CASE(refuses_a_prefix_twice_and_duplicate_actions) {
    iam::iam_service_registry reg;
    reg.add(iam::services::sts_actions());
    BOOST_CHECK_THROW(reg.add(iam::services::sts_actions()), std::invalid_argument);
    BOOST_CHECK_THROW(
        reg.add({.service_prefix = "x", .actions = {{"A", {"*"}, {}}, {"a", {"*"}, {}}}}),
        std::invalid_argument);
    BOOST_CHECK_THROW(reg.add({.service_prefix = "", .actions = {}}), std::invalid_argument);
    // A refused table leaves nothing behind.
    BOOST_TEST(!reg.serves("x"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(authentication)

BOOST_AUTO_TEST_CASE(a_user_key_authenticates_as_the_user) {
    fixture f;
    f.run(iam_command::create_user,
          {{"name", "alice"}, {"id", "AIDAALICE00000000001"}, {"path", "/eng/"}});
    const auto k = f.key_for("alice");
    const auto req = signed_request(k.key_id, k.secret, k_list_users, f.clock);
    const auto who = f.authz.authenticate(req, k_iam_rules);
    BOOST_TEST_REQUIRE(who.has_value(), who.error().message);
    BOOST_TEST((who->type == aws_auth::principal_type::user));
    BOOST_TEST(who->arn == "arn:aws:iam::111122223333:user/eng/alice");
    BOOST_TEST(who->username == "alice");
    BOOST_TEST(who->userid == "AIDAALICE00000000001");
    BOOST_TEST(who->access_key_id == k.key_id);
}

BOOST_AUTO_TEST_CASE(a_root_key_authenticates_as_the_account) {
    fixture f;
    const auto k = f.key_for("");
    const auto req = signed_request(k.key_id, k.secret, k_list_users, f.clock);
    const auto who = f.authz.authenticate(req, k_iam_rules);
    BOOST_TEST_REQUIRE(who.has_value(), who.error().message);
    BOOST_TEST((who->type == aws_auth::principal_type::root));
    BOOST_TEST(who->arn == "arn:aws:iam::111122223333:root");
}

// Requirement 7.5: the configured partition is in every ARN produced,
// and an ARN in another partition matches nothing.
BOOST_AUTO_TEST_CASE(the_configured_partition_is_in_every_arn) {
    fixture f("aws-cn");
    f.run(iam_command::create_user, {{"name", "alice"}, {"id", "AIDAALICE00000000001"}});
    f.run(iam_command::put_inline_policy,
          {{"kind", "user"},
           {"name", "alice"},
           {"policy_name", "own"},
           {"document",
            R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"s3:GetObject",)"
            R"("Resource":"*","Condition":{"StringEquals":{"aws:PrincipalArn":)"
            R"("arn:aws-cn:iam::111122223333:user/alice"}}}]})"}});
    const auto root = f.key_for("");
    const auto rreq = signed_request(root.key_id, root.secret, k_list_users, f.clock);
    const auto r = f.authz.authenticate(rreq, k_iam_rules);
    BOOST_TEST_REQUIRE(r.has_value(), r.error().message);
    BOOST_TEST(r->arn == "arn:aws-cn:iam::111122223333:root");

    const auto k = f.key_for("alice");
    const auto req = signed_request(k.key_id, k.secret, k_list_users, f.clock);
    const auto who = f.authz.authenticate(req, k_iam_rules);
    BOOST_TEST_REQUIRE(who.has_value(), who.error().message);
    BOOST_TEST(who->arn == "arn:aws-cn:iam::111122223333:user/alice");
    BOOST_TEST(f.authz.authorize(*who, {.action = "s3:GetObject"}).allowed);
}

BOOST_AUTO_TEST_CASE(a_wrong_secret_is_a_signature_mismatch) {
    fixture f;
    f.run(iam_command::create_user, {{"name", "alice"}, {"id", "AIDAALICE00000000001"}});
    const auto k = f.key_for("alice");
    const auto req = signed_request(k.key_id, "not-the-secret", k_list_users, f.clock);
    const auto who = f.authz.authenticate(req, k_iam_rules);
    BOOST_TEST(!who.has_value());
    BOOST_TEST(who.error().code == "SignatureDoesNotMatch");
}

BOOST_AUTO_TEST_CASE(inv_key_4_unknown_inactive_deleted_and_temporary_keys_are_invalid_tokens) {
    fixture f;
    f.run(iam_command::create_user, {{"name", "alice"}, {"id", "AIDAALICE00000000001"}});
    const auto k = f.key_for("alice");

    const auto unknown = signed_request("AKIAUNKNOWNFIXTURE01", k.secret, k_list_users, f.clock);
    BOOST_TEST(f.authz.authenticate(unknown, k_iam_rules).error().code == "InvalidClientTokenId");

    const auto temp = signed_request("ASIATESTFIXTUREKEY01", k.secret, k_list_users, f.clock);
    BOOST_TEST(f.authz.authenticate(temp, k_iam_rules).error().code == "InvalidClientTokenId");

    f.run(iam_command::update_access_key, {{"key_id", k.key_id}, {"status", "Inactive"}});
    const auto inactive = signed_request(k.key_id, k.secret, k_list_users, f.clock);
    BOOST_TEST(f.authz.authenticate(inactive, k_iam_rules).error().code == "InvalidClientTokenId");

    // A key that worked stops working once deleted, even with its signing
    // key already cached.
    const auto k2 = f.key_for("alice");
    const auto good = signed_request(k2.key_id, k2.secret, k_list_users, f.clock);
    BOOST_TEST(f.authz.authenticate(good, k_iam_rules).has_value());
    f.run(iam_command::delete_access_key, {{"key_id", k2.key_id}});
    BOOST_TEST(f.authz.authenticate(good, k_iam_rules).error().code == "InvalidClientTokenId");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(authorization)

BOOST_AUTO_TEST_CASE(user_group_and_boundary_policies_are_gathered) {
    fixture f;
    f.run(iam_command::create_user, {{"name", "alice"}, {"id", "AIDAALICE00000000001"}});
    f.run(iam_command::create_group, {{"name", "devs"}, {"id", "AGPADEVS000000000001"}});
    f.run(iam_command::add_user_to_group, {{"group", "devs"}, {"user", "alice"}});
    f.run(
        iam_command::put_inline_policy,
        {{"kind", "group"},
         {"name", "devs"},
         {"policy_name", "ListUsers"},
         {"document",
          R"({"Version":"2012-10-17","Statement":[{"Sid":"List","Effect":"Allow","Action":"iam:ListUsers","Resource":"*"}]})"}});
    aws_auth::principal_context who;
    who.type = aws_auth::principal_type::user;
    who.account = std::string(k_account);
    who.arn = "arn:aws:iam::111122223333:user/alice";
    who.username = "alice";

    auto d = f.authz.authorize(who, {.action = "iam:ListUsers"});
    BOOST_TEST(d.allowed, d.reason);
    BOOST_TEST(d.policy == "group/devs:ListUsers");
    BOOST_TEST(d.sid == "List");
    BOOST_TEST(!f.authz.authorize(who, {.action = "iam:ListRoles"}).allowed);

    // A boundary that does not allow ListUsers takes it away.
    f.run(
        iam_command::create_policy,
        {{"name", "OnlyRoles"},
         {"id", "ANPAONLYROLES0000001"},
         {"document",
          R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"iam:ListRoles","Resource":"*"}]})"}});
    f.run(iam_command::put_boundary, {{"kind", "user"},
                                      {"name", "alice"},
                                      {"arn", "arn:aws:iam::111122223333:policy/OnlyRoles"}});
    d = f.authz.authorize(who, {.action = "iam:ListUsers"});
    BOOST_TEST(!d.allowed);
    BOOST_TEST(d.reason == "the permissions boundary does not allow");
}

BOOST_AUTO_TEST_CASE(an_unregistered_action_is_denied_whatever_the_policy) {
    fixture f;
    aws_auth::principal_context root;
    root.type = aws_auth::principal_type::root;
    root.account = std::string(k_account);
    root.arn = "arn:aws:iam::111122223333:root";
    BOOST_TEST(f.authz.authorize(root, {.action = "iam:ListUsers"}).allowed);
    const auto d = f.authz.authorize(root, {.action = "kinesis:PutRecord"});
    BOOST_TEST(!d.allowed);
    BOOST_TEST(d.reason == "the action is not registered by any service");
}

BOOST_AUTO_TEST_CASE(role_sessions_use_the_role_and_its_session_policies) {
    fixture f;
    f.run(
        iam_command::create_role,
        {{"name", "reader"},
         {"id", "AROAREADER0000000001"},
         {"trust_policy",
          R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"AWS":"111122223333"},"Action":"sts:AssumeRole"}]})"}});
    f.run(
        iam_command::put_inline_policy,
        {{"kind", "role"},
         {"name", "reader"},
         {"policy_name", "Get"},
         {"document",
          R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"s3:GetObject","Resource":"*"}]})"}});
    aws_auth::principal_context who;
    who.type = aws_auth::principal_type::assumed_role;
    who.account = std::string(k_account);
    who.arn = "arn:aws:sts::111122223333:assumed-role/reader/s1";
    who.role_arn = "arn:aws:iam::111122223333:role/reader";
    who.role_name = "reader";
    BOOST_TEST(
        f.authz.authorize(who, {.action = "s3:GetObject", .resource = "arn:aws:s3:::b/k"}).allowed);
    who.session_policies = {
        R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"s3:PutObject","Resource":"*"}]})"};
    BOOST_TEST(!f.authz.authorize(who, {.action = "s3:GetObject", .resource = "arn:aws:s3:::b/k"})
                    .allowed);
}

BOOST_AUTO_TEST_CASE(principal_tags_and_service_keys_reach_conditions) {
    fixture f;
    f.run(iam_command::create_user,
          {{"name", "alice"},
           {"id", "AIDAALICE00000000001"},
           {"tags", boost::json::array{boost::json::object{{"Key", "team"}, {"Value", "blue"}}}}});
    f.run(
        iam_command::put_inline_policy,
        {{"kind", "user"},
         {"name", "alice"},
         {"policy_name", "Abac"},
         {"document",
          R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"s3:GetObject","Resource":"*","Condition":{"StringEquals":{"aws:ResourceTag/team":"${aws:PrincipalTag/team}","s3:prefix":"docs/"}}}]})"}});
    aws_auth::principal_context who;
    who.type = aws_auth::principal_type::user;
    who.account = std::string(k_account);
    who.arn = "arn:aws:iam::111122223333:user/alice";
    who.username = "alice";
    aws_auth::authz_request r{.action = "s3:GetObject", .resource = "arn:aws:s3:::b/k"};
    r.resource_tags = {{"team", "blue"}};
    r.keys = {{"s3:prefix", {"docs/"}}};
    BOOST_TEST(f.authz.authorize(who, r).allowed);
    r.resource_tags = {{"team", "red"}};
    BOOST_TEST(!f.authz.authorize(who, r).allowed);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(audit)

BOOST_AUTO_TEST_CASE(audit_lines_name_the_decision_and_hold_no_secrets) {
    fixture f;
    f.run(iam_command::create_user, {{"name", "alice"}, {"id", "AIDAALICE00000000001"}});
    f.run(
        iam_command::put_inline_policy,
        {{"kind", "user"},
         {"name", "alice"},
         {"policy_name", "List"},
         {"document",
          R"({"Version":"2012-10-17","Statement":[{"Sid":"L","Effect":"Allow","Action":"iam:ListUsers","Resource":"*"}]})"}});
    const auto k = f.key_for("alice");
    const auto req = signed_request(k.key_id, k.secret, k_list_users, f.clock);
    const auto who = f.authz.authenticate(req, k_iam_rules);
    BOOST_TEST_REQUIRE(who.has_value(), who.error().message);
    (void)f.authz.authorize(*who, {.action = "iam:ListUsers", .source_ip = "203.0.113.5"});
    (void)f.authz.authorize(*who, {.action = "iam:DeleteUser", .resource = who->arn});

    const auto all = f.audit_text();
    BOOST_TEST(all.find(R"("decision":"allow")") != std::string::npos);
    BOOST_TEST(all.find(R"("sid":"L")") != std::string::npos);
    BOOST_TEST(all.find(R"("decision":"implicit_deny")") != std::string::npos);
    BOOST_TEST(all.find(k.key_id) != std::string::npos);
    BOOST_TEST(all.find("203.0.113.5") != std::string::npos);
    // No secret, sealed secret, signature or authorization header.
    BOOST_TEST(all.find(k.secret) == std::string::npos);
    BOOST_TEST(all.find(f.sm.access_key(k.key_id).second->sealed_secret) == std::string::npos);
    BOOST_TEST(all.find("Signature") == std::string::npos);
    for (const auto& [name, value] : req.headers) {
        if (name == "Authorization") {
            const auto sig = value.substr(value.rfind('=') + 1);
            BOOST_TEST(all.find(sig) == std::string::npos);
        }
    }
}

BOOST_AUTO_TEST_CASE(a_full_queue_counts_drops_instead_of_blocking) {
    std::mutex gate;
    std::unique_lock hold(gate);
    std::vector<std::string> out;
    {
        iam::audit_log log(
            [&](std::string_view l) {
                const std::lock_guard g(gate);
                out.emplace_back(l);
            },
            2);
        for (int i = 0; i < 50; ++i) {
            log.post("line " + std::to_string(i));
        }
        BOOST_TEST(log.dropped() > 0U);
        hold.unlock();
        log.flush();
    }
    // The writer may take its first batch before the queue fills, report
    // the drops after that batch, and then write lines queued later, so the
    // drop report is somewhere in the output, not necessarily last.
    BOOST_TEST(std::any_of(out.begin(), out.end(), [](const std::string& l) {
        return l.find(R"("audit":"dropped")") != std::string::npos;
    }));
}

BOOST_AUTO_TEST_SUITE_END()

// Requirement 7.4: a replica's staleness, its metric, and failing closed.
BOOST_AUTO_TEST_SUITE(replica_staleness)

using steady = std::chrono::steady_clock;

BOOST_AUTO_TEST_CASE(lag_is_the_time_since_last_caught_up) {
    std::optional<steady::time_point> caught_up;
    steady::time_point now{std::chrono::seconds{1000}};
    const iam::replica_freshness fr([&] { return caught_up; }, std::chrono::seconds{60},
                                    [&] { return now; });
    // Never caught up: infinitely stale.
    BOOST_TEST(!fr.lag().has_value());
    BOOST_TEST(!fr.fresh());
    BOOST_TEST(fr.metrics_text().find("kythira_iam_replica_lag_seconds +Inf\n") !=
               std::string::npos);
    BOOST_TEST(fr.metrics_text().find("# TYPE kythira_iam_replica_lag_seconds gauge") !=
               std::string::npos);

    caught_up = now - std::chrono::milliseconds{1500};
    BOOST_REQUIRE(fr.lag().has_value());
    BOOST_TEST(fr.lag()->count() == 1.5);
    BOOST_TEST(fr.fresh());
    BOOST_TEST(fr.metrics_text().find("kythira_iam_replica_lag_seconds 1.500\n") !=
               std::string::npos);

    // At the bound still fresh; past it, stale.
    now = *caught_up + std::chrono::seconds{60};
    BOOST_TEST(fr.fresh());
    now += std::chrono::milliseconds{1};
    BOOST_TEST(!fr.fresh());
}

BOOST_AUTO_TEST_CASE(a_stale_replica_fails_closed) {
    fixture f;
    f.run(iam_command::create_user, {{"name", "alice"}, {"id", "AIDAALICE00000000001"}});
    f.run(
        iam_command::put_inline_policy,
        {{"kind", "user"},
         {"name", "alice"},
         {"policy_name", "all"},
         {"document",
          R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"*","Resource":"*"}]})"}});
    const auto k = f.key_for("alice");
    const auto req = signed_request(k.key_id, k.secret, k_list_users, f.clock);
    bool fresh = true;
    f.authz.set_freshness([&] { return fresh; });
    const auto who = f.authz.authenticate(req, k_iam_rules);
    BOOST_TEST_REQUIRE(who.has_value(), who.error().message);
    BOOST_TEST(f.authz.authorize(*who, {.action = "s3:GetObject"}).allowed);

    fresh = false;
    const auto stale = f.authz.authenticate(req, k_iam_rules);
    BOOST_TEST_REQUIRE(!stale.has_value());
    BOOST_TEST(stale.error().code == "ServiceUnavailable");
    BOOST_TEST(stale.error().http_status == 503);
    // A principal authenticated before the replica went stale is denied
    // too, and the audit line says why.
    const auto d = f.authz.authorize(*who, {.action = "s3:GetObject"});
    BOOST_TEST(!d.allowed);
    BOOST_TEST(f.audit_text().find("the IAM replica is stale") != std::string::npos);

    fresh = true;
    BOOST_TEST(f.authz.authenticate(req, k_iam_rules).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
