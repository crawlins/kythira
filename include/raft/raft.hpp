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

private:
    // ========================================================================
    // Implementation methods
    // ========================================================================
    
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
    auto become_leader() -> void;
    
    // Log operations
    auto append_log_entry(const log_entry_type& entry) -> void;
    auto get_last_log_index() const -> log_index_type;
    auto get_last_log_term() const -> term_id_type;
    auto get_log_entry(log_index_type index) const -> std::optional<log_entry_type>;
    
    // Replication
    auto replicate_to_followers() -> void;
    auto send_append_entries_to(node_id_type target) -> void;
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
        {"node_id", std::to_string(_node_id)},
        {"state", "follower"}
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
            {"node_id", std::to_string(_node_id)},
            {"state", _state == kythira::server_state::follower ? "follower" : "candidate"}
        });
        // Return a failed future - this is a placeholder implementation
        return future_type{};
    }
    
    _logger.info("Received client command", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"command_size", std::to_string(command.size())}
    });
    
    // Return success immediately as a temporary implementation
    return future_type{};
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
    std::lock_guard<std::mutex> lock(_mutex);
    
    // Only leaders can serve linearizable reads
    if (_state != kythira::server_state::leader) {
        _logger.debug("Rejected read request: not leader", {
            {"node_id", std::to_string(_node_id)},
            {"state", _state == kythira::server_state::follower ? "follower" : "candidate"}
        });
        // Return a failed future - this is a placeholder implementation
        return future_type{};
    }
    
    // Return success immediately as a temporary implementation
    return future_type{};
}

template<raft_types Types>
auto node<Types>::start() -> void {
    // Check if already running
    if (_running.load(std::memory_order_acquire)) {
        _logger.warning("Attempted to start node that is already running", {
            {"node_id", std::to_string(_node_id)}
        });
        return;
    }
    
    _logger.info("Starting Raft node", {
        {"node_id", std::to_string(_node_id)}
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
        {"node_id", std::to_string(_node_id)},
        {"state", "follower"},
        {"current_term", std::to_string(_current_term)}
    });
}

template<raft_types Types>
auto node<Types>::stop() -> void {
    // Check if already stopped
    if (!_running.load(std::memory_order_acquire)) {
        _logger.warning("Attempted to stop node that is not running", {
            {"node_id", std::to_string(_node_id)}
        });
        return;
    }
    
    _logger.info("Stopping Raft node", {
        {"node_id", std::to_string(_node_id)}
    });
    
    // Mark as not running
    _running.store(false, std::memory_order_release);
    
    // Cancel all pending client operations
    _commit_waiter.cancel_all_operations("Node shutdown");
    
    // Stop the network server
    _network_server.stop();
    
    _logger.info("Raft node stopped successfully", {
        {"node_id", std::to_string(_node_id)}
    });
}

template<raft_types Types>
auto node<Types>::add_server(node_id_type new_node) -> future_type {
    // Placeholder implementation
    _logger.info("Add server requested", {
        {"node_id", std::to_string(_node_id)},
        {"new_node", std::to_string(new_node)}
    });
    return future_type{};
}

