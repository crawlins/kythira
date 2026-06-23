/**
 * Coverage tests for raft.hpp private RPC handlers.
 *
 * The handlers (handle_request_vote, handle_append_entries, handle_install_snapshot)
 * are registered as lambdas on the network server during node::start().  The only
 * way to call them is to deliver a real RPC message to the running node's server.
 *
 * Approach:
 *   1. Start a raft follower (the node under test).
 *   2. Create a "fake peer" node in the same simulator.
 *   3. Add a bidirectional edge so messages route.
 *   4. Serialize the desired RPC request using json_rpc_serializer and fire it
 *      from the fake peer to the follower (fire-and-forget; no response needed).
 *   5. Poll until the expected state change is observable.
 *
 * The simulator's retrieve_message(addr, timeout) immediately throws if the queue
 * is empty (no blocking wait), so the raft node's client will fail to receive
 * responses from the fake peer.  We deliberately ignore those failures and only
 * check server-side state on the follower.
 */

#define BOOST_TEST_MODULE raft_rpc_handler_coverage_test
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#include <folly/init/Init.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_rpc_handler_coverage_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {

struct test_raft_types {
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
};

using sim_t = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>;
using node_t = kythira::node<test_raft_types>;
using net_node_t = network_simulator::NetworkNode<test_raft_types::raft_network_types>;
using ser_t = test_raft_types::serializer_type;
using data_t = test_raft_types::serialized_data_type;

kythira::raft_configuration fast_cfg() {
    kythira::raft_configuration c;
    c._election_timeout_min = std::chrono::milliseconds{200};
    c._election_timeout_max = std::chrono::milliseconds{400};
    c._heartbeat_interval = std::chrono::milliseconds{50};
    c._rpc_timeout = std::chrono::milliseconds{100};
    return c;
}

// Add a zero-latency bidirectional edge between two addresses
void connect(sim_t& sim, const std::string& a, const std::string& b) {
    network_simulator::NetworkEdge e{std::chrono::milliseconds{0}, 1.0};
    sim.add_edge(a, b, e);
    sim.add_edge(b, a, e);
}

// Fire a serialized RPC message from fake_peer → target_addr
template<typename Msg>
void fire_rpc(std::shared_ptr<net_node_t> src, const std::string& dst_addr, const Msg& msg) {
    ser_t ser;
    auto payload = ser.serialize(msg);
    test_raft_types::raft_network_types::message_type nm(src->address(), 0, dst_addr, 5000,
                                                         payload);
    src->send(std::move(nm));
}

// Poll until pred() returns true or deadline passes
template<typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{3000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

}  // namespace

// ── Suite: handle_request_vote ────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(handle_request_vote_suite)

BOOST_AUTO_TEST_CASE(follower_grants_vote_for_higher_term, *boost::unit_test::timeout(15)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");  // raft node under test
    auto net2 = sim.create_node("2");  // fake peer / "candidate"
    connect(sim, "1", "2");

    node_t raft1{1,
                 test_raft_types::network_client_type{net1, ser_t{}},
                 test_raft_types::network_server_type{net1, ser_t{}},
                 test_raft_types::persistence_engine_type{},
                 kythira::console_logger{kythira::log_level::error},
                 test_raft_types::metrics_type{},
                 test_raft_types::membership_manager_type{},
                 fast_cfg()};
    raft1.start();

    // Confirm follower state before sending
    BOOST_CHECK_EQUAL(raft1.get_state(), kythira::server_state::follower);
    BOOST_CHECK_EQUAL(raft1.get_current_term(), 0u);

    // Send RequestVote from peer 2 with term=1 (higher than follower's term=0)
    test_raft_types::request_vote_request_type req;
    req._term = 1;
    req._candidate_id = 2;
    req._last_log_index = 0;
    req._last_log_term = 0;
    fire_rpc(net2, "1", req);

    // The follower should update its term to 1 after processing the vote request
    BOOST_CHECK(wait_for([&] { return raft1.get_current_term() >= 1u; }));
    BOOST_CHECK_GE(raft1.get_current_term(), 1u);

    raft1.stop();
}

