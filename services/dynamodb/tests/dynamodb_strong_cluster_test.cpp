// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Multi-region strong consistency (`.kiro/specs/dynamodb-compatible-api/`
// task 27) on three in-process regions: each a one-node
// dynamodb_gateway_node cluster with a strong host, the three strong hosts
// voters of one set of groups. Writes in one region read strongly in
// another, conditional writes race across regions, a region is lost, cut
// off and brought back, groups split under load, and a witness stands in
// for a third replica.

#define BOOST_TEST_MODULE dynamodb_strong_cluster_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_cluster_fixture.hpp"

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

// The default (Folly) backend needs folly::Init before the first Raft
// timer asks for folly's Timekeeper singleton.
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("dynamodb_strong_cluster_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

using namespace dynamodb_cluster_test;

namespace {

const std::string k_create =
    R"({"TableName":"accounts","BillingMode":"PAY_PER_REQUEST",)"
    R"("AttributeDefinitions":[{"AttributeName":"id","AttributeType":"S"}],)"
    R"("KeySchema":[{"AttributeName":"id","KeyType":"HASH"}]})";

/// Waits up to `limit` for region `i`'s description of `accounts` to
/// satisfy `ready`.
template<typename Ready>
auto described(cluster& c, int i, Ready&& ready,
               std::chrono::seconds limit = std::chrono::seconds(90)) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto d = c.call_until_served(i, "DescribeTable", R"({"TableName":"accounts"})");
        if (ready(d)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

auto all_active(const json::array& members, std::string_view status_field) -> bool {
    return std::all_of(members.begin(), members.end(), [&](const json::value& r) {
        return r.at(status_field).as_string() == "ACTIVE";
    });
}

/// Creates `accounts` in region 0 and makes it strong with replicas in
/// the other two regions, or with `witness` a replica in region 1 and a
/// witness in region 2; returns once every replica region describes it
/// with every member ACTIVE.
auto make_strong_table(cluster& c, bool witness = false) -> void {
    for (int i = 0; i < k_nodes; ++i) {
        BOOST_REQUIRE(c.wait_for_log(i, "strong host ready", std::chrono::seconds(60)));
    }
    const auto created = c.call_until_served(0, "CreateTable", k_create);
    BOOST_REQUIRE_MESSAGE(created.status == 200U, json::serialize(created.body));
    BOOST_REQUIRE(described(c, 0, [](const answer& d) {
        return d.status == 200U && d.body.at("Table").at("TableStatus").as_string() == "ACTIVE";
    }));
    const std::string update =
        witness
            ? R"({"TableName":"accounts","MultiRegionConsistency":"STRONG",)"
              R"("ReplicaUpdates":[{"Create":{"RegionName":"us-east-2"}}],)"
              R"("GlobalTableWitnessUpdates":[{"Create":{"RegionName":"us-west-2"}}]})"
            : R"({"TableName":"accounts","MultiRegionConsistency":"STRONG","ReplicaUpdates":[)"
              R"({"Create":{"RegionName":"us-east-2"}},{"Create":{"RegionName":"us-west-2"}}]})";
    const auto made = c.call_until_served(0, "UpdateTable", update);
    BOOST_REQUIRE_MESSAGE(made.status == 200U, json::serialize(made.body));
    const int replica_regions = witness ? 2 : 3;
    for (int i = 0; i < replica_regions; ++i) {
        BOOST_TEST_INFO("region " << cluster::k_regions.at(i));
        BOOST_REQUIRE(described(c, i, [&](const answer& d) {
            if (d.status != 200U) {
                return false;
            }
            const auto& t = d.body.at("Table").as_object();
            if (!t.contains("MultiRegionConsistency") || !t.contains("Replicas")) {
                return false;
            }
            const auto& replicas = t.at("Replicas").as_array();
            if (replicas.size() != static_cast<std::size_t>(replica_regions - 1) ||
                !all_active(replicas, "ReplicaStatus")) {
                return false;
            }
            return !witness ||
                   (t.contains("GlobalTableWitnesses") &&
                    all_active(t.at("GlobalTableWitnesses").as_array(), "WitnessStatus"));
        }));
    }
}

auto put(cluster& c, int i, const std::string& id) -> answer {
    return c.call_until_served(i, "PutItem",
                               R"({"TableName":"accounts","Item":{"id":{"S":")" + id + R"("}}})");
}

auto strong_get(cluster& c, int i, const std::string& id) -> answer {
    return c.call_until_served(
        i, "GetItem",
        R"({"TableName":"accounts","ConsistentRead":true,"Key":{"id":{"S":")" + id + R"("}}})");
}

/// Waits up to 30 s for region `i`'s own replica to hold `id`, by
/// eventually consistent reads. A write is acknowledged once a majority
/// holds it, which need not include region `i`.
auto applied_locally(cluster& c, int i, const std::string& id) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto got =
            c.call(i, "GetItem", R"({"TableName":"accounts","Key":{"id":{"S":")" + id + R"("}}})");
        if (got.status == 200U && got.body.contains("Item")) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

/// Every id in `accounts`, by consistent Scan pages from region `i`.
auto ids_in(cluster& c, int i) -> std::set<std::string> {
    std::set<std::string> out;
    std::string start;
    do {
        const auto page = c.call_until_served(
            i, "Scan",
            R"({"TableName":"accounts","ConsistentRead":true)" +
                (start.empty() ? std::string{} : R"(,"ExclusiveStartKey":)" + start) + "}");
        BOOST_REQUIRE_MESSAGE(page.status == 200U, json::serialize(page.body));
        for (const auto& item : page.body.at("Items").as_array()) {
            out.insert(std::string(item.at("id").at("S").as_string()));
        }
        start = page.body.contains("LastEvaluatedKey")
                    ? json::serialize(page.body.at("LastEvaluatedKey"))
                    : std::string{};
    } while (!start.empty());
    return out;
}

}  // namespace

// Task 27.2: three one-node clusters in three regions run one strong
// host. Its catalog answers on every node once a leader that a quorum
// spanning two regions elected is known, which takes the strong forward
// link from the nodes that do not lead it.
BOOST_AUTO_TEST_CASE(three_regions_form_one_strong_host) {
    cluster c(true);
    for (int i = 0; i < k_nodes; ++i) {
        BOOST_TEST_INFO("node " << i);
        BOOST_TEST(c.wait_for_log(i, "strong host ready", std::chrono::seconds(60)));
    }
    for (int i = 0; i < k_nodes; ++i) {
        BOOST_TEST(c.wait_for_log(i, "in regions us-east-1, us-east-2, us-west-2",
                                  std::chrono::seconds(1)));
    }
}

// Task 27.4: a table made strong in one region is adopted by the other
// two, and one write is read consistently everywhere: three regions' gateways
// over one strong host, each region a one-node cluster.
BOOST_AUTO_TEST_CASE(a_table_made_strong_serves_all_three_regions) {
    cluster c(true);
    for (int i = 0; i < k_nodes; ++i) {
        BOOST_REQUIRE(c.wait_for_log(i, "strong host ready", std::chrono::seconds(60)));
    }
    const std::string create =
        R"({"TableName":"accounts","BillingMode":"PAY_PER_REQUEST",)"
        R"("AttributeDefinitions":[{"AttributeName":"id","AttributeType":"S"}],)"
        R"("KeySchema":[{"AttributeName":"id","KeyType":"HASH"}]})";
    const auto created = c.call_until_served(0, "CreateTable", create);
    BOOST_REQUIRE_MESSAGE(created.status == 200U, json::serialize(created.body));
    const auto active_in = [&](int i, auto&& ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto d = c.call_until_served(i, "DescribeTable", R"({"TableName":"accounts"})");
            if (d.status == 200U && ready(d.body.at("Table").as_object())) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    };
    BOOST_REQUIRE(active_in(
        0, [](const json::object& t) { return t.at("TableStatus").as_string() == "ACTIVE"; }));
    const auto made = c.call_until_served(
        0, "UpdateTable",
        R"({"TableName":"accounts","MultiRegionConsistency":"STRONG","ReplicaUpdates":[)"
        R"({"Create":{"RegionName":"us-east-2"}},{"Create":{"RegionName":"us-west-2"}}]})");
    BOOST_REQUIRE_MESSAGE(made.status == 200U, json::serialize(made.body));

    // Every region's catalog comes to hold it, strong, with both other
    // replicas ACTIVE.
    for (int i = 0; i < k_nodes; ++i) {
        BOOST_TEST_INFO("region " << cluster::k_regions.at(i));
        BOOST_TEST(active_in(i, [](const json::object& t) {
            if (!t.contains("MultiRegionConsistency") || !t.contains("Replicas")) {
                return false;
            }
            const auto& replicas = t.at("Replicas").as_array();
            return replicas.size() == 2U &&
                   std::all_of(replicas.begin(), replicas.end(), [](const json::value& r) {
                       return r.at("ReplicaStatus").as_string() == "ACTIVE";
                   });
        }));
    }

    // A write in each region is read consistently in every other.
    for (int i = 0; i < k_nodes; ++i) {
        const auto put = c.call_until_served(i, "PutItem",
                                             R"({"TableName":"accounts","Item":{"id":{"S":"a)" +
                                                 std::to_string(i) + R"("},"by":{"N":")" +
                                                 std::to_string(i) + R"("}}})");
        BOOST_REQUIRE_MESSAGE(put.status == 200U, json::serialize(put.body));
        for (int j = 0; j < k_nodes; ++j) {
            const auto got = c.call_until_served(
                j, "GetItem",
                R"({"TableName":"accounts","ConsistentRead":true,"Key":{"id":{"S":"a)" +
                    std::to_string(i) + R"("}}})");
            BOOST_TEST_INFO("written in " << i << ", read in " << j);
            BOOST_TEST(got.status == 200U);
            BOOST_TEST(got.body.contains("Item"));
        }
    }

    // Task 27.5: lose one region and the other two keep serving, writes
    // and strongly consistent reads included.
    c.stop(2);
    const auto put = c.call_until_served(
        0, "PutItem", R"({"TableName":"accounts","Item":{"id":{"S":"after-loss"}}})");
    BOOST_REQUIRE_MESSAGE(put.status == 200U, json::serialize(put.body));
    const auto got = c.call_until_served(
        1, "GetItem",
        R"({"TableName":"accounts","ConsistentRead":true,"Key":{"id":{"S":"after-loss"}}})");
    BOOST_TEST(got.status == 200U);
    BOOST_TEST(got.body.contains("Item"));

    // Lose a second and the last region has no quorum: writes and strongly
    // consistent reads fail, eventually consistent reads still answer from
    // the region's own replica.
    c.stop(1);
    const auto lonely =
        c.call(0, "PutItem", R"({"TableName":"accounts","Item":{"id":{"S":"no-quorum"}}})");
    BOOST_TEST(lonely.status == 500U);
    const auto strong_read =
        c.call(0, "GetItem",
               R"({"TableName":"accounts","ConsistentRead":true,"Key":{"id":{"S":"after-loss"}}})");
    BOOST_TEST(strong_read.status == 500U);
    const auto eventual =
        c.call(0, "GetItem", R"({"TableName":"accounts","Key":{"id":{"S":"after-loss"}}})");
    BOOST_TEST(eventual.status == 200U);
    BOOST_TEST(eventual.body.contains("Item"));
}

