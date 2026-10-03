// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A mock ACE-OAuth Authorization Server for the CoAP ACE tests, shared by
// every backend's suite so there is one in the tree.

#include <boost/test/unit_test.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <chrono>
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

}  // namespace kythira::testing
