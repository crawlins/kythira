#pragma once

/// @file raft.hpp
/// @brief Main Raft consensus node — `node_config<Types>` and `node<Types>`.

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
#include <raft/config_entry.hpp>
#include <raft/quorum_management.hpp>

#include <vector>
#include <optional>
#include <unordered_map>
#include <map>
#include <chrono>
#include <random>
#include <mutex>
#include <atomic>
#include <format>
#include <span>

namespace kythira {

// ============================================================================
// Quorum-manager type-detection helpers
// Mirrors the _bootstrap_type_traits pattern: types that don't define
// quorum_manager_type fall back to no_op_quorum_manager transparently.
// Cannot live in types.hpp because quorum_management.hpp includes types.hpp.
// ============================================================================

template<typename T>
concept _has_quorum_manager_type = requires { typename T::quorum_manager_type; };

// Primary: no quorum_manager_type declared → fall back to no_op
template<typename T, typename NodeId, typename Address, bool = _has_quorum_manager_type<T>>
struct _quorum_manager_type_traits {
    using quorum_manager_type = no_op_quorum_manager<NodeId, Address, std::string>;
};

// Specialisation: quorum_manager_type is present → use it
template<typename T, typename NodeId, typename Address>
struct _quorum_manager_type_traits<T, NodeId, Address, true> {
    using quorum_manager_type = typename T::quorum_manager_type;
};

/// @brief Named-parameter aggregate for constructing a `node<Types>`.
///
/// Required fields have no in-struct default; omitting them in a designated
/// initializer is a compile error.  Optional fields carry defaults and may be
/// omitted without modifying existing call sites.
///
/// @tparam Types A type bundle satisfying the `raft_types` concept.
template<raft_types Types> struct node_config {
    // Type resolution — mirrors node<Types> internal aliases.
    using _bth_ = _bootstrap_type_traits<Types, typename Types::node_id_type>;
    using address_type = typename _bth_::address_type;
    using peer_discovery_type = typename _bth_::peer_discovery_type;
    using _qmth_ = _quorum_manager_type_traits<Types, typename Types::node_id_type, address_type>;
    using quorum_manager_type = typename _qmth_::quorum_manager_type;
    using placement_group_id_type = typename quorum_manager_type::placement_group_id_type;
    using _p2prth_ =
        _peer2peer_replicator_type_traits<Types, typename Types::node_id_type, address_type,
                                          typename Types::log_index_type>;
    using peer2peer_replicator_type = typename _p2prth_::peer2peer_replicator_type;

    // ── Required fields (no in-struct default) ────────────────────────────────
    typename Types::node_id_type node_id;                 ///< Unique identifier for this node.
    typename Types::network_client_type network_client;   ///< Client used to send RPCs to peers.
    typename Types::network_server_type network_server;   ///< Server used to receive incoming RPCs.
    typename Types::persistence_engine_type persistence;  ///< Durable Raft-state store.
    typename Types::logger_type logger;                   ///< Structured diagnostic logger.
    typename Types::metrics_type metrics;                 ///< Metrics back-end.
    typename Types::membership_manager_type membership;   ///< Membership-change policy.

    // ── Optional fields (have defaults) ──────────────────────────────────────
    typename Types::state_machine_type state_machine{};  ///< Application state machine.
    typename Types::configuration_type config{};         ///< Raft timing and policy configuration.
    address_type self_address{};           ///< This node's advertised network address.
    peer_discovery_type peer_discovery{};  ///< Peer-discovery back-end.
    quorum_manager_type quorum_manager{};  ///< Infrastructure quorum manager.
    std::unordered_map<typename Types::node_id_type, placement_group_id_type>
        initial_placement{};                           ///< Initial node-to-placement-group mapping.
    peer2peer_replicator_type peer2peer_replicator{};  ///< Peer-to-peer catch-up replicator.
};

/// @brief Raft consensus node with fully pluggable component types.
///
/// Implements the Raft consensus algorithm (leader election, log replication,
/// snapshotting, and joint-consensus membership changes).  All component
/// dependencies — network transport, persistence, logger, state machine,
/// quorum manager, and peer discovery — are injected via `Types` and
/// `node_config<Types>`.
///
/// ### Lifecycle
/// 1. Construct with `node_config<Types>` (preferred) or the positional constructor.
/// 2. Call `start()` to begin the election-timeout and heartbeat timers.
/// 3. Call `submit_command()` / `read_state()` from client threads.
/// 4. Call `stop()` to terminate background activity before destruction.
///
/// ### Thread safety
/// All public methods except the constructor and destructor are thread-safe.
///
/// @tparam Types A type bundle satisfying the `raft_types` concept.
///              Defaults to `default_raft_types` for simulator-based use.
template<raft_types Types = default_raft_types> class node {
public:
    /// @name Type aliases
    /// @{
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
    // Optional (`.kiro/specs/raft-pre-vote/`): a direct concrete type, not
    // typename Types::request_pre_vote_request_type, so existing Types
    // bundles that don't implement PreVote aren't required to define it
    // (mirrors fetch_log_entries_request_type's own treatment above).
    using request_pre_vote_request_type =
        request_pre_vote_request<node_id_type, term_id_type, log_index_type>;
    using request_pre_vote_response_type = request_pre_vote_response<term_id_type>;
    using append_entries_request_type = typename Types::append_entries_request_type;
    using append_entries_response_type = typename Types::append_entries_response_type;
    using install_snapshot_request_type = typename Types::install_snapshot_request_type;
    using install_snapshot_response_type = typename Types::install_snapshot_response_type;
    using fetch_log_entries_request_type =
        fetch_log_entries_request<node_id_type, term_id_type, log_index_type>;
    using fetch_log_entries_response_type =
        fetch_log_entries_response<term_id_type, log_index_type, log_entry_type>;

    // Bootstrap type aliases (with fallbacks for Types that predate them)
    using _bth = kythira::_bootstrap_type_traits<Types, node_id_type>;
    using address_type = typename _bth::address_type;
    using peer_discovery_type = typename _bth::peer_discovery_type;
    using cluster_join_request_type = typename _bth::cluster_join_request_type;
    using cluster_join_response_type = typename _bth::cluster_join_response_type;
    using cluster_leave_request_type = typename _bth::cluster_leave_request_type;
    using cluster_leave_response_type = typename _bth::cluster_leave_response_type;

    // Quorum manager type alias (with fallback to no_op for Types that predate it)
    using _qmth = kythira::_quorum_manager_type_traits<Types, node_id_type, address_type>;
    using quorum_manager_type = typename _qmth::quorum_manager_type;
    using placement_group_id_type = typename quorum_manager_type::placement_group_id_type;

    // Peer-to-peer replicator type alias (with fallback to no_op for Types that predate it)
    using _p2prth = kythira::_peer2peer_replicator_type_traits<Types, node_id_type, address_type,
                                                               log_index_type>;
    using peer2peer_replicator_type = typename _p2prth::peer2peer_replicator_type;

    using client_id_t = std::uint64_t;  ///< Opaque client identifier for session tracking.
    using serial_number_t =
        std::uint64_t;  ///< Monotonically increasing client request serial number.
    /// @}

    /// @name Constructors
    /// @{

    /// @brief Preferred constructor: all components supplied via `node_config<Types>`.
    explicit node(node_config<Types> cfg);

    /// @brief Legacy positional constructor retained for migration compatibility.
    ///
    /// Prefer `node(node_config<Types>)` for new code.
    node(node_id_type node_id, network_client_type network_client,
         network_server_type network_server, persistence_engine_type persistence,
         logger_type logger, metrics_type metrics, membership_manager_type membership,
         configuration_type config = configuration_type{},
         address_type self_address = address_type{},
         peer_discovery_type peer_discovery = peer_discovery_type{},
         peer2peer_replicator_type peer2peer_replicator = peer2peer_replicator_type{});
    /// @}

    /// @name Client operations
    /// @{

    /// @brief Submit a serialised command to the cluster for replication.
    ///
    /// The returned Future resolves with the state-machine result bytes once the
    /// entry is committed and applied.  Rejects with `std::runtime_error` if this
    /// node is not the leader, the timeout elapses, or the cluster loses quorum.
    ///
    /// @param command Serialised application command.
    /// @param timeout Maximum time to wait for the command to commit.
    /// @return Future that resolves with the state-machine result.
    auto submit_command(const std::vector<std::byte>& command, std::chrono::milliseconds timeout)
        -> future_type;

    /// @brief Submit a command with client-session deduplication.
    ///
    /// If the leader has already applied a command from `(client_id, serial_number)`,
    /// it returns the cached result without re-applying the command.  Prevents
    /// double-execution on client retry after a leader failover.
    ///
    /// @param client_id      Stable identifier assigned to the client session.
    /// @param serial_number  Monotonically increasing sequence number within the session.
    /// @param command        Serialised application command.
    /// @param timeout        Maximum time to wait for the command to commit.
    /// @return Future that resolves with the state-machine result.
    auto submit_command_with_session(client_id_t client_id, serial_number_t serial_number,
                                     const std::vector<std::byte>& command,
                                     std::chrono::milliseconds timeout) -> future_type;

    /// @brief Execute a linearisable read without appending to the log.
    ///
    /// The leader confirms its leadership via a quorum of heartbeat acknowledgements
    /// before applying the read to the state machine, satisfying linearisability.
    ///
    /// @param timeout Maximum time to wait for the read to complete.
    /// @return Future that resolves with the current state-machine state bytes.
    auto read_state(std::chrono::milliseconds timeout) -> future_type;
    /// @}

    /// @name Lifecycle
    /// @{

    /// @brief Start the election-timeout loop and register RPC handlers.
    ///
    /// Restores persistent state, runs the bootstrap protocol if a peer-discovery
    /// back-end is configured, and then sets `_running = true`.
    auto start() -> void;

    /// @brief Gracefully stop the node and join all background threads.
    auto stop() -> void;

    /// @brief Returns `true` while the node is between `start()` and `stop()`.
    [[nodiscard]] auto is_running() const noexcept -> bool;
    /// @}

    /// @name State queries
    /// @{

    /// @brief Returns this node's unique identifier.
    [[nodiscard]] auto get_node_id() const -> node_id_type;

    /// @brief Returns the current Raft term number.
    [[nodiscard]] auto get_current_term() const -> term_id_type;

    /// @brief Returns the current consensus role (follower / candidate / leader).
    [[nodiscard]] auto get_state() const -> kythira::server_state;

    /// @brief Returns `true` if this node currently believes it is the leader.
    [[nodiscard]] auto is_leader() const -> bool;

    /// @brief Returns the node id of the most recently observed leader, if any.
    ///
    /// Populated from the `leader_id` of the last accepted AppendEntries RPC;
    /// `std::nullopt` before this node has heard from any leader (e.g. during
    /// the very first election). Used by client-facing layers built on top of
    /// `node<Types>` (e.g. `ca_cluster_node`) to redirect non-leader requests.
    [[nodiscard]] auto known_leader() const -> std::optional<node_id_type>;

    /// @brief Read-only snapshot of internal Raft state for test assertions.
    ///
    /// Valid only while the node is running and not being concurrently mutated.
    struct debug_snapshot {
        term_id_type current_term;            ///< Current term number.
        log_index_type commit_index;          ///< Highest committed log index.
        log_index_type last_applied;          ///< Highest applied log index.
        bool is_leader;                       ///< Whether this node is the leader.
        std::span<const log_entry_type> log;  ///< View of the in-memory log.
    };

    /// @brief Returns an immutable snapshot of internal state.
    [[nodiscard]] auto debug_state() const noexcept -> debug_snapshot;
    /// @}

    /// @name Cluster membership
    /// @{

    /// @brief Add a new server to the cluster via a joint-consensus configuration change.
    /// @param new_node Identifier of the server to add.
    /// @return Future that resolves once the configuration change is committed.
    auto add_server(node_id_type new_node) -> future_type;

    /// @brief Remove an existing server from the cluster.
    /// @param old_node Identifier of the server to remove.
    /// @return Future that resolves once the configuration change is committed.
    auto remove_server(node_id_type old_node) -> future_type;

    /// @brief Add a node to the cluster as a learner (non-voting member).
    ///
    /// A learner replicates the log and applies committed entries exactly like a
    /// follower, but never votes, never becomes a candidate, and is never counted
    /// toward quorum or election majority. Unlike `add_server()`, this is a plain
    /// (non-joint) configuration change — it never affects the voting set, so it
    /// commits under the current voting quorum with no joint-consensus phase.
    ///
    /// Rejected if the candidate's placement group has no remaining capacity
    /// relative to `_quorum_manager.topology()` (see
    /// `.kiro/specs/non-voting-nodes/`), with `learner_capacity_exceeded_exception`.
    ///
    /// @param new_node Identifier of the node to admit as a learner.
    /// @return Future that resolves once the configuration change is committed.
    auto add_learner(node_id_type new_node) -> future_type;

    /// @brief Remove a learner from the cluster.
    ///
    /// Symmetric to `add_learner()`: a plain (non-joint) configuration change,
    /// since removing a learner never affects the voting set or quorum.
    ///
    /// @param learner Identifier of the learner to remove.
    /// @return Future that resolves once the configuration change is committed.
    auto remove_learner(node_id_type learner) -> future_type;

    /// @brief Promote an existing learner to full voting membership.
    ///
    /// Reuses the joint-consensus protocol (C_old -> C_old+new -> C_new), exactly
    /// as `add_server()` does, since promotion changes the voting set. The
    /// learner's existing `_next_index`/`_match_index` entries (already populated
    /// from its time as a learner) are left untouched rather than reinitialized.
    ///
    /// Rejected if the learner's placement group has no remaining voting capacity
    /// relative to `_quorum_manager.topology()`, with
    /// `voting_capacity_exceeded_exception`. A learner blocked this way is left
    /// unaffected — it remains an ordinary learner and becomes a promotion
    /// candidate again the moment its group has room, with no bookkeeping
    /// required (see `.kiro/specs/non-voting-nodes/`).
    ///
    /// @param learner Identifier of the learner to promote.
    /// @return Future that resolves once the configuration change is committed.
    auto promote_to_voter(node_id_type learner) -> future_type;

    /// @brief Send a ClusterLeave RPC to the current leader asking it to remove this node.
    ///
    /// No-op when the network client does not satisfy the `network_client_with_cluster_leave`
    /// concept (i.e., peer-discovery-less deployments).
    ///
    /// @param timeout Maximum time to wait for the leader to acknowledge the leave.
    auto leave_cluster(std::chrono::milliseconds timeout) -> void;
    /// @}

    /// @name Periodic callbacks
    /// @{

    /// @brief Drive the election-timeout state machine; call from an external timer loop.
    auto check_election_timeout() -> void;

    /// @brief Drive the heartbeat loop; call from an external timer loop (leaders only).
    auto check_heartbeat_timeout() -> void;

    /// @brief Immediately replicate pending log entries to all followers.
    ///
    /// Useful in tests and immediately after `submit_command` to bypass the
    /// normal heartbeat interval.
    auto replicate_to_followers() -> void;
    /// @}

    /// @name Configuration helpers
    /// @{

    /// @brief Overwrite the current cluster configuration (for bootstrap and testing).
    /// @param node_ids Complete list of node IDs in the new configuration.
    auto set_cluster_configuration(const std::vector<node_id_type>& node_ids) -> void;

    /// @brief Returns the number of nodes in the current cluster configuration.
    [[nodiscard]] auto get_cluster_size() const -> std::size_t;

    /// @brief Register or update the placement group for a node.
    ///
    /// May be called at any time before or after `start()`.  Entries are
    /// maintained automatically as nodes join and leave via the provisioning path.
    ///
    /// @param id    Node identifier.
    /// @param group Placement-group identifier (e.g., an availability-zone name).
    auto set_placement(node_id_type id, placement_group_id_type group) -> void;
    /// @}

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

    // Helper to convert node_id to uint64_t for RPC target routing and
    // fetch_log_entries_response's fixed-width responder_id field — every
    // existing RPC send already routes by std::uint64_t target regardless of
    // node_id_type (see network_client's send_* signatures).
    static auto node_id_to_u64(const node_id_type& id) -> std::uint64_t {
        if constexpr (std::is_same_v<node_id_type, std::string>) {
            return 0;
        } else {
            return static_cast<std::uint64_t>(id);
        }
    }

    // Initialize the node from persistent storage
    auto initialize_from_storage() -> void;

    // Register RPC handlers with the network server
    auto register_rpc_handlers() -> void;

    // ========================================================================
    // Bootstrap helpers
    // ========================================================================

    // Returns true iff this is a fresh node (no persisted state at all)
    [[nodiscard]] auto is_fresh_node() -> bool;

    // Handle an incoming ClusterJoin RPC (leader accepts, follower redirects)
    [[nodiscard]] auto handle_cluster_join(const cluster_join_request_type& req)
        -> cluster_join_response_type;

    // Handle an incoming ClusterLeave RPC (leader accepts, follower redirects)
    [[nodiscard]] auto handle_cluster_leave(const cluster_leave_request_type& req)
        -> cluster_leave_response_type;

    // Bootstrap entry point: runs synchronously inside start() before _running=true.
    // With no_op_peer_discovery (empty peers list) this is a no-op that immediately
    // returns, preserving existing single-node-founding behaviour.
    auto run_bootstrap() -> void;

    // Reconnection loop for restarting nodes: runs as a background thread.
    // Exits immediately when the first AppendEntries/RequestVote arrives, or on stop().
    auto run_reconnect() -> void;

    // Update the network-client routing table from a list of newly discovered peers.
    auto update_peer_addresses(const std::vector<peer_info<node_id_type, address_type>>& peers)
        -> void;

    // Bootstrap configuration accessors (fall back to hard-coded defaults when
    // raft_configuration doesn't expose these fields, preserving backwards compat)
    [[nodiscard]] auto bootstrap_retry_interval() const -> std::chrono::milliseconds;
    [[nodiscard]] auto bootstrap_peer_find_timeout() const -> std::chrono::milliseconds;

    // ========================================================================
    // Persistent state (stored before responding to RPCs)
    // ========================================================================

    // Latest term server has seen (initialized to 0 on first boot, increases monotonically)
    term_id_type _current_term;

    // CandidateId that received vote in current term (or null if none)
    std::optional<node_id_type> _voted_for;

    // Log entries; each entry contains command for state machine, and term when entry was received
    // by leader First index is 1
    std::vector<log_entry_type> _log;

    // ========================================================================
    // Volatile state (all servers)
    // ========================================================================

    // Index of highest log entry known to be committed (initialized to 0, increases monotonically)
    log_index_type _commit_index;

    // Index of highest log entry applied to state machine (initialized to 0, increases
    // monotonically)
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

    // Configuration synchronizer for safe configuration changes (always uses Future<bool>
    // internally)
    using config_synchronizer_t = kythira::configuration_synchronizer<node_id_type, log_index_type>;
    config_synchronizer_t _config_synchronizer;

    // ========================================================================
    // Error handling components using unified types
    // ========================================================================

    // Error handlers for different RPC operations using generic future types
    using append_entries_error_handler_t = error_handler<append_entries_response_type>;
    using request_vote_error_handler_t = error_handler<request_vote_response_type>;
    using request_pre_vote_error_handler_t = error_handler<request_pre_vote_response_type>;
    using install_snapshot_error_handler_t = error_handler<install_snapshot_response_type>;

    append_entries_error_handler_t _append_entries_error_handler;
    request_vote_error_handler_t _request_vote_error_handler;
    request_pre_vote_error_handler_t _request_pre_vote_error_handler;
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

    // Bootstrap configuration: the initial configuration set at node construction, used as the
    // fallback when log truncation removes all configuration entries from the in-memory log.
    cluster_configuration_type _boot_configuration;

    // Election timeout (randomized between min and max)
    std::chrono::milliseconds _election_timeout;

    // Heartbeat interval for leaders
    std::chrono::milliseconds _heartbeat_interval;

    // Last time a heartbeat was received (for followers) or sent (for leaders)
    std::chrono::steady_clock::time_point _last_heartbeat;

    // Last time this node received a valid AppendEntries from a genuine
    // leader — deliberately distinct from _last_heartbeat, which is also
    // reset by this node's OWN candidacy/leadership transitions
    // (become_candidate(), become_leader(), granting a vote) and would
    // otherwise make handle_request_pre_vote()'s "have I heard from a
    // leader recently" check (`.kiro/specs/raft-pre-vote/`) misfire against
    // an ordinary split vote between two mutually-reachable candidates.
    // Only handle_append_entries() updates this.
    std::chrono::steady_clock::time_point _last_leader_contact;

    // Random number generator for election timeout randomization
    std::mt19937 _rng;

    // ========================================================================
    // Client session tracking for duplicate detection
    // ========================================================================

    // Structure to track client session state
    struct client_session {
        serial_number_t last_serial_number{0};
        std::vector<std::byte> last_response;
    };

    // Map from client_id to session state
    std::unordered_map<client_id_t, client_session> _client_sessions;

    // ========================================================================
    // Bootstrap / peer-discovery members
    // ========================================================================

    // This node's contact address (advertised in ClusterJoinRequest and registered
    // with the peer_discovery back-end)
    address_type _self_address{};

    // Peer discovery implementation (default: no_op — founds a single-node cluster)
    peer_discovery_type _peer_discovery{};

    // Last known leader (populated from incoming AppendEntries)
    std::optional<node_id_type> _known_leader;

    // Whether stop() was requested before start() completed (cancels bootstrap loop)
    std::atomic<bool> _stop_requested{false};

    // Shared stop token captured by all async Folly callbacks.  Using shared_ptr means
    // the flag outlives the node object: when a stopped node goes out of scope and a new
    // node is created at the same stack address, any still-inflight callbacks from the
    // old node will lock the OLD shared_ptr (value=true) and exit immediately without
    // touching `this` — preventing both stack corruption and spurious mutex contention.
    std::shared_ptr<std::atomic<bool>> _stop_flag{std::make_shared<std::atomic<bool>>(false)};

    // Background thread for reconnection logic (restarting nodes only)
    std::thread _reconnect_thread;

    // Condition variable to wake the reconnect thread early (on peer contact or stop)
    std::condition_variable _reconnect_cv;
    std::mutex _reconnect_cv_mutex;

    // ========================================================================
    // Quorum management members (Req 12-15)
    // ========================================================================

    // Quorum manager implementation (default: no_op)
    quorum_manager_type _quorum_manager{};

    // Per-node placement group assignment.  Keys are node IDs in the current
    // cluster configuration.  Absent entries fall back to placement_group_id_type{}.
    // std::map is used because placement_group_id_type is only totally_ordered,
    // not necessarily hashable.
    std::map<node_id_type, placement_group_id_type> _placement_map{};

    // Per-peer consecutive heartbeat failure counter.  Reset to 0 on any
    // successful heartbeat ACK.  Triggers an immediate assess_quorum call when
    // it reaches _config.quorum_heartbeat_failure_threshold().
    std::unordered_map<node_id_type, std::size_t> _heartbeat_failure_counts{};

    // Number of in-flight or pending provision_node calls per placement group.
    // Prevents over-provisioning across consecutive assessment cycles.
    std::map<placement_group_id_type, std::size_t> _pending_provisions{};

    // Tracks which unreachable node ID each pending provision is replacing.
    // Used to call remove_server + decommission_node after the replacement joins.
    std::map<placement_group_id_type, std::optional<node_id_type>> _pending_replacing{};

    // When the quorum assessment loop was last run.
    // Reset to epoch by become_leader() to schedule an immediate first assessment.
    // The assessment loop fires from check_heartbeat_timeout() (Req 13).
    std::chrono::steady_clock::time_point _last_quorum_check{};

    // Set to true by become_leader(), false by become_follower/candidate.
    // Guards the quorum assessment branch inside check_heartbeat_timeout().
    bool _quorum_check_active{false};

    // Set to true when a peer has crossed the heartbeat failure threshold, asking
    // for an immediate out-of-cycle assess_quorum in the next heartbeat tick.
    bool _quorum_immediate_check{false};

    // ========================================================================
    // Peer-to-peer catch-up members (.kiro/specs/peer2peer-log-replication/)
    // ========================================================================

    // Peer-to-peer replicator implementation (default: no_op — preserves today's
    // leader-only replication behavior byte-for-byte).
    peer2peer_replicator_type _peer2peer_replicator{};

    // When this node last called advertise_progress(); gates maybe_gossip_progress()
    // to _config.progress_gossip_interval() cadence (Requirement 3.1).
    std::chrono::steady_clock::time_point _last_progress_gossip{};

    // At most one outstanding peer-to-peer fetch at a time (Requirement 4.5).
    bool _catch_up_in_flight{false};

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
    [[nodiscard]] auto handle_request_vote(const request_vote_request_type& request)
        -> request_vote_response_type;
    // Optional (`.kiro/specs/raft-pre-vote/`) — only instantiated (and so
    // only needs network_client_type/network_server_type to support it) for
    // Types bundles that actually call it; see start_pre_vote() and
    // register_rpc_handlers()'s if constexpr guards.
    [[nodiscard]] auto handle_request_pre_vote(const request_pre_vote_request_type& request)
        -> request_pre_vote_response_type;
    [[nodiscard]] auto handle_append_entries(const append_entries_request_type& request)
        -> append_entries_response_type;
    [[nodiscard]] auto handle_install_snapshot(const install_snapshot_request_type& request)
        -> install_snapshot_response_type;

    // Election and timing
    auto randomize_election_timeout() -> void;
    auto reset_election_timer() -> void;
    [[nodiscard]] auto election_timeout_elapsed() const -> bool;

    // Heartbeat timing
    [[nodiscard]] auto heartbeat_timeout_elapsed() const -> bool;
    auto send_heartbeats() -> void;

    // State transitions
    auto become_follower(term_id_type new_term) -> void;
    auto become_candidate() -> void;
    auto start_election() -> void;
    // Optional (`.kiro/specs/raft-pre-vote/`): called from
    // check_election_timeout() instead of become_candidate()+start_election()
    // when network_client_type supports PreVote. Broadcasts a trial round at
    // term()+1 without mutating any of this node's own state; only escalates
    // to a real become_candidate()+start_election() if a majority would
    // actually grant the vote. Falls straight through to the existing
    // become_candidate()+start_election() behavior, unchanged, for any
    // Types bundle whose transport doesn't implement PreVote.
    auto start_pre_vote() -> void;
    auto become_leader() -> void;

    // Log operations
    auto append_log_entry(const log_entry_type& entry) -> void;
    [[nodiscard]] auto get_last_log_index() const -> log_index_type;
    [[nodiscard]] auto get_last_log_term() const -> term_id_type;
    [[nodiscard]] auto get_log_entry(log_index_type index) const -> std::optional<log_entry_type>;

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

    // ── Peer-to-peer catch-up helpers (.kiro/specs/peer2peer-log-replication/) ──

    // Rules 3-5 of handle_append_entries(): prevLogIndex/prevLogTerm consistency
    // check, conflict-truncation, and append — extracted so the peer-to-peer
    // fetch path (maybe_catch_up_from_peer()) gets the exact same Log Matching
    // Property guarantees as leader-driven replication, with no separate safety
    // argument needed. Does NOT touch _commit_index, _known_leader, or the
    // election timer — those remain exclusively handle_append_entries()'s own
    // concern. Must be called with _mutex held.
    [[nodiscard]] auto append_entries_with_consistency_check(
        log_index_type prev_log_index, term_id_type prev_log_term,
        const std::vector<log_entry_type>& entries) -> append_entries_response_type;

    // Core cluster membership: _configuration.nodes(), unioned with old_nodes()
    // during joint consensus. Excludes learners() (Requirement 11.2).
    [[nodiscard]] auto cluster_members() const -> std::vector<node_id_type>;

    // Fire-and-forget push of cluster_members() into _peer2peer_replicator.
    // Called immediately after every _configuration assignment (Requirement 11.1).
    // Must be called with _mutex held (or before start(), single-threaded).
    auto sync_peer2peer_membership() -> void;

    // Responder side of fetch_log_entries (Requirement 5.4/5.5). Only reachable
    // if network_server_with_log_fetch<network_server_type>.
    [[nodiscard]] auto handle_fetch_log_entries(const fetch_log_entries_request_type& request)
        -> fetch_log_entries_response_type;

    // Advertises this node's own progress digest, gated by
    // _config.progress_gossip_interval() (Requirement 3).
    auto maybe_gossip_progress() -> void;

    // Detects the catch-up gap and, if it exceeds _config.catch_up_gap_threshold(),
    // fetches missing entries from a peer (Requirement 4-7).
    auto maybe_catch_up_from_peer() -> void;

    // ── Quorum management helpers (Req 12-15) ────────────────────────────────

    // Build the cluster vector passed to assess_quorum from the current
    // configuration, annotating each node with its known placement group.
    // Must be called with _mutex held.
    [[nodiscard]] auto build_quorum_cluster_vector() const
        -> std::vector<node_placement<node_id_type, placement_group_id_type>>;

    // Run a single assess_quorum cycle and act on the result.
    // Called from check_heartbeat_timeout() when the quorum check is due
    // or when an immediate check was requested.
    // Must NOT be called with _mutex held (calls .get() on futures).
    auto run_quorum_assessment() -> void;

    // Called by become_leader() — initialises quorum loop state.
    auto start_quorum_loop() -> void;

    // Called by become_follower/candidate — disables the quorum check.
    auto stop_quorum_loop() -> void;

    // ── Learner helpers (.kiro/specs/non-voting-nodes/) ──────────────────────

    // Returns true iff _node_id is currently a learner (non-voting member).
    // A live read of _configuration — never cached — so a node's election
    // eligibility updates immediately when it is promoted, added, or removed.
    [[nodiscard]] auto is_learner() const -> bool;

    // Returns this node's own placement group, defaulting to placement_group_id_type{}
    // when absent from _placement_map (same fallback used by build_quorum_cluster_vector()).
    [[nodiscard]] auto placement_of(const node_id_type& id) const -> placement_group_id_type;

    // Number of voting nodes (_configuration.nodes()) whose placement is `group`.
    [[nodiscard]] auto voting_count_in_group(placement_group_id_type group) const -> std::size_t;

    // Number of learners (_configuration.learners()) whose placement is `group`.
    [[nodiscard]] auto learner_count_in_group(placement_group_id_type group) const -> std::size_t;

    // voting_count_in_group(group) plus learner_count_in_group(group).
    [[nodiscard]] auto voting_and_learner_count_in_group(placement_group_id_type group) const
        -> std::size_t;

    // Looks up `group`'s declared target in _quorum_manager.topology(); nullopt
    // if the group has no declared target at all.
    [[nodiscard]] auto find_group_target(placement_group_id_type group) const
        -> std::optional<kythira::placement_group_target<placement_group_id_type>>;

    // Admission capacity check: does `group` have room for one more learner?
    // Fails closed (false) when the group has no declared target. When the
    // group's target declares an independent learner_capacity, compares the
    // learner-only count against it; otherwise falls back to comparing the
    // combined voting+learner count against target_count (original,
    // backward-compatible behavior — see .kiro/specs/non-voting-nodes/).
    [[nodiscard]] auto group_has_admission_capacity(placement_group_id_type group) const -> bool;

    // Promotion capacity check: does `group` have room for one more voter?
    // Always compares the voting-only count against target_count, regardless
    // of any learner_capacity the group declares — promotion never affects,
    // and is never affected by, the separate learner-capacity ceiling.
    [[nodiscard]] auto group_has_promotion_capacity(placement_group_id_type group) const -> bool;
};

// Raft node concept - defines the interface for a Raft node using unified types
template<typename N>
concept raft_node = requires(N node, const std::vector<std::byte>& command,
                             std::chrono::milliseconds timeout, typename N::node_id_type node_id) {
    // Type requirements
    typename N::types;
    typename N::future_type;
    typename N::node_id_type;

    // Client operations - return template future types
    { node.submit_command(command, timeout) } -> std::same_as<typename N::future_type>;
    { node.read_state(timeout) } -> std::same_as<typename N::future_type>;

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

// node_config constructor — the primary constructor (Req 17 AC 2)
template<raft_types Types>
node<Types>::node(node_config<Types> cfg)
    : _current_term{0},
      _voted_for{std::nullopt},
      _log{},
      _commit_index{0},
      _last_applied{0},
      _state{kythira::server_state::follower},
      _next_index{},
      _match_index{},
      _network_client{std::move(cfg.network_client)},
      _network_server{std::move(cfg.network_server)},
      _persistence{std::move(cfg.persistence)},
      _logger{std::move(cfg.logger)},
      _metrics{std::move(cfg.metrics)},
      _membership{std::move(cfg.membership)},
      _state_machine{std::move(cfg.state_machine)},
      _config{cfg.config},
      _node_id{cfg.node_id},
      _configuration{},
      _election_timeout{cfg.config.election_timeout_min()},
      _heartbeat_interval{cfg.config.heartbeat_interval()},
      _last_heartbeat{std::chrono::steady_clock::now()},
      _last_leader_contact{std::chrono::steady_clock::now()},
      _rng{std::random_device{}()},
      _self_address{std::move(cfg.self_address)},
      _peer_discovery{std::move(cfg.peer_discovery)},
      _quorum_manager{std::move(cfg.quorum_manager)},
      _peer2peer_replicator{std::move(cfg.peer2peer_replicator)} {
    _placement_map.insert(cfg.initial_placement.begin(), cfg.initial_placement.end());
    _configuration._nodes = {_node_id};
    _configuration._is_joint_consensus = false;
    _configuration._old_nodes = std::nullopt;

    randomize_election_timeout();

    _logger.info("Raft node created",
                 {{"node_id", node_id_to_string(_node_id)}, {"state", "follower"}});
}

// Legacy positional constructor — delegates to node_config constructor (Req 17 AC 3)
template<raft_types Types>
node<Types>::node(node_id_type node_id, network_client_type network_client,
                  network_server_type network_server, persistence_engine_type persistence,
                  logger_type logger, metrics_type metrics, membership_manager_type membership,
                  configuration_type config, address_type self_address,
                  peer_discovery_type peer_discovery,
                  peer2peer_replicator_type peer2peer_replicator)
    : node(node_config<Types>{
          .node_id = std::move(node_id),
          .network_client = std::move(network_client),
          .network_server = std::move(network_server),
          .persistence = std::move(persistence),
          .logger = std::move(logger),
          .metrics = std::move(metrics),
          .membership = std::move(membership),
          .config = std::move(config),
          .self_address = std::move(self_address),
          .peer_discovery = std::move(peer_discovery),
          .peer2peer_replicator = std::move(peer2peer_replicator),
      }) {}

template<raft_types Types>

auto node<Types>::set_cluster_configuration(const std::vector<node_id_type>& node_ids) -> void {
    std::lock_guard<std::mutex> lock(_mutex);

    _configuration._nodes = node_ids;
    _configuration._is_joint_consensus = false;
    _configuration._old_nodes = std::nullopt;
    _boot_configuration = _configuration;
    sync_peer2peer_membership();

    _logger.info("Cluster configuration updated", {{"node_id", node_id_to_string(_node_id)},
                                                   {"num_nodes", std::to_string(node_ids.size())}});
}

template<raft_types Types> auto node<Types>::get_cluster_size() const -> std::size_t {
    std::lock_guard<std::mutex> lock(_mutex);
    return _configuration._nodes.size();
}

template<raft_types Types>
auto node<Types>::set_placement(node_id_type id, placement_group_id_type group) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _placement_map[std::move(id)] = std::move(group);
}

// ── Quorum management (Req 12-15) ────────────────────────────────────────────

template<raft_types Types>
auto node<Types>::build_quorum_cluster_vector() const
    -> std::vector<node_placement<node_id_type, placement_group_id_type>> {
    std::vector<node_placement<node_id_type, placement_group_id_type>> cluster;
    for (const auto& nid : _configuration.nodes()) {
        placement_group_id_type grp{};
        auto it = _placement_map.find(nid);
        if (it != _placement_map.end()) {
            grp = it->second;
        }
        cluster.push_back({.node_id = nid, .group_id = grp});
    }
    return cluster;
}

// ── Learner placement-capacity helpers (.kiro/specs/non-voting-nodes/) ──────

template<raft_types Types> auto node<Types>::is_learner() const -> bool {
    const auto& learners = _configuration.learners();
    return std::find(learners.begin(), learners.end(), _node_id) != learners.end();
}

template<raft_types Types>
auto node<Types>::placement_of(const node_id_type& id) const -> placement_group_id_type {
    auto it = _placement_map.find(id);
    return it != _placement_map.end() ? it->second : placement_group_id_type{};
}

template<raft_types Types>
auto node<Types>::voting_count_in_group(placement_group_id_type group) const -> std::size_t {
    return static_cast<std::size_t>(
        std::count_if(_configuration.nodes().begin(), _configuration.nodes().end(),
                      [&](const node_id_type& id) { return placement_of(id) == group; }));
}

template<raft_types Types>
auto node<Types>::learner_count_in_group(placement_group_id_type group) const -> std::size_t {
    return static_cast<std::size_t>(
        std::count_if(_configuration.learners().begin(), _configuration.learners().end(),
                      [&](const node_id_type& id) { return placement_of(id) == group; }));
}

template<raft_types Types>
auto node<Types>::voting_and_learner_count_in_group(placement_group_id_type group) const
    -> std::size_t {
    return voting_count_in_group(group) + learner_count_in_group(group);
}

template<raft_types Types>
auto node<Types>::find_group_target(placement_group_id_type group) const
    -> std::optional<kythira::placement_group_target<placement_group_id_type>> {
    auto desired = _quorum_manager.topology();
    auto it = std::find_if(desired.groups.begin(), desired.groups.end(),
                           [&](const auto& g) { return g.group_id == group; });
    if (it == desired.groups.end()) {
        return std::nullopt;
    }
    return *it;
}

template<raft_types Types>
auto node<Types>::group_has_admission_capacity(placement_group_id_type group) const -> bool {
    auto target = find_group_target(group);
    if (!target) {
        // Fail closed: no declared target for this group means no capacity.
        return false;
    }
    if (target->learner_capacity) {
        return learner_count_in_group(group) < *target->learner_capacity;
    }
    // No independent learner_capacity declared: learners and voters share
    // target_count (original, backward-compatible behavior).
    return voting_and_learner_count_in_group(group) < target->target_count;
}

template<raft_types Types>
auto node<Types>::group_has_promotion_capacity(placement_group_id_type group) const -> bool {
    auto target = find_group_target(group);
    if (!target) {
        return false;
    }
    // Always target_count, regardless of learner_capacity — promotion never
    // affects, and is never affected by, the learner-capacity ceiling.
    return voting_count_in_group(group) < target->target_count;
}

template<raft_types Types> auto node<Types>::run_quorum_assessment() -> void {
    // Build cluster vector under lock, then release for async call
    std::vector<node_placement<node_id_type, placement_group_id_type>> cluster;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state != kythira::server_state::leader) {
            {
                return;
            }
        }
        cluster = build_quorum_cluster_vector();
    }

    quorum_health<node_id_type, placement_group_id_type> health;
    try {
        health = _quorum_manager.assess_quorum(cluster).get();
    } catch (const std::exception& ex) {
        _logger.error("assess_quorum failed", {{"error", ex.what()}});
        return;
    }

    // Update last-check timestamp
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _last_quorum_check = std::chrono::steady_clock::now();
        if (_state != kythira::server_state::leader) {
            {
                return;
            }
        }
    }

    // Req 14.2 — never provision on quorum loss
    if (health.status == quorum_status::lost) {
        _logger.warning("Quorum lost — no autonomous provisioning attempted", {});
        return;
    }

    // Req 14.1 — provision replacements for groups below target
    if (health.status == quorum_status::degraded || health.status == quorum_status::critical) {
        auto desired = _quorum_manager.topology();

        for (const auto& grp_health : health.groups) {
            // How many slots are missing in this group?
            std::size_t target = 0;
            for (const auto& gt : desired.groups) {
                if (gt.group_id == grp_health.group_id) {
                    target = gt.target_count;
                    break;
                }
            }
            std::size_t live = grp_health.live_count;
            if (live >= target) {
                {
                    continue;
                }
            }

            std::size_t deficit = target - live;

            // Subtract already-in-flight provisions (Req 14.3)
            std::size_t pending = 0;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                auto pit = _pending_provisions.find(grp_health.group_id);
                if (pit != _pending_provisions.end()) {
                    pending = pit->second;
                }
            }
            if (pending >= deficit) {
                {
                    continue;
                }
            }
            std::size_t to_provision = deficit - pending;

            // Learner placement-capacity policy (.kiro/specs/non-voting-nodes/,
            // Requirement 4): prefer promoting an existing, eligible learner in this
            // group over provisioning brand-new infrastructure, for as many deficit
            // slots as have both an eligible learner and remaining voting capacity.
            // Promotion is fire-and-forget, exactly like add_learner() triggered from
            // handle_cluster_join() — its log entry commits via the ordinary
            // heartbeat-driven replication path, not synchronously here. A learner
            // the capacity criterion currently blocks is left completely untouched
            // and is reconsidered fresh on the next assessment cycle; no bookkeeping
            // is needed (Requirement 5).
            std::size_t remaining_to_provision = to_provision;
            while (remaining_to_provision > 0) {
                std::optional<node_id_type> candidate;
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    if (group_has_promotion_capacity(grp_health.group_id)) {
                        for (const auto& learner_id : _configuration.learners()) {
                            if (placement_of(learner_id) == grp_health.group_id) {
                                candidate = learner_id;
                                break;
                            }
                        }
                    }
                }
                if (!candidate) {
                    break;  // no eligible learner (or no voting capacity) in this group
                }

                _logger.info("Promoting eligible learner instead of provisioning",
                             {{"node_id", node_id_to_string(_node_id)},
                              {"learner", node_id_to_string(*candidate)},
                              {"group", grp_health.group_id}});
                auto promote_fut = promote_to_voter(*candidate);
                (void)promote_fut;

                --remaining_to_provision;
            }
            to_provision = remaining_to_provision;
            if (to_provision == 0) {
                continue;  // deficit fully covered by promotion(s) this cycle
            }

            // Pick a node to replace from the group's unreachable list
            std::size_t replacing_idx = 0;
            for (std::size_t slot = 0; slot < to_provision; ++slot) {
                std::optional<node_id_type> replacing = std::nullopt;
                if (replacing_idx < grp_health.unreachable_nodes.size()) {
                    replacing = grp_health.unreachable_nodes[replacing_idx++];
                }

                // Mark slot as in-flight
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    _pending_provisions[grp_health.group_id]++;
                    _pending_replacing[grp_health.group_id] = replacing;
                }

                auto provision_fut = _quorum_manager.provision_node(grp_health.group_id, replacing);
                try {
                    auto info = std::move(provision_fut).get();
                    _logger.info("Provisioned replacement node",
                                 {
                                     {"new_node_id", node_id_to_string(info.node_id)},
                                     {"group", grp_health.group_id},
                                 });
                    // The provisioned node joins via ClusterJoin → add_learner(), then
                    // becomes a promotion candidate on a future assessment cycle once
                    // it has caught up. (Req 14.4 — nothing further to do here)
                } catch (const std::exception& ex) {
                    _logger.error("provision_node failed", {
                                                               {"group", grp_health.group_id},
                                                               {"error", ex.what()},
                                                           });
                    // Req 14.6 — clear the pending slot on failure
                    std::lock_guard<std::mutex> lock(_mutex);
                    auto& cnt = _pending_provisions[grp_health.group_id];
                    if (cnt > 0) {
                        {
                            --cnt;
                        }
                    }
                }
            }
        }
    }
}

