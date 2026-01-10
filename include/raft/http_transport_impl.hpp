#pragma once

#include <raft/http_transport.hpp>
#include <httplib.h>
#include <format>
#include <stdexcept>
#include <thread>

// Conditional includes based on folly availability
#ifdef FOLLY_AVAILABLE
#include <folly/Future.h>
#else
#include <network_simulator/types.hpp>
#include <future>
#endif

namespace kythira {

namespace {
    constexpr const char* endpoint_request_vote = "/v1/raft/request_vote";
    constexpr const char* endpoint_append_entries = "/v1/raft/append_entries";
    constexpr const char* endpoint_install_snapshot = "/v1/raft/install_snapshot";
    constexpr const char* content_type_json = "application/json";
    constexpr const char* header_content_type = "Content-Type";
    constexpr const char* header_content_length = "Content-Length";
    constexpr const char* header_user_agent = "User-Agent";
    
    // Helper function to create futures with exceptions
    template<typename Types, typename Response>
    auto make_future_with_exception(const std::exception& e) -> typename Types::template future_template<Response> {
#ifdef FOLLY_AVAILABLE
        if constexpr (std::is_same_v<typename Types::template future_template<Response>, folly::Future<Response>>) {
            return folly::makeFuture<Response>(e);
        } else
#endif
        {
            // For SimpleFuture or std::future
            return typename Types::template future_template<Response>(std::make_exception_ptr(e));
        }
    }
    
    // Helper function to create futures with values
    template<typename Types, typename Response>
    auto make_future_with_value(Response&& value) -> typename Types::template future_template<Response> {
#ifdef FOLLY_AVAILABLE
        if constexpr (std::is_same_v<typename Types::template future_template<Response>, folly::Future<Response>>) {
            return folly::makeFuture<Response>(std::forward<Response>(value));
        } else
#endif
        {
            // For SimpleFuture or std::future
            return typename Types::template future_template<Response>(std::forward<Response>(value));
        }
    }
}

// Constructor implementation
template<typename Types>
requires transport_types<Types>
cpp_httplib_client<Types>::cpp_httplib_client(
    std::unordered_map<std::uint64_t, std::string> node_id_to_url_map,
    cpp_httplib_client_config config,
    typename Types::metrics_type metrics
)
    : _serializer{}
    , _node_id_to_url{std::move(node_id_to_url_map)}
    , _http_clients{}
    , _config{std::move(config)}
    , _metrics{std::move(metrics)}
    , _mutex{}
{
}

// Destructor implementation
template<typename Types>
requires transport_types<Types>
cpp_httplib_client<Types>::~cpp_httplib_client() = default;

// Helper to get base URL for a node
template<typename Types>
requires transport_types<Types>
auto cpp_httplib_client<Types>::get_base_url(std::uint64_t node_id) const -> std::string {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _node_id_to_url.find(node_id);
    if (it == _node_id_to_url.end()) {
        throw std::runtime_error(std::format("No URL mapping found for node {}", node_id));
    }
    return it->second;
}

// Helper to get or create HTTP client for a node
template<typename Types>
requires transport_types<Types>
auto cpp_httplib_client<Types>::get_or_create_client(std::uint64_t node_id) -> httplib::Client* {
    std::lock_guard<std::mutex> lock(_mutex);
    
    auto it = _http_clients.find(node_id);
    if (it != _http_clients.end()) {
        // Emit connection reused metric
        auto metric = _metrics;
        metric.set_metric_name("http.client.connection.reused");
        metric.add_dimension("target_node_id", std::to_string(node_id));
        metric.add_one();
        metric.emit();
        
        return it->second.get();
    }
    
    // Get base URL
    auto url_it = _node_id_to_url.find(node_id);
    if (url_it == _node_id_to_url.end()) {
        throw std::runtime_error(std::format("No URL mapping found for node {}", node_id));
    }
    
    const auto& base_url = url_it->second;
    
    // Parse URL to determine if HTTPS
    bool is_https = base_url.starts_with("https://");
    
    // Create new client
    std::unique_ptr<httplib::Client> client;
    if (is_https) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        // For HTTPS, we need to use the universal Client constructor
        client = std::make_unique<httplib::Client>(base_url);
        
        // Note: SSL configuration would be handled by the Client class internally
        // when it detects HTTPS URLs
#else
        throw std::runtime_error("HTTPS support not available (OpenSSL not enabled)");
#endif
    } else {
        client = std::make_unique<httplib::Client>(base_url);
    }
    
