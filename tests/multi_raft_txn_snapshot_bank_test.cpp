// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_txn_snapshot_bank_test.cpp
/// @brief The tier-2 bank workload (task 14 of
///        `.kiro/specs/multi-raft-transactions/`, Requirement 13.4).
///
/// Sixteen accounts over two shards, three in-process hosts and a timestamp
/// oracle group, every shard a `redis_kv_mvcc_state_machine` under
/// `mvcc::on`. Unlike the tier-1 workload, a transfer here is a real
/// read-modify-write: a `snapshot` transaction reads both balances at its
/// start timestamp and writes the new values back. Snapshot isolation must
/// make that safe on its own: two transfers that touch one account cannot
/// both commit, because the second to commit finds a version newer than
/// its start (first committer wins).
///
/// While workers transfer, a chaos thread moves leaderships, splits the
/// lower shard and merges it back, and some coordinators die after every
/// prepare with a credit of money that does not exist. Checked:
///
/// - **Committed state: the total is constant**, once every lock is gone.
/// - **`snapshot`: every read of all sixteen accounts in one transaction
///   sums to the total.** A reader that saw half a transfer, a poison
///   credit, or a lost update would see another sum.

#define BOOST_TEST_MODULE multi_raft_txn_snapshot_bank_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/multi_raft_txn_client.hpp>
#include <raft/persistence.hpp>
#include <raft/redis_kv_commands.hpp>
#include <raft/redis_kv_mvcc_state_machine.hpp>
#include <raft/txn_coordinator.hpp>
#include <raft/txn_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_txn_snapshot_bank_test"), nullptr};
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
using kythira::mvcc;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_range;
using kythira::txn_isolation;
using kythira::txn_outcome_kind;
using kythira::txn_step;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_t = std::uint64_t;
using kv_sm = kythira::redis_kv_mvcc_state_machine<std::uint64_t>;
using txn_sm =
    kythira::transactional_state_machine<kv_sm, key_type, kythira::redis_kv_partitioner, mvcc::on>;

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
using client_type = kythira::multi_raft_txn_client<host_types, key_type, group_id_type>;
using coordinator_type = kythira::transaction_coordinator<client_type, key_type>;

constexpr node_id_t k_node_count = 3;
constexpr group_id_type k_oracle = 1000;
constexpr std::int64_t k_opening_balance = 100;
/// What a poison transaction credits: money that does not exist.
constexpr std::int64_t k_poison_credit = 1'000'000;

/// Eight accounts below the shard boundary "m" and eight above it.
auto accounts() -> std::vector<std::string> {
    std::vector<std::string> out;
    for (int i = 0; i < 8; ++i) {
        out.push_back("a" + std::to_string(i));
    }
    for (int i = 0; i < 8; ++i) {
        out.push_back("q" + std::to_string(i));
    }
    return out;
}

auto set_cmd(const std::string& k, std::int64_t v) -> std::vector<std::byte> {
    const auto text = std::to_string(v);
    std::vector<std::byte> bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[i] = static_cast<std::byte>(text[i]);
    }
    return kythira::encode_redis_kv_command(kythira::redis_kv_set_command{k, std::move(bytes), 0});
}

auto bytes_or_none(const kv_sm::entry_handle& h) -> std::optional<std::vector<std::byte>> {
    if (!h) {
        return std::nullopt;
    }
    return h->_value;
}

auto value_of(const kv_sm& sm, const key_type& k) -> std::optional<std::vector<std::byte>> {
    return bytes_or_none(sm.lookup(k));
}

auto value_at(const kv_sm& sm, const key_type& k, std::uint64_t ts)
    -> std::optional<std::vector<std::byte>> {
    return bytes_or_none(sm.read_at(k, ts));
}

/// A balance as stored; an absent account reads as zero.
auto balance_of(const std::optional<std::vector<std::byte>>& b) -> std::int64_t {
    if (!b.has_value()) {
        return 0;
    }
    return std::stoll(std::string(reinterpret_cast<const char*>(b->data()), b->size()));  // NOLINT
}

template<typename Future> auto settle(Future&& f, std::chrono::milliseconds budget) -> bool {
    if (!f.wait(budget)) {
        return false;
    }
    try {
        std::ignore = std::forward<Future>(f).get();
        return true;
    } catch (const std::exception&) {
        // Refused (busy, not leader, epoch moved): the chaos thread just
        // tries something else next time. Boost.Test's log is not safe to
        // write from this thread, so nothing is reported.
        return false;
    }
}