// start_quorum_loop — called from become_leader() while _mutex is held.
// Initialises quorum loop state so that check_heartbeat_timeout() begins
// scheduling periodic assessments.
template<raft_types Types> auto node<Types>::start_quorum_loop() -> void {
    _quorum_check_active = true;
    _quorum_immediate_check = false;
    // Set to epoch so the first check_heartbeat_timeout() fires immediately
    _last_quorum_check = std::chrono::steady_clock::time_point{};
}

// stop_quorum_loop — called from become_follower/candidate while _mutex is held.
template<raft_types Types> auto node<Types>::stop_quorum_loop() -> void {
    _quorum_check_active = false;
    _quorum_immediate_check = false;
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

template<raft_types Types> auto node<Types>::known_leader() const -> std::optional<node_id_type> {
    std::lock_guard<std::mutex> lock(_mutex);
    return _known_leader;
}

template<raft_types Types>

auto node<Types>::debug_state() const noexcept -> debug_snapshot {
    std::lock_guard<std::mutex> lock(_mutex);
    return debug_snapshot{
        .current_term = _current_term,
        .commit_index = _commit_index,
        .last_applied = _last_applied,
        .is_leader = (_state == kythira::server_state::leader),
        .log = std::span<const log_entry_type>(_log),
    };
}

template<raft_types Types>

auto node<Types>::is_running() const noexcept -> bool {
    return _running.load(std::memory_order_acquire);
}

template<raft_types Types>

auto node<Types>::randomize_election_timeout() -> void {
    // Randomize election timeout between min and max to prevent split votes
    std::uniform_int_distribution<std::chrono::milliseconds::rep> dist(
        _config.election_timeout_min().count(), _config.election_timeout_max().count());
    _election_timeout = std::chrono::milliseconds{dist(_rng)};
}

// Stub implementations for methods to be implemented in later tasks
template<raft_types Types>
auto node<Types>::submit_command(const std::vector<std::byte>& command,
                                 std::chrono::milliseconds timeout) -> future_type {
    std::unique_lock<std::mutex> lock(_mutex);

    // Only leaders can accept client commands
    if (_state != kythira::server_state::leader) {
        _logger.debug(
            "Rejected command submission: not leader",
            {{"node_id", node_id_to_string(_node_id)},
             {"state", _state == kythira::server_state::follower ? "follower" : "candidate"}});

        // Emit command rejection metric
        _metrics.set_metric_name("command_rejected");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("reason", "not_leader");
        _metrics.add_one();
        _metrics.emit();

        // Return a failed future
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::make_exception_ptr(std::runtime_error("Not leader")));
        return future;
    }

    auto submission_start = std::chrono::steady_clock::now();

    _logger.info("Received client command", {{"node_id", node_id_to_string(_node_id)},
                                             {"term", std::to_string(_current_term)},
                                             {"command_size", std::to_string(command.size())}});

    // Emit command received metric
    _metrics.set_metric_name("command_received");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("term", std::to_string(_current_term));
    _metrics.add_one();
    _metrics.emit();

    // Step 1: Append command to leader's log
    log_entry_type entry{
        ._term = _current_term, ._index = get_last_log_index() + 1, ._command = command};

    try {
        _persistence.append_log_entry(entry);
        _log.push_back(entry);

        _logger.debug("Appended command to log", {{"node_id", node_id_to_string(_node_id)},
                                                  {"log_index", std::to_string(entry._index)},
                                                  {"term", std::to_string(entry._term)}});

        // Emit log append metric
        _metrics.set_metric_name("log_entry_appended");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("log_index", std::to_string(entry._index));
        _metrics.add_one();
        _metrics.emit();

    } catch (const std::exception& e) {
        _logger.error("Failed to append command to log",
                      {{"node_id", node_id_to_string(_node_id)}, {"error", e.what()}});

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
        [shared_promise, entry_index, node_id, &logger, &metrics,
         submission_start](std::vector<std::byte> result) mutable {
            auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - submission_start);

            logger.info("Command committed and applied",
                        {{"node_id", node_id_to_string(node_id)},
                         {"log_index", std::to_string(entry_index)},
                         {"latency_ms", std::to_string(latency.count())}});

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
        [shared_promise, entry_index, node_id, &logger, &metrics](std::exception_ptr ex) mutable {
            try {
                std::rethrow_exception(ex);
            } catch (const kythira::commit_timeout_exception<log_index_type>& e) {
                logger.warning("Command timed out waiting for commit",
                               {{"node_id", node_id_to_string(node_id)},
                                {"log_index", std::to_string(entry_index)},
                                {"timeout_ms", std::to_string(e.get_timeout().count())}});

                // Emit timeout metric
                metrics.set_metric_name("command_timeout");
                metrics.add_dimension("node_id", node_id_to_string(node_id));
                metrics.add_dimension("log_index", std::to_string(entry_index));
                metrics.add_one();
                metrics.emit();

            } catch (const kythira::leadership_lost_exception<term_id_type>& e) {
                logger.warning("Command cancelled due to leadership loss",
                               {{"node_id", node_id_to_string(node_id)},
                                {"log_index", std::to_string(entry_index)},
                                {"old_term", std::to_string(e.get_old_term())},
                                {"new_term", std::to_string(e.get_new_term())}});

                // Emit leadership loss metric
                metrics.set_metric_name("command_leadership_lost");
                metrics.add_dimension("node_id", node_id_to_string(node_id));
                metrics.add_dimension("log_index", std::to_string(entry_index));
                metrics.add_one();
                metrics.emit();

            } catch (const std::exception& e) {
                logger.warning("Command cancelled", {{"node_id", node_id_to_string(node_id)},
                                                     {"log_index", std::to_string(entry_index)},
                                                     {"error", e.what()}});

                // Emit cancellation metric
                metrics.set_metric_name("command_cancelled");
                metrics.add_dimension("node_id", node_id_to_string(node_id));
                metrics.add_dimension("log_index", std::to_string(entry_index));
                metrics.add_one();
                metrics.emit();
            }

            shared_promise->setException(ex);
        },
        timeout > std::chrono::milliseconds{0} ? std::optional{timeout} : std::nullopt);

    _logger.debug("Registered operation with CommitWaiter",
                  {{"node_id", node_id_to_string(_node_id)},
                   {"log_index", std::to_string(entry_index)},
                   {"timeout_ms", std::to_string(timeout.count())}});

    // Step 3: Trigger replication to followers
    // Release lock first — replicate_to_followers() acquires _mutex itself for each I/O phase
    lock.unlock();
    replicate_to_followers();

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
auto node<Types>::submit_command_with_session(client_id_t client_id, serial_number_t serial_number,
                                              const std::vector<std::byte>& command,
                                              std::chrono::milliseconds timeout) -> future_type {
    std::lock_guard<std::mutex> lock(_mutex);

    // Only leaders can accept client commands
    if (_state != kythira::server_state::leader) {
        _logger.debug("Rejected command with session: not leader",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"client_id", std::to_string(client_id)},
                       {"serial_number", std::to_string(serial_number)}});

        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::make_exception_ptr(std::runtime_error("Not leader")));
        return future;
    }

    // Check if this is a new client or existing client
    auto session_it = _client_sessions.find(client_id);

    if (session_it == _client_sessions.end()) {
        // New client - must start with serial number 1
        if (serial_number != 1) {
            _logger.warning("Rejected command: new client must start with serial number 1",
                            {{"node_id", node_id_to_string(_node_id)},
                             {"client_id", std::to_string(client_id)},
                             {"serial_number", std::to_string(serial_number)}});

            _metrics.set_metric_name("session_validation_failed");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("reason", "invalid_initial_serial");
            _metrics.add_one();
            _metrics.emit();

            promise_type promise;
            auto future = promise.getFuture();
            promise.setException(std::make_exception_ptr(
                std::invalid_argument("New client must start with serial number 1")));
            return future;
        }

        // Create new session for this client
        _client_sessions[client_id] = client_session{};

        _logger.debug("Created new client session", {{"node_id", node_id_to_string(_node_id)},
                                                     {"client_id", std::to_string(client_id)}});
    } else {
        // Existing client - validate serial number
        auto& session = session_it->second;

        if (serial_number <= session.last_serial_number) {
            // Duplicate or old request - return cached response
            _logger.debug("Returning cached response for duplicate/old request",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"client_id", std::to_string(client_id)},
                           {"serial_number", std::to_string(serial_number)},
                           {"last_serial_number", std::to_string(session.last_serial_number)}});

            _metrics.set_metric_name("duplicate_request_detected");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_one();
            _metrics.emit();

            promise_type promise;
            auto future = promise.getFuture();
            promise.setValue(session.last_response);
            return future;
        }
        if (serial_number > session.last_serial_number + 1) {
            // Skipped serial numbers - reject
            _logger.warning(
                "Rejected command: skipped serial numbers",
                {{"node_id", node_id_to_string(_node_id)},
                 {"client_id", std::to_string(client_id)},
                 {"serial_number", std::to_string(serial_number)},
                 {"expected_serial_number", std::to_string(session.last_serial_number + 1)}});

            _metrics.set_metric_name("session_validation_failed");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("reason", "skipped_serial_numbers");
            _metrics.add_one();
            _metrics.emit();

            promise_type promise;
            auto future = promise.getFuture();
            promise.setException(std::make_exception_ptr(std::invalid_argument(
                std::format("Serial numbers must be sequential. Expected {}, got {}",
                            session.last_serial_number + 1, serial_number))));
            return future;
        }

        // serial_number == session.last_serial_number + 1 - valid new request
    }

    // Valid request - submit command and cache response
    _logger.debug("Processing command with session",
                  {{"node_id", node_id_to_string(_node_id)},
                   {"client_id", std::to_string(client_id)},
                   {"serial_number", std::to_string(serial_number)}});

    // Release lock before calling submit_command (which will acquire it again)
    lock.~lock_guard();

    // Submit the command
    auto command_future = submit_command(command, timeout);

    // Create a continuation to cache the response
    auto shared_client_id = client_id;
    auto shared_serial_number = serial_number;
    auto& sessions_ref = _client_sessions;
    auto& mutex_ref = _mutex;
    auto& logger_ref = _logger;
    auto& metrics_ref = _metrics;
    auto node_id = _node_id;

    return command_future.thenValue([shared_client_id, shared_serial_number, &sessions_ref,
                                     &mutex_ref, &logger_ref, &metrics_ref,
                                     node_id](std::vector<std::byte> result) {
        // Cache the response
        std::lock_guard<std::mutex> cache_lock(mutex_ref);

        auto& session = sessions_ref[shared_client_id];
        session.last_serial_number = shared_serial_number;
        session.last_response = result;

        logger_ref.debug("Cached response for client session",
                         {{"node_id", node_id_to_string(node_id)},
                          {"client_id", std::to_string(shared_client_id)},
                          {"serial_number", std::to_string(shared_serial_number)}});

        metrics_ref.set_metric_name("session_response_cached");
        metrics_ref.add_dimension("node_id", node_id_to_string(node_id));
        metrics_ref.add_one();
        metrics_ref.emit();

        return result;
    });
}

