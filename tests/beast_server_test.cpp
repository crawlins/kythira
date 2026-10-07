// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE beast_server_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/executor_default.hpp>
#include <raft/network.hpp>

#include "beast_test_thread_pool.hpp"
#include "http_limit_test_helpers.hpp"

#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

// Server-focused slice of the one-file-per-concern split of what used to be
// tests/beast_transport_test.cpp -- one-to-one with tests/http_server_*,
// covering boost_beast_server's own lifecycle/resilience/limits (start/
// stop/drain, malformed-request handling, TLS material reload) rather than
// full client+server round trips, which live in beast_integration_test.cpp.
// Each test picks its own port from a disjoint range (this file is its own
// ctest binary now, and may run concurrently with the other beast_* test
// binaries under `ctest -j`).
namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint16_t test_bind_port_base = 18200;
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

// Generates a real, valid self-signed cert/key pair via the `openssl` CLI
// (not a mocked or placeholder PEM) so server_reload_tls_material exercises
// boost_beast_server's actual certificate-validation code path.
struct temp_tls_material {
    std::filesystem::path cert_path;
    std::filesystem::path key_path;

    temp_tls_material() {
        auto dir = std::filesystem::temp_directory_path();
        auto unique = std::to_string(std::random_device{}());
        cert_path = dir / ("beast_test_cert_" + unique + ".pem");
        key_path = dir / ("beast_test_key_" + unique + ".pem");
        std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key_path.string() +
                          " -out " + cert_path.string() +
                          " -days 1 -nodes -subj \"/CN=127.0.0.1\" -addext "
                          "\"subjectAltName=IP:127.0.0.1\" 2>/dev/null";
        int rc = std::system(cmd.c_str());
        BOOST_REQUIRE_MESSAGE(rc == 0,
                              "openssl CLI must be available to generate test TLS material");
        BOOST_REQUIRE(std::filesystem::exists(cert_path));
        BOOST_REQUIRE(std::filesystem::exists(key_path));
    }

    ~temp_tls_material() {
        std::error_code ec;
        std::filesystem::remove(cert_path, ec);
        std::filesystem::remove(key_path, ec);
    }
};

}  // namespace

// **Feature: boost-beast-http-transport, Property 10: Concept Compliance**
// **Validates: Requirement 16.1**
static_assert(kythira::network_server<kythira::boost_beast_server<test_transport_types>>);

BOOST_AUTO_TEST_SUITE(beast_server_tests)

// **Feature: boost-beast-http-transport, Property 8: Server Drain on Stop**
// **Validates: Requirement 5.2**
// Regression test for a real deadlock found during development: a session
// sitting idle on a keep-alive connection (no request in flight, just
// waiting for the next one that never arrives) must not prevent stop() from
// returning (Property 8's active-close behavior, design.md).
BOOST_AUTO_TEST_CASE(server_stop_drains_idle_keep_alive_connection) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    auto port = static_cast<std::uint16_t>(test_bind_port_base + 0);
    kythira::boost_beast_server<test_transport_types> server(ioc, test_bind_address, port, {},
                                                             kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, {},
                                                             kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 7;
    // Leaves the connection open (HTTP/1.1 keep-alive is the default), then
    // never sends another request -- the server session this creates is
    // exactly the "idle, nothing in flight" case that used to deadlock stop().
    auto resp =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(30000)))
            .get();
    BOOST_TEST(resp.vote_granted());

    std::mutex stop_mutex;
    std::condition_variable stop_cv;
    bool stop_done = false;
    kythira::testing::joining_thread stop_thread([&] {
        server.stop();
        {
            std::lock_guard<std::mutex> lock(stop_mutex);
            stop_done = true;
        }
        stop_cv.notify_one();
    });

    bool stop_completed = false;
    {
        std::unique_lock<std::mutex> lock(stop_mutex);
        stop_completed = stop_cv.wait_for(lock, std::chrono::seconds(5), [&] { return stop_done; });
    }
    BOOST_TEST(stop_completed);
}

