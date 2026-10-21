// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// `node` over the object-store engine with write-behind on
// (`.kiro/specs/object-store-group-commit/` task 3, Requirements 7.2, 7.3 and
// 8.4): a real three-node cluster over the network simulator, every node's
// engine an `object_store_persistence_engine` with `write_behind` over one
// shared mock bucket, a prefix per node.
//
//  1. Concurrent `submit_command` callers on the leader share flushes: the
//     leader makes fewer log PUTs than there are commands.
//  2. No entry is counted before its barrier: whenever a command's future
//     completes, a majority of the nodes' prefixes already hold the log
//     through that command, read back by a fresh engine. Node 3 is cut off,
//     so the majority is the leader and node 2 and the leader's own count
//     matters: every PUT is held in flight for a while, so a leader that
//     counted an entry before its flush landed would be caught. (A barrier
//     that returns while a flush covering it is still in flight fails this.)
#define BOOST_TEST_MODULE raft_object_store_group_commit_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include "mock_object_store.hpp"

#include <raft/object_store_persistence.hpp>
#include <raft/persistence.hpp>
#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

// ── Folly global fixture ───────────────────────────────────────────────────

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_object_store_group_commit_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

using store_t = kythira::mock_object_store;
using engine_t = kythira::object_store_persistence_engine<store_t>;
using snapshot_t = engine_t::snapshot_t;

constexpr const char* k_bucket = "kythira";
constexpr std::uint64_t k_snapshot_index = 10;
constexpr std::uint64_t k_seed_term = 1;
constexpr std::size_t k_commands = 24;

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

struct test_types {
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

    using persistence_engine_type = engine_t;
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

using sm_t = kythira::test_key_value_state_machine<std::uint64_t>;
using node_t = kythira::node<test_types>;
using sim_t = network_simulator::NetworkSimulator<test_types::raft_network_types>;

auto prefix_of(std::uint64_t id) -> std::string {
    return "node-" + std::to_string(id);
}

auto write_behind_options() -> kythira::object_persistence_options {
    kythira::object_persistence_options opts;
    opts.log = kythira::log_layout::segmented;
    opts.write_behind = true;
    return opts;
}

/// Node 1 is the only node whose election timer can fire within the test.
auto make_config(std::uint64_t id) -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    const bool candidate = id == 1;
    cfg._election_timeout_min = std::chrono::milliseconds{candidate ? 150 : 30000};
    cfg._election_timeout_max = std::chrono::milliseconds{candidate ? 300 : 40000};
    cfg._heartbeat_interval = std::chrono::milliseconds{40};
    cfg._rpc_timeout = std::chrono::milliseconds{2000};
    cfg._bootstrap_retry_interval = std::chrono::milliseconds{200};
    cfg._bootstrap_peer_find_timeout = std::chrono::milliseconds{100};
    return cfg;
}

/// Every node starts from a snapshot naming {1, 2, 3} at term 1, written to
/// its own prefix by an engine of its own before the node opens it.
auto seed(const store_t& store, std::uint64_t id) -> void {
    engine_t seeder{store, k_bucket, prefix_of(id), write_behind_options()};
    seeder.save_current_term(k_seed_term);
    sm_t sm;
    seeder.save_snapshot(snapshot_t{
        ._last_included_index = k_snapshot_index,
        ._last_included_term = k_seed_term,
        ._configuration =
            kythira::cluster_configuration<std::uint64_t>{{1, 2, 3}, false, std::nullopt},
        ._state_machine_state = sm.get_state()});
}

/// The last index a fresh engine reads from node @p id's prefix: what that node
/// holds durably, whatever its own mirror says. Construction of an unfenced
/// engine writes nothing.
auto durable_last_index(const store_t& store, std::uint64_t id) -> std::uint64_t {
    kythira::object_persistence_options reader;
    reader.log = kythira::log_layout::segmented;
    engine_t engine{store, k_bucket, prefix_of(id), reader};
    return engine.get_last_log_index();
}

auto count_log_puts(const store_t& store, std::uint64_t id) -> std::size_t {
    const std::string needle = "PUT " + prefix_of(id) + "/log/";
    const auto log = store.request_log();
    return static_cast<std::size_t>(std::count_if(
        log.begin(), log.end(), [&](const std::string& r) { return r.starts_with(needle); }));
}

/// Every object in the bucket and what a fresh reader makes of each node's
/// prefix, printed when a wait gives up, so a failure in CI says what the
/// store actually held rather than only that a predicate stayed false.
auto dump_store(const store_t& store) -> void {
    std::cout << "---- store contents ----\n";
    {
        const std::lock_guard lock(store.state()->mu);
        for (const auto& [key, object] : store.state()->objects) {
            std::cout << key << " = " << object.body.substr(0, 160) << "\n";
        }
    }
    std::cout << "---- requests ----\n";
    for (const auto& request : store.request_log()) {
        if (!request.starts_with("GET ") && !request.starts_with("LIST ")) {
            std::cout << request << "\n";
        }
    }
    for (std::uint64_t id = 1; id <= 3; ++id) {
        try {
            std::cout << "node " << id << " durable_last_index=" << durable_last_index(store, id)
                      << "\n";
        } catch (const std::exception& e) {
            std::cout << "node " << id << " reader threw: " << e.what() << "\n";
        }
    }
    std::cout.flush();
}

