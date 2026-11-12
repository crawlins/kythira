// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Global tables (`.kiro/specs/dynamodb-compatible-api/` task 22,
// Requirements 18.1 and 18.2) over clusters of local_backend linked
// in-process through each other's front ends, so every peer call is
// SigV4-signed and authorized as it is over HTTP: `ReplicaUpdates` and the
// statuses `DescribeTable` reports, the backfill and its stamps, writes in
// both directions, last-writer-wins under clock skew, a three-region mesh,
// replica deletion, an unreachable peer, a refused creation, and the
// refusals.

#define BOOST_TEST_MODULE dynamodb_global_table_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/global_tables.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace dynamodb_test;

namespace {

constexpr std::string_view k_east = "us-east-1";
constexpr std::string_view k_west = "us-west-2";
constexpr std::string_view k_europe = "eu-west-1";

/// A request signed for `region` as an SDK signs it.
auto signed_in(std::string_view region, std::string_view op, const std::string& body)
    -> ddb::front_end_request {
    ddb::front_end_request r;
    r.method = "POST";
    r.path = "/";
    r.body = body;
    r.source_ip = "192.0.2.10";
    r.headers = {
        {"Host", "localhost:8000"},
        {"Content-Type", "application/x-amz-json-1.0"},
        {"X-Amz-Date", "20261110T120000Z"},
        {"X-Amz-Target", std::string(ddb::k_dynamodb_target_prefix) + "." + std::string(op)}};
    sv4::service_rules rules;
    rules.service = "dynamodb";
    rules.protocol = sv4::error_protocol::json;
    const sv4::credential_scope scope{"20261110", std::string(region), "dynamodb"};
    r.headers.emplace_back("Authorization",
                           "AWS4-HMAC-SHA256 Credential=AKIDADMIN/" + scope.text() +
                               ", SignedHeaders=content-type;host;x-amz-date;x-amz-target"
                               ", Signature=" +
                               std::string(64, '0'));
    const auto parsed =
        sv4::parse(sv4::request{r.method, r.path, r.query, r.headers, r.body}, rules, k_now);
    BOOST_REQUIRE(parsed.has_value());
    const auto sig = sv4::compute_signature(*parsed, sv4::derive_signing_key(k_secret, scope));
    r.headers.back().second.replace(r.headers.back().second.size() - 64, 64, sig);
    return r;
}

/// One region: a single-node cluster with the table, item and peer APIs.
struct region_cluster {
    std::string region;
    kythira::memory_kv_engine engine;
    std::uint64_t clock_ms;
    scratch_dir log_dir;
    kythira::record_segment_store change_log{change_log_options(log_dir.path)};
    ddb::local_backend<> backend{engine, [this] { return clock_ms; }, &change_log};
    az::static_key_authorizer auth{
        std::vector<az::static_key>{{"AKIDADMIN", std::string(k_secret), std::string(k_account),
                                     "admin", az::static_role::admin}},
        "aws", [] { return std::chrono::system_clock::time_point(k_now); }};
    std::unique_ptr<ddb::front_end<az::static_key_authorizer>> fe;
    ddb::table_api<ddb::local_backend<>> tables{backend};
    ddb::item_api<ddb::local_backend<>> items{backend};
    ddb::query_api<ddb::local_backend<>> queries{backend};
    ddb::replication_receiver<ddb::local_backend<>> receiver{backend};
    ddb::peer_set peers;
    std::vector<std::string> log;
    ddb::global_table_replicator<ddb::local_backend<>> replicator{
        backend, &peers, {.log = [this](std::string_view line) { log.emplace_back(line); }}};
    /// Peer calls to this region fail as if it could not be reached.
    bool down = false;

    region_cluster(std::string_view r, std::uint64_t clock) : region(r), clock_ms(clock) {
        auth.register_service(ddb::dynamodb_static_roles());
        ddb::front_end_config cfg;
        cfg.region = region;
        cfg.advertised_endpoint = "localhost:8000";
        fe = std::make_unique<ddb::front_end<az::static_key_authorizer>>(cfg, auth);
        tables.register_with(*fe);
        items.register_with(*fe);
        queries.register_with(*fe);
        receiver.register_with(*fe);
        tables.set_peers([this](std::string_view p) { return peers.has(p); });
        const auto off = ddb::cluster_profile({{"throttling", "off"}}).value();
        items.set_compat_profile(off);
        queries.set_compat_profile(off);
    }

