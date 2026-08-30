// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_transport_harness.hpp
/// @brief Stands a `multi_raft` host up over a *real* transport, generically in
///        the transport and in the RPC serializer.
///
/// ### Why this exists
///
/// Every multi-Raft suite in this tree runs on `multi_raft_test_fabric.hpp` — an
/// in-process message fabric with no sockets and no encoding. That is the right
/// substrate for correctness, and it is the wrong one for a performance number:
/// it measures the host and the consensus core with the two costs a real
/// deployment actually pays (serialization and the wire) removed. Before this
/// header, **nothing in the tree had ever driven `multi_raft` through a
/// socket**, so `.kiro/specs/multi-raft-performance/` Tier B had no substrate.
///
/// ### The two adapters, and why they are needed
///
/// `multi_raft_config` holds `network_client_type` / `network_server_type`
/// **by value**, and `multi_raft` moves them into place. None of this project's
/// real transports are movable — `boost_beast_client` and `proxygen_client`
/// delete their move constructors outright, and `cpp_httplib_client` holds a
/// `std::mutex` and a `std::jthread`. So the host is handed a *handle*: a
/// movable, pointer-thin view that satisfies `network_client` /
/// `network_server` by forwarding to a transport the fixture owns and outlives.
///
/// The handles forward the three mandatory RPCs unconditionally and every
/// optional one **behind a `requires` clause on the underlying transport**,
/// exactly as `group_scoped_client` does. That is what makes the extension
/// story cheap: a transport that grows `timeout_now` lights it up here with no
/// edit, and one that never has it — CoAP, whose deployments are constrained
/// devices and whose transport is shaped around a fixed resource set — simply
/// does not satisfy `network_client_with_timeout_now`, `multi_group_network_server`'s
/// own `if constexpr` skips the handler, and `transfer_leader` / `scatter`
/// report `unsupported` on that row rather than failing to compile.
///
/// ### Adding a transport
///
/// Implement a fixture with the shape of `cpp_httplib_transport` below:
/// a `transport_bundle` alias, `client_type` / `server_type`, `name()`,
/// a `tier()` naming which of Requirement 3.1's tiers it realises, a
/// `capabilities()` descriptor, `server(id)` / `client(id)` accessors, and
/// `drain()` / `shutdown()` — `drain()` returning only once no request handler
/// is still running, `shutdown()` releasing the fixture's own threads.
/// Nothing else in the harness or in the tests changes. A CoAP fixture would
/// differ only in owning a libcoap context instead of an `io_context` and in
/// reporting `_timeout_now = false`.

#include "multi_raft_kv_workload.hpp"
#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/executor_default.hpp>
#include <raft/file_persistence.hpp>
#include <raft/future_default.hpp>
#include <raft/group_transport.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/network.hpp>
#include <raft/persistence.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/test_state_machine.hpp>
#include <raft/types.hpp>

#if defined(KYTHIRA_BENCH_HAS_BEAST)
#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#endif

