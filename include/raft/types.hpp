#pragma once

#include <concepts/future.hpp>
#include <raft/metrics.hpp>
#include <concepts>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <cstddef>
#include <ranges>
#include <chrono>
#include <ostream>

// Forward declarations for kythira future types
namespace kythira {
    template<typename T> class Future;
    template<typename T> class Promise;
    template<typename T> class Try;
}

namespace kythira {

// Node identifier concept - can be any unsigned integer or string
template<typename T>
concept node_id = std::unsigned_integral<T> || std::same_as<T, std::string>;

// Term number concept (monotonically increasing unsigned integer)
template<typename T>
concept term_id = std::unsigned_integral<T>;

// Log index concept (1-based, monotonically increasing unsigned integer)
template<typename T>
concept log_index = std::unsigned_integral<T>;

// Forward declaration of test state machine (after log_index concept is defined)
template<typename LogIndex> requires log_index<LogIndex> class test_key_value_state_machine;

// Server states
enum class server_state : std::uint8_t {
    follower,
    candidate,
    leader
};

// Output operator for server_state (for testing and logging)
inline auto operator<<(std::ostream& os, server_state state) -> std::ostream& {
    switch (state) {
        case server_state::follower:
            return os << "follower";
        case server_state::candidate:
            return os << "candidate";
        case server_state::leader:
            return os << "leader";
        default:
            return os << "unknown";
    }
}

// Log entry concept
template<typename T, typename TermId, typename LogIndex>
concept log_entry_type = requires(const T& entry) {
    requires term_id<TermId>;
    requires log_index<LogIndex>;
    { entry.term() } -> std::same_as<TermId>;
    { entry.index() } -> std::same_as<LogIndex>;
    { entry.command() } -> std::same_as<const std::vector<std::byte>&>;
};

// Default log entry implementation (defined before use in append_entries_request)
template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
requires term_id<TermId> && log_index<LogIndex>
struct log_entry {
    TermId _term;
    LogIndex _index;
    std::vector<std::byte> _command;
    
    auto term() const -> TermId { return _term; }
    auto index() const -> LogIndex { return _index; }
    auto command() const -> const std::vector<std::byte>& { return _command; }
};

// Cluster configuration concept
template<typename T, typename NodeId>
concept cluster_configuration_type = requires(const T& config) {
    requires node_id<NodeId>;
    { config.nodes() } -> std::same_as<const std::vector<NodeId>&>;
    { config.is_joint_consensus() } -> std::convertible_to<bool>;
    { config.old_nodes() } -> std::same_as<const std::optional<std::vector<NodeId>>&>;
};

// Default cluster configuration implementation
template<typename NodeId = std::uint64_t>
requires node_id<NodeId>
struct cluster_configuration {
    std::vector<NodeId> _nodes;
    bool _is_joint_consensus;
    std::optional<std::vector<NodeId>> _old_nodes;
    
