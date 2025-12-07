#pragma once

#include "concepts.hpp"
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

#include <folly/futures/Future.h>
#include <folly/Try.h>

namespace network_simulator {

// 2.4 Try wrapper that adapts folly::Try to satisfy try_type concept
template<typename T>
class Try {
public:
    // Constructors
    Try() = default;
    explicit Try(folly::Try<T> ft) : _folly_try(std::move(ft)) {}
    
    // Construct from value
    explicit Try(T value) : _folly_try(std::move(value)) {}
    
    // Construct from exception
    explicit Try(folly::exception_wrapper ex) : _folly_try(std::move(ex)) {}
    
    // Access value (throws if contains exception)
    auto value() -> T& {
        return _folly_try.value();
    }
    
    auto value() const -> const T& {
        return _folly_try.value();
    }
    
    // Access exception - convert folly::exception_wrapper to std::exception_ptr
    auto exception() const -> std::exception_ptr {
        if (_folly_try.hasException()) {
            return _folly_try.exception().to_exception_ptr();
        }
        return nullptr;
    }
    
    // Check if contains value
    auto has_value() const -> bool {
        return _folly_try.hasValue();
    }
    
    // Check if contains exception
    auto has_exception() const -> bool {
        return _folly_try.hasException();
    }
    
    // Get underlying folly::Try
    auto get_folly_try() const -> const folly::Try<T>& {
        return _folly_try;
    }
    
    auto get_folly_try() -> folly::Try<T>& {
        return _folly_try;
    }

private:
    folly::Try<T> _folly_try;
};

// 2.5 Future wrapper that adapts folly::Future to satisfy future concept
template<typename T>
class Future {
public:
    // Constructors
    Future() = default;
    explicit Future(folly::Future<T> ff) : _folly_future(std::move(ff)) {}
    
    // Construct from value
    explicit Future(T value) : _folly_future(folly::makeFuture<T>(std::move(value))) {}
    
    // Construct from exception
    explicit Future(folly::exception_wrapper ex) : _folly_future(folly::makeFuture<T>(std::move(ex))) {}
    
    // Get value (blocking)
    auto get() -> T {
        return std::move(_folly_future).get();
    }
    
    // Chain continuation
    template<typename F>
    auto then(F&& func) -> Future<std::invoke_result_t<F, T>> {
        using ReturnType = std::invoke_result_t<F, T>;
        return Future<ReturnType>(std::move(_folly_future).thenValue(std::forward<F>(func)));
    }
    
    // Error handling
    template<typename F>
    auto onError(F&& func) -> Future<T> {
        return Future<T>(std::move(_folly_future).thenError(std::forward<F>(func)));
    }
    
    // Check if ready
    auto isReady() const -> bool {
        return _folly_future.isReady();
    }
    
    // Wait with timeout
    auto wait(std::chrono::milliseconds timeout) -> bool {
        return _folly_future.wait(timeout).isReady();
    }
    
    // Get underlying folly::Future
    auto get_folly_future() && -> folly::Future<T> {
        return std::move(_folly_future);
    }

private:
    folly::Future<T> _folly_future;
};

// 2.6 Collective future operations

// Wait for any future to complete (modeled after folly::collectAny)
// Returns a future of tuple containing the index and Try<T> of the first completed future
template<typename T>
auto wait_for_any(std::vector<Future<T>> futures) -> Future<std::tuple<std::size_t, Try<T>>> {
    // Convert our Future wrappers to folly::Future
    std::vector<folly::Future<T>> folly_futures;
    folly_futures.reserve(futures.size());
    for (auto& fut : futures) {
        folly_futures.push_back(std::move(fut).get_folly_future());
    }
    
    // Use folly::collectAny - returns SemiFuture, convert to Future
    auto result_future = folly::collectAny(folly_futures.begin(), folly_futures.end())
        .toUnsafeFuture()
        .thenValue([](std::pair<std::size_t, folly::Try<T>> result) {
            return std::make_tuple(result.first, Try<T>(std::move(result.second)));
        });
    
    return Future<std::tuple<std::size_t, Try<T>>>(std::move(result_future));
}

// Wait for all futures to complete (modeled after folly::collectAll)
// Returns a future of vector containing Try<T> for each future (preserving order)
template<typename T>
auto wait_for_all(std::vector<Future<T>> futures) -> Future<std::vector<Try<T>>> {
    // Convert our Future wrappers to folly::Future
    std::vector<folly::Future<T>> folly_futures;
    folly_futures.reserve(futures.size());
    for (auto& fut : futures) {
        folly_futures.push_back(std::move(fut).get_folly_future());
    }
    
    // Use folly::collectAll - returns SemiFuture, convert to Future
    auto result_future = folly::collectAll(folly_futures.begin(), folly_futures.end())
        .toUnsafeFuture()
        .thenValue([](std::vector<folly::Try<T>> results) {
            std::vector<Try<T>> wrapped_results;
            wrapped_results.reserve(results.size());
            for (auto& result : results) {
                wrapped_results.push_back(Try<T>(std::move(result)));
            }
            return wrapped_results;
        });
    
    return Future<std::vector<Try<T>>>(std::move(result_future));
}

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

template<address Addr, port Port>
class NetworkSimulator;

template<address Addr, port Port>
class NetworkNode;

template<address Addr, port Port>
class Connection;

template<address Addr, port Port>
class Listener;

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