    auto call(std::string_view op, const std::string& body) -> reply {
        const auto r = fe->handle(signed_in(region, op, body));
        return reply{r.status, json::parse(r.body).as_object()};
    }

    auto ok(std::string_view op, const std::string& body) -> json::object {
        auto r = call(op, body);
        BOOST_REQUIRE_MESSAGE(r.status == 200U, region + " " + std::string(op) +
                                                    " failed: " + r.type() + ": " + r.message());
        return r.body;
    }

    /// A peer's replication call, signed with the peer's keys for this
    /// region, as `http_replication_send` signs it.
    auto replicate(const json::object& request) -> kythira::aws_json::result<json::object> {
        if (down) {
            kythira::aws_json::error e;
            e.status = 0;
            e.type = "Unreachable";
            return e;
        }
        ddb::replication_peer_config cfg;
        cfg.region = region;
        cfg.endpoint = "localhost:8000";
        cfg.access_key_id = "AKIDADMIN";
        cfg.secret_access_key = std::string(k_secret);
        cfg.clock = [] { return std::chrono::time_point_cast<std::chrono::seconds>(k_now); };
        ddb::front_end_request r;
        r.method = "POST";
        r.path = std::string(ddb::k_replication_path);
        r.body = json::serialize(request);
        r.source_ip = "192.0.2.20";
        r.headers = ddb::global_tables_detail::signed_replication_headers(cfg, r.body);
        const auto res = fe->handle(r);
        if (res.status != 200U) {
            return ddb::global_tables_detail::answer_error(
                ddb::kinesis_http_answer{static_cast<int>(res.status), res.body});
        }
        return json::parse(res.body).as_object();
    }

    void create(const std::string& name) {
        (void)ok("CreateTable",
                 R"({"TableName":")" + name +
                     R"(","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
                     R"({"AttributeName":"sk","AttributeType":"N"}],)"
                     R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
                     R"({"AttributeName":"sk","KeyType":"RANGE"}],)"
                     R"("BillingMode":"PAY_PER_REQUEST"})");
    }

    void put(const std::string& table, const std::string& pk, int sk, const std::string& v) {
        (void)ok("PutItem", R"({"TableName":")" + table + R"(","Item":{"pk":{"S":")" + pk +
                                R"("},"sk":{"N":")" + std::to_string(sk) + R"("},"v":{"S":")" + v +
                                R"("}}})");
    }

    void del(const std::string& table, const std::string& pk, int sk) {
        (void)ok("DeleteItem", R"({"TableName":")" + table + R"(","Key":{"pk":{"S":")" + pk +
                                   R"("},"sk":{"N":")" + std::to_string(sk) + R"("}}})");
    }

    /// The item's `v`, or nothing when it is not there.
    auto get(const std::string& table, const std::string& pk, int sk)
        -> std::optional<std::string> {
        const auto r = ok("GetItem", R"({"TableName":")" + table + R"(","Key":{"pk":{"S":")" + pk +
                                         R"("},"sk":{"N":")" + std::to_string(sk) +
                                         R"("}},"ConsistentRead":true})");
        const auto* it = r.if_contains("Item");
        if (it == nullptr) {
            return std::nullopt;
        }
        return std::string(it->as_object().at("v").as_object().at("S").as_string());
    }

    auto describe(const std::string& table) -> reply {
        return call("DescribeTable", R"({"TableName":")" + table + R"("})");
    }

    /// `Replicas` as region to status.
    auto replicas(const std::string& table) -> std::map<std::string, std::string> {
        const auto r = ok("DescribeTable", R"({"TableName":")" + table + R"("})");
        std::map<std::string, std::string> out;
        const auto& t = r.at("Table").as_object();
        if (const auto* list = t.if_contains("Replicas")) {
            for (const auto& x : list->as_array()) {
                const auto& o = x.as_object();
                out.emplace(std::string(o.at("RegionName").as_string()),
                            std::string(o.at("ReplicaStatus").as_string()));
            }
        }
        return out;
    }

    auto replica_updates(const std::string& table, const std::string& updates) -> reply {
        return call("UpdateTable",
                    R"({"TableName":")" + table + R"(","ReplicaUpdates":)" + updates + "}");
    }

    auto add_replica(const std::string& table, std::string_view to) -> reply {
        return replica_updates(table,
                               R"([{"Create":{"RegionName":")" + std::string(to) + R"("}}])");
    }
};

