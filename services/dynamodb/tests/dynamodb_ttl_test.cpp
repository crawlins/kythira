// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Time to live (`.kiro/specs/dynamodb-compatible-api/` task 13,
// Requirements 15.1-15.3): UpdateTimeToLive and DescribeTimeToLive with
// DynamoDB Local's answers, the hourly limit and its switch, the catalog's
// stored form, and the sweeper over local_backend with the fixture's clock.

#define BOOST_TEST_MODULE dynamodb_ttl_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/compat_profile.hpp>
#include <kythira/dynamodb/ttl.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

using namespace dynamodb_test;

namespace {

/// The fixture's clock, in whole seconds.
constexpr std::uint64_t k_now_s = 1794300000ULL;

struct ttl_fixture : fixture {
    ttl_fixture() { clock_ms = k_now_s * 1000; }

    auto update(const std::string& table, bool enabled, const std::string& attribute) -> reply {
        return call("UpdateTimeToLive", R"({"TableName":")" + table +
                                            R"(","TimeToLiveSpecification":{"Enabled":)" +
                                            (enabled ? "true" : "false") + R"(,"AttributeName":")" +
                                            attribute + R"("}})");
    }
    auto describe(const std::string& table) -> json::object {
        return ok("DescribeTimeToLive", R"({"TableName":")" + table + R"("})")
            .at("TimeToLiveDescription")
            .as_object();
    }
    auto put(const std::string& table, const std::string& pk, const std::string& extra) -> void {
        (void)ok("PutItem", R"({"TableName":")" + table + R"(","Item":{"pk":{"S":")" + pk +
                                R"("},"sk":{"N":"1"})" + extra + "}}");
    }
    auto exists(const std::string& table, const std::string& pk) -> bool {
        return ok("GetItem", R"({"TableName":")" + table + R"(","Key":{"pk":{"S":")" + pk +
                                 R"("},"sk":{"N":"1"}},"ConsistentRead":true})")
            .contains("Item");
    }
    /// A table with TTL on attribute `exp`.
    auto table_with_ttl(const std::string& name) -> void {
        (void)create(name);
        BOOST_TEST_REQUIRE(update(name, true, "exp").status == 200U);
    }
    static auto exp(std::uint64_t seconds) -> std::string {
        return R"(,"exp":{"N":")" + std::to_string(seconds) + R"("})";
    }
};

}  // namespace

BOOST_FIXTURE_TEST_CASE(a_new_table_has_ttl_disabled, ttl_fixture) {
    (void)create("tbl");
    const auto d = describe("tbl");
    BOOST_TEST(d.at("TimeToLiveStatus").as_string() == "DISABLED");
    BOOST_TEST(!d.contains("AttributeName"));
}

BOOST_FIXTURE_TEST_CASE(enabling_and_disabling_answer_as_dynamodb_local, ttl_fixture) {
    (void)create("tbl");
    // Each answer below is DynamoDB Local 3.3's, request for request.
    auto r = update("tbl", false, "exp");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.type() == "ValidationException");
    BOOST_TEST(r.message() == "TimeToLive is already disabled");

    r = update("tbl", true, "exp");
    BOOST_TEST_REQUIRE(r.status == 200U);
    const auto& spec = r.body.at("TimeToLiveSpecification").as_object();
    BOOST_TEST(spec.at("Enabled").as_bool());
    BOOST_TEST(spec.at("AttributeName").as_string() == "exp");
    const auto d = describe("tbl");
    BOOST_TEST(d.at("TimeToLiveStatus").as_string() == "ENABLED");
    BOOST_TEST(d.at("AttributeName").as_string() == "exp");

    clock_ms += std::uint64_t{3600} * 1000;
    BOOST_TEST(update("tbl", true, "exp").message() == "TimeToLive is already enabled");
    BOOST_TEST(update("tbl", true, "other").message() ==
               "TimeToLive is active on a different AttributeName");
    BOOST_TEST(update("tbl", false, "other").message() ==
               "TimeToLive is active on a different AttributeName");

    r = update("tbl", false, "exp");
    BOOST_TEST_REQUIRE(r.status == 200U);
    BOOST_TEST(!r.body.at("TimeToLiveSpecification").at("Enabled").as_bool());
    BOOST_TEST(r.body.at("TimeToLiveSpecification").at("AttributeName").as_string() == "exp");
    BOOST_TEST(describe("tbl").at("TimeToLiveStatus").as_string() == "DISABLED");
}

