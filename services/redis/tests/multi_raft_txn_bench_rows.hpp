// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_txn_bench_rows.hpp
/// @brief The transaction rows of the `multi-raft-performance` report
///        (`.kiro/specs/multi-raft-transactions/` Requirement 13.6).
///
/// Each row measures one transaction shape against three in-process hosts on
/// the message fabric (Tier A): one-phase commit on one shard, two-phase
/// commit over two and four shards, and read-only snapshot transactions. The
/// rows exist so each isolation tier's cost is a number, not a guess. They
/// are Tier A on purpose: the fabric removes the socket, so what is left is
/// the protocol's own round trips (prepare, timestamp, primary commit), which
/// is the part a transaction adds over a plain write.
///
/// ### Shape
///
/// `txn_row<StateMachine>` is instantiated once per translation unit, the
/// rule `multi_raft_bench_row_runners.hpp` sets for every row: one for the
/// tier-1 wrapper over `redis_kv_state_machine` (`mvcc::off`, isolation
/// `read_committed`) in `bench_rows/txn_fabric.cpp`, and one for the tier-2
/// wrapper over `redis_kv_mvcc_state_machine` (`mvcc::on`, isolation
/// `snapshot`, with an oracle group) in `bench_rows/txn_snapshot_fabric.cpp`.
///
/// ### What a row does not count
///
/// The fabric here is not the counted fabric the KV rows wrap, so a
/// transaction row carries no RPC or barrier counters and says so with
/// `_internal_counters = false`. The artifacts then write those columns empty
/// rather than zero.
///
/// ### Contention
///
/// Each worker owns its own keys, so no two transactions in a window touch the
/// same key. A row measures the protocol's cost, not its conflict rate: a
/// conflict-heavy row would measure the retry policy and the key distribution
/// as much as the commit path.

#include "multi_raft_bench_row_runners.hpp"
#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/multi_raft_txn_client.hpp>
#include <raft/multi_version_state_machine.hpp>
#include <raft/persistence.hpp>
#include <kythira/redis/redis_kv_commands.hpp>
#include <raft/txn_coordinator.hpp>
#include <raft/txn_state_machine.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace kythira::testing {

