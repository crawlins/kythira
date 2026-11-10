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

#include <kythira/dynamodb/group_state_machine.hpp>
#include <kythira/dynamodb/raft_backend.hpp>

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
    using state_machine_type = ddb::dynamo_group_state_machine<kythira::memory_kv_engine>;

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
          batches(backend, &guard) {
        auth.register_service(ddb::dynamodb_static_roles());
        ddb::front_end_config cfg;
        cfg.advertised_endpoint = "localhost:8000";
        fe = std::make_unique<ddb::front_end<az::static_key_authorizer>>(cfg, auth);
        tables.register_with(*fe);
        items.register_with(*fe);
        queries.register_with(*fe);
        batches.register_with(*fe);
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
