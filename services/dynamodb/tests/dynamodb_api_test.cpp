// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The control plane and single-item API through the front end
// (`.kiro/specs/dynamodb-compatible-api/` task 7, Requirements 7.1, 8.1,
// 8.6). Requests are signed with SigV4 and checked by the static-key
// authorizer and the codec, as from an SDK. The handlers run on
// local_backend over memory_kv_engine. Each case checks status
// transitions, AWS's validation wording, tags, the background delete,
// ReturnValues, conditions, projections, consumed capacity and the disk
// guard.

#define BOOST_TEST_MODULE dynamodb_api_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <boost/json.hpp>

#include <cstdint>
#include <string>

using namespace dynamodb_test;

namespace {

const std::string k_arn = "arn:aws:dynamodb:us-east-1:111122223333:table/orders";

}  // namespace

BOOST_AUTO_TEST_CASE(create_then_describe_shows_the_status_transition) {
    fixture f;
    const auto created =
        f.create("orders", R"(,"Tags":[{"Key":"b","Value":"2"},{"Key":"a","Value":"1"}])");
    const auto& d = created.at("TableDescription").as_object();
    BOOST_TEST(d.at("TableStatus").as_string() == "CREATING");
    BOOST_TEST(d.at("TableArn").as_string() == k_arn);
    BOOST_TEST(d.at("TableId").as_string() == "00000000-0000-4000-8000-000000000001");
    BOOST_TEST(d.at("CreationDateTime").as_double() == 1794300000.123);
    BOOST_TEST(d.at("KeySchema").as_array().size() == 2U);
    BOOST_TEST(d.at("ProvisionedThroughput").at("ReadCapacityUnits").to_number<int>() == 5);
    BOOST_TEST(!d.contains("BillingModeSummary"));

    const auto described = f.ok("DescribeTable", R"({"TableName":"orders"})");
    BOOST_TEST(described.at("Table").at("TableStatus").as_string() == "ACTIVE");
    // A table ARN works where a name does.
    BOOST_TEST(f.ok("DescribeTable", R"({"TableName":")" + k_arn + R"("})")
                   .at("Table")
                   .at("TableName")
                   .as_string() == "orders");

    const auto dup = f.call(
        "CreateTable",
        R"({"TableName":"orders","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],"KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],"BillingMode":"PAY_PER_REQUEST"})");
    BOOST_TEST(dup.type() == "ResourceInUseException");
    BOOST_TEST(dup.message() == "Table already exists: orders");

    const auto missing = f.call("DescribeTable", R"({"TableName":"nope"})");
    BOOST_TEST(missing.type() == "ResourceNotFoundException");
    BOOST_TEST(missing.message() == "Requested resource not found: Table: nope not found");
}

