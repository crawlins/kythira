#pragma once

#include "types.hpp"
#include "network.hpp"
#include "persistence.hpp"
#include "logger.hpp"
#include "metrics.hpp"
#include "membership.hpp"
#include "json_serializer.hpp"
#include "simulator_network.hpp"
#include "console_logger.hpp"
#include <concepts/future.hpp>

#include <raft/future.hpp>
#include <raft/future_collector.hpp>
#include <raft/commit_waiter.hpp>
#include <raft/configuration_synchronizer.hpp>
#include <raft/error_handler.hpp>

#include <vector>
#include <optional>
#include <unordered_map>
#include <chrono>
#include <random>
#include <mutex>
#include <atomic>
#include <format>

namespace kythira {

// Raft node class template with unified types parameter
// Implements the Raft consensus algorithm with pluggable components
template<raft_types Types = default_raft_types>
class node {
public:
    // Type aliases extracted from unified Types parameter
    using types = Types;
    using future_type = typename Types::future_type;
    using promise_type = typename Types::promise_type;
    using try_type = typename Types::try_type;
    using network_client_type = typename Types::network_client_type;
    using network_server_type = typename Types::network_server_type;
    using persistence_engine_type = typename Types::persistence_engine_type;
    using logger_type = typename Types::logger_type;
    using metrics_type = typename Types::metrics_type;
    using membership_manager_type = typename Types::membership_manager_type;
    using state_machine_type = typename Types::state_machine_type;
    using node_id_type = typename Types::node_id_type;
    using term_id_type = typename Types::term_id_type;
    using log_index_type = typename Types::log_index_type;
    using serializer_type = typename Types::serializer_type;
    using serialized_data_type = typename Types::serialized_data_type;
    using configuration_type = typename Types::configuration_type;
    
    // Compound type aliases
    using log_entry_type = typename Types::log_entry_type;
    using cluster_configuration_type = typename Types::cluster_configuration_type;
    using snapshot_type = typename Types::snapshot_type;
    using request_vote_request_type = typename Types::request_vote_request_type;
    using request_vote_response_type = typename Types::request_vote_response_type;
    using append_entries_request_type = typename Types::append_entries_request_type;
    using append_entries_response_type = typename Types::append_entries_response_type;
    using install_snapshot_request_type = typename Types::install_snapshot_request_type;
    using install_snapshot_response_type = typename Types::install_snapshot_response_type;
    
    // Client session tracking types
    using client_id_t = std::uint64_t;
    using serial_number_t = std::uint64_t;
    
    // Constructor with unified types
    node(
        node_id_type node_id,
        network_client_type network_client,
        network_server_type network_server,
        persistence_engine_type persistence,
        logger_type logger,
        metrics_type metrics,
        membership_manager_type membership,
        configuration_type config = configuration_type{}
    );
    
    // Client operations - return template future types
    auto submit_command(const std::vector<std::byte>& command, std::chrono::milliseconds timeout) 
        -> future_type;
    
    // Client operation with session tracking for duplicate detection
    auto submit_command_with_session(
        client_id_t client_id,
        serial_number_t serial_number,
        const std::vector<std::byte>& command,
        std::chrono::milliseconds timeout
    ) -> future_type;
    
    auto read_state(std::chrono::milliseconds timeout) 
        -> future_type;
    
    // Node lifecycle
    auto start() -> void;
    auto stop() -> void;
    [[nodiscard]] auto is_running() const noexcept -> bool;
    
    // Node state queries
    [[nodiscard]] auto get_node_id() const -> node_id_type;
    [[nodiscard]] auto get_current_term() const -> term_id_type;
    [[nodiscard]] auto get_state() const -> kythira::server_state;
    [[nodiscard]] auto is_leader() const -> bool;
    
    // Cluster operations - return template future types
    auto add_server(node_id_type new_node) -> future_type;
    auto remove_server(node_id_type old_node) -> future_type;
    
    // Election timeout check - should be called periodically
    auto check_election_timeout() -> void;
    
    // Heartbeat check - should be called periodically for leaders
    auto check_heartbeat_timeout() -> void;
    
    // Replication trigger - can be called to immediately replicate to followers
    // This is useful for testing and for immediate replication after command submission
    auto replicate_to_followers() -> void;
    
    // Cluster configuration management - for testing and bootstrap
    auto set_cluster_configuration(const std::vector<node_id_type>& node_ids) -> void;

private:
    // ========================================================================
    // Implementation methods
    // ========================================================================
    
    // Helper function to convert node_id to string for logging
    // Handles both string and numeric node IDs
    static auto node_id_to_string(const node_id_type& id) -> std::string {
        if constexpr (std::is_same_v<node_id_type, std::string>) {
            return id;
        } else {
            return std::to_string(id);
        }
    }
    
    // Initialize the node from persistent storage
    auto initialize_from_storage() -> void;
    
    // Register RPC handlers with the network server
    auto register_rpc_handlers() -> void;
    
    // ========================================================================
    // Persistent state (stored before responding to RPCs)
    // ========================================================================
    
    // Latest term server has seen (initialized to 0 on first boot, increases monotonically)
    term_id_type _current_term;
    
    // CandidateId that received vote in current term (or null if none)
    std::optional<node_id_type> _voted_for;
    
    // Log entries; each entry contains command for state machine, and term when entry was received by leader
    // First index is 1
    std::vector<log_entry_type> _log;
    
    // ========================================================================
    // Volatile state (all servers)
    // ========================================================================
    
    // Index of highest log entry known to be committed (initialized to 0, increases monotonically)
    log_index_type _commit_index;
    
    // Index of highest log entry applied to state machine (initialized to 0, increases monotonically)
    log_index_type _last_applied;
    
    // Current server state (follower, candidate, or leader)
    kythira::server_state _state;
    
    // ========================================================================
    // Volatile state (leaders only)
    // Reinitialized after election
    // ========================================================================
    
    // For each server, index of the next log entry to send to that server
    // (initialized to leader last log index + 1)
    std::unordered_map<node_id_type, log_index_type> _next_index;
    
    // For each server, index of highest log entry known to be replicated on server
    // (initialized to 0, increases monotonically)
    std::unordered_map<node_id_type, log_index_type> _match_index;
    
    // Track unresponsive followers for monitoring
    std::unordered_set<node_id_type> _unresponsive_followers;
    
    // ========================================================================
    // Component members using unified types
    // ========================================================================
    
    // Network client for sending RPCs to other nodes
    network_client_type _network_client;
    
    // Network server for receiving RPCs from other nodes
    network_server_type _network_server;
    
    // Persistence engine for durable storage
    persistence_engine_type _persistence;
    
    // Diagnostic logger for structured logging
    logger_type _logger;
    
    // Metrics recorder for performance monitoring
    metrics_type _metrics;
    
    // Membership manager for cluster configuration changes
    membership_manager_type _membership;
    
    // State machine for executing committed log entries
    state_machine_type _state_machine;
    
    // ========================================================================
    // Future collection components using unified types
    // ========================================================================
    
    // Future collectors for async operation coordination using generic future types
    using heartbeat_collector_t = raft_future_collector<append_entries_response_type>;
    using election_collector_t = raft_future_collector<request_vote_response_type>;
    using replication_collector_t = raft_future_collector<append_entries_response_type>;
    
    // Commit waiter for client operations using generic future types
    using commit_waiter_t = kythira::commit_waiter<log_index_type>;
    commit_waiter_t _commit_waiter;
    
    // Configuration synchronizer for safe configuration changes using generic future types
    using config_synchronizer_t = kythira::configuration_synchronizer<node_id_type, log_index_type, future_type>;
    config_synchronizer_t _config_synchronizer;
    
    // ========================================================================
    // Error handling components using unified types
    // ========================================================================
    
    // Error handlers for different RPC operations using generic future types
    using append_entries_error_handler_t = error_handler<append_entries_response_type>;
    using request_vote_error_handler_t = error_handler<request_vote_response_type>;
    using install_snapshot_error_handler_t = error_handler<install_snapshot_response_type>;
    
    append_entries_error_handler_t _append_entries_error_handler;
    request_vote_error_handler_t _request_vote_error_handler;
    install_snapshot_error_handler_t _install_snapshot_error_handler;
    
    // ========================================================================
    // Configuration and timing using unified types
    // ========================================================================
    
    // Raft configuration parameters
    configuration_type _config;
    
    // This node's identifier
    node_id_type _node_id;
    
    // Current cluster configuration
    cluster_configuration_type _configuration;
    
    // Election timeout (randomized between min and max)
    std::chrono::milliseconds _election_timeout;
    
    // Heartbeat interval for leaders
    std::chrono::milliseconds _heartbeat_interval;
    
    // Last time a heartbeat was received (for followers) or sent (for leaders)
    std::chrono::steady_clock::time_point _last_heartbeat;
    
    // Random number generator for election timeout randomization
    std::mt19937 _rng;
    
    // ========================================================================
    // Client session tracking for duplicate detection
    // ========================================================================
    
    // Structure to track client session state
    struct client_session {
        serial_number_t last_serial_number{0};
        std::vector<std::byte> last_response{};
    };
    
    // Map from client_id to session state
    std::unordered_map<client_id_t, client_session> _client_sessions;
    
    // ========================================================================
    // Synchronization
    // ========================================================================
    
    // Mutex for protecting shared state
    mutable std::mutex _mutex;
    
    // Atomic flag for running state
    std::atomic<bool> _running{false};
    
    // ========================================================================
    // Private helper methods using unified types
    // ========================================================================
    
    // RPC handlers
    auto handle_request_vote(const request_vote_request_type& request) -> request_vote_response_type;
    auto handle_append_entries(const append_entries_request_type& request) -> append_entries_response_type;
    auto handle_install_snapshot(const install_snapshot_request_type& request) -> install_snapshot_response_type;
    
    // Election and timing
    auto randomize_election_timeout() -> void;
    auto reset_election_timer() -> void;
    auto election_timeout_elapsed() const -> bool;
    
    // Heartbeat timing
    auto heartbeat_timeout_elapsed() const -> bool;
    auto send_heartbeats() -> void;
    
    // State transitions
    auto become_follower(term_id_type new_term) -> void;
    auto become_candidate() -> void;
    auto start_election() -> void;
    auto become_leader() -> void;
    
    // Log operations
    auto append_log_entry(const log_entry_type& entry) -> void;
    auto get_last_log_index() const -> log_index_type;
    auto get_last_log_term() const -> term_id_type;
    auto get_log_entry(log_index_type index) const -> std::optional<log_entry_type>;
    
    // Replication helpers
    auto send_append_entries_to(node_id_type target) -> void;
    auto send_heartbeat_with_retry(node_id_type target) -> void;
    auto send_install_snapshot_to(node_id_type target) -> void;
    auto advance_commit_index() -> void;
    auto apply_committed_entries() -> void;
    
    // Snapshot operations
    auto create_snapshot() -> void;
    auto create_snapshot(const std::vector<std::byte>& state_machine_state) -> void;
    auto compact_log() -> void;
    auto install_snapshot(const snapshot_type& snap) -> void;
};

// Raft node concept - defines the interface for a Raft node using unified types
template<typename N>
concept raft_node = requires(
    N node,
    const std::vector<std::byte>& command,
    std::chrono::milliseconds timeout,
    typename N::node_id_type node_id
) {
    // Type requirements
    typename N::types;
    typename N::future_type;
    typename N::node_id_type;
    
    // Client operations - return template future types
    { node.submit_command(command, timeout) } 
        -> std::same_as<typename N::future_type>;
    { node.read_state(timeout) } 
        -> std::same_as<typename N::future_type>;
    
    // Node lifecycle
    { node.start() } -> std::same_as<void>;
    { node.stop() } -> std::same_as<void>;
    { node.is_running() } -> std::convertible_to<bool>;
    
    // Node state queries
    { node.get_node_id() } -> std::same_as<typename N::node_id_type>;
    { node.get_current_term() } -> std::same_as<typename N::term_id_type>;
    { node.get_state() } -> std::same_as<kythira::server_state>;
    { node.is_leader() } -> std::convertible_to<bool>;
    
    // Cluster operations - return template future types
    { node.add_server(node_id) } -> std::same_as<typename N::future_type>;
    { node.remove_server(node_id) } -> std::same_as<typename N::future_type>;
    
    // Ensure the types parameter satisfies raft_types concept
    requires raft_types<typename N::types>;
};

// ============================================================================
// Implementation using unified types
// ============================================================================

template<raft_types Types>

node<Types>::node(
    node_id_type node_id,
    network_client_type network_client,
    network_server_type network_server,
    persistence_engine_type persistence,
    logger_type logger,
    metrics_type metrics,
    membership_manager_type membership,
    configuration_type config
)
    : _current_term{0}
    , _voted_for{std::nullopt}
    , _log{}
    , _commit_index{0}
    , _last_applied{0}
    , _state{kythira::server_state::follower}
    , _next_index{}
    , _match_index{}
    , _network_client{std::move(network_client)}
    , _network_server{std::move(network_server)}
    , _persistence{std::move(persistence)}
    , _logger{std::move(logger)}
    , _metrics{std::move(metrics)}
    , _membership{std::move(membership)}
    , _state_machine{}
    , _config{config}
    , _node_id{node_id}
    , _configuration{}
    , _election_timeout{config.election_timeout_min()}
    , _heartbeat_interval{config.heartbeat_interval()}
    , _last_heartbeat{std::chrono::steady_clock::now()}
    , _rng{std::random_device{}()}
{
    // Initialize configuration with just this node
    _configuration._nodes = {_node_id};
    _configuration._is_joint_consensus = false;
    _configuration._old_nodes = std::nullopt;
    
    // Randomize the initial election timeout
    randomize_election_timeout();
    
    // Log node creation
    _logger.info("Raft node created", {
        {"node_id", node_id_to_string(_node_id)},
        {"state", "follower"}
    });
}

template<raft_types Types>

auto node<Types>::set_cluster_configuration(const std::vector<node_id_type>& node_ids) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _configuration._nodes = node_ids;
    _configuration._is_joint_consensus = false;
    _configuration._old_nodes = std::nullopt;
    
    _logger.info("Cluster configuration updated", {
        {"node_id", node_id_to_string(_node_id)},
        {"num_nodes", std::to_string(node_ids.size())}
    });
}

template<raft_types Types>

auto node<Types>::get_node_id() const -> node_id_type {
    return _node_id;
}

template<raft_types Types>

auto node<Types>::get_current_term() const -> term_id_type {
    std::lock_guard<std::mutex> lock(_mutex);
    return _current_term;
}

template<raft_types Types>

auto node<Types>::get_state() const -> kythira::server_state {
    std::lock_guard<std::mutex> lock(_mutex);
    return _state;
}

template<raft_types Types>

auto node<Types>::is_leader() const -> bool {
    std::lock_guard<std::mutex> lock(_mutex);
    return _state == kythira::server_state::leader;
}

template<raft_types Types>

auto node<Types>::is_running() const noexcept -> bool {
    return _running.load(std::memory_order_acquire);
}

