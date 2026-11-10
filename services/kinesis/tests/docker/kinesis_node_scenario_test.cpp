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
//
// A second case (task 9, Requirement 15) runs three unmodified KCL 2.x
// workers (the `kcl` profile) with their leases on the cluster, one worker
// per node, and kills each node in turn with SIGKILL while they renew, so
// the lease group's leader is killed whichever node holds it. Every record
// is processed, and no shard is processed by two workers at once for longer
// than the failover time.
//
// A third case (task 11, Requirement 10) reshards a stream three times
// (UpdateShardCount, SplitShard, MergeShards), each call through one node
// while another is killed with SIGKILL, and a producer writes straight
// through, one record at a time over eight partition keys. The catalog and
// the shards live in different Raft groups (KYTHIRA_SHARD_CUTS), so each
// reshard is a cross-group transaction, and in turn the leader of each is
// killed. A reshard the kill interrupts is finished by the maintenance
// pass. Read in lineage order, parents before children, every record is
// there and each key's records are in the order they were written.

#define BOOST_TEST_MODULE kinesis_node_scenario_test
#include <boost/test/results_collector.hpp>
#include <boost/test/unit_test.hpp>

#include "os_faults.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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
        os::try_exec(os::real_exec, compose_cmd({"--profile", "clients", "--profile", "kcl", "down",
                                                 "-v", "--remove-orphans"}));
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

// ---- KCL workers -----------------------------------------------------------

/// The failover time the workers run with (compose.yml's
/// KCL_FAILOVER_MILLIS).
constexpr std::int64_t k_failover_ms = 10'000;

/// One `EVENT <millis> <worker> <what> <shard> [<sequence> <data>]` line of
/// the sample consumer.
struct kcl_event {
    std::int64_t _ms = 0;
    std::string _worker;
    std::string _what;
    std::string _shard;
    std::string _data;
};

/// Every worker's events so far, in time order. Compose prefixes each line
/// with the service name unless told not to, and not every compose takes
/// --no-log-prefix, so the event is found by its marker.
std::vector<kcl_event> kcl_events() {
    auto r = os::real_exec(
        compose_cmd({"--profile", "kcl", "logs", "--no-color", "kcl-1", "kcl-2", "kcl-3"}));
    std::vector<kcl_event> out;
    std::istringstream lines(r.out);
    std::string line;
    while (std::getline(lines, line)) {
        const auto at = line.find("EVENT ");
        if (at == std::string::npos) {
            continue;
        }
        std::istringstream f(line.substr(at + 6));
        kcl_event e;
        std::string seq;
        if (!(f >> e._ms >> e._worker >> e._what >> e._shard)) {
            continue;
        }
        if (e._what == "record") {
            f >> seq >> e._data;
        }
        out.push_back(std::move(e));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const kcl_event& a, const kcl_event& b) { return a._ms < b._ms; });
    return out;
}

std::set<std::string> kcl_records(const std::vector<kcl_event>& events) {
    std::set<std::string> out;
    for (const auto& e : events) {
        if (e._what == "record") {
            out.insert(e._data);
        }
    }
    return out;
}

