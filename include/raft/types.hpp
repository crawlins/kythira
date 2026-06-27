#pragma once

/// @file types.hpp
/// @brief Core types, concepts, and default implementations for the Kythira Raft library.

#include <concepts/future.hpp>
#include <raft/metrics.hpp>
#include <raft/peer_discovery.hpp>
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

/// @brief Concept for a node identifier: any unsigned integer or `std::string`.
template<typename T>
concept node_id = std::unsigned_integral<T> || std::same_as<T, std::string>;

/// @brief Concept for a term number: a monotonically increasing unsigned integer.
template<typename T>
concept term_id = std::unsigned_integral<T>;

/// @brief Concept for a log index: a 1-based, monotonically increasing unsigned integer.
template<typename T>
concept log_index = std::unsigned_integral<T>;

// Forward declaration of test state machine (after log_index concept is defined)
template<typename LogIndex>
requires log_index<LogIndex>
class test_key_value_state_machine;

/// @brief Consensus role of a Raft server.
enum class server_state : std::uint8_t {
    follower,   ///< Passive replication target; converts to candidate on election timeout.
    candidate,  ///< Soliciting votes to become leader.
    leader,     ///< Drives replication; sends periodic heartbeats.
};

/// @brief Stream insertion operator for `server_state`.
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

/// @brief Discriminant distinguishing normal state-machine entries from configuration entries.
enum class entry_type : std::uint8_t {
    normal = 0,        ///< Application command to be applied to the state machine.
    configuration = 1  ///< Joint-consensus or final cluster-configuration record.
};

/// @brief Concept for a log entry: carries term, index, command bytes, and entry type.
/// @tparam T        Concrete log-entry type.
/// @tparam TermId   Must satisfy `term_id`.
/// @tparam LogIndex Must satisfy `log_index`.
template<typename T, typename TermId, typename LogIndex>
concept log_entry_type = requires(const T& entry) {
    requires term_id<TermId>;
    requires log_index<LogIndex>;
    { entry.term() } -> std::same_as<TermId>;
    { entry.index() } -> std::same_as<LogIndex>;
    { entry.command() } -> std::same_as<const std::vector<std::byte>&>;
    { entry.type() } -> std::same_as<entry_type>;
};

/// @brief Default log-entry implementation.
/// @tparam TermId   Term number type; defaults to `uint64_t`.
/// @tparam LogIndex Log index type; defaults to `uint64_t`.
template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
requires term_id<TermId> && log_index<LogIndex>
struct log_entry {
    TermId _term;
    LogIndex _index;
    std::vector<std::byte> _command;
    entry_type _type = entry_type::normal;

    [[nodiscard]] auto term() const -> TermId { return _term; }
    [[nodiscard]] auto index() const -> LogIndex { return _index; }
    [[nodiscard]] auto command() const -> const std::vector<std::byte>& { return _command; }
    [[nodiscard]] auto type() const -> entry_type { return _type; }
};

/// @brief Concept for a cluster configuration: a set of node IDs optionally in joint consensus.
/// @tparam T      Concrete configuration type.
/// @tparam NodeId Must satisfy `node_id`.
template<typename T, typename NodeId>
concept cluster_configuration_type = requires(const T& config) {
    requires node_id<NodeId>;
    { config.nodes() } -> std::same_as<const std::vector<NodeId>&>;
    { config.is_joint_consensus() } -> std::convertible_to<bool>;
    { config.old_nodes() } -> std::same_as<const std::optional<std::vector<NodeId>>&>;
};

/// @brief Default cluster-configuration implementation.
///
/// During a membership change, `_is_joint_consensus` is `true` and `_old_nodes`
/// holds the previous node set so that a majority of both old and new configurations
/// is required for a quorum.
///
/// @tparam NodeId Node identifier type; defaults to `uint64_t`.
template<typename NodeId = std::uint64_t>
requires node_id<NodeId>
struct cluster_configuration {
    std::vector<NodeId> _nodes;
    bool _is_joint_consensus;
    std::optional<std::vector<NodeId>> _old_nodes;

