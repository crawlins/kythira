// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// `node<Types>` over a persistent state machine
// (.kiro/specs/persistent-state-machine/ task 3): restart without reloading
// state (Requirement 4), metadata-only snapshots and compaction bound by the
// durable applied index (3), reads that serialize nothing (5), crash and
// replay with a held-back durable sequence (2.2, 2.4), and InstallSnapshot
// refused (10.2).

#define BOOST_TEST_MODULE persistent_node_integration_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/raft.hpp>

#include "persistent_register_state_machine.hpp"
#include "recording_metrics.hpp"

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("persistent_node_integration_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

using engine_type = kythira::memory_persistence_engine<>;
using register_sm = kythira::testing::persistent_register_state_machine;

// Every copy shares one engine, so a restarted node sees the same store and
// the test can inspect it.
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

struct persistent_raft_types {
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
    using metrics_type = kythira::testing::recording_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = register_sm;
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

static_assert(kythira::raft_types<persistent_raft_types>);

using node_type = kythira::node<persistent_raft_types>;
using simulator_type =
    network_simulator::NetworkSimulator<persistent_raft_types::raft_network_types>;

constexpr std::uint64_t node_id = 1;
constexpr std::chrono::milliseconds commit_timeout{2000};
const kythira::key_range group_range{.start = "g1/", .end = "g10"};

auto make_config() -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{80};
    cfg._election_timeout_max = std::chrono::milliseconds{160};
    cfg._heartbeat_interval = std::chrono::milliseconds{26};
    cfg._rpc_timeout = std::chrono::milliseconds{80};
    return cfg;
}

// Each command is "kNN=vNN", 7 bytes; a threshold of one command's bytes
// makes every apply batch try to snapshot.
auto make_compacting_config() -> kythira::raft_configuration {
    auto cfg = make_config();
    cfg._persistent_log_compaction = true;
    cfg._snapshot_threshold_bytes = 7;
    cfg._snapshot_chunk_size = 1;
    return cfg;
}

struct harness {
    simulator_type sim;
    shared_persistence persistence;
    std::shared_ptr<kythira::memory_kv_engine> engine =
        std::make_shared<kythira::memory_kv_engine>();
    std::shared_ptr<kythira::testing::metric_recorder> metrics =
        std::make_shared<kythira::testing::metric_recorder>();

    harness() { sim.start(); }

    auto make_node(const kythira::raft_configuration& cfg) -> std::unique_ptr<node_type> {
        auto net = sim.create_node(std::to_string(node_id));
        register_sm sm;
        sm.open(*engine, group_range);
        auto serializer = persistent_raft_types::serializer_type{};
        return std::make_unique<node_type>(kythira::node_config<persistent_raft_types>{
            .node_id = node_id,
            .network_client = persistent_raft_types::network_client_type{net, serializer},
            .network_server = persistent_raft_types::network_server_type{net, serializer},
            .persistence = persistence,
            .logger = kythira::console_logger{kythira::log_level::error},
            .metrics = kythira::testing::recording_metrics{metrics},
            .membership = kythira::default_membership_manager<std::uint64_t>{},
            .state_machine = std::move(sm),
            .config = cfg,
        });
    }
};

auto elect(node_type& node, const kythira::raft_configuration& cfg) -> void {
    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
}

auto bytes(const std::string& s) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(s.data()),
            reinterpret_cast<const std::byte*>(s.data() + s.size())};
}

auto key(int i) -> std::string {
    return std::string("k") + (i < 10 ? "0" : "") + std::to_string(i);
}

auto put(node_type& node, int i) -> void {
    const auto v = std::string("v") + (i < 10 ? "0" : "") + std::to_string(i);
    std::move(node.submit_command(bytes(key(i) + "=" + v), commit_timeout)).get();
}

// The register's value, or empty when it is unset.
auto value_of(node_type& node, int i) -> std::string {
    return node.with_state_machine(
        [i](const register_sm& sm) { return sm.value_of(key(i)).value_or(""); });
}

