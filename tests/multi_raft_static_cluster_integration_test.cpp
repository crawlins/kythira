// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_static_cluster_integration_test.cpp
/// @brief Three nodes, four statically configured shards, no split and no
///        merge — the milestone that proves Phases 1-5 stand on their own
///        (task 14 of `.kiro/specs/multi-raft/`).
///
/// This is the first test where the layering is actually load-bearing. Each
/// node runs one `multi_raft` host over **one** shared transport, and four
/// `node<Types>` replicas share it. Every RPC therefore has to be demultiplexed
/// by group id at the far end, and every one of the twelve replicas has to
/// elect, replicate and apply without ever observing another group's state.
///
/// The assertions are deliberately about the two things a sharded cluster can
/// get wrong that a single-group one cannot:
///
/// - **Isolation.** A command routed to shard 2 must not appear in shard 1's
///   state machine. A demultiplexer that leaked would still pass a
///   "did the command commit" check.
/// - **Tiling.** `check_tiling()` on every node after every operation. It is
///   the executable form of the design's most important invariant, and a
///   routing table that developed a gap under failover would otherwise only
///   show up as a mysteriously unroutable key much later.
///
/// Restart is included because a shard's replica set spans nodes: killing node
/// 2 has to leave all four shards writable through their remaining majorities,
/// and bringing it back has to catch all four up.

#define BOOST_TEST_MODULE multi_raft_static_cluster_integration_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_static_cluster_integration_test"),
                             nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_range;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_t = std::uint64_t;

struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type = fabric_client;
    using network_server_type = fabric_server;

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
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type, group_id_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type, group_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type,
                                        group_id_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type, group_id_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type,
                                          group_id_type>;
    using install_snapshot_response_type =
        kythira::install_snapshot_response<term_id_type, group_id_type>;
};

using host_type = multi_raft<host_types, key_type, group_id_type>;
using config_type = multi_raft_config<host_types, key_type, group_id_type>;
using descriptor_type = shard_descriptor<group_id_type, key_type, node_id_t>;

constexpr std::size_t k_node_count = 3;
constexpr std::size_t k_shard_count = 4;

// ── the state machine's command encoding ─────────────────────────────────────

auto encode_command(std::uint8_t type, const std::string& key, const std::string& value)
    -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(type));
    const auto append_u32 = [&out](std::uint32_t n) {
        std::byte buf[sizeof(std::uint32_t)];
        std::memcpy(buf, &n, sizeof(n));
        out.insert(out.end(), std::begin(buf), std::end(buf));
    };
    const auto append_str = [&out](const std::string& s) {
        for (char c : s) {
            out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
    };
    append_u32(static_cast<std::uint32_t>(key.size()));
    append_str(key);
    if (type == 1) {
        append_u32(static_cast<std::uint32_t>(value.size()));
        append_str(value);
    }
    return out;
}

auto put_command(const std::string& key, const std::string& value) -> std::vector<std::byte> {
    return encode_command(1, key, value);
}
auto get_command(const std::string& key) -> std::vector<std::byte> {
    return encode_command(0, key, {});
}

struct kv_partitioner {
    [[nodiscard]] auto key_of(const std::vector<std::byte>& command) const -> key_type {
        if (command.size() < 1 + sizeof(std::uint32_t)) {
            return {};
        }
        std::uint32_t key_length{};
        std::memcpy(&key_length, command.data() + 1, sizeof(key_length));
        if (1 + sizeof(std::uint32_t) + key_length > command.size()) {
            return {};
        }
        std::string key;
        key.reserve(key_length);
        for (std::uint32_t i = 0; i < key_length; ++i) {
            key.push_back(static_cast<char>(command[1 + sizeof(std::uint32_t) + i]));
        }
        return key;
    }
};

/// @brief The keys a `normal` log entry writes, decoded from the command.
///
/// Reading the *log* rather than the state machine is the sharper isolation
/// check: it shows which commands actually reached this replica, on followers
/// as well as leaders, and `node<Types>` exposes no state-machine accessor
/// anyway. `no_op` and `configuration` entries carry no application command and
/// are skipped.
auto keys_in_log(const std::vector<kythira::log_entry<std::uint64_t, std::uint64_t>>& log)
    -> std::vector<key_type> {
    const kv_partitioner p;
    std::vector<key_type> out;
    for (const auto& entry : log) {
        if (entry.type() != kythira::entry_type::normal || entry.command().empty()) {
            continue;
        }
        auto k = p.key_of(entry.command());
        if (!k.empty()) {
            out.push_back(std::move(k));
        }
    }
    return out;
}

