// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// DynamoDB Streams (`.kiro/specs/dynamodb-compatible-api/` task 16,
// Requirements 14.2-14.4) over local_backend with a change log store:
// StreamSpecification on CreateTable and UpdateTable, the four Streams
// actions with DynamoDB Local's answers, records per view type, streams
// that never see each other's records, retention, iterator expiry, and
// the stored forms that carry a stream.

#define BOOST_TEST_MODULE dynamodb_streams_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/change_log.hpp>
#include <kythira/dynamodb/compat_profile.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/streams.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace dynamodb_test;

namespace {

constexpr std::uint64_t k_hour_ms = std::uint64_t{3600} * 1000;

const std::string k_new_and_old =
    R"(,"StreamSpecification":{"StreamEnabled":true,"StreamViewType":"NEW_AND_OLD_IMAGES"})";

struct streams_fixture : fixture {
    streams_fixture() { without_throttling(); }

    auto stream_arn(const std::string& table) -> std::string {
        return std::string(ok("DescribeTable", R"({"TableName":")" + table + R"("})")
                               .at("Table")
                               .as_object()
                               .at("LatestStreamArn")
                               .as_string());
    }
    auto update_stream(const std::string& table, const std::string& spec) -> reply {
        return call("UpdateTable",
                    R"({"TableName":")" + table + R"(","StreamSpecification":)" + spec + "}");
    }
    auto shards(const std::string& arn) -> json::array {
        return ok("DescribeStream", R"({"StreamArn":")" + arn + R"("})")
            .at("StreamDescription")
            .as_object()
            .at("Shards")
            .as_array();
    }
    auto iterator(const std::string& arn, const std::string& shard, const std::string& type,
                  const std::string& seq = "") -> std::string {
        std::string body = R"({"StreamArn":")" + arn + R"(","ShardId":")" + shard +
                           R"(","ShardIteratorType":")" + type + R"(")";
        if (!seq.empty()) {
            body += R"(,"SequenceNumber":")" + seq + R"(")";
        }
        return std::string(ok("GetShardIterator", body + "}").at("ShardIterator").as_string());
    }
    auto records(const std::string& it) -> json::object {
        return ok("GetRecords", R"({"ShardIterator":")" + it + R"("})");
    }
    /// Every record of the stream's only shard, from its start.
    auto all_records(const std::string& table) -> json::array {
        const auto arn = stream_arn(table);
        const auto s = shards(arn);
        BOOST_TEST_REQUIRE(s.size() == 1U);
        const std::string id(s[0].as_object().at("ShardId").as_string());
        return records(iterator(arn, id, "TRIM_HORIZON")).at("Records").as_array();
    }
    auto put(const std::string& table, const std::string& pk, const std::string& v) -> void {
        (void)ok("PutItem", R"({"TableName":")" + table + R"(","Item":{"pk":{"S":")" + pk +
                                R"("},"sk":{"N":"1"},"v":{"S":")" + v + R"("}}})");
    }
    auto remove(const std::string& table, const std::string& pk) -> void {
        (void)ok("DeleteItem", R"({"TableName":")" + table + R"(","Key":{"pk":{"S":")" + pk +
                                   R"("},"sk":{"N":"1"}}})");
    }
    static auto event_names(const json::array& records) -> std::vector<std::string> {
        std::vector<std::string> out;
        for (const auto& r : records) {
            out.emplace_back(r.as_object().at("eventName").as_string());
        }
        return out;
    }
};

}  // namespace

BOOST_FIXTURE_TEST_CASE(stream_specifications_are_checked_as_dynamodb_local_checks_them,
                        streams_fixture) {
    const auto create_with = [&](const std::string& spec) {
        return call(
            "CreateTable",
            R"({"TableName":"bad","AttributeDefinitions":[{"AttributeName":"pk",)"
            R"("AttributeType":"S"}],"KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],)"
            R"("BillingMode":"PAY_PER_REQUEST","StreamSpecification":)" +
                spec + "}");
    };
    auto r = create_with(R"({"StreamEnabled":true})");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.message() ==
               "Invalid StreamViewType: Stream ViewType must not be null or empty when enabling "
               "streams.");
    r = create_with(R"({"StreamEnabled":false,"StreamViewType":"KEYS_ONLY"})");
    BOOST_TEST(r.message() ==
               "Invalid StreamViewType: Stream StreamEnabled was false but a StreamViewType was "
               "present.");
    // Disabled at creation: no stream at all.
    r = create_with(R"({"StreamEnabled":false})");
    BOOST_TEST_REQUIRE(r.status == 200U);
    const auto& d = r.body.at("TableDescription").as_object();
    BOOST_TEST(!d.contains("StreamSpecification"));
    BOOST_TEST(!d.contains("LatestStreamArn"));
}