auto applies(node_type& node) -> std::size_t {
    return node.with_state_machine([](const register_sm& sm) { return sm.applies(); });
}

auto marker() -> std::vector<std::byte> {
    return bytes("kythira-persistent-snapshot/1");
}

}  // namespace

// Requirement 5.1: a linearizable read confirms the read index and returns
// no bytes; the data is read through with_state_machine.
BOOST_AUTO_TEST_CASE(read_state_is_a_barrier_without_bytes, *boost::unit_test::timeout(30)) {
    harness h;
    const auto cfg = make_config();
    auto node = h.make_node(cfg);
    node->start();
    elect(*node, cfg);
    BOOST_REQUIRE(node->is_leader());

    for (int i = 1; i <= 5; ++i) {
        put(*node, i);
    }
    const auto state = std::move(node->read_state(commit_timeout)).get();
    BOOST_TEST(state.empty());
    BOOST_TEST(value_of(*node, 5) == "v05");
    node->stop();
}

// Requirement 4.1: a restart opens the engine and starts at its applied
// index, replaying nothing and loading nothing.
BOOST_AUTO_TEST_CASE(restart_resumes_at_applied_index_without_replay,
                     *boost::unit_test::timeout(30)) {
    harness h;
    const auto cfg = make_config();
    std::uint64_t applied = 0;
    {
        auto node = h.make_node(cfg);
        node->start();
        elect(*node, cfg);
        BOOST_REQUIRE(node->is_leader());
        for (int i = 1; i <= 8; ++i) {
            put(*node, i);
        }
        applied = node->last_applied_index();
        node->stop();
    }

    harness restart;
    restart.persistence = h.persistence;
    restart.engine = h.engine;
    auto node = restart.make_node(cfg);
    node->start();
    BOOST_TEST(node->last_applied_index() == applied);
    BOOST_TEST(applies(*node) == 0U);
    BOOST_TEST(value_of(*node, 8) == "v08");

    elect(*node, cfg);
    BOOST_REQUIRE(node->is_leader());
    put(*node, 9);
    BOOST_TEST(value_of(*node, 9) == "v09");
    // Only the new entry (and the leader's no-op, if it writes one) applied.
    BOOST_TEST(applies(*node) <= 2U);
    node->stop();
}

// Requirements 2.2 and 2.4: applies lost in a power cut come back from the
// log. The engine holds durability after entry 4, so 5-8 are lost; the
// restarted node starts at the recovered applied index and replays them.
BOOST_AUTO_TEST_CASE(crash_with_held_back_durability_replays_from_log,
                     *boost::unit_test::timeout(30)) {
    harness h;
    const auto cfg = make_config();
    std::uint64_t durable = 0;
    {
        auto node = h.make_node(cfg);
        node->start();
        elect(*node, cfg);
        BOOST_REQUIRE(node->is_leader());
        for (int i = 1; i <= 4; ++i) {
            put(*node, i);
        }
        durable = node->last_applied_index();
        h.engine->hold_durability();
        for (int i = 5; i <= 8; ++i) {
            put(*node, i);
        }
        node->stop();
    }
    h.engine->simulate_crash();
    h.engine->release_durability();

    harness restart;
    restart.persistence = h.persistence;
    restart.engine = h.engine;
    auto node = restart.make_node(cfg);
    node->start();
    BOOST_TEST(node->last_applied_index() == durable);
    BOOST_TEST(value_of(*node, 5).empty());

    // The entries after the crash point are committed; the new leader's
    // first commit makes the node apply them again.
    elect(*node, cfg);
    BOOST_REQUIRE(node->is_leader());
    put(*node, 9);
    for (int i = 1; i <= 9; ++i) {
        BOOST_TEST_CONTEXT("key " << i) {
            BOOST_TEST(!value_of(*node, i).empty());
        }
    }
    node->stop();
}

