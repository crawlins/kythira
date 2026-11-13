// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Multi-region strong consistency (`.kiro/specs/dynamodb-compatible-api/`
// task 27): the strong catalog's ids and the backend that routes each call
// to the host owning its table. The cluster's host and the strong host are
// two local_backends here; the Raft-hosted pair is tested in
// dynamodb_gateway_cluster_test.

#define BOOST_TEST_MODULE dynamodb_strong_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/backup.hpp>
#include <kythira/dynamodb/pitr.hpp>
#include <kythira/dynamodb/routed_backend.hpp>
#include <kythira/dynamodb/strong_tables.hpp>

#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dynamodb_test;

namespace {

const std::string k_create_s =
    R"({"TableName":"Strong","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
    R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],"BillingMode":"PAY_PER_REQUEST"})";
const std::string k_create_l =
    R"({"TableName":"Local","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
    R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],"BillingMode":"PAY_PER_REQUEST"})";

auto seed(ddb::local_backend<>& b, std::uint64_t id) -> void {
    ddb::catalog_command c;
    c.op = ddb::catalog_command::kind::seed_ids;
    c.table.table_id = id;
    BOOST_TEST_REQUIRE(b.catalog(c).ok());
}

auto put_of(const ddb::table_descriptor& d, const std::string& pk) -> ddb::item_command {
    ddb::item_command c;
    c.op = ddb::item_command::kind::put;
    c.table_id = d.table_id;
    c.schema = d.schema;
    c.payload = ddb::item{{"pk", ddb::attribute_value::string(pk)}};
    return c;
}

auto key_of(const std::string& pk) -> ddb::item {
    return ddb::item{{"pk", ddb::attribute_value::string(pk)}};
}

auto count_in(auto& backend, std::uint64_t table_id) -> std::size_t {
    std::size_t n = 0;
    backend.read_range(ddb::table_range(table_id), kythira::scan_direction::forward, true,
                       [&](std::string_view, const ddb::item&) {
                           ++n;
                           return true;
                       });
    return n;
}

/// A region's node: its cluster's host (`local`) and the strong host
/// (`strong`), whose catalog was seeded as a strong voter seeds it.
struct region {
    fixture local;
    fixture strong;
    ddb::routed_backend<ddb::local_backend<>> routed{local.backend, &strong.backend};
    ddb::table_descriptor s;
    ddb::table_descriptor l;

