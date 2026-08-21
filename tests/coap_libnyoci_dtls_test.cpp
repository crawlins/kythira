// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_libnyoci_dtls_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT (180 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/json_serializer.hpp>
#include <raft/network.hpp>
#include <raft/serializer_registry.hpp>
// Deliberately NOT raft/coap_transport.hpp — see the header comment in
// coap_transport_libnyoci_impl.hpp for why the two backends cannot share a
// translation unit.
#include <raft/coap_transport_libnyoci_impl.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {
using test_serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;
using test_metrics = kythira::noop_metrics;

struct test_types {
    using serializer_type = test_serializer;
    using serializer_registry_type = kythira::single_serializer_registry<test_serializer>;
    using rpc_serializer_type = test_serializer;
    using metrics_type = test_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

using test_client = kythira::coap_libnyoci_client<test_types>;
using test_server = kythira::coap_libnyoci_server<test_types>;

constexpr std::uint64_t peer_node_id = 3;
constexpr const char* loopback = "127.0.0.1";
constexpr std::uint16_t ephemeral_port = 0;

[[nodiscard]] auto endpoint_for(std::uint16_t port) -> std::string {
    // Written as coap:// on purpose: a DTLS-configured client must rewrite the
    // scheme to coaps:// itself, because libnyoci picks the session type
    // straight out of the URI scheme. If it did not, this endpoint would go out
    // in the clear and the handshake-less server would never answer.
    return std::string{"coap://"} + loopback + ":" + std::to_string(port);
}

[[nodiscard]] auto psk_key() -> std::vector<std::byte> {
    std::vector<std::byte> key;
    for (int i = 1; i <= 16; ++i) {
        key.push_back(static_cast<std::byte>(i));
    }
    return key;
}

[[nodiscard]] auto psk_security(std::string identity) -> kythira::coap_security_config {
    kythira::coap_security_config config;
    config.mode = kythira::coap_auth_mode::dtls_psk;
    config.credentials = kythira::psk_credentials{std::move(identity), psk_key()};
    return config;
}

/// A self-signed cert/key pair on disk, mirroring the fixture
/// coap_security_provider_libcoap_test.cpp uses, so the PKI path is exercised
/// against real material rather than a stub.
///
/// **P-256, not RSA-2048, and that is a hard constraint rather than a
/// preference.** libnyoci reads every inbound datagram into a
/// `char packet[NYOCI_MAX_PACKET_LENGTH+1]` — 1033 bytes with the library's
/// default configuration (nyoci-plat-net.c). A DTLS handshake flight carrying
/// an RSA-2048 certificate exceeds that and is silently truncated, so the
/// handshake stalls and the request times out with no diagnostic. An ECDSA
/// P-256 certificate is a few hundred bytes and fits comfortably. Anyone
/// deploying DTLS-PKI over this backend needs small certificates, or a libnyoci
/// built with a larger NYOCI_MAX_CONTENT_LENGTH.
struct pki_material {
    std::string cert_file;
    std::string key_file;

    explicit pki_material(const std::string& suffix) {
        EVP_PKEY* pkey = EVP_EC_gen("P-256");
        BOOST_REQUIRE(pkey != nullptr);

        X509* cert = X509_new();
        BOOST_REQUIRE(cert != nullptr);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60L * 24L * 365L);
        X509_set_pubkey(cert, pkey);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("kythira-libnyoci"), -1,
                                   -1, 0);
        X509_set_issuer_name(cert, name);
        BOOST_REQUIRE(X509_sign(cert, pkey, EVP_sha256()) > 0);

        const auto tmp_dir = std::filesystem::temp_directory_path();
        cert_file = (tmp_dir / ("coap_libnyoci_dtls_cert_" + suffix + ".pem")).string();
        key_file = (tmp_dir / ("coap_libnyoci_dtls_key_" + suffix + ".pem")).string();

        FILE* cert_fp = std::fopen(cert_file.c_str(), "wb");
        BOOST_REQUIRE(cert_fp != nullptr);
        PEM_write_X509(cert_fp, cert);
        std::fclose(cert_fp);

        FILE* key_fp = std::fopen(key_file.c_str(), "wb");
        BOOST_REQUIRE(key_fp != nullptr);
        PEM_write_PrivateKey(key_fp, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        std::fclose(key_fp);

        X509_free(cert);
        EVP_PKEY_free(pkey);
    }

