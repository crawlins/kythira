// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The policy evaluator (`.kiro/specs/iam-compatible-auth/` task 12,
// Requirement 6): one named case per INV-EVAL-* row of invariants.md, plus
// the decision's reporting. The corpus (corpus/evaluate/, run by
// iam_policy_test) holds the many variations; these cases are the
// invariants stated once each, in code a reader can follow.

#define BOOST_TEST_MODULE iam_evaluator_test

#include <boost/test/unit_test.hpp>

#include <kythira/iam/iam_evaluator.hpp>
#include <kythira/iam/iam_policy.hpp>

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace iam = kythira::iam;

namespace {

constexpr std::string_view k_account = "111122223333";
constexpr std::string_view k_other = "444455556666";
constexpr std::string_view k_object = "arn:aws:s3:::bucket/key";

// Builds requests and owns the documents they point at.
class fixture {
public:
    auto doc(std::string_view json, iam::policy_type t = iam::policy_type::identity)
        -> const iam::policy_document* {
        _docs.push_back(iam::parse_policy(json, t));
        return &_docs.back();
    }
    auto named(std::string name, std::string_view json,
               iam::policy_type t = iam::policy_type::identity) -> iam::named_policy {
        return {std::move(name), doc(json, t)};
    }

private:
    std::deque<iam::policy_document> _docs;
};

auto user(std::string_view name, std::string_view account = k_account) -> iam::principal {
    iam::principal p;
    p.kind = iam::principal_kind::user;
    p.account = std::string(account);
    p.arn = "arn:aws:iam::" + p.account + ":user/" + std::string(name);
    return p;
}

auto session(std::string_view role, std::string_view name, std::string_view account = k_account)
    -> iam::principal {
    iam::principal p;
    p.kind = iam::principal_kind::role_session;
    p.account = std::string(account);
    p.role_arn = "arn:aws:iam::" + p.account + ":role/" + std::string(role);
    p.arn = "arn:aws:sts::" + p.account + ":assumed-role/" + std::string(role) + "/" +
            std::string(name);
    return p;
}

auto request(iam::principal who) -> iam::evaluation_request {
    iam::evaluation_request r;
    r.who = std::move(who);
    r.action = "s3:GetObject";
    r.resource = std::string(k_object);
    return r;
}

constexpr std::string_view k_allow_get = R"({"Version":"2012-10-17","Statement":[
    {"Sid":"Get","Effect":"Allow","Action":"s3:GetObject","Resource":"arn:aws:s3:::bucket/*"}]})";
constexpr std::string_view k_allow_ec2 = R"({"Version":"2012-10-17","Statement":[
    {"Effect":"Allow","Action":"ec2:*","Resource":"*"}]})";
constexpr std::string_view k_allow_all = R"({"Version":"2012-10-17","Statement":[
    {"Effect":"Allow","Action":"*","Resource":"*"}]})";

