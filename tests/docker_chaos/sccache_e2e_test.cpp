// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE sccache_e2e_test
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
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The sccache acceptance tests for .kiro/specs/redis-compatible-kv/
// (tasks 12 and 13; Requirements 11.1-11.3, 11.8, 17.1-17.4). A real sccache
// 0.10 — the upstream binary, unpatched — builds a real Rust crate against a
// real three-node gateway cluster, and the assertions are on what sccache
// itself reports through `--show-stats`, not on what this project believes
// the Redis protocol to be.
//
// docker/sccache-e2e-compose.yml defines the cluster (kv1..kv3, published on
// 16381..16383) and one `sccache` runner service behind the `runner` profile,
// so `up -d kv1 kv2 kv3` brings up the cluster alone and each scenario is one
// `run --rm` of the runner with its own environment. The runner prints
// KYTHIRA_MODE / KYTHIRA_BUILD / KYTHIRA_STATS / KYTHIRA_RESULT lines
// (docker/sccache_runner/run.sh) which `run_sccache` below parses.
//
// One sccache 0.10 fact shapes two scenarios and is worth knowing before
// reading them: the library compile is the only cacheable one (`bin` crate
// types are CannotCache), so a crate has exactly one cache key. The runner's
// KYTHIRA_SALT rewrites a constant in the library so a scenario can choose
// a fresh key on purpose.

using namespace std::chrono_literals;
namespace json = boost::json;
namespace os = docker_chaos::os;

namespace {

constexpr std::array<std::uint16_t, 3> k_gateway_ports{16381, 16382, 16383};
constexpr std::array<const char*, 3> k_gateway_containers{"sccache-e2e-kv1", "sccache-e2e-kv2",
                                                          "sccache-e2e-kv3"};

// Secrets as docker/sccache_e2e/acl.txt hashes them. The audit assertion
// greps the gateway logs for every one of these.
const std::map<std::string, std::string> k_secrets{
    {"ci-main", "ci-main-secret"},
    {"ci-pr", "ci-pr-secret"},
    {"elsewhere", "elsewhere-secret"},
    {"ops", "ops-secret"},
};
constexpr const char* k_internal_secret = "sccache-e2e-internal";

std::string compose_file() {
    const char* env = std::getenv("KYTHIRA_SCCACHE_E2E_COMPOSE_FILE");
    if (env && *env) return env;
    return "docker/sccache-e2e-compose.yml";
}

std::vector<std::string> compose_cmd(std::initializer_list<std::string> args) {
    auto cmd = os::compose_prefix();
    cmd.insert(cmd.end(), {"-f", compose_file()});
    cmd.insert(cmd.end(), args);
    return cmd;
}

// ── a minimal blocking RESP client ────────────────────────────────────────────
// Enough to AUTH as a configured user and issue one command at a time; the
// reply framing comes from the gateway's own parser so this stays tiny.

class resp_client {
public:
    explicit resp_client(std::uint16_t port) {
        _fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (_fd < 0) throw std::runtime_error("socket() failed");
        timeval tv{.tv_sec = 5, .tv_usec = 0};
        ::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        int one = 1;
        ::setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            ::close(_fd);
            throw std::runtime_error("connect to 127.0.0.1:" + std::to_string(port) + " refused");
        }
    }
    ~resp_client() {
        if (_fd >= 0) ::close(_fd);
    }
    resp_client(const resp_client&) = delete;
    resp_client& operator=(const resp_client&) = delete;
    resp_client(resp_client&& o) noexcept
        : _fd(std::exchange(o._fd, -1)), _buffer(std::move(o._buffer)) {}
    resp_client& operator=(resp_client&&) = delete;

