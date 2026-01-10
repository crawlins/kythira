#pragma once

#include "concepts.hpp"
#include "types.hpp"
#include "exceptions.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace network_simulator {

// Forward declarations
template<typename Types>
class NetworkNode;

template<typename Types>
class Connection;

template<typename Types>
class Listener;

// NetworkSimulator Class Template
template<typename Types>
class NetworkSimulator {
public:
    // Type aliases from Types template argument
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    using message_type = typename Types::message_type;
    using connection_type = typename Types::connection_type;
    using listener_type = typename Types::listener_type;
    using node_type = typename Types::node_type;
    using endpoint_type = Endpoint<Types>;
    using connection_id_type = ConnectionId<Types>;
    
    // Future type aliases
    using future_bool_type = typename Types::future_bool_type;
    using future_message_type = typename Types::future_message_type;
    using future_connection_type = typename Types::future_connection_type;
    using future_listener_type = typename Types::future_listener_type;
    using future_bytes_type = typename Types::future_bytes_type;
    
    NetworkSimulator()
        : _rng(std::random_device{}())
        , _started(false)
    {}
    
    ~NetworkSimulator() = default;
    
    // Topology configuration
    auto add_node(address_type address) -> void;
    auto remove_node(address_type address) -> void;
    auto add_edge(address_type from, address_type to, NetworkEdge edge) -> void;
    auto remove_edge(address_type from, address_type to) -> void;
    
    // Node creation
    auto create_node(address_type address) -> std::shared_ptr<node_type>;
    
    // Simulation control
    auto start() -> void;
    auto stop() -> void;
    auto reset() -> void;
    
    // Query methods for testing
    auto has_node(address_type address) const -> bool;
    auto has_edge(address_type from, address_type to) const -> bool;
    auto get_edge(address_type from, address_type to) const -> NetworkEdge;
    
    // RNG seeding for reproducible tests
    auto seed_rng(std::uint32_t seed) -> void;
    
    // Internal methods (used by NetworkNode, Connection, Listener)
    auto route_message(message_type msg) -> future_bool_type;
    auto deliver_message(message_type msg) -> void;
    auto apply_latency(address_type from, address_type to) -> std::chrono::milliseconds;
    auto check_reliability(address_type from, address_type to) -> bool;
    auto retrieve_message(address_type address) -> future_message_type;
    auto retrieve_message(address_type address, std::chrono::milliseconds timeout) -> future_message_type;
    
    // Connection establishment
    auto establish_connection(address_type src_addr, port_type src_port, 
                             address_type dst_addr, port_type dst_port) -> future_connection_type;
    
    // Listener establishment
    auto create_listener(address_type addr, port_type port) -> future_listener_type;
    auto create_listener(address_type addr) -> future_listener_type;  // Random port
    auto create_listener(address_type addr, port_type port, 
                        std::chrono::milliseconds timeout) -> future_listener_type;
    
    // Connection data routing
    auto route_connection_data(connection_id_type conn_id, std::vector<std::byte> data) -> future_bool_type;
    
private:
    // Path finding for multi-hop routing
    auto find_path(address_type from, address_type to) -> std::vector<address_type>;
    
    // Timer and scheduling methods
    auto timer_thread_main() -> void;
    auto schedule_message_delivery(message_type msg, std::chrono::milliseconds delay) -> void;
    auto schedule_connection_data_delivery(connection_id_type conn_id, std::vector<std::byte> data, std::chrono::milliseconds delay) -> void;
    auto schedule_connection_establishment(std::shared_ptr<listener_type> listener, std::shared_ptr<connection_type> connection, std::chrono::milliseconds delay) -> void;
    auto process_scheduled_deliveries() -> void;
    
private:
    // Scheduled message delivery structure
    struct ScheduledMessage {
        std::chrono::steady_clock::time_point delivery_time;
        message_type message;
        
        // For priority queue (earliest delivery time has highest priority)
        bool operator<(const ScheduledMessage& other) const {
            return delivery_time > other.delivery_time; // Reverse for min-heap
        }
    };
    
    // Scheduled connection data delivery structure
    struct ScheduledConnectionData {
        std::chrono::steady_clock::time_point delivery_time;
        connection_id_type connection_id;
        std::vector<std::byte> data;
        
        // For priority queue (earliest delivery time has highest priority)
        bool operator<(const ScheduledConnectionData& other) const {
            return delivery_time > other.delivery_time; // Reverse for min-heap
        }
    };
    
    // Scheduled connection establishment structure
    struct ScheduledConnectionEstablishment {
        std::chrono::steady_clock::time_point delivery_time;
        std::shared_ptr<listener_type> listener;
        std::shared_ptr<connection_type> connection;
        
        // For priority queue (earliest delivery time has highest priority)
        bool operator<(const ScheduledConnectionEstablishment& other) const {
            return delivery_time > other.delivery_time; // Reverse for min-heap
        }
    };
    
    // Directed graph: adjacency list representation
    std::unordered_map<address_type, std::unordered_map<address_type, NetworkEdge>> _topology;
    
    // Active nodes
    std::unordered_map<address_type, std::shared_ptr<node_type>> _nodes;
    
    // Message queues per node
    std::unordered_map<address_type, std::queue<message_type>> _message_queues;
    
    // Connection state - use ConnectionId as key
    std::unordered_map<connection_id_type, std::shared_ptr<connection_type>> _connections;
    
    // Listeners
    std::unordered_map<endpoint_type, std::shared_ptr<listener_type>> _listeners;
    
    // Random number generator for reliability simulation
    std::mt19937 _rng;
    
    // Timer and scheduling infrastructure
    std::priority_queue<ScheduledMessage> _scheduled_messages;
    std::priority_queue<ScheduledConnectionData> _scheduled_connection_data;
    std::priority_queue<ScheduledConnectionEstablishment> _scheduled_connection_establishments;
    std::thread _timer_thread;
    std::condition_variable _timer_cv;
    mutable std::mutex _timer_mutex;
    
    // Simulation state
    std::atomic<bool> _started;
    
    // Mutex for thread safety
    mutable std::shared_mutex _mutex;
};

} // namespace network_simulator

#include "simulator_impl.hpp"
