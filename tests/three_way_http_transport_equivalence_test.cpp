// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Requirement 19.5/Property 9 (.kiro/specs/proxygen-http-transport/design.md)
// -- and, satisfied as a direct consequence, .kiro/specs/boost-beast-http-transport/'s
// own Task 15 (its stated prerequisite: a two-way cpp-httplib-vs-Beast
// equivalence test that spec never built). Runs the same RPC sequence
// through all three of this project's HTTP transports -- cpp-httplib
// (include/raft/http_transport.hpp), Boost.Beast
// (include/raft/beast_http_transport.hpp), and Proxygen
// (include/raft/proxygen_http_transport.hpp) -- against each transport's
// own client/server pair, and asserts equivalent externally-observable
// results: identical response field values for the success case, and
// equivalent status-code categories for the failure cases, even though the
// three transports' internal I/O mechanics are genuinely different
// (synchronous/blocking; async on a caller-owned boost::asio::io_context;
// async on a caller-owned folly::IOThreadPoolExecutor, generic bridge or
// Folly fast path).
//
// Only built when both KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT and
// KYTHIRA_BUILD_PROXYGEN_TRANSPORT are enabled (tests/CMakeLists.txt),
// mirroring examples/raft/http_transport_comparison_benchmark.cpp's own
// gating -- this test needs all three transports available in the same
// binary.

#define BOOST_TEST_MODULE three_way_http_transport_equivalence_test
#include <boost/test/unit_test.hpp>

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/proxygen_http_transport.hpp>
#include <raft/proxygen_http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/executor_default.hpp>

#include <httplib.h>

#include <folly/executors/IOThreadPoolExecutor.h>
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Initialize Folly exactly once for the whole test module, mirroring the
// FollyInitFixture the ~118 other Folly-touching Boost.Test binaries in this
// suite install. Proxygen sits on Folly's EventBase/singletons, and several of
// those (notably folly::Timekeeper, reached via future timeouts) are
// registration-gated: accessing one before folly::init() has run
// registrationComplete() aborts the process. This binary is the only one that
// combines the Beast (boost::asio) and Proxygen (Folly) transports in a single
// process, and without this fixture it relied on those singletons never being
// touched -- a fragile assumption that ThreadSanitizer's perturbed thread
// ordering turned into an early, output-less SIGSEGV before Boost.Test's own
// crash handler was installed (see doc/TODO.md). Guarded the same way the
// sibling fixtures are: only the Folly future backend needs (and provides)
// folly::Init.
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

constexpr std::uint64_t test_node_id = 1;
constexpr std::chrono::milliseconds rpc_timeout{5000};

using http_types = kythira::http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                                 kythira::executor_default>;
using beast_types =
    kythira::future_default_http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                                 kythira::executor_default>;
using proxygen_types = kythira::future_default_proxygen_transport_types<
    kythira::json_serializer, kythira::noop_metrics, kythira::executor_default>;

