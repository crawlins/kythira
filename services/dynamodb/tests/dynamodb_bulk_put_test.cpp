// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Bulk put (`.kiro/specs/dynamodb-compatible-api/` task 20, Requirement
// 22.1) over local_backend: StartBulkPutItems into an existing table with
// GSIs, records with conditions, the error and condition-failure reports,
// the rate limit, resuming after the objects already done, cancelling,
// client tokens, listings newest first, and the refusals.

#define BOOST_TEST_MODULE dynamodb_bulk_put_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/backup.hpp>
#include <kythira/dynamodb/bulk_put.hpp>
#include <kythira/dynamodb/export_formats.hpp>
#include <kythira/dynamodb/export_import.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace dynamodb_test;
namespace ef = kythira::dynamodb::export_formats;

namespace {

struct bulk_fixture : fixture {
    std::shared_ptr<ddb::memory_backup_store> store = std::make_shared<ddb::memory_backup_store>();
    ddb::memory_buckets buckets;
    /// The rate limit's clock, moved only by its sleeps.
    std::chrono::steady_clock::time_point fake_now{};
    std::chrono::microseconds slept{0};
    ddb::bulk_put_api<ddb::local_backend<>> api{backend, store, buckets.resolver()};
    ddb::bulk_put_worker<ddb::local_backend<>> worker{backend, store, buckets.resolver(),
                                                      worker_options()};

    bulk_fixture() {
        without_throttling();
        api.register_with(*fe);
        api.on_started([this] { worker.poke(); });
        (void)ok("CreateTable",
                 R"({"TableName":"tbl","BillingMode":"PAY_PER_REQUEST",)"
                 R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
                 R"({"AttributeName":"sk","AttributeType":"N"},)"
                 R"({"AttributeName":"g","AttributeType":"S"}],)"
                 R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
                 R"({"AttributeName":"sk","KeyType":"RANGE"}],)"
                 R"("GlobalSecondaryIndexes":[{"IndexName":"by_g","KeySchema":)"
                 R"([{"AttributeName":"g","KeyType":"HASH"}],)"
                 R"("Projection":{"ProjectionType":"ALL"}}]})");
    }

    auto worker_options() -> ddb::bulk_put_worker_options {
        ddb::bulk_put_worker_options o;
        o.now = [this] { return fake_now; };
        o.sleep = [this](std::chrono::microseconds d) {
            slept += d;
            fake_now += d;
        };
        return o;
    }

    static auto line(const std::string& pk, const std::string& v, const std::string& extra = "")
        -> std::string {
        return R"({"Item":{"pk":{"S":")" + pk + R"("},"sk":{"N":"1"},"v":{"S":")" + v +
               R"("},"g":{"S":"grp"}})" + extra + "}\n";
    }

    auto start(const std::string& prefix, const std::string& extra = "",
               const std::string& format = "DYNAMODB_JSON") -> reply {
        return call("StartBulkPutItems",
                    R"({"TableName":"tbl","InputS3Configuration":{"S3Bucket":"in-bkt",)"
                    R"("S3KeyPrefix":")" +
                        prefix + R"(","InputFormat":")" + format + R"("},)" +
                        R"("OutputS3Configuration":{"S3Bucket":"out-bkt","S3KeyPrefix":"rep",)"
                        R"("OutputType":["ERROR","CONDITION_CHECK_FAILURE"]})" +
                        extra + "}");
    }
    auto run(const std::string& prefix, const std::string& extra = "",
             const std::string& format = "DYNAMODB_JSON") -> json::object {
        const auto r = start(prefix, extra, format);
        BOOST_REQUIRE_MESSAGE(r.status == 200U, r.type() + ": " + r.message());
        (void)worker.pass();
        return describe(arn_of(r));
    }
    static auto arn_of(const reply& r) -> std::string {
        return std::string(
            r.body.at("BulkOperationDescription").as_object().at("BulkOperationArn").as_string());
    }
    auto describe(const std::string& arn) -> json::object {
        return ok("DescribeBulkOperation", R"({"BulkOperationArn":")" + arn + R"("})")
            .at("BulkOperationDescription")
            .as_object();
    }
    auto contents() -> std::map<std::string, std::string> {
        std::map<std::string, std::string> out;
        const auto r = ok("Scan", R"({"TableName":"tbl","ConsistentRead":true})");
        for (const auto& i : r.at("Items").as_array()) {
            const auto& o = i.as_object();
            out[std::string(o.at("pk").as_object().at("S").as_string())] =
                std::string(o.at("v").as_object().at("S").as_string());
        }
        return out;
    }
    /// Every report line under `kind` in the output bucket.
    auto reports(const std::string& kind) -> std::vector<json::object> {
        std::vector<json::object> out;
        auto b = buckets.store("out-bkt");
        for (const auto& key : b->list("rep/AWSDynamoDB/BulkOperations/")) {
            if (key.find("/" + kind + "/") == std::string::npos) {
                continue;
            }
            const auto body = *b->get(key);
            std::string_view rest(body);
            while (!rest.empty()) {
                const auto nl = rest.find('\n');
                out.push_back(json::parse(rest.substr(0, nl)).as_object());
                rest = rest.substr(nl + 1);
            }
        }
        return out;
    }
};

}  // namespace

