// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// cmd/chaos_ctl (cairn-chaos-testing task 7): the tools over the
// CmdExecutor/HttpGet stubs, the schedule log's ordering, and replay's
// ddmin and reproduction counting over a stub attempt.

#define BOOST_TEST_MODULE chaos_ctl_unit_test
#include <boost/test/unit_test.hpp>

#include "replay.hpp"
#include "run_state.hpp"
#include "schedule_log.hpp"
#include "tools.hpp"

#include <boost/json.hpp>

#include <unistd.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace dc = docker_chaos;
using chaos_ctl::call_record;

namespace {

/// A three-node cluster as its control planes and container runtime would
/// answer, recording every request and command in one ordered trace.
struct fake_cluster {
    struct node {
        std::string role{"follower"};
        std::uint64_t term{1};
        std::uint64_t commit{2};
        std::vector<std::pair<std::uint64_t, std::uint64_t>> log{{1, 1}, {2, 1}};  // index, term
        std::vector<std::uint64_t> won;                                            // terms
        bool up{true};
    };
    std::map<int, node> nodes;
    std::vector<std::string> trace;
    std::vector<std::vector<std::string>> commands;
    std::function<void(const std::vector<std::string>&)> on_command;
    /// Called before each GET is answered, to change the cluster mid-read.
    std::function<void(int id, const std::string& path)> on_get;

    fake_cluster() {
        for (int id = 1; id <= 3; ++id) {
            nodes[id] = node{};
        }
        nodes[1].role = "leader";
        nodes[1].won = {1};
    }

    static auto id_of(const std::string& url) -> int {
        for (const auto& [id, p] : dc::k_node_map) {
            if (url.find(":" + std::to_string(p.http_port) + "/") != std::string::npos) {
                return id;
            }
        }
        return 0;
    }

    auto get(const std::string& url) -> dc::HttpResult {
        const int id = id_of(url);
        const auto path = url.substr(url.find('/', 8));
        trace.push_back("GET " + std::to_string(id) + " " + path);
        if (on_get) {
            on_get(id, path);
        }
        auto& n = nodes.at(id);
        if (!n.up) {
            return {0, "connection failed"};
        }
        if (path == "/health") {
            return {200, R"({"status":"running"})"};
        }
        if (path == "/status") {
            const auto last = n.log.empty() ? 0 : n.log.back().first;
            return {200, boost::json::serialize(boost::json::object{{"node_id", id},
                                                                    {"role", n.role},
                                                                    {"term", n.term},
                                                                    {"commit_index", n.commit},
                                                                    {"last_applied", n.commit},
                                                                    {"snapshot_index", 0},
                                                                    {"last_log_index", last}})};
        }
        if (path.starts_with("/log?")) {
            boost::json::array entries;
            for (const auto& [i, t] : n.log) {
                entries.push_back(boost::json::object{
                    {"index", i}, {"term", t}, {"type", "command"}, {"command_b64", "eA=="}});
            }
            return {200,
                    boost::json::serialize(boost::json::object{{"from", 1},
                                                               {"to", n.log.size()},
                                                               {"snapshot_index", 0},
                                                               {"last_log_index", n.log.size()},
                                                               {"entries", entries}})};
        }
        if (path == "/elections") {
            boost::json::array el;
            for (auto t : n.won) {
                el.push_back(boost::json::object{{"term", t}, {"at_ms", 1}});
            }
            return {200, boost::json::serialize(
                             boost::json::object{{"node_id", id}, {"elections", el}})};
        }
        return {404, "not found"};
    }

    auto post(const std::string& url, const std::string& body) -> dc::HttpResult {
        trace.push_back("POST " + std::to_string(id_of(url)) + " " + url.substr(url.find('/', 8)) +
                        " " + body);
        return {200, R"({"success":true,"commit_index":3})"};
    }

