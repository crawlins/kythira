// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// chaos_node's raft type bundles: one per component combination the node can
// start with. Shared by main.cpp, which builds a node_config for the chosen
// bundle, and the run_node_*.cpp files, which instantiate kythira::node for
// them (see run_node.hpp for why those are split out).

#pragma once

#include "aws_quorum.hpp"
#include "config.hpp"

#include <raft/console_logger.hpp>
#include <raft/docker_quorum_manager.hpp>
#include <raft/file_persistence.hpp>
#include <raft/loki_logger.hpp>
#include <raft/membership.hpp>
#include <raft/metrics.hpp>
#include <raft/netdata_metrics.hpp>
#include <raft/otlp_logger.hpp>
#include <raft/otlp_metrics.hpp>
#include <raft/peer_discovery.hpp>
#include <raft/prometheus_metrics.hpp>
#include <raft/raft.hpp>
#include <raft/tcp_raft_types.hpp>
#include <raft/tcp_rpc.hpp>
#include <raft/telegraf_logger.hpp>
#include <raft/telegraf_metrics.hpp>
#include <raft/test_state_machine.hpp>
#include <raft/victorialogs_logger.hpp>
#include <raft/victoriametrics_metrics.hpp>

#ifdef KYTHIRA_CHAOS_NODE_RPC_TLS
#include <raft/tls_tcp_rpc.hpp>
#endif

#include <string>
#include <type_traits>

namespace chaos_node {

// The node id every bundle below uses: kythira::host_node_id, which is
// std::uint64_t or canonical text depending on KYTHIRA_NODE_ID_KIND.
using host_tcp_raft_types = kythira::basic_tcp_raft_types<node_id_type>;

// Inherits all type aliases from tcp_raft_types; adds seed-list peer discovery
// so that a node started with JOIN=1 can send a ClusterJoin to PEERS instead of
// installing them as its configuration (quorum-management Req 19). A node
// without JOIN gets an empty seed list, which run_bootstrap() treats exactly
// like the no_op discovery it replaces.
struct tcp_raft_types_joinable : host_tcp_raft_types {
    using address_type = std::string;
    using peer_discovery_type = kythira::seed_peer_discovery<node_id_type, address_type>;
};

template<typename RaftTypes>
constexpr bool is_joinable =
    std::is_same_v<typename kythira::node_config<RaftTypes>::peer_discovery_type,
                   kythira::seed_peer_discovery<node_id_type, std::string>>;

// Adds quorum_manager_type so that _quorum_manager_type_traits picks up
// docker_quorum_manager instead of the no_op fallback.
struct tcp_raft_types_with_docker_qm : tcp_raft_types_joinable {
    using quorum_manager_type = kythira::docker_quorum_manager<node_id_type, std::string>;
};

#ifdef KYTHIRA_HAS_AWS_SDK
// QUORUM_MANAGER=aws: the leader provisions and terminates EC2 instances.
struct tcp_raft_types_with_aws_qm : tcp_raft_types_joinable {
    using quorum_manager_type = kythira::aws_ec2_quorum_manager<node_id_type, std::string>;
};
#endif

// Inherits all type aliases from tcp_raft_types; swaps logger_type/
// metrics_type to the OTLP implementations (.kiro/specs/otlp-telemetry-backend/,
// Requirement 5.3). Selected at startup, not combined with
// tcp_raft_types_with_docker_qm above in this pass — OTLP + docker quorum
// manager together is future work, not required by that spec.
struct tcp_raft_types_with_otlp : host_tcp_raft_types {
    using logger_type = kythira::otlp_logger;
    using metrics_type = kythira::otlp_metrics;
};

// Self-hosted metrics backends (doc/TODO.md "Metrics Backends"). Same
// swap-one-alias pattern as tcp_raft_types_with_otlp; logger stays the
// default console_logger — these backends are metrics-only.
struct tcp_raft_types_with_prometheus : host_tcp_raft_types {
    using metrics_type = kythira::prometheus_metrics;
};
struct tcp_raft_types_with_victoriametrics : host_tcp_raft_types {
    using metrics_type = kythira::victoriametrics_metrics;
};
struct tcp_raft_types_with_telegraf : host_tcp_raft_types {
    using metrics_type = kythira::telegraf_metrics;
};

// Paired logger bundles (doc/TODO.md's logging-alongside-backends item):
// each ecosystem's logger is only selectable together with its metrics
// backend — config.hpp rejects an unpaired logger env var — so exactly
// three logger-bearing combinations exist, not a full logger×metrics matrix.
struct tcp_raft_types_with_prometheus_loki : tcp_raft_types_with_prometheus {
    using logger_type = kythira::loki_logger;
};
struct tcp_raft_types_with_victoriametrics_victorialogs : tcp_raft_types_with_victoriametrics {
    using logger_type = kythira::victorialogs_logger;
};
struct tcp_raft_types_with_telegraf_logs : tcp_raft_types_with_telegraf {
    using logger_type = kythira::telegraf_logger;
};
struct tcp_raft_types_with_netdata : host_tcp_raft_types {
    using metrics_type = kythira::netdata_metrics;
};

#ifdef KYTHIRA_CHAOS_NODE_RPC_TLS
// The RPC transport swapped for the mutual-TLS one (RPC_TLS_* in
// config.hpp); everything else is the plain path's.
struct tcp_raft_types_with_tls : host_tcp_raft_types {
    using network_client_type = kythira::basic_tls_tcp_rpc_client<node_id_type>;
    using network_server_type = kythira::basic_tls_tcp_rpc_server<node_id_type>;
};
#endif

}  // namespace chaos_node
