// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// .kiro/specs/cloud-composite-node-ids/, task 6 (Requirement 9): the
// simulator and TCP transports route by a textual or composite node id, and a
// Raft cluster whose members are named by composite ids elects, replicates
// and catches up over them.
#define BOOST_TEST_MODULE composite_node_id_transport_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/composite_node_id.hpp>
#include <raft/raft.hpp>
#include <raft/tcp_rpc.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("composite_node_id_transport_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

using text_client = kythira::basic_tcp_rpc_client<std::string>;
using text_server = kythira::basic_tcp_rpc_server<std::string>;
using docker_id = kythira::docker_container_node_id;

auto find_free_port() -> std::uint16_t {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE_GE(fd, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    (void)::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    const auto port = ntohs(a.sin_port);
    ::close(fd);
    return port;
}

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

// A simulator bundle whose members are named by Docker composite ids.
struct composite_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = docker_id;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using raft_network_types = kythira::raft_simulator_network_types<std::string>;
    using network_client_type =
        kythira::simulator_network_client<raft_network_types, serializer_type, serialized_data_type,
                                          node_id_type>;
    using network_server_type =
        kythira::simulator_network_server<raft_network_types, serializer_type, serialized_data_type,
                                          node_id_type>;

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

using composite_node = kythira::node<composite_types>;
using sim_t = network_simulator::NetworkSimulator<composite_types::raft_network_types>;
using replicator_t = composite_types::peer2peer_replicator_type;

template<typename Pred>
auto wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{8000})
    -> bool {
    const auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{15});
    }
    return true;
}

auto make_fast_config() -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{80};
    cfg._election_timeout_max = std::chrono::milliseconds{160};
    cfg._heartbeat_interval = std::chrono::milliseconds{20};
    cfg._rpc_timeout = std::chrono::milliseconds{200};
    cfg._progress_gossip_interval = std::chrono::milliseconds{25};
    cfg._catch_up_fetch_timeout = std::chrono::milliseconds{300};
    cfg._catch_up_gap_threshold = 0;
    return cfg;
}

}  // namespace

// ── TCP: a std::string target ──────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(tcp_request_vote_routes_by_text_id, *boost::unit_test::timeout(15)) {
    const auto port = find_free_port();
    text_server server(port, "127.0.0.1");
    std::mutex seen_mu;
    std::optional<std::string> seen_candidate;
    server.register_request_vote_handler(
        [&](const kythira::request_vote_request<std::string>& req) {
            std::lock_guard lock(seen_mu);
            seen_candidate = req.candidate_id();
            return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = true};
        });
    server.start();

    text_client client;
    client.add_peer("docker:kythira:node-2", "127.0.0.1", port);
    kythira::request_vote_request<std::string> req{};
    req._term = 7;
    req._candidate_id = "docker:kythira:node-1";

    auto resp =
        client.send_request_vote("docker:kythira:node-2", req, std::chrono::milliseconds{5000})
            .get();
    BOOST_TEST(resp.term() == 7u);
    BOOST_TEST(resp.vote_granted());
    {
        std::lock_guard lock(seen_mu);
        BOOST_REQUIRE(seen_candidate.has_value());
        BOOST_TEST(*seen_candidate == "docker:kythira:node-1");
    }

    BOOST_CHECK_THROW(
        client.send_request_vote("docker:kythira:node-9", req, std::chrono::milliseconds{100})
            .get(),
        kythira::network_exception);
    server.stop();
}

BOOST_AUTO_TEST_CASE(tcp_fetch_log_entries_carries_text_responder, *boost::unit_test::timeout(15)) {
    const auto port = find_free_port();
    text_server server(port, "127.0.0.1");
    server.register_fetch_log_entries_handler(
        [](const kythira::fetch_log_entries_request<std::string>& req) {
            return kythira::fetch_log_entries_response<std::string>{
                ._responder_id = "docker:kythira:node-2",
                ._available = req.requester_id() == "docker:kythira:node-3",
                ._prev_log_term = 0,
                ._entries = {}};
        });
    server.start();

    text_client client;
    client.add_peer("docker:kythira:node-2", "127.0.0.1", port);
    kythira::fetch_log_entries_request<std::string> req{};
    req._requester_id = "docker:kythira:node-3";
    req._from_index = 1;
    req._to_index = 1;
    auto resp =
        client.send_fetch_log_entries("docker:kythira:node-2", req, std::chrono::milliseconds{5000})
            .get();
    BOOST_TEST(resp.responder_id() == "docker:kythira:node-2");
    BOOST_TEST(resp.available());
    server.stop();
}

