// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The IAM state machine and its sealing (`.kiro/specs/iam-compatible-auth/`
// task 8, Requirements 4.1-4.8): each command family, the DeleteConflict and
// quota checks, AES-GCM sealing, the snapshot format, and property tests
// that random command sequences leave every replica, and every restore, in
// the same state without a plaintext secret anywhere in the log or snapshot.

#define BOOST_TEST_MODULE iam_state_machine_test

#include <boost/test/unit_test.hpp>

#include <kythira/iam/iam_state_machine.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace iam = kythira::iam;
using iam::iam_command;
namespace leader = kythira::iam::iam_leader;

namespace {

constexpr std::string_view account_id = "111122223333";
constexpr std::int64_t t0 = 1'780'000'000;

const std::string allow_all =
    R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"*","Resource":"*"}]})";
const std::string trust_ec2 =
    R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"ec2.amazonaws.com"},"Action":"sts:AssumeRole"}]})";

auto test_kek() -> iam::seal_key {
    std::array<unsigned char, iam::seal_key::size> b{};
    for (std::size_t i = 0; i < b.size(); ++i) {
        b[i] = static_cast<unsigned char>(0xA0 + i);
    }
    return iam::seal_key(b);
}

auto text(const std::vector<std::byte>& v) -> std::string {
    return {reinterpret_cast<const char*>(v.data()),
            v.size()};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

auto parse(const std::vector<std::byte>& v) -> boost::json::object {
    return boost::json::parse(text(v)).as_object();
}

auto error_of(const std::vector<std::byte>& v) -> std::string {
    const auto o = parse(v);
    const auto* e = o.if_contains("error");
    return e == nullptr ? std::string() : std::string(e->as_string());
}

/// A state machine plus a log index, applying leader-built commands.
struct fixture {
    iam::iam_state_machine sm;
    std::uint64_t index = 0;
    std::int64_t now = t0;

    explicit fixture(iam::iam_config cfg = {}) : sm(std::move(cfg)) {
        BOOST_TEST_REQUIRE(
            error_of(run(iam_command::create_account,
                         {{"account", account_id}, {"kek_salt", "c2FsdHNhbHRzYWx0c2FsdA"}}))
                .empty());
    }

    auto run(iam_command type, boost::json::object body) -> std::vector<std::byte> {
        if (!body.contains("account") && type != iam_command::record_key_last_used &&
            type != iam_command::record_sts_key && type != iam_command::retire_sts_key) {
            body["account"] = account_id;
        }
        return sm.apply(leader::command(type, std::move(body), ++now), ++index);
    }

    auto err(iam_command type, boost::json::object body) -> std::string {
        return error_of(run(type, std::move(body)));
    }

    auto policy_arn(std::string_view name) const -> std::string {
        return sm.arn(account_id, "policy", "/", name);
    }
};

}  // namespace

// ── sealing ────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(sealing)

BOOST_AUTO_TEST_CASE(round_trip_and_associated_data) {
    const auto kek = test_kek();
    const auto blob = iam::seal_secret(kek, "a-secret-value", "AKIAEXAMPLEKEYID0001");
    BOOST_TEST(blob.find("a-secret-value") == std::string::npos);
    const auto opened = iam::open_secret(kek, blob, "AKIAEXAMPLEKEYID0001");
    BOOST_TEST_REQUIRE(opened.has_value());
    BOOST_TEST(*opened == "a-secret-value");
    // Moved onto another record: the associated data no longer matches.
    BOOST_TEST(!iam::open_secret(kek, blob, "AKIAEXAMPLEKEYID0002").has_value());
    // Another key.
    std::array<unsigned char, iam::seal_key::size> other{};
    other.fill(7);
    BOOST_TEST(!iam::open_secret(iam::seal_key(other), blob, "AKIAEXAMPLEKEYID0001").has_value());
    // No key at all: fail closed.
    BOOST_TEST(!iam::open_secret(iam::seal_key{}, blob, "AKIAEXAMPLEKEYID0001").has_value());
}

BOOST_AUTO_TEST_CASE(nonces_differ_and_tampering_fails) {
    const auto kek = test_kek();
    const auto a = iam::seal_secret(kek, "same", "id");
    const auto b = iam::seal_secret(kek, "same", "id");
    BOOST_TEST(a != b);
    for (std::size_t i = 0; i < a.size(); i += 5) {
        auto t = a;
        t[i] = t[i] == 'A' ? 'B' : 'A';
        BOOST_TEST(!iam::open_secret(kek, t, "id").has_value());
    }
    BOOST_TEST(!iam::open_secret(kek, "", "id").has_value());
    BOOST_TEST(!iam::open_secret(kek, "not base64 !!", "id").has_value());
    BOOST_TEST(!iam::open_secret(kek, a.substr(0, 20), "id").has_value());
}

BOOST_AUTO_TEST_CASE(empty_plaintext) {
    const auto kek = test_kek();
    const auto blob = iam::seal_secret(kek, "", "id");
    const auto opened = iam::open_secret(kek, blob, "id");
    BOOST_TEST_REQUIRE(opened.has_value());
    BOOST_TEST(opened->empty());
}

