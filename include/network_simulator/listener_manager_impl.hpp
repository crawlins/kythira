// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

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
    // Closed outside the lock: Listener::close() reports back through
    // release_listener(), which takes it.
    std::shared_ptr<listener_type> listener;
    {
        std::unique_lock lock(_listeners_mutex);

        auto it = _active_listeners.find(endpoint);
        if (it == _active_listeners.end()) {
            return;
        }
        it->second.is_active.store(false);
        it->second.pending_connections.clear();
        listener = it->second.listener;

        {
            std::lock_guard port_lock(_port_allocation_mutex);
            _allocated_ports.erase(endpoint.port);
        }
        _active_listeners.erase(it);
    }

    if (listener && listener->is_listening()) {
        listener->close();
    }
}

template<typename Types>
auto ListenerManager<Types>::release_listener(endpoint_type endpoint, const listener_type* listener)
    -> void {
    std::unique_lock lock(_listeners_mutex);

    auto it = _active_listeners.find(endpoint);
    if (it == _active_listeners.end() || it->second.listener.get() != listener) {
        return;
    }
    it->second.is_active.store(false);
    it->second.pending_connections.clear();
    {
        std::lock_guard port_lock(_port_allocation_mutex);
        _allocated_ports.erase(endpoint.port);
    }
    _active_listeners.erase(it);
}

template<typename Types> auto ListenerManager<Types>::cleanup_all_listeners() -> void {
    // As in close_listener(): take them out under the lock, close them after.
    std::vector<std::shared_ptr<listener_type>> listeners;
    {
        std::unique_lock lock(_listeners_mutex);

        for (auto& [endpoint, resource] : _active_listeners) {
            resource.is_active.store(false);
            resource.pending_connections.clear();
            if (resource.listener) {
                listeners.push_back(resource.listener);
            }
        }
        _active_listeners.clear();

        std::lock_guard port_lock(_port_allocation_mutex);
        _allocated_ports.clear();
    }

    for (auto& listener : listeners) {
        if (listener->is_listening()) {
            listener->close();
        }
    }
}

template<typename Types> auto ListenerManager<Types>::release_port(port_type port) -> void {
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
auto ListenerManager<Types>::get_listener(endpoint_type endpoint) const
    -> std::shared_ptr<listener_type> {
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

}  // namespace network_simulator
