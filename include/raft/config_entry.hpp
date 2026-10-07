// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/types.hpp>
#include <boost/json.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <map>
#include <string>
#include <vector>

// Helpers for encoding/decoding cluster_configuration as log entry command bytes.
// These are used for entry_type::configuration log entries; the payload is a JSON
// object so it is human-readable in debugging tools and self-describing.

namespace kythira {

namespace config_entry_detail {

// A node id as JSON: a number for a numeric id, its text otherwise (a
// `std::string` as is, a composite id in canonical form).
template<typename NodeId> auto encode_id(const NodeId& id) -> boost::json::value {
    if constexpr (node_id_traits<NodeId>::is_textual) {
        return boost::json::string(node_id_traits<NodeId>::to_text(id));
    } else {
        return static_cast<std::uint64_t>(id);
    }
}

// The inverse of `encode_id`. A numeric id is read across the whole uint64
// range (boost::json keeps values past INT64_MAX as uint64, which `as_int64`
// refused) and must fit `NodeId`; a composite id's text must parse. A
// `std::string` id is taken verbatim, as before. Anything else throws, as a
// malformed entry always has.
template<typename NodeId> auto decode_id(const boost::json::value& v) -> NodeId {
    if constexpr (std::same_as<NodeId, std::string>) {
        return std::string{v.as_string()};
    } else if constexpr (node_id_traits<NodeId>::is_textual) {
        auto id = node_id_traits<NodeId>::from_text(std::string_view{v.as_string()});
        if (!id) {
            throw std::invalid_argument("configuration entry: invalid node id '" +
                                        std::string{v.as_string()} + "'");
        }
        return std::move(*id);
    } else {
        const auto n = v.to_number<std::uint64_t>();
        if (n > std::numeric_limits<NodeId>::max()) {
            throw std::out_of_range("configuration entry: node id " + std::to_string(n) +
                                    " out of range");
        }
        return static_cast<NodeId>(n);
    }
}

}  // namespace config_entry_detail

template<typename NodeId>
requires node_id<NodeId>
auto serialize_configuration(const cluster_configuration<NodeId>& cfg,
                             const std::map<NodeId, std::string>* placement = nullptr)
    -> std::vector<std::byte> {
    boost::json::object obj;

    boost::json::array nodes;
    for (const auto& n : cfg.nodes()) {
        nodes.push_back(config_entry_detail::encode_id(n));
    }
    obj["nodes"] = std::move(nodes);
    obj["is_joint_consensus"] = cfg.is_joint_consensus();

    if (cfg.is_joint_consensus() && cfg.old_nodes()) {
        boost::json::array old_nodes;
        for (const auto& n : *cfg.old_nodes()) {
            old_nodes.push_back(config_entry_detail::encode_id(n));
        }
        obj["old_nodes"] = std::move(old_nodes);
    }

    // Learners are serialized unconditionally (not gated on is_joint_consensus()) because
    // they are meaningful in every configuration state, joint or not.
    boost::json::array learners;
    for (const auto& n : cfg.learners()) {
        learners.push_back(config_entry_detail::encode_id(n));
    }
    obj["learners"] = std::move(learners);

    if (placement != nullptr && !placement->empty()) {
        // Placement-group assignments ride along with the configuration so
        // every replica, and so any future leader, groups nodes the way the
        // leader that provisioned them did (quorum-management Req 9.4/12.3).
        // An array of [node, group] pairs rather than an object, because a
        // numeric NodeId is not a JSON key.
        boost::json::array pairs;
        for (const auto& [n, group] : *placement) {
            boost::json::array pair;
            pair.push_back(config_entry_detail::encode_id(n));
            pair.push_back(boost::json::string(group));
            pairs.push_back(std::move(pair));
        }
        obj["placement"] = std::move(pairs);
    }

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
        cfg._nodes.push_back(config_entry_detail::decode_id<NodeId>(n));
    }

    cfg._is_joint_consensus =
        obj.contains("is_joint_consensus") ? obj["is_joint_consensus"].as_bool() : false;

    if (cfg._is_joint_consensus && obj.contains("old_nodes")) {
        std::vector<NodeId> old_nodes;
        for (const auto& n : obj["old_nodes"].as_array()) {
            old_nodes.push_back(config_entry_detail::decode_id<NodeId>(n));
        }
        cfg._old_nodes = std::move(old_nodes);
    }

    // Absent "learners" key (entries written before this feature existed) leaves
    // cfg._learners at its default-constructed empty vector.
    if (obj.contains("learners")) {
        for (const auto& n : obj["learners"].as_array()) {
            cfg._learners.push_back(config_entry_detail::decode_id<NodeId>(n));
        }
    }

    return cfg;
}

// The placement-group assignments a configuration entry carries, or an empty
// map for entries written without them (before this field existed, or by a
// node whose placement group type is not a string).
template<typename NodeId>
requires node_id<NodeId>
auto deserialize_placement(const std::vector<std::byte>& data) -> std::map<NodeId, std::string> {
    std::string s;
    s.reserve(data.size());
    for (std::byte b : data) {
        s.push_back(static_cast<char>(b));
    }

    std::map<NodeId, std::string> placement;
    auto obj = boost::json::parse(s).as_object();
    auto* pairs = obj.if_contains("placement");
    if (pairs == nullptr) {
        return placement;
    }
    for (const auto& p : pairs->as_array()) {
        const auto& pair = p.as_array();
        std::string group{pair.at(1).as_string()};
        placement[config_entry_detail::decode_id<NodeId>(pair.at(0))] = std::move(group);
    }
    return placement;
}

}  // namespace kythira