// Requirement 4 / Task 13: a request body larger than
// boost_beast_server_config::max_request_body_size must be rejected rather
// than silently accepted by dispatch() -- a real, previously-unenforced gap
// (async_read_kf used to read into a bare message with no configurable
// limit; server_session::read_loop now reads into a
// beast_http::request_parser with .body_limit() set instead, so the config
// field actually does something). The exact exception surfaced isn't
// asserted precisely: the server responds 413 as soon as it sees a
// Content-Length exceeding the limit, but since the request body here isn't
// drained before the connection subsequently closes, whether the client's
// own write of that (still-oversized) body completes cleanly before the
// close race is not guaranteed by TCP -- what *is* guaranteed, and what this
// test actually checks, is that the RPC never succeeds.
BOOST_AUTO_TEST_CASE(server_rejects_oversized_request_body) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    auto port = static_cast<std::uint16_t>(test_bind_port_base + 1);
    kythira::boost_beast_server_config server_config;
    server_config.max_request_body_size = 64;
    kythira::boost_beast_server<test_transport_types> server(
        ioc, test_bind_address, port, server_config, kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, {},
                                                             kythira::noop_metrics{});

    kythira::install_snapshot_request<> req{};
    req._term = 1;
    req._leader_id = 1;
    req._last_included_index = 1;
    req._last_included_term = 1;
    req._offset = 0;
    req._data = std::vector<std::byte>(500, std::byte{'x'});
    req._done = true;

    BOOST_CHECK_THROW(
        std::move(client.send_install_snapshot(test_node_id, req, std::chrono::milliseconds(30000)))
            .get(),
        std::exception);

    server.stop();
}

// Error Handling: Server Accept-Loop Resilience -- a connection that sends a
// truncated request (a declared Content-Length the peer never finishes
// sending, then disconnects) must not crash or wedge do_accept(); a
// well-formed RPC against the same server afterward must still succeed.
BOOST_AUTO_TEST_CASE(server_survives_truncated_request) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    auto port = static_cast<std::uint16_t>(test_bind_port_base + 2);
    kythira::boost_beast_server<test_transport_types> server(ioc, test_bind_address, port, {},
                                                             kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();

    {
        boost::asio::ip::tcp::socket raw_socket(ioc);
        boost::asio::ip::tcp::endpoint ep(boost::asio::ip::make_address(test_bind_address), port);
        raw_socket.connect(ep);
        std::string truncated_request =
            "POST /v1/raft/request_vote HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 500\r\n"
            "\r\n"
            "{\"incomplete";
        boost::asio::write(raw_socket, boost::asio::buffer(truncated_request));
        boost::system::error_code ec;
        (void)raw_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
        (void)raw_socket.close(ec);
    }

    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, {},
                                                             kythira::noop_metrics{});
    kythira::request_vote_request<> req{};
    req._term = 88;
    auto resp =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(30000)))
            .get();
    BOOST_TEST(resp.term() == 88);
    BOOST_TEST(resp.vote_granted());

    server.stop();
}

// **Feature: boost-beast-http-transport, Property 7: TLS Material Reload Atomicity**
// **Validates: Requirement 7.3**
// Requirement 7.1-7.3: reload_tls_material() validates new material
// all-or-nothing before swapping in a fresh net::ssl::context; a server that
// isn't configured for TLS at all should reject the call outright rather
// than silently no-op.
BOOST_AUTO_TEST_CASE(server_reload_tls_material) {
    boost::asio::io_context ioc;

    kythira::boost_beast_server<test_transport_types> plain_server(
        ioc, test_bind_address, static_cast<std::uint16_t>(test_bind_port_base + 3), {},
        kythira::noop_metrics{});
    BOOST_CHECK_THROW(plain_server.reload_tls_material(), std::exception);

    temp_tls_material tls;
    kythira::boost_beast_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = tls.cert_path.string();
    server_config.ssl_key_path = tls.key_path.string();
    kythira::boost_beast_server<test_transport_types> tls_server(
        ioc, test_bind_address, static_cast<std::uint16_t>(test_bind_port_base + 4), server_config,
        kythira::noop_metrics{});
    BOOST_CHECK_NO_THROW(tls_server.reload_tls_material());

    // Construct successfully with valid material, then remove the cert file
    // from disk before reloading -- reload_tls_material() re-validates
    // whatever is *currently* on disk at the configured path, so this (not
    // constructing with an already-bad path, which the constructor's own
    // validation would reject before reload() is ever reached) is what
    // actually exercises reload()'s own validation failure path.
    std::filesystem::remove(tls.cert_path);
    BOOST_CHECK_THROW(tls_server.reload_tls_material(), std::exception);
}

namespace {

auto ipv6_loopback_available() -> bool {
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    ::close(fd);
    return ok;
}

auto vote_via(boost::asio::io_context& ioc, const std::string& url) -> bool {
    std::unordered_map<std::uint64_t, std::string> node_map{{test_node_id, url}};
    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, {},
                                                             kythira::noop_metrics{});
    kythira::request_vote_request<> req{};
    req._term = 3;
    return std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(5000)))
        .get()
        .vote_granted();
}

}  // namespace

