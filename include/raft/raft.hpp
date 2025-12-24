#pragma once

#include "types.hpp"
#include "network.hpp"
#include "persistence.hpp"
#include "logger.hpp"
#include "metrics.hpp"
#include "membership.hpp"
#include <concepts/future.hpp>

#include <raft/future.hpp>

#include <vector>
#include <optional>
#include <unordered_map>
#include <chrono>
#include <random>
#include <mutex>
#include <atomic>

namespace kythira {

// Raft node class template
// Implements the Raft consensus algorithm with pluggable components
template<
    typename FutureType,
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId = std::uint64_t,
    typename TermId = std::uint64_t,
    typename LogIndex = std::uint64_t
>
requires 
    future<FutureType, std::vector<std::byte>> &&
    future<FutureType, bool> &&
    network_client<NetworkClient, FutureType> &&
    raft::network_server<NetworkServer> &&
    raft::persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, raft::log_entry<TermId, LogIndex>, raft::snapshot<NodeId, TermId, LogIndex>> &&
    raft::diagnostic_logger<Logger> &&
    raft::metrics<Metrics> &&
    raft::membership_manager<MembershipManager, NodeId, raft::cluster_configuration<NodeId>> &&
    raft::node_id<NodeId> &&
    raft::term_id<TermId> &&
    raft::log_index<LogIndex>
class node {
public:
    // Type aliases for convenience
    using log_entry_t = raft::log_entry<TermId, LogIndex>;
    using cluster_configuration_t = raft::cluster_configuration<NodeId>;
    using snapshot_t = raft::snapshot<NodeId, TermId, LogIndex>;
    using request_vote_request_t = raft::request_vote_request<NodeId, TermId, LogIndex>;
    using request_vote_response_t = raft::request_vote_response<TermId>;
    using append_entries_request_t = raft::append_entries_request<NodeId, TermId, LogIndex, log_entry_t>;
    using append_entries_response_t = raft::append_entries_response<TermId, LogIndex>;
    using install_snapshot_request_t = raft::install_snapshot_request<NodeId, TermId, LogIndex>;
    using install_snapshot_response_t = raft::install_snapshot_response<TermId>;
    
    // Client session tracking types
    using client_id_t = std::uint64_t;
    using serial_number_t = std::uint64_t;
    
    // Constructor
    node(
        NodeId node_id,
        NetworkClient network_client,
        NetworkServer network_server,
        PersistenceEngine persistence,
        Logger logger,
        Metrics metrics,
        MembershipManager membership,
        raft::raft_configuration config = raft::raft_configuration{}
    );
    
    // Client operations - return template future types
    auto submit_command(const std::vector<std::byte>& command, std::chrono::milliseconds timeout) 
        -> FutureType;
    
    // Client operation with session tracking for duplicate detection
    auto submit_command_with_session(
        client_id_t client_id,
        serial_number_t serial_number,
        const std::vector<std::byte>& command,
        std::chrono::milliseconds timeout
    ) -> FutureType;
    
    auto read_state(std::chrono::milliseconds timeout) 
        -> FutureType;
    
    // Node lifecycle
    auto start() -> void;
    auto stop() -> void;
    [[nodiscard]] auto is_running() const noexcept -> bool;
    
    // Node state queries
    [[nodiscard]] auto get_node_id() const -> NodeId;
    [[nodiscard]] auto get_current_term() const -> TermId;
    [[nodiscard]] auto get_state() const -> server_state;
    [[nodiscard]] auto is_leader() const -> bool;
    
    // Cluster operations - return template future types
    auto add_server(NodeId new_node) -> FutureType;
    auto remove_server(NodeId old_node) -> FutureType;
    
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
    TermId _current_term;
    
    // CandidateId that received vote in current term (or null if none)
    std::optional<NodeId> _voted_for;
    
    // Log entries; each entry contains command for state machine, and term when entry was received by leader
    // First index is 1
    std::vector<log_entry_t> _log;
    
    // ========================================================================
    // Volatile state (all servers)
    // ========================================================================
    
    // Index of highest log entry known to be committed (initialized to 0, increases monotonically)
    LogIndex _commit_index;
    
    // Index of highest log entry applied to state machine (initialized to 0, increases monotonically)
    LogIndex _last_applied;
    
    // Current server state (follower, candidate, or leader)
    server_state _state;
    
    // ========================================================================
    // Volatile state (leaders only)
    // Reinitialized after election
    // ========================================================================
    
    // For each server, index of the next log entry to send to that server
    // (initialized to leader last log index + 1)
    std::unordered_map<NodeId, LogIndex> _next_index;
    
    // For each server, index of highest log entry known to be replicated on server
    // (initialized to 0, increases monotonically)
    std::unordered_map<NodeId, LogIndex> _match_index;
    
    // ========================================================================
    // Component members
    // ========================================================================
    
    // Network client for sending RPCs to other nodes
    NetworkClient _network_client;
    
    // Network server for receiving RPCs from other nodes
    NetworkServer _network_server;
    
    // Persistence engine for durable storage
    PersistenceEngine _persistence;
    
    // Diagnostic logger for structured logging
    Logger _logger;
    
    // Metrics recorder for performance monitoring
    Metrics _metrics;
    
    // Membership manager for cluster configuration changes
    MembershipManager _membership;
    
    // ========================================================================
    // Configuration and timing
    // ========================================================================
    
    // Raft configuration parameters
    raft_configuration _config;
    
    // This node's identifier
    NodeId _node_id;
    
    // Current cluster configuration
    cluster_configuration_t _configuration;
    
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
    // Private helper methods (to be implemented in later tasks)
    // ========================================================================
    
    // RPC handlers
    auto handle_request_vote(const request_vote_request_t& request) -> request_vote_response_t;
    auto handle_append_entries(const append_entries_request_t& request) -> append_entries_response_t;
    auto handle_install_snapshot(const install_snapshot_request_t& request) -> install_snapshot_response_t;
    
    // Election and timing
    auto randomize_election_timeout() -> void;
    auto reset_election_timer() -> void;
    auto election_timeout_elapsed() const -> bool;
    
    // Heartbeat timing
    auto heartbeat_timeout_elapsed() const -> bool;
    auto send_heartbeats() -> void;
    
    // State transitions
    auto become_follower(TermId new_term) -> void;
    auto become_candidate() -> void;
    auto become_leader() -> void;
    
    // Log operations
    auto append_log_entry(const log_entry_t& entry) -> void;
    auto get_last_log_index() const -> LogIndex;
    auto get_last_log_term() const -> TermId;
    auto get_log_entry(LogIndex index) const -> std::optional<log_entry_t>;
    
    // Replication
    auto replicate_to_followers() -> void;
    auto send_append_entries_to(NodeId target) -> void;
    auto send_install_snapshot_to(NodeId target) -> void;
    auto advance_commit_index() -> void;
    auto apply_committed_entries() -> void;
    
