// **Feature: membership-change, Requirement 8**
// `initialize_from_storage()` recovery scenarios: a node reloads its durable
// state (term, log, snapshot, and — since a snapshot or the log may carry a
// `entry_type::configuration` entry — its cluster configuration) from the
// persistence engine on start(), rather than beginning as a blank slate.
//
// `install_snapshot()`'s own one-line `_configuration = snap.configuration()`
// fix (Task 17) shares the exact restoration logic exercised here via the
// restart path and is additionally exercised end-to-end (live leader ->
// lagging-follower InstallSnapshot RPC) by
// tests/raft_snapshot_preserves_state_property_test.cpp; it is not
// re-tested via a live RPC exchange in this file.
#define BOOST_TEST_MODULE node_recovery_unit_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 30

#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <memory>
#include <vector>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("node_recovery_unit_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {

// Same shape as raft_batch_entry_application_property_test.cpp's
// test_raft_types — plain (non-bootstrap-extended) raft_types, single-node
// simulator network. No peers are ever connected in this file: every test
// here exercises initialize_from_storage() itself, not replication.
struct test_raft_types {
    using future_type = kythira::Future<std::vector<std::byte>>;
    using promise_type = kythira::Promise<std::vector<std::byte>>;
    using try_type = kythira::Try<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type =
        kythira::simulator_network_client<kythira::raft_simulator_network_types<node_id_type>,
                                          serializer_type, serialized_data_type>;
    using network_server_type =
        kythira::simulator_network_server<kythira::raft_simulator_network_types<node_id_type>,
                                          serializer_type, serialized_data_type>;
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

using node_type = kythira::node<test_raft_types>;
using persistence_t = test_raft_types::persistence_engine_type;
using raft_network_types = kythira::raft_simulator_network_types<test_raft_types::node_id_type>;

auto make_fast_config() -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{80};
    cfg._election_timeout_max = std::chrono::milliseconds{160};
    cfg._heartbeat_interval = std::chrono::milliseconds{26};
    cfg._rpc_timeout = std::chrono::milliseconds{80};
    return cfg;
}

// Constructs a fresh node_id=1 around `persistence` (already pre-populated
// to simulate "what a real disk would contain after a prior process's
// crash"), sets the boot configuration to the single-node {1} (overwritten
// by start()'s initialize_from_storage() if a snapshot/log config entry
// says otherwise), and calls start(). Callers MUST call node->stop() before
// the returned unique_ptr is destroyed — node<Types> has no destructor that
// joins its background threads, so destroying a still-running node aborts.
//
// `simulator` is a caller-owned out-parameter, not constructed locally: the
// node's network_client/server hold non-owning handles into it, so it must
// outlive the returned node — same lifetime requirement as every simulator
// used by network_simulator::NetworkSimulator elsewhere in this test suite.
auto start_recovered_node(network_simulator::NetworkSimulator<raft_network_types>& simulator,
                          persistence_t persistence,
                          kythira::raft_configuration cfg = make_fast_config())
    -> std::unique_ptr<node_type> {
    simulator.start();
    auto sim_node = simulator.create_node(1);

    auto node = std::make_unique<node_type>(
        std::uint64_t{1},
        test_raft_types::network_client_type{sim_node, test_raft_types::serializer_type{}},
        test_raft_types::network_server_type{sim_node, test_raft_types::serializer_type{}},
        std::move(persistence), test_raft_types::logger_type{kythira::log_level::error},
        test_raft_types::metrics_type{}, test_raft_types::membership_manager_type{}, cfg);

    node->set_cluster_configuration({1});
    node->start();
    return node;
}

auto make_config_entry(std::uint64_t term, std::uint64_t index,
                       const std::vector<std::uint64_t>& nodes) -> test_raft_types::log_entry_type {
    kythira::cluster_configuration<std::uint64_t> config{nodes, false, std::nullopt};
    auto payload = kythira::serialize_configuration<std::uint64_t>(config);
    return test_raft_types::log_entry_type{term, index, payload,
                                           kythira::entry_type::configuration};
}

