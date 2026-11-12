// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file raft_timestamp_oracle_test.cpp
/// @brief The timestamp oracle (task 10 of
///        `.kiro/specs/multi-raft-transactions/`, Requirement 7).
///
/// Three in-process hosts, each with a replica of one shard and of the
/// oracle group, over one message fabric, with every group's Raft state on
/// disk so a restart is a real one. The properties:
///
/// - every value is served at most once, and each caller sees its own
///   values strictly increase, under concurrent load;
/// - a new leader, after a leader change or a whole-cluster restart, serves
///   above everything any earlier leader served;
/// - a leader cut off from its peers serves nothing;
/// - the oracle group is invisible to routing, split, merge and placement;
/// - the `allocate_timestamps` hook replaces the in-host oracle.

#define BOOST_TEST_MODULE raft_timestamp_oracle_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_kv_workload.hpp"
#include "multi_raft_test_fabric.hpp"

#include <raft/binary_file_persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/raft_timestamp_oracle.hpp>
#include <raft/test_state_machine.hpp>
#include <raft/txn_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_timestamp_oracle_test"), nullptr};
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
using kythira::timestamp_range;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::kv_partitioner;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_t = std::uint64_t;
using kv_sm = kythira::test_key_value_state_machine<std::uint64_t>;
using txn_sm = kythira::transactional_state_machine<kv_sm, key_type, kv_partitioner>;

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
        kythira::binary_file_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = txn_sm;

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
using descriptor_type = shard_descriptor<group_id_type, key_type, std::uint64_t>;
using not_leader_type = kythira::shard_not_leader_exception<group_id_type, node_id_t>;

static_assert(kythira::timestamp_oracle<kythira::raft_timestamp_oracle<host_type>>);
static_assert(kythira::timestamp_oracle<kythira::hook_timestamp_oracle>);

constexpr group_id_type k_shard = 1;
constexpr group_id_type k_oracle = 1000;
constexpr node_id_t k_node_count = 3;
constexpr auto k_timeout = std::chrono::seconds{2};
constexpr auto k_election_timeout_max = std::chrono::milliseconds{260};

