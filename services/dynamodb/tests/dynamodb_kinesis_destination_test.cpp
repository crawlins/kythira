// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Kinesis streaming destinations (`.kiro/specs/dynamodb-compatible-api/`
// task 17, Requirements 16.1-16.2) over local_backend with a change log
// store: the four actions and their refusals, the publisher's status moves,
// AWS's record format, per-item order across batches and partial failures,
// at-least-once resends, resuming from stored cursors, the stored forms,
// and the SigV4 HTTP sink against a fake endpoint.

#define BOOST_TEST_MODULE dynamodb_kinesis_destination_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/kinesis_destination.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace dynamodb_test;

namespace {

const std::string k_stream = "arn:aws:kinesis:us-east-1:111122223333:stream/cdc";
const std::string k_other_stream = "arn:aws:kinesis:us-east-1:111122223333:stream/other";

/// A Kinesis stand-in: every record put, in order, and failures to inject.
struct fake_kinesis {
    std::vector<std::vector<ddb::kinesis_put_record>> calls;
    std::vector<ddb::kinesis_put_record> landed;
    ddb::kinesis_stream_check check{ddb::kinesis_stream_check::outcome::ok, {}};
    /// Per call (from the next one on), the record indexes to fail.
    std::vector<std::set<std::size_t>> fail;
    /// Calls left that get no answer at all.
    int unreachable = 0;

    auto sink() -> ddb::kinesis_sink {
        return ddb::kinesis_sink{
            .check_stream = [this](const std::string&) { return check; },
            .put_records = [this](const std::string& arn,
                                  const std::vector<ddb::kinesis_put_record>& records)
                -> std::optional<std::vector<bool>> {
                BOOST_TEST(arn == k_stream);
                calls.push_back(records);
                if (unreachable > 0) {
                    --unreachable;
                    return std::nullopt;
                }
                std::set<std::size_t> failing;
                if (!fail.empty()) {
                    failing = fail.front();
                    fail.erase(fail.begin());
                }
                std::vector<bool> out;
                for (std::size_t i = 0; i < records.size(); ++i) {
                    out.push_back(!failing.contains(i));
                    if (out.back()) {
                        landed.push_back(records[i]);
                    }
                }
                return out;
            }};
    }

    /// The landed records' documents.
    [[nodiscard]] auto documents() const -> std::vector<json::object> {
        std::vector<json::object> out;
        for (const auto& r : landed) {
            out.push_back(json::parse(r.data).as_object());
        }
        return out;
    }
};

struct destination_fixture : fixture {
    fake_kinesis kinesis;
    ddb::kinesis_destination_api<ddb::local_backend<>> destinations{backend};
    ddb::kinesis_publisher<ddb::local_backend<>> publisher{backend, kinesis.sink()};

    destination_fixture() {
        without_throttling();
        destinations.register_with(*fe);
    }

    auto enable(const std::string& table, const std::string& arn = k_stream,
                const std::string& extra = "") -> reply {
        return call(
            "EnableKinesisStreamingDestination",
            R"({"TableName":")" + table + R"(","StreamArn":")" + arn + R"(")" + extra + "}");
    }
    auto disable(const std::string& table, const std::string& arn = k_stream) -> reply {
        return call("DisableKinesisStreamingDestination",
                    R"({"TableName":")" + table + R"(","StreamArn":")" + arn + R"("})");
    }
    auto update(const std::string& table, const std::string& precision) -> reply {
        return call("UpdateKinesisStreamingDestination",
                    R"({"TableName":")" + table + R"(","StreamArn":")" + k_stream +
                        R"(","UpdateKinesisStreamingConfiguration":)"
                        R"({"ApproximateCreationDateTimePrecision":")" +
                        precision + R"("}})");
    }
    auto describe_destinations(const std::string& table) -> json::array {
        return ok("DescribeKinesisStreamingDestination", R"({"TableName":")" + table + R"("})")
            .at("KinesisDataStreamDestinations")
            .as_array();
    }
    auto status(const std::string& table) -> std::string {
        const auto list = describe_destinations(table);
        BOOST_TEST_REQUIRE(!list.empty());
        return std::string(list.back().as_object().at("DestinationStatus").as_string());
    }
    /// A table streaming to `k_stream`, ACTIVE.
    void streaming(const std::string& table) {
        (void)create(table);
        BOOST_TEST_REQUIRE(enable(table).status == 200U);
        (void)publisher.pass();
        BOOST_TEST_REQUIRE(status(table) == "ACTIVE");
    }
    auto put(const std::string& table, const std::string& pk, const std::string& v) -> void {
        (void)ok("PutItem", R"({"TableName":")" + table + R"(","Item":{"pk":{"S":")" + pk +
                                R"("},"sk":{"N":"1"},"v":{"S":")" + v + R"("}}})");
    }
    auto remove(const std::string& table, const std::string& pk) -> void {
        (void)ok("DeleteItem", R"({"TableName":")" + table + R"(","Key":{"pk":{"S":")" + pk +
                                   R"("},"sk":{"N":"1"}}})");
    }
    /// "pk=v" per landed record, in order; "pk=-" for a REMOVE.
    [[nodiscard]] auto landed() const -> std::vector<std::string> {
        std::vector<std::string> out;
        for (const auto& doc : kinesis.documents()) {
            const auto& body = doc.at("dynamodb").as_object();
            const std::string pk(
                body.at("Keys").as_object().at("pk").as_object().at("S").as_string());
            const auto* img = body.if_contains("NewImage");
            out.push_back(
                pk + "=" +
                (img == nullptr
                     ? std::string("-")
                     : std::string(img->as_object().at("v").as_object().at("S").as_string())));
        }
        return out;
    }
};

}  // namespace

