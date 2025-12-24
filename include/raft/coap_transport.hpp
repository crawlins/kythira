#pragma once

#include <raft/types.hpp>
#include <raft/network.hpp>
#include <raft/coap_exceptions.hpp>
#include <raft/metrics.hpp>
#include <raft/logger.hpp>
#include <raft/console_logger.hpp>
#include <raft/json_serializer.hpp>
#include <concepts/future.hpp>

#include <string>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <optional>

#include <raft/future.hpp>

// Forward declarations for libcoap
struct coap_context_t;
struct coap_session_t;
struct coap_pdu_t;
struct coap_resource_t;
struct coap_string_t;
typedef std::uint8_t coap_pdu_code_t;

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

// Block option parsing structure
struct block_option {
    std::uint32_t block_number{0};
    bool more_blocks{false};
    std::uint32_t block_size{1024};
    
    // Parse Block1/Block2 option value
    static auto parse(std::uint32_t option_value) -> block_option {
        block_option result;
        
        // CoAP Block option format (RFC 7959):
        // 0-19 bits: Block number
        // 20-22 bits: Block size (encoded as 2^(SZX+4))
        // 23 bit: More flag
        
        result.block_number = option_value & 0xFFFFF;  // Lower 20 bits
        std::uint8_t szx = (option_value >> 20) & 0x7;  // Bits 20-22
        result.more_blocks = (option_value >> 23) & 0x1;  // Bit 23
        
        // Calculate actual block size from SZX
        result.block_size = 1U << (szx + 4);  // 2^(SZX+4)
        
        return result;
    }
    
    // Encode Block1/Block2 option value
    auto encode() const -> std::uint32_t {
        // Calculate SZX from block size
        std::uint8_t szx = 0;
        std::uint32_t size = block_size;
        while (size > 16 && szx < 7) {
            size >>= 1;
            szx++;
        }
        
        std::uint32_t result = 0;
        result |= (block_number & 0xFFFFF);  // Lower 20 bits
        result |= (static_cast<std::uint32_t>(szx) & 0x7) << 20;  // Bits 20-22
        result |= (more_blocks ? 1U : 0U) << 23;  // Bit 23
        
        return result;
    }
};

namespace kythira {

// Utility functions for CoAP transport configuration and operations
namespace coap_utils {



// Endpoint parsing utilities
struct parsed_endpoint {
    std::string scheme;      // "coap" or "coaps"
    std::string host;        // hostname or IP address
    std::uint16_t port;      // port number
    std::string path;        // resource path (optional)
    
    parsed_endpoint() = default;
    parsed_endpoint(std::string s, std::string h, std::uint16_t p, std::string path = "")
        : scheme(std::move(s)), host(std::move(h)), port(p), path(std::move(path)) {}
};

auto parse_coap_endpoint(const std::string& endpoint) -> parsed_endpoint;
auto format_coap_endpoint(const parsed_endpoint& endpoint) -> std::string;
auto is_valid_coap_endpoint(const std::string& endpoint) -> bool;

// Token generation utilities
auto generate_coap_token() -> std::vector<std::byte>;
auto generate_coap_token(std::size_t length) -> std::vector<std::byte>;
auto is_valid_coap_token(const std::vector<std::byte>& token) -> bool;

// CoAP option handling utilities
enum class coap_content_format : std::uint16_t {
    text_plain = 0,
    application_link_format = 40,
    application_xml = 41,
    application_octet_stream = 42,
    application_exi = 47,
    application_json = 50,
    application_cbor = 60
};

auto get_content_format_for_serializer(const std::string& serializer_name) -> coap_content_format;
auto content_format_to_string(coap_content_format format) -> std::string;
auto parse_content_format(std::uint16_t format_value) -> coap_content_format;

// Block option utilities (already defined above as block_option struct)
auto calculate_block_size_szx(std::size_t block_size) -> std::uint8_t;
auto szx_to_block_size(std::uint8_t szx) -> std::size_t;
auto is_valid_block_size(std::size_t block_size) -> bool;

} // namespace coap_utils

// Client configuration structure
struct coap_client_config {
    std::chrono::milliseconds ack_timeout{2000};
    std::chrono::milliseconds ack_random_factor_ms{1000};
    std::size_t max_retransmit{4};
    std::chrono::milliseconds nstart{1};
    std::chrono::milliseconds default_leisure{5000};
    std::chrono::milliseconds probing_rate{1000};
    