BOOST_FIXTURE_TEST_CASE(a_bulk_put_writes_through_the_write_path, bulk_fixture) {
    auto in = buckets.store("in-bkt");
    in->put("load/a.json", line("a", "1") + line("b", "1"));
    in->put("load/b.json",
            line("c", "1") + "not json\n" + R"({"Item":{"pk":{"N":"5"},"sk":{"N":"1"}}})" + "\n");
    (void)ok("PutItem", R"({"TableName":"tbl","Item":{"pk":{"S":"keep"},"sk":{"N":"1"},)"
                        R"("v":{"S":"old"}}})");
    in->put("load/c.json",
            line("keep", "new", R"x(,"ConditionExpression":"attribute_not_exists(pk)")x") +
                line("d", "1",
                     R"x(,"ConditionExpression":"attribute_not_exists(#p)",)x"
                     R"("ExpressionAttributeNames":{"#p":"pk"})"));
    const auto d = run("load/", R"(,"ClientToken":"tok","MaxWriteRequestUnits":2048)");
    BOOST_TEST(d.at("BulkOperationStatus").as_string() == "COMPLETED_WITH_ERRORS");
    BOOST_TEST(d.at("ProcessedItemCount").to_number<int>() == 7);
    BOOST_TEST(d.at("FailedItemCount").to_number<int>() == 2);
    BOOST_TEST(d.at("ClientToken").as_string() == "tok");
    BOOST_TEST(d.at("MaxWriteRequestUnits").to_number<int>() == 2048);
    BOOST_TEST(d.at("PerformanceTier").as_string() == "STANDARD");
    BOOST_TEST(d.contains("EndTime"));
    BOOST_TEST(d.at("TotalInputBytes") == d.at("ProcessedInputBytes"));
    BOOST_TEST(d.at("TableDetails").as_array()[0].as_object().at("TableName").as_string() ==
               "arn:aws:dynamodb:us-east-1:" + std::string(k_account) + ":table/tbl");
    BOOST_TEST(
        d.at("BulkOperationUsage").as_array()[0].as_object().at("BulkPutBytes").to_number<int>() >
        0);
    // The condition kept the stored item; the other conditional put went in.
    BOOST_TEST(
        (contents() == std::map<std::string, std::string>{
                           {"a", "1"}, {"b", "1"}, {"c", "1"}, {"d", "1"}, {"keep", "old"}}));
    // The GSI saw the items.
    const auto q = ok("Query", R"({"TableName":"tbl","IndexName":"by_g",)"
                               R"("KeyConditionExpression":"g = :g",)"
                               R"("ExpressionAttributeValues":{":g":{"S":"grp"}}})");
    BOOST_TEST(q.at("Count").to_number<int>() == 4);
    const auto errors = reports("errors");
    BOOST_TEST_REQUIRE(errors.size() == 2U);
    BOOST_TEST(errors[0].at("InputObject").as_string() == "load/b.json");
    BOOST_TEST(errors[0].at("ErrorCode").as_string() == "ParseError");
    BOOST_TEST(errors[1].at("ErrorCode").as_string() == "ValidationException");
    const auto conditions = reports("condition-check-failures");
    BOOST_TEST_REQUIRE(conditions.size() == 1U);
    BOOST_TEST(conditions[0].at("Item").as_object().at("v").as_object().at("S").as_string() ==
               "new");
    BOOST_TEST(
        conditions[0].at("ExistingItem").as_object().at("v").as_object().at("S").as_string() ==
        "old");

    // The same token and request is the same operation; another is refused.
    const auto again = start("load/", R"(,"ClientToken":"tok","MaxWriteRequestUnits":2048)");
    BOOST_TEST(arn_of(again) == std::string(d.at("BulkOperationArn").as_string()));
    BOOST_TEST(start("load/", R"(,"ClientToken":"tok")").type() ==
               "IdempotentParameterMismatchException");
}

BOOST_FIXTURE_TEST_CASE(clean_input_completes_and_ion_reads_with_conditions, bulk_fixture) {
    std::string ion = "$ion_1_0\n";
    ion += R"({Item:{pk:"i1",sk:1.,v:"x",g:"grp"}})"
           "\n";
    ion += R"x({Item:{pk:"i2",sk:1,v:"y",g:"grp"},ConditionExpression:"attribute_not_exists(pk)"})x"
           "\n";
    buckets.store("in-bkt")->put("ion/1.ion", ion);
    const auto d = run("ion/", "", "ION");
    BOOST_TEST(d.at("BulkOperationStatus").as_string() == "COMPLETED");
    BOOST_TEST(d.at("FailedItemCount").to_number<int>() == 0);
    BOOST_TEST((contents() == std::map<std::string, std::string>{{"i1", "x"}, {"i2", "y"}}));
    // A condition failure is not an error.
    const auto e = run("ion/", "", "ION");
    BOOST_TEST(e.at("BulkOperationStatus").as_string() == "COMPLETED");
    BOOST_TEST(reports("condition-check-failures").size() == 1U);
    if (ef::has_gzip()) {
        buckets.store("in-bkt")->put("gz/1.json.gz", ef::gzip(line("z", "9")));
        const auto g = run("gz/", R"(,"InputS3Configuration":{"S3Bucket":"in-bkt",)"
                                  R"("S3KeyPrefix":"gz/","InputFormat":"DYNAMODB_JSON",)"
                                  R"("InputCompressionType":"GZIP"})");
        (void)g;
    }
}

BOOST_FIXTURE_TEST_CASE(the_rate_limit_holds_writes_to_max_write_request_units, bulk_fixture) {
    std::string lines;
    for (int i = 0; i < 3072; ++i) {
        lines += line("p" + std::to_string(i), "v");
    }
    buckets.store("in-bkt")->put("many/1.json", lines);
    const auto d = run("many/", R"(,"MaxWriteRequestUnits":1024)");
    BOOST_TEST(d.at("BulkOperationStatus").as_string() == "COMPLETED");
    // One unit each: the first second's burst, then 1024 a second.
    const auto seconds = std::chrono::duration<double>(slept).count();
    BOOST_TEST(seconds > 1.9);
    BOOST_TEST(seconds < 2.1);
    BOOST_TEST(contents().size() == 3072U);
}

BOOST_FIXTURE_TEST_CASE(a_new_leader_resumes_after_the_objects_done, bulk_fixture) {
    auto in = buckets.store("in-bkt");
    in->put("r/1.json", line("a", "1"));
    in->put("r/2.json", line("b", "1"));
    const auto r = start("r/");
    BOOST_TEST_REQUIRE(r.status == 200U);
    // As if a leader had finished the first object and stopped.
    const auto id = arn_of(r).substr(arn_of(r).rfind('/') + 1);
    auto rec = ddb::bulk_detail::decode(*store->get(ddb::bulk_detail::record_key(id)));
    BOOST_TEST_REQUIRE(rec.has_value());
    rec->done = {"r/1.json"};
    rec->processed_items = 1;
    store->put(ddb::bulk_detail::record_key(id), ddb::bulk_detail::encode(*rec));
    (void)worker.pass();
    const auto d = describe(arn_of(r));
    BOOST_TEST(d.at("BulkOperationStatus").as_string() == "COMPLETED");
    BOOST_TEST(d.at("ProcessedItemCount").to_number<int>() == 2);
    // Only the object not done was read.
    BOOST_TEST((contents() == std::map<std::string, std::string>{{"b", "1"}}));
}

BOOST_FIXTURE_TEST_CASE(cancelling_keeps_what_was_written, bulk_fixture) {
    auto in = buckets.store("in-bkt");
    in->put("c/1.json", line("a", "1"));
    const auto r = start("c/");
    BOOST_TEST_REQUIRE(r.status == 200U);
    const auto c = ok("CancelBulkOperation", R"({"BulkOperationArn":")" + arn_of(r) + R"("})");
    BOOST_TEST(c.at("BulkOperationStatus").as_string() == "CANCELLING");
    BOOST_TEST(describe(arn_of(r)).at("BulkOperationStatus").as_string() == "CANCELLING");
    (void)worker.pass();
    const auto d = describe(arn_of(r));
    BOOST_TEST(d.at("BulkOperationStatus").as_string() == "CANCELLED");
    BOOST_TEST(d.at("FailureCode").as_string() == "UserCancelled");
    BOOST_TEST(contents().empty());
    BOOST_TEST(
        call("CancelBulkOperation", R"({"BulkOperationArn":")" + arn_of(r) + R"("})").type() ==
        "ValidationException");

    // Cancelled mid-object: items before the check stay.
    std::string lines;
    for (int i = 0; i < 600; ++i) {
        lines += line("m" + std::to_string(i), "v");
    }
    in->put("m/1.json", lines);
    const auto r2 = start("m/", R"(,"MaxWriteRequestUnits":100000)");
    BOOST_TEST_REQUIRE(r2.status == 200U);
    std::size_t puts = 0;
    // Cancel at the 300th put, past the worker's first check (256).
    ddb::bulk_put_worker_options o = worker_options();
    const auto arn2 = arn_of(r2);
    o.now = [&] {
        if (++puts == 300) {
            (void)ok("CancelBulkOperation", R"({"BulkOperationArn":")" + arn2 + R"("})");
        }
        return fake_now;
    };
    ddb::bulk_put_worker<ddb::local_backend<>> w2{backend, store, buckets.resolver(), o};
    (void)w2.pass();
    const auto d2 = describe(arn2);
    BOOST_TEST(d2.at("BulkOperationStatus").as_string() == "CANCELLED");
    const auto n = contents().size();
    BOOST_TEST(n == 511U);  // stopped at the check after the cancel
}

BOOST_FIXTURE_TEST_CASE(bulk_refusals_and_listings, bulk_fixture) {
    BOOST_TEST(call("StartBulkPutItems",
                    R"({"TableName":"nope","InputS3Configuration":{"S3Bucket":"in-bkt",)"
                    R"("InputFormat":"DYNAMODB_JSON"},"OutputS3Configuration":{"OutputType":[]}})")
                   .type() == "ResourceNotFoundException");
    BOOST_TEST(call("StartBulkPutItems",
                    R"({"TableName":"tbl","InputS3Configuration":{"S3Bucket":"Bad_Bucket",)"
                    R"("InputFormat":"DYNAMODB_JSON"},"OutputS3Configuration":{"OutputType":[]}})")
                   .type() == "ValidationException");
    BOOST_TEST(call("StartBulkPutItems",
                    R"({"TableName":"tbl","InputS3Configuration":{"S3Bucket":"in-bkt",)"
                    R"("InputFormat":"DYNAMODB_JSON"},"OutputS3Configuration":)"
                    R"({"OutputType":["ERROR"]}})")
                   .type() == "ValidationException");
    const std::string missing = "arn:aws:dynamodb:us-east-1:" + std::string(k_account) +
                                ":table/tbl/bulk-operation/01794300000123-0000abcd";
    BOOST_TEST(
        call("DescribeBulkOperation", R"({"BulkOperationArn":")" + missing + R"("})").type() ==
        "ResourceNotFoundException");
    BOOST_TEST(call("CancelBulkOperation", R"({"BulkOperationArn":")" + missing + R"("})").type() ==
               "ResourceNotFoundException");

    buckets.store("in-bkt")->put("l/1.json", line("a", "1"));
    std::vector<std::string> arns;
    for (int i = 0; i < 3; ++i) {
        clock_ms += 10;
        arns.emplace_back(run("l/").at("BulkOperationArn").as_string());
    }
    const auto all = ok("ListBulkOperations", "{}").at("BulkOperationsSummaryList").as_array();
    BOOST_TEST_REQUIRE(all.size() == 3U);
    // Newest first.
    BOOST_TEST(all[0].as_object().at("BulkOperationArn").as_string() == arns[2]);
    BOOST_TEST(all[0].as_object().at("BulkOperationStatus").as_string() == "COMPLETED");
    const auto page = ok("ListBulkOperations", R"({"MaxResults":2})");
    BOOST_TEST(page.at("BulkOperationsSummaryList").as_array().size() == 2U);
    const auto rest =
        ok("ListBulkOperations", R"({"MaxResults":2,"NextToken":")" +
                                     std::string(page.at("NextToken").as_string()) + R"("})");
    BOOST_TEST(rest.at("BulkOperationsSummaryList").as_array().size() == 1U);
    BOOST_TEST(
        rest.at("BulkOperationsSummaryList").as_array()[0].as_object().at("BulkOperationArn") ==
        json::value(arns[0]));
}

BOOST_AUTO_TEST_CASE(bulk_records_survive_their_encoding) {
    ddb::bulk_record r;
    r.id = "01794300000123-0000abcd";
    r.arn = "arn:aws:dynamodb:us-east-1:1:table/t/bulk-operation/" + r.id;
    r.input = json::object{{"S3Bucket", "b"}};
    r.output = json::object{{"OutputType", json::array{"ERROR"}}};
    r.max_units = 2048;
    r.done = {"x", "y"};
    r.failed_items = 3;
    const auto back = ddb::bulk_detail::decode(ddb::bulk_detail::encode(r));
    BOOST_TEST_REQUIRE(back.has_value());
    BOOST_TEST((*back == r));
    BOOST_TEST(ddb::bulk_detail::units_of(0) == 1U);
    BOOST_TEST(ddb::bulk_detail::units_of(1024) == 1U);
    BOOST_TEST(ddb::bulk_detail::units_of(1025) == 2U);
}
