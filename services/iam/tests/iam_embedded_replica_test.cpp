// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The IAM side of the Redis gateway's AUTH, end to end
// (.kiro/specs/iam-compatible-auth/ task 21, Requirements 7.3 and 13.6):
// three iam_node members as processes, and in this process the replica a
// front end embeds (iam_embedded_replica.hpp) with iam_redis_verifier on
// top of it.
//
//  - the replica joins over mutual-TLS RPC once `kythira-iam replica-add`
//    registers it, and applies the log;
//  - an access key and a policy made through the IAM Query API reach it,
//    and an ElastiCache token signed with that key is accepted with no
//    network call per check;
//  - deleting the policy reaches it too, and the same token is refused;
//  - a stopped replica fails closed once past its staleness bound.

#define BOOST_TEST_MODULE iam_embedded_replica_test
#include <boost/test/unit_test.hpp>

#include "iam_node_test_support.hpp"
#include "iam_redis_token_test_support.hpp"

#include <kythira/iam/iam_embedded_replica.hpp>
#include <kythira/iam/iam_redis_verifier.hpp>
#include <kythira/iam/services/elasticache_actions.hpp>

using namespace iam_node_testing;  // NOLINT(google-build-using-namespace)
using namespace std::chrono_literals;

namespace {

auto enc(std::string_view s) -> std::string {
    return kythira::sigv4::detail::uri_encode(s, false);
}

auto element(const std::string& xml, const std::string& name) -> std::string {
    const auto open = "<" + name + ">";
    const auto at = xml.find(open);
    if (at == std::string::npos) {
        return {};
    }
    const auto start = at + open.size();
    return xml.substr(start, xml.find("</" + name + ">", start) - start);
}

auto now_seconds() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

BOOST_AUTO_TEST_CASE(an_embedded_replica_checks_redis_tokens_locally,
                     *boost::unit_test::timeout(400)) {
    constexpr int k_replica = 5;
    cluster c("Kythira Test Root CA", {k_replica});
    for (auto* m : c.voters()) {
        c.start(*m);
    }
    auto* leader = c.wait_leader();
    if (leader == nullptr) {
        c.dump_logs();
    }
    BOOST_REQUIRE(leader != nullptr);

    const auto key_file = c.dir() / "root-key.ini";
    auto [rc, out] = c.cli(*leader, {"create-account", "--root-key-file", key_file.string()});
    BOOST_TEST_INFO("kythira-iam output: " << out);
    BOOST_REQUIRE(rc == 0);
    const auto ini = slurp(key_file);
    const auto root_id = ini_value(ini, "aws_access_key_id");
    const auto root_secret = ini_value(ini, "aws_secret_access_key");
    const auto lead = [&]() -> member& {
        auto* l = c.wait_leader();
        BOOST_REQUIRE(l != nullptr);
        return *l;
    };
    const auto root_call = [&](const std::string& body, const std::string& service = "iam") {
        auto r = signed_call(lead().http_port, c.pki, root_id, root_secret, body, service);
        BOOST_REQUIRE(r.has_value());
        BOOST_TEST_INFO("root call " << body << ": " << r->body);
        BOOST_TEST_REQUIRE(r->status == 200U);
        return r->body;
    };
    const auto account =
        element(root_call("Action=GetCallerIdentity&Version=2011-06-15", "sts"), "Account");
    BOOST_REQUIRE_EQUAL(account.size(), 12U);
    (void)root_call("Action=CreateUser&Version=2010-05-08&UserName=redis-client");
    const auto key_xml =
        root_call("Action=CreateAccessKey&Version=2010-05-08&UserName=redis-client");
    const auto key_id = element(key_xml, "AccessKeyId");
    const auto secret = element(key_xml, "SecretAccessKey");
    BOOST_REQUIRE(!key_id.empty() && !secret.empty());
    const std::string policy =
        R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"elasticache:Connect",)"
        R"("Resource":["arn:aws:elasticache:us-east-1:)" +
        account + R"(:replicationgroup:kythira-cache","arn:aws:elasticache:us-east-1:)" + account +
        R"(:user:farm"]}]})";
    (void)root_call(
        "Action=PutUserPolicy&Version=2010-05-08&UserName=redis-client&PolicyName=redis"
        "&PolicyDocument=" +
        enc(policy));

    // The replica, as redis_gateway_node's composition root builds it.
    kythira::iam::embedded_replica_config cfg;
    cfg.node_id = make_id(k_replica);
    cfg.rpc_address = "127.0.0.1";
    const auto replica_port = kythira::testing::free_port();
    cfg.rpc_port = replica_port;
    for (auto* v : c.voters()) {
        cfg.voters.push_back({v->id, "127.0.0.1", v->rpc_port});
    }
    cfg.data_dir = c.dir() / "embedded-replica";
    cfg.unseal_key_file = c.pki.unseal.string();
    cfg.rpc_tls_cert = c.pki.node_cert(make_id(k_replica)).string();
    cfg.rpc_tls_key = c.pki.node_key(make_id(k_replica)).string();
    cfg.rpc_tls_ca = c.pki.ca.string();
    cfg.max_staleness = 3s;
    kythira::iam::embedded_replica replica(std::move(cfg),
                                           [](kythira::iam::iam_service_registry& r) {
                                               r.add(kythira::iam::services::elasticache_actions());
                                           });
    replica.start();
    auto [rc_add, added] =
        c.cli(lead(), {"replica-add", "--node-id", std::to_string(k_replica), "--rpc-host",
                       "127.0.0.1", "--rpc-port", std::to_string(replica_port)});
    BOOST_TEST_INFO("replica-add: " << added);
    BOOST_REQUIRE(rc_add == 0);

    kythira::iam::iam_redis_verifier verifier(
        replica.authorizer(),
        {.account = account, .region = "us-east-1", .cache_name = "kythira-cache"});
    const auto check = [&] {
        const auto token = iam_redis_testing::presign(key_id, secret, now_seconds());
        return verifier.check({"redis", "farm", token, "127.0.0.1"});
    };
    const auto reason = [](const kythira::credential_result& r) {
        const auto* d = std::get_if<kythira::credential_denied>(&r);
        return d == nullptr ? std::string() : d->reason;
    };
    const auto eventually = [&](const std::function<bool()>& ok, std::chrono::seconds within) {
        const auto deadline = std::chrono::steady_clock::now() + within;
        while (std::chrono::steady_clock::now() < deadline) {
            if (ok()) {
                return true;
            }
            std::this_thread::sleep_for(200ms);
        }
        return ok();
    };

    // The key and the policy reach the replica.
    kythira::credential_result last = kythira::credential_denied{"not checked"};
    const bool accepted = eventually(
        [&] {
            last = check();
            return std::holds_alternative<kythira::credential_accepted>(last);
        },
        60s);
    if (!accepted) {
        c.dump_logs();
    }
    BOOST_TEST_REQUIRE(accepted, "last refusal: " << reason(last));
    BOOST_TEST(std::get<kythira::credential_accepted>(last).principal ==
               "arn:aws:iam::" + account + ":user/redis-client");

    // So does the policy's deletion.
    (void)root_call(
        "Action=DeleteUserPolicy&Version=2010-05-08&UserName=redis-client"
        "&PolicyName=redis");
    BOOST_TEST(eventually([&] { return !reason(check()).empty(); }, 30s));
    BOOST_TEST(reason(check()).find("elasticache:Connect denied") != std::string::npos);

    // A replica cut off from the voters fails closed past its bound.
    (void)root_call(
        "Action=PutUserPolicy&Version=2010-05-08&UserName=redis-client&PolicyName=redis"
        "&PolicyDocument=" +
        enc(policy));
    BOOST_TEST(eventually([&] { return reason(check()).empty(); }, 30s));
    for (auto* v : c.voters()) {
        cluster::stop(*v);
    }
    BOOST_TEST(
        eventually([&] { return reason(check()).find("Unavailable") != std::string::npos; }, 20s),
        "last: " << reason(check()));
    replica.stop();
}
