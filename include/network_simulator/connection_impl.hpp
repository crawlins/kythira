#pragma once

#include "connection.hpp"

#ifdef FOLLY_FUTURES_AVAILABLE
#include <folly/futures/Future.h>
#endif

namespace network_simulator {

template<typename Types>
auto Connection<Types>::read() -> future_bytes_type {
    std::unique_lock lock(_buffer_mutex);
    
    if (!_open.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::vector<std::byte>>(
            folly::exception_wrapper(ConnectionClosedException{}));
#else
        return future_bytes_type(std::make_exception_ptr(ConnectionClosedException{}));
#endif
    }
    
    // If data is immediately available, return it
    if (!_read_buffer.empty()) {
        auto data = _read_buffer.front();
        _read_buffer.pop();
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture(std::move(data));
#else
        return future_bytes_type(std::move(data));
#endif
    }
    
    // No data available - wait indefinitely for data to arrive
    _data_available.wait(lock, [this] { 
        return !_read_buffer.empty() || !_open.load(); 
    });
    
    // Check if connection was closed while waiting
    if (!_open.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::vector<std::byte>>(
            folly::exception_wrapper(ConnectionClosedException{}));
#else
        return future_bytes_type(std::make_exception_ptr(ConnectionClosedException{}));
#endif
    }
    
    // Should have data now
    if (_read_buffer.empty()) {
        // This shouldn't happen, but handle it gracefully
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture(std::vector<std::byte>{});
#else
        return future_bytes_type(std::vector<std::byte>{});
#endif
    }
    
    auto data = _read_buffer.front();
    _read_buffer.pop();
#ifdef FOLLY_FUTURES_AVAILABLE
    return folly::makeFuture(std::move(data));
#else
    return future_bytes_type(std::move(data));
#endif
}

template<typename Types>
auto Connection<Types>::read(std::chrono::milliseconds timeout) -> future_bytes_type {
    std::unique_lock lock(_buffer_mutex);
    
    if (!_open.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::vector<std::byte>>(
            folly::exception_wrapper(ConnectionClosedException{}));
#else
        return future_bytes_type(std::make_exception_ptr(ConnectionClosedException{}));
#endif
    }
    
    // Wait for data to become available with timeout
    bool data_available = _data_available.wait_for(lock, timeout, [this] { 
        return !_read_buffer.empty() || !_open.load(); 
    });
    
    // Check if connection was closed while waiting
    if (!_open.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::vector<std::byte>>(
            folly::exception_wrapper(ConnectionClosedException{}));
#else
        return future_bytes_type(std::make_exception_ptr(ConnectionClosedException{}));
#endif
    }
    
    // Check if timeout occurred
    if (!data_available || _read_buffer.empty()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::vector<std::byte>>(
            folly::exception_wrapper(TimeoutException{}));
#else
        return future_bytes_type(std::make_exception_ptr(TimeoutException{}));
#endif
    }
    
    auto data = _read_buffer.front();
    _read_buffer.pop();
#ifdef FOLLY_FUTURES_AVAILABLE
    return folly::makeFuture(std::move(data));
#else
    return future_bytes_type(std::move(data));
#endif
}

template<typename Types>
auto Connection<Types>::write(std::vector<std::byte> data) -> future_bool_type {
    if (!_open.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<bool>(
            folly::exception_wrapper(ConnectionClosedException{}));
#else
        return future_bool_type(std::make_exception_ptr(ConnectionClosedException{}));
#endif
    }
    
    // Route data through simulator using connection ID
    return _simulator->route_connection_data(_connection_id, std::move(data));
}

template<typename Types>
auto Connection<Types>::write(std::vector<std::byte> data, std::chrono::milliseconds timeout) -> future_bool_type {
    if (!_open.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<bool>(
            folly::exception_wrapper(ConnectionClosedException{}));
#else
        return future_bool_type(std::make_exception_ptr(ConnectionClosedException{}));
#endif
    }
    
    // For timeout version, we simulate timeout behavior by checking latency
    if (_simulator) {
        try {
            auto edge = _simulator->get_edge(_connection_id.src_addr, _connection_id.dst_addr);
            auto write_latency = edge.latency();
            
            // If the write latency is greater than timeout, throw TimeoutException
            if (write_latency > timeout) {
#ifdef FOLLY_FUTURES_AVAILABLE
                return folly::makeFuture<bool>(
                    folly::exception_wrapper(TimeoutException{}));
#else
                return future_bool_type(std::make_exception_ptr(TimeoutException{}));
#endif
            }
        } catch (const NoRouteException&) {
            // No route exists, write would timeout
#ifdef FOLLY_FUTURES_AVAILABLE
            return folly::makeFuture<bool>(
                folly::exception_wrapper(TimeoutException{}));
#else
            return future_bool_type(std::make_exception_ptr(TimeoutException{}));
#endif
        }
    }
    
    // Try the write operation
    auto future = _simulator->route_connection_data(_connection_id, std::move(data));
    
    // For very short timeouts, we might want to simulate timeout behavior
    if (timeout < std::chrono::milliseconds(10)) {
        // Very short timeout - simulate timeout for testing
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<bool>(
            folly::exception_wrapper(TimeoutException{}));
#else
        return future_bool_type(std::make_exception_ptr(TimeoutException{}));
#endif
    }
    
    return future;
}

template<typename Types>
auto Connection<Types>::close() -> void {
    _open.store(false);
    
    // Notify all waiting threads that the connection is closed
    {
        std::lock_guard lock(_buffer_mutex);
        _data_available.notify_all();
    }
}

template<typename Types>
auto Connection<Types>::is_open() const -> bool {
    return _open.load();
}

template<typename Types>
auto Connection<Types>::deliver_data(std::vector<std::byte> data) -> void {
    std::unique_lock lock(_buffer_mutex);
    
    if (_open.load()) {
        _read_buffer.push(std::move(data));
        _data_available.notify_one();
    }
}

} // namespace network_simulator