#pragma once

#include "concepts.hpp"
#include "types.hpp"
#include "connection.hpp"
#include "exceptions.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include <folly/futures/Future.h>

namespace network_simulator {

// Forward declaration
template<address Addr, port Port>
class NetworkSimulator;

// Listener class - implements server-side connection acceptance
template<address Addr, port Port>
class Listener {
public:
    Listener(Endpoint<Addr, Port> local_endpoint,
             NetworkSimulator<Addr, Port>* simulator)
        : _local(std::move(local_endpoint))
        , _simulator(simulator)
        , _listening(true)
    {}
    
    // Accept operations
    auto accept() -> folly::Future<std::shared_ptr<Connection<Addr, Port>>>;
    auto accept(std::chrono::milliseconds timeout) -> folly::Future<std::shared_ptr<Connection<Addr, Port>>>;
    
    // Listener control
    auto close() -> void;
    auto is_listening() const -> bool;
    
    // Endpoint accessor
    auto local_endpoint() const -> Endpoint<Addr, Port> { return _local; }
    
    // Internal method for simulator to queue pending connections
    auto queue_pending_connection(std::shared_ptr<Connection<Addr, Port>> connection) -> void;
    
private:
    Endpoint<Addr, Port> _local;
    NetworkSimulator<Addr, Port>* _simulator;
    
    std::atomic<bool> _listening;
    
    // Queue of pending connections
    std::queue<std::shared_ptr<Connection<Addr, Port>>> _pending_connections;
    mutable std::mutex _queue_mutex;
    std::condition_variable _connection_available;
};

// Listener implementation

template<address Addr, port Port>
auto Listener<Addr, Port>::accept() -> folly::Future<std::shared_ptr<Connection<Addr, Port>>> {
    // Check if listener is still listening
    if (!_listening) {
        return folly::makeFuture<std::shared_ptr<Connection<Addr, Port>>>(
            folly::exception_wrapper(ConnectionClosedException()));
    }
    
    // Check if connection is immediately available
    {
        std::unique_lock lock(_queue_mutex);
        if (!_pending_connections.empty()) {
            auto connection = std::move(_pending_connections.front());
            _pending_connections.pop();
            return folly::makeFuture(std::move(connection));
        }
    }
    
    // No connection available - create a promise and wait
    auto promise = std::make_shared<folly::Promise<std::shared_ptr<Connection<Addr, Port>>>>();
    auto future = promise->getFuture();
    
    // Start a background task to wait for connections
    std::thread([this, promise]() {
        std::unique_lock lock(_queue_mutex);
        _connection_available.wait(lock, [this] { 
            return !_pending_connections.empty() || !_listening; 
        });
        
        if (!_listening) {
            promise->setException(ConnectionClosedException());
            return;
        }
        
        if (!_pending_connections.empty()) {
            auto connection = std::move(_pending_connections.front());
            _pending_connections.pop();
            promise->setValue(std::move(connection));
        } else {
            promise->setException(ConnectionClosedException());
        }
    }).detach();
    
    return future;
}

template<address Addr, port Port>
auto Listener<Addr, Port>::accept(std::chrono::milliseconds timeout) -> folly::Future<std::shared_ptr<Connection<Addr, Port>>> {
    // Check if listener is still listening
    if (!_listening) {
        return folly::makeFuture<std::shared_ptr<Connection<Addr, Port>>>(
            folly::exception_wrapper(ConnectionClosedException()));
    }
    
    // Check if connection is immediately available
    {
        std::unique_lock lock(_queue_mutex);
        if (!_pending_connections.empty()) {
            auto connection = std::move(_pending_connections.front());
            _pending_connections.pop();
            return folly::makeFuture(std::move(connection));
        }
    }
    
    // No connection available - wait with timeout
    auto promise = std::make_shared<folly::Promise<std::shared_ptr<Connection<Addr, Port>>>>();
    auto future = promise->getFuture();
    
    // Start a background task to wait for connections with timeout
    std::thread([this, promise, timeout]() {
        std::unique_lock lock(_queue_mutex);
        bool connection_arrived = _connection_available.wait_for(lock, timeout, [this] { 
            return !_pending_connections.empty() || !_listening; 
        });
        
        if (!_listening) {
            promise->setException(ConnectionClosedException());
            return;
        }
        
        if (connection_arrived && !_pending_connections.empty()) {
            auto connection = std::move(_pending_connections.front());
            _pending_connections.pop();
            promise->setValue(std::move(connection));
        } else {
            // Timeout occurred
            promise->setException(TimeoutException());
        }
    }).detach();
    
    return future;
}

template<address Addr, port Port>
auto Listener<Addr, Port>::close() -> void {
    _listening = false;
    
    // Wake up any blocked accept operations
    {
        std::unique_lock lock(_queue_mutex);
        _connection_available.notify_all();
    }
}

template<address Addr, port Port>
auto Listener<Addr, Port>::is_listening() const -> bool {
    return _listening;
}

template<address Addr, port Port>
auto Listener<Addr, Port>::queue_pending_connection(std::shared_ptr<Connection<Addr, Port>> connection) -> void {
    std::unique_lock lock(_queue_mutex);
    _pending_connections.push(std::move(connection));
    _connection_available.notify_one();
}

} // namespace network_simulator
