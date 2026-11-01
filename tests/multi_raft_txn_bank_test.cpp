// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_txn_bank_test.cpp
/// @brief The tier-1 bank workload (task 8 of
///        `.kiro/specs/multi-raft-transactions/`, Requirement 13.4).
///
/// Sixteen accounts over two shards, three in-process hosts. Worker threads
/// move money between random pairs of accounts in cross-shard and
/// single-shard transactions while a chaos thread transfers leaderships,
/// splits a shard and merges it back. Two properties are checked:
///
/// - **Committed state: the total is constant.** Once every transaction has
///   ended and the resolver has cleared every lock, the balances sum to what
///   they started at, whatever each transfer's outcome was: committed,
///   aborted, or unknown to its coordinator and settled by the resolver.
/// - **`read_committed`: no read returns an uncommitted value.** "Poison"
///   transactions write a marker value and credit an account with money
///   that does not exist, then their coordinator dies after every prepare,
///   before the commit point. The resolver must abort them, so a reader
///   must never see the marker and the credit must never land.
///
/// A transfer is a pair of commutative `add` commands, not puts of values
/// read earlier: tier 1 has no read validation, so read-modify-write
/// transfers would lose updates by design (that is tier 3's job). The `add`
/// lives in `bank_sm`, a thin splittable wrapper over the key/value test
/// machine.

