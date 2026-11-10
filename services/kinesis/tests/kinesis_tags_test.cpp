// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Tags, resource policies, enhanced monitoring and the account calls
// (`.kiro/specs/kinesis-compatible-streams/` task 15, Requirements 14.2-14.5)
// through the front end on the in-process cluster: both tag forms with
// their paging and limits, policies stored and handed to authorization with
// the stream's tags, the shard-level metric switches, and the account
// settings.

#define BOOST_TEST_MODULE kinesis_tags_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
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
        _path =
            std::filesystem::temp_directory_path() /
            ("kinesis_tags_test." + std::to_string(::getpid()) + "." + std::to_string(++counter));
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
    std::vector<az::authz_request> _seen;
    auto authenticate(const sv4::request&, const sv4::service_rules&) -> az::authn_result {
        az::principal_context p;
        p.account = std::string(k_account);
        p.arn = "arn:aws:iam::" + std::string(k_account) + ":user/alice";
        return p;
    }
    auto authorize(const az::principal_context&, const az::authz_request& r) -> az::authz_decision {
        _seen.push_back(r);
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

    fixture() {
        service = std::make_unique<kin::kinesis_service<kin::local_stream_cluster>>(
            cluster, kin::kinesis_service_config{});
        service->register_with(fe);
        fe.set_resource_lookup(
            [this](std::string_view arn) { return service->resource_facts_of(arn); });
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

    auto arn(std::string_view stream) -> std::string {
        return std::string(ok("DescribeStreamSummary", {{"StreamName", stream}})
                               .at("StreamDescriptionSummary")
                               .as_object()
                               .at("StreamARN")
                               .as_string());
    }

    auto tags(std::string_view stream) -> std::map<std::string, std::string> {
        std::map<std::string, std::string> out;
        const auto r = ok("ListTagsForStream", {{"StreamName", stream}, {"Limit", 50}});
        for (const auto& t : r.at("Tags").as_array()) {
            out[std::string(t.as_object().at("Key").as_string())] =
                std::string(t.as_object().at("Value").as_string());
        }
        return out;
    }
};

auto type_of(const aj::http_response& r) -> std::string {
    return std::string(json::parse(r.body).as_object().at("__type").as_string());
}

}  // namespace

BOOST_AUTO_TEST_CASE(stream_tags_add_overwrite_page_and_remove) {
    fixture f;
    f.create("s", 1);
    (void)f.ok("AddTagsToStream", {{"StreamName", "s"}, {"Tags", {{"b", "1"}, {"a", "2"}}}});
    (void)f.ok("AddTagsToStream", {{"StreamName", "s"}, {"Tags", {{"b", "3"}, {"c", ""}}}});
    BOOST_TEST(
        (f.tags("s") == std::map<std::string, std::string>{{"a", "2"}, {"b", "3"}, {"c", ""}}));

    const auto page = f.ok("ListTagsForStream", {{"StreamName", "s"}, {"Limit", 2}});
    BOOST_TEST(page.at("Tags").as_array().size() == 2U);
    BOOST_TEST(page.at("HasMoreTags").as_bool());
    const auto rest = f.ok("ListTagsForStream",
                           {{"StreamName", "s"}, {"ExclusiveStartTagKey", "b"}, {"Limit", 2}});
    BOOST_REQUIRE(rest.at("Tags").as_array().size() == 1U);
    BOOST_TEST(rest.at("Tags").as_array()[0].as_object().at("Key").as_string() == "c");
    BOOST_TEST(!rest.at("HasMoreTags").as_bool());

    (void)f.ok("RemoveTagsFromStream", {{"StreamName", "s"}, {"TagKeys", {"a", "zz"}}});
    BOOST_TEST((f.tags("s") == std::map<std::string, std::string>{{"b", "3"}, {"c", ""}}));

    // The limit is on the stream's total.
    json::object many;
    for (int i = 0; i < 48; ++i) {
        many["k" + std::to_string(i)] = "v";
    }
    (void)f.ok("AddTagsToStream", {{"StreamName", "s"}, {"Tags", many}});
    const auto over = f.call("AddTagsToStream", {{"StreamName", "s"}, {"Tags", {{"one", "more"}}}});
    BOOST_TEST(type_of(over) == "LimitExceededException");
    BOOST_TEST(f.tags("s").size() == 50U);

    // A profile tag is checked like at CreateStream.
    const auto bad =
        f.call("AddTagsToStream",
               {{"StreamName", "s"}, {"Tags", {{"kythira:compat:throttling", "maybe"}}}});
    BOOST_TEST(bad.status == 400U);
    BOOST_TEST(f.tags("s").count("kythira:compat:throttling") == 0U);
}

