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
#include <raft/binary_file_persistence.hpp>
#include <raft/bounded_resolver.hpp>
#include <raft/cache_chunk_fetcher.hpp>
#include <raft/cache_chunk_store.hpp>
#include <raft/cache_disk_guard.hpp>
#include <raft/cache_http_front_end.hpp>
#include <raft/cache_kv_state_machine.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/redis_gateway_impl.hpp>
#include <raft/redis_kv_state_machine.hpp>

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
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::redis_node {

template<typename Serializer> struct beast_stack {
    using bundle = kythira::testing::harness_transport_types<Serializer>;
    using client_type = kythira::boost_beast_client<bundle, kythira::host_node_id>;
    using server_type = kythira::boost_beast_server<bundle, kythira::host_node_id>;

    explicit beast_stack(const node_options& opt)
        : _work(boost::asio::make_work_guard(*_ioc)),
          _server(*_ioc, opt._bind_address, opt._raft_port, kythira::boost_beast_server_config{},
                  kythira::noop_metrics{}),
          _client(*_ioc,
                  std::unordered_map<kythira::host_node_id, std::string>{opt._peers.begin(),
                                                                         opt._peers.end()},
                  kythira::boost_beast_client_config{}, kythira::noop_metrics{}, _ioc) {
        // A peer that speaks to this node has told it where it is: how a
        // gateway finds the shard leader to forward to while its name does
        // not resolve, e.g. right after the gateways restart at new
        // addresses under rootless Podman (see bounded_resolver.hpp).
        // The Raft client learns the peer's Raft address; the forward link
        // dials the peer's gateway listener on another port, so that address
        // is learned here from the same sighting.
        _server.set_peer_seen_handler(
            [raft = _client.peer_seen_handler(), gateways = opt._peer_gateways](
                const kythira::host_node_id& id, const sockaddr_storage& from, socklen_t len) {
                raft(id, from, len);
                auto it = gateways.find(id);
                if (it == gateways.end()) {
                    return;
                }
                try {
                    auto [host, port] = kythira::redis_gateway_detail::parse_listen(it->second);
                    if (auto addr =
                            kythira::net_resolve::address_from(from, len, port, SOCK_STREAM)) {
                        kythira::net_resolve::learn(host, std::to_string(port), SOCK_STREAM, *addr);
                    }
                } catch (const std::exception&) {
                    // A malformed peer gateway spec is reported when it is
                    // forwarded to; a sighting is not the place to fail.
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

/// Values and Raft state in RAM: the gateway before the disk-backed cache.
struct memory_storage {
    static constexpr bool on_disk = false;
    using state_machine_type = kythira::redis_kv_state_machine<std::uint64_t>;
    using persistence_engine_type =
        kythira::memory_persistence_engine<kythira::host_node_id, std::uint64_t, std::uint64_t>;
};

/// Values in the node's chunk store, Raft state in the binary segmented
/// engine (.kiro/specs/disk-backed-build-cache/).
struct file_storage {
    static constexpr bool on_disk = true;
    using state_machine_type = kythira::cache_kv_state_machine<std::uint64_t>;
    using persistence_engine_type =
        kythira::binary_file_persistence_engine<kythira::host_node_id, std::uint64_t,
                                                std::uint64_t>;
};

template<typename Stack, typename Storage> struct host_types {
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
        kythira::testing::transport_client_handle<typename Stack::client_type, node_id_type>;
    using network_server_type =
        kythira::testing::transport_server_handle<typename Stack::server_type, node_id_type>;

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

/// Build, start, serve, shut down — in the order that does not terminate the
/// process (cmd/multi_raft_node/host_stacks.hpp's `run_host` ordering).
template<typename Serializer, typename Storage> auto run_host(const node_options& opt) -> int {
    using stack_type = beast_stack<Serializer>;
    using types = host_types<stack_type, Storage>;
    using node_id_type = typename types::node_id_type;
    using host_type = kythira::multi_raft<types, std::string, std::uint64_t>;
    using config_type = kythira::multi_raft_config<types, std::string, std::uint64_t>;
    using descriptor_type = kythira::shard_descriptor<std::uint64_t, std::string, node_id_type>;
    using gateway_type =
        kythira::redis_gateway<host_type, kythira::console_logger, kythira::noop_metrics>;
    using disk_type = disk_node<node_id_type, std::uint64_t>;
    using front_end_type = kythira::cache_http_front_end<host_type, kythira::console_logger>;

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

    // The disk-backed node's store opens before anything binds a port, so a
    // data directory that cannot be used fails start-up cleanly.
    std::unique_ptr<disk_type> disk;
    host_type* host_ptr = nullptr;
    if constexpr (Storage::on_disk) {
        std::filesystem::create_directories(opt._cache._data_dir / "raft");
        disk = std::make_unique<disk_type>(opt._cache, node_text(opt._node_id));
        disk->refresh();
        kythira::cache_chunk_fetcher_options fo;
        fo._bytes_per_sec = opt._cache._fetch_bytes_per_sec;
        disk->_fetcher =
            std::make_unique<kythira::cache_chunk_fetcher<node_id_type, std::uint64_t>>(
                disk->_refs,
                // The shard's leader first, then its other voters.
                [&host_ptr](const std::uint64_t& group) {
                    std::vector<node_id_type> out;
                    if (host_ptr == nullptr) {
                        return out;
                    }
                    auto* node = host_ptr->group_node(group);
                    auto desc = host_ptr->local_descriptor(group);
                    if (node != nullptr) {
                        if (auto leader = node->known_leader();
                            leader && *leader != host_ptr->node_id()) {
                            out.push_back(*leader);
                        }
                    }
                    if (desc) {
                        for (const auto& v : desc->voters()) {
                            if (v != host_ptr->node_id() &&
                                std::find(out.begin(), out.end(), v) == out.end()) {
                                out.push_back(v);
                            }
                        }
                    }
                    return out;
                },
                // Bound to the front end once it exists; until then every fetch
                // misses and is retried.
                [](const node_id_type&, const kythira::chunk_sha&)
                    -> std::optional<std::vector<std::byte>> { return std::nullopt; },
                [&host_ptr](const std::uint64_t& group) {
                    if (host_ptr == nullptr) {
                        return;
                    }
                    if (auto* node = host_ptr->group_node(group)) {
                        (void)node->with_state_machine(
                            [](auto& sm) { return sm.refresh_has_all(); });
                    }
                },
                fo);
    }

    stack_type stack{opt};

    std::vector<node_id_type> voters;
    for (const auto& [id, url] : opt._peers) {
        voters.push_back(id);
    }

    config_type cfg{
        .node_id = opt._node_id,
        .network_client = typename types::network_client_type{stack._client},
        .network_server = typename types::network_server_type{stack._server},
        .store_factory =
            [&opt](const std::uint64_t& group) {
                if constexpr (Storage::on_disk) {
                    return typename types::persistence_engine_type{opt._cache._data_dir / "raft" /
                                                                   std::to_string(group)};
                } else {
                    // A cache: in-memory persistence is the honest choice here.
                    // A replica that restarts catches up from its peers.
                    (void)opt;
                    (void)group;
                    return typename types::persistence_engine_type{};
                }
            },
    };
    if constexpr (Storage::on_disk) {
        cfg.state_machine_factory = [&disk, &opt](const std::uint64_t& group) {
            kythira::cache_kv_options o;
            o._orphan_grace_entries = opt._cache._orphan_grace_entries;
            // Called under the node lock: the fetcher only enqueues.
            auto* fetcher = disk->_fetcher.get();
            o._on_missing_chunks = [fetcher, group](std::vector<kythira::chunk_sha> missing) {
                fetcher->want(group, missing);
            };
            return typename types::state_machine_type(disk->_refs, std::move(o));
        };
        cfg.config._snapshot_threshold_bytes = opt._cache._snapshot_threshold_bytes;
    }
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
    host_ptr = host.get();

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

    // Each group restores its snapshot inside start(), so once it returns
    // the node-wide counts are complete and any chunk they do not name is
    // an orphan (Requirement 2.6).
    host->start();
    if constexpr (Storage::on_disk) {
        const auto removed = disk->_refs->collect_orphans();
        disk->_orphans_collected += removed;
        logger.log(kythira::log_level::info, "cache: start-up orphan collection",
                   {{"removed", std::to_string(removed)},
                    {"referenced", std::to_string(disk->_refs->size())}});
        disk->_pinned = [h = host.get()] {
            std::uint64_t pinned = 0;
            for (const auto& g : h->group_ids()) {
                if (auto* node = h->group_node(g)) {
                    pinned += node->with_state_machine([](auto& sm) { return sm.pending_bytes(); });
                }
            }
            return pinned;
        };
    }

    auto peer_gateways = opt._peer_gateways;
    gateway_type gateway(*host, acl, logger, metrics, gateway_cfg,
                         [peer_gateways](const node_id_type& id) -> std::optional<std::string> {
                             auto it = peer_gateways.find(id);
                             if (it == peer_gateways.end()) {
                                 return std::nullopt;
                             }
                             return it->second;
                         });

    std::unique_ptr<front_end_type> front_end;
    if constexpr (Storage::on_disk) {
        auto voter_names = [](const std::vector<node_id_type>& v) {
            std::vector<std::string> out;
            out.reserve(v.size());
            for (const auto& id : v) {
                out.push_back(node_text(id));
            }
            return out;
        };
        auto* guard = &disk->_guard;
        auto refusal = [guard, voter_names](const std::uint64_t&,
                                            const std::vector<node_id_type>& v) {
            return guard->refusal(voter_names(v));
        };
        typename gateway_type::cache_hooks hooks;
        hooks._write_refusal = refusal;
        // The last time each shard was told to evict for the disk budget:
        // reports older than that still count the bytes it freed. Only the
        // maintenance thread asks, but under a mutex all the same.
        auto evicted_at = std::make_shared<std::pair<
            std::mutex, std::map<std::uint64_t, kythira::cache_disk_guard::clock::time_point>>>();
        hooks._excess_bytes = [guard, voter_names, evicted_at](const std::uint64_t& group,
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
        hooks._advisory = [d = disk.get()] {
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
        hooks._info = [d = disk.get(), h = host.get()] {
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
        gateway.set_cache_hooks(std::move(hooks));

        kythira::cache_http_config hc;
        hc._listen = opt._cache._http_listen;
        hc._tls_listen = opt._cache._https_listen;
        hc._allow_plaintext = opt._cache._allow_plaintext_http;
        hc._tls_cert_path = opt._cache._tls_cert_path;
        hc._tls_key_path = opt._cache._tls_key_path;
        hc._chunk_bytes = opt._cache._chunk_bytes;
        hc._max_value_bytes = opt._cache._max_value_bytes;
        hc._min_free_bytes = opt._cache._min_free_bytes;
        hc._min_free_inodes = opt._cache._min_free_inodes;
        hc._auth_cache_ttl = opt._cache._auth_cache_ttl;
        hc._internal_secret = gateway_cfg._internal_secret;
        hc._forwarding = gateway_cfg._forwarding;
        hc._forward_tls = gateway_cfg._forward_tls;
        hc._allow_plaintext_forwarding = gateway_cfg._allow_plaintext_forwarding;
        hc._forward_tls_ca_path = gateway_cfg._forward_tls_ca_path.empty()
                                      ? gateway_cfg._tls_ca_path
                                      : gateway_cfg._forward_tls_ca_path;
        hc._command_timeout = gateway_cfg._command_timeout;
        auto peer_http = opt._cache._peer_http;
        auto* fetcher = disk->_fetcher.get();
        front_end = std::make_unique<front_end_type>(
            *host, acl, logger, disk->_refs, hc,
            [peer_http](const node_id_type& id) -> std::optional<std::string> {
                auto it = peer_http.find(id);
                if (it == peer_http.end()) {
                    return std::nullopt;
                }
                return it->second;
            },
            // A chunk that failed its hash on a read is fetched again.
            [fetcher](const std::uint64_t& g, const kythira::chunk_sha& sha) {
                fetcher->want(g, {sha});
            });
        front_end->set_write_refusal(refusal);
        // HTTP reads and writes count in the gateway's eviction order; the
        // gateway outlives the front end (stopped and reset first).
        front_end->set_access_hook([gw = &gateway](const std::string& key, bool removed) {
            if (removed) {
                gw->note_removed(key);
            } else {
                gw->note_access(key);
            }
        });
        front_end->set_metrics_extra([d = disk.get()] {
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

    gateway.start();
    if constexpr (Storage::on_disk) {
        if (!opt._cache._http_listen.empty() || !opt._cache._https_listen.empty()) {
            front_end->start();
        }
        auto* fe = front_end.get();
        disk->_fetcher->set_fetch([fe](const node_id_type& peer, const kythira::chunk_sha& sha) {
            return fe->fetch_chunk(peer, sha);
        });
        disk->_fetcher->start();
    }

    // The driver thread: tick the host, and run the gateway's maintenance
    // whenever the policy phase ran (Requirement 6.3: no thread of its own).
    std::atomic<bool> driving{true};
    std::thread driver([&] {
        while (driving.load(std::memory_order_acquire)) {
            auto report = host->tick();
            if (report._policy_ran) {
                if (disk) {
                    // Fresh figures for the write gate and the budget
                    // before the gateway evicts or advises against them.
                    try {
                        disk->refresh();
                    } catch (const std::exception& e) {
                        logger.log(kythira::log_level::warning, "cache: statvfs failed",
                                   {{"error", e.what()}});
                    }
                }
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

    // Pack compaction copies up to a pack's worth of records under the
    // store's lock, so it runs here rather than on the driver thread, whose
    // ticks drive elections (Requirement 6.8).
    std::thread compactor;
    std::mutex compactor_mu;
    std::condition_variable compactor_cv;
    if (disk) {
        compactor = std::thread([&] {
            std::unique_lock lock(compactor_mu);
            while (driving.load(std::memory_order_acquire)) {
                compactor_cv.wait_for(lock, opt._policy_interval);
                if (!driving.load(std::memory_order_acquire)) {
                    break;
                }
                try {
                    if (auto n = disk->_store.compact(); n > 0) {
                        logger.log(kythira::log_level::info, "cache: compacted packs",
                                   {{"packs", std::to_string(n)}});
                    }
                } catch (const std::exception& e) {
                    logger.log(kythira::log_level::warning, "cache: pack compaction failed",
                               {{"error", e.what()}});
                }
            }
        });
    }

    std::cout << "redis_gateway_node " << opt._node_id << " ready: raft=" << opt._raft_port
              << " redis=" << gateway.port() << " tls=" << gateway.tls_port()
              << " shards=" << (cuts.size() + 1) << " serializer=" << Serializer{}.media_type()
              << " persistence=" << (Storage::on_disk ? "file" : "memory");
    if constexpr (Storage::on_disk) {
        std::cout << " http=" << front_end->port() << " https=" << front_end->tls_port();
    }
    std::cout << std::endl;

    wait_for_stop();

    // The ordering. Nothing above this line may be reordered below it.
    if constexpr (Storage::on_disk) {
        front_end->stop();
    }
    gateway.stop();
    {
        const std::lock_guard lock(compactor_mu);
        driving.store(false, std::memory_order_release);
    }
    compactor_cv.notify_all();
    if (driver.joinable()) {
        driver.join();
    }
    if (compactor.joinable()) {
        compactor.join();
    }
    if (disk) {
        disk->_fetcher->stop();
        disk->_pinned = nullptr;  // it reads the host
    }
    front_end.reset();
    host->stop();
    host_ptr = nullptr;
    host.reset();
    stack.shutdown();
    return 0;
}

}  // namespace kythira::redis_node
