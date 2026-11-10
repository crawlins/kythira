// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Local secondary indexes where the recorded corpus cannot reach them
// (`.kiro/specs/dynamodb-compatible-api/` task 9, Requirements 11.1, 11.5):
// the entries a write leaves in the engine, the item collection limit and
// ItemCollectionMetrics (which DynamoDB Local never returns), the split
// veto around an item collection, a table drop that clears entries and
// counters, and the stored forms.

#define BOOST_TEST_MODULE dynamodb_lsi_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/local_index.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

using namespace dynamodb_test;

namespace {

constexpr const char* k_create =
    R"({"TableName":"lsit","BillingMode":"PAY_PER_REQUEST",)"
    R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
    R"({"AttributeName":"sk","AttributeType":"N"},{"AttributeName":"ls","AttributeType":"S"}],)"
    R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},{"AttributeName":"sk","KeyType":"RANGE"}],)"
    R"("LocalSecondaryIndexes":[)"
    R"({"IndexName":"keys","KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
    R"({"AttributeName":"ls","KeyType":"RANGE"}],"Projection":{"ProjectionType":"KEYS_ONLY"}},)"
    R"({"IndexName":"some","KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
    R"({"AttributeName":"ls","KeyType":"RANGE"}],)"
    R"("Projection":{"ProjectionType":"INCLUDE","NonKeyAttributes":["a"]}}]})";

auto put(fixture& f, const std::string& pk, int sk, const std::string& rest = "") -> json::object {
    return f.ok("PutItem", R"({"TableName":"lsit","Item":{"pk":{"S":")" + pk + R"("},"sk":{"N":")" +
                               std::to_string(sk) + "\"}" + rest + "}}");
}

auto data_keys(fixture& f) -> std::size_t {
    return f.engine.key_count(kythira::column_family::data);
}

auto s(const std::string& v) -> ddb::attribute_value {
    return ddb::attribute_value::string(v);
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_write_keeps_one_entry_per_index_and_moves_it) {
    fixture f;
    f.ok("CreateTable", k_create);
    const auto before = data_keys(f);
    put(f, "p", 1, R"(,"ls":{"S":"m"},"a":{"N":"1"},"b":{"N":"2"})");
    // The item and an entry in each index.
    BOOST_TEST(data_keys(f) == before + 3U);
    // Changing the index key moves the entries; removing it drops them.
    f.ok("UpdateItem",
         R"({"TableName":"lsit","Key":{"pk":{"S":"p"},"sk":{"N":"1"}},)"
         R"("UpdateExpression":"SET ls = :v","ExpressionAttributeValues":{":v":{"S":"n"}}})");
    BOOST_TEST(data_keys(f) == before + 3U);
    auto r = f.ok(
        "Query",
        R"({"TableName":"lsit","IndexName":"some","KeyConditionExpression":"pk = :p AND ls = :l",)"
        R"("ExpressionAttributeValues":{":p":{"S":"p"},":l":{"S":"n"}}})");
    BOOST_TEST(r.at("Count").to_number<int>() == 1);
    BOOST_TEST(json::serialize(r.at("Items").at(0)) ==
               R"({"a":{"N":"1"},"ls":{"S":"n"},"pk":{"S":"p"},"sk":{"N":"1"}})");
    r = f.ok(
        "Query",
        R"({"TableName":"lsit","IndexName":"some","KeyConditionExpression":"pk = :p AND ls = :l",)"
        R"("ExpressionAttributeValues":{":p":{"S":"p"},":l":{"S":"m"}}})");
    BOOST_TEST(r.at("Count").to_number<int>() == 0);
    f.ok("UpdateItem", R"({"TableName":"lsit","Key":{"pk":{"S":"p"},"sk":{"N":"1"}},)"
                       R"("UpdateExpression":"REMOVE ls"})");
    BOOST_TEST(data_keys(f) == before + 1U);
    f.ok("DeleteItem", R"({"TableName":"lsit","Key":{"pk":{"S":"p"},"sk":{"N":"1"}}})");
    BOOST_TEST(data_keys(f) == before);
    // The collection's counter goes with its last key.
    const auto& sm = f.backend.items();
    const auto d = f.backend.describe("lsit");
    BOOST_TEST(sm.collection_size(d->table_id, s("p")) == 0U);
}