// A redirect names the leader by its id's text. "docker:kythira:3" also splits
// as host "docker:kythira", port 3, so the address form must not win over a
// registered peer of that name.
BOOST_AUTO_TEST_CASE(tcp_address_dispatch_prefers_registered_text_id,
                     *boost::unit_test::timeout(15)) {
    const auto port = find_free_port();
    text_server server(port, "127.0.0.1");
    server.register_cluster_join_handler([](const kythira::cluster_join_request<std::string>& req) {
        return kythira::cluster_join_response<std::string>{req.node_id == "docker:kythira:4",
                                                           std::nullopt};
    });
    server.start();

    text_client client;
    client.add_peer("docker:kythira:3", "127.0.0.1", port);
    kythira::cluster_join_request<std::string> req{.node_id = "docker:kythira:4",
                                                   .contact_address = "node-4:7000"};
    auto resp =
        client.send_cluster_join_request("docker:kythira:3", req, std::chrono::milliseconds{5000})
            .get();
    BOOST_TEST(resp.is_accepted());

    // A plain "host:port" still dials that endpoint.
    auto by_address = client
                          .send_cluster_join_request("127.0.0.1:" + std::to_string(port), req,
                                                     std::chrono::milliseconds{5000})
                          .get();
    BOOST_TEST(by_address.is_accepted());
    server.stop();
}

// A textual id that is all digits is a name like any other, not a numeric id.
BOOST_AUTO_TEST_CASE(tcp_all_digit_text_id_is_looked_up_by_name, *boost::unit_test::timeout(15)) {
    const auto port = find_free_port();
    text_server server(port, "127.0.0.1");
    server.register_cluster_join_handler([](const kythira::cluster_join_request<std::string>&) {
        return kythira::cluster_join_response<std::string>{true, std::nullopt};
    });
    server.start();

    text_client client;
    client.add_peer("0042", "127.0.0.1", port);
    kythira::cluster_join_request<std::string> req{.node_id = "a", .contact_address = "a:1"};
    BOOST_TEST(client.send_cluster_join_request("0042", req, std::chrono::milliseconds{5000})
                   .get()
                   .is_accepted());
    BOOST_CHECK_THROW(
        client.send_cluster_join_request("42", req, std::chrono::milliseconds{100}).get(),
        kythira::network_exception);
    server.stop();
}

// A digit string past uint64_t's range used to escape std::stoull as
// std::out_of_range from inside the call; it is now an ordinary failed RPC.
BOOST_AUTO_TEST_CASE(tcp_numeric_address_overflow_is_an_error_result,
                     *boost::unit_test::timeout(5)) {
    kythira::tcp_rpc_client client;
    kythira::cluster_join_request<> req{.node_id = 4, .contact_address = "node-4:7000"};
    BOOST_CHECK_THROW(client
                          .send_cluster_join_request("123456789012345678901234567890", req,
                                                     std::chrono::milliseconds{100})
                          .get(),
                      kythira::network_exception);
}

// ── Configuration log entries ───────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(configuration_entry_round_trips_composite_and_large_ids,
                     *boost::unit_test::timeout(5)) {
    const docker_id a{"kythira", "node-a"};
    const docker_id b{"kythira", "node-b"};
    kythira::cluster_configuration<docker_id> cfg;
    cfg._nodes = {a, b};
    cfg._learners = {docker_id{"kythira", "node-c"}};
    const std::map<docker_id, std::string> placement{{a, "zone-1"}};
    const auto bytes = kythira::serialize_configuration(cfg, &placement);
    const auto back = kythira::deserialize_configuration<docker_id>(bytes);
    BOOST_TEST((back._nodes == cfg._nodes));
    BOOST_TEST((back._learners == cfg._learners));
    BOOST_TEST((kythira::deserialize_placement<docker_id>(bytes) == placement));

    // Past INT64_MAX boost::json holds the value as uint64; `as_int64` threw.
    constexpr std::uint64_t big = (std::uint64_t{1} << 63U) + 5U;
    kythira::cluster_configuration<std::uint64_t> numeric;
    numeric._nodes = {1, big};
    const auto numeric_back = kythira::deserialize_configuration<std::uint64_t>(
        kythira::serialize_configuration(numeric));
    BOOST_TEST((numeric_back._nodes == numeric._nodes));
}

