#pragma once

#include "simulator.hpp"
#include "node.hpp"
#include "connection.hpp"
#include "listener.hpp"
#include <algorithm>
#include <future>
#include <random>
#include <sstream>
#include <queue>
#include <unordered_set>
#include <unordered_map>

#ifdef FOLLY_FUTURES_AVAILABLE
#include <folly/futures/Future.h>
#endif

#ifdef FOLLY_FUTURES_AVAILABLE
#include <folly/futures/Future.h>
#endif
#include <thread>

namespace network_simulator {

// NetworkSimulator Implementation

template<typename Types>
auto NetworkSimulator<Types>::add_node(address_type address) -> void {
    std::unique_lock lock(_mutex);
    
    // Add node to topology if not already present
    if (_topology.find(address) == _topology.end()) {
        _topology[address] = std::unordered_map<address_type, NetworkEdge>{};
    }
    
    // Initialize message queue for this node
    if (_message_queues.find(address) == _message_queues.end()) {
        _message_queues[address] = std::queue<message_type>{};
    }
}

template<typename Types>
auto NetworkSimulator<Types>::remove_node(address_type address) -> void {
    std::unique_lock lock(_mutex);
    
    // Remove node from topology
    _topology.erase(address);
    
    // Remove all edges pointing to this node
    for (auto& [from_addr, edges] : _topology) {
        edges.erase(address);
    }
    
    // Remove from active nodes
    _nodes.erase(address);
    
    // Clear message queue
    _message_queues.erase(address);
    
    // Remove connections involving this node
    auto conn_it = _connections.begin();
    while (conn_it != _connections.end()) {
        const auto& conn_id = conn_it->first;
        if (conn_id.src_addr == address || conn_id.dst_addr == address) {
            conn_it = _connections.erase(conn_it);
        } else {
            ++conn_it;
        }
    }
    
    // Remove listeners on this node
    auto listener_it = _listeners.begin();
    while (listener_it != _listeners.end()) {
        const auto& endpoint = listener_it->first;
        if (endpoint.address == address) {
            listener_it = _listeners.erase(listener_it);
        } else {
            ++listener_it;
        }
    }
}

template<typename Types>
auto NetworkSimulator<Types>::add_edge(address_type from, address_type to, NetworkEdge edge) -> void {
    std::unique_lock lock(_mutex);
    
    // Ensure both nodes exist in topology
    if (_topology.find(from) == _topology.end()) {
        _topology[from] = std::unordered_map<address_type, NetworkEdge>{};
    }
    if (_topology.find(to) == _topology.end()) {
        _topology[to] = std::unordered_map<address_type, NetworkEdge>{};
    }
    
    // Add the directed edge
    _topology[from][to] = edge;
}

template<typename Types>
auto NetworkSimulator<Types>::remove_edge(address_type from, address_type to) -> void {
    std::unique_lock lock(_mutex);
    
    auto from_it = _topology.find(from);
    if (from_it != _topology.end()) {
        from_it->second.erase(to);
    }
}

template<typename Types>
auto NetworkSimulator<Types>::create_node(address_type address) -> std::shared_ptr<node_type> {
    std::unique_lock lock(_mutex);
    
    // Check if node already exists
    auto it = _nodes.find(address);
    if (it != _nodes.end()) {
        return it->second;
    }
    
    // Ensure node exists in topology (without calling add_node to avoid deadlock)
    if (_topology.find(address) == _topology.end()) {
        _topology[address] = std::unordered_map<address_type, NetworkEdge>{};
    }
    
    // Initialize message queue for this node
    if (_message_queues.find(address) == _message_queues.end()) {
        _message_queues[address] = std::queue<message_type>{};
    }
    
    auto node = std::make_shared<node_type>(address, this);
    _nodes[address] = node;
    return node;
}

template<typename Types>
auto NetworkSimulator<Types>::has_node(address_type address) const -> bool {
    std::shared_lock lock(_mutex);
    return _topology.find(address) != _topology.end();
}

template<typename Types>
auto NetworkSimulator<Types>::has_edge(address_type from, address_type to) const -> bool {
    std::shared_lock lock(_mutex);
    
    auto from_it = _topology.find(from);
    if (from_it == _topology.end()) {
        return false;
    }
    
    return from_it->second.find(to) != from_it->second.end();
}

template<typename Types>
auto NetworkSimulator<Types>::get_edge(address_type from, address_type to) const -> NetworkEdge {
    std::shared_lock lock(_mutex);
    
    auto from_it = _topology.find(from);
    if (from_it == _topology.end()) {
        throw NoRouteException("Node not found", "Node not found");
    }
    
    auto to_it = from_it->second.find(to);
    if (to_it == from_it->second.end()) {
        throw NoRouteException("Edge not found", "Edge not found");
    }
    
    return to_it->second;
}

template<typename Types>
auto NetworkSimulator<Types>::seed_rng(std::uint32_t seed) -> void {
    std::unique_lock lock(_mutex);
    _rng.seed(seed);
}

// Simulation Control Methods

template<typename Types>
auto NetworkSimulator<Types>::start() -> void {
    std::unique_lock lock(_mutex);
    
    if (_started.load()) {
        return; // Already started
    }
    
    _started.store(true);
    
    // Note: Using synchronous delivery for both messages and connection data
    // to avoid threading complexity while maintaining correct behavior
}

template<typename Types>
auto NetworkSimulator<Types>::stop() -> void {
    std::unique_lock lock(_mutex);
    
    if (!_started.load()) {
        return; // Already stopped
    }
    
    _started.store(false);
    
    // Complete pending operations before stopping
    // Close all connections
    for (auto& [endpoint, connection] : _connections) {
        if (connection && connection->is_open()) {
            connection->close();
        }
    }
    
    // Close all listeners using ListenerManager
    if (_listener_manager) {
        _listener_manager->cleanup_all_listeners();
    }
    
    // Also close listeners in the legacy _listeners map for backward compatibility
    for (auto& [endpoint, listener] : _listeners) {
        if (listener && listener->is_listening()) {
            listener->close();
        }
    }
}

template<typename Types>
auto NetworkSimulator<Types>::reset() -> void {
    std::unique_lock lock(_mutex);
    
    // Stop the simulator first
    bool was_started = _started.load();
    _started.store(false);
    
    // Close all connections before clearing
    for (auto& [endpoint, connection] : _connections) {
        if (connection && connection->is_open()) {
            connection->close();
        }
    }
    
    // Close all listeners using ListenerManager
    if (_listener_manager) {
        _listener_manager->cleanup_all_listeners();
    }
    
    // Also close listeners in the legacy _listeners map for backward compatibility
    for (auto& [endpoint, listener] : _listeners) {
        if (listener && listener->is_listening()) {
            listener->close();
        }
    }
    
    // Clear all state and return to initial conditions
    _topology.clear();
    _nodes.clear();
    _message_queues.clear();
    _connections.clear();
    _listeners.clear();
    
    // Reset random number generator
    _rng.seed(std::random_device{}());
}

// Path Finding for Multi-hop Routing

template<typename Types>
auto NetworkSimulator<Types>::find_path(address_type from, address_type to) -> std::vector<address_type> {
    // Note: This method is called from within other methods that already hold the lock
    // So we don't acquire the lock here to avoid deadlock
    
    if (from == to) {
        return {from};  // Same node
    }
    
    // Use BFS to find shortest path
    std::queue<address_type> queue;
    std::unordered_map<address_type, address_type> parent;
    std::unordered_set<address_type> visited;
    
    queue.push(from);
    visited.insert(from);
    parent[from] = from;  // Mark root
    
    while (!queue.empty()) {
        auto current = queue.front();
        queue.pop();
        
        if (current == to) {
            // Found destination, reconstruct path
            std::vector<address_type> path;
            auto node = to;
            
            while (node != from) {
                path.push_back(node);
                node = parent[node];
            }
            path.push_back(from);
            
            // Reverse to get path from source to destination
            std::reverse(path.begin(), path.end());
            return path;
        }
        
        // Explore neighbors
        auto current_it = _topology.find(current);
        if (current_it != _topology.end()) {
            for (const auto& [neighbor, edge] : current_it->second) {
                if (visited.find(neighbor) == visited.end()) {
                    visited.insert(neighbor);
                    parent[neighbor] = current;
                    queue.push(neighbor);
                }
            }
        }
    }
    
    // No path found
    return {};
}

// Message Routing Logic

template<typename Types>
auto NetworkSimulator<Types>::route_message(message_type msg) -> future_bool_type {
    std::unique_lock lock(_mutex);  // Need unique lock for _rng in check_reliability
    
    if (!_started.load()) {
        // Return false if simulator is not started
        if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
            return future_bool_type(false);
        } else {
            return future_bool_type(false);
        }
    }
    
