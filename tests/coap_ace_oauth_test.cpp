// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "test_timeout_scale.hpp"
// **Feature: coap-transport-security, Requirement 9.6**
// A mock Authorization Server exercises run_ace_token_exchange(): the DTLS
// profile response populates working psk_credentials, the OSCORE profile
// response populates working oscore_credentials, and a failed/rejected
// exchange raises coap_credential_bootstrap_error rather than falling back
// to an unauthenticated session (Requirement 6.3).
#define BOOST_TEST_MODULE coap_ace_oauth_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT (30 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/coap_ace_oauth.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <utility>

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <unistd.h>
#endif

#ifdef LIBCOAP_AVAILABLE
#include <raft/future_default.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#endif

#include "coap_ace_mock_as.hpp"

using namespace kythira;

using kythira::testing::mock_authorization_server;

BOOST_AUTO_TEST_SUITE(coap_ace_oauth_tests)

BOOST_AUTO_TEST_CASE(dtls_profile_populates_psk_credentials,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    mock_authorization_server as;
    auto config = as.config("raft-cluster", ace_target_profile::dtls_psk);

    auto result = run_ace_token_exchange(config);
    BOOST_REQUIRE(std::holds_alternative<psk_credentials>(result));
    const auto& creds = std::get<psk_credentials>(result);
    BOOST_CHECK_EQUAL(creds.identity, "issued-identity-raft-cluster");
    BOOST_CHECK_EQUAL(creds.key.size(), 16u);
}

BOOST_AUTO_TEST_CASE(oscore_profile_populates_oscore_credentials,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    mock_authorization_server as;
    auto config = as.config("raft-cluster", ace_target_profile::oscore);

    auto result = run_ace_token_exchange(config);
    BOOST_REQUIRE(std::holds_alternative<oscore_credentials>(result));
    const auto& creds = std::get<oscore_credentials>(result);
    BOOST_CHECK_EQUAL(creds.sender_id.size(), 1u);
    BOOST_CHECK_EQUAL(creds.recipient_id.size(), 1u);
    BOOST_CHECK_EQUAL(creds.master_secret.size(), 16u);
    BOOST_CHECK_EQUAL(creds.master_salt.size(), 8u);
    BOOST_CHECK_EQUAL(creds.aead_algorithm, "AES-CCM-16-64-128");
}

BOOST_AUTO_TEST_CASE(denied_scope_throws_bootstrap_error,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    mock_authorization_server as;
    auto config = as.config("deny-me", ace_target_profile::dtls_psk);

    BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_credential_bootstrap_error);
}

BOOST_AUTO_TEST_CASE(malformed_response_throws_bootstrap_error,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    mock_authorization_server as;
    auto config = as.config("malformed", ace_target_profile::dtls_psk);

    BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_credential_bootstrap_error);
}

BOOST_AUTO_TEST_CASE(unreachable_as_throws_bootstrap_error,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    ace_oauth_config config;
    // Port 1 is reserved and nothing will ever be listening there.
    config.as_token_endpoint = "https://127.0.0.1:1/token";
    config.client_id = "node-1";
    config.client_secret = "secret";
    config.scope = "raft-cluster";
    config.target_profile = ace_target_profile::dtls_psk;

    BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_credential_bootstrap_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Cleartext refusal ─────────────────────────────────────────────────────
// The request carries client_secret and the reply the PSK / OSCORE master
// secret, so a non-https endpoint is a config error raised before any
// request goes out. Plain http passes only to a loopback host, and only
// with allow_plain_http_loopback.

BOOST_AUTO_TEST_SUITE(ace_token_endpoint_scheme_tests)

BOOST_AUTO_TEST_CASE(loopback_http_without_the_opt_in_is_refused_before_any_request,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    mock_authorization_server as;
    auto config = as.config("raft-cluster", ace_target_profile::dtls_psk);
    config.allow_plain_http_loopback = false;
    BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_security_config_error);
    BOOST_TEST(as.request_count() == 0);

    config.allow_plain_http_loopback = true;
    BOOST_CHECK_NO_THROW(run_ace_token_exchange(config));
    BOOST_TEST(as.request_count() == 1);
}

BOOST_AUTO_TEST_CASE(off_host_http_is_refused_even_with_the_opt_in) {
    for (const auto* endpoint : {
             "http://192.0.2.1/token",
             "http://as.example/token",
             "http://localhost.example/token",
             "http://127.0.0.1.example/token",
             "http://[2001:db8::1]:8080/token",
             // userinfo and fragment tricks put the real host after the "@"
             // or before the "#".
             "http://127.0.0.1@as.example/token",
             "http://as.example#@127.0.0.1/token",
         }) {
        ace_oauth_config config;
        config.as_token_endpoint = endpoint;
        config.allow_plain_http_loopback = true;
        BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_security_config_error);
    }
}

