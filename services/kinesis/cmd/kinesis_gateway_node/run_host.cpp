// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_host.cpp
/// @brief The daemon's Raft transport and serve loop, alone in its
///        translation unit (one multi_raft stack per TU: cmd/multi_raft_node's
///        lesson, as in redis_gateway_node).
///
/// Raft runs over Boost.Beast with the CBOR wire, as it always has. The
/// daemon is a node with its one module fixed
/// (.kiro/specs/service-colocation/ task 5, Requirement 3.1): the Kinesis
/// module (kinesis_module.hpp) in a stand-alone group space, so its group
/// ids, variables and data directory are exactly the daemon's before.

#include "config.hpp"
#include "kinesis_module.hpp"
#include "stop_signal.hpp"

#include <raft/beast_http_transport_impl.hpp>
#include <raft/cbor_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/node_core.hpp>
#include <raft/node_host.hpp>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::kinesis_node {

namespace {

using serializer_type = kythira::cbor_rpc_serializer<std::vector<std::byte>>;

struct beast_stack {
    using bundle = kythira::host_transport_types<serializer_type>;
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
        // A peer that speaks to this node has told the Raft client where it
        // is, for names that do not resolve yet (bounded_resolver.hpp).
        _server.set_peer_seen_handler(_client->peer_seen_handler());
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

    /// An AppendEntries carries at most `_max_append_bytes` of commands
    /// (set to the largest write below) or one larger entry, so 4/3 of it
    /// (base64 in the worst wire) plus framing always fits.
    static auto raft_server_config(const node_options& opt) -> kythira::boost_beast_server_config {
        kythira::boost_beast_server_config c;
        constexpr std::size_t framing = std::size_t{1024} * 1024;
        c.max_request_body_size = std::max<std::size_t>(std::size_t{10} * 1024 * 1024,
                                                        opt._max_body_bytes / 3 * 4 + framing);
        return c;
    }

    void shutdown() {
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

using standalone_host = service_node::node_host<beast_stack::client_type, beast_stack::server_type,
                                                kythira::host_node_id>;

}  // namespace

auto run(const node_options& opt) -> int {
    kythira::console_logger logger{opt._log_level};
    beast_stack stack{opt};
    service_node::node_config node;
    node.node_id = kythira::host_node_id_text(opt._node_id);
    node.data_dir = opt._data_dir;
    node.raft_bind = opt._bind_address;
    node.raft_port = opt._raft_port;
    standalone_host host(node, opt._node_id, *stack._client, stack._server);

    service_node::module_plan<standalone_host> plan;
    plan.modules.push_back(std::make_unique<kinesis_module<standalone_host>>(true));
    plan.spaces.push_back(
        service_node::group_space::standalone(service_node::module_numbers::kinesis));
    service_node::node_runner<standalone_host> runner(
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
        std::cerr << "kinesis_gateway_node: " << e.what() << '\n';
        status = 2;
    } catch (const std::exception& e) {
        std::cerr << "kinesis_gateway_node: " << e.what() << '\n';
        status = 1;
    }
    // The module closes its API and in-process interface, then stops its
    // host, before the transport goes.
    runner.stop([&stack] { stack.shutdown(); });
    return status;
}

}  // namespace kythira::kinesis_node