    auto src_addr = msg.source_address();
    auto dst_addr = msg.destination_address();
    
    // Check if source and destination nodes exist
    if (_topology.find(src_addr) == _topology.end() || _topology.find(dst_addr) == _topology.end()) {
        if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
            return future_bool_type(false);
        } else {
            return future_bool_type(false);
        }
    }
    
    // Find path from source to destination using BFS
    auto path = find_path(src_addr, dst_addr);
    if (path.empty()) {
        // No route exists
        if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
            return future_bool_type(false);
        } else {
            return future_bool_type(false);
        }
    }
    
    // Apply reliability and latency for the entire path
    std::chrono::milliseconds total_delay{0};
    
    // Check reliability and calculate latency for each hop in the path
    for (std::size_t i = 0; i < path.size() - 1; ++i) {
        const auto& hop_from = path[i];
        const auto& hop_to = path[i + 1];
        
        // Apply reliability check for this hop
        if (!check_reliability(hop_from, hop_to)) {
            if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
                return future_bool_type(false);
            } else {
                return future_bool_type(false);
            }
        }
        
        // Accumulate latency for this hop
        total_delay += apply_latency(hop_from, hop_to);
    }
    
    // Apply the total latency delay
    if (total_delay.count() > 0) {
        // Release the lock before sleeping to avoid blocking other operations
        lock.unlock();
        std::this_thread::sleep_for(total_delay);
        lock.lock();
        
        // Check if simulator is still started after the delay
        if (!_started.load()) {
            if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
                return future_bool_type(false);
            } else {
                return future_bool_type(false);
            }
        }
    }
    
    // Deliver message immediately after delay
    deliver_message(std::move(msg));
    
    if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
        return future_bool_type(true);
    } else {
        return future_bool_type(true);
    }
}

