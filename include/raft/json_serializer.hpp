// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/types.hpp>
#include <raft/exceptions.hpp>

#include <boost/json.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kythira {

// JSON RPC Serializer implementation
template<typename Data>
requires std::ranges::range<Data> && std::same_as<std::ranges::range_value_t<Data>, std::byte>
class json_rpc_serializer {
public:
    // ── multi-Raft group id (.kiro/specs/multi-raft/ design §2.2) ────────────
    //
    // The key is emitted unconditionally, and an absent key decodes to
    // `GroupId{}` rather than throwing. That pair is the whole backward
    // compatibility story: every payload recorded before multi-Raft existed
    // still decodes, and it decodes to "the single group", which is what it
    // always meant.

    template<typename GroupId>
    static auto encode_group_id(boost::json::object& obj, const GroupId& group) -> void {
        obj["group_id"] = group;
    }

    template<typename GroupId>
    [[nodiscard]] static auto decode_group_id(const boost::json::object& obj) -> GroupId {
        if (!obj.contains("group_id")) {
            return GroupId{};
        }
        return read_id<GroupId>(obj, "group_id");
    }

    // Serialize RequestVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const request_vote_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        boost::json::object obj;
        obj["type"] = "request_vote_request";
        encode_group_id(obj, req.group_id());
        obj["term"] = req.term();
        obj["candidate_id"] = req.candidate_id();
        obj["last_log_index"] = req.last_log_index();
        obj["last_log_term"] = req.last_log_term();

