// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The test cases coap-alternate-backend-security-parity (Requirement 8) runs
// against each alternate CoAP backend. Written once, so the two backends are
// held to literally the same cases; each suite includes this after defining,
// in namespace parity:
//
//   test_client, test_server            the backend's client and server types
//   endpoint_for(std::uint16_t port)    the endpoint string a client dials
//   client_config()                     a coap_client_config with the
//                                       backend's preferred retransmit timing
//
// Every handshake is real and on loopback. The negative cases prove the
// handler never ran, not just that the RPC failed: a transport that let the
// request through and lost the reply would otherwise pass them.

#include <raft/coap_security.hpp>

#include "coap_ace_mock_as.hpp"
#include "coap_revocation_fixtures.hpp"
#include "test_timeout_scale.hpp"

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

namespace parity {

using kythira::testing::mock_authorization_server;
using kythira::testing::revocation::revocation_pki;

constexpr std::uint64_t peer_node_id = 7;
constexpr const char* loopback = "127.0.0.1";

/// A started server whose request_vote handler records whether it ever ran.
struct recording_server {
    test_server server;
    std::shared_ptr<std::atomic<bool>> handler_ran = std::make_shared<std::atomic<bool>>(false);

    explicit recording_server(kythira::coap_server_config config)
        : server{loopback, 0, std::move(config), kythira::noop_metrics{}} {
        server.register_request_vote_handler(
            [ran = handler_ran](const kythira::request_vote_request<>& request) {
                ran->store(true);
                return kythira::request_vote_response<>{request.term(), true};
            });
        server.start();
    }

    recording_server(const recording_server&) = delete;
    auto operator=(const recording_server&) -> recording_server& = delete;

    ~recording_server() { server.stop(); }
};

[[nodiscard]] inline auto make_client(const recording_server& peer,
                                      const kythira::coap_client_config& config) -> test_client {
    return test_client{
        {{peer_node_id, endpoint_for(peer.server.bound_port())}}, config, kythira::noop_metrics{}};
}

/// True when one RPC completes and is answered.
[[nodiscard]] inline auto answered(test_client& client,
                                   std::chrono::seconds timeout = std::chrono::seconds{30})
    -> bool {
    const kythira::request_vote_request<> request{3, 11, 0, 0};
    try {
        const auto response = client.send_request_vote(peer_node_id, request, timeout).get();
        return response.term() == 3U && response.vote_granted();
    } catch (const kythira::coap_transport_error& error) {
        BOOST_TEST_MESSAGE("RPC failed: " << error.what());
        return false;
    }
}

// ── PKI configs ────────────────────────────────────────────────────────────

[[nodiscard]] inline auto pki_security(
    const revocation_pki& pki, const std::string& cert, const std::string& key,
    std::optional<kythira::certificate_revocation_config> revocation = std::nullopt)
    -> kythira::coap_security_config {
    kythira::pki_credentials creds;
    creds.cert_file = cert;
    creds.key_file = key;
    creds.ca_file = pki.ca_file;
    creds.verify_peer_cert = true;
    if (revocation) {
        creds.revocation = *revocation;
    }
    return {kythira::coap_auth_mode::dtls_pki, creds, std::nullopt};
}

[[nodiscard]] inline auto server_config_with(kythira::coap_security_config security)
    -> kythira::coap_server_config {
    kythira::coap_server_config config;
    config.security = std::move(security);
    return config;
}

[[nodiscard]] inline auto client_config_with(kythira::coap_security_config security)
    -> kythira::coap_client_config {
    auto config = client_config();
    config.security = std::move(security);
    return config;
}

[[nodiscard]] inline auto with_validator(kythira::coap_security_config security,
                                         std::function<bool(const std::string&)> validator)
    -> kythira::coap_security_config {
    std::get<kythira::pki_credentials>(security.credentials).cn_validator = std::move(validator);
    return security;
}

// ── ACE configs ────────────────────────────────────────────────────────────

[[nodiscard]] inline auto ace_security(const mock_authorization_server& as,
                                       kythira::ace_target_profile profile, std::string scope)
    -> kythira::coap_security_config {
    auto ace = as.config(std::move(scope), profile);

    kythira::coap_security_config security;
    security.mode = profile == kythira::ace_target_profile::oscore
                        ? kythira::coap_auth_mode::oscore
                        : kythira::coap_auth_mode::dtls_psk;
    security.ace_bootstrap = ace;
    if (profile == kythira::ace_target_profile::oscore) {
        // The AS-issued context inherits this; the mock AS issues
        // test-only keys, so process-lifetime counters are enough.
        kythira::oscore_credentials counters;
        counters.volatile_sequence_state = true;
        security.credentials = counters;
    }
    return security;
}

// A static OSCORE pair with test-only keys, sequence state left unset.
[[nodiscard]] inline auto static_oscore_security(bool is_client) -> kythira::coap_security_config {
    kythira::oscore_credentials creds;
    creds.master_secret = std::vector<std::byte>(16, std::byte{0x2a});
    creds.master_salt = std::vector<std::byte>(8, std::byte{0x77});
    creds.sender_id = {is_client ? std::byte{0x00} : std::byte{0x01}};
    creds.recipient_id = {is_client ? std::byte{0x01} : std::byte{0x00}};
    kythira::coap_security_config security;
    security.mode = kythira::coap_auth_mode::oscore;
    security.credentials = creds;
    return security;
}

}  // namespace parity

