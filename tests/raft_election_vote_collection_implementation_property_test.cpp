// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE raft_election_vote_collection_implementation_property_test
#include "raft_membership_test_cluster.hpp"

#include <boost/test/unit_test.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace kythira::test::membership;

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {
            const_cast<char*>("raft_election_vote_collection_implementation_property_test"),
            nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

// ── Persistence that leaves a trace in the node's log ──────────────────────

/// The message `recording_persistence` logs for every write of the term or
/// vote. It shares the node's log sink, so its position among the node's own
/// messages shows what was durable when each RPC went out.
constexpr std::string_view persisted = "test: term and vote persisted";

/// The in-memory engine, recording each term and vote write in the node's
/// log sink before passing it on.
class recording_persistence : public kythira::memory_persistence_engine<> {
public:
    recording_persistence() = default;
    explicit recording_persistence(std::shared_ptr<log_sink> sink) : _sink(std::move(sink)) {}

    auto save_hard_state(std::uint64_t term, std::optional<std::uint64_t> vote) -> void {
        record(term, vote);
        memory_persistence_engine::save_hard_state(term, vote);
    }
    auto save_current_term(std::uint64_t term) -> void {
        record(term, load_voted_for());
        memory_persistence_engine::save_current_term(term);
    }
    auto save_voted_for(std::uint64_t vote) -> void {
        record(load_current_term(), vote);
        memory_persistence_engine::save_voted_for(vote);
    }

private:
    auto record(std::uint64_t term, std::optional<std::uint64_t> vote) -> void {
        if (!_sink) {
            return;
        }
        log_sink::entry e{.level = kythira::log_level::debug,
                          .message = std::string(persisted),
                          .fields = {{"term", std::to_string(term)},
                                     {"vote", vote ? std::to_string(*vote) : "none"}}};
        const std::lock_guard<std::mutex> lock(_sink->mutex);
        _sink->entries.push_back(std::move(e));
    }

    std::shared_ptr<log_sink> _sink;
};

struct election_types : test_types {
    using persistence_engine_type = recording_persistence;
};

using election_node = kythira::node<election_types>;

// ── Cluster harness ────────────────────────────────────────────────────────

/// A cluster of the given voters in which nobody campaigns until a case calls
/// `campaign()` or `invite()`: unlike the membership harness, there is no
/// leader to start with, because electing one is what these cases test.
class election_cluster {
public:
    explicit election_cluster(std::initializer_list<std::uint64_t> ids,
                              kythira::raft_configuration cfg = make_fast_config())
        : _cfg(cfg), _ids(ids) {
        _sim.start();
        for (auto id : _ids) {
            create(id);
        }
        for (auto from : _ids) {
            for (auto to : _ids) {
                if (from != to) {
                    link(from, to);
                }
            }
        }
        // The raw client exists from the start so that `isolate()` cuts it
        // off too: the simulator routes over several hops, and a later raw
        // client linked to an isolated node would carry the cluster's traffic
        // to it.
        auto raw = _sim.create_node("9");
        for (auto id : _ids) {
            link(9, id);
            link(id, 9);
        }
        _raw.emplace(raw, election_types::serializer_type{});
        for (auto id : _ids) {
            node(id).set_cluster_configuration(_ids);
            node(id).start();
        }
    }

    election_cluster(const election_cluster&) = delete;
    auto operator=(const election_cluster&) -> election_cluster& = delete;

    ~election_cluster() {
        for (auto& [id, n] : std::views::reverse(_nodes)) {
            n->stop();
        }
        _sim.stop();
    }

    auto node(std::uint64_t id) -> election_node& { return *_nodes.at(id); }
    auto logs(std::uint64_t id) -> log_sink& { return *_logs.at(id); }
    auto metrics(std::uint64_t id) -> metric_sink& { return *_metrics.at(id); }

    /// Stops every message to `to`, the raw client's included. Every inbound
    /// edge goes, for the multi-hop routing `cluster::isolate()` explains.
    auto isolate(std::uint64_t to) -> void {
        for (const auto& [id, n] : _nodes) {
            if (id != to) {
                _sim.remove_edge(std::to_string(id), std::to_string(to));
            }
        }
        _sim.remove_edge("9", std::to_string(to));
    }
    auto reconnect(std::uint64_t to) -> void {
        for (const auto& [id, n] : _nodes) {
            if (id != to) {
                link(id, to);
            }
        }
        link(9, to);
    }

    /// Lets `id`'s election timeout elapse, then ticks it once: a pre-vote
    /// round and, if that passes, an election.
    auto campaign(std::uint64_t id) -> void {
        std::this_thread::sleep_for(_cfg._election_timeout_max + 20ms);
        node(id).check_election_timeout();
    }

    /// Sends `id` a TimeoutNow and ticks it, so it campaigns at once and
    /// without a pre-vote round, whoever it can reach.
    auto invite(std::uint64_t id) -> void {
        kythira::timeout_now_request<std::uint64_t> request{
            ._term = node(id).get_current_term(), ._leader_id = 9, ._last_log_index = 0};
        auto response = raw_client().send_timeout_now(id, request, 1000ms).get();
        BOOST_REQUIRE(response.success());
        node(id).check_election_timeout();
    }

    /// Has `voter` grant its vote to `candidate` in `term`, as if `candidate`
    /// had campaigned, so it refuses anyone else in that term.
    auto grant(std::uint64_t voter, std::uint64_t candidate, std::uint64_t term) -> void {
        kythira::request_vote_request<> request{
            ._term = term, ._candidate_id = candidate, ._last_log_index = 0, ._last_log_term = 0};
        auto response = raw_client().send_request_vote(voter, request, 1000ms).get();
        BOOST_REQUIRE(response.vote_granted());
        BOOST_REQUIRE_EQUAL(node(voter).get_current_term(), term);
    }

    /// The peers `id` sent RequestVote to, by the `vote_request_sent` metric.
    auto vote_requests_from(std::uint64_t id) -> std::set<std::string> {
        auto peers = metrics(id).dimension_values("vote_request_sent", "peer_id");
        return {peers.begin(), peers.end()};
    }

    /// The `vote_response_received` metrics `id` emitted, peer to
    /// `vote_granted`.
    auto vote_responses_to(std::uint64_t id) -> std::map<std::string, std::string> {
        const auto& sink = metrics(id);
        const std::lock_guard<std::mutex> lock(sink.mutex);
        std::map<std::string, std::string> out;
        for (const auto& e : sink.entries) {
            if (e.name == "vote_response_received") {
                out.insert_or_assign(e.dimensions.at("peer_id"), e.dimensions.at("vote_granted"));
            }
        }
        return out;
    }

private:
    auto link(std::uint64_t from, std::uint64_t to) -> void {
        _sim.add_edge(std::to_string(from), std::to_string(to), network_simulator::NetworkEdge{});
    }

    auto raw_client() -> election_types::network_client_type& { return *_raw; }

    auto create(std::uint64_t id) -> void {
        auto log = std::make_shared<log_sink>();
        auto met = std::make_shared<metric_sink>();
        auto net = _sim.create_node(std::to_string(id));
        _nodes[id] = std::make_unique<election_node>(kythira::node_config<election_types>{
            .node_id = id,
            .network_client = {net, election_types::serializer_type{}},
            .network_server = {net, election_types::serializer_type{}},
            .persistence = recording_persistence{log},
            .logger = recording_logger{log},
            .metrics = recording_metrics{met},
            .membership = {},
            .config = _cfg,
            .self_address = std::to_string(id),
            .peer_discovery = preset_peer_discovery<std::uint64_t, std::string>{},
        });
        _logs[id] = log;
        _metrics[id] = met;
    }

    kythira::raft_configuration _cfg;
    std::vector<std::uint64_t> _ids;
    sim_t _sim;
    std::map<std::uint64_t, std::unique_ptr<election_node>> _nodes;
    std::map<std::uint64_t, std::shared_ptr<log_sink>> _logs;
    std::map<std::uint64_t, std::shared_ptr<metric_sink>> _metrics;
    std::optional<election_types::network_client_type> _raw;
};

/// Position of the first entry logged as `message` whose fields include
/// every pair in `fields`, or nullopt.
auto first_index(log_sink& sink, std::string_view message,
                 const std::map<std::string, std::string>& fields) -> std::optional<std::size_t> {
    const std::lock_guard<std::mutex> lock(sink.mutex);
    for (std::size_t i = 0; i < sink.entries.size(); ++i) {
        const auto& e = sink.entries[i];
        if (e.message != message) {
            continue;
        }
        const bool all = std::all_of(fields.begin(), fields.end(), [&](const auto& kv) {
            auto it = e.fields.find(kv.first);
            return it != e.fields.end() && it->second == kv.second;
        });
        if (all) {
            return i;
        }
    }
    return std::nullopt;
}

auto is_candidate(election_node& n) -> bool {
    return n.get_state() == kythira::server_state::candidate;
}

}  // namespace