template<raft_types Types>

auto node<Types>::randomize_election_timeout() -> void {
    // Randomize election timeout between min and max to prevent split votes
    std::uniform_int_distribution<std::chrono::milliseconds::rep> dist(
        _config.election_timeout_min().count(),
        _config.election_timeout_max().count()
    );
    _election_timeout = std::chrono::milliseconds{dist(_rng)};
}

// Stub implementations for methods to be implemented in later tasks
template<raft_types Types>
auto node<Types>::submit_command(
    const std::vector<std::byte>& command,
    std::chrono::milliseconds timeout
) -> future_type {
    std::lock_guard<std::mutex> lock(_mutex);
    
    // Only leaders can accept client commands
    if (_state != kythira::server_state::leader) {
        _logger.debug("Rejected command submission: not leader", {
            {"node_id", node_id_to_string(_node_id)},
            {"state", _state == kythira::server_state::follower ? "follower" : "candidate"}
        });
        
        // Emit command rejection metric
        _metrics.set_metric_name("command_rejected");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("reason", "not_leader");
        _metrics.add_one();
        _metrics.emit();
        
        // Return a failed future
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::make_exception_ptr(
            std::runtime_error("Not leader")));
        return future;
    }
    
    auto submission_start = std::chrono::steady_clock::now();
    
    _logger.info("Received client command", {
        {"node_id", node_id_to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"command_size", std::to_string(command.size())}
    });
    
    // Emit command received metric
    _metrics.set_metric_name("command_received");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("term", std::to_string(_current_term));
    _metrics.add_one();
    _metrics.emit();
    
    // Step 1: Append command to leader's log
    log_entry_type entry{
        ._term = _current_term,
        ._index = get_last_log_index() + 1,
        ._command = command
    };
    
    try {
        _persistence.append_log_entry(entry);
        _log.push_back(entry);
        
        _logger.debug("Appended command to log", {
            {"node_id", node_id_to_string(_node_id)},
            {"log_index", std::to_string(entry._index)},
            {"term", std::to_string(entry._term)}
        });
        
        // Emit log append metric
        _metrics.set_metric_name("log_entry_appended");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("log_index", std::to_string(entry._index));
        _metrics.add_one();
        _metrics.emit();
        
    } catch (const std::exception& e) {
        _logger.error("Failed to append command to log", {
            {"node_id", node_id_to_string(_node_id)},
            {"error", e.what()}
        });
        
        // Emit log append failure metric
        _metrics.set_metric_name("log_append_failed");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_one();
        _metrics.emit();
        
        // Return failed future
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::current_exception());
        return future;
    }
    
    // Step 2: Register operation with CommitWaiter BEFORE replication
    promise_type promise;
    auto future = promise.getFuture();
    
    auto entry_index = entry._index;
    auto node_id = _node_id;
    auto& logger = _logger;
    auto& metrics = _metrics;
    
    // Create shared promise that can be captured by both callbacks
    auto shared_promise = std::make_shared<promise_type>(std::move(promise));
    
    _commit_waiter.register_operation(
        entry_index,
        // Fulfill callback - called when entry is committed and applied
        [shared_promise, entry_index, node_id, &logger, &metrics, submission_start]
        (std::vector<std::byte> result) mutable {
            auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - submission_start
            );
            
            logger.info("Command committed and applied", {
                {"node_id", node_id_to_string(node_id)},
                {"log_index", std::to_string(entry_index)},
                {"latency_ms", std::to_string(latency.count())}
            });
            
            // Emit command completion metric
            metrics.set_metric_name("command_completed");
            metrics.add_dimension("node_id", node_id_to_string(node_id));
            metrics.add_dimension("log_index", std::to_string(entry_index));
            metrics.add_one();
            metrics.emit();
            
            // Emit command latency metric
            metrics.set_metric_name("command_latency");
            metrics.add_dimension("node_id", node_id_to_string(node_id));
            metrics.add_duration(latency);
            metrics.emit();
            
            shared_promise->setValue(std::move(result));
        },
        // Reject callback - called on timeout or leadership loss
        [shared_promise, entry_index, node_id, &logger, &metrics]
        (std::exception_ptr ex) mutable {
            try {
                std::rethrow_exception(ex);
            } catch (const kythira::commit_timeout_exception<log_index_type>& e) {
                logger.warning("Command timed out waiting for commit", {
                    {"node_id", node_id_to_string(node_id)},
                    {"log_index", std::to_string(entry_index)},
                    {"timeout_ms", std::to_string(e.get_timeout().count())}
                });
                
                // Emit timeout metric
                metrics.set_metric_name("command_timeout");
                metrics.add_dimension("node_id", node_id_to_string(node_id));
                metrics.add_dimension("log_index", std::to_string(entry_index));
                metrics.add_one();
                metrics.emit();
                
            } catch (const kythira::leadership_lost_exception<term_id_type>& e) {
                logger.warning("Command cancelled due to leadership loss", {
                    {"node_id", node_id_to_string(node_id)},
                    {"log_index", std::to_string(entry_index)},
                    {"old_term", std::to_string(e.get_old_term())},
                    {"new_term", std::to_string(e.get_new_term())}
                });
                
                // Emit leadership loss metric
                metrics.set_metric_name("command_leadership_lost");
                metrics.add_dimension("node_id", node_id_to_string(node_id));
                metrics.add_dimension("log_index", std::to_string(entry_index));
                metrics.add_one();
                metrics.emit();
                
            } catch (const std::exception& e) {
                logger.warning("Command cancelled", {
                    {"node_id", node_id_to_string(node_id)},
                    {"log_index", std::to_string(entry_index)},
                    {"error", e.what()}
                });
                
                // Emit cancellation metric
                metrics.set_metric_name("command_cancelled");
                metrics.add_dimension("node_id", node_id_to_string(node_id));
                metrics.add_dimension("log_index", std::to_string(entry_index));
                metrics.add_one();
                metrics.emit();
            }
            
            shared_promise->setException(ex);
        },
        timeout > std::chrono::milliseconds{0} ? std::optional{timeout} : std::nullopt
    );
    
    _logger.debug("Registered operation with CommitWaiter", {
        {"node_id", node_id_to_string(_node_id)},
        {"log_index", std::to_string(entry_index)},
        {"timeout_ms", std::to_string(timeout.count())}
    });
    
    // Step 3: Trigger replication to followers
    // Note: replicate_to_followers is called asynchronously by the leader's heartbeat/replication loop
    // For immediate replication, we could call it here, but the existing implementation
    // relies on the periodic replication mechanism
    // TODO: Call replicate_to_followers() here for immediate replication once type issues are resolved
    
    // Emit replication trigger metric
    _metrics.set_metric_name("replication_triggered");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("log_index", std::to_string(entry_index));
    _metrics.add_one();
    _metrics.emit();
    
    // Step 4: Return future that completes when entry is committed AND applied
    // The future will be fulfilled by CommitWaiter when apply_committed_entries
    // calls notify_committed_and_applied
    return future;
}

template<raft_types Types>
auto node<Types>::submit_command_with_session(
    client_id_t client_id,
    serial_number_t serial_number,
    const std::vector<std::byte>& command,
    std::chrono::milliseconds timeout
) -> future_type {
    // Placeholder implementation
    return submit_command(command, timeout);
}

template<raft_types Types>
auto node<Types>::read_state(
    std::chrono::milliseconds timeout
) -> future_type {
    auto start_time = std::chrono::steady_clock::now();
    
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.debug("Received read_state request", {
        {"node_id", node_id_to_string(_node_id)},
        {"state", _state == kythira::server_state::leader ? "leader" : 
                  (_state == kythira::server_state::follower ? "follower" : "candidate")},
        {"timeout_ms", std::to_string(timeout.count())}
    });
    
    // Emit metrics for read request received
    _metrics.set_metric_name("raft_read_request_received");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_one();
    _metrics.emit();
    
    // Only leaders can serve linearizable reads
    if (_state != kythira::server_state::leader) {
        _logger.debug("Rejected read request: not leader", {
            {"node_id", node_id_to_string(_node_id)},
            {"state", _state == kythira::server_state::follower ? "follower" : "candidate"}
        });
        
        _metrics.set_metric_name("raft_read_request_rejected");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("reason", "not_leader");
        _metrics.add_one();
        _metrics.emit();
        
        // Return a failed future with leadership error
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(kythira::leadership_lost_exception(_current_term, _current_term));
        return future;
    }
    
    // For single-node cluster, return immediately (no need for heartbeat confirmation)
    if (_configuration.nodes().size() == 1) {
        _logger.debug("Single-node cluster, returning state immediately", {
            {"node_id", node_id_to_string(_node_id)},
            {"commit_index", std::to_string(_commit_index)},
            {"last_applied", std::to_string(_last_applied)}
        });
        
        // Get current state from state machine
        try {
            auto state = _state_machine.get_state();
            
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            _logger.debug("Read request completed (single-node)", {
                {"node_id", node_id_to_string(_node_id)},
                {"state_size", std::to_string(state.size())},
                {"duration_ms", std::to_string(duration.count())}
            });
            
            _metrics.set_metric_name("raft_read_request_success");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("cluster_type", "single_node");
            _metrics.add_one();
            _metrics.emit();
            
            _metrics.set_metric_name("raft_read_latency");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(duration));
            _metrics.emit();
            
            promise_type promise;
            auto future = promise.getFuture();
            promise.setValue(std::move(state));
            return future;
            
        } catch (const std::exception& e) {
            _logger.error("Failed to get state from state machine", {
                {"node_id", node_id_to_string(_node_id)},
                {"error", e.what()}
            });
            
            _metrics.set_metric_name("raft_read_request_failed");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("reason", "state_machine_error");
            _metrics.add_one();
            _metrics.emit();
            
            promise_type promise;
            auto future = promise.getFuture();
            promise.setException(std::current_exception());
            return future;
        }
    }
    
    // For multi-node clusters, verify leadership by collecting majority heartbeat responses
    _logger.debug("Multi-node cluster, sending heartbeats to verify leadership", {
        {"node_id", node_id_to_string(_node_id)},
        {"follower_count", std::to_string(_configuration.nodes().size() - 1)},
        {"timeout_ms", std::to_string(timeout.count())}
    });
    
    // Send heartbeats to all followers and collect responses
    std::vector<kythira::Future<append_entries_response_type>> heartbeat_futures;
    heartbeat_futures.reserve(_configuration.nodes().size() - 1);
    
    for (const auto& follower_id : _configuration.nodes()) {
        if (follower_id == _node_id) {
            continue;  // Skip self
        }
        
        // Send empty AppendEntries (heartbeat) to each follower
        auto next_idx = _next_index[follower_id];
        log_index_type prev_log_index = next_idx - 1;
        term_id_type prev_log_term = term_id_type{0};
        
        if (prev_log_index > 0) {
            auto prev_entry = get_log_entry(prev_log_index);
            if (prev_entry.has_value()) {
                prev_log_term = prev_entry->term();
            }
        }
        
        append_entries_request_type request{
            _current_term,
            _node_id,
            prev_log_index,
            prev_log_term,
            std::vector<log_entry_type>{},  // Empty entries for heartbeat
            _commit_index
        };
        
        _logger.debug("Sending heartbeat for read verification", {
            {"node_id", node_id_to_string(_node_id)},
            {"target", node_id_to_string(follower_id)},
            {"term", std::to_string(_current_term)}
        });
        
        // Send heartbeat with timeout
        auto heartbeat_future = _network_client.send_append_entries(
            follower_id, 
            request, 
            timeout
        );
        
        heartbeat_futures.push_back(std::move(heartbeat_future));
    }
    
    // Collect majority of heartbeat responses
    auto current_term = _current_term;
    auto node_id = _node_id;
    
    return kythira::raft_future_collector<append_entries_response_type>::collect_majority(
        std::move(heartbeat_futures),
        timeout
    ).thenValue([this, current_term, node_id, start_time](std::vector<append_entries_response_type> responses) -> std::vector<std::byte> {
        std::lock_guard<std::mutex> lock(_mutex);
        
        _logger.debug("Received majority heartbeat responses", {
            {"node_id", node_id_to_string(node_id)},
            {"response_count", std::to_string(responses.size())}
        });
        
        // Check if we're still the leader and term hasn't changed
        if (_state != kythira::server_state::leader) {
            _logger.warning("Lost leadership during read operation", {
                {"node_id", node_id_to_string(node_id)},
                {"current_state", _state == kythira::server_state::follower ? "follower" : "candidate"}
            });
            
            _metrics.set_metric_name("raft_read_request_aborted");
            _metrics.add_dimension("node_id", node_id_to_string(node_id));
            _metrics.add_dimension("reason", "leadership_lost");
            _metrics.add_one();
            _metrics.emit();
            
            throw kythira::leadership_lost_exception(current_term, _current_term);
        }
        
        if (_current_term != current_term) {
            _logger.warning("Term changed during read operation", {
                {"node_id", node_id_to_string(node_id)},
                {"old_term", std::to_string(current_term)},
                {"new_term", std::to_string(_current_term)}
            });
            
            _metrics.set_metric_name("raft_read_request_aborted");
            _metrics.add_dimension("node_id", node_id_to_string(node_id));
            _metrics.add_dimension("reason", "term_changed");
            _metrics.add_one();
            _metrics.emit();
            
            throw kythira::leadership_lost_exception(current_term, _current_term);
        }
        
        // Check for higher terms in responses
        for (const auto& response : responses) {
            if (response.term() > _current_term) {
                _logger.info("Discovered higher term in heartbeat response, stepping down", {
                    {"node_id", node_id_to_string(node_id)},
                    {"current_term", std::to_string(_current_term)},
                    {"response_term", std::to_string(response.term())}
                });
                
                become_follower(response.term());
                
                _metrics.set_metric_name("raft_read_request_aborted");
                _metrics.add_dimension("node_id", node_id_to_string(node_id));
                _metrics.add_dimension("reason", "higher_term_discovered");
                _metrics.add_one();
                _metrics.emit();
                
                throw kythira::leadership_lost_exception(current_term, response.term());
            }
        }
        
        // Leadership confirmed by majority, return current state
        _logger.debug("Leadership confirmed, returning state", {
            {"node_id", node_id_to_string(node_id)},
            {"commit_index", std::to_string(_commit_index)},
            {"last_applied", std::to_string(_last_applied)}
        });
        
        try {
            auto state = _state_machine.get_state();
            
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            _logger.info("Read request completed successfully", {
                {"node_id", node_id_to_string(node_id)},
                {"state_size", std::to_string(state.size())},
                {"duration_ms", std::to_string(duration.count())},
                {"heartbeat_responses", std::to_string(responses.size())}
            });
            
            _metrics.set_metric_name("raft_read_request_success");
            _metrics.add_dimension("node_id", node_id_to_string(node_id));
            _metrics.add_dimension("cluster_type", "multi_node");
            _metrics.add_one();
            _metrics.emit();
            
            _metrics.set_metric_name("raft_read_latency");
            _metrics.add_dimension("node_id", node_id_to_string(node_id));
            _metrics.add_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(duration));
            _metrics.emit();
            
            return state;
            
        } catch (const std::exception& e) {
            _logger.error("Failed to get state from state machine after leadership confirmation", {
                {"node_id", node_id_to_string(node_id)},
                {"error", e.what()}
            });
            
            _metrics.set_metric_name("raft_read_request_failed");
            _metrics.add_dimension("node_id", node_id_to_string(node_id));
            _metrics.add_dimension("reason", "state_machine_error");
            _metrics.add_one();
            _metrics.emit();
            
            throw;
        }
    }).thenError([this, node_id, start_time](folly::exception_wrapper ew) -> std::vector<std::byte> {
        std::lock_guard<std::mutex> lock(_mutex);
        
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        _logger.error("Read request failed", {
            {"node_id", node_id_to_string(node_id)},
            {"error", ew.what().toStdString()},
            {"duration_ms", std::to_string(duration.count())}
        });
        
        _metrics.set_metric_name("raft_read_request_failed");
        _metrics.add_dimension("node_id", node_id_to_string(node_id));
        _metrics.add_dimension("reason", "heartbeat_collection_failed");
        _metrics.add_one();
        _metrics.emit();
        
        // Re-throw the exception
        ew.throw_exception();
        
        // This line is unreachable but needed for compilation
        return std::vector<std::byte>{};
    });
}