BOOST_FIXTURE_TEST_CASE(a_stream_shows_on_the_table_and_outlives_its_disable, streams_fixture) {
    const auto created = create("tbl", k_new_and_old).at("TableDescription").as_object();
    const auto& spec = created.at("StreamSpecification").as_object();
    BOOST_TEST(spec.at("StreamEnabled").as_bool());
    BOOST_TEST(spec.at("StreamViewType").as_string() == "NEW_AND_OLD_IMAGES");
    // The label is the creation time, to the millisecond, UTC.
    BOOST_TEST(created.at("LatestStreamLabel").as_string() == "2026-11-10T08:40:00.123");
    const std::string arn(created.at("LatestStreamArn").as_string());
    BOOST_TEST(arn ==
               std::string(created.at("TableArn").as_string()) + "/stream/2026-11-10T08:40:00.123");

    auto r = update_stream("tbl", R"({"StreamEnabled":true,"StreamViewType":"KEYS_ONLY"})");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.message() == "Table already has an enabled stream");
    r = call("UpdateTable",
             R"({"TableName":"tbl","StreamSpecification":{"StreamEnabled":false},)"
             R"("ProvisionedThroughput":{"ReadCapacityUnits":6,"WriteCapacityUnits":6}})");
    BOOST_TEST(r.message() == "Cannot change IOPS and Streams at the same time.");
    // The model requires StreamEnabled, so the codec refuses this one.
    r = update_stream("tbl", R"({})");
    BOOST_TEST(r.type() == "ValidationException");

    clock_ms += 1000;
    r = update_stream("tbl", R"({"StreamEnabled":false})");
    BOOST_TEST_REQUIRE(r.status == 200U);
    const auto& after = r.body.at("TableDescription").as_object();
    BOOST_TEST(after.at("TableStatus").as_string() == "ACTIVE");
    BOOST_TEST(!after.contains("StreamSpecification"));
    BOOST_TEST(after.at("LatestStreamArn").as_string() == arn);
    r = update_stream("tbl", R"({"StreamEnabled":false})");
    BOOST_TEST(r.message() == "Table has no stream to disable");

    // Enabling again makes a new stream with a new label.
    clock_ms += 1000;
    r = update_stream("tbl", R"({"StreamEnabled":true,"StreamViewType":"KEYS_ONLY"})");
    BOOST_TEST_REQUIRE(r.status == 200U);
    BOOST_TEST(r.body.at("TableDescription").as_object().at("LatestStreamLabel").as_string() ==
               "2026-11-10T08:40:02.123");
}

