#pragma once

#include "connection_pool.hpp"
#include <algorithm>

namespace network_simulator {

template<typename Types>
ConnectionPool<Types>::ConnectionPool(PoolConfig config)
    : _config(std::move(config))
{}

template<typename Types>
auto ConnectionPool<Types>::get_or_create_connection(
    endpoint_type destination,
    std::function<future_connection_type()> create_fn) -> future_connection_type {
    
    // First, try to find a healthy pooled connection
    {
        std::unique_lock lock(_pool_mutex);
        
        auto pool_it = _connection_pools.find(destination);
        if (pool_it != _connection_pools.end() && !pool_it->second.empty()) {
            // Search for a healthy connection
            for (auto it = pool_it->second.begin(); it != pool_it->second.end(); ++it) {
                if (is_connection_healthy(*it) && it->connection && it->connection->is_open()) {
                    // Found a healthy connection, reuse it
                    auto conn = it->connection;
                    it->last_used = std::chrono::steady_clock::now();
                    
                    // Move to end (most recently used)
                    auto pooled_conn = std::move(*it);
                    pool_it->second.erase(it);
                    pool_it->second.push_back(std::move(pooled_conn));
                    
#ifdef FOLLY_FUTURES_AVAILABLE
                    return folly::makeFuture(conn);
#else
                    return future_connection_type(conn);
#endif
                }
            }
            
            // No healthy connections found, remove stale ones
            pool_it->second.erase(
                std::remove_if(pool_it->second.begin(), pool_it->second.end(),
                    [this](const PooledConnection& pc) {
                        return !is_connection_healthy(pc) || !pc.connection || !pc.connection->is_open();
                    }),
                pool_it->second.end()
            );
        }
    }
    
    // No pooled connection available, create a new one
    return create_fn();
}

template<typename Types>
auto ConnectionPool<Types>::return_connection(std::shared_ptr<connection_type> conn) -> void {
    if (!conn || !conn->is_open()) {
        return; // Don't pool closed connections
    }
    
    std::unique_lock lock(_pool_mutex);
    
    // Determine the destination endpoint from the connection
    auto remote_ep = conn->remote_endpoint();
    
    auto& pool = _connection_pools[remote_ep];
    
    // Check if pool is at capacity
    if (pool.size() >= _config.max_connections_per_endpoint) {
        // Evict LRU connection
        evict_lru_connection(remote_ep);
    }
    
    // Add connection to pool
    pool.emplace_back(conn);
}

template<typename Types>
auto ConnectionPool<Types>::cleanup_stale_connections() -> void {
    std::unique_lock lock(_pool_mutex);
    
    for (auto& [endpoint, pool] : _connection_pools) {
        // Remove stale or unhealthy connections
        pool.erase(
            std::remove_if(pool.begin(), pool.end(),
                [this](const PooledConnection& pc) {
                    return !is_connection_healthy(pc) || 
                           pc.is_stale(_config.max_idle_time) ||
                           !pc.connection ||
                           !pc.connection->is_open();
                }),
            pool.end()
        );
    }
    
    // Remove empty pools
    for (auto it = _connection_pools.begin(); it != _connection_pools.end();) {
        if (it->second.empty()) {
            it = _connection_pools.erase(it);
        } else {
            ++it;
        }
    }
}

template<typename Types>
auto ConnectionPool<Types>::configure_pool(PoolConfig config) -> void {
    std::unique_lock lock(_pool_mutex);
    _config = std::move(config);
}

template<typename Types>
auto ConnectionPool<Types>::get_pool_size(endpoint_type destination) const -> std::size_t {
    std::shared_lock lock(_pool_mutex);
    
    auto it = _connection_pools.find(destination);
    if (it != _connection_pools.end()) {
        return it->second.size();
    }
    return 0;
}

template<typename Types>
auto ConnectionPool<Types>::evict_lru_connection(endpoint_type destination) -> void {
    // Note: Caller must hold _pool_mutex
    
    auto pool_it = _connection_pools.find(destination);
    if (pool_it == _connection_pools.end() || pool_it->second.empty()) {
        return;
    }
    
    // Find LRU connection (oldest last_used time)
    auto lru_it = std::min_element(pool_it->second.begin(), pool_it->second.end(),
        [](const PooledConnection& a, const PooledConnection& b) {
            return a.last_used < b.last_used;
        });
    
    if (lru_it != pool_it->second.end()) {
        // Close the connection before removing
        if (lru_it->connection && lru_it->connection->is_open()) {
            lru_it->connection->close();
        }
        pool_it->second.erase(lru_it);
    }
}

template<typename Types>
auto ConnectionPool<Types>::is_connection_healthy(const PooledConnection& pooled_conn) const -> bool {
    if (!_config.enable_health_checks) {
        return true; // Skip health checks if disabled
    }
    
    // Check if connection is too old
    auto age = std::chrono::steady_clock::now() - pooled_conn.created;
    if (age > _config.max_connection_age) {
        return false;
    }
    
    // Check if connection is stale
    if (pooled_conn.is_stale(_config.max_idle_time)) {
        return false;
    }
    
    // Check if connection is still open
    if (!pooled_conn.connection || !pooled_conn.connection->is_open()) {
        return false;
    }
    
    return pooled_conn.is_healthy;
}

} // namespace network_simulator
