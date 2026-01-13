#pragma once

#include <raft/types.hpp>
#include <raft/coap_block_option.hpp>
#include <raft/network.hpp>
#include <raft/coap_exceptions.hpp>
#include <raft/metrics.hpp>
#include <raft/logger.hpp>
#include <raft/console_logger.hpp>
#include <raft/json_serializer.hpp>
#include <concepts/future.hpp>
#include <network_simulator/network_simulator.hpp>

#include <string>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <optional>

#include <raft/future.hpp>
#include <raft/coap_utils.hpp>

// Conditional libcoap includes
#ifdef LIBCOAP_AVAILABLE
#include <coap3/coap.h>
#endif

// Forward declarations for libcoap
struct coap_context_t;
struct coap_session_t;
struct coap_address_t;
struct coap_uri_t;
struct coap_pdu_t;
struct coap_resource_t;
struct coap_string_t;
struct coap_string_t;
typedef std::uint8_t coap_pdu_code_t;

namespace kythira {

// Message tracking structures - using callbacks to work with generic future types
struct pending_message {
    std::string token;
    std::uint16_t message_id;
    std::chrono::steady_clock::time_point send_time;
    std::chrono::milliseconds timeout;
    std::size_t retransmission_count{0};
    std::function<void(std::vector<std::byte>)> resolve_callback;
    std::function<void(std::exception_ptr)> reject_callback;
    std::vector<std::byte> original_payload;
    std::string target_endpoint;
    std::string resource_path;
    bool is_confirmable{true};
    
    pending_message(std::string tok, std::uint16_t msg_id, std::chrono::milliseconds to, 
                   std::function<void(std::vector<std::byte>)> resolve_cb,
                   std::function<void(std::exception_ptr)> reject_cb,
                   std::vector<std::byte> payload,
                   std::string endpoint, std::string path, bool confirmable)
        : token(std::move(tok))
        , message_id(msg_id)
        , send_time(std::chrono::steady_clock::now())
        , timeout(to)
        , resolve_callback(std::move(resolve_cb))
        , reject_callback(std::move(reject_cb))
        , original_payload(std::move(payload))
        , target_endpoint(std::move(endpoint))
        , resource_path(std::move(path))
        , is_confirmable(confirmable)
    {}
};

struct received_message_info {
    std::uint16_t message_id;
    std::chrono::steady_clock::time_point received_time;
    
    received_message_info(std::uint16_t msg_id)
        : message_id(msg_id)
        , received_time(std::chrono::steady_clock::now())
    {}
};

// Block transfer state management
struct block_transfer_state {
    std::string token;
    std::vector<std::byte> complete_payload;
    std::size_t expected_total_size{0};
    std::size_t received_size{0};
    std::uint32_t next_block_num{0};
    std::uint32_t block_size{1024};
    bool is_complete{false};
    std::chrono::steady_clock::time_point last_activity;
    
