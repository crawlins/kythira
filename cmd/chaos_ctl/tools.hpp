// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// chaos_ctl's tools (.kiro/specs/cairn-chaos-testing/ Requirement 3.1,
// design §3): the doc's toolset, one function each, JSON arguments in and a
// JSON result out. call() wraps every tool in the ordering of design §3.3:
//
//   1. append the call to the schedule log, status `started`;
//   2. before a kill or restart, snapshot the node's /elections and /log;
//   3. execute;
//   4. run the Raft safety checker on the state the call left (for every
//      tool that changes the cluster);
//   5. append the outcome, with any violation.
//
// Everything a tool touches comes through `context`, so the unit tests run
// the tools over the CmdExecutor/HttpGet stubs and a stub process table.

#include "checkers/linearizability.hpp"
#include "checkers/liveness.hpp"
#include "checkers/raft_safety.hpp"
#include "cluster_checks.hpp"
#include "fault_control.hpp"
#include "harness.hpp"
#include "history.hpp"
#include "run_state.hpp"
#include "schedule_log.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chaos_ctl {

namespace checkers = docker_chaos::checkers;

/// Bad arguments, or a call the cluster's state does not allow.
struct tool_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// A tool this host cannot deliver (design §6), recorded as such and never
/// as a fault that took effect.
struct tool_unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct context {
    run_paths paths;
    run_config cfg;
    docker_chaos::ChaosCluster& cluster;
    /// Sends one fiu line-protocol command to a node.
    std::function<void(docker_chaos::ChaosNode&, const std::string& cmd)> fiu_send;
    process_ops procs;
    std::function<std::int64_t()> mono_ms = real_mono_ms;
    std::function<std::string()> wall = real_wall;
    /// docker/cairn/failpoints.allowlist.
    std::set<std::string> failpoints;
};

/// What one call accumulates besides its result.
struct call_state {
    std::int64_t seq{0};
    boost::json::array violations;
    boost::json::array notes;

    void note(const std::string& s) { notes.emplace_back(s); }
    void violation(boost::json::object v) {
        v["schedule_seq"] = seq;
        violations.emplace_back(std::move(v));
    }
};

using handler_fn =
    std::function<boost::json::value(context&, const boost::json::object&, call_state&)>;

struct tool_spec {
    std::string name;
    std::string summary;
    /// Changes the cluster, so the Raft safety checker runs after it.
    bool mutates{true};
    /// Kills or restarts the node in args["node"], so it is snapshotted first.
    bool snapshots_node{false};
    handler_fn handler;
    /// Fills in defaults before the call is logged, so the log holds exactly
    /// what ran and a replay runs the same thing.
    std::function<void(const context&, boost::json::object&, std::int64_t seq)> normalize;
};

// ── Argument helpers ─────────────────────────────────────────────────────────

namespace args {

inline auto has(const boost::json::object& a, std::string_view k) -> bool {
    const auto* v = a.if_contains(k);
    return v != nullptr && !v->is_null();
}

inline auto integer(const boost::json::object& a, std::string_view k) -> std::int64_t {
    const auto* v = a.if_contains(k);
    if (v == nullptr || !(v->is_int64() || v->is_uint64())) {
        throw tool_error("argument '" + std::string(k) + "' must be an integer");
    }
    return v->to_number<std::int64_t>();
}

inline auto integer_or(const boost::json::object& a, std::string_view k, std::int64_t fallback)
    -> std::int64_t {
    return has(a, k) ? integer(a, k) : fallback;
}

inline auto number(const boost::json::object& a, std::string_view k) -> double {
    const auto* v = a.if_contains(k);
    if (v == nullptr || !(v->is_number())) {
        throw tool_error("argument '" + std::string(k) + "' must be a number");
    }
    return v->to_number<double>();
}

inline auto string(const boost::json::object& a, std::string_view k) -> std::string {
    const auto* v = a.if_contains(k);
    if (v == nullptr || !v->is_string()) {
        throw tool_error("argument '" + std::string(k) + "' must be a string");
    }
    return std::string(v->as_string());
}

inline auto flag(const boost::json::object& a, std::string_view k) -> bool {
    const auto* v = a.if_contains(k);
    if (v == nullptr || v->is_null()) {
        return false;
    }
    if (!v->is_bool()) {
        throw tool_error("argument '" + std::string(k) + "' must be true or false");
    }
    return v->as_bool();
}

inline auto node(context& ctx, const boost::json::object& a) -> docker_chaos::ChaosNode& {
    const auto id = integer(a, "node");
    if (!ctx.cfg.nodes.contains(static_cast<int>(id))) {
        throw tool_error("no node " + std::to_string(id) + " in this run");
    }
    return ctx.cluster.node(static_cast<int>(id));
}

}  // namespace args

