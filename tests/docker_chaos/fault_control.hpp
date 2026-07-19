#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace docker_chaos::fiu {

// ── Fault point name constants (mirror include/raft/fault_injection.hpp) ─────

inline constexpr std::string_view SEND_REQUEST_VOTE = "raft/network/send_request_vote";
inline constexpr std::string_view SEND_APPEND_ENTRIES = "raft/network/send_append_entries";
inline constexpr std::string_view SEND_INSTALL_SNAPSHOT = "raft/network/send_install_snapshot";
inline constexpr std::string_view SAVE_CURRENT_TERM = "raft/persistence/save_current_term";
inline constexpr std::string_view SAVE_VOTED_FOR = "raft/persistence/save_voted_for";
inline constexpr std::string_view APPEND_LOG_ENTRY = "raft/persistence/append_log_entry";
inline constexpr std::string_view TRUNCATE_LOG = "raft/persistence/truncate_log";
inline constexpr std::string_view SAVE_SNAPSHOT = "raft/persistence/save_snapshot";
inline constexpr std::string_view STATE_MACHINE_APPLY = "raft/state_machine/apply";

// ── Command string builders (pure — testable without a live fiu_rc_tcp) ──────

inline std::string build_enable_always_cmd(std::string_view name) {
    return std::format("enable name={} failnum=-1 failinfo=0", name);
}

inline std::string build_enable_random_cmd(std::string_view name, double probability) {
    return std::format("enable_random name={} failnum=-1 failinfo=0 probability={:.6f}", name,
                       probability);
}

inline std::string build_enable_once_cmd(std::string_view name) {
    return std::format("enable name={} failnum=1 one", name);
}

inline std::string build_disable_cmd(std::string_view name) {
    return std::format("disable name={}", name);
}

inline std::string build_disable_all_cmd() {
    return "disable_all";
}

// ── TCP transport — send one line-protocol command, return integer reply ──────

inline int send_fiu_cmd_raw(const std::string& host, std::uint16_t port, const std::string& cmd) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(std::format("fiu: socket() failed: {}:{}", host, port));
    }

    struct timeval tv{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // inet_pton() only parses numeric IPv4 literals — it never resolves
    // hostnames, so callers passing "localhost" (ChaosNode::enable_fault()
    // in harness.hpp) always failed here. getaddrinfo() handles both
    // numeric addresses and hostnames uniformly.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    int gai_rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &resolved);
    if (gai_rc != 0) {
        ::close(fd);
        throw std::runtime_error(
            std::format("fiu: bad host address: {} ({})", host, ::gai_strerror(gai_rc)));
    }
    addr.sin_addr = reinterpret_cast<sockaddr_in*>(resolved->ai_addr)->sin_addr;
    ::freeaddrinfo(resolved);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error(std::format("fiu: connect to {}:{} failed", host, port));
    }

    std::string line = cmd + "\n";
    if (::send(fd, line.data(), line.size(), MSG_NOSIGNAL) < 0) {
        ::close(fd);
        throw std::runtime_error("fiu: send failed");
    }

    char buf[32]{};
    ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);

    if (n <= 0) {
        throw std::runtime_error(std::format("fiu: no reply from {}:{}", host, port));
    }
    return std::stoi(std::string(buf, static_cast<std::size_t>(n)));
}

inline void send_fiu_cmd(const std::string& host, std::uint16_t port, const std::string& cmd) {
    int rc = send_fiu_cmd_raw(host, port, cmd);
    if (rc != 0) {
        throw std::runtime_error(std::format("fiu: '{}' failed with code {}", cmd, rc));
    }
}

// ── High-level API ────────────────────────────────────────────────────────────

inline void enable_fault(const std::string& host, std::uint16_t port, std::string_view name,
                         std::string_view mode = "always", double probability = 1.0) {
    std::string cmd;
    if (mode == "always") {
        cmd = build_enable_always_cmd(name);
    } else if (mode == "random") {
        cmd = build_enable_random_cmd(name, probability);
    } else if (mode == "once") {
        cmd = build_enable_once_cmd(name);
    } else {
        throw std::invalid_argument(std::format("unknown fault mode: '{}'", mode));
    }
    send_fiu_cmd(host, port, cmd);
}

inline void disable_fault(const std::string& host, std::uint16_t port, std::string_view name) {
    send_fiu_cmd(host, port, build_disable_cmd(name));
}

inline void disable_all_faults(const std::string& host, std::uint16_t port) {
    send_fiu_cmd(host, port, build_disable_all_cmd());
}

}  // namespace docker_chaos::fiu
