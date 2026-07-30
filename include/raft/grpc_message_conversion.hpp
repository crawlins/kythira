#pragma once

/// @file grpc_message_conversion.hpp
/// @brief `to_proto`/`from_proto` conversions between `kythira` Raft message
/// structs and the generated Protocol Buffers messages
/// (.kiro/specs/grpc-transport/, Task 2.2).
///
/// The gRPC transport deliberately does **not** use `Types::serializer_type`:
/// Protocol Buffers is both the IDL and the wire format, so conversion happens
/// through this fixed set of free functions rather than a pluggable
/// `rpc_serializer` (see the design doc's "Protobuf replaces `rpc_serializer`"
/// decision). Every function is total on its domain and **fails closed** on
/// out-of-range or unknown-enum input (Requirement 11.6) — the server maps that
/// failure to `grpc::StatusCode::INVALID_ARGUMENT` rather than letting an
/// unhandled exception escape onto the RPC thread.
///
/// `from_proto(to_proto(value)) == value` for every field, including absent
/// `std::optional`s and empty `entries()` (Property 2).

#include <raft/types.hpp>

#include "raft.pb.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace kythira {

/// @brief Thrown by `from_proto` when a protobuf message cannot be represented
/// as its `kythira` counterpart (unknown enum value, integer out of the target
/// type's range). Derives from `std::invalid_argument` so the server's
/// conversion guard can map it to `INVALID_ARGUMENT` (Requirement 11.6).
class grpc_message_conversion_error : public std::invalid_argument {
public:
    explicit grpc_message_conversion_error(const std::string& message)
        : std::invalid_argument(message) {}
};

namespace grpc_detail {

/// @brief Copy a byte buffer into a protobuf `bytes` field (`std::string`).
inline auto bytes_to_proto(const std::vector<std::byte>& data) -> std::string {
    std::string out;
    out.resize(data.size());
    if (!data.empty()) {
        std::memcpy(out.data(), data.data(), data.size());
    }
    return out;
}

/// @brief Copy a protobuf `bytes` field (`std::string`) into a byte buffer.
inline auto bytes_from_proto(const std::string& data) -> std::vector<std::byte> {
    std::vector<std::byte> out(data.size());
    if (!data.empty()) {
        std::memcpy(out.data(), data.data(), data.size());
    }
    return out;
}

/// @brief Narrow a protobuf `uint64` to `std::size_t`, failing closed if the
/// value does not fit (only possible on a 32-bit `std::size_t`).
inline auto uint64_to_size(std::uint64_t value) -> std::size_t {
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw grpc_message_conversion_error("offset value exceeds std::size_t range");
    }
    return static_cast<std::size_t>(value);
}

}  // namespace grpc_detail

// ── EntryType / entry_type ──────────────────────────────────────────────────

inline auto to_proto(entry_type type) -> raft::v1::EntryType {
    switch (type) {
        case entry_type::normal:
            return raft::v1::ENTRY_TYPE_NORMAL;
        case entry_type::configuration:
            return raft::v1::ENTRY_TYPE_CONFIGURATION;
        case entry_type::no_op:
            return raft::v1::ENTRY_TYPE_NO_OP;
    }
    // entry_type is a closed C++ enum; an out-of-range value is UB upstream, so
    // reaching here means the caller handed us a corrupt value — fail closed.
    throw grpc_message_conversion_error("unknown kythira::entry_type value");
}

inline auto entry_type_from_proto(raft::v1::EntryType type) -> entry_type {
    switch (type) {
        case raft::v1::ENTRY_TYPE_NORMAL:
            return entry_type::normal;
        case raft::v1::ENTRY_TYPE_CONFIGURATION:
            return entry_type::configuration;
        case raft::v1::ENTRY_TYPE_NO_OP:
            return entry_type::no_op;
        default:
            // proto3 open enums preserve unknown wire values as their raw int
            // (e.g. a tag a newer peer added) — reject rather than silently
            // coercing to normal (Requirement 11.6).
            throw grpc_message_conversion_error("unknown EntryType wire value");
    }
}