    auto nodes() const -> const std::vector<NodeId>& { return _nodes; }
    auto is_joint_consensus() const -> bool { return _is_joint_consensus; }
    auto old_nodes() const -> const std::optional<std::vector<NodeId>>& { return _old_nodes; }
};

// Snapshot concept
template<typename T, typename NodeId, typename TermId, typename LogIndex>
concept snapshot_type = requires(const T& snap) {
    requires node_id<NodeId>;
    requires term_id<TermId>;
    requires log_index<LogIndex>;
    { snap.last_included_index() } -> std::same_as<LogIndex>;
    { snap.last_included_term() } -> std::same_as<TermId>;
    { snap.configuration() } -> std::same_as<const cluster_configuration<NodeId>&>;
    { snap.state_machine_state() } -> std::same_as<const std::vector<std::byte>&>;
};

// State machine concept
// Defines the interface for application-specific state machines that execute committed log entries
// Requirements: 1.1, 7.4, 10.1-10.4, 15.2, 19.1-19.5, 31.1-31.2
template<typename SM, typename LogIndex>
concept state_machine = requires(
    SM& sm,
    const SM& const_sm,
    const std::vector<std::byte>& command,
    const std::vector<std::byte>& snapshot_data,
    LogIndex index
) {
    requires log_index<LogIndex>;
    
    // Apply a committed log entry to the state machine
    // Returns the result of applying the command (may be empty for some commands)
    // May throw exceptions if the command cannot be applied
    // Requirements: 7.4, 15.2, 19.1-19.5
    { sm.apply(command, index) } -> std::same_as<std::vector<std::byte>>;
    
    // Get the current state of the state machine for snapshot creation
    // Returns a serialized representation of the entire state machine state
    // Requirements: 10.1-10.4, 31.1-31.2
    { const_sm.get_state() } -> std::same_as<std::vector<std::byte>>;
    
    // Restore the state machine from a snapshot
    // Replaces the entire state machine state with the provided snapshot data
    // May throw exceptions if the snapshot data is invalid or corrupted
    // Requirements: 10.1-10.4, 31.1-31.2
    { sm.restore_from_snapshot(snapshot_data, index) } -> std::same_as<void>;
};

// Default snapshot implementation
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
struct snapshot {
    LogIndex _last_included_index;
    TermId _last_included_term;
    cluster_configuration<NodeId> _configuration;
    std::vector<std::byte> _state_machine_state;
    
    auto last_included_index() const -> LogIndex { return _last_included_index; }
    auto last_included_term() const -> TermId { return _last_included_term; }
    auto configuration() const -> const cluster_configuration<NodeId>& { return _configuration; }
    auto state_machine_state() const -> const std::vector<std::byte>& { return _state_machine_state; }
};

// RequestVote RPC Request concept
template<typename T, typename NodeId, typename TermId, typename LogIndex>
concept request_vote_request_type = requires(const T& req) {
    requires node_id<NodeId>;
    requires term_id<TermId>;
    requires log_index<LogIndex>;
    { req.term() } -> std::same_as<TermId>;
    { req.candidate_id() } -> std::same_as<NodeId>;
    { req.last_log_index() } -> std::same_as<LogIndex>;
    { req.last_log_term() } -> std::same_as<TermId>;
};

// RequestVote RPC Response concept
template<typename T, typename TermId>
concept request_vote_response_type = requires(const T& resp) {
    requires term_id<TermId>;
    { resp.term() } -> std::same_as<TermId>;
    { resp.vote_granted() } -> std::convertible_to<bool>;
};

// AppendEntries RPC Request concept
template<typename T, typename NodeId, typename TermId, typename LogIndex, typename LogEntry>
concept append_entries_request_type = requires(const T& req) {
    requires node_id<NodeId>;
    requires term_id<TermId>;
    requires log_index<LogIndex>;
    requires log_entry_type<LogEntry, TermId, LogIndex>;
    { req.term() } -> std::same_as<TermId>;
    { req.leader_id() } -> std::same_as<NodeId>;
    { req.prev_log_index() } -> std::same_as<LogIndex>;
    { req.prev_log_term() } -> std::same_as<TermId>;
    { req.entries() } -> std::same_as<const std::vector<LogEntry>&>;
    { req.leader_commit() } -> std::same_as<LogIndex>;
};

// AppendEntries RPC Response concept
template<typename T, typename TermId, typename LogIndex>
concept append_entries_response_type = requires(const T& resp) {
    requires term_id<TermId>;
    requires log_index<LogIndex>;
    { resp.term() } -> std::same_as<TermId>;
    { resp.success() } -> std::convertible_to<bool>;
    { resp.conflict_index() } -> std::same_as<std::optional<LogIndex>>;
    { resp.conflict_term() } -> std::same_as<std::optional<TermId>>;
};

// InstallSnapshot RPC Request concept
template<typename T, typename NodeId, typename TermId, typename LogIndex>
concept install_snapshot_request_type = requires(const T& req) {
    requires node_id<NodeId>;
    requires term_id<TermId>;
    requires log_index<LogIndex>;
    { req.term() } -> std::same_as<TermId>;
    { req.leader_id() } -> std::same_as<NodeId>;
    { req.last_included_index() } -> std::same_as<LogIndex>;
    { req.last_included_term() } -> std::same_as<TermId>;
    { req.offset() } -> std::same_as<std::size_t>;
    { req.data() } -> std::same_as<const std::vector<std::byte>&>;
    { req.done() } -> std::convertible_to<bool>;
};

// InstallSnapshot RPC Response concept
template<typename T, typename TermId>
concept install_snapshot_response_type = requires(const T& resp) {
    requires term_id<TermId>;
    { resp.term() } -> std::same_as<TermId>;
};

// Default RequestVote RPC Request implementation
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
struct request_vote_request {
    TermId _term;
    NodeId _candidate_id;
    LogIndex _last_log_index;
    TermId _last_log_term;
    