template<raft_types Types>
auto node<Types>::read_state(std::chrono::milliseconds timeout) -> future_type {
    auto start_time = std::chrono::steady_clock::now();

    // Heartbeat parameters gathered under the lock; network I/O happens outside it.
    // Keeping network I/O inside the lock would self-deadlock when the Folly chain
    // resolves synchronously and the thenValue callback below tries to re-acquire _mutex.
    struct heartbeat_params {
        node_id_type follower_id;
        append_entries_request_type request;
    };
    std::vector<heartbeat_params> pending_heartbeats;
    term_id_type current_term{};
    node_id_type node_id{};

    {
        std::lock_guard<std::mutex> lock(_mutex);

        _logger.debug("Received read_state request",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"state", _state == kythira::server_state::leader
                                     ? "leader"
                                     : (_state == kythira::server_state::follower ? "follower"
                                                                                  : "candidate")},
                       {"timeout_ms", std::to_string(timeout.count())}});

        // Emit metrics for read request received
        _metrics.set_metric_name("raft_read_request_received");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_one();
        _metrics.emit();

        // Only leaders can serve linearizable reads
        if (_state != kythira::server_state::leader) {
            _logger.debug(
                "Rejected read request: not leader",
                {{"node_id", node_id_to_string(_node_id)},
                 {"state", _state == kythira::server_state::follower ? "follower" : "candidate"}});

            _metrics.set_metric_name("raft_read_request_rejected");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("reason", "not_leader");
            _metrics.add_one();
            _metrics.emit();

            promise_type promise;
            auto future = promise.getFuture();
            promise.setException(kythira::leadership_lost_exception(_current_term, _current_term));
            return future;
        }

        // For single-node cluster, return immediately (no need for heartbeat confirmation)
        if (_configuration.nodes().size() == 1) {
            _logger.debug("Single-node cluster, returning state immediately",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"commit_index", std::to_string(_commit_index)},
                           {"last_applied", std::to_string(_last_applied)}});

            try {
                auto state = _state_machine.get_state();

                auto end_time = std::chrono::steady_clock::now();
                auto duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

                _logger.debug("Read request completed (single-node)",
                              {{"node_id", node_id_to_string(_node_id)},
                               {"state_size", std::to_string(state.size())},
                               {"duration_ms", std::to_string(duration.count())}});

                _metrics.set_metric_name("raft_read_request_success");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_dimension("cluster_type", "single_node");
                _metrics.add_one();
                _metrics.emit();

                _metrics.set_metric_name("raft_read_latency");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_duration(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(duration));
                _metrics.emit();

                promise_type promise;
                auto future = promise.getFuture();
                promise.setValue(std::move(state));
                return future;

            } catch (const std::exception& e) {
                _logger.error("Failed to get state from state machine",
                              {{"node_id", node_id_to_string(_node_id)}, {"error", e.what()}});

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

        // Multi-node: build heartbeat request parameters under the lock so that
        // _next_index, _commit_index, _current_term are read consistently.
        _logger.debug("Multi-node cluster, sending heartbeats to verify leadership",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"follower_count", std::to_string(_configuration.nodes().size() - 1)},
                       {"timeout_ms", std::to_string(timeout.count())}});

        pending_heartbeats.reserve(_configuration.nodes().size() - 1);
        for (const auto& follower_id : _configuration.nodes()) {
            if (follower_id == _node_id) {
                {
                    continue;
                }
            }

            auto next_idx = _next_index[follower_id];
            log_index_type prev_log_index = next_idx - 1;
            term_id_type prev_log_term = term_id_type{0};

            if (prev_log_index > 0) {
                auto prev_entry = get_log_entry(prev_log_index);
                if (prev_entry.has_value()) {
                    prev_log_term = prev_entry->term();
                }
            }

            pending_heartbeats.push_back(
                {follower_id,
                 append_entries_request_type{_current_term, _node_id, prev_log_index, prev_log_term,
                                             std::vector<log_entry_type>{},  // heartbeat
                                             _commit_index}});
        }

        current_term = _current_term;
        node_id = _node_id;
    }  // _mutex released — network I/O and the async callback below run without it

    // Send heartbeats to all followers and collect responses
    std::vector<kythira::Future<append_entries_response_type>> heartbeat_futures;
    heartbeat_futures.reserve(pending_heartbeats.size());

    for (auto& [follower_id, request] : pending_heartbeats) {
        _logger.debug("Sending heartbeat for read verification",
                      {{"node_id", node_id_to_string(node_id)},
                       {"target", node_id_to_string(follower_id)},
                       {"term", std::to_string(current_term)}});

        heartbeat_futures.push_back(
            _network_client.send_append_entries(follower_id, request, timeout));
    }

    // NOTE: intentionally NOT kythira::raft_future_collector::collect_majority() here.
    // That helper computes majority as (futures.size() / 2) + 1, which is correct
    // for "strict majority of exactly these futures" — but heartbeat_futures excludes
    // the leader itself (see the `if (follower_id == _node_id) continue;` loop above),
    // so the true cluster majority only requires the leader's own implicit vote plus
    // floor(heartbeat_futures.size() / 2) more, not a majority of the followers alone.
    // Using collect_majority's formula here would require ALL followers to ack in a
    // 3-node cluster (majority-of-2-followers = 2), making read_state() unavailable
    // the moment a single follower is unreachable — defeating Raft's whole point of
    // tolerating N/2 failures. required_follower_acks below is the corrected value:
    // for a cluster of (heartbeat_futures.size() + 1) total nodes, true majority is
    // (N/2)+1 including the leader's own vote, so followers needed = (N/2)+1-1 = N/2,
    // i.e. (heartbeat_futures.size() + 1) / 2 with integer division.
    const std::size_t required_follower_acks = (heartbeat_futures.size() + 1) / 2;
    return kythira::raft_future_collector<append_entries_response_type>::collect_all_with_timeout(
               std::move(heartbeat_futures), timeout)
        .thenValue([this, current_term, node_id, start_time, required_follower_acks](
                       std::vector<kythira::Try<append_entries_response_type>> try_responses)
                       -> std::vector<append_entries_response_type> {
            std::vector<append_entries_response_type> responses;
            responses.reserve(try_responses.size());
            for (auto& try_response : try_responses) {
                if (try_response.hasValue()) {
                    responses.push_back(std::move(try_response.value()));
                }
            }
            if (responses.size() < required_follower_acks) {
                throw kythira::future_collection_exception("read_state_heartbeat_quorum",
                                                           try_responses.size() - responses.size());
            }
            return responses;
        })
        .thenValue(
            [this, current_term, node_id, start_time](
                std::vector<append_entries_response_type> responses) -> std::vector<std::byte> {
                std::lock_guard<std::mutex> lock(_mutex);

                _logger.debug("Received majority heartbeat responses",
                              {{"node_id", node_id_to_string(node_id)},
                               {"response_count", std::to_string(responses.size())}});

                // Check if we're still the leader and term hasn't changed
                if (_state != kythira::server_state::leader) {
                    _logger.warning(
                        "Lost leadership during read operation",
                        {{"node_id", node_id_to_string(node_id)},
                         {"current_state",
                          _state == kythira::server_state::follower ? "follower" : "candidate"}});

                    _metrics.set_metric_name("raft_read_request_aborted");
                    _metrics.add_dimension("node_id", node_id_to_string(node_id));
                    _metrics.add_dimension("reason", "leadership_lost");
                    _metrics.add_one();
                    _metrics.emit();

                    throw kythira::leadership_lost_exception(current_term, _current_term);
                }

                if (_current_term != current_term) {
                    _logger.warning("Term changed during read operation",
                                    {{"node_id", node_id_to_string(node_id)},
                                     {"old_term", std::to_string(current_term)},
                                     {"new_term", std::to_string(_current_term)}});

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
                        _logger.info("Discovered higher term in heartbeat response, stepping down",
                                     {{"node_id", node_id_to_string(node_id)},
                                      {"current_term", std::to_string(_current_term)},
                                      {"response_term", std::to_string(response.term())}});

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
                _logger.debug("Leadership confirmed, returning state",
                              {{"node_id", node_id_to_string(node_id)},
                               {"commit_index", std::to_string(_commit_index)},
                               {"last_applied", std::to_string(_last_applied)}});

                try {
                    auto state = _state_machine.get_state();

                    auto end_time = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        end_time - start_time);

                    _logger.info("Read request completed successfully",
                                 {{"node_id", node_id_to_string(node_id)},
                                  {"state_size", std::to_string(state.size())},
                                  {"duration_ms", std::to_string(duration.count())},
                                  {"heartbeat_responses", std::to_string(responses.size())}});

                    _metrics.set_metric_name("raft_read_request_success");
                    _metrics.add_dimension("node_id", node_id_to_string(node_id));
                    _metrics.add_dimension("cluster_type", "multi_node");
                    _metrics.add_one();
                    _metrics.emit();

                    _metrics.set_metric_name("raft_read_latency");
                    _metrics.add_dimension("node_id", node_id_to_string(node_id));
                    _metrics.add_duration(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(duration));
                    _metrics.emit();

                    return state;

                } catch (const std::exception& e) {
                    _logger.error(
                        "Failed to get state from state machine after leadership confirmation",
                        {{"node_id", node_id_to_string(node_id)}, {"error", e.what()}});

                    _metrics.set_metric_name("raft_read_request_failed");
                    _metrics.add_dimension("node_id", node_id_to_string(node_id));
                    _metrics.add_dimension("reason", "state_machine_error");
                    _metrics.add_one();
                    _metrics.emit();

                    throw;
                }
            })
        .thenError([this, node_id,
                    start_time](const folly::exception_wrapper& ew) -> std::vector<std::byte> {
            std::lock_guard<std::mutex> lock(_mutex);

            auto end_time = std::chrono::steady_clock::now();
            auto duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

            _logger.error("Read request failed",
                          {{"node_id", node_id_to_string(node_id)},
                           {"error", ew.what().toStdString()},
                           {"duration_ms", std::to_string(duration.count())}});

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
        _logger.warning("Attempted to start node that is already running",
                        {{"node_id", node_id_to_string(_node_id)}});
        return;
    }

    // Clear any stop signal left over from a previous stop() so that restart works.
    // Safe because stop() is synchronous — all threads using _stop_requested have
    // already exited by the time the caller invokes start() again.
    _stop_requested.store(false, std::memory_order_release);

    // Replace the shared stop flag with a fresh one so that async callbacks from the
    // previous run (which captured the old shared_ptr) continue to see the old flag
    // as true, while new callbacks from this run see false.
    _stop_flag = std::make_shared<std::atomic<bool>>(false);

    _logger.info("Starting Raft node", {{"node_id", node_id_to_string(_node_id)}});

    // Initialize from persistent storage (recover state after crash)
    initialize_from_storage();

    // Register this node with the peer-discovery back-end. With the default
    // no_op_peer_discovery this is a no-op that resolves immediately.
    _peer_discovery.register_node(_node_id, _self_address).get();

    if (is_fresh_node()) {
        // Fresh node: either join an existing cluster or found a new one.
        run_bootstrap();
    } else {
        // Restarting node: launch reconnect loop in the background so that if
        // configured peers are unreachable the node can re-discover them.
        // With no_op_peer_discovery the loop exits immediately on first RPC.
        _reconnect_thread = std::thread([this]() { run_reconnect(); });
    }

    // stop() may have been called while bootstrap was in progress
    if (_stop_requested.load(std::memory_order_acquire)) {
        return;
    }

    // Register RPC handlers with network server
    register_rpc_handlers();

    // Start any lifecycle-capable peer2peer_replicator (e.g.
    // tcp_gossip_peer2peer_replicator's background listener/gossip thread) now
    // that this object has reached its final address inside node<Types> —
    // never call this any earlier, since a node_config<Types>-constructed
    // instance may still be moved before this point. Detected structurally
    // (not via a named concept) so peer2peer_replicator_type implementations
    // with no such lifecycle (no_op_peer2peer_replicator,
    // static_peer2peer_replicator) are unaffected.
    if constexpr (requires { _peer2peer_replicator.start(); }) {
        _peer2peer_replicator.start();
    }

    // Start the network server
    _network_server.start();

    // Reset election timer
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _last_heartbeat = std::chrono::steady_clock::now();
    }

    // Mark as running
    _running.store(true, std::memory_order_release);

    _logger.info("Raft node started successfully",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"state", "follower"},
                  {"current_term", std::to_string(_current_term)}});
}

template<raft_types Types>

auto node<Types>::stop() -> void {
    // Signal bootstrap / reconnect loops first — this is safe even if start() is
    // still in progress (run_bootstrap / run_reconnect check _stop_requested).
    _stop_requested.store(true, std::memory_order_release);
    _stop_flag->store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(_reconnect_cv_mutex);
        _reconnect_cv.notify_all();
    }

    // Always join the reconnect thread here so that stop() is fully synchronous
    // even in the not-running early-return path below. Without this, a stop/start/stop
    // sequence leaves the thread joinable and the destructor calls std::terminate().
    if (_reconnect_thread.joinable()) {
        _reconnect_thread.join();
    }

    if (!_running.load(std::memory_order_acquire)) {
        _logger.warning("Attempted to stop node that is not running",
                        {{"node_id", node_id_to_string(_node_id)}});
        return;
    }

    _logger.info("Stopping Raft node", {{"node_id", node_id_to_string(_node_id)}});

    // Mark as not running
    _running.store(false, std::memory_order_release);

    // Cancel all pending client operations
    _commit_waiter.cancel_all_operations("Node shutdown");

    // Stop the network server
    _network_server.stop();

    if constexpr (requires { _peer2peer_replicator.stop(); }) {
        _peer2peer_replicator.stop();
    }

    _logger.info("Raft node stopped successfully", {{"node_id", node_id_to_string(_node_id)}});
}

template<raft_types Types>

