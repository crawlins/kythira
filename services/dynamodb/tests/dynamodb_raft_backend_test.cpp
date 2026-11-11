// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The Raft-hosted backend (`.kiro/specs/dynamodb-compatible-api/` task 7):
// three in-process nodes, each a multi_raft host with the catalog group and
// an item group on its own engine, and the DynamoDB handlers over a
// raft_backend on every node. Requests go to any node; the backend finds the
// leader. Covers the table lifecycle (the catalog leader's follow-on work),
// consistent and eventual reads, range reads across a split in both
// directions with small pages, losing an item group's leader, and the
// delete job emptying every replica.

#define BOOST_TEST_MODULE dynamodb_raft_backend_test
#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"
#include "multi_raft_test_fabric.hpp"

#include <kythira/dynamodb/backup.hpp>
#include <kythira/dynamodb/group_state_machine.hpp>
#include <kythira/dynamodb/pitr.hpp>
#include <kythira/dynamodb/raft_backend.hpp>

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/record_segment_store.hpp>
#include <raft/txn_envelope.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("dynamodb_raft_backend_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using namespace dynamodb_test;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

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
    using state_machine_type = ddb::dynamo_txn_group_state_machine<kythira::memory_kv_engine>;

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

using host_type = kythira::multi_raft<host_types, std::string, std::uint64_t>;
using config_type = kythira::multi_raft_config<host_types, std::string, std::uint64_t>;
using backend_type = ddb::raft_backend<host_type>;

constexpr std::uint64_t k_nodes = 3;
constexpr std::uint64_t k_catalog_group = 1;
constexpr std::uint64_t k_item_group = 2;

/// The handlers over one node's backend, as a gateway registers them.
struct stack {
    explicit stack(backend_type& backend)
        : guard("/data", 1024,
                [](const std::filesystem::path&) {
                    return std::optional<std::uintmax_t>(std::uintmax_t{1} << 40U);
                }),
          tables(backend),
          items(backend, &guard),
          queries(backend),
          batches(backend, &guard),
          transactions(backend, &guard),
          streams(backend) {
        auth.register_service(ddb::dynamodb_static_roles());
        ddb::front_end_config cfg;
        cfg.advertised_endpoint = "localhost:8000";
        fe = std::make_unique<ddb::front_end<az::static_key_authorizer>>(cfg, auth);
        tables.register_with(*fe);
        items.register_with(*fe);
        queries.register_with(*fe);
        batches.register_with(*fe);
        transactions.register_with(*fe);
        streams.register_with(*fe);
    }

    auto call(std::string_view op, const std::string& body) -> reply {
        const auto r = fe->handle(signed_request(op, body));
        return reply{r.status, json::parse(r.body).as_object()};
    }

    auto ok(std::string_view op, const std::string& body) -> json::object {
        auto r = call(op, body);
        BOOST_REQUIRE_MESSAGE(r.status == 200U,
                              std::string(op) + " failed: " + r.type() + ": " + r.message());
        return r.body;
    }

    ddb::disk_guard guard;
    az::static_key_authorizer auth{
        std::vector<az::static_key>{{"AKIDADMIN", std::string(k_secret), std::string(k_account),
                                     "admin", az::static_role::admin}},
        "aws", [] { return std::chrono::system_clock::time_point(k_now); }};
    std::unique_ptr<ddb::front_end<az::static_key_authorizer>> fe;
    ddb::table_api<backend_type> tables;
    ddb::item_api<backend_type> items;
    ddb::query_api<backend_type> queries;
    ddb::batch_api<backend_type> batches;
    ddb::txn_api<backend_type> transactions;
    ddb::streams_api<backend_type> streams;
};

auto test_options(std::size_t page_bytes = std::size_t{1} << 20U) -> ddb::raft_backend_options {
    ddb::raft_backend_options o;
    o.attempt_timeout = std::chrono::milliseconds{2000};
    o.deadline = std::chrono::milliseconds{15000};
    o.settle_interval = std::chrono::milliseconds{50};
    o.page_bytes = page_bytes;
    o.drop_step_keys = 7;
    return o;
}

/// Three nodes, both bootstrap groups on each.
class cluster {
public:
    explicit cluster(ddb::raft_backend_options options = test_options()) {
        for (std::uint64_t id = 1; id <= k_nodes; ++id) {
            _engines.push_back(std::make_shared<kythira::memory_kv_engine>());
            kythira::record_segment_store_options o;
            o._data_dir = _log_root.path / std::to_string(id);
            o._barriers_enabled = false;
            _logs.push_back(std::make_unique<kythira::record_segment_store>(o));
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
        }
        const auto layout =
            backend_type::bootstrap_descriptors({1, 2, 3}, k_catalog_group, k_item_group);
        for (auto& h : _hosts) {
            for (const auto& d : layout) {
                h->create_group(d);
            }
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
        for (std::uint64_t id = 1; id <= k_nodes; ++id) {
            _backends.push_back(std::make_unique<backend_type>(
                *_hosts[id - 1],
                [this, id](const std::uint64_t& to, const std::string& op)
                    -> std::optional<std::string> { return forward(id, to, op); },
                options));
            _backends.back()->set_change_log(_logs[id - 1].get());
        }
        for (auto& b : _backends) {
            _stacks.push_back(std::make_unique<stack>(*b));
            b->start();
        }
        BOOST_REQUIRE(await([&] { return leader(k_catalog_group) && leader(k_item_group); }));
    }

    ~cluster() {
        for (auto& b : _backends) {
            b->stop();
        }
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

    [[nodiscard]] auto node(std::uint64_t id) -> stack& { return *_stacks.at(id - 1); }
    [[nodiscard]] auto backend(std::uint64_t id) -> backend_type& { return *_backends.at(id - 1); }
    [[nodiscard]] auto host(std::uint64_t id) -> host_type& { return *_hosts.at(id - 1); }
    [[nodiscard]] auto engine(std::uint64_t id) -> kythira::memory_kv_engine& {
        return *_engines.at(id - 1);
    }
    [[nodiscard]] auto fabric() -> message_fabric& { return _fabric; }
    [[nodiscard]] auto change_log(std::uint64_t id) -> kythira::record_segment_store& {
        return *_logs.at(id - 1);
    }

    /// The live node leading `group`, or 0.
    [[nodiscard]] auto leader(std::uint64_t group) -> std::uint64_t {
        for (std::uint64_t id = 1; id <= k_nodes; ++id) {
            if (!_fabric.is_running(id)) {
                continue;
            }
            auto* n = _hosts[id - 1]->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return id;
            }
        }
        return 0;
    }

    /// A live node that does not lead `group`.
    [[nodiscard]] auto follower(std::uint64_t group) -> std::uint64_t {
        const auto l = leader(group);
        for (std::uint64_t id = 1; id <= k_nodes; ++id) {
            if (id != l && _fabric.is_running(id)) {
                return id;
            }
        }
        return 0;
    }

    static auto await(const std::function<bool()>& predicate,
                      std::chrono::milliseconds budget = std::chrono::milliseconds{15000}) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return predicate();
    }