BOOST_FIXTURE_TEST_CASE(a_missing_table_is_not_found, ttl_fixture) {
    auto r = call("DescribeTimeToLive", R"({"TableName":"nope"})");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.type() == "ResourceNotFoundException");
    r = update("nope", true, "exp");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.type() == "ResourceNotFoundException");
    r = call("DescribeTimeToLive", R"({"TableName":"ab"})");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.type() == "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(a_second_change_within_an_hour_is_refused, ttl_fixture) {
    table_with_ttl("tbl");
    clock_ms += std::uint64_t{3599} * 1000;
    const auto r = update("tbl", false, "exp");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.type() == "ValidationException");
    BOOST_TEST(r.message() ==
               "Time to live has been modified multiple times within a fixed interval");
    clock_ms += 1000;
    BOOST_TEST(update("tbl", false, "exp").status == 200U);
}

BOOST_FIXTURE_TEST_CASE(the_prompt_switch_lifts_the_hourly_limit, ttl_fixture) {
    // Per cluster...
    ttl.set_compat_profile(ddb::cluster_profile({{"ttl-delay", "prompt"}}).value());
    table_with_ttl("tbl");
    BOOST_TEST(update("tbl", false, "exp").status == 200U);
    BOOST_TEST(update("tbl", true, "exp").status == 200U);
    // ...and per table, over the cluster's.
    ttl.set_compat_profile(ddb::dynamo_compat_profile{});
    (void)create("tagged", R"(,"Tags":[{"Key":"kythira:compat:ttl-delay","Value":"prompt"}])");
    BOOST_TEST(update("tagged", true, "exp").status == 200U);
    BOOST_TEST(update("tagged", false, "exp").status == 200U);
    BOOST_TEST(update("tbl", false, "exp").status == 400U);
}

BOOST_AUTO_TEST_CASE(the_setting_survives_the_catalog_stored_form) {
    ddb::table_descriptor d;
    d.name = "tbl";
    d.table_id = 9;
    d.ttl.enabled = true;
    d.ttl.attribute = "exp";
    d.ttl.changed_ms = 1794300000123ULL;
    const auto back = ddb::decode_table_descriptor(ddb::encode_table_descriptor(d));
    BOOST_TEST_REQUIRE(back.has_value());
    BOOST_TEST((back->ttl == d.ttl));

    ddb::catalog_command c;
    c.op = ddb::catalog_command::kind::set_ttl;
    c.table = d;
    c.ttl_min_interval_ms = 3600000;
    bool newer = false;
    const auto cb = ddb::decode_catalog_command(ddb::encode_catalog_command(c), newer);
    BOOST_TEST_REQUIRE(cb.has_value());
    BOOST_TEST((*cb == c));

    // A version 2 descriptor (before TTL) still decodes, with TTL off.
    auto bytes = ddb::encode_table_descriptor(ddb::table_descriptor{.name = "old", .table_id = 3});
    bytes[0] = '\2';
    bytes.resize(bytes.size() - 3);  // the flag, an empty name and a zero time
    const auto old = ddb::decode_table_descriptor(bytes);
    BOOST_TEST_REQUIRE(old.has_value());
    BOOST_TEST(!old->ttl.enabled);
}

