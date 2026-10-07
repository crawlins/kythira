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
    ace_oauth_config config;
    config.as_token_endpoint = as.token_endpoint();
    config.client_id = "node-1";
    config.client_secret = "secret";
    config.scope = "raft-cluster";
    config.target_profile = ace_target_profile::dtls_psk;

    auto result = run_ace_token_exchange(config);
    BOOST_REQUIRE(std::holds_alternative<psk_credentials>(result));
    const auto& creds = std::get<psk_credentials>(result);
    BOOST_CHECK_EQUAL(creds.identity, "issued-identity-raft-cluster");
    BOOST_CHECK_EQUAL(creds.key.size(), 16u);
}

BOOST_AUTO_TEST_CASE(oscore_profile_populates_oscore_credentials,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    mock_authorization_server as;
    ace_oauth_config config;
    config.as_token_endpoint = as.token_endpoint();
    config.client_id = "node-1";
    config.client_secret = "secret";
    config.scope = "raft-cluster";
    config.target_profile = ace_target_profile::oscore;

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
    ace_oauth_config config;
    config.as_token_endpoint = as.token_endpoint();
    config.client_id = "node-1";
    config.client_secret = "secret";
    config.scope = "deny-me";
    config.target_profile = ace_target_profile::dtls_psk;

    BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_credential_bootstrap_error);
}

BOOST_AUTO_TEST_CASE(malformed_response_throws_bootstrap_error,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    mock_authorization_server as;
    ace_oauth_config config;
    config.as_token_endpoint = as.token_endpoint();
    config.client_id = "node-1";
    config.client_secret = "secret";
    config.scope = "malformed";
    config.target_profile = ace_target_profile::dtls_psk;

    BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_credential_bootstrap_error);
}

BOOST_AUTO_TEST_CASE(unreachable_as_throws_bootstrap_error,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    ace_oauth_config config;
    // Port 1 is reserved and nothing will ever be listening there.
    config.as_token_endpoint = "http://127.0.0.1:1/token";
    config.client_id = "node-1";
    config.client_secret = "secret";
    config.scope = "raft-cluster";
    config.target_profile = ace_target_profile::dtls_psk;

    BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_credential_bootstrap_error);
}

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
// refusal came first.
constexpr const char* unreachable_as = "http://127.0.0.1:1/token";

}  // namespace

BOOST_AUTO_TEST_CASE(replaces_static_credentials_with_the_issued_ones,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    mock_authorization_server as;
    auto config =
        ace_config(as.token_endpoint(), ace_target_profile::dtls_psk, coap_auth_mode::dtls_psk);
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