class id_authority {
public:
    auto allocate(std::size_t n) -> std::vector<group_id_type> {
        std::lock_guard lock(_mutex);
        std::vector<group_id_type> out;
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(_next++);
        }
        return out;
    }

private:
    std::mutex _mutex;
    group_id_type _next{100};
};

/// Three hosts, two shards split at "m" and an oracle group, every host a
/// voter of each.
class bank_cluster {
public:
    bank_cluster() {
        const std::vector<node_id_t> voters{1, 2, 3};
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
            auto& h = *_hosts.back();
            h.create_group(descriptor_type{
                ._group_id = 1,
                ._range = shard_range<key_type>{._start = std::nullopt, ._end = key_type{"m"}},
                ._epoch = shard_epoch{},
                ._voters = voters});
            h.create_group(descriptor_type{
                ._group_id = 2,
                ._range = shard_range<key_type>{._start = key_type{"m"}, ._end = std::nullopt},
                ._epoch = shard_epoch{},
                ._voters = voters});
            h.create_oracle_group(voters);
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
        std::vector<host_type*> raw;
        for (auto& h : _hosts) {
            raw.push_back(h.get());
        }
        _client = std::make_unique<client_type>(std::move(raw), value_of, value_at);
        for (auto& h : _hosts) {
            h->set_txn_forwarder([this](const std::vector<key_type>& keys,
                                        const std::vector<std::byte>& env,
                                        std::chrono::milliseconds timeout) {
                return _client->send(keys, env, timeout).get();
            });
            h->set_timestamp_forwarder([this](std::size_t n, std::chrono::milliseconds timeout) {
                return _client->allocate_timestamps(n, timeout);
            });
        }
    }

    ~bank_cluster() {
        _running = false;
        for (auto& t : _drivers) {
            t.join();
        }
        for (auto& h : _hosts) {
            h->stop();
        }
    }

    bank_cluster(const bank_cluster&) = delete;
    auto operator=(const bank_cluster&) -> bank_cluster& = delete;

    [[nodiscard]] auto client() -> client_type& { return *_client; }

    auto await_leaders() -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (std::chrono::steady_clock::now() < deadline) {
            if (leader_of(1) != nullptr && leader_of(2) != nullptr &&
                leader_of(k_oracle) != nullptr) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    [[nodiscard]] auto leader_of(group_id_type group) -> host_type* {
        const auto i = leader_index(group);
        return i.has_value() ? _hosts[*i].get() : nullptr;
    }

    [[nodiscard]] auto leader_node(group_id_type group) -> std::optional<node_id_t> {
        const auto i = leader_index(group);
        return i.has_value() ? std::optional<node_id_t>(*i + 1) : std::nullopt;
    }

    [[nodiscard]] auto group_of(const key_type& key) -> std::optional<group_id_type> {
        return _client->resolve(key);
    }

    [[nodiscard]] auto balance(const key_type& key) -> std::int64_t {
        return balance_of(_client->read(key, std::chrono::seconds{5})._value);
    }

    [[nodiscard]] auto intent_on(const key_type& key) -> bool {
        return _client->read(key, std::chrono::seconds{5})._intent.has_value();
    }

private:
    [[nodiscard]] auto leader_index(group_id_type group) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            auto* n = _hosts[i]->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return i;
            }
        }
        return std::nullopt;
    }

    auto make_config(node_id_t id) -> config_type {
        config_type cfg{
            .node_id = id,
            .network_client = fabric_client{_fabric, id},
            .network_server = fabric_server{_fabric, id},
            .store_factory =
                [](const group_id_type&) { return host_types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{120};
        cfg.config._election_timeout_max = std::chrono::milliseconds{260};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{25};
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.partitioner = kythira::make_partitioner<key_type>(kythira::redis_kv_partitioner{});
        cfg.allocate_group_ids = [this](std::size_t n) { return _ids.allocate(n); };
        cfg.oracle_group = k_oracle;
        // The test drives every split and merge itself.
        cfg.automatic_split_merge_enabled = false;
        cfg.split_merge_interval = std::chrono::milliseconds{0};
        cfg.resolver_interval = std::chrono::milliseconds{200};
        // Garbage collection runs throughout, with a lifetime well above
        // any one transaction, so no reader here should be too old.
        cfg.txn_gc_lifetime = std::chrono::seconds{20};
        cfg.txn_gc_interval = std::chrono::seconds{1};
        return cfg;
    }

    message_fabric _fabric{8};
    id_authority _ids;
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
    std::unique_ptr<client_type> _client;
};

auto snapshot_options() -> kythira::txn_options {
    kythira::txn_options o;
    o.isolation = txn_isolation::snapshot;
    o.lock_ttl = std::chrono::milliseconds{500};
    o.timeout = std::chrono::seconds{5};
    return o;
}

struct coordinator_crashed : std::runtime_error {
    coordinator_crashed() : std::runtime_error("coordinator crashed") {}
};

struct tally {
    std::atomic<int> _committed{0};
    std::atomic<int> _conflicts{0};
    std::atomic<int> _other_aborts{0};
    std::atomic<int> _unknown{0};
    std::atomic<int> _failed{0};
    std::atomic<int> _poisoned{0};
    std::atomic<int> _reads{0};
    std::atomic<int> _bad_sums{0};
    std::atomic<std::int64_t> _last_bad_sum{0};
    std::atomic<int> _leader_moves{0};
    std::atomic<int> _splits{0};
    std::atomic<int> _merges{0};
};

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_txn_snapshot_bank)

