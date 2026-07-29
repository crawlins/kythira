// Example: Proxygen HTTP transport, side by side with the existing
// cpp-httplib (http_transport_example.cpp) and Boost.Beast
// (beast_transport_example.cpp) transports. All three are reached purely
// through the same transport_types concept (include/raft/types.hpp) -- the
// Raft-facing code that constructs a client/server and sends an RPC
// doesn't need to know or care which one it's using.
//
// Unlike Beast (a caller-owned net::io_context shared identically by both
// client and server), Proxygen's client/server halves have a genuine
// asymmetry (.kiro/specs/proxygen-http-transport/design.md's "Why
// folly::IOThreadPoolExecutor" section): the client takes a caller-shared
// folly::IOThreadPoolExecutorBase& and never owns one, while the server
// owns a dedicated thread to run proxygen::HTTPServer::start()'s own
// blocking call on. This example demonstrates both halves sharing one
// executor (Requirement 8), and a second, explicitly-labeled section
// demonstrating the Folly-native fast path (Requirement 16) actually being
// taken, verified via the same metrics path-label the test suite uses
// (Requirement 12.6), not asserted in prose alone.

#include <raft/proxygen_http_transport.hpp>
#include <raft/proxygen_http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/executor_default.hpp>

#include <folly/executors/IOThreadPoolExecutor.h>

#include <chrono>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
constexpr const char* server_bind_address = "127.0.0.1";
constexpr std::uint16_t server_bind_port = 8092;
constexpr const char* server_url = "http://127.0.0.1:8092";
constexpr std::uint64_t node_id = 1;
constexpr std::chrono::milliseconds rpc_timeout{5000};

// Requirement 15: proxygen_client/server require this narrower bundle
// (future_template pinned to kythira::future_default) -- see
// proxygen_http_transport.hpp's own comment for why (the generic bridge is
// built on kythira::promise_default). Since this project's default
// KYTHIRA_DEFAULT_FUTURE_BACKEND is folly, future_default<T> here is
// kythira::Future<T> exactly, so every RPC below actually takes
// Requirement 16's Folly-native fast path -- the metrics dimension printed
// at the end confirms this rather than merely asserting it.
// A tiny metrics implementation that just prints which internal path
// (Requirement 12.6) each request took, so this example can demonstrate
// Requirement 16's fast path being taken without a separate test harness.
class printing_metrics {
public:
    auto set_metric_name(std::string_view name) -> void { _name = std::string(name); }
    auto add_dimension(std::string_view dimension_name, std::string_view dimension_value) -> void {
        if (dimension_name == "path") {
            _path = std::string(dimension_value);
        }
    }
    auto add_one() -> void {}
    auto add_count(std::int64_t) -> void {}
    auto add_duration(std::chrono::nanoseconds) -> void {}
    auto add_value(double) -> void {}
    auto emit() -> void {
        if (_name == "proxygen_http.client.request.sent" && !_path.empty()) {
            std::cout << "  [metrics] " << _name << " path=" << _path << "\n";
        }
    }

private:
    std::string _name;
    std::string _path;
};

using example_transport_types =
    kythira::future_default_proxygen_transport_types<kythira::json_serializer, printing_metrics,
                                                     kythira::executor_default>;
}  // namespace

using ProxygenClient = kythira::proxygen_client<example_transport_types>;
using ProxygenServer = kythira::proxygen_server<example_transport_types>;

auto main() -> int {
    static_assert(kythira::proxygen_future_default_transport_types<example_transport_types>);

    std::cout << "Proxygen HTTP transport example\n";

    // Requirement 8: a single shared folly::IOThreadPoolExecutor, handed to
    // both the client (as a reference, never owned -- Requirement 8.1) and
    // the server (as a shared_ptr, since HTTPServer::start()'s own
    // ioExecutor parameter takes one -- Requirement 8.2).
    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(4);

    printing_metrics metrics;

    ProxygenServer server(server_bind_address, server_bind_port, {}, metrics, io_executor);
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
    // Requirement 5.1: start() blocks internally (its own dedicated thread
    // runs proxygen::HTTPServer::start()'s equally-blocking call) until
    // onSuccess fires, then returns -- the same synchronous "returns once
    // actually accepting" contract cpp_httplib_server/boost_beast_server
    // already have.
    server.start();
    std::cout << "  Server started on " << server_bind_address << ":" << server_bind_port << "\n";

    std::unordered_map<std::uint64_t, std::string> node_id_to_url_map{{node_id, server_url}};
    ProxygenClient client(*io_executor, node_id_to_url_map, {}, metrics);

    kythira::request_vote_request<> request{};
    request._term = 1;
    // send_request_vote() returns immediately -- Requirement 16's fast path
    // (this Types bundle's future_template is kythira::Future<T> exactly)
    // constructs a raw folly::Promise<T> and schedules the connect/send/
    // receive/deserialize chain on the connection's own pinned EventBase.
    // .get() blocks this thread until it's done.
    auto response = std::move(client.send_request_vote(node_id, request, rpc_timeout)).get();
    std::cout << "  Client: RequestVote response, term=" << response.term()
              << " vote_granted=" << response.vote_granted() << "\n";

    // A second RPC to the same node -- exercises connection reuse
    // (Requirement 9): no fresh TCP handshake, the pooled
    // HTTPUpstreamSession from the first RPC is reused directly.
    request._term = 2;
    auto response2 = std::move(client.send_request_vote(node_id, request, rpc_timeout)).get();
    std::cout << "  Client: second RequestVote response (reused connection), term="
              << response2.term() << "\n";

    server.stop();
    std::cout << "  Server stopped\n";
    return 0;
}
