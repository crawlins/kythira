#pragma once

#include "concepts.hpp"
#include "types.hpp"
#include "exceptions.hpp"

#include <memory>
#include <mutex>
#include <unordered_set>

namespace network_simulator {

// Forward declaration
template<typename Types>
class NetworkSimulator;

// NetworkNode class template
template<typename Types>
class NetworkNode {
public:
    // Type aliases from Types template argument
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    using message_type = typename Types::message_type;
    using connection_type = typename Types::connection_type;
    using listener_type = typename Types::listener_type;
    using simulator_type = NetworkSimulator<Types>;
    
    // Future type aliases
    using future_bool_type = typename Types::future_bool_type;
    using future_message_type = typename Types::future_message_type;
    using future_connection_type = typename Types::future_connection_type;
    using future_listener_type = typename Types::future_listener_type;
    
    explicit NetworkNode(address_type addr, simulator_type* simulator)
        : _address(std::move(addr))
        , _simulator(simulator)
    {}
    
    // Node identity
    auto address() const -> address_type { return _address; }
    
    // Connectionless operations
    auto send(message_type msg) -> future_bool_type;
    auto send(message_type msg, std::chrono::milliseconds timeout) -> future_bool_type;
    auto receive() -> future_message_type;
    auto receive(std::chrono::milliseconds timeout) -> future_message_type;
    
    // Connection-oriented client operations
    auto connect(address_type dst_addr, port_type dst_port) -> future_connection_type;
    auto connect(address_type dst_addr, port_type dst_port, port_type src_port) -> future_connection_type;
    auto connect(address_type dst_addr, port_type dst_port, std::chrono::milliseconds timeout) -> future_connection_type;
    
    // Connection-oriented server operations
    auto bind() -> future_listener_type;  // bind to random port
    auto bind(port_type port) -> future_listener_type;  // bind to specific port
    auto bind(port_type port, std::chrono::milliseconds timeout) -> future_listener_type;
    
private:
    address_type _address;
    simulator_type* _simulator;
    
    // Ephemeral port allocation
    auto allocate_ephemeral_port() -> port_type;
    auto release_port(port_type port) -> void;
    std::unordered_set<port_type> _used_ports;
    mutable std::mutex _port_mutex;
};

} // namespace network_simulator

#include "node_impl.hpp"