// Requirement 10.2: compaction is off by default, so the log is kept however
// much is applied.
BOOST_AUTO_TEST_CASE(compaction_is_off_by_default, *boost::unit_test::timeout(30)) {
    harness h;
    auto cfg = make_compacting_config();
    cfg._persistent_log_compaction = false;
    auto node = h.make_node(cfg);
    node->start();
    elect(*node, cfg);
    BOOST_REQUIRE(node->is_leader());
    for (int i = 1; i <= 6; ++i) {
        put(*node, i);
    }
    node->create_snapshot();
    BOOST_TEST(!h.persistence.load_snapshot().has_value());
    BOOST_TEST(h.persistence.get_log_entry(1).has_value());
    // Requirement 3.4: the lag metric is reported anyway.
    BOOST_TEST(h.metrics->count_named("raft_persistent_apply_lag") > 0U);
    node->stop();
}

// Requirements 3.1 and 3.2: with compaction on, the snapshot is metadata at
// the durable applied index, never past it, and the log above it survives.
BOOST_AUTO_TEST_CASE(compaction_stops_at_durable_applied_index, *boost::unit_test::timeout(30)) {
    harness h;
    const auto cfg = make_compacting_config();
    auto node = h.make_node(cfg);
    node->start();
    elect(*node, cfg);
    BOOST_REQUIRE(node->is_leader());

    for (int i = 1; i <= 4; ++i) {
        put(*node, i);
    }
    const auto durable = node->last_applied_index();
    auto snap = h.persistence.load_snapshot();
    BOOST_REQUIRE(snap.has_value());
    BOOST_TEST(snap->last_included_index() == durable);
    BOOST_TEST((snap->state_machine_state() == marker()));
    BOOST_TEST(snap->configuration().nodes() == std::vector<std::uint64_t>{node_id});

    h.engine->hold_durability();
    for (int i = 5; i <= 8; ++i) {
        put(*node, i);
    }
    const auto applied = node->last_applied_index();
    BOOST_REQUIRE(applied > durable);
    snap = h.persistence.load_snapshot();
    BOOST_REQUIRE(snap.has_value());
    BOOST_TEST(snap->last_included_index() == durable);
    for (auto i = durable + 1; i <= applied; ++i) {
        BOOST_TEST_CONTEXT("entry " << i) {
            BOOST_TEST(h.persistence.get_log_entry(i).has_value());
        }
    }

    // Durability catches up; the next snapshot follows it.
    h.engine->release_durability();
    put(*node, 9);
    snap = h.persistence.load_snapshot();
    BOOST_REQUIRE(snap.has_value());
    BOOST_TEST(snap->last_included_index() == node->last_applied_index());
    BOOST_TEST(!h.persistence.get_log_entry(applied).has_value());
    node->stop();
}

// Requirement 3.3: past the configured lag, compaction syncs the engine first
// (when the state machine offers sync_durable()) and then compacts through
// everything applied.
BOOST_AUTO_TEST_CASE(compaction_syncs_past_lag_bound, *boost::unit_test::timeout(30)) {
    harness h;
    auto cfg = make_compacting_config();
    cfg._persistent_sync_lag_entries = 2;
    auto node = h.make_node(cfg);
    node->start();
    elect(*node, cfg);
    BOOST_REQUIRE(node->is_leader());
    h.engine->hold_durability();
    for (int i = 1; i <= 6; ++i) {
        put(*node, i);
    }
    BOOST_TEST(node->with_state_machine([](const register_sm& sm) { return sm.syncs(); }) > 0U);
    const auto snap = h.persistence.load_snapshot();
    BOOST_REQUIRE(snap.has_value());
    BOOST_TEST(snap->last_included_index() > 2U);
    node->stop();
}