    [[nodiscard]] auto nodes() const -> const std::vector<NodeId>& { return _nodes; }
    [[nodiscard]] auto is_joint_consensus() const -> bool { return _is_joint_consensus; }
    [[nodiscard]] auto old_nodes() const -> const std::optional<std::vector<NodeId>>& {
        return _old_nodes;
    }
};

/// @brief Concept for a Raft snapshot: captures state-machine state and the log prefix it replaces.
/// @tparam T        Concrete snapshot type.
/// @tparam NodeId   Must satisfy `node_id`.
/// @tparam TermId   Must satisfy `term_id`.
/// @tparam LogIndex Must satisfy `log_index`.
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

/// @brief Concept for an application state machine.
///
/// Implementations must be deterministic: given the same sequence of commands
/// starting from the same snapshot, every node must produce identical state.
///
/// @tparam SM       Concrete state machine type.
/// @tparam LogIndex Must satisfy `log_index`.
template<typename SM, typename LogIndex>
concept state_machine = requires(SM& sm, const SM& const_sm, const std::vector<std::byte>& command,
                                 const std::vector<std::byte>& snapshot_data, LogIndex index) {
    requires log_index<LogIndex>;

    /// Apply a committed log entry. Returns the result bytes (may be empty).
    { sm.apply(command, index) } -> std::same_as<std::vector<std::byte>>;

    /// Serialize the full state machine state for snapshotting.
    { const_sm.get_state() } -> std::same_as<std::vector<std::byte>>;

    /// Replace the current state with a previously serialized snapshot.
    { sm.restore_from_snapshot(snapshot_data, index) } -> std::same_as<void>;
};

/// @brief Default snapshot implementation.
/// @tparam NodeId   Node identifier type; defaults to `uint64_t`.
/// @tparam TermId   Term number type; defaults to `uint64_t`.
/// @tparam LogIndex Log index type; defaults to `uint64_t`.
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
struct snapshot {
    LogIndex _last_included_index;
    TermId _last_included_term;
    cluster_configuration<NodeId> _configuration;
    std::vector<std::byte> _state_machine_state;

    [[nodiscard]] auto last_included_index() const -> LogIndex { return _last_included_index; }
    [[nodiscard]] auto last_included_term() const -> TermId { return _last_included_term; }
    [[nodiscard]] auto configuration() const -> const cluster_configuration<NodeId>& {
        return _configuration;
    }
    [[nodiscard]] auto state_machine_state() const -> const std::vector<std::byte>& {
        return _state_machine_state;
    }
};

/// @brief Concept for a RequestVote RPC request message.
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

/// @brief Concept for a RequestVote RPC response message.
template<typename T, typename TermId>
concept request_vote_response_type = requires(const T& resp) {
    requires term_id<TermId>;
    { resp.term() } -> std::same_as<TermId>;
    { resp.vote_granted() } -> std::convertible_to<bool>;
};

/// @brief Concept for an AppendEntries RPC request message.
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

/// @brief Concept for an AppendEntries RPC response message.
template<typename T, typename TermId, typename LogIndex>
concept append_entries_response_type = requires(const T& resp) {
    requires term_id<TermId>;
    requires log_index<LogIndex>;
    { resp.term() } -> std::same_as<TermId>;
    { resp.success() } -> std::convertible_to<bool>;
    { resp.conflict_index() } -> std::same_as<std::optional<LogIndex>>;
    { resp.conflict_term() } -> std::same_as<std::optional<TermId>>;
};

/// @brief Concept for an InstallSnapshot RPC request message.
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

/// @brief Concept for an InstallSnapshot RPC response message.
template<typename T, typename TermId>
concept install_snapshot_response_type = requires(const T& resp) {
    requires term_id<TermId>;
    { resp.term() } -> std::same_as<TermId>;
};

/// @brief Default RequestVote request.
/// @tparam NodeId   Node identifier type; defaults to `uint64_t`.
/// @tparam TermId   Term number type; defaults to `uint64_t`.
/// @tparam LogIndex Log index type; defaults to `uint64_t`.
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
struct request_vote_request {
    TermId _term;
    NodeId _candidate_id;
    LogIndex _last_log_index;
    TermId _last_log_term;

    [[nodiscard]] auto term() const -> TermId { return _term; }
    [[nodiscard]] auto candidate_id() const -> NodeId { return _candidate_id; }
    [[nodiscard]] auto last_log_index() const -> LogIndex { return _last_log_index; }
    [[nodiscard]] auto last_log_term() const -> TermId { return _last_log_term; }
};