    auto term() const -> TermId { return _term; }
    auto candidate_id() const -> NodeId { return _candidate_id; }
    auto last_log_index() const -> LogIndex { return _last_log_index; }
    auto last_log_term() const -> TermId { return _last_log_term; }
};

// Default RequestVote RPC Response implementation
template<typename TermId = std::uint64_t>
requires term_id<TermId>
struct request_vote_response {
    TermId _term;
    bool _vote_granted;
    
    auto term() const -> TermId { return _term; }
    auto vote_granted() const -> bool { return _vote_granted; }
};

// Default AppendEntries RPC Request implementation
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t, typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex> && log_entry_type<LogEntry, TermId, LogIndex>
struct append_entries_request {
    TermId _term;
    NodeId _leader_id;
    LogIndex _prev_log_index;
    TermId _prev_log_term;
    std::vector<LogEntry> _entries;
    LogIndex _leader_commit;
    
    auto term() const -> TermId { return _term; }
    auto leader_id() const -> NodeId { return _leader_id; }
    auto prev_log_index() const -> LogIndex { return _prev_log_index; }
    auto prev_log_term() const -> TermId { return _prev_log_term; }
    auto entries() const -> const std::vector<LogEntry>& { return _entries; }
    auto leader_commit() const -> LogIndex { return _leader_commit; }
};

// Default AppendEntries RPC Response implementation
template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
requires term_id<TermId> && log_index<LogIndex>
struct append_entries_response {
    TermId _term;
    bool _success;
    std::optional<LogIndex> _conflict_index;
    std::optional<TermId> _conflict_term;
    
    auto term() const -> TermId { return _term; }
    auto success() const -> bool { return _success; }
    auto conflict_index() const -> std::optional<LogIndex> { return _conflict_index; }
    auto conflict_term() const -> std::optional<TermId> { return _conflict_term; }
};

// Default InstallSnapshot RPC Request implementation
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
struct install_snapshot_request {
    TermId _term;
    NodeId _leader_id;
    LogIndex _last_included_index;
    TermId _last_included_term;
    std::size_t _offset;
    std::vector<std::byte> _data;
    bool _done;
    
    auto term() const -> TermId { return _term; }
    auto leader_id() const -> NodeId { return _leader_id; }
    auto last_included_index() const -> LogIndex { return _last_included_index; }
    auto last_included_term() const -> TermId { return _last_included_term; }
    auto offset() const -> std::size_t { return _offset; }
    auto data() const -> const std::vector<std::byte>& { return _data; }
    auto done() const -> bool { return _done; }
};

// Default InstallSnapshot RPC Response implementation
template<typename TermId = std::uint64_t>
requires term_id<TermId>
struct install_snapshot_response {
    TermId _term;
    
