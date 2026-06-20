#include "config.hpp"
#include "http_control.hpp"

#include <raft/console_logger.hpp>
#include <raft/file_persistence.hpp>
#include <raft/membership.hpp>
#include <raft/metrics.hpp>
#include <raft/raft.hpp>
#include <raft/tcp_raft_types.hpp>
#include <raft/tcp_rpc.hpp>
#include <raft/test_state_machine.hpp>

#ifdef KYTHIRA_FAULT_INJECTION
#include <fiu.h>
#include "fiu_remote.hpp"
#endif

#include <folly/init/Init.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

// ── Signal handling ──────────────────────────────────────────────────────────

static std::atomic<bool> g_stop{false};
static std::mutex g_stop_mu;
static std::condition_variable g_stop_cv;

static void sigterm_handler(int) {
    g_stop = true;
    g_stop_cv.notify_all();
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    folly::Init init_(&argc, &argv, false);

    // ── Config ───────────────────────────────────────────────────────────────
    chaos_node::node_config cfg;
    try {
        cfg = chaos_node::node_config::from_env();
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n"
                  << "Usage: set NODE_ID and PEERS env vars.\n"
                  << "  NODE_ID=1\n"
                  << "  PEERS=2:node2:7000,3:node3:7000\n";
        return 1;
    }

    // ── Fault injection (remote control) ─────────────────────────────────────
#ifdef KYTHIRA_FAULT_INJECTION
    fiu_init(0);
    chaos_node::fiu_tcp_rc fiu_server(cfg.fiu_port);
    fiu_server.start();
    std::cerr << "[info] fiu remote control listening on :" << cfg.fiu_port << "\n";
#endif

    // ── Components ───────────────────────────────────────────────────────────
    kythira::tcp_rpc_server server(cfg.rpc_port);
    kythira::tcp_rpc_client client;
    for (const auto& p : cfg.peers) client.add_peer(p.node_id, p.host, p.port);

    kythira::file_persistence_engine<> persistence(cfg.data_dir);

    kythira::raft_configuration raft_cfg;
    raft_cfg._election_timeout_min = cfg.election_timeout_min;
    raft_cfg._election_timeout_max = cfg.election_timeout_max;
    raft_cfg._heartbeat_interval = cfg.heartbeat_interval;

    // ── Raft node ─────────────────────────────────────────────────────────────
    kythira::node<kythira::tcp_raft_types> raft_node(
        cfg.node_id, std::move(client), std::move(server), std::move(persistence),
        kythira::console_logger{}, kythira::noop_metrics{},
        kythira::default_membership_manager<std::uint64_t>{}, raft_cfg);

    raft_node.set_cluster_configuration(cfg.all_node_ids());

    std::cerr << "[info] chaos_node starting: id=" << cfg.node_id << " rpc=" << cfg.rpc_port
              << " http=" << cfg.http_port << " peers=" << cfg.peers.size() << "\n";

    raft_node.start();

    // ── HTTP control plane ────────────────────────────────────────────────────
    chaos_node::http_control http(raft_node, cfg.node_id, cfg.http_port);
    http.start();

    // ── Election / heartbeat timer threads ────────────────────────────────────
    std::thread election_timer([&] {
        while (!g_stop) {
            std::this_thread::sleep_for(cfg.election_timeout_min / 2);
            if (!g_stop) raft_node.check_election_timeout();
        }
    });

    std::thread heartbeat_timer([&] {
        while (!g_stop) {
            std::this_thread::sleep_for(cfg.heartbeat_interval);
            if (!g_stop) raft_node.check_heartbeat_timeout();
        }
    });

    // ── Signal handler ────────────────────────────────────────────────────────
    std::signal(SIGTERM, sigterm_handler);
    std::signal(SIGINT, sigterm_handler);

    {
        std::unique_lock lock(g_stop_mu);
        g_stop_cv.wait(lock, [] { return g_stop.load(); });
    }

    std::cerr << "[info] chaos_node shutting down\n";

    g_stop = true;
    election_timer.join();
    heartbeat_timer.join();

    http.stop();
    raft_node.stop();

    return 0;
}
