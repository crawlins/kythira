#include <raft/membership.hpp>
#include <raft/types.hpp>
#include <cstdint>
#include <iostream>
#include <cassert>

namespace {
    constexpr std::uint64_t node_1 = 1;
    constexpr std::uint64_t node_2 = 2;
    constexpr std::uint64_t node_3 = 3;
    constexpr std::uint64_t node_4 = 4;
    constexpr std::uint64_t node_5 = 5;
}

// Verify that default_membership_manager satisfies the membership_manager concept
static_assert(
    raft::membership_manager<
        raft::default_membership_manager<std::uint64_t>,
        std::uint64_t,
        raft::cluster_configuration<std::uint64_t>
    >,
    "default_membership_manager must satisfy membership_manager concept"
);

// Test with string node IDs
static_assert(
    raft::membership_manager<
        raft::default_membership_manager<std::string>,
        std::string,
        raft::cluster_configuration<std::string>
    >,
    "default_membership_manager with string IDs must satisfy membership_manager concept"
);

auto test_basic_validation() -> bool {
    std::cout << "Testing basic validation...\n";
    
    raft::default_membership_manager<std::uint64_t> manager;
    
    // Test validate_new_node - should accept all nodes
    if (!manager.validate_new_node(node_1)) {
        std::cerr << "  ✗ Failed: validate_new_node rejected valid node\n";
        return false;
    }
    
    // Test authenticate_node - should accept all nodes
    if (!manager.authenticate_node(node_1)) {
        std::cerr << "  ✗ Failed: authenticate_node rejected valid node\n";
        return false;
    }
    
    std::cout << "  ✓ Basic validation passed\n";
    return true;
}

auto test_joint_consensus_creation() -> bool {
    std::cout << "Testing joint consensus configuration creation...\n";
    
    raft::default_membership_manager<std::uint64_t> manager;
    
    // Create old configuration with 3 nodes
    raft::cluster_configuration<std::uint64_t> old_config;
    old_config._nodes = {node_1, node_2, node_3};
    old_config._is_joint_consensus = false;
    old_config._old_nodes = std::nullopt;
    
    // Create new configuration with 4 nodes (adding node_4)
    raft::cluster_configuration<std::uint64_t> new_config;
    new_config._nodes = {node_1, node_2, node_3, node_4};
    new_config._is_joint_consensus = false;
    new_config._old_nodes = std::nullopt;
    
    // Create joint configuration
    auto joint_config = manager.create_joint_configuration(old_config, new_config);
    
    // Verify joint configuration properties
    if (!joint_config.is_joint_consensus()) {
        std::cerr << "  ✗ Failed: joint configuration not marked as joint consensus\n";
        return false;
    }
    
    if (!joint_config.old_nodes().has_value()) {
        std::cerr << "  ✗ Failed: joint configuration missing old nodes\n";
        return false;
    }
    
    if (joint_config.old_nodes().value().size() != 3) {
        std::cerr << "  ✗ Failed: joint configuration old nodes has wrong size\n";
        return false;
    }
    
    if (joint_config.nodes().size() != 4) {
        std::cerr << "  ✗ Failed: joint configuration new nodes has wrong size\n";
        return false;
    }
    
    // Verify old nodes are preserved (check they're all present)
    const auto& old_nodes = joint_config.old_nodes().value();
    bool has_node_1 = false, has_node_2 = false, has_node_3 = false;
    for (const auto& n : old_nodes) {
        if (n == node_1) has_node_1 = true;
        if (n == node_2) has_node_2 = true;
        if (n == node_3) has_node_3 = true;
    }
    if (!has_node_1 || !has_node_2 || !has_node_3) {
        std::cerr << "  ✗ Failed: joint configuration old nodes not preserved correctly\n";
        return false;
    }
    
    // Verify new nodes are set (check they're all present)
    const auto& new_nodes = joint_config.nodes();
    has_node_1 = false;
    has_node_2 = false;
    has_node_3 = false;
    bool has_node_4 = false;
    for (const auto& n : new_nodes) {
        if (n == node_1) has_node_1 = true;
        if (n == node_2) has_node_2 = true;
        if (n == node_3) has_node_3 = true;
        if (n == node_4) has_node_4 = true;
    }
    if (!has_node_1 || !has_node_2 || !has_node_3 || !has_node_4) {
        std::cerr << "  ✗ Failed: joint configuration new nodes not set correctly\n";
        return false;
    }
    
    std::cout << "  ✓ Joint consensus creation passed\n";
    return true;
}

