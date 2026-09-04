// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file redis_gateway_integration_test.cpp
/// @brief Three in-process `multi_raft` hosts, each with a `redis_gateway` on
///        a real TCP port, driven by a hand-rolled RESP client
///        (.kiro/specs/redis-compatible-kv/ tasks 5-9 and 11).
///
/// The client is deliberately not a Redis library: it sends exactly the bytes
/// the test names and compares exactly the bytes that come back, so a reply
/// that redis-rs would tolerate but the spec forbids still fails. The real
/// sccache acceptance lives under tests/docker_chaos/sccache_e2e/.

#define BOOST_TEST_MODULE redis_gateway_integration_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/redis_gateway_impl.hpp>
#include <raft/redis_kv_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <boost/asio.hpp>

#include <array>
#include <atomic>
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
        char* argv_data[] = {const_cast<char*>("redis_gateway_integration_test"), nullptr};
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
using kythira::redis_read_consistency;
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
    using state_machine_type = kythira::redis_kv_state_machine<log_index_type>;

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

constexpr std::size_t k_node_count = 3;
constexpr std::size_t k_shard_count = 2;
constexpr std::uint32_t k_kdf_iters = 1000;
/// Every `sccache/...` key sorts above the "m" cut, so it lives in shard 2.
/// Tests that inspect a shard's node directly must look there, not in shard 1.
constexpr group_id_type k_sccache_group = 2;

auto range_of(std::optional<key_type> start, std::optional<key_type> end) -> shard_range<key_type> {
    return shard_range<key_type>{._start = std::move(start), ._end = std::move(end)};
}

/// Two shards split at "m", every one replicated on all three nodes.
auto static_shards() -> std::vector<descriptor_type> {
    const std::vector<node_id_t> voters{1, 2, 3};
    return {
        descriptor_type{._group_id = 1,
                        ._range = range_of(std::nullopt, key_type{"m"}),
                        ._epoch = shard_epoch{},
                        ._voters = voters},
        descriptor_type{._group_id = 2,
                        ._range = range_of(key_type{"m"}, std::nullopt),
                        ._epoch = shard_epoch{},
                        ._voters = voters},
    };
}

auto acl_text() -> std::string {
    return "user farm " + redis_acl::hash_secret("farm-secret", k_kdf_iters) +
           " read_write sccache/\n" + "user reader " +
           redis_acl::hash_secret("reader-secret", k_kdf_iters) + " read_only sccache/\n" +
           "user ops " + redis_acl::hash_secret("ops-secret", k_kdf_iters) + " admin *\n" +
           "user kythira-internal " + redis_acl::hash_secret("internal-secret", k_kdf_iters) +
           " read_write *\n";
}

// ── a minimal RESP client ────────────────────────────────────────────────────

class resp_client {
public:
    explicit resp_client(std::uint16_t port) : _socket(_io) {
        boost::asio::ip::tcp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"), port);
        _socket.connect(ep);
        _socket.set_option(boost::asio::ip::tcp::no_delay(true));
    }

    static auto encode(const std::vector<std::string>& argv) -> std::string {
        std::string out = "*" + std::to_string(argv.size()) + "\r\n";
        for (const auto& a : argv) {
            out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
        }
        return out;
    }

    auto send_raw(const std::string& bytes) -> void {
        boost::asio::write(_socket, boost::asio::buffer(bytes));
    }

    /// Send one command and return its raw reply.
    auto call(const std::vector<std::string>& argv) -> std::string {
        send_raw(encode(argv));
        return read_reply();
    }

    auto read_reply() -> std::string {
        std::size_t len = 0;
        while ((len = resp_reply_length(_buffer)) == 0) {
            std::array<char, 65536> chunk{};
            boost::system::error_code ec;
            auto n = _socket.read_some(boost::asio::buffer(chunk), ec);
            if (ec) {
                throw std::runtime_error("connection closed: " + ec.message());
            }
            _buffer.append(chunk.data(), n);
        }
        auto reply = _buffer.substr(0, len);
        _buffer.erase(0, len);
        return reply;
    }

