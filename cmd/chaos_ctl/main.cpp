// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// chaos_ctl: the cairn tool server's logic (.kiro/specs/cairn-chaos-testing/
// Requirement 3, design §3-§4). One subcommand per tool, JSON in and JSON
// out; the MCP adapter in docker/cairn/tool_server/ maps tools onto these
// and does nothing else.
//
//   chaos_ctl tools
//   chaos_ctl init [--compose F] [--layout chaos3|chaos5] [--seed N]
//                  [--election-timeout-max-ms N] [--image I]
//                  [--workload-binary P] [--start]
//   chaos_ctl <tool> [JSON | -]          e.g. chaos_ctl fault.kill '{"node":2}'
//   chaos_ctl replay [--log F] [--repeat N] [--out-dir D]
//                    [--allow-image-mismatch] [--keep-cluster]
//   chaos_ctl minimize [--repeat N] [--keep 3,7,9] [--out F]
//                      [--allow-image-mismatch]
//
// Every command takes --run-dir D (default $KYTHIRA_CHAOS_RUN_DIR) and
// --failpoints F (default $KYTHIRA_CAIRN_FAILPOINTS, else the allowlist the
// build copies next to this binary). Output is one JSON object on stdout.
//
// Exit status: 0 success; 1 the tool reported an error or was unsupported,
// or replay/minimize reproduced the violation; 2 bad usage or a failure of
// chaos_ctl itself.

#include "replay.hpp"
#include "run_state.hpp"
#include "schedule_log.hpp"
#include "tools.hpp"

#include <boost/json.hpp>

#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace chaos_ctl;

struct usage_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct cli {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::string> opts;
    std::set<std::string> flags;
};

const std::set<std::string> k_flag_opts{"--start", "--allow-image-mismatch", "--keep-cluster"};

auto parse(int argc, char** argv) -> cli {
    cli c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.starts_with("--")) {
            if (k_flag_opts.contains(a)) {
                c.flags.insert(a);
            } else if (i + 1 < argc) {
                c.opts[a] = argv[++i];
            } else {
                throw usage_error(a + " needs a value");
            }
        } else if (c.command.empty()) {
            c.command = a;
        } else {
            c.positional.push_back(a);
        }
    }
    if (c.command.empty()) {
        throw usage_error("no command; try: chaos_ctl tools");
    }
    return c;
}

auto opt(const cli& c, const std::string& name, const char* env = nullptr)
    -> std::optional<std::string> {
    if (auto it = c.opts.find(name); it != c.opts.end()) {
        return it->second;
    }
    if (env != nullptr) {
        if (const char* v = std::getenv(env); v != nullptr && *v != 0) {
            return std::string(v);
        }
    }
    return std::nullopt;
}

auto exe_dir() -> fs::path {
    return fs::read_symlink("/proc/self/exe").parent_path();
}

auto paths_of(const cli& c) -> run_paths {
    auto dir = opt(c, "--run-dir", "KYTHIRA_CHAOS_RUN_DIR");
    if (!dir) {
        throw usage_error("no run directory: pass --run-dir or set KYTHIRA_CHAOS_RUN_DIR");
    }
    return run_paths{fs::absolute(*dir)};
}

auto failpoints_of(const cli& c) -> std::set<std::string> {
    auto p = opt(c, "--failpoints", "KYTHIRA_CAIRN_FAILPOINTS");
    return load_failpoints(p ? fs::path(*p) : exe_dir() / "cairn-failpoints.allowlist");
}

auto trimmed(std::string s) -> std::string {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.pop_back();
    }
    return s;
}

auto image_id(const std::string& image) -> std::string {
    auto r = docker_chaos::os::real_exec(
        {docker_chaos::os::container_runtime(), "image", "inspect", "--format", "{{.Id}}", image});
    return r.code == 0 ? trimmed(r.out) : std::string{};
}

auto kythira_commit() -> std::string {
    if (const char* v = std::getenv("KYTHIRA_COMMIT"); v != nullptr && *v != 0) {
        return v;
    }
    auto r = docker_chaos::os::real_exec({"git", "rev-parse", "HEAD"});
    return r.code == 0 ? trimmed(r.out) : std::string{"unknown"};
}

void real_fiu_send(docker_chaos::ChaosNode& n, const std::string& cmd) {
    docker_chaos::fiu::send_fiu_cmd("localhost", n.fiu_port(), cmd);
}

void print(const boost::json::value& v) {
    std::cout << boost::json::serialize(v) << '\n';
}

void start_cluster(docker_chaos::ChaosCluster& cluster) {
    cluster.stop();
    cluster.start();
    cluster.wait_for_leader(std::chrono::seconds{30});
}

