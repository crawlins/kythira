// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Query, Scan and the batch operations at the limits the recorded corpus
// does not reach cheaply (`.kiro/specs/dynamodb-compatible-api/` task 8,
// Requirements 9.1, 9.2, 10.1): the 1 MB page, segments that cover the
// table exactly once, a cursor taken across segments of any count, the
// 16 MB BatchGetItem response, ConsumedCapacity, the total-count limits
// across tables, and the disk guard on BatchWriteItem.

#define BOOST_TEST_MODULE dynamodb_query_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <boost/json.hpp>

#include <cstddef>
#include <set>
#include <string>

using namespace dynamodb_test;

namespace {

auto put_big(fixture& f, const std::string& table, const std::string& pk, int sk, std::size_t bytes)
    -> void {
    f.ok("PutItem", R"({"TableName":")" + table + R"(","Item":{"pk":{"S":")" + pk +
                        R"("},"sk":{"N":")" + std::to_string(sk) + R"("},"blob":{"S":")" +
                        std::string(bytes, 'x') + R"("}}})");
}

auto create(fixture& f, const std::string& name) -> void {
    f.ok("CreateTable", R"({"TableName":")" + name +
                            R"(","BillingMode":"PAY_PER_REQUEST",)"
                            R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
                            R"({"AttributeName":"sk","AttributeType":"N"}],)"
                            R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
                            R"({"AttributeName":"sk","KeyType":"RANGE"}]})");
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_page_stops_before_one_megabyte) {
    fixture f;
    f.without_throttling();
    create(f, "big");
    for (int i = 0; i < 5; ++i) {
        put_big(f, "big", "p", i, 300 * 1024);
    }
    const std::string q = R"({"TableName":"big","KeyConditionExpression":"pk = :p",)"
                          R"("ExpressionAttributeValues":{":p":{"S":"p"}},"Select":"COUNT"})";
    auto r = f.ok("Query", q);
    // Three items are 900 KB; a fourth would pass 1 MB.
    BOOST_TEST(r.at("ScannedCount").to_number<int>() == 3);
    BOOST_TEST(r.at("LastEvaluatedKey").at("sk").at("N").as_string() == "2");
    auto next = json::parse(q).as_object();
    next["ExclusiveStartKey"] = r.at("LastEvaluatedKey");
    r = f.ok("Query", json::serialize(next));
    BOOST_TEST(r.at("ScannedCount").to_number<int>() == 2);
    BOOST_TEST(!r.contains("LastEvaluatedKey"));

    // Scan pages the same way.
    r = f.ok("Scan", R"({"TableName":"big","Select":"COUNT"})");
    BOOST_TEST(r.at("ScannedCount").to_number<int>() == 3);
    BOOST_TEST(r.contains("LastEvaluatedKey"));

    // One item over the limit still comes back alone.
    create(f, "huge");
    put_big(f, "huge", "p", 0, 399 * 1024);
    put_big(f, "huge", "p", 1, 399 * 1024);
    put_big(f, "huge", "p", 2, 399 * 1024);
    r = f.ok("Scan", R"({"TableName":"huge","Select":"COUNT"})");
    BOOST_TEST(r.at("ScannedCount").to_number<int>() == 2);
}

BOOST_AUTO_TEST_CASE(segments_cover_the_table_exactly_once) {
    fixture f;
    create(f, "seg");
    for (int p = 0; p < 200; ++p) {
        f.ok("PutItem", R"({"TableName":"seg","Item":{"pk":{"S":"p)" + std::to_string(p) +
                            R"("},"sk":{"N":"1"}}})");
    }
    for (const int total : {1, 2, 3, 7, 64, 65536, 100000}) {
        std::multiset<std::string> seen;
        // Walk every segment of the small counts; for the large ones check
        // that the last segment answers.
        const int walk = total <= 64 ? total : 0;
        for (int s = 0; s < walk; ++s) {
            json::object in{
                {"TableName", "seg"}, {"Segment", s}, {"TotalSegments", total}, {"Limit", 9}};
            for (;;) {
                auto r = f.ok("Scan", json::serialize(in));
                for (const auto& it : r.at("Items").as_array()) {
                    seen.insert(std::string(it.at("pk").at("S").as_string()));
                }
                if (!r.contains("LastEvaluatedKey")) {
                    break;
                }
                // A cursor from this segment is accepted only by it.
                if (total > 1) {
                    auto other = in;
                    other["Segment"] = (s + 1) % total;
                    other["ExclusiveStartKey"] = r.at("LastEvaluatedKey");
                    BOOST_TEST(f.call("Scan", json::serialize(other)).message() ==
                               "Exclusive start key must lie within the segment");
                }
                in["ExclusiveStartKey"] = r.at("LastEvaluatedKey");
            }
        }
        if (walk > 0) {
            BOOST_TEST_INFO("TotalSegments " << total);
            BOOST_TEST(seen.size() == 200U);
            BOOST_TEST(std::set<std::string>(seen.begin(), seen.end()).size() == 200U);
        } else {
            // The last segment of a large count still answers.
            auto r = f.ok("Scan", R"({"TableName":"seg","Segment":)" + std::to_string(total - 1) +
                                      R"(,"TotalSegments":)" + std::to_string(total) + "}");
            BOOST_TEST(r.at("Count").to_number<int>() <= 200);
        }
    }
}

