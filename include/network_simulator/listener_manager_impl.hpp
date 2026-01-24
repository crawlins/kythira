#pragma once

#include "listener_manager.hpp"

namespace network_simulator {

template<typename Types>
auto ListenerManager<Types>::register_listener(endpoint_type endpoint, 
                                               std::shared_ptr<listener_type> listener) -> void {
    std::unique_lock lock(_listeners_mutex);
    
    _active_listeners.emplace(endpoint, ListenerResource(listener, endpoint));
    
    // Track port allocation
    {
        std::lock_guard port_lock(_port_allocation_mutex);
        _allocated_ports.insert(endpoint.port);
    }
}

template<typename Types>
auto ListenerManager<Types>::close_listener(endpoint_type endpoint) -> void {
    std::unique_lock lock(_listeners_mutex);
    
    auto it = _active_listeners.find(endpoint);
    if (it != _active_listeners.end()) {
        // Mark as inactive
        it->second.is_active.store(false);
        
        // Close the listener
        if (it->second.listener && it->second.listener->is_listening()) {
            it->second.listener->close();
        }
        
        // Clear pending connections
        it->second.pending_connections.clear();
        
        // Release the port
        {
            std::lock_guard port_lock(_port_allocation_mutex);
            _allocated_ports.erase(endpoint.port);
        }
        
        // Remove from active listeners
        _active_listeners.erase(it);
    }
}

template<typename Types>
auto ListenerManager<Types>::cleanup_all_listeners() -> void {
    std::unique_lock lock(_listeners_mutex);
    
    // Close all listeners
    for (auto& [endpoint, resource] : _active_listeners) {
        resource.is_active.store(false);
        
        if (resource.listener && resource.listener->is_listening()) {
            resource.listener->close();
        }
        
        resource.pending_connections.clear();
    }
    
    // Clear all listeners
    _active_listeners.clear();
    
    // Release all ports
    {
        std::lock_guard port_lock(_port_allocation_mutex);
        _allocated_ports.clear();
    }
}

template<typename Types>
auto ListenerManager<Types>::release_port(port_type port) -> void {
    std::lock_guard lock(_port_allocation_mutex);
    _allocated_ports.erase(port);
}

template<typename Types>
auto ListenerManager<Types>::is_port_available(port_type port) const -> bool {
    std::lock_guard lock(_port_allocation_mutex);
    return _allocated_ports.find(port) == _allocated_ports.end();
}

template<typename Types>
auto ListenerManager<Types>::is_port_available(address_type addr, port_type port) const -> bool {
    std::shared_lock lock(_listeners_mutex);
    
    endpoint_type ep(addr, port);
    return _active_listeners.find(ep) == _active_listeners.end();
}

template<typename Types>
auto ListenerManager<Types>::get_listener(endpoint_type endpoint) const -> std::shared_ptr<listener_type> {
    std::shared_lock lock(_listeners_mutex);
    
    auto it = _active_listeners.find(endpoint);
    if (it != _active_listeners.end() && it->second.is_active.load()) {
        return it->second.listener;
    }
    return nullptr;
}

template<typename Types>
auto ListenerManager<Types>::get_all_listeners() const -> std::vector<endpoint_type> {
    std::shared_lock lock(_listeners_mutex);
    
    std::vector<endpoint_type> result;
    result.reserve(_active_listeners.size());
    
    for (const auto& [endpoint, resource] : _active_listeners) {
        if (resource.is_active.load()) {
            result.push_back(endpoint);
        }
    }
    
    return result;
}

} // namespace network_simulator