    block_transfer_state(std::string tok, std::uint32_t blk_size)
        : token(std::move(tok))
        , block_size(blk_size)
        , last_activity(std::chrono::steady_clock::now())
    {}
};

// CoAP error information structure for enhanced error handling
struct coap_error_info {
    coap_pdu_code_t code;
    std::string error_class;
    std::string description;
    bool is_retryable{false};
};

// Note: transport_types concept is defined in types.hpp

// CoAP transport types implementation using folly
template<typename RPC_Serializer, typename Metrics, typename Executor>
struct coap_transport_types {
#ifdef FOLLY_AVAILABLE
    template<typename T> using future_template = folly::Future<T>;
#else
    template<typename T> using future_template = network_simulator::SimpleFuture<T>;
#endif
    using serializer_type = RPC_Serializer;
    using metrics_type = Metrics;
    using executor_type = Executor;
    using logger_type = kythira::console_logger;
};

// Alternative implementations for different future types
template<typename RPC_Serializer, typename Metrics, typename Executor>
struct std_coap_transport_types {
    template<typename T> using future_template = std::future<T>;
    using serializer_type = RPC_Serializer;
    using metrics_type = Metrics;
    using executor_type = Executor;
    using logger_type = kythira::console_logger;
};

// Simple future implementation for when folly is not available
template<typename RPC_Serializer, typename Metrics, typename Executor>
struct simple_coap_transport_types {
    template<typename T> using future_template = network_simulator::SimpleFuture<T>;
    using serializer_type = RPC_Serializer;
    using metrics_type = Metrics;
    using executor_type = Executor;
    using logger_type = kythira::console_logger;
};

// Default transport types implementation (deprecated - use coap_transport_types instead)
template<typename FutureType, typename RPC_Serializer, typename Metrics, typename Logger>
struct default_transport_types {
    using future_type = FutureType;
    using serializer_type = RPC_Serializer;
    using metrics_type = Metrics;
    using logger_type = Logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
};

// Configuration structures
struct coap_client_config {
    bool enable_dtls{false};
    bool enable_block_transfer{true};
    std::size_t max_block_size{1024};
    std::size_t max_sessions{100};
    std::chrono::milliseconds session_timeout{30000};
    std::chrono::milliseconds ack_timeout{2000};
    std::chrono::milliseconds ack_random_factor_ms{1000};
    std::size_t max_retransmit{4};
    bool use_confirmable_messages{true};
    std::size_t max_retransmissions{4};
    std::chrono::milliseconds retransmission_timeout{2000};
    double exponential_backoff_factor{2.0};
    
    // DTLS configuration
    std::string cert_file;
    std::string key_file;
    std::string ca_file;
    std::string psk_identity;
    std::vector<std::byte> psk_key;
    bool verify_peer_cert{true};
    std::vector<std::string> cipher_suites; // Allowed cipher suites for DTLS
    bool enable_session_resumption{true}; // Enable DTLS session resumption
    
    // Multicast configuration
    bool enable_multicast{false};
    std::string multicast_address{"224.0.1.187"};
    std::uint16_t multicast_port{5683};
    
    // Performance optimization settings
    bool enable_session_reuse{true};
    bool enable_connection_pooling{true};
    std::size_t connection_pool_size{10};
    bool enable_concurrent_processing{true};
    std::size_t max_concurrent_requests{50};
    bool enable_memory_optimization{false};
    std::size_t memory_pool_size{1024 * 1024}; // 1MB
    bool enable_serialization_caching{false};
    std::size_t serialization_cache_size{100};
    std::size_t max_cache_entries{100};
    std::chrono::milliseconds cache_ttl{60000}; // 1 minute
    bool enable_certificate_validation{true};
};

struct coap_server_config {
    bool enable_dtls{false};
    bool enable_block_transfer{true};
    std::size_t max_block_size{1024};
    std::size_t max_concurrent_sessions{100};
    std::chrono::milliseconds session_timeout{30000};
    std::size_t max_request_size{65536};
    
    // DTLS configuration
    std::string cert_file;
    std::string key_file;
    std::string ca_file;
    std::string psk_identity;
    std::vector<std::byte> psk_key;
    bool verify_peer_cert{true};
    std::vector<std::string> cipher_suites; // Allowed cipher suites for DTLS
    bool enable_session_resumption{true}; // Enable DTLS session resumption
    
    // Multicast configuration
    bool enable_multicast{false};
    std::string multicast_address{"224.0.1.187"}; // CoAP multicast address
    std::uint16_t multicast_port{5683};
    
    // Performance optimization settings
    bool enable_concurrent_processing{true};
    std::size_t max_concurrent_requests{100};
    bool enable_memory_optimization{false};
    std::size_t memory_pool_size{1024 * 1024}; // 1MB
    bool enable_serialization_caching{false};
    std::size_t serialization_cache_size{100};
};

// Memory pool for optimization
struct memory_pool {
    std::vector<std::byte> buffer;
    std::size_t offset{0};
    std::size_t peak_usage{0};
    std::size_t allocation_count{0};
    std::size_t reset_count{0};
    mutable std::mutex mutex;
    
