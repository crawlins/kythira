// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// QUORUM_MANAGER=aws: the leader owns an aws_ec2_quorum_manager, the EC2
// counterpart of QUORUM_MANAGER=docker (.kiro/specs/aws-quorum-manager/
// task 8). A replacement is launched with user data that starts chaos_node
// with JOIN=1, so the leader admits it through ClusterJoin exactly as it
// admits a docker replacement.
//
// The parsing helpers and the user data need no AWS SDK; the manager's
// configuration (and so QUORUM_MANAGER=aws) exists only in an SDK build.
// tests/chaos_node_config_unit_test.cpp checks both.

#include "config.hpp"

#include <raft/aws_ec2_quorum_manager.hpp>
#include <raft/host_node_id.hpp>

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chaos_node {

/// "a=b,c=d" -> {{a,b},{c,d}}. Splits each item at its LAST '=', so a key
/// may be composite node-id text; neither part may be empty.
[[nodiscard]] inline auto parse_assignments(std::string_view text, std::string_view var)
    -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find(',', start);
        if (end == std::string_view::npos) end = text.size();
        auto item = text.substr(start, end - start);
        if (!item.empty()) {
            auto eq = item.rfind('=');
            if (eq == std::string_view::npos || eq == 0 || eq + 1 == item.size()) {
                throw std::invalid_argument("chaos_node: " + std::string(var) +
                                            ": expected key=value, got: " + std::string(item));
            }
            out.emplace_back(std::string(item.substr(0, eq)), std::string(item.substr(eq + 1)));
        }
        start = end + 1;
    }
    return out;
}

/// "a,b" -> {a,b}, empty items dropped.
[[nodiscard]] inline auto parse_list(std::string_view text) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find(',', start);
        if (end == std::string_view::npos) end = text.size();
        if (end > start) out.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

/// The bash user data a replacement boots with: write chaos_node's
/// environment, take SELF_ADDRESS from the instance's private IP (IMDSv2,
/// which the manager requires), and start `node_command` detached.
///
/// A numeric build hands over the id the manager allocated ({NODE_ID}); a
/// text build cannot know the instance id before launch, so the node reads
/// it from instance metadata. Like a docker replacement, this node runs no
/// quorum manager of its own: a cluster whose original members have all
/// been replaced stops healing.
///
/// The manager substitutes {NODE_ID}, {NODE_PORT}, {CLUSTER} and {AZ}, so
/// the script uses no other braces.
[[nodiscard]] inline auto replacement_user_data(const node_config& cfg,
                                                const std::string& node_command) -> std::string {
    const std::string env_file = "/etc/default/chaos_node";
    std::string env;
    auto line = [&](const std::string& key, const std::string& value) {
        env += key + '=' + value + '\n';
    };
    line("NODE_ID", kythira::host_node_id_is_text ? "metadata:aws" : "{NODE_ID}");
    line("JOIN", "1");
    line("PEERS", cfg.peers_env());
    line("RPC_PORT", std::to_string(cfg.rpc_port));
    line("HTTP_PORT", std::to_string(cfg.http_port));
    line("FIU_PORT", std::to_string(cfg.fiu_port));
    line("DATA_DIR", cfg.data_dir);
    line("ELECTION_TIMEOUT_MIN_MS", std::to_string(cfg.election_timeout_min.count()));
    line("ELECTION_TIMEOUT_MAX_MS", std::to_string(cfg.election_timeout_max.count()));
    line("HEARTBEAT_INTERVAL_MS", std::to_string(cfg.heartbeat_interval.count()));
    if (cfg.rpc_timeout) line("RPC_TIMEOUT_MS", std::to_string(cfg.rpc_timeout->count()));

    std::string script = "#!/bin/bash\nset -eu\n";
    script += "imds=http://169.254.169.254/latest\n";
    script +=
        "token=$(curl -sS -X PUT $imds/api/token "
        "-H 'X-aws-ec2-metadata-token-ttl-seconds: 300')\n";
    script +=
        "ip=$(curl -sS -H \"X-aws-ec2-metadata-token: $token\" "
        "$imds/meta-data/local-ipv4)\n";
    script += "cat > " + env_file + " <<'KYTHIRA_ENV'\n" + env + "KYTHIRA_ENV\n";
    script +=
        "echo \"SELF_ADDRESS=$ip:" + std::to_string(cfg.rpc_port) + "\" >> " + env_file + '\n';
    script += "mkdir -p " + cfg.data_dir + '\n';
    script += "set -a\n. " + env_file + "\nset +a\n";
    script += "setsid nohup " + node_command + " > /var/log/chaos_node.log 2>&1 < /dev/null &\n";
    return script;
}

