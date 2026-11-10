// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Per-shard throughput limits (`.kiro/specs/kinesis-compatible-streams/`
// task 8, Requirements 3.2, 3.5 and 12.1): the token buckets, the leader's
// write admission (a prefix of a shard's records), the read buckets
// (calls, and bytes that may go into debt), and the handlers' answers:
// ProvisionedThroughputExceededException for PutRecord, GetRecords and
// GetShardIterator, the per-record error in PutRecords, and the
// `throttling` switch on by default, off per cluster, off per stream, and a
// stream's setting winning over the cluster's.

#define BOOST_TEST_MODULE kinesis_throttle_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_service.hpp>
#include <kythira/kinesis/kinesis_throttle.hpp>

#include <raft/record_segment_store.hpp>

#include <boost/json.hpp>

#include <unistd.h>

#include <filesystem>
#include <memory>
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

struct temp_dir {
    std::filesystem::path _path;
    temp_dir() {
        static int counter = 0;
        _path = std::filesystem::temp_directory_path() /
                ("kinesis_throttle_test." + std::to_string(::getpid()) + "." +
                 std::to_string(++counter));
        std::filesystem::remove_all(_path);
        std::filesystem::create_directories(_path);
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
};

struct allow_all {
    auto authenticate(const sv4::request&, const sv4::service_rules&) -> az::authn_result {
        az::principal_context p;
        p.account = std::string(k_account);
        p.arn = "arn:aws:iam::" + std::string(k_account) + ":user/alice";
        return p;
    }
    auto authorize(const az::principal_context&, const az::authz_request&) -> az::authz_decision {
        az::authz_decision d;
        d.allowed = true;
        return d;
    }
};

auto append(std::string key, std::size_t n, std::size_t bytes = 10) -> kin::append_records_command {
    kin::append_records_command a;
    a._key = std::move(key);
    for (std::size_t i = 0; i < n; ++i) {
        a._records.push_back({"k", std::vector<std::byte>(bytes - 1, std::byte{0})});
    }
    return a;
}

// A cluster with a store and the leader's write buckets, and a service
// whose read buckets share the same fake monotonic clock.
struct fixture {
    temp_dir dir;
    std::unique_ptr<kythira::record_segment_store> store =
        std::make_unique<kythira::record_segment_store>(kythira::record_segment_store_options{
            ._data_dir = dir._path, ._barriers_enabled = false});
    std::int64_t mono = 1'000'000;
    std::int64_t wall = 1'762'776'000'000;
    kin::write_throttle writes{{}, [this] { return mono; }};
    kin::local_stream_cluster cluster{store.get(), [this] { return wall; }};
    allow_all auth;
    kin::front_end<allow_all> fe{kin::front_end_config{}, auth};
    std::unique_ptr<kin::kinesis_service<kin::local_stream_cluster>> service;

