#include "config.hpp"
#include "http_control.hpp"

#include <raft/console_logger.hpp>
#include <raft/docker_quorum_manager.hpp>
#include <raft/file_persistence.hpp>
#include <raft/membership.hpp>
#include <raft/metrics.hpp>
#include <raft/otlp_logger.hpp>
#include <raft/otlp_metrics.hpp>
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

// ── Types: tcp_raft_types extended with docker_quorum_manager ────────────────

namespace {

// Inherits all type aliases from tcp_raft_types; adds quorum_manager_type so
// that _quorum_manager_type_traits picks up docker_quorum_manager instead of
// the no_op fallback.
struct tcp_raft_types_with_docker_qm : kythira::tcp_raft_types {
    using quorum_manager_type = kythira::docker_quorum_manager<node_id_type, std::string>;
};

// Inherits all type aliases from tcp_raft_types; swaps logger_type/
// metrics_type to the OTLP implementations (.kiro/specs/otlp-telemetry-backend/,
// Requirement 5.3). Selected at startup, not combined with
// tcp_raft_types_with_docker_qm above in this pass — OTLP + docker quorum
// manager together is future work, not required by that spec.
struct tcp_raft_types_with_otlp : kythira::tcp_raft_types {
    using logger_type = kythira::otlp_logger;
    using metrics_type = kythira::otlp_metrics;
};

// Core startup logic templated on the raft types.  Both the plain and the
// docker-QM paths share identical timer, signal, and HTTP control logic.
template<typename RaftTypes>
int run_node(chaos_node::node_config cfg, kythira::node_config<RaftTypes> ncfg) {
    kythira::node<RaftTypes> raft_node(std::move(ncfg));
    raft_node.set_cluster_configuration(cfg.all_node_ids());

    std::cerr << "[info] chaos_node starting: id=" << cfg.node_id << " rpc=" << cfg.rpc_port
              << " http=" << cfg.http_port << " peers=" << cfg.peers.size() << "\n";

    raft_node.start();

    chaos_node::http_control_tmpl<kythira::node<RaftTypes>> http(raft_node, cfg.node_id,
                                                                 cfg.http_port);
    http.start();

    std::thread election_timer([&] {
        while (!g_stop) {
            std::this_thread::sleep_for(cfg.election_timeout_min / 2);
            if (!g_stop) {
                raft_node.check_election_timeout();
            }
        }
    });

    std::thread heartbeat_timer([&] {
        while (!g_stop) {
            std::this_thread::sleep_for(cfg.heartbeat_interval);
            if (!g_stop) {
                raft_node.check_heartbeat_timeout();
            }
        }
    });

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

}  // namespace

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

    // ── Raft configuration ────────────────────────────────────────────────────
    kythira::raft_configuration raft_cfg;
    raft_cfg._election_timeout_min = cfg.election_timeout_min;
    raft_cfg._election_timeout_max = cfg.election_timeout_max;
    raft_cfg._heartbeat_interval = cfg.heartbeat_interval;

    // ── Components ───────────────────────────────────────────────────────────
    kythira::tcp_rpc_server server(cfg.rpc_port);
    kythira::tcp_rpc_client client;
    for (const auto& p : cfg.peers) {
        client.add_peer(p.node_id, p.host, p.port);
    }
    kythira::file_persistence_engine<> persistence(cfg.data_dir);

    // ── OTLP telemetry (.kiro/specs/otlp-telemetry-backend/, Req 5.3) ─────────
    // Opt-in and additive: unset OTLP_ENDPOINT leaves today's console_logger/
    // noop_metrics path (below) completely unchanged. Not combined with the
    // docker quorum manager path in this pass (see tcp_raft_types_with_otlp's
    // comment above).
    if (cfg.otlp_endpoint) {
        kythira::otlp_export_config otlp_cfg;
        otlp_cfg.endpoint_base_url = *cfg.otlp_endpoint;
        otlp_cfg.headers = cfg.otlp_headers;

        kythira::otlp_resource otlp_res;
        otlp_res.service_name = cfg.otlp_service_name;
        otlp_res.service_instance_id = std::to_string(cfg.node_id);

        std::cerr << "[info] telemetry: otlp (" << *cfg.otlp_endpoint << ")\n";

        kythira::node_config<tcp_raft_types_with_otlp> ncfg{
            .node_id = cfg.node_id,
            .network_client = std::move(client),
            .network_server = std::move(server),
            .persistence = std::move(persistence),
            .logger = kythira::otlp_logger{otlp_cfg, otlp_res},
            .metrics = kythira::otlp_metrics{otlp_cfg, otlp_res},
            .membership = kythira::default_membership_manager<std::uint64_t>{},
            .config = raft_cfg,
        };

        return run_node<tcp_raft_types_with_otlp>(cfg, std::move(ncfg));
    }

    std::cerr << "[info] telemetry: console/noop\n";

    // ── Quorum manager selection (Req 19 AC 2) ────────────────────────────────
    const char* qm_env = std::getenv("QUORUM_MANAGER");
    const bool use_docker_qm = (qm_env != nullptr && std::string{qm_env} == "docker");

    if (use_docker_qm) {
        auto get_env = [](const char* name, const char* def) -> std::string {
            const char* v = std::getenv(name);
            return (v != nullptr && *v != '\0') ? v : def;
        };

        kythira::docker_quorum_manager_config qm_cfg;
        qm_cfg.image = get_env("QUORUM_IMAGE", "kythira-chaos-node:dev");
        qm_cfg.cluster_name = get_env("QUORUM_CLUSTER", "kythira-quorum-test");
        qm_cfg.network_name = get_env("QUORUM_NETWORK", "kythira-quorum-net");
        qm_cfg.target_count = static_cast<std::size_t>(std::stoul(get_env("QUORUM_TARGET", "3")));
        qm_cfg.node_port = static_cast<std::uint16_t>(cfg.rpc_port);

        std::cerr << "[info] quorum manager: docker (cluster=" << qm_cfg.cluster_name
                  << " target=" << qm_cfg.target_count << ")\n";

        kythira::node_config<tcp_raft_types_with_docker_qm> ncfg{
            .node_id = cfg.node_id,
            .network_client = std::move(client),
            .network_server = std::move(server),
            .persistence = std::move(persistence),
            .logger = kythira::console_logger{},
            .metrics = kythira::noop_metrics{},
            .membership = kythira::default_membership_manager<std::uint64_t>{},
            .config = raft_cfg,
            .quorum_manager = tcp_raft_types_with_docker_qm::quorum_manager_type{qm_cfg},
        };

        return run_node<tcp_raft_types_with_docker_qm>(cfg, std::move(ncfg));
    }

    // Default path — no quorum manager (no_op fallback via detection trait)
    std::cerr << "[info] quorum manager: no_op\n";

    kythira::node_config<kythira::tcp_raft_types> ncfg{
        .node_id = cfg.node_id,
        .network_client = std::move(client),
        .network_server = std::move(server),
        .persistence = std::move(persistence),
        .logger = kythira::console_logger{},
        .metrics = kythira::noop_metrics{},
        .membership = kythira::default_membership_manager<std::uint64_t>{},
        .config = raft_cfg,
    };

    return run_node<kythira::tcp_raft_types>(cfg, std::move(ncfg));
}
