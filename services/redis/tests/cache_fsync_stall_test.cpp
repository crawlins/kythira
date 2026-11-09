// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file cache_fsync_stall_test.cpp
/// @brief A disk whose fsync stalls must not cost a disk-mode shard its
///        leader (doc/TODO.md, "Disk mode applies under the node mutex").
///
/// Three in-process hosts run one shard over `cache_kv_state_machine`, each
/// with its own chunk store, at the production election timeouts
/// (150-300 ms). The stores' `_before_fsync` seam then makes every fsync
/// take 800 ms, longer than any election timeout, and the leader takes a
/// run of `put_chunk` writes, large (loose files) and small (pack records).
///
/// `apply()` runs under the Raft node's mutex, which `tick()` needs to send
/// heartbeats. When the store fsynced inside `apply()`, each write silenced
/// the leader for at least one stalled fsync, its followers elected a new
/// leader, and this test saw the term move. Now `apply()` only writes to the
/// page cache and the store's syncer thread fsyncs, so the leader and its
/// term must hold for the whole run. The chunks must still be readable on
/// every replica straight away, and durable once the syncer catches up.
///
/// No sockets: the hosts talk over the in-process fabric.

#define BOOST_TEST_MODULE cache_fsync_stall_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"
#include "scratch_dir.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <kythira/redis/cache_kv_state_machine.hpp>
#include <kythira/redis/redis_kv_commands.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("cache_fsync_stall_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_range;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_t = std::uint64_t;

struct host_types {
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
    using state_machine_type = kythira::cache_kv_state_machine<log_index_type>;

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

using host_type = multi_raft<host_types, key_type, group_id_type>;
using config_type = multi_raft_config<host_types, key_type, group_id_type>;
using descriptor_type = shard_descriptor<group_id_type, key_type, node_id_t>;

constexpr std::size_t k_node_count = 3;
constexpr group_id_type k_group = 1;
/// Longer than the largest election timeout below, so one stalled fsync
/// under the node mutex is enough to lose the leader.
constexpr std::chrono::milliseconds k_fsync_stall{800};
constexpr std::chrono::milliseconds k_submit_timeout{20000};

/// How long every fsync takes, in ms. Shared by every store in the process.
std::atomic<int> g_fsync_stall_ms{0};

/// A node's chunk store and the counts over it, kept alive by any replica.
struct node_store {
    explicit node_store(kythira::cache_chunk_store_options o)
        : _store(std::move(o)), _refs(_store) {}
    kythira::cache_chunk_store _store;
    kythira::chunk_refcounts _refs;
};

class cluster {
public:
    explicit cluster(const std::filesystem::path& root) {
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            kythira::cache_chunk_store_options opts;
            opts._data_dir = root / ("host-" + std::to_string(id));
            opts._before_fsync = [](const std::filesystem::path&) {
                if (const int ms = g_fsync_stall_ms.load(); ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{ms});
                }
            };
            _stores.push_back(std::make_shared<node_store>(std::move(opts)));
            _hosts.push_back(std::make_unique<host_type>(make_config(id, _stores.back())));
            _hosts.back()->create_group(descriptor_type{
                ._group_id = k_group,
                ._range = shard_range<key_type>{._start = std::nullopt, ._end = std::nullopt},
                ._epoch = shard_epoch{},
                ._voters = {1, 2, 3}});
        }
        for (auto& h : _hosts) {
            h->start();
        }
        _running = true;
        for (auto& h : _hosts) {
            _drivers.emplace_back([this, host = h.get()] {
                while (_running.load()) {
                    host->tick();
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
            });
        }
    }

    ~cluster() {
        g_fsync_stall_ms = 0;
        _running = false;
        for (auto& t : _drivers) {
            t.join();
        }
        for (auto& h : _hosts) {
            h->stop();
        }
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;

    [[nodiscard]] auto host(node_id_t id) -> host_type& { return *_hosts.at(id - 1); }
    [[nodiscard]] auto store(node_id_t id) -> kythira::cache_chunk_store& {
        return _stores.at(id - 1)->_store;
    }

    [[nodiscard]] auto leader() -> node_id_t {
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            auto* n = _hosts[id - 1]->group_node(k_group);
            if (n != nullptr && n->is_leader()) {
                return id;
            }
        }
        return 0;
    }

    [[nodiscard]] auto term(node_id_t id) -> std::uint64_t {
        auto* n = _hosts[id - 1]->group_node(k_group);
        return n == nullptr ? 0 : n->get_current_term();
    }