/// @brief Default RequestVote response.
/// @tparam TermId Term number type; defaults to `uint64_t`.
template<typename TermId = std::uint64_t>
requires term_id<TermId>
struct request_vote_response {
    TermId _term;
    bool _vote_granted;

    [[nodiscard]] auto term() const -> TermId { return _term; }
    [[nodiscard]] auto vote_granted() const -> bool { return _vote_granted; }
};

/// @brief Default AppendEntries request.
/// @tparam NodeId   Node identifier type; defaults to `uint64_t`.
/// @tparam TermId   Term number type; defaults to `uint64_t`.
/// @tparam LogIndex Log index type; defaults to `uint64_t`.
/// @tparam LogEntry Log-entry type; defaults to `log_entry<TermId, LogIndex>`.
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex> &&
         log_entry_type<LogEntry, TermId, LogIndex>
struct append_entries_request {
    TermId _term;
    NodeId _leader_id;
    LogIndex _prev_log_index;
    TermId _prev_log_term;
    std::vector<LogEntry> _entries;
    LogIndex _leader_commit;

    [[nodiscard]] auto term() const -> TermId { return _term; }
    [[nodiscard]] auto leader_id() const -> NodeId { return _leader_id; }
    [[nodiscard]] auto prev_log_index() const -> LogIndex { return _prev_log_index; }
    [[nodiscard]] auto prev_log_term() const -> TermId { return _prev_log_term; }
    [[nodiscard]] auto entries() const -> const std::vector<LogEntry>& { return _entries; }
    [[nodiscard]] auto leader_commit() const -> LogIndex { return _leader_commit; }
};

/// @brief Default AppendEntries response.
/// @tparam TermId   Term number type; defaults to `uint64_t`.
/// @tparam LogIndex Log index type; defaults to `uint64_t`.
template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
requires term_id<TermId> && log_index<LogIndex>
struct append_entries_response {
    TermId _term;
    bool _success;
    std::optional<LogIndex> _conflict_index;
    std::optional<TermId> _conflict_term;

    [[nodiscard]] auto term() const -> TermId { return _term; }
    [[nodiscard]] auto success() const -> bool { return _success; }
    [[nodiscard]] auto conflict_index() const -> std::optional<LogIndex> { return _conflict_index; }
    [[nodiscard]] auto conflict_term() const -> std::optional<TermId> { return _conflict_term; }
};

/// @brief Default InstallSnapshot request.
/// @tparam NodeId   Node identifier type; defaults to `uint64_t`.
/// @tparam TermId   Term number type; defaults to `uint64_t`.
/// @tparam LogIndex Log index type; defaults to `uint64_t`.
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
struct install_snapshot_request {
    TermId _term;
    NodeId _leader_id;
    LogIndex _last_included_index;
    TermId _last_included_term;
    std::size_t _offset;
    std::vector<std::byte> _data;
    bool _done;

    [[nodiscard]] auto term() const -> TermId { return _term; }
    [[nodiscard]] auto leader_id() const -> NodeId { return _leader_id; }
    [[nodiscard]] auto last_included_index() const -> LogIndex { return _last_included_index; }
    [[nodiscard]] auto last_included_term() const -> TermId { return _last_included_term; }
    [[nodiscard]] auto offset() const -> std::size_t { return _offset; }
    [[nodiscard]] auto data() const -> const std::vector<std::byte>& { return _data; }
    [[nodiscard]] auto done() const -> bool { return _done; }
};

/// @brief Default InstallSnapshot response.
/// @tparam TermId Term number type; defaults to `uint64_t`.
template<typename TermId = std::uint64_t>
requires term_id<TermId>
struct install_snapshot_response {
    TermId _term;

    [[nodiscard]] auto term() const -> TermId { return _term; }
};

/// @brief Concept constraining the serialized-data container to a `std::byte` range.
template<typename T>
concept serialized_data =
    std::ranges::range<T> && std::same_as<std::ranges::range_value_t<T>, std::byte>;

