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
/// a `capabilities()` descriptor, and `server(id)` / `client(id)` accessors.
/// Nothing else in the harness or in the tests changes. A CoAP fixture would
/// differ only in owning a libcoap context instead of an `io_context` and in
/// reporting `_timeout_now = false`.

#include "multi_raft_kv_workload.hpp"

#include <raft/console_logger.hpp>
#include <raft/executor_default.hpp>
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::testing {

// ─────────────────────────────────────────────────────────────────────────────
// Movable handles over non-movable transports
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A movable `network_client` view of a transport the caller owns.
///
/// Every method forwards verbatim. The optional RPCs carry a `requires` clause
/// on `Client` so that this handle satisfies exactly the extension concepts its
/// underlying transport does — no more (which would produce a link error at
/// best) and no fewer (which would silently disable leadership transfer on a
/// transport that supports it).
template<typename Client> class transport_client_handle {
public:
    using client_type = Client;

    explicit transport_client_handle(Client& client) noexcept : _client(&client) {}

    // ── the three every transport has ────────────────────────────────────────

    auto send_request_vote(std::uint64_t target, const kythira::request_vote_request<>& request,
                           std::chrono::milliseconds timeout) {
        return _client->send_request_vote(target, request, timeout);
    }

    auto send_append_entries(std::uint64_t target, const kythira::append_entries_request<>& request,
                             std::chrono::milliseconds timeout) {
        return _client->send_append_entries(target, request, timeout);
    }

    auto send_install_snapshot(std::uint64_t target,
                               const kythira::install_snapshot_request<>& request,
                               std::chrono::milliseconds timeout) {
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
    static auto capabilities() -> transport_capabilities { return transport_capabilities{}; }

    explicit beast_http_transport(const std::vector<std::uint64_t>& nodes)
        : _work(boost::asio::make_work_guard(_ioc)) {
        const auto threads = std::max(2U, std::thread::hardware_concurrency() / 2);
        for (unsigned i = 0; i < threads; ++i) {
            _io_threads.emplace_back([this] { _ioc.run(); });
        }
        for (auto id : nodes) {
            _urls.emplace(id, loopback_url(reserve_port()));
        }
        for (auto id : nodes) {
            _servers.emplace(id, std::make_unique<server_type>(_ioc, "127.0.0.1", port_of(id),
                                                               kythira::boost_beast_server_config{},
                                                               noop_metrics{}));
            _clients.emplace(
                id, std::make_unique<client_type>(_ioc, _urls, kythira::boost_beast_client_config{},
                                                  noop_metrics{}));
        }
    }

    ~beast_http_transport() { shutdown(); }

    beast_http_transport(const beast_http_transport&) = delete;
    auto operator=(const beast_http_transport&) -> beast_http_transport& = delete;

    [[nodiscard]] auto server(std::uint64_t id) -> server_type& { return *_servers.at(id); }
    [[nodiscard]] auto client(std::uint64_t id) -> client_type& { return *_clients.at(id); }

    /// @brief Stop the io threads. Called after every host is stopped, never
    /// before: a running host still has RPCs on these threads.
    auto shutdown() -> void {
        if (_stopped.exchange(true)) {
            return;
        }
        _clients.clear();
        _servers.clear();
        _work.reset();
        _ioc.stop();
        for (auto& t : _io_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        _io_threads.clear();
    }

private:
    [[nodiscard]] auto port_of(std::uint64_t id) const -> std::uint16_t {
        const auto& url = _urls.at(id);
        return static_cast<std::uint16_t>(std::stoi(url.substr(url.rfind(':') + 1)));
    }

    boost::asio::io_context _ioc;
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

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
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
        : _options(options), _transport(node_ids(options._nodes)) {
        const auto voters = node_ids(_options._nodes);
        const auto ranges = kv_shard_ranges(_options._groups, _options._key_count);

        for (std::uint64_t id = 1; id <= _options._nodes; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
            for (std::size_t g = 0; g < _options._groups; ++g) {
                descriptor_type d;
                d._group_id = static_cast<group_id_type>(g + 1);
                d._range = ranges[g];
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
        _hosts.clear();
        _transport.shutdown();
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
                transport_client_handle<typename Transport::client_type>{_transport.client(id)},
            .network_server =
                transport_server_handle<typename Transport::server_type>{_transport.server(id)},
            .store_factory =
                [](const group_id_type&) { return typename types::persistence_engine_type{}; },
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
        return cfg;
    }

    auto drive(std::size_t index) -> void {
        while (_running.load(std::memory_order_relaxed)) {
            _hosts[index]->tick();
            std::this_thread::sleep_for(_options._tick_interval);
        }
    }

    kv_cluster_options _options;
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
    key_distribution _distribution{key_distribution::uniform};
    double _zipf_theta{0.99};
    std::chrono::milliseconds _op_timeout{3000};
    std::string _scenario{"put"};
};

/// @brief Run `_operations` PUTs spread over `_in_flight` client threads.
///
/// Closed-loop: each worker holds exactly one operation outstanding, so
/// `_in_flight` is literally the concurrency. Open-loop — a fixed offered rate
/// with coordinated-omission correction — is the other mode
/// `.kiro/specs/multi-raft-performance/` Requirement 4 calls for, and belongs
/// with the report binary rather than here, where a CI-registered test would
/// have to pick a rate the runner can sustain.
template<typename Transport>
auto run_put_workload(kv_cluster<Transport>& cluster, const workload_options& workload)
    -> benchmark_result {
    std::vector<latency_sample_set> per_worker(workload._in_flight);
    std::vector<operation_tally> tallies(workload._in_flight);
    const auto per_worker_ops = std::max<std::size_t>(
        1, workload._operations / std::max<std::size_t>(1, workload._in_flight));

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
                    const auto n = sampler.next();
                    const auto key = kv_key(n);
                    const auto command = kv_put(key, kv_value(n + i, workload._value_bytes));
                    if (auto latency =
                            cluster.run_command(key, command, workload._op_timeout, tallies[w])) {
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

    latency_sample_set all;
    operation_tally tally;
    for (std::size_t w = 0; w < workload._in_flight; ++w) {
        all.merge(per_worker[w]);
        tally.merge(tallies[w]);
    }
    all.sort();

    typename Transport::transport_bundle::serializer_type serializer{};
    typename kv_cluster<Transport>::types::serializer_type node_serializer{};

    benchmark_result result;
    result._transport = std::string{Transport::name()};
    // The media type is the serializer's own name for itself, so a row can
    // never disagree with what actually went on the wire.
    result._serializer = serializer.media_type();
    // Same principle applied to the serializer that is *not* being swept: read
    // it off the node-internal bundle the cluster was actually built from, so a
    // row that silently changed it would say so rather than be assumed not to.
    result._node_serializer = node_serializer.media_type();
    result._scenario = workload._scenario;
    result._nodes = cluster.options()._nodes;
    result._groups = cluster.options()._groups;
    result._value_bytes = workload._value_bytes;
    result._in_flight = workload._in_flight;
    result._duration = elapsed;
    result._tally = tally;
    result._p50 = all.p50();
    result._p95 = all.p95();
    result._p99 = all.p99();
    const auto seconds = std::chrono::duration<double>(elapsed).count();
    result._ops_per_second = seconds > 0.0 ? static_cast<double>(tally._completed) / seconds : 0.0;
    return result;
}

}  // namespace kythira::testing
