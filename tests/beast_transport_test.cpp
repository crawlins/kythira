#define BOOST_TEST_MODULE beast_transport_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/http_transport.hpp>
#include <raft/json_serializer.hpp>
#include <raft/executor_default.hpp>
#include <raft/network.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint16_t test_bind_port = 18099;
constexpr std::uint16_t test_tls_bind_port = 18100;
constexpr std::uint16_t test_multi_bind_port_base = 18110;
constexpr const char* test_server_url = "http://127.0.0.1:18099";
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
// (not a mocked or placeholder PEM) so the TLS test below exercises a
// genuine handshake through boost_beast_client/server's actual TLS code
// path (SNI, boost::asio::ssl::context construction, ssl_stream), not just
// config-field validation.
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

static_assert(kythira::future_default_transport_types<test_transport_types>);
static_assert(kythira::network_client<kythira::boost_beast_client<test_transport_types>>);
static_assert(kythira::network_server<kythira::boost_beast_server<test_transport_types>>);

// Requirement 19.4: the narrower concept boost_beast_client/server actually
// require must reject a Types bundle that merely satisfies transport_types
// but doesn't pin future_template to kythira::future_default -- confirming
// this is a real, checked constraint, not just documentation.
using http_transport_types_for_beast_test =
    kythira::http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                  kythira::executor_default>;
static_assert(kythira::transport_types<http_transport_types_for_beast_test>);
static_assert(!kythira::future_default_transport_types<http_transport_types_for_beast_test>);

BOOST_AUTO_TEST_SUITE(beast_transport_tests)

BOOST_AUTO_TEST_CASE(request_vote_round_trip_and_connection_reuse) {
    boost::asio::io_context ioc;
    // io_context::run() returns as soon as there's no outstanding work, and
    // there is none yet at this point (the server/client below haven't been
    // constructed). Without a work guard keeping it artificially "busy",
    // these threads would return (and exit) immediately, and every
    // subsequent async_accept/async_connect this test posts would sit
    // forever with nothing driving it -- Requirement 8.2's "no progress
    // without a caller-run io_context::run() thread" applies to the whole
    // lifetime a caller wants the pool available, not just to moments when
    // work already happens to be queued.
    auto work_guard = boost::asio::make_work_guard(ioc);
    std::vector<std::thread> io_threads;
    for (int i = 0; i < 2; ++i) {
        io_threads.emplace_back([&ioc] { ioc.run(); });
    }

    kythira::boost_beast_server<test_transport_types> server(ioc, test_bind_address, test_bind_port,
                                                             {}, kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());

    std::unordered_map<std::uint64_t, std::string> node_map{{test_node_id, test_server_url}};
    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, {},
                                                             kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 42;
    auto resp =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(2000)))
            .get();
    BOOST_TEST(resp.term() == 42);
    BOOST_TEST(resp.vote_granted());

    // Second RPC on the same connection -- exercises pooling/reuse (Property 5).
    auto resp2 =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(2000)))
            .get();
    BOOST_TEST(resp2.vote_granted());

    server.stop();
    work_guard.reset();
    ioc.stop();
    for (auto& t : io_threads) {
        t.join();
    }
}

// Regression test for a real deadlock found during development: a session
// sitting idle on a keep-alive connection (no request in flight, just
// waiting for the next one that never arrives) must not prevent stop() from
// returning (Property 8's active-close behavior, design.md).
BOOST_AUTO_TEST_CASE(server_stop_drains_idle_keep_alive_connection) {
    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    std::vector<std::thread> io_threads;
    for (int i = 0; i < 2; ++i) {
        io_threads.emplace_back([&ioc] { ioc.run(); });
    }

    auto port = static_cast<std::uint16_t>(test_bind_port + 1);
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
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(2000)))
            .get();
    BOOST_TEST(resp.vote_granted());

    std::mutex stop_mutex;
    std::condition_variable stop_cv;
    bool stop_done = false;
    std::thread stop_thread([&] {
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
    stop_thread.join();

    work_guard.reset();
    ioc.stop();
    for (auto& t : io_threads) {
        t.join();
    }
}

// Property 6: concurrent RPCs to different target nodes run genuinely
// concurrently (different connections, different strands), and each gets
// the right response back -- guards against a request/response mixup if
// the connection pool or per-node routing were broken.
BOOST_AUTO_TEST_CASE(concurrent_rpcs_to_multiple_nodes) {
    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    std::vector<std::thread> io_threads;
    for (int i = 0; i < 4; ++i) {
        io_threads.emplace_back([&ioc] { ioc.run(); });
    }

    constexpr int node_count = 4;
    std::vector<std::unique_ptr<kythira::boost_beast_server<test_transport_types>>> servers;
    std::unordered_map<std::uint64_t, std::string> node_map;
    for (int i = 0; i < node_count; ++i) {
        auto port = static_cast<std::uint16_t>(test_multi_bind_port_base + i);
        auto server = std::make_unique<kythira::boost_beast_server<test_transport_types>>(
            ioc, test_bind_address, port, kythira::boost_beast_server_config{},
            kythira::noop_metrics{});
        register_echo_handlers(*server);
        server->start();
        node_map[static_cast<std::uint64_t>(i + 1)] =
            std::string("http://127.0.0.1:") + std::to_string(port);
        servers.push_back(std::move(server));
    }

    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, {},
                                                             kythira::noop_metrics{});

    std::vector<kythira::request_vote_response<>> responses(static_cast<std::size_t>(node_count));
    std::vector<std::thread> rpc_threads;
    for (int i = 0; i < node_count; ++i) {
        kythira::request_vote_request<> req{};
        req._term = static_cast<std::uint64_t>(100 + i);
        rpc_threads.emplace_back([&client, &responses, i, req] {
            responses[static_cast<std::size_t>(i)] =
                std::move(client.send_request_vote(static_cast<std::uint64_t>(i + 1), req,
                                                   std::chrono::milliseconds(3000)))
                    .get();
        });
    }
    for (auto& t : rpc_threads) {
        t.join();
    }

    for (int i = 0; i < node_count; ++i) {
        const auto& resp = responses[static_cast<std::size_t>(i)];
        BOOST_TEST(resp.term() == static_cast<std::uint64_t>(100 + i));
        BOOST_TEST(resp.vote_granted());
    }

    for (auto& server : servers) {
        server->stop();
    }
    work_guard.reset();
    ioc.stop();
    for (auto& t : io_threads) {
        t.join();
    }
}