BOOST_AUTO_TEST_CASE(item_collection_metrics_and_the_size_limit) {
    fixture f;
    f.ok("CreateTable", k_create);
    const auto d = f.backend.describe("lsit");
    auto r = f.ok("PutItem",
                  R"({"TableName":"lsit","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"ls":{"S":"x"}},)"
                  R"("ReturnItemCollectionMetrics":"SIZE"})");
    BOOST_TEST(json::serialize(r.at("ItemCollectionMetrics")) ==
               R"({"ItemCollectionKey":{"pk":{"S":"p"}},"SizeEstimateRangeGB":[0E0,1E0]})");
    // The counted size: the item plus an entry per index with AWS's
    // 100-byte overhead.
    const auto one = *ddb::item_from_json(
        json::parse(R"({"pk":{"S":"p"},"sk":{"N":"1"},"ls":{"S":"x"}})").as_object());
    const auto size = f.backend.items().collection_size(d->table_id, s("p"));
    BOOST_TEST(size == 3U * ddb::item_size(one) + 200U);

    // A table without LSIs never reports metrics.
    f.ok("CreateTable", R"({"TableName":"plain","BillingMode":"PAY_PER_REQUEST",)"
                        R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
                        R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}]})");
    r = f.ok(
        "PutItem",
        R"({"TableName":"plain","Item":{"pk":{"S":"p"}},"ReturnItemCollectionMetrics":"SIZE"})");
    BOOST_TEST(!r.contains("ItemCollectionMetrics"));

    // Past the limit a growing write fails and changes nothing; a write
    // that shrinks the collection still applies, and other collections are
    // unaffected.
    f.backend.set_collection_limit(size + 50);
    auto e = f.call(
        "PutItem", R"({"TableName":"lsit","Item":{"pk":{"S":"p"},"sk":{"N":"2"},"ls":{"S":"y"}}})");
    BOOST_TEST(e.status == 400U);
    BOOST_TEST(e.type() == "ItemCollectionSizeLimitExceededException");
    BOOST_TEST(e.message() == "Item collection size limit exceeded");
    BOOST_TEST(f.backend.items().collection_size(d->table_id, s("p")) == size);
    put(f, "q", 1, R"(,"ls":{"S":"y"})");
    e = f.call("BatchWriteItem",
               R"({"RequestItems":{"lsit":[{"PutRequest":{"Item":{"pk":{"S":"p"},"sk":{"N":"3"},)"
               R"("big":{"S":")" +
                   std::string(100, 'z') + R"("}}}}]}})");
    BOOST_TEST(e.type() == "ItemCollectionSizeLimitExceededException");
    r = f.ok("UpdateItem",
             R"({"TableName":"lsit","Key":{"pk":{"S":"p"},"sk":{"N":"1"}},)"
             R"("UpdateExpression":"REMOVE ls","ReturnItemCollectionMetrics":"SIZE"})");
    BOOST_TEST(r.contains("ItemCollectionMetrics"));
    const auto bare =
        *ddb::item_from_json(json::parse(R"({"pk":{"S":"p"},"sk":{"N":"1"}})").as_object());
    BOOST_TEST(f.backend.items().collection_size(d->table_id, s("p")) == ddb::item_size(bare));

    // BatchWriteItem lists each collection it wrote once.
    f.backend.set_collection_limit(ddb::k_max_collection_bytes);
    r = f.ok("BatchWriteItem",
             R"({"RequestItems":{"lsit":[{"PutRequest":{"Item":{"pk":{"S":"p"},"sk":{"N":"4"}}}},)"
             R"({"PutRequest":{"Item":{"pk":{"S":"p"},"sk":{"N":"5"}}}},)"
             R"({"DeleteRequest":{"Key":{"pk":{"S":"q"},"sk":{"N":"1"}}}}]},)"
             R"("ReturnItemCollectionMetrics":"SIZE"})");
    BOOST_TEST(r.at("ItemCollectionMetrics").at("lsit").as_array().size() == 2U);
    r = f.ok(
        "BatchWriteItem",
        R"({"RequestItems":{"lsit":[{"PutRequest":{"Item":{"pk":{"S":"p"},"sk":{"N":"6"}}}}]}})");
    BOOST_TEST(!r.contains("ItemCollectionMetrics"));
}