template<raft_types Types>


auto node<Types>::start() -> void {
    // Check if already running
    if (_running.load(std::memory_order_acquire)) {
        _logger.warning("Attempted to start node that is already running", {
            {"node_id", node_id_to_string(_node_id)}
        });
        return;
    }
    
    _logger.info("Starting Raft node", {
        {"node_id", node_id_to_string(_node_id)}
    });
    
    // Initialize from persistent storage (recover state after crash)
    initialize_from_storage();
    
    // Register RPC handlers with network server
    register_rpc_handlers();
    
    // Start the network server
    _network_server.start();
    
    // Reset election timer
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _last_heartbeat = std::chrono::steady_clock::now();
    }
    
    // Mark as running
    _running.store(true, std::memory_order_release);
    
    _logger.info("Raft node started successfully", {
        {"node_id", node_id_to_string(_node_id)},
        {"state", "follower"},
        {"current_term", std::to_string(_current_term)}
    });
}

template<raft_types Types>

auto node<Types>::stop() -> void {
    // Check if already stopped
    if (!_running.load(std::memory_order_acquire)) {
        _logger.warning("Attempted to stop node that is not running", {
            {"node_id", node_id_to_string(_node_id)}
        });
        return;
    }
    
    _logger.info("Stopping Raft node", {
        {"node_id", node_id_to_string(_node_id)}
    });
    
    // Mark as not running
    _running.store(false, std::memory_order_release);
    
    // Cancel all pending client operations
    _commit_waiter.cancel_all_operations("Node shutdown");
    
    // Stop the network server
    _network_server.stop();
    
    _logger.info("Raft node stopped successfully", {
        {"node_id", node_id_to_string(_node_id)}
    });
}

template<raft_types Types>

auto node<Types>::add_server(node_id_type new_node) -> future_type {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.info("Add server requested", {
        {"node_id", node_id_to_string(_node_id)},
        {"new_node", node_id_to_string(new_node)},
        {"current_state", _state == kythira::server_state::leader ? "leader" : 
                         _state == kythira::server_state::candidate ? "candidate" : "follower"}
    });
    
    // Only leaders can add servers
    if (_state != kythira::server_state::leader) {
        _logger.warning("Cannot add server: not leader", {
            {"node_id", node_id_to_string(_node_id)},
            {"new_node", node_id_to_string(new_node)}
        });
        
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Not leader - cannot add server"));
        return future;
    }
    
    // Check if configuration change is already in progress
    if (_config_synchronizer.is_configuration_change_in_progress()) {
        _logger.warning("Cannot add server: configuration change already in progress", {
            {"node_id", node_id_to_string(_node_id)},
            {"new_node", node_id_to_string(new_node)}
        });
        
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Configuration change already in progress"));
        return future;
    }
    
    // Validate new node is not already in configuration
    if (_membership.is_node_in_configuration(new_node, _configuration)) {
        _logger.warning("Cannot add server: already in configuration", {
            {"node_id", node_id_to_string(_node_id)},
            {"new_node", node_id_to_string(new_node)}
        });
        
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Node already in configuration"));
        return future;
    }
    
    // Validate new node with membership manager
    if (!_membership.validate_new_node(new_node)) {
        _logger.warning("Cannot add server: validation failed", {
            {"node_id", node_id_to_string(_node_id)},
            {"new_node", node_id_to_string(new_node)}
        });
        
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Node validation failed"));
        return future;
    }
    
    _logger.info("Starting server addition with joint consensus", {
        {"node_id", node_id_to_string(_node_id)},
        {"new_node", node_id_to_string(new_node)}
    });
    
    // Create new configuration with added node
    std::vector<node_id_type> new_nodes = _configuration.nodes();
    new_nodes.push_back(new_node);
    
    cluster_configuration_type new_config{
        new_nodes,
        false,  // Not joint consensus yet
        std::nullopt
    };
    
    // Start configuration change using ConfigurationSynchronizer
    auto timeout = _config.append_entries_timeout() * 10;  // Longer timeout for config changes
    auto config_future = _config_synchronizer.start_configuration_change(new_config, timeout);
    
    // Initialize next_index and match_index for new server
    _next_index[new_node] = get_last_log_index() + 1;
    _match_index[new_node] = 0;
    
    _metrics.set_metric_name("raft_add_server_started");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("new_node", node_id_to_string(new_node));
    _metrics.add_one();
    _metrics.emit();
    
    // Return the future from configuration synchronizer
    // It will complete when the configuration change is committed
    return config_future.then([this, new_node](auto try_result) {
        if (try_result.hasException()) {
            _logger.error("Add server failed", {
                {"node_id", node_id_to_string(_node_id)},
                {"new_node", node_id_to_string(new_node)}
            });
            
            _metrics.set_metric_name("raft_add_server_failed");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("new_node", node_id_to_string(new_node));
            _metrics.add_one();
            _metrics.emit();
            
            throw try_result.exception();
        }
        
        _logger.info("Add server completed successfully", {
            {"node_id", node_id_to_string(_node_id)},
            {"new_node", node_id_to_string(new_node)}
        });
        
        _metrics.set_metric_name("raft_add_server_success");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("new_node", node_id_to_string(new_node));
        _metrics.add_one();
        _metrics.emit();
        
        return std::vector<std::byte>{};
    });
}


template<raft_types Types>

auto node<Types>::remove_server(node_id_type old_node) -> future_type {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.info("Remove server requested", {
        {"node_id", node_id_to_string(_node_id)},
        {"old_node", node_id_to_string(old_node)},
        {"current_state", _state == kythira::server_state::leader ? "leader" : 
                         _state == kythira::server_state::candidate ? "candidate" : "follower"}
    });
    
    // Only leaders can remove servers
    if (_state != kythira::server_state::leader) {
        _logger.warning("Cannot remove server: not leader", {
            {"node_id", node_id_to_string(_node_id)},
            {"old_node", node_id_to_string(old_node)}
        });
        
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Not leader - cannot remove server"));
        return future;
    }
    
    // Check if configuration change is already in progress
    if (_config_synchronizer.is_configuration_change_in_progress()) {
        _logger.warning("Cannot remove server: configuration change already in progress", {
            {"node_id", node_id_to_string(_node_id)},
            {"old_node", node_id_to_string(old_node)}
        });
        
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Configuration change already in progress"));
        return future;
    }
    
    // Validate server is in current configuration
    if (!_membership.is_node_in_configuration(old_node, _configuration)) {
        _logger.warning("Cannot remove server: not in configuration", {
            {"node_id", node_id_to_string(_node_id)},
            {"old_node", node_id_to_string(old_node)}
        });
        
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Node not in configuration"));
        return future;
    }
    
    // Check if removing this server would leave cluster with no nodes
    if (_configuration.nodes().size() <= 1) {
        _logger.warning("Cannot remove server: would leave cluster empty", {
            {"node_id", node_id_to_string(_node_id)},
            {"old_node", node_id_to_string(old_node)}
        });
        
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Cannot remove last node from cluster"));
        return future;
    }
    
    bool removing_self = (old_node == _node_id);
    
    _logger.info("Starting server removal with joint consensus", {
        {"node_id", node_id_to_string(_node_id)},
        {"old_node", node_id_to_string(old_node)},
        {"removing_self", removing_self ? "true" : "false"}
    });
    
    // Create new configuration without the removed node
    std::vector<node_id_type> new_nodes;
    for (const auto& node : _configuration.nodes()) {
        if (node != old_node) {
            new_nodes.push_back(node);
        }
    }
    
    cluster_configuration_type new_config{
        new_nodes,
        false,  // Not joint consensus yet
        std::nullopt
    };
    
    // Start configuration change using ConfigurationSynchronizer
    auto timeout = _config.append_entries_timeout() * 10;  // Longer timeout for config changes
    auto config_future = _config_synchronizer.start_configuration_change(new_config, timeout);
    
    // Clean up state for removed server
    _next_index.erase(old_node);
    _match_index.erase(old_node);
    _unresponsive_followers.erase(old_node);
    
    // Handle node removal with membership manager
    _membership.handle_node_removal(old_node);
    
    _metrics.set_metric_name("raft_remove_server_started");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("old_node", node_id_to_string(old_node));
    _metrics.add_dimension("removing_self", removing_self ? "true" : "false");
    _metrics.add_one();
    _metrics.emit();
    
    // Return the future from configuration synchronizer
    return config_future.then([this, old_node, removing_self](auto try_result) {
        if (try_result.hasException()) {
            _logger.error("Remove server failed", {
                {"node_id", node_id_to_string(_node_id)},
                {"old_node", node_id_to_string(old_node)}
            });
            
            _metrics.set_metric_name("raft_remove_server_failed");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("old_node", node_id_to_string(old_node));
            _metrics.add_one();
            _metrics.emit();
            
            throw try_result.exception();
        }
        
        _logger.info("Remove server completed successfully", {
            {"node_id", node_id_to_string(_node_id)},
            {"old_node", node_id_to_string(old_node)}
        });
        
        // If we removed ourselves, step down from leadership
        if (removing_self) {
            _logger.info("Removed self from cluster, stepping down", {
                {"node_id", node_id_to_string(_node_id)}
            });
            
            become_follower(_current_term);
        }
        
        _metrics.set_metric_name("raft_remove_server_success");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("old_node", node_id_to_string(old_node));
        _metrics.add_one();
        _metrics.emit();
        
        return std::vector<std::byte>{};
    });
}


template<raft_types Types>

auto node<Types>::check_election_timeout() -> void {
    // Placeholder implementation
    std::lock_guard<std::mutex> lock(_mutex);
    
    // Only followers and candidates check for election timeout
    if (_state == kythira::server_state::leader) {
        return;
    }
    
    // Check if election timeout has elapsed
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_heartbeat);
    
    if (elapsed >= _election_timeout) {
        _logger.debug("Election timeout elapsed", {
            {"node_id", node_id_to_string(_node_id)},
            {"state", _state == kythira::server_state::follower ? "follower" : "candidate"},
            {"term", std::to_string(_current_term)}
        });
        
        // Start a new election (placeholder)
        become_candidate();
    }
}

template<raft_types Types>

auto node<Types>::check_heartbeat_timeout() -> void {
    // Placeholder implementation
    std::lock_guard<std::mutex> lock(_mutex);
    
    // Check for timed-out operations (do this for all states)
    auto cancelled_count = _commit_waiter.cancel_timed_out_operations();
    if (cancelled_count > 0) {
        _logger.debug("Cancelled timed-out operations", {
            {"node_id", node_id_to_string(_node_id)},
            {"cancelled_count", std::to_string(cancelled_count)}
        });
        
        // Emit timeout cancellation metric
        _metrics.set_metric_name("operations_timed_out");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_count(cancelled_count);
        _metrics.emit();
    }
    
    // Only leaders send heartbeats
    if (_state != kythira::server_state::leader) {
        return;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_heartbeat);
    
    if (elapsed >= _heartbeat_interval) {
        _logger.debug("Heartbeat timeout elapsed, sending heartbeats", {
            {"node_id", node_id_to_string(_node_id)},
            {"term", std::to_string(_current_term)}
        });
        
        send_heartbeats();
        _last_heartbeat = now;
    }
}

// Placeholder implementations for private methods
template<raft_types Types>

auto node<Types>::initialize_from_storage() -> void {
    _logger.info("Initializing node from persistent storage", {
        {"node_id", node_id_to_string(_node_id)}
    });
    
    // Load current term
    _current_term = _persistence.load_current_term();
    
    // Load voted_for
    _voted_for = _persistence.load_voted_for();
    
    _logger.info("Node initialized from storage", {
        {"node_id", node_id_to_string(_node_id)},
        {"current_term", std::to_string(_current_term)}
    });
}

template<raft_types Types>

auto node<Types>::register_rpc_handlers() -> void {
    // Register RequestVote handler
    _network_server.register_request_vote_handler(
        [this](const request_vote_request_type& request) -> request_vote_response_type {
            return this->handle_request_vote(request);
        }
    );
    
    // Register AppendEntries handler
    _network_server.register_append_entries_handler(
        [this](const append_entries_request_type& request) -> append_entries_response_type {
            return this->handle_append_entries(request);
        }
    );
    
    // Register InstallSnapshot handler
    _network_server.register_install_snapshot_handler(
        [this](const install_snapshot_request_type& request) -> install_snapshot_response_type {
            return this->handle_install_snapshot(request);
        }
    );
    
    _logger.debug("RPC handlers registered", {
        {"node_id", node_id_to_string(_node_id)}
    });
}

template<raft_types Types>