        return json_to_bytes(boost::json::serialize(obj));
    }

    // Serialize RequestVote Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(const request_vote_response<TermId, GroupId>& resp) const -> Data {
        boost::json::object obj;
        obj["type"] = "request_vote_response";
        encode_group_id(obj, resp.group_id());
        obj["term"] = resp.term();
        obj["vote_granted"] = resp.vote_granted();

        return json_to_bytes(boost::json::serialize(obj));
    }

    // Serialize RequestPreVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const request_pre_vote_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        boost::json::object obj;
        obj["type"] = "request_pre_vote_request";
        encode_group_id(obj, req.group_id());
        obj["term"] = req.term();
        obj["candidate_id"] = req.candidate_id();
        obj["last_log_index"] = req.last_log_index();
        obj["last_log_term"] = req.last_log_term();

        return json_to_bytes(boost::json::serialize(obj));
    }

    // Serialize RequestPreVote Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(const request_pre_vote_response<TermId, GroupId>& resp) const
        -> Data {
        boost::json::object obj;
        obj["type"] = "request_pre_vote_response";
        encode_group_id(obj, resp.group_id());
        obj["term"] = resp.term();
        obj["vote_granted"] = resp.vote_granted();

        return json_to_bytes(boost::json::serialize(obj));
    }

    // Serialize TimeoutNow Request (leadership transfer, dissertation §3.10)
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const timeout_now_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        boost::json::object obj;
        obj["type"] = "timeout_now_request";
        encode_group_id(obj, req.group_id());
        obj["term"] = req.term();
        obj["leader_id"] = req.leader_id();
        obj["last_log_index"] = req.last_log_index();

        return json_to_bytes(boost::json::serialize(obj));
    }

    // Serialize TimeoutNow Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(const timeout_now_response<TermId, GroupId>& resp) const -> Data {
        boost::json::object obj;
        obj["type"] = "timeout_now_response";
        encode_group_id(obj, resp.group_id());
        obj["term"] = resp.term();
        obj["success"] = resp.success();

        return json_to_bytes(boost::json::serialize(obj));
    }

    // Serialize AppendEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>,
             typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const append_entries_request<NodeId, TermId, LogIndex, LogEntry, GroupId>& req) const
        -> Data {
        boost::json::object obj;
        obj["type"] = "append_entries_request";
        encode_group_id(obj, req.group_id());
        obj["term"] = req.term();
        obj["leader_id"] = req.leader_id();
        obj["prev_log_index"] = req.prev_log_index();
        obj["prev_log_term"] = req.prev_log_term();
        obj["leader_commit"] = req.leader_commit();

        // Serialize entries
        boost::json::array entries_array;
        for (const auto& entry : req.entries()) {
            boost::json::object entry_obj;
            entry_obj["term"] = entry.term();
            entry_obj["index"] = entry.index();
            entry_obj["command"] = bytes_to_base64(entry.command());
            entry_obj["entry_type"] = static_cast<int>(entry.type());
            entries_array.push_back(entry_obj);
        }
        obj["entries"] = entries_array;

        return json_to_bytes(boost::json::serialize(obj));
    }

    // Serialize AppendEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const append_entries_response<TermId, LogIndex, GroupId>& resp) const -> Data {
        boost::json::object obj;
        obj["type"] = "append_entries_response";
        encode_group_id(obj, resp.group_id());
        obj["term"] = resp.term();
        obj["success"] = resp.success();

        if (const auto& ci = resp.conflict_index()) {
            obj["conflict_index"] = *ci;
        }

        if (const auto& ct = resp.conflict_term()) {
            obj["conflict_term"] = *ct;
        }

        return json_to_bytes(boost::json::serialize(obj));
    }

    // Serialize InstallSnapshot Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const install_snapshot_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        boost::json::object obj;
        obj["type"] = "install_snapshot_request";
        encode_group_id(obj, req.group_id());
        obj["term"] = req.term();
        obj["leader_id"] = req.leader_id();
        obj["last_included_index"] = req.last_included_index();
        obj["last_included_term"] = req.last_included_term();
        obj["offset"] = req.offset();
        obj["data"] = bytes_to_base64(req.data());
        obj["done"] = req.done();

        return json_to_bytes(boost::json::serialize(obj));
    }

    // Serialize InstallSnapshot Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(const install_snapshot_response<TermId, GroupId>& resp) const
        -> Data {
        boost::json::object obj;
        obj["type"] = "install_snapshot_response";
        encode_group_id(obj, resp.group_id());
        obj["term"] = resp.term();

        return json_to_bytes(boost::json::serialize(obj));
    }

    // Deserialize RequestVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_vote_request(const Data& data) const
        -> request_vote_request<NodeId, TermId, LogIndex, GroupId> {
        return guarded("request_vote_request", [&] {
            const auto obj = parse_object(data, "request_vote_request");

            request_vote_request<NodeId, TermId, LogIndex, GroupId> req;
            req._group_id = decode_group_id<GroupId>(obj);
            req._term = read_int<TermId>(obj, "term");
            req._last_log_index = read_int<LogIndex>(obj, "last_log_index");
            req._last_log_term = read_int<TermId>(obj, "last_log_term");

            req._candidate_id = read_id<NodeId>(obj, "candidate_id");

            return req;
        });
    }

    // Deserialize RequestVote Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_vote_response(const Data& data) const
        -> request_vote_response<TermId, GroupId> {
        return guarded("request_vote_response", [&] {
            const auto obj = parse_object(data, "request_vote_response");

            request_vote_response<TermId, GroupId> resp;
            resp._group_id = decode_group_id<GroupId>(obj);
            resp._term = read_int<TermId>(obj, "term");
            resp._vote_granted = read_bool(obj, "vote_granted");

            return resp;
        });
    }

    // Deserialize RequestPreVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_pre_vote_request(const Data& data) const
        -> request_pre_vote_request<NodeId, TermId, LogIndex, GroupId> {
        return guarded("request_pre_vote_request", [&] {
            const auto obj = parse_object(data, "request_pre_vote_request");

            request_pre_vote_request<NodeId, TermId, LogIndex, GroupId> req;
            req._group_id = decode_group_id<GroupId>(obj);
            req._term = read_int<TermId>(obj, "term");
            req._last_log_index = read_int<LogIndex>(obj, "last_log_index");
            req._last_log_term = read_int<TermId>(obj, "last_log_term");

            req._candidate_id = read_id<NodeId>(obj, "candidate_id");

            return req;
        });
    }

    // Deserialize RequestPreVote Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_pre_vote_response(const Data& data) const
        -> request_pre_vote_response<TermId, GroupId> {
        return guarded("request_pre_vote_response", [&] {
            const auto obj = parse_object(data, "request_pre_vote_response");

            request_pre_vote_response<TermId, GroupId> resp;
            resp._group_id = decode_group_id<GroupId>(obj);
            resp._term = read_int<TermId>(obj, "term");
            resp._vote_granted = read_bool(obj, "vote_granted");

            return resp;
        });
    }

    // Deserialize TimeoutNow Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_timeout_now_request(const Data& data) const
        -> timeout_now_request<NodeId, TermId, LogIndex, GroupId> {
        return guarded("timeout_now_request", [&] {
            const auto obj = parse_object(data, "timeout_now_request");

            timeout_now_request<NodeId, TermId, LogIndex, GroupId> req;
            req._group_id = decode_group_id<GroupId>(obj);
            req._term = read_int<TermId>(obj, "term");
            req._last_log_index = read_int<LogIndex>(obj, "last_log_index");

            req._leader_id = read_id<NodeId>(obj, "leader_id");

            return req;
        });
    }

    // Deserialize TimeoutNow Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_timeout_now_response(const Data& data) const
        -> timeout_now_response<TermId, GroupId> {
        return guarded("timeout_now_response", [&] {
            const auto obj = parse_object(data, "timeout_now_response");

            timeout_now_response<TermId, GroupId> resp;
            resp._group_id = decode_group_id<GroupId>(obj);
            resp._term = read_int<TermId>(obj, "term");
            resp._success = read_bool(obj, "success");

            return resp;
        });
    }

    // Deserialize AppendEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>,
             typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_append_entries_request(const Data& data) const
        -> append_entries_request<NodeId, TermId, LogIndex, LogEntry, GroupId> {
        return guarded("append_entries_request", [&] {
            const auto obj = parse_object(data, "append_entries_request");

            append_entries_request<NodeId, TermId, LogIndex, LogEntry, GroupId> req;
            req._group_id = decode_group_id<GroupId>(obj);
            req._term = read_int<TermId>(obj, "term");
            req._prev_log_index = read_int<LogIndex>(obj, "prev_log_index");
            req._prev_log_term = read_int<TermId>(obj, "prev_log_term");
            req._leader_commit = read_int<LogIndex>(obj, "leader_commit");

            req._leader_id = read_id<NodeId>(obj, "leader_id");

            req._entries = read_entries<LogEntry, TermId, LogIndex>(obj);

            return req;
        });
    }

    // Deserialize AppendEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_append_entries_response(const Data& data) const
        -> append_entries_response<TermId, LogIndex, GroupId> {
        return guarded("append_entries_response", [&] {
            const auto obj = parse_object(data, "append_entries_response");

            append_entries_response<TermId, LogIndex, GroupId> resp;
            resp._group_id = decode_group_id<GroupId>(obj);
            resp._term = read_int<TermId>(obj, "term");
            resp._success = read_bool(obj, "success");

            if (obj.contains("conflict_index")) {
                resp._conflict_index = read_int<LogIndex>(obj, "conflict_index");
            }

            if (obj.contains("conflict_term")) {
                resp._conflict_term = read_int<TermId>(obj, "conflict_term");
            }

            return resp;
        });
    }

    // Deserialize InstallSnapshot Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_install_snapshot_request(const Data& data) const
        -> install_snapshot_request<NodeId, TermId, LogIndex, GroupId> {
        return guarded("install_snapshot_request", [&] {
            const auto obj = parse_object(data, "install_snapshot_request");

            install_snapshot_request<NodeId, TermId, LogIndex, GroupId> req;
            req._group_id = decode_group_id<GroupId>(obj);
            req._term = read_int<TermId>(obj, "term");
            req._last_included_index = read_int<LogIndex>(obj, "last_included_index");
            req._last_included_term = read_int<TermId>(obj, "last_included_term");
            req._offset = read_int<std::size_t>(obj, "offset");
            req._data = read_bytes(obj, "data");
            req._done = read_bool(obj, "done");

            req._leader_id = read_id<NodeId>(obj, "leader_id");

            return req;
        });
    }

    // Deserialize InstallSnapshot Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_install_snapshot_response(const Data& data) const
        -> install_snapshot_response<TermId, GroupId> {
        return guarded("install_snapshot_response", [&] {
            const auto obj = parse_object(data, "install_snapshot_response");

            install_snapshot_response<TermId, GroupId> resp;
            resp._group_id = decode_group_id<GroupId>(obj);
            resp._term = read_int<TermId>(obj, "term");

            return resp;
        });
    }

    // Serialize ClusterJoin Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_join_request<NodeId, Address>& req) const -> Data {
        boost::json::object obj;
        obj["type"] = "cluster_join_request";
        if constexpr (std::same_as<NodeId, std::string>) {
            obj["node_id"] = req.node_id;
        } else {
            obj["node_id"] = req.node_id;
        }
        obj["contact_address"] = req.contact_address;
        return json_to_bytes(boost::json::serialize(obj));
    }

    // Serialize ClusterJoin Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_join_response<NodeId, Address>& resp) const -> Data {
        boost::json::object obj;
        obj["type"] = "cluster_join_response";
        obj["accepted"] = resp.accepted;
        if (resp.redirect.has_value()) {
            if constexpr (std::same_as<NodeId, std::string>) {
                obj["redirect_node_id"] = resp.redirect->node_id;
            } else {
                obj["redirect_node_id"] = resp.redirect->node_id;
            }
            obj["redirect_address"] = resp.redirect->address;
        }
        return json_to_bytes(boost::json::serialize(obj));
    }

    // Deserialize ClusterJoin Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_join_request(const Data& data) const
        -> cluster_join_request<NodeId, Address> {
        return guarded("cluster_join_request", [&] {
            const auto obj = parse_object(data, "cluster_join_request");

            cluster_join_request<NodeId, Address> req;
            req.node_id = read_id<NodeId>(obj, "node_id");
            req.contact_address = read_string(obj, "contact_address");
            return req;
        });
    }

    // Deserialize ClusterJoin Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_join_response(const Data& data) const
        -> cluster_join_response<NodeId, Address> {
        return guarded("cluster_join_response", [&] {
            const auto obj = parse_object(data, "cluster_join_response");

            cluster_join_response<NodeId, Address> resp;
            resp.accepted = read_bool(obj, "accepted");
            if (obj.contains("redirect_node_id")) {
                peer_info<NodeId, Address> pi;
                pi.node_id = read_id<NodeId>(obj, "redirect_node_id");
                pi.address = read_string(obj, "redirect_address");
                resp.redirect = pi;
            }
            return resp;
        });
    }

    // Serialize ClusterLeave Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_leave_request<NodeId, Address>& req) const -> Data {
        boost::json::object obj;
        obj["type"] = "cluster_leave_request";
        obj["node_id"] = req.node_id;
        return json_to_bytes(boost::json::serialize(obj));
    }

    // Serialize ClusterLeave Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_leave_response<NodeId, Address>& resp) const
        -> Data {
        boost::json::object obj;
        obj["type"] = "cluster_leave_response";
        obj["accepted"] = resp.accepted;
        if (resp.redirect.has_value()) {
            obj["redirect_node_id"] = resp.redirect->node_id;
            obj["redirect_address"] = resp.redirect->address;
        }
        return json_to_bytes(boost::json::serialize(obj));
    }

    // Deserialize ClusterLeave Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_leave_request(const Data& data) const
        -> cluster_leave_request<NodeId, Address> {
        return guarded("cluster_leave_request", [&] {
            const auto obj = parse_object(data, "cluster_leave_request");

            cluster_leave_request<NodeId, Address> req;
            req.node_id = read_id<NodeId>(obj, "node_id");
            return req;
        });
    }

    // Deserialize ClusterLeave Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_leave_response(const Data& data) const
        -> cluster_leave_response<NodeId, Address> {
        return guarded("cluster_leave_response", [&] {
            const auto obj = parse_object(data, "cluster_leave_response");

            cluster_leave_response<NodeId, Address> resp;
            resp.accepted = read_bool(obj, "accepted");
            if (obj.contains("redirect_node_id")) {
                peer_info<NodeId, Address> pi;
                pi.node_id = read_id<NodeId>(obj, "redirect_node_id");
                pi.address = read_string(obj, "redirect_address");
                resp.redirect = pi;
            }
            return resp;
        });
    }

    // Serialize FetchLogEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const fetch_log_entries_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        boost::json::object obj;
        obj["type"] = "fetch_log_entries_request";
        encode_group_id(obj, req.group_id());
        obj["requester_id"] = req.requester_id();
        obj["from_index"] = req.from_index();
        obj["to_index"] = req.to_index();
        return json_to_bytes(boost::json::serialize(obj));
    }

    // Serialize FetchLogEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename LogEntry = log_entry<TermId, LogIndex>, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const fetch_log_entries_response<TermId, LogIndex, LogEntry, GroupId>& resp) const -> Data {
        boost::json::object obj;
        obj["type"] = "fetch_log_entries_response";
        encode_group_id(obj, resp.group_id());
        obj["responder_id"] = resp.responder_id();
        obj["available"] = resp.available();
        obj["prev_log_term"] = resp.prev_log_term();

        boost::json::array entries_array;
        for (const auto& entry : resp.entries()) {
            boost::json::object entry_obj;
            entry_obj["term"] = entry.term();
            entry_obj["index"] = entry.index();
            entry_obj["command"] = bytes_to_base64(entry.command());
            entry_obj["entry_type"] = static_cast<int>(entry.type());
            entries_array.push_back(entry_obj);
        }
        obj["entries"] = entries_array;

        return json_to_bytes(boost::json::serialize(obj));
    }

    // Deserialize FetchLogEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_fetch_log_entries_request(const Data& data) const
        -> fetch_log_entries_request<NodeId, TermId, LogIndex, GroupId> {
        return guarded("fetch_log_entries_request", [&] {
            const auto obj = parse_object(data, "fetch_log_entries_request");

            fetch_log_entries_request<NodeId, TermId, LogIndex, GroupId> req;
            req._group_id = decode_group_id<GroupId>(obj);
            req._requester_id = read_id<NodeId>(obj, "requester_id");
            req._from_index = read_int<LogIndex>(obj, "from_index");
            req._to_index = read_int<LogIndex>(obj, "to_index");
            return req;
        });
    }

    // Deserialize FetchLogEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename LogEntry = log_entry<TermId, LogIndex>, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_fetch_log_entries_response(const Data& data) const
        -> fetch_log_entries_response<TermId, LogIndex, LogEntry, GroupId> {
        return guarded("fetch_log_entries_response", [&] {
            const auto obj = parse_object(data, "fetch_log_entries_response");

            fetch_log_entries_response<TermId, LogIndex, LogEntry, GroupId> resp;
            resp._group_id = decode_group_id<GroupId>(obj);
            resp._responder_id = read_int<std::uint64_t>(obj, "responder_id");
            resp._available = read_bool(obj, "available");
            resp._prev_log_term = read_int<TermId>(obj, "prev_log_term");

            resp._entries = read_entries<LogEntry, TermId, LogIndex>(obj);

            return resp;
        });
    }

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
        } else if constexpr (std::same_as<T, timeout_now_request<>>) {
            return deserialize_timeout_now_request(data);
        } else if constexpr (std::same_as<T, timeout_now_response<>>) {
            return deserialize_timeout_now_response(data);
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
    [[nodiscard]] auto name() const -> std::string { return "json"; }

    /// @brief IANA media type used for HTTP `Content-Type`/`Accept` negotiation
    ///        and CoAP Content-Format mapping (`rpc_serializer`).
    [[nodiscard]] auto media_type() const -> std::string { return "application/json"; }

private:
    // Helper to convert JSON string to bytes
    [[nodiscard]] auto json_to_bytes(const std::string& json_str) const -> Data {
        Data result;
        if constexpr (requires { result.resize(0); }) {
            result.resize(json_str.size());
            std::transform(json_str.begin(), json_str.end(), result.begin(),
                           [](char c) { return static_cast<std::byte>(c); });
        }
        return result;
    }

    // ── checked decoding (.kiro/specs/json-serializer-input-validation) ─────
    //
    // Every field read on a decode path goes through these accessors, so a
    // missing field, a wrong JSON kind, an out-of-range number or bad base64
    // surfaces as serialization_exception naming the field, never as a
    // wrapped-around value or a Boost.JSON exception.

    [[noreturn]] static auto fail(std::string_view key, std::string_view reason) -> void {
        throw serialization_exception("JSON decode: field '" + std::string(key) +
                                      "': " + std::string(reason));
    }

    [[noreturn]] static auto fail_kind(std::string_view key, std::string_view expected,
                                       const boost::json::value& v) -> void {
        fail(key, "expected " + std::string(expected) + ", got " +
                      std::string(boost::json::to_string(v.kind())));
    }

    // Converts any library exception escaping `decode` into
    // serialization_exception. std::bad_alloc passes through so memory pressure
    // is not reported as a protocol error.
    template<typename F>
    static auto guarded(const char* message_type, F&& decode) -> decltype(decode()) {
        try {
            return std::forward<F>(decode)();
        } catch (const serialization_exception&) {
            throw;
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& e) {
            throw serialization_exception(std::string("JSON decode (") + message_type +
                                          "): " + e.what());
        }
    }

    // Parses `data` as a JSON object and checks its "type" discriminant.
    [[nodiscard]] static auto parse_object(const Data& data, std::string_view expected_type)
        -> boost::json::object {
        std::string text;
        text.reserve(std::ranges::size(data));
        for (auto b : data) {
            text.push_back(static_cast<char>(b));
        }

        boost::system::error_code ec;
        auto parsed = boost::json::parse(text, ec);
        if (ec) {
            throw serialization_exception("JSON decode (" + std::string(expected_type) +
                                          "): invalid JSON: " + ec.message());
        }
        if (!parsed.is_object()) {
            throw serialization_exception("JSON decode (" + std::string(expected_type) +
                                          "): top-level value is not an object");
        }
        auto obj = std::move(parsed.get_object());
        if (read_string(obj, "type") != expected_type) {
            throw serialization_exception("Invalid message type for " + std::string(expected_type));
        }
        return obj;
    }

    // Uses if_contains rather than operator[], which would insert a null.
    [[nodiscard]] static auto require(const boost::json::object& obj, std::string_view key)
        -> const boost::json::value& {
        const auto* v = obj.if_contains(key);
        if (v == nullptr) {
            fail(key, "missing");
        }
        return *v;
    }

    // Accepts both Boost.JSON integer kinds: the parser yields kind::uint64
    // for values at or above 2^63, which the encoder emits for large
    // std::uint64_t fields. Doubles are rejected even when integral; the
    // encoder never emits them and accepting them would admit rounding.
    template<std::integral Target>
    [[nodiscard]] static auto to_int(const boost::json::value& v, std::string_view key) -> Target {
        switch (v.kind()) {
            case boost::json::kind::int64: {
                const std::int64_t i = v.get_int64();
                if (std::is_unsigned_v<Target> && i < 0) {
                    fail(key, "negative value for unsigned field");
                }
                if (!std::in_range<Target>(i)) {
                    fail(key, "value out of range");
                }
                return static_cast<Target>(i);
            }
            case boost::json::kind::uint64: {
                const std::uint64_t u = v.get_uint64();
                if (!std::in_range<Target>(u)) {
                    fail(key, "value out of range");
                }
                return static_cast<Target>(u);
            }
            default:
                fail_kind(key, "integer", v);
        }
    }

    template<typename Target>
    requires std::integral<Target> || std::is_enum_v<Target>
    [[nodiscard]] static auto read_int(const boost::json::object& obj, std::string_view key)
        -> Target {
        if constexpr (std::is_enum_v<Target>) {
            return static_cast<Target>(
                to_int<std::underlying_type_t<Target>>(require(obj, key), key));
        } else {
            return to_int<Target>(require(obj, key), key);
        }
    }

    [[nodiscard]] static auto read_bool(const boost::json::object& obj, std::string_view key)
        -> bool {
        const auto& v = require(obj, key);
        if (!v.is_bool()) {
            fail_kind(key, "bool", v);
        }
        return v.get_bool();
    }

    [[nodiscard]] static auto read_string(const boost::json::object& obj, std::string_view key)
        -> std::string {
        const auto& v = require(obj, key);
        if (!v.is_string()) {
            fail_kind(key, "string", v);
        }
        return std::string(v.get_string());
    }

    [[nodiscard]] static auto read_array(const boost::json::object& obj, std::string_view key)
        -> const boost::json::array& {
        const auto& v = require(obj, key);
        if (!v.is_array()) {
            fail_kind(key, "array", v);
        }
        return v.get_array();
    }

    [[nodiscard]] static auto read_object(const boost::json::value& v, std::string_view what)
        -> const boost::json::object& {
        if (!v.is_object()) {
            fail_kind(what, "object", v);
        }
        return v.get_object();
    }

    [[nodiscard]] static auto read_bytes(const boost::json::object& obj, std::string_view key)
        -> std::vector<std::byte> {
        const auto& v = require(obj, key);
        if (!v.is_string()) {
            fail_kind(key, "base64 string", v);
        }
        return base64_to_bytes(v.get_string(), key);
    }

    template<typename Id>
    [[nodiscard]] static auto read_id(const boost::json::object& obj, std::string_view key) -> Id {
        if constexpr (std::same_as<Id, std::string>) {
            return read_string(obj, key);
        } else {
            return read_int<Id>(obj, key);
        }
    }

    template<typename LogEntry, typename TermId, typename LogIndex>
    [[nodiscard]] static auto read_entries(const boost::json::object& obj)
        -> std::vector<LogEntry> {
        std::vector<LogEntry> entries;
        for (const auto& entry_val : read_array(obj, "entries")) {
            const auto& entry_obj = read_object(entry_val, "entries[]");
            LogEntry entry;
            entry._term = read_int<TermId>(entry_obj, "term");
            entry._index = read_int<LogIndex>(entry_obj, "index");
            entry._command = read_bytes(entry_obj, "command");
            entry._type = entry_obj.contains("entry_type")
                              ? read_int<entry_type>(entry_obj, "entry_type")
                              : entry_type::normal;
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    // Helper to convert bytes to base64. The accumulator is unsigned and
    // masked to the bits still pending, so no shift can overflow.
    [[nodiscard]] static auto bytes_to_base64(const std::vector<std::byte>& data) -> std::string {
        static const char* base64_chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789+/";

        std::string result;
        std::uint32_t val = 0;
        int valb = -6;

        for (auto b : data) {
            val = (val << 8) | static_cast<unsigned char>(b);
            valb += 8;
            while (valb >= 0) {
                result.push_back(base64_chars[(val >> valb) & 0x3F]);
                valb -= 6;
            }
            val &= (1u << (valb + 6)) - 1u;
        }

        if (valb > -6) {
            result.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
        }

        while (result.size() % 4 != 0) {
            result.push_back('=');
        }

        return result;
    }

    // Strict RFC 4648 §4 decoder: the whole string is validated, so a damaged
    // field is rejected instead of truncated. Only the canonical form that
    // bytes_to_base64 emits is accepted (zero pad bits), so each byte string
    // has exactly one encoding.
    [[nodiscard]] static auto base64_to_bytes(std::string_view base64, std::string_view key)
        -> std::vector<std::byte> {
        static const unsigned char base64_table[256] = {
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62,
            64, 64, 64, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64, 64, 0,
            1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
            23, 24, 25, 64, 64, 64, 64, 64, 64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
            39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64};

        const std::size_t n = base64.size();
        if (n % 4 != 0) {
            fail(key, "base64 length not a multiple of 4");
        }

        std::size_t pad = 0;
        if (n > 0 && base64[n - 1] == '=') {
            pad = base64[n - 2] == '=' ? 2 : 1;
        }

        std::vector<std::byte> result;
        result.reserve((n / 4) * 3);
        std::uint32_t val = 0;
        int valb = -8;

        for (std::size_t i = 0; i < n - pad; ++i) {
            const auto c = static_cast<unsigned char>(base64[i]);
            const unsigned char sextet = base64_table[c];
            if (sextet == 64) {
                fail(key, c == '=' ? "misplaced base64 padding" : "invalid base64 character");
            }
            val = (val << 6) | sextet;
            valb += 6;
            if (valb >= 0) {
                result.push_back(static_cast<std::byte>((val >> valb) & 0xFF));
                valb -= 8;
            }
            val &= (1u << (valb + 8)) - 1u;
        }

        if (val != 0) {
            fail(key, "non-zero base64 pad bits");
        }

        return result;
    }
};

// Verify that json_rpc_serializer satisfies the rpc_serializer concept
static_assert(rpc_serializer<json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>,
              "json_rpc_serializer must satisfy the rpc_serializer concept");

// Convenience type alias for the default JSON serializer
using json_serializer = json_rpc_serializer<std::vector<std::byte>>;

}  // namespace kythira
