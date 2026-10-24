// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Follower liveness under one slow persistence run
// (`.kiro/specs/object-store-segment-size-sweep/` task 1, Requirements 7.1 and
// 7.2): a real three-node cluster over the network simulator, every node's
// engine a synchronous (`write_behind = false`) `object_store_persistence_engine`
// with the segmented log over a mock bucket of its own.
//
// Once node 1 leads, every PUT to node 2's bucket is held for longer than
// node 2's `election_timeout_max` but well inside the leader's
// `append_entries_timeout`, for exactly one AppendEntries run. Node 2 answers
// that run late, with `node::_mutex` held throughout, while the leader keeps
// heartbeating it and node 2's own election tick waits on the mutex. The test
// records what node 2 does next:
//
//  - whether its term moves (it campaigned against a healthy leader), and
//  - whether the leader resent the run (a log key PUT twice).
//
// Which of these holds decides the bound a segment size is judged against
// (design.md, "Requirement 7: the liveness bound"). What it showed when it was
// written: node 2's election timer does fire on release (the tick takes the
// mutex before the queued heartbeat, and the time spent persisting counts),
// but the tick runs a PreVote round, which nodes 1 and 3 refuse because they
// heard from the leader within their own timeouts. So the term never moves
// and the leader is never disturbed; the ceiling on a run's persist time is
// the leader's append_entries_timeout. Every shipped transport supports
// PreVote, and the static_assert below keeps this test honest about relying
// on it: a transport without it would campaign here.
#define BOOST_TEST_MODULE raft_object_store_segment_liveness_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include "mock_object_store.hpp"

#include <raft/object_store_persistence.hpp>
#include <raft/persistence.hpp>
#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

#endif
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

// ── Folly global fixture ───────────────────────────────────────────────────

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_object_store_segment_liveness_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

using store_t = kythira::mock_object_store;
using engine_t = kythira::object_store_persistence_engine<store_t>;
using snapshot_t = engine_t::snapshot_t;

constexpr const char* k_bucket = "kythira";
constexpr std::uint64_t k_snapshot_index = 10;
constexpr std::uint64_t k_seed_term = 1;

// Node 2, the follower under test. Its run is held past election_timeout_max
// and well inside append_entries_timeout (5 s by default), so a resend would
// be the leader's doing and a campaign node 2's own.
constexpr std::chrono::milliseconds k_follower_timeout_min{300};
constexpr std::chrono::milliseconds k_follower_timeout_max{600};
constexpr std::chrono::milliseconds k_heartbeat{50};
constexpr std::chrono::milliseconds k_stall{1500};
constexpr int k_rounds = 5;

template<typename NodeId, typename Address> class preset_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    auto register_node(NodeId, Address) -> kythira::future_default<void> {
        return kythira::future_factory_default::makeFuture();
    }
    [[nodiscard]] auto find_peers(std::chrono::milliseconds) const
        -> kythira::future_default<std::vector<kythira::peer_info<NodeId, Address>>> {
        return kythira::future_factory_default::makeFuture(
            std::vector<kythira::peer_info<NodeId, Address>>{});
    }
};

struct test_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using raft_network_types = kythira::raft_simulator_network_types<std::string>;
    using network_client_type =
        kythira::simulator_network_client<raft_network_types, serializer_type,
                                          serialized_data_type>;
    using network_server_type =
        kythira::simulator_network_server<raft_network_types, serializer_type,
                                          serialized_data_type>;

    using persistence_engine_type = engine_t;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kythira::test_key_value_state_machine<log_index_type>;
    using configuration_type = kythira::raft_configuration;

    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;

    using request_vote_request_type =
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type>;
    using install_snapshot_response_type = kythira::install_snapshot_response<term_id_type>;

    using address_type = std::string;
    using peer_discovery_type = preset_peer_discovery<node_id_type, address_type>;
    using cluster_join_request_type = kythira::cluster_join_request<node_id_type, address_type>;
    using cluster_join_response_type = kythira::cluster_join_response<node_id_type, address_type>;
};

static_assert(kythira::network_client_with_pre_vote<test_types::network_client_type,
                                                    test_types::node_id_type>,
              "the result this test guards depends on PreVote");

