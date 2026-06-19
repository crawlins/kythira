#define BOOST_TEST_MODULE bootstrap_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/peer_discovery.hpp>
#include <raft/types.hpp>
#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/json_serializer.hpp>
#include <raft/persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/membership.hpp>
#include <raft/test_state_machine.hpp>
#include <network_simulator/network_simulator.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>

// ── Folly global fixture ───────────────────────────────────────────────────

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("bootstrap_unit_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

// ── Shared types ───────────────────────────────────────────────────────────

namespace {

// A simple peer-discovery whose peer list can be configured at construction.
// Default-constructible (empty list → no peers returned).
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

    auto find_peers(std::chrono::milliseconds) const
        -> kythira::Future<std::vector<kythira::peer_info<NodeId, Address>>> {
        return kythira::FutureFactory::makeFuture(
            std::vector<kythira::peer_info<NodeId, Address>>(_peers));
    }

private:
    std::vector<kythira::peer_info<NodeId, Address>> _peers;
};

static_assert(kythira::peer_discovery<preset_peer_discovery<std::uint64_t, std::string>,
                                      std::uint64_t, std::string>);

// Types used for bootstrap-aware node tests
struct bootstrap_raft_types {
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

    // Bootstrap extensions
    using address_type = std::string;
    using peer_discovery_type = preset_peer_discovery<node_id_type, address_type>;
    using cluster_join_request_type = kythira::cluster_join_request<node_id_type, address_type>;
    using cluster_join_response_type = kythira::cluster_join_response<node_id_type, address_type>;
};

using bootstrap_node_type = kythira::node<bootstrap_raft_types>;

kythira::raft_configuration make_fast_config() {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{80};
    cfg._election_timeout_max = std::chrono::milliseconds{160};
    cfg._heartbeat_interval = std::chrono::milliseconds{26};
    cfg._rpc_timeout = std::chrono::milliseconds{80};
    return cfg;
}

// Build a PUT command: [0x01][key_len:u32le][key][val_len:u32le][val]
std::vector<std::byte> make_put_cmd(std::string_view key, std::string_view value) {
    std::vector<std::byte> cmd;
    cmd.push_back(static_cast<std::byte>(1));
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

template<typename Pred>
bool wait_ready(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{2000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}

using bsim_t = network_simulator::NetworkSimulator<bootstrap_raft_types::raft_network_types>;

// Fully connect a set of simulator nodes (bidirectional, zero latency)
void bconnect_all(bsim_t& sim, std::initializer_list<std::string> addrs) {
    network_simulator::NetworkEdge edge{};
    for (const auto& from : addrs)
        for (const auto& to : addrs)
            if (from != to) sim.add_edge(from, to, edge);
}

// Construct a bootstrap node with empty peer discovery (founds single-node cluster)
bootstrap_node_type make_bootstrap_node(std::uint64_t id, auto net,
                                        kythira::raft_configuration cfg = make_fast_config()) {
    auto ser = bootstrap_raft_types::serializer_type{};
    return bootstrap_node_type{id,
                               bootstrap_raft_types::network_client_type{net, ser},
                               bootstrap_raft_types::network_server_type{net, ser},
                               bootstrap_raft_types::persistence_engine_type{},
                               kythira::console_logger{},
                               bootstrap_raft_types::metrics_type{},
                               bootstrap_raft_types::membership_manager_type{},
                               cfg,
                               std::to_string(id),
                               preset_peer_discovery<std::uint64_t, std::string>{}};
}

// Elect a single-node cluster: sleep past election timeout then drive the check.
void elect_single_node(bootstrap_node_type& node, kythira::raft_configuration const& cfg) {
    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
}

// Fast config with a short bootstrap retry interval for multi-node cluster tests.
kythira::raft_configuration make_cluster_config() {
    auto cfg = make_fast_config();
    cfg._bootstrap_retry_interval = std::chrono::milliseconds{200};
    return cfg;
}

// Create a node that joins an existing cluster.
bootstrap_node_type make_joining_node(std::uint64_t id, auto net, std::uint64_t leader_id,
                                      std::string leader_addr,
                                      kythira::raft_configuration cfg = make_cluster_config()) {
    auto ser = bootstrap_raft_types::serializer_type{};
    return bootstrap_node_type{
        id,
        bootstrap_raft_types::network_client_type{net, ser},
        bootstrap_raft_types::network_server_type{net, ser},
        bootstrap_raft_types::persistence_engine_type{},
        kythira::console_logger{},
        bootstrap_raft_types::metrics_type{},
        bootstrap_raft_types::membership_manager_type{},
        cfg,
        std::to_string(id),
        preset_peer_discovery<std::uint64_t, std::string>(
            std::vector<kythira::peer_info<std::uint64_t, std::string>>{{leader_id, leader_addr}})};
}

}  // namespace

// ── Suite 1: peer_discovery concept and implementations ───────────────────

BOOST_AUTO_TEST_SUITE(peer_discovery_implementations)

BOOST_AUTO_TEST_CASE(no_op_concept_satisfied) {
    static_assert(kythira::peer_discovery<kythira::no_op_peer_discovery<std::uint64_t, std::string>,
                                          std::uint64_t, std::string>);
    BOOST_TEST(true);  // concept satisfaction confirmed at compile time
}

BOOST_AUTO_TEST_CASE(static_concept_satisfied) {
    static_assert(
        kythira::peer_discovery<kythira::static_peer_discovery<std::uint64_t, std::string>,
                                std::uint64_t, std::string>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(no_op_register_returns_ok) {
    kythira::no_op_peer_discovery<std::uint64_t, std::string> disc;
    // Should not throw
    BOOST_CHECK_NO_THROW(disc.register_node(1, "addr").get());
}

BOOST_AUTO_TEST_CASE(no_op_find_peers_returns_empty) {
    kythira::no_op_peer_discovery<std::uint64_t, std::string> disc;
    auto peers = disc.find_peers(std::chrono::milliseconds{100}).get();
    BOOST_TEST(peers.empty());
}

BOOST_AUTO_TEST_CASE(static_find_peers_returns_configured_list) {
    using pi = kythira::peer_info<std::uint64_t, std::string>;
    kythira::static_peer_discovery<std::uint64_t, std::string> disc{
        {pi{1, "host1"}, pi{2, "host2"}, pi{3, "host3"}}};

    auto peers = disc.find_peers(std::chrono::milliseconds{100}).get();
    BOOST_REQUIRE_EQUAL(peers.size(), 3u);
    BOOST_TEST(peers[0].node_id == 1u);
    BOOST_TEST(peers[1].node_id == 2u);
    BOOST_TEST(peers[2].node_id == 3u);
}

BOOST_AUTO_TEST_CASE(static_register_valid_self_id_succeeds) {
    using pi = kythira::peer_info<std::uint64_t, std::string>;
    kythira::static_peer_discovery<std::uint64_t, std::string> disc{{pi{1, "h1"}, pi{2, "h2"}}};
    BOOST_CHECK_NO_THROW(disc.register_node(1, "h1").get());
}

BOOST_AUTO_TEST_CASE(static_register_unknown_self_id_throws) {
    using pi = kythira::peer_info<std::uint64_t, std::string>;
    kythira::static_peer_discovery<std::uint64_t, std::string> disc{{pi{1, "h1"}}};
    BOOST_CHECK_THROW(disc.register_node(99, "h99").get(), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite 2: cluster_join serialization ───────────────────────────────────

BOOST_AUTO_TEST_SUITE(cluster_join_serialization)

BOOST_AUTO_TEST_CASE(request_round_trip) {
    kythira::json_rpc_serializer<std::vector<std::byte>> ser;
    kythira::cluster_join_request<> req;
    req.node_id = 42;
    req.contact_address = "192.168.1.5:7000";

    auto data = ser.serialize(req);
    auto decoded = ser.deserialize_cluster_join_request(data);

    BOOST_TEST(decoded.joining_node_id() == 42u);
    BOOST_TEST(decoded.joining_address() == "192.168.1.5:7000");
}

BOOST_AUTO_TEST_CASE(response_accepted_round_trip) {
    kythira::json_rpc_serializer<std::vector<std::byte>> ser;
    kythira::cluster_join_response<> resp;
    resp.accepted = true;

    auto data = ser.serialize(resp);
    auto decoded = ser.deserialize_cluster_join_response(data);

    BOOST_TEST(decoded.is_accepted());
    BOOST_TEST(!decoded.redirect_peer().has_value());
}

BOOST_AUTO_TEST_CASE(response_redirect_round_trip) {
    kythira::json_rpc_serializer<std::vector<std::byte>> ser;
    kythira::cluster_join_response<> resp;
    resp.accepted = false;
    resp.redirect = kythira::peer_info<std::uint64_t, std::string>{7, "10.0.0.7:8000"};

    auto data = ser.serialize(resp);
    auto decoded = ser.deserialize_cluster_join_response(data);

    BOOST_TEST(!decoded.is_accepted());
    BOOST_REQUIRE(decoded.redirect_peer().has_value());
    BOOST_TEST(decoded.redirect_peer()->node_id == 7u);
    BOOST_TEST(decoded.redirect_peer()->address == "10.0.0.7:8000");
}

BOOST_AUTO_TEST_CASE(wrong_type_tag_throws) {
    kythira::json_rpc_serializer<std::vector<std::byte>> ser;
    kythira::cluster_join_request<> req;
    req.node_id = 1;
    req.contact_address = "a";

    // Serialised as a request but decoded as a response — must throw
    auto data = ser.serialize(req);
    BOOST_CHECK_THROW(ser.deserialize_cluster_join_response(data),
                      kythira::serialization_exception);
}

BOOST_AUTO_TEST_CASE(cluster_join_request_accessors) {
    kythira::cluster_join_request<std::uint64_t, std::string> req;
    req.node_id = 5;
    req.contact_address = "node5:9000";
    BOOST_TEST(req.joining_node_id() == 5u);
    BOOST_TEST(req.joining_address() == "node5:9000");
}

BOOST_AUTO_TEST_CASE(cluster_join_response_accessors) {
    kythira::cluster_join_response<std::uint64_t, std::string> resp;
    resp.accepted = false;
    BOOST_TEST(!resp.is_accepted());
    BOOST_TEST(!resp.redirect_peer().has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite 3: bootstrap node lifecycle ─────────────────────────────────────

BOOST_AUTO_TEST_SUITE(bootstrap_node_lifecycle)

// Fresh node with no-op discovery completes start() as a single-node cluster.
BOOST_AUTO_TEST_CASE(fresh_node_with_no_op_starts_ok, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<bootstrap_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("node1");
    auto ser = bootstrap_raft_types::serializer_type{};

    bootstrap_node_type node{
        1,
        bootstrap_raft_types::network_client_type{net, ser},
        bootstrap_raft_types::network_server_type{net, ser},
        bootstrap_raft_types::persistence_engine_type{},
        kythira::console_logger{},
        bootstrap_raft_types::metrics_type{},
        bootstrap_raft_types::membership_manager_type{},
        make_fast_config(),
        "node1",
        preset_peer_discovery<std::uint64_t, std::string>{}  // empty → single-node
    };
    BOOST_CHECK_NO_THROW(node.start());
    node.stop();
}

// stop() called while run_bootstrap() is sleeping in its retry loop exits promptly.
BOOST_AUTO_TEST_CASE(stop_cancels_bootstrap_retry_loop, *boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<bootstrap_raft_types::raft_network_types>{};
    sim.start();
    auto net = sim.create_node("node1");
    auto ser = bootstrap_raft_types::serializer_type{};

    // One fake peer — not registered in the simulator, so ClusterJoin throws immediately.
    // This pushes run_bootstrap() into the retry-sleep loop.
    using pi = kythira::peer_info<std::uint64_t, std::string>;
    preset_peer_discovery<std::uint64_t, std::string> disc{{pi{99, "ghost_node:9000"}}};

    bootstrap_node_type node{1,
                             bootstrap_raft_types::network_client_type{net, ser},
                             bootstrap_raft_types::network_server_type{net, ser},
                             bootstrap_raft_types::persistence_engine_type{},
                             kythira::console_logger{},
                             bootstrap_raft_types::metrics_type{},
                             bootstrap_raft_types::membership_manager_type{},
                             make_fast_config(),
                             "node1",
                             std::move(disc)};

    // Run start() in a background thread (it blocks on the bootstrap retry loop)
    std::promise<void> done;
    auto done_fut = done.get_future();
    std::thread start_thread([&node, &done]() {
        node.start();
        done.set_value();
    });

    // Wait long enough for the bootstrap retry loop to be entered (~50 ms), then stop.
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    node.stop();

    // start() should return within one retry-sleep tick (~50 ms) after stop() is called.
    auto status = done_fut.wait_for(std::chrono::milliseconds{500});
    BOOST_CHECK(status == std::future_status::ready);
    start_thread.join();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite 4: full Raft protocol via bootstrap types (single node) ──────────
//
// All nodes use empty peer discovery → immediately found a single-node cluster.
// These tests cover the core Raft code paths (submit_command, elections,
// heartbeats, sessions, read_state) for the bootstrap_raft_types instantiation.

BOOST_AUTO_TEST_SUITE(bootstrap_protocol_single_node)

BOOST_AUTO_TEST_CASE(submit_command_on_follower_fails, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto node = make_bootstrap_node(1, sim.create_node("1"));
    node.start();

    // Immediately after start the node is a follower — command must be rejected
    auto fut = node.submit_command(make_put_cmd("k", "v"), std::chrono::milliseconds{500});
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);

    node.stop();
}

BOOST_AUTO_TEST_CASE(submit_command_with_session_on_follower_fails,
                     *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto node = make_bootstrap_node(1, sim.create_node("1"));
    node.start();

    auto fut = node.submit_command_with_session(42, 1, make_put_cmd("k", "v"),
                                                std::chrono::milliseconds{500});
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);

    node.stop();
}

BOOST_AUTO_TEST_CASE(add_server_on_follower_fails, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto node = make_bootstrap_node(1, sim.create_node("1"));
    node.start();

    auto fut = node.add_server(99);
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);

    node.stop();
}

BOOST_AUTO_TEST_CASE(remove_server_on_follower_fails, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto node = make_bootstrap_node(1, sim.create_node("1"));
    node.start();

    auto fut = node.remove_server(99);
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);

    node.stop();
}

BOOST_AUTO_TEST_CASE(read_state_on_follower_fails, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto node = make_bootstrap_node(1, sim.create_node("1"));
    node.start();
    BOOST_REQUIRE_EQUAL(node.get_state(), kythira::server_state::follower);

    auto fut = node.read_state(std::chrono::milliseconds{500});
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);

    node.stop();
}

BOOST_AUTO_TEST_CASE(single_node_wins_election, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);
    node.start();
    BOOST_REQUIRE_EQUAL(node.get_state(), kythira::server_state::follower);

    elect_single_node(node, cfg);

    BOOST_CHECK(node.is_leader());
    BOOST_CHECK_GE(node.get_current_term(), 1u);

    node.stop();
}

BOOST_AUTO_TEST_CASE(single_node_command_commits, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);
    node.start();

    elect_single_node(node, cfg);
    BOOST_REQUIRE(node.is_leader());

    auto result =
        std::move(node.submit_command(make_put_cmd("x", "1"), std::chrono::milliseconds{2000}))
            .get();
    BOOST_CHECK(result.empty());

    node.stop();
}

BOOST_AUTO_TEST_CASE(single_node_multiple_commands, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);
    node.start();

    elect_single_node(node, cfg);
    BOOST_REQUIRE(node.is_leader());

    std::size_t committed = 0;
    for (auto const& [k, v] :
         std::vector<std::pair<std::string, std::string>>{{"a", "1"}, {"b", "2"}, {"c", "3"}}) {
        std::move(node.submit_command(make_put_cmd(k, v), std::chrono::milliseconds{2000})).get();
        ++committed;
    }
    BOOST_CHECK_EQUAL(committed, 3u);

    node.stop();
}

BOOST_AUTO_TEST_CASE(add_server_already_in_config, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);
    node.start();

    elect_single_node(node, cfg);
    BOOST_REQUIRE(node.is_leader());

    auto fut = node.add_server(1);  // node 1 is already the only member
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);

    node.stop();
}