    region() {
        seed(strong.backend, ddb::k_strong_table_id_base);
        strong.ok("CreateTable", k_create_s);
        local.ok("CreateTable", k_create_l);
        s = *strong.backend.describe("Strong");
        l = *local.backend.describe("Local");
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(the_strong_catalog_hands_out_ids_from_the_strong_base) {
    region r;
    BOOST_TEST(r.s.table_id == ddb::k_strong_table_id_base);
    BOOST_TEST(ddb::is_strong_table(r.s.table_id));
    BOOST_TEST(!ddb::is_strong_table(r.l.table_id));
    BOOST_TEST(r.l.table_id < 16U);
    // Every key of a strong table sorts at or after the strong range, and
    // every key of a cluster table before it.
    BOOST_TEST(ddb::is_strong_key(ddb::table_prefix(r.s.table_id)));
    BOOST_TEST(!ddb::is_strong_key(ddb::table_prefix(r.l.table_id)));
    BOOST_TEST(!ddb::is_strong_key(ddb::table_prefix(ddb::k_strong_table_id_base - 1)));
    BOOST_TEST(
        (ddb::table_range(ddb::k_strong_table_id_base - 1).end.value() <= ddb::strong_key_start()));

    // Every strong voter seeds at start; only the first changes anything,
    // and a lower seed never moves the next id back.
    seed(r.strong.backend, ddb::k_strong_table_id_base);
    seed(r.strong.backend, 5);
    r.strong.ok("CreateTable", R"({"TableName":"Strong2",)" + k_create_s.substr(22));
    BOOST_TEST(r.strong.backend.describe("Strong2")->table_id == ddb::k_strong_table_id_base + 1);
}

BOOST_AUTO_TEST_CASE(the_seed_command_round_trips_and_needs_format_11) {
    ddb::catalog_command c;
    c.op = ddb::catalog_command::kind::seed_ids;
    c.table.table_id = ddb::k_strong_table_id_base;
    const auto bytes = ddb::encode_catalog_command(c);
    BOOST_TEST(static_cast<int>(static_cast<std::uint8_t>(bytes[0])) == 11);
    bool newer = false;
    const auto back = ddb::decode_catalog_command(bytes, newer);
    BOOST_TEST_REQUIRE(back.has_value());
    BOOST_TEST((back->op == ddb::catalog_command::kind::seed_ids));
    BOOST_TEST(back->table.table_id == ddb::k_strong_table_id_base);
    // A node on format 10 could not apply it, so it must not read it as
    // something else.
    auto old = bytes;
    old[0] = static_cast<char>(10);
    BOOST_TEST(!ddb::decode_catalog_command(old, newer).has_value());
}

BOOST_AUTO_TEST_CASE(items_go_to_the_host_owning_their_table) {
    region r;
    BOOST_TEST(
        (r.routed.write(put_of(r.s, "strong-item")).outcome == ddb::item_result::status::ok));
    BOOST_TEST((r.routed.write(put_of(r.l, "local-item")).outcome == ddb::item_result::status::ok));

    BOOST_TEST(
        r.strong.backend.read(r.s.table_id, r.s.schema, key_of("strong-item"), true).has_value());
    BOOST_TEST(
        !r.local.backend.read(r.s.table_id, r.s.schema, key_of("strong-item"), true).has_value());
    BOOST_TEST(
        r.local.backend.read(r.l.table_id, r.l.schema, key_of("local-item"), true).has_value());
    BOOST_TEST(
        !r.strong.backend.read(r.l.table_id, r.l.schema, key_of("local-item"), true).has_value());

    BOOST_TEST(r.routed.read(r.s.table_id, r.s.schema, key_of("strong-item"), false).has_value());
    BOOST_TEST(r.routed.read(r.l.table_id, r.l.schema, key_of("local-item"), false).has_value());
    BOOST_TEST(count_in(r.routed, r.s.table_id) == 1U);
    BOOST_TEST(count_in(r.routed, r.l.table_id) == 1U);
    BOOST_TEST(count_in(r.local.backend, r.s.table_id) == 0U);

    // The catalog the API reads is the cluster's.
    BOOST_TEST(r.routed.describe("Local").has_value());
    BOOST_TEST(!r.routed.describe("Strong").has_value());
    BOOST_TEST((r.routed.list_tables("", 10) == std::vector<std::string>{"Local"}));
}

BOOST_AUTO_TEST_CASE(change_streams_go_to_the_host_owning_their_range) {
    region r;
    const auto shards = r.routed.change_shards(ddb::table_range(r.s.table_id));
    BOOST_TEST_REQUIRE(!shards.empty());
    for (const auto& sh : shards) {
        const auto parsed = ddb::parse_change_stream_key(sh.stream.key);
        BOOST_TEST_REQUIRE(parsed.has_value());
        // The strong backend here is one group over `t/`; on a strong host
        // the bootstrap layout starts it at the strong range.
        (void)r.routed.read_changes(sh.stream.key, 0, 10);
    }
    BOOST_TEST(!r.routed.change_shards(ddb::table_range(r.l.table_id)).empty());
}

BOOST_AUTO_TEST_CASE(a_range_across_both_hosts_or_a_mixed_transaction_is_refused) {
    region r;
    BOOST_CHECK_THROW(r.routed.read_range(kythira::key_range{"t/", std::nullopt},
                                          kythira::scan_direction::forward, false,
                                          [](std::string_view, const ddb::item&) { return true; }),
                      std::invalid_argument);
    BOOST_CHECK_THROW(r.routed.transact({put_of(r.s, "a"), put_of(r.l, "b")}),
                      std::invalid_argument);
    const auto report = r.routed.transact({put_of(r.s, "a")});
    BOOST_TEST(r.strong.backend.read(r.s.table_id, r.s.schema, key_of("a"), true).has_value());
    (void)report;
}

BOOST_AUTO_TEST_CASE(without_a_strong_host_everything_stays_in_the_cluster) {
    fixture f;
    f.ok("CreateTable", k_create_l);
    const auto d = *f.backend.describe("Local");
    ddb::routed_backend<ddb::local_backend<>> routed(f.backend);
    BOOST_TEST(!routed.has_strong());
    BOOST_TEST((routed.write(put_of(d, "x")).outcome == ddb::item_result::status::ok));
    BOOST_TEST(count_in(f.backend, d.table_id) == 1U);
    BOOST_TEST(count_in(routed, d.table_id) == 1U);
}

// -- UpdateTable with MultiRegionConsistency=STRONG (task 27.4) ---------------

namespace {

using routed = ddb::routed_backend<ddb::local_backend<>>;

/// One region's node as the API sees it: its cluster's catalog and items
/// (`local`), the strong host every region shares (one replicated log, so
/// one backend here), and the handlers over the routed backend.
struct api_region {
    std::string name;
    fixture local;
    routed backend;
    az::static_key_authorizer auth{
        std::vector<az::static_key>{{"AKIDADMIN", std::string(k_secret), std::string(k_account),
                                     "admin", az::static_role::admin}},
        "aws", [] { return std::chrono::system_clock::time_point(k_now); }};
    std::unique_ptr<ddb::front_end<az::static_key_authorizer>> fe;
    ddb::table_api<routed> tables{backend};
    ddb::item_api<routed> items{backend};
    ddb::ttl_api<routed> ttl{backend};
    ddb::txn_api<routed> transactions{backend};
    ddb::query_api<routed> queries{backend};
    ddb::streams_api<routed> streams{backend};
    ddb::strong_table_worker<routed> worker{backend};
    /// Point-in-time recovery is a replica's own (task 27.7).
    std::shared_ptr<ddb::memory_backup_store> store = std::make_shared<ddb::memory_backup_store>();
    ddb::pitr_api<routed> pitr{backend, store};
    ddb::backup_api<routed> backups{backend, store};
    ddb::backup_worker<routed> backup_worker{backend, store};
    ddb::pitr_worker<routed> pitr_worker{backend, store, [] {
                                             ddb::pitr_worker_options o;
                                             o.latest_lag_ms = 0;
                                             return o;
                                         }()};

    api_region(std::string region, ddb::local_backend<>& strong)
        : name(std::move(region)), backend(local.backend, &strong) {
        auth.register_service(ddb::dynamodb_static_roles());
        ddb::front_end_config cfg;
        cfg.advertised_endpoint = "localhost:8000";
        cfg.region = name;
        cfg.on_internal_error = [](std::string_view op, std::string_view what) {
            BOOST_ERROR(std::string(op) + " answered InternalServerError: " + std::string(what));
        };
        fe = std::make_unique<ddb::front_end<az::static_key_authorizer>>(cfg, auth);
        tables.register_with(*fe);
        items.register_with(*fe);
        ttl.register_with(*fe);
        transactions.register_with(*fe);
        queries.register_with(*fe);
        streams.register_with(*fe);
        pitr.register_with(*fe);
        backups.register_with(*fe);
        tables.set_strong_regions({"us-east-1", "us-east-2", "us-west-2"});
        worker.set_region(name);
    }

    auto call(std::string_view op, const std::string& body) -> reply {
        auto req = signed_request(op, body);
        // Signed for this region.
        sv4::service_rules rules;
        rules.service = "dynamodb";
        rules.protocol = sv4::error_protocol::json;
        const sv4::credential_scope scope{"20261110", name, "dynamodb"};
        req.headers.back().second =
            "AWS4-HMAC-SHA256 Credential=AKIDADMIN/" + scope.text() +
            ", SignedHeaders=content-type;host;x-amz-date;x-amz-target, Signature=" +
            std::string(64, '0');
        const auto parsed = sv4::parse(
            sv4::request{req.method, req.path, req.query, req.headers, req.body}, rules, k_now);
        BOOST_REQUIRE(parsed.has_value());
        const auto sig = sv4::compute_signature(*parsed, sv4::derive_signing_key(k_secret, scope));
        req.headers.back().second.replace(req.headers.back().second.size() - 64, 64, sig);
        const auto r = fe->handle(req);
        return reply{r.status, json::parse(r.body).as_object()};
    }

    auto ok(std::string_view op, const std::string& body) -> json::object {
        auto r = call(op, body);
        BOOST_REQUIRE_MESSAGE(r.status == 200U, name + " " + std::string(op) +
                                                    " failed: " + r.type() + ": " + r.message());
        return r.body;
    }
};

/// Three regions sharing one strong host.
struct three_regions {
    /// Its item group starts at the strong keys, as the Raft strong host's
    /// does, so a stream key names the host that owns it.
    fixture strong_host{kythira::key_range{ddb::strong_key_start(), ddb::prefix_end("t/")}};
    api_region east1{"us-east-1", strong_host.backend};
    api_region east2{"us-east-2", strong_host.backend};
    api_region west2{"us-west-2", strong_host.backend};

    three_regions() { seed(strong_host.backend, ddb::k_strong_table_id_base); }

    auto run_workers() -> void {
        for (int i = 0; i < 2; ++i) {
            (void)east1.worker.pass();
            (void)east2.worker.pass();
            (void)west2.worker.pass();
        }
    }
};

const std::string k_music =
    R"({"TableName":"Music","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
    R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],"BillingMode":"PAY_PER_REQUEST"})";