BOOST_FIXTURE_TEST_CASE(list_streams_answers_newest_first_and_pages, streams_fixture) {
    (void)create("tbl", k_new_and_old);
    clock_ms += 1000;
    BOOST_TEST_REQUIRE(update_stream("tbl", R"({"StreamEnabled":false})").status == 200U);
    clock_ms += 1000;
    BOOST_TEST_REQUIRE(
        update_stream("tbl", R"({"StreamEnabled":true,"StreamViewType":"NEW_IMAGE"})").status ==
        200U);
    (void)create("other", k_new_and_old);

    const auto mine = ok("ListStreams", R"({"TableName":"tbl"})").at("Streams").as_array();
    BOOST_TEST_REQUIRE(mine.size() == 2U);
    BOOST_TEST(mine[0].as_object().at("StreamLabel").as_string() == "2026-11-10T08:40:02.123");
    BOOST_TEST(mine[1].as_object().at("StreamLabel").as_string() == "2026-11-10T08:40:00.123");
    BOOST_TEST(mine[0].as_object().at("TableName").as_string() == "tbl");
    BOOST_TEST(ok("ListStreams", R"({"TableName":"nosuch"})").at("Streams").as_array().empty());

    const auto first = ok("ListStreams", R"({"Limit":1})");
    BOOST_TEST(first.at("Streams").as_array().size() == 1U);
    const std::string cursor(first.at("LastEvaluatedStreamArn").as_string());
    const auto rest = ok("ListStreams", R"({"ExclusiveStartStreamArn":")" + cursor + R"("})");
    BOOST_TEST(rest.at("Streams").as_array().size() == 2U);
    BOOST_TEST(!rest.contains("LastEvaluatedStreamArn"));

    // A disabled stream drops out once its records have expired.
    clock_ms += std::uint64_t{25} * k_hour_ms;
    BOOST_TEST(ok("ListStreams", R"({"TableName":"tbl"})").at("Streams").as_array().size() == 1U);
}

BOOST_FIXTURE_TEST_CASE(records_carry_the_images_the_view_type_asks_for, streams_fixture) {
    (void)create("tbl", k_new_and_old);
    put("tbl", "a", "1");
    put("tbl", "a", "2");
    put("tbl", "a", "2");  // changes nothing: no record
    remove("tbl", "a");
    remove("tbl", "a");  // deletes nothing: no record
    const auto recs = all_records("tbl");
    BOOST_TEST((event_names(recs) == std::vector<std::string>{"INSERT", "MODIFY", "REMOVE"}));
    const auto& insert = recs[0].as_object();
    BOOST_TEST(insert.at("eventVersion").as_string() == "1.1");
    BOOST_TEST(insert.at("eventSource").as_string() == "aws:dynamodb");
    const auto& body = insert.at("dynamodb").as_object();
    BOOST_TEST(body.at("Keys").as_object().size() == 2U);
    BOOST_TEST(body.at("NewImage").as_object().at("v").as_object().at("S").as_string() == "1");
    BOOST_TEST(!body.contains("OldImage"));
    BOOST_TEST(body.at("StreamViewType").as_string() == "NEW_AND_OLD_IMAGES");
    BOOST_TEST(body.at("SequenceNumber").as_string().size() == 21U);
    // Rounded down to the minute, in seconds.
    BOOST_TEST(body.at("ApproximateCreationDateTime").to_number<double>() == 1794300000.0);
    const auto& modify = recs[1].as_object().at("dynamodb").as_object();
    BOOST_TEST(modify.at("OldImage").as_object().at("v").as_object().at("S").as_string() == "1");
    BOOST_TEST(modify.at("NewImage").as_object().at("v").as_object().at("S").as_string() == "2");
    const auto& remove_body = recs[2].as_object().at("dynamodb").as_object();
    BOOST_TEST(!remove_body.contains("NewImage"));
    BOOST_TEST(remove_body.contains("OldImage"));
    // Sequence numbers grow, and event ids differ.
    BOOST_TEST(modify.at("SequenceNumber").as_string() > body.at("SequenceNumber").as_string());
    BOOST_TEST(recs[0].as_object().at("eventID") != recs[1].as_object().at("eventID"));

    (void)create("keys",
                 R"(,"StreamSpecification":{"StreamEnabled":true,"StreamViewType":"KEYS_ONLY"})");
    put("keys", "k", "1");
    const auto k = all_records("keys");
    BOOST_TEST_REQUIRE(k.size() == 1U);
    const auto& kb = k[0].as_object().at("dynamodb").as_object();
    BOOST_TEST(!kb.contains("NewImage"));
    BOOST_TEST(!kb.contains("OldImage"));
    // The other table's records are not in this table's stream.
    BOOST_TEST(all_records("tbl").size() == 3U);
}