auto node<Types>::add_server(node_id_type new_node) -> future_type {
    std::lock_guard<std::mutex> lock(_mutex);

    _logger.info("Add server requested",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"new_node", node_id_to_string(new_node)},
                  {"current_state", _state == kythira::server_state::leader      ? "leader"
                                    : _state == kythira::server_state::candidate ? "candidate"
                                                                                 : "follower"}});

    // Only leaders can add servers
    if (_state != kythira::server_state::leader) {
        _logger.warning(
            "Cannot add server: not leader",
            {{"node_id", node_id_to_string(_node_id)}, {"new_node", node_id_to_string(new_node)}});

        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Not leader - cannot add server"));
        return future;
    }

    // Check if configuration change is already in progress
    if (_config_synchronizer.is_configuration_change_in_progress()) {
        _logger.warning(
            "Cannot add server: configuration change already in progress",
            {{"node_id", node_id_to_string(_node_id)}, {"new_node", node_id_to_string(new_node)}});

        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Configuration change already in progress"));
        return future;
    }

    // Validate new node is not already in configuration
    if (_membership.is_node_in_configuration(new_node, _configuration)) {
        _logger.warning(
            "Cannot add server: already in configuration",
            {{"node_id", node_id_to_string(_node_id)}, {"new_node", node_id_to_string(new_node)}});

        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Node already in configuration"));
        return future;
    }

    // Validate new node with membership manager
    if (!_membership.validate_new_node(new_node)) {
        _logger.warning(
            "Cannot add server: validation failed",
            {{"node_id", node_id_to_string(_node_id)}, {"new_node", node_id_to_string(new_node)}});

        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Node validation failed"));
        return future;
    }

    _logger.info(
        "Starting server addition with joint consensus",
        {{"node_id", node_id_to_string(_node_id)}, {"new_node", node_id_to_string(new_node)}});

    // Create new configuration with added node
    std::vector<node_id_type> new_nodes = _configuration.nodes();
    new_nodes.push_back(new_node);

    // C_new (final target): non-joint, used by config synchronizer to know when we are done.
    // Learners are preserved unchanged — add_server() only affects the voting set.
    cluster_configuration_type new_config{new_nodes, false, std::nullopt,
                                          _configuration.learners()};

    // Start configuration change using ConfigurationSynchronizer
    auto timeout = _config.append_entries_timeout() * 10;  // Longer timeout for config changes
    auto config_future = _config_synchronizer.start_configuration_change(new_config, timeout);

    // Build C_old+new joint configuration and append it as a configuration log entry.
    // Setting _configuration = joint_config immediately causes the leader to replicate to
    // all nodes in C_old ∪ C_new on the next heartbeat, before the entry commits.
    cluster_configuration_type joint_config{new_nodes, true, _configuration.nodes(),
                                            _configuration.learners()};
    auto joint_bytes = serialize_configuration<node_id_type>(joint_config);
    const auto joint_idx = get_last_log_index() + 1;
    log_entry_type joint_entry{._term = _current_term,
                               ._index = joint_idx,
                               ._command = std::move(joint_bytes),
                               ._type = entry_type::configuration};
    // Initialize tracking for the new server before appending so it starts receiving entries
    _next_index[new_node] = joint_idx;
    _match_index[new_node] = 0;
    append_log_entry(joint_entry);
    _persistence.append_log_entry(joint_entry);
    _configuration = joint_config;
    sync_peer2peer_membership();

    _metrics.set_metric_name("raft_add_server_started");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("new_node", node_id_to_string(new_node));
    _metrics.add_one();
    _metrics.emit();

    // Return the future from configuration synchronizer
    // It will complete when the configuration change is committed
    return config_future.thenTry([this, new_node, old_config = _configuration, new_config,
                                  stop_flag = _stop_flag](auto try_result) {
        if (stop_flag->load(std::memory_order_acquire)) {
            throw std::runtime_error("node stopped");
        }
        if (try_result.hasException()) {
            _logger.error("Add server failed", {{"node_id", node_id_to_string(_node_id)},
                                                {"new_node", node_id_to_string(new_node)}});

            _metrics.set_metric_name("raft_add_server_failed");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("new_node", node_id_to_string(new_node));
            _metrics.add_one();
            _metrics.emit();

            std::rethrow_exception(try_result.exception());
        }

        _logger.info(
            "Add server completed successfully",
            {{"node_id", node_id_to_string(_node_id)}, {"new_node", node_id_to_string(new_node)}});

        // Notify membership manager of configuration change
        _membership.handle_cluster_membership_change(old_config, new_config);

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

    _logger.info("Remove server requested",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"old_node", node_id_to_string(old_node)},
                  {"current_state", _state == kythira::server_state::leader      ? "leader"
                                    : _state == kythira::server_state::candidate ? "candidate"
                                                                                 : "follower"}});

    // Only leaders can remove servers
    if (_state != kythira::server_state::leader) {
        _logger.warning(
            "Cannot remove server: not leader",
            {{"node_id", node_id_to_string(_node_id)}, {"old_node", node_id_to_string(old_node)}});

        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Not leader - cannot remove server"));
        return future;
    }

    // Check if configuration change is already in progress
    if (_config_synchronizer.is_configuration_change_in_progress()) {
        _logger.warning(
            "Cannot remove server: configuration change already in progress",
            {{"node_id", node_id_to_string(_node_id)}, {"old_node", node_id_to_string(old_node)}});

        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Configuration change already in progress"));
        return future;
    }

    // Validate server is in current configuration
    if (!_membership.is_node_in_configuration(old_node, _configuration)) {
        _logger.warning(
            "Cannot remove server: not in configuration",
            {{"node_id", node_id_to_string(_node_id)}, {"old_node", node_id_to_string(old_node)}});

        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Node not in configuration"));
        return future;
    }

    // Check if removing this server would leave cluster with no nodes
    if (_configuration.nodes().size() <= 1) {
        _logger.warning(
            "Cannot remove server: would leave cluster empty",
            {{"node_id", node_id_to_string(_node_id)}, {"old_node", node_id_to_string(old_node)}});

        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Cannot remove last node from cluster"));
        return future;
    }

    bool removing_self = (old_node == _node_id);

    _logger.info("Starting server removal with joint consensus",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"old_node", node_id_to_string(old_node)},
                  {"removing_self", removing_self ? "true" : "false"}});

    // Create new configuration without the removed node
    std::vector<node_id_type> new_nodes;
    for (const auto& n : _configuration.nodes()) {
        if (n != old_node) {
            new_nodes.push_back(n);
        }
    }

    // C_new (final target): non-joint, used by config synchronizer to know when we are done.
    // Learners are preserved unchanged — remove_server() only affects the voting set.
    cluster_configuration_type new_config{new_nodes, false, std::nullopt,
                                          _configuration.learners()};

    // Start configuration change using ConfigurationSynchronizer
    auto timeout = _config.append_entries_timeout() * 10;  // Longer timeout for config changes
    auto config_future = _config_synchronizer.start_configuration_change(new_config, timeout);

    // Build C_old+new joint configuration and append it as a configuration log entry.
    // Do NOT erase old_node from _next_index/_match_index yet: it must continue receiving
    // AppendEntries during the joint phase for quorum and catchup. Cleanup happens when C_new
    // commits in apply_committed_entries().
    cluster_configuration_type joint_config{new_nodes, true, _configuration.nodes(),
                                            _configuration.learners()};
    auto joint_bytes = serialize_configuration<node_id_type>(joint_config);
    const auto joint_idx = get_last_log_index() + 1;
    log_entry_type joint_entry{._term = _current_term,
                               ._index = joint_idx,
                               ._command = std::move(joint_bytes),
                               ._type = entry_type::configuration};
    append_log_entry(joint_entry);
    _persistence.append_log_entry(joint_entry);
    _configuration = joint_config;
    sync_peer2peer_membership();
    _unresponsive_followers.erase(old_node);

    _metrics.set_metric_name("raft_remove_server_started");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("old_node", node_id_to_string(old_node));
    _metrics.add_dimension("removing_self", removing_self ? "true" : "false");
    _metrics.add_one();
    _metrics.emit();

    // Return the future from configuration synchronizer
    return config_future.thenTry([this, old_node, removing_self, old_config = _configuration,
                                  new_config, stop_flag = _stop_flag](auto try_result) {
        if (stop_flag->load(std::memory_order_acquire)) {
            throw std::runtime_error("node stopped");
        }
        if (try_result.hasException()) {
            _logger.error("Remove server failed", {{"node_id", node_id_to_string(_node_id)},
                                                   {"old_node", node_id_to_string(old_node)}});

            _metrics.set_metric_name("raft_remove_server_failed");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("old_node", node_id_to_string(old_node));
            _metrics.add_one();
            _metrics.emit();

            std::rethrow_exception(try_result.exception());
        }

        _logger.info(
            "Remove server completed successfully",
            {{"node_id", node_id_to_string(_node_id)}, {"old_node", node_id_to_string(old_node)}});

        // Notify membership manager of configuration change
        _membership.handle_cluster_membership_change(old_config, new_config);

        // If we removed ourselves, step down from leadership
        if (removing_self) {
            _logger.info("Removed self from cluster, stepping down",
                         {{"node_id", node_id_to_string(_node_id)}});

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

// ── Learner admission, removal, and promotion (.kiro/specs/non-voting-nodes/) ──

template<raft_types Types>

auto node<Types>::add_learner(node_id_type new_node) -> future_type {
    std::lock_guard<std::mutex> lock(_mutex);

    _logger.info("Add learner requested", {{"node_id", node_id_to_string(_node_id)},
                                           {"new_node", node_id_to_string(new_node)}});

    if (_state != kythira::server_state::leader) {
        _logger.warning(
            "Cannot add learner: not leader",
            {{"node_id", node_id_to_string(_node_id)}, {"new_node", node_id_to_string(new_node)}});
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Not leader - cannot add learner"));
        return future;
    }

    bool already_present =
        _membership.is_node_in_configuration(new_node, _configuration) ||
        std::find(_configuration.learners().begin(), _configuration.learners().end(), new_node) !=
            _configuration.learners().end();
    if (already_present) {
        _logger.warning(
            "Cannot add learner: already in configuration",
            {{"node_id", node_id_to_string(_node_id)}, {"new_node", node_id_to_string(new_node)}});
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Node already in configuration"));
        return future;
    }

    // Placement-capacity criterion: only admit a learner when its group has
    // room — either an independent learner_capacity declared for the group, or
    // (by default) the group's combined voting+learner count below its
    // desired-topology target_count (Requirement 2,
    // .kiro/specs/non-voting-nodes/requirements.md).
    auto group = placement_of(new_node);
    if (!group_has_admission_capacity(group)) {
        _logger.warning(
            "Cannot add learner: placement group at capacity",
            {{"node_id", node_id_to_string(_node_id)}, {"new_node", node_id_to_string(new_node)}});
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(learner_capacity_exceeded_exception());
        return future;
    }

    // Plain (non-joint) configuration change: only _learners changes, so it commits
    // under the current voting quorum with no joint-consensus phase.
    cluster_configuration_type new_config = _configuration;
    new_config._learners.push_back(new_node);

    auto command = serialize_configuration<node_id_type>(new_config);
    const auto entry_index = get_last_log_index() + 1;
    log_entry_type entry{._term = _current_term,
                         ._index = entry_index,
                         ._command = std::move(command),
                         ._type = entry_type::configuration};

    // Initialize tracking before appending so replication starts on the next heartbeat.
    _next_index[new_node] = entry_index;
    _match_index[new_node] = 0;
    append_log_entry(entry);
    _persistence.append_log_entry(entry);
    _configuration = new_config;
    sync_peer2peer_membership();

    _metrics.set_metric_name("raft_add_learner_started");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("new_node", node_id_to_string(new_node));
    _metrics.add_one();
    _metrics.emit();

    promise_type promise;
    auto future = promise.getFuture();
    auto shared_promise = std::make_shared<promise_type>(std::move(promise));
    auto node_id = _node_id;
    auto& logger = _logger;

    _commit_waiter.register_operation(
        entry_index,
        [shared_promise, node_id, new_node, &logger](std::vector<std::byte> result) mutable {
            logger.info("Add learner completed successfully",
                        {{"node_id", node_id_to_string(node_id)},
                         {"new_node", node_id_to_string(new_node)}});
            shared_promise->setValue(std::move(result));
        },
        [shared_promise, node_id, new_node, &logger](std::exception_ptr ex) mutable {
            logger.error("Add learner failed", {{"node_id", node_id_to_string(node_id)},
                                                {"new_node", node_id_to_string(new_node)}});
            shared_promise->setException(ex);
        });

    return future;
}

template<raft_types Types>

auto node<Types>::remove_learner(node_id_type learner) -> future_type {
    std::lock_guard<std::mutex> lock(_mutex);

    _logger.info("Remove learner requested", {{"node_id", node_id_to_string(_node_id)},
                                              {"learner", node_id_to_string(learner)}});

    if (_state != kythira::server_state::leader) {
        _logger.warning(
            "Cannot remove learner: not leader",
            {{"node_id", node_id_to_string(_node_id)}, {"learner", node_id_to_string(learner)}});
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Not leader - cannot remove learner"));
        return future;
    }

    const auto& current_learners = _configuration.learners();
    bool is_present = std::find(current_learners.begin(), current_learners.end(), learner) !=
                      current_learners.end();
    if (!is_present) {
        _logger.warning(
            "Cannot remove learner: not in configuration",
            {{"node_id", node_id_to_string(_node_id)}, {"learner", node_id_to_string(learner)}});
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Node not a learner in configuration"));
        return future;
    }

    // Plain (non-joint) configuration change: removing a learner never affects the
    // voting set or quorum, so no joint-consensus phase is needed.
    cluster_configuration_type new_config = _configuration;
    new_config._learners.erase(
        std::remove(new_config._learners.begin(), new_config._learners.end(), learner),
        new_config._learners.end());

    auto command = serialize_configuration<node_id_type>(new_config);
    const auto entry_index = get_last_log_index() + 1;
    log_entry_type entry{._term = _current_term,
                         ._index = entry_index,
                         ._command = std::move(command),
                         ._type = entry_type::configuration};

    append_log_entry(entry);
    _persistence.append_log_entry(entry);
    _configuration = new_config;
    sync_peer2peer_membership();
    // _next_index/_match_index for `learner` are cleaned up once this entry commits
    // and apply_committed_entries() observes it is no longer in nodes() or learners().

    _metrics.set_metric_name("raft_remove_learner_started");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("learner", node_id_to_string(learner));
    _metrics.add_one();
    _metrics.emit();

    promise_type promise;
    auto future = promise.getFuture();
    auto shared_promise = std::make_shared<promise_type>(std::move(promise));
    auto node_id = _node_id;
    auto& logger = _logger;

    _commit_waiter.register_operation(
        entry_index,
        [shared_promise, node_id, learner, &logger](std::vector<std::byte> result) mutable {
            logger.info(
                "Remove learner completed successfully",
                {{"node_id", node_id_to_string(node_id)}, {"learner", node_id_to_string(learner)}});
            shared_promise->setValue(std::move(result));
        },
        [shared_promise, node_id, learner, &logger](std::exception_ptr ex) mutable {
            logger.error("Remove learner failed", {{"node_id", node_id_to_string(node_id)},
                                                   {"learner", node_id_to_string(learner)}});
            shared_promise->setException(ex);
        });

    return future;
}

template<raft_types Types>

auto node<Types>::promote_to_voter(node_id_type learner) -> future_type {
    std::lock_guard<std::mutex> lock(_mutex);

    _logger.info("Promote to voter requested", {{"node_id", node_id_to_string(_node_id)},
                                                {"learner", node_id_to_string(learner)}});

    if (_state != kythira::server_state::leader) {
        _logger.warning(
            "Cannot promote to voter: not leader",
            {{"node_id", node_id_to_string(_node_id)}, {"learner", node_id_to_string(learner)}});
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Not leader - cannot promote to voter"));
        return future;
    }

    if (_config_synchronizer.is_configuration_change_in_progress()) {
        _logger.warning(
            "Cannot promote to voter: configuration change already in progress",
            {{"node_id", node_id_to_string(_node_id)}, {"learner", node_id_to_string(learner)}});
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Configuration change already in progress"));
        return future;
    }

    const auto& current_learners = _configuration.learners();
    bool is_learner_node = std::find(current_learners.begin(), current_learners.end(), learner) !=
                           current_learners.end();
    if (!is_learner_node) {
        _logger.warning(
            "Cannot promote to voter: not a learner",
            {{"node_id", node_id_to_string(_node_id)}, {"learner", node_id_to_string(learner)}});
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(std::runtime_error("Node is not a learner in configuration"));
        return future;
    }

    // Placement-capacity criterion: only promote when the learner's group's current
    // VOTING-only count is below the group's desired-topology target_count —
    // promoting past the target would over-grow that group's voting membership
    // (Requirement 3, .kiro/specs/non-voting-nodes/requirements.md). Always uses
    // target_count, never any independent learner_capacity the group may declare
    // (Requirement 3.4). A learner blocked here is left completely unaffected: no
    // state is recorded, so it becomes a promotion candidate again the moment its
    // own group's live voting count drops (Requirement 5).
    auto group = placement_of(learner);
    if (!group_has_promotion_capacity(group)) {
        _logger.warning(
            "Cannot promote to voter: placement group at voting capacity",
            {{"node_id", node_id_to_string(_node_id)}, {"learner", node_id_to_string(learner)}});
        promise_type promise;
        auto future = promise.getFuture();
        promise.setException(voting_capacity_exceeded_exception());
        return future;
    }

    _logger.info(
        "Starting learner promotion with joint consensus",
        {{"node_id", node_id_to_string(_node_id)}, {"learner", node_id_to_string(learner)}});

    std::vector<node_id_type> new_voting = _configuration.nodes();
    new_voting.push_back(learner);

    std::vector<node_id_type> remaining_learners = _configuration.learners();
    remaining_learners.erase(
        std::remove(remaining_learners.begin(), remaining_learners.end(), learner),
        remaining_learners.end());

    // C_new (final target): non-joint, used by config synchronizer to know when we are done.
    cluster_configuration_type new_config{new_voting, false, std::nullopt, remaining_learners};

    auto timeout = _config.append_entries_timeout() * 10;  // Longer timeout for config changes
    auto config_future = _config_synchronizer.start_configuration_change(new_config, timeout);

    // Build C_old+new joint configuration and append it as a configuration log entry.
    cluster_configuration_type joint_config{new_voting, true, _configuration.nodes(),
                                            remaining_learners};
    auto joint_bytes = serialize_configuration<node_id_type>(joint_config);
    const auto joint_idx = get_last_log_index() + 1;
    log_entry_type joint_entry{._term = _current_term,
                               ._index = joint_idx,
                               ._command = std::move(joint_bytes),
                               ._type = entry_type::configuration};
    // Deliberately do NOT touch _next_index[learner]/_match_index[learner] here: they
    // already exist from the learner's time as a non-voting replica, and resetting them
    // would force a wasteful resend of already-replicated entries (Requirement 6.6).
    append_log_entry(joint_entry);
    _persistence.append_log_entry(joint_entry);
    _configuration = joint_config;
    sync_peer2peer_membership();

    _metrics.set_metric_name("raft_promote_to_voter_started");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("learner", node_id_to_string(learner));
    _metrics.add_one();
    _metrics.emit();

    return config_future.thenTry([this, learner, old_config = _configuration, new_config,
                                  stop_flag = _stop_flag](auto try_result) {
        if (stop_flag->load(std::memory_order_acquire)) {
            throw std::runtime_error("node stopped");
        }
        if (try_result.hasException()) {
            _logger.error("Promote to voter failed", {{"node_id", node_id_to_string(_node_id)},
                                                      {"learner", node_id_to_string(learner)}});
            std::rethrow_exception(try_result.exception());
        }

        _logger.info(
            "Promote to voter completed successfully",
            {{"node_id", node_id_to_string(_node_id)}, {"learner", node_id_to_string(learner)}});

        _membership.handle_cluster_membership_change(old_config, new_config);

        _metrics.set_metric_name("raft_promote_to_voter_success");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("learner", node_id_to_string(learner));
        _metrics.add_one();
        _metrics.emit();

        return std::vector<std::byte>{};
    });
}

template<raft_types Types>

auto node<Types>::check_election_timeout() -> void {
    // Requirement 3.2/4.1 (.kiro/specs/peer2peer-log-replication/): gossip this
    // node's own progress and check for a peer-to-peer catch-up opportunity on
    // every tick, regardless of server_state. Piggybacked here because this is
    // the one tick callback every existing binary's external timer loop already
    // calls unconditionally for every node state — unlike check_heartbeat_timeout(),
    // which returns early for non-leaders before doing any other per-tick work.
    // Both manage their own locking internally and never hold _mutex across
    // network I/O.
    maybe_gossip_progress();
    maybe_catch_up_from_peer();

    std::unique_lock<std::mutex> lock(_mutex);

    // Only followers and candidates check for election timeout
    if (_state == kythira::server_state::leader) {
        return;
    }

    // Learners never start an election, regardless of elapsed time since the last
    // heartbeat (.kiro/specs/non-voting-nodes/requirements.md, Requirement 2.1).
    if (is_learner()) {
        return;
    }

    // Check if election timeout has elapsed
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_heartbeat);

    if (elapsed >= _election_timeout) {
        _logger.debug(
            "Election timeout elapsed",
            {{"node_id", node_id_to_string(_node_id)},
             {"state", _state == kythira::server_state::follower ? "follower" : "candidate"},
             {"term", std::to_string(_current_term)}});

        // `.kiro/specs/raft-pre-vote/`: Types bundles whose network_client_type
        // supports PreVote run a trial round first (start_pre_vote() only
        // escalates to become_candidate()+start_election() if it would
        // actually win a majority) — this is what stops a node that's been
        // isolated (or just restarted) from disrupting an otherwise-stable
        // cluster with a term it climbed while cut off, without changing
        // the RPC-level protocol any Types bundle that doesn't implement
        // PreVote relies on. Any Types bundle whose transport doesn't
        // support it (e.g. the in-memory simulator) falls through to
        // exactly the same become_candidate()+start_election() sequence
        // this function has always used.
        if constexpr (network_client_with_pre_vote<network_client_type>) {
            lock.unlock();
            start_pre_vote();
        } else {
            become_candidate();

            // Release the lock before start_election(): send_request_vote blocks on the
            // simulator CV, so the future callbacks resolve inline on this same thread.
            // The callbacks re-acquire _mutex, which would deadlock if we still held it.
            lock.unlock();
            start_election();
        }
    }
}

template<raft_types Types>

auto node<Types>::check_heartbeat_timeout() -> void {
    bool should_heartbeat = false;

    {
        std::unique_lock<std::mutex> lock(_mutex);

        // Check for timed-out operations (do this for all states)
        auto cancelled_count = _commit_waiter.cancel_timed_out_operations();
        if (cancelled_count > 0) {
            _logger.debug("Cancelled timed-out operations",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"cancelled_count", std::to_string(cancelled_count)}});

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
            _logger.debug("Heartbeat timeout elapsed, sending heartbeats",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"term", std::to_string(_current_term)}});

            _last_heartbeat = now;
            should_heartbeat = true;
        }
    }  // _mutex released before any blocking I/O

    // send_heartbeats() does blocking network I/O whose Folly callbacks re-acquire
    // _mutex inline.  Must not hold the lock here (same pattern as check_election_timeout
    // releasing before start_election()).
    if (should_heartbeat) {
        send_heartbeats();
    }

    // Quorum assessment (Req 13) — piggybacks on the heartbeat tick so the
    // event loop does not need a separate quorum timer.  run_quorum_assessment()
    // must be called without _mutex held (it calls future.get() which may block).
    bool should_assess = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_quorum_check_active) {
            auto now = std::chrono::steady_clock::now();
            bool interval_elapsed = (now - _last_quorum_check) >= _config.quorum_check_interval();
            should_assess = _quorum_immediate_check || interval_elapsed;
            if (should_assess) {
                _quorum_immediate_check = false;
            }
        }
    }
    if (should_assess) {
        run_quorum_assessment();
    }
}

// Placeholder implementations for private methods
template<raft_types Types>

auto node<Types>::initialize_from_storage() -> void {
    _logger.info("Initializing node from persistent storage",
                 {{"node_id", node_id_to_string(_node_id)}});

    // Load current term
    _current_term = _persistence.load_current_term();

    // Load voted_for
    _voted_for = _persistence.load_voted_for();

    // Restore from snapshot (if any): state machine, configuration, commit/applied indices
    auto snap_opt = _persistence.load_snapshot();
    if (snap_opt.has_value()) {
        const auto& snap = *snap_opt;
        try {
            _state_machine.restore_from_snapshot(snap.state_machine_state(),
                                                 snap.last_included_index());
        } catch (...) {
            _logger.warning("Failed to restore state machine from snapshot during init",
                            {{"node_id", node_id_to_string(_node_id)}});
        }
        _last_applied = snap.last_included_index();
        _commit_index = snap.last_included_index();
        _configuration = snap.configuration();

        _logger.info("Restored from snapshot",
                     {{"node_id", node_id_to_string(_node_id)},
                      {"last_included_index", std::to_string(snap.last_included_index())}});
    }

    // Reload log entries from persistence (entries after the snapshot point)
    log_index_type first_needed = (_last_applied > 0) ? _last_applied + 1 : log_index_type{1};
    log_index_type last_persisted = _persistence.get_last_log_index();
    if (last_persisted >= first_needed) {
        auto entries = _persistence.get_log_entries(first_needed, last_persisted);
        for (auto& entry : entries) {
            _log.push_back(std::move(entry));
        }
        _logger.info("Reloaded log entries from persistence",
                     {{"node_id", node_id_to_string(_node_id)},
                      {"first_index", std::to_string(first_needed)},
                      {"last_index", std::to_string(last_persisted)},
                      {"count", std::to_string(_log.size())}});
    }

    // Scan log backward for the most recent configuration entry and restore it
    for (auto it = _log.rbegin(); it != _log.rend(); ++it) {
        if (it->type() == entry_type::configuration) {
            _configuration = deserialize_configuration<node_id_type>(it->command());
            _logger.info("Restored configuration from log entry",
                         {{"node_id", node_id_to_string(_node_id)},
                          {"log_index", std::to_string(it->index())}});
            break;
        }
    }

    _logger.info("Node initialized from storage", {{"node_id", node_id_to_string(_node_id)},
                                                   {"current_term", std::to_string(_current_term)},
                                                   {"last_applied", std::to_string(_last_applied)},
                                                   {"log_size", std::to_string(_log.size())}});

    // Covers both _configuration assignments above (snapshot restore and
    // log-scan restore) — called once, single-threaded, before start() has
    // spawned any other thread (Requirement 11.1).
    sync_peer2peer_membership();
}

template<raft_types Types>

auto node<Types>::cluster_members() const -> std::vector<node_id_type> {
    auto members = _configuration.nodes();
    if (_configuration.is_joint_consensus()) {
        const auto& old_nodes = _configuration.old_nodes().value();
        for (auto id : old_nodes) {
            if (std::ranges::find(members, id) == members.end()) {
                members.push_back(id);
            }
        }
    }
    return members;
}

template<raft_types Types>

auto node<Types>::sync_peer2peer_membership() -> void {
    // Fire-and-forget, matching advertise_progress()'s error-handling posture
    // (Requirement 11.3) — a failure or delay here must never block the
    // log-mutating operation that triggered it.
    _peer2peer_replicator.update_membership(cluster_members());
}

template<raft_types Types>

auto node<Types>::register_rpc_handlers() -> void {
    // Register RequestVote handler
    _network_server.register_request_vote_handler(
        [this](const request_vote_request_type& request) -> request_vote_response_type {
            return this->handle_request_vote(request);
        });

    // Register RequestPreVote handler if the network server supports it
    // (`.kiro/specs/raft-pre-vote/`)
    if constexpr (network_server_with_pre_vote<network_server_type>) {
        _network_server.register_request_pre_vote_handler(
            [this](const request_pre_vote_request_type& request) -> request_pre_vote_response_type {
                return this->handle_request_pre_vote(request);
            });
    }

    // Register AppendEntries handler
    _network_server.register_append_entries_handler(
        [this](const append_entries_request_type& request) -> append_entries_response_type {
            return this->handle_append_entries(request);
        });

    // Register InstallSnapshot handler
    _network_server.register_install_snapshot_handler(
        [this](const install_snapshot_request_type& request) -> install_snapshot_response_type {
            return this->handle_install_snapshot(request);
        });

    // Register ClusterJoin handler if the network server supports it
    if constexpr (network_server_with_cluster_join<network_server_type>) {
        _network_server.register_cluster_join_handler(
            [this](const cluster_join_request_type& req) -> cluster_join_response_type {
                return this->handle_cluster_join(req);
            });
    }

    // Register ClusterLeave handler if the network server supports it
    if constexpr (network_server_with_cluster_leave<network_server_type>) {
        _network_server.register_cluster_leave_handler(
            [this](const cluster_leave_request_type& req) -> cluster_leave_response_type {
                return this->handle_cluster_leave(req);
            });
    }

    // Register FetchLogEntries handler if the network server supports it
    // (.kiro/specs/peer2peer-log-replication/, Requirement 5.3)
    if constexpr (network_server_with_log_fetch<network_server_type>) {
        _network_server.register_fetch_log_entries_handler(
            [this](const fetch_log_entries_request_type& req) -> fetch_log_entries_response_type {
                return this->handle_fetch_log_entries(req);
            });
    }

    _logger.debug("RPC handlers registered", {{"node_id", node_id_to_string(_node_id)}});
}

template<raft_types Types>