    /// Data keys under `t/` in `id`'s engine.
    [[nodiscard]] auto item_keys(std::uint64_t id) -> std::size_t {
        return engine(id).scan(kythira::column_family::data, ddb::prefix_range("t/"),
                               kythira::scan_direction::forward,
                               [](std::string_view, std::string_view) { return true; });
    }

private:
    auto make_config(std::uint64_t id) -> config_type {
        config_type cfg{
            .node_id = id,
            .network_client = fabric_client{_fabric, id},
            .network_server = fabric_server{_fabric, id},
            .store_factory =
                [](const std::uint64_t&) { return host_types::persistence_engine_type{}; },
            .on_group_destroyed = {},
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{120};
        cfg.config._election_timeout_max = std::chrono::milliseconds{260};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{25};
        cfg.hibernation = kythira::hibernation_mode::off;
        cfg.state_engine = _engines.back();
        // Each node's groups log to its own store and fetch segments they
        // lack from the group's peers through that node's backend.
        cfg.state_machine_factory = [this, id](const std::uint64_t& group) {
            ddb::dynamo_group_state_machine<> inner;
            inner.set_change_log(_logs[id - 1].get(),
                                 [this, id, group](const kythira::record_segment_descriptor& d) {
                                     _backends.at(id - 1)->fetch_change_segment(group, d);
                                 });
            return host_types::state_machine_type(std::move(inner));
        };
        cfg.executor_stripes = 2;
        cfg.allocate_group_ids = [this](std::size_t n) {
            const std::lock_guard lock(_ids_mu);
            std::vector<std::uint64_t> out;
            for (std::size_t i = 0; i < n; ++i) {
                out.push_back(_next_id++);
            }
            return out;
        };
        cfg.split_merge_interval = std::chrono::milliseconds{0};
        // Expired transaction locks resolve within a few passes.
        cfg.resolver_interval = std::chrono::milliseconds{100};
        return cfg;
    }

    // The deployment's hop between gateways; a killed node is unreachable.
    auto forward(std::uint64_t from, std::uint64_t to, const std::string& op)
        -> std::optional<std::string> {
        if (to == 0 || to > k_nodes || !_fabric.is_running(from) || !_fabric.is_running(to)) {
            return std::nullopt;
        }
        return _backends[to - 1]->serve(op);
    }

    message_fabric _fabric{8};
    std::mutex _ids_mu;
    std::uint64_t _next_id = 100;
    std::vector<std::shared_ptr<kythira::memory_kv_engine>> _engines;
    /// Removed after the stores below are closed.
    struct temp_root {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("kythira-ddb-raft-log-" + std::to_string(std::random_device{}()));
        temp_root() = default;
        temp_root(const temp_root&) = delete;
        auto operator=(const temp_root&) -> temp_root& = delete;
        temp_root(temp_root&&) = delete;
        auto operator=(temp_root&&) -> temp_root& = delete;
        ~temp_root() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } _log_root;
    // Declared before the hosts, whose groups append to them.
    std::vector<std::unique_ptr<kythira::record_segment_store>> _logs;
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::unique_ptr<backend_type>> _backends;
    std::vector<std::unique_ptr<stack>> _stacks;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
};

const std::string k_create =
    R"({"TableName":"Music","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
    R"({"AttributeName":"sk","AttributeType":"N"}],)"
    R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
    R"({"AttributeName":"sk","KeyType":"RANGE"}],"BillingMode":"PAY_PER_REQUEST"})";

auto status_of(stack& s, const std::string& table) -> std::string {
    const auto r = s.call("DescribeTable", R"({"TableName":")" + table + R"("})");
    if (r.status != 200U) {
        return r.type();
    }
    return std::string(r.body.at("Table").as_object().at("TableStatus").as_string());
}

auto put(stack& s, const std::string& pk, int sk, const std::string& extra = "") -> void {
    s.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":")" + pk + R"("},"sk":{"N":")" +
                        std::to_string(sk) + R"("})" + extra + "}}");
}

auto get_body(bool consistent, const std::string& pk, int sk) -> std::string {
    return R"({"TableName":"Music","ConsistentRead":)" +
           std::string(consistent ? "true" : "false") + R"(,"Key":{"pk":{"S":")" + pk +
           R"("},"sk":{"N":")" + std::to_string(sk) + R"("}}})";
}

auto count_all(stack& s, bool consistent, std::set<std::string>* seen = nullptr) -> std::size_t {
    std::size_t total = 0;
    std::string start;
    for (;;) {
        std::string body = R"({"TableName":"Music","Limit":7,"ConsistentRead":)" +
                           std::string(consistent ? "true" : "false");
        if (!start.empty()) {
            body += R"(,"ExclusiveStartKey":)" + start;
        }
        body += "}";
        const auto r = s.ok("Scan", body);
        for (const auto& it : r.at("Items").as_array()) {
            const auto& o = it.as_object();
            if (seen != nullptr) {
                seen->insert(std::string(o.at("pk").as_object().at("S").as_string()) + "/" +
                             std::string(o.at("sk").as_object().at("N").as_string()));
            }
            ++total;
        }
        const auto* last = r.if_contains("LastEvaluatedKey");
        if (last == nullptr) {
            return total;
        }
        start = json::serialize(*last);
    }
}

}  // namespace

BOOST_AUTO_TEST_CASE(table_lifecycle_and_reads_on_any_node) {
    cluster c;
    // Created through a node that does not lead the catalog: forwarded.
    const auto via = c.follower(k_catalog_group);
    const auto created = c.node(via).ok("CreateTable", k_create);
    BOOST_TEST(created.at("TableDescription").as_object().at("TableStatus").as_string() ==
               "CREATING");
    // The catalog leader's worker makes it ACTIVE; every node sees it.
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        BOOST_TEST_REQUIRE(
            cluster::await([&] { return status_of(c.node(id), "Music") == "ACTIVE"; }));
    }
    const auto listed = c.node(3).ok("ListTables", "{}");
    BOOST_TEST(listed.at("TableNames").as_array().size() == 1U);

    // A write through each node, and consistent reads through every node.
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        put(c.node(id), "artist", static_cast<int>(id),
            R"(,"by":{"N":")" + std::to_string(id) + R"("})");
    }
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        for (int sk = 1; sk <= 3; ++sk) {
            const auto r = c.node(id).ok("GetItem", get_body(true, "artist", sk));
            BOOST_TEST_REQUIRE(r.contains("Item"));
            BOOST_TEST(r.at("Item").as_object().at("by").as_object().at("N").as_string() ==
                       std::to_string(sk));
        }
    }
    // An eventual read on a follower catches up.
    const auto f = c.follower(k_item_group);
    BOOST_TEST(cluster::await(
        [&] { return c.node(f).ok("GetItem", get_body(false, "artist", 3)).contains("Item"); }));
    // A conditional write's failure comes back through the hop.
    const auto failed =
        c.node(c.follower(k_item_group))
            .call("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"artist"},"sk":{"N":"1"}},)"
                             R"x("ConditionExpression":"attribute_not_exists(pk)"})x");
    BOOST_TEST(failed.status == 400U);
    BOOST_TEST(failed.type() == "ConditionalCheckFailedException");

    // Query in both directions through a follower.
    const std::string q =
        R"({"TableName":"Music","KeyConditionExpression":"pk = :p",)"
        R"("ExpressionAttributeValues":{":p":{"S":"artist"}},"ConsistentRead":true)";
    const auto up = c.node(f).ok("Query", q + "}");
    const auto down = c.node(f).ok("Query", q + R"(,"ScanIndexForward":false})");
    BOOST_TEST_REQUIRE(up.at("Items").as_array().size() == 3U);
    BOOST_TEST_REQUIRE(down.at("Items").as_array().size() == 3U);
    BOOST_TEST(up.at("Items").as_array()[0].as_object().at("sk").as_object().at("N").as_string() ==
               "1");
    BOOST_TEST(
        down.at("Items").as_array()[0].as_object().at("sk").as_object().at("N").as_string() == "3");

    // DeleteTable: the job empties every replica, then the name is free.
    for (int i = 0; i < 40; ++i) {
        put(c.node(1), "bulk" + std::to_string(i), i);
    }
    c.node(2).ok("DeleteTable", R"({"TableName":"Music"})");
    BOOST_TEST(cluster::await(
        [&] { return c.node(1).ok("ListTables", "{}").at("TableNames").as_array().empty(); }));
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        BOOST_TEST(cluster::await([&] { return c.item_keys(id) == 0U; }), "node " << id);
    }
    BOOST_TEST(status_of(c.node(3), "Music") == "ResourceNotFoundException");
    c.node(3).ok("CreateTable", k_create);
}