// A host name binds every address /etc/hosts lists it under, and the client
// tries every address a peer name resolves to, so a server and peer both
// named "localhost" meet whichever of 127.0.0.1 and ::1 each side lists
// first.
BOOST_AUTO_TEST_CASE(server_localhost_bind_round_trip) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);
    auto port = static_cast<std::uint16_t>(test_bind_port_base + 5);
    kythira::boost_beast_server<test_transport_types> server(ioc, "localhost", port, {},
                                                             kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();
    BOOST_TEST(vote_via(ioc, "http://localhost:" + std::to_string(port)));
    BOOST_TEST(vote_via(ioc, "http://127.0.0.1:" + std::to_string(port)));
    server.stop();
}

// "*" listens on both wildcards with separate sockets, so IPv4 and IPv6
// clients both reach it whatever net.ipv6.bindv6only says.
BOOST_AUTO_TEST_CASE(server_star_bind_serves_both_families) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);
    auto port = static_cast<std::uint16_t>(test_bind_port_base + 6);
    kythira::boost_beast_server<test_transport_types> server(ioc, "*", port, {},
                                                             kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();
    BOOST_TEST(vote_via(ioc, "http://127.0.0.1:" + std::to_string(port)));
    if (ipv6_loopback_available()) {
        BOOST_TEST(vote_via(ioc, "http://[::1]:" + std::to_string(port)));
    } else {
        BOOST_TEST_MESSAGE("no IPv6 loopback on this host; [::1] leg skipped");
    }
    server.stop();
}

// A bind name that /etc/hosts doesn't list is refused at start(), not
// looked up in DNS.
BOOST_AUTO_TEST_CASE(server_refuses_unlisted_bind_name) {
    boost::asio::io_context ioc;
    kythira::boost_beast_server<test_transport_types> server(
        ioc, "kythira-test.invalid", static_cast<std::uint16_t>(test_bind_port_base + 7), {},
        kythira::noop_metrics{});
    BOOST_CHECK_THROW(server.start(), std::invalid_argument);
    BOOST_TEST(!server.is_running());
}

// ── Request and connection limits (.kiro/specs/http-server-request-limits/) ──
// Own port range, clear of every other HTTP test binary's.
namespace {
constexpr std::uint16_t limits_port_base = 18330;
namespace limits = kythira::testing::http_limits;
}  // namespace

// Requirement 6.1: the 413 itself, read off the wire. The older
// server_rejects_oversized_request_body above only checks the RPC failed.
BOOST_AUTO_TEST_CASE(server_answers_oversized_body_with_413_on_the_wire) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);
    auto port = static_cast<std::uint16_t>(limits_port_base + 0);
    kythira::boost_beast_server_config config;
    config.max_request_body_size = 64;
    kythira::boost_beast_server<test_transport_types> server(ioc, test_bind_address, port, config,
                                                             kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();

    limits::raw_connection conn(test_bind_address, port);
    BOOST_REQUIRE(conn.connected());
    conn.send_all(limits::sized_request(65));
    std::string response;
    auto outcome = conn.read_until_close(response, std::chrono::seconds(10));
    BOOST_TEST((outcome == limits::raw_connection::read_outcome::closed));
    BOOST_TEST(limits::status_of(response) == 413);
    BOOST_TEST(limits::header_of(response, "Content-Type") == "text/plain");

    // Exactly at the limit is a request, not a refusal: it reaches dispatch
    // (which rejects the "xxx" body as undecodable, but not with 413).
    limits::raw_connection at_limit(test_bind_address, port);
    BOOST_REQUIRE(at_limit.connected());
    at_limit.send_all(limits::sized_request(64));
    auto headers = at_limit.read_headers(std::chrono::seconds(10));
    BOOST_TEST(limits::status_of(headers) != 0);
    BOOST_TEST(limits::status_of(headers) != 413);

    server.stop();
}

// Requirement 2.5.
BOOST_AUTO_TEST_CASE(server_rejects_zero_connection_limit) {
    boost::asio::io_context ioc;
    kythira::boost_beast_server_config config;
    config.max_concurrent_connections = 0;
    using server_type = kythira::boost_beast_server<test_transport_types>;
    BOOST_CHECK_THROW(
        server_type(ioc, test_bind_address, limits_port_base + 1, config, kythira::noop_metrics{}),
        std::invalid_argument);
}

