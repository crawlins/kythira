// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The IAM Query API's user, group, access-key and account-alias actions
// (`.kiro/specs/iam-compatible-auth/` task 14, Requirements 9.1-9.3, 9.8,
// 9.9): parameters validated as AWS does, every action authorized as
// iam:<Action>, AWS's error codes and statuses, and sealed pagination.

#define BOOST_TEST_MODULE iam_query_api_test

#include <boost/test/unit_test.hpp>

#include <kythira/iam/iam_authorizer.hpp>
#include <kythira/iam/iam_query_api.hpp>
#include <kythira/iam/iam_service_registry.hpp>
#include <kythira/iam/iam_state_machine.hpp>
#include <kythira/iam/services/iam_actions.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <optional>
#include <regex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace iam = kythira::iam;
namespace aws_auth = kythira::aws_auth;
namespace q = kythira::aws_query;
namespace leader = kythira::iam::iam_leader;
using iam::iam_command;

namespace {

constexpr std::string_view k_account = "111122223333";

auto test_kek() -> iam::seal_key {
    std::array<unsigned char, iam::seal_key::size> b{};
    for (std::size_t i = 0; i < b.size(); ++i) {
        b[i] = static_cast<unsigned char>(0x40 + i);
    }
    return iam::seal_key(b);
}

/// A state machine behind a lock, standing in for the leader.
struct fake_cluster {
    iam::iam_state_machine sm;
    std::shared_mutex mu;
    std::uint64_t index = 0;
    iam::seal_key k = test_kek();

    void read(const std::function<void(const iam::iam_state_machine&)>& f) {
        const std::shared_lock lock(mu);
        f(sm);
    }
    auto submit(const std::vector<std::byte>& cmd) -> std::vector<std::byte> {
        const std::unique_lock lock(mu);
        return sm.apply(cmd, ++index);
    }
    auto kek(std::string_view) -> const iam::seal_key& { return k; }
};

static_assert(iam::query::query_cluster<fake_cluster>);

struct fixture {
    fake_cluster c;
    iam::iam_service_registry registry;
    iam::iam_authorizer authz{
        [this](const std::function<void(const iam::iam_state_machine&)>& f) { c.read(f); },
        [this](std::string_view) -> const iam::seal_key& { return c.k; }, registry};
    iam::query::iam_query_api<fake_cluster> api{c, authz};
    aws_auth::principal_context root;

    fixture() {
        registry.add(iam::services::iam_actions());
        (void)c.submit(
            leader::command(iam_command::create_account,
                            {{"account", k_account}, {"kek_salt", "c2FsdHNhbHRzYWx0c2FsdA"}}));
        root.type = aws_auth::principal_type::root;
        root.account = std::string(k_account);
        root.arn = "arn:aws:iam::111122223333:root";
        root.userid = std::string(k_account);
    }

    auto call(const aws_auth::principal_context& who, const std::string& body)
        -> iam::query::reply {
        const auto f = q::form::parse(body);
        BOOST_TEST_REQUIRE(f.has_value(), body);
        return api.handle(who, *f, {.request_id = "req-1"});
    }
    auto as_root(const std::string& body) -> iam::query::reply { return call(root, body); }

    /// A principal for IAM user `name`, which must exist.
    auto user(const std::string& name) -> aws_auth::principal_context {
        aws_auth::principal_context p;
        p.type = aws_auth::principal_type::user;
        p.account = std::string(k_account);
        p.arn = "arn:aws:iam::111122223333:user/" + name;
        p.username = name;
        c.read([&](const iam::iam_state_machine& sm) { p.userid = sm.user(k_account, name)->id; });
        return p;
    }

