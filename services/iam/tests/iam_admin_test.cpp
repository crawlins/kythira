// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The admin operations behind iam_node's /v1/admin/* routes
// (services/iam/include/kythira/iam/iam_admin.hpp,
// .kiro/specs/iam-compatible-auth/ task 9), against a bare state machine
// standing in for the Raft cluster.

#define BOOST_TEST_MODULE iam_admin_test
#include <boost/test/unit_test.hpp>

#include <kythira/iam/iam_admin.hpp>
#include <kythira/iam/iam_routing.hpp>

#include <boost/json.hpp>

#include <string>
#include <vector>

namespace {

namespace iam = kythira::iam;
namespace admin = kythira::iam::admin;

struct fake_cluster {
    iam::iam_state_machine sm;
    std::uint64_t index = 0;
    admin::kek_cache keks{"correct horse battery staple"};
    std::vector<iam::replica_record> learners;
    std::size_t submits = 0;

    auto read() -> iam::iam_state_machine {
        iam::iam_state_machine copy;
        copy.restore_from_snapshot(sm.get_state(), index);
        return copy;
    }
    auto submit(const std::vector<std::byte>& cmd) -> std::vector<std::byte> {
        ++submits;
        return sm.apply(cmd, ++index);
    }
    auto kek(std::string_view salt) -> const iam::seal_key& { return keks.get(salt); }
    void add_learner(const iam::replica_record& r) { learners.push_back(r); }
};
static_assert(admin::admin_cluster<fake_cluster>);

auto str(const boost::json::object& o, std::string_view k) -> std::string {
    return std::string(o.at(k).as_string());
}

}  // namespace

BOOST_AUTO_TEST_CASE(create_account_sets_salt_and_root_key) {
    fake_cluster c;
    auto r = admin::create_account(c, {{"alias", "team-a"}});
    BOOST_TEST_REQUIRE(r.status == 201U);
    const auto id = str(r.body, "account_id");
    BOOST_TEST(id.size() == 12U);
    BOOST_TEST(id.front() != '0');
    BOOST_TEST(str(r.body, "alias") == "team-a");
    BOOST_TEST(!c.sm.kek_salt().empty());

    // The root key is stored sealed, and opens to the secret handed out.
    const auto key_id = str(r.body, "access_key_id");
    const auto secret = str(r.body, "secret_access_key");
    BOOST_TEST(key_id.starts_with("AKIA"));
    BOOST_TEST(secret.size() == 40U);
    const auto* acct = c.sm.account(id);
    BOOST_TEST_REQUIRE(acct != nullptr);
    const auto& key = acct->access_keys.at(key_id);
    BOOST_TEST(key.owner.empty());
    BOOST_TEST(key.sealed_secret.find(secret) == std::string::npos);
    BOOST_TEST(iam::open_secret(c.kek(c.sm.kek_salt()), key.sealed_secret, key_id).value_or("") ==
               secret);

    // A second account keeps the salt.
    const auto salt = c.sm.kek_salt();
    auto r2 = admin::create_account(c, {{"account_id", "123456789012"}});
    BOOST_TEST(r2.status == 201U);
    BOOST_TEST(str(r2.body, "account_id") == "123456789012");
    BOOST_TEST(c.sm.kek_salt() == salt);
}

BOOST_AUTO_TEST_CASE(create_account_validates_and_conflicts) {
    fake_cluster c;
    BOOST_TEST(admin::create_account(c, {{"account_id", "12345"}}).status == 400U);
    BOOST_TEST(admin::create_account(c, {{"account_id", "12345678901a"}}).status == 400U);
    BOOST_TEST(admin::create_account(c, {{"alias", "Upper"}}).status == 400U);
    BOOST_TEST(admin::create_account(c, {{"alias", "-a-"}}).status == 400U);
    // INV-ACCT-2: no two hyphens in a row, and not a 12-digit number.
    BOOST_TEST(admin::create_account(c, {{"alias", "a--b"}}).status == 400U);
    BOOST_TEST(admin::create_account(c, {{"alias", "123456789012"}}).status == 400U);
    BOOST_TEST(c.submits == 0U);
    BOOST_TEST(
        admin::create_account(c, {{"account_id", "111111111111"}, {"alias", "dup"}}).status ==
        201U);
    auto again = admin::create_account(c, {{"account_id", "111111111111"}});
    BOOST_TEST(again.status == 409U);
    BOOST_TEST(str(again.body, "error") == "EntityAlreadyExists");
    auto alias = admin::create_account(c, {{"alias", "dup"}});
    BOOST_TEST(alias.status == 409U);
}

