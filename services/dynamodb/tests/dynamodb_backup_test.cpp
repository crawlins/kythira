// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// On-demand backups (`.kiro/specs/dynamodb-compatible-api/` task 18,
// Requirement 17.1) over local_backend: CreateBackup, DescribeBackup,
// ListBackups, DeleteBackup and RestoreTableFromBackup with the worker that
// takes and loads them, the object layout, overrides, refusals, a table
// deleted under its backup, a damaged object, and the stores (memory,
// directory, any key_object_store).

#define BOOST_TEST_MODULE dynamodb_backup_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"
#include "mock_object_store.hpp"

#include <kythira/dynamodb/backup.hpp>
#include <kythira/dynamodb/catalog.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace dynamodb_test;

namespace {

struct backup_fixture : fixture {
    std::shared_ptr<ddb::memory_backup_store> store = std::make_shared<ddb::memory_backup_store>();
    ddb::backup_api<ddb::local_backend<>> backups{backend, store};
    ddb::backup_worker<ddb::local_backend<>> worker{backend, store};

    backup_fixture() {
        without_throttling();
        backups.register_with(*fe);
        backups.on_created([this] { worker.poke(); });
    }

    auto put(const std::string& table, const std::string& pk, int sk, const std::string& v)
        -> void {
        (void)ok("PutItem", R"({"TableName":")" + table + R"(","Item":{"pk":{"S":")" + pk +
                                R"("},"sk":{"N":")" + std::to_string(sk) + R"("},"v":{"S":")" + v +
                                R"("}}})");
    }
    auto backup(const std::string& table, const std::string& name = "nightly") -> std::string {
        const auto r = ok("CreateBackup",
                          R"({"TableName":")" + table + R"(","BackupName":")" + name + R"("})");
        return std::string(r.at("BackupDetails").as_object().at("BackupArn").as_string());
    }
    auto details(const std::string& arn) -> json::object {
        return ok("DescribeBackup", R"({"BackupArn":")" + arn + R"("})")
            .at("BackupDescription")
            .as_object();
    }
    auto status_of(const std::string& arn) -> std::string {
        return std::string(
            details(arn).at("BackupDetails").as_object().at("BackupStatus").as_string());
    }
    auto restore(const std::string& arn, const std::string& target, const std::string& extra = "")
        -> reply {
        return call(
            "RestoreTableFromBackup",
            R"({"TargetTableName":")" + target + R"(","BackupArn":")" + arn + R"(")" + extra + "}");
    }
    auto table(const std::string& name) -> json::object {
        return ok("DescribeTable", R"({"TableName":")" + name + R"("})").at("Table").as_object();
    }
    auto scan_count(const std::string& name) -> std::int64_t {
        return ok("Scan", R"({"TableName":")" + name + R"("})")
            .at("Count")
            .to_number<std::int64_t>();
    }
};

}  // namespace

