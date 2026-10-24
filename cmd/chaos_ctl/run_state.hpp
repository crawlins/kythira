// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A chaos_ctl run lives in one directory, because every tool call is a new
// chaos_ctl process (design §3.1: the MCP adapter runs one subcommand per
// tool call) and anything a later call needs must be on disk:
//
//   run.json        the run's configuration: node table, compose file,
//                   seed, image and commit (written by `chaos_ctl init`)
//   schedule.jsonl  the schedule log (schedule_log.hpp)
//   snapshots/      /elections, /status and /log of a node, saved before
//                   each kill or restart (Requirement 3.3, design §1.1)
//   workload.json   the running workload's pid, while there is one
//   history.jsonl   the workload's history (chaos_workload's stdout)
//   workload.log    the workload's stderr
//   lock            flock'd for the length of each call, so calls from
//                   several workers are serialized into one total order

#include "harness.hpp"

#include <boost/json.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern char** environ;  // NOLINT(readability-redundant-declaration)

namespace chaos_ctl {

namespace fs = std::filesystem;

struct run_config {
    std::string compose_file{"docker/docker-compose.yml"};
    std::map<int, docker_chaos::NodePorts> nodes{docker_chaos::k_node_map};
    std::uint64_t seed{0};
    std::string image{"kythira-chaos-node:dev"};
    std::string image_id;
    std::string kythira_commit;
    /// ELECTION_TIMEOUT_MAX_MS of the compose file; the liveness bound is a
    /// multiple of it.
    std::int64_t election_timeout_max_ms{300};
    /// chaos_workload, which workload.start runs.
    std::string workload_binary;
};

[[nodiscard]] inline auto to_json(const run_config& c) -> boost::json::object {
    boost::json::array nodes;
    for (const auto& [id, p] : c.nodes) {
        nodes.push_back(boost::json::object{{"id", id},
                                            {"rpc_port", p.rpc_port},
                                            {"http_port", p.http_port},
                                            {"fiu_port", p.fiu_port},
                                            {"container", p.container_name}});
    }
    return {{"compose_file", c.compose_file},
            {"nodes", std::move(nodes)},
            {"seed", c.seed},
            {"image", c.image},
            {"image_id", c.image_id},
            {"kythira_commit", c.kythira_commit},
            {"election_timeout_max_ms", c.election_timeout_max_ms},
            {"workload_binary", c.workload_binary}};
}

[[nodiscard]] inline auto config_from_json(const boost::json::object& o) -> run_config {
    run_config c;
    c.compose_file = std::string(o.at("compose_file").as_string());
    c.nodes.clear();
    for (const auto& v : o.at("nodes").as_array()) {
        const auto& n = v.as_object();
        c.nodes.emplace(
            static_cast<int>(n.at("id").to_number<std::int64_t>()),
            docker_chaos::NodePorts{static_cast<int>(n.at("rpc_port").to_number<std::int64_t>()),
                                    static_cast<int>(n.at("http_port").to_number<std::int64_t>()),
                                    static_cast<int>(n.at("fiu_port").to_number<std::int64_t>()),
                                    std::string(n.at("container").as_string())});
    }
    if (c.nodes.empty()) {
        throw std::runtime_error("run.json: no nodes");
    }
    c.seed = o.at("seed").to_number<std::uint64_t>();
    c.image = std::string(o.at("image").as_string());
    c.image_id = std::string(o.at("image_id").as_string());
    c.kythira_commit = std::string(o.at("kythira_commit").as_string());
    c.election_timeout_max_ms = o.at("election_timeout_max_ms").to_number<std::int64_t>();
    c.workload_binary = std::string(o.at("workload_binary").as_string());
    return c;
}

/// The files of one run directory.
struct run_paths {
    fs::path dir;

    [[nodiscard]] auto config() const -> fs::path { return dir / "run.json"; }
    [[nodiscard]] auto schedule() const -> fs::path { return dir / "schedule.jsonl"; }
    [[nodiscard]] auto snapshots() const -> fs::path { return dir / "snapshots"; }
    [[nodiscard]] auto workload() const -> fs::path { return dir / "workload.json"; }
    [[nodiscard]] auto history() const -> fs::path { return dir / "history.jsonl"; }
    [[nodiscard]] auto workload_log() const -> fs::path { return dir / "workload.log"; }
    [[nodiscard]] auto lock() const -> fs::path { return dir / "lock"; }
};

[[nodiscard]] inline auto read_json_file(const fs::path& p) -> boost::json::value {
    std::ifstream in(p);
    if (!in) {
        throw std::runtime_error("cannot read " + p.string());
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return boost::json::parse(ss.str());
}

/// Writes through a temporary file and a rename, so a reader never sees a
/// half-written file.
inline void write_json_file(const fs::path& p, const boost::json::value& v) {
    const auto tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot write " + tmp);
        }
        out << boost::json::serialize(v) << '\n';
    }
    fs::rename(tmp, p);
}

