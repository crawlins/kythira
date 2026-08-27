// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/types.hpp>
#include <raft/exceptions.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kythira {

/// @brief CBOR (RFC 8949) implementation of the `rpc_serializer` concept.
///
/// A drop-in alternative to `json_rpc_serializer` that encodes every Raft RPC
/// message using a hand-rolled, deliberately-minimal CBOR codec — no external
/// dependency. Only the subset of RFC 8949 the fixed Raft message shapes need
/// is supported: definite-length unsigned integers (major type 0), byte
/// strings (2), text strings (3), arrays (4), maps (5), and the boolean simple
/// values (7, `0xf4`/`0xf5`). Every other construct (indefinite-length items,
/// tags, floats, negative integers, `null`/`undefined`) is a decode error, not
/// a best-effort partial parse.
///
/// The public shape mirrors `json_rpc_serializer` exactly: one `serialize`
/// overload and one `deserialize_<message>` method per message type, a generic
/// dispatching `deserialize<T>`, and a `name()` returning `"cbor"` so the CoAP
/// transport's existing Content-Format detection resolves it to
/// `application/cbor` with no transport change.
template<typename Data>
requires std::ranges::range<Data> && std::same_as<std::ranges::range_value_t<Data>, std::byte>
class cbor_rpc_serializer {
public:
    // ── Serialize: core four RPCs ───────────────────────────────────────────

