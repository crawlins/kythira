// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The core HTTPS server the AWS-compatible front ends share
// (include/raft/beast_https_server.hpp, .kiro/specs/iam-compatible-auth/
// task 9): request handling and keep-alive, optional client certificates,
// the size and connection limits, and shutdown with connections open.

#define BOOST_TEST_MODULE beast_https_server_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_https_server.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/private_file.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using namespace std::chrono_literals;
using kythira::https::request;
using kythira::https::request_context;
using kythira::https::response;

struct pki {
    std::filesystem::path dir;
    std::string ca_path;
    std::string server_cert;
    std::string server_key;
    std::string client_cert;
    std::string client_key;
    std::string stranger_cert;  // valid, but from a CA the server does not trust
    std::string stranger_key;

    pki() {
        dir = raft::make_private_temp_directory(std::filesystem::temp_directory_path(),
                                                "https-server-test-");
        raft::testing::certificate_authority ca;
        raft::testing::certificate_authority other;
        auto write = [&](const std::string& name, const std::string& pem) {
            const auto p = (dir / name).string();
            raft::write_private_file(p, pem);
            return p;
        };
        ca_path = write("ca.pem", ca.root_certificate_pem());
        raft::testing::leaf_certificate_options server;
        server.subject.common_name = "localhost";
        server.dns_names = {"localhost"};
        server.ip_addresses = {"127.0.0.1"};
        auto s = ca.issue(server);
        server_cert = write("server.pem", s.chain_pem);
        server_key = write("server.key", s.private_key_pem);

        raft::testing::leaf_certificate_options client;
        client.subject.common_name = "admin";
        client.uri_names = {"spiffe://kythira/admin/alice"};
        client.server_auth = false;
        client.client_auth = true;
        auto c = ca.issue(client);
        client_cert = write("client.pem", c.chain_pem);
        client_key = write("client.key", c.private_key_pem);

        auto x = other.issue(client);
        stranger_cert = write("stranger.pem", x.chain_pem);
        stranger_key = write("stranger.key", x.private_key_pem);
    }
    ~pki() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    pki(const pki&) = delete;
    auto operator=(const pki&) -> pki& = delete;
};

auto config_for(const pki& p) -> kythira::https::server_config {
    kythira::https::server_config cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.tls_cert_path = p.server_cert;
    cfg.tls_key_path = p.server_key;
    cfg.client_ca_path = p.ca_path;
    return cfg;
}

// Answers with what the server saw.
auto echo(const request& req, const request_context& ctx) -> response {
    response res{http::status::ok, req.version()};
    std::string body = std::string(req.method_string()) + " " + std::string(req.target()) +
                       " tls=" + (ctx.tls ? "1" : "0") +
                       " verified=" + (ctx.client_verified ? "1" : "0") +
                       " peer=" + ctx.peer_address;
    for (const auto& u : ctx.client_uri_sans) {
        body += " uri=" + u;
    }
    body += " body=" + req.body();
    res.body() = body;
    res.keep_alive(req.keep_alive());
    return res;
}

struct client_options {
    bool tls = true;
    std::string cert{};
    std::string key{};
    std::string ca{};
};

// One TLS (or plaintext) connection that can send several requests.
class client {
public:
    client(std::uint16_t port, const client_options& o) : _ctx(ssl::context::tls_client) {
        if (!o.ca.empty()) {
            _ctx.load_verify_file(o.ca);
            _ctx.set_verify_mode(ssl::verify_peer);
        }
        if (!o.cert.empty()) {
            _ctx.use_certificate_chain_file(o.cert);
            _ctx.use_private_key_file(o.key, ssl::context::pem);
        }
        asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), port);
        if (o.tls) {
            _tls.emplace(_ioc, _ctx);
            beast::get_lowest_layer(*_tls).expires_after(5s);
            beast::get_lowest_layer(*_tls).connect(ep);
            SSL_set_tlsext_host_name(_tls->native_handle(), "localhost");
            _tls->handshake(ssl::stream_base::client);
        } else {
            _plain.emplace(_ioc);
            _plain->expires_after(5s);
            _plain->connect(ep);
        }
    }

    auto send(http::request<http::string_body> req) -> response {
        req.prepare_payload();
        response res;
        if (_tls) {
            beast::get_lowest_layer(*_tls).expires_after(5s);
            http::write(*_tls, req);
            http::read(*_tls, _buf, res);
        } else {
            _plain->expires_after(5s);
            http::write(*_plain, req);
            http::read(*_plain, _buf, res);
        }
        return res;
    }

    auto get(const std::string& target) -> response {
        http::request<http::string_body> req{http::verb::get, target, 11};
        req.set(http::field::host, "localhost");
        return send(std::move(req));
    }

private:
    asio::io_context _ioc;
    ssl::context _ctx;
    std::optional<beast::ssl_stream<beast::tcp_stream>> _tls;
    std::optional<beast::tcp_stream> _plain;
    beast::flat_buffer _buf;
};

}  // namespace

BOOST_AUTO_TEST_CASE(tls_request_without_client_certificate) {
    pki p;
    kythira::https::server srv(config_for(p), echo);
    srv.start();
    client c(srv.port(), {.ca = p.ca_path});
    auto res = c.get("/a?b=c");
    BOOST_TEST(res.result_int() == 200U);
    BOOST_TEST(res.body().find("GET /a?b=c tls=1 verified=0 peer=127.0.0.1") == 0U);
    // Keep-alive: a second request on the same connection.
    auto again = c.get("/second");
    BOOST_TEST(again.body().find("GET /second") == 0U);
    BOOST_TEST(srv.stats().connections_accepted.load() == 1U);
}

