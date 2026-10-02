// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "connection_tracker.hpp"

#include <algorithm>

namespace network_simulator {

template<typename Types> ConnectionTracker<Types>::ConnectionTracker() = default;

template<typename Types>
auto ConnectionTracker<Types>::register_connection(endpoint_type local, endpoint_type remote,
                                                   std::shared_ptr<connection_type> conn) -> void {
    std::unique_lock lock(_info_mutex);

    ConnectionInfo info(local, remote);
    info.connection_ref = conn;
    info.state = ConnectionState::CONNECTED;

    _connection_info.insert_or_assign(local, std::move(info));
}

template<typename Types>
auto ConnectionTracker<Types>::update_connection_state(endpoint_type local,
                                                       ConnectionState new_state) -> void {
    std::unique_lock lock(_info_mutex);

    auto it = _connection_info.find(local);
    if (it != _connection_info.end()) {
        auto old_state = it->second.state;
        it->second.state = new_state;

        // Invoke callback if registered
        if (it->second.state_change_callback) {
            auto callback = it->second.state_change_callback;
            lock.unlock();  // Release lock before calling callback
            callback(old_state, new_state);
        }
    }
}

template<typename Types>
auto ConnectionTracker<Types>::update_connection_stats(endpoint_type local,
                                                       std::size_t bytes_transferred, bool is_send)
    -> void {
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
auto ConnectionTracker<Types>::configure_keep_alive(std::chrono::milliseconds interval,
                                                    std::size_t max_missed) -> void {
    std::unique_lock lock(_info_mutex);
    _keep_alive_interval = interval;
    _keep_alive_max_missed = std::max<std::size_t>(max_missed, 1);
}

template<typename Types>
auto ConnectionTracker<Types>::configure_idle_timeout(std::chrono::milliseconds timeout) -> void {
    std::unique_lock lock(_info_mutex);
    _idle_timeout = timeout;
}

template<typename Types>
auto ConnectionTracker<Types>::set_keep_alive_probe(keep_alive_probe_type probe) -> void {
    std::unique_lock lock(_info_mutex);
    _keep_alive_probe = std::move(probe);
}

template<typename Types> auto ConnectionTracker<Types>::process_keep_alive() -> void {
    struct Due {
        endpoint_type local;
        endpoint_type remote;
        std::weak_ptr<connection_type> connection;
    };

    std::vector<Due> due;
    keep_alive_probe_type probe;
    std::size_t max_missed = 0;
    {
        std::shared_lock lock(_info_mutex);
        if (!_keep_alive_probe) {
            return;
        }
        probe = _keep_alive_probe;
        max_missed = _keep_alive_max_missed;

        auto now = std::chrono::steady_clock::now();
        for (const auto& [endpoint, info] : _connection_info) {
            if (info.state != ConnectionState::CONNECTED) {
                continue;
            }
            auto last_heard = std::max(info.stats.last_activity, info.stats.last_keep_alive);
            if (now - last_heard >= _keep_alive_interval) {
                due.push_back(Due{endpoint, info.remote_endpoint, info.connection_ref});
            }
        }
    }

    // Probes run unlocked: the simulator's probe takes its own lock, and a
    // connection closing on another thread re-enters this tracker.
    std::vector<Due> dead;
    for (auto& entry : due) {
        bool answered = probe(entry.local, entry.remote);

        std::unique_lock lock(_info_mutex);
        auto it = _connection_info.find(entry.local);
        if (it == _connection_info.end() || it->second.state != ConnectionState::CONNECTED) {
            continue;  // closed or replaced while the probe was out
        }
        auto& stats = it->second.stats;
        stats.last_keep_alive = std::chrono::steady_clock::now();
        ++stats.keep_alive_probes_sent;
        if (answered) {
            stats.keep_alive_probes_missed = 0;
        } else if (++stats.keep_alive_probes_missed >= max_missed) {
            dead.push_back(std::move(entry));
        }
    }

    for (const auto& entry : dead) {
        update_connection_state(entry.local, ConnectionState::ERROR);
        close_tracked_connection(entry.local, entry.connection,
                                 "Connection closed: peer did not answer " +
                                     std::to_string(max_missed) + " keep-alive probes");
    }
}

template<typename Types> auto ConnectionTracker<Types>::process_idle_timeouts() -> void {
    std::vector<std::pair<endpoint_type, std::weak_ptr<connection_type>>> idle;
    {
        std::shared_lock lock(_info_mutex);

        auto now = std::chrono::steady_clock::now();
        for (const auto& [endpoint, info] : _connection_info) {
            if (info.state == ConnectionState::CONNECTED &&
                now - info.stats.last_activity >= _idle_timeout) {
                idle.emplace_back(endpoint, info.connection_ref);
            }
        }
    }

    for (const auto& [endpoint, connection] : idle) {
        close_tracked_connection(endpoint, connection, "Connection closed due to idle timeout");
    }
}

template<typename Types>
auto ConnectionTracker<Types>::close_tracked_connection(
    const endpoint_type& local, const std::weak_ptr<connection_type>& connection,
    const std::string& reason) -> void {
    {
        std::unique_lock lock(_info_mutex);
        auto it = _connection_info.find(local);
        if (it == _connection_info.end()) {
            return;
        }
        it->second.stats.last_error = reason;
    }

    // Closing notifies the simulator, which moves the entry to CLOSED and
    // fires the observer. Done unlocked, because that path re-enters here.
    if (auto conn = connection.lock(); conn && conn->is_open()) {
        conn->close();
    }

    // Covers a connection object that is already gone, or a simulator with
    // tracking notifications turned off.
    std::unique_lock lock(_info_mutex);
    auto it = _connection_info.find(local);
    if (it != _connection_info.end() && it->second.state != ConnectionState::CLOSED) {
        lock.unlock();
        update_connection_state(local, ConnectionState::CLOSED);
    }
}

template<typename Types>
auto ConnectionTracker<Types>::set_state_change_callback(
    endpoint_type local, std::function<void(ConnectionState, ConnectionState)> callback) -> void {
    std::unique_lock lock(_info_mutex);

    auto it = _connection_info.find(local);
    if (it != _connection_info.end()) {
        it->second.state_change_callback = std::move(callback);
    }
}

}  // namespace network_simulator
