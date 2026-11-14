// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kythira_node_host.hpp
/// @brief `kythira_node`'s Raft transport and the host type its modules
///        register against (.kiro/specs/service-colocation/ task 3).
///
/// Raft runs over Boost.Beast with the CBOR wire, on the node's one
/// listener (`KYTHIRA_NODE_RAFT_BIND`/`_RAFT_PORT`) and its one client.
/// With a certificate (`KYTHIRA_NODE_TLS_CERT`/`_KEY`, and `_CA` to check
/// peers, required with them) both ends speak mutual TLS; `KYTHIRA_NODE_INSECURE_PLAINTEXT=1`
/// turns TLS off for tests.
///
/// A module's library registers itself for `kythira_node_host`:
///
/// @code
/// const kythira::service_node::module_registrar<kythira::service_node::kythira_node_host>
///     k_registrar("dynamodb", [] { return std::make_unique<dynamodb_module>(); });
/// @endcode

#include <raft/beast_http_transport_impl.hpp>
#include <raft/cbor_serializer.hpp>
#include <raft/host_node_id.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_transport_handles.hpp>
#include <raft/node_core.hpp>
#include <raft/node_host.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::service_node {

/// The node's Raft client and server and the threads that run them.
class kythira_node_stack {
public:
    using serializer_type = kythira::cbor_rpc_serializer<std::vector<std::byte>>;
    using bundle = kythira::host_transport_types<serializer_type>;
    using client_type = kythira::boost_beast_client<bundle, kythira::host_node_id>;
    using server_type = kythira::boost_beast_server<bundle, kythira::host_node_id>;

    /// The largest AppendEntries body the listener takes; a module whose
    /// entries are larger raises it with `KYTHIRA_NODE_RAFT_MAX_BODY_BYTES`
    /// in a later task.
    static constexpr std::size_t k_max_body_bytes = std::size_t{64} * 1024 * 1024;

    /// @throws std::invalid_argument for a peer id that does not parse, or
    ///         a TLS setting missing.
    explicit kythira_node_stack(const node_config& c)
        : _work(boost::asio::make_work_guard(*_ioc)),
          _server(*_ioc, c.raft_bind, c.raft_port, server_config(c), kythira::noop_metrics{}),
          _client(std::in_place, *_ioc, peer_urls(c), client_config(c), kythira::noop_metrics{},
                  _ioc) {
        // A peer that speaks to this node tells the client where it is, for
        // names that do not resolve yet (bounded_resolver.hpp).
        _server.set_peer_seen_handler(_client->peer_seen_handler());
        const auto threads = std::max(2U, std::thread::hardware_concurrency() / 2);
        for (unsigned i = 0; i < threads; ++i) {
            _threads.emplace_back([this] { _ioc->run(); });
        }
    }

    kythira_node_stack(const kythira_node_stack&) = delete;
    auto operator=(const kythira_node_stack&) -> kythira_node_stack& = delete;
    kythira_node_stack(kythira_node_stack&&) = delete;
    auto operator=(kythira_node_stack&&) -> kythira_node_stack& = delete;
    ~kythira_node_stack() { shutdown(); }

    [[nodiscard]] auto client() -> client_type& { return *_client; }
    [[nodiscard]] auto server() -> server_type& { return _server; }

    /// Stops the transport. Call after every module's host has stopped.
    void shutdown() {
        if (_stopped.exchange(true)) {
            return;
        }
        // The client goes first, while the threads still run: its
        // destructor waits for each RPC in flight to reach its handler,
        // which only these threads run.
        _client.reset();
        _server.stop();
        _work.reset();
        _ioc->stop();
        for (auto& t : _threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        _threads.clear();
    }

    /// `id`, parsed as the host's node id kind.
    /// @throws std::invalid_argument when it does not parse.
    [[nodiscard]] static auto parse_id(const std::string& id) -> kythira::host_node_id {
        auto r = kythira::parse_host_peer_node_id(id);
        if (!r) {
            throw std::invalid_argument("node id '" + id + "': " + r.error());
        }
        return r.value();
    }

private:
    static auto peer_urls(const node_config& c)
        -> std::unordered_map<kythira::host_node_id, std::string> {
        const std::string scheme = c.insecure_plaintext ? "http://" : "https://";
        std::unordered_map<kythira::host_node_id, std::string> out;
        for (const auto& p : c.peers) {
            const bool v6 = p.host.find(':') != std::string::npos;
            out.emplace(parse_id(p.id),
                        scheme + (v6 ? "[" + p.host + "]" : p.host) + ":" + std::to_string(p.port));
        }
        return out;
    }

    static auto server_config(const node_config& c) -> kythira::boost_beast_server_config {
        kythira::boost_beast_server_config s;
        s.max_request_body_size = k_max_body_bytes;
        if (!c.insecure_plaintext) {
            s.enable_ssl = true;
            s.ssl_cert_path = c.tls_cert;
            s.ssl_key_path = c.tls_key;
            s.ca_cert_path = c.tls_ca;
            // node_config_from_env requires the CA with a certificate, so
            // peers always check each other.
            s.require_client_cert = true;
        }
        return s;
    }

    static auto client_config(const node_config& c) -> kythira::boost_beast_client_config {
        kythira::boost_beast_client_config s;
        if (!c.insecure_plaintext) {
            s.enable_ssl_verification = true;
            s.ca_cert_path = c.tls_ca;
            s.client_cert_path = c.tls_cert;
            s.client_key_path = c.tls_key;
        }
        return s;
    }

    std::shared_ptr<boost::asio::io_context> _ioc{std::make_shared<boost::asio::io_context>()};
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work;
    server_type _server;
    /// Reset by `shutdown`, before the threads stop.
    std::optional<client_type> _client;
    std::vector<std::thread> _threads;
    std::atomic<bool> _stopped{false};
};

/// The host every `kythira_node` module registers against.
using kythira_node_host = node_host<kythira_node_stack::client_type,
                                    kythira_node_stack::server_type, kythira::host_node_id>;

}  // namespace kythira::service_node
