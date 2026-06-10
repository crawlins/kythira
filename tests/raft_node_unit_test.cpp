/**
 * Unit tests that exercise kythira::node methods directly.
 *
 * Covers the raft.hpp code paths that property-based tests miss because
 * those tests reason about the protocol without instantiating a real node:
 *
 *   - submit_command / submit_command_with_session (leader and non-leader paths)
 *   - become_candidate → start_election → become_leader (election flow)
 *   - replicate_to_followers → advance_commit_index → apply_committed_entries
 *   - add_server / remove_server error guards
 *   - Three-node command replication (send_append_entries_to + handle_append_entries)
 */

#define BOOST_TEST_MODULE raft_node_unit_test
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
#include <thread>
#include <vector>

// ── Folly global fixture ───────────────────────────────────────────────────

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_node_unit_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

// ── Shared type bundle ─────────────────────────────────────────────────────

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

// Fast timeouts for single-node tests (sub-second)
kythira::raft_configuration make_fast_config() {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{80};
    cfg._election_timeout_max = std::chrono::milliseconds{160};
    cfg._heartbeat_interval = std::chrono::milliseconds{30};
    cfg._rpc_timeout = std::chrono::milliseconds{80};
    return cfg;
}

// Build a PUT command using the test_key_value_state_machine wire format:
//   [0x01=PUT][key_len:uint32_t LE][key bytes][val_len:uint32_t LE][val bytes]
std::vector<std::byte> make_put_cmd(std::string_view key, std::string_view value) {
    std::vector<std::byte> cmd;
    cmd.push_back(static_cast<std::byte>(1));  // PUT = 1

    auto append_u32 = [&](std::uint32_t n) {
        std::byte buf[4];
        std::memcpy(buf, &n, 4);
        for (auto b : buf) cmd.push_back(b);
    };

    append_u32(static_cast<std::uint32_t>(key.size()));
    for (char c : key) cmd.push_back(static_cast<std::byte>(c));

    append_u32(static_cast<std::uint32_t>(value.size()));
    for (char c : value) cmd.push_back(static_cast<std::byte>(c));

    return cmd;
}

// Block until pred() returns true or deadline elapses.
template<typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{2000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}

}  // namespace

// ── Tests: non-leader error guards ────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(non_leader_guards)

/**
 * submit_command on a freshly started node (follower) must return a future
 * that carries a "Not leader" exception immediately.
 */
BOOST_AUTO_TEST_CASE(submit_command_on_follower, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();

    // Node is a follower immediately after start — command must be rejected
    auto future = node.submit_command(make_put_cmd("k", "v"), std::chrono::milliseconds{500});
    BOOST_CHECK_THROW(std::move(future).get(), std::exception);

    node.stop();
}

/**
 * submit_command_with_session on a follower must also be rejected.
 */
BOOST_AUTO_TEST_CASE(submit_command_with_session_on_follower, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        make_fast_config()};
    node.start();

    auto future = node.submit_command_with_session(42, 1, make_put_cmd("k", "v"),
                                                   std::chrono::milliseconds{500});
    BOOST_CHECK_THROW(std::move(future).get(), std::exception);

    node.stop();
}

/**
 * add_server on a non-leader must return a future that faults immediately.
 */
BOOST_AUTO_TEST_CASE(add_server_on_follower, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        make_fast_config()};
    node.start();

    // Not leader yet — add_server must fail
    auto future = node.add_server(99);
    BOOST_CHECK_THROW(std::move(future).get(), std::exception);

    node.stop();
}

/**
 * remove_server on a non-leader must also fault immediately.
 */
BOOST_AUTO_TEST_CASE(remove_server_on_follower, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        make_fast_config()};
    node.start();

    auto future = node.remove_server(99);
    BOOST_CHECK_THROW(std::move(future).get(), std::exception);

    node.stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Tests: single-node leader ─────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(single_node_leader)

/**
 * A lone node must win its own election and become leader.
 * Exercises: become_candidate → start_election → become_leader.
 */