BOOST_AUTO_TEST_CASE(remove_server_not_in_config, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);
    node.start();

    elect_single_node(node, cfg);
    BOOST_REQUIRE(node.is_leader());

    auto fut = node.remove_server(99);
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);

    node.stop();
}

BOOST_AUTO_TEST_CASE(submit_command_with_session_commits, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);
    node.start();

    elect_single_node(node, cfg);
    BOOST_REQUIRE(node.is_leader());

    auto result = std::move(node.submit_command_with_session(1, 1, make_put_cmd("k", "v"),
                                                             std::chrono::milliseconds{2000}))
                      .get();
    BOOST_CHECK(result.empty());

    node.stop();
}

BOOST_AUTO_TEST_CASE(submit_command_with_session_dedup, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);
    node.start();

    elect_single_node(node, cfg);
    BOOST_REQUIRE(node.is_leader());

    auto cmd = make_put_cmd("x", "1");
    auto r1 =
        std::move(node.submit_command_with_session(7, 1, cmd, std::chrono::milliseconds{2000}))
            .get();
    auto r2 =
        std::move(node.submit_command_with_session(7, 1, cmd, std::chrono::milliseconds{2000}))
            .get();
    BOOST_CHECK(r1 == r2);

    node.stop();
}

BOOST_AUTO_TEST_CASE(session_invalid_initial_serial, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);
    node.start();

    elect_single_node(node, cfg);
    BOOST_REQUIRE(node.is_leader());

    // New client must start at serial_number=1; anything else is rejected
    auto fut = node.submit_command_with_session(99, 5, make_put_cmd("k", "v"),
                                                std::chrono::milliseconds{500});
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);

    node.stop();
}

