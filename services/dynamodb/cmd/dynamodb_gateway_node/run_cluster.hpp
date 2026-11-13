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

#include "group_ids.hpp"
#include "group_layout.hpp"

#include <kythira/dynamodb/forward_link.hpp>
#include <kythira/dynamodb/gateway.hpp>
#include <kythira/dynamodb/group_state_machine.hpp>
#include <kythira/dynamodb/raft_backend.hpp>
#include <kythira/dynamodb/routed_backend.hpp>

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
#include <raft/split_merge_policy.hpp>

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
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
/// A directory for the life of the process: the memory engine's change
/// log (task 16), which outlives nothing the items do not.
class scratch_dir {
public:
    explicit scratch_dir(const std::string& stem) {
        auto pattern = (std::filesystem::temp_directory_path() / (stem + "-XXXXXX")).string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("dynamodb_gateway_node: cannot make a scratch directory");
        }
        _path = pattern;
    }
    scratch_dir(const scratch_dir&) = delete;
    auto operator=(const scratch_dir&) -> scratch_dir& = delete;
    scratch_dir(scratch_dir&&) = delete;
    auto operator=(scratch_dir&&) -> scratch_dir& = delete;
    ~scratch_dir() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    [[nodiscard]] auto path() const -> const std::filesystem::path& { return _path; }

private:
    std::filesystem::path _path;
};

/// The node's change log store: beside the engine, or for the memory
/// engine (or no data directory) in `scratch`, made on first use.
inline auto open_change_log(const ddb::gateway_config& cfg, std::optional<scratch_dir>& scratch,
                            const std::filesystem::path& sub = {})
    -> std::unique_ptr<kythira::record_segment_store> {
    kythira::record_segment_store_options o;
    if (cfg.engine != "memory" && !cfg.data_dir.empty()) {
        o._data_dir = std::filesystem::path(cfg.data_dir) / sub / "change-log";
    } else {
        scratch.emplace("kythira-ddb-change-log");
        o._data_dir = scratch->path();
        // Nothing here outlives the process, so nothing needs to reach
        // the disk.
        o._barriers_enabled = false;
    }
    return std::make_unique<kythira::record_segment_store>(o);
}

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
    beast_stack(const std::string& bind, std::uint16_t port,
                const std::unordered_map<node_id, std::string>& urls)
        : _work(boost::asio::make_work_guard(*_ioc)),
          _server(*_ioc, bind, port, kythira::boost_beast_server_config{}, kythira::noop_metrics{}),
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

/// One host's place in the deployment: the cluster's own host, or the
/// strong host (task 27), whose groups span three regions.
struct host_spec {
    /// For log lines: "raft" or "strong raft".
    std::string label;
    node_id self;
    /// Every voter's Raft URL, this node's included.
    std::unordered_map<node_id, std::string> urls;
    std::vector<node_id> voters;
    std::string bind;
    std::uint16_t port = 0;
    /// This voter's place among the sorted voters, for its group id range.
    std::size_t ordinal = 0;
    /// Where this host keeps its snapshots, layout and id reservation
    /// (under the data directory), on a node that keeps its data.
    std::filesystem::path sub;
    /// Raft timings; the host's defaults when unset.
    std::optional<std::chrono::milliseconds> heartbeat;
    std::optional<std::chrono::milliseconds> election_min;
    std::optional<std::chrono::milliseconds> election_max;
    /// The strong host's first layout (`strong_bootstrap_descriptors`)
    /// instead of a cluster's.
    bool strong = false;
};

/// A host's storage: its engine, and its groups' Raft log stores.
template<typename Storage> struct host_storage {
    std::shared_ptr<typename Storage::engine_type> engine;
    std::function<typename Storage::persistence_engine_type(std::uint64_t)> store_factory;
};