BOOST_AUTO_TEST_CASE(index_keys_are_checked_before_a_batch_writes) {
    fixture f;
    f.ok("CreateTable", k_create);
    const auto e = f.call(
        "BatchWriteItem",
        R"({"RequestItems":{"lsit":[{"PutRequest":{"Item":{"pk":{"S":"p"},"sk":{"N":"1"}}}},)"
        R"({"PutRequest":{"Item":{"pk":{"S":"p"},"sk":{"N":"2"},"ls":{"N":"1"}}}}]}})");
    BOOST_TEST(e.message() ==
               "One or more parameter values were invalid: Type mismatch for Index Key");
    BOOST_TEST(f.ok("Scan", R"({"TableName":"lsit"})").at("Count").to_number<int>() == 0);
    const auto b = f.call(
        "PutItem", R"({"TableName":"lsit","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"ls":{"S":""}}})");
    BOOST_TEST(b.message().ends_with("IndexName: keys, IndexKey: ls"));
}

BOOST_AUTO_TEST_CASE(an_index_query_pages_by_its_own_key_across_ties) {
    fixture f;
    f.ok("CreateTable", k_create);
    // Five items share one index key: the entries are ordered by the
    // table's sort key, and a cursor (which carries both) resumes exactly.
    for (int i = 0; i < 5; ++i) {
        put(f, "p", i, R"(,"ls":{"S":"same"})");
    }
    json::object in{{"TableName", "lsit"},
                    {"IndexName", "keys"},
                    {"KeyConditionExpression", "pk = :p"},
                    {"ExpressionAttributeValues", json::object{{":p", json::object{{"S", "p"}}}}},
                    {"Limit", 2}};
    std::vector<std::string> seen;
    for (;;) {
        auto r = f.ok("Query", json::serialize(in));
        for (const auto& it : r.at("Items").as_array()) {
            seen.emplace_back(it.at("sk").at("N").as_string());
        }
        if (!r.contains("LastEvaluatedKey")) {
            break;
        }
        BOOST_TEST(r.at("LastEvaluatedKey").as_object().size() == 3U);
        in["ExclusiveStartKey"] = r.at("LastEvaluatedKey");
    }
    BOOST_TEST((seen == std::vector<std::string>{"0", "1", "2", "3", "4"}));
}

BOOST_AUTO_TEST_CASE(dropping_a_table_clears_entries_and_counters) {
    fixture f;
    const auto empty_data = data_keys(f);
    f.ok("CreateTable", k_create);
    for (int i = 0; i < 30; ++i) {
        put(f, "p" + std::to_string(i % 4), i, R"(,"ls":{"S":"v)" + std::to_string(i) + R"("})");
    }
    const auto d = f.backend.describe("lsit");
    f.ok("DeleteTable", R"({"TableName":"lsit"})");
    BOOST_TEST(!f.backend.describe("lsit").has_value());
    std::size_t left = 0;
    for (const auto cf : {kythira::column_family::data, kythira::column_family::meta}) {
        f.engine.scan(cf, ddb::table_range(d->table_id), kythira::scan_direction::forward,
                      [&](std::string_view, std::string_view) {
                          ++left;
                          return true;
                      });
    }
    BOOST_TEST(left == 0U);
    BOOST_TEST(data_keys(f) >= empty_data);
}