const std::string k_make_strong =
    R"({"TableName":"Music","MultiRegionConsistency":"STRONG","ReplicaUpdates":[)"
    R"({"Create":{"RegionName":"us-east-2"}},{"Create":{"RegionName":"us-west-2"}}]})";

const std::string k_make_strong_with_witness =
    R"({"TableName":"Music","MultiRegionConsistency":"STRONG",)"
    R"("ReplicaUpdates":[{"Create":{"RegionName":"us-east-2"}}],)"
    R"("GlobalTableWitnessUpdates":[{"Create":{"RegionName":"us-west-2"}}]})";

auto statuses(const json::object& table) -> std::map<std::string, std::string> {
    std::map<std::string, std::string> out;
    for (const auto& r : table.at("Replicas").as_array()) {
        out[std::string(r.at("RegionName").as_string())] =
            std::string(r.at("ReplicaStatus").as_string());
    }
    return out;
}

auto describe(api_region& r, const std::string& table = "Music") -> json::object {
    return r.ok("DescribeTable", R"({"TableName":")" + table + R"("})").at("Table").as_object();
}

}  // namespace

BOOST_AUTO_TEST_CASE(an_empty_table_becomes_strong_in_three_regions) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    const auto made = w.east1.ok("UpdateTable", k_make_strong).at("TableDescription").as_object();
    BOOST_TEST(made.at("MultiRegionConsistency").as_string() == "STRONG");
    BOOST_TEST(made.at("GlobalTableVersion").as_string() == "2019.11.21");
    BOOST_TEST((statuses(made) == std::map<std::string, std::string>{{"us-east-2", "CREATING"},
                                                                     {"us-west-2", "CREATING"}}));

    w.run_workers();
    for (auto* r : {&w.east1, &w.east2, &w.west2}) {
        BOOST_TEST_INFO("region " << r->name);
        const auto t = describe(*r);
        BOOST_TEST(t.at("TableStatus").as_string() == "ACTIVE");
        BOOST_TEST(t.at("MultiRegionConsistency").as_string() == "STRONG");
        const auto s = statuses(t);
        BOOST_TEST(s.size() == 2U);
        BOOST_TEST(!s.contains(r->name));
        for (const auto& [region, status] : s) {
            BOOST_TEST(status == "ACTIVE");
        }
    }

    // One table, written in one region, read in the others.
    w.east2.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"a"},"v":{"N":"1"}}})");
    for (auto* r : {&w.east1, &w.west2}) {
        const auto got = r->ok(
            "GetItem", R"({"TableName":"Music","Key":{"pk":{"S":"a"}},"ConsistentRead":true})");
        BOOST_TEST(got.at("Item").as_object().at("v").as_object().at("N").as_string() == "1");
    }
    // A conditional write sees the latest version wherever it is made.
    const auto clash =
        w.west2.call("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"a"}},)"
                                R"x("ConditionExpression":"attribute_not_exists(pk)"})x");
    BOOST_TEST(clash.type() == "ConditionalCheckFailedException");
    // Nothing went to any region's own host.
    for (auto* r : {&w.east1, &w.east2, &w.west2}) {
        const auto d = r->local.backend.describe("Music");
        BOOST_TEST_REQUIRE(d.has_value());
        BOOST_TEST(ddb::is_strong_table(d->table_id));
        BOOST_TEST(count_in(r->local.backend, d->table_id) == 0U);
    }
}

