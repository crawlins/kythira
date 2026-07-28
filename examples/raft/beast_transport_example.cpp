// Example: Boost.Beast HTTP transport, side by side with the existing
// cpp-httplib transport (see http_transport_example.cpp).
//
// Both transports are reached purely through the same transport_types
// concept (include/raft/types.hpp) -- the Raft-facing code that constructs
// a client/server and sends an RPC doesn't need to know or care which one
// it's using, only that it satisfies network_client/network_server. The one
// genuinely new thing this transport asks of a caller, which cpp-httplib
// never did, is a shared net::io_context that something has to actively
// run (.kiro/specs/boost-beast-http-transport/, Requirement 8) -- this
// example exists mainly to make that pattern concrete.

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/executor_default.hpp>

#include <chrono>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
constexpr const char* server_bind_address = "127.0.0.1";
constexpr std::uint16_t server_bind_port = 8091;
constexpr const char* server_url = "http://127.0.0.1:8091";
constexpr std::uint64_t node_id = 1;
constexpr std::chrono::milliseconds rpc_timeout{5000};

// Requirement 19: boost_beast_client/server require this narrower bundle
// (future_template pinned to kythira::future_default), not just any type
// satisfying transport_types -- see beast_http_transport.hpp's own comment
// for why (the async_*_kf adaptors are built on kythira::promise_default).
using example_transport_types =
    kythira::future_default_http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                                 kythira::executor_default>;
}  // namespace

using BeastClient = kythira::boost_beast_client<example_transport_types>;
using BeastServer = kythira::boost_beast_server<example_transport_types>;

auto main() -> int {
    static_assert(kythira::future_default_transport_types<example_transport_types>);

    std::cout << "Boost.Beast HTTP transport example\n";

    // Requirement 8: both the client and server below take this same
    // net::io_context& and never own one. Nothing runs without a caller
    // driving it (the 2 threads below), and a work_guard keeps those
    // threads parked in run() rather than returning immediately -- there is
    // no work queued yet at this point (client/server don't exist), and
    // io_context::run() returns as soon as there's none.
    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    std::vector<std::thread> io_threads;
    for (int i = 0; i < 2; ++i) {
        io_threads.emplace_back([&ioc] { ioc.run(); });
    }

    kythira::noop_metrics metrics;

    BeastServer server(ioc, server_bind_address, server_bind_port, {}, metrics);
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>& request) -> kythira::request_vote_response<> {
            std::cout << "  Server: handling RequestVote for term " << request.term() << "\n";
            kythira::request_vote_response<> response{};
            response._term = request.term();
            response._vote_granted = true;
            return response;
        });
    server.register_append_entries_handler(
        [](const kythira::append_entries_request<>& request) -> kythira::append_entries_response<> {
            kythira::append_entries_response<> response{};
            response._term = request.term();
            response._success = true;
            return response;
        });
    server.register_install_snapshot_handler([](const kythira::install_snapshot_request<>& request)
                                                 -> kythira::install_snapshot_response<> {
        kythira::install_snapshot_response<> response{};
        response._term = request.term();
        return response;
    });
    server.start();
    std::cout << "  Server started on " << server_bind_address << ":" << server_bind_port << "\n";

    std::unordered_map<std::uint64_t, std::string> node_id_to_url_map{{node_id, server_url}};
    BeastClient client(ioc, node_id_to_url_map, {}, metrics);

    kythira::request_vote_request<> request{};
    request._term = 1;
    // send_request_vote() returns immediately -- the connect/write/read
    // sequence (Phase 2.5's thenValue chain) actually happens on whichever
    // io_context thread picks up each step. .get() blocks this thread until
    // it's done, same as it would with any other future-satisfying type.
    auto response = std::move(client.send_request_vote(node_id, request, rpc_timeout)).get();
    std::cout << "  Client: RequestVote response, term=" << response.term()
              << " vote_granted=" << response.vote_granted() << "\n";

    server.stop();       // stops accepting, drains in-flight requests -- does NOT stop ioc
    work_guard.reset();  // let ioc.run() return once nothing else keeps it busy
    ioc.stop();          // caller's own responsibility, only once nothing else needs it
    for (auto& t : io_threads) {
        t.join();
    }

    std::cout << "Done.\n";
    return 0;
}
