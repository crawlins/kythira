// Tests for the learner (non-voting node) mechanism this feature is built on:
// serialization round-trip, election exclusion, and the add_learner()/
// remove_learner() admission and removal entry points.
//
// See .kiro/specs/non-voting-nodes/ — the capacity criteria layered on top of
// this mechanism are covered by learner_admission_capacity_test.cpp,
// learner_promotion_capacity_test.cpp, quorum_promotion_capacity_fallback_test.cpp,
// and learner_promotion_retry_test.cpp.

#define BOOST_TEST_MODULE learner_mechanism_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/config_entry.hpp>
#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

// ── Folly global fixture ───────────────────────────────────────────────────

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("learner_mechanism_unit_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

// ── Serialization round-trip (no node required) ─────────────────────────────

BOOST_AUTO_TEST_SUITE(learner_serialization)

BOOST_AUTO_TEST_CASE(round_trips_learners) {
    kythira::cluster_configuration<std::uint64_t> cfg;
    cfg._nodes = {1, 2, 3};
    cfg._is_joint_consensus = false;
    cfg._old_nodes = std::nullopt;
    cfg._learners = {4, 5};

    auto bytes = kythira::serialize_configuration<std::uint64_t>(cfg);
    auto round_tripped = kythira::deserialize_configuration<std::uint64_t>(bytes);

    BOOST_CHECK(round_tripped.nodes() == cfg.nodes());
    BOOST_CHECK(round_tripped.learners() == cfg.learners());
}

BOOST_AUTO_TEST_CASE(missing_learners_key_defaults_to_empty) {
    // Hand-craft a payload matching what a pre-learner-feature log entry looked like:
    // no "learners" key at all. Backward compatibility requires this to deserialize
    // cleanly with an empty learner set, not throw.
    boost::json::object obj;
    boost::json::array nodes;
    nodes.push_back(static_cast<std::uint64_t>(1));
    nodes.push_back(static_cast<std::uint64_t>(2));
    obj["nodes"] = std::move(nodes);
    obj["is_joint_consensus"] = false;
    auto s = boost::json::serialize(obj);
    std::vector<std::byte> bytes;
    bytes.reserve(s.size());
    for (char c : s) {
        bytes.push_back(static_cast<std::byte>(c));
    }

    auto cfg = kythira::deserialize_configuration<std::uint64_t>(bytes);
    BOOST_CHECK(cfg.learners().empty());
    BOOST_CHECK_EQUAL(cfg.nodes().size(), 2u);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Shared test scaffolding for node-level tests ────────────────────────────

namespace {

template<typename NodeId, typename Address> class preset_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;

    preset_peer_discovery() = default;
    explicit preset_peer_discovery(std::vector<kythira::peer_info<NodeId, Address>> peers)
        : _peers(std::move(peers)) {}

    auto register_node(NodeId, Address) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }
    [[nodiscard]] auto find_peers(std::chrono::milliseconds) const
        -> kythira::Future<std::vector<kythira::peer_info<NodeId, Address>>> {
        return kythira::FutureFactory::makeFuture(
            std::vector<kythira::peer_info<NodeId, Address>>(_peers));
    }

private:
    std::vector<kythira::peer_info<NodeId, Address>> _peers;
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

    // Generous, single-default-group topology so add_learner()/promote_to_voter()'s
    // placement-capacity criterion never blocks these mechanism-level tests — capacity
    // itself is exercised separately in learner_admission_capacity_test.cpp and
    // learner_promotion_capacity_test.cpp.
    using quorum_manager_type =
        kythira::no_op_quorum_manager<node_id_type, address_type, std::string>;
};

using test_node = kythira::node<test_types>;
using sim_t = network_simulator::NetworkSimulator<test_types::raft_network_types>;

kythira::raft_configuration make_fast_config() {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{80};
    cfg._election_timeout_max = std::chrono::milliseconds{160};
    cfg._heartbeat_interval = std::chrono::milliseconds{26};
    cfg._rpc_timeout = std::chrono::milliseconds{200};
    cfg._bootstrap_retry_interval = std::chrono::milliseconds{200};
    cfg._bootstrap_peer_find_timeout = std::chrono::milliseconds{100};
    return cfg;
}

template<typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{5000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

void connect_all(sim_t& sim, std::initializer_list<std::string> addresses) {
    network_simulator::NetworkEdge edge{};
    for (const auto& from : addresses) {
        for (const auto& to : addresses) {
            if (from != to) {
                sim.add_edge(from, to, edge);
            }
        }
    }
}

// Generous default-group topology: every node's placement defaults to the empty
// string group (no set_placement() calls in these tests), so a single generous
// target keeps the capacity criterion out of the way.
test_types::quorum_manager_type generous_quorum_manager() {
    return test_types::quorum_manager_type{
        kythira::desired_topology<std::string>{.groups = {{.group_id = "", .target_count = 10}}}};
}

kythira::node_config<test_types> make_node_config(std::uint64_t id, auto net,
                                                  const kythira::raft_configuration& cfg) {
    return kythira::node_config<test_types>{
        .node_id = id,
        .network_client = {net, test_types::serializer_type{}},
        .network_server = {net, test_types::serializer_type{}},
        .persistence = {},
        .logger = kythira::console_logger{},
        .metrics = {},
        .membership = {},
        .config = cfg,
        .self_address = std::to_string(id),
        .peer_discovery = preset_peer_discovery<std::uint64_t, std::string>{},
        .quorum_manager = generous_quorum_manager(),
    };
}

}  // namespace

// ── Election exclusion ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(learner_election_exclusion)

BOOST_AUTO_TEST_CASE(learner_never_starts_election, *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    connect_all(sim, {"1", "2"});

    auto cfg = make_fast_config();
    test_node node1{make_node_config(1, net1, cfg)};
    test_node node2{make_node_config(2, net2, cfg)};

    node1.set_cluster_configuration({1});
    node2.set_cluster_configuration({2});  // node2 starts as its own tiny single-node config;
                                           // add_learner() below overwrites its live view once
                                           // it replicates from node1.

    node1.start();
    node2.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    auto add_fut = node1.add_learner(2);
    for (int i = 0; i < 15; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    bool add_done = false;
    std::move(add_fut)
        .thenValue([&](std::vector<std::byte>) { add_done = true; })
        .thenError([&](const std::exception_ptr&) { add_done = true; });
    BOOST_REQUIRE(wait_until([&] { return add_done; }));

    // node2 is now a learner replicating from node1. Stop node1 so heartbeats stop
    // arriving, then drive node2's election-timeout check repeatedly — a voting
    // follower would become a candidate; a learner must not.
    node1.stop();
    for (int i = 0; i < 6; ++i) {
        std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
        node2.check_election_timeout();
    }
    BOOST_CHECK(node2.get_state() == kythira::server_state::follower);
    BOOST_CHECK(!node2.is_leader());

    node2.stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── add_learner() / remove_learner() ────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(learner_admission_and_removal)

BOOST_AUTO_TEST_CASE(add_learner_rejects_when_not_leader, *boost::unit_test::timeout(10)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto cfg = make_fast_config();

    test_node node1{make_node_config(1, net1, cfg)};
    node1.set_cluster_configuration(
        {1, 2});  // node1 starts as a follower (2-node config, no peer up)
    node1.start();

    // node1 has not been elected leader yet.
    auto fut = node1.add_learner(3);
    bool threw = false;
    std::move(fut)
        .thenValue([&](std::vector<std::byte>) {})
        .thenError([&](const std::exception_ptr&) { threw = true; });
    BOOST_CHECK(wait_until([&] { return threw; }, std::chrono::milliseconds{2000}));

    node1.stop();
}

BOOST_AUTO_TEST_CASE(add_learner_and_remove_learner_round_trip, *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    connect_all(sim, {"1", "2"});

    auto cfg = make_fast_config();
    test_node node1{make_node_config(1, net1, cfg)};
    test_node node2{make_node_config(2, net2, cfg)};

    node1.set_cluster_configuration({1});
    node2.set_cluster_configuration({2});

    node1.start();
    node2.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }));

    // Admitting the same node twice must fail the second time.
    auto add_fut = node1.add_learner(2);
    for (int i = 0; i < 15; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    bool add_done = false, add_threw = false;
    std::move(add_fut)
        .thenValue([&](std::vector<std::byte>) { add_done = true; })
        .thenError([&](const std::exception_ptr&) { add_threw = true; });
    BOOST_REQUIRE(wait_until([&] { return add_done || add_threw; }));
    BOOST_REQUIRE(!add_threw);

    auto dup_fut = node1.add_learner(2);
    bool dup_threw = false;
    std::move(dup_fut)
        .thenValue([&](std::vector<std::byte>) {})
        .thenError([&](const std::exception_ptr&) { dup_threw = true; });
    BOOST_CHECK(wait_until([&] { return dup_threw; }, std::chrono::milliseconds{2000}));

    // Learner replicates: a command submitted after admission is applied on both nodes.
    using sm_t = kythira::test_key_value_state_machine<test_types::log_index_type>;
    auto cmd = sm_t::make_put_command("k", "v");
    auto cmd_fut = node1.submit_command(cmd, std::chrono::milliseconds{3000});
    bool cmd_applied = false;
    std::move(cmd_fut)
        .thenValue([&](std::vector<std::byte>) { cmd_applied = true; })
        .thenError([&](const std::exception_ptr&) {});
    for (int i = 0; i < 15; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    BOOST_CHECK(wait_until([&] { return cmd_applied; }, std::chrono::milliseconds{3000}));
    BOOST_CHECK(wait_until(
        [&] { return node2.debug_state().last_applied == node1.debug_state().last_applied; },
        std::chrono::milliseconds{3000}));

    // Voting membership is unaffected by learner admission.
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 1u);

    // Remove the learner, then confirm it can be re-admitted (proving it was actually removed).
    auto remove_fut = node1.remove_learner(2);
    for (int i = 0; i < 15; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    bool remove_done = false, remove_threw = false;
    std::move(remove_fut)
        .thenValue([&](std::vector<std::byte>) { remove_done = true; })
        .thenError([&](const std::exception_ptr&) { remove_threw = true; });
    BOOST_REQUIRE(wait_until([&] { return remove_done || remove_threw; }));
    BOOST_REQUIRE(!remove_threw);

    auto readd_fut = node1.add_learner(2);
    bool readd_done = false, readd_threw = false;
    for (int i = 0; i < 15; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    std::move(readd_fut)
        .thenValue([&](std::vector<std::byte>) { readd_done = true; })
        .thenError([&](const std::exception_ptr&) { readd_threw = true; });
    BOOST_CHECK(wait_until([&] { return readd_done || readd_threw; }));
    BOOST_CHECK(!readd_threw);

    node2.stop();
    node1.stop();
}

BOOST_AUTO_TEST_SUITE_END()