BOOST_AUTO_TEST_CASE(create_table_checks_in_aws_order_and_wording) {
    fixture f;
    const auto try_create = [&](const std::string& ad, const std::string& ks,
                                const std::string& extra) {
        return f.call("CreateTable", R"({"TableName":"tbl","AttributeDefinitions":)" + ad +
                                         R"(,"KeySchema":)" + ks + extra + "}");
    };
    const std::string pt =
        R"(,"ProvisionedThroughput":{"ReadCapacityUnits":1,"WriteCapacityUnits":1})";
    const std::string ad1 = R"([{"AttributeName":"pk","AttributeType":"S"}])";
    const std::string ad2 =
        R"([{"AttributeName":"pk","AttributeType":"S"},{"AttributeName":"x","AttributeType":"S"}])";
    const std::string hash = R"({"AttributeName":"pk","KeyType":"HASH"})";
    const std::string range_x = R"({"AttributeName":"x","KeyType":"RANGE"})";

    struct expectation {
        reply got;
        std::string message;
    };
    const std::vector<expectation> cases = {
        {f.call("CreateTable", R"({"TableName":"a b","AttributeDefinitions":)" + ad1 +
                                   R"(,"KeySchema":[)" + hash + "]" + pt + "}"),
         "Invalid table/index name.  Table/index names must be between 3 and 255 characters "
         "long, and may contain only the characters a-z, A-Z, 0-9, '_', '-', and '.'"},
        {try_create(ad2, "[" + range_x + "]", pt),
         "No Hash Key specified in schema.  All Dynamo DB tables must have exactly one hash key"},
        {try_create(ad2, "[" + hash + R"(,{"AttributeName":"x","KeyType":"HASH"}])", pt),
         "Too many hash keys specified.  All Dynamo DB tables must have exactly one hash key"},
        {try_create(ad2, "[" + range_x + "," + hash + "]", pt),
         "Invalid key order. Hash Key must be specified first in key schema, Range Key must be "
         "specifed second"},
        {try_create(ad1, "[" + hash + R"(,{"AttributeName":"pk","KeyType":"RANGE"}])", pt),
         "Two keys can not have the same name"},
        {try_create(ad2, "[" + hash + "]", pt),
         "One or more parameter values were invalid: Number of attributes in KeySchema does not "
         "exactly match number of attributes defined in AttributeDefinitions"},
        {try_create(R"([{"AttributeName":"x","AttributeType":"S"}])", "[" + hash + "]", pt),
         "One or more parameter values were invalid: Some index key attributes are not defined "
         "in AttributeDefinitions. Keys: [pk], AttributeDefinitions: [x]"},
        {try_create(ad1, "[" + hash + "]", ""),
         "One or more parameter values were invalid: ReadCapacityUnits and WriteCapacityUnits "
         "must both be specified when BillingMode is PROVISIONED"},
        {try_create(ad1, "[" + hash + "]", R"(,"BillingMode":"PAY_PER_REQUEST")" + pt),
         "One or more parameter values were invalid: Neither ReadCapacityUnits nor "
         "WriteCapacityUnits can be specified when BillingMode is PAY_PER_REQUEST"},
        {try_create(ad1, "[" + hash + "]",
                    pt + R"(,"GlobalSecondaryIndexes":[{"IndexName":"gsi","KeySchema":[)" + hash +
                        R"(],"Projection":{"ProjectionType":"ALL"}}])"),
         "No provisioned throughput specified for the global secondary index"},
        // Vector indexes (task 26) are checked too.
        {try_create(ad1, "[" + hash + "]",
                    pt + R"(,"VectorIndexes":[{"IndexName":"vec","VectorAttribute":)"
                         R"({"AttributeName":"pk"},"Projection":{"ProjectionType":"ALL"},)"
                         R"("Dimensions":3,"DistanceFunction":"COSINE"}])"),
         "One or more parameter values were invalid: The vector attribute pk of vector index vec "
         "cannot be a key attribute of the table"},
    };
    for (const auto& c : cases) {
        BOOST_TEST(c.got.status == 400U);
        BOOST_TEST(c.got.type() == "ValidationException");
        BOOST_TEST(c.got.message() == c.message);
    }
    BOOST_TEST(f.ok("ListTables", "{}").at("TableNames").as_array().empty());
}

BOOST_AUTO_TEST_CASE(list_tables_pages) {
    fixture f;
    for (const char* n : {"ccc", "aaa", "bbb"}) {
        f.create(n);
    }
    auto page = f.ok("ListTables", R"({"Limit":2})");
    BOOST_TEST(page.at("TableNames").as_array().size() == 2U);
    BOOST_TEST(page.at("LastEvaluatedTableName").as_string() == "bbb");
    page = f.ok("ListTables", R"({"Limit":2,"ExclusiveStartTableName":"bbb"})");
    BOOST_TEST(page.at("TableNames").as_array().size() == 1U);
    BOOST_TEST(!page.contains("LastEvaluatedTableName"));
    BOOST_TEST(f.ok("ListTables", "{}").at("TableNames").as_array().size() == 3U);
}