// ── the static shard map ─────────────────────────────────────────────────────

auto range_of(std::optional<key_type> start, std::optional<key_type> end) -> shard_range<key_type> {
    return shard_range<key_type>{._start = std::move(start), ._end = std::move(end)};
}

/// Four shards tiling the key space, every one replicated on all three nodes.
auto static_shards() -> std::vector<descriptor_type> {
    const std::vector<node_id_t> voters{1, 2, 3};
    return {
        descriptor_type{._group_id = 1,
                        ._range = range_of(std::nullopt, key_type{"d"}),
                        ._epoch = shard_epoch{},
                        ._voters = voters},
        descriptor_type{._group_id = 2,
                        ._range = range_of(key_type{"d"}, key_type{"m"}),
                        ._epoch = shard_epoch{},
                        ._voters = voters},
        descriptor_type{._group_id = 3,
                        ._range = range_of(key_type{"m"}, key_type{"t"}),
                        ._epoch = shard_epoch{},
                        ._voters = voters},
        descriptor_type{._group_id = 4,
                        ._range = range_of(key_type{"t"}, std::nullopt),
                        ._epoch = shard_epoch{},
                        ._voters = voters},
    };
}

/// Keys chosen so each shard gets at least two, and the boundary keys land on
/// the shard that owns them under half-open semantics.
auto workload_keys() -> std::vector<key_type> {
    return {"apple", "cherry", "d", "grape", "lemon", "m", "orange", "plum", "t", "watermelon"};
}

auto expected_shard_for(const key_type& k) -> group_id_type {
    if (k < "d") {
        return 1;
    }
    if (k < "m") {
        return 2;
    }
    if (k < "t") {
        return 3;
    }
    return 4;
}

// ── a three-node cluster that ticks itself ───────────────────────────────────