BOOST_AUTO_TEST_CASE(range_reads_cross_a_split_with_small_pages) {
    // Pages of about one item, so every read takes many hops and pages.
    cluster c(test_options(64));
    c.node(1).ok("CreateTable", k_create);
    BOOST_TEST_REQUIRE(cluster::await([&] { return status_of(c.node(1), "Music") == "ACTIVE"; }));
    constexpr int k_partitions = 12;
    for (int p = 0; p < k_partitions; ++p) {
        for (int sk = 0; sk < 3; ++sk) {
            put(c.node(1 + static_cast<std::uint64_t>(p) % k_nodes), "p" + std::to_string(p), sk);
        }
    }
    BOOST_TEST(count_all(c.node(2), true) == 36U);

    // Split the item group at two partition boundaries.
    const auto d = c.backend(1).describe("Music");
    BOOST_TEST_REQUIRE(d.has_value());
    std::vector<std::string> cuts;
    for (const char* pk : {"p3", "p8"}) {
        cuts.push_back(ddb::partition_prefix(d->table_id, ddb::attribute_value::string(pk)));
    }
    const auto l = c.leader(k_item_group);
    BOOST_TEST_REQUIRE(l != 0U);
    auto f = c.host(l).split_shard(k_item_group, cuts, std::chrono::milliseconds{5000});
    BOOST_TEST_REQUIRE(f.wait(std::chrono::milliseconds{8000}));
    std::ignore = std::move(f).get();
    BOOST_TEST_REQUIRE(cluster::await([&] {
        for (std::uint64_t id = 1; id <= k_nodes; ++id) {
            kythira::shard_range<std::string> all;
            all._start = std::string("t/");
            if (c.host(id).shard_map_snapshot().range_scan(all).size() != 3U) {
                return false;
            }
        }
        return true;
    }));

    // Every item once, consistent and eventual, from every node; each
    // partition in both directions.
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        std::set<std::string> seen;
        BOOST_TEST(count_all(c.node(id), true, &seen) == 36U);
        BOOST_TEST(seen.size() == 36U);
        BOOST_TEST(cluster::await([&] { return count_all(c.node(id), false) == 36U; }));
    }
    for (int p = 0; p < k_partitions; ++p) {
        const auto q = R"({"TableName":"Music","KeyConditionExpression":"pk = :p",)"
                       R"("ExpressionAttributeValues":{":p":{"S":"p)" +
                       std::to_string(p) + R"("}},"ConsistentRead":true,"ScanIndexForward":false})";
        const auto r = c.node(3).ok("Query", q);
        BOOST_TEST_REQUIRE(r.at("Items").as_array().size() == 3U);
        BOOST_TEST(
            r.at("Items").as_array()[0].as_object().at("sk").as_object().at("N").as_string() ==
            "2");
    }
    // Writes after the split land in the new groups and a delete still
    // reaches all of them.
    put(c.node(2), "p9", 7);
    BOOST_TEST(count_all(c.node(3), true) == 37U);
    c.node(1).ok("DeleteTable", R"({"TableName":"Music"})");
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        BOOST_TEST(cluster::await([&] { return c.item_keys(id) == 0U; }), "node " << id);
    }
}

BOOST_AUTO_TEST_CASE(item_leader_loss) {
    cluster c;
    c.node(1).ok("CreateTable", k_create);
    BOOST_TEST_REQUIRE(cluster::await([&] { return status_of(c.node(1), "Music") == "ACTIVE"; }));
    put(c.node(1), "before", 1);
    BOOST_TEST_REQUIRE(cluster::await([&] {
        for (std::uint64_t id = 1; id <= k_nodes; ++id) {
            if (c.item_keys(id) == 0U) {
                return false;
            }
        }
        return true;
    }));

    // Cut off the item group's leader; the others elect a new one and
    // requests through them keep working.
    const auto old = c.leader(k_item_group);
    BOOST_TEST_REQUIRE(old != 0U);
    c.fabric().kill(old);
    const auto live = old == 1 ? 2U : 1U;
    put(c.node(live), "after", 2);
    const auto r = c.node(live).ok("GetItem", get_body(true, "before", 1));
    BOOST_TEST(r.contains("Item"));
    const auto r2 = c.node(old == 3 ? 2U : 3U).ok("GetItem", get_body(true, "after", 2));
    BOOST_TEST(r2.contains("Item"));
    c.fabric().revive(old);
}

namespace {

/// Splits the item group so each partition of `pks` lies in a group of its
/// own (partitions sort by their hash, so the cuts are the partitions
/// themselves, all but the lowest), and waits until every node routes by
/// the new layout.
auto split_apart(cluster& c, const std::vector<std::string>& pks) -> void {
    const auto d = c.backend(1).describe("Music");
    BOOST_TEST_REQUIRE(d.has_value());
    std::vector<std::string> cuts;
    for (const auto& pk : pks) {
        cuts.push_back(ddb::partition_prefix(d->table_id, ddb::attribute_value::string(pk)));
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(cuts.begin());
    const auto l = c.leader(k_item_group);
    BOOST_TEST_REQUIRE(l != 0U);
    auto f = c.host(l).split_shard(k_item_group, cuts, std::chrono::milliseconds{5000});
    BOOST_TEST_REQUIRE(f.wait(std::chrono::milliseconds{8000}));
    std::ignore = std::move(f).get();
    BOOST_TEST_REQUIRE(cluster::await([&] {
        for (std::uint64_t id = 1; id <= k_nodes; ++id) {
            kythira::shard_range<std::string> all;
            all._start = std::string("t/");
            if (c.host(id).shard_map_snapshot().range_scan(all).size() != pks.size()) {
                return false;
            }
        }
        return true;
    }));
}

auto value_of(stack& s, const std::string& pk, int sk) -> std::string {
    const auto r = s.ok("GetItem", get_body(true, pk, sk));
    if (!r.contains("Item")) {
        return "<none>";
    }
    const auto& it = r.at("Item").as_object();
    return it.contains("v") ? std::string(it.at("v").as_object().at("N").as_string()) : "<no v>";
}

auto put_action(const std::string& pk, int sk, int v) -> std::string {
    return R"({"Put":{"TableName":"Music","Item":{"pk":{"S":")" + pk + R"("},"sk":{"N":")" +
           std::to_string(sk) + R"("},"v":{"N":")" + std::to_string(v) + R"("}}}})";
}

auto key_json(const std::string& pk, int sk) -> std::string {
    return R"({"pk":{"S":")" + pk + R"("},"sk":{"N":")" + std::to_string(sk) + R"("}})";
}

}  // namespace

