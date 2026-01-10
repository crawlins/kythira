#pragma once

#include "concepts.hpp"
#include <chrono>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <vector>

// Include kythira future wrapper which provides the correct API
#include "../raft/future.hpp"
#define KYTHIRA_FUTURES_AVAILABLE 1

namespace network_simulator {

// Wrapper for in_addr to satisfy address concept
struct IPv4Address {
    in_addr _addr;
    
    IPv4Address() : _addr{} {}
    explicit IPv4Address(in_addr addr) : _addr(addr) {}
    
    auto operator==(const IPv4Address& other) const -> bool {
        return _addr.s_addr == other._addr.s_addr;
    }
    
    auto operator!=(const IPv4Address& other) const -> bool {
        return !(*this == other);
    }
    
    auto get() const -> const in_addr& { return _addr; }
};

// Wrapper for in6_addr to satisfy address concept
struct IPv6Address {
    in6_addr _addr;
    
    IPv6Address() : _addr{} {}
    explicit IPv6Address(in6_addr addr) : _addr(addr) {}
    
    auto operator==(const IPv6Address& other) const -> bool {
        return std::memcmp(&_addr, &other._addr, sizeof(in6_addr)) == 0;
    }
    
    auto operator!=(const IPv6Address& other) const -> bool {
        return !(*this == other);
    }
    
    auto get() const -> const in6_addr& { return _addr; }
};

// Simple Future implementation for when kythira is not available
template<typename T>
class SimpleFuture {
public:
    SimpleFuture() = default;
    explicit SimpleFuture(T value) : _value(std::move(value)), _ready(true) {}
    explicit SimpleFuture(std::exception_ptr ex) : _exception(ex), _ready(true) {}
    
    auto get() -> T {
        if (_exception) {
            std::rethrow_exception(_exception);
        }
        return _value;
    }
    
    template<typename F>
    auto then(F&& func) -> SimpleFuture<std::invoke_result_t<F, T>> {
        if (_exception) {
            return SimpleFuture<std::invoke_result_t<F, T>>(_exception);
        }
        try {
            return SimpleFuture<std::invoke_result_t<F, T>>(func(_value));
        } catch (...) {
            return SimpleFuture<std::invoke_result_t<F, T>>(std::current_exception());
        }
    }
    
    template<typename F>
    auto onError(F&& func) -> SimpleFuture<T> {
        if (_exception) {
            try {
                func(_exception);
            } catch (...) {
                // Ignore errors in error handler
            }
        }
        return *this;
    }
    
    auto isReady() const -> bool { return _ready; }
    
    auto wait(std::chrono::milliseconds timeout) -> bool {
        // Simple implementation - always ready for now
        return _ready;
    }
    
private:
    T _value{};
    std::exception_ptr _exception;
    bool _ready = false;
};

// Specialization for void
template<>
class SimpleFuture<void> {
public:
    SimpleFuture() : _ready(true) {}
    explicit SimpleFuture(std::exception_ptr ex) : _exception(ex), _ready(true) {}
    
    auto get() -> void {
        if (_exception) {
            std::rethrow_exception(_exception);
        }
    }
    
    template<typename F>
    auto then(F&& func) -> SimpleFuture<std::invoke_result_t<F>> {
        if (_exception) {
            return SimpleFuture<std::invoke_result_t<F>>(_exception);
        }
        try {
            if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
                func();
                return SimpleFuture<void>();
            } else {
                return SimpleFuture<std::invoke_result_t<F>>(func());
            }
        } catch (...) {
            return SimpleFuture<std::invoke_result_t<F>>(std::current_exception());
        }
    }
    
    template<typename F>
    auto onError(F&& func) -> SimpleFuture<void> {
        if (_exception) {
            try {
                func(_exception);
            } catch (...) {
                // Ignore errors in error handler
            }
        }
        return *this;
    }
    
    auto isReady() const -> bool { return _ready; }
    
    auto wait(std::chrono::milliseconds timeout) -> bool {
        // Simple implementation - always ready for now
        return _ready;
    }
    
private:
    std::exception_ptr _exception;
    bool _ready = false;
};

// Forward declarations for template classes that will be defined later
template<typename Types>
class Message;

template<typename Types>
class NetworkNode;

template<typename Types>
class Connection;

template<typename Types>
class Listener;

template<typename Types>
class NetworkSimulator;

// Message Structure
template<typename Types>
class Message {
public:
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    
    // Default constructor for empty messages
    Message() = default;
    
    Message(address_type src_addr, port_type src_port, 
            address_type dst_addr, port_type dst_port,
            std::vector<std::byte> payload = {})
        : _source_address(std::move(src_addr))
        , _source_port(std::move(src_port))
        , _destination_address(std::move(dst_addr))
        , _destination_port(std::move(dst_port))
        , _payload(std::move(payload))
    {}
    