// A deliberately non-trivial (not "always true") response mapping -- if any
// transport silently dropped or miscopied the request's term on its way to
// the handler, or the handler's response on its way back to the client,
// this makes that observable (vote_granted alone wouldn't distinguish "the
// right response" from "some other, coincidentally-also-true response").
template<typename Server> auto register_echo_handlers(Server& server) -> void {
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            kythira::request_vote_response<> resp{};
            resp._term = req.term();
            resp._vote_granted = (req.term() % 2 == 0);
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

BOOST_AUTO_TEST_SUITE(three_way_http_transport_equivalence_tests)

BOOST_AUTO_TEST_CASE(request_vote_success_is_equivalent_across_all_three_transports,
                     *boost::unit_test::timeout(60)) {
    constexpr std::uint16_t http_port = 28390;
    constexpr std::uint16_t beast_port = 28391;
    constexpr std::uint16_t proxygen_port = 28392;

    kythira::cpp_httplib_server<http_types> http_server("127.0.0.1", http_port, {},
                                                        kythira::noop_metrics{});
    register_echo_handlers(http_server);
    http_server.start();
    std::unordered_map<std::uint64_t, std::string> http_node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(http_port)}};
    kythira::cpp_httplib_client<http_types> http_client(http_node_map, {}, kythira::noop_metrics{});

    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    std::vector<std::thread> io_threads;
    for (int i = 0; i < 2; ++i) {
        io_threads.emplace_back([&ioc] { ioc.run(); });
    }
    kythira::boost_beast_server<beast_types> beast_server(ioc, "127.0.0.1", beast_port, {},
                                                          kythira::noop_metrics{});
    register_echo_handlers(beast_server);
    beast_server.start();
    std::unordered_map<std::uint64_t, std::string> beast_node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(beast_port)}};
    kythira::boost_beast_client<beast_types> beast_client(ioc, beast_node_map, {},
                                                          kythira::noop_metrics{});

    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(2);
    kythira::proxygen_server<proxygen_types> proxygen_server_inst(
        "127.0.0.1", proxygen_port, {}, kythira::noop_metrics{}, io_executor);
    register_echo_handlers(proxygen_server_inst);
    proxygen_server_inst.start();
    std::unordered_map<std::uint64_t, std::string> proxygen_node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(proxygen_port)}};
    kythira::proxygen_client<proxygen_types> proxygen_client_inst(*io_executor, proxygen_node_map,
                                                                  {}, kythira::noop_metrics{});

    for (std::uint64_t term : {1ULL, 2ULL, 3ULL, 42ULL, 1000ULL}) {
        kythira::request_vote_request<> req{};
        req._term = term;

        auto http_resp =
            std::move(http_client.send_request_vote(test_node_id, req, rpc_timeout)).get();
        auto beast_resp =
            std::move(beast_client.send_request_vote(test_node_id, req, rpc_timeout)).get();
        auto proxygen_resp =
            std::move(proxygen_client_inst.send_request_vote(test_node_id, req, rpc_timeout)).get();

        BOOST_TEST(http_resp.term() == term);
        BOOST_TEST(beast_resp.term() == term);
        BOOST_TEST(proxygen_resp.term() == term);
        BOOST_TEST(beast_resp.vote_granted() == http_resp.vote_granted());
        BOOST_TEST(proxygen_resp.vote_granted() == http_resp.vote_granted());
    }

    kythira::append_entries_request<> ae_req{};
    ae_req._term = 7;
    auto http_ae =
        std::move(http_client.send_append_entries(test_node_id, ae_req, rpc_timeout)).get();
    auto beast_ae =
        std::move(beast_client.send_append_entries(test_node_id, ae_req, rpc_timeout)).get();
    auto proxygen_ae =
        std::move(proxygen_client_inst.send_append_entries(test_node_id, ae_req, rpc_timeout))
            .get();
    BOOST_TEST(http_ae.term() == 7);
    BOOST_TEST(beast_ae.term() == http_ae.term());
    BOOST_TEST(proxygen_ae.term() == http_ae.term());
    BOOST_TEST(beast_ae.success() == http_ae.success());
    BOOST_TEST(proxygen_ae.success() == http_ae.success());

    http_server.stop();
    beast_server.stop();
    proxygen_server_inst.stop();
    work_guard.reset();
    ioc.stop();
    for (auto& t : io_threads) {
        t.join();
    }
}