BOOST_AUTO_TEST_CASE(a_witness_region_holds_no_table_and_is_listed) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    const auto made =
        w.east1.ok("UpdateTable", k_make_strong_with_witness).at("TableDescription").as_object();
    const auto& witnesses = made.at("GlobalTableWitnesses").as_array();
    BOOST_TEST_REQUIRE(witnesses.size() == 1U);
    BOOST_TEST(witnesses[0].at("RegionName").as_string() == "us-west-2");
    BOOST_TEST(witnesses[0].at("WitnessStatus").as_string() == "CREATING");

    w.run_workers();
    const auto t = describe(w.east1);
    BOOST_TEST(t.at("GlobalTableWitnesses").as_array()[0].at("WitnessStatus").as_string() ==
               "ACTIVE");
    BOOST_TEST((statuses(t) == std::map<std::string, std::string>{{"us-east-2", "ACTIVE"}}));
    BOOST_TEST(describe(w.east2).at("GlobalTableWitnesses").as_array().size() == 1U);
    BOOST_TEST(w.west2.call("DescribeTable", R"({"TableName":"Music"})").type() ==
               "ResourceNotFoundException");
}

BOOST_AUTO_TEST_CASE(making_a_table_strong_is_refused_as_aws_refuses_it) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    const auto refused = [&](const std::string& body) {
        const auto r = w.east1.call("UpdateTable", body);
        BOOST_TEST_INFO(body);
        BOOST_TEST(r.status == 400U);
        return r.message();
    };
    BOOST_TEST(refused(R"({"TableName":"Music","MultiRegionConsistency":"SOMETIMES",)"
                       R"("ReplicaUpdates":[{"Create":{"RegionName":"us-east-2"}}]})")
                   .find("enum value set: [EVENTUAL, STRONG]") != std::string::npos);
    // Two regions, or a region outside the strong host's.
    BOOST_TEST(refused(R"({"TableName":"Music","MultiRegionConsistency":"STRONG",)"
                       R"("ReplicaUpdates":[{"Create":{"RegionName":"us-east-2"}}]})")
                   .find("spans exactly three regions") != std::string::npos);
    BOOST_TEST(refused(R"({"TableName":"Music","MultiRegionConsistency":"STRONG",)"
                       R"("ReplicaUpdates":[{"Create":{"RegionName":"us-east-2"}},)"
                       R"({"Create":{"RegionName":"eu-west-1"}}]})")
                   .find("spans exactly three regions") != std::string::npos);
    BOOST_TEST(refused(R"({"TableName":"Music","MultiRegionConsistency":"STRONG",)"
                       R"("ReplicaUpdates":[{"Create":{"RegionName":"us-east-2"}}],)"
                       R"("GlobalTableWitnessUpdates":[{"Create":{"RegionName":"us-west-2"}},)"
                       R"({"Create":{"RegionName":"us-east-2"}}]})")
                   .find("Member must have length less than or equal to 1") != std::string::npos);
    BOOST_TEST(refused(R"({"TableName":"Music","MultiRegionConsistency":"STRONG",)"
                       R"("ReplicaUpdates":[{"Delete":{"RegionName":"us-east-2"}}]})")
                   .find("by creating its replicas") != std::string::npos);
    BOOST_TEST(refused(R"({"TableName":"Music",)"
                       R"("GlobalTableWitnessUpdates":[{"Create":{"RegionName":"us-west-2"}}]})")
                   .find("needs MultiRegionConsistency STRONG") != std::string::npos);

    // Not empty.
    w.east1.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"a"}}})");
    BOOST_TEST(refused(k_make_strong).find("must be empty") != std::string::npos);
    w.east1.ok("DeleteItem", R"({"TableName":"Music","Key":{"pk":{"S":"a"}}})");

    // TTL on.
    w.east1.ok("UpdateTimeToLive", R"({"TableName":"Music","TimeToLiveSpecification":)"
                                   R"({"Enabled":true,"AttributeName":"exp"}})");
    BOOST_TEST(refused(k_make_strong).find("Time to Live is not supported") != std::string::npos);

    // An LSI.
    w.east1.ok(
        "CreateTable",
        R"({"TableName":"Indexed","AttributeDefinitions":[)"
        R"({"AttributeName":"pk","AttributeType":"S"},)"
        R"({"AttributeName":"sk","AttributeType":"S"},)"
        R"({"AttributeName":"alt","AttributeType":"S"}],)"
        R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
        R"({"AttributeName":"sk","KeyType":"RANGE"}],)"
        R"("LocalSecondaryIndexes":[{"IndexName":"byAlt","KeySchema":[)"
        R"({"AttributeName":"pk","KeyType":"HASH"},{"AttributeName":"alt","KeyType":"RANGE"}],)"
        R"("Projection":{"ProjectionType":"KEYS_ONLY"}}],"BillingMode":"PAY_PER_REQUEST"})");
    BOOST_TEST(refused(R"({"TableName":"Indexed","MultiRegionConsistency":"STRONG",)"
                       R"("ReplicaUpdates":[{"Create":{"RegionName":"us-east-2"}},)"
                       R"({"Create":{"RegionName":"us-west-2"}}]})")
                   .find("Local secondary indexes are not supported") != std::string::npos);

    // Once strong, nothing can be added, and a single replica cannot go.
    w.east1.ok("CreateTable", R"({"TableName":"Other",)" + k_music.substr(21));
    w.east1.ok("UpdateTable", R"({"TableName":"Other",)" + k_make_strong.substr(21));
    w.run_workers();
    BOOST_TEST(refused(R"({"TableName":"Other","ReplicaUpdates":[)"
                       R"({"Delete":{"RegionName":"us-east-2"}}]})")
                   .find("A single replica or witness cannot be deleted") != std::string::npos);
    BOOST_TEST(refused(R"({"TableName":"Other","ReplicaUpdates":[)"
                       R"({"Create":{"RegionName":"eu-west-1"}}]})")
                   .find("cannot be added") != std::string::npos);
    BOOST_TEST(refused(R"({"TableName":"Other","MultiRegionConsistency":"STRONG"})")
                   .find("cannot be added") != std::string::npos);
    BOOST_TEST(refused(R"({"TableName":"Other","ReplicaUpdates":[)"
                       R"({"Delete":{"RegionName":"us-east-1"}},)"
                       R"({"Delete":{"RegionName":"us-east-2"}}]})")
                   .find("from the region that keeps the table") != std::string::npos);
    BOOST_TEST(w.east1.call("DeleteTable", R"({"TableName":"Other"})").status == 400U);
}

