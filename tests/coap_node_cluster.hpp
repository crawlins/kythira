// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file coap_node_cluster.hpp
/// @brief A cluster of `node<Types>` over a real CoAP transport
///        (.kiro/specs/coap-transport-multi-raft/ task 5 onward).
///
/// Before this header no test in the repository instantiated a Raft node over
/// any CoAP backend: the CoAP suite exercised the transport standalone. This
/// stands three (or more) nodes up in one process, each with its own started
/// `coap_server` on an ephemeral loopback port and its own `coap_client`
/// addressing every peer by numeric endpoint, and drives them the way the
/// simulator-based cluster tests do — by ticking each node's election and
/// heartbeat checks from the test thread.
///
/// ### Why handles
///
/// `node<Types>` takes its network client and server **by value** and moves
/// them into place; `coap_client` and `coap_server` hold a mutex, an io thread
/// and a libcoap context, and are not movable. Each node is therefore handed a
/// pointer-thin handle that forwards to a transport this fixture owns and
/// outlives. The optional RPCs forward behind a `requires` clause on the
/// underlying transport, so a handle satisfies exactly the extension concepts
/// its transport does and `node<Types>`'s own `if constexpr` detection sees the
/// transport's real capability set.
///
/// ### Ports
///
/// A client needs every peer's endpoint at construction, which is before any
/// node has started its server, so each server's port is reserved up front:
/// bind a UDP socket to port 0, read the port back, release it. That leaves a
/// small window in which another process could take the port, which any port
/// chosen in advance has; it never collides with one already in use, and
/// nothing here contends for 5683 or any fixed port (Requirement 8.3). The
/// alternative — starting each server early to read `bound_port()` — would
/// make every `node::start()` re-register its handlers on a running server.

#include "test_timeout_scale.hpp"

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/network.hpp>
#include <raft/persistence.hpp>
#include <raft/raft.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/test_state_machine.hpp>
#include <raft/types.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kythira::testing {

/// The Types bundle every CoAP transport in these tests is built over.
/// `future_template` is `future_default` because `node<Types>` assigns the
/// client's future straight into one.
template<typename Serializer> struct coap_node_transport_types {
    using serializer_type = Serializer;
    using serializer_registry_type = kythira::single_serializer_registry<Serializer>;
    using rpc_serializer_type = Serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = int;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

/// A movable `network_client` view of a `coap_client` the fixture owns.
template<typename Client> class coap_client_handle {
public:
    explicit coap_client_handle(Client& client) noexcept : _client(&client) {}

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

    auto send_timeout_now(std::uint64_t target, const kythira::timeout_now_request<>& request,
                          std::chrono::milliseconds timeout)
    requires kythira::network_client_with_timeout_now<Client>
    {
        return _client->send_timeout_now(target, request, timeout);
    }

private:
    Client* _client;
};

/// A movable `network_server` view of a `coap_server` the fixture owns. Every
/// call forwards, so stopping a node really takes its endpoint off the network.
template<typename Server> class coap_server_handle {
public:
    explicit coap_server_handle(Server& server) noexcept : _server(&server) {}

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

    auto register_timeout_now_handler(
        std::function<kythira::timeout_now_response<>(const kythira::timeout_now_request<>&)>
            handler) -> void
    requires kythira::network_server_with_timeout_now<Server>
    {
        _server->register_timeout_now_handler(std::move(handler));
    }

    auto start() -> void { _server->start(); }
    auto stop() -> void {
        if (_server->is_running()) {
            _server->stop();
        }
    }
    [[nodiscard]] auto is_running() const -> bool { return _server->is_running(); }

private:
    Server* _server;
};

/// The `node<Types>` bundle over CoAP handles.
template<typename Serializer> struct coap_node_raft_types {
    using transport_types = coap_node_transport_types<Serializer>;
    using coap_client_type = kythira::coap_client<transport_types>;
    using coap_server_type = kythira::coap_server<transport_types>;

    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = Serializer;

    using network_client_type = coap_client_handle<coap_client_type>;
    using network_server_type = coap_server_handle<coap_server_type>;

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
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type>;
    using install_snapshot_response_type = kythira::install_snapshot_response<term_id_type>;
};

/// A UDP port on loopback that was free a moment ago. See "Ports" above.
[[nodiscard]] inline auto reserve_udp_port() -> std::uint16_t {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        throw std::runtime_error("reserve_udp_port: socket() failed");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        throw std::runtime_error("reserve_udp_port: bind()/getsockname() failed");
    }
    ::close(fd);
    return ntohs(addr.sin_port);
}

/// Raft timings for a loopback CoAP cluster, scaled with every other budget
/// by KYTHIRA_TEST_TIMEOUT_SCALE so a 4x-slower Coverage build keeps the same
/// ratio between heartbeat, election timeout and RPC timeout as Release does.
/// Scaling the test's waits but not these is what lets a slow build depose a
/// leader that is merely mid-heartbeat.
inline auto coap_cluster_raft_config() -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = kythira::testing::scaled_deadline(300);
    cfg._election_timeout_max = kythira::testing::scaled_deadline(600);
    cfg._heartbeat_interval = kythira::testing::scaled_deadline(60);
    cfg._rpc_timeout = kythira::testing::scaled_deadline(250);
    return cfg;
}

/// @brief N nodes over CoAP, driven from the test thread.
template<typename Serializer = kythira::json_rpc_serializer<std::vector<std::byte>>>
class coap_node_cluster {
public:
    using types = coap_node_raft_types<Serializer>;
    using node_type = kythira::node<types>;
    using client_type = typename types::coap_client_type;
    using server_type = typename types::coap_server_type;