// Requirement 6.3: hold two idle connections, see a third closed without a
// response, close one of the two, then complete an RPC on a new connection.
BOOST_AUTO_TEST_CASE(server_refuses_connections_past_the_limit) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);
    auto port = static_cast<std::uint16_t>(limits_port_base + 2);
    kythira::boost_beast_server_config config;
    config.max_concurrent_connections = 2;
    // Far beyond the test's own length, so the idle pair is not reaped by
    // the read timeout while the test still needs it.
    config.request_timeout = std::chrono::seconds(300);
    kythira::boost_beast_server<test_transport_types> server(ioc, test_bind_address, port, config,
                                                             kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();

    limits::raw_connection first(test_bind_address, port);
    limits::raw_connection second(test_bind_address, port);
    BOOST_REQUIRE(first.connected());
    BOOST_REQUIRE(second.connected());
    BOOST_REQUIRE(
        limits::wait_for([&] { return server.live_connections() == 2; }, std::chrono::seconds(10)));

    limits::raw_connection third(test_bind_address, port);
    // The kernel completes the handshake before the server accepts and resets
    // it, so connect() normally succeeds; it fails with ECONNRESET instead when
    // the reset lands before this thread is scheduled again.
    BOOST_REQUIRE(third.reached_server());
    std::string unexpected;
    auto outcome = third.read_until_close(unexpected, std::chrono::seconds(10));
    BOOST_TEST((outcome == limits::raw_connection::read_outcome::closed));
    BOOST_TEST(unexpected.empty());
    BOOST_TEST(server.live_connections() == 2);

    first.close();
    BOOST_REQUIRE(
        limits::wait_for([&] { return server.live_connections() == 1; }, std::chrono::seconds(10)));

    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, {},
                                                             kythira::noop_metrics{});
    kythira::request_vote_request<> req{};
    req._term = 5;
    auto resp =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(10000)))
            .get();
    BOOST_TEST(resp.vote_granted());

    server.stop();
}

// Requirement 6.4: the 0.0.0.0 and :: listeners of a "*" bind draw on one
// limit.
BOOST_AUTO_TEST_CASE(server_connection_limit_is_shared_across_listeners) {
    if (!ipv6_loopback_available()) {
        BOOST_TEST_MESSAGE("no IPv6 loopback on this host; shared-limit test skipped");
        return;
    }
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);
    auto port = static_cast<std::uint16_t>(limits_port_base + 3);
    kythira::boost_beast_server_config config;
    config.max_concurrent_connections = 2;
    config.request_timeout = std::chrono::seconds(300);
    kythira::boost_beast_server<test_transport_types> server(ioc, "*", port, config,
                                                             kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();

    limits::raw_connection v4("127.0.0.1", port);
    limits::raw_connection v6("::1", port);
    BOOST_REQUIRE(v4.connected());
    BOOST_REQUIRE(v6.connected());
    BOOST_REQUIRE(
        limits::wait_for([&] { return server.live_connections() == 2; }, std::chrono::seconds(10)));

    for (const char* address : {"127.0.0.1", "::1"}) {
        BOOST_TEST_INFO("third connection on " << address);
        limits::raw_connection extra(address, port);
        BOOST_REQUIRE(extra.reached_server());  // see the case above
        std::string unexpected;
        auto outcome = extra.read_until_close(unexpected, std::chrono::seconds(10));
        BOOST_TEST((outcome == limits::raw_connection::read_outcome::closed));
        BOOST_TEST(unexpected.empty());
    }

    server.stop();
}

BOOST_AUTO_TEST_CASE(server_rejects_non_positive_handshake_timeout) {
    boost::asio::io_context ioc;
    using server_type = kythira::boost_beast_server<test_transport_types>;
    for (auto timeout : {std::chrono::seconds(0), std::chrono::seconds(-1)}) {
        BOOST_TEST_INFO("handshake_timeout " << timeout.count() << "s");
        kythira::boost_beast_server_config config;
        config.handshake_timeout = timeout;
        BOOST_CHECK_THROW(server_type(ioc, test_bind_address, limits_port_base + 4, config,
                                      kythira::noop_metrics{}),
                          std::invalid_argument);
    }
}