auto node<Types>::handle_request_vote(const request_vote_request_type& request)
    -> request_vote_response_type {
    std::lock_guard<std::mutex> lock(_mutex);

    // A learner never grants a vote, unconditionally — before inspecting the
    // candidate's term or log. Defense-in-depth: start_election() already only
    // requests votes from the voting set, so a learner should never legitimately
    // receive a RequestVote (.kiro/specs/non-voting-nodes/requirements.md, Req 2.2).
    if (is_learner()) {
        return request_vote_response_type{_current_term, false};
    }

    _logger.debug("Received RequestVote RPC",
                  {{"node_id", node_id_to_string(_node_id)},
                   {"from_candidate", node_id_to_string(request.candidate_id())},
                   {"request_term", std::to_string(request.term())},
                   {"current_term", std::to_string(_current_term)},
                   {"candidate_last_log_index", std::to_string(request.last_log_index())},
                   {"candidate_last_log_term", std::to_string(request.last_log_term())}});

    // Emit metrics for vote request received
    _metrics.set_metric_name("raft_vote_request_received");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_one();
    _metrics.emit();

    // Rule 1: Reply false if request term < current term (§5.1)
    if (request.term() < _current_term) {
        _logger.debug("Denying vote: request term < current term",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"request_term", std::to_string(request.term())},
                       {"current_term", std::to_string(_current_term)}});

        _metrics.set_metric_name("raft_vote_denied");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("reason", "stale_term");
        _metrics.add_one();
        _metrics.emit();

        return request_vote_response_type{_current_term, false};
    }

    // Rule 2: If request term > current term, update current term and become follower (§5.1)
    if (request.term() > _current_term) {
        _logger.info("Discovered higher term in RequestVote, becoming follower",
                     {{"node_id", node_id_to_string(_node_id)},
                      {"old_term", std::to_string(_current_term)},
                      {"new_term", std::to_string(request.term())}});

        become_follower(request.term());
    }

    // Rule 3: Check if we've already voted for another candidate in this term
    if (_voted_for.has_value() && _voted_for.value() != request.candidate_id()) {
        _logger.debug("Denying vote: already voted for another candidate",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"voted_for", node_id_to_string(_voted_for.value())},
                       {"candidate", node_id_to_string(request.candidate_id())},
                       {"term", std::to_string(_current_term)}});

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
        _logger.debug("Denying vote: candidate log not up-to-date",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"candidate", node_id_to_string(request.candidate_id())},
                       {"my_last_log_term", std::to_string(my_last_log_term)},
                       {"my_last_log_index", std::to_string(my_last_log_index)},
                       {"candidate_last_log_term", std::to_string(request.last_log_term())},
                       {"candidate_last_log_index", std::to_string(request.last_log_index())}});

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

    // Notify reconnect thread: we have peer contact
    {
        std::lock_guard<std::mutex> lk(_reconnect_cv_mutex);
        _reconnect_cv.notify_all();
    }

    _logger.info("Granting vote", {{"node_id", node_id_to_string(_node_id)},
                                   {"candidate", node_id_to_string(request.candidate_id())},
                                   {"term", std::to_string(_current_term)}});

    _metrics.set_metric_name("raft_vote_granted");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("candidate", node_id_to_string(request.candidate_id()));
    _metrics.add_one();
    _metrics.emit();

    return request_vote_response_type{_current_term, true};
}

template<raft_types Types>

// `.kiro/specs/raft-pre-vote/`: answers "if you called a real election right
// now, would I vote for you?" without mutating any local state — no term
// bump, no become_follower(), no _voted_for write, no persistence. Two
// checks gate a grant, deliberately different from handle_request_vote()'s:
//
//   1. Leader-stickiness: if we've heard a valid AppendEntries from a
//      genuine leader within the last election_timeout, refuse outright,
//      regardless of the candidate's term or log. This is what stops an
//      isolated node's climbed-while-cut-off term from disrupting an
//      otherwise-stable majority once it reconnects — the isolated node's
//      PreVote broadcasts never reach anyone (it's isolated), so it never
//      wins a majority of pre-votes, so it never increments its real term
//      at all. Uses _last_leader_contact, NOT _last_heartbeat/
//      reset_election_timer() — the latter is also touched by this node's
//      OWN candidacy/leadership transitions and granting a real vote, which
//      would incorrectly suppress pre-vote grants during an ordinary split
//      vote between two mutually-reachable candidates (neither of which has
//      heard from any leader either, but that's not what this check is
//      about).
//   2. The same log-up-to-date comparison as handle_request_vote()'s Rule 4
//      (candidate's last_log_term/last_log_index at least as current as
//      ours) — this is what stops a genuinely-behind node (not just a
//      recently-isolated one) from being able to disrupt progress even once
//      the stickiness window has elapsed.
auto node<Types>::handle_request_pre_vote(const request_pre_vote_request_type& request)
    -> request_pre_vote_response_type {
    std::lock_guard<std::mutex> lock(_mutex);

    if (is_learner()) {
        return request_pre_vote_response_type{_current_term, false};
    }

    _logger.debug("Received RequestPreVote RPC",
                  {{"node_id", node_id_to_string(_node_id)},
                   {"from_candidate", node_id_to_string(request.candidate_id())},
                   {"request_term", std::to_string(request.term())},
                   {"current_term", std::to_string(_current_term)}});

    auto now = std::chrono::steady_clock::now();
    auto since_leader_contact =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_leader_contact);
    if (since_leader_contact < _election_timeout) {
        _logger.debug("Denying pre-vote: heard from a leader recently",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"candidate", node_id_to_string(request.candidate_id())},
                       {"since_leader_contact_ms", std::to_string(since_leader_contact.count())}});
        return request_pre_vote_response_type{_current_term, false};
    }

    auto my_last_log_index = get_last_log_index();
    auto my_last_log_term = get_last_log_term();

    bool candidate_log_up_to_date = false;
    if (request.last_log_term() > my_last_log_term) {
        candidate_log_up_to_date = true;
    } else if (request.last_log_term() == my_last_log_term) {
        candidate_log_up_to_date = (request.last_log_index() >= my_last_log_index);
    }

    if (!candidate_log_up_to_date) {
        _logger.debug("Denying pre-vote: candidate log not up-to-date",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"candidate", node_id_to_string(request.candidate_id())},
                       {"my_last_log_term", std::to_string(my_last_log_term)},
                       {"my_last_log_index", std::to_string(my_last_log_index)},
                       {"candidate_last_log_term", std::to_string(request.last_log_term())},
                       {"candidate_last_log_index", std::to_string(request.last_log_index())}});
        return request_pre_vote_response_type{_current_term, false};
    }

    _logger.debug("Granting pre-vote", {{"node_id", node_id_to_string(_node_id)},
                                        {"candidate", node_id_to_string(request.candidate_id())}});

    return request_pre_vote_response_type{_current_term, true};
}

template<raft_types Types>

auto node<Types>::handle_append_entries(const append_entries_request_type& request)
    -> append_entries_response_type {
    std::lock_guard<std::mutex> lock(_mutex);

    _logger.debug("Received AppendEntries RPC",
                  {{"node_id", node_id_to_string(_node_id)},
                   {"from_leader", node_id_to_string(request.leader_id())},
                   {"request_term", std::to_string(request.term())},
                   {"current_term", std::to_string(_current_term)},
                   {"prev_log_index", std::to_string(request.prev_log_index())},
                   {"prev_log_term", std::to_string(request.prev_log_term())},
                   {"num_entries", std::to_string(request.entries().size())},
                   {"leader_commit", std::to_string(request.leader_commit())}});

    // Emit metrics for AppendEntries received
    _metrics.set_metric_name("raft_append_entries_received");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("is_heartbeat", request.entries().empty() ? "true" : "false");
    _metrics.add_one();
    _metrics.emit();

    // Rule 1: Reply false if request term < current term (§5.1)
    if (request.term() < _current_term) {
        _logger.debug("Rejecting AppendEntries: request term < current term",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"request_term", std::to_string(request.term())},
                       {"current_term", std::to_string(_current_term)}});

        _metrics.set_metric_name("raft_append_entries_rejected");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("reason", "stale_term");
        _metrics.add_one();
        _metrics.emit();

        return append_entries_response_type{_current_term, false, std::nullopt, std::nullopt};
    }

    // Rule 2: If request term >= current term, update current term and become follower (§5.1)
    if (request.term() > _current_term) {
        _logger.info("Discovered higher term in AppendEntries, becoming follower",
                     {{"node_id", node_id_to_string(_node_id)},
                      {"old_term", std::to_string(_current_term)},
                      {"new_term", std::to_string(request.term())}});

        become_follower(request.term());
    } else if (_state == kythira::server_state::candidate) {
        // If we're a candidate and receive AppendEntries from valid leader, become follower
        _logger.info(
            "Received valid AppendEntries as candidate, becoming follower",
            {{"node_id", node_id_to_string(_node_id)}, {"term", std::to_string(_current_term)}});

        become_follower(_current_term);
    }

    // Reset election timer on valid AppendEntries (§5.2)
    reset_election_timer();

    // `.kiro/specs/raft-pre-vote/`: this is the ONLY place that updates
    // _last_leader_contact — deliberately not shared with
    // reset_election_timer(), which is also called from this node's own
    // become_candidate()/become_leader()/vote-granting paths (see
    // handle_request_pre_vote()'s comment for why that distinction matters).
    _last_leader_contact = std::chrono::steady_clock::now();

    // Track the current leader for ClusterJoin redirect responses and notify
    // the reconnect thread that peer contact has been established.
    _known_leader = request.leader_id();
    {
        std::lock_guard<std::mutex> lk(_reconnect_cv_mutex);
        _reconnect_cv.notify_all();
    }

    // Rules 3-5 (consistency check, conflict-truncation, append) — extracted into
    // append_entries_with_consistency_check() (.kiro/specs/peer2peer-log-replication/,
    // Requirement 6.1) so the peer-to-peer fetch path shares this exact logic.
    auto response = append_entries_with_consistency_check(
        request.prev_log_index(), request.prev_log_term(), request.entries());
    if (!response.success()) {
        return response;
    }

    // Rule 6: If leaderCommit > commitIndex, set commitIndex = min(leaderCommit, index of last new
    // entry)
    if (request.leader_commit() > _commit_index) {
        auto old_commit_index = _commit_index;
        _commit_index = std::min(request.leader_commit(), get_last_log_index());

        _logger.debug("Updated commit index",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"old_commit_index", std::to_string(old_commit_index)},
                       {"new_commit_index", std::to_string(_commit_index)},
                       {"leader_commit", std::to_string(request.leader_commit())}});

        // Apply newly committed entries to state machine
        apply_committed_entries();
    }

    _logger.debug("AppendEntries succeeded",
                  {{"node_id", node_id_to_string(_node_id)},
                   {"last_log_index", std::to_string(get_last_log_index())},
                   {"commit_index", std::to_string(_commit_index)}});

    _metrics.set_metric_name("raft_append_entries_accepted");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("entries_appended", std::to_string(request.entries().size()));
    _metrics.add_one();
    _metrics.emit();

    return response;
}

template<raft_types Types>

auto node<Types>::append_entries_with_consistency_check(log_index_type prev_log_index,
                                                        term_id_type prev_log_term,
                                                        const std::vector<log_entry_type>& entries)
    -> append_entries_response_type {
    // Rule 3: Reply false if log doesn't contain an entry at prevLogIndex whose term matches
    // prevLogTerm (§5.3)
    if (prev_log_index > 0) {
        // Check if we have an entry at prevLogIndex
        if (prev_log_index > get_last_log_index()) {
            // We don't have enough entries
            _logger.debug("Rejecting AppendEntries: log too short",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"prev_log_index", std::to_string(prev_log_index)},
                           {"last_log_index", std::to_string(get_last_log_index())}});

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
        auto prev_entry = get_log_entry(prev_log_index);
        if (!prev_entry.has_value() || prev_entry->term() != prev_log_term) {
            // Term mismatch at prevLogIndex
            auto conflict_term = prev_entry.has_value() ? prev_entry->term() : term_id_type{0};

            _logger.debug("Rejecting AppendEntries: term mismatch at prevLogIndex",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"prev_log_index", std::to_string(prev_log_index)},
                           {"expected_term", std::to_string(prev_log_term)},
                           {"actual_term", std::to_string(conflict_term)}});

            _metrics.set_metric_name("raft_append_entries_rejected");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("reason", "term_mismatch");
            _metrics.add_one();
            _metrics.emit();

            // Find the first index of the conflicting term for faster backtracking
            log_index_type conflict_index = prev_log_index;
            if (prev_entry.has_value()) {
                // Search backwards to find first entry with this term
                for (log_index_type i = prev_log_index; i > 0; --i) {
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

            return append_entries_response_type{_current_term, false, conflict_index,
                                                conflict_term};
        }
    }

    // Rule 4: If an existing entry conflicts with a new one (same index but different terms),
    // delete the existing entry and all that follow it (§5.3)
    bool log_modified = false;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& new_entry = entries[i];
        auto entry_index = prev_log_index + i + 1;

        // Check if we have an entry at this index
        if (entry_index <= get_last_log_index()) {
            auto existing_entry = get_log_entry(entry_index);
            if (existing_entry.has_value() && existing_entry->term() != new_entry.term()) {
                // Conflict detected - delete this entry and all that follow
                _logger.info("Detected log conflict, truncating log",
                             {{"node_id", node_id_to_string(_node_id)},
                              {"conflict_index", std::to_string(entry_index)},
                              {"existing_term", std::to_string(existing_entry->term())},
                              {"new_term", std::to_string(new_entry.term())}});

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
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& new_entry = entries[i];
        auto entry_index = prev_log_index + i + 1;

        if (entry_index > get_last_log_index()) {
            // This is a new entry - append it
            append_log_entry(new_entry);
            log_modified = true;

            // Persist the new entry
            _persistence.append_log_entry(new_entry);

            _logger.debug("Appended new log entry", {{"node_id", node_id_to_string(_node_id)},
                                                     {"index", std::to_string(entry_index)},
                                                     {"term", std::to_string(new_entry.term())}});
        }
    }

    // After any log modification (truncation or append), rescan backward for the most recent
    // configuration entry and update _configuration.  This handles both the "follower receives
    // a config entry" path and the "log is truncated past a config entry" path in one place.
    // Applies identically whether entries arrived from the leader or a peer
    // (.kiro/specs/peer2peer-log-replication/design.md Property 2/3) — nothing about
    // this rescan is leader-specific.
    if (log_modified) {
        cluster_configuration_type reverted = _boot_configuration;
        for (auto it = _log.rbegin(); it != _log.rend(); ++it) {
            if (it->type() == entry_type::configuration) {
                reverted = deserialize_configuration<node_id_type>(it->command());
                break;
            }
        }
        _configuration = reverted;
        sync_peer2peer_membership();

        // Persist current term alongside any log modification (matches the
        // pre-extraction combined condition; the other original disjunct,
        // "request.term() > _current_term" evaluated after Rule 2 already
        // synced _current_term up to request.term(), was always false there).
        _persistence.save_current_term(_current_term);
    }

    return append_entries_response_type{_current_term, true, std::nullopt, std::nullopt};
}

template<raft_types Types>

auto node<Types>::handle_install_snapshot(const install_snapshot_request_type& request)
    -> install_snapshot_response_type {
    std::lock_guard<std::mutex> lock(_mutex);

    _logger.debug("Received InstallSnapshot RPC",
                  {{"node_id", node_id_to_string(_node_id)},
                   {"from_leader", node_id_to_string(request.leader_id())},
                   {"request_term", std::to_string(request.term())},
                   {"current_term", std::to_string(_current_term)},
                   {"last_included_index", std::to_string(request.last_included_index())},
                   {"last_included_term", std::to_string(request.last_included_term())},
                   {"offset", std::to_string(request.offset())},
                   {"data_size", std::to_string(request.data().size())},
                   {"done", request.done() ? "true" : "false"}});

    // Emit metrics for InstallSnapshot received
    _metrics.set_metric_name("raft_install_snapshot_received");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("is_first_chunk", request.offset() == 0 ? "true" : "false");
    _metrics.add_dimension("is_last_chunk", request.done() ? "true" : "false");
    _metrics.add_one();
    _metrics.emit();

    // Rule 1: Reply immediately if term < currentTerm (§7)
    if (request.term() < _current_term) {
        _logger.debug("Rejecting InstallSnapshot: request term < current term",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"request_term", std::to_string(request.term())},
                       {"current_term", std::to_string(_current_term)}});

        _metrics.set_metric_name("raft_install_snapshot_rejected");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("reason", "stale_term");
        _metrics.add_one();
        _metrics.emit();

        return install_snapshot_response_type{_current_term};
    }

    // Rule 2: If request term > current term, update current term and become follower
    if (request.term() > _current_term) {
        _logger.info("Discovered higher term in InstallSnapshot, becoming follower",
                     {{"node_id", node_id_to_string(_node_id)},
                      {"old_term", std::to_string(_current_term)},
                      {"new_term", std::to_string(request.term())}});

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

        _logger.debug("Started receiving snapshot",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"last_included_index", std::to_string(request.last_included_index())},
                       {"last_included_term", std::to_string(request.last_included_term())},
                       {"first_chunk_size", std::to_string(request.data().size())}});
    } else {
        // Subsequent chunk - append to buffer
        if (snapshot_buffers.find(_node_id) == snapshot_buffers.end()) {
            // Missing first chunk - reject
            _logger.error("Received snapshot chunk without first chunk",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"offset", std::to_string(request.offset())}});

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
            _logger.error("Snapshot chunk offset mismatch",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"expected_offset", std::to_string(buffer.size())},
                           {"actual_offset", std::to_string(request.offset())}});

            _metrics.set_metric_name("raft_install_snapshot_rejected");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("reason", "offset_mismatch");
            _metrics.add_one();
            _metrics.emit();

            return install_snapshot_response_type{_current_term};
        }

        // Append chunk data
        buffer.insert(buffer.end(), request.data().begin(), request.data().end());

        _logger.debug("Received snapshot chunk",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"offset", std::to_string(request.offset())},
                       {"chunk_size", std::to_string(request.data().size())},
                       {"total_size", std::to_string(buffer.size())}});
    }

    // Rule 4: Reply and wait for more data chunks if done is false
    if (!request.done()) {
        _logger.debug("Waiting for more snapshot chunks",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"bytes_received", std::to_string(snapshot_buffers[_node_id].size())}});

        return install_snapshot_response_type{_current_term};
    }

    // Rule 5: Save snapshot file, discard any existing or partial snapshot with smaller index
    auto& complete_snapshot_data = snapshot_buffers[_node_id];

    _logger.info("Installing complete snapshot",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"last_included_index", std::to_string(request.last_included_index())},
                  {"last_included_term", std::to_string(request.last_included_term())},
                  {"snapshot_size", std::to_string(complete_snapshot_data.size())}});

    // Create snapshot object
    snapshot_type snap{
        request.last_included_index(), request.last_included_term(),
        _configuration,  // Use current configuration (leader will send updated config if needed)
        complete_snapshot_data};

    // Rule 6: If existing log entry has same index and term as snapshot's last included entry,
    // retain log entries following it and reply
    bool retain_log = false;
    if (request.last_included_index() <= get_last_log_index()) {
        auto entry_at_snapshot_index = get_log_entry(request.last_included_index());
        if (entry_at_snapshot_index.has_value() &&
            entry_at_snapshot_index->term() == request.last_included_term()) {
            // Log entry matches snapshot - retain entries after snapshot
            retain_log = true;

            _logger.debug("Retaining log entries after snapshot",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"snapshot_index", std::to_string(request.last_included_index())},
                           {"last_log_index", std::to_string(get_last_log_index())}});
        }
    }

    // Rule 7: Discard the entire log if no matching entry exists
    if (!retain_log) {
        _logger.info("Discarding entire log for snapshot installation",
                     {{"node_id", node_id_to_string(_node_id)},
                      {"old_log_size", std::to_string(_log.size())},
                      {"snapshot_index", std::to_string(request.last_included_index())}});

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

    _logger.info("Successfully installed snapshot",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"last_included_index", std::to_string(request.last_included_index())},
                  {"last_included_term", std::to_string(request.last_included_term())},
                  {"commit_index", std::to_string(_commit_index)},
                  {"last_applied", std::to_string(_last_applied)},
                  {"remaining_log_entries", std::to_string(_log.size())}});

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
    // Called without _mutex held (check_heartbeat_timeout released it).
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state != kythira::server_state::leader) {
            return;
        }
        _logger.debug("Sending heartbeats/replication to followers",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"term", std::to_string(_current_term)},
                       {"commit_index", std::to_string(_commit_index)}});
    }

    // replicate_to_followers() manages its own locking around blocking I/O.
    replicate_to_followers();

    _metrics.set_metric_name("raft_heartbeat_sent");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_one();
    _metrics.emit();
}

template<raft_types Types>
auto node<Types>::send_heartbeat_with_retry(node_id_type target) -> void {
    // Create a lambda that sends the heartbeat (AppendEntries with empty entries)
    auto send_heartbeat_operation =
        [this, target, stop_flag = _stop_flag]() -> kythira::Future<append_entries_response_type> {
        if (stop_flag->load(std::memory_order_acquire)) {
            return kythira::FutureFactory::makeExceptionalFuture<append_entries_response_type>(
                std::runtime_error("node stopped"));
        }
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
            _commit_index};

        _logger.debug("Sending heartbeat with retry",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"target", node_id_to_string(target)},
                       {"term", std::to_string(_current_term)},
                       {"prev_log_index", std::to_string(prev_log_index)},
                       {"leader_commit", std::to_string(_commit_index)}});

        // Send RPC with timeout
        auto timeout = _config.append_entries_timeout();
        return _network_client.send_append_entries(target, request, timeout);
    };

    // Wrap the operation with error handler for exponential backoff retry
    auto start_time = std::chrono::steady_clock::now();

    _append_entries_error_handler
        .execute_with_retry("heartbeat", send_heartbeat_operation,
                            std::nullopt  // Use default heartbeat retry policy
                            )
        .thenTry([this, target, start_time,
                  stop_flag = _stop_flag](kythira::Try<append_entries_response_type> try_response) {
            if (stop_flag->load(std::memory_order_acquire)) {
                return;
            }
            std::lock_guard<std::mutex> lock(_mutex);

            // Calculate RPC latency
            auto end_time = std::chrono::steady_clock::now();
            auto latency =
                std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

            _metrics.set_metric_name("raft_heartbeat_latency");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("target", node_id_to_string(target));
            _metrics.add_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(latency));
            _metrics.emit();

            if (try_response.hasException()) {
                // Heartbeat failed after all retries - log error and mark follower as unresponsive
                _logger.warning("Heartbeat failed after retries",
                                {{"node_id", node_id_to_string(_node_id)},
                                 {"target", node_id_to_string(target)},
                                 {"latency_ms", std::to_string(latency.count())}});

                _unresponsive_followers.insert(target);

                _metrics.set_metric_name("raft_heartbeat_failed");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_dimension("target", node_id_to_string(target));
                _metrics.add_one();
                _metrics.emit();

                // Req 13.3 — track consecutive heartbeat failures for quorum assessment
                if (_quorum_check_active) {
                    auto& count = _heartbeat_failure_counts[target];
                    ++count;
                    if (count >= _config.quorum_heartbeat_failure_threshold()) {
                        _quorum_immediate_check = true;
                    }
                }

                return;
            }

            auto response = try_response.value();

            // Check if we've been deposed
            if (response.term() > _current_term) {
                _logger.info("Discovered higher term in heartbeat response, stepping down",
                             {{"node_id", node_id_to_string(_node_id)},
                              {"old_term", std::to_string(_current_term)},
                              {"new_term", std::to_string(response.term())}});

                become_follower(response.term());
                return;
            }

            // Only process response if we're still leader in the same term
            if (_state != kythira::server_state::leader || response.term() != _current_term) {
                return;
            }

            if (response.success()) {
                // Heartbeat succeeded - remove from unresponsive set and reset failure counter
                _unresponsive_followers.erase(target);
                _heartbeat_failure_counts.erase(target);

                _logger.debug("Heartbeat succeeded",
                              {{"node_id", node_id_to_string(_node_id)},
                               {"target", node_id_to_string(target)},
                               {"latency_ms", std::to_string(latency.count())}});

                _metrics.set_metric_name("raft_heartbeat_success");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_dimension("target", node_id_to_string(target));
                _metrics.add_one();
                _metrics.emit();
            } else {
                // Heartbeat failed due to log inconsistency - will be handled by normal replication
                _logger.debug("Heartbeat failed due to log inconsistency",
                              {{"node_id", node_id_to_string(_node_id)},
                               {"target", node_id_to_string(target)}});
            }
        });
}

template<raft_types Types>