BOOST_AUTO_TEST_CASE(update_table) {
    fixture f;
    f.create("orders");
    auto r = f.call("UpdateTable", R"({"TableName":"orders"})");
    BOOST_TEST(r.message() ==
               "At least one of ProvisionedThroughput, BillingMode, UpdateStreamEnabled, "
               "GlobalSecondaryIndexUpdates or SSESpecification or ReplicaUpdates is required");
    r = f.call("UpdateTable",
               R"({"TableName":"orders","VectorIndexUpdates":[{"Delete":{"IndexName":"vec"}}]})");
    BOOST_TEST(r.type() == "ResourceNotFoundException");
    BOOST_TEST(r.message() == "Requested resource not found: Vector index vec does not exist");
    r = f.call(
        "UpdateTable",
        R"({"TableName":"orders","ProvisionedThroughput":{"ReadCapacityUnits":5,"WriteCapacityUnits":5}})");
    BOOST_TEST(
        r.message().starts_with("The provisioned throughput for the table will not change."));
    BOOST_TEST(r.message().find("Requested WriteCapacityUnits: 5.") != std::string::npos);

    auto u = f.ok(
        "UpdateTable",
        R"({"TableName":"orders","ProvisionedThroughput":{"ReadCapacityUnits":7,"WriteCapacityUnits":5}})");
    BOOST_TEST(u.at("TableDescription").at("TableStatus").as_string() == "UPDATING");
    BOOST_TEST(f.ok("DescribeTable", R"({"TableName":"orders"})")
                   .at("Table")
                   .at("TableStatus")
                   .as_string() == "ACTIVE");

    r = f.call(
        "UpdateTable",
        R"({"TableName":"orders","BillingMode":"PAY_PER_REQUEST","ProvisionedThroughput":{"ReadCapacityUnits":7,"WriteCapacityUnits":5}})");
    BOOST_TEST(r.type() == "ValidationException");
    u = f.ok("UpdateTable", R"({"TableName":"orders","BillingMode":"PAY_PER_REQUEST"})");
    const auto& d = u.at("TableDescription").as_object();
    BOOST_TEST(d.at("BillingModeSummary").at("BillingMode").as_string() == "PAY_PER_REQUEST");
    BOOST_TEST(d.at("ProvisionedThroughput").at("ReadCapacityUnits").to_number<int>() == 0);

    u = f.ok("UpdateTable", R"({"TableName":"orders","TableClass":"STANDARD_INFREQUENT_ACCESS"})");
    BOOST_TEST(u.at("TableDescription").at("TableClassSummary").at("TableClass").as_string() ==
               "STANDARD_INFREQUENT_ACCESS");

    u = f.ok("UpdateTable", R"({"TableName":"orders","DeletionProtectionEnabled":true})");
    BOOST_TEST(u.at("TableDescription").at("DeletionProtectionEnabled").as_bool());
    r = f.call("DeleteTable", R"({"TableName":"orders"})");
    BOOST_TEST(r.message() ==
               "Resource cannot be deleted as it is currently protected against deletion. "
               "Disable deletion protection first.");
    BOOST_TEST(
        f.call("UpdateTable", R"({"TableName":"nope","DeletionProtectionEnabled":true})").type() ==
        "ResourceNotFoundException");
}