#define BOOST_TEST_MODULE multi_raft_txn_bank_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_kv_workload.hpp"
#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/multi_raft_txn_client.hpp>
#include <raft/persistence.hpp>
#include <raft/test_state_machine.hpp>
#include <raft/txn_coordinator.hpp>
#include <raft/txn_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
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
        char* argv_data[] = {const_cast<char*>("multi_raft_txn_bank_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<std::string>)

namespace {

using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_range;
using kythira::txn_isolation;
using kythira::txn_outcome_kind;
using kythira::txn_step;
using kythira::testing::encode_kv_command;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::kv_partitioner;
using kythira::testing::kv_put;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_t = std::uint64_t;
using kv_sm = kythira::test_key_value_state_machine<std::uint64_t>;

/// The key/value test machine plus one command: `add`, which adds a signed
/// decimal delta to a key's decimal value (absent counts as zero). Same
/// layout as a put, so `kv_partitioner` finds its key; only the type byte
/// differs. Everything else, splitting included, is the wrapped machine's.
class bank_sm {
public:
    static constexpr std::uint8_t k_add = 0x40;

    static auto make_add(const std::string& key, std::int64_t delta) -> std::vector<std::byte> {
        auto cmd = encode_kv_command(1, key, std::to_string(delta));
        cmd[0] = static_cast<std::byte>(k_add);
        return cmd;
    }

    auto apply(const std::vector<std::byte>& command, std::uint64_t index)
        -> std::vector<std::byte> {
        if (command.empty() || static_cast<std::uint8_t>(command[0]) != k_add) {
            return _kv.apply(command, index);
        }
        std::size_t at = 1;
        const auto key = read_str(command, at);
        const auto delta = std::stoll(read_str(command, at));
        const auto now = _kv.get_value(key);
        const std::int64_t current = now.has_value() ? std::stoll(*now) : 0;
        std::ignore = _kv.apply(kv_put(key, std::to_string(current + delta)), index);
        return {};
    }

    [[nodiscard]] auto get_state() const -> std::vector<std::byte> { return _kv.get_state(); }
    auto restore_from_snapshot(const std::vector<std::byte>& s, std::uint64_t index) -> void {
        _kv.restore_from_snapshot(s, index);
    }

    [[nodiscard]] auto approximate_size_bytes() const -> std::size_t {
        return _kv.approximate_size_bytes();
    }
    [[nodiscard]] auto approximate_key_count() const -> std::size_t {
        return _kv.approximate_key_count();
    }
    [[nodiscard]] auto suggest_split_keys(std::size_t max) -> std::vector<std::string> {
        return _kv.suggest_split_keys(max);
    }
    [[nodiscard]] auto can_split_at(const std::string& key) const -> bool {
        return _kv.can_split_at(key);
    }
    [[nodiscard]] auto split_state(const std::vector<std::string>& keys)
        -> std::vector<std::vector<std::byte>> {
        return _kv.split_state(keys);
    }
    auto absorb(const std::vector<std::byte>& other, const shard_range<std::string>& range)
        -> void {
        _kv.absorb(other, range);
    }

    [[nodiscard]] auto get_value(const std::string& key) const -> std::optional<std::string> {
        return _kv.get_value(key);
    }

private:
    static auto read_str(const std::vector<std::byte>& c, std::size_t& at) -> std::string {
        std::uint32_t n{};
        if (at + sizeof(n) > c.size()) {
            throw std::invalid_argument("bank_sm: truncated add");
        }
        std::memcpy(&n, c.data() + at, sizeof(n));
        at += sizeof(n);
        if (at + n > c.size()) {
            throw std::invalid_argument("bank_sm: truncated add");
        }
        std::string s(reinterpret_cast<const char*>(c.data() + at), n);  // NOLINT
        at += n;
        return s;
    }

    kv_sm _kv;
};

static_assert(kythira::splittable_state_machine<bank_sm, std::string>);

using txn_sm = kythira::transactional_state_machine<bank_sm, key_type, kv_partitioner>;

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
constexpr std::int64_t k_opening_balance = 100;
/// What a poison transaction credits: money that does not exist. If one
/// ever committed, the total would be off by at least this much.
constexpr std::int64_t k_poison_credit = 1'000'000;
constexpr const char* k_poison_marker = "uncommitted";

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

/// Marker keys sit on the upper shard, so a poison transaction (a marker
/// plus a lower-shard account) is always two-phase.
auto marker(int worker) -> std::string {
    return "mark" + std::to_string(worker);
}

auto as_string(const std::optional<std::vector<std::byte>>& b) -> std::optional<std::string> {
    if (!b.has_value()) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(b->data()), b->size());  // NOLINT
}

auto bank_value_of(const bank_sm& sm, const key_type& k) -> std::optional<std::vector<std::byte>> {
    auto v = sm.get_value(k);
    if (!v.has_value()) {
        return std::nullopt;
    }
    std::vector<std::byte> out;
    for (char c : *v) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
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

/// Three hosts, two shards split at "m", every host a voter of every shard.
class bank_cluster {
public:
    bank_cluster() {
        const std::vector<node_id_t> voters{1, 2, 3};
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
            _hosts.back()->create_group(descriptor_type{
                ._group_id = 1,
                ._range = shard_range<key_type>{._start = std::nullopt, ._end = key_type{"m"}},
                ._epoch = shard_epoch{},
                ._voters = voters});
            _hosts.back()->create_group(descriptor_type{
                ._group_id = 2,
                ._range = shard_range<key_type>{._start = key_type{"m"}, ._end = std::nullopt},
                ._epoch = shard_epoch{},
                ._voters = voters});
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
        _client = std::make_unique<client_type>(std::move(raw), bank_value_of);
        for (auto& h : _hosts) {
            h->set_txn_forwarder([this](const std::vector<key_type>& keys,
                                        const std::vector<std::byte>& env,
                                        std::chrono::milliseconds timeout) {
                return _client->send(keys, env, timeout).get();
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
            if (leader_of(1) != nullptr && leader_of(2) != nullptr) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    /// The host leading `group`, if any.
    [[nodiscard]] auto leader_of(group_id_type group) -> host_type* {
        const auto i = leader_index(group);
        return i.has_value() ? _hosts[*i].get() : nullptr;
    }

    /// The node id of the host leading `group`, if any.
    [[nodiscard]] auto leader_node(group_id_type group) -> std::optional<node_id_t> {
        const auto i = leader_index(group);
        return i.has_value() ? std::optional<node_id_t>(*i + 1) : std::nullopt;
    }

    /// The group that owns `key`, by the first host that can resolve it.
    [[nodiscard]] auto group_of(const key_type& key) -> std::optional<group_id_type> {
        return _client->resolve(key);
    }

    [[nodiscard]] auto balance(const key_type& key) -> std::int64_t {
        const auto v = as_string(_client->read(key, std::chrono::seconds{5})._value);
        return v.has_value() ? std::stoll(*v) : 0;
    }

    [[nodiscard]] auto value(const key_type& key) -> std::optional<std::string> {
        return as_string(_client->read(key, std::chrono::seconds{5})._value);
    }

    [[nodiscard]] auto intent_on(const key_type& key) -> bool {
        return _client->read(key, std::chrono::seconds{5})._intent.has_value();
    }

    [[nodiscard]] auto host(std::size_t i) -> host_type& { return *_hosts.at(i); }

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
        cfg.partitioner = kythira::make_partitioner<key_type>(kv_partitioner{});
        cfg.allocate_group_ids = [this](std::size_t n) { return _ids.allocate(n); };
        // The test drives every split and merge itself.
        cfg.automatic_split_merge_enabled = false;
        cfg.split_merge_interval = std::chrono::milliseconds{0};
        cfg.resolver_interval = std::chrono::milliseconds{200};
        return cfg;
    }

    message_fabric _fabric{8};
    id_authority _ids;
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
    std::unique_ptr<client_type> _client;
};

auto bank_options(txn_isolation isolation = txn_isolation::read_committed) -> kythira::txn_options {
    kythira::txn_options o;
    o.isolation = isolation;
    o.lock_ttl = std::chrono::milliseconds{500};
    o.timeout = std::chrono::seconds{5};
    return o;
}

struct coordinator_crashed : std::runtime_error {
    coordinator_crashed() : std::runtime_error("coordinator crashed") {}
};

struct tally {
    std::atomic<int> _committed{0};
    std::atomic<int> _aborted{0};
    std::atomic<int> _unknown{0};
    std::atomic<int> _failed{0};
    std::atomic<int> _poisoned{0};
    std::atomic<int> _reads{0};
    std::atomic<int> _dirty_reads{0};
    std::atomic<int> _leader_moves{0};
    std::atomic<int> _splits{0};
    std::atomic<int> _merges{0};
};

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_txn_bank)

BOOST_AUTO_TEST_CASE(transfers_keep_the_total_through_leader_moves_splits_and_merges,
                     *boost::unit_test::timeout(300)) {
    bank_cluster c;
    BOOST_REQUIRE(c.await_leaders());
    const auto accts = accounts();

    // Open every account: one transaction per shard, so each is one phase.
    for (const char side : {'a', 'q'}) {
        coordinator_type open(c.client(), bank_options());
        open.begin();
        for (const auto& a : accts) {
            if (a[0] == side) {
                open.put(a, bank_sm::make_add(a, k_opening_balance));
            }
        }
        BOOST_REQUIRE(open.commit()._kind == txn_outcome_kind::committed);
    }
    const std::int64_t expected_total = k_opening_balance * static_cast<std::int64_t>(accts.size());

    tally t;
    std::atomic<bool> workers_done{false};
    constexpr int k_workers = 4;
    // The workers run until the chaos thread has moved leaders, split and
    // merged at least twice each, or this long, whichever is later: the
    // point is transactions in flight while the shards move.
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

    // ── workers: transfers, and every eighth a poison transaction ────────────
    std::vector<std::thread> workers;
    for (int w = 0; w < k_workers; ++w) {
        workers.emplace_back([&, w] {
            std::mt19937 rng(static_cast<unsigned>(1000 + w));
            std::uniform_int_distribution<std::size_t> pick(0, accts.size() - 1);
            std::uniform_int_distribution<std::int64_t> amount(1, 10);
            for (int i = 0; keep_going(); ++i) {
                try {
                    if (i % 8 == 7) {
                        // Credit a different lower-shard account each time, so
                        // one poison's locks do not simply block the next.
                        const auto victim = "a" + std::to_string((w + i / 8) % 8);
                        coordinator_type poison(c.client(), bank_options());
                        poison.set_crash_hook([](txn_step s) {
                            if (s == txn_step::prepared) {
                                throw coordinator_crashed{};
                            }
                        });
                        poison.begin();
                        poison.put(marker(w), kv_put(marker(w), k_poison_marker));
                        poison.put(victim, bank_sm::make_add(victim, k_poison_credit));
                        try {
                            std::ignore = poison.commit();
                        } catch (const coordinator_crashed&) {
                            ++t._poisoned;
                        }
                        poison.drain();
                        continue;
                    }
                    const auto& from = accts[pick(rng)];
                    auto to = accts[pick(rng)];
                    while (to == from) {
                        to = accts[pick(rng)];
                    }
                    const auto d = amount(rng);
                    coordinator_type txn(c.client(), bank_options());
                    txn.begin();
                    txn.put(from, bank_sm::make_add(from, -d));
                    txn.put(to, bank_sm::make_add(to, d));
                    const auto out = txn.commit();
                    txn.drain();
                    switch (out._kind) {
                        case txn_outcome_kind::committed:
                            ++t._committed;
                            break;
                        case txn_outcome_kind::aborted:
                            ++t._aborted;
                            break;
                        default:
                            ++t._unknown;
                            break;
                    }
                } catch (const std::exception&) {
                    // A coordinator call that failed outright (a merge freeze,
                    // a leader in transit). Its fate is the resolver's.
                    ++t._failed;
                }
            }
        });
    }

    // ── a reader: read_committed must never see a poison marker ──────────────
    std::thread reader([&] {
        while (!workers_done.load()) {
            try {
                coordinator_type r(c.client(), bank_options(txn_isolation::read_committed));
                r.begin();
                for (int w = 0; w < k_workers; ++w) {
                    const auto v = as_string(r.get(marker(w)));
                    ++t._reads;
                    if (v == std::optional<std::string>(k_poison_marker)) {
                        ++t._dirty_reads;
                    }
                }
                r.rollback();
            } catch (const std::exception&) {
                // A read during a leader move or a merge freeze; try again.
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    });

    // ── chaos: leader moves, a split of the lower shard, and its merge back ──
    std::thread chaos([&] {
        std::mt19937 rng(7);
        std::uniform_int_distribution<node_id_t> node(1, k_node_count);
        std::optional<group_id_type> child;
        int step = 0;
        while (!workers_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{250});
            ++step;
            if (step % 3 == 1) {
                // Move some shard's leadership.
                const auto* k =
                    std::array{"a0", "a6", "q0"}[static_cast<std::size_t>(step / 3) % 3];
                {
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
                }
            } else if (!child.has_value()) {
                // Split the lower shard at "a4".
                auto* leader = c.leader_of(1);
                if (leader != nullptr &&
                    settle(leader->split_shard(1, {"a4"}, std::chrono::seconds{5}),
                           std::chrono::seconds{6})) {
                    ++t._splits;
                    // The splitting host applied it, so its map is current.
                    const auto d = leader->resolve("a6");
                    if (d.has_value() && d->_group_id != 1) {
                        child = d->_group_id;
                    }
                }
            } else {
                // Merge the right child back into shard 1.
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
    reader.join();
    chaos.join();

    // ── settle: wait for the resolver to clear every lock ────────────────────
    std::vector<std::string> every_key = accts;
    for (int w = 0; w < k_workers; ++w) {
        every_key.push_back(marker(w));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    bool clear = false;
    while (!clear && std::chrono::steady_clock::now() < deadline) {
        clear = true;
        for (const auto& k : every_key) {
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
    BOOST_TEST_MESSAGE("committed " << t._committed << ", aborted " << t._aborted << ", unknown "
                                    << t._unknown << ", failed " << t._failed << ", poison "
                                    << t._poisoned << ", reads " << t._reads << ", leader moves "
                                    << t._leader_moves << ", splits " << t._splits << ", merges "
                                    << t._merges);
    BOOST_REQUIRE_MESSAGE(clear, "intents still held 30 s after the workload ended");

    std::int64_t total = 0;
    for (const auto& a : accts) {
        total += c.balance(a);
    }
    BOOST_TEST(total == expected_total);
    for (int w = 0; w < k_workers; ++w) {
        BOOST_TEST(c.value(marker(w)) != std::optional<std::string>(k_poison_marker));
    }
    BOOST_TEST(t._dirty_reads.load() == 0);

    // The workload did what it claims: money moved, the coordinators died,
    // and the shards moved under it.
    BOOST_TEST(t._committed.load() > 0);
    BOOST_TEST(t._poisoned.load() > 0);
    BOOST_TEST(t._leader_moves.load() > 0);
    BOOST_TEST(t._splits.load() > 0);
    BOOST_TEST(t._merges.load() > 0);
}

BOOST_AUTO_TEST_SUITE_END()