auto make_normal_entry(std::uint64_t term, std::uint64_t index) -> test_raft_types::log_entry_type {
    return test_raft_types::log_entry_type{term, index, std::vector<std::byte>{std::byte{0x01}},
                                           kythira::entry_type::normal};
}

}  // namespace

BOOST_AUTO_TEST_SUITE(node_recovery_tests)

// No persisted state: a wholly fresh node boots at term 0 with an empty log,
// and — since neither a snapshot nor a log configuration entry exists to
// override it — the boot configuration set before start() is preserved.
BOOST_AUTO_TEST_CASE(no_persisted_state_boots_with_boot_configuration,
                     *boost::unit_test::timeout(10)) {
    network_simulator::NetworkSimulator<raft_network_types> sim;
    auto node = start_recovered_node(sim, persistence_t{});

    auto snap = node->debug_state();
    BOOST_CHECK_EQUAL(snap.current_term, 0u);
    BOOST_CHECK_EQUAL(snap.commit_index, 0u);
    BOOST_CHECK_EQUAL(snap.last_applied, 0u);
    BOOST_CHECK(snap.log.empty());
    BOOST_CHECK_EQUAL(node->get_cluster_size(), 1u);
    node->stop();
}

// Term and voted_for only (no log, no snapshot): the term is restored; the
// log remains empty. (voted_for is internal safety state with no public
// accessor — its effect on vote-granting is exercised by the project's
// existing election-safety test suite, not re-verified here.)
BOOST_AUTO_TEST_CASE(term_and_voted_for_only_restores_term_with_empty_log,
                     *boost::unit_test::timeout(10)) {
    persistence_t persistence;
    persistence.save_current_term(std::uint64_t{7});
    persistence.save_voted_for(std::uint64_t{1});

    network_simulator::NetworkSimulator<raft_network_types> sim;
    auto node = start_recovered_node(sim, std::move(persistence));

    auto snap = node->debug_state();
    BOOST_CHECK_EQUAL(snap.current_term, 7u);
    BOOST_CHECK(snap.log.empty());
    BOOST_CHECK_EQUAL(snap.last_applied, 0u);
    node->stop();
}

// Snapshot only: state-machine indices and configuration are all restored
// from the snapshot, overriding the {1}-only boot configuration set before
// start().
BOOST_AUTO_TEST_CASE(snapshot_only_restores_indices_and_configuration,
                     *boost::unit_test::timeout(10)) {
    persistence_t persistence;

    test_raft_types::state_machine_type sm;
    kythira::snapshot<std::uint64_t, std::uint64_t, std::uint64_t> snap;
    snap._last_included_index = 10;
    snap._last_included_term = 3;
    snap._configuration =
        kythira::cluster_configuration<std::uint64_t>{{1, 2, 3}, false, std::nullopt};
    snap._state_machine_state = sm.get_state();
    persistence.save_snapshot(snap);

    network_simulator::NetworkSimulator<raft_network_types> sim;
    auto node = start_recovered_node(sim, std::move(persistence));

    auto debug = node->debug_state();
    BOOST_CHECK_EQUAL(debug.last_applied, 10u);
    BOOST_CHECK_EQUAL(debug.commit_index, 10u);
    BOOST_CHECK(debug.log.empty());
    BOOST_CHECK_EQUAL(node->get_cluster_size(), 3u);
    node->stop();
}

