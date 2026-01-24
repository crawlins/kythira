#pragma once

#include "types.hpp"
#include "listener.hpp"
#include <chrono>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <shared_mutex>
#include <atomic>

namespace network_simulator {

// Forward declaration
template<typename Types>
class NetworkSimulator;

template<typename Types>
class ListenerManager {
public:
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    using listener_type = typename Types::listener_type;
    using connection_type = typename Types::connection_type;
    using endpoint_type = Endpoint<Types>;
    using future_listener_type = typename Types::future_listener_type;
    
    struct ListenerResource {
        std::shared_ptr<listener_type> listener;
        endpoint_type bound_endpoint;
        std::chrono::steady_clock::time_point created;
        std::vector<std::shared_ptr<connection_type>> pending_connections;
        std::atomic<bool> is_active{true};
        
        ListenerResource(std::shared_ptr<listener_type> l, endpoint_type ep)
            : listener(std::move(l))
            , bound_endpoint(ep)
            , created(std::chrono::steady_clock::now())
        {}
        
        // Delete copy constructor and assignment due to atomic member
        ListenerResource(const ListenerResource&) = delete;
        ListenerResource& operator=(const ListenerResource&) = delete;
        
        // Provide move constructor and assignment
        ListenerResource(ListenerResource&& other) noexcept
            : listener(std::move(other.listener))
            , bound_endpoint(std::move(other.bound_endpoint))
            , created(other.created)
            , pending_connections(std::move(other.pending_connections))
            , is_active(other.is_active.load())
        {}
        
        ListenerResource& operator=(ListenerResource&& other) noexcept {
            if (this != &other) {
                listener = std::move(other.listener);
                bound_endpoint = std::move(other.bound_endpoint);
                created = other.created;
                pending_connections = std::move(other.pending_connections);
                is_active.store(other.is_active.load());
            }
            return *this;
        }
    };
    
    ListenerManager() = default;
    
    auto register_listener(endpoint_type endpoint, std::shared_ptr<listener_type> listener) -> void;
    auto close_listener(endpoint_type endpoint) -> void;
    auto cleanup_all_listeners() -> void;
    auto release_port(port_type port) -> void;
    auto is_port_available(port_type port) const -> bool;
    auto is_port_available(address_type addr, port_type port) const -> bool;
    auto get_listener(endpoint_type endpoint) const -> std::shared_ptr<listener_type>;
    auto get_all_listeners() const -> std::vector<endpoint_type>;
    
private:
    std::unordered_map<endpoint_type, ListenerResource> _active_listeners;
    mutable std::shared_mutex _listeners_mutex;
    
    // Port allocation tracking
    std::unordered_set<port_type> _allocated_ports;
    mutable std::mutex _port_allocation_mutex;
};

} // namespace network_simulator