BOOST_AUTO_TEST_SUITE(revocation)

// Requirements 1.1 / 3.1: a revocation-enabled server refuses a revoked
// client, and the request never reaches a handler.
BOOST_AUTO_TEST_CASE(server_refuses_revoked_client,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    parity::revocation_pki pki;
    parity::recording_server peer{parity::server_config_with(parity::pki_security(
        pki, pki.server_cert_file, pki.server_key_file, pki.checking(pki.crl_file)))};

    auto revoked =
        parity::make_client(peer, parity::client_config_with(parity::pki_security(
                                      pki, pki.revoked_cert_file, pki.revoked_key_file)));
    BOOST_TEST(!parity::answered(revoked, std::chrono::seconds{5}));
    BOOST_TEST(!peer.handler_ran->load(), "a revoked client must never be served");
}

// Requirements 1.2 / 3.1: a revocation-enabled client refuses a revoked
// server, and no response from it is delivered.
BOOST_AUTO_TEST_CASE(client_refuses_revoked_server,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    parity::revocation_pki pki;
    parity::recording_server peer{parity::server_config_with(
        parity::pki_security(pki, pki.revoked_server_cert_file, pki.revoked_server_key_file))};

    auto client = parity::make_client(
        peer, parity::client_config_with(parity::pki_security(
                  pki, pki.good_cert_file, pki.good_key_file, pki.checking(pki.crl_file))));
    BOOST_TEST(!parity::answered(client, std::chrono::seconds{5}));
    BOOST_TEST(!peer.handler_ran->load(), "no request may cross a refused session");
}

// Requirement 8.1: under the same CRL, unrevoked certificates on both ends
// still complete an RPC, with both sides checking.
BOOST_AUTO_TEST_CASE(good_certificates_pass_the_same_crl,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    parity::revocation_pki pki;
    parity::recording_server peer{parity::server_config_with(parity::pki_security(
        pki, pki.server_cert_file, pki.server_key_file, pki.checking(pki.crl_file)))};

    auto client = parity::make_client(
        peer, parity::client_config_with(parity::pki_security(
                  pki, pki.good_cert_file, pki.good_key_file, pki.checking(pki.crl_file))));
    BOOST_TEST(parity::answered(client));
    BOOST_TEST(peer.handler_ran->load());
}

// Requirement 1.3: no CRL for the issuer fails closed, unless the config
// explicitly allows a missing CRL.
BOOST_AUTO_TEST_CASE(missing_crl_is_refused_unless_allowed,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    parity::revocation_pki pki;
    {
        parity::recording_server peer{parity::server_config_with(
            parity::pki_security(pki, pki.server_cert_file, pki.server_key_file, pki.checking()))};
        auto client = parity::make_client(peer, parity::client_config_with(parity::pki_security(
                                                    pki, pki.good_cert_file, pki.good_key_file)));
        BOOST_TEST(!parity::answered(client, std::chrono::seconds{5}));
        BOOST_TEST(!peer.handler_ran->load());
    }
    {
        parity::recording_server peer{parity::server_config_with(parity::pki_security(
            pki, pki.server_cert_file, pki.server_key_file, pki.checking({}, true)))};
        auto client = parity::make_client(peer, parity::client_config_with(parity::pki_security(
                                                    pki, pki.good_cert_file, pki.good_key_file)));
        BOOST_TEST(parity::answered(client));
    }
}

