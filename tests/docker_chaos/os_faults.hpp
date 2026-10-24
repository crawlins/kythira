// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <sys/wait.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace docker_chaos::os {

// ── Result and executor types ─────────────────────────────────────────────────

struct CmdResult {
    int code{0};
    std::string out;
};

using CmdExecutor = std::function<CmdResult(const std::vector<std::string>&)>;

// Runs argv via popen, returning stdout+stderr merged, and the exit code.
inline CmdResult real_exec(const std::vector<std::string>& argv) {
    std::string cmd;
    for (const auto& arg : argv) {
        cmd += '\'';
        for (char c : arg) {
            if (c == '\'') {
                cmd += "'\\''";
            } else {
                cmd += c;
            }
        }
        cmd += "' ";
    }
    cmd += "2>&1";

    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (fp == nullptr) {
        return {-1, "popen failed"};
    }

    std::string output;
    std::array<char, 256> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr) {
        output += buf.data();
    }
    int status = ::pclose(fp);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {code, std::move(output)};
}

// Runs a command and throws std::runtime_error on non-zero exit.
inline std::string checked_exec(const CmdExecutor& exec, const std::vector<std::string>& argv) {
    auto [code, out] = exec(argv);
    if (code != 0) {
        std::string joined;
        for (const auto& a : argv) {
            joined += a;
            joined += ' ';
        }
        throw std::runtime_error("command failed (exit " + std::to_string(code) + "): " + joined +
                                 "\n" + out);
    }
    return out;
}

// Runs a command, ignoring errors (for cleanup calls that may legitimately fail).
inline void try_exec(const CmdExecutor& exec, const std::vector<std::string>& argv) {
    exec(argv);
}

// ── Container runtime (env-var controlled) ────────────────────────────────────

// Returns the container runtime CLI name from $KYTHIRA_CONTAINER_RUNTIME,
// defaulting to "docker".  Set the variable to "podman" to use Podman.
inline const std::string& container_runtime() {
    static const std::string rt = [] {
        const char* e = std::getenv("KYTHIRA_CONTAINER_RUNTIME");
        return std::string{(e != nullptr && *e != '\0') ? e : "docker"};
    }();
    return rt;
}

// Returns the argv prefix for compose commands.
// $KYTHIRA_COMPOSE_COMMAND set (e.g. "podman-compose") → [that token]
// unset or empty                                        → [runtime, "compose"]
inline std::vector<std::string> compose_prefix() {
    const char* e = std::getenv("KYTHIRA_COMPOSE_COMMAND");
    if (e != nullptr && *e != '\0') {
        return {std::string{e}};
    }
    return {container_runtime(), "compose"};
}

// ── Command-vector builders (pure — testable without Docker) ──────────────────

inline std::vector<std::string> tc_netem_cmd(const std::string& container, const std::string& loss,
                                             const std::string& delay, const std::string& corrupt) {
    const auto& rt = container_runtime();
    return {rt,     "exec",  container, "tc", "qdisc", "replace", "dev",     "eth0",
            "root", "netem", "loss",    loss, "delay", delay,     "corrupt", corrupt};
}

// One `tc qdisc replace` each for the two shapes a chaos tool names on its
// own (cairn-chaos-testing Requirement 2.3): delay with optional jitter, and
// loss alone. Both replace whatever qdisc is there, so they do not stack with
// each other or with tc_netem_cmd; tc_clear_cmd undoes any of them.
inline std::vector<std::string> tc_delay_cmd(const std::string& container, int delay_ms,
                                             int jitter_ms = 0) {
    std::vector<std::string> cmd{container_runtime(),
                                 "exec",
                                 container,
                                 "tc",
                                 "qdisc",
                                 "replace",
                                 "dev",
                                 "eth0",
                                 "root",
                                 "netem",
                                 "delay",
                                 std::to_string(delay_ms) + "ms"};
    if (jitter_ms > 0) {
        cmd.push_back(std::to_string(jitter_ms) + "ms");
    }
    return cmd;
}