// Snapshot + trailing log entries: entries persisted after the snapshot's
// last_included_index are reloaded into the in-memory log; with no
// configuration entry among them, the snapshot's configuration still wins.
BOOST_AUTO_TEST_CASE(snapshot_plus_trailing_log_entries_are_reloaded,
                     *boost::unit_test::timeout(10)) {
    persistence_t persistence;

    test_raft_types::state_machine_type sm;
    kythira::snapshot<std::uint64_t, std::uint64_t, std::uint64_t> snap;
    snap._last_included_index = 10;
    snap._last_included_term = 3;
    snap._configuration =
        kythira::cluster_configuration<std::uint64_t>{{1, 2, 3}, false, std::nullopt};
    snap._state_machine_state = sm.get_state();
    persistence.save_snapshot(snap);

    for (std::uint64_t idx = 11; idx <= 15; ++idx) {
        persistence.append_log_entry(make_normal_entry(3, idx));
    }

    network_simulator::NetworkSimulator<raft_network_types> sim;
    auto node = start_recovered_node(sim, std::move(persistence));

    auto debug = node->debug_state();
    BOOST_CHECK_EQUAL(debug.log.size(), 5u);
    BOOST_CHECK_EQUAL(debug.last_applied, 10u);  // reload populates _log; it does not itself apply
    BOOST_CHECK_EQUAL(node->get_cluster_size(), 3u);
    node->stop();
}

// The core gap this file closes (Requirement 8.3): a configuration entry in
// the trailing log is more recent than the snapshot's own configuration and
// SHALL override it.
BOOST_AUTO_TEST_CASE(configuration_entry_in_trailing_log_overrides_snapshot_configuration,
                     *boost::unit_test::timeout(10)) {
    persistence_t persistence;

    test_raft_types::state_machine_type sm;
    kythira::snapshot<std::uint64_t, std::uint64_t, std::uint64_t> snap;
    snap._last_included_index = 10;
    snap._last_included_term = 3;
    snap._configuration =
        kythira::cluster_configuration<std::uint64_t>{{1, 2, 3}, false, std::nullopt};
    snap._state_machine_state = sm.get_state();
    persistence.save_snapshot(snap);

    persistence.append_log_entry(make_normal_entry(3, 11));
    persistence.append_log_entry(make_normal_entry(3, 12));
    // A membership change committed after the snapshot was taken: node 4
    // joined. This is the most recent configuration entry in the log and
    // SHALL win over the snapshot's 3-node configuration.
    persistence.append_log_entry(make_config_entry(3, 13, {1, 2, 3, 4}));
    persistence.append_log_entry(make_normal_entry(3, 14));

    network_simulator::NetworkSimulator<raft_network_types> sim;
    auto node = start_recovered_node(sim, std::move(persistence));

    auto debug = node->debug_state();
    BOOST_CHECK_EQUAL(debug.log.size(), 4u);
    BOOST_CHECK_EQUAL(node->get_cluster_size(), 4u);
    node->stop();
}

// With multiple configuration entries in the trailing log, the highest-
// indexed one wins — not merely "the last one found while scanning
// forward," which would be the opposite (and wrong) answer.
BOOST_AUTO_TEST_CASE(highest_indexed_configuration_entry_wins_among_several,
                     *boost::unit_test::timeout(10)) {
    persistence_t persistence;

    test_raft_types::state_machine_type sm;
    kythira::snapshot<std::uint64_t, std::uint64_t, std::uint64_t> snap;
    snap._last_included_index = 10;
    snap._last_included_term = 3;
    snap._configuration =
        kythira::cluster_configuration<std::uint64_t>{{1, 2, 3}, false, std::nullopt};
    snap._state_machine_state = sm.get_state();
    persistence.save_snapshot(snap);

    persistence.append_log_entry(make_config_entry(3, 11, {1, 2, 3, 4}));  // node 4 joins
    persistence.append_log_entry(make_normal_entry(3, 12));
    persistence.append_log_entry(make_config_entry(3, 13, {1, 2, 4}));  // node 3 later removed

    network_simulator::NetworkSimulator<raft_network_types> sim;
    auto node = start_recovered_node(sim, std::move(persistence));

    BOOST_CHECK_EQUAL(node->get_cluster_size(), 3u);
    node->stop();
}

BOOST_AUTO_TEST_SUITE_END()
