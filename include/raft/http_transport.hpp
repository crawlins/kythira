// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/types.hpp>
#include <raft/network.hpp>
#include <raft/http_exceptions.hpp>
#include <raft/metrics.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/http_content_negotiation.hpp>
#include <raft/peer_capability_cache.hpp>
#include <concepts/future.hpp>
#include <network_simulator/types.hpp>

#include <string>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <stop_token>
#include <future>
#include <vector>

// Forward declarations for folly (when available)
#ifdef FOLLY_AVAILABLE
namespace folly {
template<typename T> class Future;
}
#endif

// Forward declarations for cpp-httplib
namespace httplib {
class Client;
class Server;
class SSLServer;
class Request;
class Response;
}

namespace kythira {

// Default HTTP transport types implementation using folly
template<typename RPC_Serializer, typename Metrics, typename Executor> struct http_transport_types {
#ifdef FOLLY_AVAILABLE
    template<typename T> using future_template = folly::Future<T>;
#else
    template<typename T> using future_template = network_simulator::SimpleFuture<T>;
#endif
    using serializer_type = RPC_Serializer;
    using serializer_registry_type = single_serializer_registry<RPC_Serializer>;
    using metrics_type = Metrics;
    using executor_type = Executor;
};

// Alternative implementations for different future types
template<typename RPC_Serializer, typename Metrics, typename Executor>
struct std_http_transport_types {
    template<typename T> using future_template = std::future<T>;
    using serializer_type = RPC_Serializer;
    using serializer_registry_type = single_serializer_registry<RPC_Serializer>;
    using metrics_type = Metrics;
    using executor_type = Executor;
};

// Simple future implementation for when folly is not available
template<typename RPC_Serializer, typename Metrics, typename Executor>
struct simple_http_transport_types {
    template<typename T> using future_template = network_simulator::SimpleFuture<T>;
    using serializer_type = RPC_Serializer;
    using serializer_registry_type = single_serializer_registry<RPC_Serializer>;
    using metrics_type = Metrics;
    using executor_type = Executor;
};

// Client configuration structure
struct cpp_httplib_client_config {
    std::size_t connection_pool_size{10};
    std::chrono::milliseconds connection_timeout{5000};
    std::chrono::milliseconds request_timeout{10000};
    std::chrono::milliseconds keep_alive_timeout{60000};
    bool enable_ssl_verification{true};
    std::string ca_cert_path{};
    std::string client_cert_path{};
    std::string client_key_path{};
    std::string cipher_suites{};
    std::string min_tls_version{"TLSv1.2"};
    std::string max_tls_version{"TLSv1.3"};
    std::string user_agent{"raft-cpp-httplib/1.0"};

    /// @brief Disable Nagle's algorithm on the socket.
    ///
    /// **Default `true`, which is a change of behaviour and a deliberate one.**
    /// cpp-httplib's own default (`CPPHTTPLIB_TCP_NODELAY`) is `false`, so a
    /// small write waits for an ACK the peer's delayed-ACK timer will not send
    /// for 40 ms. That is the wrong default for RPC traffic, where every
    /// message is small and every one is on a latency path.
    ///
    /// It is a field rather than an unconditional call so that a row taken
    /// before this existed can be re-taken as it was originally measured.
    /// `.kiro/specs/multi-raft-performance/` compares rows across sessions and
    /// machines; a behaviour change with no way back makes every such
    /// comparison a comparison of two binaries.
    bool tcp_nodelay{true};
};

// Server configuration structure
struct cpp_httplib_server_config {
    std::size_t max_concurrent_connections{100};
    std::size_t max_request_body_size{10 * 1024 * 1024};  // 10 MB
    std::chrono::seconds request_timeout{30};
    bool enable_ssl{false};
    std::string ssl_cert_path{};
    std::string ssl_key_path{};
    std::string ca_cert_path{};
    bool require_client_cert{false};
    std::string cipher_suites{};
    std::string min_tls_version{"TLSv1.2"};
    std::string max_tls_version{"TLSv1.3"};