[[nodiscard]] inline auto load_config(const run_paths& paths) -> run_config {
    return config_from_json(read_json_file(paths.config()).as_object());
}

/// Holds an exclusive flock on the run's lock file for its lifetime.
class run_lock {
public:
    explicit run_lock(const run_paths& paths) {
        _fd = ::open(paths.lock().c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (_fd < 0) {
            throw std::runtime_error("cannot open " + paths.lock().string());
        }
        if (::flock(_fd, LOCK_EX) != 0) {
            ::close(_fd);
            throw std::runtime_error("cannot lock " + paths.lock().string());
        }
    }
    ~run_lock() {
        if (_fd >= 0) {
            ::flock(_fd, LOCK_UN);
            ::close(_fd);
        }
    }
    run_lock(const run_lock&) = delete;
    run_lock& operator=(const run_lock&) = delete;

private:
    int _fd{-1};
};

// ── Clocks ───────────────────────────────────────────────────────────────────

/// CLOCK_MONOTONIC in milliseconds: steady_clock on Linux, shared by every
/// process on the host.
[[nodiscard]] inline auto real_mono_ms() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// UTC wall clock as RFC 3339 with milliseconds.
[[nodiscard]] inline auto real_wall() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
    ::gmtime_r(&secs, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[40];
    std::snprintf(out, sizeof(out), "%s.%03dZ", buf, static_cast<int>(ms.count()));
    return out;
}

// ── Processes (the workload runs as its own process) ────────────────────────

struct process_ops {
    /// Starts argv detached in its own session, with `env` added to this
    /// process's environment, stdout to `out` and stderr to `err`. Returns
    /// its pid.
    std::function<int(const std::vector<std::string>& argv, const std::vector<std::string>& env,
                      const std::string& out, const std::string& err)>
        spawn;
    /// Whether `pid` is a live process running `binary` (a zombie, or a pid
    /// reused by something else, is not).
    std::function<bool(int pid, const std::string& binary)> alive;
    std::function<void(int pid, int sig)> signal;
    std::function<void(std::chrono::milliseconds)> sleep;
};

namespace detail {

inline auto real_spawn(const std::vector<std::string>& argv, const std::vector<std::string>& env,
                       const std::string& out, const std::string& err) -> int {
    if (argv.empty()) {
        throw std::invalid_argument("spawn: empty argv");
    }
    std::vector<std::string> envs;
    for (char** e = environ; *e != nullptr; ++e) {
        envs.emplace_back(*e);
    }
    envs.insert(envs.end(), env.begin(), env.end());
    std::vector<char*> cargv;
    for (const auto& a : argv) {
        cargv.push_back(
            const_cast<char*>(a.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }
    cargv.push_back(nullptr);
    std::vector<char*> cenv;
    for (const auto& e : envs) {
        cenv.push_back(
            const_cast<char*>(e.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }
    cenv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        throw std::runtime_error("spawn: fork failed");
    }
    if (pid == 0) {
        ::setsid();
        const int o = ::open(out.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
        const int e = ::open(err.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
        const int in = ::open("/dev/null", O_RDONLY);
        if (o < 0 || e < 0 || in < 0) {
            ::_exit(127);
        }
        ::dup2(in, STDIN_FILENO);
        ::dup2(o, STDOUT_FILENO);
        ::dup2(e, STDERR_FILENO);
        ::execve(cargv[0], cargv.data(), cenv.data());
        ::_exit(127);
    }
    return static_cast<int>(pid);
}

inline auto real_alive(int pid, const std::string& binary) -> bool {
    if (pid <= 0 || ::kill(pid, 0) != 0) {
        return false;
    }
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(stat, line);
    // Field 3, after the parenthesised command name.
    const auto close = line.rfind(')');
    if (close == std::string::npos || close + 2 >= line.size() || line[close + 2] == 'Z') {
        return false;
    }
    std::ifstream cmdline("/proc/" + std::to_string(pid) + "/cmdline");
    std::string argv0;
    std::getline(cmdline, argv0, '\0');
    return fs::path(argv0).filename() == fs::path(binary).filename();
}

}  // namespace detail

[[nodiscard]] inline auto real_process_ops() -> process_ops {
    return {.spawn = detail::real_spawn,
            .alive = detail::real_alive,
            .signal = [](int pid, int sig) { ::kill(pid, sig); },
            .sleep = [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); }};
}

}  // namespace chaos_ctl
