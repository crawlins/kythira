// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

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
/// The four cells involving Proxygen need `KYTHIRA_BUILD_PROXYGEN_TRANSPORT`,
/// which is an optional dependency, so they live in
/// `proxygen_implementation_interop_test.cpp` and run the *same* rig from
/// `http_interop_rpc_rig.hpp` — a cell's result only means something next to
/// the other cells' results, so the two files must not drift.
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
#include "http_interop_rpc_rig.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

namespace {

using kythira::testing::exercise_all_three;
using kythira::testing::interop_bind_address;
using kythira::testing::interop_node_map_for;
using kythira::testing::register_interop_handlers;

// Fresh ports: the equivalence suites hold 18220-18223 and may run concurrently
// under `ctest -j`.
constexpr std::uint16_t beast_server_port = 18290;
constexpr std::uint16_t httplib_server_port = 18291;

using httplib_transport_types =
    kythira::http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                  kythira::executor_default>;
using beast_transport_types =
    kythira::future_default_http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                                 kythira::executor_default>;

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
        ioc, interop_bind_address, beast_server_port, {}, kythira::noop_metrics{});
    register_interop_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    auto node_map = interop_node_map_for(beast_server_port);
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
        interop_bind_address, httplib_server_port, {}, kythira::noop_metrics{});
    register_interop_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    auto node_map = interop_node_map_for(httplib_server_port);
    kythira::boost_beast_client<beast_transport_types> client(ioc, node_map, {},
                                                              kythira::noop_metrics{});

    exercise_all_three(client);

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