auto cmd_init(const cli& c) -> int {
    const auto paths = paths_of(c);
    fs::create_directories(paths.dir);
    if (fs::exists(paths.config())) {
        throw usage_error(paths.config().string() + " exists; use a new run directory");
    }
    run_config cfg;
    cfg.compose_file = fs::absolute(opt(c, "--compose").value_or("docker/docker-compose.yml"));
    const auto layout = opt(c, "--layout").value_or("chaos3");
    if (layout == "chaos5") {
        cfg.nodes = docker_chaos::chaos5_node_table(docker_chaos::chaos5_cluster_name());
    } else if (layout != "chaos3") {
        throw usage_error("--layout must be chaos3 or chaos5");
    }
    cfg.seed = opt(c, "--seed") ? std::stoull(*opt(c, "--seed"))
                                : static_cast<std::uint64_t>(real_mono_ms()) ^
                                      static_cast<std::uint64_t>(::getpid());
    cfg.election_timeout_max_ms = std::stoll(opt(c, "--election-timeout-max-ms").value_or("300"));
    cfg.image = opt(c, "--image").value_or(cfg.image);
    cfg.image_id = image_id(cfg.image);
    cfg.kythira_commit = kythira_commit();
    cfg.workload_binary =
        fs::absolute(opt(c, "--workload-binary").value_or((exe_dir() / "chaos_workload").string()));
    write_json_file(paths.config(), to_json(cfg));
    if (c.flags.contains("--start")) {
        docker_chaos::ChaosCluster cluster(cfg.compose_file, cfg.nodes, std::chrono::seconds{60});
        start_cluster(cluster);
        cluster.detach();
    }
    print(boost::json::object{{"run_dir", paths.dir.string()}, {"config", to_json(cfg)}});
    return 0;
}

auto read_args(const cli& c) -> boost::json::object {
    if (c.positional.empty()) {
        return {};
    }
    std::string text = c.positional.front();
    if (text == "-") {
        text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    }
    auto v = boost::json::parse(text);
    if (!v.is_object()) {
        throw usage_error("tool arguments must be a JSON object");
    }
    return v.as_object();
}

auto cmd_tool(const cli& c) -> int {
    const auto paths = paths_of(c);
    auto args = read_args(c);
    run_lock lock(paths);
    const auto cfg = load_config(paths);
    // The cluster outlives every chaos_ctl process.
    docker_chaos::ChaosCluster cluster(cfg.compose_file, cfg.nodes);
    cluster.detach();
    context ctx{.paths = paths,
                .cfg = cfg,
                .cluster = cluster,
                .fiu_send = real_fiu_send,
                .procs = real_process_ops(),
                .mono_ms = real_mono_ms,
                .wall = real_wall,
                .failpoints = failpoints_of(c)};
    auto out = call(ctx, c.command, std::move(args));
    print(out);
    return out.at("status").as_string() == "ok" ? 0 : 1;
}

/// One replay attempt in its own run directory under `out_dir`.
auto real_attempt(const run_config& cfg, const fs::path& out_dir, const std::set<std::string>& fps,
                  bool keep_cluster) -> replay::attempt_fn {
    return [=](const std::vector<call_record>& calls, std::int64_t base, int attempt) {
        run_paths paths{out_dir / ("attempt-" + std::to_string(attempt))};
        fs::remove_all(paths.dir);
        fs::create_directories(paths.dir);
        write_json_file(paths.config(), to_json(cfg));
        docker_chaos::ChaosCluster cluster(cfg.compose_file, cfg.nodes, std::chrono::seconds{60});
        start_cluster(cluster);
        if (keep_cluster) {
            // The next attempt's start takes it down.
            cluster.detach();
        }
        context ctx{.paths = paths,
                    .cfg = cfg,
                    .cluster = cluster,
                    .fiu_send = real_fiu_send,
                    .procs = real_process_ops(),
                    .mono_ms = real_mono_ms,
                    .wall = real_wall,
                    .failpoints = fps};
        const auto t0 = real_mono_ms();
        for (const auto& rec : calls) {
            if (!replay::replayable(rec)) {
                continue;
            }
            const auto due = t0 + (rec.started_mono_ms - base);
            if (const auto wait = due - real_mono_ms(); wait > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{wait});
            }
            static_cast<void>(call(ctx, rec.tool, rec.args));
        }
        // The same end as a scripted case: stop the workload, heal, and run
        // every checker.
        static_cast<void>(call(ctx, "workload.stop", {}));
        static_cast<void>(call(ctx, "run.reset", {}));
        static_cast<void>(
            call(ctx, "check.run",
                 {{"checks", boost::json::array{"raft_safety", "linearizability", "liveness"}}}));
        const auto replayed = read_calls(paths.schedule().string());
        replay::attempt_outcome out;
        out.violated = violated(replayed);
        out.summary = {{"run_dir", paths.dir.string()}, {"calls", replayed.size()}};
        return out;
    };
}