using sm_t = kythira::test_key_value_state_machine<std::uint64_t>;
using node_t = kythira::node<test_types>;
using sim_t = network_simulator::NetworkSimulator<test_types::raft_network_types>;

auto prefix_of(std::uint64_t id) -> std::string {
    return "node-" + std::to_string(id);
}

auto sync_options() -> kythira::object_persistence_options {
    kythira::object_persistence_options opts;
    opts.log = kythira::log_layout::segmented;
    opts.write_behind = false;
    return opts;
}

/// Node 1 campaigns first; node 2 has a real timeout and is ticked once node 1
/// leads; node 3's timer never fires within the test.
auto make_config(std::uint64_t id) -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    switch (id) {
        case 1:
            cfg._election_timeout_min = std::chrono::milliseconds{150};
            cfg._election_timeout_max = std::chrono::milliseconds{300};
            break;
        case 2:
            cfg._election_timeout_min = k_follower_timeout_min;
            cfg._election_timeout_max = k_follower_timeout_max;
            break;
        default:
            cfg._election_timeout_min = std::chrono::milliseconds{30000};
            cfg._election_timeout_max = std::chrono::milliseconds{40000};
            break;
    }
    cfg._heartbeat_interval = k_heartbeat;
    cfg._bootstrap_retry_interval = std::chrono::milliseconds{200};
    cfg._bootstrap_peer_find_timeout = std::chrono::milliseconds{100};
    return cfg;
}

/// Every node starts from a snapshot naming {1, 2, 3} at term 1.
auto seed(const store_t& store, std::uint64_t id) -> void {
    engine_t seeder{store, k_bucket, prefix_of(id), sync_options()};
    seeder.save_current_term(k_seed_term);
    sm_t sm;
    seeder.save_snapshot(snapshot_t{
        ._last_included_index = k_snapshot_index,
        ._last_included_term = k_seed_term,
        ._configuration =
            kythira::cluster_configuration<std::uint64_t>{{1, 2, 3}, false, std::nullopt},
        ._state_machine_state = sm.get_state()});
}

/// Log keys PUT more than once since the requests were last cleared: a run
/// the leader sent again after the first one went unanswered.
auto repeated_log_puts(const store_t& store, std::uint64_t id) -> std::vector<std::string> {
    const std::string needle = "PUT " + prefix_of(id) + "/log/";
    std::map<std::string, int> seen;
    for (const auto& request : store.request_log()) {
        if (request.starts_with(needle)) {
            ++seen[request];
        }
    }
    std::vector<std::string> repeated;
    for (const auto& [request, count] : seen) {
        if (count > 1) {
            repeated.push_back(request);
        }
    }
    return repeated;
}

auto set_put_delay(const store_t& store, std::chrono::milliseconds delay) -> void {
    const std::lock_guard lock(store.state()->mu);
    store.state()->put_delay = delay;
}

auto puts_in_flight(const store_t& store) -> std::size_t {
    const std::lock_guard lock(store.state()->mu);
    return store.state()->puts_in_flight;
}

template<typename Pred> auto wait_until(Pred pred, std::chrono::milliseconds deadline) -> bool {
    const auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return true;
}

struct cluster {
    std::vector<store_t> stores = std::vector<store_t>(3);
    sim_t sim;
    std::vector<std::unique_ptr<node_t>> nodes;
    std::atomic<bool> pumping{true};
    std::atomic<bool> tick_follower{false};
    std::jthread leader_pump;
    std::jthread follower_pump;