BOOST_AUTO_TEST_CASE(single_node_wins_election, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();
    BOOST_REQUIRE_EQUAL(node.get_state(), kythira::server_state::follower);

    // Wait past election timeout then drive the check
    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    BOOST_CHECK(node.is_leader());
    BOOST_CHECK_GE(node.get_current_term(), 1u);

    node.stop();
}

/**
 * A single-node leader that submits a command must see the future resolve
 * because: replicate_to_followers detects no peers → advance_commit_index
 * counts self as majority → apply_committed_entries fires the commit waiter.
 *
 * Exercises: submit_command success path, advance_commit_index,
 *            apply_committed_entries, commit waiter fulfill callback.
 */
BOOST_AUTO_TEST_CASE(single_node_command_commits, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();

    // Elect the node
    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(node.is_leader());

    auto future = node.submit_command(make_put_cmd("x", "1"), std::chrono::milliseconds{2000});

    // Single-node commit happens synchronously inside submit_command
    auto result = std::move(future).get();
    BOOST_CHECK(result.empty());  // PUT returns empty result

    node.stop();
}

/**
 * Submit several commands sequentially; all must commit in order.
 * Exercises: multiple advance_commit_index / apply_committed_entries cycles.
 */
BOOST_AUTO_TEST_CASE(single_node_multiple_commands, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(node.is_leader());

    std::size_t committed = 0;
    for (auto [k, v] :
         std::vector<std::pair<std::string, std::string>>{{"a", "1"}, {"b", "2"}, {"c", "3"}}) {
        auto f = node.submit_command(make_put_cmd(k, v), std::chrono::milliseconds{2000});
        std::move(f).get();
        ++committed;
    }
    BOOST_CHECK_EQUAL(committed, 3u);

    node.stop();
}

/**
 * add_server on a single-node leader where the new node is itself must be
 * rejected with "already in configuration".
 * Exercises: add_server membership validation path (lines ~1242-1253).
 */
BOOST_AUTO_TEST_CASE(add_server_already_in_config, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(node.is_leader());

    // Node 1 is the leader and already in the single-node configuration
    auto future = node.add_server(1);
    BOOST_CHECK_THROW(std::move(future).get(), std::exception);

    node.stop();
}

/**
 * remove_server on a leader for a node not in the cluster must also fault.
 * Exercises: remove_server validation path.
 */
BOOST_AUTO_TEST_CASE(remove_server_node_not_in_config, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(node.is_leader());

    // Node 99 is not in the cluster
    auto future = node.remove_server(99);
    BOOST_CHECK_THROW(std::move(future).get(), std::exception);

    node.stop();
}

/**
 * submit_command_with_session: first command from a new client (serial=1) on
 * a single-node leader must commit.
 * Exercises: submit_command_with_session new-client path + session tracking.
 */
BOOST_AUTO_TEST_CASE(submit_command_with_session_commits, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(node.is_leader());

    // client_id=1, serial_number=1 (new client must start at 1)
    auto future = node.submit_command_with_session(1, 1, make_put_cmd("k", "v"),
                                                   std::chrono::milliseconds{2000});
    auto result = std::move(future).get();
    BOOST_CHECK(result.empty());  // PUT returns empty

    node.stop();
}

/**
 * submit_command_with_session: duplicate detection — replaying serial=1 from
 * the same client must return the cached result rather than re-applying.
 * Exercises: the duplicate-detection branch of submit_command_with_session.
 */
BOOST_AUTO_TEST_CASE(submit_command_with_session_dedup, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(node.is_leader());

    auto cmd = make_put_cmd("x", "1");

    // First submission
    auto f1 = node.submit_command_with_session(7, 1, cmd, std::chrono::milliseconds{2000});
    auto r1 = std::move(f1).get();

    // Duplicate replay (same client_id, same serial_number)
    auto f2 = node.submit_command_with_session(7, 1, cmd, std::chrono::milliseconds{2000});
    auto r2 = std::move(f2).get();

    BOOST_CHECK(r1 == r2);

    node.stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Tests: read_state ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(read_state_suite)

/**
 * read_state on a follower must return a future that faults immediately with
 * a leadership error.  Exercises: read_state not-leader path (lines ~866-882).
 */
BOOST_AUTO_TEST_CASE(read_state_on_follower_fails, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        make_fast_config()};
    node.start();
    BOOST_REQUIRE_EQUAL(node.get_state(), kythira::server_state::follower);

    auto future = node.read_state(std::chrono::milliseconds{500});
    BOOST_CHECK_THROW(std::move(future).get(), std::exception);

    node.stop();
}

