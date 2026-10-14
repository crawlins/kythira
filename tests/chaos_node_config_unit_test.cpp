// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// chaos_node's environment parsing for joining a running cluster
// (quorum-management Req 19): the variables docker_quorum_manager hands a
// node it provisions, and the addresses the node derives from them.

#define BOOST_TEST_MODULE chaos_node_config_unit_test
#include <boost/test/unit_test.hpp>

#include "config.hpp"

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using chaos_node::node_config;

constexpr const char* k_vars[] = {
    "NODE_ID",
    "KYTHIRA_NODE_ID",
    "PEERS",
    "RPC_PORT",
    "JOIN",
    "SELF_ADDRESS",
    "QUORUM_CLUSTER",
    "KYTHIRA_CLUSTER",
    "PEER_ADDRESS_TEMPLATE",
    "QUORUM_CHECK_INTERVAL_MS",
    "RPC_TLS_CERT",
    "RPC_TLS_KEY",
    "RPC_TLS_CA",
    "QUORUM_MANAGER",
    "OTLP_ENDPOINT",
};

// Clears every variable under test, and again on destruction.
struct clean_env {
    clean_env() { clear(); }
    ~clean_env() { clear(); }
    clean_env(const clean_env&) = delete;
    auto operator=(const clean_env&) -> clean_env& = delete;

    static void clear() {
        for (const char* v : k_vars) {
            ::unsetenv(v);
        }
    }
};

}  // namespace

BOOST_FIXTURE_TEST_CASE(founder_defaults_are_unchanged, clean_env) {
    ::setenv("NODE_ID", "1", 1);
    ::setenv("PEERS", "2:qnode2:7000,3:qnode3:7000", 1);
    auto cfg = node_config::from_env();
    BOOST_TEST(cfg.node_id == 1u);
    BOOST_TEST(!cfg.join);
    BOOST_TEST(cfg.peers.size() == 2u);
    BOOST_TEST(cfg.self_address.empty());
    BOOST_TEST(!cfg.peer_address_template.has_value());
    BOOST_TEST(!cfg.quorum_check_interval.has_value());
}

BOOST_FIXTURE_TEST_CASE(node_id_falls_back_to_kythira_node_id, clean_env) {
    ::setenv("KYTHIRA_NODE_ID", "7", 1);
    ::setenv("PEERS", "1:a:7000", 1);
    BOOST_TEST(node_config::from_env().node_id == 7u);

    ::setenv("NODE_ID", "8", 1);
    BOOST_TEST(node_config::from_env().node_id == 8u);  // NODE_ID wins
}

