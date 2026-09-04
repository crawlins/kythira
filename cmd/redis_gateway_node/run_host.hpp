// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file run_host.hpp
/// @brief The daemon's `multi_raft` stack and serve loop, as a template
///        instantiated ONCE PER TRANSLATION UNIT (`run_cbor.cpp`,
///        `run_json.cpp`) — the lesson of cmd/multi_raft_node/host_stacks.hpp,
///        where four stacks in one file cost 11 GiB of compiler RSS.
///
/// The transport is Boost.Beast, and only Beast: the gateway already links
/// Boost.Asio and OpenSSL for its own listeners, so Beast adds no dependency,
/// and cpp-httplib's Nagle stall (see doc/multi-raft-performance) is the wrong
/// default for a cache whose entries are megabytes.

#include "config.hpp"
#include "stop_signal.hpp"

// The transport handle types are shared with the in-process harness rather
// than re-declared here; see cmd/multi_raft_node/main.cpp for why.
#include "multi_raft_transport_harness.hpp"

#include <raft/beast_http_transport_impl.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/redis_gateway_impl.hpp>
#include <raft/redis_kv_state_machine.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kythira::redis_node {

template<typename Serializer> struct beast_stack {
    using bundle = kythira::testing::harness_transport_types<Serializer>;
    using client_type = kythira::boost_beast_client<bundle>;
    using server_type = kythira::boost_beast_server<bundle>;

    explicit beast_stack(const node_options& opt)
        : _work(boost::asio::make_work_guard(*_ioc)),
          _server(*_ioc, opt._bind_address, opt._raft_port, kythira::boost_beast_server_config{},
                  kythira::noop_metrics{}),
          _client(
              *_ioc,
              std::unordered_map<std::uint64_t, std::string>{opt._peers.begin(), opt._peers.end()},
              kythira::boost_beast_client_config{}, kythira::noop_metrics{}, _ioc) {
        const auto threads = std::max(2U, std::thread::hardware_concurrency() / 2);
        for (unsigned i = 0; i < threads; ++i) {
            _io_threads.emplace_back([this] { _ioc->run(); });
        }
    }
    ~beast_stack() { shutdown(); }
    beast_stack(const beast_stack&) = delete;
    auto operator=(const beast_stack&) -> beast_stack& = delete;

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

template<typename Stack> struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type =
        kythira::testing::transport_client_handle<typename Stack::client_type>;
    using network_server_type =
        kythira::testing::transport_server_handle<typename Stack::server_type>;

    // A cache: in-memory persistence is the honest choice. A replica that
    // restarts catches up from its peers, and a cluster that loses every
    // replica at once has lost a cache, which is what caches are for.
    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kythira::redis_kv_state_machine<log_index_type>;

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

inline auto read_file(const std::string& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("redis_gateway_node: cannot read ACL file " + path);
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

/// Build, start, serve, shut down — in the order that does not terminate the
/// process (cmd/multi_raft_node/host_stacks.hpp's `run_host` ordering).
template<typename Serializer> auto run_host(const node_options& opt) -> int {
    using stack_type = beast_stack<Serializer>;
    using types = host_types<stack_type>;
    using host_type = kythira::multi_raft<types, std::string, std::uint64_t>;
    using config_type = kythira::multi_raft_config<types, std::string, std::uint64_t>;
    using descriptor_type = kythira::shard_descriptor<std::uint64_t, std::string, std::uint64_t>;
    using gateway_type =
        kythira::redis_gateway<host_type, kythira::console_logger, kythira::noop_metrics>;

    kythira::console_logger logger{opt._log_level};
    kythira::noop_metrics metrics;

    // The ACL first: a bad file must fail before a port is bound.
    kythira::redis_acl acl;
    if (!opt._acl_file.empty()) {
        acl.reload(read_file(opt._acl_file));
    }
    auto gateway_cfg = opt._gateway;
    if (!gateway_cfg._internal_secret.empty() && !acl.empty()) {
        // The internal user is what forwarded commands authenticate as. It
        // is created here from the secret rather than written into the ACL
        // file, so the file never holds an identity whose secret it cannot
        // verify was the one the peers were given.
        std::string line = "user " + gateway_cfg._internal_user + " " +
                           kythira::redis_acl::hash_secret(gateway_cfg._internal_secret) +
                           " read_write *\n";
        acl.reload(read_file(opt._acl_file) + line);
    }

    stack_type stack{opt};

    std::vector<std::uint64_t> voters;
    for (const auto& [id, url] : opt._peers) {
        voters.push_back(id);
    }

    config_type cfg{
        .node_id = opt._node_id,
        .network_client = typename types::network_client_type{stack._client},
        .network_server = typename types::network_server_type{stack._server},
        .store_factory =
            [](const std::uint64_t&) { return typename types::persistence_engine_type{}; },
    };
    cfg.logger = kythira::console_logger{opt._log_level};
    cfg.logger_factory = [level = opt._log_level](const std::uint64_t&) {
        return kythira::console_logger{level};
    };
    cfg.config._election_timeout_min = opt._election_timeout_min;
    cfg.config._election_timeout_max = opt._election_timeout_max;
    cfg.config._heartbeat_interval = opt._heartbeat_interval;
    cfg.config._rpc_timeout = opt._rpc_timeout;
    cfg.hibernation = kythira::hibernation_mode::off;
    cfg.executor_stripes = opt._executor_stripes;
    cfg.policy_interval = opt._policy_interval;
    cfg.partitioner = kythira::make_partitioner<std::string>(kythira::redis_kv_partitioner{});

    auto host = std::make_unique<host_type>(std::move(cfg));

    // Initial tiling: cuts sorted and deduplicated, one shard per range.
    auto cuts = opt._shard_cuts;
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    std::optional<std::string> start;
    std::uint64_t group = 1;
    for (std::size_t i = 0; i <= cuts.size(); ++i) {
        std::optional<std::string> end =
            i < cuts.size() ? std::optional<std::string>{cuts[i]} : std::nullopt;
        host->create_group(descriptor_type{
            ._group_id = group++,
            ._range = kythira::shard_range<std::string>{._start = start, ._end = end},
            ._epoch = kythira::shard_epoch{},
            ._voters = voters});
        start = end;
    }

    host->start();

    auto peer_gateways = opt._peer_gateways;
    gateway_type gateway(*host, acl, logger, metrics, gateway_cfg,
                         [peer_gateways](const std::uint64_t& id) -> std::optional<std::string> {
                             auto it = peer_gateways.find(id);
                             if (it == peer_gateways.end()) {
                                 return std::nullopt;
                             }
                             return it->second;
                         });
    gateway.start();

    // The driver thread: tick the host, and run the gateway's maintenance
    // whenever the policy phase ran (Requirement 6.3: no thread of its own).
    std::atomic<bool> driving{true};
    std::thread driver([&] {
        while (driving.load(std::memory_order_acquire)) {
            auto report = host->tick();
            if (report._policy_ran) {
                try {
                    gateway.run_maintenance();
                } catch (const std::exception& e) {
                    logger.log(kythira::log_level::warning, "redis gateway: maintenance failed",
                               {{"error", e.what()}});
                }
            }
            std::this_thread::sleep_for(opt._tick_interval);
        }
    });

    std::cout << "redis_gateway_node " << opt._node_id << " ready: raft=" << opt._raft_port
              << " redis=" << gateway.port() << " tls=" << gateway.tls_port()
              << " shards=" << (cuts.size() + 1) << " serializer=" << Serializer{}.media_type()
              << std::endl;

    wait_for_stop();

    // The ordering. Nothing above this line may be reordered below it.
    gateway.stop();
    driving.store(false, std::memory_order_release);
    if (driver.joinable()) {
        driver.join();
    }
    host->stop();
    host.reset();
    stack.shutdown();
    return 0;
}

}  // namespace kythira::redis_node