BOOST_AUTO_TEST_CASE(deleting_two_replicas_leaves_a_single_region_table) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    w.east1.ok("UpdateTable", k_make_strong);
    w.run_workers();
    w.east2.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"a"},"v":{"N":"1"}}})");

    // Asked in us-west-2, which keeps the table.
    const auto asked = w.west2
                           .ok("UpdateTable", R"({"TableName":"Music","ReplicaUpdates":[)"
                                              R"({"Delete":{"RegionName":"us-east-1"}},)"
                                              R"({"Delete":{"RegionName":"us-east-2"}}]})")
                           .at("TableDescription")
                           .as_object();
    BOOST_TEST((statuses(asked) == std::map<std::string, std::string>{{"us-east-1", "DELETING"},
                                                                      {"us-east-2", "DELETING"}}));
    w.run_workers();

    const auto t = describe(w.west2);
    BOOST_TEST(!t.contains("MultiRegionConsistency"));
    BOOST_TEST(!t.contains("Replicas"));
    BOOST_TEST(!t.contains("GlobalTableWitnesses"));
    // The item written in us-east-2 while the table was strong is still
    // here, and the table takes writes as any other.
    const auto got = w.west2.ok(
        "GetItem", R"({"TableName":"Music","Key":{"pk":{"S":"a"}},"ConsistentRead":true})");
    BOOST_TEST(got.at("Item").as_object().at("v").as_object().at("N").as_string() == "1");
    w.west2.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"b"}}})");
    // The removed regions forgot it.
    for (auto* r : {&w.east1, &w.east2}) {
        BOOST_TEST_INFO("region " << r->name);
        BOOST_TEST(r->call("DescribeTable", R"({"TableName":"Music"})").type() ==
                   "ResourceNotFoundException");
    }
    // Transactions and TTL work on it again.
    w.west2.ok("TransactGetItems",
               R"({"TransactItems":[{"Get":{"TableName":"Music","Key":{"pk":{"S":"a"}}}}]})");

    // Deleting it here deletes the strong catalog's entry too.
    w.west2.ok("DeleteTable", R"({"TableName":"Music"})");
    w.run_workers();
    const auto shared = w.strong_host.backend.describe("Music");
    BOOST_TEST((!shared || shared->status == ddb::table_status::deleting));
}

