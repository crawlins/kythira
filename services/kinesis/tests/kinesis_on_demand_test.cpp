// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// On-demand capacity mode (`.kiro/specs/kinesis-compatible-streams/` task
// 12, Requirement 10.3) on the in-process cluster: UpdateStreamMode and its
// refusals, the provisioned-only resharding calls, and the controller
// splitting a shard that stays hot while leaving quiet shards and
// provisioned streams alone.

#define BOOST_TEST_MODULE kinesis_on_demand_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_hash.hpp>
#include <kythira/kinesis/kinesis_on_demand.hpp>
#include <kythira/kinesis/kinesis_service.hpp>

#include <raft/record_segment_store.hpp>

#include <boost/json.hpp>

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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
                ("kinesis_on_demand_test." + std::to_string(::getpid()) + "." +
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

struct fixture {
    temp_dir dir;
    std::unique_ptr<kythira::record_segment_store> store =
        std::make_unique<kythira::record_segment_store>(kythira::record_segment_store_options{
            ._data_dir = dir._path, ._barriers_enabled = false});
    std::int64_t now = 1'000'000;
    kin::local_stream_cluster cluster{store.get(), [this] { return now; }};
    allow_all auth;
    kin::front_end<allow_all> fe{kin::front_end_config{}, auth};
    std::unique_ptr<kin::kinesis_service<kin::local_stream_cluster>> service;

    explicit fixture(kin::kinesis_service_config cfg = {}) {
        service = std::make_unique<kin::kinesis_service<kin::local_stream_cluster>>(cluster,
                                                                                    std::move(cfg));
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

    void create(std::string_view name, int shards) {
        (void)ok("CreateStream", {{"StreamName", name}, {"ShardCount", shards}});
    }

    // Puts one record per key, with the key as its data; the shard each
    // landed on.
    auto put(std::string_view stream, const std::vector<std::string>& keys)
        -> std::vector<std::string> {
        std::vector<std::string> shards;
        for (const auto& k : keys) {
            const auto r = ok("PutRecord", {{"StreamName", stream},
                                            {"PartitionKey", k},
                                            {"Data", "aGk="},
                                            {"ExplicitHashKey", k}});
            shards.emplace_back(r.at("ShardId").as_string());
        }
        return shards;
    }

    auto shards(std::string_view stream) -> std::map<std::string, json::object> {
        std::map<std::string, json::object> out;
        const auto listed = ok("ListShards", {{"StreamName", stream}});
        for (const auto& s : listed.at("Shards").as_array()) {
            out[std::string(s.as_object().at("ShardId").as_string())] = s.as_object();
        }
        return out;
    }

    auto status(std::string_view stream) -> std::string {
        return std::string(ok("DescribeStreamSummary", {{"StreamName", stream}})
                               .at("StreamDescriptionSummary")
                               .as_object()
                               .at("StreamStatus")
                               .as_string());
    }

    auto summary(std::string_view stream) -> json::object {
        return ok("DescribeStreamSummary", {{"StreamName", stream}})
            .at("StreamDescriptionSummary")
            .as_object();
    }

    auto arn(std::string_view stream) -> std::string {
        return std::string(summary(stream).at("StreamARN").as_string());
    }

    auto mode(std::string_view stream) -> std::string {
        return std::string(
            summary(stream).at("StreamModeDetails").as_object().at("StreamMode").as_string());
    }

    auto set_mode(std::string_view stream, std::string_view m) -> aj::http_response {
        return call("UpdateStreamMode",
                    {{"StreamARN", arn(stream)}, {"StreamModeDetails", {{"StreamMode", m}}}});
    }

    // `n` records of `bytes` bytes each, spread over the key space.
    void write(std::string_view stream, int n, std::size_t bytes = 10) {
        const std::string data(bytes, 'x');
        for (int done = 0; done < n;) {
            json::array batch;
            for (int i = 0; i < 500 && done < n; ++i, ++done) {
                batch.push_back({{"PartitionKey", "pk-" + std::to_string(done)},
                                 {"Data", kin::service_detail::base64_encode(data)}});
            }
            const auto r = ok("PutRecords", {{"StreamName", stream}, {"Records", batch}});
            BOOST_REQUIRE(r.at("FailedRecordCount").as_int64() == 0);
        }
    }

    auto open_shards(std::string_view stream) -> std::size_t {
        return static_cast<std::size_t>(summary(stream).at("OpenShardCount").as_int64());
    }
};

auto type_of(const aj::http_response& r) -> std::string {
    return std::string(json::parse(r.body).as_object().at("__type").as_string());
}

}  // namespace

BOOST_AUTO_TEST_CASE(update_stream_mode_switches_and_refuses) {
    fixture f;
    f.create("s", 2);
    BOOST_TEST(f.mode("s") == "PROVISIONED");
    BOOST_TEST(f.set_mode("s", "ON_DEMAND").status == 200U);
    BOOST_TEST(f.mode("s") == "ON_DEMAND");
    BOOST_TEST(f.open_shards("s") == 2U);
    // The mode it has: nothing changes, and it does not count.
    BOOST_TEST(f.set_mode("s", "ON_DEMAND").status == 200U);
    BOOST_TEST(f.set_mode("s", "PROVISIONED").status == 200U);
    // A third switch within 24 hours.
    const auto third = f.set_mode("s", "ON_DEMAND");
    BOOST_TEST(third.status == 400U);
    BOOST_TEST(type_of(third) == "LimitExceededException");
    BOOST_TEST(f.mode("s") == "PROVISIONED");
    // A day after the first switch, one more is allowed.
    f.now += std::int64_t{24} * 3600 * 1000;
    BOOST_TEST(f.set_mode("s", "ON_DEMAND").status == 200U);
    BOOST_TEST(f.mode("s") == "ON_DEMAND");

    const auto bad = f.set_mode("s", "SOMETIMES");
    // The model's enum: the front end refuses it before the handler.
    BOOST_TEST(type_of(bad) == "ValidationException");
    const auto warm =
        f.call("UpdateStreamMode", {{"StreamARN", f.arn("s")},
                                    {"StreamModeDetails", {{"StreamMode", "ON_DEMAND"}}},
                                    {"WarmThroughputMiBps", 100}});
    BOOST_TEST(type_of(warm) == "InvalidArgumentException");
}

BOOST_AUTO_TEST_CASE(on_demand_streams_refuse_manual_resharding) {
    fixture f;
    (void)f.ok("CreateStream",
               {{"StreamName", "d"}, {"StreamModeDetails", {{"StreamMode", "ON_DEMAND"}}}});
    BOOST_TEST(f.open_shards("d") == 4U);
    for (const auto& [op, body] : std::vector<std::pair<std::string, json::object>>{
             {"SplitShard",
              {{"StreamName", "d"},
               {"ShardToSplit", "shardId-000000000000"},
               {"NewStartingHashKey", "1000"}}},
             {"MergeShards",
              {{"StreamName", "d"},
               {"ShardToMerge", "shardId-000000000000"},
               {"AdjacentShardToMerge", "shardId-000000000001"}}},
             {"UpdateShardCount",
              {{"StreamName", "d"},
               {"TargetShardCount", 5},
               {"ScalingType", "UNIFORM_SCALING"}}}}) {
        const auto r = f.call(op, body);
        BOOST_TEST(r.status == 400U, op);
        BOOST_TEST(type_of(r) == "ValidationException", op);
    }
    // The refusal undid the UPDATING the reshard began with.
    BOOST_TEST(std::string(f.summary("d").at("StreamStatus").as_string()) == "ACTIVE");
    BOOST_TEST(f.open_shards("d") == 4U);
}

BOOST_AUTO_TEST_CASE(the_controller_splits_a_shard_that_stays_hot) {
    fixture f;
    kin::on_demand_controller<kin::local_stream_cluster> controller(
        f.cluster, *f.service, kin::on_demand_config{._window_ms = 10'000, ._sustain_ms = 30'000});
    (void)f.ok("CreateStream", {{"StreamName", "hot"},
                                {"ShardCount", 1},
                                {"StreamModeDetails", {{"StreamMode", "ON_DEMAND"}}}});
    f.create("prov", 1);  // provisioned: never split, however busy
    (void)f.ok("CreateStream", {{"StreamName", "quiet"},
                                {"ShardCount", 1},
                                {"StreamModeDetails", {{"StreamMode", "ON_DEMAND"}}}});

    // 700 records/s on "hot" and "prov", 10/s on "quiet", for 70 seconds.
    std::uint64_t splits = 0;
    for (int step = 0; step <= 7; ++step) {
        const auto st = controller.run_once(f.now);
        splits += st._splits;
        if (step == 7) {
            break;
        }
        f.write("hot", 7000);
        f.write("prov", 7000);
        f.write("quiet", 100);
        f.now += 10'000;
    }
    BOOST_TEST(splits == 1U);
    BOOST_TEST(f.open_shards("hot") == 2U);
    BOOST_TEST(f.open_shards("prov") == 1U);
    BOOST_TEST(f.open_shards("quiet") == 1U);
    const auto shards = f.ok("ListShards", {{"StreamName", "hot"}}).at("Shards").as_array();
    BOOST_REQUIRE(shards.size() == 3U);
    BOOST_TEST(shards[1].as_object().at("ParentShardId").as_string() == "shardId-000000000000");
    BOOST_TEST(std::string(f.summary("hot").at("StreamStatus").as_string()) == "ACTIVE");

    // Big records count by bytes: 60 records/s of 10 KiB is 600 KiB/s.
    f.now += 10'000;
    (void)controller.run_once(f.now);
    for (int step = 0; step < 5; ++step) {
        f.write("quiet", 600, 10 * 1024);
        f.now += 10'000;
        splits += controller.run_once(f.now)._splits;
    }
    BOOST_TEST(f.open_shards("quiet") == 2U);
}

BOOST_AUTO_TEST_CASE(a_stream_back_in_provisioned_mode_is_left_alone) {
    fixture f;
    kin::on_demand_controller<kin::local_stream_cluster> controller(
        f.cluster, *f.service, kin::on_demand_config{._window_ms = 10'000, ._sustain_ms = 20'000});
    (void)f.ok("CreateStream", {{"StreamName", "s"},
                                {"ShardCount", 1},
                                {"StreamModeDetails", {{"StreamMode", "ON_DEMAND"}}}});
    (void)controller.run_once(f.now);
    f.write("s", 7000);
    f.now += 10'000;
    BOOST_TEST(controller.run_once(f.now)._hot == 1U);
    BOOST_TEST(f.set_mode("s", "PROVISIONED").status == 200U);
    for (int step = 0; step < 4; ++step) {
        f.write("s", 7000);
        f.now += 10'000;
        BOOST_TEST(controller.run_once(f.now)._splits == 0U);
    }
    BOOST_TEST(f.open_shards("s") == 1U);
}

// AWS's on-demand stream limit (50 per account by default) holds both when
// a stream is created on demand and when one switches to it; the refused
// switch leaves the stream as it was.
BOOST_AUTO_TEST_CASE(the_on_demand_stream_limit_holds_at_create_and_at_switch) {
    {
        fixture f;
        BOOST_TEST(
            f.ok("DescribeLimits", json::object{}).at("OnDemandStreamCountLimit").as_int64() == 50);
    }
    kin::kinesis_service_config cfg;
    cfg._limits._on_demand_stream_limit = 2;
    fixture f(std::move(cfg));
    const auto create_on_demand = [&](std::string_view name) {
        return f.call("CreateStream",
                      {{"StreamName", name}, {"StreamModeDetails", {{"StreamMode", "ON_DEMAND"}}}});
    };
    BOOST_TEST(create_on_demand("a").status == 200U);
    BOOST_TEST(create_on_demand("b").status == 200U);
    const auto third = create_on_demand("c");
    BOOST_TEST(third.status == 400U);
    BOOST_TEST(type_of(third) == "LimitExceededException");
    f.create("p", 1);
    const auto sw = f.set_mode("p", "ON_DEMAND");
    BOOST_TEST(type_of(sw) == "LimitExceededException");
    BOOST_TEST(f.mode("p") == "PROVISIONED");
    const auto limits = f.ok("DescribeLimits", json::object{});
    BOOST_TEST(limits.at("OnDemandStreamCount").as_int64() == 2);
    BOOST_TEST(limits.at("OnDemandStreamCountLimit").as_int64() == 2);
    // Leaving on-demand frees a place.
    BOOST_TEST(f.set_mode("a", "PROVISIONED").status == 200U);
    BOOST_TEST(f.set_mode("p", "ON_DEMAND").status == 200U);
}

// doc/on-demand.md: the controller never splits past its ceiling of open
// shards and never merges, however long traffic stays quiet.
BOOST_AUTO_TEST_CASE(the_controller_stops_at_its_ceiling_and_never_merges) {
    fixture f;
    kin::on_demand_controller<kin::local_stream_cluster> controller(
        f.cluster, *f.service,
        kin::on_demand_config{._window_ms = 10'000, ._sustain_ms = 20'000, ._max_open_shards = 2});
    (void)f.ok("CreateStream", {{"StreamName", "s"},
                                {"ShardCount", 1},
                                {"StreamModeDetails", {{"StreamMode", "ON_DEMAND"}}}});
    std::uint64_t splits = 0;
    std::uint64_t hot = 0;
    (void)controller.run_once(f.now);
    for (int step = 0; step < 10; ++step) {
        // 1400 records/s over the key space: hot on one shard or two.
        f.write("s", 14'000);
        f.now += 10'000;
        const auto st = controller.run_once(f.now);
        splits += st._splits;
        hot += st._hot;
    }
    BOOST_TEST(splits == 1U);
    BOOST_TEST(hot > 2U);  // still hot after the split, yet no second split
    BOOST_TEST(f.open_shards("s") == 2U);
    // A long quiet spell: nothing merges.
    for (int step = 0; step < 20; ++step) {
        f.now += 60'000;
        splits += controller.run_once(f.now)._splits;
    }
    BOOST_TEST(splits == 1U);
    BOOST_TEST(f.open_shards("s") == 2U);
    BOOST_TEST(f.shards("s").size() == 3U);
}
