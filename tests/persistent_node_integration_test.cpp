// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// `node<Types>` over a persistent state machine
// (.kiro/specs/persistent-state-machine/ task 3): restart without reloading
// state (Requirement 4), metadata-only snapshots and compaction bound by the
// durable applied index (3), reads that serialize nothing (5), crash and
// replay with a held-back durable sequence (2.2, 2.4), and format-1
// InstallSnapshot received, staged and ingested
// (.kiro/specs/file-install-snapshot/ task 4).
//
// Built twice: on `memory_kv_engine`, and with
// KYTHIRA_PERSISTENT_TEST_ROCKSDB as persistent_node_rocksdb_test on
// `rocksdb_kv_engine` (file-install-snapshot task 7). The RocksDB build
// leaves out the cases that hold durability back or simulate a crash,
// which only the memory engine can do, and adds the 1 GB catch-up case.

#define BOOST_TEST_MODULE persistent_node_integration_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/raft.hpp>

#include "format1_snapshot_transfer.hpp"
#include "persistent_register_state_machine.hpp"
#include "recording_metrics.hpp"

#if defined(FIU_ENABLE)
#include <fiu-control.h>
#endif
#if defined(KYTHIRA_PERSISTENT_TEST_ROCKSDB)
#include <raft/rocksdb_kv_engine.hpp>

#include <malloc.h>
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
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
#if defined(KYTHIRA_PERSISTENT_TEST_ROCKSDB)
using kv_engine_t = kythira::rocksdb_kv_engine;
#else
using kv_engine_t = kythira::memory_kv_engine;
#endif
using register_sm = kythira::testing::basic_persistent_register_state_machine<kv_engine_t>;

// A fresh, empty engine. A RocksDB one lives in its own temp directory,
// removed after the engine closes.
auto make_kv_engine() -> std::shared_ptr<kv_engine_t> {
#if defined(KYTHIRA_PERSISTENT_TEST_ROCKSDB)
    struct holder {
        kythira::test::format1::temp_dir dir;
        kv_engine_t engine{dir.path() / "db"};
    };
    auto h = std::make_shared<holder>();
    return {h, &h->engine};
#else
    return std::make_shared<kv_engine_t>();
#endif
}

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
    std::shared_ptr<kv_engine_t> engine = make_kv_engine();
    std::shared_ptr<kythira::testing::metric_recorder> metrics =
        std::make_shared<kythira::testing::metric_recorder>();

    kythira::test::format1::temp_dir staging;

    harness() { sim.start(); }

    // A client on simulator node "9", standing in for another replica's
    // leader.
    auto peer_client() -> persistent_raft_types::network_client_type {
        auto peer = sim.create_node("9");
        const network_simulator::NetworkEdge edge{std::chrono::milliseconds{0}, 1.0};
        sim.add_edge("9", "1", edge);
        sim.add_edge("1", "9", edge);
        return persistent_raft_types::network_client_type{peer,
                                                          persistent_raft_types::serializer_type{}};
    }

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

// A leader-side checkpoint: registers k01..kNN applied at indices 1..N on
// another engine, checkpointed at index N, as format-1 requests.
class source {
public:
    explicit source(int n) {
        _sm.open(*_engine, group_range);
        for (int i = 1; i <= n; ++i) {
            const auto v = std::string("v") + (i < 10 ? "0" : "") + std::to_string(i);
            (void)_sm.apply(bytes(key(i) + "=" + v), static_cast<std::uint64_t>(i));
        }
        _index = static_cast<std::uint64_t>(n);
        _manifest = _sm.checkpoint(dir());
    }

    [[nodiscard]] auto dir() const -> std::filesystem::path { return _root.path() / "cp"; }
    [[nodiscard]] auto manifest() const -> const kythira::snapshot_manifest& { return _manifest; }
    [[nodiscard]] auto total_bytes() const -> std::uint64_t { return _manifest.total_bytes(); }

    [[nodiscard]] auto requests(std::uint64_t leader_term, std::size_t chunk) const
        -> std::vector<kythira::install_snapshot_request<>> {
        const kythira::snapshot_stream_reader reader(dir(), _manifest);
        const auto encoded = _manifest.encode();
        std::vector<kythira::install_snapshot_request<>> out;
        std::uint64_t offset = 0;
        do {
            kythira::install_snapshot_request<> req;
            req._term = leader_term;
            req._leader_id = 9;
            req._last_included_index = _index;
            req._last_included_term = 1;
            req._offset = static_cast<std::size_t>(offset);
            req._data = reader.read(offset, chunk);
            req._format = kythira::install_snapshot_format::file_set;
            if (offset == 0) {
                req._manifest = bytes(encoded);
            }
            offset += req._data.size();
            req._done = offset == reader.total_bytes();
            out.push_back(std::move(req));
        } while (offset < reader.total_bytes());
        return out;
    }

private:
    kythira::test::format1::temp_dir _root;
    std::shared_ptr<kv_engine_t> _engine = make_kv_engine();
    register_sm _sm;
    std::uint64_t _index{0};
    kythira::snapshot_manifest _manifest;
};

auto send(persistent_raft_types::network_client_type& client,
          const kythira::install_snapshot_request<>& req)
    -> kythira::install_snapshot_response<std::uint64_t> {
    return client.send_install_snapshot(1, req, std::chrono::milliseconds{2000}).get();
}

auto reason_of(const kythira::install_snapshot_response<std::uint64_t>& resp)
    -> std::optional<kythira::install_snapshot_reject_reason> {
    return resp.reject_reason();
}

auto staging_entries(const harness& h) -> std::size_t {
    std::size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(h.staging.path())) {
        n += e.path().filename().string().starts_with("kythira-staging-") ? 1 : 0;
    }
    return n;
}

// Three persistent replicas on the simulator, each with its own engine, log
// store and staging directory. Every store starts from a metadata snapshot
// at index 0 naming all three, so no bootstrap is needed.
struct persistent_cluster {
    struct member {
        shared_persistence persistence;
        std::shared_ptr<kv_engine_t> engine = make_kv_engine();
        std::shared_ptr<kythira::testing::metric_recorder> metrics =
            std::make_shared<kythira::testing::metric_recorder>();
        kythira::test::format1::temp_dir staging;
        std::unique_ptr<node_type> node;
    };

