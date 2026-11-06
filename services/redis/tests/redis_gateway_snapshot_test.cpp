// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file redis_gateway_snapshot_test.cpp
/// @brief The Redis gateway built for snapshot isolation
///        (.kiro/specs/multi-raft-transactions/ task 14): three in-process
///        hosts whose KV state machine is `redis_kv_mvcc_state_machine`
///        wrapped under `mvcc::on`, with a timestamp oracle group beside the
///        shards, each with a gateway on a real TCP port.
///
/// As in redis_gateway_txn_test.cpp, two shards split at "m", so an `a/...`
/// key and a `z/...` key are always on different shards. Every write here,
/// plain or not, takes an oracle timestamp, so a gateway whose host does not
/// lead the oracle reaches its leader over `KYTHIRA.TXN TS`; a snapshot read
/// on a shard this host does not lead goes over `KYTHIRA.TXN READAT`.
/// `WATCH` (task 16) makes the next `EXEC` a serializable transaction that
/// started at the `WATCH`.

#define BOOST_TEST_MODULE redis_gateway_snapshot_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <kythira/redis/redis_gateway_impl.hpp>
#include <kythira/redis/redis_kv_mvcc_state_machine.hpp>
#include <raft/txn_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("redis_gateway_snapshot_test"), nullptr};
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
using kythira::redis_acl;
using kythira::redis_gateway;
using kythira::redis_gateway_config;
using kythira::resp_reply_length;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_range;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_t = std::uint64_t;

using kv_type = kythira::redis_kv_mvcc_state_machine<std::uint64_t>;
using txn_sm_type =
    kythira::transactional_state_machine<kv_type, key_type, kythira::redis_kv_partitioner,
                                         kythira::mvcc::on>;

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
    using state_machine_type = txn_sm_type;

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
using gateway_type = redis_gateway<host_type, kythira::console_logger, kythira::noop_metrics>;

static_assert(gateway_type::k_transactional);
static_assert(gateway_type::k_snapshot);

constexpr group_id_type k_oracle = 1000;

constexpr std::size_t k_node_count = 3;
constexpr std::uint32_t k_kdf_iters = 1000;

auto range_of(std::optional<key_type> start, std::optional<key_type> end) -> shard_range<key_type> {
    return shard_range<key_type>{._start = std::move(start), ._end = std::move(end)};
}

auto acl_text() -> std::string {
    return "user ops " + redis_acl::hash_secret("ops-secret", k_kdf_iters) + " admin *\n" +
           "user farm " + redis_acl::hash_secret("farm-secret", k_kdf_iters) + " read_write a/\n" +
           "user kythira-internal " + redis_acl::hash_secret("internal-secret", k_kdf_iters) +
           " read_write *\n";
}

class resp_client {
public:
    explicit resp_client(std::uint16_t port) : _socket(_io) {
        _socket.connect({boost::asio::ip::make_address("127.0.0.1"), port});
        _socket.set_option(boost::asio::ip::tcp::no_delay(true));
    }

    auto call(const std::vector<std::string>& argv) -> std::string {
        std::string out = "*" + std::to_string(argv.size()) + "\r\n";
        for (const auto& a : argv) {
            out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
        }
        boost::asio::write(_socket, boost::asio::buffer(out));
        std::size_t len = 0;
        while ((len = resp_reply_length(_buffer)) == 0) {
            std::array<char, 65536> chunk{};
            auto n = _socket.read_some(boost::asio::buffer(chunk));
            _buffer.append(chunk.data(), n);
        }
        auto reply = _buffer.substr(0, len);
        _buffer.erase(0, len);
        return reply;
    }

private:
    boost::asio::io_context _io;
    boost::asio::ip::tcp::socket _socket;
    std::string _buffer;
};

