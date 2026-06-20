#pragma once

// Shared type bundle for chaos tests.  All chaos test executables are compiled
// with -DFIU_ENABLE, which activates the fiu_do_on() calls embedded in the
// production headers (persistence.hpp, simulator_network.hpp,
// test_state_machine.hpp).  No decorator wrapper types are used.

#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/membership.hpp>
#include <raft/test_state_machine.hpp>
#include <network_simulator/network_simulator.hpp>

#include <folly/init/Init.h>
#include <fiu.h>
#include <fiu-control.h>

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

namespace kythira::chaos {

// Type bundle matching default_raft_types but using the simulator network.
// When compiled with FIU_ENABLE the instrumented methods become live fault points.
struct chaos_raft_types {
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
};

// Convenience alias
using chaos_node = kythira::node<chaos_raft_types>;

// All Raft fault point names — used by clear_all_faults() to reset state.
static constexpr const char* k_all_fault_points[] = {
    "raft/network/send_request_vote",
    "raft/network/send_append_entries",
    "raft/network/send_install_snapshot",
    "raft/persistence/save_current_term",
    "raft/persistence/save_voted_for",
    "raft/persistence/append_log_entry",
    "raft/persistence/truncate_log",
    "raft/persistence/save_snapshot",
    "raft/state_machine/apply",
    "raft/smoke",  // synthetic point used in smoke test only
};

// Disable every Raft fault point — replacement for the non-existent fiu_disable_all().
inline void clear_all_faults() {
    for (const auto* name : k_all_fault_points) {
        fiu_disable(name);
    }
}

// Global Folly + FIU initialisation fixture — use with BOOST_GLOBAL_FIXTURE.
// BOOST_GLOBAL_FIXTURE cannot handle namespaced types directly; test files must
// declare a file-scope alias before invoking the macro.
struct chaos_test_fixture {
    chaos_test_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("chaos_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
        fiu_init(0);
    }
    ~chaos_test_fixture() { clear_all_faults(); }
    std::unique_ptr<folly::Init> _init;
};

// Add bidirectional full-mesh edges between every pair of node addresses.
// Call this after all make_chaos_node() calls so every node can reach every other.
// The edges use zero latency and perfect reliability (1.0); chaos faults are
// injected at the Raft layer, not at the simulator transport layer.
inline void wire_full_mesh(
    std::shared_ptr<network_simulator::NetworkSimulator<chaos_raft_types::raft_network_types>> sim,
    std::initializer_list<std::string> addrs) {
    network_simulator::NetworkEdge perfect{std::chrono::milliseconds{0}, 1.0};
    std::vector<std::string> av(addrs);
    for (std::size_t i = 0; i < av.size(); ++i) {
        for (std::size_t j = 0; j < av.size(); ++j) {
            if (i != j) {
                sim->add_edge(av[i], av[j], perfect);
            }
        }
    }
}

// Build a single chaos_node connected to the shared simulator.
// The caller owns the returned node.
inline auto make_chaos_node(
    std::uint64_t node_id,
    std::shared_ptr<network_simulator::NetworkSimulator<chaos_raft_types::raft_network_types>> sim,
    kythira::raft_configuration cfg = kythira::raft_configuration{})
    -> std::unique_ptr<chaos_node> {
    auto addr = std::to_string(node_id);
    auto sim_node = sim->create_node(addr);
    return std::make_unique<chaos_node>(
        node_id,
        chaos_raft_types::network_client_type{sim_node, chaos_raft_types::serializer_type{}},
        chaos_raft_types::network_server_type{sim_node, chaos_raft_types::serializer_type{}},
        chaos_raft_types::persistence_engine_type{},
        chaos_raft_types::logger_type{kythira::log_level::error}, chaos_raft_types::metrics_type{},
        chaos_raft_types::membership_manager_type{}, cfg);
}

}  // namespace kythira::chaos
