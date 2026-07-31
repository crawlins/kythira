#pragma once

#include <raft/types.hpp>
#include <raft/exceptions.hpp>

#include <ionc/ion.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace kythira {

/// @brief Selects which Ion encoding `ion_rpc_serializer` writes.
///
/// Deserialize is always encoding-agnostic — Ion's binary version marker
/// (`0xE0 0x01 0x00 0xEA`) makes the two encodings self-describing on read, so a
/// binary-writing peer and a text-writing peer interoperate without negotiating
/// format out of band (Requirement 4.4).
enum class ion_encoding : std::uint8_t {
    binary,  ///< Compact, self-identifying binary Ion — the production default.
    text,    ///< Human-readable text Ion (a JSON superset) for logs / manual inspection.
};

namespace detail {

/// @brief RAII owner for an `ion-c` writer handle.
///
/// `ion-c`'s `ion_writer_open_*`/`ion_writer_close` are paired C-style
/// open/close calls, not RAII. Wrapping the handle so its destructor calls
/// `ion_writer_close` means a `serialization_exception` thrown mid-encode still
/// releases the writer via stack unwinding rather than leaking it
/// (Requirement 5.6).
struct ion_writer_handle {
    hWRITER handle = nullptr;

    ion_writer_handle() = default;
    explicit ion_writer_handle(hWRITER h) : handle(h) {}
    ion_writer_handle(const ion_writer_handle&) = delete;
    auto operator=(const ion_writer_handle&) -> ion_writer_handle& = delete;
    ion_writer_handle(ion_writer_handle&&) = delete;
    auto operator=(ion_writer_handle&&) -> ion_writer_handle& = delete;

    ~ion_writer_handle() {
        if (handle != nullptr) {
            ion_writer_close(handle);
        }
    }
};

/// @brief RAII owner for an `ion-c` reader handle (see `ion_writer_handle`).
struct ion_reader_handle {
    hREADER handle = nullptr;

    ion_reader_handle() = default;
    explicit ion_reader_handle(hREADER h) : handle(h) {}
    ion_reader_handle(const ion_reader_handle&) = delete;
    auto operator=(const ion_reader_handle&) -> ion_reader_handle& = delete;
    ion_reader_handle(ion_reader_handle&&) = delete;
    auto operator=(ion_reader_handle&&) -> ion_reader_handle& = delete;

    ~ion_reader_handle() {
        if (handle != nullptr) {
            ion_reader_close(handle);
        }
    }
};

}  // namespace detail

/// @brief Amazon Ion implementation of the `rpc_serializer` concept.
///
/// A drop-in alternative to `json_rpc_serializer` (`include/raft/json_serializer.hpp`)
/// that encodes every Raft RPC message using Amazon Ion via the `ion-c` C library.
/// The public surface — one `serialize` overload and one `deserialize_<message>`
/// method per message type, a generic dispatching `deserialize<T>`, and a
/// `name()` — matches `json_rpc_serializer` exactly, so a call site switches
/// serializers by changing only the type; the sole addition is the constructor's
/// `ion_encoding` selector.
///
/// Design highlights (see `.kiro/specs/ion-rpc-serializer/design.md`):
///   - The RPC message type is carried as a single Ion **annotation** on the
///     top-level struct rather than a `"type"` string field, since Ion provides
///     annotations as the idiomatic type-tag mechanism (Requirement 3.1).
///   - `command`/`data` byte payloads are written as native Ion `blob`s with no
///     base64 detour (Requirement 3.2) — the concrete advantage over the JSON
///     serializer's base64-in-string encoding.
///   - Binary is the default write encoding; deserialize accepts either
///     encoding from a single code path (Requirement 4).
template<typename Data>
requires std::ranges::range<Data> && std::same_as<std::ranges::range_value_t<Data>, std::byte>
class ion_rpc_serializer {
public:
    explicit ion_rpc_serializer(ion_encoding encoding = ion_encoding::binary)
        : _encoding(encoding) {}

    // ── Serialize: core RPCs ────────────────────────────────────────────────

    // Serialize RequestVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto serialize(const request_vote_request<NodeId, TermId, LogIndex>& req) const
        -> Data {
        return encode(base_estimate, [&](hWRITER w) {
            write_annotation(w, "request_vote_request");
            start_struct(w);
            write_field_int(w, "term", static_cast<std::int64_t>(req.term()));
            write_field_id(w, "candidate_id", req.candidate_id());
            write_field_int(w, "last_log_index", static_cast<std::int64_t>(req.last_log_index()));
            write_field_int(w, "last_log_term", static_cast<std::int64_t>(req.last_log_term()));
            finish_struct(w);
        });
    }

    // Serialize RequestVote Response
    template<typename TermId = std::uint64_t>
    [[nodiscard]] auto serialize(const request_vote_response<TermId>& resp) const -> Data {
        return encode(base_estimate, [&](hWRITER w) {
            write_annotation(w, "request_vote_response");
            start_struct(w);
            write_field_int(w, "term", static_cast<std::int64_t>(resp.term()));
            write_field_bool(w, "vote_granted", resp.vote_granted());
            finish_struct(w);
        });
    }