BOOST_FIXTURE_TEST_CASE(a_backup_is_taken_and_restored, backup_fixture) {
    (void)create("tbl");
    for (int i = 0; i < 25; ++i) {
        put("tbl", "p" + std::to_string(i % 4), i, "v" + std::to_string(i));
    }
    const auto created = ok("CreateBackup", R"({"TableName":"tbl","BackupName":"nightly"})")
                             .at("BackupDetails")
                             .as_object();
    const std::string arn(created.at("BackupArn").as_string());
    BOOST_TEST(created.at("BackupStatus").as_string() == "CREATING");
    BOOST_TEST(created.at("BackupType").as_string() == "USER");
    BOOST_TEST(created.at("BackupName").as_string() == "nightly");
    // The table's ARN, /backup/, 13 digits, a dash and 8 hex digits.
    const std::string prefix = "arn:aws:dynamodb:us-east-1:" + std::string(k_account) +
                               ":table/tbl/backup/01794300000123-";
    BOOST_TEST(arn.starts_with(prefix));
    BOOST_TEST(arn.size() == prefix.size() + 8);
    BOOST_TEST(status_of(arn) == "CREATING");

    BOOST_TEST(worker.pass() == 1U);
    // Written once the backup is taken: not in it.
    put("tbl", "late", 1, "x");
    const auto d = details(arn);
    BOOST_TEST(d.at("BackupDetails").as_object().at("BackupStatus").as_string() == "AVAILABLE");
    BOOST_TEST(d.at("BackupDetails").as_object().at("BackupSizeBytes").to_number<std::uint64_t>() >
               0U);
    const auto& source = d.at("SourceTableDetails").as_object();
    BOOST_TEST(source.at("TableName").as_string() == "tbl");
    BOOST_TEST(source.at("ItemCount").to_number<std::uint64_t>() == 25U);
    BOOST_TEST(source.at("BillingMode").as_string() == "PROVISIONED");
    BOOST_TEST(source.at("KeySchema").as_array().size() == 2U);
    BOOST_TEST(d.at("SourceTableFeatureDetails")
                   .as_object()
                   .at("TimeToLiveDescription")
                   .as_object()
                   .at("TimeToLiveStatus")
                   .as_string() == "DISABLED");

    // The layout: a record and one data object of DynamoDB JSON lines.
    const auto keys = store->list("backups/");
    BOOST_TEST(keys.size() == 2U);
    const auto data = std::ranges::find_if(keys, [](const std::string& k) {
        return k.find("/data/000000.json") != std::string::npos;
    });
    BOOST_TEST_REQUIRE((data != keys.end()));
    const auto body = store->get(*data).value();
    BOOST_TEST(std::ranges::count(body, '\n') == 25);
    BOOST_TEST(body.starts_with(R"({"Item":{)"));

    const auto r = restore(arn, "copy");
    BOOST_TEST_REQUIRE(r.status == 200U);
    const auto& t = r.body.at("TableDescription").as_object();
    BOOST_TEST(t.at("TableStatus").as_string() == "CREATING");
    const auto& summary = t.at("RestoreSummary").as_object();
    BOOST_TEST(summary.at("SourceBackupArn").as_string() == arn);
    BOOST_TEST(summary.at("SourceTableArn").as_string() ==
               "arn:aws:dynamodb:us-east-1:" + std::string(k_account) + ":table/tbl");
    BOOST_TEST(summary.at("RestoreInProgress").as_bool());
    // Not writable while it loads.
    BOOST_TEST(
        call("PutItem", R"({"TableName":"copy","Item":{"pk":{"S":"x"},"sk":{"N":"1"}}})").type() ==
        "ResourceNotFoundException");

    (void)worker.pass();
    const auto after = table("copy");
    BOOST_TEST(after.at("TableStatus").as_string() == "ACTIVE");
    BOOST_TEST(!after.at("RestoreSummary").as_object().at("RestoreInProgress").as_bool());
    BOOST_TEST(scan_count("copy") == 25);
    BOOST_TEST(scan_count("tbl") == 26);
    const auto got =
        ok("GetItem", R"({"TableName":"copy","Key":{"pk":{"S":"p1"},"sk":{"N":"5"}}})");
    BOOST_TEST(got.at("Item").as_object().at("v").as_object().at("S").as_string() == "v5");
    // Another pass finds nothing to do.
    BOOST_TEST(worker.pass() == 0U);
}