// Task 27.6: deleting the other two replicas makes a strong table a
// single-region one. Its groups get a range of their own and lose the
// other regions' voters, so it keeps serving with those regions gone.
BOOST_AUTO_TEST_CASE(a_strong_table_left_for_one_region_survives_the_others) {
    cluster c(true);
    for (int i = 0; i < k_nodes; ++i) {
        BOOST_REQUIRE(c.wait_for_log(i, "strong host ready", std::chrono::seconds(60)));
    }
    const auto described = [&](int i, auto&& ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto d = c.call_until_served(i, "DescribeTable", R"({"TableName":"ledger"})");
            if (ready(d)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    };
    const auto created = c.call_until_served(
        0, "CreateTable",
        R"({"TableName":"ledger","BillingMode":"PAY_PER_REQUEST",)"
        R"("AttributeDefinitions":[{"AttributeName":"id","AttributeType":"S"}],)"
        R"("KeySchema":[{"AttributeName":"id","KeyType":"HASH"}]})");
    BOOST_REQUIRE_MESSAGE(created.status == 200U, json::serialize(created.body));
    BOOST_REQUIRE(described(0, [](const answer& d) {
        return d.status == 200U && d.body.at("Table").at("TableStatus").as_string() == "ACTIVE";
    }));
    const auto made = c.call_until_served(
        0, "UpdateTable",
        R"({"TableName":"ledger","MultiRegionConsistency":"STRONG","ReplicaUpdates":[)"
        R"({"Create":{"RegionName":"us-east-2"}},{"Create":{"RegionName":"us-west-2"}}]})");
    BOOST_REQUIRE_MESSAGE(made.status == 200U, json::serialize(made.body));
    const auto all_active = [](const answer& d) {
        if (d.status != 200U || !d.body.at("Table").as_object().contains("Replicas")) {
            return false;
        }
        const auto& replicas = d.body.at("Table").at("Replicas").as_array();
        return replicas.size() == 2U &&
               std::all_of(replicas.begin(), replicas.end(), [](const json::value& r) {
                   return r.at("ReplicaStatus").as_string() == "ACTIVE";
               });
    };
    for (int i = 0; i < k_nodes; ++i) {
        BOOST_REQUIRE(described(i, all_active));
    }
    const auto written = c.call_until_served(
        1, "PutItem", R"({"TableName":"ledger","Item":{"id":{"S":"while-strong"}}})");
    BOOST_REQUIRE_MESSAGE(written.status == 200U, json::serialize(written.body));

    const auto left = c.call_until_served(
        0, "UpdateTable",
        R"({"TableName":"ledger","ReplicaUpdates":[{"Delete":{"RegionName":"us-east-2"}},)"
        R"({"Delete":{"RegionName":"us-west-2"}}]})");
    BOOST_REQUIRE_MESSAGE(left.status == 200U, json::serialize(left.body));
    BOOST_TEST(described(0, [](const answer& d) {
        return d.status == 200U && !d.body.at("Table").as_object().contains("Replicas") &&
               !d.body.at("Table").as_object().contains("MultiRegionConsistency");
    }));
    for (int i = 1; i < k_nodes; ++i) {
        BOOST_TEST_INFO("region " << cluster::k_regions.at(i));
        BOOST_TEST(
            described(i, [](const answer& d) { return d.type() == "ResourceNotFoundException"; }));
    }

    // With both other regions gone the table still takes writes and
    // strongly consistent reads: its groups' only voters are here.
    c.stop(1);
    c.stop(2);
    const auto put =
        c.call_until_served(0, "PutItem", R"({"TableName":"ledger","Item":{"id":{"S":"alone"}}})");
    BOOST_REQUIRE_MESSAGE(put.status == 200U, json::serialize(put.body));
    for (const char* id : {"while-strong", "alone"}) {
        const auto got = c.call_until_served(
            0, "GetItem",
            std::string(R"({"TableName":"ledger","ConsistentRead":true,"Key":{"id":{"S":")") + id +
                R"("}}})");
        BOOST_TEST_INFO(id);
        BOOST_TEST(got.status == 200U);
        BOOST_TEST(got.body.contains("Item"));
    }
}

