// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The Kinesis module's settings in kythira_node
// (.kiro/specs/service-colocation/ task 5, Requirements 8.1 and 8.2): the
// node's KYTHIRA_NODE_* settings fill in what the module's own variables
// leave unset, the data directory is always the node's kinesis/, and the
// stand-alone daemon reads exactly what it always has.

#define BOOST_TEST_MODULE kinesis_node_config_test

#include <boost/test/unit_test.hpp>

#include "config.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace kn = kythira::kinesis_node;
namespace sn = kythira::service_node;

namespace {

auto lookup(std::map<std::string, std::string> vars) -> sn::env_lookup {
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
        {"KYTHIRA_NODE_PEERS", "1=10.0.0.1:7400,2=10.0.0.2:7400,3=10.0.0.3:7400"},
        {"KYTHIRA_NODE_DATA_DIR", "/var/lib/kythira"},
        {"KYTHIRA_NODE_RAFT_BIND", "0.0.0.0"},
        {"KYTHIRA_NODE_TLS_CERT", "/etc/kythira/node.pem"},
        {"KYTHIRA_NODE_TLS_KEY", "/etc/kythira/node.key"},
        {"KYTHIRA_NODE_TLS_CA", "/etc/kythira/ca.pem"},
        {"KINESIS_STATIC_KEYS_FILE", "/etc/kythira/kinesis-keys"},
        {"KINESIS_FORWARD_SECRET", "0123456789abcdef0123"},
    };
}

}  // namespace

BOOST_AUTO_TEST_CASE(the_node_settings_fill_in) {
    const auto o = kn::from_env(kn::composite_env(lookup(node_vars())));
    BOOST_TEST(kythira::host_node_id_text(o._node_id) == "2");
    BOOST_TEST(o._data_dir.string() == "/var/lib/kythira/kinesis");
    BOOST_TEST(o._peers.size() == 3U);
    BOOST_TEST(o._peers.at(o._node_id) == "https://10.0.0.2:7400");
    BOOST_TEST(o._raft_port == 7400);
    // The node's mutual TLS: the API's certificate and the peers' CA.
    BOOST_TEST(o._tls_cert_path == "/etc/kythira/node.pem");
    BOOST_TEST(o._tls_key_path == "/etc/kythira/node.key");
    BOOST_TEST(o._peer_ca_path == "/etc/kythira/ca.pem");
    BOOST_TEST(!o._allow_plaintext);
    // Raft is the node's listener: the daemon's plaintext guard is off,
    // even bound off loopback.
    BOOST_TEST(o._allow_plaintext_raft);
    // The peer gateways default to each Raft host with this node's API port.
    BOOST_TEST(o._peer_gateways.size() == 3U);
    for (const auto& [id, gw] : o._peer_gateways) {
        BOOST_TEST(gw.ends_with(":4567"));
    }
}

BOOST_AUTO_TEST_CASE(the_module_variables_win) {
    auto vars = node_vars();
    vars["KINESIS_TLS_CERT"] = "/etc/kinesis/api.pem";
    vars["KINESIS_TLS_KEY"] = "/etc/kinesis/api.key";
    vars["KINESIS_PEER_CA"] = "/etc/kinesis/ca.pem";
    vars["KYTHIRA_PEERS"] = "1=https://k1:7400,2=https://k2:7400";
    const auto o = kn::from_env(kn::composite_env(lookup(vars)));
    BOOST_TEST(o._tls_cert_path == "/etc/kinesis/api.pem");
    BOOST_TEST(o._peer_ca_path == "/etc/kinesis/ca.pem");
    BOOST_TEST(o._peers.size() == 2U);
}

BOOST_AUTO_TEST_CASE(the_data_directory_is_always_the_nodes) {
    auto vars = node_vars();
    vars["KINESIS_DATA_DIR"] = "/somewhere/else";
    const auto o = kn::from_env(kn::composite_env(lookup(vars)));
    BOOST_TEST(o._data_dir.string() == "/var/lib/kythira/kinesis");
}

BOOST_AUTO_TEST_CASE(a_plaintext_node_serves_http) {
    auto vars = node_vars();
    vars.erase("KYTHIRA_NODE_TLS_CERT");
    vars.erase("KYTHIRA_NODE_TLS_KEY");
    vars.erase("KYTHIRA_NODE_TLS_CA");
    vars["KYTHIRA_NODE_INSECURE_PLAINTEXT"] = "1";
    const auto o = kn::from_env(kn::composite_env(lookup(vars)));
    BOOST_TEST(o._allow_plaintext);
    BOOST_TEST(o._tls_cert_path.empty());
    for (const auto& [id, url] : o._peers) {
        BOOST_TEST(url.starts_with("http://"));
    }
}

BOOST_AUTO_TEST_CASE(stand_alone_reads_the_daemons_variables) {
    // The module interface's lookup over the daemon's own variables: no
    // KYTHIRA_NODE_* fallback, and KINESIS_DATA_DIR as given.
    const auto o = kn::from_env(lookup({
        {"KYTHIRA_NODE_ID", "1"},
        {"KYTHIRA_PEERS", "1=http://127.0.0.1:7001"},
        {"KYTHIRA_RAFT_BIND", "127.0.0.1"},
        {"KINESIS_ALLOW_PLAINTEXT", "true"},
        {"KINESIS_STATIC_KEYS_FILE", "/k"},
        {"KINESIS_DATA_DIR", "/data/kinesis-1"},
        {"KYTHIRA_NODE_TLS_CERT", "/ignored.pem"},
    }));
    BOOST_TEST(o._data_dir.string() == "/data/kinesis-1");
    BOOST_TEST(o._raft_port == 7000);
    BOOST_TEST(o._tls_cert_path.empty());
    BOOST_TEST(!o._allow_plaintext_raft);
    // Off loopback without the opt-in, the daemon still refuses plaintext
    // Raft.
    BOOST_CHECK_THROW(kn::from_env(lookup({{"KYTHIRA_NODE_ID", "1"},
                                           {"KYTHIRA_PEERS", "1=http://10.0.0.1:7001"},
                                           {"KINESIS_ALLOW_PLAINTEXT", "true"},
                                           {"KINESIS_STATIC_KEYS_FILE", "/k"},
                                           {"KINESIS_DATA_DIR", "/d"}})),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(a_composite_node_without_a_data_directory_is_refused) {
    auto vars = node_vars();
    vars.erase("KYTHIRA_NODE_DATA_DIR");
    BOOST_CHECK_EXCEPTION(kn::from_env(kn::composite_env(lookup(vars))), std::invalid_argument,
                          [](const std::invalid_argument& e) {
                              return std::string_view(e.what()).find("KINESIS_DATA_DIR") !=
                                     std::string_view::npos;
                          });
}