    // Configure timeouts
    client->set_connection_timeout(_config.connection_timeout.count() / 1000, 
                                   (_config.connection_timeout.count() % 1000) * 1000);
    client->set_read_timeout(_config.request_timeout.count() / 1000,
                            (_config.request_timeout.count() % 1000) * 1000);
    client->set_write_timeout(_config.request_timeout.count() / 1000,
                             (_config.request_timeout.count() % 1000) * 1000);
    
    // Enable keep-alive
    client->set_keep_alive(true);
    
    // Store and return
    auto* client_ptr = client.get();
    _http_clients[node_id] = std::move(client);
    
    // Emit connection created metric
    auto metric = _metrics;
    metric.set_metric_name("http.client.connection.created");
    metric.add_dimension("target_node_id", std::to_string(node_id));
    metric.add_one();
    metric.emit();
    
    // Update pool size metric
    metric = _metrics;
    metric.set_metric_name("http.client.connection.pool_size");
    metric.add_dimension("target_node_id", std::to_string(node_id));
    metric.add_value(static_cast<double>(_http_clients.size()));
    metric.emit();
    
    return client_ptr;
}

// Generic RPC send implementation
template<typename Types>
requires transport_types<Types>
template<typename Request, typename Response>
auto cpp_httplib_client<Types>::send_rpc(
    std::uint64_t target,
    const std::string& endpoint,
    const Request& request,
    std::chrono::milliseconds timeout
) -> typename Types::template future_template<Response> {
    try {
        // Get or create HTTP client
        auto* client = this->get_or_create_client(target);
        
        // Serialize request
        auto serialized_data = _serializer.serialize(request);
        
        // Convert bytes to string for HTTP body
        std::string body;
        body.reserve(serialized_data.size());
        for (auto b : serialized_data) {
            body.push_back(static_cast<char>(b));
        }
        
        // Set headers
        httplib::Headers headers;
        headers.emplace(header_content_type, content_type_json);
        // Let cpp-httplib handle Content-Length automatically
        headers.emplace(header_user_agent, _config.user_agent);
        
        // Determine RPC type for metrics
        std::string rpc_type;
        if (endpoint == endpoint_request_vote) {
            rpc_type = "request_vote";
        } else if (endpoint == endpoint_append_entries) {
            rpc_type = "append_entries";
        } else if (endpoint == endpoint_install_snapshot) {
            rpc_type = "install_snapshot";
        }
        
        // Record request metrics
        auto start_time = std::chrono::steady_clock::now();
        auto metric = _metrics;
        metric.set_metric_name("http.client.request.sent");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("target_node_id", std::to_string(target));
        metric.add_one();
        metric.emit();
        
        metric = _metrics;
        metric.set_metric_name("http.client.request.size");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("target_node_id", std::to_string(target));
        metric.add_value(static_cast<double>(body.size()));
        metric.emit();
        
        // Send POST request
        auto result = client->Post(endpoint.c_str(), headers, body, content_type_json);
        
        // Record latency
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        
        if (!result) {
            // Connection error or timeout
            std::string error_type = "connection_failed";
            if (result.error() == httplib::Error::ConnectionTimeout ||
                result.error() == httplib::Error::Read) {
                error_type = "timeout";
            }
            
            auto error_metric = _metrics;
            error_metric.set_metric_name("http.client.error");
            error_metric.add_dimension("error_type", error_type);
            error_metric.add_dimension("target_node_id", std::to_string(target));
            error_metric.add_one();
            error_metric.emit();
            
            // Record error latency
            auto latency_metric = _metrics;
            latency_metric.set_metric_name("http.client.request.latency");
            latency_metric.add_dimension("rpc_type", rpc_type);
            latency_metric.add_dimension("target_node_id", std::to_string(target));
            latency_metric.add_dimension("status", "error");
            latency_metric.add_duration(latency);
            latency_metric.emit();
            
            if (error_type == "timeout") {
                return make_future_with_exception<Types, Response>(kythira::http_timeout_error(
                    std::format("HTTP request timed out after {}ms", timeout.count())));
            } else {
                return make_future_with_exception<Types, Response>(std::runtime_error(
                    std::format("HTTP request failed: {}", httplib::to_string(result.error()))));
            }
        } else if (result->status == 200) {
            // Success - deserialize response
            try {
                std::vector<std::byte> response_data;
                response_data.reserve(result->body.size());
                for (char c : result->body) {
                    response_data.push_back(static_cast<std::byte>(c));
                }
                
                Response response;
                if constexpr (std::is_same_v<Response, kythira::request_vote_response<>>) {
                    response = _serializer.deserialize_request_vote_response(response_data);
                } else if constexpr (std::is_same_v<Response, kythira::append_entries_response<>>) {
                    response = _serializer.deserialize_append_entries_response(response_data);
                } else if constexpr (std::is_same_v<Response, kythira::install_snapshot_response<>>) {
                    response = _serializer.deserialize_install_snapshot_response(response_data);
                }
                
                // Record response size
                auto size_metric = _metrics;
                size_metric.set_metric_name("http.client.response.size");
                size_metric.add_dimension("rpc_type", rpc_type);
                size_metric.add_dimension("target_node_id", std::to_string(target));
                size_metric.add_value(static_cast<double>(result->body.size()));
                size_metric.emit();
                
                // Record success latency
                auto latency_metric = _metrics;
                latency_metric.set_metric_name("http.client.request.latency");
                latency_metric.add_dimension("rpc_type", rpc_type);
                latency_metric.add_dimension("target_node_id", std::to_string(target));
                latency_metric.add_dimension("status", "success");
                latency_metric.add_duration(latency);
                latency_metric.emit();
                
                return make_future_with_value<Types, Response>(std::move(response));
            } catch (const std::exception& e) {
                // Deserialization error
                auto error_metric = _metrics;
                error_metric.set_metric_name("http.client.error");
                error_metric.add_dimension("error_type", "deserialization_failed");
                error_metric.add_dimension("target_node_id", std::to_string(target));
                error_metric.add_one();
                error_metric.emit();
                
                return make_future_with_exception<Types, Response>(kythira::serialization_error(
                    std::format("Failed to deserialize response: {}", e.what())));
            }
        } else if (result->status >= 400 && result->status < 500) {
            // Client error (4xx)
            auto error_metric = _metrics;
            error_metric.set_metric_name("http.client.error");
            error_metric.add_dimension("error_type", "4xx");
            error_metric.add_dimension("target_node_id", std::to_string(target));
            error_metric.add_one();
            error_metric.emit();
            
            // Record error latency
            auto latency_metric = _metrics;
            latency_metric.set_metric_name("http.client.request.latency");
            latency_metric.add_dimension("rpc_type", rpc_type);
            latency_metric.add_dimension("target_node_id", std::to_string(target));
            latency_metric.add_dimension("status", "error");
            latency_metric.add_duration(latency);
            latency_metric.emit();
            
            return make_future_with_exception<Types, Response>(kythira::http_client_error(
                result->status,
                std::format("HTTP client error {}: {}", result->status, result->body)));
        } else if (result->status >= 500) {
            // Server error (5xx)
            auto error_metric = _metrics;
            error_metric.set_metric_name("http.client.error");
            error_metric.add_dimension("error_type", "5xx");
            error_metric.add_dimension("target_node_id", std::to_string(target));
            error_metric.add_one();
            error_metric.emit();
            
            // Record error latency
            auto latency_metric = _metrics;
            latency_metric.set_metric_name("http.client.request.latency");
            latency_metric.add_dimension("rpc_type", rpc_type);
            latency_metric.add_dimension("target_node_id", std::to_string(target));
            latency_metric.add_dimension("status", "error");
            latency_metric.add_duration(latency);
            latency_metric.emit();
            
            return make_future_with_exception<Types, Response>(kythira::http_server_error(
                result->status,
                std::format("HTTP server error {}: {}", result->status, result->body)));
        } else {
            // Unexpected status code
            return make_future_with_exception<Types, Response>(std::runtime_error(
                std::format("Unexpected HTTP status code: {}", result->status)));
        }
    } catch (const std::exception& e) {
        return make_future_with_exception<Types, Response>(e);
    }
}