/// Each host is driven by its own thread, the way `cmd/chaos_node/main.cpp`
/// drives a single node: `multi_raft` has no timer thread of its own, and that
/// is the property the whole design rests on.
class cluster {
public:
    cluster() {
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
            for (const auto& shard : static_shards()) {
                _hosts.back()->create_group(shard);
            }
        }
        for (auto& h : _hosts) {
            h->start();
        }
        _running = true;
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            _drivers.emplace_back([this, i] { drive(i); });
        }
    }

    ~cluster() {
        _running = false;
        for (auto& t : _drivers) {
            if (t.joinable()) {
                t.join();
            }
        }
        for (auto& h : _hosts) {
            h->stop();
        }
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;

    [[nodiscard]] auto host(node_id_t id) -> host_type& { return *_hosts.at(id - 1); }
    [[nodiscard]] auto fabric() -> message_fabric& { return _fabric; }

    /// @brief The host whose replica of `group` currently leads, if any.
    [[nodiscard]] auto leader_of(group_id_type group) -> host_type* {
        for (auto& h : _hosts) {
            auto* n = h->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return h.get();
            }
        }
        return nullptr;
    }

    /// @brief Wait until every shard has a leader somewhere.
    auto await_all_leaders(std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            bool all = true;
            for (group_id_type g = 1; g <= k_shard_count; ++g) {
                if (leader_of(g) == nullptr) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    /// @brief Put `key = value` through whichever host leads the owning shard.
    ///
    /// Retries across leader changes, which is the client-side hop the
    /// `shard_not_leader_exception` hint exists to make cheap.
    auto put(const key_type& key, const std::string& value,
             std::chrono::milliseconds budget = std::chrono::milliseconds{5000}) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& h : _hosts) {
                auto desc = h->resolve(key);
                if (!desc.has_value()) {
                    continue;
                }
                auto* n = h->group_node(desc->_group_id);
                if (n == nullptr || !n->is_leader()) {
                    continue;
                }
                auto f = h->submit_command(key, put_command(key, value),
                                           std::chrono::milliseconds{2000});
                if (f.wait(std::chrono::milliseconds{2000})) {
                    try {
                        std::ignore = f.get();
                        return true;
                    } catch (...) {
                        // Leadership moved between the check and the submit;
                        // fall through and try again.
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    /// @brief Stop a host and cut it off, as a process death would.
    ///
    /// The driver is paused and *acknowledged* before the host is stopped.
    /// `multi_raft::stop()` tolerates a tick already in flight, but a test that
    /// deliberately overlapped them would be testing that tolerance rather than
    /// the restart it is named for.
    auto kill(node_id_t id) -> void {
        pause_and_await(id - 1);
        _fabric.kill(id);
        _hosts.at(id - 1)->stop();
    }

    auto restart(node_id_t id) -> void {
        _hosts.at(id - 1)->start();
        _fabric.revive(id);
        _paused[id - 1] = false;
    }

    /// @brief Stop every driver thread and let the fabric go quiet.
    ///
    /// With no host ticking, nothing sends a heartbeat and nothing appends, so
    /// the replicas' logs are genuinely static and can be read directly. The
    /// alternative — reading them under load — would be a data race dressed up
    /// as a test.
    auto quiesce() -> void {
        for (std::size_t i = 0; i < k_node_count; ++i) {
            pause_and_await(i);
        }
        // A message already on a fabric worker can still land after the last
        // tick returns; give it room before reading any log.
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    auto resume() -> void {
        for (auto& p : _paused) {
            p = false;
        }
    }

    /// @brief The application keys in `group`'s log on `node`. Call while quiesced.
    [[nodiscard]] auto logged_keys(node_id_t node, group_id_type group) -> std::vector<key_type> {
        auto* n = _hosts.at(node - 1)->group_node(group);
        if (n == nullptr) {
            return {};
        }
        const auto snapshot = n->debug_state();
        return keys_in_log({snapshot.log.begin(), snapshot.log.end()});
    }

    /// @brief The tiling invariant, on every node.
    [[nodiscard]] auto tiling_problem() -> std::optional<std::string> {
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            if (auto problem = _hosts.at(id - 1)->shard_map_snapshot().check_tiling()) {
                return "node " + std::to_string(id) + ": " + *problem;
            }
        }
        return std::nullopt;
    }

private:
    auto make_config(node_id_t id) -> config_type {
        config_type cfg{
            .node_id = id,
            .network_client = fabric_client{_fabric, id},
            .network_server = fabric_server{_fabric, id},
            .store_factory =
                [](const group_id_type&) { return host_types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{120};
        cfg.config._election_timeout_max = std::chrono::milliseconds{260};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{25};
        // Hibernation off: it is task 12's property, and leaving it on here
        // would make a leader-election timing failure look like a hibernation
        // bug and vice versa.
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.partitioner = kythira::make_partitioner<key_type>(kv_partitioner{});
        return cfg;
    }

    auto drive(std::size_t index) -> void {
        while (_running.load()) {
            if (_paused[index].load()) {
                _idle[index] = true;
            } else {
                _idle[index] = false;
                _hosts[index]->tick();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        _idle[index] = true;
    }

    /// @brief Ask driver `index` to stop and wait until it says it has.
    auto pause_and_await(std::size_t index) -> void {
        _paused[index] = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!_idle[index].load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }

    message_fabric _fabric{8};
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
    std::array<std::atomic<bool>, k_node_count> _paused{};
    /// Set by each driver while it is not inside `tick()`, so `pause_and_await`
    /// can tell "asked to stop" from "actually stopped".
    std::array<std::atomic<bool>, k_node_count> _idle{};
};

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_static_cluster)

BOOST_AUTO_TEST_CASE(four_shards_on_three_nodes_each_elect_a_leader,
                     *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));

    // Twelve replicas, four leaders, and every node's map tiles the key space.
    for (group_id_type g = 1; g <= k_shard_count; ++g) {
        BOOST_CHECK(c.leader_of(g) != nullptr);
    }
    if (auto problem = c.tiling_problem()) {
        BOOST_FAIL("tiling broken after election: " << *problem);
    }
    BOOST_CHECK_EQUAL(c.host(1).group_count(), k_shard_count);
}

BOOST_AUTO_TEST_CASE(one_transport_carries_four_groups_without_leaking_between_them,
                     *boost::unit_test::timeout(120)) {
    // The isolation claim, and the reason this test exists at all. A
    // demultiplexer that mixed groups up would still pass a "did the command
    // commit" check, so the assertion is on *which replica's log holds which
    // key* — on followers as well as leaders.
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));

    for (const auto& key : workload_keys()) {
        BOOST_REQUIRE_MESSAGE(c.put(key, "v-" + key), "failed to commit key " << key);
        if (auto problem = c.tiling_problem()) {
            BOOST_FAIL("tiling broken after writing " << key << ": " << *problem);
        }
    }

    // Let the writes reach every follower, then stop the world so the logs can
    // be read without racing the drivers.
    std::this_thread::sleep_for(std::chrono::milliseconds{800});
    c.quiesce();

    for (node_id_t id = 1; id <= k_node_count; ++id) {
        for (group_id_type g = 1; g <= k_shard_count; ++g) {
            const auto keys = c.logged_keys(id, g);
            for (const auto& k : keys) {
                BOOST_CHECK_MESSAGE(expected_shard_for(k) == g,
                                    "node " << id << " shard " << g << " holds key " << k
                                            << ", which belongs to shard "
                                            << expected_shard_for(k));
            }
        }
    }

    // And every key really did land somewhere — an isolation check alone would
    // pass against a cluster that dropped everything.
    for (const auto& key : workload_keys()) {
        const auto owner = expected_shard_for(key);
        std::size_t replicas_holding = 0;
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            const auto keys = c.logged_keys(id, owner);
            if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
                ++replicas_holding;
            }
        }
        // A committed entry is on a majority by definition; all three is the
        // steady state a quiet cluster reaches.
        BOOST_CHECK_MESSAGE(replicas_holding >= 2, "key " << key << " is on only "
                                                          << replicas_holding
                                                          << " replica(s) of shard " << owner);
    }
    c.resume();
}

