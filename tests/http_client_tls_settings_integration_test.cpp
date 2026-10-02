// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE http_client_tls_settings_integration_test

// cpp_httplib_client must apply cipher_suites, min_tls_version and
// max_tls_version to the SSL_CTX* its handshakes actually use. Until
// 2026-10-02 they were validated and then dropped, so the client always
// offered cpp-httplib's defaults (TLS 1.2-1.3, OpenSSL's default ciphers).
// Each case pins the server to one setting and shows that a real RPC succeeds
// when the client's setting overlaps it and fails when it does not; with the
// settings dropped, every "fails" case below would succeed.

#include <boost/test/unit_test.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include "ca_test_fixture.hpp"
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>

namespace {
using test_types =
    kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                  kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint64_t test_node_id = 1;
constexpr std::chrono::milliseconds rpc_timeout{2000};

// TLS 1.2 suites for both key types, so the cases hold whichever key
// algorithm ca_test_fixture issues. The two lists share no suite.
constexpr const char* server_only_ciphers =
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384";
constexpr const char* disjoint_client_ciphers =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256";

struct tls_material {
    raft::testing::ca_test_fixture fixture;
    std::string ca_cert_path;

    tls_material() {
        auto ca_temp = std::filesystem::temp_directory_path() /
                       ("test_ca_cert_" + std::to_string(std::random_device{}()));
        std::ofstream file(ca_temp);
        file << fixture.root_certificate_pem();
        file.close();
        ca_cert_path = ca_temp.string();
    }

    ~tls_material() {
        std::error_code ec;
        std::filesystem::remove(ca_cert_path, ec);
    }

    tls_material(const tls_material&) = delete;
    auto operator=(const tls_material&) -> tls_material& = delete;

    auto server() -> const raft::testing::temp_cert_files& {
        return fixture.bootstrap_client("server", {"localhost"}, {"127.0.0.1"},
                                        /*server_auth=*/true,
                                        /*client_auth=*/false);
    }
};

auto server_config_for(tls_material& material) -> kythira::cpp_httplib_server_config {
    const auto& files = material.server();
    kythira::cpp_httplib_server_config config;
    config.enable_ssl = true;
    config.ssl_cert_path = files.cert_path();
    config.ssl_key_path = files.key_path();
    // Construction validates the server's own chain; the fixture CA is not
    // in the system store. No client certificate is required.
    config.ca_cert_path = material.ca_cert_path;
    return config;
}

auto client_config_for(const tls_material& material) -> kythira::cpp_httplib_client_config {
    kythira::cpp_httplib_client_config config;
    config.ca_cert_path = material.ca_cert_path;
    config.enable_ssl_verification = true;
    config.connection_timeout = std::chrono::milliseconds{2000};
    config.request_timeout = rpc_timeout;
    return config;
}

// Starts a TLS server with server_config on port, sends one RequestVote from a
// cpp_httplib_client built with client_config, and reports whether the RPC
// completed.
auto request_vote_succeeds(std::uint16_t port,
                           const kythira::cpp_httplib_server_config& server_config,
                           const kythira::cpp_httplib_client_config& client_config) -> bool {
    typename test_types::metrics_type server_metrics;
    kythira::cpp_httplib_server<test_types> server(test_bind_address, port, server_config,
                                                   server_metrics);
    server.register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        kythira::request_vote_response<> response;
        response._term = req.term();
        response._vote_granted = true;
        return response;
    });
    server.start();
    BOOST_REQUIRE(server.is_running());

    std::unordered_map<std::uint64_t, std::string> node_urls;
    node_urls[test_node_id] = std::string("https://127.0.0.1:") + std::to_string(port);
    typename test_types::metrics_type client_metrics;
    kythira::cpp_httplib_client<test_types> client(std::move(node_urls), client_config,
                                                   client_metrics);

    kythira::request_vote_request<> request;
    request._term = 3;
    request._candidate_id = 2;

    bool succeeded = false;
    try {
        auto response = client.send_request_vote(test_node_id, request, rpc_timeout).get();
        succeeded = response.term() == 3 && response.vote_granted();
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("RPC failed: " << e.what());
    }
    server.stop();
    return succeeded;
}
}  // namespace

BOOST_AUTO_TEST_SUITE(http_client_tls_settings_integration_tests)

BOOST_AUTO_TEST_CASE(client_max_tls_version_is_applied, *boost::unit_test::timeout(60)) {
    tls_material material;
    auto server_config = server_config_for(material);
    server_config.min_tls_version = "TLSv1.3";
    server_config.max_tls_version = "TLSv1.3";

    auto client_config = client_config_for(material);
    BOOST_TEST(request_vote_succeeds(18561, server_config, client_config));

    // A TLS 1.2-only client cannot reach a TLS 1.3-only server.
    client_config.max_tls_version = "TLSv1.2";
    BOOST_TEST(!request_vote_succeeds(18562, server_config, client_config));
}

BOOST_AUTO_TEST_CASE(client_min_tls_version_is_applied, *boost::unit_test::timeout(60)) {
    tls_material material;
    auto server_config = server_config_for(material);
    server_config.min_tls_version = "TLSv1.2";
    server_config.max_tls_version = "TLSv1.2";

    auto client_config = client_config_for(material);
    BOOST_TEST(request_vote_succeeds(18563, server_config, client_config));

    // A TLS 1.3-only client cannot reach a TLS 1.2-only server.
    client_config.min_tls_version = "TLSv1.3";
    BOOST_TEST(!request_vote_succeeds(18564, server_config, client_config));
}

BOOST_AUTO_TEST_CASE(client_cipher_suites_are_applied, *boost::unit_test::timeout(60)) {
    tls_material material;
    // cipher_suites governs TLS 1.2 and below, so pin the handshake to 1.2.
    auto server_config = server_config_for(material);
    server_config.max_tls_version = "TLSv1.2";
    server_config.cipher_suites = server_only_ciphers;

    auto client_config = client_config_for(material);
    client_config.cipher_suites = server_only_ciphers;
    BOOST_TEST(request_vote_succeeds(18565, server_config, client_config));

    // No suite in common: the handshake must fail.
    client_config.cipher_suites = disjoint_client_ciphers;
    BOOST_TEST(!request_vote_succeeds(18566, server_config, client_config));
}

BOOST_AUTO_TEST_SUITE_END()
