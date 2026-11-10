// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The Kinesis cluster in containers (.kiro/specs/kinesis-compatible-streams/
// task 7, Requirements 9.1-9.4 and 17.3). services/kinesis/tests/docker/
// compose.yml starts three gateway nodes, kin-1..kin-3, addressed by
// service name only, and two clients in the `clients` profile. Under
// whichever runtime the harness selects (Docker or rootless Podman) the case
// shows that:
//
//   * the AWS CLI v2 creates a stream through one node, sees it ACTIVE
//     through the others, writes through a third, and reads its records back
//     with an iterator issued by one node and spent on another;
//   * boto3 writes 500 records through one node and a polling consumer
//     reads all of them, once each and in sequence order per shard, with
//     each page from the next node;
//   * with one node stopped the other two keep taking writes and serving
//     reads, and the stopped node catches up when it starts again and
//     serves every record itself.

#define BOOST_TEST_MODULE kinesis_node_scenario_test
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
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace os = docker_chaos::os;

namespace {

// keys.txt beside compose.yml.
constexpr const char* k_admin_id = "AKIDKINESISADMIN001";
constexpr const char* k_admin_secret = "kinesis-scenario-admin-secret";
constexpr const char* k_client_id = "AKIDKINESISCLIENT01";
constexpr const char* k_client_secret = "kinesis-scenario-client-secret";

std::string compose_file() {
    const char* env = std::getenv("KYTHIRA_KINESIS_COMPOSE_FILE");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return "services/kinesis/tests/docker/compose.yml";
}

std::vector<std::string> compose_cmd(std::initializer_list<std::string> args) {
    auto cmd = os::compose_prefix();
    cmd.insert(cmd.end(), {"-f", compose_file()});
    cmd.insert(cmd.end(), args);
    return cmd;
}

std::string service(int n) {
    return "kin-" + std::to_string(n);
}

std::string endpoint(int n) {
    return "http://" + service(n) + ":4567";
}

/// The AWS CLI v2 against node `target`.
os::CmdResult aws_cli(int target, std::initializer_list<std::string> args,
                      const char* key_id = k_client_id, const char* secret = k_client_secret) {
    auto cmd = compose_cmd({"--profile", "clients", "run", "--rm", "-T", "-e",
                            std::string("AWS_ACCESS_KEY_ID=") + key_id, "-e",
                            std::string("AWS_SECRET_ACCESS_KEY=") + secret, "aws-cli",
                            "--endpoint-url", endpoint(target), "--output", "json", "kinesis"});
    cmd.insert(cmd.end(), args);
    return os::real_exec(cmd);
}

/// kinesis_e2e.py in the boto3 client.
os::CmdResult boto3(std::initializer_list<std::string> args, const char* key_id = k_client_id,
                    const char* secret = k_client_secret) {
    auto cmd = compose_cmd({"--profile", "clients", "run", "--rm", "-T", "-e",
                            std::string("AWS_ACCESS_KEY_ID=") + key_id, "-e",
                            std::string("AWS_SECRET_ACCESS_KEY=") + secret, "boto3"});
    cmd.insert(cmd.end(), args);
    return os::real_exec(cmd);
}

/// The last JSON object in `out` (compose can print pull or warning lines
/// around a client's output).
std::optional<boost::json::object> last_json(const std::string& out) {
    const auto last = out.rfind('}');
    if (last == std::string::npos) {
        return std::nullopt;
    }
    // Walk back to the brace that opens the object ending at `last`.
    int depth = 0;
    for (auto i = static_cast<std::ptrdiff_t>(last); i >= 0; --i) {
        const char c = out[static_cast<std::size_t>(i)];
        depth += c == '}' ? 1 : (c == '{' ? -1 : 0);
        if (depth == 0) {
            boost::system::error_code ec;
            auto v = boost::json::parse(
                out.substr(static_cast<std::size_t>(i), last - static_cast<std::size_t>(i) + 1),
                ec);
            if (ec || !v.is_object()) {
                return std::nullopt;
            }
            return v.as_object();
        }
    }
    return std::nullopt;
}

std::string text_at(const boost::json::object& o, std::initializer_list<const char*> path) {
    const boost::json::value* v = nullptr;
    const boost::json::object* at = &o;
    for (const auto* key : path) {
        v = at == nullptr ? nullptr : at->if_contains(key);
        if (v == nullptr) {
            return {};
        }
        at = v->is_object() ? &v->as_object() : nullptr;
    }
    return v != nullptr && v->is_string() ? std::string(v->as_string()) : std::string{};
}

/// Polls until `pred` holds or `timeout` passes.
bool eventually(const std::function<bool()>& pred, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(2s);
    }
    return pred();
}

std::string stream_status(int n, const std::string& stream) {
    auto r = aws_cli(n, {"describe-stream-summary", "--stream-name", stream});
    if (r.code != 0) {
        return {};
    }
    auto j = last_json(r.out);
    return j ? text_at(*j, {"StreamDescriptionSummary", "StreamStatus"}) : std::string{};
}

void dump_logs() {
    auto r = os::real_exec(compose_cmd({"logs", "--no-color", "--tail", "200"}));
    std::cerr << "---- compose logs ----\n" << r.out << "\n----------------------\n";
}

struct stack_fixture {
    stack_fixture() {
        os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
        try {
            os::checked_exec(os::real_exec, compose_cmd({"up", "-d", "kin-1", "kin-2", "kin-3"}));
        } catch (...) {
            dump_logs();
            throw;
        }
    }
    ~stack_fixture() {
        os::try_exec(os::real_exec,
                     compose_cmd({"--profile", "clients", "down", "-v", "--remove-orphans"}));
    }
    stack_fixture(const stack_fixture&) = delete;
    stack_fixture& operator=(const stack_fixture&) = delete;
};

