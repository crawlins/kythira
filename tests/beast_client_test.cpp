// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE beast_client_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/http_transport.hpp>
#include <raft/json_serializer.hpp>
#include <raft/executor_default.hpp>
#include <raft/network.hpp>

#include <cstdlib>
#include <filesystem>
#include <random>
#include <unordered_map>

// Client-focused slice of the one-file-per-concern split of what used to be
// tests/beast_transport_test.cpp -- one-to-one with tests/http_client_*,
// covering client-only concerns (construction, concept compliance, TLS
// reload) rather than full request/response round trips, which live in
// beast_integration_test.cpp instead (matching how http_client_test.cpp
// itself stays construction/config-level and defers round trips to
// http_integration_test.cpp).
namespace {
constexpr const char* test_server_url = "http://127.0.0.1:18099";
constexpr std::uint64_t test_node_id = 1;

using test_transport_types =
    kythira::future_default_http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                                 kythira::executor_default>;

// Generates a real, valid self-signed cert/key pair via the `openssl` CLI
// (not a mocked or placeholder PEM) so the reload test below exercises
// boost_beast_client's actual certificate-validation code path, not just a
// hardcoded fixture.
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
static_assert(kythira::future_default_transport_types<test_transport_types>);
static_assert(kythira::network_client<kythira::boost_beast_client<test_transport_types>>);

// Requirement 19.4: the narrower concept boost_beast_client actually
// requires must reject a Types bundle that merely satisfies transport_types
// but doesn't pin future_template to kythira::future_default -- confirming
// this is a real, checked constraint, not just documentation.
using http_transport_types_for_beast_test =
    kythira::http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                  kythira::executor_default>;
static_assert(kythira::transport_types<http_transport_types_for_beast_test>);
static_assert(!kythira::future_default_transport_types<http_transport_types_for_beast_test>);

BOOST_AUTO_TEST_SUITE(beast_client_tests)

// **Feature: boost-beast-http-transport, Property 7: TLS Material Reload Atomicity**
// **Validates: Requirement 7.3**
// Requirement 7.1: the client side of the all-or-nothing reload contract,
// applied to client_cert_path/client_key_path/ca_cert_path. No server is
// started here -- reload_tls_material() only re-validates on-disk material
// and swaps the client's own SSL context, neither of which needs a live
// peer to exercise.
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

    // Remove the on-disk key *after* successful construction, so this
    // exercises reload()'s own re-validation rather than the constructor's.
    std::filesystem::remove(tls.key_path);
    BOOST_CHECK_THROW(client.reload_tls_material(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()