    // DTLS Configuration
    bool enable_dtls{false};
    std::string psk_identity{};
    std::vector<std::byte> psk_key{};
    std::string cert_file{};
    std::string key_file{};
    std::string ca_file{};
    bool verify_peer_cert{true};
    
    // Block Transfer
    std::size_t max_block_size{1024};
    bool enable_block_transfer{true};
    
    // Connection Management
    std::size_t max_sessions{100};
    std::chrono::seconds session_timeout{300};
    
    // Performance Optimizations
    bool enable_session_reuse{true};
    bool enable_connection_pooling{true};
    std::size_t connection_pool_size{50};
    bool enable_concurrent_processing{true};
    std::size_t max_concurrent_requests{100};
    bool enable_memory_optimization{true};
    std::size_t memory_pool_size{1024 * 1024}; // 1MB
    bool enable_serialization_caching{true};
    std::size_t serialization_cache_size{1000};
    
    // Reliable Message Delivery
    bool use_confirmable_messages{true};
    std::chrono::milliseconds retransmission_timeout{2000};
    double exponential_backoff_factor{2.0};
    std::size_t max_retransmissions{4};
};

// Server configuration structure
struct coap_server_config {
    std::size_t max_concurrent_sessions{200};
    std::size_t max_request_size{64 * 1024};  // 64 KB
    std::chrono::seconds session_timeout{300};
    
    // DTLS Configuration
    bool enable_dtls{false};
    std::string psk_identity{};
    std::vector<std::byte> psk_key{};
    std::string cert_file{};
    std::string key_file{};
    std::string ca_file{};
    bool verify_peer_cert{true};
    
    // Block Transfer
    std::size_t max_block_size{1024};
    bool enable_block_transfer{true};
    
    // Multicast Support
    bool enable_multicast{false};
    std::string multicast_address{"224.0.1.187"};
    std::uint16_t multicast_port{5683};
    
    // Performance Optimizations
    bool enable_session_reuse{true};
    bool enable_connection_pooling{true};
    std::size_t connection_pool_size{100};
    bool enable_concurrent_processing{true};
    std::size_t max_concurrent_requests{200};
    bool enable_memory_optimization{true};
    std::size_t memory_pool_size{2 * 1024 * 1024}; // 2MB
    bool enable_serialization_caching{true};
    std::size_t serialization_cache_size{1000};
    
    // Reliable Message Delivery
    bool use_confirmable_messages{true};
    std::chrono::milliseconds retransmission_timeout{2000};
    double exponential_backoff_factor{2.0};
    std::size_t max_retransmissions{4};
};

// CoAP client implementation
template<typename FutureType, typename RPC_Serializer, typename Metrics, typename Logger>
requires 
    raft::rpc_serializer<RPC_Serializer, std::vector<std::byte>> && 
    raft::metrics<Metrics> && 
    raft::diagnostic_logger<Logger>
class coap_client {
public:
    // Constructor
    coap_client(
        std::unordered_map<std::uint64_t, std::string> node_id_to_endpoint_map,
        coap_client_config config,
        Metrics metrics,
        Logger logger
    );

    // Destructor
    ~coap_client();

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

    // Public methods for testing
    auto generate_message_token() -> std::string;
    auto generate_message_id() -> std::uint16_t;
    auto calculate_retransmission_timeout(std::size_t attempt) const -> std::chrono::milliseconds;
    auto is_duplicate_message(std::uint16_t message_id) -> bool;
    auto record_received_message(std::uint16_t message_id) -> void;
    
    // Error handling methods
    auto detect_malformed_message(const std::vector<std::byte>& message_data) -> bool;
    auto handle_resource_exhaustion() -> void;
    auto detect_network_partition(const std::string& endpoint) -> bool;
    auto attempt_network_recovery(const std::string& endpoint) -> bool;
    auto enforce_connection_limits() -> void;
    
    // DTLS methods for testing
    auto establish_dtls_connection(const std::string& endpoint) -> bool;
    auto validate_peer_certificate(const std::string& peer_cert_data) -> bool;
    auto is_dtls_enabled() const -> bool;
    
