// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// An embedded IAM read replica (`.kiro/specs/iam-compatible-auth/` task 13,
// Requirements 7.3-7.4): a learner that applies the IAM log, takes a
// policy change within 5 s, and fails closed once cut off from the voters
// for longer than its staleness bound. In process, over the network
// simulator, so the partition is exact and the test is quick.

#define BOOST_TEST_MODULE iam_replica_test

#include <boost/test/unit_test.hpp>

#include <kythira/iam/iam_authorizer.hpp>
#include <kythira/iam/iam_replica.hpp>
#include <kythira/iam/iam_service_registry.hpp>
#include <kythira/iam/iam_state_machine.hpp>

#include <raft/future_default.hpp>
#include <raft/raft.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <array>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace iam = kythira::iam;
namespace aws_auth = kythira::aws_auth;
namespace leader = kythira::iam::iam_leader;
using iam::iam_command;
using namespace std::chrono_literals;

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
// The RPC client's retries use folly's Timekeeper.
struct folly_init {
    folly_init() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("iam_replica_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(folly_init);
#endif

namespace {

constexpr std::string_view k_account = "111122223333";

template<typename NodeId, typename Address> class no_peer_discovery {
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

struct types {
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

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = iam::iam_state_machine;
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
    using peer_discovery_type = no_peer_discovery<node_id_type, address_type>;
    using cluster_join_request_type = kythira::cluster_join_request<node_id_type, address_type>;
    using cluster_join_response_type = kythira::cluster_join_response<node_id_type, address_type>;
    using quorum_manager_type =
        kythira::no_op_quorum_manager<node_id_type, address_type, std::string>;
};

using sim_t = network_simulator::NetworkSimulator<types::raft_network_types>;

auto fast_config() -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = 80ms;
    cfg._election_timeout_max = 160ms;
    cfg._heartbeat_interval = 26ms;
    cfg._rpc_timeout = 200ms;
    return cfg;
}

auto node_config(std::uint64_t id, auto net) -> kythira::node_config<types> {
    return kythira::node_config<types>{
        .node_id = id,
        .network_client = {net, types::serializer_type{}},
        .network_server = {net, types::serializer_type{}},
        .persistence = {},
        .logger = kythira::console_logger{kythira::log_level::warning},
        .metrics = {},
        .membership = {},
        .state_machine = iam::iam_state_machine{},
        .config = fast_config(),
        .self_address = std::to_string(id),
        .peer_discovery = {},
        .quorum_manager = types::quorum_manager_type{kythira::desired_topology<std::string>{
            .groups = {{.group_id = "", .target_count = 1, .learner_capacity = 4}}}},
    };
}

template<typename Pred> auto wait_until(Pred pred, std::chrono::milliseconds limit) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

void link(sim_t& sim, bool up) {
    for (const auto& [from, to] : {std::pair{"1", "2"}, std::pair{"2", "1"}}) {
        if (up) {
            sim.add_edge(from, to, network_simulator::NetworkEdge{});
        } else {
            sim.remove_edge(from, to);
        }
    }
}

auto test_kek() -> iam::seal_key {
    std::array<unsigned char, iam::seal_key::size> b{};
    for (std::size_t i = 0; i < b.size(); ++i) {
        b[i] = static_cast<unsigned char>(0x40 + i);
    }
    return iam::seal_key(b);
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_replica_applies_changes_and_fails_closed_when_cut_off,
                     *boost::unit_test::timeout(120)) {
    sim_t sim;
    sim.start();
    auto net1 = sim.create_node("1");
    auto net2 = sim.create_node("2");
    link(sim, true);

    kythira::node<types> voter(node_config(1, net1));
    voter.set_cluster_configuration({1});
    // A short bound, so the partition below is detected quickly.
    iam::iam_replica<types> replica(node_config(2, net2), {1}, 1000ms);
    BOOST_TEST(!replica.freshness().fresh());  // never caught up yet
    voter.start();
    replica.start();

    std::atomic<bool> stopping{false};
    std::thread timers([&] {
        while (!stopping.load()) {
            voter.check_election_timeout();
            voter.check_heartbeat_timeout();
            std::this_thread::sleep_for(13ms);
        }
    });
    BOOST_REQUIRE(wait_until([&] { return voter.is_leader(); }, 10s));
    voter.add_learner(2).get();

    auto submit = [&](iam_command type, boost::json::object body) {
        body["account"] = k_account;
        voter.submit_command(leader::command(type, std::move(body)), 5000ms).get();
    };
    submit(iam_command::create_account, {{"kek_salt", "c2FsdHNhbHRzYWx0c2FsdA"}});
    submit(iam_command::create_user, {{"name", "alice"}, {"id", "AIDAALICE00000000001"}});

    iam::iam_service_registry registry;
    registry.add({.service_prefix = "s3", .actions = {{"GetObject", {"*"}, {}}}});
    const auto kek = test_kek();
    iam::iam_authorizer authz(
        replica.reader(), [&](std::string_view) -> const iam::seal_key& { return kek; }, registry);
    authz.set_freshness([&] { return replica.freshness().fresh(); });

    aws_auth::principal_context alice;
    alice.type = aws_auth::principal_type::user;
    alice.account = std::string(k_account);
    alice.arn = "arn:aws:iam::111122223333:user/alice";
    alice.userid = "AIDAALICE00000000001";
    alice.username = "alice";
    const aws_auth::authz_request get{.action = "s3:GetObject",
                                      .resource = "arn:aws:s3:::bucket/key"};

    BOOST_REQUIRE(wait_until(
        [&] {
            bool seen = false;
            replica.reader()([&](const iam::iam_state_machine& sm) {
                seen = sm.user(std::string(k_account), "alice") != nullptr;
            });
            return seen;
        },
        10s));
    BOOST_TEST(replica.freshness().fresh());
    BOOST_TEST(!authz.authorize(alice, get).allowed);

    // A policy change committed on the voter takes effect on the replica
    // within 5 s (Requirement 7.4).
    submit(
        iam_command::put_inline_policy,
        {{"kind", "user"},
         {"name", "alice"},
         {"policy_name", "read"},
         {"document",
          R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"s3:GetObject","Resource":"*"}]})"}});
    const auto committed = std::chrono::steady_clock::now();
    BOOST_TEST(wait_until([&] { return authz.authorize(alice, get).allowed; }, 5s));
    BOOST_TEST_MESSAGE("policy change visible on the replica after "
                       << std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - committed)
                              .count()
                       << " ms");
    const auto lag = replica.freshness().lag();
    BOOST_REQUIRE(lag.has_value());
    BOOST_TEST(lag->count() < 1.0);

    // Cut the replica off: within the bound (plus scheduling slack) it
    // stops answering, and an allow it gave before becomes a deny.
    link(sim, false);
    const auto cut = std::chrono::steady_clock::now();
    BOOST_TEST(wait_until([&] { return !replica.freshness().fresh(); }, 5s));
    BOOST_TEST((std::chrono::steady_clock::now() - cut >= 1000ms - 100ms));
    const auto d = authz.authorize(alice, get);
    BOOST_TEST(!d.allowed);
    BOOST_TEST(d.reason == "the IAM replica is stale");
    BOOST_TEST(replica.freshness().metrics_text().find("kythira_iam_replica_lag_seconds 1.") !=
               std::string::npos);

    // Reconnected, it catches up and answers again.
    link(sim, true);
    BOOST_TEST(wait_until([&] { return replica.freshness().fresh(); }, 10s));
    BOOST_TEST(authz.authorize(alice, get).allowed);

    stopping = true;
    timers.join();
    replica.stop();
    voter.stop();
}
