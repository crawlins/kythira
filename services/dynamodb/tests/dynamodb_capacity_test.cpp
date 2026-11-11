// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Capacity, throttling and consumed capacity
// (`.kiro/specs/dynamodb-compatible-api/` task 12, Requirements 3.5 and
// 19.1-19.2): the token buckets on their own, then the handlers over
// local_backend with the fixture's fixed clock, throttling on by default and
// turned off per cluster or per table.

#define BOOST_TEST_MODULE dynamodb_capacity_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/capacity.hpp>
#include <kythira/dynamodb/compat_profile.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

using namespace dynamodb_test;

namespace {

constexpr std::uint64_t k_t0 = 1794300000000ULL;
constexpr std::string_view k_arn = "arn:aws:dynamodb:us-east-1:111122223333:table/";

auto provisioned(std::uint64_t rcu, std::uint64_t wcu) -> ddb::table_descriptor {
    ddb::table_descriptor d;
    d.name = "t";
    d.table_id = 7;
    d.billing = ddb::billing_mode::provisioned;
    d.read_capacity = rcu;
    d.write_capacity = wcu;
    return d;
}

auto on_demand() -> ddb::table_descriptor {
    auto d = provisioned(0, 0);
    d.billing = ddb::billing_mode::pay_per_request;
    return d;
}

const ddb::capacity_target k_table_write{"", ddb::capacity_kind::write, ""};
const ddb::capacity_target k_table_read{"", ddb::capacity_kind::read, ""};

/// A string of `n` bytes, so an item's size (and units) is under control.
auto blob(std::size_t n) -> std::string {
    return std::string(n, 'x');
}

/// The test tables: provisioned at 1 unit each way (300 units of burst), or
/// on demand.
struct capacity_fixture : fixture {
    void create_small(const std::string& name, const std::string& extra = "") {
        ok("CreateTable",
           R"({"TableName":")" + name +
               R"(","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
               R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],)"
               R"("ProvisionedThroughput":{"ReadCapacityUnits":1,"WriteCapacityUnits":1})" +
               extra + "}");
    }
    void create_on_demand(const std::string& name) {
        ok("CreateTable",
           R"({"TableName":")" + name +
               R"(","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
               R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],)"
               R"("BillingMode":"PAY_PER_REQUEST"})");
    }
    /// A put of an item of about `kb` KB under `pk`.
    auto put(const std::string& table, const std::string& pk, std::size_t kb,
             const std::string& more = "") -> reply {
        return call("PutItem", R"({"TableName":")" + table + R"(","Item":{"pk":{"S":")" + pk +
                                   R"("},"b":{"S":")" + blob(kb * 1024 - 64) + "\"}" + more + "}}");
    }
    auto get(const std::string& table, const std::string& pk) -> reply {
        return call("GetItem", R"({"TableName":")" + table + R"(","Key":{"pk":{"S":")" + pk +
                                   R"("}},"ConsistentRead":true})");
    }
    static void require_throttle(const reply& r, const std::string& exception,
                                 const std::string& reason, const std::string& resource) {
        BOOST_TEST_REQUIRE(r.status == 400U);
        BOOST_TEST(r.type() == exception);
        const char* member =
            exception == "ThrottlingException" ? "throttlingReasons" : "ThrottlingReasons";
        BOOST_TEST_REQUIRE(r.body.contains(member));
        const auto& reasons = r.body.at(member).as_array();
        BOOST_TEST_REQUIRE(reasons.size() == 1U);
        BOOST_TEST(reasons[0].at("reason").as_string() == reason);
        BOOST_TEST(reasons[0].at("resource").as_string() == resource);
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(a_bucket_refills_at_its_rate_and_admits_one_request_into_debt) {
    ddb::token_bucket b(10.0, 20.0, k_t0);
    BOOST_TEST(b.admits());
    b.charge(25.0);
    BOOST_TEST(!b.admits());
    b.refill(k_t0 + 400);  // 4 units back: still in debt
    BOOST_TEST(!b.admits());
    b.refill(k_t0 + 1000);  // 10 units back in all
    BOOST_TEST(b.admits());
    BOOST_TEST(b.tokens() == 5.0);
    b.refill(k_t0 + 100000);
    BOOST_TEST(b.tokens() == 20.0);  // never above its size
}

BOOST_AUTO_TEST_CASE(provisioned_capacity_keeps_five_minutes_of_burst) {
    ddb::capacity_throttler th;
    const auto t = provisioned(1, 1);
    for (int i = 0; i < 300; ++i) {
        BOOST_TEST_REQUIRE(!th.admit(t, k_table_write, k_t0).has_value());
        th.charge(t, k_table_write, 1.0, k_t0);
    }
    const auto refused = th.admit(t, k_table_write, k_t0);
    BOOST_TEST_REQUIRE(refused.has_value());
    BOOST_TEST(refused->exception == "ProvisionedThroughputExceededException");
    BOOST_TEST(refused->reason == "TableWriteProvisionedThroughputExceeded");
    BOOST_TEST(refused->index.empty());
    // Reads have their own bucket.
    BOOST_TEST(!th.admit(t, k_table_read, k_t0).has_value());
    // One second refills one unit.
    BOOST_TEST(!th.admit(t, k_table_write, k_t0 + 1000).has_value());
}

BOOST_AUTO_TEST_CASE(an_on_demand_table_is_held_to_the_account_quota_with_throttling_exception) {
    ddb::capacity_throttler th;
    const auto t = on_demand();
    th.charge(t, k_table_read, ddb::k_on_demand_table_units, k_t0);
    const auto refused = th.admit(t, k_table_read, k_t0);
    BOOST_TEST_REQUIRE(refused.has_value());
    BOOST_TEST(refused->exception == "ThrottlingException");
    BOOST_TEST(refused->reason == "TableReadAccountLimitExceeded");
}

BOOST_AUTO_TEST_CASE(a_hot_partition_key_is_throttled_while_the_others_are_not) {
    ddb::capacity_throttler th;
    const auto t = on_demand();
    const ddb::capacity_target hot{"", ddb::capacity_kind::write, "hot"};
    const ddb::capacity_target cold{"", ddb::capacity_kind::write, "cold"};
    th.charge(t, hot, ddb::k_partition_write_units, k_t0);
    const auto refused = th.admit(t, hot, k_t0);
    BOOST_TEST_REQUIRE(refused.has_value());
    BOOST_TEST(refused->exception == "ProvisionedThroughputExceededException");
    BOOST_TEST(refused->reason == "TableWriteKeyRangeThroughputExceeded");
    BOOST_TEST(!th.admit(t, cold, k_t0).has_value());
}

BOOST_AUTO_TEST_CASE(a_gsi_has_buckets_of_its_own) {
    ddb::capacity_throttler th;
    auto t = provisioned(100, 100);
    ddb::global_index g;
    g.name = "byOwner";
    g.read_capacity = 1;
    g.write_capacity = 1;
    t.gsis.push_back(g);
    const ddb::capacity_target index_write{"byOwner", ddb::capacity_kind::write, ""};
    th.charge(t, index_write, 300.0, k_t0);
    const auto refused = th.admit(t, index_write, k_t0);
    BOOST_TEST_REQUIRE(refused.has_value());
    BOOST_TEST(refused->reason == "IndexWriteProvisionedThroughputExceeded");
    BOOST_TEST(refused->index == "byOwner");
    BOOST_TEST(!th.admit(t, k_table_write, k_t0).has_value());
    // An index the table no longer has is not enforced.
    BOOST_TEST(!th.admit(t, {"gone", ddb::capacity_kind::write, ""}, k_t0).has_value());
}

BOOST_AUTO_TEST_CASE(a_share_scales_every_limit) {
    ddb::capacity_throttler th(0.5);
    const auto t = provisioned(2, 2);  // 300 units of burst at half of 2/s
    th.charge(t, k_table_write, 299.0, k_t0);
    BOOST_TEST(!th.admit(t, k_table_write, k_t0).has_value());
    th.charge(t, k_table_write, 1.0, k_t0);
    BOOST_TEST(th.admit(t, k_table_write, k_t0).has_value());
}

BOOST_AUTO_TEST_CASE(idle_partition_buckets_are_swept) {
    ddb::capacity_throttler th;
    const auto t = on_demand();
    for (int i = 0; i < 5000; ++i) {
        th.charge(t, {"", ddb::capacity_kind::read, "k" + std::to_string(i)}, 1.0, k_t0);
    }
    BOOST_TEST(th.partition_buckets() > 4000U);
    th.charge(t, {"", ddb::capacity_kind::read, "late"}, 1.0, k_t0 + 10000);
    BOOST_TEST(th.partition_buckets() < 10U);
}

BOOST_FIXTURE_TEST_CASE(throttling_is_on_by_default_and_answers_as_aws, capacity_fixture) {
    create_small("tbl");
    clock_ms = k_t0;
    // 300 KB: the whole burst at once, admitted because the bucket held
    // tokens when it arrived.
    BOOST_TEST_REQUIRE(put("tbl", "a", 300).status == 200U);
    require_throttle(put("tbl", "b", 1), "ProvisionedThroughputExceededException",
                     "TableWriteProvisionedThroughputExceeded", std::string(k_arn) + "tbl");
    BOOST_TEST(call("PutItem", R"({"TableName":"tbl","Item":{"pk":{"S":"b"}}})").message() ==
               "The level of configured provisioned throughput for the table was exceeded. "
               "Consider increasing your provisioning level with the UpdateTable API.");
    // Reads are separate, and the bucket refills with time.
    BOOST_TEST(get("tbl", "a").status == 200U);
    clock_ms = k_t0 + 2000;
    BOOST_TEST(put("tbl", "b", 1).status == 200U);
}

BOOST_FIXTURE_TEST_CASE(the_cluster_switch_turns_throttling_off, capacity_fixture) {
    create_small("tbl");
    clock_ms = k_t0;
    items.set_compat_profile(ddb::cluster_profile({{"throttling", "off"}}).value());
    BOOST_TEST(put("tbl", "a", 300).status == 200U);
    BOOST_TEST(put("tbl", "b", 300).status == 200U);
    BOOST_TEST(put("tbl", "c", 300).status == 200U);
}

BOOST_FIXTURE_TEST_CASE(a_table_tag_turns_throttling_off_or_back_on, capacity_fixture) {
    create_small("off", R"(,"Tags":[{"Key":"kythira:compat:throttling","Value":"off"}])");
    create_small("ont", R"(,"Tags":[{"Key":"kythira:compat:throttling","Value":"on"}])");
    clock_ms = k_t0;
    BOOST_TEST(put("off", "a", 300).status == 200U);
    BOOST_TEST(put("off", "b", 300).status == 200U);
    items.set_compat_profile(ddb::cluster_profile({{"throttling", "off"}}).value());
    BOOST_TEST(put("ont", "a", 300).status == 200U);
    BOOST_TEST(put("ont", "b", 1).status == 400U);
}

BOOST_FIXTURE_TEST_CASE(a_throttled_gsi_throttles_the_writes_that_feed_it, capacity_fixture) {
    ok("CreateTable",
       R"({"TableName":"gtb","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
       R"({"AttributeName":"o","AttributeType":"S"}],)"
       R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],)"
       R"("ProvisionedThroughput":{"ReadCapacityUnits":1000,"WriteCapacityUnits":1000},)"
       R"("GlobalSecondaryIndexes":[{"IndexName":"byOwner","KeySchema":[{"AttributeName":"o","KeyType":"HASH"}],)"
       R"("Projection":{"ProjectionType":"KEYS_ONLY"},)"
       R"("ProvisionedThroughput":{"ReadCapacityUnits":1,"WriteCapacityUnits":1}}]})");
    clock_ms = k_t0;
    BOOST_TEST_REQUIRE(put("gtb", "a", 300, R"(,"o":{"S":"x"})").status == 200U);
    require_throttle(
        put("gtb", "b", 1, R"(,"o":{"S":"y"})"), "ProvisionedThroughputExceededException",
        "IndexWriteProvisionedThroughputExceeded", std::string(k_arn) + "gtb/index/byOwner");
    // A put of an item the index will not hold does not draw on it.
    BOOST_TEST(put("gtb", "c", 1).status == 200U);
    // An update or delete may change the index, so it does.
    BOOST_TEST(call("DeleteItem", R"({"TableName":"gtb","Key":{"pk":{"S":"c"}}})").status == 400U);
}