    // Block transfer methods
    auto should_use_block_transfer(const std::vector<std::byte>& payload) const -> bool;
    auto split_payload_into_blocks(const std::vector<std::byte>& payload) const -> std::vector<std::vector<std::byte>>;
    auto reassemble_blocks(const std::string& token, const std::vector<std::byte>& block_data, const block_option& block_opt) -> std::optional<std::vector<std::byte>>;
    auto cleanup_expired_block_transfers() -> void;
    
    // Performance optimization methods
    auto get_or_create_session(const std::string& endpoint) -> coap_session_t*;
    auto return_session_to_pool(const std::string& endpoint, coap_session_t* session) -> void;
    auto cleanup_expired_sessions() -> void;
    auto acquire_concurrent_slot() -> bool;
    auto release_concurrent_slot() -> void;
    auto allocate_from_pool(std::size_t size) -> std::byte*;
    auto get_cached_serialization(std::size_t hash) -> std::optional<std::vector<std::byte>>;
    auto cache_serialization(std::size_t hash, const std::vector<std::byte>& data) -> void;
    auto cleanup_serialization_cache() -> void;
    
    // Multicast support methods
    auto send_multicast_message(
        const std::string& multicast_address,
        std::uint16_t multicast_port,
        const std::string& resource_path,
        const std::vector<std::byte>& payload,
        std::chrono::milliseconds timeout
    ) -> FutureType;
    
    auto is_valid_multicast_address(const std::string& address) -> bool;
    auto handle_multicast_response(const std::string& token, const std::vector<std::byte>& response_data, const std::string& sender_address) -> void;
    auto finalize_multicast_response_collection(const std::string& token) -> void;
    auto cleanup_expired_multicast_requests() -> void;

private:
    RPC_Serializer _serializer;
    std::unordered_map<std::uint64_t, std::string> _node_id_to_endpoint;
    coap_context_t* _coap_context;
    coap_client_config _config;
    Metrics _metrics;
    Logger _logger;
    
    // Reliable message delivery tracking
    std::unordered_map<std::string, std::unique_ptr<pending_message>> _pending_requests;
    std::unordered_map<std::uint16_t, received_message_info> _received_messages;
    std::atomic<std::uint16_t> _next_message_id{1};
    std::atomic<std::uint64_t> _token_counter{0};
    
    // Block transfer state tracking
    std::unordered_map<std::string, std::unique_ptr<block_transfer_state>> _active_block_transfers;
    
    // Error handling and resource management
    std::atomic<std::size_t> _active_connections{0};
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> _network_partition_detection;
    std::atomic<bool> _resource_exhaustion_detected{false};
    std::chrono::steady_clock::time_point _last_resource_check;
    
    // Performance optimization structures
    struct session_pool_entry {
        coap_session_t* session;
        std::chrono::steady_clock::time_point last_used;
        std::atomic<bool> in_use{false};
        std::string endpoint;
        
        session_pool_entry(coap_session_t* sess, const std::string& ep)
            : session(sess), last_used(std::chrono::steady_clock::now()), endpoint(ep) {}
    };
    
    std::unordered_map<std::string, std::vector<coap_session_t*>> _session_pools;
    std::atomic<std::size_t> _concurrent_requests{0};
    
    // Memory optimization
    struct memory_pool {
        std::vector<std::byte> buffer;
        std::atomic<std::size_t> offset{0};
        std::mutex mutex;
        
        memory_pool(std::size_t size) : buffer(size) {}
        
        auto allocate(std::size_t size) -> std::byte* {
            std::lock_guard<std::mutex> lock(mutex);
            std::size_t current_offset = offset.load();
            if (current_offset + size > buffer.size()) {
                offset = 0; // Reset pool (simple strategy)
                current_offset = 0;
            }
            offset = current_offset + size;
            return buffer.data() + current_offset;
        }
    };
    
    std::unique_ptr<memory_pool> _memory_pool;
    
    // Serialization cache
    struct serialization_cache_entry {
        std::vector<std::byte> serialized_data;
        std::chrono::steady_clock::time_point created;
        std::size_t access_count{0};
    };
    