    // Serialize RequestPreVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto serialize(
        const request_pre_vote_request<NodeId, TermId, LogIndex>& req) const -> Data {
        return encode(base_estimate, [&](hWRITER w) {
            write_annotation(w, "request_pre_vote_request");
            start_struct(w);
            write_field_int(w, "term", static_cast<std::int64_t>(req.term()));
            write_field_id(w, "candidate_id", req.candidate_id());
            write_field_int(w, "last_log_index", static_cast<std::int64_t>(req.last_log_index()));
            write_field_int(w, "last_log_term", static_cast<std::int64_t>(req.last_log_term()));
            finish_struct(w);
        });
    }

    // Serialize RequestPreVote Response
    template<typename TermId = std::uint64_t>
    [[nodiscard]] auto serialize(const request_pre_vote_response<TermId>& resp) const -> Data {
        return encode(base_estimate, [&](hWRITER w) {
            write_annotation(w, "request_pre_vote_response");
            start_struct(w);
            write_field_int(w, "term", static_cast<std::int64_t>(resp.term()));
            write_field_bool(w, "vote_granted", resp.vote_granted());
            finish_struct(w);
        });
    }

    // Serialize AppendEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>>
    [[nodiscard]] auto serialize(
        const append_entries_request<NodeId, TermId, LogIndex, LogEntry>& req) const -> Data {
        return encode(base_estimate + entries_estimate(req.entries()), [&](hWRITER w) {
            write_annotation(w, "append_entries_request");
            start_struct(w);
            write_field_int(w, "term", static_cast<std::int64_t>(req.term()));
            write_field_id(w, "leader_id", req.leader_id());
            write_field_int(w, "prev_log_index", static_cast<std::int64_t>(req.prev_log_index()));
            write_field_int(w, "prev_log_term", static_cast<std::int64_t>(req.prev_log_term()));
            write_field_int(w, "leader_commit", static_cast<std::int64_t>(req.leader_commit()));
            write_field_entries(w, "entries", req.entries());
            finish_struct(w);
        });
    }

    // Serialize AppendEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
    [[nodiscard]] auto serialize(const append_entries_response<TermId, LogIndex>& resp) const
        -> Data {
        return encode(base_estimate, [&](hWRITER w) {
            write_annotation(w, "append_entries_response");
            start_struct(w);
            write_field_int(w, "term", static_cast<std::int64_t>(resp.term()));
            write_field_bool(w, "success", resp.success());
            // Absent optionals are omitted struct fields (never Ion null), so
            // deserialize maps their absence back to std::nullopt (Requirement 2.5).
            if (const auto& ci = resp.conflict_index()) {
                write_field_int(w, "conflict_index", static_cast<std::int64_t>(*ci));
            }
            if (const auto& ct = resp.conflict_term()) {
                write_field_int(w, "conflict_term", static_cast<std::int64_t>(*ct));
            }
            finish_struct(w);
        });
    }

    // Serialize InstallSnapshot Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto serialize(
        const install_snapshot_request<NodeId, TermId, LogIndex>& req) const -> Data {
        return encode(base_estimate + req.data().size() * 2, [&](hWRITER w) {
            write_annotation(w, "install_snapshot_request");
            start_struct(w);
            write_field_int(w, "term", static_cast<std::int64_t>(req.term()));
            write_field_id(w, "leader_id", req.leader_id());
            write_field_int(w, "last_included_index",
                            static_cast<std::int64_t>(req.last_included_index()));
            write_field_int(w, "last_included_term",
                            static_cast<std::int64_t>(req.last_included_term()));
            write_field_int(w, "offset", static_cast<std::int64_t>(req.offset()));
            write_field_blob(w, "data", req.data());
            write_field_bool(w, "done", req.done());
            finish_struct(w);
        });
    }

    // Serialize InstallSnapshot Response
    template<typename TermId = std::uint64_t>
    [[nodiscard]] auto serialize(const install_snapshot_response<TermId>& resp) const -> Data {
        return encode(base_estimate, [&](hWRITER w) {
            write_annotation(w, "install_snapshot_response");
            start_struct(w);
            write_field_int(w, "term", static_cast<std::int64_t>(resp.term()));
            finish_struct(w);
        });
    }

    // ── Serialize: bootstrap and peer-to-peer extension RPCs ────────────────

    // Serialize ClusterJoin Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_join_request<NodeId, Address>& req) const -> Data {
        return encode(base_estimate, [&](hWRITER w) {
            write_annotation(w, "cluster_join_request");
            start_struct(w);
            write_field_id(w, "node_id", req.node_id);
            write_field_string(w, "contact_address", req.contact_address);
            finish_struct(w);
        });
    }

    // Serialize ClusterJoin Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_join_response<NodeId, Address>& resp) const -> Data {
        return encode(base_estimate, [&](hWRITER w) {
            write_annotation(w, "cluster_join_response");
            start_struct(w);
            write_field_bool(w, "accepted", resp.accepted);
            if (resp.redirect.has_value()) {
                write_field_redirect(w, *resp.redirect);
            }
            finish_struct(w);
        });
    }

    // Serialize ClusterLeave Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_leave_request<NodeId, Address>& req) const -> Data {
        return encode(base_estimate, [&](hWRITER w) {
            write_annotation(w, "cluster_leave_request");
            start_struct(w);
            write_field_id(w, "node_id", req.node_id);
            finish_struct(w);
        });
    }

    // Serialize ClusterLeave Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto serialize(const cluster_leave_response<NodeId, Address>& resp) const
        -> Data {
        return encode(base_estimate, [&](hWRITER w) {
            write_annotation(w, "cluster_leave_response");
            start_struct(w);
            write_field_bool(w, "accepted", resp.accepted);
            if (resp.redirect.has_value()) {
                write_field_redirect(w, *resp.redirect);
            }
            finish_struct(w);
        });
    }

    // Serialize FetchLogEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto serialize(
        const fetch_log_entries_request<NodeId, TermId, LogIndex>& req) const -> Data {
        return encode(base_estimate, [&](hWRITER w) {
            write_annotation(w, "fetch_log_entries_request");
            start_struct(w);
            write_field_id(w, "requester_id", req.requester_id());
            write_field_int(w, "from_index", static_cast<std::int64_t>(req.from_index()));
            write_field_int(w, "to_index", static_cast<std::int64_t>(req.to_index()));
            finish_struct(w);
        });
    }

    // Serialize FetchLogEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename LogEntry = log_entry<TermId, LogIndex>>
    [[nodiscard]] auto serialize(
        const fetch_log_entries_response<TermId, LogIndex, LogEntry>& resp) const -> Data {
        return encode(base_estimate + entries_estimate(resp.entries()), [&](hWRITER w) {
            write_annotation(w, "fetch_log_entries_response");
            start_struct(w);
            write_field_int(w, "responder_id", static_cast<std::int64_t>(resp.responder_id()));
            write_field_bool(w, "available", resp.available());
            write_field_int(w, "prev_log_term", static_cast<std::int64_t>(resp.prev_log_term()));
            write_field_entries(w, "entries", resp.entries());
            finish_struct(w);
        });
    }

    // ── Deserialize: core RPCs ──────────────────────────────────────────────

    // Deserialize RequestVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto deserialize_request_vote_request(const Data& data) const
        -> request_vote_request<NodeId, TermId, LogIndex> {
        request_vote_request<NodeId, TermId, LogIndex> req{};
        bool have_term = false, have_candidate = false, have_lli = false, have_llt = false;
        decode(data, "request_vote_request", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "term") {
                    req._term = read_int<TermId>(r, t);
                    have_term = true;
                } else if (key == "candidate_id") {
                    req._candidate_id = read_id<NodeId>(r, t);
                    have_candidate = true;
                } else if (key == "last_log_index") {
                    req._last_log_index = read_int<LogIndex>(r, t);
                    have_lli = true;
                } else if (key == "last_log_term") {
                    req._last_log_term = read_int<TermId>(r, t);
                    have_llt = true;
                } else {
                    throw serialization_exception("Unexpected field in request_vote_request: " +
                                                  key);
                }
            });
        });
        if (!have_term || !have_candidate || !have_lli || !have_llt) {
            throw serialization_exception("Missing required field in request_vote_request");
        }
        return req;
    }

    // Deserialize RequestVote Response
    template<typename TermId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_vote_response(const Data& data) const
        -> request_vote_response<TermId> {
        request_vote_response<TermId> resp{};
        bool have_term = false, have_granted = false;
        decode(data, "request_vote_response", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "term") {
                    resp._term = read_int<TermId>(r, t);
                    have_term = true;
                } else if (key == "vote_granted") {
                    resp._vote_granted = read_bool(r, t);
                    have_granted = true;
                } else {
                    throw serialization_exception("Unexpected field in request_vote_response: " +
                                                  key);
                }
            });
        });
        if (!have_term || !have_granted) {
            throw serialization_exception("Missing required field in request_vote_response");
        }
        return resp;
    }

    // Deserialize RequestPreVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto deserialize_request_pre_vote_request(const Data& data) const
        -> request_pre_vote_request<NodeId, TermId, LogIndex> {
        request_pre_vote_request<NodeId, TermId, LogIndex> req{};
        bool have_term = false, have_candidate = false, have_lli = false, have_llt = false;
        decode(data, "request_pre_vote_request", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "term") {
                    req._term = read_int<TermId>(r, t);
                    have_term = true;
                } else if (key == "candidate_id") {
                    req._candidate_id = read_id<NodeId>(r, t);
                    have_candidate = true;
                } else if (key == "last_log_index") {
                    req._last_log_index = read_int<LogIndex>(r, t);
                    have_lli = true;
                } else if (key == "last_log_term") {
                    req._last_log_term = read_int<TermId>(r, t);
                    have_llt = true;
                } else {
                    throw serialization_exception("Unexpected field in request_pre_vote_request: " +
                                                  key);
                }
            });
        });
        if (!have_term || !have_candidate || !have_lli || !have_llt) {
            throw serialization_exception("Missing required field in request_pre_vote_request");
        }
        return req;
    }

    // Deserialize RequestPreVote Response
    template<typename TermId = std::uint64_t>
    [[nodiscard]] auto deserialize_request_pre_vote_response(const Data& data) const
        -> request_pre_vote_response<TermId> {
        request_pre_vote_response<TermId> resp{};
        bool have_term = false, have_granted = false;
        decode(data, "request_pre_vote_response", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "term") {
                    resp._term = read_int<TermId>(r, t);
                    have_term = true;
                } else if (key == "vote_granted") {
                    resp._vote_granted = read_bool(r, t);
                    have_granted = true;
                } else {
                    throw serialization_exception(
                        "Unexpected field in request_pre_vote_response: " + key);
                }
            });
        });
        if (!have_term || !have_granted) {
            throw serialization_exception("Missing required field in request_pre_vote_response");
        }
        return resp;
    }

    // Deserialize AppendEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>>
    [[nodiscard]] auto deserialize_append_entries_request(const Data& data) const
        -> append_entries_request<NodeId, TermId, LogIndex, LogEntry> {
        append_entries_request<NodeId, TermId, LogIndex, LogEntry> req{};
        bool have_term = false, have_leader = false, have_pli = false, have_plt = false,
             have_commit = false, have_entries = false;
        decode(data, "append_entries_request", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "term") {
                    req._term = read_int<TermId>(r, t);
                    have_term = true;
                } else if (key == "leader_id") {
                    req._leader_id = read_id<NodeId>(r, t);
                    have_leader = true;
                } else if (key == "prev_log_index") {
                    req._prev_log_index = read_int<LogIndex>(r, t);
                    have_pli = true;
                } else if (key == "prev_log_term") {
                    req._prev_log_term = read_int<TermId>(r, t);
                    have_plt = true;
                } else if (key == "leader_commit") {
                    req._leader_commit = read_int<LogIndex>(r, t);
                    have_commit = true;
                } else if (key == "entries") {
                    req._entries = read_entries<TermId, LogIndex, LogEntry>(r, t);
                    have_entries = true;
                } else {
                    throw serialization_exception("Unexpected field in append_entries_request: " +
                                                  key);
                }
            });
        });
        if (!have_term || !have_leader || !have_pli || !have_plt || !have_commit || !have_entries) {
            throw serialization_exception("Missing required field in append_entries_request");
        }
        return req;
    }

    // Deserialize AppendEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
    [[nodiscard]] auto deserialize_append_entries_response(const Data& data) const
        -> append_entries_response<TermId, LogIndex> {
        append_entries_response<TermId, LogIndex> resp{};
        bool have_term = false, have_success = false;
        decode(data, "append_entries_response", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "term") {
                    resp._term = read_int<TermId>(r, t);
                    have_term = true;
                } else if (key == "success") {
                    resp._success = read_bool(r, t);
                    have_success = true;
                } else if (key == "conflict_index") {
                    resp._conflict_index = read_int<LogIndex>(r, t);
                } else if (key == "conflict_term") {
                    resp._conflict_term = read_int<TermId>(r, t);
                } else {
                    throw serialization_exception("Unexpected field in append_entries_response: " +
                                                  key);
                }
            });
        });
        if (!have_term || !have_success) {
            throw serialization_exception("Missing required field in append_entries_response");
        }
        return resp;
    }

    // Deserialize InstallSnapshot Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto deserialize_install_snapshot_request(const Data& data) const
        -> install_snapshot_request<NodeId, TermId, LogIndex> {
        install_snapshot_request<NodeId, TermId, LogIndex> req{};
        bool have_term = false, have_leader = false, have_lii = false, have_lit = false,
             have_offset = false, have_data = false, have_done = false;
        decode(data, "install_snapshot_request", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "term") {
                    req._term = read_int<TermId>(r, t);
                    have_term = true;
                } else if (key == "leader_id") {
                    req._leader_id = read_id<NodeId>(r, t);
                    have_leader = true;
                } else if (key == "last_included_index") {
                    req._last_included_index = read_int<LogIndex>(r, t);
                    have_lii = true;
                } else if (key == "last_included_term") {
                    req._last_included_term = read_int<TermId>(r, t);
                    have_lit = true;
                } else if (key == "offset") {
                    req._offset = read_int<std::size_t>(r, t);
                    have_offset = true;
                } else if (key == "data") {
                    req._data = read_blob(r, t);
                    have_data = true;
                } else if (key == "done") {
                    req._done = read_bool(r, t);
                    have_done = true;
                } else {
                    throw serialization_exception("Unexpected field in install_snapshot_request: " +
                                                  key);
                }
            });
        });
        if (!have_term || !have_leader || !have_lii || !have_lit || !have_offset || !have_data ||
            !have_done) {
            throw serialization_exception("Missing required field in install_snapshot_request");
        }
        return req;
    }

    // Deserialize InstallSnapshot Response
    template<typename TermId = std::uint64_t>
    [[nodiscard]] auto deserialize_install_snapshot_response(const Data& data) const
        -> install_snapshot_response<TermId> {
        install_snapshot_response<TermId> resp{};
        bool have_term = false;
        decode(data, "install_snapshot_response", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "term") {
                    resp._term = read_int<TermId>(r, t);
                    have_term = true;
                } else {
                    throw serialization_exception(
                        "Unexpected field in install_snapshot_response: " + key);
                }
            });
        });
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
        cluster_join_request<NodeId, Address> req{};
        bool have_node = false, have_address = false;
        decode(data, "cluster_join_request", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "node_id") {
                    req.node_id = read_id<NodeId>(r, t);
                    have_node = true;
                } else if (key == "contact_address") {
                    req.contact_address = read_string(r, t);
                    have_address = true;
                } else {
                    throw serialization_exception("Unexpected field in cluster_join_request: " +
                                                  key);
                }
            });
        });
        if (!have_node || !have_address) {
            throw serialization_exception("Missing required field in cluster_join_request");
        }
        return req;
    }

    // Deserialize ClusterJoin Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_join_response(const Data& data) const
        -> cluster_join_response<NodeId, Address> {
        cluster_join_response<NodeId, Address> resp{};
        bool have_accepted = false;
        decode(data, "cluster_join_response", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "accepted") {
                    resp.accepted = read_bool(r, t);
                    have_accepted = true;
                } else if (key == "redirect") {
                    resp.redirect = read_redirect<NodeId, Address>(r, t);
                } else {
                    throw serialization_exception("Unexpected field in cluster_join_response: " +
                                                  key);
                }
            });
        });
        if (!have_accepted) {
            throw serialization_exception("Missing required field in cluster_join_response");
        }
        return resp;
    }

    // Deserialize ClusterLeave Request
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_leave_request(const Data& data) const
        -> cluster_leave_request<NodeId, Address> {
        cluster_leave_request<NodeId, Address> req{};
        bool have_node = false;
        decode(data, "cluster_leave_request", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "node_id") {
                    req.node_id = read_id<NodeId>(r, t);
                    have_node = true;
                } else {
                    throw serialization_exception("Unexpected field in cluster_leave_request: " +
                                                  key);
                }
            });
        });
        if (!have_node) {
            throw serialization_exception("Missing required field in cluster_leave_request");
        }
        return req;
    }

    // Deserialize ClusterLeave Response
    template<typename NodeId = std::uint64_t, typename Address = std::string>
    [[nodiscard]] auto deserialize_cluster_leave_response(const Data& data) const
        -> cluster_leave_response<NodeId, Address> {
        cluster_leave_response<NodeId, Address> resp{};
        bool have_accepted = false;
        decode(data, "cluster_leave_response", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "accepted") {
                    resp.accepted = read_bool(r, t);
                    have_accepted = true;
                } else if (key == "redirect") {
                    resp.redirect = read_redirect<NodeId, Address>(r, t);
                } else {
                    throw serialization_exception("Unexpected field in cluster_leave_response: " +
                                                  key);
                }
            });
        });
        if (!have_accepted) {
            throw serialization_exception("Missing required field in cluster_leave_response");
        }
        return resp;
    }

    // Deserialize FetchLogEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
             typename LogIndex = std::uint64_t>
    [[nodiscard]] auto deserialize_fetch_log_entries_request(const Data& data) const
        -> fetch_log_entries_request<NodeId, TermId, LogIndex> {
        fetch_log_entries_request<NodeId, TermId, LogIndex> req{};
        bool have_requester = false, have_from = false, have_to = false;
        decode(data, "fetch_log_entries_request", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "requester_id") {
                    req._requester_id = read_id<NodeId>(r, t);
                    have_requester = true;
                } else if (key == "from_index") {
                    req._from_index = read_int<LogIndex>(r, t);
                    have_from = true;
                } else if (key == "to_index") {
                    req._to_index = read_int<LogIndex>(r, t);
                    have_to = true;
                } else {
                    throw serialization_exception(
                        "Unexpected field in fetch_log_entries_request: " + key);
                }
            });
        });
        if (!have_requester || !have_from || !have_to) {
            throw serialization_exception("Missing required field in fetch_log_entries_request");
        }
        return req;
    }

    // Deserialize FetchLogEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t,
             typename LogEntry = log_entry<TermId, LogIndex>>
    [[nodiscard]] auto deserialize_fetch_log_entries_response(const Data& data) const
        -> fetch_log_entries_response<TermId, LogIndex, LogEntry> {
        fetch_log_entries_response<TermId, LogIndex, LogEntry> resp{};
        bool have_responder = false, have_available = false, have_plt = false, have_entries = false;
        decode(data, "fetch_log_entries_response", [&](hREADER r) {
            for_each_field(r, [&](const std::string& key, ION_TYPE t) {
                if (key == "responder_id") {
                    resp._responder_id = read_int<std::uint64_t>(r, t);
                    have_responder = true;
                } else if (key == "available") {
                    resp._available = read_bool(r, t);
                    have_available = true;
                } else if (key == "prev_log_term") {
                    resp._prev_log_term = read_int<TermId>(r, t);
                    have_plt = true;
                } else if (key == "entries") {
                    resp._entries = read_entries<TermId, LogIndex, LogEntry>(r, t);
                    have_entries = true;
                } else {
                    throw serialization_exception(
                        "Unexpected field in fetch_log_entries_response: " + key);
                }
            });
        });
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

    /// @brief Content-format name: `"ion-binary"` or `"ion-text"`.
    ///
    /// Both contain the `"ion"` substring the CoAP/HTTP format detection keys off
    /// of (Requirement 1.4, 4.5), and the two are distinguishable per encoding.
    [[nodiscard]] auto name() const -> std::string {
        return _encoding == ion_encoding::binary ? "ion-binary" : "ion-text";
    }

private:
    /// Generous per-message baseline; the grow-retry loop in `encode` covers any
    /// shortfall, so this only trims the number of retries in the common case.
    static constexpr std::size_t base_estimate = 512;

    ion_encoding _encoding;

    // ── Internal signal for the writer grow-retry loop ──────────────────────
    // Thrown by check_write when ion-c reports the fixed output buffer is too
    // small; caught only by encode(), which doubles the buffer and retries. It
    // is deliberately not a serialization_exception, so it never escapes to a
    // caller as a genuine encode failure.
    struct buffer_too_small {};

    // ── ion-c error translation ─────────────────────────────────────────────

    /// Translate a non-`IERR_OK` `ion-c` status into `serialization_exception`
    /// carrying the failing operation's context plus `ion-c`'s own description
    /// (Requirement 5.5). Never propagates raw `iERR` codes to the caller.
    [[noreturn]] static auto translate_ion_error(iERR code, const char* context) -> void {
        throw serialization_exception(std::string("Ion error while ") + context + ": " +
                                      ion_error_to_str(code));
    }

    /// Check a writer-side call; a buffer-too-small status becomes the internal
    /// grow signal, every other non-OK status becomes `serialization_exception`.
    static auto check_write(iERR code, const char* context) -> void {
        if (code == IERR_OK) {
            return;
        }
        if (code == IERR_BUFFER_TOO_SMALL) {
            throw buffer_too_small{};
        }
        translate_ion_error(code, context);
    }

    /// Check a reader-side call; any non-OK status becomes `serialization_exception`.
    static auto check_read(iERR code, const char* context) -> void {
        if (code != IERR_OK) {
            translate_ion_error(code, context);
        }
    }

    // ── ION_STRING helpers ──────────────────────────────────────────────────

    /// Build an `ION_STRING` view over a C string for a write call. `ion-c`
    /// never mutates field-name/annotation/value strings on write, so the
    /// const_cast is sound.
    static auto ion_view(const char* s) -> ION_STRING {
        ION_STRING str;
        str.value = reinterpret_cast<BYTE*>(const_cast<char*>(s));
        str.length = static_cast<SIZE>(std::strlen(s));
        return str;
    }

    static auto ion_view(const std::string& s) -> ION_STRING {
        ION_STRING str;
        str.value = reinterpret_cast<BYTE*>(const_cast<char*>(s.data()));
        str.length = static_cast<SIZE>(s.size());
        return str;
    }

    static auto to_std_string(const ION_STRING& s) -> std::string {
        if (s.value == nullptr || s.length <= 0) {
            return {};
        }
        return std::string(reinterpret_cast<const char*>(s.value),
                           static_cast<std::size_t>(s.length));
    }

    static auto ion_string_equals(const ION_STRING& s, const char* c) -> bool {
        const auto n = std::strlen(c);
        if (s.value == nullptr) {
            return n == 0;
        }
        if (static_cast<std::size_t>(s.length) != n) {
            return false;
        }
        return std::memcmp(s.value, c, n) == 0;
    }

    // ── Write primitives ────────────────────────────────────────────────────

    static auto write_annotation(hWRITER w, const char* annotation) -> void {
        ION_STRING s = ion_view(annotation);
        check_write(ion_writer_add_annotation(w, &s), "adding annotation");
    }

    static auto start_struct(hWRITER w) -> void {
        check_write(ion_writer_start_container(w, tid_STRUCT), "starting Ion struct");
    }

    static auto finish_struct(hWRITER w) -> void {
        check_write(ion_writer_finish_container(w), "finishing Ion struct");
    }

    static auto write_field_name(hWRITER w, const char* name) -> void {
        ION_STRING s = ion_view(name);
        check_write(ion_writer_write_field_name(w, &s), "writing field name");
    }

    static auto write_field_int(hWRITER w, const char* name, std::int64_t value) -> void {
        write_field_name(w, name);
        check_write(ion_writer_write_int64(w, value), "writing int field");
    }

    static auto write_field_bool(hWRITER w, const char* name, bool value) -> void {
        write_field_name(w, name);
        check_write(ion_writer_write_bool(w, value ? TRUE : FALSE), "writing bool field");
    }

    static auto write_field_blob(hWRITER w, const char* name, const std::vector<std::byte>& value)
        -> void {
        write_field_name(w, name);
        BYTE dummy = 0;
        BYTE* ptr =
            value.empty() ? &dummy : reinterpret_cast<BYTE*>(const_cast<std::byte*>(value.data()));
        check_write(ion_writer_write_blob(w, ptr, static_cast<SIZE>(value.size())),
                    "writing blob field");
    }

    static auto write_field_string(hWRITER w, const char* name, const std::string& value) -> void {
        write_field_name(w, name);
        ION_STRING s = ion_view(value);
        check_write(ion_writer_write_string(w, &s), "writing string field");
    }

    /// Write a `NodeId` field: native Ion `int` when numeric, `string` when
    /// `std::string` (Requirement 2.7, 3.3).
    template<typename NodeId>
    static auto write_field_id(hWRITER w, const char* name, const NodeId& id) -> void {
        if constexpr (std::same_as<NodeId, std::string>) {
            write_field_string(w, name, id);
        } else {
            write_field_int(w, name, static_cast<std::int64_t>(id));
        }
    }

    /// Write an `entries()` container as a native Ion `list<struct>`; an empty
    /// container round-trips to an empty (not omitted or null) list (Requirement 2.4).
    template<typename Entries>
    static auto write_field_entries(hWRITER w, const char* name, const Entries& entries) -> void {
        write_field_name(w, name);
        check_write(ion_writer_start_container(w, tid_LIST), "starting entries list");
        for (const auto& entry : entries) {
            start_struct(w);
            write_field_int(w, "term", static_cast<std::int64_t>(entry.term()));
            write_field_int(w, "index", static_cast<std::int64_t>(entry.index()));
            write_field_blob(w, "command", entry.command());
            write_field_int(w, "entry_type",
                            static_cast<std::int64_t>(
                                static_cast<std::underlying_type_t<entry_type>>(entry.type())));
            finish_struct(w);
        }
        check_write(ion_writer_finish_container(w), "finishing entries list");
    }

    /// Write a redirect hint as a nested `{node_id, address}` struct (Requirement 2.6).
    template<typename PeerInfo>
    static auto write_field_redirect(hWRITER w, const PeerInfo& peer) -> void {
        write_field_name(w, "redirect");
        start_struct(w);
        write_field_id(w, "node_id", peer.node_id);
        write_field_string(w, "address", peer.address);
        finish_struct(w);
    }

    template<typename Entries> static auto entries_estimate(const Entries& entries) -> std::size_t {
        std::size_t total = 0;
        for (const auto& entry : entries) {
            total += 96 + entry.command().size() * 2;
        }
        return total;
    }

    // ── Read primitives ─────────────────────────────────────────────────────

    static auto read_int64(hREADER r, ION_TYPE t, const char* context) -> std::int64_t {
        if (t != tid_INT) {
            throw serialization_exception(std::string("expected Ion int for ") + context);
        }
        std::int64_t value = 0;
        check_read(ion_reader_read_int64(r, &value), context);
        return value;
    }

    /// Read an int field and narrow it into `Target`, rejecting values outside
    /// the target type's range rather than silently truncating (Requirement 3.3, 5.3).
    template<typename Target> static auto read_int(hREADER r, ION_TYPE t) -> Target {
        const std::int64_t value = read_int64(r, t, "reading int field");
        if constexpr (std::is_unsigned_v<Target>) {
            if (value < 0) {
                throw serialization_exception("negative Ion int for unsigned field");
            }
            const auto u = static_cast<std::uint64_t>(value);
            if (u > static_cast<std::uint64_t>(std::numeric_limits<Target>::max())) {
                throw serialization_exception("Ion int out of range for target type");
            }
            return static_cast<Target>(u);
        } else {
            if (value < static_cast<std::int64_t>(std::numeric_limits<Target>::min()) ||
                value > static_cast<std::int64_t>(std::numeric_limits<Target>::max())) {
                throw serialization_exception("Ion int out of range for target type");
            }
            return static_cast<Target>(value);
        }
    }

    static auto read_bool(hREADER r, ION_TYPE t) -> bool {
        if (t != tid_BOOL) {
            throw serialization_exception("expected Ion bool");
        }
        BOOL value = FALSE;
        check_read(ion_reader_read_bool(r, &value), "reading bool field");
        return value != FALSE;
    }

    static auto read_string(hREADER r, ION_TYPE t) -> std::string {
        if (t != tid_STRING && t != tid_SYMBOL) {
            throw serialization_exception("expected Ion string/symbol");
        }
        ION_STRING s;
        ION_STRING_INIT(&s);
        check_read(ion_reader_read_string(r, &s), "reading string field");
        return to_std_string(s);
    }

    static auto read_blob(hREADER r, ION_TYPE t) -> std::vector<std::byte> {
        if (t != tid_BLOB) {
            throw serialization_exception("expected Ion blob");
        }
        SIZE size = 0;
        check_read(ion_reader_get_lob_size(r, &size), "reading blob size");
        if (size < 0) {
            throw serialization_exception("negative Ion blob size");
        }
        std::vector<std::byte> out(static_cast<std::size_t>(size));
        if (size > 0) {
            SIZE read_len = 0;
            check_read(
                ion_reader_read_lob_bytes(r, reinterpret_cast<BYTE*>(out.data()), size, &read_len),
                "reading blob bytes");
            if (read_len != size) {
                throw serialization_exception("short Ion blob read");
            }
        }
        return out;
    }

    /// Read a `NodeId` field: `string`/`symbol` when `std::string`, `int` otherwise.
    template<typename NodeId> static auto read_id(hREADER r, ION_TYPE t) -> NodeId {
        if constexpr (std::same_as<NodeId, std::string>) {
            return read_string(r, t);
        } else {
            return read_int<NodeId>(r, t);
        }
    }

    template<typename TermId, typename LogIndex, typename LogEntry>
    static auto read_log_entry(hREADER r) -> LogEntry {
        LogEntry entry{};
        entry._type = entry_type::normal;
        bool have_term = false, have_index = false, have_command = false;
        check_read(ion_reader_step_in(r), "stepping into log entry");
        for_each_field(r, [&](const std::string& key, ION_TYPE t) {
            if (key == "term") {
                entry._term = read_int<TermId>(r, t);
                have_term = true;
            } else if (key == "index") {
                entry._index = read_int<LogIndex>(r, t);
                have_index = true;
            } else if (key == "command") {
                entry._command = read_blob(r, t);
                have_command = true;
            } else if (key == "entry_type") {
                entry._type =
                    static_cast<entry_type>(read_int<std::underlying_type_t<entry_type>>(r, t));
            } else {
                throw serialization_exception("Unexpected field in log entry: " + key);
            }
        });
        check_read(ion_reader_step_out(r), "stepping out of log entry");
        if (!have_term || !have_index || !have_command) {
            throw serialization_exception("Missing required field in log entry");
        }
        return entry;
    }

    template<typename TermId, typename LogIndex, typename LogEntry>
    static auto read_entries(hREADER r, ION_TYPE t) -> std::vector<LogEntry> {
        if (t != tid_LIST) {
            throw serialization_exception("expected Ion list for entries");
        }
        std::vector<LogEntry> entries;
        check_read(ion_reader_step_in(r), "stepping into entries list");
        for (;;) {
            ION_TYPE item = nullptr;
            check_read(ion_reader_next(r, &item), "advancing entries list");
            if (item == tid_EOF) {
                break;
            }
            if (item != tid_STRUCT) {
                throw serialization_exception("entry is not an Ion struct");
            }
            entries.push_back(read_log_entry<TermId, LogIndex, LogEntry>(r));
        }
        check_read(ion_reader_step_out(r), "stepping out of entries list");
        return entries;
    }

    template<typename NodeId, typename Address>
    static auto read_redirect(hREADER r, ION_TYPE t) -> std::optional<peer_info<NodeId, Address>> {
        if (t != tid_STRUCT) {
            throw serialization_exception("expected Ion struct for redirect");
        }
        peer_info<NodeId, Address> peer{};
        bool have_node = false, have_address = false;
        check_read(ion_reader_step_in(r), "stepping into redirect struct");
        for_each_field(r, [&](const std::string& key, ION_TYPE ft) {
            if (key == "node_id") {
                peer.node_id = read_id<NodeId>(r, ft);
                have_node = true;
            } else if (key == "address") {
                peer.address = read_string(r, ft);
                have_address = true;
            } else {
                throw serialization_exception("Unexpected field in redirect: " + key);
            }
        });
        check_read(ion_reader_step_out(r), "stepping out of redirect struct");
        if (!have_node || !have_address) {
            throw serialization_exception("Incomplete redirect struct");
        }
        return peer;
    }

    /// Validate that the value the reader is positioned on carries exactly the
    /// one expected annotation (Requirement 5.2).
    static auto expect_annotation(hREADER r, const char* expected) -> void {
        SIZE count = 0;
        check_read(ion_reader_get_annotation_count(r, &count), "reading annotation count");
        if (count != 1) {
            throw serialization_exception(std::string("expected exactly one annotation (") +
                                          expected + ")");
        }
        ION_STRING annotation;
        ION_STRING_INIT(&annotation);
        check_read(ion_reader_get_an_annotation(r, 0, &annotation), "reading annotation");
        if (!ion_string_equals(annotation, expected)) {
            throw serialization_exception(std::string("Invalid message type, expected ") +
                                          expected);
        }
    }

    /// Iterate the fields of the struct the reader has stepped into, invoking
    /// `fn(field_name, value_type)` for each. `fn` reads the value.
    template<typename FieldFn> static auto for_each_field(hREADER r, FieldFn&& fn) -> void {
        for (;;) {
            ION_TYPE t = nullptr;
            check_read(ion_reader_next(r, &t), "advancing to next field");
            if (t == tid_EOF) {
                break;
            }
            ION_STRING field_name;
            ION_STRING_INIT(&field_name);
            check_read(ion_reader_get_field_name(r, &field_name), "reading field name");
            fn(to_std_string(field_name), t);
        }
    }

    // ── Buffer conversion ───────────────────────────────────────────────────

    static auto to_data(const std::vector<BYTE>& bytes, std::size_t length) -> Data {
        Data result;
        if constexpr (requires { result.resize(std::size_t{}); }) {
            result.resize(length);
            std::transform(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length),
                           result.begin(), [](BYTE b) { return static_cast<std::byte>(b); });
        }
        return result;
    }

    static auto to_buffer(const Data& data) -> std::vector<BYTE> {
        std::vector<BYTE> buffer;
        buffer.reserve(std::ranges::size(data));
        for (auto b : data) {
            buffer.push_back(static_cast<BYTE>(b));
        }
        return buffer;
    }

    // ── Encode / decode drivers ─────────────────────────────────────────────

    /// Open an `ion-c` writer over a growable fixed buffer, run `body`, flush,
    /// and copy the bytes into `Data`. `body` writes exactly one top-level
    /// annotated struct. When the buffer proves too small, it is doubled and the
    /// whole body re-run — `ion-c`'s buffer writer surfaces overflow at write or
    /// flush time, so the grow-retry is the correct way to size the output
    /// without pre-computing an exact upper bound.
    template<typename Body> auto encode(std::size_t hint, Body&& body) const -> Data {
        std::size_t capacity = std::max<std::size_t>(hint, base_estimate);
        for (;;) {
            std::vector<BYTE> buffer(capacity);
            ION_WRITER_OPTIONS options;
            std::memset(&options, 0, sizeof(options));
            options.output_as_binary = (_encoding == ion_encoding::binary) ? TRUE : FALSE;

            hWRITER writer = nullptr;
            const iERR open_err = ion_writer_open_buffer(
                &writer, buffer.data(), static_cast<SIZE>(buffer.size()), &options);
            if (open_err != IERR_OK) {
                translate_ion_error(open_err, "opening Ion writer");
            }
            detail::ion_writer_handle guard{writer};

            SIZE flushed = 0;
            try {
                body(writer);
                check_write(ion_writer_flush(writer, &flushed), "flushing Ion writer");
            } catch (const buffer_too_small&) {
                capacity *= 2;
                continue;  // guard closes this writer before the next attempt
            }
            return to_data(buffer, static_cast<std::size_t>(flushed));
        }
    }

    /// Open an `ion-c` reader over `data`, require a single top-level struct
    /// carrying `expected_annotation`, step into it, run `body`, and step back
    /// out. The reader handle is released by RAII even on the exception paths
    /// (Requirement 5.6).
    template<typename Body>
    auto decode(const Data& data, const char* expected_annotation, Body&& body) const -> void {
        std::vector<BYTE> buffer = to_buffer(data);
        BYTE dummy = 0;
        BYTE* ptr = buffer.empty() ? &dummy : buffer.data();

        ION_READER_OPTIONS options;
        std::memset(&options, 0, sizeof(options));

        hREADER reader = nullptr;
        const iERR open_err =
            ion_reader_open_buffer(&reader, ptr, static_cast<SIZE>(buffer.size()), &options);
        if (open_err != IERR_OK) {
            translate_ion_error(open_err, "opening Ion reader");
        }
        detail::ion_reader_handle guard{reader};

        ION_TYPE t = nullptr;
        check_read(ion_reader_next(reader, &t), "reading top-level value");
        if (t != tid_STRUCT) {
            throw serialization_exception("Ion top-level value is not a struct");
        }
        expect_annotation(reader, expected_annotation);
        check_read(ion_reader_step_in(reader), "stepping into message struct");
        body(reader);
        check_read(ion_reader_step_out(reader), "stepping out of message struct");
    }
};

// Verify that ion_rpc_serializer satisfies the rpc_serializer concept
static_assert(rpc_serializer<ion_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>,
              "ion_rpc_serializer must satisfy the rpc_serializer concept");

// Convenience type alias for the default Ion serializer
using ion_serializer = ion_rpc_serializer<std::vector<std::byte>>;

}  // namespace kythira
