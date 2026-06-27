#pragma once

/// @file membership.hpp
/// @brief Membership-manager concept and default implementation.

#include "types.hpp"
#include <concepts>

namespace kythira {

/// @brief Concept for a cluster-membership manager.
///
/// Implementations control which nodes may join, authenticate joining nodes, and
/// build the joint-consensus configuration used during safe membership transitions.
///
/// @tparam M      Concrete membership manager type.
/// @tparam NodeId Must satisfy `node_id`.
/// @tparam Config Must satisfy `cluster_configuration_type<NodeId>`.
template<typename M, typename NodeId, typename Config>
concept membership_manager = requires(M manager, const NodeId& node, const Config& config) {
    requires node_id<NodeId>;
    requires cluster_configuration_type<Config, NodeId>;

    /// Returns `true` if the node is permitted to join the cluster.
    { manager.validate_new_node(node) } -> std::convertible_to<bool>;

    /// Returns `true` if the node's credentials are accepted.
    { manager.authenticate_node(node) } -> std::convertible_to<bool>;

    /// Builds the joint-consensus configuration spanning both `old_config` and `new_config`.
    { manager.create_joint_configuration(config, config) } -> std::same_as<Config>;

    /// Returns `true` if `node` appears in the primary or old node set of `config`.
    { manager.is_node_in_configuration(node, config) } -> std::convertible_to<bool>;

    /// Called when a membership change is committed so the implementation can react
    /// (update service discovery, close stale connections, etc.).
    { manager.handle_cluster_membership_change(config, config) } -> std::same_as<void>;
};

/// @brief Default membership manager: accepts all nodes without authentication.
///
/// Suitable for development and testing.  Production deployments should replace
/// `validate_new_node` and `authenticate_node` with credential checks.
///
/// @tparam NodeId Node identifier type; defaults to `uint64_t`.
template<typename NodeId = std::uint64_t>
requires node_id<NodeId>
class default_membership_manager {
public:
    /// @brief Returns `true`; all nodes are permitted to join.
    [[nodiscard]] auto validate_new_node(const NodeId& node) -> bool { return true; }

    /// @brief Returns `true`; all nodes are authenticated unconditionally.
    [[nodiscard]] auto authenticate_node(const NodeId& node) -> bool { return true; }

    /// @brief Creates a joint-consensus configuration from `old_config` and `new_config`.
    ///
    /// The joint configuration marks `_is_joint_consensus = true` and stores `old_config`
    /// nodes in `_old_nodes` so that a majority of both old and new configurations is
    /// required until the transition completes.
    auto create_joint_configuration(const cluster_configuration<NodeId>& old_config,
                                    const cluster_configuration<NodeId>& new_config)
        -> cluster_configuration<NodeId> {
        cluster_configuration<NodeId> joint_config{};
        joint_config._nodes = new_config._nodes;
        joint_config._is_joint_consensus = true;
        joint_config._old_nodes = old_config._nodes;
        return joint_config;
    }

    /// @brief Returns `true` if `node` is present in `config`'s primary or old node set.
    auto is_node_in_configuration(const NodeId& node, const cluster_configuration<NodeId>& config)
        -> bool {
        for (const auto& n : config.nodes()) {
            if (n == node) {
                return true;
            }
        }
        if (config.is_joint_consensus()) {
            const auto& old_nodes_opt = config.old_nodes();
            if (old_nodes_opt.has_value()) {
                for (const auto& n : old_nodes_opt.value()) {
                    if (n == node) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    /// @brief No-op notification hook; override in derived classes to react to membership changes.
    /// @param old_config The configuration before the change.
    /// @param new_config The committed configuration after the change.
    auto handle_cluster_membership_change(const cluster_configuration<NodeId>& old_config,
                                          const cluster_configuration<NodeId>& new_config) -> void {
    }
};

}  // namespace kythira