BOOST_FIXTURE_TEST_CASE(a_destination_moves_through_its_states, destination_fixture) {
    (void)create("tbl");
    auto r = enable("tbl");
    BOOST_TEST_REQUIRE(r.status == 200U);
    BOOST_TEST(r.body.at("DestinationStatus").as_string() == "ENABLING");
    BOOST_TEST(r.body.at("StreamArn").as_string() == k_stream);
    BOOST_TEST(r.body.at("EnableKinesisStreamingConfiguration")
                   .as_object()
                   .at("ApproximateCreationDateTimePrecision")
                   .as_string() == "MILLISECOND");
    BOOST_TEST(status("tbl") == "ENABLING");
    // One stream at a time, as on AWS.
    BOOST_TEST(enable("tbl").type() == "ValidationException");
    BOOST_TEST(enable("tbl", k_other_stream).type() == "ValidationException");
    // Updating waits for the enable to finish.
    BOOST_TEST(update("tbl", "MICROSECOND").type() == "ResourceInUseException");

    (void)publisher.pass();
    BOOST_TEST(status("tbl") == "ACTIVE");

    r = update("tbl", "MICROSECOND");
    BOOST_TEST_REQUIRE(r.status == 200U);
    BOOST_TEST(r.body.at("DestinationStatus").as_string() == "UPDATING");
    BOOST_TEST(status("tbl") == "UPDATING");
    (void)publisher.pass();
    BOOST_TEST(status("tbl") == "ACTIVE");
    BOOST_TEST(describe_destinations("tbl")[0]
                   .as_object()
                   .at("ApproximateCreationDateTimePrecision")
                   .as_string() == "MICROSECOND");
    BOOST_TEST(update("tbl", "MICROSECOND").type() == "ValidationException");

    r = disable("tbl");
    BOOST_TEST_REQUIRE(r.status == 200U);
    BOOST_TEST(r.body.at("DestinationStatus").as_string() == "DISABLING");
    BOOST_TEST(disable("tbl").type() == "ValidationException");
    (void)publisher.pass();
    BOOST_TEST(status("tbl") == "DISABLED");
    BOOST_TEST(disable("tbl").type() == "ValidationException");

    // Enabling again reuses the entry; another stream gets its own.
    BOOST_TEST_REQUIRE(enable("tbl").status == 200U);
    (void)publisher.pass();
    BOOST_TEST_REQUIRE(disable("tbl").status == 200U);
    (void)publisher.pass();
    BOOST_TEST_REQUIRE(enable("tbl", k_other_stream).status == 200U);
    BOOST_TEST(describe_destinations("tbl").size() == 2U);
}