// send_request_vote implementation
template<typename Types>
requires transport_types<Types>
auto cpp_httplib_client<Types>::send_request_vote(
    std::uint64_t target,
    const kythira::request_vote_request<>& request,
    std::chrono::milliseconds timeout
) -> typename Types::template future_template<kythira::request_vote_response<>> {
    return send_rpc<kythira::request_vote_request<>, kythira::request_vote_response<>>(
        target, endpoint_request_vote, request, timeout);
}

// send_append_entries implementation
template<typename Types>
requires transport_types<Types>
auto cpp_httplib_client<Types>::send_append_entries(
    std::uint64_t target,
    const kythira::append_entries_request<>& request,
    std::chrono::milliseconds timeout
) -> typename Types::template future_template<kythira::append_entries_response<>> {
    return send_rpc<kythira::append_entries_request<>, kythira::append_entries_response<>>(
        target, endpoint_append_entries, request, timeout);
}

// send_install_snapshot implementation
template<typename Types>
requires transport_types<Types>
auto cpp_httplib_client<Types>::send_install_snapshot(
    std::uint64_t target,
    const kythira::install_snapshot_request<>& request,
    std::chrono::milliseconds timeout
) -> typename Types::template future_template<kythira::install_snapshot_response<>> {
    return send_rpc<kythira::install_snapshot_request<>, kythira::install_snapshot_response<>>(
        target, endpoint_install_snapshot, request, timeout);
}

