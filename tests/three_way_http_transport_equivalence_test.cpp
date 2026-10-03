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
#include "http_limit_test_helpers.hpp"
#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/proxygen_http_transport.hpp>
#include <raft/proxygen_http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/exceptions.hpp>
#include <raft/executor_default.hpp>

#include <httplib.h>

#include <folly/executors/IOThreadPoolExecutor.h>
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <atomic>
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
    server.register_fetch_log_entries_handler([](const kythira::fetch_log_entries_request<>& req)
                                                  -> kythira::fetch_log_entries_response<> {
        kythira::fetch_log_entries_response<> resp{};
        resp._responder_id = req.requester_id();
        resp._available = true;
        resp._prev_log_term = req.from_index();
        for (auto idx = req.from_index(); idx <= req.to_index(); ++idx) {
            resp._entries.push_back({idx, idx, {}});
        }
        return resp;
    });
}

// The two optional extensions (.kiro/specs/http-coap-pre-vote-timeout-now/).
// Each echoes `group_id`, since a multi-Raft host routes on it and a transport
// that dropped it would hand the RPC to the wrong group.
template<typename Server> auto register_extension_echo_handlers(Server& server) -> void {
    server.register_request_pre_vote_handler(
        [](const kythira::request_pre_vote_request<>& req) -> kythira::request_pre_vote_response<> {
            kythira::request_pre_vote_response<> resp{};
            resp._term = req.term();
            resp._vote_granted = req.last_log_index() >= req.last_log_term();
            resp._group_id = req.group_id();
            return resp;
        });
    server.register_timeout_now_handler(
        [](const kythira::timeout_now_request<>& req) -> kythira::timeout_now_response<> {
            kythira::timeout_now_response<> resp{};
            resp._term = req.term();
            resp._success = req.last_log_index() % 2 == 0;
            resp._group_id = req.group_id();
            return resp;
        });
}

/// The three servers on fixed ports with their io threads, and a client for
/// each, torn down in the right order. Only for the extension cases below;
/// the original cases keep their inline setup.
struct three_transports {
    three_transports(std::uint16_t http_port, std::uint16_t beast_port, std::uint16_t proxygen_port,
                     bool with_extensions)
        : http_server("127.0.0.1", http_port, {}, kythira::noop_metrics{}),
          http_client(node_map(http_port), {}, kythira::noop_metrics{}),
          work_guard(boost::asio::make_work_guard(ioc)),
          beast_server(ioc, "127.0.0.1", beast_port, {}, kythira::noop_metrics{}),
          beast_client(ioc, node_map(beast_port), {}, kythira::noop_metrics{}),
          io_executor(std::make_shared<folly::IOThreadPoolExecutor>(2)),
          proxygen_server_inst("127.0.0.1", proxygen_port, {}, kythira::noop_metrics{},
                               io_executor),
          proxygen_client_inst(*io_executor, node_map(proxygen_port), {}, kythira::noop_metrics{}) {
        for (int i = 0; i < 2; ++i) {
            io_threads.emplace_back([this] { ioc.run(); });
        }
        register_echo_handlers(http_server);
        register_echo_handlers(beast_server);
        register_echo_handlers(proxygen_server_inst);
        if (with_extensions) {
            register_extension_echo_handlers(http_server);
            register_extension_echo_handlers(beast_server);
            register_extension_echo_handlers(proxygen_server_inst);
        }
        http_server.start();
        beast_server.start();
        proxygen_server_inst.start();
    }

    ~three_transports() {
        http_server.stop();
        beast_server.stop();
        proxygen_server_inst.stop();
        work_guard.reset();
        ioc.stop();
        for (auto& t : io_threads) {
            t.join();
        }
    }

    three_transports(const three_transports&) = delete;
    auto operator=(const three_transports&) -> three_transports& = delete;

