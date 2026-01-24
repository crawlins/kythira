#pragma once

#include "connection_tracker.hpp"

namespace network_simulator {

template<typename Types>
ConnectionTracker<Types>::ConnectionTracker() = default;

template<typename Types>
auto ConnectionTracker<Types>::register_connection(
    endpoint_type local,
    endpoint_type remote,
    std::shared_ptr<connection_type> conn) -> void {
    
    std::unique_lock lock(_info_mutex);
    
    ConnectionInfo info(local, remote);
    info.connection_ref = conn;
    info.state = ConnectionState::CONNECTED;
    
    _connection_info.insert_or_assign(local, std::move(info));
}

template<typename Types>
auto ConnectionTracker<Types>::update_connection_state(
    endpoint_type local,
    ConnectionState new_state) -> void {
    
    std::unique_lock lock(_info_mutex);
    
    auto it = _connection_info.find(local);
    if (it != _connection_info.end()) {
        auto old_state = it->second.state;
        it->second.state = new_state;
        
        // Invoke callback if registered
        if (it->second.state_change_callback) {
            auto callback = it->second.state_change_callback;
            lock.unlock(); // Release lock before calling callback
            callback(old_state, new_state);
        }
    }
}

template<typename Types>
auto ConnectionTracker<Types>::update_connection_stats(
    endpoint_type local,
    std::size_t bytes_transferred,
    bool is_send) -> void {
    
    std::unique_lock lock(_info_mutex);
    
    auto it = _connection_info.find(local);
    if (it != _connection_info.end()) {
        it->second.stats.last_activity = std::chrono::steady_clock::now();
        
        if (is_send) {
            it->second.stats.bytes_sent += bytes_transferred;
            it->second.stats.messages_sent++;
        } else {
            it->second.stats.bytes_received += bytes_transferred;
            it->second.stats.messages_received++;
        }
    }
}

template<typename Types>
auto ConnectionTracker<Types>::get_connection_info(endpoint_type local) const 
    -> std::optional<ConnectionInfo> {
    
    std::shared_lock lock(_info_mutex);
    
    auto it = _connection_info.find(local);
    if (it != _connection_info.end()) {
        return it->second;
    }
    return std::nullopt;
}

template<typename Types>
auto ConnectionTracker<Types>::get_all_connections() const -> std::vector<ConnectionInfo> {
    std::shared_lock lock(_info_mutex);
    
    std::vector<ConnectionInfo> result;
    result.reserve(_connection_info.size());
    
    for (const auto& [endpoint, info] : _connection_info) {
        result.push_back(info);
    }
    
    return result;
}

template<typename Types>
auto ConnectionTracker<Types>::cleanup_connection(endpoint_type local) -> void {
    std::unique_lock lock(_info_mutex);
    _connection_info.erase(local);
}

template<typename Types>
auto ConnectionTracker<Types>::configure_keep_alive(std::chrono::milliseconds interval) -> void {
    _keep_alive_interval = interval;
}

template<typename Types>
auto ConnectionTracker<Types>::configure_idle_timeout(std::chrono::milliseconds timeout) -> void {
    _idle_timeout = timeout;
}

template<typename Types>
auto ConnectionTracker<Types>::process_keep_alive() -> void {
    std::shared_lock lock(_info_mutex);
    
    auto now = std::chrono::steady_clock::now();
    
    for (const auto& [endpoint, info] : _connection_info) {
        if (info.state == ConnectionState::CONNECTED) {
            auto time_since_activity = now - info.stats.last_activity;
            
            if (time_since_activity >= _keep_alive_interval) {
                // In a real implementation, we would send a keep-alive message
                // For now, we just update the last activity time
                // This would be done by the connection itself
            }
        }
    }
}

template<typename Types>
auto ConnectionTracker<Types>::process_idle_timeouts() -> void {
    std::unique_lock lock(_info_mutex);
    
    auto now = std::chrono::steady_clock::now();
    std::vector<endpoint_type> to_close;
    
    for (const auto& [endpoint, info] : _connection_info) {
        if (info.state == ConnectionState::CONNECTED) {
            auto time_since_activity = now - info.stats.last_activity;
            
            if (time_since_activity >= _idle_timeout) {
                to_close.push_back(endpoint);
            }
        }
    }
    
    // Close idle connections
    for (const auto& endpoint : to_close) {
        auto it = _connection_info.find(endpoint);
        if (it != _connection_info.end()) {
            auto conn = it->second.connection_ref.lock();
            if (conn && conn->is_open()) {
                lock.unlock();
                conn->close();
                lock.lock();
            }
            
            it->second.state = ConnectionState::CLOSED;
            it->second.stats.last_error = "Connection closed due to idle timeout";
        }
    }
}

template<typename Types>
auto ConnectionTracker<Types>::set_state_change_callback(
    endpoint_type local,
    std::function<void(ConnectionState, ConnectionState)> callback) -> void {
    
    std::unique_lock lock(_info_mutex);
    
    auto it = _connection_info.find(local);
    if (it != _connection_info.end()) {
        it->second.state_change_callback = std::move(callback);
    }
}

} // namespace network_simulator
