// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE http_transport_reload_property_test

#include <boost/test/unit_test.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include "ca_test_fixture.hpp"
#include "recording_metrics.hpp"
#include "tls_reload_probe.hpp"

#include <httplib.h>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <chrono>
#include <fstream>
#include <thread>

using namespace raft::testing;
using kythira::testing::pem_file_fingerprint;
using kythira::testing::tls_reload_probe;

namespace {
constexpr const char* test_bind_address = "127.0.0.1";

using test_types =
    kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                  kythira::testing::recording_metrics,
                                  folly::CPUThreadPoolExecutor>;

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

    auto original_fingerprint = pem_file_fingerprint(files.cert_path());
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

    // ...and still presenting the certificate it had before the attempt.
    tls_reload_probe probe(tls_reload_probe::kind::tls, port);
    BOOST_TEST(probe.peer_fingerprint() == original_fingerprint);

    replace_file(files.cert_path(), original_cert);  // restore for cleanliness
    probe.close();
    server->stop();
}

// A half-finished rotation (new certificate on disk, old key still beside
// it) is rejected as a whole: the server keeps presenting the old pair rather
// than a certificate it can no longer sign for (Requirement 16.3).
BOOST_AUTO_TEST_CASE(mismatched_key_reload_keeps_previous_pair, *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    typename test_types::metrics_type metrics;
    constexpr std::uint16_t port = 18604;
    const auto& files = fixture.bootstrap_client("reload-mismatch", {"localhost"}, {"127.0.0.1"});
    auto ca_cert_path = write_ca_file(fixture.root_certificate_pem());
    auto server = make_server(files, ca_cert_path, port, metrics);
    server->start();

    auto original_fingerprint = pem_file_fingerprint(files.cert_path());
    auto original_key = read_file(files.key_path());
    (void)fixture.renew("reload-mismatch");
    replace_file(files.key_path(), original_key);
    BOOST_REQUIRE(pem_file_fingerprint(files.cert_path()) != original_fingerprint);

    BOOST_CHECK_THROW(server->reload_tls_material(), std::exception);

    tls_reload_probe probe(tls_reload_probe::kind::tls, port);
    BOOST_TEST(probe.peer_fingerprint() == original_fingerprint);
    BOOST_TEST(probe.http_get("/nonexistent") == 404);

    probe.close();
    server->stop();
}

// After rotating to a genuinely different certificate (a fresh issuance from
// the test CA, not the same bytes rewritten), new handshakes present the new
// certificate (Property 12, Requirement 16.2).
BOOST_AUTO_TEST_CASE(reload_presents_rotated_certificate_to_new_handshakes,
                     *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    typename test_types::metrics_type metrics;
    constexpr std::uint16_t port = 18602;
    const auto& files = fixture.bootstrap_client("reload-ok", {"localhost"}, {"127.0.0.1"});
    auto ca_cert_path = write_ca_file(fixture.root_certificate_pem());
    auto server = make_server(files, ca_cert_path, port, metrics);
    server->start();

    auto old_fingerprint = pem_file_fingerprint(files.cert_path());
    {
        tls_reload_probe before(tls_reload_probe::kind::tls, port);
        BOOST_TEST(before.peer_fingerprint() == old_fingerprint);
    }

    (void)fixture.renew("reload-ok");
    auto new_fingerprint = pem_file_fingerprint(files.cert_path());
    BOOST_REQUIRE(new_fingerprint != old_fingerprint);

    // Until reload_tls_material() runs, the rotated files on disk are not
    // what the server presents: the reload is what switches it.
    {
        tls_reload_probe not_yet(tls_reload_probe::kind::tls, port);
        BOOST_TEST(not_yet.peer_fingerprint() == old_fingerprint);
    }

    BOOST_CHECK_NO_THROW(server->reload_tls_material());
    BOOST_TEST(server->is_running());

    tls_reload_probe after(tls_reload_probe::kind::tls, port);
    BOOST_TEST(after.peer_fingerprint() == new_fingerprint);
    BOOST_TEST(after.http_get("/nonexistent") == 404);

    after.close();
    server->stop();
}