    std::unordered_map<std::size_t, serialization_cache_entry> _serialization_cache;
    std::chrono::steady_clock::time_point _last_cache_cleanup;
    
    // Multicast support structures
    struct multicast_response {
        std::string sender_address;
        std::vector<std::byte> response_data;
        std::chrono::steady_clock::time_point received_time;
    };
    
    struct multicast_response_collector {
        std::string token;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::milliseconds timeout;
        std::size_t expected_responses;
        std::vector<multicast_response> responses;
        std::function<void(std::vector<std::vector<std::byte>>)> resolve_callback;
        std::function<void(std::exception_ptr)> reject_callback;
    };
    
    std::unordered_map<std::string, std::shared_ptr<multicast_response_collector>> _multicast_requests;
    
    mutable std::mutex _mutex;

    // Helper methods
    auto get_endpoint_uri(std::uint64_t node_id) const -> std::string;
    auto setup_dtls_context() -> void;
    auto handle_response(coap_pdu_t* response, const std::string& token) -> void;
    auto handle_acknowledgment(std::uint16_t message_id) -> void;
    auto retransmit_message(const std::string& token) -> void;
    auto cleanup_expired_messages() -> void;
    
    template<typename Request, typename Response>
    auto send_rpc(
        std::uint64_t target,
        const std::string& resource_path,
        const Request& request,
        std::chrono::milliseconds timeout
    ) -> FutureType;
};

// CoAP server implementation
template<typename FutureType, typename RPC_Serializer, typename Metrics, typename Logger>
requires 
    future<FutureType, raft::request_vote_response<>> &&
    future<FutureType, raft::append_entries_response<>> &&
    future<FutureType, raft::install_snapshot_response<>> &&
    raft::rpc_serializer<RPC_Serializer, std::vector<std::byte>> && 
    raft::metrics<Metrics> && 
    raft::diagnostic_logger<Logger>
class coap_server {
public:
    // Constructor
    coap_server(
        std::string bind_address,
        std::uint16_t bind_port,
        coap_server_config config,
        Metrics metrics,
        Logger logger
    );

    // Destructor
    ~coap_server();

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

    // Public methods for testing
    auto is_duplicate_message(std::uint16_t message_id) -> bool;
    auto record_received_message(std::uint16_t message_id) -> void;
    
    // Error handling methods
    auto detect_malformed_message(const std::vector<std::byte>& message_data) -> bool;
    auto handle_resource_exhaustion() -> void;
    auto enforce_connection_limits() -> void;
    auto reject_malformed_request(coap_pdu_t* response, const std::string& reason) -> void;
    
    // DTLS methods for testing
    auto validate_client_certificate(const std::string& client_cert_data) -> bool;
    auto is_dtls_enabled() const -> bool;
    
    // Block transfer methods
    auto should_use_block_transfer(const std::vector<std::byte>& payload) const -> bool;
    auto split_payload_into_blocks(const std::vector<std::byte>& payload) const -> std::vector<std::vector<std::byte>>;
    auto reassemble_blocks(const std::string& token, const std::vector<std::byte>& block_data, const block_option& block_opt) -> std::optional<std::vector<std::byte>>;
    auto cleanup_expired_block_transfers() -> void;
    
    // Performance optimization methods
    auto acquire_concurrent_slot() -> bool;
    auto release_concurrent_slot() -> void;
    auto allocate_from_pool(std::size_t size) -> std::byte*;
    auto get_cached_serialization(std::size_t hash) -> std::optional<std::vector<std::byte>>;
    auto cache_serialization(std::size_t hash, const std::vector<std::byte>& data) -> void;
    auto cleanup_serialization_cache() -> void;
    