BOOST_FIXTURE_TEST_CASE(iterators_start_where_their_type_says, streams_fixture) {
    (void)create("tbl", k_new_and_old);
    put("tbl", "a", "1");
    put("tbl", "b", "1");
    const auto arn = stream_arn("tbl");
    const auto s = shards(arn);
    BOOST_TEST_REQUIRE(s.size() == 1U);
    const auto& shard = s[0].as_object();
    const std::string id(shard.at("ShardId").as_string());
    BOOST_TEST(id.starts_with("shardId-"));
    BOOST_TEST(id.size() == std::string("shardId-00000000000000000000-00000000").size());
    BOOST_TEST(!shard.at("SequenceNumberRange").as_object().contains("EndingSequenceNumber"));
    BOOST_TEST(!shard.contains("ParentShardId"));

    const auto recs = records(iterator(arn, id, "TRIM_HORIZON")).at("Records").as_array();
    BOOST_TEST_REQUIRE(recs.size() == 2U);
    const std::string first(
        recs[0].as_object().at("dynamodb").as_object().at("SequenceNumber").as_string());
    const std::string second(
        recs[1].as_object().at("dynamodb").as_object().at("SequenceNumber").as_string());
    BOOST_TEST(
        records(iterator(arn, id, "AT_SEQUENCE_NUMBER", second)).at("Records").as_array().size() ==
        1U);
    BOOST_TEST(records(iterator(arn, id, "AFTER_SEQUENCE_NUMBER", first))
                   .at("Records")
                   .as_array()
                   .size() == 1U);

    // LATEST sees only what comes after it, and the reply always carries
    // the next iterator while the shard is open.
    const auto latest = records(iterator(arn, id, "LATEST"));
    BOOST_TEST(latest.at("Records").as_array().empty());
    put("tbl", "c", "1");
    const auto next = records(std::string(latest.at("NextShardIterator").as_string()));
    BOOST_TEST_REQUIRE(next.at("Records").as_array().size() == 1U);
    BOOST_TEST(next.contains("NextShardIterator"));

    // Refusals, in DynamoDB Local's words.
    auto r = call("GetShardIterator", R"({"StreamArn":")" + arn + R"(","ShardId":")" + id +
                                          R"(","ShardIteratorType":"TRIM_HORIZON",)"
                                          R"("SequenceNumber":")" +
                                          first + R"("})");
    BOOST_TEST(r.message() ==
               "One or more parameter values were invalid: SequenceNumber should not be passed "
               "for this iterator type");
    r = call("GetShardIterator", R"({"StreamArn":")" + arn +
                                     R"(","ShardId":"shardId-00000000000000000009-00000000",)"
                                     R"("ShardIteratorType":"TRIM_HORIZON"})");
    BOOST_TEST(r.type() == "ResourceNotFoundException");
    BOOST_TEST(r.message() == "Shard does not exist");
    r = call("GetShardIterator", R"({"StreamArn":")" + arn + R"(","ShardId":")" + id +
                                     R"(","ShardIteratorType":"AT_SEQUENCE_NUMBER",)"
                                     R"("SequenceNumber":"999999999999999999999"})");
    BOOST_TEST(r.message() == "One or more parameter values were invalid: Invalid SequenceNumber");
    r = call("GetRecords", R"({"ShardIterator":")" + arn + R"(|001|garbage"})");
    BOOST_TEST(r.message() == "Invalid ShardIterator");
    const auto missing = arn.substr(0, arn.find(":table/")) + ":table/nope/stream/x";
    r = call("DescribeStream", R"({"StreamArn":")" + missing + R"("})");
    BOOST_TEST(r.type() == "ResourceNotFoundException");
    BOOST_TEST(r.message() == "Requested resource not found: Stream: " + missing + " not found");
}