// Server constructor implementation
template<typename Types>
requires transport_types<Types>
cpp_httplib_server<Types>::cpp_httplib_server(
    std::string bind_address,
    std::uint16_t bind_port,
    cpp_httplib_server_config config,
    typename Types::metrics_type metrics
)
    : _serializer{}
    , _http_server{std::make_unique<httplib::Server>()}
    , _request_vote_handler{}
    , _append_entries_handler{}
    , _install_snapshot_handler{}
    , _bind_address{std::move(bind_address)}
    , _bind_port{bind_port}
    , _config{std::move(config)}
    , _metrics{std::move(metrics)}
    , _running{false}
    , _mutex{}
{
    // Configure server settings
    _http_server->set_payload_max_length(_config.max_request_body_size);
    _http_server->set_read_timeout(_config.request_timeout.count());
    _http_server->set_write_timeout(_config.request_timeout.count());
}

// Server destructor implementation
template<typename Types>
requires transport_types<Types>
cpp_httplib_server<Types>::~cpp_httplib_server() {
    if (_running.load()) {
        stop();
    }
}

// Register RequestVote handler
template<typename Types>
requires transport_types<Types>
auto cpp_httplib_server<Types>::register_request_vote_handler(
    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)> handler
) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _request_vote_handler = std::move(handler);
}

// Register AppendEntries handler
template<typename Types>
requires transport_types<Types>
auto cpp_httplib_server<Types>::register_append_entries_handler(
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)> handler
) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _append_entries_handler = std::move(handler);
}

// Register InstallSnapshot handler
template<typename Types>
requires transport_types<Types>
auto cpp_httplib_server<Types>::register_install_snapshot_handler(
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)> handler
) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _install_snapshot_handler = std::move(handler);
}

