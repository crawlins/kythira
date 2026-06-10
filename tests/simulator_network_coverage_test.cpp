/**
 * Coverage tests for simulator_network.hpp targeting the paths not reached by other tests:
 *   - move constructor and move assignment of simulator_network_server
 *   - is_running() state transitions
 *   - register_install_snapshot_handler + process_messages invoking it
 *   - register_append_entries_handler invoked through server
 *   - send_install_snapshot on the client side
 *   - double-start / double-stop guard on server
 */

#define BOOST_TEST_MODULE simulator_network_coverage_test
#include <boost/test/unit_test.hpp>

#include <raft/simulator_network.hpp>
#include <raft/types.hpp>
#include <raft/json_serializer.hpp>

#include <network_simulator/network_simulator.hpp>
#include <network_simulator/types.hpp>

#include <folly/init/Init.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("simulator_network_coverage_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {

using net_types   = kythira::raft_simulator_network_types<std::string>;
using serializer  = kythira::json_rpc_serializer<std::vector<std::byte>>;
using data_t      = std::vector<std::byte>;
using client_t    = kythira::simulator_network_client<net_types, serializer, data_t>;
using server_t    = kythira::simulator_network_server<net_types, serializer, data_t>;

template<typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{2000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}

} // namespace

// ── Suite: server lifecycle ───────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(server_lifecycle_suite)

BOOST_AUTO_TEST_CASE(start_stop_is_running, * boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<net_types>{};
    sim.start();
    auto node = sim.create_node("1");

    server_t srv{node, serializer{}};
    BOOST_CHECK(!srv.is_running());

    srv.start();
    BOOST_CHECK(srv.is_running());

    // Double-start should be a no-op
    BOOST_CHECK_NO_THROW(srv.start());
    BOOST_CHECK(srv.is_running());

    srv.stop();
    BOOST_CHECK(!srv.is_running());

    // Double-stop should be a no-op
    BOOST_CHECK_NO_THROW(srv.stop());
    BOOST_CHECK(!srv.is_running());
}

BOOST_AUTO_TEST_CASE(move_constructor_stopped, * boost::unit_test::timeout(10)) {
    // Moving a stopped server is safe — the thread hasn't captured `this` yet.
    auto sim = network_simulator::NetworkSimulator<net_types>{};
    sim.start();
    auto node = sim.create_node("1");

    server_t srv1{node, serializer{}};
    BOOST_CHECK(!srv1.is_running());

    server_t srv2{std::move(srv1)};
    BOOST_CHECK(!srv2.is_running());

    // Start the moved-into server and verify it works
    srv2.start();
    BOOST_CHECK(srv2.is_running());
    srv2.stop();
    BOOST_CHECK(!srv2.is_running());
}

BOOST_AUTO_TEST_CASE(move_assignment_stopped, * boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<net_types>{};
    sim.start();
    auto node1 = sim.create_node("1");
    auto node2 = sim.create_node("2");

    server_t srv1{node1, serializer{}};
    server_t srv2{node2, serializer{}};
    BOOST_CHECK(!srv1.is_running());

    // Move-assign a stopped server into another stopped server
    srv2 = std::move(srv1);
    BOOST_CHECK(!srv2.is_running());

    srv2.start();
    BOOST_CHECK(srv2.is_running());
    srv2.stop();
}

BOOST_AUTO_TEST_SUITE_END()

// ── Suite: handler registration and dispatch ──────────────────────────────────
//
// The simulator's retrieve_message(addr, timeout) immediately throws if the
// queue is empty (no blocking poll), so the high-level send_*() helpers that
// chain send→receive in one synchronous step cannot be used for roundtrip tests.
//
// Instead we: (1) send the raw serialized payload directly via node->send(),
// (2) poll for the handler invocation via an atomic counter, and
// (3) call register_*_handler to cover that code path.

BOOST_AUTO_TEST_SUITE(handler_dispatch_suite)