BOOST_FIXTURE_TEST_CASE(each_stream_sees_only_its_own_records, streams_fixture) {
    (void)create("tbl", k_new_and_old);
    const auto old_arn = stream_arn("tbl");
    put("tbl", "before", "1");
    clock_ms += 1000;
    BOOST_TEST_REQUIRE(update_stream("tbl", R"({"StreamEnabled":false})").status == 200U);
    put("tbl", "between", "1");  // no stream: not logged at all
    clock_ms += 1000;
    BOOST_TEST_REQUIRE(
        update_stream("tbl", R"({"StreamEnabled":true,"StreamViewType":"NEW_IMAGE"})").status ==
        200U);
    put("tbl", "after", "1");
    const auto new_arn = stream_arn("tbl");
    BOOST_TEST(new_arn != old_arn);

    const auto read_all = [&](const std::string& arn) {
        const auto d = ok("DescribeStream", R"({"StreamArn":")" + arn + R"("})")
                           .at("StreamDescription")
                           .as_object();
        std::vector<std::string> keys;
        bool ended = false;
        for (const auto& s : d.at("Shards").as_array()) {
            const std::string id(s.as_object().at("ShardId").as_string());
            const auto page = records(iterator(arn, id, "TRIM_HORIZON"));
            for (const auto& r : page.at("Records").as_array()) {
                keys.emplace_back(r.as_object()
                                      .at("dynamodb")
                                      .as_object()
                                      .at("Keys")
                                      .as_object()
                                      .at("pk")
                                      .as_object()
                                      .at("S")
                                      .as_string());
            }
            ended = !page.contains("NextShardIterator");
        }
        return std::pair{keys, ended};
    };
    const auto [old_keys, old_ended] = read_all(old_arn);
    BOOST_TEST((old_keys == std::vector<std::string>{"before"}));
    // A disabled stream's shard ends once read to its last record.
    BOOST_TEST(old_ended);
    const auto old_desc = ok("DescribeStream", R"({"StreamArn":")" + old_arn + R"("})")
                              .at("StreamDescription")
                              .as_object();
    BOOST_TEST(old_desc.at("StreamStatus").as_string() == "DISABLED");
    BOOST_TEST(old_desc.at("Shards")
                   .as_array()[0]
                   .as_object()
                   .at("SequenceNumberRange")
                   .as_object()
                   .contains("EndingSequenceNumber"));
    const auto [new_keys, new_ended] = read_all(new_arn);
    BOOST_TEST((new_keys == std::vector<std::string>{"after"}));
    BOOST_TEST(!new_ended);
}

BOOST_FIXTURE_TEST_CASE(a_ttl_delete_carries_the_service_identity, streams_fixture) {
    sweeper.set_compat_profile(ddb::cluster_profile({{"ttl-delay", "prompt"}}).value());
    (void)create("tbl", k_new_and_old);
    (void)ok(
        "UpdateTimeToLive",
        R"({"TableName":"tbl","TimeToLiveSpecification":{"Enabled":true,"AttributeName":"exp"}})");
    (void)ok("PutItem", R"({"TableName":"tbl","Item":{"pk":{"S":"a"},"sk":{"N":"1"},"exp":{"N":")" +
                            std::to_string(clock_ms / 1000 - 10) + R"("}}})");
    BOOST_TEST(sweeper.pass() == 1U);
    const auto recs = all_records("tbl");
    BOOST_TEST_REQUIRE(recs.size() == 2U);
    BOOST_TEST(!recs[0].as_object().contains("userIdentity"));
    const auto& who = recs[1].as_object().at("userIdentity").as_object();
    BOOST_TEST(who.at("PrincipalId").as_string() == "dynamodb.amazonaws.com");
    BOOST_TEST(who.at("Type").as_string() == "Service");
}

BOOST_FIXTURE_TEST_CASE(records_expire_after_a_day, streams_fixture) {
    (void)create("tbl", k_new_and_old);
    const auto arn = stream_arn("tbl");
    put("tbl", "old", "1");
    const std::string id(shards(arn)[0].as_object().at("ShardId").as_string());
    const auto stale = iterator(arn, id, "TRIM_HORIZON");
    // Two hours later the segment rolls by age, so the first one seals.
    clock_ms += 2 * k_hour_ms;
    put("tbl", "new", "1");
    clock_ms += 23 * k_hour_ms;
    backend.trim_changes();
    const auto recs = records(iterator(arn, id, "TRIM_HORIZON")).at("Records").as_array();
    BOOST_TEST_REQUIRE(recs.size() == 1U);
    BOOST_TEST(recs[0].as_object().at("dynamodb").as_object().at("Keys").as_object().at("pk") ==
               json::value({{"S", "new"}}));
    // An iterator into what was trimmed is refused, as AWS refuses it...
    clock_ms -= 25 * k_hour_ms;
    auto r = call("GetRecords", R"({"ShardIterator":")" + stale + R"("})");
    BOOST_TEST(r.type() == "TrimmedDataAccessException");
    // ...and one older than 15 minutes has expired.
    clock_ms += 25 * k_hour_ms;
    r = call("GetRecords", R"({"ShardIterator":")" + stale + R"("})");
    BOOST_TEST(r.type() == "ExpiredIteratorException");
}

