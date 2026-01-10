#pragma once

#include <raft/connection.hpp>
#include <raft/types.hpp>
#include <raft/exceptions.hpp>

#include <atomic>
#include <chrono>
#include <memory>

namespace kythira {

// Listener class for Raft network operations
template<typename AddressType, typename PortType, typename FutureType>
class Listener {
public:
    using address_type = AddressType;
    using port_type = PortType;
    using connection_type = Connection<AddressType, PortType, FutureType>;
    using future_connection_type = FutureType;
    
    // Endpoint structure (same as Connection)
    using endpoint = typename connection_type::endpoint;
    
    Listener(endpoint local_endpoint)
        : _local(std::move(local_endpoint))
        , _listening(true)
    {}
    
    // Accept incoming connections
    auto accept() -> future_connection_type {
        return accept(std::chrono::milliseconds{5000});
    }
    
    auto accept(std::chrono::milliseconds timeout) -> future_connection_type {
        if (!_listening) {
            return FutureType(std::make_exception_ptr(
                network_exception("Listener is not listening")));
        }
        
        // For now, create a dummy connection - this would be implemented by concrete transport
        auto remote_endpoint = endpoint{_local.address, static_cast<port_type>(0)};
        auto connection = std::make_shared<connection_type>(_local, remote_endpoint);
        
        return FutureType(connection);
    }
    
    // Close the listener
    auto close() -> void {
        _listening = false;
    }
    
    // Check if listener is active
    auto is_listening() const -> bool {
        return _listening;
    }
    
    // Get local endpoint
    auto local_endpoint() const -> endpoint {
        return _local;
    }
    
private:
    endpoint _local;
    std::atomic<bool> _listening;
};

} // namespace kythira