// ── Simulator: a three-node cluster named by composite ids ─────────────────

// Nodes A and B form the cluster; C is told only about B and has no link to
// A, so its log can only come from B's peer-to-peer fetch. Every RPC on the
// way (votes, AppendEntries, progress gossip, FetchLogEntries) is routed and
// stamped with a composite id.
BOOST_AUTO_TEST_CASE(simulator_composite_cluster_elects_replicates_and_catches_up,
                     *boost::unit_test::timeout(30)) {
    const docker_id a{"kythira", "node-a"};
    const docker_id b{"kythira", "node-b"};
    const docker_id c{"kythira", "node-c"};
    auto table = std::make_shared<replicator_t::table_type>();

    sim_t sim;
    sim.start();
    auto net_a = sim.create_node(a.to_string());
    auto net_b = sim.create_node(b.to_string());
    auto net_c = sim.create_node(c.to_string());
    network_simulator::NetworkEdge edge{};
    sim.add_edge(a.to_string(), b.to_string(), edge);
    sim.add_edge(b.to_string(), a.to_string(), edge);
    sim.add_edge(b.to_string(), c.to_string(), edge);
    sim.add_edge(c.to_string(), b.to_string(), edge);

    const auto cfg = make_fast_config();
    auto make_node = [&](const docker_id& id, auto net) {
        return composite_node{{
            .node_id = id,
            .network_client = {net, composite_types::serializer_type{}},
            .network_server = {net, composite_types::serializer_type{}},
            .persistence = {},
            .logger = kythira::console_logger{},
            .metrics = {},
            .membership = {},
            .config = cfg,
            .self_address = id.to_string(),
            .peer_discovery = preset_peer_discovery<docker_id, std::string>{},
            .peer2peer_replicator = replicator_t{table},
        }};
    };

    auto node_a = make_node(a, net_a);
    auto node_b = make_node(b, net_b);
    auto node_c = make_node(c, net_c);
    node_a.set_cluster_configuration({a, b});
    node_b.set_cluster_configuration({a, b});
    node_c.set_cluster_configuration({b, c});

    node_a.start();
    node_b.start();
    node_c.start();

    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node_a.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node_a.is_leader(); }));

    constexpr int num_commands = 10;
    for (int i = 0; i < num_commands; ++i) {
        auto command = kythira::test_key_value_state_machine<std::uint64_t>::make_put_command(
            "key" + std::to_string(i), "value" + std::to_string(i));
        try {
            node_a.submit_command(command, std::chrono::milliseconds{500});
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // A timed-out submit still lands in the log; the checks below
            // compare logs, not submit results.
        }
        node_a.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{15});
    }
    for (int i = 0; i < 15; ++i) {
        node_a.check_heartbeat_timeout();
        std::this_thread::sleep_for(cfg._heartbeat_interval);
    }

    const auto commit = node_a.debug_state().commit_index;
    BOOST_REQUIRE_GT(commit, 0u);
    BOOST_REQUIRE(wait_until([&] { return node_b.debug_state().last_applied >= commit; }));

    BOOST_REQUIRE(wait_until(
        [&] {
            node_a.check_election_timeout();
            node_b.check_election_timeout();
            node_c.check_peer_catch_up();
            return node_c.debug_state().log.size() >= node_a.debug_state().log.size();
        },
        std::chrono::milliseconds{6000}));

    const auto log_a = node_a.debug_state().log;
    const auto log_c = node_c.debug_state().log;
    BOOST_REQUIRE_EQUAL(log_c.size(), log_a.size());
    for (std::size_t i = 0; i < log_a.size(); ++i) {
        BOOST_CHECK_EQUAL(log_c[i].index(), log_a[i].index());
        BOOST_CHECK_EQUAL(log_c[i].term(), log_a[i].term());
        BOOST_CHECK(log_c[i].command() == log_a[i].command());
    }
    BOOST_CHECK(!node_c.is_leader());

    node_c.stop();
    node_b.stop();
    node_a.stop();
}