BOOST_AUTO_TEST_CASE(the_stored_forms_carry_streams_and_stay_old_without_one) {
    ddb::table_descriptor d;
    d.name = "tbl";
    d.table_id = 7;
    d.schema.partition_name = "pk";
    BOOST_TEST(static_cast<int>(ddb::encode_table_descriptor(d)[0]) == 3);
    d.streams = {ddb::table_stream{100, ddb::stream_view_type::new_image, 200},
                 ddb::table_stream{300, ddb::stream_view_type::keys_only, 0}};
    const auto bytes = ddb::encode_table_descriptor(d);
    BOOST_TEST(static_cast<int>(bytes[0]) == 4);
    const auto back = ddb::decode_table_descriptor(bytes);
    BOOST_TEST_REQUIRE(back.has_value());
    BOOST_TEST((*back == d));
    BOOST_TEST(back->logs_changes());
    BOOST_TEST(back->stream_label() == 300U);

    ddb::catalog_command c;
    c.op = ddb::catalog_command::kind::update_table;
    c.table = d;
    c.update_fields = ddb::catalog_command::update_stream;
    bool newer = false;
    const auto cmd = ddb::decode_catalog_command(ddb::encode_catalog_command(c), newer);
    BOOST_TEST_REQUIRE(cmd.has_value());
    BOOST_TEST((*cmd == c));

    // An item command's stream label, and the trim kind, need format 3.
    ddb::item_command ic;
    ic.op = ddb::item_command::kind::put;
    ic.log_changes = true;
    ic.stream_ms = 300;
    ic.written_ms = 400;
    ddb::item_command out;
    BOOST_TEST_REQUIRE((ddb::decode_item_command(ddb::encode_item_command(ic), out) ==
                        ddb::item_command_status::ok));
    BOOST_TEST(out.stream_ms == 300U);
    ddb::item_command trim;
    trim.op = ddb::item_command::kind::trim_change_log;
    trim.written_ms = 5;
    auto encoded = ddb::encode_item_command(trim);
    BOOST_TEST(static_cast<int>(encoded[0]) == 3);
    BOOST_TEST_REQUIRE((ddb::decode_item_command(encoded, out) == ddb::item_command_status::ok));
    BOOST_TEST((out.op == ddb::item_command::kind::trim_change_log));
    encoded[0] = '\2';
    BOOST_TEST(
        (ddb::decode_item_command(encoded, out) == ddb::item_command_status::unknown_version));

    // Sequence numbers and iterators.
    BOOST_TEST(ddb::streams_detail::sequence_number(3, 42) == "000003000000000000042");
    const auto parsed = ddb::streams_detail::parse_sequence_number("000003000000000000042");
    BOOST_TEST_REQUIRE(parsed.has_value());
    BOOST_TEST(parsed->first == 3U);
    BOOST_TEST(parsed->second == 42U);
    const ddb::streams_detail::iterator_position p{"arn:x", ddb::change_stream_key("t/\1", 2), 9,
                                                   77};
    const auto it = ddb::streams_detail::decode_iterator(ddb::streams_detail::encode_iterator(p));
    BOOST_TEST_REQUIRE(it.has_value());
    BOOST_TEST(it->key == p.key);
    BOOST_TEST(it->seq == 9U);
    BOOST_TEST(it->issued_ms == 77U);
    const auto key = ddb::parse_change_stream_key(p.key);
    BOOST_TEST_REQUIRE(key.has_value());
    BOOST_TEST(key->first == std::string("t/\1"));
    BOOST_TEST(key->second == 2U);
    BOOST_TEST(!ddb::parse_change_stream_key("ddb/zz/1").has_value());
}
