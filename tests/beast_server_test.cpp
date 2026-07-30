#define BOOST_TEST_MODULE beast_server_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/executor_default.hpp>
#include <raft/network.hpp>

#include "beast_test_thread_pool.hpp"

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

    bool stop_completed;
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
        raw_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
        raw_socket.close(ec);
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

BOOST_AUTO_TEST_SUITE_END()