BOOST_AUTO_TEST_CASE(batch_get_leaves_keys_past_sixteen_megabytes_unprocessed) {
    fixture f;
    f.without_throttling();
    create(f, "bgt");
    std::string keys;
    for (int i = 0; i < 50; ++i) {
        put_big(f, "bgt", "p", i, 390 * 1024);
        keys += std::string(keys.empty() ? "" : ",") + R"({"pk":{"S":"p"},"sk":{"N":")" +
                std::to_string(i) + R"("}})";
    }
    auto r =
        f.ok("BatchGetItem", R"({"RequestItems":{"bgt":{"Keys":[)" + keys +
                                 R"(],"ConsistentRead":true}},"ReturnConsumedCapacity":"TOTAL"})");
    const auto got = r.at("Responses").at("bgt").as_array().size();
    const auto& left = r.at("UnprocessedKeys").at("bgt").as_object();
    // 42 x 390 KB fit in 16 MB and a 43rd would pass it, so it comes back
    // unprocessed (DynamoDB Local answers the same).
    BOOST_TEST(got == 42U);
    BOOST_TEST(left.at("Keys").as_array().size() == 50U - got);
    BOOST_TEST(left.at("ConsistentRead").as_bool());
    BOOST_TEST(r.at("ConsumedCapacity").at(0).at("CapacityUnits").as_double() ==
               static_cast<double>(got) * 98.0);
}

BOOST_AUTO_TEST_CASE(totals_across_tables_and_capacity) {
    fixture f;
    create(f, "tb1");
    create(f, "tb2");
    std::string w1;
    std::string w2;
    std::string k1;
    std::string k2;
    for (int i = 0; i < 13; ++i) {
        const auto item = R"({"pk":{"S":"a"},"sk":{"N":")" + std::to_string(i) + R"("}})";
        w1 += std::string(w1.empty() ? "" : ",") + R"({"PutRequest":{"Item":)" + item + "}}";
        w2 += std::string(w2.empty() ? "" : ",") + R"({"PutRequest":{"Item":)" + item + "}}";
    }
    auto r =
        f.call("BatchWriteItem", R"({"RequestItems":{"tb1":[)" + w1 + R"(],"tb2":[)" + w2 + "]}}");
    BOOST_TEST(r.message() == "Too many items requested for the BatchWriteItem call");
    r = f.call("BatchWriteItem",
               R"({"RequestItems":{"tb1":[)" + w1 + R"(]},"ReturnConsumedCapacity":"TOTAL"})");
    BOOST_TEST(r.status == 200U);
    BOOST_TEST(r.body.at("ConsumedCapacity").at(0).at("CapacityUnits").as_double() == 13.0);
    for (int i = 0; i < 60; ++i) {
        const auto key = R"({"pk":{"S":"a"},"sk":{"N":")" + std::to_string(i) + R"("}})";
        k1 += std::string(k1.empty() ? "" : ",") + key;
        k2 += std::string(k2.empty() ? "" : ",") + key;
    }
    r = f.call("BatchGetItem",
               R"({"RequestItems":{"tb1":{"Keys":[)" + k1 + R"(]},"tb2":{"Keys":[)" + k2 + "]}}}");
    BOOST_TEST(r.message() == "Too many items requested for the BatchGetItem call");

    // Query capacity: one 4 KB unit, halved when eventually consistent.
    auto q = f.ok("Query", R"({"TableName":"tb1","KeyConditionExpression":"pk = :p",)"
                           R"("ExpressionAttributeValues":{":p":{"S":"a"}},)"
                           R"("ReturnConsumedCapacity":"INDEXES"})");
    BOOST_TEST(q.at("Count").to_number<int>() == 13);
    BOOST_TEST(q.at("ConsumedCapacity").at("CapacityUnits").as_double() == 0.5);
    BOOST_TEST(q.at("ConsumedCapacity").at("Table").at("CapacityUnits").as_double() == 0.5);
}

BOOST_AUTO_TEST_CASE(batch_write_asks_the_disk_guard_and_reads_do_not) {
    fixture f;
    create(f, "dgt");
    f.free_bytes = 10;
    auto r = f.call(
        "BatchWriteItem",
        R"({"RequestItems":{"dgt":[{"PutRequest":{"Item":{"pk":{"S":"a"},"sk":{"N":"1"}}}}]}})");
    BOOST_TEST(r.status == 500U);
    BOOST_TEST(r.type() == "InternalServerError");
    BOOST_TEST(f.ok("Scan", R"({"TableName":"dgt"})").at("Count").to_number<int>() == 0);
    BOOST_TEST(f.ok("BatchGetItem",
                    R"({"RequestItems":{"dgt":{"Keys":[{"pk":{"S":"a"},"sk":{"N":"1"}}]}}})")
                   .at("Responses")
                   .at("dgt")
                   .as_array()
                   .empty());
}

BOOST_AUTO_TEST_CASE(a_missing_table_answers_with_the_data_plane_wording) {
    fixture f;
    for (const auto& [op, body] :
         {std::pair{"Query", R"({"TableName":"gone","KeyConditionExpression":"pk = :p",)"
                             R"("ExpressionAttributeValues":{":p":{"S":"a"}}})"},
          std::pair{"Scan", R"({"TableName":"gone"})"}}) {
        const auto r = f.call(op, body);
        BOOST_TEST(r.type() == "ResourceNotFoundException");
        BOOST_TEST(r.message() == "Requested resource not found");
    }
}