BOOST_AUTO_TEST_CASE(entries_stay_with_their_collection_through_a_split) {
    // Every key of a collection, entries and counter included, shares its
    // partition prefix, and nothing inside one is a split point.
    const std::uint64_t id = 9;
    ddb::key_schema table;
    table.partition_name = "pk";
    table.sort_name = "sk";
    table.sort_type = ddb::attribute_type::N;
    ddb::local_index lsi;
    lsi.name = "idx";
    lsi.number = 1;
    lsi.sort_name = "ls";
    ddb::item it{{"ls", s("v")}, {"pk", s("c")}, {"sk", ddb::attribute_value::number({})}};
    const auto prefix = ddb::partition_prefix(id, s("c"));
    const auto entry = *ddb::index_entry_key(id, table, lsi, it);
    const auto counter = ddb::collection_size_key(id, s("c"));
    const auto base = *ddb::base_item_key(id, table, it);
    for (const auto& k : {entry, counter, base}) {
        BOOST_TEST(k.starts_with(prefix));
        BOOST_TEST(!ddb::is_partition_boundary(k));
    }
    // Base items sort before entries, and entries before the counter.
    // (Parenthesized: Boost.Test would compare the keys as C strings.)
    BOOST_TEST((base < entry));
    BOOST_TEST((entry < counter));
    const auto cls = ddb::classify_item_key(entry, ddb::attribute_type::S);
    BOOST_REQUIRE(cls.has_value());
    BOOST_TEST((cls->what == ddb::item_key_class::kind::index));
    BOOST_TEST(cls->number == 1U);
    BOOST_TEST((ddb::classify_item_key(base, ddb::attribute_type::S)->what ==
                ddb::item_key_class::kind::base));
    BOOST_TEST(!ddb::classify_item_key(counter, ddb::attribute_type::S).has_value());
    BOOST_TEST(!ddb::classify_item_key(prefix, ddb::attribute_type::S).has_value());

    // A group cut at the collection's prefix takes all of them along.
    fixture f;
    f.ok("CreateTable", k_create);
    put(f, "c", 1, R"(,"ls":{"S":"v"},"a":{"N":"1"})");
    const auto d = f.backend.describe("lsit");
    kythira::memory_kv_engine& engine = f.engine;
    std::size_t in_collection = 0;
    for (const auto cf : {kythira::column_family::data, kythira::column_family::meta}) {
        engine.scan(cf, ddb::prefix_range(ddb::partition_prefix(d->table_id, s("c"))),
                    kythira::scan_direction::forward, [&](std::string_view, std::string_view) {
                        ++in_collection;
                        return true;
                    });
    }
    // The item, two entries, the item's version and the counter.
    BOOST_TEST(in_collection == 5U);
}

BOOST_AUTO_TEST_CASE(stored_forms_round_trip_with_indexes) {
    ddb::local_index a;
    a.name = "first";
    a.number = 0;
    a.sort_name = "x";
    a.sort_type = ddb::attribute_type::N;
    a.projection = ddb::projection_kind::include;
    a.non_key = {"q", "r"};
    ddb::local_index b;
    b.name = "second";
    b.number = 1;
    b.sort_name = "y";
    b.sort_type = ddb::attribute_type::B;
    b.projection = ddb::projection_kind::keys_only;

    ddb::item_command c;
    c.table_id = 4;
    c.schema.partition_name = "pk";
    c.schema.sort_name = "sk";
    c.payload = ddb::item{{"pk", s("a")}, {"sk", s("b")}};
    c.lsis = {a, b};
    ddb::item_command back;
    BOOST_TEST((ddb::decode_item_command(ddb::encode_item_command(c), back) ==
                ddb::item_command_status::ok));
    BOOST_TEST((back == c));

    ddb::item_result r;
    r.collection_bytes = 123456789;
    BOOST_TEST((ddb::decode_item_result(ddb::encode_item_result(r)) == r));
    r.collection_bytes.reset();
    BOOST_TEST((ddb::decode_item_result(ddb::encode_item_result(r)) == r));

    ddb::table_descriptor d;
    d.name = "t";
    d.table_id = 4;
    d.schema = c.schema;
    d.lsis = {a, b};
    d.tags = {{"k", "v"}};
    BOOST_TEST((ddb::decode_table_descriptor(ddb::encode_table_descriptor(d)) == d));

    // More than five indexes is a corrupt form.
    d.lsis.assign(6, a);
    BOOST_TEST(!ddb::decode_table_descriptor(ddb::encode_table_descriptor(d)).has_value());
}
