#pragma once

#include <raft/types.hpp>
#include <raft/exceptions.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace kythira {

// Forward declarations
template<typename AddressType, typename PortType, typename FutureType>
class Listener;

// Connection class for Raft network operations
template<typename AddressType, typename PortType, typename FutureType>
class Connection {
public:
    using address_type = AddressType;
    using port_type = PortType;
    using future_bool_type = FutureType;
    using future_bytes_type = FutureType;
    
    // Endpoint structure
    struct endpoint {
        address_type address;
        port_type port;
        
        endpoint(address_type addr, port_type prt)
            : address(std::move(addr)), port(std::move(prt)) {}
        
        auto operator==(const endpoint&) const -> bool = default;
    };
    
    Connection(endpoint local, endpoint remote)
        : _local(std::move(local))
        , _remote(std::move(remote))
        , _open(true)
    {}
    
    // Read data from connection
    auto read() -> future_bytes_type {
        return read(std::chrono::milliseconds{5000});
    }
    
    auto read(std::chrono::milliseconds timeout) -> future_bytes_type {
        if (!_open) {
            return FutureType(std::make_exception_ptr(
                network_exception("Connection is closed")));
        }
        
        // For now, return empty data - this would be implemented by concrete transport
        std::vector<std::byte> empty_data;
        return FutureType(std::move(empty_data));
    }
    
    // Write data to connection
    auto write(std::vector<std::byte> data) -> future_bool_type {
        return write(std::move(data), std::chrono::milliseconds{5000});
    }
    
    auto write(std::vector<std::byte> data, std::chrono::milliseconds timeout) -> future_bool_type {
        if (!_open) {
            return FutureType(std::make_exception_ptr(
                network_exception("Connection is closed")));
        }
        
        // For now, always succeed - this would be implemented by concrete transport
        return FutureType(true);
    }
    
    // Close the connection
    auto close() -> void {
        _open = false;
    }
    
    // Check if connection is open
    auto is_open() const -> bool {
        return _open;
    }
    
    // Get local endpoint
    auto local_endpoint() const -> endpoint {
        return _local;
    }
    
    // Get remote endpoint
    auto remote_endpoint() const -> endpoint {
        return _remote;
    }
    
private:
    endpoint _local;
    endpoint _remote;
    std::atomic<bool> _open;
};

} // namespace kythira