BOOST_AUTO_TEST_CASE(list_accounts_carries_no_secrets) {
    fake_cluster c;
    auto made = admin::create_account(c, {{"alias", "listed"}});
    const auto secret = str(made.body, "secret_access_key");
    auto r = admin::list_accounts(c);
    BOOST_TEST(r.status == 200U);
    const auto& accounts = r.body.at("accounts").as_array();
    BOOST_TEST_REQUIRE(accounts.size() == 1U);
    const auto& a = accounts[0].as_object();
    BOOST_TEST(str(a, "alias") == "listed");
    BOOST_TEST(a.at("root_access_keys").as_int64() == 1);
    const auto text = boost::json::serialize(r.body);
    BOOST_TEST(text.find(secret) == std::string::npos);
    BOOST_TEST(text.find("sealed") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(rotate_sts_key_needs_an_account_then_retires_the_old_key) {
    fake_cluster c;
    auto early = admin::rotate_sts_key(c, {});
    BOOST_TEST(early.status == 409U);
    BOOST_TEST(str(early.body, "error") == "NotUnsealed");

    (void)admin::create_account(c, {});
    auto first = admin::rotate_sts_key(c, {});
    BOOST_TEST_REQUIRE(first.status == 200U);
    BOOST_TEST(first.body.at("retiring").as_array().empty());
    const auto kid1 = str(first.body, "kid");

    auto second = admin::rotate_sts_key(c, {{"retire_after_seconds", 600}});
    BOOST_TEST_REQUIRE(second.status == 200U);
    const auto& retiring = second.body.at("retiring").as_array();
    BOOST_TEST_REQUIRE(retiring.size() == 1U);
    BOOST_TEST(str(retiring[0].as_object(), "kid") == kid1);
    BOOST_TEST_REQUIRE(c.sm.sts_keys().size() == 2U);
    for (const auto& k : c.sm.sts_keys()) {
        BOOST_TEST(iam::open_secret(c.kek(c.sm.kek_salt()), k.sealed_key, k.kid).has_value());
        BOOST_TEST(k.retire_after.has_value() == (k.kid == kid1));
    }
    // A third rotation retires only the newest active key.
    auto third = admin::rotate_sts_key(c, {});
    BOOST_TEST(third.body.at("retiring").as_array().size() == 1U);

    BOOST_TEST(admin::rotate_sts_key(c, {{"retire_after_seconds", -1}}).status == 400U);
    BOOST_TEST(admin::rotate_sts_key(c, {{"retire_after_seconds", "soon"}}).status == 400U);
}

BOOST_AUTO_TEST_CASE(add_replica_registers_then_adds_the_learner) {
    fake_cluster c;
    BOOST_TEST(admin::add_replica(c, {{"node_id", "4"}, {"rpc_host", "h"}}).status == 400U);
    BOOST_TEST(
        admin::add_replica(c, {{"node_id", "4"}, {"rpc_host", "h"}, {"rpc_port", 70000}}).status ==
        400U);
    BOOST_TEST(c.learners.empty());
    auto r =
        admin::add_replica(c, {{"node_id", "4"}, {"rpc_host", "replica-4"}, {"rpc_port", 7004}});
    BOOST_TEST(r.status == 200U);
    BOOST_TEST_REQUIRE(c.learners.size() == 1U);
    BOOST_TEST(c.learners[0].rpc_host == "replica-4");
    BOOST_TEST(c.sm.replicas().contains("4"));
}

BOOST_AUTO_TEST_CASE(kek_check_catches_a_wrong_passphrase) {
    fake_cluster c;
    {
        admin::kek_cache any("anything");
        // Nothing sealed yet: any KEK is accepted.
        iam::iam_state_machine empty;
        BOOST_TEST(admin::kek_opens(empty, any.get(iam::generate_kek_salt())));
    }
    (void)admin::create_account(c, {});
    const auto salt = c.sm.kek_salt();
    BOOST_TEST(admin::kek_opens(c.sm, c.kek(salt)));
    admin::kek_cache wrong("not the passphrase");
    BOOST_TEST(!admin::kek_opens(c.sm, wrong.get(salt)));
    BOOST_CHECK_THROW(admin::kek_cache(""), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(state_machine_copies_and_moves) {
    fake_cluster c;
    (void)admin::create_account(c, {{"alias", "copied"}});
    iam::iam_state_machine copy(c.sm);
    BOOST_TEST(copy.accounts().size() == 1U);
    iam::iam_state_machine moved(std::move(copy));
    BOOST_TEST(moved.accounts().size() == 1U);
    BOOST_TEST(moved.kek_salt() == c.sm.kek_salt());
    iam::iam_state_machine assigned;
    assigned = moved;
    BOOST_TEST(assigned.get_state() == c.sm.get_state());
}

// ── routing ────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(routes_by_path_then_credential_scope) {
    namespace r = kythira::iam::routing;
    const std::string header =
        "AWS4-HMAC-SHA256 Credential=AKIAX/20261106/us-east-1/iam/aws4_request, "
        "SignedHeaders=host, Signature=ab";
    BOOST_TEST(r::scope_service(header, "").value_or("") == "iam");
    BOOST_TEST(r::scope_service("AWS4-HMAC-SHA256 Credential=AKIAX/20261106/eu-west-1/sts/"
                                "aws4_request,SignedHeaders=host,Signature=ab",
                                "")
                   .value_or("") == "sts");
    BOOST_TEST(r::scope_service("",
                                "Action=GetCallerIdentity&X-Amz-Credential=AKIAX%2F20261106%2F"
                                "us-east-1%2Fsts%2Faws4_request&X-Amz-Date=x")
                   .value_or("") == "sts");
    BOOST_TEST(!r::scope_service("", "Action=ListUsers").has_value());
    BOOST_TEST(!r::scope_service("Bearer abc", "").has_value());
    BOOST_TEST(!r::scope_service("AWS4-HMAC-SHA256 Credential=AKIAX/x/iam", "").has_value());
    BOOST_TEST(
        !r::scope_service("AWS4-HMAC-SHA256 Credential=AKIAX/d/r//aws4_request", "").has_value());

    BOOST_TEST((r::route_of("/healthz", header, "") == r::route::health));
    BOOST_TEST((r::route_of("/v1/admin/accounts", "", "") == r::route::admin));
    BOOST_TEST((r::route_of("/", header, "") == r::route::iam));
    BOOST_TEST((r::route_of("/", "", "") == r::route::unauthenticated));
    BOOST_TEST((r::route_of("/", "AWS4-HMAC-SHA256 Credential=K/d/r/s3/aws4_request,", "") ==
                r::route::unknown_service));
}

BOOST_AUTO_TEST_CASE(admin_needs_a_verified_listed_san) {
    namespace r = kythira::iam::routing;
    const std::set<std::string> admins{"spiffe://k/admin"};
    BOOST_TEST(r::admin_allowed(true, {"spiffe://k/other", "spiffe://k/admin"}, admins));
    BOOST_TEST(!r::admin_allowed(false, {"spiffe://k/admin"}, admins));
    BOOST_TEST(!r::admin_allowed(true, {"spiffe://k/other"}, admins));
    BOOST_TEST(!r::admin_allowed(true, {"spiffe://k/admin"}, {}));
}

BOOST_AUTO_TEST_CASE(query_error_document) {
    namespace r = kythira::iam::routing;
    const auto id = r::request_id();
    BOOST_TEST(id.size() == 36U);
    BOOST_TEST(id[8] == '-');
    BOOST_TEST(id != r::request_id());
    const auto xml = r::query_error_xml(false, true, "NotImplemented", "a <b> & 'c'", "rid");
    BOOST_TEST(xml ==
               "<ErrorResponse xmlns=\"https://iam.amazonaws.com/doc/2010-05-08/\"><Error><Type>"
               "Sender</Type><Code>NotImplemented</Code><Message>a &lt;b&gt; &amp; &apos;c&apos;"
               "</Message></Error><RequestId>rid</RequestId></ErrorResponse>");
    BOOST_TEST(r::query_error_xml(true, false, "X", "m", "r")
                   .find("https://sts.amazonaws.com/doc/2011-06-15/") != std::string::npos);
    BOOST_TEST(r::query_error_xml(true, false, "X", "m", "r").find("<Type>Receiver</Type>") !=
               std::string::npos);
}
