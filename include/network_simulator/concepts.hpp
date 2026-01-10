#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <vector>
#include <netinet/in.h>  // For in_addr, in6_addr

namespace network_simulator {

// Individual type concepts

// Address Concept
template<typename T>
concept address = requires(T a, T b) {
    // Must be copyable and movable
    { T(a) } -> std::same_as<T>;
    { T(std::move(a)) } -> std::same_as<T>;
    
    // Must support equality comparison
    { a == b } -> std::convertible_to<bool>;
    { a != b } -> std::convertible_to<bool>;
    
    // Must be hashable for use in maps
    { std::hash<T>{}(a) } -> std::convertible_to<std::size_t>;
};

// Port Concept
template<typename T>
concept port = requires(T p, T q) {
    // Must be copyable and movable
    { T(p) } -> std::same_as<T>;
    { T(std::move(p)) } -> std::same_as<T>;
    
    // Must support equality comparison
    { p == q } -> std::convertible_to<bool>;
    { p != q } -> std::convertible_to<bool>;
    
    // Must be hashable
    { std::hash<T>{}(p) } -> std::convertible_to<std::size_t>;
};

// Future Concept - matches kythira::Future API
template<typename F, typename T>
concept future = requires(F f, const F cf) {
    // Must be able to check if ready
    { cf.isReady() } -> std::convertible_to<bool>;
    
    // Must support timeout
    { f.wait(std::chrono::milliseconds{100}) } -> std::convertible_to<bool>;
    
    // Must support error handling (legacy method name for compatibility)
    { f.onError([](std::exception_ptr) {}) };
} && (
    // Handle void case - get() returns void
    std::is_void_v<T> ? requires(F f) {
        { std::move(f).get() } -> std::same_as<void>;
        { f.then([]() {}) };
    } : requires(F f) {
        // Handle non-void case - get() returns T
        { std::move(f).get() } -> std::same_as<T>;
        { f.then([](T val) { return val; }) };
    }
);

// Message Concept
template<typename M, typename Addr, typename Port>
concept message = requires(M msg) {
    // Must provide source address
    { msg.source_address() } -> std::same_as<Addr>;
    
    // Must provide source port
    { msg.source_port() } -> std::same_as<Port>;
    
    // Must provide destination address
    { msg.destination_address() } -> std::same_as<Addr>;
    
    // Must provide destination port
    { msg.destination_port() } -> std::same_as<Port>;
    
    // Must provide payload access
    { msg.payload() } -> std::same_as<std::vector<std::byte>>;
};

// Connection Concept
template<typename C>
concept connection = requires(C conn, std::vector<std::byte> data) {
    // Must support reading data
    { conn.read() };
    { conn.read(std::chrono::milliseconds{100}) };
    
    // Must support writing data
    { conn.write(data) };
    { conn.write(data, std::chrono::milliseconds{100}) };
    
    // Must be closeable
    { conn.close() } -> std::same_as<void>;
    
    // Must provide connection state
    { conn.is_open() } -> std::convertible_to<bool>;
};

// Listener Concept
template<typename L, typename FutureConn>
concept listener = requires(L lstn) {
    // Must support accepting connections
    { lstn.accept() } -> std::same_as<FutureConn>;
    { lstn.accept(std::chrono::milliseconds{100}) } -> std::same_as<FutureConn>;
    
    // Must be closeable
    { lstn.close() } -> std::same_as<void>;
    
    // Must provide listener state
    { lstn.is_listening() } -> std::convertible_to<bool>;
};

// Network Edge Concept
template<typename E>
concept network_edge = requires(E edge) {
    // Must provide latency
    { edge.latency() } -> std::same_as<std::chrono::milliseconds>;
    
    // Must provide reliability (0.0 to 1.0)
    { edge.reliability() } -> std::convertible_to<double>;
};

// Network Node Concept
template<typename N, typename Types>
concept network_node = requires(N node, typename Types::message_type msg, 
                                typename Types::address_type addr, typename Types::port_type port) {
    // Connectionless operations
    { node.send(msg) } -> std::same_as<typename Types::future_bool_type>;
    { node.send(msg, std::chrono::milliseconds{100}) } -> std::same_as<typename Types::future_bool_type>;
    { node.receive() } -> std::same_as<typename Types::future_message_type>;
    { node.receive(std::chrono::milliseconds{100}) } -> std::same_as<typename Types::future_message_type>;
    
    // Connection-oriented client operations
    { node.connect(addr, port) } -> std::same_as<typename Types::future_connection_type>;
    { node.connect(addr, port, port) } -> std::same_as<typename Types::future_connection_type>;  // with source port
    { node.connect(addr, port, std::chrono::milliseconds{100}) } -> std::same_as<typename Types::future_connection_type>;
    
    // Connection-oriented server operations
    { node.bind() } -> std::same_as<typename Types::future_listener_type>;  // bind to random port
    { node.bind(port) } -> std::same_as<typename Types::future_listener_type>;  // bind to specific port
    { node.bind(port, std::chrono::milliseconds{100}) } -> std::same_as<typename Types::future_listener_type>;
    
    // Node identity
    { node.address() } -> std::same_as<typename Types::address_type>;
};

// Network Simulator Concept
template<typename S, typename Types, typename Edge>
concept network_simulator = requires(S sim, typename Types::address_type addr, Edge edge) {
    // Topology configuration
    { sim.add_node(addr) } -> std::same_as<void>;
    { sim.remove_node(addr) } -> std::same_as<void>;
    { sim.add_edge(addr, addr, edge) } -> std::same_as<void>;
    { sim.remove_edge(addr, addr) } -> std::same_as<void>;
    
    // Node creation
    { sim.create_node(addr) } -> std::same_as<std::shared_ptr<typename Types::node_type>>;
    
    // Simulation control
    { sim.start() } -> std::same_as<void>;
    { sim.stop() } -> std::same_as<void>;
    { sim.reset() } -> std::same_as<void>;
    
    // Query methods
    { sim.has_node(addr) } -> std::convertible_to<bool>;
    { sim.has_edge(addr, addr) } -> std::convertible_to<bool>;
};

// Primary Network Simulator Types Concept
template<typename T>
concept network_simulator_types = requires {
    // Core types
    typename T::address_type;
    typename T::port_type;
    typename T::message_type;
    typename T::connection_type;
    typename T::listener_type;
    typename T::node_type;
    
    // Future types for specific operations
    typename T::future_bool_type;
    typename T::future_message_type;
    typename T::future_connection_type;
    typename T::future_listener_type;
    typename T::future_bytes_type;
    
    // Type constraints
    requires address<typename T::address_type>;
    requires port<typename T::port_type>;
    requires message<typename T::message_type, typename T::address_type, typename T::port_type>;
    requires connection<typename T::connection_type>;
    requires listener<typename T::listener_type, typename T::future_connection_type>;
    
    // Future constraints
    requires future<typename T::future_bool_type, bool>;
    requires future<typename T::future_message_type, typename T::message_type>;
    requires future<typename T::future_connection_type, std::shared_ptr<typename T::connection_type>>;
    requires future<typename T::future_listener_type, std::shared_ptr<typename T::listener_type>>;
    requires future<typename T::future_bytes_type, std::vector<std::byte>>;
    
    // Node constraint
    requires network_node<typename T::node_type, T>;
};

} // namespace network_simulator