/// Shards processed by two workers at once for longer than the failover
/// time. A tenure is one worker's hold on one shard, from `init` to `lost`
/// or `ended`; two tenures of different workers overlap by the span their
/// first and last records share.
std::vector<std::string> double_ownership(const std::vector<kcl_event>& events) {
    struct tenure {
        std::string _worker;
        std::int64_t _first = -1;
        std::int64_t _last = -1;
    };
    std::map<std::string, std::vector<tenure>> by_shard;
    std::map<std::pair<std::string, std::string>, std::size_t> open;
    for (const auto& e : events) {
        auto& ts = by_shard[e._shard];
        const auto key = std::make_pair(e._worker, e._shard);
        if (e._what == "init") {
            open[key] = ts.size();
            ts.push_back({._worker = e._worker});
        } else if (e._what == "record") {
            if (!open.contains(key)) {
                open[key] = ts.size();
                ts.push_back({._worker = e._worker});
            }
            auto& t = ts[open[key]];
            if (t._first < 0) {
                t._first = e._ms;
            }
            t._last = e._ms;
        } else if (e._what == "lost" || e._what == "ended") {
            open.erase(key);
        }
    }
    std::vector<std::string> bad;
    for (const auto& [shard, ts] : by_shard) {
        for (std::size_t i = 0; i < ts.size(); ++i) {
            for (std::size_t j = i + 1; j < ts.size(); ++j) {
                const auto& a = ts[i];
                const auto& b = ts[j];
                if (a._worker == b._worker || a._first < 0 || b._first < 0) {
                    continue;
                }
                const auto overlap = std::min(a._last, b._last) - std::max(a._first, b._first);
                if (overlap > k_failover_ms) {
                    bad.push_back(shard + ": " + a._worker + " and " + b._worker + " for " +
                                  std::to_string(overlap) + " ms");
                }
            }
        }
    }
    return bad;
}

}  // namespace

BOOST_TEST_GLOBAL_FIXTURE(stack_fixture);

