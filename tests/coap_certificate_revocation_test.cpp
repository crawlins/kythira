// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Certificate revocation checking for CoAP DTLS peers (coap-transport
 * Requirements 6.5 and 11.3).
 *
 * The certificate validators used to look for a CRL distribution point and
 * an OCSP responder in the peer's certificate, log that they were there,
 * and accept the certificate regardless. These cases build a throwaway CA, a
 * good and a revoked client certificate and a CRL listing the revoked one,
 * then check every layer that should now refuse it: the shared
 * coap_revocation::check(), both legacy validators, the dtls_pki_provider
 * callback, and finally a real DTLS handshake against a coap_server.
 */
#define BOOST_TEST_MODULE coap_certificate_revocation_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>

#ifdef LIBCOAP_AVAILABLE
#include <raft/coap_revocation.hpp>
#include <raft/coap_security_impl.hpp>

#include "coap_revocation_fixtures.hpp"
#endif

#include <cstdio>
#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <vector>

#include "test_timeout_scale.hpp"

using namespace kythira;

using test_transport_types =
    kythira::default_transport_types<kythira::future_default<kythira::request_vote_response<>>,
                                     kythira::json_rpc_serializer<std::vector<std::byte>>,
                                     kythira::noop_metrics, kythira::console_logger>;