BOOST_AUTO_TEST_CASE(session_skipped_serial, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);
    node.start();

    elect_single_node(node, cfg);
    BOOST_REQUIRE(node.is_leader());

    // Establish session at serial=1
    std::move(node.submit_command_with_session(5, 1, make_put_cmd("a", "1"),
                                               std::chrono::milliseconds{2000}))
        .get();

    // Skip serial=2, jump to serial=3 — must be rejected
    auto fut = node.submit_command_with_session(5, 3, make_put_cmd("a", "2"),
                                                std::chrono::milliseconds{500});
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);

    node.stop();
}

BOOST_AUTO_TEST_CASE(check_heartbeat_as_leader, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);
    node.start();

    elect_single_node(node, cfg);
    BOOST_REQUIRE(node.is_leader());

    std::this_thread::sleep_for(cfg._heartbeat_interval + std::chrono::milliseconds{20});
    BOOST_CHECK_NO_THROW(node.check_heartbeat_timeout());
    BOOST_CHECK(node.is_leader());

    node.stop();
}

BOOST_AUTO_TEST_CASE(check_heartbeat_as_follower, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto node = make_bootstrap_node(1, sim.create_node("1"));
    node.start();
    BOOST_REQUIRE_EQUAL(node.get_state(), kythira::server_state::follower);

    BOOST_CHECK_NO_THROW(node.check_heartbeat_timeout());

    node.stop();
}

