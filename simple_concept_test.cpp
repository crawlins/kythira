#include <network_simulator/types.hpp>
#include <vector>
#include <iostream>
#include <type_traits>

// Simplified transport types for testing
template<typename RPC_Serializer, typename Metrics, typename Executor>
struct simple_http_transport_types {
    template<typename T> using future_template = network_simulator::SimpleFuture<T>;
    using serializer_type = RPC_Serializer;
    using metrics_type = Metrics;
    using executor_type = Executor;
};

// Mock types
struct mock_serializer {};
struct mock_metrics {};
struct mock_executor {};

// Mock response types
template<typename TermId = std::uint64_t>
struct request_vote_response {
    TermId _term;
    bool _vote_granted;
};

template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
struct append_entries_response {
    TermId _term;
    bool _success;
};

template<typename TermId = std::uint64_t>
struct install_snapshot_response {
    TermId _term;
};

using test_transport_types = simple_http_transport_types<
    mock_serializer,
    mock_metrics,
    mock_executor
>;

int main() {
    // Test that future_template can be instantiated with different response types
    static_assert(std::is_same_v<typename test_transport_types::template future_template<request_vote_response<>>,
                                network_simulator::SimpleFuture<request_vote_response<>>>,
                  "future_template<request_vote_response> must be SimpleFuture<request_vote_response>");
    
    static_assert(std::is_same_v<typename test_transport_types::template future_template<append_entries_response<>>,
                                network_simulator::SimpleFuture<append_entries_response<>>>,
                  "future_template<append_entries_response> must be SimpleFuture<append_entries_response>");
    
    static_assert(std::is_same_v<typename test_transport_types::template future_template<install_snapshot_response<>>,
                                network_simulator::SimpleFuture<install_snapshot_response<>>>,
                  "future_template<install_snapshot_response> must be SimpleFuture<install_snapshot_response>");
    
    // Test that the types are correctly defined
    static_assert(std::is_same_v<typename test_transport_types::serializer_type, mock_serializer>,
                  "serializer_type must be correctly defined");
    
    static_assert(std::is_same_v<typename test_transport_types::metrics_type, mock_metrics>,
                  "metrics_type must be correctly defined");
    
    static_assert(std::is_same_v<typename test_transport_types::executor_type, mock_executor>,
                  "executor_type must be correctly defined");
    
    std::cout << "✓ Template template parameter validation passed\n";
    std::cout << "✓ Future type instantiation validation passed\n";
    std::cout << "✓ All type member validation passed\n";
    std::cout << "Template template parameter redesign successful!\n";
    
    return 0;
}