// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_host.cpp
/// @brief The daemon's `multi_raft` stack and serve loop, alone in its
///        translation unit (one stack per TU: cmd/multi_raft_node's lesson,
///        as in redis_gateway_node).
///
/// Raft runs over Boost.Beast with the CBOR wire and the binary segmented
/// persistence engine under `KINESIS_DATA_DIR/raft/<group>`. Every group's
/// state machine is a `stream_state_machine` over the node's one
/// `record_segment_store` (`KINESIS_DATA_DIR/segments`), so splitting or
/// merging groups moves no record bytes. The API, the internal endpoints
/// and health share one HTTPS listener (`kinesis_gateway.hpp`).

#include "config.hpp"
#include "stop_signal.hpp"

#include <kythira/kinesis/kinesis_actions.hpp>
#include <kythira/kinesis/kinesis_compat_profile.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_gateway.hpp>
#include <kythira/kinesis/kinesis_peer_client.hpp>
#include <kythira/kinesis/kinesis_raft_cluster.hpp>
#include <kythira/kinesis/kinesis_service.hpp>
#include <kythira/kinesis/kinesis_static_keys.hpp>
#include <kythira/kinesis/kinesis_stream_state_machine.hpp>

#include <raft/beast_http_transport_impl.hpp>
#include <raft/beast_https_server.hpp>
#include <raft/binary_file_persistence.hpp>
#include <raft/cbor_serializer.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/multi_raft_transport_handles.hpp>
#include <raft/persistence.hpp>
#include <raft/record_segment_store.hpp>
#include <raft/static_key_authorizer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
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
          _client(*_ioc,
                  std::unordered_map<kythira::host_node_id, std::string>{opt._peers.begin(),
                                                                         opt._peers.end()},
                  kythira::boost_beast_client_config{}, kythira::noop_metrics{}, _ioc) {
        // A peer that speaks to this node has told the Raft client where it
        // is, for names that do not resolve yet (bounded_resolver.hpp).
        _server.set_peer_seen_handler(_client.peer_seen_handler());
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

struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = kythira::host_node_id;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type =
        kythira::transport_client_handle<beast_stack::client_type, node_id_type>;
    using network_server_type =
        kythira::transport_server_handle<beast_stack::server_type, node_id_type>;

    using persistence_engine_type =
        kythira::binary_file_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kinesis::stream_state_machine<log_index_type>;

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

using host_type = kythira::multi_raft<host_types, std::string, std::uint64_t>;
using config_type = kythira::multi_raft_config<host_types, std::string, std::uint64_t>;
using descriptor_type =
    kythira::shard_descriptor<std::uint64_t, std::string, kythira::host_node_id>;
using cluster_type = kinesis::raft_stream_cluster<host_type>;
using front_end_type = kinesis::front_end<aws_auth::static_key_authorizer>;
using gateway_type = kinesis::gateway<front_end_type>;

auto read_file(const std::string& path, std::string_view what) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("kinesis_gateway_node: cannot read " + std::string(what) + " " +
                                 path);
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

/// The routing key a command names, so the host's cross-shard admission
/// check runs on every proposal.
struct stream_partitioner {
    [[nodiscard]] auto key_of(const std::vector<std::byte>& command) const -> std::string {
        return kinesis::command_key(kinesis::decode_command(command));
    }
};

}  // namespace