    /// True once the peer has closed the connection (EOF on read).
    auto wait_closed() -> bool {
        std::array<char, 16> chunk{};
        boost::system::error_code ec;
        while (true) {
            auto n = _socket.read_some(boost::asio::buffer(chunk), ec);
            if (ec == boost::asio::error::eof) {
                return true;
            }
            if (ec) {
                return true;
            }
            _buffer.append(chunk.data(), n);
        }
    }

    auto auth(const std::string& user, const std::string& secret) -> std::string {
        return call({"AUTH", user, secret});
    }

private:
    boost::asio::io_context _io;
    boost::asio::ip::tcp::socket _socket;
    std::string _buffer;
};

auto bulk(const std::string& s) -> std::string {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

// ── a three-node cluster with a gateway per node ─────────────────────────────

class cluster {
public:
    explicit cluster(redis_gateway_config base = {}) {
        _acl.reload(acl_text());
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
            for (const auto& shard : static_shards()) {
                _hosts.back()->create_group(shard);
            }
        }
        for (auto& h : _hosts) {
            h->start();
        }
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            auto cfg = base;
            cfg._listen = "127.0.0.1:0";
            cfg._internal_secret = "internal-secret";
            cfg._io_threads = 1;
            cfg._worker_threads = 4;
            cfg._command_timeout = std::chrono::milliseconds{3000};
            auto resolver = [this](const node_id_t& to) -> std::optional<std::string> {
                if (to < 1 || to > _gateways.size() || !_gateways[to - 1]) {
                    return std::nullopt;
                }
                return "127.0.0.1:" + std::to_string(_gateways[to - 1]->port());
            };
            _gateways.push_back(
                std::make_unique<gateway_type>(*_hosts[i], _acl, _logger, _metrics, cfg, resolver));
        }
        for (auto& g : _gateways) {
            g->start();
        }
        _running = true;
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            _drivers.emplace_back([this, i] { drive(i); });
        }
    }

    ~cluster() {
        _running = false;
        for (auto& t : _drivers) {
            if (t.joinable()) {
                t.join();
            }
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

    [[nodiscard]] auto a_follower_of(group_id_type group) -> node_id_t {
        auto leader = leader_of(group);
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            if (id != leader) {
                return id;
            }
        }
        return 0;
    }

    auto await_all_leaders(std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            bool all = true;
            for (group_id_type g = 1; g <= k_shard_count; ++g) {
                if (leader_of(g) == 0) {
                    all = false;
                    break;
                }
            }
            if (all) {
                // Let the leaders' no-op entries commit so the first write does
                // not race the term's first heartbeat.
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    /// Run the gateway's maintenance on the leader of `group` right now.
    auto maintain(group_id_type group) -> std::size_t {
        auto leader = leader_of(group);
        if (leader == 0) {
            return 0;
        }
        return _gateways[leader - 1]->run_maintenance();
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
        return cfg;
    }

    auto drive(std::size_t index) -> void {
        while (_running.load()) {
            _hosts[index]->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
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

}  // namespace

BOOST_AUTO_TEST_SUITE(redis_gateway)

BOOST_AUTO_TEST_CASE(sccache_handshake_pipeline_and_round_trip, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    auto leader = c.leader_of(2);
    resp_client client(c.port(leader));

    // redis-rs opens with this exact pipeline; the replies must come back in
    // order as one stream (Requirement 1.2, 1.5).
    client.send_raw(resp_client::encode({"AUTH", "farm", "farm-secret"}) +
                    resp_client::encode({"SELECT", "0"}) +
                    resp_client::encode({"CLIENT", "SETINFO", "LIB-NAME", "redis-rs"}) +
                    resp_client::encode({"CLIENT", "SETINFO", "LIB-VER", "0.25.0"}));
    BOOST_CHECK_EQUAL(client.read_reply(), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.read_reply(), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.read_reply(), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.read_reply(), "+OK\r\n");

    BOOST_CHECK_EQUAL(client.call({"PING"}), "+PONG\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/missing"}), "$-1\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/obj1", "object-bytes"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/obj1"}), bulk("object-bytes"));
    BOOST_CHECK_EQUAL(client.call({"EXISTS", "sccache/obj1"}), ":1\r\n");
    BOOST_CHECK_EQUAL(client.call({"STRLEN", "sccache/obj1"}), ":12\r\n");
    BOOST_CHECK_EQUAL(client.call({"GETRANGE", "sccache/obj1", "0", "5"}), bulk("object"));
    BOOST_CHECK_EQUAL(client.call({"GETRANGE", "sccache/obj1", "-5", "-1"}), bulk("bytes"));
    BOOST_CHECK_EQUAL(client.call({"TTL", "sccache/obj1"}), ":-1\r\n");
    BOOST_CHECK_EQUAL(client.call({"DEL", "sccache/obj1"}), ":1\r\n");
    BOOST_CHECK_EQUAL(client.call({"DEL", "sccache/obj1"}), ":0\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/obj1"}), "$-1\r\n");

    // Binary-safe: values may contain CRLF and NULs (Requirement 1.3).
    std::string binary("a\r\n\0b\xff", 6);
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/bin", binary}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/bin"}), bulk(binary));

    BOOST_CHECK_EQUAL(client.call({"SELECT", "1"}), "-ERR DB index is out of range\r\n");
    BOOST_CHECK_EQUAL(client.call({"ECHO", "hi"}), bulk("hi"));
    BOOST_CHECK_EQUAL(client.call({"GET"}), "-ERR wrong number of arguments for 'get' command\r\n");
    auto unknown = client.call({"FLUSHALL"});
    BOOST_CHECK(unknown.rfind("-ERR unknown command 'FLUSHALL'", 0) == 0);
    // ...and the connection is still usable afterwards (Requirement 1.8).
    BOOST_CHECK_EQUAL(client.call({"PING"}), "+PONG\r\n");
    BOOST_CHECK_EQUAL(client.call({"QUIT"}), "+OK\r\n");
    BOOST_CHECK(client.wait_closed());
}