// ── Shared pieces ────────────────────────────────────────────────────────────

/// Every /elections body saved in this run's snapshots.
[[nodiscard]] inline auto saved_elections(const run_paths& paths)
    -> std::vector<boost::json::object> {
    std::vector<boost::json::object> out;
    if (!fs::exists(paths.snapshots())) {
        return out;
    }
    std::vector<fs::path> files;
    for (const auto& f : fs::directory_iterator(paths.snapshots())) {
        files.push_back(f.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        auto snap = read_json_file(f).as_object();
        if (const auto* e = snap.if_contains("elections"); e != nullptr && e->is_object()) {
            out.push_back(e->as_object());
        }
    }
    return out;
}

/// Saves the node's /elections, /status and /log before a kill or restart
/// loses them. Best effort: a node already down has nothing left to save,
/// and that is recorded.
inline void snapshot_node(context& ctx, docker_chaos::ChaosNode& n, call_state& st) {
    fs::create_directories(ctx.paths.snapshots());
    boost::json::object snap{{"node", n.id()}, {"seq", st.seq}, {"wall", ctx.wall()}};
    try {
        snap["elections"] = n.elections();
    } catch (const std::exception& e) {
        snap["elections"] = nullptr;
        snap["elections_error"] = e.what();
    }
    try {
        auto [status, ranges] = docker_chaos::read_node(n);
        snap["status"] = std::move(status);
        boost::json::array log;
        for (auto& r : ranges) {
            log.emplace_back(std::move(r));
        }
        snap["log"] = std::move(log);
    } catch (const std::exception& e) {
        snap["log_error"] = e.what();
    }
    if (snap.contains("elections_error") || snap.contains("log_error")) {
        st.note("snapshot of node " + std::to_string(n.id()) + " incomplete: node not readable");
    }
    char name[64];
    std::snprintf(name, sizeof(name), "%06lld-node%d.json", static_cast<long long>(st.seq), n.id());
    write_json_file(ctx.paths.snapshots() / name, snap);
}

/// Raft safety over every node now, plus the elections saved before kills.
inline auto run_raft_safety(context& ctx, call_state& st) -> boost::json::object {
    auto note = [&st](const std::string& s) { st.note(s); };
    auto r = checkers::raft_safety::check(
        docker_chaos::collect_node_states(ctx.cluster, note),
        docker_chaos::collect_elections(ctx.cluster, saved_elections(ctx.paths), note));
    for (const auto& v : r.violations) {
        st.violation(checkers::raft_safety::violation_json(v));
    }
    return {{"verdict", checkers::to_string(r.overall)},
            {"terms_with_a_leader", r.terms_with_a_leader},
            {"committed_entries_compared", r.committed_entries_compared},
            {"violations", r.violations.size()}};
}

/// The history the workload has written so far. A last line still being
/// written is left out.
[[nodiscard]] inline auto read_history(const run_paths& paths)
    -> std::vector<docker_chaos::history::event> {
    std::vector<docker_chaos::history::event> out;
    std::ifstream in(paths.history());
    if (!in) {
        return out;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t pos = 0;
    while (true) {
        const auto nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            break;
        }
        if (nl > pos) {
            out.push_back(
                docker_chaos::history::from_line(std::string_view(text).substr(pos, nl - pos)));
        }
        pos = nl + 1;
    }
    return out;
}

inline auto run_linearizability(context& ctx, call_state& st) -> boost::json::object {
    const auto h = read_history(ctx.paths);
    if (auto err = docker_chaos::history::structural_error(h); !err.empty()) {
        throw tool_error("workload history malformed: " + err);
    }
    auto r = checkers::linearizability::check(h);
    std::size_t ops = 0;
    for (const auto& k : r.keys) {
        ops += k.operations;
    }
    for (const auto* k : r.violations()) {
        auto v = checkers::linearizability::violation_json(*k);
        v["invariant"] = "linearizability";
        st.violation(std::move(v));
    }
    return {{"verdict", checkers::to_string(r.overall)},
            {"events", h.size()},
            {"operations", ops},
            {"keys", r.keys.size()},
            {"failed_operations", r.failed_operations},
            {"local_reads", r.local_reads.size()}};
}

inline auto run_liveness(context& ctx, call_state& st, unsigned k) -> boost::json::object {
    int writes = 0;
    const auto baseline = docker_chaos::highest_commit(ctx.cluster);
    const auto limit =
        checkers::liveness::bound(std::chrono::milliseconds{ctx.cfg.election_timeout_max_ms}, k);
    auto r = checkers::liveness::wait(
        [&ctx, &writes] { return docker_chaos::liveness_sample(ctx.cluster, writes); }, baseline,
        limit);
    if (r.overall == checkers::verdict::violation) {
        st.violation({{"invariant", "liveness"}, {"detail", r.detail}});
    }
    boost::json::object out{{"verdict", checkers::to_string(r.overall)},
                            {"limit_ms", r.limit.count()},
                            {"baseline_commit", r.baseline_commit},
                            {"detail", r.detail}};
    out["leader_at_ms"] = r.leader_at ? boost::json::value(r.leader_at->count()) : nullptr;
    out["commit_at_ms"] = r.commit_at ? boost::json::value(r.commit_at->count()) : nullptr;
    return out;
}

[[nodiscard]] inline auto find_leader(context& ctx) -> docker_chaos::ChaosNode& {
    for (auto* n : ctx.cluster.all_nodes()) {
        if (n->is_leader()) {
            return *n;
        }
    }
    throw tool_error("no node reports itself leader");
}

// ── Workload process ─────────────────────────────────────────────────────────

[[nodiscard]] inline auto workload_pid(context& ctx) -> int {
    if (!fs::exists(ctx.paths.workload())) {
        return 0;
    }
    const auto pid = static_cast<int>(
        read_json_file(ctx.paths.workload()).as_object().at("pid").to_number<std::int64_t>());
    return ctx.procs.alive(pid, ctx.cfg.workload_binary) ? pid : 0;
}

[[nodiscard]] inline auto count_lines(const fs::path& p) -> std::size_t {
    std::ifstream in(p);
    std::size_t n = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++n;
    }
    return n;
}