// Generic RPC endpoint handler
template<typename Types>
requires transport_types<Types>
template<typename Request, typename Response>
auto cpp_httplib_server<Types>::handle_rpc_endpoint(
    const httplib::Request& http_req,
    httplib::Response& http_resp,
    std::function<Response(const Request&)> handler
) -> void {
    auto start_time = std::chrono::steady_clock::now();
    
    // Determine RPC type for metrics
    std::string rpc_type;
    std::string endpoint = http_req.path;
    if (endpoint == endpoint_request_vote) {
        rpc_type = "request_vote";
    } else if (endpoint == endpoint_append_entries) {
        rpc_type = "append_entries";
    } else if (endpoint == endpoint_install_snapshot) {
        rpc_type = "install_snapshot";
    }
    
    try {
        // Check if handler is registered
        if (!handler) {
            auto error_metric = _metrics;
            error_metric.set_metric_name("http.server.error");
            error_metric.add_dimension("error_type", "handler_not_registered");
            error_metric.add_dimension("endpoint", endpoint);
            error_metric.add_one();
            error_metric.emit();
            
            http_resp.status = 500;
            http_resp.body = "Handler not registered";
            http_resp.set_header(header_content_type, "text/plain");
            // Let cpp-httplib handle Content-Length automatically
            return;
        }
        
        // Record request received metric
        auto metric = _metrics;
        metric.set_metric_name("http.server.request.received");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("endpoint", endpoint);
        metric.add_one();
        metric.emit();
        
        // Record request size
        metric = _metrics;
        metric.set_metric_name("http.server.request.size");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("endpoint", endpoint);
        metric.add_value(static_cast<double>(http_req.body.size()));
        metric.emit();
        
        // Convert request body to bytes
        std::vector<std::byte> request_data;
        request_data.reserve(http_req.body.size());
        for (char c : http_req.body) {
            request_data.push_back(static_cast<std::byte>(c));
        }
        
        // Deserialize request
        Request request;
        if constexpr (std::is_same_v<Request, kythira::request_vote_request<>>) {
            request = _serializer.deserialize_request_vote_request(request_data);
        } else if constexpr (std::is_same_v<Request, kythira::append_entries_request<>>) {
            request = _serializer.deserialize_append_entries_request(request_data);
        } else if constexpr (std::is_same_v<Request, kythira::install_snapshot_request<>>) {
            request = _serializer.deserialize_install_snapshot_request(request_data);
        }
        
        // Invoke handler
        Response response = handler(request);
        
        // Serialize response
        auto serialized_response = _serializer.serialize(response);
        
        // Convert bytes to string for HTTP body
        std::string response_body;
        response_body.reserve(serialized_response.size());
        for (auto b : serialized_response) {
            response_body.push_back(static_cast<char>(b));
        }
        
        // Set response
        http_resp.status = 200;
        http_resp.body = std::move(response_body);
        http_resp.set_header(header_content_type, content_type_json);
        // Let cpp-httplib handle Content-Length automatically
        
        // Record response size
        metric = _metrics;
        metric.set_metric_name("http.server.response.size");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("endpoint", endpoint);
        metric.add_dimension("status_code", "200");
        metric.add_value(static_cast<double>(http_resp.body.size()));
        metric.emit();
        
        // Record success latency
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        
        auto latency_metric = _metrics;
        latency_metric.set_metric_name("http.server.request.latency");
        latency_metric.add_dimension("rpc_type", rpc_type);
        latency_metric.add_dimension("endpoint", endpoint);
        latency_metric.add_dimension("status_code", "200");
        latency_metric.add_duration(latency);
        latency_metric.emit();
        
    } catch (const std::exception& e) {
        // Handle errors
        std::string error_type = "deserialization_failed";
        int status_code = 400;
        std::string error_message = std::format("Bad Request: {}", e.what());
        
        // Check if it's a handler exception
        if (http_req.body.empty() == false) {
            try {
                // Try to deserialize to see if it's a deserialization error
                std::vector<std::byte> test_data;
                test_data.reserve(http_req.body.size());
                for (char c : http_req.body) {
                    test_data.push_back(static_cast<std::byte>(c));
                }
                
                if constexpr (std::is_same_v<Request, kythira::request_vote_request<>>) {
                    _serializer.deserialize_request_vote_request(test_data);
                } else if constexpr (std::is_same_v<Request, kythira::append_entries_request<>>) {
                    _serializer.deserialize_append_entries_request(test_data);
                } else if constexpr (std::is_same_v<Request, kythira::install_snapshot_request<>>) {
                    _serializer.deserialize_install_snapshot_request(test_data);
                }
                
                // If we get here, deserialization worked, so it's a handler exception
                error_type = "handler_exception";
                status_code = 500;
                error_message = "Internal Server Error";
            } catch (...) {
                // Deserialization failed, keep original error type
            }
        }
        
        // Record error metric
        auto error_metric = _metrics;
        error_metric.set_metric_name("http.server.error");
        error_metric.add_dimension("error_type", error_type);
        error_metric.add_dimension("endpoint", endpoint);
        error_metric.add_one();
        error_metric.emit();
        
        // Set error response
        http_resp.status = status_code;
        http_resp.body = error_message;
        http_resp.set_header(header_content_type, "text/plain");
        // Let cpp-httplib handle Content-Length automatically
        
        // Record error latency
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        
        auto latency_metric = _metrics;
        latency_metric.set_metric_name("http.server.request.latency");
        latency_metric.add_dimension("rpc_type", rpc_type);
        latency_metric.add_dimension("endpoint", endpoint);
        latency_metric.add_dimension("status_code", std::to_string(status_code));
        latency_metric.add_duration(latency);
        latency_metric.emit();
    }
}

