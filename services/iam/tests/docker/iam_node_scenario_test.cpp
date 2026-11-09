// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The IAM cluster in containers (.kiro/specs/iam-compatible-auth/ task 9,
// Requirements 8.1-8.5, 14.1 and 17.3). services/iam/tests/docker/compose.yml
// starts three voters, iam-1..iam-3, and a replica, iam-4, over mutual-TLS
// Raft and HTTPS, addressed by service name only. The cases run the admin
// CLI inside the containers (`compose exec -T <node> kythira-iam ...`) and
// show that, under whichever runtime the harness selects (Docker or rootless
// Podman):
//
//   * the voters elect a leader and an admin call made to a follower is
//     redirected to it;
//   * create-account writes the root key with mode 0600 once, refuses to
//     overwrite it, and the account reaches every voter;
//   * replica-add brings iam-4 in as a learner that applies the log and
//     never leads;
//   * the AWS CLI v2 (task 14), signing with the account root's key,
//     creates a user and an access key; the key authenticates, stops
//     authenticating once deactivated, and both are deleted;
//   * with the leader stopped the other two elect a new one and accept a
//     write, and the stopped node catches up when it starts again.
//
// Each node's own /healthz (`kythira-iam health`, never redirected) is the
// replication probe: an admin read would be answered by the leader.

#define BOOST_TEST_MODULE iam_node_scenario_test
#include <boost/test/results_collector.hpp>
#include <boost/test/unit_test.hpp>

#include "os_faults.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace os = docker_chaos::os;

namespace {

std::string compose_file() {
    const char* env = std::getenv("KYTHIRA_IAM_COMPOSE_FILE");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return "services/iam/tests/docker/compose.yml";
}

std::vector<std::string> compose_cmd(std::initializer_list<std::string> args) {
    auto cmd = os::compose_prefix();
    cmd.insert(cmd.end(), {"-f", compose_file()});
    cmd.insert(cmd.end(), args);
    return cmd;
}

std::string service(int n) {
    return "iam-" + std::to_string(n);
}

std::string endpoint(int n) {
    return "https://" + service(n) + ":8443";
}

/// `kythira-iam` run inside `on`, talking to `target`'s API.
os::CmdResult iam_cli(int on, int target, std::initializer_list<std::string> args,
                      int wait_secs = 30) {
    auto cmd = compose_cmd({"exec", "-T", service(on), "kythira-iam", "--endpoint",
                            endpoint(target), "--wait-secs", std::to_string(wait_secs)});
    cmd.insert(cmd.end(), args);
    return os::real_exec(cmd);
}

/// The AWS CLI v2 (compose's `aws-cli` service) against `target`, signing
/// with `key_id`/`secret`.
os::CmdResult aws_cli(int target, const std::string& key_id, const std::string& secret,
                      std::initializer_list<std::string> args) {
    auto cmd = compose_cmd({"--profile", "cli", "run", "--rm", "-T", "-e",
                            "AWS_ACCESS_KEY_ID=" + key_id, "-e", "AWS_SECRET_ACCESS_KEY=" + secret,
                            "aws-cli", "--endpoint-url", endpoint(target), "--output", "json"});
    cmd.insert(cmd.end(), args);
    return os::real_exec(cmd);
}

/// The value after `name = ` in an INI file's text.
std::string ini_value(const std::string& ini, const std::string& name) {
    const auto at = ini.find(name + " = ");
    if (at == std::string::npos) {
        return {};
    }
    const auto start = at + name.size() + 3;
    const auto end = ini.find_first_of("\r\n", start);
    return ini.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

/// The string field `key` of the first JSON object in `out`.
std::string json_field(const std::string& out, const char* outer, const char* key) {
    // Compose can print a pull or warning line around the CLI's JSON.
    const auto brace = out.find('{');
    const auto last = out.rfind('}');
    if (brace == std::string::npos || last == std::string::npos || last < brace) {
        return {};
    }
    boost::system::error_code ec;
    auto v = boost::json::parse(out.substr(brace, last - brace + 1), ec);
    if (ec || !v.is_object()) {
        return {};
    }
    const auto* o = v.as_object().if_contains(outer);
    if (o == nullptr || !o->is_object()) {
        return {};
    }
    const auto* f = o->as_object().if_contains(key);
    return (f != nullptr && f->is_string()) ? std::string(f->as_string()) : std::string{};
}

/// A shell command inside `on`.
os::CmdResult in_node(int on, const std::string& script) {
    return os::real_exec(compose_cmd({"exec", "-T", service(on), "sh", "-c", script}));
}

/// Node `n`'s own /healthz, or nullopt if it cannot be read.
std::optional<boost::json::object> health(int n) {
    auto r = iam_cli(n, n, {"health"}, 5);
    if (r.code != 0) {
        return std::nullopt;
    }
    // compose exec can interleave a warning line before the JSON.
    const auto brace = r.out.find('{');
    if (brace == std::string::npos) {
        return std::nullopt;
    }
    boost::system::error_code ec;
    auto v = boost::json::parse(r.out.substr(brace), ec);
    if (ec || !v.is_object()) {
        return std::nullopt;
    }
    return v.as_object();
}

std::string text_of(const boost::json::object& o, const char* key) {
    const auto* v = o.if_contains(key);
    return (v != nullptr && v->is_string()) ? std::string(v->as_string()) : std::string{};
}

std::int64_t int_of(const boost::json::object& o, const char* key) {
    const auto* v = o.if_contains(key);
    return (v != nullptr && v->is_int64()) ? v->as_int64() : -1;
}

/// Polls until `pred` holds or `timeout` passes.
bool eventually(const std::function<bool()>& pred, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1s);
    }
    return pred();
}