// An unreadable CRL file is a policy that cannot run: fail closed.
BOOST_AUTO_TEST_CASE(unreadable_crl_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    parity::revocation_pki pki;
    parity::recording_server peer{parity::server_config_with(
        parity::pki_security(pki, pki.server_cert_file, pki.server_key_file,
                             pki.checking((pki.dir / "absent.pem").string())))};
    auto client = parity::make_client(peer, parity::client_config_with(parity::pki_security(
                                                pki, pki.good_cert_file, pki.good_key_file)));
    BOOST_TEST(!parity::answered(client, std::chrono::seconds{5}));
    BOOST_TEST(!peer.handler_ran->load());
}

// Requirements 2.4 / 3.3: revocation is checked first, and a revoked peer is
// refused without the validator ever being asked.
BOOST_AUTO_TEST_CASE(revocation_runs_before_the_validator,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    parity::revocation_pki pki;
    auto validator_calls = std::make_shared<std::atomic<int>>(0);
    parity::recording_server peer{parity::server_config_with(
        parity::with_validator(parity::pki_security(pki, pki.server_cert_file, pki.server_key_file,
                                                    pki.checking(pki.crl_file)),
                               [validator_calls](const std::string&) {
                                   validator_calls->fetch_add(1);
                                   return true;
                               }))};

    auto revoked =
        parity::make_client(peer, parity::client_config_with(parity::pki_security(
                                      pki, pki.revoked_cert_file, pki.revoked_key_file)));
    BOOST_TEST(!parity::answered(revoked, std::chrono::seconds{5}));
    BOOST_TEST(validator_calls->load() == 0, "the validator must not see a revoked peer");

    auto good = parity::make_client(peer, parity::client_config_with(parity::pki_security(
                                              pki, pki.good_cert_file, pki.good_key_file)));
    BOOST_TEST(parity::answered(good));
    BOOST_TEST(validator_calls->load() > 0, "the validator must still run for a good peer");
}

// Requirement 1.5: revocation configured through the legacy cert_file fields
// is carried by translate_legacy_fields() and enforced the same way.
BOOST_AUTO_TEST_CASE(legacy_fields_enforce_revocation,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    parity::revocation_pki pki;
    kythira::coap_server_config server_config;
    server_config.enable_dtls = true;
    server_config.cert_file = pki.server_cert_file;
    server_config.key_file = pki.server_key_file;
    server_config.ca_file = pki.ca_file;
    server_config.verify_peer_cert = true;
    server_config.revocation = pki.checking(pki.crl_file);
    parity::recording_server peer{server_config};

    auto legacy_client = [&](const std::string& cert, const std::string& key) {
        auto config = parity::client_config();
        config.enable_dtls = true;
        config.cert_file = cert;
        config.key_file = key;
        config.ca_file = pki.ca_file;
        config.verify_peer_cert = true;
        return parity::make_client(peer, config);
    };

    auto revoked = legacy_client(pki.revoked_cert_file, pki.revoked_key_file);
    BOOST_TEST(!parity::answered(revoked, std::chrono::seconds{5}));
    BOOST_TEST(!peer.handler_ran->load());

    auto good = legacy_client(pki.good_cert_file, pki.good_key_file);
    BOOST_TEST(parity::answered(good));
}