BOOST_FIXTURE_TEST_CASE(a_restore_rebuilds_the_indexes_it_keeps, backup_fixture) {
    (void)ok(
        "CreateTable",
        R"({"TableName":"idx","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
        R"({"AttributeName":"sk","AttributeType":"N"},{"AttributeName":"v","AttributeType":"S"}],)"
        R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},{"AttributeName":"sk","KeyType":"RANGE"}],)"
        R"("BillingMode":"PAY_PER_REQUEST",)"
        R"("LocalSecondaryIndexes":[{"IndexName":"byv","KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
        R"({"AttributeName":"v","KeyType":"RANGE"}],"Projection":{"ProjectionType":"ALL"}}],)"
        R"("GlobalSecondaryIndexes":[{"IndexName":"gsi1","KeySchema":[{"AttributeName":"v","KeyType":"HASH"}],)"
        R"("Projection":{"ProjectionType":"ALL"}},{"IndexName":"gsi2","KeySchema":[{"AttributeName":"v","KeyType":"HASH"}],)"
        R"("Projection":{"ProjectionType":"KEYS_ONLY"}}]})");
    put("idx", "a", 1, "red");
    put("idx", "b", 1, "red");
    put("idx", "c", 1, "blue");
    const auto arn = backup("idx");
    (void)worker.pass();
    const auto features = details(arn).at("SourceTableFeatureDetails").as_object();
    BOOST_TEST(features.at("GlobalSecondaryIndexes").as_array().size() == 2U);
    BOOST_TEST(features.at("LocalSecondaryIndexes").as_array().size() == 1U);

    BOOST_TEST(restore(arn, "bad",
                       R"(,"GlobalSecondaryIndexOverride":[{"IndexName":"nope",)"
                       R"("KeySchema":[{"AttributeName":"v","KeyType":"HASH"}],)"
                       R"("Projection":{"ProjectionType":"ALL"}}])")
                   .type() == "ValidationException");
    const auto r =
        restore(arn, "copy",
                R"(,"GlobalSecondaryIndexOverride":[{"IndexName":"gsi1",)"
                R"("KeySchema":[{"AttributeName":"v","KeyType":"HASH"}],)"
                R"("Projection":{"ProjectionType":"ALL"}}],"LocalSecondaryIndexOverride":[])");
    BOOST_TEST_REQUIRE(r.status == 200U);
    const auto& t = r.body.at("TableDescription").as_object();
    BOOST_TEST(t.at("GlobalSecondaryIndexes").as_array().size() == 1U);
    BOOST_TEST(!t.contains("LocalSecondaryIndexes"));
    BOOST_TEST(t.at("BillingModeSummary").as_object().at("BillingMode").as_string() ==
               "PAY_PER_REQUEST");
    (void)worker.pass();
    const auto q =
        ok("Query", R"({"TableName":"copy","IndexName":"gsi1","KeyConditionExpression":"v = :v",)"
                    R"("ExpressionAttributeValues":{":v":{"S":"red"}}})");
    BOOST_TEST(q.at("Count").to_number<int>() == 2);
    BOOST_TEST(call("Query",
                    R"({"TableName":"copy","IndexName":"gsi2","KeyConditionExpression":"v = :v",)"
                    R"("ExpressionAttributeValues":{":v":{"S":"red"}}})")
                   .type() == "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(capacity_overrides_apply, backup_fixture) {
    (void)create("tbl");
    const auto arn = backup("tbl");
    (void)worker.pass();
    auto r = restore(arn, "ondemand", R"(,"BillingModeOverride":"PAY_PER_REQUEST")");
    BOOST_TEST_REQUIRE(r.status == 200U);
    BOOST_TEST(r.body.at("TableDescription")
                   .as_object()
                   .at("BillingModeSummary")
                   .as_object()
                   .at("BillingMode")
                   .as_string() == "PAY_PER_REQUEST");
    r = restore(
        arn, "bigger",
        R"(,"ProvisionedThroughputOverride":{"ReadCapacityUnits":50,"WriteCapacityUnits":40})");
    BOOST_TEST_REQUIRE(r.status == 200U);
    const auto& pt =
        r.body.at("TableDescription").as_object().at("ProvisionedThroughput").as_object();
    BOOST_TEST(pt.at("ReadCapacityUnits").to_number<int>() == 50);
    BOOST_TEST(pt.at("WriteCapacityUnits").to_number<int>() == 40);
    BOOST_TEST(
        restore(arn, "conflict",
                R"(,"BillingModeOverride":"PAY_PER_REQUEST",)"
                R"("ProvisionedThroughputOverride":{"ReadCapacityUnits":5,"WriteCapacityUnits":5})")
            .type() == "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(refusals_follow_aws, backup_fixture) {
    (void)create("tbl");
    BOOST_TEST(call("CreateBackup", R"({"TableName":"nosuch","BackupName":"bk1"})").type() ==
               "TableNotFoundException");
    BOOST_TEST(call("CreateBackup", R"({"TableName":"tbl","BackupName":"no spaces"})").type() ==
               "ValidationException");
    const auto arn = backup("tbl");
    // Still being created.
    BOOST_TEST(call("DeleteBackup", R"({"BackupArn":")" + arn + R"("})").type() ==
               "BackupInUseException");
    BOOST_TEST(restore(arn, "copy").type() == "BackupInUseException");
    (void)worker.pass();
    BOOST_TEST(restore(arn, "tbl").type() == "TableAlreadyExistsException");
    const std::string missing = "arn:aws:dynamodb:us-east-1:" + std::string(k_account) +
                                ":table/tbl/backup/01794300000123-deadbeef";
    BOOST_TEST(call("DescribeBackup", R"({"BackupArn":")" + missing + R"("})").type() ==
               "BackupNotFoundException");
    BOOST_TEST(
        call("DescribeBackup", R"({"BackupArn":"arn:aws:dynamodb:us-east-1:1:table/tbl/backup/x"})")
            .type() == "BackupNotFoundException");
    BOOST_TEST(restore(missing, "copy").type() == "BackupNotFoundException");

    // Without a store, nothing but an empty list.
    backups.set_store(nullptr);
    BOOST_TEST(call("CreateBackup", R"({"TableName":"tbl","BackupName":"bk2"})").type() ==
               "ValidationException");
    BOOST_TEST(ok("ListBackups", "{}").at("BackupSummaries").as_array().empty());
}

BOOST_FIXTURE_TEST_CASE(a_deleted_backup_is_gone, backup_fixture) {
    (void)create("tbl");
    put("tbl", "a", 1, "x");
    const auto arn = backup("tbl");
    (void)worker.pass();
    const auto r = ok("DeleteBackup", R"({"BackupArn":")" + arn + R"("})");
    BOOST_TEST(r.at("BackupDescription")
                   .as_object()
                   .at("BackupDetails")
                   .as_object()
                   .at("BackupStatus")
                   .as_string() == "DELETED");
    BOOST_TEST(store->list("backups/").empty());
    BOOST_TEST(call("DescribeBackup", R"({"BackupArn":")" + arn + R"("})").type() ==
               "BackupNotFoundException");
}

BOOST_FIXTURE_TEST_CASE(a_backup_of_a_deleted_table_is_dropped, backup_fixture) {
    (void)create("tbl");
    const auto arn = backup("tbl");
    (void)ok("DeleteTable", R"({"TableName":"tbl"})");
    (void)worker.pass();
    BOOST_TEST(store->list("backups/").empty());
    BOOST_TEST(call("DescribeBackup", R"({"BackupArn":")" + arn + R"("})").type() ==
               "BackupNotFoundException");
}

BOOST_FIXTURE_TEST_CASE(a_damaged_object_stops_the_restore, backup_fixture) {
    (void)create("tbl");
    put("tbl", "a", 1, "x");
    const auto arn = backup("tbl");
    (void)worker.pass();
    for (const auto& k : store->list("backups/")) {
        if (k.find("/data/") != std::string::npos) {
            store->put(k, R"({"Item":{"pk":{"S":"forged"},"sk":{"N":"1"}}})");
        }
    }
    BOOST_TEST_REQUIRE(restore(arn, "copy").status == 200U);
    BOOST_CHECK_THROW((void)worker.pass(), std::runtime_error);
    BOOST_TEST(table("copy").at("TableStatus").as_string() == "CREATING");
}

BOOST_FIXTURE_TEST_CASE(list_backups_filters_and_pages, backup_fixture) {
    (void)create("one");
    (void)create("two");
    const auto a = backup("one", "first");
    clock_ms += 60000;
    const auto b = backup("two", "second");
    clock_ms += 60000;
    const auto c = backup("one", "third");
    (void)worker.pass();
    const auto all = ok("ListBackups", "{}").at("BackupSummaries").as_array();
    BOOST_TEST_REQUIRE(all.size() == 3U);
    BOOST_TEST(all[0].as_object().at("BackupArn").as_string() == a);
    BOOST_TEST(all[0].as_object().at("TableName").as_string() == "one");
    BOOST_TEST(all[0].as_object().at("BackupStatus").as_string() == "AVAILABLE");
    BOOST_TEST(
        ok("ListBackups", R"({"TableName":"one"})").at("BackupSummaries").as_array().size() == 2U);
    // Seconds since the epoch: only the middle one.
    const auto mid =
        ok("ListBackups", R"({"TimeRangeLowerBound":1794300030,"TimeRangeUpperBound":1794300090})")
            .at("BackupSummaries")
            .as_array();
    BOOST_TEST_REQUIRE(mid.size() == 1U);
    BOOST_TEST(mid[0].as_object().at("BackupArn").as_string() == b);
    const auto page = ok("ListBackups", R"({"Limit":2})");
    BOOST_TEST(page.at("BackupSummaries").as_array().size() == 2U);
    const std::string last(page.at("LastEvaluatedBackupArn").as_string());
    BOOST_TEST(last == b);
    const auto rest = ok("ListBackups", R"({"ExclusiveStartBackupArn":")" + last + R"("})");
    BOOST_TEST(rest.at("BackupSummaries").as_array().size() == 1U);
    BOOST_TEST(rest.at("BackupSummaries").as_array()[0].as_object().at("BackupArn").as_string() ==
               c);
    BOOST_TEST(!rest.contains("LastEvaluatedBackupArn"));
    BOOST_TEST(
        ok("ListBackups", R"({"BackupType":"SYSTEM"})").at("BackupSummaries").as_array().empty());
}

BOOST_AUTO_TEST_CASE(stores_keep_what_they_are_given) {
    scratch_dir dir;
    ddb::directory_backup_store files(dir.path / "backups-root");
    kythira::mock_object_store bucket;
    ddb::object_backup_store<kythira::mock_object_store> objects(
        std::make_shared<kythira::mock_object_store>(bucket), "bkt", "ddb/");
    ddb::memory_backup_store memory;
    for (ddb::backup_store* s : std::vector<ddb::backup_store*>{&files, &objects, &memory}) {
        s->put("backups/x/backup.json", "record");
        s->put("backups/x/data/000000.json", "data");
        s->put("other/y", "z");
        auto keys = s->list("backups/");
        std::ranges::sort(keys);
        BOOST_TEST((keys == std::vector<std::string>{"backups/x/backup.json",
                                                     "backups/x/data/000000.json"}));
        BOOST_TEST(s->get("backups/x/backup.json").value() == "record");
        s->remove("backups/x/backup.json");
        BOOST_TEST(!s->get("backups/x/backup.json").has_value());
    }
    // The object store keeps its prefix.
    BOOST_TEST(bucket.has("ddb/backups/x/data/000000.json"));
    BOOST_CHECK_THROW(files.put("../escape", "x"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(restored_descriptors_round_trip) {
    ddb::table_descriptor d;
    d.name = "copy";
    d.table_id = 9;
    d.schema.partition_name = "pk";
    d.restore = ddb::table_restore{"arn:b", "arn:t", 77, true};
    const auto bytes = ddb::encode_table_descriptor(d);
    BOOST_TEST(static_cast<int>(bytes[0]) == 6);
    const auto back = ddb::decode_table_descriptor(bytes);
    BOOST_TEST_REQUIRE(back.has_value());
    BOOST_TEST((*back == d));
    ddb::catalog_command c;
    c.op = ddb::catalog_command::kind::restore_done;
    c.table = d;
    auto encoded = ddb::encode_catalog_command(c);
    bool newer = false;
    BOOST_TEST(ddb::decode_catalog_command(encoded, newer).has_value());
    encoded[0] = '\5';
    BOOST_TEST(!ddb::decode_catalog_command(encoded, newer).has_value());
}