/**
 * Property 81: Election Vote Collection Implementation
 *
 * start_election() sends RequestVote to every peer, collects the responses
 * within the election timeout, and decides the outcome from a majority that
 * counts the candidate's own vote. The cases drive real nodes over the
 * network simulator; nobody campaigns until a case makes one node do so.
 *
 * Validates: Requirements 16.2, 20.1, 20.2, 27.2
 */

BOOST_AUTO_TEST_CASE(property_election_sends_request_vote_to_all_peers,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 81.1: Election sends RequestVote to all peers");

    election_cluster c{1, 2, 3, 4, 5};
    c.campaign(1);
    BOOST_REQUIRE(wait_until([&] { return c.node(1).is_leader(); }));

    // One RequestVote per peer, none to itself, all for the election's term.
    BOOST_TEST((c.vote_requests_from(1) == std::set<std::string>{"2", "3", "4", "5"}));
    const auto term = std::to_string(c.node(1).get_current_term());
    for (std::uint64_t peer : {2u, 3u, 4u, 5u}) {
        BOOST_TEST(first_index(c.logs(peer), "Received RequestVote RPC",
                               {{"from_candidate", "1"}, {"request_term", term}})
                       .has_value(),
                   "node " << peer << " never received the RequestVote");
    }
    BOOST_TEST(c.metrics(1).count("raft_vote_request_received") == 0u);
}

