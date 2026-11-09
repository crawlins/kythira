// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_persistence_bench_rows.hpp
/// @brief The persistence rows of the `multi-raft-performance` report
///        (`.kiro/specs/persistent-state-machine/` Requirements 4.4 and 9.4).
///
/// Each row runs the Redis gateway's state machine three ways on the message
/// fabric (Tier A), with everything else held fixed:
///
/// | arm               | state machine                                     |
/// |-------------------|---------------------------------------------------|
/// | `memory`          | `redis_kv_state_machine`: values in RAM           |
/// | `engine-memory`   | `persistent_kv_state_machine<memory_kv_engine>`   |
/// | `engine-rocksdb`  | `persistent_kv_state_machine<rocksdb_kv_engine>`  |
///
/// Every arm keeps its Raft logs in `binary_file_persistence_engine`, the
/// gateway's file and engine store, with fsync barriers on. The log is the
/// same in all three, so a difference between rows is the state machine's.
/// The `engine-memory` arm separates the engine path's own cost (the batch,
/// the applied-index key, compaction through `durable_applied_index`) from
/// RocksDB's.
///
/// ### Two kinds of row
///
/// * **write**: closed-loop SETs through the log, `_in_flight` at a time, on
///   a fresh cluster per repetition. Proposals/s and latency, as every other
///   write row reports them.
/// * **restart**: load `_state_bytes` once, then stop every host (drivers,
///   hosts, engines, the fabric) and build it again over the same
///   directories, once per repetition. The time runs from the first host's
///   construction until every shard has a leader, a fresh write has committed
///   on each, and the last loaded key reads back. That is what a client
///   waiting on a restarted node waits for. The in-memory arm restores its
///   last snapshot and replays the log tail; the engine arm opens the
///   database and reads its applied index (Requirement 4.1).
///
/// A restart row reports the restart time as its latency and as
/// `_restart_time`, and one restart per window as its rate, so the report's
/// five-repetition spread verdict applies to it unchanged.
///
/// ### Values
///
/// Values are pseudo-random bytes, so RocksDB's block compression cannot make
/// the engine arm's state smaller than the in-memory arm's.

#include "multi_raft_bench_row_runners.hpp"
#include "multi_raft_test_fabric.hpp"

#include <raft/binary_file_persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistent_state_machine.hpp>
#include <raft/shard_exceptions.hpp>
#include <kythira/redis/redis_kv_commands.hpp>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace kythira::testing {

namespace persistence_rows_detail {

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_type = std::uint64_t;

/// How long the drivers sleep between ticks; carried on the row.
inline constexpr std::chrono::milliseconds k_tick{2};
/// Log bytes between snapshots (in-memory arm) or between compactions
/// (engine arms), held equal so each arm's log stays the same size.
inline constexpr std::size_t k_snapshot_threshold = std::size_t{64} << 20U;

template<typename StateMachine>
inline constexpr bool k_engine = kythira::persistent_state_machine<StateMachine, std::uint64_t>;

template<typename StateMachine> struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type = fabric_client;
    using network_server_type = fabric_server;

