/// @file proxygen_implementation_interop_test.cpp
/// @brief The Proxygen half of the client-implementation ×
///        server-implementation interop grid (`doc/TODO.md`,
///        "client-implementation × server-implementation interop grid").
///
/// `http_implementation_interop_test.cpp` covers the two off-diagonal cells that
/// build on every default CI leg. The remaining four all involve Proxygen, which
/// is an optional vcpkg dependency, so they cannot go in that always-built file
/// and live here instead — behind the same `KYTHIRA_BUILD_PROXYGEN_TRANSPORT`
/// gate `three_way_http_transport_equivalence_test` already uses.
///
///   * `boost_beast_client`  → `proxygen_server`
///   * `proxygen_client`     → `boost_beast_server`
///   * `proxygen_client`     → `cpp_httplib_server`
///   * `cpp_httplib_client`  → `proxygen_server`
///
/// The last of those is not a duplicate of `proxygen_negotiation_integration_test`,
/// which drives a live `proxygen_server` with a **raw** `httplib::Client`. A raw
/// client answers "would an arbitrary HTTP peer interoperate"; it says nothing
/// about `cpp_httplib_client`, which chooses its own headers, its own body
/// framing and its own error mapping. `doc/TODO.md` flagged exactly this gap
/// ("that cell deserves a second look"); with this case the grid is complete for
/// *our own* transports — nine cells, three on the diagonal covered by the
/// equivalence suites, six off it covered by the two interop files.
///
/// Everything except which client meets which server comes from
/// `http_interop_rpc_rig.hpp`, deliberately: a cell's result only means
/// something next to the other cells' results, so a rig that drifted between the
/// two files would turn a finding about the transports into a finding about the
/// test files.
///
/// Each transport keeps its own `Types` bundle, exactly as the equivalence tests
/// do — each one's `requires` clause pins a different `future_template`. What is
/// held constant is `json_serializer` on both sides of every pairing, so the
/// wire format is identical and only the implementation carrying it differs,
/// which is the whole point of the axis being tested.

#define BOOST_TEST_MODULE proxygen_implementation_interop_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/executor_default.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/proxygen_http_transport.hpp>
#include <raft/proxygen_http_transport_impl.hpp>

#include <folly/executors/IOThreadPoolExecutor.h>
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include "beast_test_thread_pool.hpp"
#include "http_interop_rpc_rig.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

// Initialize Folly exactly once for the whole module, for the same reason
// three_way_http_transport_equivalence_test.cpp does: this binary combines the
// Beast (boost::asio) and Proxygen (Folly) transports in one process, and
// several Folly singletons reached through Proxygen — notably folly::Timekeeper,
// via future timeouts — abort if touched before folly::init() has run
// registrationComplete(). Guarded the way the sibling fixtures are: only the
// Folly future backend needs (and provides) folly::Init.
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv[] = {const_cast<char*>("test"), nullptr};
        char** argv_ptr = argv;
        init_obj = std::make_unique<folly::Init>(&argc, &argv_ptr);
    }
    ~FollyInitFixture() = default;

    std::unique_ptr<folly::Init> init_obj;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

using kythira::testing::exercise_all_three;
using kythira::testing::interop_bind_address;
using kythira::testing::interop_node_map_for;
using kythira::testing::register_interop_handlers;

// Fresh ports. 18220-18223 belong to the equivalence suites, 18240 to
// proxygen_negotiation_integration_test and 18290-18291 to
// http_implementation_interop_test; any of them may be running concurrently
// under `ctest -j`. One port per case rather than one per server role, so a
// socket still in TIME_WAIT from the previous case cannot fail the next one's
// bind.
constexpr std::uint16_t proxygen_server_port_for_beast_client = 18300;
constexpr std::uint16_t beast_server_port = 18301;
constexpr std::uint16_t httplib_server_port = 18302;
constexpr std::uint16_t proxygen_server_port_for_httplib_client = 18303;

// How long to give a freshly-started server before the first request. Matches
// the sibling interop and negotiation suites; every server here starts
// asynchronously, so `start()` returning is not the same as the listening
// socket being accepted on.
constexpr std::chrono::milliseconds server_settle_time{200};

