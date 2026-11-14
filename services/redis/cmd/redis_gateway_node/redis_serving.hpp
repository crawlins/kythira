// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file redis_serving.hpp
/// @brief The gateway's `multi_raft` host and everything serving over it,
///        on whatever Raft transport the node has
///        (.kiro/specs/service-colocation/ task 7): what the daemon's
///        run_host() did between binding its Raft port and its signal wait,
///        split into the module phases.
///
/// `host_serving<Host, Storage>` is instantiated ONCE PER TRANSLATION UNIT
/// (`serve_*.cpp`) — the lesson of cmd/multi_raft_node/host_stacks.hpp,
/// where four stacks in one file cost 11 GiB of compiler RSS. `Host` is a
/// service_node::node_host: the daemon's own Beast transport
/// (standalone_node.hpp), or kythira_node's shared listener.
///
/// The group ids are the module's group space's: the initial tiling's
/// `1..n` and the snapshot-isolation oracle's `k_oracle_group` stand-alone,
/// `1 << 56` and up in kythira_node. The gateway never splits a group
/// (no id allocator is set), so those are all the ids it uses.

#include "config.hpp"
#include "redis_serving_api.hpp"

#include <raft/binary_file_persistence.hpp>
#include <kythira/redis/cache_chunk_fetcher.hpp>
#include <kythira/redis/cache_chunk_store.hpp>
#include <kythira/redis/cache_disk_guard.hpp>
#include <kythira/redis/cache_http_front_end.hpp>
#include <kythira/redis/cache_kv_state_machine.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/multi_raft_transport_handles.hpp>
#include <raft/persistence.hpp>
#include <kythira/redis/redis_gateway_impl.hpp>
#include <kythira/redis/redis_kv_mvcc_state_machine.hpp>
#include <kythira/redis/redis_kv_state_machine.hpp>
#include <raft/service_module.hpp>
#include <raft/txn_state_machine.hpp>
#if defined(KYTHIRA_REDIS_GATEWAY_ENGINE)
#include <raft/rocksdb_kv_engine.hpp>
#include <kythira/redis/persistent_kv_state_machine.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::redis_node {

/// The timestamp oracle group's id stand-alone (multi-raft-transactions
/// task 14). Fixed, so every node of a cluster names the same group; far
/// above the shard ids, which count up from 1 once per `--shard-cut`.
inline constexpr std::uint64_t k_oracle_group = 0xFFFF'FFFF'FFFF'FF00ULL;

/// Values and Raft state in RAM: the gateway before the disk-backed cache.
struct memory_storage {
    static constexpr bool on_disk = false;
    static constexpr bool engine = false;
    // CONFIG_REDIS_GATEWAY_TRANSACTIONS=y wraps the KV state machine for
    // cross-shard transactions (multi-raft-transactions task 9). A build
    // choice, not a runtime one: the wrapper's snapshot format differs, so
    // every node of a cluster must be built the same way. Only the in-memory
    // store is wrapped; config.cpp refuses transactions with file persistence.
#if defined(KYTHIRA_REDIS_GATEWAY_SNAPSHOT_ISOLATION)
    // Tier 2 (task 14): every value versioned, every write timestamped by
    // the oracle group the node bootstrap creates beside the shards.
    using state_machine_type =
        kythira::transactional_state_machine<kythira::redis_kv_mvcc_state_machine<std::uint64_t>,
                                             std::string, kythira::redis_kv_partitioner,
                                             kythira::mvcc::on>;
#elif defined(KYTHIRA_REDIS_GATEWAY_TRANSACTIONS)
    using state_machine_type =
        kythira::transactional_state_machine<kythira::redis_kv_state_machine<std::uint64_t>,
                                             std::string, kythira::redis_kv_partitioner>;
#else
    using state_machine_type = kythira::redis_kv_state_machine<std::uint64_t>;
#endif
    using persistence_engine_type =
        kythira::memory_persistence_engine<kythira::host_node_id, std::uint64_t, std::uint64_t>;
};

/// Values in the node's chunk store, Raft state in the binary segmented
/// engine (.kiro/specs/disk-backed-build-cache/).
struct file_storage {
    static constexpr bool on_disk = true;
    static constexpr bool engine = false;
    using state_machine_type = kythira::cache_kv_state_machine<std::uint64_t>;
    using persistence_engine_type =
        kythira::binary_file_persistence_engine<kythira::host_node_id, std::uint64_t,
                                                std::uint64_t>;
};

