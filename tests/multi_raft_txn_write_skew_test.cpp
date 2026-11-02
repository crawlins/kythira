// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_txn_write_skew_test.cpp
/// @brief The write-skew workload (task 17 of
///        `.kiro/specs/multi-raft-transactions/`, Requirement 13.4).
///
/// Pairs of accounts, one on each of two shards, each opening at 50. The
/// invariant is the pair's combined minimum balance: `x + y >= 0`. A
/// withdrawal reads both accounts and takes 60 from one of them only if the
/// pair's sum covers it. Two withdrawals that read the same snapshot and
/// write different accounts are the textbook write skew: each sees 100,
/// each writes a key the other did not, and snapshot isolation's first
/// committer wins rule has no write-write conflict to catch.
///
/// - **`snapshot` breaks the invariant.** The control: without it the
///   serializable case would prove nothing.
/// - **`serializable` never does**, while shard leaders move: commit-time
///   read validation refuses whichever withdrawal read a key the other
///   wrote.

#define BOOST_TEST_MODULE multi_raft_txn_write_skew_test
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
#include <barrier>
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
#include <utility>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_txn_write_skew_test"), nullptr};
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
using kythira::txn_abort_reason;
using kythira::txn_isolation;
using kythira::txn_outcome_kind;
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
constexpr std::int64_t k_opening_balance = 50;
/// More than one account holds and less than the pair does.
constexpr std::int64_t k_withdrawal = 60;