template<typename Types>
auto NetworkSimulator<Types>::apply_latency(address_type from, address_type to) -> std::chrono::milliseconds {
    // Note: This method is called from within other methods that already hold the lock
    // So we don't acquire the lock here to avoid deadlock
    
    auto from_it = _topology.find(from);
    if (from_it == _topology.end()) {
        return std::chrono::milliseconds(0);
    }
    
    auto to_it = from_it->second.find(to);
    if (to_it == from_it->second.end()) {
        return std::chrono::milliseconds(0);
    }
    
    return to_it->second.latency();
}

template<typename Types>
auto NetworkSimulator<Types>::check_reliability(address_type from, address_type to) -> bool {
    // Note: This method is called from within other methods that already hold the lock
    // So we don't acquire the lock here to avoid deadlock
    
    auto from_it = _topology.find(from);
    if (from_it == _topology.end()) {
        return false;
    }
    
    auto to_it = from_it->second.find(to);
    if (to_it == from_it->second.end()) {
        return false;
    }
    
    double reliability = to_it->second.reliability();
    
    // Handle perfect reliability case explicitly
    if (reliability >= 1.0) {
        return true;
    }
    
    // Use bernoulli_distribution for probabilistic message drops
    std::bernoulli_distribution dist(reliability);
    
    // Message succeeds based on the bernoulli distribution
    return dist(_rng);
}

// Message Delivery

template<typename Types>
auto NetworkSimulator<Types>::deliver_message(message_type msg) -> void {
    // Note: This method is called from within other methods that already hold the lock
    // So we don't acquire the lock here to avoid deadlock
    
    auto dst_addr = msg.destination_address();
    
    // Queue message at destination node
    auto queue_it = _message_queues.find(dst_addr);
    if (queue_it != _message_queues.end()) {
        queue_it->second.push(std::move(msg));
        
        // Notify any threads waiting for messages at this address
        // Note: In a full implementation, we would have per-node condition variables
        // For now, we rely on the polling mechanism in retrieve_message
    }
}

template<typename Types>
auto NetworkSimulator<Types>::retrieve_message(address_type address) -> future_message_type {
    std::unique_lock lock(_mutex);
    
    auto queue_it = _message_queues.find(address);
    if (queue_it == _message_queues.end() || queue_it->second.empty()) {
        // No messages available - in a full implementation, this would block
        // For now, return an empty message
        if constexpr (std::is_same_v<future_message_type, SimpleFuture<message_type>>) {
            return future_message_type(message_type{});
        } else {
            return future_message_type(message_type{});
        }
    }
    
    auto msg = queue_it->second.front();
    queue_it->second.pop();
    if constexpr (std::is_same_v<future_message_type, SimpleFuture<message_type>>) {
        return future_message_type(std::move(msg));
    } else {
        return future_message_type(std::move(msg));
    }
}

template<typename Types>
auto NetworkSimulator<Types>::retrieve_message(address_type address, std::chrono::milliseconds timeout) -> future_message_type {
    std::unique_lock lock(_mutex);
    
    auto queue_it = _message_queues.find(address);
    if (queue_it == _message_queues.end() || queue_it->second.empty()) {
        // No messages available - throw TimeoutException for timeout version
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<message_type>(
            folly::exception_wrapper(TimeoutException()));
#else
        return future_message_type(std::make_exception_ptr(TimeoutException()));
#endif
    }
    
    auto msg = queue_it->second.front();
    queue_it->second.pop();
    if constexpr (std::is_same_v<future_message_type, SimpleFuture<message_type>>) {
        return future_message_type(std::move(msg));
    } else {
        return future_message_type(std::move(msg));
    }
}

// Connection and Listener Management (stubs for now)

template<typename Types>
auto NetworkSimulator<Types>::establish_connection(address_type src_addr, port_type src_port, 
                                                  address_type dst_addr, port_type dst_port) -> future_connection_type {
    // Check if connection pooling is enabled and try to reuse existing connection
    endpoint_type destination_endpoint(dst_addr, dst_port);
    
    if (_connection_config.enable_connection_pooling && _connection_pool) {
        // Use get_or_create_connection which will reuse if available or create new
        return _connection_pool->get_or_create_connection(destination_endpoint, [&]() {
            return establish_connection_internal(src_addr, src_port, dst_addr, dst_port);
        });
    } else {
        // Connection pooling disabled, create connection directly
        return establish_connection_internal(src_addr, src_port, dst_addr, dst_port);
    }
}

