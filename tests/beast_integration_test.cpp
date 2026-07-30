#define BOOST_TEST_MODULE beast_integration_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/executor_default.hpp>

#include "beast_test_thread_pool.hpp"

#include <chrono>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

// Full client+server round-trip slice of the one-file-per-concern split of
// what used to be tests/beast_transport_test.cpp -- one-to-one with
// tests/http_integration_test.cpp: real RPCs against a real
// boost_beast_server, through a real boost_beast_client, both driven by the
// same caller-owned io_context (Requirement 8). Each test picks its own
// port from a disjoint range (this file is its own ctest binary now, and
// may run concurrently with the other beast_* test binaries under
// `ctest -j`).
namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint16_t test_bind_port = 18210;
constexpr std::uint16_t test_multi_bind_port_base = 18211;
constexpr const char* test_server_url = "http://127.0.0.1:18210";
constexpr std::uint64_t test_node_id = 1;

using test_transport_types =
    kythira::future_default_http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                                 kythira::executor_default>;

auto register_echo_handlers(kythira::boost_beast_server<test_transport_types>& server) -> void {
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            kythira::request_vote_response<> resp{};
            resp._term = req.term();
            resp._vote_granted = true;
            return resp;
        });
    server.register_append_entries_handler(
        [](const kythira::append_entries_request<>& req) -> kythira::append_entries_response<> {
            kythira::append_entries_response<> resp{};
            resp._term = req.term();
            resp._success = true;
            return resp;
        });
    server.register_install_snapshot_handler(
        [](const kythira::install_snapshot_request<>& req) -> kythira::install_snapshot_response<> {
            kythira::install_snapshot_response<> resp{};
            resp._term = req.term();
            return resp;
        });
}

}  // namespace

BOOST_AUTO_TEST_SUITE(beast_integration_tests)

// **Feature: boost-beast-http-transport, Property 5: Connection Reuse Correctness**
// **Validates: Requirements 9.1, 9.3**
BOOST_AUTO_TEST_CASE(request_vote_round_trip_and_connection_reuse) {
    boost::asio::io_context ioc;
    // io_context::run() returns as soon as there's no outstanding work, and
    // there is none yet at this point (the server/client below haven't been
    // constructed). Without a work guard keeping it artificially "busy",
    // these threads would return (and exit) immediately, and every
    // subsequent async_accept/async_connect this test posts would sit
    // forever with nothing driving it -- Requirement 8.2's "no progress
    // without a caller-run io_context::run() thread" applies to the whole
    // lifetime a caller wants the pool available, not just to moments when
    // work already happens to be queued.
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    kythira::boost_beast_server<test_transport_types> server(ioc, test_bind_address, test_bind_port,
                                                             {}, kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());

    std::unordered_map<std::uint64_t, std::string> node_map{{test_node_id, test_server_url}};
    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, {},
                                                             kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 42;
    auto resp =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(30000)))
            .get();
    BOOST_TEST(resp.term() == 42);
    BOOST_TEST(resp.vote_granted());

    // Second RPC on the same connection -- exercises pooling/reuse.
    auto resp2 =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(30000)))
            .get();
    BOOST_TEST(resp2.vote_granted());

    server.stop();
}

// **Feature: boost-beast-http-transport, Property 6: Per-Node Serialization, Cross-Node
// Concurrency**
// **Validates: Requirement 3.4**
// Concurrent RPCs to different target nodes run genuinely concurrently
// (different connections, different strands), and each gets the right
// response back -- guards against a request/response mixup if the
// connection pool or per-node routing were broken.
BOOST_AUTO_TEST_CASE(concurrent_rpcs_to_multiple_nodes) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 4);

    constexpr int node_count = 4;
    std::vector<std::unique_ptr<kythira::boost_beast_server<test_transport_types>>> servers;
    std::unordered_map<std::uint64_t, std::string> node_map;
    for (int i = 0; i < node_count; ++i) {
        auto port = static_cast<std::uint16_t>(test_multi_bind_port_base + i);
        auto server = std::make_unique<kythira::boost_beast_server<test_transport_types>>(
            ioc, test_bind_address, port, kythira::boost_beast_server_config{},
            kythira::noop_metrics{});
        register_echo_handlers(*server);
        server->start();
        node_map[static_cast<std::uint64_t>(i + 1)] =
            std::string("http://127.0.0.1:") + std::to_string(port);
        servers.push_back(std::move(server));
    }

    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, {},
                                                             kythira::noop_metrics{});

    std::vector<kythira::request_vote_response<>> responses(static_cast<std::size_t>(node_count));
    std::vector<std::thread> rpc_threads;
    for (int i = 0; i < node_count; ++i) {
        kythira::request_vote_request<> req{};
        req._term = static_cast<std::uint64_t>(100 + i);
        rpc_threads.emplace_back([&client, &responses, i, req] {
            responses[static_cast<std::size_t>(i)] =
                std::move(client.send_request_vote(static_cast<std::uint64_t>(i + 1), req,
                                                   std::chrono::milliseconds(30000)))
                    .get();
        });
    }
    for (auto& t : rpc_threads) {
        t.join();
    }

    for (int i = 0; i < node_count; ++i) {
        const auto& resp = responses[static_cast<std::size_t>(i)];
        BOOST_TEST(resp.term() == static_cast<std::uint64_t>(100 + i));
        BOOST_TEST(resp.vote_granted());
    }

    for (auto& server : servers) {
        server->stop();
    }
}

BOOST_AUTO_TEST_SUITE_END()