// ── LogEntry / log_entry ────────────────────────────────────────────────────

inline auto to_proto(const log_entry<>& entry) -> raft::v1::LogEntry {
    raft::v1::LogEntry proto;
    proto.set_term(entry.term());
    proto.set_index(entry.index());
    proto.set_command(grpc_detail::bytes_to_proto(entry.command()));
    proto.set_type(to_proto(entry.type()));
    return proto;
}

inline auto from_proto(const raft::v1::LogEntry& proto) -> log_entry<> {
    return log_entry<>{._term = proto.term(),
                       ._index = proto.index(),
                       ._command = grpc_detail::bytes_from_proto(proto.command()),
                       ._type = entry_type_from_proto(proto.type())};
}

// ── RequestVote ─────────────────────────────────────────────────────────────

inline auto to_proto(const request_vote_request<>& req) -> raft::v1::RequestVoteRequest {
    raft::v1::RequestVoteRequest proto;
    proto.set_term(req.term());
    proto.set_candidate_id(req.candidate_id());
    proto.set_last_log_index(req.last_log_index());
    proto.set_last_log_term(req.last_log_term());
    return proto;
}

inline auto from_proto(const raft::v1::RequestVoteRequest& proto) -> request_vote_request<> {
    return request_vote_request<>{._term = proto.term(),
                                  ._candidate_id = proto.candidate_id(),
                                  ._last_log_index = proto.last_log_index(),
                                  ._last_log_term = proto.last_log_term()};
}

inline auto to_proto(const request_vote_response<>& resp) -> raft::v1::RequestVoteResponse {
    raft::v1::RequestVoteResponse proto;
    proto.set_term(resp.term());
    proto.set_vote_granted(resp.vote_granted());
    return proto;
}

inline auto from_proto(const raft::v1::RequestVoteResponse& proto) -> request_vote_response<> {
    return request_vote_response<>{._term = proto.term(), ._vote_granted = proto.vote_granted()};
}

// ── RequestPreVote (same shape as RequestVote, Requirement 16) ───────────────

inline auto to_proto(const request_pre_vote_request<>& req) -> raft::v1::RequestPreVoteRequest {
    raft::v1::RequestPreVoteRequest proto;
    proto.set_term(req.term());
    proto.set_candidate_id(req.candidate_id());
    proto.set_last_log_index(req.last_log_index());
    proto.set_last_log_term(req.last_log_term());
    return proto;
}

inline auto from_proto(const raft::v1::RequestPreVoteRequest& proto) -> request_pre_vote_request<> {
    return request_pre_vote_request<>{._term = proto.term(),
                                      ._candidate_id = proto.candidate_id(),
                                      ._last_log_index = proto.last_log_index(),
                                      ._last_log_term = proto.last_log_term()};
}

inline auto to_proto(const request_pre_vote_response<>& resp) -> raft::v1::RequestPreVoteResponse {
    raft::v1::RequestPreVoteResponse proto;
    proto.set_term(resp.term());
    proto.set_vote_granted(resp.vote_granted());
    return proto;
}

inline auto from_proto(const raft::v1::RequestPreVoteResponse& proto)
    -> request_pre_vote_response<> {
    return request_pre_vote_response<>{._term = proto.term(),
                                       ._vote_granted = proto.vote_granted()};
}

// ── AppendEntries ───────────────────────────────────────────────────────────

inline auto to_proto(const append_entries_request<>& req) -> raft::v1::AppendEntriesRequest {
    raft::v1::AppendEntriesRequest proto;
    proto.set_term(req.term());
    proto.set_leader_id(req.leader_id());
    proto.set_prev_log_index(req.prev_log_index());
    proto.set_prev_log_term(req.prev_log_term());
    proto.set_leader_commit(req.leader_commit());
    for (const auto& entry : req.entries()) {
        *proto.add_entries() = to_proto(entry);
    }
    return proto;
}