BOOST_AUTO_TEST_CASE(tls_request_vote_round_trip) {
    temp_tls_material tls;

    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    std::vector<std::thread> io_threads;
    for (int i = 0; i < 2; ++i) {
        io_threads.emplace_back([&ioc] { ioc.run(); });
    }

    kythira::boost_beast_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = tls.cert_path.string();
    server_config.ssl_key_path = tls.key_path.string();
    kythira::boost_beast_server<test_transport_types> server(
        ioc, test_bind_address, test_tls_bind_port, server_config, kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());

    // enable_ssl_verification=false: this test's self-signed cert has no CA
    // to chain to, and exercising the encrypted-transport code path (SNI,
    // handshake, ssl_stream reads/writes) is the point here, not peer
    // certificate validation, which is exercised at the config-validation
    // level elsewhere.
    kythira::boost_beast_client_config client_config;
    client_config.enable_ssl_verification = false;
    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("https://127.0.0.1:") + std::to_string(test_tls_bind_port)}};
    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, client_config,
                                                             kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 99;
    auto resp =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(3000)))
            .get();
    BOOST_TEST(resp.term() == 99);
    BOOST_TEST(resp.vote_granted());

    server.stop();
    work_guard.reset();
    ioc.stop();
    for (auto& t : io_threads) {
        t.join();
    }
}

// Requirement 7.1-7.3: reload_tls_material() validates new material
// all-or-nothing before swapping in a fresh net::ssl::context; a server that
// isn't configured for TLS at all should reject the call outright rather
// than silently no-op.
BOOST_AUTO_TEST_CASE(server_reload_tls_material) {
    boost::asio::io_context ioc;

    kythira::boost_beast_server<test_transport_types> plain_server(
        ioc, test_bind_address, static_cast<std::uint16_t>(test_bind_port + 2), {},
        kythira::noop_metrics{});
    BOOST_CHECK_THROW(plain_server.reload_tls_material(), std::exception);

    temp_tls_material tls;
    kythira::boost_beast_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = tls.cert_path.string();
    server_config.ssl_key_path = tls.key_path.string();
    kythira::boost_beast_server<test_transport_types> tls_server(
        ioc, test_bind_address, static_cast<std::uint16_t>(test_bind_port + 3), server_config,
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

// Requirement 7.1: the client side of the same all-or-nothing reload
// contract, applied to client_cert_path/client_key_path/ca_cert_path.
BOOST_AUTO_TEST_CASE(client_reload_tls_material) {
    boost::asio::io_context ioc;
    std::unordered_map<std::uint64_t, std::string> node_map{{test_node_id, test_server_url}};

    temp_tls_material tls;
    kythira::boost_beast_client_config client_config;
    client_config.client_cert_path = tls.cert_path.string();
    client_config.client_key_path = tls.key_path.string();
    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, client_config,
                                                             kythira::noop_metrics{});
    BOOST_CHECK_NO_THROW(client.reload_tls_material());

    // As with the server case: remove the on-disk key *after* successful
    // construction, so this exercises reload()'s own re-validation rather
    // than the constructor's.
    std::filesystem::remove(tls.key_path);
    BOOST_CHECK_THROW(client.reload_tls_material(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()
