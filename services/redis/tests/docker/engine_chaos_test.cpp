// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE engine_chaos_test
#include <boost/test/results_collector.hpp>
#include <boost/test/tree/observer.hpp>
#include <boost/test/unit_test.hpp>

#include "checkers/linearizability.hpp"
#include "history.hpp"
#include "os_faults.hpp"

#include <kythira/redis/resp_protocol.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// redis_gateway_node on engine storage in containers.
// services/redis/docker/engine-chaos-compose.yml starts three nodes with
// KYTHIRA_CACHE_PERSISTENCE=engine, each publishing RESP on 1641<n>. Two
// cases share the stack:
//
//   1. .kiro/specs/persistent-state-machine/ Requirement 9.3. Clients GET
//      and SET a handful of keys through every node while the test SIGKILLs
//      replicas and starts them again: one at a time, two at once (the
//      cluster loses its quorum), and all three. Every operation goes into
//      a Jepsen-style history, and the history must pass the
//      linearizability checker (tests/docker_chaos/checkers/). A write that
//      was acknowledged and then lost to a kill, or applied twice from a
//      replayed log, shows up there as a read no linearization explains.
//
//   2. .kiro/specs/file-install-snapshot/ Requirement 8.3. A follower is
//      stopped while the others take a bulk load large enough to compact
//      the leader's log past it many times, so catching it up takes a
//      format-1 snapshot transfer (paced by
//      KYTHIRA_CACHE_SNAPSHOT_BYTES_PER_SEC so it lasts seconds). Mid
//      transfer the follower is cut off the network, then SIGKILLed and
//      started again. It must still finish an install, and then hold the
//      load.
//
// Unlike the transaction chaos test, killed nodes are started again: the
// Raft log, vote and values are on each node's volume.

using namespace std::chrono_literals;
namespace os = docker_chaos::os;
namespace history = docker_chaos::history;
namespace lin = docker_chaos::checkers::linearizability;