BOOST_AUTO_TEST_CASE(a_replica_and_the_witness_go_together) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    w.east1.ok("UpdateTable", k_make_strong_with_witness);
    w.run_workers();
    const auto refused = w.east1.call(
        "UpdateTable",
        R"({"TableName":"Music","GlobalTableWitnessUpdates":[{"Delete":{"RegionName":"us-west-2"}}]})");
    BOOST_TEST(refused.message().find("A single replica or witness") != std::string::npos);
    w.east1.ok("UpdateTable",
               R"({"TableName":"Music","ReplicaUpdates":[{"Delete":{"RegionName":"us-east-2"}}],)"
               R"("GlobalTableWitnessUpdates":[{"Delete":{"RegionName":"us-west-2"}}]})");
    w.run_workers();
    BOOST_TEST(!describe(w.east1).contains("MultiRegionConsistency"));
    BOOST_TEST(w.east2.call("DescribeTable", R"({"TableName":"Music"})").type() ==
               "ResourceNotFoundException");
}

// Task 27.7: a GSI made in one region is the strong catalog's, with a
// strong index id, and serves queries in every region.
BOOST_AUTO_TEST_CASE(a_strong_tables_gsis_are_shared_by_its_regions) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    w.east1.ok("UpdateTable", k_make_strong);
    w.run_workers();
    w.east2.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"a"},"artist":{"S":"Nina"}}})");
    const auto made = w.east2
                          .ok("UpdateTable",
                              R"({"TableName":"Music","AttributeDefinitions":[)"
                              R"({"AttributeName":"artist","AttributeType":"S"}],)"
                              R"("GlobalSecondaryIndexUpdates":[{"Create":{"IndexName":"byArtist",)"
                              R"("KeySchema":[{"AttributeName":"artist","KeyType":"HASH"}],)"
                              R"("Projection":{"ProjectionType":"ALL"}}}]})")
                          .at("TableDescription")
                          .as_object();
    BOOST_TEST(made.at("GlobalSecondaryIndexes").as_array().size() == 1U);
    const auto shared = w.strong_host.backend.describe("Music");
    BOOST_TEST_REQUIRE(shared.has_value());
    BOOST_TEST_REQUIRE(shared->gsis.size() == 1U);
    BOOST_TEST(ddb::is_strong_table(shared->gsis[0].index_id));
    w.run_workers();
    for (auto* r : {&w.east1, &w.east2, &w.west2}) {
        BOOST_TEST_INFO("region " << r->name);
        const auto d = r->local.backend.describe("Music");
        BOOST_TEST_REQUIRE(d.has_value());
        BOOST_TEST_REQUIRE(d->gsis.size() == 1U);
        BOOST_TEST(d->gsis[0].index_id == shared->gsis[0].index_id);
        BOOST_TEST((d->gsis[0].status == ddb::index_status::active));
        const auto q = r->ok("Query", R"({"TableName":"Music","IndexName":"byArtist",)"
                                      R"("KeyConditionExpression":"artist = :a",)"
                                      R"("ExpressionAttributeValues":{":a":{"S":"Nina"}}})");
        BOOST_TEST(q.at("Count").to_number<int>() == 1);
    }
    // Dropped in another region, it goes everywhere.
    w.west2.ok("UpdateTable", R"({"TableName":"Music","GlobalSecondaryIndexUpdates":[)"
                              R"({"Delete":{"IndexName":"byArtist"}}]})");
    w.run_workers();
    w.run_workers();
    for (auto* r : {&w.east1, &w.east2, &w.west2}) {
        BOOST_TEST_INFO("region " << r->name);
        BOOST_TEST(r->local.backend.describe("Music")->gsis.empty());
    }
}

// Task 27.7: AWS keeps the billing mode and capacity the same on every
// replica. A switch to provisioned in one region reaches the others.
BOOST_AUTO_TEST_CASE(a_strong_tables_billing_is_shared_by_its_regions) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    w.east1.ok("UpdateTable", k_make_strong);
    w.run_workers();
    w.east2.ok("UpdateTable",
               R"({"TableName":"Music","BillingMode":"PROVISIONED",)"
               R"("ProvisionedThroughput":{"ReadCapacityUnits":7,"WriteCapacityUnits":3}})");
    const auto shared = w.strong_host.backend.describe("Music");
    BOOST_TEST_REQUIRE(shared.has_value());
    BOOST_TEST((shared->billing == ddb::billing_mode::provisioned));
    w.run_workers();
    for (auto* r : {&w.east1, &w.east2, &w.west2}) {
        BOOST_TEST_INFO("region " << r->name);
        const auto d = r->local.backend.describe("Music");
        BOOST_TEST_REQUIRE(d.has_value());
        BOOST_TEST((d->billing == ddb::billing_mode::provisioned));
        BOOST_TEST(d->read_capacity == 7U);
        BOOST_TEST(d->write_capacity == 3U);
    }
}

