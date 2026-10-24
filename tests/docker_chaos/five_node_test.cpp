// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Five chaos_nodes (docker/chaos-5-compose.yml) driven through the node-table
// ChaosCluster and the faults cairn-chaos-testing task 3 adds: pause, delay
// and loss. Election safety is checked from every node's /elections, which
// sees every term each node won, not only the ones a /status sample catches.

#define BOOST_TEST_MODULE five_node_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>

using namespace std::chrono_literals;
using namespace docker_chaos;

namespace {

// The leader among `candidates`, re-checked after a heartbeat so a node that
// won and immediately lost is not returned (see ChaosCluster::wait_for_leader).
// Takes an explicit set so a paused node, whose HTTP calls only time out, is
// never asked.
ChaosNode* leader_among(ChaosCluster& cluster, const std::set<int>& candidates,
                        std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (int id : candidates) {
            auto& n = cluster.node(id);
            if (n.is_leader()) {
                std::this_thread::sleep_for(100ms);
                if (n.is_leader()) {
                    return &n;
                }
            }
        }
        std::this_thread::sleep_for(200ms);
    }
    return nullptr;
}

// A put through whichever of `candidates` leads, retried across a leader
// change. A put is idempotent, so a retry after an ambiguous failure is safe.
bool commit(ChaosCluster& cluster, const std::set<int>& candidates, const std::string& key,
            const std::string& value) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        auto* leader = leader_among(cluster, candidates, 15s);
        if (leader == nullptr) {
            return false;
        }
        try {
            auto resp = leader->submit_command(key, value);
            if (resp.contains("success") && resp["success"].as_bool()) {
                return true;
            }
            BOOST_TEST_MESSAGE("put " + key + " attempt " + std::to_string(attempt) + ": " +
                               boost::json::serialize(resp));
        } catch (const std::exception& e) {
            BOOST_TEST_MESSAGE("put " + key + " attempt " + std::to_string(attempt) +
                               " threw: " + e.what());
        }
        std::this_thread::sleep_for(300ms);
    }
    return false;
}

std::set<int> all_ids() {
    return {1, 2, 3, 4, 5};
}

// A node id from /status: a number, or its decimal text in a build with
// KYTHIRA_NODE_ID_KIND=text (the image's default).
int node_id_of(const boost::json::value& v) {
    if (v.is_string()) {
        return std::stoi(std::string(v.as_string()));
    }
    return static_cast<int>(v.to_number<std::int64_t>());
}

// At most one leader per term, over every term any node has won since it
// started. Each node is asked; one that is down is skipped, which is why the
// tests call this while every node is up.
void assert_one_leader_per_term(ChaosCluster& cluster) {
    std::map<std::int64_t, int> winner;
    for (int id : all_ids()) {
        auto elections = cluster.node(id).elections();
        for (const auto& e : elections["elections"].as_array()) {
            auto term = e.as_object().at("term").to_number<std::int64_t>();
            auto [it, inserted] = winner.emplace(term, id);
            BOOST_TEST(inserted,
                       "term " << term << " won by nodes " << it->second << " and " << id);
        }
    }
    BOOST_TEST_MESSAGE(std::to_string(winner.size()) + " elections recorded across five nodes");
    BOOST_TEST(!winner.empty());
}

struct FiveNodeFixture {
    ChaosCluster cluster;

    FiveNodeFixture()
        : cluster(default_chaos5_compose_file(), chaos5_node_table(chaos5_cluster_name()), 45s) {
        cluster.start();
        BOOST_REQUIRE(leader_among(cluster, all_ids(), 20s) != nullptr);
    }

    ~FiveNodeFixture() {
        for (int id : all_ids()) {
            try {
                cluster.node(id).unpause();
            } catch (...) {
            }
            cluster.node(id).clear_tc_netem();
        }
        cluster.stop();
    }

    FiveNodeFixture(const FiveNodeFixture&) = delete;
    FiveNodeFixture& operator=(const FiveNodeFixture&) = delete;
};

int some_follower(int leader_id) {
    return leader_id == 1 ? 2 : 1;
}

}  // namespace

BOOST_FIXTURE_TEST_SUITE(docker_chaos_five_node, FiveNodeFixture)