    explicit memory_pool(std::size_t size) : buffer(size) {}
    
    auto allocate(std::size_t size) -> std::byte* {
        std::lock_guard<std::mutex> lock(mutex);
        
        // Align allocation to 8-byte boundary for better performance
        std::size_t aligned_size = (size + 7) & ~7;
        
        if (offset + aligned_size > buffer.size()) {
            return nullptr; // Pool exhausted
        }
        
        auto* ptr = buffer.data() + offset;
        offset += aligned_size;
        allocation_count++;
        
        // Track peak usage
        if (offset > peak_usage) {
            peak_usage = offset;
        }
        
        return ptr;
    }
    
    auto reset() -> void {
        std::lock_guard<std::mutex> lock(mutex);
        offset = 0;
        reset_count++;
    }
    
    auto get_usage_stats() const -> std::tuple<std::size_t, std::size_t, std::size_t, std::size_t> {
        std::lock_guard<std::mutex> lock(mutex);
        return {offset, peak_usage, allocation_count, reset_count};
    }
    
    auto get_utilization_percentage() const -> double {
        std::lock_guard<std::mutex> lock(mutex);
        if (buffer.empty()) return 0.0;
        return (static_cast<double>(offset) / buffer.size()) * 100.0;
    }
    
    auto is_exhausted() const -> bool {
        std::lock_guard<std::mutex> lock(mutex);
        return offset >= buffer.size();
    }
    
    auto available_space() const -> std::size_t {
        std::lock_guard<std::mutex> lock(mutex);
        return buffer.size() - offset;
    }
};

// Serialization cache entry
struct cache_entry {
    std::vector<std::byte> serialized_data;
    std::chrono::steady_clock::time_point created;
    std::size_t access_count{1};
    std::chrono::steady_clock::time_point last_accessed;
    std::size_t hash_value{0};
    
    cache_entry(std::vector<std::byte> data, std::size_t hash) 
        : serialized_data(std::move(data))
        , created(std::chrono::steady_clock::now())
        , last_accessed(created)
        , hash_value(hash) {}
    
    auto touch() -> void {
        last_accessed = std::chrono::steady_clock::now();
        access_count++;
    }
    
    auto age() const -> std::chrono::milliseconds {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - created);
    }
    
    auto time_since_last_access() const -> std::chrono::milliseconds {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_accessed);
    }
};
    std::chrono::steady_clock::time_point last_access;
    std::size_t access_count{0};
};

// Multicast response structure
struct multicast_response {
    std::string sender_address;
    std::vector<std::byte> response_data;
    std::chrono::steady_clock::time_point received_time;
};

// Multicast response collector
struct multicast_response_collector {
    std::string token;
    std::vector<multicast_response> responses;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::milliseconds timeout;
    std::function<void(std::vector<std::vector<std::byte>>)> resolve_callback;
    std::function<void(std::exception_ptr)> reject_callback;
    
    multicast_response_collector(std::string tok, std::chrono::milliseconds to,
                               std::function<void(std::vector<std::vector<std::byte>>)> resolve_cb,
                               std::function<void(std::exception_ptr)> reject_cb)
        : token(std::move(tok))
        , start_time(std::chrono::steady_clock::now())
        , timeout(to)
        , resolve_callback(std::move(resolve_cb))
        , reject_callback(std::move(reject_cb))
    {}
};

// CoAP client class declaration
template<typename Types>
requires transport_types<Types>
class coap_client {
public:
    template<typename T> using future_template = typename Types::template future_template<T>;
    using serializer_type = typename Types::serializer_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;
    using logger_type = typename Types::logger_type;

    // Constructor and destructor
    coap_client(
        std::unordered_map<std::uint64_t, std::string> node_id_to_endpoint_map,
        coap_client_config config,
        metrics_type metrics
    );
    
    ~coap_client();