template<typename Types>
auto NetworkSimulator<Types>::establish_connection_internal(address_type src_addr, port_type src_port, 
                                                           address_type dst_addr, port_type dst_port) -> future_connection_type {
    // First, check basic conditions without holding the lock for too long
    {
        std::shared_lock lock(_mutex);
        
        if (!_started.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
            // For folly::Future, return exception for not started
            return folly::makeFuture<std::shared_ptr<connection_type>>(
                folly::exception_wrapper(std::runtime_error("Simulator not started")));
#else
            return future_connection_type(std::make_exception_ptr(std::runtime_error("Simulator not started")));
#endif
        }
        
        // Check if there's a route between the addresses using path finding
        auto path = find_path(src_addr, dst_addr);
        if (path.empty()) {
#ifdef FOLLY_FUTURES_AVAILABLE
            // For folly::Future, return exception for no route
            return folly::makeFuture<std::shared_ptr<connection_type>>(
                folly::exception_wrapper(NoRouteException("No route from " + src_addr + " to " + dst_addr, 
                                                         "No route from " + src_addr + " to " + dst_addr)));
#else
            return future_connection_type(std::make_exception_ptr(
                NoRouteException("No route from " + src_addr + " to " + dst_addr, 
                               "No route from " + src_addr + " to " + dst_addr)));
#endif
        }
    }
    
    // Create connection endpoints
    endpoint_type client_endpoint(src_addr, src_port);
    endpoint_type server_endpoint(dst_addr, dst_port);
    
    // Find and validate the listener
    std::shared_ptr<listener_type> listener;
    {
        std::shared_lock lock(_mutex);
        auto listener_it = _listeners.find(server_endpoint);
        if (listener_it == _listeners.end() || !listener_it->second) {
            // Debug: Print available listeners
            std::string available_listeners;
            for (const auto& [ep, l] : _listeners) {
                available_listeners += "(" + ep.address + ":" + std::to_string(ep.port) + ") ";
            }
            std::string error_msg = "Connection refused: no listener on " + dst_addr + ":" + std::to_string(dst_port) + 
                                  ". Available listeners: " + available_listeners;
#ifdef FOLLY_FUTURES_AVAILABLE
            // For folly::Future, return exception for no listener - this should cause timeout
            return folly::makeFuture<std::shared_ptr<connection_type>>(
                folly::exception_wrapper(std::runtime_error(error_msg)));
#else
            return future_connection_type(std::make_exception_ptr(
                std::runtime_error(error_msg)));
#endif
        }
        
        listener = listener_it->second;
        if (!listener->is_listening()) {
#ifdef FOLLY_FUTURES_AVAILABLE
            // For folly::Future, return exception for listener not listening
            return folly::makeFuture<std::shared_ptr<connection_type>>(
                folly::exception_wrapper(std::runtime_error("Connection refused: listener not accepting connections")));
#else
            return future_connection_type(std::make_exception_ptr(
                std::runtime_error("Connection refused: listener not accepting connections")));
#endif
        }
    }
    
    // Create connection IDs using 4-tuple
    connection_id_type client_conn_id(src_addr, src_port, dst_addr, dst_port);
    connection_id_type server_conn_id(dst_addr, dst_port, src_addr, src_port);  // Reversed for server side
    
    // Apply reliability check for connection establishment
    // Note: For connection establishment, we use perfect reliability to ensure connections can be made
    // Reliability is applied to data transfer, not connection establishment
    bool reliability_passed = true;  // Always allow connection establishment
    
    if (!reliability_passed) {
#ifdef FOLLY_FUTURES_AVAILABLE
        // For folly::Future, return exception for reliability failure
        return folly::makeFuture<std::shared_ptr<connection_type>>(
            folly::exception_wrapper(std::runtime_error("Connection failed due to network unreliability")));
#else
        return future_connection_type(std::make_exception_ptr(
            std::runtime_error("Connection failed due to network unreliability")));
#endif
    }
    
    // Apply latency delay for connection establishment
    std::chrono::milliseconds delay;
    {
        std::shared_lock lock(_mutex);
        delay = apply_latency(src_addr, dst_addr);
    }
    
    // For connection establishment, apply the latency delay synchronously
    if (delay.count() > 0) {
        std::this_thread::sleep_for(delay);
        
        // Check if simulator is still started after the delay
        std::shared_lock lock(_mutex);
        if (!_started.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
            return folly::makeFuture<std::shared_ptr<connection_type>>(
                folly::exception_wrapper(std::runtime_error("Simulator stopped during connection establishment")));
#else
            return future_connection_type(std::make_exception_ptr(
                std::runtime_error("Simulator stopped during connection establishment")));
#endif
        }
        
        // Re-check listener is still available after delay
        auto listener_it = _listeners.find(server_endpoint);
        if (listener_it == _listeners.end() || !listener_it->second || !listener_it->second->is_listening()) {
#ifdef FOLLY_FUTURES_AVAILABLE
            return folly::makeFuture<std::shared_ptr<connection_type>>(
                folly::exception_wrapper(std::runtime_error("Connection refused: listener unavailable after delay")));
#else
            return future_connection_type(std::make_exception_ptr(
                std::runtime_error("Connection refused: listener unavailable after delay")));
#endif
        }
        listener = listener_it->second;
    }
    
    // Create connections
    auto client_connection = std::make_shared<connection_type>(client_endpoint, server_endpoint, this);
    auto server_connection = std::make_shared<connection_type>(server_endpoint, client_endpoint, this);
    
    // Store connections with unique lock
    {
        std::unique_lock lock(_mutex);
        _connections[client_conn_id] = client_connection;
        _connections[server_conn_id] = server_connection;
    }
    
    // Register connections with the connection tracker
    if (_connection_tracker) {
        _connection_tracker->register_connection(client_endpoint, server_endpoint, client_connection);
        _connection_tracker->register_connection(server_endpoint, client_endpoint, server_connection);
    }
    
    // Notify the listener about the incoming connection
    listener->queue_pending_connection(server_connection);
    
#ifdef FOLLY_FUTURES_AVAILABLE
    // For folly::Future, use folly::makeFuture
    return folly::makeFuture(client_connection);
#else
    return future_connection_type(client_connection);
#endif
}

