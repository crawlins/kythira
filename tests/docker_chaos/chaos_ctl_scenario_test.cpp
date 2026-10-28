// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// chaos_ctl end to end (cairn-chaos-testing task 7): the binary an agent's
// tool calls reach, driven exactly as the MCP adapter will drive it, one
// process per call, against the 3-node compose cluster. A known-good
// schedule (workload, kill, restart, partition and heal, a failpoint,
// reset) must leave every checker clean, and its replay on a fresh cluster
// must run and find nothing either.
//
// KYTHIRA_CHAOS_CTL names the chaos_ctl binary (the build sets it);
// KYTHIRA_COMPOSE_FILE the compose file, as for the other scenarios;
// KYTHIRA_CHAOS_ARTIFACT_DIR, if set, keeps a failed run's directory.

#define BOOST_TEST_MODULE chaos_ctl_scenario_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"
#include "os_faults.hpp"

#include <boost/json.hpp>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

struct ctl_result {
    int code{0};
    boost::json::object out;
    std::string raw;
};

auto chaos_ctl() -> std::string {
    const char* p = std::getenv("KYTHIRA_CHAOS_CTL");
    BOOST_REQUIRE_MESSAGE(p != nullptr && *p != 0, "KYTHIRA_CHAOS_CTL is not set");
    return p;
}

/// Runs one chaos_ctl command; its last output line is its JSON result.
auto run(const std::vector<std::string>& args) -> ctl_result {
    std::vector<std::string> argv{chaos_ctl()};
    argv.insert(argv.end(), args.begin(), args.end());
    auto r = docker_chaos::os::real_exec(argv);
    ctl_result out{r.code, {}, r.out};
    auto text = r.out;
    while (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    const auto nl = text.rfind('\n');
    const auto last = nl == std::string::npos ? text : text.substr(nl + 1);
    try {
        out.out = boost::json::parse(last).as_object();
    } catch (...) {
        BOOST_ERROR("chaos_ctl printed no JSON: " << r.out);
    }
    BOOST_TEST_MESSAGE("chaos_ctl " << args.front() << " -> " << r.code << " "
                                    << last.substr(0, 400));
    return out;
}

/// KYTHIRA_CHAOS_ARTIFACT_DIR if set, so CI uploads a failed run's
/// schedule log, snapshots and history; else the temp directory.
auto run_base() -> fs::path {
    const char* d = std::getenv("KYTHIRA_CHAOS_ARTIFACT_DIR");
    return (d != nullptr && *d != 0) ? fs::path(d) : fs::temp_directory_path();
}

struct run_dir {
    fs::path path = run_base() / ("chaos_ctl_scenario_" + std::to_string(::getpid()));
    run_dir() { fs::remove_all(path); }
    ~run_dir() {
        // Leave the cluster as the other scenarios do: the next one's start
        // takes it down. An artifact directory keeps the run for upload.
        if (std::getenv("KYTHIRA_CHAOS_ARTIFACT_DIR") == nullptr) {
            fs::remove_all(path);
        }
    }
    run_dir(const run_dir&) = delete;
    run_dir& operator=(const run_dir&) = delete;
};

/// A tool call that must succeed and report no violation.
auto ok(const fs::path& dir, const std::string& tool, const std::string& args = "{}")
    -> boost::json::object {
    auto r = run({tool, args, "--run-dir", dir.string()});
    BOOST_TEST(r.code == 0, tool << ": " << r.raw);
    if (const auto* s = r.out.if_contains("status"); s != nullptr) {
        BOOST_TEST(s->as_string() == "ok", tool << ": " << r.raw);
    }
    if (const auto* v = r.out.if_contains("violations"); v != nullptr) {
        BOOST_TEST(v->as_array().empty(), tool << " reported a violation: " << r.raw);
    }
    return r.out;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(chaos_ctl_scenario)

BOOST_AUTO_TEST_CASE(known_good_schedule_is_clean_and_replays_clean,
                     *boost::unit_test::timeout(600)) {
    run_dir dir;
    auto init = run({"init", "--run-dir", dir.path.string(), "--compose",
                     docker_chaos::default_compose_file(), "--seed", "1", "--start"});
    BOOST_REQUIRE_MESSAGE(init.code == 0, init.raw);
    BOOST_TEST(!init.out.at("config").as_object().at("image_id").as_string().empty());

    auto status = ok(dir.path, "cluster.status");
    BOOST_TEST(!status.at("result").as_object().at("leader").is_null());

    ok(dir.path, "workload.start", R"({"rate":40,"concurrency":3})");
    std::this_thread::sleep_for(2s);
    ok(dir.path, "fault.kill", R"({"node":3})");
    std::this_thread::sleep_for(1s);
    ok(dir.path, "fault.restart", R"({"node":3})");
    ok(dir.path, "fault.partition", R"({"groups":[[1],[2,3]]})");
    std::this_thread::sleep_for(2s);
    ok(dir.path, "fault.partition", R"({"heal":true})");
    ok(dir.path, "fault.delay", R"({"node":2,"delay_ms":40,"jitter_ms":10})");
    ok(dir.path, "fault.failpoint",
       R"({"node":1,"name":"raft/persistence/append_log_entry","mode":"random","probability":0.2})");
    std::this_thread::sleep_for(2s);

    // Refused, and on record as such.
    auto skew = run({"fault.clock_skew", "{}", "--run-dir", dir.path.string()});
    BOOST_TEST(skew.code == 1);
    BOOST_TEST(skew.out.at("status").as_string() == "unsupported");
    auto bad =
        run({"fault.failpoint", R"({"node":1,"name":"raft/objstore/put_object","mode":"always"})",
             "--run-dir", dir.path.string()});
    BOOST_TEST(bad.out.at("status").as_string() == "error");

    ok(dir.path, "run.reset");
    std::this_thread::sleep_for(1s);
    auto stop = ok(dir.path, "workload.stop");
    BOOST_TEST(stop.at("result").as_object().at("events").to_number<std::int64_t>() > 0);
    auto check =
        ok(dir.path, "check.run", R"({"checks":["raft_safety","linearizability","liveness"]})");
    const auto& res = check.at("result").as_object();
    BOOST_TEST(res.at("raft_safety").as_object().at("verdict").as_string() == "ok");
    BOOST_TEST(res.at("linearizability").as_object().at("verdict").as_string() == "ok");
    BOOST_TEST(res.at("liveness").as_object().at("verdict").as_string() == "ok");
    BOOST_TEST(res.at("linearizability").as_object().at("operations").to_number<std::int64_t>() >
               0);

    // The schedule replays on a fresh cluster and, with nothing recorded,
    // reproduces only if the replay itself sees a violation.
    auto replay = run({"replay", "--run-dir", dir.path.string(), "--repeat", "1"});
    BOOST_TEST(replay.code == 0, replay.raw);
    BOOST_TEST(replay.out.at("attempts").to_number<std::int64_t>() == 1);
    BOOST_TEST(replay.out.at("reproduced").to_number<std::int64_t>() == 0);
}

BOOST_AUTO_TEST_SUITE_END()