    // RPC methods
    auto send_request_vote(
        std::uint64_t target,
        const kythira::request_vote_request<>& request,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}
    ) -> future_template<kythira::request_vote_response<>>;

    auto send_append_entries(
        std::uint64_t target,
        const kythira::append_entries_request<>& request,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}
    ) -> future_template<kythira::append_entries_response<>>;

    auto send_install_snapshot(
        std::uint64_t target,
        const kythira::install_snapshot_request<>& request,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{30000}
    ) -> future_template<kythira::install_snapshot_response<>>;

    // Multicast support
    auto send_multicast_message(
        const std::string& multicast_address,
        std::uint16_t multicast_port,
        const std::string& resource_path,
        const std::vector<std::byte>& payload,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}
    ) -> future_template<std::vector<std::byte>>;
    
    // Enhanced multicast discovery operations
    auto discover_raft_nodes(
        const std::string& multicast_address = "224.0.1.187",
        std::uint16_t multicast_port = 5683,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}
    ) -> future_template<std::vector<std::string>>;
    
    auto send_multicast_heartbeat(
        const std::string& multicast_address = "224.0.1.187",
        std::uint16_t multicast_port = 5683,
        const std::string& node_id = "",
        std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}
    ) -> future_template<std::vector<std::string>>;
    
    // Multicast group management
    auto join_multicast_group(const std::string& multicast_address) -> bool;
    auto leave_multicast_group(const std::string& multicast_address) -> bool;
    auto get_joined_multicast_groups() const -> std::vector<std::string>;

    // DTLS support
    auto is_dtls_enabled() const -> bool;
    auto establish_dtls_connection(const std::string& endpoint) -> bool;
    
    // Certificate validation (exposed for testing)
    auto validate_peer_certificate(const std::string& peer_cert_data) -> bool;
    
    // Multicast helper methods (exposed for testing)
    auto is_valid_multicast_address(const std::string& address) -> bool;
    auto create_discovery_message(const std::string& node_id = "") -> std::vector<std::byte>;
    auto parse_discovery_response(const std::vector<std::byte>& response_data) -> std::optional<std::string>;