BOOST_AUTO_TEST_CASE(warm_throughput_is_recorded_and_only_raised) {
    fixture f;
    const auto warm = [&](const json::object& table) {
        const auto& w = table.at("WarmThroughput").as_object();
        return std::to_string(w.at("ReadUnitsPerSecond").to_number<std::int64_t>()) + "/" +
               std::to_string(w.at("WriteUnitsPerSecond").to_number<std::int64_t>()) + " " +
               std::string(w.at("Status").as_string());
    };
    // AWS's defaults for a new table.
    auto t = f.create("orders").at("TableDescription").as_object();
    BOOST_TEST(warm(t) == "12000/4000 CREATING");
    t = f.ok("DescribeTable", R"({"TableName":"orders"})").at("Table").as_object();
    BOOST_TEST(warm(t) == "12000/4000 ACTIVE");

    auto u = f.ok("UpdateTable",
                  R"({"TableName":"orders","WarmThroughput":{"ReadUnitsPerSecond":15000}})");
    BOOST_TEST(warm(u.at("TableDescription").as_object()) == "15000/4000 UPDATING");
    t = f.ok("DescribeTable", R"({"TableName":"orders"})").at("Table").as_object();
    BOOST_TEST(warm(t) == "15000/4000 ACTIVE");

    auto r = f.call("UpdateTable",
                    R"({"TableName":"orders","WarmThroughput":{"ReadUnitsPerSecond":14000}})");
    BOOST_TEST(r.type() == "ValidationException");
    BOOST_TEST(r.message() ==
               "One or more parameter values were invalid: WarmThroughput ReadUnitsPerSecond "
               "cannot be lower than the table's current value of 15000");
    r = f.call("UpdateTable",
               R"({"TableName":"orders","WarmThroughput":{"WriteUnitsPerSecond":0}})");
    BOOST_TEST(r.message() ==
               "One or more parameter values were invalid: WarmThroughput WriteUnitsPerSecond "
               "must be positive");

    auto c = f.ok("CreateTable",
                  R"({"TableName":"warm","BillingMode":"PAY_PER_REQUEST",)"
                  R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
                  R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],)"
                  R"("WarmThroughput":{"ReadUnitsPerSecond":30000,"WriteUnitsPerSecond":10000}})");
    BOOST_TEST(warm(c.at("TableDescription").as_object()) == "30000/10000 CREATING");
    r = f.call("CreateTable",
               R"({"TableName":"cold","BillingMode":"PAY_PER_REQUEST",)"
               R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
               R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],)"
               R"("WarmThroughput":{"WriteUnitsPerSecond":100}})");
    BOOST_TEST(r.message() ==
               "One or more parameter values were invalid: WarmThroughput WriteUnitsPerSecond "
               "cannot be lower than the table's current value of 4000");
}

BOOST_AUTO_TEST_CASE(tags) {
    fixture f;
    f.create("orders", R"(,"Tags":[{"Key":"team","Value":"a"}])");
    f.ok("TagResource",
         R"({"ResourceArn":")" + k_arn +
             R"(","Tags":[{"Key":"team","Value":"b"},{"Key":"env","Value":"dev"}]})");
    auto tags =
        f.ok("ListTagsOfResource", R"({"ResourceArn":")" + k_arn + R"("})").at("Tags").as_array();
    BOOST_REQUIRE(tags.size() == 2U);
    BOOST_TEST(tags[0].at("Key").as_string() == "env");
    BOOST_TEST(tags[1].at("Value").as_string() == "b");
    f.ok("UntagResource", R"({"ResourceArn":")" + k_arn + R"(","TagKeys":["team","absent"]})");
    tags =
        f.ok("ListTagsOfResource", R"({"ResourceArn":")" + k_arn + R"("})").at("Tags").as_array();
    BOOST_TEST(tags.size() == 1U);

    const std::string other = "arn:aws:dynamodb:us-east-1:999999999999:table/orders";
    auto r = f.call("ListTagsOfResource", R"({"ResourceArn":")" + other + R"("})");
    BOOST_TEST(r.type() == "ResourceNotFoundException");
    // Another account's ARN is refused by the front end, before the
    // handler, with AWS's generic wording.
    BOOST_TEST(r.message() == "Requested resource not found");
    r = f.call("TagResource", R"({"ResourceArn":"bogus","Tags":[{"Key":"k","Value":"v"}]})");
    BOOST_TEST(r.type() == "ValidationException");
    r = f.call(
        "TagResource",
        R"({"ResourceArn":"arn:aws:dynamodb:us-east-1:111122223333:table/nope","Tags":[{"Key":"k","Value":"v"}]})");
    BOOST_TEST(r.type() == "ResourceNotFoundException");
}

