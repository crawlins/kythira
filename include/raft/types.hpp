#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <cstddef>
#include <ranges>
#include <chrono>
#include <ostream>

namespace raft {

// Node identifier concept - can be any unsigned integer or string
template<typename T>
concept node_id = std::unsigned_integral<T> || std::same_as<T, std::string>;

// Term number concept (monotonically increasing unsigned integer)
template<typename T>
concept term_id = std::unsigned_integral<T>;

// Log index concept (1-based, monotonically increasing unsigned integer)
template<typename T>
concept log_index = std::unsigned_integral<T>;

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

// RPC serializer concept
template<typename S, typename Data>
concept rpc_serializer = requires(
    S serializer,
    const request_vote_request<>& rvr,
    const request_vote_response<>& rvresp,
    const append_entries_request<>& aer,
    const append_entries_response<>& aeresp,
    const install_snapshot_request<>& isr,
    const install_snapshot_response<>& isresp,
    const Data& data
) {
    // Data must satisfy serialized_data concept
    requires serialized_data<Data>;
    
    // Serialize RequestVote RPC
    { serializer.serialize(rvr) } -> std::same_as<Data>;
    { serializer.deserialize_request_vote_request(data) } -> std::same_as<request_vote_request<>>;
    { serializer.serialize(rvresp) } -> std::same_as<Data>;
    { serializer.deserialize_request_vote_response(data) } -> std::same_as<request_vote_response<>>;
    
    // Serialize AppendEntries RPC
    { serializer.serialize(aer) } -> std::same_as<Data>;
    { serializer.deserialize_append_entries_request(data) } -> std::same_as<append_entries_request<>>;
    { serializer.serialize(aeresp) } -> std::same_as<Data>;
    { serializer.deserialize_append_entries_response(data) } -> std::same_as<append_entries_response<>>;
    
    // Serialize InstallSnapshot RPC
    { serializer.serialize(isr) } -> std::same_as<Data>;
    { serializer.deserialize_install_snapshot_request(data) } -> std::same_as<install_snapshot_request<>>;
    { serializer.serialize(isresp) } -> std::same_as<Data>;
    { serializer.deserialize_install_snapshot_response(data) } -> std::same_as<install_snapshot_response<>>;
};

// Raft configuration concept
template<typename T>
concept raft_configuration_type = requires(const T& config) {
    { config.election_timeout_min() } -> std::same_as<std::chrono::milliseconds>;
    { config.election_timeout_max() } -> std::same_as<std::chrono::milliseconds>;
    { config.heartbeat_interval() } -> std::same_as<std::chrono::milliseconds>;
    { config.rpc_timeout() } -> std::same_as<std::chrono::milliseconds>;
    { config.max_entries_per_append() } -> std::same_as<std::size_t>;
    { config.snapshot_threshold_bytes() } -> std::same_as<std::size_t>;
    { config.snapshot_chunk_size() } -> std::same_as<std::size_t>;
};

// Default raft configuration implementation
struct raft_configuration {
    std::chrono::milliseconds _election_timeout_min{150};
    std::chrono::milliseconds _election_timeout_max{300};
    std::chrono::milliseconds _heartbeat_interval{50};
    std::chrono::milliseconds _rpc_timeout{100};
    std::size_t _max_entries_per_append{100};
    std::size_t _snapshot_threshold_bytes{10'000'000};
    std::size_t _snapshot_chunk_size{1'000'000};
    
    auto election_timeout_min() const -> std::chrono::milliseconds { return _election_timeout_min; }
    auto election_timeout_max() const -> std::chrono::milliseconds { return _election_timeout_max; }
    auto heartbeat_interval() const -> std::chrono::milliseconds { return _heartbeat_interval; }
    auto rpc_timeout() const -> std::chrono::milliseconds { return _rpc_timeout; }
    auto max_entries_per_append() const -> std::size_t { return _max_entries_per_append; }
    auto snapshot_threshold_bytes() const -> std::size_t { return _snapshot_threshold_bytes; }
    auto snapshot_chunk_size() const -> std::size_t { return _snapshot_chunk_size; }
};

} // namespace raft