auto run(const node_options& opt) -> int {
    kythira::console_logger logger{opt._log_level};

    // Everything that can be wrong with the configuration fails before a
    // port is bound: the key file, the data directory, the store.
    aws_auth::static_key_authorizer authorizer(
        kinesis::parse_static_keys(read_file(opt._static_keys_file, "KINESIS_STATIC_KEYS_FILE")),
        opt._partition);
    authorizer.register_service(kinesis::kinesis_static_roles());
    auto profile = kinesis::cluster_profile(opt._compat);
    if (!profile) {
        throw std::invalid_argument("kinesis_gateway_node: KINESIS_COMPAT refused");
    }

    std::filesystem::create_directories(opt._data_dir / "raft");
    std::filesystem::create_directories(opt._data_dir / "segments");
    kythira::record_segment_store_options so;
    so._data_dir = opt._data_dir / "segments";
    kythira::record_segment_store store(so);

    const kinesis::peer_client peers(kinesis::peer_client_config{
        ._tls = !opt._allow_plaintext,
        ._ca_path = opt._peer_ca_path,
        ._timeout = opt._command_timeout,
    });
    std::vector<std::string> other_gateways;
    for (const auto& [id, gw] : opt._peer_gateways) {
        if (id != opt._node_id) {
            other_gateways.push_back(gw);
        }
    }
    const auto transport = kinesis::make_segment_transport(peers, opt._forward_secret);
    // Restore cannot finish without the active segment a snapshot names:
    // try every peer, and fail the restore (Raft retries it) when none has
    // it.
    kinesis::segment_fetcher fetch = [&store, &other_gateways,
                                      &transport](const kythira::record_segment_descriptor& d) {
        if (!store.fetch_segment(d, other_gateways, transport)) {
            throw kythira::record_segment_error("kinesis: no peer served segment " + d._stream +
                                                " at " + std::to_string(d._first_seq));
        }
    };

    beast_stack stack{opt};

    std::vector<kythira::host_node_id> voters;
    for (const auto& [id, url] : opt._peers) {
        voters.push_back(id);
    }

    config_type cfg{
        .node_id = opt._node_id,
        .network_client = host_types::network_client_type{stack._client},
        .network_server = host_types::network_server_type{stack._server},
        .store_factory =
            [&opt](const std::uint64_t& group) {
                return host_types::persistence_engine_type{opt._data_dir / "raft" /
                                                           std::to_string(group)};
            },
    };
    cfg.state_machine_factory = [&store, &fetch](const std::uint64_t&) {
        return host_types::state_machine_type(&store, fetch);
    };
    cfg.logger = kythira::console_logger{opt._log_level};
    cfg.logger_factory = [level = opt._log_level](const std::uint64_t&) {
        return kythira::console_logger{level};
    };
    cfg.config._election_timeout_min = opt._election_timeout_min;
    cfg.config._election_timeout_max = opt._election_timeout_max;
    cfg.config._heartbeat_interval = opt._heartbeat_interval;
    cfg.config._rpc_timeout = opt._rpc_timeout;
    cfg.config._max_append_bytes = opt._max_body_bytes;
    cfg.hibernation = kythira::hibernation_mode::off;
    cfg.executor_stripes = opt._executor_stripes;
    cfg.policy_interval = opt._policy_interval;
    cfg.partitioner = kythira::make_partitioner<std::string>(stream_partitioner{});

    auto host = std::make_unique<host_type>(std::move(cfg));

    // Initial tiling: cuts sorted and deduplicated, one group per range.
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

    const auto gateways = opt._peer_gateways;
    cluster_type cluster(
        *host,
        kinesis::make_http_forwarder<kythira::host_node_id>(
            peers, opt._forward_secret,
            [gateways](const kythira::host_node_id& id) -> std::optional<std::string> {
                const auto it = gateways.find(id);
                return it == gateways.end() ? std::nullopt : std::optional{it->second};
            }),
        opt._command_timeout);

    kinesis::front_end_config fc;
    fc._region = opt._region;
    fc._partition = opt._partition;
    front_end_type front_end(fc, authorizer);
    kinesis::kinesis_service_config sc;
    sc._cluster_profile = *profile;
    sc._limits = kinesis::kinesis_limits::for_region(opt._region);
    kinesis::kinesis_service<cluster_type> service(cluster, sc);
    service.register_with(front_end);
    gateway_type gateway(
        front_end,
        [&cluster](const std::string& key, const std::vector<std::byte>& command) {
            return cluster.propose_local(key, command);
        },
        &store, opt._forward_secret);

    https::server_config hc;
    hc.bind_address = opt._listen_host;
    hc.port = opt._listen_port;
    hc.tls_cert_path = opt._tls_cert_path;
    hc.tls_key_path = opt._tls_key_path;
    hc.insecure_plaintext = opt._allow_plaintext;
    hc.max_body_bytes = opt._max_body_bytes;
    https::server server(
        hc,
        [&gateway](const https::request& req, const https::request_context& ctx) {
            return gateway.handle(req, ctx);
        },
        [&logger](bool warning, std::string_view message) {
            logger.log(warning ? kythira::log_level::warning : kythira::log_level::info,
                       "kinesis gateway: " + std::string(message), {});
        });
    server.start();

    std::atomic<bool> driving{true};
    std::thread driver([&] {
        while (driving.load(std::memory_order_acquire)) {
            (void)host->tick();
            std::this_thread::sleep_for(opt._tick_interval);
        }
    });

    std::cout << "kinesis_gateway_node " << kythira::host_node_id_text(opt._node_id)
              << " ready: raft=" << opt._raft_port << " kinesis=" << server.port()
              << (opt._allow_plaintext ? " (http)" : " (https)") << " groups=" << (cuts.size() + 1)
              << " voters=" << voters.size() << std::endl;

    wait_for_stop();

    // The ordering: no request in flight once the host stops, no tick once
    // it is gone.
    server.stop();
    driving.store(false, std::memory_order_release);
    if (driver.joinable()) {
        driver.join();
    }
    host->stop();
    host.reset();
    stack.shutdown();
    return 0;
}

}  // namespace kythira::kinesis_node
