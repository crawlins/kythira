// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Raw-socket helpers for the HTTP server limit tests
// (.kiro/specs/http-server-request-limits/). Plain POSIX sockets rather than
// any transport's client, so a test can hold a connection idle, see exactly
// what status line came back, and tell "closed by the server" apart from
// "the RPC failed somehow".

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

namespace kythira::testing::http_limits {

/// An owned, connected TCP socket; -1 when the connect failed.
class raw_connection {
public:
    raw_connection(const std::string& address, std::uint16_t port) {
        bool v6 = address.find(':') != std::string::npos;
        _fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
        if (_fd < 0) {
            return;
        }
        int rc = -1;
        if (v6) {
            sockaddr_in6 a{};
            a.sin6_family = AF_INET6;
            a.sin6_port = htons(port);
            ::inet_pton(AF_INET6, address.c_str(), &a.sin6_addr);
            rc = ::connect(_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        } else {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            ::inet_pton(AF_INET, address.c_str(), &a.sin_addr);
            rc = ::connect(_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        }
        if (rc != 0) {
            ::close(_fd);
            _fd = -1;
        }
    }
    raw_connection(const raw_connection&) = delete;
    auto operator=(const raw_connection&) -> raw_connection& = delete;
    ~raw_connection() { close(); }

    [[nodiscard]] auto connected() const -> bool { return _fd >= 0; }

    auto close() -> void {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    /// Writes all of `data`; false if the peer closed or reset first.
    auto send_all(const std::string& data) -> bool {
        std::size_t sent = 0;
        while (sent < data.size()) {
            auto n = ::send(_fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    enum class read_outcome {
        data,
        closed,
        timed_out
    };

    /// Reads until the peer closes (EOF or reset) or `timeout` passes,
    /// appending what arrived to `out`.
    auto read_until_close(std::string& out, std::chrono::milliseconds timeout) -> read_outcome {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                return read_outcome::timed_out;
            }
            timeval tv{};
            tv.tv_sec = static_cast<time_t>(left.count() / 1000);
            tv.tv_usec = static_cast<suseconds_t>((left.count() % 1000) * 1000);
            ::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[4096];
            auto n = ::recv(_fd, buf, sizeof(buf), 0);
            if (n > 0) {
                out.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0 || errno == ECONNRESET) {
                return read_outcome::closed;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return read_outcome::timed_out;
            }
            if (errno != EINTR) {
                return read_outcome::closed;
            }
        }
    }

    /// Reads until the first CRLFCRLF (the end of a response's headers), the
    /// peer closes, or `timeout` passes. Leaves the connection open.
    auto read_headers(std::chrono::milliseconds timeout) -> std::string {
        std::string out;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (out.find("\r\n\r\n") == std::string::npos) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                break;
            }
            timeval tv{};
            tv.tv_sec = static_cast<time_t>(left.count() / 1000);
            tv.tv_usec = static_cast<suseconds_t>((left.count() % 1000) * 1000);
            ::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[4096];
            auto n = ::recv(_fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            out.append(buf, static_cast<std::size_t>(n));
        }
        return out;
    }

private:
    int _fd{-1};
};

/// A POST of `body` to the request_vote endpoint with a matching
/// `Content-Length`.
inline auto request_with_body(const std::string& body) -> std::string {
    return "POST /v1/raft/request_vote HTTP/1.1\r\n"
           "Host: 127.0.0.1\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

/// A POST to the request_vote endpoint with a body of `body_size` bytes and
/// a matching `Content-Length`.
inline auto sized_request(std::size_t body_size) -> std::string {
    return request_with_body(std::string(body_size, 'x'));
}

/// The headers of a chunked POST to the request_vote endpoint; follow with
/// `chunk()`s.
inline auto chunked_request_head() -> std::string {
    return "POST /v1/raft/request_vote HTTP/1.1\r\n"
           "Host: 127.0.0.1\r\n"
           "Content-Type: application/json\r\n"
           "Transfer-Encoding: chunked\r\n\r\n";
}

/// One chunk of `size` bytes in chunked transfer coding.
inline auto chunk(std::size_t size) -> std::string {
    char length[32];
    std::snprintf(length, sizeof(length), "%zx\r\n", size);
    return std::string(length) + std::string(size, 'x') + "\r\n";
}

/// The status code from a response's status line, or 0 if there is none.
inline auto status_of(const std::string& response) -> int {
    // "HTTP/1.1 413 Payload Too Large"
    auto sp = response.find(' ');
    if (response.rfind("HTTP/", 0) != 0 || sp == std::string::npos || response.size() < sp + 4) {
        return 0;
    }
    return std::atoi(response.substr(sp + 1, 3).c_str());
}

/// The value of header `name` (case-insensitive), with any parameters after
/// ';' and surrounding blanks removed; empty when absent.
inline auto header_of(const std::string& response, const std::string& name) -> std::string {
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto head = lower(response.substr(0, response.find("\r\n\r\n")));
    auto key = "\r\n" + lower(name) + ":";
    auto at = head.find(key);
    if (at == std::string::npos) {
        return {};
    }
    auto start = at + key.size();
    auto end = head.find("\r\n", start);
    auto value = head.substr(start, end - start);
    if (auto semi = value.find(';'); semi != std::string::npos) {
        value.resize(semi);
    }
    auto first = value.find_first_not_of(" \t");
    auto last = value.find_last_not_of(" \t");
    return first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
}

/// Whether this host can bind ::1; the shared-listener tests skip without it.
inline auto ipv6_loopback_available() -> bool {
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    ::close(fd);
    return ok;
}

/// Polls `condition` until it holds or `timeout` passes.
inline auto wait_for(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
    -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

}  // namespace kythira::testing::http_limits
