// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The IAM conformance job (.kiro/specs/iam-compatible-auth/ task 23,
// Requirements 10.11, 15.1, 15.3 and 15.4). services/iam/tests/docker/
// conformance.yml starts three IAM voters, the Kythira CA as an OIDC issuer,
// a test OIDC issuer and two Redis gateways with embedded IAM replicas. This
// harness:
//
//   1. waits for the voters to agree on a leader;
//   2. creates the two fixed accounts (111111111111 and 222222222222),
//      reads their root keys and rotates in the first STS key;
//   3. registers the gateways' replicas (ids 6 and 7);
//   4. runs conformance/driver.py (boto3, pinned) against the leader, which
//      prints one PASS or FAIL line per check, and fails the case for each
//      FAIL line;
//   5. hands the AWS CLI v2 (pinned) the config, credentials and web
//      identity token the driver left in the shared volume, and checks its
//      own credential providers resolve a two-role chain, a
//      web_identity_token_file profile and the AWS_ROLE_ARN /
//      AWS_WEB_IDENTITY_TOKEN_FILE pair against Kythira's STS.
//
// KYTHIRA_IAM_CONFORMANCE_NO_REDIS=1 leaves the gateways out (a local run
// on a host that cannot build redis_gateway_node); the driver then skips its
// Redis checks. CI always runs the whole stack.

#define BOOST_TEST_MODULE iam_conformance_test
#include <boost/test/results_collector.hpp>
#include <boost/test/unit_test.hpp>

#include "os_faults.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace os = docker_chaos::os;