/// One multi-raft host running: its transport, change log, groups and
/// tick driver. Built and stopped in the order that leaves no thread or
/// callback behind; neither copied nor moved, since the host's callbacks
/// hold its address.
template<typename Storage> class host_runtime {
public:
    using types = host_types<Storage>;
    using host_type = kythira::multi_raft<types, std::string, std::uint64_t>;
    using config_type = kythira::multi_raft_config<types, std::string, std::uint64_t>;
    using backend_type = ddb::raft_backend<host_type>;
    using descriptor_type = typename config_type::descriptor_type;
    using traits = kythira::node_id_traits<node_id>;

    host_runtime(const ddb::gateway_config& cfg, const host_spec& spec,
                 host_storage<Storage> storage,
                 const std::function<void(bool, std::string_view)>& log)
        : _keeps_data(cfg.engine != "memory" && !cfg.data_dir.empty()),
          _dir(_keeps_data ? std::filesystem::path(cfg.data_dir) / spec.sub
                           : std::filesystem::path{}),
          // The change log's segments (task 15) live beside the engine; the
          // memory engine's in a scratch directory, gone with the process as
          // its items are. The store outlives the host, whose groups append
          // to it.
          _change_log(open_change_log(cfg, _scratch, spec.sub)),
          _stack(spec.bind, spec.port, spec.urls) {
        config_type hc{
            .node_id = spec.self,
            .network_client = typename types::network_client_type{_stack._client},
            .network_server = typename types::network_server_type{_stack._server},
            .store_factory = [factory = std::move(storage.store_factory)](
                                 const std::uint64_t& group) { return factory(group); },
            .on_group_destroyed = {},
        };
        hc.state_engine = std::move(storage.engine);
        if (spec.heartbeat) {
            hc.config._heartbeat_interval = *spec.heartbeat;
        }
        if (spec.election_min) {
            hc.config._election_timeout_min = *spec.election_min;
        }
        if (spec.election_max) {
            hc.config._election_timeout_max = *spec.election_max;
        }
        // Where a group stages the checkpoints it sends and the snapshots it
        // receives. Without it a leader cannot send a format-1 snapshot, so
        // a follower that fell behind the leader's log (one that was down,
        // or a split child's replica that missed entries) never catches up.
        if (_keeps_data) {
            hc.config._snapshot_staging_directory = _dir / "snapshots";
        } else {
            _snapshot_scratch.emplace("kythira-ddb-snapshots");
            hc.config._snapshot_staging_directory = _snapshot_scratch->path();
        }
        std::filesystem::create_directories(hc.config._snapshot_staging_directory);
        hc.logger = kythira::console_logger{cfg.log_level};
        hc.logger_factory = [level = cfg.log_level](const std::uint64_t&) {
            return kythira::console_logger{level};
        };
        hc.state_machine_factory = [this](const std::uint64_t& group) {
            ddb::dynamo_group_state_machine<typename Storage::engine_type> inner;
            inner.set_change_log(
                _change_log.get(), [this, group](const kythira::record_segment_descriptor& d) {
                    auto* b = _fetch_via.load(std::memory_order_acquire);
                    if (b == nullptr) {
                        throw kythira::record_segment_error(
                            "dynamodb_gateway_node: cannot fetch a change log segment before "
                            "serving");
                    }
                    b->fetch_change_segment(group, d);
                });
            return typename types::state_machine_type(std::move(inner));
        };
        // Automatic splits (task 24): the host's size policy, on the bytes
        // and keys each group's state machine reports, cut at a partition
        // hash bucket the state machine picks. Never automatic merges: the
        // catalog group reports no size, and a merge policy would read that
        // as small.
        if (cfg.cluster.splits_enabled()) {
            constexpr auto k_never = std::numeric_limits<std::size_t>::max();
            kythira::threshold_split_merge_policy_config pc;
            pc._shard_split_size_bytes = cfg.cluster.split_bytes != 0
                                             ? static_cast<std::size_t>(cfg.cluster.split_bytes)
                                             : k_never;
            pc._shard_split_keys = cfg.cluster.split_keys != 0
                                       ? static_cast<std::size_t>(cfg.cluster.split_keys)
                                       : k_never;
            hc.evaluate_split =
                [policy = kythira::threshold_split_merge_policy<std::uint64_t, std::string,
                                                                typename types::node_id_type>(pc)](
                    const auto& stats) mutable { return policy.evaluate_split(stats); };
        }
        hc.automatic_split_merge_enabled = cfg.cluster.splits_enabled();
        hc.split_merge_interval = cfg.cluster.split_interval;
        // A split's children need fresh group ids: this voter's reserved
        // range (group_ids.hpp), recorded beside a RocksDB node's data.
        _group_ids = std::make_shared<reserved_group_ids>(
            spec.ordinal, cfg.engine == "rocksdb"
                              ? std::optional<std::filesystem::path>(_dir / "group-ids")
                              : std::nullopt);
        hc.allocate_group_ids = [ids = _group_ids](std::size_t n) { return ids->allocate(n); };
        // A RocksDB node restarts with the groups it last had
        // (group_layout.hpp), saved on every split before the parent applies
        // anything later.
        if (_keeps_data) {
            _layout.emplace(
                _dir / "layout.json", [](const node_id& n) { return traits::to_text(n); },
                [](const std::string& t) { return peer_id(t); });
            hc.on_local_layout_changed = [this, log](const std::vector<descriptor_type>& groups) {
                if (!_layout->save(groups)) {
                    log(true, "cannot save the group layout to " + _layout->path().string() +
                                  "; a restart before the next save may lose a split's groups");
                }
            };
        }
        _host = std::make_unique<host_type>(std::move(hc));
        const auto saved = _layout ? _layout->load() : std::nullopt;
        const auto first = spec.strong ? backend_type::strong_bootstrap_descriptors(spec.voters)
                                       : backend_type::bootstrap_descriptors(spec.voters);
        for (const auto& d : saved ? *saved : first) {
            _host->create_group(d);
        }
        _host->start();
        _driving.store(true, std::memory_order_release);
        _driver = std::thread([this] {
            while (_driving.load(std::memory_order_acquire)) {
                _host->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        });
    }

    ~host_runtime() { stop(); }
    host_runtime(const host_runtime&) = delete;
    auto operator=(const host_runtime&) -> host_runtime& = delete;
    host_runtime(host_runtime&&) = delete;
    auto operator=(host_runtime&&) -> host_runtime& = delete;

    [[nodiscard]] auto host() -> host_type& { return *_host; }
    [[nodiscard]] auto change_log() -> kythira::record_segment_store* { return _change_log.get(); }

    /// The backend a group installing a snapshot fetches the change log
    /// segment it names through; null when there is none.
    auto set_fetch(backend_type* b) -> void { _fetch_via.store(b, std::memory_order_release); }

    /// Stops the driver, the host and the transport, in that order.
    auto stop() -> void {
        if (_stopped.exchange(true)) {
            return;
        }
        _driving.store(false, std::memory_order_release);
        if (_driver.joinable()) {
            _driver.join();
        }
        if (_host) {
            _host->stop();
            _host.reset();
        }
        _stack.shutdown();
    }

private:
    bool _keeps_data;
    std::filesystem::path _dir;
    std::optional<scratch_dir> _scratch;
    std::unique_ptr<kythira::record_segment_store> _change_log;
    std::atomic<backend_type*> _fetch_via{nullptr};
    beast_stack _stack;
    std::optional<scratch_dir> _snapshot_scratch;
    std::shared_ptr<reserved_group_ids> _group_ids;
    std::optional<group_layout_file<descriptor_type, node_id>> _layout;
    std::unique_ptr<host_type> _host;
    std::atomic<bool> _driving{false};
    std::thread _driver;
    std::atomic<bool> _stopped{false};
};

/// The strong host's place (task 27): its voters across the three regions,
/// with timings for a wide-area network, under `<data dir>/strong`.
inline auto strong_spec(const ddb::strong_config& k) -> host_spec {
    host_spec spec;
    spec.label = "strong raft";
    spec.self = peer_id(k.node_id);
    for (const auto& [text, p] : k.peers) {
        const auto id = peer_id(text);
        spec.urls.emplace(id, p.url);
        spec.voters.push_back(id);
    }
    spec.bind = k.raft_bind;
    spec.port = k.raft_port;
    spec.ordinal =
        static_cast<std::size_t>(std::distance(k.peers.begin(), k.peers.find(k.node_id)));
    spec.sub = "strong";
    spec.heartbeat = k.heartbeat;
    spec.election_min = k.election_min;
    spec.election_max = k.election_max;
    spec.strong = true;
    return spec;
}

/// Builds and runs the node until `wait()` returns, then shuts it down in
/// the order that leaves no thread or callback behind. `strong` is the
/// strong host's storage (task 27), when the node has one; a memory node
/// gets a fresh memory engine when none is given.
template<typename Storage>
auto run_cluster(
    const ddb::gateway_config& cfg, std::shared_ptr<typename Storage::engine_type> engine,
    const std::function<typename Storage::persistence_engine_type(std::uint64_t)>& store_factory,
    const std::function<void(bool, std::string_view)>& log, const std::function<void()>& wait,
    std::optional<host_storage<Storage>> strong = std::nullopt) -> void {
    using runtime = host_runtime<Storage>;
    using backend_type = typename runtime::backend_type;
    using traits = kythira::node_id_traits<node_id>;

    const auto& c = cfg.cluster;
    auto self = kythira::parse_host_node_id(c.node_id);
    if (!self) {
        throw std::invalid_argument("KYTHIRA_DYNAMODB_NODE_ID: " + self.error());
    }
    host_spec spec;
    spec.label = "raft";
    spec.self = *self;
    for (const auto& [text, url] : c.peers) {
        const auto id = text == c.node_id ? *self : peer_id(text);
        spec.urls.emplace(id, url);
        spec.voters.push_back(id);
    }
    spec.bind = c.raft_bind;
    spec.port = c.raft_port;
    spec.ordinal =
        static_cast<std::size_t>(std::distance(c.peers.begin(), c.peers.find(c.node_id)));
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

    runtime local(cfg, spec, host_storage<Storage>{std::move(engine), store_factory}, log);

    // The strong host (task 27), when this node is one of its voters.
    std::optional<host_spec> sspec;
    std::unique_ptr<runtime> strong_host;
    std::optional<ddb::forward_client> strong_forward;
    if (cfg.strong.enabled()) {
        sspec = strong_spec(cfg.strong);
        if (!strong) {
            if constexpr (std::is_same_v<Storage, memory_storage>) {
                strong = host_storage<Storage>{
                    std::make_shared<typename Storage::engine_type>(),
                    [](std::uint64_t) { return typename Storage::persistence_engine_type{}; }};
            } else {
                throw std::invalid_argument(
                    "dynamodb_gateway_node: no storage for the strong host");
            }
        }
        std::map<std::string, ddb::forward_peer> strong_gateways;
        for (const auto& [text, peer] : cfg.strong.gateways) {
            strong_gateways.emplace(traits::to_text(peer_id(text)), peer);
        }
        strong_forward.emplace(
            ddb::forward_client_config{.secret = cfg.strong.forward_secret,
                                       .tls = !cfg.http.insecure_plaintext,
                                       .ca_path = c.forward_ca_path,
                                       .timeout = std::chrono::milliseconds{20000},
                                       .path = std::string(ddb::k_strong_forward_path)},
            std::move(strong_gateways));
        strong_host = std::make_unique<runtime>(cfg, *sspec, std::move(*strong), log);
    }

    auto stop_hosts = [&] {
        if (strong_host) {
            strong_host->stop();
        }
        local.stop();
    };

    try {
        // Each voter enforces its share of every capacity limit: a client
        // spreading requests over the nodes gets the table's whole limit.
        ddb::raft_backend_options options;
        options.capacity_share = 1.0 / static_cast<double>(spec.voters.size());
        backend_type backend(
            local.host(),
            [&forward](const node_id& to, const std::string& op) {
                return forward.send(traits::to_text(to), op);
            },
            options);
        backend.set_change_log(local.change_log());
        local.set_fetch(&backend);
        // No fetch through the backend once it is gone.
        const std::unique_ptr<runtime, void (*)(runtime*)> clear_fetch(
            &local, [](runtime* r) { r->set_fetch(nullptr); });

        // The strong host's backend: its catalog group is the strong
        // catalog, and its ops reach leaders in other regions through their
        // gateways.
        std::optional<backend_type> strong_backend;
        if (strong_host) {
            ddb::raft_backend_options so;
            // Leaders may be a region away, and elections take seconds.
            so.attempt_timeout = std::chrono::milliseconds{10000};
            so.deadline = std::chrono::milliseconds{30000};
            so.capacity_share = 1.0 / static_cast<double>(sspec->voters.size());
            strong_backend.emplace(
                strong_host->host(),
                [&strong_forward](const node_id& to, const std::string& op) {
                    return strong_forward->send(traits::to_text(to), op);
                },
                so);
            strong_backend->set_change_log(strong_host->change_log());
            strong_host->set_fetch(&*strong_backend);
        }
        const std::unique_ptr<std::unique_ptr<runtime>, void (*)(std::unique_ptr<runtime>*)>
            clear_strong_fetch(&strong_host, [](std::unique_ptr<runtime>* r) {
                if (*r) {
                    (*r)->set_fetch(nullptr);
                }
            });

        // The gateway serves from both hosts: a key under a strong table id
        // goes to the strong host, everything else to the cluster's.
        ddb::routed_backend<backend_type> routed(backend,
                                                 strong_backend ? &*strong_backend : nullptr);
        ddb::gateway<ddb::routed_backend<backend_type>> gw(cfg, routed, log);
        if (strong_backend) {
            gw.set_strong_ops(
                [&strong_backend](std::string_view op) { return strong_backend->serve(op); });
        }
        gw.start();
        backend.start();
        if (strong_backend) {
            strong_backend->start();
        }
        log(false, "serving " + std::string(cfg.http.insecure_plaintext ? "http" : "https") +
                       " on " + cfg.http.bind_address + ":" + std::to_string(gw.port()) +
                       ", raft on " + c.raft_bind + ":" + std::to_string(c.raft_port) + ", node " +
                       c.node_id + " of " + std::to_string(spec.voters.size()) + ", engine " +
                       std::string(Storage::k_name));
        // The strong host is ready once its catalog has taken this node's
        // seed (ids from `k_strong_table_id_base`; only the first voter's
        // changes anything), which takes a leader that a quorum spanning
        // two regions elected.
        std::atomic<bool> probing{static_cast<bool>(strong_backend)};
        std::thread probe;
        if (strong_backend) {
            probe = std::thread([&] {
                while (probing.load(std::memory_order_acquire)) {
                    try {
                        ddb::catalog_command seed;
                        seed.op = ddb::catalog_command::kind::seed_ids;
                        seed.table.table_id = ddb::k_strong_table_id_base;
                        if (!strong_backend->catalog(seed).ok()) {
                            throw std::runtime_error("the strong catalog refused its seed");
                        }
                        std::string regions;
                        for (const auto& r : cfg.strong.regions()) {
                            regions += (regions.empty() ? "" : ", ") + r;
                        }
                        log(false, "strong host ready: strong raft on " + cfg.strong.raft_bind +
                                       ":" + std::to_string(cfg.strong.raft_port) + ", node " +
                                       cfg.strong.node_id + " of " +
                                       std::to_string(sspec->voters.size()) + " in regions " +
                                       regions);
                        return;
                    } catch (const std::exception&) {
                        std::this_thread::sleep_for(std::chrono::milliseconds{200});
                    }
                }
            });
        }
        const auto stop_probe = [&] {
            probing.store(false, std::memory_order_release);
            if (probe.joinable()) {
                probe.join();
            }
        };
        try {
            wait();
        } catch (...) {
            stop_probe();
            throw;
        }
        stop_probe();
        gw.stop();
        if (strong_backend) {
            strong_backend->stop();
        }
        backend.stop();
    } catch (...) {
        stop_hosts();
        throw;
    }
    stop_hosts();
}

}  // namespace dynamodb_gateway