BOOST_AUTO_TEST_CASE(read_state_on_leader_succeeds, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);
    node.start();

    elect_single_node(node, cfg);
    BOOST_REQUIRE(node.is_leader());

    auto state = std::move(node.read_state(std::chrono::milliseconds{2000})).get();
    (void)state;

    node.stop();
}

BOOST_AUTO_TEST_CASE(read_state_reflects_committed_command, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);
    node.start();

    elect_single_node(node, cfg);
    BOOST_REQUIRE(node.is_leader());

    std::move(node.submit_command(make_put_cmd("hello", "world"), std::chrono::milliseconds{2000}))
        .get();

    auto state = std::move(node.read_state(std::chrono::milliseconds{2000})).get();
    BOOST_CHECK(!state.empty());

    node.stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite 5: full Raft protocol via bootstrap types (three nodes) ──────────
//
// Three independent single-node clusters; each elects itself and commits.
// Exercises concurrent node lifecycles and the commit pipeline for three
// separate bootstrap_raft_types instantiation paths.

BOOST_AUTO_TEST_SUITE(bootstrap_protocol_three_node)

BOOST_AUTO_TEST_CASE(three_independent_leaders_commit, *boost::unit_test::timeout(30)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();

    auto node1 = make_bootstrap_node(1, sim.create_node("1"), cfg);
    auto node2 = make_bootstrap_node(2, sim.create_node("2"), cfg);
    auto node3 = make_bootstrap_node(3, sim.create_node("3"), cfg);

    node1.start();
    node2.start();
    node3.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node1.check_election_timeout();
    node2.check_election_timeout();
    node3.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    BOOST_REQUIRE(node1.is_leader());
    BOOST_REQUIRE(node2.is_leader());
    BOOST_REQUIRE(node3.is_leader());

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

BOOST_AUTO_TEST_CASE(three_nodes_commit_index_advances, *boost::unit_test::timeout(30)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();

    auto node1 = make_bootstrap_node(1, sim.create_node("1"), cfg);
    auto node2 = make_bootstrap_node(2, sim.create_node("2"), cfg);
    auto node3 = make_bootstrap_node(3, sim.create_node("3"), cfg);

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

    BOOST_CHECK_GE(node1.get_current_term(), 1u);
    BOOST_CHECK_EQUAL(node1.get_state(), kythira::server_state::leader);

    node1.stop();
    node2.stop();
    node3.stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite 6: handle_cluster_join via network ──────────────────────────────
//
// handle_cluster_join is called internally when a ClusterJoin RPC arrives.
// We test it indirectly: a fresh node bootstraps by contacting a leader/follower
// and the join response determines what happens.

BOOST_AUTO_TEST_SUITE(bootstrap_cluster_join)

// A leader node receives a ClusterJoin request through the network protocol
// and accepts it (internally calls handle_cluster_join → add_server).
BOOST_AUTO_TEST_CASE(leader_accepts_join_via_network, *boost::unit_test::timeout(20)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    bconnect_all(sim, {"1", "2"});

    // Node 1: single-node cluster, empty discovery
    auto node1 = make_bootstrap_node(1, net1, cfg);
    node1.start();

    elect_single_node(node1, cfg);
    BOOST_REQUIRE(node1.is_leader());

    // Node 2: fresh node, peer discovery returns node 1 (the leader)
    auto ser2 = bootstrap_raft_types::serializer_type{};
    bootstrap_node_type node2{
        2,
        bootstrap_raft_types::network_client_type{net2, ser2},
        bootstrap_raft_types::network_server_type{net2, ser2},
        bootstrap_raft_types::persistence_engine_type{},
        kythira::console_logger{},
        bootstrap_raft_types::metrics_type{},
        bootstrap_raft_types::membership_manager_type{},
        cfg,
        "2",
        preset_peer_discovery<std::uint64_t, std::string>(
            std::vector<kythira::peer_info<std::uint64_t, std::string>>{{1u, "1"}})};

    // start() blocks until ClusterJoin is accepted; run in background thread
    std::promise<void> joined;
    auto join_fut = joined.get_future();
    std::thread t([&node2, &joined]() {
        node2.start();
        joined.set_value();
    });

    // The join should complete promptly since the leader is reachable
    auto status = join_fut.wait_for(std::chrono::seconds{10});
    BOOST_CHECK(status == std::future_status::ready);

    node2.stop();
    node1.stop();
    t.join();
}

// A node that was stopped before becoming leader receives a ClusterJoin
// request; it returns "not accepted, no redirect" because it has no
// known leader.  Covered via the network-facing handler path inside start().
BOOST_AUTO_TEST_CASE(fresh_node_no_peers_founds_cluster, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);

    // start() with no peers must return quickly (single-node founding)
    BOOST_CHECK_NO_THROW(node.start());

    node.stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite 7: stop/start lifecycle ─────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(bootstrap_lifecycle)

// stop() → start() → stop() must not crash or terminate (tests the
// _stop_requested reset bug fix in start()).
BOOST_AUTO_TEST_CASE(stop_start_stop_works, *boost::unit_test::timeout(15)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    auto node = make_bootstrap_node(1, sim.create_node("1"), cfg);

    node.start();
    elect_single_node(node, cfg);
    BOOST_REQUIRE(node.is_leader());

    node.stop();
    BOOST_CHECK(!node.is_running());

    // Second start must succeed (not get stuck because _stop_requested is stale)
    BOOST_CHECK_NO_THROW(node.start());
    BOOST_CHECK(node.is_running());

    node.stop();
}

// start() on an already-running node is a no-op (logged warning, no crash).
BOOST_AUTO_TEST_CASE(double_start_is_noop, *boost::unit_test::timeout(10)) {
    bsim_t sim;
    sim.start();
    auto node = make_bootstrap_node(1, sim.create_node("1"));
    node.start();
    BOOST_CHECK(node.is_running());

    BOOST_CHECK_NO_THROW(node.start());  // duplicate start must not crash
    BOOST_CHECK(node.is_running());

    node.stop();
}

// stop() on a node that never started is a no-op (logged warning, no crash).
BOOST_AUTO_TEST_CASE(stop_before_start_is_noop, *boost::unit_test::timeout(5)) {
    bsim_t sim;
    sim.start();
    auto node = make_bootstrap_node(1, sim.create_node("1"));

    BOOST_CHECK_NO_THROW(node.stop());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite 8: proper multi-node Raft clusters ──────────────────────────────
//
// These tests build real connected clusters where nodes communicate over the
// network simulator.  They cover the follower-side AppendEntries handler, the
// multi-node read_state heartbeat path (raft.hpp ~981-1067), and the
// membership-change pipeline across bootstrap_raft_types instantiations.

BOOST_AUTO_TEST_SUITE(bootstrap_protocol_cluster)

// Two-node cluster: node2 joins node1 (which is already the leader), then node1
// replicates several commands to node2 via AppendEntries.  Covers the follower
// AppendEntries handler paths (raft.hpp ~1800-2000) in the bootstrap compilation unit.
BOOST_AUTO_TEST_CASE(two_node_leader_replicates_to_follower, *boost::unit_test::timeout(30)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_cluster_config();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    bconnect_all(sim, {"1", "2"});

    auto node1 = make_bootstrap_node(1, net1, cfg);
    node1.start();
    elect_single_node(node1, cfg);
    BOOST_REQUIRE(node1.is_leader());

    auto node2 = make_joining_node(2, net2, 1, "1", cfg);

    std::promise<void> joined;
    auto join_fut = joined.get_future();
    std::thread t([&node2, &joined] {
        node2.start();
        joined.set_value();
    });

    BOOST_REQUIRE(join_fut.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK_NO_THROW(
            std::move(node1.submit_command(make_put_cmd("k" + std::to_string(i), "v"),
                                           std::chrono::milliseconds{5000}))
                .get());
    }

    BOOST_CHECK(node1.is_leader());

    node2.stop();
    node1.stop();
    t.join();
}

// Two-node cluster: after node2 joins, the leader calls read_state() which
// sends heartbeats to node2 for multi-node leadership verification.
// Covers raft.hpp lines ~981-1067 (the multi-node read_state heartbeat path).
BOOST_AUTO_TEST_CASE(two_node_read_state_multi_node, *boost::unit_test::timeout(30)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_cluster_config();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    bconnect_all(sim, {"1", "2"});

    auto node1 = make_bootstrap_node(1, net1, cfg);
    node1.start();
    elect_single_node(node1, cfg);
    BOOST_REQUIRE(node1.is_leader());

    auto node2 = make_joining_node(2, net2, 1, "1", cfg);

    std::promise<void> joined;
    auto join_fut = joined.get_future();
    std::thread t([&node2, &joined] {
        node2.start();
        joined.set_value();
    });

    BOOST_REQUIRE(join_fut.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    BOOST_CHECK_NO_THROW(std::move(node1.read_state(std::chrono::milliseconds{5000})).get());

    node2.stop();
    node1.stop();
    t.join();
}

// Two-node cluster: the leader's heartbeat timer fires and sends AppendEntries
// to node2.  Covers the leader-side periodic heartbeat send path.
BOOST_AUTO_TEST_CASE(two_node_heartbeat_reaches_follower, *boost::unit_test::timeout(30)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_cluster_config();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    bconnect_all(sim, {"1", "2"});

    auto node1 = make_bootstrap_node(1, net1, cfg);
    node1.start();
    elect_single_node(node1, cfg);
    BOOST_REQUIRE(node1.is_leader());

    auto node2 = make_joining_node(2, net2, 1, "1", cfg);

    std::promise<void> joined;
    auto join_fut = joined.get_future();
    std::thread t([&node2, &joined] {
        node2.start();
        joined.set_value();
    });

    BOOST_REQUIRE(join_fut.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    std::this_thread::sleep_for(cfg._heartbeat_interval + std::chrono::milliseconds{10});
    BOOST_CHECK_NO_THROW(node1.check_heartbeat_timeout());
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    BOOST_CHECK(node1.is_leader());

    node2.stop();
    node1.stop();
    t.join();
}

// Two-node cluster: interleave writes and multi-node reads.
// Exercises commit_waiter and future paths across bootstrap_raft_types instantiations.
BOOST_AUTO_TEST_CASE(two_node_interleaved_reads_and_writes, *boost::unit_test::timeout(30)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_cluster_config();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    bconnect_all(sim, {"1", "2"});

    auto node1 = make_bootstrap_node(1, net1, cfg);
    node1.start();
    elect_single_node(node1, cfg);
    BOOST_REQUIRE(node1.is_leader());

    auto node2 = make_joining_node(2, net2, 1, "1", cfg);

    std::promise<void> joined;
    auto join_fut = joined.get_future();
    std::thread t([&node2, &joined] {
        node2.start();
        joined.set_value();
    });

    BOOST_REQUIRE(join_fut.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK_NO_THROW(
            std::move(node1.submit_command(make_put_cmd("key" + std::to_string(i), "val"),
                                           std::chrono::milliseconds{5000}))
                .get());
        BOOST_CHECK_NO_THROW(std::move(node1.read_state(std::chrono::milliseconds{5000})).get());
    }

    node2.stop();
    node1.stop();
    t.join();
}

// Three-node cluster: node2 and node3 join node1 and the leader replicates to
// both followers.  The concurrent join exercises membership-change retry (only
// one config change at a time in Raft), AppendEntries to two followers, and
// the three-node read_state majority heartbeat.
BOOST_AUTO_TEST_CASE(three_node_cluster_two_followers, *boost::unit_test::timeout(60)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_cluster_config();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    auto net3 = sim.create_node("3");
    bconnect_all(sim, {"1", "2", "3"});

    auto node1 = make_bootstrap_node(1, net1, cfg);
    node1.start();
    elect_single_node(node1, cfg);
    BOOST_REQUIRE(node1.is_leader());

    auto node2 = make_joining_node(2, net2, 1, "1", cfg);
    auto node3 = make_joining_node(3, net3, 1, "1", cfg);

    std::promise<void> joined2;
    std::promise<void> joined3;
    auto fut2 = joined2.get_future();
    auto fut3 = joined3.get_future();

    std::thread t2([&node2, &joined2] {
        node2.start();
        joined2.set_value();
    });
    std::thread t3([&node3, &joined3] {
        node3.start();
        joined3.set_value();
    });

    BOOST_REQUIRE(fut2.wait_for(std::chrono::seconds{15}) == std::future_status::ready);
    BOOST_REQUIRE(fut3.wait_for(std::chrono::seconds{15}) == std::future_status::ready);

    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK_NO_THROW(
            std::move(node1.submit_command(make_put_cmd("k" + std::to_string(i), "v"),
                                           std::chrono::milliseconds{5000}))
                .get());
    }

    BOOST_CHECK_NO_THROW(std::move(node1.read_state(std::chrono::milliseconds{5000})).get());

    node3.stop();
    node2.stop();
    node1.stop();
    t3.join();
    t2.join();
}

// Two-node cluster: submit commands with a client session in a multi-node
// context.  Exercises the session-aware commit path and deduplication logic.
BOOST_AUTO_TEST_CASE(two_node_submit_with_session, *boost::unit_test::timeout(30)) {
    bsim_t sim;
    sim.start();
    auto cfg = make_cluster_config();

    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    bconnect_all(sim, {"1", "2"});

    auto node1 = make_bootstrap_node(1, net1, cfg);
    node1.start();
    elect_single_node(node1, cfg);
    BOOST_REQUIRE(node1.is_leader());

    auto node2 = make_joining_node(2, net2, 1, "1", cfg);

    std::promise<void> joined;
    auto join_fut = joined.get_future();
    std::thread t([&node2, &joined] {
        node2.start();
        joined.set_value();
    });

    BOOST_REQUIRE(join_fut.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    BOOST_CHECK_NO_THROW(
        std::move(node1.submit_command_with_session(10, 1, make_put_cmd("a", "1"),
                                                    std::chrono::milliseconds{5000}))
            .get());
    BOOST_CHECK_NO_THROW(
        std::move(node1.submit_command_with_session(10, 2, make_put_cmd("a", "2"),
                                                    std::chrono::milliseconds{5000}))
            .get());

    node2.stop();
    node1.stop();
    t.join();
}

BOOST_AUTO_TEST_SUITE_END()
