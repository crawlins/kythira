// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_txn_snapshot_test.cpp
/// @brief Snapshot isolation over versioned shards (task 12 of
///        `.kiro/specs/multi-raft-transactions/`).
///
/// The wrapper cases drive one `transactional_state_machine` over
/// `redis_kv_mvcc_state_machine` directly: first committer wins at prepare,
/// commits land at their timestamps, and the snapshot format keeps each
/// intent's start timestamp.
///
/// The cluster cases run three hosts with two shards split at "m" and a
/// timestamp oracle group, every shard under `mvcc::on`:
///
/// - every write, plain or transactional, lands at an oracle timestamp;
/// - a snapshot transaction reads one point in time across shards;
/// - of two snapshot transactions writing one key, the first to commit wins
///   and the other is not retried;
/// - a reader waits for an intent whose holder started at or below its read
///   point, and ignores one whose holder started later.

#define BOOST_TEST_MODULE multi_raft_txn_snapshot_test
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_txn_snapshot_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

BOOST_TEST_DONT_PRINT_LOG_VALUE(kythira::txn_outcome_kind)
BOOST_TEST_DONT_PRINT_LOG_VALUE(kythira::txn_abort_reason)
BOOST_TEST_DONT_PRINT_LOG_VALUE(kythira::txn_result_status)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<std::string>)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<std::uint64_t>)

namespace {

using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::mvcc;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_range;
using kythira::txn_abort_reason;
using kythira::txn_id;
using kythira::txn_isolation;
using kythira::txn_outcome_kind;
using kythira::txn_result_status;
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

static_assert(txn_sm::k_snapshot_version == 2);

auto bytes_of(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<std::byte>(s[i]);
    }
    return out;
}

auto set_cmd(const std::string& k, const std::string& v) -> std::vector<std::byte> {
    return kythira::encode_redis_kv_command(kythira::redis_kv_set_command{k, bytes_of(v), 0});
}

auto text_of(const kv_sm::entry_handle& h) -> std::optional<std::string> {
    if (!h) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(h->_value.data()),  // NOLINT
                       h->_value.size());
}

auto bytes_or_none(const kv_sm::entry_handle& h) -> std::optional<std::vector<std::byte>> {
    if (!h) {
        return std::nullopt;
    }
    return h->_value;
}

auto as_string(const std::optional<std::vector<std::byte>>& b) -> std::optional<std::string> {
    if (!b.has_value()) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(b->data()), b->size());  // NOLINT
}

// ── The wrapper alone ────────────────────────────────────────────────────

using env = kythira::txn_envelope<key_type>;
using write = kythira::txn_write<key_type>;
using result = kythira::txn_result<key_type>;

struct wrapper_fixture {
    txn_sm sm;
    std::uint64_t index = 0;

    auto run(const env& e) -> result {
        return kythira::decode_txn_result<key_type>(
            sm.apply(kythira::encode_txn_envelope<key_type>(e), ++index));
    }
    auto prepare(const txn_id& id, const std::string& key, const std::string& value,
                 std::uint64_t start_ts) -> result {
        return run(env{kythira::txn_prepare<key_type>{
            id, key, {write{key, set_cmd(key, value)}}, {}, start_ts, 1'000'000, true}});
    }
    auto commit(const txn_id& id, const std::string& key, std::uint64_t commit_ts) -> result {
        return run(env{kythira::txn_commit_primary<key_type>{id, key, commit_ts}});
    }
    [[nodiscard]] auto at(const std::string& key, std::uint64_t ts) const
        -> std::optional<std::string> {
        return text_of(sm.inner().read_at(key, ts));
    }
};

BOOST_AUTO_TEST_SUITE(wrapper)

BOOST_AUTO_TEST_CASE(a_commit_lands_at_its_timestamp) {
    wrapper_fixture f;
    const txn_id a{1, 1};
    BOOST_TEST(f.prepare(a, "k", "one", 5)._status == txn_result_status::ok);
    // An intent, not yet a version.
    BOOST_TEST(f.sm.intent_on("k")->_start_ts == 5U);
    BOOST_TEST(!f.sm.inner().latest_commit_ts("k").has_value());
    const auto committed = f.commit(a, "k", 9);
    BOOST_TEST(committed._status == txn_result_status::ok);
    // The primary's own reply is the commit's payload.
    BOOST_TEST(committed._payload ==
               kv_sm{}.apply_at(set_cmd("k", "one"), 9));  // same command, same reply
    BOOST_TEST(f.sm.inner().latest_commit_ts("k") == std::optional<std::uint64_t>(9));
    BOOST_TEST(!f.at("k", 8).has_value());
    BOOST_TEST(f.at("k", 9) == std::optional<std::string>("one"));
    BOOST_TEST(!f.sm.intent_on("k").has_value());
}

