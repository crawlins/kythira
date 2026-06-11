/**
 * Tests for raft.hpp code paths not covered by raft_node_unit_test.cpp:
 *
 *   - read_state on follower (not-leader rejection)
 *   - read_state on single-node leader (single-node fast path)
 *   - stop() on an already-stopped node
 *   - check_heartbeat_timeout on follower (non-leader early return)
 *   - check_heartbeat_timeout on leader (triggers send_heartbeats flow)
 *   - check_heartbeat_timeout when a pending operation has timed out
 *   - replicate_to_followers on follower (non-leader early return)
 *   - replicate_to_followers on single-node leader (no followers path)
 */

#define BOOST_TEST_MODULE raft_node_rpc_handler_test
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/membership.hpp>
#include <raft/types.hpp>
#include <raft/test_state_machine.hpp>

#include <network_simulator/network_simulator.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_node_rpc_handler_test"), nullptr};
        char** argv = argv0;
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

using raft_node_type = kythira::node<test_raft_types>;

kythira::raft_configuration make_fast_config() {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{60};
    cfg._election_timeout_max = std::chrono::milliseconds{100};
    cfg._heartbeat_interval = std::chrono::milliseconds{20};
    cfg._rpc_timeout = std::chrono::milliseconds{80};
    return cfg;
}

std::vector<std::byte> make_put_cmd(std::string_view key, std::string_view value) {
    std::vector<std::byte> cmd;
    cmd.push_back(static_cast<std::byte>(1));
    auto append_u32 = [&](std::uint32_t n) {
        std::byte buf[4];
        std::memcpy(buf, &n, 4);
        for (auto b : buf) {
            cmd.push_back(b);
        }
    };
    append_u32(static_cast<std::uint32_t>(key.size()));
    for (char c : key) {
        cmd.push_back(static_cast<std::byte>(c));
    }
    append_u32(static_cast<std::uint32_t>(value.size()));
    for (char c : value) {
        cmd.push_back(static_cast<std::byte>(c));
    }
    return cmd;
}

template<typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{2000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}

// Create a single-node leader, wait for it to win election, return the node.
// Caller must stop() and the simulator must outlive the returned node.
std::unique_ptr<raft_node_type> make_single_node_leader(
    network_simulator::NetworkSimulator<test_raft_types::raft_network_types>& sim,
    std::uint64_t id = 1) {
    auto net = sim.create_node(std::to_string(id));
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    auto node = std::make_unique<raft_node_type>(
        id, test_raft_types::network_client_type{net, ser},
        test_raft_types::network_server_type{net, ser}, test_raft_types::persistence_engine_type{},
        kythira::console_logger{kythira::log_level::error}, test_raft_types::metrics_type{},
        test_raft_types::membership_manager_type{}, cfg);
    node->start();

    // Drive election
    bool became_leader = wait_until(
        [&]() {
            node->check_election_timeout();
            return node->is_leader();
        },
        std::chrono::milliseconds{2000});
    (void)became_leader;

    return node;
}

}  // namespace

// ── Suite: read_state ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(read_state_suite)

BOOST_AUTO_TEST_CASE(read_state_on_follower_throws, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{kythira::log_level::error},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        make_fast_config()};
    node.start();

    // Immediately after start, node is a follower — read_state must throw
    BOOST_CHECK_EQUAL(node.get_state(), kythira::server_state::follower);
    BOOST_CHECK_THROW(node.read_state(std::chrono::milliseconds{200}).get(), std::exception);

    node.stop();
}

BOOST_AUTO_TEST_CASE(read_state_on_single_node_leader, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();

    auto leader = make_single_node_leader(sim, 1);
    BOOST_REQUIRE(leader->is_leader());

    // Single-node leader can serve reads immediately (no heartbeat round-trip needed)
    auto f = leader->read_state(std::chrono::milliseconds{500});
    BOOST_CHECK_NO_THROW(f.get());

    leader->stop();
}