auto node<Types>::handle_request_vote(const request_vote_request_type& request) -> request_vote_response_type {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.debug("Received RequestVote RPC", {
        {"node_id", node_id_to_string(_node_id)},
        {"from_candidate", node_id_to_string(request.candidate_id())},
        {"request_term", std::to_string(request.term())},
        {"current_term", std::to_string(_current_term)},
        {"candidate_last_log_index", std::to_string(request.last_log_index())},
        {"candidate_last_log_term", std::to_string(request.last_log_term())}
    });
    
    // Emit metrics for vote request received
    _metrics.set_metric_name("raft_vote_request_received");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_one();
    _metrics.emit();
    
    // Rule 1: Reply false if request term < current term (§5.1)
    if (request.term() < _current_term) {
        _logger.debug("Denying vote: request term < current term", {
            {"node_id", node_id_to_string(_node_id)},
            {"request_term", std::to_string(request.term())},
            {"current_term", std::to_string(_current_term)}
        });
        
        _metrics.set_metric_name("raft_vote_denied");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("reason", "stale_term");
        _metrics.add_one();
        _metrics.emit();
        
        return request_vote_response_type{_current_term, false};
    }
    
    // Rule 2: If request term > current term, update current term and become follower (§5.1)
    if (request.term() > _current_term) {
        _logger.info("Discovered higher term in RequestVote, becoming follower", {
            {"node_id", node_id_to_string(_node_id)},
            {"old_term", std::to_string(_current_term)},
            {"new_term", std::to_string(request.term())}
        });
        
        become_follower(request.term());
    }
    
    // Rule 3: Check if we've already voted for another candidate in this term
    if (_voted_for.has_value() && _voted_for.value() != request.candidate_id()) {
        _logger.debug("Denying vote: already voted for another candidate", {
            {"node_id", node_id_to_string(_node_id)},
            {"voted_for", node_id_to_string(_voted_for.value())},
            {"candidate", node_id_to_string(request.candidate_id())},
            {"term", std::to_string(_current_term)}
        });
        
        _metrics.set_metric_name("raft_vote_denied");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("reason", "already_voted");
        _metrics.add_one();
        _metrics.emit();
        
        return request_vote_response_type{_current_term, false};
    }
    
    // Rule 4: Check if candidate's log is at least as up-to-date as receiver's log (§5.4.1)
    // Log is more up-to-date if:
    // - Last log term is higher, OR
    // - Last log terms are equal AND last log index is >= receiver's last log index
    auto my_last_log_index = get_last_log_index();
    auto my_last_log_term = get_last_log_term();
    
    bool candidate_log_up_to_date = false;
    if (request.last_log_term() > my_last_log_term) {
        // Candidate's last log term is higher - candidate is more up-to-date
        candidate_log_up_to_date = true;
    } else if (request.last_log_term() == my_last_log_term) {
        // Same last log term - compare log lengths
        candidate_log_up_to_date = (request.last_log_index() >= my_last_log_index);
    }
    // else: candidate's last log term is lower - candidate is not up-to-date
    
    if (!candidate_log_up_to_date) {
        _logger.debug("Denying vote: candidate log not up-to-date", {
            {"node_id", node_id_to_string(_node_id)},
            {"candidate", node_id_to_string(request.candidate_id())},
            {"my_last_log_term", std::to_string(my_last_log_term)},
            {"my_last_log_index", std::to_string(my_last_log_index)},
            {"candidate_last_log_term", std::to_string(request.last_log_term())},
            {"candidate_last_log_index", std::to_string(request.last_log_index())}
        });
        
        _metrics.set_metric_name("raft_vote_denied");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("reason", "log_not_up_to_date");
        _metrics.add_one();
        _metrics.emit();
        
        return request_vote_response_type{_current_term, false};
    }
    
    // All checks passed - grant vote
    _voted_for = request.candidate_id();
    
    // Persist voted_for before responding (§5.2)
    _persistence.save_voted_for(_voted_for.value());
    _persistence.save_current_term(_current_term);
    
    // Reset election timer when granting vote (§5.2)
    reset_election_timer();
    
    _logger.info("Granting vote", {
        {"node_id", node_id_to_string(_node_id)},
        {"candidate", node_id_to_string(request.candidate_id())},
        {"term", std::to_string(_current_term)}
    });
    
    _metrics.set_metric_name("raft_vote_granted");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("candidate", node_id_to_string(request.candidate_id()));
    _metrics.add_one();
    _metrics.emit();
    
    return request_vote_response_type{_current_term, true};
}


template<raft_types Types>


auto node<Types>::handle_append_entries(const append_entries_request_type& request) -> append_entries_response_type {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.debug("Received AppendEntries RPC", {
        {"node_id", node_id_to_string(_node_id)},
        {"from_leader", node_id_to_string(request.leader_id())},
        {"request_term", std::to_string(request.term())},
        {"current_term", std::to_string(_current_term)},
        {"prev_log_index", std::to_string(request.prev_log_index())},
        {"prev_log_term", std::to_string(request.prev_log_term())},
        {"num_entries", std::to_string(request.entries().size())},
        {"leader_commit", std::to_string(request.leader_commit())}
    });
    
    // Emit metrics for AppendEntries received
    _metrics.set_metric_name("raft_append_entries_received");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("is_heartbeat", request.entries().empty() ? "true" : "false");
    _metrics.add_one();
    _metrics.emit();
    
    // Rule 1: Reply false if request term < current term (§5.1)
    if (request.term() < _current_term) {
        _logger.debug("Rejecting AppendEntries: request term < current term", {
            {"node_id", node_id_to_string(_node_id)},
            {"request_term", std::to_string(request.term())},
            {"current_term", std::to_string(_current_term)}
        });
        
        _metrics.set_metric_name("raft_append_entries_rejected");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("reason", "stale_term");
        _metrics.add_one();
        _metrics.emit();
        
        return append_entries_response_type{_current_term, false, std::nullopt, std::nullopt};
    }
    
    // Rule 2: If request term >= current term, update current term and become follower (§5.1)
    if (request.term() > _current_term) {
        _logger.info("Discovered higher term in AppendEntries, becoming follower", {
            {"node_id", node_id_to_string(_node_id)},
            {"old_term", std::to_string(_current_term)},
            {"new_term", std::to_string(request.term())}
        });
        
        become_follower(request.term());
    } else if (_state == kythira::server_state::candidate) {
        // If we're a candidate and receive AppendEntries from valid leader, become follower
        _logger.info("Received valid AppendEntries as candidate, becoming follower", {
            {"node_id", node_id_to_string(_node_id)},
            {"term", std::to_string(_current_term)}
        });
        
        become_follower(_current_term);
    }
    
    // Reset election timer on valid AppendEntries (§5.2)
    reset_election_timer();
    
    // Rule 3: Reply false if log doesn't contain an entry at prevLogIndex whose term matches prevLogTerm (§5.3)
    if (request.prev_log_index() > 0) {
        // Check if we have an entry at prevLogIndex
        if (request.prev_log_index() > get_last_log_index()) {
            // We don't have enough entries
            _logger.debug("Rejecting AppendEntries: log too short", {
                {"node_id", node_id_to_string(_node_id)},
                {"prev_log_index", std::to_string(request.prev_log_index())},
                {"last_log_index", std::to_string(get_last_log_index())}
            });
            
            _metrics.set_metric_name("raft_append_entries_rejected");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("reason", "log_too_short");
            _metrics.add_one();
            _metrics.emit();
            
            // Return conflict information for faster log backtracking
            auto conflict_index = get_last_log_index() + 1;
            return append_entries_response_type{_current_term, false, conflict_index, std::nullopt};
        }
        
        // Check if the term at prevLogIndex matches prevLogTerm
        auto prev_entry = get_log_entry(request.prev_log_index());
        if (!prev_entry.has_value() || prev_entry->term() != request.prev_log_term()) {
            // Term mismatch at prevLogIndex
            auto conflict_term = prev_entry.has_value() ? prev_entry->term() : term_id_type{0};
            
            _logger.debug("Rejecting AppendEntries: term mismatch at prevLogIndex", {
                {"node_id", node_id_to_string(_node_id)},
                {"prev_log_index", std::to_string(request.prev_log_index())},
                {"expected_term", std::to_string(request.prev_log_term())},
                {"actual_term", std::to_string(conflict_term)}
            });
            
            _metrics.set_metric_name("raft_append_entries_rejected");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("reason", "term_mismatch");
            _metrics.add_one();
            _metrics.emit();
            
            // Find the first index of the conflicting term for faster backtracking
            log_index_type conflict_index = request.prev_log_index();
            if (prev_entry.has_value()) {
                // Search backwards to find first entry with this term
                for (log_index_type i = request.prev_log_index(); i > 0; --i) {
                    auto entry = get_log_entry(i);
                    if (!entry.has_value() || entry->term() != conflict_term) {
                        conflict_index = i + 1;
                        break;
                    }
                    if (i == 1) {
                        conflict_index = 1;
                    }
                }
            }
            
            return append_entries_response_type{_current_term, false, conflict_index, conflict_term};
        }
    }
    
    // Rule 4: If an existing entry conflicts with a new one (same index but different terms),
    // delete the existing entry and all that follow it (§5.3)
    bool log_modified = false;
    for (std::size_t i = 0; i < request.entries().size(); ++i) {
        const auto& new_entry = request.entries()[i];
        auto entry_index = request.prev_log_index() + i + 1;
        
        // Check if we have an entry at this index
        if (entry_index <= get_last_log_index()) {
            auto existing_entry = get_log_entry(entry_index);
            if (existing_entry.has_value() && existing_entry->term() != new_entry.term()) {
                // Conflict detected - delete this entry and all that follow
                _logger.info("Detected log conflict, truncating log", {
                    {"node_id", node_id_to_string(_node_id)},
                    {"conflict_index", std::to_string(entry_index)},
                    {"existing_term", std::to_string(existing_entry->term())},
                    {"new_term", std::to_string(new_entry.term())}
                });
                
                // Truncate log from conflict point
                _log.erase(_log.begin() + (entry_index - 1), _log.end());
                log_modified = true;
                
                // Persist truncation
                _persistence.truncate_log(entry_index);
                break;
            }
        }
    }
    
    // Rule 5: Append any new entries not already in the log
    for (std::size_t i = 0; i < request.entries().size(); ++i) {
        const auto& new_entry = request.entries()[i];
        auto entry_index = request.prev_log_index() + i + 1;
        
        if (entry_index > get_last_log_index()) {
            // This is a new entry - append it
            append_log_entry(new_entry);
            log_modified = true;
            
            // Persist the new entry
            _persistence.append_log_entry(new_entry);
            
            _logger.debug("Appended new log entry", {
                {"node_id", node_id_to_string(_node_id)},
                {"index", std::to_string(entry_index)},
                {"term", std::to_string(new_entry.term())}
            });
        }
    }
    
    // Rule 6: If leaderCommit > commitIndex, set commitIndex = min(leaderCommit, index of last new entry)
    if (request.leader_commit() > _commit_index) {
        auto old_commit_index = _commit_index;
        _commit_index = std::min(request.leader_commit(), get_last_log_index());
        
        _logger.debug("Updated commit index", {
            {"node_id", node_id_to_string(_node_id)},
            {"old_commit_index", std::to_string(old_commit_index)},
            {"new_commit_index", std::to_string(_commit_index)},
            {"leader_commit", std::to_string(request.leader_commit())}
        });
        
        // Apply newly committed entries to state machine
        apply_committed_entries();
    }
    
    // Persist current term if it was updated
    if (log_modified || request.term() > _current_term) {
        _persistence.save_current_term(_current_term);
    }
    
    _logger.debug("AppendEntries succeeded", {
        {"node_id", node_id_to_string(_node_id)},
        {"last_log_index", std::to_string(get_last_log_index())},
        {"commit_index", std::to_string(_commit_index)}
    });
    
    _metrics.set_metric_name("raft_append_entries_accepted");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("entries_appended", std::to_string(request.entries().size()));
    _metrics.add_one();
    _metrics.emit();
    
    return append_entries_response_type{_current_term, true, std::nullopt, std::nullopt};
}


template<raft_types Types>


auto node<Types>::handle_install_snapshot(const install_snapshot_request_type& request) -> install_snapshot_response_type {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.debug("Received InstallSnapshot RPC", {
        {"node_id", node_id_to_string(_node_id)},
        {"from_leader", node_id_to_string(request.leader_id())},
        {"request_term", std::to_string(request.term())},
        {"current_term", std::to_string(_current_term)},
        {"last_included_index", std::to_string(request.last_included_index())},
        {"last_included_term", std::to_string(request.last_included_term())},
        {"offset", std::to_string(request.offset())},
        {"data_size", std::to_string(request.data().size())},
        {"done", request.done() ? "true" : "false"}
    });
    
    // Emit metrics for InstallSnapshot received
    _metrics.set_metric_name("raft_install_snapshot_received");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("is_first_chunk", request.offset() == 0 ? "true" : "false");
    _metrics.add_dimension("is_last_chunk", request.done() ? "true" : "false");
    _metrics.add_one();
    _metrics.emit();
    
    // Rule 1: Reply immediately if term < currentTerm (§7)
    if (request.term() < _current_term) {
        _logger.debug("Rejecting InstallSnapshot: request term < current term", {
            {"node_id", node_id_to_string(_node_id)},
            {"request_term", std::to_string(request.term())},
            {"current_term", std::to_string(_current_term)}
        });
        
        _metrics.set_metric_name("raft_install_snapshot_rejected");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("reason", "stale_term");
        _metrics.add_one();
        _metrics.emit();
        
        return install_snapshot_response_type{_current_term};
    }
    
    // Rule 2: If request term > current term, update current term and become follower
    if (request.term() > _current_term) {
        _logger.info("Discovered higher term in InstallSnapshot, becoming follower", {
            {"node_id", node_id_to_string(_node_id)},
            {"old_term", std::to_string(_current_term)},
            {"new_term", std::to_string(request.term())}
        });
        
        become_follower(request.term());
    }
    
    // Reset election timer on valid InstallSnapshot
    reset_election_timer();
    
    // Rule 3: Create new snapshot file if first chunk (offset is 0)
    // For simplicity, we'll accumulate chunks in memory and install when done
    // In production, you'd want to write chunks to disk incrementally
    static std::unordered_map<node_id_type, std::vector<std::byte>> snapshot_buffers;
    
    if (request.offset() == 0) {
        // First chunk - initialize buffer
        snapshot_buffers[_node_id] = request.data();
        
        _logger.debug("Started receiving snapshot", {
            {"node_id", node_id_to_string(_node_id)},
            {"last_included_index", std::to_string(request.last_included_index())},
            {"last_included_term", std::to_string(request.last_included_term())},
            {"first_chunk_size", std::to_string(request.data().size())}
        });
    } else {
        // Subsequent chunk - append to buffer
        if (snapshot_buffers.find(_node_id) == snapshot_buffers.end()) {
            // Missing first chunk - reject
            _logger.error("Received snapshot chunk without first chunk", {
                {"node_id", node_id_to_string(_node_id)},
                {"offset", std::to_string(request.offset())}
            });
            
            _metrics.set_metric_name("raft_install_snapshot_rejected");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("reason", "missing_first_chunk");
            _metrics.add_one();
            _metrics.emit();
            
            return install_snapshot_response_type{_current_term};
        }
        
        auto& buffer = snapshot_buffers[_node_id];
        if (buffer.size() != request.offset()) {
            // Offset mismatch - reject
            _logger.error("Snapshot chunk offset mismatch", {
                {"node_id", node_id_to_string(_node_id)},
                {"expected_offset", std::to_string(buffer.size())},
                {"actual_offset", std::to_string(request.offset())}
            });
            
            _metrics.set_metric_name("raft_install_snapshot_rejected");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("reason", "offset_mismatch");
            _metrics.add_one();
            _metrics.emit();
            
            return install_snapshot_response_type{_current_term};
        }
        
        // Append chunk data
        buffer.insert(buffer.end(), request.data().begin(), request.data().end());
        
        _logger.debug("Received snapshot chunk", {
            {"node_id", node_id_to_string(_node_id)},
            {"offset", std::to_string(request.offset())},
            {"chunk_size", std::to_string(request.data().size())},
            {"total_size", std::to_string(buffer.size())}
        });
    }
    
    // Rule 4: Reply and wait for more data chunks if done is false
    if (!request.done()) {
        _logger.debug("Waiting for more snapshot chunks", {
            {"node_id", node_id_to_string(_node_id)},
            {"bytes_received", std::to_string(snapshot_buffers[_node_id].size())}
        });
        
        return install_snapshot_response_type{_current_term};
    }
    
    // Rule 5: Save snapshot file, discard any existing or partial snapshot with smaller index
    auto& complete_snapshot_data = snapshot_buffers[_node_id];
    
    _logger.info("Installing complete snapshot", {
        {"node_id", node_id_to_string(_node_id)},
        {"last_included_index", std::to_string(request.last_included_index())},
        {"last_included_term", std::to_string(request.last_included_term())},
        {"snapshot_size", std::to_string(complete_snapshot_data.size())}
    });
    
    // Create snapshot object
    snapshot_type snap{
        request.last_included_index(),
        request.last_included_term(),
        _configuration,  // Use current configuration (leader will send updated config if needed)
        complete_snapshot_data
    };
    
    // Rule 6: If existing log entry has same index and term as snapshot's last included entry,
    // retain log entries following it and reply
    bool retain_log = false;
    if (request.last_included_index() <= get_last_log_index()) {
        auto entry_at_snapshot_index = get_log_entry(request.last_included_index());
        if (entry_at_snapshot_index.has_value() && 
            entry_at_snapshot_index->term() == request.last_included_term()) {
            // Log entry matches snapshot - retain entries after snapshot
            retain_log = true;
            
            _logger.debug("Retaining log entries after snapshot", {
                {"node_id", node_id_to_string(_node_id)},
                {"snapshot_index", std::to_string(request.last_included_index())},
                {"last_log_index", std::to_string(get_last_log_index())}
            });
        }
    }
    
    // Rule 7: Discard the entire log if no matching entry exists
    if (!retain_log) {
        _logger.info("Discarding entire log for snapshot installation", {
            {"node_id", node_id_to_string(_node_id)},
            {"old_log_size", std::to_string(_log.size())},
            {"snapshot_index", std::to_string(request.last_included_index())}
        });
        
        _log.clear();
        _persistence.truncate_log(1);  // Clear all log entries
    } else {
        // Retain entries after snapshot index
        auto entries_to_remove = request.last_included_index();
        if (entries_to_remove > 0 && entries_to_remove <= _log.size()) {
            _log.erase(_log.begin(), _log.begin() + entries_to_remove);
            _persistence.truncate_log(request.last_included_index() + 1);
        }
    }
    
    // Rule 8: Reset state machine using snapshot contents
    // Call install_snapshot method which will handle state machine restoration
    install_snapshot(snap);
    
    // Update commit index and last applied to snapshot's last included index
    _commit_index = std::max(_commit_index, request.last_included_index());
    _last_applied = request.last_included_index();
    
    // Persist snapshot
    _persistence.save_snapshot(snap);
    _persistence.save_current_term(_current_term);
    
    // Clean up snapshot buffer
    snapshot_buffers.erase(_node_id);
    
    _logger.info("Successfully installed snapshot", {
        {"node_id", node_id_to_string(_node_id)},
        {"last_included_index", std::to_string(request.last_included_index())},
        {"last_included_term", std::to_string(request.last_included_term())},
        {"commit_index", std::to_string(_commit_index)},
        {"last_applied", std::to_string(_last_applied)},
        {"remaining_log_entries", std::to_string(_log.size())}
    });
    
    _metrics.set_metric_name("raft_install_snapshot_success");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("snapshot_size", std::to_string(complete_snapshot_data.size()));
    _metrics.add_one();
    _metrics.emit();
    
    // Rule 9: Reply with current term
    return install_snapshot_response_type{_current_term};
}


