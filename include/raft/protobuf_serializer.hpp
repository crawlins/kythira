// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file protobuf_serializer.hpp
/// @brief Protocol Buffers implementation of the `rpc_serializer` concept.
///
/// A drop-in alternative to `json_rpc_serializer` (`include/raft/json_serializer.hpp`)
/// that encodes every Raft RPC message using generated Protocol Buffers message
/// classes (`raft_messages.pb.h`, generated from `proto/raft_messages.proto`).
/// Because `cpp_httplib_client`/`cpp_httplib_server`, `coap_client`/`coap_server`,
/// and `tcp_rpc`/`tls_tcp_rpc` are all parameterized by `Types::serializer_type`
/// and only ever call `.serialize(msg)` / `.deserialize_*(data)` / `.name()`, this
/// class is a drop-in replacement: switching a transport from JSON to Protocol
/// Buffers is a one-line `serializer_type` change with no transport-code change.
///
/// Protocol Buffers is used here purely as an encoding library for the existing
/// `rpc_serializer` slot -- there is NO gRPC service/channel/stub machinery, and
/// this serializer does not depend on the (separately specified) `grpc-transport`
/// (`.kiro/specs/grpc-transport/`) or on the gRPC C++ library. See
/// `.kiro/specs/protobuf-rpc-serializer/` for the full design.

#include <raft/types.hpp>
#include <raft/exceptions.hpp>
#include <raft/peer_discovery.hpp>

#include <raft_messages.pb.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace kythira {

/// @brief Short alias for the generated protobuf message namespace
/// (`proto/raft_messages.proto`'s `package kythira.raft.serializer.v1`).
namespace raft_pb = ::kythira::raft::serializer::v1;

/// @brief Protocol Buffers implementation of the `rpc_serializer` concept.
///
/// The public surface mirrors `json_rpc_serializer<Data>` exactly: one
/// `serialize` overload and one `deserialize_<message>` method per message type,
/// a generic dispatching `deserialize<T>`, and a `name()` returning `"protobuf"`.
/// @tparam Data Must satisfy `serialized_data` (a `std::byte` range,
///              e.g. `std::vector<std::byte>`).
template<typename Data>
requires std::ranges::range<Data> && std::same_as<std::ranges::range_value_t<Data>, std::byte>
class protobuf_rpc_serializer {
public:
    // ── Serialize: core four RPCs ───────────────────────────────────────────