    simulator_type sim;
    std::vector<std::unique_ptr<member>> members;

    explicit persistent_cluster(const kythira::raft_configuration& base) {
        sim.start();
        connect_all();
        for (std::uint64_t id = 1; id <= 3; ++id) {
            auto m = std::make_unique<member>();
            m->persistence.save_snapshot(persistent_raft_types::snapshot_type{
                ._last_included_index = 0,
                ._last_included_term = 0,
                ._configuration =
                    kythira::cluster_configuration<std::uint64_t>{._nodes = {1, 2, 3},
                                                                  ._is_joint_consensus = false,
                                                                  ._old_nodes = std::nullopt},
                ._state_machine_state = marker()});
            auto cfg = base;
            cfg._snapshot_staging_directory = m->staging.path();
            auto net = sim.create_node(std::to_string(id));
            register_sm sm;
            sm.open(*m->engine, group_range);
            auto serializer = persistent_raft_types::serializer_type{};
            m->node = std::make_unique<node_type>(kythira::node_config<persistent_raft_types>{
                .node_id = id,
                .network_client = persistent_raft_types::network_client_type{net, serializer},
                .network_server = persistent_raft_types::network_server_type{net, serializer},
                .persistence = m->persistence,
                .logger = kythira::console_logger{kythira::log_level::error},
                .metrics = kythira::testing::recording_metrics{m->metrics},
                .membership = kythira::default_membership_manager<std::uint64_t>{},
                .state_machine = std::move(sm),
                .config = cfg,
            });
            members.push_back(std::move(m));
        }
        for (auto& m : members) {
            m->node->start();
        }
        std::this_thread::sleep_for(base._election_timeout_max + std::chrono::milliseconds{30});
        leader().check_election_timeout();
    }
    ~persistent_cluster() {
        for (auto& m : std::views::reverse(members)) {
            m->node->stop();
        }
    }
    persistent_cluster(const persistent_cluster&) = delete;
    auto operator=(const persistent_cluster&) -> persistent_cluster& = delete;
    persistent_cluster(persistent_cluster&&) = delete;
    auto operator=(persistent_cluster&&) -> persistent_cluster& = delete;

    auto leader() -> node_type& { return *members[0]->node; }
    auto at(std::size_t i) -> node_type& { return *members[i]->node; }

    auto connect_all() -> void {
        const network_simulator::NetworkEdge edge{std::chrono::milliseconds{0}, 1.0};
        for (const auto* from : {"1", "2", "3"}) {
            for (const auto* to : {"1", "2", "3"}) {
                if (std::string(from) != to) {
                    sim.add_edge(from, to, edge);
                }
            }
        }
    }
    auto cut(const std::string& id) -> void {
        for (const auto* peer : {"1", "2", "3"}) {
            if (peer != id) {
                sim.remove_edge(peer, id);
                sim.remove_edge(id, peer);
            }
        }
    }

    // Drives heartbeats until `pred` holds or the deadline passes.
    template<typename Pred>
    auto pump_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{5000})
        -> bool {
        const auto end = std::chrono::steady_clock::now() + deadline;
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end) {
                return false;
            }
            leader().check_heartbeat_timeout();
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return true;
    }

    // Submits register i and reports whether it committed.
    auto commit(int i) -> bool {
        const auto v = std::string("v") + (i < 10 ? "0" : "") + std::to_string(i);
        return submit(key(i) + "=" + v);
    }

    // Submits one `key=value` command and reports whether it committed
    // within `timeout`.
    auto submit(const std::string& command,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) -> bool {
        auto done = std::make_shared<std::atomic<int>>(0);
        leader()
            .submit_command(bytes(command), timeout)
            .thenValue([done](std::vector<std::byte>) { *done = 1; })
            .thenError([done](const std::exception_ptr&) { *done = 2; })
            .detach();
        pump_until([&] { return done->load() != 0; }, timeout + std::chrono::milliseconds{2000});
        return done->load() == 1;
    }
};
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

#if !defined(KYTHIRA_PERSISTENT_TEST_ROCKSDB)  // memory engine: held-back durability
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

#endif