BOOST_AUTO_TEST_CASE(follower_rejects_lower_term_vote, *boost::unit_test::timeout(15)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    connect(sim, "1", "2");

    node_t raft1{1,
                 test_raft_types::network_client_type{net1, ser_t{}},
                 test_raft_types::network_server_type{net1, ser_t{}},
                 test_raft_types::persistence_engine_type{},
                 kythira::console_logger{kythira::log_level::error},
                 test_raft_types::metrics_type{},
                 test_raft_types::membership_manager_type{},
                 fast_cfg()};
    raft1.start();

    // First, artificially advance the follower to term 3 by sending a higher-term vote request
    test_raft_types::request_vote_request_type req_high;
    req_high._term = 3;
    req_high._candidate_id = 2;
    req_high._last_log_index = 0;
    req_high._last_log_term = 0;
    fire_rpc(net2, "1", req_high);
    BOOST_CHECK(wait_for([&] { return raft1.get_current_term() >= 3u; }));

    // Now send a stale vote request with term=1 (lower than current)
    test_raft_types::request_vote_request_type req_low;
    req_low._term = 1;
    req_low._candidate_id = 2;
    req_low._last_log_index = 0;
    req_low._last_log_term = 0;
    fire_rpc(net2, "1", req_low);

    // Term should remain 3 (no regression)
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    BOOST_CHECK_GE(raft1.get_current_term(), 3u);

    raft1.stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite: handle_append_entries ──────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(handle_append_entries_suite)

BOOST_AUTO_TEST_CASE(follower_accepts_heartbeat_from_leader, *boost::unit_test::timeout(15)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");  // follower under test
    auto net2 = sim.create_node("2");  // fake leader
    connect(sim, "1", "2");

    node_t raft1{1,
                 test_raft_types::network_client_type{net1, ser_t{}},
                 test_raft_types::network_server_type{net1, ser_t{}},
                 test_raft_types::persistence_engine_type{},
                 kythira::console_logger{kythira::log_level::error},
                 test_raft_types::metrics_type{},
                 test_raft_types::membership_manager_type{},
                 fast_cfg()};
    raft1.start();
    BOOST_CHECK_EQUAL(raft1.get_state(), kythira::server_state::follower);

    // Send an AppendEntries heartbeat (empty entries) from "leader" at term=1
    test_raft_types::append_entries_request_type heartbeat;
    heartbeat._term = 1;
    heartbeat._leader_id = 2;
    heartbeat._prev_log_index = 0;
    heartbeat._prev_log_term = 0;
    heartbeat._leader_commit = 0;

    fire_rpc(net2, "1", heartbeat);

    // Follower should update its term to 1 (recognising a valid leader heartbeat)
    BOOST_CHECK(wait_for([&] { return raft1.get_current_term() >= 1u; }));
    BOOST_CHECK_GE(raft1.get_current_term(), 1u);
    // Follower remains a follower when it receives a valid heartbeat
    BOOST_CHECK_EQUAL(raft1.get_state(), kythira::server_state::follower);

    raft1.stop();
}

BOOST_AUTO_TEST_CASE(follower_rejects_stale_append_entries, *boost::unit_test::timeout(15)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    connect(sim, "1", "2");

    node_t raft1{1,
                 test_raft_types::network_client_type{net1, ser_t{}},
                 test_raft_types::network_server_type{net1, ser_t{}},
                 test_raft_types::persistence_engine_type{},
                 kythira::console_logger{kythira::log_level::error},
                 test_raft_types::metrics_type{},
                 test_raft_types::membership_manager_type{},
                 fast_cfg()};
    raft1.start();

    // Advance the node's term first
    test_raft_types::request_vote_request_type adv;
    adv._term = 5;
    adv._candidate_id = 2;
    fire_rpc(net2, "1", adv);
    BOOST_CHECK(wait_for([&] { return raft1.get_current_term() >= 5u; }));

    // Send stale AppendEntries (term=1 < current term=5)
    test_raft_types::append_entries_request_type stale;
    stale._term = 1;
    stale._leader_id = 2;
    fire_rpc(net2, "1", stale);

    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    // Term should not decrease
    BOOST_CHECK_GE(raft1.get_current_term(), 5u);

    raft1.stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite: handle_install_snapshot ───────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(handle_install_snapshot_suite)

BOOST_AUTO_TEST_CASE(follower_processes_snapshot, *boost::unit_test::timeout(15)) {
    sim_t sim;
    sim.start();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    connect(sim, "1", "2");

    node_t raft1{1,
                 test_raft_types::network_client_type{net1, ser_t{}},
                 test_raft_types::network_server_type{net1, ser_t{}},
                 test_raft_types::persistence_engine_type{},
                 kythira::console_logger{kythira::log_level::error},
                 test_raft_types::metrics_type{},
                 test_raft_types::membership_manager_type{},
                 fast_cfg()};
    raft1.start();

    // Send InstallSnapshot from a leader with term=2
    test_raft_types::install_snapshot_request_type snap;
    snap._term = 2;
    snap._leader_id = 2;
    snap._last_included_index = 5;
    snap._last_included_term = 2;
    snap._offset = 0;
    snap._done = true;

    fire_rpc(net2, "1", snap);

    // Follower should update its term when it receives the snapshot message
    BOOST_CHECK(wait_for([&] { return raft1.get_current_term() >= 2u; }));
    BOOST_CHECK_GE(raft1.get_current_term(), 2u);

    raft1.stop();
}

BOOST_AUTO_TEST_SUITE_END()
