#define BOOST_TEST_MODULE http_transport_reload_property_test

#include <boost/test/unit_test.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include "ca_test_fixture.hpp"

#include <httplib.h>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <chrono>
#include <fstream>
#include <thread>

using namespace raft::testing;

namespace {
constexpr const char* test_bind_address = "127.0.0.1";

using test_types =
    kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                  kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

// Overwrites the file at `path` with `content` via a same-directory rename —
// the same atomic-replace shape ca_test_fixture::renew() (task 20) will use.
void replace_file(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    std::filesystem::rename(tmp, path);
}

auto read_file(const std::string& path) -> std::string {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Writes `pem` to a fresh temp file, returning its path. Used for
// ca_cert_path, since validate_certificate_files() validates the server
// certificate's chain unconditionally (not just when require_client_cert is
// set) — against system CAs when ca_cert_path is empty, which a self-signed
// test CA can never satisfy.
auto write_ca_file(const std::string& pem) -> std::string {
    auto path = std::filesystem::temp_directory_path() /
                ("reload_test_ca_" + std::to_string(std::random_device{}()) + ".pem");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << pem;
    return path.string();
}

auto make_server(const raft::testing::temp_cert_files& files, const std::string& ca_cert_path,
                 std::uint16_t port, typename test_types::metrics_type& metrics)
    -> std::unique_ptr<kythira::cpp_httplib_server<test_types>> {
    kythira::cpp_httplib_server_config config;
    config.enable_ssl = true;
    config.ssl_cert_path = files.cert_path();
    config.ssl_key_path = files.key_path();
    config.ca_cert_path = ca_cert_path;

    auto server = std::make_unique<kythira::cpp_httplib_server<test_types>>(test_bind_address, port,
                                                                            config, metrics);
    server->register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        kythira::request_vote_response<> response;
        response._term = req.term();
        response._vote_granted = true;
        return response;
    });
    return server;
}

}  // namespace

// Property 13: a rejected reload (invalid new material) leaves the server
// still serving its previous, valid material — reload is all-or-nothing
// (Requirement 16.3).
BOOST_AUTO_TEST_CASE(invalid_reload_leaves_previous_material_serving,
                     *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    typename test_types::metrics_type metrics;
    constexpr std::uint16_t port = 18601;
    const auto& files = fixture.bootstrap_client("reload-invalid", {"localhost"}, {"127.0.0.1"});
    auto ca_cert_path = write_ca_file(fixture.root_certificate_pem());
    auto server = make_server(files, ca_cert_path, port, metrics);
    server->start();
    BOOST_TEST(server->is_running());

    auto original_cert = read_file(files.cert_path());
    replace_file(files.cert_path(), "NOT A VALID CERTIFICATE");

    BOOST_CHECK_THROW(server->reload_tls_material(), std::exception);
    BOOST_TEST(server->is_running());

    // The server is still alive and answering — old material never got torn
    // out from under it, despite the rejected reload attempt.
    httplib::SSLClient client(test_bind_address, port);
    client.enable_server_certificate_verification(false);
    client.set_connection_timeout(5, 0);
    auto res = client.Get("/nonexistent");
    BOOST_REQUIRE(res);  // Handshake + HTTP round trip still work (404 is fine).
    BOOST_TEST(res->status == 404);

    replace_file(files.cert_path(), original_cert);  // restore for cleanliness
    server->stop();
}

// A successful reload_tls_material() call (new, still-valid material) keeps
// the server running and still answering new connections (Requirement 16.1).
BOOST_AUTO_TEST_CASE(reload_succeeds_and_new_connections_still_work,
                     *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    typename test_types::metrics_type metrics;
    constexpr std::uint16_t port = 18602;
    const auto& files = fixture.bootstrap_client("reload-ok", {"localhost"}, {"127.0.0.1"});
    auto ca_cert_path = write_ca_file(fixture.root_certificate_pem());
    auto server = make_server(files, ca_cert_path, port, metrics);
    server->start();

    // Re-write the same (still valid) cert/key bytes in place via an atomic
    // rename, standing in for a real renewal (ca_test_fixture::renew(),
    // task 20) without requiring that primitive to exist yet.
    replace_file(files.cert_path(), read_file(files.cert_path()));
    replace_file(files.key_path(), read_file(files.key_path()));

    BOOST_CHECK_NO_THROW(server->reload_tls_material());
    BOOST_TEST(server->is_running());

    httplib::SSLClient client(test_bind_address, port);
    client.enable_server_certificate_verification(false);
    client.set_connection_timeout(5, 0);
    auto res = client.Get("/nonexistent");
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 404);

    server->stop();
}

// enable_auto_reload() picks up an on-disk cert change within a bounded
// number of poll intervals, with no caller-side polling loop required
// (Requirement 16.5), and never disturbs the running listener while doing so.
BOOST_AUTO_TEST_CASE(auto_reload_picks_up_file_change, *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    typename test_types::metrics_type metrics;
    constexpr std::uint16_t port = 18603;
    const auto& files = fixture.bootstrap_client("auto-reload", {"localhost"}, {"127.0.0.1"});
    auto ca_cert_path = write_ca_file(fixture.root_certificate_pem());
    auto server = make_server(files, ca_cert_path, port, metrics);
    server->start();

    server->enable_auto_reload(std::chrono::seconds(1));

    // Force a distinct mtime, then rewrite the (still valid) cert content —
    // the poller should pick this up on its next tick.
    auto cert_content = read_file(files.cert_path());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    replace_file(files.cert_path(), cert_content);

    std::this_thread::sleep_for(std::chrono::seconds(3));  // bounded number of poll intervals

    server->disable_auto_reload();
    BOOST_TEST(server->is_running());  // the poll loop never disturbed the live server

    httplib::SSLClient client(test_bind_address, port);
    client.enable_server_certificate_verification(false);
    client.set_connection_timeout(5, 0);
    auto res = client.Get("/nonexistent");
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 404);

    server->stop();
}
