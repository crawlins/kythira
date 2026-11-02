// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE cache_cluster_test
#include <boost/test/results_collector.hpp>
#include <boost/test/tree/observer.hpp>
#include <boost/test/unit_test.hpp>

#include "os_faults.hpp"

#include <raft/resp_protocol.hpp>

#include <boost/json.hpp>

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
#include <ctime>
#include <initializer_list>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
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
// Task 9 adds the real clients (Requirements 9.1, 12.1 and 12.2). The
// compose file's `client` service, behind the `client` profile, is
// docker/cache-cluster/client/: upstream sccache 0.17.0 and vcpkg at the
// pinned baseline. Each case is one `run --rm -T client <mode>`, and
// client/run.sh prints KYTHIRA_* key=value lines that `run_client` parses.
// The assertions are on what the clients themselves report: sccache's
// `--show-stats`, and vcpkg's own build-or-restore output.
//
// The chaos scenarios (task 10) build on this file's fixture.

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

// ── the client container's report ─────────────────────────────────────────────

/// One KYTHIRA_<tag> line as its key=value fields; a `{`-led tail (sccache's
/// JSON stats) is kept whole under "json".
using fields = std::map<std::string, std::string>;

struct client_report {
    int exit_code{-1};
    std::map<std::string, std::vector<fields>> lines;  // by tag

    [[nodiscard]] auto all(const std::string& tag) const -> std::vector<fields> {
        auto it = lines.find(tag);
        return it == lines.end() ? std::vector<fields>{} : it->second;
    }
};

client_report run_client(const std::string& mode,
                         const std::vector<std::pair<std::string, std::string>>& env = {}) {
    // -T: no pseudo-TTY, so the output is what run.sh printed.
    std::vector<std::string> cmd = compose_cmd({"run", "--rm", "-T"});
    for (const auto& [k, v] : env) {
        cmd.push_back("-e");
        cmd.push_back(k + "=" + v);
    }
    cmd.push_back("client");
    cmd.push_back(mode);
    auto [code, out] = os::real_exec(cmd);
    BOOST_TEST_MESSAGE("client " << mode << " exit " << code << ":\n" << out);

    client_report r;
    r.exit_code = code;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("KYTHIRA_", 0) != 0) {
            continue;
        }
        const auto sp = line.find(' ');
        const auto tag = line.substr(8, sp == std::string::npos ? std::string::npos : sp - 8);
        fields f;
        std::string rest = sp == std::string::npos ? std::string{} : line.substr(sp + 1);
        if (auto brace = rest.find('{'); brace != std::string::npos) {
            f["json"] = rest.substr(brace);
            rest.erase(brace);
        }
        std::istringstream words(rest);
        std::string word;
        while (words >> word) {
            if (auto eq = word.find('='); eq != std::string::npos) {
                f[word.substr(0, eq)] = word.substr(eq + 1);
            }
        }
        r.lines[tag].push_back(std::move(f));
    }
    return r;
}

long field_num(const fields& f, const std::string& key) {
    auto it = f.find(key);
    return it == f.end() || it->second.empty() ? -1 : std::stol(it->second);
}

struct sccache_counts {
    long requests{0};
    long hits{0};
    long misses{0};
    long writes{0};
    long write_errors{0};
};

sccache_counts sccache_stats(const fields& f) {
    const auto stats = boost::json::parse(f.at("json")).at("stats");
    auto sum = [&](const char* key) {
        long total = 0;
        if (const auto* counts = stats.at(key).as_object().if_contains("counts")) {
            for (const auto& kv : counts->as_object()) {
                total += kv.value().to_number<long>();
            }
        }
        return total;
    };
    return {stats.at("compile_requests").to_number<long>(), sum("cache_hits"), sum("cache_misses"),
            stats.at("cache_writes").to_number<long>(),
            stats.at("cache_write_errors").to_number<long>()};
}

