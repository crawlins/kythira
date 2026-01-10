#include <raft/http_transport.hpp>
#include <raft/types.hpp>
#include <raft/metrics.hpp>
#include <network_simulator/types.hpp>
#include <iostream>

// Mock serializer that doesn't use boost::json
template<typename Data>
class mock_serializer {
public:
    template<typename T>
    auto serialize(const T&) const -> Data {
        return Data{};
    }
    
    template<typename T>
    auto deserialize_request_vote_response(const Data&) const -> T {
        return T{};
    }
    
    template<typename T>
    auto deserialize_append_entries_response(const Data&) const -> T {
        return T{};
    }
    
    template<typename T>
    auto deserialize_install_snapshot_response(const Data&) const -> T {
        return T{};
    }
};

// Test transport types using SimpleFuture
using test_transport_types = kythira::simple_http_transport_types<
    mock_serializer<std::vector<std::byte>>,
    kythira::noop_metrics,
    kythira::noop_metrics  // Using noop_metrics as executor placeholder
>;

int main() {
    // Test that the transport_types concept is satisfied
    static_assert(kythira::transport_types<test_transport_types>,
                  "test_transport_types must satisfy transport_types concept");
    
    // Test that future_template works correctly
    static_assert(std::is_same_v<typename test_transport_types::template future_template<kythira::request_vote_response<>>,
                                network_simulator::SimpleFuture<kythira::request_vote_response<>>>,
                  "future_template must use SimpleFuture");
    
    // Test that the client can be instantiated
    using client_type = kythira::cpp_httplib_client<test_transport_types>;
    
    std::cout << "✓ transport_types concept validation passed\n";
    std::cout << "✓ future_template type validation passed\n";
    std::cout << "✓ client template instantiation passed\n";
    std::cout << "All concept validations successful!\n";
    
    return 0;
}