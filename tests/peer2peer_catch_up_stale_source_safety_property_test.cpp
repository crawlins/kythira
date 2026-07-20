// Property test for .kiro/specs/peer2peer-log-replication/ Task 19
// (design.md Property 3, Requirements 6.4/6.5): entries fetched via
// peer-to-peer catch-up from a source that turns out to hold an abandoned,
// never-committed tail are safely superseded once a legitimately
// higher-term leader's normal heartbeats arrive — no crash, no stuck state,
// no operator intervention required.
//
// Simulating "abandoned term" via pure network manipulation of a 3-node
// cluster is not possible without violating Raft's own Election Safety
// property: a node holding a longer log (even an uncommitted tail it
// authored, or copied via peer-to-peer fetch from the node that did)
// always outranks a shorter-log competitor in any subsequent election, so
// with only 3 nodes there is no way to elect a leader that excludes both
// the original author and the node that fetched its tail. This test
// instead uses 5 real nodes: node 1 (leader) appends a command locally
// without ever replicating it, node 2 fetches that unreplicated tail via
// the real peer-to-peer path (exactly what Task 19 is about), and nodes
// 3/4/5 — genuinely untouched by any of this — form a real majority (3 of
// 5) entirely on their own once node 1 and node 2 are network-isolated,
// electing a new leader in a higher term with a *different* command at the
// same log index. Reconnecting node 1 and node 2 then exercises the exact
// safety property under test: ordinary AppendEntries from the new leader
// must detect the term conflict at that index and truncate/replace the
// stale entry, on both nodes, without any special-casing for how they
// came to have it.
#define BOOST_TEST_MODULE peer2peer_catch_up_stale_source_safety_property_test
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ── Folly global fixture ───────────────────────────────────────────────────

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("peer2peer_catch_up_stale_source_safety_property_test"),
                         nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {

template<typename NodeId, typename Address> class preset_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    preset_peer_discovery() = default;
    auto register_node(NodeId, Address) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }
    [[nodiscard]] auto find_peers(std::chrono::milliseconds) const
        -> kythira::Future<std::vector<kythira::peer_info<NodeId, Address>>> {
        return kythira::FutureFactory::makeFuture(
            std::vector<kythira::peer_info<NodeId, Address>>{});
    }
};

struct test_types {
    using future_type = kythira::Future<std::vector<std::byte>>;
    using promise_type = kythira::Promise<std::vector<std::byte>>;
    using try_type = kythira::Try<std::vector<std::byte>>;

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

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
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

    using peer2peer_replicator_type =
        kythira::static_peer2peer_replicator<node_id_type, address_type, log_index_type>;
};

using test_node = kythira::node<test_types>;
using sim_t = network_simulator::NetworkSimulator<test_types::raft_network_types>;
using replicator_t = test_types::peer2peer_replicator_type;

template<typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{8000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{15});
    }
    return true;
}

kythira::raft_configuration make_fast_config() {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{80};
    cfg._election_timeout_max = std::chrono::milliseconds{160};
    cfg._heartbeat_interval = std::chrono::milliseconds{20};
    cfg._rpc_timeout = std::chrono::milliseconds{200};
    cfg._progress_gossip_interval = std::chrono::milliseconds{25};
    cfg._catch_up_fetch_timeout = std::chrono::milliseconds{300};
    return cfg;
}

// Never elects on its own within the test window — used for nodes 4 and 5
// so node 3 is deterministically the one to campaign for the replacement
// election, rather than racing three simultaneous candidates.
kythira::raft_configuration make_dormant_config() {
    kythira::raft_configuration cfg = make_fast_config();
    cfg._election_timeout_min = std::chrono::minutes{10};
    cfg._election_timeout_max = std::chrono::minutes{10} + std::chrono::milliseconds{1};
    return cfg;
}

}  // namespace