using httplib_transport_types =
    kythira::http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                  kythira::executor_default>;
using beast_transport_types =
    kythira::future_default_http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                                 kythira::executor_default>;
using proxygen_transport_types = kythira::future_default_proxygen_transport_types<
    kythira::json_serializer, kythira::noop_metrics, kythira::executor_default>;

}  // namespace

BOOST_AUTO_TEST_SUITE(proxygen_implementation_interop_tests)

/// @brief Cell: `boost_beast_client` → `proxygen_server`.
///
/// Two async implementations that share nothing: the client runs on a
/// caller-owned `boost::asio::io_context`, the server on a Folly
/// `IOThreadPoolExecutor` and Proxygen's own `HTTPServer`.
BOOST_AUTO_TEST_CASE(beast_client_talks_to_a_proxygen_server, *boost::unit_test::timeout(60)) {
    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(2);
    kythira::proxygen_server<proxygen_transport_types> server(
        interop_bind_address, proxygen_server_port_for_beast_client, {}, kythira::noop_metrics{},
        io_executor);
    register_interop_handlers(server);
    server.start();
    std::this_thread::sleep_for(server_settle_time);

    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    auto node_map = interop_node_map_for(proxygen_server_port_for_beast_client);
    kythira::boost_beast_client<beast_transport_types> client(ioc, node_map, {},
                                                              kythira::noop_metrics{});

    exercise_all_three(client);

    server.stop();
}

/// @brief Cell: `proxygen_client` → `boost_beast_server`.
///
/// The mirror image of the case above, and not redundant with it: the two
/// implementations differ on both the sending and the receiving side, so a
/// disagreement can live in either direction independently.
BOOST_AUTO_TEST_CASE(proxygen_client_talks_to_a_beast_server, *boost::unit_test::timeout(60)) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    kythira::boost_beast_server<beast_transport_types> server(
        ioc, interop_bind_address, beast_server_port, {}, kythira::noop_metrics{});
    register_interop_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());
    std::this_thread::sleep_for(server_settle_time);

    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(2);
    auto node_map = interop_node_map_for(beast_server_port);
    kythira::proxygen_client<proxygen_transport_types> client(*io_executor, node_map, {},
                                                              kythira::noop_metrics{});

    exercise_all_three(client);

    server.stop();
}

/// @brief Cell: `proxygen_client` → `cpp_httplib_server`.
///
/// The widest gap in the grid: an HTTP/2-capable client built on Folly's
/// EventBase against the blocking, thread-per-connection reference server.
BOOST_AUTO_TEST_CASE(proxygen_client_talks_to_an_httplib_server, *boost::unit_test::timeout(60)) {
    kythira::cpp_httplib_server<httplib_transport_types> server(
        interop_bind_address, httplib_server_port, {}, kythira::noop_metrics{});
    register_interop_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());
    std::this_thread::sleep_for(server_settle_time);

    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(2);
    auto node_map = interop_node_map_for(httplib_server_port);
    kythira::proxygen_client<proxygen_transport_types> client(*io_executor, node_map, {},
                                                              kythira::noop_metrics{});

    exercise_all_three(client);

    server.stop();
}

/// @brief Cell: `cpp_httplib_client` → `proxygen_server`.
///
/// `proxygen_negotiation_integration_test` reaches this pairing with a *raw*
/// `httplib::Client`, which answers a different question — whether an arbitrary
/// HTTP peer interoperates. This one puts our own client on the wire, so the
/// headers, body framing and error mapping under test are the ones a Kythira
/// node actually sends.
BOOST_AUTO_TEST_CASE(httplib_client_talks_to_a_proxygen_server, *boost::unit_test::timeout(60)) {
    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(2);
    kythira::proxygen_server<proxygen_transport_types> server(
        interop_bind_address, proxygen_server_port_for_httplib_client, {}, kythira::noop_metrics{},
        io_executor);
    register_interop_handlers(server);
    server.start();
    std::this_thread::sleep_for(server_settle_time);

    auto node_map = interop_node_map_for(proxygen_server_port_for_httplib_client);
    kythira::cpp_httplib_client<httplib_transport_types> client(node_map, {},
                                                                kythira::noop_metrics{});

    exercise_all_three(client);

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
