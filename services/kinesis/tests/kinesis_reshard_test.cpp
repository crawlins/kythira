// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Resharding (`.kiro/specs/kinesis-compatible-streams/` task 11,
// Requirements 10.1 and 10.2) through the front end on the in-process
// cluster: SplitShard and MergeShards close their parents at an ending
// sequence number and open children with lineage; writes after a reshard go
// to the children; a parent read to its end answers its ChildShards and
// keeps its records readable; AWS's refusals; UpdateShardCount converging on
// the target and keeping the key space tiled; and a reshard abandoned after
// its transaction finished by the maintenance repair.

#define BOOST_TEST_MODULE kinesis_reshard_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_hash.hpp>
#include <kythira/kinesis/kinesis_service.hpp>

#include <raft/record_segment_store.hpp>

#include <boost/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <map>
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
                ("kinesis_reshard_test." + std::to_string(::getpid()) + "." +
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
    kin::local_stream_cluster cluster{store.get()};
    allow_all auth;
    kin::front_end<allow_all> fe{kin::front_end_config{}, auth};
    std::unique_ptr<kin::kinesis_service<kin::local_stream_cluster>> service;

    fixture() {
        service = std::make_unique<kin::kinesis_service<kin::local_stream_cluster>>(
            cluster, kin::kinesis_service_config{});
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

    // Reads one shard from TRIM_HORIZON to its end (or tip): the sequence
    // numbers, and the last response.
    auto drain(std::string_view stream, std::string_view shard)
        -> std::pair<std::vector<std::string>, json::object> {
        auto it = std::string(
            ok("GetShardIterator",
               {{"StreamName", stream}, {"ShardId", shard}, {"ShardIteratorType", "TRIM_HORIZON"}})
                .at("ShardIterator")
                .as_string());
        std::vector<std::string> seqs;
        for (int i = 0; i < 20; ++i) {
            auto r = ok("GetRecords", {{"ShardIterator", it}});
            for (const auto& rec : r.at("Records").as_array()) {
                seqs.emplace_back(rec.as_object().at("SequenceNumber").as_string());
            }
            if (!r.contains("NextShardIterator") || r.at("NextShardIterator").is_null() ||
                r.at("Records").as_array().empty()) {
                return {seqs, r};
            }
            it = std::string(r.at("NextShardIterator").as_string());
        }
        return {seqs, {}};
    }
};

auto type_of(const aj::http_response& r) -> std::string {
    return std::string(json::parse(r.body).as_object().at("__type").as_string());
}

auto message_of(const aj::http_response& r) -> std::string {
    return std::string(json::parse(r.body).as_object().at("message").as_string());
}

auto range_of(const json::object& shard) -> kin::hash_range {
    const auto& r = shard.at("HashKeyRange").as_object();
    return {*kin::parse_hash_key(std::string(r.at("StartingHashKey").as_string())),
            *kin::parse_hash_key(std::string(r.at("EndingHashKey").as_string()))};
}

// The open shards' ranges cover the whole key space with no gap or overlap.
auto tiles(const std::map<std::string, json::object>& shards) -> bool {
    std::vector<kin::hash_range> open;
    for (const auto& [id, s] : shards) {
        if (!s.at("SequenceNumberRange").as_object().contains("EndingSequenceNumber")) {
            open.push_back(range_of(s));
        }
    }
    std::sort(open.begin(), open.end(),
              [](const auto& a, const auto& b) { return a._start < b._start; });
    if (open.empty() || open.front()._start != 0 || open.back()._end != kin::max_hash_key) {
        return false;
    }
    for (std::size_t i = 1; i < open.size(); ++i) {
        if (open[i - 1]._end + 1 != open[i]._start) {
            return false;
        }
    }
    return true;
}

const std::string k_low = "1000";
const std::string k_high = "300000000000000000000000000000000000000";  // above 2^127

}  // namespace