#if defined(KYTHIRA_REDIS_GATEWAY_ENGINE)
/// Values in the node's RocksDB database, Raft logs in the binary segmented
/// engine beside it (.kiro/specs/persistent-state-machine/ task 7). Not the
/// chunk store's `on_disk`: no chunk store, fetcher or HTTP front end.
/// Transactions wrap the persistent machine, whose intents then live in the
/// database too (task 6); snapshot isolation is not built for it.
struct engine_storage {
    static constexpr bool on_disk = false;
    static constexpr bool engine = true;
    using kv_type = kythira::persistent_kv_state_machine<kythira::rocksdb_kv_engine, std::uint64_t>;
#if defined(KYTHIRA_REDIS_GATEWAY_SNAPSHOT_ISOLATION)
#error "CONFIG_REDIS_GATEWAY_ENGINE does not combine with CONFIG_REDIS_GATEWAY_SNAPSHOT_ISOLATION"
#elif defined(KYTHIRA_REDIS_GATEWAY_TRANSACTIONS)
    using state_machine_type =
        kythira::transactional_state_machine<kv_type, std::string, kythira::redis_kv_partitioner>;
#else
    using state_machine_type = kv_type;
#endif
    using persistence_engine_type =
        kythira::binary_file_persistence_engine<kythira::host_node_id, std::uint64_t,
                                                std::uint64_t>;
};

/// Bytes under `dir`, for the engine's disk budget. Files can vanish while
/// RocksDB compacts, so a file that cannot be sized counts as empty.
inline auto directory_bytes(const std::filesystem::path& dir) -> std::uint64_t {
    std::uint64_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code size_ec;
        if (it->is_regular_file(size_ec)) {
            const auto n = it->file_size(size_ec);
            total += size_ec ? 0 : n;
        }
    }
    return total;
}
#endif

/// The gateway's multi_raft types on a node whose transport is `Host`'s.
template<typename Host, typename Storage> struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = kythira::host_node_id;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    /// Not used by the host: the wire is the node's transport's.
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type =
        kythira::transport_client_handle<typename Host::client_type, node_id_type>;
    using network_server_type = typename Host::view_type;

    using persistence_engine_type = typename Storage::persistence_engine_type;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = typename Storage::state_machine_type;

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

/// The disk-backed node's own state: the chunk store and the counts over
/// it, the disk guard, and the chunk fetcher. Built before the host, whose
/// state machines need the counts, and torn down after it.
template<typename NodeId, typename GroupId> struct disk_node {
    explicit disk_node(const cache_options& c, std::string self)
        : _store(store_options(c)),
          _guard(std::move(self), kythira::cache_disk_guard_options{
                                      ._max_disk_bytes = c._max_disk_bytes,
                                      ._min_free_bytes = c._min_free_bytes,
                                      ._min_free_inodes = c._min_free_inodes,
                                  }) {}

    static auto store_options(const cache_options& c) -> kythira::cache_chunk_store_options {
        kythira::cache_chunk_store_options o;
        o._data_dir = c._data_dir;
        o._pack_threshold_bytes = c._pack_threshold_bytes;
        return o;
    }

    /// Re-read this node's free space, store size and pinned bytes into
    /// the guard.
    auto refresh() -> kythira::cache_node_report {
        const auto space = _store.space();
        const auto stats = _store.stats();
        kythira::cache_node_report r{
            ._free_bytes = space._free_bytes,
            ._free_inodes = space._free_inodes,
            ._store_bytes = stats.bytes_on_disk(),
            ._pinned_bytes = _pinned ? _pinned() : 0,
        };
        return _guard.set_local(r);
    }

    /// Bytes of chunks inside some replica's orphan grace, summed over the
    /// host's groups. Set once the host has started and cleared before it
    /// stops.
    std::function<std::uint64_t()> _pinned;

    kythira::cache_chunk_store _store;
    /// The counts hold a reference to `_store`. Every holder of `_refs`
    /// (the host's state machines, the fetcher, the front end) is gone
    /// before this object is, so the store outlives them.
    std::shared_ptr<kythira::chunk_refcounts> _refs{
        std::make_shared<kythira::chunk_refcounts>(_store)};
    kythira::cache_disk_guard _guard;
    std::unique_ptr<kythira::cache_chunk_fetcher<NodeId, GroupId>> _fetcher;
    std::atomic<std::uint64_t> _orphans_collected{0};
};

inline auto node_text(const kythira::host_node_id& id) -> std::string {
    std::ostringstream out;
    out << id;
    return out.str();
}