BOOST_AUTO_TEST_CASE(other_schemes_and_bare_hosts_are_refused) {
    for (const auto* endpoint : {
             "ftp://127.0.0.1/token",
             "HTTP://127.0.0.1/token",
             "127.0.0.1:8080/token",
             "",
         }) {
        ace_oauth_config config;
        config.as_token_endpoint = endpoint;
        config.allow_plain_http_loopback = true;
        BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_security_config_error);
    }
}

BOOST_AUTO_TEST_CASE(every_loopback_spelling_is_accepted_with_the_opt_in) {
    // Port 1 has no listener, so getting past the scheme check shows up as
    // a bootstrap (network) error instead of a config error.
    for (const auto* endpoint : {
             "http://127.0.0.1:1/token",
             "http://127.8.9.10:1/token",
             "http://localhost:1/token",
             "http://[::1]:1/token",
         }) {
        ace_oauth_config config;
        config.as_token_endpoint = endpoint;
        config.allow_plain_http_loopback = true;
        BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_credential_bootstrap_error);
    }
}

BOOST_AUTO_TEST_CASE(resolve_ace_bootstrap_refuses_cleartext_and_keeps_the_static_credentials) {
    coap_security_config config;
    config.mode = coap_auth_mode::dtls_psk;
    config.credentials = psk_credentials{"static", {std::byte{1}}};
    ace_oauth_config ace;
    ace.as_token_endpoint = "http://as.example/token";
    config.ace_bootstrap = ace;
    BOOST_CHECK_THROW(resolve_ace_bootstrap(config), coap_security_config_error);
    BOOST_TEST(std::get<psk_credentials>(config.credentials).identity == "static");
}

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
namespace {

// A self-signed P-256 certificate for 127.0.0.1, written to PEM files the
// mock AS loads, plus the certificate PEM a client can trust it by.
struct self_signed_tls {
    std::filesystem::path cert_path;
    std::filesystem::path key_path;
    std::string cert_pem;

    self_signed_tls() {
        EVP_PKEY* key = EVP_EC_gen("P-256");
        BOOST_REQUIRE(key != nullptr);
        X509* cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("mock-as"), -1, -1, 0);
        X509_set_issuer_name(cert, name);
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
        for (const auto& [nid, value] : std::initializer_list<std::pair<int, const char*>>{
                 {NID_subject_alt_name, "IP:127.0.0.1"},
                 {NID_basic_constraints, "critical,CA:TRUE"}}) {
            X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
            BOOST_REQUIRE(ext != nullptr);
            X509_add_ext(cert, ext, -1);
            X509_EXTENSION_free(ext);
        }
        BOOST_REQUIRE(X509_sign(cert, key, EVP_sha256()) > 0);

        auto to_pem = [](auto write) {
            BIO* bio = BIO_new(BIO_s_mem());
            write(bio);
            char* data = nullptr;
            long len = BIO_get_mem_data(bio, &data);
            std::string pem(data, static_cast<std::size_t>(len));
            BIO_free(bio);
            return pem;
        };
        cert_pem = to_pem([&](BIO* bio) { PEM_write_bio_X509(bio, cert); });
        auto key_pem = to_pem([&](BIO* bio) {
            PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
        });
        X509_free(cert);
        EVP_PKEY_free(key);

        static std::atomic<int> instance{0};
        auto stem =
            std::filesystem::temp_directory_path() /
            ("coap_ace_mock_as_" + std::to_string(::getpid()) + "_" + std::to_string(instance++));
        cert_path = stem.string() + "_cert.pem";
        key_path = stem.string() + "_key.pem";
        std::ofstream(cert_path) << cert_pem;
        std::ofstream(key_path) << key_pem;
    }

