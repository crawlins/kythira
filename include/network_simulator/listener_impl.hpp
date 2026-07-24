#pragma once

#include "listener.hpp"
#include <raft/future_default.hpp>

namespace network_simulator {

template<typename Types> auto Listener<Types>::accept() -> future_connection_type {
    std::unique_lock lock(_queue_mutex);

    if (!_listening.load()) {
        return kythira::future_factory_default::makeFuture(std::shared_ptr<connection_type>{});
    }

    // If there's already a pending connection, return it immediately
    if (!_pending_connections.empty()) {
        auto connection = _pending_connections.front();
        _pending_connections.pop();

        return kythira::future_factory_default::makeFuture(connection);
    }

    // No pending connections - wait indefinitely using condition variable
    _connection_available.wait(
        lock, [this] { return !_pending_connections.empty() || !_listening.load(); });

    // Check if listener was closed while waiting
    if (!_listening.load()) {
        return kythira::future_factory_default::makeFuture(std::shared_ptr<connection_type>{});
    }

    // Should have a connection now
    if (_pending_connections.empty()) {
        // This shouldn't happen, but handle it gracefully
        return kythira::future_factory_default::makeFuture(std::shared_ptr<connection_type>{});
    }

    auto connection = _pending_connections.front();
    _pending_connections.pop();

    return kythira::future_factory_default::makeFuture(connection);
}

template<typename Types>
auto Listener<Types>::accept(std::chrono::milliseconds timeout) -> future_connection_type {
    std::unique_lock lock(_queue_mutex);

    if (!_listening.load()) {
        return kythira::future_factory_default::makeFuture(std::shared_ptr<connection_type>{});
    }

    // If there's already a pending connection, return it immediately
    if (!_pending_connections.empty()) {
        auto connection = _pending_connections.front();
        _pending_connections.pop();

        return kythira::future_factory_default::makeFuture(connection);
    }

    // No pending connections - wait with timeout using condition variable
    bool connection_available = _connection_available.wait_for(
        lock, timeout, [this] { return !_pending_connections.empty() || !_listening.load(); });

    // Check if listener was closed while waiting
    if (!_listening.load()) {
        return kythira::future_factory_default::makeFuture(std::shared_ptr<connection_type>{});
    }

    // Check if timeout occurred or no connection available
    if (!connection_available || _pending_connections.empty()) {
        return kythira::future_factory_default::makeExceptionalFuture<
            std::shared_ptr<connection_type>>(std::make_exception_ptr(TimeoutException()));
    }

    // Get the connection
    auto connection = _pending_connections.front();
    _pending_connections.pop();

    return kythira::future_factory_default::makeFuture(connection);
}

template<typename Types> auto Listener<Types>::close() -> void {
    std::unique_lock lock(_queue_mutex);
    _listening.store(false);

    // Notify any threads waiting on the condition variable
    _connection_available.notify_all();
}

template<typename Types> auto Listener<Types>::is_listening() const -> bool {
    return _listening.load();
}

template<typename Types>
auto Listener<Types>::queue_pending_connection(std::shared_ptr<connection_type> connection)
    -> void {
    std::unique_lock lock(_queue_mutex);

    if (_listening.load()) {
        // Queue the connection
        _pending_connections.push(connection);
        _connection_available.notify_one();
    }
}

}  // namespace network_simulator