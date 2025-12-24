#pragma once

#include "concepts.hpp"
#include "types.hpp"
#include "exceptions.hpp"
#include "node.hpp"
#include "connection.hpp"
#include "listener.hpp"

#include <algorithm>
#include <memory>
#include <queue>
#include <random>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include <folly/executors/CPUThreadPoolExecutor.h>

namespace kythira {

// NetworkSimulator class
template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>
class NetworkSimulator {
public:
    NetworkSimulator()
        : _executor(nullptr)  // TODO: Replace with generic executor
        , _rng(std::random_device{}())
        , _started(false)
    {}
    
    ~NetworkSimulator() {
        stop();
    }
    
    // Topology configuration
    auto add_node(Addr address) -> void;
    auto remove_node(Addr address) -> void;
    auto add_edge(Addr from, Addr to, network_simulator::NetworkEdge edge) -> void;
    auto remove_edge(Addr from, Addr to) -> void;
    
    // Node creation
    auto create_node(Addr address) -> std::shared_ptr<network_simulator::NetworkNode<Addr, Port, FutureType>>;
    
    // Simulation control
    auto start() -> void;
    auto stop() -> void;
    auto reset() -> void;
    
    // Internal methods (used by NetworkNode, Connection, Listener)
    auto route_message(network_simulator::Message<Addr, Port> msg) -> FutureType;
    auto deliver_message(network_simulator::Message<Addr, Port> msg) -> void;
    auto deliver_connection_data(network_simulator::Message<Addr, Port> msg) -> void;
    auto apply_latency(Addr from, Addr to) -> std::chrono::milliseconds;
    auto check_reliability(Addr from, Addr to) -> bool;
    auto retrieve_message(Addr address) -> FutureType;
    auto retrieve_message(Addr address, std::chrono::milliseconds timeout) -> FutureType;
    
    // Connection establishment
    auto establish_connection(Addr src_addr, Port src_port, Addr dst_addr, Port dst_port) -> kythira::Future<std::shared_ptr<Connection<Addr, Port, FutureType>>>;
    
    // Listener establishment
    auto create_listener(Addr addr, Port port) -> kythira::Future<std::shared_ptr<Listener<Addr, Port, FutureType>>>;
    auto create_listener(Addr addr) -> kythira::Future<std::shared_ptr<Listener<Addr, Port, FutureType>>>;  // Random port
    auto create_listener(Addr addr, Port port, std::chrono::milliseconds timeout) -> kythira::Future<std::shared_ptr<Listener<Addr, Port, FutureType>>>;
    
    // Query methods for testing
    auto has_node(Addr address) const -> bool;
    auto has_edge(Addr from, Addr to) const -> bool;
    auto get_edge(Addr from, Addr to) const -> network_simulator::NetworkEdge;
    
private:
    // Directed graph: adjacency list representation
    std::unordered_map<Addr, std::unordered_map<Addr, network_simulator::NetworkEdge>> _topology;
    
    // Active nodes
    std::unordered_map<Addr, std::shared_ptr<network_simulator::NetworkNode<Addr, Port, FutureType>>> _nodes;
    
    // Message queues per node
    std::unordered_map<Addr, std::queue<network_simulator::Message<Addr, Port>>> _message_queues;
    
    // Connection state
    std::unordered_map<network_simulator::Endpoint<Addr, Port>, std::shared_ptr<Connection<Addr, Port, FutureType>>> _connections;
    
    // Listeners
    std::unordered_map<network_simulator::Endpoint<Addr, Port>, std::shared_ptr<Listener<Addr, Port, FutureType>>> _listeners;
    
    // Thread pool executor for async operations (TODO: Replace with generic executor)
    std::unique_ptr<folly::CPUThreadPoolExecutor> _executor;
    
    // Random number generator for reliability simulation
    std::mt19937 _rng;
    
    // Simulation state
    std::atomic<bool> _started;
    