/// A directory removed when the test ends.
class temp_dir {
public:
    temp_dir() {
        static std::atomic<int> counter{0};
        _path = std::filesystem::temp_directory_path() /
                ("kythira-oracle-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(_path);
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    [[nodiscard]] auto path() const -> const std::filesystem::path& { return _path; }

private:
    std::filesystem::path _path;
};

/// Three hosts over one fabric, each driven by its own thread, with every
/// group's Raft state under `dir`, so a second `cluster` over the same
/// directory is the first one restarted.
class cluster {
public:
    using tweak_fn = std::function<void(config_type&)>;

    explicit cluster(const std::filesystem::path& dir, const tweak_fn& tweak = {}) {
        const std::vector<node_id_t> voters{1, 2, 3};
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            auto cfg = make_config(id, dir);
            if (tweak) {
                tweak(cfg);
            }
            _hosts.push_back(std::make_unique<host_type>(std::move(cfg)));
            _hosts.back()->create_group(descriptor_type{
                ._group_id = k_shard,
                ._range = shard_range<key_type>{._start = std::nullopt, ._end = std::nullopt},
                ._epoch = shard_epoch{},
                ._voters = voters});
            _hosts.back()->create_oracle_group(voters);
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
    [[nodiscard]] auto fabric() -> message_fabric& { return _fabric; }

    /// The host whose replica leads `group`, among those not in `skip`.
    [[nodiscard]] auto await_leader(group_id_type group, const std::vector<node_id_t>& skip = {})
        -> std::optional<node_id_t> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
        while (std::chrono::steady_clock::now() < deadline) {
            for (node_id_t id = 1; id <= k_node_count; ++id) {
                if (std::find(skip.begin(), skip.end(), id) != skip.end()) {
                    continue;
                }
                auto* n = host(id).group_node(group);
                if (n != nullptr && n->is_leader()) {
                    return id;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return std::nullopt;
    }

    /// `await_leader`, then wait until that host has kept the lead, in one
    /// term, for three maximum election timeouts.
    ///
    /// A just-elected leader of a fresh cluster can still be deposed: two
    /// nodes that start pre-vote in the same instant are both granted, and
    /// the loser's election wins the next term. CI caught it under
    /// concurrent_allocations_are_unique_and_increasing, twice in 54 runs:
    /// node 2 won term 1, node 1 won term 2 within milliseconds, and all
    /// 1200 calls the case sent to node 2 failed. A case that sends straight
    /// to one host needs the leader that stays.
    [[nodiscard]] auto await_settled_leader(group_id_type group) -> std::optional<node_id_t> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto id = await_leader(group);
            if (!id) {
                return std::nullopt;
            }
            auto* n = host(*id).group_node(group);
            if (n == nullptr) {
                continue;
            }
            const auto term = n->get_current_term();
            const auto hold_until = std::chrono::steady_clock::now() + 3 * k_election_timeout_max;
            bool held = true;
            while (held && std::chrono::steady_clock::now() < hold_until) {
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                held = n->is_leader() && n->get_current_term() == term;
            }
            if (held) {
                return id;
            }
        }
        return std::nullopt;
    }

    /// `n` timestamps from whichever host leads the oracle, retrying through
    /// elections.
    auto allocate(std::size_t n, const std::vector<node_id_t>& skip = {}) -> timestamp_range {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
        std::string last;
        while (std::chrono::steady_clock::now() < deadline) {
            auto leader = await_leader(k_oracle, skip);
            if (!leader) {
                break;
            }
            try {
                return host(*leader).allocate_timestamps(n, k_timeout).get();
            } catch (const std::exception& e) {
                last = e.what();
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
        }
        throw std::runtime_error("no oracle leader served: " + last);
    }

    /// Cut `id` off from every other host, both ways.
    auto isolate(node_id_t id) -> void {
        for (node_id_t other = 1; other <= k_node_count; ++other) {
            if (other != id) {
                _fabric.partition(id, other);
                _fabric.partition(other, id);
            }
        }
    }

private:
    auto make_config(node_id_t id, const std::filesystem::path& dir) -> config_type {
        const auto node_dir = dir / ("node-" + std::to_string(id));
        config_type cfg{
            .node_id = id,
            .network_client = fabric_client{_fabric, id},
            .network_server = fabric_server{_fabric, id},
            .store_factory =
                [node_dir](const group_id_type& group) {
                    return host_types::persistence_engine_type{node_dir /
                                                               ("group-" + std::to_string(group))};
                },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{120};
        cfg.config._election_timeout_max = k_election_timeout_max;
        cfg.config._heartbeat_interval = std::chrono::milliseconds{25};
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.partitioner = kythira::make_partitioner<key_type>(kv_partitioner{});
        cfg.oracle_group = k_oracle;
        // Small, so the cases cross many window boundaries.
        cfg.oracle_window = 50;
        return cfg;
    }

    message_fabric _fabric{8};
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
};

}  // namespace

BOOST_AUTO_TEST_SUITE(raft_timestamp_oracle)

// Requirements 7.1, 7.3, 7.5: under concurrent callers every value is served
// once, each caller's values increase, and callers share batches.
BOOST_AUTO_TEST_CASE(concurrent_allocations_are_unique_and_increasing,
                     *boost::unit_test::timeout(120)) {
    temp_dir dir;
    cluster c(dir.path());
    const auto leader = c.await_settled_leader(k_oracle);
    BOOST_REQUIRE(leader.has_value());
    auto& host = c.host(*leader);

    constexpr int k_threads = 8;
    constexpr int k_calls = 150;
    std::mutex mutex;
    std::vector<timestamp_range> all;
    std::atomic<int> failures{0};
    std::atomic<int> not_increasing{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < k_threads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(static_cast<unsigned>(t));
            std::uniform_int_distribution<std::size_t> size(1, 7);
            std::uint64_t last = 0;
            for (int i = 0; i < k_calls; ++i) {
                try {
                    const auto r = host.allocate_timestamps(size(rng), k_timeout).get();
                    if (r._first <= last) {
                        ++not_increasing;
                    }
                    last = r.end() - 1;
                    std::lock_guard lock(mutex);
                    all.push_back(r);
                } catch (const std::exception&) {
                    ++failures;
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    BOOST_TEST(failures.load() == 0);
    BOOST_TEST(not_increasing.load() == 0);
    BOOST_REQUIRE(all.size() == static_cast<std::size_t>(k_threads * k_calls));

    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) { return a._first < b._first; });
    BOOST_TEST(all.front()._first >= 1U);
    for (std::size_t i = 1; i < all.size(); ++i) {
        BOOST_TEST_CONTEXT("range " << i) {
            BOOST_TEST(all[i]._first >= all[i - 1].end());
        }
    }

    const auto stats = host.oracle_stats();
    BOOST_TEST(stats._allocations == all.size());
    // Shared batches: fewer heartbeat rounds than allocations.
    BOOST_TEST(stats._batches < stats._allocations);
    BOOST_TEST(stats._reserves >= 1U);
}

// Requirement 7.4: a new leader serves above everything an earlier one
// served.
BOOST_AUTO_TEST_CASE(a_new_leader_serves_above_its_predecessor, *boost::unit_test::timeout(120)) {
    temp_dir dir;
    cluster c(dir.path());
    std::uint64_t high = 0;
    for (int i = 0; i < 20; ++i) {
        high = std::max(high, c.allocate(3).end());
    }
    std::vector<node_id_t> down;
    for (int round = 0; round < 2; ++round) {
        const auto old_leader = c.await_leader(k_oracle, down);
        BOOST_REQUIRE(old_leader.has_value());
        // Only one host may be cut off at a time, or there is no quorum.
        // Both directions: a leader whose heartbeats still arrive is never
        // replaced.
        c.fabric().heal_all();
        c.isolate(*old_leader);
        down = {*old_leader};

        const auto r = c.allocate(5, down);
        BOOST_TEST(r._first >= high);
        high = r.end();
        for (int i = 0; i < 10; ++i) {
            const auto next = c.allocate(1, down);
            BOOST_TEST(next._first >= high);
            high = next.end();
        }
    }
}

// Requirement 7.5: a leader cut off from its peers cannot confirm its
// leadership and serves nothing, while the majority side elects a new leader
// that serves above it.
BOOST_AUTO_TEST_CASE(a_deposed_leader_serves_nothing, *boost::unit_test::timeout(120)) {
    temp_dir dir;
    cluster c(dir.path());
    const auto first = c.allocate(10);
    const auto old_leader = c.await_leader(k_oracle);
    BOOST_REQUIRE(old_leader.has_value());

    c.isolate(*old_leader);
    BOOST_CHECK_THROW((void)c.host(*old_leader).allocate_timestamps(1, k_timeout).get(),
                      std::exception);
    const auto r = c.allocate(1, {*old_leader});
    BOOST_TEST(r._first >= first.end());

    c.fabric().heal_all();
    // Healed, the old leader steps down; whoever leads serves higher still.
    const auto after = c.allocate(1);
    BOOST_TEST(after._first >= r.end());
}

// Requirement 7.4: across a restart of every host.
BOOST_AUTO_TEST_CASE(a_restarted_cluster_serves_above_what_it_served,
                     *boost::unit_test::timeout(120)) {
    temp_dir dir;
    std::uint64_t high = 0;
    {
        cluster c(dir.path());
        for (int i = 0; i < 30; ++i) {
            high = std::max(high, c.allocate(4).end());
        }
    }
    cluster c(dir.path());
    const auto r = c.allocate(1);
    BOOST_TEST(r._first >= high);
}

// A host that does not lead the oracle group refuses with the leader hint.
BOOST_AUTO_TEST_CASE(a_follower_refuses_with_a_hint, *boost::unit_test::timeout(60)) {
    temp_dir dir;
    cluster c(dir.path());
    (void)c.allocate(1);
    const auto leader = c.await_leader(k_oracle);
    BOOST_REQUIRE(leader.has_value());
    const node_id_t follower = *leader == 1 ? 2 : 1;
    try {
        (void)c.host(follower).allocate_timestamps(1, k_timeout).get();
        BOOST_FAIL("a follower served");
    } catch (const not_leader_type& e) {
        BOOST_TEST(e.group_id() == k_oracle);
    }
    BOOST_CHECK_THROW((void)c.host(*leader).allocate_timestamps(0, k_timeout).get(),
                      std::invalid_argument);
}

// Requirement 7.2: the oracle group is not a shard.
BOOST_AUTO_TEST_CASE(the_oracle_group_is_not_a_shard, *boost::unit_test::timeout(60)) {
    temp_dir dir;
    cluster c(dir.path());
    const auto leader = c.await_leader(k_oracle);
    BOOST_REQUIRE(leader.has_value());
    BOOST_REQUIRE(c.await_leader(k_shard).has_value());
    auto& host = c.host(*leader);

    BOOST_TEST(host.is_oracle_group(k_oracle));
    BOOST_TEST(!host.is_oracle_group(k_shard));
    BOOST_TEST(host.has_group(k_oracle));
    BOOST_TEST(!host.shard_map_snapshot().find(k_oracle).has_value());
    BOOST_TEST(host.resolve("").has_value());
    BOOST_TEST(host.resolve("")->_group_id == k_shard);
    host.refresh_map_from_local_groups();
    BOOST_TEST(!host.shard_map_snapshot().find(k_oracle).has_value());

    BOOST_CHECK_THROW((void)host.split_shard(k_oracle, {"m"}, {}, k_timeout).get(), std::exception);
    BOOST_CHECK_THROW((void)host.merge_shards(k_oracle, k_shard, {}, k_timeout).get(),
                      std::exception);
    for (const auto& r : host.build_shard_reports()) {
        BOOST_TEST(r._descriptor._group_id != k_oracle);
    }
}

// Requirement 7.6: the hook replaces the in-host oracle.
BOOST_AUTO_TEST_CASE(the_hook_replaces_the_oracle_group, *boost::unit_test::timeout(60)) {
    temp_dir dir;
    std::atomic<std::uint64_t> next{100};
    cluster c(dir.path(), [&next](config_type& cfg) {
        cfg.allocate_timestamps = [&next](std::size_t n) {
            return timestamp_range{._first = next.fetch_add(n), ._count = n};
        };
    });
    // Any host serves: the hook is the deployment's, not the oracle group's.
    const auto a = c.host(1).allocate_timestamps(2, k_timeout).get();
    const auto b = c.host(2).allocate_timestamps(3, k_timeout).get();
    BOOST_TEST(a._first == 100U);
    BOOST_TEST(a._count == 2U);
    BOOST_TEST(b._first == 102U);
    BOOST_TEST(c.host(1).oracle_stats()._batches == 0U);

    kythira::hook_timestamp_oracle bad{
        [](std::size_t n) { return timestamp_range{._first = 0, ._count = n}; }};
    BOOST_CHECK_THROW((void)bad.allocate(1).get(), kythira::timestamp_oracle_unavailable_exception);
}

BOOST_AUTO_TEST_SUITE_END()
