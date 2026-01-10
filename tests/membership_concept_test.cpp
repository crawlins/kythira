#include <raft/membership.hpp>
#include <raft/types.hpp>
#include <cstdint>

// Test implementation that satisfies the membership_manager concept
template<typename NodeId = std::uint64_t>
class test_membership_manager {
public:
    auto validate_new_node(const NodeId& node) -> bool {
        // Basic validation - always accept for testing
        return true;
    }
    
    auto authenticate_node(const NodeId& node) -> bool {
        // Basic authentication - always accept for testing
        return true;
    }
    
    auto create_joint_configuration(
        const kythira::cluster_configuration<NodeId>& old_config,
        const kythira::cluster_configuration<NodeId>& new_config
    ) -> kythira::cluster_configuration<NodeId> {
        // Create joint consensus configuration
        kythira::cluster_configuration<NodeId> joint_config;
        joint_config._nodes = new_config._nodes;
        joint_config._is_joint_consensus = true;
        joint_config._old_nodes = old_config._nodes;
        return joint_config;
    }
    
    auto is_node_in_configuration(
        const NodeId& node,
        const kythira::cluster_configuration<NodeId>& config
    ) -> bool {
        // Check if node is in the configuration
        for (const auto& n : config.nodes()) {
            if (n == node) {
                return true;
            }
        }
        
        // Also check old configuration if in joint consensus
        const auto& old_nodes_opt = config.old_nodes();
        if (config.is_joint_consensus() && old_nodes_opt.has_value()) {
            for (const auto& n : old_nodes_opt.value()) {
                if (n == node) {
                    return true;
                }
            }
        }
        
        return false;
    }
    
    auto handle_node_removal(const NodeId& node) -> void {
        // Cleanup for removed node
        // In a real implementation, this might close connections, free resources, etc.
    }
};

// Verify that test_membership_manager satisfies the membership_manager concept
static_assert(
    kythira::membership_manager<
        test_membership_manager<std::uint64_t>,
        std::uint64_t,
        kythira::cluster_configuration<std::uint64_t>
    >,
    "test_membership_manager must satisfy membership_manager concept"
);

// Test with string node IDs
static_assert(
    kythira::membership_manager<
        test_membership_manager<std::string>,
        std::string,
        kythira::cluster_configuration<std::string>
    >,
    "test_membership_manager with string IDs must satisfy membership_manager concept"
);

// Test that a non-conforming type does not satisfy the concept
class non_membership_manager {
public:
    auto validate_new_node(std::uint64_t node) -> bool { return true; }
    // Missing other required methods
};

static_assert(
    !kythira::membership_manager<
        non_membership_manager,
        std::uint64_t,
        kythira::cluster_configuration<std::uint64_t>
    >,
    "non_membership_manager must not satisfy membership_manager concept"
);

int main() {
    // Instantiate to ensure it compiles
    test_membership_manager<std::uint64_t> manager;
    
    kythira::cluster_configuration<std::uint64_t> old_config;
    old_config._nodes = {1, 2, 3};
    old_config._is_joint_consensus = false;
    
    kythira::cluster_configuration<std::uint64_t> new_config;
    new_config._nodes = {1, 2, 3, 4};
    new_config._is_joint_consensus = false;
    
    // Test validate_new_node
    bool valid = manager.validate_new_node(4);
    
    // Test authenticate_node
    bool authenticated = manager.authenticate_node(4);
    
    // Test create_joint_configuration
    auto joint_config = manager.create_joint_configuration(old_config, new_config);
    
    // Test is_node_in_configuration
    bool in_config = manager.is_node_in_configuration(4, new_config);
    bool not_in_config = !manager.is_node_in_configuration(5, new_config);
    
    // Test handle_node_removal
    manager.handle_node_removal(4);
    
    // Verify results
    if (!valid || !authenticated || !in_config || !not_in_config) {
        return 1;
    }
    
    // Verify joint configuration properties
    if (!joint_config.is_joint_consensus()) {
        return 1;
    }
    
    if (!joint_config.old_nodes().has_value()) {
        return 1;
    }
    
    if (joint_config.old_nodes().value().size() != 3) {
        return 1;
    }
    
    return 0;
}
