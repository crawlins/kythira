#define BOOST_TEST_MODULE membership_change_leader_crash_property_test
#include <boost/test/unit_test.hpp>

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
        char* argv0[] = {const_cast<char*>("membership_change_leader_crash_property_test"),
                         nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

// ── Shared types ───────────────────────────────────────────────────────────

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

}  // namespace

// ── Property tests ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(leader_crash_properties)

/**
 * Property: if the initial leader crashes while a joint-config entry is
 * in-flight (i.e. the joint entry has been appended to the leader's log but
 * NOT yet committed), a new leader elected from the surviving quorum can still
 * make forward progress and the cluster reaches C_new without data loss.
 *
 * The joint-config entry that was appended by L1 will be replicated to at
 * least one follower (node2 or node3).  If it is replicated to a node that
 * wins the next election, that new leader re-drives the protocol:
 *   1. Commits the joint entry, then appends C_new
 *   2. Commits C_new — cluster now equals {1,2,3,4} (or stays {1,2,3} if the
 *      joint entry was NOT replicated before the crash, which is also valid)
 *
 * Either way the remaining cluster must be able to commit a normal command
 * after node1 crashes — this verifies liveness.
 */
BOOST_AUTO_TEST_CASE(cluster_survives_leader_crash_during_add, *boost::unit_test::timeout(30)) {
    for (int iteration = 0; iteration < 3; ++iteration) {
        sim_t sim;
        sim.start();

        auto net1 = sim.create_node("1");
        auto net2 = sim.create_node("2");
        auto net3 = sim.create_node("3");
        auto net4 = sim.create_node("4");
        connect_all(sim, {"1", "2", "3", "4"});

        auto cfg = make_fast_config();
        auto make_node = [&](std::uint64_t id, auto net) {
            return test_node{id,
                             {net, test_types::serializer_type{}},
                             {net, test_types::serializer_type{}},
                             {},
                             kythira::console_logger{},
                             {},
                             {},
                             cfg,
                             std::to_string(id),
                             preset_peer_discovery<std::uint64_t, std::string>{}};
        };

        auto node1 = make_node(1, net1);
        auto node2 = make_node(2, net2);
        auto node3 = make_node(3, net3);
        auto node4 = make_node(4, net4);

        node1.set_cluster_configuration({1, 2, 3});
        node2.set_cluster_configuration({1, 2, 3});
        node3.set_cluster_configuration({1, 2, 3});
        node4.set_cluster_configuration({4});

        node1.start();
        node2.start();
        node3.start();
        node4.start();

        std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
        node1.check_election_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{200});

        BOOST_REQUIRE_MESSAGE(
            wait_until([&] { return node1.is_leader(); }, std::chrono::milliseconds{4000}),
            "iteration " + std::to_string(iteration) + ": node1 did not become leader");

        // Trigger add_server — this appends the joint config entry to node1's log
        // and begins replication.  We do NOT wait for it to complete.
        auto add_fut = node1.add_server(4);
        // One heartbeat so the joint entry reaches some followers
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{40});

        // "Crash" the leader: stop it while the joint entry may be in-flight.
        // Also remove node1 from the simulator topology so that subsequent RPCs
        // to node1 fail immediately (route_message returns false) instead of
        // queueing a message that nobody reads and blocking the caller for rpc_timeout
        // (200ms) on every send_request_vote / send_append_entries_to call.
        node1.stop();
        sim.remove_node("1");

        // Drive elections.  The simulator requires manual calls to check_election_timeout()
        // since there is no background timer thread.
        //
        // In this test node2 ALWAYS has the most up-to-date log (it receives both the joint
        // config entry and the C_new entry from L1 in one AppendEntries batch), so only node2
        // can win the election under joint-consensus quorum rules.  Driving node3 or node4
        // simultaneously causes split votes — they vote for themselves, blocking node2 from
        // winning C_old quorum — so we drive only node2 here.
        //
        // The vote to crashed node1 is async (within(election_timeout) fires after ~80-160ms),
        // so we sleep 250ms after each drive call to let the callback run and become_leader().
        // Give node2 enough time for its election_timeout to expire before driving it.
        std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});

        BOOST_CHECK_MESSAGE(
            wait_until(
                [&] {
                    node2.check_election_timeout();
                    return node2.is_leader() || node3.is_leader();
                },
                std::chrono::milliseconds{4000}),
            "iteration " + std::to_string(iteration) + ": no new leader elected after L1 crash");

        // node2 is always the new leader (it has the most up-to-date log)
        test_node* new_leader = node2.is_leader() ? &node2 : &node3;

        // Drive the new leader to make progress
        for (int i = 0; i < 20; ++i) {
            new_leader->check_heartbeat_timeout();
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
        }

        // Property: a normal command commits successfully on the surviving cluster
        using sm_t = kythira::test_key_value_state_machine<test_types::log_index_type>;
        auto cmd = sm_t::make_put_command("k", "v");
        auto cmd_fut = new_leader->submit_command(cmd, std::chrono::milliseconds{3000});

        bool cmd_applied = false;
        std::move(cmd_fut)
            .thenValue([&](std::vector<std::byte>) { cmd_applied = true; })
            .thenError([&](const std::exception_ptr&) {});

        for (int i = 0; i < 15; ++i) {
            new_leader->check_heartbeat_timeout();
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
        }

        BOOST_CHECK_MESSAGE(
            wait_until([&] { return cmd_applied; }, std::chrono::milliseconds{4000}),
            "iteration " + std::to_string(iteration) +
                ": surviving cluster could not commit a command after leader crash");

        // add_fut from the crashed leader is now orphaned — just discard it
        (void)std::move(add_fut);

        node4.stop();
        node3.stop();
        node2.stop();

        // execute_with_retry chains for crashed node1 run on Folly's thread pool and hold
        // raw `this` pointers into these node objects.  Each chain may schedule up to 4
        // retries with cumulative delays of 100+200+400+800 = 1 500 ms.  We must stay in
        // this scope (keeping the nodes alive) until all chains have finished, otherwise
        // the next iteration's stack-allocated nodes land at the same addresses and the
        // dangling callbacks corrupt their state.
        std::this_thread::sleep_for(std::chrono::milliseconds{2500});
    }
}

BOOST_AUTO_TEST_SUITE_END()
