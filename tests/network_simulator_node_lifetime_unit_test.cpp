#define BOOST_TEST_MODULE NetworkSimulatorNodeLifetimeUnitTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace network_simulator;

namespace {

constexpr const char* node_a = "node_a";
constexpr const char* node_b = "node_b";

/// A link latency long enough that "the destructor waited for the in-flight
/// send" and "it returned straight away" are unmistakably different
/// measurements. `route_message` sleeps for this while the caller holds its
/// scope ticket, and -- unlike a blocked receive -- nothing cancels it, because
/// the call is genuinely running rather than parked.
constexpr std::chrono::milliseconds link_latency{2000};

/// Long enough that a waited-out receive is obvious, and far longer than any
/// drain should take.
constexpr std::chrono::milliseconds long_receive_timeout{5000};

/// Time given to a worker to get inside the call before the simulator is
/// destroyed. Never silently weakens a test: each case asserts the worker is
/// still inside rather than assuming it.
constexpr std::chrono::milliseconds settle{300};

/// Lower bound on how long the drain must block for the in-flight send. Half
/// the link latency, so scheduler noise cannot produce a false failure while
/// still being an order of magnitude above the ~0ms a missing drain gives.
constexpr std::chrono::milliseconds drain_floor{1000};

/// Upper bound on a cancelled receive's teardown. Generous against `settle`,
/// tiny against `long_receive_timeout`.
constexpr std::chrono::milliseconds drain_budget{1500};

}  // namespace

BOOST_AUTO_TEST_SUITE(simulator_destruction_drains_node_calls)

/**
 * The destructor must not return while a node call is still executing against
 * the simulator's members.
 *
 * Asserted through the latency sleep in `route_message`, deliberately, rather
 * than through a blocked `receive`. Two reasons:
 *
 * 1. **It is observable.** The ticket is held for a known ~2s, so "the drain
 *    waited" is a large positive measurement rather than a race. An earlier
 *    version of this test asserted that a flag set by the worker *after*
 *    `receive()` returned was visible the instant the destructor returned --
 *    which is not something the drain guarantees. The drain orders the
 *    *ticket's release*, not the caller's next statement, so that assertion
 *    failed intermittently under load. It is the difference between a test of
 *    the guarantee and a test of thread scheduling.
 * 2. **It is the uncancellable case.** A blocked receive is cancelled by
 *    `_closing`; a running send is not, and must not be. This pins the half of
 *    the contract that says a drain waits for work that is genuinely running.
 *
 * All assertions run on the main thread: Boost.Test's assertion state is not
 * thread-safe, and a `BOOST_REQUIRE` on the worker would surface a real
 * regression as an unreadable SIGABRT.
 */
BOOST_AUTO_TEST_CASE(destructor_waits_for_an_in_flight_node_call, *boost::unit_test::timeout(60)) {
    auto sim = std::make_unique<NetworkSimulator<DefaultNetworkTypes>>();
    sim->add_node(node_a);
    sim->add_node(node_b);
    sim->add_edge(node_a, node_b, NetworkEdge{link_latency, 1.0});
    sim->start();  // route_message returns immediately unless started

    auto sender = sim->create_node(node_a);

    std::atomic<bool> send_started{false};
    std::atomic<bool> send_returned{false};

    std::thread worker([&] {
        send_started.store(true);
        auto fut = sender->send(Message<DefaultNetworkTypes>{node_a, 1000, node_b, 2000, {}});
        send_returned.store(true);
    });

    while (!send_started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(settle);

    // Guard against a vacuous pass: if the send had already finished there
    // would be nothing in flight and the measurement below would mean nothing.
    BOOST_REQUIRE(!send_returned.load());

    const auto started = std::chrono::steady_clock::now();
    sim.reset();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    // Deleting `_scope->close_and_drain()` from the destructor takes this from
    // ~1700ms to ~0ms.
    BOOST_CHECK_GE(elapsed.count(), drain_floor.count());

    worker.join();
}

/**
 * A `receive` parked in the simulator's condition variable must be cancelled by
 * the destructor, not waited out.
 *
 * This is the half of the change that made the fix affordable. The drain alone
 * is correct but slow: a parked receive holds its ticket for the whole
 * caller-supplied timeout, so a simulator destroyed while any node is waiting
 * on a message stalls for that timeout. Deleting the `_closing` check from
 * `retrieve_message`'s wait predicate takes this from ~0ms to ~4700ms --
 * exactly `long_receive_timeout` minus `settle`.
 */
BOOST_AUTO_TEST_CASE(destructor_cancels_a_blocked_receive, *boost::unit_test::timeout(60)) {
    auto sim = std::make_unique<NetworkSimulator<DefaultNetworkTypes>>();
    sim->add_node(node_a);
    auto receiver = sim->create_node(node_a);

    std::atomic<bool> receive_started{false};
    std::atomic<bool> receive_returned{false};

    std::thread worker([&] {
        receive_started.store(true);
        // No message will ever arrive: the only ways out are the timeout and
        // the destructor's cancellation.
        auto fut = receiver->receive(long_receive_timeout);
        receive_returned.store(true);
    });

    while (!receive_started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(settle);

    BOOST_REQUIRE(!receive_returned.load());

    const auto started = std::chrono::steady_clock::now();
    sim.reset();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    BOOST_CHECK_LT(elapsed.count(), drain_budget.count());

    worker.join();
}

/**
 * A node call that starts *after* the simulator is gone must be refused, not
 * run against freed memory.
 *
 * The node outlives the simulator here on purpose: `_scope` is held by
 * `shared_ptr` precisely so a surviving node still has something valid to ask.
 * Before this change `_simulator` was a raw pointer that nothing ever nulled,
 * so the `if (!_simulator)` guard passed and `retrieve_message` dereferenced a
 * destroyed object on its first line -- the use-after-free AddressSanitizer
 * reported in the boost chaos suite.
 */
BOOST_AUTO_TEST_CASE(node_outliving_the_simulator_refuses_calls, *boost::unit_test::timeout(60)) {
    std::shared_ptr<NetworkSimulator<DefaultNetworkTypes>::node_type> node;
    {
        NetworkSimulator<DefaultNetworkTypes> sim;
        sim.add_node(node_a);
        node = sim.create_node(node_a);
    }

    // Must fail fast rather than blocking for the timeout or touching freed
    // members. Under AddressSanitizer this is also the case that reports.
    const auto started = std::chrono::steady_clock::now();
    auto fut = node->receive(long_receive_timeout);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    BOOST_CHECK_LT(elapsed.count(), drain_budget.count());
}

BOOST_AUTO_TEST_SUITE_END()
