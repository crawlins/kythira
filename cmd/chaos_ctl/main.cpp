// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// chaos_ctl: the cairn tool server's logic (.kiro/specs/cairn-chaos-testing/
// Requirement 3, design §3-§4). One subcommand per tool, JSON in and JSON
// out; the MCP adapter in docker/cairn/tool_server/ maps tools onto these
// and does nothing else.
//
//   chaos_ctl tools
//   chaos_ctl init [--compose F] [--layout chaos3|chaos5|cairn] [--seed N]
//                  [--election-timeout-max-ms N] [--image I] [--image-id ID]
//                  [--workload-binary P] [--start]
//   chaos_ctl <tool> [JSON | -]          e.g. chaos_ctl fault.kill '{"node":2}'
//   chaos_ctl replay [--log F] [--repeat N] [--out-dir D] [--layout L --compose F]
//                    [--workload-binary P] [--allow-image-mismatch] [--keep-cluster]
//   chaos_ctl minimize [--repeat N] [--keep 3,7,9] [--out F] [--layout L --compose F]
//                      [--workload-binary P] [--allow-image-mismatch]
//   chaos_ctl report [--log F | --run-dir D] [--replay-result F] [--id ID] [--image I]
//                    [--kythira-commit C] [--cairn-version V] [--note TEXT]
//                    [--source-dir R] [--minimized] [--keep-refused]
//                    [--keep-after-violation] [--clang-format P]
//
// The cairn layout is the cluster of docker/cairn/cairn-compose.yml, driven
// from the tool server inside the fence: nodes by service name, the runtime
// only through the filtering proxy, so `init` there takes the image id
// (--image-id or $KYTHIRA_IMAGE_ID) rather than asking the runtime, and
// does not start the cluster. Replay and minimize start clusters, so they
// run on the host: a cairn run replays there with --layout chaos3, the same
// image and settings on published ports.
//
// `report` (task 12, report.hpp) turns a log's first violation into a
// replay file, a draft test and a doc/chaos-findings.md entry under the
// source tree R (default: the current directory). It writes files only and
// refuses to overwrite a finding that is already there.
//
// Every other command takes --run-dir D (default $KYTHIRA_CHAOS_RUN_DIR) and
// --failpoints F (default $KYTHIRA_CAIRN_FAILPOINTS, else the allowlist the
// build copies next to this binary). Output is one JSON object on stdout.
//
// Exit status: 0 success; 1 the tool reported an error or was unsupported,
// or replay/minimize reproduced the violation; 2 bad usage or a failure of
// chaos_ctl itself.

#include "attempt.hpp"
#include "replay.hpp"
#include "report.hpp"
#include "run_state.hpp"
#include "schedule_log.hpp"
#include "tools.hpp"

#include <boost/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
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

const std::set<std::string> k_flag_opts{"--start",        "--allow-image-mismatch",
                                        "--keep-cluster", "--minimized",
                                        "--keep-refused", "--keep-after-violation"};

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

/// Sets cfg's node table and compose file from --layout and --compose.
void apply_layout(run_config& cfg, const std::string& layout,
                  const std::optional<std::string>& compose) {
    std::string default_compose = "docker/docker-compose.yml";
    if (layout == "chaos3") {
        cfg.nodes = docker_chaos::k_node_map;
    } else if (layout == "chaos5") {
        cfg.nodes = docker_chaos::chaos5_node_table(docker_chaos::chaos5_cluster_name());
    } else if (layout == "cairn") {
        cfg.nodes = docker_chaos::cairn_node_table(docker_chaos::cairn_run_name());
        default_compose = "docker/cairn/cairn-compose.yml";
    } else {
        throw usage_error("--layout must be chaos3, chaos5 or cairn");
    }
    cfg.compose_file = fs::absolute(compose.value_or(default_compose));
}

/// Whether the host can reach every node itself, as a cluster it starts
/// must be reached; a node reached by service name is inside a stack.
auto reachable_from_host(const run_config& cfg) -> bool {
    return std::ranges::all_of(cfg.nodes,
                               [](const auto& n) { return n.second.host == "localhost"; });
}

void print(const boost::json::value& v) {
    std::cout << boost::json::serialize(v) << '\n';
}