#ifdef KYTHIRA_HAS_AWS_SDK

struct aws_quorum_settings {
    kythira::aws_ec2_quorum_manager_config manager;
    /// QUORUM_PLACEMENT: the placement group (AZ) of every bootstrap member.
    std::unordered_map<node_id_type, std::string> placement;
};

/// QUORUM_MANAGER=aws's configuration, from:
///
///   QUORUM_CLUSTER              cluster name, the kythira:cluster tag (required)
///   QUORUM_AWS_IMAGE            AMI with chaos_node installed (required)
///   QUORUM_AWS_SUBNETS          AZ1=subnet-..,AZ2=subnet-.. one group per entry (required)
///   QUORUM_PLACEMENT            <id>=<group> for every bootstrap member (required)
///   QUORUM_TARGET               voters per group (default 1)
///   QUORUM_AWS_REGION           default: the SDK's (AWS_REGION, then IMDS)
///   QUORUM_AWS_INSTANCE_TYPE    default t3.micro
///   QUORUM_AWS_SECURITY_GROUPS  sg-..,sg-..
///   QUORUM_AWS_INSTANCE_PROFILE replacement's instance profile name
///   QUORUM_AWS_KEY_NAME         replacement's EC2 key pair
///   QUORUM_AWS_NODE_COMMAND     default /usr/local/bin/chaos_node
[[nodiscard]] inline auto aws_quorum_from_env(const node_config& cfg) -> aws_quorum_settings {
    auto get = [](const char* name, const char* def = nullptr) -> std::string {
        const char* v = std::getenv(name);
        if (v != nullptr && *v != '\0') return v;
        if (def != nullptr) return def;
        throw std::invalid_argument(std::string("chaos_node: QUORUM_MANAGER=aws needs ") + name);
    };

    aws_quorum_settings out;
    auto& m = out.manager;
    m.cluster_name = get("QUORUM_CLUSTER");
    m.image_id = get("QUORUM_AWS_IMAGE");
    m.instance_type = get("QUORUM_AWS_INSTANCE_TYPE", "t3.micro");
    m.node_port = cfg.rpc_port;
    m.aws.region = get("QUORUM_AWS_REGION", "");
    m.iam_instance_profile = get("QUORUM_AWS_INSTANCE_PROFILE", "");
    m.key_name = get("QUORUM_AWS_KEY_NAME", "");
    m.security_group_ids = parse_list(get("QUORUM_AWS_SECURITY_GROUPS", ""));

    const auto target = std::stoul(get("QUORUM_TARGET", "1"));
    for (auto& [group, subnet] :
         parse_assignments(get("QUORUM_AWS_SUBNETS"), "QUORUM_AWS_SUBNETS")) {
        m.topology.groups.push_back({.group_id = group, .target_count = target});
        m.subnet_by_group.emplace(std::move(group), std::move(subnet));
    }

    for (auto& [id_text, group] : parse_assignments(get("QUORUM_PLACEMENT"), "QUORUM_PLACEMENT")) {
        auto id = kythira::parse_host_peer_node_id(id_text);
        if (!id) {
            throw std::invalid_argument("chaos_node: QUORUM_PLACEMENT: " + id.error());
        }
        if (!m.subnet_by_group.contains(group)) {
            throw std::invalid_argument("chaos_node: QUORUM_PLACEMENT puts " + id_text +
                                        " in group " + group + ", which QUORUM_AWS_SUBNETS lacks");
        }
        out.placement.emplace(std::move(id).value(), std::move(group));
    }
    // A member with no group would never be counted toward a target, so its
    // loss could not be replaced.
    for (const auto& id : cfg.all_node_ids()) {
        if (!out.placement.contains(id)) {
            throw std::invalid_argument("chaos_node: QUORUM_PLACEMENT has no group for node " +
                                        kythira::host_node_id_text(id));
        }
    }

    m.user_data_template =
        replacement_user_data(cfg, get("QUORUM_AWS_NODE_COMMAND", "/usr/local/bin/chaos_node"));
    return out;
}

#endif  // KYTHIRA_HAS_AWS_SDK

}  // namespace chaos_node
