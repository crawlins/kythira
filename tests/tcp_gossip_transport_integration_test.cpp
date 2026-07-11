// Integration test: real TCP sockets, real background gossip thread, multiple
// tcp_gossip_peer2peer_replicator instances constructed in a single test
// process on distinct loopback ports. Deliberately NOT spawning subprocesses
// (see .kiro/specs/peer2peer-gossip-transport/tasks.md's anti-flakiness note
// — ca_cluster_node_test.cpp's posix_spawn-based pattern was this project's
// dominant CI flake source under ctest -j$(nproc) CPU contention).
#define BOOST_TEST_MODULE tcp_gossip_transport_integration_test
#include <boost/test/unit_test.hpp>

#include <raft/tcp_gossip_transport.hpp>

#include <chrono>
#include <thread>

namespace {

using gossip_t =
    kythira::tcp_gossip_peer2peer_replicator<std::uint64_t, std::string, std::uint64_t>;

constexpr std::uint16_t k_port1 = 19701;
constexpr std::uint16_t k_port2 = 19702;
constexpr std::uint16_t k_port3 = 19703;

template<typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{5000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

}  // namespace

BOOST_AUTO_TEST_CASE(gossip_round_trip_propagates_advertised_progress,
                     *boost::unit_test::timeout(30)) {
    kythira::tcp_gossip_config<std::uint64_t, std::string> cfg1;
    cfg1.listen_port = k_port1;
    cfg1.fanout = 3;
    cfg1.gossip_round_interval = std::chrono::milliseconds{50};
    cfg1.freshness_interval = std::chrono::seconds{5};
    cfg1.address_book = {{1, "127.0.0.1:" + std::to_string(k_port1)},
                         {2, "127.0.0.1:" + std::to_string(k_port2)},
                         {3, "127.0.0.1:" + std::to_string(k_port3)}};

    auto cfg2 = cfg1;
    cfg2.listen_port = k_port2;
    auto cfg3 = cfg1;
    cfg3.listen_port = k_port3;

    gossip_t node1{cfg1};
    gossip_t node2{cfg2};
    gossip_t node3{cfg3};

    // Construction is cheap and starts no thread/socket (so this object can
    // be freely moved into node_config<Types>/node<Types>); start() is what
    // actually opens the listener and begins gossiping.
    node1.start();
    node2.start();
    node3.start();

    std::move(node1.update_membership({1, 2, 3})).get();
    std::move(node2.update_membership({1, 2, 3})).get();
    std::move(node3.update_membership({1, 2, 3})).get();

    // node1 advertises far-ahead progress; confirm it becomes visible on
    // node3 (which never talks to node1 directly except via gossip fanout)
    // within a small, bounded number of rounds.
    std::move(node1.advertise_progress(1, "127.0.0.1:" + std::to_string(k_port1), 5, 12345)).get();
    std::move(node2.advertise_progress(2, "127.0.0.1:" + std::to_string(k_port2), 1, 0)).get();
    std::move(node3.advertise_progress(3, "127.0.0.1:" + std::to_string(k_port3), 1, 0)).get();

    bool converged = wait_until([&] {
        auto source =
            std::move(node3.find_catch_up_source(12345, 12345, std::chrono::milliseconds{100}))
                .get();
        return source.has_value() && source->node_id == 1u;
    });
    BOOST_CHECK(converged);
}