BOOST_AUTO_TEST_CASE(five_voters_commit_and_report_membership, *boost::unit_test::timeout(180)) {
    BOOST_REQUIRE(commit(cluster, all_ids(), "five", "1"));
    auto* leader = leader_among(cluster, all_ids(), 15s);
    BOOST_REQUIRE(leader != nullptr);
    auto status = leader->status();
    BOOST_TEST(status["config"].as_object().at("voters").as_array().size() == 5U);
    BOOST_TEST(node_id_of(status["leader_id"]) == leader->id());

    // Every node serves the committed value from its applied state once it
    // has caught up; the linearizable read goes through the leader.
    auto read = leader->kv_read("five", "linearizable");
    BOOST_TEST(read["value"].as_string() == "1");
    assert_one_leader_per_term(cluster);
}

// A paused leader keeps its sockets and its belief that it leads. The other
// four elect a new leader and keep committing; on thaw the old leader learns
// the newer term and follows.
BOOST_AUTO_TEST_CASE(paused_leader_is_replaced_and_rejoins, *boost::unit_test::timeout(240)) {
    auto* old_leader = leader_among(cluster, all_ids(), 15s);
    BOOST_REQUIRE(old_leader != nullptr);
    const int old_id = old_leader->id();
    const auto old_term = old_leader->status()["term"].to_number<std::int64_t>();
    try {
        old_leader->pause();
    } catch (const fault_unsupported& e) {
        // Task 3.4: reported as unsupported, never as a pause that worked.
        BOOST_TEST_MESSAGE(std::string("pause unsupported on this host, case skipped: ") +
                           e.what());
        return;
    }

    auto rest = all_ids();
    rest.erase(old_id);
    auto* new_leader = leader_among(cluster, rest, 20s);
    BOOST_REQUIRE(new_leader != nullptr);
    BOOST_TEST(new_leader->status()["term"].to_number<std::int64_t>() > old_term);
    BOOST_REQUIRE(commit(cluster, rest, "during_pause", "1"));

    old_leader->unpause();
    bool stepped_down = false;
    auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto s = old_leader->status();
        if (s["role"].as_string() != "leader" || s["term"].to_number<std::int64_t>() > old_term) {
            stepped_down = true;
            break;
        }
        std::this_thread::sleep_for(200ms);
    }
    BOOST_TEST(stepped_down);
    BOOST_REQUIRE(commit(cluster, all_ids(), "after_pause", "1"));
    assert_one_leader_per_term(cluster);
}

BOOST_AUTO_TEST_CASE(delay_and_loss_on_a_follower_still_commit, *boost::unit_test::timeout(240)) {
    auto* leader = leader_among(cluster, all_ids(), 15s);
    BOOST_REQUIRE(leader != nullptr);
    auto& follower = cluster.node(some_follower(leader->id()));

    follower.apply_delay(80, 20);
    BOOST_REQUIRE(commit(cluster, all_ids(), "delayed", "1"));
    follower.clear_tc_netem();

    follower.apply_loss(30);
    BOOST_REQUIRE(commit(cluster, all_ids(), "lossy", "1"));
    follower.clear_tc_netem();

    BOOST_REQUIRE(commit(cluster, all_ids(), "clean", "1"));
    assert_one_leader_per_term(cluster);
}

// Five voters tolerate two failures: kill two followers and the remaining
// three still commit.
BOOST_AUTO_TEST_CASE(two_followers_down_three_still_commit, *boost::unit_test::timeout(240)) {
    auto* leader = leader_among(cluster, all_ids(), 15s);
    BOOST_REQUIRE(leader != nullptr);
    std::set<int> down;
    for (int id : all_ids()) {
        if (id != leader->id() && down.size() < 2) {
            down.insert(id);
        }
    }
    for (int id : down) {
        cluster.node(id).kill();
    }
    auto up = all_ids();
    for (int id : down) {
        up.erase(id);
    }
    BOOST_REQUIRE(commit(cluster, up, "two_down", "1"));

    for (int id : down) {
        cluster.node(id).restart(true, 30s);
    }
    BOOST_REQUIRE(commit(cluster, all_ids(), "all_back", "1"));
    assert_one_leader_per_term(cluster);
}

BOOST_AUTO_TEST_SUITE_END()
