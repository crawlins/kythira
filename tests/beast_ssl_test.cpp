#define BOOST_TEST_MODULE beast_ssl_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/executor_default.hpp>

#include "beast_test_thread_pool.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

// TLS-focused slice of the one-file-per-concern split of what used to be
// tests/beast_transport_test.cpp -- one-to-one with tests/http_ssl_*:
// genuine end-to-end TLS handshakes (server-only and mutual), using a real
// self-signed cert/key pair generated fresh per test run via the `openssl`
// CLI rather than a placeholder/mocked PEM. TLS material reload lives in
// beast_client_test.cpp/beast_server_test.cpp instead, since it doesn't
// need a live handshake to exercise. Each test picks its own port from a
// disjoint range (this file is its own ctest binary now, and may run
// concurrently with the other beast_* test binaries under `ctest -j`).
namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint16_t test_bind_port_base = 18230;
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
// (not a mocked or placeholder PEM) so the tests below exercise a genuine
// handshake through boost_beast_client/server's actual TLS code path (SNI,
// boost::asio::ssl::context construction, ssl_stream), not just
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

BOOST_AUTO_TEST_SUITE(beast_ssl_tests)

// Requirement 6: server-only TLS -- a real handshake, not just config
// validation.
BOOST_AUTO_TEST_CASE(tls_request_vote_round_trip) {
    temp_tls_material tls;

    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    auto port = static_cast<std::uint16_t>(test_bind_port_base + 0);
    kythira::boost_beast_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = tls.cert_path.string();
    server_config.ssl_key_path = tls.key_path.string();
    kythira::boost_beast_server<test_transport_types> server(
        ioc, test_bind_address, port, server_config, kythira::noop_metrics{});
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
        {test_node_id, std::string("https://127.0.0.1:") + std::to_string(port)}};
    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, client_config,
                                                             kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 99;
    auto resp =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(30000)))
            .get();
    BOOST_TEST(resp.term() == 99);
    BOOST_TEST(resp.vote_granted());

    server.stop();
}

// Requirement 6, mutual TLS (require_client_cert): a client presenting the
// server's trusted certificate completes the handshake; a client presenting
// none is rejected -- exercising verify_fail_if_no_peer_cert end-to-end
// (build_ssl_context() already sets this per Requirement 6). Reuses a
// single self-signed cert as server identity, trust anchor (ca_cert_path),
// and client identity alike, the same simplification
// tls_request_vote_round_trip already uses (a real 3-tier CA/server/client
// chain is not the point here, the require_client_cert enforcement path is).
BOOST_AUTO_TEST_CASE(mutual_tls_client_certificate_enforcement) {
    temp_tls_material tls;

    boost::asio::io_context ioc;
    kythira::testing::io_thread_pool io_threads(ioc, 2);

    auto port = static_cast<std::uint16_t>(test_bind_port_base + 1);
    kythira::boost_beast_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = tls.cert_path.string();
    server_config.ssl_key_path = tls.key_path.string();
    server_config.ca_cert_path = tls.cert_path.string();
    server_config.require_client_cert = true;
    kythira::boost_beast_server<test_transport_types> server(
        ioc, test_bind_address, port, server_config, kythira::noop_metrics{});
    register_echo_handlers(server);
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("https://127.0.0.1:") + std::to_string(port)}};

    // Positive case: client presents the trusted certificate.
    {
        kythira::boost_beast_client_config client_config;
        client_config.enable_ssl_verification = false;
        client_config.client_cert_path = tls.cert_path.string();
        client_config.client_key_path = tls.key_path.string();
        kythira::boost_beast_client<test_transport_types> client(ioc, node_map, client_config,
                                                                 kythira::noop_metrics{});
        kythira::request_vote_request<> req{};
        req._term = 55;
        auto resp =
            std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(30000)))
                .get();
        BOOST_TEST(resp.term() == 55);
    }

    // Negative case: client presents no certificate at all -- the server's
    // require_client_cert must reject the handshake.
    {
        kythira::boost_beast_client_config client_config;
        client_config.enable_ssl_verification = false;
        kythira::boost_beast_client<test_transport_types> client(ioc, node_map, client_config,
                                                                 kythira::noop_metrics{});
        kythira::request_vote_request<> req{};
        req._term = 56;
        BOOST_CHECK_THROW(
            std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(30000)))
                .get(),
            std::exception);
    }

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