auto test_node_in_configuration() -> bool {
    std::cout << "Testing node in configuration check...\n";
    
    raft::default_membership_manager<std::uint64_t> manager;
    
    // Test with simple configuration
    raft::cluster_configuration<std::uint64_t> simple_config;
    simple_config._nodes = {node_1, node_2, node_3};
    simple_config._is_joint_consensus = false;
    
    // Nodes in configuration should return true
    if (!manager.is_node_in_configuration(node_1, simple_config)) {
        std::cerr << "  ✗ Failed: node_1 not found in simple configuration\n";
        return false;
    }
    
    if (!manager.is_node_in_configuration(node_2, simple_config)) {
        std::cerr << "  ✗ Failed: node_2 not found in simple configuration\n";
        return false;
    }
    
    if (!manager.is_node_in_configuration(node_3, simple_config)) {
        std::cerr << "  ✗ Failed: node_3 not found in simple configuration\n";
        return false;
    }
    
    // Node not in configuration should return false
    if (manager.is_node_in_configuration(node_4, simple_config)) {
        std::cerr << "  ✗ Failed: node_4 incorrectly found in simple configuration\n";
        return false;
    }
    
    // Test with joint consensus configuration
    raft::cluster_configuration<std::uint64_t> old_config;
    old_config._nodes = {node_1, node_2, node_3};
    old_config._is_joint_consensus = false;
    
    raft::cluster_configuration<std::uint64_t> new_config;
    new_config._nodes = {node_2, node_3, node_4};
    new_config._is_joint_consensus = false;
    
    auto joint_config = manager.create_joint_configuration(old_config, new_config);
    
    // All nodes from both configurations should be found
    if (!manager.is_node_in_configuration(node_1, joint_config)) {
        std::cerr << "  ✗ Failed: node_1 not found in joint configuration (from old)\n";
        return false;
    }
    
    if (!manager.is_node_in_configuration(node_2, joint_config)) {
        std::cerr << "  ✗ Failed: node_2 not found in joint configuration\n";
        return false;
    }
    
    if (!manager.is_node_in_configuration(node_3, joint_config)) {
        std::cerr << "  ✗ Failed: node_3 not found in joint configuration\n";
        return false;
    }
    
    if (!manager.is_node_in_configuration(node_4, joint_config)) {
        std::cerr << "  ✗ Failed: node_4 not found in joint configuration (from new)\n";
        return false;
    }
    
    // Node not in either configuration should return false
    if (manager.is_node_in_configuration(node_5, joint_config)) {
        std::cerr << "  ✗ Failed: node_5 incorrectly found in joint configuration\n";
        return false;
    }
    
    std::cout << "  ✓ Node in configuration check passed\n";
    return true;
}

auto test_node_removal() -> bool {
    std::cout << "Testing node removal...\n";
    
    raft::default_membership_manager<std::uint64_t> manager;
    
    // Test handle_node_removal - should not throw or crash
    manager.handle_node_removal(node_1);
    
    std::cout << "  ✓ Node removal passed\n";
    return true;
}

auto test_string_node_ids() -> bool {
    std::cout << "Testing with string node IDs...\n";
    
    raft::default_membership_manager<std::string> manager;
    
    // Create configurations with string IDs
    raft::cluster_configuration<std::string> old_config;
    old_config._nodes = {"node_a", "node_b", "node_c"};
    old_config._is_joint_consensus = false;
    
    raft::cluster_configuration<std::string> new_config;
    new_config._nodes = {"node_a", "node_b", "node_c", "node_d"};
    new_config._is_joint_consensus = false;
    
    // Test validation
    if (!manager.validate_new_node("node_d")) {
        std::cerr << "  ✗ Failed: validate_new_node rejected valid string node\n";
        return false;
    }
    
    // Test joint configuration
    auto joint_config = manager.create_joint_configuration(old_config, new_config);
    
    if (!joint_config.is_joint_consensus()) {
        std::cerr << "  ✗ Failed: joint configuration not marked as joint consensus\n";
        return false;
    }
    
    // Test node lookup
    if (!manager.is_node_in_configuration("node_a", joint_config)) {
        std::cerr << "  ✗ Failed: node_a not found in joint configuration\n";
        return false;
    }
    
    if (!manager.is_node_in_configuration("node_d", joint_config)) {
        std::cerr << "  ✗ Failed: node_d not found in joint configuration\n";
        return false;
    }
    
    if (manager.is_node_in_configuration("node_e", joint_config)) {
        std::cerr << "  ✗ Failed: node_e incorrectly found in joint configuration\n";
        return false;
    }
    
    std::cout << "  ✓ String node IDs passed\n";
    return true;
}

int main() {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "  Default Membership Manager Tests\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    int failed_tests = 0;
    
    if (!test_basic_validation()) failed_tests++;
    if (!test_joint_consensus_creation()) failed_tests++;
    if (!test_node_in_configuration()) failed_tests++;
    if (!test_node_removal()) failed_tests++;
    if (!test_string_node_ids()) failed_tests++;
    
    std::cout << "\n" << std::string(60, '=') << "\n";
    if (failed_tests > 0) {
        std::cerr << "  " << failed_tests << " test(s) failed\n";
        std::cout << std::string(60, '=') << "\n";
        return 1;
    }
    
    std::cout << "  All tests passed!\n";
    std::cout << std::string(60, '=') << "\n";
    return 0;
}
