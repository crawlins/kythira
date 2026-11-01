// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE cache_cluster_test
#include <boost/test/results_collector.hpp>
#include <boost/test/tree/observer.hpp>
#include <boost/test/unit_test.hpp>

#include "os_faults.hpp"

#include <raft/resp_protocol.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The disk-backed build cache in containers (.kiro/specs/disk-backed-build-
// cache/ task 8, Requirement 11). docker/cache-cluster/compose.yml starts
// three nodes, cache-1..cache-3, each publishing RESP on 1639<n> and HTTP on
// 1809<n>. These cases show that the image comes up under the runtime the
// harness selects (Docker or rootless Podman), runs as a non-root user,
// keeps its data on the volume across a restart, and that the nodes find
// each other by service name: a write on one node reads back on every
// other, over both protocols.
//
// The real-client end-to-end tests (task 9) and the chaos scenarios
// (task 10) build on this file's fixture.

using namespace std::chrono_literals;
namespace os = docker_chaos::os;

namespace {

constexpr std::array<std::uint16_t, 3> k_resp_ports{16391, 16392, 16393};
constexpr std::array<std::uint16_t, 3> k_http_ports{18091, 18092, 18093};
constexpr std::array<const char*, 3> k_containers{"cache-cluster-1", "cache-cluster-2",
                                                  "cache-cluster-3"};

// Secrets as docker/cache-cluster/acl.txt hashes them.
const std::map<std::string, std::string> k_secrets{
    {"ci-main", "ci-main-secret"},
    {"ci-pr", "ci-pr-secret"},
    {"ops", "ops-secret"},
};

std::string compose_file() {
    const char* env = std::getenv("KYTHIRA_CACHE_CLUSTER_COMPOSE_FILE");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return "docker/cache-cluster/compose.yml";
}

std::vector<std::string> compose_cmd(std::initializer_list<std::string> args) {
    auto cmd = os::compose_prefix();
    cmd.insert(cmd.end(), {"-f", compose_file()});
    cmd.insert(cmd.end(), args);
    return cmd;
}

/// A connected loopback TCP socket with a receive timeout, or -1.
int dial(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    timeval tv{.tv_sec = 10, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void send_all(int fd, const std::string& out) {
    for (std::size_t sent = 0; sent < out.size();) {
        auto n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            throw std::runtime_error("send failed");
        }
        sent += static_cast<std::size_t>(n);
    }
}

// ── a minimal blocking RESP client ────────────────────────────────────────────

class resp_client {
public:
    explicit resp_client(std::uint16_t port) : _fd(dial(port)) {
        if (_fd < 0) {
            throw std::runtime_error("connect to 127.0.0.1:" + std::to_string(port) + " refused");
        }
    }
    ~resp_client() {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }
    resp_client(const resp_client&) = delete;
    resp_client& operator=(const resp_client&) = delete;
    resp_client(resp_client&& o) noexcept
        : _fd(std::exchange(o._fd, -1)), _buffer(std::move(o._buffer)) {}
    resp_client& operator=(resp_client&&) = delete;

    std::string call(const std::vector<std::string>& argv) {
        std::string out = "*" + std::to_string(argv.size()) + "\r\n";
        for (const auto& a : argv) {
            out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
        }
        send_all(_fd, out);
        std::size_t len = 0;
        while ((len = kythira::resp_reply_length(_buffer)) == 0) {
            std::array<char, 65536> chunk{};
            auto n = ::recv(_fd, chunk.data(), chunk.size(), 0);
            if (n <= 0) {
                throw std::runtime_error("connection closed or timed out waiting for a reply");
            }
            _buffer.append(chunk.data(), static_cast<std::size_t>(n));
        }
        auto reply = _buffer.substr(0, len);
        _buffer.erase(0, len);
        return reply;
    }

private:
    int _fd{-1};
    std::string _buffer;
};

std::optional<resp_client> connect_as(std::uint16_t port, const std::string& user) {
    try {
        resp_client c(port);
        if (c.call({"AUTH", user, k_secrets.at(user)}) != "+OK\r\n") {
            return std::nullopt;
        }
        return c;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string bulk(const std::string& v) {
    return "$" + std::to_string(v.size()) + "\r\n" + v + "\r\n";
}

long info_field(const std::string& info, const std::string& field) {
    auto pos = info.find("\r\n" + field + ":");
    if (pos == std::string::npos) {
        return -1;
    }
    return std::stol(info.substr(pos + field.size() + 3));
}

std::string info_text(const std::string& info, const std::string& field) {
    auto pos = info.find("\r\n" + field + ":");
    if (pos == std::string::npos) {
        return {};
    }
    pos += field.size() + 3;
    return info.substr(pos, info.find("\r\n", pos) - pos);
}

// ── a minimal blocking HTTP/1.1 client ────────────────────────────────────────
// One request per connection (`Connection: close`), Basic credentials, the
// body read to EOF. Enough for the front end's PUT/GET/DELETE.

struct http_reply {
    int status = 0;
    std::string body;
};

std::string base64(const std::string& in) {
    static constexpr char k_table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : in) {
        acc = (acc << 8U) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(k_table[(acc >> static_cast<unsigned>(bits)) & 0x3FU]);
        }
    }
    if (bits > 0) {
        out.push_back(k_table[(acc << static_cast<unsigned>(6 - bits)) & 0x3FU]);
    }
    while (out.size() % 4 != 0) {
        out.push_back('=');
    }
    return out;
}

http_reply http_call(std::uint16_t port, const std::string& method, const std::string& path,
                     const std::string& body = {}, const std::string& user = "ci-main") {
    int fd = dial(port);
    if (fd < 0) {
        throw std::runtime_error("connect to 127.0.0.1:" + std::to_string(port) + " refused");
    }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: cache\r\nConnection: close\r\n" +
                      "Authorization: Basic " + base64(user + ":" + k_secrets.at(user)) + "\r\n" +
                      "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::string in;
    try {
        send_all(fd, req);
        std::array<char, 65536> chunk{};
        for (;;) {
            auto n = ::recv(fd, chunk.data(), chunk.size(), 0);
            if (n <= 0) {
                break;
            }
            in.append(chunk.data(), static_cast<std::size_t>(n));
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    http_reply r;
    if (in.size() < 12 || in.rfind("HTTP/1.1 ", 0) != 0) {
        throw std::runtime_error("no HTTP status line from port " + std::to_string(port));
    }
    r.status = std::stoi(in.substr(9, 3));
    auto head_end = in.find("\r\n\r\n");
    if (head_end != std::string::npos) {
        r.body = in.substr(head_end + 4);
    }
    return r;
}

// Ready: every node answers AUTH + PING and the two shards have a leader.
bool wait_cluster_ready(std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        long led = 0;
        bool all_up = true;
        for (auto port : k_resp_ports) {
            auto c = connect_as(port, "ops");
            if (!c || c->call({"PING"}) != "+PONG\r\n") {
                all_up = false;
                break;
            }
            led += std::max(0L, info_field(c->call({"INFO"}), "local_shards_led"));
        }
        if (all_up && led == 2) {
            return true;
        }
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

/// Poll `fn` until it returns true or `timeout` passes.
template<typename F> bool eventually(F&& fn, std::chrono::milliseconds timeout = 20s) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            if (fn()) {
                return true;
            }
        } catch (const std::exception&) {
            // a node still starting; try again
        }
        std::this_thread::sleep_for(250ms);
    }
    return false;
}

std::string node_logs() {
    std::string all;
    for (const auto* c : k_containers) {
        all += "── " + std::string(c) + " ──\n" + os::real_exec(os::docker_logs_cmd(c, 200)).out;
    }
    return all;
}

}  // namespace

struct NodeLogsOnFailure : boost::unit_test::test_observer {
    void test_unit_finish(const boost::unit_test::test_unit& tu, unsigned long) override {
        if (tu.p_type != boost::unit_test::TUT_CASE ||
            boost::unit_test::results_collector.results(tu.p_id).passed()) {
            return;
        }
        std::cerr << "── node logs after " << tu.full_name() << " failed ──\n" << node_logs();
    }
};

struct CacheClusterFixture {
    CacheClusterFixture() {
        boost::unit_test::framework::register_observer(_observer);
        // `-v`: every run starts from empty volumes.
        os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
        os::checked_exec(os::real_exec, compose_cmd({"up", "-d"}));
        if (!wait_cluster_ready(90s)) {
            BOOST_TEST_MESSAGE(node_logs());
            os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
            BOOST_FAIL("cache cluster did not become ready within 90 s");
        }
    }
    ~CacheClusterFixture() {
        boost::unit_test::framework::deregister_observer(_observer);
        try {
            os::real_exec(compose_cmd({"down", "-v", "--remove-orphans"}));
        } catch (...) {
        }
    }
    CacheClusterFixture(const CacheClusterFixture&) = delete;
    CacheClusterFixture& operator=(const CacheClusterFixture&) = delete;
    CacheClusterFixture(CacheClusterFixture&&) = delete;
    CacheClusterFixture& operator=(CacheClusterFixture&&) = delete;

private:
    NodeLogsOnFailure _observer;
};

BOOST_TEST_GLOBAL_FIXTURE(CacheClusterFixture);

// Requirement 11.1: the daemon runs as uid 10001, in file mode, on /data.
BOOST_AUTO_TEST_CASE(nodes_run_unprivileged_in_file_mode, *boost::unit_test::timeout(120)) {
    for (const auto* c : k_containers) {
        auto user = os::checked_exec(
            os::real_exec, {os::container_runtime(), "inspect", "--format", "{{.Config.User}}", c});
        BOOST_TEST_INFO(c);
        BOOST_CHECK_EQUAL(user.substr(0, user.find_first_of(":\n")), "10001");
    }
    for (auto port : k_resp_ports) {
        auto ops = connect_as(port, "ops");
        BOOST_REQUIRE(ops);
        const auto info = ops->call({"INFO"});
        BOOST_CHECK_EQUAL(info_text(info, "cache_persistence"), "file");
        BOOST_CHECK_GT(info_field(info, "cache_fs_free_bytes"), 0);
    }
}

// Requirement 11.2: the nodes address each other by service name. A write
// on any node, over either protocol, reads back from every node.
BOOST_AUTO_TEST_CASE(writes_on_one_node_read_back_on_every_node, *boost::unit_test::timeout(180)) {
    auto writer = connect_as(k_resp_ports[0], "ci-main");
    BOOST_REQUIRE(writer);
    BOOST_REQUIRE_EQUAL(writer->call({"SET", "sccache/1/2/3/resp", "via-resp"}), "+OK\r\n");

    std::string value(3 * 1024 * 1024 + 11, '\0');
    for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] = static_cast<char>((i * 131U + 7U) & 0xFFU);
    }
    BOOST_REQUIRE_EQUAL(http_call(k_http_ports[1], "PUT", "/vcpkg/abc.zip", value).status, 201);

    for (std::size_t n = 0; n < k_resp_ports.size(); ++n) {
        BOOST_TEST_INFO("node " << n + 1);
        BOOST_CHECK(eventually([&] {
            auto r = connect_as(k_resp_ports[n], "ci-pr");
            return r && r->call({"GET", "sccache/1/2/3/resp"}) == bulk("via-resp");
        }));
        BOOST_CHECK(eventually([&] {
            auto r = http_call(k_http_ports[n], "GET", "/vcpkg/abc.zip", {}, "ci-pr");
            return r.status == 200 && r.body == value;
        }));
    }
    // read_only is refused writes over HTTP too.
    BOOST_CHECK_EQUAL(http_call(k_http_ports[2], "PUT", "/vcpkg/no.zip", "x", "ci-pr").status, 403);
}

// Requirement 5.2 in containers: the volume keeps the data across a restart
// of every node.
BOOST_AUTO_TEST_CASE(data_survives_restarting_every_node, *boost::unit_test::timeout(300)) {
    BOOST_REQUIRE_EQUAL(http_call(k_http_ports[0], "PUT", "/sccache/k/e/e/p", "kept").status, 201);
    os::checked_exec(os::real_exec, compose_cmd({"restart"}));
    BOOST_REQUIRE(wait_cluster_ready(90s));
    for (std::size_t n = 0; n < k_http_ports.size(); ++n) {
        BOOST_TEST_INFO("node " << n + 1);
        BOOST_CHECK(eventually([&] {
            auto r = http_call(k_http_ports[n], "GET", "/sccache/k/e/e/p", {}, "ci-pr");
            return r.status == 200 && r.body == "kept";
        }));
    }
}