BOOST_AUTO_TEST_CASE(delete_table_removes_items_and_frees_the_name) {
    fixture f;
    f.create("orders");
    for (int i = 0; i < 20; ++i) {
        f.ok("PutItem", R"({"TableName":"orders","Item":{"pk":{"S":"p"},"sk":{"N":")" +
                            std::to_string(i) + R"("}}})");
    }
    const auto deleted = f.ok("DeleteTable", R"({"TableName":"orders"})");
    BOOST_TEST(deleted.at("TableDescription").at("TableStatus").as_string() == "DELETING");
    BOOST_TEST(f.call("DescribeTable", R"({"TableName":"orders"})").type() ==
               "ResourceNotFoundException");
    BOOST_TEST(f.engine.key_count(kythira::column_family::data) == 1U);  // the id counter

    const auto again = f.create("orders");
    BOOST_TEST(again.at("TableDescription").at("TableId").as_string() ==
               "00000000-0000-4000-8000-000000000002");
    BOOST_TEST(!f.ok("GetItem", R"({"TableName":"orders","Key":{"pk":{"S":"p"},"sk":{"N":"1"}}})")
                    .contains("Item"));
}

BOOST_AUTO_TEST_CASE(single_item_writes_and_reads) {
    fixture f;
    f.create("orders");
    const std::string key = R"("Key":{"pk":{"S":"p"},"sk":{"N":"1"}})";

    auto r = f.ok(
        "PutItem",
        R"({"TableName":"orders","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"a":{"S":"x"},"m":{"M":{"n":{"N":"1"}}}},"ReturnConsumedCapacity":"TOTAL"})");
    BOOST_TEST(r.at("ConsumedCapacity").at("TableName").as_string() == "orders");
    BOOST_TEST(r.at("ConsumedCapacity").at("CapacityUnits").as_double() == 1.0);
    BOOST_TEST(!r.contains("Attributes"));

    auto g = f.ok("GetItem",
                  R"({"TableName":"orders",)" + key + R"(,"ReturnConsumedCapacity":"INDEXES"})");
    BOOST_TEST(g.at("Item").as_object().size() == 4U);
    BOOST_TEST(g.at("ConsumedCapacity").at("CapacityUnits").as_double() == 0.5);
    BOOST_TEST(g.at("ConsumedCapacity").at("Table").at("CapacityUnits").as_double() == 0.5);
    g = f.ok(
        "GetItem",
        R"({"TableName":"orders",)" + key +
            R"(,"ConsistentRead":true,"ProjectionExpression":"m.n","ReturnConsumedCapacity":"TOTAL"})");
    BOOST_TEST(json::serialize(g.at("Item")) == R"({"m":{"M":{"n":{"N":"1"}}}})");
    BOOST_TEST(g.at("ConsumedCapacity").at("CapacityUnits").as_double() == 1.0);

    auto u = f.ok(
        "UpdateItem",
        R"({"TableName":"orders",)" + key +
            R"(,"UpdateExpression":"SET a = :v ADD c :one","ExpressionAttributeValues":{":v":{"S":"y"},":one":{"N":"1"}},"ReturnValues":"UPDATED_NEW"})");
    BOOST_TEST(u.at("Attributes").as_object().size() == 2U);

    auto failed = f.call(
        "PutItem",
        R"j({"TableName":"orders","Item":{"pk":{"S":"p"},"sk":{"N":"1"}},"ConditionExpression":"attribute_not_exists(pk)","ReturnValuesOnConditionCheckFailure":"ALL_OLD"})j");
    BOOST_TEST(failed.status == 400U);
    BOOST_TEST(failed.type() == "ConditionalCheckFailedException");
    BOOST_TEST(failed.message() == "The conditional request failed");
    BOOST_TEST(failed.body.at("Item").at("a").at("S").as_string() == "y");

    auto bad = f.call(
        "PutItem",
        R"({"TableName":"orders","Item":{"pk":{"S":"p"},"sk":{"N":"1"}},"ReturnValues":"ALL_NEW"})");
    BOOST_TEST(bad.message() == "Return values set to invalid value");
    bad = f.call("DeleteItem",
                 R"({"TableName":"orders",)" + key + R"(,"ReturnValues":"UPDATED_OLD"})");
    BOOST_TEST(bad.message() == "Return values set to invalid value");
    bad = f.call("GetItem", R"({"TableName":"orders","Key":{"pk":{"N":"1"},"sk":{"N":"1"}}})");
    BOOST_TEST(bad.message() == "The provided key element does not match the schema");
    bad = f.call("PutItem", R"({"TableName":"orders","Item":{"x":{"S":"1"}}})");
    BOOST_TEST(bad.message() ==
               "One or more parameter values were invalid: Missing the key pk in the item");
    bad = f.call("GetItem", R"({"TableName":"nope",)" + key + "}");
    BOOST_TEST(bad.type() == "ResourceNotFoundException");
    BOOST_TEST(bad.message() == "Requested resource not found");
    bad = f.call(
        "UpdateItem",
        R"({"TableName":"orders",)" + key +
            R"(,"UpdateExpression":"SET pk = :v","ExpressionAttributeValues":{":v":{"S":"q"}}})");
    BOOST_TEST(bad.message() ==
               "One or more parameter values were invalid: Cannot update attribute pk. This "
               "attribute is part of the key");

    auto del =
        f.ok("DeleteItem", R"({"TableName":"orders",)" + key + R"(,"ReturnValues":"ALL_OLD"})");
    BOOST_TEST(del.at("Attributes").at("c").at("N").as_string() == "1");
    BOOST_TEST(!f.ok("GetItem", R"({"TableName":"orders",)" + key + "}").contains("Item"));
}