template<raft_types Types>
auto node<Types>::remove_server(node_id_type old_node) -> future_type {
    // Placeholder implementation
    _logger.info("Remove server requested", {
        {"node_id", std::to_string(_node_id)},
        {"old_node", std::to_string(old_node)}
    });
    return future_type{};
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
            {"node_id", std::to_string(_node_id)},
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
    
    // Only leaders send heartbeats
    if (_state != kythira::server_state::leader) {
        return;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_heartbeat);
    
    if (elapsed >= _heartbeat_interval) {
        _logger.debug("Heartbeat timeout elapsed, sending heartbeats", {
            {"node_id", std::to_string(_node_id)},
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
        {"node_id", std::to_string(_node_id)}
    });
    
    // Load current term
    _current_term = _persistence.load_current_term();
    
    // Load voted_for
    _voted_for = _persistence.load_voted_for();
    
    _logger.info("Node initialized from storage", {
        {"node_id", std::to_string(_node_id)},
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
        {"node_id", std::to_string(_node_id)}
    });
}

template<raft_types Types>
auto node<Types>::handle_request_vote(const request_vote_request_type& request) -> request_vote_response_type {
    // Placeholder implementation
    _logger.debug("Received RequestVote RPC", {
        {"node_id", std::to_string(_node_id)},
        {"from_candidate", std::to_string(request.candidate_id())},
        {"request_term", std::to_string(request.term())},
        {"current_term", std::to_string(_current_term)}
    });
    
    // Return denial by default (placeholder)
    return request_vote_response_type{_current_term, false};
}

template<raft_types Types>
auto node<Types>::handle_append_entries(const append_entries_request_type& request) -> append_entries_response_type {
    // Placeholder implementation
    _logger.debug("Received AppendEntries RPC", {
        {"node_id", std::to_string(_node_id)},
        {"from_leader", std::to_string(request.leader_id())},
        {"request_term", std::to_string(request.term())},
        {"current_term", std::to_string(_current_term)}
    });
    
    // Reset election timer on valid AppendEntries
    reset_election_timer();
    
    // Return success by default (placeholder)
    return append_entries_response_type{_current_term, true, std::nullopt, std::nullopt};
}

template<raft_types Types>
auto node<Types>::handle_install_snapshot(const install_snapshot_request_type& request) -> install_snapshot_response_type {
    // Placeholder implementation
    _logger.debug("Received InstallSnapshot RPC", {
        {"node_id", std::to_string(_node_id)},
        {"from_leader", std::to_string(request.leader_id())},
        {"request_term", std::to_string(request.term())}
    });
    
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
    _logger.debug("Sending heartbeats to followers", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)}
    });
    
    // Placeholder implementation - would send empty AppendEntries RPCs
}

template<raft_types Types>
auto node<Types>::become_follower(term_id_type new_term) -> void {
    auto old_state = _state;
    
    _logger.info("Transitioning to follower", {
        {"node_id", std::to_string(_node_id)},
        {"old_term", std::to_string(_current_term)},
        {"new_term", std::to_string(new_term)}
    });
    
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
        {"node_id", std::to_string(_node_id)},
        {"old_term", std::to_string(_current_term)},
        {"new_term", std::to_string(_current_term + 1)}
    });
    
    // Increment current term
    _current_term = _current_term + 1;
    _state = kythira::server_state::candidate;
    
    // Vote for self
    _voted_for = _node_id;
    
    // Reset election timer with new randomized timeout
    reset_election_timer();
    randomize_election_timeout();
    
    // Placeholder: would send RequestVote RPCs to all other nodes
}

template<raft_types Types>
auto node<Types>::become_leader() -> void {
    auto old_state = _state;
    
    _logger.info("Transitioning to leader", {
        {"node_id", std::to_string(_node_id)},
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
    // Placeholder implementation
    return std::nullopt;
}

template<raft_types Types>
auto node<Types>::replicate_to_followers() -> void {
    // Placeholder implementation
}

template<raft_types Types>
auto node<Types>::send_append_entries_to(node_id_type target) -> void {
    // Placeholder implementation
}

template<raft_types Types>
auto node<Types>::send_install_snapshot_to(node_id_type target) -> void {
    // Placeholder implementation
}

template<raft_types Types>
auto node<Types>::advance_commit_index() -> void {
    // Placeholder implementation
}

template<raft_types Types>
auto node<Types>::apply_committed_entries() -> void {
    // Placeholder implementation
}

template<raft_types Types>
auto node<Types>::create_snapshot() -> void {
    // Placeholder implementation
}

template<raft_types Types>
auto node<Types>::create_snapshot(const std::vector<std::byte>& state_machine_state) -> void {
    // Placeholder implementation
}

template<raft_types Types>
auto node<Types>::compact_log() -> void {
    // Placeholder implementation
}

template<raft_types Types>
auto node<Types>::install_snapshot(const snapshot_type& snap) -> void {
    // Placeholder implementation
}

} // namespace kythira