// Task 27.8: the three regions race to create the same items with
// attribute_not_exists. One log orders every write, so for each key
// exactly one region wins and the others see the condition fail; every
// region then reads the same winner.
BOOST_AUTO_TEST_CASE(conditional_writes_racing_across_regions_have_one_winner) {
    cluster c(true);
    make_strong_table(c);
    constexpr int k_keys = 12;
    std::array<std::array<unsigned, k_keys>, k_nodes> status{};
    std::vector<std::thread> writers;
    for (int i = 0; i < k_nodes; ++i) {
        writers.emplace_back([&, i] {
            for (int k = 0; k < k_keys; ++k) {
                const auto r = c.call_until_served(
                    i, "PutItem",
                    R"({"TableName":"accounts","Item":{"id":{"S":"race-)" + std::to_string(k) +
                        R"("},"by":{"S":")" + std::string(cluster::k_regions.at(i)) +
                        R"x("}},"ConditionExpression":"attribute_not_exists(id)"})x");
                status.at(i).at(k) =
                    r.status == 400U && r.type() == "ConditionalCheckFailedException" ? 400U
                                                                                      : r.status;
            }
        });
    }
    for (auto& t : writers) {
        t.join();
    }
    for (int k = 0; k < k_keys; ++k) {
        BOOST_TEST_INFO("key race-" << k);
        int won = 0;
        int lost = 0;
        for (int i = 0; i < k_nodes; ++i) {
            won += status.at(i).at(k) == 200U ? 1 : 0;
            lost += status.at(i).at(k) == 400U ? 1 : 0;
        }
        BOOST_TEST(won == 1);
        BOOST_TEST(lost == k_nodes - 1);
        std::set<std::string> winners;
        for (int i = 0; i < k_nodes; ++i) {
            const auto got = strong_get(c, i, "race-" + std::to_string(k));
            BOOST_REQUIRE(got.body.contains("Item"));
            winners.insert(std::string(got.body.at("Item").at("by").at("S").as_string()));
        }
        BOOST_TEST(winners.size() == 1U);
    }
}