#ifdef LIBCOAP_AVAILABLE
namespace {

using namespace kythira::testing::revocation;

auto pki_server_config(const revocation_pki& pki) -> coap_server_config {
    coap_server_config config;
    config.enable_dtls = true;
    config.cert_file = pki.server_cert_file;
    config.key_file = pki.server_key_file;
    config.ca_file = pki.ca_file;
    config.verify_peer_cert = true;
    config.revocation = pki.checking(pki.crl_file);
    return config;
}

auto pki_client_config(const revocation_pki& pki, const std::string& cert, const std::string& key)
    -> coap_client_config {
    coap_client_config config;
    config.enable_dtls = true;
    config.cert_file = cert;
    config.key_file = key;
    config.ca_file = pki.ca_file;
    config.verify_peer_cert = true;
    return config;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_revocation_check)

BOOST_AUTO_TEST_CASE(revoked_certificate_is_rejected) {
    revocation_pki pki;
    const auto failure =
        coap_revocation::check(pki.revoked_cert, pki.ca_file, pki.checking(pki.crl_file));
    BOOST_REQUIRE(failure.has_value());
    BOOST_TEST(*failure == "certificate revoked");
}

BOOST_AUTO_TEST_CASE(good_certificate_passes) {
    revocation_pki pki;
    BOOST_TEST(!coap_revocation::check(pki.good_cert, pki.ca_file, pki.checking(pki.crl_file)));
}

BOOST_AUTO_TEST_CASE(disabled_check_passes_even_a_revoked_certificate) {
    revocation_pki pki;
    auto config = pki.checking(pki.crl_file);
    config.enabled = false;
    BOOST_TEST(!coap_revocation::check(pki.revoked_cert, pki.ca_file, config));
}

// Fail closed: no CRL for the issuer is a rejection unless explicitly allowed.
BOOST_AUTO_TEST_CASE(missing_crl_rejects_unless_allowed) {
    revocation_pki pki;
    BOOST_TEST(coap_revocation::check(pki.good_cert, pki.ca_file, pki.checking()).has_value());
    BOOST_TEST(!coap_revocation::check(pki.good_cert, pki.ca_file, pki.checking({}, true)));
}

// A CRL from some other CA says nothing about this certificate's issuer.
BOOST_AUTO_TEST_CASE(crl_from_another_issuer_does_not_count) {
    revocation_pki pki;
    BOOST_TEST(
        coap_revocation::check(pki.good_cert, pki.ca_file, pki.checking(pki.foreign_crl_file))
            .has_value());
}

BOOST_AUTO_TEST_CASE(unreadable_crl_file_rejects) {
    revocation_pki pki;
    const auto failure = coap_revocation::check(pki.good_cert, pki.ca_file,
                                                pki.checking((pki.dir / "absent.pem").string()));
    BOOST_REQUIRE(failure.has_value());
    BOOST_TEST(failure->find("failed to load CRL file") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(missing_ca_file_rejects) {
    revocation_pki pki;
    BOOST_TEST(coap_revocation::check(pki.good_cert, "", pki.checking(pki.crl_file)).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(coap_revocation_wiring)

BOOST_AUTO_TEST_CASE(client_validator_rejects_revoked_server_certificate) {
    revocation_pki pki;
    auto config = pki_client_config(pki, pki.good_cert_file, pki.good_key_file);
    config.revocation = pki.checking(pki.crl_file);
    test_transport_types::metrics_type metrics;
    coap_client<test_transport_types> client({}, config, metrics);

    BOOST_TEST(client.validate_peer_certificate(pem_of(pki.good_cert)));
    BOOST_CHECK_THROW(client.validate_peer_certificate(pem_of(pki.revoked_cert)),
                      coap_security_error);
}

BOOST_AUTO_TEST_CASE(server_validator_rejects_revoked_client_certificate) {
    revocation_pki pki;
    test_transport_types::metrics_type metrics;
    coap_server<test_transport_types> server("127.0.0.1", 0, pki_server_config(pki), metrics);

    BOOST_TEST(server.validate_client_certificate(pem_of(pki.good_cert)));
    BOOST_CHECK_THROW(server.validate_client_certificate(pem_of(pki.revoked_cert)),
                      coap_security_error);
}

BOOST_AUTO_TEST_CASE(pki_provider_callback_rejects_revoked_certificate) {
    revocation_pki pki;
    pki_credentials creds;
    creds.cert_file = pki.server_cert_file;
    creds.key_file = pki.server_key_file;
    creds.ca_file = pki.ca_file;
    creds.revocation = pki.checking(pki.crl_file);
    dtls_pki_provider provider(creds, coap_security_role::server);

    const auto good = der_of(pki.good_cert);
    const auto revoked = der_of(pki.revoked_cert);
    BOOST_TEST(dtls_pki_provider::validate_cn("good-client", good.data(), good.size(), nullptr, 0,
                                              1, &provider) == 1);
    BOOST_TEST(dtls_pki_provider::validate_cn("revoked-client", revoked.data(), revoked.size(),
                                              nullptr, 0, 1, &provider) == 0);
    // Revocation adds to libcoap's verdict; it never overturns a rejection.
    BOOST_TEST(dtls_pki_provider::validate_cn("good-client", good.data(), good.size(), nullptr, 0,
                                              0, &provider) == 0);
}

// translate_legacy_fields() is what carries the legacy config's revocation
// settings into an explicit-mode provider.
BOOST_AUTO_TEST_CASE(legacy_fields_carry_revocation_into_pki_credentials) {
    coap_client_config config;
    config.cert_file = "cert.pem";
    config.key_file = "key.pem";
    config.ca_file = "ca.pem";
    config.revocation = certificate_revocation_config{true, "crl.pem", true};
    const auto security = translate_legacy_fields(config);
    const auto& creds = std::get<pki_credentials>(security.credentials);
    BOOST_TEST(creds.revocation.enabled);
    BOOST_TEST(creds.revocation.crl_file == "crl.pem");
    BOOST_TEST(creds.revocation.allow_missing_crl);
}

// A revocation check or validator with peer verification off could never
// reject anything on libcoap: its CN callback is only installed when
// verify_peer_cert is set. Refused at construction, as on the other backends
// (coap-alternate-backend-security-parity Requirement 4).
BOOST_AUTO_TEST_CASE(policy_without_peer_verification_is_refused) {
    revocation_pki pki;
    test_transport_types::metrics_type metrics;

    // Revocation through the legacy fields.
    auto server_config = pki_server_config(pki);
    server_config.verify_peer_cert = false;
    auto client_config = pki_client_config(pki, pki.good_cert_file, pki.good_key_file);
    client_config.verify_peer_cert = false;
    client_config.revocation = pki.checking(pki.crl_file);
    BOOST_CHECK_THROW((coap_server<test_transport_types>("127.0.0.1", 0, server_config, metrics)),
                      coap_security_config_error);
    BOOST_CHECK_THROW((coap_client<test_transport_types>({}, client_config, metrics)),
                      coap_security_config_error);

    // A validator through explicit credentials.
    pki_credentials creds;
    creds.cert_file = pki.server_cert_file;
    creds.key_file = pki.server_key_file;
    creds.verify_peer_cert = false;
    creds.cn_validator = [](const std::string&) { return true; };
    coap_server_config explicit_server;
    explicit_server.security = {coap_auth_mode::dtls_pki, creds, std::nullopt};
    coap_client_config explicit_client;
    explicit_client.security = explicit_server.security;
    BOOST_CHECK_THROW((coap_server<test_transport_types>("127.0.0.1", 0, explicit_server, metrics)),
                      coap_security_config_error);
    BOOST_CHECK_THROW((coap_client<test_transport_types>({}, explicit_client, metrics)),
                      coap_security_config_error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(coap_revocation_handshake)

// The server checks client certificates against the CRL during the DTLS
// handshake itself, so a revoked client never gets a session.
BOOST_AUTO_TEST_CASE(server_refuses_handshake_from_revoked_client,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    revocation_pki pki;
    test_transport_types::metrics_type server_metrics;
    coap_server<test_transport_types> server("127.0.0.1", 0, pki_server_config(pki),
                                             server_metrics);
    server.start();
    const auto endpoint = std::format("coaps://127.0.0.1:{}", server.bound_port());

    {
        test_transport_types::metrics_type metrics;
        coap_client<test_transport_types> good(
            {{1, endpoint}}, pki_client_config(pki, pki.good_cert_file, pki.good_key_file),
            metrics);
        BOOST_TEST(good.initiate_dtls_handshake(endpoint));
        BOOST_TEST(good.complete_dtls_handshake(endpoint));
    }
    {
        test_transport_types::metrics_type metrics;
        coap_client<test_transport_types> revoked(
            {{1, endpoint}}, pki_client_config(pki, pki.revoked_cert_file, pki.revoked_key_file),
            metrics);
        BOOST_TEST(revoked.initiate_dtls_handshake(endpoint));
        BOOST_TEST(!revoked.complete_dtls_handshake(endpoint));
    }

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()

#else
BOOST_AUTO_TEST_CASE(coap_certificate_revocation_test_requires_libcoap) {
    BOOST_TEST_MESSAGE("libcoap not available; revocation checks are part of its DTLS path");
}
#endif  // LIBCOAP_AVAILABLE