    auto exec(const std::vector<std::string>& argv) -> dc::os::CmdResult {
        commands.push_back(argv);
        std::string joined;
        for (const auto& a : argv) {
            joined += (joined.empty() ? "" : " ") + a;
        }
        trace.push_back("EXEC " + joined);
        if (on_command) {
            on_command(argv);
        }
        if (argv.size() > 1 && argv[1] == "inspect") {
            const auto& c = argv.back();
            return {0, "10.0.0." + c.substr(c.size() - 1) + "\n"};
        }
        if (argv.size() > 2 && argv[1] == "kill") {
            for (auto& [id, n] : nodes) {
                if (dc::k_node_map.at(id).container_name == argv[2]) {
                    n.up = false;
                }
            }
        }
        return {0, ""};
    }
};

struct fixture {
    fs::path dir;
    fake_cluster fake;
    dc::ChaosCluster cluster;
    std::vector<std::pair<int, std::string>> fiu;
    std::map<int, bool> procs_alive;
    std::vector<std::pair<int, int>> signals;
    std::int64_t clock{1000};
    chaos_ctl::context ctx;

    fixture()
        : dir(fs::temp_directory_path() /
              ("chaos_ctl_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter()++))),
          cluster(
              "unused.yml", dc::k_node_map, std::chrono::seconds{1},
              [this](const std::vector<std::string>& a) { return fake.exec(a); },
              [this](const std::string& u) { return fake.get(u); },
              [this](const std::string& u, const std::string& b) { return fake.post(u, b); }),
          ctx{.paths = {dir},
              .cfg = {},
              .cluster = cluster,
              .fiu_send = [this](dc::ChaosNode& n,
                                 const std::string& cmd) { fiu.emplace_back(n.id(), cmd); },
              .procs = {.spawn =
                            [this](const std::vector<std::string>&,
                                   const std::vector<std::string>& env, const std::string&,
                                   const std::string&) {
                                spawned_env = env;
                                procs_alive[4242] = true;
                                return 4242;
                            },
                        .alive = [this](int pid, const std::string&) { return procs_alive[pid]; },
                        .signal =
                            [this](int pid, int sig) {
                                signals.emplace_back(pid, sig);
                                procs_alive[pid] = false;
                            },
                        .sleep = [](std::chrono::milliseconds) {}},
              .mono_ms = [this] { return clock += 10; },
              .wall = [] { return std::string("2026-10-24T00:00:00.000Z"); },
              .failpoints = {"raft/persistence/save_current_term", "raft/state_machine/apply"}} {
        cluster.detach();
        fs::create_directories(dir);
        ctx.cfg.seed = 7;
        ctx.cfg.workload_binary = "/bin/chaos_workload";
    }
    ~fixture() { fs::remove_all(dir); }
    fixture(const fixture&) = delete;
    fixture& operator=(const fixture&) = delete;

    std::vector<std::string> spawned_env;

    static auto counter() -> int& {
        static int c = 0;
        return c;
    }

    auto lines() -> std::vector<boost::json::object> {
        return chaos_ctl::read_lines(ctx.paths.schedule().string());
    }
    auto log_reads() -> std::size_t {
        std::size_t n = 0;
        for (const auto& t : fake.trace) {
            n += t.find(" /log?") != std::string::npos ? 1 : 0;
        }
        return n;
    }
};

auto rec(std::int64_t seq, std::string tool, std::int64_t mono,
         std::vector<std::string> violated = {}) -> call_record {
    call_record r;
    r.seq = seq;
    r.tool = std::move(tool);
    r.started_mono_ms = mono;
    r.status = "ok";
    for (auto& v : violated) {
        r.violations.push_back(boost::json::object{{"invariant", v}});
    }
    r.started_line = {{"seq", seq},
                      {"status", "started"},
                      {"tool", r.tool},
                      {"args", boost::json::object{}},
                      {"mono_ms", mono}};
    return r;
}

}  // namespace

// ── Schedule log ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(schedule_log)

BOOST_FIXTURE_TEST_CASE(every_call_is_logged_before_it_runs, fixture) {
    // The kill's command runs after the started line is on disk.
    std::string status_when_killed;
    fake.on_command = [&](const std::vector<std::string>& argv) {
        if (argv.size() > 1 && argv[1] == "kill") {
            status_when_killed = std::string(lines().back().at("status").as_string());
        }
    };
    auto out = chaos_ctl::call(ctx, "fault.kill", {{"node", 2}});
    BOOST_TEST(out.at("status").as_string() == "ok");
    BOOST_TEST(status_when_killed == "started");
    auto l = lines();
    BOOST_REQUIRE_EQUAL(l.size(), 2u);
    BOOST_TEST(l[0].at("seq").as_int64() == 1);
    BOOST_TEST(l[0].at("tool").as_string() == "fault.kill");
    BOOST_TEST(l[0].at("seed").to_number<std::uint64_t>() == 7u);
    BOOST_TEST(l[0].contains("kythira_commit"));
    BOOST_TEST(l[0].contains("image_id"));
    BOOST_TEST(l[0].contains("wall"));
    BOOST_TEST(l[1].at("seq").as_int64() == 1);
    BOOST_TEST(l[1].at("status").as_string() == "ok");
}

BOOST_FIXTURE_TEST_CASE(seq_continues_across_calls, fixture) {
    static_cast<void>(chaos_ctl::call(ctx, "cluster.status", {}));
    static_cast<void>(chaos_ctl::call(ctx, "cluster.status", {}));
    auto calls = chaos_ctl::read_calls(ctx.paths.schedule().string());
    BOOST_REQUIRE_EQUAL(calls.size(), 2u);
    BOOST_TEST(calls[1].seq == 2);
}

BOOST_AUTO_TEST_CASE(fold_keeps_a_call_with_no_outcome_and_drops_a_torn_line) {
    const auto p = fs::temp_directory_path() / ("chaos_ctl_fold_" + std::to_string(::getpid()));
    {
        std::ofstream out(p);
        out << R"({"seq":1,"status":"started","tool":"fault.kill","args":{"node":1},"mono_ms":5})"
            << "\n"
            << R"({"seq":1,"status":"ok","result":{},"violations":[{"invariant":"log_matching"}]})"
            << "\n"
            << R"({"seq":2,"status":"started","tool":"fault.pause","args":{"node":2},"mono_ms":9})"
            << "\n"
            << R"({"seq":2,"stat)";
    }
    auto calls = chaos_ctl::read_calls(p.string());
    fs::remove(p);
    BOOST_REQUIRE_EQUAL(calls.size(), 2u);
    BOOST_TEST(calls[0].status == "ok");
    BOOST_TEST(calls[0].violated() == std::set<std::string>{"log_matching"});
    BOOST_TEST(calls[1].status == "started");  // aborted, still on record
    BOOST_TEST(!calls[1].done_line.has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Tools ────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(tools)

BOOST_AUTO_TEST_CASE(toolset_is_exactly_requirement_3_1) {
    std::set<std::string> names;
    for (const auto& t : chaos_ctl::tools()) {
        names.insert(t.name);
    }
    const std::set<std::string> expected{"fault.failpoint",
                                         "fault.partition",
                                         "fault.delay",
                                         "fault.drop",
                                         "fault.kill",
                                         "fault.pause",
                                         "fault.restart",
                                         "fault.clock_skew",
                                         "cluster.status",
                                         "cluster.add_member",
                                         "cluster.remove_member",
                                         "workload.start",
                                         "workload.stop",
                                         "workload.history",
                                         "check.run",
                                         "run.reset"};
    BOOST_TEST(names == expected);
}

BOOST_FIXTURE_TEST_CASE(unknown_tool_is_refused_without_logging, fixture) {
    BOOST_CHECK_THROW(static_cast<void>(chaos_ctl::call(ctx, "fault.reboot_host", {})),
                      chaos_ctl::tool_error);
    BOOST_TEST(lines().empty());
}

BOOST_FIXTURE_TEST_CASE(failpoint_outside_allowlist_is_refused, fixture) {
    auto out = chaos_ctl::call(
        ctx, "fault.failpoint",
        {{"node", 1}, {"name", "raft/network/send_request_vote"}, {"mode", "always"}});
    BOOST_TEST(out.at("status").as_string() == "error");
    BOOST_TEST(std::string(out.at("error").as_string()).find("allowlist") != std::string::npos);
    BOOST_TEST(fiu.empty());
    BOOST_TEST(lines().size() == 2u);  // still on record
}

BOOST_FIXTURE_TEST_CASE(failpoint_modes_build_fiu_commands, fixture) {
    const std::string fp = "raft/persistence/save_current_term";
    for (const auto* mode : {"always", "once", "off"}) {
        auto out =
            chaos_ctl::call(ctx, "fault.failpoint", {{"node", 2}, {"name", fp}, {"mode", mode}});
        BOOST_TEST(out.at("status").as_string() == "ok");
    }
    auto random =
        chaos_ctl::call(ctx, "fault.failpoint",
                        {{"node", 3}, {"name", fp}, {"mode", "random"}, {"probability", 0.25}});
    BOOST_TEST(random.at("status").as_string() == "ok");
    auto no_p =
        chaos_ctl::call(ctx, "fault.failpoint", {{"node", 3}, {"name", fp}, {"mode", "random"}});
    BOOST_TEST(no_p.at("status").as_string() == "error");
    BOOST_REQUIRE_EQUAL(fiu.size(), 4u);
    BOOST_TEST(fiu[0].first == 2);
    BOOST_TEST(fiu[0].second == dc::fiu::build_enable_always_cmd(fp));
    BOOST_TEST(fiu[1].second == dc::fiu::build_enable_once_cmd(fp));
    BOOST_TEST(fiu[2].second == dc::fiu::build_disable_cmd(fp));
    BOOST_TEST(fiu[3].second == dc::fiu::build_enable_random_cmd(fp, 0.25));
}

BOOST_FIXTURE_TEST_CASE(unknown_node_is_an_error, fixture) {
    auto out = chaos_ctl::call(ctx, "fault.kill", {{"node", 9}});
    BOOST_TEST(out.at("status").as_string() == "error");
    BOOST_TEST(fake.commands.empty());
}

BOOST_FIXTURE_TEST_CASE(clock_skew_is_unsupported_not_applied, fixture) {
    auto out = chaos_ctl::call(ctx, "fault.clock_skew", {{"node", 1}, {"offset_ms", 500}});
    BOOST_TEST(out.at("status").as_string() == "unsupported");
    BOOST_TEST(out.at("raft_safety").is_null());
    BOOST_TEST(fake.commands.empty());
    BOOST_TEST(lines().back().at("status").as_string() == "unsupported");
}

BOOST_FIXTURE_TEST_CASE(kill_snapshots_the_node_first, fixture) {
    static_cast<void>(chaos_ctl::call(ctx, "fault.kill", {{"node", 1}}));
    // /elections of node 1 read before the kill command ran.
    std::size_t elections_at = 0;
    std::size_t kill_at = 0;
    for (std::size_t i = 0; i < fake.trace.size(); ++i) {
        if (elections_at == 0 && fake.trace[i] == "GET 1 /elections") {
            elections_at = i + 1;
        }
        if (fake.trace[i].find(" kill chaos_node_1") != std::string::npos) {
            kill_at = i + 1;
        }
    }
    BOOST_TEST(elections_at != 0u);
    BOOST_TEST(kill_at > elections_at);
    // And the term it won survives the kill for the safety checker.
    auto saved = chaos_ctl::saved_elections(ctx.paths);
    BOOST_REQUIRE(!saved.empty());
    BOOST_TEST(saved.front().at("elections").as_array().size() == 1u);
    BOOST_TEST(fs::exists(ctx.paths.snapshots() / "000001-node1.json"));
}

BOOST_FIXTURE_TEST_CASE(partition_cuts_links_between_groups_only, fixture) {
    auto out = chaos_ctl::call(
        ctx, "fault.partition",
        {{"groups", boost::json::array{boost::json::array{1}, boost::json::array{2, 3}}}});
    BOOST_TEST(out.at("status").as_string() == "ok");
    std::map<std::string, std::set<std::string>> drops;  // container -> dropped ips
    for (const auto& c : fake.commands) {
        // <runtime> exec <container> iptables -A <chain> -s|-d <ip> -j DROP
        if (c.size() > 7 && c[3] == "iptables" && c[4] == "-A") {
            drops[c[2]].insert(c[7]);
        }
    }
    BOOST_TEST(drops["chaos_node_1"] == (std::set<std::string>{"10.0.0.2", "10.0.0.3"}));
    BOOST_TEST(drops["chaos_node_2"] == std::set<std::string>{"10.0.0.1"});
    BOOST_TEST(drops["chaos_node_3"] == std::set<std::string>{"10.0.0.1"});
}

BOOST_FIXTURE_TEST_CASE(partition_needs_every_node_once, fixture) {
    using boost::json::array;
    auto missing = chaos_ctl::call(ctx, "fault.partition", {{"groups", array{array{1}, array{2}}}});
    BOOST_TEST(missing.at("status").as_string() == "error");
    auto twice =
        chaos_ctl::call(ctx, "fault.partition", {{"groups", array{array{1, 2}, array{2, 3}}}});
    BOOST_TEST(twice.at("status").as_string() == "error");
}

BOOST_FIXTURE_TEST_CASE(delay_and_drop_validate_their_ranges, fixture) {
    BOOST_TEST(
        chaos_ctl::call(ctx, "fault.delay", {{"node", 1}, {"delay_ms", 100}, {"jitter_ms", 200}})
            .at("status")
            .as_string() == "error");
    BOOST_TEST(chaos_ctl::call(ctx, "fault.drop", {{"node", 1}, {"loss_pct", 120.0}})
                   .at("status")
                   .as_string() == "error");
    BOOST_TEST(
        chaos_ctl::call(ctx, "fault.delay", {{"node", 1}, {"delay_ms", 100}, {"jitter_ms", 10}})
            .at("status")
            .as_string() == "ok");
    BOOST_TEST(chaos_ctl::call(ctx, "fault.drop", {{"node", 1}, {"clear", true}})
                   .at("status")
                   .as_string() == "ok");
}

BOOST_FIXTURE_TEST_CASE(a_fault_runs_raft_safety_after_and_reports_violations, fixture) {
    // Nodes 1 and 2 both claim term 1.
    fake.nodes[2].won = {1};
    auto out = chaos_ctl::call(ctx, "fault.delay", {{"node", 3}, {"delay_ms", 50}});
    BOOST_TEST(out.at("status").as_string() == "ok");
    BOOST_TEST(out.at("raft_safety").as_object().at("verdict").as_string() == "violation");
    const auto& v = out.at("violations").as_array();
    BOOST_REQUIRE_EQUAL(v.size(), 1u);
    BOOST_TEST(v[0].as_object().at("invariant").as_string() == "election_safety");
    BOOST_TEST(v[0].as_object().at("schedule_seq").as_int64() == 1);
    auto calls = chaos_ctl::read_calls(ctx.paths.schedule().string());
    BOOST_TEST(chaos_ctl::violated(calls) == std::set<std::string>{"election_safety"});
}

BOOST_FIXTURE_TEST_CASE(a_commit_between_reads_is_not_a_leader_completeness_violation, fixture) {
    // Node 1 leads. By the time node 3 is read, the leader has appended
    // and committed index 3, after its own log was read: a sampling
    // artefact of a running workload, not a lost entry (seen in CI).
    bool grown = false;
    fake.on_get = [&](int id, const std::string& path) {
        if (!grown && id == 3 && path == "/status") {
            grown = true;
            for (auto& [nid, n] : fake.nodes) {
                n.log.emplace_back(3, 1);
                n.commit = 3;
            }
        }
    };
    auto out = chaos_ctl::call(ctx, "fault.delay", {{"node", 2}, {"delay_ms", 10}});
    BOOST_TEST(grown);
    BOOST_TEST(out.at("violations").as_array().empty(), boost::json::serialize(out));
}

BOOST_FIXTURE_TEST_CASE(read_only_tools_skip_the_safety_check, fixture) {
    auto out = chaos_ctl::call(ctx, "cluster.status", {});
    BOOST_TEST(out.at("raft_safety").is_null());
    BOOST_TEST(log_reads() == 0u);
    BOOST_TEST(out.at("result").as_object().at("leader").as_int64() == 1);
}

BOOST_FIXTURE_TEST_CASE(membership_goes_to_the_leader, fixture) {
    auto out = chaos_ctl::call(ctx, "cluster.remove_member", {{"node", 3}});
    BOOST_TEST(out.at("status").as_string() == "ok");
    bool sent = false;
    for (const auto& t : fake.trace) {
        sent = sent || t == R"(POST 1 /membership/remove {"node_id":3})";
    }
    BOOST_TEST(sent);
}

BOOST_FIXTURE_TEST_CASE(workload_start_logs_its_resolved_seed_and_stop_signals_it, fixture) {
    auto start = chaos_ctl::call(ctx, "workload.start", {});
    BOOST_TEST(start.at("status").as_string() == "ok");
    // The seed a replay needs is in the started line, not derived later.
    BOOST_TEST(lines().front().at("args").as_object().at("seed").to_number<std::uint64_t>() == 8u);
    bool has_seed = false;
    bool runs_forever = false;
    for (const auto& e : spawned_env) {
        has_seed = has_seed || e == "WORKLOAD_SEED=8";
        runs_forever = runs_forever || e == "WORKLOAD_DURATION_S=0";
    }
    BOOST_TEST(has_seed);
    BOOST_TEST(runs_forever);
    BOOST_TEST(chaos_ctl::call(ctx, "workload.start", {}).at("status").as_string() == "error");

    auto stop = chaos_ctl::call(ctx, "workload.stop", {});
    BOOST_TEST(stop.at("status").as_string() == "ok");
    BOOST_REQUIRE_EQUAL(signals.size(), 1u);
    BOOST_TEST(signals[0].second == SIGTERM);
    BOOST_TEST(!fs::exists(ctx.paths.workload()));
}

BOOST_FIXTURE_TEST_CASE(check_run_reads_the_history_file, fixture) {
    {
        // Write 1, then a read that sees 2: no order explains it.
        using dc::history::event_type;
        using dc::history::op_kind;
        auto ev = [](event_type type, op_kind f, std::optional<std::string> value, std::int64_t t) {
            dc::history::event e;
            e.type = type;
            e.f = f;
            e.key = "k0";
            e.value = std::move(value);
            e.time_ns = t;
            e.node = type == event_type::invoke ? 0 : 1;
            if (f == op_kind::read) {
                e.mode = "linearizable";
            }
            return e;
        };
        std::ofstream h(ctx.paths.history());
        for (const auto& e : {ev(event_type::invoke, op_kind::write, "1", 1),
                              ev(event_type::ok, op_kind::write, "1", 2),
                              ev(event_type::invoke, op_kind::read, std::nullopt, 3),
                              ev(event_type::ok, op_kind::read, "2", 4)}) {
            h << dc::history::to_line(e) << "\n";
        }
    }
    auto out =
        chaos_ctl::call(ctx, "check.run", {{"checks", boost::json::array{"linearizability"}}});
    BOOST_TEST(out.at("status").as_string() == "ok", boost::json::serialize(out));
    BOOST_TEST(
        out.at("result").as_object().at("linearizability").as_object().at("verdict").as_string() ==
        "violation");
    BOOST_TEST(out.at("violations").as_array().front().as_object().at("invariant").as_string() ==
               "linearizability");
}

BOOST_FIXTURE_TEST_CASE(reset_snapshots_and_restarts_a_dead_node, fixture) {
    fake.nodes[3].up = false;
    fake.on_command = [&](const std::vector<std::string>& argv) {
        if (argv.size() > 2 && argv[1] == "start") {
            fake.nodes[3].up = true;
        }
    };
    auto out = chaos_ctl::call(ctx, "run.reset", {});
    BOOST_TEST(out.at("status").as_string() == "ok");
    bool started = false;
    for (const auto& c : fake.commands) {
        started = started || (c.size() > 2 && c[1] == "start" && c[2] == "chaos_node_3");
    }
    BOOST_TEST(started);
}

BOOST_AUTO_TEST_CASE(config_round_trips) {
    chaos_ctl::run_config c;
    c.nodes = dc::chaos5_node_table("x");
    c.seed = 99;
    c.image_id = "sha256:abc";
    c.kythira_commit = "deadbeef";
    c.workload_binary = "/w";
    auto back = chaos_ctl::config_from_json(chaos_ctl::to_json(c));
    BOOST_TEST(back.nodes.size() == 5u);
    BOOST_TEST(back.nodes.at(4).container_name == "x-4");
    BOOST_TEST(back.seed == 99u);
    BOOST_TEST(back.image_id == "sha256:abc");
    BOOST_TEST(back.nodes.at(4).host == "localhost");

    c.nodes = dc::cairn_node_table("r");
    back = chaos_ctl::config_from_json(chaos_ctl::to_json(c));
    BOOST_TEST(back.nodes.at(2).host == "node2");
    BOOST_TEST(back.nodes.at(2).http_port == 8080);
    BOOST_TEST(back.nodes.at(2).container_name == "r-node2");

    // A run.json written before nodes had a host reads as localhost.
    auto old = chaos_ctl::to_json(c);
    for (auto& n : old.at("nodes").as_array()) {
        n.as_object().erase("host");
    }
    BOOST_TEST(chaos_ctl::config_from_json(old).nodes.at(1).host == "localhost");
}

BOOST_AUTO_TEST_CASE(checked_in_allowlist_holds_only_names_chaos_node_reaches) {
    auto fps = chaos_ctl::load_failpoints(fs::path(KYTHIRA_SOURCE_DIR) /
                                          "docker/cairn/failpoints.allowlist");
    BOOST_TEST(fps.contains("raft/persistence/save_current_term"));
    BOOST_TEST(fps.contains("raft/state_machine/apply"));
    // Evaluated only by the simulator transport and object-store persistence.
    BOOST_TEST(!fps.contains("raft/network/send_request_vote"));
    BOOST_TEST(!fps.contains("raft/objstore/put_object"));
    for (const auto& n : fps) {
        BOOST_TEST(n.starts_with("raft/"), n);
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ── Replay and minimization ──────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(replay)

BOOST_AUTO_TEST_CASE(reproduction_means_a_recorded_invariant_again) {
    using chaos_ctl::replay::reproduces;
    BOOST_TEST(reproduces({"log_matching"}, {"log_matching", "liveness"}));
    BOOST_TEST(!reproduces({"log_matching"}, {"liveness"}));
    BOOST_TEST(reproduces({}, {"liveness"}));
    BOOST_TEST(!reproduces({}, {}));
}

BOOST_AUTO_TEST_CASE(ddmin_finds_the_two_calls_that_matter) {
    std::vector<std::size_t> items{0, 1, 2, 3, 4, 5, 6, 7};
    int tests = 0;
    auto keep = chaos_ctl::replay::ddmin(items, [&tests](const std::vector<std::size_t>& s) {
        ++tests;
        const std::set<std::size_t> set(s.begin(), s.end());
        return set.contains(2) && set.contains(5);
    });
    BOOST_TEST(keep == (std::vector<std::size_t>{2, 5}));
    BOOST_TEST(tests < 30);
}

BOOST_AUTO_TEST_CASE(ddmin_returns_empty_when_nothing_is_needed) {
    auto keep = chaos_ctl::replay::ddmin({0, 1, 2}, [](const auto&) { return true; });
    BOOST_TEST(keep.empty());
}

BOOST_AUTO_TEST_CASE(minimize_drops_faults_but_keeps_workload_and_checks) {
    std::vector<call_record> all{
        rec(1, "workload.start", 100),
        rec(2, "fault.delay", 200),
        rec(3, "fault.kill", 300),
        rec(4, "fault.pause", 400),
        rec(5, "fault.restart", 500),
        rec(6, "workload.stop", 600),
        rec(7, "check.run", 700, {"state_machine_safety"}),
    };
    std::vector<std::vector<std::int64_t>> attempted;
    std::vector<std::int64_t> offsets_seen;
    auto attempt = [&](const std::vector<call_record>& calls, std::int64_t base, int) {
        std::vector<std::int64_t> seqs;
        for (const auto& c : calls) {
            seqs.push_back(c.seq);
            if (c.seq == 5) {
                offsets_seen.push_back(c.started_mono_ms - base);
            }
        }
        attempted.push_back(seqs);
        const std::set<std::int64_t> s(seqs.begin(), seqs.end());
        chaos_ctl::replay::attempt_outcome out;
        if (s.contains(3) && s.contains(5)) {  // kill then restart
            out.violated = {"state_machine_safety"};
        }
        return out;
    };
    auto r = chaos_ctl::replay::minimize(all, 1, attempt);
    BOOST_TEST(r.reproduced_in_full);
    std::vector<std::int64_t> kept;
    for (const auto& c : r.minimal) {
        kept.push_back(c.seq);
    }
    BOOST_TEST(kept == (std::vector<std::int64_t>{1, 3, 5, 6, 7}));
    // Every attempt kept the workload and the check, and a kept call kept
    // its original offset from the first call.
    for (const auto& a : attempted) {
        BOOST_TEST(a.front() == 1);
        BOOST_TEST(a.back() == 7);
    }
    for (auto off : offsets_seen) {
        BOOST_TEST(off == 400);
    }
}

BOOST_AUTO_TEST_CASE(minimize_reports_a_log_that_does_not_reproduce) {
    std::vector<call_record> all{rec(1, "fault.kill", 0, {"log_matching"})};
    auto r = chaos_ctl::replay::minimize(all, 2, [](const auto&, std::int64_t, int) {
        return chaos_ctl::replay::attempt_outcome{};
    });
    BOOST_TEST(!r.reproduced_in_full);
    BOOST_TEST(r.attempts == 2);
}

BOOST_AUTO_TEST_CASE(repeat_counts_reproductions) {
    std::vector<call_record> all{rec(1, "fault.kill", 0, {"liveness"})};
    int n = 0;
    auto attempt = [&n](const auto&, std::int64_t, int) {
        chaos_ctl::replay::attempt_outcome out;
        if (++n % 2 == 0) {
            out.violated = {"liveness"};
        }
        return out;
    };
    auto r = chaos_ctl::replay::repeat(all, 0, {"liveness"}, 4, attempt);
    BOOST_TEST(r.attempts == 4);
    BOOST_TEST(r.reproduced == 2);
    BOOST_TEST(r.per_attempt.size() == 4u);
}

BOOST_AUTO_TEST_CASE(replay_skips_read_only_views) {
    BOOST_TEST(!chaos_ctl::replay::replayable(rec(1, "cluster.status", 0)));
    BOOST_TEST(!chaos_ctl::replay::replayable(rec(1, "workload.history", 0)));
    BOOST_TEST(chaos_ctl::replay::replayable(rec(1, "check.run", 0)));
    BOOST_TEST(!chaos_ctl::replay::droppable(rec(1, "check.run", 0)));
    BOOST_TEST(chaos_ctl::replay::droppable(rec(1, "run.reset", 0)));
}

BOOST_AUTO_TEST_SUITE_END()