// file-install-snapshot Requirement 7.1: compaction is on by default. Turned
// off, the log is kept however much is applied.
BOOST_AUTO_TEST_CASE(compaction_is_on_by_default_and_can_be_turned_off,
                     *boost::unit_test::timeout(30)) {
    BOOST_TEST(kythira::raft_configuration{}.persistent_log_compaction());
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

#if !defined(KYTHIRA_PERSISTENT_TEST_ROCKSDB)  // memory engine: held-back durability
// Requirements 3.1 and 3.2: with compaction on, the snapshot is metadata at
// the durable applied index, never past it, and the log above it survives.
BOOST_AUTO_TEST_CASE(compaction_stops_at_durable_applied_index, *boost::unit_test::timeout(30)) {
    harness h;
    auto cfg = make_compacting_config();
    cfg._persistent_sync_lag_entries = 0;  // never sync: the test holds durability
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
#endif

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

// file-install-snapshot Requirement 4.3: a format-0 InstallSnapshot is
// refused with format_mismatch and the state is left as it was.
BOOST_AUTO_TEST_CASE(format0_install_snapshot_is_refused, *boost::unit_test::timeout(30)) {
    harness h;
    const auto cfg = make_config();
    auto node = h.make_node(cfg);
    node->start();
    elect(*node, cfg);
    BOOST_REQUIRE(node->is_leader());
    put(*node, 1);
    const auto applied = node->last_applied_index();

    auto peer = h.peer_client();
    kythira::install_snapshot_request<> req;
    req._term = node->get_current_term() + 1;
    req._leader_id = 9;
    req._last_included_index = applied + 10;
    req._last_included_term = req._term;
    req._offset = 0;
    req._data = bytes("anything");
    req._done = true;
    const auto resp = send(peer, req);
    BOOST_REQUIRE(resp.reject_reason().has_value());
    BOOST_TEST((*resp.reject_reason() == kythira::install_snapshot_reject_reason::format_mismatch));
    BOOST_TEST(node->last_applied_index() == applied);
    BOOST_TEST(value_of(*node, 1) == "v01");
    node->stop();
}

// ── Format-1 receive and install (.kiro/specs/file-install-snapshot/ task 4) ──

// Requirements 3.2, 3.3 and 4.1: a multi-chunk format-1 transfer is staged,
// verified and ingested; the node saves a metadata-only snapshot, deletes
// the staging directory, and a restart starts from the ingested state.
BOOST_AUTO_TEST_CASE(format1_snapshot_installs_and_survives_restart,
                     *boost::unit_test::timeout(30)) {
    harness h;
    source src(20);
    auto cfg = make_config();
    cfg._snapshot_staging_directory = h.staging.path();
    auto node = h.make_node(cfg);
    node->start();
    auto peer = h.peer_client();

    const auto reqs = src.requests(/*leader_term=*/1, /*chunk=*/64);
    BOOST_REQUIRE_GT(reqs.size(), 3U);
    for (std::size_t i = 0; i + 1 < reqs.size(); ++i) {
        const auto resp = send(peer, reqs[i]);
        BOOST_TEST(!resp.reject_reason().has_value());
        BOOST_REQUIRE(resp.bytes_received().has_value());
        BOOST_TEST(*resp.bytes_received() == reqs[i]._offset + reqs[i]._data.size());
        BOOST_TEST(staging_entries(h) == 1U);
    }
    const auto last = send(peer, reqs.back());
    BOOST_TEST(!last.reject_reason().has_value());
    BOOST_TEST((last.bytes_received() == std::optional<std::uint64_t>(src.total_bytes())));

    BOOST_TEST(node->last_applied_index() == 20U);
    BOOST_TEST(value_of(*node, 7) == "v07");
    BOOST_TEST(value_of(*node, 20) == "v20");
    BOOST_TEST(staging_entries(h) == 0U);
    const auto saved = h.persistence.load_snapshot();
    BOOST_REQUIRE(saved.has_value());
    BOOST_TEST(saved->last_included_index() == 20U);
    BOOST_TEST((saved->state_machine_state() == marker()));
    node->stop();
    node.reset();

    auto restarted = h.make_node(cfg);
    restarted->start();
    BOOST_TEST(restarted->last_applied_index() == 20U);
    BOOST_TEST(value_of(*restarted, 13) == "v13");
    restarted->stop();
}

// Requirement 3.1 and the reject reasons that become response codes: every
// refusal says why, and a poisoned staging directory is deleted.
BOOST_AUTO_TEST_CASE(format1_rejections_carry_reason_codes, *boost::unit_test::timeout(30)) {
    using reason = kythira::install_snapshot_reject_reason;
    harness h;
    source src(20);
    auto cfg = make_config();
    cfg._snapshot_staging_directory = h.staging.path();
    auto node = h.make_node(cfg);
    node->start();
    auto peer = h.peer_client();
    const auto reqs = src.requests(1, 64);
    BOOST_REQUIRE_GT(reqs.size(), 3U);

    // A later chunk with no transfer staged.
    BOOST_TEST((reason_of(send(peer, reqs[1])) == reason::missing_first_chunk));

    // A manifest that does not decode.
    auto bad = reqs[0];
    bad._manifest = bytes("not a manifest");
    BOOST_TEST((reason_of(send(peer, bad)) == reason::manifest_invalid));
    BOOST_TEST(staging_entries(h) == 0U);

    // A gap in the stream keeps the transfer and reports how far it got.
    BOOST_TEST(!reason_of(send(peer, reqs[0])).has_value());
    const auto gap = send(peer, reqs[2]);
    BOOST_TEST((reason_of(gap) == reason::offset_mismatch));
    BOOST_TEST((gap.bytes_received() == std::optional<std::uint64_t>(reqs[0]._data.size())));
    BOOST_TEST(staging_entries(h) == 1U);

    // Corrupt bytes fail the file's SHA-256 when it completes; the staged
    // files are deleted.
    for (std::size_t i = 1; i + 1 < reqs.size(); ++i) {
        BOOST_TEST(!reason_of(send(peer, reqs[i])).has_value());
    }
    auto corrupt = reqs.back();
    corrupt._data.back() ^= std::byte{0x01};
    BOOST_TEST((reason_of(send(peer, corrupt)) == reason::checksum));
    BOOST_TEST(staging_entries(h) == 0U);
    BOOST_TEST(node->last_applied_index() == 0U);

    // A format-0 request (Requirement 4.3).
    auto format0 = reqs[0];
    format0._format = kythira::install_snapshot_format::byte_buffer;
    format0._manifest.clear();
    BOOST_TEST((reason_of(send(peer, format0)) == reason::format_mismatch));

    // A complete transfer installs; the same snapshot again is stale
    // (Requirement 3.6), refused before anything is staged.
    for (const auto& r : reqs) {
        BOOST_TEST(!reason_of(send(peer, r)).has_value());
    }
    BOOST_TEST(node->last_applied_index() == 20U);
    BOOST_TEST((reason_of(send(peer, reqs[0])) == reason::stale_snapshot));
    BOOST_TEST(staging_entries(h) == 0U);

    // An older leader term.
    auto old_term = src.requests(1, 64);
    for (auto& r : old_term) {
        r._term = 0;
    }
    BOOST_TEST((reason_of(send(peer, old_term[0])) == reason::stale_term));
    node->stop();
}

// Requirement 3.1: no staging directory configured, or not enough disk.
BOOST_AUTO_TEST_CASE(format1_needs_a_staging_directory_and_disk_space,
                     *boost::unit_test::timeout(30)) {
    using reason = kythira::install_snapshot_reject_reason;
    source src(5);
    {
        harness h;
        auto node = h.make_node(make_config());
        node->start();
        auto peer = h.peer_client();
        BOOST_TEST((reason_of(send(peer, src.requests(1, 64)[0])) == reason::io_error));
        node->stop();
    }
    {
        harness h;
        auto cfg = make_config();
        cfg._snapshot_staging_directory = h.staging.path();
        cfg._snapshot_disk_margin_bytes = std::numeric_limits<std::uint64_t>::max() / 2;
        auto node = h.make_node(cfg);
        node->start();
        auto peer = h.peer_client();
        BOOST_TEST((reason_of(send(peer, src.requests(1, 64)[0])) == reason::disk_space));
        BOOST_TEST(staging_entries(h) == 0U);
        node->stop();
    }
}

// Requirement 3.4: a newer transfer (a later snapshot, or a new leader)
// replaces the staged one, whose chunks are then refused.
BOOST_AUTO_TEST_CASE(newer_transfer_abandons_the_staged_one, *boost::unit_test::timeout(30)) {
    using reason = kythira::install_snapshot_reject_reason;
    harness h;
    source older(10);
    source newer(30);
    auto cfg = make_config();
    cfg._snapshot_staging_directory = h.staging.path();
    auto node = h.make_node(cfg);
    node->start();
    auto peer = h.peer_client();

    const auto a = older.requests(1, 64);
    const auto b = newer.requests(1, 64);
    BOOST_TEST(!reason_of(send(peer, a[0])).has_value());
    BOOST_TEST(!reason_of(send(peer, b[0])).has_value());
    BOOST_TEST(staging_entries(h) == 1U);
    // The older transfer's chunks, first or later, are stale now.
    BOOST_TEST((reason_of(send(peer, a[1])) == reason::stale_snapshot));
    BOOST_TEST((reason_of(send(peer, a[0])) == reason::stale_snapshot));

    // A new leader's transfer replaces it even at a lower index.
    const auto c = older.requests(2, 64);
    BOOST_TEST(!reason_of(send(peer, c[0])).has_value());
    BOOST_TEST(staging_entries(h) == 1U);
    BOOST_TEST((reason_of(send(peer, b[1])) == reason::stale_term));
    for (std::size_t i = 1; i < c.size(); ++i) {
        BOOST_TEST(!reason_of(send(peer, c[i])).has_value());
    }
    BOOST_TEST(node->last_applied_index() == 10U);
    BOOST_TEST(value_of(*node, 10) == "v10");
    BOOST_TEST(value_of(*node, 11).empty());
    node->stop();
}

// Requirement 3.5: staging directories a crash left are deleted at start;
// anything else in the directory is not the node's and stays.
BOOST_AUTO_TEST_CASE(stale_staging_directories_are_deleted_at_start,
                     *boost::unit_test::timeout(30)) {
    harness h;
    std::filesystem::create_directories(h.staging.path() / "kythira-staging-20-1-0011223344556677");
    std::ofstream(h.staging.path() / "kythira-staging-20-1-0011223344556677" / "part") << "x";
    std::filesystem::create_directories(h.staging.path() / "someone-else");
    auto cfg = make_config();
    cfg._snapshot_staging_directory = h.staging.path();
    auto node = h.make_node(cfg);
    node->start();
    BOOST_TEST(staging_entries(h) == 0U);
    BOOST_TEST(std::filesystem::exists(h.staging.path() / "someone-else"));
    node->stop();
}

// Requirement 4.2: a crash after ingest and before the metadata save
// leaves the engine at the snapshot's index and the store at the old
// snapshot. Restart takes the engine's applied index.
BOOST_AUTO_TEST_CASE(crash_between_ingest_and_metadata_save_recovers,
                     *boost::unit_test::timeout(30)) {
    harness h;
    source src(12);
    {
        // What the follower's ingest wrote before the crash.
        register_sm sm;
        sm.open(*h.engine, group_range);
        sm.ingest(src.dir(), src.manifest(), 12);
    }
    auto node = h.make_node(make_config());
    node->start();
    BOOST_TEST(node->last_applied_index() == 12U);
    BOOST_TEST(value_of(*node, 12) == "v12");
    BOOST_TEST(!h.persistence.load_snapshot().has_value());
    node->stop();
}

// ── Format-1 send (.kiro/specs/file-install-snapshot/ task 5) ──

// Requirements 5.1-5.3: a follower cut off while the leader compacts past
// it catches up through a format-1 snapshot streamed from a checkpoint,
// then by AppendEntries. The checkpoint is kept, unused, for reuse; the
// follower's staging directory is empty afterwards.
BOOST_AUTO_TEST_CASE(follower_behind_compaction_catches_up_by_format1_snapshot,
                     *boost::unit_test::timeout(60)) {
    auto cfg = make_config();
    cfg._persistent_log_compaction = true;
    cfg._snapshot_threshold_bytes = 7 * 4;  // about four puts
    cfg._snapshot_chunk_size = 16;
    cfg._install_snapshot_timeout = std::chrono::milliseconds{300};
    persistent_cluster c(cfg);
    BOOST_REQUIRE(c.pump_until([&] { return c.leader().is_leader(); }));

    BOOST_REQUIRE(c.commit(1));
    BOOST_REQUIRE(c.pump_until([&] { return value_of(c.at(2), 1) == "v01"; }));
    const auto follower_last = c.at(2).last_applied_index();

    c.cut("3");
    constexpr int writes = 12;
    for (int i = 2; i < 2 + writes; ++i) {
        BOOST_REQUIRE(c.commit(i));
    }
    const auto leader_last = c.leader().last_applied_index();
    const auto retained = c.leader().log_entries_between(1, leader_last);
    BOOST_REQUIRE(!retained.empty());
    BOOST_REQUIRE_GT(retained.front().index(), follower_last + 1);

    c.connect_all();
    BOOST_REQUIRE(c.commit(50));
    BOOST_CHECK(c.pump_until([&] { return value_of(c.at(2), 50) == "v50"; }));
    for (int i = 2; i < 2 + writes; ++i) {
        BOOST_CHECK_MESSAGE(!value_of(c.at(2), i).empty(),
                            key(i) << " missing on the follower that caught up by snapshot");
    }
    BOOST_TEST(c.members[0]->metrics->count_named("raft_install_snapshot_success") >= 1U);
    const auto installed = c.members[2]->persistence.load_snapshot();
    BOOST_REQUIRE(installed.has_value());
    BOOST_TEST(installed->last_included_index() > follower_last);
    BOOST_TEST((installed->state_machine_state() == marker()));

    const auto entries_named = [](const std::filesystem::path& dir, std::string_view prefix) {
        std::size_t n = 0;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            n += e.path().filename().string().starts_with(prefix) ? 1 : 0;
        }
        return n;
    };
    BOOST_TEST(entries_named(c.members[2]->staging.path(), "kythira-staging-") == 0U);
    // Superseded checkpoints are deleted as newer ones are taken.
    BOOST_TEST(entries_named(c.members[0]->staging.path(), "kythira-checkpoint-") <= 2U);
    BOOST_TEST(c.members[0]->metrics->count_named("raft_snapshot_checkpoints") >= 1U);
}

// ── Resume and pacing (.kiro/specs/file-install-snapshot/ task 6) ──

// Requirements 6.1 and 6.2: the same checkpoint sent again from offset 0
// is answered with offset_mismatch and how far it is staged, and the
// transfer goes on from there. A different checkpoint at the same index
// replaces it.
BOOST_AUTO_TEST_CASE(follower_reports_staged_progress_for_the_same_checkpoint,
                     *boost::unit_test::timeout(30)) {
    using reason = kythira::install_snapshot_reject_reason;
    harness h;
    source src(20);
    auto cfg = make_config();
    cfg._snapshot_staging_directory = h.staging.path();
    auto node = h.make_node(cfg);
    node->start();
    auto peer = h.peer_client();
    const auto reqs = src.requests(1, 64);
    BOOST_REQUIRE_GT(reqs.size(), 3U);

    for (std::size_t i = 0; i < 3; ++i) {
        BOOST_TEST(!reason_of(send(peer, reqs[i])).has_value());
    }
    const auto staged = reqs[2]._offset + reqs[2]._data.size();
    const auto again = send(peer, reqs[0]);
    BOOST_TEST((reason_of(again) == reason::offset_mismatch));
    BOOST_TEST((again.bytes_received() == std::optional<std::uint64_t>(staged)));
    BOOST_TEST(h.metrics->count_named("raft_install_snapshot_resumed") == 1U);
    // A new leader with the same checkpoint resumes too.
    auto new_leader = reqs[0];
    new_leader._term = 2;
    BOOST_TEST((send(peer, new_leader).bytes_received() == std::optional<std::uint64_t>(staged)));
    for (std::size_t i = 3; i < reqs.size(); ++i) {
        auto r = reqs[i];
        r._term = 2;
        BOOST_TEST(!reason_of(send(peer, r)).has_value());
    }
    BOOST_TEST(node->last_applied_index() == 20U);
    BOOST_TEST(value_of(*node, 20) == "v20");
    node->stop();
}

// Requirement 6.1 end to end, and 6.3: with the leader paced, a follower
// cut off mid-transfer resumes from what it staged once it is back, and the
// transfer takes at least as long as the pacer allows.
BOOST_AUTO_TEST_CASE(leader_resumes_a_cut_transfer_and_paces_it, *boost::unit_test::timeout(90)) {
    auto cfg = make_config();
    cfg._persistent_log_compaction = true;
    // Thirteen 7-byte puts: the log compacts once, at the last write, so the
    // leader never takes an earlier checkpoint that a slow, paced send would
    // still be busy with when the test cuts the follower.
    cfg._snapshot_threshold_bytes = 7 * 13;
    cfg._snapshot_chunk_size = 16;
    cfg._install_snapshot_timeout = std::chrono::milliseconds{300};
    // One attempt per chunk, so the cut fails the transfer instead of a
    // retry riding it out; the next one must resume.
    cfg._install_snapshot_retry_policy = {.initial_delay = std::chrono::milliseconds{50},
                                          .max_delay = std::chrono::milliseconds{100},
                                          .backoff_multiplier = 2.0,
                                          .jitter_factor = 0.0,
                                          .max_attempts = 1};
    constexpr std::uint64_t rate = 150;  // bytes per second
    cfg._snapshot_pacer = std::make_shared<kythira::snapshot_pacer>(rate, 16);
    persistent_cluster c(cfg);
    BOOST_REQUIRE(c.pump_until([&] { return c.leader().is_leader(); }));
    BOOST_REQUIRE(c.commit(1));
    BOOST_REQUIRE(c.pump_until([&] { return value_of(c.at(2), 1) == "v01"; }));

    c.cut("3");
    for (int i = 2; i < 14; ++i) {
        BOOST_REQUIRE(c.commit(i));
    }
    // The only checkpoint is at the leader's applied index (see the
    // threshold above); cut the follower once it has started staging it.
    const auto wanted = "kythira-staging-" + std::to_string(c.leader().last_applied_index()) + "-";
    c.connect_all();
    const auto started = std::chrono::steady_clock::now();
    const auto staging_has_part = [&] {
        for (const auto& e : std::filesystem::directory_iterator(c.members[2]->staging.path())) {
            if (e.path().filename().string().starts_with(wanted)) {
                return true;
            }
        }
        return false;
    };
    BOOST_REQUIRE(c.pump_until(staging_has_part, std::chrono::milliseconds{10000}));
    c.cut("3");
    std::this_thread::sleep_for(std::chrono::milliseconds{1500});
    c.connect_all();
    BOOST_REQUIRE(c.commit(50));
    BOOST_CHECK(c.pump_until([&] { return value_of(c.at(2), 50) == "v50"; },
                             std::chrono::milliseconds{30000}));
    for (int i = 2; i < 14; ++i) {
        BOOST_CHECK(!value_of(c.at(2), i).empty());
    }
    BOOST_TEST(c.members[2]->metrics->count_named("raft_install_snapshot_resumed") >= 1U);
    const auto installed = c.members[2]->persistence.load_snapshot();
    BOOST_REQUIRE(installed.has_value());
    // Every byte went through the pacer, one 16-byte burst aside.
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto total = c.members[0]->metrics->count_named("raft_install_snapshot_success");
    BOOST_TEST(total >= 1U);
    BOOST_TEST(elapsed >= std::chrono::milliseconds{1500});
}

// ── Faults (.kiro/specs/file-install-snapshot/ task 8) ──

// Requirement 8.2: a follower restarted mid-transfer deletes what it staged
// and takes the transfer again from offset 0, then installs it.
BOOST_AUTO_TEST_CASE(follower_restarted_mid_transfer_starts_again_from_zero,
                     *boost::unit_test::timeout(30)) {
    harness h;
    source src(20);
    auto cfg = make_config();
    cfg._snapshot_staging_directory = h.staging.path();
    auto node = h.make_node(cfg);
    node->start();
    auto peer = h.peer_client();
    const auto reqs = src.requests(1, 64);
    BOOST_REQUIRE_GT(reqs.size(), 3U);
    for (std::size_t i = 0; i < 3; ++i) {
        BOOST_TEST(!reason_of(send(peer, reqs[i])).has_value());
    }
    BOOST_TEST(staging_entries(h) == 1U);
    node->stop();
    node.reset();

    auto restarted = h.make_node(cfg);
    restarted->start();
    BOOST_TEST(staging_entries(h) == 0U);
    // Going on from where the old process was is refused: nothing is staged.
    BOOST_TEST(reason_of(send(peer, reqs[3])).has_value());
    for (const auto& r : reqs) {
        BOOST_TEST(!reason_of(send(peer, r)).has_value());
    }
    BOOST_TEST(h.metrics->count_named("raft_install_snapshot_resumed") == 0U);
    BOOST_TEST(restarted->last_applied_index() == 20U);
    BOOST_TEST(value_of(*restarted, 20) == "v20");
    restarted->stop();
}

// Requirement 8.2: the leader changes while a follower is catching up by
// snapshot. The new leader, which compacted on its own, finishes the job.
BOOST_AUTO_TEST_CASE(leader_change_mid_transfer_still_catches_the_follower_up,
                     *boost::unit_test::timeout(90)) {
    auto cfg = make_config();
    cfg._persistent_log_compaction = true;
    cfg._snapshot_threshold_bytes = 7 * 13;  // one compaction, at the last write
    cfg._snapshot_chunk_size = 16;
    cfg._install_snapshot_timeout = std::chrono::milliseconds{300};
    cfg._snapshot_pacer = std::make_shared<kythira::snapshot_pacer>(150, 16);
    persistent_cluster c(cfg);
    BOOST_REQUIRE(c.pump_until([&] { return c.leader().is_leader(); }));
    BOOST_REQUIRE(c.commit(1));
    BOOST_REQUIRE(c.pump_until([&] { return value_of(c.at(1), 1) == "v01"; }));
    c.cut("3");
    for (int i = 2; i < 14; ++i) {
        BOOST_REQUIRE(c.commit(i));
    }
    BOOST_REQUIRE(c.pump_until([&] { return value_of(c.at(1), 13) == "v13"; }));
    c.connect_all();
    const auto staging_has_part = [&] {
        for (const auto& e : std::filesystem::directory_iterator(c.members[2]->staging.path())) {
            if (e.path().filename().string().starts_with("kythira-staging-")) {
                return true;
            }
        }
        return false;
    };
    BOOST_REQUIRE(c.pump_until(staging_has_part, std::chrono::milliseconds{10000}));

    // Depose node 1: cut it off and let node 2 win an election.
    c.cut("1");
    auto& next = c.at(1);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds{10000};
    while (!next.is_leader() && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{30});
        next.check_election_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    BOOST_REQUIRE(next.is_leader());
    const auto caught_up = [&] { return value_of(c.at(2), 13) == "v13"; };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{60000};
    while (!caught_up() && std::chrono::steady_clock::now() < deadline) {
        next.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    BOOST_REQUIRE(caught_up());
    for (int i = 2; i < 14; ++i) {
        BOOST_CHECK(!value_of(c.at(2), i).empty());
    }
    BOOST_TEST(c.members[2]->persistence.load_snapshot().has_value());
    c.connect_all();
}

#if defined(FIU_ENABLE)
// Requirement 8.1's failpoints. This build (persistent_node_fault_test) has
// FIU_ENABLE and links libfiu.

namespace {

struct fiu_fixture {
    fiu_fixture() { fiu_init(0); }
};

class armed_fault {
public:
    explicit armed_fault(const char* name) : _name(name) { fiu_enable(_name, 1, nullptr, 0); }
    ~armed_fault() { disarm(); }
    armed_fault(const armed_fault&) = delete;
    auto operator=(const armed_fault&) -> armed_fault& = delete;
    armed_fault(armed_fault&&) = delete;
    auto operator=(armed_fault&&) -> armed_fault& = delete;
    auto disarm() -> void {
        if (_armed) {
            fiu_disable(_name);
            _armed = false;
        }
    }

private:
    const char* _name;
    bool _armed{true};
};

}  // namespace

BOOST_GLOBAL_FIXTURE(fiu_fixture);

// A chunk damaged in transit: the file it completes fails its SHA-256, the
// transfer is refused with `checksum` and dropped, and a clean resend
// installs.
BOOST_AUTO_TEST_CASE(a_corrupt_chunk_is_refused_and_a_clean_resend_installs,
                     *boost::unit_test::timeout(30)) {
    using reason = kythira::install_snapshot_reject_reason;
    harness h;
    source src(20);
    auto cfg = make_config();
    cfg._snapshot_staging_directory = h.staging.path();
    auto node = h.make_node(cfg);
    node->start();
    auto peer = h.peer_client();
    const auto reqs = src.requests(1, 64);
    std::optional<reason> refused;
    {
        armed_fault fault("raft/snapshot/corrupt_chunk");
        for (const auto& r : reqs) {
            refused = reason_of(send(peer, r));
            if (refused.has_value()) {
                break;
            }
        }
    }
    BOOST_TEST((refused == reason::checksum));
    BOOST_TEST(node->last_applied_index() == 0U);
    BOOST_TEST(staging_entries(h) == 0U);
    for (const auto& r : reqs) {
        BOOST_TEST(!reason_of(send(peer, r)).has_value());
    }
    BOOST_TEST(node->last_applied_index() == 20U);
    BOOST_TEST(value_of(*node, 20) == "v20");
    node->stop();
}

// A failed file fsync: refused with `io_error`, nothing installed, and a
// resend once the disk recovers installs.
BOOST_AUTO_TEST_CASE(a_failed_fsync_is_refused_and_a_resend_installs,
                     *boost::unit_test::timeout(30)) {
    using reason = kythira::install_snapshot_reject_reason;
    harness h;
    source src(20);
    auto cfg = make_config();
    cfg._snapshot_staging_directory = h.staging.path();
    auto node = h.make_node(cfg);
    node->start();
    auto peer = h.peer_client();
    const auto reqs = src.requests(1, 64);
    std::optional<reason> refused;
    {
        armed_fault fault("raft/snapshot/fsync_file");
        for (const auto& r : reqs) {
            refused = reason_of(send(peer, r));
            if (refused.has_value()) {
                break;
            }
        }
    }
    BOOST_TEST((refused == reason::io_error));
    BOOST_TEST(node->last_applied_index() == 0U);
    BOOST_TEST(!h.persistence.load_snapshot().has_value());
    for (const auto& r : reqs) {
        BOOST_TEST(!reason_of(send(peer, r)).has_value());
    }
    BOOST_TEST(node->last_applied_index() == 20U);
    node->stop();
}

// The follower dies after ingest and before the metadata save: the engine
// has the snapshot, the log store does not. Restart takes the engine's
// applied index and serves the state (Requirement 4.2).
BOOST_AUTO_TEST_CASE(a_crash_after_ingest_recovers_from_the_engine,
                     *boost::unit_test::timeout(30)) {
    harness h;
    source src(20);
    auto cfg = make_config();
    cfg._snapshot_staging_directory = h.staging.path();
    {
        auto node = h.make_node(cfg);
        node->start();
        auto peer = h.peer_client();
        const auto reqs = src.requests(1, 64);
        armed_fault fault("raft/snapshot/crash_after_ingest");
        for (const auto& r : reqs) {
            try {
                (void)send(peer, r);
            } catch (const std::exception&) {
                // The last chunk's handler "died"; the RPC fails.
            }
        }
        fault.disarm();
        node->stop();
    }
    BOOST_TEST(!h.persistence.load_snapshot().has_value());
    auto restarted = h.make_node(cfg);
    restarted->start();
    BOOST_TEST(restarted->last_applied_index() == 20U);
    BOOST_TEST(value_of(*restarted, 20) == "v20");
    BOOST_TEST(staging_entries(h) == 0U);
    restarted->stop();
}

// persistent-state-machine Requirement 9.2: kill between the engine's batch
// commits and its sync, and between the snapshot and the log truncation.
// Every entry is a `+key` counter increment, so a lost entry reads 0 and a
// doubled one reads 2.

namespace {

auto bump(node_type& node, int i) -> void {
    std::move(node.submit_command(bytes("+" + key(i)), commit_timeout)).get();
}

auto counts_are_one(node_type& node, int n) -> bool {
    bool ok = true;
    for (int i = 1; i <= n; ++i) {
        const auto v = value_of(node, i);
        BOOST_TEST_CONTEXT("counter " << i) {
            BOOST_TEST(v == "1");
        }
        ok = ok && v == "1";
    }
    return ok;
}

}  // namespace

#if !defined(KYTHIRA_PERSISTENT_TEST_ROCKSDB)
BOOST_AUTO_TEST_CASE(a_kill_before_the_engine_sync_loses_and_doubles_nothing,
                     *boost::unit_test::timeout(60)) {
    harness h;
    auto cfg = make_compacting_config();
    cfg._persistent_sync_lag_entries = 2;
    constexpr int k_entries = 12;
    {
        auto node = h.make_node(cfg);
        node->start();
        elect(*node, cfg);
        BOOST_REQUIRE(node->is_leader());
        bump(*node, 1);
        // From here no batch is durable unless the node syncs, and every
        // sync the node attempts dies first.
        h.engine->hold_durability();
        armed_fault fault("raft/persistent/before_sync");
        for (int i = 2; i <= k_entries; ++i) {
            bump(*node, i);
        }
        BOOST_TEST(node->with_state_machine([](const register_sm& sm) { return sm.syncs(); }) ==
                   0U);
        node->stop();
    }
    // The power goes with the process: every batch after the first is lost.
    h.engine->simulate_crash();
    h.engine->release_durability();
    BOOST_TEST(!h.engine->get(kythira::column_family::data, group_range.start + key(k_entries))
                    .has_value());

    auto restarted = h.make_node(cfg);
    restarted->start();
    elect(*restarted, cfg);
    BOOST_REQUIRE(restarted->is_leader());
    // The log was durable, so the new leader's first commit replays every
    // lost batch, once.
    bump(*restarted, k_entries + 1);
    BOOST_TEST(counts_are_one(*restarted, k_entries + 1));
    restarted->stop();
}
#endif

BOOST_AUTO_TEST_CASE(a_kill_between_snapshot_and_compaction_loses_and_doubles_nothing,
                     *boost::unit_test::timeout(60)) {
    harness h;
    auto cfg = make_compacting_config();
    constexpr int k_entries = 8;
    std::uint64_t snapshot_index = 0;
    {
        auto node = h.make_node(cfg);
        node->start();
        elect(*node, cfg);
        BOOST_REQUIRE(node->is_leader());
        armed_fault fault("raft/persistent/before_compact");
        for (int i = 1; i <= k_entries; ++i) {
            bump(*node, i);
        }
        const auto snap = h.persistence.load_snapshot();
        BOOST_REQUIRE(snap.has_value());
        snapshot_index = snap->last_included_index();
        BOOST_REQUIRE(snapshot_index > 1U);
        // The snapshot point was saved, but the log below it was kept.
        BOOST_TEST(h.persistence.get_log_entry(1).has_value());
        node->stop();
    }
    auto restarted = h.make_node(cfg);
    restarted->start();
    // Entries at or below the snapshot point are in the engine already and
    // must not apply again, though the log still holds them.
    BOOST_TEST(restarted->last_applied_index() >= snapshot_index);
    elect(*restarted, cfg);
    BOOST_REQUIRE(restarted->is_leader());
    bump(*restarted, k_entries + 1);
    BOOST_TEST(counts_are_one(*restarted, k_entries + 1));
    restarted->stop();
}
#endif

#if defined(KYTHIRA_PERSISTENT_TEST_ROCKSDB)
// ── Compaction on, at scale (.kiro/specs/file-install-snapshot/ task 7) ──

namespace {

// This process's resident set, from /proc/self/status, in bytes, after
// handing malloc's free pages back to the system. Without the trim, RSS
// counts memory glibc keeps cached in its per-thread arenas after a free:
// with the futures backend's many threads that varied from 4 MB to 70 MB
// between identical runs, swamping the transfer's own use. Trimmed, it is
// the memory the process actually holds, which is what Requirement 7.2
// bounds.
auto resident_bytes() -> std::uint64_t {
    ::malloc_trim(0);
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("VmRSS:")) {
            return std::stoull(line.substr(6)) * 1024;
        }
    }
    return 0;
}

// Samples resident_bytes() every 20 ms on its own thread and
// keeps the peak, until stopped.
class rss_sampler {
public:
    rss_sampler() : _thread([this] { run(); }) {}
    ~rss_sampler() { stop(); }
    rss_sampler(const rss_sampler&) = delete;
    auto operator=(const rss_sampler&) -> rss_sampler& = delete;
    rss_sampler(rss_sampler&&) = delete;
    auto operator=(rss_sampler&&) -> rss_sampler& = delete;

    auto stop() -> std::uint64_t {
        _stop = true;
        if (_thread.joinable()) {
            _thread.join();
        }
        return _peak;
    }

private:
    auto run() -> void {
        while (!_stop) {
            _peak = std::max(_peak.load(), resident_bytes());
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }

    std::atomic<bool> _stop{false};
    std::atomic<std::uint64_t> _peak{0};
    std::thread _thread;
};

// A size from the environment, or `fallback`.
auto env_bytes(const char* name, std::uint64_t fallback) -> std::uint64_t {
    if (const char* v = std::getenv(name); v != nullptr && *v != '\0') {
        return std::stoull(v);
    }
    return fallback;
}

}  // namespace

// Requirement 7.2: a follower partitioned while a group of at least 1 GB is
// written past compaction catches up through a format-1 snapshot, and the
// process's resident set grows by less than the bound (64 MB) while it does.
// All three replicas share this process, so the bound covers the leader and
// the follower together, which is stricter than one bound each.
//
// KYTHIRA_CATCHUP_TEST_BYTES and KYTHIRA_CATCHUP_TEST_RSS_BOUND override the
// group size and the bound, for a quick local run.
BOOST_AUTO_TEST_CASE(follower_catches_up_a_1gb_group_with_bounded_rss,
                     *boost::unit_test::timeout(3600)) {
    constexpr std::size_t value_bytes = std::size_t{1} << 20;
    const auto group_bytes = env_bytes("KYTHIRA_CATCHUP_TEST_BYTES", std::uint64_t{1} << 30);
    const auto rss_bound = env_bytes("KYTHIRA_CATCHUP_TEST_RSS_BOUND", std::uint64_t{64} << 20);
    const auto values = static_cast<int>((group_bytes + value_bytes - 1) / value_bytes);

    auto cfg = make_config();
    // Deadlines sized for megabyte entries under sanitizers and on a loaded
    // CI runner: a batch is JSON-encoded, base64 included, on every hop.
    cfg._election_timeout_min = std::chrono::milliseconds{2000};
    cfg._election_timeout_max = std::chrono::milliseconds{4000};
    cfg._heartbeat_interval = std::chrono::milliseconds{100};
    cfg._rpc_timeout = std::chrono::milliseconds{10000};
    cfg._append_entries_timeout = std::chrono::milliseconds{30000};
    cfg._max_append_bytes = std::size_t{2} << 20;
    cfg._persistent_log_compaction = true;  // the default, stated
    // Compact every 8 MB. After the snapshot, the follower takes the log
    // tail by AppendEntries into this harness's in-memory log store, so the
    // tail (at most one threshold of entries) is resident; it is bounded by
    // the threshold, not the group, and kept well under the RSS bound.
    cfg._snapshot_threshold_bytes = std::size_t{8} << 20;
    cfg._snapshot_chunk_size = std::size_t{1} << 20;
    cfg._install_snapshot_timeout = std::chrono::milliseconds{5000};
    persistent_cluster c(cfg);
    BOOST_REQUIRE(c.pump_until([&] { return c.leader().is_leader(); }));
    BOOST_REQUIRE(c.commit(1));
    BOOST_REQUIRE(c.pump_until([&] { return value_of(c.at(2), 1) == "v01"; }));
    const auto follower_last = c.at(2).last_applied_index();

    // Incompressible values, so the checkpoint really is the group's size.
    c.cut("3");
    std::mt19937_64 rng(7);
    std::string value(value_bytes, '\0');
    const auto big_key = [](int i) { return "big" + std::to_string(i); };
    for (int i = 0; i < values; ++i) {
        for (std::size_t off = 0; off + 8 <= value.size(); off += 8) {
            const auto r = rng();
            std::memcpy(value.data() + off, &r, 8);
        }
        // A generous deadline: a 1 MiB entry is slow under sanitizers and on
        // a loaded CI runner.
        BOOST_REQUIRE_MESSAGE(c.submit(big_key(i) + "=" + value, std::chrono::milliseconds{30000}),
                              "write " << i);
    }
    const auto leader_last = c.leader().last_applied_index();
    // The leader's log no longer reaches back to what the follower has: the
    // follower can only catch up by snapshot.
    const auto leader_snapshot = c.members[0]->persistence.load_snapshot();
    BOOST_REQUIRE(leader_snapshot.has_value());
    BOOST_REQUIRE_GT(leader_snapshot->last_included_index(), follower_last + 1);
    BOOST_REQUIRE_LE(leader_snapshot->last_included_index(), leader_last);
    const auto last_value = value;

    const auto baseline = resident_bytes();
    rss_sampler sampler;
    c.connect_all();
    BOOST_REQUIRE(c.commit(50));
    const bool caught_up = c.pump_until([&] { return value_of(c.at(2), 50) == "v50"; },
                                        std::chrono::milliseconds{900'000});
    const auto peak = sampler.stop();
    BOOST_REQUIRE(caught_up);

    const auto growth = peak > baseline ? peak - baseline : 0;
    BOOST_TEST_MESSAGE("catch-up of " << values << " MiB: RSS " << (baseline >> 20)
                                      << " MiB -> peak " << (peak >> 20) << " MiB, growth "
                                      << (growth >> 20) << " MiB (bound " << (rss_bound >> 20)
                                      << " MiB)");
    BOOST_TEST(growth < rss_bound);

    const auto installed = c.members[2]->persistence.load_snapshot();
    BOOST_REQUIRE(installed.has_value());
    BOOST_TEST(installed->last_included_index() > follower_last);
    BOOST_TEST(c.members[0]->metrics->count_named("raft_install_snapshot_success") >= 1U);
    const auto big_of = [&](std::size_t member, int i) {
        return c.at(member).with_state_machine(
            [&](const register_sm& sm) { return sm.value_of(big_key(i)).value_or(""); });
    };
    BOOST_TEST(big_of(2, 0).size() == value_bytes);
    BOOST_TEST(big_of(2, 0) == big_of(0, 0));
    BOOST_TEST(big_of(2, values / 2) == big_of(0, values / 2));
    BOOST_TEST((big_of(2, values - 1) == last_value));
}
#endif
