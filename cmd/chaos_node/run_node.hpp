// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// chaos_node's run loop, run_node<RaftTypes>: start the node, its timers and
// its HTTP control server, and wait for SIGTERM.
//
// Every bundle in node_types.hpp gets its own kythira::node instantiation,
// and there are up to twelve of them. Compiled in one translation unit
// (main.cpp, until November 2026) that was the slowest compile in the tree:
// under the stdexec backend it ran alone at the end of the Full suite build
// for 45-95 minutes on every run, and from November 1 the runner was lost
// partway through it. So main.cpp only *declares* the instantiations
// (CHAOS_NODE_EXTERN_RUN_NODE below) and the run_node_*.cpp files define
// them, a few per file, compiled in parallel and each a fraction of the
// memory. A new bundle needs an entry in CHAOS_NODE_FOR_EACH_BUNDLE and one
// in a run_node_*.cpp file, or main.cpp fails to link.

#pragma once

#include "http_control.hpp"
#include "node_types.hpp"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

namespace chaos_node {

namespace detail {
inline std::atomic<bool> g_stop{false};
inline std::mutex g_stop_mu;
inline std::condition_variable g_stop_cv;

inline void sigterm_handler(int) {
    g_stop = true;
    g_stop_cv.notify_all();
}
}  // namespace detail

// Core startup logic templated on the raft types.  Both the plain and the
// docker-QM paths share identical timer, signal, and HTTP control logic.
template<typename RaftTypes> int run_node(node_config cfg, kythira::node_config<RaftTypes> ncfg) {
    if constexpr (!is_joinable<RaftTypes>) {
        if (cfg.join) {
            std::cerr << "chaos_node: JOIN=1 is not supported with a telemetry backend\n";
            return 1;
        }
    }

    kythira::node<RaftTypes> raft_node(std::move(ncfg));
    // A joiner's configuration arrives from the leader once it is admitted.
    if (!cfg.join) {
        raft_node.set_cluster_configuration(cfg.all_node_ids());
    }

    std::cerr << "[info] chaos_node starting: id=" << cfg.node_id << " rpc=" << cfg.rpc_address
              << ":" << cfg.rpc_port << " http=" << cfg.http_port << " peers=" << cfg.peers.size()
              << (cfg.join ? " join=" + cfg.self_address : std::string{}) << "\n";

    raft_node.start();

    chaos_node::http_control_tmpl<kythira::node<RaftTypes>> http(raft_node, cfg.node_id,
                                                                 cfg.http_port);
    http.start();

    std::thread election_timer([&] {
        while (!detail::g_stop) {
            std::this_thread::sleep_for(cfg.election_timeout_min / 2);
            if (!detail::g_stop) {
                raft_node.check_election_timeout();
            }
        }
    });

    std::thread heartbeat_timer([&] {
        while (!detail::g_stop) {
            std::this_thread::sleep_for(cfg.heartbeat_interval);
            if (!detail::g_stop) {
                raft_node.check_heartbeat_timeout();
            }
        }
    });

    std::signal(SIGTERM, detail::sigterm_handler);
    std::signal(SIGINT, detail::sigterm_handler);

    {
        std::unique_lock lock(detail::g_stop_mu);
        detail::g_stop_cv.wait(lock, [] { return detail::g_stop.load(); });
    }

    std::cerr << "[info] chaos_node shutting down\n";

    detail::g_stop = true;
    election_timer.join();
    heartbeat_timer.join();

    http.stop();
    raft_node.stop();

    return 0;
}

}  // namespace chaos_node

// Applies X to every bundle this build has.
#ifdef KYTHIRA_CHAOS_NODE_RPC_TLS
#define CHAOS_NODE_FOR_EACH_TLS_BUNDLE(X) X(tcp_raft_types_with_tls)
#else
#define CHAOS_NODE_FOR_EACH_TLS_BUNDLE(X)
#endif
#ifdef KYTHIRA_HAS_AWS_SDK
#define CHAOS_NODE_FOR_EACH_AWS_BUNDLE(X) X(tcp_raft_types_with_aws_qm)
#else
#define CHAOS_NODE_FOR_EACH_AWS_BUNDLE(X)
#endif
#define CHAOS_NODE_FOR_EACH_BUNDLE(X)                   \
    X(tcp_raft_types_joinable)                          \
    X(tcp_raft_types_with_docker_qm)                    \
    X(tcp_raft_types_with_otlp)                         \
    X(tcp_raft_types_with_prometheus)                   \
    X(tcp_raft_types_with_prometheus_loki)              \
    X(tcp_raft_types_with_victoriametrics)              \
    X(tcp_raft_types_with_victoriametrics_victorialogs) \
    X(tcp_raft_types_with_telegraf)                     \
    X(tcp_raft_types_with_telegraf_logs)                \
    X(tcp_raft_types_with_netdata)                      \
    CHAOS_NODE_FOR_EACH_TLS_BUNDLE(X)                   \
    CHAOS_NODE_FOR_EACH_AWS_BUNDLE(X)

// An explicit instantiation declaration (main.cpp) and definition (the
// run_node_*.cpp files) of run_node for one bundle.
#define CHAOS_NODE_EXTERN_RUN_NODE(Bundle)                        \
    extern template int chaos_node::run_node<chaos_node::Bundle>( \
        chaos_node::node_config, kythira::node_config<chaos_node::Bundle>);
#define CHAOS_NODE_INSTANTIATE_RUN_NODE(Bundle)            \
    template int chaos_node::run_node<chaos_node::Bundle>( \
        chaos_node::node_config, kythira::node_config<chaos_node::Bundle>);

CHAOS_NODE_FOR_EACH_BUNDLE(CHAOS_NODE_EXTERN_RUN_NODE)