template<typename Types>
auto NetworkSimulator<Types>::create_listener(address_type addr, port_type port) -> future_listener_type {
    std::unique_lock lock(_mutex);
    
    if (!_started.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<listener_type>>(
            folly::exception_wrapper(std::runtime_error("Simulator not started")));
#else
        return future_listener_type(std::make_exception_ptr(std::runtime_error("Simulator not started")));
#endif
    }
    
    endpoint_type local_endpoint(addr, port);
    
    // Check if port is already in use using ListenerManager
    if (_listener_manager && !_listener_manager->is_port_available(addr, port)) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<listener_type>>(
            folly::exception_wrapper(PortInUseException("Port " + std::to_string(port) + " is already in use")));
#else
        return future_listener_type(std::make_exception_ptr(
            PortInUseException("Port " + std::to_string(port) + " is already in use")));
#endif
    }
    
    // Check legacy _listeners map and clean up closed listeners
    auto it = _listeners.find(local_endpoint);
    if (it != _listeners.end()) {
        // If the listener is no longer listening, remove it
        if (!it->second || !it->second->is_listening()) {
            _listeners.erase(it);
        } else {
            // Port is still in use by an active listener
#ifdef FOLLY_FUTURES_AVAILABLE
            return folly::makeFuture<std::shared_ptr<listener_type>>(
                folly::exception_wrapper(PortInUseException("Port " + std::to_string(port) + " is already in use")));
#else
            return future_listener_type(std::make_exception_ptr(
                PortInUseException("Port " + std::to_string(port) + " is already in use")));
#endif
        }
    }
    
    auto listener = std::make_shared<listener_type>(local_endpoint, this);
    _listeners[local_endpoint] = listener;
    
    // Register listener with ListenerManager
    if (_listener_manager) {
        _listener_manager->register_listener(local_endpoint, listener);
    }
    
#ifdef FOLLY_FUTURES_AVAILABLE
    return folly::makeFuture(listener);
#else
    return future_listener_type(listener);
#endif
}

// Timer and Scheduling Implementation

template<typename Types>
auto NetworkSimulator<Types>::timer_thread_main() -> void {
    while (_started.load()) {
        std::unique_lock<std::mutex> timer_lock(_timer_mutex);
        
        // Process any scheduled deliveries that are ready
        process_scheduled_deliveries();
        
        // Calculate next wake-up time
        auto now = std::chrono::steady_clock::now();
        auto next_wake_time = now + std::chrono::milliseconds(10); // Check every 10ms
        
        // Check if there are any scheduled messages that need earlier wake-up
        if (!_scheduled_messages.empty()) {
            auto next_message_time = _scheduled_messages.top().delivery_time;
            next_wake_time = std::min(next_wake_time, next_message_time);
        }
        
        // Check if there are any scheduled connection data that need earlier wake-up
        if (!_scheduled_connection_data.empty()) {
            auto next_data_time = _scheduled_connection_data.top().delivery_time;
            next_wake_time = std::min(next_wake_time, next_data_time);
        }
        
        // Check if there are any scheduled connection establishments that need earlier wake-up
        if (!_scheduled_connection_establishments.empty()) {
            auto next_establishment_time = _scheduled_connection_establishments.top().delivery_time;
            next_wake_time = std::min(next_wake_time, next_establishment_time);
        }
        
        // Wait until next wake-up time or until notified
        if (next_wake_time > now) {
            _timer_cv.wait_until(timer_lock, next_wake_time);
        }
    }
}

template<typename Types>
auto NetworkSimulator<Types>::schedule_message_delivery(message_type msg, std::chrono::milliseconds delay) -> void {
    auto delivery_time = std::chrono::steady_clock::now() + delay;
    
    {
        std::lock_guard<std::mutex> timer_lock(_timer_mutex);
        _scheduled_messages.emplace(ScheduledMessage{delivery_time, std::move(msg)});
        _timer_cv.notify_one();  // Wake up timer thread for new scheduled message
    }
}