    // Mutex for thread safety
    mutable std::shared_mutex _mutex;
};

// Implementation

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::add_node(Addr address) -> void {
    std::unique_lock lock(_mutex);
    
    // Add node to topology if not already present
    if (_topology.find(address) == _topology.end()) {
        _topology[address] = std::unordered_map<Addr, network_simulator::NetworkEdge>();
    }
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::remove_node(Addr address) -> void {
    std::unique_lock lock(_mutex);
    
    // Remove node from topology
    _topology.erase(address);
    
    // Remove all edges to this node
    for (auto& [from_addr, edges] : _topology) {
        edges.erase(address);
    }
    
    // Remove node instance
    _nodes.erase(address);
    
    // Clear message queue
    _message_queues.erase(address);
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::add_edge(Addr from, Addr to, network_simulator::NetworkEdge edge) -> void {
    std::unique_lock lock(_mutex);
    
    // Ensure both nodes exist in topology
    if (_topology.find(from) == _topology.end()) {
        _topology[from] = std::unordered_map<Addr, network_simulator::NetworkEdge>();
    }
    if (_topology.find(to) == _topology.end()) {
        _topology[to] = std::unordered_map<Addr, network_simulator::NetworkEdge>();
    }
    
    // Add edge
    _topology[from][to] = edge;
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::remove_edge(Addr from, Addr to) -> void {
    std::unique_lock lock(_mutex);
    
    auto from_it = _topology.find(from);
    if (from_it != _topology.end()) {
        from_it->second.erase(to);
    }
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::create_node(Addr address) -> std::shared_ptr<network_simulator::NetworkNode<Addr, Port, FutureType>> {
    std::unique_lock lock(_mutex);
    
    // Check if node already exists
    auto it = _nodes.find(address);
    if (it != _nodes.end()) {
        return it->second;
    }
    
    // Ensure node exists in topology
    if (_topology.find(address) == _topology.end()) {
        _topology[address] = std::unordered_map<Addr, network_simulator::NetworkEdge>();
    }
    
    // Create new node
    auto node = std::make_shared<network_simulator::NetworkNode<Addr, Port, FutureType>>(address, this);
    _nodes[address] = node;
    
    return node;
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::start() -> void {
    std::unique_lock lock(_mutex);
    _started = true;
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::stop() -> void {
    std::unique_lock lock(_mutex);
    _started = false;
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::reset() -> void {
    std::unique_lock lock(_mutex);
    
    // Clear all state
    _topology.clear();
    _nodes.clear();
    _message_queues.clear();
    _connections.clear();
    _listeners.clear();
    _started = false;
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::has_node(Addr address) const -> bool {
    std::shared_lock lock(_mutex);
    return _topology.find(address) != _topology.end();
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::has_edge(Addr from, Addr to) const -> bool {
    std::shared_lock lock(_mutex);
    
    auto from_it = _topology.find(from);
    if (from_it == _topology.end()) {
        return false;
    }
    
    return from_it->second.find(to) != from_it->second.end();
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::get_edge(Addr from, Addr to) const -> network_simulator::NetworkEdge {
    std::shared_lock lock(_mutex);
    
    auto from_it = _topology.find(from);
    if (from_it == _topology.end()) {
        throw network_simulator::NoRouteException("Node not found", "Node not found");
    }
    
    auto to_it = from_it->second.find(to);
    if (to_it == from_it->second.end()) {
        throw network_simulator::NoRouteException("Edge not found", "Edge not found");
    }
    
    return to_it->second;
}

// Implementation of routing and delivery methods

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::apply_latency(Addr from, Addr to) -> std::chrono::milliseconds {
    std::shared_lock lock(_mutex);
    
    // Check if edge exists
    auto from_it = _topology.find(from);
    if (from_it == _topology.end()) {
        return std::chrono::milliseconds{0};
    }
    
    auto to_it = from_it->second.find(to);
    if (to_it == from_it->second.end()) {
        return std::chrono::milliseconds{0};
    }
    
    // Return the latency from the edge
    return to_it->second.latency();
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::check_reliability(Addr from, Addr to) -> bool {
    double reliability;
    
    {
        std::shared_lock lock(_mutex);
        
        // Check if edge exists
        auto from_it = _topology.find(from);
        if (from_it == _topology.end()) {
            return false;
        }
        
        auto to_it = from_it->second.find(to);
        if (to_it == from_it->second.end()) {
            return false;
        }
        
        // Get reliability from edge
        reliability = to_it->second.reliability();
    }
    
    // Use Bernoulli distribution to determine if message passes
    // Need unique lock for RNG access
    std::unique_lock rng_lock(_mutex);
    std::bernoulli_distribution dist(reliability);
    return dist(_rng);
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::route_message(network_simulator::Message<Addr, Port> msg) -> FutureType {
    // Check if simulator is started
    if (!_started) {
        return FutureType(false);
    }
    
    Addr src = msg.source_address();
    Addr dst = msg.destination_address();
    
    // Check if direct edge exists from source to destination
    bool has_direct_edge = false;
    {
        std::shared_lock lock(_mutex);
        auto from_it = _topology.find(src);
        if (from_it != _topology.end()) {
            has_direct_edge = from_it->second.find(dst) != from_it->second.end();
        }
    }
    
    if (!has_direct_edge) {
        // No route exists
        return FutureType(false);
    }
    
    // Check reliability - message may be dropped
    if (!check_reliability(src, dst)) {
        // Message dropped due to reliability
        return FutureType(false);
    }
    
    // Get latency for this edge
    auto latency = apply_latency(src, dst);
    
    // Schedule message delivery after latency
    // TODO: Implement generic future delay mechanism
    // For now, deliver immediately as a temporary fix
    deliver_message(std::move(msg));
    return FutureType(true);
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::deliver_message(network_simulator::Message<Addr, Port> msg) -> void {
    // Check if this is connection-oriented data (has payload)
    if (!msg.payload().empty()) {
        // Try to deliver to connection first
        deliver_connection_data(msg);
        return;
    }
    
    std::unique_lock lock(_mutex);
    
    // Get destination address
    Addr dst = msg.destination_address();
    
    // Queue message at destination node (connectionless)
    _message_queues[dst].push(std::move(msg));
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::deliver_connection_data(network_simulator::Message<Addr, Port> msg) -> void {
    std::shared_lock lock(_mutex);
    
    // Create destination endpoint
    network_simulator::Endpoint<Addr, Port> dst_endpoint{msg.destination_address(), msg.destination_port()};
    
    // Find connection for this endpoint
    auto conn_it = _connections.find(dst_endpoint);
    if (conn_it != _connections.end()) {
        // Deliver data to connection
        auto connection = conn_it->second;
        lock.unlock();
        
        // Extract payload and deliver
        auto payload = msg.payload();
        connection->deliver_data(std::move(payload));
    } else {
        // No connection found - this could be a connectionless message
        // Fall back to regular message delivery
        lock.unlock();
        std::unique_lock write_lock(_mutex);
        Addr dst = msg.destination_address();
        _message_queues[dst].push(std::move(msg));
    }
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::retrieve_message(Addr address) -> FutureType {
    // Poll for message without timeout
    std::unique_lock lock(_mutex);
    
    auto& queue = _message_queues[address];
    if (!queue.empty()) {
        auto msg = std::move(queue.front());
        queue.pop();
        return FutureType(std::move(msg));
    }
    
    // No message available - return a future that will never complete
    // TODO: Implement proper message waiting mechanism
    // For now, return an empty message as a temporary fix
    return FutureType(network_simulator::Message<Addr, Port>{});
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::retrieve_message(Addr address, std::chrono::milliseconds timeout) -> FutureType {
    // Check if message is available immediately
    {
        std::unique_lock lock(_mutex);
        auto& queue = _message_queues[address];
        if (!queue.empty()) {
            auto msg = std::move(queue.front());
            queue.pop();
            return FutureType(std::move(msg));
        }
    }
    
    // No message available - poll with short intervals
    // TODO: Implement proper message waiting with timeout mechanism
    // For now, return timeout immediately as a temporary fix
    return FutureType(std::make_exception_ptr(network_simulator::TimeoutException()));
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::establish_connection(Addr src_addr, Port src_port, Addr dst_addr, Port dst_port) -> kythira::Future<std::shared_ptr<Connection<Addr, Port, FutureType>>> {
    // Check if simulator is started
    if (!_started) {
        return kythira::Future<std::shared_ptr<Connection<Addr, Port, FutureType>>>(
            std::make_exception_ptr(std::runtime_error("Simulator not started")));
    }
    
    // Check if both nodes exist in topology
    {
        std::shared_lock lock(_mutex);
        if (_topology.find(src_addr) == _topology.end()) {
            return kythira::Future<std::shared_ptr<Connection<Addr, Port, FutureType>>>(
                std::make_exception_ptr(network_simulator::NodeNotFoundException("Source node not found")));
        }
        if (_topology.find(dst_addr) == _topology.end()) {
            return kythira::Future<std::shared_ptr<Connection<Addr, Port, FutureType>>>(
                std::make_exception_ptr(network_simulator::NodeNotFoundException("Destination node not found")));
        }
    }
    
    // Check if there's a route from source to destination
    bool has_route = false;
    {
        std::shared_lock lock(_mutex);
        auto from_it = _topology.find(src_addr);
        if (from_it != _topology.end()) {
            has_route = from_it->second.find(dst_addr) != from_it->second.end();
        }
    }
    
    if (!has_route) {
        return kythira::Future<std::shared_ptr<Connection<Addr, Port, FutureType>>>(
            std::make_exception_ptr(network_simulator::NoRouteException("No route to destination", "No route to destination")));
    }
    
    // Check reliability - connection may fail
    if (!check_reliability(src_addr, dst_addr)) {
        return kythira::Future<std::shared_ptr<Connection<Addr, Port, FutureType>>>(
            std::make_exception_ptr(std::runtime_error("Connection failed due to reliability")));
    }
    
    // Get latency for connection establishment
    // For now, create connection immediately without latency simulation
    // TODO: Implement proper latency simulation for generic futures
    
    // Create endpoints
    network_simulator::Endpoint<Addr, Port> local_endpoint{src_addr, src_port};
    network_simulator::Endpoint<Addr, Port> remote_endpoint{dst_addr, dst_port};
    
    // Check if there's a listener on the destination endpoint
    std::shared_ptr<Listener<Addr, Port, FutureType>> listener;
    {
        std::shared_lock lock(_mutex);
        auto listener_it = _listeners.find(remote_endpoint);
        if (listener_it != _listeners.end()) {
            listener = listener_it->second;
        }
    }
    
    if (!listener) {
        return kythira::Future<std::shared_ptr<Connection<Addr, Port, FutureType>>>(
            std::make_exception_ptr(std::runtime_error("No listener on destination endpoint")));
    }
    
    // Create client-side connection
    auto client_connection = std::make_shared<Connection<Addr, Port, FutureType>>(
        local_endpoint, 
        remote_endpoint, 
        this
    );
    
    // Create server-side connection (with endpoints swapped)
    auto server_connection = std::make_shared<Connection<Addr, Port, FutureType>>(
        remote_endpoint,
        local_endpoint,
        this
    );
    
    // Store both connections in simulator
    {
        std::unique_lock lock(_mutex);
        _connections[client_connection->local_endpoint()] = client_connection;
        _connections[server_connection->local_endpoint()] = server_connection;
    }
    
    // Queue the server-side connection to the listener
    listener->queue_pending_connection(server_connection);
    
    return kythira::Future<std::shared_ptr<Connection<Addr, Port, FutureType>>>(client_connection);
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::create_listener(Addr addr, Port port) -> kythira::Future<std::shared_ptr<Listener<Addr, Port, FutureType>>> {
    // Check if simulator is started
    if (!_started) {
        return kythira::Future<std::shared_ptr<Listener<Addr, Port, FutureType>>>(
            std::make_exception_ptr(std::runtime_error("Simulator not started")));
    }
    
    // Check if node exists in topology
    {
        std::shared_lock lock(_mutex);
        if (_topology.find(addr) == _topology.end()) {
            return kythira::Future<std::shared_ptr<Listener<Addr, Port, FutureType>>>(
                std::make_exception_ptr(network_simulator::NodeNotFoundException("Node not found")));
        }
    }
    
    // Check if port is already in use
    network_simulator::Endpoint<Addr, Port> endpoint{addr, port};
    {
        std::shared_lock lock(_mutex);
        if (_listeners.find(endpoint) != _listeners.end()) {
            return kythira::Future<std::shared_ptr<Listener<Addr, Port, FutureType>>>(
                std::make_exception_ptr(network_simulator::PortInUseException("Port already in use")));
        }
    }
    
    // Create listener immediately (no latency for bind operation)
    auto listener = std::make_shared<Listener<Addr, Port, FutureType>>(endpoint, this);
    
    // Store listener in simulator
    {
        std::unique_lock lock(_mutex);
        _listeners[endpoint] = listener;
    }
    
    return kythira::Future<std::shared_ptr<Listener<Addr, Port, FutureType>>>(listener);
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::create_listener(Addr addr) -> kythira::Future<std::shared_ptr<Listener<Addr, Port, FutureType>>> {
    // Allocate a random port
    Port random_port;
    
    if constexpr (std::is_same_v<Port, unsigned short>) {
        // For unsigned short, use a random port in the ephemeral range
        std::uniform_int_distribution<unsigned short> port_dist(49152, 65535);
        std::unique_lock lock(_mutex);
        random_port = port_dist(_rng);
        
        // Find an unused port
        network_simulator::Endpoint<Addr, Port> test_endpoint{addr, random_port};
        while (_listeners.find(test_endpoint) != _listeners.end()) {
            random_port = port_dist(_rng);
            test_endpoint = network_simulator::Endpoint<Addr, Port>{addr, random_port};
        }
    } else if constexpr (std::is_same_v<Port, std::string>) {
        // For string, generate unique names
        std::size_t counter = 0;
        std::unique_lock lock(_mutex);
        do {
            random_port = "listener_" + std::to_string(counter++);
        } while (_listeners.find(network_simulator::Endpoint<Addr, Port>{addr, random_port}) != _listeners.end());
    }
    
    // Use the specific port version
    return create_listener(std::move(addr), std::move(random_port));
}

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>

auto NetworkSimulator<Addr, Port, FutureType>::create_listener(Addr addr, Port port, std::chrono::milliseconds timeout) -> kythira::Future<std::shared_ptr<Listener<Addr, Port, FutureType>>> {
    // For now, just call the base method without timeout handling
    // TODO: Implement proper timeout handling for generic futures
    return create_listener(std::move(addr), std::move(port));
}

} // namespace kythira
