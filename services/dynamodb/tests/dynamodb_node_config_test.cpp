// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The DynamoDB module's settings in kythira_node
// (.kiro/specs/service-colocation/ task 6, Requirements 8.1 and 8.2): the
// node's KYTHIRA_NODE_* settings fill in what the module's own variables
// leave unset, the peer gateways default to each peer's host and this
// node's API port, and the data directory is always the node's dynamodb/.

#define BOOST_TEST_MODULE dynamodb_node_config_test

#include <boost/test/unit_test.hpp>

#include "composite_env.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace ddb = kythira::dynamodb;

namespace {

auto lookup(std::map<std::string, std::string> vars) -> ddb::env_lookup {
    return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(std::string(name));
        if (it == vars.end() || it->second.empty()) {
            return std::nullopt;
        }
        return it->second;
    };
}

auto node_vars() -> std::map<std::string, std::string> {
    return {
        {"KYTHIRA_NODE_ID", "2"},
        {"KYTHIRA_NODE_PEERS", "1=10.0.0.1:7400,2=10.0.0.2:7400,3=[fd00::3]:7400"},
        {"KYTHIRA_NODE_DATA_DIR", "/var/lib/kythira"},
        {"KYTHIRA_NODE_TLS_CERT", "/etc/kythira/node.pem"},
        {"KYTHIRA_NODE_TLS_KEY", "/etc/kythira/node.key"},
        {"KYTHIRA_NODE_TLS_CA", "/etc/kythira/ca.pem"},
        {"KYTHIRA_DYNAMODB_STATIC_KEYS", "AKIDADMIN:secretsecret:111122223333:admin:admin"},
        {"KYTHIRA_DYNAMODB_FORWARD_SECRET", "0123456789abcdef-node"},
    };
}

auto parse(const ddb::env_lookup& env) -> ddb::gateway_config {
    auto r = ddb::gateway_config_from_env(env);
    if (const auto* e = std::get_if<std::string>(&r)) {
        BOOST_FAIL(*e);
    }
    return std::get<ddb::gateway_config>(std::move(r));
}

}  // namespace

BOOST_AUTO_TEST_CASE(the_node_settings_fill_in) {
    const auto c = parse(dynamodb_gateway::composite_env(lookup(node_vars())));
    BOOST_TEST(c.data_dir == "/var/lib/kythira/dynamodb");
    BOOST_TEST(c.cluster.node_id == "2");
    BOOST_TEST(c.cluster.peers.size() == 3U);
    BOOST_TEST(c.cluster.peers.at("2") == "https://10.0.0.2:7400");
    BOOST_TEST(c.cluster.raft_port == 7400);
    BOOST_TEST(c.cluster.forward_ca_path == "/etc/kythira/ca.pem");
    BOOST_TEST(!c.http.insecure_plaintext);
    BOOST_TEST(c.http.tls_cert_path == "/etc/kythira/node.pem");
    // Every other peer's gateway: its host and this node's API port.
    BOOST_TEST(c.cluster.peer_gateways.size() == 2U);
    BOOST_TEST(c.cluster.peer_gateways.at("1").host == "10.0.0.1");
    BOOST_TEST(c.cluster.peer_gateways.at("1").port == 8000);
    BOOST_TEST(c.cluster.peer_gateways.at("3").host == "fd00::3");
}

BOOST_AUTO_TEST_CASE(the_module_variables_win) {
    auto vars = node_vars();
    vars["KYTHIRA_DYNAMODB_PORT"] = "9000";
    vars["KYTHIRA_DYNAMODB_TLS_CERT"] = "/etc/ddb/api.pem";
    vars["KYTHIRA_DYNAMODB_TLS_KEY"] = "/etc/ddb/api.key";
    vars["KYTHIRA_DYNAMODB_PEER_GATEWAYS"] = "1=gw1:8443,3=gw3:8443";
    const auto c = parse(dynamodb_gateway::composite_env(lookup(vars)));
    BOOST_TEST(c.http.tls_cert_path == "/etc/ddb/api.pem");
    BOOST_TEST(c.cluster.peer_gateways.at("1").host == "gw1");
    BOOST_TEST(c.cluster.peer_gateways.at("3").port == 8443);
}

BOOST_AUTO_TEST_CASE(the_api_port_moves_the_default_gateways) {
    auto vars = node_vars();
    vars["KYTHIRA_DYNAMODB_PORT"] = "9000";
    const auto c = parse(dynamodb_gateway::composite_env(lookup(vars)));
    BOOST_TEST(c.cluster.peer_gateways.at("1").port == 9000);
}

BOOST_AUTO_TEST_CASE(the_data_directory_is_always_the_nodes) {
    auto vars = node_vars();
    vars["KYTHIRA_DYNAMODB_DATA_DIR"] = "/somewhere/else";
    const auto c = parse(dynamodb_gateway::composite_env(lookup(vars)));
    BOOST_TEST(c.data_dir == "/var/lib/kythira/dynamodb");
}

BOOST_AUTO_TEST_CASE(a_plaintext_node_serves_http) {
    auto vars = node_vars();
    vars.erase("KYTHIRA_NODE_TLS_CERT");
    vars.erase("KYTHIRA_NODE_TLS_KEY");
    vars.erase("KYTHIRA_NODE_TLS_CA");
    vars["KYTHIRA_NODE_INSECURE_PLAINTEXT"] = "1";
    const auto c = parse(dynamodb_gateway::composite_env(lookup(vars)));
    BOOST_TEST(c.http.insecure_plaintext);
    BOOST_TEST(c.cluster.peers.at("1") == "http://10.0.0.1:7400");
}

BOOST_AUTO_TEST_CASE(a_node_without_peers_runs_without_raft) {
    auto vars = node_vars();
    vars.erase("KYTHIRA_NODE_PEERS");
    const auto c = parse(dynamodb_gateway::composite_env(lookup(vars)));
    BOOST_TEST(!c.cluster.enabled());
}
