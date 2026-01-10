#pragma once

#include "types.hpp"
#include <concepts>

namespace kythira {

// Membership manager concept
// Defines the interface for managing cluster membership changes
template<typename M, typename NodeId, typename Config>
concept membership_manager = requires(
    M manager,
    const NodeId& node,
    const Config& config
) {
    requires node_id<NodeId>;
    requires cluster_configuration_type<Config, NodeId>;
    
    // Node validation - returns true if the node is valid for joining
    { manager.validate_new_node(node) } -> std::convertible_to<bool>;
    
    // Node authentication - returns true if the node is authenticated
    { manager.authenticate_node(node) } -> std::convertible_to<bool>;
    
    // Configuration management - creates a joint consensus configuration
    { manager.create_joint_configuration(config, config) } -> std::same_as<Config>;
    
    // Check if a node is in the configuration
    { manager.is_node_in_configuration(node, config) } -> std::convertible_to<bool>;
    
    // Cleanup when a node is removed
    { manager.handle_node_removal(node) } -> std::same_as<void>;
};

// Default membership manager implementation
// Provides basic validation and joint consensus configuration creation
template<typename NodeId = std::uint64_t>
requires node_id<NodeId>
class default_membership_manager {
public:
    // Validate a new node for joining the cluster
    // Basic implementation accepts all nodes
    auto validate_new_node(const NodeId& node) -> bool {
        // In a production implementation, this could check:
        // - Node ID format and validity
        // - Whether the node is already in the cluster
        // - Resource constraints (max cluster size)
        // - Network reachability
        return true;
    }
    
    // Authenticate a node
    // Basic implementation accepts all nodes
    auto authenticate_node(const NodeId& node) -> bool {
        // In a production implementation, this could check:
        // - Cryptographic credentials
        // - Access control lists
        // - Certificate validation
        // - Token-based authentication
        return true;
    }
    
    // Create a joint consensus configuration from old and new configurations
    // This is used during cluster membership changes to ensure safety
    auto create_joint_configuration(
        const cluster_configuration<NodeId>& old_config,
        const cluster_configuration<NodeId>& new_config
    ) -> cluster_configuration<NodeId> {
        // Joint consensus requires majorities from both old and new configurations
        cluster_configuration<NodeId> joint_config{};
        
        // The new configuration becomes the primary configuration
        joint_config._nodes = new_config._nodes;
        
        // Mark as joint consensus
        joint_config._is_joint_consensus = true;
        
        // Store the old configuration for majority calculations
        joint_config._old_nodes = old_config._nodes;
        
        return joint_config;
    }
    
    // Check if a node is in the configuration
    // During joint consensus, checks both old and new configurations
    auto is_node_in_configuration(
        const NodeId& node,
        const cluster_configuration<NodeId>& config
    ) -> bool {
        // Check the primary (new) configuration
        for (const auto& n : config.nodes()) {
            if (n == node) {
                return true;
            }
        }
        
        // If in joint consensus, also check the old configuration
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
    
    // Handle cleanup when a node is removed from the cluster
    auto handle_node_removal(const NodeId& node) -> void {
        // In a production implementation, this could:
        // - Close network connections to the removed node
        // - Free resources associated with the node
        // - Update routing tables
        // - Notify monitoring systems
        // - Clean up cached state
        
        // Basic implementation does nothing
    }
};

} // namespace kythira
