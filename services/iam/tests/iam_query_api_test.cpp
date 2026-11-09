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
#include <mutex>
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