// A client that connects to a TLS server and never sends a ClientHello is
// cut off after handshake_timeout, and its connection slot goes back to the
// gate, even though request_timeout is far longer.
BOOST_AUTO_TEST_CASE(server_drops_stalled_tls_handshake) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);
    auto port = static_cast<std::uint16_t>(limits_port_base + 5);
    temp_tls_material tls;
    kythira::boost_beast_server_config config;
    config.enable_ssl = true;
    config.ssl_cert_path = tls.cert_path.string();
    config.ssl_key_path = tls.key_path.string();
    config.max_concurrent_connections = 1;
    config.request_timeout = std::chrono::seconds(300);
    config.handshake_timeout = std::chrono::seconds(1);
    kythira::boost_beast_server<test_transport_types> server(ioc, test_bind_address, port, config,
                                                             kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();

    auto connected_at = std::chrono::steady_clock::now();
    limits::raw_connection stalled(test_bind_address, port);
    BOOST_REQUIRE(stalled.connected());
    BOOST_REQUIRE(
        limits::wait_for([&] { return server.live_connections() == 1; }, std::chrono::seconds(10)));

    std::string unexpected;
    auto outcome = stalled.read_until_close(unexpected, std::chrono::seconds(30));
    auto held_for = std::chrono::steady_clock::now() - connected_at;
    BOOST_TEST((outcome == limits::raw_connection::read_outcome::closed));
    BOOST_TEST(unexpected.empty());
    BOOST_TEST(held_for >= std::chrono::milliseconds(900));
    BOOST_REQUIRE(
        limits::wait_for([&] { return server.live_connections() == 0; }, std::chrono::seconds(10)));

    // The slot is free again: the next connection is admitted, not reset.
    limits::raw_connection next(test_bind_address, port);
    BOOST_REQUIRE(next.connected());
    BOOST_TEST(
        limits::wait_for([&] { return server.live_connections() == 1; }, std::chrono::seconds(10)));

    server.stop();
}

namespace {

auto process_cpu_time() -> std::chrono::microseconds {
    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
    auto to_us = [](const timeval& tv) {
        return std::chrono::seconds(tv.tv_sec) + std::chrono::microseconds(tv.tv_usec);
    };
    return to_us(usage.ru_utime) + to_us(usage.ru_stime);
}

// Uses up every descriptor the process may open, after lowering the soft
// RLIMIT_NOFILE so that takes a bounded number of dup()s; the destructor
// gives them all back and restores the limit.
class descriptor_exhaustion {
public:
    descriptor_exhaustion() {
        BOOST_REQUIRE(::getrlimit(RLIMIT_NOFILE, &_saved) == 0);
        rlimit lowered = _saved;
        lowered.rlim_cur = std::min<rlim_t>(_saved.rlim_cur, 4096);
        BOOST_REQUIRE(::setrlimit(RLIMIT_NOFILE, &lowered) == 0);
        while (true) {
            int fd = ::dup(STDERR_FILENO);
            if (fd < 0) {
                _exhausted = errno == EMFILE;
                break;
            }
            _fds.push_back(fd);
        }
    }
    descriptor_exhaustion(const descriptor_exhaustion&) = delete;
    auto operator=(const descriptor_exhaustion&) -> descriptor_exhaustion& = delete;
    ~descriptor_exhaustion() { release(); }

    [[nodiscard]] auto exhausted() const -> bool { return _exhausted; }

    auto release() -> void {
        for (int fd : _fds) {
            ::close(fd);
        }
        _fds.clear();
        (void)::setrlimit(RLIMIT_NOFILE, &_saved);
    }

private:
    rlimit _saved{};
    std::vector<int> _fds;
    bool _exhausted{false};
};

}  // namespace

// With no descriptors left, accept() keeps failing with EMFILE while the
// pending connection keeps the listener readable. The server must back off
// rather than retry at once in a loop that pins a core, and must pick the
// connection up once descriptors are free again.
BOOST_AUTO_TEST_CASE(server_backs_off_accept_when_out_of_descriptors) {
    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);
    auto port = static_cast<std::uint16_t>(limits_port_base + 6);
    kythira::boost_beast_server<test_transport_types> server(ioc, test_bind_address, port, {},
                                                             kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();

    // The client's socket must exist before the descriptors run out; only
    // connect() happens after, and that needs no new descriptor.
    int client = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(client >= 0);
    std::chrono::microseconds cpu_used{};
    {
        descriptor_exhaustion exhaustion;
        BOOST_REQUIRE(exhaustion.exhausted());
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        ::inet_pton(AF_INET, test_bind_address, &a.sin_addr);
        int rc = ::connect(client, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        // Let the first failed accepts happen, then measure a quiet second.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto before = process_cpu_time();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        cpu_used = process_cpu_time() - before;
        exhaustion.release();
        BOOST_REQUIRE(rc == 0);
    }
    BOOST_TEST_MESSAGE("CPU used while out of descriptors: " << cpu_used.count() << " us");
    // A spinning accept loop burns the whole second on one io thread.
    BOOST_TEST(cpu_used < std::chrono::milliseconds(500));

    // The back-off tops out at 1 s, so the pending connection is accepted
    // soon after descriptors come back.
    BOOST_TEST(
        limits::wait_for([&] { return server.live_connections() == 1; }, std::chrono::seconds(10)));
    ::close(client);
    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