// Setup endpoints
template<typename Types>
requires transport_types<Types>
auto cpp_httplib_server<Types>::setup_endpoints() -> void {
    // RequestVote endpoint
    _http_server->Post(endpoint_request_vote, [this](const httplib::Request& req, httplib::Response& resp) {
        this->handle_rpc_endpoint<kythira::request_vote_request<>, kythira::request_vote_response<>>(
            req, resp, _request_vote_handler);
    });
    
    // AppendEntries endpoint
    _http_server->Post(endpoint_append_entries, [this](const httplib::Request& req, httplib::Response& resp) {
        this->handle_rpc_endpoint<kythira::append_entries_request<>, kythira::append_entries_response<>>(
            req, resp, _append_entries_handler);
    });
    
    // InstallSnapshot endpoint
    _http_server->Post(endpoint_install_snapshot, [this](const httplib::Request& req, httplib::Response& resp) {
        this->handle_rpc_endpoint<kythira::install_snapshot_request<>, kythira::install_snapshot_response<>>(
            req, resp, _install_snapshot_handler);
    });
}

// Start server
template<typename Types>
requires transport_types<Types>
auto cpp_httplib_server<Types>::start() -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    
    if (_running.load()) {
        return; // Already running
    }
    
    // Setup endpoints
    setup_endpoints();
    
    // Configure TLS if enabled
    if (_config.enable_ssl) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        if (_config.ssl_cert_path.empty() || _config.ssl_key_path.empty()) {
            throw std::runtime_error("SSL certificate and key paths must be provided when SSL is enabled");
        }
        
        // Note: SSL certificate loading method may vary by cpp-httplib version
        // This is a placeholder for SSL configuration
        throw std::runtime_error("SSL configuration not implemented for this cpp-httplib version");
#else
        throw std::runtime_error("SSL support not available (OpenSSL not enabled)");
#endif
    }
    
    // Start server in a separate thread
    _running.store(true);
    
    // Emit server started metric
    auto metric = _metrics;
    metric.set_metric_name("http.server.started");
    metric.add_one();
    metric.emit();
    
    // Start the server in a separate thread
    _server_thread = std::thread([this]() {
        try {
            if (!_http_server->listen(_bind_address.c_str(), _bind_port)) {
                _running.store(false);
            }
        } catch (const std::exception& e) {
            _running.store(false);
        }
    });
    
    // Give the server a moment to start up
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
}

// Stop server
template<typename Types>
requires transport_types<Types>
auto cpp_httplib_server<Types>::stop() -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    
    if (!_running.load()) {
        return; // Already stopped
    }
    
    // Stop the server
    _http_server->stop();
    _running.store(false);
    
    // Wait for server thread to finish
    if (_server_thread.joinable()) {
        _server_thread.join();
    }
    
    // Emit server stopped metric
    auto metric = _metrics;
    metric.set_metric_name("http.server.stopped");
    metric.add_one();
    metric.emit();
}

// Check if server is running
template<typename Types>
requires transport_types<Types>
auto cpp_httplib_server<Types>::is_running() const -> bool {
    return _running.load();
}

} // namespace kythira
