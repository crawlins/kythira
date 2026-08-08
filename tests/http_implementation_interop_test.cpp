/// @file http_implementation_interop_test.cpp
/// @brief One HTTP implementation's client against a *different*
///        implementation's server (`doc/TODO.md`, "client-implementation ×
///        server-implementation interop grid").
///
/// **Equivalence is not interoperability, and until now only equivalence was
/// tested.** `beast_cross_transport_equivalence_test` runs httplib client →
/// httplib server and Beast client → Beast server and compares the *results*;
/// `three_way_http_transport_equivalence_test` says so in its own header —
/// "against each transport's **own** client/server pair". Both instantiate all
/// the clients and all the servers, which makes them look like interop tests
/// while every pairing stays on the diagonal.
///
/// The distinction is not pedantic. Each implementation's client is only ever
/// exercised against a server that shares its assumptions, so two transports can
/// satisfy every equivalence assertion and still fail to talk to each other: a
/// header one side always sends and the other never reads, a status the sender
/// never emits, a framing choice both ends happen to agree on. Nothing in an
/// equivalence test can see any of that.
///
/// This file covers the two off-diagonal cells that build on every default CI
/// leg:
///
///   * `cpp_httplib_client` → `boost_beast_server`
///   * `boost_beast_client` → `cpp_httplib_server`
///
/// The three cells involving Proxygen need `KYTHIRA_BUILD_PROXYGEN_TRANSPORT`
/// and are not covered here; the httplib → Proxygen cell is already covered
/// incidentally by `proxygen_negotiation_integration_test`, though with a *raw*
/// `httplib::Client` rather than `cpp_httplib_client`.
///
/// The two transports deliberately use different `Types` bundles, exactly as the
/// equivalence tests do: each transport's own `requires` clause pins a different
/// `future_template`. What is held constant is `json_serializer` on both sides,
/// so the wire format is identical and only the implementation carrying it
/// differs — which is the whole point of the axis being tested.

#define BOOST_TEST_MODULE http_implementation_interop_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/executor_default.hpp>
#include <raft/http_exceptions.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>

#include "beast_test_thread_pool.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* test_bind_address = "127.0.0.1";
// Fresh ports: the equivalence suites hold 18220-18223 and may run concurrently
// under `ctest -j`.
constexpr std::uint16_t beast_server_port = 18290;
constexpr std::uint16_t httplib_server_port = 18291;
constexpr std::uint64_t test_node_id = 1;
constexpr std::chrono::milliseconds rpc_timeout{4000};

using httplib_transport_types =
    kythira::http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                  kythira::executor_default>;
using beast_transport_types =
    kythira::future_default_http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                                 kythira::executor_default>;

// Responses depend on more than one request field, so "the server decoded the
// request this client encoded" can be told apart from "the server returned a
// constant". A cross-implementation test is precisely where a half-decoded
// request would otherwise slip through.
auto compute_request_vote_response(const kythira::request_vote_request<>& req)
    -> kythira::request_vote_response<> {
    kythira::request_vote_response<> resp{};
    resp._term = req.term() + req.last_log_term();
    resp._vote_granted = (req.candidate_id() % 2 == 0);
    return resp;
}

auto compute_append_entries_response(const kythira::append_entries_request<>& req)
    -> kythira::append_entries_response<> {
    kythira::append_entries_response<> resp{};
    resp._term = req.term() + req.prev_log_term();
    resp._success = req.leader_commit() <= req.prev_log_index();
    return resp;
}

auto compute_install_snapshot_response(const kythira::install_snapshot_request<>& req)
    -> kythira::install_snapshot_response<> {
    kythira::install_snapshot_response<> resp{};
    resp._term = req.term() + static_cast<std::uint64_t>(req.offset()) + req.data().size();
    return resp;
}

template<typename Server> auto register_handlers(Server& server) -> void {
    server.register_request_vote_handler(compute_request_vote_response);
    server.register_append_entries_handler(compute_append_entries_response);
    server.register_install_snapshot_handler(compute_install_snapshot_response);
}

auto sample_request_vote() -> kythira::request_vote_request<> {
    kythira::request_vote_request<> req{};
    req._term = 12;
    req._candidate_id = 4;
    req._last_log_index = 9;
    req._last_log_term = 3;
    return req;
}

auto sample_append_entries() -> kythira::append_entries_request<> {
    kythira::append_entries_request<> req{};
    req._term = 21;
    req._leader_id = 2;
    req._prev_log_index = 14;
    req._prev_log_term = 6;
    req._leader_commit = 11;
    return req;
}

auto sample_install_snapshot() -> kythira::install_snapshot_request<> {
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

/// Drives all three RPCs through @p client and checks each answer against the
/// handler the server was given.
///
/// All three rather than one because each RPC is a separate endpoint with its
/// own encode/decode pair on both sides; a mismatch confined to one of them —
/// the one carrying a byte vector, say — would be invisible to a single-RPC
/// check.
template<typename Client> auto exercise_all_three(Client& client) -> void {
    {
        const auto req = sample_request_vote();
        const auto expected = compute_request_vote_response(req);
        auto resp = client.send_request_vote(test_node_id, req, rpc_timeout).get();
        BOOST_TEST(resp.term() == expected.term());
        BOOST_TEST(resp.vote_granted() == expected.vote_granted());
    }
    {
        const auto req = sample_append_entries();
        const auto expected = compute_append_entries_response(req);
        auto resp = client.send_append_entries(test_node_id, req, rpc_timeout).get();
        BOOST_TEST(resp.term() == expected.term());
        BOOST_TEST(resp.success() == expected.success());
    }
    {
        const auto req = sample_install_snapshot();
        const auto expected = compute_install_snapshot_response(req);
        auto resp = client.send_install_snapshot(test_node_id, req, rpc_timeout).get();
        BOOST_TEST(resp.term() == expected.term());
    }
}

auto node_map_for(std::uint16_t port) -> std::unordered_map<std::uint64_t, std::string> {
    return {
        {test_node_id, std::string("http://") + test_bind_address + ":" + std::to_string(port)}};
}

}  // namespace

BOOST_AUTO_TEST_SUITE(http_implementation_interop_tests)

/// @brief Cell: `cpp_httplib_client` → `boost_beast_server`.
///
/// The client is the blocking-on-executor implementation; the server is the
/// io_context-driven async one. Neither has ever seen the other's traffic
/// before this test.
BOOST_AUTO_TEST_CASE(httplib_client_talks_to_a_beast_server) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    kythira::boost_beast_server<beast_transport_types> server(
        ioc, test_bind_address, beast_server_port, {}, kythira::noop_metrics{});
    register_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    auto node_map = node_map_for(beast_server_port);
    kythira::cpp_httplib_client<httplib_transport_types> client(node_map, {},
                                                                kythira::noop_metrics{});

    exercise_all_three(client);

    server.stop();
}

/// @brief Cell: `boost_beast_client` → `cpp_httplib_server`.
///
/// The mirror image, and not redundant with it: the two implementations differ
/// on both the sending and the receiving side, so a disagreement can live in
/// either direction independently. One cell passing says nothing about the
/// other.
BOOST_AUTO_TEST_CASE(beast_client_talks_to_an_httplib_server) {
    kythira::cpp_httplib_server<httplib_transport_types> server(
        test_bind_address, httplib_server_port, {}, kythira::noop_metrics{});
    register_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    auto node_map = node_map_for(httplib_server_port);
    kythira::boost_beast_client<beast_transport_types> client(ioc, node_map, {},
                                                              kythira::noop_metrics{});

    exercise_all_three(client);

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