auto bulk(const std::string& s) -> std::string {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

auto starts_with(const std::string& s, std::string_view prefix) -> bool {
    return s.rfind(prefix, 0) == 0;
}

/// The GC lifetime is shortened for the safe-point case; the others keep
/// the defaults, under which nothing is collected while they run.
struct gc_tuning {
    std::chrono::milliseconds _lifetime{std::chrono::minutes{10}};
    std::chrono::milliseconds _interval{std::chrono::minutes{1}};
    /// Let `SET`/`MSET` overwrite a value (the gateway's default refuses).
    bool _mutable_values = false;
};

class cluster {
public:
    explicit cluster(gc_tuning gc = {}) {
        _acl.reload(acl_text());
        const std::vector<node_id_t> voters{1, 2, 3};
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id, gc)));
            _hosts.back()->create_group(descriptor_type{._group_id = 1,
                                                        ._range = range_of(std::nullopt, "m"),
                                                        ._epoch = shard_epoch{},
                                                        ._voters = voters});
            _hosts.back()->create_group(descriptor_type{._group_id = 2,
                                                        ._range = range_of("m", std::nullopt),
                                                        ._epoch = shard_epoch{},
                                                        ._voters = voters});
            _hosts.back()->create_oracle_group(voters);
        }
        for (auto& h : _hosts) {
            h->start();
        }
        for (auto& host : _hosts) {
            redis_gateway_config cfg;
            cfg._listen = "127.0.0.1:0";
            cfg._internal_secret = "internal-secret";
            cfg._io_threads = 1;
            cfg._worker_threads = 4;
            cfg._command_timeout = std::chrono::milliseconds{5000};
            cfg._allow_plaintext_forwarding = true;
            cfg._transactions = true;
            cfg._txn_isolation = kythira::txn_isolation::snapshot;
            cfg._immutable_values = !gc._mutable_values;
            auto resolver = [this](const node_id_t& to) -> std::optional<std::string> {
                if (to < 1 || to > _gateways.size() || !_gateways[to - 1]) {
                    return std::nullopt;
                }
                return "127.0.0.1:" + std::to_string(_gateways[to - 1]->port());
            };
            _gateways.push_back(
                std::make_unique<gateway_type>(*host, _acl, _logger, _metrics, cfg, resolver));
        }
        for (auto& g : _gateways) {
            g->start();
        }
        _running = true;
        for (auto& host : _hosts) {
            _drivers.emplace_back([this, h = host.get()] {
                while (_running.load()) {
                    h->tick();
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
        for (auto& g : _gateways) {
            g->stop();
        }
        for (auto& h : _hosts) {
            h->stop();
        }
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;

    [[nodiscard]] auto host(node_id_t id) -> host_type& { return *_hosts.at(id - 1); }
    [[nodiscard]] auto gateway(node_id_t id) -> gateway_type& { return *_gateways.at(id - 1); }
    [[nodiscard]] auto port(node_id_t id) -> std::uint16_t { return _gateways.at(id - 1)->port(); }

    [[nodiscard]] auto leader_of(group_id_type group) -> node_id_t {
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            auto* n = _hosts[id - 1]->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return id;
            }
        }
        return 0;
    }

    auto await_all_leaders(std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (leader_of(1) != 0 && leader_of(2) != 0 && leader_of(k_oracle) != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    /// The safe point of shard `group` on its current leader, or 0.
    [[nodiscard]] auto safe_point(group_id_type group) -> std::uint64_t {
        const auto leader = leader_of(group);
        if (leader == 0) {
            return 0;
        }
        return host(leader)
            .with_group_state_machine(group,
                                      [](txn_sm_type& sm, const auto&) { return sm.safe_point(); })
            .value_or(0);
    }

private:
    auto make_config(node_id_t id, gc_tuning gc) -> config_type {
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
        cfg.txn_gc_lifetime = gc._lifetime;
        cfg.txn_gc_interval = gc._interval;
        if (gc._lifetime < std::chrono::seconds{10}) {
            cfg.resolver_interval = std::chrono::milliseconds{50};
        }
        return cfg;
    }

    message_fabric _fabric{8};
    redis_acl _acl;
    kythira::console_logger _logger{kythira::log_level::warning};
    kythira::noop_metrics _metrics;
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::unique_ptr<gateway_type>> _gateways;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
};

auto be64(std::string_view b) -> std::uint64_t {
    std::uint64_t v = 0;
    for (char c : b) {
        v = (v << 8) | static_cast<unsigned char>(c);
    }
    return v;
}

/// The bulk body of a RESP reply, or "" when it is not a bulk string.
auto bulk_body(const std::string& reply) -> std::string {
    if (!starts_with(reply, "$") || starts_with(reply, "$-1")) {
        return {};
    }
    const auto crlf = reply.find("\r\n");
    return reply.substr(crlf + 2, std::stoull(reply.substr(1, crlf - 1)));
}

}  // namespace

BOOST_AUTO_TEST_SUITE(redis_gateway_snapshot)

BOOST_AUTO_TEST_CASE(snapshot_is_offered_and_is_the_default, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(1));
    BOOST_REQUIRE_EQUAL(client.call({"AUTH", "ops", "ops-secret"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"KYTHIRA.ISOLATION"}), bulk("snapshot"));
    BOOST_CHECK_EQUAL(client.call({"KYTHIRA.ISOLATION", "read_committed"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"KYTHIRA.ISOLATION", "snapshot"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"KYTHIRA.ISOLATION", "serializable"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"KYTHIRA.ISOLATION"}), bulk("serializable"));
    BOOST_CHECK_EQUAL(client.call({"MSET", "a/s", "1", "z/s", "2"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"MGET", "a/s", "z/s"}), "*2\r\n" + bulk("1") + bulk("2"));
}

BOOST_AUTO_TEST_CASE(watch_aborts_an_exec_when_a_watched_key_changed,
                     *boost::unit_test::timeout(180)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        const auto n = std::to_string(id);
        resp_client client(c.port(id));
        resp_client other(c.port((id % k_node_count) + 1));
        BOOST_REQUIRE_EQUAL(client.call({"AUTH", "ops", "ops-secret"}), "+OK\r\n");
        BOOST_REQUIRE_EQUAL(other.call({"AUTH", "ops", "ops-secret"}), "+OK\r\n");

        // Changed after the WATCH, on another gateway: a null array, and
        // nothing the EXEC queued applies.
        BOOST_CHECK_EQUAL(client.call({"WATCH", "a/w" + n}), "+OK\r\n");
        BOOST_CHECK_EQUAL(other.call({"SET", "a/w" + n, "theirs"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"MULTI"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"SET", "z/w" + n, "mine"}), "+QUEUED\r\n");
        BOOST_CHECK_EQUAL(client.call({"EXEC"}), "*-1\r\n");
        BOOST_CHECK_EQUAL(client.call({"GET", "z/w" + n}), "$-1\r\n");

        // EXEC unwatched it: the same transaction again commits.
        BOOST_CHECK_EQUAL(client.call({"MULTI"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"SET", "z/w" + n, "mine"}), "+QUEUED\r\n");
        BOOST_CHECK_EQUAL(client.call({"EXEC"}), "*1\r\n+OK\r\n");

        // Changed, and the EXEC writes that key itself: first-committer-
        // wins refuses it before read validation does, and the answer is
        // still the null array, not an error.
        BOOST_CHECK_EQUAL(client.call({"WATCH", "a/s" + n}), "+OK\r\n");
        BOOST_CHECK_EQUAL(other.call({"SET", "a/s" + n, "theirs"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"MULTI"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"SET", "a/s" + n, "mine"}), "+QUEUED\r\n");
        BOOST_CHECK_EQUAL(client.call({"EXEC"}), "*-1\r\n");
        BOOST_CHECK_EQUAL(client.call({"GET", "a/s" + n}), bulk("theirs"));

        // Unchanged: it commits, writing the watched key itself.
        BOOST_CHECK_EQUAL(client.call({"WATCH", "a/v" + n, "z/v" + n}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"MULTI"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"GET", "a/v" + n}), "+QUEUED\r\n");
        BOOST_CHECK_EQUAL(client.call({"SET", "a/v" + n, "1"}), "+QUEUED\r\n");
        BOOST_CHECK_EQUAL(client.call({"EXEC"}), "*2\r\n$-1\r\n+OK\r\n");
        BOOST_CHECK_EQUAL(other.call({"GET", "a/v" + n}), bulk("1"));

        // UNWATCH forgets: a change after it does not abort.
        BOOST_CHECK_EQUAL(client.call({"WATCH", "a/u" + n}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"UNWATCH"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(other.call({"SET", "a/u" + n, "theirs"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"MULTI"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"SET", "z/u" + n, "mine"}), "+QUEUED\r\n");
        BOOST_CHECK_EQUAL(client.call({"EXEC"}), "*1\r\n+OK\r\n");

        // A read-only EXEC reads as of the WATCH and commits.
        BOOST_CHECK_EQUAL(client.call({"WATCH", "a/r" + n}), "+OK\r\n");
        BOOST_CHECK_EQUAL(other.call({"SET", "a/r" + n, "later"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"MULTI"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"GET", "a/r" + n}), "+QUEUED\r\n");
        BOOST_CHECK_EQUAL(client.call({"EXEC"}), "*1\r\n$-1\r\n");

        // Not inside MULTI.
        BOOST_CHECK_EQUAL(client.call({"MULTI"}), "+OK\r\n");
        BOOST_CHECK(starts_with(client.call({"WATCH", "a/x"}), "-ERR"));
        BOOST_CHECK(starts_with(client.call({"EXEC"}), "-EXECABORT"));
    }

    // The key scope applies to WATCH as to a read.
    resp_client farm(c.port(1));
    BOOST_REQUIRE_EQUAL(farm.call({"AUTH", "farm", "farm-secret"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(farm.call({"WATCH", "z/1"}), "-NOPERM No permissions to access a key\r\n");
    BOOST_CHECK_EQUAL(farm.call({"WATCH", "a/1"}), "+OK\r\n");
}

BOOST_AUTO_TEST_CASE(every_gateway_writes_and_reads_under_snapshot,
                     *boost::unit_test::timeout(180)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        const auto n = std::to_string(id);
        resp_client client(c.port(id));
        BOOST_REQUIRE_EQUAL(client.call({"AUTH", "ops", "ops-secret"}), "+OK\r\n");
        // A plain write takes a timestamp too, from whichever host leads
        // the oracle.
        BOOST_CHECK_EQUAL(client.call({"SET", "a/p" + n, "plain" + n}), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.call({"MSET", "a/" + n, "left" + n, "z/" + n, "right" + n}),
                          "+OK\r\n");
        for (node_id_t other = 1; other <= k_node_count; ++other) {
            resp_client reader(c.port(other));
            BOOST_REQUIRE_EQUAL(reader.call({"AUTH", "ops", "ops-secret"}), "+OK\r\n");
            BOOST_CHECK_EQUAL(
                reader.call({"MGET", "a/" + n, "z/" + n, "a/p" + n, "a/none"}),
                "*4\r\n" + bulk("left" + n) + bulk("right" + n) + bulk("plain" + n) + "$-1\r\n");
            BOOST_CHECK_EQUAL(reader.call({"GET", "z/" + n}), bulk("right" + n));
        }
        BOOST_CHECK_EQUAL(client.call({"DEL", "a/" + n, "z/" + n, "a/none"}), ":2\r\n");
        BOOST_CHECK_EQUAL(client.call({"MGET", "a/" + n, "z/" + n}), "*2\r\n$-1\r\n$-1\r\n");
        BOOST_CHECK_EQUAL(client.call({"DEL", "a/p" + n}), ":1\r\n");
        BOOST_CHECK_EQUAL(client.call({"GET", "a/p" + n}), "$-1\r\n");
    }
    std::uint64_t committed = 0;
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        committed += c.gateway(id).stats()._txn_committed.load();
        BOOST_CHECK_EQUAL(c.gateway(id).stats()._txn_unknown.load(), 0u);
    }
    BOOST_CHECK_GE(committed, 2 * k_node_count);
}

BOOST_AUTO_TEST_CASE(a_snapshot_mget_reads_one_point_in_time, *boost::unit_test::timeout(240)) {
    cluster c(gc_tuning{._mutable_values = true});
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    {
        resp_client seed(c.port(1));
        BOOST_REQUIRE_EQUAL(seed.call({"AUTH", "ops", "ops-secret"}), "+OK\r\n");
        BOOST_REQUIRE_EQUAL(seed.call({"MSET", "a/pair", "0", "z/pair", "0"}), "+OK\r\n");
    }
    // One writer keeps both halves equal, a new value per MSET; readers on
    // every gateway must never see the halves differ.
    std::atomic<bool> stop{false};
    std::atomic<int> writes{0};
    std::thread writer([&] {
        resp_client w(c.port(1));
        if (w.call({"AUTH", "ops", "ops-secret"}) != "+OK\r\n") {
            return;
        }
        for (int i = 1; !stop.load(); ++i) {
            const auto v = std::to_string(i);
            if (w.call({"MSET", "a/pair", v, "z/pair", v}) == "+OK\r\n") {
                ++writes;
            }
        }
    });
    int reads = 0;
    int torn = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{6};
    while (std::chrono::steady_clock::now() < until) {
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            resp_client reader(c.port(id));
            BOOST_REQUIRE_EQUAL(reader.call({"AUTH", "ops", "ops-secret"}), "+OK\r\n");
            const auto reply = reader.call({"MGET", "a/pair", "z/pair"});
            if (!starts_with(reply, "*2\r\n")) {
                continue;  // a retryable error under churn
            }
            ++reads;
            // Equal halves are two byte-identical items after the header.
            const auto items = reply.substr(4);
            if (items.size() % 2 != 0 ||
                items.substr(0, items.size() / 2) != items.substr(items.size() / 2)) {
                ++torn;
                BOOST_TEST_MESSAGE("torn snapshot read: " << reply);
            }
        }
    }
    stop = true;
    writer.join();
    BOOST_TEST_MESSAGE("reads " << reads << ", writes " << writes.load());
    BOOST_CHECK_GT(reads, 0);
    BOOST_CHECK_GT(writes.load(), 0);
    BOOST_CHECK_EQUAL(torn, 0);
}

BOOST_AUTO_TEST_CASE(kythira_txn_ts_and_readat_are_internal_and_one_hop,
                     *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    const auto oracle = c.leader_of(k_oracle);
    resp_client client(c.port(oracle));
    BOOST_REQUIRE_EQUAL(client.call({"AUTH", "ops", "ops-secret"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"KYTHIRA.TXN", "TS", "1"}),
                      "-NOPERM KYTHIRA.TXN is internal to the gateways\r\n");

    resp_client internal(c.port(oracle));
    BOOST_REQUIRE_EQUAL(internal.call({"AUTH", "kythira-internal", "internal-secret"}), "+OK\r\n");
    const auto first = bulk_body(internal.call({"KYTHIRA.TXN", "TS", "4"}));
    BOOST_REQUIRE_EQUAL(first.size(), 16u);
    BOOST_CHECK_GT(be64(first.substr(0, 8)), 0u);
    BOOST_CHECK_GE(be64(first.substr(8)), 4u);
    const auto second = bulk_body(internal.call({"KYTHIRA.TXN", "TS", "1"}));
    BOOST_REQUIRE_EQUAL(second.size(), 16u);
    BOOST_CHECK_GE(be64(second.substr(0, 8)), be64(first.substr(0, 8)) + 4);
    BOOST_CHECK(starts_with(internal.call({"KYTHIRA.TXN", "TS", "0"}), "-ERR wrong arguments"));

    // Not the oracle's leader: the retry error, never a second hop.
    const auto follower = oracle == 1 ? 2 : 1;
    resp_client internal_follower(c.port(follower));
    BOOST_REQUIRE_EQUAL(internal_follower.call({"AUTH", "kythira-internal", "internal-secret"}),
                        "+OK\r\n");
    BOOST_CHECK_EQUAL(internal_follower.call({"KYTHIRA.TXN", "TS", "1"}),
                      "-ERR not the timestamp oracle's leader, retry\r\n");

    // READAT on the shard's leader: the version at or below the timestamp.
    resp_client writer(c.port(1));
    BOOST_REQUIRE_EQUAL(writer.call({"AUTH", "ops", "ops-secret"}), "+OK\r\n");
    BOOST_REQUIRE_EQUAL(writer.call({"SET", "a/v", "one"}), "+OK\r\n");
    const auto between = bulk_body(internal.call({"KYTHIRA.TXN", "TS", "1"}));
    BOOST_REQUIRE_EQUAL(between.size(), 16u);
    const auto ts = std::to_string(be64(between.substr(0, 8)));
    BOOST_REQUIRE_EQUAL(writer.call({"DEL", "a/v"}), ":1\r\n");
    const auto shard_leader = c.leader_of(1);
    resp_client at_leader(c.port(shard_leader));
    BOOST_REQUIRE_EQUAL(at_leader.call({"AUTH", "kythira-internal", "internal-secret"}), "+OK\r\n");
    // Present, then three bytes, then no intent.
    BOOST_CHECK_EQUAL(at_leader.call({"KYTHIRA.TXN", "READAT", ts, "a/v"}),
                      bulk(std::string("\1\0\0\0\3one\0", 9)));
    const auto other = shard_leader == 1 ? 2 : 1;
    resp_client at_other(c.port(other));
    BOOST_REQUIRE_EQUAL(at_other.call({"AUTH", "kythira-internal", "internal-secret"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(at_other.call({"KYTHIRA.TXN", "READAT", ts, "a/v"}),
                      "-ERR shard has no reachable leader, retry\r\n");
}

BOOST_AUTO_TEST_CASE(a_read_below_the_safe_point_is_refused, *boost::unit_test::timeout(180)) {
    cluster c(gc_tuning{._lifetime = std::chrono::milliseconds{400},
                        ._interval = std::chrono::milliseconds{0}});
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    const auto oracle = c.leader_of(k_oracle);
    resp_client internal(c.port(oracle));
    BOOST_REQUIRE_EQUAL(internal.call({"AUTH", "kythira-internal", "internal-secret"}), "+OK\r\n");
    resp_client writer(c.port(1));
    BOOST_REQUIRE_EQUAL(writer.call({"AUTH", "ops", "ops-secret"}), "+OK\r\n");
    BOOST_REQUIRE_EQUAL(writer.call({"SET", "a/old", "1"}), "+OK\r\n");
    const auto taken = bulk_body(internal.call({"KYTHIRA.TXN", "TS", "1"}));
    BOOST_REQUIRE_EQUAL(taken.size(), 16u);
    const auto old_ts = be64(taken.substr(0, 8));

    // Keep writing until the shard's safe point passes the old timestamp.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    int i = 0;
    while (c.safe_point(1) <= old_ts && std::chrono::steady_clock::now() < deadline) {
        BOOST_REQUIRE_EQUAL(writer.call({"SET", "a/tick" + std::to_string(i++), "x"}), "+OK\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    const auto safe = c.safe_point(1);
    BOOST_REQUIRE_GT(safe, old_ts);

    // Over the internal hop: the too-old refusal with both timestamps.
    const auto shard_leader = c.leader_of(1);
    resp_client at_leader(c.port(shard_leader));
    BOOST_REQUIRE_EQUAL(at_leader.call({"AUTH", "kythira-internal", "internal-secret"}), "+OK\r\n");
    const auto refused = at_leader.call({"KYTHIRA.TXN", "READAT", std::to_string(old_ts), "a/old"});
    BOOST_CHECK(starts_with(refused, "-KYTHIRA.TOOOLD " + std::to_string(old_ts) + " "));

    // And at the client, through every gateway: a fresh snapshot still reads.
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        resp_client reader(c.port(id));
        BOOST_REQUIRE_EQUAL(reader.call({"AUTH", "ops", "ops-secret"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(reader.call({"MGET", "a/old", "z/none"}),
                          "*2\r\n" + bulk("1") + "$-1\r\n");
    }
}

BOOST_AUTO_TEST_SUITE_END()