// ── The tools ────────────────────────────────────────────────────────────────

namespace tool {

inline auto failpoint(context& ctx, const boost::json::object& a, call_state& /*st*/)
    -> boost::json::value {
    auto& n = args::node(ctx, a);
    const auto name = args::string(a, "name");
    if (!ctx.failpoints.contains(name)) {
        throw tool_error("failpoint '" + name + "' is not in docker/cairn/failpoints.allowlist");
    }
    const auto mode = args::string(a, "mode");
    std::string cmd;
    if (mode == "always") {
        cmd = docker_chaos::fiu::build_enable_always_cmd(name);
    } else if (mode == "random") {
        const auto p = args::number(a, "probability");
        if (std::isnan(p) || p <= 0.0 || p > 1.0) {
            throw tool_error("probability must be in (0, 1]");
        }
        cmd = docker_chaos::fiu::build_enable_random_cmd(name, p);
    } else if (mode == "once") {
        cmd = docker_chaos::fiu::build_enable_once_cmd(name);
    } else if (mode == "off") {
        cmd = docker_chaos::fiu::build_disable_cmd(name);
    } else {
        throw tool_error("mode must be always, random, once or off");
    }
    ctx.fiu_send(n, cmd);
    return boost::json::object{{"node", n.id()}, {"name", name}, {"mode", mode}};
}

/// {"groups": [[1], [2, 3]]} cuts every link between groups, replacing any
/// partition already in place; {"heal": true} removes it.
inline auto partition(context& ctx, const boost::json::object& a, call_state& /*st*/)
    -> boost::json::value {
    auto nodes = ctx.cluster.all_nodes();
    if (args::flag(a, "heal")) {
        for (auto* n : nodes) {
            n->unpartition();
        }
        return boost::json::object{{"healed", true}};
    }
    const auto* g = a.if_contains("groups");
    if (g == nullptr || !g->is_array() || g->as_array().size() < 2) {
        throw tool_error("groups must list at least two groups of node ids, or pass heal");
    }
    std::map<int, std::size_t> group_of;
    std::size_t gi = 0;
    for (const auto& group : g->as_array()) {
        if (!group.is_array() || group.as_array().empty()) {
            throw tool_error("each group must be a non-empty list of node ids");
        }
        for (const auto& id : group.as_array()) {
            if (!id.is_int64() && !id.is_uint64()) {
                throw tool_error("node ids must be integers");
            }
            const auto nid = static_cast<int>(id.to_number<std::int64_t>());
            if (!ctx.cfg.nodes.contains(nid)) {
                throw tool_error("no node " + std::to_string(nid) + " in this run");
            }
            if (!group_of.emplace(nid, gi).second) {
                throw tool_error("node " + std::to_string(nid) + " is in two groups");
            }
        }
        ++gi;
    }
    if (group_of.size() != ctx.cfg.nodes.size()) {
        throw tool_error("every node must be in exactly one group");
    }
    std::map<int, std::string> ip;
    for (auto* n : nodes) {
        ip[n->id()] = n->container_ip();
        if (ip[n->id()].empty()) {
            throw tool_error("node " + std::to_string(n->id()) +
                             " has no address (is it running?)");
        }
    }
    for (auto* n : nodes) {
        n->unpartition();
    }
    for (auto* n : nodes) {
        std::vector<std::string> others;
        for (const auto& [id, gid] : group_of) {
            if (gid != group_of.at(n->id())) {
                others.push_back(ip.at(id));
            }
        }
        n->partition_from(others);
    }
    return boost::json::object{{"groups", *g}};
}

inline auto delay(context& ctx, const boost::json::object& a, call_state& /*st*/)
    -> boost::json::value {
    auto& n = args::node(ctx, a);
    if (args::flag(a, "clear")) {
        n.clear_tc_netem();
        return boost::json::object{{"node", n.id()}, {"cleared", true}};
    }
    const auto ms = args::integer(a, "delay_ms");
    const auto jitter = args::integer_or(a, "jitter_ms", 0);
    if (ms < 0 || ms > 60'000 || jitter < 0 || jitter > ms) {
        throw tool_error("delay_ms must be in [0, 60000] and jitter_ms in [0, delay_ms]");
    }
    n.apply_delay(static_cast<int>(ms), static_cast<int>(jitter));
    return boost::json::object{{"node", n.id()},
                               {"delay_ms", ms},
                               {"jitter_ms", jitter},
                               {"replaces", "any delay or drop"}};
}

inline auto drop(context& ctx, const boost::json::object& a, call_state& /*st*/)
    -> boost::json::value {
    auto& n = args::node(ctx, a);
    if (args::flag(a, "clear")) {
        n.clear_tc_netem();
        return boost::json::object{{"node", n.id()}, {"cleared", true}};
    }
    const auto pct = args::number(a, "loss_pct");
    try {
        n.apply_loss(pct);
    } catch (const std::invalid_argument& e) {
        throw tool_error(e.what());
    }
    return boost::json::object{
        {"node", n.id()}, {"loss_pct", pct}, {"replaces", "any delay or drop"}};
}

inline auto kill(context& ctx, const boost::json::object& a, call_state& /*st*/)
    -> boost::json::value {
    auto& n = args::node(ctx, a);
    n.kill();
    return boost::json::object{{"node", n.id()}};
}

/// {"node": 1} freezes the node; {"node": 1, "resume": true} thaws it.
inline auto pause(context& ctx, const boost::json::object& a, call_state& /*st*/)
    -> boost::json::value {
    auto& n = args::node(ctx, a);
    if (args::flag(a, "resume")) {
        n.unpause();
        return boost::json::object{{"node", n.id()}, {"paused", false}};
    }
    try {
        n.pause();
    } catch (const docker_chaos::fault_unsupported& e) {
        throw tool_unsupported(e.what());
    }
    return boost::json::object{{"node", n.id()}, {"paused", true}};
}

inline auto restart(context& ctx, const boost::json::object& a, call_state& /*st*/)
    -> boost::json::value {
    auto& n = args::node(ctx, a);
    n.restart(true, std::chrono::seconds{30});
    return boost::json::object{{"node", n.id()}, {"healthy", true}};
}

inline auto clock_skew(context& /*ctx*/, const boost::json::object& /*a*/, call_state& /*st*/)
    -> boost::json::value {
    throw tool_unsupported(
        "fault.clock_skew: containers share the host's clock and setting it needs CAP_SYS_TIME, "
        "which rootless Podman cannot grant (design section 6)");
}

inline auto status(context& ctx, const boost::json::object& /*a*/, call_state& /*st*/)
    -> boost::json::value {
    boost::json::array nodes;
    boost::json::value leader = nullptr;
    for (auto* n : ctx.cluster.all_nodes()) {
        boost::json::object row{{"id", n->id()}, {"container", n->container()}};
        try {
            auto s = n->status();
            if (s.at("role").as_string() == "leader") {
                leader = n->id();
            }
            row["reachable"] = true;
            row["status"] = std::move(s);
        } catch (const std::exception& e) {
            row["reachable"] = false;
            row["error"] = e.what();
        }
        nodes.emplace_back(std::move(row));
    }
    return boost::json::object{{"nodes", std::move(nodes)}, {"leader", leader}};
}

inline auto add_member(context& ctx, const boost::json::object& a, call_state& /*st*/)
    -> boost::json::value {
    const auto id = static_cast<int>(args::integer(a, "node"));
    if (!ctx.cfg.nodes.contains(id)) {
        throw tool_error("no node " + std::to_string(id) + " in this run");
    }
    auto& leader = find_leader(ctx);
    auto r = leader.membership_add(id);
    r["leader"] = leader.id();
    return r;
}

inline auto remove_member(context& ctx, const boost::json::object& a, call_state& /*st*/)
    -> boost::json::value {
    const auto id = static_cast<int>(args::integer(a, "node"));
    if (!ctx.cfg.nodes.contains(id)) {
        throw tool_error("no node " + std::to_string(id) + " in this run");
    }
    auto& leader = find_leader(ctx);
    auto r = leader.membership_remove(id);
    r["leader"] = leader.id();
    return r;
}

inline void normalize_workload_start(const context& ctx, boost::json::object& a, std::int64_t seq) {
    if (!args::has(a, "seed")) {
        a["seed"] = ctx.cfg.seed + static_cast<std::uint64_t>(seq);
    }
    if (!args::has(a, "concurrency")) {
        a["concurrency"] = 3;
    }
    if (!args::has(a, "rate")) {
        a["rate"] = 30;
    }
    if (!args::has(a, "keys")) {
        a["keys"] = 3;
    }
    if (!args::has(a, "values")) {
        a["values"] = 5;
    }
}

inline auto workload_start(context& ctx, const boost::json::object& a, call_state& /*st*/)
    -> boost::json::value {
    if (workload_pid(ctx) != 0) {
        throw tool_error("a workload is already running; stop it first");
    }
    // One workload per run: a second would number its processes from 0
    // again, and the two histories would not read as one.
    if (count_lines(ctx.paths.history()) != 0) {
        throw tool_error("this run already has a workload history; one workload per run");
    }
    const auto concurrency = args::integer(a, "concurrency");
    const auto rate = args::integer(a, "rate");
    const auto keys = args::integer(a, "keys");
    const auto values = args::integer(a, "values");
    if (concurrency < 1 || concurrency > 16 || rate < 1 || rate > 500 || keys < 1 || keys > 64 ||
        values < 1 || values > 1000) {
        throw tool_error(
            "concurrency must be in [1, 16], rate in [1, 500], keys in [1, 64], values in [1, "
            "1000]");
    }
    std::string nodes;
    for (const auto& [id, p] : ctx.cfg.nodes) {
        nodes += (nodes.empty() ? "" : ",") + std::to_string(id) +
                 "=localhost:" + std::to_string(p.http_port);
    }
    std::vector<std::string> env{
        "WORKLOAD_NODES=" + nodes,
        "WORKLOAD_DURATION_S=0",
        "WORKLOAD_SEED=" + std::to_string(args::integer(a, "seed")),
        "WORKLOAD_CONCURRENCY=" + std::to_string(concurrency),
        "WORKLOAD_RATE=" + std::to_string(rate),
        "WORKLOAD_KEYS=" + std::to_string(keys),
        "WORKLOAD_VALUES=" + std::to_string(values),
        "WORKLOAD_TIMEOUT_MS=2000",
    };
    if (args::has(a, "mix")) {
        env.push_back("WORKLOAD_MIX=" + args::string(a, "mix"));
    }
    const int pid = ctx.procs.spawn({ctx.cfg.workload_binary}, env, ctx.paths.history().string(),
                                    ctx.paths.workload_log().string());
    write_json_file(ctx.paths.workload(),
                    boost::json::object{{"pid", pid}, {"started_mono_ms", ctx.mono_ms()}});
    return boost::json::object{{"pid", pid}, {"seed", a.at("seed")}};
}

inline auto workload_stop(context& ctx, const boost::json::object& /*a*/, call_state& st)
    -> boost::json::value {
    const int pid = workload_pid(ctx);
    bool forced = false;
    if (pid == 0) {
        st.note("no workload was running");
    } else {
        // SIGTERM lets each worker finish the operation in flight (two
        // seconds at most, the request timeout), so the history ends on
        // completions. SIGKILL only if that fails.
        ctx.procs.signal(pid, SIGTERM);
        int waited = 0;
        while (ctx.procs.alive(pid, ctx.cfg.workload_binary) && waited < 15'000) {
            ctx.procs.sleep(std::chrono::milliseconds{100});
            waited += 100;
        }
        if (ctx.procs.alive(pid, ctx.cfg.workload_binary)) {
            ctx.procs.signal(pid, SIGKILL);
            forced = true;
        }
    }
    fs::remove(ctx.paths.workload());
    return boost::json::object{
        {"stopped", pid != 0}, {"forced", forced}, {"events", count_lines(ctx.paths.history())}};
}

inline auto workload_history(context& ctx, const boost::json::object& a, call_state& /*st*/)
    -> boost::json::value {
    const auto tail = args::integer_or(a, "tail", 20);
    if (tail < 0 || tail > 200) {
        throw tool_error("tail must be in [0, 200]");
    }
    auto h = read_history(ctx.paths);
    boost::json::array lines;
    const auto from =
        h.size() > static_cast<std::size_t>(tail) ? h.size() - static_cast<std::size_t>(tail) : 0;
    for (auto i = from; i < h.size(); ++i) {
        lines.emplace_back(docker_chaos::history::to_json(h[i]));
    }
    return boost::json::object{{"running", workload_pid(ctx) != 0},
                               {"events", h.size()},
                               {"path", ctx.paths.history().string()},
                               {"tail", std::move(lines)}};
}

/// {"checks": ["raft_safety", "linearizability", "liveness"]}. Liveness is
/// measured from now, so heal first (run.reset) unless the point is to see
/// whether the cluster makes progress under the faults in place.
inline auto check_run(context& ctx, const boost::json::object& a, call_state& st)
    -> boost::json::value {
    std::vector<std::string> checks{"raft_safety", "linearizability"};
    if (const auto* c = a.if_contains("checks"); c != nullptr && !c->is_null()) {
        if (!c->is_array()) {
            throw tool_error("checks must be a list");
        }
        checks.clear();
        for (const auto& v : c->as_array()) {
            if (!v.is_string()) {
                throw tool_error("checks must be a list of names");
            }
            checks.emplace_back(v.as_string());
        }
    }
    boost::json::object out;
    for (const auto& c : checks) {
        if (c == "raft_safety") {
            out["raft_safety"] = run_raft_safety(ctx, st);
        } else if (c == "linearizability") {
            if (workload_pid(ctx) != 0) {
                st.note("workload still running: linearizability checked the history so far");
            }
            out["linearizability"] = run_linearizability(ctx, st);
        } else if (c == "liveness") {
            const auto k = args::integer_or(a, "k", 2 * checkers::liveness::default_k);
            if (k < 1 || k > 100) {
                throw tool_error("k must be in [1, 100]");
            }
            out["liveness"] = run_liveness(ctx, st, static_cast<unsigned>(k));
        } else {
            throw tool_error("unknown check '" + c + "'");
        }
    }
    return out;
}

/// Undoes every fault: unpause, clear tc, flush iptables, disable every
/// failpoint, restart any node that is down.
inline auto reset(context& ctx, const boost::json::object& /*a*/, call_state& st)
    -> boost::json::value {
    for (auto* n : ctx.cluster.all_nodes()) {
        if (!n->is_healthy()) {
            snapshot_node(ctx, *n, st);
        }
    }
    docker_chaos::heal_all(ctx.cluster, [&st](const std::string& s) { st.note(s); });
    return boost::json::object{{"healed", true}};
}

}  // namespace tool

