// Example: Demonstrating unified types template parameter system
// This example shows how to:
// 1. Use the default_raft_types for simple instantiation
// 2. Create custom types configurations
// 3. Instantiate Raft nodes with clean single-parameter interface

#include <raft/raft.hpp>
#include <raft/types.hpp>
#include <raft/json_serializer.hpp>
#include <raft/simulator_network.hpp>
#include <raft/persistence.hpp>
#include <raft/logger.hpp>
#include <raft/metrics.hpp>
#include <raft/membership.hpp>
#include <iostream>
#include <vector>

namespace {
    constexpr const char* example_name = "Unified Types Example";
    constexpr std::size_t test_node_count = 3;
    constexpr std::chrono::milliseconds test_timeout{5000};
}

// Example 1: Using default types for simple instantiation
auto example_default_types() -> bool {
    std::cout << "\n=== Example 1: Default Types ===\n";
    
    try {
        // Clean single-parameter instantiation using default types
        using default_node = kythira::node<kythira::default_raft_types>;
        
        // All component types are automatically deduced from default_raft_types
        auto node_id = kythira::default_raft_types::node_id_type{1};
        
        std::cout << "✓ Successfully defined node type with default types\n";
        std::cout << "✓ Node ID type: " << typeid(node_id).name() << "\n";
        
        // Verify the node satisfies the raft_node concept
        static_assert(kythira::raft_node<default_node>, 
                     "default_node must satisfy raft_node concept");
        
        std::cout << "✓ Node type satisfies raft_node concept\n";
        std::cout << "✓ Type system validation successful\n";
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "✗ Default types example failed: " << e.what() << "\n";
        return false;
    }
}

// Example 2: Custom types configuration
struct custom_raft_types {
    // Use different basic types
    using node_id_type = std::string;  // String node IDs instead of uint64_t
    using term_id_type = std::uint32_t;  // 32-bit terms instead of 64-bit
    using log_index_type = std::uint32_t;  // 32-bit log indices
    
    // Future types - same as default
    using future_type = kythira::Future<std::vector<std::byte>>;
    using promise_type = kythira::Promise<std::vector<std::byte>>;
    using try_type = kythira::Try<std::vector<std::byte>>;
    
    // Serializer and data types
    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;
    
    // Component types with custom node_id_type
    using network_client_type = kythira::simulator_network_client<future_type, serializer_type, serialized_data_type>;
    using network_server_type = kythira::simulator_network_server<future_type, serializer_type, serialized_data_type>;
    using persistence_engine_type = kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using configuration_type = kythira::raft_configuration;
    
    // Compound types using custom basic types
    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;
    
    // RPC message types with custom types
    using request_vote_request_type = kythira::request_vote_request<node_id_type, term_id_type, log_index_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type>;
    using append_entries_request_type = kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type>;
    using append_entries_response_type = kythira::append_entries_response<term_id_type, log_index_type>;
    using install_snapshot_request_type = kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type>;
    using install_snapshot_response_type = kythira::install_snapshot_response<term_id_type>;
};

// Verify custom types satisfy the raft_types concept
static_assert(kythira::raft_types<custom_raft_types>, 
             "custom_raft_types must satisfy raft_types concept");

auto example_custom_types() -> bool {
    std::cout << "\n=== Example 2: Custom Types ===\n";
    
    try {
        // Clean single-parameter instantiation using custom types
        using custom_node = kythira::node<custom_raft_types>;
        
        // Use string node IDs and 32-bit terms/indices
        auto node_id = custom_raft_types::node_id_type{"node_alpha"};
        
        std::cout << "✓ Successfully defined node type with custom types\n";
        std::cout << "✓ Node ID: " << node_id << "\n";
        
        // Verify the node satisfies the raft_node concept
        static_assert(kythira::raft_node<custom_node>, 
                     "custom_node must satisfy raft_node concept");
        
        // Demonstrate type safety - these types are different from default
        static_assert(std::is_same_v<custom_node::node_id_type, std::string>);
        static_assert(std::is_same_v<custom_node::term_id_type, std::uint32_t>);
        static_assert(std::is_same_v<custom_node::log_index_type, std::uint32_t>);
        
        std::cout << "✓ Type safety verified: string node IDs, 32-bit terms/indices\n";
        std::cout << "✓ Custom types system working correctly\n";
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "✗ Custom types example failed: " << e.what() << "\n";
        return false;
    }
}

