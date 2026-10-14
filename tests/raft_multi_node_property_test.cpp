// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Multi-node Raft properties under randomized failures.
 *
 * Each property runs several seeded iterations on a real cluster (3 or 5
 * nodes) while crashing, restarting and partitioning random minorities:
 *
 * - the cluster eventually elects a single leader, and no term ever has two;
 * - an acknowledged (committed) command is never lost;
 * - every state machine converges to the same state;
 * - the cluster keeps committing with any minority of nodes down.
 *
 * The seed of a failing iteration is in the test context, so a failure can be
 * replayed by pinning it.
 *
 * Requirements: raft-consensus 2.1, 2.2, 2.3, 2.4, 5.1
 * Task: raft-consensus 731
 */

#define BOOST_TEST_MODULE raft_multi_node_property_test
#include <boost/test/unit_test.hpp>

#include "raft_multi_node_test_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <random>
#include <string>
#include <thread>

using kythira::test::cluster_config;
using kythira::test::raft_multi_node_fixture;
using kythira::test::wait_until;
using kythira::testing::scaled_deadline;
using kythira::testing::scaled_timeout;
using namespace std::chrono_literals;

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
// BOOST_GLOBAL_FIXTURE pastes its argument into an identifier, so the
// fixture must be named unqualified.
using kythira::test::folly_init_fixture;
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using node_id = raft_multi_node_fixture::node_id_type;

constexpr int iterations = 3;
constexpr std::uint32_t base_seed = 0x5eed731;

auto make_running_cluster(std::size_t nodes) -> std::unique_ptr<raft_multi_node_fixture> {
    cluster_config cfg;
    cfg.node_count = nodes;
    auto f = std::make_unique<raft_multi_node_fixture>(cfg);
    f->initialize_cluster();
    f->start_all_nodes();
    return f;
}

auto random_minority(const raft_multi_node_fixture& f, std::mt19937& rng) -> std::vector<node_id> {
    auto ids = f.get_node_ids();
    std::shuffle(ids.begin(), ids.end(), rng);
    std::uniform_int_distribution<std::size_t> size(1, (ids.size() - 1) / 2);
    ids.resize(size(rng));
    return ids;
}