// Task 27.7: AWS gives every replica's stream the same records. A stream
// enabled in one region shows the writes every region makes.
BOOST_AUTO_TEST_CASE(a_stream_on_one_replica_sees_every_regions_writes) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    w.east1.ok("UpdateTable", k_make_strong);
    w.run_workers();
    w.east2.ok("UpdateTable", R"({"TableName":"Music","StreamSpecification":)"
                              R"({"StreamEnabled":true,"StreamViewType":"NEW_IMAGE"}})");
    w.east1.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"from-east1"}}})");
    w.west2.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"from-west2"}}})");
    w.east2.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"from-east2"}}})");

    const auto arn = std::string(describe(w.east2).at("LatestStreamArn").as_string());
    const auto stream =
        w.east2.ok("DescribeStream", R"({"StreamArn":")" + arn + R"("})").at("StreamDescription");
    std::set<std::string> seen;
    for (const auto& shard : stream.at("Shards").as_array()) {
        auto it = w.east2
                      .ok("GetShardIterator", R"({"StreamArn":")" + arn + R"(","ShardId":")" +
                                                  std::string(shard.at("ShardId").as_string()) +
                                                  R"(","ShardIteratorType":"TRIM_HORIZON"})")
                      .at("ShardIterator");
        const auto got = w.east2.ok(
            "GetRecords", R"({"ShardIterator":")" + std::string(it.as_string()) + R"("})");
        for (const auto& rec : got.at("Records").as_array()) {
            seen.insert(std::string(rec.at("dynamodb").at("Keys").at("pk").at("S").as_string()));
        }
    }
    BOOST_TEST((seen == std::set<std::string>{"from-east1", "from-east2", "from-west2"}));
    // The other regions have no stream of their own.
    BOOST_TEST(!describe(w.east1).contains("LatestStreamArn"));
}

// Task 27.7: PITR is per replica, as on AWS, and a replica's archive holds
// every region's writes, so a restore there has them all.
BOOST_AUTO_TEST_CASE(pitr_on_one_replica_restores_every_regions_writes) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    w.east1.ok("UpdateTable", k_make_strong);
    w.run_workers();
    w.west2.ok("UpdateContinuousBackups",
               R"({"TableName":"Music","PointInTimeRecoverySpecification":)"
               R"({"PointInTimeRecoveryEnabled":true}})");
    w.east1.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"from-east1"}}})");
    w.east2.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"from-east2"}}})");
    w.west2.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"from-west2"}}})");
    (void)w.west2.pitr_worker.pass();
    (void)w.west2.pitr_worker.pass();

    w.west2.ok("RestoreTableToPointInTime",
               R"({"SourceTableName":"Music","TargetTableName":"Restored",)"
               R"("UseLatestRestorableTime":true})");
    (void)w.west2.pitr_worker.pass();
    const auto scan = w.west2.ok("Scan", R"({"TableName":"Restored","ConsistentRead":true})");
    std::set<std::string> keys;
    for (const auto& i : scan.at("Items").as_array()) {
        keys.insert(std::string(i.at("pk").at("S").as_string()));
    }
    BOOST_TEST((keys == std::set<std::string>{"from-east1", "from-east2", "from-west2"}));
    // The restored table is the region's own, and the other replicas have
    // PITR off.
    BOOST_TEST(!describe(w.west2, "Restored").contains("MultiRegionConsistency"));
    BOOST_TEST(w.east1.ok("DescribeContinuousBackups", R"({"TableName":"Music"})")
                   .at("ContinuousBackupsDescription")
                   .at("PointInTimeRecoveryDescription")
                   .at("PointInTimeRecoveryStatus")
                   .as_string() == "DISABLED");
}

// Task 27.7: an on-demand backup taken in one region holds every region's
// writes and restores as that region's own table.
BOOST_AUTO_TEST_CASE(a_backup_in_one_region_holds_every_regions_writes) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    w.east1.ok("UpdateTable", k_make_strong);
    w.run_workers();
    w.east1.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"from-east1"}}})");
    w.west2.ok("PutItem", R"({"TableName":"Music","Item":{"pk":{"S":"from-west2"}}})");
    const auto arn =
        std::string(w.east2.ok("CreateBackup", R"({"TableName":"Music","BackupName":"before"})")
                        .at("BackupDetails")
                        .at("BackupArn")
                        .as_string());
    (void)w.east2.backup_worker.pass();
    w.east2.ok("RestoreTableFromBackup",
               R"({"TargetTableName":"FromBackup","BackupArn":")" + arn + R"("})");
    (void)w.east2.backup_worker.pass();
    const auto scan = w.east2.ok("Scan", R"({"TableName":"FromBackup","ConsistentRead":true})");
    std::set<std::string> keys;
    for (const auto& i : scan.at("Items").as_array()) {
        keys.insert(std::string(i.at("pk").at("S").as_string()));
    }
    BOOST_TEST((keys == std::set<std::string>{"from-east1", "from-west2"}));
    BOOST_TEST(!describe(w.east2, "FromBackup").contains("MultiRegionConsistency"));
}