/// Links every cluster to every other.
void link(const std::vector<region_cluster*>& all) {
    for (auto* a : all) {
        for (auto* b : all) {
            if (a != b) {
                a->peers.add(b->region, [b](const json::object& req) { return b->replicate(req); });
            }
        }
    }
}

/// Replicator passes on every cluster until none moves anything.
void pump(const std::vector<region_cluster*>& all) {
    for (int round = 0; round < 30; ++round) {
        std::size_t moved = 0;
        for (auto* c : all) {
            moved += c->replicator.pass();
        }
        if (moved == 0) {
            return;
        }
    }
    BOOST_FAIL("the replicators never settled");
}

struct two_regions {
    region_cluster east{k_east, 1794300000000ULL};
    region_cluster west{k_west, 1794300000000ULL};
    std::vector<region_cluster*> all{&east, &west};

    two_regions() { link(all); }

    /// A table in the east with a replica in the west, ACTIVE.
    void global(const std::string& table) {
        east.create(table);
        (void)east.ok("UpdateTable", R"({"TableName":")" + table +
                                         R"(","ReplicaUpdates":[{"Create":{"RegionName":")" +
                                         std::string(k_west) + R"("}}]})");
        pump(all);
        BOOST_REQUIRE_EQUAL(east.replicas(table).at(std::string(k_west)), "ACTIVE");
    }
};

}  // namespace

BOOST_FIXTURE_TEST_CASE(create_replica_backfills_and_reports_status, two_regions) {
    east.create("orders");
    east.put("orders", "a", 1, "one");
    east.put("orders", "b", 2, "two");
    east.del("orders", "b", 2);
    east.put("orders", "c", 3, "three");

    const auto r = east.add_replica("orders", k_west);
    BOOST_REQUIRE_EQUAL(r.status, 200U);
    const auto& t = r.body.at("TableDescription").as_object();
    BOOST_TEST(t.at("GlobalTableVersion").as_string() == "2019.11.21");
    BOOST_TEST(east.replicas("orders").at(std::string(k_west)) == "CREATING");
    // Not there yet: nothing has run.
    BOOST_TEST(west.describe("orders").type() == "ResourceNotFoundException");

    pump(all);
    BOOST_TEST(east.replicas("orders").at(std::string(k_west)) == "ACTIVE");
    // The replica lists the region it came from, and not its own.
    const auto west_view = west.replicas("orders");
    BOOST_TEST(west_view.size() == 1U);
    BOOST_TEST(west_view.at(std::string(k_east)) == "ACTIVE");
    BOOST_TEST(west.get("orders", "a", 1).value_or("") == "one");
    BOOST_TEST(!west.get("orders", "b", 2).has_value());
    BOOST_TEST(west.get("orders", "c", 3).value_or("") == "three");
    // A table that is not global says nothing of replicas.
    east.create("plain");
    const auto plain = east.ok("DescribeTable", R"({"TableName":"plain"})");
    BOOST_TEST(!plain.at("Table").as_object().contains("GlobalTableVersion"));
    BOOST_TEST(!plain.at("Table").as_object().contains("Replicas"));
}