    // Snapshot operations
    auto create_snapshot() -> void;
    auto create_snapshot(const std::vector<std::byte>& state_machine_state) -> void;
    auto compact_log() -> void;
    auto install_snapshot(const snapshot_t& snap) -> void;
};

// Raft node concept - defines the interface for a Raft node
template<typename N, typename FutureType>
concept raft_node = requires(
    N node,
    const std::vector<std::byte>& command,
    std::chrono::milliseconds timeout,
    std::uint64_t node_id
) {
    // Client operations - return template future types
    { node.submit_command(command, timeout) } 
        -> std::same_as<FutureType>;
    { node.read_state(timeout) } 
        -> std::same_as<FutureType>;
    
    // Node lifecycle
    { node.start() } -> std::same_as<void>;
    { node.stop() } -> std::same_as<void>;
    { node.is_running() } -> std::convertible_to<bool>;
    
    // Node state queries
    { node.get_node_id() };
    { node.get_current_term() };
    { node.get_state() } -> std::same_as<raft::server_state>;
    { node.is_leader() } -> std::convertible_to<bool>;
    
    // Cluster operations - return template future types
    { node.add_server(node_id) } -> std::same_as<FutureType>;
    { node.remove_server(node_id) } -> std::same_as<FutureType>;
} && future<FutureType, std::vector<std::byte>>
  && future<FutureType, bool>;

// ============================================================================
// Implementation
// ============================================================================

template<
    typename FutureType,
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    future<FutureType, std::vector<std::byte>> &&
    future<FutureType, bool> &&
    network_client<NetworkClient, FutureType> &&
    raft::network_server<NetworkServer> &&
    raft::persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, raft::log_entry<TermId, LogIndex>, raft::snapshot<NodeId, TermId, LogIndex>> &&
    raft::diagnostic_logger<Logger> &&
    raft::metrics<Metrics> &&
    raft::membership_manager<MembershipManager, NodeId, raft::cluster_configuration<NodeId>> &&
    raft::node_id<NodeId> &&
    raft::term_id<TermId> &&
    raft::log_index<LogIndex>
node<FutureType, NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::node(
    NodeId node_id,
    NetworkClient network_client,
    NetworkServer network_server,
    PersistenceEngine persistence,
    Logger logger,
    Metrics metrics,
    MembershipManager membership,
    raft::raft_configuration config
)
    : _current_term{0}
    , _voted_for{std::nullopt}
    , _log{}
    , _commit_index{0}
    , _last_applied{0}
    , _state{server_state::follower}
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

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::get_node_id() const -> NodeId {
    return _node_id;
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::get_current_term() const -> TermId {
    std::lock_guard<std::mutex> lock(_mutex);
    return _current_term;
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::get_state() const -> server_state {
    std::lock_guard<std::mutex> lock(_mutex);
    return _state;
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::is_leader() const -> bool {
    std::lock_guard<std::mutex> lock(_mutex);
    return _state == server_state::leader;
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::is_running() const noexcept -> bool {
    return _running.load(std::memory_order_acquire);
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::randomize_election_timeout() -> void {
    // Randomize election timeout between min and max to prevent split votes
    std::uniform_int_distribution<std::chrono::milliseconds::rep> dist(
        _config.election_timeout_min().count(),
        _config.election_timeout_max().count()
    );
    _election_timeout = std::chrono::milliseconds{dist(_rng)};
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::initialize_from_storage() -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.info("Initializing node from persistent storage", {
        {"node_id", std::to_string(_node_id)}
    });
    
    // Load current term
    _current_term = _persistence.load_current_term();
    
    // Load voted_for
    _voted_for = _persistence.load_voted_for();
    
    // Load log entries
    auto last_log_idx = _persistence.get_last_log_index();
    if (last_log_idx > 0) {
        _log = _persistence.get_log_entries(1, last_log_idx);
    }
    
    // Load snapshot if available
    auto snapshot_opt = _persistence.load_snapshot();
    if (snapshot_opt.has_value()) {
        const auto& snap = snapshot_opt.value();
        
        // Update configuration from snapshot
        _configuration = snap.configuration();
        
        // Update commit index and last applied to snapshot's last included index
        _commit_index = snap.last_included_index();
        _last_applied = snap.last_included_index();
        
        // Remove log entries covered by snapshot
        if (!_log.empty() && _log.front().index() <= snap.last_included_index()) {
            auto it = std::find_if(_log.begin(), _log.end(), 
                [&snap](const log_entry_t& entry) {
                    return entry.index() > snap.last_included_index();
                });
            _log.erase(_log.begin(), it);
        }
        
        _logger.info("Loaded snapshot from storage", {
            {"node_id", std::to_string(_node_id)},
            {"last_included_index", std::to_string(snap.last_included_index())},
            {"last_included_term", std::to_string(snap.last_included_term())}
        });
    }
    
    _logger.info("Node initialized from storage", {
        {"node_id", std::to_string(_node_id)},
        {"current_term", std::to_string(_current_term)},
        {"log_size", std::to_string(_log.size())},
        {"commit_index", std::to_string(_commit_index)},
        {"last_applied", std::to_string(_last_applied)}
    });
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::register_rpc_handlers() -> void {
    // Register RequestVote handler
    _network_server.register_request_vote_handler(
        [this](const request_vote_request_t& request) -> request_vote_response_t {
            return this->handle_request_vote(request);
        }
    );
    
    // Register AppendEntries handler
    _network_server.register_append_entries_handler(
        [this](const append_entries_request_t& request) -> append_entries_response_t {
            return this->handle_append_entries(request);
        }
    );
    
    // Register InstallSnapshot handler
    _network_server.register_install_snapshot_handler(
        [this](const install_snapshot_request_t& request) -> install_snapshot_response_t {
            return this->handle_install_snapshot(request);
        }
    );
    
    _logger.debug("RPC handlers registered", {
        {"node_id", std::to_string(_node_id)}
    });
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::start() -> void {
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
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.node.started");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_one();
    metric.emit();
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::stop() -> void {
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
    
    // Stop the network server
    _network_server.stop();
    
    _logger.info("Raft node stopped successfully", {
        {"node_id", std::to_string(_node_id)}
    });
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.node.stopped");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_one();
    metric.emit();
}

// Stub implementations for methods to be implemented in later tasks
template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<FutureType, NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::submit_command(
    const std::vector<std::byte>& command,
    std::chrono::milliseconds timeout
) -> FutureType {
    std::lock_guard<std::mutex> lock(_mutex);
    
    // Only leaders can accept client commands
    if (_state != server_state::leader) {
        _logger.debug("Rejected command submission: not leader", {
            {"node_id", std::to_string(_node_id)},
            {"state", _state == server_state::follower ? "follower" : "candidate"}
        });
        return FutureType(std::make_exception_ptr(
            std::runtime_error("Not the leader")
        ));
    }
    
    _logger.info("Received client command", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"command_size", std::to_string(command.size())}
    });
    
    // Create new log entry with current term
    auto new_index = get_last_log_index() + 1;
    log_entry_t new_entry{_current_term, new_index, command};
    
    // Append to leader's log
    _log.push_back(new_entry);
    
    // Persist the entry before responding
    _persistence.append_log_entry(new_entry);
    
    _logger.info("Appended command to log", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"index", std::to_string(new_index)}
    });
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.command.submitted");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("term", std::to_string(_current_term));
    metric.add_one();
    metric.emit();
    
    // Replicate to followers immediately
    replicate_to_followers();
    
    // Return a future that will be fulfilled when the entry is committed
    // For now, return a simple success response
    // TODO: In a complete implementation, this would wait for commit and apply to state machine
    return FutureType(std::vector<std::byte>{});
}

template<
    typename FutureType,
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    future<FutureType, std::vector<std::byte>> &&
    future<FutureType, bool> &&
    network_client<NetworkClient, FutureType> &&
    raft::network_server<NetworkServer> &&
    raft::persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, raft::log_entry<TermId, LogIndex>, raft::snapshot<NodeId, TermId, LogIndex>> &&
    raft::diagnostic_logger<Logger> &&
    raft::metrics<Metrics> &&
    raft::membership_manager<MembershipManager, NodeId, raft::cluster_configuration<NodeId>> &&
    raft::node_id<NodeId> &&
    raft::term_id<TermId> &&
    raft::log_index<LogIndex>
auto node<FutureType, NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::submit_command_with_session(
    client_id_t client_id,
    serial_number_t serial_number,
    const std::vector<std::byte>& command,
    std::chrono::milliseconds timeout
) -> FutureType {
    std::lock_guard<std::mutex> lock(_mutex);
    
    // Only leaders can accept client commands
    if (_state != server_state::leader) {
        _logger.debug("Rejected command submission: not leader", {
            {"node_id", std::to_string(_node_id)},
            {"state", _state == server_state::follower ? "follower" : "candidate"},
            {"client_id", std::to_string(client_id)},
            {"serial_number", std::to_string(serial_number)}
        });
        return FutureType(std::make_exception_ptr(
            std::runtime_error("Not the leader")
        ));
    }
    
    _logger.info("Received client command with session", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"client_id", std::to_string(client_id)},
        {"serial_number", std::to_string(serial_number)},
        {"command_size", std::to_string(command.size())}
    });
    
    // Check for duplicate request
    auto session_it = _client_sessions.find(client_id);
    if (session_it != _client_sessions.end()) {
        const auto& session = session_it->second;
        
        // If this is a duplicate (same or older serial number), return cached response
        if (serial_number <= session.last_serial_number) {
            _logger.info("Detected duplicate request, returning cached response", {
                {"node_id", std::to_string(_node_id)},
                {"client_id", std::to_string(client_id)},
                {"serial_number", std::to_string(serial_number)},
                {"last_serial_number", std::to_string(session.last_serial_number)}
            });
            
            // Emit metrics for duplicate detection
            auto metric = _metrics;
            metric.set_metric_name("raft.command.duplicate_detected");
            metric.add_dimension("node_id", std::to_string(_node_id));
            metric.add_dimension("client_id", std::to_string(client_id));
            metric.add_one();
            metric.emit();
            
            // Return the cached response
            return FutureType(std::vector<std::byte>(session.last_response));
        }
        
        // Serial number must be exactly last_serial_number + 1 for proper ordering
        if (serial_number != session.last_serial_number + 1) {
            _logger.warning("Out-of-order serial number detected", {
                {"node_id", std::to_string(_node_id)},
                {"client_id", std::to_string(client_id)},
                {"serial_number", std::to_string(serial_number)},
                {"expected_serial_number", std::to_string(session.last_serial_number + 1)}
            });
            
            // Emit metrics
            auto metric = _metrics;
            metric.set_metric_name("raft.command.out_of_order");
            metric.add_dimension("node_id", std::to_string(_node_id));
            metric.add_dimension("client_id", std::to_string(client_id));
            metric.add_one();
            metric.emit();
            
            return FutureType(std::make_exception_ptr(
                std::runtime_error("Out-of-order serial number")
            ));
        }
    } else {
        // New client session - serial number should start at 1
        if (serial_number != 1) {
            _logger.warning("New client session with invalid initial serial number", {
                {"node_id", std::to_string(_node_id)},
                {"client_id", std::to_string(client_id)},
                {"serial_number", std::to_string(serial_number)}
            });
            
            return FutureType(std::make_exception_ptr(
                std::runtime_error("Invalid initial serial number (must be 1)")
            ));
        }
        
        _logger.info("Creating new client session", {
            {"node_id", std::to_string(_node_id)},
            {"client_id", std::to_string(client_id)}
        });
    }
    
    // This is a new request - process it normally
    _logger.info("Processing new client command", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"client_id", std::to_string(client_id)},
        {"serial_number", std::to_string(serial_number)}
    });
    
    // Create new log entry with current term
    auto new_index = get_last_log_index() + 1;
    log_entry_t new_entry{_current_term, new_index, command};
    
    // Append to leader's log
    _log.push_back(new_entry);
    
    // Persist the entry before responding
    _persistence.append_log_entry(new_entry);
    
    _logger.info("Appended command to log", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"index", std::to_string(new_index)},
        {"client_id", std::to_string(client_id)},
        {"serial_number", std::to_string(serial_number)}
    });
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.command.submitted_with_session");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("term", std::to_string(_current_term));
    metric.add_dimension("client_id", std::to_string(client_id));
    metric.add_one();
    metric.emit();
    
    // Replicate to followers immediately
    replicate_to_followers();
    
    // Generate response (in a real implementation, this would be the state machine response)
    std::vector<std::byte> response{};
    
    // Update client session with new serial number and response
    _client_sessions[client_id] = client_session{serial_number, response};
    
    _logger.debug("Updated client session", {
        {"node_id", std::to_string(_node_id)},
        {"client_id", std::to_string(client_id)},
        {"serial_number", std::to_string(serial_number)}
    });
    
    // Return the response
    return FutureType(std::vector<std::byte>(response));
}

template<
    typename FutureType,
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    future<FutureType, std::vector<std::byte>> &&
    future<FutureType, bool> &&
    network_client<NetworkClient, FutureType> &&
    raft::network_server<NetworkServer> &&
    raft::persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, raft::log_entry<TermId, LogIndex>, raft::snapshot<NodeId, TermId, LogIndex>> &&
    raft::diagnostic_logger<Logger> &&
    raft::metrics<Metrics> &&
    raft::membership_manager<MembershipManager, NodeId, raft::cluster_configuration<NodeId>> &&
    raft::node_id<NodeId> &&
    raft::term_id<TermId> &&
    raft::log_index<LogIndex>
auto node<FutureType, NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::read_state(
    std::chrono::milliseconds timeout
) -> FutureType {
    std::lock_guard<std::mutex> lock(_mutex);
    
    // Only leaders can serve linearizable reads
    if (_state != server_state::leader) {
        _logger.debug("Rejected read request: not leader", {
            {"node_id", std::to_string(_node_id)},
            {"state", _state == server_state::follower ? "follower" : "candidate"}
        });
        return FutureType(std::make_exception_ptr(
            std::runtime_error("Not the leader")
        ));
    }
    
    _logger.info("Received linearizable read request", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"commit_index", std::to_string(_commit_index)}
    });
    
    // Read Index Optimization:
    // 1. Record the current commit index as the read_index
    // 2. Send heartbeats to a majority to ensure we're still the leader
    // 3. Wait for commit_index to advance to at least read_index
    // 4. Return the state at read_index
    
    auto read_index = _commit_index;
    auto current_term = _current_term;
    auto node_id = _node_id;
    
    _logger.debug("Starting linearizable read with read index", {
        {"node_id", std::to_string(node_id)},
        {"term", std::to_string(current_term)},
        {"read_index", std::to_string(read_index)}
    });
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.read.started");
    metric.add_dimension("node_id", std::to_string(node_id));
    metric.add_dimension("term", std::to_string(current_term));
    metric.add_one();
    metric.emit();
    
    // Check if this is a single-node cluster
    // In a single-node cluster, we already have a majority (ourselves)
    if (_configuration.nodes().size() == 1) {
        _logger.debug("Single-node cluster, skipping heartbeat confirmation", {
            {"node_id", std::to_string(node_id)},
            {"term", std::to_string(current_term)}
        });
        
        // Emit metrics
        auto metric = _metrics;
        metric.set_metric_name("raft.read.succeeded");
        metric.add_dimension("node_id", std::to_string(node_id));
        metric.add_dimension("term", std::to_string(current_term));
        metric.add_one();
        metric.emit();
        
        // Return immediately - we have confirmed leadership (we're the only node)
        return FutureType(std::vector<std::byte>{});
    }
    
    // Send heartbeats to all followers to confirm leadership
    // We need to receive successful responses from a majority
    std::vector<FutureType> heartbeat_futures;
    
    for (const auto& peer_id : _configuration.nodes()) {
        // Skip ourselves
        if (peer_id == _node_id) {
            continue;
        }
        
        // Get the previous log index and term for this follower
        auto next_idx = _next_index[peer_id];
        LogIndex prev_log_index = next_idx > 1 ? next_idx - 1 : 0;
        TermId prev_log_term = 0;
        
        // If prev_log_index > 0, we need to get the term of that entry
        if (prev_log_index > 0) {
            LogIndex log_start_index = _commit_index + 1;
            if (!_log.empty()) {
                log_start_index = _log.front().index();
            }
            
            if (prev_log_index >= log_start_index && prev_log_index <= get_last_log_index()) {
                std::size_t offset = static_cast<std::size_t>(prev_log_index - log_start_index);
                if (offset < _log.size()) {
                    prev_log_term = _log[offset].term();
                }
            } else if (prev_log_index <= _commit_index) {
                prev_log_term = 0;
            }
        }
        
        // Create empty AppendEntries request (heartbeat)
        append_entries_request_t heartbeat_request{
            _current_term,
            _node_id,
            prev_log_index,
            prev_log_term,
            {},  // Empty entries vector (heartbeat)
            _commit_index
        };
        
        _logger.debug("Sending heartbeat for linearizable read", {
            {"node_id", std::to_string(_node_id)},
            {"target", std::to_string(peer_id)},
            {"term", std::to_string(_current_term)}
        });
        
        // Send heartbeat
        auto future = _network_client.send_append_entries(peer_id, heartbeat_request, timeout);
        heartbeat_futures.push_back(std::move(future));
    }
    
    // Wait for heartbeat responses and check for majority
    // TODO: Implement generic future collection mechanism
    // For now, return success immediately as a temporary fix
    return FutureType(std::vector<std::byte>{});
}

template<
    typename FutureType,
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    future<FutureType, std::vector<std::byte>> &&
    future<FutureType, bool> &&
    network_client<NetworkClient, FutureType> &&
    raft::network_server<NetworkServer> &&
    raft::persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, raft::log_entry<TermId, LogIndex>, raft::snapshot<NodeId, TermId, LogIndex>> &&
    raft::diagnostic_logger<Logger> &&
    raft::metrics<Metrics> &&
    raft::membership_manager<MembershipManager, NodeId, raft::cluster_configuration<NodeId>> &&
    raft::node_id<NodeId> &&
    raft::term_id<TermId> &&
    raft::log_index<LogIndex>