// Helper: create a two-node simulator with a bidirectional edge so messages route.
void add_edge_both(
    network_simulator::NetworkSimulator<net_types>& sim,
    const std::string& a, const std::string& b)
{
    network_simulator::NetworkEdge edge{std::chrono::milliseconds{0}, 1.0};
    sim.add_edge(a, b, edge);
    sim.add_edge(b, a, edge);
}

// Helper: send a raw serialized message from node_src to node_dst address.
void send_raw(
    std::shared_ptr<network_simulator::NetworkNode<net_types>> src,
    const std::string& dst_addr,
    const std::vector<std::byte>& payload)
{
    net_types::message_type msg(src->address(), 0, dst_addr, 5000, payload);
    src->send(std::move(msg));  // fire-and-forget; delivers synchronously to dst queue
}

BOOST_AUTO_TEST_CASE(install_snapshot_handler, * boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<net_types>{};
    sim.start();

    auto node1 = sim.create_node("1");
    auto node2 = sim.create_node("2");
    add_edge_both(sim, "1", "2");

    std::atomic<int> handler_calls{0};

    server_t srv{node2, serializer{}};
    srv.register_install_snapshot_handler(
        [&](const kythira::install_snapshot_request<>& req) -> kythira::install_snapshot_response<> {
            ++handler_calls;
            return kythira::install_snapshot_response<>{req._term};
        }
    );
    srv.start();

    kythira::install_snapshot_request<> req;
    req._leader_id            = 1;
    req._term                 = 5;
    req._last_included_index  = 10;
    req._last_included_term   = 3;
    req._offset               = 0;
    req._done                 = true;

    serializer ser;
    send_raw(node1, "2", ser.serialize(req));

    // The server's background thread polls every 100 ms; give it time to process
    BOOST_CHECK(wait_for([&]{ return handler_calls.load() >= 1; }, std::chrono::milliseconds{2000}));
    BOOST_CHECK_GE(handler_calls.load(), 1);

    srv.stop();
}

BOOST_AUTO_TEST_CASE(append_entries_handler, * boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<net_types>{};
    sim.start();

    auto node1 = sim.create_node("1");
    auto node2 = sim.create_node("2");
    add_edge_both(sim, "1", "2");

    std::atomic<int> handler_calls{0};

    server_t srv{node2, serializer{}};
    srv.register_append_entries_handler(
        [&](const kythira::append_entries_request<>& req) -> kythira::append_entries_response<> {
            ++handler_calls;
            return kythira::append_entries_response<>{req._term, true, std::nullopt, std::nullopt};
        }
    );
    srv.start();

    kythira::append_entries_request<> req;
    req._leader_id      = 1;
    req._term           = 3;
    req._prev_log_index = 0;
    req._prev_log_term  = 0;
    req._leader_commit  = 0;

    serializer ser;
    send_raw(node1, "2", ser.serialize(req));

    BOOST_CHECK(wait_for([&]{ return handler_calls.load() >= 1; }, std::chrono::milliseconds{2000}));
    BOOST_CHECK_GE(handler_calls.load(), 1);

    srv.stop();
}

BOOST_AUTO_TEST_CASE(request_vote_handler, * boost::unit_test::timeout(10)) {
    auto sim = network_simulator::NetworkSimulator<net_types>{};
    sim.start();

    auto node1 = sim.create_node("1");
    auto node2 = sim.create_node("2");
    add_edge_both(sim, "1", "2");

    std::atomic<int> handler_calls{0};

    server_t srv{node2, serializer{}};
    srv.register_request_vote_handler(
        [&](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            ++handler_calls;
            return kythira::request_vote_response<>{req._term, true};
        }
    );
    srv.start();

    kythira::request_vote_request<> req;
    req._candidate_id   = 1;
    req._term           = 2;
    req._last_log_index = 0;
    req._last_log_term  = 0;

    serializer ser;
    send_raw(node1, "2", ser.serialize(req));

    BOOST_CHECK(wait_for([&]{ return handler_calls.load() >= 1; }, std::chrono::milliseconds{2000}));
    BOOST_CHECK_GE(handler_calls.load(), 1);

    srv.stop();
}

BOOST_AUTO_TEST_SUITE_END()