    using persistence_engine_type =
        kythira::binary_file_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = StateMachine;

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

/// @brief The first key byte of shard `i`, as the transaction rows lay them
///        out: shard `i` owns `[letter(i), letter(i + 1))`.
inline auto shard_letter(std::size_t i) -> char {
    return static_cast<char>('b' + 2 * i);
}

/// @brief Key `n` on shard `s`.
inline auto key_for(std::size_t shard, std::size_t n) -> key_type {
    return std::string(1, shard_letter(shard)) + "/" + std::to_string(n);
}

/// @brief `bytes` of xorshift output seeded by `seed`: incompressible, and
///        different for every key.
inline auto value_bytes(std::size_t bytes, std::uint64_t seed) -> std::vector<std::byte> {
    std::vector<std::byte> out(bytes);
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ULL + 1;
    for (std::size_t i = 0; i < bytes; ++i) {
        if (i % 8 == 0) {
            x ^= x << 13U;
            x ^= x >> 7U;
            x ^= x << 17U;
        }
        out[i] = static_cast<std::byte>((x >> (8 * (i % 8))) & 0xFFU);
    }
    return out;
}

inline auto set_cmd(const key_type& k, std::size_t bytes, std::uint64_t seed)
    -> std::vector<std::byte> {
    return kythira::encode_redis_kv_command(
        kythira::redis_kv_set_command{k, value_bytes(bytes, seed), 0});
}

/// @brief A directory this row owns: created fresh, removed with the object.
class owned_directory {
public:
    explicit owned_directory(const std::filesystem::path& base)
        : _path((base.empty() ? std::filesystem::temp_directory_path() : base) /
                ("kythira-persistence-bench-" + std::to_string(::getpid()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(_path);
    }
    ~owned_directory() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    owned_directory(const owned_directory&) = delete;
    auto operator=(const owned_directory&) -> owned_directory& = delete;
    owned_directory(owned_directory&&) = delete;
    auto operator=(owned_directory&&) -> owned_directory& = delete;

    [[nodiscard]] auto path() const -> const std::filesystem::path& { return _path; }

private:
    std::filesystem::path _path;
};

/// @brief `hosts` hosts over one data root, each a voter of every shard.
///
/// `start()` builds the fabric, the engines and the hosts; `stop()` tears all
/// of them down and leaves the directories. A restart is the two in turn,
/// which is what a process restart does to everything but the disk.
template<typename StateMachine> class persistence_cluster {
public:
    using types = host_types<StateMachine>;
    using host_type = kythira::multi_raft<types, key_type, group_id_type>;
    using config_type = kythira::multi_raft_config<types, key_type, group_id_type>;
    using descriptor_type = kythira::shard_descriptor<group_id_type, key_type, std::uint64_t>;
    using engine_type = kythira::persistent_engine_of_t<StateMachine, std::uint64_t>;

    persistence_cluster(std::filesystem::path root, std::size_t hosts, std::size_t shards)
        : _root(std::move(root)), _host_count(hosts), _shards(shards) {}

    ~persistence_cluster() { stop(); }

    persistence_cluster(const persistence_cluster&) = delete;
    auto operator=(const persistence_cluster&) -> persistence_cluster& = delete;
    persistence_cluster(persistence_cluster&&) = delete;
    auto operator=(persistence_cluster&&) -> persistence_cluster& = delete;

    auto start() -> void {
        _fabric = std::make_unique<message_fabric>(8);
        std::vector<node_id_type> voters;
        for (node_id_type id = 1; id <= _host_count; ++id) {
            voters.push_back(id);
        }
        for (node_id_type id = 1; id <= _host_count; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
            auto& h = *_hosts.back();
            for (std::size_t s = 0; s < _shards; ++s) {
                kythira::shard_range<key_type> range;
                if (s > 0) {
                    range._start = key_type(1, shard_letter(s));
                }
                if (s + 1 < _shards) {
                    range._end = key_type(1, shard_letter(s + 1));
                }
                h.create_group(descriptor_type{._group_id = s + 1,
                                               ._range = range,
                                               ._epoch = kythira::shard_epoch{},
                                               ._voters = voters});
            }
        }
        for (auto& h : _hosts) {
            h->start();
        }
        _running = true;
        for (auto& h : _hosts) {
            _drivers.emplace_back([this, host = h.get()] {
                while (_running.load()) {
                    host->tick();
                    std::this_thread::sleep_for(k_tick);
                }
            });
        }
    }

    /// @brief Drivers, then hosts, then the fabric, then the engines: the
    ///        order nothing is still running against what goes next.
    auto stop() -> void {
        _running = false;
        for (auto& t : _drivers) {
            t.join();
        }
        _drivers.clear();
        for (auto& h : _hosts) {
            h->stop();
        }
        _fabric.reset();
        _hosts.clear();
        _engines.clear();
    }

    [[nodiscard]] auto await_leaders(std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            bool all = true;
            for (std::size_t s = 1; s <= _shards && all; ++s) {
                all = leader_of(s) != nullptr;
            }
            if (all) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return false;
    }

    /// @brief The host leading shard `group` (1-based), or null.
    [[nodiscard]] auto leader_of(group_id_type group) -> host_type* {
        for (auto& h : _hosts) {
            auto* n = h->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return h.get();
            }
        }
        return nullptr;
    }

    /// @brief SET `key`, retrying through leader changes until `deadline`.
    ///        True once it committed.
    auto put(const key_type& key, const std::vector<std::byte>& command,
             std::chrono::steady_clock::time_point deadline, operation_tally& tally) -> bool {
        const auto group = static_cast<group_id_type>(shard_of(key) + 1);
        while (std::chrono::steady_clock::now() < deadline) {
            auto* h = leader_of(group);
            if (h == nullptr) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                continue;
            }
            try {
                std::ignore = h->submit_command(key, command, k_operation_timeout).get();
                return true;
            } catch (const kythira::shard_not_leader_exception<group_id_type, node_id_type>&) {
                ++tally._not_leader;
            } catch (const std::exception&) {
                ++tally._other;
            }
        }
        ++tally._timeout;
        return false;
    }