    auto term() const -> TermId { return _term; }
};

// Serialized data concept - must be a range of std::byte
template<typename T>
concept serialized_data = std::ranges::range<T> && 
    std::same_as<std::ranges::range_value_t<T>, std::byte>;

// RPC serializer concept - simplified to avoid circular dependency issues
template<typename S, typename Data>
concept rpc_serializer = requires {
    // Data must satisfy serialized_data concept
    requires serialized_data<Data>;
    
    // The serializer must be a valid type
    typename S;
};

// Retry policy configuration for different RPC operations
struct retry_policy_config {
    std::chrono::milliseconds initial_delay{100};
    std::chrono::milliseconds max_delay{5000};
    double backoff_multiplier{2.0};
    double jitter_factor{0.1};
    std::size_t max_attempts{5};
    
    // Validation method
    auto is_valid() const -> bool {
        return initial_delay > std::chrono::milliseconds{0} &&
               max_delay >= initial_delay &&
               backoff_multiplier > 1.0 &&
               jitter_factor >= 0.0 && jitter_factor <= 1.0 &&
               max_attempts > 0;
    }
};

// Adaptive timeout configuration
struct adaptive_timeout_config {
    bool enabled{false};
    std::chrono::milliseconds min_timeout{50};
    std::chrono::milliseconds max_timeout{10000};
    double adaptation_factor{1.2};
    std::size_t sample_window_size{10};
    
    auto is_valid() const -> bool {
        return min_timeout > std::chrono::milliseconds{0} &&
               max_timeout >= min_timeout &&
               adaptation_factor > 1.0 &&
               sample_window_size > 0;
    }
};

// Raft configuration concept
template<typename T>
concept raft_configuration_type = requires(const T& config) {
    { config.election_timeout_min() } -> std::same_as<std::chrono::milliseconds>;
    { config.election_timeout_max() } -> std::same_as<std::chrono::milliseconds>;
    { config.heartbeat_interval() } -> std::same_as<std::chrono::milliseconds>;
    { config.rpc_timeout() } -> std::same_as<std::chrono::milliseconds>;
    { config.append_entries_timeout() } -> std::same_as<std::chrono::milliseconds>;
    { config.request_vote_timeout() } -> std::same_as<std::chrono::milliseconds>;
    { config.install_snapshot_timeout() } -> std::same_as<std::chrono::milliseconds>;
    { config.max_entries_per_append() } -> std::same_as<std::size_t>;
    { config.snapshot_threshold_bytes() } -> std::same_as<std::size_t>;
    { config.snapshot_chunk_size() } -> std::same_as<std::size_t>;
    { config.heartbeat_retry_policy() } -> std::same_as<const retry_policy_config&>;
    { config.append_entries_retry_policy() } -> std::same_as<const retry_policy_config&>;
    { config.request_vote_retry_policy() } -> std::same_as<const retry_policy_config&>;
    { config.install_snapshot_retry_policy() } -> std::same_as<const retry_policy_config&>;
    { config.get_adaptive_timeout_config() } -> std::same_as<const adaptive_timeout_config&>;
    { config.validate() } -> std::same_as<bool>;
    { config.get_validation_errors() } -> std::same_as<std::vector<std::string>>;
};

// Application failure handling policy
enum class application_failure_policy : std::uint8_t {
    halt,    // Stop applying further entries on failure (safe default)
    retry,   // Retry application with exponential backoff
    skip     // Skip failed entry and continue (dangerous - can lead to inconsistency)
};

// Default raft configuration implementation
struct raft_configuration {
    // Basic timing configuration
    std::chrono::milliseconds _election_timeout_min{150};
    std::chrono::milliseconds _election_timeout_max{300};
    std::chrono::milliseconds _heartbeat_interval{50};
    std::chrono::milliseconds _rpc_timeout{100};
    
    // RPC-specific timeouts
    std::chrono::milliseconds _append_entries_timeout{5000};
    std::chrono::milliseconds _request_vote_timeout{2000};
    std::chrono::milliseconds _install_snapshot_timeout{30000};
    
    // Log and snapshot configuration
    std::size_t _max_entries_per_append{100};
    std::size_t _snapshot_threshold_bytes{10'000'000};
    std::size_t _snapshot_chunk_size{1'000'000};
    