BOOST_FIXTURE_TEST_CASE(requests_are_checked, destination_fixture) {
    (void)create("tbl");
    BOOST_TEST(enable("nosuch").type() == "ResourceNotFoundException");
    BOOST_TEST(call("DescribeKinesisStreamingDestination", R"({"TableName":"nosuch"})").type() ==
               "ResourceNotFoundException");
    BOOST_TEST(enable("tbl", "arn:aws:sqs:us-east-1:111122223333:queue").type() ==
               "ValidationException");
    BOOST_TEST(enable("tbl", "arn:aws:kinesis:us-east-1:111122223333:stream/").type() ==
               "ValidationException");
    // Same account and region only.
    BOOST_TEST(enable("tbl", "arn:aws:kinesis:eu-west-1:111122223333:stream/cdc").type() ==
               "ValidationException");
    auto r = enable("tbl", k_stream,
                    R"(,"EnableKinesisStreamingConfiguration":)"
                    R"({"ApproximateCreationDateTimePrecision":"MICROSECOND"})");
    BOOST_TEST_REQUIRE(r.status == 200U);
    BOOST_TEST(describe_destinations("tbl")[0]
                   .as_object()
                   .at("ApproximateCreationDateTimePrecision")
                   .as_string() == "MICROSECOND");
    BOOST_TEST(ok("DescribeKinesisStreamingDestination", R"({"TableName":"tbl"})")
                   .at("TableName")
                   .as_string() == "tbl");
}

BOOST_FIXTURE_TEST_CASE(a_stream_that_cannot_be_used_fails_the_enable, destination_fixture) {
    (void)create("tbl");
    kinesis.check = {ddb::kinesis_stream_check::outcome::unreachable, {}};
    BOOST_TEST_REQUIRE(enable("tbl").status == 200U);
    (void)publisher.pass();
    BOOST_TEST(status("tbl") == "ENABLING");
    kinesis.check = {ddb::kinesis_stream_check::outcome::refused, "Stream cdc not found"};
    (void)publisher.pass();
    const auto d = describe_destinations("tbl")[0].as_object();
    BOOST_TEST(d.at("DestinationStatus").as_string() == "ENABLE_FAILED");
    BOOST_TEST(d.at("DestinationStatusDescription").as_string() == "Stream cdc not found");
    // A failed destination logs nothing and can be enabled again.
    BOOST_TEST(!backend.describe("tbl")->logs_changes());
    kinesis.check = {ddb::kinesis_stream_check::outcome::ok, {}};
    BOOST_TEST(enable("tbl").status == 200U);
}

BOOST_FIXTURE_TEST_CASE(records_take_the_format_aws_sends, destination_fixture) {
    streaming("tbl");
    put("tbl", "a", "1");
    clock_ms += 5;
    put("tbl", "a", "2");
    remove("tbl", "a");
    BOOST_TEST(publisher.pass() == 3U);
    const auto docs = kinesis.documents();
    BOOST_TEST_REQUIRE(docs.size() == 3U);
    const auto& insert = docs[0];
    BOOST_TEST(insert.at("awsRegion").as_string() == "us-east-1");
    BOOST_TEST(insert.at("eventName").as_string() == "INSERT");
    BOOST_TEST(insert.at("eventSource").as_string() == "aws:dynamodb");
    BOOST_TEST(insert.at("recordFormat").as_string() == "application/json");
    BOOST_TEST(insert.at("tableName").as_string() == "tbl");
    BOOST_TEST(insert.at("userIdentity").is_null());
    BOOST_TEST(insert.at("eventID").as_string().size() == 36U);
    const auto& body = insert.at("dynamodb").as_object();
    BOOST_TEST(body.at("ApproximateCreationDateTime").to_number<std::uint64_t>() ==
               1794300000123ULL);
    BOOST_TEST(body.at("ApproximateCreationDateTimePrecision").as_string() == "MILLISECOND");
    BOOST_TEST(json::serialize(body.at("Keys")) == R"({"pk":{"S":"a"},"sk":{"N":"1"}})");
    BOOST_TEST(body.contains("NewImage"));
    BOOST_TEST(!body.contains("OldImage"));
    BOOST_TEST(body.at("SizeBytes").to_number<std::uint64_t>() > 0U);
    // Both images on a MODIFY, whatever any DynamoDB stream's view type.
    const auto& modify = docs[1].at("dynamodb").as_object();
    BOOST_TEST(docs[1].at("eventName").as_string() == "MODIFY");
    BOOST_TEST(modify.contains("NewImage"));
    BOOST_TEST(modify.contains("OldImage"));
    BOOST_TEST(modify.at("ApproximateCreationDateTime").to_number<std::uint64_t>() ==
               1794300000128ULL);
    BOOST_TEST(docs[2].at("eventName").as_string() == "REMOVE");
    BOOST_TEST(!docs[2].at("dynamodb").as_object().contains("NewImage"));
    // One item, one partition key.
    BOOST_TEST(kinesis.landed[0].partition_key == kinesis.landed[2].partition_key);
    BOOST_TEST(kinesis.landed[0].partition_key.size() == 32U);

    BOOST_TEST_REQUIRE(update("tbl", "MICROSECOND").status == 200U);
    (void)publisher.pass();
    put("tbl", "b", "1");
    (void)publisher.pass();
    const auto micro = kinesis.documents().back().at("dynamodb").as_object();
    BOOST_TEST(micro.at("ApproximateCreationDateTimePrecision").as_string() == "MICROSECOND");
    BOOST_TEST(micro.at("ApproximateCreationDateTime").to_number<std::uint64_t>() ==
               1794300000128000ULL);
}

