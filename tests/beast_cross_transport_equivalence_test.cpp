// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE beast_cross_transport_equivalence_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/executor_default.hpp>
#include <raft/http_exceptions.hpp>

#include "beast_test_thread_pool.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

// Task 15 / Property 9 (design.md): cpp_httplib_client/server and
// boost_beast_client/server, run against the *same* RPC sequence, must
// produce equivalent externally-observable results even though their
// internal I/O mechanics (blocking-on-executor vs. io_context-driven async)
// differ completely. Requirement 19.4 is deliberate here: the two transports
// are exercised through *different* Types bundles (http_transport_types for
// cpp-httplib, future_default_http_transport_types for Beast) rather than
// one shared bundle, because each transport's own `requires` clause pins a
// different future_template -- what stays identical across both is
// serializer_type (kythira::json_serializer for both), so the wire format
// itself is the same, only the transport carrying it differs.
namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint16_t httplib_port = 18220;
constexpr std::uint16_t beast_port = 18221;
constexpr std::uint16_t httplib_error_port = 18222;
constexpr std::uint16_t beast_error_port = 18223;
constexpr std::uint64_t test_node_id = 1;

using httplib_transport_types =
    kythira::http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                  kythira::executor_default>;
using beast_transport_types =
    kythira::future_default_http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                                 kythira::executor_default>;

// Deliberately not the trivial identity/echo the other beast_transport_test.cpp
// cases use -- a handler whose output depends on more than one input field
// makes it possible to tell "both transports ran the same handler logic
// against the same deserialized request" apart from "both transports just
// happen to return a fixed constant."
auto compute_request_vote_response(const kythira::request_vote_request<>& req)
    -> kythira::request_vote_response<> {
    kythira::request_vote_response<> resp{};
    resp._term = req.term() + 1;
    resp._vote_granted = (req.candidate_id() % 2 == 0);
    return resp;
}

auto compute_append_entries_response(const kythira::append_entries_request<>& req)
    -> kythira::append_entries_response<> {
    kythira::append_entries_response<> resp{};
    resp._term = req.term();
    resp._success = req.leader_commit() <= req.prev_log_index();
    return resp;
}

auto compute_install_snapshot_response(const kythira::install_snapshot_request<>& req)
    -> kythira::install_snapshot_response<> {
    kythira::install_snapshot_response<> resp{};
    resp._term = req.term() + static_cast<std::uint64_t>(req.offset());
    return resp;
}

