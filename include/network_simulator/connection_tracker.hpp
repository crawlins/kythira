// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "types.hpp"
#include "connection.hpp"
#include <chrono>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>
#include <optional>
#include <functional>
#include <shared_mutex>
#include <string>

namespace network_simulator {

enum class ConnectionState : std::uint8_t {
    CONNECTING,  // Connection establishment in progress
    CONNECTED,   // Connection established and ready
    CLOSING,     // Connection close initiated
    CLOSED,      // Connection closed
    ERROR        // Connection in error state
};

template<typename Types> class ConnectionTracker {
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

        // Keep-alive bookkeeping. A probe does not count as activity: it keeps
        // `last_activity` untouched, so the idle timeout still measures time
        // since the application last moved data.
        std::chrono::steady_clock::time_point last_keep_alive;
        std::size_t keep_alive_probes_sent = 0;
        std::size_t keep_alive_probes_missed = 0;  // consecutive, reset on success

        ConnectionStats()
            : established_time(std::chrono::steady_clock::now()),
              last_activity(established_time),
              last_keep_alive(established_time) {}
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
            : local_endpoint(std::move(local)),
              remote_endpoint(std::move(remote)),
              state(ConnectionState::CONNECTING) {}
    };

    /// Sends one keep-alive probe from `local` to `remote` and reports whether
    /// the peer answered. The simulator installs one that walks the topology;
    /// without a probe, `process_keep_alive()` does nothing.
    using keep_alive_probe_type =
        std::function<bool(const endpoint_type& local, const endpoint_type& remote)>;

    ConnectionTracker();

    auto register_connection(endpoint_type local, endpoint_type remote,
                             std::shared_ptr<connection_type> conn) -> void;
    auto update_connection_state(endpoint_type local, ConnectionState new_state) -> void;
    auto update_connection_stats(endpoint_type local, std::size_t bytes_transferred, bool is_send)
        -> void;
    auto get_connection_info(endpoint_type local) const -> std::optional<ConnectionInfo>;
    auto get_all_connections() const -> std::vector<ConnectionInfo>;
    auto cleanup_connection(endpoint_type local) -> void;

    // Keep-alive and idle management
    //
    // A connected connection that has been quiet for `interval` (no data and
    // no probe) is probed. After `max_missed` consecutive unanswered probes it
    // is marked ERROR, closed, and left in CLOSED with the reason in
    // `last_error`.
    auto configure_keep_alive(std::chrono::milliseconds interval, std::size_t max_missed = 3)
        -> void;
    auto configure_idle_timeout(std::chrono::milliseconds timeout) -> void;
    auto set_keep_alive_probe(keep_alive_probe_type probe) -> void;
    auto process_keep_alive() -> void;
    auto process_idle_timeouts() -> void;

    // Observer registration
    auto set_state_change_callback(endpoint_type local,
                                   std::function<void(ConnectionState, ConnectionState)> callback)
        -> void;

private:
    // Records `reason`, closes the connection if it is still open, and leaves
    // the entry CLOSED even when the connection object is already gone.
    auto close_tracked_connection(const endpoint_type& local,
                                  const std::weak_ptr<connection_type>& connection,
                                  const std::string& reason) -> void;

    std::unordered_map<endpoint_type, ConnectionInfo> _connection_info;
    mutable std::shared_mutex _info_mutex;

    // Keep-alive and idle timeout management
    std::chrono::milliseconds _keep_alive_interval{30000};  // 30 seconds
    std::chrono::milliseconds _idle_timeout{300000};        // 5 minutes
    std::size_t _keep_alive_max_missed{3};
    keep_alive_probe_type _keep_alive_probe;
};

}  // namespace network_simulator