BOOST_AUTO_TEST_CASE(split_closes_the_parent_and_routes_new_writes_to_children) {
    fixture f;
    f.create("s", 1);
    (void)f.put("s", {k_low, k_high, k_low});
    const auto mid = kin::to_decimal(kin::hash_key{1} << 127U);
    (void)f.ok("SplitShard", {{"StreamName", "s"},
                              {"ShardToSplit", "shardId-000000000000"},
                              {"NewStartingHashKey", mid}});
    BOOST_TEST(f.status("s") == "ACTIVE");

    auto shards = f.shards("s");
    BOOST_REQUIRE(shards.size() == 3U);
    const auto& parent = shards.at("shardId-000000000000");
    const auto& seq = parent.at("SequenceNumberRange").as_object();
    BOOST_REQUIRE(seq.contains("EndingSequenceNumber"));
    const auto& low = shards.at("shardId-000000000001");
    const auto& high = shards.at("shardId-000000000002");
    BOOST_TEST(low.at("ParentShardId").as_string() == "shardId-000000000000");
    BOOST_TEST(high.at("ParentShardId").as_string() == "shardId-000000000000");
    BOOST_TEST(kin::to_decimal(range_of(low)._end + 1) == mid);
    BOOST_TEST(kin::to_decimal(range_of(high)._start) == mid);
    BOOST_TEST(tiles(shards));

    // New writes land in the child owning their key.
    const auto landed = f.put("s", {k_low, k_high});
    BOOST_TEST(landed[0] == "shardId-000000000001");
    BOOST_TEST(landed[1] == "shardId-000000000002");

    // The parent keeps its records and, read to its end, names its children.
    const auto [records, last] = f.drain("s", "shardId-000000000000");
    BOOST_TEST(records.size() == 3U);
    // Its ending number is past its last record and carried by none, as
    // AWS answers.
    BOOST_TEST(records.back() < std::string(seq.at("EndingSequenceNumber").as_string()));
    BOOST_TEST(records.back().size() == seq.at("EndingSequenceNumber").as_string().size());
    BOOST_TEST(!last.contains("NextShardIterator"));
    BOOST_REQUIRE(last.contains("ChildShards"));
    BOOST_TEST(last.at("ChildShards").as_array().size() == 2U);
    // A child's records follow the parent's for the same key.
    BOOST_TEST(f.drain("s", "shardId-000000000001").first.size() == 1U);
}

BOOST_AUTO_TEST_CASE(merge_closes_both_parents_into_one_child) {
    fixture f;
    f.create("s", 2);
    (void)f.put("s", {k_low, k_high});
    (void)f.ok("MergeShards", {{"StreamName", "s"},
                               {"ShardToMerge", "shardId-000000000000"},
                               {"AdjacentShardToMerge", "shardId-000000000001"}});
    auto shards = f.shards("s");
    BOOST_REQUIRE(shards.size() == 3U);
    const auto& c = shards.at("shardId-000000000002");
    BOOST_TEST(c.at("ParentShardId").as_string() == "shardId-000000000000");
    BOOST_TEST(c.at("AdjacentParentShardId").as_string() == "shardId-000000000001");
    BOOST_TEST((range_of(c)._start == 0U));
    BOOST_TEST((range_of(c)._end == kin::max_hash_key));
    BOOST_TEST(tiles(shards));
    for (const auto* id : {"shardId-000000000000", "shardId-000000000001"}) {
        BOOST_TEST(
            shards.at(id).at("SequenceNumberRange").as_object().contains("EndingSequenceNumber"));
    }
    const auto landed = f.put("s", {k_low, k_high});
    BOOST_TEST(landed[0] == "shardId-000000000002");
    BOOST_TEST(landed[1] == "shardId-000000000002");
}

BOOST_AUTO_TEST_CASE(reshard_refusals_carry_aws_messages) {
    fixture f;
    f.create("s", 3);
    auto r = f.call("SplitShard", {{"StreamName", "s"},
                                   {"ShardToSplit", "shardId-000000000000"},
                                   {"NewStartingHashKey", "1"}});
    BOOST_TEST(type_of(r) == "InvalidArgumentException");
    BOOST_TEST(message_of(r).find("is not both greater than one plus the shard's "
                                  "StartingHashKey 0") != std::string::npos);
    r = f.call("SplitShard", {{"StreamName", "s"},
                              {"ShardToSplit", "shardId-000000000009"},
                              {"NewStartingHashKey", "100"}});
    BOOST_TEST(type_of(r) == "ResourceNotFoundException");
    r = f.call("MergeShards", {{"StreamName", "s"},
                               {"ShardToMerge", "shardId-000000000000"},
                               {"AdjacentShardToMerge", "shardId-000000000002"}});
    BOOST_TEST(type_of(r) == "InvalidArgumentException");
    BOOST_TEST(message_of(r) ==
               "Shards shardId-000000000000 and shardId-000000000002 in stream s under account "
               "111122223333 are not an adjacent pair of shards eligible for merging");

    (void)f.ok("MergeShards", {{"StreamName", "s"},
                               {"ShardToMerge", "shardId-000000000000"},
                               {"AdjacentShardToMerge", "shardId-000000000001"}});
    r = f.call("SplitShard", {{"StreamName", "s"},
                              {"ShardToSplit", "shardId-000000000000"},
                              {"NewStartingHashKey", "100"}});
    BOOST_TEST(type_of(r) == "ResourceInUseException");
    BOOST_TEST(message_of(r).find("has already been merged or split") != std::string::npos);

    r = f.call("UpdateShardCount",
               {{"StreamName", "s"}, {"TargetShardCount", 5}, {"ScalingType", "UNIFORM_SCALING"}});
    BOOST_TEST(message_of(r) ==
               "UpdateShardCount cannot scale up over double your current open shard count. "
               "Current open shard count: 2 Target shard count: 5");
    // Nothing above left the stream UPDATING.
    BOOST_TEST(f.status("s") == "ACTIVE");
}