auto cmd_init(const cli& c) -> int {
    const auto paths = paths_of(c);
    fs::create_directories(paths.dir);
    if (fs::exists(paths.config())) {
        throw usage_error(paths.config().string() + " exists; use a new run directory");
    }
    run_config cfg;
    apply_layout(cfg, opt(c, "--layout").value_or("chaos3"), opt(c, "--compose"));
    if (c.flags.contains("--start") && !reachable_from_host(cfg)) {
        throw usage_error("this layout's cluster is started from the host by its compose file");
    }
    cfg.seed = opt(c, "--seed") ? std::stoull(*opt(c, "--seed"))
                                : static_cast<std::uint64_t>(real_mono_ms()) ^
                                      static_cast<std::uint64_t>(::getpid());
    cfg.election_timeout_max_ms = std::stoll(opt(c, "--election-timeout-max-ms").value_or("300"));
    cfg.image = opt(c, "--image").value_or(cfg.image);
    cfg.image_id = opt(c, "--image-id", "KYTHIRA_IMAGE_ID").value_or("");
    if (cfg.image_id.empty()) {
        cfg.image_id = image_id(cfg.image);
    }
    cfg.kythira_commit = kythira_commit();
    cfg.workload_binary =
        fs::absolute(opt(c, "--workload-binary").value_or((exe_dir() / "chaos_workload").string()));
    write_json_file(paths.config(), to_json(cfg));
    use_image(cfg);
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
    use_image(cfg);
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

/// The run's config as a replay must use it: on the cluster --layout
/// names, if given, so a run from inside a stack replays on the host.
auto replay_config(const run_paths& paths, const cli& c) -> run_config {
    auto cfg = load_config(paths);
    if (auto layout = opt(c, "--layout")) {
        apply_layout(cfg, *layout, opt(c, "--compose"));
    }
    if (!reachable_from_host(cfg)) {
        throw usage_error(
            "this run's nodes are reached inside its stack; replay it on the host with "
            "--layout chaos3");
    }
    // A run recorded in the tool server names the workload binary at its
    // path in that image.
    if (auto w = opt(c, "--workload-binary")) {
        cfg.workload_binary = fs::absolute(*w);
    } else if (!fs::exists(cfg.workload_binary)) {
        cfg.workload_binary = exe_dir() / "chaos_workload";
    }
    check_image(cfg, c);
    return cfg;
}

auto cmd_replay(const cli& c) -> int {
    const auto paths = paths_of(c);
    const auto cfg = replay_config(paths, c);
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
    const auto cfg = replay_config(paths, c);
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

auto read_file(const fs::path& p) -> std::string {
    std::ifstream in(p);
    if (!in) {
        throw usage_error("cannot read " + p.string());
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_new_file(const fs::path& p, const std::string& text) {
    if (fs::exists(p)) {
        throw usage_error(p.string() + " exists; pass another --id or remove it");
    }
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::trunc);
    out << text;
    if (!out) {
        throw std::runtime_error("write to " + p.string() + " failed");
    }
}

auto current_year() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto ymd = std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(now)};
    return std::to_string(static_cast<int>(ymd.year()));
}

auto cmd_report(const cli& c) -> int {
    std::optional<run_paths> paths;
    if (auto d = opt(c, "--run-dir", "KYTHIRA_CHAOS_RUN_DIR")) {
        paths = run_paths{fs::absolute(*d)};
    }
    report::options o;
    o.minimized = c.flags.contains("--minimized");
    if (auto l = opt(c, "--log")) {
        o.source_log = *l;
    } else if (paths) {
        const auto minimized = paths->dir / "minimized.jsonl";
        o.minimized = o.minimized || fs::exists(minimized);
        o.source_log = (fs::exists(minimized) ? minimized : paths->schedule()).string();
    } else {
        throw usage_error("report needs --log or --run-dir");
    }
    if (paths && fs::exists(paths->config())) {
        o.image = load_config(*paths).image;
    }
    o.image = opt(c, "--image").value_or(o.image);
    o.id = opt(c, "--id").value_or("");
    o.kythira_commit = opt(c, "--kythira-commit").value_or("");
    o.cairn_version = opt(c, "--cairn-version").value_or(std::string(report::k_cairn_version));
    o.note = opt(c, "--note").value_or("");
    o.cut.keep_refused = c.flags.contains("--keep-refused");
    o.cut.keep_after_violation = c.flags.contains("--keep-after-violation");
    if (auto r = opt(c, "--replay-result")) {
        o.replay = report::parse_replay_result(read_file(*r));
        if (!o.replay) {
            throw usage_error(*r + " holds no chaos_ctl replay result");
        }
    }

    const auto calls = read_calls(o.source_log);
    if (calls.empty()) {
        throw usage_error(o.source_log + " holds no calls");
    }
    report::finding f;
    try {
        f = report::build(calls, o);
    } catch (const std::invalid_argument& e) {
        throw usage_error(e.what());
    }

    const fs::path root = fs::absolute(opt(c, "--source-dir").value_or(".")).lexically_normal();
    const std::string replay_rel = "tests/docker_chaos/findings/" + f.id + ".jsonl";
    const std::string test_rel = "tests/docker_chaos/findings/" + f.id + "_test.cpp";
    const fs::path doc = root / "doc" / "chaos-findings.md";
    std::string doc_text = fs::exists(doc) ? read_file(doc) : report::doc_header();
    if (doc_text.find("\n## " + f.id + ":") != std::string::npos) {
        throw usage_error(doc.string() + " already has finding " + f.id);
    }
    write_new_file(root / replay_rel, report::render_replay_file(f));
    write_new_file(root / test_rel, report::render_test(f, current_year()));
    // The format-check target covers tests/, so the draft is formatted with
    // the tree's own .clang-format when a clang-format is at hand.
    const auto clang_format = opt(c, "--clang-format", "CLANG_FORMAT").value_or("clang-format");
    const bool formatted = docker_chaos::os::real_exec(
                               {clang_format, "-i", "--style=file", (root / test_rel).string()})
                               .code == 0;
    doc_text += report::render_entry(f, replay_rel, test_rel);
    {
        std::ofstream out(doc, std::ios::trunc);
        out << doc_text;
    }

    boost::json::array inv;
    for (const auto& i : f.invariants) {
        inv.emplace_back(i);
    }
    print(boost::json::object{
        {"id", f.id},
        {"invariants", std::move(inv)},
        {"seq", f.violation->seq},
        {"calls_in_log", f.total_calls},
        {"calls_kept", f.schedule.calls.size()},
        {"cut", boost::json::object{{"after_violation", f.schedule.after_violation},
                                    {"read_only", f.schedule.read_only},
                                    {"refused", f.schedule.refused}}},
        {"attempts", f.attempts},
        {"replay_file", (root / replay_rel).string()},
        {"draft_test", (root / test_rel).string()},
        {"findings_doc", doc.string()},
        {"draft_formatted", formatted},
        {"committed", false}});
    return 0;
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
        if (c.command == "report") {
            return cmd_report(c);
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