template<typename Server> auto register_equivalence_handlers(Server& server) -> void {
    server.register_request_vote_handler(compute_request_vote_response);
    server.register_append_entries_handler(compute_append_entries_response);
    server.register_install_snapshot_handler(compute_install_snapshot_response);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(beast_cross_transport_equivalence_tests)

// **Feature: boost-beast-http-transport, Property 9: Cross-Transport Equivalence**
// **Validates: Requirement 16.4**
BOOST_AUTO_TEST_CASE(identical_rpc_sequence_produces_equivalent_responses) {
    // --- cpp-httplib side: self-managed server thread, no io_context. ---
    kythira::cpp_httplib_server<httplib_transport_types> httplib_server(
        test_bind_address, httplib_port, {}, kythira::noop_metrics{});
    register_equivalence_handlers(httplib_server);
    httplib_server.start();
    BOOST_TEST(httplib_server.is_running());

    std::unordered_map<std::uint64_t, std::string> httplib_node_map{
        {test_node_id,
         std::string("http://") + test_bind_address + ":" + std::to_string(httplib_port)}};
    kythira::cpp_httplib_client<httplib_transport_types> httplib_client(httplib_node_map, {},
                                                                        kythira::noop_metrics{});

    // --- Beast side: caller-owned io_context (Requirement 8). ---
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    kythira::boost_beast_server<beast_transport_types> beast_server(
        ioc, test_bind_address, beast_port, {}, kythira::noop_metrics{});
    register_equivalence_handlers(beast_server);
    beast_server.start();
    BOOST_TEST(beast_server.is_running());

    std::unordered_map<std::uint64_t, std::string> beast_node_map{
        {test_node_id,
         std::string("http://") + test_bind_address + ":" + std::to_string(beast_port)}};
    kythira::boost_beast_client<beast_transport_types> beast_client(ioc, beast_node_map, {},
                                                                    kythira::noop_metrics{});

    // RequestVote: run the identical request through both transports.
    {
        kythira::request_vote_request<> req{};
        req._term = 12;
        req._candidate_id = 4;
        req._last_log_index = 3;
        req._last_log_term = 11;

        auto httplib_resp = std::move(httplib_client.send_request_vote(
                                          test_node_id, req, std::chrono::milliseconds(30000)))
                                .get();
        auto beast_resp = std::move(beast_client.send_request_vote(
                                        test_node_id, req, std::chrono::milliseconds(30000)))
                              .get();

        BOOST_TEST(httplib_resp.term() == beast_resp.term());
        BOOST_TEST(httplib_resp.vote_granted() == beast_resp.vote_granted());
        // Pin the actual expected value too, not just cross-transport
        // agreement -- two transports that both computed the wrong thing
        // identically would otherwise still pass.
        BOOST_TEST(httplib_resp.term() == 13);
        BOOST_TEST(httplib_resp.vote_granted() == true);
    }

    // AppendEntries.
    {
        kythira::append_entries_request<> req{};
        req._term = 9;
        req._leader_id = 4;
        req._prev_log_index = 20;
        req._prev_log_term = 8;
        req._leader_commit = 15;

        auto httplib_resp = std::move(httplib_client.send_append_entries(
                                          test_node_id, req, std::chrono::milliseconds(30000)))
                                .get();
        auto beast_resp = std::move(beast_client.send_append_entries(
                                        test_node_id, req, std::chrono::milliseconds(30000)))
                              .get();

        BOOST_TEST(httplib_resp.term() == beast_resp.term());
        BOOST_TEST(httplib_resp.success() == beast_resp.success());
        BOOST_TEST(httplib_resp.success() == true);
    }

    // InstallSnapshot.
    {
        kythira::install_snapshot_request<> req{};
        req._term = 5;
        req._leader_id = 4;
        req._last_included_index = 100;
        req._last_included_term = 4;
        req._offset = 7;
        req._data = {std::byte{'e'}, std::byte{'q'}, std::byte{'u'}, std::byte{'i'},
                     std::byte{'v'}};
        req._done = true;

        auto httplib_resp = std::move(httplib_client.send_install_snapshot(
                                          test_node_id, req, std::chrono::milliseconds(30000)))
                                .get();
        auto beast_resp = std::move(beast_client.send_install_snapshot(
                                        test_node_id, req, std::chrono::milliseconds(30000)))
                              .get();

        BOOST_TEST(httplib_resp.term() == beast_resp.term());
        BOOST_TEST(httplib_resp.term() == 12);
    }

    beast_server.stop();
    httplib_server.stop();
}

// **Feature: boost-beast-http-transport, Property 9: Cross-Transport Equivalence**
// **Validates: Requirement 16.4**
// Property 9's "error status-code category" half: a registered handler that
// throws must surface as a failed call on both transports rather than a
// successful 200 response -- an unregistered-path 404 isn't reachable
// through the public network_client API on either transport (both only ever
// POST to their three fixed RPC endpoints), so a handler exception is the
// one error path both a real cpp-httplib caller and a real Beast caller can
// actually trigger through send_request_vote/send_append_entries/
// send_install_snapshot. The precise exception type/status_code() is only
// asserted on the Beast side, not compared across both -- see the inline
// comment below for why cpp_httplib_client's own async error path can't
// reliably expose it.
BOOST_AUTO_TEST_CASE(handler_exception_maps_to_equivalent_5xx_category_on_both_transports) {
    kythira::cpp_httplib_server<httplib_transport_types> httplib_server(
        test_bind_address, httplib_error_port, {}, kythira::noop_metrics{});
    httplib_server.register_request_vote_handler(
        [](const kythira::request_vote_request<>&) -> kythira::request_vote_response<> {
            throw std::runtime_error("handler failure");
        });
    httplib_server.start();

    std::unordered_map<std::uint64_t, std::string> httplib_node_map{
        {test_node_id,
         std::string("http://") + test_bind_address + ":" + std::to_string(httplib_error_port)}};
    kythira::cpp_httplib_client<httplib_transport_types> httplib_client(httplib_node_map, {},
                                                                        kythira::noop_metrics{});

    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    kythira::boost_beast_server<beast_transport_types> beast_server(
        ioc, test_bind_address, beast_error_port, {}, kythira::noop_metrics{});
    beast_server.register_request_vote_handler(
        [](const kythira::request_vote_request<>&) -> kythira::request_vote_response<> {
            throw std::runtime_error("handler failure");
        });
    beast_server.start();

    std::unordered_map<std::uint64_t, std::string> beast_node_map{
        {test_node_id,
         std::string("http://") + test_bind_address + ":" + std::to_string(beast_error_port)}};
    kythira::boost_beast_client<beast_transport_types> beast_client(ioc, beast_node_map, {},
                                                                    kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 1;

    // cpp_httplib_client's async error path routes through
    // http_transport_impl.hpp's make_future_with_exception, which used to take
    // `const std::exception&` and so sliced any http_client_error/
    // http_server_error down to plain std::exception before it reached this
    // catch -- template argument deduction for make_exception_ptr(E) picked up
    // e's *static* type at that call site, not its dynamic one, losing the
    // derived type and message along with status_code(). This case could then
    // only check that *some* exception was thrown.
    //
    // That helper is now templated on the concrete exception type, so the
    // derived type survives on this side too and the two transports can be
    // compared on the assertion that actually matters: identical status codes
    // recovered from identically-typed exceptions. See Task 15 in
    // .kiro/specs/boost-beast-http-transport/tasks.md.
    int httplib_status = 0;
    try {
        std::move(
            httplib_client.send_request_vote(test_node_id, req, std::chrono::milliseconds(30000)))
            .get();
        BOOST_FAIL("expected cpp_httplib_client to surface the handler's exception as an error");
    } catch (const kythira::http_server_error& e) {
        httplib_status = e.status_code();
    }

    BOOST_TEST(httplib_status == 500);

    // boost_beast_client's send_rpc throws kythira::http_server_error directly
    // inside a thenValue continuation; future_transformable's thenValue/
    // thenError composition (every backend kythira::future_default can be)
    // captures a thrown exception via std::current_exception(), which does
    // not slice -- so the exact type and status_code() survive to here.
    int beast_status = 0;
    try {
        std::move(
            beast_client.send_request_vote(test_node_id, req, std::chrono::milliseconds(30000)))
            .get();
        BOOST_FAIL("expected boost_beast_client to surface the handler's exception as an error");
    } catch (const kythira::http_server_error& e) {
        beast_status = e.status_code();
    }

    BOOST_TEST(beast_status == 500);

    // The equivalence this case exists to assert, now checkable on both sides
    // rather than only on Beast's.
    BOOST_TEST(httplib_status == beast_status);

    beast_server.stop();
    httplib_server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
