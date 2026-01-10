#pragma once

#include "listener.hpp"

#ifdef FOLLY_FUTURES_AVAILABLE
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#endif

namespace network_simulator {

template<typename Types>
auto Listener<Types>::accept() -> future_connection_type {
    std::unique_lock lock(_queue_mutex);
    
    if (!_listening.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<connection_type>>(
            std::shared_ptr<connection_type>(nullptr));
#else
        return future_connection_type(nullptr);
#endif
    }
    
    // If there's already a pending connection, return it immediately
    if (!_pending_connections.empty()) {
        auto connection = _pending_connections.front();
        _pending_connections.pop();
        
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture(std::move(connection));
#else
        return future_connection_type(connection);
#endif
    }
    
    // No pending connections - wait indefinitely using condition variable
    _connection_available.wait(lock, [this] { 
        return !_pending_connections.empty() || !_listening.load(); 
    });
    
    // Check if listener was closed while waiting
    if (!_listening.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<connection_type>>(
            std::shared_ptr<connection_type>(nullptr));
#else
        return future_connection_type(nullptr);
#endif
    }
    
    // Should have a connection now
    if (_pending_connections.empty()) {
        // This shouldn't happen, but handle it gracefully
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<connection_type>>(
            std::shared_ptr<connection_type>(nullptr));
#else
        return future_connection_type(nullptr);
#endif
    }
    
    auto connection = _pending_connections.front();
    _pending_connections.pop();
    
#ifdef FOLLY_FUTURES_AVAILABLE
    return folly::makeFuture(std::move(connection));
#else
    return future_connection_type(connection);
#endif
}

template<typename Types>
auto Listener<Types>::accept(std::chrono::milliseconds timeout) -> future_connection_type {
    std::unique_lock lock(_queue_mutex);
    
    if (!_listening.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<connection_type>>(
            std::shared_ptr<connection_type>(nullptr));
#else
        return future_connection_type(nullptr);
#endif
    }
    
    // If there's already a pending connection, return it immediately
    if (!_pending_connections.empty()) {
        auto connection = _pending_connections.front();
        _pending_connections.pop();
        
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture(std::move(connection));
#else
        return future_connection_type(connection);
#endif
    }
    
    // No pending connections - wait with timeout using condition variable
    bool connection_available = _connection_available.wait_for(lock, timeout, [this] { 
        return !_pending_connections.empty() || !_listening.load(); 
    });
    
    // Check if listener was closed while waiting
    if (!_listening.load()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<connection_type>>(
            std::shared_ptr<connection_type>(nullptr));
#else
        return future_connection_type(nullptr);
#endif
    }
    
    // Check if timeout occurred or no connection available
    if (!connection_available || _pending_connections.empty()) {
#ifdef FOLLY_FUTURES_AVAILABLE
        return folly::makeFuture<std::shared_ptr<connection_type>>(
            folly::exception_wrapper(TimeoutException()));
#else
        return future_connection_type(std::make_exception_ptr(TimeoutException()));
#endif
    }
    
    // Get the connection
    auto connection = _pending_connections.front();
    _pending_connections.pop();
    
#ifdef FOLLY_FUTURES_AVAILABLE
    return folly::makeFuture(std::move(connection));
#else
    return future_connection_type(connection);
#endif
}

template<typename Types>
auto Listener<Types>::close() -> void {
    std::unique_lock lock(_queue_mutex);
    _listening.store(false);
    
    // Notify any threads waiting on the condition variable
    _connection_available.notify_all();
}

template<typename Types>
auto Listener<Types>::is_listening() const -> bool {
    return _listening.load();
}

template<typename Types>
auto Listener<Types>::queue_pending_connection(std::shared_ptr<connection_type> connection) -> void {
    std::unique_lock lock(_queue_mutex);
    
    if (_listening.load()) {
        // Queue the connection
        _pending_connections.push(connection);
        _connection_available.notify_one();
    }
}

} // namespace network_simulator