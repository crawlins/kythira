// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Three dynamodb_gateway_node run_cluster() nodes on threads in one process,
// on the memory engine with free loopback ports, in plaintext: one
// three-voter cluster, or three one-node clusters in three regions over
// one strong host (task 27). Shared by dynamodb_gateway_cluster_test and
// dynamodb_strong_cluster_test.

#pragma once

#include "run_cluster.hpp"
#include "test_ports.hpp"

#include <raft/sigv4_verifier.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dynamodb_cluster_test {

namespace ddb = kythira::dynamodb;
namespace sv4 = kythira::sigv4;
namespace json = boost::json;
namespace http = boost::beast::http;

inline constexpr std::string_view k_secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
inline constexpr int k_nodes = 3;

struct answer {
    unsigned status = 0;
    json::object body;

    [[nodiscard]] auto type() const -> std::string {
        const auto* t = body.if_contains("__type");
        if (t == nullptr) {
            return {};
        }
        const std::string full(t->as_string());
        return full.substr(full.find('#') + 1);
    }
    [[nodiscard]] auto message() const -> std::string {
        const auto* m = body.if_contains("message");
        if (m == nullptr) {
            m = body.if_contains("Message");
        }
        return m == nullptr ? std::string{} : std::string(m->as_string());
    }
};

/// Aborts the process unless destroyed within `limit`, first printing
/// `what` and running `report`. A cluster test that hangs (a node that
/// never stops, a step that never returns) otherwise runs into ctest's
/// timeout while the live nodes log a retry per heartbeat to the stuck
/// one: one such run grew its output until ctest itself failed with
/// bad_alloc, and the output that would have named the step was lost.
/// Aborting names the step, and Boost.Test reports the case and its
/// last checkpoint.
class watchdog {
public:
    watchdog(std::string what, std::chrono::seconds limit, std::function<void()> report = {})
        : _thread([this, what = std::move(what), limit, report = std::move(report)] {
              std::unique_lock lock(_mu);
              if (_cv.wait_for(lock, limit, [this] { return _done; })) {
                  return;
              }
              std::cerr << "watchdog: " << what << " did not finish within " << limit.count()
                        << " s; aborting\n";
              if (report) {
                  report();
              }
              std::cerr << std::flush;
              std::abort();
          }) {}

    watchdog(const watchdog&) = delete;
    auto operator=(const watchdog&) -> watchdog& = delete;
    watchdog(watchdog&&) = delete;
    auto operator=(watchdog&&) -> watchdog& = delete;

    ~watchdog() {
        {
            const std::lock_guard lock(_mu);
            _done = true;
        }
        _cv.notify_all();
        _thread.join();
    }

private:
    std::mutex _mu;
    std::condition_variable _cv;
    bool _done = false;
    // Last, so the members it uses exist before it starts.
    std::thread _thread;
};