template<raft_types Types>

auto node<Types>::reset_election_timer() -> void {
    _last_heartbeat = std::chrono::steady_clock::now();
}

template<raft_types Types>

auto node<Types>::election_timeout_elapsed() const -> bool {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_heartbeat);
    return elapsed >= _election_timeout;
}

template<raft_types Types>

auto node<Types>::heartbeat_timeout_elapsed() const -> bool {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_heartbeat);
    return elapsed >= _heartbeat_interval;
}

template<raft_types Types>


auto node<Types>::send_heartbeats() -> void {
    // Only leaders send heartbeats
    if (_state != kythira::server_state::leader) {
        return;
    }
    
    _logger.debug("Sending heartbeats/replication to followers", {
        {"node_id", node_id_to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"commit_index", std::to_string(_commit_index)}
    });
    
    // In Raft, heartbeats are actually AppendEntries RPCs that may contain log entries
    // So we use replicate_to_followers() which will send AppendEntries with any pending entries
    // or empty AppendEntries (heartbeats) if there are no pending entries
    replicate_to_followers();
    
    // Update last heartbeat timestamp
    _last_heartbeat = std::chrono::steady_clock::now();
    
    _metrics.set_metric_name("raft_heartbeat_sent");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_one();
    _metrics.emit();
}


template<raft_types Types>
auto node<Types>::send_heartbeat_with_retry(node_id_type target) -> void {
    // Create a lambda that sends the heartbeat (AppendEntries with empty entries)
    auto send_heartbeat_operation = [this, target]() -> kythira::Future<append_entries_response_type> {
        // Get next index for this follower
        auto next_idx = _next_index[target];
        
        // Calculate prevLogIndex and prevLogTerm
        log_index_type prev_log_index = next_idx - 1;
        term_id_type prev_log_term = term_id_type{0};
        
        if (prev_log_index > 0) {
            auto prev_entry = get_log_entry(prev_log_index);
            if (prev_entry.has_value()) {
                prev_log_term = prev_entry->term();
            } else {
                // Previous entry not in log (compacted) - return error
                return kythira::FutureFactory::makeExceptionalFuture<append_entries_response_type>(
                    std::runtime_error("Previous entry not in log, need snapshot"));
            }
        }
        
        // Create empty AppendEntries request (heartbeat)
        append_entries_request_type request{
            _current_term,
            _node_id,
            prev_log_index,
            prev_log_term,
            std::vector<log_entry_type>{},  // Empty entries for heartbeat
            _commit_index
        };
        
        _logger.debug("Sending heartbeat with retry", {
            {"node_id", node_id_to_string(_node_id)},
            {"target", node_id_to_string(target)},
            {"term", std::to_string(_current_term)},
            {"prev_log_index", std::to_string(prev_log_index)},
            {"leader_commit", std::to_string(_commit_index)}
        });
        
        // Send RPC with timeout
        auto timeout = _config.append_entries_timeout();
        return _network_client.send_append_entries(target, request, timeout);
    };
    
    // Wrap the operation with error handler for exponential backoff retry
    auto start_time = std::chrono::steady_clock::now();
    
    _append_entries_error_handler.execute_with_retry(
        "heartbeat",
        send_heartbeat_operation,
        std::nullopt  // Use default heartbeat retry policy
    ).thenTry([this, target, start_time](kythira::Try<append_entries_response_type> try_response) {
        std::lock_guard<std::mutex> lock(_mutex);
        
        // Calculate RPC latency
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        _metrics.set_metric_name("raft_heartbeat_latency");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("target", node_id_to_string(target));
        _metrics.add_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(latency));
        _metrics.emit();
        
        if (try_response.hasException()) {
            // Heartbeat failed after all retries - log error and mark follower as unresponsive
            _logger.warning("Heartbeat failed after retries", {
                {"node_id", node_id_to_string(_node_id)},
                {"target", node_id_to_string(target)},
                {"latency_ms", std::to_string(latency.count())}
            });
            
            _unresponsive_followers.insert(target);
            
            _metrics.set_metric_name("raft_heartbeat_failed");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("target", node_id_to_string(target));
            _metrics.add_one();
            _metrics.emit();
            
            return;
        }
        
        auto response = try_response.value();
        
        // Check if we've been deposed
        if (response.term() > _current_term) {
            _logger.info("Discovered higher term in heartbeat response, stepping down", {
                {"node_id", node_id_to_string(_node_id)},
                {"old_term", std::to_string(_current_term)},
                {"new_term", std::to_string(response.term())}
            });
            
            become_follower(response.term());
            return;
        }
        
        // Only process response if we're still leader in the same term
        if (_state != kythira::server_state::leader || response.term() != _current_term) {
            return;
        }
        
        if (response.success()) {
            // Heartbeat succeeded - remove from unresponsive set
            _unresponsive_followers.erase(target);
            
            _logger.debug("Heartbeat succeeded", {
                {"node_id", node_id_to_string(_node_id)},
                {"target", node_id_to_string(target)},
                {"latency_ms", std::to_string(latency.count())}
            });
            
            _metrics.set_metric_name("raft_heartbeat_success");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("target", node_id_to_string(target));
            _metrics.add_one();
            _metrics.emit();
        } else {
            // Heartbeat failed due to log inconsistency - will be handled by normal replication
            _logger.debug("Heartbeat failed due to log inconsistency", {
                {"node_id", node_id_to_string(_node_id)},
                {"target", node_id_to_string(target)}
            });
        }
    });
}


template<raft_types Types>

auto node<Types>::become_follower(term_id_type new_term) -> void {
    auto old_state = _state;
    auto old_term = _current_term;
    
    _logger.info("Transitioning to follower", {
        {"node_id", node_id_to_string(_node_id)},
        {"old_term", std::to_string(_current_term)},
        {"new_term", std::to_string(new_term)}
    });
    
    // If we were a leader, cancel all pending client operations
    if (old_state == kythira::server_state::leader) {
        _logger.info("Leadership lost, cancelling pending operations", {
            {"node_id", node_id_to_string(_node_id)},
            {"old_term", std::to_string(old_term)},
            {"new_term", std::to_string(new_term)},
            {"pending_count", std::to_string(_commit_waiter.get_pending_count())}
        });
        
        // Cancel all pending operations with leadership lost exception
        _commit_waiter.cancel_all_operations_leadership_lost(old_term, new_term);
        
        // Emit leadership lost metric
        _metrics.set_metric_name("leadership_lost");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("old_term", std::to_string(old_term));
        _metrics.add_dimension("new_term", std::to_string(new_term));
        _metrics.add_one();
        _metrics.emit();
    }
    
    _current_term = new_term;
    _state = kythira::server_state::follower;
    _voted_for = std::nullopt;
    
    // Reset election timer
    reset_election_timer();
    randomize_election_timeout();
}

template<raft_types Types>

auto node<Types>::become_candidate() -> void {
    auto old_state = _state;
    
    _logger.info("Transitioning to candidate and starting election", {
        {"node_id", node_id_to_string(_node_id)},
        {"old_term", std::to_string(_current_term)},
        {"new_term", std::to_string(_current_term + 1)}
    });
    
    // Increment current term
    _current_term = _current_term + 1;
    _state = kythira::server_state::candidate;
    
    // Vote for self
    _voted_for = _node_id;
    
    // Persist state before sending RequestVote RPCs
    _persistence.save_current_term(_current_term);
    _persistence.save_voted_for(_node_id);
    
    // Reset election timer with new randomized timeout
    reset_election_timer();
    randomize_election_timeout();
    
    // Send RequestVote RPCs to all other nodes
    start_election();
}

template<raft_types Types>