BOOST_FIXTURE_TEST_CASE(writes_flow_both_ways_and_settle, two_regions) {
    global("orders");
    east.put("orders", "k", 1, "from-east");
    pump(all);
    BOOST_TEST(west.get("orders", "k", 1).value_or("") == "from-east");

    west.clock_ms += 10;
    west.put("orders", "k", 1, "from-west");
    west.put("orders", "w", 2, "west-only");
    pump(all);
    BOOST_TEST(east.get("orders", "k", 1).value_or("") == "from-west");
    BOOST_TEST(east.get("orders", "w", 2).value_or("") == "west-only");

    west.clock_ms += 10;
    west.del("orders", "w", 2);
    (void)east.ok(
        "UpdateItem",
        R"({"TableName":"orders","Key":{"pk":{"S":"k"},"sk":{"N":"1"}},)"
        R"("UpdateExpression":"SET v = :v","ExpressionAttributeValues":{":v":{"S":"upd"}}})");
    pump(all);
    BOOST_TEST(!east.get("orders", "w", 2).has_value());
    BOOST_TEST(west.get("orders", "k", 1).value_or("") == "upd");
    // Settled: another round sends nothing either way.
    BOOST_TEST(east.replicator.pass() == 0U);
    BOOST_TEST(west.replicator.pass() == 0U);
}

BOOST_FIXTURE_TEST_CASE(last_writer_wins_under_clock_skew, two_regions) {
    global("orders");
    // The west's clock runs five seconds ahead.
    west.clock_ms = east.clock_ms + 5000;
    east.put("orders", "k", 1, "east-first");
    west.put("orders", "k", 1, "west-later-stamp");
    pump(all);
    BOOST_TEST(east.get("orders", "k", 1).value_or("") == "west-later-stamp");
    BOOST_TEST(west.get("orders", "k", 1).value_or("") == "west-later-stamp");

    // A write made in the east after it saw the west's wins everywhere,
    // though the east's clock is behind the stamp it overwrites.
    east.clock_ms += 10;
    east.put("orders", "k", 1, "east-after");
    pump(all);
    BOOST_TEST(east.get("orders", "k", 1).value_or("") == "east-after");
    BOOST_TEST(west.get("orders", "k", 1).value_or("") == "east-after");

    // Concurrent writes with the same time: the greater region name wins.
    west.clock_ms = east.clock_ms = east.clock_ms + 60000;
    east.put("orders", "t", 1, "east");
    west.put("orders", "t", 1, "west");
    pump(all);
    BOOST_TEST(east.get("orders", "t", 1).value_or("") == "west");
    BOOST_TEST(west.get("orders", "t", 1).value_or("") == "west");

    // A delete is a write like any other: an older put does not bring the
    // item back.
    west.clock_ms += 100;
    west.del("orders", "t", 1);
    pump(all);
    const auto stale = west.replicate(json::object{
        {"Action", "Apply"},
        {"TableName", "orders"},
        {"SourceRegion", std::string(k_east)},
        {"Writes",
         json::array{json::object{{"Item", json::object{{"pk", json::object{{"S", "t"}}},
                                                        {"sk", json::object{{"N", "1"}}},
                                                        {"v", json::object{{"S", "stale"}}}}},
                                  {"WrittenMs", east.clock_ms - 1},
                                  {"Region", std::string(k_east)}}}}});
    BOOST_REQUIRE(stale.has_value());
    BOOST_TEST(!west.get("orders", "t", 1).has_value());
}

BOOST_AUTO_TEST_CASE(three_region_mesh) {
    region_cluster east{k_east, 1794300000000ULL};
    region_cluster west{k_west, 1794300000000ULL};
    region_cluster europe{k_europe, 1794300000000ULL};
    const std::vector<region_cluster*> all{&east, &west, &europe};
    link(all);
    east.create("orders");
    east.put("orders", "a", 1, "before");
    BOOST_REQUIRE_EQUAL(east.add_replica("orders", k_west).status, 200U);
    pump(all);
    // A write in the west on its way east when Europe joins still reaches
    // Europe.
    west.clock_ms += 5;
    west.put("orders", "w", 1, "west");
    BOOST_REQUIRE_EQUAL(east.add_replica("orders", k_europe).status, 200U);
    // One change at a time per table.
    BOOST_TEST(east.add_replica("orders", k_europe).type() == "ValidationException");
    pump(all);
    BOOST_TEST(east.replicas("orders").at(std::string(k_europe)) == "ACTIVE");
    BOOST_TEST(west.replicas("orders").size() == 2U);
    BOOST_TEST(west.replicas("orders").at(std::string(k_europe)) == "ACTIVE");
    BOOST_TEST(europe.replicas("orders").size() == 2U);
    BOOST_TEST(europe.get("orders", "a", 1).value_or("") == "before");
    BOOST_TEST(europe.get("orders", "w", 1).value_or("") == "west");

    europe.clock_ms += 50;
    europe.put("orders", "e", 1, "europe");
    pump(all);
    BOOST_TEST(east.get("orders", "e", 1).value_or("") == "europe");
    BOOST_TEST(west.get("orders", "e", 1).value_or("") == "europe");

    // Deleting Europe's replica from the east tells the west too.
    BOOST_REQUIRE_EQUAL(east.replica_updates("orders", R"([{"Delete":{"RegionName":")" +
                                                           std::string(k_europe) + R"("}}])")
                            .status,
                        200U);
    BOOST_TEST(east.replicas("orders").at(std::string(k_europe)) == "DELETING");
    pump(all);
    BOOST_TEST(!east.replicas("orders").contains(std::string(k_europe)));
    BOOST_TEST(!west.replicas("orders").contains(std::string(k_europe)));
    BOOST_TEST(europe.describe("orders").type() == "ResourceNotFoundException");
    east.put("orders", "x", 1, "after");
    pump(all);
    BOOST_TEST(west.get("orders", "x", 1).value_or("") == "after");
}