    cluster() {
        sim.start();
        for (const auto* from : {"1", "2", "3"}) {
            for (const auto* to : {"1", "2", "3"}) {
                if (std::string{from} != to) {
                    sim.add_edge(from, to, network_simulator::NetworkEdge{});
                }
            }
        }
        for (std::uint64_t id = 1; id <= 3; ++id) {
            const auto& store = stores[id - 1];
            seed(store, id);
            auto net = sim.create_node(std::to_string(id));
            nodes.push_back(std::make_unique<node_t>(kythira::node_config<test_types>{
                .node_id = id,
                .network_client =
                    test_types::network_client_type{net, test_types::serializer_type{}},
                .network_server =
                    test_types::network_server_type{net, test_types::serializer_type{}},
                .persistence = engine_t{store, k_bucket, prefix_of(id), sync_options()},
                .logger = kythira::console_logger{kythira::log_level::warning},
                .metrics = test_types::metrics_type{},
                .membership = test_types::membership_manager_type{},
                .config = make_config(id),
                .self_address = std::to_string(id),
                .peer_discovery = preset_peer_discovery<std::uint64_t, std::string>{}}));
        }
        for (auto& n : nodes) {
            n->start();
        }
        // Each node is ticked from a thread of its own, as a binary's timer
        // loop would: node 2's tick blocking on its mutex must not hold up
        // the leader's heartbeats.
        leader_pump = std::jthread([this] {
            while (pumping.load()) {
                nodes[0]->check_election_timeout();
                nodes[0]->check_heartbeat_timeout();
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        });
        follower_pump = std::jthread([this] {
            while (pumping.load()) {
                if (tick_follower.load()) {
                    nodes[1]->check_election_timeout();
                    nodes[1]->check_heartbeat_timeout();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        });
    }

    ~cluster() {
        pumping.store(false);
        leader_pump.join();
        follower_pump.join();
        for (auto& node : std::ranges::reverse_view(nodes)) {
            node->stop();
        }
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;
    cluster(cluster&&) = delete;
    auto operator=(cluster&&) -> cluster& = delete;

    auto leader() -> node_t& { return *nodes[0]; }
    auto follower() -> node_t& { return *nodes[1]; }
    auto follower_store() -> const store_t& { return stores[1]; }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(raft_object_store_segment_liveness)

BOOST_AUTO_TEST_CASE(follower_keeps_its_term_while_persisting_a_slow_run,
                     *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(wait_until([&] { return c.leader().is_leader(); }, std::chrono::seconds{10}));
    // The leader's no-op is on node 2 before its timer starts counting.
    BOOST_REQUIRE(wait_until([&] { return c.follower().last_log_index() > k_snapshot_index; },
                             std::chrono::seconds{10}));
    c.tick_follower.store(true);
    // A quiet second with node 2 ticking: a healthy follower stays put.
    std::this_thread::sleep_for(std::chrono::seconds{1});
    const auto term = c.leader().get_current_term();
    BOOST_REQUIRE(c.leader().is_leader());
    BOOST_REQUIRE_EQUAL(c.follower().get_current_term(), term);

    int campaigned = 0;
    int resent = 0;
    for (int round = 0; round < k_rounds; ++round) {
        c.follower_store().clear_requests();
        const auto before = c.follower().last_log_index();
        // Hold node 2's next run, and only that one: the delay is cleared as
        // soon as its first PUT is inside it.
        set_put_delay(c.follower_store(), k_stall);
        auto submitted = c.leader().submit_command(
            sm_t::make_put_command("k" + std::to_string(round), "v"), std::chrono::seconds{30});
        BOOST_REQUIRE(wait_until([&] { return puts_in_flight(c.follower_store()) > 0; },
                                 std::chrono::seconds{5}));
        set_put_delay(c.follower_store(), std::chrono::milliseconds{0});
        // Node 3 makes the majority, so the command commits while node 2 is
        // still inside its PUT.
        std::move(submitted).get();
        BOOST_REQUIRE(wait_until([&] { return c.follower().last_log_index() > before; },
                                 std::chrono::seconds{10}));
        // Long enough for a campaign started on release to show, and for the
        // leader to send more heartbeats.
        std::this_thread::sleep_for(k_follower_timeout_max * 2);

        const auto follower_term = c.follower().get_current_term();
        const auto repeated = repeated_log_puts(c.follower_store(), 2);
        BOOST_TEST_MESSAGE("round " << round << ": node 2 term " << follower_term
                                    << " (leader's was " << term << "), " << repeated.size()
                                    << " log key(s) PUT twice");
        if (follower_term != term) {
            ++campaigned;
        }
        if (!repeated.empty()) {
            ++resent;
        }
        if (follower_term != term) {
            break;  // a new election: later rounds would measure a different leader
        }
    }
    BOOST_TEST_MESSAGE("campaigned in " << campaigned << ", resent in " << resent << " of up to "
                                        << k_rounds << " rounds");
    BOOST_TEST(campaigned == 0);
    BOOST_TEST(resent == 0);
}

BOOST_AUTO_TEST_SUITE_END()