BOOST_AUTO_TEST_CASE(transactions_span_groups_all_or_nothing) {
    cluster c;
    c.node(1).ok("CreateTable", k_create);
    BOOST_TEST_REQUIRE(cluster::await([&] { return status_of(c.node(1), "Music") == "ACTIVE"; }));
    for (const char* pk : {"p1", "p5", "p9"}) {
        put(c.node(1), pk, 1, R"(,"v":{"N":"1"})");
    }
    // p1, p5 and p9 in three different groups.
    split_apart(c, {"p1", "p5", "p9"});

    // Through a node that leads none of the item groups it touches, if any.
    const auto via = c.follower(k_item_group);
    c.node(via).ok(
        "TransactWriteItems",
        R"({"TransactItems":[)" + put_action("p1", 1, 10) + "," + put_action("p5", 1, 50) + "," +
            R"({"Update":{"TableName":"Music","Key":)" + key_json("p9", 1) +
            R"(,"UpdateExpression":"SET v = v + :n","ExpressionAttributeValues":{":n":{"N":"89"}}}}]})");
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        BOOST_TEST(value_of(c.node(id), "p1", 1) == "10");
        BOOST_TEST(value_of(c.node(id), "p5", 1) == "50");
        BOOST_TEST(value_of(c.node(id), "p9", 1) == "90");
    }

    // One failing condition in the third group cancels the writes in the
    // other two.
    const auto refused = c.node(3).call(
        "TransactWriteItems",
        R"({"TransactItems":[)" + put_action("p1", 1, 11) + "," + put_action("p5", 1, 51) + "," +
            R"({"ConditionCheck":{"TableName":"Music","Key":)" + key_json("p9", 1) +
            R"(,"ConditionExpression":"v = :x","ExpressionAttributeValues":{":x":{"N":"0"}},)"
            R"("ReturnValuesOnConditionCheckFailure":"ALL_OLD"}}]})");
    BOOST_TEST(refused.status == 400U);
    BOOST_TEST(refused.type() == "TransactionCanceledException");
    const auto& reasons = refused.body.at("CancellationReasons").as_array();
    BOOST_TEST_REQUIRE(reasons.size() == 3U);
    BOOST_TEST(reasons[0].as_object().at("Code").as_string() == "None");
    BOOST_TEST(reasons[1].as_object().at("Code").as_string() == "None");
    BOOST_TEST(reasons[2].as_object().at("Code").as_string() == "ConditionalCheckFailed");
    BOOST_TEST(
        reasons[2].as_object().at("Item").as_object().at("v").as_object().at("N").as_string() ==
        "90");
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        BOOST_TEST(value_of(c.node(id), "p1", 1) == "10");
        BOOST_TEST(value_of(c.node(id), "p5", 1) == "50");
    }

    // A read of all three, and of one that is not there.
    const auto got = c.node(2).ok(
        "TransactGetItems", R"({"TransactItems":[{"Get":{"TableName":"Music","Key":)" +
                                key_json("p1", 1) + R"(}},{"Get":{"TableName":"Music","Key":)" +
                                key_json("p5", 1) + R"(}},{"Get":{"TableName":"Music","Key":)" +
                                key_json("p9", 1) + R"(}},{"Get":{"TableName":"Music","Key":)" +
                                key_json("p4", 1) + R"(}}]})");
    const auto& responses = got.at("Responses").as_array();
    BOOST_TEST_REQUIRE(responses.size() == 4U);
    BOOST_TEST(responses[2].as_object().at("Item").as_object().at("v").as_object().at("N") == "90");
    BOOST_TEST(responses[3].as_object().empty());

    // No lock is left behind on any replica once the secondaries land.
    BOOST_TEST(cluster::await([&] {
        for (std::uint64_t id = 1; id <= k_nodes; ++id) {
            kythira::shard_range<std::string> all;
            all._start = std::string("t/");
            for (const auto& d : c.host(id).shard_map_snapshot().range_scan(all)) {
                const auto locks = c.host(id).with_group_state_machine(
                    d._group_id, [](auto& sm, const auto&) { return sm.intent_count(); });
                if (locks && *locks != 0U) {
                    return false;
                }
            }
        }
        return true;
    }));
}

BOOST_AUTO_TEST_CASE(a_held_lock_refuses_plain_writes_until_it_resolves) {
    cluster c;
    c.node(1).ok("CreateTable", k_create);
    BOOST_TEST_REQUIRE(cluster::await([&] { return status_of(c.node(1), "Music") == "ACTIVE"; }));
    put(c.node(1), "held", 1, R"(,"v":{"N":"1"})");

    // A coordinator that prepared and died: an intent with a short TTL and
    // nobody to commit it.
    const auto d = c.backend(1).describe("Music");
    BOOST_TEST_REQUIRE(d.has_value());
    ddb::item_command cmd;
    cmd.op = ddb::item_command::kind::put;
    cmd.table_id = d->table_id;
    cmd.schema = d->schema;
    cmd.payload =
        ddb::item{{"pk", ddb::attribute_value::string("held")},
                  {"sk", ddb::attribute_value::number(*ddb::dynamo_decimal::from_string("1"))}};
    const auto key = *ddb::base_item_key(d->table_id, d->schema, cmd.payload);
    const auto bytes = ddb::encode_item_command(cmd);
    const std::vector<std::byte> command(
        reinterpret_cast<const std::byte*>(bytes.data()),
        reinterpret_cast<const std::byte*>(bytes.data()) + bytes.size());
    const auto l = c.leader(k_item_group);
    BOOST_TEST_REQUIRE(l != 0U);
    const auto deadline = c.host(l).txn_now_ms() + 1500;
    const auto envelope = kythira::encode_txn_envelope<std::string>(
        kythira::txn_prepare<std::string>{kythira::txn_id{7, 7},
                                          key,
                                          {kythira::txn_write<std::string>{key, command}},
                                          {},
                                          0,
                                          deadline,
                                          true});
    auto f = c.host(l).submit_txn({key}, envelope, std::chrono::milliseconds{3000});
    BOOST_TEST_REQUIRE(f.wait(std::chrono::milliseconds{4000}));
    const auto prepared = kythira::decode_txn_result<std::string>(std::move(f).get());
    BOOST_TEST((prepared._status == kythira::txn_result_status::ok));

    // While it stands: a plain write conflicts, and a transaction is
    // cancelled with the item's reason.
    const auto plain =
        c.node(c.follower(k_item_group))
            .call("PutItem", R"({"TableName":"Music","Item":)" + key_json("held", 1) + "}");
    BOOST_TEST(plain.status == 400U);
    BOOST_TEST(plain.type() == "TransactionConflictException");
    const auto txn =
        c.node(2).call("TransactWriteItems", R"({"TransactItems":[)" + put_action("free", 1, 1) +
                                                 "," + put_action("held", 1, 2) + "]}");
    BOOST_TEST(txn.type() == "TransactionCanceledException");
    const auto& reasons = txn.body.at("CancellationReasons").as_array();
    BOOST_TEST_REQUIRE(reasons.size() == 2U);
    BOOST_TEST(reasons[1].as_object().at("Code").as_string() == "TransactionConflict");
    BOOST_TEST(value_of(c.node(1), "free", 1) == "<none>");

    // Past its TTL the resolver aborts it, and writes go through.
    BOOST_TEST(cluster::await(
        [&] {
            return c.node(3)
                       .call("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"held"},)"
                                        R"("sk":{"N":"1"},"v":{"N":"3"}}})")
                       .status == 200U;
        },
        std::chrono::milliseconds{10000}));
    BOOST_TEST(value_of(c.node(1), "held", 1) == "3");
}

BOOST_AUTO_TEST_CASE(transactions_survive_an_item_leader_loss) {
    cluster c;
    c.node(1).ok("CreateTable", k_create);
    BOOST_TEST_REQUIRE(cluster::await([&] { return status_of(c.node(1), "Music") == "ACTIVE"; }));
    put(c.node(1), "a", 1, R"(,"v":{"N":"1"})");
    split_apart(c, {"a", "z"});

    const auto old = c.leader(k_item_group);
    BOOST_TEST_REQUIRE(old != 0U);
    c.fabric().kill(old);
    const auto live = old == 1 ? 2U : 1U;
    c.node(live).ok("TransactWriteItems", R"({"TransactItems":[)" + put_action("a", 1, 2) + "," +
                                              put_action("z", 1, 3) + "]}");
    BOOST_TEST(value_of(c.node(live), "a", 1) == "2");
    BOOST_TEST(value_of(c.node(live), "z", 1) == "3");
    const auto got =
        c.node(old == 3 ? 2U : 3U)
            .ok("TransactGetItems",
                R"({"TransactItems":[{"Get":{"TableName":"Music","Key":)" + key_json("a", 1) +
                    R"(}},{"Get":{"TableName":"Music","Key":)" + key_json("z", 1) + R"(}}]})");
    BOOST_TEST(got.at("Responses").as_array().size() == 2U);
    c.fabric().revive(old);
}