// Example 3: Type deduction and concept validation
auto example_type_deduction() -> bool {
    std::cout << "\n=== Example 3: Type Deduction and Concept Validation ===\n";
    
    try {
        // Demonstrate automatic type deduction from unified types parameter
        using node_type = kythira::node<kythira::default_raft_types>;
        
        // All these types are automatically deduced from the unified parameter
        using future_type = node_type::future_type;
        using node_id_type = node_type::node_id_type;
        using term_id_type = node_type::term_id_type;
        using log_index_type = node_type::log_index_type;
        using network_client_type = node_type::network_client_type;
        using network_server_type = node_type::network_server_type;
        
        std::cout << "✓ Type deduction successful:\n";
        std::cout << "  - future_type: " << typeid(future_type).name() << "\n";
        std::cout << "  - node_id_type: " << typeid(node_id_type).name() << "\n";
        std::cout << "  - term_id_type: " << typeid(term_id_type).name() << "\n";
        std::cout << "  - log_index_type: " << typeid(log_index_type).name() << "\n";
        
        // Verify concept compliance
        static_assert(kythira::node_id<node_id_type>);
        static_assert(kythira::term_id<term_id_type>);
        static_assert(kythira::log_index<log_index_type>);
        static_assert(kythira::raft_types<kythira::default_raft_types>);
        static_assert(kythira::raft_node<node_type>);
        
        std::cout << "✓ All concept validations passed\n";
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "✗ Type deduction example failed: " << e.what() << "\n";
        return false;
    }
}

// Example 4: Clean API usage comparison
auto example_api_comparison() -> bool {
    std::cout << "\n=== Example 4: API Usage Comparison ===\n";
    
    std::cout << "Before unified types (complex multi-parameter template):\n";
    std::cout << "  kythira::node<\n";
    std::cout << "    kythira::Future<std::vector<std::byte>>,\n";
    std::cout << "    kythira::simulator_network_client<...>,\n";
    std::cout << "    kythira::simulator_network_server<...>,\n";
    std::cout << "    kythira::memory_persistence_engine,\n";
    std::cout << "    kythira::console_logger,\n";
    std::cout << "    kythira::noop_metrics,\n";
    std::cout << "    kythira::default_membership_manager,\n";
    std::cout << "    std::uint64_t,  // NodeId\n";
    std::cout << "    std::uint64_t,  // TermId\n";
    std::cout << "    std::uint64_t   // LogIndex\n";
    std::cout << "  > node{...};\n\n";
    
    std::cout << "After unified types (clean single-parameter interface):\n";
    std::cout << "  kythira::node<kythira::default_raft_types> node{...};\n";
    std::cout << "  // or\n";
    std::cout << "  kythira::node<custom_raft_types> node{...};\n";
    std::cout << "  // or even\n";
    std::cout << "  kythira::node<> node{...};  // uses default_raft_types\n\n";
    
    std::cout << "✓ API complexity reduced from 10+ template parameters to 1\n";
    std::cout << "✓ Type safety maintained through concept validation\n";
    std::cout << "✓ Flexibility preserved through custom types configurations\n";
    
    return true;
}

// anonymous namespace

auto main() -> int {
    std::cout << "=== " << example_name << " ===\n";
    std::cout << "Demonstrating the unified types template parameter system\n";
    
    int failed_scenarios = 0;
    
    // Run all example scenarios
    if (!example_default_types()) failed_scenarios++;
    if (!example_custom_types()) failed_scenarios++;
    if (!example_type_deduction()) failed_scenarios++;
    if (!example_api_comparison()) failed_scenarios++;
    
    // Report results
    if (failed_scenarios > 0) {
        std::cerr << "\n" << failed_scenarios << " scenario(s) failed\n";
        return 1;  // Non-zero exit code
    }
    
    std::cout << "\n✓ All scenarios passed!\n";
    std::cout << "The unified types template parameter system provides:\n";
    std::cout << "  - Clean single-parameter interface\n";
    std::cout << "  - Automatic type deduction\n";
    std::cout << "  - Concept-based validation\n";
    std::cout << "  - Flexible customization\n";
    std::cout << "  - Maintained type safety\n";
    
    return 0;  // Success
}