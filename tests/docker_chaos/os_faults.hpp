#pragma once

#include <sys/wait.h>

#include <array>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
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
            if (c == '\'')
                cmd += "'\\''";
            else
                cmd += c;
        }
        cmd += "' ";
    }
    cmd += "2>&1";

    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return {-1, "popen failed"};

    std::string output;
    std::array<char, 256> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp)) {
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

// ── Command-vector builders (pure — testable without Docker) ──────────────────

inline std::vector<std::string> tc_netem_cmd(const std::string& container, const std::string& loss,
                                             const std::string& delay, const std::string& corrupt) {
    return {"docker", "exec",  container, "tc", "qdisc", "replace", "dev",     "eth0",
            "root",   "netem", "loss",    loss, "delay", delay,     "corrupt", corrupt};
}

inline std::vector<std::string> tc_clear_cmd(const std::string& container) {
    return {"docker", "exec", container, "tc", "qdisc", "del", "dev", "eth0", "root"};
}

inline std::vector<std::string> iptables_drop_src_cmd(const std::string& container,
                                                      const std::string& src_ip) {
    return {"docker", "exec", container, "iptables", "-A", "CHAOS", "-s", src_ip, "-j", "DROP"};
}

inline std::vector<std::string> iptables_drop_dst_cmd(const std::string& container,
                                                      const std::string& dst_ip) {
    return {"docker", "exec", container, "iptables", "-A", "OUTPUT", "-d", dst_ip, "-j", "DROP"};
}

inline std::vector<std::string> iptables_flush_chaos_cmd(const std::string& container) {
    return {"docker", "exec", container, "iptables", "-F", "CHAOS"};
}

inline std::vector<std::string> iptables_flush_output_cmd(const std::string& container) {
    return {"docker", "exec", container, "iptables", "-F", "OUTPUT"};
}

inline std::vector<std::string> docker_kill_cmd(const std::string& container) {
    return {"docker", "kill", container};
}

inline std::vector<std::string> docker_stop_cmd(const std::string& container,
                                                int timeout_sec = 10) {
    return {"docker", "stop", "-t", std::to_string(timeout_sec), container};
}

inline std::vector<std::string> docker_start_cmd(const std::string& container) {
    return {"docker", "start", container};
}

inline std::vector<std::string> docker_logs_cmd(const std::string& container,
                                                int tail_lines = 200) {
    return {"docker", "logs", "--tail", std::to_string(tail_lines), container};
}

inline std::vector<std::string> container_ip_cmd(const std::string& container) {
    return {"docker", "inspect", "--format",
            "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", container};
}

inline std::vector<std::string> compose_up_cmd(const std::string& compose_file) {
    return {"docker", "compose", "-f", compose_file, "up", "-d"};
}

inline std::vector<std::string> compose_down_cmd(const std::string& compose_file) {
    return {"docker", "compose", "-f", compose_file, "down", "--remove-orphans"};
}

}  // namespace docker_chaos::os