BOOST_AUTO_TEST_CASE(hello_3_switches_the_reply_encoding, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(c.leader_of(1)));

    BOOST_CHECK_EQUAL(client.call({"HELLO", "4"}), "-NOPROTO unsupported protocol version\r\n");
    auto hello = client.call({"HELLO", "3", "AUTH", "farm", "farm-secret", "SETNAME", "t"});
    BOOST_CHECK(hello.rfind("%", 0) == 0);
    BOOST_CHECK(hello.find("$6\r\nserver\r\n$7\r\nkythira\r\n") != std::string::npos);
    BOOST_CHECK(hello.find("$5\r\nproto\r\n:3\r\n") != std::string::npos);
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/none"}), "_\r\n");
    BOOST_CHECK_EQUAL(client.call({"CLIENT", "GETNAME"}), bulk("t"));

    BOOST_CHECK_EQUAL(client.call({"RESET"}), "+RESET\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/none"}), "-NOAUTH Authentication required.\r\n");
    auto hello2 = client.call({"HELLO", "2", "AUTH", "farm", "farm-secret"});
    BOOST_CHECK(hello2.rfind("*", 0) == 0);
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/none"}), "$-1\r\n");
}

BOOST_AUTO_TEST_CASE(authentication_and_authorization, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._auth_failure_limit = 3;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    auto port = c.port(c.leader_of(1));

    {
        resp_client client(port);
        BOOST_CHECK_EQUAL(client.call({"GET", "sccache/x"}),
                          "-NOAUTH Authentication required.\r\n");
        BOOST_CHECK_EQUAL(client.call({"PING"}), "-NOAUTH Authentication required.\r\n");
        const std::string wrongpass =
            "-WRONGPASS invalid username-password pair or user is disabled.\r\n";
        BOOST_CHECK_EQUAL(client.auth("farm", "nope"), wrongpass);
        BOOST_CHECK_EQUAL(client.auth("ghost", "nope"), wrongpass);
        BOOST_CHECK_EQUAL(client.call({"AUTH", "nope"}), wrongpass);  // "default" user
        // Fourth failure from this source inside the window is refused
        // without running the KDF.
        auto limited = client.auth("farm", "farm-secret");
        BOOST_CHECK(limited.rfind("-ERR too many authentication failures", 0) == 0);
        BOOST_CHECK_GE(c.gateway(c.leader_of(1)).stats()._auth_failures.load(), 4u);
    }
    {
        // Rate limit is per source address, which for loopback tests is per
        // port, so a fresh socket is a fresh source.
        resp_client reader(port);
        BOOST_CHECK_EQUAL(reader.auth("reader", "reader-secret"), "+OK\r\n");
        BOOST_CHECK_EQUAL(reader.call({"GET", "sccache/x"}), "$-1\r\n");
        BOOST_CHECK_EQUAL(reader.call({"SET", "sccache/x", "v"}),
                          "-NOPERM User reader has no permissions to run the 'SET' command\r\n");
        BOOST_CHECK_EQUAL(reader.call({"DEL", "sccache/x"}),
                          "-NOPERM User reader has no permissions to run the 'DEL' command\r\n");
        BOOST_CHECK_EQUAL(reader.call({"INFO"}),
                          "-NOPERM User reader has no permissions to run the 'INFO' command\r\n");
        BOOST_CHECK_EQUAL(reader.call({"GET", "other/x"}),
                          "-NOPERM No permissions to access a key\r\n");

        resp_client farm(port);
        BOOST_CHECK_EQUAL(farm.auth("farm", "farm-secret"), "+OK\r\n");
        BOOST_CHECK_EQUAL(farm.call({"SET", "other/x", "v"}),
                          "-NOPERM No permissions to access a key\r\n");
        BOOST_CHECK_EQUAL(farm.call({"SET", "sccache/x", "v"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(farm.call({"INFO"}),
                          "-NOPERM User farm has no permissions to run the 'INFO' command\r\n");

        // DBSIZE counts this node's replicas, so ask the node that led the
        // SET rather than one that may not have applied it yet.
        resp_client ops(c.port(c.leader_of(k_sccache_group)));
        BOOST_CHECK_EQUAL(ops.auth("ops", "ops-secret"), "+OK\r\n");
        auto info = ops.call({"INFO"});
        BOOST_CHECK(info.find("kythira_gateway:1") != std::string::npos);
        BOOST_CHECK(info.find("kythira_authz_denials:") != std::string::npos);
        BOOST_CHECK_EQUAL(ops.call({"DBSIZE"}), ":1\r\n");
        BOOST_CHECK_EQUAL(ops.call({"COMMAND", "COUNT"}), ":17\r\n");
    }
}

BOOST_AUTO_TEST_CASE(any_node_answers_any_key_by_forwarding, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));

    // Talk only to a follower of shard 2 and write a shard-2 key: the write
    // must be forwarded to the leader's gateway and the read served the same
    // way (Requirements 9.1-9.3).
    auto follower = c.a_follower_of(2);
    BOOST_REQUIRE_NE(follower, 0u);
    resp_client client(c.port(follower));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/zzz", "far away"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/zzz"}), bulk("far away"));
    BOOST_CHECK_EQUAL(client.call({"EXISTS", "sccache/zzz"}), ":1\r\n");
    BOOST_CHECK_EQUAL(client.call({"TTL", "sccache/zzz"}), ":-1\r\n");
    BOOST_CHECK_GE(c.gateway(follower).stats()._forwards.load(), 4u);
    BOOST_CHECK_EQUAL(c.gateway(follower).stats()._forward_failures.load(), 0u);

    // The leader's own view agrees.
    resp_client direct(c.port(c.leader_of(2)));
    BOOST_REQUIRE_EQUAL(direct.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK_EQUAL(direct.call({"GET", "sccache/zzz"}), bulk("far away"));
    BOOST_CHECK_EQUAL(client.call({"DEL", "sccache/zzz"}), ":1\r\n");
    BOOST_CHECK_EQUAL(direct.call({"GET", "sccache/zzz"}), "$-1\r\n");

    // A RESP3 client forwarding through a RESP2 internal hop still gets `_`.
    resp_client v3(c.port(follower));
    auto hello = v3.call({"HELLO", "3", "AUTH", "farm", "farm-secret"});
    BOOST_REQUIRE(hello.rfind("%", 0) == 0);
    BOOST_CHECK_EQUAL(v3.call({"GET", "sccache/zzz"}), "_\r\n");
}