/// A TCP relay on a free loopback port to `target` that can be cut: while
/// cut it drops every connection it carries and every new one. A link
/// between two regions runs through one, so a test can cut a region off
/// and bring it back without stopping it (task 27.8).
class relay {
public:
    explicit relay(std::uint16_t target)
        : _target(target), _listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        if (_listener < 0) {
            throw std::runtime_error("relay: socket failed");
        }
        const int one = 1;
        (void)::setsockopt(_listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        port = kythira::testing::free_port();
        const auto addr = loopback(port);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
        if (::bind(_listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(_listener, 64) != 0) {
            ::close(_listener);
            throw std::runtime_error("relay: cannot listen on port " + std::to_string(port));
        }
        _acceptor = std::thread([this] { accept_loop(); });
    }

    relay(const relay&) = delete;
    auto operator=(const relay&) -> relay& = delete;
    relay(relay&&) = delete;
    auto operator=(relay&&) -> relay& = delete;

    ~relay() {
        _stopping = true;
        _acceptor.join();
        std::vector<std::thread> pumps;
        {
            const std::lock_guard lock(_mu);
            pumps.swap(_pumps);
        }
        for (auto& t : pumps) {
            t.join();
        }
        ::close(_listener);
    }

    /// Drops what it carries until `heal`.
    auto cut() -> void { _cut = true; }
    auto heal() -> void { _cut = false; }

    std::uint16_t port = 0;

private:
    static auto loopback(std::uint16_t p) -> sockaddr_in {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(p);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return addr;
    }

    /// Waits up to 100 ms for `fd` to be readable.
    static auto readable(int fd) -> bool {
        pollfd p{.fd = fd, .events = POLLIN, .revents = 0};
        return ::poll(&p, 1, 100) > 0;
    }

    auto accept_loop() -> void {
        while (!_stopping) {
            if (!readable(_listener)) {
                continue;
            }
            const int in = ::accept4(_listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (in < 0) {
                continue;
            }
            if (_cut) {
                ::close(in);
                continue;
            }
            const int out = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            const auto addr = loopback(_target);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
            if (out < 0 ||
                ::connect(out, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
                ::close(in);
                if (out >= 0) {
                    ::close(out);
                }
                continue;
            }
            const std::lock_guard lock(_mu);
            _pumps.emplace_back([this, in, out] { pump(in, out); });
        }
    }

    /// Copies both ways until either side closes, the relay is cut or it
    /// stops.
    auto pump(int a, int b) -> void {
        std::array<char, 16384> buf{};
        std::array<pollfd, 2> fds{pollfd{.fd = a, .events = POLLIN, .revents = 0},
                                  pollfd{.fd = b, .events = POLLIN, .revents = 0}};
        bool open = true;
        while (open && !_stopping && !_cut) {
            if (::poll(fds.data(), fds.size(), 100) <= 0) {
                continue;
            }
            for (std::size_t i = 0; i < fds.size() && open; ++i) {
                if ((fds.at(i).revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                    continue;
                }
                const auto n = ::read(fds.at(i).fd, buf.data(), buf.size());
                if (n <= 0) {
                    open = false;
                    break;
                }
                const int to = fds.at(1 - i).fd;
                for (ssize_t sent = 0; sent < n;) {
                    const auto w = ::send(to, buf.data() + sent, static_cast<std::size_t>(n - sent),
                                          MSG_NOSIGNAL);
                    if (w <= 0) {
                        open = false;
                        break;
                    }
                    sent += w;
                }
            }
        }
        ::close(a);
        ::close(b);
    }

    std::uint16_t _target;
    int _listener = -1;
    std::atomic<bool> _stopping{false};
    std::atomic<bool> _cut{false};
    std::mutex _mu;
    std::vector<std::thread> _pumps;
    std::thread _acceptor;
};

/// Three run_cluster() nodes on threads, stopped when the fixture goes.
struct cluster {
    /// The longest a cluster may live, stops included. The slowest case
    /// takes about 65 s on an idle machine; this leaves room for a loaded
    /// runner and still fails well inside ctest's scaled timeout.
    static constexpr std::chrono::seconds k_lifetime{600};
    /// The longest one node may take to stop.
    static constexpr std::chrono::seconds k_stop_limit{120};

    std::array<std::uint16_t, k_nodes> gateway_ports{};
    std::array<std::uint16_t, k_nodes> raft_ports{};
    std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
    std::vector<std::thread> nodes;
    std::array<std::exception_ptr, k_nodes> failures{};
    /// Three regions (task 27): node `i` is a one-node cluster in
    /// `k_regions[i]`, and the three are voters of one strong host.
    bool regions = false;
    std::array<bool, k_nodes> stopped{};
    std::array<std::uint16_t, k_nodes> strong_ports{};
    std::array<std::vector<std::string>, k_nodes> logs{};

    std::map<std::string, std::string> extra_vars;
    // After `mutex` and `logs`, which its report reads, and before the
    // relays, so it is destroyed after them: the destructor's body stops
    // the nodes under their own watchdogs, and this one also covers the
    // relays' teardown that follows.
    watchdog lifetime{"a cluster test", k_lifetime, [this] { report_logs(); }};
    /// `[from][to]`: node `from`'s link to node `to`, when relayed.
    std::array<std::array<std::unique_ptr<relay>, k_nodes>, k_nodes> raft_relays{};
    std::array<std::array<std::unique_ptr<relay>, k_nodes>, k_nodes> gateway_relays{};

    static constexpr std::array<std::string_view, k_nodes> k_regions{"us-east-1", "us-east-2",
                                                                     "us-west-2"};

    /// Settings every node gets on top of its own (`extra`), and with
    /// `relayed` every link between two regions' strong hosts and gateways
    /// through a relay `cut_off` can cut.
    explicit cluster(bool three_regions = false, std::map<std::string, std::string> extra = {},
                     bool relayed = false)
        : regions(three_regions), extra_vars(std::move(extra)) {
        for (int i = 0; i < k_nodes; ++i) {
            gateway_ports.at(i) = kythira::testing::free_port();
            raft_ports.at(i) = kythira::testing::free_port();
            strong_ports.at(i) = kythira::testing::free_port();
        }
        if (relayed) {
            for (int from = 0; from < k_nodes; ++from) {
                for (int to = 0; to < k_nodes; ++to) {
                    if (from != to) {
                        raft_relays.at(from).at(to) = std::make_unique<relay>(strong_ports.at(to));
                        gateway_relays.at(from).at(to) =
                            std::make_unique<relay>(gateway_ports.at(to));
                    }
                }
            }
        }
        for (int i = 0; i < k_nodes; ++i) {
            nodes.emplace_back([this, i] { run(i); });
        }
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;
    cluster(cluster&&) = delete;
    auto operator=(cluster&&) -> cluster& = delete;

    ~cluster() {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        cv.notify_all();
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (nodes.at(i).joinable()) {
                const watchdog w("stopping node " + std::to_string(i), k_stop_limit,
                                 [this] { report_logs(); });
                nodes.at(i).join();
            }
        }
    }

    /// Stops node `i` (a region, with `regions`) for the rest of the test.
    auto stop(int i) -> void {
        {
            const std::lock_guard lock(mutex);
            stopped.at(i) = true;
        }
        cv.notify_all();
        const watchdog w("stopping node " + std::to_string(i), k_stop_limit,
                         [this] { report_logs(); });
        nodes.at(i).join();
    }

    /// Each node's last log lines, for a watchdog about to abort. It does
    /// not wait for `mutex`, which a hung step may hold.
    auto report_logs() -> void {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            std::cerr << "watchdog: node logs are locked; not printed\n";
            return;
        }
        constexpr std::size_t k_tail = 20;
        for (std::size_t i = 0; i < logs.size(); ++i) {
            const auto& lines = logs.at(i);
            std::cerr << "watchdog: node " << i << "'s last log lines:\n";
            for (std::size_t j = lines.size() > k_tail ? lines.size() - k_tail : 0;
                 j < lines.size(); ++j) {
                std::cerr << "  " << lines.at(j) << "\n";
            }
        }
    }

    /// Cuts node `i` off from the other two, both ways, or with `heal`
    /// brings it back; it keeps running. Needs `relayed`.
    auto cut_off(int i, bool heal = false) -> void {
        for (int j = 0; j < k_nodes; ++j) {
            if (j == i) {
                continue;
            }
            for (auto* r : {raft_relays.at(i).at(j).get(), raft_relays.at(j).at(i).get(),
                            gateway_relays.at(i).at(j).get(), gateway_relays.at(j).at(i).get()}) {
                BOOST_REQUIRE(r != nullptr);
                if (heal) {
                    r->heal();
                } else {
                    r->cut();
                }
            }
        }
    }

    /// The port node `from` reaches node `to`'s strong host (or gateway)
    /// on: a relay when relayed.
    [[nodiscard]] auto strong_port(int from, int to) const -> std::uint16_t {
        const auto& r = raft_relays.at(from).at(to);
        return r ? r->port : strong_ports.at(to);
    }
    [[nodiscard]] auto gateway_port(int from, int to) const -> std::uint16_t {
        const auto& r = gateway_relays.at(from).at(to);
        return r ? r->port : gateway_ports.at(to);
    }

    /// Node `i`'s settings: one of three voters of one cluster, or with
    /// `regions` a one-node cluster in its own region with a strong host.
    auto config_of(int i) -> ddb::gateway_config {
        if (regions) {
            return region_config_of(i);
        }
        std::string peers;
        std::string gateways;
        for (int j = 0; j < k_nodes; ++j) {
            const auto id = std::to_string(j + 1);
            peers +=
                (j == 0 ? "" : ",") + id + "=http://127.0.0.1:" + std::to_string(raft_ports.at(j));
            if (j != i) {
                gateways += (gateways.empty() ? "" : ",") + id +
                            "=127.0.0.1:" + std::to_string(gateway_ports.at(j));
            }
        }
        std::map<std::string, std::string> vars{
            {"KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT", "1"},
            {"KYTHIRA_DYNAMODB_BIND", "127.0.0.1"},
            {"KYTHIRA_DYNAMODB_PORT", std::to_string(gateway_ports.at(i))},
            {"KYTHIRA_DYNAMODB_STATIC_KEYS",
             "AKIDADMIN:" + std::string(k_secret) + ":111122223333:admin:admin"},
            {"KYTHIRA_DYNAMODB_NODE_ID", std::to_string(i + 1)},
            {"KYTHIRA_DYNAMODB_PEERS", peers},
            {"KYTHIRA_DYNAMODB_RAFT_BIND", "127.0.0.1"},
            {"KYTHIRA_DYNAMODB_RAFT_PORT", std::to_string(raft_ports.at(i))},
            {"KYTHIRA_DYNAMODB_PEER_GATEWAYS", gateways},
            {"KYTHIRA_DYNAMODB_FORWARD_SECRET", "0123456789abcdef-cluster-test"},
        };
        for (const auto& [k, v] : extra_vars) {
            vars[k] = v;
        }
        return parse(vars);
    }

    auto region_config_of(int i) -> ddb::gateway_config {
        const auto id = std::to_string(i + 1);
        std::string strong_peers;
        std::string strong_gateways;
        for (int j = 0; j < k_nodes; ++j) {
            const auto sid = std::to_string(10 * (j + 1) + 1);
            strong_peers += (j == 0 ? "" : ",") + sid + "=" + std::string(k_regions.at(j)) +
                            "@http://127.0.0.1:" + std::to_string(strong_port(i, j));
            if (j != i) {
                strong_gateways += (strong_gateways.empty() ? "" : ",") + sid +
                                   "=127.0.0.1:" + std::to_string(gateway_port(i, j));
            }
        }
        std::map<std::string, std::string> vars{
            {"KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT", "1"},
            {"KYTHIRA_DYNAMODB_BIND", "127.0.0.1"},
            {"KYTHIRA_DYNAMODB_PORT", std::to_string(gateway_ports.at(i))},
            {"KYTHIRA_DYNAMODB_REGION", std::string(k_regions.at(i))},
            {"KYTHIRA_DYNAMODB_STATIC_KEYS",
             "AKIDADMIN:" + std::string(k_secret) + ":111122223333:admin:admin"},
            {"KYTHIRA_DYNAMODB_NODE_ID", id},
            {"KYTHIRA_DYNAMODB_PEERS",
             id + "=http://127.0.0.1:" + std::to_string(raft_ports.at(i))},
            {"KYTHIRA_DYNAMODB_RAFT_BIND", "127.0.0.1"},
            {"KYTHIRA_DYNAMODB_RAFT_PORT", std::to_string(raft_ports.at(i))},
            {"KYTHIRA_DYNAMODB_STRONG_NODE_ID", std::to_string(10 * (i + 1) + 1)},
            {"KYTHIRA_DYNAMODB_STRONG_PEERS", strong_peers},
            {"KYTHIRA_DYNAMODB_STRONG_RAFT_BIND", "127.0.0.1"},
            {"KYTHIRA_DYNAMODB_STRONG_RAFT_PORT", std::to_string(strong_ports.at(i))},
            {"KYTHIRA_DYNAMODB_STRONG_GATEWAYS", strong_gateways},
            {"KYTHIRA_DYNAMODB_STRONG_SECRET", "0123456789abcdef-strong-test"},
            // Loopback is not a wide-area network.
            {"KYTHIRA_DYNAMODB_STRONG_HEARTBEAT_MS", "50"},
            {"KYTHIRA_DYNAMODB_STRONG_ELECTION_MIN_MS", "300"},
            {"KYTHIRA_DYNAMODB_STRONG_ELECTION_MAX_MS", "600"},
        };
        for (const auto& [k, v] : extra_vars) {
            vars[k] = v;
        }
        return parse(vars);
    }

    static auto parse(const std::map<std::string, std::string>& vars) -> ddb::gateway_config {
        auto r = ddb::gateway_config_from_env([&vars](std::string_view name) {
            const auto it = vars.find(std::string(name));
            return it == vars.end() ? std::nullopt : std::optional<std::string>(it->second);
        });
        if (const auto* e = std::get_if<std::string>(&r)) {
            throw std::runtime_error(*e);
        }
        return std::get<ddb::gateway_config>(r);
    }

    /// Waits until node `i` has logged a line holding `text`.
    auto wait_for_log(int i, std::string_view text, std::chrono::seconds limit) -> bool {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, limit, [&] {
            const auto& lines = logs.at(i);
            return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) {
                return l.find(text) != std::string::npos;
            });
        });
    }