    /// @brief The size of `key`'s value on its shard's leader, or nullopt
    ///        when no leader has applied an entry of its term yet or the key
    ///        is absent.
    [[nodiscard]] auto value_size(const key_type& key) -> std::optional<std::size_t> {
        const auto group = static_cast<group_id_type>(shard_of(key) + 1);
        auto* h = leader_of(group);
        if (h == nullptr) {
            return std::nullopt;
        }
        auto* n = h->group_node(group);
        if (n == nullptr) {
            return std::nullopt;
        }
        return n->with_state_machine([&](const StateMachine& sm) -> std::optional<std::size_t> {
            const auto handle = sm.lookup(key);
            if (!handle) {
                return std::nullopt;
            }
            return handle->_value.size();
        });
    }

    [[nodiscard]] auto shards() const -> std::size_t { return _shards; }

    [[nodiscard]] auto term_sum() -> std::uint64_t {
        std::uint64_t sum = 0;
        for (auto& h : _hosts) {
            for (std::size_t s = 1; s <= _shards; ++s) {
                if (auto* n = h->group_node(s); n != nullptr) {
                    sum += n->get_current_term();
                }
            }
        }
        return sum;
    }

private:
    [[nodiscard]] auto shard_of(const key_type& key) const -> std::size_t {
        for (std::size_t s = _shards; s-- > 1;) {
            if (key >= key_type(1, shard_letter(s))) {
                return s;
            }
        }
        return 0;
    }

    auto make_config(node_id_type id) -> config_type {
        const auto dir = _root / ("host-" + std::to_string(id));
        std::filesystem::create_directories(dir / "raft");
        config_type cfg{
            .node_id = id,
            .network_client = fabric_client{*_fabric, id},
            .network_server = fabric_server{*_fabric, id},
            .store_factory =
                [dir](const group_id_type& group) {
                    return typename types::persistence_engine_type{dir / "raft" /
                                                                   std::to_string(group)};
                },
        };
        // The engine-chaos timings: a follower fsyncs each batch before it
        // answers, so a short election timeout would elect on a slow disk.
        cfg.config._election_timeout_min = std::chrono::milliseconds{300};
        cfg.config._election_timeout_max = std::chrono::milliseconds{600};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{50};
        cfg.config._snapshot_threshold_bytes = k_snapshot_threshold;
        cfg.hibernation = kythira::hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.partitioner = kythira::make_partitioner<key_type>(kythira::redis_kv_partitioner{});
        cfg.allocate_group_ids = [this](std::size_t n) {
            const std::lock_guard lock(_ids_mutex);
            std::vector<group_id_type> out;
            for (std::size_t i = 0; i < n; ++i) {
                out.push_back(_next_id++);
            }
            return out;
        };
        cfg.automatic_split_merge_enabled = false;
        cfg.split_merge_interval = std::chrono::milliseconds{0};
        if constexpr (k_engine<StateMachine>) {
            std::filesystem::create_directories(dir / "snapshots");
            cfg.config._snapshot_staging_directory = dir / "snapshots";
            std::shared_ptr<engine_type> engine;
            if constexpr (std::is_default_constructible_v<engine_type>) {
                engine = std::make_shared<engine_type>();
            } else {
                std::filesystem::create_directories(dir / "engine");
                engine = std::make_shared<engine_type>(dir / "engine");
            }
            _engines.push_back(engine);
            cfg.state_engine = std::move(engine);
        }
        return cfg;
    }