bool accounts_on(int n, std::int64_t want) {
    auto h = health(n);
    return h && int_of(*h, "accounts") == want;
}

/// The voter that nodes `among` agree leads, as its node number, or 0.
int agreed_leader(std::initializer_list<int> among) {
    int leader = 0;
    for (int n : among) {
        auto h = health(n);
        if (!h) {
            return 0;
        }
        const auto id = text_of(*h, "leader");
        if (id.empty()) {
            return 0;
        }
        const int l = std::atoi(id.c_str());
        if (l < 1 || l > 3 || (leader != 0 && l != leader)) {
            return 0;
        }
        leader = l;
    }
    return leader;
}

void dump_logs() {
    auto r = os::real_exec(compose_cmd({"logs", "--no-color", "--tail", "200"}));
    std::cerr << "---- compose logs ----\n" << r.out << "\n----------------------\n";
}

struct stack_fixture {
    stack_fixture() {
        os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
        try {
            os::checked_exec(os::real_exec, compose_cmd({"up", "-d"}));
        } catch (...) {
            // A one-shot or node that died says why only in its log.
            dump_logs();
            throw;
        }
    }
    ~stack_fixture() {
        os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
    }
    stack_fixture(const stack_fixture&) = delete;
    stack_fixture& operator=(const stack_fixture&) = delete;
};

/// Prints the stack's logs when the case it lives in has failed, by an
/// assertion or a BOOST_REQUIRE's throw, while the stack is still up.
struct logs_on_failure {
    logs_on_failure() = default;
    ~logs_on_failure() {
        const auto& results = boost::unit_test::results_collector.results(
            boost::unit_test::framework::current_test_case().p_id);
        if (!results.passed() || std::getenv("KYTHIRA_IAM_KEEP_LOGS") != nullptr) {
            dump_logs();
        }
    }
    logs_on_failure(const logs_on_failure&) = delete;
    logs_on_failure& operator=(const logs_on_failure&) = delete;
};

}  // namespace

BOOST_TEST_GLOBAL_FIXTURE(stack_fixture);