template<typename Pred> auto wait_until(Pred pred, std::chrono::milliseconds deadline) -> bool {
    const auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

struct cluster {
    store_t store;
    sim_t sim;
    std::vector<std::unique_ptr<node_t>> nodes;
    std::atomic<bool> pumping{true};
    std::jthread pump;

    cluster() {
        sim.start();
        // Node 3 is cut off: the leader and node 2 are the only majority.
        sim.add_edge("1", "2", network_simulator::NetworkEdge{});
        sim.add_edge("2", "1", network_simulator::NetworkEdge{});
        for (std::uint64_t id = 1; id <= 3; ++id) {
            seed(store, id);
            auto net = sim.create_node(std::to_string(id));
            nodes.push_back(std::make_unique<node_t>(kythira::node_config<test_types>{
                .node_id = id,
                .network_client =
                    test_types::network_client_type{net, test_types::serializer_type{}},
                .network_server =
                    test_types::network_server_type{net, test_types::serializer_type{}},
                .persistence = engine_t{store, k_bucket, prefix_of(id), write_behind_options()},
                .logger = kythira::console_logger{kythira::log_level::info},
                .metrics = test_types::metrics_type{},
                .membership = test_types::membership_manager_type{},
                .config = make_config(id),
                .self_address = std::to_string(id),
                .peer_discovery = preset_peer_discovery<std::uint64_t, std::string>{}}));
        }
        for (auto& n : nodes) {
            n->start();
        }
        pump = std::jthread([this] {
            while (pumping.load()) {
                nodes[0]->check_election_timeout();
                nodes[0]->check_heartbeat_timeout();
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        });
    }

    ~cluster() {
        pumping.store(false);
        pump.join();
        for (auto& node : std::ranges::reverse_view(nodes)) {
            node->stop();
        }
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;
    cluster(cluster&&) = delete;
    auto operator=(cluster&&) -> cluster& = delete;

    auto leader() -> node_t& { return *nodes[0]; }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(raft_object_store_group_commit)

BOOST_AUTO_TEST_CASE(concurrent_commands_share_flushes_and_none_is_counted_early,
                     *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(wait_until([&] { return c.leader().is_leader(); }, std::chrono::seconds{10}));
    // The new leader's no-op entry, committed before the measured commands.
    const bool no_op_durable = wait_until(
        [&] {
            return durable_last_index(c.store, 1) > k_snapshot_index &&
                   durable_last_index(c.store, 2) > k_snapshot_index;
        },
        std::chrono::seconds{10});
    if (!no_op_durable) {
        dump_store(c.store);
        for (std::size_t i = 0; i < c.nodes.size(); ++i) {
            std::cout << "node " << i + 1
                      << " in-memory last_log_index=" << c.nodes[i]->last_log_index() << "\n";
        }
    }
    BOOST_REQUIRE(no_op_durable);
    const std::uint64_t base = durable_last_index(c.store, 1);

    // Every PUT is held this long before it lands, so a flush is long enough
    // for the other submitters to stage behind it, and long enough that an
    // entry counted before its flush landed would be seen as not durable.
    {
        const std::lock_guard lock(c.store.state()->mu);
        c.store.state()->put_delay = std::chrono::milliseconds{60};
    }
    c.store.clear_requests();

    std::mutex completion_mu;
    std::size_t completed = 0;
    std::vector<std::string> violations;
    std::atomic<int> failures{0};
    std::vector<std::jthread> submitters;
    submitters.reserve(k_commands);
    for (std::size_t i = 0; i < k_commands; ++i) {
        submitters.emplace_back([&, i] {
            try {
                std::move(
                    c.leader().submit_command(sm_t::make_put_command("k" + std::to_string(i), "v"),
                                              std::chrono::seconds{30}))
                    .get();
            } catch (const std::exception&) {
                ++failures;
                return;
            }
            // This command is committed, and so is everything before it in
            // the log. Commits complete in index order, so the n-th
            // completion means at least base + n entries are committed.
            const std::lock_guard lock(completion_mu);
            ++completed;
            const std::uint64_t committed_through = base + completed;
            int holding = 0;
            for (std::uint64_t id = 1; id <= 3; ++id) {
                if (durable_last_index(c.store, id) >= committed_through) {
                    ++holding;
                }
            }
            if (holding < 2) {
                violations.push_back("index " + std::to_string(committed_through) +
                                     " committed with " + std::to_string(holding) +
                                     " node(s) holding it durably");
            }
        });
    }
    submitters.clear();

    BOOST_TEST(failures.load() == 0);
    BOOST_TEST(completed == k_commands);
    for (const auto& v : violations) {
        BOOST_ERROR(v);
    }
    const auto leader_puts = count_log_puts(c.store, 1);
    BOOST_TEST_MESSAGE("leader log PUTs for " << k_commands << " commands: " << leader_puts);
    BOOST_TEST(leader_puts < k_commands);
    BOOST_TEST(durable_last_index(c.store, 1) >= base + k_commands);
    // Node 3 never received an entry: its prefix holds the snapshot alone.
    BOOST_TEST(durable_last_index(c.store, 3) == 0U);
}

BOOST_AUTO_TEST_SUITE_END()