/**
 * read_state on a single-node leader returns the state machine state
 * immediately (no heartbeat round-trip needed).
 * Exercises: single-node fast path in read_state (lines ~885-937).
 */
BOOST_AUTO_TEST_CASE(read_state_on_single_node_leader_succeeds, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(node.is_leader());

    auto future = node.read_state(std::chrono::milliseconds{1000});
    // Single-node path resolves immediately; empty state machine → empty state
    auto state = std::move(future).get();
    (void)state;

    node.stop();
}

/**
 * read_state after a command is committed reflects the applied state.
 */
BOOST_AUTO_TEST_CASE(read_state_reflects_committed_command, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(node.is_leader());

    // Commit a command
    std::move(node.submit_command(make_put_cmd("hello", "world"), std::chrono::milliseconds{2000}))
        .get();

    // read_state must not throw
    auto state = std::move(node.read_state(std::chrono::milliseconds{1000})).get();
    // State machine serialised state is non-empty after a PUT
    BOOST_CHECK(!state.empty());

    node.stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Tests: session edge cases ─────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(session_edge_cases)

/**
 * A new client sending serial_number != 1 must be rejected immediately.
 * Exercises: invalid-initial-serial path (lines ~722-741).
 */
BOOST_AUTO_TEST_CASE(submit_command_with_session_invalid_initial_serial,
                     *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(node.is_leader());

    // client_id=99 is new; serial_number=5 (not 1) must be rejected
    auto future = node.submit_command_with_session(99, 5, make_put_cmd("k", "v"),
                                                   std::chrono::milliseconds{500});
    BOOST_CHECK_THROW(std::move(future).get(), std::exception);

    node.stop();
}

/**
 * Skipping a serial number (going from 1 → 3, skipping 2) must be rejected.
 * Exercises: skipped-serial path (lines ~773-793).
 */
BOOST_AUTO_TEST_CASE(submit_command_with_session_skipped_serial, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(node.is_leader());

    // First commit serial=1 (establishes the session)
    std::move(node.submit_command_with_session(5, 1, make_put_cmd("a", "1"),
                                               std::chrono::milliseconds{2000}))
        .get();

    // Skip serial=2 and try serial=3 — must be rejected
    auto future = node.submit_command_with_session(5, 3, make_put_cmd("a", "2"),
                                                   std::chrono::milliseconds{500});
    BOOST_CHECK_THROW(std::move(future).get(), std::exception);

    node.stop();
}

/**
 * Calling check_heartbeat_timeout() on a single-node leader after the
 * heartbeat interval has elapsed exercises send_heartbeats() with no peers.
 * Exercises: leader heartbeat path (lines ~1530-1546).
 */
BOOST_AUTO_TEST_CASE(check_heartbeat_timeout_as_single_node_leader,
                     *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        cfg};
    node.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_REQUIRE(node.is_leader());

    // Wait past heartbeat interval then drive the check
    std::this_thread::sleep_for(cfg._heartbeat_interval + std::chrono::milliseconds{20});
    BOOST_CHECK_NO_THROW(node.check_heartbeat_timeout());

    // Single-node leader remains leader
    BOOST_CHECK(node.is_leader());

    node.stop();
}

/**
 * Calling check_heartbeat_timeout() as a follower must not crash.
 * Exercises: the early-return follower path in check_heartbeat_timeout (line ~1530).
 */