template<typename Types>
auto NetworkSimulator<Types>::schedule_connection_data_delivery(connection_id_type conn_id, std::vector<std::byte> data, std::chrono::milliseconds delay) -> void {
    auto delivery_time = std::chrono::steady_clock::now() + delay;
    
    {
        std::lock_guard<std::mutex> timer_lock(_timer_mutex);
        _scheduled_connection_data.emplace(ScheduledConnectionData{delivery_time, conn_id, std::move(data)});
        _timer_cv.notify_one();  // Wake up timer thread for new scheduled data
    }
}

template<typename Types>
auto NetworkSimulator<Types>::schedule_connection_establishment(std::shared_ptr<listener_type> listener, std::shared_ptr<connection_type> connection, std::chrono::milliseconds delay) -> void {
    auto delivery_time = std::chrono::steady_clock::now() + delay;
    
    {
        std::lock_guard<std::mutex> timer_lock(_timer_mutex);
        _scheduled_connection_establishments.emplace(ScheduledConnectionEstablishment{delivery_time, listener, connection});
        _timer_cv.notify_one();  // Wake up timer thread for new scheduled connection
    }
}

template<typename Types>
auto NetworkSimulator<Types>::process_scheduled_deliveries() -> void {
    auto now = std::chrono::steady_clock::now();
    
    // Process scheduled messages
    while (!_scheduled_messages.empty() && _scheduled_messages.top().delivery_time <= now) {
        auto scheduled_msg = _scheduled_messages.top();
        _scheduled_messages.pop();
        
        // Deliver the message (need to acquire simulator lock)
        // Release timer lock temporarily to avoid deadlock
        auto msg_copy = std::move(scheduled_msg.message);
        
        // Use a separate scope to manage the simulator lock
        {
            std::unique_lock<std::shared_mutex> sim_lock(_mutex);
            deliver_message(std::move(msg_copy));
        }
    }
    
    // Process scheduled connection data
    while (!_scheduled_connection_data.empty() && _scheduled_connection_data.top().delivery_time <= now) {
        auto scheduled_data = _scheduled_connection_data.top();
        _scheduled_connection_data.pop();
        
        // Find the destination connection and deliver data
        // Copy the data and connection ID to avoid holding timer lock too long
        auto conn_id = scheduled_data.connection_id;
        auto data_copy = std::move(scheduled_data.data);
        
        // Use a separate scope to manage the simulator lock
        std::shared_ptr<connection_type> dest_connection;
        {
            std::shared_lock<std::shared_mutex> sim_lock(_mutex);
            auto conn_it = _connections.find(conn_id);
            if (conn_it != _connections.end() && conn_it->second && conn_it->second->is_open()) {
                dest_connection = conn_it->second;
            }
        }
        
        // Deliver data outside of any locks to avoid deadlock
        if (dest_connection) {
            dest_connection->deliver_data(std::move(data_copy));
        }
    }
    
    // Process scheduled connection establishments
    while (!_scheduled_connection_establishments.empty() && _scheduled_connection_establishments.top().delivery_time <= now) {
        auto scheduled_establishment = _scheduled_connection_establishments.top();
        _scheduled_connection_establishments.pop();
        
        // Queue the connection to the listener (outside of any locks to avoid deadlock)
        if (scheduled_establishment.listener && scheduled_establishment.connection) {
            scheduled_establishment.listener->queue_pending_connection(scheduled_establishment.connection);
        }
    }
}