BOOST_AUTO_TEST_CASE(property_election_collects_vote_responses, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 81.2: Election collects vote responses for majority");

    // Five voters, three of them unreachable: one grant and the candidate's
    // own vote are two of five. The invitation skips the pre-vote round,
    // which would otherwise stop the campaign before it starts.
    election_cluster c{1, 2, 3, 4, 5};
    for (std::uint64_t id : {3u, 4u, 5u}) {
        c.isolate(id);
    }
    c.invite(1);
    BOOST_REQUIRE(
        wait_until([&] { return c.logs(1).count("Election failed, insufficient votes"); }));
    BOOST_TEST(is_candidate(c.node(1)));
    BOOST_TEST((c.vote_responses_to(1) == std::map<std::string, std::string>{{"2", "true"}}));

    // A third voter's grant makes three of five, and the next campaign wins.
    // Node 3's grant is read from node 3: the first round's RequestVote to
    // it is still being retried, and once it lands its refusal (a stale
    // term) is counted in node 1's response metrics too.
    c.reconnect(3);
    c.invite(1);
    BOOST_REQUIRE(wait_until([&] { return c.node(1).is_leader(); }));
    const auto term = std::to_string(c.node(1).get_current_term());
    BOOST_TEST(
        first_index(c.logs(3), "Granting vote", {{"candidate", "1"}, {"term", term}}).has_value());
    BOOST_TEST(c.logs(1).count("Election won, transitioning to leader") == 1u);
}

BOOST_AUTO_TEST_CASE(property_election_includes_self_vote, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 81.3: Election includes self-vote in count");

    // Node 3 never hears the campaign, so node 2's is the only grant: one of
    // three, a majority only with the candidate's own vote.
    election_cluster c{1, 2, 3};
    c.isolate(3);
    c.campaign(1);
    BOOST_REQUIRE(wait_until([&] { return c.node(1).is_leader(); }));

    BOOST_TEST((c.vote_responses_to(1) == std::map<std::string, std::string>{{"2", "true"}}));
    const auto term = std::to_string(c.node(1).get_current_term());
    BOOST_TEST(first_index(c.logs(1), persisted, {{"term", term}, {"vote", "1"}}).has_value(),
               "the candidate never recorded its vote for itself");
    // It asked node 3 too: the self-vote is counted, not a substitute for
    // asking.
    BOOST_TEST(c.vote_requests_from(1).count("3") == 1u);
}