/// The toolset of Requirement 3.1, in its order.
[[nodiscard]] inline auto tools() -> const std::vector<tool_spec>& {
    static const std::vector<tool_spec> all{
        {"fault.failpoint",
         "Arm or disarm a libfiu failpoint on one node. Args: node, name (from "
         "failpoints.allowlist), mode (always|random|once|off), probability (random only).",
         true,
         false,
         tool::failpoint,
         {}},
        {"fault.partition",
         "Cut the network between groups of nodes, replacing any partition in place. Args: "
         "groups ([[1],[2,3]], every node exactly once), or heal: true.",
         true,
         false,
         tool::partition,
         {}},
        {"fault.delay",
         "Delay one node's outbound packets (tc netem); replaces any delay or drop on it. Args: "
         "node, delay_ms, jitter_ms; or node, clear: true.",
         true,
         false,
         tool::delay,
         {}},
        {"fault.drop",
         "Drop a percentage of one node's outbound packets (tc netem); replaces any delay or "
         "drop on it. Args: node, loss_pct; or node, clear: true.",
         true,
         false,
         tool::drop,
         {}},
        {"fault.kill",
         "Kill one node's container (SIGKILL). Args: node.",
         true,
         true,
         tool::kill,
         {}},
        {"fault.pause",
         "Freeze one node's processes; resume: true thaws them. Args: node, resume.",
         true,
         false,
         tool::pause,
         {}},
        {"fault.restart",
         "Start a stopped node and wait for it to be healthy. Args: node.",
         true,
         true,
         tool::restart,
         {}},
        {"fault.clock_skew",
         "Unsupported in the Docker phase; always reports why.",
         false,
         false,
         tool::clock_skew,
         {}},
        {"cluster.status",
         "Every node's /status, and the leader if one reports itself.",
         false,
         false,
         tool::status,
         {}},
        {"cluster.add_member",
         "Ask the leader to add a node to the configuration. Args: node.",
         true,
         false,
         tool::add_member,
         {}},
        {"cluster.remove_member",
         "Ask the leader to remove a node from the configuration. Args: node.",
         true,
         false,
         tool::remove_member,
         {}},
        {"workload.start",
         "Start the workload (reads, writes, CAS). Args, all optional: seed, concurrency, rate, "
         "keys, values, mix.",
         false, false, tool::workload_start, tool::normalize_workload_start},
        {"workload.stop",
         "Stop the workload; each worker finishes its operation in flight.",
         false,
         false,
         tool::workload_stop,
         {}},
        {"workload.history",
         "How many events the history holds, and the last few. Args: tail.",
         false,
         false,
         tool::workload_history,
         {}},
        {"check.run",
         "Run checkers: raft_safety and linearizability by default; liveness on request. Args: "
         "checks, k (liveness bound in election timeouts).",
         false,
         false,
         tool::check_run,
         {}},
        {"run.reset",
         "Heal every fault: unpause, clear netem and iptables, disarm failpoints, restart any "
         "node that is down.",
         true,
         false,
         tool::reset,
         {}},
    };
    return all;
}