    explicit fixture(kin::kinesis_compat_profile cluster_profile = {}) {
        cluster.set_write_throttle(&writes);
        kin::kinesis_service_config c;
        c._cluster_profile = cluster_profile;
        c._monotonic = [this] { return mono; };
        service = std::make_unique<kin::kinesis_service<kin::local_stream_cluster>>(cluster, c);
        service->register_with(fe);
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

    void create(std::string_view name, const json::object& tags = {}) {
        json::object body{{"StreamName", name}, {"ShardCount", 1}};
        if (!tags.empty()) {
            body["Tags"] = tags;
        }
        (void)ok("CreateStream", body);
    }

    // `n` records of `bytes` data each (aGk= is 2 bytes) to the one shard.
    auto put_records(std::string_view stream, int n) -> json::object {
        json::array records;
        for (int i = 0; i < n; ++i) {
            records.push_back({{"PartitionKey", "p"}, {"Data", "aGk="}});
        }
        return ok("PutRecords", {{"StreamName", stream}, {"Records", records}});
    }

    auto iterator(std::string_view stream) -> aj::http_response {
        return call("GetShardIterator", {{"StreamName", stream},
                                         {"ShardId", "shardId-000000000000"},
                                         {"ShardIteratorType", "TRIM_HORIZON"}});
    }
};

auto type_of(const aj::http_response& r) -> std::string {
    return std::string(json::parse(r.body).as_object().at("__type").as_string());
}

auto message_of(const aj::http_response& r) -> std::string {
    return std::string(json::parse(r.body).as_object().at("message").as_string());
}

}  // namespace

// ---- buckets ------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(a_bucket_holds_one_second_and_refills_continuously) {
    kin::token_bucket b(1000.0, 0);
    BOOST_TEST(b.tokens() == 1000.0);
    b.take(1000.0);
    b.refill(250);
    BOOST_TEST(b.tokens() == 250.0);
    b.refill(10'000);
    BOOST_TEST(b.tokens() == 1000.0);  // never more than a second's worth
    b.give(5.0);
    BOOST_TEST(b.tokens() == 1000.0);
}

BOOST_AUTO_TEST_CASE(write_admission_takes_a_prefix_and_refunds) {
    std::int64_t now = 0;
    kin::write_throttle t({}, [&now] { return now; });
    const auto a = append("s1", 700);
    const auto b = append("s1", 700);
    const auto other = append("s2", 700);
    kin::admission_ticket ticket;
    const auto got = t.admit({&a, &b, &other}, ticket);
    BOOST_TEST(got[0] == 700U);
    BOOST_TEST(got[1] == 300U);  // same shard: what is left of 1000
    BOOST_TEST(got[2] == 700U);  // another shard has its own buckets
    t.refund(ticket);
    kin::admission_ticket again;
    BOOST_TEST(t.admit({&a}, again)[0] == 700U);

    // The byte bucket: 1 MiB, data plus partition key.
    kin::write_throttle bytes({}, [&now] { return now; });
    const auto big = append("s3", 5, 300 * 1024);
    kin::admission_ticket tb;
    BOOST_TEST(bytes.admit({&big}, tb)[0] == 3U);
}

BOOST_AUTO_TEST_CASE(idle_write_buckets_are_forgotten) {
    std::int64_t now = 0;
    kin::write_throttle t({}, [&now] { return now; });
    kin::admission_ticket ticket;
    const auto a = append("s1", 1);
    (void)t.admit({&a}, ticket);
    BOOST_TEST(t.size() == 1U);
    now += 120'000;
    const auto b = append("s2", 1);
    (void)t.admit({&b}, ticket);
    BOOST_TEST(t.size() == 1U);
}

BOOST_AUTO_TEST_CASE(read_buckets_count_calls_and_let_bytes_run_into_debt) {
    std::int64_t now = 0;
    kin::read_throttle t({}, [&now] { return now; });
    for (int i = 0; i < 5; ++i) {
        BOOST_TEST(t.admit_get_records("s"));
    }
    BOOST_TEST(!t.admit_get_records("s"));
    BOOST_TEST(t.admit_get_records("other"));
    now += 200;  // one call's worth
    BOOST_TEST(t.admit_get_records("s"));
    BOOST_TEST(!t.admit_get_records("s"));

    // A 10 MiB answer: no reads of that shard for the next ~4 s (2 MiB/s,
    // starting from a full 2 MiB bucket), as AWS documents.
    now += 10'000;
    BOOST_TEST(t.admit_get_records("s"));
    t.charge("s", std::uint64_t{10} * 1024 * 1024);
    now += 3'900;
    BOOST_TEST(!t.admit_get_records("s"));
    now += 200;
    BOOST_TEST(t.admit_get_records("s"));

    for (int i = 0; i < 5; ++i) {
        BOOST_TEST(t.admit_get_shard_iterator("s"));
    }
    BOOST_TEST(!t.admit_get_shard_iterator("s"));
}

// ---- the handlers -------------------------------------------------------------

BOOST_AUTO_TEST_CASE(put_records_throttles_the_records_past_the_limit) {
    fixture f;
    f.create("s");
    BOOST_TEST(f.put_records("s", 500).at("FailedRecordCount").as_int64() == 0);
    BOOST_TEST(f.put_records("s", 400).at("FailedRecordCount").as_int64() == 0);
    const auto r = f.put_records("s", 200);
    BOOST_TEST(r.at("FailedRecordCount").as_int64() == 100);
    const auto& recs = r.at("Records").as_array();
    // A shard's admitted records are a prefix: the failures are the tail.
    for (std::size_t i = 0; i < 100; ++i) {
        BOOST_TEST(recs[i].as_object().contains("SequenceNumber"));
    }
    for (std::size_t i = 100; i < 200; ++i) {
        const auto& e = recs[i].as_object();
        BOOST_TEST(std::string(e.at("ErrorCode").as_string()) ==
                   "ProvisionedThroughputExceededException");
        BOOST_TEST(std::string(e.at("ErrorMessage").as_string()) ==
                   "Rate exceeded for shard shardId-000000000000 in stream s under account "
                   "111122223333.");
    }

    // PutRecord has no per-record channel: the call fails.
    const auto one =
        f.call("PutRecord", {{"StreamName", "s"}, {"PartitionKey", "p"}, {"Data", "aGk="}});
    BOOST_TEST(one.status == 400U);
    BOOST_TEST(type_of(one) == "ProvisionedThroughputExceededException");
    BOOST_TEST(message_of(one) ==
               "Rate exceeded for shard shardId-000000000000 in stream s under account "
               "111122223333.");

    // A second later the shard takes writes again.
    f.mono += 1000;
    BOOST_TEST(f.put_records("s", 500).at("FailedRecordCount").as_int64() == 0);
}

BOOST_AUTO_TEST_CASE(the_switch_is_on_by_default_and_off_per_cluster_or_stream) {
    {
        fixture f;  // default: on
        f.create("s");
        f.create("free", {{"kythira:compat:throttling", "off"}});
        (void)f.put_records("s", 500);
        (void)f.put_records("s", 500);
        BOOST_TEST(f.put_records("s", 10).at("FailedRecordCount").as_int64() == 10);
        // Off per stream: never throttled.
        for (int i = 0; i < 4; ++i) {
            BOOST_TEST(f.put_records("free", 500).at("FailedRecordCount").as_int64() == 0);
        }
        for (int i = 0; i < 10; ++i) {
            BOOST_TEST(f.iterator("free").status == 200U);
        }
    }
    {
        kin::kinesis_compat_profile off;
        off._throttling = false;
        fixture f(off);  // off for the cluster
        f.create("s");
        f.create("strict", {{"kythira:compat:throttling", "on"}});
        for (int i = 0; i < 4; ++i) {
            BOOST_TEST(f.put_records("s", 500).at("FailedRecordCount").as_int64() == 0);
        }
        // The stream's own setting wins over the cluster's.
        (void)f.put_records("strict", 500);
        (void)f.put_records("strict", 500);
        BOOST_TEST(f.put_records("strict", 10).at("FailedRecordCount").as_int64() == 10);
    }
}

BOOST_AUTO_TEST_CASE(reads_meet_this_nodes_buckets) {
    fixture f;
    f.create("s");
    (void)f.put_records("s", 10);
    std::string it;
    for (int i = 0; i < 5; ++i) {
        const auto r = f.iterator("s");
        BOOST_REQUIRE(r.status == 200U);
        it = std::string(json::parse(r.body).at("ShardIterator").as_string());
    }
    const auto sixth = f.iterator("s");
    BOOST_TEST(sixth.status == 400U);
    BOOST_TEST(type_of(sixth) == "ProvisionedThroughputExceededException");
    BOOST_TEST(message_of(sixth) ==
               "Rate exceeded for shard shardId-000000000000 in stream s under account "
               "111122223333.");

    for (int i = 0; i < 5; ++i) {
        BOOST_TEST(f.call("GetRecords", {{"ShardIterator", it}}).status == 200U);
    }
    const auto r = f.call("GetRecords", {{"ShardIterator", it}});
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(type_of(r) == "ProvisionedThroughputExceededException");
    BOOST_TEST(message_of(r) == "Rate exceeded for Shard - 111122223333:s:shardId-000000000000");
    f.mono += 200;
    BOOST_TEST(f.call("GetRecords", {{"ShardIterator", it}}).status == 200U);
}

BOOST_AUTO_TEST_CASE(a_big_read_blocks_the_shard_until_its_bytes_refill) {
    fixture f;
    f.create("s");
    // Nearly 3 MiB of records over three seconds of writes (1 MiB/s per
    // shard, the partition key counting too, so each record stays a little
    // under a quarter MiB).
    const std::string data =
        kin::service_detail::base64_encode(std::string((256 * 1024) - 16, 'x'));
    for (int i = 0; i < 12; ++i) {
        if (i % 4 == 0) {
            f.mono += 1000;
        }
        (void)f.ok("PutRecord", {{"StreamName", "s"}, {"PartitionKey", "p"}, {"Data", data}});
    }
    const auto it = std::string(json::parse(f.iterator("s").body).at("ShardIterator").as_string());
    const auto r = f.ok("GetRecords", {{"ShardIterator", it}});
    BOOST_TEST(r.at("Records").as_array().size() == 12U);
    // ~3 MiB from a 2 MiB bucket: ~1 MiB of debt, half a second to clear.
    BOOST_TEST(f.call("GetRecords", {{"ShardIterator", it}}).status == 400U);
    f.mono += 400;
    BOOST_TEST(f.call("GetRecords", {{"ShardIterator", it}}).status == 400U);
    f.mono += 200;
    BOOST_TEST(f.call("GetRecords", {{"ShardIterator", it}}).status == 200U);
}
