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
#include <raft/txn_envelope.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
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
          transactions(backend, &guard) {
        auth.register_service(ddb::dynamodb_static_roles());
        ddb::front_end_config cfg;
        cfg.advertised_endpoint = "localhost:8000";
        fe = std::make_unique<ddb::front_end<az::static_key_authorizer>>(cfg, auth);
        tables.register_with(*fe);
        items.register_with(*fe);
        queries.register_with(*fe);
        batches.register_with(*fe);
        transactions.register_with(*fe);
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