BOOST_AUTO_TEST_CASE(stale_peer_fetched_entry_superseded_by_higher_term_leader,
                     *boost::unit_test::timeout(30)) {
    auto table = std::make_shared<replicator_t::table_type>();

    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    auto net4 = sim.create_node("4");
    auto net5 = sim.create_node("5");

    std::vector<std::string> ids = {"1", "2", "3", "4", "5"};
    network_simulator::NetworkEdge edge{};
    for (const auto& from : ids) {
        for (const auto& to : ids) {
            if (from != to) {
                sim.add_edge(from, to, edge);
            }
        }
    }

    auto cfg = make_fast_config();
    auto cfg_dormant = make_dormant_config();

    auto make_node = [&](std::uint64_t id, auto net, const kythira::raft_configuration& c) {
        return test_node{id,
                         {net, test_types::serializer_type{}},
                         {net, test_types::serializer_type{}},
                         {},
                         kythira::console_logger{},
                         {},
                         {},
                         c,
                         std::to_string(id),
                         preset_peer_discovery<std::uint64_t, std::string>{},
                         replicator_t{table}};
    };

    auto node1 = make_node(1, net1, cfg);
    auto node2 = make_node(2, net2, cfg);
    auto node3 = make_node(3, net3, cfg);
    auto node4 = make_node(4, net4, cfg_dormant);
    auto node5 = make_node(5, net5, cfg_dormant);

    for (auto* n : {&node1, &node2, &node3, &node4, &node5}) {
        n->set_cluster_configuration({1, 2, 3, 4, 5});
    }
    for (auto* n : {&node1, &node2, &node3, &node4, &node5}) {
        n->start();
    }

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    auto pump_heartbeats = [&](int cycles) {
        for (int i = 0; i < cycles; ++i) {
            node1.check_heartbeat_timeout();
            std::this_thread::sleep_for(cfg._heartbeat_interval);
        }
    };

    // Real, committed prefix — replicated to all five.
    for (int i = 0; i < 2; ++i) {
        auto command = kythira::test_key_value_state_machine<std::uint64_t>::make_put_command(
            "prefix" + std::to_string(i), "v");
        try {
            node1.submit_command(command, std::chrono::milliseconds{500});
        } catch (...) {
        }
        pump_heartbeats(1);
    }
    pump_heartbeats(10);
    auto prefix_index = node1.debug_state().commit_index;
    BOOST_REQUIRE_GT(prefix_index, 0u);
    BOOST_REQUIRE(wait_until([&] { return node5.debug_state().last_applied >= prefix_index; }));

    // submit_command() itself calls replicate_to_followers() synchronously —
    // it does not wait for the next heartbeat tick — so a "local-only"
    // entry is only achievable by having no reachable followers at the
    // moment it is submitted. Isolate node1 from everyone first.
    for (auto other : {"2", "3", "4", "5"}) {
        sim.remove_edge("1", other);
        sim.remove_edge(other, "1");
    }
    auto poisoned_command =
        kythira::test_key_value_state_machine<std::uint64_t>::make_put_command("poisoned", "v");
    try {
        node1.submit_command(poisoned_command, std::chrono::milliseconds{500});
    } catch (...) {
    }
    auto poisoned_index = node1.debug_state().log.size();
    BOOST_REQUIRE_EQUAL(poisoned_index, prefix_index + 1);

    // Reconnect only node1<->node2 (never node1<->{3,4,5}) and drive ONLY
    // node2's own tick (never node1.check_heartbeat_timeout() again) — the
    // pull-based fetch path is the only way node2 can acquire this entry.
    sim.add_edge("1", "2", edge);
    sim.add_edge("2", "1", edge);
    BOOST_REQUIRE(wait_until(
        [&] {
            node1.check_election_timeout();
            node2.check_election_timeout();
            return static_cast<std::size_t>(node2.debug_state().log.size()) >= poisoned_index;
        },
        std::chrono::milliseconds{4000}));
    BOOST_REQUIRE(node2.debug_state().log.back().command() == poisoned_command);

    // node1<->node2 no longer needs to stay connected to each other for
    // what follows; leave as-is (both remain isolated from {3,4,5} below).

    // Isolate the polluted pair {1,2} from the clean trio {3,4,5}.
    for (auto polluted : {"1", "2"}) {
        for (auto clean : {"3", "4", "5"}) {
            sim.remove_edge(polluted, clean);
            sim.remove_edge(clean, polluted);
        }
    }

    // node3 (only non-dormant node among {3,4,5}) wins a real election using
    // just node4/node5's votes — neither node1 nor node2 is reachable, and
    // neither is needed: 3 of 5 is majority regardless.
    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node3.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    BOOST_REQUIRE(wait_until([&] { return node3.is_leader(); }));

    auto pump_node3_heartbeats = [&](int cycles) {
        for (int i = 0; i < cycles; ++i) {
            node3.check_heartbeat_timeout();
            std::this_thread::sleep_for(cfg._heartbeat_interval);
        }
    };

    // node3 commits a DIFFERENT command at the same log index the poisoned
    // entry occupies — this is the legitimate replacement.
    auto corrected_command =
        kythira::test_key_value_state_machine<std::uint64_t>::make_put_command("corrected", "v");
    try {
        node3.submit_command(corrected_command, std::chrono::milliseconds{500});
    } catch (...) {
    }
    pump_node3_heartbeats(10);
    // node3's own become_leader() appends a no-op barrier entry (Raft
    // §5.4.2) before corrected_command is ever submitted, so it lands at
    // exactly the log slot node1's poisoned entry occupies — same
    // position, higher term, which is the direct safety-property proof: a
    // higher-term leader's own entry supersedes a stale entry at that slot
    // before any client command lands. corrected_command itself lands one
    // slot further, right after the no-op — hence poisoned_index + 1 here.
    BOOST_REQUIRE(wait_until([&] { return node4.debug_state().log.size() >= poisoned_index + 1; }));
    BOOST_CHECK_GT(node3.debug_state().log[poisoned_index - 1].term(),
                   node1.debug_state().log[poisoned_index - 1].term());
    BOOST_CHECK(node3.debug_state().log[poisoned_index].command() == corrected_command);

    // Heal: reconnect node1 and node2 to the (now higher-term) real cluster.
    for (auto id : {"1", "2"}) {
        for (auto other : {"3", "4", "5"}) {
            sim.add_edge(id, other, edge);
            sim.add_edge(other, id, edge);
        }
    }

    // Safety property under test: ordinary heartbeats from the new,
    // higher-term leader must detect the term conflict at poisoned_index
    // and truncate/replace it (with its own no-op barrier entry, then
    // corrected_command right after) on both node1 and node2 — no crash,
    // no exception, no stuck state, no operator intervention.
    BOOST_REQUIRE(wait_until(
        [&] {
            node1.check_election_timeout();
            node2.check_election_timeout();
            node3.check_heartbeat_timeout();
            return node1.debug_state().log.size() >= poisoned_index + 1 &&
                   node1.debug_state().log[poisoned_index].command() == corrected_command &&
                   node2.debug_state().log.size() >= poisoned_index + 1 &&
                   node2.debug_state().log[poisoned_index].command() == corrected_command;
        },
        std::chrono::milliseconds{6000}));

    auto leader_log = node3.debug_state().log;
    auto node1_log = node1.debug_state().log;
    auto node2_log = node2.debug_state().log;
    BOOST_REQUIRE_EQUAL(node1_log.size(), leader_log.size());
    BOOST_REQUIRE_EQUAL(node2_log.size(), leader_log.size());
    for (std::size_t i = 0; i < leader_log.size(); ++i) {
        BOOST_CHECK_EQUAL(node1_log[i].term(), leader_log[i].term());
        BOOST_CHECK(node1_log[i].command() == leader_log[i].command());
        BOOST_CHECK_EQUAL(node2_log[i].term(), leader_log[i].term());
        BOOST_CHECK(node2_log[i].command() == leader_log[i].command());
    }

    node5.stop();
    node4.stop();
    node3.stop();
    node2.stop();
    node1.stop();
}