BOOST_AUTO_TEST_CASE(derived_keys_agree_per_salt) {
    const auto salt = iam::generate_kek_salt();
    const auto k1 = iam::derive_seal_key("correct horse", salt);
    const auto k2 = iam::derive_seal_key("correct horse", salt);
    const auto blob = iam::seal_secret(k1, "s", "id");
    BOOST_TEST(iam::open_secret(k2, blob, "id").value_or("") == "s");
    BOOST_TEST(!iam::open_secret(iam::derive_seal_key("wrong", salt), blob, "id").has_value());
    BOOST_TEST(!iam::open_secret(iam::derive_seal_key("correct horse", iam::generate_kek_salt()),
                                 blob, "id")
                    .has_value());
    BOOST_CHECK_THROW((void)iam::derive_seal_key("p", "short"), std::invalid_argument);
    BOOST_CHECK_THROW((void)iam::seal_secret(iam::seal_key{}, "s", "id"), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

// ── accounts ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(accounts)

BOOST_AUTO_TEST_CASE(first_account_sets_the_salt) {
    iam::iam_state_machine sm;
    BOOST_CHECK_THROW(
        sm.apply(leader::command(iam_command::create_account, {{"account", account_id}}, t0), 1),
        std::invalid_argument);
    BOOST_CHECK_THROW(sm.apply(leader::command(iam_command::create_account,
                                               {{"account", "12345"}, {"kek_salt", "x"}}, t0),
                               2),
                      std::invalid_argument);
    fixture f;
    BOOST_TEST(f.sm.kek_salt() == "c2FsdHNhbHRzYWx0c2FsdA");
    BOOST_TEST(f.err(iam_command::create_account, {{"account", account_id}}) ==
               "EntityAlreadyExists");
    BOOST_TEST(f.err(iam_command::create_account,
                     {{"account", "444455556666"}, {"kek_salt", "different"}}) == "InvalidInput");
    BOOST_TEST(f.err(iam_command::create_account, {{"account", "444455556666"}}).empty());
    BOOST_TEST(f.sm.accounts().size() == 2U);
}

BOOST_AUTO_TEST_CASE(aliases_are_unique) {
    fixture f;
    BOOST_TEST(f.err(iam_command::set_account_alias, {{"alias", "prod"}}).empty());
    BOOST_TEST(f.sm.account(account_id)->alias == "prod");
    BOOST_TEST(f.err(iam_command::create_account,
                     {{"account", "444455556666"}, {"alias", "prod"}}) == "EntityAlreadyExists");
    BOOST_TEST(f.err(iam_command::set_account_alias,
                     {{"account", "999999999999"}, {"alias", "x"}}) == "NoSuchEntity");
}

BOOST_AUTO_TEST_CASE(malformed_commands_throw) {
    fixture f;
    const auto bytes = [](std::string_view s) {
        std::vector<std::byte> v(s.size());
        std::ranges::transform(s, v.begin(), [](char c) { return static_cast<std::byte>(c); });
        return v;
    };
    BOOST_CHECK_THROW(f.sm.apply(bytes("not json"), 99), std::invalid_argument);
    BOOST_CHECK_THROW(f.sm.apply(bytes("{}"), 99), std::invalid_argument);
    BOOST_CHECK_THROW(f.sm.apply(bytes(R"({"type":250})"), 99), std::invalid_argument);
    BOOST_CHECK_THROW(f.sm.apply(bytes(R"({"type":10,"account":"111122223333"})"), 99),
                      std::invalid_argument);
    BOOST_TEST(text(f.sm.apply(leader::command(iam_command::noop, {}), 100)) == "{}");
    BOOST_TEST(f.sm.last_applied() == 100U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── users, groups, roles, instance profiles ────────────────────────────────

BOOST_AUTO_TEST_SUITE(principals)

BOOST_AUTO_TEST_CASE(users_are_case_insensitive) {
    fixture f;
    const auto r =
        parse(f.run(iam_command::create_user,
                    {{"name", "Alice"}, {"id", "AIDAAAAAAAAAAAAAAAAA"}, {"path", "/eng/"}}));
    BOOST_TEST(std::string(r.at("arn").as_string()) == "arn:aws:iam::111122223333:user/eng/Alice");
    BOOST_TEST(f.err(iam_command::create_user, {{"name", "alice"}, {"id", "AIDAB"}}) ==
               "EntityAlreadyExists");
    const auto* u = f.sm.user(account_id, "ALICE");
    BOOST_TEST_REQUIRE(u != nullptr);
    BOOST_TEST(u->name == "Alice");
    BOOST_TEST(u->create_date == t0 + 2);
}

BOOST_AUTO_TEST_CASE(rename_user_keeps_memberships_and_keys) {
    fixture f;
    f.run(iam_command::create_user, {{"name", "bob"}, {"id", "AIDA1"}});
    f.run(iam_command::create_user, {{"name", "carol"}, {"id", "AIDA2"}});
    f.run(iam_command::create_group, {{"name", "devs"}, {"id", "AGPA1"}});
    f.run(iam_command::add_user_to_group, {{"group", "devs"}, {"user", "bob"}});
    const auto k = leader::make_access_key(test_kek(), account_id, "bob", ++f.now);
    BOOST_TEST(error_of(f.sm.apply(k.command, ++f.index)).empty());

    // A clash leaves the user untouched, path included.
    BOOST_TEST(f.err(iam_command::update_user,
                     {{"name", "bob"}, {"new_name", "Carol"}, {"new_path", "/x/"}}) ==
               "EntityAlreadyExists");
    BOOST_TEST(f.sm.user(account_id, "bob")->path == "/");

    BOOST_TEST(f.err(iam_command::update_user, {{"name", "bob"}, {"new_name", "Robert"}}).empty());
    BOOST_TEST(f.sm.user(account_id, "bob") == nullptr);
    BOOST_TEST(f.sm.group(account_id, "devs")->members.contains("robert"));
    BOOST_TEST(f.sm.access_key(k.key_id).second->owner == "robert");
    // A change of case only.
    BOOST_TEST(
        f.err(iam_command::update_user, {{"name", "robert"}, {"new_name", "ROBERT"}}).empty());
    BOOST_TEST(f.sm.user(account_id, "robert")->name == "ROBERT");
}

BOOST_AUTO_TEST_CASE(delete_user_conflicts) {
    fixture f;
    f.run(iam_command::create_user, {{"name", "u"}, {"id", "AIDA1"}});
    f.run(iam_command::create_group, {{"name", "g"}, {"id", "AGPA1"}});

    f.run(iam_command::add_user_to_group, {{"group", "g"}, {"user", "u"}});
    BOOST_TEST(f.err(iam_command::delete_user, {{"name", "u"}}) == "DeleteConflict");
    BOOST_TEST(f.err(iam_command::delete_group, {{"name", "g"}}) == "DeleteConflict");
    f.run(iam_command::remove_user_from_group, {{"group", "g"}, {"user", "u"}});

    f.run(iam_command::put_inline_policy,
          {{"kind", "user"}, {"name", "u"}, {"policy_name", "p"}, {"document", allow_all}});
    BOOST_TEST(f.err(iam_command::delete_user, {{"name", "u"}}) == "DeleteConflict");
    BOOST_TEST(f.err(iam_command::delete_inline_policy,
                     {{"kind", "user"}, {"name", "u"}, {"policy_name", "P"}})
                   .empty());
    BOOST_TEST(f.err(iam_command::delete_inline_policy,
                     {{"kind", "user"}, {"name", "u"}, {"policy_name", "P"}}) == "NoSuchEntity");

    const auto admin = f.sm.arn("aws", "policy", "/", "AdministratorAccess");
    f.run(iam_command::attach_policy, {{"kind", "user"}, {"name", "u"}, {"arn", admin}});
    BOOST_TEST(f.err(iam_command::delete_user, {{"name", "u"}}) == "DeleteConflict");
    f.run(iam_command::detach_policy, {{"kind", "user"}, {"name", "u"}, {"arn", admin}});

    const auto k = leader::make_access_key(test_kek(), account_id, "u", ++f.now);
    f.sm.apply(k.command, ++f.index);
    BOOST_TEST(f.err(iam_command::delete_user, {{"name", "u"}}) == "DeleteConflict");
    f.run(iam_command::delete_access_key, {{"key_id", k.key_id}});

    BOOST_TEST(f.err(iam_command::delete_user, {{"name", "u"}}).empty());
    BOOST_TEST(f.err(iam_command::delete_group, {{"name", "g"}}).empty());
    BOOST_TEST(f.err(iam_command::delete_user, {{"name", "u"}}) == "NoSuchEntity");
}

BOOST_AUTO_TEST_CASE(groups_per_user_quota) {
    iam::iam_config cfg;
    cfg.limits.groups_per_user = 2;
    fixture f(cfg);
    f.run(iam_command::create_user, {{"name", "u"}, {"id", "AIDA1"}});
    for (const char* g : {"a", "b", "c"}) {
        f.run(iam_command::create_group, {{"name", g}, {"id", std::string("AGPA") + g}});
    }
    BOOST_TEST(f.err(iam_command::add_user_to_group, {{"group", "a"}, {"user", "u"}}).empty());
    BOOST_TEST(f.err(iam_command::add_user_to_group, {{"group", "b"}, {"user", "u"}}).empty());
    BOOST_TEST(f.err(iam_command::add_user_to_group, {{"group", "a"}, {"user", "u"}}).empty());
    BOOST_TEST(f.err(iam_command::add_user_to_group, {{"group", "c"}, {"user", "u"}}) ==
               "LimitExceeded");
}

BOOST_AUTO_TEST_CASE(rename_group_updates_members) {
    fixture f;
    f.run(iam_command::create_user, {{"name", "u"}, {"id", "AIDA1"}});
    f.run(iam_command::create_group, {{"name", "g"}, {"id", "AGPA1"}});
    f.run(iam_command::create_group, {{"name", "h"}, {"id", "AGPA2"}});
    f.run(iam_command::add_user_to_group, {{"group", "g"}, {"user", "u"}});
    BOOST_TEST(f.err(iam_command::update_group, {{"name", "g"}, {"new_name", "H"}}) ==
               "EntityAlreadyExists");
    BOOST_TEST(f.err(iam_command::update_group, {{"name", "g"}, {"new_name", "ops"}}).empty());
    BOOST_TEST(f.sm.user(account_id, "u")->groups == std::set<std::string>{"ops"});
}

BOOST_AUTO_TEST_CASE(roles_and_instance_profiles) {
    fixture f;
    const auto r = parse(f.run(iam_command::create_role, {{"name", "web"},
                                                          {"id", "AROA1"},
                                                          {"trust_policy", trust_ec2},
                                                          {"max_session_duration", 7200}}));
    BOOST_TEST(std::string(r.at("arn").as_string()) == "arn:aws:iam::111122223333:role/web");
    BOOST_TEST(f.sm.role(account_id, "WEB")->max_session_duration == 7200);
    f.run(iam_command::update_role, {{"name", "web"}, {"description", "front end"}});
    BOOST_TEST(f.sm.role(account_id, "web")->description == "front end");
    f.run(iam_command::update_trust_policy, {{"name", "web"}, {"trust_policy", allow_all}});
    BOOST_TEST(f.sm.role(account_id, "web")->trust_policy == allow_all);

    f.run(iam_command::create_role, {{"name", "db"}, {"id", "AROA2"}, {"trust_policy", trust_ec2}});
    f.run(iam_command::create_instance_profile, {{"name", "ip"}, {"id", "AIPA1"}});
    BOOST_TEST(f.err(iam_command::add_role_to_instance_profile,
                     {{"instance_profile", "ip"}, {"role", "web"}})
                   .empty());
    BOOST_TEST(f.err(iam_command::add_role_to_instance_profile,
                     {{"instance_profile", "ip"}, {"role", "web"}}) == "EntityAlreadyExists");
    BOOST_TEST(f.err(iam_command::add_role_to_instance_profile,
                     {{"instance_profile", "ip"}, {"role", "db"}}) == "LimitExceeded");
    BOOST_TEST(f.err(iam_command::delete_role, {{"name", "web"}}) == "DeleteConflict");
    BOOST_TEST(f.err(iam_command::delete_instance_profile, {{"name", "ip"}}) == "DeleteConflict");
    f.run(iam_command::remove_role_from_instance_profile,
          {{"instance_profile", "ip"}, {"role", "web"}});
    BOOST_TEST(f.err(iam_command::remove_role_from_instance_profile,
                     {{"instance_profile", "ip"}, {"role", "web"}}) == "NoSuchEntity");
    BOOST_TEST(f.err(iam_command::delete_instance_profile, {{"name", "ip"}}).empty());
    BOOST_TEST(f.err(iam_command::delete_role, {{"name", "web"}}).empty());
}

BOOST_AUTO_TEST_CASE(trust_policy_size_quota) {
    iam::iam_config cfg;
    cfg.limits.trust_policy_size = 40;
    fixture f(cfg);
    BOOST_TEST(f.err(iam_command::create_role,
                     {{"name", "r"}, {"id", "AROA1"}, {"trust_policy", trust_ec2}}) ==
               "LimitExceeded");
    BOOST_TEST(f.sm.role(account_id, "r") == nullptr);
}

BOOST_AUTO_TEST_CASE(tags) {
    iam::iam_config cfg;
    cfg.limits.tags_per_entity = 2;
    fixture f(cfg);
    f.run(iam_command::create_user, {{"name", "u"},
                                     {"id", "AIDA1"},
                                     {"tags", boost::json::array{{{"Key", "a"}, {"Value", "1"}}}}});
    BOOST_TEST(f.err(iam_command::tag_entity,
                     {{"kind", "user"},
                      {"name", "u"},
                      {"tags", boost::json::array{{{"Key", "a"}, {"Value", "2"}},
                                                  {{"Key", "b"}, {"Value", "3"}}}}})
                   .empty());
    BOOST_TEST(f.sm.user(account_id, "u")->tags.at("a") == "2");
    BOOST_TEST(f.err(iam_command::tag_entity,
                     {{"kind", "user"},
                      {"name", "u"},
                      {"tags", boost::json::array{{{"Key", "c"}, {"Value", "4"}}}}}) ==
               "LimitExceeded");
    BOOST_TEST(f.sm.user(account_id, "u")->tags.size() == 2U);
    f.run(iam_command::untag_entity,
          {{"kind", "user"}, {"name", "u"}, {"keys", boost::json::array{"a"}}});
    BOOST_TEST(f.sm.user(account_id, "u")->tags.size() == 1U);
    BOOST_TEST(f.err(iam_command::tag_entity,
                     {{"kind", "role"}, {"name", "none"}, {"tags", boost::json::array{}}}) ==
               "NoSuchEntity");
}

BOOST_AUTO_TEST_SUITE_END()

// ── managed and inline policies, boundaries ────────────────────────────────

BOOST_AUTO_TEST_SUITE(policies)

BOOST_AUTO_TEST_CASE(versions) {
    iam::iam_config cfg;
    cfg.limits.policy_versions = 3;
    fixture f(cfg);
    const auto r = parse(f.run(iam_command::create_policy,
                               {{"name", "P"}, {"id", "ANPA1"}, {"document", allow_all}}));
    const auto arn = std::string(r.at("arn").as_string());
    BOOST_TEST(arn == "arn:aws:iam::111122223333:policy/P");
    BOOST_TEST(f.err(iam_command::create_policy,
                     {{"name", "p"}, {"id", "ANPA2"}, {"document", allow_all}}) ==
               "EntityAlreadyExists");

    const auto v2 = parse(f.run(iam_command::create_policy_version,
                                {{"arn", arn}, {"document", allow_all}, {"set_as_default", true}}));
    BOOST_TEST(std::string(v2.at("version_id").as_string()) == "v2");
    BOOST_TEST(f.sm.managed_policy(account_id, "p")->default_version == 2);
    f.run(iam_command::create_policy_version, {{"arn", arn}, {"document", allow_all}});
    BOOST_TEST(f.err(iam_command::create_policy_version, {{"arn", arn}, {"document", allow_all}}) ==
               "LimitExceeded");
    BOOST_TEST(f.err(iam_command::delete_policy_version, {{"arn", arn}, {"version_id", "v2"}}) ==
               "DeleteConflict");
    BOOST_TEST(f.err(iam_command::delete_policy_version, {{"arn", arn}, {"version_id", "v9"}}) ==
               "NoSuchEntity");
    BOOST_TEST(
        f.err(iam_command::set_default_version, {{"arn", arn}, {"version_id", "v1"}}).empty());
    BOOST_TEST(f.err(iam_command::delete_policy, {{"arn", arn}}) ==
               "DeleteConflict");  // >1 version
    f.run(iam_command::delete_policy_version, {{"arn", arn}, {"version_id", "v2"}});
    f.run(iam_command::delete_policy_version, {{"arn", arn}, {"version_id", "v3"}});
    // Version numbers are never reused.
    const auto v4 =
        parse(f.run(iam_command::create_policy_version, {{"arn", arn}, {"document", allow_all}}));
    BOOST_TEST(std::string(v4.at("version_id").as_string()) == "v4");
    f.run(iam_command::delete_policy_version, {{"arn", arn}, {"version_id", "v4"}});
    BOOST_TEST(f.err(iam_command::delete_policy, {{"arn", arn}}).empty());
    BOOST_TEST(f.err(iam_command::delete_policy, {{"arn", arn}}) == "NoSuchEntity");
}

BOOST_AUTO_TEST_CASE(policy_size_quota) {
    iam::iam_config cfg;
    cfg.limits.managed_policy_size = 50;
    fixture f(cfg);
    BOOST_TEST(f.err(iam_command::create_policy,
                     {{"name", "p"}, {"id", "ANPA1"}, {"document", allow_all}}) == "LimitExceeded");
}

BOOST_AUTO_TEST_CASE(aws_managed_policies_are_read_only) {
    fixture f;
    const auto admin = f.sm.arn("aws", "policy", "/", "AdministratorAccess");
    BOOST_TEST(f.err(iam_command::create_policy_version,
                     {{"arn", admin}, {"document", allow_all}}) == "InvalidInput");
    BOOST_TEST(f.err(iam_command::delete_policy, {{"arn", admin}}) == "InvalidInput");
    const auto doc = f.sm.compiled_policy(admin);
    BOOST_TEST_REQUIRE(doc != nullptr);
    BOOST_TEST(doc == f.sm.compiled_policy(admin));  // cached
    BOOST_TEST(f.sm.compiled_policy(f.sm.arn("aws", "policy", "/", "NoSuchPolicy")) == nullptr);
    BOOST_TEST(f.sm.compiled_policy(admin, 7) == nullptr);
    BOOST_TEST(iam::find_aws_managed_policy("administratoraccess").has_value());
}

BOOST_AUTO_TEST_CASE(every_bundled_policy_parses) {
    fixture f;
    BOOST_TEST(iam::aws_managed_policy_data.size() == 6U);
    for (const auto& e : iam::aws_managed_policy_data) {
        BOOST_TEST_CONTEXT(e.name) {
            const auto arn = f.sm.arn("aws", "policy", e.path, e.name);
            BOOST_TEST(f.sm.compiled_policy(arn) != nullptr);
            BOOST_TEST(e.version_id.starts_with("v"));
        }
    }
    for (const char* n : {"ReadOnlyAccess", "IAMFullAccess", "IAMReadOnlyAccess",
                          "AmazonElastiCacheFullAccess", "AmazonElastiCacheReadOnlyAccess"}) {
        BOOST_TEST(iam::find_aws_managed_policy(n).has_value(), n);
    }
}

BOOST_AUTO_TEST_CASE(attachments_count_and_block_delete) {
    iam::iam_config cfg;
    cfg.limits.attached_per_entity = 2;
    fixture f(cfg);
    f.run(iam_command::create_role, {{"name", "r"}, {"id", "AROA1"}, {"trust_policy", trust_ec2}});
    f.run(iam_command::create_group, {{"name", "g"}, {"id", "AGPA1"}});
    for (const char* n : {"a", "b", "c"}) {
        f.run(iam_command::create_policy,
              {{"name", n}, {"id", std::string("ANPA") + n}, {"document", allow_all}});
    }
    BOOST_TEST(f.err(iam_command::attach_policy,
                     {{"kind", "role"}, {"name", "r"}, {"arn", f.policy_arn("a")}})
                   .empty());
    BOOST_TEST(f.err(iam_command::attach_policy,
                     {{"kind", "role"}, {"name", "r"}, {"arn", f.policy_arn("a")}})
                   .empty());
    BOOST_TEST(f.err(iam_command::attach_policy,
                     {{"kind", "group"}, {"name", "g"}, {"arn", f.policy_arn("a")}})
                   .empty());
    BOOST_TEST(f.sm.managed_policy(account_id, "a")->attachment_count == 2U);
    BOOST_TEST(f.err(iam_command::attach_policy,
                     {{"kind", "role"}, {"name", "r"}, {"arn", f.policy_arn("b")}})
                   .empty());
    BOOST_TEST(f.err(iam_command::attach_policy,
                     {{"kind", "role"}, {"name", "r"}, {"arn", f.policy_arn("c")}}) ==
               "LimitExceeded");
    BOOST_TEST(f.err(iam_command::attach_policy,
                     {{"kind", "role"}, {"name", "r"}, {"arn", f.policy_arn("zz")}}) ==
               "NoSuchEntity");
    BOOST_TEST(f.err(iam_command::delete_policy, {{"arn", f.policy_arn("a")}}) == "DeleteConflict");
    BOOST_TEST(f.err(iam_command::delete_role, {{"name", "r"}}) == "DeleteConflict");
    f.run(iam_command::detach_policy,
          {{"kind", "role"}, {"name", "r"}, {"arn", f.policy_arn("a")}});
    f.run(iam_command::detach_policy,
          {{"kind", "group"}, {"name", "g"}, {"arn", f.policy_arn("a")}});
    BOOST_TEST(f.err(iam_command::detach_policy,
                     {{"kind", "group"}, {"name", "g"}, {"arn", f.policy_arn("a")}}) ==
               "NoSuchEntity");
    BOOST_TEST(f.sm.managed_policy(account_id, "a")->attachment_count == 0U);
    BOOST_TEST(f.err(iam_command::delete_policy, {{"arn", f.policy_arn("a")}}).empty());
}

BOOST_AUTO_TEST_CASE(inline_size_quota_is_a_total) {
    iam::iam_config cfg;
    cfg.limits.user_inline_total = 2 * iam::detail::policy_size(allow_all) + 1;
    fixture f(cfg);
    f.run(iam_command::create_user, {{"name", "u"}, {"id", "AIDA1"}});
    auto put = [&](const char* n) {
        return f.err(
            iam_command::put_inline_policy,
            {{"kind", "user"}, {"name", "u"}, {"policy_name", n}, {"document", allow_all}});
    };
    BOOST_TEST(put("one").empty());
    BOOST_TEST(put("two").empty());
    BOOST_TEST(put("ONE").empty());  // replaces, does not add
    BOOST_TEST(put("three") == "LimitExceeded");
    BOOST_TEST(f.sm.user(account_id, "u")->inline_policies.size() == 2U);
}

BOOST_AUTO_TEST_CASE(boundaries) {
    fixture f;
    f.run(iam_command::create_policy, {{"name", "b1"}, {"id", "ANPA1"}, {"document", allow_all}});
    f.run(iam_command::create_policy, {{"name", "b2"}, {"id", "ANPA2"}, {"document", allow_all}});
    f.run(iam_command::create_user,
          {{"name", "u"}, {"id", "AIDA1"}, {"boundary", f.policy_arn("b1")}});
    BOOST_TEST(f.sm.managed_policy(account_id, "b1")->boundary_usage_count == 1U);
    BOOST_TEST(f.err(iam_command::create_user,
                     {{"name", "v"}, {"id", "AIDA2"}, {"boundary", f.policy_arn("none")}}) ==
               "NoSuchEntity");
    BOOST_TEST(f.err(iam_command::delete_policy, {{"arn", f.policy_arn("b1")}}) ==
               "DeleteConflict");
    f.run(iam_command::put_boundary,
          {{"kind", "user"}, {"name", "u"}, {"arn", f.policy_arn("b2")}});
    BOOST_TEST(f.sm.managed_policy(account_id, "b1")->boundary_usage_count == 0U);
    BOOST_TEST(f.sm.managed_policy(account_id, "b2")->boundary_usage_count == 1U);
    BOOST_CHECK_THROW(f.run(iam_command::put_boundary,
                            {{"kind", "group"}, {"name", "u"}, {"arn", f.policy_arn("b2")}}),
                      std::invalid_argument);
    f.run(iam_command::delete_boundary, {{"kind", "user"}, {"name", "u"}});
    BOOST_TEST(f.err(iam_command::delete_boundary, {{"kind", "user"}, {"name", "u"}}) ==
               "NoSuchEntity");
    BOOST_TEST(f.sm.managed_policy(account_id, "b2")->boundary_usage_count == 0U);
    // Deleting a user releases its boundary.
    f.run(iam_command::put_boundary,
          {{"kind", "user"}, {"name", "u"}, {"arn", f.policy_arn("b2")}});
    f.run(iam_command::delete_user, {{"name", "u"}});
    BOOST_TEST(f.sm.managed_policy(account_id, "b2")->boundary_usage_count == 0U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── access keys, OIDC providers, STS keys ──────────────────────────────────

BOOST_AUTO_TEST_SUITE(credentials)

BOOST_AUTO_TEST_CASE(access_keys) {
    fixture f;
    const auto kek = test_kek();
    f.run(iam_command::create_user, {{"name", "u"}, {"id", "AIDA1"}});
    const auto k1 = leader::make_access_key(kek, account_id, "u", ++f.now);
    BOOST_TEST(k1.key_id.size() == 20U);
    BOOST_TEST(k1.key_id.starts_with("AKIA"));
    BOOST_TEST(k1.secret.size() == 40U);
    BOOST_TEST(text(k1.command).find(k1.secret) == std::string::npos);
    BOOST_TEST(error_of(f.sm.apply(k1.command, ++f.index)).empty());
    BOOST_TEST(error_of(f.sm.apply(k1.command, ++f.index)) == "EntityAlreadyExists");
    const auto k2 = leader::make_access_key(kek, account_id, "u", ++f.now);
    BOOST_TEST(error_of(f.sm.apply(k2.command, ++f.index)).empty());
    const auto k3 = leader::make_access_key(kek, account_id, "u", ++f.now);
    BOOST_TEST(error_of(f.sm.apply(k3.command, ++f.index)) == "LimitExceeded");
    const auto k4 = leader::make_access_key(kek, account_id, "nobody", ++f.now);
    BOOST_TEST(error_of(f.sm.apply(k4.command, ++f.index)) == "NoSuchEntity");
    // The account root holds its own two.
    const auto root = leader::make_access_key(kek, account_id, "", ++f.now);
    BOOST_TEST(error_of(f.sm.apply(root.command, ++f.index)).empty());

    const auto [a, key] = f.sm.access_key(k1.key_id);
    BOOST_TEST_REQUIRE(key != nullptr);
    BOOST_TEST(a->id == account_id);
    BOOST_TEST(key->active);
    BOOST_TEST(leader::open_access_key(kek, *key).value_or("") == k1.secret);
    BOOST_TEST(!leader::open_access_key(iam::seal_key{}, *key).has_value());

    f.run(iam_command::update_access_key, {{"key_id", k1.key_id}, {"status", "Inactive"}});
    BOOST_TEST(!f.sm.access_key(k1.key_id).second->active);
    BOOST_CHECK_THROW(
        f.run(iam_command::update_access_key, {{"key_id", k1.key_id}, {"status", "Off"}}),
        std::invalid_argument);
    BOOST_TEST(f.err(iam_command::delete_access_key, {{"key_id", k1.key_id}}).empty());
    BOOST_TEST(f.sm.access_key(k1.key_id).second == nullptr);
    BOOST_TEST(f.err(iam_command::delete_access_key, {{"key_id", k1.key_id}}) == "NoSuchEntity");
}

BOOST_AUTO_TEST_CASE(last_used_batching) {
    fixture f;
    f.run(iam_command::create_user, {{"name", "u"}, {"id", "AIDA1"}});
    const auto k = leader::make_access_key(test_kek(), account_id, "u", ++f.now);
    f.sm.apply(k.command, ++f.index);

    leader::last_used_batcher b(std::chrono::seconds(300));
    BOOST_TEST(b.note(k.key_id, t0 + 100, "s3", "us-east-1"));
    BOOST_TEST(b.note("AKIAGONEGONEGONEGONE", t0 + 100, "s3", "us-east-1"));
    auto cmd = b.take_due(t0 + 100);
    BOOST_TEST_REQUIRE(cmd.has_value());
    f.sm.apply(*cmd, ++f.index);  // the deleted key is skipped
    BOOST_TEST(f.sm.access_key(k.key_id).second->last_used->time == t0 + 100);

    BOOST_TEST(!b.note(k.key_id, t0 + 200, "iam", "us-west-2"));
    BOOST_TEST(!b.take_due(t0 + 200).has_value());
    BOOST_TEST(b.note(k.key_id, t0 + 450, "iam", "us-west-2"));
    cmd = b.take_due(t0 + 450);
    BOOST_TEST_REQUIRE(cmd.has_value());
    f.sm.apply(*cmd, ++f.index);
    const auto& lu = *f.sm.access_key(k.key_id).second->last_used;
    BOOST_TEST(lu.time == t0 + 450);
    BOOST_TEST(lu.service == "iam");
    BOOST_TEST(!b.take_due(t0 + 2000).has_value());  // nothing pending
}

BOOST_AUTO_TEST_CASE(oidc_providers) {
    iam::iam_config cfg;
    cfg.limits.client_ids_per_oidc_provider = 2;
    fixture f(cfg);
    const auto r = parse(f.run(iam_command::create_oidc_provider,
                               {{"url", "issuer.example.com/p"},
                                {"client_ids", boost::json::array{"sts.amazonaws.com"}}}));
    BOOST_TEST(std::string(r.at("arn").as_string()) ==
               "arn:aws:iam::111122223333:oidc-provider/issuer.example.com/p");
    BOOST_TEST(f.err(iam_command::create_oidc_provider, {{"url", "issuer.example.com/p"}}) ==
               "EntityAlreadyExists");
    BOOST_TEST(f.err(iam_command::update_oidc_provider,
                     {{"url", "issuer.example.com/p"}, {"add_client_id", "b"}})
                   .empty());
    BOOST_TEST(f.err(iam_command::update_oidc_provider,
                     {{"url", "issuer.example.com/p"}, {"add_client_id", "c"}}) == "LimitExceeded");
    BOOST_TEST(
        f.sm.account(account_id)->oidc_providers.at("issuer.example.com/p").client_ids.size() ==
        2U);
    f.run(iam_command::update_oidc_provider,
          {{"url", "issuer.example.com/p"}, {"remove_client_id", "b"}});
    BOOST_TEST(
        f.sm.account(account_id)->oidc_providers.at("issuer.example.com/p").client_ids.size() ==
        1U);
    BOOST_TEST(f.err(iam_command::delete_oidc_provider, {{"url", "issuer.example.com/p"}}).empty());
    BOOST_TEST(f.err(iam_command::delete_oidc_provider, {{"url", "issuer.example.com/p"}}) ==
               "NoSuchEntity");
}

BOOST_AUTO_TEST_CASE(sts_keys_retire_on_the_log_clock) {
    fixture f;
    const auto kek = test_kek();
    f.run(iam_command::record_sts_key,
          {{"kid", "k1"}, {"sealed_key", iam::seal_secret(kek, "x", "k1")}});
    BOOST_TEST(f.err(iam_command::record_sts_key, {{"kid", "k1"}, {"sealed_key", "y"}}) ==
               "EntityAlreadyExists");
    const auto retire_at = f.now + 100;
    BOOST_TEST(
        f.err(iam_command::retire_sts_key, {{"kid", "k1"}, {"retire_after", retire_at}}).empty());
    BOOST_TEST(f.err(iam_command::retire_sts_key, {{"kid", "k1"}, {"retire_after", retire_at}}) ==
               "InvalidInput");
    BOOST_TEST(f.sm.sts_keys().size() == 1U);
    // A later STS command at or past retire_after drops it.
    f.now = retire_at;
    f.run(iam_command::record_sts_key,
          {{"kid", "k2"}, {"sealed_key", iam::seal_secret(kek, "z", "k2")}});
    BOOST_TEST_REQUIRE(f.sm.sts_keys().size() == 1U);
    BOOST_TEST(f.sm.sts_keys()[0].kid == "k2");
    BOOST_TEST(iam::open_secret(kek, f.sm.sts_keys()[0].sealed_key, "k2").value_or("") == "z");
}

BOOST_AUTO_TEST_SUITE_END()

// ── leader helpers ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(names_paths_and_ids) {
    BOOST_TEST(leader::valid_name("user", "a+b=c,d.e@f_g-h"));
    BOOST_TEST(!leader::valid_name("user", ""));
    BOOST_TEST(!leader::valid_name("user", "a b"));
    BOOST_TEST(!leader::valid_name("user", std::string(65, 'a')));
    BOOST_TEST(leader::valid_name("group", std::string(128, 'a')));
    BOOST_TEST(leader::valid_path("/"));
    BOOST_TEST(leader::valid_path("/a/b/"));
    BOOST_TEST(!leader::valid_path("a/"));
    BOOST_TEST(!leader::valid_path("/a"));
    BOOST_TEST(!leader::valid_path("/a b/"));
    const auto id = leader::generate_unique_id("AIDA");
    BOOST_TEST(id.size() == 20U);
    BOOST_TEST(id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567") == std::string::npos);
    BOOST_TEST(id != leader::generate_unique_id("AIDA"));
}

// ── snapshots ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(snapshots)

BOOST_AUTO_TEST_CASE(format_and_partition_refused) {
    fixture f;
    const auto state = f.sm.get_state();
    auto o = parse(state);
    o["format"] = 2;
    const auto newer = iam::iam_detail::bytes_of(o);
    BOOST_CHECK_THROW(f.sm.restore_from_snapshot(newer, 5), std::invalid_argument);
    BOOST_TEST(f.sm.account(account_id) != nullptr);  // untouched

    iam::iam_config other;
    other.partition = "aws-cn";
    iam::iam_state_machine cn(other);
    BOOST_CHECK_THROW(cn.restore_from_snapshot(state, 5), std::invalid_argument);

    std::vector<std::byte> junk{std::byte{'{'}};
    BOOST_CHECK_THROW(f.sm.restore_from_snapshot(junk, 5), std::invalid_argument);

    // An empty snapshot is the empty state.
    f.sm.restore_from_snapshot({}, 7);
    BOOST_TEST(f.sm.accounts().empty());
    BOOST_TEST(f.sm.last_applied() == 7U);
}

BOOST_AUTO_TEST_CASE(managed_policy_digest_recorded) {
    fixture f;
    auto o = parse(f.sm.get_state());
    BOOST_TEST(std::string(o.at("aws_managed").as_string()) == iam::aws_managed_digest());
    BOOST_TEST(iam::aws_managed_digest().size() == 64U);
    iam::iam_state_machine same;
    same.restore_from_snapshot(f.sm.get_state(), 2);
    BOOST_TEST(!same.aws_managed_mismatch());
    // A member bundling another policy set: restored, and reported.
    o["aws_managed"] = std::string(64, '0');
    iam::iam_state_machine other;
    other.restore_from_snapshot(iam::iam_detail::bytes_of(o), 2);
    BOOST_TEST(other.aws_managed_mismatch());
    BOOST_TEST(other.account(account_id) != nullptr);
}

BOOST_AUTO_TEST_CASE(restore_checks_invariants) {
    fixture f;
    f.run(iam_command::create_user, {{"name", "u"}, {"id", "AIDA1"}});
    f.run(iam_command::create_group, {{"name", "g"}, {"id", "AGPA1"}});
    f.run(iam_command::add_user_to_group, {{"group", "g"}, {"user", "u"}});
    auto o = parse(f.sm.get_state());
    // Break one side of the membership.
    auto& g = o["accounts"].as_array()[0].as_object()["groups"].as_array()[0].as_object();
    g["members"] = boost::json::array{};
    iam::iam_state_machine sm;
    BOOST_CHECK_THROW(sm.restore_from_snapshot(iam::iam_detail::bytes_of(o), 3),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

// ── properties ─────────────────────────────────────────────────────────────

namespace {

/// Random commands over small name pools, so they collide, conflict and hit
/// quotas often. Returns the leader-built command bytes and every plaintext
/// secret the leader handed out.
struct generator {
    std::mt19937_64 rng;
    iam::seal_key kek = test_kek();
    std::vector<std::string> secrets;
    std::vector<std::string> key_ids;
    std::int64_t now = t0;

    explicit generator(std::uint64_t seed) : rng(seed) {}

    auto pick(std::initializer_list<const char*> pool) -> std::string {
        std::uniform_int_distribution<std::size_t> d(0, pool.size() - 1);
        return *(pool.begin() + d(rng));
    }
    auto user() { return pick({"ann", "Ann", "ben", "cat"}); }
    auto group() { return pick({"g1", "g2", "G1"}); }
    auto role() { return pick({"r1", "r2"}); }
    auto policy() { return pick({"p1", "p2", "P1"}); }
    auto policy_arn() -> std::string {
        if (rng() % 5 == 0) {
            return "arn:aws:iam::aws:policy/AdministratorAccess";
        }
        return "arn:aws:iam::111122223333:policy/" + policy();
    }
    auto kind() { return pick({"user", "group", "role"}); }
    auto name_of(const std::string& k) -> std::string {
        return k == "user" ? user() : k == "group" ? group() : role();
    }
    auto id() -> std::string { return leader::generate_unique_id("AIDA"); }

    auto next() -> std::vector<std::byte> {
        using enum iam_command;
        ++now;
        const boost::json::string_view a = account_id;
        switch (rng() % 22) {
            case 0:
                return leader::command(create_user,
                                       {{"account", a}, {"name", user()}, {"id", id()}}, now);
            case 1:
                return leader::command(delete_user, {{"account", a}, {"name", user()}}, now);
            case 2:
                return leader::command(
                    update_user, {{"account", a}, {"name", user()}, {"new_name", user()}}, now);
            case 3:
                return leader::command(create_group,
                                       {{"account", a}, {"name", group()}, {"id", id()}}, now);
            case 4:
                return leader::command(delete_group, {{"account", a}, {"name", group()}}, now);
            case 5:
                return leader::command(rng() % 2 != 0 ? add_user_to_group : remove_user_from_group,
                                       {{"account", a}, {"group", group()}, {"user", user()}}, now);
            case 6:
                return leader::command(
                    create_role,
                    {{"account", a}, {"name", role()}, {"id", id()}, {"trust_policy", trust_ec2}},
                    now);
            case 7:
                return leader::command(delete_role, {{"account", a}, {"name", role()}}, now);
            case 8:
                return leader::command(
                    create_policy,
                    {{"account", a}, {"name", policy()}, {"id", id()}, {"document", allow_all}},
                    now);
            case 9:
                return leader::command(delete_policy, {{"account", a}, {"arn", policy_arn()}}, now);
            case 10:
                return leader::command(create_policy_version,
                                       {{"account", a},
                                        {"arn", policy_arn()},
                                        {"document", allow_all},
                                        {"set_as_default", rng() % 2 == 0}},
                                       now);
            case 11:
                return leader::command(delete_policy_version,
                                       {{"account", a},
                                        {"arn", policy_arn()},
                                        {"version_id", pick({"v1", "v2", "v3"})}},
                                       now);
            case 12: {
                const auto k = kind();
                return leader::command(
                    rng() % 2 != 0 ? attach_policy : detach_policy,
                    {{"account", a}, {"kind", k}, {"name", name_of(k)}, {"arn", policy_arn()}},
                    now);
            }
            case 13: {
                const auto k = kind();
                return leader::command(put_inline_policy,
                                       {{"account", a},
                                        {"kind", k},
                                        {"name", name_of(k)},
                                        {"policy_name", pick({"i1", "i2"})},
                                        {"document", allow_all}},
                                       now);
            }
            case 14: {
                const auto k = kind();
                return leader::command(delete_inline_policy,
                                       {{"account", a},
                                        {"kind", k},
                                        {"name", name_of(k)},
                                        {"policy_name", pick({"i1", "i2"})}},
                                       now);
            }
            case 15: {
                const auto k = rng() % 2 != 0 ? std::string("user") : std::string("role");
                if (rng() % 2 != 0) {
                    return leader::command(
                        put_boundary,
                        {{"account", a}, {"kind", k}, {"name", name_of(k)}, {"arn", policy_arn()}},
                        now);
                }
                return leader::command(delete_boundary,
                                       {{"account", a}, {"kind", k}, {"name", name_of(k)}}, now);
            }
            case 16: {
                auto k =
                    leader::make_access_key(kek, account_id, rng() % 4 != 0 ? user() : "", now);
                secrets.push_back(k.secret);
                key_ids.push_back(k.key_id);
                return std::move(k.command);
            }
            case 17:
            case 18: {
                if (key_ids.empty()) {
                    return leader::command(noop, {}, now);
                }
                const auto& kid = key_ids[rng() % key_ids.size()];
                if (rng() % 3 == 0) {
                    return leader::command(delete_access_key, {{"account", a}, {"key_id", kid}},
                                           now);
                }
                if (rng() % 2 != 0) {
                    return leader::command(
                        update_access_key,
                        {{"account", a}, {"key_id", kid}, {"status", pick({"Active", "Inactive"})}},
                        now);
                }
                return leader::command(record_key_last_used,
                                       {{"entries", boost::json::array{{{"key_id", kid},
                                                                        {"time", now},
                                                                        {"service", "s3"},
                                                                        {"region", "us-east-1"}}}}},
                                       now);
            }
            case 19:
                return leader::command(
                    rng() % 2 != 0 ? add_role_to_instance_profile
                                   : remove_role_from_instance_profile,
                    {{"account", a}, {"instance_profile", "ip"}, {"role", role()}}, now);
            case 20: {
                const auto secret = std::to_string(rng()) + "-sts-secret";
                secrets.push_back(secret);
                const auto kid = "kid-" + std::to_string(rng() % 4);
                return leader::command(
                    record_sts_key,
                    {{"kid", kid}, {"sealed_key", iam::seal_secret(kek, secret, kid)}}, now);
            }
            default:
                return leader::command(
                    retire_sts_key,
                    {{"kid", "kid-" + std::to_string(rng() % 4)}, {"retire_after", now + 5}}, now);
        }
    }
};

auto contains(const std::vector<std::byte>& hay, const std::string& needle) -> bool {
    return text(hay).find(needle) != std::string::npos;
}

}  // namespace

BOOST_AUTO_TEST_CASE(property_replicas_and_restores_agree) {
    std::map<std::string, int> outcomes;  // error code ("" for success) → count
    std::size_t keys_left = 0;
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        BOOST_TEST_CONTEXT("seed " << seed) {
            generator gen(seed);
            std::vector<std::vector<std::byte>> log;
            log.push_back(leader::command(
                iam_command::create_account,
                {{"account", account_id}, {"kek_salt", iam::generate_kek_salt()}}, t0));
            log.push_back(
                leader::command(iam_command::create_instance_profile,
                                {{"account", account_id}, {"name", "ip"}, {"id", "AIPA1"}}, t0));
            for (int i = 0; i < 300; ++i) {
                log.push_back(gen.next());
            }

            iam::iam_state_machine a;
            iam::iam_state_machine b;
            iam::iam_state_machine c;
            const std::size_t cut = log.size() / 2;
            std::vector<std::byte> mid;
            for (std::size_t i = 0; i < log.size(); ++i) {
                const auto ra = a.apply(log[i], i + 1);
                const auto rb = b.apply(log[i], i + 1);
                BOOST_TEST_REQUIRE(text(ra) == text(rb));
                ++outcomes[error_of(ra)];
                if (i + 1 == cut) {
                    mid = a.get_state();
                    c.restore_from_snapshot(mid, i + 1);
                    BOOST_TEST_REQUIRE(text(c.get_state()) == text(mid));
                } else if (i + 1 > cut) {
                    BOOST_TEST_REQUIRE(text(c.apply(log[i], i + 1)) == text(ra));
                }
            }
            const auto final_state = a.get_state();
            BOOST_TEST_REQUIRE(text(b.get_state()) == text(final_state));
            BOOST_TEST_REQUIRE(text(c.get_state()) == text(final_state));

            // A snapshot round trip is byte-identical.
            iam::iam_state_machine d;
            d.restore_from_snapshot(final_state, log.size());
            BOOST_TEST_REQUIRE(text(d.get_state()) == text(final_state));

            // No plaintext secret anywhere in the log or a snapshot.
            for (const auto& s : gen.secrets) {
                BOOST_TEST_REQUIRE(!contains(final_state, s));
                BOOST_TEST_REQUIRE(!contains(mid, s));
                for (const auto& cmd : log) {
                    BOOST_TEST_REQUIRE(!contains(cmd, s));
                }
            }
            // Every surviving key still opens to a secret the leader issued.
            for (const auto& [kid, k] : a.account(account_id)->access_keys) {
                const auto opened = leader::open_access_key(gen.kek, k);
                BOOST_TEST_REQUIRE(opened.has_value());
                BOOST_TEST_REQUIRE(std::ranges::count(gen.secrets, *opened) == 1);
                BOOST_TEST_REQUIRE(d.access_key(kid).second != nullptr);
                ++keys_left;
            }
        }
    }
    // The generator reaches every outcome, so the agreement above means
    // something: successes, and each kind of refusal apply() makes.
    for (const char* code : {"", "NoSuchEntity", "EntityAlreadyExists", "DeleteConflict",
                             "LimitExceeded", "InvalidInput"}) {
        BOOST_TEST_CONTEXT("outcome '" << code << "'") {
            BOOST_TEST(outcomes[code] > 20);
        }
    }
    BOOST_TEST(keys_left > 0U);
}