/// @brief Concept for an RPC serializer.
/// @tparam S    Serializer type.
/// @tparam Data Must satisfy `serialized_data`.
template<typename S, typename Data>
concept rpc_serializer = requires {
    requires serialized_data<Data>;
    typename S;
};

/// @brief Per-operation retry policy with exponential back-off and jitter.
struct retry_policy_config {
    std::chrono::milliseconds initial_delay{100};  ///< Delay before the first retry.
    std::chrono::milliseconds max_delay{5000};     ///< Maximum delay between retries.
    double backoff_multiplier{2.0};                ///< Multiplier applied after each attempt.
    double jitter_factor{0.1};    ///< Fraction of the delay added as random jitter.
    std::size_t max_attempts{5};  ///< Total number of attempts (including the first).

    /// @brief Returns `true` when all fields are in valid ranges.
    [[nodiscard]] auto is_valid() const -> bool {
        return initial_delay > std::chrono::milliseconds{0} && max_delay >= initial_delay &&
               backoff_multiplier > 1.0 && jitter_factor >= 0.0 && jitter_factor <= 1.0 &&
               max_attempts > 0;
    }
};

/// @brief Configuration for the adaptive RPC timeout mechanism.
struct adaptive_timeout_config {
    bool enabled{false};                           ///< Enable adaptive timeout adjustment.
    std::chrono::milliseconds min_timeout{50};     ///< Floor for the adaptive timeout.
    std::chrono::milliseconds max_timeout{10000};  ///< Ceiling for the adaptive timeout.
    double adaptation_factor{1.2};                 ///< Growth factor when timeout is exceeded.
    std::size_t sample_window_size{10};            ///< Number of samples used for adaptation.

    /// @brief Returns `true` when all fields are in valid ranges.
    [[nodiscard]] auto is_valid() const -> bool {
        return min_timeout > std::chrono::milliseconds{0} && max_timeout >= min_timeout &&
               adaptation_factor > 1.0 && sample_window_size > 0;
    }
};

/// @brief Concept for a Raft node configuration object.
///
/// Provides all timing, retry, snapshot, and quorum-management parameters
/// consumed by `node<Types>`.
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
    { config.quorum_check_interval() } -> std::same_as<std::chrono::milliseconds>;
    { config.quorum_heartbeat_failure_threshold() } -> std::same_as<std::size_t>;
};

/// @brief Policy applied when the state machine's `apply()` call throws.
enum class application_failure_policy : std::uint8_t {
    halt,   ///< Stop applying further entries (safe default).
    retry,  ///< Retry application with exponential back-off.
    skip    ///< Skip the failed entry and continue — risks inconsistency.
};

/// @brief Default Raft node configuration with conservative production-ready defaults.
struct raft_configuration {
    // ── Timing ────────────────────────────────────────────────────────────────
    std::chrono::milliseconds _election_timeout_min{150};  ///< Minimum randomised election timeout.
    std::chrono::milliseconds _election_timeout_max{300};  ///< Maximum randomised election timeout.
    std::chrono::milliseconds _heartbeat_interval{50};     ///< Leader heartbeat cadence.
    std::chrono::milliseconds _rpc_timeout{100};           ///< Default RPC deadline.

    // ── Per-RPC timeouts ──────────────────────────────────────────────────────
    std::chrono::milliseconds _append_entries_timeout{5000};     ///< AppendEntries RPC deadline.
    std::chrono::milliseconds _request_vote_timeout{2000};       ///< RequestVote RPC deadline.
    std::chrono::milliseconds _install_snapshot_timeout{30000};  ///< InstallSnapshot RPC deadline.

    // ── Log and snapshot ──────────────────────────────────────────────────────
    std::size_t _max_entries_per_append{100};  ///< Maximum entries in one AppendEntries RPC.
    std::size_t _snapshot_threshold_bytes{10'000'000};  ///< Log size that triggers snapshotting.
    std::size_t _snapshot_chunk_size{1'000'000};  ///< Chunk size for InstallSnapshot transfers.

    // ── Retry policies ────────────────────────────────────────────────────────
    retry_policy_config _heartbeat_retry_policy{.initial_delay = std::chrono::milliseconds{50},
                                                .max_delay = std::chrono::milliseconds{1000},
                                                .backoff_multiplier = 1.5,
                                                .jitter_factor = 0.1,
                                                .max_attempts = 3};

