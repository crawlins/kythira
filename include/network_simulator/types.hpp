#pragma once

#include "concepts.hpp"
#include "../raft/future.hpp"
#include <concepts/future.hpp>
#include <cstring>
#include <netinet/in.h>
#include <vector>

namespace network_simulator {

// Import Future and Try from kythira namespace
using kythira::Future;
using kythira::Try;
using kythira::wait_for_any;
using kythira::wait_for_all;

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

// Forward declarations
template<address Addr, port Port>
class Message;

template<address Addr, port Port, typename FutureType>

class NetworkNode;

} // namespace network_simulator

// Forward declarations for kythira namespace
namespace kythira {

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>
class Connection;

template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>
class Listener;

} // namespace kythira

namespace network_simulator {

// 2.1 Message Structure
template<address Addr, port Port>
class Message {
public:
    Message(Addr src_addr, Port src_port, 
            Addr dst_addr, Port dst_port,
            std::vector<std::byte> payload = {})
        : _source_address(std::move(src_addr))
        , _source_port(std::move(src_port))
        , _destination_address(std::move(dst_addr))
        , _destination_port(std::move(dst_port))
        , _payload(std::move(payload))
    {}
    
    auto source_address() const -> Addr { return _source_address; }
    auto source_port() const -> Port { return _source_port; }
    auto destination_address() const -> Addr { return _destination_address; }
    auto destination_port() const -> Port { return _destination_port; }
    auto payload() const -> const std::vector<std::byte>& { return _payload; }
    
private:
    Addr _source_address;
    Port _source_port;
    Addr _destination_address;
    Port _destination_port;
    std::vector<std::byte> _payload;
};

// 2.2 Network Edge
struct NetworkEdge {
    std::chrono::milliseconds _latency;
    double _reliability;  // 0.0 to 1.0
    
    NetworkEdge() : _latency(0), _reliability(1.0) {}
    
    NetworkEdge(std::chrono::milliseconds lat, double rel)
        : _latency(lat), _reliability(rel) {}
    
    auto latency() const -> std::chrono::milliseconds { return _latency; }
    auto reliability() const -> double { return _reliability; }
};

// 2.3 Endpoint
template<address Addr, port Port>
struct Endpoint {
    Addr _address;
    Port _port;
    
    Endpoint(Addr addr, Port prt)
        : _address(std::move(addr)), _port(std::move(prt)) {}
    
    auto address() const -> Addr { return _address; }
    auto port() const -> Port { return _port; }
    
    auto operator==(const Endpoint& other) const -> bool {
        return _address == other._address && _port == other._port;
    }
    
    auto operator!=(const Endpoint& other) const -> bool {
        return !(*this == other);
    }
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
template<network_simulator::address Addr, network_simulator::port Port>
struct std::hash<network_simulator::Endpoint<Addr, Port>> {
    auto operator()(const network_simulator::Endpoint<Addr, Port>& ep) const -> std::size_t {
        std::size_t h1 = std::hash<Addr>{}(ep._address);
        std::size_t h2 = std::hash<Port>{}(ep._port);
        return h1 ^ (h2 << 1);
    }
};