    /// @brief Disable Nagle's algorithm on the socket.
    ///
    /// **Default `true`, which is a change of behaviour and a deliberate one.**
    /// cpp-httplib's own default (`CPPHTTPLIB_TCP_NODELAY`) is `false`, so a
    /// small write waits for an ACK the peer's delayed-ACK timer will not send
    /// for 40 ms. That is the wrong default for RPC traffic, where every
    /// message is small and every one is on a latency path.
    ///
    /// It is a field rather than an unconditional call so that a row taken
    /// before this existed can be re-taken as it was originally measured.
    /// `.kiro/specs/multi-raft-performance/` compares rows across sessions and
    /// machines; a behaviour change with no way back makes every such
    /// comparison a comparison of two binaries.
    bool tcp_nodelay{true};
};

// HTTP client implementation
template<typename Types>
requires kythira::transport_types<Types>
class cpp_httplib_client {
public:
    // Type aliases for convenience
    using serializer_type = typename Types::serializer_type;
    using serializer_registry_type = typename Types::serializer_registry_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;
    template<typename T> using future_template = typename Types::template future_template<T>;

    // Constructor
    cpp_httplib_client(std::unordered_map<std::uint64_t, std::string> node_id_to_url_map,
                       cpp_httplib_client_config config, metrics_type metrics);

    // Destructor
    ~cpp_httplib_client();

    // Network client interface
    auto send_request_vote(std::uint64_t target, const kythira::request_vote_request<>& request,
                           std::chrono::milliseconds timeout) ->
        typename Types::template future_template<kythira::request_vote_response<>>;

    auto send_append_entries(std::uint64_t target, const kythira::append_entries_request<>& request,
                             std::chrono::milliseconds timeout) ->
        typename Types::template future_template<kythira::append_entries_response<>>;

    auto send_install_snapshot(std::uint64_t target,
                               const kythira::install_snapshot_request<>& request,
                               std::chrono::milliseconds timeout) ->
        typename Types::template future_template<kythira::install_snapshot_response<>>;

    /// Validates `client_cert_path`/`client_key_path`/`ca_cert_path`, then retires
    /// every cached per-node `httplib::Client` so subsequent RPCs build fresh
    /// connections using the reloaded material. Retired clients are kept alive
    /// (not destroyed) rather than erased outright, since a concurrent in-flight
    /// RPC may still hold a raw pointer obtained from `get_or_create_client()`
    /// before the reload — matching Requirement 16.4 (already-established
    /// sessions are unaffected, never forcibly dropped).
    auto reload_tls_material() -> void;

    /// Starts a background thread that polls `client_cert_path`'s mtime every
    /// `poll_interval` and calls `reload_tls_material()` when it has changed.
    auto enable_auto_reload(std::chrono::seconds poll_interval) -> void;

    /// Stops the auto-reload background thread cleanly (joined, not detached).
    auto disable_auto_reload() -> void;

private:
    serializer_type _serializer;
    /// Negotiation goes through the registry; `_serializer` is retained because
    /// the surrounding code still names it and, for a single-serializer bundle,
    /// the two encode identically.
    serializer_registry_type _registry;
    /// What each peer last answered in. An optimisation only — every request
    /// still advertises the full `Accept` list, so a stale entry costs a
    /// re-choice, never a failure.
    peer_capability_cache<std::uint64_t> _capability_cache;
    std::unordered_map<std::uint64_t, std::string> _node_id_to_url;
    std::unordered_map<std::uint64_t, std::unique_ptr<httplib::Client>> _http_clients;
    std::vector<std::unique_ptr<httplib::Client>> _retired_clients;
    cpp_httplib_client_config _config;
    metrics_type _metrics;
    mutable std::mutex _mutex;
    std::jthread _auto_reload_thread;
    std::filesystem::file_time_type _last_reloaded_cert_mtime{};

    // Helper methods
    auto get_base_url(std::uint64_t node_id) const -> std::string;
    auto get_or_create_client(std::uint64_t node_id) -> httplib::Client*;
    auto configure_ssl_client(httplib::Client* client) -> void;
    auto load_client_certificates() -> void;
    auto validate_certificate_files() const -> void;