    explicit coap_node_cluster(
        std::size_t size, kythira::coap_client_config client_config = {},
        kythira::raft_configuration raft_config = coap_cluster_raft_config()) {
        for (std::size_t i = 0; i < size; ++i) {
            _ids.push_back(static_cast<std::uint64_t>(i + 1));
        }
        for (auto id : _ids) {
            const auto port = reserve_udp_port();
            _endpoints.emplace(id, "coap://127.0.0.1:" + std::to_string(port));
            _servers.emplace(
                id, std::make_unique<server_type>("127.0.0.1", port, kythira::coap_server_config{},
                                                  kythira::noop_metrics{}));
        }
        for (auto id : _ids) {
            std::unordered_map<std::uint64_t, std::string> peers;
            for (auto peer : _ids) {
                if (peer != id) {
                    peers.emplace(peer, _endpoints.at(peer));
                }
            }
            _clients.emplace(id, std::make_unique<client_type>(std::move(peers), client_config,
                                                               kythira::noop_metrics{}));
        }
        for (auto id : _ids) {
            auto n = std::make_unique<node_type>(kythira::node_config<types>{
                .node_id = id,
                .network_client = typename types::network_client_type{*_clients.at(id)},
                .network_server = typename types::network_server_type{*_servers.at(id)},
                .persistence = typename types::persistence_engine_type{},
                .logger = typename types::logger_type{kythira::log_level::error},
                .metrics = typename types::metrics_type{},
                .membership = typename types::membership_manager_type{},
                .config = raft_config,
            });
            n->set_cluster_configuration(_ids);
            _nodes.emplace(id, std::move(n));
        }
    }

    ~coap_node_cluster() { stop_all(); }

    coap_node_cluster(const coap_node_cluster&) = delete;
    auto operator=(const coap_node_cluster&) -> coap_node_cluster& = delete;

    auto start_all() -> void {
        for (auto id : _ids) {
            _nodes.at(id)->start();
            _live.push_back(id);
        }
    }

    /// Stops every node still running. Clients outlive their nodes (members are
    /// destroyed in reverse order), so no in-flight continuation can reach a
    /// destroyed client.
    auto stop_all() -> void {
        for (auto id : std::vector<std::uint64_t>(_live)) {
            kill(id);
        }
    }

    /// Takes one node off the cluster the way a crash does: its node stops and
    /// its server stops answering. Its peers learn of it only by their RPCs to
    /// it failing.
    auto kill(std::uint64_t id) -> void {
        const auto it = std::find(_live.begin(), _live.end(), id);
        if (it == _live.end()) {
            return;
        }
        _live.erase(it);
        _nodes.at(id)->stop();
        if (_servers.at(id)->is_running()) {
            _servers.at(id)->stop();
        }
    }

    auto tick() -> void {
        for (auto id : _live) {
            _nodes.at(id)->check_election_timeout();
            _nodes.at(id)->check_heartbeat_timeout();
        }
    }

    template<typename Predicate>
    auto tick_until(Predicate done, std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            tick();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return done();
    }

    /// The live leader, if exactly one live node believes it leads.
    [[nodiscard]] auto leader() const -> std::optional<std::uint64_t> {
        std::optional<std::uint64_t> found;
        for (auto id : _live) {
            if (_nodes.at(id)->is_leader()) {
                if (found) {
                    return std::nullopt;
                }
                found = id;
            }
        }
        return found;
    }

    auto require_leader(std::chrono::milliseconds budget) -> std::uint64_t {
        std::optional<std::uint64_t> id;
        tick_until([&] { return (id = leader()).has_value(); }, budget);
        if (!id) {
            throw std::runtime_error("no single leader emerged over CoAP within the budget");
        }
        return *id;
    }

    /// Resolves a future while the cluster keeps ticking underneath it: the
    /// tick is the only clock `node<Types>` has, so blocking on `.get()` with
    /// nobody ticking waits for a deadline that can never fire.
    template<typename Future>
    auto settle(Future&& f, std::chrono::milliseconds budget) -> std::exception_ptr {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (!f.wait(std::chrono::milliseconds{2}) &&
               std::chrono::steady_clock::now() < deadline) {
            tick();
        }
        if (!f.wait(std::chrono::milliseconds{100})) {
            return std::make_exception_ptr(
                std::runtime_error("future never resolved while the cluster was ticking"));
        }
        try {
            std::ignore = std::forward<Future>(f).get();
            return nullptr;
        } catch (...) {
            return std::current_exception();
        }
    }

    [[nodiscard]] auto node(std::uint64_t id) -> node_type& { return *_nodes.at(id); }
    [[nodiscard]] auto client(std::uint64_t id) -> client_type& { return *_clients.at(id); }
    [[nodiscard]] auto ids() const -> const std::vector<std::uint64_t>& { return _ids; }
    [[nodiscard]] auto live() const -> const std::vector<std::uint64_t>& { return _live; }

private:
    std::vector<std::uint64_t> _ids;
    std::vector<std::uint64_t> _live;
    std::unordered_map<std::uint64_t, std::string> _endpoints;
    // Declaration order is destruction order reversed: nodes go first, then
    // clients, then servers.
    std::map<std::uint64_t, std::unique_ptr<server_type>> _servers;
    std::map<std::uint64_t, std::unique_ptr<client_type>> _clients;
    std::map<std::uint64_t, std::unique_ptr<node_type>> _nodes;
};

}  // namespace kythira::testing
