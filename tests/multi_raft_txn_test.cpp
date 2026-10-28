// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_txn_test.cpp
/// @brief The transaction coordinator over three in-process hosts (task 6
///        of `.kiro/specs/multi-raft-transactions/`).
///
/// Three hosts, each with a replica of two shards split at "m", every shard
/// wrapping `test_key_value_state_machine` in `transactional_state_machine`.
/// A transaction that writes "apple" and "zebra" therefore spans both shards
/// and takes the two-phase path; one that writes "apple" and "banana" takes
/// the one-phase path.
///
/// The property that matters is all-or-nothing after resolution: the crash
/// cases stop the coordinator after every protocol step, as a process death
/// would, then let a second coordinator resolve what was left by reading
/// the keys under `read_committed_wait`. Either every write of the crashed
/// transaction is visible afterwards or none is, and which one follows from
/// where it stopped: before the commit point, none; after it, all.

#define BOOST_TEST_MODULE multi_raft_txn_test
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
        char* argv_data[] = {const_cast<char*>("multi_raft_txn_test"), nullptr};
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
BOOST_TEST_DONT_PRINT_LOG_VALUE(kythira::txn_record_status)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<std::string>)

namespace {

using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_range;
using kythira::txn_abort_reason;
using kythira::txn_id;
using kythira::txn_isolation;
using kythira::txn_outcome_kind;
using kythira::txn_record_status;
using kythira::txn_step;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::kv_partitioner;
using kythira::testing::kv_put;
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

static_assert(kythira::txn_participant_client<client_type, key_type>);

constexpr node_id_t k_node_count = 3;

auto as_bytes(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (char c : s) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

auto as_string(const std::optional<std::vector<std::byte>>& b) -> std::optional<std::string> {
    if (!b.has_value()) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(b->data()), b->size());  // NOLINT
}

auto kv_value_of(const kv_sm& sm, const key_type& k) -> std::optional<std::vector<std::byte>> {
    auto v = sm.get_value(k);
    if (!v.has_value()) {
        return std::nullopt;
    }
    return as_bytes(*v);
}

/// Three hosts over one fabric, each driven by its own thread.
class cluster {
public:
    cluster() {
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
        _client = std::make_unique<client_type>(std::move(raw), kv_value_of);
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
            if (has_leader(1) && has_leader(2)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    /// The committed value of `key` on its shard's leader.
    [[nodiscard]] auto value(const key_type& key) -> std::optional<std::string> {
        return as_string(_client->read(key, std::chrono::seconds{2})._value);
    }

    [[nodiscard]] auto intent_on(const key_type& key) -> bool {
        return _client->read(key, std::chrono::seconds{2})._intent.has_value();
    }

private:
    auto has_leader(group_id_type g) -> bool {
        for (auto& h : _hosts) {
            auto* n = h->group_node(g);
            if (n != nullptr && n->is_leader()) {
                return true;
            }
        }
        return false;
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
        return cfg;
    }

    message_fabric _fabric{8};
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
    std::unique_ptr<client_type> _client;
};

/// Short TTL, so the crash cases resolve in test time.
auto fast_options(txn_isolation isolation = txn_isolation::read_committed) -> kythira::txn_options {
    kythira::txn_options o;
    o.isolation = isolation;
    o.lock_ttl = std::chrono::milliseconds{300};
    o.timeout = std::chrono::seconds{5};
    return o;
}

struct coordinator_crashed : std::runtime_error {
    coordinator_crashed() : std::runtime_error("coordinator crashed") {}
};

/// A client whose routing is stale until its first repair: every key
/// resolves to shard 1, as if the split at "m" had not yet been seen.
class stale_route_client {
public:
    using group_id_type = ::group_id_type;

    explicit stale_route_client(client_type& inner) : _inner(inner) {}

    [[nodiscard]] auto resolve(const key_type& key) -> std::optional<group_id_type> {
        if (!_repaired) {
            return group_id_type{1};
        }
        return _inner.resolve(key);
    }
    [[nodiscard]] auto send(const std::vector<key_type>& keys, const std::vector<std::byte>& env,
                            std::chrono::milliseconds timeout) -> client_type::send_future {
        return _inner.send(keys, env, timeout);
    }
    [[nodiscard]] auto refresh(std::exception_ptr error) -> bool {
        const bool ok = _inner.refresh(std::move(error));
        if (ok) {
            _repaired = true;
            ++_repairs;
        }
        return ok;
    }
    [[nodiscard]] auto read(const key_type& key, std::chrono::milliseconds timeout)
        -> kythira::txn_read_result<key_type> {
        return _inner.read(key, timeout);
    }
    [[nodiscard]] auto now_ms() const -> std::uint64_t { return _inner.now_ms(); }

    [[nodiscard]] auto repairs() const -> int { return _repairs; }

private:
    client_type& _inner;
    bool _repaired = false;
    int _repairs = 0;
};

static_assert(kythira::txn_participant_client<stale_route_client, key_type>);

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_txn)

BOOST_AUTO_TEST_CASE(a_single_shard_transaction_commits_in_one_phase,
                     *boost::unit_test::timeout(60)) {
    cluster c;
    BOOST_REQUIRE(c.await_leaders());
    coordinator_type txn(c.client(), fast_options());
    txn.begin();
    txn.put("apple", kv_put("apple", "1"));
    txn.put("banana", kv_put("banana", "2"));
    const auto out = txn.commit();
    BOOST_TEST(out._kind == txn_outcome_kind::committed);
    BOOST_TEST(!out._primary.has_value());  // no primary: one entry, one shard
    BOOST_TEST(c.value("apple") == std::optional<std::string>("1"));
    BOOST_TEST(c.value("banana") == std::optional<std::string>("2"));
    BOOST_TEST(!c.intent_on("apple"));
}

BOOST_AUTO_TEST_CASE(a_cross_shard_transaction_commits_on_both_shards,
                     *boost::unit_test::timeout(60)) {
    cluster c;
    BOOST_REQUIRE(c.await_leaders());
    coordinator_type txn(c.client(), fast_options());
    const auto id = txn.begin();
    txn.put("apple", kv_put("apple", "a"));
    txn.put("banana", kv_put("banana", "b"));
    txn.put("zebra", kv_put("zebra", "z"));
    const auto out = txn.commit();
    txn.drain();
    BOOST_TEST(out._kind == txn_outcome_kind::committed);
    // The largest group is shard 1 (apple, banana); its smallest key leads.
    BOOST_TEST(out._primary == std::optional<std::string>("apple"));
    BOOST_TEST(c.value("apple") == std::optional<std::string>("a"));
    BOOST_TEST(c.value("banana") == std::optional<std::string>("b"));
    BOOST_TEST(c.value("zebra") == std::optional<std::string>("z"));
    BOOST_TEST(!c.intent_on("zebra"));
    BOOST_TEST(txn.transaction_status(id, "apple") == txn_record_status::committed);
}

BOOST_AUTO_TEST_CASE(a_stale_route_is_repaired_and_the_keys_re_partitioned,
                     *boost::unit_test::timeout(60)) {
    cluster c;
    BOOST_REQUIRE(c.await_leaders());
    stale_route_client stale(c.client());
    kythira::transaction_coordinator<stale_route_client, key_type> txn(stale, fast_options());
    txn.begin();
    txn.put("apple", kv_put("apple", "a"));
    txn.put("zebra", kv_put("zebra", "z"));
    // The stale map puts both keys on shard 1, so the coordinator tries one
    // phase there; shard 1 refuses "zebra", the client repairs, and the
    // transaction goes again over two shards.
    const auto out = txn.commit();
    txn.drain();
    BOOST_TEST(out._kind == txn_outcome_kind::committed);
    BOOST_TEST(stale.repairs() >= 1);
    BOOST_TEST(out._primary.has_value());
    BOOST_TEST(c.value("apple") == std::optional<std::string>("a"));
    BOOST_TEST(c.value("zebra") == std::optional<std::string>("z"));
    BOOST_TEST(!c.intent_on("apple"));
    BOOST_TEST(!c.intent_on("zebra"));
}

BOOST_AUTO_TEST_CASE(a_conflict_aborts_and_leaves_nothing_behind, *boost::unit_test::timeout(60)) {
    cluster c;
    BOOST_REQUIRE(c.await_leaders());
    // Another transaction holds "zebra": prepared, never committed.
    coordinator_type holder(c.client(), fast_options());
    holder.set_crash_hook([](txn_step s) {
        if (s == txn_step::prepared) {
            throw coordinator_crashed{};
        }
    });
    holder.begin();
    holder.put("mango", kv_put("mango", "h"));
    holder.put("zebra", kv_put("zebra", "h"));
    holder.put("apple", kv_put("apple", "h"));
    BOOST_CHECK_THROW(holder.commit(), coordinator_crashed);
    BOOST_REQUIRE(c.intent_on("zebra"));

    kythira::txn_options opts = fast_options();
    opts.lock_ttl = std::chrono::seconds{30};
    coordinator_type txn(c.client(), opts);
    txn.begin();
    txn.put("banana", kv_put("banana", "x"));
    txn.put("zebra", kv_put("zebra", "x"));
    const auto out = txn.commit();
    txn.drain();
    BOOST_TEST(out._kind == txn_outcome_kind::aborted);
    BOOST_TEST(out._reason == txn_abort_reason::write_conflict);
    BOOST_TEST(out._conflict_key == std::optional<std::string>("zebra"));
    // Its own prepare on the other shard was rolled back.
    BOOST_TEST(!c.value("banana").has_value());
    BOOST_TEST(!c.intent_on("banana"));
}

BOOST_AUTO_TEST_CASE(read_committed_never_sees_an_intent_and_never_waits,
                     *boost::unit_test::timeout(60)) {
    cluster c;
    BOOST_REQUIRE(c.await_leaders());
    {
        coordinator_type setup(c.client(), fast_options());
        setup.begin();
        setup.put("apple", kv_put("apple", "old"));
        BOOST_REQUIRE(setup.commit()._kind == txn_outcome_kind::committed);
    }
    kythira::txn_options long_ttl = fast_options();
    long_ttl.lock_ttl = std::chrono::seconds{30};
    coordinator_type writer(c.client(), long_ttl);
    writer.set_crash_hook([](txn_step s) {
        if (s == txn_step::prepared) {
            throw coordinator_crashed{};
        }
    });
    writer.begin();
    writer.put("apple", kv_put("apple", "new"));
    writer.put("zebra", kv_put("zebra", "new"));
    BOOST_CHECK_THROW(writer.commit(), coordinator_crashed);

    coordinator_type reader(c.client(), fast_options(txn_isolation::read_committed));
    reader.begin();
    const auto started = std::chrono::steady_clock::now();
    BOOST_TEST(as_string(reader.get("apple")) == std::optional<std::string>("old"));
    BOOST_CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{1});
    reader.rollback();
}

BOOST_AUTO_TEST_CASE(read_committed_wait_resolves_an_expired_intent,
                     *boost::unit_test::timeout(60)) {
    cluster c;
    BOOST_REQUIRE(c.await_leaders());
    coordinator_type writer(c.client(), fast_options());
    writer.set_crash_hook([](txn_step s) {
        if (s == txn_step::prepared) {
            throw coordinator_crashed{};
        }
    });
    const auto id = writer.begin();
    writer.put("apple", kv_put("apple", "lost"));
    writer.put("zebra", kv_put("zebra", "lost"));
    BOOST_CHECK_THROW(writer.commit(), coordinator_crashed);
    BOOST_REQUIRE(c.intent_on("zebra"));

    coordinator_type reader(c.client(), fast_options(txn_isolation::read_committed_wait));
    reader.begin();
    // Waits out the 300 ms TTL, resolves, then reads the committed value:
    // the crashed transaction never reached its commit point, so none.
    BOOST_TEST(!reader.get("zebra").has_value());
    reader.rollback();
    BOOST_TEST(!c.intent_on("zebra"));
    BOOST_TEST(reader.transaction_status(id, "apple") == txn_record_status::aborted);
}

BOOST_AUTO_TEST_CASE(every_crash_point_is_all_or_nothing_after_resolution,
                     *boost::unit_test::timeout(240)) {
    for (const auto step : {txn_step::prepares_sent, txn_step::prepared,
                            txn_step::primary_committed, txn_step::secondaries_sent}) {
        BOOST_TEST_CONTEXT("crash after step " << static_cast<int>(step)) {
            cluster c;
            BOOST_REQUIRE(c.await_leaders());
            coordinator_type writer(c.client(), fast_options());
            writer.set_crash_hook([step](txn_step s) {
                if (s == step) {
                    throw coordinator_crashed{};
                }
            });
            writer.begin();
            writer.put("apple", kv_put("apple", "v"));
            writer.put("mango", kv_put("mango", "v"));
            writer.put("zebra", kv_put("zebra", "v"));
            BOOST_CHECK_THROW(writer.commit(), coordinator_crashed);
            writer.drain();

            // Resolution: a second coordinator reads every key, waiting out
            // and resolving whatever the first left locked.
            coordinator_type reader(c.client(), fast_options(txn_isolation::read_committed_wait));
            reader.begin();
            std::vector<std::optional<std::string>> seen;
            for (const auto* k : {"apple", "mango", "zebra"}) {
                seen.push_back(as_string(reader.get(k)));
            }
            reader.rollback();

            const bool committed =
                step == txn_step::primary_committed || step == txn_step::secondaries_sent;
            for (const auto& v : seen) {
                BOOST_TEST(v == (committed ? std::optional<std::string>("v") : std::nullopt));
            }
            for (const auto* k : {"apple", "mango", "zebra"}) {
                BOOST_TEST(!c.intent_on(k));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(an_isolation_the_build_lacks_is_refused_at_begin) {
    struct never_called {
        using group_id_type = std::uint64_t;
        auto resolve(const key_type&) -> std::optional<group_id_type> { return std::nullopt; }
        auto send(const std::vector<key_type>&, const std::vector<std::byte>&,
                  std::chrono::milliseconds) -> client_type::send_future {
            throw std::logic_error("never_called: send");
        }
        auto refresh(std::exception_ptr) -> bool { return false; }
        auto read(const key_type&, std::chrono::milliseconds)
            -> kythira::txn_read_result<key_type> {
            return {};
        }
        [[nodiscard]] auto now_ms() const -> std::uint64_t { return 0; }
    };
    never_called client;
    kythira::transaction_coordinator<never_called, key_type> txn(client);
    kythira::txn_options o;
    o.isolation = txn_isolation::snapshot;
    BOOST_CHECK_THROW(txn.begin(o), kythira::isolation_unsupported_exception);
    o.isolation = txn_isolation::serializable;
    BOOST_CHECK_THROW(txn.begin(o), kythira::isolation_unsupported_exception);
    o.isolation = txn_isolation::read_committed;
    BOOST_CHECK_NO_THROW(txn.begin(o));
    BOOST_CHECK_THROW(txn.begin(o), std::logic_error);
    txn.rollback();
}

BOOST_AUTO_TEST_SUITE_END()