auto node<Types>::start_election() -> void {
    _logger.info("Starting election", {
        {"node_id", node_id_to_string(_node_id)},
        {"term", std::to_string(_current_term)}
    });
    
    // Emit election started metric
    _metrics.set_metric_name("election_started");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("term", std::to_string(_current_term));
    _metrics.add_one();
    _metrics.emit();
    
    // Configure retry policy for RequestVote RPCs
    typename error_handler<request_vote_response_type>::retry_policy vote_retry_policy{
        .initial_delay = std::chrono::milliseconds{100},
        .max_delay = std::chrono::milliseconds{3000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.1,
        .max_attempts = 3
    };
    
    // Create error handler for RequestVote operations
    error_handler<request_vote_response_type> vote_error_handler;
    vote_error_handler.set_retry_policy("request_vote", vote_retry_policy);
    
    // Send RequestVote RPCs to all peers with retry logic
    std::vector<kythira::Future<request_vote_response_type>> vote_futures;
    vote_futures.reserve(_configuration.nodes().size() - 1);
    
    for (const auto& peer_id : _configuration.nodes()) {
        if (peer_id == _node_id) {
            continue;  // Skip self (already voted for self)
        }
        
        // Create RequestVote request
        request_vote_request_type vote_request{
            ._term = _current_term,
            ._candidate_id = _node_id,
            ._last_log_index = get_last_log_index(),
            ._last_log_term = get_last_log_term()
        };
        
        // Wrap RequestVote RPC call with retry logic
        auto vote_future = vote_error_handler.execute_with_retry(
            "request_vote",
            [this, peer_id, vote_request]() -> kythira::Future<request_vote_response_type> {
                // Log vote request attempt
                _logger.debug("Sending RequestVote RPC", {
                    {"node_id", node_id_to_string(_node_id)},
                    {"peer_id", node_id_to_string(peer_id)},
                    {"term", std::to_string(vote_request._term)},
                    {"last_log_index", std::to_string(vote_request._last_log_index)},
                    {"last_log_term", std::to_string(vote_request._last_log_term)}
                });
                
                // Emit vote request metric
                _metrics.set_metric_name("vote_request_sent");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_dimension("peer_id", node_id_to_string(peer_id));
                _metrics.add_one();
                _metrics.emit();
                
                return _network_client.send_request_vote(
                    peer_id,
                    vote_request,
                    _config.rpc_timeout()
                );
            },
            vote_retry_policy
        ).thenTry([this, peer_id](kythira::Try<request_vote_response_type> result) -> request_vote_response_type {
            if (result.hasException()) {
                // Log vote request failure
                _logger.warning("RequestVote RPC failed after retries", {
                    {"node_id", node_id_to_string(_node_id)},
                    {"peer_id", node_id_to_string(peer_id)},
                    {"error", "Exception occurred"}
                });
                
                // Emit vote request failure metric
                _metrics.set_metric_name("vote_request_failed");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_dimension("peer_id", node_id_to_string(peer_id));
                _metrics.add_one();
                _metrics.emit();
                
                // Rethrow the exception
                std::rethrow_exception(result.exception());
            }
            
            // Log successful vote response
            auto response = result.value();
            _logger.debug("Received RequestVote response", {
                {"node_id", node_id_to_string(_node_id)},
                {"peer_id", node_id_to_string(peer_id)},
                {"vote_granted", response.vote_granted() ? "true" : "false"},
                {"response_term", std::to_string(response.term())}
            });
            
            // Emit vote response metric
            _metrics.set_metric_name("vote_response_received");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("peer_id", node_id_to_string(peer_id));
            _metrics.add_dimension("vote_granted", response.vote_granted() ? "true" : "false");
            _metrics.add_one();
            _metrics.emit();
            
            return response;
        });
        
        vote_futures.push_back(std::move(vote_future));
    }
    
    // If there are no peers (single-node cluster), become leader immediately
    if (vote_futures.empty()) {
        _logger.info("Single-node cluster, becoming leader immediately", {
            {"node_id", node_id_to_string(_node_id)}
        });
        become_leader();
        return;
    }
    
    // Collect majority of vote responses
    auto current_term = _current_term;
    auto node_id = _node_id;
    auto& logger = _logger;
    auto& metrics = _metrics;
    auto& state = _state;
    
    election_collector_t::collect_majority(
        std::move(vote_futures),
        _election_timeout
    ).thenValue([this, current_term, node_id, &logger, &metrics, &state](std::vector<request_vote_response_type> responses) {
        std::lock_guard<std::mutex> lock(_mutex);
        
        // Check if we're still a candidate in the same term
        if (_state != kythira::server_state::candidate || _current_term != current_term) {
            logger.debug("Election outcome irrelevant, state changed", {
                {"node_id", node_id_to_string(node_id)},
                {"current_state", _state == kythira::server_state::follower ? "follower" : "leader"},
                {"current_term", std::to_string(_current_term)},
                {"election_term", std::to_string(current_term)}
            });
            return;
        }
        
        // Count votes (including self-vote)
        std::size_t votes_granted = 1;  // Self-vote
        
        for (const auto& response : responses) {
            // Check for higher term
            if (response.term() > _current_term) {
                logger.info("Discovered higher term during election, stepping down", {
                    {"node_id", node_id_to_string(node_id)},
                    {"current_term", std::to_string(_current_term)},
                    {"discovered_term", std::to_string(response.term())}
                });
                
                become_follower(response.term());
                
                // Emit election lost metric
                metrics.set_metric_name("election_lost");
                metrics.add_dimension("node_id", node_id_to_string(node_id));
                metrics.add_dimension("reason", "higher_term_discovered");
                metrics.add_one();
                metrics.emit();
                
                return;
            }
            
            if (response.vote_granted()) {
                votes_granted++;
            }
        }
        
        // Calculate majority
        const std::size_t total_nodes = _configuration.nodes().size();
        const std::size_t majority = (total_nodes / 2) + 1;
        
        if (votes_granted >= majority) {
            logger.info("Election won, transitioning to leader", {
                {"node_id", node_id_to_string(node_id)},
                {"term", std::to_string(_current_term)},
                {"votes_received", std::to_string(votes_granted)},
                {"total_nodes", std::to_string(total_nodes)}
            });
            
            become_leader();
            
            // Emit election won metric
            metrics.set_metric_name("election_won");
            metrics.add_dimension("node_id", node_id_to_string(node_id));
            metrics.add_dimension("term", std::to_string(_current_term));
            metrics.add_one();
            metrics.emit();
        } else {
            logger.info("Election failed, insufficient votes", {
                {"node_id", node_id_to_string(node_id)},
                {"term", std::to_string(_current_term)},
                {"votes_received", std::to_string(votes_granted)},
                {"votes_needed", std::to_string(majority)}
            });
            
            // Remain as candidate, will retry on next election timeout
            // Emit election lost metric
            metrics.set_metric_name("election_lost");
            metrics.add_dimension("node_id", node_id_to_string(node_id));
            metrics.add_dimension("reason", "insufficient_votes");
            metrics.add_one();
            metrics.emit();
        }
    }).thenError([this, current_term, node_id, &logger, &metrics](std::exception_ptr ex) {
        std::lock_guard<std::mutex> lock(_mutex);
        
        try {
            std::rethrow_exception(ex);
        } catch (const kythira::future_collection_exception& e) {
            logger.warning("Failed to collect vote majority", {
                {"node_id", node_id_to_string(node_id)},
                {"operation", e.get_operation()},
                {"failed_count", std::to_string(e.get_failed_count())}
            });
            
            // Emit election lost metric
            metrics.set_metric_name("election_lost");
            metrics.add_dimension("node_id", node_id_to_string(node_id));
            metrics.add_dimension("reason", "collection_failure");
            metrics.add_one();
            metrics.emit();
            
            // Remain as candidate, will retry on next election timeout
        } catch (...) {
            logger.error("Unexpected error during election", {
                {"node_id", node_id_to_string(node_id)}
            });
        }
    });
}

template<raft_types Types>

auto node<Types>::become_leader() -> void {
    auto old_state = _state;
    
    _logger.info("Transitioning to leader", {
        {"node_id", node_id_to_string(_node_id)},
        {"term", std::to_string(_current_term)}
    });
    
    _state = kythira::server_state::leader;
    
    // Initialize leader-specific state
    auto last_log_idx = get_last_log_index();
    for (const auto& peer_id : _configuration.nodes()) {
        if (peer_id != _node_id) {
            _next_index[peer_id] = last_log_idx + 1;
            _match_index[peer_id] = 0;
        }
    }
    
    // Reset heartbeat timer
    _last_heartbeat = std::chrono::steady_clock::now();
}

template<raft_types Types>

auto node<Types>::get_last_log_index() const -> log_index_type {
    // If log is empty, return commit_index (which accounts for snapshots)
    if (_log.empty()) {
        return _commit_index;
    }
    return _log.back().index();
}

template<raft_types Types>

auto node<Types>::get_last_log_term() const -> term_id_type {
    // If log is empty, return 0 (no entries)
    if (_log.empty()) {
        return term_id_type{0};
    }
    return _log.back().term();
}

// Placeholder implementations for remaining methods
template<raft_types Types>

auto node<Types>::append_log_entry(const log_entry_type& entry) -> void {
    _log.push_back(entry);
}

template<raft_types Types>


auto node<Types>::get_log_entry(log_index_type index) const -> std::optional<log_entry_type> {
    // Handle invalid index
    if (index == 0) {
        return std::nullopt;
    }
    
    // Check if index is within our log bounds
    // Note: Log indices are 1-based, but vector indices are 0-based
    if (_log.empty()) {
        return std::nullopt;
    }
    
    // Get the first log entry's index (may not be 1 if log has been compacted)
    auto first_log_index = _log.empty() ? log_index_type{1} : _log.front().index();
    auto last_log_index = get_last_log_index();
    
    // Check if index is before our log (compacted by snapshot)
    if (index < first_log_index) {
        // Entry has been compacted into a snapshot
        return std::nullopt;
    }
    
    // Check if index is beyond our log
    if (index > last_log_index) {
        // Entry doesn't exist yet
        return std::nullopt;
    }
    
    // Calculate vector index from log index
    // If first_log_index is 1, then log index 1 is at vector index 0
    // If first_log_index is 100 (after compaction), then log index 100 is at vector index 0
    auto vector_index = index - first_log_index;
    
    // Bounds check
    if (vector_index >= _log.size()) {
        return std::nullopt;
    }
    
    // Return the entry
    return _log[vector_index];
}


template<raft_types Types>


auto node<Types>::replicate_to_followers() -> void {
    // Only leaders replicate to followers
    if (_state != kythira::server_state::leader) {
        return;
    }
    
    _logger.debug("Replicating to followers", {
        {"node_id", node_id_to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"last_log_index", std::to_string(get_last_log_index())},
        {"commit_index", std::to_string(_commit_index)}
    });
    
    // Get all nodes in the cluster except ourselves
    std::vector<node_id_type> followers;
    for (const auto& peer_id : _configuration.nodes()) {
        if (peer_id != _node_id) {
            followers.push_back(peer_id);
        }
    }
    
    if (followers.empty()) {
        _logger.debug("No followers to replicate to", {
            {"node_id", node_id_to_string(_node_id)}
        });
        return;
    }
    
    // Send AppendEntries or InstallSnapshot to each follower in parallel
    for (const auto& follower_id : followers) {
        // Check if follower needs snapshot (next_index is too far behind)
        auto next_idx = _next_index[follower_id];
        auto first_log_index = _log.empty() ? log_index_type{1} : _log.front().index();
        
        if (next_idx < first_log_index) {
            // Follower is too far behind - send snapshot
            _logger.debug("Follower needs snapshot", {
                {"node_id", node_id_to_string(_node_id)},
                {"follower", node_id_to_string(follower_id)},
                {"next_index", std::to_string(next_idx)},
                {"first_log_index", std::to_string(first_log_index)}
            });
            
            send_install_snapshot_to(follower_id);
        } else {
            // Send AppendEntries
            send_append_entries_to(follower_id);
        }
    }
    
    // After sending to all followers, check if we can advance commit index
    advance_commit_index();
    
    _metrics.set_metric_name("raft_replication_round");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("num_followers", std::to_string(followers.size()));
    _metrics.add_one();
    _metrics.emit();
}


template<raft_types Types>


auto node<Types>::send_append_entries_to(node_id_type target) -> void {
    // Only leaders send AppendEntries
    if (_state != kythira::server_state::leader) {
        return;
    }
    
    // Get next index for this follower
    auto next_idx = _next_index[target];
    auto last_log_idx = get_last_log_index();
    
    // Calculate prevLogIndex and prevLogTerm
    log_index_type prev_log_index = next_idx - 1;
    term_id_type prev_log_term = term_id_type{0};
    
    if (prev_log_index > 0) {
        auto prev_entry = get_log_entry(prev_log_index);
        if (prev_entry.has_value()) {
            prev_log_term = prev_entry->term();
        } else {
            // Previous entry not in log (compacted) - need to send snapshot instead
            _logger.debug("Previous entry not in log, switching to snapshot", {
                {"node_id", node_id_to_string(_node_id)},
                {"target", node_id_to_string(target)},
                {"prev_log_index", std::to_string(prev_log_index)}
            });
            send_install_snapshot_to(target);
            return;
        }
    }
    
    // Collect entries to send (from next_idx to end of log, up to max batch size)
    std::vector<log_entry_type> entries_to_send;
    auto max_entries = _config.max_entries_per_append();
    
    for (log_index_type idx = next_idx; idx <= last_log_idx && entries_to_send.size() < max_entries; ++idx) {
        auto entry = get_log_entry(idx);
        if (entry.has_value()) {
            entries_to_send.push_back(entry.value());
        } else {
            // Entry not found - should not happen
            _logger.error("Log entry not found during replication", {
                {"node_id", node_id_to_string(_node_id)},
                {"index", std::to_string(idx)}
            });
            break;
        }
    }
    
    // Create AppendEntries request
    append_entries_request_type request{
        _current_term,
        _node_id,
        prev_log_index,
        prev_log_term,
        entries_to_send,
        _commit_index
    };
    
    _logger.debug("Sending AppendEntries", {
        {"node_id", node_id_to_string(_node_id)},
        {"target", node_id_to_string(target)},
        {"term", std::to_string(_current_term)},
        {"prev_log_index", std::to_string(prev_log_index)},
        {"prev_log_term", std::to_string(prev_log_term)},
        {"num_entries", std::to_string(entries_to_send.size())},
        {"leader_commit", std::to_string(_commit_index)}
    });
    
    // Send RPC with timeout and error handling using ErrorHandler for retry with exponential backoff
    auto timeout = _config.append_entries_timeout();
    auto start_time = std::chrono::steady_clock::now();
    
    try {
        // Wrap the RPC call in a lambda for ErrorHandler::execute_with_retry
        auto rpc_operation = [this, target, request, timeout]() -> kythira::Future<append_entries_response_type> {
            return _network_client.send_append_entries(target, request, timeout);
        };
        
        // Execute with retry using ErrorHandler (exponential backoff with jitter)
        auto response_future = _append_entries_error_handler.execute_with_retry(
            "append_entries",
            rpc_operation
        );
        
        // Handle response asynchronously using thenTry to get folly::Try<response>
        std::move(response_future).thenTry([this, target, next_idx, entries_to_send, start_time](auto try_response) {
            std::lock_guard<std::mutex> lock(_mutex);
            
            // Calculate RPC latency (including retry delays)
            auto end_time = std::chrono::steady_clock::now();
            auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            _metrics.set_metric_name("raft_append_entries_latency");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("target", node_id_to_string(target));
            _metrics.add_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(latency));
            _metrics.emit();
            
            if (try_response.hasException()) {
                // RPC failed after all retry attempts - log error and mark follower as unresponsive
                try {
                    std::rethrow_exception(try_response.exception());
                } catch (const std::exception& e) {
                    auto error_msg = std::format("AppendEntries RPC failed after retries: node_id={}, target={}, error={}",
                        node_id_to_string(_node_id),
                        node_id_to_string(target),
                        e.what());
                    _logger.warning(error_msg);
                }
                
                _unresponsive_followers.insert(target);
                
                _metrics.set_metric_name("raft_append_entries_failed");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_dimension("target", node_id_to_string(target));
                _metrics.add_one();
                _metrics.emit();
                
                return;
            }
            
            auto response = try_response.value();
            
            // Check if we've been deposed
            if (response.term() > _current_term) {
                auto msg = std::format(
                    "Discovered higher term in AppendEntries response, stepping down: node_id={}, old_term={}, new_term={}",
                    node_id_to_string(_node_id),
                    _current_term,
                    response.term()
                );
                _logger.info(msg);
                
                become_follower(response.term());
                return;
            }
            
            // Only process response if we're still leader in the same term
            if (_state != kythira::server_state::leader || response.term() != _current_term) {
                return;
            }
            
            if (response.success()) {
                // Success - update next_index and match_index
                auto new_match_index = next_idx + entries_to_send.size() - 1;
                _next_index[target] = new_match_index + 1;
                _match_index[target] = new_match_index;
                
                // Remove from unresponsive set
                _unresponsive_followers.erase(target);
                
                _logger.debug("AppendEntries succeeded", {
                    {"node_id", node_id_to_string(_node_id)},
                    {"target", node_id_to_string(target)},
                    {"match_index", std::to_string(new_match_index)},
                    {"next_index", std::to_string(_next_index[target])}
                });
                
                _metrics.set_metric_name("raft_entries_replicated");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_dimension("target", node_id_to_string(target));
                _metrics.add_count(entries_to_send.size());
                _metrics.emit();
                
                // Try to advance commit index
                advance_commit_index();
            } else {
                // Failure - decrement next_index and retry
                _logger.debug("AppendEntries rejected, decrementing next_index", {
                    {"node_id", node_id_to_string(_node_id)},
                    {"target", node_id_to_string(target)},
                    {"old_next_index", std::to_string(_next_index[target])}
                });
                
                // Use conflict information if available for faster backtracking
                if (response.conflict_index().has_value()) {
                    _next_index[target] = response.conflict_index().value();
                } else {
                    // Decrement by 1 (conservative approach)
                    if (_next_index[target] > 1) {
                        _next_index[target]--;
                    }
                }
                
                _logger.debug("Retrying with new next_index", {
                    {"node_id", node_id_to_string(_node_id)},
                    {"target", node_id_to_string(target)},
                    {"new_next_index", std::to_string(_next_index[target])}
                });
                
                // Retry immediately
                send_append_entries_to(target);
            }
        });
        
    } catch (const std::exception& e) {
        _logger.error("Exception sending AppendEntries", {
            {"node_id", node_id_to_string(_node_id)},
            {"target", node_id_to_string(target)},
            {"error", e.what()}
        });
        
        _unresponsive_followers.insert(target);
    }
}