BOOST_FIXTURE_TEST_CASE(only_writes_made_while_streaming_are_published, destination_fixture) {
    // A DynamoDB stream logs these, but the destination came later.
    (void)create("tbl",
                 R"(,"StreamSpecification":{"StreamEnabled":true,"StreamViewType":"KEYS_ONLY"})");
    (void)create("other",
                 R"(,"StreamSpecification":{"StreamEnabled":true,"StreamViewType":"KEYS_ONLY"})");
    put("tbl", "early", "1");
    clock_ms += 10;
    BOOST_TEST_REQUIRE(enable("tbl").status == 200U);
    (void)publisher.pass();
    put("tbl", "a", "1");
    put("other", "o", "1");  // same change log, another table
    (void)publisher.pass();
    BOOST_TEST((landed() == std::vector<std::string>{"a=1"}));

    // Without a DynamoDB stream the destination alone turns logging on, and
    // a disable turns it off again.
    (void)create("plain");
    BOOST_TEST(!backend.describe("plain")->logs_changes());
    BOOST_TEST_REQUIRE(enable("plain").status == 200U);
    BOOST_TEST(backend.describe("plain")->logs_changes());
    BOOST_TEST_REQUIRE(disable("plain").status == 200U);
    BOOST_TEST(!backend.describe("plain")->logs_changes());
}

BOOST_FIXTURE_TEST_CASE(a_call_never_carries_two_records_of_one_item, destination_fixture) {
    streaming("tbl");
    put("tbl", "a", "1");
    put("tbl", "b", "1");
    put("tbl", "a", "2");
    put("tbl", "c", "1");
    BOOST_TEST(publisher.pass() == 4U);
    BOOST_TEST_REQUIRE(kinesis.calls.size() == 2U);
    BOOST_TEST(kinesis.calls[0].size() == 2U);  // a=1, b=1
    BOOST_TEST(kinesis.calls[1].size() == 2U);  // a=2, c=1
    BOOST_TEST((landed() == std::vector<std::string>{"a=1", "b=1", "a=2", "c=1"}));
    // Nothing new: no call.
    BOOST_TEST(publisher.pass() == 0U);
    BOOST_TEST(kinesis.calls.size() == 2U);
}

BOOST_FIXTURE_TEST_CASE(a_failed_record_is_resent_without_reordering_its_item,
                        destination_fixture) {
    streaming("tbl");
    put("tbl", "a", "1");
    put("tbl", "b", "1");
    put("tbl", "c", "1");
    put("tbl", "b", "2");
    // b=1 fails; a=1 and c=1 land.
    kinesis.fail = {{1}};
    (void)publisher.pass();
    // The pass stopped at b=1: nothing after it went out.
    BOOST_TEST((landed() == std::vector<std::string>{"a=1", "c=1"}));
    (void)publisher.pass();
    // b=1 again, then c=1 a second time (at least once), then b=2.
    BOOST_TEST((landed() == std::vector<std::string>{"a=1", "c=1", "b=1", "c=1", "b=2"}));

    // Per item, the first sighting of each record keeps write order.
    std::map<std::string, std::vector<std::string>> per_item;
    for (const auto& r : landed()) {
        auto& seen = per_item[r.substr(0, 1)];
        if (seen.empty() || seen.back() != r) {
            seen.push_back(r);
        }
    }
    BOOST_TEST((per_item["b"] == std::vector<std::string>{"b=1", "b=2"}));
}

BOOST_FIXTURE_TEST_CASE(an_unreachable_endpoint_loses_nothing, destination_fixture) {
    streaming("tbl");
    put("tbl", "a", "1");
    put("tbl", "b", "1");
    kinesis.unreachable = 1;
    BOOST_TEST(publisher.pass() == 0U);
    BOOST_TEST(landed().empty());
    BOOST_TEST(publisher.pass() == 2U);
    BOOST_TEST((landed() == std::vector<std::string>{"a=1", "b=1"}));
}