BOOST_AUTO_TEST_CASE(an_item_expires_once_its_time_has_passed_unless_years_old) {
    const std::uint64_t now_ms = k_now_s * 1000 + 500;
    const auto with = [](ddb::attribute_value v) { return ddb::item{{"exp", std::move(v)}}; };
    const auto num = [](const char* text) {
        return ddb::attribute_value::number(*ddb::dynamo_decimal::from_string(text));
    };
    BOOST_TEST(ddb::expired_value(with(num("1794299999")), "exp", now_ms).has_value());
    BOOST_TEST(ddb::expired_value(with(num("1794300000.4")), "exp", now_ms).has_value());
    BOOST_TEST(!ddb::expired_value(with(num("1794300000.6")), "exp", now_ms).has_value());
    BOOST_TEST(!ddb::expired_value(with(num("1794300001")), "exp", now_ms).has_value());
    // More than five years old: ignored, as DynamoDB ignores it.
    BOOST_TEST(!ddb::expired_value(with(num("1000")), "exp", now_ms).has_value());
    BOOST_TEST(ddb::expired_value(with(num("1636620001")), "exp", now_ms).has_value());
    // Not a number, or absent.
    BOOST_TEST(!ddb::expired_value(with(ddb::attribute_value::string("1")), "exp", now_ms));
    BOOST_TEST(!ddb::expired_value(with(num("1")), "other", now_ms).has_value());
}

BOOST_FIXTURE_TEST_CASE(the_sweeper_deletes_expired_items_and_keeps_the_rest, ttl_fixture) {
    sweeper.set_compat_profile(ddb::cluster_profile({{"ttl-delay", "prompt"}}).value());
    table_with_ttl("tbl");
    put("tbl", "gone", exp(k_now_s - 10));
    put("tbl", "later", exp(k_now_s + 10));
    put("tbl", "none", "");
    put("tbl", "text", R"(,"exp":{"S":"1"})");
    // Expired but still readable until the sweeper runs (15.3).
    BOOST_TEST(exists("tbl", "gone"));
    BOOST_TEST(sweeper.pass() == 1U);
    BOOST_TEST(!exists("tbl", "gone"));
    BOOST_TEST(exists("tbl", "later"));
    BOOST_TEST(exists("tbl", "none"));
    BOOST_TEST(exists("tbl", "text"));
    clock_ms += 11000;
    BOOST_TEST(sweeper.pass() == 1U);
    BOOST_TEST(!exists("tbl", "later"));
}

BOOST_FIXTURE_TEST_CASE(a_table_without_ttl_is_left_alone, ttl_fixture) {
    sweeper.set_compat_profile(ddb::cluster_profile({{"ttl-delay", "prompt"}}).value());
    (void)create("tbl");
    put("tbl", "a", exp(k_now_s - 10));
    BOOST_TEST(sweeper.pass() == 0U);
    BOOST_TEST(exists("tbl", "a"));
    // Turning TTL off stops it too.
    BOOST_TEST_REQUIRE(update("tbl", true, "exp").status == 200U);
    clock_ms += std::uint64_t{3600} * 1000;
    BOOST_TEST_REQUIRE(update("tbl", false, "exp").status == 200U);
    BOOST_TEST(sweeper.pass() == 0U);
    BOOST_TEST(exists("tbl", "a"));
}

BOOST_FIXTURE_TEST_CASE(a_pass_is_bounded_and_the_next_one_resumes, ttl_fixture) {
    ddb::ttl_sweeper<ddb::local_backend<>> small(
        backend, ddb::ttl_sweeper_options{.scan_items = 10, .max_deletes = 4});
    small.set_compat_profile(ddb::cluster_profile({{"ttl-delay", "prompt"}}).value());
    table_with_ttl("tbl");
    for (int i = 0; i < 25; ++i) {
        put("tbl", "k" + std::to_string(i), i % 2 == 0 ? exp(k_now_s - 1) : "");
    }
    // 13 expired: at most 4 deletes and 10 reads a pass.
    std::size_t total = 0;
    for (int pass = 0; pass < 10 && total < 13; ++pass) {
        const auto n = small.pass();
        BOOST_TEST(n <= 4U);
        total += n;
    }
    BOOST_TEST(total == 13U);
    for (int i = 0; i < 25; ++i) {
        BOOST_TEST(exists("tbl", "k" + std::to_string(i)) == (i % 2 == 1));
    }
}