    void allow(const std::string& name, const std::string& statement) {
        const auto r = as_root("Action=CreateUser&UserName=" + name);
        (void)r;
        (void)c.submit(leader::command(
            iam_command::put_inline_policy,
            {{"account", k_account},
             {"kind", "user"},
             {"name", name},
             {"policy_name", "p"},
             {"document", R"({"Version":"2012-10-17","Statement":[)" + statement + "]}"}}));
    }
};

auto code_of(const iam::query::reply& r) -> std::string {
    std::smatch m;
    static const std::regex re("<Code>([^<]*)</Code>");
    return std::regex_search(r.body, m, re) ? m[1].str() : std::string{};
}

auto element(const std::string& xml, const std::string& tag) -> std::string {
    std::smatch m;
    const std::regex re("<" + tag + ">([^<]*)</" + tag + ">");
    return std::regex_search(xml, m, re) ? m[1].str() : std::string{};
}

auto count(const std::string& xml, const std::string& needle) -> std::size_t {
    std::size_t n = 0;
    for (auto p = xml.find(needle); p != std::string::npos; p = xml.find(needle, p + 1)) {
        ++n;
    }
    return n;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(protocol)

BOOST_AUTO_TEST_CASE(responses_and_errors_have_aws_shapes) {
    fixture f;
    const auto ok = f.as_root("Action=CreateUser&UserName=alice&Version=2010-05-08");
    BOOST_TEST(ok.status == 200U);
    BOOST_TEST(ok.body.starts_with(
        R"(<CreateUserResponse xmlns="https://iam.amazonaws.com/doc/2010-05-08/"><CreateUserResult>)"));
    BOOST_TEST(ok.body.find("<RequestId>req-1</RequestId>") != std::string::npos);

    const auto missing = f.as_root("Version=2010-05-08");
    BOOST_TEST(missing.status == 400U);
    BOOST_TEST(code_of(missing) == "MissingAction");
    const auto unknown = f.as_root("Action=CreateServiceLinkedRole");
    BOOST_TEST(unknown.status == 501U);
    BOOST_TEST(code_of(unknown) == "NotImplemented");
    const auto version = f.as_root("Action=ListUsers&Version=2006-03-01");
    BOOST_TEST(code_of(version) == "InvalidParameterValue");
    BOOST_TEST(version.body.find("<Type>Sender</Type>") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(users)

BOOST_AUTO_TEST_CASE(a_user_is_created_read_listed_renamed_and_deleted) {
    fixture f;
    const auto created = f.as_root(
        "Action=CreateUser&UserName=alice&Path=%2Fdev%2F&Tags.member.1.Key=team&Tags.member.1."
        "Value=iam");
    BOOST_TEST_REQUIRE(created.status == 200U, created.body);
    BOOST_TEST(element(created.body, "Arn") == "arn:aws:iam::111122223333:user/dev/alice");
    BOOST_TEST(element(created.body, "UserId").starts_with("AIDA"));
    BOOST_TEST(element(created.body, "Value") == "iam");
    BOOST_TEST(std::regex_match(element(created.body, "CreateDate"),
                                std::regex(R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ)")));

    const auto dup = f.as_root("Action=CreateUser&UserName=ALICE");
    BOOST_TEST(dup.status == 409U);
    BOOST_TEST(code_of(dup) == "EntityAlreadyExists");

    BOOST_TEST(element(f.as_root("Action=GetUser&UserName=alice").body, "Path") == "/dev/");
    BOOST_TEST(count(f.as_root("Action=ListUsers").body, "<member>") == 1U);
    BOOST_TEST(count(f.as_root("Action=ListUsers&PathPrefix=%2Fops%2F").body, "<member>") == 0U);

    BOOST_TEST(f.as_root("Action=UpdateUser&UserName=alice&NewUserName=alicia").status == 200U);
    BOOST_TEST(code_of(f.as_root("Action=GetUser&UserName=alice")) == "NoSuchEntity");
    BOOST_TEST(f.as_root("Action=DeleteUser&UserName=alicia").status == 200U);
    const auto gone = f.as_root("Action=GetUser&UserName=alicia");
    BOOST_TEST(gone.status == 404U);
    BOOST_TEST(code_of(gone) == "NoSuchEntity");
}

BOOST_AUTO_TEST_CASE(get_user_without_a_name_is_the_signer) {
    fixture f;
    const auto r = f.as_root("Action=GetUser");
    BOOST_TEST(element(r.body, "Arn") == "arn:aws:iam::111122223333:root");
    BOOST_TEST(element(r.body, "UserId") == "111122223333");
    f.allow("bob", R"({"Effect":"Allow","Action":"iam:GetUser","Resource":"*"})");
    const auto b = f.call(f.user("bob"), "Action=GetUser");
    BOOST_TEST(element(b.body, "UserName") == "bob");
}

BOOST_AUTO_TEST_CASE(names_and_paths_are_validated) {
    fixture f;
    BOOST_TEST(code_of(f.as_root("Action=CreateUser&UserName=a%2Fb")) == "ValidationError");
    BOOST_TEST(code_of(f.as_root("Action=CreateUser&UserName=" + std::string(65, 'a'))) ==
               "ValidationError");
    BOOST_TEST(code_of(f.as_root("Action=CreateUser&UserName=ok&Path=dev")) == "ValidationError");
    BOOST_TEST(code_of(f.as_root("Action=CreateUser")) == "ValidationError");
    BOOST_TEST(f.as_root("Action=CreateGroup&GroupName=" + std::string(128, 'g')).status == 200U);
}

// INV-TAG-1..6 through the API: the iam_validation checks run before any
// write, so one bad tag creates nothing.
BOOST_AUTO_TEST_CASE(inv_tag_bad_tags_fail_the_whole_request) {
    fixture f;
    std::string many = "Action=CreateUser&UserName=t";
    for (int i = 1; i <= 51; ++i) {
        many += "&Tags.member." + std::to_string(i) + ".Key=k" + std::to_string(i) +
                "&Tags.member." + std::to_string(i) + ".Value=v";
    }
    BOOST_TEST(code_of(f.as_root(many)) == "LimitExceeded");  // INV-TAG-1
    BOOST_TEST(code_of(f.as_root("Action=CreateUser&UserName=t&Tags.member.1.Key=a%23b")) ==
               "ValidationError");  // INV-TAG-2
    BOOST_TEST(code_of(f.as_root("Action=CreateUser&UserName=t&Tags.member.1.Key=k&Tags.member.1."
                                 "Value=" +
                                 std::string(257, 'v'))) == "ValidationError");  // INV-TAG-3
    BOOST_TEST(code_of(f.as_root("Action=CreateUser&UserName=t&Tags.member.1.Key=aws%3Ax")) ==
               "ValidationError");  // INV-TAG-4
    // INV-TAG-5: keys are case-insensitive on users.
    BOOST_TEST(code_of(f.as_root("Action=CreateUser&UserName=t&Tags.member.1.Key=Team&Tags.member."
                                 "2.Key=team")) == "InvalidInput");
    // INV-TAG-6: none of the refused requests created the user.
    BOOST_TEST(code_of(f.as_root("Action=GetUser&UserName=t")) == "NoSuchEntity");

    BOOST_TEST(f.as_root("Action=CreateUser&UserName=t").status == 200U);
    BOOST_TEST(
        f.as_root("Action=TagUser&UserName=t&Tags.member.1.Key=Env&Tags.member.1.Value=a").status ==
        200U);
    // The latest spelling of a case-insensitive key wins.
    BOOST_TEST(
        f.as_root("Action=TagUser&UserName=t&Tags.member.1.Key=ENV&Tags.member.1.Value=b").status ==
        200U);
    const auto tags = f.as_root("Action=ListUserTags&UserName=t");
    BOOST_TEST(count(tags.body, "<member>") == 1U);
    BOOST_TEST(element(tags.body, "Key") == "ENV");
    BOOST_TEST(f.as_root("Action=UntagUser&UserName=t&TagKeys.member.1=env").status == 200U);
    BOOST_TEST(count(f.as_root("Action=ListUserTags&UserName=t").body, "<member>") == 0U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(authorization)

BOOST_AUTO_TEST_CASE(every_action_is_authorized_as_iam_action_on_its_resource) {
    fixture f;
    f.allow(
        "carol",
        R"({"Effect":"Allow","Action":"iam:CreateUser","Resource":"arn:aws:iam::111122223333:user/team/*"})");
    const auto carol = f.user("carol");
    BOOST_TEST(f.call(carol, "Action=CreateUser&UserName=x&Path=%2Fteam%2F").status == 200U);
    const auto denied = f.call(carol, "Action=CreateUser&UserName=y");
    BOOST_TEST(denied.status == 403U);
    BOOST_TEST(code_of(denied) == "AccessDenied");
    BOOST_TEST(element(denied.body, "Message") ==
               "User: arn:aws:iam::111122223333:user/carol is not authorized to perform: "
               "iam:CreateUser on resource: arn:aws:iam::111122223333:user/y because no "
               "identity-based policy allows the iam:CreateUser action");
    BOOST_TEST(code_of(f.as_root("Action=GetUser&UserName=y")) == "NoSuchEntity");
    BOOST_TEST(code_of(f.call(carol, "Action=ListUsers")) == "AccessDenied");
    BOOST_TEST(code_of(f.call(carol, "Action=DeleteUser&UserName=x")) == "AccessDenied");
}

BOOST_AUTO_TEST_CASE(resource_tags_and_boundaries_reach_the_condition_keys) {
    fixture f;
    f.allow(
        "dave",
        R"({"Effect":"Allow","Action":"iam:TagUser","Resource":"*","Condition":{"StringEquals":{"iam:ResourceTag/team":"a"}}},)"
        R"({"Effect":"Allow","Action":"iam:CreateUser","Resource":"*","Condition":{"StringEquals":{"iam:PermissionsBoundary":"arn:aws:iam::aws:policy/ReadOnlyAccess"}}})");
    const auto dave = f.user("dave");
    BOOST_TEST(
        f.as_root("Action=CreateUser&UserName=ta&Tags.member.1.Key=team&Tags.member.1.Value=a")
            .status == 200U);
    BOOST_TEST(
        f.as_root("Action=CreateUser&UserName=tb&Tags.member.1.Key=team&Tags.member.1.Value=b")
            .status == 200U);
    const std::string tag = "&Tags.member.1.Key=x&Tags.member.1.Value=1";
    BOOST_TEST(f.call(dave, "Action=TagUser&UserName=ta" + tag).status == 200U);
    BOOST_TEST(code_of(f.call(dave, "Action=TagUser&UserName=tb" + tag)) == "AccessDenied");
    BOOST_TEST(code_of(f.call(dave, "Action=CreateUser&UserName=nb")) == "AccessDenied");
    BOOST_TEST(f.call(dave,
                      "Action=CreateUser&UserName=wb&PermissionsBoundary=arn%3Aaws%3Aiam%3A%3Aaws%"
                      "3Apolicy%2FReadOnlyAccess")
                   .status == 200U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(access_keys)

// INV-KEY-3: without UserName a key action acts on the signer's own keys,
// and the signer can be the account root.
BOOST_AUTO_TEST_CASE(inv_key_3_without_a_user_name_the_key_is_the_signers) {
    fixture f;
    const auto root_key = f.as_root("Action=CreateAccessKey");
    BOOST_TEST_REQUIRE(root_key.status == 200U, root_key.body);
    const auto rid = element(root_key.body, "AccessKeyId");
    BOOST_TEST(rid.starts_with("AKIA"));
    BOOST_TEST(element(root_key.body, "SecretAccessKey").size() == 40U);
    BOOST_TEST(element(root_key.body, "UserName").empty());
    f.c.read([&](const iam::iam_state_machine& sm) {
        BOOST_TEST(sm.access_key(rid).second->owner.empty());
    });

    f.allow(
        "erin",
        R"({"Effect":"Allow","Action":["iam:*AccessKey*"],"Resource":"arn:aws:iam::111122223333:user/${aws:username}"})");
    const auto erin = f.user("erin");
    const auto own = f.call(erin, "Action=CreateAccessKey");
    BOOST_TEST_REQUIRE(own.status == 200U, own.body);
    const auto eid = element(own.body, "AccessKeyId");
    BOOST_TEST(element(own.body, "UserName") == "erin");
    f.c.read([&](const iam::iam_state_machine& sm) {
        BOOST_TEST(sm.access_key(eid).second->owner == "erin");
    });
    // The list without UserName is the signer's, and holds no secret.
    const auto list = f.call(erin, "Action=ListAccessKeys");
    BOOST_TEST(list.body.find(eid) != std::string::npos);
    BOOST_TEST(list.body.find(rid) == std::string::npos);
    BOOST_TEST(list.body.find("SecretAccessKey") == std::string::npos);
    // Someone else's keys are not the signer's to touch.
    BOOST_TEST(code_of(f.call(erin, "Action=CreateAccessKey&UserName=frank")) == "AccessDenied");

    aws_auth::principal_context role = erin;
    role.type = aws_auth::principal_type::assumed_role;
    BOOST_TEST(code_of(f.call(role, "Action=CreateAccessKey")) == "ValidationError");
}

BOOST_AUTO_TEST_CASE(a_key_is_deactivated_and_deleted_only_by_its_owners_name) {
    fixture f;
    BOOST_TEST(f.as_root("Action=CreateUser&UserName=gail").status == 200U);
    BOOST_TEST(f.as_root("Action=CreateUser&UserName=hank").status == 200U);
    const auto id = element(f.as_root("Action=CreateAccessKey&UserName=gail").body, "AccessKeyId");
    // Named under another user, the key does not exist.
    BOOST_TEST(code_of(f.as_root("Action=UpdateAccessKey&UserName=hank&AccessKeyId=" + id +
                                 "&Status=Inactive")) == "NoSuchEntity");
    BOOST_TEST(code_of(f.as_root("Action=UpdateAccessKey&UserName=gail&AccessKeyId=" + id +
                                 "&Status=Disabled")) == "ValidationError");
    BOOST_TEST(
        f.as_root("Action=UpdateAccessKey&UserName=gail&AccessKeyId=" + id + "&Status=Inactive")
            .status == 200U);
    f.c.read(
        [&](const iam::iam_state_machine& sm) { BOOST_TEST(!sm.access_key(id).second->active); });
    const auto list = f.as_root("Action=ListAccessKeys&UserName=gail");
    BOOST_TEST(element(list.body, "Status") == "Inactive");
    const auto last = f.as_root("Action=GetAccessKeyLastUsed&AccessKeyId=" + id);
    BOOST_TEST(element(last.body, "UserName") == "gail");
    BOOST_TEST(element(last.body, "ServiceName") == "N/A");
    BOOST_TEST(code_of(f.as_root("Action=GetAccessKeyLastUsed&AccessKeyId=AKIANOSUCHKEY0000000")) ==
               "AccessDenied");
    // A user with a key cannot be deleted.
    BOOST_TEST(code_of(f.as_root("Action=DeleteUser&UserName=gail")) == "DeleteConflict");
    BOOST_TEST(f.as_root("Action=DeleteAccessKey&UserName=gail&AccessKeyId=" + id).status == 200U);
    BOOST_TEST(f.as_root("Action=DeleteUser&UserName=gail").status == 200U);
    // The quota is AWS's: two keys per user.
    BOOST_TEST(f.as_root("Action=CreateAccessKey&UserName=hank").status == 200U);
    BOOST_TEST(f.as_root("Action=CreateAccessKey&UserName=hank").status == 200U);
    BOOST_TEST(code_of(f.as_root("Action=CreateAccessKey&UserName=hank")) == "LimitExceeded");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(groups)

BOOST_AUTO_TEST_CASE(groups_hold_members_and_refuse_deletion_while_used) {
    fixture f;
    BOOST_TEST(f.as_root("Action=CreateUser&UserName=ivy").status == 200U);
    const auto g = f.as_root("Action=CreateGroup&GroupName=devs&Path=%2Feng%2F");
    BOOST_TEST(element(g.body, "Arn") == "arn:aws:iam::111122223333:group/eng/devs");
    BOOST_TEST(element(g.body, "GroupId").starts_with("AGPA"));
    BOOST_TEST(f.as_root("Action=AddUserToGroup&GroupName=devs&UserName=ivy").status == 200U);
    BOOST_TEST(code_of(f.as_root("Action=AddUserToGroup&GroupName=devs&UserName=nobody")) ==
               "NoSuchEntity");
    const auto get = f.as_root("Action=GetGroup&GroupName=devs");
    BOOST_TEST(element(get.body, "UserName") == "ivy");
    BOOST_TEST(element(get.body, "IsTruncated") == "false");
    BOOST_TEST(element(f.as_root("Action=ListGroupsForUser&UserName=ivy").body, "GroupName") ==
               "devs");
    const auto conflict = f.as_root("Action=DeleteGroup&GroupName=devs");
    BOOST_TEST(conflict.status == 409U);
    BOOST_TEST(code_of(conflict) == "DeleteConflict");
    BOOST_TEST(f.as_root("Action=UpdateGroup&GroupName=devs&NewGroupName=builders").status == 200U);
    BOOST_TEST(element(f.as_root("Action=ListGroupsForUser&UserName=ivy").body, "GroupName") ==
               "builders");
    BOOST_TEST(f.as_root("Action=RemoveUserFromGroup&GroupName=builders&UserName=ivy").status ==
               200U);
    BOOST_TEST(f.as_root("Action=DeleteGroup&GroupName=builders").status == 200U);
    BOOST_TEST(count(f.as_root("Action=ListGroups").body, "<member>") == 0U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(pagination)

BOOST_AUTO_TEST_CASE(markers_page_through_a_list_and_cannot_be_forged) {
    fixture f;
    for (const auto* n : {"u1", "u2", "u3", "u4", "u5"}) {
        BOOST_TEST(f.as_root(std::string("Action=CreateUser&UserName=") + n).status == 200U);
    }
    BOOST_TEST(f.as_root("Action=CreateGroup&GroupName=g1").status == 200U);
    BOOST_TEST(f.as_root("Action=CreateGroup&GroupName=g2").status == 200U);
    std::vector<std::string> seen;
    std::string marker;
    for (int pages = 0; pages < 5; ++pages) {
        const auto r = f.as_root("Action=ListUsers&MaxItems=2" +
                                 (marker.empty() ? std::string() : "&Marker=" + marker));
        BOOST_TEST_REQUIRE(r.status == 200U, r.body);
        for (auto p = r.body.find("<UserName>"); p != std::string::npos;
             p = r.body.find("<UserName>", p + 1)) {
            seen.push_back(r.body.substr(p + 10, 2));
        }
        if (element(r.body, "IsTruncated") == "false") {
            break;
        }
        marker = element(r.body, "Marker");
    }
    BOOST_TEST((seen == std::vector<std::string>{"u1", "u2", "u3", "u4", "u5"}));

    const auto first = f.as_root("Action=ListUsers&MaxItems=1");
    const auto m = element(first.body, "Marker");
    BOOST_TEST(code_of(f.as_root("Action=ListUsers&Marker=" + m + "x")) == "InvalidInput");
    // A ListUsers marker means nothing to ListGroups.
    BOOST_TEST(code_of(f.as_root("Action=ListGroups&Marker=" + m)) == "InvalidInput");
    BOOST_TEST(code_of(f.as_root("Action=ListUsers&MaxItems=0")) == "ValidationError");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(account_alias)

// INV-ACCT-2 through the API.
BOOST_AUTO_TEST_CASE(inv_acct_2_aliases_are_validated_set_listed_and_deleted) {
    fixture f;
    for (const auto* bad : {"ab", "-lead", "trail-", "dou--ble", "Upper", "123456789012"}) {
        BOOST_TEST(code_of(f.as_root(std::string("Action=CreateAccountAlias&AccountAlias=") +
                                     bad)) == "ValidationError");
    }
    BOOST_TEST(f.as_root("Action=CreateAccountAlias&AccountAlias=my-team").status == 200U);
    BOOST_TEST(element(f.as_root("Action=ListAccountAliases").body, "member") == "my-team");
    BOOST_TEST(code_of(f.as_root("Action=DeleteAccountAlias&AccountAlias=other")) ==
               "NoSuchEntity");
    BOOST_TEST(f.as_root("Action=DeleteAccountAlias&AccountAlias=my-team").status == 200U);
    BOOST_TEST(count(f.as_root("Action=ListAccountAliases").body, "<member>") == 0U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Task 15: roles, managed policies, attachments, boundaries and instance
// profiles (Requirements 9.4, 9.5, 9.7, 9.8) ──

namespace {

constexpr std::string_view k_trust =
    R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"ec2.amazonaws.com"},"Action":"sts:AssumeRole"}]})";
constexpr std::string_view k_doc =
    R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"s3:GetObject","Resource":"*"}]})";
constexpr std::string_view k_doc2 =
    R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"s3:PutObject","Resource":"*"}]})";
constexpr std::string_view k_policy_arn = "arn:aws:iam::111122223333:policy/p1";
constexpr std::string_view k_readonly = "arn:aws:iam::aws:policy/ReadOnlyAccess";

auto enc(std::string_view s) -> std::string {
    return q::url_encode(s);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(roles_and_policies)

BOOST_FIXTURE_TEST_CASE(a_role_is_created_read_listed_updated_and_deleted, fixture) {
    auto r = as_root(
        "Action=CreateRole&RoleName=app&Path=%2Fsvc%2F&Description=App+role&"
        "Tags.member.1.Key=team&Tags.member.1.Value=iam&AssumeRolePolicyDocument=" +
        enc(k_trust));
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(element(r.body, "Arn") == "arn:aws:iam::111122223333:role/svc/app");
    BOOST_TEST(element(r.body, "RoleId").starts_with("AROA"));
    BOOST_TEST(element(r.body, "MaxSessionDuration") == "3600");
    // The trust policy comes back URL-encoded, as AWS returns it.
    BOOST_TEST(element(r.body, "AssumeRolePolicyDocument") == enc(k_trust));
    BOOST_TEST(code_of(as_root("Action=CreateRole&RoleName=APP&AssumeRolePolicyDocument=" +
                               enc(k_trust))) == "EntityAlreadyExists");

    r = as_root("Action=GetRole&RoleName=app");
    BOOST_TEST(element(r.body, "Description") == "App role");
    BOOST_TEST(count(r.body, "<RoleLastUsed>") == 1U);
    BOOST_TEST(element(r.body, "Value") == "iam");

    BOOST_TEST(as_root("Action=UpdateRole&RoleName=app&MaxSessionDuration=7200").status == 200U);
    r = as_root("Action=UpdateRoleDescription&RoleName=app&Description=Changed");
    BOOST_TEST(element(r.body, "Description") == "Changed");
    BOOST_TEST(element(r.body, "MaxSessionDuration") == "7200");

    constexpr std::string_view other =
        R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"AWS":"arn:aws:iam::111122223333:root"},"Action":"sts:AssumeRole"}]})";
    BOOST_TEST(
        as_root("Action=UpdateAssumeRolePolicy&RoleName=app&PolicyDocument=" + enc(other)).status ==
        200U);
    BOOST_TEST(element(as_root("Action=GetRole&RoleName=app").body, "AssumeRolePolicyDocument") ==
               enc(other));

    r = as_root("Action=ListRoles&PathPrefix=%2Fsvc%2F");
    BOOST_TEST(count(r.body, "<RoleName>app</RoleName>") == 1U);
    BOOST_TEST(count(r.body, "<RoleLastUsed>") == 0U);  // only GetRole has it
    BOOST_TEST(count(as_root("Action=ListRoles&PathPrefix=%2Fother%2F").body, "<member>") == 0U);

    BOOST_TEST(as_root("Action=TagRole&RoleName=app&Tags.member.1.Key=env&Tags.member.1.Value=dev")
                   .status == 200U);
    BOOST_TEST(count(as_root("Action=ListRoleTags&RoleName=app").body, "<Key>") == 2U);
    BOOST_TEST(as_root("Action=UntagRole&RoleName=app&TagKeys.member.1=team").status == 200U);
    BOOST_TEST(count(as_root("Action=ListRoleTags&RoleName=app").body, "<Key>") == 1U);

    BOOST_TEST(as_root("Action=DeleteRole&RoleName=app").status == 200U);
    r = as_root("Action=GetRole&RoleName=app");
    BOOST_TEST(r.status == 404U);
    BOOST_TEST(code_of(r) == "NoSuchEntity");
}

BOOST_FIXTURE_TEST_CASE(trust_policies_are_parsed_as_trust_policies, fixture) {
    // INV-ROLE-3 through the API: a trust policy names a Principal and no
    // Resource; an identity document is not a trust policy.
    const auto r = as_root("Action=CreateRole&RoleName=app&AssumeRolePolicyDocument=" + enc(k_doc));
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(code_of(r) == "MalformedPolicyDocument");
    BOOST_TEST(code_of(as_root("Action=CreateRole&RoleName=app&AssumeRolePolicyDocument=%7Bnot")) ==
               "MalformedPolicyDocument");
    BOOST_TEST(code_of(as_root("Action=CreateRole&RoleName=app")) == "ValidationError");
}

BOOST_FIXTURE_TEST_CASE(inv_role_1_create_and_update_role_check_max_session_duration, fixture) {
    const auto create = [&](const std::string& extra) {
        return as_root("Action=CreateRole&RoleName=r&AssumeRolePolicyDocument=" + enc(k_trust) +
                       extra);
    };
    BOOST_TEST(code_of(create("&MaxSessionDuration=3599")) == "ValidationError");
    BOOST_TEST(code_of(create("&MaxSessionDuration=43201")) == "ValidationError");
    BOOST_TEST(code_of(create("&MaxSessionDuration=1h")) == "ValidationError");
    BOOST_TEST(create("&MaxSessionDuration=43200").status == 200U);
    BOOST_TEST(element(as_root("Action=GetRole&RoleName=r").body, "MaxSessionDuration") == "43200");
    // UpdateRole checks it too, and a refused update changes nothing.
    BOOST_TEST(code_of(as_root("Action=UpdateRole&RoleName=r&MaxSessionDuration=3000")) ==
               "ValidationError");
    BOOST_TEST(as_root("Action=UpdateRole&RoleName=r&MaxSessionDuration=3600").status == 200U);
    BOOST_TEST(element(as_root("Action=GetRole&RoleName=r").body, "MaxSessionDuration") == "3600");
}

BOOST_FIXTURE_TEST_CASE(inv_role_2_role_descriptions_are_checked, fixture) {
    const std::string long_text(1001, 'a');
    const auto base = "Action=CreateRole&RoleName=r&AssumeRolePolicyDocument=" + enc(k_trust);
    BOOST_TEST(code_of(as_root(base + "&Description=" + long_text)) == "ValidationError");
    BOOST_TEST(code_of(as_root(base + "&Description=a%01b")) == "ValidationError");
    BOOST_TEST(as_root(base + "&Description=" + std::string(1000, 'a')).status == 200U);
    BOOST_TEST(code_of(as_root("Action=UpdateRole&RoleName=r&Description=" + long_text)) ==
               "ValidationError");
    BOOST_TEST(code_of(as_root("Action=UpdateRoleDescription&RoleName=r&Description=" +
                               long_text)) == "ValidationError");
    BOOST_TEST(code_of(as_root("Action=UpdateRoleDescription&RoleName=r")) == "ValidationError");
}

BOOST_FIXTURE_TEST_CASE(inv_pol_1_policy_documents_are_checked_before_parsing, fixture) {
    // Characters outside tab, LF, CR and U+0020-U+00FF, and an empty
    // document, are ValidationError: the parser never sees them.
    BOOST_TEST(code_of(as_root("Action=CreatePolicy&PolicyName=p&PolicyDocument=")) ==
               "ValidationError");
    BOOST_TEST(code_of(as_root("Action=CreatePolicy&PolicyName=p&PolicyDocument=" +
                               enc(std::string(k_doc).insert(1, "\x01")))) == "ValidationError");
    // U+0100 (two UTF-8 bytes) is outside the set; U+00E9 is inside it.
    BOOST_TEST(code_of(as_root("Action=CreatePolicy&PolicyName=p&PolicyDocument=" +
                               enc(std::string(k_doc).insert(1, "\xC4\x80")))) ==
               "ValidationError");
    BOOST_TEST(code_of(as_root("Action=CreatePolicy&PolicyName=p&PolicyDocument=" +
                               enc(std::string(k_doc).insert(1, "\xC3\xA9")))) ==
               "MalformedPolicyDocument");
    // Longer than the managed quota but inside the outer bound: LimitExceeded.
    const auto big = R"({"Version":"2012-10-17","Statement":[{"Sid":")" + std::string(7000, 'x') +
                     R"(","Effect":"Allow","Action":"s3:GetObject","Resource":"*"}]})";
    BOOST_TEST(code_of(as_root("Action=CreatePolicy&PolicyName=p&PolicyDocument=" + enc(big))) ==
               "LimitExceeded");
    // The same rule guards inline policies and policy versions.
    BOOST_TEST(as_root("Action=CreateUser&UserName=u").status == 200U);
    BOOST_TEST(code_of(as_root("Action=PutUserPolicy&UserName=u&PolicyName=i&PolicyDocument=")) ==
               "ValidationError");
    BOOST_TEST(as_root("Action=CreatePolicy&PolicyName=p1&PolicyDocument=" + enc(k_doc)).status ==
               200U);
    BOOST_TEST(code_of(as_root("Action=CreatePolicyVersion&PolicyArn=" + std::string(k_policy_arn) +
                               "&PolicyDocument=%01")) == "ValidationError");
}

BOOST_FIXTURE_TEST_CASE(inv_pol_6_version_ids_are_checked, fixture) {
    BOOST_TEST(as_root("Action=CreatePolicy&PolicyName=p1&PolicyDocument=" + enc(k_doc)).status ==
               200U);
    const auto get = [&](const std::string& vid) {
        return as_root("Action=GetPolicyVersion&PolicyArn=" + std::string(k_policy_arn) +
                       "&VersionId=" + vid);
    };
    for (const auto* bad : {"v0", "1", "v", "V1", "v1.%21", "v01"}) {
        BOOST_TEST(code_of(get(bad)) == "ValidationError", bad);
    }
    // Well formed but absent is the state's answer.
    BOOST_TEST(code_of(get("v9")) == "NoSuchEntity");
    BOOST_TEST(code_of(get("v1.beta-2")) == "NoSuchEntity");
    BOOST_TEST(get("v1").status == 200U);
    BOOST_TEST(code_of(as_root("Action=SetDefaultPolicyVersion&PolicyArn=" +
                               std::string(k_policy_arn) + "&VersionId=v0")) == "ValidationError");
    BOOST_TEST(code_of(as_root("Action=DeletePolicyVersion&PolicyArn=" + std::string(k_policy_arn) +
                               "&VersionId=x")) == "ValidationError");
}

BOOST_FIXTURE_TEST_CASE(a_policy_has_versions_and_is_deleted_only_when_unused, fixture) {
    auto r =
        as_root("Action=CreatePolicy&PolicyName=p1&Description=Reads&PolicyDocument=" + enc(k_doc) +
                "&Tags.member.1.Key=Team&Tags.member.1.Value=iam");
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(element(r.body, "Arn") == k_policy_arn);
    BOOST_TEST(element(r.body, "PolicyId").starts_with("ANPA"));
    BOOST_TEST(element(r.body, "DefaultVersionId") == "v1");
    BOOST_TEST(element(r.body, "Description") == "Reads");

    const auto parn = "&PolicyArn=" + std::string(k_policy_arn);
    r = as_root("Action=CreatePolicyVersion" + parn +
                "&SetAsDefault=true&PolicyDocument=" + enc(k_doc2));
    BOOST_TEST(element(r.body, "VersionId") == "v2");
    BOOST_TEST(element(r.body, "IsDefaultVersion") == "true");
    r = as_root("Action=GetPolicyVersion" + parn + "&VersionId=v2");
    BOOST_TEST(element(r.body, "Document") == enc(k_doc2));
    r = as_root("Action=ListPolicyVersions" + parn);
    BOOST_TEST(count(r.body, "<member>") == 2U);
    BOOST_TEST(element(as_root("Action=GetPolicy" + parn).body, "DefaultVersionId") == "v2");

    BOOST_TEST(code_of(as_root("Action=DeletePolicyVersion" + parn + "&VersionId=v2")) ==
               "DeleteConflict");  // INV-POL-3
    BOOST_TEST(as_root("Action=SetDefaultPolicyVersion" + parn + "&VersionId=v1").status == 200U);
    BOOST_TEST(as_root("Action=DeletePolicyVersion" + parn + "&VersionId=v2").status == 200U);

    BOOST_TEST(as_root("Action=CreateUser&UserName=u").status == 200U);
    BOOST_TEST(as_root("Action=AttachUserPolicy&UserName=u" + parn).status == 200U);
    BOOST_TEST(element(as_root("Action=GetPolicy" + parn).body, "AttachmentCount") == "1");
    r = as_root("Action=ListEntitiesForPolicy" + parn);
    BOOST_TEST(element(r.body, "UserName") == "u");
    BOOST_TEST(count(r.body, "<PolicyGroups></PolicyGroups>") == 1U);
    BOOST_TEST(code_of(as_root("Action=DeletePolicy" + parn)) == "DeleteConflict");
    BOOST_TEST(as_root("Action=DetachUserPolicy&UserName=u" + parn).status == 200U);
    BOOST_TEST(code_of(as_root("Action=DetachUserPolicy&UserName=u" + parn)) == "NoSuchEntity");

    BOOST_TEST(as_root("Action=TagPolicy" + parn + "&Tags.member.1.Key=team&Tags.member.1.Value=x")
                   .status == 200U);
    // Policy tag keys are case-sensitive, unlike a user's or a role's.
    BOOST_TEST(count(as_root("Action=ListPolicyTags" + parn).body, "<Key>") == 2U);
    BOOST_TEST(as_root("Action=UntagPolicy" + parn + "&TagKeys.member.1=Team").status == 200U);

    BOOST_TEST(as_root("Action=DeletePolicy" + parn).status == 200U);
    BOOST_TEST(code_of(as_root("Action=GetPolicy" + parn)) == "NoSuchEntity");
}

BOOST_FIXTURE_TEST_CASE(aws_managed_policies_are_listed_and_read_but_not_changed, fixture) {
    const auto parn = "&PolicyArn=" + std::string(k_readonly);
    auto r = as_root("Action=GetPolicy" + parn);
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(element(r.body, "PolicyName") == "ReadOnlyAccess");
    // Its id is derived, so every member and every call agree on it.
    BOOST_TEST(element(r.body, "PolicyId") ==
               element(as_root("Action=GetPolicy" + parn).body, "PolicyId"));
    const auto v = element(r.body, "DefaultVersionId");
    BOOST_TEST(as_root("Action=GetPolicyVersion" + parn + "&VersionId=" + v).status == 200U);
    BOOST_TEST(code_of(as_root("Action=CreatePolicyVersion" + parn +
                               "&PolicyDocument=" + enc(k_doc))) == "InvalidInput");
    BOOST_TEST(code_of(as_root("Action=TagPolicy" + parn +
                               "&Tags.member.1.Key=a&Tags.member.1.Value=b")) == "InvalidInput");
    BOOST_TEST(code_of(as_root("Action=DeletePolicy" + parn)) == "InvalidInput");

    BOOST_TEST(as_root("Action=CreatePolicy&PolicyName=p1&PolicyDocument=" + enc(k_doc)).status ==
               200U);
    r = as_root("Action=ListPolicies");
    BOOST_TEST(count(r.body, "<PolicyName>ReadOnlyAccess</PolicyName>") == 1U);
    BOOST_TEST(count(r.body, "<PolicyName>p1</PolicyName>") == 1U);
    r = as_root("Action=ListPolicies&Scope=Local");
    BOOST_TEST(count(r.body, "<PolicyName>ReadOnlyAccess</PolicyName>") == 0U);
    BOOST_TEST(count(r.body, "<member>") == 1U);
    BOOST_TEST(
        count(as_root("Action=ListPolicies&Scope=AWS").body, "<PolicyName>p1</PolicyName>") == 0U);
    BOOST_TEST(code_of(as_root("Action=ListPolicies&Scope=Mine")) == "ValidationError");

    // OnlyAttached counts this account's uses of an AWS-managed policy.
    BOOST_TEST(as_root("Action=CreateGroup&GroupName=g").status == 200U);
    BOOST_TEST(as_root("Action=AttachGroupPolicy&GroupName=g" + parn).status == 200U);
    r = as_root("Action=ListPolicies&OnlyAttached=true");
    BOOST_TEST(count(r.body, "<member>") == 1U);
    BOOST_TEST(element(r.body, "AttachmentCount") == "1");
    BOOST_TEST(element(as_root("Action=ListAttachedGroupPolicies&GroupName=g").body, "PolicyArn") ==
               k_readonly);
    BOOST_TEST(count(as_root("Action=ListPolicies&OnlyAttached=true&PolicyUsageFilter="
                             "PermissionsBoundary")
                         .body,
                     "<member>") == 0U);
}

BOOST_FIXTURE_TEST_CASE(inline_policies_round_trip_for_users_groups_and_roles, fixture) {
    BOOST_TEST(as_root("Action=CreateUser&UserName=u").status == 200U);
    BOOST_TEST(as_root("Action=CreateGroup&GroupName=g").status == 200U);
    BOOST_TEST(
        as_root("Action=CreateRole&RoleName=r&AssumeRolePolicyDocument=" + enc(k_trust)).status ==
        200U);
    for (const auto& [kind, param] :
         {std::pair{"User", "UserName=u"}, std::pair{"Group", "GroupName=g"},
          std::pair{"Role", "RoleName=r"}}) {
        const std::string k(kind);
        const std::string p(param);
        BOOST_TEST(as_root("Action=Put" + k + "Policy&" + p +
                           "&PolicyName=Read&PolicyDocument=" + enc(k_doc))
                           .status == 200U,
                   k);
        auto r = as_root("Action=Get" + k + "Policy&" + p + "&PolicyName=read");
        BOOST_TEST(element(r.body, "PolicyName") == "Read");
        BOOST_TEST(element(r.body, "PolicyDocument") == enc(k_doc));
        BOOST_TEST(element(as_root("Action=List" + k + "Policies&" + p).body, "member") == "Read");
        BOOST_TEST(code_of(as_root("Action=Put" + k + "Policy&" + p +
                                   "&PolicyName=bad%20name&PolicyDocument=" + enc(k_doc))) ==
                   "ValidationError");
        BOOST_TEST(code_of(as_root("Action=Put" + k + "Policy&" + p +
                                   "&PolicyName=x&PolicyDocument=" + enc(k_trust))) ==
                   "MalformedPolicyDocument");
        BOOST_TEST(as_root("Action=Delete" + k + "Policy&" + p + "&PolicyName=Read").status ==
                   200U);
        BOOST_TEST(code_of(as_root("Action=Get" + k + "Policy&" + p + "&PolicyName=Read")) ==
                   "NoSuchEntity");
    }
    BOOST_TEST(code_of(as_root("Action=GetRolePolicy&RoleName=nobody&PolicyName=x")) ==
               "NoSuchEntity");
}

BOOST_FIXTURE_TEST_CASE(policy_arn_and_permissions_boundary_reach_the_condition_keys, fixture) {
    BOOST_TEST(as_root("Action=CreatePolicy&PolicyName=p1&PolicyDocument=" + enc(k_doc)).status ==
               200U);
    BOOST_TEST(
        as_root("Action=CreateRole&RoleName=r&AssumeRolePolicyDocument=" + enc(k_trust)).status ==
        200U);
    // iam:PolicyARN: may attach only p1; iam:PermissionsBoundary: may set
    // only p1 as a boundary, and create roles only with it.
    allow(
        "admin",
        R"({"Effect":"Allow","Action":["iam:AttachRolePolicy","iam:DetachRolePolicy"],"Resource":"*","Condition":{"ArnEquals":{"iam:PolicyARN":"arn:aws:iam::111122223333:policy/p1"}}},)"
        R"({"Effect":"Allow","Action":["iam:PutRolePermissionsBoundary","iam:CreateRole"],"Resource":"*","Condition":{"StringEquals":{"iam:PermissionsBoundary":"arn:aws:iam::111122223333:policy/p1"}}},)"
        R"({"Effect":"Allow","Action":"iam:DeleteRolePermissionsBoundary","Resource":"*","Condition":{"StringEquals":{"iam:PermissionsBoundary":"arn:aws:iam::aws:policy/ReadOnlyAccess"}}})");
    const auto admin = user("admin");
    const auto attach = [&](std::string_view parn) {
        return call(admin, "Action=AttachRolePolicy&RoleName=r&PolicyArn=" + std::string(parn));
    };
    BOOST_TEST(attach(k_policy_arn).status == 200U);
    BOOST_TEST(code_of(attach(k_readonly)) == "AccessDenied");
    BOOST_TEST(
        call(admin, "Action=DetachRolePolicy&RoleName=r&PolicyArn=" + std::string(k_policy_arn))
            .status == 200U);

    BOOST_TEST(code_of(call(admin,
                            "Action=PutRolePermissionsBoundary&RoleName=r&"
                            "PermissionsBoundary=" +
                                std::string(k_readonly))) == "AccessDenied");
    BOOST_TEST(call(admin, "Action=PutRolePermissionsBoundary&RoleName=r&PermissionsBoundary=" +
                               std::string(k_policy_arn))
                   .status == 200U);
    BOOST_TEST(element(as_root("Action=GetRole&RoleName=r").body, "PermissionsBoundaryArn") ==
               k_policy_arn);
    BOOST_TEST(element(as_root("Action=GetPolicy&PolicyArn=" + std::string(k_policy_arn)).body,
                       "PermissionsBoundaryUsageCount") == "1");
    // Delete is keyed on the boundary being removed: p1 here, so denied.
    BOOST_TEST(code_of(call(admin, "Action=DeleteRolePermissionsBoundary&RoleName=r")) ==
               "AccessDenied");
    BOOST_TEST(as_root("Action=DeleteRolePermissionsBoundary&RoleName=r").status == 200U);
    BOOST_TEST(code_of(as_root("Action=DeleteRolePermissionsBoundary&RoleName=r")) ==
               "NoSuchEntity");

    const auto create = [&](const std::string& extra) {
        return call(admin, "Action=CreateRole&RoleName=r2&AssumeRolePolicyDocument=" +
                               enc(k_trust) + extra);
    };
    BOOST_TEST(code_of(create("")) == "AccessDenied");
    BOOST_TEST(create("&PermissionsBoundary=" + std::string(k_policy_arn)).status == 200U);

    // A user boundary works the same way; a missing policy is NoSuchEntity.
    BOOST_TEST(as_root("Action=PutUserPermissionsBoundary&UserName=admin&PermissionsBoundary=" +
                       std::string(k_readonly))
                   .status == 200U);
    BOOST_TEST(code_of(as_root("Action=PutUserPermissionsBoundary&UserName=admin&"
                               "PermissionsBoundary=arn:aws:iam::111122223333:policy/none")) ==
               "NoSuchEntity");
    BOOST_TEST(code_of(as_root("Action=PutUserPermissionsBoundary&UserName=admin&"
                               "PermissionsBoundary=short")) == "ValidationError");
}

BOOST_FIXTURE_TEST_CASE(adding_a_role_to_an_instance_profile_needs_pass_role_to_ec2, fixture) {
    BOOST_TEST(
        as_root("Action=CreateRole&RoleName=r&AssumeRolePolicyDocument=" + enc(k_trust)).status ==
        200U);
    auto r = as_root("Action=CreateInstanceProfile&InstanceProfileName=ip&Path=%2Fweb%2F");
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(element(r.body, "InstanceProfileId").starts_with("AIPA"));
    BOOST_TEST(element(r.body, "Arn") == "arn:aws:iam::111122223333:instance-profile/web/ip");

    // iam:PassedToService: PassRole is allowed only towards Lambda, so the
    // EC2 instance profile is refused until it is allowed towards EC2.
    allow(
        "ops",
        R"({"Effect":"Allow","Action":"iam:AddRoleToInstanceProfile","Resource":"*"},)"
        R"({"Effect":"Allow","Action":"iam:PassRole","Resource":"*","Condition":{"StringEquals":{"iam:PassedToService":"lambda.amazonaws.com"}}})");
    const auto ops = user("ops");
    const std::string add = "Action=AddRoleToInstanceProfile&InstanceProfileName=ip&RoleName=r";
    r = call(ops, add);
    BOOST_TEST(code_of(r) == "AccessDenied");
    BOOST_TEST(r.body.find("iam:PassRole") != std::string::npos);
    (void)c.submit(leader::command(
        iam_command::put_inline_policy,
        {{"account", k_account},
         {"kind", "user"},
         {"name", "ops"},
         {"policy_name", "p"},
         {"document",
          R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"iam:AddRoleToInstanceProfile","Resource":"*"},{"Effect":"Allow","Action":"iam:PassRole","Resource":"*","Condition":{"StringEquals":{"iam:PassedToService":"ec2.amazonaws.com"},"ArnLike":{"iam:AssociatedResourceArn":"arn:aws:iam::111122223333:instance-profile/web/*"}}}]})"}}));
    BOOST_TEST(call(ops, add).status == 200U);

    r = as_root("Action=GetInstanceProfile&InstanceProfileName=ip");
    BOOST_TEST(element(r.body, "RoleName") == "r");
    r = as_root("Action=ListInstanceProfilesForRole&RoleName=r");
    BOOST_TEST(element(r.body, "InstanceProfileName") == "ip");
    BOOST_TEST(count(as_root("Action=ListInstanceProfiles&PathPrefix=%2Fweb%2F").body,
                     "<InstanceProfileName>") == 1U);
    // One role per profile, and a role in a profile cannot be deleted.
    BOOST_TEST(
        as_root("Action=CreateRole&RoleName=r2&AssumeRolePolicyDocument=" + enc(k_trust)).status ==
        200U);
    BOOST_TEST(
        code_of(as_root("Action=AddRoleToInstanceProfile&InstanceProfileName=ip&RoleName=r2")) ==
        "LimitExceeded");
    BOOST_TEST(code_of(as_root("Action=DeleteRole&RoleName=r")) == "DeleteConflict");
    BOOST_TEST(code_of(as_root("Action=DeleteInstanceProfile&InstanceProfileName=ip")) ==
               "DeleteConflict");

    BOOST_TEST(as_root("Action=TagInstanceProfile&InstanceProfileName=ip&Tags.member.1.Key=a&"
                       "Tags.member.1.Value=b")
                   .status == 200U);
    BOOST_TEST(count(as_root("Action=ListInstanceProfileTags&InstanceProfileName=ip").body,
                     "<Key>") == 1U);
    BOOST_TEST(
        as_root("Action=UntagInstanceProfile&InstanceProfileName=ip&TagKeys.member.1=a").status ==
        200U);

    // Removing needs no PassRole; then both deletes succeed.
    BOOST_TEST(
        as_root("Action=RemoveRoleFromInstanceProfile&InstanceProfileName=ip&RoleName=r").status ==
        200U);
    BOOST_TEST(as_root("Action=DeleteInstanceProfile&InstanceProfileName=ip").status == 200U);
    BOOST_TEST(as_root("Action=DeleteRole&RoleName=r").status == 200U);
    BOOST_TEST(code_of(as_root("Action=GetInstanceProfile&InstanceProfileName=ip")) ==
               "NoSuchEntity");
}

BOOST_FIXTURE_TEST_CASE(a_role_with_policies_cannot_be_deleted_until_they_are_removed, fixture) {
    BOOST_TEST(
        as_root("Action=CreateRole&RoleName=r&AssumeRolePolicyDocument=" + enc(k_trust)).status ==
        200U);
    BOOST_TEST(
        as_root("Action=AttachRolePolicy&RoleName=r&PolicyArn=" + std::string(k_readonly)).status ==
        200U);
    BOOST_TEST(as_root("Action=PutRolePolicy&RoleName=r&PolicyName=i&PolicyDocument=" + enc(k_doc))
                   .status == 200U);
    BOOST_TEST(code_of(as_root("Action=DeleteRole&RoleName=r")) == "DeleteConflict");
    BOOST_TEST(count(as_root("Action=ListAttachedRolePolicies&RoleName=r").body,
                     "<PolicyName>ReadOnlyAccess</PolicyName>") == 1U);
    BOOST_TEST(count(as_root("Action=ListAttachedRolePolicies&RoleName=r&PathPrefix=%2Fx%2F").body,
                     "<member>") == 0U);
    BOOST_TEST(
        as_root("Action=DetachRolePolicy&RoleName=r&PolicyArn=" + std::string(k_readonly)).status ==
        200U);
    BOOST_TEST(as_root("Action=DeleteRolePolicy&RoleName=r&PolicyName=i").status == 200U);
    BOOST_TEST(as_root("Action=DeleteRole&RoleName=r").status == 200U);
}

BOOST_FIXTURE_TEST_CASE(task_15_actions_are_authorized_on_their_resources, fixture) {
    BOOST_TEST(
        as_root("Action=CreateRole&RoleName=r&AssumeRolePolicyDocument=" + enc(k_trust)).status ==
        200U);
    BOOST_TEST(as_root("Action=CreatePolicy&PolicyName=p1&PolicyDocument=" + enc(k_doc)).status ==
               200U);
    allow(
        "reader",
        R"({"Effect":"Allow","Action":["iam:GetRole","iam:GetPolicy"],"Resource":["arn:aws:iam::111122223333:role/r","arn:aws:iam::111122223333:policy/p1"]})");
    const auto reader = user("reader");
    BOOST_TEST(call(reader, "Action=GetRole&RoleName=r").status == 200U);
    BOOST_TEST(call(reader, "Action=GetPolicy&PolicyArn=" + std::string(k_policy_arn)).status ==
               200U);
    BOOST_TEST(code_of(call(reader, "Action=ListRoles")) == "AccessDenied");
    BOOST_TEST(code_of(call(reader, "Action=DeleteRole&RoleName=r")) == "AccessDenied");
    // A missing role is authorized first, so it reads as AccessDenied.
    BOOST_TEST(code_of(call(reader, "Action=GetRole&RoleName=ghost")) == "AccessDenied");
    BOOST_TEST(code_of(call(reader, "Action=GetPolicy&PolicyArn=" + std::string(k_readonly))) ==
               "AccessDenied");
    // Another account's policy is as unknown as a missing one.
    BOOST_TEST(code_of(as_root("Action=GetPolicy&PolicyArn=arn:aws:iam::444455556666:policy/p1")) ==
               "NoSuchEntity");
}

BOOST_FIXTURE_TEST_CASE(policy_versions_page_in_version_order, fixture) {
    BOOST_TEST(as_root("Action=CreatePolicy&PolicyName=p1&PolicyDocument=" + enc(k_doc)).status ==
               200U);
    const auto parn = "&PolicyArn=" + std::string(k_policy_arn);
    for (int i = 0; i < 4; ++i) {
        BOOST_TEST(as_root("Action=CreatePolicyVersion" + parn + "&PolicyDocument=" + enc(k_doc2))
                       .status == 200U);
    }
    // A sixth version is LimitExceeded (INV-POL-2).
    BOOST_TEST(code_of(as_root("Action=CreatePolicyVersion" + parn +
                               "&PolicyDocument=" + enc(k_doc2))) == "LimitExceeded");
    std::vector<std::string> seen;
    std::string marker;
    for (int i = 0; i < 5; ++i) {
        const auto r = as_root("Action=ListPolicyVersions" + parn + "&MaxItems=2" +
                               (marker.empty() ? "" : "&Marker=" + marker));
        std::string body = r.body;
        static const std::regex re("<VersionId>([^<]*)</VersionId>");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
             it != std::sregex_iterator(); ++it) {
            seen.push_back((*it)[1].str());
        }
        marker = element(r.body, "Marker");
        if (element(r.body, "IsTruncated") == "false") {
            break;
        }
    }
    BOOST_TEST(seen == (std::vector<std::string>{"v1", "v2", "v3", "v4", "v5"}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Task 16: OIDC providers, simulation and account calls (Requirements
// 9.6, 9.7, 6.7) ──

namespace {

constexpr std::string_view k_oidc_arn =
    "arn:aws:iam::111122223333:oidc-provider/token.example.com/ci";
const std::string k_thumb(40, 'a');

/// `<name>.member.N=<value>` for each value.
auto members(const std::string& name, const std::vector<std::string>& values) -> std::string {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        out += "&" + name + ".member." + std::to_string(i + 1) + "=" + enc(values[i]);
    }
    return out;
}

auto decision_of(const std::string& xml) -> std::string {
    return element(xml, "EvalDecision");
}

}  // namespace

BOOST_AUTO_TEST_SUITE(oidc_simulation_and_account)

BOOST_FIXTURE_TEST_CASE(an_oidc_provider_is_created_read_updated_and_deleted, fixture) {
    auto r =
        as_root("Action=CreateOpenIDConnectProvider&Url=" + enc("https://token.example.com/ci") +
                members("ClientIDList", {"sts"}) + members("ThumbprintList", {k_thumb}) +
                "&Tags.member.1.Key=a&Tags.member.1.Value=b");
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(element(r.body, "OpenIDConnectProviderArn") == k_oidc_arn);
    BOOST_TEST(code_of(as_root("Action=CreateOpenIDConnectProvider&Url=" +
                               enc("https://token.example.com/ci"))) == "EntityAlreadyExists");

    const auto parn = "&OpenIDConnectProviderArn=" + enc(k_oidc_arn);
    r = as_root("Action=GetOpenIDConnectProvider" + parn);
    BOOST_TEST(element(r.body, "Url") == "token.example.com/ci");
    BOOST_TEST(element(r.body, "member") == "sts");
    BOOST_TEST(count(r.body, k_thumb) == 1U);

    BOOST_TEST(
        as_root("Action=AddClientIDToOpenIDConnectProvider" + parn + "&ClientID=app").status ==
        200U);
    BOOST_TEST(count(as_root("Action=GetOpenIDConnectProvider" + parn).body, "<member>app") == 1U);
    BOOST_TEST(
        as_root("Action=RemoveClientIDFromOpenIDConnectProvider" + parn + "&ClientID=sts").status ==
        200U);
    const std::string other(40, 'b');
    BOOST_TEST(as_root("Action=UpdateOpenIDConnectProviderThumbprint" + parn +
                       members("ThumbprintList", {other}))
                   .status == 200U);
    r = as_root("Action=GetOpenIDConnectProvider" + parn);
    BOOST_TEST(count(r.body, other) == 1U);
    BOOST_TEST(count(r.body, "<member>sts") == 0U);
    BOOST_TEST(count(as_root("Action=ListOpenIDConnectProviders").body, "<Arn>") == 1U);
    BOOST_TEST(count(as_root("Action=ListOpenIDConnectProviderTags" + parn).body, "<Key>") == 1U);
    BOOST_TEST(as_root("Action=UntagOpenIDConnectProvider" + parn + "&TagKeys.member.1=a").status ==
               200U);

    BOOST_TEST(as_root("Action=DeleteOpenIDConnectProvider" + parn).status == 200U);
    BOOST_TEST(code_of(as_root("Action=GetOpenIDConnectProvider" + parn)) == "NoSuchEntity");
}

BOOST_FIXTURE_TEST_CASE(inv_oidc_1_provider_urls_are_checked, fixture) {
    for (const auto* bad : {"http://token.example.com", "https://", "https://h:8443/x",
                            "https://u@h/x", "https://h/x?y=1", "https://h/x#f"}) {
        BOOST_TEST(code_of(as_root("Action=CreateOpenIDConnectProvider&Url=" + enc(bad))) ==
                       "ValidationError",
                   bad);
    }
    BOOST_TEST(code_of(as_root("Action=CreateOpenIDConnectProvider&Url=" +
                               enc("https://" + std::string(250, 'h')))) == "ValidationError");
    BOOST_TEST(code_of(as_root("Action=CreateOpenIDConnectProvider")) == "ValidationError");
    BOOST_TEST(
        as_root("Action=CreateOpenIDConnectProvider&Url=" + enc("https://h.example/p")).status ==
        200U);
}

BOOST_FIXTURE_TEST_CASE(inv_oidc_2_client_ids_are_checked, fixture) {
    const auto url = "Action=CreateOpenIDConnectProvider&Url=" + enc("https://h.example");
    BOOST_TEST(code_of(as_root(url + "&ClientIDList.member.1=")) == "ValidationError");
    BOOST_TEST(code_of(as_root(url + members("ClientIDList", {std::string(256, 'c')}))) ==
               "ValidationError");
    std::vector<std::string> many;
    for (int i = 0; i < 101; ++i) {
        many.push_back("c" + std::to_string(i));
    }
    BOOST_TEST(code_of(as_root(url + members("ClientIDList", many))) == "LimitExceeded");
    many.pop_back();
    BOOST_TEST(as_root(url + members("ClientIDList", many)).status == 200U);
    BOOST_TEST(code_of(as_root("Action=AddClientIDToOpenIDConnectProvider&"
                               "OpenIDConnectProviderArn=" +
                               enc("arn:aws:iam::111122223333:oidc-provider/h.example") +
                               "&ClientID=one-too-many")) == "LimitExceeded");
}

BOOST_FIXTURE_TEST_CASE(inv_oidc_3_thumbprints_are_checked, fixture) {
    const auto url = "Action=CreateOpenIDConnectProvider&Url=" + enc("https://h.example");
    BOOST_TEST(code_of(as_root(url + members("ThumbprintList", {std::string(39, 'a')}))) ==
               "ValidationError");
    BOOST_TEST(code_of(as_root(url + members("ThumbprintList", {std::string(40, 'g')}))) ==
               "ValidationError");
    BOOST_TEST(as_root(url + members("ThumbprintList", {std::string(40, 'F')})).status == 200U);
    BOOST_TEST(code_of(as_root("Action=UpdateOpenIDConnectProviderThumbprint&"
                               "OpenIDConnectProviderArn=" +
                               enc("arn:aws:iam::111122223333:oidc-provider/h.example") +
                               members("ThumbprintList", {"xyz"}))) == "ValidationError");
}

BOOST_FIXTURE_TEST_CASE(simulate_principal_policy_uses_the_entitys_stored_policies, fixture) {
    allow(
        "alice",
        R"({"Effect":"Allow","Action":"s3:GetObject","Resource":"*"},)"
        R"({"Effect":"Deny","Action":"s3:DeleteObject","Resource":"*"},)"
        R"({"Effect":"Allow","Action":"s3:PutObject","Resource":"*","Condition":{"StringEquals":{"aws:RequestedRegion":"eu-west-1"}}})");
    const auto base = "Action=SimulatePrincipalPolicy&PolicySourceArn=" +
                      enc("arn:aws:iam::111122223333:user/alice");
    auto r = as_root(base +
                     members("ActionNames", {"s3:GetObject", "s3:DeleteObject", "s3:ListBucket"}) +
                     members("ResourceArns", {"arn:aws:s3:::b/k"}));
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(count(r.body, "<EvalDecision>allowed</EvalDecision>") == 1U);
    BOOST_TEST(count(r.body, "<EvalDecision>explicitDeny</EvalDecision>") == 1U);
    BOOST_TEST(count(r.body, "<EvalDecision>implicitDeny</EvalDecision>") == 1U);
    BOOST_TEST(count(r.body, "<SourcePolicyId>user/alice:p</SourcePolicyId>") == 2U);

    // A condition key the caller did not supply is reported missing; given
    // as a context entry it decides.
    r = as_root(base + members("ActionNames", {"s3:PutObject"}));
    BOOST_TEST(decision_of(r.body) == "implicitDeny");
    BOOST_TEST(element(r.body, "member") == "aws:RequestedRegion");
    r = as_root(base + members("ActionNames", {"s3:PutObject"}) +
                "&ContextEntries.member.1.ContextKeyName=aws:RequestedRegion"
                "&ContextEntries.member.1.ContextKeyType=string"
                "&ContextEntries.member.1.ContextKeyValues.member.1=eu-west-1");
    BOOST_TEST(decision_of(r.body) == "allowed");
    BOOST_TEST(count(r.body, "<MissingContextValues></MissingContextValues>") == 1U);

    // Group policies reach the user; a boundary input caps them.
    BOOST_TEST(as_root("Action=CreateGroup&GroupName=g").status == 200U);
    BOOST_TEST(as_root("Action=AddUserToGroup&GroupName=g&UserName=alice").status == 200U);
    BOOST_TEST(as_root("Action=AttachGroupPolicy&GroupName=g&PolicyArn=" + std::string(k_readonly))
                   .status == 200U);
    r = as_root(base + members("ActionNames", {"ec2:DescribeInstances"}));
    BOOST_TEST(decision_of(r.body) == "allowed");
    r = as_root(base + members("ActionNames", {"ec2:DescribeInstances"}) +
                members("PermissionsBoundaryPolicyInputList", {std::string(k_doc)}));
    BOOST_TEST(decision_of(r.body) == "implicitDeny");
    // An extra identity policy input adds to the stored ones.
    r = as_root(base + members("ActionNames", {"s3:PutObject"}) +
                members("PolicyInputList", {std::string(k_doc2)}));
    BOOST_TEST(decision_of(r.body) == "allowed");

    BOOST_TEST(code_of(as_root("Action=SimulatePrincipalPolicy&PolicySourceArn=" +
                               enc("arn:aws:iam::111122223333:user/nobody") +
                               members("ActionNames", {"s3:GetObject"}))) == "NoSuchEntity");
    BOOST_TEST(code_of(as_root(base)) == "ValidationError");
}

BOOST_FIXTURE_TEST_CASE(simulation_is_authorized_on_the_source_entity, fixture) {
    BOOST_TEST(
        as_root("Action=CreateRole&RoleName=r&AssumeRolePolicyDocument=" + enc(k_trust)).status ==
        200U);
    allow(
        "auditor",
        R"({"Effect":"Allow","Action":"iam:SimulatePrincipalPolicy","Resource":"arn:aws:iam::111122223333:user/*"})");
    const auto auditor = user("auditor");
    const auto sim = [&](const std::string& source) {
        return call(auditor, "Action=SimulatePrincipalPolicy&PolicySourceArn=" + enc(source) +
                                 members("ActionNames", {"s3:GetObject"}));
    };
    BOOST_TEST(sim("arn:aws:iam::111122223333:user/auditor").status == 200U);
    BOOST_TEST(code_of(sim("arn:aws:iam::111122223333:role/r")) == "AccessDenied");
    BOOST_TEST(code_of(call(auditor, "Action=SimulateCustomPolicy" +
                                         members("ActionNames", {"s3:GetObject"}) +
                                         members("PolicyInputList", {std::string(k_doc)}))) ==
               "AccessDenied");
}

BOOST_FIXTURE_TEST_CASE(context_keys_are_listed_once_in_first_spelling, fixture) {
    allow(
        "bob",
        R"({"Effect":"Allow","Action":"s3:*","Resource":"*","Condition":{"StringEquals":{"aws:RequestedRegion":"x","s3:prefix":"y"}}},)"
        R"({"Effect":"Deny","Action":"ec2:*","Resource":"*","Condition":{"Bool":{"AWS:requestedregion":"x"}}})");
    auto r = as_root("Action=GetContextKeysForPrincipalPolicy&PolicySourceArn=" +
                     enc("arn:aws:iam::111122223333:user/bob"));
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(count(r.body, "<member>") == 2U);
    BOOST_TEST(count(r.body, "<member>aws:RequestedRegion</member>") == 1U);
    r = as_root(
        "Action=GetContextKeysForCustomPolicy" +
        members(
            "PolicyInputList",
            {R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"s3:*","Resource":"*","Condition":{"IpAddress":{"aws:SourceIp":"10.0.0.0/8"}}}]})"}));
    BOOST_TEST(element(r.body, "member") == "aws:SourceIp");
    BOOST_TEST(code_of(as_root("Action=GetContextKeysForCustomPolicy")) == "ValidationError");
}

/// Every corpus `evaluate` case SimulateCustomPolicy can express (an IAM
/// user in its own account, identity policies, an optional boundary and
/// resource policy, no session) gives the corpus's decision.
BOOST_FIXTURE_TEST_CASE(simulate_custom_policy_matches_the_corpus_decisions, fixture) {
    std::size_t compared = 0;
    for (const auto& file :
         std::filesystem::directory_iterator(std::string(KYTHIRA_IAM_CORPUS_DIR) + "/evaluate")) {
        std::ifstream in(file.path());
        std::stringstream text;
        text << in.rdbuf();
        const auto doc = boost::json::parse(text.str()).as_object();
        for (const auto& cv : doc.at("cases").as_array()) {
            const auto& c = cv.as_object();
            const std::string who(c.at("principal").as_string());
            const auto* identity = c.if_contains("identity");
            if (!who.starts_with("user/") || identity == nullptr || identity->as_array().empty() ||
                c.contains("session") || c.contains("principal_account") ||
                c.contains("resource_account") ||
                (c.contains("resource_kind") && c.at("resource_kind").as_string() != "ordinary")) {
                continue;
            }
            const auto serial = [](const boost::json::value& v) {
                return v.is_string() ? std::string(v.as_string()) : boost::json::serialize(v);
            };
            std::vector<std::string> ids;
            for (const auto& d : identity->as_array()) {
                ids.push_back(serial(d));
            }
            std::string body = "Action=SimulateCustomPolicy" +
                               members("ActionNames", {std::string(c.at("action").as_string())}) +
                               members("PolicyInputList", ids) +
                               "&CallerArn=" + enc("arn:aws:iam::111122223333:" + who);
            if (const auto* res = c.if_contains("resource")) {
                body += members("ResourceArns", {std::string(res->as_string())});
            }
            if (const auto* b = c.if_contains("boundary")) {
                body += members("PermissionsBoundaryPolicyInputList", {serial(*b)});
            }
            if (const auto* rp = c.if_contains("resource_policy")) {
                body += "&ResourcePolicy=" + enc(serial(*rp));
            }
            if (const auto* ctx = c.if_contains("context")) {
                std::size_t i = 0;
                for (const auto& [k, v] : ctx->as_object()) {
                    const auto base = "&ContextEntries.member." + std::to_string(++i) + ".";
                    body += base + "ContextKeyName=" + enc(std::string(k));
                    std::vector<std::string> values;
                    if (v.is_array()) {
                        for (const auto& x : v.as_array()) {
                            values.push_back(std::string(x.as_string()));
                        }
                        body += base + "ContextKeyType=stringList";
                    } else {
                        values.push_back(std::string(v.as_string()));
                    }
                    for (std::size_t j = 0; j < values.size(); ++j) {
                        body += base + "ContextKeyValues.member." + std::to_string(j + 1) + "=" +
                                enc(values[j]);
                    }
                }
            }
            const auto r = as_root(body);
            const std::string expect(c.at("expect").as_string());
            const std::string want = expect == "allow"           ? "allowed"
                                     : expect == "explicit_deny" ? "explicitDeny"
                                                                 : "implicitDeny";
            BOOST_TEST(decision_of(r.body) == want, file.path().filename().string()
                                                        << ": " << c.at("name").as_string() << "\n"
                                                        << r.body);
            ++compared;
        }
    }
    BOOST_TEST_MESSAGE("compared " << compared << " corpus cases");
    BOOST_TEST(compared >= 40U);
}

BOOST_FIXTURE_TEST_CASE(account_authorization_details_and_summary, fixture) {
    BOOST_TEST(as_root("Action=CreateUser&UserName=u").status == 200U);
    BOOST_TEST(as_root("Action=CreateGroup&GroupName=g").status == 200U);
    BOOST_TEST(as_root("Action=AddUserToGroup&GroupName=g&UserName=u").status == 200U);
    BOOST_TEST(as_root("Action=PutUserPolicy&UserName=u&PolicyName=i&PolicyDocument=" + enc(k_doc))
                   .status == 200U);
    BOOST_TEST(
        as_root("Action=CreateRole&RoleName=r&AssumeRolePolicyDocument=" + enc(k_trust)).status ==
        200U);
    BOOST_TEST(
        as_root("Action=AttachRolePolicy&RoleName=r&PolicyArn=" + std::string(k_readonly)).status ==
        200U);
    BOOST_TEST(as_root("Action=CreatePolicy&PolicyName=p1&PolicyDocument=" + enc(k_doc)).status ==
               200U);

    auto r = as_root("Action=GetAccountAuthorizationDetails");
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(count(r.body, "<UserName>u</UserName>") == 1U);
    BOOST_TEST(count(r.body, "<GroupList><member>g</member></GroupList>") == 1U);
    BOOST_TEST(count(r.body, "<PolicyDocument>" + enc(k_doc) + "</PolicyDocument>") == 1U);
    BOOST_TEST(count(r.body, "<RoleName>r</RoleName>") == 1U);
    // Local policies, and only the AWS-managed ones this account uses.
    BOOST_TEST(count(r.body, "<PolicyName>p1</PolicyName>") == 1U);
    BOOST_TEST(count(r.body, "<PolicyName>ReadOnlyAccess</PolicyName>") ==
               2U);  // attached + listed
    BOOST_TEST(count(r.body, "<PolicyName>AdministratorAccess</PolicyName>") == 0U);

    r = as_root("Action=GetAccountAuthorizationDetails&Filter.member.1=Role");
    BOOST_TEST(count(r.body, "<UserName>") == 0U);
    BOOST_TEST(count(r.body, "<RoleName>r</RoleName>") == 1U);
    BOOST_TEST(code_of(as_root("Action=GetAccountAuthorizationDetails&Filter.member.1=Bucket")) ==
               "ValidationError");
    // It pages like any list.
    r = as_root("Action=GetAccountAuthorizationDetails&MaxItems=2");
    BOOST_TEST(element(r.body, "IsTruncated") == "true");
    BOOST_TEST(as_root("Action=GetAccountAuthorizationDetails&MaxItems=2&Marker=" +
                       element(r.body, "Marker"))
                   .status == 200U);

    r = as_root("Action=GetAccountSummary");
    const auto value = [&](const std::string& key) {
        std::smatch m;
        const std::regex re("<key>" + key + "</key><value>([0-9]+)</value>");
        return std::regex_search(r.body, m, re) ? m[1].str() : std::string{};
    };
    BOOST_TEST(value("Users") == "1");
    BOOST_TEST(value("Groups") == "1");
    BOOST_TEST(value("Roles") == "1");
    BOOST_TEST(value("Policies") == "1");
    BOOST_TEST(value("VersionsPerPolicyQuota") == "5");
    BOOST_TEST(value("AccessKeysPerUserQuota") == "2");
}

BOOST_FIXTURE_TEST_CASE(unlisted_actions_are_not_implemented, fixture) {
    for (const auto* a : {"CreateLoginProfile", "CreateSAMLProvider", "UploadServerCertificate",
                          "CreateVirtualMFADevice", "CreateServiceLinkedRole"}) {
        const auto r = as_root(std::string("Action=") + a);
        BOOST_TEST(r.status == 501U, a);
        BOOST_TEST(code_of(r) == "NotImplemented");
    }
}

BOOST_AUTO_TEST_SUITE_END()
