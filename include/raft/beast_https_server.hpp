// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file beast_https_server.hpp
/// @brief A small Boost.Beast HTTPS server for the AWS-compatible front ends:
///        IAM and STS now (`.kiro/specs/iam-compatible-auth/` design
///        Component 7), and Kinesis, DynamoDB and S3 later. Modules may not
///        include each other, so it lives in core.
///
/// It follows `cache_http_front_end` (services/redis/), whose limits and
/// TLS setup it shares:
///
///  - One thread per connection, each with its own `io_context`. Every
///    socket operation is asynchronous with a Beast deadline and is driven to
///    completion before the thread moves on, so a handler reads like
///    blocking code while every read and write still has a timeout. A
///    handler may block on a Raft proposal; that holds only its own
///    connection.
///  - The `http-server-request-limits` connection gate caps the threads. A
///    connection over the cap is closed before its handshake.
///  - Header size, body size, and header, idle, I/O and handshake timeouts.
///  - TLS 1.2 or later. Plaintext only with `insecure_plaintext`, which
///    logs a warning every start (iam-compatible-auth Requirement 14.1).
///
/// **Client certificates.** With `client_ca_path` set, the server asks for a
/// client certificate but does not require one: SigV4 clients present none,
/// while admin tools authenticate with one. OpenSSL verifies a presented
/// certificate against that bundle, and a certificate that fails
/// verification fails the handshake. The handler sees whether a verified
/// certificate was presented and its URI SANs, and decides what that
/// identity may do.
///
/// Whole bodies are read into memory, up to `max_body_bytes`: the Query and
/// JSON protocols this serves carry small bodies. A streaming handler for
/// S3 objects would be a separate entry point.

#include <raft/asio_listeners.hpp>
#include <raft/http_connection_gate.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <sys/socket.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::https {

struct server_config {
    /// What to listen on: an IP literal, "*" (IPv4 and IPv6), or an
    /// /etc/hosts name of this host (net_bind.hpp).
    std::string bind_address{"0.0.0.0"};
    /// 0 picks a free port; `server::port()` reports it.
    std::uint16_t port{0};
    std::string tls_cert_path;  // chain, leaf first
    std::string tls_key_path;
    /// When set, clients may present a certificate chained to this bundle.
    std::string client_ca_path;
    /// Serve plaintext HTTP instead of HTTPS. For tests only; start() logs a
    /// warning every time.
    bool insecure_plaintext{false};

    std::size_t max_connections{256};
    std::uint32_t max_header_bytes{16 * 1024};
    std::uint64_t max_body_bytes{std::uint64_t{1024} * 1024};
    std::chrono::seconds header_timeout{10};
    std::chrono::seconds idle_timeout{60};
    std::chrono::seconds io_timeout{60};
    std::chrono::seconds handshake_timeout{10};
};

/// What the server knows about who sent a request.
struct request_context {
    /// The remote address as text, without a port: what a rate limiter
    /// charges.
    std::string peer_address;
    bool tls{false};
    /// True when the client presented a certificate that verified against
    /// `client_ca_path`.
    bool client_verified{false};
    /// The verified certificate's URI SANs; empty when none was presented.
    std::vector<std::string> client_uri_sans;
};

using request = boost::beast::http::request<boost::beast::http::string_body>;
using response = boost::beast::http::response<boost::beast::http::string_body>;
/// Answers one request. Runs on the connection's thread. An exception
/// closes the connection with a 500.
using handler = std::function<response(const request&, const request_context&)>;
/// Receives the server's own log lines: (is_warning, message).
using log_sink = std::function<void(bool, std::string_view)>;

struct server_stats {
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> connections_accepted{0};
    std::atomic<std::uint64_t> connections_rejected{0};
    std::atomic<std::uint64_t> handshake_failures{0};
};