    auto await_leader(std::chrono::milliseconds budget) -> node_id_t {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto id = leader(); id != 0) {
                // Let the term's no-op commit before the first write.
                std::this_thread::sleep_for(std::chrono::milliseconds{300});
                if (leader() == id) {
                    return id;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return 0;
    }

private:
    auto make_config(node_id_t id, const std::shared_ptr<node_store>& owner) -> config_type {
        config_type cfg{
            .node_id = id,
            .network_client = fabric_client{_fabric, id},
            .network_server = fabric_server{_fabric, id},
            .store_factory =
                [](const group_id_type&) { return host_types::persistence_engine_type{}; },
        };
        // The production defaults: what a real disk-mode node runs with.
        cfg.config._election_timeout_min = std::chrono::milliseconds{150};
        cfg.config._election_timeout_max = std::chrono::milliseconds{300};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{50};
        // No automatic snapshot during the run: a snapshot waits for the
        // syncer by design, and this test is about the writes.
        cfg.config._snapshot_threshold_bytes = std::size_t{1} << 40U;
        cfg.logger = kythira::console_logger{kythira::log_level::warning};
        cfg.logger_factory = [](const group_id_type&) {
            return kythira::console_logger{kythira::log_level::warning};
        };
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.partitioner = kythira::make_partitioner<key_type>(kythira::redis_kv_partitioner{});
        std::shared_ptr<kythira::chunk_refcounts> refs(owner, &owner->_refs);
        cfg.state_machine_factory = [refs](const group_id_type&) {
            return host_types::state_machine_type(refs);
        };
        return cfg;
    }

    message_fabric _fabric{8};
    std::vector<std::shared_ptr<node_store>> _stores;
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
};

/// A scratch directory on tmpfs where there is one: the seam supplies the
/// stall, so the real disk's own latency only adds noise.
struct scratch {
    std::filesystem::path _path = kythira::testing::fast_scratch_root() /
                                  ("kythira-fsync-stall-" + std::to_string(::getpid()));
    scratch() {
        std::filesystem::remove_all(_path);
        std::filesystem::create_directories(_path);
    }
    ~scratch() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    scratch(const scratch&) = delete;
    auto operator=(const scratch&) -> scratch& = delete;
};

auto random_bytes(std::size_t n, std::mt19937_64& rng) -> std::vector<std::byte> {
    std::vector<std::byte> out(n);
    for (auto& b : out) {
        b = static_cast<std::byte>(rng() & 0xFFU);
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_stalled_fsync_does_not_cost_the_leader, *boost::unit_test::timeout(180)) {
    scratch dir;
    cluster c(dir._path);

    const node_id_t leader = c.await_leader(std::chrono::seconds{10});
    BOOST_REQUIRE_NE(leader, 0U);
    const std::uint64_t term = c.term(leader);

    // Watch every node's term and who leads, every 10 ms, for the whole run.
    std::atomic<bool> watching{true};
    std::atomic<std::uint64_t> highest_term{term};
    std::atomic<node_id_t> other_leader{0};
    std::thread watcher([&] {
        while (watching.load()) {
            for (node_id_t id = 1; id <= k_node_count; ++id) {
                const auto t = c.term(id);
                if (t > highest_term.load()) {
                    highest_term = t;
                }
            }
            if (const auto now = c.leader(); now != 0 && now != leader) {
                other_leader = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    });

    g_fsync_stall_ms = static_cast<int>(k_fsync_stall.count());

    std::mt19937_64 rng{20261109};
    // Large chunks (loose files: a file fsync and directory fsyncs each) and
    // small ones (pack records, plus the new pack's directory entry).
    const std::vector<std::size_t> sizes{std::size_t{1} << 20U, 1000, std::size_t{256} * 1024, 4000,
                                         std::size_t{1} << 20U, 100};
    std::vector<kythira::chunk_sha> written;
    std::size_t failures = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        kythira::redis_kv_put_chunk_command cmd;
        cmd._key = "stall/" + std::to_string(i);
        cmd._bytes = random_bytes(sizes[i], rng);
        cmd._sha = kythira::sha256_of(cmd._bytes);
        try {
            std::ignore =
                c.host(leader)
                    .submit_command(kythira::encode_redis_kv_command(cmd), k_submit_timeout)
                    .get();
            written.push_back(cmd._sha);
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("write " << i << " failed: " << e.what());
        }
    }
    const auto took = std::chrono::steady_clock::now() - started;
    // Let any election a stall provoked show up in the watcher.
    std::this_thread::sleep_for(std::chrono::milliseconds{600});
    watching = false;
    watcher.join();

    BOOST_TEST_MESSAGE("six writes took "
                       << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
                       << " ms");
    BOOST_TEST(failures == 0U);
    BOOST_TEST(highest_term.load() == term,
               "a node moved to term " << highest_term.load() << " from " << term);
    BOOST_TEST(other_leader.load() == 0U, "node " << other_leader.load() << " took over");
    BOOST_TEST(c.leader() == leader);

    // Every replica can read every chunk before any of it is durable.
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        for (const auto& sha : written) {
            BOOST_TEST(c.store(id).contains(sha), "node " << id << " lacks a chunk");
            BOOST_TEST(c.store(id).read(sha).has_value(), "node " << id << " cannot read one");
        }
    }

    // Once the syncer has caught up, every large chunk is at its final name.
    g_fsync_stall_ms = 0;
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        c.store(id).wait_durable();
        for (const auto& sha : written) {
            if (!c.store(id).is_packed_size(c.store(id).chunk_size(sha).value_or(0))) {
                BOOST_TEST(std::filesystem::exists(c.store(id).loose_path(sha)));
            }
        }
    }
}
