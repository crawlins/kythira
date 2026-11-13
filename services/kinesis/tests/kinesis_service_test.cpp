// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Stream lifecycle and writes (`.kiro/specs/kinesis-compatible-streams/`
// task 6, Requirements 6.1, 7.1-7.3, 8.2) through the front end over the
// in-process cluster: create, describe, list and delete with AWS's shapes
// and errors, MD5 and explicit-hash routing, sequence numbers in both id
// formats, PutRecords' per-record results, paging tokens, account limits
// and account isolation.

#define BOOST_TEST_MODULE kinesis_service_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_service.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace kin = kythira::kinesis;
namespace aj = kythira::aws_json;
namespace az = kythira::aws_auth;
namespace sv4 = kythira::sigv4;
namespace json = boost::json;

namespace {

constexpr std::string_view k_account = "111122223333";

// Everyone is a user of `account`, allowed everything.
struct allow_all {
    std::string account{k_account};

    auto authenticate(const sv4::request&, const sv4::service_rules&) -> az::authn_result {
        az::principal_context p;
        p.account = account;
        p.arn = "arn:aws:iam::" + account + ":user/alice";
        return p;
    }
    auto authorize(const az::principal_context&, const az::authz_request&) -> az::authz_decision {
        az::authz_decision d;
        d.allowed = true;
        return d;
    }
};

struct fixture {
    std::int64_t clock = 1'762'776'000'000;
    kin::local_stream_cluster cluster{nullptr, [this] { return clock; }};
    allow_all auth;
    kin::front_end<allow_all> fe{kin::front_end_config{}, auth};
    kin::kinesis_service<kin::local_stream_cluster> service;

    explicit fixture(kin::kinesis_service_config cfg = {}) : service(cluster, std::move(cfg)) {
        service.register_with(fe);
    }

    auto call(std::string_view op, const json::value& body) -> aj::http_response {
        kin::front_end_request r;
        r._method = "POST";
        r._path = "/";
        r._body = json::serialize(body);
        r._headers = {{"Content-Type", "application/x-amz-json-1.1"},
                      {"X-Amz-Target", "Kinesis_20131202." + std::string(op)}};
        return fe.handle(r);
    }

    auto ok(std::string_view op, const json::value& body) -> json::object {
        const auto r = call(op, body);
        BOOST_REQUIRE_MESSAGE(r.status == 200U, std::string(op) + ": " + r.body);
        return json::parse(r.body).as_object();
    }

    auto error_type(std::string_view op, const json::value& body) -> std::string {
        const auto r = call(op, body);
        BOOST_REQUIRE_MESSAGE(r.status != 200U, std::string(op) + " unexpectedly succeeded");
        return std::string(json::parse(r.body).as_object().at("__type").as_string());
    }

    auto put(std::string_view stream, std::string_view key, std::string_view data_b64 = "aGk=")
        -> json::object {
        return ok("PutRecord", {{"StreamName", stream}, {"PartitionKey", key}, {"Data", data_b64}});
    }
};

auto str(const json::value& v) -> std::string {
    return std::string(v.as_string());
}

// The shard a partition key lands on among `n` even shards.
auto expected_shard(std::string_view key, std::uint32_t n) -> std::uint32_t {
    const auto h = kin::partition_key_hash(key);
    const auto ranges = kin::even_ranges(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (ranges[i].contains(h)) {
            return i;
        }
    }
    return n;
}

}  // namespace