// A keep-alive connection established before the reload keeps working after
// it, on the same TLS session and still under the certificate it negotiated,
// while a connection opened afterwards gets the new one (Property 11,
// Requirements 16.1/16.4).
BOOST_AUTO_TEST_CASE(established_connection_survives_reload, *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    typename test_types::metrics_type metrics;
    constexpr std::uint16_t port = 18605;
    const auto& files = fixture.bootstrap_client("reload-held", {"localhost"}, {"127.0.0.1"});
    auto ca_cert_path = write_ca_file(fixture.root_certificate_pem());
    auto server = make_server(files, ca_cert_path, port, metrics);
    server->start();

    auto old_fingerprint = pem_file_fingerprint(files.cert_path());
    tls_reload_probe held(tls_reload_probe::kind::tls, port);
    BOOST_REQUIRE(held.http_get("/nonexistent") == 404);

    (void)fixture.renew("reload-held");
    auto new_fingerprint = pem_file_fingerprint(files.cert_path());
    BOOST_REQUIRE(new_fingerprint != old_fingerprint);
    BOOST_REQUIRE_NO_THROW(server->reload_tls_material());

    // Same socket, same SSL session: a second request goes through.
    BOOST_TEST(held.http_get("/nonexistent") == 404);
    BOOST_TEST(held.peer_fingerprint() == old_fingerprint);

    tls_reload_probe fresh(tls_reload_probe::kind::tls, port);
    BOOST_TEST(fresh.peer_fingerprint() == new_fingerprint);

    held.close();
    fresh.close();
    server->stop();
}

namespace {
// Polls until a fresh handshake on `port` presents `expected`, or the deadline
// passes. Returns the last fingerprint seen.
auto wait_for_presented(std::uint16_t port, const std::string& expected,
                        std::chrono::seconds deadline) -> std::string {
    auto until = std::chrono::steady_clock::now() + deadline;
    std::string seen;
    while (std::chrono::steady_clock::now() < until) {
        tls_reload_probe probe(tls_reload_probe::kind::tls, port);
        seen = probe.peer_fingerprint();
        if (seen == expected) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return seen;
}
}  // namespace

// enable_auto_reload() picks up a rotated certificate on disk within a
// bounded number of poll intervals, with no caller-side reload call
// (Requirement 16.5): the next handshake presents the new certificate.
BOOST_AUTO_TEST_CASE(auto_reload_picks_up_file_change, *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    typename test_types::metrics_type metrics;
    constexpr std::uint16_t port = 18603;
    const auto& files = fixture.bootstrap_client("auto-reload", {"localhost"}, {"127.0.0.1"});
    auto ca_cert_path = write_ca_file(fixture.root_certificate_pem());
    auto server = make_server(files, ca_cert_path, port, metrics);
    server->start();

    server->enable_auto_reload(std::chrono::seconds(1));

    // Let the poller record the initial mtime, then rotate under it.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    (void)fixture.renew("auto-reload");
    auto new_fingerprint = pem_file_fingerprint(files.cert_path());

    BOOST_TEST(wait_for_presented(port, new_fingerprint, std::chrono::seconds(10)) ==
               new_fingerprint);

    server->disable_auto_reload();
    BOOST_TEST(server->is_running());  // the poll loop never disturbed the live server
    BOOST_TEST(metrics.recorder()->count_named("http.server.tls_reload.failed") == 0U);

    server->stop();
}

// A failed automatic reload is reported through metrics rather than thrown or
// swallowed, and the server keeps serving its previous material
// (Requirement 16.7).
BOOST_AUTO_TEST_CASE(failed_auto_reload_is_reported_via_metrics, *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    typename test_types::metrics_type metrics;
    constexpr std::uint16_t port = 18606;
    const auto& files = fixture.bootstrap_client("auto-reload-bad", {"localhost"}, {"127.0.0.1"});
    auto ca_cert_path = write_ca_file(fixture.root_certificate_pem());
    auto server = make_server(files, ca_cert_path, port, metrics);
    server->start();

    auto original_fingerprint = pem_file_fingerprint(files.cert_path());
    auto original_cert = read_file(files.cert_path());
    server->enable_auto_reload(std::chrono::seconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    replace_file(files.cert_path(), "NOT A VALID CERTIFICATE");

    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (metrics.recorder()->count_named("http.server.tls_reload.failed") == 0 &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server->disable_auto_reload();

    BOOST_TEST(metrics.recorder()->count_named("http.server.tls_reload.failed") >= 1U);
    BOOST_TEST(server->is_running());
    tls_reload_probe probe(tls_reload_probe::kind::tls, port);
    BOOST_TEST(probe.peer_fingerprint() == original_fingerprint);

    replace_file(files.cert_path(), original_cert);
    probe.close();
    server->stop();
}