// Requirement 4.2/4.4 (both existing specs) and this spec's own Requirement
// 4.2/4.4: malformed request body -> 400, unregistered path -> 404,
// checked directly over the wire (a raw httplib::Client, not any of this
// project's own *_client types, so this exercises exactly what an
// arbitrary HTTP peer would observe -- the same "externally observable"
// bar Requirement 19.5 sets) against all three servers' real listening
// sockets.
BOOST_AUTO_TEST_CASE(
    malformed_and_unknown_endpoint_status_codes_are_equivalent_across_all_three_transports,
    *boost::unit_test::timeout(60)) {
    constexpr std::uint16_t http_port = 28393;
    constexpr std::uint16_t beast_port = 28394;
    constexpr std::uint16_t proxygen_port = 28395;

    kythira::cpp_httplib_server<http_types> http_server("127.0.0.1", http_port, {},
                                                        kythira::noop_metrics{});
    register_echo_handlers(http_server);
    http_server.start();

    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    std::vector<std::thread> io_threads;
    for (int i = 0; i < 2; ++i) {
        io_threads.emplace_back([&ioc] { ioc.run(); });
    }
    kythira::boost_beast_server<beast_types> beast_server(ioc, "127.0.0.1", beast_port, {},
                                                          kythira::noop_metrics{});
    register_echo_handlers(beast_server);
    beast_server.start();

    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(2);
    kythira::proxygen_server<proxygen_types> proxygen_server_inst(
        "127.0.0.1", proxygen_port, {}, kythira::noop_metrics{}, io_executor);
    register_echo_handlers(proxygen_server_inst);
    proxygen_server_inst.start();

    for (auto port : {http_port, beast_port, proxygen_port}) {
        httplib::Client raw_client("127.0.0.1", port);
        raw_client.set_connection_timeout(5, 0);

        auto malformed =
            raw_client.Post("/v1/raft/request_vote", "not valid json", "application/json");
        BOOST_REQUIRE_MESSAGE(malformed, "port " << port << ": request itself failed");
        BOOST_TEST(malformed->status == 400);

        auto unknown = raw_client.Post("/v1/raft/no_such_endpoint", "{}", "application/json");
        BOOST_REQUIRE_MESSAGE(unknown, "port " << port << ": request itself failed");
        BOOST_TEST(unknown->status == 404);
    }

    http_server.stop();
    beast_server.stop();
    proxygen_server_inst.stop();
    work_guard.reset();
    ioc.stop();
    for (auto& t : io_threads) {
        t.join();
    }
}

// Requirement 3.3 (this spec)/the analogous requirement in both existing
// specs: a connection failure (nothing listening on the target port)
// completes the returned future with an exception on all three transports
// -- "matching in kind, not necessarily the same concrete exception type"
// (this spec's own Requirement 3.3 wording), so this checks the common
// std::exception base only, not a specific derived type.
BOOST_AUTO_TEST_CASE(connection_failure_is_reported_as_exception_by_all_three_transports,
                     *boost::unit_test::timeout(60)) {
    // Nothing is bound to any of these ports -- every connection attempt
    // must fail.
    constexpr std::uint16_t closed_http_port = 28396;
    constexpr std::uint16_t closed_beast_port = 28397;
    constexpr std::uint16_t closed_proxygen_port = 28398;

    std::unordered_map<std::uint64_t, std::string> http_node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(closed_http_port)}};
    kythira::cpp_httplib_client<http_types> http_client(http_node_map, {}, kythira::noop_metrics{});

    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    std::vector<std::thread> io_threads;
    for (int i = 0; i < 2; ++i) {
        io_threads.emplace_back([&ioc] { ioc.run(); });
    }
    std::unordered_map<std::uint64_t, std::string> beast_node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(closed_beast_port)}};
    kythira::boost_beast_client<beast_types> beast_client(ioc, beast_node_map, {},
                                                          kythira::noop_metrics{});

    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(2);
    std::unordered_map<std::uint64_t, std::string> proxygen_node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(closed_proxygen_port)}};
    kythira::proxygen_client<proxygen_types> proxygen_client_inst(*io_executor, proxygen_node_map,
                                                                  {}, kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 1;
    BOOST_CHECK_THROW(
        std::move(http_client.send_request_vote(test_node_id, req, rpc_timeout)).get(),
        std::exception);
    BOOST_CHECK_THROW(
        std::move(beast_client.send_request_vote(test_node_id, req, rpc_timeout)).get(),
        std::exception);
    BOOST_CHECK_THROW(
        std::move(proxygen_client_inst.send_request_vote(test_node_id, req, rpc_timeout)).get(),
        std::exception);

    work_guard.reset();
    ioc.stop();
    for (auto& t : io_threads) {
        t.join();
    }
}

BOOST_AUTO_TEST_SUITE_END()