// Leaders killed mid-transaction (task 10's chaos case, in process): one node
// runs transactions over two groups back to back while another kills and
// revives the leader of one of them. Any single transaction may fail or time
// out, but none may land on one item only, so once the cluster settles a
// transactional read sees both items equal, at the value of a transaction
// that ran.
BOOST_AUTO_TEST_CASE(leaders_killed_mid_transaction_never_split_one) {
    cluster c;
    c.node(1).ok("CreateTable", k_create);
    BOOST_TEST_REQUIRE(cluster::await([&] { return status_of(c.node(1), "Music") == "ACTIVE"; }));
    put(c.node(1), "a", 1, R"(,"v":{"N":"0"})");
    put(c.node(1), "z", 1, R"(,"v":{"N":"0"})");
    split_apart(c, {"a", "z"});

    // The driving node is never killed; the others take turns leading.
    const std::uint64_t driver = 1;
    std::atomic<bool> stop{false};
    std::atomic<int> committed{0};
    std::atomic<int> last_ok{0};
    std::thread load([&] {
        for (int i = 1; !stop.load(); ++i) {
            const auto r = c.node(driver).call(
                "TransactWriteItems", R"({"TransactItems":[)" + put_action("a", 1, i) + "," +
                                          put_action("z", 1, i) + "]}");
            if (r.status == 200U) {
                committed.fetch_add(1);
                last_ok.store(i);
            }
        }
    });

    int leaders_killed = 0;
    for (int round = 0; round < 3; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        std::uint64_t victim = 0;
        BOOST_TEST_REQUIRE(cluster::await([&] {
            victim = c.leader(k_item_group);
            return victim != 0U;
        }));
        if (victim == driver) {
            // The driver's own coordinator must survive to be checked, so
            // kill a follower instead; the other group may be led there.
            victim = 2;
        } else {
            ++leaders_killed;
        }
        c.fabric().kill(victim);
        std::this_thread::sleep_for(std::chrono::milliseconds{700});
        c.fabric().revive(victim);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
    stop.store(true);
    load.join();
    BOOST_TEST_MESSAGE("committed " << committed.load() << " of " << last_ok.load() << ", "
                                    << leaders_killed << " leader kills");
    BOOST_TEST(committed.load() > 0);

    // Locks a dead coordinator or primary left behind are resolved by the
    // resolvers, after which a transactional read gets both items.
    std::string a;
    std::string z;
    BOOST_TEST_REQUIRE(cluster::await(
        [&] {
            const auto r = c.node(driver).call(
                "TransactGetItems",
                R"({"TransactItems":[{"Get":{"TableName":"Music","Key":)" + key_json("a", 1) +
                    R"(}},{"Get":{"TableName":"Music","Key":)" + key_json("z", 1) + R"(}}]})");
            if (r.status != 200U) {
                return false;
            }
            const auto& got = r.body.at("Responses").as_array();
            a = got.at(0)
                    .as_object()
                    .at("Item")
                    .as_object()
                    .at("v")
                    .as_object()
                    .at("N")
                    .as_string();
            z = got.at(1)
                    .as_object()
                    .at("Item")
                    .as_object()
                    .at("v")
                    .as_object()
                    .at("N")
                    .as_string();
            return true;
        },
        std::chrono::milliseconds{30000}));
    BOOST_TEST(a == z);
    BOOST_TEST(std::stoi(a) >= last_ok.load());
}

namespace {

const std::string k_create_with_gsi =
    R"({"TableName":"Music","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
    R"({"AttributeName":"sk","AttributeType":"N"},{"AttributeName":"g","AttributeType":"S"}],)"
    R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
    R"({"AttributeName":"sk","KeyType":"RANGE"}],"BillingMode":"PAY_PER_REQUEST",)"
    R"("GlobalSecondaryIndexes":[{"IndexName":"by_g","KeySchema":)"
    R"([{"AttributeName":"g","KeyType":"HASH"}],"Projection":{"ProjectionType":"ALL"}}]})";

/// `pk/sk=v` of every entry of `by_g` with partition `g`, through `s`.
auto index_items(stack& s, const std::string& g) -> std::set<std::string> {
    std::set<std::string> out;
    std::string start;
    for (;;) {
        std::string body = R"({"TableName":"Music","IndexName":"by_g","Limit":5,)"
                           R"("KeyConditionExpression":"g = :g",)"
                           R"("ExpressionAttributeValues":{":g":{"S":")" +
                           g + R"("}})";
        if (!start.empty()) {
            body += R"(,"ExclusiveStartKey":)" + start;
        }
        body += "}";
        const auto r = s.ok("Query", body);
        for (const auto& it : r.at("Items").as_array()) {
            const auto& o = it.as_object();
            out.insert(std::string(o.at("pk").as_object().at("S").as_string()) + "/" +
                       std::string(o.at("sk").as_object().at("N").as_string()) + "=" +
                       std::string(o.at("v").as_object().at("N").as_string()));
        }
        const auto* last = r.if_contains("LastEvaluatedKey");
        if (last == nullptr) {
            return out;
        }
        start = json::serialize(*last);
    }
}

auto put_g(stack& s, const std::string& pk, int sk, const std::string& g, int v) -> void {
    put(s, pk, sk, R"(,"g":{"S":")" + g + R"("},"v":{"N":")" + std::to_string(v) + R"("})");
}

auto expected(const std::vector<std::string>& pks, int per, int v) -> std::set<std::string> {
    std::set<std::string> out;
    for (const auto& pk : pks) {
        for (int sk = 0; sk < per; ++sk) {
            out.insert(pk + "/" + std::to_string(sk) + "=" + std::to_string(v));
        }
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(gsi_entries_reach_other_groups_through_a_leader_loss) {
    cluster c;
    c.node(1).ok("CreateTable", k_create_with_gsi);
    BOOST_TEST_REQUIRE(cluster::await([&] { return status_of(c.node(1), "Music") == "ACTIVE"; }));
    const std::vector<std::string> pks{"p0", "p1", "p2", "p3"};
    constexpr int k_per = 3;
    for (const auto& pk : pks) {
        for (int sk = 0; sk < k_per; ++sk) {
            put_g(c.node(2), pk, sk, "red", 1);
        }
    }
    // Each partition in a group of its own; the index's key space follows
    // the table's, so most entries live in another group than their items.
    split_apart(c, pks);
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        BOOST_TEST(cluster::await(
                       [&] { return index_items(c.node(id), "red") == expected(pks, k_per, 1); }),
                   "node " << id);
    }

    // Move every item to another index partition while the leader of the
    // lowest group is cut off: its changes are carried by the new leader.
    const auto old = c.leader(k_item_group);
    BOOST_TEST_REQUIRE(old != 0U);
    c.fabric().kill(old);
    const auto live = old == 1 ? 2U : 1U;
    for (const auto& pk : pks) {
        for (int sk = 0; sk < k_per; ++sk) {
            put_g(c.node(live), pk, sk, "blue", 2);
        }
    }
    BOOST_TEST(cluster::await([&] {
        return index_items(c.node(live), "blue") == expected(pks, k_per, 2) &&
               index_items(c.node(live), "red").empty();
    }));
    c.fabric().revive(old);

    // Deleted items leave the index; the revived node catches up and every
    // leader reports no lag.
    for (int sk = 0; sk < k_per; ++sk) {
        c.node(old).ok("DeleteItem", R"({"TableName":"Music","Key":)" + key_json("p1", sk) + "}");
    }
    auto rest = expected({"p0", "p2", "p3"}, k_per, 2);
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        BOOST_TEST(cluster::await([&] { return index_items(c.node(id), "blue") == rest; }),
                   "node " << id);
    }
    BOOST_TEST(cluster::await([&] {
        std::size_t lag = 0;
        for (std::uint64_t id = 1; id <= k_nodes; ++id) {
            lag += c.backend(id).gsi_lag();
        }
        return lag == 0U;
    }));

    // DeleteTable removes the table's and the index's keys everywhere.
    c.node(3).ok("DeleteTable", R"({"TableName":"Music"})");
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        BOOST_TEST(cluster::await([&] { return c.item_keys(id) == 0U; }), "node " << id);
    }
}