/// The oracle's group id in `space`. The initial tiling counts up from 1;
/// the oracle keeps the stand-alone id's low 56 bits, so a stand-alone
/// node's ids are exactly what they always were.
[[nodiscard]] inline auto oracle_group_in(const service_node::group_space& space) -> std::uint64_t {
    if (!space.is_composite()) {
        return k_oracle_group;
    }
    return space.to_host(k_oracle_group &
                         ((std::uint64_t{1} << service_node::group_space::k_shift) - 1));
}

/// Build, start, serve, shut down — in the order that does not terminate the
/// process (cmd/multi_raft_node/host_stacks.hpp's `run_host` ordering).
template<typename Host, typename Storage> class host_serving final : public serving {
public:
    using types = host_types<Host, Storage>;
    using node_id_type = typename types::node_id_type;
    using host_type = kythira::multi_raft<types, std::string, std::uint64_t>;
    using config_type = kythira::multi_raft_config<types, std::string, std::uint64_t>;
    using descriptor_type = kythira::shard_descriptor<std::uint64_t, std::string, node_id_type>;
    using gateway_type =
        kythira::redis_gateway<host_type, kythira::console_logger, kythira::noop_metrics>;
    using disk_type = disk_node<node_id_type, std::uint64_t>;
    using front_end_type = kythira::cache_http_front_end<host_type, kythira::console_logger>;

    /// Opens the disk-backed node's store (so a data directory that cannot
    /// be used fails before the node serves), builds the host on `node`'s
    /// transport in `space`, and creates the initial groups.
    /// @param acl, gateway_cfg Outlive this object (the module's).
    host_serving(const node_options& opt, kythira::redis_acl& acl,
                 const kythira::redis_gateway_config& gateway_cfg, Host& node,
                 const service_node::group_space& space)
        : _opt(opt), _acl(&acl), _gateway_cfg(&gateway_cfg), _logger{opt._log_level} {
        if constexpr (Storage::on_disk) {
            std::filesystem::create_directories(_opt._cache._data_dir / "raft");
            _disk = std::make_unique<disk_type>(_opt._cache, node_text(_opt._node_id));
            _disk->refresh();
            kythira::cache_chunk_fetcher_options fo;
            fo._bytes_per_sec = _opt._cache._fetch_bytes_per_sec;
            _disk->_fetcher =
                std::make_unique<kythira::cache_chunk_fetcher<node_id_type, std::uint64_t>>(
                    _disk->_refs,
                    // The shard's leader first, then its other voters.
                    [this](const std::uint64_t& group) {
                        std::vector<node_id_type> out;
                        auto* h = _host_ptr.load(std::memory_order_acquire);
                        if (h == nullptr) {
                            return out;
                        }
                        auto* node = h->group_node(group);
                        auto desc = h->local_descriptor(group);
                        if (node != nullptr) {
                            if (auto leader = node->known_leader();
                                leader && *leader != h->node_id()) {
                                out.push_back(*leader);
                            }
                        }
                        if (desc) {
                            for (const auto& v : desc->voters()) {
                                if (v != h->node_id() &&
                                    std::find(out.begin(), out.end(), v) == out.end()) {
                                    out.push_back(v);
                                }
                            }
                        }
                        return out;
                    },
                    // Bound to the front end once it exists; until then every
                    // fetch misses and is retried.
                    [](const node_id_type&, const kythira::chunk_sha&)
                        -> std::optional<std::vector<std::byte>> { return std::nullopt; },
                    [this](const std::uint64_t& group) {
                        auto* h = _host_ptr.load(std::memory_order_acquire);
                        if (h == nullptr) {
                            return;
                        }
                        if (auto* node = h->group_node(group)) {
                            (void)node->with_state_machine(
                                [](auto& sm) { return sm.refresh_has_all(); });
                        }
                    },
                    fo);
        }

        for (const auto& [id, url] : _opt._peers) {
            _voters.push_back(id);
        }

        _host = std::make_unique<host_type>(make_config(node, space));
        _host_ptr.store(_host.get(), std::memory_order_release);

        // Initial tiling: cuts sorted and deduplicated, one shard per range.
        auto cuts = _opt._shard_cuts;
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        std::optional<std::string> start;
        std::uint64_t group = 1;
        for (std::size_t i = 0; i <= cuts.size(); ++i) {
            std::optional<std::string> end =
                i < cuts.size() ? std::optional<std::string>{cuts[i]} : std::nullopt;
            _host->create_group(descriptor_type{
                ._group_id = space.to_host(group++),
                ._range = kythira::shard_range<std::string>{._start = start, ._end = end},
                ._epoch = kythira::shard_epoch{},
                ._voters = _voters});
            start = end;
        }
        _groups = cuts.size() + 1;
#if defined(KYTHIRA_REDIS_GATEWAY_SNAPSHOT_ISOLATION)
        if constexpr (!Storage::on_disk && !Storage::engine) {
            _host->create_oracle_group(_voters);
        }
#endif
    }

    host_serving(const host_serving&) = delete;
    auto operator=(const host_serving&) -> host_serving& = delete;
    host_serving(host_serving&&) = delete;
    auto operator=(host_serving&&) -> host_serving& = delete;
    ~host_serving() override { stop(); }

    void start() override {
        // Each group restores its snapshot inside start(), so once it returns
        // the node-wide counts are complete and any chunk they do not name is
        // an orphan (Requirement 2.6).
        _host->start();
        if constexpr (Storage::on_disk) {
            const auto removed = _disk->_refs->collect_orphans();
            _disk->_orphans_collected += removed;
            _logger.log(kythira::log_level::info, "cache: start-up orphan collection",
                        {{"removed", std::to_string(removed)},
                         {"referenced", std::to_string(_disk->_refs->size())}});
            _disk->_pinned = [h = _host.get()] {
                std::uint64_t pinned = 0;
                for (const auto& g : h->group_ids()) {
                    if (auto* node = h->group_node(g)) {
                        pinned +=
                            node->with_state_machine([](auto& sm) { return sm.pending_bytes(); });
                    }
                }
                return pinned;
            };
        }

        auto peer_gateways = _opt._peer_gateways;
        _gateway = std::make_unique<gateway_type>(
            *_host, *_acl, _logger, _metrics, *_gateway_cfg,
            [peer_gateways](const node_id_type& id) -> std::optional<std::string> {
                auto it = peer_gateways.find(id);
                if (it == peer_gateways.end()) {
                    return std::nullopt;
                }
                return it->second;
            });
        if constexpr (Storage::on_disk) {
            build_front_end();
        }
        if (_engine_guard) {
            set_engine_hooks();
        }

        _gateway->start();
        if constexpr (Storage::on_disk) {
            if (!_opt._cache._http_listen.empty() || !_opt._cache._https_listen.empty()) {
                _front_end->start();
            }
            auto* fe = _front_end.get();
            _disk->_fetcher->set_fetch(
                [fe](const node_id_type& peer, const kythira::chunk_sha& sha) {
                    return fe->fetch_chunk(peer, sha);
                });
            _disk->_fetcher->start();
        }
        start_threads();
        _started = true;

        std::cout << "redis_gateway_node " << _opt._node_id << " ready: raft=" << _opt._raft_port
                  << " redis=" << _gateway->port() << " tls=" << _gateway->tls_port()
                  << " shards=" << _groups << " serializer="
                  << (_opt._serializer == wire_serializer::cbor ? "application/cbor"
                                                                : "application/json")
                  << " persistence="
                  << (Storage::on_disk ? "file" : (Storage::engine ? "engine" : "memory"));
        if constexpr (Storage::on_disk) {
            std::cout << " http=" << _front_end->port() << " https=" << _front_end->tls_port();
        }
        std::cout << std::endl;
    }

    // The ordering. Nothing in drain() may be reordered into stop().
    void drain() override {
        if (_front_end) {
            _front_end->stop();
        }
        if (_gateway) {
            _gateway->stop();
        }
    }

    void stop() override {
        drain();
        {
            const std::lock_guard lock(_compactor_mu);
            _driving.store(false, std::memory_order_release);
        }
        _compactor_cv.notify_all();
        if (_driver.joinable()) {
            _driver.join();
        }
        if (_compactor.joinable()) {
            _compactor.join();
        }
        if (_disk && _disk->_fetcher) {
            _disk->_fetcher->stop();
            _disk->_pinned = nullptr;  // it reads the host
        }
        _front_end.reset();
        if (_host) {
            _host->stop();
        }
        _host_ptr.store(nullptr, std::memory_order_release);
        _host.reset();
        _gateway.reset();
        _started = false;
    }

    [[nodiscard]] auto group_count() const -> std::size_t override { return _groups; }
    [[nodiscard]] auto redis_port() const -> std::uint16_t override {
        return _gateway ? _gateway->port() : 0;
    }

private:
    auto make_config(Host& node, const service_node::group_space& space) -> config_type {
        config_type cfg{
            .node_id = _opt._node_id,
            .network_client = typename types::network_client_type{node.client()},
            .network_server = node.server_for(space),
            .store_factory =
                [this](const std::uint64_t& group) {
                    if constexpr (Storage::on_disk || Storage::engine) {
                        return typename types::persistence_engine_type{
                            _opt._cache._data_dir / "raft" / std::to_string(group)};
                    } else {
                        // A cache: in-memory persistence is the honest
                        // choice here. A replica that restarts catches up
                        // from its peers.
                        (void)group;
                        return typename types::persistence_engine_type{};
                    }
                },
        };
        if constexpr (Storage::on_disk) {
            cfg.state_machine_factory = [this](const std::uint64_t& group) {
                kythira::cache_kv_options o;
                o._orphan_grace_entries = _opt._cache._orphan_grace_entries;
                // Called under the node lock: the fetcher only enqueues.
                auto* fetcher = _disk->_fetcher.get();
                o._on_missing_chunks = [fetcher, group](std::vector<kythira::chunk_sha> missing) {
                    fetcher->want(group, missing);
                };
                return typename types::state_machine_type(_disk->_refs, std::move(o));
            };
            cfg.config._snapshot_threshold_bytes = _opt._cache._snapshot_threshold_bytes;
        }
#if defined(KYTHIRA_REDIS_GATEWAY_ENGINE)
        // The engine node's guard: free space and the database's size
        // against the floors and the budget, with the peers' reports, as the
        // chunk store node's (disk-backed-build-cache task 7).
        if constexpr (Storage::engine) {
            // Layout: engine/ (the database), raft/<group> (Raft logs) and
            // snapshots/ (checkpoints and incoming transfers, on the
            // database's filesystem so ingest hard-links rather than
            // copies).
            const auto engine_dir = _opt._cache._data_dir / "engine";
            std::filesystem::create_directories(engine_dir);
            std::filesystem::create_directories(_opt._cache._data_dir / "raft");
            std::filesystem::create_directories(_opt._cache._data_dir / "snapshots");
            cfg.state_engine = std::make_shared<kythira::rocksdb_kv_engine>(engine_dir);
            cfg.config._snapshot_staging_directory = _opt._cache._data_dir / "snapshots";
            cfg.config._snapshot_disk_margin_bytes = _opt._cache._min_free_bytes;
            cfg.config._snapshot_threshold_bytes = _opt._cache._snapshot_threshold_bytes;
            // One bucket for the whole node: every shard's configuration is
            // a copy of this one and shares the pointer.
            if (_opt._cache._snapshot_bytes_per_sec != 0) {
                cfg.config._snapshot_pacer =
                    std::make_shared<kythira::snapshot_pacer>(_opt._cache._snapshot_bytes_per_sec);
            }
            _engine_guard = std::make_unique<kythira::cache_disk_guard>(
                node_text(_opt._node_id), kythira::cache_disk_guard_options{
                                              ._max_disk_bytes = _opt._cache._max_disk_bytes,
                                              ._min_free_bytes = _opt._cache._min_free_bytes,
                                              ._min_free_inodes = _opt._cache._min_free_inodes,
                                          });
            _engine_refresh = [g = _engine_guard.get(), dir = _opt._cache._data_dir, engine_dir] {
                const auto space = kythira::probe_filesystem_space(dir);
                return g->set_local(kythira::cache_node_report{
                    ._free_bytes = space._free_bytes,
                    ._free_inodes = space._free_inodes,
                    ._store_bytes = directory_bytes(engine_dir),
                    ._pinned_bytes = 0,
                });
            };
            _engine_refresh();
        }
#endif
        cfg.logger = kythira::console_logger{_opt._log_level};
        cfg.logger_factory = [level = _opt._log_level](const std::uint64_t&) {
            return kythira::console_logger{level};
        };
        cfg.config._election_timeout_min = _opt._election_timeout_min;
        cfg.config._election_timeout_max = _opt._election_timeout_max;
        cfg.config._heartbeat_interval = _opt._heartbeat_interval;
        cfg.config._rpc_timeout = _opt._rpc_timeout;
        // One chunk per AppendEntries at most, so the batch fits the
        // listener's limit (raft_request_body_limit).
        cfg.config._max_append_bytes = largest_entry_bytes(_opt);
        cfg.hibernation = kythira::hibernation_mode::off;
        cfg.executor_stripes = _opt._executor_stripes;
        cfg.policy_interval = _opt._policy_interval;
        cfg.partitioner = kythira::make_partitioner<std::string>(kythira::redis_kv_partitioner{});
#if defined(KYTHIRA_REDIS_GATEWAY_SNAPSHOT_ISOLATION)
        // The timestamp oracle's group, on every node, with an id no shard's
        // tiling can reach (shards count up from 1). Only the in-memory
        // store is transactional; config.cpp refuses transactions with file
        // persistence, and the file stack has no wrapper to hold the oracle.
        if constexpr (!Storage::on_disk && !Storage::engine) {
            cfg.oracle_group = oracle_group_in(space);
        }
#else
        (void)space;
#endif
        return cfg;
    }

    [[nodiscard]] static auto voter_names(const std::vector<node_id_type>& v)
        -> std::vector<std::string> {
        std::vector<std::string> out;
        out.reserve(v.size());
        for (const auto& id : v) {
            out.push_back(node_text(id));
        }
        return out;
    }

    /// The disk-backed node's cache hooks and HTTP front end.
    void build_front_end() {
        auto* guard = &_disk->_guard;
        auto refusal = [guard](const std::uint64_t&, const std::vector<node_id_type>& v) {
            return guard->refusal(voter_names(v));
        };
        typename gateway_type::cache_hooks hooks;
        hooks._write_refusal = refusal;
        // The last time each shard was told to evict for the disk budget:
        // reports older than that still count the bytes it freed. Only the
        // maintenance thread asks, but under a mutex all the same.
        auto evicted_at = std::make_shared<std::pair<
            std::mutex, std::map<std::uint64_t, kythira::cache_disk_guard::clock::time_point>>>();
        hooks._excess_bytes = [guard, evicted_at](const std::uint64_t& group,
                                                  const std::vector<node_id_type>& v) {
            const auto now = kythira::cache_disk_guard::clock::now();
            const std::lock_guard lock(evicted_at->first);
            auto& last = evicted_at->second[group];
            // A shard not yet evicted has `last` at the clock's epoch, before
            // every report.
            const auto excess = guard->excess_bytes(voter_names(v), now, last);
            if (excess > 0) {
                last = now;
            }
            return excess;
        };
        hooks._advisory = [d = _disk.get()] {
            return d->_guard.self() + ";" + d->refresh().encode();
        };
        hooks._on_advisory = [guard](std::string_view payload) {
            auto semi = payload.find(';');
            if (semi == std::string_view::npos) {
                return;
            }
            if (auto r = kythira::cache_node_report::decode(payload.substr(semi + 1))) {
                guard->record(std::string(payload.substr(0, semi)), *r);
            }
        };
        hooks._info = [d = _disk.get(), h = _host.get()] {
            const auto stats = d->_store.stats();
            const auto local = d->_guard.local().value_or(kythira::cache_node_report{});
            std::uint64_t keys = 0;
            std::uint64_t orphans = d->_orphans_collected.load();
            for (const auto& g : h->group_ids()) {
                if (auto* node = h->group_node(g)) {
                    auto [k, o] = node->with_state_machine([](auto& sm) {
                        return std::pair<std::uint64_t, std::uint64_t>{sm.approximate_key_count(),
                                                                       sm.orphans_removed()};
                    });
                    keys += k;
                    orphans += o;
                }
            }
            const auto& fetch = d->_fetcher->stats();
            std::ostringstream out;
            out << "# Cache\r\n"
                << "cache_persistence:file\r\n"
                << "cache_keys:" << keys << "\r\n"
                << "cache_chunks:" << d->_refs->size() << "\r\n"
                << "cache_store_bytes:" << stats.bytes_on_disk() << "\r\n"
                << "cache_store_files:" << stats.files() << "\r\n"
                << "cache_loose_chunks:" << stats._loose_files << "\r\n"
                << "cache_packed_chunks:" << stats._packed_chunks << "\r\n"
                << "cache_pack_dead_bytes:" << stats._pack_dead_bytes << "\r\n"
                << "cache_compactions:" << stats._compactions << "\r\n"
                << "cache_hash_failures:" << stats._hash_failures << "\r\n"
                << "cache_max_disk_bytes:" << d->_guard.options()._max_disk_bytes << "\r\n"
                << "cache_orphans_removed:" << orphans << "\r\n"
                << "cache_fetch_pending_chunks:" << d->_fetcher->pending() << "\r\n"
                << "cache_fetched_chunks:" << fetch._fetched.load() << "\r\n"
                << "cache_fetched_bytes:" << fetch._fetched_bytes.load() << "\r\n"
                << "cache_fs_free_bytes:" << local._free_bytes << "\r\n"
                << "cache_fs_free_inodes:" << local._free_inodes << "\r\n"
                << "cache_space_refusals:" << d->_guard.refusals() << "\r\n"
                << "cache_peer_reports:" << d->_guard.fresh_peers() << "\r\n";
            return out.str();
        };
        _gateway->set_cache_hooks(std::move(hooks));

        const auto& gateway_cfg = *_gateway_cfg;
        kythira::cache_http_config hc;
        hc._listen = _opt._cache._http_listen;
        hc._tls_listen = _opt._cache._https_listen;
        hc._allow_plaintext = _opt._cache._allow_plaintext_http;
        hc._tls_cert_path = _opt._cache._tls_cert_path;
        hc._tls_key_path = _opt._cache._tls_key_path;
        hc._chunk_bytes = _opt._cache._chunk_bytes;
        hc._max_value_bytes = _opt._cache._max_value_bytes;
        hc._min_free_bytes = _opt._cache._min_free_bytes;
        hc._min_free_inodes = _opt._cache._min_free_inodes;
        hc._auth_cache_ttl = _opt._cache._auth_cache_ttl;
        hc._internal_secret = gateway_cfg._internal_secret;
        hc._forwarding = gateway_cfg._forwarding;
        hc._forward_tls = gateway_cfg._forward_tls;
        hc._allow_plaintext_forwarding = gateway_cfg._allow_plaintext_forwarding;
        hc._forward_tls_ca_path = gateway_cfg._forward_tls_ca_path.empty()
                                      ? gateway_cfg._tls_ca_path
                                      : gateway_cfg._forward_tls_ca_path;
        hc._command_timeout = gateway_cfg._command_timeout;
        auto peer_http = _opt._cache._peer_http;
        auto* fetcher = _disk->_fetcher.get();
        _front_end = std::make_unique<front_end_type>(
            *_host, *_acl, _logger, _disk->_refs, hc,
            [peer_http](const node_id_type& id) -> std::optional<std::string> {
                auto it = peer_http.find(id);
                if (it == peer_http.end()) {
                    return std::nullopt;
                }
                return it->second;
            },
            // A chunk that failed its hash on a read is dropped and fetched
            // again. Dropped first: while the bad copy is stored the fetcher
            // counts the chunk as present and would never replace it.
            [fetcher, refs = _disk->_refs](const std::uint64_t& g, const kythira::chunk_sha& sha) {
                refs->store().drop_if_corrupt(sha);
                fetcher->want(g, {sha});
            });
        _front_end->set_write_refusal(refusal);
        // HTTP reads and writes count in the gateway's eviction order; the
        // gateway outlives the front end (stopped and reset first).
        _front_end->set_access_hook([gw = _gateway.get()](const std::string& key, bool removed) {
            if (removed) {
                gw->note_removed(key);
            } else {
                gw->note_access(key);
            }
        });
        _front_end->set_metrics_extra([d = _disk.get()] {
            const auto local = d->_guard.local().value_or(kythira::cache_node_report{});
            std::ostringstream out;
            out << "kythira_cache_chunks " << d->_refs->size() << '\n'
                << "kythira_cache_store_files " << d->_store.stats().files() << '\n'
                << "kythira_cache_fetch_pending_chunks " << d->_fetcher->pending() << '\n'
                << "kythira_cache_fs_free_bytes " << local._free_bytes << '\n'
                << "kythira_cache_fs_free_inodes " << local._free_inodes << '\n'
                << "kythira_cache_space_refusals " << d->_guard.refusals() << '\n';
            return out.str();
        });
    }

    /// The engine node's cache hooks.
    void set_engine_hooks() {
        auto* guard = _engine_guard.get();
        typename gateway_type::cache_hooks hooks;
        hooks._write_refusal = [guard](const std::uint64_t&, const std::vector<node_id_type>& v) {
            return guard->refusal(voter_names(v));
        };
        auto evicted_at = std::make_shared<std::pair<
            std::mutex, std::map<std::uint64_t, kythira::cache_disk_guard::clock::time_point>>>();
        hooks._excess_bytes = [guard, evicted_at](const std::uint64_t& group,
                                                  const std::vector<node_id_type>& v) {
            const auto now = kythira::cache_disk_guard::clock::now();
            const std::lock_guard lock(evicted_at->first);
            auto& last = evicted_at->second[group];
            const auto excess = guard->excess_bytes(voter_names(v), now, last);
            if (excess > 0) {
                last = now;
            }
            return excess;
        };
        hooks._advisory = [guard, refresh = _engine_refresh] {
            return guard->self() + ";" + refresh().encode();
        };
        hooks._on_advisory = [guard](std::string_view payload) {
            auto semi = payload.find(';');
            if (semi == std::string_view::npos) {
                return;
            }
            if (auto r = kythira::cache_node_report::decode(payload.substr(semi + 1))) {
                guard->record(std::string(payload.substr(0, semi)), *r);
            }
        };
        hooks._info = [guard, h = _host.get()] {
            const auto local = guard->local().value_or(kythira::cache_node_report{});
            std::uint64_t keys = 0;
            for (const auto& g : h->group_ids()) {
                if (auto* node = h->group_node(g)) {
                    keys += node->with_state_machine(
                        [](auto& sm) -> std::uint64_t { return sm.approximate_key_count(); });
                }
            }
            std::ostringstream out;
            out << "# Cache\r\n"
                << "cache_persistence:engine\r\n"
                << "cache_keys:" << keys << "\r\n"
                << "cache_store_bytes:" << local._store_bytes << "\r\n"
                << "cache_max_disk_bytes:" << guard->options()._max_disk_bytes << "\r\n"
                << "cache_fs_free_bytes:" << local._free_bytes << "\r\n"
                << "cache_fs_free_inodes:" << local._free_inodes << "\r\n"
                << "cache_space_refusals:" << guard->refusals() << "\r\n"
                << "cache_peer_reports:" << guard->fresh_peers() << "\r\n";
            return out.str();
        };
        _gateway->set_cache_hooks(std::move(hooks));
    }

    void start_threads() {
        _driving.store(true, std::memory_order_release);
        // The driver thread: tick the host, and run the gateway's
        // maintenance whenever the policy phase ran (Requirement 6.3: no
        // thread of its own).
        _driver = std::thread([this] {
            while (_driving.load(std::memory_order_acquire)) {
                auto report = _host->tick();
                if (report._policy_ran) {
                    if (_disk || _engine_refresh) {
                        // Fresh figures for the write gate and the budget
                        // before the gateway evicts or advises against them.
                        try {
                            if (_disk) {
                                _disk->refresh();
                            } else {
                                _engine_refresh();
                            }
                        } catch (const std::exception& e) {
                            _logger.log(kythira::log_level::warning, "cache: statvfs failed",
                                        {{"error", e.what()}});
                        }
                    }
                    try {
                        _gateway->run_maintenance();
                    } catch (const std::exception& e) {
                        _logger.log(kythira::log_level::warning,
                                    "redis gateway: maintenance failed", {{"error", e.what()}});
                    }
                }
                std::this_thread::sleep_for(_opt._tick_interval);
            }
        });

        // Pack compaction copies up to a pack's worth of records under the
        // store's lock, so it runs here rather than on the driver thread,
        // whose ticks drive elections (Requirement 6.8).
        if (_disk) {
            _compactor = std::thread([this] {
                std::unique_lock lock(_compactor_mu);
                while (_driving.load(std::memory_order_acquire)) {
                    _compactor_cv.wait_for(lock, _opt._policy_interval);
                    if (!_driving.load(std::memory_order_acquire)) {
                        break;
                    }
                    try {
                        if (auto n = _disk->_store.compact(); n > 0) {
                            _logger.log(kythira::log_level::info, "cache: compacted packs",
                                        {{"packs", std::to_string(n)}});
                        }
                    } catch (const std::exception& e) {
                        _logger.log(kythira::log_level::warning, "cache: pack compaction failed",
                                    {{"error", e.what()}});
                    }
                }
            });
        }
    }

    node_options _opt;
    kythira::redis_acl* _acl;
    const kythira::redis_gateway_config* _gateway_cfg;
    kythira::console_logger _logger;
    kythira::noop_metrics _metrics;
    std::vector<node_id_type> _voters;
    std::size_t _groups{0};

    /// The disk-backed node's store opens first and goes last.
    std::unique_ptr<disk_type> _disk;
    /// The engine node's guard (KYTHIRA_REDIS_GATEWAY_ENGINE builds only).
    std::unique_ptr<kythira::cache_disk_guard> _engine_guard;
    std::function<kythira::cache_node_report()> _engine_refresh;
    /// What the fetcher's callbacks read, cleared before the host goes.
    std::atomic<host_type*> _host_ptr{nullptr};
    std::unique_ptr<host_type> _host;
    std::unique_ptr<gateway_type> _gateway;
    std::unique_ptr<front_end_type> _front_end;

    std::atomic<bool> _driving{false};
    std::thread _driver;
    std::thread _compactor;
    std::mutex _compactor_mu;
    std::condition_variable _compactor_cv;
    bool _started{false};
};

template<typename Host, typename Storage> auto serving_factory_for() -> serving_factory<Host> {
    return [](const node_options& opt, kythira::redis_acl& acl,
              const kythira::redis_gateway_config& gateway_cfg, Host& node,
              const service_node::group_space& space) -> std::unique_ptr<serving> {
        return std::make_unique<host_serving<Host, Storage>>(opt, acl, gateway_cfg, node, space);
    };
}

}  // namespace kythira::redis_node