namespace {

std::string compose_file() {
    const char* env = std::getenv("KYTHIRA_IAM_CONFORMANCE_COMPOSE_FILE");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return "services/iam/tests/docker/conformance.yml";
}

bool with_redis() {
    const char* env = std::getenv("KYTHIRA_IAM_CONFORMANCE_NO_REDIS");
    return env == nullptr || *env == 0 || std::string(env) == "0";
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

os::CmdResult iam_cli(int on, int target, std::initializer_list<std::string> args,
                      int wait_secs = 30) {
    auto cmd = compose_cmd({"exec", "-T", service(on), "kythira-iam", "--endpoint",
                            endpoint(target), "--wait-secs", std::to_string(wait_secs)});
    cmd.insert(cmd.end(), args);
    return os::real_exec(cmd);
}

os::CmdResult in_node(int on, const std::string& script) {
    return os::real_exec(compose_cmd({"exec", "-T", service(on), "sh", "-c", script}));
}

std::string ini_value(const std::string& ini, const std::string& name) {
    const auto at = ini.find(name + " = ");
    if (at == std::string::npos) {
        return {};
    }
    const auto start = at + name.size() + 3;
    const auto end = ini.find_first_of("\r\n", start);
    return ini.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::optional<boost::json::object> health(int n) {
    auto r = iam_cli(n, n, {"health"}, 5);
    if (r.code != 0) {
        return std::nullopt;
    }
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

int agreed_leader() {
    int leader = 0;
    for (int n : {1, 2, 3}) {
        auto h = health(n);
        if (!h) {
            return 0;
        }
        const int l = std::atoi(text_of(*h, "leader").c_str());
        if (l < 1 || l > 3 || (leader != 0 && l != leader)) {
            return 0;
        }
        leader = l;
    }
    return leader;
}

/// The account root key create-account writes into `on`'s /tmp.
struct root_key {
    std::string id;
    std::string secret;
};

root_key create_account(int on, int leader, const std::string& account, const std::string& alias) {
    const auto file = "/tmp/root-" + account + ".ini";
    auto r = iam_cli(
        on, leader,
        {"create-account", "--alias", alias, "--account-id", account, "--root-key-file", file});
    BOOST_REQUIRE_MESSAGE(r.code == 0, "create-account " << account << " failed:\n" << r.out);
    const auto ini = in_node(on, "cat " + file).out;
    root_key k{ini_value(ini, "aws_access_key_id"), ini_value(ini, "aws_secret_access_key")};
    BOOST_REQUIRE_MESSAGE(!k.id.empty() && !k.secret.empty(), "no root key for " << account);
    return k;
}

/// The AWS CLI v2's GetCallerIdentity Arn under `env`, which configures
/// its credentials the way a user's shell would; STS's endpoint is in the
/// standard AWS_ENDPOINT_URL_STS.
os::CmdResult cli_identity(int leader, const std::vector<std::string>& env) {
    auto cmd = compose_cmd({"--profile", "cli", "run", "--rm", "-T", "-e",
                            "AWS_ENDPOINT_URL_STS=" + endpoint(leader)});
    for (const auto& e : env) {
        cmd.insert(cmd.end(), {"-e", e});
    }
    cmd.insert(cmd.end(),
               {"aws-cli", "sts", "get-caller-identity", "--query", "Arn", "--output", "text"});
    return os::real_exec(cmd);
}

void dump_logs() {
    auto r = os::real_exec(compose_cmd({"logs", "--no-color", "--tail", "200"}));
    std::cerr << "---- compose logs ----\n" << r.out << "\n----------------------\n";
}

struct stack_fixture {
    stack_fixture() {
        os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
        auto up = compose_cmd({"up", "-d"});
        if (!with_redis()) {
            up.insert(up.end(), {"certs", "iam-1", "iam-2", "iam-3", "ca-1", "oidc-issuer"});
        }
        try {
            os::checked_exec(os::real_exec, up);
        } catch (...) {
            dump_logs();
            throw;
        }
    }
    ~stack_fixture() {
        if (std::getenv("KYTHIRA_IAM_KEEP_STACK") == nullptr) {
            os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
        }
    }
    stack_fixture(const stack_fixture&) = delete;
    stack_fixture& operator=(const stack_fixture&) = delete;
};

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

// One case: the driver's checks build on each other's state (users, roles
// and providers it made), and the CLI checks use the files it left.
BOOST_AUTO_TEST_CASE(sdk_and_cli_conformance) {
    const logs_on_failure logs;

    int leader = 0;
    BOOST_REQUIRE_MESSAGE(eventually(
                              [&] {
                                  leader = agreed_leader();
                                  return leader != 0;
                              },
                              120s),
                          "the three voters never agreed on a leader");
    BOOST_TEST_MESSAGE("leader is " << service(leader));

    const auto a = create_account(leader, leader, "111111111111", "conformance-a");
    const auto b = create_account(leader, leader, "222222222222", "conformance-b");
    // STS signs nothing until the cluster has a key, and the key can only
    // be sealed once the first account has set the sealing salt.
    {
        auto rot = iam_cli(leader, leader, {"rotate-sts-key"});
        BOOST_REQUIRE_MESSAGE(rot.code == 0, "rotate-sts-key failed:\n" << rot.out);
    }

    std::string redis_hosts;
    if (with_redis()) {
        for (const auto& [id, host] : {std::pair{"6", "redis-1"}, std::pair{"7", "redis-2"}}) {
            auto r =
                iam_cli(leader, leader,
                        {"replica-add", "--node-id", id, "--rpc-host", host, "--rpc-port", "7100"});
            BOOST_REQUIRE_MESSAGE(r.code == 0, "replica-add " << host << " failed:\n" << r.out);
        }
        redis_hosts = "redis-1:6379,redis-2:6379";
    }

    // The driver: one PASS or FAIL line per check.
    auto driver = os::real_exec(compose_cmd({"--profile",
                                             "cli",
                                             "run",
                                             "--rm",
                                             "-T",
                                             "-e",
                                             "IAM_ENDPOINT=" + endpoint(leader),
                                             "-e",
                                             "ROOT_A_ID=" + a.id,
                                             "-e",
                                             "ROOT_A_SECRET=" + a.secret,
                                             "-e",
                                             "ROOT_B_ID=" + b.id,
                                             "-e",
                                             "ROOT_B_SECRET=" + b.secret,
                                             "-e",
                                             "REDIS_HOSTS=" + redis_hosts,
                                             "-e",
                                             "CA_AUTH_TOKEN=conformance-ca-token",
                                             "driver"}));
    int passed = 0;
    {
        std::istringstream lines(driver.out);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.starts_with("PASS ")) {
                ++passed;
                BOOST_TEST_MESSAGE(line);
            } else if (line.starts_with("FAIL ")) {
                BOOST_ERROR("driver: " << line);
            } else if (line.starts_with("SKIP ")) {
                BOOST_TEST_MESSAGE(line);
            }
        }
    }
    BOOST_TEST(driver.code == 0, "the driver exited " << driver.code << ":\n" << driver.out);
    BOOST_TEST(passed >= 14, "only " << passed << " driver checks passed:\n" << driver.out);

    // The AWS CLI v2's own credential providers (Requirement 10.11).
    const std::string files[] = {"AWS_CONFIG_FILE=/shared/config",
                                 "AWS_SHARED_CREDENTIALS_FILE=/shared/credentials"};
    {
        auto r = cli_identity(leader, {files[0], files[1], "AWS_PROFILE=conf-role-b"});
        BOOST_TEST(r.code == 0, r.out);
        BOOST_TEST(
            r.out.find("arn:aws:sts::111111111111:assumed-role/conf-b/") != std::string::npos,
            "two-role chain: " << r.out);
    }
    {
        auto r = cli_identity(leader, {files[0], files[1], "AWS_PROFILE=conf-web"});
        BOOST_TEST(r.code == 0, r.out);
        BOOST_TEST(
            r.out.find("arn:aws:sts::111111111111:assumed-role/conf-web/web") != std::string::npos,
            "web_identity_token_file: " << r.out);
    }
    {
        auto r = cli_identity(
            leader,
            {"AWS_CONFIG_FILE=/dev/null", "AWS_SHARED_CREDENTIALS_FILE=/dev/null",
             "AWS_ROLE_ARN=arn:aws:iam::111111111111:role/conf-web",
             "AWS_WEB_IDENTITY_TOKEN_FILE=/shared/web-token", "AWS_ROLE_SESSION_NAME=cli-env"});
        BOOST_TEST(r.code == 0, r.out);
        BOOST_TEST(r.out.find("arn:aws:sts::111111111111:assumed-role/conf-web/cli-env") !=
                       std::string::npos,
                   "AWS_ROLE_ARN pair: " << r.out);
    }
}