BOOST_AUTO_TEST_CASE(resource_tags_name_the_stream_by_arn) {
    fixture f;
    f.create("s", 1);
    const auto arn = f.arn("s");
    (void)f.ok("TagResource", {{"ResourceARN", arn}, {"Tags", {{"team", "data"}, {"x", "y"}}}});
    auto listed = f.ok("ListTagsForResource", {{"ResourceARN", arn}});
    BOOST_TEST(listed.at("Tags").as_array().size() == 2U);
    (void)f.ok("UntagResource", {{"ResourceARN", arn}, {"TagKeys", {"x"}}});
    BOOST_TEST((f.tags("s") == std::map<std::string, std::string>{{"team", "data"}}));

    // Authorization saw the stream's tags through the resource lookup.
    f.auth._seen.clear();
    (void)f.ok("ListTagsForResource", {{"ResourceARN", arn}});
    BOOST_REQUIRE(!f.auth._seen.empty());
    BOOST_TEST(f.auth._seen.back().action == "kinesis:ListTagsForResource");
    BOOST_TEST(f.auth._seen.back().resource == arn);
    BOOST_TEST((f.auth._seen.back().resource_tags ==
                std::vector<std::pair<std::string, std::string>>{{"team", "data"}}));

    const auto consumer =
        f.call("ListTagsForResource", {{"ResourceARN", arn + "/consumer/c:1700000000"}});
    BOOST_TEST(type_of(consumer) == "ResourceNotFoundException");
    const auto missing =
        f.call("ListTagsForResource", {{"ResourceARN", arn.substr(0, arn.rfind('/')) + "/nope"}});
    BOOST_TEST(type_of(missing) == "ResourceNotFoundException");
}

BOOST_AUTO_TEST_CASE(resource_policies_are_stored_and_reach_authorization) {
    fixture f;
    f.create("s", 1);
    const auto arn = f.arn("s");
    BOOST_TEST(f.ok("GetResourcePolicy", {{"ResourceARN", arn}}).at("Policy").as_string() == "{}");
    const auto bad = f.call("PutResourcePolicy", {{"ResourceARN", arn}, {"Policy", "not json"}});
    BOOST_TEST(type_of(bad) == "InvalidArgumentException");
    const std::string policy =
        R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"AWS":"arn:aws:iam::444455556666:root"},"Action":"kinesis:GetRecords","Resource":"*"}]})";
    (void)f.ok("PutResourcePolicy", {{"ResourceARN", arn}, {"Policy", policy}});
    BOOST_TEST(f.ok("GetResourcePolicy", {{"ResourceARN", arn}}).at("Policy").as_string() ==
               policy);

    f.auth._seen.clear();
    (void)f.ok("DescribeStreamSummary", {{"StreamName", "s"}});
    BOOST_REQUIRE(!f.auth._seen.empty());
    BOOST_REQUIRE(f.auth._seen.back().resource_policy.has_value());
    BOOST_TEST(*f.auth._seen.back().resource_policy == policy);

    (void)f.ok("DeleteResourcePolicy", {{"ResourceARN", arn}});
    BOOST_TEST(f.ok("GetResourcePolicy", {{"ResourceARN", arn}}).at("Policy").as_string() == "{}");
    const auto again = f.call("DeleteResourcePolicy", {{"ResourceARN", arn}});
    BOOST_TEST(type_of(again) == "ResourceNotFoundException");
}

