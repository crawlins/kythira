#pragma once

#include "concepts.hpp"
#include "types.hpp"
#include "connection.hpp"
#include "exceptions.hpp"
#include <concepts/future.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include <raft/future.hpp>

namespace kythira {

// Forward declaration
template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>
class NetworkSimulator;

// Listener class - implements server-side connection acceptance
template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>
class Listener {
public:
    Listener(network_simulator::Endpoint<Addr, Port> local_endpoint,
             NetworkSimulator<Addr, Port, FutureType>* simulator)
        : _local(std::move(local_endpoint))
        , _simulator(simulator)
        , _listening(true)
    {}
    
    // Accept operations
    auto accept() -> FutureType {
        // Check if listener is still listening
        if (!_listening) {
            return FutureType(std::make_exception_ptr(network_simulator::ConnectionClosedException()));
        }
        
        // Check if connection is immediately available
        {
            std::unique_lock<std::mutex> lock(_queue_mutex);
            if (!_pending_connections.empty()) {
                auto connection = std::move(_pending_connections.front());
                _pending_connections.pop();
                return FutureType(std::move(connection));
            }
        }
        
        // No connection available - create a promise and wait
        auto promise = std::make_shared<folly::Promise<std::shared_ptr<Connection<Addr, Port, FutureType>>>>();
        auto future = promise->getFuture();
        
        // Start a background task to wait for connections
        std::thread([this, promise]() {
            std::unique_lock<std::mutex> lock(_queue_mutex);
            _connection_available.wait(lock, [this] { 
                return !_pending_connections.empty() || !_listening; 
            });
            
            if (!_listening) {
                promise->setException(network_simulator::ConnectionClosedException());
                return;
            }
            
            if (!_pending_connections.empty()) {
                auto connection = std::move(_pending_connections.front());
                _pending_connections.pop();
                promise->setValue(std::move(connection));
            } else {
                promise->setException(network_simulator::ConnectionClosedException());
            }
        }).detach();
        
        return FutureType(std::move(future));
    }
    
    auto accept(std::chrono::milliseconds timeout) -> FutureType {
        // Check if listener is still listening
        if (!_listening) {
            return FutureType(std::make_exception_ptr(network_simulator::ConnectionClosedException()));
        }
        
        // Check if connection is immediately available
        {
            std::unique_lock<std::mutex> lock(_queue_mutex);
            if (!_pending_connections.empty()) {
                auto connection = std::move(_pending_connections.front());
                _pending_connections.pop();
                return FutureType(std::move(connection));
            }
        }
        
        // No connection available - wait with timeout
        auto promise = std::make_shared<folly::Promise<std::shared_ptr<Connection<Addr, Port, FutureType>>>>();
        auto future = promise->getFuture();
        
        // Start a background task to wait for connections with timeout
        std::thread([this, promise, timeout]() {
            std::unique_lock<std::mutex> lock(_queue_mutex);
            bool connection_arrived = _connection_available.wait_for(lock, timeout, [this] { 
                return !_pending_connections.empty() || !_listening; 
            });
            
            if (!_listening) {
                promise->setException(network_simulator::ConnectionClosedException());
                return;
            }
            
            if (connection_arrived && !_pending_connections.empty()) {
                auto connection = std::move(_pending_connections.front());
                _pending_connections.pop();
                promise->setValue(std::move(connection));
            } else {
                // Timeout occurred
                promise->setException(network_simulator::TimeoutException());
            }
        }).detach();
        
        return FutureType(std::move(future));
    }
    
    // Listener control
    auto close() -> void {
        _listening = false;
        
        // Wake up any blocked accept operations
        {
            std::unique_lock<std::mutex> lock(_queue_mutex);
            _connection_available.notify_all();
        }
    }
    
    auto is_listening() const -> bool {
        return _listening;
    }
    
    // Endpoint accessor
    auto local_endpoint() const -> network_simulator::Endpoint<Addr, Port> { return _local; }
    
    // Internal method for simulator to queue pending connections
    auto queue_pending_connection(std::shared_ptr<Connection<Addr, Port, FutureType>> connection) -> void {
        std::unique_lock<std::mutex> lock(_queue_mutex);
        _pending_connections.push(std::move(connection));
        _connection_available.notify_one();
    }
    
private:
    network_simulator::Endpoint<Addr, Port> _local;
    NetworkSimulator<Addr, Port, FutureType>* _simulator;
    
    std::atomic<bool> _listening;
    
    // Queue of pending connections
    std::queue<std::shared_ptr<Connection<Addr, Port, FutureType>>> _pending_connections;
    mutable std::mutex _queue_mutex;
    std::condition_variable _connection_available;
};

} // namespace kythira