BOOST_AUTO_TEST_CASE(the_disk_guard_refuses_writes_but_not_reads) {
    fixture f;
    f.create("orders");
    f.ok("PutItem", R"({"TableName":"orders","Item":{"pk":{"S":"p"},"sk":{"N":"1"}}})");
    f.free_bytes = 10;
    const auto r =
        f.call("PutItem", R"({"TableName":"orders","Item":{"pk":{"S":"p"},"sk":{"N":"2"}}})");
    BOOST_TEST(r.status == 500U);
    BOOST_TEST(r.type() == "InternalServerError");
    BOOST_TEST(r.message().find("disk guard") != std::string::npos);
    BOOST_TEST(f.ok("GetItem", R"({"TableName":"orders","Key":{"pk":{"S":"p"},"sk":{"N":"1"}}})")
                   .contains("Item"));
}

BOOST_AUTO_TEST_CASE(describe_limits) {
    fixture f;
    const auto r = f.ok("DescribeLimits", "{}");
    BOOST_TEST(r.at("TableMaxReadCapacityUnits").to_number<int>() == 40000);
    BOOST_TEST(r.at("AccountMaxWriteCapacityUnits").to_number<int>() == 80000);
}

BOOST_AUTO_TEST_CASE(state_survives_a_restart_of_the_backend) {
    fixture f;
    f.create("orders");
    f.ok("PutItem", R"({"TableName":"orders","Item":{"pk":{"S":"p"},"sk":{"N":"1"}}})");
    ddb::local_backend<> again(f.engine);
    const auto d = again.describe("orders");
    BOOST_REQUIRE(d.has_value());
    BOOST_TEST((d->status == ddb::table_status::active));
    ddb::item key;
    ddb::attribute_value pk;
    pk.type = ddb::attribute_type::S;
    pk.str = "p";
    ddb::attribute_value sk;
    sk.type = ddb::attribute_type::N;
    BOOST_REQUIRE((ddb::dynamo_decimal::parse("1", sk.num) == ddb::decimal_status::ok));
    key = {{"pk", pk}, {"sk", sk}};
    BOOST_TEST(again.read(d->table_id, d->schema, key, true).has_value());
}