BOOST_AUTO_TEST_CASE(an_index_added_later_is_backfilled_across_groups_while_writes_race_it) {
    auto options = test_options();
    // Long enough to see the index CREATING, short enough for a test.
    options.gsi_backfill_delay = std::chrono::milliseconds{1500};
    options.gsi_backfill_batch = 5;
    cluster c(options);
    c.node(1).ok("CreateTable", k_create);
    BOOST_TEST_REQUIRE(cluster::await([&] { return status_of(c.node(1), "Music") == "ACTIVE"; }));
    const std::vector<std::string> pks{"p0", "p1", "p2", "p3"};
    constexpr int k_per = 4;
    for (const auto& pk : pks) {
        for (int sk = 0; sk < k_per; ++sk) {
            put_g(c.node(1), pk, sk, "red", 1);
        }
    }
    split_apart(c, pks);

    const auto created = c.node(2).ok(
        "UpdateTable",
        R"({"TableName":"Music","AttributeDefinitions":[{"AttributeName":"g","AttributeType":"S"}],)"
        R"("GlobalSecondaryIndexUpdates":[{"Create":{"IndexName":"by_g","KeySchema":)"
        R"([{"AttributeName":"g","KeyType":"HASH"}],"Projection":{"ProjectionType":"ALL"}}}]})");
    const auto& gsis = created.at("TableDescription").as_object().at("GlobalSecondaryIndexes");
    BOOST_TEST(gsis.as_array()[0].as_object().at("IndexStatus").as_string() == "CREATING");
    BOOST_TEST(gsis.as_array()[0].as_object().at("Backfilling").as_bool());

    // While it is CREATING: no other index change, and no reads of it.
    const auto busy = c.node(3).call(
        "UpdateTable", R"({"TableName":"Music","GlobalSecondaryIndexUpdates":[{"Delete":)"
                       R"({"IndexName":"by_g"}}]})");
    BOOST_TEST(busy.type() == "ResourceInUseException");
    const auto early = c.node(3).call(
        "Query", R"({"TableName":"Music","IndexName":"by_g","KeyConditionExpression":"g = :g",)"
                 R"("ExpressionAttributeValues":{":g":{"S":"red"}}})");
    BOOST_TEST(early.message() == "Cannot read from backfilling global secondary index: by_g");

    // Writes race the backfill: half the items move to blue at version 2.
    for (const auto& pk : pks) {
        for (int sk = 0; sk < k_per; sk += 2) {
            put_g(c.node(3), pk, sk, "blue", 2);
        }
    }
    BOOST_TEST_REQUIRE(cluster::await(
        [&] {
            const auto d = c.backend(1).describe("Music");
            return d && !d->gsis.empty() && d->gsis[0].status == ddb::index_status::active;
        },
        std::chrono::milliseconds{30000}));
    std::set<std::string> red;
    std::set<std::string> blue;
    for (const auto& pk : pks) {
        for (int sk = 0; sk < k_per; ++sk) {
            (sk % 2 == 0 ? blue : red)
                .insert(pk + "/" + std::to_string(sk) + "=" + std::to_string(sk % 2 == 0 ? 2 : 1));
        }
    }
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        BOOST_TEST(cluster::await([&] {
                       return index_items(c.node(id), "red") == red &&
                              index_items(c.node(id), "blue") == blue;
                   }),
                   "node " << id);
    }

    // Deleting it: DELETING, then gone with its key space emptied, while
    // the table keeps every item.
    const auto deleted = c.node(1).ok(
        "UpdateTable", R"({"TableName":"Music","GlobalSecondaryIndexUpdates":[{"Delete":)"
                       R"({"IndexName":"by_g"}}]})");
    BOOST_TEST(deleted.at("TableDescription")
                   .as_object()
                   .at("GlobalSecondaryIndexes")
                   .as_array()[0]
                   .as_object()
                   .at("IndexStatus")
                   .as_string() == "DELETING");
    BOOST_TEST_REQUIRE(cluster::await([&] {
        const auto d = c.backend(2).describe("Music");
        return d && d->gsis.empty();
    }));
    const auto d = c.backend(1).describe("Music");
    BOOST_REQUIRE(d.has_value());
    BOOST_TEST(count_all(c.node(2), true) == pks.size() * k_per);
    const auto gone = c.node(2).call(
        "Query", R"({"TableName":"Music","IndexName":"by_g","KeyConditionExpression":"g = :g",)"
                 R"("ExpressionAttributeValues":{":g":{"S":"red"}}})");
    BOOST_TEST(gone.message() == "The table does not have the specified index: by_g");
}

BOOST_AUTO_TEST_CASE(synchronous_gsis_are_read_your_writes) {
    auto options = test_options();
    // The item leaders' propagators sleep through the test, so only a
    // synchronous table's own writes can reach its index.
    options.gsi_interval = std::chrono::milliseconds{120000};
    cluster c(options);
    const auto with_tag = [](const std::string& name, const std::string& tags) {
        auto body = k_create_with_gsi;
        body.replace(body.find("\"Music\""), 7, "\"" + name + "\"");
        body.insert(body.size() - 1, tags);
        return body;
    };
    c.node(1).ok(
        "CreateTable",
        with_tag("Sync", R"(,"Tags":[{"Key":"kythira:compat:gsi-consistency","Value":"sync"}])"));
    c.node(1).ok("CreateTable", with_tag("Async", ""));
    BOOST_TEST_REQUIRE(cluster::await([&] {
        return status_of(c.node(1), "Sync") == "ACTIVE" &&
               status_of(c.node(1), "Async") == "ACTIVE";
    }));
    const auto query = [&](std::uint64_t node, const std::string& table) {
        const auto r = c.node(node).ok(
            "Query", R"({"TableName":")" + table +
                         R"(","IndexName":"by_g","KeyConditionExpression":"g = :g",)"
                         R"("ExpressionAttributeValues":{":g":{"S":"red"}}})");
        return r.at("Count").to_number<int>();
    };
    for (const char* table : {"Sync", "Async"}) {
        c.node(2).ok("PutItem", R"({"TableName":")" + std::string(table) +
                                    R"(","Item":{"pk":{"S":"a"},"sk":{"N":"1"},"g":{"S":"red"},)"
                                    R"("v":{"N":"1"}}})");
    }
    // The synchronous table's entry is there when PutItem answers, on
    // every node; the asynchronous one waits for a propagator.
    for (std::uint64_t id = 1; id <= k_nodes; ++id) {
        BOOST_TEST(cluster::await([&] { return query(id, "Sync") == 1; }), "node " << id);
    }
    BOOST_TEST(query(c.leader(k_item_group), "Sync") == 1);
    BOOST_TEST(query(c.leader(k_item_group), "Async") == 0);
    // Moving and deleting the item, through a transaction too.
    c.node(3).ok("TransactWriteItems",
                 R"({"TransactItems":[{"Update":{"TableName":"Sync","Key":)" + key_json("a", 1) +
                     R"(,"UpdateExpression":"SET g = :b","ExpressionAttributeValues":)"
                     R"({":b":{"S":"blue"}}}}]})");
    BOOST_TEST(query(c.leader(k_item_group), "Sync") == 0);
}

