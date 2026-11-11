// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// dynamodb_gateway_node on a Raft cluster: a multi_raft host on core's Beast
// transport, a raft_backend over it, and the gateway serving SDK requests and
// peers' forwarded ops (.kiro/specs/dynamodb-compatible-api/ task 7).
//
// Every node creates the same two groups at start (the catalog group and one
// item group, raft_backend::bootstrap_descriptors) with every peer as a
// voter, so a cluster forms as soon as a majority is up. Automatic splitting
// is off until the split policy lands; the groups' state lives in the
// node's engine (memory, or one RocksDB database under the data directory,
// with the Raft logs beside it).

#include <kythira/dynamodb/forward_link.hpp>
#include <kythira/dynamodb/gateway.hpp>
#include <kythira/dynamodb/group_state_machine.hpp>
#include <kythira/dynamodb/raft_backend.hpp>

#include <raft/beast_http_transport_impl.hpp>
#include <raft/binary_file_persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/host_node_id.hpp>
#include <raft/json_serializer.hpp>
#include <raft/memory_kv_engine.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/multi_raft_transport_handles.hpp>
#include <raft/persistence.hpp>
#include <raft/record_segment_store.hpp>

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dynamodb_gateway {

namespace ddb = kythira::dynamodb;
using node_id = kythira::host_node_id;
using serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;
using bundle = kythira::host_transport_types<serializer>;
using client_type = kythira::boost_beast_client<bundle, node_id>;
using server_type = kythira::boost_beast_server<bundle, node_id>;

/// The node's state in memory: nothing survives a restart.
struct memory_storage {
    using engine_type = kythira::memory_kv_engine;
    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id, std::uint64_t, std::uint64_t>;
    static constexpr std::string_view k_name = "memory";
};

template<typename Storage> struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = node_id;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = serializer;

    using network_client_type = kythira::transport_client_handle<client_type, node_id_type>;
    using network_server_type = kythira::transport_server_handle<server_type, node_id_type>;

    using persistence_engine_type = typename Storage::persistence_engine_type;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = ddb::dynamo_txn_group_state_machine<typename Storage::engine_type>;

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

/// A peer id from the environment's text.
inline auto peer_id(const std::string& text) -> node_id {
    auto id = kythira::parse_host_peer_node_id(text);
    if (!id) {
        throw std::invalid_argument("KYTHIRA_DYNAMODB_PEERS: bad node id '" + text +
                                    "': " + id.error());
    }
    return *id;
}

/// The Raft transport: one io_context for the server and the client.
struct beast_stack {
    beast_stack(const ddb::cluster_config& c, const std::unordered_map<node_id, std::string>& urls)
        : _work(boost::asio::make_work_guard(*_ioc)),
          _server(*_ioc, c.raft_bind, c.raft_port, kythira::boost_beast_server_config{},
                  kythira::noop_metrics{}),
          _client(*_ioc, urls, kythira::boost_beast_client_config{}, kythira::noop_metrics{},
                  _ioc) {
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

    /// Only after the host has stopped: a running host has RPCs on these
    /// threads.
    auto shutdown() -> void {
        if (_stopped.exchange(true)) {
            return;
        }
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
    client_type _client;
    std::vector<std::thread> _io_threads;
    std::atomic<bool> _stopped{false};
};

/// Builds and runs the node until `wait()` returns, then shuts it down in
/// the order that leaves no thread or callback behind.
template<typename Storage>
auto run_cluster(
    const ddb::gateway_config& cfg, std::shared_ptr<typename Storage::engine_type> engine,
    const std::function<typename Storage::persistence_engine_type(std::uint64_t)>& store_factory,
    const std::function<void(bool, std::string_view)>& log, const std::function<void()>& wait)
    -> void {
    using types = host_types<Storage>;
    using host_type = kythira::multi_raft<types, std::string, std::uint64_t>;
    using config_type = kythira::multi_raft_config<types, std::string, std::uint64_t>;
    using backend_type = ddb::raft_backend<host_type>;
    using traits = kythira::node_id_traits<node_id>;

    const auto& c = cfg.cluster;
    auto self = kythira::parse_host_node_id(c.node_id);
    if (!self) {
        throw std::invalid_argument("KYTHIRA_DYNAMODB_NODE_ID: " + self.error());
    }
    std::unordered_map<node_id, std::string> urls;
    std::vector<node_id> voters;
    for (const auto& [text, url] : c.peers) {
        const auto id = text == c.node_id ? *self : peer_id(text);
        urls.emplace(id, url);
        voters.push_back(id);
    }
    // Keyed by each id's canonical text, which is what the backend asks for.
    std::map<std::string, ddb::forward_peer> gateways;
    for (const auto& [text, peer] : c.peer_gateways) {
        gateways.emplace(traits::to_text(peer_id(text)), peer);
    }
    const ddb::forward_client forward(
        ddb::forward_client_config{.secret = c.forward_secret,
                                   .tls = !cfg.http.insecure_plaintext,
                                   .ca_path = c.forward_ca_path,
                                   .timeout = std::chrono::milliseconds{20000}},
        std::move(gateways));

    // The change log's segments (task 15) live beside the engine. The
    // memory engine keeps none: its items do not outlive the process
    // either. The store outlives the host, whose groups append to it.
    std::unique_ptr<kythira::record_segment_store> change_log;
    if (cfg.engine != "memory" && !cfg.data_dir.empty()) {
        kythira::record_segment_store_options o;
        o._data_dir = std::filesystem::path(cfg.data_dir) / "change-log";
        change_log = std::make_unique<kythira::record_segment_store>(o);
    }
    // A group installing a snapshot fetches the active change log segment
    // it names from the group's peers, through the backend once it runs.
    std::atomic<backend_type*> fetch_via{nullptr};

    beast_stack stack(c, urls);
    config_type hc{
        .node_id = *self,
        .network_client = typename types::network_client_type{stack._client},
        .network_server = typename types::network_server_type{stack._server},
        .store_factory =
            [&store_factory](const std::uint64_t& group) { return store_factory(group); },
        .on_group_destroyed = {},
    };
    hc.state_engine = std::move(engine);
    hc.logger = kythira::console_logger{cfg.log_level};
    hc.logger_factory = [level = cfg.log_level](const std::uint64_t&) {
        return kythira::console_logger{level};
    };
    hc.state_machine_factory = [&change_log, &fetch_via](const std::uint64_t& group) {
        ddb::dynamo_group_state_machine<typename Storage::engine_type> inner;
        inner.set_change_log(
            change_log.get(), [&fetch_via, group](const kythira::record_segment_descriptor& d) {
                auto* b = fetch_via.load(std::memory_order_acquire);
                if (b == nullptr) {
                    throw kythira::record_segment_error(
                        "dynamodb_gateway_node: cannot fetch a change log segment before serving");
                }
                b->fetch_change_segment(group, d);
            });
        return typename types::state_machine_type(std::move(inner));
    };
    hc.automatic_split_merge_enabled = false;
    hc.split_merge_interval = std::chrono::hours{1};
    auto host = std::make_unique<host_type>(std::move(hc));
    for (const auto& d : backend_type::bootstrap_descriptors(voters)) {
        host->create_group(d);
    }
    host->start();

    std::atomic<bool> driving{true};
    std::thread driver([&] {
        while (driving.load(std::memory_order_acquire)) {
            host->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    });
    auto stop_driver = [&] {
        driving.store(false, std::memory_order_release);
        if (driver.joinable()) {
            driver.join();
        }
    };

    try {
        // Each voter enforces its share of every capacity limit: a client
        // spreading requests over the nodes gets the table's whole limit.
        ddb::raft_backend_options options;
        options.capacity_share = 1.0 / static_cast<double>(voters.size());
        backend_type backend(
            *host,
            [&forward](const node_id& to, const std::string& op) {
                return forward.send(traits::to_text(to), op);
            },
            options);
        backend.set_change_log(change_log.get());
        fetch_via.store(&backend, std::memory_order_release);
        // No fetch through the backend once it is gone.
        const std::unique_ptr<std::atomic<backend_type*>, void (*)(std::atomic<backend_type*>*)>
            clear_fetch(&fetch_via, [](std::atomic<backend_type*>* slot) {
                slot->store(nullptr, std::memory_order_release);
            });
        ddb::gateway<backend_type> gw(cfg, backend, log);
        gw.start();
        backend.start();
        log(false, "serving " + std::string(cfg.http.insecure_plaintext ? "http" : "https") +
                       " on " + cfg.http.bind_address + ":" + std::to_string(gw.port()) +
                       ", raft on " + c.raft_bind + ":" + std::to_string(c.raft_port) + ", node " +
                       c.node_id + " of " + std::to_string(voters.size()) + ", engine " +
                       std::string(Storage::k_name));
        wait();
        gw.stop();
        backend.stop();
    } catch (...) {
        stop_driver();
        host->stop();
        host.reset();
        stack.shutdown();
        throw;
    }
    stop_driver();
    host->stop();
    host.reset();
    stack.shutdown();
}

}  // namespace dynamodb_gateway
