// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The gateway side of an embedded IAM replica (iam_gateway_auth.hpp,
// DynamoDB spec task 28): the KYTHIRA_IAM_* settings the DynamoDB and
// Kinesis gateways read, their defaults and their errors. The replica and
// its hooks run in iam_embedded_replica_test and the gateways' own tests.

#define BOOST_TEST_MODULE iam_gateway_auth_test

#include <boost/test/unit_test.hpp>

#include <kythira/iam/iam_gateway_auth.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace iam = kythira::iam;

namespace {

using vars = std::map<std::string, std::string>;

auto full() -> vars {
    return {{"KYTHIRA_IAM_VOTERS", "1:iam-1:7100,2:iam-2:7100,3:iam-3:7100"},
            {"KYTHIRA_IAM_REPLICA_NODE_ID", "9"},
            {"KYTHIRA_IAM_REPLICA_RPC_PORT", "7300"},
            {"KYTHIRA_IAM_DATA_DIR", "/var/lib/iam-replica"},
            {"KYTHIRA_IAM_UNSEAL_KEY_FILE", "/run/secrets/iam-unseal"},
            {"KYTHIRA_IAM_RPC_TLS_CERT", "/etc/iam/node.crt"},
            {"KYTHIRA_IAM_RPC_TLS_KEY", "/etc/iam/node.key"},
            {"KYTHIRA_IAM_RPC_TLS_CA", "/etc/iam/ca.crt"}};
}

auto read(const vars& v) -> iam::embedded_replica_config {
    return iam::gateway_replica_config(
        [&](const char* name) -> std::optional<std::string> {
            const auto it = v.find(name);
            if (it == v.end() || it->second.empty()) {
                return std::nullopt;
            }
            return it->second;
        },
        "with KINESIS_AUTH=iam or both");
}

auto error_of(const vars& v) -> std::string {
    try {
        (void)read(v);
    } catch (const std::invalid_argument& e) {
        return e.what();
    }
    return "ok";
}

}  // namespace

BOOST_AUTO_TEST_CASE(settings_are_read_with_their_defaults) {
    const auto r = read(full());
    BOOST_TEST(r.voters.size() == 3U);
    BOOST_TEST(r.voters.at(1).rpc_host == "iam-2");
    BOOST_TEST(r.voters.at(1).rpc_port == 7100);
    BOOST_TEST(kythira::host_node_id_text(r.node_id) == "9");
    BOOST_TEST(r.rpc_address == "0.0.0.0");
    BOOST_TEST(r.rpc_port == 7300);
    BOOST_TEST(r.data_dir == "/var/lib/iam-replica");
    BOOST_TEST(r.unseal_key_file == "/run/secrets/iam-unseal");
    BOOST_TEST(r.rpc_tls_ca == "/etc/iam/ca.crt");
    BOOST_TEST((r.max_staleness == std::chrono::seconds(60)));
    BOOST_TEST(r.partition == "aws");

    auto v = full();
    v["KYTHIRA_IAM_REPLICA_RPC_BIND"] = "10.0.0.9";
    v["KYTHIRA_IAM_MAX_STALENESS_SECS"] = "5";
    v["KYTHIRA_IAM_ARN_PARTITION"] = "aws-cn";
    const auto set = read(v);
    BOOST_TEST(set.rpc_address == "10.0.0.9");
    BOOST_TEST((set.max_staleness == std::chrono::seconds(5)));
    BOOST_TEST(set.partition == "aws-cn");
}

BOOST_AUTO_TEST_CASE(missing_and_malformed_settings_are_named) {
    for (const char* name :
         {"KYTHIRA_IAM_VOTERS", "KYTHIRA_IAM_REPLICA_NODE_ID", "KYTHIRA_IAM_REPLICA_RPC_PORT",
          "KYTHIRA_IAM_DATA_DIR", "KYTHIRA_IAM_UNSEAL_KEY_FILE", "KYTHIRA_IAM_RPC_TLS_CERT",
          "KYTHIRA_IAM_RPC_TLS_KEY", "KYTHIRA_IAM_RPC_TLS_CA"}) {
        auto v = full();
        v.erase(name);
        BOOST_TEST(error_of(v) == std::string(name) + " is required with KINESIS_AUTH=iam or both");
    }
    auto v = full();
    v["KYTHIRA_IAM_REPLICA_RPC_PORT"] = "70000";
    BOOST_TEST(error_of(v) == "KYTHIRA_IAM_REPLICA_RPC_PORT must be an integer from 1 to 65535");
    v = full();
    v["KYTHIRA_IAM_MAX_STALENESS_SECS"] = "soon";
    BOOST_TEST(error_of(v).starts_with("KYTHIRA_IAM_MAX_STALENESS_SECS must be an integer"));
    v = full();
    v["KYTHIRA_IAM_VOTERS"] = "1:iam-1";
    BOOST_TEST(error_of(v).starts_with("IAM voter '1:iam-1'"));
}