BOOST_AUTO_TEST_CASE(update_shard_count_reaches_the_target_and_keeps_the_tiling) {
    fixture f;
    f.create("s", 2);
    auto r =
        f.ok("UpdateShardCount",
             {{"StreamName", "s"}, {"TargetShardCount", 3}, {"ScalingType", "UNIFORM_SCALING"}});
    BOOST_TEST(r.at("CurrentShardCount").as_int64() == 2);
    BOOST_TEST(r.at("TargetShardCount").as_int64() == 3);
    auto shards = f.shards("s");
    BOOST_TEST(tiles(shards));
    BOOST_TEST(f.ok("DescribeStreamSummary", {{"StreamName", "s"}})
                   .at("StreamDescriptionSummary")
                   .as_object()
                   .at("OpenShardCount")
                   .as_int64() == 3);

    (void)f.ok("UpdateShardCount",
               {{"StreamName", "s"}, {"TargetShardCount", 6}, {"ScalingType", "UNIFORM_SCALING"}});
    (void)f.ok("UpdateShardCount",
               {{"StreamName", "s"}, {"TargetShardCount", 3}, {"ScalingType", "UNIFORM_SCALING"}});
    shards = f.shards("s");
    BOOST_TEST(tiles(shards));
    std::size_t open = 0;
    for (const auto& [id, s] : shards) {
        open += s.at("SequenceNumberRange").as_object().contains("EndingSequenceNumber") ? 0 : 1;
    }
    BOOST_TEST(open == 3U);
    BOOST_TEST(f.status("s") == "ACTIVE");
}