    static auto node_map(std::uint16_t port) -> std::unordered_map<std::uint64_t, std::string> {
        return {{test_node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    }

    kythira::cpp_httplib_server<http_types> http_server;
    kythira::cpp_httplib_client<http_types> http_client;
    boost::asio::io_context ioc;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard;
    std::vector<std::thread> io_threads;
    kythira::boost_beast_server<beast_types> beast_server;
    kythira::boost_beast_client<beast_types> beast_client;
    std::shared_ptr<folly::IOThreadPoolExecutor> io_executor;
    kythira::proxygen_server<proxygen_types> proxygen_server_inst;
    kythira::proxygen_client<proxygen_types> proxygen_client_inst;
};

/// Whether `f` fails with exactly `rpc_not_implemented_exception`.
template<typename Future> auto fails_not_implemented(Future&& f) -> bool {
    try {
        std::ignore = std::forward<Future>(f).get();
    } catch (const kythira::rpc_not_implemented_exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
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

    kythira::fetch_log_entries_request<> fl_req{};
    fl_req._requester_id = 9;
    fl_req._from_index = 5;
    fl_req._to_index = 7;
    auto http_fl =
        std::move(http_client.send_fetch_log_entries(test_node_id, fl_req, rpc_timeout)).get();
    auto beast_fl =
        std::move(beast_client.send_fetch_log_entries(test_node_id, fl_req, rpc_timeout)).get();
    auto proxygen_fl =
        std::move(proxygen_client_inst.send_fetch_log_entries(test_node_id, fl_req, rpc_timeout))
            .get();
    for (const auto& fl : {http_fl, beast_fl, proxygen_fl}) {
        BOOST_TEST(fl.responder_id() == 9u);
        BOOST_TEST(fl.available());
        BOOST_TEST(fl.prev_log_term() == 5u);
        BOOST_REQUIRE_EQUAL(fl.entries().size(), 3u);
        BOOST_TEST(fl.entries().back().index() == 7u);
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

// .kiro/specs/http-server-request-limits/ Requirement 6.5: one byte over
// max_request_body_size, with a Content-Length, gets the same status and
// Content-Type from every server, read off the wire.
BOOST_AUTO_TEST_CASE(oversized_body_is_refused_the_same_way_by_all_three_transports,
                     *boost::unit_test::timeout(60)) {
    constexpr std::uint16_t http_port = 28399;
    constexpr std::uint16_t beast_port = 28400;
    constexpr std::uint16_t proxygen_port = 28401;
    constexpr std::size_t limit = 64;

    kythira::cpp_httplib_server_config http_config;
    http_config.max_request_body_size = limit;
    kythira::cpp_httplib_server<http_types> http_server("127.0.0.1", http_port, http_config,
                                                        kythira::noop_metrics{});
    register_echo_handlers(http_server);
    http_server.start();

    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    std::vector<std::thread> io_threads;
    for (int i = 0; i < 2; ++i) {
        io_threads.emplace_back([&ioc] { ioc.run(); });
    }
    kythira::boost_beast_server_config beast_config;
    beast_config.max_request_body_size = limit;
    kythira::boost_beast_server<beast_types> beast_server(ioc, "127.0.0.1", beast_port,
                                                          beast_config, kythira::noop_metrics{});
    register_echo_handlers(beast_server);
    beast_server.start();

    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(2);
    kythira::proxygen_server_config proxygen_config;
    proxygen_config.max_request_body_size = limit;
    kythira::proxygen_server<proxygen_types> proxygen_server_inst(
        "127.0.0.1", proxygen_port, proxygen_config, kythira::noop_metrics{}, io_executor);
    register_echo_handlers(proxygen_server_inst);
    proxygen_server_inst.start();

    namespace limits = kythira::testing::http_limits;
    for (auto port : {http_port, beast_port, proxygen_port}) {
        BOOST_TEST_INFO("port " << port);
        limits::raw_connection conn("127.0.0.1", port);
        BOOST_REQUIRE(conn.connected());
        conn.send_all(limits::sized_request(limit + 1));
        auto headers = conn.read_headers(std::chrono::seconds(10));
        BOOST_TEST(limits::status_of(headers) == 413);
        BOOST_TEST(limits::header_of(headers, "Content-Type") == "text/plain");
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

// .kiro/specs/http-coap-pre-vote-timeout-now/ Requirement 6.1: both extension
// RPCs round-trip through all three transports with every field, group_id
// included, and the three agree.
BOOST_AUTO_TEST_CASE(extension_rpcs_are_equivalent_across_all_three_transports,
                     *boost::unit_test::timeout(60)) {
    three_transports t(28420, 28421, 28422, /*with_extensions=*/true);

    for (std::uint64_t term : {1ULL, 2ULL, 41ULL}) {
        kythira::request_pre_vote_request<> pv{};
        pv._term = term;
        pv._candidate_id = 3;
        pv._last_log_index = term * 2;
        pv._last_log_term = term + 1;
        pv._group_id = 77;
        auto http_pv =
            std::move(t.http_client.send_request_pre_vote(test_node_id, pv, rpc_timeout)).get();
        auto beast_pv =
            std::move(t.beast_client.send_request_pre_vote(test_node_id, pv, rpc_timeout)).get();
        auto proxygen_pv =
            std::move(t.proxygen_client_inst.send_request_pre_vote(test_node_id, pv, rpc_timeout))
                .get();
        BOOST_TEST(http_pv.term() == term);
        BOOST_TEST(http_pv.group_id() == 77U);
        BOOST_TEST(http_pv.vote_granted() == (term * 2 >= term + 1));
        BOOST_TEST(beast_pv.term() == http_pv.term());
        BOOST_TEST(proxygen_pv.term() == http_pv.term());
        BOOST_TEST(beast_pv.vote_granted() == http_pv.vote_granted());
        BOOST_TEST(proxygen_pv.vote_granted() == http_pv.vote_granted());
        BOOST_TEST(beast_pv.group_id() == http_pv.group_id());
        BOOST_TEST(proxygen_pv.group_id() == http_pv.group_id());

        kythira::timeout_now_request<> tn{};
        tn._term = term;
        tn._leader_id = 2;
        tn._last_log_index = term;
        tn._group_id = 78;
        auto http_tn =
            std::move(t.http_client.send_timeout_now(test_node_id, tn, rpc_timeout)).get();
        auto beast_tn =
            std::move(t.beast_client.send_timeout_now(test_node_id, tn, rpc_timeout)).get();
        auto proxygen_tn =
            std::move(t.proxygen_client_inst.send_timeout_now(test_node_id, tn, rpc_timeout)).get();
        BOOST_TEST(http_tn.term() == term);
        BOOST_TEST(http_tn.group_id() == 78U);
        BOOST_TEST(http_tn.success() == (term % 2 == 0));
        BOOST_TEST(beast_tn.term() == http_tn.term());
        BOOST_TEST(proxygen_tn.term() == http_tn.term());
        BOOST_TEST(beast_tn.success() == http_tn.success());
        BOOST_TEST(proxygen_tn.success() == http_tn.success());
        BOOST_TEST(beast_tn.group_id() == http_tn.group_id());
        BOOST_TEST(proxygen_tn.group_id() == http_tn.group_id());
    }
}

// Requirement 3.1, 3.3 and 6.4: a server with no handler for an extension
// answers 501 on the wire, and every client reports it as
// rpc_not_implemented_exception. The mandatory RPCs are untouched by that.
BOOST_AUTO_TEST_CASE(unregistered_extension_handlers_answer_not_implemented_on_all_three,
                     *boost::unit_test::timeout(60)) {
    three_transports t(28423, 28424, 28425, /*with_extensions=*/false);

    for (auto port : {std::uint16_t{28423}, std::uint16_t{28424}, std::uint16_t{28425}}) {
        httplib::Client raw_client("127.0.0.1", port);
        raw_client.set_connection_timeout(5, 0);
        for (const auto* path : {"/v1/raft/request_pre_vote", "/v1/raft/timeout_now"}) {
            auto result = raw_client.Post(path, "{}", "application/json");
            BOOST_REQUIRE_MESSAGE(result, "port " << port << ": request itself failed");
            BOOST_TEST(result->status == 501, "port " << port << " " << path);
        }
    }

    const kythira::request_pre_vote_request<> pv{};
    const kythira::timeout_now_request<> tn{};
    BOOST_TEST(
        fails_not_implemented(t.http_client.send_request_pre_vote(test_node_id, pv, rpc_timeout)));
    BOOST_TEST(
        fails_not_implemented(t.beast_client.send_request_pre_vote(test_node_id, pv, rpc_timeout)));
    BOOST_TEST(fails_not_implemented(
        t.proxygen_client_inst.send_request_pre_vote(test_node_id, pv, rpc_timeout)));
    BOOST_TEST(
        fails_not_implemented(t.http_client.send_timeout_now(test_node_id, tn, rpc_timeout)));
    BOOST_TEST(
        fails_not_implemented(t.beast_client.send_timeout_now(test_node_id, tn, rpc_timeout)));
    BOOST_TEST(fails_not_implemented(
        t.proxygen_client_inst.send_timeout_now(test_node_id, tn, rpc_timeout)));
}

// Requirement 3.3, 3.4 and 6.3: a peer on a build that predates the
// extensions has no route for them and answers 404. Played here by a raw
// httplib server that serves only the three mandatory paths, so no transport
// code is forked. On an extension the 404 is rpc_not_implemented_exception
// after exactly one request; on RequestVote it stays an ordinary client error.
BOOST_AUTO_TEST_CASE(an_older_peer_without_the_routes_is_reported_as_not_implemented,
                     *boost::unit_test::timeout(60)) {
    constexpr std::uint16_t older_port = 28426;
    httplib::Server older_peer;
    std::atomic<int> pre_vote_hits{0};
    older_peer.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response&) {
        if (req.path == "/v1/raft/request_pre_vote") {
            pre_vote_hits.fetch_add(1);
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    for (const auto* path :
         {"/v1/raft/request_vote", "/v1/raft/append_entries", "/v1/raft/install_snapshot"}) {
        older_peer.Post(path, [](const httplib::Request&, httplib::Response& res) {
            res.status = 500;  // Never reached by this case.
        });
    }
    BOOST_REQUIRE(older_peer.bind_to_port("127.0.0.1", older_port));
    std::thread older_thread([&] { older_peer.listen_after_bind(); });
    older_peer.wait_until_ready();

    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    std::thread io_thread([&ioc] { ioc.run(); });
    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(1);
    {
        const auto map = three_transports::node_map(older_port);
        kythira::cpp_httplib_client<http_types> http_client(map, {}, kythira::noop_metrics{});
        kythira::boost_beast_client<beast_types> beast_client(ioc, map, {},
                                                              kythira::noop_metrics{});
        kythira::proxygen_client<proxygen_types> proxygen_client_inst(*io_executor, map, {},
                                                                      kythira::noop_metrics{});

        const kythira::request_pre_vote_request<> pv{};
        BOOST_TEST(fails_not_implemented(
            http_client.send_request_pre_vote(test_node_id, pv, rpc_timeout)));
        BOOST_TEST(pre_vote_hits.load() == 1);
        BOOST_TEST(fails_not_implemented(
            beast_client.send_request_pre_vote(test_node_id, pv, rpc_timeout)));
        BOOST_TEST(pre_vote_hits.load() == 2);
        BOOST_TEST(fails_not_implemented(
            proxygen_client_inst.send_request_pre_vote(test_node_id, pv, rpc_timeout)));
        BOOST_TEST(pre_vote_hits.load() == 3);

        const kythira::timeout_now_request<> tn{};
        BOOST_TEST(
            fails_not_implemented(http_client.send_timeout_now(test_node_id, tn, rpc_timeout)));
        BOOST_TEST(
            fails_not_implemented(beast_client.send_timeout_now(test_node_id, tn, rpc_timeout)));
        BOOST_TEST(fails_not_implemented(
            proxygen_client_inst.send_timeout_now(test_node_id, tn, rpc_timeout)));
    }
    older_peer.stop();
    older_thread.join();

    // And a 404 on a mandatory RPC is a misconfigured peer, not an older one:
    // checked against a server with no routes at all.
    constexpr std::uint16_t empty_port = 28427;
    httplib::Server empty_peer;
    BOOST_REQUIRE(empty_peer.bind_to_port("127.0.0.1", empty_port));
    std::thread empty_thread([&] { empty_peer.listen_after_bind(); });
    empty_peer.wait_until_ready();
    {
        const auto map = three_transports::node_map(empty_port);
        kythira::cpp_httplib_client<http_types> http_client(map, {}, kythira::noop_metrics{});
        kythira::boost_beast_client<beast_types> beast_client(ioc, map, {},
                                                              kythira::noop_metrics{});
        kythira::proxygen_client<proxygen_types> proxygen_client_inst(*io_executor, map, {},
                                                                      kythira::noop_metrics{});
        const kythira::request_vote_request<> rv{};
        BOOST_CHECK_THROW(
            std::ignore =
                std::move(http_client.send_request_vote(test_node_id, rv, rpc_timeout)).get(),
            kythira::http_client_error);
        BOOST_CHECK_THROW(
            std::ignore =
                std::move(beast_client.send_request_vote(test_node_id, rv, rpc_timeout)).get(),
            kythira::http_client_error);
        BOOST_CHECK_THROW(std::ignore = std::move(proxygen_client_inst.send_request_vote(
                                                      test_node_id, rv, rpc_timeout))
                                            .get(),
                          kythira::http_client_error);
    }
    empty_peer.stop();
    empty_thread.join();

    work_guard.reset();
    ioc.stop();
    io_thread.join();
}

BOOST_AUTO_TEST_SUITE_END()