BOOST_AUTO_TEST_CASE(property_election_transitions_to_leader_on_majority,
                     *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 81.4: Election transitions to leader on majority");

    election_cluster c{1, 2, 3};
    const auto before = c.node(1).get_current_term();
    c.campaign(1);
    BOOST_REQUIRE(wait_until([&] { return c.node(1).is_leader(); }));

    const auto term = c.node(1).get_current_term();
    BOOST_TEST(c.node(1).get_state() == kythira::server_state::leader);
    BOOST_TEST(term == before + 1);
    BOOST_TEST(first_index(c.logs(1), "Election won, transitioning to leader",
                           {{"term", std::to_string(term)}})
                   .has_value());
    // The voters followed it into its term and stayed followers.
    for (std::uint64_t peer : {2u, 3u}) {
        BOOST_TEST(c.node(peer).get_current_term() == term);
        BOOST_TEST(c.node(peer).get_state() == kythira::server_state::follower);
    }
}

BOOST_AUTO_TEST_CASE(property_election_handles_split_vote, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 81.5: Election handles split vote scenario");

    // Nodes 2 and 3 have each voted for the other in term 1, so node 1's
    // term-1 campaign gets no grant. A pre-vote changes no state and ignores
    // votes already cast, so node 1 still reaches the real election.
    election_cluster c{1, 2, 3};
    c.grant(2, 3, 1);
    c.grant(3, 2, 1);
    c.campaign(1);
    BOOST_REQUIRE(
        wait_until([&] { return c.logs(1).count("Election failed, insufficient votes"); }));

    BOOST_TEST(is_candidate(c.node(1)));
    BOOST_TEST(!c.node(1).is_leader());
    BOOST_TEST(c.node(1).get_current_term() == 1u);
    BOOST_TEST((c.vote_responses_to(1) ==
                std::map<std::string, std::string>{{"2", "false"}, {"3", "false"}}));
    BOOST_TEST(c.metrics(2).dimension_values("raft_vote_denied", "reason") ==
                   std::vector<std::string>{"already_voted"},
               boost::test_tools::per_element());

    // The next election timeout starts term 2, in which nobody has voted.
    c.campaign(1);
    BOOST_REQUIRE(wait_until([&] { return c.node(1).is_leader(); }));
    BOOST_TEST(c.node(1).get_current_term() == 2u);
}

BOOST_AUTO_TEST_CASE(property_election_detects_higher_term, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 81.6: Election detects higher term in responses");

    // Node 3 has moved on to term 10. Node 2's grant gives node 1 a majority
    // for term 1, but node 3's refusal names a newer term, and a candidate
    // that learns of one stops, majority or not.
    election_cluster c{1, 2, 3};
    c.grant(3, 2, 10);
    c.campaign(1);
    BOOST_REQUIRE(wait_until([&] { return c.node(1).get_current_term() == 10u; }));

    BOOST_TEST(!c.node(1).is_leader());
    BOOST_TEST(c.node(1).get_state() == kythira::server_state::follower);
    BOOST_TEST((c.vote_responses_to(1) ==
                std::map<std::string, std::string>{{"2", "true"}, {"3", "false"}}));
    BOOST_TEST(c.logs(1).count("Discovered higher term during election, stepping down") == 1u);
    BOOST_TEST(c.logs(1).count("Election won, transitioning to leader") == 0u);
    // The new term is stored with no vote: node 1 has not voted in term 10.
    BOOST_TEST(first_index(c.logs(1), persisted, {{"term", "10"}, {"vote", "none"}}).has_value());
}

BOOST_AUTO_TEST_CASE(property_election_handles_single_node_cluster,
                     *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Property 81.7: Election handles single-node cluster");

    election_cluster c{1};
    c.campaign(1);
    BOOST_REQUIRE(wait_until([&] { return c.node(1).is_leader(); }));

    BOOST_TEST(c.node(1).get_current_term() == 1u);
    BOOST_TEST(c.logs(1).count("Single-node cluster, becoming leader immediately") == 1u);
    BOOST_TEST(c.metrics(1).count("election_started") == 1u);
    BOOST_TEST(c.metrics(1).count("vote_request_sent") == 0u);
    BOOST_TEST(c.logs(1).count("Starting pre-vote round") == 0u);
    BOOST_TEST(first_index(c.logs(1), persisted, {{"term", "1"}, {"vote", "1"}}).has_value());
}