    ~self_signed_tls() {
        std::error_code ec;
        std::filesystem::remove(cert_path, ec);
        std::filesystem::remove(key_path, ec);
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(https_endpoint_works_with_its_ca_bundle_and_no_opt_in,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    self_signed_tls tls;
    mock_authorization_server as(tls.cert_path.string(), tls.key_path.string());
    auto config = as.config("raft-cluster", ace_target_profile::dtls_psk);
    BOOST_REQUIRE(config.as_token_endpoint.starts_with("https://"));
    BOOST_REQUIRE(!config.allow_plain_http_loopback);
    config.as_ca_bundle_pem = tls.cert_pem;

    auto result = run_ace_token_exchange(config);
    BOOST_REQUIRE(std::holds_alternative<psk_credentials>(result));
    BOOST_CHECK_EQUAL(std::get<psk_credentials>(result).identity, "issued-identity-raft-cluster");
}

BOOST_AUTO_TEST_CASE(https_endpoint_with_an_untrusted_certificate_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    self_signed_tls tls;
    self_signed_tls other;
    mock_authorization_server as(tls.cert_path.string(), tls.key_path.string());
    auto config = as.config("raft-cluster", ace_target_profile::dtls_psk);
    // A bundle that does not hold the AS's certificate: the client secret
    // must not go out over a handshake it could not verify.
    config.as_ca_bundle_pem = other.cert_pem;
    BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_credential_bootstrap_error);
    BOOST_TEST(as.request_count() == 0);
}
#endif  // CPPHTTPLIB_OPENSSL_SUPPORT

BOOST_AUTO_TEST_SUITE_END()

// ── resolve_ace_bootstrap(): the step every CoAP backend runs ─────────────
// (coap-alternate-backend-security-parity Requirements 5 and 6)

BOOST_AUTO_TEST_SUITE(resolve_ace_bootstrap_tests)

namespace {

auto ace_config(std::string endpoint, ace_target_profile profile, coap_auth_mode mode)
    -> coap_security_config {
    ace_oauth_config ace;
    ace.as_token_endpoint = std::move(endpoint);
    ace.client_id = "node-1";
    ace.client_secret = "secret";
    ace.scope = "raft-cluster";
    ace.target_profile = profile;
    coap_security_config config;
    config.mode = mode;
    config.ace_bootstrap = ace;
    return config;
}

// Nothing listens on port 1, so reaching the AS would raise
// coap_credential_bootstrap_error instead: a config_error proves the
// refusal came first. https, so the cleartext refusal (also a config_error)
// cannot be what these tests see.
constexpr const char* unreachable_as = "https://127.0.0.1:1/token";

}  // namespace

BOOST_AUTO_TEST_CASE(replaces_static_credentials_with_the_issued_ones,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    mock_authorization_server as;
    auto config =
        ace_config(as.token_endpoint(), ace_target_profile::dtls_psk, coap_auth_mode::dtls_psk);
    config.ace_bootstrap->allow_plain_http_loopback = true;
    config.credentials = psk_credentials{"stale-static-identity", {std::byte{1}}};
    resolve_ace_bootstrap(config);
    BOOST_REQUIRE(std::holds_alternative<psk_credentials>(config.credentials));
    BOOST_TEST(std::get<psk_credentials>(config.credentials).identity ==
               "issued-identity-raft-cluster");
}

// The AS knows nothing about where this node keeps its OSCORE counters: the
// issued context keeps the configured ones, or a restart that gets the same
// Master Secret back would start its Sender Sequence Number at 0 again.
BOOST_AUTO_TEST_CASE(issued_oscore_context_keeps_the_configured_sequence_state,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    mock_authorization_server as;
    auto config =
        ace_config(as.token_endpoint(), ace_target_profile::oscore, coap_auth_mode::oscore);
    config.ace_bootstrap->allow_plain_http_loopback = true;
    oscore_credentials counters;
    counters.sequence_state_dir = "/var/lib/kythira/oscore";
    counters.volatile_sequence_state = true;
    config.credentials = counters;
    resolve_ace_bootstrap(config);
    BOOST_REQUIRE(std::holds_alternative<oscore_credentials>(config.credentials));
    const auto& issued = std::get<oscore_credentials>(config.credentials);
    BOOST_TEST(issued.master_secret.size() == 16U);
    BOOST_TEST(issued.sequence_state_dir == "/var/lib/kythira/oscore");
    BOOST_TEST(issued.volatile_sequence_state);
}

BOOST_AUTO_TEST_CASE(static_oscore_needs_sequence_state) {
    coap_security_config config;
    config.mode = coap_auth_mode::oscore;
    oscore_credentials creds;
    creds.master_secret = std::vector<std::byte>(16, std::byte{0x2a});
    config.credentials = creds;
    BOOST_CHECK_THROW(validate_oscore_sequence_state(config), coap_security_config_error);

    std::get<oscore_credentials>(config.credentials).sequence_state_dir = "/var/lib/kythira";
    BOOST_CHECK_NO_THROW(validate_oscore_sequence_state(config));

    creds.volatile_sequence_state = true;
    config.credentials = creds;
    BOOST_CHECK_NO_THROW(validate_oscore_sequence_state(config));

    // EDHOC derives a fresh Master Secret per handshake.
    creds.volatile_sequence_state = false;
    creds.bootstrap_method = oscore_bootstrap::edhoc;
    config.credentials = creds;
    BOOST_CHECK_NO_THROW(validate_oscore_sequence_state(config));
}

BOOST_AUTO_TEST_CASE(no_ace_bootstrap_leaves_the_config_alone) {
    coap_security_config config;
    config.mode = coap_auth_mode::dtls_psk;
    config.credentials = psk_credentials{"static", {std::byte{1}}};
    resolve_ace_bootstrap(config);
    BOOST_TEST(std::get<psk_credentials>(config.credentials).identity == "static");
}

BOOST_AUTO_TEST_CASE(profile_that_disagrees_with_mode_is_refused_before_the_as) {
    for (const auto& [profile, mode] :
         {std::pair{ace_target_profile::dtls_psk, coap_auth_mode::oscore},
          std::pair{ace_target_profile::dtls_psk, coap_auth_mode::none},
          std::pair{ace_target_profile::dtls_psk, coap_auth_mode::dtls_pki},
          std::pair{ace_target_profile::oscore, coap_auth_mode::dtls_psk},
          std::pair{ace_target_profile::oscore, coap_auth_mode::none}}) {
        auto config = ace_config(unreachable_as, profile, mode);
        BOOST_CHECK_THROW(resolve_ace_bootstrap(config), coap_security_config_error);
    }
}

BOOST_AUTO_TEST_CASE(ace_together_with_edhoc_is_refused_as_ambiguous) {
    auto config = ace_config(unreachable_as, ace_target_profile::oscore, coap_auth_mode::oscore);
    oscore_credentials creds;
    creds.bootstrap_method = oscore_bootstrap::edhoc;
    config.credentials = creds;
    BOOST_CHECK_THROW(resolve_ace_bootstrap(config), coap_security_config_error);
}

#ifdef LIBCOAP_AVAILABLE
// The libcoap constructors run the same helper, so they refuse the same
// configs at construction with the same error.
using libcoap_types =
    kythira::default_transport_types<kythira::future_default<kythira::request_vote_response<>>,
                                     kythira::json_rpc_serializer<std::vector<std::byte>>,
                                     kythira::noop_metrics, kythira::console_logger>;

BOOST_AUTO_TEST_CASE(libcoap_refuses_mismatch_and_ambiguity_at_construction) {
    auto mismatch =
        ace_config(unreachable_as, ace_target_profile::oscore, coap_auth_mode::dtls_psk);
    auto ambiguous = ace_config(unreachable_as, ace_target_profile::oscore, coap_auth_mode::oscore);
    oscore_credentials edhoc;
    edhoc.bootstrap_method = oscore_bootstrap::edhoc;
    ambiguous.credentials = edhoc;

    for (const auto& security : {mismatch, ambiguous}) {
        coap_client_config client_config;
        client_config.security = security;
        BOOST_CHECK_THROW((coap_client<libcoap_types>({}, client_config, kythira::noop_metrics{})),
                          coap_security_config_error);
        coap_server_config server_config;
        server_config.security = security;
        BOOST_CHECK_THROW(
            (coap_server<libcoap_types>("127.0.0.1", 0, server_config, kythira::noop_metrics{})),
            coap_security_config_error);
    }
}

BOOST_AUTO_TEST_CASE(libcoap_refuses_static_oscore_without_sequence_state) {
    coap_security_config security;
    security.mode = coap_auth_mode::oscore;
    oscore_credentials creds;
    creds.master_secret = std::vector<std::byte>(16, std::byte{0x2a});
    creds.sender_id = {std::byte{0x01}};
    creds.recipient_id = {std::byte{0x00}};
    security.credentials = creds;

    coap_client_config client_config;
    client_config.security = security;
    BOOST_CHECK_THROW((coap_client<libcoap_types>({}, client_config, kythira::noop_metrics{})),
                      coap_security_config_error);
    coap_server_config server_config;
    server_config.security = security;
    BOOST_CHECK_THROW(
        (coap_server<libcoap_types>("127.0.0.1", 0, server_config, kythira::noop_metrics{})),
        coap_security_config_error);
}
#endif  // LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_SUITE_END()