void check_image(const run_config& cfg, const cli& c) {
    const auto now = image_id(cfg.image);
    if (now != cfg.image_id && !c.flags.contains("--allow-image-mismatch")) {
        throw usage_error("image " + cfg.image + " is " + (now.empty() ? "missing" : now) +
                          ", the log recorded " + cfg.image_id +
                          "; rebuild it or pass --allow-image-mismatch");
    }
}

auto positive_int(const std::optional<std::string>& v, int fallback) -> int {
    if (!v) {
        return fallback;
    }
    const int n = std::stoi(*v);
    if (n < 1) {
        throw usage_error("--repeat must be at least 1");
    }
    return n;
}

auto cmd_replay(const cli& c) -> int {
    const auto paths = paths_of(c);
    const auto cfg = load_config(paths);
    check_image(cfg, c);
    const auto log = opt(c, "--log").value_or(paths.schedule().string());
    const auto calls = read_calls(log);
    if (calls.empty()) {
        throw usage_error(log + " holds no calls");
    }
    const auto n = positive_int(opt(c, "--repeat"), 1);
    const fs::path out_dir = opt(c, "--out-dir").value_or((paths.dir / "replay").string());
    const auto recorded = violated(calls);
    auto r = replay::repeat(
        calls, replay::base_of(calls), recorded, n,
        real_attempt(cfg, out_dir, failpoints_of(c), c.flags.contains("--keep-cluster")));
    print(r.to_json(recorded));
    return r.reproduced > 0 ? 1 : 0;
}

auto parse_keep(const std::string& s) -> std::set<std::int64_t> {
    std::set<std::int64_t> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        out.insert(std::stoll(item));
    }
    return out;
}

auto cmd_minimize(const cli& c) -> int {
    const auto paths = paths_of(c);
    const auto cfg = load_config(paths);
    check_image(cfg, c);
    const auto all = read_calls(paths.schedule().string());
    const auto n = positive_int(opt(c, "--repeat"), 3);
    const auto attempt = real_attempt(cfg, paths.dir / "minimize", failpoints_of(c), false);

    if (auto keep = opt(c, "--keep")) {
        // Judge one subset, named by seq, of the droppable calls.
        const auto seqs = parse_keep(*keep);
        std::vector<std::size_t> positions;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (replay::droppable(all[i]) && seqs.contains(all[i].seq)) {
                positions.push_back(i);
            }
        }
        const auto subset = replay::with_droppable(all, positions);
        const auto recorded = violated(all);
        auto r = replay::repeat(subset, replay::base_of(all), recorded, n, attempt, true);
        auto out = r.to_json(recorded);
        out["kept_seqs"] =
            boost::json::value_from(std::vector<std::int64_t>(seqs.begin(), seqs.end()));
        print(out);
        return r.reproduced > 0 ? 1 : 0;
    }

    auto r = replay::minimize(all, n, attempt);
    boost::json::object out{{"reproduced_in_full", r.reproduced_in_full}, {"attempts", r.attempts}};
    if (r.reproduced_in_full) {
        const fs::path dest = opt(c, "--out").value_or((paths.dir / "minimized.jsonl").string());
        std::ofstream f(dest, std::ios::trunc);
        boost::json::array seqs;
        for (const auto& line : lines_of(r.minimal)) {
            f << boost::json::serialize(line) << '\n';
        }
        for (const auto& rec : r.minimal) {
            seqs.emplace_back(rec.seq);
        }
        out["minimal_seqs"] = std::move(seqs);
        out["log"] = dest.string();
    }
    print(out);
    return r.reproduced_in_full ? 1 : 0;
}

auto cmd_tools() -> int {
    boost::json::array out;
    for (const auto& t : tools()) {
        out.emplace_back(boost::json::object{{"name", t.name}, {"summary", t.summary}});
    }
    print(out);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto c = parse(argc, argv);
        if (c.command == "tools") {
            return cmd_tools();
        }
        if (c.command == "init") {
            return cmd_init(c);
        }
        if (c.command == "replay") {
            return cmd_replay(c);
        }
        if (c.command == "minimize") {
            return cmd_minimize(c);
        }
        if (find_tool(c.command) == nullptr) {
            throw usage_error("unknown command '" + c.command + "'; try: chaos_ctl tools");
        }
        return cmd_tool(c);
    } catch (const usage_error& e) {
        print(boost::json::object{{"status", "usage"}, {"error", e.what()}});
        return 2;
    } catch (const std::exception& e) {
        print(boost::json::object{{"status", "failed"}, {"error", e.what()}});
        return 2;
    }
}