BOOST_AUTO_TEST_CASE(snapshot_reads_always_sum_to_the_total, *boost::unit_test::timeout(300)) {
    bank_cluster c;
    BOOST_REQUIRE(c.await_leaders());
    const auto accts = accounts();

    // Open every account in one transaction.
    {
        bool opened = false;
        for (int attempt = 0; attempt < 5 && !opened; ++attempt) {
            coordinator_type open(c.client(), snapshot_options());
            open.begin();
            for (const auto& a : accts) {
                open.put(a, set_cmd(a, k_opening_balance));
            }
            const auto out = open.commit();
            open.drain();
            opened = out._kind == txn_outcome_kind::committed;
        }
        BOOST_REQUIRE(opened);
    }
    const std::int64_t expected_total = k_opening_balance * static_cast<std::int64_t>(accts.size());

    tally t;
    std::atomic<bool> workers_done{false};
    constexpr int k_workers = 4;
    const auto min_run = std::chrono::seconds{8};
    const auto max_run = std::chrono::seconds{90};
    const auto run_started = std::chrono::steady_clock::now();
    const auto keep_going = [&] {
        const auto ran = std::chrono::steady_clock::now() - run_started;
        if (ran >= max_run) {
            return false;
        }
        return ran < min_run || t._leader_moves.load() < 2 || t._splits.load() < 2 ||
               t._merges.load() < 2;
    };

    // ── workers: read-modify-write transfers, and every eighth a poison ─────
    std::vector<std::thread> workers;
    for (int w = 0; w < k_workers; ++w) {
        workers.emplace_back([&, w] {
            std::mt19937 rng(static_cast<unsigned>(2000 + w));
            std::uniform_int_distribution<std::size_t> pick(0, accts.size() - 1);
            std::uniform_int_distribution<std::int64_t> amount(1, 10);
            for (int i = 0; keep_going(); ++i) {
                try {
                    const auto& from = accts[pick(rng)];
                    auto to = accts[pick(rng)];
                    while (to == from) {
                        to = accts[pick(rng)];
                    }
                    const bool poison = i % 8 == 7;
                    coordinator_type txn(c.client(), snapshot_options());
                    if (poison) {
                        txn.set_crash_hook([](txn_step s) {
                            if (s == txn_step::prepared) {
                                throw coordinator_crashed{};
                            }
                        });
                    }
                    txn.begin();
                    const auto from_balance = balance_of(txn.get(from));
                    const auto to_balance = balance_of(txn.get(to));
                    const auto d = amount(rng);
                    // A poison credits money that does not exist; it must
                    // never commit.
                    txn.put(from, set_cmd(from, from_balance - (poison ? 0 : d)));
                    txn.put(to, set_cmd(to, to_balance + (poison ? k_poison_credit : d)));
                    if (poison) {
                        try {
                            std::ignore = txn.commit();
                        } catch (const coordinator_crashed&) {
                            ++t._poisoned;
                        }
                        txn.drain();
                        continue;
                    }
                    const auto out = txn.commit();
                    txn.drain();
                    switch (out._kind) {
                        case txn_outcome_kind::committed:
                            ++t._committed;
                            break;
                        case txn_outcome_kind::aborted:
                            if (out._reason == kythira::txn_abort_reason::write_conflict) {
                                ++t._conflicts;
                            } else {
                                ++t._other_aborts;
                            }
                            break;
                        default:
                            ++t._unknown;
                            break;
                    }
                } catch (const std::exception&) {
                    // A read or commit that failed outright during a leader
                    // move or a merge freeze. Its fate is the resolver's.
                    ++t._failed;
                }
            }
        });
    }

    // ── readers: every snapshot of all accounts sums to the total ────────────
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&] {
            while (!workers_done.load()) {
                try {
                    coordinator_type reader(c.client(), snapshot_options());
                    reader.begin();
                    std::int64_t sum = 0;
                    for (const auto& a : accts) {
                        sum += balance_of(reader.get(a));
                    }
                    reader.rollback();
                    ++t._reads;
                    if (sum != expected_total) {
                        ++t._bad_sums;
                        t._last_bad_sum = sum;
                    }
                } catch (const std::exception&) {
                    // A read that timed out during a leader move; a partial
                    // sum is never counted.
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        });
    }

    // ── chaos: leader moves, a split of the lower shard, and its merge back ──
    std::thread chaos([&] {
        std::mt19937 rng(11);
        std::uniform_int_distribution<node_id_t> node(1, k_node_count);
        std::optional<group_id_type> child;
        int step = 0;
        while (!workers_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{250});
            ++step;
            if (step % 3 == 1) {
                const auto* k =
                    std::array{"a0", "a6", "q0"}[static_cast<std::size_t>(step / 3) % 3];
                const auto g = c.group_of(k);
                auto* leader = g.has_value() ? c.leader_of(*g) : nullptr;
                const auto from = g.has_value() ? c.leader_node(*g) : std::nullopt;
                if (leader == nullptr || !from.has_value()) {
                    continue;
                }
                auto to = node(rng);
                while (to == *from) {
                    to = node(rng);
                }
                if (settle(leader->transfer_leadership(*g, to, std::chrono::seconds{2}),
                           std::chrono::seconds{3})) {
                    ++t._leader_moves;
                }
            } else if (!child.has_value()) {
                auto* leader = c.leader_of(1);
                if (leader != nullptr &&
                    settle(leader->split_shard(1, {"a4"}, std::chrono::seconds{5}),
                           std::chrono::seconds{6})) {
                    ++t._splits;
                    const auto d = leader->resolve("a6");
                    if (d.has_value() && d->_group_id != 1) {
                        child = d->_group_id;
                    }
                }
            } else {
                auto* leader = c.leader_of(*child);
                if (leader != nullptr &&
                    settle(leader->merge_shards(*child, 1, std::chrono::seconds{5}),
                           std::chrono::seconds{6})) {
                    ++t._merges;
                    child.reset();
                }
            }
        }
    });

    for (auto& w : workers) {
        w.join();
    }
    workers_done = true;
    for (auto& r : readers) {
        r.join();
    }
    chaos.join();

    // ── settle: wait for the resolver to clear every lock ────────────────────
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    bool clear = false;
    while (!clear && std::chrono::steady_clock::now() < deadline) {
        clear = true;
        for (const auto& k : accts) {
            try {
                if (c.intent_on(k)) {
                    clear = false;
                    break;
                }
            } catch (const std::exception&) {
                clear = false;
                break;
            }
        }
        if (!clear) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
    BOOST_TEST_MESSAGE("committed "
                       << t._committed << ", conflicts " << t._conflicts << ", other aborts "
                       << t._other_aborts << ", unknown " << t._unknown << ", failed " << t._failed
                       << ", poison " << t._poisoned << ", snapshot reads " << t._reads
                       << ", leader moves " << t._leader_moves << ", splits " << t._splits
                       << ", merges " << t._merges);
    BOOST_REQUIRE_MESSAGE(clear, "intents still held 30 s after the workload ended");

    std::int64_t total = 0;
    for (const auto& a : accts) {
        total += c.balance(a);
    }
    BOOST_TEST(total == expected_total);
    BOOST_TEST_MESSAGE("last bad sum " << t._last_bad_sum.load());
    BOOST_TEST(t._bad_sums.load() == 0);

    // The workload did what it claims: money moved, transfers collided,
    // coordinators died, snapshots were read, and the shards moved.
    BOOST_TEST(t._committed.load() > 0);
    BOOST_TEST(t._conflicts.load() > 0);
    BOOST_TEST(t._poisoned.load() > 0);
    BOOST_TEST(t._reads.load() > 0);
    BOOST_TEST(t._leader_moves.load() > 0);
    BOOST_TEST(t._splits.load() > 0);
    BOOST_TEST(t._merges.load() > 0);
}

BOOST_AUTO_TEST_SUITE_END()
