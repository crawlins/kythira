// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

// The variables the DynamoDB module reads in kythira_node
// (.kiro/specs/service-colocation/ task 6, Requirement 8.1).
//
// - KYTHIRA_DYNAMODB_DATA_DIR is always the node's data directory's
//   dynamodb/.
// - Any other variable set in `env` is taken as it is.
// - Otherwise the node's settings fill in:
//   - KYTHIRA_DYNAMODB_NODE_ID from KYTHIRA_NODE_ID.
//   - KYTHIRA_DYNAMODB_PEERS from KYTHIRA_NODE_PEERS (`id=host:port`, with
//     the scheme added).
//   - KYTHIRA_DYNAMODB_RAFT_BIND and _PORT from the node's.
//   - The API's certificate and key from KYTHIRA_NODE_TLS_CERT and _KEY.
//   - The peer gateways' CA (KYTHIRA_DYNAMODB_FORWARD_CA) from
//     KYTHIRA_NODE_TLS_CA.
//   - KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT from
//     KYTHIRA_NODE_INSECURE_PLAINTEXT=1.
//   - KYTHIRA_DYNAMODB_PEER_GATEWAYS from each other peer's host and this
//     node's API port, as Kinesis's peer gateways default.

#include <kythira/dynamodb/gateway.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dynamodb_gateway {

namespace composite_detail {

/// `id=host:port` entries of KYTHIRA_NODE_PEERS, as (id, host:port).
[[nodiscard]] inline auto node_peers(std::string_view text)
    -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> out;
    std::string tok;
    for (const char c : std::string(text) + ',') {
        if (c != ',') {
            if (c != ' ') {
                tok += c;
            }
            continue;
        }
        if (!tok.empty()) {
            const auto eq = tok.find('=');
            out.emplace_back(eq == std::string::npos ? tok : tok.substr(0, eq),
                             eq == std::string::npos ? std::string() : tok.substr(eq + 1));
        }
        tok.clear();
    }
    return out;
}

/// The host of `host:port` or `[v6]:port`, brackets kept.
[[nodiscard]] inline auto host_of(const std::string& host_port) -> std::string {
    if (!host_port.empty() && host_port.front() == '[') {
        const auto close = host_port.find(']');
        return close == std::string::npos ? host_port : host_port.substr(0, close + 1);
    }
    const auto colon = host_port.rfind(':');
    return colon == std::string::npos ? host_port : host_port.substr(0, colon);
}

}  // namespace composite_detail

[[nodiscard]] inline auto composite_env(kythira::dynamodb::env_lookup env)
    -> kythira::dynamodb::env_lookup {
    return [env = std::move(env)](std::string_view name) -> std::optional<std::string> {
        const bool plaintext = env("KYTHIRA_NODE_INSECURE_PLAINTEXT").value_or("") == "1";
        if (name == "KYTHIRA_DYNAMODB_DATA_DIR") {
            auto dir = env("KYTHIRA_NODE_DATA_DIR");
            if (!dir) {
                return std::nullopt;
            }
            return (std::filesystem::path(*dir) / "dynamodb").string();
        }
        if (auto v = env(name)) {
            return v;
        }
        if (name == "KYTHIRA_DYNAMODB_NODE_ID") {
            return env("KYTHIRA_NODE_ID");
        }
        if (name == "KYTHIRA_DYNAMODB_PEERS") {
            const auto peers = env("KYTHIRA_NODE_PEERS");
            if (!peers) {
                return std::nullopt;
            }
            std::string out;
            for (const auto& [id, hp] : composite_detail::node_peers(*peers)) {
                out +=
                    (out.empty() ? "" : ",") + id + "=" + (plaintext ? "http://" : "https://") + hp;
            }
            return out;
        }
        if (name == "KYTHIRA_DYNAMODB_PEER_GATEWAYS") {
            const auto peers = env("KYTHIRA_NODE_PEERS");
            const auto self =
                env("KYTHIRA_DYNAMODB_NODE_ID").value_or(env("KYTHIRA_NODE_ID").value_or(""));
            if (!peers) {
                return std::nullopt;
            }
            const auto port = env("KYTHIRA_DYNAMODB_PORT").value_or("8000");
            std::string out;
            for (const auto& [id, hp] : composite_detail::node_peers(*peers)) {
                if (id != self) {
                    out += (out.empty() ? "" : ",") + id + "=" + composite_detail::host_of(hp) +
                           ":" + port;
                }
            }
            return out;
        }
        if (name == "KYTHIRA_DYNAMODB_RAFT_BIND") {
            return env("KYTHIRA_NODE_RAFT_BIND");
        }
        if (name == "KYTHIRA_DYNAMODB_RAFT_PORT") {
            return env("KYTHIRA_NODE_RAFT_PORT").value_or("7400");  // node_config's default
        }
        if (name == "KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT") {
            return plaintext ? std::optional<std::string>("1") : std::nullopt;
        }
        if (name == "KYTHIRA_DYNAMODB_TLS_CERT" && !plaintext) {
            return env("KYTHIRA_NODE_TLS_CERT");
        }
        if (name == "KYTHIRA_DYNAMODB_TLS_KEY" && !plaintext) {
            return env("KYTHIRA_NODE_TLS_KEY");
        }
        if (name == "KYTHIRA_DYNAMODB_FORWARD_CA") {
            return env("KYTHIRA_NODE_TLS_CA");
        }
        return std::nullopt;
    };
}

}  // namespace dynamodb_gateway