inline std::vector<std::string> tc_loss_cmd(const std::string& container, double loss_pct) {
    std::ostringstream pct;
    pct << loss_pct << '%';
    return {container_runtime(),
            "exec",
            container,
            "tc",
            "qdisc",
            "replace",
            "dev",
            "eth0",
            "root",
            "netem",
            "loss",
            pct.str()};
}

inline std::vector<std::string> tc_clear_cmd(const std::string& container) {
    return {container_runtime(), "exec", container, "tc", "qdisc", "del", "dev", "eth0", "root"};
}

inline std::vector<std::string> iptables_drop_src_cmd(const std::string& container,
                                                      const std::string& src_ip) {
    return {container_runtime(),
            "exec",
            container,
            "iptables",
            "-A",
            "CHAOS",
            "-s",
            src_ip,
            "-j",
            "DROP"};
}

inline std::vector<std::string> iptables_drop_dst_cmd(const std::string& container,
                                                      const std::string& dst_ip) {
    return {container_runtime(),
            "exec",
            container,
            "iptables",
            "-A",
            "OUTPUT",
            "-d",
            dst_ip,
            "-j",
            "DROP"};
}

inline std::vector<std::string> iptables_flush_chaos_cmd(const std::string& container) {
    return {container_runtime(), "exec", container, "iptables", "-F", "CHAOS"};
}

inline std::vector<std::string> iptables_flush_output_cmd(const std::string& container) {
    return {container_runtime(), "exec", container, "iptables", "-F", "OUTPUT"};
}

inline std::vector<std::string> docker_kill_cmd(const std::string& container) {
    return {container_runtime(), "kill", container};
}

inline std::vector<std::string> docker_stop_cmd(const std::string& container,
                                                int timeout_sec = 10) {
    return {container_runtime(), "stop", "-t", std::to_string(timeout_sec), container};
}

inline std::vector<std::string> docker_start_cmd(const std::string& container) {
    return {container_runtime(), "start", container};
}

// Freezes every process in the container (cgroup freezer); unpause thaws it.
// Unlike kill or stop, the node keeps its sockets and in-memory state, so on
// thaw it acts on whatever it believed before: the case a paused GC or a
// stalled VM produces.
inline std::vector<std::string> pause_cmd(const std::string& container) {
    return {container_runtime(), "pause", container};
}

inline std::vector<std::string> unpause_cmd(const std::string& container) {
    return {container_runtime(), "unpause", container};
}

// Whether a failed `pause` failed because this host cannot freeze containers
// at all, rather than because of this container. Rootless Podman has the
// freezer only on cgroup v2, and refuses on cgroup v1 with an error that
// names cgroups. Matched loosely, on the words the runtimes use, because the
// exact text differs between versions; a miss reports an ordinary failure,
// which is the safe direction.
inline bool is_pause_unsupported(std::string_view output) {
    std::string lower(output);
    for (auto& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower.find("cgroup") == std::string::npos) {
        return false;
    }
    for (std::string_view marker : {"v1", "freezer", "not available", "not supported"}) {
        if (lower.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

inline std::vector<std::string> docker_logs_cmd(const std::string& container,
                                                int tail_lines = 200) {
    return {container_runtime(), "logs", "--tail", std::to_string(tail_lines), container};
}

inline std::vector<std::string> container_ip_cmd(const std::string& container) {
    return {container_runtime(), "inspect", "--format",
            "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", container};
}

inline std::vector<std::string> compose_up_cmd(const std::string& compose_file) {
    auto cmd = compose_prefix();
    cmd.insert(cmd.end(), {"-f", compose_file, "up", "-d"});
    return cmd;
}

inline std::vector<std::string> compose_down_cmd(const std::string& compose_file) {
    auto cmd = compose_prefix();
    cmd.insert(cmd.end(), {"-f", compose_file, "down", "--remove-orphans"});
    return cmd;
}

}  // namespace docker_chaos::os