    // Serialize RequestVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const request_vote_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        raft_pb::RequestVoteRequest msg;
        *msg.mutable_group_id() = to_group_id_value<GroupId>(req.group_id());
        msg.set_term(static_cast<std::uint64_t>(req.term()));
        *msg.mutable_candidate_id() = to_node_id_value<NodeId>(req.candidate_id());
        msg.set_last_log_index(static_cast<std::uint64_t>(req.last_log_index()));
        msg.set_last_log_term(static_cast<std::uint64_t>(req.last_log_term()));
        return encode(message_tag::request_vote_request, msg);
    }

    // Serialize RequestVote Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(const request_vote_response<TermId, GroupId>& resp) const -> Data {
        raft_pb::RequestVoteResponse msg;
        *msg.mutable_group_id() = to_group_id_value<GroupId>(resp.group_id());
        msg.set_term(static_cast<std::uint64_t>(resp.term()));
        msg.set_vote_granted(resp.vote_granted());
        return encode(message_tag::request_vote_response, msg);
    }

    // Serialize RequestPreVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const request_pre_vote_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        raft_pb::RequestPreVoteRequest msg;
        *msg.mutable_group_id() = to_group_id_value<GroupId>(req.group_id());
        msg.set_term(static_cast<std::uint64_t>(req.term()));
        *msg.mutable_candidate_id() = to_node_id_value<NodeId>(req.candidate_id());
        msg.set_last_log_index(static_cast<std::uint64_t>(req.last_log_index()));
        msg.set_last_log_term(static_cast<std::uint64_t>(req.last_log_term()));
        return encode(message_tag::request_pre_vote_request, msg);
    }

    // Serialize RequestPreVote Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(const request_pre_vote_response<TermId, GroupId>& resp) const
        -> Data {
        raft_pb::RequestPreVoteResponse msg;
        *msg.mutable_group_id() = to_group_id_value<GroupId>(resp.group_id());
        msg.set_term(static_cast<std::uint64_t>(resp.term()));
        msg.set_vote_granted(resp.vote_granted());
        return encode(message_tag::request_pre_vote_response, msg);
    }

    // Serialize AppendEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>,
             typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const append_entries_request<NodeId, TermId, LogIndex, LogEntry, GroupId>& req) const
        -> Data {
        raft_pb::AppendEntriesRequest msg;
        *msg.mutable_group_id() = to_group_id_value<GroupId>(req.group_id());
        msg.set_term(static_cast<std::uint64_t>(req.term()));
        *msg.mutable_leader_id() = to_node_id_value<NodeId>(req.leader_id());
        msg.set_prev_log_index(static_cast<std::uint64_t>(req.prev_log_index()));
        msg.set_prev_log_term(static_cast<std::uint64_t>(req.prev_log_term()));
        msg.set_leader_commit(static_cast<std::uint64_t>(req.leader_commit()));
        for (const auto& entry : req.entries()) {
            to_proto_log_entry(entry, *msg.add_entries());
        }
        return encode(message_tag::append_entries_request, msg);
    }

    // Serialize AppendEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const append_entries_response<TermId, LogIndex, GroupId>& resp) const -> Data {
        raft_pb::AppendEntriesResponse msg;
        *msg.mutable_group_id() = to_group_id_value<GroupId>(resp.group_id());
        msg.set_term(static_cast<std::uint64_t>(resp.term()));
        msg.set_success(resp.success());
        if (const auto& ci = resp.conflict_index()) {
            msg.set_conflict_index(static_cast<std::uint64_t>(*ci));
        }
        if (const auto& ct = resp.conflict_term()) {
            msg.set_conflict_term(static_cast<std::uint64_t>(*ct));
        }
        return encode(message_tag::append_entries_response, msg);
    }

    // Serialize InstallSnapshot Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const install_snapshot_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        raft_pb::InstallSnapshotRequest msg;
        *msg.mutable_group_id() = to_group_id_value<GroupId>(req.group_id());
        msg.set_term(static_cast<std::uint64_t>(req.term()));
        *msg.mutable_leader_id() = to_node_id_value<NodeId>(req.leader_id());
        msg.set_last_included_index(static_cast<std::uint64_t>(req.last_included_index()));
        msg.set_last_included_term(static_cast<std::uint64_t>(req.last_included_term()));
        msg.set_offset(static_cast<std::uint64_t>(req.offset()));
        msg.set_data(bytes_to_proto_string(req.data()));
        msg.set_done(req.done());
        return encode(message_tag::install_snapshot_request, msg);
    }

    // Serialize InstallSnapshot Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(const install_snapshot_response<TermId, GroupId>& resp) const
        -> Data {
        raft_pb::InstallSnapshotResponse msg;
        *msg.mutable_group_id() = to_group_id_value<GroupId>(resp.group_id());
        msg.set_term(static_cast<std::uint64_t>(resp.term()));
        return encode(message_tag::install_snapshot_response, msg);
    }

    // ── Serialize: bootstrap + peer2peer RPCs ───────────────────────────────

    // Serialize ClusterJoin Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_join_request<NodeId, Address>& req) const -> Data {
        raft_pb::ClusterJoinRequest msg;
        *msg.mutable_node_id() = to_node_id_value<NodeId>(req.node_id);
        msg.set_contact_address(std::string(req.contact_address));
        return encode(message_tag::cluster_join_request, msg);
    }

    // Serialize ClusterJoin Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_join_response<NodeId, Address>& resp) const -> Data {
        raft_pb::ClusterJoinResponse msg;
        msg.set_accepted(resp.accepted);
        if (resp.redirect.has_value()) {
            to_proto_peer_info<NodeId, Address>(*resp.redirect, *msg.mutable_redirect());
        }
        return encode(message_tag::cluster_join_response, msg);
    }

    // Serialize ClusterLeave Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_leave_request<NodeId, Address>& req) const -> Data {
        raft_pb::ClusterLeaveRequest msg;
        *msg.mutable_node_id() = to_node_id_value<NodeId>(req.node_id);
        return encode(message_tag::cluster_leave_request, msg);
    }

    // Serialize ClusterLeave Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_leave_response<NodeId, Address>& resp) const
        -> Data {
        raft_pb::ClusterLeaveResponse msg;
        msg.set_accepted(resp.accepted);
        if (resp.redirect.has_value()) {
            to_proto_peer_info<NodeId, Address>(*resp.redirect, *msg.mutable_redirect());
        }
        return encode(message_tag::cluster_leave_response, msg);
    }

    // Serialize FetchLogEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const fetch_log_entries_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        raft_pb::FetchLogEntriesRequest msg;
        *msg.mutable_group_id() = to_group_id_value<GroupId>(req.group_id());
        *msg.mutable_requester_id() = to_node_id_value<NodeId>(req.requester_id());
        msg.set_from_index(static_cast<std::uint64_t>(req.from_index()));
        msg.set_to_index(static_cast<std::uint64_t>(req.to_index()));
        return encode(message_tag::fetch_log_entries_request, msg);
    }

    // Serialize FetchLogEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename LogEntry = log_entry<TermId, LogIndex>, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const fetch_log_entries_response<TermId, LogIndex, LogEntry, GroupId>& resp) const -> Data {
        raft_pb::FetchLogEntriesResponse msg;
        *msg.mutable_group_id() = to_group_id_value<GroupId>(resp.group_id());
        // responder_id stays a bare uint64 (see raft_messages.proto note).
        msg.set_responder_id(static_cast<std::uint64_t>(resp.responder_id()));
        msg.set_available(resp.available());
        msg.set_prev_log_term(static_cast<std::uint64_t>(resp.prev_log_term()));
        for (const auto& entry : resp.entries()) {
            to_proto_log_entry(entry, *msg.add_entries());
        }
        return encode(message_tag::fetch_log_entries_response, msg);
    }

    // ── Deserialize: core four RPCs ─────────────────────────────────────────

    // Deserialize RequestVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_vote_request(const Data& data) const
        -> request_vote_request<NodeId, TermId, LogIndex, GroupId> {
        auto msg = decode<raft_pb::RequestVoteRequest>(message_tag::request_vote_request, data);

        request_vote_request<NodeId, TermId, LogIndex, GroupId> req;
        req._group_id = from_group_id_value<GroupId>(msg.group_id());
        req._term = static_cast<TermId>(msg.term());
        req._candidate_id = from_node_id_value<NodeId>(msg.candidate_id());
        req._last_log_index = static_cast<LogIndex>(msg.last_log_index());
        req._last_log_term = static_cast<TermId>(msg.last_log_term());
        return req;
    }

    // Deserialize RequestVote Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_vote_response(const Data& data) const
        -> request_vote_response<TermId, GroupId> {
        auto msg = decode<raft_pb::RequestVoteResponse>(message_tag::request_vote_response, data);

        request_vote_response<TermId, GroupId> resp;
        resp._group_id = from_group_id_value<GroupId>(msg.group_id());
        resp._term = static_cast<TermId>(msg.term());
        resp._vote_granted = msg.vote_granted();
        return resp;
    }

    // Deserialize RequestPreVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_pre_vote_request(const Data& data) const
        -> request_pre_vote_request<NodeId, TermId, LogIndex, GroupId> {
        auto msg =
            decode<raft_pb::RequestPreVoteRequest>(message_tag::request_pre_vote_request, data);

        request_pre_vote_request<NodeId, TermId, LogIndex, GroupId> req;
        req._group_id = from_group_id_value<GroupId>(msg.group_id());
        req._term = static_cast<TermId>(msg.term());
        req._candidate_id = from_node_id_value<NodeId>(msg.candidate_id());
        req._last_log_index = static_cast<LogIndex>(msg.last_log_index());
        req._last_log_term = static_cast<TermId>(msg.last_log_term());
        return req;
    }

    // Deserialize RequestPreVote Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_pre_vote_response(const Data& data) const
        -> request_pre_vote_response<TermId, GroupId> {
        auto msg =
            decode<raft_pb::RequestPreVoteResponse>(message_tag::request_pre_vote_response, data);

        request_pre_vote_response<TermId, GroupId> resp;
        resp._group_id = from_group_id_value<GroupId>(msg.group_id());
        resp._term = static_cast<TermId>(msg.term());
        resp._vote_granted = msg.vote_granted();
        return resp;
    }

    // Deserialize AppendEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>,
             typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_append_entries_request(const Data& data) const
        -> append_entries_request<NodeId, TermId, LogIndex, LogEntry, GroupId> {
        auto msg = decode<raft_pb::AppendEntriesRequest>(message_tag::append_entries_request, data);

        append_entries_request<NodeId, TermId, LogIndex, LogEntry, GroupId> req;
        req._group_id = from_group_id_value<GroupId>(msg.group_id());
        req._term = static_cast<TermId>(msg.term());
        req._leader_id = from_node_id_value<NodeId>(msg.leader_id());
        req._prev_log_index = static_cast<LogIndex>(msg.prev_log_index());
        req._prev_log_term = static_cast<TermId>(msg.prev_log_term());
        req._leader_commit = static_cast<LogIndex>(msg.leader_commit());
        req._entries.reserve(static_cast<std::size_t>(msg.entries_size()));
        for (const auto& proto_entry : msg.entries()) {
            req._entries.push_back(from_proto_log_entry<TermId, LogIndex, LogEntry>(proto_entry));
        }
        return req;
    }

    // Deserialize AppendEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_append_entries_response(const Data& data) const
        -> append_entries_response<TermId, LogIndex, GroupId> {
        auto msg =
            decode<raft_pb::AppendEntriesResponse>(message_tag::append_entries_response, data);

        append_entries_response<TermId, LogIndex, GroupId> resp;
        resp._group_id = from_group_id_value<GroupId>(msg.group_id());
        resp._term = static_cast<TermId>(msg.term());
        resp._success = msg.success();
        if (msg.has_conflict_index()) {
            resp._conflict_index = static_cast<LogIndex>(msg.conflict_index());
        }
        if (msg.has_conflict_term()) {
            resp._conflict_term = static_cast<TermId>(msg.conflict_term());
        }
        return resp;
    }

    // Deserialize InstallSnapshot Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_install_snapshot_request(const Data& data) const
        -> install_snapshot_request<NodeId, TermId, LogIndex, GroupId> {
        auto msg =
            decode<raft_pb::InstallSnapshotRequest>(message_tag::install_snapshot_request, data);

        install_snapshot_request<NodeId, TermId, LogIndex, GroupId> req;
        req._group_id = from_group_id_value<GroupId>(msg.group_id());
        req._term = static_cast<TermId>(msg.term());
        req._leader_id = from_node_id_value<NodeId>(msg.leader_id());
        req._last_included_index = static_cast<LogIndex>(msg.last_included_index());
        req._last_included_term = static_cast<TermId>(msg.last_included_term());
        req._offset = static_cast<std::size_t>(msg.offset());
        req._data = proto_string_to_bytes(msg.data());
        req._done = msg.done();
        return req;
    }

    // Deserialize InstallSnapshot Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_install_snapshot_response(const Data& data) const
        -> install_snapshot_response<TermId, GroupId> {
        auto msg =
            decode<raft_pb::InstallSnapshotResponse>(message_tag::install_snapshot_response, data);

        install_snapshot_response<TermId, GroupId> resp;
        resp._group_id = from_group_id_value<GroupId>(msg.group_id());
        resp._term = static_cast<TermId>(msg.term());
        return resp;
    }

    // ── Deserialize: bootstrap + peer2peer RPCs ─────────────────────────────

    // Deserialize ClusterJoin Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_join_request(const Data& data) const
        -> cluster_join_request<NodeId, Address> {
        auto msg = decode<raft_pb::ClusterJoinRequest>(message_tag::cluster_join_request, data);

        cluster_join_request<NodeId, Address> req;
        req.node_id = from_node_id_value<NodeId>(msg.node_id());
        req.contact_address = msg.contact_address();
        return req;
    }

    // Deserialize ClusterJoin Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_join_response(const Data& data) const
        -> cluster_join_response<NodeId, Address> {
        auto msg = decode<raft_pb::ClusterJoinResponse>(message_tag::cluster_join_response, data);

        cluster_join_response<NodeId, Address> resp;
        resp.accepted = msg.accepted();
        if (msg.has_redirect()) {
            resp.redirect = from_proto_peer_info<NodeId, Address>(msg.redirect());
        }
        return resp;
    }

    // Deserialize ClusterLeave Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_leave_request(const Data& data) const
        -> cluster_leave_request<NodeId, Address> {
        auto msg = decode<raft_pb::ClusterLeaveRequest>(message_tag::cluster_leave_request, data);

        cluster_leave_request<NodeId, Address> req;
        req.node_id = from_node_id_value<NodeId>(msg.node_id());
        return req;
    }

    // Deserialize ClusterLeave Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_leave_response(const Data& data) const
        -> cluster_leave_response<NodeId, Address> {
        auto msg = decode<raft_pb::ClusterLeaveResponse>(message_tag::cluster_leave_response, data);

        cluster_leave_response<NodeId, Address> resp;
        resp.accepted = msg.accepted();
        if (msg.has_redirect()) {
            resp.redirect = from_proto_peer_info<NodeId, Address>(msg.redirect());
        }
        return resp;
    }

    // Deserialize FetchLogEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_fetch_log_entries_request(const Data& data) const
        -> fetch_log_entries_request<NodeId, TermId, LogIndex, GroupId> {
        auto msg =
            decode<raft_pb::FetchLogEntriesRequest>(message_tag::fetch_log_entries_request, data);

        fetch_log_entries_request<NodeId, TermId, LogIndex, GroupId> req;
        req._group_id = from_group_id_value<GroupId>(msg.group_id());
        req._requester_id = from_node_id_value<NodeId>(msg.requester_id());
        req._from_index = static_cast<LogIndex>(msg.from_index());
        req._to_index = static_cast<LogIndex>(msg.to_index());
        return req;
    }

    // Deserialize FetchLogEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename LogEntry = log_entry<TermId, LogIndex>, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_fetch_log_entries_response(const Data& data) const
        -> fetch_log_entries_response<TermId, LogIndex, LogEntry, GroupId> {
        auto msg =
            decode<raft_pb::FetchLogEntriesResponse>(message_tag::fetch_log_entries_response, data);

        fetch_log_entries_response<TermId, LogIndex, LogEntry, GroupId> resp;
        resp._group_id = from_group_id_value<GroupId>(msg.group_id());
        resp._responder_id = static_cast<std::uint64_t>(msg.responder_id());
        resp._available = msg.available();
        resp._prev_log_term = static_cast<TermId>(msg.prev_log_term());
        resp._entries.reserve(static_cast<std::size_t>(msg.entries_size()));
        for (const auto& proto_entry : msg.entries()) {
            resp._entries.push_back(from_proto_log_entry<TermId, LogIndex, LogEntry>(proto_entry));
        }
        return resp;
    }

    // ── Generic dispatcher + name ───────────────────────────────────────────

    // Generic deserialize method that dispatches to specific deserialize methods
    template<typename T> [[nodiscard]] auto deserialize(const Data& data) const -> T {
        if constexpr (std::same_as<T, request_vote_request<>>) {
            return deserialize_request_vote_request(data);
        } else if constexpr (std::same_as<T, request_vote_response<>>) {
            return deserialize_request_vote_response(data);
        } else if constexpr (std::same_as<T, request_pre_vote_request<>>) {
            return deserialize_request_pre_vote_request(data);
        } else if constexpr (std::same_as<T, request_pre_vote_response<>>) {
            return deserialize_request_pre_vote_response(data);
        } else if constexpr (std::same_as<T, append_entries_request<>>) {
            return deserialize_append_entries_request(data);
        } else if constexpr (std::same_as<T, append_entries_response<>>) {
            return deserialize_append_entries_response(data);
        } else if constexpr (std::same_as<T, install_snapshot_request<>>) {
            return deserialize_install_snapshot_request(data);
        } else if constexpr (std::same_as<T, install_snapshot_response<>>) {
            return deserialize_install_snapshot_response(data);
        } else if constexpr (std::same_as<T, cluster_join_request<>>) {
            return deserialize_cluster_join_request(data);
        } else if constexpr (std::same_as<T, cluster_join_response<>>) {
            return deserialize_cluster_join_response(data);
        } else if constexpr (std::same_as<T, cluster_leave_request<>>) {
            return deserialize_cluster_leave_request(data);
        } else if constexpr (std::same_as<T, cluster_leave_response<>>) {
            return deserialize_cluster_leave_response(data);
        } else if constexpr (std::same_as<T, fetch_log_entries_request<>>) {
            return deserialize_fetch_log_entries_request(data);
        } else if constexpr (std::same_as<T, fetch_log_entries_response<>>) {
            return deserialize_fetch_log_entries_response(data);
        } else {
            static_assert(std::is_same_v<T, void>, "Unsupported type for deserialization");
        }
    }

    // Provide name method for content format detection
    [[nodiscard]] auto name() const -> std::string { return "protobuf"; }

    /// @brief IANA media type used for HTTP `Content-Type`/`Accept` negotiation
    ///        and CoAP Content-Format mapping (`rpc_serializer`).
    ///
    /// `application/x-protobuf` rather than `application/protobuf`: the former
    /// is what gRPC-Gateway, Prometheus and the wider ecosystem actually emit,
    /// and interoperating with existing peers matters more here than preferring
    /// the later formal registration.
    [[nodiscard]] auto media_type() const -> std::string { return "application/x-protobuf"; }