BOOST_AUTO_TEST_CASE(a_version_newer_than_the_start_timestamp_refuses_the_prepare) {
    wrapper_fixture f;
    BOOST_TEST(f.prepare(txn_id{1, 1}, "k", "one", 5)._status == txn_result_status::ok);
    BOOST_TEST(f.commit(txn_id{1, 1}, "k", 9)._status == txn_result_status::ok);

    // Started at 7, before the commit at 9: first committer wins.
    const auto late = f.prepare(txn_id{2, 2}, "k", "two", 7);
    BOOST_TEST(late._status == txn_result_status::write_conflict);
    BOOST_TEST(late._key == "k");
    BOOST_TEST(!late._holder.has_value());  // a version, not a lock
    BOOST_TEST(!f.sm.intent_on("k").has_value());

    // Started at 9 or later: no conflict.
    BOOST_TEST(f.prepare(txn_id{3, 3}, "k", "three", 9)._status == txn_result_status::ok);
    BOOST_TEST(f.commit(txn_id{3, 3}, "k", 12)._status == txn_result_status::ok);
    BOOST_TEST(f.at("k", 11) == std::optional<std::string>("one"));
    BOOST_TEST(f.at("k", 12) == std::optional<std::string>("three"));

    // A zero start timestamp (tier-1 isolation) asks for no check.
    BOOST_TEST(f.prepare(txn_id{4, 4}, "k", "four", 0)._status == txn_result_status::ok);
}

BOOST_AUTO_TEST_CASE(gc_drops_versions_below_the_safe_point) {
    wrapper_fixture f;
    BOOST_TEST(f.prepare(txn_id{1, 1}, "k", "one", 1)._status == txn_result_status::ok);
    BOOST_TEST(f.commit(txn_id{1, 1}, "k", 2)._status == txn_result_status::ok);
    BOOST_TEST(f.prepare(txn_id{2, 2}, "k", "two", 3)._status == txn_result_status::ok);
    BOOST_TEST(f.commit(txn_id{2, 2}, "k", 4)._status == txn_result_status::ok);
    BOOST_TEST(f.sm.inner().version_count() == 2U);
    BOOST_TEST(f.run(env{kythira::txn_gc{5}})._status == txn_result_status::ok);
    BOOST_TEST(f.sm.inner().version_count() == 1U);
    BOOST_TEST(f.at("k", 5) == std::optional<std::string>("two"));
}

BOOST_AUTO_TEST_CASE(the_snapshot_keeps_each_intents_start_timestamp) {
    wrapper_fixture f;
    BOOST_TEST(f.prepare(txn_id{1, 1}, "k", "one", 5)._status == txn_result_status::ok);
    BOOST_TEST(f.commit(txn_id{1, 1}, "k", 9)._status == txn_result_status::ok);
    BOOST_TEST(f.prepare(txn_id{2, 2}, "j", "two", 11)._status == txn_result_status::ok);

    const auto state = f.sm.get_state();
    txn_sm restored;
    restored.restore_from_snapshot(state, f.index);
    BOOST_TEST(restored.get_state() == state);
    BOOST_TEST(restored.intent_on("j")->_start_ts == 11U);
    BOOST_TEST(text_of(restored.inner().read_at("k", 9)) == std::optional<std::string>("one"));
}

BOOST_AUTO_TEST_SUITE_END()

// ── Three hosts ──────────────────────────────────────────────────────────

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

static_assert(kythira::txn_snapshot_client<client_type, key_type>);
static_assert(coordinator_type::k_mvcc);

constexpr group_id_type k_oracle = 1000;
constexpr node_id_t k_node_count = 3;
constexpr auto k_timeout = std::chrono::seconds{5};

auto value_of(const kv_sm& sm, const key_type& k) -> std::optional<std::vector<std::byte>> {
    return bytes_or_none(sm.lookup(k));
}

auto value_at(const kv_sm& sm, const key_type& k, std::uint64_t ts)
    -> std::optional<std::vector<std::byte>> {
    return bytes_or_none(sm.read_at(k, ts));
}