#if defined(KYTHIRA_BENCH_HAS_PROXYGEN)
#include <raft/proxygen_http_transport.hpp>
#include <raft/proxygen_http_transport_impl.hpp>
#include <folly/executors/IOThreadPoolExecutor.h>
#endif

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::testing {

// ─────────────────────────────────────────────────────────────────────────────
// Replication-RPC accounting
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Replication RPCs counted at the one place every transport shares.
///
/// Counted in `transport_client_handle` rather than inside a transport or
/// inside `raft_node`, for two reasons. It is the only point on the send path
/// that all three HTTP transports pass through, so a figure taken here is
/// comparable across the transport axis by construction; and the production
/// code stays free of instrumentation inserted for a benchmark, which
/// Requirement 8.1 asks for in as many words.
///
/// Relaxed ordering throughout: these are counters read once at each end of a
/// window that is already fenced by thread joins, not a synchronisation
/// mechanism.
struct rpc_counters {
    std::atomic<std::uint64_t> _append_entries{0};
    std::atomic<std::uint64_t> _append_entries_empty{0};
    std::atomic<std::uint64_t> _entries{0};
    std::atomic<std::uint64_t> _request_vote{0};
    std::atomic<std::uint64_t> _install_snapshot{0};

    [[nodiscard]] auto snapshot() const -> rpc_snapshot {
        return rpc_snapshot{
            ._append_entries = _append_entries.load(std::memory_order_relaxed),
            ._append_entries_empty = _append_entries_empty.load(std::memory_order_relaxed),
            ._entries = _entries.load(std::memory_order_relaxed),
            ._request_vote = _request_vote.load(std::memory_order_relaxed),
            ._install_snapshot = _install_snapshot.load(std::memory_order_relaxed),
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Movable handles over non-movable transports
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A movable `network_client` view of a transport the caller owns.
///
/// Every method forwards; the three mandatory RPCs also tick `rpc_counters`
/// when the constructor was given one, which is how the benchmark gets
/// entries-per-AppendEntries and RPCs-per-committed-entry without a counter in
/// production code. The optional RPCs carry a `requires` clause
/// on `Client` so that this handle satisfies exactly the extension concepts its
/// underlying transport does — no more (which would produce a link error at
/// best) and no fewer (which would silently disable leadership transfer on a
/// transport that supports it).
template<typename Client> class transport_client_handle {
public:
    using client_type = Client;

    explicit transport_client_handle(Client& client, rpc_counters* counters = nullptr) noexcept
        : _client(&client), _counters(counters) {}

    // ── the three every transport has ────────────────────────────────────────

    auto send_request_vote(std::uint64_t target, const kythira::request_vote_request<>& request,
                           std::chrono::milliseconds timeout) {
        if (_counters != nullptr) {
            _counters->_request_vote.fetch_add(1, std::memory_order_relaxed);
        }
        return _client->send_request_vote(target, request, timeout);
    }

    auto send_append_entries(std::uint64_t target, const kythira::append_entries_request<>& request,
                             std::chrono::milliseconds timeout) {
        if (_counters != nullptr) {
            // Counted on the way out, before the transport can fail it. An RPC
            // the leader issued is a cost it paid whether or not a response
            // came back, and "RPCs per committed entry" is a cost ratio.
            _counters->_append_entries.fetch_add(1, std::memory_order_relaxed);
            const auto entries = request.entries().size();
            if (entries == 0) {
                _counters->_append_entries_empty.fetch_add(1, std::memory_order_relaxed);
            } else {
                _counters->_entries.fetch_add(entries, std::memory_order_relaxed);
            }
        }
        return _client->send_append_entries(target, request, timeout);
    }

    auto send_install_snapshot(std::uint64_t target,
                               const kythira::install_snapshot_request<>& request,
                               std::chrono::milliseconds timeout) {
        if (_counters != nullptr) {
            _counters->_install_snapshot.fetch_add(1, std::memory_order_relaxed);
        }
        return _client->send_install_snapshot(target, request, timeout);
    }

    // ── the optional ones, mirrored from the underlying transport ────────────

    auto send_request_pre_vote(std::uint64_t target,
                               const kythira::request_pre_vote_request<>& request,
                               std::chrono::milliseconds timeout)
    requires kythira::network_client_with_pre_vote<Client>
    {
        return _client->send_request_pre_vote(target, request, timeout);
    }

    auto send_fetch_log_entries(std::uint64_t target,
                                const kythira::fetch_log_entries_request<>& request,
                                std::chrono::milliseconds timeout)
    requires kythira::network_client_with_log_fetch<Client>
    {
        return _client->send_fetch_log_entries(target, request, timeout);
    }

    auto send_timeout_now(std::uint64_t target, const kythira::timeout_now_request<>& request,
                          std::chrono::milliseconds timeout)
    requires kythira::network_client_with_timeout_now<Client>
    {
        return _client->send_timeout_now(target, request, timeout);
    }

    [[nodiscard]] auto underlying() noexcept -> Client& { return *_client; }

private:
    Client* _client;
    /// Null when nobody is counting — every non-benchmark construction site,
    /// which is why the parameter is defaulted rather than required.
    rpc_counters* _counters{nullptr};
};

/// @brief A movable `network_server` view of a transport the caller owns.
///
/// `start()` and `stop()` are forwarded rather than suppressed: the host is the
/// component that knows when the transport should be live, and every fixture
/// below constructs its server *stopped* precisely so that stays true.
template<typename Server> class transport_server_handle {
public:
    using server_type = Server;

    explicit transport_server_handle(Server& server) noexcept : _server(&server) {}

    auto register_request_vote_handler(
        std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)>
            handler) -> void {
        _server->register_request_vote_handler(std::move(handler));
    }

    auto register_append_entries_handler(
        std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
            handler) -> void {
        _server->register_append_entries_handler(std::move(handler));
    }

    auto register_install_snapshot_handler(std::function<kythira::install_snapshot_response<>(
                                               const kythira::install_snapshot_request<>&)>
                                               handler) -> void {
        _server->register_install_snapshot_handler(std::move(handler));
    }

    auto register_request_pre_vote_handler(std::function<kythira::request_pre_vote_response<>(
                                               const kythira::request_pre_vote_request<>&)>
                                               handler) -> void
    requires kythira::network_server_with_pre_vote<Server>
    {
        _server->register_request_pre_vote_handler(std::move(handler));
    }

    auto register_fetch_log_entries_handler(std::function<kythira::fetch_log_entries_response<>(
                                                const kythira::fetch_log_entries_request<>&)>
                                                handler) -> void
    requires kythira::network_server_with_log_fetch<Server>
    {
        _server->register_fetch_log_entries_handler(std::move(handler));
    }

    auto register_timeout_now_handler(
        std::function<kythira::timeout_now_response<>(const kythira::timeout_now_request<>&)>
            handler) -> void
    requires kythira::network_server_with_timeout_now<Server>
    {
        _server->register_timeout_now_handler(std::move(handler));
    }

    auto start() -> void { _server->start(); }
    auto stop() -> void { _server->stop(); }
    [[nodiscard]] auto is_running() const -> bool { return _server->is_running(); }

    [[nodiscard]] auto underlying() noexcept -> Server& { return *_server; }

private:
    Server* _server;
};

// ─────────────────────────────────────────────────────────────────────────────
// Transport fixtures
// ─────────────────────────────────────────────────────────────────────────────

/// @brief What a transport can carry, reported per row rather than assumed.
///
/// The multi-Raft design's own "known limitations" section records that CoAP
/// does not implement TimeoutNow and that `transfer_leader` / `scatter` report
/// `unsupported` there. That is a property of the transport, so it is described
/// here rather than rediscovered by a test that fails.
struct transport_capabilities {
    bool _pre_vote{false};
    bool _log_fetch{false};
    bool _timeout_now{false};
    /// @brief Whether concurrent RPCs to one peer can actually proceed in
    /// parallel. cpp-httplib serializes them behind one `httplib::Client`, which
    /// is a first-order throughput fact rather than a footnote.
    bool _concurrent_per_peer{true};
};

/// @brief The `Types` bundle every transport in this harness is instantiated
/// over.
///
/// `future_template` is pinned to `kythira::future_default` because
/// `node<Types>` declares its RPC lambdas as returning
/// `kythira::future_default<T>` and assigns the client's future straight into
/// that — the folly-typed `http_transport_types` bundle would only convert
/// under one backend, and would not compile under boost or stdexec at all.
template<typename Serializer> struct harness_transport_types {
    template<typename T> using future_template = kythira::future_default<T>;
    using serializer_type = Serializer;
    using serializer_registry_type = kythira::single_serializer_registry<Serializer>;
    using metrics_type = kythira::noop_metrics;
    using executor_type = kythira::executor_default;
};

// ─────────────────────────────────────────────────────────────────────────────
// Tier A: no wire at all
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The wire encoding of a tier that has no wire.
///
/// Exists so that `fill_result` can read a serializer label off every transport
/// fixture without a special case, and so that a Tier A row says "none (no
/// wire)" in the column every other row fills with a media type. A blank there
/// would read as "not recorded", which is a different claim.
struct no_wire_serializer {
    [[nodiscard]] auto media_type() const -> std::string { return "none (no wire)"; }
};

/// @brief The bundle `fabric_transport` reports itself through.
///
/// Deliberately not `harness_transport_types<no_wire_serializer>`: the fabric
/// hands typed messages straight to the target's handler, so there is no
/// registry, no metrics sink and no executor on its send path to describe. A
/// bundle listing components this tier does not have would invite the reader to
/// subtract costs that were never paid.
struct fabric_transport_types {
    using serializer_type = no_wire_serializer;
};

/// @brief How many threads carry messages across the fabric.
///
/// Eight, matching `multi_raft_static_cluster_integration_test` — the fabric's
/// other user — so a Tier A number taken here behaves like the Tier A that
/// suite already exercises. It is **not** matched to the io-thread count of the
/// HTTP fixtures, and cannot usefully be: a fabric worker runs the whole target
/// handler while a Beast io thread runs a socket read. The thread budgets
/// therefore differ between the tiers, and that difference is one of the things
/// the A→B delta contains — which is why the decomposition states a residual
/// instead of claiming the delta is the wire alone.
inline constexpr std::size_t k_fabric_workers = 8;

/// @brief Tier A: every host in one process over the in-process message fabric.
///
/// The same fixture shape as the HTTP transports — `transport_bundle`,
/// `client_type` / `server_type`, `name()`, `tier()`, `capabilities()`, and
/// `client(id)` / `server(id)` — so `kv_cluster` is instantiated over it with
/// no special case and every row in this suite can be taken at either tier.
///
/// This is the tier Requirement 3.1 describes as isolating host and consensus
/// cost with no wire and no disk, and labels **never comparable to an external
/// number**. It is here for exactly one purpose: the A→B delta is where the
/// transport and serialization components of Requirement 8.1's decomposition
/// come from, without a single counter inserted into a production path
/// (Requirement 8.2).
class fabric_transport {
public:
    using transport_bundle = fabric_transport_types;
    using client_type = fabric_client;
    using server_type = fabric_server;

    static auto name() -> std::string_view { return "in-process fabric"; }
    static auto tier() -> deployment_tier { return deployment_tier::a_fabric; }
    /// The fabric carries every RPC the group transport defines, and delivers
    /// concurrent requests to one peer on separate workers.
    static auto capabilities() -> transport_capabilities {
        return transport_capabilities{._pre_vote = true,
                                      ._log_fetch = true,
                                      ._timeout_now = true,
                                      ._concurrent_per_peer = true};
    }

    explicit fabric_transport(const std::vector<std::uint64_t>& nodes) : _fabric(k_fabric_workers) {
        for (auto id : nodes) {
            _servers.emplace(id, std::make_unique<server_type>(_fabric, id));
            _clients.emplace(id, std::make_unique<client_type>(_fabric, id));
        }
    }

    ~fabric_transport() { shutdown(); }

    fabric_transport(const fabric_transport&) = delete;
    auto operator=(const fabric_transport&) -> fabric_transport& = delete;

    [[nodiscard]] auto server(std::uint64_t id) -> server_type& { return *_servers.at(id); }
    [[nodiscard]] auto client(std::uint64_t id) -> client_type& { return *_clients.at(id); }

    /// @brief Join the fabric's workers, so no handler is still running.
    ///
    /// `message_fabric::shutdown()` drains the queue rather than discarding it,
    /// and every endpoint has already been stopped by the time this is called,
    /// so what drains is a queue of messages that all fail fast with "no
    /// handler on target". What it buys is the guarantee the hosts need: when
    /// this returns, no worker is inside a handler that captured one.
    auto drain() -> void { _fabric.shutdown(); }

    /// @brief Release the client and server objects. Called after `drain()`.
    ///
    /// There is nothing left to join by this point — `drain()` did that — and
    /// dropping these is safe even if it did not: `message_fabric` holds its
    /// endpoint table **by value**, so a worker mid-delivery is using the
    /// fabric's copy of the handlers and never touches a `fabric_server`. What
    /// it is not safe to drop early is the *hosts* those handlers close over,
    /// which is `drain()`'s whole reason for existing.
    auto shutdown() -> void {
        if (_stopped.exchange(true)) {
            return;
        }
        _clients.clear();
        _servers.clear();
    }

private:
    message_fabric _fabric;
    std::atomic<bool> _stopped{false};
    std::unordered_map<std::uint64_t, std::unique_ptr<server_type>> _servers;
    std::unordered_map<std::uint64_t, std::unique_ptr<client_type>> _clients;
};

// ─────────────────────────────────────────────────────────────────────────────
// Tier B: a real transport over loopback
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Ask the kernel for a free TCP port, then give it straight back.
///
/// The alternative — binding port 0 inside the server and reading the chosen
/// port back — cannot work here: the client needs every peer's URL at
/// *construction* time, which is before `multi_raft::start()` has started any
/// server. Reserving up front keeps startup in its natural order, at the cost of
/// a small window in which another process could take the port. Any port chosen
/// in advance has that window; this at least never collides with a port already
/// in use.
[[nodiscard]] inline auto reserve_port() -> std::uint16_t {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("reserve_port: socket() failed");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error("reserve_port: bind() failed");
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        throw std::runtime_error("reserve_port: getsockname() failed");
    }
    const auto port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

inline auto loopback_url(std::uint16_t port) -> std::string {
    return "http://127.0.0.1:" + std::to_string(port);
}

/// @brief cpp-httplib: synchronous, one connection per peer.
///
/// The simplest of the three and the slowest by construction — `send_rpc` calls
/// `httplib::Client::Post` on the caller's thread, so a leader's replication
/// round to N followers is N blocking round trips in sequence. That is a real
/// property of this transport under multi-Raft, not a harness artefact, and the
/// benchmark reports it rather than working around it.
template<typename Serializer> class cpp_httplib_transport {
public:
    using transport_bundle = harness_transport_types<Serializer>;
    using client_type = kythira::cpp_httplib_client<transport_bundle>;
    using server_type = kythira::cpp_httplib_server<transport_bundle>;

    static auto name() -> std::string_view { return "cpp-httplib"; }
    static auto tier() -> deployment_tier { return deployment_tier::b_loopback; }
    static auto capabilities() -> transport_capabilities {
        return transport_capabilities{._concurrent_per_peer = false};
    }

    explicit cpp_httplib_transport(const std::vector<std::uint64_t>& nodes) {
        for (auto id : nodes) {
            _urls.emplace(id, loopback_url(reserve_port()));
        }
        for (auto id : nodes) {
            _servers.emplace(id, std::make_unique<server_type>("127.0.0.1", port_of(id),
                                                               server_config(), noop_metrics{}));
            _clients.emplace(id,
                             std::make_unique<client_type>(_urls, client_config(), noop_metrics{}));
        }
    }

    [[nodiscard]] auto server(std::uint64_t id) -> server_type& { return *_servers.at(id); }
    [[nodiscard]] auto client(std::uint64_t id) -> client_type& { return *_clients.at(id); }

    /// @brief Nothing to wait for: this fixture's request handling happens on
    /// threads its own `shutdown()` joins, after the hosts are gone.
    auto drain() -> void {}

    /// @brief Released after every host has been stopped. Nothing to do here;
    /// the servers own their own threads.
    auto shutdown() -> void {}

private:
    [[nodiscard]] auto port_of(std::uint64_t id) const -> std::uint16_t {
        const auto& url = _urls.at(id);
        return static_cast<std::uint16_t>(std::stoi(url.substr(url.rfind(':') + 1)));
    }

    static auto client_config() -> kythira::cpp_httplib_client_config {
        kythira::cpp_httplib_client_config cfg;
        // A replication round must not be able to outlive the tick that issued
        // it by minutes; the benchmark's own deadlines are far shorter.
        cfg.connection_timeout = std::chrono::milliseconds{2000};
        cfg.request_timeout = std::chrono::milliseconds{5000};
        return cfg;
    }

    static auto server_config() -> kythira::cpp_httplib_server_config {
        kythira::cpp_httplib_server_config cfg;
        cfg.request_timeout = std::chrono::seconds{5};
        return cfg;
    }

    std::unordered_map<std::uint64_t, std::string> _urls;
    std::unordered_map<std::uint64_t, std::unique_ptr<server_type>> _servers;
    std::unordered_map<std::uint64_t, std::unique_ptr<client_type>> _clients;
};

#if defined(KYTHIRA_BENCH_HAS_BEAST)
/// @brief Boost.Beast: asynchronous on a caller-owned `io_context`.
template<typename Serializer> class beast_http_transport {
public:
    using transport_bundle = harness_transport_types<Serializer>;
    using client_type = kythira::boost_beast_client<transport_bundle>;
    using server_type = kythira::boost_beast_server<transport_bundle>;

    static auto name() -> std::string_view { return "beast"; }
    static auto tier() -> deployment_tier { return deployment_tier::b_loopback; }
    static auto capabilities() -> transport_capabilities { return transport_capabilities{}; }

    explicit beast_http_transport(const std::vector<std::uint64_t>& nodes)
        : _work(boost::asio::make_work_guard(*_ioc)) {
        const auto threads = std::max(2U, std::thread::hardware_concurrency() / 2);
        for (unsigned i = 0; i < threads; ++i) {
            _io_threads.emplace_back([this] { _ioc->run(); });
        }
        for (auto id : nodes) {
            _urls.emplace(id, loopback_url(reserve_port()));
        }
        for (auto id : nodes) {
            _servers.emplace(id, std::make_unique<server_type>(*_ioc, "127.0.0.1", port_of(id),
                                                               kythira::boost_beast_server_config{},
                                                               noop_metrics{}));
            // `_ioc` is passed as the client's context keeper, not just
            // dereferenced into it. This fixture is destroyed at the end of
            // every repetition while `error_handler`'s delayed retries are
            // still armed on Folly's process-wide Timekeeper, and those chains
            // hold `KeepAlive`s on the Beast connections' strand executors. A
            // by-value `io_context` here died first and the last executor's
            // destructor then read the service registry it had taken with it
            // -- `.kiro/specs/multi-raft-performance/` task 5a. The server
            // needs no keeper: `server_session` holds its executor by value,
            // so `keepAliveAcquire()` returns false and it never self-pins.
            _clients.emplace(
                id, std::make_unique<client_type>(
                        *_ioc, _urls, kythira::boost_beast_client_config{}, noop_metrics{}, _ioc));
        }
    }

    ~beast_http_transport() { shutdown(); }

    beast_http_transport(const beast_http_transport&) = delete;
    auto operator=(const beast_http_transport&) -> beast_http_transport& = delete;

    [[nodiscard]] auto server(std::uint64_t id) -> server_type& { return *_servers.at(id); }
    [[nodiscard]] auto client(std::uint64_t id) -> client_type& { return *_clients.at(id); }

    /// @brief Nothing to wait for: this fixture's request handling happens on
    /// threads its own `shutdown()` joins, after the hosts are gone.
    auto drain() -> void {}

    /// @brief Stop the io threads. Called after every host is stopped, never
    /// before: a running host still has RPCs on these threads.
    auto shutdown() -> void {
        if (_stopped.exchange(true)) {
            return;
        }
        _clients.clear();
        _servers.clear();
        _work.reset();
        _ioc->stop();
        for (auto& t : _io_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        _io_threads.clear();
        // `_ioc` itself is deliberately *not* reset here. Dropping this
        // fixture's reference is all shutdown owes; the `io_context` is
        // destroyed by whichever reference goes last, which may be a Folly
        // chain releasing a strand executor on another thread long after this
        // returns. Every io thread has been joined by then, so nothing is
        // inside `run()` when that happens, which is all `~io_context`
        // requires.
    }

private:
    [[nodiscard]] auto port_of(std::uint64_t id) const -> std::uint16_t {
        const auto& url = _urls.at(id);
        return static_cast<std::uint16_t>(std::stoi(url.substr(url.rfind(':') + 1)));
    }

    // `shared_ptr` rather than a value member: see the constructor. Declared
    // first so it is destroyed last of this fixture's own members.
    std::shared_ptr<boost::asio::io_context> _ioc{std::make_shared<boost::asio::io_context>()};
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work;
    std::vector<std::thread> _io_threads;
    std::atomic<bool> _stopped{false};
    std::unordered_map<std::uint64_t, std::string> _urls;
    std::unordered_map<std::uint64_t, std::unique_ptr<server_type>> _servers;
    std::unordered_map<std::uint64_t, std::unique_ptr<client_type>> _clients;
};
#endif  // KYTHIRA_BENCH_HAS_BEAST

#if defined(KYTHIRA_BENCH_HAS_PROXYGEN)
/// @brief Proxygen: asynchronous on a caller-owned `folly::IOThreadPoolExecutor`.
template<typename Serializer> class proxygen_http_transport {
public:
    using transport_bundle = harness_transport_types<Serializer>;
    using client_type = kythira::proxygen_client<transport_bundle>;
    using server_type = kythira::proxygen_server<transport_bundle>;

    static auto name() -> std::string_view { return "proxygen"; }
    static auto tier() -> deployment_tier { return deployment_tier::b_loopback; }
    static auto capabilities() -> transport_capabilities { return transport_capabilities{}; }

    explicit proxygen_http_transport(const std::vector<std::uint64_t>& nodes)
        : _io(std::make_shared<folly::IOThreadPoolExecutor>(
              std::max(2U, std::thread::hardware_concurrency() / 2))) {
        for (auto id : nodes) {
            _urls.emplace(id, loopback_url(reserve_port()));
        }
        for (auto id : nodes) {
            _servers.emplace(id, std::make_unique<server_type>("127.0.0.1", port_of(id),
                                                               kythira::proxygen_server_config{},
                                                               noop_metrics{}, _io));
            _clients.emplace(
                id, std::make_unique<client_type>(*_io, _urls, kythira::proxygen_client_config{},
                                                  noop_metrics{}));
        }
    }

    ~proxygen_http_transport() { shutdown(); }

    proxygen_http_transport(const proxygen_http_transport&) = delete;
    auto operator=(const proxygen_http_transport&) -> proxygen_http_transport& = delete;

    [[nodiscard]] auto server(std::uint64_t id) -> server_type& { return *_servers.at(id); }
    [[nodiscard]] auto client(std::uint64_t id) -> client_type& { return *_clients.at(id); }

    /// @brief Nothing to wait for: this fixture's request handling happens on
    /// threads its own `shutdown()` joins, after the hosts are gone.
    auto drain() -> void {}

    auto shutdown() -> void {
        if (_stopped.exchange(true)) {
            return;
        }
        _clients.clear();
        _servers.clear();
        _io->join();
    }

private:
    [[nodiscard]] auto port_of(std::uint64_t id) const -> std::uint16_t {
        const auto& url = _urls.at(id);
        return static_cast<std::uint16_t>(std::stoi(url.substr(url.rfind(':') + 1)));
    }

    std::shared_ptr<folly::IOThreadPoolExecutor> _io;
    std::atomic<bool> _stopped{false};
    std::unordered_map<std::uint64_t, std::string> _urls;
    std::unordered_map<std::uint64_t, std::unique_ptr<server_type>> _servers;
    std::unordered_map<std::uint64_t, std::unique_ptr<client_type>> _clients;
};
#endif  // KYTHIRA_BENCH_HAS_PROXYGEN

// ─────────────────────────────────────────────────────────────────────────────
// Durability
// ─────────────────────────────────────────────────────────────────────────────

/// @brief What a durable row has to report, counted outside production code.
///
/// Requirement 3.4 names two numbers — fsyncs per second per host, and entries
/// per fsync — and Requirement 8.2 keeps the counter that produces them out of
/// `include/`. This lives on the harness side of `benchmark_persistence_engine`
/// for exactly that reason: the wrapper counts what it forwards.
struct durability_counters {
    /// `commit_batch()` calls that actually flushed. A batch that closed with
    /// nothing buffered issues no barrier, and counting it would divide the
    /// entry count by a number that includes ticks where nothing happened.
    std::atomic<std::uint64_t> _barriers{0};
    /// Batches that closed empty. Reported separately for the same reason
    /// empty AppendEntries are: a quiet tick is not a cheap fsync.
    std::atomic<std::uint64_t> _empty_batches{0};
    /// Log entries handed to `append_log_entry`, batched or not.
    std::atomic<std::uint64_t> _entries{0};
};

/// @brief One group's store, either memory- or file-backed, with the barrier
/// and entry counters on the outside of it.
///
/// A handle rather than a value. `store_factory` returns by value and
/// `multi_raft` moves the result into the group's `node`, so a caller that
/// wants to open a batch across every group later has no way to reach the
/// engines it created — unless the thing it returned was a handle to shared
/// state, which is what this is. `file_persistence_engine` is also move-
/// constructible but not move-assignable and holds a mutex, so it wants to
/// live behind a pointer regardless.
///
/// It exists at all because `kv_host_types::persistence_engine_type` is one
/// type. Making it a template parameter would thread a second parameter
/// through `kv_cluster`, every fixture and every row definition, to express a
/// choice that is a runtime option everywhere else in this harness.
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t>
class benchmark_persistence_engine {
public:
    using memory_engine_type = kythira::memory_persistence_engine<NodeId, TermId, LogIndex>;
    using file_engine_type = kythira::file_persistence_engine<NodeId, TermId, LogIndex>;
    using log_entry_t = typename memory_engine_type::log_entry_t;
    using snapshot_t = typename memory_engine_type::snapshot_t;

    /// Memory-backed, and the default so that every existing row keeps the
    /// engine it was measured with.
    ///
    /// `counters` is optional here for the same reason it is not on the file
    /// constructor: a memory row has no barrier to count, but it does append
    /// the same entries, and a durability table whose control row reads zero
    /// entries looks like a row that did no work rather than a row that did
    /// the same work without a disk.
    explicit benchmark_persistence_engine(durability_counters* counters = nullptr)
        : _state(std::make_shared<state>()) {
        _state->_memory.emplace();
        _state->_counters = counters;
    }

    /// File-backed at `dir`. `counters` may be null; a row that does not
    /// report durability does not have to supply one.
    benchmark_persistence_engine(std::filesystem::path dir, durability_counters* counters)
        : _state(std::make_shared<state>()) {
        _state->_file = std::make_unique<file_engine_type>(std::move(dir));
        _state->_counters = counters;
    }

    [[nodiscard]] auto file_backed() const -> bool { return _state->_file != nullptr; }

    // ── persistence_engine (include/raft/persistence.hpp) ────────────────────

    auto save_current_term(TermId term) -> void {
        dispatch([&](auto& e) { e.save_current_term(term); });
    }
    auto load_current_term() -> TermId {
        return dispatch([&](auto& e) { return e.load_current_term(); });
    }
    auto save_voted_for(NodeId node) -> void {
        dispatch([&](auto& e) { e.save_voted_for(node); });
    }
    auto load_voted_for() -> std::optional<NodeId> {
        return dispatch([&](auto& e) { return e.load_voted_for(); });
    }

    auto append_log_entry(const log_entry_t& entry) -> void {
        if (_state->_counters != nullptr) {
            _state->_counters->_entries.fetch_add(1, std::memory_order_relaxed);
        }
        _state->_in_batch.fetch_add(1, std::memory_order_relaxed);
        dispatch([&](auto& e) { e.append_log_entry(entry); });
    }
    auto get_log_entry(LogIndex index) -> std::optional<log_entry_t> {
        return dispatch([&](auto& e) { return e.get_log_entry(index); });
    }
    auto get_log_entries(LogIndex start, LogIndex end) -> std::vector<log_entry_t> {
        return dispatch([&](auto& e) { return e.get_log_entries(start, end); });
    }
    auto get_last_log_index() -> LogIndex {
        return dispatch([&](auto& e) { return e.get_last_log_index(); });
    }
    auto truncate_log(LogIndex index) -> void {
        dispatch([&](auto& e) { e.truncate_log(index); });
    }
    auto save_snapshot(const snapshot_t& snap) -> void {
        dispatch([&](auto& e) { e.save_snapshot(snap); });
    }
    auto load_snapshot() -> std::optional<snapshot_t> {
        return dispatch([&](auto& e) { return e.load_snapshot(); });
    }
    auto delete_log_entries_before(LogIndex index) -> void {
        dispatch([&](auto& e) { e.delete_log_entries_before(index); });
    }

    // ── The batch, reached through the handle rather than the concept ────────
    //
    // Deliberately NOT named begin_batch/commit_batch/abort_batch. Satisfying
    // `batched_persistence_engine` would change what `group_scoped_persistence`
    // exposes and therefore what a future `tick()` could call, which is a
    // behaviour change smuggled in as a benchmark. The harness's controller
    // calls these by name on the handles it kept.

    auto open_barrier() -> void {
        if (!_state->_file) {
            return;
        }
        _state->_in_batch.store(0, std::memory_order_relaxed);
        _state->_file->begin_batch();
    }
    auto close_barrier() -> void {
        if (!_state->_file) {
            return;
        }
        // Whether this commit will actually fsync is decided by whether
        // anything was buffered, and `file_persistence_engine` does not expose
        // that. Counting the appends this handle forwarded since
        // `open_barrier()` answers it from the outside, which is the point:
        // Requirement 8.2 keeps the counter out of production code, so the
        // wrapper counts what it forwards rather than asking the engine.
        //
        // The distinction is not pedantic. `commit_batch()` returns without
        // touching the disk when its buffer is empty, and a tick in which no
        // group had anything to append is most ticks in a quiet window.
        // Folding those into the barrier count would divide the entry total by
        // ticks in which nothing happened, and report a system that fsyncs far
        // more often and far more cheaply than it does.
        const bool flushed = _state->_in_batch.load(std::memory_order_relaxed) != 0;
        _state->_file->commit_batch();
        if (_state->_counters != nullptr) {
            if (flushed) {
                _state->_counters->_barriers.fetch_add(1, std::memory_order_relaxed);
            } else {
                _state->_counters->_empty_batches.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    auto abort_barrier() -> void {
        if (_state->_file) {
            _state->_file->abort_batch();
        }
    }

private:
    struct state {
        std::optional<memory_engine_type> _memory{};
        std::unique_ptr<file_engine_type> _file{};
        durability_counters* _counters{nullptr};
        /// Appends forwarded since the last `open_barrier()`. Only read by
        /// `close_barrier()`, on the same host driver thread that opened it.
        std::atomic<std::uint64_t> _in_batch{0};
    };

    template<typename F> auto dispatch(F&& f) -> decltype(auto) {
        if (_state->_file) {
            return f(*_state->_file);
        }
        return f(*_state->_memory);
    }

    std::shared_ptr<state> _state;
};

// ─────────────────────────────────────────────────────────────────────────────
// The host `Types` bundle, over whichever transport
// ─────────────────────────────────────────────────────────────────────────────

/// @brief `raft_types` for a multi-Raft host whose transport is `Transport`.
///
/// `serializer_type` here is the *node-internal* one (log entries, snapshots) and
/// is deliberately held at JSON across every row: the axis being swept is the
/// **wire** serializer, which lives in the transport's own bundle. Holding the
/// node-internal one fixed keeps the rows comparable.
template<typename Transport> struct kv_host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type = transport_client_handle<typename Transport::client_type>;
    using network_server_type = transport_server_handle<typename Transport::server_type>;

    /// The wrapper, not `memory_persistence_engine` directly. It defaults to a
    /// memory engine, so every row taken before Requirement 3.4 existed keeps
    /// the store it was measured with; `kv_cluster_options::_durability`
    /// chooses a file-backed one instead.
    using persistence_engine_type =
        benchmark_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kythira::test_key_value_state_machine<log_index_type>;

    using configuration_type = kythira::raft_configuration;

    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;

    using request_vote_request_type =
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type, group_id_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type, group_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type,
                                        group_id_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type, group_id_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type,
                                          group_id_type>;
    using install_snapshot_response_type =
        kythira::install_snapshot_response<term_id_type, group_id_type>;
};

// ─────────────────────────────────────────────────────────────────────────────
// A KV cluster over a real transport
// ─────────────────────────────────────────────────────────────────────────────

/// @brief How the cluster is shaped. Every field is reported with the result:
/// a number without its configuration is not a measurement.
struct kv_cluster_options {
    std::size_t _nodes{3};
    std::size_t _groups{4};
    std::uint64_t _key_count{100000};
    std::size_t _executor_stripes{2};

    /// How often each host's driver thread calls `tick()`. The tick is this
    /// library's only clock, so this is a latency floor for anything that waits
    /// on a heartbeat — swept deliberately rather than left implicit.
    std::chrono::milliseconds _tick_interval{2};

    std::chrono::milliseconds _election_timeout_min{300};
    std::chrono::milliseconds _election_timeout_max{600};
    std::chrono::milliseconds _heartbeat_interval{50};

    /// Which engine every group's store is built from, and — for
    /// `file_barrier` — whether this harness supplies the controller that is
    /// the only thing in the codebase able to open a batch. Defaults to
    /// `memory`, so a row that does not ask for durability gets exactly the
    /// store every earlier row was measured with.
    durability_mode _durability{durability_mode::memory};

    /// Where a file-backed run puts its logs. Empty means a fresh directory
    /// under `std::filesystem::temp_directory_path()`, removed on shutdown.
    /// A cloud row points this at the volume whose class and IOPS the
    /// provenance records (Requirement 3.4).
    std::filesystem::path _data_dir{};
};

/// @brief `_nodes` hosts, each holding a replica of every one of `_groups`
/// shards, wired to each other over a real transport.
///
/// Each host is driven by its own thread, the way `cmd/chaos_node/main.cpp`
/// drives a single node: `multi_raft` has no timer thread of its own, and that
/// is the property the whole design rests on.
///
/// Member order is load-bearing. `_transport` is declared first so it is
/// destroyed *last*: a host's `stop()` still touches its server, and the
/// transport owns both.
template<typename Transport> class kv_cluster {
public:
    using transport_type = Transport;
    using types = kv_host_types<Transport>;
    using key_type = std::string;
    using group_id_type = std::uint64_t;
    using host_type = kythira::multi_raft<types, key_type, group_id_type>;
    using config_type = kythira::multi_raft_config<types, key_type, group_id_type>;
    using descriptor_type = kythira::shard_descriptor<group_id_type, key_type, std::uint64_t>;

    explicit kv_cluster(kv_cluster_options options)
        : _options(std::move(options)), _transport(node_ids(_options._nodes)) {
        const auto voters = node_ids(_options._nodes);
        _ranges = kv_shard_ranges(_options._groups, _options._key_count);

        if (_options._durability != durability_mode::memory) {
            if (_options._data_dir.empty()) {
                // Owned, and therefore removed in `shutdown()`. A run that
                // left its logs behind would make the next run's first append
                // land on a non-empty log, which is a correctness problem
                // before it is a measurement one.
                _data_root =
                    std::filesystem::temp_directory_path() /
                    ("kythira-bench-" + std::to_string(::getpid()) + "-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                _owns_data_root = true;
            } else {
                // A cloud row points this at the provisioned volume whose
                // class and IOPS the provenance records. Not removed: it is
                // the caller's directory, and deleting somebody's mount point
                // contents on shutdown is not this class's business.
                _data_root = _options._data_dir;
                _owns_data_root = false;
            }
            std::filesystem::create_directories(_data_root);
        }

        for (std::uint64_t id = 1; id <= _options._nodes; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
            for (std::size_t g = 0; g < _options._groups; ++g) {
                descriptor_type d;
                d._group_id = static_cast<group_id_type>(g + 1);
                d._range = _ranges[g];
                d._epoch = kythira::shard_epoch{._version = 1, ._conf_version = 1};
                d._voters = voters;
                _hosts.back()->create_group(d);
            }
        }
        for (auto& h : _hosts) {
            h->start();
        }
        _running = true;
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            _drivers.emplace_back([this, i] { drive(i); });
        }
    }

    ~kv_cluster() { shutdown(); }

    kv_cluster(const kv_cluster&) = delete;
    auto operator=(const kv_cluster&) -> kv_cluster& = delete;

    /// @brief Stop the drivers, then the hosts, then the transport — in that
    /// order, and exactly once.
    ///
    /// A host destroyed with a group still running terminates the process
    /// (`~group_state` destroys unstopped nodes through a deferred closure's
    /// reference), so this is not merely tidy.
    auto shutdown() -> void {
        if (_shut_down.exchange(true)) {
            return;
        }
        _running = false;
        for (auto& t : _drivers) {
            if (t.joinable()) {
                t.join();
            }
        }
        _drivers.clear();
        for (auto& h : _hosts) {
            h->stop();
        }
        // Between stopping the hosts and destroying them, and never anywhere
        // else. `stop()` makes a transport refuse *new* requests; it does not
        // wait for the ones already inside a handler, and those handlers hold
        // references into the hosts about to be freed. The HTTP fixtures own
        // threads that are joined in their own `shutdown()` below and have
        // nothing to do here; the fabric hands a copied-out handler to a worker
        // and runs it outside its lock, so it does.
        _transport.drain();
        _hosts.clear();
        _transport.shutdown();
        // After the hosts, because a store outlives the node that held it
        // only by the handle this class kept, and a directory removed while a
        // group is still appending is a crash rather than a leak.
        _stores_by_host.clear();
        if (_owns_data_root && !_data_root.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(_data_root, ec);
        }
    }

    /// @brief Barriers and entries since this cluster started.
    ///
    /// Requirement 3.4's two numbers come from differencing two of these
    /// around the measured window: a barrier issued during election or warm-up
    /// belongs to neither the numerator nor the denominator of "entries per
    /// fsync".
    [[nodiscard]] auto durability_counts() const -> durability_snapshot {
        return durability_snapshot{
            ._barriers = _durability_counters._barriers.load(std::memory_order_relaxed),
            ._empty_batches = _durability_counters._empty_batches.load(std::memory_order_relaxed),
            ._entries = _durability_counters._entries.load(std::memory_order_relaxed),
        };
    }

    [[nodiscard]] auto host(std::size_t index) -> host_type& { return *_hosts.at(index); }
    [[nodiscard]] auto host_count() const -> std::size_t { return _hosts.size(); }
    [[nodiscard]] auto options() const -> const kv_cluster_options& { return _options; }

    /// @brief The host whose replica of `group` currently leads, if any.
    ///
    /// A sample, not a fact: leadership read on one line is stale on the next.
    /// Callers that act on it must tolerate the answer having changed.
    [[nodiscard]] auto leader_of(group_id_type group) -> host_type* {
        for (auto& h : _hosts) {
            auto* n = h->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return h.get();
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto await_all_leaders(std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            bool all = true;
            for (std::size_t g = 1; g <= _options._groups; ++g) {
                if (leader_of(static_cast<group_id_type>(g)) == nullptr) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    /// @brief Replication RPCs issued by every node in this cluster so far.
    ///
    /// Cumulative from construction; subtract two snapshots to get a window.
    /// The count is cluster-wide rather than leader-only on purpose — a
    /// follower that starts campaigning is a cost the window paid, and a
    /// leader-only count would hide it.
    [[nodiscard]] auto rpc_counts() const -> rpc_snapshot { return _rpc.snapshot(); }

    /// @brief Sum of every group's current term, as a cheap "did anything
    /// re-elect" probe across a measured window.
    [[nodiscard]] auto term_sum() -> std::uint64_t {
        std::uint64_t total = 0;
        for (auto& h : _hosts) {
            for (std::size_t g = 1; g <= _options._groups; ++g) {
                auto* n = h->group_node(static_cast<group_id_type>(g));
                if (n != nullptr) {
                    total += n->get_current_term();
                }
            }
        }
        return total;
    }

    /// @brief Run `command` against whichever host leads the shard owning `key`,
    /// classifying the failure if it does not commit.
    ///
    /// Returns the latency actually waited, or `nullopt` on failure — measured
    /// from before the routing lookup, so it includes everything the caller paid
    /// rather than only the part after the shard was found.
    auto run_command(const key_type& key, const std::vector<std::byte>& command,
                     std::chrono::milliseconds timeout, operation_tally& tally,
                     std::vector<std::byte>* result = nullptr)
        -> std::optional<std::chrono::nanoseconds> {
        const auto started = std::chrono::steady_clock::now();
        ++tally._offered;

        auto* leader = leader_for_key(key);
        if (leader == nullptr) {
            ++tally._not_leader;
            return std::nullopt;
        }

        try {
            auto f = leader->submit_command(key, command, timeout);
            if (!f.wait(timeout)) {
                ++tally._timeout;
                return std::nullopt;
            }
            auto value = std::move(f).get();
            if (result != nullptr) {
                *result = std::move(value);
            }
            ++tally._completed;
            return std::chrono::steady_clock::now() - started;
        } catch (const kythira::shard_not_leader_exception<group_id_type, std::uint64_t>&) {
            ++tally._not_leader;
        } catch (const kythira::shard_epoch_mismatch_exception<group_id_type, key_type,
                                                               std::uint64_t>&) {
            ++tally._epoch_mismatch;
        } catch (const kythira::shard_merging_exception<group_id_type>&) {
            ++tally._merging;
        } catch (const std::exception&) {
            ++tally._other;
        }
        return std::nullopt;
    }

    /// @brief What a client that already holds a descriptor would name: the
    ///        group id and the epoch it computed its request against.
    ///
    /// Sampled from a host's own shard map, one entry per group, indexed by
    /// `group - 1`. Callers take it **once**, before a measured window, which is
    /// what a real client's descriptor cache is: automatic split and merge are
    /// off for every row in this suite (`make_config`), so no epoch moves under
    /// a window and a cache taken at the start is still valid at the end. If one
    /// ever did move, every operation in the treatment arm would fail with an
    /// epoch mismatch and `operation_tally::_epoch_mismatch` would say so — the
    /// staleness is visible rather than silent.
    [[nodiscard]] auto descriptor_cache() -> std::vector<addressed_shard> {
        std::vector<addressed_shard> cache;
        cache.reserve(_options._groups);
        const auto map = _hosts.front()->shard_map_snapshot();
        for (std::size_t g = 1; g <= _options._groups; ++g) {
            const auto id = static_cast<group_id_type>(g);
            const auto desc = map.find(id);
            cache.push_back(addressed_shard{
                ._group = id, ._epoch = desc.has_value() ? desc->_epoch : kythira::shard_epoch{}});
        }
        return cache;
    }

    /// @brief The group whose range contains `key`, found in the harness's own
    ///        copy of the tiling.
    ///
    /// Deliberately **not** `multi_raft::resolve`: that is the routing lookup
    /// Requirement 8.3 asks this suite to price, and paying it here would put it
    /// back into both arms of the comparison. A scan of `_groups` ranges is
    /// identical in the two arms, so whatever it costs cancels out of the delta
    /// instead of contaminating it.
    [[nodiscard]] auto group_of_key(const key_type& key) const -> std::optional<group_id_type> {
        for (std::size_t i = 0; i < _ranges.size(); ++i) {
            if (_ranges[i].contains(key)) {
                return static_cast<group_id_type>(i + 1);
            }
        }
        return std::nullopt;
    }

    /// @brief Run `command` against the leader of `target._group`, addressed
    ///        either by key or by group id — the two arms of Requirement 8.3.
    ///
    /// The leader is found the same way in both arms, from the group id the
    /// caller already holds, so the only thing that differs between them is
    /// which `submit_command` overload runs. That is what makes the difference
    /// in latency attributable to the routing lookup rather than to this
    /// harness.
    ///
    /// `run_command` above is left alone and still used by every other row: it
    /// is the production path (Requirement 1.7), and this one is the relaxation
    /// Requirement 1.7 permits only here.
    auto run_attributed_command(const addressed_shard& target, const key_type& key,
                                const std::vector<std::byte>& command, routing_mode routing,
                                std::chrono::milliseconds timeout, operation_tally& tally)
        -> std::optional<std::chrono::nanoseconds> {
        const auto started = std::chrono::steady_clock::now();
        ++tally._offered;

        auto* leader = leader_of(target._group);
        if (leader == nullptr) {
            ++tally._not_leader;
            return std::nullopt;
        }

        try {
            auto f = routing == routing_mode::attributed_group
                         ? leader->submit_command(target._group, target._epoch, command, timeout)
                         : leader->submit_command(key, command, timeout);
            if (!f.wait(timeout)) {
                ++tally._timeout;
                return std::nullopt;
            }
            std::ignore = std::move(f).get();
            ++tally._completed;
            return std::chrono::steady_clock::now() - started;
        } catch (const kythira::shard_not_leader_exception<group_id_type, std::uint64_t>&) {
            ++tally._not_leader;
        } catch (const kythira::shard_epoch_mismatch_exception<group_id_type, key_type,
                                                               std::uint64_t>&) {
            ++tally._epoch_mismatch;
        } catch (const kythira::shard_merging_exception<group_id_type>&) {
            ++tally._merging;
        } catch (const std::exception&) {
            ++tally._other;
        }
        return std::nullopt;
    }

    /// @brief `multi_raft::read_state` against the shard owning `key` — the
    /// quorum-confirmed **whole-store** read, not a point lookup.
    auto run_read_state(const key_type& key, std::chrono::milliseconds timeout,
                        operation_tally& tally, std::size_t& bytes_returned)
        -> std::optional<std::chrono::nanoseconds> {
        const auto started = std::chrono::steady_clock::now();
        ++tally._offered;

        auto* leader = leader_for_key(key);
        if (leader == nullptr) {
            ++tally._not_leader;
            return std::nullopt;
        }

        try {
            auto f = leader->read_state(key, timeout);
            if (!f.wait(timeout)) {
                ++tally._timeout;
                return std::nullopt;
            }
            auto value = std::move(f).get();
            bytes_returned += value.size();
            ++tally._completed;
            return std::chrono::steady_clock::now() - started;
        } catch (const std::exception&) {
            ++tally._other;
        }
        return std::nullopt;
    }

    /// @brief The local, non-consensus read: a replica's state machine, queried
    ///        directly.
    ///
    /// **Deliberately prefers a replica that is NOT the leader.** A "stale read"
    /// served by the leader would be stale only in theory; served by a follower
    /// it is stale in the way a deployment would actually experience, and the
    /// row is only honest about being non-linearizable if it can be.
    ///
    /// Note what this still pays: `with_state_machine` takes the node's own
    /// `_mutex` — the very lock H7 is about. A read that skips consensus
    /// entirely does not skip the per-group serialization, and that is a
    /// finding, not an implementation detail of the harness.
    auto run_local_read(const key_type& key, operation_tally& tally, std::size_t& bytes_returned)
        -> std::optional<std::chrono::nanoseconds> {
        const auto started = std::chrono::steady_clock::now();
        ++tally._offered;

        auto* replica = follower_for_key(key);
        if (replica == nullptr) {
            // No replica holds this shard at all — a routing failure, not a
            // stale answer. Counted apart from a miss.
            ++tally._not_leader;
            return std::nullopt;
        }

        try {
            auto value =
                replica->with_state_machine([&key](auto& sm) { return sm.get_value(key); });
            // A miss is a completed read. The store is preloaded before the
            // measured window, so a miss here means the follower has not
            // applied that entry yet — which is exactly the staleness this row
            // exists to expose, and counting it as a failure would hide it.
            bytes_returned += value.has_value() ? value->size() : 0;
            ++tally._completed;
            return std::chrono::steady_clock::now() - started;
        } catch (const std::exception&) {
            ++tally._other;
        }
        return std::nullopt;
    }

    /// @brief Every host's routing table tiles the key space.
    [[nodiscard]] auto tiling_problem() -> std::optional<std::string> {
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            if (auto problem = _hosts[i]->shard_map_snapshot().check_tiling()) {
                return "host " + std::to_string(i + 1) + ": " + *problem;
            }
        }
        return std::nullopt;
    }

private:
    [[nodiscard]] auto leader_for_key(const key_type& key) -> host_type* {
        for (auto& h : _hosts) {
            auto desc = h->resolve(key);
            if (!desc.has_value()) {
                continue;
            }
            auto* n = h->group_node(desc->_group_id);
            if (n != nullptr && n->is_leader()) {
                return h.get();
            }
        }
        return nullptr;
    }

    /// @brief A replica of `key`'s shard that is not currently its leader, or
    ///        the leader if this cluster somehow has no other replica.
    ///
    /// The fallback is not a nicety: a row that silently measured nothing
    /// because every candidate was the leader would be worse than one that
    /// measured a leader-local read and can be seen to have done so in the
    /// tally.
    [[nodiscard]] auto follower_for_key(const key_type& key) ->
        typename host_type::group_node_type* {
        typename host_type::group_node_type* leader_node = nullptr;
        for (auto& h : _hosts) {
            auto desc = h->resolve(key);
            if (!desc.has_value()) {
                continue;
            }
            auto* n = h->group_node(desc->_group_id);
            if (n == nullptr) {
                continue;
            }
            if (!n->is_leader()) {
                return n;
            }
            leader_node = n;
        }
        return leader_node;
    }

    static auto node_ids(std::size_t count) -> std::vector<std::uint64_t> {
        std::vector<std::uint64_t> ids;
        ids.reserve(count);
        for (std::uint64_t i = 1; i <= count; ++i) {
            ids.push_back(i);
        }
        return ids;
    }

    auto make_config(std::uint64_t id) -> config_type {
        config_type cfg{
            .node_id = id,
            .network_client =
                transport_client_handle<typename Transport::client_type>{_transport.client(id),
                                                                         &_rpc},
            .network_server =
                transport_server_handle<typename Transport::server_type>{_transport.server(id)},
            .store_factory = [this,
                              id](const group_id_type& group) { return make_store(id, group); },
        };
        cfg.config._election_timeout_min = _options._election_timeout_min;
        cfg.config._election_timeout_max = _options._election_timeout_max;
        cfg.config._heartbeat_interval = _options._heartbeat_interval;
        // Hibernation off: a benchmark whose population hibernated mid-window
        // would be measuring hibernation.
        cfg.hibernation = kythira::hibernation_mode::off;
        cfg.executor_stripes = _options._executor_stripes;
        // The policy phase and automatic split/merge are off so that neither
        // lands inside a timed window. Split-under-load is its own scenario.
        cfg.policy_interval = std::chrono::hours{1};
        cfg.split_merge_interval = std::chrono::hours{1};
        cfg.automatic_split_merge_enabled = false;
        cfg.heartbeat_interval = std::chrono::milliseconds{0};
        cfg.partitioner = kythira::make_partitioner<key_type>(kv_partitioner{});
        // The ONLY thing in this codebase that opens a batch. `tick()` has no
        // fallback — nothing else calls `begin_batch()` — so without this a
        // file-backed log is written to the page cache and never fsynced,
        // which is `durability_mode::file_buffered` and is labelled NOT
        // DURABLE wherever it appears (Requirement 3.5).
        //
        // It fans out across the per-group handles this host created, so it is
        // N barriers wearing one name rather than one barrier for N groups.
        // `tick_batch_controller`'s own documentation is explicit that no
        // wrapper can manufacture the second from N independent engines, and a
        // row measured through this must say which one it got.
        if (_options._durability == durability_mode::file_barrier) {
            // `operator[]`, not `at`: `make_config(id)` runs before this
            // host's first `create_group`, so the vector does not exist yet.
            // `unordered_map` keeps pointers to its elements valid across
            // rehash, so the address taken here stays good as other hosts are
            // added.
            auto* stores = &_stores_by_host[id];
            cfg.batch_controller = kythira::tick_batch_controller{
                ._begin =
                    [stores] {
                        for (auto& [group, store] : *stores) {
                            store.open_barrier();
                        }
                    },
                ._commit =
                    [stores] {
                        for (auto& [group, store] : *stores) {
                            store.close_barrier();
                        }
                    },
                ._abort =
                    [stores] {
                        for (auto& [group, store] : *stores) {
                            store.abort_barrier();
                        }
                    },
            };
        }
        return cfg;
    }

    /// @brief One group's store on one host, remembered so the controller can
    /// reach it.
    ///
    /// `store_factory` hands its result to `multi_raft`, which moves it into
    /// the group's `node`; the handle kept here shares state with that copy,
    /// which is the whole reason `benchmark_persistence_engine` is a handle.
    auto make_store(std::uint64_t host_id, const group_id_type& group) ->
        typename types::persistence_engine_type {
        if (_options._durability == durability_mode::memory) {
            return typename types::persistence_engine_type{&_durability_counters};
        }
        // One directory per host per group. `group_scoped_persistence` owns
        // its engine and the isolation is in the location, so two groups
        // sharing a directory would share a log file and interleave two
        // independent index spaces into it.
        const auto dir =
            _data_root / ("host-" + std::to_string(host_id)) / ("group-" + std::to_string(group));
        typename types::persistence_engine_type store{dir, &_durability_counters};
        _stores_by_host[host_id].emplace_back(group, store);
        return store;
    }

    auto drive(std::size_t index) -> void {
        while (_running.load(std::memory_order_relaxed)) {
            _hosts[index]->tick();
            std::this_thread::sleep_for(_options._tick_interval);
        }
    }

    kv_cluster_options _options;
    /// The tiling the hosts were built from, kept so that `group_of_key` can
    /// answer without consulting a `multi_raft` shard map — which is the very
    /// lookup the routing-cost scenario exists to price.
    std::vector<kythira::shard_range<key_type>> _ranges;
    /// Where a file-backed run's logs live, and whether this object owns the
    /// directory. An owned one is removed in `shutdown()`; a caller-supplied
    /// one — a cloud row pointing at a provisioned volume — is left alone.
    std::filesystem::path _data_root;
    bool _owns_data_root{false};
    /// Barrier and entry counts, shared by every store on every host. Declared
    /// before `_transport` and `_hosts` for the same reason `_rpc` is: a tick
    /// still in flight during `shutdown()` writes through a pointer to it.
    durability_counters _durability_counters;
    /// Every store this cluster created, by host, so the per-host batch
    /// controller can open and close a barrier across all of them. Keyed by
    /// host id rather than index because `make_config` runs before `_hosts`
    /// has an entry to index.
    std::unordered_map<
        std::uint64_t,
        std::vector<std::pair<group_id_type, typename types::persistence_engine_type>>>
        _stores_by_host;
    /// Declared before `_transport` and `_hosts`, so it is destroyed after
    /// them: every handle inside a host holds a pointer to it, and a tick
    /// still in flight during `shutdown()` would otherwise write through a
    /// dangling one. Same reasoning, and the same hazard, as the `io_context`
    /// keeper in the Beast client.
    rpc_counters _rpc;
    Transport _transport;
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
    std::atomic<bool> _shut_down{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// The closed-loop workload driver
// ─────────────────────────────────────────────────────────────────────────────

/// @brief What one measured window asks for.
struct workload_options {
    std::size_t _in_flight{8};
    std::size_t _operations{400};
    std::size_t _value_bytes{128};
    std::uint64_t _key_count{100000};
    /// Multiplies every sampled key index. Exists so a read row can be run
    /// against a *preloaded* store: the preloader writes `kv_key(i * stride)`
    /// for `i` in `[0, _key_count)` and the sampler draws the same `i`, so
    /// every read hits. At stride 1 the keys are contiguous and therefore all
    /// in one shard — which is what the shard-size curve wants — and at
    /// `100000 / _key_count` they are spread evenly over the whole space, which
    /// is what the taxonomy rows want. Default 1 leaves every write row
    /// unchanged.
    std::uint64_t _key_stride{1};
    key_distribution _distribution{key_distribution::uniform};
    double _zipf_theta{0.99};
    std::chrono::milliseconds _op_timeout{3000};
    /// How the command is addressed. Defaults to the production path, so a row
    /// that does not mention routing gets the routing every other row got.
    routing_mode _routing{routing_mode::by_key};
    /// Closed loop by default, so every row written before Requirement 4.2 was
    /// implemented keeps the shape it was measured in.
    load_mode _load{load_mode::closed_loop};
    /// Operations per second the open-loop schedule asks for, across all
    /// workers. Ignored in closed loop.
    double _offered_rate_per_second{0.0};
    std::string _scenario{"put"};
};

/// @brief Assemble the parts of a `benchmark_result` that do not depend on
///        whether the window was reads or writes.
///
/// Shared rather than duplicated because the labels are the part a reader
/// trusts — `_serializer` read off the serializer's own `media_type()`, and
/// `_node_serializer` off the bundle the cluster was actually built from — and
/// two copies of that logic is two chances for a read row to describe itself
/// differently from a write row taken on the same cluster.
template<typename Transport>
auto fill_result(const kv_cluster<Transport>& cluster, const workload_options& workload,
                 latency_sample_set& samples, const operation_tally& tally,
                 std::chrono::nanoseconds elapsed, const rpc_snapshot& rpc,
                 const durability_snapshot& durability) -> benchmark_result {
    samples.sort();

    typename Transport::transport_bundle::serializer_type serializer{};
    typename kv_cluster<Transport>::types::serializer_type node_serializer{};

    benchmark_result result;
    result._transport = std::string{Transport::name()};
    result._serializer = serializer.media_type();
    result._node_serializer = node_serializer.media_type();
    result._scenario = workload._scenario;
    result._nodes = cluster.options()._nodes;
    result._groups = cluster.options()._groups;
    result._value_bytes = workload._value_bytes;
    result._in_flight = workload._in_flight;
    result._tier = Transport::tier();
    result._routing = workload._routing;
    result._load = workload._load;
    result._offered_rate_per_second =
        workload._load == load_mode::open_loop ? workload._offered_rate_per_second : 0.0;
    result._tick_interval = cluster.options()._tick_interval;
    result._duration = elapsed;
    result._tally = tally;
    result._rpc = rpc;
    result._durability = cluster.options()._durability;
    result._durability_counts = durability;
    result._p50 = samples.p50();
    result._p95 = samples.p95();
    result._p99 = samples.p99();
    const auto seconds = std::chrono::duration<double>(elapsed).count();
    result._ops_per_second = seconds > 0.0 ? static_cast<double>(tally._completed) / seconds : 0.0;
    return result;
}

/// @brief Run `_operations` PUTs spread over `_in_flight` client threads, in
///        whichever of Requirement 4.1's two modes `_load` names.
///
/// **Closed loop** is the default and what every CI-registered row uses: each
/// worker holds exactly one operation outstanding, so `_in_flight` is literally
/// the concurrency and the offered rate is an outcome rather than an input.
///
/// **Open loop** offers `_offered_rate_per_second` on a fixed schedule
/// computed before the window starts, and measures each operation's latency
/// **from its intended start time rather than from dispatch** (Requirement
/// 4.2). That is the coordinated-omission correction, and it is the whole
/// reason the mode exists: a closed loop cannot produce a queue, because a
/// slow system simply receives less work, so its tail latency describes a load
/// nobody offered.
///
/// The schedule is round-robin across the workers — worker `w`'s `i`-th
/// operation is due at `start + (i * in_flight + w) / rate` — so the workers
/// interleave into one arrival process rather than each running its own.
///
/// **What this mode is not.** A true open loop needs unbounded concurrency,
/// and this one is served by `_in_flight` threads. When the system cannot keep
/// up, the due times recede into the past and operations issue back to back;
/// the offered rate then silently becomes the closed-loop rate. That failure is
/// **visible rather than silent**: `_mean_schedule_lag` is how far behind its
/// due time the average operation began, so a row whose lag is a large fraction
/// of its latency was not measured at the rate it asked for, and the row says
/// so instead of quietly reporting a lower rate as a result.
template<typename Transport>
auto run_put_workload(kv_cluster<Transport>& cluster, const workload_options& workload)
    -> benchmark_result {
    std::vector<latency_sample_set> per_worker(workload._in_flight);
    std::vector<operation_tally> tallies(workload._in_flight);
    std::vector<std::chrono::nanoseconds> lag(workload._in_flight, std::chrono::nanoseconds{0});
    const auto per_worker_ops = std::max<std::size_t>(
        1, workload._operations / std::max<std::size_t>(1, workload._in_flight));

    // The interval between consecutive arrivals across the whole schedule, not
    // per worker. Zero — and therefore no waiting at all — in closed loop, and
    // also when an open-loop row is handed a non-positive rate, which is a
    // caller error that would otherwise divide by zero.
    const auto arrival_interval =
        workload._load == load_mode::open_loop && workload._offered_rate_per_second > 0.0
            ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::duration<double>(1.0 / workload._offered_rate_per_second))
            : std::chrono::nanoseconds{0};

    // The client's descriptor cache, taken once before the window rather than
    // per operation: that is what a cache is, and paying for it inside the
    // window would price the thing the attributed arms exist to remove. Empty
    // and unused on the production path.
    const auto cache = workload._routing == routing_mode::by_key ? std::vector<addressed_shard>{}
                                                                 : cluster.descriptor_cache();

    // Taken before the clock and read after the join, so the window the RPC
    // ratios describe is exactly the window the throughput describes.
    const auto rpc_before = cluster.rpc_counts();
    const auto durability_before = cluster.durability_counts();
    const auto started = std::chrono::steady_clock::now();
    {
        std::vector<std::thread> workers;
        workers.reserve(workload._in_flight);
        for (std::size_t w = 0; w < workload._in_flight; ++w) {
            workers.emplace_back([&, w] {
                key_sampler sampler(workload._distribution, workload._key_count,
                                    workload._zipf_theta, 0x5eed0000ULL + w);
                per_worker[w].reserve(per_worker_ops);
                for (std::size_t i = 0; i < per_worker_ops; ++i) {
                    // Open loop: this operation was due at a time fixed before
                    // the window began. Waiting for it is what makes the
                    // arrival process independent of the system's speed, and
                    // measuring from it rather than from now is what corrects
                    // for coordinated omission.
                    std::chrono::steady_clock::time_point due = started;
                    if (arrival_interval > std::chrono::nanoseconds{0}) {
                        due = started + arrival_interval * (i * workload._in_flight + w);
                        const auto now = std::chrono::steady_clock::now();
                        if (now < due) {
                            std::this_thread::sleep_until(due);
                        } else {
                            // Behind schedule. Issue immediately and record by
                            // how much, so the row can say it was not measured
                            // at the rate it asked for.
                            lag[w] += now - due;
                        }
                    }

                    const auto n = sampler.next() * workload._key_stride;
                    const auto key = kv_key(n);
                    const auto command = kv_put(key, kv_value(n + i, workload._value_bytes));
                    std::optional<std::chrono::nanoseconds> latency;
                    if (workload._routing == routing_mode::by_key) {
                        latency =
                            cluster.run_command(key, command, workload._op_timeout, tallies[w]);
                    } else if (const auto group = cluster.group_of_key(key)) {
                        latency = cluster.run_attributed_command(cache.at(*group - 1), key, command,
                                                                 workload._routing,
                                                                 workload._op_timeout, tallies[w]);
                    } else {
                        // No range holds this key. A tiling failure, not a
                        // slow operation, and counted where a reader will see
                        // it rather than dropped.
                        ++tallies[w]._offered;
                        ++tallies[w]._not_leader;
                    }
                    if (latency) {
                        // Requirement 4.2: from the intended start, not from
                        // dispatch. `run_command` timed from its own entry, so
                        // the queueing delay this operation suffered while its
                        // worker was busy with the previous one is added back
                        // here. In closed loop `due` is the window start and
                        // this branch is not taken.
                        per_worker[w].record(arrival_interval > std::chrono::nanoseconds{0}
                                                 ? std::chrono::steady_clock::now() - due
                                                 : *latency);
                    }
                }
            });
        }
        for (auto& t : workers) {
            t.join();
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto rpc_after = cluster.rpc_counts();
    const auto durability_after = cluster.durability_counts();

    latency_sample_set all;
    operation_tally tally;
    std::chrono::nanoseconds total_lag{0};
    for (std::size_t w = 0; w < workload._in_flight; ++w) {
        all.merge(per_worker[w]);
        tally.merge(tallies[w]);
        total_lag += lag[w];
    }
    auto result = fill_result(cluster, workload, all, tally, elapsed, rpc_after - rpc_before,
                              durability_after - durability_before);
    result._mean_schedule_lag = tally._completed == 0
                                    ? std::chrono::nanoseconds{0}
                                    : total_lag / static_cast<std::int64_t>(tally._completed);
    return result;
}

/// @brief Write `count` keys so a read row has something to read.
///
/// Single-threaded and sequential: this is setup, not a measurement, and a
/// concurrent preloader would only add a way for it to half-fail. Returns how
/// many actually committed, which the caller checks — a read row over a store
/// that is 90% loaded is measuring a different thing from the one it claims,
/// and the only way to know is to count.
///
/// Keys are `kv_key(i * stride)` for `i` in `[0, count)`, matching what
/// `workload_options::_key_stride` makes the sampler draw.
template<typename Transport>
auto preload_keys(kv_cluster<Transport>& cluster, std::uint64_t count, std::uint64_t stride,
                  std::size_t value_bytes, std::chrono::milliseconds timeout,
                  operation_tally& tally) -> std::uint64_t {
    std::uint64_t committed = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto n = i * stride;
        const auto key = kv_key(n);
        if (cluster.run_command(key, kv_put(key, kv_value(n, value_bytes)), timeout, tally)) {
            ++committed;
        }
    }
    return committed;
}

/// @brief Run `_operations` reads of one kind, spread over `_in_flight` client
///        threads.
///
/// The kind is a parameter rather than three functions because everything
/// except the one call differs in nothing — and Requirement 2.1's rule that the
/// kinds are never aggregated is enforced by each *row* carrying its kind, not
/// by keeping the drivers apart.
///
/// **The store must already hold the keys.** A read workload against an empty
/// store measures a miss path: `read_state` returns an empty store, `GET`
/// returns nothing, and the local read finds nothing. Callers preload, and the
/// bytes-per-operation figure on the row is what shows whether they did.
template<typename Transport>
auto run_read_workload(kv_cluster<Transport>& cluster, const workload_options& workload,
                       read_kind kind) -> benchmark_result {
    std::vector<latency_sample_set> per_worker(workload._in_flight);
    std::vector<operation_tally> tallies(workload._in_flight);
    std::vector<std::size_t> bytes(workload._in_flight, 0);
    const auto per_worker_ops = std::max<std::size_t>(
        1, workload._operations / std::max<std::size_t>(1, workload._in_flight));

    const auto rpc_before = cluster.rpc_counts();
    const auto durability_before = cluster.durability_counts();
    const auto started = std::chrono::steady_clock::now();
    {
        std::vector<std::thread> workers;
        workers.reserve(workload._in_flight);
        for (std::size_t w = 0; w < workload._in_flight; ++w) {
            workers.emplace_back([&, w] {
                key_sampler sampler(workload._distribution, workload._key_count,
                                    workload._zipf_theta, 0x5eed0000ULL + w);
                per_worker[w].reserve(per_worker_ops);
                for (std::size_t i = 0; i < per_worker_ops; ++i) {
                    const auto key = kv_key(sampler.next() * workload._key_stride);
                    std::optional<std::chrono::nanoseconds> latency;
                    switch (kind) {
                        case read_kind::read_state:
                            latency = cluster.run_read_state(key, workload._op_timeout, tallies[w],
                                                             bytes[w]);
                            break;
                        case read_kind::log_get: {
                            std::vector<std::byte> out;
                            latency = cluster.run_command(key, kv_get(key), workload._op_timeout,
                                                          tallies[w], &out);
                            bytes[w] += out.size();
                            break;
                        }
                        case read_kind::local_stale:
                            latency = cluster.run_local_read(key, tallies[w], bytes[w]);
                            break;
                    }
                    if (latency) {
                        per_worker[w].record(*latency);
                    }
                }
            });
        }
        for (auto& t : workers) {
            t.join();
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto rpc_after = cluster.rpc_counts();
    const auto durability_after = cluster.durability_counts();

    latency_sample_set all;
    operation_tally tally;
    std::uint64_t total_bytes = 0;
    for (std::size_t w = 0; w < workload._in_flight; ++w) {
        all.merge(per_worker[w]);
        tally.merge(tallies[w]);
        total_bytes += bytes[w];
    }

    auto result = fill_result(cluster, workload, all, tally, elapsed, rpc_after - rpc_before,
                              durability_after - durability_before);
    result._bytes_returned = total_bytes;
    result._read_kind = kind;
    return result;
}

}  // namespace kythira::testing