namespace txn_rows_detail {

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_type = std::uint64_t;

/// The oracle's group id: above every data group the cluster creates.
inline constexpr group_id_type k_oracle_group = 1000;
/// How long the drivers sleep between ticks; carried on the row.
inline constexpr std::chrono::milliseconds k_tick{5};
/// The cluster size every row uses.
inline constexpr std::size_t k_hosts = 3;

/// `mvcc::on` exactly when the state machine keeps versions.
template<typename StateMachine>
inline constexpr kythira::mvcc k_mvcc_of =
    kythira::multi_version_state_machine<StateMachine, key_type> ? kythira::mvcc::on
                                                                 : kythira::mvcc::off;

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
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type =
        kythira::transactional_state_machine<StateMachine, key_type, kythira::redis_kv_partitioner,
                                             k_mvcc_of<StateMachine>>;

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

/// @brief The first key byte of shard `i`. Shard `i` owns the keys that start
///        with this letter: its range is `[letter(i), letter(i + 1))`, open at
///        both ends of the keyspace.
inline auto shard_letter(std::size_t i) -> char {
    return static_cast<char>('b' + 2 * i);
}

/// @brief Key `n` of worker `w` on shard `s`. Workers never share keys.
inline auto key_for(std::size_t shard, std::size_t worker, std::size_t n) -> key_type {
    return std::string(1, shard_letter(shard)) + "/w" + std::to_string(worker) + "/" +
           std::to_string(n);
}

inline auto set_cmd(const key_type& k, std::size_t bytes) -> std::vector<std::byte> {
    return kythira::encode_redis_kv_command(
        kythira::redis_kv_set_command{k, std::vector<std::byte>(bytes, std::byte{'v'}), 0});
}

/// @brief Three hosts, `shards` data groups and, under `mvcc::on`, an oracle
///        group; every host a voter of each.
template<typename StateMachine> class txn_cluster {
public:
    using types = host_types<StateMachine>;
    using host_type = kythira::multi_raft<types, key_type, group_id_type>;
    using config_type = kythira::multi_raft_config<types, key_type, group_id_type>;
    using descriptor_type = kythira::shard_descriptor<group_id_type, key_type, std::uint64_t>;
    using client_type = kythira::multi_raft_txn_client<types, key_type, group_id_type>;
    using coordinator_type = kythira::transaction_coordinator<client_type, key_type>;
    static constexpr bool k_versioned = k_mvcc_of<StateMachine> == kythira::mvcc::on;

    explicit txn_cluster(std::size_t shards) : _shards(shards) {
        std::vector<node_id_type> voters;
        for (node_id_type id = 1; id <= k_hosts; ++id) {
            voters.push_back(id);
        }
        for (node_id_type id = 1; id <= k_hosts; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
            auto& h = *_hosts.back();
            for (std::size_t s = 0; s < shards; ++s) {
                kythira::shard_range<key_type> range;
                if (s > 0) {
                    range._start = key_type(1, shard_letter(s));
                }
                if (s + 1 < shards) {
                    range._end = key_type(1, shard_letter(s + 1));
                }
                h.create_group(descriptor_type{._group_id = s + 1,
                                               ._range = range,
                                               ._epoch = kythira::shard_epoch{},
                                               ._voters = voters});
            }
            if constexpr (k_versioned) {
                h.create_oracle_group(voters);
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
        std::vector<host_type*> raw;
        raw.reserve(_hosts.size());
        for (auto& h : _hosts) {
            raw.push_back(h.get());
        }
        if constexpr (k_versioned) {
            _client = std::make_unique<client_type>(std::move(raw), value_of, value_at);
        } else {
            _client = std::make_unique<client_type>(std::move(raw), value_of);
        }
        for (auto& h : _hosts) {
            h->set_txn_forwarder([this](const std::vector<key_type>& keys,
                                        const std::vector<std::byte>& env,
                                        std::chrono::milliseconds timeout) {
                return _client->send(keys, env, timeout).get();
            });
            if constexpr (k_versioned) {
                h->set_timestamp_forwarder(
                    [this](std::size_t n, std::chrono::milliseconds timeout) {
                        return _client->allocate_timestamps(n, timeout);
                    });
            }
        }
    }

    ~txn_cluster() {
        _running = false;
        for (auto& t : _drivers) {
            t.join();
        }
        _client.reset();
        for (auto& h : _hosts) {
            h->stop();
        }
    }

    txn_cluster(const txn_cluster&) = delete;
    auto operator=(const txn_cluster&) -> txn_cluster& = delete;
    txn_cluster(txn_cluster&&) = delete;
    auto operator=(txn_cluster&&) -> txn_cluster& = delete;

    [[nodiscard]] auto client() -> client_type& { return *_client; }

    [[nodiscard]] auto await_leaders(std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            bool all = true;
            for (std::size_t s = 1; s <= _shards && all; ++s) {
                all = has_leader(s);
            }
            if (all && (!k_versioned || has_leader(k_oracle_group))) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

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
    static auto bytes_or_none(const typename StateMachine::entry_handle& h)
        -> std::optional<std::vector<std::byte>> {
        if (!h) {
            return std::nullopt;
        }
        return h->_value;
    }
    static auto value_of(const StateMachine& sm, const key_type& k)
        -> std::optional<std::vector<std::byte>> {
        return bytes_or_none(sm.lookup(k));
    }
    static auto value_at(const StateMachine& sm, const key_type& k, std::uint64_t ts)
        -> std::optional<std::vector<std::byte>>
    requires(k_versioned)
    {
        return bytes_or_none(sm.read_at(k, ts));
    }

    [[nodiscard]] auto has_leader(group_id_type group) -> bool {
        return std::ranges::any_of(_hosts, [group](const auto& h) {
            auto* n = h->group_node(group);
            return n != nullptr && n->is_leader();
        });
    }

    auto make_config(node_id_type id) -> config_type {
        config_type cfg{
            .node_id = id,
            .network_client = fabric_client{_fabric, id},
            .network_server = fabric_server{_fabric, id},
            .store_factory =
                [](const group_id_type&) { return typename types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{150};
        cfg.config._election_timeout_max = std::chrono::milliseconds{300};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{30};
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
        if constexpr (k_versioned) {
            cfg.oracle_group = k_oracle_group;
        }
        // A measurement window must not include a split the driver did not
        // ask for.
        cfg.automatic_split_merge_enabled = false;
        cfg.split_merge_interval = std::chrono::milliseconds{0};
        return cfg;
    }

    std::size_t _shards;
    message_fabric _fabric{8};
    std::mutex _ids_mutex;
    group_id_type _next_id{100};
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
    std::unique_ptr<client_type> _client;
};

/// @brief The isolation each row's wrapper is measured at: the weakest its
///        tier offers, which is the tier's own cost.
template<typename StateMachine> constexpr auto row_isolation() -> kythira::txn_isolation {
    return k_mvcc_of<StateMachine> == kythira::mvcc::on ? kythira::txn_isolation::snapshot
                                                        : kythira::txn_isolation::read_committed;
}

/// @brief Run `count` transactions over `in_flight` workers; returns each
///        completed one's latency, and the tally.
template<typename StateMachine>
auto run_window(txn_cluster<StateMachine>& cluster, const txn_row_spec& spec, std::size_t count,
                latency_sample_set& samples, operation_tally& tally) -> std::chrono::nanoseconds {
    using coordinator_type = typename txn_cluster<StateMachine>::coordinator_type;
    kythira::txn_options options;
    options.isolation = row_isolation<StateMachine>();
    options.timeout = k_operation_timeout;

    std::atomic<std::size_t> next{0};
    std::mutex merge_mutex;
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (std::size_t w = 0; w < spec._in_flight; ++w) {
        workers.emplace_back([&, w] {
            latency_sample_set mine;
            operation_tally own;
            // Writers own their keys; readers all read the preloaded set.
            const std::size_t owner = spec._kind == txn_row_kind::write ? w : 0;
            for (std::size_t i = next++; i < count; i = next++) {
                ++own._offered;
                const auto t0 = std::chrono::steady_clock::now();
                try {
                    coordinator_type txn(cluster.client(), options);
                    txn.begin();
                    for (std::size_t k = 0; k < spec._keys_per_txn; ++k) {
                        const auto key = key_for(k % spec._shards, owner, i % 64 + k);
                        if (spec._kind == txn_row_kind::write) {
                            txn.put(key, set_cmd(key, spec._value_bytes));
                        } else {
                            std::ignore = txn.get(key);
                        }
                    }
                    const auto out = txn.commit();
                    const auto t1 = std::chrono::steady_clock::now();
                    // Secondary commits finish in the background; waiting for
                    // them is cleanup, not the latency a caller sees.
                    txn.drain();
                    if (out._kind == kythira::txn_outcome_kind::committed) {
                        ++own._completed;
                        mine.record(t1 - t0);
                    } else {
                        ++own._other;
                    }
                } catch (const std::exception&) {
                    ++own._other;
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

/// @brief One repetition: a fresh cluster, a discarded warm-up, one window.
template<typename StateMachine>
auto one_txn_measurement(const txn_row_spec& spec, const row_observer& observer)
    -> benchmark_result {
    txn_cluster<StateMachine> cluster{spec._shards};
    observer._require(cluster.await_leaders(k_election_budget),
                      "transactions: no leader on every shard within budget");

    // A read row reads keys that exist, so the read path is the one measured.
    if (spec._kind == txn_row_kind::snapshot_read) {
        txn_row_spec load = spec;
        load._kind = txn_row_kind::write;
        load._in_flight = 1;
        latency_sample_set ignored;
        operation_tally preload;
        std::ignore = run_window(cluster, load, 64, ignored, preload);
        observer._require(preload._completed == 64, "transactions: preload committed only " +
                                                        std::to_string(preload._completed) +
                                                        " of 64");
    }

    const auto terms_before = cluster.term_sum();
    {
        latency_sample_set ignored;
        operation_tally warm;
        std::ignore = run_window(
            cluster, spec, warmup_operations(spec._operations, spec._in_flight), ignored, warm);
    }

    benchmark_result row;
    latency_sample_set samples;
    samples.reserve(spec._operations);
    row._duration = run_window(cluster, spec, spec._operations, samples, row._tally);
    samples.sort();

    row._tier = deployment_tier::a_fabric;
    row._transport = "fabric";
    row._serializer = "none";
    row._node_serializer = "application/json";
    row._scenario = std::string(spec._kind == txn_row_kind::write ? "txn-write/" : "txn-read/") +
                    kythira::to_string(row_isolation<StateMachine>());
    row._nodes = k_hosts;
    row._groups = spec._shards;
    row._value_bytes = spec._value_bytes;
    row._in_flight = spec._in_flight;
    row._tick_interval = k_tick;
    row._p50 = samples.p50();
    row._p95 = samples.p95();
    row._p99 = samples.p99();
    const auto seconds = std::chrono::duration<double>(row._duration).count();
    row._ops_per_second =
        seconds > 0.0 ? static_cast<double>(row._tally._completed) / seconds : 0.0;
    row._internal_counters = false;

    const auto terms_after = cluster.term_sum();
    {
        std::ostringstream note;
        note << "    transactions: term sum " << terms_before << " -> " << terms_after
             << (terms_before == terms_after ? " (steady)" : " (AN ELECTION OCCURRED)");
        observer._message(note.str());
    }
    observer._check(row._tally._completed == row._tally._offered,
                    "transactions: " + std::to_string(row._tally._offered - row._tally._completed) +
                        " of " + std::to_string(row._tally._offered) + " did not commit");
    return row;
}

}  // namespace txn_rows_detail

/// @brief `k_required_repetitions` measurements of one transaction row.
template<typename StateMachine>
auto txn_row(const txn_row_spec& spec, const row_observer& observer) -> repeated_result {
    repeated_result row;
    row._warmup_operations = warmup_operations(spec._operations, spec._in_flight);
    row._measured_operations = spec._operations;
    for (std::size_t repetition = 0; repetition < k_required_repetitions; ++repetition) {
        auto run = txn_rows_detail::one_txn_measurement<StateMachine>(spec, observer);
        observer._message(format_repetition(repetition, run));
        row.record(std::move(run));
    }
    return row;
}

}  // namespace kythira::testing
