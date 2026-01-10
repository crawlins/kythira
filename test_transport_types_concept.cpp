#include <raft/types.hpp>
#include <raft/http_transport.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <network_simulator/types.hpp>

// Test that the current transport_types concept works with template template parameters
int main() {
    // Test transport types using the new template template parameter approach
    using test_transport_types = kythira::simple_http_transport_types<
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics,
        kythira::noop_metrics  // Using noop_metrics as executor placeholder
    >;
    
    // Test that it satisfies the transport_types concept
    static_assert(kythira::transport_types<test_transport_types>,
                  "simple_http_transport_types must satisfy transport_types concept");
    
    // Test that future_template can be instantiated with different response types
    static_assert(std::is_same_v<typename test_transport_types::template future_template<kythira::request_vote_response<>>,
                                network_simulator::SimpleFuture<kythira::request_vote_response<>>>,
                  "future_template<request_vote_response> must be SimpleFuture<request_vote_response>");
    
    static_assert(std::is_same_v<typename test_transport_types::template future_template<kythira::append_entries_response<>>,
                                network_simulator::SimpleFuture<kythira::append_entries_response<>>>,
                  "future_template<append_entries_response> must be SimpleFuture<append_entries_response>");
    
    static_assert(std::is_same_v<typename test_transport_types::template future_template<kythira::install_snapshot_response<>>,
                                network_simulator::SimpleFuture<kythira::install_snapshot_response<>>>,
                  "future_template<install_snapshot_response> must be SimpleFuture<install_snapshot_response>");
    
    // Test that the serializer_type satisfies rpc_serializer concept
    static_assert(kythira::rpc_serializer<typename test_transport_types::serializer_type, std::vector<std::byte>>,
                  "serializer_type must satisfy rpc_serializer concept");
    
    // Test that the metrics_type satisfies metrics concept
    static_assert(kythira::metrics<typename test_transport_types::metrics_type>,
                  "metrics_type must satisfy metrics concept");
    
    // Test that the future_template satisfies future concept for all required response types
    static_assert(kythira::future<typename test_transport_types::template future_template<kythira::request_vote_response<>>, kythira::request_vote_response<>>,
                  "future_template must satisfy future concept for request_vote_response");
    
    static_assert(kythira::future<typename test_transport_types::template future_template<kythira::append_entries_response<>>, kythira::append_entries_response<>>,
                  "future_template must satisfy future concept for append_entries_response");
    
    static_assert(kythira::future<typename test_transport_types::template future_template<kythira::install_snapshot_response<>>, kythira::install_snapshot_response<>>,
                  "future_template must satisfy future concept for install_snapshot_response");
    
    return 0;
}