    template<typename Request, typename Response>
    auto send_rpc(std::uint64_t target, const std::string& endpoint, const Request& request,
                  std::chrono::milliseconds timeout) ->
        typename Types::template future_template<Response>;
};

// HTTP server implementation
template<typename Types>
requires kythira::transport_types<Types>
class cpp_httplib_server {
public:
    // Type aliases for convenience
    using serializer_type = typename Types::serializer_type;
    using serializer_registry_type = typename Types::serializer_registry_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;
    template<typename T> using future_template = typename Types::template future_template<T>;

    // Constructor
    cpp_httplib_server(std::string bind_address, std::uint16_t bind_port,
                       cpp_httplib_server_config config, metrics_type metrics);

    // Destructor
    ~cpp_httplib_server();

    // Network server interface
    auto register_request_vote_handler(
        std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)>
            handler) -> void;

    auto register_append_entries_handler(
        std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
            handler) -> void;

    auto register_install_snapshot_handler(std::function<kythira::install_snapshot_response<>(
                                               const kythira::install_snapshot_request<>&)>
                                               handler) -> void;

    auto start() -> void;
    auto stop() -> void;
    auto is_running() const -> bool;

    /// @brief The port this server is actually listening on, or 0 before the
    /// first successful `start()`.
    ///
    /// Equal to the constructor's `bind_port` whenever that was non-zero. Pass 0
    /// instead to have the kernel allocate a free port, then read it back here
    /// to build the client's URL — the only collision-free way to pick a port,
    /// since any port chosen in advance can be taken before the bind happens.
    /// Mirrors `coap_server::bound_port()` and `grpc_server::bound_port()`.
    auto bound_port() const -> std::uint16_t;

    /// Re-reads `ssl_cert_path`/`ssl_key_path`/`ca_cert_path` from disk and applies
    /// them to the live SSL context, without closing the listening socket or
    /// dropping any established connection. Validates the new material first
    /// (all-or-nothing): on failure, throws and the server keeps serving its
    /// previous, still-valid material. Requires `enable_ssl` and a running server.
    auto reload_tls_material() -> void;

    /// Starts a background thread that polls `ssl_cert_path`'s mtime every
    /// `poll_interval` and calls `reload_tls_material()` when it has changed
    /// since the last successful reload. A failed automatic reload is reported
    /// via `metrics_type` and does not stop the poll loop.
    auto enable_auto_reload(std::chrono::seconds poll_interval) -> void;

    /// Stops the auto-reload background thread cleanly (joined, not detached).
    auto disable_auto_reload() -> void;

private:
    serializer_type _serializer;
    /// Decoding and encoding both go through the registry, so the server can
    /// answer a peer in a format it did not itself pick.
    serializer_registry_type _registry;
    std::unique_ptr<httplib::Server> _http_server;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    std::unique_ptr<httplib::SSLServer> _ssl_server;
#endif
    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)>
        _request_vote_handler;
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
        _append_entries_handler;
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)>
        _install_snapshot_handler;
    std::string _bind_address;
    std::uint16_t _bind_port;
    // Resolved by start(); atomic because bound_port() is callable without the
    // mutex, in the same spirit as _running.
    std::atomic<std::uint16_t> _bound_port{0};
    cpp_httplib_server_config _config;
    metrics_type _metrics;
    std::atomic<bool> _running{false};
    mutable std::mutex _mutex;
    std::thread _server_thread;
    std::jthread _auto_reload_thread;
    std::filesystem::file_time_type _last_reloaded_cert_mtime{};

    // Helper methods
    auto setup_endpoints() -> void;
    auto configure_ssl_server() -> void;
    auto load_server_certificates() -> void;
    auto validate_certificate_files() const -> void;
    auto active_server() -> httplib::Server*;

    template<typename Request, typename Response>
    auto handle_rpc_endpoint(const httplib::Request& http_req, httplib::Response& http_resp,
                             std::function<Response(const Request&)> handler) -> void;
};

}  // namespace kythira
