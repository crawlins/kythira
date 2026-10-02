// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Integration Test for Snapshot Creation with State Machine
 *
 * Tests that create_snapshot() captures the real state machine state at the
 * last applied index, compacts the log it covers, and that a node restarted
 * on the same persistence engine restores from it.
 *
 * Requirements: 10.1, 10.2, 31.1
 * Covers raft-consensus tasks 303.2/314/504 (create_snapshot takes its state
 * from the state machine) and membership-change task 19 (restart restores
 * from the snapshot).
 */

#define BOOST_TEST_MODULE RaftSnapshotCreationIntegrationTest
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/raft.hpp>
#include <raft/examples/counter_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_snapshot_creation_integration_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

using engine_type = kythira::memory_persistence_engine<>;

// The node takes its persistence engine by value, so a plain
// memory_persistence_engine leaves the test nothing to inspect afterwards and
// nothing to hand a restarted node. Every copy of this one shares the same
// engine.
class shared_persistence {
public:
    using log_entry_t = engine_type::log_entry_t;
    using snapshot_t = engine_type::snapshot_t;

    shared_persistence() : _engine(std::make_shared<engine_type>()) {}

    auto save_current_term(std::uint64_t term) -> void { _engine->save_current_term(term); }
    auto load_current_term() -> std::uint64_t { return _engine->load_current_term(); }
    auto save_voted_for(std::uint64_t node) -> void { _engine->save_voted_for(node); }
    auto load_voted_for() -> std::optional<std::uint64_t> { return _engine->load_voted_for(); }
    auto append_log_entry(const log_entry_t& entry) -> void { _engine->append_log_entry(entry); }
    auto get_log_entry(std::uint64_t index) -> std::optional<log_entry_t> {
        return _engine->get_log_entry(index);
    }
    auto get_log_entries(std::uint64_t start, std::uint64_t end) -> std::vector<log_entry_t> {
        return _engine->get_log_entries(start, end);
    }
    auto get_last_log_index() -> std::uint64_t { return _engine->get_last_log_index(); }
    auto truncate_log(std::uint64_t index) -> void { _engine->truncate_log(index); }
    auto save_snapshot(const snapshot_t& snap) -> void { _engine->save_snapshot(snap); }
    auto load_snapshot() -> std::optional<snapshot_t> { return _engine->load_snapshot(); }
    auto delete_log_entries_before(std::uint64_t index) -> void {
        _engine->delete_log_entries_before(index);
    }

private:
    std::shared_ptr<engine_type> _engine;
};

struct counter_raft_types {
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

    using persistence_engine_type = shared_persistence;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kythira::examples::counter_state_machine;
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

using node_type = kythira::node<counter_raft_types>;
using simulator_type = network_simulator::NetworkSimulator<counter_raft_types::raft_network_types>;

constexpr std::uint64_t node_id = 1;
constexpr std::int64_t increments = 10;
constexpr std::chrono::milliseconds commit_timeout{2000};

auto make_config() -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{80};
    cfg._election_timeout_max = std::chrono::milliseconds{160};
    cfg._heartbeat_interval = std::chrono::milliseconds{26};
    cfg._rpc_timeout = std::chrono::milliseconds{80};
    return cfg;
}

auto make_node(simulator_type& sim, const shared_persistence& persistence,
               const kythira::raft_configuration& cfg) -> std::unique_ptr<node_type> {
    auto net = sim.create_node(std::to_string(node_id));
    auto serializer = counter_raft_types::serializer_type{};
    return std::make_unique<node_type>(
        node_id, counter_raft_types::network_client_type{net, serializer},
        counter_raft_types::network_server_type{net, serializer}, persistence,
        kythira::console_logger{kythira::log_level::error}, kythira::noop_metrics{},
        kythira::default_membership_manager<std::uint64_t>{}, cfg);
}

// Sleep past the election timeout, then drive the check: a node alone in its
// configuration wins its own election.
auto elect(node_type& node, const kythira::raft_configuration& cfg) -> void {
    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
}

auto make_command(const std::string& cmd) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(cmd.data()),
            reinterpret_cast<const std::byte*>(cmd.data() + cmd.size())};
}

auto decode_counter(const std::vector<std::byte>& state) -> std::int64_t {
    std::int64_t value = 0;
    BOOST_REQUIRE_EQUAL(state.size(), sizeof(value));
    std::memcpy(&value, state.data(), sizeof(value));
    return value;
}

auto counter_value(node_type& node) -> std::int64_t {
    return node.with_state_machine(
        [](const kythira::examples::counter_state_machine& sm) { return sm.get_value(); });
}

}  // namespace

/**
 * create_snapshot() stores the state machine's own state at the last applied
 * index, and the log entries it covers are gone from the persistence engine.
 */
BOOST_AUTO_TEST_CASE(create_snapshot_captures_state_machine_state, *boost::unit_test::timeout(30)) {
    simulator_type sim;
    sim.start();
    const auto cfg = make_config();
    shared_persistence persistence;

    auto node = make_node(sim, persistence, cfg);
    node->start();
    elect(*node, cfg);
    BOOST_REQUIRE(node->is_leader());

    for (std::int64_t i = 0; i < increments; ++i) {
        std::move(node->submit_command(make_command("INC"), commit_timeout)).get();
    }
    BOOST_REQUIRE_EQUAL(counter_value(*node), increments);

    const auto applied = node->last_applied_index();
    const auto term = node->get_current_term();
    BOOST_REQUIRE(!persistence.load_snapshot().has_value());

    node->create_snapshot();

    const auto snapshot = persistence.load_snapshot();
    BOOST_REQUIRE(snapshot.has_value());
    BOOST_CHECK_EQUAL(snapshot->last_included_index(), applied);
    BOOST_CHECK_EQUAL(snapshot->last_included_term(), term);
    BOOST_CHECK_EQUAL(decode_counter(snapshot->state_machine_state()), increments);

    // compact_log() ran: nothing at or below the snapshot index is left.
    for (std::uint64_t index = 1; index <= applied; ++index) {
        BOOST_CHECK_MESSAGE(!persistence.get_log_entry(index).has_value(),
                            "entry " << index << " survived compaction");
    }

    node->stop();
}

/**
 * A node restarted on the same persistence engine restores the counter and
 * its applied index from the snapshot, then keeps accepting commands on top
 * of it.
 */
BOOST_AUTO_TEST_CASE(restart_restores_from_created_snapshot, *boost::unit_test::timeout(30)) {
    simulator_type sim;
    sim.start();
    const auto cfg = make_config();
    shared_persistence persistence;

    std::uint64_t applied = 0;
    {
        auto node = make_node(sim, persistence, cfg);
        node->start();
        elect(*node, cfg);
        BOOST_REQUIRE(node->is_leader());
        for (std::int64_t i = 0; i < increments; ++i) {
            std::move(node->submit_command(make_command("INC"), commit_timeout)).get();
        }
        applied = node->last_applied_index();
        node->create_snapshot();
        node->stop();
    }

    simulator_type restart_sim;
    restart_sim.start();
    auto restarted = make_node(restart_sim, persistence, cfg);
    restarted->start();

    BOOST_CHECK_EQUAL(restarted->last_applied_index(), applied);
    BOOST_CHECK_EQUAL(counter_value(*restarted), increments);

    elect(*restarted, cfg);
    BOOST_REQUIRE(restarted->is_leader());
    std::move(restarted->submit_command(make_command("INC"), commit_timeout)).get();
    BOOST_CHECK_EQUAL(counter_value(*restarted), increments + 1);

    restarted->stop();
}