BOOST_FIXTURE_TEST_CASE(a_new_publisher_resumes_from_the_stored_cursors, destination_fixture) {
    streaming("tbl");
    put("tbl", "a", "1");
    put("tbl", "b", "1");
    BOOST_TEST(publisher.pass() == 2U);
    const auto d = backend.describe("tbl");
    BOOST_TEST_REQUIRE(d->destinations.size() == 1U);
    BOOST_TEST(d->destinations[0].cursors.size() == 1U);

    // Another node takes over: it sends only what came after.
    fake_kinesis other;
    ddb::kinesis_publisher<ddb::local_backend<>> next{backend, other.sink()};
    BOOST_TEST(next.pass() == 0U);
    put("tbl", "c", "1");
    BOOST_TEST(next.pass() == 1U);
    BOOST_TEST_REQUIRE(other.landed.size() == 1U);
    BOOST_TEST(
        json::parse(other.landed[0].data).at("dynamodb").at("Keys").at("pk").at("S").as_string() ==
        "c");
}

BOOST_FIXTURE_TEST_CASE(a_pass_is_bounded, destination_fixture) {
    ddb::kinesis_publisher_options o;
    o.max_records_per_pass = 3;
    o.page_records = 2;
    ddb::kinesis_publisher<ddb::local_backend<>> small{backend, kinesis.sink(), o};
    (void)create("tbl");
    BOOST_TEST_REQUIRE(enable("tbl").status == 200U);
    (void)small.pass();
    for (const auto* pk : {"a", "b", "c", "d", "e"}) {
        put("tbl", pk, "1");
    }
    BOOST_TEST(small.pass() == 3U);
    BOOST_TEST(small.pass() == 2U);
    BOOST_TEST((landed() == std::vector<std::string>{"a=1", "b=1", "c=1", "d=1", "e=1"}));
}

BOOST_FIXTURE_TEST_CASE(ttl_deletes_carry_the_service_identity, destination_fixture) {
    streaming("tbl");
    BOOST_TEST_REQUIRE(call("UpdateTimeToLive", R"({"TableName":"tbl","TimeToLiveSpecification":)"
                                                R"({"Enabled":true,"AttributeName":"exp"}})")
                           .status == 200U);
    (void)ok("PutItem", R"({"TableName":"tbl","Item":{"pk":{"S":"a"},"sk":{"N":"1"},)"
                        R"("exp":{"N":"1700000000"}}})");
    BOOST_TEST_REQUIRE(sweeper.pass() == 1U);
    (void)publisher.pass();
    const auto docs = kinesis.documents();
    BOOST_TEST_REQUIRE(docs.size() == 2U);
    BOOST_TEST(docs[1].at("eventName").as_string() == "REMOVE");
    const auto& who = docs[1].at("userIdentity").as_object();
    BOOST_TEST(who.at("principalId").as_string() == "dynamodb.amazonaws.com");
    BOOST_TEST(who.at("type").as_string() == "Service");
}

BOOST_AUTO_TEST_CASE(descriptors_with_destinations_round_trip) {
    ddb::table_descriptor d;
    d.name = "tbl";
    d.table_id = 7;
    d.schema.partition_name = "pk";
    BOOST_TEST(static_cast<int>(ddb::encode_table_descriptor(d)[0]) == 3);
    ddb::kinesis_destination k;
    k.stream_arn = k_stream;
    k.status = ddb::destination_status::active;
    k.precision = ddb::time_precision::microsecond;
    k.enabled_ms = 42;
    k.cursors = {{"stream-a", 5}, {"stream-b", 9}};
    d.destinations.push_back(k);
    const auto bytes = ddb::encode_table_descriptor(d);
    BOOST_TEST(static_cast<int>(bytes[0]) == 5);
    const auto back = ddb::decode_table_descriptor(bytes);
    BOOST_TEST_REQUIRE(back.has_value());
    BOOST_TEST((*back == d));

    ddb::catalog_command c;
    c.op = ddb::catalog_command::kind::destination_progress;
    c.table = d;
    bool newer = false;
    const auto cmd = ddb::decode_catalog_command(ddb::encode_catalog_command(c), newer);
    BOOST_TEST_REQUIRE(cmd.has_value());
    BOOST_TEST((*cmd == c));
    // A version-4 form cannot carry the new commands.
    auto old = ddb::encode_catalog_command(c);
    old[0] = '\4';
    BOOST_TEST(!ddb::decode_catalog_command(old, newer).has_value());
}

