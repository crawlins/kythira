#pragma once

#include "concepts.hpp"
#include "types.hpp"
#include "exceptions.hpp"

#include <memory>
#include <mutex>
#include <unordered_set>

#include <raft/future.hpp>

// Forward declaration
namespace kythira {
template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>
class NetworkSimulator;
}

namespace network_simulator {

// NetworkNode class (stub for now, will be fully implemented in task 5)
template<address Addr, port Port, typename FutureType>
class NetworkNode {
public:
    explicit NetworkNode(Addr addr, kythira::NetworkSimulator<Addr, Port, FutureType>* simulator)
        : _address(std::move(addr))
        , _simulator(simulator)
        , _next_ephemeral_port(Port{})
    {
        // Initialize ephemeral port based on port type
        if constexpr (std::is_same_v<Port, unsigned short>) {
            _next_ephemeral_port = 49152; // Start of ephemeral port range
        } else if constexpr (std::is_same_v<Port, std::string>) {
            _next_ephemeral_port = "ephemeral_0";
        }
    }
    
    // Node identity
    auto address() const -> Addr { return _address; }
    
    // Connectionless operations
    auto send(Message<Addr, Port> msg) -> FutureType {
        // Delegate to simulator's route_message
        return _simulator->route_message(std::move(msg));
    }
    
    auto send(Message<Addr, Port> msg, std::chrono::milliseconds timeout) -> FutureType {
        // Route message with timeout - implementation depends on FutureType
        return _simulator->route_message(std::move(msg));
    }
    
    auto receive() -> FutureType {
        // Retrieve message from simulator
        return _simulator->retrieve_message(_address);
    }
    
    auto receive(std::chrono::milliseconds timeout) -> FutureType {
        // Retrieve message with timeout
        return _simulator->retrieve_message(_address, timeout);
    }
    
    // Connection-oriented client operations
    auto connect(Addr dst_addr, Port dst_port) -> FutureType {
        // Use ephemeral port allocation
        auto src_port = allocate_ephemeral_port();
        return connect(std::move(dst_addr), std::move(dst_port), std::move(src_port));
    }
    
    auto connect(Addr dst_addr, Port dst_port, Port src_port) -> FutureType {
        // Delegate to simulator's connect implementation
        return _simulator->establish_connection(_address, std::move(src_port), std::move(dst_addr), std::move(dst_port));
    }
    
    auto connect(Addr dst_addr, Port dst_port, std::chrono::milliseconds timeout) -> FutureType {
        // Use ephemeral port allocation with timeout
        auto src_port = allocate_ephemeral_port();
        return _simulator->establish_connection(_address, std::move(src_port), std::move(dst_addr), std::move(dst_port));
    }
    
    // Connection-oriented server operations
    auto bind() -> FutureType {
        // Bind to random port
        return _simulator->create_listener(_address);
    }
    
    auto bind(Port port) -> FutureType {
        // Bind to specific port
        return _simulator->create_listener(_address, std::move(port));
    }
    
    auto bind(Port port, std::chrono::milliseconds timeout) -> FutureType {
        // Bind to specific port with timeout
        return _simulator->create_listener(_address, std::move(port), timeout);
    }
    
private:
    Addr _address;
    kythira::NetworkSimulator<Addr, Port, FutureType>* _simulator;
    
    // Ephemeral port allocation
    auto allocate_ephemeral_port() -> Port;
    std::unordered_set<Port> _used_ports;
    mutable std::mutex _port_mutex;
    Port _next_ephemeral_port;
};

// Implementation of allocate_ephemeral_port
template<address Addr, port Port, typename FutureType>

auto NetworkNode<Addr, Port, FutureType>::allocate_ephemeral_port() -> Port {
    std::lock_guard lock(_port_mutex);
    
    // Find an unused port
    Port allocated_port;
    
    if constexpr (std::is_same_v<Port, unsigned short>) {
        // For unsigned short, increment until we find an unused port
        allocated_port = _next_ephemeral_port;
        while (_used_ports.find(allocated_port) != _used_ports.end()) {
            allocated_port++;
            if (allocated_port == 0) {
                // Wrapped around, start from ephemeral range again
                allocated_port = 49152;
            }
        }
        _next_ephemeral_port = allocated_port + 1;
    } else if constexpr (std::is_same_v<Port, std::string>) {
        // For string, generate unique names
        std::size_t counter = 0;
        do {
            allocated_port = "ephemeral_" + std::to_string(counter++);
        } while (_used_ports.find(allocated_port) != _used_ports.end());
    }
    
    // Mark port as used
    _used_ports.insert(allocated_port);
    
    return allocated_port;
}

} // namespace network_simulator