inline auto from_proto(const raft::v1::AppendEntriesRequest& proto) -> append_entries_request<> {
    append_entries_request<> req{._term = proto.term(),
                                 ._leader_id = proto.leader_id(),
                                 ._prev_log_index = proto.prev_log_index(),
                                 ._prev_log_term = proto.prev_log_term(),
                                 ._entries = {},
                                 ._leader_commit = proto.leader_commit()};
    req._entries.reserve(static_cast<std::size_t>(proto.entries_size()));
    for (const auto& entry : proto.entries()) {
        req._entries.push_back(from_proto(entry));
    }
    return req;
}

inline auto to_proto(const append_entries_response<>& resp) -> raft::v1::AppendEntriesResponse {
    raft::v1::AppendEntriesResponse proto;
    proto.set_term(resp.term());
    proto.set_success(resp.success());
    // Absent optionals stay unset so the round-trip yields std::nullopt again,
    // never a zero the requester would read as "conflict at index 0".
    if (resp.conflict_index().has_value()) {
        proto.set_conflict_index(resp.conflict_index().value());
    }
    if (resp.conflict_term().has_value()) {
        proto.set_conflict_term(resp.conflict_term().value());
    }
    return proto;
}

inline auto from_proto(const raft::v1::AppendEntriesResponse& proto) -> append_entries_response<> {
    append_entries_response<> resp{._term = proto.term(),
                                   ._success = proto.success(),
                                   ._conflict_index = std::nullopt,
                                   ._conflict_term = std::nullopt};
    if (proto.has_conflict_index()) {
        resp._conflict_index = proto.conflict_index();
    }
    if (proto.has_conflict_term()) {
        resp._conflict_term = proto.conflict_term();
    }
    return resp;
}

// ── InstallSnapshot ─────────────────────────────────────────────────────────

inline auto to_proto(const install_snapshot_request<>& req) -> raft::v1::InstallSnapshotRequest {
    raft::v1::InstallSnapshotRequest proto;
    proto.set_term(req.term());
    proto.set_leader_id(req.leader_id());
    proto.set_last_included_index(req.last_included_index());
    proto.set_last_included_term(req.last_included_term());
    proto.set_offset(static_cast<std::uint64_t>(req.offset()));
    proto.set_data(grpc_detail::bytes_to_proto(req.data()));
    proto.set_done(req.done());
    return proto;
}

inline auto from_proto(const raft::v1::InstallSnapshotRequest& proto)
    -> install_snapshot_request<> {
    return install_snapshot_request<>{._term = proto.term(),
                                      ._leader_id = proto.leader_id(),
                                      ._last_included_index = proto.last_included_index(),
                                      ._last_included_term = proto.last_included_term(),
                                      ._offset = grpc_detail::uint64_to_size(proto.offset()),
                                      ._data = grpc_detail::bytes_from_proto(proto.data()),
                                      ._done = proto.done()};
}

inline auto to_proto(const install_snapshot_response<>& resp) -> raft::v1::InstallSnapshotResponse {
    raft::v1::InstallSnapshotResponse proto;
    proto.set_term(resp.term());
    return proto;
}

inline auto from_proto(const raft::v1::InstallSnapshotResponse& proto)
    -> install_snapshot_response<> {
    return install_snapshot_response<>{._term = proto.term()};
}

// ── PeerInfo / peer_info redirect hint (Requirement 15.4) ────────────────────

inline auto to_proto(const peer_info<std::uint64_t, std::string>& peer) -> raft::v1::PeerInfo {
    raft::v1::PeerInfo proto;
    proto.set_node_id(peer.node_id);
    proto.set_address(peer.address);
    return proto;
}

inline auto from_proto(const raft::v1::PeerInfo& proto) -> peer_info<std::uint64_t, std::string> {
    return peer_info<std::uint64_t, std::string>{.node_id = proto.node_id(),
                                                 .address = proto.address()};
}

// ── ClusterJoin ─────────────────────────────────────────────────────────────