BOOST_AUTO_TEST_CASE(forwarding_off_answers_with_a_retry_error, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._forwarding = false;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    auto follower = c.a_follower_of(k_sccache_group);
    resp_client client(c.port(follower));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/aaa", "v"}),
                      "-ERR shard has no reachable leader, retry\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/aaa"}),
                      "-ERR shard has no reachable leader, retry\r\n");
}

BOOST_AUTO_TEST_CASE(any_replica_reads_serve_locally, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._read_consistency = redis_read_consistency::any_replica;
    cfg._forwarding = false;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client leader(c.port(c.leader_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(leader.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK_EQUAL(leader.call({"SET", "sccache/aaa", "replicated"}), "+OK\r\n");

    resp_client follower(c.port(c.a_follower_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(follower.auth("farm", "farm-secret"), "+OK\r\n");
    // Replication is asynchronous from the client's point of view; poll.
    std::string got;
    for (int i = 0; i < 100 && got != bulk("replicated"); ++i) {
        got = follower.call({"GET", "sccache/aaa"});
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    BOOST_CHECK_EQUAL(got, bulk("replicated"));
    BOOST_CHECK_EQUAL(c.gateway(c.a_follower_of(k_sccache_group)).stats()._forwards.load(), 0u);
}

BOOST_AUTO_TEST_CASE(linearizable_reads_go_through_the_log, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._read_consistency = redis_read_consistency::linearizable;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(c.leader_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/aaa", "v1"}), "+OK\r\n");
    auto before =
        c.host(c.leader_of(k_sccache_group)).group_node(k_sccache_group)->last_applied_index();
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/aaa"}), bulk("v1"));
    BOOST_CHECK_GT(
        c.host(c.leader_of(k_sccache_group)).group_node(k_sccache_group)->last_applied_index(),
        before);
}

BOOST_AUTO_TEST_CASE(setex_expires_and_the_sweep_removes_it, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(c.leader_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");

    BOOST_CHECK_EQUAL(client.call({"SETEX", "sccache/ttl", "0", "v"}),
                      "-ERR invalid expire time in 'setex' command\r\n");
    BOOST_CHECK_EQUAL(client.call({"SETEX", "sccache/ttl", "-5", "v"}),
                      "-ERR invalid expire time in 'setex' command\r\n");
    BOOST_CHECK_EQUAL(client.call({"SETEX", "sccache/ttl", "abc", "v"}),
                      "-ERR value is not an integer or out of range\r\n");
    BOOST_CHECK_EQUAL(client.call({"SETEX", "sccache/ttl", "1", "v"}), "+OK\r\n");
    auto ttl = client.call({"TTL", "sccache/ttl"});
    BOOST_CHECK(ttl == ":1\r\n" || ttl == ":0\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/ttl"}), bulk("v"));
    BOOST_CHECK_EQUAL(client.call({"SETEX", "sccache/keep", "1000", "k"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/forever", "f"}), "+OK\r\n");

    std::this_thread::sleep_for(std::chrono::milliseconds{1200});
    // Expired for readers immediately, before any sweep (Requirement 6.2).
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/ttl"}), "$-1\r\n");
    BOOST_CHECK_EQUAL(client.call({"EXISTS", "sccache/ttl"}), ":0\r\n");
    BOOST_CHECK_EQUAL(client.call({"TTL", "sccache/ttl"}), ":-2\r\n");
    // Rewriting an expired key is not a conflict.
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/ttl", "v2"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"DEL", "sccache/ttl"}), ":1\r\n");
    BOOST_CHECK_EQUAL(client.call({"SETEX", "sccache/ttl", "1", "v3"}), "+OK\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds{1200});

    // The sweep removes it from the store on every replica.
    auto proposals = c.maintain(k_sccache_group);
    BOOST_CHECK_GE(proposals, 1u);
    auto count_on = [&](node_id_t id) {
        return c.host(id).group_node(k_sccache_group)->with_state_machine([](auto& sm) {
            return sm.approximate_key_count();
        });
    };
    for (int i = 0; i < 100 && (count_on(1) != 2 || count_on(2) != 2 || count_on(3) != 2); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    BOOST_CHECK_EQUAL(count_on(1), 2u);
    BOOST_CHECK_EQUAL(count_on(2), 2u);
    BOOST_CHECK_EQUAL(count_on(3), 2u);
    BOOST_CHECK_GE(c.gateway(c.leader_of(k_sccache_group)).stats()._expirations.load(), 1u);
    // Nothing to sweep now: the maintenance pass is a no-op.
    BOOST_CHECK_EQUAL(c.maintain(k_sccache_group), 0u);
}

BOOST_AUTO_TEST_CASE(immutable_values_and_size_limit, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._max_value_bytes = 64;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(c.leader_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");

    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/k", "one"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/k", "one"}), "+OK\r\n");  // identical: no-op
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/k", "two"}),
                      "-ERR value conflict for an existing key\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/k"}), bulk("one"));
    BOOST_CHECK_EQUAL(c.gateway(c.leader_of(k_sccache_group)).stats()._value_conflicts.load(), 1u);

    std::string big(65, 'x');
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/big", big}),
                      "-ERR value exceeds the configured maximum of 64 bytes\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/big"}), "$-1\r\n");
    std::string fits(64, 'x');
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/big", fits}), "+OK\r\n");
}

BOOST_AUTO_TEST_CASE(eviction_when_a_shard_is_over_budget, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._max_shard_bytes = 400;
    cfg._sweep_batch = 2;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(c.leader_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");

    // Ten 50-byte values: well past 400 bytes. Writes are never refused.
    for (int i = 0; i < 10; ++i) {
        BOOST_CHECK_EQUAL(
            client.call({"SET", "sccache/e" + std::to_string(i), std::string(50, 'v')}), "+OK\r\n");
    }
    // Reads keep e9 hot so it survives; the LRU tail goes first.
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/e0"}), bulk(std::string(50, 'v')));

    auto bytes_on_leader = [&] {
        return c.host(c.leader_of(k_sccache_group))
            .group_node(k_sccache_group)
            ->with_state_machine([](auto& sm) { return sm.approximate_size_bytes(); });
    };
    std::size_t passes = 0;
    while (bytes_on_leader() > 400 && passes < 20) {
        BOOST_CHECK_GE(c.maintain(k_sccache_group), 1u);
        ++passes;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    BOOST_CHECK_LE(bytes_on_leader(), 400u);
    BOOST_CHECK_GE(c.gateway(c.leader_of(k_sccache_group)).stats()._evictions.load(), 1u);
    BOOST_CHECK_GE(c.gateway(c.leader_of(k_sccache_group)).stats()._over_budget_ticks.load(), 1u);
    // The most recently read key survived.
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/e0"}), bulk(std::string(50, 'v')));
    BOOST_CHECK_EQUAL(client.call({"EXISTS", "sccache/e1"}), ":0\r\n");
}

BOOST_AUTO_TEST_CASE(protocol_errors_close_and_limits_hold, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._max_clients = 2;
    cfg._parser_limits._max_bulk_len = 1024;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    auto port = c.port(c.leader_of(1));

    {
        resp_client bad(port);
        bad.send_raw("*1\r\n$2000\r\n");
        auto reply = bad.read_reply();
        BOOST_CHECK(reply.rfind("-ERR Protocol error:", 0) == 0);
        BOOST_CHECK(bad.wait_closed());
    }
    {
        resp_client bad(port);
        bad.send_raw("*1\r\n$abc\r\n");
        BOOST_CHECK(bad.read_reply().rfind("-ERR Protocol error:", 0) == 0);
        BOOST_CHECK(bad.wait_closed());
    }
    {
        resp_client first(port);
        resp_client second(port);
        BOOST_CHECK_EQUAL(first.auth("farm", "farm-secret"), "+OK\r\n");
        BOOST_CHECK_EQUAL(second.auth("farm", "farm-secret"), "+OK\r\n");
        resp_client third(port);
        BOOST_CHECK_EQUAL(third.read_reply(), "-ERR max number of clients reached\r\n");
        BOOST_CHECK(third.wait_closed());
        BOOST_CHECK_GE(c.gateway(c.leader_of(1)).stats()._connections_rejected.load(), 1u);
    }
    // Slots are released on close.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    resp_client again(port);
    BOOST_CHECK_EQUAL(again.auth("farm", "farm-secret"), "+OK\r\n");

    // Inline commands work too (redis-cli style).
    again.send_raw("PING\r\n");
    BOOST_CHECK_EQUAL(again.read_reply(), "+PONG\r\n");
}

BOOST_AUTO_TEST_CASE(pipelined_writes_keep_order, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(c.leader_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    std::string batch;
    constexpr int n = 50;
    for (int i = 0; i < n; ++i) {
        batch += resp_client::encode({"SET", "sccache/p" + std::to_string(i), std::to_string(i)});
        batch += resp_client::encode({"GET", "sccache/p" + std::to_string(i)});
    }
    client.send_raw(batch);
    for (int i = 0; i < n; ++i) {
        BOOST_CHECK_EQUAL(client.read_reply(), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.read_reply(), bulk(std::to_string(i)));
    }
}

BOOST_AUTO_TEST_CASE(empty_acl_refuses_to_start_unless_anonymous, *boost::unit_test::timeout(60)) {
    // No cluster needed: this is a start()-time check. A dummy host is
    // enough because start() never touches it.
    message_fabric fabric{2};
    config_type cfg{
        .node_id = 1,
        .network_client = fabric_client{fabric, 1},
        .network_server = fabric_server{fabric, 1},
        .store_factory = [](const group_id_type&) { return host_types::persistence_engine_type{}; },
    };
    cfg.partitioner = kythira::make_partitioner<key_type>(kythira::redis_kv_partitioner{});
    host_type host(std::move(cfg));
    redis_acl acl;
    kythira::console_logger logger{kythira::log_level::error};
    kythira::noop_metrics metrics;
    redis_gateway_config gcfg;
    gcfg._listen = "127.0.0.1:0";
    {
        gateway_type gw(host, acl, logger, metrics, gcfg, nullptr);
        BOOST_CHECK_THROW(gw.start(), std::runtime_error);
        BOOST_CHECK(!gw.is_running());
    }
    gcfg._allow_anonymous = true;
    gateway_type gw(host, acl, logger, metrics, gcfg, nullptr);
    BOOST_CHECK_NO_THROW(gw.start());
    BOOST_CHECK(gw.is_running());
    BOOST_CHECK_NE(gw.port(), 0);
    resp_client client(gw.port());
    // Anonymous connections are admin; the host is not running, so a key
    // command reports LOADING rather than NOAUTH.
    BOOST_CHECK_EQUAL(client.call({"PING"}), "+PONG\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "x"}),
                      "-LOADING Kythira is loading the dataset in memory\r\n");
    gw.stop();
    BOOST_CHECK(!gw.is_running());
}

BOOST_AUTO_TEST_SUITE_END()