    // Multicast support methods
    auto setup_multicast_listener() -> void;
    auto is_valid_multicast_address(const std::string& address) -> bool;
    auto handle_multicast_message(const std::vector<std::byte>& message_data, const std::string& resource_path, const std::string& sender_address) -> void;
    auto handle_multicast_request_vote(const std::vector<std::byte>& message_data, const std::string& sender_address) -> void;
    auto handle_multicast_append_entries(const std::vector<std::byte>& message_data, const std::string& sender_address) -> void;
    auto handle_multicast_install_snapshot(const std::vector<std::byte>& message_data, const std::string& sender_address) -> void;
    auto send_multicast_response(const std::string& target_address, const std::vector<std::byte>& response_data) -> void;

private:
    RPC_Serializer _serializer;
    coap_context_t* _coap_context;
    std::function<raft::request_vote_response<>(const raft::request_vote_request<>&)> _request_vote_handler;
    std::function<raft::append_entries_response<>(const raft::append_entries_request<>&)> _append_entries_handler;
    std::function<raft::install_snapshot_response<>(const raft::install_snapshot_request<>&)> _install_snapshot_handler;
    std::string _bind_address;
    std::uint16_t _bind_port;
    coap_server_config _config;
    Metrics _metrics;
    Logger _logger;
    std::atomic<bool> _running{false};
    
    // Duplicate detection
    std::unordered_map<std::uint16_t, received_message_info> _received_messages;
    
    // Block transfer state tracking
    std::unordered_map<std::string, std::unique_ptr<block_transfer_state>> _active_block_transfers;
    
    // Error handling and resource management
    std::atomic<std::size_t> _active_connections{0};
    std::atomic<bool> _resource_exhaustion_detected{false};
    std::chrono::steady_clock::time_point _last_resource_check;
    
    // Performance optimization structures
    std::atomic<std::size_t> _concurrent_requests{0};
    
    // Memory optimization
    struct memory_pool {
        std::vector<std::byte> buffer;
        std::atomic<std::size_t> offset{0};
        std::mutex mutex;
        
        memory_pool(std::size_t size) : buffer(size) {}
        
        auto allocate(std::size_t size) -> std::byte* {
            std::lock_guard<std::mutex> lock(mutex);
            std::size_t current_offset = offset.load();
            if (current_offset + size > buffer.size()) {
                offset = 0; // Reset pool (simple strategy)
                current_offset = 0;
            }
            offset = current_offset + size;
            return buffer.data() + current_offset;
        }
    };
    
    std::unique_ptr<memory_pool> _memory_pool;
    
    // Serialization cache
    struct serialization_cache_entry {
        std::vector<std::byte> serialized_data;
        std::chrono::steady_clock::time_point created;
        std::size_t access_count{0};
    };
    
    std::unordered_map<std::size_t, serialization_cache_entry> _serialization_cache;
    std::chrono::steady_clock::time_point _last_cache_cleanup;
    
    mutable std::mutex _mutex;

    // Helper methods
    auto setup_resources() -> void;
    auto setup_dtls_context() -> void;
    auto send_error_response(coap_pdu_t* response, coap_pdu_code_t code, const std::string& message) -> void;
    auto cleanup_expired_messages() -> void;
    
    template<typename Request, typename Response>
    auto handle_rpc_resource(
        coap_resource_t* resource,
        coap_session_t* session,
        const coap_pdu_t* request,
        const coap_string_t* query,
        coap_pdu_t* response,
        std::function<Response(const Request&)> handler
    ) -> void;
};

// Static assertions to verify concept conformance at compile time
// These ensure that the CoAP transport implementations satisfy the required concepts

#ifdef LIBCOAP_AVAILABLE
// Verify that coap_client satisfies network_client concept with valid template parameters
using coap_future_type = kythira::Future<raft::request_vote_response<>>;
static_assert(network_client<coap_client<coap_future_type, raft::json_rpc_serializer<std::vector<std::byte>>, raft::noop_metrics, raft::console_logger>, coap_future_type>, 
              "coap_client must satisfy network_client concept");

// Verify that coap_server satisfies network_server concept with valid template parameters  
static_assert(network_server<coap_server<coap_future_type, raft::json_rpc_serializer<std::vector<std::byte>>, raft::noop_metrics, raft::console_logger>, coap_future_type>, 
              "coap_server must satisfy network_server concept");
#endif

// Verify that the template constraints are properly enforced
static_assert(raft::rpc_serializer<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>, 
              "json_rpc_serializer must satisfy rpc_serializer concept for CoAP transport");

static_assert(raft::metrics<raft::noop_metrics>, 
              "noop_metrics must satisfy metrics concept for CoAP transport");

static_assert(raft::diagnostic_logger<raft::console_logger>, 
              "console_logger must satisfy diagnostic_logger concept for CoAP transport");

} // namespace kythira