/// A fresh sccache key set per case; see project/CMakeLists.txt.
std::string next_salt() {
    static long salt = static_cast<long>(std::time(nullptr) % 100000) * 10;
    return std::to_string(++salt);
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

// Requirement 12.1: a clean build through sccache's WebDAV backend misses
// and writes every compile, and a second clean build hits every one.
BOOST_AUTO_TEST_CASE(sccache_misses_then_hits_through_webdav, *boost::unit_test::timeout(600)) {
    auto r = run_client("sccache", {{"KYTHIRA_SALT", next_salt()}});
    BOOST_REQUIRE_EQUAL(r.exit_code, 0);
    const auto mode = r.all("MODE");
    BOOST_REQUIRE_EQUAL(mode.size(), 1u);
    BOOST_REQUIRE_EQUAL(mode[0].at("launcher"), "sccache");
    const auto stats = r.all("STATS");
    BOOST_REQUIRE_EQUAL(stats.size(), 2u);

    const auto cold = sccache_stats(stats[0]);
    BOOST_TEST(cold.requests >= 5);
    BOOST_TEST(cold.hits == 0);
    BOOST_TEST(cold.misses == cold.requests);
    BOOST_TEST(cold.writes == cold.misses);
    BOOST_TEST(cold.write_errors == 0);

    const auto warm = sccache_stats(stats[1]);
    BOOST_TEST(warm.requests == cold.requests);
    BOOST_TEST(warm.hits == cold.misses);
    BOOST_TEST(warm.misses == 0);
}

// Requirement 12.2: vcpkg's `http` binary source builds zlib and uploads
// it, then a fresh tree restores it instead of building.
BOOST_AUTO_TEST_CASE(vcpkg_builds_then_restores_through_http, *boost::unit_test::timeout(1200)) {
    auto r = run_client("vcpkg");
    BOOST_REQUIRE_EQUAL(r.exit_code, 0);
    const auto installs = r.all("VCPKG");
    BOOST_REQUIRE_EQUAL(installs.size(), 2u);
    BOOST_TEST(installs[0].at("status") == "ok");
    BOOST_TEST(field_num(installs[0], "built") == 1);
    BOOST_TEST(field_num(installs[0], "submitted") >= 1);
    BOOST_TEST(installs[1].at("status") == "ok");
    BOOST_TEST(field_num(installs[1], "built") == 0);
    BOOST_TEST(field_num(installs[1], "restored") >= 1);
}

// design.md Risks: a 500 MB PUT to one node and a GET of it from another,
// timed. The numbers are recorded in tasks.md; the assertion is only that
// the object arrives whole. The GET may wait for the other node to apply the
// commit first (run.sh reports that wait separately).
BOOST_AUTO_TEST_CASE(a_500_mb_object_round_trips, *boost::unit_test::timeout(900)) {
    auto r = run_client("throughput", {{"KYTHIRA_MB", "500"}});
    const auto ops = r.all("THROUGHPUT");
    BOOST_REQUIRE_EQUAL(ops.size(), 2u);
    for (const auto& op : ops) {
        const double bytes = std::stod(op.at("bytes"));
        const double seconds = std::stod(op.at("seconds"));
        BOOST_TEST_MESSAGE(op.at("op")
                           << " " << bytes / 1048576.0 << " MiB in " << seconds
                           << " s = " << (bytes / 1048576.0) / seconds << " MiB/s"
                           << (op.contains("waited") ? ", after " + op.at("waited") +
                                                           " s for the follower to apply it"
                                                     : std::string{}));
    }
    BOOST_TEST(ops[0].at("status") == "201");
    BOOST_TEST(ops[1].at("status") == "200");
    BOOST_TEST(ops[1].at("match") == "1");
    BOOST_TEST(r.exit_code == 0);
}

// Requirement 9.1: with every node stopped both clients still build. CI's
// own gate (start-sccache.sh) turns sccache off, and vcpkg builds from
// source after its requests are refused. Last, because it stops the
// cluster.
BOOST_AUTO_TEST_CASE(builds_pass_with_every_node_stopped, *boost::unit_test::timeout(1200)) {
    os::checked_exec(os::real_exec, compose_cmd({"stop", "cache-1", "cache-2", "cache-3"}));

    auto sc = run_client("sccache", {{"KYTHIRA_SALT", next_salt()}});
    BOOST_TEST(sc.exit_code == 0);
    const auto mode = sc.all("MODE");
    BOOST_REQUIRE_EQUAL(mode.size(), 1u);
    BOOST_TEST(mode[0].at("launcher") == "none");
    const auto builds = sc.all("BUILD");
    BOOST_REQUIRE_EQUAL(builds.size(), 2u);
    for (const auto& b : builds) {
        BOOST_TEST(b.at("status") == "ok");
    }

    auto vc = run_client("vcpkg", {{"KYTHIRA_INSTALLS", "1"}});
    BOOST_TEST(vc.exit_code == 0);
    const auto installs = vc.all("VCPKG");
    BOOST_REQUIRE_EQUAL(installs.size(), 1u);
    BOOST_TEST(installs[0].at("status") == "ok");
    BOOST_TEST(field_num(installs[0], "built") == 1);
}