namespace detail {

namespace asio = boost::asio;
namespace beast = boost::beast;

/// Runs one asynchronous operation on a connection's private `io_context`
/// to completion. Every operation passed here has a Beast deadline, so this
/// cannot block forever.
template<typename Start> auto drive(asio::io_context& ioc, Start&& start) -> beast::error_code {
    beast::error_code result = asio::error::would_block;
    bool done = false;
    std::forward<Start>(start)([&result, &done](beast::error_code ec, auto&&...) {
        result = ec;
        done = true;
    });
    ioc.restart();
    while (!done && ioc.run_one() != 0) {
    }
    return done ? result : beast::error_code(asio::error::operation_aborted);
}

template<typename Stream> auto lowest(Stream& s) -> beast::tcp_stream& {
    if constexpr (std::is_same_v<Stream, beast::tcp_stream>) {
        return s;
    } else {
        return beast::get_lowest_layer(s);
    }
}

/// The URI SANs of `cert`.
inline auto uri_sans(X509* cert) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (cert == nullptr) {
        return out;
    }
    auto* names =
        static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (names == nullptr) {
        return out;
    }
    for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
        const GENERAL_NAME* gn = sk_GENERAL_NAME_value(names, i);
        if (gn->type != GEN_URI) {
            continue;
        }
        const ASN1_STRING* s = gn->d.uniformResourceIdentifier;
        out.emplace_back(reinterpret_cast<const char*>(ASN1_STRING_get0_data(s)),
                         static_cast<std::size_t>(ASN1_STRING_length(s)));
    }
    GENERAL_NAMES_free(names);
    return out;
}

inline auto plain_reply(unsigned status, std::string_view body) -> response {
    response res{static_cast<boost::beast::http::status>(status), 11};
    res.set(boost::beast::http::field::content_type, "text/plain");
    res.keep_alive(false);
    res.body() = std::string(body);
    res.prepare_payload();
    return res;
}

}  // namespace detail

class server {
public:
    server(server_config config, handler on_request, log_sink log = {})
        : _config(std::move(config)), _handler(std::move(on_request)), _log(std::move(log)) {}

    server(const server&) = delete;
    auto operator=(const server&) -> server& = delete;
    server(server&&) = delete;
    auto operator=(server&&) -> server& = delete;
    ~server() { stop(); }