    // Retry policies for different RPC operations
    retry_policy_config _heartbeat_retry_policy{
        .initial_delay = std::chrono::milliseconds{50},
        .max_delay = std::chrono::milliseconds{1000},
        .backoff_multiplier = 1.5,
        .jitter_factor = 0.1,
        .max_attempts = 3
    };
    
    retry_policy_config _append_entries_retry_policy{
        .initial_delay = std::chrono::milliseconds{100},
        .max_delay = std::chrono::milliseconds{5000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.1,
        .max_attempts = 5
    };
    
    retry_policy_config _request_vote_retry_policy{
        .initial_delay = std::chrono::milliseconds{100},
        .max_delay = std::chrono::milliseconds{2000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.1,
        .max_attempts = 3
    };
    
    retry_policy_config _install_snapshot_retry_policy{
        .initial_delay = std::chrono::milliseconds{500},
        .max_delay = std::chrono::milliseconds{30000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.1,
        .max_attempts = 10
    };
    
    // Adaptive timeout configuration
    adaptive_timeout_config _adaptive_timeout_config{
        .enabled = false,
        .min_timeout = std::chrono::milliseconds{50},
        .max_timeout = std::chrono::milliseconds{10000},
        .adaptation_factor = 1.2,
        .sample_window_size = 10
    };
    
    // Application failure handling configuration
    application_failure_policy _application_failure_policy{application_failure_policy::halt};
    std::size_t _application_retry_max_attempts{3};
    std::chrono::milliseconds _application_retry_initial_delay{100};
    std::chrono::milliseconds _application_retry_max_delay{5000};
    double _application_retry_backoff_multiplier{2.0};
    
    // Accessor methods
    auto election_timeout_min() const -> std::chrono::milliseconds { return _election_timeout_min; }
    auto election_timeout_max() const -> std::chrono::milliseconds { return _election_timeout_max; }
    auto heartbeat_interval() const -> std::chrono::milliseconds { return _heartbeat_interval; }
    auto rpc_timeout() const -> std::chrono::milliseconds { return _rpc_timeout; }
    auto append_entries_timeout() const -> std::chrono::milliseconds { return _append_entries_timeout; }
    auto request_vote_timeout() const -> std::chrono::milliseconds { return _request_vote_timeout; }
    auto install_snapshot_timeout() const -> std::chrono::milliseconds { return _install_snapshot_timeout; }
    auto max_entries_per_append() const -> std::size_t { return _max_entries_per_append; }
    auto snapshot_threshold_bytes() const -> std::size_t { return _snapshot_threshold_bytes; }
    auto snapshot_chunk_size() const -> std::size_t { return _snapshot_chunk_size; }
    auto heartbeat_retry_policy() const -> const retry_policy_config& { return _heartbeat_retry_policy; }
    auto append_entries_retry_policy() const -> const retry_policy_config& { return _append_entries_retry_policy; }
    auto request_vote_retry_policy() const -> const retry_policy_config& { return _request_vote_retry_policy; }
    auto install_snapshot_retry_policy() const -> const retry_policy_config& { return _install_snapshot_retry_policy; }
    auto get_adaptive_timeout_config() const -> const adaptive_timeout_config& { return _adaptive_timeout_config; }
    auto get_application_failure_policy() const -> application_failure_policy { return _application_failure_policy; }
    auto application_retry_max_attempts() const -> std::size_t { return _application_retry_max_attempts; }
    auto application_retry_initial_delay() const -> std::chrono::milliseconds { return _application_retry_initial_delay; }
    auto application_retry_max_delay() const -> std::chrono::milliseconds { return _application_retry_max_delay; }
    auto application_retry_backoff_multiplier() const -> double { return _application_retry_backoff_multiplier; }
    
    // Configuration validation
    auto validate() const -> bool {
        return get_validation_errors().empty();
    }
    
    auto get_validation_errors() const -> std::vector<std::string> {
        std::vector<std::string> errors;
        
        // Validate basic timing constraints
        if (_election_timeout_min <= std::chrono::milliseconds{0}) {
            errors.push_back("election_timeout_min must be positive");
        }
        
        if (_election_timeout_max <= _election_timeout_min) {
            errors.push_back("election_timeout_max must be greater than election_timeout_min");
        }
        
        if (_heartbeat_interval <= std::chrono::milliseconds{0}) {
            errors.push_back("heartbeat_interval must be positive");
        }
        
        // Validate heartbeat interval compatibility with election timeout
        // Heartbeat interval should be significantly smaller than election timeout
        if (_heartbeat_interval > _election_timeout_min / 3) {
            errors.push_back("heartbeat_interval should be less than or equal to election_timeout_min/3 to prevent false timeouts");
        }
        
        // Validate RPC timeouts
        if (_rpc_timeout <= std::chrono::milliseconds{0}) {
            errors.push_back("rpc_timeout must be positive");
        }
        
        if (_append_entries_timeout <= std::chrono::milliseconds{0}) {
            errors.push_back("append_entries_timeout must be positive");
        }
        
        if (_request_vote_timeout <= std::chrono::milliseconds{0}) {
            errors.push_back("request_vote_timeout must be positive");
        }
        
        if (_install_snapshot_timeout <= std::chrono::milliseconds{0}) {
            errors.push_back("install_snapshot_timeout must be positive");
        }
        
        // Validate retry policies
        if (!_heartbeat_retry_policy.is_valid()) {
            errors.push_back("heartbeat_retry_policy is invalid");
        }
        
        if (!_append_entries_retry_policy.is_valid()) {
            errors.push_back("append_entries_retry_policy is invalid");
        }
        
        if (!_request_vote_retry_policy.is_valid()) {
            errors.push_back("request_vote_retry_policy is invalid");
        }
        
        if (!_install_snapshot_retry_policy.is_valid()) {
            errors.push_back("install_snapshot_retry_policy is invalid");
        }
        
        // Validate adaptive timeout configuration
        if (!_adaptive_timeout_config.is_valid()) {
            errors.push_back("adaptive_timeout_config is invalid");
        }
        
        // Validate size parameters
        if (_max_entries_per_append == 0) {
            errors.push_back("max_entries_per_append must be positive");
        }
        
        if (_snapshot_threshold_bytes == 0) {
            errors.push_back("snapshot_threshold_bytes must be positive");
        }
        
        if (_snapshot_chunk_size == 0) {
            errors.push_back("snapshot_chunk_size must be positive");
        }
        
        if (_snapshot_chunk_size > _snapshot_threshold_bytes) {
            errors.push_back("snapshot_chunk_size should not exceed snapshot_threshold_bytes");
        }
        
        return errors;
    }
};

// ============================================================================
// Transport Types Concept System
// ============================================================================

// Transport types concept - defines the interface for a unified types parameter
// Used by HTTP and CoAP transport implementations
template<typename T>
concept transport_types = requires {
    // RPC serializer type
    typename T::serializer_type;
    
    // Metrics type
    typename T::metrics_type;
    
    // Executor type (optional for some transports)
    typename T::executor_type;
} && 
    // Validate that the types satisfy their respective concepts
    kythira::rpc_serializer<typename T::serializer_type, std::vector<std::byte>> &&
    kythira::metrics<typename T::metrics_type> &&
    // Future template constraints - validate that future_template can be instantiated with Raft response types
    requires {
        typename T::template future_template<kythira::request_vote_response<>>;
        typename T::template future_template<kythira::append_entries_response<>>;
        typename T::template future_template<kythira::install_snapshot_response<>>;
    } &&
    future<typename T::template future_template<kythira::request_vote_response<>>, kythira::request_vote_response<>> &&
    future<typename T::template future_template<kythira::append_entries_response<>>, kythira::append_entries_response<>> &&
    future<typename T::template future_template<kythira::install_snapshot_response<>>, kythira::install_snapshot_response<>>;

// ============================================================================
// Unified Types Template Parameter System
// ============================================================================

// Forward declarations for default implementations
template<typename Data>
requires std::ranges::range<Data> && std::same_as<std::ranges::range_value_t<Data>, std::byte>
class json_rpc_serializer;

template<typename FutureType, typename Serializer, typename Data>
requires kythira::rpc_serializer<Serializer, Data>
class simulator_network_client;

template<typename FutureType, typename Serializer, typename Data>
requires kythira::rpc_serializer<Serializer, Data>
class simulator_network_server;

template<typename NodeId, typename TermId, typename LogIndex>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
class memory_persistence_engine;

class console_logger;
class noop_metrics;

template<typename NodeId>
requires node_id<NodeId>
class default_membership_manager;

// Raft types concept that encapsulates all required type information
template<typename T>
concept raft_types = requires {
    // Future types
    typename T::future_type;
    typename T::promise_type;
    typename T::try_type;
    
    // Component types
    typename T::network_client_type;
    typename T::network_server_type;
    typename T::persistence_engine_type;
    typename T::logger_type;
    typename T::metrics_type;
    typename T::membership_manager_type;
    typename T::state_machine_type;
    
    // Data types
    typename T::node_id_type;
    typename T::term_id_type;
    typename T::log_index_type;
    
    // Serializer and data types
    typename T::serializer_type;
    typename T::serialized_data_type;
    
    // Configuration type
    typename T::configuration_type;
    
    // Concept validation - ensure all types satisfy their respective concepts
    requires node_id<typename T::node_id_type>;
    requires term_id<typename T::term_id_type>;
    requires log_index<typename T::log_index_type>;
    requires serialized_data<typename T::serialized_data_type>;
    requires rpc_serializer<typename T::serializer_type, typename T::serialized_data_type>;
    requires raft_configuration_type<typename T::configuration_type>;
    requires state_machine<typename T::state_machine_type, typename T::log_index_type>;
};

// Default types implementation with sensible defaults
struct default_raft_types {
    // Future types - using kythira::Future as default concrete implementation
    using future_type = kythira::Future<std::vector<std::byte>>;
    using promise_type = kythira::Promise<std::vector<std::byte>>;
    using try_type = kythira::Try<std::vector<std::byte>>;
    
    // Basic data types
    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    
    // Serializer and data types
    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = json_rpc_serializer<serialized_data_type>;
    
    // Network types are forward declared - actual types defined in simulator_network.hpp
    // Users should use the network types from raft_simulator_network_types directly
    class network_client_type;
    class network_server_type;
    
    // Other component types
    using persistence_engine_type = memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = console_logger;
    using metrics_type = noop_metrics;
    using membership_manager_type = default_membership_manager<node_id_type>;
    using state_machine_type = test_key_value_state_machine<log_index_type>;
    
    // Configuration type
    using configuration_type = raft_configuration;
    
    // Type aliases for commonly used compound types
    using log_entry_type = log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = cluster_configuration<node_id_type>;
    using snapshot_type = snapshot<node_id_type, term_id_type, log_index_type>;
    
    // RPC message types
    using request_vote_request_type = request_vote_request<node_id_type, term_id_type, log_index_type>;
    using request_vote_response_type = request_vote_response<term_id_type>;
    using append_entries_request_type = append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type>;
    using append_entries_response_type = append_entries_response<term_id_type, log_index_type>;
    using install_snapshot_request_type = install_snapshot_request<node_id_type, term_id_type, log_index_type>;
    using install_snapshot_response_type = install_snapshot_response<term_id_type>;
};

// Validation that default_raft_types satisfies the raft_types concept
// Note: This static_assert is commented out because test_key_value_state_machine
// is only forward-declared at this point. The concept will be validated when
// the node class is instantiated.
// static_assert(raft_types<default_raft_types>, "default_raft_types must satisfy raft_types concept");

} // namespace kythira