BOOST_AUTO_TEST_CASE(an_abandoned_reshard_is_completed_by_the_repair) {
    fixture f;
    f.create("s", 1);
    (void)f.put("s", {k_low, k_low});
    // A coordinator that committed the transaction and died before the
    // ending counters and ACTIVE.
    const std::string account(k_account);
    const auto apply = [&](const kin::stream_command& c) {
        return f.cluster.propose(kin::command_key(c), kin::encode_command(c));
    };
    BOOST_REQUIRE(
        apply(kin::set_stream_status_command{account, "s", 1, kin::stream_status::updating}));
    kin::shard_entry a;
    a._ordinal = 1;
    a._parent = 0;
    a._range = {0, kin::hash_key{1} << 127U};
    kin::shard_entry b;
    b._ordinal = 2;
    b._parent = 0;
    b._range = {(kin::hash_key{1} << 127U) + 1, kin::max_hash_key};
    std::vector<kin::txn_write> writes;
    for (const kin::stream_command& c : std::vector<kin::stream_command>{
             kin::reshard_catalog_command{account, "s", 1, {0}, {a, b}},
             kin::close_shard_command{kin::routing_key(1, 0)},
             kin::open_shard_command{kin::routing_key(1, 1), 0},
             kin::open_shard_command{kin::routing_key(1, 2), 0}}) {
        writes.push_back({kin::command_key(c), kin::encode_command(c)});
    }
    const auto outcome = f.cluster.transact(std::move(writes));
    BOOST_REQUIRE((outcome == kin::transact_outcome::committed));
    BOOST_TEST(f.status("s") == "UPDATING");
    // Writes already go to the children.
    BOOST_TEST(f.put("s", {k_low})[0] == "shardId-000000000001");

    // First sighting starts the clock; only a stale one is repaired.
    BOOST_TEST(f.service->repair_reshards(1000, 30'000) == 0U);
    BOOST_TEST(f.service->repair_reshards(20'000, 30'000) == 0U);
    BOOST_TEST(f.service->repair_reshards(31'000, 30'000) == 1U);
    BOOST_TEST(f.status("s") == "ACTIVE");
    const auto shards = f.shards("s");
    const auto& seq = shards.at("shardId-000000000000").at("SequenceNumberRange").as_object();
    BOOST_REQUIRE(seq.contains("EndingSequenceNumber"));
    const auto [records, last] = f.drain("s", "shardId-000000000000");
    BOOST_TEST(records.size() == 2U);
    BOOST_TEST(last.contains("ChildShards"));
    // Nothing more to do.
    BOOST_TEST(f.service->repair_reshards(90'000, 30'000) == 0U);
}

// Requirement 7.2 across resharding: a partition key's next record lands in
// a child (or merged) shard whose sequence numbers are all above the
// parent's, so SequenceNumberForOrdering still yields a greater number,
// and string order stays integer order.
BOOST_AUTO_TEST_CASE(sequence_number_for_ordering_holds_across_a_split_and_a_merge) {
    fixture f;
    f.create("s", 1);
    const auto put_after = [&](const std::string& previous) {
        json::object body{{"StreamName", "s"},
                          {"PartitionKey", "pk"},
                          {"Data", "aGk="},
                          {"ExplicitHashKey", k_low}};
        if (!previous.empty()) {
            body["SequenceNumberForOrdering"] = previous;
        }
        const auto r = f.ok("PutRecord", body);
        return std::pair{std::string(r.at("SequenceNumber").as_string()),
                         std::string(r.at("ShardId").as_string())};
    };
    auto [a, a_shard] = put_after("");
    BOOST_TEST(a_shard == "shardId-000000000000");
    const auto mid = kin::to_decimal(kin::hash_key{1} << 127U);
    (void)f.ok("SplitShard", {{"StreamName", "s"},
                              {"ShardToSplit", "shardId-000000000000"},
                              {"NewStartingHashKey", mid}});
    auto [b, b_shard] = put_after(a);
    BOOST_TEST(b_shard == "shardId-000000000001");
    BOOST_TEST(b.size() == a.size());
    BOOST_TEST(b > a);
    (void)f.ok("MergeShards", {{"StreamName", "s"},
                               {"ShardToMerge", "shardId-000000000001"},
                               {"AdjacentShardToMerge", "shardId-000000000002"}});
    auto [c, c_shard] = put_after(b);
    BOOST_TEST(c_shard == "shardId-000000000003");
    BOOST_TEST(c > b);
    // A child's first number is above its parent's last.
    const auto shards = f.shards("s");
    const auto& parent = shards.at("shardId-000000000000").at("SequenceNumberRange").as_object();
    const auto& child = shards.at("shardId-000000000001").at("SequenceNumberRange").as_object();
    BOOST_TEST(std::string(child.at("StartingSequenceNumber").as_string()) >
               std::string(parent.at("EndingSequenceNumber").as_string()));
}

// AWS's UpdateShardCount bounds: no more than double and no less than half
// the open shards in one call. A refused call leaves the stream ACTIVE and
// its shards untouched.
BOOST_AUTO_TEST_CASE(update_shard_count_stays_within_half_and_double) {
    fixture f;
    f.create("s", 4);
    const auto update = [&](int target) {
        return f.call("UpdateShardCount", {{"StreamName", "s"},
                                           {"TargetShardCount", target},
                                           {"ScalingType", "UNIFORM_SCALING"}});
    };
    auto r = update(1);
    BOOST_TEST(type_of(r) == "InvalidArgumentException");
    BOOST_TEST(message_of(r) ==
               "UpdateShardCount cannot scale down below half your current open shard count. "
               "Current open shard count: 4 Target shard count: 1");
    r = update(9);
    BOOST_TEST(type_of(r) == "InvalidArgumentException");
    BOOST_TEST(f.status("s") == "ACTIVE");
    BOOST_TEST(f.shards("s").size() == 4U);
    // The bounds themselves are allowed.
    BOOST_TEST(update(2).status == 200U);
    BOOST_TEST(f.status("s") == "ACTIVE");
    std::size_t open = 0;
    for (const auto& [id, s] : f.shards("s")) {
        open += s.at("SequenceNumberRange").as_object().contains("EndingSequenceNumber") ? 0 : 1;
    }
    BOOST_TEST(open == 2U);
}