[[nodiscard]] inline auto find_tool(std::string_view name) -> const tool_spec* {
    for (const auto& t : tools()) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

/// One tool call, in design §3.3's order. The caller holds the run lock.
/// Throws tool_error for a name that is not a tool; every other failure is
/// an outcome in the returned object and the log.
inline auto call(context& ctx, std::string_view name, boost::json::object a)
    -> boost::json::object {
    const auto* spec = find_tool(name);
    if (spec == nullptr) {
        throw tool_error("unknown tool '" + std::string(name) + "'");
    }
    schedule_log log(ctx.paths.schedule().string());
    call_state st;
    st.seq = next_seq(read_lines(log.path()));
    if (spec->normalize) {
        spec->normalize(ctx, a, st.seq);
    }

    // 1. On record before anything happens.
    log.append({{"seq", st.seq},
                {"status", "started"},
                {"tool", spec->name},
                {"args", a},
                {"wall", ctx.wall()},
                {"mono_ms", ctx.mono_ms()},
                {"seed", ctx.cfg.seed},
                {"kythira_commit", ctx.cfg.kythira_commit},
                {"image_id", ctx.cfg.image_id}});

    std::string status = "ok";
    boost::json::value result;
    std::string error;
    try {
        // 2. Save what a kill or restart would lose.
        if (spec->snapshots_node) {
            snapshot_node(ctx, args::node(ctx, a), st);
        }
        // 3.
        result = spec->handler(ctx, a, st);
    } catch (const tool_unsupported& e) {
        status = "unsupported";
        error = e.what();
    } catch (const std::exception& e) {
        status = "error";
        error = e.what();
    }

    // 4. A failed call may still have changed the cluster.
    boost::json::value safety = nullptr;
    if (spec->mutates && status != "unsupported") {
        safety = run_raft_safety(ctx, st);
    }

    // 5.
    boost::json::object done{{"seq", st.seq},
                             {"status", status},
                             {"result", result},
                             {"raft_safety", safety},
                             {"violations", st.violations},
                             {"notes", st.notes},
                             {"wall", ctx.wall()},
                             {"mono_ms", ctx.mono_ms()}};
    if (!error.empty()) {
        done["error"] = error;
    }
    log.append(done);
    done.erase("wall");
    done.erase("mono_ms");
    done["tool"] = spec->name;
    return done;
}

/// Reads docker/cairn/failpoints.allowlist: one name per line, '#' comments.
[[nodiscard]] inline auto load_failpoints(const fs::path& p) -> std::set<std::string> {
    std::ifstream in(p);
    if (!in) {
        throw std::runtime_error("cannot read failpoint allowlist " + p.string());
    }
    std::set<std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        out.insert(line);
    }
    return out;
}

}  // namespace chaos_ctl