// One case on purpose: each step builds on the cluster state the last one
// left, and a fresh stack per step would triple the run for no new coverage.
BOOST_AUTO_TEST_CASE(cluster_by_service_name) {
    const logs_on_failure logs;
    // 1. A leader, and the voters agree on it.
    int leader = 0;
    BOOST_REQUIRE_MESSAGE(eventually(
                              [&] {
                                  leader = agreed_leader({1, 2, 3});
                                  return leader != 0;
                              },
                              120s),
                          "the three voters never agreed on a leader");
    const int follower = leader % 3 + 1;
    BOOST_TEST_MESSAGE("leader is " << service(leader) << ", using follower " << service(follower));

    // 2. create-account through a follower: the CLI follows the redirect.
    {
        auto r = iam_cli(follower, follower,
                         {"create-account", "--alias", "scenario-one", "--account-id",
                          "111111111111", "--root-key-file", "/tmp/root-one.ini"});
        BOOST_REQUIRE_MESSAGE(r.code == 0, "create-account failed:\n" << r.out);
        BOOST_TEST(r.out.find("111111111111") != std::string::npos);
        BOOST_TEST(r.out.find("aws_secret_access_key") != std::string::npos);

        auto mode = in_node(follower, "stat -c %a /tmp/root-one.ini");
        BOOST_TEST(mode.code == 0);
        BOOST_TEST(mode.out.find("600") == 0, "root key file mode: " << mode.out);
        auto ini = in_node(follower, "grep -c '^aws_' /tmp/root-one.ini");
        BOOST_TEST(ini.out.find('2') == 0, "root key file lines: " << ini.out);

        // A second create to the same file is refused before any request,
        // so the first secret is never lost.
        auto again = iam_cli(
            follower, follower,
            {"create-account", "--alias", "scenario-dup", "--root-key-file", "/tmp/root-one.ini"});
        BOOST_TEST(again.code != 0);
        BOOST_TEST(again.out.find("refusing to overwrite") != std::string::npos, again.out);
    }
    for (int n : {1, 2, 3}) {
        BOOST_TEST(eventually([&] { return accounts_on(n, 1); }, 60s),
                   service(n) << " never applied the account");
    }
    {
        auto r = iam_cli(follower, leader, {"list-accounts", "--json"});
        BOOST_REQUIRE_MESSAGE(r.code == 0, r.out);
        BOOST_TEST(r.out.find("scenario-one") != std::string::npos);
        BOOST_TEST(r.out.find("secret") == std::string::npos, "list-accounts leaks: " << r.out);
    }

    // 3. The replica: registered by service name, then a learner.
    {
        auto r =
            iam_cli(leader, leader,
                    {"replica-add", "--node-id", "4", "--rpc-host", "iam-4", "--rpc-port", "7100"});
        BOOST_REQUIRE_MESSAGE(r.code == 0, "replica-add failed:\n" << r.out);
    }
    BOOST_TEST(eventually([] { return accounts_on(4, 1); }, 90s),
               "the replica never applied the log");
    {
        auto h = health(4);
        BOOST_REQUIRE(h.has_value());
        BOOST_TEST(text_of(*h, "role") == "replica");
        BOOST_TEST(int_of(*h, "replicas") == 1);
    }

    // 4. The AWS CLI v2 against the leader (task 14): the account root
    // creates a user and a key for it; the key authenticates (the user has
    // no policy yet, so the call is denied, not refused as unknown); once
    // deactivated it no longer authenticates; then both are deleted.
    {
        const auto ini = in_node(follower, "cat /tmp/root-one.ini").out;
        const auto root_id = ini_value(ini, "aws_access_key_id");
        const auto root_secret = ini_value(ini, "aws_secret_access_key");
        BOOST_REQUIRE_MESSAGE(!root_id.empty() && !root_secret.empty(), "no root key in the file");

        auto user = aws_cli(leader, root_id, root_secret,
                            {"iam", "create-user", "--user-name", "cli-user"});
        BOOST_REQUIRE_MESSAGE(user.code == 0, "aws iam create-user failed:\n" << user.out);
        BOOST_TEST(json_field(user.out, "User", "Arn") ==
                   "arn:aws:iam::111111111111:user/cli-user");

        auto key = aws_cli(leader, root_id, root_secret,
                           {"iam", "create-access-key", "--user-name", "cli-user"});
        BOOST_REQUIRE_MESSAGE(key.code == 0, "aws iam create-access-key failed:\n" << key.out);
        const auto id = json_field(key.out, "AccessKey", "AccessKeyId");
        const auto secret = json_field(key.out, "AccessKey", "SecretAccessKey");
        BOOST_REQUIRE(id.starts_with("AKIA"));

        auto listed = aws_cli(leader, root_id, root_secret,
                              {"iam", "list-access-keys", "--user-name", "cli-user"});
        BOOST_TEST(listed.code == 0, listed.out);
        BOOST_TEST(listed.out.find(id) != std::string::npos);
        BOOST_TEST(listed.out.find(secret) == std::string::npos, "list-access-keys leaks");

        auto as_user = aws_cli(leader, id, secret, {"iam", "get-user"});
        BOOST_TEST(as_user.code != 0);
        BOOST_TEST(as_user.out.find("AccessDenied") != std::string::npos, as_user.out);

        auto off = aws_cli(leader, root_id, root_secret,
                           {"iam", "update-access-key", "--user-name", "cli-user",
                            "--access-key-id", id, "--status", "Inactive"});
        BOOST_TEST(off.code == 0, off.out);
        BOOST_TEST(eventually(
                       [&] {
                           auto r = aws_cli(leader, id, secret, {"iam", "get-user"});
                           return r.out.find("InvalidClientTokenId") != std::string::npos;
                       },
                       30s),
                   "the deactivated key still authenticates");

        BOOST_TEST(
            aws_cli(leader, root_id, root_secret,
                    {"iam", "delete-access-key", "--user-name", "cli-user", "--access-key-id", id})
                .code == 0);
        BOOST_TEST(
            aws_cli(leader, root_id, root_secret, {"iam", "delete-user", "--user-name", "cli-user"})
                .code == 0);
        auto gone =
            aws_cli(leader, root_id, root_secret, {"iam", "get-user", "--user-name", "cli-user"});
        BOOST_TEST(gone.out.find("NoSuchEntity") != std::string::npos, gone.out);
    }

    // 5. Stop the leader: the other two elect a new one and take a write.
    os::checked_exec(os::real_exec, compose_cmd({"stop", service(leader)}));
    std::vector<int> survivors;
    for (int n : {1, 2, 3}) {
        if (n != leader) {
            survivors.push_back(n);
        }
    }
    int next = 0;
    BOOST_REQUIRE_MESSAGE(eventually(
                              [&] {
                                  next = agreed_leader({survivors[0], survivors[1]});
                                  return next != 0 && next != leader;
                              },
                              120s),
                          "no new leader after stopping " << service(leader));
    {
        auto r = iam_cli(survivors[0], survivors[0],
                         {"create-account", "--alias", "scenario-two", "--account-id",
                          "222222222222", "--root-key-file", "/tmp/root-two.ini"},
                         60);
        BOOST_REQUIRE_MESSAGE(r.code == 0, "create-account after failover failed:\n" << r.out);
    }
    for (int n : {survivors[0], survivors[1], 4}) {
        BOOST_TEST(eventually([&] { return accounts_on(n, 2); }, 60s),
                   service(n) << " never applied the second account");
    }

    // 6. The old leader comes back as a follower and catches up. It reports
    // itself unsealed once its once-a-second check has opened the sealed
    // root keys with its passphrase, which can trail the apply by a second.
    os::checked_exec(os::real_exec, compose_cmd({"start", service(leader)}));
    BOOST_TEST(eventually([&] { return accounts_on(leader, 2); }, 120s),
               service(leader) << " never caught up after restarting");
    BOOST_TEST(eventually(
                   [&] {
                       auto h = health(leader);
                       const auto* unsealed = h ? h->if_contains("unsealed") : nullptr;
                       return unsealed != nullptr && unsealed->is_bool() && unsealed->as_bool();
                   },
                   30s),
               service(leader) << " never reported itself unsealed");
}