// Task 27.8: cut one region off without stopping it. The other two keep
// writing; the cut-off region still answers eventually consistent reads
// from its own replica, but cannot write. Brought back, it catches up and
// writes again.
BOOST_AUTO_TEST_CASE(a_region_cut_off_and_brought_back_catches_up) {
    cluster c(true, {}, true);
    make_strong_table(c);
    BOOST_REQUIRE_EQUAL(put(c, 2, "before-cut").status, 200U);
    // Cut off before its replica applied the write, region 2 would have
    // nothing for the eventually consistent read below to find.
    BOOST_REQUIRE(applied_locally(c, 2, "before-cut"));

    c.cut_off(2);
    for (int i = 0; i < 2; ++i) {
        const auto r = put(c, i, "during-cut-" + std::to_string(i));
        BOOST_REQUIRE_MESSAGE(r.status == 200U, json::serialize(r.body));
    }
    const auto lonely =
        c.call(2, "PutItem", R"({"TableName":"accounts","Item":{"id":{"S":"lonely"}}})");
    BOOST_TEST(lonely.status == 500U);
    const auto eventual =
        c.call(2, "GetItem", R"({"TableName":"accounts","Key":{"id":{"S":"before-cut"}}})");
    BOOST_TEST(eventual.status == 200U);
    BOOST_TEST(eventual.body.contains("Item"));

    c.cut_off(2, true);
    for (int i = 0; i < 2; ++i) {
        const auto got = strong_get(c, 2, "during-cut-" + std::to_string(i));
        BOOST_TEST_INFO("during-cut-" << i);
        BOOST_TEST(got.status == 200U);
        BOOST_TEST(got.body.contains("Item"));
    }
    const auto back = put(c, 2, "after-heal");
    BOOST_REQUIRE_MESSAGE(back.status == 200U, json::serialize(back.body));
    BOOST_TEST(strong_get(c, 0, "after-heal").body.contains("Item"));
}

