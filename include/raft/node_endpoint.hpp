// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file node_endpoint.hpp
/// @brief The node's one health and metrics listener
///        (.kiro/specs/service-colocation/ task 2, Requirement 2.2):
///        `GET /healthz` and `GET /metrics` for every module a
///        `node_runner` runs, over core's Beast server
///        (`beast_https_server.hpp`). It uses the node's certificate, or
///        plaintext with `KYTHIRA_NODE_INSECURE_PLAINTEXT=1`.
///
/// Separate from `node_core.hpp` so a build that only plans and runs
/// modules needs no Boost.Beast or OpenSSL.

#include <raft/beast_https_server.hpp>
#include <raft/node_core.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace kythira::service_node {

template<typename Host> class node_endpoint {
public:
    node_endpoint(const node_runner<Host>& runner, log_sink log = {})
        : _runner(runner), _log(std::move(log)) {}

    node_endpoint(const node_endpoint&) = delete;
    auto operator=(const node_endpoint&) -> node_endpoint& = delete;
    node_endpoint(node_endpoint&&) = delete;
    auto operator=(node_endpoint&&) -> node_endpoint& = delete;
    ~node_endpoint() { stop(); }

    /// Binds and serves. @throws on a bind or TLS failure.
    void start() {
        const auto& c = _runner.config();
        https::server_config sc;
        sc.bind_address = c.metrics_bind;
        sc.port = c.metrics_port;
        sc.insecure_plaintext = c.insecure_plaintext;
        sc.tls_cert_path = c.tls_cert;
        sc.tls_key_path = c.tls_key;
        sc.max_connections = 16;
        sc.max_body_bytes = 1024;
        _server = std::make_unique<https::server>(
            sc,
            [this](const https::request& req, const https::request_context&) {
                return answer(req);
            },
            [log = _log](bool warning, std::string_view line) {
                if (log) {
                    log(warning, "service=node " + std::string(line));
                }
            });
        _server->start();
    }

    void stop() {
        if (_server) {
            _server->stop();
            _server.reset();
        }
    }

    [[nodiscard]] auto port() const -> std::uint16_t { return _server ? _server->port() : 0; }

private:
    [[nodiscard]] auto answer(const https::request& req) const -> https::response {
        namespace http = boost::beast::http;
        https::response res;
        res.version(req.version());
        res.keep_alive(false);
        const auto target = std::string_view(req.target().data(), req.target().size());
        const auto path = target.substr(0, target.find('?'));
        if (req.method() != http::verb::get && req.method() != http::verb::head) {
            res.result(http::status::method_not_allowed);
            res.set(http::field::allow, "GET, HEAD");
        } else if (path == "/healthz") {
            auto [status, body] = _runner.health_report();
            res.result(status);
            res.set(http::field::content_type, "application/json");
            res.body() = std::move(body);
        } else if (path == "/metrics") {
            res.result(http::status::ok);
            res.set(http::field::content_type, "text/plain; version=0.0.4");
            res.body() = _runner.metrics_text();
        } else {
            res.result(http::status::not_found);
        }
        if (req.method() == http::verb::head) {
            const auto size = res.body().size();
            res.body().clear();
            res.content_length(size);
        } else {
            res.prepare_payload();
        }
        return res;
    }

    const node_runner<Host>& _runner;
    log_sink _log;
    std::unique_ptr<https::server> _server;
};

}  // namespace kythira::service_node
