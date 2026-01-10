#pragma once

#include "node.hpp"
#include "simulator.hpp"
#include "exceptions.hpp"

#include <atomic>
#include <random>
#include <sstream>

#ifdef FOLLY_FUTURES_AVAILABLE
#include <folly/futures/Future.h>
#endif

#ifdef FOLLY_FUTURES_AVAILABLE
#include <folly/futures/Future.h>
#endif

namespace network_simulator {

// Ephemeral port allocation implementation
template<typename Types>
auto NetworkNode<Types>::allocate_ephemeral_port() -> port_type {
    std::lock_guard<std::mutex> lock(_port_mutex);
    
    if constexpr (std::is_same_v<port_type, unsigned short>) {
        // For unsigned short ports, use standard ephemeral port range
        constexpr unsigned short ephemeral_start = 49152;
        constexpr unsigned short ephemeral_end = 65535;
        
        // Find next available port
        for (unsigned short port = ephemeral_start; port <= ephemeral_end; ++port) {
            if (_used_ports.find(port) == _used_ports.end()) {
                _used_ports.insert(port);
                return port;
            }
        }
        
        // If no ports available, throw exception
        throw PortInUseException("No ephemeral ports available");
        
    } else if constexpr (std::is_same_v<port_type, std::string>) {
        // For string ports, generate unique string identifiers
        static std::atomic<std::size_t> counter{0};
        std::string port;
        
        do {
            std::ostringstream oss;
            oss << "ephemeral_" << counter++;
            port = oss.str();
        } while (_used_ports.find(port) != _used_ports.end());
        
        _used_ports.insert(port);
        return port;
    } else {
        static_assert(std::is_same_v<port_type, unsigned short> || 
                     std::is_same_v<port_type, std::string>,
                     "Port type must be unsigned short or std::string");
    }
}

// Port release implementation
template<typename Types>
auto NetworkNode<Types>::release_port(port_type port) -> void {
    std::lock_guard<std::mutex> lock(_port_mutex);
    _used_ports.erase(port);
}

// Connectionless send operations
template<typename Types>
auto NetworkNode<Types>::send(message_type msg) -> future_bool_type {
    if (!_simulator) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<bool>(
            folly::exception_wrapper(std::runtime_error("Simulator not available")));
#else
        return future_bool_type(std::make_exception_ptr(
            std::runtime_error("Simulator not available")));
#endif
    }
    
    return _simulator->route_message(std::move(msg));
}

template<typename Types>
auto NetworkNode<Types>::send(message_type msg, std::chrono::milliseconds timeout) -> future_bool_type {
    // For now, implement timeout by delegating to the basic send
    // In a full implementation, this would use a timer
    auto future = send(std::move(msg));
    
#ifdef FOLLY_FUTURES_AVAILABLE
    // folly::Future doesn't have a simple wait() method, so we use get() with timeout
    try {
        auto result = std::move(future).get(timeout);
        return folly::makeFuture(result);
    } catch (const folly::FutureTimeout&) {
        return folly::makeFuture(false);
    }
#else
    // Check if timeout occurred (simplified implementation)
    if (!future.wait(timeout)) {
        return future_bool_type(false);
    }
    
    return future;
#endif
}

// Connectionless receive operations
template<typename Types>
auto NetworkNode<Types>::receive() -> future_message_type {
    if (!_simulator) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<message_type>(
            folly::exception_wrapper(std::runtime_error("Simulator not available")));
#else
        return future_message_type(std::make_exception_ptr(
            std::runtime_error("Simulator not available")));
#endif
    }
    
    return _simulator->retrieve_message(_address);
}

template<typename Types>
auto NetworkNode<Types>::receive(std::chrono::milliseconds timeout) -> future_message_type {
    if (!_simulator) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<message_type>(
            folly::exception_wrapper(TimeoutException()));
#else
        return future_message_type(std::make_exception_ptr(
            TimeoutException()));
#endif
    }
    
    return _simulator->retrieve_message(_address, timeout);
}

// Connection-oriented client operations
template<typename Types>
auto NetworkNode<Types>::connect(address_type dst_addr, port_type dst_port) -> future_connection_type {
    // Use ephemeral port allocation
    auto src_port = allocate_ephemeral_port();
    return connect(std::move(dst_addr), std::move(dst_port), std::move(src_port));
}

template<typename Types>
auto NetworkNode<Types>::connect(address_type dst_addr, port_type dst_port, port_type src_port) -> future_connection_type {
    if (!_simulator) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<connection_type>>(
            folly::exception_wrapper(std::runtime_error("Simulator not available")));
#else
        return future_connection_type(std::make_exception_ptr(
            std::runtime_error("Simulator not available")));
#endif
    }
    
    // Mark source port as used (thread-safe)
    {
        std::lock_guard<std::mutex> lock(_port_mutex);
        _used_ports.insert(src_port);
    }
    
    return _simulator->establish_connection(_address, std::move(src_port), 
                                          std::move(dst_addr), std::move(dst_port));
}

template<typename Types>
auto NetworkNode<Types>::connect(address_type dst_addr, port_type dst_port, 
                                std::chrono::milliseconds timeout) -> future_connection_type {
    if (!_simulator) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<connection_type>>(
            folly::exception_wrapper(std::runtime_error("Simulator not available")));
#else
        return future_connection_type(std::make_exception_ptr(
            std::runtime_error("Simulator not available")));
#endif
    }
    
    // Check if there's a route and get the latency
    try {
        auto edge = _simulator->get_edge(_address, dst_addr);
        auto connection_latency = edge.latency();
        
        // If the connection latency is greater than timeout, throw TimeoutException
        if (connection_latency > timeout) {
#ifdef FOLLY_FUTURES_AVAILABLE
            return folly::makeFuture<std::shared_ptr<connection_type>>(
                folly::exception_wrapper(TimeoutException()));
#else
            return future_connection_type(std::make_exception_ptr(TimeoutException()));
#endif
        }
    } catch (const NoRouteException&) {
        // No route exists, connection would timeout
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<connection_type>>(
            folly::exception_wrapper(TimeoutException()));
#else
        return future_connection_type(std::make_exception_ptr(TimeoutException()));
#endif
    }
    
    // Try to establish the connection
    auto future = connect(dst_addr, dst_port);
    
