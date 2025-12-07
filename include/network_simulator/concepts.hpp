#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <ranges>
#include <tuple>
#include <vector>

namespace network_simulator {

// 1.1 Address Concept
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

// 1.2 Port Concept
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

// 1.3 Try Concept
template<typename T>
concept try_type = requires(T t) {
    // Must provide value access
    { t.value() };
    
    // Must provide exception access
    { t.exception() } -> std::same_as<std::exception_ptr>;
    
    // Must be able to check if contains value
    { t.has_value() } -> std::convertible_to<bool>;
    
    // Must be able to check if contains exception
    { t.has_exception() } -> std::convertible_to<bool>;
};

// 1.4 Future Concept
template<typename F, typename T>
concept future = requires(F f) {
    // Must provide value access (blocking)
    { f.get() } -> std::same_as<T>;
    
    // Must support then() for chaining
    { f.then([](T val) { return val; }) };
    
    // Must support error handling
    { f.onError([](std::exception_ptr) {}) };
    
    // Must be able to check if ready
    { f.isReady() } -> std::convertible_to<bool>;
    
    // Must support timeout
    { f.wait(std::chrono::milliseconds{100}) } -> std::convertible_to<bool>;
};

// 1.5 Message Concept
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
    { msg.payload() } -> std::same_as<const std::vector<std::byte>&>;
};

// 1.6 Connection Concept
template<typename C, typename T>
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

// 1.7 Listener Concept
template<typename L, typename Conn>
concept listener = requires(L lstn) {
    // Must support accepting connections
    { lstn.accept() };
    { lstn.accept(std::chrono::milliseconds{100}) };
    
    // Must be closeable
    { lstn.close() } -> std::same_as<void>;
    
    // Must provide listener state
    { lstn.is_listening() } -> std::convertible_to<bool>;
};

// 1.8 Endpoint Concept
template<typename E, typename Addr, typename Port>
concept endpoint = requires(E ep) {
    // Must provide address access
    { ep.address() } -> std::same_as<Addr>;
    
    // Must provide port access
    { ep.port() } -> std::same_as<Port>;
    
    // Must support equality comparison
    { ep == ep } -> std::convertible_to<bool>;
    { ep != ep } -> std::convertible_to<bool>;
};

// 1.9 Network Edge Concept
template<typename E>
concept network_edge = requires(E edge) {
    // Must provide latency
    { edge.latency() } -> std::same_as<std::chrono::milliseconds>;
    
    // Must provide reliability (0.0 to 1.0)
    { edge.reliability() } -> std::convertible_to<double>;
};

// 1.10 Network Node Concept
template<typename N, typename Addr, typename Port, typename Msg, typename Conn, typename Lstn>
concept network_node = requires(N node, Msg msg, Addr addr, Port prt) {
    // Connectionless operations
    { node.send(msg) };
    { node.send(msg, std::chrono::milliseconds{100}) };
    { node.receive() };
    { node.receive(std::chrono::milliseconds{100}) };
    
    // Connection-oriented client operations
    { node.connect(addr, prt) };
    { node.connect(addr, prt, prt) };  // with source port
    { node.connect(addr, prt, std::chrono::milliseconds{100}) };
    
    // Connection-oriented server operations
    { node.bind() };  // bind to random port
    { node.bind(prt) };  // bind to specific port
    { node.bind(prt, std::chrono::milliseconds{100}) };
    
    // Node identity
    { node.address() } -> std::same_as<Addr>;
};

// 1.11 Network Simulator Concept
template<typename S, typename Addr, typename Port, typename Node, typename Edge>
concept network_simulator = requires(S sim, Addr addr, Edge edge) {
    // Edge must satisfy network_edge concept
    requires network_edge<Edge>;
    
    // Topology configuration
    { sim.add_node(addr) } -> std::same_as<void>;
    { sim.remove_node(addr) } -> std::same_as<void>;
    { sim.add_edge(addr, addr, edge) } -> std::same_as<void>;
    { sim.remove_edge(addr, addr) } -> std::same_as<void>;
    
    // Node creation
    { sim.create_node(addr) } -> std::same_as<std::shared_ptr<Node>>;
    
    // Simulation control
    { sim.start() } -> std::same_as<void>;
    { sim.stop() } -> std::same_as<void>;
    { sim.reset() } -> std::same_as<void>;
};

} // namespace network_simulator