    // Serialize RequestVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const request_vote_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 6);
        write_discriminant(out, "request_vote_request");
        write_text_string(out, "group_id");
        write_id(out, req.group_id());
        write_text_string(out, "term");
        write_uint(out, req.term());
        write_text_string(out, "candidate_id");
        write_id(out, req.candidate_id());
        write_text_string(out, "last_log_index");
        write_uint(out, req.last_log_index());
        write_text_string(out, "last_log_term");
        write_uint(out, req.last_log_term());
        return to_data(out);
    }

    // Serialize RequestVote Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(const request_vote_response<TermId, GroupId>& resp) const -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 4);
        write_discriminant(out, "request_vote_response");
        write_text_string(out, "group_id");
        write_id(out, resp.group_id());
        write_text_string(out, "term");
        write_uint(out, resp.term());
        write_text_string(out, "vote_granted");
        write_bool(out, resp.vote_granted());
        return to_data(out);
    }

    // Serialize RequestPreVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const request_pre_vote_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 6);
        write_discriminant(out, "request_pre_vote_request");
        write_text_string(out, "group_id");
        write_id(out, req.group_id());
        write_text_string(out, "term");
        write_uint(out, req.term());
        write_text_string(out, "candidate_id");
        write_id(out, req.candidate_id());
        write_text_string(out, "last_log_index");
        write_uint(out, req.last_log_index());
        write_text_string(out, "last_log_term");
        write_uint(out, req.last_log_term());
        return to_data(out);
    }

    // Serialize RequestPreVote Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(const request_pre_vote_response<TermId, GroupId>& resp) const
        -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 4);
        write_discriminant(out, "request_pre_vote_response");
        write_text_string(out, "group_id");
        write_id(out, resp.group_id());
        write_text_string(out, "term");
        write_uint(out, resp.term());
        write_text_string(out, "vote_granted");
        write_bool(out, resp.vote_granted());
        return to_data(out);
    }

    // Serialize TimeoutNow Request (leadership transfer, dissertation §3.10)
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const timeout_now_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 5);
        write_discriminant(out, "timeout_now_request");
        write_text_string(out, "group_id");
        write_id(out, req.group_id());
        write_text_string(out, "term");
        write_uint(out, req.term());
        write_text_string(out, "leader_id");
        write_id(out, req.leader_id());
        write_text_string(out, "last_log_index");
        write_uint(out, req.last_log_index());
        return to_data(out);
    }

    // Serialize TimeoutNow Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(const timeout_now_response<TermId, GroupId>& resp) const -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 4);
        write_discriminant(out, "timeout_now_response");
        write_text_string(out, "group_id");
        write_id(out, resp.group_id());
        write_text_string(out, "term");
        write_uint(out, resp.term());
        write_text_string(out, "success");
        write_bool(out, resp.success());
        return to_data(out);
    }

    // Serialize AppendEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>,
             typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const append_entries_request<NodeId, TermId, LogIndex, LogEntry, GroupId>& req) const
        -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 8);
        write_discriminant(out, "append_entries_request");
        write_text_string(out, "group_id");
        write_id(out, req.group_id());
        write_text_string(out, "term");
        write_uint(out, req.term());
        write_text_string(out, "leader_id");
        write_id(out, req.leader_id());
        write_text_string(out, "prev_log_index");
        write_uint(out, req.prev_log_index());
        write_text_string(out, "prev_log_term");
        write_uint(out, req.prev_log_term());
        write_text_string(out, "leader_commit");
        write_uint(out, req.leader_commit());
        write_text_string(out, "entries");
        write_entries(out, req.entries());
        return to_data(out);
    }

    // Serialize AppendEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const append_entries_response<TermId, LogIndex, GroupId>& resp) const -> Data {
        // Absent optionals are omitted map keys (never CBOR null), so the map's
        // pair count depends on which optionals are present.
        std::size_t count = 4;  // type, term, success, group_id
        if (resp.conflict_index()) {
            ++count;
        }
        if (resp.conflict_term()) {
            ++count;
        }

        std::vector<std::byte> out;
        write_map_header(out, count);
        write_discriminant(out, "append_entries_response");
        write_text_string(out, "group_id");
        write_id(out, resp.group_id());
        write_text_string(out, "term");
        write_uint(out, resp.term());
        write_text_string(out, "success");
        write_bool(out, resp.success());
        if (const auto& ci = resp.conflict_index()) {
            write_text_string(out, "conflict_index");
            write_uint(out, *ci);
        }
        if (const auto& ct = resp.conflict_term()) {
            write_text_string(out, "conflict_term");
            write_uint(out, *ct);
        }
        return to_data(out);
    }

    // Serialize InstallSnapshot Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const install_snapshot_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 9);
        write_discriminant(out, "install_snapshot_request");
        write_text_string(out, "group_id");
        write_id(out, req.group_id());
        write_text_string(out, "term");
        write_uint(out, req.term());
        write_text_string(out, "leader_id");
        write_id(out, req.leader_id());
        write_text_string(out, "last_included_index");
        write_uint(out, req.last_included_index());
        write_text_string(out, "last_included_term");
        write_uint(out, req.last_included_term());
        write_text_string(out, "offset");
        write_uint(out, req.offset());
        write_text_string(out, "data");
        write_byte_string(out, req.data());
        write_text_string(out, "done");
        write_bool(out, req.done());
        return to_data(out);
    }

    // Serialize InstallSnapshot Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(const install_snapshot_response<TermId, GroupId>& resp) const
        -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 3);
        write_discriminant(out, "install_snapshot_response");
        write_text_string(out, "group_id");
        write_id(out, resp.group_id());
        write_text_string(out, "term");
        write_uint(out, resp.term());
        return to_data(out);
    }

    // ── Serialize: bootstrap and peer-to-peer extension RPCs ────────────────

    // Serialize ClusterJoin Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_join_request<NodeId, Address>& req) const -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 3);
        write_discriminant(out, "cluster_join_request");
        write_text_string(out, "node_id");
        write_id(out, req.node_id);
        write_text_string(out, "contact_address");
        write_text_string(out, req.contact_address);
        return to_data(out);
    }

    // Serialize ClusterJoin Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_join_response<NodeId, Address>& resp) const -> Data {
        std::size_t count = 2;  // type, accepted
        if (resp.redirect.has_value()) {
            count += 2;  // redirect_node_id, redirect_address
        }

        std::vector<std::byte> out;
        write_map_header(out, count);
        write_discriminant(out, "cluster_join_response");
        write_text_string(out, "accepted");
        write_bool(out, resp.accepted);
        if (resp.redirect.has_value()) {
            write_text_string(out, "redirect_node_id");
            write_id(out, resp.redirect->node_id);
            write_text_string(out, "redirect_address");
            write_text_string(out, resp.redirect->address);
        }
        return to_data(out);
    }

    // Serialize ClusterLeave Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_leave_request<NodeId, Address>& req) const -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 2);
        write_discriminant(out, "cluster_leave_request");
        write_text_string(out, "node_id");
        write_id(out, req.node_id);
        return to_data(out);
    }

    // Serialize ClusterLeave Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_leave_response<NodeId, Address>& resp) const
        -> Data {
        std::size_t count = 2;  // type, accepted
        if (resp.redirect.has_value()) {
            count += 2;  // redirect_node_id, redirect_address
        }

        std::vector<std::byte> out;
        write_map_header(out, count);
        write_discriminant(out, "cluster_leave_response");
        write_text_string(out, "accepted");
        write_bool(out, resp.accepted);
        if (resp.redirect.has_value()) {
            write_text_string(out, "redirect_node_id");
            write_id(out, resp.redirect->node_id);
            write_text_string(out, "redirect_address");
            write_text_string(out, resp.redirect->address);
        }
        return to_data(out);
    }

    // Serialize FetchLogEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const fetch_log_entries_request<NodeId, TermId, LogIndex, GroupId>& req) const -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 5);
        write_discriminant(out, "fetch_log_entries_request");
        write_text_string(out, "group_id");
        write_id(out, req.group_id());
        write_text_string(out, "requester_id");
        write_id(out, req.requester_id());
        write_text_string(out, "from_index");
        write_uint(out, req.from_index());
        write_text_string(out, "to_index");
        write_uint(out, req.to_index());
        return to_data(out);
    }

    // Serialize FetchLogEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename LogEntry = log_entry<TermId, LogIndex>, typename GroupId = std::uint64_t>
    [[nodiscard]] auto serialize(
        const fetch_log_entries_response<TermId, LogIndex, LogEntry, GroupId>& resp) const -> Data {
        std::vector<std::byte> out;
        write_map_header(out, 6);
        write_discriminant(out, "fetch_log_entries_response");
        write_text_string(out, "group_id");
        write_id(out, resp.group_id());
        write_text_string(out, "responder_id");
        write_uint(out, resp.responder_id());
        write_text_string(out, "available");
        write_bool(out, resp.available());
        write_text_string(out, "prev_log_term");
        write_uint(out, resp.prev_log_term());
        write_text_string(out, "entries");
        write_entries(out, resp.entries());
        return to_data(out);
    }

    // ── Deserialize: core four RPCs ─────────────────────────────────────────

    // Deserialize RequestVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_vote_request(const Data& data) const
        -> request_vote_request<NodeId, TermId, LogIndex, GroupId> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "request_vote_request");

        request_vote_request<NodeId, TermId, LogIndex, GroupId> req;
        bool have_term = false, have_candidate = false, have_lli = false, have_llt = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "term") {
                req._term = read_narrowed<TermId>(cur);
                have_term = true;
            } else if (key == "candidate_id") {
                req._candidate_id = read_id<NodeId>(cur);
                have_candidate = true;
            } else if (key == "last_log_index") {
                req._last_log_index = read_narrowed<LogIndex>(cur);
                have_lli = true;
            } else if (key == "last_log_term") {
                req._last_log_term = read_narrowed<TermId>(cur);
                have_llt = true;
            } else if (key == "group_id") {
                // Absent from any payload recorded before multi-Raft. The
                // default value is exactly "the single group", so a missing
                // key is not an error and carries no required-field flag.
                req._group_id = read_id<GroupId>(cur);
            } else {
                throw serialization_exception("Unexpected key in request_vote_request: " + key);
            }
        }
        if (!have_term || !have_candidate || !have_lli || !have_llt) {
            throw serialization_exception("Missing required field in request_vote_request");
        }
        return req;
    }

    // Deserialize RequestVote Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_vote_response(const Data& data) const
        -> request_vote_response<TermId, GroupId> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "request_vote_response");

        request_vote_response<TermId, GroupId> resp;
        bool have_term = false, have_granted = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "term") {
                resp._term = read_narrowed<TermId>(cur);
                have_term = true;
            } else if (key == "vote_granted") {
                resp._vote_granted = read_bool(cur);
                have_granted = true;
            } else if (key == "group_id") {
                // Absent from any payload recorded before multi-Raft. The
                // default value is exactly "the single group", so a missing
                // key is not an error and carries no required-field flag.
                resp._group_id = read_id<GroupId>(cur);
            } else {
                throw serialization_exception("Unexpected key in request_vote_response: " + key);
            }
        }
        if (!have_term || !have_granted) {
            throw serialization_exception("Missing required field in request_vote_response");
        }
        return resp;
    }

    // Deserialize RequestPreVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_pre_vote_request(const Data& data) const
        -> request_pre_vote_request<NodeId, TermId, LogIndex, GroupId> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "request_pre_vote_request");

        request_pre_vote_request<NodeId, TermId, LogIndex, GroupId> req;
        bool have_term = false, have_candidate = false, have_lli = false, have_llt = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "term") {
                req._term = read_narrowed<TermId>(cur);
                have_term = true;
            } else if (key == "candidate_id") {
                req._candidate_id = read_id<NodeId>(cur);
                have_candidate = true;
            } else if (key == "last_log_index") {
                req._last_log_index = read_narrowed<LogIndex>(cur);
                have_lli = true;
            } else if (key == "last_log_term") {
                req._last_log_term = read_narrowed<TermId>(cur);
                have_llt = true;
            } else if (key == "group_id") {
                // Absent from any payload recorded before multi-Raft. The
                // default value is exactly "the single group", so a missing
                // key is not an error and carries no required-field flag.
                req._group_id = read_id<GroupId>(cur);
            } else {
                throw serialization_exception("Unexpected key in request_pre_vote_request: " + key);
            }
        }
        if (!have_term || !have_candidate || !have_lli || !have_llt) {
            throw serialization_exception("Missing required field in request_pre_vote_request");
        }
        return req;
    }

    // Deserialize RequestPreVote Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_pre_vote_response(const Data& data) const
        -> request_pre_vote_response<TermId, GroupId> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "request_pre_vote_response");

        request_pre_vote_response<TermId, GroupId> resp;
        bool have_term = false, have_granted = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "term") {
                resp._term = read_narrowed<TermId>(cur);
                have_term = true;
            } else if (key == "vote_granted") {
                resp._vote_granted = read_bool(cur);
                have_granted = true;
            } else if (key == "group_id") {
                // Absent from any payload recorded before multi-Raft. The
                // default value is exactly "the single group", so a missing
                // key is not an error and carries no required-field flag.
                resp._group_id = read_id<GroupId>(cur);
            } else {
                throw serialization_exception("Unexpected key in request_pre_vote_response: " +
                                              key);
            }
        }
        if (!have_term || !have_granted) {
            throw serialization_exception("Missing required field in request_pre_vote_response");
        }
        return resp;
    }

    // Deserialize TimeoutNow Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_timeout_now_request(const Data& data) const
        -> timeout_now_request<NodeId, TermId, LogIndex, GroupId> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "timeout_now_request");

        timeout_now_request<NodeId, TermId, LogIndex, GroupId> req;
        bool have_term = false, have_leader = false, have_lli = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "term") {
                req._term = read_narrowed<TermId>(cur);
                have_term = true;
            } else if (key == "leader_id") {
                req._leader_id = read_id<NodeId>(cur);
                have_leader = true;
            } else if (key == "last_log_index") {
                req._last_log_index = read_narrowed<LogIndex>(cur);
                have_lli = true;
            } else if (key == "group_id") {
                req._group_id = read_id<GroupId>(cur);
            } else {
                throw serialization_exception("Unexpected key in timeout_now_request: " + key);
            }
        }
        if (!have_term || !have_leader || !have_lli) {
            throw serialization_exception("Missing required field in timeout_now_request");
        }
        return req;
    }

    // Deserialize TimeoutNow Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_timeout_now_response(const Data& data) const
        -> timeout_now_response<TermId, GroupId> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "timeout_now_response");

        timeout_now_response<TermId, GroupId> resp;
        bool have_term = false, have_success = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "term") {
                resp._term = read_narrowed<TermId>(cur);
                have_term = true;
            } else if (key == "success") {
                resp._success = read_bool(cur);
                have_success = true;
            } else if (key == "group_id") {
                resp._group_id = read_id<GroupId>(cur);
            } else {
                throw serialization_exception("Unexpected key in timeout_now_response: " + key);
            }
        }
        if (!have_term || !have_success) {
            throw serialization_exception("Missing required field in timeout_now_response");
        }
        return resp;
    }

    // Deserialize AppendEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>,
             typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_append_entries_request(const Data& data) const
        -> append_entries_request<NodeId, TermId, LogIndex, LogEntry, GroupId> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "append_entries_request");

        append_entries_request<NodeId, TermId, LogIndex, LogEntry, GroupId> req;
        bool have_term = false, have_leader = false, have_pli = false, have_plt = false,
             have_commit = false, have_entries = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "term") {
                req._term = read_narrowed<TermId>(cur);
                have_term = true;
            } else if (key == "leader_id") {
                req._leader_id = read_id<NodeId>(cur);
                have_leader = true;
            } else if (key == "prev_log_index") {
                req._prev_log_index = read_narrowed<LogIndex>(cur);
                have_pli = true;
            } else if (key == "prev_log_term") {
                req._prev_log_term = read_narrowed<TermId>(cur);
                have_plt = true;
            } else if (key == "leader_commit") {
                req._leader_commit = read_narrowed<LogIndex>(cur);
                have_commit = true;
            } else if (key == "entries") {
                req._entries = read_entries<TermId, LogIndex, LogEntry>(cur);
                have_entries = true;
            } else if (key == "group_id") {
                // Absent from any payload recorded before multi-Raft. The
                // default value is exactly "the single group", so a missing
                // key is not an error and carries no required-field flag.
                req._group_id = read_id<GroupId>(cur);
            } else {
                throw serialization_exception("Unexpected key in append_entries_request: " + key);
            }
        }
        if (!have_term || !have_leader || !have_pli || !have_plt || !have_commit || !have_entries) {
            throw serialization_exception("Missing required field in append_entries_request");
        }
        return req;
    }

    // Deserialize AppendEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_append_entries_response(const Data& data) const
        -> append_entries_response<TermId, LogIndex, GroupId> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "append_entries_response");

        append_entries_response<TermId, LogIndex, GroupId> resp;
        bool have_term = false, have_success = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "term") {
                resp._term = read_narrowed<TermId>(cur);
                have_term = true;
            } else if (key == "success") {
                resp._success = read_bool(cur);
                have_success = true;
            } else if (key == "conflict_index") {
                resp._conflict_index = read_narrowed<LogIndex>(cur);
            } else if (key == "conflict_term") {
                resp._conflict_term = read_narrowed<TermId>(cur);
            } else if (key == "group_id") {
                // Absent from any payload recorded before multi-Raft. The
                // default value is exactly "the single group", so a missing
                // key is not an error and carries no required-field flag.
                resp._group_id = read_id<GroupId>(cur);
            } else {
                throw serialization_exception("Unexpected key in append_entries_response: " + key);
            }
        }
        if (!have_term || !have_success) {
            throw serialization_exception("Missing required field in append_entries_response");
        }
        return resp;
    }

    // Deserialize InstallSnapshot Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_install_snapshot_request(const Data& data) const
        -> install_snapshot_request<NodeId, TermId, LogIndex, GroupId> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "install_snapshot_request");

        install_snapshot_request<NodeId, TermId, LogIndex, GroupId> req;
        bool have_term = false, have_leader = false, have_lii = false, have_lit = false,
             have_offset = false, have_data = false, have_done = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "term") {
                req._term = read_narrowed<TermId>(cur);
                have_term = true;
            } else if (key == "leader_id") {
                req._leader_id = read_id<NodeId>(cur);
                have_leader = true;
            } else if (key == "last_included_index") {
                req._last_included_index = read_narrowed<LogIndex>(cur);
                have_lii = true;
            } else if (key == "last_included_term") {
                req._last_included_term = read_narrowed<TermId>(cur);
                have_lit = true;
            } else if (key == "offset") {
                req._offset = read_narrowed<std::size_t>(cur);
                have_offset = true;
            } else if (key == "data") {
                req._data = read_byte_string(cur);
                have_data = true;
            } else if (key == "done") {
                req._done = read_bool(cur);
                have_done = true;
            } else if (key == "group_id") {
                // Absent from any payload recorded before multi-Raft. The
                // default value is exactly "the single group", so a missing
                // key is not an error and carries no required-field flag.
                req._group_id = read_id<GroupId>(cur);
            } else {
                throw serialization_exception("Unexpected key in install_snapshot_request: " + key);
            }
        }
        if (!have_term || !have_leader || !have_lii || !have_lit || !have_offset || !have_data ||
            !have_done) {
            throw serialization_exception("Missing required field in install_snapshot_request");
        }
        return req;
    }

    // Deserialize InstallSnapshot Response
    template<typename TermId = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_install_snapshot_response(const Data& data) const
        -> install_snapshot_response<TermId, GroupId> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "install_snapshot_response");

        install_snapshot_response<TermId, GroupId> resp;
        bool have_term = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "term") {
                resp._term = read_narrowed<TermId>(cur);
                have_term = true;
            } else if (key == "group_id") {
                // Absent from any payload recorded before multi-Raft. The
                // default value is exactly "the single group", so a missing
                // key is not an error and carries no required-field flag.
                resp._group_id = read_id<GroupId>(cur);
            } else {
                throw serialization_exception("Unexpected key in install_snapshot_response: " +
                                              key);
            }
        }
        if (!have_term) {
            throw serialization_exception("Missing required field in install_snapshot_response");
        }
        return resp;
    }

    // ── Deserialize: bootstrap and peer-to-peer extension RPCs ──────────────

    // Deserialize ClusterJoin Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_join_request(const Data& data) const
        -> cluster_join_request<NodeId, Address> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "cluster_join_request");

        cluster_join_request<NodeId, Address> req;
        bool have_node = false, have_address = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "node_id") {
                req.node_id = read_id<NodeId>(cur);
                have_node = true;
            } else if (key == "contact_address") {
                req.contact_address = read_text_string(cur);
                have_address = true;
            } else {
                throw serialization_exception("Unexpected key in cluster_join_request: " + key);
            }
        }
        if (!have_node || !have_address) {
            throw serialization_exception("Missing required field in cluster_join_request");
        }
        return req;
    }

    // Deserialize ClusterJoin Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_join_response(const Data& data) const
        -> cluster_join_response<NodeId, Address> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "cluster_join_response");

        cluster_join_response<NodeId, Address> resp;
        bool have_accepted = false;
        std::optional<NodeId> redirect_node;
        std::optional<Address> redirect_address;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "accepted") {
                resp.accepted = read_bool(cur);
                have_accepted = true;
            } else if (key == "redirect_node_id") {
                redirect_node = read_id<NodeId>(cur);
            } else if (key == "redirect_address") {
                redirect_address = read_text_string(cur);
            } else {
                throw serialization_exception("Unexpected key in cluster_join_response: " + key);
            }
        }
        if (!have_accepted) {
            throw serialization_exception("Missing required field in cluster_join_response");
        }
        if (redirect_node.has_value() || redirect_address.has_value()) {
            if (!redirect_node.has_value() || !redirect_address.has_value()) {
                throw serialization_exception("Incomplete redirect in cluster_join_response");
            }
            peer_info<NodeId, Address> pi;
            pi.node_id = *redirect_node;
            pi.address = *redirect_address;
            resp.redirect = pi;
        }
        return resp;
    }

    // Deserialize ClusterLeave Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_leave_request(const Data& data) const
        -> cluster_leave_request<NodeId, Address> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "cluster_leave_request");

        cluster_leave_request<NodeId, Address> req;
        bool have_node = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "node_id") {
                req.node_id = read_id<NodeId>(cur);
                have_node = true;
            } else {
                throw serialization_exception("Unexpected key in cluster_leave_request: " + key);
            }
        }
        if (!have_node) {
            throw serialization_exception("Missing required field in cluster_leave_request");
        }
        return req;
    }

    // Deserialize ClusterLeave Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_leave_response(const Data& data) const
        -> cluster_leave_response<NodeId, Address> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "cluster_leave_response");

        cluster_leave_response<NodeId, Address> resp;
        bool have_accepted = false;
        std::optional<NodeId> redirect_node;
        std::optional<Address> redirect_address;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "accepted") {
                resp.accepted = read_bool(cur);
                have_accepted = true;
            } else if (key == "redirect_node_id") {
                redirect_node = read_id<NodeId>(cur);
            } else if (key == "redirect_address") {
                redirect_address = read_text_string(cur);
            } else {
                throw serialization_exception("Unexpected key in cluster_leave_response: " + key);
            }
        }
        if (!have_accepted) {
            throw serialization_exception("Missing required field in cluster_leave_response");
        }
        if (redirect_node.has_value() || redirect_address.has_value()) {
            if (!redirect_node.has_value() || !redirect_address.has_value()) {
                throw serialization_exception("Incomplete redirect in cluster_leave_response");
            }
            peer_info<NodeId, Address> pi;
            pi.node_id = *redirect_node;
            pi.address = *redirect_address;
            resp.redirect = pi;
        }
        return resp;
    }

    // Deserialize FetchLogEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_fetch_log_entries_request(const Data& data) const
        -> fetch_log_entries_request<NodeId, TermId, LogIndex, GroupId> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "fetch_log_entries_request");

        fetch_log_entries_request<NodeId, TermId, LogIndex, GroupId> req;
        bool have_requester = false, have_from = false, have_to = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "requester_id") {
                req._requester_id = read_id<NodeId>(cur);
                have_requester = true;
            } else if (key == "from_index") {
                req._from_index = read_narrowed<LogIndex>(cur);
                have_from = true;
            } else if (key == "to_index") {
                req._to_index = read_narrowed<LogIndex>(cur);
                have_to = true;
            } else if (key == "group_id") {
                // Absent from any payload recorded before multi-Raft. The
                // default value is exactly "the single group", so a missing
                // key is not an error and carries no required-field flag.
                req._group_id = read_id<GroupId>(cur);
            } else {
                throw serialization_exception("Unexpected key in fetch_log_entries_request: " +
                                              key);
            }
        }
        if (!have_requester || !have_from || !have_to) {
            throw serialization_exception("Missing required field in fetch_log_entries_request");
        }
        return req;
    }

    // Deserialize FetchLogEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename LogEntry = log_entry<TermId, LogIndex>, typename GroupId = std::uint64_t>
    [[nodiscard]] auto deserialize_fetch_log_entries_response(const Data& data) const
        -> fetch_log_entries_response<TermId, LogIndex, LogEntry, GroupId> {
        auto buf = to_buffer(data);
        decode_cursor cur{buf.data(), buf.data() + buf.size()};

        auto pairs = read_map_header(cur);
        require_discriminant(cur, pairs, "fetch_log_entries_response");

        fetch_log_entries_response<TermId, LogIndex, LogEntry, GroupId> resp;
        bool have_responder = false, have_available = false, have_plt = false, have_entries = false;
        for (std::size_t i = 1; i < pairs; ++i) {
            auto key = read_text_string(cur);
            if (key == "responder_id") {
                resp._responder_id = read_narrowed<std::uint64_t>(cur);
                have_responder = true;
            } else if (key == "available") {
                resp._available = read_bool(cur);
                have_available = true;
            } else if (key == "prev_log_term") {
                resp._prev_log_term = read_narrowed<TermId>(cur);
                have_plt = true;
            } else if (key == "entries") {
                resp._entries = read_entries<TermId, LogIndex, LogEntry>(cur);
                have_entries = true;
            } else if (key == "group_id") {
                // Absent from any payload recorded before multi-Raft. The
                // default value is exactly "the single group", so a missing
                // key is not an error and carries no required-field flag.
                resp._group_id = read_id<GroupId>(cur);
            } else {
                throw serialization_exception("Unexpected key in fetch_log_entries_response: " +
                                              key);
            }
        }
        if (!have_responder || !have_available || !have_plt || !have_entries) {
            throw serialization_exception("Missing required field in fetch_log_entries_response");
        }
        return resp;
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
    [[nodiscard]] auto name() const -> std::string { return "cbor"; }

    /// @brief IANA media type used for HTTP `Content-Type`/`Accept` negotiation
    ///        and CoAP Content-Format mapping (`rpc_serializer`).
    ///
    /// `application/cbor` is the registration from RFC 8949 §9.1, and is the
    /// name CoAP Content-Format 60 corresponds to.
    [[nodiscard]] auto media_type() const -> std::string { return "application/cbor"; }

private:
    // ── CBOR major types (RFC 8949 §3), the accepted subset ─────────────────
    static constexpr std::uint8_t major_uint = 0;         ///< unsigned integer
    static constexpr std::uint8_t major_byte_string = 2;  ///< byte string
    static constexpr std::uint8_t major_text_string = 3;  ///< text string
    static constexpr std::uint8_t major_array = 4;        ///< array
    static constexpr std::uint8_t major_map = 5;          ///< map
    static constexpr std::uint8_t major_simple = 7;       ///< simple values (bool)

    static constexpr std::byte cbor_false{0xf4};
    static constexpr std::byte cbor_true{0xf5};

    // ── Encoding primitives (RFC 8949 §3.1) ─────────────────────────────────

    /// Emit a major-type/argument header using the shortest encoding that fits
    /// `value` — immediate (0-23), or 1/2/4/8 following big-endian bytes.
    static auto write_head(std::vector<std::byte>& out, std::uint8_t major, std::uint64_t value)
        -> void {
        const auto tag = static_cast<std::uint8_t>(major << 5);
        if (value < 24) {
            out.push_back(static_cast<std::byte>(tag | static_cast<std::uint8_t>(value)));
        } else if (value <= 0xFFULL) {
            out.push_back(static_cast<std::byte>(tag | 24));
            out.push_back(static_cast<std::byte>(value & 0xFF));
        } else if (value <= 0xFFFFULL) {
            out.push_back(static_cast<std::byte>(tag | 25));
            out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
            out.push_back(static_cast<std::byte>(value & 0xFF));
        } else if (value <= 0xFFFFFFFFULL) {
            out.push_back(static_cast<std::byte>(tag | 26));
            out.push_back(static_cast<std::byte>((value >> 24) & 0xFF));
            out.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
            out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
            out.push_back(static_cast<std::byte>(value & 0xFF));
        } else {
            out.push_back(static_cast<std::byte>(tag | 27));
            for (int shift = 56; shift >= 0; shift -= 8) {
                out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
            }
        }
    }

    static auto write_uint(std::vector<std::byte>& out, std::uint64_t value) -> void {
        write_head(out, major_uint, value);
    }

    static auto write_byte_string(std::vector<std::byte>& out, const std::vector<std::byte>& bytes)
        -> void {
        write_head(out, major_byte_string, bytes.size());
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    static auto write_text_string(std::vector<std::byte>& out, std::string_view text) -> void {
        write_head(out, major_text_string, text.size());
        for (char c : text) {
            out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
    }

    static auto write_bool(std::vector<std::byte>& out, bool value) -> void {
        out.push_back(value ? cbor_true : cbor_false);
    }

    static auto write_array_header(std::vector<std::byte>& out, std::size_t count) -> void {
        write_head(out, major_array, count);
    }

    static auto write_map_header(std::vector<std::byte>& out, std::size_t count) -> void {
        write_head(out, major_map, count);
    }

    static auto write_discriminant(std::vector<std::byte>& out, std::string_view discriminant)
        -> void {
        write_text_string(out, "type");
        write_text_string(out, discriminant);
    }

    /// Encode a `NodeId` field: unsigned integer, or text string when `std::string`.
    template<typename NodeId>
    static auto write_id(std::vector<std::byte>& out, const NodeId& id) -> void {
        if constexpr (std::same_as<NodeId, std::string>) {
            write_text_string(out, id);
        } else {
            write_uint(out, static_cast<std::uint64_t>(id));
        }
    }

    /// Encode an `entries()` container as a CBOR array of per-entry maps.
    template<typename Entries>
    static auto write_entries(std::vector<std::byte>& out, const Entries& entries) -> void {
        write_array_header(out, entries.size());
        for (const auto& entry : entries) {
            write_map_header(out, 4);
            write_text_string(out, "term");
            write_uint(out, entry.term());
            write_text_string(out, "index");
            write_uint(out, entry.index());
            write_text_string(out, "command");
            write_byte_string(out, entry.command());
            write_text_string(out, "entry_type");
            write_uint(out, static_cast<std::uint64_t>(
                                static_cast<std::underlying_type_t<entry_type>>(entry.type())));
        }
    }

    // ── Decoding cursor: sequential position tracking over the buffer ───────
    struct decode_cursor {
        const std::byte* pos;
        const std::byte* end;
    };

    /// Read one raw byte, bounds-checked.
    static auto read_byte(decode_cursor& cur) -> std::uint8_t {
        if (cur.pos >= cur.end) {
            throw serialization_exception("CBOR decode: unexpected end of input");
        }
        return static_cast<std::uint8_t>(*cur.pos++);
    }

    /// Decode an item header, returning `{major_type, argument}`. Rejects the
    /// additional-info values this codec does not support (indefinite-length /
    /// reserved: 28-31).
    static auto read_head(decode_cursor& cur) -> std::pair<std::uint8_t, std::uint64_t> {
        const std::uint8_t initial = read_byte(cur);
        const std::uint8_t major = initial >> 5;
        const std::uint8_t info = initial & 0x1f;

        std::uint64_t value = 0;
        if (info < 24) {
            value = info;
        } else if (info == 24) {
            value = read_byte(cur);
        } else if (info == 25) {
            value = (static_cast<std::uint64_t>(read_byte(cur)) << 8);
            value |= read_byte(cur);
        } else if (info == 26) {
            value = (static_cast<std::uint64_t>(read_byte(cur)) << 24);
            value |= (static_cast<std::uint64_t>(read_byte(cur)) << 16);
            value |= (static_cast<std::uint64_t>(read_byte(cur)) << 8);
            value |= read_byte(cur);
        } else if (info == 27) {
            for (int i = 0; i < 8; ++i) {
                value = (value << 8) | read_byte(cur);
            }
        } else {
            // 28-30 reserved, 31 indefinite-length — unsupported.
            throw serialization_exception("CBOR decode: unsupported additional-info value");
        }
        return {major, value};
    }

    /// Read an item header and require its major type. Returns the argument.
    static auto read_head_of(decode_cursor& cur, std::uint8_t expected_major) -> std::uint64_t {
        auto [major, value] = read_head(cur);
        if (major != expected_major) {
            throw serialization_exception("CBOR decode: unexpected major type");
        }
        return value;
    }

    static auto read_uint(decode_cursor& cur) -> std::uint64_t {
        return read_head_of(cur, major_uint);
    }

    /// Read an unsigned integer and narrow it into `Target`, rejecting overflow.
    template<typename Target> static auto read_narrowed(decode_cursor& cur) -> Target {
        return narrow<Target>(read_uint(cur));
    }

    static auto read_byte_string(decode_cursor& cur) -> std::vector<std::byte> {
        const std::uint64_t len = read_head_of(cur, major_byte_string);
        require_available(cur, len);
        std::vector<std::byte> result(cur.pos, cur.pos + len);
        cur.pos += len;
        return result;
    }

    static auto read_text_string(decode_cursor& cur) -> std::string {
        const std::uint64_t len = read_head_of(cur, major_text_string);
        require_available(cur, len);
        std::string result(reinterpret_cast<const char*>(cur.pos), static_cast<std::size_t>(len));
        cur.pos += len;
        return result;
    }

    static auto read_bool(decode_cursor& cur) -> bool {
        const std::uint8_t initial = read_byte(cur);
        if (initial == static_cast<std::uint8_t>(cbor_true)) {
            return true;
        }
        if (initial == static_cast<std::uint8_t>(cbor_false)) {
            return false;
        }
        throw serialization_exception("CBOR decode: expected boolean simple value");
    }

    static auto read_array_header(decode_cursor& cur) -> std::size_t {
        const std::uint64_t count = read_head_of(cur, major_array);
        // Every array item occupies at least one byte, so a count exceeding the
        // remaining buffer is malformed — reject before it drives any loop.
        require_available(cur, count);
        return static_cast<std::size_t>(count);
    }

    static auto read_map_header(decode_cursor& cur) -> std::size_t {
        const std::uint64_t pairs = read_head_of(cur, major_map);
        // Each pair is at least two bytes; a count exceeding the remaining
        // buffer is malformed.
        require_available(cur, pairs);
        return static_cast<std::size_t>(pairs);
    }

    /// Read and verify the leading `"type"` discriminant of a message map.
    static auto require_discriminant(decode_cursor& cur, std::size_t pairs,
                                     std::string_view expected) -> void {
        if (pairs < 1) {
            throw serialization_exception("CBOR decode: message map missing type discriminant");
        }
        auto key = read_text_string(cur);
        if (key != "type") {
            throw serialization_exception(
                "CBOR decode: first map key is not the type discriminant");
        }
        auto value = read_text_string(cur);
        if (value != expected) {
            throw serialization_exception("Invalid message type, expected " +
                                          std::string(expected));
        }
    }

    /// Decode a `NodeId` field: unsigned integer, or text string when `std::string`.
    template<typename NodeId> static auto read_id(decode_cursor& cur) -> NodeId {
        if constexpr (std::same_as<NodeId, std::string>) {
            return read_text_string(cur);
        } else {
            return narrow<NodeId>(read_uint(cur));
        }
    }

    /// Decode a CBOR array of per-entry maps into a `std::vector<LogEntry>`.
    template<typename TermId, typename LogIndex, typename LogEntry>
    static auto read_entries(decode_cursor& cur) -> std::vector<LogEntry> {
        const std::size_t count = read_array_header(cur);
        std::vector<LogEntry> entries;
        entries.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t pairs = read_map_header(cur);
            LogEntry entry;
            entry._type = entry_type::normal;
            bool have_term = false, have_index = false, have_command = false;
            for (std::size_t j = 0; j < pairs; ++j) {
                auto key = read_text_string(cur);
                if (key == "term") {
                    entry._term = narrow<TermId>(read_uint(cur));
                    have_term = true;
                } else if (key == "index") {
                    entry._index = narrow<LogIndex>(read_uint(cur));
                    have_index = true;
                } else if (key == "command") {
                    entry._command = read_byte_string(cur);
                    have_command = true;
                } else if (key == "entry_type") {
                    entry._type = static_cast<entry_type>(
                        narrow<std::underlying_type_t<entry_type>>(read_uint(cur)));
                } else {
                    throw serialization_exception("Unexpected key in log entry: " + key);
                }
            }
            if (!have_term || !have_index || !have_command) {
                throw serialization_exception("Missing required field in log entry");
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    // ── Bounds and narrowing helpers ────────────────────────────────────────

    /// Throw unless at least `count` bytes remain from the cursor position.
    static auto require_available(const decode_cursor& cur, std::uint64_t count) -> void {
        const auto remaining = static_cast<std::uint64_t>(cur.end - cur.pos);
        if (count > remaining) {
            throw serialization_exception("CBOR decode: declared length exceeds remaining input");
        }
    }

    /// Narrow a decoded `std::uint64_t` into `Target`, rejecting loss of value.
    template<typename Target> static auto narrow(std::uint64_t value) -> Target {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<Target>::max())) {
            throw serialization_exception("CBOR decode: integer value exceeds target type range");
        }
        return static_cast<Target>(value);
    }

    // ── Buffer conversion ───────────────────────────────────────────────────

    /// Convert the internal build buffer to the caller's `Data` container.
    static auto to_data(const std::vector<std::byte>& bytes) -> Data {
        Data result;
        if constexpr (requires { result.resize(0); }) {
            result.resize(bytes.size());
            std::copy(bytes.begin(), bytes.end(), result.begin());
        }
        return result;
    }

    /// Copy the (possibly non-contiguous) `Data` range into a contiguous buffer
    /// so the decode cursor can walk it with plain pointer arithmetic.
    static auto to_buffer(const Data& data) -> std::vector<std::byte> {
        return std::vector<std::byte>(std::ranges::begin(data), std::ranges::end(data));
    }
};

// Verify that cbor_rpc_serializer satisfies the rpc_serializer concept
static_assert(rpc_serializer<cbor_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>,
              "cbor_rpc_serializer must satisfy the rpc_serializer concept");

// Convenience type alias for the default CBOR serializer
using cbor_serializer = cbor_rpc_serializer<std::vector<std::byte>>;

}  // namespace kythira