// A resource policy allowing s3:GetObject on the bucket to `principal`
// (a JSON value).
auto grant_to(std::string_view principal) -> std::string {
    return R"({"Version":"2012-10-17","Statement":[{"Sid":"Grant","Effect":"Allow","Principal":)" +
           std::string(principal) +
           R"(,"Action":"s3:GetObject","Resource":"arn:aws:s3:::bucket/*"}]})";
}

auto outcome(const iam::decision& d) -> iam::decision::outcome {
    return d.result;
}

using o = iam::decision::outcome;

}  // namespace

BOOST_AUTO_TEST_SUITE(iam_evaluation_invariants)

BOOST_AUTO_TEST_CASE(inv_eval_1_default_deny) {
    auto r = request(user("alice"));
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
    fixture f;
    r.identity.push_back(f.named("p", k_allow_ec2));
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
}

BOOST_AUTO_TEST_CASE(inv_eval_2_explicit_deny_in_any_policy_wins) {
    constexpr std::string_view deny = R"({"Version":"2012-10-17","Statement":[
        {"Sid":"No","Effect":"Deny","Action":"s3:*","Resource":"*"}]})";
    constexpr std::string_view resource_deny = R"({"Version":"2012-10-17","Statement":[
        {"Sid":"No","Effect":"Deny","Principal":"*","Action":"s3:*","Resource":"*"}]})";
    const auto resource_allow = grant_to(R"("*")");
    // A deny in each of the four places a policy comes from, against an
    // allow in every other one.
    for (int where = 0; where < 4; ++where) {
        BOOST_TEST_CONTEXT("deny in place " << where) {
            fixture f;
            auto r = request(session("reader", "s1"));
            r.identity.push_back(f.named("identity", where == 0 ? deny : k_allow_all));
            r.resource_policy =
                f.named("resource", where == 1 ? resource_deny : std::string_view(resource_allow),
                        iam::policy_type::resource);
            r.boundary =
                f.named("boundary", where == 2 ? deny : k_allow_all, iam::policy_type::boundary);
            r.session.push_back(
                f.named("session", where == 3 ? deny : k_allow_all, iam::policy_type::session));
            const auto d = iam::evaluate(r);
            BOOST_TEST((d.result == o::explicit_deny));
            BOOST_REQUIRE(d.deciding.has_value());
            BOOST_TEST(d.deciding->sid == "No");
        }
    }
}

BOOST_AUTO_TEST_CASE(inv_eval_3_deny_scan_comes_before_every_allow) {
    // A direct resource grant is a final allow, the root user is allowed
    // everything: an explicit deny still comes first.
    fixture f;
    constexpr std::string_view deny = R"({"Version":"2012-10-17","Statement":[
        {"Effect":"Deny","Action":"s3:GetObject","Resource":"*"}]})";
    auto r = request(user("alice"));
    r.identity.push_back(f.named("identity", deny));
    r.resource_policy =
        f.named("resource", grant_to(R"({"AWS":"arn:aws:iam::111122223333:user/alice"})"),
                iam::policy_type::resource);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::explicit_deny));
    // ...and the boundary is checked after the identity allow, the session
    // policy after the boundary: each fails on its own.
    auto s = request(session("reader", "s1"));
    s.identity.push_back(f.named("identity", k_allow_get));
    s.boundary = f.named("boundary", k_allow_ec2, iam::policy_type::boundary);
    BOOST_TEST(iam::evaluate(s).reason == "the permissions boundary does not allow");
    s.boundary = f.named("boundary", k_allow_all, iam::policy_type::boundary);
    s.session.push_back(f.named("session", k_allow_ec2, iam::policy_type::session));
    BOOST_TEST(iam::evaluate(s).reason == "the session policy does not allow");
}

BOOST_AUTO_TEST_CASE(inv_eval_4_resource_grant_alone_except_trust_and_key_policies) {
    fixture f;
    const auto to_alice = grant_to(R"({"AWS":"arn:aws:iam::111122223333:user/alice"})");
    auto r = request(user("alice"));
    r.resource_policy = f.named("resource", to_alice, iam::policy_type::resource);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::allow));

    // A trust policy must itself allow: an identity allow is not enough.
    auto t = request(user("alice"));
    t.action = "sts:AssumeRole";
    t.resource = "arn:aws:iam::111122223333:role/admin";
    t.resource_kind = iam::resource_policy_kind::trust;
    t.identity.push_back(f.named("identity", k_allow_all));
    BOOST_TEST((outcome(iam::evaluate(t)) == o::implicit_deny));
    t.resource_policy = f.named("trust", R"({"Version":"2012-10-17","Statement":[
        {"Effect":"Allow","Principal":{"AWS":"111122223333"},"Action":"sts:AssumeRole"}]})",
                                iam::policy_type::trust);
    BOOST_TEST((outcome(iam::evaluate(t)) == o::allow));
    // Naming the account delegates to identity policies, so both are needed.
    t.identity.clear();
    BOOST_TEST((outcome(iam::evaluate(t)) == o::implicit_deny));
}

BOOST_AUTO_TEST_CASE(inv_eval_5_boundary_limits_identity_allows_only) {
    fixture f;
    auto r = request(user("alice"));
    r.identity.push_back(f.named("identity", k_allow_all));
    r.boundary = f.named("boundary", k_allow_ec2, iam::policy_type::boundary);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
    r.resource_policy =
        f.named("resource", grant_to(R"({"AWS":"arn:aws:iam::111122223333:user/alice"})"),
                iam::policy_type::resource);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::allow));
}

BOOST_AUTO_TEST_CASE(inv_eval_6_role_arn_grant_limited_session_arn_grant_not) {
    fixture f;
    auto r = request(session("reader", "s1"));
    r.boundary = f.named("boundary", k_allow_ec2, iam::policy_type::boundary);
    r.resource_policy =
        f.named("resource", grant_to(R"({"AWS":"arn:aws:iam::111122223333:role/reader"})"),
                iam::policy_type::resource);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
    r.boundary.reset();
    r.session.push_back(f.named("session", k_allow_ec2, iam::policy_type::session));
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
    r.session.clear();
    BOOST_TEST((outcome(iam::evaluate(r)) == o::allow));

    r.boundary = f.named("boundary", k_allow_ec2, iam::policy_type::boundary);
    r.session.push_back(f.named("session", k_allow_ec2, iam::policy_type::session));
    r.resource_policy = f.named(
        "resource", grant_to(R"({"AWS":"arn:aws:sts::111122223333:assumed-role/reader/s1"})"),
        iam::policy_type::resource);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::allow));
}

BOOST_AUTO_TEST_CASE(inv_eval_7_federated_user_arn_grant_not_limited_by_session_policy) {
    fixture f;
    iam::principal fed;
    fed.kind = iam::principal_kind::federated_user;
    fed.account = std::string(k_account);
    fed.arn = "arn:aws:sts::111122223333:federated-user/fed1";
    fed.user_arn = "arn:aws:iam::111122223333:user/alice";
    auto r = request(fed);
    r.session.push_back(f.named("session", k_allow_ec2, iam::policy_type::session));
    r.resource_policy =
        f.named("resource", grant_to(R"({"AWS":"arn:aws:sts::111122223333:federated-user/fed1"})"),
                iam::policy_type::resource);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::allow));
    // A grant to the user who federated is limited by it.
    r.resource_policy =
        f.named("resource", grant_to(R"({"AWS":"arn:aws:iam::111122223333:user/alice"})"),
                iam::policy_type::resource);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
}

BOOST_AUTO_TEST_CASE(inv_eval_8_session_policy_must_also_allow) {
    fixture f;
    auto r = request(session("reader", "s1"));
    r.identity.push_back(f.named("identity", k_allow_get));
    BOOST_TEST((outcome(iam::evaluate(r)) == o::allow));
    r.session.push_back(f.named("session", k_allow_ec2, iam::policy_type::session));
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
    r.session.push_back(f.named("session2", k_allow_get, iam::policy_type::session));
    BOOST_TEST((outcome(iam::evaluate(r)) == o::allow));
}

BOOST_AUTO_TEST_CASE(inv_eval_9_cross_account_needs_both_sides) {
    fixture f;
    auto r = request(user("alice", k_other));
    r.resource_account = std::string(k_account);
    r.identity.push_back(f.named("identity", k_allow_get));
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
    r.resource_policy =
        f.named("resource", grant_to(R"({"AWS":"444455556666"})"), iam::policy_type::resource);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::allow));
    r.identity.clear();
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
    // Even a grant naming the user itself needs its identity side.
    r.resource_policy =
        f.named("resource", grant_to(R"({"AWS":"arn:aws:iam::444455556666:user/alice"})"),
                iam::policy_type::resource);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
}

BOOST_AUTO_TEST_CASE(inv_eval_10_root_user_allowed_unless_denied) {
    fixture f;
    iam::principal root;
    root.kind = iam::principal_kind::root;
    root.account = std::string(k_account);
    root.arn = "arn:aws:iam::111122223333:root";
    auto r = request(root);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::allow));
    r.resource_policy = f.named("resource", R"({"Version":"2012-10-17","Statement":[
        {"Effect":"Deny","Principal":{"AWS":"111122223333"},"Action":"s3:*","Resource":"*"}]})",
                                iam::policy_type::resource);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::explicit_deny));
}

BOOST_AUTO_TEST_CASE(inv_eval_11_not_elements_invert_and_not_principal_exempts) {
    fixture f;
    auto r = request(user("alice"));
    r.identity.push_back(f.named("identity", R"({"Version":"2012-10-17","Statement":[
        {"Effect":"Allow","NotAction":"iam:*","NotResource":"arn:aws:s3:::secret/*"}]})"));
    BOOST_TEST((outcome(iam::evaluate(r)) == o::allow));
    r.resource = "arn:aws:s3:::secret/key";
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
    r.resource = std::string(k_object);
    r.action = "iam:ListUsers";
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));

    // Deny with NotPrincipal: the listed user (with its account) is exempt,
    // everyone else denied.
    auto n = request(user("alice"));
    n.identity.push_back(f.named("identity", k_allow_all));
    n.resource_policy = f.named("resource", R"({"Version":"2012-10-17","Statement":[
        {"Effect":"Deny","NotPrincipal":{"AWS":["arn:aws:iam::111122223333:user/alice",
         "111122223333"]},"Action":"s3:*","Resource":"*"}]})",
                                iam::policy_type::resource);
    BOOST_TEST((outcome(iam::evaluate(n)) == o::allow));
    n.who = user("bob");
    BOOST_TEST((outcome(iam::evaluate(n)) == o::explicit_deny));
}

BOOST_AUTO_TEST_CASE(inv_eval_12_principal_star_narrowed_by_principal_arn_is_not_limited) {
    fixture f;
    auto r = request(session("reader", "s1"));
    r.context.set("aws:PrincipalArn", r.who.role_arn);
    r.boundary = f.named("boundary", k_allow_ec2, iam::policy_type::boundary);
    r.session.push_back(f.named("session", k_allow_ec2, iam::policy_type::session));
    r.resource_policy = f.named("resource", R"({"Version":"2012-10-17","Statement":[
        {"Effect":"Allow","Principal":"*","Action":"s3:GetObject","Resource":"*",
         "Condition":{"ArnEquals":{"aws:PrincipalArn":"arn:aws:iam::111122223333:role/reader"}}}]})",
                                iam::policy_type::resource);
    BOOST_TEST((outcome(iam::evaluate(r)) == o::allow));
    r.context.set("aws:PrincipalArn", "arn:aws:iam::111122223333:role/writer");
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(iam_evaluation_reporting)

BOOST_AUTO_TEST_CASE(decision_names_the_deciding_statement) {
    fixture f;
    auto r = request(user("alice"));
    r.identity.push_back(f.named("arn:aws:iam::111122223333:policy/ec2", k_allow_ec2));
    r.identity.push_back(f.named("arn:aws:iam::111122223333:policy/get", k_allow_get));
    const auto d = iam::evaluate(r);
    BOOST_TEST(d.allowed());
    BOOST_REQUIRE(d.deciding.has_value());
    BOOST_TEST(d.deciding->policy == "arn:aws:iam::111122223333:policy/get");
    BOOST_TEST(d.deciding->sid == "Get");
    BOOST_TEST(d.deciding->index == 0U);
    BOOST_TEST(!d.reason.empty());
}

BOOST_AUTO_TEST_CASE(implicit_deny_has_a_reason_and_no_statement) {
    const auto d = iam::evaluate(request(user("alice")));
    BOOST_TEST(!d.allowed());
    BOOST_TEST(!d.deciding.has_value());
    BOOST_TEST(d.reason == "no identity-based or resource-based policy allows");
}

BOOST_AUTO_TEST_CASE(a_policy_without_a_document_is_ignored) {
    auto r = request(user("alice"));
    r.identity.push_back(iam::named_policy{"missing", nullptr});
    BOOST_TEST((outcome(iam::evaluate(r)) == o::implicit_deny));
}

BOOST_AUTO_TEST_SUITE_END()