BOOST_AUTO_TEST_CASE(change_logs_agree_across_replicas_and_a_split) {
    cluster c;
    c.node(1).ok("CreateTable", k_create);
    BOOST_TEST_REQUIRE(cluster::await([&] { return status_of(c.node(1), "Music") == "ACTIVE"; }));
    const auto d = c.backend(1).describe("Music");
    BOOST_TEST_REQUIRE(d.has_value());
    // Logged writes (no table logs its changes until Streams, so the
    // commands ask for it themselves).
    const auto write = [&](std::uint64_t node, const std::string& pk) {
        ddb::item_command cmd;
        cmd.op = ddb::item_command::kind::put;
        cmd.table_id = d->table_id;
        cmd.schema = d->schema;
        cmd.payload = *ddb::item_from_json(
            json::parse(R"({"pk":{"S":")" + pk + R"("},"sk":{"N":"1"}})").as_object());
        cmd.log_changes = true;
        const auto r = c.backend(node).write(cmd);
        BOOST_REQUIRE((r.outcome == ddb::item_result::status::ok));
    };
    for (const char* pk : {"a", "m", "z"}) {
        write(1, pk);
    }
    const auto state = [&](std::uint64_t id, std::uint64_t group) {
        return c.host(id)
            .with_group_state_machine(
                group,
                [](auto& sm, const auto&) {
                    const auto* g = sm.inner().item_group();
                    return g == nullptr ? std::optional<ddb::change_log_state>{} : g->change_log();
                })
            .value_or(std::nullopt);
    };
    BOOST_TEST_REQUIRE(cluster::await([&] {
        for (std::uint64_t id = 1; id <= k_nodes; ++id) {
            const auto st = state(id, k_item_group);
            if (!st || st->current.last_seq != 3U) {
                return false;
            }
        }
        return true;
    }));
    const auto key = state(1, k_item_group)->current.key;
    for (std::uint64_t id = 2; id <= k_nodes; ++id) {
        BOOST_TEST((state(id, k_item_group) == state(1, k_item_group)));
        BOOST_TEST((c.change_log(id).describe(key, 1) == c.change_log(1).describe(key, 1)));
    }

    // A split closes the stream on every replica, at the same record.
    const auto cut = ddb::partition_prefix(d->table_id, ddb::attribute_value::string("m"));
    const auto l = c.leader(k_item_group);
    BOOST_TEST_REQUIRE(l != 0U);
    auto f = c.host(l).split_shard(k_item_group, {cut}, std::chrono::milliseconds{5000});
    BOOST_TEST_REQUIRE(f.wait(std::chrono::milliseconds{8000}));
    std::ignore = std::move(f).get();
    BOOST_TEST_REQUIRE(cluster::await([&] {
        for (std::uint64_t id = 1; id <= k_nodes; ++id) {
            const auto st = state(id, k_item_group);
            if (!st || st->epoch != 1U) {
                return false;
            }
        }
        return true;
    }));
    const auto parent = state(1, k_item_group);
    BOOST_TEST(parent->current.parents == std::vector<std::string>{key});
    BOOST_TEST_REQUIRE(parent->closed.size() == 1U);
    BOOST_TEST_REQUIRE(parent->closed[0].sealed.size() == 1U);
    const auto sealed = parent->closed[0].sealed[0];
    for (std::uint64_t id = 2; id <= k_nodes; ++id) {
        BOOST_TEST((state(id, k_item_group) == parent));
    }

    // A node that lost a segment fetches it from the group's peers.
    BOOST_TEST_REQUIRE(c.change_log(3).remove_segment(sealed._stream, sealed._first_seq));
    BOOST_TEST(!c.change_log(3).has_segment(sealed));
    c.backend(3).fetch_change_segment(k_item_group, sealed);
    BOOST_TEST(c.change_log(3).has_segment(sealed));
}

BOOST_AUTO_TEST_CASE(a_stream_follows_a_split_and_vetoes_merges) {
    cluster c;
    c.node(1).ok("CreateTable", k_create.substr(0, k_create.size() - 1) +
                                    R"(,"StreamSpecification":{"StreamEnabled":true,)"
                                    R"("StreamViewType":"NEW_AND_OLD_IMAGES"}})");
    BOOST_TEST_REQUIRE(cluster::await([&] { return status_of(c.node(1), "Music") == "ACTIVE"; }));
    const auto d = c.backend(1).describe("Music");
    BOOST_TEST_REQUIRE(d.has_value());
    BOOST_TEST_REQUIRE(d->logs_changes());
    const auto put = [&](const std::string& pk) {
        c.node(1).ok("PutItem",
                     R"({"TableName":"Music","Item":{"pk":{"S":")" + pk + R"("},"sk":{"N":"1"}}})");
    };
    for (const char* pk : {"a", "m", "z"}) {
        put(pk);
    }
    const auto arn = std::string(c.node(1)
                                     .ok("DescribeTable", R"({"TableName":"Music"})")
                                     .at("Table")
                                     .as_object()
                                     .at("LatestStreamArn")
                                     .as_string());
    const auto shards = [&](std::uint64_t node) {
        return c.node(node)
            .ok("DescribeStream", R"({"StreamArn":")" + arn + R"("})")
            .at("StreamDescription")
            .as_object()
            .at("Shards")
            .as_array();
    };
    // Reads every record of a shard from its start, on `node`; also
    // whether the shard ended.
    const auto read = [&](std::uint64_t node, const std::string& shard) {
        auto it = std::string(
            c.node(node)
                .ok("GetShardIterator", R"({"StreamArn":")" + arn + R"(","ShardId":")" + shard +
                                            R"(","ShardIteratorType":"TRIM_HORIZON"})")
                .at("ShardIterator")
                .as_string());
        std::vector<std::string> keys;
        for (int page = 0; page < 10; ++page) {
            const auto r = c.node(node).ok("GetRecords", R"({"ShardIterator":")" + it + R"("})");
            for (const auto& rec : r.at("Records").as_array()) {
                keys.emplace_back(rec.as_object()
                                      .at("dynamodb")
                                      .as_object()
                                      .at("Keys")
                                      .as_object()
                                      .at("pk")
                                      .as_object()
                                      .at("S")
                                      .as_string());
            }
            if (!r.contains("NextShardIterator")) {
                return std::pair{keys, true};
            }
            if (r.at("Records").as_array().empty()) {
                break;
            }
            it = std::string(r.at("NextShardIterator").as_string());
        }
        return std::pair{keys, false};
    };
    const auto before = shards(2);
    BOOST_TEST_REQUIRE(before.size() == 1U);
    const std::string parent(before[0].as_object().at("ShardId").as_string());
    // Any node answers: the read goes to the group's leader.
    for (std::uint64_t node = 1; node <= k_nodes; ++node) {
        BOOST_TEST((read(node, parent).first == std::vector<std::string>{"a", "m", "z"}));
    }

    // A split closes the shard and opens one per child, each naming it.
    const auto cut = ddb::partition_prefix(d->table_id, ddb::attribute_value::string("m"));
    const auto l = c.leader(k_item_group);
    BOOST_TEST_REQUIRE(l != 0U);
    auto f = c.host(l).split_shard(k_item_group, {cut}, std::chrono::milliseconds{5000});
    BOOST_TEST_REQUIRE(f.wait(std::chrono::milliseconds{8000}));
    std::ignore = std::move(f).get();
    BOOST_TEST_REQUIRE(cluster::await([&] { return shards(1).size() == 3U; }));
    put("b");
    put("n");
    const auto after = shards(1);
    BOOST_TEST(after[0].as_object().at("ShardId").as_string() == parent);
    BOOST_TEST(after[0]
                   .as_object()
                   .at("SequenceNumberRange")
                   .as_object()
                   .contains("EndingSequenceNumber"));
    std::vector<std::string> child_keys;
    for (std::size_t i = 1; i < after.size(); ++i) {
        const auto& s = after[i].as_object();
        BOOST_TEST(s.at("ParentShardId").as_string() == parent);
        BOOST_TEST(!s.at("SequenceNumberRange").as_object().contains("EndingSequenceNumber"));
        const auto [keys, ended] = read(3, std::string(s.at("ShardId").as_string()));
        BOOST_TEST(!ended);
        BOOST_TEST(keys.size() == 1U);
        child_keys.insert(child_keys.end(), keys.begin(), keys.end());
    }
    std::ranges::sort(child_keys);
    BOOST_TEST((child_keys == std::vector<std::string>{"b", "n"}));
    // The closed parent still holds its records, and ends.
    const auto [parent_keys, parent_ended] = read(2, parent);
    BOOST_TEST((parent_keys == std::vector<std::string>{"a", "m", "z"}));
    BOOST_TEST(parent_ended);

    // A merge of the two children would give a shard two parents.
    kythira::shard_range<std::string> items;
    items._start = std::string("t/");
    items._end = ddb::prefix_end("t/");
    const auto groups = c.host(1).shard_map_snapshot().range_scan(items);
    BOOST_TEST_REQUIRE(groups.size() == 2U);
    BOOST_TEST(!c.backend(1).merge_groups(groups[1]._group_id, groups[0]._group_id));
}