    /// Binds and starts accepting. Throws on a bad configuration or a bind
    /// failure.
    void start() {
        namespace ssl = boost::asio::ssl;
        if (_running.load()) {
            return;
        }
        if (!_handler) {
            throw std::invalid_argument("https server: no request handler");
        }
        if (_config.insecure_plaintext) {
            warn(
                "https server: PLAINTEXT listener (--insecure-plaintext); credentials and "
                "signatures cross the network unencrypted. For tests only.");
        } else {
            if (_config.tls_cert_path.empty() || _config.tls_key_path.empty()) {
                throw std::invalid_argument("https server: a certificate and key are required");
            }
            _ssl_ctx = std::make_shared<ssl::context>(ssl::context::tls_server);
            _ssl_ctx->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                                  ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                                  ssl::context::no_tlsv1_1);
            SSL_CTX_set_min_proto_version(_ssl_ctx->native_handle(), TLS1_2_VERSION);
            _ssl_ctx->use_certificate_chain_file(_config.tls_cert_path);
            _ssl_ctx->use_private_key_file(_config.tls_key_path, ssl::context::pem);
            if (!_config.client_ca_path.empty()) {
                _ssl_ctx->load_verify_file(_config.client_ca_path);
                // Ask, do not require: verify_peer without
                // verify_fail_if_no_peer_cert.
                _ssl_ctx->set_verify_mode(ssl::verify_peer);
                // Session resumption would skip the client-certificate
                // check on a resumed connection's context: keep it simple
                // and off.
                SSL_CTX_set_session_cache_mode(_ssl_ctx->native_handle(), SSL_SESS_CACHE_OFF);
                SSL_CTX_set_options(_ssl_ctx->native_handle(), SSL_OP_NO_TICKET);
                static constexpr std::string_view sid = "kythira-https";
                SSL_CTX_set_session_id_context(_ssl_ctx->native_handle(),
                                               reinterpret_cast<const unsigned char*>(sid.data()),
                                               static_cast<unsigned int>(sid.size()));
            }
        }
        _gate = std::make_shared<http_detail::connection_gate>(_config.max_connections);
        _acceptors = net_bind::open_asio_acceptors(_accept_ioc, _config.bind_address, _config.port,
                                                   "https server");
        _port = _acceptors.front()->local_endpoint().port();
        _running = true;
        for (const auto& a : _acceptors) {
            _accept_threads.emplace_back([this, a] { accept_loop(a); });
        }
    }

    /// Stops accepting, ends every connection and joins every thread.
    void stop() {
        if (!_running.exchange(false)) {
            return;
        }
        for (const auto& a : _acceptors) {
            // Wakes the blocking accept(); the acceptor is closed only after
            // its thread is joined.
            (void)::shutdown(a->native_handle(), SHUT_RDWR);
        }
        for (auto& t : _accept_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        for (const auto& a : _acceptors) {
            boost::system::error_code ec;
            (void)a->close(ec);
        }
        _accept_threads.clear();
        _acceptors.clear();
        std::list<std::shared_ptr<connection>> live;
        {
            const std::lock_guard lock(_connections_mu);
            live.swap(_connections);
        }
        for (const auto& c : live) {
            const std::lock_guard lock(c->mu);
            if (c->fd >= 0) {
                (void)::shutdown(c->fd, SHUT_RDWR);
            }
        }
        for (const auto& c : live) {
            if (c->thread.joinable()) {
                c->thread.join();
            }
        }
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t { return _port; }
    [[nodiscard]] auto stats() const noexcept -> const server_stats& { return _stats; }

private:
    using tcp = boost::asio::ip::tcp;
    using plain_stream = boost::beast::tcp_stream;
    using tls_stream = boost::beast::ssl_stream<boost::beast::tcp_stream>;

    struct connection {
        boost::asio::io_context ioc{1};
        std::thread thread;
        std::atomic<bool> done{false};
        std::mutex mu;
        int fd = -1;  // for stop(); -1 once the stream no longer owns it
        http_detail::connection_gate::slot slot;
    };

    void warn(std::string_view m) const {
        if (_log) {
            _log(true, m);
        }
    }
    void debug(std::string_view m) const {
        if (_log) {
            _log(false, m);
        }
    }

    void accept_loop(const std::shared_ptr<tcp::acceptor>& acceptor) {
        while (_running.load()) {
            boost::system::error_code ec;
            tcp::socket socket(_accept_ioc);
            (void)acceptor->accept(socket, ec);
            if (ec) {
                if (!_running.load() || ec == boost::asio::error::bad_descriptor ||
                    ec == boost::asio::error::operation_aborted) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                continue;
            }
            reap();
            auto slot = _gate->try_acquire();
            if (!slot) {
                ++_stats.connections_rejected;
                (void)socket.close(ec);
                continue;
            }
            ++_stats.connections_accepted;
            auto c = std::make_shared<connection>();
            c->slot = std::move(slot);
            {
                const std::lock_guard lock(_connections_mu);
                if (!_running.load()) {
                    return;
                }
                _connections.push_back(c);
            }
            c->thread =
                std::thread([this, c, s = std::move(socket)]() mutable { serve(c, std::move(s)); });
        }
    }

    void reap() {
        std::list<std::shared_ptr<connection>> finished;
        {
            const std::lock_guard lock(_connections_mu);
            for (auto it = _connections.begin(); it != _connections.end();) {
                if ((*it)->done.load()) {
                    finished.push_back(*it);
                    it = _connections.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& c : finished) {
            if (c->thread.joinable()) {
                c->thread.join();
            }
        }
    }

    void serve(const std::shared_ptr<connection>& c, tcp::socket socket) {
        try {
            boost::system::error_code ec;
            const auto protocol = socket.local_endpoint(ec).protocol();
            tcp::socket mine(c->ioc, protocol, socket.release());
            {
                const std::lock_guard lock(c->mu);
                c->fd = mine.native_handle();
            }
            auto forget = [&c] {
                const std::lock_guard lock(c->mu);
                c->fd = -1;
            };
            try {
                if (_ssl_ctx) {
                    tls_stream stream(plain_stream(std::move(mine)), *_ssl_ctx);
                    serve_stream(*c, stream);
                } else {
                    plain_stream stream(std::move(mine));
                    serve_stream(*c, stream);
                }
            } catch (...) {
                forget();
                throw;
            }
            forget();
        } catch (const std::exception& e) {
            debug(std::string("https server: connection ended: ") + e.what());
        }
        c->slot.release();
        c->done = true;
    }

    template<typename Stream> void serve_stream(connection& c, Stream& stream) {
        namespace http = boost::beast::http;
        using detail::drive;
        using detail::lowest;
        request_context ctx;
        {
            boost::system::error_code ec;
            auto ep = lowest(stream).socket().remote_endpoint(ec);
            if (!ec) {
                auto addr = ep.address();
                if (addr.is_v6() && addr.to_v6().is_v4_mapped()) {
                    addr =
                        boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, addr.to_v6());
                }
                ctx.peer_address = addr.to_string();
            }
        }
        if constexpr (!std::is_same_v<Stream, plain_stream>) {
            ctx.tls = true;
            lowest(stream).expires_after(_config.handshake_timeout);
            auto ec = drive(c.ioc, [&](auto h) {
                stream.async_handshake(boost::asio::ssl::stream_base::server, std::move(h));
            });
            if (ec) {
                ++_stats.handshake_failures;
                return;
            }
            SSL* ssl = stream.native_handle();
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
            X509* peer = SSL_get1_peer_certificate(ssl);
#else
            X509* peer = SSL_get_peer_certificate(ssl);
#endif
            if (peer != nullptr) {
                ctx.client_verified = SSL_get_verify_result(ssl) == X509_V_OK;
                if (ctx.client_verified) {
                    ctx.client_uri_sans = detail::uri_sans(peer);
                }
                X509_free(peer);
            }
        }
        boost::beast::flat_buffer buffer;
        bool first = true;
        while (_running.load()) {
            http::request_parser<http::string_body> parser;
            parser.header_limit(_config.max_header_bytes);
            parser.body_limit(_config.max_body_bytes);
            lowest(stream).expires_after(first ? _config.header_timeout : _config.idle_timeout);
            first = false;
            auto ec = drive(c.ioc, [&](auto h) {
                http::async_read_header(stream, buffer, parser, std::move(h));
            });
            if (ec == http::error::header_limit) {
                (void)write(c, stream, detail::plain_reply(431, "request header too large\n"));
                break;
            }
            if (ec == http::error::body_limit) {
                (void)write(c, stream, detail::plain_reply(413, "request body too large\n"));
                break;
            }
            if (ec) {
                break;  // closed, timed out, or not HTTP
            }
            if (!parser.is_done()) {
                lowest(stream).expires_after(_config.io_timeout);
                ec = drive(c.ioc,
                           [&](auto h) { http::async_read(stream, buffer, parser, std::move(h)); });
                if (ec == http::error::body_limit) {
                    (void)write(c, stream, detail::plain_reply(413, "request body too large\n"));
                    break;
                }
                if (ec) {
                    break;
                }
            }
            ++_stats.requests;
            const auto& req = parser.get();
            response res;
            bool failed = false;
            try {
                res = _handler(req, ctx);
            } catch (const std::exception& e) {
                warn(std::string("https server: handler failed: ") + e.what());
                res = detail::plain_reply(500, "internal error\n");
                failed = true;
            }
            res.version(req.version());
            if (failed || !req.keep_alive()) {
                res.keep_alive(false);
            }
            res.prepare_payload();
            const bool keep = res.keep_alive();
            if (!write(c, stream, std::move(res)) || !keep) {
                break;
            }
        }
        lowest(stream).expires_after(std::chrono::seconds{1});
        if constexpr (!std::is_same_v<Stream, plain_stream>) {
            (void)drive(c.ioc, [&](auto h) { stream.async_shutdown(std::move(h)); });
        }
        boost::system::error_code ignored;
        (void)lowest(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
    }

    template<typename Stream> auto write(connection& c, Stream& stream, response res) -> bool {
        namespace http = boost::beast::http;
        detail::lowest(stream).expires_after(_config.io_timeout);
        res.prepare_payload();
        auto ec =
            detail::drive(c.ioc, [&](auto h) { http::async_write(stream, res, std::move(h)); });
        return !ec;
    }

    server_config _config;
    handler _handler;
    log_sink _log;
    server_stats _stats;
    std::atomic<bool> _running{false};
    std::uint16_t _port{0};
    boost::asio::io_context _accept_ioc;
    std::shared_ptr<boost::asio::ssl::context> _ssl_ctx;
    std::shared_ptr<http_detail::connection_gate> _gate;
    std::vector<std::shared_ptr<tcp::acceptor>> _acceptors;
    std::vector<std::thread> _accept_threads;
    std::mutex _connections_mu;
    std::list<std::shared_ptr<connection>> _connections;
};

}  // namespace kythira::https