struct logs_on_failure {
    logs_on_failure() = default;
    ~logs_on_failure() {
        const auto& results = boost::unit_test::results_collector.results(
            boost::unit_test::framework::current_test_case().p_id);
        if (!results.passed() || std::getenv("KYTHIRA_KINESIS_KEEP_LOGS") != nullptr) {
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
BOOST_AUTO_TEST_CASE(sdks_against_a_cluster_by_service_name) {
    const logs_on_failure logs;

    // 1. Every node answers once the cluster has a leader: ListStreams is a
    //    replica read, CreateStream needs the catalog group's leader.
    for (int n : {1, 2, 3}) {
        BOOST_REQUIRE_MESSAGE(eventually([&] { return aws_cli(n, {"list-streams"}).code == 0; },
                                         120s),
                              service(n) << " never answered ListStreams");
    }
    BOOST_REQUIRE_MESSAGE(
        eventually(
            [] {
                auto r = aws_cli(2, {"create-stream", "--stream-name", "cli", "--shard-count", "2"},
                                 k_admin_id, k_admin_secret);
                return r.code == 0 || r.out.find("ResourceInUseException") != std::string::npos;
            },
            120s),
        "CreateStream through kin-2 never succeeded");

    // 2. The AWS CLI: ACTIVE everywhere, a write through kin-3, an iterator
    //    from kin-1 spent on kin-2.
    for (int n : {1, 3}) {
        BOOST_TEST(eventually([&] { return stream_status(n, "cli") == "ACTIVE"; }, 60s),
                   service(n) << " never saw the stream ACTIVE");
    }
    std::string shard;
    {
        auto r = aws_cli(3, {"put-record", "--stream-name", "cli", "--partition-key", "pk-cli",
                             "--data", "aGVsbG8gY2xp", "--cli-binary-format", "base64"});
        BOOST_REQUIRE_MESSAGE(r.code == 0, "put-record failed:\n" << r.out);
        auto j = last_json(r.out);
        BOOST_REQUIRE(j.has_value());
        shard = text_at(*j, {"ShardId"});
        BOOST_TEST(!text_at(*j, {"SequenceNumber"}).empty());
    }
    std::string iterator;
    {
        auto r = aws_cli(1, {"get-shard-iterator", "--stream-name", "cli", "--shard-id", shard,
                             "--shard-iterator-type", "TRIM_HORIZON"});
        BOOST_REQUIRE_MESSAGE(r.code == 0, "get-shard-iterator failed:\n" << r.out);
        auto j = last_json(r.out);
        BOOST_REQUIRE(j.has_value());
        iterator = text_at(*j, {"ShardIterator"});
        BOOST_REQUIRE(!iterator.empty());
    }
    BOOST_TEST(eventually(
                   [&] {
                       auto r = aws_cli(2, {"get-records", "--shard-iterator", iterator});
                       // The CLI prints Data base64-encoded: "hello cli".
                       return r.code == 0 && r.out.find("aGVsbG8gY2xp") != std::string::npos;
                   },
                   30s),
               "kin-2 never served the record through kin-1's iterator");
    {
        auto r = aws_cli(1, {"get-records", "--shard-iterator", "AAAA"});
        BOOST_TEST(r.code != 0);
        BOOST_TEST(r.out.find("InvalidArgumentException") != std::string::npos, r.out);
    }

    // 3. boto3: 500 records in through kin-1, all of them out across nodes.
    {
        auto r = boto3({"create", "--endpoint", endpoint(1), "--stream", "py", "--shards", "3"},
                       k_admin_id, k_admin_secret);
        BOOST_REQUIRE_MESSAGE(r.code == 0, "boto3 create failed:\n" << r.out);
    }
    {
        auto r = boto3({"produce", "--endpoint", endpoint(1), "--stream", "py", "--count", "500",
                        "--tag", "a"});
        BOOST_REQUIRE_MESSAGE(r.code == 0, "boto3 produce failed:\n" << r.out);
    }
    {
        auto r = boto3({"consume", "--endpoints",
                        endpoint(1) + "," + endpoint(2) + "," + endpoint(3), "--stream", "py",
                        "--expect", "500"});
        BOOST_REQUIRE_MESSAGE(r.code == 0, "boto3 consume failed:\n" << r.out);
        BOOST_TEST(r.out.find("\"consumed\": 500") != std::string::npos, r.out);
    }

    // 4. One node down: the other two take writes and serve reads.
    os::checked_exec(os::real_exec, compose_cmd({"stop", "kin-1"}));
    {
        auto r = boto3({"produce", "--endpoint", endpoint(2), "--stream", "py", "--count", "100",
                        "--tag", "b"});
        BOOST_REQUIRE_MESSAGE(r.code == 0, "produce with kin-1 down failed:\n" << r.out);
        auto c = boto3({"consume", "--endpoints", endpoint(2) + "," + endpoint(3), "--stream",
                        "py", "--expect", "600"});
        BOOST_REQUIRE_MESSAGE(c.code == 0, "consume with kin-1 down failed:\n" << c.out);
    }

    // 5. kin-1 back: it catches up and serves every record itself.
    os::checked_exec(os::real_exec, compose_cmd({"start", "kin-1"}));
    BOOST_TEST(eventually(
                   [] {
                       auto c = boto3({"consume", "--endpoints", endpoint(1), "--stream", "py",
                                       "--expect", "600", "--timeout", "20"});
                       return c.code == 0;
                   },
                   120s),
               "kin-1 never served all 600 records after restarting");
}