BOOST_AUTO_TEST_CASE(enhanced_monitoring_switches_shard_metrics) {
    fixture f;
    f.create("s", 1);
    auto on = f.ok("EnableEnhancedMonitoring",
                   {{"StreamName", "s"}, {"ShardLevelMetrics", {"IncomingBytes"}}});
    BOOST_TEST(on.at("CurrentShardLevelMetrics").as_array().empty());
    BOOST_TEST(on.at("DesiredShardLevelMetrics").as_array().size() == 1U);
    on = f.ok("EnableEnhancedMonitoring", {{"StreamName", "s"}, {"ShardLevelMetrics", {"ALL"}}});
    BOOST_TEST(on.at("CurrentShardLevelMetrics").as_array().size() == 1U);
    BOOST_TEST(on.at("DesiredShardLevelMetrics").as_array().size() == 7U);
    const auto off =
        f.ok("DisableEnhancedMonitoring",
             {{"StreamName", "s"}, {"ShardLevelMetrics", {"IncomingBytes", "OutgoingBytes"}}});
    BOOST_TEST(off.at("DesiredShardLevelMetrics").as_array().size() == 5U);
    const auto d = f.ok("DescribeStream", {{"StreamName", "s"}})
                       .at("StreamDescription")
                       .as_object()
                       .at("EnhancedMonitoring")
                       .as_array();
    BOOST_REQUIRE(d.size() == 1U);
    BOOST_TEST(d[0].as_object().at("ShardLevelMetrics").as_array().size() == 5U);
}

BOOST_AUTO_TEST_CASE(account_settings_and_record_size) {
    fixture f;
    f.create("s", 1);
    const auto a = f.ok("DescribeAccountSettings", json::object{});
    BOOST_TEST(a.at("MinimumThroughputBillingCommitment").as_object().at("Status").as_string() ==
               "DISABLED");
    const auto enable = f.call("UpdateAccountSettings",
                               {{"MinimumThroughputBillingCommitment", {{"Status", "ENABLED"}}}});
    BOOST_TEST(type_of(enable) == "InvalidArgumentException");
    (void)f.ok("UpdateAccountSettings",
               {{"MinimumThroughputBillingCommitment", {{"Status", "DISABLED"}}}});

    (void)f.ok("UpdateMaxRecordSize", {{"StreamARN", f.arn("s")}, {"MaxRecordSizeInKiB", 1024}});
    const auto big =
        f.call("UpdateMaxRecordSize", {{"StreamARN", f.arn("s")}, {"MaxRecordSizeInKiB", 2048}});
    BOOST_TEST(type_of(big) == "InvalidArgumentException");
}

BOOST_AUTO_TEST_CASE(metrics_count_traffic_and_follow_the_monitoring_switches) {
    fixture f;
    f.create("m", 1);
    json::array batch;
    for (int i = 0; i < 3; ++i) {
        batch.push_back({{"PartitionKey", "k" + std::to_string(i)}, {"Data", "aGVsbG8="}});
    }
    (void)f.ok("PutRecords", {{"StreamName", "m"}, {"Records", batch}});
    const auto it = std::string(f.ok("GetShardIterator", {{"StreamName", "m"},
                                                          {"ShardId", "shardId-000000000000"},
                                                          {"ShardIteratorType", "TRIM_HORIZON"}})
                                    .at("ShardIterator")
                                    .as_string());
    (void)f.ok("GetRecords", {{"ShardIterator", it}});

    const std::string labels = R"({account="111122223333",stream="m"})";
    auto text = f.service->metrics_text();
    // 3 records of 5 data bytes and a 2-byte key.
    BOOST_TEST(text.find("kinesis_incoming_records_total" + labels + " 3\n") != std::string::npos,
               text);
    BOOST_TEST(text.find("kinesis_incoming_bytes_total" + labels + " 21\n") != std::string::npos);
    BOOST_TEST(text.find("kinesis_outgoing_records_total" + labels + " 3\n") != std::string::npos);
    BOOST_TEST(text.find("kinesis_get_records_latency_milliseconds_count" + labels + " 1\n") !=
               std::string::npos);
    // No shard-level traffic series until monitoring is switched on; the
    // segment bytes are always per shard.
    BOOST_TEST(
        text.find(R"(kinesis_incoming_records_total{account="111122223333",stream="m",shard=)") ==
        std::string::npos);
    BOOST_TEST(
        text.find(
            R"(kinesis_segment_bytes{account="111122223333",stream="m",shard="shardId-000000000000",location="local"})") !=
        std::string::npos);

    (void)f.ok("EnableEnhancedMonitoring",
               {{"StreamName", "m"}, {"ShardLevelMetrics", {"IncomingRecords"}}});
    text = f.service->metrics_text();
    BOOST_TEST(
        text.find(
            R"(kinesis_incoming_records_total{account="111122223333",stream="m",shard="shardId-000000000000"} 3)") !=
        std::string::npos);
    BOOST_TEST(
        text.find(R"(kinesis_outgoing_records_total{account="111122223333",stream="m",shard=)") ==
        std::string::npos);
}