// One case on purpose: each step builds on the cluster state the last one
// left, and a fresh stack per step would triple the run for no new coverage.
BOOST_AUTO_TEST_CASE(sdks_against_a_cluster_by_service_name) {
    const logs_on_failure logs;

    // 1. Every node answers once the cluster has a leader: ListStreams is a
    //    replica read, CreateStream needs the catalog group's leader.
    for (int n : {1, 2, 3}) {
        BOOST_REQUIRE_MESSAGE(
            eventually([&] { return aws_cli(n, {"list-streams"}).code == 0; }, 120s),
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
        auto r =
            boto3({"consume", "--endpoints", endpoint(1) + "," + endpoint(2) + "," + endpoint(3),
                   "--stream", "py", "--expect", "500"});
        BOOST_REQUIRE_MESSAGE(r.code == 0, "boto3 consume failed:\n" << r.out);
        BOOST_TEST(r.out.find("\"consumed\": 500") != std::string::npos, r.out);
    }

    // 4. One node down: the other two take writes and serve reads.
    os::checked_exec(os::real_exec, compose_cmd({"stop", "kin-1"}));
    {
        auto r = boto3({"produce", "--endpoint", endpoint(2), "--stream", "py", "--count", "100",
                        "--tag", "b"});
        BOOST_REQUIRE_MESSAGE(r.code == 0, "produce with kin-1 down failed:\n" << r.out);
        auto c = boto3({"consume", "--endpoints", endpoint(2) + "," + endpoint(3), "--stream", "py",
                        "--expect", "600"});
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

// Task 9: KCL 2.x on the native lease layer, through a kill of every node.
BOOST_AUTO_TEST_CASE(kcl_workers_keep_one_owner_per_shard_through_node_kills) {
    const logs_on_failure logs;
    for (int n : {1, 2, 3}) {
        BOOST_REQUIRE_MESSAGE(
            eventually([&] { return aws_cli(n, {"list-streams"}).code == 0; }, 120s),
            service(n) << " never answered ListStreams");
    }
    {
        auto r = boto3({"create", "--endpoint", endpoint(1), "--stream", "kcl", "--shards", "4"},
                       k_admin_id, k_admin_secret);
        BOOST_REQUIRE_MESSAGE(r.code == 0, "boto3 create failed:\n" << r.out);
        auto p = boto3({"produce", "--endpoint", endpoint(1), "--stream", "kcl", "--count", "200",
                        "--tag", "k0"});
        BOOST_REQUIRE_MESSAGE(p.code == 0, "boto3 produce failed:\n" << p.out);
    }
    std::set<std::string> expected;
    const auto expect = [&expected](const std::string& tag, int count) {
        for (int i = 0; i < count; ++i) {
            expected.insert(tag + "-" + std::to_string(i));
        }
    };
    expect("k0", 200);
    const auto all_seen = [&expected] {
        const auto seen = kcl_records(kcl_events());
        return std::includes(seen.begin(), seen.end(), expected.begin(), expected.end());
    };

    os::checked_exec(os::real_exec,
                     compose_cmd({"--profile", "kcl", "up", "-d", "kcl-1", "kcl-2", "kcl-3"}));
    BOOST_REQUIRE_MESSAGE(eventually(all_seen, 180s),
                          "the KCL workers never processed the first 200 records");

    // Kill each node in turn while the workers renew every few seconds: one
    // of the three is the lease group's leader. Write through a live node
    // while it is down, and let the leases fail over before it returns.
    for (int n : {1, 2, 3}) {
        os::checked_exec(os::real_exec, compose_cmd({"kill", service(n)}));
        const int live = (n % 3) + 1;
        const std::string tag = "k" + std::to_string(n);
        BOOST_REQUIRE_MESSAGE(
            eventually(
                [&] {
                    return boto3({"produce", "--endpoint", endpoint(live), "--stream", "kcl",
                                  "--count", "100", "--tag", tag})
                               .code == 0;
                },
                120s),
            "produce with " << service(n) << " killed never succeeded");
        expect(tag, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(k_failover_ms + 5'000));
        os::checked_exec(os::real_exec, compose_cmd({"start", service(n)}));
        BOOST_REQUIRE_MESSAGE(
            eventually([&] { return aws_cli(n, {"list-streams"}).code == 0; }, 120s),
            service(n) << " never answered after restarting");
    }

    BOOST_TEST(eventually(all_seen, 240s), "the KCL workers never processed all 500 records");
    const auto events = kcl_events();
    const auto seen = kcl_records(events);
    std::vector<std::string> missing;
    std::set_difference(expected.begin(), expected.end(), seen.begin(), seen.end(),
                        std::back_inserter(missing));
    BOOST_TEST(missing.empty(), missing.size() << " records never processed, first "
                                               << (missing.empty() ? "" : missing.front()));
    std::set<std::string> workers;
    for (const auto& e : events) {
        if (e._what == "record") {
            workers.insert(e._worker);
        }
    }
    BOOST_TEST(workers.size() >= 2U, "only " << workers.size() << " worker processed records");
    for (const auto& b : double_ownership(events)) {
        BOOST_ERROR("shard processed by two workers past the failover time: " << b);
    }
}

// Task 11: resharding with a node killed in the middle of each reshard.
BOOST_AUTO_TEST_CASE(reshards_survive_a_node_kill_with_no_lost_or_reordered_records) {
    const logs_on_failure logs;
    for (int n : {1, 2, 3}) {
        BOOST_REQUIRE_MESSAGE(
            eventually([&] { return aws_cli(n, {"list-streams"}).code == 0; }, 120s),
            service(n) << " never answered ListStreams");
    }
    {
        auto r = boto3({"create", "--endpoint", endpoint(1), "--stream", "rs", "--shards", "2"},
                       k_admin_id, k_admin_secret);
        BOOST_REQUIRE_MESSAGE(r.code == 0, "boto3 create failed:\n" << r.out);
    }
    const std::string all = endpoint(1) + "," + endpoint(2) + "," + endpoint(3);
    constexpr int k_records = 600;
    os::CmdResult produced;
    std::thread producer([&] {
        produced = boto3({"ordered-produce", "--endpoints", all, "--stream", "rs", "--count",
                          std::to_string(k_records), "--keys", "8", "--tag", "r"});
    });

    // Each reshard goes through `via` while `victim` is killed as the call
    // starts (a reshard on a settled cluster takes well under a second, so
    // a later kill would miss it). Whatever the call answers, the stream must
    // be ACTIVE again once the kill's reshard is finished, by the call or by
    // the maintenance pass. A call refused outright (InternalFailure while a
    // killed leader's group elects a new one, or ResourceInUse while an
    // interrupted reshard is still being finished) changed nothing, so it is
    // made again, as a client would, until the stream has the target count:
    // each reshard happens exactly once, kill or no kill.
    const auto open_shards = [&](int n) -> std::int64_t {
        auto r = aws_cli(n, {"describe-stream-summary", "--stream-name", "rs"});
        auto j = r.code == 0 ? last_json(r.out) : std::nullopt;
        if (!j) {
            return -1;
        }
        const auto* d = j->if_contains("StreamDescriptionSummary");
        const auto* v =
            d != nullptr && d->is_object() ? d->as_object().if_contains("OpenShardCount") : nullptr;
        return v != nullptr && v->is_int64() ? v->as_int64() : -1;
    };
    const auto reshard_with_kill = [&](int via, int victim, int target) {
        const std::vector<std::string> call = {
            "update-shard-count",   "--stream-name",        "rs",
            "--target-shard-count", std::to_string(target), "--scaling-type",
            "UNIFORM_SCALING"};
        const auto update = [&] {
            auto cmd =
                compose_cmd({"--profile", "clients", "run", "--rm", "-T", "-e",
                             std::string("AWS_ACCESS_KEY_ID=") + k_admin_id, "-e",
                             std::string("AWS_SECRET_ACCESS_KEY=") + k_admin_secret, "aws-cli",
                             "--endpoint-url", endpoint(via), "--output", "json", "kinesis"});
            cmd.insert(cmd.end(), call.begin(), call.end());
            return os::real_exec(cmd);
        };
        os::CmdResult answer;
        std::thread caller([&] { answer = update(); });
        std::this_thread::sleep_for(20ms);
        os::checked_exec(os::real_exec, compose_cmd({"kill", service(victim)}));
        caller.join();
        BOOST_TEST_MESSAGE("reshard through " << service(via) << " with " << service(victim)
                                              << " killed: exit " << answer.code << "\n"
                                              << answer.out);
        BOOST_TEST(eventually([&] { return stream_status(via, "rs") == "ACTIVE"; }, 240s),
                   "the stream never returned to ACTIVE after killing " << service(victim));
        os::checked_exec(os::real_exec, compose_cmd({"start", service(victim)}));
        BOOST_REQUIRE_MESSAGE(
            eventually([&] { return aws_cli(victim, {"list-streams"}).code == 0; }, 120s),
            service(victim) << " never answered after restarting");
        BOOST_TEST(eventually(
                       [&] {
                           if (stream_status(via, "rs") != "ACTIVE") {
                               return false;
                           }
                           if (open_shards(via) == target) {
                               return true;
                           }
                           auto again = update();
                           BOOST_TEST_MESSAGE("reshard to " << target << " again through "
                                                            << service(via) << ": exit "
                                                            << again.code << "\n"
                                                            << again.out);
                           return false;
                       },
                       300s),
                   "the stream never reached " << target << " open shards");
    };
    std::this_thread::sleep_for(2s);
    reshard_with_kill(2, 1, 4);
    reshard_with_kill(3, 2, 2);
    reshard_with_kill(1, 3, 3);
    producer.join();
    BOOST_REQUIRE_MESSAGE(produced.code == 0, "ordered-produce failed:\n" << produced.out);

    // Lineage: some shards closed with an ending sequence number, children
    // naming their parents.
    {
        auto r = aws_cli(1, {"list-shards", "--stream-name", "rs"});
        BOOST_REQUIRE_MESSAGE(r.code == 0, "list-shards failed:\n" << r.out);
        BOOST_TEST(r.out.find("EndingSequenceNumber") != std::string::npos, r.out);
        BOOST_TEST(r.out.find("ParentShardId") != std::string::npos, r.out);
    }
    auto c = boto3({"lineage-consume", "--endpoints", all, "--stream", "rs", "--count",
                    std::to_string(k_records), "--keys", "8", "--tag", "r", "--timeout", "180"});
    BOOST_TEST(c.code == 0, "lineage-consume failed:\n" << c.out);
    BOOST_TEST(c.out.find("\"consumed\": " + std::to_string(k_records)) != std::string::npos,
               c.out);
}