BOOST_FIXTURE_TEST_CASE(with_aws_delay_a_table_is_walked_again_after_the_interval, ttl_fixture) {
    // The default switch: one walk, then none until the interval passes.
    table_with_ttl("tbl");
    BOOST_TEST(sweeper.pass() == 0U);
    put("tbl", "a", exp(k_now_s - 1));
    BOOST_TEST(sweeper.pass() == 0U);
    clock_ms += std::uint64_t{10} * 60 * 1000;
    BOOST_TEST(sweeper.pass() == 1U);
    BOOST_TEST(!exists("tbl", "a"));
}

BOOST_FIXTURE_TEST_CASE(an_item_written_again_since_it_was_read_is_kept, ttl_fixture) {
    // The sweeper's delete carries the value it read: here the item was
    // rewritten between the read and the delete, so the condition fails.
    table_with_ttl("tbl");
    put("tbl", "a", exp(k_now_s - 1));
    ddb::table_descriptor d = *backend.describe("tbl");
    ddb::item key{{"pk", ddb::attribute_value::string("a")},
                  {"sk", ddb::attribute_value::number(*ddb::dynamo_decimal::from_string("1"))}};
    put("tbl", "a", exp(k_now_s + 1000));
    ddb::item_command c;
    c.op = ddb::item_command::kind::delete_;
    c.table_id = d.table_id;
    c.schema = d.schema;
    c.payload = key;
    boost::json::object in{
        {"ConditionExpression", "#t = :v"},
        {"ExpressionAttributeNames", boost::json::object{{"#t", "exp"}}},
        {"ExpressionAttributeValues",
         boost::json::object{{":v", boost::json::object{{"N", std::to_string(k_now_s - 1)}}}}}};
    ddb::request_keys keys;
    keys.table = d.schema;
    c.expressions = *ddb::compile_request("DeleteItem", in, keys);
    BOOST_TEST((backend.write(c).outcome == ddb::item_result::status::condition_failed));
    BOOST_TEST(exists("tbl", "a"));
}

BOOST_FIXTURE_TEST_CASE(a_sweep_removes_the_item_from_its_indexes, ttl_fixture) {
    sweeper.set_compat_profile(ddb::cluster_profile({{"ttl-delay", "prompt"}}).value());
    (void)ok(
        "CreateTable",
        R"({"TableName":"idx","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
        R"({"AttributeName":"sk","AttributeType":"N"},{"AttributeName":"g","AttributeType":"S"}],)"
        R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},{"AttributeName":"sk","KeyType":"RANGE"}],)"
        R"("GlobalSecondaryIndexes":[{"IndexName":"byG","KeySchema":[{"AttributeName":"g","KeyType":"HASH"}],)"
        R"("Projection":{"ProjectionType":"ALL"}}],"BillingMode":"PAY_PER_REQUEST"})");
    BOOST_TEST_REQUIRE(update("idx", true, "exp").status == 200U);
    put("idx", "a", R"(,"g":{"S":"x"})" + exp(k_now_s - 1));
    const auto query = [&] {
        return ok("Query",
                  R"({"TableName":"idx","IndexName":"byG","KeyConditionExpression":"g = :g",)"
                  R"("ExpressionAttributeValues":{":g":{"S":"x"}}})")
            .at("Count")
            .to_number<int>();
    };
    BOOST_TEST(query() == 1);
    BOOST_TEST(sweeper.pass() == 1U);
    BOOST_TEST(query() == 0);
}
