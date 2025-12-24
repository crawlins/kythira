#pragma once

#include <raft/types.hpp>
#include <raft/network.hpp>
#include <raft/http_exceptions.hpp>
#include <raft/metrics.hpp>
#include <concepts/future.hpp>

#include <string>
#include <chrono>
#include <cstddef>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>

// Forward declarations for cpp-httplib
namespace httplib {
    class Client;
    class Server;
    class Request;
    class Response;
}

namespace kythira {

// Client configuration structure
struct cpp_httplib_client_config {
    std::size_t connection_pool_size{10};
    std::chrono::milliseconds connection_timeout{5000};
    std::chrono::milliseconds request_timeout{10000};
    std::chrono::milliseconds keep_alive_timeout{60000};
    bool enable_ssl_verification{true};
    std::string ca_cert_path{};
    std::string user_agent{"raft-cpp-httplib/1.0"};
};

// Server configuration structure
struct cpp_httplib_server_config {
    std::size_t max_concurrent_connections{100};
    std::size_t max_request_body_size{10 * 1024 * 1024};  // 10 MB
    std::chrono::seconds request_timeout{30};
    bool enable_ssl{false};
    std::string ssl_cert_path{};
    std::string ssl_key_path{};
};

// HTTP client implementation
template<typename FutureType, typename RPC_Serializer, typename Metrics>
requires 
    raft::rpc_serializer<RPC_Serializer, std::vector<std::byte>> && 
    raft::metrics<Metrics>
class cpp_httplib_client {
public:
    // Constructor
    cpp_httplib_client(
        std::unordered_map<std::uint64_t, std::string> node_id_to_url_map,
        cpp_httplib_client_config config,
        Metrics metrics
    );

    // Destructor
    ~cpp_httplib_client();

    // Network client interface
    auto send_request_vote(
        std::uint64_t target,
        const raft::request_vote_request<>& request,
        std::chrono::milliseconds timeout
    ) -> FutureType;

    auto send_append_entries(
        std::uint64_t target,
        const raft::append_entries_request<>& request,
        std::chrono::milliseconds timeout
    ) -> FutureType;

    auto send_install_snapshot(
        std::uint64_t target,
        const raft::install_snapshot_request<>& request,
        std::chrono::milliseconds timeout
    ) -> FutureType;

private:
    RPC_Serializer _serializer;
    std::unordered_map<std::uint64_t, std::string> _node_id_to_url;
    std::unordered_map<std::uint64_t, std::unique_ptr<httplib::Client>> _http_clients;
    cpp_httplib_client_config _config;
    Metrics _metrics;
    mutable std::mutex _mutex;

    // Helper methods
    auto get_base_url(std::uint64_t node_id) const -> std::string;
    auto get_or_create_client(std::uint64_t node_id) -> httplib::Client*;
    
    template<typename Request, typename Response>
    auto send_rpc(
        std::uint64_t target,
        const std::string& endpoint,
        const Request& request,
        std::chrono::milliseconds timeout
    ) -> FutureType;
};

// HTTP server implementation
template<typename FutureType, typename RPC_Serializer, typename Metrics>
requires 
    future<FutureType, raft::request_vote_response<>> &&
    future<FutureType, raft::append_entries_response<>> &&
    future<FutureType, raft::install_snapshot_response<>> &&
    raft::rpc_serializer<RPC_Serializer, std::vector<std::byte>> && 
    raft::metrics<Metrics>
class cpp_httplib_server {
public:
    // Constructor
    cpp_httplib_server(
        std::string bind_address,
        std::uint16_t bind_port,
        cpp_httplib_server_config config,
        Metrics metrics
    );

    // Destructor
    ~cpp_httplib_server();

    // Network server interface
    auto register_request_vote_handler(
        std::function<raft::request_vote_response<>(const raft::request_vote_request<>&)> handler
    ) -> void;

    auto register_append_entries_handler(
        std::function<raft::append_entries_response<>(const raft::append_entries_request<>&)> handler
    ) -> void;

    auto register_install_snapshot_handler(
        std::function<raft::install_snapshot_response<>(const raft::install_snapshot_request<>&)> handler
    ) -> void;

    auto start() -> void;
    auto stop() -> void;
    auto is_running() const -> bool;

private:
    RPC_Serializer _serializer;
    std::unique_ptr<httplib::Server> _http_server;
    std::function<raft::request_vote_response<>(const raft::request_vote_request<>&)> _request_vote_handler;
    std::function<raft::append_entries_response<>(const raft::append_entries_request<>&)> _append_entries_handler;
    std::function<raft::install_snapshot_response<>(const raft::install_snapshot_request<>&)> _install_snapshot_handler;
    std::string _bind_address;
    std::uint16_t _bind_port;
    cpp_httplib_server_config _config;
    Metrics _metrics;
    std::atomic<bool> _running{false};
    mutable std::mutex _mutex;
    std::thread _server_thread;

    // Helper methods
    auto setup_endpoints() -> void;
    
    template<typename Request, typename Response>
    auto handle_rpc_endpoint(
        const httplib::Request& http_req,
        httplib::Response& http_resp,
        std::function<Response(const Request&)> handler
    ) -> void;
};

} // namespace kythira