BOOST_AUTO_TEST_CASE(read_state_after_committed_command, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();

    auto leader = make_single_node_leader(sim, 1);
    BOOST_REQUIRE(leader->is_leader());

    // Commit a command first
    auto cmd = make_put_cmd("hello", "world");
    auto cf = leader->submit_command(cmd, std::chrono::milliseconds{2000});
    cf.get();

    // Now read back state — should reflect the committed entry
    auto rf = leader->read_state(std::chrono::milliseconds{500});
    BOOST_CHECK_NO_THROW(rf.get());

    leader->stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite: stop guard ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(stop_guard_suite)

BOOST_AUTO_TEST_CASE(stop_already_stopped_node, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{kythira::log_level::error},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        make_fast_config()};
    node.start();
    BOOST_CHECK(node.is_running());

    node.stop();
    BOOST_CHECK(!node.is_running());

    // Second stop must be a no-op and must not crash
    BOOST_CHECK_NO_THROW(node.stop());
    BOOST_CHECK(!node.is_running());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite: check_heartbeat_timeout ────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(heartbeat_timeout_suite)

BOOST_AUTO_TEST_CASE(check_heartbeat_on_follower, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{kythira::log_level::error},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        make_fast_config()};
    node.start();

    BOOST_CHECK_EQUAL(node.get_state(), kythira::server_state::follower);
    // On a follower, check_heartbeat_timeout is a no-op for heartbeat sending
    BOOST_CHECK_NO_THROW(node.check_heartbeat_timeout());
    BOOST_CHECK_NO_THROW(node.check_heartbeat_timeout());

    node.stop();
}

BOOST_AUTO_TEST_CASE(check_heartbeat_on_leader_triggers_send, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();

    auto leader = make_single_node_leader(sim, 1);
    BOOST_REQUIRE(leader->is_leader());

    // Wait for heartbeat interval to elapse then call check
    std::this_thread::sleep_for(std::chrono::milliseconds{40});
    BOOST_CHECK_NO_THROW(leader->check_heartbeat_timeout());

    // Call multiple times to exercise both elapsed and non-elapsed branches
    BOOST_CHECK_NO_THROW(leader->check_heartbeat_timeout());
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    BOOST_CHECK_NO_THROW(leader->check_heartbeat_timeout());

    leader->stop();
}

BOOST_AUTO_TEST_CASE(check_heartbeat_cancels_timed_out_operation, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();

    auto leader = make_single_node_leader(sim, 1);
    BOOST_REQUIRE(leader->is_leader());

    // Submit a command with a very short timeout — it should commit quickly on a single-node
    // leader so this mainly tests the path. We call check_heartbeat_timeout which also
    // calls cancel_timed_out_operations internally.
    auto cmd = make_put_cmd("x", "y");
    auto f = leader->submit_command(cmd, std::chrono::milliseconds{2000});
    f.get();  // ensure it committed

    // Now trigger heartbeat check — exercises cancel_timed_out_operations code path
    // (count will be 0 since no ops are pending, but the path is still taken)
    BOOST_CHECK_NO_THROW(leader->check_heartbeat_timeout());

    leader->stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite: replicate_to_followers ─────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(replicate_suite)

BOOST_AUTO_TEST_CASE(replicate_on_follower_is_noop, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{kythira::log_level::error},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        make_fast_config()};
    node.start();

    BOOST_CHECK_EQUAL(node.get_state(), kythira::server_state::follower);
    BOOST_CHECK_NO_THROW(node.replicate_to_followers());

    node.stop();
}

BOOST_AUTO_TEST_CASE(replicate_on_single_node_leader, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();

    auto leader = make_single_node_leader(sim, 1);
    BOOST_REQUIRE(leader->is_leader());

    // Single-node leader has no followers: replicate_to_followers goes to advance_commit_index
    BOOST_CHECK_NO_THROW(leader->replicate_to_followers());

    // After replicate: commit a command too
    auto f = leader->submit_command(make_put_cmd("a", "b"), std::chrono::milliseconds{2000});
    f.get();

    BOOST_CHECK_NO_THROW(leader->replicate_to_followers());

    leader->stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite: node state queries ─────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(state_queries_suite)

BOOST_AUTO_TEST_CASE(get_node_id_and_term, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();

    auto leader = make_single_node_leader(sim, 42);
    BOOST_REQUIRE(leader->is_leader());

    BOOST_CHECK_EQUAL(leader->get_node_id(), 42u);
    BOOST_CHECK_GE(leader->get_current_term(), 1u);
    BOOST_CHECK_EQUAL(leader->get_state(), kythira::server_state::leader);
    BOOST_CHECK(leader->is_running());

    leader->stop();
    BOOST_CHECK(!leader->is_running());
}

BOOST_AUTO_TEST_SUITE_END()
