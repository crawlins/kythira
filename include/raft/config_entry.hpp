#pragma once

#include <raft/types.hpp>
#include <boost/json.hpp>
#include <cstddef>
#include <string>
#include <vector>

// Helpers for encoding/decoding cluster_configuration as log entry command bytes.
// These are used for entry_type::configuration log entries; the payload is a JSON
// object so it is human-readable in debugging tools and self-describing.

namespace kythira {

template<typename NodeId>
requires node_id<NodeId>
auto serialize_configuration(const cluster_configuration<NodeId>& cfg) -> std::vector<std::byte> {
    boost::json::object obj;

    boost::json::array nodes;
    for (const auto& n : cfg.nodes()) {
        if constexpr (std::same_as<NodeId, std::string>) {
            nodes.push_back(boost::json::string(n));
        } else {
            nodes.push_back(static_cast<std::uint64_t>(n));
        }
    }
    obj["nodes"] = std::move(nodes);
    obj["is_joint_consensus"] = cfg.is_joint_consensus();

    if (cfg.is_joint_consensus() && cfg.old_nodes()) {
        boost::json::array old_nodes;
        for (const auto& n : *cfg.old_nodes()) {
            if constexpr (std::same_as<NodeId, std::string>) {
                old_nodes.push_back(boost::json::string(n));
            } else {
                old_nodes.push_back(static_cast<std::uint64_t>(n));
            }
        }
        obj["old_nodes"] = std::move(old_nodes);
    }

    // Learners are serialized unconditionally (not gated on is_joint_consensus()) because
    // they are meaningful in every configuration state, joint or not.
    boost::json::array learners;
    for (const auto& n : cfg.learners()) {
        if constexpr (std::same_as<NodeId, std::string>) {
            learners.push_back(boost::json::string(n));
        } else {
            learners.push_back(static_cast<std::uint64_t>(n));
        }
    }
    obj["learners"] = std::move(learners);

    auto s = boost::json::serialize(obj);
    std::vector<std::byte> out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

template<typename NodeId>
requires node_id<NodeId>
auto deserialize_configuration(const std::vector<std::byte>& data)
    -> cluster_configuration<NodeId> {
    std::string s;
    s.reserve(data.size());
    for (std::byte b : data) {
        s.push_back(static_cast<char>(b));
    }

    auto obj = boost::json::parse(s).as_object();
    cluster_configuration<NodeId> cfg;

    for (const auto& n : obj["nodes"].as_array()) {
        if constexpr (std::same_as<NodeId, std::string>) {
            cfg._nodes.emplace_back(n.as_string());
        } else {
            cfg._nodes.push_back(static_cast<NodeId>(n.as_int64()));
        }
    }

    cfg._is_joint_consensus =
        obj.contains("is_joint_consensus") ? obj["is_joint_consensus"].as_bool() : false;

    if (cfg._is_joint_consensus && obj.contains("old_nodes")) {
        std::vector<NodeId> old_nodes;
        for (const auto& n : obj["old_nodes"].as_array()) {
            if constexpr (std::same_as<NodeId, std::string>) {
                old_nodes.emplace_back(n.as_string());
            } else {
                old_nodes.push_back(static_cast<NodeId>(n.as_int64()));
            }
        }
        cfg._old_nodes = std::move(old_nodes);
    }

    // Absent "learners" key (entries written before this feature existed) leaves
    // cfg._learners at its default-constructed empty vector.
    if (obj.contains("learners")) {
        for (const auto& n : obj["learners"].as_array()) {
            if constexpr (std::same_as<NodeId, std::string>) {
                cfg._learners.emplace_back(n.as_string());
            } else {
                cfg._learners.push_back(static_cast<NodeId>(n.as_int64()));
            }
        }
    }

    return cfg;
}

}  // namespace kythira
