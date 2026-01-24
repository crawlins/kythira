#pragma once

#include "types.hpp"
#include "connection.hpp"
#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>
#include <optional>
#include <functional>
#include <shared_mutex>

namespace network_simulator {

enum class ConnectionState {
    CONNECTING,    // Connection establishment in progress
    CONNECTED,     // Connection established and ready
    CLOSING,       // Connection close initiated
    CLOSED,        // Connection closed
    ERROR          // Connection in error state
};

template<typename Types>
class ConnectionTracker {
public:
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    using connection_type = typename Types::connection_type;
    using endpoint_type = Endpoint<Types>;
    
    struct ConnectionStats {
        std::chrono::steady_clock::time_point established_time;
        std::chrono::steady_clock::time_point last_activity;
        std::size_t bytes_sent = 0;
        std::size_t bytes_received = 0;
        std::size_t messages_sent = 0;
        std::size_t messages_received = 0;
        std::optional<std::string> last_error;
        
        ConnectionStats()
            : established_time(std::chrono::steady_clock::now())
            , last_activity(std::chrono::steady_clock::now())
        {}
    };
    
    struct ConnectionInfo {
        endpoint_type local_endpoint;
        endpoint_type remote_endpoint;
        ConnectionState state;
        ConnectionStats stats;
        std::weak_ptr<connection_type> connection_ref;
        
        // Optional observer callback
        std::function<void(ConnectionState, ConnectionState)> state_change_callback;
        
        ConnectionInfo(endpoint_type local, endpoint_type remote)
            : local_endpoint(local)
            , remote_endpoint(remote)
            , state(ConnectionState::CONNECTING)
        {}
    };
    
    ConnectionTracker();
    
    auto register_connection(endpoint_type local, endpoint_type remote, 
                           std::shared_ptr<connection_type> conn) -> void;
    auto update_connection_state(endpoint_type local, ConnectionState new_state) -> void;
    auto update_connection_stats(endpoint_type local, std::size_t bytes_transferred, bool is_send) -> void;
    auto get_connection_info(endpoint_type local) const -> std::optional<ConnectionInfo>;
    auto get_all_connections() const -> std::vector<ConnectionInfo>;
    auto cleanup_connection(endpoint_type local) -> void;
    
    // Keep-alive and idle management
    auto configure_keep_alive(std::chrono::milliseconds interval) -> void;
    auto configure_idle_timeout(std::chrono::milliseconds timeout) -> void;
    auto process_keep_alive() -> void;
    auto process_idle_timeouts() -> void;
    
    // Observer registration
    auto set_state_change_callback(endpoint_type local, 
                                   std::function<void(ConnectionState, ConnectionState)> callback) -> void;
    
private:
    std::unordered_map<endpoint_type, ConnectionInfo> _connection_info;
    mutable std::shared_mutex _info_mutex;
    
    // Keep-alive and idle timeout management
    std::chrono::milliseconds _keep_alive_interval{30000}; // 30 seconds
    std::chrono::milliseconds _idle_timeout{300000}; // 5 minutes
};

} // namespace network_simulator