template<raft_types Types>


auto node<Types>::send_install_snapshot_to(node_id_type target) -> void {
    // Only leaders send InstallSnapshot
    if (_state != kythira::server_state::leader) {
        return;
    }
    
    _logger.info("Sending InstallSnapshot to follower", {
        {"node_id", node_id_to_string(_node_id)},
        {"target", node_id_to_string(target)},
        {"term", std::to_string(_current_term)}
    });
    
    // Load snapshot from persistence
    auto snapshot_opt = _persistence.load_snapshot();
    if (!snapshot_opt.has_value()) {
        _logger.error("No snapshot available to send", {
            {"node_id", node_id_to_string(_node_id)},
            {"target", node_id_to_string(target)}
        });
        return;
    }
    
    auto& snap = snapshot_opt.value();
    const auto& snapshot_data = snap.state_machine_state();
    auto chunk_size = _config.snapshot_chunk_size();
    
    _logger.debug("Snapshot details", {
        {"node_id", node_id_to_string(_node_id)},
        {"last_included_index", std::to_string(snap.last_included_index())},
        {"last_included_term", std::to_string(snap.last_included_term())},
        {"snapshot_size", std::to_string(snapshot_data.size())},
        {"chunk_size", std::to_string(chunk_size)}
    });
    
    // Send snapshot in chunks
    std::size_t offset = 0;
    std::size_t total_chunks = (snapshot_data.size() + chunk_size - 1) / chunk_size;
    std::size_t chunk_num = 0;
    
    while (offset < snapshot_data.size()) {
        // Calculate chunk size for this iteration
        auto remaining = snapshot_data.size() - offset;
        auto current_chunk_size = std::min(remaining, chunk_size);
        
        // Extract chunk data
        std::vector<std::byte> chunk_data(
            snapshot_data.begin() + offset,
            snapshot_data.begin() + offset + current_chunk_size
        );
        
        // Determine if this is the last chunk
        bool is_last_chunk = (offset + current_chunk_size >= snapshot_data.size());
        
        // Create InstallSnapshot request
        install_snapshot_request_type request{
            _current_term,
            _node_id,
            snap.last_included_index(),
            snap.last_included_term(),
            offset,
            chunk_data,
            is_last_chunk
        };
        
        _logger.debug("Sending snapshot chunk", {
            {"node_id", node_id_to_string(_node_id)},
            {"target", node_id_to_string(target)},
            {"chunk", std::to_string(chunk_num + 1)},
            {"total_chunks", std::to_string(total_chunks)},
            {"offset", std::to_string(offset)},
            {"chunk_size", std::to_string(current_chunk_size)},
            {"is_last", is_last_chunk ? "true" : "false"}
        });
        
        // Send RPC with timeout and retry logic using ErrorHandler
        auto timeout = _config.install_snapshot_timeout();
        auto start_time = std::chrono::steady_clock::now();
        
        try {
            // Wrap the RPC call in a lambda for ErrorHandler::execute_with_retry
            auto rpc_operation = [this, target, request, timeout]() -> kythira::Future<install_snapshot_response_type> {
                return _network_client.send_install_snapshot(target, request, timeout);
            };
            
            // Execute with retry using ErrorHandler (exponential backoff with jitter)
            // For snapshots, use longer delays and more attempts due to larger data transfers
            auto response_future = _install_snapshot_error_handler.execute_with_retry(
                "install_snapshot",
                rpc_operation
            );
            
            // Wait for response synchronously (snapshot transfer is sequential)
            // Wrap in try-catch to handle exceptions
            install_snapshot_response_type response;
            try {
                response = std::move(response_future).get();
            } catch (const std::exception& e) {
                _logger.error(std::format("InstallSnapshot RPC failed after retries: node_id={}, target={}, chunk={}/{}, offset={}, error={}",
                    node_id_to_string(_node_id),
                    node_id_to_string(target),
                    chunk_num + 1,
                    total_chunks,
                    offset,
                    e.what()));
                
                _metrics.set_metric_name("raft_install_snapshot_failed");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_dimension("target", node_id_to_string(target));
                _metrics.add_dimension("chunk", std::to_string(chunk_num + 1));
                _metrics.add_dimension("total_chunks", std::to_string(total_chunks));
                _metrics.add_one();
                _metrics.emit();
                
                // Resume capability: Return here to allow retry from this chunk
                // The caller can retry send_install_snapshot_to() and it will start from beginning,
                // but the follower should handle duplicate chunks gracefully
                return;
            }
            
            auto end_time = std::chrono::steady_clock::now();
            auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            _metrics.set_metric_name("raft_install_snapshot_chunk_latency");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("target", node_id_to_string(target));
            _metrics.add_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(latency));
            _metrics.emit();
            
            // Check if we've been deposed
            if (response.term() > _current_term) {
                auto msg = std::format(
                    "Discovered higher term in InstallSnapshot response, stepping down: node_id={}, old_term={}, new_term={}",
                    node_id_to_string(_node_id),
                    _current_term,
                    response.term()
                );
                _logger.info(msg);
                
                become_follower(response.term());
                return;
            }
            
            // Only continue if we're still leader in the same term
            if (_state != kythira::server_state::leader || response.term() != _current_term) {
                return;
            }
            
            _logger.debug("Snapshot chunk sent successfully", {
                {"node_id", node_id_to_string(_node_id)},
                {"target", node_id_to_string(target)},
                {"chunk", std::to_string(chunk_num + 1)}
            });
            
        } catch (const std::exception& e) {
            _logger.error("Exception sending InstallSnapshot chunk", {
                {"node_id", node_id_to_string(_node_id)},
                {"target", node_id_to_string(target)},
                {"chunk", std::to_string(chunk_num + 1)},
                {"total_chunks", std::to_string(total_chunks)},
                {"offset", std::to_string(offset)},
                {"error", e.what()}
            });
            
            _metrics.set_metric_name("raft_install_snapshot_failed");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("target", node_id_to_string(target));
            _metrics.add_dimension("chunk", std::to_string(chunk_num + 1));
            _metrics.add_one();
            _metrics.emit();
            
            // Resume capability: Return here to allow retry from this chunk
            return;
        }
        
        // Move to next chunk
        offset += current_chunk_size;
        chunk_num++;
    }
    
    // Snapshot transfer complete - update next_index and match_index
    std::lock_guard<std::mutex> lock(_mutex);
    _next_index[target] = snap.last_included_index() + 1;
    _match_index[target] = snap.last_included_index();
    
    _logger.info("InstallSnapshot completed successfully", {
        {"node_id", node_id_to_string(_node_id)},
        {"target", node_id_to_string(target)},
        {"last_included_index", std::to_string(snap.last_included_index())},
        {"total_chunks", std::to_string(total_chunks)},
        {"total_bytes", std::to_string(snapshot_data.size())}
    });
    
    _metrics.set_metric_name("raft_install_snapshot_success");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("target", node_id_to_string(target));
    _metrics.add_value(static_cast<double>(snapshot_data.size()));
    _metrics.emit();
}


template<raft_types Types>

auto node<Types>::advance_commit_index() -> void {
    // Only leaders can advance commit index based on replication
    if (_state != kythira::server_state::leader) {
        return;
    }
    
    // Find the highest log index that has been replicated to a majority
    // Start from current commit index and work forward
    auto last_log_idx = get_last_log_index();
    
    for (log_index_type n = _commit_index + 1; n <= last_log_idx; ++n) {
        // Count how many servers have replicated this entry
        // Start with 1 for the leader itself (leader self-acknowledgment)
        std::size_t replication_count = 1;
        
        for (const auto& [peer_id, match_idx] : _match_index) {
            if (match_idx >= n) {
                replication_count++;
            }
        }
        
        // Calculate majority
        const std::size_t total_nodes = _configuration.nodes().size();
        const std::size_t majority = (total_nodes / 2) + 1;
        
        // Check if this entry has been replicated to a majority
        if (replication_count >= majority) {
            // Raft safety requirement: only commit entries from current term directly
            // Entries from previous terms are committed indirectly
            auto entry_opt = get_log_entry(n);
            if (entry_opt.has_value() && entry_opt->term() == _current_term) {
                // Advance commit index
                auto old_commit_index = _commit_index;
                _commit_index = n;
                
                _logger.info("Advanced commit index", {
                    {"node_id", node_id_to_string(_node_id)},
                    {"old_commit_index", std::to_string(old_commit_index)},
                    {"new_commit_index", std::to_string(_commit_index)},
                    {"replication_count", std::to_string(replication_count)},
                    {"majority_required", std::to_string(majority)}
                });
                
                // Emit commit metric
                _metrics.set_metric_name("entries_committed");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_count(_commit_index - old_commit_index);
                _metrics.emit();
                
                // Trigger state machine application
                // This will also notify the commit waiter for each applied entry
                apply_committed_entries();
            } else {
                // Entry is from a previous term, cannot commit directly
                // Will be committed indirectly when an entry from current term is committed
                break;
            }
        } else {
            // This entry hasn't been replicated to majority yet
            // No point checking higher indices
            break;
        }
    }
    
    // Check for slow or unresponsive followers
    for (const auto& [peer_id, match_idx] : _match_index) {
        // If a follower is significantly behind, mark it as potentially unresponsive
        if (_commit_index > match_idx + 100) {  // Arbitrary threshold
            if (_unresponsive_followers.find(peer_id) == _unresponsive_followers.end()) {
                _logger.warning("Follower is lagging significantly", {
                    {"node_id", node_id_to_string(_node_id)},
                    {"follower_id", node_id_to_string(peer_id)},
                    {"follower_match_index", std::to_string(match_idx)},
                    {"commit_index", std::to_string(_commit_index)},
                    {"lag", std::to_string(_commit_index - match_idx)}
                });
                
                _unresponsive_followers.insert(peer_id);
            }
        } else {
            // Follower has caught up, remove from unresponsive set
            _unresponsive_followers.erase(peer_id);
        }
    }
}

template<raft_types Types>

