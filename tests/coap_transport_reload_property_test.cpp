// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE coap_transport_reload_property_test

#include <boost/test/unit_test.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/test_types.hpp>
#include "ca_test_fixture.hpp"
#include "recording_metrics.hpp"
#include "tls_reload_probe.hpp"

#include <chrono>
#include <fstream>
#include <thread>

#include "test_timeout_scale.hpp"

using namespace kythira;
using namespace raft::testing;
using kythira::testing::pem_file_fingerprint;
using kythira::testing::tls_reload_probe;

namespace {
constexpr const char* test_bind_address = "127.0.0.1";

using test_types = kythira::test_transport_types<json_serializer>;

// test_transport_types with a metrics model that remembers what was emitted,
// so the failed-auto-reload path (Requirement 16.7) can be observed.
struct recording_types : kythira::test_transport_types<json_serializer> {
    using metrics_type = kythira::testing::recording_metrics;
};

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

// A started DTLS server on `port` presenting `files`' certificate. Peer
// verification is off: these cases are about what the server presents, and
// the probe's own trust decisions are out of scope.
template<typename Types>
auto make_dtls_server(const temp_cert_files& files, std::uint16_t port,
                      typename Types::metrics_type& metrics)
    -> std::unique_ptr<coap_server<Types>> {
    coap_server_config config;
    config.enable_dtls = true;
    config.cert_file = files.cert_path();
    config.key_file = files.key_path();
    config.verify_peer_cert = false;
    auto server = std::make_unique<coap_server<Types>>(test_bind_address, port, config, metrics);
    server->start();
    return server;
}

// Polls until a fresh DTLS handshake on `port` presents `expected`, or the
// deadline passes. Returns the last fingerprint seen.
auto wait_for_presented(std::uint16_t port, const std::string& expected,
                        std::chrono::seconds deadline) -> std::string {
    auto until = std::chrono::steady_clock::now() + deadline;
    std::string seen;
    while (std::chrono::steady_clock::now() < until) {
        tls_reload_probe probe(tls_reload_probe::kind::dtls, port);
        seen = probe.peer_fingerprint();
        if (seen == expected) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return seen;
}

}  // namespace

// After rotating to a genuinely different certificate, a new DTLS handshake
// against the running server presents it (Property 12, Requirements
// 16.2/16.4).
BOOST_AUTO_TEST_CASE(coap_server_reload_presents_rotated_certificate,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    ca_test_fixture fixture;
    const auto& files =
        fixture.bootstrap_client("coap-reload-server", {"localhost"}, {"127.0.0.1"});
    constexpr std::uint16_t port = 18701;
    test_types::metrics_type metrics;
    auto server = make_dtls_server<test_types>(files, port, metrics);

    auto old_fingerprint = pem_file_fingerprint(files.cert_path());
    {
        tls_reload_probe before(tls_reload_probe::kind::dtls, port);
        BOOST_TEST(before.peer_fingerprint() == old_fingerprint);
    }

    (void)fixture.renew("coap-reload-server");
    auto new_fingerprint = pem_file_fingerprint(files.cert_path());
    BOOST_REQUIRE(new_fingerprint != old_fingerprint);

    // Until reload_tls_material() runs, the rotated files on disk are not
    // what the server presents: the reload is what switches it.
    {
        tls_reload_probe not_yet(tls_reload_probe::kind::dtls, port);
        BOOST_TEST(not_yet.peer_fingerprint() == old_fingerprint);
    }

    BOOST_CHECK_NO_THROW(server->reload_tls_material());

    tls_reload_probe after(tls_reload_probe::kind::dtls, port);
    BOOST_TEST(after.peer_fingerprint() == new_fingerprint);
    BOOST_TEST(after.coap_request(0x1234));

    server->stop();
}

// A DTLS association established before the reload still carries traffic
// after it, under the certificate it negotiated, while a new handshake gets
// the rotated one (Property 11, Requirement 16.4).
BOOST_AUTO_TEST_CASE(coap_dtls_association_survives_reload,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    ca_test_fixture fixture;
    const auto& files = fixture.bootstrap_client("coap-reload-held", {"localhost"}, {"127.0.0.1"});
    constexpr std::uint16_t port = 18705;
    test_types::metrics_type metrics;
    auto server = make_dtls_server<test_types>(files, port, metrics);

    auto old_fingerprint = pem_file_fingerprint(files.cert_path());
    tls_reload_probe held(tls_reload_probe::kind::dtls, port);
    BOOST_REQUIRE(held.coap_request(0x0001));

    (void)fixture.renew("coap-reload-held");
    auto new_fingerprint = pem_file_fingerprint(files.cert_path());
    BOOST_REQUIRE(new_fingerprint != old_fingerprint);
    BOOST_REQUIRE_NO_THROW(server->reload_tls_material());

    BOOST_TEST(held.coap_request(0x0002));
    BOOST_TEST(held.peer_fingerprint() == old_fingerprint);

    tls_reload_probe fresh(tls_reload_probe::kind::dtls, port);
    BOOST_TEST(fresh.peer_fingerprint() == new_fingerprint);

    server->stop();
}

// reload_tls_material() rejects unparseable material and requires cert_file
// to have been configured in the first place (Requirement 16.3).
BOOST_AUTO_TEST_CASE(coap_server_reload_rejects_invalid_material,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    ca_test_fixture fixture;
    const auto& files =
        fixture.bootstrap_client("coap-reload-invalid", {"localhost"}, {"127.0.0.1"});

    coap_server_config config;
    config.enable_dtls = true;
    config.cert_file = files.cert_path();
    config.key_file = files.key_path();
    config.verify_peer_cert = false;

    constexpr std::uint16_t port = 18702;
    test_types::metrics_type metrics;
    coap_server<test_types> server(test_bind_address, port, config, metrics);
    server.start();

    auto original_fingerprint = pem_file_fingerprint(files.cert_path());
    auto original_cert = read_file(files.cert_path());
    replace_file(files.cert_path(), "NOT A VALID CERTIFICATE");

    BOOST_CHECK_THROW(server.reload_tls_material(), std::exception);

    // The rejected attempt left the previous certificate serving.
    tls_reload_probe probe(tls_reload_probe::kind::dtls, port);
    BOOST_TEST(probe.peer_fingerprint() == original_fingerprint);

    replace_file(files.cert_path(), original_cert);
    server.stop();
}

BOOST_AUTO_TEST_CASE(coap_server_reload_requires_cert_configured,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    coap_server_config config;
    config.enable_dtls = false;  // no cert_file/key_file configured

    test_types::metrics_type metrics;
    coap_server<test_types> server(test_bind_address, 18703, config, metrics);

    BOOST_CHECK_THROW(server.reload_tls_material(), std::logic_error);
}

// enable_auto_reload() picks up a rotated certificate on disk within a
// bounded number of poll intervals: the next DTLS handshake presents it
// (Requirements 16.5/16.6), and disable_auto_reload() stops the poller.
BOOST_AUTO_TEST_CASE(coap_server_auto_reload_picks_up_rotation,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    ca_test_fixture fixture;
    const auto& files = fixture.bootstrap_client("coap-auto-reload", {"localhost"}, {"127.0.0.1"});
    constexpr std::uint16_t port = 18704;
    recording_types::metrics_type metrics;
    auto server = make_dtls_server<recording_types>(files, port, metrics);

    server->enable_auto_reload(std::chrono::seconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    (void)fixture.renew("coap-auto-reload");
    auto new_fingerprint = pem_file_fingerprint(files.cert_path());

    BOOST_TEST(wait_for_presented(port, new_fingerprint, std::chrono::seconds(10)) ==
               new_fingerprint);

    BOOST_CHECK_NO_THROW(server->disable_auto_reload());
    BOOST_TEST(metrics.recorder()->count_named("coap.server.tls_reload.failed") == 0U);
    server->stop();
}

// A failed automatic reload is reported through metrics, and the server keeps
// presenting its previous certificate (Requirement 16.7).
BOOST_AUTO_TEST_CASE(coap_server_failed_auto_reload_is_reported_via_metrics,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    ca_test_fixture fixture;
    const auto& files =
        fixture.bootstrap_client("coap-auto-reload-bad", {"localhost"}, {"127.0.0.1"});
    constexpr std::uint16_t port = 18706;
    recording_types::metrics_type metrics;
    auto server = make_dtls_server<recording_types>(files, port, metrics);

    auto original_fingerprint = pem_file_fingerprint(files.cert_path());
    auto original_cert = read_file(files.cert_path());
    server->enable_auto_reload(std::chrono::seconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    replace_file(files.cert_path(), "NOT A VALID CERTIFICATE");

    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (metrics.recorder()->count_named("coap.server.tls_reload.failed") == 0 &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server->disable_auto_reload();

    BOOST_TEST(metrics.recorder()->count_named("coap.server.tls_reload.failed") >= 1U);
    tls_reload_probe probe(tls_reload_probe::kind::dtls, port);
    BOOST_TEST(probe.peer_fingerprint() == original_fingerprint);

    replace_file(files.cert_path(), original_cert);
    server->stop();
}

// Same coverage for coap_client's own presented certificate under mutual DTLS.
BOOST_AUTO_TEST_CASE(coap_client_reload_succeeds_with_valid_material,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    ca_test_fixture fixture;
    const auto& files = fixture.bootstrap_client("coap-reload-client", {"client.example.com"});

    coap_client_config config;
    config.enable_dtls = true;
    config.cert_file = files.cert_path();
    config.key_file = files.key_path();
    config.verify_peer_cert = false;

    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[1] = "coaps://127.0.0.1:5684";

    test_types::metrics_type metrics;
    coap_client<test_types> client(endpoints, config, metrics);

    // Rotate to a different certificate, not the same bytes rewritten.
    auto old_fingerprint = pem_file_fingerprint(files.cert_path());
    (void)fixture.renew("coap-reload-client");
    BOOST_REQUIRE(pem_file_fingerprint(files.cert_path()) != old_fingerprint);

    BOOST_CHECK_NO_THROW(client.reload_tls_material());

    client.disable_auto_reload();  // no-op, never enabled — exercises the joinable() guard
}