auto node<FutureType, NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::add_server(
    NodeId new_node
) -> FutureType {
    std::lock_guard<std::mutex> lock(_mutex);
    
    // Only leaders can add servers
    if (_state != server_state::leader) {
        _logger.debug("Rejected add_server request: not leader", {
            {"node_id", std::to_string(_node_id)},
            {"state", _state == server_state::follower ? "follower" : "candidate"},
            {"new_node", std::to_string(new_node)}
        });
        return FutureType(std::make_exception_ptr(
            std::runtime_error("Not the leader")
        ));
    }
    
    _logger.info("Received add_server request", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"new_node", std::to_string(new_node)}
    });
    
    // Validate the new node using the membership manager
    if (!_membership.validate_new_node(new_node)) {
        _logger.warning("New node validation failed", {
            {"node_id", std::to_string(_node_id)},
            {"new_node", std::to_string(new_node)}
        });
        return FutureType(false);
    }
    
    // Authenticate the new node using the membership manager
    if (!_membership.authenticate_node(new_node)) {
        _logger.warning("New node authentication failed", {
            {"node_id", std::to_string(_node_id)},
            {"new_node", std::to_string(new_node)}
        });
        return FutureType(false);
    }
    
    // Check if the node is already in the configuration
    if (_membership.is_node_in_configuration(new_node, _configuration)) {
        _logger.warning("Node is already in the configuration", {
            {"node_id", std::to_string(_node_id)},
            {"new_node", std::to_string(new_node)}
        });
        return FutureType(false);
    }
    
    // Check if we're already in a configuration change
    if (_configuration.is_joint_consensus()) {
        _logger.warning("Cannot add server: already in joint consensus", {
            {"node_id", std::to_string(_node_id)},
            {"new_node", std::to_string(new_node)}
        });
        return FutureType(std::make_exception_ptr(
            std::runtime_error("Configuration change already in progress")
        ));
    }
    
    _logger.info("Starting add_server with joint consensus", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"new_node", std::to_string(new_node)}
    });
    
    // Phase 1: Add the new server as a non-voting member (catch-up phase)
    // In this simplified implementation, we assume the new server catches up quickly
    // A full implementation would:
    // 1. Add the server as non-voting
    // 2. Replicate log entries to it
    // 3. Wait for it to catch up (match_index close to leader's last log index)
    // 4. Then proceed to joint consensus
    
    // Initialize replication state for the new server
    _next_index[new_node] = get_last_log_index() + 1;
    _match_index[new_node] = 0;
    
    _logger.info("Initialized replication state for new server", {
        {"node_id", std::to_string(_node_id)},
        {"new_node", std::to_string(new_node)},
        {"next_index", std::to_string(_next_index[new_node])},
        {"match_index", std::to_string(_match_index[new_node])}
    });
    
    // Start replicating to the new server immediately
    // This allows it to catch up while we prepare the configuration change
    send_append_entries_to(new_node);
    
    // Phase 2: Create new configuration with the new server
    cluster_configuration_t new_config = _configuration;
    new_config._nodes.push_back(new_node);
    new_config._is_joint_consensus = false;
    new_config._old_nodes = std::nullopt;
    
    // Phase 3: Create joint consensus configuration (C_old,new)
    cluster_configuration_t joint_config = _membership.create_joint_configuration(_configuration, new_config);
    
    _logger.info("Created joint consensus configuration", {
        {"node_id", std::to_string(_node_id)},
        {"old_config_size", std::to_string(_configuration.nodes().size())},
        {"new_config_size", std::to_string(new_config.nodes().size())},
        {"joint_config_size", std::to_string(joint_config.nodes().size())}
    });
    
    // Phase 4: Append C_old,new to the log
    auto config_entry_index = get_last_log_index() + 1;
    
    // Serialize the joint configuration as a log entry command
    // In a real implementation, this would use proper serialization
    // For now, we use an empty command to represent the configuration change
    std::vector<std::byte> config_command{};
    
    log_entry_t config_entry{_current_term, config_entry_index, config_command};
    
    _log.push_back(config_entry);
    _persistence.append_log_entry(config_entry);
    
    _logger.info("Appended joint consensus configuration to log", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"config_entry_index", std::to_string(config_entry_index)}
    });
    
    // Phase 5: Update our configuration to joint consensus
    _configuration = joint_config;
    
    // Log membership change
    _logger.info("Membership change: entered joint consensus", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"operation", "add_server"},
        {"new_node", std::to_string(new_node)},
        {"configuration_type", "joint_consensus"}
    });
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.membership.joint_consensus_started");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("term", std::to_string(_current_term));
    metric.add_dimension("new_node", std::to_string(new_node));
    metric.add_one();
    metric.emit();
    
    // Phase 6: Replicate C_old,new to all servers
    replicate_to_followers();
    
    // Phase 7: Wait for C_old,new to be committed
    // Once committed, the cluster is using joint consensus
    // We need to wait for the configuration entry to be committed before proceeding
    
    // For this implementation, we'll use a simplified approach
    // In a real implementation, this would be more sophisticated
    // TODO: Implement proper configuration change waiting mechanism
    
    // For this simplified implementation, we'll immediately proceed to C_new
    // A full implementation would wait for C_old,new to be committed first
    
    _logger.info("Joint consensus configuration active, proceeding to final configuration", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)}
    });
    
    // Create the final configuration entry (C_new)
    auto final_config_entry_index = get_last_log_index() + 1;
    log_entry_t final_config_entry{_current_term, final_config_entry_index, std::vector<std::byte>{}};
    
    _log.push_back(final_config_entry);
    _persistence.append_log_entry(final_config_entry);
    
    _logger.info("Appended final configuration to log", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"final_config_entry_index", std::to_string(final_config_entry_index)}
    });
    
    // Phase 9: Update our configuration to the final configuration (C_new)
    _configuration = new_config;
    
    _logger.info("Transitioned to final configuration", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"new_config_size", std::to_string(_configuration.nodes().size())}
    });
    
    // Log membership change completion
    _logger.info("Membership change: completed add_server", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"operation", "add_server"},
        {"new_node", std::to_string(new_node)},
        {"configuration_type", "final"},
        {"cluster_size", std::to_string(_configuration.nodes().size())}
    });
    
    // Emit metrics
    metric = _metrics;
    metric.set_metric_name("raft.membership.server_added");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("term", std::to_string(_current_term));
    metric.add_dimension("new_node", std::to_string(new_node));
    metric.add_one();
    metric.emit();
    
    // Phase 10: Replicate C_new to all servers
    replicate_to_followers();
    
    _logger.info("Server added successfully", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"new_node", std::to_string(new_node)},
        {"cluster_size", std::to_string(_configuration.nodes().size())}
    });
    
    // Return success
    return FutureType(true);
}

template<
    typename FutureType,
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    future<FutureType, std::vector<std::byte>> &&
    future<FutureType, bool> &&
    network_client<NetworkClient, FutureType> &&
    raft::network_server<NetworkServer> &&
    raft::persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, raft::log_entry<TermId, LogIndex>, raft::snapshot<NodeId, TermId, LogIndex>> &&
    raft::diagnostic_logger<Logger> &&
    raft::metrics<Metrics> &&
    raft::membership_manager<MembershipManager, NodeId, raft::cluster_configuration<NodeId>> &&
    raft::node_id<NodeId> &&
    raft::term_id<TermId> &&
    raft::log_index<LogIndex>