    auto source_address() const -> address_type { return _source_address; }
    auto source_port() const -> port_type { return _source_port; }
    auto destination_address() const -> address_type { return _destination_address; }
    auto destination_port() const -> port_type { return _destination_port; }
    auto payload() const -> std::vector<std::byte> { return _payload; }
    
private:
    address_type _source_address;
    port_type _source_port;
    address_type _destination_address;
    port_type _destination_port;
    std::vector<std::byte> _payload;
};

// Network Edge
struct NetworkEdge {
    std::chrono::milliseconds _latency;
    double _reliability;  // 0.0 to 1.0
    
    NetworkEdge() : _latency(0), _reliability(1.0) {}
    
    NetworkEdge(std::chrono::milliseconds lat, double rel)
        : _latency(lat), _reliability(rel) {}
    
    auto latency() const -> std::chrono::milliseconds { return _latency; }
    auto reliability() const -> double { return _reliability; }
};

// Endpoint
template<typename Types>
struct Endpoint {
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    
    address_type address;
    port_type port;
    
    Endpoint(address_type addr, port_type prt)
        : address(std::move(addr)), port(std::move(prt)) {}
    
    auto operator==(const Endpoint&) const -> bool = default;
};

// Connection ID using 4-tuple (src_addr, src_port, dst_addr, dst_port)
template<typename Types>
struct ConnectionId {
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    
    address_type src_addr;
    port_type src_port;
    address_type dst_addr;
    port_type dst_port;
    
    ConnectionId(address_type src_a, port_type src_p, address_type dst_a, port_type dst_p)
        : src_addr(std::move(src_a)), src_port(std::move(src_p))
        , dst_addr(std::move(dst_a)), dst_port(std::move(dst_p)) {}
    
    auto operator==(const ConnectionId&) const -> bool = default;
};

// Default Types Implementation using folly::Future when available, SimpleFuture otherwise
struct DefaultNetworkTypes {
    // Core types
    using address_type = std::string;
    using port_type = unsigned short;
    using message_type = Message<DefaultNetworkTypes>;
    using connection_type = Connection<DefaultNetworkTypes>;
    using listener_type = Listener<DefaultNetworkTypes>;
    using node_type = NetworkNode<DefaultNetworkTypes>;
    
    // Future types - use kythira::Future since we included it
#ifdef KYTHIRA_FUTURES_AVAILABLE
    using future_bool_type = kythira::Future<bool>;
    using future_message_type = kythira::Future<message_type>;
    using future_connection_type = kythira::Future<std::shared_ptr<connection_type>>;
    using future_listener_type = kythira::Future<std::shared_ptr<listener_type>>;
    using future_bytes_type = kythira::Future<std::vector<std::byte>>;
#else
    using future_bool_type = SimpleFuture<bool>;
    using future_message_type = SimpleFuture<message_type>;
    using future_connection_type = SimpleFuture<std::shared_ptr<connection_type>>;
    using future_listener_type = SimpleFuture<std::shared_ptr<listener_type>>;
    using future_bytes_type = SimpleFuture<std::vector<std::byte>>;
#endif
};

} // namespace network_simulator

// Hash specialization for IPv4Address
template<>
struct std::hash<network_simulator::IPv4Address> {
    auto operator()(const network_simulator::IPv4Address& addr) const -> std::size_t {
        return std::hash<uint32_t>{}(addr._addr.s_addr);
    }
};

// Hash specialization for IPv6Address
template<>
struct std::hash<network_simulator::IPv6Address> {
    auto operator()(const network_simulator::IPv6Address& addr) const -> std::size_t {
        std::size_t hash = 0;
        for (std::size_t i = 0; i < sizeof(in6_addr); ++i) {
            hash ^= std::hash<unsigned char>{}(reinterpret_cast<const unsigned char*>(&addr._addr)[i]) << (i % 8);
        }
        return hash;
    }
};

// Hash specialization for Endpoint
template<typename Types>
struct std::hash<network_simulator::Endpoint<Types>> {
    auto operator()(const network_simulator::Endpoint<Types>& ep) const -> std::size_t {
        std::size_t h1 = std::hash<typename Types::address_type>{}(ep.address);
        std::size_t h2 = std::hash<typename Types::port_type>{}(ep.port);
        return h1 ^ (h2 << 1);
    }
};

// Hash specialization for ConnectionId
template<typename Types>
struct std::hash<network_simulator::ConnectionId<Types>> {
    auto operator()(const network_simulator::ConnectionId<Types>& conn_id) const -> std::size_t {
        std::size_t h1 = std::hash<typename Types::address_type>{}(conn_id.src_addr);
        std::size_t h2 = std::hash<typename Types::port_type>{}(conn_id.src_port);
        std::size_t h3 = std::hash<typename Types::address_type>{}(conn_id.dst_addr);
        std::size_t h4 = std::hash<typename Types::port_type>{}(conn_id.dst_port);
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
    }
};
