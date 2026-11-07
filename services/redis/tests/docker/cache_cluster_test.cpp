// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE cache_cluster_test
#include <boost/test/results_collector.hpp>
#include <boost/test/tree/observer.hpp>
#include <boost/test/unit_test.hpp>

#include "os_faults.hpp"

#include <kythira/redis/resp_protocol.hpp>

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
// cache/ task 8, Requirement 11). services/redis/docker/cache-cluster/compose.yml starts
// three nodes, cache-1..cache-3, each publishing RESP on 1639<n> and HTTP on
// 1809<n>. These cases show that the image comes up under the runtime the
// harness selects (Docker or rootless Podman), runs as a non-root user,
// keeps its data on the volume across a restart, and that the nodes find
// each other by service name: a write on one node reads back on every
// other, over both protocols.
//
// Task 9 adds the real clients (Requirements 9.1, 12.1 and 12.2). The
// compose file's `client` service, behind the `client` profile, is
// services/redis/docker/cache-cluster/client/: upstream sccache 0.17.0 and vcpkg at the
// pinned baseline. Each case is one `run --rm -T client <mode>`, and
// client/run.sh prints KYTHIRA_* key=value lines that `run_client` parses.
// The assertions are on what the clients themselves report: sccache's
// `--show-stats`, and vcpkg's own build-or-restore output.
//
// Task 10 adds the chaos scenarios (Requirements 10.3 and 12.3-12.6): the
// leader killed during a 100 MB PUT, every node stopped and started, a node
// replaced with an empty volume, a byte flipped in a stored chunk, and a
// node whose data directory runs short of inodes.

using namespace std::chrono_literals;
namespace os = docker_chaos::os;

namespace {

constexpr std::array<std::uint16_t, 3> k_resp_ports{16391, 16392, 16393};
constexpr std::array<std::uint16_t, 3> k_http_ports{18091, 18092, 18093};
constexpr std::array<const char*, 3> k_containers{"cache-cluster-1", "cache-cluster-2",
                                                  "cache-cluster-3"};

// Secrets as services/redis/docker/cache-cluster/acl.txt hashes them.
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
    return "services/redis/docker/cache-cluster/compose.yml";
}

/// An override applied on top of compose_file(), or empty. The rolling-upgrade
/// target sets it to rolling-upgrade.yml, which puts two nodes on an older
/// build (.kiro/specs/redis-gateway-module/ task 7).
std::string compose_override() {
    const char* env = std::getenv("KYTHIRA_CACHE_CLUSTER_COMPOSE_OVERRIDE");
    return ((env != nullptr) && (*env != 0)) ? std::string(env) : std::string();
}