namespace {

constexpr std::array<std::uint16_t, 3> k_resp_ports{16411, 16412, 16413};
constexpr std::array<const char*, 3> k_containers{"engine-chaos-1", "engine-chaos-2",
                                                  "engine-chaos-3"};
constexpr std::array<const char*, 3> k_services{"engine-1", "engine-2", "engine-3"};
constexpr const char* k_network = "kythira_engine_chaos_net";
/// Keys of the linearizability case; "k/2" is the shard cut, so they span
/// both shards. Enough of them, and a slow enough load (k_think), that
/// each key's history stays a few hundred operations: the checker's search
/// is exponential in the worst case, and the first draft's 3,600
/// operations a key ran every search out of steps (`unknown`).
constexpr int k_keys = 30;
constexpr auto k_think = std::chrono::milliseconds{100};
/// The transfer case's bulk load: incompressible values, so the RocksDB
/// checkpoint is about as large as the load.
constexpr int k_bulk_values = 600;
constexpr std::size_t k_bulk_value_bytes = 64 * 1024;
/// The follower the transfer case stops and catches up.
constexpr std::size_t k_lagging = 2;

std::string compose_file() {
    const char* env = std::getenv("KYTHIRA_ENGINE_CHAOS_COMPOSE_FILE");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return "services/redis/docker/engine-chaos-compose.yml";
}

std::vector<std::string> compose_cmd(std::initializer_list<std::string> args) {
    auto cmd = os::compose_prefix();
    cmd.insert(cmd.end(), {"-f", compose_file()});
    cmd.insert(cmd.end(), args);
    return cmd;
}

std::string key(int i) {
    return "k/" + std::to_string(i);
}

std::string bulk_key(int i) {
    return "big/" + std::to_string(i);
}

/// A connected loopback TCP socket with a receive timeout, or -1.
int dial(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    timeval tv{.tv_sec = 15, .tv_usec = 0};
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

class resp_client {
public:
    explicit resp_client(std::uint16_t port) : _fd(dial(port)) {
        if (_fd < 0) {
            throw std::runtime_error("connect to 127.0.0.1:" + std::to_string(port) + " refused");
        }
        if (call({"AUTH", "ops", "ops-secret"}) != "+OK\r\n") {
            throw std::runtime_error("AUTH refused");
        }
    }
    ~resp_client() {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }
    resp_client(const resp_client&) = delete;
    resp_client& operator=(const resp_client&) = delete;
    resp_client(resp_client&&) = delete;
    resp_client& operator=(resp_client&&) = delete;

    std::string call(const std::vector<std::string>& argv) {
        std::string out = "*" + std::to_string(argv.size()) + "\r\n";
        for (const auto& a : argv) {
            out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
        }
        for (std::size_t sent = 0; sent < out.size();) {
            auto n = ::send(_fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                throw std::runtime_error("send failed");
            }
            sent += static_cast<std::size_t>(n);
        }
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

/// A GET reply: nullopt for an error, an inner nullopt for nil, else the
/// value.
std::optional<std::optional<std::string>> parse_get(const std::string& reply) {
    if (reply == "$-1\r\n") {
        return std::optional<std::string>{};
    }
    if (reply.rfind('$', 0) != 0) {
        return std::nullopt;
    }
    const auto crlf = reply.find("\r\n");
    const auto len = std::stoul(reply.substr(1, crlf - 1));
    return std::optional<std::string>{reply.substr(crlf + 2, len)};
}

/// The identity a value carries before its padding: "p3-17:xxxx" -> "p3-17".
std::string identity(const std::string& value) {
    return value.substr(0, value.find(':'));
}

std::string g_not_ready_reason;

/// Every node in `nodes` answers a linearizable GET (so both shards have a
/// leader that can reach a quorum).
bool wait_ready(std::chrono::milliseconds timeout, std::vector<std::size_t> nodes = {0, 1, 2}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool all = true;
        for (auto i : nodes) {
            try {
                resp_client c(k_resp_ports[i]);
                for (const auto* k : {"k/0", "k/9"}) {
                    const auto reply = c.call({"GET", k});
                    if (!parse_get(reply).has_value()) {
                        g_not_ready_reason = "port " + std::to_string(k_resp_ports[i]) +
                                             " replied " + reply.substr(0, 200);
                        all = false;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                g_not_ready_reason =
                    "port " + std::to_string(k_resp_ports[i]) + " threw " + e.what();
                all = false;
            }
            if (!all) {
                break;
            }
        }
        if (all) {
            return true;
        }
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

std::string logs_of(std::size_t i, int lines) {
    return os::real_exec(os::docker_logs_cmd(k_containers[i], lines)).out;
}

std::string node_logs() {
    std::string all;
    for (std::size_t i = 0; i < k_containers.size(); ++i) {
        all += "── " + std::string(k_containers[i]) + " ──\n" + logs_of(i, 200);
    }
    return all;
}

std::size_t occurrences(const std::string& text, const std::string& needle) {
    std::size_t n = 0;
    for (auto pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

/// Occurrences of `needle` in a container's whole log, across restarts.
std::size_t log_count(std::size_t i, const std::string& needle) {
    return occurrences(os::real_exec(os::container_logs_all_cmd(k_containers[i])).out, needle);
}

// A kill that fails means the container was not running: it exited on
// its own, which is the failure worth reporting, not the kill.
void kill_node(std::size_t i) {
    BOOST_TEST_MESSAGE("SIGKILL " << k_containers[i]);
    const auto r = os::real_exec(os::docker_kill_cmd(k_containers[i]));
    BOOST_REQUIRE_MESSAGE(r.code == 0, k_containers[i] << " was not running when the test went to "
                                                          "kill it (it exited by itself): "
                                                       << r.out);
}

void start_node(std::size_t i) {
    BOOST_TEST_MESSAGE("start " << k_containers[i]);
    const auto r = os::real_exec(os::docker_start_cmd(k_containers[i]));
    BOOST_REQUIRE_MESSAGE(r.code == 0, "start " << k_containers[i] << " failed: " << r.out);
}

/// Stops and joins the workload threads however the test body leaves, so
/// a failed assertion never destroys a joinable thread.
class workers_guard {
public:
    workers_guard(std::atomic<bool>& stop, std::vector<std::thread>& threads)
        : _stop(stop), _threads(threads) {}
    workers_guard(const workers_guard&) = delete;
    workers_guard& operator=(const workers_guard&) = delete;
    workers_guard(workers_guard&&) = delete;
    workers_guard& operator=(workers_guard&&) = delete;
    ~workers_guard() {
        _stop = true;
        for (auto& t : _threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

private:
    std::atomic<bool>& _stop;
    std::vector<std::thread>& _threads;
};

/// The history, in the order events happened: each is timestamped and
/// appended under one lock.
class recorder {
public:
    auto invoke(std::int64_t p, history::op_kind f, const std::string& k,
                std::optional<std::string> value) -> void {
        add(history::event{.process = p,
                           .type = history::event_type::invoke,
                           .f = f,
                           .key = k,
                           .value = std::move(value),
                           .mode = f == history::op_kind::read ? "linearizable" : ""});
    }
    auto complete(std::int64_t p, history::event_type type, history::op_kind f,
                  const std::string& k, std::optional<std::string> value, int node,
                  std::string error = {}) -> void {
        add(history::event{.process = p,
                           .type = type,
                           .f = f,
                           .key = k,
                           .value = std::move(value),
                           .mode = f == history::op_kind::read ? "linearizable" : "",
                           .node = node,
                           .error = std::move(error)});
    }
    auto take() -> std::vector<history::event> {
        std::lock_guard lock(_mutex);
        return _events;
    }
    auto next_process() -> std::int64_t { return _next_process++; }

private:
    auto add(history::event e) -> void {
        std::lock_guard lock(_mutex);
        e.time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - _start)
                        .count();
        _events.push_back(std::move(e));
    }

    std::mutex _mutex;
    std::vector<history::event> _events;
    std::atomic<std::int64_t> _next_process{1};
    std::chrono::steady_clock::time_point _start{std::chrono::steady_clock::now()};
};

struct counts {
    std::atomic<long> ok_writes{0};
    std::atomic<long> ok_reads{0};
    std::atomic<long> indeterminate{0};
    std::atomic<long> refused_connects{0};
};

/// One logical client: a process in the history until an operation's
/// outcome is unknown, then a fresh process (Jepsen's rule: a process with
/// an `info` never invokes again). It starts on node `home` and moves to
/// the next node whenever its node stops answering.
class client {
public:
    client(recorder& rec, counts& n, std::size_t home, unsigned seed)
        : _rec(rec), _n(n), _node(home), _rng(seed), _p(rec.next_process()) {}

    auto step() -> void {
        if (!_conn) {
            try {
                _conn.emplace(k_resp_ports[_node]);
            } catch (const std::exception&) {
                // Nothing invoked yet: no history entry.
                ++_n.refused_connects;
                _node = (_node + 1) % k_resp_ports.size();
                std::this_thread::sleep_for(100ms);
                return;
            }
        }
        const auto k = key(std::uniform_int_distribution<int>(0, k_keys - 1)(_rng));
        if (std::bernoulli_distribution(0.5)(_rng)) {
            read(k);
        } else {
            write(k);
        }
    }

    auto final_read(const std::string& k) -> void {
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (!_conn) {
                try {
                    _conn.emplace(k_resp_ports[_node]);
                } catch (const std::exception&) {
                    _node = (_node + 1) % k_resp_ports.size();
                    std::this_thread::sleep_for(250ms);
                    continue;
                }
            }
            if (read(k)) {
                return;
            }
            std::this_thread::sleep_for(250ms);
        }
        BOOST_ERROR("no final read of " << k << " succeeded");
    }

private:
    [[nodiscard]] auto node_id() const -> int { return static_cast<int>(_node) + 1; }

    /// Drops the connection and retires the process after an operation
    /// whose outcome is unknown.
    auto lost(history::op_kind f, const std::string& k, std::optional<std::string> value,
              const std::string& why) -> void {
        _rec.complete(_p, history::event_type::info, f, k, std::move(value), node_id(), why);
        ++_n.indeterminate;
        _p = _rec.next_process();
        _conn.reset();
        _node = (_node + 1) % k_resp_ports.size();
    }

    auto read(const std::string& k) -> bool {
        _rec.invoke(_p, history::op_kind::read, k, std::nullopt);
        std::string reply;
        try {
            reply = _conn->call({"GET", k});
        } catch (const std::exception& e) {
            lost(history::op_kind::read, k, std::nullopt, e.what());
            return false;
        }
        auto got = parse_get(reply);
        if (!got) {
            // A read has no effect, so a refused one is a plain failure.
            _rec.complete(_p, history::event_type::fail, history::op_kind::read, k, std::nullopt,
                          node_id(), reply.substr(0, 200));
            return false;
        }
        std::optional<std::string> value;
        if (*got) {
            value = identity(**got);
        }
        _rec.complete(_p, history::event_type::ok, history::op_kind::read, k, std::move(value),
                      node_id());
        ++_n.ok_reads;
        return true;
    }

    auto write(const std::string& k) -> void {
        const auto id = "p" + std::to_string(_p) + "-" + std::to_string(_seq++);
        // 1 KiB of padding, so the load also compacts logs (the threshold is
        // 4 MiB) and a restarted node may need a snapshot, not just entries.
        const auto value = id + ":" + std::string(1024, static_cast<char>('a' + (_seq % 26)));
        _rec.invoke(_p, history::op_kind::write, k, id);
        std::string reply;
        try {
            reply = _conn->call({"SET", k, value});
        } catch (const std::exception& e) {
            lost(history::op_kind::write, k, id, e.what());
            return;
        }
        if (reply == "+OK\r\n") {
            _rec.complete(_p, history::event_type::ok, history::op_kind::write, k, id, node_id());
            ++_n.ok_writes;
            return;
        }
        // An error reply can come after the command was proposed (a timeout
        // waiting for the commit, a leader that stepped down), so it does
        // not prove the write had no effect.
        lost(history::op_kind::write, k, id, reply.substr(0, 200));
    }

    recorder& _rec;
    counts& _n;
    std::size_t _node;
    std::mt19937 _rng;
    std::int64_t _p;
    std::uint64_t _seq{0};
    std::optional<resp_client> _conn;
};

std::string random_value(std::mt19937& rng, std::size_t bytes) {
    std::string s(bytes, 'a');
    std::uniform_int_distribution<int> letter(0, 25);
    for (auto& c : s) {
        c = static_cast<char>('a' + letter(rng));
    }
    return s;
}

/// SET through whichever node answers, retrying until it acknowledges.
void set_until_acknowledged(const std::string& k, const std::string& v,
                            std::vector<std::size_t> nodes) {
    for (int attempt = 0; attempt < 120; ++attempt) {
        try {
            resp_client c(k_resp_ports[nodes[static_cast<std::size_t>(attempt) % nodes.size()]]);
            if (c.call({"SET", k, v}) == "+OK\r\n") {
                return;
            }
        } catch (const std::exception&) {
        }
        std::this_thread::sleep_for(500ms);
    }
    BOOST_FAIL("SET " << k << " was never acknowledged");
}

std::uint64_t engine_bytes(std::size_t i) {
    const auto out = os::real_exec({os::container_runtime(), "exec", k_containers[i], "du", "-sb",
                                    "/data/engine"})
                         .out;
    try {
        return std::stoull(out);
    } catch (const std::exception&) {
        return 0;
    }
}

struct NodeLogsOnFailure : boost::unit_test::test_observer {
    void test_unit_finish(const boost::unit_test::test_unit& tu, unsigned long) override {
        if (tu.p_type != boost::unit_test::TUT_CASE ||
            boost::unit_test::results_collector.results(tu.p_id).passed()) {
            return;
        }
        std::cerr << "── node logs after " << tu.full_name() << " failed ──\n" << node_logs();
    }
};

}  // namespace

struct EngineChaosFixture {
    EngineChaosFixture() {
        boost::unit_test::framework::register_observer(_observer);
        os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
        os::checked_exec(os::real_exec, compose_cmd({"up", "-d"}));
        if (!wait_ready(120s)) {
            BOOST_TEST_MESSAGE(node_logs());
            os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
            BOOST_FAIL(
                "engine chaos cluster did not become ready within 120 s: " << g_not_ready_reason);
        }
    }
    ~EngineChaosFixture() {
        boost::unit_test::framework::deregister_observer(_observer);
        try {
            os::real_exec(compose_cmd({"down", "-v", "--remove-orphans"}));
        } catch (...) {
        }
    }
    EngineChaosFixture(const EngineChaosFixture&) = delete;
    EngineChaosFixture& operator=(const EngineChaosFixture&) = delete;
    EngineChaosFixture(EngineChaosFixture&&) = delete;
    EngineChaosFixture& operator=(EngineChaosFixture&&) = delete;

private:
    NodeLogsOnFailure _observer;
};

BOOST_TEST_GLOBAL_FIXTURE(EngineChaosFixture);

BOOST_AUTO_TEST_CASE(sigkilled_replicas_keep_the_history_linearizable,
                     *boost::unit_test::timeout(1200)) {
    recorder rec;
    counts n;
    std::atomic<bool> stop{false};
    constexpr std::size_t k_clients = 6;
    std::vector<std::thread> workers;
    const workers_guard guard(stop, workers);
    for (std::size_t w = 0; w < k_clients; ++w) {
        workers.emplace_back([&, w] {
            client c(rec, n, w % k_resp_ports.size(), static_cast<unsigned>(w) + 1);
            while (!stop.load()) {
                c.step();
                std::this_thread::sleep_for(k_think);
            }
        });
    }

    // Faults. Each kill is SIGKILL, so nothing is flushed on the way down;
    // whatever a node acknowledged must already be on its disk.
    std::this_thread::sleep_for(5s);
    for (std::size_t round = 0; round < 2; ++round) {
        for (std::size_t i = 0; i < k_containers.size(); ++i) {
            kill_node(i);
            std::this_thread::sleep_for(5s);
            start_node(i);
            std::this_thread::sleep_for(10s);
        }
    }
    // Two at once: no quorum until they are back, and both come back from
    // their own logs.
    kill_node(0);
    kill_node(1);
    std::this_thread::sleep_for(4s);
    start_node(0);
    start_node(1);
    std::this_thread::sleep_for(10s);
    // The whole cluster: every acknowledged write must survive on disk.
    for (std::size_t i = 0; i < k_containers.size(); ++i) {
        kill_node(i);
    }
    std::this_thread::sleep_for(3s);
    for (std::size_t i = 0; i < k_containers.size(); ++i) {
        start_node(i);
    }
    BOOST_REQUIRE_MESSAGE(wait_ready(180s),
                          "cluster not ready after the full restart: " << g_not_ready_reason);
    std::this_thread::sleep_for(10s);
    stop = true;
    for (auto& t : workers) {
        t.join();
    }

    // Close the history with a read of every key through every node.
    for (std::size_t i = 0; i < k_resp_ports.size(); ++i) {
        client c(rec, n, i, 1000U + static_cast<unsigned>(i));
        for (int k = 0; k < k_keys; ++k) {
            c.final_read(key(k));
        }
    }

    const auto events = rec.take();
    BOOST_REQUIRE_EQUAL(history::structural_error(events), "");
    const auto result = lin::check(events, lin::options{.max_steps_per_key = 20'000'000});
    for (const auto* v : result.violations()) {
        BOOST_TEST_MESSAGE("violation: " << boost::json::serialize(lin::violation_json(*v)));
    }
    for (const auto& k : result.keys) {
        BOOST_TEST_MESSAGE(k.key << ": " << k.result << ", " << k.operations << " operations, "
                                 << k.steps << " steps");
    }
    BOOST_TEST_MESSAGE("ok writes " << n.ok_writes.load() << ", ok reads " << n.ok_reads.load()
                                    << ", indeterminate " << n.indeterminate.load()
                                    << ", refused connects " << n.refused_connects.load() << ", "
                                    << events.size() << " events");
    // `unknown` (the search ran out of steps) is not a pass.
    BOOST_CHECK_EQUAL(result.overall, docker_chaos::checkers::verdict::ok);
    // The load was real, on both sides of the faults.
    BOOST_CHECK_GT(n.ok_writes.load(), 200);
    BOOST_CHECK_GT(n.ok_reads.load(), 200);
}

BOOST_AUTO_TEST_CASE(a_lagging_follower_cut_off_and_killed_mid_transfer_still_catches_up,
                     *boost::unit_test::timeout(1200)) {
    BOOST_REQUIRE_MESSAGE(wait_ready(120s), g_not_ready_reason);
    kill_node(k_lagging);

    // The bulk load, through the two survivors. Values are random letters
    // so RocksDB cannot compress the checkpoint away.
    std::mt19937 rng(42);
    std::vector<std::string> prefixes;
    const std::vector<std::size_t> survivors{0, 1};
    for (int i = 0; i < k_bulk_values; ++i) {
        prefixes.push_back("v" + std::to_string(i));
        set_until_acknowledged(
            bulk_key(i), prefixes.back() + ":" + random_value(rng, k_bulk_value_bytes), survivors);
    }
    BOOST_TEST_MESSAGE("bulk load of " << k_bulk_values << " x " << k_bulk_value_bytes
                                       << " B acknowledged");

    const std::string started = "Started staging a format-1 snapshot";
    const std::string installed = "Successfully installed snapshot";
    const auto started_before = log_count(k_lagging, started);
    start_node(k_lagging);

    auto wait_for = [&](const std::string& needle, std::size_t above,
                        std::chrono::seconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (log_count(k_lagging, needle) > above) {
                return true;
            }
            std::this_thread::sleep_for(250ms);
        }
        return false;
    };
    BOOST_REQUIRE_MESSAGE(wait_for(started, started_before, 90s),
                          "the lagging follower never started a format-1 transfer:\n"
                              << logs_of(k_lagging, 200));
    const auto installed_mid = log_count(k_lagging, installed);

    // Mid transfer: cut it off, let it come back, then kill it outright.
    std::this_thread::sleep_for(2s);
    BOOST_TEST_MESSAGE("isolate " << k_containers[k_lagging]);
    os::checked_exec(os::real_exec, os::network_disconnect_cmd(k_network, k_containers[k_lagging]));
    std::this_thread::sleep_for(4s);
    os::checked_exec(os::real_exec, os::network_connect_cmd(k_network, k_containers[k_lagging],
                                                            k_services[k_lagging]));
    std::this_thread::sleep_for(2s);
    // The transfer is paced to seconds; if it beat the faults anyway the
    // case proves less than it claims, so say so.
    BOOST_WARN_EQUAL(log_count(k_lagging, installed), installed_mid);
    const auto installed_before_kill = log_count(k_lagging, installed);
    kill_node(k_lagging);
    std::this_thread::sleep_for(2s);
    start_node(k_lagging);

    BOOST_REQUIRE_MESSAGE(wait_for(installed, installed_before_kill, 300s),
                          "the lagging follower never finished an install after the kill:\n"
                              << logs_of(k_lagging, 300));

    // It holds the load: its database is about the size of it, and the
    // cluster serves every value with the follower in the quorum.
    const auto bytes = engine_bytes(k_lagging);
    BOOST_TEST_MESSAGE(k_containers[k_lagging] << " engine/ holds " << bytes << " B");
    BOOST_CHECK_GE(bytes, std::uint64_t{k_bulk_values} * k_bulk_value_bytes / 2);
    BOOST_REQUIRE_MESSAGE(wait_ready(120s), g_not_ready_reason);
    kill_node(0);  // the follower is now one of the two voters left
    BOOST_REQUIRE_MESSAGE(wait_ready(120s, {1, 2}), g_not_ready_reason);
    resp_client c(k_resp_ports[k_lagging]);
    for (int i = 0; i < k_bulk_values; ++i) {
        std::optional<std::optional<std::string>> got;
        for (int attempt = 0; attempt < 20 && !got; ++attempt) {
            got = parse_get(c.call({"GET", bulk_key(i)}));
            if (!got) {
                std::this_thread::sleep_for(250ms);
            }
        }
        BOOST_REQUIRE(got.has_value());
        BOOST_REQUIRE(got->has_value());
        BOOST_CHECK_EQUAL(identity(**got), prefixes[static_cast<std::size_t>(i)]);
    }
    // And it takes writes as a voter of the two-node quorum.
    BOOST_CHECK_EQUAL(c.call({"SET", "k/0", "after-transfer"}), "+OK\r\n");
    start_node(0);
    BOOST_CHECK(wait_ready(120s));
}