BOOST_FIXTURE_TEST_CASE(missing_node_id_still_throws, clean_env) {
    ::setenv("PEERS", "1:a:7000", 1);
    BOOST_CHECK_THROW(node_config::from_env(), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(cluster_name_derives_container_addresses, clean_env) {
    ::setenv("KYTHIRA_NODE_ID", "4", 1);
    ::setenv("KYTHIRA_CLUSTER", "qt", 1);
    ::setenv("RPC_PORT", "7100", 1);
    ::setenv("JOIN", "1", 1);
    // A joiner's PEERS lists every member, itself included.
    ::setenv("PEERS", "1:kythira-qt-1:7100,4:kythira-qt-4:7100", 1);

    auto cfg = node_config::from_env();
    BOOST_TEST(cfg.join);
    BOOST_TEST(cfg.self_address == "kythira-qt-4:7100");
    BOOST_REQUIRE(cfg.peers.size() == 1u);
    BOOST_TEST(cfg.peers.front().node_id == 1u);

    auto resolved = cfg.resolve(9);
    BOOST_REQUIRE(resolved.has_value());
    BOOST_TEST(resolved->first == "kythira-qt-9");
    BOOST_TEST(resolved->second == 7100);
}

BOOST_FIXTURE_TEST_CASE(quorum_cluster_takes_precedence, clean_env) {
    ::setenv("NODE_ID", "1", 1);
    ::setenv("PEERS", "2:b:7000", 1);
    ::setenv("QUORUM_CLUSTER", "founders", 1);
    ::setenv("KYTHIRA_CLUSTER", "other", 1);
    BOOST_TEST(node_config::from_env().self_address == "kythira-founders-1:7000");
}

BOOST_FIXTURE_TEST_CASE(explicit_template_and_self_address, clean_env) {
    ::setenv("NODE_ID", "2", 1);
    ::setenv("PEERS", "1:127.0.0.1:7101", 1);
    ::setenv("PEER_ADDRESS_TEMPLATE", "127.0.0.1:710{id}", 1);
    auto cfg = node_config::from_env();
    BOOST_TEST(cfg.self_address == "127.0.0.1:7102");
    BOOST_TEST(cfg.resolve(5)->second == 7105);

    ::setenv("SELF_ADDRESS", "10.0.0.2:7000", 1);
    BOOST_TEST(node_config::from_env().self_address == "10.0.0.2:7000");
}

BOOST_FIXTURE_TEST_CASE(join_needs_an_address_and_a_seed, clean_env) {
    ::setenv("NODE_ID", "4", 1);
    ::setenv("JOIN", "1", 1);
    ::setenv("PEERS", "1:a:7000", 1);
    BOOST_CHECK_THROW(node_config::from_env(), std::invalid_argument);  // no address

    ::setenv("SELF_ADDRESS", "d:7000", 1);
    ::setenv("PEERS", "4:d:7000", 1);  // only itself
    BOOST_CHECK_THROW(node_config::from_env(), std::invalid_argument);

    ::setenv("JOIN", "yes", 1);
    BOOST_CHECK_THROW(node_config::from_env(), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(peers_env_lists_self_first, clean_env) {
    ::setenv("NODE_ID", "1", 1);
    ::setenv("PEERS", "2:kythira-c-2:7000,3:kythira-c-3:7000", 1);
    ::setenv("QUORUM_CLUSTER", "c", 1);
    BOOST_TEST(node_config::from_env().peers_env() ==
               "1:kythira-c-1:7000,2:kythira-c-2:7000,3:kythira-c-3:7000");
}

BOOST_FIXTURE_TEST_CASE(quorum_check_interval_is_parsed, clean_env) {
    ::setenv("NODE_ID", "1", 1);
    ::setenv("PEERS", "2:b:7000", 1);
    ::setenv("QUORUM_CHECK_INTERVAL_MS", "5000", 1);
    auto cfg = node_config::from_env();
    BOOST_REQUIRE(cfg.quorum_check_interval.has_value());
    BOOST_TEST(cfg.quorum_check_interval->count() == 5000);
}

BOOST_FIXTURE_TEST_CASE(rpc_tls_is_off_by_default, clean_env) {
    ::setenv("NODE_ID", "1", 1);
    ::setenv("PEERS", "2:b:7000", 1);
    BOOST_TEST(!node_config::from_env().rpc_tls());
}

BOOST_FIXTURE_TEST_CASE(rpc_tls_takes_all_three_paths, clean_env) {
    ::setenv("NODE_ID", "1", 1);
    ::setenv("PEERS", "2:b:7000", 1);
    ::setenv("RPC_TLS_CERT", "/ca/node1/cert.pem", 1);
    ::setenv("RPC_TLS_KEY", "/ca/node1/key.pem", 1);
    // A cert and key with no root to check peers against would trust nobody
    // (or, worse, anybody), so a partial set is refused.
    BOOST_CHECK_THROW(node_config::from_env(), std::invalid_argument);

    ::setenv("RPC_TLS_CA", "/ca/root_ca.pem", 1);
    auto cfg = node_config::from_env();
    BOOST_TEST(cfg.rpc_tls());
    BOOST_TEST(cfg.rpc_tls_cert_path == "/ca/node1/cert.pem");
    BOOST_TEST(cfg.rpc_tls_key_path == "/ca/node1/key.pem");
    BOOST_TEST(cfg.rpc_tls_ca_path == "/ca/root_ca.pem");
}

BOOST_FIXTURE_TEST_CASE(rpc_tls_refuses_combinations_it_cannot_honour, clean_env) {
    ::setenv("NODE_ID", "4", 1);
    ::setenv("PEERS", "1:a:7000", 1);
    ::setenv("RPC_TLS_CERT", "c", 1);
    ::setenv("RPC_TLS_KEY", "k", 1);
    ::setenv("RPC_TLS_CA", "r", 1);

    ::setenv("JOIN", "1", 1);
    ::setenv("SELF_ADDRESS", "d:7000", 1);
    BOOST_CHECK_THROW(node_config::from_env(), std::invalid_argument);
    ::unsetenv("JOIN");

    ::setenv("QUORUM_MANAGER", "docker", 1);
    BOOST_CHECK_THROW(node_config::from_env(), std::invalid_argument);
    ::unsetenv("QUORUM_MANAGER");

    ::setenv("OTLP_ENDPOINT", "http://collector:4318", 1);
    BOOST_CHECK_THROW(node_config::from_env(), std::invalid_argument);
    ::unsetenv("OTLP_ENDPOINT");

    BOOST_CHECK_NO_THROW(node_config::from_env());
}

BOOST_AUTO_TEST_CASE(split_host_port_rejects_malformed) {
    BOOST_TEST(!node_config::split_host_port("").has_value());
    BOOST_TEST(!node_config::split_host_port("host").has_value());
    BOOST_TEST(!node_config::split_host_port(":7000").has_value());
    BOOST_TEST(!node_config::split_host_port("host:").has_value());
    BOOST_TEST(!node_config::split_host_port("host:0").has_value());
    BOOST_TEST(!node_config::split_host_port("host:70000").has_value());
    BOOST_TEST(!node_config::split_host_port("host:abc").has_value());
    BOOST_TEST(node_config::split_host_port("host:7000")->first == "host");
}