BOOST_FIXTURE_TEST_CASE(backfill_carries_stamps, two_regions) {
    global("orders");
    // The west is ahead: its write's stamp is later than the east's clock.
    west.clock_ms = east.clock_ms + 5000;
    west.put("orders", "k", 1, "west");
    pump(all);
    region_cluster europe{k_europe, east.clock_ms};
    all.push_back(&europe);
    link(all);
    BOOST_REQUIRE_EQUAL(east.add_replica("orders", k_europe).status, 200U);
    pump(all);
    BOOST_TEST(europe.get("orders", "k", 1).value_or("") == "west");
    // A write older than the west's stamp, though newer than Europe's own
    // clock, does not apply there: the copy kept the stamp.
    const auto older = europe.replicate(json::object{
        {"Action", "Apply"},
        {"TableName", "orders"},
        {"SourceRegion", std::string(k_east)},
        {"Writes",
         json::array{json::object{{"Item", json::object{{"pk", json::object{{"S", "k"}}},
                                                        {"sk", json::object{{"N", "1"}}},
                                                        {"v", json::object{{"S", "older"}}}}},
                                  {"WrittenMs", west.clock_ms - 1},
                                  {"Region", std::string(k_east)}}}}});
    BOOST_REQUIRE(older.has_value());
    BOOST_TEST(europe.get("orders", "k", 1).value_or("") == "west");
    all.pop_back();
}