BOOST_AUTO_TEST_CASE(every_shard_stays_writable_through_a_node_restart,
                     *boost::unit_test::timeout(180)) {
    // A shard's replica set spans nodes, so killing one node must leave all
    // four shards writable through their remaining majorities — and bringing it
    // back must catch all four up, not just the one that happened to be
    // exercised.
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));

    for (const auto& key : workload_keys()) {
        BOOST_REQUIRE(c.put(key, "before"));
    }

    c.kill(2);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    for (const auto& key : workload_keys()) {
        BOOST_REQUIRE_MESSAGE(c.put(key, "during"),
                              "key " << key << " unwritable with node 2 down");
    }
    if (auto problem = c.tiling_problem()) {
        BOOST_FAIL("tiling broken with a node down: " << *problem);
    }

    c.restart(2);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    for (const auto& key : workload_keys()) {
        BOOST_REQUIRE(c.put(key, "after"));
    }

    // Node 2 catches EVERY shard up, not merely the ones it happened to lead.
    // A restart that repaired one group and quietly left the others behind
    // would pass any single-shard check.
    std::this_thread::sleep_for(std::chrono::milliseconds{1500});
    c.quiesce();
    for (group_id_type g = 1; g <= k_shard_count; ++g) {
        const auto on_two = c.logged_keys(2, g);
        for (const auto& key : workload_keys()) {
            if (expected_shard_for(key) != g) {
                continue;
            }
            BOOST_CHECK_MESSAGE(std::find(on_two.begin(), on_two.end(), key) != on_two.end(),
                                "node 2 shard " << g << " never caught up on key " << key);
        }
    }
    c.resume();

    if (auto problem = c.tiling_problem()) {
        BOOST_FAIL("tiling broken after restart: " << *problem);
    }
}

BOOST_AUTO_TEST_CASE(a_mixed_read_write_workload_commits_on_every_shard,
                     *boost::unit_test::timeout(180)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));

    for (int round = 0; round < 3; ++round) {
        for (const auto& key : workload_keys()) {
            BOOST_REQUIRE(c.put(key, "round-" + std::to_string(round)));

            // A read goes through the same routing path and lands on the same
            // shard; it is counted separately so the policy layer can tell a
            // read-hot shard from a write-hot one.
            auto* leader = c.leader_of(expected_shard_for(key));
            BOOST_REQUIRE(leader != nullptr);
            auto f = leader->read_state(key, std::chrono::milliseconds{2000});
            BOOST_CHECK(f.wait(std::chrono::milliseconds{2000}));
        }
        if (auto problem = c.tiling_problem()) {
            BOOST_FAIL("tiling broken in round " << round << ": " << *problem);
        }
    }

    // Every shard saw traffic on the node that led it — a workload that
    // silently all landed on one shard would pass every check above.
    std::size_t shards_with_traffic = 0;
    for (group_id_type g = 1; g <= k_shard_count; ++g) {
        std::uint64_t writes = 0;
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            writes += c.host(id).load_counters(g).second;
        }
        if (writes > 0) {
            ++shards_with_traffic;
        }
    }
    BOOST_CHECK_EQUAL(shards_with_traffic, k_shard_count);
}

BOOST_AUTO_TEST_SUITE_END()
