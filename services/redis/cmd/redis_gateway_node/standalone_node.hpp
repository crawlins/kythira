// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file standalone_node.hpp
/// @brief redis_gateway_node as a one-module node
///        (.kiro/specs/service-colocation/ task 7, Requirement 3.1): its own
///        Raft transport, Boost.Beast with the wire KYTHIRA_WIRE_SERIALIZER
///        names, and the Redis module (redis_module.hpp) in a stand-alone
///        group space, so its group ids, variables, ports and data
///        directory are exactly the daemon's before.
///
/// The transport is Boost.Beast, and only Beast: the gateway already links
/// Boost.Asio and OpenSSL for its own listeners, so Beast adds no dependency,
/// and cpp-httplib's Nagle stall (see doc/multi-raft-performance) is the wrong
/// default for a cache whose entries are megabytes.

#include "config.hpp"
#include "redis_module.hpp"
#include "redis_serving_api.hpp"
#include "stop_signal.hpp"

#include <kythira/redis/redis_gateway_impl.hpp>

#include <raft/beast_http_transport_impl.hpp>
#include <raft/bounded_resolver.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_transport_handles.hpp>
#include <raft/node_core.hpp>
#include <raft/node_host.hpp>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::redis_node {

inline auto raft_server_config(const node_options& opt) -> kythira::boost_beast_server_config {
    kythira::boost_beast_server_config c;
    c.max_request_body_size = raft_request_body_limit(opt);
    return c;
}

template<typename Serializer> struct beast_stack {
    using bundle = kythira::host_transport_types<Serializer>;
    using client_type = kythira::boost_beast_client<bundle, kythira::host_node_id>;
    using server_type = kythira::boost_beast_server<bundle, kythira::host_node_id>;

    explicit beast_stack(const node_options& opt)
        : _work(boost::asio::make_work_guard(*_ioc)),
          _server(*_ioc, opt._bind_address, opt._raft_port, raft_server_config(opt),
                  kythira::noop_metrics{}),
          _client(std::in_place, *_ioc,
                  std::unordered_map<kythira::host_node_id, std::string>{opt._peers.begin(),
                                                                         opt._peers.end()},
                  kythira::boost_beast_client_config{}, kythira::noop_metrics{}, _ioc) {
        // A peer that speaks to this node has told it where it is: how a
        // gateway finds the shard leader to forward to while its name does
        // not resolve, e.g. right after the gateways restart at new
        // addresses under rootless Podman (see bounded_resolver.hpp).
        // The Raft client learns the peer's Raft address; the forward link
        // dials the peer's gateway listener, and the cache's HTTP front end
        // its HTTP listener, on other ports, so those addresses are learned
        // here from the same sighting.
        _server.set_peer_seen_handler([raft = _client->peer_seen_handler(),
                                       gateways = opt._peer_gateways, http = opt._cache._peer_http](
                                          const kythira::host_node_id& id,
                                          const sockaddr_storage& from, socklen_t len) {
            raft(id, from, len);
            for (const auto* specs : {&gateways, &http}) {
                auto it = specs->find(id);
                if (it == specs->end()) {
                    continue;
                }
                try {
                    auto [host, port] = kythira::redis_gateway_detail::parse_listen(it->second);
                    if (auto addr =
                            kythira::net_resolve::address_from(from, len, port, SOCK_STREAM)) {
                        kythira::net_resolve::learn(host, std::to_string(port), SOCK_STREAM, *addr);
                    }
                } catch (const std::exception&) {
                    // A malformed peer spec is reported when it is
                    // dialled; a sighting is not the place to fail.
                }
            }
        });
        const auto threads = std::max(2U, std::thread::hardware_concurrency() / 2);
        for (unsigned i = 0; i < threads; ++i) {
            _io_threads.emplace_back([this] { _ioc->run(); });
        }
    }
    ~beast_stack() { shutdown(); }
    beast_stack(const beast_stack&) = delete;
    auto operator=(const beast_stack&) -> beast_stack& = delete;
    beast_stack(beast_stack&&) = delete;
    auto operator=(beast_stack&&) -> beast_stack& = delete;

    auto shutdown() -> void {
        if (_stopped.exchange(true)) {
            return;
        }
        // The client goes first, while the threads still run: its
        // destructor closes its connections and waits for each RPC still
        // in flight to reach its completion handler, which only these
        // threads run. Destroyed after them, a node stopping with an RPC
        // to a silent peer outstanding waited forever.
        _client.reset();
        _server.stop();
        _work.reset();
        _ioc->stop();
        for (auto& t : _io_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        _io_threads.clear();
    }

    std::shared_ptr<boost::asio::io_context> _ioc{std::make_shared<boost::asio::io_context>()};
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work;
    server_type _server;
    /// Reset by `shutdown`, before the threads stop.
    std::optional<client_type> _client;
    std::vector<std::thread> _io_threads;
    std::atomic<bool> _stopped{false};
};

template<typename Serializer>
using standalone_host =
    service_node::node_host<typename beast_stack<Serializer>::client_type,
                            typename beast_stack<Serializer>::server_type, kythira::host_node_id>;

/// The daemon: its stack, a node with the Redis module alone, and the
/// signal wait. Exit status as before: 0 on a stop, 2 for a setting the
/// module refused, 1 for anything else.
template<typename Serializer>
auto run_standalone(const node_options& opt,
                    typename redis_module<standalone_host<Serializer>>::factories factories)
    -> int {
    using host_type = standalone_host<Serializer>;
    kythira::console_logger logger{opt._log_level};
    beast_stack<Serializer> stack{opt};
    service_node::node_config node;
    node.node_id = kythira::host_node_id_text(opt._node_id);
    node.data_dir = opt._cache._data_dir;
    node.raft_bind = opt._bind_address;
    node.raft_port = opt._raft_port;
    host_type host(node, opt._node_id, *stack._client, stack._server);

    service_node::module_plan<host_type> plan;
    plan.modules.push_back(std::make_unique<redis_module<host_type>>(true, std::move(factories)));
    plan.spaces.push_back(
        service_node::group_space::standalone(service_node::module_numbers::redis));
    service_node::node_runner<host_type> runner(
        std::move(plan), node, [&logger](bool error, std::string_view line) {
            logger.log(error ? kythira::log_level::error : kythira::log_level::info,
                       std::string(line), {});
        });

    int status = 0;
    try {
        runner.start(host, process_env());
        wait_for_stop();
    } catch (const std::invalid_argument& e) {
        // A setting the module refused; main() read the same ones first.
        std::cerr << "redis_gateway_node: " << e.what() << '\n';
        status = 2;
    } catch (const std::exception& e) {
        std::cerr << "redis_gateway_node: " << e.what() << '\n';
        status = 1;
    }
    // The module stops its listeners, then its host, before the transport
    // goes.
    runner.stop([&stack] { stack.shutdown(); });
    return status;
}

}  // namespace kythira::redis_node