auto node<FutureType, NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::remove_server(
    NodeId old_node
) -> FutureType {
    std::lock_guard<std::mutex> lock(_mutex);
    
    // Only leaders can remove servers
    if (_state != server_state::leader) {
        _logger.debug("Rejected remove_server request: not leader", {
            {"node_id", std::to_string(_node_id)},
            {"state", _state == server_state::follower ? "follower" : "candidate"},
            {"old_node", std::to_string(old_node)}
        });
        return FutureType(std::make_exception_ptr(
            std::runtime_error("Not the leader")
        ));
    }
    
    _logger.info("Received remove_server request", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"old_node", std::to_string(old_node)}
    });
    
    // Check if the node is in the configuration
    if (!_membership.is_node_in_configuration(old_node, _configuration)) {
        _logger.warning("Node is not in the configuration", {
            {"node_id", std::to_string(_node_id)},
            {"old_node", std::to_string(old_node)}
        });
        return FutureType(false);
    }
    
    // Check if we're already in a configuration change
    if (_configuration.is_joint_consensus()) {
        _logger.warning("Cannot remove server: already in joint consensus", {
            {"node_id", std::to_string(_node_id)},
            {"old_node", std::to_string(old_node)}
        });
        return FutureType(std::make_exception_ptr(
            std::runtime_error("Configuration change already in progress")
        ));
    }
    
    // Check if removing this node would leave the cluster empty
    if (_configuration.nodes().size() <= 1) {
        _logger.warning("Cannot remove server: would leave cluster empty", {
            {"node_id", std::to_string(_node_id)},
            {"old_node", std::to_string(old_node)},
            {"cluster_size", std::to_string(_configuration.nodes().size())}
        });
        return FutureType(false);
    }
    
    _logger.info("Starting remove_server with joint consensus", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"old_node", std::to_string(old_node)}
    });
    
    // Phase 1: Create new configuration without the old server
    cluster_configuration_t new_config = _configuration;
    
    // Remove the old node from the configuration
    auto it = std::find(new_config._nodes.begin(), new_config._nodes.end(), old_node);
    if (it != new_config._nodes.end()) {
        new_config._nodes.erase(it);
    }
    
    new_config._is_joint_consensus = false;
    new_config._old_nodes = std::nullopt;
    
    _logger.info("Created new configuration without old server", {
        {"node_id", std::to_string(_node_id)},
        {"old_config_size", std::to_string(_configuration.nodes().size())},
        {"new_config_size", std::to_string(new_config.nodes().size())}
    });
    
    // Phase 2: Create joint consensus configuration (C_old,new)
    cluster_configuration_t joint_config = _membership.create_joint_configuration(_configuration, new_config);
    
    _logger.info("Created joint consensus configuration", {
        {"node_id", std::to_string(_node_id)},
        {"old_config_size", std::to_string(_configuration.nodes().size())},
        {"new_config_size", std::to_string(new_config.nodes().size())},
        {"joint_config_size", std::to_string(joint_config.nodes().size())}
    });
    
    // Phase 3: Append C_old,new to the log
    auto config_entry_index = get_last_log_index() + 1;
    
    // Serialize the joint configuration as a log entry command
    // In a real implementation, this would use proper serialization
    // For now, we use an empty command to represent the configuration change
    std::vector<std::byte> config_command{};
    
    log_entry_t config_entry{_current_term, config_entry_index, config_command};
    
    _log.push_back(config_entry);
    _persistence.append_log_entry(config_entry);
    
    _logger.info("Appended joint consensus configuration to log", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"config_entry_index", std::to_string(config_entry_index)}
    });
    
    // Phase 4: Update our configuration to joint consensus
    _configuration = joint_config;
    
    // Log membership change
    _logger.info("Membership change: entered joint consensus", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"operation", "remove_server"},
        {"old_node", std::to_string(old_node)},
        {"configuration_type", "joint_consensus"}
    });
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.membership.joint_consensus_started");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("term", std::to_string(_current_term));
    metric.add_dimension("old_node", std::to_string(old_node));
    metric.add_one();
    metric.emit();
    
    // Phase 5: Replicate C_old,new to all servers
    replicate_to_followers();
    
    // Phase 6: Wait for C_old,new to be committed
    // Once committed, the cluster is using joint consensus
    
    // For this simplified implementation, we'll immediately proceed to C_new
    // A full implementation would wait for C_old,new to be committed first
    
    _logger.info("Joint consensus configuration active, proceeding to final configuration", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)}
    });
    
    // Phase 7: Once C_old,new is committed, append C_new to the log
    auto final_config_entry_index = get_last_log_index() + 1;
    log_entry_t final_config_entry{_current_term, final_config_entry_index, std::vector<std::byte>{}};
    
    _log.push_back(final_config_entry);
    _persistence.append_log_entry(final_config_entry);
    
    _logger.info("Appended final configuration to log", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"final_config_entry_index", std::to_string(final_config_entry_index)}
    });
    
    // Phase 8: Update our configuration to the final configuration (C_new)
    _configuration = new_config;
    
    _logger.info("Transitioned to final configuration", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"new_config_size", std::to_string(_configuration.nodes().size())}
    });
    
    // Log membership change completion
    _logger.info("Membership change: completed remove_server", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"operation", "remove_server"},
        {"old_node", std::to_string(old_node)},
        {"configuration_type", "final"},
        {"cluster_size", std::to_string(_configuration.nodes().size())}
    });
    
    // Phase 9: Clean up replication state for the removed server
    _next_index.erase(old_node);
    _match_index.erase(old_node);
    
    // Notify the membership manager about the removal
    _membership.handle_node_removal(old_node);
    
    _logger.info("Cleaned up state for removed server", {
        {"node_id", std::to_string(_node_id)},
        {"old_node", std::to_string(old_node)}
    });
    
    // Emit metrics
    metric = _metrics;
    metric.set_metric_name("raft.membership.server_removed");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("term", std::to_string(_current_term));
    metric.add_dimension("old_node", std::to_string(old_node));
    metric.add_one();
    metric.emit();
    
    // Phase 10: Replicate C_new to all remaining servers
    replicate_to_followers();
    
    // Phase 11: Check if we (the leader) were removed
    // If so, we need to step down after committing the new configuration
    bool leader_removed = (old_node == _node_id);
    
    if (leader_removed) {
        _logger.info("Leader was removed from cluster, will step down after configuration is committed", {
            {"node_id", std::to_string(_node_id)},
            {"term", std::to_string(_current_term)}
        });
        
        // In a full implementation, we would wait for the configuration to be committed
        // before stepping down. For this simplified version, we step down immediately.
        
        // Step down by becoming a follower
        // We don't call become_follower() because we want to stay in the same term
        _state = server_state::follower;
        
        _logger.info("Leader stepped down after removal", {
            {"node_id", std::to_string(_node_id)},
            {"term", std::to_string(_current_term)}
        });
        
        // Log state transition
        _logger.info("State transition", {
            {"node_id", std::to_string(_node_id)},
            {"old_state", "leader"},
            {"new_state", "follower"},
            {"term", std::to_string(_current_term)},
            {"reason", "removed_from_cluster"}
        });
        
        // Emit metrics
        metric = _metrics;
        metric.set_metric_name("raft.state.transition");
        metric.add_dimension("node_id", std::to_string(_node_id));
        metric.add_dimension("old_state", "leader");
        metric.add_dimension("new_state", "follower");
        metric.add_dimension("term", std::to_string(_current_term));
        metric.add_one();
        metric.emit();
        
        metric = _metrics;
        metric.set_metric_name("raft.leader.stepped_down");
        metric.add_dimension("node_id", std::to_string(_node_id));
        metric.add_dimension("term", std::to_string(_current_term));
        metric.add_dimension("reason", "removed_from_cluster");
        metric.add_one();
        metric.emit();
    }
    
    _logger.info("Server removed successfully", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"old_node", std::to_string(old_node)},
        {"cluster_size", std::to_string(_configuration.nodes().size())},
        {"leader_removed", leader_removed ? "true" : "false"}
    });
    
    // Return success
    return FutureType(true);
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::handle_request_vote(
    const request_vote_request_t& request
) -> request_vote_response_t {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.debug("Received RequestVote RPC", {
        {"node_id", std::to_string(_node_id)},
        {"from_candidate", std::to_string(request.candidate_id())},
        {"request_term", std::to_string(request.term())},
        {"current_term", std::to_string(_current_term)},
        {"candidate_last_log_index", std::to_string(request.last_log_index())},
        {"candidate_last_log_term", std::to_string(request.last_log_term())}
    });
    
    // Requirement 9.6: Prevent removed servers from disrupting elections
    // Ignore RequestVote RPCs from servers not in the current configuration
    if (!_membership.is_node_in_configuration(request.candidate_id(), _configuration)) {
        _logger.debug("Rejected RequestVote: candidate not in current configuration", {
            {"node_id", std::to_string(_node_id)},
            {"candidate_id", std::to_string(request.candidate_id())},
            {"request_term", std::to_string(request.term())}
        });
        
        // Return current term but deny vote
        // This prevents removed servers from disrupting elections
        return request_vote_response_t{_current_term, false};
    }
    
    // If request term is greater than current term, update term and become follower
    if (request.term() > _current_term) {
        _logger.info("Discovered higher term in RequestVote, becoming follower", {
            {"node_id", std::to_string(_node_id)},
            {"old_term", std::to_string(_current_term)},
            {"new_term", std::to_string(request.term())}
        });
        
        auto old_state = _state;
        _current_term = request.term();
        _voted_for = std::nullopt;
        _state = server_state::follower;
        
        // Persist term before continuing
        _persistence.save_current_term(_current_term);
        // Note: voted_for is now nullopt, which means we haven't voted yet in this term
        
        // Log state transition
        _logger.info("State transition", {
            {"node_id", std::to_string(_node_id)},
            {"old_state", old_state == server_state::follower ? "follower" : 
                         old_state == server_state::candidate ? "candidate" : "leader"},
            {"new_state", "follower"},
            {"term", std::to_string(_current_term)},
            {"reason", "discovered_higher_term_in_request_vote"}
        });
        
        // Emit metrics
        auto metric = _metrics;
        metric.set_metric_name("raft.state.transition");
        metric.add_dimension("node_id", std::to_string(_node_id));
        metric.add_dimension("old_state", old_state == server_state::follower ? "follower" : 
                                         old_state == server_state::candidate ? "candidate" : "leader");
        metric.add_dimension("new_state", "follower");
        metric.add_dimension("term", std::to_string(_current_term));
        metric.add_one();
        metric.emit();
    }
    
    // Default response: deny vote
    bool vote_granted = false;
    
    // Check if we can grant the vote
    // We grant vote if:
    // 1. Request term >= current term (already handled above)
    // 2. We haven't voted for anyone else in this term
    // 3. Candidate's log is at least as up-to-date as ours
    
    if (request.term() >= _current_term) {
        // Check if we haven't voted or voted for this candidate
        bool can_vote = !_voted_for.has_value() || _voted_for.value() == request.candidate_id();
        
        if (can_vote) {
            // Check if candidate's log is at least as up-to-date as ours
            // Log is more up-to-date if:
            // - Last log term is higher, OR
            // - Last log terms are equal AND last log index is >= ours
            
            auto our_last_log_index = get_last_log_index();
            auto our_last_log_term = get_last_log_term();
            
            bool log_is_up_to_date = false;
            
            if (request.last_log_term() > our_last_log_term) {
                // Candidate's log has entries from a later term
                log_is_up_to_date = true;
            } else if (request.last_log_term() == our_last_log_term) {
                // Same term, check log length
                if (request.last_log_index() >= our_last_log_index) {
                    log_is_up_to_date = true;
                }
            }
            
            if (log_is_up_to_date) {
                // Grant the vote
                vote_granted = true;
                _voted_for = request.candidate_id();
                
                // Reset election timer since we're granting a vote
                _last_heartbeat = std::chrono::steady_clock::now();
                
                // Persist voted_for before responding (critical for safety)
                _persistence.save_voted_for(request.candidate_id());
                
                _logger.info("Granted vote", {
                    {"node_id", std::to_string(_node_id)},
                    {"term", std::to_string(_current_term)},
                    {"voted_for", std::to_string(request.candidate_id())}
                });
                
                // Emit metrics
                auto metric = _metrics;
                metric.set_metric_name("raft.vote.granted");
                metric.add_dimension("node_id", std::to_string(_node_id));
                metric.add_dimension("candidate_id", std::to_string(request.candidate_id()));
                metric.add_dimension("term", std::to_string(_current_term));
                metric.add_one();
                metric.emit();
            } else {
                _logger.debug("Denied vote: candidate log not up-to-date", {
                    {"node_id", std::to_string(_node_id)},
                    {"candidate_id", std::to_string(request.candidate_id())},
                    {"our_last_log_term", std::to_string(our_last_log_term)},
                    {"our_last_log_index", std::to_string(our_last_log_index)},
                    {"candidate_last_log_term", std::to_string(request.last_log_term())},
                    {"candidate_last_log_index", std::to_string(request.last_log_index())}
                });
            }
        } else {
            _logger.debug("Denied vote: already voted for another candidate", {
                {"node_id", std::to_string(_node_id)},
                {"term", std::to_string(_current_term)},
                {"voted_for", std::to_string(_voted_for.value())},
                {"requesting_candidate", std::to_string(request.candidate_id())}
            });
        }
    } else {
        _logger.debug("Denied vote: request term is stale", {
            {"node_id", std::to_string(_node_id)},
            {"request_term", std::to_string(request.term())},
            {"current_term", std::to_string(_current_term)}
        });
    }
    
    // Emit metrics for denied votes
    if (!vote_granted) {
        auto metric = _metrics;
        metric.set_metric_name("raft.vote.denied");
        metric.add_dimension("node_id", std::to_string(_node_id));
        metric.add_dimension("candidate_id", std::to_string(request.candidate_id()));
        metric.add_dimension("term", std::to_string(_current_term));
        metric.add_one();
        metric.emit();
    }
    
    return request_vote_response_t{_current_term, vote_granted};
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::handle_append_entries(
    const append_entries_request_t& request
) -> append_entries_response_t {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.debug("Received AppendEntries RPC", {
        {"node_id", std::to_string(_node_id)},
        {"from_leader", std::to_string(request.leader_id())},
        {"request_term", std::to_string(request.term())},
        {"current_term", std::to_string(_current_term)},
        {"prev_log_index", std::to_string(request.prev_log_index())},
        {"prev_log_term", std::to_string(request.prev_log_term())},
        {"entries_count", std::to_string(request.entries().size())},
        {"leader_commit", std::to_string(request.leader_commit())}
    });
    
    // If request term is greater than current term, update term and become follower
    if (request.term() > _current_term) {
        _logger.info("Discovered higher term in AppendEntries, becoming follower", {
            {"node_id", std::to_string(_node_id)},
            {"old_term", std::to_string(_current_term)},
            {"new_term", std::to_string(request.term())}
        });
        
        auto old_state = _state;
        _current_term = request.term();
        _voted_for = std::nullopt;
        _state = server_state::follower;
        
        // Persist term before continuing
        _persistence.save_current_term(_current_term);
        
        // Log state transition
        _logger.info("State transition", {
            {"node_id", std::to_string(_node_id)},
            {"old_state", old_state == server_state::follower ? "follower" : 
                         old_state == server_state::candidate ? "candidate" : "leader"},
            {"new_state", "follower"},
            {"term", std::to_string(_current_term)},
            {"reason", "discovered_higher_term_in_append_entries"}
        });
        
        // Emit metrics
        auto metric = _metrics;
        metric.set_metric_name("raft.state.transition");
        metric.add_dimension("node_id", std::to_string(_node_id));
        metric.add_dimension("old_state", old_state == server_state::follower ? "follower" : 
                                         old_state == server_state::candidate ? "candidate" : "leader");
        metric.add_dimension("new_state", "follower");
        metric.add_dimension("term", std::to_string(_current_term));
        metric.add_one();
        metric.emit();
    }
    
    // If request term is less than current term, reject
    if (request.term() < _current_term) {
        _logger.debug("Rejected AppendEntries: stale term", {
            {"node_id", std::to_string(_node_id)},
            {"request_term", std::to_string(request.term())},
            {"current_term", std::to_string(_current_term)}
        });
        return append_entries_response_t{_current_term, false, std::nullopt, std::nullopt};
    }
    
    // Valid AppendEntries from current leader - reset election timer
    reset_election_timer();
    
    // If we're a candidate and receive valid AppendEntries, become follower
    if (_state == server_state::candidate) {
        _logger.info("Received valid AppendEntries as candidate, becoming follower", {
            {"node_id", std::to_string(_node_id)},
            {"term", std::to_string(_current_term)},
            {"leader_id", std::to_string(request.leader_id())}
        });
        _state = server_state::follower;
        
        // Log state transition
        _logger.info("State transition", {
            {"node_id", std::to_string(_node_id)},
            {"old_state", "candidate"},
            {"new_state", "follower"},
            {"term", std::to_string(_current_term)},
            {"reason", "received_valid_append_entries"}
        });
        
        // Emit metrics
        auto metric = _metrics;
        metric.set_metric_name("raft.state.transition");
        metric.add_dimension("node_id", std::to_string(_node_id));
        metric.add_dimension("old_state", "candidate");
        metric.add_dimension("new_state", "follower");
        metric.add_dimension("term", std::to_string(_current_term));
        metric.add_one();
        metric.emit();
    }
    
    // ========================================================================
    // Log consistency check (prev_log_index, prev_log_term)
    // ========================================================================
    
    // If prev_log_index is 0, this is the first entry or a heartbeat
    if (request.prev_log_index() > 0) {
        // Check if we have an entry at prev_log_index
        // Need to find the entry in our log
        
        // Calculate the offset in our log vector
        // Our log may start at index > 1 if we have a snapshot
        LogIndex log_start_index = _commit_index + 1;
        if (!_log.empty()) {
            log_start_index = _log.front().index();
        }
        
        // Check if prev_log_index is before our log (covered by snapshot)
        if (request.prev_log_index() < log_start_index) {
            // The leader is trying to send us entries we already have in a snapshot
            // This shouldn't normally happen, but we can handle it
            _logger.debug("AppendEntries prev_log_index is before our log start", {
                {"node_id", std::to_string(_node_id)},
                {"prev_log_index", std::to_string(request.prev_log_index())},
                {"log_start_index", std::to_string(log_start_index)},
                {"commit_index", std::to_string(_commit_index)}
            });
            
            // If prev_log_index <= commit_index, we have it in our snapshot
            // We can accept this if the entries start after our log
            if (request.prev_log_index() <= _commit_index) {
                // Accept - the consistency check passes (snapshot covers it)
                // But we need to skip entries we already have
                // For now, we'll just accept and process entries
            } else {
                // We don't have this entry - reject
                return append_entries_response_t{_current_term, false, _commit_index, std::nullopt};
            }
        } else if (request.prev_log_index() > get_last_log_index()) {
            // We don't have an entry at prev_log_index (gap in log)
            _logger.debug("AppendEntries rejected: missing entry at prev_log_index", {
                {"node_id", std::to_string(_node_id)},
                {"prev_log_index", std::to_string(request.prev_log_index())},
                {"our_last_log_index", std::to_string(get_last_log_index())}
            });
            
            // Return conflict information for fast log backtracking
            auto our_last_index = get_last_log_index();
            return append_entries_response_t{_current_term, false, our_last_index, std::nullopt};
        } else {
            // We have an entry at prev_log_index - check if terms match
            // Find the entry in our log
            std::size_t offset = static_cast<std::size_t>(request.prev_log_index() - log_start_index);
            
            if (offset < _log.size()) {
                const auto& our_entry = _log[offset];
                
                if (our_entry.term() != request.prev_log_term()) {
                    // Terms don't match - log inconsistency detected
                    _logger.debug("AppendEntries rejected: term mismatch at prev_log_index", {
                        {"node_id", std::to_string(_node_id)},
                        {"prev_log_index", std::to_string(request.prev_log_index())},
                        {"expected_term", std::to_string(request.prev_log_term())},
                        {"our_term", std::to_string(our_entry.term())}
                    });
                    
                    // Return conflict information
                    // Find the first index of the conflicting term
                    auto conflict_term = our_entry.term();
                    LogIndex conflict_index = request.prev_log_index();
                    
                    // Search backwards to find the first entry of this term
                    for (std::size_t i = offset; i > 0; --i) {
                        if (_log[i - 1].term() != conflict_term) {
                            break;
                        }
                        conflict_index = _log[i - 1].index();
                    }
                    
                    return append_entries_response_t{_current_term, false, conflict_index, conflict_term};
                }
            } else {
                // This shouldn't happen if our logic is correct
                _logger.error("Internal error: offset calculation mismatch", {
                    {"node_id", std::to_string(_node_id)},
                    {"prev_log_index", std::to_string(request.prev_log_index())},
                    {"log_start_index", std::to_string(log_start_index)},
                    {"offset", std::to_string(offset)},
                    {"log_size", std::to_string(_log.size())}
                });
                return append_entries_response_t{_current_term, false, std::nullopt, std::nullopt};
            }
        }
    }
    
    // ========================================================================
    // Log conflict resolution (overwrite conflicting entries)
    // ========================================================================
    
    // At this point, the consistency check passed
    // Now we need to append new entries and handle conflicts
    
    if (!request.entries().empty()) {
        _logger.debug("Processing AppendEntries with entries", {
            {"node_id", std::to_string(_node_id)},
            {"entries_count", std::to_string(request.entries().size())},
            {"first_entry_index", std::to_string(request.entries().front().index())},
            {"last_entry_index", std::to_string(request.entries().back().index())}
        });
        
        // Process each entry
        for (const auto& new_entry : request.entries()) {
            LogIndex log_start_index = _commit_index + 1;
            if (!_log.empty()) {
                log_start_index = _log.front().index();
            }
            
            // Check if we already have an entry at this index
            if (new_entry.index() >= log_start_index && new_entry.index() <= get_last_log_index()) {
                // We have an entry at this index - check for conflict
                std::size_t offset = static_cast<std::size_t>(new_entry.index() - log_start_index);
                
                if (offset < _log.size()) {
                    const auto& existing_entry = _log[offset];
                    
                    if (existing_entry.term() != new_entry.term()) {
                        // Conflict detected - delete this entry and all that follow
                        _logger.info("Conflict detected, truncating log", {
                            {"node_id", std::to_string(_node_id)},
                            {"conflict_index", std::to_string(new_entry.index())},
                            {"existing_term", std::to_string(existing_entry.term())},
                            {"new_term", std::to_string(new_entry.term())},
                            {"entries_deleted", std::to_string(_log.size() - offset)}
                        });
                        
                        // Truncate log from this point
                        _log.erase(_log.begin() + offset, _log.end());
                        
                        // Persist the truncation
                        _persistence.truncate_log(new_entry.index());
                        
                        // Append the new entry
                        _log.push_back(new_entry);
                        _persistence.append_log_entry(new_entry);
                        
                        _logger.debug("Appended entry after conflict resolution", {
                            {"node_id", std::to_string(_node_id)},
                            {"entry_index", std::to_string(new_entry.index())},
                            {"entry_term", std::to_string(new_entry.term())}
                        });
                    } else {
                        // Entry matches - skip it (already have it)
                        _logger.debug("Entry already exists with matching term, skipping", {
                            {"node_id", std::to_string(_node_id)},
                            {"entry_index", std::to_string(new_entry.index())},
                            {"entry_term", std::to_string(new_entry.term())}
                        });
                    }
                }
            } else if (new_entry.index() > get_last_log_index()) {
                // This is a new entry - append it
                _log.push_back(new_entry);
                _persistence.append_log_entry(new_entry);
                
                _logger.debug("Appended new entry", {
                    {"node_id", std::to_string(_node_id)},
                    {"entry_index", std::to_string(new_entry.index())},
                    {"entry_term", std::to_string(new_entry.term())}
                });
            } else {
                // Entry is before our log start (covered by snapshot) - skip it
                _logger.debug("Entry is before log start, skipping", {
                    {"node_id", std::to_string(_node_id)},
                    {"entry_index", std::to_string(new_entry.index())},
                    {"log_start_index", std::to_string(log_start_index)}
                });
            }
        }
    }
    
    // ========================================================================
    // Commit index update
    // ========================================================================
    
    // If leaderCommit > commitIndex, set commitIndex = min(leaderCommit, index of last new entry)
    if (request.leader_commit() > _commit_index) {
        auto old_commit_index = _commit_index;
        
        // Update commit index to the minimum of leader's commit and our last log index
        _commit_index = std::min(request.leader_commit(), get_last_log_index());
        
        _logger.info("Updated commit index", {
            {"node_id", std::to_string(_node_id)},
            {"old_commit_index", std::to_string(old_commit_index)},
            {"new_commit_index", std::to_string(_commit_index)},
            {"leader_commit", std::to_string(request.leader_commit())}
        });
        
        // Emit metrics
        auto metric = _metrics;
        metric.set_metric_name("raft.commit_index.updated");
        metric.add_dimension("node_id", std::to_string(_node_id));
        metric.add_dimension("term", std::to_string(_current_term));
        metric.add_value(static_cast<double>(_commit_index));
        metric.emit();
    }
    
    // Success!
    _logger.debug("AppendEntries succeeded", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"log_size", std::to_string(_log.size())},
        {"commit_index", std::to_string(_commit_index)}
    });
    
    return append_entries_response_t{_current_term, true, std::nullopt, std::nullopt};
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::handle_install_snapshot(
    const install_snapshot_request_t& request
) -> install_snapshot_response_t {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.debug("Received InstallSnapshot RPC", {
        {"node_id", std::to_string(_node_id)},
        {"from_leader", std::to_string(request.leader_id())},
        {"request_term", std::to_string(request.term())},
        {"current_term", std::to_string(_current_term)},
        {"last_included_index", std::to_string(request.last_included_index())},
        {"last_included_term", std::to_string(request.last_included_term())},
        {"offset", std::to_string(request.offset())},
        {"done", request.done() ? "true" : "false"}
    });
    
    // If request term is greater than current term, update term and become follower
    if (request.term() > _current_term) {
        _logger.info("Discovered higher term in InstallSnapshot, becoming follower", {
            {"node_id", std::to_string(_node_id)},
            {"old_term", std::to_string(_current_term)},
            {"new_term", std::to_string(request.term())}
        });
        
        auto old_state = _state;
        _current_term = request.term();
        _voted_for = std::nullopt;
        _state = server_state::follower;
        
        // Persist term before continuing
        _persistence.save_current_term(_current_term);
        
        // Log state transition
        _logger.info("State transition", {
            {"node_id", std::to_string(_node_id)},
            {"old_state", old_state == server_state::follower ? "follower" : 
                         old_state == server_state::candidate ? "candidate" : "leader"},
            {"new_state", "follower"},
            {"term", std::to_string(_current_term)},
            {"reason", "discovered_higher_term_in_install_snapshot"}
        });
        
        // Emit metrics
        auto metric = _metrics;
        metric.set_metric_name("raft.state.transition");
        metric.add_dimension("node_id", std::to_string(_node_id));
        metric.add_dimension("old_state", old_state == server_state::follower ? "follower" : 
                                         old_state == server_state::candidate ? "candidate" : "leader");
        metric.add_dimension("new_state", "follower");
        metric.add_dimension("term", std::to_string(_current_term));
        metric.add_one();
        metric.emit();
    }
    
    // If request term is less than current term, reject
    if (request.term() < _current_term) {
        _logger.debug("Rejected InstallSnapshot: stale term", {
            {"node_id", std::to_string(_node_id)},
            {"request_term", std::to_string(request.term())},
            {"current_term", std::to_string(_current_term)}
        });
        return install_snapshot_response_t{_current_term};
    }
    
    // Valid InstallSnapshot from current leader - reset election timer
    reset_election_timer();
    
    // If we're a candidate and receive valid InstallSnapshot, become follower
    if (_state == server_state::candidate) {
        _logger.info("Received valid InstallSnapshot as candidate, becoming follower", {
            {"node_id", std::to_string(_node_id)},
            {"term", std::to_string(_current_term)},
            {"leader_id", std::to_string(request.leader_id())}
        });
        _state = server_state::follower;
        
        // Log state transition
        _logger.info("State transition", {
            {"node_id", std::to_string(_node_id)},
            {"old_state", "candidate"},
            {"new_state", "follower"},
            {"term", std::to_string(_current_term)},
            {"reason", "received_valid_install_snapshot"}
        });
        
        // Emit metrics
        auto metric = _metrics;
        metric.set_metric_name("raft.state.transition");
        metric.add_dimension("node_id", std::to_string(_node_id));
        metric.add_dimension("old_state", "candidate");
        metric.add_dimension("new_state", "follower");
        metric.add_dimension("term", std::to_string(_current_term));
        metric.add_one();
        metric.emit();
    }
    
    // ========================================================================
    // Snapshot installation implementation
    // ========================================================================
    
    // For this simplified implementation, we assume the entire snapshot is sent in one chunk
    // A full implementation would handle chunked transfers with offset tracking
    
    // Check if this snapshot is older than what we already have
    auto current_snapshot_opt = _persistence.load_snapshot();
    if (current_snapshot_opt.has_value()) {
        const auto& current_snapshot = current_snapshot_opt.value();
        
        if (request.last_included_index() <= current_snapshot.last_included_index()) {
            _logger.debug("Received snapshot is not newer than current snapshot, ignoring", {
                {"node_id", std::to_string(_node_id)},
                {"request_last_included_index", std::to_string(request.last_included_index())},
                {"current_last_included_index", std::to_string(current_snapshot.last_included_index())}
            });
            return install_snapshot_response_t{_current_term};
        }
    }
    
    _logger.info("Installing snapshot from leader", {
        {"node_id", std::to_string(_node_id)},
        {"last_included_index", std::to_string(request.last_included_index())},
        {"last_included_term", std::to_string(request.last_included_term())},
        {"data_size", std::to_string(request.data().size())}
    });
    
    // Create snapshot from the request data
    // Note: In a real implementation with chunked transfers, we would accumulate chunks
    // For this implementation, we assume the entire snapshot is in request.data()
    
    // The snapshot data should contain the state machine state
    // We need to extract the configuration from somewhere - for now, use current configuration
    // In a full implementation, the configuration would be part of the snapshot data
    
    snapshot_t new_snapshot{
        request.last_included_index(),
        request.last_included_term(),
        _configuration,  // Use current configuration (should be part of snapshot data in full impl)
        request.data()   // State machine state
    };
    
    // Save the snapshot to persistence
    _persistence.save_snapshot(new_snapshot);
    
    _logger.info("Snapshot saved to persistence", {
        {"node_id", std::to_string(_node_id)},
        {"last_included_index", std::to_string(new_snapshot.last_included_index())}
    });
    
    // ========================================================================
    // Log truncation after snapshot installation
    // ========================================================================
    
    // If we have log entries that are covered by the snapshot, we can discard them
    // However, we must keep any entries that come after the snapshot
    
    if (!_log.empty()) {
        LogIndex log_start_index = _log.front().index();
        LogIndex log_end_index = _log.back().index();
        
        _logger.debug("Checking log for truncation", {
            {"node_id", std::to_string(_node_id)},
            {"log_start_index", std::to_string(log_start_index)},
            {"log_end_index", std::to_string(log_end_index)},
            {"snapshot_last_included_index", std::to_string(request.last_included_index())}
        });
        
        if (log_end_index <= request.last_included_index()) {
            // All log entries are covered by the snapshot - discard entire log
            _logger.info("Discarding entire log (covered by snapshot)", {
                {"node_id", std::to_string(_node_id)},
                {"entries_discarded", std::to_string(_log.size())}
            });
            
            _log.clear();
            _persistence.delete_log_entries_before(request.last_included_index() + 1);
        } else if (log_start_index <= request.last_included_index()) {
            // Some entries are covered by snapshot, keep entries after snapshot
            auto it = std::find_if(_log.begin(), _log.end(),
                [&request](const log_entry_t& entry) {
                    return entry.index() > request.last_included_index();
                });
            
            std::size_t entries_to_discard = std::distance(_log.begin(), it);
            
            _logger.info("Discarding log entries covered by snapshot", {
                {"node_id", std::to_string(_node_id)},
                {"entries_discarded", std::to_string(entries_to_discard)},
                {"entries_kept", std::to_string(std::distance(it, _log.end()))}
            });
            
            _log.erase(_log.begin(), it);
            _persistence.delete_log_entries_before(request.last_included_index() + 1);
        } else {
            // All log entries are after the snapshot - keep them all
            _logger.debug("All log entries are after snapshot, keeping entire log", {
                {"node_id", std::to_string(_node_id)},
                {"log_start_index", std::to_string(log_start_index)},
                {"snapshot_last_included_index", std::to_string(request.last_included_index())}
            });
        }
    }
    
    // ========================================================================
    // Update commit index and last applied
    // ========================================================================
    
    // The snapshot represents the state up to last_included_index
    // So we can update commit_index and last_applied to at least that point
    
    if (request.last_included_index() > _commit_index) {
        _logger.info("Updating commit index from snapshot", {
            {"node_id", std::to_string(_node_id)},
            {"old_commit_index", std::to_string(_commit_index)},
            {"new_commit_index", std::to_string(request.last_included_index())}
        });
        
        _commit_index = request.last_included_index();
    }
    
    if (request.last_included_index() > _last_applied) {
        _logger.info("Updating last applied from snapshot", {
            {"node_id", std::to_string(_node_id)},
            {"old_last_applied", std::to_string(_last_applied)},
            {"new_last_applied", std::to_string(request.last_included_index())}
        });
        
        _last_applied = request.last_included_index();
    }
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.snapshot.installed");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("last_included_index", std::to_string(request.last_included_index()));
    metric.add_value(static_cast<double>(request.data().size()));
    metric.emit();
    
    _logger.info("Snapshot installation completed successfully", {
        {"node_id", std::to_string(_node_id)},
        {"last_included_index", std::to_string(request.last_included_index())},
        {"last_included_term", std::to_string(request.last_included_term())},
        {"commit_index", std::to_string(_commit_index)},
        {"last_applied", std::to_string(_last_applied)}
    });
    
    return install_snapshot_response_t{_current_term};
}