// Requirement 4.1 after compaction: a restart uses the saved metadata for
// the snapshot index and term and the engine for the applied index.
BOOST_AUTO_TEST_CASE(restart_after_compaction_resumes, *boost::unit_test::timeout(30)) {
    harness h;
    const auto cfg = make_compacting_config();
    std::uint64_t applied = 0;
    {
        auto node = h.make_node(cfg);
        node->start();
        elect(*node, cfg);
        BOOST_REQUIRE(node->is_leader());
        for (int i = 1; i <= 6; ++i) {
            put(*node, i);
        }
        applied = node->last_applied_index();
        node->stop();
    }
    BOOST_REQUIRE(h.persistence.load_snapshot().has_value());

    harness restart;
    restart.persistence = h.persistence;
    restart.engine = h.engine;
    auto node = restart.make_node(cfg);
    node->start();
    BOOST_TEST(node->last_applied_index() == applied);
    elect(*node, cfg);
    BOOST_REQUIRE(node->is_leader());
    put(*node, 7);
    BOOST_TEST(value_of(*node, 7) == "v07");
    BOOST_TEST(value_of(*node, 1) == "v01");
    node->stop();
}

// Requirement 4.2: state older than the snapshot cannot be brought forward,
// because the entries between were compacted away. The node refuses to start.
BOOST_AUTO_TEST_CASE(restart_refuses_state_older_than_snapshot, *boost::unit_test::timeout(30)) {
    harness h;
    h.persistence.save_snapshot(persistent_raft_types::snapshot_type{
        ._last_included_index = 5,
        ._last_included_term = 1,
        ._configuration =
            kythira::cluster_configuration<std::uint64_t>{
                ._nodes = {node_id}, ._is_joint_consensus = false, ._old_nodes = std::nullopt},
        ._state_machine_state = marker()});
    auto node = h.make_node(make_config());
    BOOST_CHECK_THROW(node->start(), std::runtime_error);
}

// A byte-buffer snapshot in the store is not taken for persistent metadata.
BOOST_AUTO_TEST_CASE(restart_refuses_byte_buffer_snapshot, *boost::unit_test::timeout(30)) {
    harness h;
    h.persistence.save_snapshot(persistent_raft_types::snapshot_type{
        ._last_included_index = 0,
        ._last_included_term = 0,
        ._configuration =
            kythira::cluster_configuration<std::uint64_t>{
                ._nodes = {node_id}, ._is_joint_consensus = false, ._old_nodes = std::nullopt},
        ._state_machine_state = bytes("counter-state")});
    auto node = h.make_node(make_config());
    BOOST_CHECK_THROW(node->start(), std::runtime_error);
}

// Requirement 10.2: InstallSnapshot is refused with format_mismatch and the
// state is left as it was.
BOOST_AUTO_TEST_CASE(install_snapshot_is_refused, *boost::unit_test::timeout(30)) {
    harness h;
    const auto cfg = make_config();
    auto node = h.make_node(cfg);
    node->start();
    elect(*node, cfg);
    BOOST_REQUIRE(node->is_leader());
    put(*node, 1);
    const auto applied = node->last_applied_index();

    auto peer = h.sim.create_node("9");
    const network_simulator::NetworkEdge edge{std::chrono::milliseconds{0}, 1.0};
    h.sim.add_edge("9", "1", edge);
    h.sim.add_edge("1", "9", edge);
    persistent_raft_types::network_client_type client{peer,
                                                      persistent_raft_types::serializer_type{}};

    kythira::install_snapshot_request<> req;
    req._term = node->get_current_term() + 1;
    req._leader_id = 9;
    req._last_included_index = applied + 10;
    req._last_included_term = req._term;
    req._offset = 0;
    req._data = bytes("anything");
    req._done = true;
    const auto resp = client.send_install_snapshot(1, req, std::chrono::milliseconds{2000}).get();
    BOOST_REQUIRE(resp.reject_reason().has_value());
    BOOST_TEST((*resp.reject_reason() == kythira::install_snapshot_reject_reason::format_mismatch));
    BOOST_TEST(node->last_applied_index() == applied);
    BOOST_TEST(value_of(*node, 1) == "v01");
    node->stop();
}