    pki_material(const pki_material&) = delete;
    auto operator=(const pki_material&) -> pki_material& = delete;

    ~pki_material() {
        std::error_code ignored;
        std::filesystem::remove(cert_file, ignored);
        std::filesystem::remove(key_file, ignored);
    }
};
}

BOOST_AUTO_TEST_SUITE(coap_libnyoci_dtls_tests)

#ifdef LIBNYOCI_AVAILABLE

// ── The two modes libnyoci's OpenSSL plugin can actually provide ───────────

// The headline claim: a Raft RPC over a real DTLS-PSK handshake on loopback.
// If the handshake did not happen, or the client failed to upgrade the
// endpoint's scheme to coaps://, this hangs until the transaction times out
// and the future rejects instead.
BOOST_AUTO_TEST_CASE(test_dtls_psk_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    kythira::coap_server_config server_config;
    server_config.security = psk_security("kythira-node");

    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.term(), request.candidate_id() == 11};
    });
    server.start();
    BOOST_TEST(server.is_running());
    // A DTLS listener cannot report an ephemeral port through libnyoci, so the
    // adapter reserves one up front; bound_port() must still be meaningful.
    BOOST_TEST(server.bound_port() != 0);

    kythira::coap_client_config client_config;
    client_config.security = psk_security("kythira-node");

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config, test_metrics{}};

    const kythira::request_vote_request<> request{5, 11, 2, 4};
    const auto response =
        client.send_request_vote(peer_node_id, request, std::chrono::seconds{30}).get();
    BOOST_TEST(response.term() == 5U);
    BOOST_TEST(response.vote_granted());

    server.stop();
}

// A client whose PSK identity the server does not recognise must fail the
// handshake, and that failure must surface as a rejected future rather than a
// silent fall back to plaintext.
BOOST_AUTO_TEST_CASE(test_dtls_psk_identity_mismatch_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    kythira::coap_server_config server_config;
    server_config.security = psk_security("the-right-identity");

    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    server.register_request_vote_handler([](const kythira::request_vote_request<>&) {
        return kythira::request_vote_response<>{1, true};
    });
    server.start();

    kythira::coap_client_config client_config;
    client_config.security = psk_security("the-wrong-identity");

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config, test_metrics{}};

    const kythira::request_vote_request<> request{1, 1, 0, 0};
    bool rejected = false;
    try {
        (void)client.send_request_vote(peer_node_id, request, std::chrono::seconds{5}).get();
    } catch (const kythira::coap_transport_error&) {
        rejected = true;
    }
    BOOST_TEST(rejected, "a failed DTLS handshake must reject, never fall back to plaintext");

    server.stop();
}

// DTLS-PKI over a self-signed cert. verify_peer_cert is left off on both ends:
// the point here is that the certificate and key are loaded into the SSL_CTX
// and a handshake completes, not that a full PKI is validated.
BOOST_AUTO_TEST_CASE(test_dtls_pki_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    const pki_material server_material{"server"};
    const pki_material client_material{"client"};

    kythira::pki_credentials server_creds;
    server_creds.cert_file = server_material.cert_file;
    server_creds.key_file = server_material.key_file;
    server_creds.verify_peer_cert = false;

    kythira::coap_server_config server_config;
    server_config.security.mode = kythira::coap_auth_mode::dtls_pki;
    server_config.security.credentials = server_creds;

    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    server.register_append_entries_handler([](const kythira::append_entries_request<>& request) {
        kythira::append_entries_response<> response{};
        response._term = request.term();
        response._success = true;
        return response;
    });
    server.start();

    kythira::pki_credentials client_creds;
    client_creds.cert_file = client_material.cert_file;
    client_creds.key_file = client_material.key_file;
    client_creds.verify_peer_cert = false;

    kythira::coap_client_config client_config;
    client_config.security.mode = kythira::coap_auth_mode::dtls_pki;
    client_config.security.credentials = client_creds;

    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config, test_metrics{}};

    kythira::append_entries_request<> request{};
    request._term = 21;
    request._leader_id = 1;

    const auto response =
        client.send_append_entries(peer_node_id, request, std::chrono::seconds{30}).get();
    BOOST_TEST(response.term() == 21U);
    BOOST_TEST(response.success());

    server.stop();
}