BOOST_AUTO_TEST_CASE(property_election_timeout_handling, *boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Property 81.8: Election handles timeout correctly");

    // Neither peer hears the campaign, so no response ever arrives. The
    // collection gives up after the election timeout rather than waiting on
    // the RPCs' own retries, and the candidate stays a candidate.
    election_cluster c{1, 2, 3};
    c.isolate(2);
    c.isolate(3);
    c.invite(1);
    BOOST_REQUIRE(
        wait_until([&] { return c.logs(1).count("Election failed, insufficient votes"); }, 2000ms));

    BOOST_TEST(is_candidate(c.node(1)));
    BOOST_TEST(c.node(1).get_current_term() == 1u);
    BOOST_TEST(c.vote_responses_to(1).empty());
    BOOST_TEST(c.metrics(1).dimension_values("election_lost", "reason") ==
                   std::vector<std::string>{"insufficient_votes"},
               boost::test_tools::per_element());

    // Once the peers are back, the next timeout's campaign wins.
    c.reconnect(2);
    c.reconnect(3);
    BOOST_REQUIRE(wait_until(
        [&] {
            c.campaign(1);
            return wait_until([&] { return c.node(1).is_leader(); }, 1000ms);
        },
        8000ms));
    BOOST_TEST(c.node(1).get_current_term() >= 2u);
}

BOOST_AUTO_TEST_CASE(property_election_emits_metrics, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Property 81.9: Election emits metrics for monitoring");

    election_cluster c{1, 2, 3};
    c.campaign(1);
    BOOST_REQUIRE(wait_until([&] { return c.node(1).is_leader(); }));
    const auto term = std::to_string(c.node(1).get_current_term());

    BOOST_TEST(
        c.metrics(1).dimension_values("election_started", "term") == std::vector<std::string>{term},
        boost::test_tools::per_element());
    BOOST_TEST((c.vote_requests_from(1) == std::set<std::string>{"2", "3"}));
    BOOST_TEST((c.vote_responses_to(1) ==
                std::map<std::string, std::string>{{"2", "true"}, {"3", "true"}}));
    BOOST_TEST(
        c.metrics(1).dimension_values("election_won", "term") == std::vector<std::string>{term},
        boost::test_tools::per_element());
    BOOST_TEST(c.metrics(1).count("election_lost") == 0u);
    for (std::uint64_t peer : {2u, 3u}) {
        BOOST_TEST(c.metrics(peer).dimension_values("raft_vote_granted", "candidate") ==
                       std::vector<std::string>{"1"},
                   boost::test_tools::per_element());
    }
}

BOOST_AUTO_TEST_CASE(property_election_persists_state_before_voting,
                     *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Property 81.10: Election persists state before voting");

    election_cluster c{1, 2, 3};
    c.campaign(1);
    BOOST_REQUIRE(wait_until([&] { return c.node(1).is_leader(); }));
    const auto term = std::to_string(c.node(1).get_current_term());

    // The candidate stored its new term and its vote for itself before it
    // sent its first RequestVote.
    const auto stored = first_index(c.logs(1), persisted, {{"term", term}, {"vote", "1"}});
    const auto sent = first_index(c.logs(1), "Sending RequestVote RPC", {{"term", term}});
    BOOST_REQUIRE(stored.has_value());
    BOOST_REQUIRE(sent.has_value());
    BOOST_TEST(*stored < *sent);

    // And each voter stored its vote before granting it.
    for (std::uint64_t peer : {2u, 3u}) {
        const auto voted = first_index(c.logs(peer), persisted, {{"term", term}, {"vote", "1"}});
        const auto granted = first_index(c.logs(peer), "Granting vote", {{"term", term}});
        BOOST_REQUIRE(voted.has_value());
        BOOST_REQUIRE(granted.has_value());
        BOOST_TEST(*voted < *granted);
    }
}