#ifdef FOLLY_FUTURES_AVAILABLE
    try {
        // Get the result with timeout
        auto result = std::move(future).get(timeout);
        return folly::makeFuture(std::move(result));
    } catch (const folly::FutureTimeout&) {
        return folly::makeFuture<std::shared_ptr<connection_type>>(
            folly::exception_wrapper(TimeoutException()));
    } catch (const std::exception& e) {
        // Convert connection failures to timeout exceptions for timeout version
        std::string error_msg = e.what();
        if (error_msg.find("Connection refused") != std::string::npos ||
            error_msg.find("No listener") != std::string::npos ||
            error_msg.find("not listening") != std::string::npos) {
            return folly::makeFuture<std::shared_ptr<connection_type>>(
                folly::exception_wrapper(TimeoutException()));
        }
        throw; // Re-throw other exceptions
    }
#else
    try {
        auto result = future.get();
        if (!result) {
            // Connection failed, treat as timeout
            return future_connection_type(std::make_exception_ptr(TimeoutException()));
        }
        return future_connection_type(result);
    } catch (const std::exception& e) {
        // Convert connection failures to timeout exceptions for timeout version
        std::string error_msg = e.what();
        if (error_msg.find("Connection refused") != std::string::npos ||
            error_msg.find("No listener") != std::string::npos ||
            error_msg.find("not listening") != std::string::npos) {
            return future_connection_type(std::make_exception_ptr(TimeoutException()));
        }
        throw; // Re-throw other exceptions
    }
#endif
}

// Connection-oriented server operations
template<typename Types>
auto NetworkNode<Types>::bind() -> future_listener_type {
    if (!_simulator) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<listener_type>>(
            folly::exception_wrapper(std::runtime_error("Simulator not available")));
#else
        return future_listener_type(std::make_exception_ptr(
            std::runtime_error("Simulator not available")));
#endif
    }
    
    return _simulator->create_listener(_address);
}

template<typename Types>
auto NetworkNode<Types>::bind(port_type port) -> future_listener_type {
    if (!_simulator) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<listener_type>>(
            folly::exception_wrapper(std::runtime_error("Simulator not available")));
#else
        return future_listener_type(std::make_exception_ptr(
            std::runtime_error("Simulator not available")));
#endif
    }
    
    // Mark port as used (thread-safe)
    {
        std::lock_guard<std::mutex> lock(_port_mutex);
        _used_ports.insert(port);
    }
    
    return _simulator->create_listener(_address, std::move(port));
}

template<typename Types>
auto NetworkNode<Types>::bind(port_type port, std::chrono::milliseconds timeout) -> future_listener_type {
    if (!_simulator) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<listener_type>>(
            folly::exception_wrapper(std::runtime_error("Simulator not available")));
#else
        return future_listener_type(std::make_exception_ptr(
            std::runtime_error("Simulator not available")));
#endif
    }
    
    // Mark port as used (thread-safe)
    {
        std::lock_guard<std::mutex> lock(_port_mutex);
        _used_ports.insert(port);
    }
    
    // Try to create the listener
    auto future = _simulator->create_listener(_address, port);
    
    // For timeout handling, we need to check if the bind operation would succeed
    // In this implementation, bind operations are synchronous, so we either succeed or fail immediately
    // The timeout is mainly for testing timeout exception behavior
    
#ifdef FOLLY_FUTURES_AVAILABLE
    try {
        // Get the result with timeout
        auto result = std::move(future).get(timeout);
        return folly::makeFuture(std::move(result));
    } catch (const folly::FutureTimeout&) {
        // Release the port since bind failed
        {
            std::lock_guard<std::mutex> lock(_port_mutex);
            _used_ports.erase(port);
        }
        return folly::makeFuture<std::shared_ptr<listener_type>>(
            folly::exception_wrapper(TimeoutException()));
    } catch (const std::exception&) {
        // Release the port since bind failed
        {
            std::lock_guard<std::mutex> lock(_port_mutex);
            _used_ports.erase(port);
        }
        throw; // Re-throw the original exception
    }
#else
    // For SimpleFuture, check if the operation would timeout
    // Since bind is synchronous in our implementation, we simulate timeout behavior
    // by checking if the port is already in use and treating that as a timeout scenario
    try {
        auto result = future.get();
        if (!result) {
            // Bind failed, release the port and throw timeout exception
            {
                std::lock_guard<std::mutex> lock(_port_mutex);
                _used_ports.erase(port);
            }
            return future_listener_type(std::make_exception_ptr(TimeoutException()));
        }
        return future_listener_type(result);
    } catch (const PortInUseException&) {
        // Port in use - for timeout version, convert to TimeoutException
        {
            std::lock_guard<std::mutex> lock(_port_mutex);
            _used_ports.erase(port);
        }
        return future_listener_type(std::make_exception_ptr(TimeoutException()));
    } catch (...) {
        // Other exceptions, release port and re-throw
        {
            std::lock_guard<std::mutex> lock(_port_mutex);
            _used_ports.erase(port);
        }
        throw;
    }
#endif
}

} // namespace network_simulator