template<typename Types>
auto NetworkSimulator<Types>::create_listener(address_type addr) -> future_listener_type {
    std::unique_lock lock(_mutex);
    
    if (!_started.load()) {
        if constexpr (std::is_same_v<future_listener_type, SimpleFuture<std::shared_ptr<listener_type>>>) {
            return future_listener_type(std::shared_ptr<listener_type>{});
        } else {
            return future_listener_type(std::shared_ptr<listener_type>{});
        }
    }
    
    // Find an unused port
    port_type port;
    endpoint_type local_endpoint(addr, port);
    
    if constexpr (std::is_same_v<port_type, unsigned short>) {
        // For unsigned short ports, use standard ephemeral port range
        constexpr unsigned short ephemeral_start = 49152;
        constexpr unsigned short ephemeral_end = 65535;
        
        bool found_port = false;
        for (unsigned short candidate_port = ephemeral_start; candidate_port <= ephemeral_end; ++candidate_port) {
            local_endpoint = endpoint_type(addr, candidate_port);
            if (_listeners.find(local_endpoint) == _listeners.end()) {
                port = candidate_port;
                found_port = true;
                break;
            }
        }
        
        if (!found_port) {
            // No available ports
            if constexpr (std::is_same_v<future_listener_type, SimpleFuture<std::shared_ptr<listener_type>>>) {
                return future_listener_type(std::shared_ptr<listener_type>{});
            } else {
                return future_listener_type(std::shared_ptr<listener_type>{});
            }
        }
        
    } else if constexpr (std::is_same_v<port_type, std::string>) {
        // For string ports, generate unique string identifiers
        static std::atomic<std::size_t> counter{0};
        
        bool found_port = false;
        for (std::size_t attempts = 0; attempts < 1000; ++attempts) {
            std::ostringstream oss;
            oss << "auto_" << counter++;
            port = oss.str();
            local_endpoint = endpoint_type(addr, port);
            
            if (_listeners.find(local_endpoint) == _listeners.end()) {
                found_port = true;
                break;
            }
        }
        
        if (!found_port) {
            // No available ports after many attempts
            if constexpr (std::is_same_v<future_listener_type, SimpleFuture<std::shared_ptr<listener_type>>>) {
                return future_listener_type(std::shared_ptr<listener_type>{});
            } else {
                return future_listener_type(std::shared_ptr<listener_type>{});
            }
        }
        
    } else {
        static_assert(std::is_same_v<port_type, unsigned short> || 
                     std::is_same_v<port_type, std::string>,
                     "Port type must be unsigned short or std::string");
    }
    
    // Create the listener with the found port
    auto listener = std::make_shared<listener_type>(local_endpoint, this);
    _listeners[local_endpoint] = listener;
    
    if constexpr (std::is_same_v<future_listener_type, SimpleFuture<std::shared_ptr<listener_type>>>) {
        return future_listener_type(listener);
    } else {
        return future_listener_type(listener);
    }
}

template<typename Types>
auto NetworkSimulator<Types>::create_listener(address_type addr, port_type port, 
                                             std::chrono::milliseconds timeout) -> future_listener_type {
    // For this basic implementation, timeout behavior is the same as non-timeout version
    return create_listener(addr, port);
}

// Connection Establishment with Timeout Handling

template<typename Types>
auto NetworkSimulator<Types>::establish_connection_with_timeout(address_type src_addr, port_type src_port,
                                                               address_type dst_addr, port_type dst_port,
                                                               std::chrono::milliseconds timeout) -> future_connection_type {
    // Record the connection request with timeout tracking
    endpoint_type source_endpoint(src_addr, src_port);
    endpoint_type destination_endpoint(dst_addr, dst_port);
    
    ConnectionRequest request{
        source_endpoint,
        destination_endpoint,
        std::chrono::steady_clock::now(),
        timeout
    };
    
    // Add to pending connections for tracking
    {
        std::lock_guard<std::mutex> lock(_connection_requests_mutex);
        _pending_connections.push_back(request);
    }
    
    // Attempt to establish the connection
    auto connection_future = establish_connection(src_addr, src_port, dst_addr, dst_port);
    
#ifdef FOLLY_FUTURES_AVAILABLE
    // For folly::Future, use within() for timeout handling
    return std::move(connection_future).within(timeout).onError([=](const folly::FutureTimeout& e) {
        // Remove from pending connections on timeout
        {
            std::lock_guard<std::mutex> lock(_connection_requests_mutex);
            _pending_connections.erase(
                std::remove_if(_pending_connections.begin(), _pending_connections.end(),
                    [&](const ConnectionRequest& req) {
                        return req.source == source_endpoint && req.destination == destination_endpoint;
                    }),
                _pending_connections.end()
            );
        }
        
        // Throw TimeoutException
        throw TimeoutException();
    }).onError([=](const std::exception& e) {
        // Remove from pending connections on any error
        {
            std::lock_guard<std::mutex> lock(_connection_requests_mutex);
            _pending_connections.erase(
                std::remove_if(_pending_connections.begin(), _pending_connections.end(),
                    [&](const ConnectionRequest& req) {
                        return req.source == source_endpoint && req.destination == destination_endpoint;
                    }),
                _pending_connections.end()
            );
        }
        
        // Re-throw the exception
        throw;
    });
#else
    // For SimpleFuture, we don't have timeout support built-in
    // Just return the connection future and let the caller handle timeout
    // The timeout checking will be done at a higher level
    
    // Remove from pending connections immediately for SimpleFuture
    {
        std::lock_guard<std::mutex> lock(_connection_requests_mutex);
        _pending_connections.erase(
            std::remove_if(_pending_connections.begin(), _pending_connections.end(),
                [&](const ConnectionRequest& req) {
                    return req.source == source_endpoint && req.destination == destination_endpoint;
                }),
            _pending_connections.end()
        );
    }
    
    return connection_future;
#endif
}

template<typename Types>
auto NetworkSimulator<Types>::process_connection_timeouts() -> void {
    std::lock_guard<std::mutex> lock(_connection_requests_mutex);
    
    auto now = std::chrono::steady_clock::now();
    
    // Find and remove expired connection requests
    _pending_connections.erase(
        std::remove_if(_pending_connections.begin(), _pending_connections.end(),
            [now](const ConnectionRequest& req) {
                return req.is_expired();
            }),
        _pending_connections.end()
    );
}