// AWS's resource policy limit: 20 KiB. A policy of exactly that size is
// stored; one byte more is refused (LimitExceededException, AWS's error
// for a limit; not yet checked against a recording) and the old policy
// stays.
BOOST_AUTO_TEST_CASE(a_policy_over_20_kib_is_refused) {
    fixture f;
    f.create("s", 1);
    const auto arn = f.arn("s");
    const auto policy_of_size = [](std::size_t size) {
        const std::string head = R"({"Version":"2012-10-17","Statement":[{"Sid":")";
        const std::string tail =
            R"(","Effect":"Allow","Principal":{"AWS":"arn:aws:iam::444455556666:root"},"Action":"kinesis:GetRecords","Resource":"*"}]})";
        return head + std::string(size - head.size() - tail.size(), 'x') + tail;
    };
    const auto fits = policy_of_size(20 * 1024);
    BOOST_REQUIRE(fits.size() == 20U * 1024U);
    BOOST_TEST(f.call("PutResourcePolicy", {{"ResourceARN", arn}, {"Policy", fits}}).status ==
               200U);
    const auto over = f.call("PutResourcePolicy",
                             {{"ResourceARN", arn}, {"Policy", policy_of_size(20 * 1024 + 1)}});
    BOOST_TEST(over.status == 400U);
    BOOST_TEST(type_of(over) == "LimitExceededException");
    BOOST_TEST(f.ok("GetResourcePolicy", {{"ResourceARN", arn}}).at("Policy").as_string() == fits);
}

// doc/tags-policies-metrics.md: tags change only on an ACTIVE or UPDATING
// stream. A stream still CREATING answers ResourceInUseException.
BOOST_AUTO_TEST_CASE(tags_change_only_on_an_active_or_updating_stream) {
    fixture f;
    kin::create_stream_command c;
    c._account = std::string(k_account);
    c._name = "half";
    c._shard_count = 1;
    const kin::stream_command cmd = c;
    BOOST_REQUIRE(f.cluster.propose(kin::command_key(cmd), kin::encode_command(cmd))->_status ==
                  kin::apply_status::ok);
    BOOST_TEST(f.status("half") == "CREATING");
    const auto add =
        f.call("AddTagsToStream", {{"StreamName", "half"}, {"Tags", {{"team", "data"}}}});
    BOOST_TEST(type_of(add) == "ResourceInUseException");
    const auto remove =
        f.call("RemoveTagsFromStream", {{"StreamName", "half"}, {"TagKeys", json::array{"team"}}});
    BOOST_TEST(type_of(remove) == "ResourceInUseException");
}

// doc/tags-policies-metrics.md: the 50-tag limit is checked again when the
// entry applies, so two changes that each pass the front end's check
// against the same old tag set cannot both commit.
BOOST_AUTO_TEST_CASE(the_tag_limit_is_rechecked_when_the_entry_applies) {
    fixture f;
    f.create("s", 1);
    std::uint64_t id = 0;
    BOOST_REQUIRE(f.cluster.read(kin::stream_key(std::string(k_account), "s"), [&](const auto& sm) {
        id = sm.find_stream(std::string(k_account), "s")->_id;
    }));
    const auto change = [&](char prefix) {
        kin::tag_stream_command t;
        t._account = std::string(k_account);
        t._name = "s";
        t._id = id;
        for (int i = 0; i < 30; ++i) {
            t._set[std::string(1, prefix) + std::to_string(i)] = "v";
        }
        const kin::stream_command cmd = t;
        return f.cluster.propose(kin::command_key(cmd), kin::encode_command(cmd))->_status;
    };
    BOOST_TEST((change('a') == kin::apply_status::ok));
    BOOST_TEST((change('b') == kin::apply_status::invalid));
    BOOST_TEST(f.tags("s").size() == 30U);
    BOOST_TEST(f.tags("s").count("b0") == 0U);
}