BOOST_AUTO_TEST_CASE(the_leave_commands_round_trip) {
    for (const auto op :
         {ddb::catalog_command::kind::leave_strong, ddb::catalog_command::kind::forget_strong}) {
        ddb::catalog_command c;
        c.op = op;
        c.table.name = "Music";
        c.table.table_id = ddb::k_strong_table_id_base + 3;
        c.table.region = "us-west-2";
        const auto bytes = ddb::encode_catalog_command(c);
        BOOST_TEST(static_cast<int>(static_cast<std::uint8_t>(bytes[0])) == 11);
        bool newer = false;
        const auto back = ddb::decode_catalog_command(bytes, newer);
        BOOST_TEST_REQUIRE(back.has_value());
        BOOST_TEST((back->op == op));
        BOOST_TEST(back->table.table_id == c.table.table_id);
    }
}

BOOST_AUTO_TEST_CASE(without_a_strong_host_strong_consistency_is_not_offered) {
    fixture f;
    f.ok("CreateTable", k_music);
    const auto r = f.call("UpdateTable", k_make_strong);
    BOOST_TEST(r.type() == "ValidationException");
    BOOST_TEST(r.message() == "Kythira does not support MultiRegionConsistency yet");
}

BOOST_AUTO_TEST_CASE(a_replica_region_with_the_name_taken_fails_its_replica) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    w.east2.ok("CreateTable", k_music);
    w.east1.ok("UpdateTable", k_make_strong);
    w.run_workers();
    const auto s = statuses(describe(w.east1));
    BOOST_TEST(s.at("us-east-2") == "CREATION_FAILED");
    BOOST_TEST(s.at("us-west-2") == "ACTIVE");
    // East 2 kept its own table.
    BOOST_TEST(!describe(w.east2).contains("MultiRegionConsistency"));
}

BOOST_AUTO_TEST_CASE(a_write_in_flight_during_the_switch_is_salvaged) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    const auto before = *w.east1.local.backend.describe("Music");
    w.east1.ok("UpdateTable", k_make_strong);
    const auto after = *w.east1.local.backend.describe("Music");
    BOOST_TEST(after.retired_ids.size() == 1U);

    // A request that read the old descriptor writes after the switch.
    BOOST_TEST((w.east1.local.backend.write(put_of(before, "late")).outcome ==
                ddb::item_result::status::ok));
    BOOST_TEST(count_in(w.east1.local.backend, before.table_id) == 1U);

    // Not before the delay.
    w.run_workers();
    BOOST_TEST(count_in(w.east1.local.backend, before.table_id) == 1U);
    w.east1.local.clock_ms += ddb::table_api<routed>::k_salvage_delay_ms;
    w.run_workers();
    BOOST_TEST(count_in(w.east1.local.backend, before.table_id) == 0U);
    BOOST_TEST(w.east2.ok("GetItem", R"({"TableName":"Music","Key":{"pk":{"S":"late"}}})")
                   .contains("Item"));
    BOOST_TEST(w.east1.local.backend.describe("Music")->retired_ids.empty());
}

// -- Requests on a strong table (task 27.5) -----------------------------------

BOOST_AUTO_TEST_CASE(transactions_and_ttl_are_refused_on_a_strong_table) {
    three_regions w;
    w.east1.ok("CreateTable", k_music);
    w.east1.ok("UpdateTable", k_make_strong);
    w.run_workers();
    for (auto* r : {&w.east1, &w.east2}) {
        const auto put =
            r->call("TransactWriteItems",
                    R"({"TransactItems":[{"Put":{"TableName":"Music","Item":{"pk":{"S":"t"}}}}]})");
        BOOST_TEST(put.type() == "ValidationException");
        BOOST_TEST(put.message().find("Transactions are not supported") != std::string::npos);
        const auto get =
            r->call("TransactGetItems",
                    R"({"TransactItems":[{"Get":{"TableName":"Music","Key":{"pk":{"S":"t"}}}}]})");
        BOOST_TEST(get.type() == "ValidationException");
        const auto ttl =
            r->call("UpdateTimeToLive", R"({"TableName":"Music","TimeToLiveSpecification":)"
                                        R"({"Enabled":true,"AttributeName":"exp"}})");
        BOOST_TEST(ttl.type() == "ValidationException");
        BOOST_TEST(ttl.message().find("Time to Live is not supported") != std::string::npos);
    }
    // Nothing was written.
    BOOST_TEST(
        !w.east2.ok("GetItem", R"({"TableName":"Music","Key":{"pk":{"S":"t"}}})").contains("Item"));
    // Another table's transactions still work.
    w.east1.ok("CreateTable", R"({"TableName":"Plain",)" + k_music.substr(21));
    w.east1.ok("TransactWriteItems",
               R"({"TransactItems":[{"Put":{"TableName":"Plain","Item":{"pk":{"S":"t"}}}}]})");
}
