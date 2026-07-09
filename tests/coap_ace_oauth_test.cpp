// **Feature: coap-transport-security, Requirement 9.6**
// A mock Authorization Server exercises run_ace_token_exchange(): the DTLS
// profile response populates working psk_credentials, the OSCORE profile
// response populates working oscore_credentials, and a failed/rejected
// exchange raises coap_credential_bootstrap_error rather than falling back
// to an unauthenticated session (Requirement 6.3).
#define BOOST_TEST_MODULE coap_ace_oauth_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 30

#include <raft/coap_ace_oauth.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <chrono>
#include <thread>

using namespace kythira;

namespace {

// A minimal mock AS: /token responds according to which case name is
// passed in the request's "scope" field, so a single server instance can
// drive every test case below.
class mock_authorization_server {
public:
    mock_authorization_server() {
        _server.Post("/token", [](const httplib::Request& req, httplib::Response& res) {
            auto body = boost::json::parse(req.body).as_object();
            auto scope = std::string(body.at("scope").as_string());
            auto profile = std::string(body.at("ace_profile").as_string());

            if (scope == "deny-me") {
                res.status = 403;
                res.set_content(R"({"error":"access_denied"})", "application/json");
                return;
            }
            if (scope == "malformed") {
                res.status = 200;
                res.set_content("not json", "text/plain");
                return;
            }

            if (profile == "coap_dtls") {
                boost::json::object response{
                    {"psk_identity", "issued-identity-" + scope},
                    {"psk_key_hex", "0102030405060708090a0b0c0d0e0f10"},
                };
                res.set_content(boost::json::serialize(response), "application/json");
            } else {
                boost::json::object response{
                    {"sender_id_hex", "00"},
                    {"recipient_id_hex", "01"},
                    {"master_secret_hex", "0102030405060708090a0b0c0d0e0f10"},
                    {"master_salt_hex", "0102030405060708"},
                    {"aead_algorithm", "AES-CCM-16-64-128"},
                };
                res.set_content(boost::json::serialize(response), "application/json");
            }
        });

        _actual_port = _server.bind_to_any_port("127.0.0.1");
        BOOST_REQUIRE_MESSAGE(_actual_port > 0, "mock AS failed to bind");
        _thread = std::jthread([this](std::stop_token) { _server.listen_after_bind(); });

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!_server.is_running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~mock_authorization_server() { _server.stop(); }

    [[nodiscard]] auto token_endpoint() const -> std::string {
        return "http://127.0.0.1:" + std::to_string(_actual_port) + "/token";
    }

private:
    httplib::Server _server;
    int _actual_port{0};
    std::jthread _thread;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_ace_oauth_tests)

BOOST_AUTO_TEST_CASE(dtls_profile_populates_psk_credentials, *boost::unit_test::timeout(15)) {
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

BOOST_AUTO_TEST_CASE(oscore_profile_populates_oscore_credentials, *boost::unit_test::timeout(15)) {
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

BOOST_AUTO_TEST_CASE(denied_scope_throws_bootstrap_error, *boost::unit_test::timeout(15)) {
    mock_authorization_server as;
    ace_oauth_config config;
    config.as_token_endpoint = as.token_endpoint();
    config.client_id = "node-1";
    config.client_secret = "secret";
    config.scope = "deny-me";
    config.target_profile = ace_target_profile::dtls_psk;

    BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_credential_bootstrap_error);
}

BOOST_AUTO_TEST_CASE(malformed_response_throws_bootstrap_error, *boost::unit_test::timeout(15)) {
    mock_authorization_server as;
    ace_oauth_config config;
    config.as_token_endpoint = as.token_endpoint();
    config.client_id = "node-1";
    config.client_secret = "secret";
    config.scope = "malformed";
    config.target_profile = ace_target_profile::dtls_psk;

    BOOST_CHECK_THROW(run_ace_token_exchange(config), coap_credential_bootstrap_error);
}

BOOST_AUTO_TEST_CASE(unreachable_as_throws_bootstrap_error, *boost::unit_test::timeout(15)) {
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
