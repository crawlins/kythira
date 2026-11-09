// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// iam_node and kythira-iam as processes (.kiro/specs/iam-compatible-auth/
// task 9): a three-node cluster over mutual-TLS Raft RPC with an HTTPS
// listener, driven by the CLI with an admin certificate.
//
//  - create-account through a follower (the 307 to the leader is followed),
//    the root key file at mode 0600, list-accounts from another member;
//  - the admin routes refuse no certificate and a non-admin one;
//  - IAM and STS requests route by credential scope to the leader,
//    unsigned ones are refused;
//  - rotate-sts-key retires the previous key;
//  - a read replica joins as a learner and applies the log, reports its
//    lag in /healthz and /metrics, and with every voter frozen answers the
//    signed routes ServiceUnavailable until they resume (task 13);
//  - the leader is stopped, a new one serves writes, and the old one
//    catches up on restart;
//  - a member restarted with the wrong unseal passphrase exits.

#define BOOST_TEST_MODULE iam_node_test
#include <boost/test/results_collector.hpp>
#include <boost/test/unit_test.hpp>

#include "iam_node_test_support.hpp"

using namespace iam_node_testing;  // NOLINT(google-build-using-namespace)
using namespace std::chrono_literals;

BOOST_AUTO_TEST_CASE(cluster_lifecycle, *boost::unit_test::timeout(600)) {
    cluster c;
    for (auto* m : c.voters()) {
        c.start(*m);
    }
    auto* leader = c.wait_leader();
    if (leader == nullptr) {
        c.dump_logs();
    }
    BOOST_REQUIRE(leader != nullptr);

    // A follower redirects an admin request to the leader, once it has
    // heard from it (503 until then). Leadership can move between finding
    // the leader and asking a follower — a starved runner misses heartbeats
    // and an election follows — so each attempt re-resolves the leader and
    // only a 307 naming the leader of that moment counts.
    member* follower = nullptr;
    std::optional<http_result> redirect;
    std::string expected_location;
    const auto redirect_deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < redirect_deadline) {
        leader = c.wait_leader();
        BOOST_REQUIRE(leader != nullptr);
        follower = nullptr;
        for (auto* m : c.voters()) {
            if (m != leader) {
                follower = m;
                break;
            }
        }
        BOOST_REQUIRE(follower != nullptr);
        expected_location =
            "https://localhost:" + std::to_string(leader->http_port) + "/v1/admin/accounts";
        redirect = https_call(follower->http_port, c.pki, http::verb::get, "/v1/admin/accounts",
                              c.pki.admin_cert.string(), c.pki.admin_key.string());
        if (redirect && redirect->status == 307U && redirect->location == expected_location) {
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    BOOST_REQUIRE(redirect.has_value());
    BOOST_TEST(redirect->status == 307U);
    BOOST_TEST(redirect->location == expected_location);

    // create-account through the follower: the CLI follows the redirect.
    const auto key_file = c.dir() / "root-key.ini";
    auto [rc, out] = c.cli(*follower, {"create-account", "--alias", "first-account",
                                       "--root-key-file", key_file.string()});
    BOOST_TEST_INFO("kythira-iam output: " << out);
    BOOST_REQUIRE(rc == 0);
    BOOST_TEST(out.find("aws_access_key_id:     AKIA") != std::string::npos);
    struct stat st{};
    BOOST_REQUIRE(::stat(key_file.c_str(), &st) == 0);
    BOOST_TEST((st.st_mode & 0777) == 0600U);
    const auto ini = slurp(key_file);
    BOOST_TEST(ini.find("aws_secret_access_key = ") != std::string::npos);
    // The key file is never overwritten.
    auto [rc_again, out_again] =
        c.cli(*leader, {"create-account", "--root-key-file", key_file.string()});
    BOOST_TEST(rc_again == 1);
    BOOST_TEST(out_again.find("refusing to overwrite") != std::string::npos);

    // list-accounts from another member, once it has applied the entry.
    for (auto* m : c.voters()) {
        BOOST_TEST(c.wait_accounts(*m, 1));
    }
    auto [rc_list, listed] = c.cli(*follower, {"list-accounts", "--json"});
    BOOST_REQUIRE(rc_list == 0);
    const auto accounts = json_of(listed).at("accounts").as_array();
    BOOST_REQUIRE(accounts.size() == 1U);
    BOOST_TEST(std::string(accounts[0].as_object().at("alias").as_string()) == "first-account");
    BOOST_TEST(listed.find("secret") == std::string::npos);

    // Admin routes need an admin certificate.
    auto none = https_call(leader->http_port, c.pki, http::verb::get, "/v1/admin/accounts");
    BOOST_REQUIRE(none.has_value());
    BOOST_TEST(none->status == 403U);
    auto outsider = https_call(leader->http_port, c.pki, http::verb::get, "/v1/admin/accounts",
                               c.pki.outsider_cert.string(), c.pki.outsider_key.string());
    BOOST_REQUIRE(outsider.has_value());
    BOOST_TEST(outsider->status == 403U);

    // AWS routing by credential scope.
    const std::string iam_auth =
        "AWS4-HMAC-SHA256 Credential=AKIAEXAMPLEKEYID0000/20261106/us-east-1/iam/aws4_request, "
        "SignedHeaders=host;x-amz-date, Signature=00";
    // IAM is served by the leader; a follower redirects.
    auto iam_call = https_call(follower->http_port, c.pki, http::verb::post, "/", {}, {}, iam_auth);
    BOOST_REQUIRE(iam_call.has_value());
    BOOST_TEST(iam_call->status == 307U);
    // A signature with no X-Amz-Date is refused in AWS's Query error shape.
    auto bad_key = https_call(leader->http_port, c.pki, http::verb::post, "/", {}, {}, iam_auth);
    BOOST_REQUIRE(bad_key.has_value());
    BOOST_TEST(bad_key->status == 400U);
    BOOST_TEST(bad_key->body.find("<Code>IncompleteSignature</Code>") != std::string::npos);
    BOOST_TEST(bad_key->body.find("https://iam.amazonaws.com/doc/2010-05-08/") !=
               std::string::npos);
    // Task 14: the account root's key, signed end to end, creates and lists
    // a user.
    const auto root_id = ini_value(ini, "aws_access_key_id");
    const auto root_secret = ini_value(ini, "aws_secret_access_key");
    auto created = signed_iam_call(leader->http_port, c.pki, root_id, root_secret,
                                   "Action=CreateUser&UserName=node-test&Version=2010-05-08");
    BOOST_REQUIRE(created.has_value());
    BOOST_TEST_INFO("CreateUser: " << created->body);
    BOOST_TEST(created->status == 200U);
    BOOST_TEST(created->body.find("<UserName>node-test</UserName>") != std::string::npos);
    auto listed_users = signed_iam_call(leader->http_port, c.pki, root_id, root_secret,
                                        "Action=ListUsers&Version=2010-05-08");
    BOOST_REQUIRE(listed_users.has_value());
    BOOST_TEST(listed_users->status == 200U);
    BOOST_TEST(listed_users->body.find("node-test") != std::string::npos);
    auto wrong_secret = signed_iam_call(leader->http_port, c.pki, root_id, "not-the-secret",
                                        "Action=ListUsers&Version=2010-05-08");
    BOOST_REQUIRE(wrong_secret.has_value());
    BOOST_TEST(wrong_secret->status == 403U);
    BOOST_TEST(wrong_secret->body.find("SignatureDoesNotMatch") != std::string::npos);
    auto sts_call = https_call(follower->http_port, c.pki, http::verb::get,
                               "/?Action=GetCallerIdentity&X-Amz-Credential=AKIAEXAMPLEKEYID0000%"
                               "2F20261106%2Fus-east-1%2Fsts%2Faws4_request");
    // STS, served since task 17, goes to the leader like IAM; there an
    // unverifiable signature is refused in STS's own Query namespace.
    BOOST_REQUIRE(sts_call.has_value());
    BOOST_TEST(sts_call->status == 307U);
    auto sts_on_leader =
        https_call(leader->http_port, c.pki, http::verb::get,
                   "/?Action=GetCallerIdentity&X-Amz-Credential=AKIAEXAMPLEKEYID0000%"
                   "2F20261106%2Fus-east-1%2Fsts%2Faws4_request");
    BOOST_REQUIRE(sts_on_leader.has_value());
    BOOST_TEST_INFO("STS on the leader: " << sts_on_leader->body);
    BOOST_TEST(sts_on_leader->status >= 400U);
    BOOST_TEST(sts_on_leader->body.find("https://sts.amazonaws.com/doc/2011-06-15/") !=
               std::string::npos);
    auto unsigned_call =
        https_call(follower->http_port, c.pki, http::verb::get, "/?Action=ListUsers");
    BOOST_REQUIRE(unsigned_call.has_value());
    BOOST_TEST(unsigned_call->status == 403U);
    BOOST_TEST(unsigned_call->body.find("MissingAuthenticationToken") != std::string::npos);

    // STS key rotation.
    auto [rc_rot1, rot1] = c.cli(*leader, {"rotate-sts-key"});
    BOOST_TEST_INFO("rotate: " << rot1);
    BOOST_TEST(rc_rot1 == 0);
    auto [rc_rot2, rot2] = c.cli(*leader, {"rotate-sts-key", "--retire-after-secs", "60"});
    BOOST_TEST(rc_rot2 == 0);
    BOOST_TEST(rot2.find("retiring:") != std::string::npos);

    // A read replica joins as a learner and applies the log.
    auto& replica = c.members[3];
    c.start(replica);
    auto [rc_rep, rep] =
        c.cli(*follower, {"replica-add", "--node-id", "4", "--rpc-host", "127.0.0.1", "--rpc-port",
                          std::to_string(replica.rpc_port)});
    BOOST_TEST_INFO("replica-add: " << rep);
    BOOST_TEST(rc_rep == 0);
    BOOST_TEST(c.wait_accounts(replica, 1));
    auto rh = c.health(replica);
    BOOST_REQUIRE(rh.has_value());
    BOOST_TEST(std::string(rh->at("role").as_string()) == "replica");
    // Admin writes on a replica go to the leader too.
    auto rep_redirect = https_call(replica.http_port, c.pki, http::verb::get, "/v1/admin/accounts",
                                   c.pki.admin_cert.string(), c.pki.admin_key.string());
    BOOST_REQUIRE(rep_redirect.has_value());
    BOOST_TEST(rep_redirect->status == 307U);

    // Requirement 7.4: a caught-up replica reports a small lag, exports it
    // as a metric, and passes signed routes on to the leader.
    BOOST_TEST(rh->at("fresh").as_bool());
    BOOST_TEST(rh->at("lag_seconds").to_number<double>() < 3.0);
    auto metrics = https_call(replica.http_port, c.pki, http::verb::get, "/metrics");
    BOOST_REQUIRE(metrics.has_value());
    BOOST_TEST(metrics->status == 200U);
    BOOST_TEST(metrics->body.find("kythira_iam_replica_lag_seconds ") != std::string::npos);
    auto fresh_call = https_call(replica.http_port, c.pki, http::verb::post, "/", {}, {}, iam_auth);
    BOOST_REQUIRE(fresh_call.has_value());
    BOOST_TEST(fresh_call->status == 307U);  // fresh: on to the leader

    // With every voter frozen (a network that delivers nothing), the
    // replica fails closed once past its 3 s bound, and recovers after.
    for (auto* v : c.voters()) {
        ::kill(v->pid, SIGSTOP);
    }
    bool went_stale = false;
    for (const auto deadline = std::chrono::steady_clock::now() + 30s;
         !went_stale && std::chrono::steady_clock::now() < deadline;) {
        auto h = c.health(replica);
        went_stale = h && !h->at("fresh").as_bool();
        std::this_thread::sleep_for(200ms);
    }
    BOOST_TEST(went_stale);
    auto stale_call = https_call(replica.http_port, c.pki, http::verb::post, "/", {}, {}, iam_auth);
    for (auto* v : c.voters()) {
        ::kill(v->pid, SIGCONT);
    }
    BOOST_REQUIRE(stale_call.has_value());
    BOOST_TEST(stale_call->status == 503U);
    BOOST_TEST(stale_call->body.find("<Code>ServiceUnavailable</Code>") != std::string::npos);
    bool recovered = false;
    for (const auto deadline = std::chrono::steady_clock::now() + 60s;
         !recovered && std::chrono::steady_clock::now() < deadline;) {
        auto h = c.health(replica);
        recovered = h && h->at("fresh").as_bool();
        std::this_thread::sleep_for(200ms);
    }
    BOOST_TEST(recovered);
    leader = c.wait_leader();
    BOOST_REQUIRE(leader != nullptr);

    // Stop the leader: another voter takes over and serves writes, and the
    // replica keeps applying through the new leader.
    cluster::stop(*leader);
    auto* next = c.wait_leader();
    BOOST_REQUIRE(next != nullptr);
    BOOST_TEST(next != leader);
    auto [rc2, out2] = c.cli(*next, {"create-account", "--account-id", "222222222222",
                                     "--root-key-file", (c.dir() / "second.ini").string()});
    BOOST_TEST_INFO("second create-account: " << out2);
    BOOST_TEST(rc2 == 0);
    BOOST_TEST(c.wait_accounts(replica, 2));
    c.start(*leader);
    BOOST_TEST(c.wait_accounts(*leader, 2));

    // A member restarted with the wrong passphrase refuses to run once it
    // sees the sealed secrets.
    member* victim = nullptr;
    for (auto* m : c.voters()) {
        if (m != next) {
            victim = m;
            break;
        }
    }
    cluster::stop(*victim);
    c.start(*victim, c.dir() / "wrong-unseal.txt");
    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (::waitpid(victim->pid, &status, WNOHANG) == victim->pid) {
            exited = true;
            victim->pid = -1;
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    BOOST_TEST(exited);
    BOOST_TEST((exited && WIFEXITED(status) && WEXITSTATUS(status) == 1));
    BOOST_TEST(slurp(victim->dir / "node.log").find("wrong --unseal-key-file") !=
               std::string::npos);
    if (!boost::unit_test::results_collector
             .results(boost::unit_test::framework::current_test_case().p_id)
             .passed()) {
        c.dump_logs();
    }
}

BOOST_AUTO_TEST_CASE(refuses_unsafe_configurations) {
    auto dir = raft::make_private_temp_directory(fs::temp_directory_path(), "iam-node-flags-");
    raft::write_private_file(dir / "unseal.txt", "x\n");
    const std::string unseal = (dir / "unseal.txt").string();
    const std::vector<std::string> base{
        IAM_NODE_PATH, "--node-id",  "1",       "--unseal-key-file",        unseal,
        "--data-dir",  dir.string(), "--peers", "2:127.0.0.1:1@https://h:1"};
    auto with = [&](std::vector<std::string> extra) {
        auto args = base;
        args.insert(args.end(), extra.begin(), extra.end());
        return run_capture(args, 20s);
    };
    // No TLS and no --insecure-plaintext.
    auto [rc1, out1] = with({"--rpc-address", "127.0.0.1"});
    BOOST_TEST(rc1 == 1);
    BOOST_TEST(out1.find("--tls-cert and --tls-key are required") != std::string::npos);
    // Plaintext Raft RPC off loopback without the flag.
    auto [rc2, out2] = with({"--insecure-plaintext", "--rpc-address", "0.0.0.0"});
    BOOST_TEST(rc2 == 1);
    BOOST_TEST(out2.find("Raft RPC needs") != std::string::npos);
    // Admin SANs without TLS.
    auto [rc3, out3] = with({"--insecure-plaintext", "--rpc-address", "127.0.0.1",
                             "--admin-uri-san", "spiffe://x/admin"});
    BOOST_TEST(rc3 == 1);
    BOOST_TEST(out3.find("--admin-uri-san needs TLS") != std::string::npos);
    // An RPC certificate without this node's reserved SAN.
    kythira::iam::testing::test_pki_options o;
    o.dir = dir;
    o.nodes = {make_id(7)};
    auto pki = kythira::iam::testing::write_test_pki(o);
    auto [rc4, out4] = with({"--tls-cert", pki.node_cert(make_id(7)).string(), "--tls-key",
                             pki.node_key(make_id(7)).string(), "--rpc-tls-cert",
                             pki.node_cert(make_id(7)).string(), "--rpc-tls-key",
                             pki.node_key(make_id(7)).string(), "--rpc-tls-ca", pki.ca.string()});
    BOOST_TEST(rc4 == 1);
    BOOST_TEST(out4.find("reserved peer SAN") != std::string::npos);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