template<typename Types>
auto NetworkSimulator<Types>::cancel_expired_connections() -> void {
    std::lock_guard<std::mutex> lock(_connection_requests_mutex);
    
    auto now = std::chrono::steady_clock::now();
    
    // Identify expired requests
    std::vector<ConnectionRequest> expired_requests;
    for (const auto& req : _pending_connections) {
        if (req.is_expired()) {
            expired_requests.push_back(req);
        }
    }
    
    // Remove expired requests from pending list
    _pending_connections.erase(
        std::remove_if(_pending_connections.begin(), _pending_connections.end(),
            [now](const ConnectionRequest& req) {
                return req.is_expired();
            }),
        _pending_connections.end()
    );
    
    // Note: In a more complete implementation, we would also cancel any
    // in-flight connection establishment operations here. For now, we just
    // remove them from tracking.
}

// Connection Data Routing

template<typename Types>
auto NetworkSimulator<Types>::route_connection_data(connection_id_type conn_id, std::vector<std::byte> data) -> future_bool_type {
    std::unique_lock lock(_mutex);
    
    if (!_started.load()) {
        if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
            return future_bool_type(false);
        } else {
            return future_bool_type(false);
        }
    }
    
    // Check if there's a route between the addresses using path finding
    auto path = find_path(conn_id.src_addr, conn_id.dst_addr);
    if (path.empty()) {
        if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
            return future_bool_type(false);
        } else {
            return future_bool_type(false);
        }
    }
    
    // Apply reliability and latency for the entire path
    std::chrono::milliseconds total_delay{0};
    
    // Check reliability and calculate latency for each hop in the path
    for (std::size_t i = 0; i < path.size() - 1; ++i) {
        const auto& hop_from = path[i];
        const auto& hop_to = path[i + 1];
        
        // Apply reliability check for this hop
        if (!check_reliability(hop_from, hop_to)) {
            if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
                return future_bool_type(false);
            } else {
                return future_bool_type(false);
            }
        }
        
        // Accumulate latency for this hop
        total_delay += apply_latency(hop_from, hop_to);
    }
    
    // Find the destination connection using the reverse connection ID
    // When client (A,a) -> (B,b) writes data, it should be delivered to server (B,b) -> (A,a)
    connection_id_type dest_conn_id(conn_id.dst_addr, conn_id.dst_port, conn_id.src_addr, conn_id.src_port);
    
    auto conn_it = _connections.find(dest_conn_id);
    if (conn_it == _connections.end() || !conn_it->second) {
        if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
            return future_bool_type(false);
        } else {
            return future_bool_type(false);
        }
    }
    
    auto dest_connection = conn_it->second;
    if (!dest_connection->is_open()) {
        if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
            return future_bool_type(false);
        } else {
            return future_bool_type(false);
        }
    }
    
    // Apply the total latency delay
    if (total_delay.count() > 0) {
        // Release the lock before sleeping to avoid blocking other operations
        lock.unlock();
        std::this_thread::sleep_for(total_delay);
        lock.lock();
        
        // Check if simulator is still started and connection is still open after the delay
        if (!_started.load() || !dest_connection->is_open()) {
            if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
                return future_bool_type(false);
            } else {
                return future_bool_type(false);
            }
        }
    }
    
    // Update connection tracker statistics for data transfer
    if (_connection_config.enable_connection_tracking && _connection_tracker) {
        endpoint_type src_endpoint(conn_id.src_addr, conn_id.src_port);
        _connection_tracker->update_connection_stats(src_endpoint, data.size(), true);
    }
    
    // Deliver data immediately after delay (outside of lock to avoid deadlock)
    lock.unlock();
    dest_connection->deliver_data(std::move(data));
    
    if constexpr (std::is_same_v<future_bool_type, SimpleFuture<bool>>) {
        return future_bool_type(true);
    } else {
        return future_bool_type(true);
    }
}

// Connection Management Configuration

template<typename Types>
auto NetworkSimulator<Types>::notify_connection_closed(endpoint_type local_endpoint) -> void {
    if (_connection_config.enable_connection_tracking && _connection_tracker) {
        _connection_tracker->update_connection_state(local_endpoint, ConnectionState::CLOSED);
        // Optionally cleanup the connection from tracker after a delay
        // For now, we keep it for historical tracking
    }
}

template<typename Types>
auto NetworkSimulator<Types>::configure_connection_management(ConnectionConfig config) -> void {
    std::unique_lock lock(_mutex);
    _connection_config = config;
    
    // Configure connection pool if enabled
    if (_connection_pool && config.enable_connection_pooling) {
        _connection_pool->configure_pool(config.pool_config);
    }
}

template<typename Types>
auto NetworkSimulator<Types>::get_connection_pool() -> ConnectionPool<Types>& {
    return *_connection_pool;
}

template<typename Types>
auto NetworkSimulator<Types>::get_listener_manager() -> ListenerManager<Types>& {
    return *_listener_manager;
}

template<typename Types>
auto NetworkSimulator<Types>::get_connection_tracker() -> ConnectionTracker<Types>& {
    return *_connection_tracker;
}

} // namespace network_simulator