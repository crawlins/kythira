#pragma once

#include "concepts.hpp"
#include "types.hpp"
#include "exceptions.hpp"
#include <concepts/future.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <memory>

#include <raft/future.hpp>

namespace kythira {

// Forward declaration
template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>
class NetworkSimulator;

// Connection class - implements connection-oriented communication
template<network_simulator::address Addr, network_simulator::port Port, typename FutureType>
class Connection {
public:
    Connection(network_simulator::Endpoint<Addr, Port> local, 
               network_simulator::Endpoint<Addr, Port> remote,
               NetworkSimulator<Addr, Port, FutureType>* simulator)
        : _local(std::move(local))
        , _remote(std::move(remote))
        , _simulator(simulator)
        , _open(true)
    {}
    
    // Read operations
    auto read() -> FutureType {
        // Check if connection is open
        if (!_open) {
            return FutureType(std::make_exception_ptr(network_simulator::ConnectionClosedException()));
        }
        
        // Check if data is immediately available
        {
            std::unique_lock<std::mutex> lock(_buffer_mutex);
            if (!_read_buffer.empty()) {
                auto data = std::move(_read_buffer.front());
                _read_buffer.pop();
                return FutureType(std::move(data));
            }
        }
        
        // No data available - create a promise and wait
        auto promise = std::make_shared<folly::Promise<std::vector<std::byte>>>();
        auto future = promise->getFuture();
        
        // Start a background task to wait for data
        std::thread([this, promise]() {
            std::unique_lock<std::mutex> lock(_buffer_mutex);
            _data_available.wait(lock, [this] { 
                return !_read_buffer.empty() || !_open; 
            });
            
            if (!_open) {
                promise->setException(network_simulator::ConnectionClosedException());
                return;
            }
            
            if (!_read_buffer.empty()) {
                auto data = std::move(_read_buffer.front());
                _read_buffer.pop();
                promise->setValue(std::move(data));
            } else {
                promise->setException(network_simulator::ConnectionClosedException());
            }
        }).detach();
        
        return FutureType(std::move(future));
    }
    
    auto read(std::chrono::milliseconds timeout) -> FutureType {
        // Check if connection is open
        if (!_open) {
            return FutureType(std::make_exception_ptr(network_simulator::ConnectionClosedException()));
        }
        
        // Check if data is immediately available
        {
            std::unique_lock<std::mutex> lock(_buffer_mutex);
            if (!_read_buffer.empty()) {
                auto data = std::move(_read_buffer.front());
                _read_buffer.pop();
                return FutureType(std::move(data));
            }
        }
        
        // No data available - wait with timeout
        auto promise = std::make_shared<folly::Promise<std::vector<std::byte>>>();
        auto future = promise->getFuture();
        
        // Start a background task to wait for data with timeout
        std::thread([this, promise, timeout]() {
            std::unique_lock<std::mutex> lock(_buffer_mutex);
            bool data_arrived = _data_available.wait_for(lock, timeout, [this] { 
                return !_read_buffer.empty() || !_open; 
            });
            
            if (!_open) {
                promise->setException(network_simulator::ConnectionClosedException());
                return;
            }
            
            if (data_arrived && !_read_buffer.empty()) {
                auto data = std::move(_read_buffer.front());
                _read_buffer.pop();
                promise->setValue(std::move(data));
            } else {
                // Timeout occurred
                promise->setException(network_simulator::TimeoutException());
            }
        }).detach();
        
        return FutureType(std::move(future));
    }
    
    // Write operations
    auto write(std::vector<std::byte> data) -> FutureType {
        // Check if connection is open
        if (!_open) {
            return FutureType(std::make_exception_ptr(network_simulator::ConnectionClosedException()));
        }
        
        // Create a message with the data as payload
        network_simulator::Message<Addr, Port> msg(
            _local.address(),
            _local.port(),
            _remote.address(),
            _remote.port(),
            std::move(data)
        );
        
        // Route the message through the simulator
        return _simulator->route_message(std::move(msg));
    }
    
    auto write(std::vector<std::byte> data, std::chrono::milliseconds timeout) -> FutureType {
        // Check if connection is open
        if (!_open) {
            return FutureType(std::make_exception_ptr(network_simulator::ConnectionClosedException()));
        }
        
        // Create a message with the data as payload
        network_simulator::Message<Addr, Port> msg(
            _local.address(),
            _local.port(),
            _remote.address(),
            _remote.port(),
            std::move(data)
        );
        
        // Route the message through the simulator with timeout
        auto future = _simulator->route_message(std::move(msg));
        // Note: Timeout handling would need to be implemented in the future wrapper
        return future;
    }
    
    // Connection control
    auto close() -> void {
        _open = false;
        
        // Wake up any blocked read operations
        {
            std::unique_lock<std::mutex> lock(_buffer_mutex);
            _data_available.notify_all();
        }
    }
    
    auto is_open() const -> bool {
        return _open;
    }
    
    // Endpoint accessors
    auto local_endpoint() const -> network_simulator::Endpoint<Addr, Port> { return _local; }
    auto remote_endpoint() const -> network_simulator::Endpoint<Addr, Port> { return _remote; }
    
    // Internal method for simulator to deliver data
    auto deliver_data(std::vector<std::byte> data) -> void {
        std::unique_lock<std::mutex> lock(_buffer_mutex);
        _read_buffer.push(std::move(data));
        _data_available.notify_one();
    }
    
private:
    network_simulator::Endpoint<Addr, Port> _local;
    network_simulator::Endpoint<Addr, Port> _remote;
    NetworkSimulator<Addr, Port, FutureType>* _simulator;
    
    std::atomic<bool> _open;
    
    // Buffered data for this connection
    std::queue<std::vector<std::byte>> _read_buffer;
    mutable std::mutex _buffer_mutex;
    std::condition_variable _data_available;
};

} // namespace kythira