std::vector<std::string> compose_cmd(std::initializer_list<std::string> args) {
    auto cmd = os::compose_prefix();
    cmd.insert(cmd.end(), {"-f", compose_file()});
    if (auto extra = compose_override(); !extra.empty()) {
        cmd.insert(cmd.end(), {"-f", extra});
    }
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

// ── chaos helpers (task 10) ───────────────────────────────────────────────────

/// `size` bytes no other object shares. Chunks are stored by hash, so two
/// objects with a common chunk would share its file and a fault in one
/// would show up in the other.
std::string unique_bytes(std::size_t size, std::uint64_t seed) {
    std::string out(size, '\0');
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ULL + 1;
    for (std::size_t i = 0; i < size; i += 8) {
        x ^= x << 13U;
        x ^= x >> 7U;
        x ^= x << 17U;
        for (std::size_t b = 0; b < 8 && i + b < size; ++b) {
            out[i + b] = static_cast<char>((x >> (8U * b)) & 0xFFU);
        }
    }
    return out;
}

/// One counter from a node's `/_metrics` (an admin route), or -1.
long http_metric(std::size_t node, const std::string& name) {
    auto r = http_call(k_http_ports[node], "GET", "/_metrics", {}, "ops");
    const auto line = "kythira_cache_http_" + name + " ";
    auto pos = r.body.find(line);
    return r.status != 200 || pos == std::string::npos
               ? -1
               : std::stol(r.body.substr(pos + line.size()));
}

/// One INFO field from a node, or -1 if the node does not answer.
long node_info(std::size_t node, const std::string& field) {
    auto c = connect_as(k_resp_ports[node], "ops");
    return c ? info_field(c->call({"INFO"}), field) : -1;
}

/// The node that leads the shard holding `key`'s prefix: a PUT of a
/// one-byte object under `prefix` to a follower is forwarded, and to the
/// leader it is not.
std::optional<std::size_t> http_leader_for(const std::string& prefix) {
    for (std::size_t n = 0; n < k_http_ports.size(); ++n) {
        const auto before = http_metric(n, "forwards");
        const auto put = http_call(k_http_ports[n], "PUT",
                                   "/" + prefix + "leader-probe-" + std::to_string(n), "x");
        if (put.status == 201 && http_metric(n, "forwards") == before) {
            return n;
        }
    }
    return std::nullopt;
}

bool container_running(const char* name) {
    auto r =
        os::real_exec({os::container_runtime(), "inspect", "--format", "{{.State.Running}}", name});
    return r.code == 0 && r.out.find("true") != std::string::npos;
}

/// A shell command inside a node's container, as its own user.
os::CmdResult in_container(const char* name, const std::string& script) {
    return os::real_exec({os::container_runtime(), "exec", name, "sh", "-c", script});
}

/// Change one byte, at `offset`, of the stored chunk file of exactly `size`
/// bytes under the node's data directory. The chunk is found by size, so
/// the caller picks a size no other chunk has. Returns the file's path.
std::string flip_chunk_byte(const char* name, std::size_t size, std::size_t offset) {
    const auto script =
        "set -e; f=$(find \"${KYTHIRA_CACHE_DATA_DIR:-/data}/chunks\" -type f -size " +
        std::to_string(size) +
        "c); [ -n \"$f\" ]; chmod u+w \"$f\"; "
        "b=$(od -An -tu1 -j" +
        std::to_string(offset) +
        " -N1 \"$f\"); "
        "printf \"$(printf '\\\\%03o' $(( (b + 1) % 256 )))\" | "
        "dd of=\"$f\" bs=1 seek=" +
        std::to_string(offset) + " conv=notrunc 2>/dev/null; echo \"$f\"";
    auto r = in_container(name, script);
    BOOST_TEST_MESSAGE("flip " << size << "-byte chunk on " << name << ": exit " << r.code << " "
                               << r.out);
    BOOST_REQUIRE_MESSAGE(r.code == 0, "no " << size << "-byte chunk on " << name);
    return r.out;
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
        // `-v`: every run starts from empty volumes.
        os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
        try {
            os::checked_exec(os::real_exec, compose_cmd({"up", "-d"}));
        } catch (const std::exception& e) {
            // Printed here: a global fixture that throws is reported only as
            // "setup failed", without the runtime's own words.
            std::cerr << "compose up failed: " << e.what() << '\n' << node_logs();
            os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
            throw;
        }
        if (!wait_cluster_ready(90s)) {
            std::cerr << node_logs();
            os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
            BOOST_FAIL("cache cluster did not become ready within 90 s");
        }
        // Last: a constructor that throws never runs the destructor, and an
        // observer still registered when its member is destroyed is called
        // after it is gone (the segfault that hid the error above).
        boost::unit_test::framework::register_observer(_observer);
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

// ── chaos scenarios (task 10) ─────────────────────────────────────────────────

// Requirement 12.3: the leader of the key's shard is killed (SIGKILL) while
// a 100 MB PUT is streaming through a follower. Afterwards the key is either
// absent on every node or complete and byte-identical on every node, never
// partial. A 201 means committed, so then it must be present.
BOOST_AUTO_TEST_CASE(a_leader_killed_mid_put_leaves_the_key_absent_or_whole,
                     *boost::unit_test::timeout(600)) {
    const std::string prefix = "vcpkg/chaos/";
    const auto leader = http_leader_for(prefix);
    BOOST_REQUIRE_MESSAGE(leader, "no node leads the vcpkg shard");
    const auto via = (*leader + 1) % k_http_ports.size();
    BOOST_TEST_MESSAGE("vcpkg shard leader: cache-" << *leader + 1 << ", PUT via cache-"
                                                    << via + 1);

    const auto key = prefix + "killed-" + next_salt() + ".zip";
    const auto value = unique_bytes(std::size_t{100} * 1000 * 1000, 123);
    int put_status = 0;
    std::string put_error;
    std::thread put([&] {
        try {
            put_status = http_call(k_http_ports[via], "PUT", "/" + key, value).status;
        } catch (const std::exception& e) {
            put_error = e.what();
        }
    });
    // Kill once the leader has stored some of it, so the kill lands mid-PUT.
    const auto bytes_before = node_info(*leader, "cache_store_bytes");
    BOOST_CHECK(eventually(
        [&] { return node_info(*leader, "cache_store_bytes") >= bytes_before + 8 * 1024 * 1024; },
        60s));
    os::checked_exec(os::real_exec,
                     {os::container_runtime(), "kill", "-s", "KILL", k_containers[*leader]});
    put.join();
    BOOST_TEST_MESSAGE("PUT after the kill: status " << put_status << " " << put_error);
    BOOST_TEST(put_status != 200);

    os::checked_exec(os::real_exec, compose_cmd({"start", "cache-" + std::to_string(*leader + 1)}));
    BOOST_REQUIRE(wait_cluster_ready(90s));

    // Settle, then every node must agree: all whole, or all absent.
    std::array<int, 3> status{};
    const auto agreed = eventually(
        [&] {
            bool whole = true;
            bool absent = true;
            for (std::size_t n = 0; n < k_http_ports.size(); ++n) {
                auto r = http_call(k_http_ports[n], "GET", "/" + key, {}, "ci-pr");
                status[n] = r.status;
                if (r.status == 200) {
                    BOOST_REQUIRE_MESSAGE(r.body == value, "cache-" << n + 1 << " served "
                                                                    << r.body.size()
                                                                    << " bytes, not the "
                                                                    << value.size() << " written");
                }
                whole = whole && r.status == 200;
                absent = absent && r.status == 404;
            }
            return whole || (absent && put_status != 201);
        },
        60s);
    BOOST_TEST_MESSAGE("GET after recovery: " << status[0] << " " << status[1] << " " << status[2]);
    BOOST_TEST(agreed, "nodes disagree on the key, or a 201 key is missing");

    // The cluster takes writes again.
    BOOST_TEST(http_call(k_http_ports[via], "PUT", "/" + prefix + "after-kill", "ok").status ==
               201);
}

// Requirement 12.4: stop all three nodes, start them again, and every
// committed key is served, small RESP values and multi-chunk objects alike.
BOOST_AUTO_TEST_CASE(every_committed_key_is_served_after_stopping_every_node,
                     *boost::unit_test::timeout(600)) {
    const auto salt = next_salt();
    // The writes are setup, made as a client would make them: a refusal the
    // node says to retry (no reachable leader, a 503) is retried. Under
    // rootless Podman the shard the previous case's restart disturbed can
    // still be settling when this case starts.
    std::map<std::string, std::string> resp_keys;
    for (int i = 0; i < 20; ++i) {
        auto key = "sccache/stop/" + salt + "/" + std::to_string(i);
        auto value = "value-" + std::to_string(i) + "-" + salt;
        std::string last;
        const auto stored = eventually(
            [&] {
                auto writer = connect_as(k_resp_ports[0], "ci-main");
                if (!writer) {
                    return false;
                }
                last = writer->call({"SET", key, value});
                return last == "+OK\r\n";
            },
            60s);
        BOOST_REQUIRE_MESSAGE(stored, "SET " << key << ": " << last);
        resp_keys[key] = value;
    }
    std::map<std::string, std::string> objects;
    for (int i = 0; i < 2; ++i) {
        auto key = "vcpkg/stop/" + salt + "/" + std::to_string(i) + ".zip";
        auto value = unique_bytes(9 * 1024 * 1024 + 4321, 1240 + i);
        int last = 0;
        const auto stored = eventually(
            [&] {
                last = http_call(k_http_ports[i + 1], "PUT", "/" + key, value).status;
                return last == 201;
            },
            60s);
        BOOST_REQUIRE_MESSAGE(stored, "PUT " << key << ": " << last);
        objects[key] = std::move(value);
    }

    os::checked_exec(os::real_exec, compose_cmd({"stop", "cache-1", "cache-2", "cache-3"}));
    for (const auto* c : k_containers) {
        BOOST_REQUIRE(!container_running(c));
    }
    os::checked_exec(os::real_exec, compose_cmd({"start", "cache-1", "cache-2", "cache-3"}));
    BOOST_REQUIRE(wait_cluster_ready(90s));

    for (std::size_t n = 0; n < k_resp_ports.size(); ++n) {
        BOOST_TEST_INFO("node " << n + 1);
        BOOST_CHECK(eventually([&] {
            auto r = connect_as(k_resp_ports[n], "ci-pr");
            if (!r) {
                return false;
            }
            for (const auto& [k, v] : resp_keys) {
                if (r->call({"GET", k}) != bulk(v)) {
                    return false;
                }
            }
            return true;
        }));
        for (const auto& [k, v] : objects) {
            BOOST_CHECK(eventually([&] {
                auto r = http_call(k_http_ports[n], "GET", "/" + k, {}, "ci-pr");
                return r.status == 200 && r.body == v;
            }));
        }
    }
}

// Requirement 10.3 (10.1 and 10.2): one byte of a stored chunk is changed on
// disk. A bad first chunk is found before the headers, so the GET is a miss
// (404, or the leader's copy when the node is a follower); a bad later chunk is found after them,
// so the node aborts the connection and the client gets a short body, never a whole wrong object.
// Both chunks are then repaired from a peer and served correctly.
//
// The byte is flipped in the file itself, through the container, rather than
// with a libfiu hook: the image's daemon is a release build without libfiu,
// and a real flipped byte also exercises the read path's hash check end to
// end. The chunk sizes are chosen so no other chunk has them.
BOOST_AUTO_TEST_CASE(a_flipped_chunk_byte_is_a_miss_or_an_abort_then_repaired,
                     *boost::unit_test::timeout(300)) {
    constexpr std::size_t node = 1;  // cache-2
    const auto salt = next_salt();
    constexpr std::size_t a_size = 100'003;  // one loose chunk
    constexpr std::size_t b_tail = 100'007;  // the last of three chunks
    const auto a = unique_bytes(a_size, 1031);
    const auto b = unique_bytes(2 * 4 * 1024 * 1024 + b_tail, 1032);
    const auto a_key = "vcpkg/flip/" + salt + "/a.zip";
    const auto b_key = "vcpkg/flip/" + salt + "/b.zip";
    BOOST_REQUIRE_EQUAL(http_call(k_http_ports[0], "PUT", "/" + a_key, a).status, 201);
    BOOST_REQUIRE_EQUAL(http_call(k_http_ports[0], "PUT", "/" + b_key, b).status, 201);
    // The node has both, whole, before the fault.
    BOOST_REQUIRE(eventually([&] {
        return http_call(k_http_ports[node], "GET", "/" + a_key, {}, "ci-pr").body == a &&
               http_call(k_http_ports[node], "GET", "/" + b_key, {}, "ci-pr").body == b;
    }));

    const auto failures_before = node_info(node, "cache_hash_failures");
    const auto aborted_before = http_metric(node, "aborted_gets");
    flip_chunk_byte(k_containers[node], a_size, 4321);
    flip_chunk_byte(k_containers[node], b_tail, 77);

    auto miss = http_call(k_http_ports[node], "GET", "/" + a_key, {}, "ci-pr");
    BOOST_TEST_MESSAGE("GET of the object with a bad first chunk: "
                       << miss.status << ", " << miss.body.size() << " bytes");
    // A follower forwards to the leader rather than answer the miss itself;
    // either way, never the bad bytes.
    BOOST_TEST((miss.status == 404 || (miss.status == 200 && miss.body == a)));

    auto cut = http_call(k_http_ports[node], "GET", "/" + b_key, {}, "ci-pr");
    BOOST_TEST_MESSAGE("GET of the object with a bad last chunk: "
                       << cut.status << ", " << cut.body.size() << " of " << b.size() << " bytes");
    BOOST_TEST(cut.body != b);
    if (cut.status == 200) {
        BOOST_TEST(cut.body.size() < b.size(), "a bad chunk after the headers must cut the body");
        BOOST_TEST(http_metric(node, "aborted_gets") > aborted_before);
    }
    BOOST_TEST(node_info(node, "cache_hash_failures") >= failures_before + 2);

    // Repaired from a peer, then served whole.
    BOOST_TEST(eventually([&] {
        auto r = http_call(k_http_ports[node], "GET", "/" + a_key, {}, "ci-pr");
        return r.status == 200 && r.body == a;
    }));
    BOOST_TEST(eventually([&] {
        auto r = http_call(k_http_ports[node], "GET", "/" + b_key, {}, "ci-pr");
        return r.status == 200 && r.body == b;
    }));
}

// Requirement 12.5: cache-3 is replaced by a container with an empty data
// volume. It rejoins, fetches every chunk from its peers, and then serves
// every key. Throughout, no reader sees an error: the other nodes serve
// every key, and the new node answers each GET with the whole object or,
// before it has the key at all, a miss; never a 5xx or a wrong body.
BOOST_AUTO_TEST_CASE(a_node_replaced_with_an_empty_volume_fetches_and_serves,
                     *boost::unit_test::timeout(600)) {
    constexpr std::size_t node = 2;  // cache-3
    const auto salt = next_salt();
    std::map<std::string, std::string> objects;
    // More than compose.yml's 64 MiB snapshot threshold, so the leader has
    // compacted its log and the new node gets a snapshot of manifests whose
    // chunks it must fetch, not a log that carries the bytes.
    for (int i = 0; i < 12; ++i) {
        auto key = "vcpkg/replace/" + salt + "/" + std::to_string(i) + ".zip";
        auto value = unique_bytes(6 * 1024 * 1024 + 100'000 + i, 1250 + i);
        BOOST_REQUIRE_EQUAL(http_call(k_http_ports[i % 2], "PUT", "/" + key, value).status, 201);
        objects[key] = std::move(value);
    }
    const long keys_expected = node_info(0, "cache_keys");

    const auto volume = os::checked_exec(
        os::real_exec, {os::container_runtime(), "inspect", "--format",
                        "{{range .Mounts}}{{if eq .Destination \"/data\"}}{{.Name}}{{end}}{{end}}",
                        k_containers[node]});
    const auto volume_name = volume.substr(0, volume.find_first_of("\r\n"));
    BOOST_REQUIRE(!volume_name.empty());
    os::checked_exec(os::real_exec, {os::container_runtime(), "rm", "-f", k_containers[node]});
    os::checked_exec(os::real_exec, {os::container_runtime(), "volume", "rm", volume_name});
    os::checked_exec(os::real_exec, compose_cmd({"up", "-d", "cache-3"}));

    // Read every key from every node until the new node has fetched it all.
    std::map<std::string, long> errors;
    long new_node_hits = 0;
    long new_node_misses = 0;
    long reads_while_fetching = 0;
    const auto done = eventually(
        [&] {
            bool all_served = true;
            for (std::size_t n = 0; n < k_http_ports.size(); ++n) {
                for (const auto& [k, v] : objects) {
                    http_reply r;
                    try {
                        r = http_call(k_http_ports[n], "GET", "/" + k, {}, "ci-pr");
                    } catch (const std::exception&) {
                        if (n == node) {
                            all_served = false;  // still starting
                            continue;
                        }
                        ++errors["cache-" + std::to_string(n + 1) + " unreachable"];
                        continue;
                    }
                    if (r.status == 200 && r.body == v) {
                        new_node_hits += n == node ? 1 : 0;
                        continue;
                    }
                    if (n == node && r.status == 404) {
                        ++new_node_misses;
                        all_served = false;
                        continue;
                    }
                    ++errors["cache-" + std::to_string(n + 1) + " " + std::to_string(r.status) +
                             " (" + std::to_string(r.body.size()) + " bytes)"];
                    all_served = false;
                }
            }
            const auto pending = node_info(node, "cache_fetch_pending_chunks");
            reads_while_fetching += pending > 0 ? 1 : 0;
            return all_served && pending == 0 && node_info(node, "cache_keys") >= keys_expected;
        },
        240s);
    BOOST_TEST_MESSAGE("new node: " << new_node_hits << " whole GETs, " << new_node_misses
                                    << " misses, " << reads_while_fetching
                                    << " read rounds with fetches pending, fetched "
                                    << node_info(node, "cache_fetched_chunks") << " chunks");
    for (const auto& [what, count] : errors) {
        BOOST_TEST_MESSAGE("error: " << what << " x" << count);
    }
    BOOST_TEST(errors.empty());
    BOOST_TEST(done, "cache-3 did not fetch and serve every key within 240 s");
    BOOST_TEST(node_info(node, "cache_fetched_chunks") > 0);
}

// Requirement 12.6: cache-3's data directory is a tmpfs with few inodes
// (compose.yml's /data-small). Small RESP values are written until the
// leader refuses them with a retryable error, before the filesystem runs out
// of inodes; reads are still served and no node stops. The values are just
// over the store's 64 KiB pack threshold, so each is one file; smaller values
// share pack files and would take far longer to reach any inode floor.
//
// Last before the stopped-cluster case: it rebuilds the cluster with
// cache-3 on the tmpfs, and rebuilds it again as normal at the end.
BOOST_AUTO_TEST_CASE(writes_are_refused_before_inodes_run_out, *boost::unit_test::timeout(600)) {
    constexpr std::size_t node = 2;  // cache-3
    constexpr long inodes = 4000;
    constexpr long floor = 2500;
    auto recreate = [&](bool small) {
        if (small) {
            ::setenv("KYTHIRA_CACHE_3_DATA_DIR", "/data-small", 1);
            ::setenv("KYTHIRA_CACHE_3_SMALL_INODES", std::to_string(inodes).c_str(), 1);
            ::setenv("KYTHIRA_CACHE_CLUSTER_MIN_FREE_INODES", std::to_string(floor).c_str(), 1);
        } else {
            ::unsetenv("KYTHIRA_CACHE_3_DATA_DIR");
            ::unsetenv("KYTHIRA_CACHE_3_SMALL_INODES");
            ::unsetenv("KYTHIRA_CACHE_CLUSTER_MIN_FREE_INODES");
        }
        os::checked_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
        os::checked_exec(os::real_exec, compose_cmd({"up", "-d"}));
        BOOST_REQUIRE(wait_cluster_ready(90s));
    };
    recreate(true);

    // The runtime must honour nr_inodes, or the scenario proves nothing.
    auto fs = in_container(k_containers[node], "stat -f -c '%c %d' /data-small");
    BOOST_TEST_MESSAGE("cache-3 /data-small inodes (total free): " << fs.out);
    BOOST_REQUIRE_EQUAL(fs.code, 0);
    BOOST_REQUIRE_MESSAGE(std::stol(fs.out) == inodes,
                          os::container_runtime() << " ignored the tmpfs nr_inodes option");

    auto writer = connect_as(k_resp_ports[0], "ci-main");
    BOOST_REQUIRE(writer);
    const auto salt = next_salt();
    std::vector<std::pair<std::string, std::string>> written;
    std::string refusal;
    for (int i = 0; i < 3000 && refusal.empty(); ++i) {
        auto key = "sccache/inode/" + salt + "/" + std::to_string(i);
        auto value = unique_bytes(70 * 1024 + static_cast<std::size_t>(i), 1260 + i);
        auto reply = writer->call({"SET", key, value});
        if (reply == "+OK\r\n") {
            written.emplace_back(std::move(key), std::move(value));
        } else {
            refusal = reply;
        }
    }
    const auto free_inodes = node_info(node, "cache_fs_free_inodes");
    BOOST_TEST_MESSAGE(written.size() << " values written, then: " << refusal);
    BOOST_TEST_MESSAGE("cache-3 free inodes at the refusal: " << free_inodes << " (floor " << floor
                                                              << ")");
    BOOST_REQUIRE_MESSAGE(!refusal.empty(), "3000 values and no refusal");
    BOOST_TEST(refusal.rfind("-ERR", 0) == 0);
    BOOST_TEST(refusal.find("retry later") != std::string::npos, refusal);
    BOOST_TEST(free_inodes > 0);
    BOOST_TEST(written.size() > 100U, "refused before the inodes were used");
    BOOST_TEST(node_info(0, "cache_space_refusals") + node_info(1, "cache_space_refusals") +
                   node_info(2, "cache_space_refusals") >
               0);

    // Still refused a moment later: the floor holds, it was not a blip.
    BOOST_TEST(writer->call({"SET", "sccache/inode/" + salt + "/again", unique_bytes(70 * 1024, 7)})
                   .rfind("-ERR", 0) == 0);

    // Reads are served on every node, and every node is still running.
    for (std::size_t n = 0; n < k_resp_ports.size(); ++n) {
        BOOST_TEST_INFO("node " << n + 1);
        BOOST_CHECK(container_running(k_containers[n]));
        auto reader = connect_as(k_resp_ports[n], "ci-pr");
        BOOST_REQUIRE(reader);
        for (std::size_t i = 0; i < written.size(); i += written.size() / 10 + 1) {
            BOOST_CHECK(reader->call({"GET", written[i].first}) == bulk(written[i].second));
        }
    }
    writer.reset();
    recreate(false);
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

// ── rolling upgrade (redis-gateway-module task 7, Requirement 7.5) ───────────
//
// Run only by docker-cache-rolling-upgrade-tests, which sets
// KYTHIRA_CACHE_CLUSTER_COMPOSE_OVERRIDE to rolling-upgrade.yml and names
// this suite: cache-1 and cache-2 run kythira-cache-cluster:old (the last
// commit before the module move), cache-3 runs this tree's build. Disabled
// otherwise, so docker-cache-cluster-tests skips it.

namespace {

auto image_of(const char* container) -> std::string {
    auto out = os::checked_exec(os::real_exec, {os::container_runtime(), "inspect", "--format",
                                                "{{.Config.Image}}", container});
    return out.substr(0, out.find_first_of("\r\n"));
}

/// Whether a container runs the given tag. A substring match: Podman
/// reports locally built images as localhost/<name>:<tag>.
auto runs(const char* container, const std::string& tag) -> bool {
    return image_of(container).find("kythira-cache-cluster:" + tag) != std::string::npos;
}

/// Recreate one node on another build, keeping its volume: what a rolling
/// upgrade (or a rollback) does to a real node.
auto swap_build(std::size_t node, const std::string& image) -> void {
    const auto var = "KYTHIRA_CACHE_" + std::to_string(node + 1) + "_IMAGE";
    ::setenv(var.c_str(), image.c_str(), 1);
    os::checked_exec(os::real_exec,
                     compose_cmd({"up", "-d", "--no-deps", "cache-" + std::to_string(node + 1)}));
}

auto expect_sccache_warm_hits(const client_report& r) -> void {
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

}  // namespace

BOOST_AUTO_TEST_SUITE(rolling_upgrade, *boost::unit_test::disabled())

BOOST_AUTO_TEST_CASE(two_old_nodes_and_one_new_serve_sccache, *boost::unit_test::timeout(600)) {
    BOOST_REQUIRE(!compose_override().empty());
    BOOST_CHECK(runs(k_containers[0], "old"));
    BOOST_CHECK(runs(k_containers[1], "old"));
    BOOST_CHECK(runs(k_containers[2], "dev"));
    auto r = run_client("sccache", {{"KYTHIRA_SALT", next_salt()}});
    BOOST_REQUIRE_EQUAL(r.exit_code, 0);
    expect_sccache_warm_hits(r);
}

// A node moves to the other build between the cold build and the warm one:
// cache-1 is upgraded and cache-3 rolled back, each keeping its volume. The
// warm build must still hit every object the cold build wrote, from a
// cluster whose members changed build under it, and a key written before
// the swap reads back from every node.
BOOST_AUTO_TEST_CASE(a_node_restarted_on_the_other_build_keeps_serving,
                     *boost::unit_test::timeout(900)) {
    const auto salt = next_salt();
    auto cold = run_client("sccache", {{"KYTHIRA_SALT", salt}});
    BOOST_REQUIRE_EQUAL(cold.exit_code, 0);
    BOOST_REQUIRE_EQUAL(
        http_call(k_http_ports[2], "PUT", "/sccache/rolling/" + salt, "before-swap").status, 201);

    swap_build(0, "kythira-cache-cluster:dev");
    BOOST_REQUIRE(wait_cluster_ready(120s));
    swap_build(2, "kythira-cache-cluster:old");
    BOOST_REQUIRE(wait_cluster_ready(120s));
    BOOST_CHECK(runs(k_containers[0], "dev"));
    BOOST_CHECK(runs(k_containers[2], "old"));

    for (std::size_t n = 0; n < k_http_ports.size(); ++n) {
        BOOST_TEST_INFO("node " << n + 1);
        BOOST_CHECK(eventually([&] {
            auto g = http_call(k_http_ports[n], "GET", "/sccache/rolling/" + salt, {}, "ci-pr");
            return g.status == 200 && g.body == "before-swap";
        }));
    }
    // Same salt: the same compiles, so a warm run must hit everything the
    // cold run wrote before the swap.
    auto warm = run_client("sccache", {{"KYTHIRA_SALT", salt}});
    BOOST_REQUIRE_EQUAL(warm.exit_code, 0);
    const auto stats = warm.all("STATS");
    BOOST_REQUIRE_EQUAL(stats.size(), 2u);
    const auto first = sccache_stats(stats[0]);
    BOOST_TEST(first.requests >= 5);
    BOOST_TEST(first.misses == 0);
    BOOST_TEST(first.hits == first.requests);

    // Back to the layout the suite started from, for any case after this.
    swap_build(0, "kythira-cache-cluster:old");
    swap_build(2, "kythira-cache-cluster:dev");
    BOOST_CHECK(wait_cluster_ready(120s));
}

BOOST_AUTO_TEST_SUITE_END()