auto as_string(const std::vector<std::byte>& bytes) -> std::string {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

auto check_election_safety(const raft_multi_node_fixture& f) -> void {
    for (const auto& v : f.election_safety_violations()) {
        BOOST_ERROR("two leaders in one " << v);
    }
}

/// One disruption: crash a random minority, or cut it off. Returns the nodes
/// disrupted so the caller can undo it.
auto disrupt(raft_multi_node_fixture& f, std::mt19937& rng)
    -> std::pair<bool, std::vector<node_id>> {
    auto victims = random_minority(f, rng);
    bool crash = std::uniform_int_distribution<int>(0, 1)(rng) == 0;
    if (crash) {
        for (auto id : victims) {
            f.stop_node(id);
        }
    } else {
        f.partition({victims});
    }
    return {crash, victims};
}

auto restore(raft_multi_node_fixture& f, const std::pair<bool, std::vector<node_id>>& d) -> void {
    if (d.first) {
        for (auto id : d.second) {
            f.start_node(id);
        }
    } else {
        f.heal();
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_node_properties)

// Property: the cluster eventually elects a single leader, after start and
// after every disruption, and no term ever has two.
BOOST_AUTO_TEST_CASE(eventually_elects_single_leader,
                     *boost::unit_test::timeout(scaled_timeout(120))) {
    for (int i = 0; i < iterations; ++i) {
        auto seed = base_seed + i;
        BOOST_TEST_CONTEXT("seed " << seed) {
            std::mt19937 rng(seed);
            auto f = make_running_cluster(i % 2 == 0 ? 3 : 5);
            for (int round = 0; round < 3; ++round) {
                BOOST_REQUIRE(f->wait_for_leader(scaled_deadline(5000)).has_value());
                auto d = disrupt(*f, rng);
                std::this_thread::sleep_for(2 * f->config().election_timeout_max);
                restore(*f, d);
            }
            auto leader = f->wait_for_leader(scaled_deadline(5000));
            BOOST_REQUIRE(leader.has_value());
            BOOST_REQUIRE(f->submit(raft_multi_node_fixture::put_command("settle", "1"), 3s).ok);
            BOOST_REQUIRE(f->wait_for_convergence(scaled_deadline(5000)));

            // Once settled, exactly one node leads and everyone follows it.
            f->reset_simultaneous_leaders();
            std::this_thread::sleep_for(2 * f->config().election_timeout_max);
            BOOST_CHECK_EQUAL(f->max_simultaneous_leaders(), 1u);
            leader = f->get_leader();
            BOOST_REQUIRE(leader.has_value());
            for (auto id : f->get_node_ids()) {
                if (id != *leader) {
                    BOOST_CHECK(f->node(id).known_leader() == leader);
                }
            }
            check_election_safety(*f);
        }
    }
}

// Properties: acknowledged commands are never lost, and every state machine
// ends in the same state.
BOOST_AUTO_TEST_CASE(committed_entries_survive_and_state_converges,
                     *boost::unit_test::timeout(scaled_timeout(180))) {
    for (int i = 0; i < iterations; ++i) {
        auto seed = base_seed + 100 + i;
        BOOST_TEST_CONTEXT("seed " << seed) {
            std::mt19937 rng(seed);
            auto f = make_running_cluster(i % 2 == 0 ? 5 : 3);
            BOOST_REQUIRE(f->wait_for_leader(scaled_deadline(5000)).has_value());

            std::map<std::string, std::string> acknowledged;
            std::vector<std::vector<std::byte>> acknowledged_commands;
            for (int round = 0; round < 4; ++round) {
                auto d = disrupt(*f, rng);
                for (int k = 0; k < 4; ++k) {
                    auto key =
                        "key" + std::to_string(std::uniform_int_distribution<int>(0, 5)(rng));
                    auto value = std::to_string(round) + "." + std::to_string(k);
                    auto cmd = raft_multi_node_fixture::put_command(key, value);
                    // The majority side can always commit, so every command
                    // must be acknowledged eventually.
                    auto r = f->submit(cmd, scaled_deadline(5000));
                    BOOST_REQUIRE_MESSAGE(r.ok, "round " << round << ": " << r.error);
                    acknowledged[key] = value;
                    acknowledged_commands.push_back(cmd);
                }
                restore(*f, d);
            }

            auto leader = f->wait_for_leader(scaled_deadline(5000));
            BOOST_REQUIRE(leader.has_value());
            BOOST_REQUIRE(f->submit(raft_multi_node_fixture::put_command("final", "1"), 3s).ok);
            leader = f->get_leader();
            BOOST_REQUIRE(leader.has_value());
            BOOST_REQUIRE(
                f->wait_for_convergence(scaled_deadline(5000), f->commit_index_of(*leader)));

            for (auto id : f->get_node_ids()) {
                auto log = f->committed_entries(id);
                for (const auto& cmd : acknowledged_commands) {
                    auto found = std::any_of(log.begin(), log.end(),
                                             [&](const auto& e) { return e.command() == cmd; });
                    BOOST_CHECK_MESSAGE(found, "node " << id << " lost an acknowledged command");
                }
            }
            auto mismatch = f->committed_logs_mismatch();
            BOOST_CHECK_MESSAGE(!mismatch.has_value(), mismatch.value_or(""));

            // Reads go through the log, so each reflects the replicated state.
            for (const auto& [key, value] : acknowledged) {
                auto r = f->submit(raft_multi_node_fixture::get_command(key), 3s);
                BOOST_REQUIRE(r.ok);
                BOOST_CHECK_EQUAL(as_string(r.value), value);
            }
            check_election_safety(*f);
        }
    }
}

// Property: with any minority crashed, the rest keep electing and committing.
BOOST_AUTO_TEST_CASE(survives_minority_failures, *boost::unit_test::timeout(scaled_timeout(120))) {
    for (int i = 0; i < iterations; ++i) {
        auto seed = base_seed + 200 + i;
        BOOST_TEST_CONTEXT("seed " << seed) {
            std::mt19937 rng(seed);
            auto f = make_running_cluster(5);
            BOOST_REQUIRE(f->wait_for_leader(scaled_deadline(5000)).has_value());

            // Always include the current leader half the time, so the
            // survivors sometimes have to elect a new one first.
            auto victims = random_minority(*f, rng);
            if (i % 2 == 0) {
                auto leader = *f->get_leader();
                if (std::find(victims.begin(), victims.end(), leader) == victims.end()) {
                    victims.back() = leader;
                }
            }
            for (auto id : victims) {
                f->stop_node(id);
            }

            auto r = f->submit(raft_multi_node_fixture::put_command("survivor", "1"),
                               scaled_deadline(5000));
            BOOST_REQUIRE_MESSAGE(r.ok, r.error);
            auto leader = f->get_leader();
            BOOST_REQUIRE(leader.has_value());
            BOOST_CHECK(std::find(victims.begin(), victims.end(), *leader) == victims.end());
            BOOST_CHECK(
                f->wait_for_convergence(scaled_deadline(5000), f->commit_index_of(*leader)));
            check_election_safety(*f);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