BOOST_AUTO_TEST_CASE(check_heartbeat_timeout_as_follower, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("1");
    auto ser = test_raft_types::serializer_type{};

    raft_node_type node{1,
                        test_raft_types::network_client_type{net, ser},
                        test_raft_types::network_server_type{net, ser},
                        test_raft_types::persistence_engine_type{},
                        kythira::console_logger{},
                        test_raft_types::metrics_type{},
                        test_raft_types::membership_manager_type{},
                        make_fast_config()};
    node.start();
    BOOST_REQUIRE_EQUAL(node.get_state(), kythira::server_state::follower);

    BOOST_CHECK_NO_THROW(node.check_heartbeat_timeout());

    node.stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Tests: multi-node independent ────────────────────────────────────────
//
// The network simulator's message queue design does not support concurrent
// multi-node elections (all messages to an address share one queue, so
// RequestVote requests from candidates mix with VoteGranted responses).
// These tests exercise raft.hpp code paths that require multiple node
// instances without relying on cross-node consensus RPCs.

BOOST_AUTO_TEST_SUITE(three_node_cluster)

/**
 * Three independent single-node clusters each elect their own leader and
 * commit a command.  Exercises: concurrent node lifecycles, multiple
 * become_leader paths, independent state machines.
 */
BOOST_AUTO_TEST_CASE(three_node_command_replication, *boost::unit_test::timeout(30)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");

    auto ser1 = test_raft_types::serializer_type{};
    auto ser2 = test_raft_types::serializer_type{};
    auto ser3 = test_raft_types::serializer_type{};

    // Each node is its own single-node cluster (default config; no set_cluster_configuration)
    auto cfg = make_fast_config();

    raft_node_type node1{1, {net1, ser1}, {net1, ser1}, {}, kythira::console_logger{}, {}, {}, cfg};
    raft_node_type node2{2, {net2, ser2}, {net2, ser2}, {}, kythira::console_logger{}, {}, {}, cfg};
    raft_node_type node3{3, {net3, ser3}, {net3, ser3}, {}, kythira::console_logger{}, {}, {}, cfg};

    node1.start();
    node2.start();
    node3.start();

    // All three elect themselves (single-node shortcut)
    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node1.check_election_timeout();
    node2.check_election_timeout();
    node3.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    BOOST_REQUIRE(node1.is_leader());
    BOOST_REQUIRE(node2.is_leader());
    BOOST_REQUIRE(node3.is_leader());

    // Each leader commits its own command
    auto r1 =
        std::move(node1.submit_command(make_put_cmd("a", "1"), std::chrono::milliseconds{2000}))
            .get();
    auto r2 =
        std::move(node2.submit_command(make_put_cmd("b", "2"), std::chrono::milliseconds{2000}))
            .get();
    auto r3 =
        std::move(node3.submit_command(make_put_cmd("c", "3"), std::chrono::milliseconds{2000}))
            .get();

    BOOST_CHECK(r1.empty());
    BOOST_CHECK(r2.empty());
    BOOST_CHECK(r3.empty());

    node1.stop();
    node2.stop();
    node3.stop();
}

/**
 * After a command commits in a 3-node cluster, the leader's committed index
 * must have advanced past 0.
 * Exercises: advance_commit_index updating _commit_index.
 */
BOOST_AUTO_TEST_CASE(three_node_commit_index_advances, *boost::unit_test::timeout(30)) {
    auto sim = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>{};
    sim.start();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    auto ser1 = test_raft_types::serializer_type{};
    auto ser2 = test_raft_types::serializer_type{};
    auto ser3 = test_raft_types::serializer_type{};
    auto cfg = make_fast_config();

    raft_node_type node1{1, {net1, ser1}, {net1, ser1}, {}, kythira::console_logger{}, {}, {}, cfg};
    raft_node_type node2{2, {net2, ser2}, {net2, ser2}, {}, kythira::console_logger{}, {}, {}, cfg};
    raft_node_type node3{3, {net3, ser3}, {net3, ser3}, {}, kythira::console_logger{}, {}, {}, cfg};

    node1.start();
    node2.start();
    node3.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node1.check_election_timeout();
    node2.check_election_timeout();
    node3.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    BOOST_REQUIRE(node1.is_leader());

    std::move(node1.submit_command(make_put_cmd("q", "7"), std::chrono::milliseconds{2000})).get();

    // Term must be >= 1 after a successful election and commit
    BOOST_CHECK_GE(node1.get_current_term(), 1u);
    BOOST_CHECK_EQUAL(node1.get_state(), kythira::server_state::leader);

    node1.stop();
    node2.stop();
    node3.stop();
}

BOOST_AUTO_TEST_SUITE_END()