private:
    serializer_type _serializer;
    std::unordered_map<std::uint64_t, std::string> _node_id_to_endpoint;
    coap_context_t* _coap_context;
    coap_client_config _config;
    metrics_type _metrics;
    logger_type _logger;
    
    // Message tracking
    std::unordered_map<std::string, std::unique_ptr<pending_message>> _pending_requests;
    std::unordered_map<std::uint16_t, received_message_info> _received_messages;
    std::unordered_map<std::string, std::unique_ptr<block_transfer_state>> _active_block_transfers;
    std::unordered_map<std::string, std::shared_ptr<multicast_response_collector>> _multicast_requests;
    
    // Multicast group management
    std::unordered_set<std::string> _joined_multicast_groups;
    std::unordered_map<std::string, int> _multicast_sockets;
    
    // Session management
    std::unordered_map<std::string, std::vector<coap_session_t*>> _session_pools;
    
    // Performance optimization
    std::unique_ptr<memory_pool> _memory_pool;
    std::unordered_map<std::size_t, cache_entry> _serialization_cache;
    std::atomic<std::size_t> _concurrent_requests{0};
    
    // Network partition detection
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> _network_partition_detection;
    
    // Synchronization
    mutable std::mutex _mutex;
    std::atomic<std::uint64_t> _token_counter{1};
    std::atomic<std::uint16_t> _next_message_id{1};

    // Helper methods
    template<typename Request, typename Response>
    auto send_rpc(
        std::uint64_t target,
        const std::string& resource_path,
        const Request& request,
        std::chrono::milliseconds timeout
    ) -> future_template<Response>;

    auto get_endpoint_uri(std::uint64_t node_id) const -> std::string;
    auto generate_message_token() -> std::string;
    auto generate_message_id() -> std::uint16_t;
    auto setup_dtls_context() -> void;
    auto handle_response(coap_pdu_t* response, const std::string& token) -> void;
    auto handle_acknowledgment(std::uint16_t message_id) -> void;
    auto is_duplicate_message(std::uint16_t message_id) -> bool;
    auto record_received_message(std::uint16_t message_id) -> void;
    auto retransmit_message(const std::string& token) -> void;
    auto cleanup_expired_messages() -> void;
    auto calculate_retransmission_timeout(std::size_t attempt) const -> std::chrono::milliseconds;
    
    // Block transfer methods
    auto should_use_block_transfer(const std::vector<std::byte>& payload) const -> bool;
    auto split_payload_into_blocks(const std::vector<std::byte>& payload) const -> std::vector<std::vector<std::byte>>;
    auto reassemble_blocks(const std::string& token, const std::vector<std::byte>& block_data, const kythira::block_option& block_opt) -> std::optional<std::vector<std::byte>>;
    auto cleanup_expired_block_transfers() -> void;
    
    // Session management methods
    auto get_or_create_session(const std::string& endpoint) -> coap_session_t*;
    auto get_or_create_session(std::uint64_t target, coap_address_t* dst_addr, coap_uri_t* uri) -> coap_session_t*;
    auto create_new_session(coap_address_t* dst_addr, coap_uri_t* uri) -> coap_session_t*;
    auto return_session_to_pool(const std::string& endpoint, coap_session_t* session) -> void;
    auto cleanup_expired_sessions() -> void;
    
    // Error handling methods
    auto handle_resource_exhaustion() -> void;
    auto enforce_connection_limits() -> void;
    auto detect_malformed_message(const std::vector<std::byte>& data) -> bool;
    auto detect_network_partition(const std::string& endpoint) -> bool;
    auto attempt_network_recovery(const std::string& endpoint) -> bool;
    
    // Performance optimization methods
    auto allocate_from_pool(std::size_t size) -> std::byte*;
    auto get_cached_serialization(std::size_t hash) -> std::optional<std::vector<std::byte>>;
    auto cache_serialization(std::size_t hash, const std::vector<std::byte>& data) -> void;
    auto cleanup_serialization_cache() -> void;
    auto acquire_concurrent_slot() -> bool;
    auto release_concurrent_slot() -> void;
    
    // Enhanced serialization caching methods
    template<typename Request>
    auto get_cached_or_serialize(const Request& request) -> std::vector<std::uint8_t>;
    auto cache_serialization_result(std::size_t hash, const std::vector<std::uint8_t>& data) -> void;
    
    // Enhanced response handling methods
    auto validate_response_pdu(coap_pdu_t* response) -> bool;
    auto map_coap_error_code(coap_pdu_code_t code) -> coap_error_info;
    auto should_retry_on_error(const coap_error_info& error_info, std::size_t attempt_count) -> bool;
    auto handle_response_timeout(const std::string& token) -> void;
    auto correlate_response_with_request(const std::string& token, coap_pdu_t* response) -> bool;
    
    // Enhanced multicast methods
    auto handle_multicast_response(const std::string& token, const std::vector<std::byte>& response_data, const std::string& sender_address) -> void;
    auto finalize_multicast_response_collection(const std::string& token) -> void;
    auto cleanup_expired_multicast_requests() -> void;
    auto handle_multicast_error(const std::string& token, const std::exception_ptr& error) -> void;
};

// CoAP server class declaration
template<typename Types>
requires transport_types<Types>
class coap_server {
public:
    template<typename T> using future_template = typename Types::template future_template<T>;
    using serializer_type = typename Types::serializer_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;
    using logger_type = typename Types::logger_type;
    using address_type = std::string;
    using port_type = std::uint16_t;

    // Constructor and destructor
    coap_server(
        std::string bind_address,
        std::uint16_t bind_port,
        coap_server_config config,
        metrics_type metrics
    );
    
    ~coap_server();

    // Handler registration
    auto register_request_vote_handler(
        std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)> handler
    ) -> void;

    auto register_append_entries_handler(
        std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)> handler
    ) -> void;

    auto register_install_snapshot_handler(
        std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)> handler
    ) -> void;

    // Server lifecycle
    auto start() -> void;
    auto stop() -> void;
    auto is_running() const -> bool;
    
    // DTLS support
    auto is_dtls_enabled() const -> bool;
    
    // Certificate validation (exposed for testing)
    auto validate_client_certificate(const std::string& client_cert_data) -> bool;