// Task 27.8: two replicas and a witness. The witness region holds the
// groups' data but has no table to serve; with the other replica's region
// gone, the witness's voters still make the quorum, so the remaining
// replica keeps writing and reading strongly.
BOOST_AUTO_TEST_CASE(a_witness_keeps_the_quorum_when_a_replica_region_is_lost) {
    cluster c(true);
    make_strong_table(c, true);
    const auto at_witness = c.call(2, "DescribeTable", R"({"TableName":"accounts"})");
    BOOST_TEST(at_witness.status == 400U);
    BOOST_TEST(at_witness.type() == "ResourceNotFoundException");
    BOOST_REQUIRE_EQUAL(put(c, 1, "from-east2").status, 200U);

    c.stop(1);
    const auto r = put(c, 0, "after-replica-loss");
    BOOST_REQUIRE_MESSAGE(r.status == 200U, json::serialize(r.body));
    BOOST_TEST(strong_get(c, 0, "from-east2").body.contains("Item"));
    BOOST_TEST(strong_get(c, 0, "after-replica-loss").body.contains("Item"));
}

// Task 27.8: every region writes while the strong host's groups split by
// key count. Cuts are Raft entries, so every region's replica splits at
// the same index: no acknowledged write is lost, every region reads them
// all, and the table's stream shows the split's shards.
BOOST_AUTO_TEST_CASE(strong_groups_split_under_load_from_every_region) {
    cluster c(true, {{"KYTHIRA_DYNAMODB_SPLIT_KEYS", "40"},
                     {"KYTHIRA_DYNAMODB_SPLIT_INTERVAL_MS", "200"}});
    make_strong_table(c);
    const auto streamed =
        c.call_until_served(0, "UpdateTable",
                            R"({"TableName":"accounts","StreamSpecification":)"
                            R"({"StreamEnabled":true,"StreamViewType":"KEYS_ONLY"}})");
    BOOST_REQUIRE_MESSAGE(streamed.status == 200U, json::serialize(streamed.body));

    constexpr int k_per_region = 60;
    std::array<std::vector<std::string>, k_nodes> acked;
    std::vector<std::thread> writers;
    for (int i = 0; i < k_nodes; ++i) {
        writers.emplace_back([&, i] {
            for (int n = 0; n < k_per_region; ++n) {
                const auto id = std::string(cluster::k_regions.at(i)) + "-" + std::to_string(n);
                if (put(c, i, id).status == 200U) {
                    acked.at(i).push_back(id);
                }
            }
        });
    }
    for (auto& t : writers) {
        t.join();
    }
    std::set<std::string> expected;
    for (const auto& ids : acked) {
        BOOST_TEST(ids.size() == static_cast<std::size_t>(k_per_region));
        expected.insert(ids.begin(), ids.end());
    }
    for (int i = 0; i < k_nodes; ++i) {
        BOOST_TEST_INFO("region " << cluster::k_regions.at(i));
        BOOST_TEST((ids_in(c, i) == expected));
    }

    // The split's children are shards of the stream beside their parent.
    bool split = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!split && std::chrono::steady_clock::now() < deadline) {
        const auto d = c.call_until_served(0, "DescribeTable", R"({"TableName":"accounts"})");
        const auto arn = std::string(d.body.at("Table").at("LatestStreamArn").as_string());
        const auto s = c.call_until_served(0, "DescribeStream", R"({"StreamArn":")" + arn + R"("})",
                                           "DynamoDBStreams_20120810");
        split =
            s.status == 200U && s.body.at("StreamDescription").at("Shards").as_array().size() > 1U;
        if (!split) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    BOOST_TEST(split);
}
