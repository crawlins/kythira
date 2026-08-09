#pragma once

/// @file http_interop_rpc_rig.hpp
/// @brief The shared RPC rig for the client-implementation ×
///        server-implementation interop grid (`doc/TODO.md`, "client-implementation
///        × server-implementation interop grid").
///
/// The grid has nine cells — three HTTP implementations' clients against three
/// implementations' servers — and they cannot all live in one translation unit:
/// the Proxygen cells need `KYTHIRA_BUILD_PROXYGEN_TRANSPORT`, which is an
/// optional dependency, while the httplib ↔ Beast cells build on every default
/// CI leg. The split is a build-gating accident, so everything *except* which
/// client meets which server belongs here rather than being written twice.
///
/// That matters beyond saving lines. A cell's result is only meaningful next to
/// the other cells' results, and two hand-copied rigs that drift — a different
/// timeout, a different sample request, a response mapping one of them lets
/// through — turn "Beast → Proxygen passes and Proxygen → Beast fails" from a
/// finding about the transports into a finding about the test files.
///
/// Every response here is computed from *more than one* request field, so "the
/// server decoded the request this client encoded" can be told apart from "the
/// server returned a constant". A cross-implementation test is precisely where
/// a half-decoded request would otherwise slip through.

#include <raft/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace kythira::testing {

inline constexpr const char* interop_bind_address = "127.0.0.1";
inline constexpr std::uint64_t interop_node_id = 1;
inline constexpr std::chrono::milliseconds interop_rpc_timeout{4000};

inline auto compute_request_vote_response(const kythira::request_vote_request<>& req)
    -> kythira::request_vote_response<> {
    kythira::request_vote_response<> resp{};
    resp._term = req.term() + req.last_log_term();
    resp._vote_granted = (req.candidate_id() % 2 == 0);
    return resp;
}

inline auto compute_append_entries_response(const kythira::append_entries_request<>& req)
    -> kythira::append_entries_response<> {
    kythira::append_entries_response<> resp{};
    resp._term = req.term() + req.prev_log_term();
    resp._success = req.leader_commit() <= req.prev_log_index();
    return resp;
}

inline auto compute_install_snapshot_response(const kythira::install_snapshot_request<>& req)
    -> kythira::install_snapshot_response<> {
    kythira::install_snapshot_response<> resp{};
    resp._term = req.term() + static_cast<std::uint64_t>(req.offset()) + req.data().size();
    return resp;
}

template<typename Server> auto register_interop_handlers(Server& server) -> void {
    server.register_request_vote_handler(compute_request_vote_response);
    server.register_append_entries_handler(compute_append_entries_response);
    server.register_install_snapshot_handler(compute_install_snapshot_response);
}

inline auto sample_request_vote() -> kythira::request_vote_request<> {
    kythira::request_vote_request<> req{};
    req._term = 12;
    req._candidate_id = 4;
    req._last_log_index = 9;
    req._last_log_term = 3;
    return req;
}

inline auto sample_append_entries() -> kythira::append_entries_request<> {
    kythira::append_entries_request<> req{};
    req._term = 21;
    req._leader_id = 2;
    req._prev_log_index = 14;
    req._prev_log_term = 6;
    req._leader_commit = 11;
    return req;
}

inline auto sample_install_snapshot() -> kythira::install_snapshot_request<> {
    kythira::install_snapshot_request<> req{};
    req._term = 30;
    req._leader_id = 3;
    req._last_included_index = 40;
    req._last_included_term = 8;
    req._offset = 5;
    req._data = {std::byte{'x'}, std::byte{'y'}, std::byte{'z'}};
    req._done = true;
    return req;
}

inline auto interop_node_map_for(std::uint16_t port)
    -> std::unordered_map<std::uint64_t, std::string> {
    return {{interop_node_id,
             std::string("http://") + interop_bind_address + ":" + std::to_string(port)}};
}

/// Drives all three RPCs through @p client and checks each answer against the
/// handler `register_interop_handlers` gave the server.
///
/// All three rather than one because each RPC is a separate endpoint with its
/// own encode/decode pair on both sides; a mismatch confined to one of them —
/// the one carrying a byte vector, say — would be invisible to a single-RPC
/// check.
///
/// `BOOST_TEST` rather than a returned status: this is called directly from a
/// test case, so a failure is attributed to the cell that called it.
template<typename Client> auto exercise_all_three(Client& client) -> void {
    {
        const auto req = sample_request_vote();
        const auto expected = compute_request_vote_response(req);
        auto resp = client.send_request_vote(interop_node_id, req, interop_rpc_timeout).get();
        BOOST_TEST(resp.term() == expected.term());
        BOOST_TEST(resp.vote_granted() == expected.vote_granted());
    }
    {
        const auto req = sample_append_entries();
        const auto expected = compute_append_entries_response(req);
        auto resp = client.send_append_entries(interop_node_id, req, interop_rpc_timeout).get();
        BOOST_TEST(resp.term() == expected.term());
        BOOST_TEST(resp.success() == expected.success());
    }
    {
        const auto req = sample_install_snapshot();
        const auto expected = compute_install_snapshot_response(req);
        auto resp = client.send_install_snapshot(interop_node_id, req, interop_rpc_timeout).get();
        BOOST_TEST(resp.term() == expected.term());
    }
}

}  // namespace kythira::testing
