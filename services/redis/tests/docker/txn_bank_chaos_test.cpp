// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE txn_bank_chaos_test
#include <boost/test/results_collector.hpp>
#include <boost/test/tree/observer.hpp>
#include <boost/test/unit_test.hpp>

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

// The tier-2/3 bank through `redis_gateway_node` in containers
// (.kiro/specs/multi-raft-transactions/ task 15, Requirements 13.5 and
// 13.6). services/redis/docker/txn-chaos-compose.yml starts three nodes built for snapshot
// isolation, each publishing RESP on 1640<n>. Eight accounts span two
// shards. One client per node moves money with WATCH/MULTI/EXEC
// read-modify-write transfers; a reader sums every account with one MGET,
// which under snapshot isolation reads one point in time; and a fault thread
// cuts nodes off the network, freezes them, and finally kills one. Money is never
// created or destroyed, so every complete MGET, and the final one, must sum
// to the opening total.
//
// What the transfer leans on: WATCH validates both balances at commit
// (task 16), so a transfer computed from balances someone else changed
// meanwhile answers a null array and is retried, never applied. Without
// it, two concurrent transfers from one account would both commit and the
// total would drift.

using namespace std::chrono_literals;
namespace os = docker_chaos::os;

namespace {

constexpr std::array<std::uint16_t, 3> k_resp_ports{16401, 16402, 16403};
constexpr std::array<const char*, 3> k_containers{"txn-chaos-1", "txn-chaos-2", "txn-chaos-3"};
constexpr std::array<const char*, 3> k_services{"txn-1", "txn-2", "txn-3"};
constexpr const char* k_network = "kythira_txn_chaos_net";
/// The node killed at the end, and not started again.
constexpr std::size_t k_killed = 2;
constexpr int k_accounts = 8;
constexpr long k_opening = 100;
constexpr long k_total = k_accounts * k_opening;

std::string compose_file() {
    const char* env = std::getenv("KYTHIRA_TXN_CHAOS_COMPOSE_FILE");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return "services/redis/docker/txn-chaos-compose.yml";
}

std::vector<std::string> compose_cmd(std::initializer_list<std::string> args) {
    auto cmd = os::compose_prefix();
    cmd.insert(cmd.end(), {"-f", compose_file()});
    cmd.insert(cmd.end(), args);
    return cmd;
}

std::string account(int i) {
    return "acct/" + std::to_string(i);
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

/// The integers of an array reply of bulk strings; nullopt for anything
/// else (an error, or a missing account).
std::optional<std::vector<long>> parse_numbers(const std::string& reply) {
    if (reply.rfind('*', 0) != 0) {
        return std::nullopt;
    }
    std::vector<long> out;
    std::size_t pos = reply.find("\r\n") + 2;
    while (pos < reply.size()) {
        if (reply[pos] != '$') {
            return std::nullopt;
        }
        const auto crlf = reply.find("\r\n", pos);
        const long len = std::stol(reply.substr(pos + 1, crlf - pos - 1));
        if (len < 0) {
            return std::nullopt;
        }
        out.push_back(std::stol(reply.substr(crlf + 2, static_cast<std::size_t>(len))));
        pos = crlf + 2 + static_cast<std::size_t>(len) + 2;
    }
    return out;
}

std::vector<std::string> mget_all() {
    std::vector<std::string> argv{"MGET"};
    for (int i = 0; i < k_accounts; ++i) {
        argv.push_back(account(i));
    }
    return argv;
}

long sum(const std::vector<long>& v) {
    long s = 0;
    for (auto x : v) {
        s += x;
    }
    return s;
}

/// Every node answers, and a snapshot MGET of every account succeeds
/// through each of them (so both shards and the oracle have leaders).
/// Every node but `skip` answers a full MGET (seeded: with numbers).
// Why the last wait_ready() poll was not ready: the port and the reply (or
// exception) it got, so a timeout says which gateway was still refusing.
std::string g_not_ready_reason;

bool wait_ready(std::chrono::milliseconds timeout, bool seeded,
                std::size_t skip = k_resp_ports.size()) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool all = true;
        for (std::size_t i = 0; i < k_resp_ports.size(); ++i) {
            if (i == skip) {
                continue;
            }
            try {
                resp_client c(k_resp_ports[i]);
                const auto reply = c.call(mget_all());
                if (seeded ? !parse_numbers(reply).has_value() : reply.rfind('*', 0) != 0) {
                    g_not_ready_reason = "port " + std::to_string(k_resp_ports[i]) + " replied " +
                                         reply.substr(0, 200);
                    all = false;
                    break;
                }
            } catch (const std::exception& e) {
                g_not_ready_reason =
                    "port " + std::to_string(k_resp_ports[i]) + " threw " + e.what();
                all = false;
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

std::string node_logs() {
    std::string all;
    for (const auto* c : k_containers) {
        all += "── " + std::string(c) + " ──\n" + os::real_exec(os::docker_logs_cmd(c, 200)).out;
    }
    return all;
}

struct counts {
    std::atomic<long> committed{0};
    std::atomic<long> watch_aborts{0};
    std::atomic<long> errors{0};
    std::atomic<long> unknown{0};
    std::atomic<long> reads{0};
    std::atomic<long> torn{0};
};

/// One transfer: WATCH both, read both, then move `amount` in a MULTI.
/// Returns false when the EXEC did not commit (a watch abort or an error);
/// the caller just tries another.
bool transfer(resp_client& c, int from, int to, long amount, counts& n) {
    const auto a = account(from);
    const auto b = account(to);
    if (c.call({"WATCH", a, b}) != "+OK\r\n") {
        ++n.errors;
        return false;
    }
    auto balances = parse_numbers(c.call({"MGET", a, b}));
    if (!balances || (*balances)[0] < amount) {
        c.call({"UNWATCH"});
        return false;
    }
    c.call({"MULTI"});
    c.call({"SET", a, std::to_string((*balances)[0] - amount)});
    c.call({"SET", b, std::to_string((*balances)[1] + amount)});
    const auto exec = c.call({"EXEC"});
    if (exec == "*2\r\n+OK\r\n+OK\r\n") {
        ++n.committed;
        return true;
    }
    if (exec == "*-1\r\n") {
        ++n.watch_aborts;
    } else if (exec.find("outcome unknown") != std::string::npos) {
        // Atomic either way: the transfer applied whole or not at all.
        ++n.unknown;
    } else {
        ++n.errors;
    }
    return false;
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

struct TxnChaosFixture {
    TxnChaosFixture() {
        boost::unit_test::framework::register_observer(_observer);
        os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
        os::checked_exec(os::real_exec, compose_cmd({"up", "-d"}));
        if (!wait_ready(120s, false)) {
            BOOST_TEST_MESSAGE(node_logs());
            os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
            BOOST_FAIL("transaction chaos cluster did not become ready within 120 s");
        }
    }
    ~TxnChaosFixture() {
        boost::unit_test::framework::deregister_observer(_observer);
        try {
            os::real_exec(compose_cmd({"down", "-v", "--remove-orphans"}));
        } catch (...) {
        }
    }
    TxnChaosFixture(const TxnChaosFixture&) = delete;
    TxnChaosFixture& operator=(const TxnChaosFixture&) = delete;
    TxnChaosFixture(TxnChaosFixture&&) = delete;
    TxnChaosFixture& operator=(TxnChaosFixture&&) = delete;

private:
    NodeLogsOnFailure _observer;
};

BOOST_TEST_GLOBAL_FIXTURE(TxnChaosFixture);

BOOST_AUTO_TEST_CASE(the_bank_keeps_its_total_through_crashes_and_partitions,
                     *boost::unit_test::timeout(900)) {
    {
        resp_client seed(k_resp_ports[0]);
        std::vector<std::string> argv{"MSET"};
        for (int i = 0; i < k_accounts; ++i) {
            argv.push_back(account(i));
            argv.push_back(std::to_string(k_opening));
        }
        BOOST_REQUIRE_EQUAL(seed.call(argv), "+OK\r\n");
    }
    BOOST_REQUIRE(wait_ready(60s, true));

    counts n;
    std::atomic<bool> stop{false};
    std::mutex log_mutex;
    std::vector<std::thread> workers;
    for (std::size_t w = 0; w < k_resp_ports.size(); ++w) {
        workers.emplace_back([&, w] {
            std::mt19937 rng(static_cast<unsigned>(w) + 1);
            std::uniform_int_distribution<int> pick(0, k_accounts - 1);
            std::uniform_int_distribution<long> amount(1, 20);
            std::optional<resp_client> c;
            while (!stop.load()) {
                try {
                    if (!c) {
                        c.emplace(k_resp_ports[w]);
                    }
                    const int from = pick(rng);
                    int to = pick(rng);
                    if (to == from) {
                        to = (to + 1) % k_accounts;
                    }
                    transfer(*c, from, to, amount(rng), n);
                } catch (const std::exception&) {
                    // The node is down or cut off: reconnect, maybe elsewhere
                    // later. The transfer in flight applied whole or not.
                    ++n.errors;
                    c.reset();
                    std::this_thread::sleep_for(200ms);
                }
            }
        });
    }
    // The reader rotates over the nodes; each complete answer is one
    // snapshot and must hold the whole total.
    workers.emplace_back([&] {
        std::size_t next = 0;
        while (!stop.load()) {
            try {
                resp_client c(k_resp_ports[next++ % k_resp_ports.size()]);
                for (int i = 0; i < 20 && !stop.load(); ++i) {
                    auto balances = parse_numbers(c.call(mget_all()));
                    if (!balances) {
                        continue;  // a retryable error under the faults
                    }
                    ++n.reads;
                    if (sum(*balances) != k_total) {
                        ++n.torn;
                        std::lock_guard lock(log_mutex);
                        BOOST_TEST_MESSAGE("snapshot sums to " << sum(*balances));
                    }
                }
            } catch (const std::exception&) {
                std::this_thread::sleep_for(200ms);
            }
        }
    });

    // Faults, one node at a time so two of three always form a quorum:
    // every node in turn is cut off the network and reconnected, then
    // frozen and thawed (a coordinator stalls mid-transaction and its locks
    // outlive their TTL). Last, one node is killed for good, taking its
    // in-flight coordinators with it, and the workload goes on over the
    // other two.
    //
    // No node is killed and *restarted*. The transactional daemon keeps its
    // state in memory only, so a restarted voter comes back with no log and
    // no vote, and Raft does not survive that: it can vote twice in a term
    // or help elect a leader that lacks a committed entry. The cluster's own
    // rule is that a voter that lost its state is removed and re-added as a
    // new node. An earlier draft of this test did kill and restart, and the
    // books gained money (800 became 959) through exactly that.
    std::this_thread::sleep_for(5s);
    for (std::size_t round = 0; round < 2; ++round) {
        for (std::size_t i = 0; i < k_containers.size(); ++i) {
            BOOST_TEST_MESSAGE("isolate " << k_containers[i]);
            os::checked_exec(os::real_exec, os::network_disconnect_cmd(k_network, k_containers[i]));
            std::this_thread::sleep_for(4s);
            os::checked_exec(os::real_exec,
                             os::network_connect_cmd(k_network, k_containers[i], k_services[i]));
            std::this_thread::sleep_for(8s);

            BOOST_TEST_MESSAGE("freeze " << k_containers[i]);
            os::checked_exec(os::real_exec, os::pause_cmd(k_containers[i]));
            std::this_thread::sleep_for(4s);
            os::checked_exec(os::real_exec, os::unpause_cmd(k_containers[i]));
            std::this_thread::sleep_for(8s);
        }
    }
    BOOST_TEST_MESSAGE("kill " << k_containers[k_killed] << " for good");
    os::try_exec(os::real_exec, os::docker_kill_cmd(k_containers[k_killed]));
    std::this_thread::sleep_for(15s);
    stop = true;
    for (auto& t : workers) {
        t.join();
    }

    // Settle, then read the books through both survivors.
    BOOST_REQUIRE_MESSAGE(wait_ready(120s, true, k_killed),
                          "survivors not ready: " << g_not_ready_reason << "\n"
                                                  << node_logs());
    for (std::size_t i = 0; i < k_resp_ports.size(); ++i) {
        if (i == k_killed) {
            continue;
        }
        const auto port = k_resp_ports[i];
        resp_client c(port);
        std::optional<std::vector<long>> balances;
        for (int attempt = 0; attempt < 20 && !balances; ++attempt) {
            balances = parse_numbers(c.call(mget_all()));
            if (!balances) {
                std::this_thread::sleep_for(500ms);
            }
        }
        BOOST_REQUIRE(balances.has_value());
        BOOST_TEST_INFO("port " << port);
        BOOST_CHECK_EQUAL(sum(*balances), k_total);
        for (auto b : *balances) {
            BOOST_CHECK_GE(b, 0);
        }
    }
    BOOST_TEST_MESSAGE("committed " << n.committed.load() << ", watch aborts "
                                    << n.watch_aborts.load() << ", unknown " << n.unknown.load()
                                    << ", errors " << n.errors.load() << ", reads "
                                    << n.reads.load());
    BOOST_CHECK_GT(n.committed.load(), 0);
    // Transfers collided, and WATCH refused the losers: the property the
    // total depends on was exercised, not just never needed.
    BOOST_CHECK_GT(n.watch_aborts.load(), 0);
    BOOST_CHECK_GT(n.reads.load(), 0);
    BOOST_CHECK_EQUAL(n.torn.load(), 0);
}