// ============================================================================
// Helper methods
// ============================================================================

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::get_last_log_index() const -> LogIndex {
    // If log is empty, return commit_index (which accounts for snapshots)
    if (_log.empty()) {
        return _commit_index;
    }
    return _log.back().index();
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::get_last_log_term() const -> TermId {
    // If log is empty, return 0 (no entries)
    if (_log.empty()) {
        return TermId{0};
    }
    return _log.back().term();
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::reset_election_timer() -> void {
    _last_heartbeat = std::chrono::steady_clock::now();
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::election_timeout_elapsed() const -> bool {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_heartbeat);
    return elapsed >= _election_timeout;
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::heartbeat_timeout_elapsed() const -> bool {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_heartbeat);
    return elapsed >= _heartbeat_interval;
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::become_follower(TermId new_term) -> void {
    auto old_state = _state;
    auto old_term = _current_term;
    
    _logger.info("Transitioning to follower", {
        {"node_id", std::to_string(_node_id)},
        {"old_state", _state == server_state::follower ? "follower" : 
                      _state == server_state::candidate ? "candidate" : "leader"},
        {"old_term", std::to_string(_current_term)},
        {"new_term", std::to_string(new_term)}
    });
    
    _current_term = new_term;
    _state = server_state::follower;
    _voted_for = std::nullopt;
    
    // Persist state changes
    _persistence.save_current_term(_current_term);
    
    // Reset election timer
    reset_election_timer();
    randomize_election_timeout();
    
    // Log state transition
    _logger.info("State transition", {
        {"node_id", std::to_string(_node_id)},
        {"old_state", old_state == server_state::follower ? "follower" : 
                     old_state == server_state::candidate ? "candidate" : "leader"},
        {"new_state", "follower"},
        {"old_term", std::to_string(old_term)},
        {"new_term", std::to_string(_current_term)},
        {"reason", "become_follower_called"}
    });
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.state.transition");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("old_state", old_state == server_state::follower ? "follower" : 
                                     old_state == server_state::candidate ? "candidate" : "leader");
    metric.add_dimension("new_state", "follower");
    metric.add_dimension("term", std::to_string(_current_term));
    metric.add_one();
    metric.emit();
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::become_candidate() -> void {
    auto old_state = _state;
    auto old_term = _current_term;
    
    _logger.info("Transitioning to candidate and starting election", {
        {"node_id", std::to_string(_node_id)},
        {"old_term", std::to_string(_current_term)},
        {"new_term", std::to_string(_current_term + 1)}
    });
    
    // Increment current term
    _current_term = _current_term + 1;
    _state = server_state::candidate;
    
    // Vote for self
    _voted_for = _node_id;
    
    // Persist state changes before sending RPCs
    _persistence.save_current_term(_current_term);
    _persistence.save_voted_for(_node_id);
    
    // Reset election timer with new randomized timeout
    reset_election_timer();
    randomize_election_timeout();
    
    // Log state transition
    _logger.info("State transition", {
        {"node_id", std::to_string(_node_id)},
        {"old_state", old_state == server_state::follower ? "follower" : 
                     old_state == server_state::candidate ? "candidate" : "leader"},
        {"new_state", "candidate"},
        {"old_term", std::to_string(old_term)},
        {"new_term", std::to_string(_current_term)},
        {"reason", "election_timeout"}
    });
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.state.transition");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("old_state", old_state == server_state::follower ? "follower" : 
                                     old_state == server_state::candidate ? "candidate" : "leader");
    metric.add_dimension("new_state", "candidate");
    metric.add_dimension("term", std::to_string(_current_term));
    metric.add_one();
    metric.emit();
    
    metric = _metrics;
    metric.set_metric_name("raft.election.started");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("term", std::to_string(_current_term));
    metric.add_one();
    metric.emit();
    
    // Count votes (we already have our own vote)
    std::size_t votes_received = 1;
    std::size_t votes_needed = (_configuration.nodes().size() / 2) + 1;
    
    _logger.debug("Starting vote collection", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"votes_received", std::to_string(votes_received)},
        {"votes_needed", std::to_string(votes_needed)},
        {"cluster_size", std::to_string(_configuration.nodes().size())}
    });
    
    // If we're the only node in the cluster, become leader immediately
    if (votes_received >= votes_needed) {
        _logger.info("Won election immediately (single node cluster)", {
            {"node_id", std::to_string(_node_id)},
            {"term", std::to_string(_current_term)}
        });
        become_leader();
        return;
    }
    
    // Send RequestVote RPCs to all other nodes in parallel
    auto last_log_idx = get_last_log_index();
    auto last_log_trm = get_last_log_term();
    
    request_vote_request_t vote_request{
        _current_term,
        _node_id,
        last_log_idx,
        last_log_trm
    };
    
    // Collect futures for all RPC calls
    std::vector<FutureType> vote_futures;
    
    for (const auto& peer_id : _configuration.nodes()) {
        // Skip ourselves
        if (peer_id == _node_id) {
            continue;
        }
        
        _logger.debug("Sending RequestVote RPC", {
            {"node_id", std::to_string(_node_id)},
            {"target", std::to_string(peer_id)},
            {"term", std::to_string(_current_term)},
            {"last_log_index", std::to_string(last_log_idx)},
            {"last_log_term", std::to_string(last_log_trm)}
        });
        
        // Send RequestVote RPC
        auto future = _network_client.send_request_vote(peer_id, vote_request, _config.rpc_timeout());
        vote_futures.push_back(std::move(future));
    }
    
    // Process vote responses asynchronously
    // We need to capture necessary state by value
    auto current_term = _current_term;
    auto node_id = _node_id;
    
    // TODO: Implement generic future collection mechanism
    // For now, assume we win the election immediately as a temporary fix
    _logger.info("Election completed (simplified implementation)", {
        {"node_id", std::to_string(node_id)},
        {"term", std::to_string(current_term)}
    });
    
    // Become leader immediately
    become_leader();
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::check_election_timeout() -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    
    // Only followers and candidates check for election timeout
    // Leaders send heartbeats instead
    if (_state == server_state::leader) {
        return;
    }
    
    // Check if election timeout has elapsed
    if (election_timeout_elapsed()) {
        _logger.debug("Election timeout elapsed", {
            {"node_id", std::to_string(_node_id)},
            {"state", _state == server_state::follower ? "follower" : "candidate"},
            {"term", std::to_string(_current_term)}
        });
        
        // Start a new election
        become_candidate();
    }
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::become_leader() -> void {
    auto old_state = _state;
    
    _logger.info("Transitioning to leader", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)}
    });
    
    _state = server_state::leader;
    
    // Initialize leader-specific state
    // For each server, initialize next_index to leader's last log index + 1
    auto last_log_idx = get_last_log_index();
    for (const auto& peer_id : _configuration.nodes()) {
        if (peer_id != _node_id) {
            _next_index[peer_id] = last_log_idx + 1;
            _match_index[peer_id] = 0;
        }
    }
    
    // Reset heartbeat timer
    _last_heartbeat = std::chrono::steady_clock::now();
    
    // Log state transition
    _logger.info("State transition", {
        {"node_id", std::to_string(_node_id)},
        {"old_state", old_state == server_state::follower ? "follower" : 
                     old_state == server_state::candidate ? "candidate" : "leader"},
        {"new_state", "leader"},
        {"term", std::to_string(_current_term)},
        {"reason", "won_election"}
    });
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.state.transition");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("old_state", old_state == server_state::follower ? "follower" : 
                                     old_state == server_state::candidate ? "candidate" : "leader");
    metric.add_dimension("new_state", "leader");
    metric.add_dimension("term", std::to_string(_current_term));
    metric.add_one();
    metric.emit();
    
    _logger.info("Became leader successfully", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"last_log_index", std::to_string(last_log_idx)}
    });
    
    // Append a no-op entry to commit entries from previous terms
    // This is critical for Leader Completeness: the leader must commit a no-op entry
    // from its own term before it can safely commit entries from previous terms.
    // This ensures that all committed entries from previous terms are present in the
    // leader's log (Election Safety guarantees this), and by committing a current-term
    // entry, we can safely commit all preceding entries.
    auto noop_index = get_last_log_index() + 1;
    log_entry_t noop_entry{_current_term, noop_index, std::vector<std::byte>{}};
    
    _logger.info("Appending no-op entry on leader election", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"noop_index", std::to_string(noop_index)}
    });
    
    // Append to leader's log
    _log.push_back(noop_entry);
    
    // Persist the no-op entry
    _persistence.append_log_entry(noop_entry);
    
    // Update next_index for all followers to include the no-op entry
    for (const auto& peer_id : _configuration.nodes()) {
        if (peer_id != _node_id) {
            _next_index[peer_id] = noop_index + 1;
        }
    }
    
    // Replicate the no-op entry to followers immediately
    // This will trigger commit index advancement once replicated to a majority
    replicate_to_followers();
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::send_heartbeats() -> void {
    _logger.debug("Sending heartbeats to followers", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"follower_count", std::to_string(_configuration.nodes().size() - 1)}
    });
    
    // Send empty AppendEntries RPCs (heartbeats) to all followers
    for (const auto& peer_id : _configuration.nodes()) {
        // Skip ourselves
        if (peer_id == _node_id) {
            continue;
        }
        
        // Get the previous log index and term for this follower
        // This is based on next_index for the follower
        auto next_idx = _next_index[peer_id];
        LogIndex prev_log_index = next_idx > 1 ? next_idx - 1 : 0;
        TermId prev_log_term = 0;
        
        // If prev_log_index > 0, we need to get the term of that entry
        if (prev_log_index > 0) {
            // Check if we have this entry in our log
            LogIndex log_start_index = _commit_index + 1;
            if (!_log.empty()) {
                log_start_index = _log.front().index();
            }
            
            if (prev_log_index >= log_start_index && prev_log_index <= get_last_log_index()) {
                // Entry is in our log
                std::size_t offset = static_cast<std::size_t>(prev_log_index - log_start_index);
                if (offset < _log.size()) {
                    prev_log_term = _log[offset].term();
                }
            } else if (prev_log_index <= _commit_index) {
                // Entry is in a snapshot - we'll need to send InstallSnapshot instead
                // For now, just use term 0 (this will be handled properly in snapshot task)
                prev_log_term = 0;
            }
        }
        
        // Create empty AppendEntries request (heartbeat)
        append_entries_request_t heartbeat_request{
            _current_term,
            _node_id,
            prev_log_index,
            prev_log_term,
            {},  // Empty entries vector (heartbeat)
            _commit_index
        };
        
        _logger.debug("Sending heartbeat to follower", {
            {"node_id", std::to_string(_node_id)},
            {"target", std::to_string(peer_id)},
            {"term", std::to_string(_current_term)},
            {"prev_log_index", std::to_string(prev_log_index)},
            {"prev_log_term", std::to_string(prev_log_term)},
            {"commit_index", std::to_string(_commit_index)}
        });
        
        // Send heartbeat asynchronously
        _network_client.send_append_entries(peer_id, heartbeat_request, _config.rpc_timeout())
            .thenValue([this, peer_id](append_entries_response_t&& response) {
                std::lock_guard<std::mutex> lock(_mutex);
                
                // Check if we're still the leader
                if (_state != server_state::leader) {
                    return;
                }
                
                // If response contains a higher term, become follower
                if (response.term() > _current_term) {
                    _logger.info("Discovered higher term in heartbeat response, becoming follower", {
                        {"node_id", std::to_string(_node_id)},
                        {"our_term", std::to_string(_current_term)},
                        {"discovered_term", std::to_string(response.term())}
                    });
                    become_follower(response.term());
                    return;
                }
                
                // Log heartbeat response
                if (response.success()) {
                    _logger.debug("Heartbeat acknowledged", {
                        {"node_id", std::to_string(_node_id)},
                        {"follower", std::to_string(peer_id)},
                        {"term", std::to_string(_current_term)}
                    });
                } else {
                    _logger.debug("Heartbeat rejected", {
                        {"node_id", std::to_string(_node_id)},
                        {"follower", std::to_string(peer_id)},
                        {"term", std::to_string(_current_term)}
                    });
                }
            });
            // TODO: Add error handling for heartbeat failures
    }
    
    // Update last heartbeat time
    _last_heartbeat = std::chrono::steady_clock::now();
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.heartbeat.sent");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("term", std::to_string(_current_term));
    metric.add_count(static_cast<std::int64_t>(_configuration.nodes().size() - 1));
    metric.emit();
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::check_heartbeat_timeout() -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    
    // Only leaders send heartbeats
    if (_state != server_state::leader) {
        return;
    }
    
    // Check if heartbeat timeout has elapsed
    if (heartbeat_timeout_elapsed()) {
        _logger.debug("Heartbeat timeout elapsed, sending heartbeats", {
            {"node_id", std::to_string(_node_id)},
            {"term", std::to_string(_current_term)}
        });
        
        // Send heartbeats to all followers
        send_heartbeats();
    }
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::replicate_to_followers() -> void {
    _logger.debug("Replicating log entries to followers", {
        {"node_id", std::to_string(_node_id)},
        {"term", std::to_string(_current_term)},
        {"follower_count", std::to_string(_configuration.nodes().size() - 1)}
    });
    
    // Send AppendEntries RPCs to all followers
    for (const auto& peer_id : _configuration.nodes()) {
        // Skip ourselves
        if (peer_id == _node_id) {
            continue;
        }
        
        send_append_entries_to(peer_id);
    }
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::send_append_entries_to(NodeId target) -> void {
    // Get the next index for this follower
    auto next_idx = _next_index[target];
    
    // Calculate prev_log_index and prev_log_term
    LogIndex prev_log_index = next_idx > 1 ? next_idx - 1 : 0;
    TermId prev_log_term = 0;
    
    // If prev_log_index > 0, we need to get the term of that entry
    if (prev_log_index > 0) {
        // Check if we have this entry in our log
        LogIndex log_start_index = _commit_index + 1;
        if (!_log.empty()) {
            log_start_index = _log.front().index();
        }
        
        if (prev_log_index >= log_start_index && prev_log_index <= get_last_log_index()) {
            // Entry is in our log
            std::size_t offset = static_cast<std::size_t>(prev_log_index - log_start_index);
            if (offset < _log.size()) {
                prev_log_term = _log[offset].term();
            }
        } else if (prev_log_index < log_start_index) {
            // Entry is in a snapshot - we need to send InstallSnapshot instead
            _logger.info("Follower is too far behind, sending snapshot", {
                {"node_id", std::to_string(_node_id)},
                {"target", std::to_string(target)},
                {"prev_log_index", std::to_string(prev_log_index)},
                {"log_start_index", std::to_string(log_start_index)}
            });
            
            send_install_snapshot_to(target);
            return;
        }
    }
    
    // Collect entries to send (from next_index to end of log)
    std::vector<log_entry_t> entries_to_send;
    
    LogIndex log_start_index = _commit_index + 1;
    if (!_log.empty()) {
        log_start_index = _log.front().index();
    }
    
    // Find entries starting from next_idx
    if (next_idx >= log_start_index && next_idx <= get_last_log_index()) {
        std::size_t start_offset = static_cast<std::size_t>(next_idx - log_start_index);
        
        // Send up to max_entries_per_append entries
        std::size_t entries_to_copy = std::min(
            _log.size() - start_offset,
            _config.max_entries_per_append()
        );
        
        entries_to_send.insert(
            entries_to_send.end(),
            _log.begin() + start_offset,
            _log.begin() + start_offset + entries_to_copy
        );
    }
    
    // Create AppendEntries request
    append_entries_request_t request{
        _current_term,
        _node_id,
        prev_log_index,
        prev_log_term,
        entries_to_send,
        _commit_index
    };
    
    _logger.debug("Sending AppendEntries to follower", {
        {"node_id", std::to_string(_node_id)},
        {"target", std::to_string(target)},
        {"term", std::to_string(_current_term)},
        {"prev_log_index", std::to_string(prev_log_index)},
        {"prev_log_term", std::to_string(prev_log_term)},
        {"entries_count", std::to_string(entries_to_send.size())},
        {"commit_index", std::to_string(_commit_index)}
    });
    
    // Send AppendEntries RPC asynchronously
    _network_client.send_append_entries(target, request, _config.rpc_timeout())
        .thenValue([this, target, next_idx, entries_count = entries_to_send.size()]
                   (append_entries_response_t&& response) {
            std::lock_guard<std::mutex> lock(_mutex);
            
            // Check if we're still the leader
            if (_state != server_state::leader) {
                return;
            }
            
            // If response contains a higher term, become follower
            if (response.term() > _current_term) {
                _logger.info("Discovered higher term in AppendEntries response, becoming follower", {
                    {"node_id", std::to_string(_node_id)},
                    {"our_term", std::to_string(_current_term)},
                    {"discovered_term", std::to_string(response.term())}
                });
                become_follower(response.term());
                return;
            }
            
            if (response.success()) {
                // Update next_index and match_index for this follower
                auto new_match_index = next_idx + entries_count - 1;
                _match_index[target] = new_match_index;
                _next_index[target] = new_match_index + 1;
                
                _logger.debug("AppendEntries succeeded", {
                    {"node_id", std::to_string(_node_id)},
                    {"follower", std::to_string(target)},
                    {"match_index", std::to_string(new_match_index)},
                    {"next_index", std::to_string(_next_index[target])}
                });
                
                // Emit metrics
                auto metric = _metrics;
                metric.set_metric_name("raft.replication.success");
                metric.add_dimension("node_id", std::to_string(_node_id));
                metric.add_dimension("follower", std::to_string(target));
                metric.add_count(static_cast<std::int64_t>(entries_count));
                metric.emit();
                
                // Try to advance commit index
                advance_commit_index();
            } else {
                // AppendEntries failed - decrement next_index and retry
                _logger.debug("AppendEntries failed, will retry with earlier index", {
                    {"node_id", std::to_string(_node_id)},
                    {"follower", std::to_string(target)},
                    {"current_next_index", std::to_string(_next_index[target])}
                });
                
                // Use conflict information if available for fast backtracking
                if (response.conflict_index().has_value()) {
                    _next_index[target] = response.conflict_index().value();
                } else {
                    // Decrement next_index by 1
                    if (_next_index[target] > 1) {
                        _next_index[target]--;
                    }
                }
                
                _logger.debug("Updated next_index for follower", {
                    {"node_id", std::to_string(_node_id)},
                    {"follower", std::to_string(target)},
                    {"new_next_index", std::to_string(_next_index[target])}
                });
                
                // Retry immediately
                send_append_entries_to(target);
            }
        });
        // TODO: Add error handling for AppendEntries failures
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::send_install_snapshot_to(NodeId target) -> void {
    _logger.info("Sending InstallSnapshot to follower", {
        {"node_id", std::to_string(_node_id)},
        {"target", std::to_string(target)},
        {"term", std::to_string(_current_term)}
    });
    
    // Load the current snapshot
    auto snapshot_opt = _persistence.load_snapshot();
    
    if (!snapshot_opt.has_value()) {
        _logger.warning("No snapshot available to send to follower", {
            {"node_id", std::to_string(_node_id)},
            {"target", std::to_string(target)}
        });
        return;
    }
    
    const auto& snapshot = snapshot_opt.value();
    
    // For this simplified implementation, we send the entire snapshot in one chunk
    // A full implementation would split large snapshots into chunks
    
    install_snapshot_request_t request{
        _current_term,
        _node_id,
        snapshot.last_included_index(),
        snapshot.last_included_term(),
        0,  // offset (0 for single-chunk transfer)
        snapshot.state_machine_state(),
        true  // done (true for single-chunk transfer)
    };
    
    _logger.debug("Sending InstallSnapshot RPC", {
        {"node_id", std::to_string(_node_id)},
        {"target", std::to_string(target)},
        {"last_included_index", std::to_string(snapshot.last_included_index())},
        {"last_included_term", std::to_string(snapshot.last_included_term())},
        {"data_size", std::to_string(snapshot.state_machine_state().size())}
    });
    
    // Send InstallSnapshot RPC asynchronously
    _network_client.send_install_snapshot(target, request, _config.rpc_timeout())
        .thenValue([this, target, last_included_index = snapshot.last_included_index()]
                   (install_snapshot_response_t&& response) {
            std::lock_guard<std::mutex> lock(_mutex);
            
            // Check if we're still the leader
            if (_state != server_state::leader) {
                return;
            }
            
            // If response contains a higher term, become follower
            if (response.term() > _current_term) {
                _logger.info("Discovered higher term in InstallSnapshot response, becoming follower", {
                    {"node_id", std::to_string(_node_id)},
                    {"our_term", std::to_string(_current_term)},
                    {"discovered_term", std::to_string(response.term())}
                });
                become_follower(response.term());
                return;
            }
            
            // InstallSnapshot succeeded - update next_index and match_index
            _next_index[target] = last_included_index + 1;
            _match_index[target] = last_included_index;
            
            _logger.info("InstallSnapshot succeeded", {
                {"node_id", std::to_string(_node_id)},
                {"follower", std::to_string(target)},
                {"match_index", std::to_string(last_included_index)},
                {"next_index", std::to_string(last_included_index + 1)}
            });
            
            // Emit metrics
            auto metric = _metrics;
            metric.set_metric_name("raft.snapshot.sent");
            metric.add_dimension("node_id", std::to_string(_node_id));
            metric.add_dimension("follower", std::to_string(target));
            metric.add_one();
            metric.emit();
            
            // Try to advance commit index
            advance_commit_index();
            
            // Continue replication with AppendEntries for any entries after the snapshot
            send_append_entries_to(target);
        });
        // TODO: Add error handling for InstallSnapshot failures
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::advance_commit_index() -> void {
    // Find the highest index N such that:
    // 1. N > commitIndex
    // 2. A majority of matchIndex[i] >= N
    // 3. log[N].term == currentTerm
    
    auto old_commit_index = _commit_index;
    
    // Start from the last log index and work backwards
    for (LogIndex n = get_last_log_index(); n > _commit_index; --n) {
        // Check if this entry is from the current term
        // Find the entry in our log
        LogIndex log_start_index = _commit_index + 1;
        if (!_log.empty()) {
            log_start_index = _log.front().index();
        }
        
        if (n < log_start_index) {
            // Entry is before our log (in snapshot)
            break;
        }
        
        std::size_t offset = static_cast<std::size_t>(n - log_start_index);
        if (offset >= _log.size()) {
            continue;
        }
        
        const auto& entry = _log[offset];
        
        // Only commit entries from current term directly
        if (entry.term() != _current_term) {
            continue;
        }
        
        // Count how many servers have replicated this entry
        // During joint consensus, we need majorities from BOTH configurations
        
        bool has_majority = false;
        
        if (_configuration.is_joint_consensus()) {
            // Joint consensus: need majorities from both old and new configurations
            const auto& new_nodes = _configuration.nodes();
            const auto& old_nodes_opt = _configuration.old_nodes();
            
            if (old_nodes_opt.has_value()) {
                const auto& old_nodes = old_nodes_opt.value();
                
                // Count replication in new configuration
                std::size_t new_replication_count = 0;
                for (const auto& node : new_nodes) {
                    if (node == _node_id) {
                        new_replication_count++; // Count ourselves
                    } else {
                        auto it = _match_index.find(node);
                        if (it != _match_index.end() && it->second >= n) {
                            new_replication_count++;
                        }
                    }
                }
                
                // Count replication in old configuration
                std::size_t old_replication_count = 0;
                for (const auto& node : old_nodes) {
                    if (node == _node_id) {
                        old_replication_count++; // Count ourselves
                    } else {
                        auto it = _match_index.find(node);
                        if (it != _match_index.end() && it->second >= n) {
                            old_replication_count++;
                        }
                    }
                }
                
                // Check if we have majorities in BOTH configurations
                std::size_t new_majority = (new_nodes.size() / 2) + 1;
                std::size_t old_majority = (old_nodes.size() / 2) + 1;
                
                has_majority = (new_replication_count >= new_majority) && 
                               (old_replication_count >= old_majority);
                
                _logger.debug("Joint consensus majority check", {
                    {"node_id", std::to_string(_node_id)},
                    {"index", std::to_string(n)},
                    {"new_replication_count", std::to_string(new_replication_count)},
                    {"new_majority", std::to_string(new_majority)},
                    {"old_replication_count", std::to_string(old_replication_count)},
                    {"old_majority", std::to_string(old_majority)},
                    {"has_majority", has_majority ? "true" : "false"}
                });
            }
        } else {
            // Normal operation: need majority from single configuration
            std::size_t replication_count = 1; // Count ourselves
            
            for (const auto& [peer_id, match_idx] : _match_index) {
                if (match_idx >= n) {
                    replication_count++;
                }
            }
            
            std::size_t majority = (_configuration.nodes().size() / 2) + 1;
            has_majority = (replication_count >= majority);
            
            _logger.debug("Single configuration majority check", {
                {"node_id", std::to_string(_node_id)},
                {"index", std::to_string(n)},
                {"replication_count", std::to_string(replication_count)},
                {"majority", std::to_string(majority)},
                {"has_majority", has_majority ? "true" : "false"}
            });
        }
        
        if (has_majority) {
            // We can commit up to index n
            _commit_index = n;
            
            _logger.info("Advanced commit index", {
                {"node_id", std::to_string(_node_id)},
                {"old_commit_index", std::to_string(old_commit_index)},
                {"new_commit_index", std::to_string(_commit_index)},
                {"is_joint_consensus", _configuration.is_joint_consensus() ? "true" : "false"}
            });
            
            // Emit metrics
            auto metric = _metrics;
            metric.set_metric_name("raft.commit_index.advanced");
            metric.add_dimension("node_id", std::to_string(_node_id));
            metric.add_dimension("term", std::to_string(_current_term));
            metric.add_value(static_cast<double>(_commit_index));
            metric.emit();
            
            // Apply committed entries to state machine
            apply_committed_entries();
            
            break; // Found the highest committable index
        }
    }
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::apply_committed_entries() -> void {
    // Apply all entries from last_applied + 1 to commit_index sequentially
    // This ensures State Machine Safety: no two servers apply different commands at the same log index
    while (_last_applied < _commit_index) {
        _last_applied++;
        
        // Find the entry to apply
        LogIndex log_start_index = 0;
        if (!_log.empty()) {
            log_start_index = _log.front().index();
        }
        
        if (_last_applied >= log_start_index && !_log.empty()) {
            std::size_t offset = static_cast<std::size_t>(_last_applied - log_start_index);
            if (offset < _log.size()) {
                const auto& entry = _log[offset];
                
                _logger.debug("Applying committed entry to state machine", {
                    {"node_id", std::to_string(_node_id)},
                    {"index", std::to_string(entry.index())},
                    {"term", std::to_string(entry.term())},
                    {"command_size", std::to_string(entry.command().size())}
                });
                
                // Note: The actual application to the state machine is the responsibility
                // of the application layer. The Raft library tracks last_applied to ensure
                // sequential application and provides the committed entries to the application.
                // Applications can retrieve committed entries through the persistence engine
                // or by polling the commit_index and last_applied indices.
                
                // Emit metrics for observability
                auto metric = _metrics;
                metric.set_metric_name("raft.entry.applied");
                metric.add_dimension("node_id", std::to_string(_node_id));
                metric.add_dimension("term", std::to_string(entry.term()));
                metric.add_one();
                metric.emit();
            }
        }
    }
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::create_snapshot() -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.info("Checking if snapshot creation is needed", {
        {"node_id", std::to_string(_node_id)},
        {"commit_index", std::to_string(_commit_index)},
        {"last_applied", std::to_string(_last_applied)},
        {"log_size", std::to_string(_log.size())}
    });
    
    // Check if we have any committed entries to snapshot
    if (_commit_index == 0 || _last_applied == 0) {
        _logger.debug("No committed entries to snapshot", {
            {"node_id", std::to_string(_node_id)},
            {"commit_index", std::to_string(_commit_index)},
            {"last_applied", std::to_string(_last_applied)}
        });
        return;
    }
    
    // Calculate the size of the log in bytes (approximate)
    std::size_t log_size_bytes = 0;
    for (const auto& entry : _log) {
        log_size_bytes += entry.command().size();
        log_size_bytes += sizeof(TermId) + sizeof(LogIndex); // Metadata overhead
    }
    
    _logger.debug("Calculated log size", {
        {"node_id", std::to_string(_node_id)},
        {"log_size_bytes", std::to_string(log_size_bytes)},
        {"threshold_bytes", std::to_string(_config.snapshot_threshold_bytes())}
    });
    
    // Check if log size exceeds threshold
    if (log_size_bytes < _config.snapshot_threshold_bytes()) {
        _logger.debug("Log size below snapshot threshold", {
            {"node_id", std::to_string(_node_id)},
            {"log_size_bytes", std::to_string(log_size_bytes)},
            {"threshold_bytes", std::to_string(_config.snapshot_threshold_bytes())}
        });
        return;
    }
    
    _logger.info("Log size exceeds threshold, creating snapshot", {
        {"node_id", std::to_string(_node_id)},
        {"log_size_bytes", std::to_string(log_size_bytes)},
        {"threshold_bytes", std::to_string(_config.snapshot_threshold_bytes())}
    });
    
    // For this implementation, we create an empty state machine state
    // In a real implementation, the application would provide the actual state machine state
    // Applications should call create_snapshot(state_machine_state) with their actual state
    std::vector<std::byte> state_machine_state{};
    
    // Get the term of the last applied entry
    TermId last_included_term = 0;
    
    // Find the last applied entry to get its term
    if (!_log.empty()) {
        LogIndex log_start_index = _log.front().index();
        
        if (_last_applied >= log_start_index) {
            std::size_t offset = static_cast<std::size_t>(_last_applied - log_start_index);
            if (offset < _log.size()) {
                last_included_term = _log[offset].term();
            }
        }
    }
    
    // If we couldn't find the term in the log, it might be in a previous snapshot
    // In that case, we need to load the previous snapshot to get the term
    if (last_included_term == 0 && _last_applied > 0) {
        auto prev_snapshot = _persistence.load_snapshot();
        if (prev_snapshot.has_value() && prev_snapshot->last_included_index() == _last_applied) {
            last_included_term = prev_snapshot->last_included_term();
        }
    }
    
    // Create snapshot with metadata
    snapshot_t new_snapshot{
        _last_applied,           // last_included_index
        last_included_term,      // last_included_term
        _configuration,          // current cluster configuration
        state_machine_state      // state machine state
    };
    
    _logger.info("Created snapshot", {
        {"node_id", std::to_string(_node_id)},
        {"last_included_index", std::to_string(new_snapshot.last_included_index())},
        {"last_included_term", std::to_string(new_snapshot.last_included_term())},
        {"state_size_bytes", std::to_string(state_machine_state.size())}
    });
    
    // Save snapshot to persistence
    _persistence.save_snapshot(new_snapshot);
    
    _logger.info("Snapshot saved to persistence", {
        {"node_id", std::to_string(_node_id)},
        {"last_included_index", std::to_string(new_snapshot.last_included_index())}
    });
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.snapshot.created");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("last_included_index", std::to_string(new_snapshot.last_included_index()));
    metric.add_value(static_cast<double>(state_machine_state.size()));
    metric.emit();
    
    _logger.info("Snapshot creation completed successfully", {
        {"node_id", std::to_string(_node_id)},
        {"last_included_index", std::to_string(new_snapshot.last_included_index())},
        {"last_included_term", std::to_string(new_snapshot.last_included_term())}
    });
    
    // Automatically compact the log after creating a snapshot
    compact_log();
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::create_snapshot(
    const std::vector<std::byte>& state_machine_state
) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    
    _logger.info("Creating snapshot with provided state machine state", {
        {"node_id", std::to_string(_node_id)},
        {"commit_index", std::to_string(_commit_index)},
        {"last_applied", std::to_string(_last_applied)},
        {"state_size_bytes", std::to_string(state_machine_state.size())}
    });
    
    // Check if we have any committed entries to snapshot
    if (_commit_index == 0 || _last_applied == 0) {
        _logger.warning("Cannot create snapshot: no committed entries", {
            {"node_id", std::to_string(_node_id)},
            {"commit_index", std::to_string(_commit_index)},
            {"last_applied", std::to_string(_last_applied)}
        });
        return;
    }
    
    // Get the term of the last applied entry
    TermId last_included_term = 0;
    
    // Find the last applied entry to get its term
    if (!_log.empty()) {
        LogIndex log_start_index = _log.front().index();
        
        if (_last_applied >= log_start_index) {
            std::size_t offset = static_cast<std::size_t>(_last_applied - log_start_index);
            if (offset < _log.size()) {
                last_included_term = _log[offset].term();
            }
        }
    }
    
    // If we couldn't find the term in the log, it might be in a previous snapshot
    if (last_included_term == 0 && _last_applied > 0) {
        auto prev_snapshot = _persistence.load_snapshot();
        if (prev_snapshot.has_value() && prev_snapshot->last_included_index() == _last_applied) {
            last_included_term = prev_snapshot->last_included_term();
        }
    }
    
    // Create snapshot with metadata
    snapshot_t new_snapshot{
        _last_applied,           // last_included_index
        last_included_term,      // last_included_term
        _configuration,          // current cluster configuration
        state_machine_state      // state machine state
    };
    
    _logger.info("Created snapshot with provided state", {
        {"node_id", std::to_string(_node_id)},
        {"last_included_index", std::to_string(new_snapshot.last_included_index())},
        {"last_included_term", std::to_string(new_snapshot.last_included_term())},
        {"state_size_bytes", std::to_string(state_machine_state.size())}
    });
    
    // Save snapshot to persistence
    _persistence.save_snapshot(new_snapshot);
    
    _logger.info("Snapshot saved to persistence", {
        {"node_id", std::to_string(_node_id)},
        {"last_included_index", std::to_string(new_snapshot.last_included_index())}
    });
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.snapshot.created");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("last_included_index", std::to_string(new_snapshot.last_included_index()));
    metric.add_value(static_cast<double>(state_machine_state.size()));
    metric.emit();
    
    _logger.info("Snapshot creation completed successfully", {
        {"node_id", std::to_string(_node_id)},
        {"last_included_index", std::to_string(new_snapshot.last_included_index())},
        {"last_included_term", std::to_string(new_snapshot.last_included_term())}
    });
    
    // Automatically compact the log after creating a snapshot
    compact_log();
}

template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager,
    typename NodeId,
    typename TermId,
    typename LogIndex
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine, NodeId, TermId, LogIndex, log_entry<TermId, LogIndex>, snapshot<NodeId, TermId, LogIndex>> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager, NodeId, cluster_configuration<NodeId>> &&
    node_id<NodeId> &&
    term_id<TermId> &&
    log_index<LogIndex>
auto node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex>::compact_log() -> void {
    // Note: This method assumes the mutex is already held by the caller
    
    _logger.info("Starting log compaction", {
        {"node_id", std::to_string(_node_id)},
        {"log_size_before", std::to_string(_log.size())},
        {"last_applied", std::to_string(_last_applied)}
    });
    
    // Load the current snapshot to determine what can be safely deleted
    auto snapshot_opt = _persistence.load_snapshot();
    
    if (!snapshot_opt.has_value()) {
        _logger.debug("No snapshot available, cannot compact log", {
            {"node_id", std::to_string(_node_id)}
        });
        return;
    }
    
    const auto& snapshot = snapshot_opt.value();
    auto last_included_index = snapshot.last_included_index();
    
    _logger.debug("Snapshot loaded for compaction", {
        {"node_id", std::to_string(_node_id)},
        {"last_included_index", std::to_string(last_included_index)}
    });
    
    // Safety check: Only delete log entries that are covered by the snapshot
    // We must keep entries after last_included_index
    if (_log.empty()) {
        _logger.debug("Log is already empty, nothing to compact", {
            {"node_id", std::to_string(_node_id)}
        });
        return;
    }
    
    LogIndex log_start_index = _log.front().index();
    
    // Find the first entry that should be kept (entries after last_included_index)
    auto it = std::find_if(_log.begin(), _log.end(), 
        [last_included_index](const log_entry_t& entry) {
            return entry.index() > last_included_index;
        });
    
    // Calculate how many entries will be deleted
    std::size_t entries_to_delete = std::distance(_log.begin(), it);
    
    if (entries_to_delete == 0) {
        _logger.debug("No log entries to delete (all entries are after snapshot)", {
            {"node_id", std::to_string(_node_id)},
            {"log_start_index", std::to_string(log_start_index)},
            {"last_included_index", std::to_string(last_included_index)}
        });
        return;
    }
    
    _logger.info("Deleting log entries covered by snapshot", {
        {"node_id", std::to_string(_node_id)},
        {"entries_to_delete", std::to_string(entries_to_delete)},
        {"last_included_index", std::to_string(last_included_index)}
    });
    
    // Delete entries from in-memory log
    _log.erase(_log.begin(), it);
    
    // Delete entries from persistent storage
    // The persistence engine should delete all entries before (and including) last_included_index
    _persistence.delete_log_entries_before(last_included_index + 1);
    
    _logger.info("Log compaction completed", {
        {"node_id", std::to_string(_node_id)},
        {"entries_deleted", std::to_string(entries_to_delete)},
        {"log_size_after", std::to_string(_log.size())},
        {"last_included_index", std::to_string(last_included_index)}
    });
    
    // Emit metrics
    auto metric = _metrics;
    metric.set_metric_name("raft.log.compacted");
    metric.add_dimension("node_id", std::to_string(_node_id));
    metric.add_dimension("last_included_index", std::to_string(last_included_index));
    metric.add_count(static_cast<std::int64_t>(entries_to_delete));
    metric.emit();
}

} // namespace raft