inline auto to_proto(const cluster_join_request<>& req) -> raft::v1::ClusterJoinRequest {
    raft::v1::ClusterJoinRequest proto;
    proto.set_node_id(req.joining_node_id());
    proto.set_contact_address(req.joining_address());
    return proto;
}

inline auto from_proto(const raft::v1::ClusterJoinRequest& proto) -> cluster_join_request<> {
    return cluster_join_request<>{.node_id = proto.node_id(),
                                  .contact_address = proto.contact_address()};
}

inline auto to_proto(const cluster_join_response<>& resp) -> raft::v1::ClusterJoinResponse {
    raft::v1::ClusterJoinResponse proto;
    proto.set_accepted(resp.is_accepted());
    if (resp.redirect_peer().has_value()) {
        *proto.mutable_redirect() = to_proto(resp.redirect_peer().value());
    }
    return proto;
}

inline auto from_proto(const raft::v1::ClusterJoinResponse& proto) -> cluster_join_response<> {
    cluster_join_response<> resp{.accepted = proto.accepted(), .redirect = std::nullopt};
    if (proto.has_redirect()) {
        resp.redirect = from_proto(proto.redirect());
    }
    return resp;
}

// ── ClusterLeave ────────────────────────────────────────────────────────────

inline auto to_proto(const cluster_leave_request<>& req) -> raft::v1::ClusterLeaveRequest {
    raft::v1::ClusterLeaveRequest proto;
    proto.set_node_id(req.leaving_node_id());
    return proto;
}

inline auto from_proto(const raft::v1::ClusterLeaveRequest& proto) -> cluster_leave_request<> {
    return cluster_leave_request<>{.node_id = proto.node_id()};
}

inline auto to_proto(const cluster_leave_response<>& resp) -> raft::v1::ClusterLeaveResponse {
    raft::v1::ClusterLeaveResponse proto;
    proto.set_accepted(resp.is_accepted());
    if (resp.redirect_peer().has_value()) {
        *proto.mutable_redirect() = to_proto(resp.redirect_peer().value());
    }
    return proto;
}

inline auto from_proto(const raft::v1::ClusterLeaveResponse& proto) -> cluster_leave_response<> {
    cluster_leave_response<> resp{.accepted = proto.accepted(), .redirect = std::nullopt};
    if (proto.has_redirect()) {
        resp.redirect = from_proto(proto.redirect());
    }
    return resp;
}

// ── FetchLogEntries (Requirement 17.3) ──────────────────────────────────────

inline auto to_proto(const fetch_log_entries_request<>& req) -> raft::v1::FetchLogEntriesRequest {
    raft::v1::FetchLogEntriesRequest proto;
    proto.set_requester_id(req.requester_id());
    proto.set_from_index(req.from_index());
    proto.set_to_index(req.to_index());
    return proto;
}

inline auto from_proto(const raft::v1::FetchLogEntriesRequest& proto)
    -> fetch_log_entries_request<> {
    return fetch_log_entries_request<>{._requester_id = proto.requester_id(),
                                       ._from_index = proto.from_index(),
                                       ._to_index = proto.to_index()};
}

inline auto to_proto(const fetch_log_entries_response<>& resp)
    -> raft::v1::FetchLogEntriesResponse {
    raft::v1::FetchLogEntriesResponse proto;
    proto.set_responder_id(resp.responder_id());
    proto.set_available(resp.available());
    proto.set_prev_log_term(resp.prev_log_term());
    for (const auto& entry : resp.entries()) {
        *proto.add_entries() = to_proto(entry);
    }
    return proto;
}

inline auto from_proto(const raft::v1::FetchLogEntriesResponse& proto)
    -> fetch_log_entries_response<> {
    fetch_log_entries_response<> resp{._responder_id = proto.responder_id(),
                                      ._available = proto.available(),
                                      ._prev_log_term = proto.prev_log_term(),
                                      ._entries = {}};
    resp._entries.reserve(static_cast<std::size_t>(proto.entries_size()));
    for (const auto& entry : proto.entries()) {
        resp._entries.push_back(from_proto(entry));
    }
    return resp;
}

}  // namespace kythira