/// Three hosts over one fabric, each driven by its own thread.
class cluster {
public:
    cluster() {
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
                                        const std::vector<std::byte>& envelope,
                                        std::chrono::milliseconds timeout) {
                return _client->send(keys, envelope, timeout).get();
            });
            h->set_timestamp_forwarder([this](std::size_t n, std::chrono::milliseconds timeout) {
                return _client->allocate_timestamps(n, timeout);
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

    [[nodiscard]] auto leader_of(group_id_type g) -> host_type* {
        for (auto& h : _hosts) {
            auto* n = h->group_node(g);
            if (n != nullptr && n->is_leader()) {
                return h.get();
            }
        }
        return nullptr;
    }

    /// A plain write through the host leading the key's shard.
    auto plain_set(group_id_type shard, const std::string& key, const std::string& value)
        -> host_type* {
        const auto deadline = std::chrono::steady_clock::now() + k_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto* h = leader_of(shard); h != nullptr) {
                try {
                    std::ignore = h->submit_command(set_cmd(key, value), k_timeout).get();
                    return h;
                } catch (const kythira::shard_not_leader_exception<group_id_type, node_id_t>&) {
                    // Leadership moved between the check and the write.
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        throw std::runtime_error("plain_set: no leader took the write");
    }

    /// The newest committed value, through the shard leader.
    [[nodiscard]] auto value(const key_type& key) -> std::optional<std::string> {
        return as_string(_client->read(key, k_timeout)._value);
    }

    /// The commit timestamp of the newest version of `key` on its leader.
    [[nodiscard]] auto latest_ts(group_id_type shard, const key_type& key)
        -> std::optional<std::uint64_t> {
        auto* h = leader_of(shard);
        if (h == nullptr) {
            return std::nullopt;
        }
        return h->group_node(shard)
            ->with_leader_state_machine(
                [&](txn_sm& sm) { return sm.inner().latest_commit_ts(key); })
            .value_or(std::nullopt);
    }

private:
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
        cfg.oracle_group = k_oracle;
        return cfg;
    }

    message_fabric _fabric{8};
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
    std::unique_ptr<client_type> _client;
};

auto options(txn_isolation isolation) -> kythira::txn_options {
    kythira::txn_options o;
    o.isolation = isolation;
    o.lock_ttl = std::chrono::milliseconds{300};
    o.timeout = k_timeout;
    return o;
}

struct coordinator_crashed : std::runtime_error {
    coordinator_crashed() : std::runtime_error("coordinator crashed") {}
};

/// Commit `writes` in one transaction under read_committed. Set-up, not a
/// subject under test: a leader lost mid-commit on a loaded machine is
/// retried, which is safe because every write is a plain overwrite.
auto commit_all(cluster& c, const std::vector<std::pair<std::string, std::string>>& writes)
    -> kythira::txn_outcome<key_type> {
    kythira::txn_outcome<key_type> out;
    for (int attempt = 0; attempt < 5; ++attempt) {
        coordinator_type txn(c.client(), options(txn_isolation::read_committed));
        txn.begin();
        for (const auto& [k, v] : writes) {
            txn.put(k, set_cmd(k, v));
        }
        out = txn.commit();
        txn.drain();
        if (out._kind == txn_outcome_kind::committed) {
            break;
        }
        BOOST_TEST_MESSAGE("commit_all: attempt "
                           << attempt << " kind " << static_cast<int>(out._kind) << " reason "
                           << static_cast<int>(out._reason) << " detail " << out._detail);
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(cluster_cases)

BOOST_AUTO_TEST_CASE(snapshot_is_offered_over_versioned_shards, *boost::unit_test::timeout(30)) {
    const auto offered = coordinator_type::offered_isolations();
    BOOST_TEST(
        (std::find(offered.begin(), offered.end(), txn_isolation::snapshot) != offered.end()));
}

BOOST_AUTO_TEST_CASE(transactional_writes_land_at_their_commit_timestamp,
                     *boost::unit_test::timeout(60)) {
    cluster c;
    BOOST_REQUIRE(c.await_leaders());
    // One shard, yet two phases: the commit timestamp follows the intents.
    const auto out = commit_all(c, {{"apple", "a"}, {"banana", "b"}});
    BOOST_TEST(out._kind == txn_outcome_kind::committed);
    BOOST_TEST(out._primary == std::optional<std::string>("apple"));
    BOOST_TEST(out._commit_ts > 0U);
    BOOST_TEST(c.latest_ts(1, "apple") == std::optional<std::uint64_t>(out._commit_ts));
    BOOST_TEST(c.latest_ts(1, "banana") == std::optional<std::uint64_t>(out._commit_ts));

    const auto both = commit_all(c, {{"apple", "a2"}, {"zebra", "z"}});
    BOOST_TEST(both._kind == txn_outcome_kind::committed);
    BOOST_TEST(both._commit_ts > out._commit_ts);
    BOOST_TEST(c.latest_ts(2, "zebra") == std::optional<std::uint64_t>(both._commit_ts));
    auto& client = c.client();
    BOOST_TEST(as_string(client.read_at("apple", both._commit_ts - 1, k_timeout)._value) ==
               std::optional<std::string>("a"));
    BOOST_TEST(as_string(client.read_at("apple", both._commit_ts, k_timeout)._value) ==
               std::optional<std::string>("a2"));
}

BOOST_AUTO_TEST_CASE(plain_writes_land_at_oracle_timestamps, *boost::unit_test::timeout(60)) {
    cluster c;
    BOOST_REQUIRE(c.await_leaders());
    const auto before = c.client().allocate_timestamps(1, k_timeout)._first;
    // One write per shard. At least one of the two shard leaders is not
    // the oracle's leader unless one host leads all three groups, in which
    // case the forwarder goes unused; both paths must give a timestamp.
    auto* h1 = c.plain_set(1, "apple", "p1");
    auto* h2 = c.plain_set(2, "zebra", "p2");
    const auto* oracle = c.leader_of(k_oracle);
    BOOST_TEST_MESSAGE("forwarded: " << ((h1 != oracle) || (h2 != oracle)));
    const auto after = c.client().allocate_timestamps(1, k_timeout)._first;
    for (const auto& [shard, key] : {std::pair<group_id_type, std::string>{1, "apple"},
                                     std::pair<group_id_type, std::string>{2, "zebra"}}) {
        const auto ts = c.latest_ts(shard, key);
        BOOST_REQUIRE(ts.has_value());
        BOOST_TEST(*ts > before);
        BOOST_TEST(*ts < after);
    }
    BOOST_TEST(c.value("apple") == std::optional<std::string>("p1"));
    BOOST_TEST(!c.client().read("apple", k_timeout)._intent.has_value());
}

BOOST_AUTO_TEST_CASE(a_snapshot_reads_one_point_in_time, *boost::unit_test::timeout(60)) {
    cluster c;
    BOOST_REQUIRE(c.await_leaders());
    BOOST_REQUIRE(commit_all(c, {{"apple", "1"}, {"zebra", "1"}})._kind ==
                  txn_outcome_kind::committed);

    coordinator_type snap(c.client(), options(txn_isolation::snapshot));
    snap.begin();
    BOOST_TEST(snap.start_ts() > 0U);
    BOOST_TEST(as_string(snap.get("apple")) == std::optional<std::string>("1"));

    // Both keys move on, by transaction and by plain write, after the start.
    BOOST_REQUIRE(commit_all(c, {{"apple", "2"}, {"zebra", "2"}})._kind ==
                  txn_outcome_kind::committed);
    c.plain_set(2, "zebra", "3");

    // Still the start's point in time, on the other shard too.
    BOOST_TEST(as_string(snap.get("zebra")) == std::optional<std::string>("1"));
    BOOST_TEST(as_string(snap.get("apple")) == std::optional<std::string>("1"));
    snap.rollback();

    // read_committed sees the newest.
    coordinator_type rc(c.client(), options(txn_isolation::read_committed));
    rc.begin();
    BOOST_TEST(as_string(rc.get("zebra")) == std::optional<std::string>("3"));
    rc.rollback();

    coordinator_type later(c.client(), options(txn_isolation::snapshot));
    later.begin();
    BOOST_TEST(as_string(later.get("apple")) == std::optional<std::string>("2"));
    BOOST_TEST(as_string(later.get("zebra")) == std::optional<std::string>("3"));
    later.rollback();
}

BOOST_AUTO_TEST_CASE(the_first_committer_wins_and_the_loser_is_not_retried,
                     *boost::unit_test::timeout(60)) {
    cluster c;
    BOOST_REQUIRE(c.await_leaders());
    auto o = options(txn_isolation::snapshot);
    o.max_retries = 3;  // a version conflict must not use them
    coordinator_type first(c.client(), o);
    coordinator_type second(c.client(), o);
    first.begin();
    second.begin();
    first.put("apple", set_cmd("apple", "first"));
    first.put("zebra", set_cmd("zebra", "first"));
    second.put("apple", set_cmd("apple", "second"));

    const auto won = first.commit();
    first.drain();
    BOOST_TEST(won._kind == txn_outcome_kind::committed);
    const auto lost = second.commit();
    second.drain();
    BOOST_TEST(lost._kind == txn_outcome_kind::aborted);
    BOOST_TEST(lost._reason == txn_abort_reason::write_conflict);
    BOOST_TEST(lost._conflict_key == std::optional<std::string>("apple"));
    BOOST_TEST(!lost._holder.has_value());
    BOOST_TEST(c.value("apple") == std::optional<std::string>("first"));
    BOOST_TEST(!c.client().read("apple", k_timeout)._intent.has_value());

    // A plain write after a snapshot's start conflicts the same way.
    coordinator_type third(c.client(), o);
    third.begin();
    c.plain_set(2, "zebra", "plain");
    third.put("zebra", set_cmd("zebra", "third"));
    const auto also_lost = third.commit();
    third.drain();
    BOOST_TEST(also_lost._reason == txn_abort_reason::write_conflict);
    BOOST_TEST(c.value("zebra") == std::optional<std::string>("plain"));
}

BOOST_AUTO_TEST_CASE(a_reader_waits_for_an_intent_that_started_below_its_read_point,
                     *boost::unit_test::timeout(60)) {
    cluster c;
    BOOST_REQUIRE(c.await_leaders());
    BOOST_REQUIRE(commit_all(c, {{"apple", "old"}, {"banana", "old"}, {"zebra", "old"}})._kind ==
                  txn_outcome_kind::committed);

    // The writer dies with its primary committed and its secondary on
    // "zebra" still an intent: its commit timestamp is below any start
    // timestamp taken from here on.
    coordinator_type writer(c.client(), options(txn_isolation::snapshot));
    writer.set_crash_hook([](txn_step s) {
        if (s == txn_step::primary_committed) {
            throw coordinator_crashed{};
        }
    });
    writer.begin();
    writer.put("apple", set_cmd("apple", "new"));
    writer.put("banana", set_cmd("banana", "new"));
    writer.put("zebra", set_cmd("zebra", "new"));
    BOOST_CHECK_THROW(writer.commit(), coordinator_crashed);

    coordinator_type reader(c.client(), options(txn_isolation::snapshot));
    reader.begin();
    // Reading "old" here would show half the writer's transaction.
    BOOST_TEST(as_string(reader.get("zebra")) == std::optional<std::string>("new"));
    BOOST_TEST(as_string(reader.get("apple")) == std::optional<std::string>("new"));
    reader.rollback();
}

BOOST_AUTO_TEST_CASE(a_reader_ignores_an_intent_that_started_above_its_read_point,
                     *boost::unit_test::timeout(60)) {
    cluster c;
    BOOST_REQUIRE(c.await_leaders());
    BOOST_REQUIRE(commit_all(c, {{"apple", "old"}, {"zebra", "old"}})._kind ==
                  txn_outcome_kind::committed);

    coordinator_type reader(c.client(), options(txn_isolation::snapshot));
    reader.begin();

    // A writer that started later holds both keys for a long time.
    auto held = options(txn_isolation::snapshot);
    held.lock_ttl = std::chrono::seconds{30};
    coordinator_type writer(c.client(), held);
    writer.set_crash_hook([](txn_step s) {
        if (s == txn_step::prepared) {
            throw coordinator_crashed{};
        }
    });
    writer.begin();
    BOOST_REQUIRE(writer.start_ts() > reader.start_ts());
    writer.put("apple", set_cmd("apple", "new"));
    writer.put("zebra", set_cmd("zebra", "new"));
    BOOST_CHECK_THROW(writer.commit(), coordinator_crashed);
    BOOST_REQUIRE(c.client().read("zebra", k_timeout)._intent.has_value());

    // Its commit, if it ever comes, is above the read point: no wait.
    const auto started = std::chrono::steady_clock::now();
    BOOST_TEST(as_string(reader.get("zebra")) == std::optional<std::string>("old"));
    BOOST_TEST(as_string(reader.get("apple")) == std::optional<std::string>("old"));
    BOOST_TEST((std::chrono::steady_clock::now() - started < std::chrono::seconds{2}));
    reader.rollback();

    // A plain write on the held key is refused, as under tier 1.
    BOOST_CHECK_THROW(c.plain_set(2, "zebra", "plain"), kythira::key_locked_exception<key_type>);
}

BOOST_AUTO_TEST_SUITE_END()