    auto run(int i) -> void {
        using storage = dynamodb_gateway::memory_storage;
        try {
            dynamodb_gateway::run_cluster<storage>(
                config_of(i), std::make_shared<storage::engine_type>(),
                [](std::uint64_t) { return storage::persistence_engine_type{}; },
                [this, i](bool, std::string_view line) {
                    {
                        const std::lock_guard lock(mutex);
                        logs.at(i).emplace_back(line);
                    }
                    cv.notify_all();
                },
                [this, i] {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [this, i] { return stopping || stopped.at(i); });
                });
        } catch (...) {
            failures.at(i) = std::current_exception();
        }
    }

    /// `op` signed now and sent to node `i`'s gateway.
    auto call(int i, std::string_view op, const std::string& body,
              std::string_view target = "DynamoDB_20120810") -> answer {
        const auto port = gateway_ports.at(i);
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto amz_date = sv4::detail::format_amz_date(now);
        const std::string host = "127.0.0.1:" + std::to_string(port);
        std::vector<std::pair<std::string, std::string>> headers{
            {"Content-Type", "application/x-amz-json-1.0"},
            {"Host", host},
            {"X-Amz-Date", amz_date},
            {"X-Amz-Target", std::string(target) + "." + std::string(op)}};
        const sv4::credential_scope scope{amz_date.substr(0, 8),
                                          regions ? std::string(k_regions.at(i)) : "us-east-1",
                                          "dynamodb"};
        headers.emplace_back("Authorization",
                             "AWS4-HMAC-SHA256 Credential=AKIDADMIN/" + scope.text() +
                                 ", SignedHeaders=content-type;host;x-amz-date;x-amz-target"
                                 ", Signature=" +
                                 std::string(64, '0'));
        sv4::service_rules rules;
        rules.service = "dynamodb";
        rules.protocol = sv4::error_protocol::json;
        const auto parsed = sv4::parse(sv4::request{"POST", "/", "", headers, body}, rules, now);
        BOOST_REQUIRE(parsed.has_value());
        auto& auth = headers.back().second;
        auth.replace(auth.size() - 64, 64,
                     sv4::compute_signature(*parsed, sv4::derive_signing_key(k_secret, scope)));

        http::request<http::string_body> req{http::verb::post, "/", 11};
        for (const auto& [n, v] : headers) {
            req.set(n, v);
        }
        req.body() = body;
        req.prepare_payload();
        boost::asio::io_context ioc;
        boost::beast::tcp_stream s(ioc);
        s.expires_after(std::chrono::seconds(30));
        s.connect({boost::asio::ip::make_address("127.0.0.1"), port});
        http::write(s, req);
        boost::beast::flat_buffer buf;
        http::response_parser<http::string_body> parser;
        http::read(s, buf, parser);
        const auto& res = parser.get();
        return answer{static_cast<unsigned>(res.result_int()), json::parse(res.body()).as_object()};
    }

    /// Calls until the answer is not a server error (the cluster electing
    /// its leaders), as an SDK retries; the last answer.
    auto call_until_served(int i, std::string_view op, const std::string& body,
                           std::string_view target = "DynamoDB_20120810") -> answer {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (true) {
            std::optional<answer> a;
            try {
                a = call(i, op, body, target);
            } catch (const std::exception&) {
                // The listener is not up yet.
            }
            if ((a && a->status < 500U) || std::chrono::steady_clock::now() > deadline) {
                return a.value_or(answer{});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
};

}  // namespace dynamodb_cluster_test
