// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A mock ACE-OAuth Authorization Server for the CoAP ACE tests, shared by
// every backend's suite so there is one in the tree.

#include <boost/test/unit_test.hpp>

#include <raft/coap_security.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace kythira::testing {

// A minimal mock AS: /token responds according to which case name is
// passed in the request's "scope" field, so a single server instance can
// drive every test case. A scope ending in "-responder" gets the mirror image
// of the OSCORE context every other scope gets (sender and recipient ids
// swapped), so a client and a server provisioned by this AS can talk; the
// PSK identity is the same for every scope with that suffix stripped, for the
// same reason.
//
// Plain http by default, on 127.0.0.1, so a config pointing at it needs
// ace_oauth_config::allow_plain_http_loopback (see insecure_config()). The
// cert/key constructor serves https instead.
class mock_authorization_server {
public:
    mock_authorization_server() : _server(std::make_unique<httplib::Server>()), _tls(false) {
        start();
    }

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    mock_authorization_server(const std::string& cert_path, const std::string& key_path)
        : _server(std::make_unique<httplib::SSLServer>(cert_path.c_str(), key_path.c_str())),
          _tls(true) {
        BOOST_REQUIRE_MESSAGE(_server->is_valid(), "mock AS failed to load its TLS cert/key");
        start();
    }
#endif

    mock_authorization_server(const mock_authorization_server&) = delete;
    auto operator=(const mock_authorization_server&) -> mock_authorization_server& = delete;

    ~mock_authorization_server() { _server->stop(); }

    [[nodiscard]] auto token_endpoint() const -> std::string {
        return std::string(_tls ? "https" : "http") +
               "://127.0.0.1:" + std::to_string(_actual_port) + "/token";
    }

    // A config aimed at this AS. Plain http needs the loopback opt-in; the
    // https variant leaves it off so the test proves https alone suffices.
    [[nodiscard]] auto config(std::string scope, ace_target_profile profile) const
        -> ace_oauth_config {
        ace_oauth_config config;
        config.as_token_endpoint = token_endpoint();
        config.client_id = "node-1";
        config.client_secret = "secret";
        config.scope = std::move(scope);
        config.target_profile = profile;
        config.allow_plain_http_loopback = !_tls;
        return config;
    }

    // Requests that reached /token: a refusal "before any network I/O"
    // leaves this at zero.
    [[nodiscard]] auto request_count() const -> int { return _requests.load(); }

private:
    void start() {
        _server->Post("/token", [this](const httplib::Request& req, httplib::Response& res) {
            ++_requests;
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

            constexpr std::string_view responder_suffix = "-responder";
            const bool responder = scope.ends_with(responder_suffix);
            if (responder) {
                scope.resize(scope.size() - responder_suffix.size());
            }

            if (profile == "coap_dtls") {
                boost::json::object response{
                    {"psk_identity", "issued-identity-" + scope},
                    {"psk_key_hex", "0102030405060708090a0b0c0d0e0f10"},
                };
                res.set_content(boost::json::serialize(response), "application/json");
            } else {
                boost::json::object response{
                    {"sender_id_hex", responder ? "01" : "00"},
                    {"recipient_id_hex", responder ? "00" : "01"},
                    {"master_secret_hex", "0102030405060708090a0b0c0d0e0f10"},
                    {"master_salt_hex", "0102030405060708"},
                    {"aead_algorithm", "AES-CCM-16-64-128"},
                };
                res.set_content(boost::json::serialize(response), "application/json");
            }
        });

        _actual_port = _server->bind_to_any_port("127.0.0.1");
        BOOST_REQUIRE_MESSAGE(_actual_port > 0, "mock AS failed to bind");
        _thread = std::jthread([this](std::stop_token) { _server->listen_after_bind(); });

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!_server->is_running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::unique_ptr<httplib::Server> _server;
    bool _tls;
    std::atomic<int> _requests{0};
    int _actual_port{0};
    std::jthread _thread;
};

}  // namespace kythira::testing