// Bad key material must fail at construction, before any socket exists —
// the same "all or nothing" contract the libcoap path has.
BOOST_AUTO_TEST_CASE(test_pki_with_missing_files_fails_at_start,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::pki_credentials creds;
    creds.cert_file = "/nonexistent/kythira-libnyoci.pem";
    creds.key_file = "/nonexistent/kythira-libnyoci.key";

    kythira::coap_server_config config;
    config.security.mode = kythira::coap_auth_mode::dtls_pki;
    config.security.credentials = creds;

    // Construction plans the channel; start() is where the material is loaded.
    test_server server{loopback, ephemeral_port, config, test_metrics{}};
    BOOST_CHECK_THROW(server.start(), kythira::coap_security_error);
    BOOST_TEST(!server.is_running());
}

// A mode that names credentials of the wrong alternative is a configuration
// error, and must say so rather than dereferencing the wrong variant member.
BOOST_AUTO_TEST_CASE(test_psk_mode_with_pki_credentials_is_a_config_error,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::coap_server_config config;
    config.security.mode = kythira::coap_auth_mode::dtls_psk;
    config.security.credentials = kythira::pki_credentials{};

    test_server server{loopback, ephemeral_port, config, test_metrics{}};
    BOOST_CHECK_THROW(server.start(), kythira::coap_security_config_error);
}

// Plain CoAP must keep working unchanged now that DTLS exists — a plaintext
// client and a plaintext server still talk.
BOOST_AUTO_TEST_CASE(test_plain_coap_still_works_alongside_dtls,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    test_server server{loopback, ephemeral_port, kythira::coap_server_config{}, test_metrics{}};
    server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
        return kythira::request_vote_response<>{request.term(), true};
    });
    server.start();

    test_client client{{{peer_node_id, endpoint_for(server.bound_port())}},
                       kythira::coap_client_config{},
                       test_metrics{}};
    const kythira::request_vote_request<> request{2, 1, 0, 0};
    const auto response =
        client.send_request_vote(peer_node_id, request, std::chrono::seconds{10}).get();
    BOOST_TEST(response.term() == 2U);
    BOOST_TEST(response.vote_granted());

    server.stop();
}

#else  // LIBNYOCI_AVAILABLE

BOOST_AUTO_TEST_CASE(test_dtls_tests_skipped_without_libnyoci) {
    BOOST_TEST_MESSAGE(
        "libnyoci not available — the libnyoci DTLS tests are skipped. Rebuild with the vcpkg "
        "'coap-libnyoci' feature to run them.");
    BOOST_TEST(!test_client::backend_available());
}

#endif  // LIBNYOCI_AVAILABLE

// ── The two modes it cannot, refused rather than downgraded ────────────────
// Compiled either way: these are properties of plan_security(), not of
// libnyoci, and the refusal is the security-relevant behaviour.

BOOST_AUTO_TEST_CASE(test_rpk_is_refused_naming_the_reason,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::coap_client_config config;
    config.security.mode = kythira::coap_auth_mode::dtls_rpk;
    config.security.credentials = kythira::rpk_credentials{};

    try {
        test_client client{{}, config, test_metrics{}};
        BOOST_FAIL("dtls_rpk must be refused, not silently downgraded");
    } catch (const kythira::coap_security_error& e) {
        const std::string message = e.what();
        BOOST_TEST(message.find("RFC 7250") != std::string::npos,
                   "the refusal must name the raw-public-key gap: " << message);
    }
}

BOOST_AUTO_TEST_CASE(test_oscore_is_refused_naming_the_reason,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::coap_client_config config;
    config.security.mode = kythira::coap_auth_mode::oscore;
    config.security.credentials = kythira::oscore_credentials{};

    try {
        test_client client{{}, config, test_metrics{}};
        BOOST_FAIL("oscore must be refused, not silently downgraded");
    } catch (const kythira::coap_security_error& e) {
        const std::string message = e.what();
        BOOST_TEST(message.find("OSCORE") != std::string::npos,
                   "the refusal must name OSCORE: " << message);
    }
}

BOOST_AUTO_TEST_SUITE_END()