    std::string call(const std::vector<std::string>& argv) {
        std::string out = "*" + std::to_string(argv.size()) + "\r\n";
        for (const auto& a : argv) out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
        for (std::size_t sent = 0; sent < out.size();) {
            auto n = ::send(_fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) throw std::runtime_error("send failed");
            sent += static_cast<std::size_t>(n);
        }
        std::size_t len = 0;
        while ((len = kythira::resp_reply_length(_buffer)) == 0) {
            std::array<char, 65536> chunk{};
            auto n = ::recv(_fd, chunk.data(), chunk.size(), 0);
            if (n <= 0)
                throw std::runtime_error("connection closed or timed out waiting for a reply");
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

// Connect to one gateway as `user`, or nullopt if the port is not up yet.
std::optional<resp_client> connect_as(std::uint16_t port, const std::string& user) {
    try {
        resp_client c(port);
        auto reply = c.call({"AUTH", user, k_secrets.at(user)});
        if (reply != "+OK\r\n") throw std::runtime_error("AUTH " + user + " answered " + reply);
        return c;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// `field:` value out of an INFO bulk reply, or -1 when absent.
long info_field(const std::string& info, const std::string& field) {
    auto pos = info.find("\r\n" + field + ":");
    if (pos == std::string::npos) return -1;
    return std::stol(info.substr(pos + field.size() + 3));
}

// Ready means: every node answers AUTH + PING and, between them, the two
// shards have a leader. sccache's startup probe needs a routable write, so
// anything less lets the first scenario race the elections.
bool wait_cluster_ready(std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        long led = 0;
        bool all_up = true;
        for (auto port : k_gateway_ports) {
            auto c = connect_as(port, "ops");
            if (!c || c->call({"PING"}) != "+PONG\r\n") {
                all_up = false;
                break;
            }
            led += std::max(0L, info_field(c->call({"INFO"}), "local_shards_led"));
        }
        if (all_up && led == 2) return true;
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

// ── the runner's report ───────────────────────────────────────────────────────

struct build_report {
    bool ok{false};
    long hits{0};
    long misses{0};
    long write_errors{0};
    long writes{0};
};

struct runner_report {
    int exit_code{-1};
    std::string mode;  // cached | uncached
    std::vector<build_report> builds;
    std::string raw;
};

long sum_counts(const json::value& stats, const char* key) {
    long total = 0;
    if (auto* obj = stats.at(key).as_object().if_contains("counts")) {
        for (const auto& kv : obj->as_object()) total += kv.value().to_number<long>();
    }
    return total;
}

// One `compose run --rm` of the runner with extra `-e` settings. Every
// SCCACHE_* default (endpoint, prefix, ci-main credentials) comes from the
// compose file, so a scenario names only what differs.
runner_report run_sccache(const std::vector<std::pair<std::string, std::string>>& env) {
    // -T: no pseudo-TTY, so the output is what run.sh printed rather than a
    // terminal's rendering of it (CRLF line ends, for one).
    std::vector<std::string> cmd = compose_cmd({"run", "--rm", "-T"});
    for (const auto& [k, v] : env) {
        cmd.push_back("-e");
        cmd.push_back(k + "=" + v);
    }
    cmd.push_back("sccache");
    auto [code, out] = os::real_exec(cmd);
    BOOST_TEST_MESSAGE("runner exit " << code << ":\n" << out);

    runner_report r;
    r.exit_code = code;
    r.raw = out;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("KYTHIRA_MODE mode=", 0) == 0) {
            r.mode = line.substr(std::string("KYTHIRA_MODE mode=").size());
        } else if (line.rfind("KYTHIRA_BUILD build=", 0) == 0) {
            build_report b;
            b.ok = line.find("status=ok") != std::string::npos;
            r.builds.push_back(b);
        } else if (line.rfind("KYTHIRA_STATS build=", 0) == 0 && !r.builds.empty()) {
            auto stats = json::parse(line.substr(line.find('{'))).at("stats");
            auto& b = r.builds.back();
            b.hits = sum_counts(stats, "cache_hits");
            b.misses = sum_counts(stats, "cache_misses");
            b.write_errors = stats.at("cache_write_errors").to_number<long>();
            b.writes = stats.at("cache_writes").to_number<long>();
        }
    }
    return r;
}

std::string gateway_logs() {
    std::string all;
    for (const auto* c : k_gateway_containers) {
        all += os::real_exec(os::docker_logs_cmd(c, 100000)).out;
    }
    return all;
}

// Each scenario gets its own cache key so the order they run in cannot
// leak state between them; see the KYTHIRA_SALT note at the top.
int next_salt() {
    static int salt = static_cast<int>(std::time(nullptr) % 100000) * 10;
    return ++salt;
}

}  // namespace

struct SccacheE2eFixture {
    SccacheE2eFixture() {
        os::try_exec(os::real_exec, compose_cmd({"down", "--remove-orphans"}));
        os::checked_exec(os::real_exec, compose_cmd({"up", "-d", "kv1", "kv2", "kv3"}));
        if (!wait_cluster_ready(90s)) {
            BOOST_TEST_MESSAGE(gateway_logs());
            os::try_exec(os::real_exec, compose_cmd({"down", "--remove-orphans"}));
            BOOST_FAIL("sccache-e2e gateway cluster did not become ready within 90 s");
        }
    }
    ~SccacheE2eFixture() {
        try {
            os::real_exec(compose_cmd({"down", "--remove-orphans"}));
        } catch (...) {
        }
    }
};

BOOST_TEST_GLOBAL_FIXTURE(SccacheE2eFixture);

// Requirement 17.1: a clean build misses, a second clean build hits.
BOOST_AUTO_TEST_CASE(clean_build_misses_then_hits, *boost::unit_test::timeout(600)) {
    auto r = run_sccache({{"KYTHIRA_SALT", std::to_string(next_salt())}});
    BOOST_REQUIRE_EQUAL(r.exit_code, 0);
    BOOST_CHECK_EQUAL(r.mode, "cached");
    BOOST_REQUIRE_EQUAL(r.builds.size(), 2u);
    BOOST_CHECK(r.builds[0].ok);
    BOOST_CHECK_EQUAL(r.builds[0].hits, 0);
    BOOST_CHECK_GT(r.builds[0].misses, 0);
    BOOST_CHECK_GT(r.builds[0].writes, 0);
    BOOST_CHECK_EQUAL(r.builds[0].write_errors, 0);
    BOOST_CHECK(r.builds[1].ok);
    BOOST_CHECK_GT(r.builds[1].hits, 0);
    BOOST_CHECK_EQUAL(r.builds[1].misses, 0);

    // The cache entries really are in the cluster, under the configured
    // prefix, and every node answers for them (Requirement 17.4's prefix).
    for (auto port : k_gateway_ports) {
        auto c = connect_as(port, "ops");
        BOOST_REQUIRE(c.has_value());
        BOOST_CHECK_EQUAL(c->call({"EXISTS", "sccache/.sccache_check"}), ":1\r\n");
    }
}

// Requirement 17.4: SCCACHE_REDIS_EXPIRATION switches sccache to SETEX and the
// gateway stores the deadline; a non-default prefix is honoured throughout.
BOOST_AUTO_TEST_CASE(expiration_uses_setex_under_a_custom_prefix, *boost::unit_test::timeout(600)) {
    auto r = run_sccache({{"KYTHIRA_SALT", std::to_string(next_salt())},
                          {"SCCACHE_REDIS_KEY_PREFIX", "sccache/ttl/"},
                          {"SCCACHE_REDIS_EXPIRATION", "600"}});
    BOOST_REQUIRE_EQUAL(r.exit_code, 0);
    BOOST_CHECK_EQUAL(r.mode, "cached");
    BOOST_REQUIRE_EQUAL(r.builds.size(), 2u);
    BOOST_CHECK_GT(r.builds[0].misses, 0);
    BOOST_CHECK_GT(r.builds[1].hits, 0);

    auto c = connect_as(k_gateway_ports[0], "ops");
    BOOST_REQUIRE(c.has_value());
    // sccache's startup probe writes `<prefix>.sccache_check` with the same
    // expiration as everything else, so it is the one key we know the name of.
    auto ttl = c->call({"TTL", "sccache/ttl/.sccache_check"});
    BOOST_TEST_MESSAGE("TTL sccache/ttl/.sccache_check -> " << ttl);
    BOOST_REQUIRE(ttl.rfind(":", 0) == 0);
    auto seconds = std::stol(ttl.substr(1));
    BOOST_CHECK_GT(seconds, 0);
    BOOST_CHECK_LE(seconds, 600);
    // ...whereas the SET path of the previous scenario carries no deadline.
    BOOST_CHECK_EQUAL(c->call({"TTL", "sccache/.sccache_check"}), ":-1\r\n");
}

// Requirement 17.2: with every gateway stopped, both builds still succeed.
// The cache is an accelerant, not a build dependency — see run.sh for the
// one thing a CI job has to do to make that true with sccache 0.10.
BOOST_AUTO_TEST_CASE(builds_succeed_with_the_gateway_down, *boost::unit_test::timeout(900)) {
    os::checked_exec(os::real_exec, compose_cmd({"stop", "kv1", "kv2", "kv3"}));
    runner_report r;
    try {
        r = run_sccache({{"KYTHIRA_SALT", std::to_string(next_salt())}});
    } catch (...) {
        os::try_exec(os::real_exec, compose_cmd({"start", "kv1", "kv2", "kv3"}));
        throw;
    }
    os::checked_exec(os::real_exec, compose_cmd({"start", "kv1", "kv2", "kv3"}));

    BOOST_CHECK_EQUAL(r.exit_code, 0);
    BOOST_CHECK_EQUAL(r.mode, "uncached");
    BOOST_REQUIRE_EQUAL(r.builds.size(), 2u);
    BOOST_CHECK(r.builds[0].ok);
    BOOST_CHECK(r.builds[1].ok);

    BOOST_REQUIRE_MESSAGE(wait_cluster_ready(90s),
                          "gateway cluster did not come back after restart");
}

// Requirements 11.1-11.3, 11.8, 17.3: role and prefix enforcement, asserted
// both on the wire and through sccache's own statistics.
BOOST_AUTO_TEST_CASE(read_only_user_hits_but_cannot_write, *boost::unit_test::timeout(900)) {
    const auto salt = std::to_string(next_salt());

    // ci-main (read_write) populates the key.
    auto main = run_sccache({{"KYTHIRA_SALT", salt}});
    BOOST_REQUIRE_EQUAL(main.exit_code, 0);
    BOOST_REQUIRE_EQUAL(main.builds.size(), 2u);
    BOOST_CHECK_GT(main.builds[1].hits, 0);

    // ci-pr (read_only) hits what ci-main stored from its very first build...
    auto pr = run_sccache({{"KYTHIRA_SALT", salt},
                           {"SCCACHE_REDIS_USERNAME", "ci-pr"},
                           {"SCCACHE_REDIS_PASSWORD", k_secrets.at("ci-pr")}});
    BOOST_REQUIRE_EQUAL(pr.exit_code, 0);
    BOOST_CHECK_EQUAL(pr.mode, "cached");
    BOOST_REQUIRE_EQUAL(pr.builds.size(), 2u);
    BOOST_CHECK(pr.builds[0].ok);
    BOOST_CHECK_GT(pr.builds[0].hits, 0);
    BOOST_CHECK_EQUAL(pr.builds[0].misses, 0);

    // ...and on a key nobody has stored, its build still succeeds while every
    // store it attempts is refused (sccache counts the refusals as
    // cache_write_errors and never demotes the build to a failure).
    auto pr_fresh = run_sccache({{"KYTHIRA_SALT", std::to_string(next_salt())},
                                 {"SCCACHE_REDIS_USERNAME", "ci-pr"},
                                 {"SCCACHE_REDIS_PASSWORD", k_secrets.at("ci-pr")}});
    BOOST_REQUIRE_EQUAL(pr_fresh.exit_code, 0);
    BOOST_CHECK_EQUAL(pr_fresh.mode, "cached");
    BOOST_REQUIRE_EQUAL(pr_fresh.builds.size(), 2u);
    for (const auto& b : pr_fresh.builds) {
        BOOST_CHECK(b.ok);
        BOOST_CHECK_GT(b.misses, 0);
        BOOST_CHECK_GT(b.write_errors, 0);
        BOOST_CHECK_EQUAL(b.writes, 0);
    }

    // The wire-level refusal is -NOPERM, on every node, and reads still work.
    for (auto port : k_gateway_ports) {
        auto c = connect_as(port, "ci-pr");
        BOOST_REQUIRE(c.has_value());
        BOOST_CHECK_EQUAL(c->call({"EXISTS", "sccache/.sccache_check"}), ":1\r\n");
        BOOST_CHECK(c->call({"SET", "sccache/x", "y"}).rfind("-NOPERM", 0) == 0);
        BOOST_CHECK(c->call({"DEL", "sccache/.sccache_check"}).rfind("-NOPERM", 0) == 0);
    }
}

// Requirement 11.2, 17.3: a user whose prefix is elsewhere is refused both
// directions, so sccache's startup probe fails and the build runs uncached.
BOOST_AUTO_TEST_CASE(wrong_prefix_user_is_refused_and_builds_uncached,
                     *boost::unit_test::timeout(600)) {
    auto r = run_sccache({{"KYTHIRA_SALT", std::to_string(next_salt())},
                          {"SCCACHE_REDIS_USERNAME", "elsewhere"},
                          {"SCCACHE_REDIS_PASSWORD", k_secrets.at("elsewhere")}});
    BOOST_CHECK_EQUAL(r.exit_code, 0);
    BOOST_CHECK_EQUAL(r.mode, "uncached");
    BOOST_REQUIRE_EQUAL(r.builds.size(), 2u);
    BOOST_CHECK(r.builds[0].ok);
    BOOST_CHECK(r.builds[1].ok);
    BOOST_CHECK(r.raw.find("cache storage failed to read") != std::string::npos);

    auto c = connect_as(k_gateway_ports[1], "elsewhere");
    BOOST_REQUIRE(c.has_value());
    BOOST_CHECK(c->call({"GET", "sccache/.sccache_check"}).rfind("-NOPERM", 0) == 0);
    BOOST_CHECK(c->call({"SET", "sccache/.sccache_check", "x"}).rfind("-NOPERM", 0) == 0);
    BOOST_CHECK_EQUAL(c->call({"SET", "elsewhere/x", "y"}), "+OK\r\n");
}

// Requirement 11.8: every denial above left an audit line, and none of them —
// nor anything else the gateways logged — carries a secret.
BOOST_AUTO_TEST_CASE(denials_are_audited_without_secrets, *boost::unit_test::timeout(120)) {
    auto logs = gateway_logs();
    auto has = [&](const std::string& needle) { return logs.find(needle) != std::string::npos; };

    BOOST_CHECK(has("redis audit"));
    BOOST_CHECK(has("[identity=ci-pr] [source="));
    BOOST_CHECK(has("[identity=ci-pr]") && has("[command=SET] [reason=command denied]"));
    BOOST_CHECK(has("[identity=elsewhere]") && has("[command=GET] [reason=key out of scope]"));
    BOOST_CHECK(has("[command=DEL] [reason=command denied]"));

    for (const auto& [user, secret] : k_secrets) {
        BOOST_CHECK_MESSAGE(!has(secret), "gateway logs contain the secret of " << user);
    }
    BOOST_CHECK(!has(k_internal_secret));
}