auto node<Types>::apply_committed_entries() -> void {
    // Detect lag condition
    auto lag = _commit_index - _last_applied;
    
    if (lag == 0) {
        // No entries to apply
        return;
    }
    
    // Log catchup status if lag is significant
    if (lag > 10) {
        _logger.info("Applied index catchup needed", {
            {"node_id", node_id_to_string(_node_id)},
            {"last_applied", std::to_string(_last_applied)},
            {"commit_index", std::to_string(_commit_index)},
            {"lag", std::to_string(lag)}
        });
        
        // Emit catchup lag metric
        _metrics.set_metric_name("applied_index_lag");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_value(static_cast<double>(lag));
        _metrics.emit();
    }
    
    // Determine batch size based on lag
    // For small lags, apply all entries
    // For large lags, apply in batches to prevent blocking
    constexpr log_index_type max_batch_size = 100;
    constexpr log_index_type rate_limit_threshold = 50;
    
    auto batch_size = std::min(lag, max_batch_size);
    auto entries_to_apply = batch_size;
    
    // Rate limiting: if lag is large, add small delays between batches
    bool should_rate_limit = lag > rate_limit_threshold;
    constexpr auto rate_limit_delay = std::chrono::milliseconds(10);
    
    _logger.debug("Starting entry application", {
        {"node_id", node_id_to_string(_node_id)},
        {"entries_to_apply", std::to_string(entries_to_apply)},
        {"total_lag", std::to_string(lag)},
        {"rate_limited", should_rate_limit ? "true" : "false"}
    });
    
    auto catchup_start_time = std::chrono::steady_clock::now();
    log_index_type entries_applied_in_batch = 0;
    
    // Apply all entries from last_applied + 1 to commit_index (or batch limit)
    while (_last_applied < _commit_index && entries_applied_in_batch < entries_to_apply) {
        auto next_index = _last_applied + 1;
        
        // Get the log entry to apply
        auto entry_opt = get_log_entry(next_index);
        if (!entry_opt.has_value()) {
            _logger.error("Failed to get log entry for application", {
                {"node_id", node_id_to_string(_node_id)},
                {"entry_index", std::to_string(next_index)},
                {"last_applied", std::to_string(_last_applied)},
                {"commit_index", std::to_string(_commit_index)}
            });
            break;
        }
        
        auto& entry = entry_opt.value();
        
        _logger.debug("Applying log entry to state machine", {
            {"node_id", node_id_to_string(_node_id)},
            {"entry_index", std::to_string(entry.index())},
            {"entry_term", std::to_string(entry.term())},
            {"command_size", std::to_string(entry.command().size())}
        });
        
        auto start_time = std::chrono::steady_clock::now();
        
        try {
            // Apply entry to state machine and capture the result
            auto result = _state_machine.apply(entry.command(), entry.index());
            
            // Update last_applied index after successful application
            _last_applied = next_index;
            entries_applied_in_batch++;
            
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
            
            _logger.debug("Successfully applied log entry", {
                {"node_id", node_id_to_string(_node_id)},
                {"entry_index", std::to_string(entry.index())},
                {"last_applied", std::to_string(_last_applied)},
                {"application_time_us", std::to_string(duration.count())}
            });
            
            // Emit application metric
            _metrics.set_metric_name("entry_applied");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_duration(duration);
            _metrics.emit();
            
            // Notify CommitWaiter that this entry has been committed and applied
            // This will fulfill any pending futures waiting for this entry
            // Use shared_ptr to allow the lambda to be called multiple times
            auto shared_result = std::make_shared<std::vector<std::byte>>(std::move(result));
            _commit_waiter.notify_committed_and_applied(next_index, [shared_result](log_index_type) {
                return *shared_result;
            });
            
            // Emit commit-to-application latency metric
            _metrics.set_metric_name("commit_to_application_latency");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_duration(duration);
            _metrics.emit();
            
        } catch (const std::exception& e) {
            _logger.error("Failed to apply log entry to state machine", {
                {"node_id", node_id_to_string(_node_id)},
                {"entry_index", std::to_string(entry.index())},
                {"entry_term", std::to_string(entry.term())},
                {"command_size", std::to_string(entry.command().size())},
                {"error", e.what()},
                {"error_type", typeid(e).name()}
            });

            
            // Emit application failure metric
            _metrics.set_metric_name("entry_application_failed");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("entry_index", std::to_string(entry.index()));
            _metrics.add_dimension("error_type", typeid(e).name());
            _metrics.add_one();
            _metrics.emit();
            
            // Handle failure according to configured policy
            auto policy = _config.get_application_failure_policy();
            
            if (policy == application_failure_policy::halt) {
                // Halt: Stop applying further entries (safe default)
                _logger.warning("Halting entry application due to failure (policy: halt)", {
                    {"node_id", node_id_to_string(_node_id)},
                    {"failed_entry_index", std::to_string(entry.index())},
                    {"last_applied", std::to_string(_last_applied)}
                });
                
                // Propagate error to pending futures
                auto exception_ptr = std::current_exception();
                _commit_waiter.notify_committed_and_applied(next_index, [exception_ptr](log_index_type) -> std::vector<std::byte> {
                    std::rethrow_exception(exception_ptr);
                });
                
                // Stop applying further entries
                break;
                
            } else if (policy == application_failure_policy::retry) {
                // Retry: Attempt to retry application with exponential backoff
                _logger.warning("Retrying entry application due to failure (policy: retry)", {
                    {"node_id", node_id_to_string(_node_id)},
                    {"failed_entry_index", std::to_string(entry.index())},
                    {"max_attempts", std::to_string(_config.application_retry_max_attempts())}
                });
                
                bool retry_succeeded = false;
                auto delay = _config.application_retry_initial_delay();
                
                for (std::size_t attempt = 1; attempt <= _config.application_retry_max_attempts(); ++attempt) {
                    _logger.debug("Retry attempt for failed entry", {
                        {"node_id", node_id_to_string(_node_id)},
                        {"entry_index", std::to_string(entry.index())},
                        {"attempt", std::to_string(attempt)},
                        {"delay_ms", std::to_string(delay.count())}
                    });
                    
                    // Wait before retrying
                    std::this_thread::sleep_for(delay);
                    
                    try {
                        // Retry applying entry to state machine and capture the result
                        auto result = _state_machine.apply(entry.command(), entry.index());
                        
                        // Update last_applied index after successful retry
                        _last_applied = next_index;
                        retry_succeeded = true;
                        
                        _logger.info("Successfully applied entry after retry", {
                            {"node_id", node_id_to_string(_node_id)},
                            {"entry_index", std::to_string(entry.index())},
                            {"attempt", std::to_string(attempt)}
                        });
                        
                        // Notify CommitWaiter of successful application
                        // Use shared_ptr to allow the lambda to be called multiple times
                        auto shared_result = std::make_shared<std::vector<std::byte>>(std::move(result));
                        _commit_waiter.notify_committed_and_applied(next_index, [shared_result](log_index_type) {
                            return *shared_result;
                        });
                        
                        // Emit retry success metric
                        _metrics.set_metric_name("entry_application_retry_success");
                        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                        _metrics.add_dimension("entry_index", std::to_string(entry.index()));
                        _metrics.add_dimension("attempts", std::to_string(attempt));
                        _metrics.add_one();
                        _metrics.emit();
                        
                        break;
                        
                    } catch (const std::exception& retry_error) {
                        _logger.warning("Retry attempt failed", {
                            {"node_id", node_id_to_string(_node_id)},
                            {"entry_index", std::to_string(entry.index())},
                            {"attempt", std::to_string(attempt)},
                            {"error", retry_error.what()}
                        });
                        
                        // Calculate next delay with exponential backoff
                        delay = std::chrono::milliseconds(
                            static_cast<long long>(delay.count() * _config.application_retry_backoff_multiplier())
                        );
                        delay = std::min(delay, _config.application_retry_max_delay());
                    }
                }
                
                if (!retry_succeeded) {
                    _logger.error("All retry attempts exhausted for entry application", {
                        {"node_id", node_id_to_string(_node_id)},
                        {"entry_index", std::to_string(entry.index())},
                        {"attempts", std::to_string(_config.application_retry_max_attempts())}
                    });
                    
                    // Propagate error to pending futures
                    auto exception_ptr = std::current_exception();
                    _commit_waiter.notify_committed_and_applied(next_index, [exception_ptr](log_index_type) -> std::vector<std::byte> {
                        std::rethrow_exception(exception_ptr);
                    });
                    
                    // Emit retry exhausted metric
                    _metrics.set_metric_name("entry_application_retry_exhausted");
                    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                    _metrics.add_dimension("entry_index", std::to_string(entry.index()));
                    _metrics.add_one();
                    _metrics.emit();
                    
                    // Stop applying further entries after exhausting retries
                    break;
                }
                
            } else if (policy == application_failure_policy::skip) {
                // Skip: Skip failed entry and continue (dangerous!)
                _logger.warning("Skipping failed entry and continuing (policy: skip - DANGEROUS)", {
                    {"node_id", node_id_to_string(_node_id)},
                    {"skipped_entry_index", std::to_string(entry.index())},
                    {"last_applied", std::to_string(_last_applied)}
                });
                
                // Update last_applied to skip this entry
                _last_applied = next_index;
                
                // Propagate error to pending futures for this entry
                auto exception_ptr = std::current_exception();
                _commit_waiter.notify_committed_and_applied(next_index, [exception_ptr](log_index_type) -> std::vector<std::byte> {
                    std::rethrow_exception(exception_ptr);
                });
                
                // Emit skip metric
                _metrics.set_metric_name("entry_application_skipped");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_dimension("entry_index", std::to_string(entry.index()));
                _metrics.add_one();
                _metrics.emit();
                
                // Continue to next entry (dangerous - can lead to inconsistency)
                continue;
            }
        }
        
        // Rate limiting: add small delay between entries if needed
        if (should_rate_limit && entries_applied_in_batch < entries_to_apply) {
            std::this_thread::sleep_for(rate_limit_delay);
        }
    }
    
    // Log catchup completion
    auto catchup_end_time = std::chrono::steady_clock::now();
    auto catchup_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        catchup_end_time - catchup_start_time
    );
    
    if (entries_applied_in_batch > 0) {
        auto remaining_lag = _commit_index - _last_applied;
        
        _logger.info("Applied index catchup batch completed", {
            {"node_id", node_id_to_string(_node_id)},
            {"entries_applied", std::to_string(entries_applied_in_batch)},
            {"last_applied", std::to_string(_last_applied)},
            {"commit_index", std::to_string(_commit_index)},
            {"remaining_lag", std::to_string(remaining_lag)},
            {"duration_ms", std::to_string(catchup_duration.count())}
        });
        
        // Emit catchup throughput metric
        _metrics.set_metric_name("catchup_throughput");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_value(static_cast<double>(entries_applied_in_batch));
        _metrics.emit();
        
        // Emit catchup duration metric
        _metrics.set_metric_name("catchup_duration");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_duration(catchup_duration);
        _metrics.emit();
        
        // Emit remaining lag metric
        if (remaining_lag > 0) {
            _metrics.set_metric_name("catchup_remaining_lag");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_value(static_cast<double>(remaining_lag));
            _metrics.emit();
        }
    }
}

template<raft_types Types>

auto node<Types>::create_snapshot() -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.info("Creating snapshot from current state machine state", {
        {"node_id", node_id_to_string(_node_id)},
        {"last_applied", std::to_string(_last_applied)}
    });
    
    auto start_time = std::chrono::steady_clock::now();
    
    // Capture current state from state machine
    auto state = _state_machine.get_state();
    
    _logger.debug("State machine state captured", {
        {"node_id", node_id_to_string(_node_id)},
        {"state_size", std::to_string(state.size())}
    });
    
    // Get the term of the last applied entry
    term_id_type last_applied_term = term_id_type{0};
    if (_last_applied > 0) {
        auto last_entry = get_log_entry(_last_applied);
        if (last_entry.has_value()) {
            last_applied_term = last_entry->term();
        }
    }
    
    // Create snapshot with captured state
    snapshot_type snap{
        _last_applied,
        last_applied_term,
        _configuration,
        state
    };
    
    // Persist snapshot to storage
    _persistence.save_snapshot(snap);
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    _logger.info("Snapshot created successfully", {
        {"node_id", node_id_to_string(_node_id)},
        {"last_included_index", std::to_string(snap.last_included_index())},
        {"last_included_term", std::to_string(snap.last_included_term())},
        {"state_size", std::to_string(state.size())},
        {"duration_ms", std::to_string(duration.count())}
    });
    
    // Emit metrics for snapshot creation
    _metrics.set_metric_name("raft_snapshot_created");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_value(static_cast<double>(state.size()));
    _metrics.emit();
    
    _metrics.set_metric_name("raft_snapshot_creation_duration");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_duration(duration);
    _metrics.emit();
    
    // Trigger log compaction after successful snapshot creation
    compact_log();
}

template<raft_types Types>


auto node<Types>::create_snapshot(const std::vector<std::byte>& state_machine_state) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.info("Creating snapshot with provided state", {
        {"node_id", node_id_to_string(_node_id)},
        {"last_applied", std::to_string(_last_applied)},
        {"state_size", std::to_string(state_machine_state.size())}
    });
    
    auto start_time = std::chrono::steady_clock::now();
    
    // Get the term of the last applied entry
    term_id_type last_applied_term = term_id_type{0};
    if (_last_applied > 0) {
        auto last_entry = get_log_entry(_last_applied);
        if (last_entry.has_value()) {
            last_applied_term = last_entry->term();
        }
    }
    
    // Create snapshot with provided state
    snapshot_type snap{
        _last_applied,
        last_applied_term,
        _configuration,
        state_machine_state
    };
    
    // Persist snapshot to storage
    _persistence.save_snapshot(snap);
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    _logger.info("Snapshot created successfully", {
        {"node_id", node_id_to_string(_node_id)},
        {"last_included_index", std::to_string(snap.last_included_index())},
        {"last_included_term", std::to_string(snap.last_included_term())},
        {"snapshot_size", std::to_string(state_machine_state.size())},
        {"duration_ms", std::to_string(duration.count())}
    });
    
    _metrics.set_metric_name("raft_snapshot_created");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_value(static_cast<double>(state_machine_state.size()));
    _metrics.emit();
    
    _metrics.set_metric_name("raft_snapshot_creation_duration");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(duration));
    _metrics.emit();
    
    // Trigger log compaction after snapshot creation
    compact_log();
}


template<raft_types Types>


auto node<Types>::compact_log() -> void {
    // Load the most recent snapshot to determine compaction point
    auto snapshot_opt = _persistence.load_snapshot();
    if (!snapshot_opt.has_value()) {
        _logger.debug("No snapshot available for log compaction", {
            {"node_id", node_id_to_string(_node_id)}
        });
        return;
    }
    
    auto& snap = snapshot_opt.value();
    auto last_included_index = snap.last_included_index();
    
    _logger.info("Compacting log", {
        {"node_id", node_id_to_string(_node_id)},
        {"last_included_index", std::to_string(last_included_index)},
        {"current_log_size", std::to_string(_log.size())}
    });
    
    if (_log.empty()) {
        _logger.debug("Log already empty, no compaction needed", {
            {"node_id", node_id_to_string(_node_id)}
        });
        return;
    }
    
    auto first_log_index = _log.front().index();
    
    // Check if we have entries to compact
    if (last_included_index < first_log_index) {
        _logger.debug("Snapshot index is before log start, no compaction needed", {
            {"node_id", node_id_to_string(_node_id)},
            {"last_included_index", std::to_string(last_included_index)},
            {"first_log_index", std::to_string(first_log_index)}
        });
        return;
    }
    
    // Calculate how many entries to remove
    std::size_t entries_to_remove = 0;
    for (const auto& entry : _log) {
        if (entry.index() <= last_included_index) {
            entries_to_remove++;
        } else {
            break;
        }
    }
    
    if (entries_to_remove == 0) {
        _logger.debug("No entries to compact", {
            {"node_id", node_id_to_string(_node_id)}
        });
        return;
    }
    
    auto old_log_size = _log.size();
    
    // Remove entries up to and including last_included_index
    _log.erase(_log.begin(), _log.begin() + entries_to_remove);
    
    // Delete entries from persistence
    _persistence.delete_log_entries_before(last_included_index + 1);
    
    auto new_log_size = _log.size();
    auto entries_removed = old_log_size - new_log_size;
    
    _logger.info("Log compaction completed", {
        {"node_id", node_id_to_string(_node_id)},
        {"entries_removed", std::to_string(entries_removed)},
        {"old_log_size", std::to_string(old_log_size)},
        {"new_log_size", std::to_string(new_log_size)},
        {"last_included_index", std::to_string(last_included_index)}
    });
    
    _metrics.set_metric_name("raft_log_compacted");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_count(entries_removed);
    _metrics.emit();
    
    _metrics.set_metric_name("raft_log_size_after_compaction");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_value(static_cast<double>(new_log_size));
    _metrics.emit();
}


template<raft_types Types>

auto node<Types>::install_snapshot(const snapshot_type& snap) -> void {
    auto start_time = std::chrono::steady_clock::now();
    
    _logger.info("Installing snapshot to state machine", {
        {"node_id", node_id_to_string(_node_id)},
        {"last_included_index", std::to_string(snap.last_included_index())},
        {"last_included_term", std::to_string(snap.last_included_term())},
        {"snapshot_size", std::to_string(snap.state_machine_state().size())}
    });
    
    try {
        // Restore state machine from snapshot
        _state_machine.restore_from_snapshot(snap.state_machine_state(), snap.last_included_index());
        
        // Update last_applied to snapshot's last_included_index
        _last_applied = snap.last_included_index();
        
        // Update commit_index if snapshot index is higher
        if (snap.last_included_index() > _commit_index) {
            _commit_index = snap.last_included_index();
            
            _logger.debug("Updated commit_index from snapshot", {
                {"node_id", node_id_to_string(_node_id)},
                {"new_commit_index", std::to_string(_commit_index)}
            });
        }
        
        // Truncate log based on snapshot's last_included_index
        // Remove all entries up to and including the snapshot's last_included_index
        if (!_log.empty() && snap.last_included_index() > 0) {
            auto entries_to_remove = std::min(
                static_cast<std::size_t>(snap.last_included_index()),
                _log.size()
            );
            
            if (entries_to_remove > 0) {
                _log.erase(_log.begin(), _log.begin() + entries_to_remove);
                
                _logger.debug("Truncated log after snapshot installation", {
                    {"node_id", node_id_to_string(_node_id)},
                    {"entries_removed", std::to_string(entries_to_remove)},
                    {"remaining_entries", std::to_string(_log.size())}
                });
            }
        }
        
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        _logger.info("Successfully installed snapshot to state machine", {
            {"node_id", node_id_to_string(_node_id)},
            {"last_applied", std::to_string(_last_applied)},
            {"commit_index", std::to_string(_commit_index)},
            {"remaining_log_entries", std::to_string(_log.size())},
            {"duration_ms", std::to_string(duration.count())}
        });
        
        // Emit metrics for snapshot installation
        _metrics.set_metric_name("raft_snapshot_installation_duration");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(duration));
        _metrics.emit();
        
        _metrics.set_metric_name("raft_snapshot_installation_success");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("snapshot_size", std::to_string(snap.state_machine_state().size()));
        _metrics.add_one();
        _metrics.emit();
        
    } catch (const std::exception& e) {
        _logger.error("Failed to install snapshot to state machine", {
            {"node_id", node_id_to_string(_node_id)},
            {"error", e.what()},
            {"last_included_index", std::to_string(snap.last_included_index())}
        });
        
        _metrics.set_metric_name("raft_snapshot_installation_failure");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("error_type", "state_machine_error");
        _metrics.add_one();
        _metrics.emit();
        
        // Re-throw the exception to propagate the error
        throw;
    }
}


} // namespace kythira