auto node<Types>::become_follower(term_id_type new_term) -> void {
    auto old_state = _state;
    auto old_term = _current_term;

    _logger.info("Transitioning to follower", {{"node_id", node_id_to_string(_node_id)},
                                               {"old_term", std::to_string(_current_term)},
                                               {"new_term", std::to_string(new_term)}});

    // If we were a leader, cancel all pending client operations
    if (old_state == kythira::server_state::leader) {
        _logger.info("Leadership lost, cancelling pending operations",
                     {{"node_id", node_id_to_string(_node_id)},
                      {"old_term", std::to_string(old_term)},
                      {"new_term", std::to_string(new_term)},
                      {"pending_count", std::to_string(_commit_waiter.get_pending_count())}});

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

    // Stop quorum assessment loop if we were leader (Req 13.2)
    if (old_state == kythira::server_state::leader) {
        stop_quorum_loop();
    }

    // Reset election timer
    reset_election_timer();
    randomize_election_timeout();
}

template<raft_types Types>

auto node<Types>::become_candidate() -> void {
    auto old_state = _state;

    _logger.info("Transitioning to candidate and starting election",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"old_term", std::to_string(_current_term)},
                  {"new_term", std::to_string(_current_term + 1)}});

    // Increment current term
    _current_term = _current_term + 1;
    _state = kythira::server_state::candidate;

    // Vote for self
    _voted_for = _node_id;

    // Persist state before sending RequestVote RPCs
    _persistence.save_current_term(_current_term);
    _persistence.save_voted_for(_node_id);

    // Stop quorum assessment loop if we were leader (Req 13.2)
    if (old_state == kythira::server_state::leader) {
        stop_quorum_loop();
    }

    // Reset election timer with new randomized timeout
    reset_election_timer();
    randomize_election_timeout();

    // start_election() is invoked by the caller after releasing _mutex.
}

template<raft_types Types>

auto node<Types>::start_election() -> void {
    // Called without _mutex held. Snapshot the shared state needed to build
    // vote requests before doing any network I/O.
    term_id_type snapshot_term;
    log_index_type snapshot_last_log_index;
    term_id_type snapshot_last_log_term;
    std::chrono::milliseconds snapshot_election_timeout;
    std::vector<node_id_type> peer_ids;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state != kythira::server_state::candidate) {
            return;  // State changed (e.g. higher-term message) before election started
        }
        snapshot_term = _current_term;
        snapshot_last_log_index = get_last_log_index();
        snapshot_last_log_term = get_last_log_term();
        snapshot_election_timeout = _election_timeout;
        for (const auto& peer_id : _configuration.nodes()) {
            if (peer_id != _node_id) {
                peer_ids.push_back(peer_id);
            }
        }
        // During joint consensus, also request votes from C_old-only nodes
        if (_configuration.is_joint_consensus() && _configuration.old_nodes()) {
            for (const auto& peer_id : *_configuration.old_nodes()) {
                if (peer_id != _node_id &&
                    std::find(peer_ids.begin(), peer_ids.end(), peer_id) == peer_ids.end()) {
                    peer_ids.push_back(peer_id);
                }
            }
        }
    }

    _logger.info("Starting election", {{"node_id", node_id_to_string(_node_id)},
                                       {"term", std::to_string(snapshot_term)}});

    // Emit election started metric
    _metrics.set_metric_name("election_started");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("term", std::to_string(snapshot_term));
    _metrics.add_one();
    _metrics.emit();

    // Configure retry policy for RequestVote RPCs
    typename error_handler<request_vote_response_type>::retry_policy vote_retry_policy{
        .initial_delay = std::chrono::milliseconds{100},
        .max_delay = std::chrono::milliseconds{3000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.1,
        .max_attempts = 3};

    // Send RequestVote RPCs to all peers with retry logic.
    // Each future is tagged with the peer_id so the callback can count per-configuration for
    // joint-consensus quorum (Raft §6).
    //
    // Use the member _request_vote_error_handler (not a local) so that delayed-retry
    // lambdas hold a `this` pointer that remains valid for the lifetime of the node.
    // A local handler would be destroyed when start_election() returns, leaving the
    // Folly-executor callbacks with a dangling `this` — causing stack corruption
    // detected by clang's SSP.
    using tagged_vote_t = std::pair<node_id_type, request_vote_response_type>;
    std::vector<kythira::Future<tagged_vote_t>> vote_futures;
    vote_futures.reserve(peer_ids.size());

    for (const auto& peer_id : peer_ids) {
        // Create RequestVote request
        request_vote_request_type vote_request{._term = snapshot_term,
                                               ._candidate_id = _node_id,
                                               ._last_log_index = snapshot_last_log_index,
                                               ._last_log_term = snapshot_last_log_term};

        // Build a tagged future: pair<peer_id, response> so the callback can count
        // votes per configuration for joint-consensus quorum.
        auto tagged_future =
            _request_vote_error_handler
                .execute_with_retry(
                    "request_vote",
                    [this, peer_id, vote_request,
                     stop_flag = _stop_flag]() -> kythira::Future<request_vote_response_type> {
                        if (stop_flag->load(std::memory_order_acquire)) {
                            return kythira::FutureFactory::makeExceptionalFuture<
                                request_vote_response_type>(std::runtime_error("node stopped"));
                        }

                        _logger.debug(
                            "Sending RequestVote RPC",
                            {{"node_id", node_id_to_string(_node_id)},
                             {"peer_id", node_id_to_string(peer_id)},
                             {"term", std::to_string(vote_request._term)},
                             {"last_log_index", std::to_string(vote_request._last_log_index)},
                             {"last_log_term", std::to_string(vote_request._last_log_term)}});

                        _metrics.set_metric_name("vote_request_sent");
                        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                        _metrics.add_dimension("peer_id", node_id_to_string(peer_id));
                        _metrics.add_one();
                        _metrics.emit();

                        return _network_client.send_request_vote(peer_id, vote_request,
                                                                 _config.rpc_timeout());
                    },
                    vote_retry_policy)
                .thenTry([this, peer_id, stop_flag = _stop_flag](
                             kythira::Try<request_vote_response_type> result) -> tagged_vote_t {
                    if (stop_flag->load(std::memory_order_acquire)) {
                        throw std::runtime_error("node stopped");
                    }
                    if (result.hasException()) {
                        _logger.warning("RequestVote RPC failed after retries",
                                        {{"node_id", node_id_to_string(_node_id)},
                                         {"peer_id", node_id_to_string(peer_id)}});

                        _metrics.set_metric_name("vote_request_failed");
                        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                        _metrics.add_dimension("peer_id", node_id_to_string(peer_id));
                        _metrics.add_one();
                        _metrics.emit();

                        std::rethrow_exception(result.exception());
                    }

                    auto response = result.value();
                    _logger.debug("Received RequestVote response",
                                  {{"node_id", node_id_to_string(_node_id)},
                                   {"peer_id", node_id_to_string(peer_id)},
                                   {"vote_granted", response.vote_granted() ? "true" : "false"},
                                   {"response_term", std::to_string(response.term())}});

                    _metrics.set_metric_name("vote_response_received");
                    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                    _metrics.add_dimension("peer_id", node_id_to_string(peer_id));
                    _metrics.add_dimension("vote_granted",
                                           response.vote_granted() ? "true" : "false");
                    _metrics.add_one();
                    _metrics.emit();

                    return {peer_id, response};
                });

        vote_futures.push_back(std::move(tagged_future));
    }

    // If there are no peers (single-node cluster), become leader immediately
    if (vote_futures.empty()) {
        _logger.info("Single-node cluster, becoming leader immediately",
                     {{"node_id", node_id_to_string(_node_id)}});
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state == kythira::server_state::candidate && _current_term == snapshot_term) {
            become_leader();
        }
        return;
    }

    // Capture configuration snapshot for joint quorum evaluation in the callback
    std::vector<node_id_type> snap_c_new;
    std::optional<std::vector<node_id_type>> snap_c_old;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        snap_c_new = _configuration.nodes();
        snap_c_old = _configuration.old_nodes();
    }

    auto current_term = snapshot_term;
    auto node_id = _node_id;

    raft_future_collector<tagged_vote_t>::collect_all_with_timeout(std::move(vote_futures),
                                                                   snapshot_election_timeout)
        .thenValue([this, current_term, node_id, snap_c_new = std::move(snap_c_new),
                    snap_c_old = std::move(snap_c_old),
                    stop_flag = _stop_flag](std::vector<kythira::Try<tagged_vote_t>> results) {
            if (stop_flag->load(std::memory_order_acquire)) {
                return;
            }
            std::lock_guard<std::mutex> lock(_mutex);

            if (_state != kythira::server_state::candidate || _current_term != current_term) {
                _logger.debug("Election outcome irrelevant, state changed",
                              {{"node_id", node_id_to_string(node_id)},
                               {"current_term", std::to_string(_current_term)},
                               {"election_term", std::to_string(current_term)}});
                return;
            }

            // Check for higher terms from any response; step down if found
            for (const auto& r : results) {
                if (r.hasValue() && r.value().second.term() > _current_term) {
                    _logger.info("Discovered higher term during election, stepping down",
                                 {{"node_id", node_id_to_string(node_id)},
                                  {"current_term", std::to_string(_current_term)},
                                  {"discovered_term", std::to_string(r.value().second.term())}});
                    become_follower(r.value().second.term());
                    _metrics.set_metric_name("election_lost");
                    _metrics.add_dimension("node_id", node_id_to_string(node_id));
                    _metrics.add_dimension("reason", "higher_term_discovered");
                    _metrics.add_one();
                    _metrics.emit();
                    return;
                }
            }

            // Joint-consensus-aware quorum check.
            // Self counts as a vote in every configuration it belongs to.
            auto has_vote_from = [&](const node_id_type& n) -> bool {
                if (n == _node_id) {
                    {
                        return true;
                    }
                }
                for (const auto& r : results) {
                    if (r.hasValue() && r.value().first == n && r.value().second.vote_granted()) {
                        return true;
                    }
                }
                return false;
            };
            auto has_quorum = [&](const std::vector<node_id_type>& cfg) -> bool {
                std::size_t cnt = 0;
                for (const auto& n : cfg) {
                    if (has_vote_from(n)) {
                        {
                            cnt++;
                        }
                    }
                }
                return cnt * 2 > cfg.size();
            };

            bool won = false;
            if (snap_c_old.has_value()) {
                won = has_quorum(snap_c_new) && has_quorum(*snap_c_old);
            } else {
                won = has_quorum(snap_c_new);
            }

            if (won) {
                _logger.info("Election won, transitioning to leader",
                             {{"node_id", node_id_to_string(node_id)},
                              {"term", std::to_string(_current_term)}});
                become_leader();
                _metrics.set_metric_name("election_won");
                _metrics.add_dimension("node_id", node_id_to_string(node_id));
                _metrics.add_dimension("term", std::to_string(_current_term));
                _metrics.add_one();
                _metrics.emit();
            } else {
                _logger.info("Election failed, insufficient votes",
                             {{"node_id", node_id_to_string(node_id)},
                              {"term", std::to_string(_current_term)}});
                _metrics.set_metric_name("election_lost");
                _metrics.add_dimension("node_id", node_id_to_string(node_id));
                _metrics.add_dimension("reason", "insufficient_votes");
                _metrics.add_one();
                _metrics.emit();
            }
        })
        .thenError(
            [this, current_term, node_id, stop_flag = _stop_flag](const std::exception_ptr& ex) {
                if (stop_flag->load(std::memory_order_acquire)) {
                    return;
                }
                std::lock_guard<std::mutex> lock(_mutex);

                try {
                    std::rethrow_exception(ex);
                } catch (const kythira::future_collection_exception& e) {
                    _logger.warning("Failed to collect vote responses",
                                    {{"node_id", node_id_to_string(node_id)},
                                     {"operation", e.get_operation()},
                                     {"failed_count", std::to_string(e.get_failed_count())}});
                    _metrics.set_metric_name("election_lost");
                    _metrics.add_dimension("node_id", node_id_to_string(node_id));
                    _metrics.add_dimension("reason", "collection_failure");
                    _metrics.add_one();
                    _metrics.emit();
                } catch (...) {
                    _logger.error("Unexpected error during election",
                                  {{"node_id", node_id_to_string(node_id)}});
                }
            });
}

template<raft_types Types>

// `.kiro/specs/raft-pre-vote/`. Only instantiated for Types bundles whose
// network_client_type satisfies network_client_with_pre_vote — see
// check_election_timeout()'s if constexpr, the sole caller. Structurally
// mirrors start_election() closely (same peer/joint-consensus snapshot
// logic, same collect_all_with_timeout usage, same quorum-counting
// closures) but never mutates _current_term/_voted_for/persistence: only a
// majority-granted outcome escalates to the real become_candidate()+
// start_election(), which perform those mutations exactly as they did
// before this feature existed.
auto node<Types>::start_pre_vote() -> void {
    // Called without _mutex held (mirrors start_election()'s own contract).
    term_id_type prospective_term;
    log_index_type snapshot_last_log_index;
    term_id_type snapshot_last_log_term;
    std::chrono::milliseconds snapshot_election_timeout;
    std::vector<node_id_type> peer_ids;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state == kythira::server_state::leader || is_learner()) {
            return;  // race: became leader, or a learner — mirrors check_election_timeout()'s own
                     // guards
        }
        // Reset the timer up front, regardless of this round's outcome —
        // otherwise a pre-vote round that doesn't win a majority would
        // leave elapsed >= _election_timeout still true, and the next
        // external check_election_timeout() tick would immediately re-fire
        // with no backoff at all.
        reset_election_timer();
        randomize_election_timeout();

        prospective_term = _current_term + 1;
        snapshot_last_log_index = get_last_log_index();
        snapshot_last_log_term = get_last_log_term();
        snapshot_election_timeout = _election_timeout;
        for (const auto& peer_id : _configuration.nodes()) {
            if (peer_id != _node_id) {
                peer_ids.push_back(peer_id);
            }
        }
        if (_configuration.is_joint_consensus() && _configuration.old_nodes()) {
            for (const auto& peer_id : *_configuration.old_nodes()) {
                if (peer_id != _node_id &&
                    std::find(peer_ids.begin(), peer_ids.end(), peer_id) == peer_ids.end()) {
                    peer_ids.push_back(peer_id);
                }
            }
        }
    }

    // Single-node cluster: no one to pre-vote against — go straight to a
    // real election (which itself becomes leader immediately when
    // vote_futures is empty).
    if (peer_ids.empty()) {
        become_candidate();
        start_election();
        return;
    }

    _logger.debug("Starting pre-vote round",
                  {{"node_id", node_id_to_string(_node_id)},
                   {"prospective_term", std::to_string(prospective_term)}});

    typename error_handler<request_pre_vote_response_type>::retry_policy pre_vote_retry_policy{
        .initial_delay = std::chrono::milliseconds{100},
        .max_delay = std::chrono::milliseconds{3000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.1,
        .max_attempts = 3};

    using tagged_pre_vote_t = std::pair<node_id_type, request_pre_vote_response_type>;
    std::vector<kythira::Future<tagged_pre_vote_t>> pre_vote_futures;
    pre_vote_futures.reserve(peer_ids.size());

    for (const auto& peer_id : peer_ids) {
        request_pre_vote_request_type pre_vote_request{._term = prospective_term,
                                                       ._candidate_id = _node_id,
                                                       ._last_log_index = snapshot_last_log_index,
                                                       ._last_log_term = snapshot_last_log_term};

        auto tagged_future =
            _request_pre_vote_error_handler
                .execute_with_retry(
                    "request_pre_vote",
                    [this, peer_id, pre_vote_request,
                     stop_flag = _stop_flag]() -> kythira::Future<request_pre_vote_response_type> {
                        if (stop_flag->load(std::memory_order_acquire)) {
                            return kythira::FutureFactory::makeExceptionalFuture<
                                request_pre_vote_response_type>(std::runtime_error("node stopped"));
                        }
                        return _network_client.send_request_pre_vote(peer_id, pre_vote_request,
                                                                     _config.rpc_timeout());
                    },
                    pre_vote_retry_policy)
                .thenTry(
                    [peer_id, stop_flag = _stop_flag](
                        kythira::Try<request_pre_vote_response_type> result) -> tagged_pre_vote_t {
                        if (stop_flag->load(std::memory_order_acquire)) {
                            throw std::runtime_error("node stopped");
                        }
                        if (result.hasException()) {
                            std::rethrow_exception(result.exception());
                        }
                        return {peer_id, result.value()};
                    });

        pre_vote_futures.push_back(std::move(tagged_future));
    }

    std::vector<node_id_type> snap_c_new;
    std::optional<std::vector<node_id_type>> snap_c_old;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        snap_c_new = _configuration.nodes();
        snap_c_old = _configuration.old_nodes();
    }

    auto node_id = _node_id;

    raft_future_collector<tagged_pre_vote_t>::collect_all_with_timeout(std::move(pre_vote_futures),
                                                                       snapshot_election_timeout)
        .thenValue([this, node_id, snap_c_new = std::move(snap_c_new),
                    snap_c_old = std::move(snap_c_old),
                    stop_flag = _stop_flag](std::vector<kythira::Try<tagged_pre_vote_t>> results) {
            if (stop_flag->load(std::memory_order_acquire)) {
                return;
            }

            // Joint-consensus-aware quorum check — identical shape to
            // start_election()'s. Self counts as a pre-vote for itself in
            // every configuration it belongs to.
            auto has_vote_from = [&](const node_id_type& n) -> bool {
                if (n == node_id) {
                    return true;
                }
                for (const auto& r : results) {
                    if (r.hasValue() && r.value().first == n && r.value().second.vote_granted()) {
                        return true;
                    }
                }
                return false;
            };
            auto has_quorum = [&](const std::vector<node_id_type>& cfg) -> bool {
                std::size_t cnt = 0;
                for (const auto& n : cfg) {
                    if (has_vote_from(n)) {
                        cnt++;
                    }
                }
                return cnt * 2 > cfg.size();
            };

            bool won = snap_c_old.has_value() ? (has_quorum(snap_c_new) && has_quorum(*snap_c_old))
                                              : has_quorum(snap_c_new);

            if (!won) {
                _logger.debug("Pre-vote round did not reach quorum, not starting a real election",
                              {{"node_id", node_id_to_string(node_id)}});
                return;
            }

            // Re-check state fresh under lock: time has passed since the
            // snapshot above (real network I/O happened), so someone else
            // may have already become leader, or we may have heard from
            // one and stepped down.
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (_state == kythira::server_state::leader) {
                    return;
                }
            }

            _logger.debug("Pre-vote round reached quorum, starting a real election",
                          {{"node_id", node_id_to_string(node_id)}});
            become_candidate();
            start_election();
        })
        .thenError([this, node_id, stop_flag = _stop_flag](const std::exception_ptr& ex) {
            if (stop_flag->load(std::memory_order_acquire)) {
                return;
            }
            try {
                std::rethrow_exception(ex);
            } catch (const kythira::future_collection_exception& e) {
                _logger.debug("Failed to collect pre-vote responses",
                              {{"node_id", node_id_to_string(node_id)},
                               {"operation", e.get_operation()},
                               {"failed_count", std::to_string(e.get_failed_count())}});
            } catch (...) {
                _logger.debug("Unexpected error during pre-vote round",
                              {{"node_id", node_id_to_string(node_id)}});
            }
        });
}

template<raft_types Types>

auto node<Types>::become_leader() -> void {
    auto old_state = _state;

    _logger.info("Transitioning to leader", {{"node_id", node_id_to_string(_node_id)},
                                             {"term", std::to_string(_current_term)}});

    _state = kythira::server_state::leader;

    // Raft §5.4.2: advance_commit_index() only ever counts an entry toward commit_index
    // directly if entry.term() == _current_term (never a prior term's entry, even once a
    // majority of nodes hold it — see advance_commit_index()'s "only commit entries from
    // current term directly" rule). Without something of its own in the log, a new leader
    // can replicate entries inherited from previous terms to every follower and still never
    // advance commit_index. Appending a no-op barrier entry here, before next_index is
    // computed below, gives the leader something in its own term to commit; once that
    // commits, the same advance_commit_index() call pulls commit_index forward past the
    // inherited entries too (the "commit index only ever moves forward" prefix property).
    log_entry_type no_op_entry{._term = _current_term,
                               ._index = get_last_log_index() + 1,
                               ._command = {},
                               ._type = entry_type::no_op};
    append_log_entry(no_op_entry);
    _persistence.append_log_entry(no_op_entry);

    // Initialize leader-specific state
    auto last_log_idx = get_last_log_index();
    auto init_peer = [&](const node_id_type& peer_id) {
        if (peer_id != _node_id) {
            _next_index[peer_id] = last_log_idx + 1;
            _match_index[peer_id] = 0;
        }
    };
    for (const auto& peer_id : _configuration.nodes()) {
        init_peer(peer_id);
    }
    // During joint consensus, also initialize tracking for C_old-only peers so they receive
    // AppendEntries and participate in quorum.
    if (_configuration.is_joint_consensus() && _configuration.old_nodes()) {
        for (const auto& peer_id : *_configuration.old_nodes()) {
            if (_next_index.find(peer_id) == _next_index.end()) {
                init_peer(peer_id);
            }
        }
    }
    // Learners also receive AppendEntries/InstallSnapshot (replicate_to_followers()
    // iterates _next_index, not _configuration.nodes()), even though they never vote
    // or count toward quorum (.kiro/specs/non-voting-nodes/requirements.md, Req 1).
    for (const auto& peer_id : _configuration.learners()) {
        init_peer(peer_id);
    }

    // Reset heartbeat timer
    _last_heartbeat = std::chrono::steady_clock::now();

    // Start quorum assessment loop (Req 13.1)
    start_quorum_loop();
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
    // Snapshot the follower list and decide send vs. snapshot while holding _mutex.
    // The actual I/O must happen WITHOUT _mutex because the Folly callbacks (inline
    // executor) re-acquire it, which would deadlock.
    struct follower_action {
        node_id_type id;
        bool needs_snapshot;
    };
    std::vector<follower_action> actions;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state != kythira::server_state::leader) {
            return;
        }

        _logger.debug("Replicating to followers",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"term", std::to_string(_current_term)},
                       {"last_log_index", std::to_string(get_last_log_index())},
                       {"commit_index", std::to_string(_commit_index)}});

        // Iterate _next_index rather than _configuration.nodes() so that nodes in C_old
        // that are being removed continue to receive AppendEntries during joint consensus.
        for (const auto& [peer_id, next_idx] : _next_index) {
            if (peer_id == _node_id) {
                continue;
            }
            auto first_log_index = _log.empty() ? log_index_type{1} : _log.front().index();
            actions.push_back({peer_id, next_idx < first_log_index});
        }
    }
    // _mutex released — safe to do blocking I/O now.

    if (actions.empty()) {
        _logger.debug("No followers to replicate to", {{"node_id", node_id_to_string(_node_id)}});
        std::lock_guard<std::mutex> lock(_mutex);
        advance_commit_index();
        return;
    }

    for (const auto& action : actions) {
        if (action.needs_snapshot) {
            _logger.debug("Follower needs snapshot", {{"node_id", node_id_to_string(_node_id)},
                                                      {"follower", node_id_to_string(action.id)}});
            send_install_snapshot_to(action.id);
        } else {
            send_append_entries_to(action.id);
        }
    }

    // Re-acquire to advance commit index after sending to all followers.
    {
        std::lock_guard<std::mutex> lock(_mutex);
        advance_commit_index();
    }

    _metrics.set_metric_name("raft_replication_round");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("num_followers", std::to_string(actions.size()));
    _metrics.add_one();
    _metrics.emit();
}

template<raft_types Types>