BOOST_AUTO_TEST_CASE(the_http_sink_signs_and_reads_kinesis_answers) {
    std::vector<std::pair<std::string, std::string>> sent_headers;
    std::string sent_body;
    std::string sent_to;
    std::optional<ddb::kinesis_http_answer> answer;
    ddb::kinesis_http_sink_config cfg;
    cfg.endpoint = "kinesis.local:4567";
    cfg.tls = false;
    cfg.credentials = [] { return ddb::kinesis_credentials{"AKIDK", "secret", "token"}; };
    cfg.clock = [] { return k_now; };
    ddb::kinesis_http_sink sink(
        cfg, [&](const std::string& to, const std::vector<std::pair<std::string, std::string>>& h,
                 const std::string& body) {
            sent_to = to;
            sent_headers = h;
            sent_body = body;
            return answer;
        });
    const auto header = [&](const std::string& name) {
        for (const auto& [k, v] : sent_headers) {
            if (k == name) {
                return v;
            }
        }
        return std::string{};
    };

    answer = ddb::kinesis_http_answer{
        200, R"({"FailedRecordCount":1,"Records":[{"SequenceNumber":"1","ShardId":"s"},)"
             R"({"ErrorCode":"ProvisionedThroughputExceededException","ErrorMessage":"x"}]})"};
    const auto got = sink.put_records(k_stream, {{"one", "pk1"}, {"two", "pk2"}});
    BOOST_TEST_REQUIRE(got.has_value());
    BOOST_TEST((*got == std::vector<bool>{true, false}));
    BOOST_TEST(sent_to == "kinesis.local:4567");
    BOOST_TEST(header("x-amz-target") == "Kinesis_20131202.PutRecords");
    BOOST_TEST(header("content-type") == "application/x-amz-json-1.1");
    BOOST_TEST(header("x-amz-security-token") == "token");
    BOOST_TEST(header("Authorization")
                   .starts_with("AWS4-HMAC-SHA256 Credential=AKIDK/20261110/us-east-1/kinesis/"
                                "aws4_request, SignedHeaders=content-type;host;x-amz-date;"
                                "x-amz-security-token;x-amz-target, Signature="));
    const auto req = json::parse(sent_body).as_object();
    BOOST_TEST(req.at("StreamARN").as_string() == k_stream);
    BOOST_TEST(req.at("Records").as_array()[0].at("Data").as_string() == "b25l");
    BOOST_TEST(req.at("Records").as_array()[1].at("PartitionKey").as_string() == "pk2");

    answer = ddb::kinesis_http_answer{500, "{}"};
    BOOST_TEST(!sink.put_records(k_stream, {{"one", "pk1"}}).has_value());
    answer.reset();
    BOOST_TEST(!sink.put_records(k_stream, {{"one", "pk1"}}).has_value());

    answer = ddb::kinesis_http_answer{200, R"({"StreamDescriptionSummary":{}})"};
    BOOST_TEST((sink.check_stream(k_stream).result == ddb::kinesis_stream_check::outcome::ok));
    BOOST_TEST(header("x-amz-target") == "Kinesis_20131202.DescribeStreamSummary");
    answer = ddb::kinesis_http_answer{
        400, R"({"__type":"ResourceNotFoundException","message":"Stream cdc not found."})"};
    const auto missing = sink.check_stream(k_stream);
    BOOST_TEST((missing.result == ddb::kinesis_stream_check::outcome::refused));
    BOOST_TEST(missing.description == "Stream cdc not found.");
    answer = ddb::kinesis_http_answer{500, R"({"__type":"InternalFailure"})"};
    BOOST_TEST(
        (sink.check_stream(k_stream).result == ddb::kinesis_stream_check::outcome::unreachable));

    // Without an endpoint the stream's region picks AWS's.
    cfg.endpoint.clear();
    cfg.tls = true;
    ddb::kinesis_http_sink aws(cfg, [&](const std::string& to, const auto&, const auto&) {
        sent_to = to;
        return std::optional<ddb::kinesis_http_answer>{};
    });
    (void)aws.check_stream("arn:aws:kinesis:eu-west-1:111122223333:stream/cdc");
    BOOST_TEST(sent_to == "kinesis.eu-west-1.amazonaws.com:443");
}