    retry_policy_config _append_entries_retry_policy{
        .initial_delay = std::chrono::milliseconds{100},
        .max_delay = std::chrono::milliseconds{5000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.1,
        .max_attempts = 5};

    retry_policy_config _request_vote_retry_policy{.initial_delay = std::chrono::milliseconds{100},
                                                   .max_delay = std::chrono::milliseconds{2000},
                                                   .backoff_multiplier = 2.0,
                                                   .jitter_factor = 0.1,
                                                   .max_attempts = 3};

    retry_policy_config _install_snapshot_retry_policy{
        .initial_delay = std::chrono::milliseconds{500},
        .max_delay = std::chrono::milliseconds{30000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.1,
        .max_attempts = 10};

    adaptive_timeout_config _adaptive_timeout_config{
        .enabled = false,
        .min_timeout = std::chrono::milliseconds{50},
        .max_timeout = std::chrono::milliseconds{10000},
        .adaptation_factor = 1.2,
        .sample_window_size = 10};

    // ── Bootstrap timing ──────────────────────────────────────────────────────
    std::chrono::milliseconds _bootstrap_retry_interval{5000};  ///< Peer-find retry cadence.
    std::chrono::milliseconds _bootstrap_peer_find_timeout{
        2000};  ///< Per-attempt peer-find deadline.

    // ── Quorum management ─────────────────────────────────────────────────────
    /// How often the leader proactively calls `assess_quorum`.
    std::chrono::milliseconds _quorum_check_interval{30000};
    /// Consecutive heartbeat failures to a single peer that triggers an immediate
    /// out-of-cycle `assess_quorum` call.
    std::size_t _quorum_heartbeat_failure_threshold{3};

    // ── Application-failure policy ────────────────────────────────────────────
    application_failure_policy _application_failure_policy{application_failure_policy::halt};
    std::size_t _application_retry_max_attempts{3};
    std::chrono::milliseconds _application_retry_initial_delay{100};
    std::chrono::milliseconds _application_retry_max_delay{5000};
    double _application_retry_backoff_multiplier{2.0};

    [[nodiscard]] auto bootstrap_retry_interval() const -> std::chrono::milliseconds {
        return _bootstrap_retry_interval;
    }
    [[nodiscard]] auto bootstrap_peer_find_timeout() const -> std::chrono::milliseconds {
        return _bootstrap_peer_find_timeout;
    }
    [[nodiscard]] auto quorum_check_interval() const -> std::chrono::milliseconds {
        return _quorum_check_interval;
    }
    [[nodiscard]] auto quorum_heartbeat_failure_threshold() const -> std::size_t {
        return _quorum_heartbeat_failure_threshold;
    }

    [[nodiscard]] auto election_timeout_min() const -> std::chrono::milliseconds {
        return _election_timeout_min;
    }
    [[nodiscard]] auto election_timeout_max() const -> std::chrono::milliseconds {
        return _election_timeout_max;
    }
    [[nodiscard]] auto heartbeat_interval() const -> std::chrono::milliseconds {
        return _heartbeat_interval;
    }
    [[nodiscard]] auto rpc_timeout() const -> std::chrono::milliseconds { return _rpc_timeout; }
    [[nodiscard]] auto append_entries_timeout() const -> std::chrono::milliseconds {
        return _append_entries_timeout;
    }
    [[nodiscard]] auto request_vote_timeout() const -> std::chrono::milliseconds {
        return _request_vote_timeout;
    }
    [[nodiscard]] auto install_snapshot_timeout() const -> std::chrono::milliseconds {
        return _install_snapshot_timeout;
    }
    [[nodiscard]] auto max_entries_per_append() const -> std::size_t {
        return _max_entries_per_append;
    }
    [[nodiscard]] auto snapshot_threshold_bytes() const -> std::size_t {
        return _snapshot_threshold_bytes;
    }
    [[nodiscard]] auto snapshot_chunk_size() const -> std::size_t { return _snapshot_chunk_size; }
    [[nodiscard]] auto heartbeat_retry_policy() const -> const retry_policy_config& {
        return _heartbeat_retry_policy;
    }
    [[nodiscard]] auto append_entries_retry_policy() const -> const retry_policy_config& {
        return _append_entries_retry_policy;
    }
    [[nodiscard]] auto request_vote_retry_policy() const -> const retry_policy_config& {
        return _request_vote_retry_policy;
    }
    [[nodiscard]] auto install_snapshot_retry_policy() const -> const retry_policy_config& {
        return _install_snapshot_retry_policy;
    }
    [[nodiscard]] auto get_adaptive_timeout_config() const -> const adaptive_timeout_config& {
        return _adaptive_timeout_config;
    }
    [[nodiscard]] auto get_application_failure_policy() const -> application_failure_policy {
        return _application_failure_policy;
    }
    [[nodiscard]] auto application_retry_max_attempts() const -> std::size_t {
        return _application_retry_max_attempts;
    }
    [[nodiscard]] auto application_retry_initial_delay() const -> std::chrono::milliseconds {
        return _application_retry_initial_delay;
    }
    [[nodiscard]] auto application_retry_max_delay() const -> std::chrono::milliseconds {
        return _application_retry_max_delay;
    }
    [[nodiscard]] auto application_retry_backoff_multiplier() const -> double {
        return _application_retry_backoff_multiplier;
    }

    /// @brief Returns `true` iff `get_validation_errors()` is empty.
    [[nodiscard]] auto validate() const -> bool { return get_validation_errors().empty(); }

    /// @brief Returns a list of human-readable configuration constraint violations.
    [[nodiscard]] auto get_validation_errors() const -> std::vector<std::string> {
        std::vector<std::string> errors;

        if (_election_timeout_min <= std::chrono::milliseconds{0}) {
            errors.emplace_back("election_timeout_min must be positive");
        }

        if (_election_timeout_max <= _election_timeout_min) {
            errors.emplace_back("election_timeout_max must be greater than election_timeout_min");
        }

        if (_heartbeat_interval <= std::chrono::milliseconds{0}) {
            errors.emplace_back("heartbeat_interval must be positive");
        }

        if (_heartbeat_interval > _election_timeout_min / 3) {
            errors.emplace_back(
                "heartbeat_interval should be less than or equal to election_timeout_min/3 to "
                "prevent false timeouts");
        }

        if (_rpc_timeout <= std::chrono::milliseconds{0}) {
            errors.emplace_back("rpc_timeout must be positive");
        }

        if (_append_entries_timeout <= std::chrono::milliseconds{0}) {
            errors.emplace_back("append_entries_timeout must be positive");
        }

        if (_request_vote_timeout <= std::chrono::milliseconds{0}) {
            errors.emplace_back("request_vote_timeout must be positive");
        }

        if (_install_snapshot_timeout <= std::chrono::milliseconds{0}) {
            errors.emplace_back("install_snapshot_timeout must be positive");
        }

        if (!_heartbeat_retry_policy.is_valid()) {
            errors.emplace_back("heartbeat_retry_policy is invalid");
        }

        if (!_append_entries_retry_policy.is_valid()) {
            errors.emplace_back("append_entries_retry_policy is invalid");
        }

        if (!_request_vote_retry_policy.is_valid()) {
            errors.emplace_back("request_vote_retry_policy is invalid");
        }

        if (!_install_snapshot_retry_policy.is_valid()) {
            errors.emplace_back("install_snapshot_retry_policy is invalid");
        }

        if (!_adaptive_timeout_config.is_valid()) {
            errors.emplace_back("adaptive_timeout_config is invalid");
        }

        if (_max_entries_per_append == 0) {
            errors.emplace_back("max_entries_per_append must be positive");
        }

        if (_snapshot_threshold_bytes == 0) {
            errors.emplace_back("snapshot_threshold_bytes must be positive");
        }

        if (_snapshot_chunk_size == 0) {
            errors.emplace_back("snapshot_chunk_size must be positive");
        }

        if (_snapshot_chunk_size > _snapshot_threshold_bytes) {
            errors.emplace_back("snapshot_chunk_size should not exceed snapshot_threshold_bytes");
        }

        if (_quorum_check_interval <= std::chrono::milliseconds{0}) {
            errors.emplace_back("quorum_check_interval must be positive");
        }

        if (_quorum_heartbeat_failure_threshold < 1) {
            errors.emplace_back("quorum_heartbeat_failure_threshold must be >= 1");
        }

        return errors;
    }
};

// ============================================================================
// Bootstrap message types
// ============================================================================

/// @brief Sent by a new node to an existing cluster member to request admission.
/// @tparam NodeId  Joining node's identifier type; defaults to `uint64_t`.
/// @tparam Address Network address type; defaults to `std::string`.
template<typename NodeId = std::uint64_t, typename Address = std::string>
struct cluster_join_request {
    NodeId node_id;           ///< Identifier the joining node will use.
    Address contact_address;  ///< Address at which the joining node is reachable.

    [[nodiscard]] auto joining_node_id() const -> NodeId { return node_id; }
    [[nodiscard]] auto joining_address() const -> const Address& { return contact_address; }
};

/// @brief Response to a `cluster_join_request`.
/// @tparam NodeId  Node identifier type; defaults to `uint64_t`.
/// @tparam Address Network address type; defaults to `std::string`.
template<typename NodeId = std::uint64_t, typename Address = std::string>
struct cluster_join_response {
    bool accepted{false};  ///< `true` if the leader accepted the join.
    std::optional<peer_info<NodeId, Address>>
        redirect;  ///< Redirect hint when the recipient is not the leader.

    [[nodiscard]] auto is_accepted() const -> bool { return accepted; }
    [[nodiscard]] auto redirect_peer() const -> const std::optional<peer_info<NodeId, Address>>& {
        return redirect;
    }
};

/// @brief Sent by a departing node (or an admin) to the leader to initiate removal.
/// @tparam NodeId  Leaving node's identifier type; defaults to `uint64_t`.
/// @tparam Address Network address type; defaults to `std::string`.
template<typename NodeId = std::uint64_t, typename Address = std::string>
struct cluster_leave_request {
    NodeId node_id;  ///< Identifier of the node that wants to leave.

    [[nodiscard]] auto leaving_node_id() const -> NodeId { return node_id; }
};

/// @brief Response to a `cluster_leave_request`.
/// @tparam NodeId  Node identifier type; defaults to `uint64_t`.
/// @tparam Address Network address type; defaults to `std::string`.
template<typename NodeId = std::uint64_t, typename Address = std::string>
struct cluster_leave_response {
    bool accepted{false};  ///< `true` if the leader accepted the leave.
    std::optional<peer_info<NodeId, Address>>
        redirect;  ///< Redirect hint when the recipient is not the leader.

    [[nodiscard]] auto is_accepted() const -> bool { return accepted; }
    [[nodiscard]] auto redirect_peer() const -> const std::optional<peer_info<NodeId, Address>>& {
        return redirect;
    }
};

// ============================================================================
// Bootstrap type-detection helpers (preserves backwards compat with old test
// types that lack address_type / peer_discovery_type members)
// ============================================================================

template<typename T, typename NodeId>
concept _has_bootstrap_types = requires {
    typename T::address_type;
    typename T::peer_discovery_type;
};

template<typename T, typename NodeId, bool = _has_bootstrap_types<T, NodeId>>
struct _bootstrap_type_traits {
    using address_type = std::string;
    using peer_discovery_type = no_op_peer_discovery<NodeId, std::string>;
    using cluster_join_request_type = cluster_join_request<NodeId, std::string>;
    using cluster_join_response_type = cluster_join_response<NodeId, std::string>;
    using cluster_leave_request_type = cluster_leave_request<NodeId, std::string>;
    using cluster_leave_response_type = cluster_leave_response<NodeId, std::string>;
};

template<typename T, typename NodeId> struct _bootstrap_type_traits<T, NodeId, true> {
    using address_type = typename T::address_type;
    using peer_discovery_type = typename T::peer_discovery_type;
    using cluster_join_request_type = cluster_join_request<NodeId, typename T::address_type>;
    using cluster_join_response_type = cluster_join_response<NodeId, typename T::address_type>;
    using cluster_leave_request_type = cluster_leave_request<NodeId, typename T::address_type>;
    using cluster_leave_response_type = cluster_leave_response<NodeId, typename T::address_type>;
};

// ============================================================================
// Transport types concept
// ============================================================================

/// @brief Concept for a unified transport-types bundle used by HTTP and CoAP transports.
template<typename T>
concept transport_types =
    requires {
        typename T::serializer_type;
        typename T::metrics_type;
        typename T::executor_type;
    } && kythira::rpc_serializer<typename T::serializer_type, std::vector<std::byte>> &&
    kythira::metrics<typename T::metrics_type> &&
    requires {
        typename T::template future_template<kythira::request_vote_response<>>;
        typename T::template future_template<kythira::append_entries_response<>>;
        typename T::template future_template<kythira::install_snapshot_response<>>;
    } &&
    future<typename T::template future_template<kythira::request_vote_response<>>,
           kythira::request_vote_response<>> &&
    future<typename T::template future_template<kythira::append_entries_response<>>,
           kythira::append_entries_response<>> &&
    future<typename T::template future_template<kythira::install_snapshot_response<>>,
           kythira::install_snapshot_response<>>;

// ============================================================================
// Unified types template parameter system
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

/// @brief Concept that validates a complete set of Raft type parameters.
///
/// A type bundle `T` satisfies `raft_types` when it exposes every required nested
/// type alias and each alias satisfies the corresponding concept.  Pass a bundle
/// as the `Types` parameter of `node<Types>` to customise the Raft implementation.
template<typename T>
concept raft_types = requires {
    typename T::future_type;
    typename T::promise_type;
    typename T::try_type;

    typename T::network_client_type;
    typename T::network_server_type;
    typename T::persistence_engine_type;
    typename T::logger_type;
    typename T::metrics_type;
    typename T::membership_manager_type;
    typename T::state_machine_type;

    typename T::node_id_type;
    typename T::term_id_type;
    typename T::log_index_type;

    typename T::serializer_type;
    typename T::serialized_data_type;

    typename T::configuration_type;

    requires node_id<typename T::node_id_type>;
    requires term_id<typename T::term_id_type>;
    requires log_index<typename T::log_index_type>;
    requires serialized_data<typename T::serialized_data_type>;
    requires rpc_serializer<typename T::serializer_type, typename T::serialized_data_type>;
    requires raft_configuration_type<typename T::configuration_type>;
    requires state_machine<typename T::state_machine_type, typename T::log_index_type>;
};

/// @brief Ready-to-use type bundle suitable for simulator-based testing.
///
/// Uses `kythira::Future` / `kythira::Promise` backed by Folly, an in-memory
/// persistence engine, `console_logger`, `noop_metrics`, and the default
/// simulator network stack.
struct default_raft_types {
    using future_type = kythira::Future<std::vector<std::byte>>;
    using promise_type = kythira::Promise<std::vector<std::byte>>;
    using try_type = kythira::Try<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = json_rpc_serializer<serialized_data_type>;

    class network_client_type;
    class network_server_type;

    using persistence_engine_type =
        memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = console_logger;
    using metrics_type = noop_metrics;
    using membership_manager_type = default_membership_manager<node_id_type>;
    using state_machine_type = test_key_value_state_machine<log_index_type>;

    using configuration_type = raft_configuration;

    using log_entry_type = log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = cluster_configuration<node_id_type>;
    using snapshot_type = snapshot<node_id_type, term_id_type, log_index_type>;

    using address_type = std::string;
    using peer_discovery_type = no_op_peer_discovery<node_id_type, address_type>;
    using cluster_join_request_type = cluster_join_request<node_id_type, address_type>;
    using cluster_join_response_type = cluster_join_response<node_id_type, address_type>;
    using cluster_leave_request_type = cluster_leave_request<node_id_type, address_type>;
    using cluster_leave_response_type = cluster_leave_response<node_id_type, address_type>;

    using request_vote_request_type =
        request_vote_request<node_id_type, term_id_type, log_index_type>;
    using request_vote_response_type = request_vote_response<term_id_type>;
    using append_entries_request_type =
        append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type>;
    using append_entries_response_type = append_entries_response<term_id_type, log_index_type>;
    using install_snapshot_request_type =
        install_snapshot_request<node_id_type, term_id_type, log_index_type>;
    using install_snapshot_response_type = install_snapshot_response<term_id_type>;
};

}  // namespace kythira