BOOST_FIXTURE_TEST_CASE(a_hot_key_is_throttled_on_an_on_demand_table, capacity_fixture) {
    create_on_demand("odt");
    clock_ms = k_t0;
    BOOST_TEST_REQUIRE(put("odt", "hot", 390).status == 200U);
    BOOST_TEST_REQUIRE(put("odt", "cold", 1).status == 200U);
    // A consistent read of 390 KB is 98 units: 31 of them pass 3,000.
    int admitted = 0;
    reply last;
    for (int i = 0; i < 40; ++i) {
        last = get("odt", "hot");
        if (last.status != 200U) {
            break;
        }
        ++admitted;
    }
    BOOST_TEST(admitted == 31);
    require_throttle(last, "ProvisionedThroughputExceededException",
                     "TableReadKeyRangeThroughputExceeded", std::string(k_arn) + "odt");
    BOOST_TEST(get("odt", "cold").status == 200U);
}

BOOST_FIXTURE_TEST_CASE(a_batch_returns_throttled_writes_unprocessed, capacity_fixture) {
    create_small("tbl");
    clock_ms = k_t0;
    const auto r =
        ok("BatchWriteItem",
           R"({"RequestItems":{"tbl":[{"PutRequest":{"Item":{"pk":{"S":"a"},"b":{"S":")" +
               blob(300 * 1024 - 64) + R"("}}}},{"PutRequest":{"Item":{"pk":{"S":"b"}}}}]}})");
    const auto& left = r.at("UnprocessedItems").as_object();
    BOOST_TEST_REQUIRE(left.contains("tbl"));
    BOOST_TEST(left.at("tbl").as_array().size() == 1U);
    BOOST_TEST(
        left.at("tbl").as_array()[0].at("PutRequest").at("Item").at("pk").at("S").as_string() ==
        "b");
    // When every write is throttled, the call fails.
    const auto all = call("BatchWriteItem",
                          R"({"RequestItems":{"tbl":[{"PutRequest":{"Item":{"pk":{"S":"c"}}}}]}})");
    BOOST_TEST(all.status == 400U);
    BOOST_TEST(all.type() == "ProvisionedThroughputExceededException");
    // Reads: a throttled key comes back in UnprocessedKeys.
    BOOST_TEST_REQUIRE(get("tbl", "a").status == 200U);  // 75 units of 300
    for (int i = 0; i < 3; ++i) {
        (void)get("tbl", "a");
    }
    const auto keys =
        call("BatchGetItem",
             R"({"RequestItems":{"tbl":{"Keys":[{"pk":{"S":"a"}}],"ConsistentRead":true}}})");
    BOOST_TEST(keys.status == 400U);
    BOOST_TEST(keys.type() == "ProvisionedThroughputExceededException");
}

