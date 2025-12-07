#pragma once

#include "concepts.hpp"
#include "types.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <folly/futures/Future.h>

namespace network_simulator {

// Forward declaration
template<address Addr, port Port>
class NetworkSimulator;

// Connection class - implements connection-oriented communication
template<address Addr, port Port>
class Connection {
public:
    Connection(Endpoint<Addr, Port> local, 
               Endpoint<Addr, Port> remote,
               NetworkSimulator<Addr, Port>* simulator)
        : _local(std::move(local))
        , _remote(std::move(remote))
        , _simulator(simulator)
        , _open(true)
    {}
    
    // Read operations
    auto read() -> folly::Future<std::vector<std::byte>>;
    auto read(std::chrono::milliseconds timeout) -> folly::Future<std::vector<std::byte>>;
    
    // Write operations
    auto write(std::vector<std::byte> data) -> folly::Future<bool>;
    auto write(std::vector<std::byte> data, std::chrono::milliseconds timeout) -> folly::Future<bool>;
    
    // Connection control
    auto close() -> void;
    auto is_open() const -> bool;
    
    // Endpoint accessors
    auto local_endpoint() const -> Endpoint<Addr, Port> { return _local; }
    auto remote_endpoint() const -> Endpoint<Addr, Port> { return _remote; }
    
    // Internal method for simulator to deliver data
    auto deliver_data(std::vector<std::byte> data) -> void;
    
private:
    Endpoint<Addr, Port> _local;
    Endpoint<Addr, Port> _remote;
    NetworkSimulator<Addr, Port>* _simulator;
    
    std::atomic<bool> _open;
    
    // Buffered data for this connection
    std::queue<std::vector<std::byte>> _read_buffer;
    mutable std::mutex _buffer_mutex;
    std::condition_variable _data_available;
};

// Connection implementation

template<address Addr, port Port>
auto Connection<Addr, Port>::read() -> folly::Future<std::vector<std::byte>> {
    // Check if connection is open
    if (!_open) {
        return folly::makeFuture<std::vector<std::byte>>(
            folly::exception_wrapper(ConnectionClosedException()));
    }
    
    // Check if data is immediately available
    {
        std::unique_lock lock(_buffer_mutex);
        if (!_read_buffer.empty()) {
            auto data = std::move(_read_buffer.front());
            _read_buffer.pop();
            return folly::makeFuture(std::move(data));
        }
    }
    
    // No data available - create a promise and wait
    auto promise = std::make_shared<folly::Promise<std::vector<std::byte>>>();
    auto future = promise->getFuture();
    
    // Start a background task to wait for data
    std::thread([this, promise]() {
        std::unique_lock lock(_buffer_mutex);
        _data_available.wait(lock, [this] { 
            return !_read_buffer.empty() || !_open; 
        });
        
        if (!_open) {
            promise->setException(ConnectionClosedException());
            return;
        }
        
        if (!_read_buffer.empty()) {
            auto data = std::move(_read_buffer.front());
            _read_buffer.pop();
            promise->setValue(std::move(data));
        } else {
            promise->setException(ConnectionClosedException());
        }
    }).detach();
    
    return future;
}

template<address Addr, port Port>
auto Connection<Addr, Port>::read(std::chrono::milliseconds timeout) -> folly::Future<std::vector<std::byte>> {
    // Check if connection is open
    if (!_open) {
        return folly::makeFuture<std::vector<std::byte>>(
            folly::exception_wrapper(ConnectionClosedException()));
    }
    
    // Check if data is immediately available
    {
        std::unique_lock lock(_buffer_mutex);
        if (!_read_buffer.empty()) {
            auto data = std::move(_read_buffer.front());
            _read_buffer.pop();
            return folly::makeFuture(std::move(data));
        }
    }
    
    // No data available - wait with timeout
    auto promise = std::make_shared<folly::Promise<std::vector<std::byte>>>();
    auto future = promise->getFuture();
    
    // Start a background task to wait for data with timeout
    std::thread([this, promise, timeout]() {
        std::unique_lock lock(_buffer_mutex);
        bool data_arrived = _data_available.wait_for(lock, timeout, [this] { 
            return !_read_buffer.empty() || !_open; 
        });
        
        if (!_open) {
            promise->setException(ConnectionClosedException());
            return;
        }
        
        if (data_arrived && !_read_buffer.empty()) {
            auto data = std::move(_read_buffer.front());
            _read_buffer.pop();
            promise->setValue(std::move(data));
        } else {
            // Timeout occurred
            promise->setException(TimeoutException());
        }
    }).detach();
    
    return future;
}

template<address Addr, port Port>
auto Connection<Addr, Port>::write(std::vector<std::byte> data) -> folly::Future<bool> {
    // Check if connection is open
    if (!_open) {
        return folly::makeFuture<bool>(
            folly::exception_wrapper(ConnectionClosedException()));
    }
    
    // Create a message with the data as payload
    Message<Addr, Port> msg(
        _local.address(),
        _local.port(),
        _remote.address(),
        _remote.port(),
        std::move(data)
    );
    
    // Route the message through the simulator
    return _simulator->route_message(std::move(msg));
}

template<address Addr, port Port>
auto Connection<Addr, Port>::write(std::vector<std::byte> data, std::chrono::milliseconds timeout) -> folly::Future<bool> {
    // Check if connection is open
    if (!_open) {
        return folly::makeFuture<bool>(
            folly::exception_wrapper(ConnectionClosedException()));
    }
    
    // Create a message with the data as payload
    Message<Addr, Port> msg(
        _local.address(),
        _local.port(),
        _remote.address(),
        _remote.port(),
        std::move(data)
    );
    
    // Route the message through the simulator with timeout
    return _simulator->route_message(std::move(msg))
        .within(timeout)
        .thenError([](folly::exception_wrapper&&) -> bool {
            throw TimeoutException();
        });
}

template<address Addr, port Port>
auto Connection<Addr, Port>::close() -> void {
    _open = false;
    
    // Wake up any blocked read operations
    {
        std::unique_lock lock(_buffer_mutex);
        _data_available.notify_all();
    }
}

template<address Addr, port Port>
auto Connection<Addr, Port>::is_open() const -> bool {
    return _open;
}

template<address Addr, port Port>
auto Connection<Addr, Port>::deliver_data(std::vector<std::byte> data) -> void {
    std::unique_lock lock(_buffer_mutex);
    _read_buffer.push(std::move(data));
    _data_available.notify_one();
}

} // namespace network_simulator