/// Pair `p`: one account below the shard boundary "m" and one above it.
auto pair_accounts(int p) -> std::pair<std::string, std::string> {
    return {"a" + std::to_string(p), "q" + std::to_string(p)};
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
class skew_cluster {
public:
    skew_cluster() {
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

    ~skew_cluster() {
        _running = false;
        for (auto& t : _drivers) {
            t.join();
        }
        for (auto& h : _hosts) {
            h->stop();
        }
    }

    skew_cluster(const skew_cluster&) = delete;
    auto operator=(const skew_cluster&) -> skew_cluster& = delete;

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

auto options(txn_isolation level) -> kythira::txn_options {
    kythira::txn_options o;
    o.isolation = level;
    o.lock_ttl = std::chrono::milliseconds{500};
    o.timeout = std::chrono::seconds{5};
    return o;
}

/// What the workload saw.
struct tally {
    std::atomic<int> _rounds{0};
    std::atomic<int> _committed{0};
    std::atomic<int> _read_conflicts{0};
    std::atomic<int> _write_conflicts{0};
    std::atomic<int> _other_aborts{0};
    std::atomic<int> _failed{0};
    std::atomic<int> _refused{0};
    std::atomic<int> _checks{0};
    std::atomic<int> _violations{0};
    std::atomic<std::int64_t> _worst_sum{0};
    std::atomic<int> _leader_moves{0};
};

/// Write every pair's opening balances, retrying a busy shard.
auto open_pairs(skew_cluster& c, int pairs) -> bool {
    for (int attempt = 0; attempt < 5; ++attempt) {
        try {
            coordinator_type open(c.client(), options(txn_isolation::snapshot));
            open.begin();
            for (int p = 0; p < pairs; ++p) {
                const auto [x, y] = pair_accounts(p);
                open.put(x, set_cmd(x, k_opening_balance));
                open.put(y, set_cmd(y, k_opening_balance));
            }
            const auto out = open.commit();
            open.drain();
            if (out._kind == txn_outcome_kind::committed) {
                return true;
            }
        } catch (const std::exception&) {
        }
    }
    return false;
}

/// Check one pair on a fresh snapshot, then reset it for the next round.
/// A check or reset that fails during a leader move is skipped: the next
/// round starts from whatever committed state the pair is in, which is
/// still a state the invariant must hold for.
auto check_and_reset(skew_cluster& c, const std::string& x, const std::string& y, tally& t)
    -> void {
    for (int attempt = 0; attempt < 3; ++attempt) {
        try {
            coordinator_type check(c.client(), options(txn_isolation::snapshot));
            check.begin();
            const auto sum = balance_of(check.get(x)) + balance_of(check.get(y));
            check.rollback();
            ++t._checks;
            if (sum < 0) {
                ++t._violations;
            }
            auto worst = t._worst_sum.load();
            while (sum < worst && !t._worst_sum.compare_exchange_weak(worst, sum)) {
            }
            break;
        } catch (const std::exception&) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        try {
            coordinator_type reset(c.client(), options(txn_isolation::snapshot));
            reset.begin();
            reset.put(x, set_cmd(x, k_opening_balance));
            reset.put(y, set_cmd(y, k_opening_balance));
            const auto out = reset.commit();
            reset.drain();
            if (out._kind == txn_outcome_kind::committed) {
                return;
            }
        } catch (const std::exception&) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

/// @brief Run `rounds` write-skew rounds on each of `pairs` account pairs.
///
/// Each pair has two workers. In a round both begin a transaction at
/// `level` and read both accounts; a barrier makes sure both have read
/// before either writes, which is exactly the interleaving write skew
/// needs. Then each withdraws `k_withdrawal` from its own account if the
/// pair's combined balance it read covers it, and commits. A third
/// barrier later, worker 0 reads the pair in a fresh snapshot, records a
/// violation if the sum is below zero, and resets both accounts to the
/// opening balance for the next round.
///
/// With `chaos`, a thread moves shard leaders while the rounds run.
auto run_rounds(skew_cluster& c, txn_isolation level, int pairs, int rounds, bool chaos)
    -> std::unique_ptr<tally> {
    auto t = std::make_unique<tally>();
    t->_worst_sum = 2 * k_opening_balance;
    std::atomic<bool> done{false};
    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<std::barrier<>>> barriers;
    for (int p = 0; p < pairs; ++p) {
        barriers.push_back(std::make_unique<std::barrier<>>(2));
    }
    for (int p = 0; p < pairs; ++p) {
        for (int side = 0; side < 2; ++side) {
            workers.emplace_back([&, p, side] {
                auto& sync = *barriers[static_cast<std::size_t>(p)];
                const auto [x, y] = pair_accounts(p);
                const auto& mine = side == 0 ? x : y;
                for (int round = 0; round < rounds; ++round) {
                    std::optional<coordinator_type> txn;
                    std::optional<std::int64_t> sum;
                    try {
                        txn.emplace(c.client(), options(level));
                        txn->begin();
                        sum = balance_of(txn->get(x)) + balance_of(txn->get(y));
                    } catch (const std::exception&) {
                        ++t->_failed;
                    }
                    // Both sides have read before either writes.
                    sync.arrive_and_wait();
                    if (sum.has_value()) {
                        try {
                            if (*sum >= k_withdrawal) {
                                const auto own = balance_of(txn->get(mine));
                                txn->put(mine, set_cmd(mine, own - k_withdrawal));
                                const auto out = txn->commit();
                                txn->drain();
                                if (out._kind == txn_outcome_kind::committed) {
                                    ++t->_committed;
                                } else if (out._reason == txn_abort_reason::read_conflict) {
                                    ++t->_read_conflicts;
                                } else if (out._reason == txn_abort_reason::write_conflict) {
                                    ++t->_write_conflicts;
                                } else {
                                    ++t->_other_aborts;
                                }
                            } else {
                                ++t->_refused;
                                txn->rollback();
                            }
                        } catch (const std::exception&) {
                            ++t->_failed;
                        }
                    }
                    sync.arrive_and_wait();
                    if (side == 0) {
                        check_and_reset(c, x, y, *t);
                        ++t->_rounds;
                    }
                    sync.arrive_and_wait();
                }
            });
        }
    }

    std::thread mover;
    if (chaos) {
        mover = std::thread([&] {
            std::mt19937 rng(17);
            std::uniform_int_distribution<node_id_t> node(1, k_node_count);
            for (int step = 0; !done.load(); ++step) {
                std::this_thread::sleep_for(std::chrono::milliseconds{300});
                const group_id_type g = step % 2 == 0 ? 1 : 2;
                auto* leader = c.leader_of(g);
                const auto from = c.leader_node(g);
                if (leader == nullptr || !from.has_value()) {
                    continue;
                }
                auto to = node(rng);
                while (to == *from) {
                    to = node(rng);
                }
                if (settle(leader->transfer_leadership(g, to, std::chrono::seconds{2}),
                           std::chrono::seconds{3})) {
                    ++t->_leader_moves;
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    done = true;
    if (mover.joinable()) {
        mover.join();
    }
    return t;
}

auto log_tally(const tally& t) -> void {
    BOOST_TEST_MESSAGE("rounds " << t._rounds << ", committed " << t._committed
                                 << ", read conflicts " << t._read_conflicts << ", write conflicts "
                                 << t._write_conflicts << ", other aborts " << t._other_aborts
                                 << ", failed " << t._failed << ", refused " << t._refused
                                 << ", checks " << t._checks << ", violations " << t._violations
                                 << ", worst sum " << t._worst_sum << ", leader moves "
                                 << t._leader_moves);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_txn_write_skew)

/// The control: under `snapshot` the workload must break its invariant,
/// or the serializable case below would prove nothing.
BOOST_AUTO_TEST_CASE(snapshot_allows_write_skew, *boost::unit_test::timeout(180)) {
    skew_cluster c;
    BOOST_REQUIRE(c.await_leaders());
    BOOST_REQUIRE(open_pairs(c, 1));
    const auto t = run_rounds(c, txn_isolation::snapshot, 1, 10, false);
    log_tally(*t);
    BOOST_TEST(t->_checks.load() > 0);
    BOOST_TEST(t->_violations.load() > 0);
    BOOST_TEST(t->_worst_sum.load() < 0);
}

/// Under `serializable` no round ever leaves a pair below zero, with
/// leaders moving under the workload.
BOOST_AUTO_TEST_CASE(serializable_keeps_the_minimum_balance, *boost::unit_test::timeout(300)) {
    skew_cluster c;
    BOOST_REQUIRE(c.await_leaders());
    constexpr int k_pairs = 4;
    BOOST_REQUIRE(open_pairs(c, k_pairs));
    const auto t = run_rounds(c, txn_isolation::serializable, k_pairs, 25, true);
    log_tally(*t);

    // Wait for the resolver to clear every lock, then check every pair once
    // more on committed state.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    bool clear = false;
    while (!clear && std::chrono::steady_clock::now() < deadline) {
        clear = true;
        for (int p = 0; p < k_pairs && clear; ++p) {
            const auto [x, y] = pair_accounts(p);
            try {
                clear = !c.intent_on(x) && !c.intent_on(y);
            } catch (const std::exception&) {
                clear = false;
            }
        }
        if (!clear) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
    BOOST_REQUIRE_MESSAGE(clear, "intents still held 30 s after the workload ended");
    for (int p = 0; p < k_pairs; ++p) {
        const auto [x, y] = pair_accounts(p);
        BOOST_TEST(c.balance(x) + c.balance(y) >= 0);
    }

    BOOST_TEST(t->_violations.load() == 0);
    BOOST_TEST(t->_worst_sum.load() >= 0);
    // The workload did what it claims: rounds were checked, withdrawals
    // committed, the two sides collided and were refused by read
    // validation, and leaders moved.
    BOOST_TEST(t->_checks.load() > 0);
    BOOST_TEST(t->_committed.load() > 0);
    BOOST_TEST(t->_read_conflicts.load() + t->_write_conflicts.load() > 0);
    BOOST_TEST(t->_leader_moves.load() > 0);
}

BOOST_AUTO_TEST_SUITE_END()
