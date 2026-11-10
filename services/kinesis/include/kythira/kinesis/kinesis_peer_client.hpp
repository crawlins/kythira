// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_peer_client.hpp
/// @brief One blocking HTTP(S) request to another gateway of the cluster
///        (`.kiro/specs/kinesis-compatible-streams/` task 6): how a write
///        reaches its group's leader and how a replica fetches a segment.
///
/// Each call dials, sends one request, reads one response and closes. The
/// calls are rare next to client traffic (one per forwarded write, one per
/// missing segment), so a connection pool is not worth its failure modes
/// yet. Over TLS the peer's certificate must chain to `_ca_path` (the
/// system store when empty) and name the host dialled. Every failure (no
/// route, refused, timeout, bad certificate, unparseable response) is
/// nullopt: the callers treat a peer they cannot reach as unavailable.

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/ssl.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::kinesis {

struct peer_client_config {
    /// HTTPS when true; plain HTTP only where the operator allowed
    /// plaintext (KINESIS_ALLOW_PLAINTEXT).
    bool _tls = true;
    /// CA bundle the peers' certificates chain to; empty uses the system
    /// store.
    std::string _ca_path;
    /// For the whole exchange: dial, write, read.
    std::chrono::milliseconds _timeout{5000};
    /// The largest response body read.
    std::uint64_t _max_body_bytes = std::uint64_t{64} * 1024 * 1024;
};

struct peer_response {
    unsigned _status = 0;
    std::string _body;
};

/// "host:port" or "[v6]:port" split into host and port; nullopt when
/// malformed.
[[nodiscard]] inline auto split_host_port(std::string_view hp)
    -> std::optional<std::pair<std::string, std::string>> {
    std::string_view host;
    std::string_view rest;
    if (hp.starts_with('[')) {
        const auto close = hp.find(']');
        if (close == std::string_view::npos || close + 1 >= hp.size() || hp[close + 1] != ':') {
            return std::nullopt;
        }
        host = hp.substr(1, close - 1);
        rest = hp.substr(close + 2);
    } else {
        const auto colon = hp.rfind(':');
        if (colon == std::string_view::npos || hp.find(':') != colon) {
            return std::nullopt;
        }
        host = hp.substr(0, colon);
        rest = hp.substr(colon + 1);
    }
    if (host.empty() || rest.empty() || rest.size() > 5) {
        return std::nullopt;
    }
    unsigned port = 0;
    for (const char c : rest) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        port = port * 10 + static_cast<unsigned>(c - '0');
    }
    if (port == 0 || port > 65535) {
        return std::nullopt;
    }
    return std::pair{std::string(host), std::string(rest)};
}

class peer_client {
public:
    using headers = std::vector<std::pair<std::string, std::string>>;

    explicit peer_client(peer_client_config config) : _config(std::move(config)) {}

    [[nodiscard]] auto config() const noexcept -> const peer_client_config& { return _config; }

    /// `method` `target` on the gateway at `host_port`.
    [[nodiscard]] auto request(std::string_view host_port, boost::beast::http::verb method,
                               std::string_view target, const headers& extra,
                               std::string body = {}) const -> std::optional<peer_response> {
        const auto hp = split_host_port(host_port);
        if (!hp) {
            return std::nullopt;
        }
        try {
            namespace http = boost::beast::http;
            http::request<http::string_body> req{method, std::string(target), 11};
            req.set(http::field::host, std::string(host_port));
            req.set(http::field::user_agent, "kythira-kinesis-gateway");
            for (const auto& [k, v] : extra) {
                req.set(k, v);
            }
            req.body() = std::move(body);
            req.prepare_payload();
            return _config._tls ? exchange_tls(hp->first, hp->second, req)
                                : exchange_plain(hp->first, hp->second, req);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

private:
    peer_client_config _config;

    template<typename Stream, typename Request>
    [[nodiscard]] auto send(Stream& stream, const Request& req) const
        -> std::optional<peer_response> {
        namespace http = boost::beast::http;
        boost::beast::error_code ec;
        http::write(stream, req, ec);
        if (ec) {
            return std::nullopt;
        }
        boost::beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.body_limit(_config._max_body_bytes);
        http::read(stream, buffer, parser, ec);
        if (ec) {
            return std::nullopt;
        }
        auto res = parser.release();
        return peer_response{._status = res.result_int(), ._body = std::move(res.body())};
    }

    template<typename Request>
    [[nodiscard]] auto exchange_plain(const std::string& host, const std::string& port,
                                      const Request& req) const -> std::optional<peer_response> {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::resolver resolver(ioc);
        boost::beast::tcp_stream stream(ioc);
        stream.expires_after(_config._timeout);
        boost::beast::error_code ec;
        const auto endpoints = resolver.resolve(host, port, ec);
        if (ec) {
            return std::nullopt;
        }
        stream.connect(endpoints, ec);
        if (ec) {
            return std::nullopt;
        }
        auto out = send(stream, req);
        (void)stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        return out;
    }

    template<typename Request>
    [[nodiscard]] auto exchange_tls(const std::string& host, const std::string& port,
                                    const Request& req) const -> std::optional<peer_response> {
        namespace ssl = boost::asio::ssl;
        ssl::context ctx(ssl::context::tls_client);
        ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                        ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
        if (_config._ca_path.empty()) {
            ctx.set_default_verify_paths();
        } else {
            ctx.load_verify_file(_config._ca_path);
        }
        ctx.set_verify_mode(ssl::verify_peer);
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::resolver resolver(ioc);
        boost::beast::ssl_stream<boost::beast::tcp_stream> stream(ioc, ctx);
        // SNI and the name the certificate must carry (an IP literal is
        // checked against its IP SANs).
        if (SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()) != 1) {
            return std::nullopt;
        }
        stream.set_verify_callback(ssl::host_name_verification(host));
        auto& lowest = boost::beast::get_lowest_layer(stream);
        lowest.expires_after(_config._timeout);
        boost::beast::error_code ec;
        const auto endpoints = resolver.resolve(host, port, ec);
        if (ec) {
            return std::nullopt;
        }
        lowest.connect(endpoints, ec);
        if (ec) {
            return std::nullopt;
        }
        (void)stream.handshake(ssl::stream_base::client, ec);
        if (ec) {
            return std::nullopt;
        }
        auto out = send(stream, req);
        (void)stream.shutdown(ec);  // a peer that drops the connection is fine
        return out;
    }
};

}  // namespace kythira::kinesis