BOOST_AUTO_TEST_CASE(a_backup_spans_a_split_and_restores_on_another_node) {
    cluster c;
    c.node(1).ok("CreateTable", k_create);
    BOOST_TEST_REQUIRE(cluster::await([&] { return status_of(c.node(1), "Music") == "ACTIVE"; }));
    const auto d = c.backend(1).describe("Music");
    BOOST_TEST_REQUIRE(d.has_value());
    const auto put_pk = [&](const std::string& pk) {
        c.node(1).ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":")" + pk +
                                    R"("},"sk":{"N":"1"},"v":{"S":")" + pk + R"("}}})");
    };
    std::vector<std::string> pks;
    for (char ch = 'a'; ch <= 'z'; ++ch) {
        pks.emplace_back(1, ch);
        put_pk(pks.back());
    }
    // The table spans two groups before the backup is asked for.
    const auto cut = ddb::partition_prefix(d->table_id, ddb::attribute_value::string("m"));
    const auto l = c.leader(k_item_group);
    BOOST_TEST_REQUIRE(l != 0U);
    auto f = c.host(l).split_shard(k_item_group, {cut}, std::chrono::milliseconds{5000});
    BOOST_TEST_REQUIRE(f.wait(std::chrono::milliseconds{8000}));
    std::ignore = std::move(f).get();

    // The API on nodes 1 and 3, the worker on node 2, one shared store.
    auto store = std::make_shared<ddb::memory_backup_store>();
    ddb::backup_api<backend_type> api1{c.backend(1), store};
    ddb::backup_api<backend_type> api3{c.backend(3), store};
    api1.register_with(*c.node(1).fe);
    api3.register_with(*c.node(3).fe);
    ddb::backup_worker<backend_type> worker{c.backend(2), store};

    const auto arn =
        std::string(c.node(1)
                        .ok("CreateBackup", R"({"TableName":"Music","BackupName":"nightly"})")
                        .at("BackupDetails")
                        .as_object()
                        .at("BackupArn")
                        .as_string());
    const auto backup_status = [&] {
        return std::string(c.node(3)
                               .ok("DescribeBackup", R"({"BackupArn":")" + arn + R"("})")
                               .at("BackupDescription")
                               .as_object()
                               .at("BackupDetails")
                               .as_object()
                               .at("BackupStatus")
                               .as_string());
    };
    BOOST_TEST_REQUIRE(cluster::await([&] {
        worker.poke();
        (void)worker.pass();
        return backup_status() == "AVAILABLE";
    }));
    // Written after the backup was taken: not in it.
    put_pk("late");

    const auto r = c.node(3).call("RestoreTableFromBackup",
                                  R"({"TargetTableName":"Copy","BackupArn":")" + arn + R"("})");
    BOOST_TEST_REQUIRE(r.status == 200U);
    BOOST_TEST_REQUIRE(cluster::await([&] {
        worker.poke();
        (void)worker.pass();
        return status_of(c.node(1), "Copy") == "ACTIVE";
    }));
    const auto scanned = c.node(1).ok("Scan", R"({"TableName":"Copy","ConsistentRead":true})");
    BOOST_TEST(scanned.at("Count").to_number<std::int64_t>() == 26);
    std::vector<std::string> got;
    for (const auto& item : scanned.at("Items").as_array()) {
        got.emplace_back(item.as_object().at("v").as_object().at("S").as_string());
    }
    std::ranges::sort(got);
    BOOST_TEST((got == pks));
    // The copy takes writes once loaded, on any node.
    c.node(2).ok("PutItem", R"({"TableName":"Copy","Item":{"pk":{"S":"new"},"sk":{"N":"1"}}})");
    BOOST_TEST(c.node(3)
                   .ok("GetItem", R"({"TableName":"Copy","Key":{"pk":{"S":"new"},"sk":{"N":"1"}},)"
                                  R"("ConsistentRead":true})")
                   .contains("Item"));
}

BOOST_AUTO_TEST_CASE(point_in_time_restores_follow_a_split) {
    cluster c;
    c.node(1).ok("CreateTable", k_create);
    BOOST_TEST_REQUIRE(cluster::await([&] { return status_of(c.node(1), "Music") == "ACTIVE"; }));
    const auto d = c.backend(1).describe("Music");
    BOOST_TEST_REQUIRE(d.has_value());
    auto store = std::make_shared<ddb::memory_backup_store>();
    ddb::pitr_api<backend_type> api1{c.backend(1), store};
    ddb::pitr_api<backend_type> api3{c.backend(3), store};
    api1.register_with(*c.node(1).fe);
    api3.register_with(*c.node(3).fe);
    ddb::pitr_worker_options options;
    options.latest_lag_ms = 0;
    ddb::pitr_worker<backend_type> worker{c.backend(2), store, options};
    c.node(1).ok("UpdateContinuousBackups",
                 R"({"TableName":"Music","PointInTimeRecoverySpecification":)"
                 R"({"PointInTimeRecoveryEnabled":true}})");
    const auto put_v = [&](const std::string& pk, const std::string& v) {
        c.node(1).ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":")" + pk +
                                    R"("},"sk":{"N":"1"},"v":{"S":")" + v + R"("}}})");
    };
    const auto latest = [&] {
        return c.backend(1).describe("Music").value_or(ddb::table_descriptor{}).pitr.latest_ms;
    };
    // Waits for a pass that caught up after `since`.
    const auto archived_past = [&](std::uint64_t since) {
        return cluster::await([&] {
            (void)worker.pass();
            return latest() > since;
        });
    };
    BOOST_TEST_REQUIRE(archived_past(0));
    for (const char* pk : {"a", "m", "z"}) {
        put_v(pk, "1");
    }
    // Every item written again on each side of a split: each one's
    // records sit partly in the parent stream and partly in a child.
    const auto cut = ddb::partition_prefix(d->table_id, ddb::attribute_value::string("m"));
    const auto l = c.leader(k_item_group);
    BOOST_TEST_REQUIRE(l != 0U);
    auto f = c.host(l).split_shard(k_item_group, {cut}, std::chrono::milliseconds{5000});
    BOOST_TEST_REQUIRE(f.wait(std::chrono::milliseconds{8000}));
    std::ignore = std::move(f).get();
    for (const char* pk : {"a", "m", "z"}) {
        put_v(pk, "2");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const auto mid = c.backend(1).now_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    put_v("a", "3");
    c.node(1).ok("DeleteItem", R"({"TableName":"Music","Key":{"pk":{"S":"z"},"sk":{"N":"1"}}})");
    const auto end = c.backend(1).now_ms();
    BOOST_TEST_REQUIRE(archived_past(end));

    const auto restore = [&](const std::string& target, const std::string& when) {
        const auto r = c.node(3).call(
            "RestoreTableToPointInTime",
            R"({"SourceTableName":"Music","TargetTableName":")" + target + R"(",)" + when + "}");
        BOOST_TEST_REQUIRE(r.status == 200U, r.type() << ": " << r.message());
        BOOST_TEST_REQUIRE(cluster::await([&] {
            (void)worker.pass();
            return status_of(c.node(1), target) == "ACTIVE";
        }));
        std::map<std::string, std::string> out;
        const auto scanned =
            c.node(2).ok("Scan", R"({"TableName":")" + target + R"(","ConsistentRead":true})");
        for (const auto& item : scanned.at("Items").as_array()) {
            const auto& o = item.as_object();
            out[std::string(o.at("pk").as_object().at("S").as_string())] =
                std::string(o.at("v").as_object().at("S").as_string());
        }
        return out;
    };
    using items = std::map<std::string, std::string>;
    BOOST_TEST((restore("AtMid", R"("RestoreDateTime":)" +
                                     std::to_string(static_cast<double>(mid) / 1000.0)) ==
                items{{"a", "2"}, {"m", "2"}, {"z", "2"}}));
    BOOST_TEST(
        (restore("AtEnd", R"("UseLatestRestorableTime":true)") == items{{"a", "3"}, {"m", "2"}}));
}