BOOST_FIXTURE_TEST_CASE(unreachable_peer_and_refused_creation, two_regions) {
    east.create("orders");
    west.down = true;
    BOOST_REQUIRE_EQUAL(east.add_replica("orders", k_west).status, 200U);
    pump(all);
    BOOST_TEST(east.replicas("orders").at(std::string(k_west)) == "CREATING");
    west.down = false;
    pump(all);
    BOOST_TEST(east.replicas("orders").at(std::string(k_west)) == "ACTIVE");

    // A table of that name that is not the replica: CREATION_FAILED.
    east.create("taken");
    west.create("taken");
    BOOST_REQUIRE_EQUAL(east.add_replica("taken", k_west).status, 200U);
    pump(all);
    const auto d = east.ok("DescribeTable", R"({"TableName":"taken"})");
    const auto& rep = d.at("Table").as_object().at("Replicas").as_array().at(0).as_object();
    BOOST_TEST(rep.at("ReplicaStatus").as_string() == "CREATION_FAILED");
    BOOST_TEST(std::string(rep.at("ReplicaStatusDescription").as_string()).find("taken") !=
               std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(refusals, two_regions) {
    east.create("orders");
    const auto unknown = east.add_replica("orders", "ap-south-1");
    BOOST_TEST(unknown.type() == "ValidationException");
    BOOST_TEST(unknown.message().find("peer") != std::string::npos);
    BOOST_TEST(east.add_replica("orders", k_east).type() == "ValidationException");
    BOOST_TEST(east.replica_updates("orders", R"([{"Update":{"RegionName":"us-west-2"}}])")
                   .message()
                   .find("not support") != std::string::npos);
    BOOST_TEST(east.replica_updates("orders", R"([{"Create":{"RegionName":"us-west-2",)"
                                              R"("KMSMasterKeyId":"k"}}])")
                   .message()
                   .find("not support") != std::string::npos);
    BOOST_TEST(east.replica_updates("orders", "[]").type() == "ValidationException");
    BOOST_TEST(
        east.replica_updates("orders", R"([{"Delete":{"RegionName":"us-west-2"}}])").type() ==
        "ValidationException");
    BOOST_TEST(east.call("UpdateTable", R"({"TableName":"orders","BillingMode":"PROVISIONED",)"
                                        R"("ProvisionedThroughput":{"ReadCapacityUnits":1,)"
                                        R"("WriteCapacityUnits":1},"ReplicaUpdates":[]})")
                   .type() == "ValidationException");
    BOOST_TEST(east.add_replica("missing", k_west).type() == "ResourceNotFoundException");

    BOOST_REQUIRE_EQUAL(east.add_replica("orders", k_west).status, 200U);
    // A global table is not deleted from under its replicas.
    const auto del = east.call("DeleteTable", R"({"TableName":"orders"})");
    BOOST_TEST(del.type() == "ValidationException");
    BOOST_TEST(del.message().find("ReplicaUpdates") != std::string::npos);

    // The peer path wants a signature, and a known Action.
    ddb::front_end_request unsigned_call;
    unsigned_call.method = "POST";
    unsigned_call.path = std::string(ddb::k_replication_path);
    unsigned_call.body = R"({"Action":"Apply","TableName":"orders"})";
    BOOST_TEST(west.fe->handle(unsigned_call).status == 400U);
    const auto bad = west.replicate(json::object{{"Action", "Nope"}, {"TableName", "orders"}});
    BOOST_REQUIRE(!bad.has_value());
    BOOST_TEST(bad.error().message.value_or("").find("Action") != std::string::npos);
    pump(all);
    // Writes from a region the table does not know are refused.
    const auto stranger = west.replicate(json::object{{"Action", "Apply"},
                                                      {"TableName", "orders"},
                                                      {"SourceRegion", "ap-south-1"},
                                                      {"Writes", json::array{}}});
    BOOST_REQUIRE(!stranger.has_value());
    BOOST_TEST(stranger.error().type.ends_with("ValidationException"));
}

BOOST_AUTO_TEST_CASE(stored_forms_round_trip) {
    ddb::table_descriptor d;
    d.name = "orders";
    d.table_id = 7;
    d.schema.partition_name = "pk";
    d.region = "us-east-1";
    ddb::table_replica r{.region = "us-west-2",
                         .status = ddb::replica_status::creation_failed,
                         .created_ms = 42,
                         .backfilled = true,
                         .cursors = {{"s/1", 9}, {"s/2", 3}},
                         .failure = "no"};
    d.replicas.push_back(r);
    const auto back = ddb::decode_table_descriptor(ddb::encode_table_descriptor(d));
    BOOST_REQUIRE(back.has_value());
    BOOST_TEST(back->region == "us-east-1");
    BOOST_REQUIRE_EQUAL(back->replicas.size(), 1U);
    BOOST_TEST((back->replicas.front() == r));

    ddb::item_command c;
    c.op = ddb::item_command::kind::replicate;
    c.table_id = 7;
    c.schema.partition_name = "pk";
    c.payload = {{"pk", ddb::attribute_value::string("x")}};
    c.region = "us-west-2";
    c.replica_delete = true;
    c.written_ms = 99;
    c.log_changes = true;
    ddb::item_command decoded;
    BOOST_REQUIRE((ddb::decode_item_command(ddb::encode_item_command(c), decoded) ==
                   ddb::item_command_status::ok));
    BOOST_TEST((decoded.op == ddb::item_command::kind::replicate));
    BOOST_TEST(decoded.region == "us-west-2");
    BOOST_TEST(decoded.replica_delete);
    BOOST_TEST(decoded.written_ms == 99U);

    // A plain table's forms stay at their older versions.
    ddb::table_descriptor plain = d;
    plain.region.clear();
    plain.replicas.clear();
    BOOST_TEST(ddb::catalog_detail::version_for(plain) < 10);
}