BOOST_AUTO_TEST_CASE(create_describe_and_list_a_stream) {
    fixture f;
    BOOST_TEST(f.ok("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 3}}).empty());

    const auto summary = f.ok("DescribeStreamSummary", {{"StreamName", "orders"}})
                             .at("StreamDescriptionSummary")
                             .as_object();
    BOOST_TEST(str(summary.at("StreamStatus")) == "ACTIVE");
    BOOST_TEST(str(summary.at("StreamARN")) ==
               "arn:aws:kinesis:us-east-1:111122223333:stream/orders");
    BOOST_TEST(summary.at("OpenShardCount").as_int64() == 3);
    BOOST_TEST(summary.at("RetentionPeriodHours").as_int64() == 24);
    BOOST_TEST(str(summary.at("StreamModeDetails").at("StreamMode")) == "PROVISIONED");
    BOOST_TEST(summary.at("StreamCreationTimestamp").as_double() == 1'762'776'000.0);
    BOOST_TEST(str(summary.at("EncryptionType")) == "NONE");
    // As AWS answers (task 17 recordings): no StreamId, the record size
    // limit and the channel count.
    BOOST_TEST(!summary.contains("StreamId"));
    BOOST_TEST(summary.at("MaxRecordSizeInKiB").as_int64() == 1024);
    BOOST_TEST(summary.at("ChannelCount").as_int64() == 0);
    BOOST_TEST(!summary.contains("KythiraRelaxedCompatibility"));

    const auto d =
        f.ok("DescribeStream", {{"StreamName", "orders"}}).at("StreamDescription").as_object();
    const auto& shards = d.at("Shards").as_array();
    BOOST_REQUIRE(shards.size() == 3U);
    BOOST_TEST(!d.at("HasMoreShards").as_bool());
    BOOST_TEST(str(shards[0].at("ShardId")) == "shardId-000000000000");
    BOOST_TEST(str(shards[0].at("HashKeyRange").at("StartingHashKey")) == "0");
    BOOST_TEST(str(shards[2].at("HashKeyRange").at("EndingHashKey")) ==
               "340282366920938463463374607431768211455");
    BOOST_TEST(str(shards[1].at("SequenceNumberRange").at("StartingSequenceNumber")).size() == 56U);
    BOOST_TEST(!shards[1].at("SequenceNumberRange").as_object().contains("EndingSequenceNumber"));

    // Paging DescribeStream.
    const auto page = f.ok("DescribeStream", {{"StreamName", "orders"},
                                              {"Limit", 2},
                                              {"ExclusiveStartShardId", "shardId-000000000000"}})
                          .at("StreamDescription")
                          .as_object();
    BOOST_TEST(page.at("Shards").as_array().size() == 2U);
    BOOST_TEST(!page.at("HasMoreShards").as_bool());
    const auto first =
        f.ok("DescribeStream", {{"StreamName", "orders"}, {"Limit", 1}}).at("StreamDescription");
    BOOST_TEST(first.at("HasMoreShards").as_bool());

    const auto list = f.ok("ListStreams", json::object{});
    BOOST_TEST(list.at("StreamNames").as_array().size() == 1U);
    BOOST_TEST(str(list.at("StreamSummaries").at(0).at("StreamStatus")) == "ACTIVE");
    BOOST_TEST(!list.at("HasMoreStreams").as_bool());

    const auto limits = f.ok("DescribeLimits", json::object{});
    BOOST_TEST(limits.at("OpenShardCount").as_int64() == 3);
    BOOST_TEST(limits.at("ShardLimit").as_int64() == 500);
    BOOST_TEST(limits.at("OnDemandStreamCount").as_int64() == 0);
}

BOOST_AUTO_TEST_CASE(create_errors_and_modes) {
    fixture f;
    (void)f.ok("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 1}});
    BOOST_TEST(f.error_type("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 1}}) ==
               "ResourceInUseException");
    BOOST_TEST(
        json::parse(f.call("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 1}}).body)
            .at("message")
            .as_string() == "Stream orders under account 111122223333 already exists.");
    BOOST_TEST(f.error_type("CreateStream", {{"StreamName", "noshards"}}) ==
               "InvalidArgumentException");

    // On demand: four shards unless told otherwise.
    (void)f.ok("CreateStream",
               {{"StreamName", "od"}, {"StreamModeDetails", {{"StreamMode", "ON_DEMAND"}}}});
    const auto od =
        f.ok("DescribeStreamSummary", {{"StreamName", "od"}}).at("StreamDescriptionSummary");
    BOOST_TEST(od.at("OpenShardCount").as_int64() == 4);
    BOOST_TEST(str(od.at("StreamModeDetails").at("StreamMode")) == "ON_DEMAND");
    BOOST_TEST(f.ok("DescribeLimits", json::object{}).at("OnDemandStreamCount").as_int64() == 1);

    // A compatibility tag with a typo is refused before anything is made.
    BOOST_TEST(f.error_type("CreateStream", {{"StreamName", "typo"},
                                             {"ShardCount", 1},
                                             {"Tags", {{"kythira:compat:throttling", "of"}}}}) ==
               "InvalidArgumentException");
    BOOST_TEST(f.error_type("DescribeStreamSummary", {{"StreamName", "typo"}}) ==
               "ResourceNotFoundException");

    // What this endpoint does not do yet is refused, not ignored.
    BOOST_TEST(f.error_type("DescribeStreamSummary", {{"StreamId", "00000000000000000001-kyt"}}) ==
               "InvalidArgumentException");
    BOOST_TEST(f.error_type("PutRecord", {{"StreamName", "orders"},
                                          {"PartitionKey", "k"},
                                          {"Data", "aGk="},
                                          {"DryRun", true}}) == "InvalidArgumentException");
}

BOOST_AUTO_TEST_CASE(the_shard_limit_and_its_switch) {
    kin::kinesis_service_config cfg;
    cfg._limits._shard_limit = 5;
    fixture f(cfg);
    (void)f.ok("CreateStream", {{"StreamName", "a"}, {"ShardCount", 4}});
    const auto r = f.call("CreateStream", {{"StreamName", "b"}, {"ShardCount", 2}});
    BOOST_TEST(r.status == 400U);
    const auto body = json::parse(r.body).as_object();
    BOOST_TEST(str(body.at("__type")) == "LimitExceededException");
    BOOST_TEST(str(body.at("message"))
                   .starts_with("This request would exceed the shard limit for the account "
                                "111122223333 in us-east-1. Current shard count for the "
                                "account: 4. Limit: 5."));
    // A stream may relax count limits for itself.
    (void)f.ok("CreateStream", {{"StreamName", "b"},
                                {"ShardCount", 2},
                                {"Tags", {{"kythira:compat:count-limits", "relaxed"}}}});
    const auto summary =
        f.ok("DescribeStreamSummary", {{"StreamName", "b"}}).at("StreamDescriptionSummary");
    BOOST_TEST(str(summary.at("KythiraRelaxedCompatibility").at(0)) == "count-limits");
}

BOOST_AUTO_TEST_CASE(put_record_routes_by_md5_and_numbers_in_order) {
    fixture f;
    (void)f.ok("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 4}});
    std::string previous;
    for (int i = 0; i < 5; ++i) {
        const auto r = f.put("orders", "customer-7");
        const auto want = expected_shard("customer-7", 4);
        BOOST_TEST(str(r.at("ShardId")) == kin::shard_id_of(want));
        const auto seq = str(r.at("SequenceNumber"));
        BOOST_TEST(seq.size() == 56U);
        // Same length, so string order is integer order.
        BOOST_TEST(seq > previous);
        previous = seq;
        BOOST_TEST(!r.contains("EncryptionType"));
    }
    // An explicit hash key overrides the partition key.
    const auto top =
        f.ok("PutRecord", {{"StreamName", "orders"},
                           {"PartitionKey", "customer-7"},
                           {"ExplicitHashKey", "340282366920938463463374607431768211455"},
                           {"Data", "aGk="}});
    BOOST_TEST(str(top.at("ShardId")) == "shardId-000000000003");
    BOOST_TEST(
        f.error_type("PutRecord", {{"StreamName", "orders"},
                                   {"PartitionKey", "k"},
                                   {"ExplicitHashKey", "340282366920938463463374607431768211456"},
                                   {"Data", "aGk="}}) == "InvalidArgumentException");
    BOOST_TEST(f.error_type("PutRecord", {{"StreamName", "orders"},
                                          {"PartitionKey", "k"},
                                          {"SequenceNumberForOrdering", "12x"},
                                          {"Data", "aGk="}}) == "ValidationException");
    BOOST_TEST(f.error_type("PutRecord",
                            {{"StreamName", "missing"}, {"PartitionKey", "k"}, {"Data", "aGk="}}) ==
               "ResourceNotFoundException");
}

BOOST_AUTO_TEST_CASE(put_records_answers_per_record_in_request_order) {
    fixture f;
    (void)f.ok("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 2}});
    json::array records;
    for (int i = 0; i < 20; ++i) {
        records.push_back({{"PartitionKey", "pk-" + std::to_string(i)}, {"Data", "aGVsbG8="}});
    }
    const auto before = f.cluster.proposals();
    const auto r = f.ok("PutRecords", {{"StreamName", "orders"}, {"Records", records}});
    BOOST_TEST(r.at("FailedRecordCount").as_int64() == 0);
    // Both shards live in the one local group: the whole batch is one
    // proposal (group commit), not one per shard.
    BOOST_TEST(f.cluster.proposals() == before + 1);
    const auto& out = r.at("Records").as_array();
    BOOST_REQUIRE(out.size() == 20U);
    std::map<std::string, std::string> last_per_shard;
    for (int i = 0; i < 20; ++i) {
        const auto& e = out[static_cast<std::size_t>(i)];
        const auto shard = str(e.at("ShardId"));
        BOOST_TEST(shard == kin::shard_id_of(expected_shard("pk-" + std::to_string(i), 2)));
        // Within a shard, request order is sequence order.
        const auto seq = str(e.at("SequenceNumber"));
        BOOST_TEST(seq > last_per_shard[shard]);
        last_per_shard[shard] = seq;
    }

    // A lost leader fails every record, in one 200 response.
    f.cluster.set_unavailable(true);
    const auto failed = f.ok("PutRecords", {{"StreamName", "orders"}, {"Records", records}});
    BOOST_TEST(failed.at("FailedRecordCount").as_int64() == 20);
    BOOST_TEST(str(failed.at("Records").at(0).at("ErrorCode")) == "InternalFailure");
    BOOST_TEST(str(failed.at("Records").at(0).at("ErrorMessage")) == "Internal service failure.");
    // PutRecord has no per-record channel: the whole call fails.
    const auto single =
        f.call("PutRecord", {{"StreamName", "orders"}, {"PartitionKey", "k"}, {"Data", "aGk="}});
    BOOST_TEST(single.status == 500U);
}

BOOST_AUTO_TEST_CASE(compact_ids_on_request) {
    fixture f;
    (void)f.ok("CreateStream", {{"StreamName", "small"},
                                {"ShardCount", 1},
                                {"Tags", {{"kythira:compat:id-format", "compact"}}}});
    const auto a = f.put("small", "k");
    const auto b = f.put("small", "k");
    BOOST_TEST(str(a.at("ShardId")) == "shardId-0");
    BOOST_TEST(str(a.at("SequenceNumber")) == "1");
    BOOST_TEST(str(b.at("SequenceNumber")) == "2");
}

BOOST_AUTO_TEST_CASE(list_streams_pages_and_isolates_accounts) {
    fixture f;
    for (const auto* n : {"delta", "alpha", "charlie", "bravo"}) {
        (void)f.ok("CreateStream", {{"StreamName", n}, {"ShardCount", 1}});
    }
    auto page = f.ok("ListStreams", {{"Limit", 3}});
    BOOST_TEST(page.at("StreamNames").as_array().size() == 3U);
    BOOST_TEST(str(page.at("StreamNames").at(0)) == "alpha");
    BOOST_TEST(page.at("HasMoreStreams").as_bool());
    const auto next = f.ok("ListStreams", {{"NextToken", page.at("NextToken")}});
    BOOST_TEST(next.at("StreamNames").as_array().size() == 1U);
    BOOST_TEST(str(next.at("StreamNames").at(0)) == "delta");
    BOOST_TEST(!next.at("HasMoreStreams").as_bool());
    const auto after = f.ok("ListStreams", {{"ExclusiveStartStreamName", "bravo"}});
    BOOST_TEST(str(after.at("StreamNames").at(0)) == "charlie");
    BOOST_TEST(f.error_type("ListStreams", {{"NextToken", "bm9wZQ=="}}) ==
               "InvalidArgumentException");

    // Another account sees none of them, and may reuse a name.
    f.auth.account = "444455556666";
    BOOST_TEST(f.ok("ListStreams", json::object{}).at("StreamNames").as_array().empty());
    BOOST_TEST(f.error_type("DescribeStreamSummary", {{"StreamName", "alpha"}}) ==
               "ResourceNotFoundException");
    (void)f.ok("CreateStream", {{"StreamName", "alpha"}, {"ShardCount", 1}});
    BOOST_TEST(str(f.ok("DescribeStreamSummary", {{"StreamName", "alpha"}})
                       .at("StreamDescriptionSummary")
                       .at("StreamARN")) == "arn:aws:kinesis:us-east-1:444455556666:stream/alpha");
}

BOOST_AUTO_TEST_CASE(list_shards_filters_and_pages) {
    fixture f;
    (void)f.ok("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 5}});
    const auto all = f.ok("ListShards", {{"StreamName", "orders"}});
    BOOST_TEST(all.at("Shards").as_array().size() == 5U);
    BOOST_TEST(!all.contains("NextToken"));

    const auto first = f.ok("ListShards", {{"StreamName", "orders"}, {"MaxResults", 2}});
    BOOST_TEST(first.at("Shards").as_array().size() == 2U);
    BOOST_REQUIRE(first.contains("NextToken"));
    const auto second = f.ok("ListShards", {{"NextToken", first.at("NextToken")}});
    BOOST_TEST(second.at("Shards").as_array().size() == 3U);
    BOOST_TEST(str(second.at("Shards").at(0).at("ShardId")) == "shardId-000000000002");

    BOOST_TEST(f.error_type("ListShards",
                            {{"StreamName", "orders"}, {"NextToken", first.at("NextToken")}}) ==
               "InvalidArgumentException");
    BOOST_TEST(f.error_type("ListShards", {{"NextToken", "garbage="}}) ==
               "InvalidArgumentException");
    BOOST_TEST(f.error_type("ListShards", json::object{}) == "InvalidArgumentException");

    const auto after =
        f.ok("ListShards",
             {{"StreamName", "orders"},
              {"ShardFilter", {{"Type", "AFTER_SHARD_ID"}, {"ShardId", "shardId-000000000003"}}}});
    BOOST_TEST(after.at("Shards").as_array().size() == 1U);
    BOOST_TEST(
        f.ok("ListShards", {{"StreamName", "orders"}, {"ShardFilter", {{"Type", "AT_LATEST"}}}})
            .at("Shards")
            .as_array()
            .size() == 5U);
    BOOST_TEST(f.error_type("ListShards", {{"StreamName", "orders"},
                                           {"ShardFilter", {{"Type", "AFTER_SHARD_ID"}}}}) ==
               "InvalidArgumentException");

    // A token outlives nothing: after the stream is deleted it finds no stream.
    (void)f.ok("DeleteStream", {{"StreamName", "orders"}});
    BOOST_TEST(f.error_type("ListShards", {{"NextToken", first.at("NextToken")}}) ==
               "ResourceNotFoundException");
}

BOOST_AUTO_TEST_CASE(delete_and_recreate) {
    fixture f;
    (void)f.ok("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 2}});
    (void)f.put("orders", "k");
    BOOST_TEST(f.ok("DeleteStream", {{"StreamName", "orders"}}).empty());
    BOOST_TEST(f.error_type("DescribeStream", {{"StreamName", "orders"}}) ==
               "ResourceNotFoundException");
    BOOST_TEST(f.error_type("PutRecord",
                            {{"StreamName", "orders"}, {"PartitionKey", "k"}, {"Data", "aGk="}}) ==
               "ResourceNotFoundException");
    BOOST_TEST(f.error_type("DeleteStream", {{"StreamName", "orders"}}) ==
               "ResourceNotFoundException");
    BOOST_TEST(f.ok("DescribeLimits", json::object{}).at("OpenShardCount").as_int64() == 0);

    // Recreated, it is a new stream: new id, so new routing keys.
    (void)f.ok("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 1}});
    std::uint64_t ids = 0;
    BOOST_TEST(f.cluster.read(kin::next_id_key(), [&](const auto& sm) {
        ids = sm.find_stream(std::string(k_account), "orders")->_id;
    }));
    BOOST_TEST(ids == 2U);
    BOOST_TEST(str(f.put("orders", "k").at("ShardId")) == "shardId-000000000000");
}

namespace {

// A create interrupted after its first step: CREATING, no shards open.
void half_create(fixture& f, const std::string& name, std::uint32_t shards) {
    kin::create_stream_command c;
    c._account = std::string(k_account);
    c._name = name;
    c._shard_count = shards;
    const kin::stream_command cmd = c;
    BOOST_REQUIRE(f.cluster.propose(kin::command_key(cmd), kin::encode_command(cmd))->_status ==
                  kin::apply_status::ok);
}

auto status_of(fixture& f, const std::string& name) -> std::string {
    return str(f.ok("DescribeStreamSummary", {{"StreamName", name}})
                   .at("StreamDescriptionSummary")
                   .at("StreamStatus"));
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_stream_not_yet_active_takes_no_writes) {
    fixture f;
    half_create(f, "half", 1);
    BOOST_TEST(status_of(f, "half") == "CREATING");
    BOOST_TEST(f.error_type("PutRecord",
                            {{"StreamName", "half"}, {"PartitionKey", "k"}, {"Data", "aGk="}}) ==
               "ResourceNotFoundException");
    // DeleteStream clears it.
    BOOST_TEST(f.ok("DeleteStream", {{"StreamName", "half"}}).empty());
}

// A create interrupted between the catalog commit and ACTIVE (a leader
// change mid-create, seen once under rootless Podman as a stream that never
// left CREATING) must not stay that way: the caller's retry finishes it, and
// so does the maintenance pass once it has sat CREATING for a while.
BOOST_AUTO_TEST_CASE(an_interrupted_create_is_finished) {
    fixture f;
    half_create(f, "retried", 2);
    // The SDK's retry: answered in use, as AWS answers, but the stream
    // comes out ACTIVE with both shards open and writable.
    BOOST_TEST(f.error_type("CreateStream", {{"StreamName", "retried"}, {"ShardCount", 2}}) ==
               "ResourceInUseException");
    BOOST_TEST(status_of(f, "retried") == "ACTIVE");
    for (const auto* key : {"a", "b", "c", "d", "e", "f"}) {
        (void)f.put("retried", key);
    }

    half_create(f, "abandoned", 3);
    BOOST_TEST(f.service.repair_creates(1'000, 30'000) == 0U);   // first sighting
    BOOST_TEST(f.service.repair_creates(20'000, 30'000) == 0U);  // still fresh
    BOOST_TEST(status_of(f, "abandoned") == "CREATING");
    BOOST_TEST(f.service.repair_creates(31'000, 30'000) == 1U);
    BOOST_TEST(status_of(f, "abandoned") == "ACTIVE");
    BOOST_TEST(f.ok("ListShards", {{"StreamName", "abandoned"}}).at("Shards").as_array().size() ==
               3U);
    (void)f.put("abandoned", "k");
    BOOST_TEST(f.service.repair_creates(90'000, 30'000) == 0U);  // nothing left
}

BOOST_AUTO_TEST_CASE(record_and_batch_size_limits) {
    fixture f;
    (void)f.ok("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 1}});
    // The limit is on the data alone: AWS takes 1 MiB of data with a
    // partition key (the task 17 recordings) and refuses a byte more.
    const std::string mib(std::size_t{1024} * 1024, 'a');
    BOOST_TEST(f.ok("PutRecord", {{"StreamName", "orders"},
                                  {"PartitionKey", "k"},
                                  {"Data", kin::service_detail::base64_encode(mib)}})
                   .contains("SequenceNumber"));
    const auto big = kin::service_detail::base64_encode(mib + "a");
    BOOST_TEST(f.error_type("PutRecord",
                            {{"StreamName", "orders"}, {"PartitionKey", "k"}, {"Data", big}}) ==
               "InvalidArgumentException");
    // Six records of 1 MiB less the key are each fine but over 5 MB together.
    const auto almost = kin::service_detail::base64_encode(mib.substr(1));
    json::array records;
    for (int i = 0; i < 6; ++i) {
        records.push_back({{"PartitionKey", "k"}, {"Data", almost}});
    }
    BOOST_TEST(f.error_type("PutRecords", {{"StreamName", "orders"}, {"Records", records}}) ==
               "InvalidArgumentException");
}

// AWS refuses an AT_TIMESTAMP iterator for a time after now, naming both
// times in milliseconds (the task 17 recordings), and takes now itself.
BOOST_AUTO_TEST_CASE(at_timestamp_in_the_future_is_refused) {
    fixture f;
    (void)f.ok("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 1}});
    const auto at = [&](double seconds) {
        return f.call("GetShardIterator", {{"StreamName", "orders"},
                                           {"ShardId", "shardId-000000000000"},
                                           {"ShardIteratorType", "AT_TIMESTAMP"},
                                           {"Timestamp", seconds}});
    };
    const auto future = at(static_cast<double>(f.clock + 1000) / 1000.0);
    BOOST_TEST(future.status == 400U);
    BOOST_TEST(future.body.find("InvalidArgumentException") != std::string::npos);
    BOOST_TEST(future.body.find("timestampInMillis: 1762776001000, currentTimestampInMillis: "
                                "1762776000000") != std::string::npos,
               future.body);
    BOOST_TEST(at(static_cast<double>(f.clock) / 1000.0).status == 200U);
}

BOOST_AUTO_TEST_CASE(base64_round_trips) {
    for (const std::string s : {"", "a", "ab", "abc", "abcd", "\xff\x00\x10"}) {
        const auto e = kin::service_detail::base64_encode(s);
        BOOST_TEST(kin::service_detail::base64_decode(e).value_or("?") == s);
    }
    BOOST_TEST(kin::service_detail::base64_encode("hello") == "aGVsbG8=");
    BOOST_TEST(!kin::service_detail::base64_decode("abc").has_value());
    BOOST_TEST(!kin::service_detail::base64_decode("a=bc").has_value());
    BOOST_TEST(!kin::service_detail::base64_decode("ab!c").has_value());
}

// AWS's PutRecords takes at most 500 records (the model's bound, enforced
// whatever the compatibility profile) and at least one.
BOOST_AUTO_TEST_CASE(put_records_takes_one_to_500_records) {
    fixture f;
    (void)f.ok("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 1}});
    const auto batch = [](int n) {
        json::array records;
        for (int i = 0; i < n; ++i) {
            records.push_back({{"PartitionKey", "pk-" + std::to_string(i)}, {"Data", "aGk="}});
        }
        return records;
    };
    BOOST_TEST(f.error_type("PutRecords", {{"StreamName", "orders"}, {"Records", batch(501)}}) ==
               "ValidationException");
    BOOST_TEST(f.error_type("PutRecords", {{"StreamName", "orders"}, {"Records", batch(0)}}) ==
               "ValidationException");
    const auto ok = f.ok("PutRecords", {{"StreamName", "orders"}, {"Records", batch(500)}});
    BOOST_TEST(ok.at("FailedRecordCount").as_int64() == 0);
    BOOST_TEST(ok.at("Records").as_array().size() == 500U);
}

// Requirement 7.2: SequenceNumberForOrdering. A record put with the
// previous record's sequence number for the same partition key gets a
// strictly greater one, and the parameter accepts a number this cluster
// issued in either id format.
BOOST_AUTO_TEST_CASE(sequence_number_for_ordering_is_honoured) {
    fixture f;
    (void)f.ok("CreateStream", {{"StreamName", "orders"}, {"ShardCount", 2}});
    std::string previous = str(f.put("orders", "user-1").at("SequenceNumber"));
    for (int i = 0; i < 5; ++i) {
        const auto r = f.ok("PutRecord", {{"StreamName", "orders"},
                                          {"PartitionKey", "user-1"},
                                          {"Data", "aGk="},
                                          {"SequenceNumberForOrdering", previous}});
        const auto seq = str(r.at("SequenceNumber"));
        BOOST_TEST(seq.size() == previous.size());
        BOOST_TEST(seq > previous);
        previous = seq;
    }
    // Interleaved writes to the other shard do not disturb the order.
    (void)f.put("orders", "user-2");
    const auto r = f.ok("PutRecord", {{"StreamName", "orders"},
                                      {"PartitionKey", "user-1"},
                                      {"Data", "aGk="},
                                      {"SequenceNumberForOrdering", previous}});
    BOOST_TEST(str(r.at("SequenceNumber")) > previous);
    // The compact form is a sequence number too.
    BOOST_TEST(f.ok("PutRecord", {{"StreamName", "orders"},
                                  {"PartitionKey", "user-1"},
                                  {"Data", "aGk="},
                                  {"SequenceNumberForOrdering", "7"}})
                   .contains("SequenceNumber"));
}