auto node<Types>::send_append_entries_to(node_id_type target) -> void {
    // --- Read phase: snapshot all needed state under _mutex ---
    // (May be called with _mutex already released by replicate_to_followers or with
    //  it held by the recursive retry path in the callback — the callback releases
    //  before calling us, so we always arrive here WITHOUT _mutex held.)
    log_index_type next_idx;
    log_index_type prev_log_index;
    term_id_type prev_log_term;
    std::vector<log_entry_type> entries_to_send;
    append_entries_request_type request;
    std::chrono::milliseconds timeout;
    bool need_snapshot = false;

    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_state != kythira::server_state::leader) {
            return;
        }

        next_idx = _next_index[target];
        auto last_log_idx = get_last_log_index();

        prev_log_index = next_idx - 1;
        prev_log_term = term_id_type{0};

        if (prev_log_index > 0) {
            auto prev_entry = get_log_entry(prev_log_index);
            if (prev_entry.has_value()) {
                prev_log_term = prev_entry->term();
            } else {
                _logger.debug("Previous entry not in log, switching to snapshot",
                              {{"node_id", node_id_to_string(_node_id)},
                               {"target", node_id_to_string(target)},
                               {"prev_log_index", std::to_string(prev_log_index)}});
                need_snapshot = true;
            }
        }

        if (!need_snapshot) {
            auto max_entries = _config.max_entries_per_append();
            for (log_index_type idx = next_idx;
                 idx <= last_log_idx && entries_to_send.size() < max_entries; ++idx) {
                auto entry = get_log_entry(idx);
                if (entry.has_value()) {
                    entries_to_send.push_back(entry.value());
                } else {
                    _logger.error(
                        "Log entry not found during replication",
                        {{"node_id", node_id_to_string(_node_id)}, {"index", std::to_string(idx)}});
                    break;
                }
            }

            request = append_entries_request_type{_current_term, _node_id,        prev_log_index,
                                                  prev_log_term, entries_to_send, _commit_index};

            _logger.debug("Sending AppendEntries",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"target", node_id_to_string(target)},
                           {"term", std::to_string(_current_term)},
                           {"prev_log_index", std::to_string(prev_log_index)},
                           {"prev_log_term", std::to_string(prev_log_term)},
                           {"num_entries", std::to_string(entries_to_send.size())},
                           {"leader_commit", std::to_string(_commit_index)}});

            timeout = _config.append_entries_timeout();
        }
    }
    // _mutex released — safe to do blocking I/O now.

    if (need_snapshot) {
        send_install_snapshot_to(target);
        return;
    }

    auto start_time = std::chrono::steady_clock::now();

    try {
        // Wrap the RPC call in a lambda for ErrorHandler::execute_with_retry
        auto rpc_operation = [this, target, request, timeout,
                              stop_flag =
                                  _stop_flag]() -> kythira::Future<append_entries_response_type> {
            if (stop_flag->load(std::memory_order_acquire)) {
                return kythira::FutureFactory::makeExceptionalFuture<append_entries_response_type>(
                    std::runtime_error("node stopped"));
            }
            return _network_client.send_append_entries(target, request, timeout);
        };

        // Execute with retry using ErrorHandler (exponential backoff with jitter)
        auto response_future =
            _append_entries_error_handler.execute_with_retry("append_entries", rpc_operation);

        // Handle response asynchronously using thenTry to get folly::Try<response>
        std::move(response_future)
            .thenTry([this, target, next_idx, entries_to_send, start_time,
                      stop_flag = _stop_flag](auto try_response) {
                if (stop_flag->load(std::memory_order_acquire)) {
                    return;
                }
                std::unique_lock<std::mutex> lock(_mutex);

                // Calculate RPC latency (including retry delays)
                auto end_time = std::chrono::steady_clock::now();
                auto latency =
                    std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

                _metrics.set_metric_name("raft_append_entries_latency");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_dimension("target", node_id_to_string(target));
                _metrics.add_duration(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(latency));
                _metrics.emit();

                if (try_response.hasException()) {
                    // RPC failed after all retry attempts - log error and mark follower as
                    // unresponsive
                    try {
                        std::rethrow_exception(try_response.exception());
                    } catch (const std::exception& e) {
                        auto error_msg = std::format(
                            "AppendEntries RPC failed after retries: node_id={}, target={}, "
                            "error={}",
                            node_id_to_string(_node_id), node_id_to_string(target), e.what());
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
                        "Discovered higher term in AppendEntries response, stepping down: "
                        "node_id={}, old_term={}, new_term={}",
                        node_id_to_string(_node_id), _current_term, response.term());
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

                    _logger.debug("AppendEntries succeeded",
                                  {{"node_id", node_id_to_string(_node_id)},
                                   {"target", node_id_to_string(target)},
                                   {"match_index", std::to_string(new_match_index)},
                                   {"next_index", std::to_string(_next_index[target])}});

                    _metrics.set_metric_name("raft_entries_replicated");
                    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                    _metrics.add_dimension("target", node_id_to_string(target));
                    _metrics.add_count(entries_to_send.size());
                    _metrics.emit();

                    // Try to advance commit index
                    advance_commit_index();
                } else {
                    // Failure - decrement next_index and retry
                    _logger.debug("AppendEntries rejected, decrementing next_index",
                                  {{"node_id", node_id_to_string(_node_id)},
                                   {"target", node_id_to_string(target)},
                                   {"old_next_index", std::to_string(_next_index[target])}});

                    // Use conflict information if available for faster backtracking
                    if (response.conflict_index().has_value()) {
                        _next_index[target] = response.conflict_index().value();
                    } else {
                        // Decrement by 1 (conservative approach)
                        if (_next_index[target] > 1) {
                            _next_index[target]--;
                        }
                    }

                    _logger.debug("Retrying with new next_index",
                                  {{"node_id", node_id_to_string(_node_id)},
                                   {"target", node_id_to_string(target)},
                                   {"new_next_index", std::to_string(_next_index[target])}});

                    // Retry immediately — release lock first (send_append_entries_to acquires its
                    // own)
                    lock.unlock();
                    send_append_entries_to(target);
                }
            });

    } catch (const std::exception& e) {
        _logger.error("Exception sending AppendEntries", {{"node_id", node_id_to_string(_node_id)},
                                                          {"target", node_id_to_string(target)},
                                                          {"error", e.what()}});

        _unresponsive_followers.insert(target);
    }
}

template<raft_types Types>

auto node<Types>::send_install_snapshot_to(node_id_type target) -> void {
    // Only leaders send InstallSnapshot
    if (_state != kythira::server_state::leader) {
        return;
    }

    _logger.info("Sending InstallSnapshot to follower", {{"node_id", node_id_to_string(_node_id)},
                                                         {"target", node_id_to_string(target)},
                                                         {"term", std::to_string(_current_term)}});

    // Load snapshot from persistence
    auto snapshot_opt = _persistence.load_snapshot();
    if (!snapshot_opt.has_value()) {
        _logger.error("No snapshot available to send", {{"node_id", node_id_to_string(_node_id)},
                                                        {"target", node_id_to_string(target)}});
        return;
    }

    auto& snap = snapshot_opt.value();
    const auto& snapshot_data = snap.state_machine_state();
    auto chunk_size = _config.snapshot_chunk_size();

    _logger.debug("Snapshot details",
                  {{"node_id", node_id_to_string(_node_id)},
                   {"last_included_index", std::to_string(snap.last_included_index())},
                   {"last_included_term", std::to_string(snap.last_included_term())},
                   {"snapshot_size", std::to_string(snapshot_data.size())},
                   {"chunk_size", std::to_string(chunk_size)}});

    // Send snapshot in chunks
    std::size_t offset = 0;
    std::size_t total_chunks = (snapshot_data.size() + chunk_size - 1) / chunk_size;
    std::size_t chunk_num = 0;

    while (offset < snapshot_data.size()) {
        // Calculate chunk size for this iteration
        auto remaining = snapshot_data.size() - offset;
        auto current_chunk_size = std::min(remaining, chunk_size);

        // Extract chunk data
        std::vector<std::byte> chunk_data(snapshot_data.begin() + offset,
                                          snapshot_data.begin() + offset + current_chunk_size);

        // Determine if this is the last chunk
        bool is_last_chunk = (offset + current_chunk_size >= snapshot_data.size());

        // Create InstallSnapshot request
        install_snapshot_request_type request{_current_term,
                                              _node_id,
                                              snap.last_included_index(),
                                              snap.last_included_term(),
                                              offset,
                                              chunk_data,
                                              is_last_chunk};

        _logger.debug("Sending snapshot chunk", {{"node_id", node_id_to_string(_node_id)},
                                                 {"target", node_id_to_string(target)},
                                                 {"chunk", std::to_string(chunk_num + 1)},
                                                 {"total_chunks", std::to_string(total_chunks)},
                                                 {"offset", std::to_string(offset)},
                                                 {"chunk_size", std::to_string(current_chunk_size)},
                                                 {"is_last", is_last_chunk ? "true" : "false"}});

        // Send RPC with timeout and retry logic using ErrorHandler
        auto timeout = _config.install_snapshot_timeout();
        auto start_time = std::chrono::steady_clock::now();

        try {
            // Wrap the RPC call in a lambda for ErrorHandler::execute_with_retry
            auto rpc_operation = [this, target, request,
                                  timeout]() -> kythira::Future<install_snapshot_response_type> {
                return _network_client.send_install_snapshot(target, request, timeout);
            };

            // Execute with retry using ErrorHandler (exponential backoff with jitter)
            // For snapshots, use longer delays and more attempts due to larger data transfers
            auto response_future = _install_snapshot_error_handler.execute_with_retry(
                "install_snapshot", rpc_operation);

            // Wait for response synchronously (snapshot transfer is sequential)
            // Wrap in try-catch to handle exceptions
            install_snapshot_response_type response;
            try {
                response = std::move(response_future).get();
            } catch (const std::exception& e) {
                _logger.error(
                    std::format("InstallSnapshot RPC failed after retries: node_id={}, target={}, "
                                "chunk={}/{}, offset={}, error={}",
                                node_id_to_string(_node_id), node_id_to_string(target),
                                chunk_num + 1, total_chunks, offset, e.what()));

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
            auto latency =
                std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

            _metrics.set_metric_name("raft_install_snapshot_chunk_latency");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_dimension("target", node_id_to_string(target));
            _metrics.add_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(latency));
            _metrics.emit();

            // Check if we've been deposed
            if (response.term() > _current_term) {
                auto msg = std::format(
                    "Discovered higher term in InstallSnapshot response, stepping down: "
                    "node_id={}, old_term={}, new_term={}",
                    node_id_to_string(_node_id), _current_term, response.term());
                _logger.info(msg);

                become_follower(response.term());
                return;
            }

            // Only continue if we're still leader in the same term
            if (_state != kythira::server_state::leader || response.term() != _current_term) {
                return;
            }

            _logger.debug("Snapshot chunk sent successfully",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"target", node_id_to_string(target)},
                           {"chunk", std::to_string(chunk_num + 1)}});

        } catch (const std::exception& e) {
            _logger.error("Exception sending InstallSnapshot chunk",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"target", node_id_to_string(target)},
                           {"chunk", std::to_string(chunk_num + 1)},
                           {"total_chunks", std::to_string(total_chunks)},
                           {"offset", std::to_string(offset)},
                           {"error", e.what()}});

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

    _logger.info("InstallSnapshot completed successfully",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"target", node_id_to_string(target)},
                  {"last_included_index", std::to_string(snap.last_included_index())},
                  {"total_chunks", std::to_string(total_chunks)},
                  {"total_bytes", std::to_string(snapshot_data.size())}});

    _metrics.set_metric_name("raft_install_snapshot_success");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_dimension("target", node_id_to_string(target));
    _metrics.add_value(static_cast<double>(snapshot_data.size()));
    _metrics.emit();
}

// ============================================================================
// Peer-to-peer catch-up (.kiro/specs/peer2peer-log-replication/)
// ============================================================================

template<raft_types Types>

auto node<Types>::handle_fetch_log_entries(const fetch_log_entries_request_type& request)
    -> fetch_log_entries_response_type {
    std::lock_guard<std::mutex> lock(_mutex);

    _logger.debug("Received FetchLogEntries RPC",
                  {{"node_id", node_id_to_string(_node_id)},
                   {"requester_id", node_id_to_string(request.requester_id())},
                   {"from_index", std::to_string(request.from_index())},
                   {"to_index", std::to_string(request.to_index())}});

    // Requirement 5.4: available=false if this node doesn't itself have an entry
    // at from_index (already compacted into a snapshot, or simply doesn't have it
    // yet) — never blocks waiting to acquire it, never recurses into its own
    // peer2peer_replicator on the requester's behalf.
    auto from_entry = get_log_entry(request.from_index());
    if (!from_entry.has_value()) {
        _logger.debug("FetchLogEntries: entry not available",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"from_index", std::to_string(request.from_index())}});
        return fetch_log_entries_response_type{
            node_id_to_u64(_node_id), false, term_id_type{0}, {}};
    }

    term_id_type prev_log_term{0};
    if (request.from_index() > 1) {
        auto prev_entry = get_log_entry(request.from_index() - 1);
        if (prev_entry.has_value()) {
            prev_log_term = prev_entry->term();
        }
        // else: the entry immediately before from_index was already compacted into
        // a snapshot whose term this node doesn't separately track here — falls
        // back to term 0, which will simply fail the requester's consistency check
        // (Requirement 6.2) and be treated as a failed catch-up attempt
        // (Requirement 7.1), never a safety issue (design.md Property 3).
    }

    // Requirement 5.5: never serve more than min(to_index, get_last_log_index()).
    auto last_index_to_serve = std::min(request.to_index(), get_last_log_index());
    std::vector<log_entry_type> entries_to_send;
    for (log_index_type idx = request.from_index(); idx <= last_index_to_serve; ++idx) {
        auto entry = get_log_entry(idx);
        if (!entry.has_value()) {
            break;
        }
        entries_to_send.push_back(entry.value());
    }

    _logger.debug("FetchLogEntries: serving entries",
                  {{"node_id", node_id_to_string(_node_id)},
                   {"requester_id", node_id_to_string(request.requester_id())},
                   {"count", std::to_string(entries_to_send.size())}});

    return fetch_log_entries_response_type{node_id_to_u64(_node_id), true, prev_log_term,
                                           std::move(entries_to_send)};
}

template<raft_types Types>

auto node<Types>::maybe_gossip_progress() -> void {
    // Requirement 3.2: advertised regardless of server_state (leader, candidate,
    // or follower) — a lagging leader that just lost an election still has
    // entries other nodes may need.
    address_type self_address;
    term_id_type term;
    log_index_type last_log_index;
    node_id_type self_id;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto now = std::chrono::steady_clock::now();
        // Requirement 3.1: gated by progress_gossip_interval so this does not
        // fire on every maintenance-thread tick.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_progress_gossip) <
            _config.progress_gossip_interval()) {
            return;
        }
        _last_progress_gossip = now;
        self_address = _self_address;
        term = _current_term;
        last_log_index = get_last_log_index();
        self_id = _node_id;
    }

    // Requirement 3.3: best-effort, fire-and-forget — a transport error here must
    // never block or delay any Raft-critical operation. Captures _stop_flag per
    // this project's async-callback convention (guards against dangling `this`
    // if the node is destroyed while the future is still outstanding).
    auto stop_flag = _stop_flag;
    _peer2peer_replicator.advertise_progress(self_id, self_address, term, last_log_index)
        .thenTry([this, stop_flag](auto try_result) {
            if (stop_flag->load(std::memory_order_acquire)) {
                return;
            }
            if (try_result.hasException()) {
                this->_logger.debug("Failed to advertise progress via peer2peer_replicator",
                                    {{"node_id", node_id_to_string(this->_node_id)}});
            }
        });
}

template<raft_types Types>