BOOST_FIXTURE_TEST_CASE(a_throttled_transaction_is_cancelled_with_throttling_error,
                        capacity_fixture) {
    create_small("tbl");
    create_small("utb");
    clock_ms = k_t0;
    BOOST_TEST_REQUIRE(put("tbl", "a", 300).status == 200U);
    const auto r = call("TransactWriteItems",
                        R"({"TransactItems":[{"Put":{"TableName":"utb","Item":{"pk":{"S":"x"}}}},)"
                        R"({"Put":{"TableName":"tbl","Item":{"pk":{"S":"y"}}}}]})");
    BOOST_TEST_REQUIRE(r.status == 400U);
    BOOST_TEST(r.type() == "TransactionCanceledException");
    const auto& reasons = r.body.at("CancellationReasons").as_array();
    BOOST_TEST(reasons[0].at("Code").as_string() == "None");
    BOOST_TEST(reasons[1].at("Code").as_string() == "ThrottlingError");
    BOOST_TEST(r.message() ==
               "Transaction cancelled, please refer cancellation reasons for "
               "specific reasons [None, ThrottlingError]");
    // Nothing was written.
    BOOST_TEST(!ok("GetItem", R"({"TableName":"utb","Key":{"pk":{"S":"x"}}})").contains("Item"));
}