BOOST_AUTO_TEST_CASE(verified_client_certificate_reports_uri_sans) {
    pki p;
    kythira::https::server srv(config_for(p), echo);
    srv.start();
    client c(srv.port(), {.cert = p.client_cert, .key = p.client_key, .ca = p.ca_path});
    auto res = c.get("/admin");
    BOOST_TEST(res.body().find("verified=1") != std::string::npos);
    BOOST_TEST(res.body().find("uri=spiffe://kythira/admin/alice") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(untrusted_client_certificate_fails_the_handshake) {
    pki p;
    kythira::https::server srv(config_for(p), echo);
    srv.start();
    bool failed = false;
    try {
        client c(srv.port(), {.cert = p.stranger_cert, .key = p.stranger_key, .ca = p.ca_path});
        // TLS 1.3 reports the client-certificate rejection on the first read.
        (void)c.get("/admin");
    } catch (const std::exception&) {
        failed = true;
    }
    BOOST_TEST(failed);
    BOOST_TEST(srv.stats().requests.load() == 0U);
}

BOOST_AUTO_TEST_CASE(without_client_ca_a_presented_certificate_is_not_verified) {
    pki p;
    auto cfg = config_for(p);
    cfg.client_ca_path.clear();
    kythira::https::server srv(cfg, echo);
    srv.start();
    client c(srv.port(), {.cert = p.client_cert, .key = p.client_key, .ca = p.ca_path});
    auto res = c.get("/x");
    BOOST_TEST(res.body().find("verified=0") != std::string::npos);
    BOOST_TEST(res.body().find("uri=") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(plaintext_only_when_asked) {
    pki p;
    auto cfg = config_for(p);
    cfg.tls_cert_path.clear();
    cfg.tls_key_path.clear();
    {
        kythira::https::server srv(cfg, echo);
        BOOST_CHECK_THROW(srv.start(), std::invalid_argument);
    }
    cfg.insecure_plaintext = true;
    std::string warned;
    kythira::https::server srv(cfg, echo, [&](bool warning, std::string_view m) {
        if (warning) {
            warned += std::string(m);
        }
    });
    srv.start();
    BOOST_TEST(warned.find("PLAINTEXT") != std::string::npos);
    client c(srv.port(), {.tls = false});
    auto res = c.get("/p");
    BOOST_TEST(res.body().find("tls=0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(body_and_header_limits) {
    pki p;
    auto cfg = config_for(p);
    cfg.max_body_bytes = 100;
    cfg.max_header_bytes = 1024;
    kythira::https::server srv(cfg, echo);
    srv.start();
    {
        client c(srv.port(), {.ca = p.ca_path});
        http::request<http::string_body> req{http::verb::post, "/", 11};
        req.set(http::field::host, "localhost");
        req.body() = std::string(100, 'a');
        auto ok = c.send(req);
        BOOST_TEST(ok.result_int() == 200U);
        req.body() = std::string(101, 'a');
        auto res = c.send(req);
        BOOST_TEST(res.result_int() == 413U);
    }
    {
        client c(srv.port(), {.ca = p.ca_path});
        http::request<http::string_body> req{http::verb::get, "/", 11};
        req.set(http::field::host, "localhost");
        req.set("X-Big", std::string(2000, 'b'));
        auto res = c.send(req);
        BOOST_TEST(res.result_int() == 431U);
    }
}

BOOST_AUTO_TEST_CASE(handler_exception_answers_500_and_closes) {
    pki p;
    kythira::https::server srv(config_for(p),
                               [](const request&, const request_context&) -> response {
                                   throw std::runtime_error("boom");
                               });
    srv.start();
    client c(srv.port(), {.ca = p.ca_path});
    auto res = c.get("/");
    BOOST_TEST(res.result_int() == 500U);
    BOOST_TEST(!res.keep_alive());
}

BOOST_AUTO_TEST_CASE(connection_cap_closes_extra_connections) {
    pki p;
    auto cfg = config_for(p);
    cfg.max_connections = 1;
    kythira::https::server srv(cfg, echo);
    srv.start();
    client first(srv.port(), {.ca = p.ca_path});
    BOOST_TEST(first.get("/1").result_int() == 200U);
    bool refused = false;
    try {
        client second(srv.port(), {.ca = p.ca_path});
        (void)second.get("/2");
    } catch (const std::exception&) {
        refused = true;
    }
    BOOST_TEST(refused);
    BOOST_TEST(srv.stats().connections_rejected.load() >= 1U);
    // The first connection is unaffected.
    BOOST_TEST(first.get("/3").result_int() == 200U);
}

BOOST_AUTO_TEST_CASE(stop_ends_open_connections_promptly) {
    pki p;
    auto cfg = config_for(p);
    cfg.idle_timeout = std::chrono::seconds{60};
    auto srv = std::make_unique<kythira::https::server>(cfg, echo);
    srv->start();
    client c(srv->port(), {.ca = p.ca_path});
    BOOST_TEST(c.get("/").result_int() == 200U);
    const auto t0 = std::chrono::steady_clock::now();
    srv->stop();
    const bool prompt = std::chrono::steady_clock::now() - t0 < 5s;
    BOOST_TEST(prompt);
}
