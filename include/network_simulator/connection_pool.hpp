#pragma once

#include "types.hpp"
#include "connection.hpp"
#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>
#include <shared_mutex>

namespace network_simulator {

// Forward declaration
template<typename Types>
class NetworkSimulator;

template<typename Types>
class ConnectionPool {
public:
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    using connection_type = typename Types::connection_type;
    using endpoint_type = Endpoint<Types>;
    using future_connection_type = typename Types::future_connection_type;
    
    struct PooledConnection {
        std::shared_ptr<connection_type> connection;
        std::chrono::steady_clock::time_point last_used;
        std::chrono::steady_clock::time_point created;
        bool is_healthy;
        
        PooledConnection(std::shared_ptr<connection_type> conn)
            : connection(std::move(conn))
            , last_used(std::chrono::steady_clock::now())
            , created(std::chrono::steady_clock::now())
            , is_healthy(true)
        {}
        
        auto is_stale(std::chrono::milliseconds max_age) const -> bool {
            return std::chrono::steady_clock::now() - last_used > max_age;
        }
    };
    
    struct PoolConfig {
        std::size_t max_connections_per_endpoint = 10;
        std::chrono::milliseconds max_idle_time{300000}; // 5 minutes
        std::chrono::milliseconds max_connection_age{3600000}; // 1 hour
        bool enable_health_checks = true;
    };
    
    explicit ConnectionPool(PoolConfig config = PoolConfig{});
    
    auto get_or_create_connection(endpoint_type destination, 
                                  std::function<future_connection_type()> create_fn) -> future_connection_type;
    auto return_connection(std::shared_ptr<connection_type> conn) -> void;
    auto cleanup_stale_connections() -> void;
    auto configure_pool(PoolConfig config) -> void;
    auto get_pool_size(endpoint_type destination) const -> std::size_t;
    
private:
    // Pool organized by destination endpoint
    std::unordered_map<endpoint_type, std::vector<PooledConnection>> _connection_pools;
    PoolConfig _config;
    mutable std::shared_mutex _pool_mutex;
    
    auto evict_lru_connection(endpoint_type destination) -> void;
    auto is_connection_healthy(const PooledConnection& pooled_conn) const -> bool;
};

} // namespace network_simulator