private:
    serializer_type _serializer;
    coap_context_t* _coap_context;
    address_type _bind_address;
    port_type _bind_port;
    coap_server_config _config;
    metrics_type _metrics;
    logger_type _logger;
    
    // Server state
    std::atomic<bool> _running{false};
    std::atomic<std::size_t> _active_connections{0};
    std::atomic<std::size_t> _concurrent_requests{0};
    
    // Message tracking
    std::unordered_map<std::uint16_t, received_message_info> _received_messages;
    std::unordered_map<std::string, std::unique_ptr<block_transfer_state>> _active_block_transfers;
    std::unordered_set<std::string> _multicast_groups;
    
    // Resource exhaustion management
    std::atomic<bool> _resource_exhaustion_mode{false};
    std::chrono::steady_clock::time_point _resource_exhaustion_start_time;
    
    // Performance optimization
    std::unique_ptr<memory_pool> _memory_pool;
    std::unordered_map<std::size_t, cache_entry> _serialization_cache;
    
    // RPC handlers
    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)> _request_vote_handler;
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)> _append_entries_handler;
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)> _install_snapshot_handler;
    
    // Synchronization
    mutable std::mutex _mutex;

    // Helper methods
    auto setup_resources() -> void;
    auto setup_dtls_context() -> void;
    auto send_error_response(coap_pdu_t* response, coap_pdu_code_t code, const std::string& message) -> void;
    auto is_duplicate_message(std::uint16_t message_id) -> bool;
    auto record_received_message(std::uint16_t message_id) -> void;
    auto cleanup_expired_messages() -> void;
    
    // Resource handler template
    template<typename Request, typename Response>
    auto handle_rpc_resource(
        coap_resource_t* resource,
        coap_session_t* session,
        const coap_pdu_t* request,
        const coap_string_t* query,
        coap_pdu_t* response,
        std::function<Response(const Request&)> handler
    ) -> void;
    
    // Block transfer methods
    auto should_use_block_transfer(const std::vector<std::byte>& payload) const -> bool;
    auto split_payload_into_blocks(const std::vector<std::byte>& payload) const -> std::vector<std::vector<std::byte>>;
    auto reassemble_blocks(const std::string& token, const std::vector<std::byte>& block_data, const kythira::block_option& block_opt) -> std::optional<std::vector<std::byte>>;
    auto cleanup_expired_block_transfers() -> void;
    auto reject_malformed_request(coap_pdu_t* response, const std::string& reason) -> void;
    
    // Error handling methods
    auto handle_resource_exhaustion() -> void;
    auto enforce_connection_limits() -> void;
    auto detect_malformed_message(const std::vector<std::byte>& data) -> bool;
    
    // Performance optimization methods
    auto allocate_from_pool(std::size_t size) -> std::byte*;
    auto get_cached_serialization(std::size_t hash) -> std::optional<std::vector<std::byte>>;
    auto cache_serialization(std::size_t hash, const std::vector<std::byte>& data) -> void;
    auto cleanup_serialization_cache() -> void;
    auto acquire_concurrent_slot() -> bool;
    auto release_concurrent_slot() -> void;
    
    // Multicast methods
    auto setup_multicast_listener() -> void;
    auto is_valid_multicast_address(const std::string& address) -> bool;
    auto handle_multicast_message(const std::vector<std::byte>& message_data, const std::string& resource_path, const std::string& sender_address) -> void;
    auto handle_multicast_request_vote(const std::vector<std::byte>& message_data, const std::string& sender_address) -> void;
    auto handle_multicast_append_entries(const std::vector<std::byte>& message_data, const std::string& sender_address) -> void;
    auto handle_multicast_install_snapshot(const std::vector<std::byte>& message_data, const std::string& sender_address) -> void;
    auto send_multicast_response(const std::string& target_address, const std::vector<std::byte>& response_data) -> void;
    auto cleanup_expired_multicast_groups() -> void;
};

} // namespace kythira