// Requirement 4: a revocation check or validator with peer verification off
// could never reject anything, so it is refused at construction on both ends.
BOOST_AUTO_TEST_CASE(policy_without_peer_verification_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    parity::revocation_pki pki;
    auto unverified = [&](bool with_revocation) {
        auto security = parity::pki_security(pki, pki.server_cert_file, pki.server_key_file);
        auto& creds = std::get<kythira::pki_credentials>(security.credentials);
        creds.verify_peer_cert = false;
        if (with_revocation) {
            creds.revocation = pki.checking(pki.crl_file);
        } else {
            creds.cn_validator = [](const std::string&) { return true; };
        }
        return security;
    };
    for (const bool with_revocation : {true, false}) {
        BOOST_CHECK_THROW(
            (parity::test_server{parity::loopback, 0,
                                 parity::server_config_with(unverified(with_revocation)),
                                 kythira::noop_metrics{}}),
            kythira::coap_security_config_error);
        BOOST_CHECK_THROW(
            (parity::test_client{{{parity::peer_node_id, "127.0.0.1:1"}},
                                 parity::client_config_with(unverified(with_revocation)),
                                 kythira::noop_metrics{}}),
            kythira::coap_security_config_error);
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ace_oauth)

// Requirements 5.1 / 5.3: an ACE-provisioned DTLS-PSK pair completes an RPC
// using the identity and key the AS issued, with no static credentials.
BOOST_AUTO_TEST_CASE(ace_dtls_psk_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    parity::mock_authorization_server as;
    parity::recording_server peer{parity::server_config_with(
        parity::ace_security(as, kythira::ace_target_profile::dtls_psk, "raft-cluster-responder"))};
    auto client =
        parity::make_client(peer, parity::client_config_with(parity::ace_security(
                                      as, kythira::ace_target_profile::dtls_psk, "raft-cluster")));
    BOOST_TEST(parity::answered(client));
    BOOST_TEST(peer.handler_ran->load());
}

// Requirement 5.4: an ACE-provisioned OSCORE pair completes an RPC. Before
// this spec the alternates reached std::get<oscore_credentials> on a
// monostate and threw std::bad_variant_access.
BOOST_AUTO_TEST_CASE(ace_oscore_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    parity::mock_authorization_server as;
    parity::recording_server peer{parity::server_config_with(
        parity::ace_security(as, kythira::ace_target_profile::oscore, "raft-cluster-responder"))};
    auto client =
        parity::make_client(peer, parity::client_config_with(parity::ace_security(
                                      as, kythira::ace_target_profile::oscore, "raft-cluster")));
    BOOST_TEST(parity::answered(client));
    BOOST_TEST(peer.handler_ran->load());
}

// Requirement 5.2: a refusing AS fails construction; nothing falls back to
// static or absent credentials.
BOOST_AUTO_TEST_CASE(denied_scope_fails_construction,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    parity::mock_authorization_server as;
    for (const auto profile :
         {kythira::ace_target_profile::dtls_psk, kythira::ace_target_profile::oscore}) {
        BOOST_CHECK_THROW((parity::test_client{{{parity::peer_node_id, "127.0.0.1:1"}},
                                               parity::client_config_with(
                                                   parity::ace_security(as, profile, "deny-me")),
                                               kythira::noop_metrics{}}),
                          kythira::coap_credential_bootstrap_error);
        BOOST_CHECK_THROW((parity::test_server{parity::loopback, 0,
                                               parity::server_config_with(
                                                   parity::ace_security(as, profile, "deny-me")),
                                               kythira::noop_metrics{}}),
                          kythira::coap_credential_bootstrap_error);
    }
}

// Requirement 6.1: a profile that disagrees with security.mode is refused
// before the AS is contacted (the endpoint here has nothing listening).
BOOST_AUTO_TEST_CASE(profile_mode_mismatch_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::ace_oauth_config ace;
    ace.as_token_endpoint = "https://127.0.0.1:1/token";
    ace.target_profile = kythira::ace_target_profile::oscore;
    kythira::coap_security_config security;
    security.mode = kythira::coap_auth_mode::dtls_psk;
    security.ace_bootstrap = ace;

    BOOST_CHECK_THROW((parity::test_client{{{parity::peer_node_id, "127.0.0.1:1"}},
                                           parity::client_config_with(security),
                                           kythira::noop_metrics{}}),
                      kythira::coap_security_config_error);
    BOOST_CHECK_THROW(
        (parity::test_server{parity::loopback, 0, parity::server_config_with(security),
                             kythira::noop_metrics{}}),
        kythira::coap_security_config_error);
}

// Requirement 6.2: ACE and an EDHOC bootstrap together are ambiguous.
BOOST_AUTO_TEST_CASE(ace_with_edhoc_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::ace_oauth_config ace;
    ace.as_token_endpoint = "https://127.0.0.1:1/token";
    ace.target_profile = kythira::ace_target_profile::oscore;
    kythira::oscore_credentials creds;
    creds.bootstrap_method = kythira::oscore_bootstrap::edhoc;
    kythira::coap_security_config security;
    security.mode = kythira::coap_auth_mode::oscore;
    security.credentials = creds;
    security.ace_bootstrap = ace;

    BOOST_CHECK_THROW((parity::test_client{{{parity::peer_node_id, "127.0.0.1:1"}},
                                           parity::client_config_with(security),
                                           kythira::noop_metrics{}}),
                      kythira::coap_security_config_error);
    BOOST_CHECK_THROW(
        (parity::test_server{parity::loopback, 0, parity::server_config_with(security),
                             kythira::noop_metrics{}}),
        kythira::coap_security_config_error);
}

// Requirement 7: OSCORE without OSCORE credentials is a security-config
// error, not std::bad_variant_access.
BOOST_AUTO_TEST_CASE(oscore_without_credentials_is_a_config_error,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::coap_security_config security;
    security.mode = kythira::coap_auth_mode::oscore;

    BOOST_CHECK_THROW((parity::test_client{{{parity::peer_node_id, "127.0.0.1:1"}},
                                           parity::client_config_with(security),
                                           kythira::noop_metrics{}}),
                      kythira::coap_security_config_error);
    BOOST_CHECK_THROW(
        (parity::test_server{parity::loopback, 0, parity::server_config_with(security),
                             kythira::noop_metrics{}}),
        kythira::coap_security_config_error);
}

BOOST_AUTO_TEST_SUITE_END()

// H7: a static Master Secret whose Sender Sequence Number lives only in
// memory restarts at Partial IV 0 after a restart and reuses every AES-CCM
// nonce it issued before. Every backend refuses that at construction, and
// accepts it once the counters have somewhere durable to live.
BOOST_AUTO_TEST_SUITE(oscore_sequence_state)

BOOST_AUTO_TEST_CASE(static_oscore_without_sequence_state_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    BOOST_CHECK_THROW(
        (parity::test_client{{{parity::peer_node_id, "127.0.0.1:1"}},
                             parity::client_config_with(parity::static_oscore_security(true)),
                             kythira::noop_metrics{}}),
        kythira::coap_security_config_error);
    BOOST_CHECK_THROW(
        (parity::test_server{parity::loopback, 0,
                             parity::server_config_with(parity::static_oscore_security(false)),
                             kythira::noop_metrics{}}),
        kythira::coap_security_config_error);
}

// ACE replaces the credentials with what the AS issued; without the
// configured counters carried over, the issued context would be refused too.
BOOST_AUTO_TEST_CASE(ace_oscore_without_sequence_state_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    parity::mock_authorization_server as;
    auto security = parity::ace_security(as, kythira::ace_target_profile::oscore, "raft-cluster");
    security.credentials = std::monostate{};
    BOOST_CHECK_THROW((parity::test_client{{{parity::peer_node_id, "127.0.0.1:1"}},
                                           parity::client_config_with(security),
                                           kythira::noop_metrics{}}),
                      kythira::coap_security_config_error);
}

BOOST_AUTO_TEST_CASE(static_oscore_with_sequence_state_dir_round_trips,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("kythira-oscore-seq-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    {
        auto server_security = parity::static_oscore_security(false);
        std::get<kythira::oscore_credentials>(server_security.credentials).sequence_state_dir =
            dir.string();
        auto client_security = parity::static_oscore_security(true);
        std::get<kythira::oscore_credentials>(client_security.credentials).sequence_state_dir =
            dir.string();
        parity::recording_server peer{parity::server_config_with(server_security)};
        auto client = parity::make_client(peer, parity::client_config_with(client_security));
        BOOST_TEST(parity::answered(client));
        BOOST_TEST(peer.handler_ran->load());
    }
    std::filesystem::remove_all(dir);
}

BOOST_AUTO_TEST_SUITE_END()