    std::filesystem::path _root;
    std::size_t _host_count;
    std::size_t _shards;
    std::unique_ptr<message_fabric> _fabric;
    std::mutex _ids_mutex;
    group_id_type _next_id{100};
    std::vector<std::shared_ptr<void>> _engines;
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
};

/// @brief `count` SETs over `in_flight` workers, each SET on its own key
///        `first + i`; returns the window's duration.
template<typename StateMachine>
auto run_window(persistence_cluster<StateMachine>& cluster, std::size_t first, std::size_t count,
                std::size_t in_flight, std::size_t bytes, latency_sample_set& samples,
                operation_tally& tally) -> std::chrono::nanoseconds {
    std::atomic<std::size_t> next{0};
    std::mutex merge_mutex;
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (std::size_t w = 0; w < in_flight; ++w) {
        workers.emplace_back([&] {
            latency_sample_set mine;
            operation_tally own;
            for (std::size_t i = next++; i < count; i = next++) {
                ++own._offered;
                const auto n = first + i;
                const auto key = key_for(n % cluster.shards(), n);
                const auto cmd = set_cmd(key, bytes, n);
                const auto t0 = std::chrono::steady_clock::now();
                if (cluster.put(key, cmd, t0 + k_operation_timeout, own)) {
                    ++own._completed;
                    mine.record(std::chrono::steady_clock::now() - t0);
                }
            }
            const std::lock_guard lock(merge_mutex);
            tally.merge(own);
            samples.merge(mine);
        });
    }
    for (auto& t : workers) {
        t.join();
    }
    return std::chrono::steady_clock::now() - started;
}

template<typename StateMachine>
auto fill_common(benchmark_result& row, const persistence_row_spec& spec, std::string_view arm)
    -> void {
    row._tier = deployment_tier::a_fabric;
    row._transport = "fabric";
    row._serializer = "none";
    row._node_serializer = "application/json";
    row._nodes = spec._hosts;
    row._groups = spec._shards;
    row._value_bytes = spec._value_bytes;
    row._in_flight = spec._in_flight;
    row._tick_interval = k_tick;
    row._durability = durability_mode::file_barrier;
    row._state_machine = std::string(arm);
    // The fabric here is not the counted fabric the KV rows wrap.
    row._internal_counters = false;
}

/// @brief One write repetition: a fresh cluster, a discarded warm-up, one
///        window.
template<typename StateMachine>
auto one_write_measurement(const persistence_row_spec& spec, std::string_view arm,
                           const row_observer& observer) -> benchmark_result {
    const owned_directory dir{spec._data_dir};
    persistence_cluster<StateMachine> cluster{dir.path(), spec._hosts, spec._shards};
    cluster.start();
    observer._require(cluster.await_leaders(k_election_budget),
                      "persistence: no leader on every shard within budget");

    const auto warm = warmup_operations(spec._operations, spec._in_flight);
    {
        latency_sample_set ignored;
        operation_tally tally;
        std::ignore =
            run_window(cluster, 0, warm, spec._in_flight, spec._value_bytes, ignored, tally);
    }
    const auto terms_before = cluster.term_sum();
    benchmark_result row;
    latency_sample_set samples;
    samples.reserve(spec._operations);
    row._duration = run_window(cluster, warm, spec._operations, spec._in_flight, spec._value_bytes,
                               samples, row._tally);
    samples.sort();
    fill_common<StateMachine>(row, spec, arm);
    row._scenario = "write";
    row._p50 = samples.p50();
    row._p95 = samples.p95();
    row._p99 = samples.p99();
    const auto seconds = std::chrono::duration<double>(row._duration).count();
    row._ops_per_second =
        seconds > 0.0 ? static_cast<double>(row._tally._completed) / seconds : 0.0;
    const auto terms_after = cluster.term_sum();
    {
        std::ostringstream note;
        note << "    persistence: term sum " << terms_before << " -> " << terms_after
             << (terms_before == terms_after ? " (steady)" : " (AN ELECTION OCCURRED)");
        observer._message(note.str());
    }
    observer._check(row._tally._completed == row._tally._offered,
                    "persistence: " + std::to_string(row._tally._offered - row._tally._completed) +
                        " of " + std::to_string(row._tally._offered) + " did not commit");
    return row;
}

/// @brief Restart `cluster` once and time it to the first served write and
///        read on every shard.
template<typename StateMachine>
auto one_restart(persistence_cluster<StateMachine>& cluster, const persistence_row_spec& spec,
                 std::size_t loaded, std::size_t probe, std::chrono::nanoseconds& opened,
                 const row_observer& observer) -> std::chrono::nanoseconds {
    cluster.stop();
    const auto t0 = std::chrono::steady_clock::now();
    cluster.start();
    // Construction and `start()` are where a host reads its storage: the
    // in-memory arm's snapshot restore and log replay, the engine arm's
    // database open. What follows is the election and one commit.
    opened = std::chrono::steady_clock::now() - t0;
    const auto deadline = t0 + k_election_budget;
    for (std::size_t s = 0; s < spec._shards; ++s) {
        operation_tally ignored;
        const auto key = key_for(s, probe + s);
        observer._require(cluster.put(key, set_cmd(key, 16, probe + s), deadline, ignored),
                          "persistence: a restarted shard did not commit a write in budget");
    }
    // The last key loaded on each shard, which the in-memory arm has only if
    // it restored its snapshot and replayed its log, and the engine arm only
    // if its database survived.
    for (std::size_t s = 0; s < spec._shards; ++s) {
        const auto n = loaded - 1 - ((loaded - 1 + spec._shards - s) % spec._shards);
        const auto key = key_for(s, n);
        std::optional<std::size_t> size;
        while (!(size = cluster.value_size(key)).has_value() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        observer._require(size == spec._value_bytes,
                          "persistence: key " + key + " did not survive the restart");
    }
    return std::chrono::steady_clock::now() - t0;
}

/// @brief The restart row: one load, then `k_required_repetitions` restarts.
template<typename StateMachine>
auto restart_row(const persistence_row_spec& spec, std::string_view arm,
                 const row_observer& observer) -> repeated_result {
    const owned_directory dir{spec._data_dir};
    persistence_cluster<StateMachine> cluster{dir.path(), spec._hosts, spec._shards};
    cluster.start();
    observer._require(cluster.await_leaders(k_election_budget),
                      "persistence: no leader on every shard within budget");

    const auto loaded = static_cast<std::size_t>(
        std::max<std::uint64_t>(spec._shards, spec._state_bytes / spec._value_bytes));
    {
        latency_sample_set ignored;
        operation_tally tally;
        const auto t0 = std::chrono::steady_clock::now();
        std::ignore =
            run_window(cluster, 0, loaded, spec._in_flight, spec._value_bytes, ignored, tally);
        const auto seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::ostringstream note;
        note << std::fixed << std::setprecision(1) << "    persistence: loaded " << loaded << " x "
             << spec._value_bytes << " B ("
             << static_cast<double>(loaded * spec._value_bytes) / (1024.0 * 1024.0) << " MiB) in "
             << seconds << " s";
        observer._message(note.str());
        observer._require(tally._completed == loaded, "persistence: the load committed only " +
                                                          std::to_string(tally._completed) +
                                                          " of " + std::to_string(loaded));
    }

    repeated_result out;
    out._warmup_operations = 0;
    out._measured_operations = 1;
    for (std::size_t repetition = 0; repetition < k_required_repetitions; ++repetition) {
        std::chrono::nanoseconds opened{0};
        const auto took = one_restart(cluster, spec, loaded, loaded + repetition * spec._shards,
                                      opened, observer);
        benchmark_result row;
        fill_common<StateMachine>(row, spec, arm);
        row._scenario = "restart";
        row._in_flight = 1;
        row._duration = took;
        row._restart_time = took;
        row._state_bytes = static_cast<std::uint64_t>(loaded) * spec._value_bytes;
        row._p50 = took;
        row._p95 = took;
        row._tally._offered = 1;
        row._tally._completed = 1;
        const auto seconds = std::chrono::duration<double>(took).count();
        row._ops_per_second = seconds > 0.0 ? 1.0 / seconds : 0.0;
        std::ostringstream note;
        note << std::fixed << std::setprecision(1) << "    restart " << (repetition + 1) << "/"
             << k_required_repetitions << ": "
             << std::chrono::duration<double, std::milli>(took).count() << " ms (storage "
             << std::chrono::duration<double, std::milli>(opened).count() << " ms)";
        observer._message(note.str());
        out.record(std::move(row));
    }
    return out;
}

}  // namespace persistence_rows_detail

/// @brief One persistence row over `StateMachine`, labelled `arm`.
template<typename StateMachine>
auto persistence_row(const persistence_row_spec& spec, std::string_view arm,
                     const row_observer& observer) -> repeated_result {
    if (spec._kind == persistence_row_kind::restart) {
        return persistence_rows_detail::restart_row<StateMachine>(spec, arm, observer);
    }
    repeated_result row;
    row._warmup_operations = warmup_operations(spec._operations, spec._in_flight);
    row._measured_operations = spec._operations;
    for (std::size_t repetition = 0; repetition < k_required_repetitions; ++repetition) {
        auto run =
            persistence_rows_detail::one_write_measurement<StateMachine>(spec, arm, observer);
        observer._message(format_repetition(repetition, run));
        row.record(std::move(run));
    }
    return row;
}

}  // namespace kythira::testing