private:
    /// @brief One-byte discriminator prepended to every payload, checked before
    /// any protobuf parse. Raw Protocol Buffers bytes are not self-describing --
    /// proto3 has no required fields, so `ParseFromArray` on bytes belonging to a
    /// *different* message type can silently "succeed" and yield a
    /// plausible-but-wrong value. This tag reproduces the safety property
    /// `json_rpc_serializer`'s `"type"` field provides for JSON, guaranteeing
    /// wrong-message-type payloads are rejected rather than mis-parsed.
    enum class message_tag : std::uint8_t {
        request_vote_request = 0,
        request_vote_response = 1,
        request_pre_vote_request = 2,
        request_pre_vote_response = 3,
        append_entries_request = 4,
        append_entries_response = 5,
        install_snapshot_request = 6,
        install_snapshot_response = 7,
        cluster_join_request = 8,
        cluster_join_response = 9,
        cluster_leave_request = 10,
        cluster_leave_response = 11,
        fetch_log_entries_request = 12,
        fetch_log_entries_response = 13,
    };

    // ── Tag-prepend / tag-check codec (Requirement 5) ───────────────────────

    /// @brief Prepend the message-type tag byte to `msg`'s Protocol-Buffers bytes.
    template<typename ProtoMessage>
    [[nodiscard]] auto encode(message_tag tag, const ProtoMessage& msg) const -> Data {
        std::string payload;
        payload.push_back(static_cast<char>(static_cast<std::uint8_t>(tag)));
        payload.append(msg.SerializeAsString());
        return string_to_data(payload);
    }

    /// @brief Verify the leading tag byte, then parse the remainder as
    /// `ProtoMessage`. Throws `serialization_exception` on a short payload, a
    /// tag mismatch, or a protobuf parse failure -- never returns a
    /// mis-interpreted message.
    template<typename ProtoMessage>
    [[nodiscard]] auto decode(message_tag expected_tag, const Data& data) const -> ProtoMessage {
        if (std::ranges::size(data) < 1) {
            throw serialization_exception("protobuf payload too short for tag");
        }

        auto it = std::ranges::begin(data);
        const auto tag_byte = static_cast<std::uint8_t>(*it);
        if (tag_byte != static_cast<std::uint8_t>(expected_tag)) {
            throw serialization_exception("wrong message type for protobuf payload");
        }

        // Copy the tag-stripped remainder into a std::string. `serialized_data`
        // only guarantees a byte range (not contiguous storage), so we cannot
        // rely on a raw pointer into `data` -- this mirrors json_rpc_serializer's
        // generic `bytes_to_string` iteration.
        std::string body;
        body.reserve(std::ranges::size(data) - 1);
        for (++it; it != std::ranges::end(data); ++it) {
            body.push_back(static_cast<char>(*it));
        }

        ProtoMessage msg;
        if (!msg.ParseFromString(body)) {
            throw serialization_exception("malformed protobuf payload");
        }
        return msg;
    }

    // ── NodeId <-> NodeIdValue oneof wrapper (Requirement 3) ─────────────────

    /// @brief Encode a `NodeId` (either `std::uint64_t` or `std::string`) into
    /// the `NodeIdValue` oneof, selecting the case at compile time.
    template<typename NodeId>
    [[nodiscard]] auto to_node_id_value(const NodeId& id) const -> raft_pb::NodeIdValue {
        raft_pb::NodeIdValue v;
        if constexpr (std::same_as<NodeId, std::string>) {
            v.set_text(id);
        } else {
            v.set_numeric(static_cast<std::uint64_t>(id));
        }
        return v;
    }

    /// @brief Decode a `NodeIdValue` against a target `NodeId` type, throwing if
    /// the populated oneof case does not match the requested type.
    template<typename NodeId>
    [[nodiscard]] auto from_node_id_value(const raft_pb::NodeIdValue& v) const -> NodeId {
        if constexpr (std::same_as<NodeId, std::string>) {
            if (v.value_case() != raft_pb::NodeIdValue::kText) {
                throw serialization_exception("NodeIdValue: expected string, got numeric");
            }
            return v.text();
        } else {
            if (v.value_case() != raft_pb::NodeIdValue::kNumeric) {
                throw serialization_exception("NodeIdValue: expected numeric, got string");
            }
            return static_cast<NodeId>(v.numeric());
        }
    }

    // ── GroupId <-> GroupIdValue oneof wrapper (multi-Raft, design §2.2) ─────

    /// @brief Encode a `GroupId` (either `std::uint64_t` or `std::string`) into
    /// the `GroupIdValue` oneof, selecting the case at compile time.
    ///
    /// Separate from `to_node_id_value` because `NodeId` and `GroupId` are
    /// independent template parameters, and sharing one wire type would make a
    /// future change to either of them a change to both.
    template<typename GroupId>
    [[nodiscard]] auto to_group_id_value(const GroupId& id) const -> raft_pb::GroupIdValue {
        raft_pb::GroupIdValue v;
        if constexpr (std::same_as<GroupId, std::string>) {
            v.set_text(id);
        } else {
            v.set_numeric(static_cast<std::uint64_t>(id));
        }
        return v;
    }

    /// @brief Decode a `GroupIdValue`, treating an UNSET oneof as `GroupId{}`.
    ///
    /// This is the one place the group id's decode rule differs from the node
    /// id's, and it is the whole backward-compatibility story: every payload
    /// recorded before multi-Raft carries no `group_id` field, proto3 hands
    /// those back as a default-constructed submessage with no case set, and
    /// `GroupId{}` is exactly what "the single group" has always meant. A
    /// populated case of the *wrong* type is still an error — that is a real
    /// type mismatch rather than an old payload.
    template<typename GroupId>
    [[nodiscard]] auto from_group_id_value(const raft_pb::GroupIdValue& v) const -> GroupId {
        if (v.value_case() == raft_pb::GroupIdValue::VALUE_NOT_SET) {
            return GroupId{};
        }
        if constexpr (std::same_as<GroupId, std::string>) {
            if (v.value_case() != raft_pb::GroupIdValue::kText) {
                throw serialization_exception("GroupIdValue: expected string, got numeric");
            }
            return v.text();
        } else {
            if (v.value_case() != raft_pb::GroupIdValue::kNumeric) {
                throw serialization_exception("GroupIdValue: expected numeric, got string");
            }
            return static_cast<GroupId>(v.numeric());
        }
    }

    // ── LogEntry / PeerInfo conversions ─────────────────────────────────────

    /// @brief Convert a kythira log entry into its protobuf representation.
    template<typename Entry>
    void to_proto_log_entry(const Entry& entry, raft_pb::LogEntry& out) const {
        out.set_term(static_cast<std::uint64_t>(entry.term()));
        out.set_index(static_cast<std::uint64_t>(entry.index()));
        out.set_command(bytes_to_proto_string(entry.command()));
        out.set_type(static_cast<raft_pb::EntryType>(static_cast<int>(entry.type())));
    }

    /// @brief Convert a protobuf log entry back into a kythira `LogEntry`.
    template<typename TermId, typename LogIndex, typename LogEntry>
    [[nodiscard]] auto from_proto_log_entry(const raft_pb::LogEntry& proto_entry) const
        -> LogEntry {
        LogEntry entry;
        entry._term = static_cast<TermId>(proto_entry.term());
        entry._index = static_cast<LogIndex>(proto_entry.index());
        entry._command = proto_string_to_bytes(proto_entry.command());
        entry._type = static_cast<entry_type>(static_cast<int>(proto_entry.type()));
        return entry;
    }

    /// @brief Convert a kythira `peer_info` into its protobuf representation.
    template<typename NodeId, typename Address>
    void to_proto_peer_info(const peer_info<NodeId, Address>& pi, raft_pb::PeerInfo& out) const {
        *out.mutable_node_id() = to_node_id_value<NodeId>(pi.node_id);
        out.set_address(std::string(pi.address));
    }

    /// @brief Convert a protobuf `PeerInfo` back into a kythira `peer_info`.
    template<typename NodeId, typename Address>
    [[nodiscard]] auto from_proto_peer_info(const raft_pb::PeerInfo& proto_pi) const
        -> peer_info<NodeId, Address> {
        peer_info<NodeId, Address> pi;
        pi.node_id = from_node_id_value<NodeId>(proto_pi.node_id());
        pi.address = proto_pi.address();
        return pi;
    }

    // ── Byte-buffer helpers ─────────────────────────────────────────────────

    /// @brief Copy a `std::string` (holding raw serialized bytes) into a `Data`.
    [[nodiscard]] auto string_to_data(const std::string& str) const -> Data {
        Data result;
        if constexpr (requires { result.resize(0); }) {
            result.resize(str.size());
            std::transform(str.begin(), str.end(), result.begin(),
                           [](char c) { return static_cast<std::byte>(c); });
        }
        return result;
    }

    /// @brief View a `std::vector<std::byte>` as a protobuf `bytes` value.
    ///
    /// Protocol Buffers' `bytes` setters accept a `std::string` over arbitrary
    /// octets (not just valid UTF-8), so `command`/`data` pass straight through
    /// with no base64 step (Requirement 6.6).
    [[nodiscard]] auto bytes_to_proto_string(const std::vector<std::byte>& data) const
        -> std::string {
        return std::string(reinterpret_cast<const char*>(data.data()), data.size());
    }

    /// @brief Copy a protobuf `bytes` value back into a `std::vector<std::byte>`.
    [[nodiscard]] auto proto_string_to_bytes(const std::string& str) const
        -> std::vector<std::byte> {
        const auto* begin = reinterpret_cast<const std::byte*>(str.data());
        return std::vector<std::byte>(begin, begin + str.size());
    }
};

// Verify that protobuf_rpc_serializer satisfies the rpc_serializer concept
static_assert(
    rpc_serializer<protobuf_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>,
    "protobuf_rpc_serializer must satisfy the rpc_serializer concept");

// Convenience type alias for the default Protocol Buffers serializer
using protobuf_serializer = protobuf_rpc_serializer<std::vector<std::byte>>;

}  // namespace kythira