BOOST_FIXTURE_TEST_CASE(a_write_is_charged_the_larger_of_the_item_before_and_after,
                        capacity_fixture) {
    create_on_demand("odt");
    clock_ms = k_t0;
    BOOST_TEST_REQUIRE(put("odt", "a", 3).status == 200U);
    const auto units = [](const json::object& out) {
        return out.at("ConsumedCapacity").at("CapacityUnits").to_number<double>();
    };
    BOOST_TEST(
        units(ok(
            "UpdateItem",
            R"({"TableName":"odt","Key":{"pk":{"S":"a"}},"UpdateExpression":"SET n = :n",)"
            R"("ExpressionAttributeValues":{":n":{"N":"1"}},"ReturnConsumedCapacity":"TOTAL"})")) ==
        3.0);
    // A failed condition is charged too (here, the stored item's size), but
    // the error carries no ConsumedCapacity.
    BOOST_TEST(call("DeleteItem", R"({"TableName":"odt","Key":{"pk":{"S":"a"}},)"
                                  R"j("ConditionExpression":"attribute_not_exists(pk)"})j")
                   .status == 400U);
    BOOST_TEST(units(ok("DeleteItem", R"({"TableName":"odt","Key":{"pk":{"S":"a"}},)"
                                      R"("ReturnConsumedCapacity":"TOTAL"})")) == 3.0);
    // A delete of nothing costs one unit.
    BOOST_TEST(units(ok("DeleteItem", R"({"TableName":"odt","Key":{"pk":{"S":"a"}},)"
                                      R"("ReturnConsumedCapacity":"TOTAL"})")) == 1.0);
}