auto node<Types>::maybe_catch_up_from_peer() -> void {
    log_index_type from_index;
    log_index_type to_index;
    std::chrono::milliseconds timeout;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // Requirement 4.3: a leader never triggers catch-up fetches for itself —
        // it is, by definition of _commit_index's quorum requirement, never the
        // node furthest behind.
        if (_state == kythira::server_state::leader) {
            return;
        }
        // Requirement 4.5: at most one outstanding peer-to-peer fetch at a time.
        if (_catch_up_in_flight) {
            return;
        }
        from_index = get_last_log_index() + 1;
        auto max_entries = static_cast<log_index_type>(_config.catch_up_fetch_max_entries());
        to_index = max_entries > 0 ? from_index + max_entries - 1 : from_index;
        timeout = _config.catch_up_fetch_timeout();
        _catch_up_in_flight = true;
    }

    _metrics.set_metric_name("raft_peer_catch_up_attempt");
    _metrics.add_dimension("node_id", node_id_to_string(_node_id));
    _metrics.add_one();
    _metrics.emit();

    auto stop_flag = _stop_flag;
    _peer2peer_replicator.find_catch_up_source(from_index, to_index, timeout)
        .thenTry([this, stop_flag, from_index, to_index, timeout](auto try_result) {
            if (stop_flag->load(std::memory_order_acquire)) {
                return;
            }

            // Requirement 4.4/7.1: no source known (including always, for
            // no_op_peer2peer_replicator) — no-op this tick, re-evaluated next tick.
            if (try_result.hasException() || !try_result.value().has_value()) {
                std::lock_guard<std::mutex> lock(this->_mutex);
                this->_catch_up_in_flight = false;
                return;
            }

            auto source = try_result.value().value();

            if constexpr (!network_client_with_log_fetch<network_client_type>) {
                // Requirement 5.3: transport doesn't support fetch_log_entries —
                // peer-to-peer catch-up is unreachable regardless of
                // peer2peer_replicator_type configuration.
                std::lock_guard<std::mutex> lock(this->_mutex);
                this->_catch_up_in_flight = false;
            } else {
                fetch_log_entries_request_type request{._requester_id = this->_node_id,
                                                       ._from_index = from_index,
                                                       ._to_index = to_index};

                this->_logger.debug("Attempting peer-to-peer catch-up fetch",
                                    {{"node_id", node_id_to_string(this->_node_id)},
                                     {"source_peer", node_id_to_string(source.node_id)},
                                     {"from_index", std::to_string(from_index)},
                                     {"to_index", std::to_string(to_index)}});

                const auto& stop_flag2 = stop_flag;
                this->_network_client
                    .send_fetch_log_entries(node_id_to_u64(source.node_id), request, timeout)
                    .thenTry([this, stop_flag2, from_index, source](auto resp_try) {
                        if (stop_flag2->load(std::memory_order_acquire)) {
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lock(this->_mutex);
                            this->_catch_up_in_flight = false;
                        }

                        // Requirement 7.1: RPC failure — logged, no-op this tick.
                        if (resp_try.hasException()) {
                            this->_logger.debug(
                                "Peer-to-peer catch-up fetch failed",
                                {{"node_id", node_id_to_string(this->_node_id)},
                                 {"source_peer", node_id_to_string(source.node_id)}});
                            return;
                        }

                        auto response = resp_try.value();
                        if (!response.available()) {
                            this->_logger.debug(
                                "Peer-to-peer catch-up source unavailable",
                                {{"node_id", node_id_to_string(this->_node_id)},
                                 {"source_peer", node_id_to_string(source.node_id)}});
                            return;
                        }

                        // Requirement 6.2: apply exactly as if this were an AppendEntries
                        // payload from prev_log_index = from_index - 1 — same
                        // consistency-check-and-truncate guarantees as leader-driven
                        // replication (Requirement 6.1). Never touches _commit_index,
                        // _known_leader, or the election timer (Requirement 6.3).
                        append_entries_response_type consistency_response;
                        log_index_type new_last_index{};
                        {
                            std::lock_guard<std::mutex> lock(this->_mutex);
                            consistency_response = this->append_entries_with_consistency_check(
                                from_index - 1, response.prev_log_term(), response.entries());
                            new_last_index = this->get_last_log_index();
                        }

                        if (consistency_response.success()) {
                            this->_metrics.set_metric_name("raft_peer_catch_up_success");
                            this->_metrics.add_dimension("node_id",
                                                         node_id_to_string(this->_node_id));
                            this->_metrics.add_dimension("source_peer",
                                                         node_id_to_string(source.node_id));
                            this->_metrics.add_one();
                            this->_metrics.emit();
                            this->_logger.debug(
                                "Peer-to-peer catch-up succeeded",
                                {{"node_id", node_id_to_string(this->_node_id)},
                                 {"source_peer", node_id_to_string(source.node_id)},
                                 {"last_log_index", std::to_string(new_last_index)}});
                        } else {
                            // Requirement 6.2/7.1: consistency check failed — the fetched
                            // batch was discarded entirely, treated as a failed catch-up
                            // attempt (design.md Property 3: never a safety issue).
                            this->_logger.debug(
                                "Peer-to-peer catch-up consistency check failed",
                                {{"node_id", node_id_to_string(this->_node_id)},
                                 {"source_peer", node_id_to_string(source.node_id)}});
                        }
                    });
            }
        });
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

    // Joint-consensus-aware quorum check.
    // During joint consensus, an entry must achieve majority in BOTH C_old and C_new.
    auto has_commit_quorum = [&](log_index_type n) -> bool {
        auto count_acks = [&](const std::vector<node_id_type>& node_set) -> std::size_t {
            std::size_t cnt = 0;
            for (const auto& nid : node_set) {
                if (nid == _node_id) {
                    cnt++;  // leader self-ack
                    continue;
                }
                if (auto it = _match_index.find(nid); it != _match_index.end() && it->second >= n) {
                    cnt++;
                }
            }
            return cnt;
        };
        if (_configuration.is_joint_consensus() && _configuration.old_nodes()) {
            const auto& c_old = *_configuration.old_nodes();
            const auto& c_new = _configuration.nodes();
            return count_acks(c_old) * 2 > c_old.size() && count_acks(c_new) * 2 > c_new.size();
        }
        const auto& s = _configuration.nodes();
        return count_acks(s) * 2 > s.size();
    };

    for (log_index_type n = _commit_index + 1; n <= last_log_idx; ++n) {
        if (has_commit_quorum(n)) {
            // Raft safety requirement: only commit entries from current term directly
            // Entries from previous terms are committed indirectly
            auto entry_opt = get_log_entry(n);
            if (entry_opt.has_value() && entry_opt->term() == _current_term) {
                // Advance commit index
                auto old_commit_index = _commit_index;
                _commit_index = n;

                _logger.info("Advanced commit index",
                             {{"node_id", node_id_to_string(_node_id)},
                              {"old_commit_index", std::to_string(old_commit_index)},
                              {"new_commit_index", std::to_string(_commit_index)}});

                // Emit commit metric
                _metrics.set_metric_name("entries_committed");
                _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                _metrics.add_count(_commit_index - old_commit_index);
                _metrics.emit();

                // Trigger state machine application
                // This will also notify the commit waiter for each applied entry
                apply_committed_entries();
            }
            // else: entry from a previous term — keep scanning; a current-term entry
            // at a higher index may still satisfy quorum and pull this one along.
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
                _logger.warning("Follower is lagging significantly",
                                {{"node_id", node_id_to_string(_node_id)},
                                 {"follower_id", node_id_to_string(peer_id)},
                                 {"follower_match_index", std::to_string(match_idx)},
                                 {"commit_index", std::to_string(_commit_index)},
                                 {"lag", std::to_string(_commit_index - match_idx)}});

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
        _logger.info("Applied index catchup needed",
                     {{"node_id", node_id_to_string(_node_id)},
                      {"last_applied", std::to_string(_last_applied)},
                      {"commit_index", std::to_string(_commit_index)},
                      {"lag", std::to_string(lag)}});

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

    _logger.debug("Starting entry application",
                  {{"node_id", node_id_to_string(_node_id)},
                   {"entries_to_apply", std::to_string(entries_to_apply)},
                   {"total_lag", std::to_string(lag)},
                   {"rate_limited", should_rate_limit ? "true" : "false"}});

    auto catchup_start_time = std::chrono::steady_clock::now();
    log_index_type entries_applied_in_batch = 0;

    // Apply all entries from last_applied + 1 to commit_index (or batch limit)
    while (_last_applied < _commit_index && entries_applied_in_batch < entries_to_apply) {
        auto next_index = _last_applied + 1;

        // Get the log entry to apply
        auto entry_opt = get_log_entry(next_index);
        if (!entry_opt.has_value()) {
            _logger.error("Failed to get log entry for application",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"entry_index", std::to_string(next_index)},
                           {"last_applied", std::to_string(_last_applied)},
                           {"commit_index", std::to_string(_commit_index)}});
            break;
        }

        auto& entry = entry_opt.value();

        // No-op barrier entries (Raft §5.4.2) are NOT applied to the state machine; their
        // sole purpose is to occupy a slot in the leader's own term so that
        // advance_commit_index()'s "only commit entries from current term directly" rule
        // has something to commit, which then pulls commit_index forward past any entries
        // inherited from previous terms.
        if (entry.type() == entry_type::no_op) {
            _commit_waiter.notify_committed_and_applied(next_index);
            _last_applied = next_index;
            entries_applied_in_batch++;
            continue;
        }

        // Configuration entries are NOT applied to the state machine; they advance the
        // configuration change protocol instead.
        if (entry.type() == entry_type::configuration) {
            auto new_config = deserialize_configuration<node_id_type>(entry.command());
            _configuration = new_config;
            sync_peer2peer_membership();
            _config_synchronizer.notify_configuration_committed(new_config, entry.index());
            // add_learner()/remove_learner() register on _commit_waiter (not
            // _config_synchronizer, which only tracks the joint/final voting-set
            // phases) — this is a no-op when nothing is pending for this index.
            _commit_waiter.notify_committed_and_applied(next_index);
            _last_applied = next_index;
            entries_applied_in_batch++;

            _logger.info("Applied configuration entry",
                         {{"node_id", node_id_to_string(_node_id)},
                          {"entry_index", std::to_string(entry.index())},
                          {"is_joint", new_config.is_joint_consensus() ? "true" : "false"}});

            if (new_config.is_joint_consensus() && _state == kythira::server_state::leader) {
                // Leader immediately appends C_new to start the second phase.
                // Learners are preserved unchanged — the joint phase only affects the voting set.
                cluster_configuration_type c_new{new_config.nodes(), false, std::nullopt,
                                                 new_config.learners()};
                auto c_new_bytes = serialize_configuration<node_id_type>(c_new);
                log_entry_type c_new_entry{._term = _current_term,
                                           ._index = get_last_log_index() + 1,
                                           ._command = std::move(c_new_bytes),
                                           ._type = entry_type::configuration};
                append_log_entry(c_new_entry);
                _persistence.append_log_entry(c_new_entry);
                _logger.info("Appended C_new entry after joint commit",
                             {{"node_id", node_id_to_string(_node_id)},
                              {"c_new_index", std::to_string(c_new_entry.index())}});
            }

            if (!new_config.is_joint_consensus()) {
                // C_new (or a plain learner-only) commit: remove peers that are no
                // longer tracked at all — checked against BOTH the voting set and the
                // learner set, so add_learner()'s freshly-admitted learner (present only
                // in learners(), not nodes()) is not immediately erased here.
                for (auto it = _next_index.begin(); it != _next_index.end();) {
                    bool in_new =
                        std::find(new_config.nodes().begin(), new_config.nodes().end(),
                                  it->first) != new_config.nodes().end() ||
                        std::find(new_config.learners().begin(), new_config.learners().end(),
                                  it->first) != new_config.learners().end();
                    if (!in_new && it->first != _node_id) {
                        _match_index.erase(it->first);
                        it = _next_index.erase(it);
                    } else {
                        ++it;
                    }
                }
                // Leader self-removal: step down after C_new commits.
                bool self_in_new = std::find(new_config.nodes().begin(), new_config.nodes().end(),
                                             _node_id) != new_config.nodes().end();
                if (!self_in_new && _state == kythira::server_state::leader) {
                    _logger.info("Leader removed from C_new, stepping down",
                                 {{"node_id", node_id_to_string(_node_id)}});
                    become_follower(_current_term);
                }
            }
            continue;
        }

        _logger.debug("Applying log entry to state machine",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"entry_index", std::to_string(entry.index())},
                       {"entry_term", std::to_string(entry.term())},
                       {"command_size", std::to_string(entry.command().size())}});

        auto start_time = std::chrono::steady_clock::now();

        try {
            // Apply entry to state machine and capture the result
            auto result = _state_machine.apply(entry.command(), entry.index());

            // Update last_applied index after successful application
            _last_applied = next_index;
            entries_applied_in_batch++;

            auto end_time = std::chrono::steady_clock::now();
            auto duration =
                std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

            _logger.debug("Successfully applied log entry",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"entry_index", std::to_string(entry.index())},
                           {"last_applied", std::to_string(_last_applied)},
                           {"application_time_us", std::to_string(duration.count())}});

            // Emit application metric
            _metrics.set_metric_name("entry_applied");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_duration(duration);
            _metrics.emit();

            // Notify CommitWaiter that this entry has been committed and applied
            // This will fulfill any pending futures waiting for this entry
            // Use shared_ptr to allow the lambda to be called multiple times
            auto shared_result = std::make_shared<std::vector<std::byte>>(std::move(result));
            _commit_waiter.notify_committed_and_applied(
                next_index, [shared_result](log_index_type) { return *shared_result; });

            // Emit commit-to-application latency metric
            _metrics.set_metric_name("commit_to_application_latency");
            _metrics.add_dimension("node_id", node_id_to_string(_node_id));
            _metrics.add_duration(duration);
            _metrics.emit();

        } catch (const std::exception& e) {
            _logger.error("Failed to apply log entry to state machine",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"entry_index", std::to_string(entry.index())},
                           {"entry_term", std::to_string(entry.term())},
                           {"command_size", std::to_string(entry.command().size())},
                           {"error", e.what()},
                           {"error_type", typeid(e).name()}});

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
                _logger.warning("Halting entry application due to failure (policy: halt)",
                                {{"node_id", node_id_to_string(_node_id)},
                                 {"failed_entry_index", std::to_string(entry.index())},
                                 {"last_applied", std::to_string(_last_applied)}});

                // Propagate error to pending futures
                auto exception_ptr = std::current_exception();
                _commit_waiter.notify_committed_and_applied(
                    next_index, [exception_ptr](log_index_type) -> std::vector<std::byte> {
                        std::rethrow_exception(exception_ptr);
                    });

                // Stop applying further entries
                break;
            }
            if (policy == application_failure_policy::retry) {
                // Retry: Attempt to retry application with exponential backoff
                _logger.warning(
                    "Retrying entry application due to failure (policy: retry)",
                    {{"node_id", node_id_to_string(_node_id)},
                     {"failed_entry_index", std::to_string(entry.index())},
                     {"max_attempts", std::to_string(_config.application_retry_max_attempts())}});

                bool retry_succeeded = false;
                auto delay = _config.application_retry_initial_delay();

                for (std::size_t attempt = 1; attempt <= _config.application_retry_max_attempts();
                     ++attempt) {
                    _logger.debug("Retry attempt for failed entry",
                                  {{"node_id", node_id_to_string(_node_id)},
                                   {"entry_index", std::to_string(entry.index())},
                                   {"attempt", std::to_string(attempt)},
                                   {"delay_ms", std::to_string(delay.count())}});

                    // Wait before retrying
                    std::this_thread::sleep_for(delay);

                    try {
                        // Retry applying entry to state machine and capture the result
                        auto result = _state_machine.apply(entry.command(), entry.index());

                        // Update last_applied index after successful retry
                        _last_applied = next_index;
                        retry_succeeded = true;

                        _logger.info("Successfully applied entry after retry",
                                     {{"node_id", node_id_to_string(_node_id)},
                                      {"entry_index", std::to_string(entry.index())},
                                      {"attempt", std::to_string(attempt)}});

                        // Notify CommitWaiter of successful application
                        // Use shared_ptr to allow the lambda to be called multiple times
                        auto shared_result =
                            std::make_shared<std::vector<std::byte>>(std::move(result));
                        _commit_waiter.notify_committed_and_applied(
                            next_index, [shared_result](log_index_type) { return *shared_result; });

                        // Emit retry success metric
                        _metrics.set_metric_name("entry_application_retry_success");
                        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
                        _metrics.add_dimension("entry_index", std::to_string(entry.index()));
                        _metrics.add_dimension("attempts", std::to_string(attempt));
                        _metrics.add_one();
                        _metrics.emit();

                        break;

                    } catch (const std::exception& retry_error) {
                        _logger.warning("Retry attempt failed",
                                        {{"node_id", node_id_to_string(_node_id)},
                                         {"entry_index", std::to_string(entry.index())},
                                         {"attempt", std::to_string(attempt)},
                                         {"error", retry_error.what()}});

                        // Calculate next delay with exponential backoff
                        delay = std::chrono::milliseconds(static_cast<long long>(
                            delay.count() * _config.application_retry_backoff_multiplier()));
                        delay = std::min(delay, _config.application_retry_max_delay());
                    }
                }

                if (!retry_succeeded) {
                    _logger.error(
                        "All retry attempts exhausted for entry application",
                        {{"node_id", node_id_to_string(_node_id)},
                         {"entry_index", std::to_string(entry.index())},
                         {"attempts", std::to_string(_config.application_retry_max_attempts())}});

                    // Propagate error to pending futures
                    auto exception_ptr = std::current_exception();
                    _commit_waiter.notify_committed_and_applied(
                        next_index, [exception_ptr](log_index_type) -> std::vector<std::byte> {
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
                _logger.warning("Skipping failed entry and continuing (policy: skip - DANGEROUS)",
                                {{"node_id", node_id_to_string(_node_id)},
                                 {"skipped_entry_index", std::to_string(entry.index())},
                                 {"last_applied", std::to_string(_last_applied)}});

                // Update last_applied to skip this entry
                _last_applied = next_index;

                // Propagate error to pending futures for this entry
                auto exception_ptr = std::current_exception();
                _commit_waiter.notify_committed_and_applied(
                    next_index, [exception_ptr](log_index_type) -> std::vector<std::byte> {
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
        catchup_end_time - catchup_start_time);

    if (entries_applied_in_batch > 0) {
        auto remaining_lag = _commit_index - _last_applied;

        _logger.info("Applied index catchup batch completed",
                     {{"node_id", node_id_to_string(_node_id)},
                      {"entries_applied", std::to_string(entries_applied_in_batch)},
                      {"last_applied", std::to_string(_last_applied)},
                      {"commit_index", std::to_string(_commit_index)},
                      {"remaining_lag", std::to_string(remaining_lag)},
                      {"duration_ms", std::to_string(catchup_duration.count())}});

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

    _logger.info("Creating snapshot from current state machine state",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"last_applied", std::to_string(_last_applied)}});

    auto start_time = std::chrono::steady_clock::now();

    // Capture current state from state machine
    auto state = _state_machine.get_state();

    _logger.debug("State machine state captured", {{"node_id", node_id_to_string(_node_id)},
                                                   {"state_size", std::to_string(state.size())}});

    // Get the term of the last applied entry
    term_id_type last_applied_term = term_id_type{0};
    if (_last_applied > 0) {
        auto last_entry = get_log_entry(_last_applied);
        if (last_entry.has_value()) {
            last_applied_term = last_entry->term();
        }
    }

    // Create snapshot with captured state
    snapshot_type snap{_last_applied, last_applied_term, _configuration, state};

    // Persist snapshot to storage
    _persistence.save_snapshot(snap);

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    _logger.info("Snapshot created successfully",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"last_included_index", std::to_string(snap.last_included_index())},
                  {"last_included_term", std::to_string(snap.last_included_term())},
                  {"state_size", std::to_string(state.size())},
                  {"duration_ms", std::to_string(duration.count())}});

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

    _logger.info("Creating snapshot with provided state",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"last_applied", std::to_string(_last_applied)},
                  {"state_size", std::to_string(state_machine_state.size())}});

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
    snapshot_type snap{_last_applied, last_applied_term, _configuration, state_machine_state};

    // Persist snapshot to storage
    _persistence.save_snapshot(snap);

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    _logger.info("Snapshot created successfully",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"last_included_index", std::to_string(snap.last_included_index())},
                  {"last_included_term", std::to_string(snap.last_included_term())},
                  {"snapshot_size", std::to_string(state_machine_state.size())},
                  {"duration_ms", std::to_string(duration.count())}});

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
        _logger.debug("No snapshot available for log compaction",
                      {{"node_id", node_id_to_string(_node_id)}});
        return;
    }

    auto& snap = snapshot_opt.value();
    auto last_included_index = snap.last_included_index();

    _logger.info("Compacting log", {{"node_id", node_id_to_string(_node_id)},
                                    {"last_included_index", std::to_string(last_included_index)},
                                    {"current_log_size", std::to_string(_log.size())}});

    if (_log.empty()) {
        _logger.debug("Log already empty, no compaction needed",
                      {{"node_id", node_id_to_string(_node_id)}});
        return;
    }

    auto first_log_index = _log.front().index();

    // Check if we have entries to compact
    if (last_included_index < first_log_index) {
        _logger.debug("Snapshot index is before log start, no compaction needed",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"last_included_index", std::to_string(last_included_index)},
                       {"first_log_index", std::to_string(first_log_index)}});
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
        _logger.debug("No entries to compact", {{"node_id", node_id_to_string(_node_id)}});
        return;
    }

    auto old_log_size = _log.size();

    // Remove entries up to and including last_included_index
    _log.erase(_log.begin(), _log.begin() + entries_to_remove);

    // Delete entries from persistence
    _persistence.delete_log_entries_before(last_included_index + 1);

    auto new_log_size = _log.size();
    auto entries_removed = old_log_size - new_log_size;

    _logger.info("Log compaction completed",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"entries_removed", std::to_string(entries_removed)},
                  {"old_log_size", std::to_string(old_log_size)},
                  {"new_log_size", std::to_string(new_log_size)},
                  {"last_included_index", std::to_string(last_included_index)}});

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

    _logger.info("Installing snapshot to state machine",
                 {{"node_id", node_id_to_string(_node_id)},
                  {"last_included_index", std::to_string(snap.last_included_index())},
                  {"last_included_term", std::to_string(snap.last_included_term())},
                  {"snapshot_size", std::to_string(snap.state_machine_state().size())}});

    try {
        // Restore state machine from snapshot
        _state_machine.restore_from_snapshot(snap.state_machine_state(),
                                             snap.last_included_index());

        // Restore cluster configuration from the snapshot
        _configuration = snap.configuration();
        sync_peer2peer_membership();

        // Update last_applied to snapshot's last_included_index
        _last_applied = snap.last_included_index();

        // Update commit_index if snapshot index is higher
        if (snap.last_included_index() > _commit_index) {
            _commit_index = snap.last_included_index();

            _logger.debug("Updated commit_index from snapshot",
                          {{"node_id", node_id_to_string(_node_id)},
                           {"new_commit_index", std::to_string(_commit_index)}});
        }

        // Truncate log based on snapshot's last_included_index
        // Remove all entries up to and including the snapshot's last_included_index
        if (!_log.empty() && snap.last_included_index() > 0) {
            auto entries_to_remove =
                std::min(static_cast<std::size_t>(snap.last_included_index()), _log.size());

            if (entries_to_remove > 0) {
                _log.erase(_log.begin(), _log.begin() + entries_to_remove);

                _logger.debug("Truncated log after snapshot installation",
                              {{"node_id", node_id_to_string(_node_id)},
                               {"entries_removed", std::to_string(entries_to_remove)},
                               {"remaining_entries", std::to_string(_log.size())}});
            }
        }

        auto end_time = std::chrono::steady_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        _logger.info("Successfully installed snapshot to state machine",
                     {{"node_id", node_id_to_string(_node_id)},
                      {"last_applied", std::to_string(_last_applied)},
                      {"commit_index", std::to_string(_commit_index)},
                      {"remaining_log_entries", std::to_string(_log.size())},
                      {"duration_ms", std::to_string(duration.count())}});

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
        _logger.error("Failed to install snapshot to state machine",
                      {{"node_id", node_id_to_string(_node_id)},
                       {"error", e.what()},
                       {"last_included_index", std::to_string(snap.last_included_index())}});

        _metrics.set_metric_name("raft_snapshot_installation_failure");
        _metrics.add_dimension("node_id", node_id_to_string(_node_id));
        _metrics.add_dimension("error_type", "state_machine_error");
        _metrics.add_one();
        _metrics.emit();

        // Re-throw the exception to propagate the error
        throw;
    }
}

// ============================================================================
// Bootstrap helpers
// ============================================================================

template<raft_types Types> auto node<Types>::is_fresh_node() -> bool {
    return _current_term == 0 && !_voted_for.has_value() &&
           _persistence.get_last_log_index() == 0 && !_persistence.load_snapshot().has_value();
}

template<raft_types Types>
auto node<Types>::bootstrap_retry_interval() const -> std::chrono::milliseconds {
    if constexpr (requires(const configuration_type& c) {
                      { c.bootstrap_retry_interval() } -> std::same_as<std::chrono::milliseconds>;
                  }) {
        return _config.bootstrap_retry_interval();
    } else {
        return std::chrono::milliseconds{5000};
    }
}

template<raft_types Types>
auto node<Types>::bootstrap_peer_find_timeout() const -> std::chrono::milliseconds {
    if constexpr (requires(const configuration_type& c) {
                      {
                          c.bootstrap_peer_find_timeout()
                      } -> std::same_as<std::chrono::milliseconds>;
                  }) {
        return _config.bootstrap_peer_find_timeout();
    } else {
        return std::chrono::milliseconds{2000};
    }
}

template<raft_types Types>
auto node<Types>::handle_cluster_join(const cluster_join_request_type& req)
    -> cluster_join_response_type {
    bool is_leader_now = false;
    std::optional<peer_info<node_id_type, address_type>> redirect;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        is_leader_now = (_state == kythira::server_state::leader);
        if (!is_leader_now && _known_leader.has_value()) {
            redirect = peer_info<node_id_type, address_type>{
                *_known_leader, address_type{node_id_to_string(*_known_leader)}};
        }
    }

    if (is_leader_now) {
        // Fire-and-forget: add_learner() acquires its own lock, so call it after
        // releasing ours. The joining node is admitted as a learner (non-voting) —
        // it replicates and catches up before an operator or the leader's automatic
        // quorum-maintenance policy promotes it via promote_to_voter(). This avoids
        // the availability gap of a brand-new, empty node counting toward quorum
        // before it has replicated anything (.kiro/specs/non-voting-nodes/).
        // We discard the future — the log-append side-effect already occurred.
        auto fut = add_learner(req.joining_node_id());
        (void)fut;
        return cluster_join_response_type{true, std::nullopt};
    }

    return cluster_join_response_type{false, redirect};
}

template<raft_types Types>
auto node<Types>::handle_cluster_leave(const cluster_leave_request_type& req)
    -> cluster_leave_response_type {
    bool is_leader_now = false;
    std::optional<peer_info<node_id_type, address_type>> redirect;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        is_leader_now = (_state == kythira::server_state::leader);
        if (!is_leader_now && _known_leader.has_value()) {
            redirect = peer_info<node_id_type, address_type>{
                *_known_leader, address_type{node_id_to_string(*_known_leader)}};
        }
    }

    if (is_leader_now) {
        // Fire-and-forget: remove_server() acquires its own lock.
        auto fut = remove_server(req.leaving_node_id());
        (void)fut;
        return cluster_leave_response_type{true, std::nullopt};
    }

    return cluster_leave_response_type{false, redirect};
}

template<raft_types Types> auto node<Types>::run_bootstrap() -> void {
    while (!_stop_requested.load(std::memory_order_acquire)) {
        auto peers = _peer_discovery.find_peers(bootstrap_peer_find_timeout()).get();

        if (peers.empty()) {
            // No peers → found a single-node cluster (existing behaviour)
            _logger.info("Bootstrap: no peers found, founding single-node cluster",
                         {{"node_id", node_id_to_string(_node_id)}});
            return;
        }

        _logger.info("Bootstrap: attempting to join cluster",
                     {{"node_id", node_id_to_string(_node_id)},
                      {"peer_count", std::to_string(peers.size())}});

        if constexpr (network_client_with_cluster_join<network_client_type>) {
            bool joined = false;
            auto timeout = bootstrap_peer_find_timeout();

            for (const auto& peer : peers) {
                if (_stop_requested.load(std::memory_order_acquire)) {
                    return;
                }

                auto try_join = [&](const address_type& addr) -> bool {
                    try {
                        cluster_join_request_type req;
                        req.node_id = _node_id;
                        req.contact_address = _self_address;

                        auto resp =
                            _network_client.send_cluster_join_request(addr, req, timeout).get();

                        if (resp.is_accepted()) {
                            _logger.info("Bootstrap: join accepted",
                                         {{"node_id", node_id_to_string(_node_id)},
                                          {"via", std::string(addr)}});
                            return true;
                        }

                        // Redirect: try the leader directly, once
                        if (resp.redirect_peer().has_value()) {
                            const auto& redir = *resp.redirect_peer();
                            _logger.debug("Bootstrap: redirected to leader",
                                          {{"node_id", node_id_to_string(_node_id)},
                                           {"leader", std::string(redir.address)}});
                            try {
                                auto resp2 =
                                    _network_client
                                        .send_cluster_join_request(redir.address, req, timeout)
                                        .get();
                                if (resp2.is_accepted()) {
                                    _logger.info("Bootstrap: join accepted via redirect",
                                                 {{"node_id", node_id_to_string(_node_id)}});
                                    return true;
                                }
                            } catch (...) {
                                // redirect target unreachable
                            }
                        }
                    } catch (...) {
                        // peer unreachable
                    }
                    return false;
                };

                joined = try_join(peer.address);
                if (joined) {
                    return;
                }
            }
        }

        // All peers exhausted — wait and retry
        _logger.debug("Bootstrap: all peers exhausted, waiting before retry",
                      {{"node_id", node_id_to_string(_node_id)}});

        auto deadline = std::chrono::steady_clock::now() + bootstrap_retry_interval();
        while (std::chrono::steady_clock::now() < deadline) {
            if (_stop_requested.load(std::memory_order_acquire)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
}

template<raft_types Types>
auto node<Types>::leave_cluster(std::chrono::milliseconds timeout) -> void {
    if constexpr (!network_client_with_cluster_leave<network_client_type>) {
        return;
    } else {
        std::optional<address_type> leader_addr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_known_leader.has_value()) {
                leader_addr = address_type{node_id_to_string(*_known_leader)};
            }
        }

        if (!leader_addr.has_value()) {
            _logger.warning("leave_cluster: no known leader, cannot send ClusterLeave",
                            {{"node_id", node_id_to_string(_node_id)}});
            return;
        }

        cluster_leave_request_type req;
        req.node_id = _node_id;

        try {
            auto resp =
                _network_client.send_cluster_leave_request(*leader_addr, req, timeout).get();

            if (resp.is_accepted()) {
                _logger.info("leave_cluster: accepted by leader",
                             {{"node_id", node_id_to_string(_node_id)}});
                return;
            }

            // Leader redirected us — try again with the real leader
            if (resp.redirect_peer().has_value()) {
                const auto& redir = *resp.redirect_peer();
                _logger.debug("leave_cluster: redirected to leader",
                              {{"node_id", node_id_to_string(_node_id)},
                               {"leader", std::string(redir.address)}});
                auto resp2 =
                    _network_client.send_cluster_leave_request(redir.address, req, timeout).get();
                if (resp2.is_accepted()) {
                    _logger.info("leave_cluster: accepted via redirect",
                                 {{"node_id", node_id_to_string(_node_id)}});
                }
            }
        } catch (const std::exception& e) {
            _logger.warning("leave_cluster: RPC failed",
                            {{"node_id", node_id_to_string(_node_id)}, {"error", e.what()}});
        }
    }
}

template<raft_types Types>
auto node<Types>::update_peer_addresses(
    const std::vector<peer_info<node_id_type, address_type>>& peers) -> void {
    if constexpr (requires(network_client_type& c, node_id_type id, address_type a) {
                      { c.update_peer_address(id, a) } -> std::same_as<void>;
                  }) {
        for (const auto& p : peers) {
            _network_client.update_peer_address(p.node_id, p.address);
        }
    }
}

template<raft_types Types> auto node<Types>::run_reconnect() -> void {
    // Restarting-node reconnection loop.  Waits for the first peer contact; if
    // no contact arrives within isolation_timeout, calls find_peers() and updates
    // the routing table, then sleeps and repeats.  Exits on peer contact or stop().

    auto isolation_timeout = std::chrono::milliseconds{_config.election_timeout_max().count() * 2};

    while (!_stop_requested.load(std::memory_order_acquire)) {
        // Wait for peer contact or timeout
        {
            std::unique_lock<std::mutex> lk(_reconnect_cv_mutex);
            if (_reconnect_cv.wait_for(lk, isolation_timeout, [this] {
                    return _known_leader.has_value() ||
                           _stop_requested.load(std::memory_order_acquire);
                })) {
                // Woken by peer contact or stop
                return;
            }
        }

        if (_stop_requested.load(std::memory_order_acquire)) {
            return;
        }

        // Timeout elapsed without peer contact — try peer discovery
        try {
            auto peers = _peer_discovery.find_peers(bootstrap_peer_find_timeout()).get();
            if (!peers.empty()) {
                _logger.info("Reconnect: updating peer addresses from discovery",
                             {{"node_id", node_id_to_string(_node_id)},
                              {"peer_count", std::to_string(peers.size())}});
                update_peer_addresses(peers);
            }
        } catch (...) {
            // ignore discovery errors; retry on next cycle
        }

        // Sleep before next check (honour stop signal promptly)
        auto deadline = std::chrono::steady_clock::now() + bootstrap_retry_interval();
        while (std::chrono::steady_clock::now() < deadline) {
            if (_stop_requested.load(std::memory_order_acquire)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
}

}  // namespace kythira