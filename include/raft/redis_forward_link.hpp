// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file redis_forward_link.hpp
/// @brief One gateway-to-gateway forwarding connection, plaintext or TLS,
///        with a hard deadline on every operation
///        (.kiro/specs/redis-compatible-kv/ Requirements 12.5 and 13.4).
///
/// Forwarding is synchronous: a gateway worker sends the command and waits
/// for the reply. Asio's blocking socket calls cannot time out. A receive
/// timeout set with SO_RCVTIMEO only makes recv() return EAGAIN, which Asio
/// answers by polling the socket with no timeout at all, so a peer that
/// accepted the connection and then went quiet held a worker forever. A link
/// therefore owns a private io_context and runs one asynchronous operation at
/// a time on it with `run_until(deadline)`; an operation that misses the
/// deadline closes the socket and throws, and the link is never pooled again.
///
/// The link carries `AUTH <internal_user> <internal_secret>`, so it is TLS
/// unless the operator explicitly allowed plaintext. A TLS link verifies the
/// peer's certificate chain against the configured CA and checks that the
/// certificate names the host the endpoint was dialled by: a DNS SAN for a
/// host name, an IP SAN for an address literal.

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace kythira {

class redis_forward_link {
public:
    using tcp = boost::asio::ip::tcp;
    using clock = std::chrono::steady_clock;

    /// `tls` null means plaintext. A TLS context must already carry the CA,
    /// `verify_peer` and any client certificate.
    explicit redis_forward_link(std::shared_ptr<boost::asio::ssl::context> tls)
        : _tls_ctx(std::move(tls)) {
        if (_tls_ctx) {
            _tls.emplace(_io, *_tls_ctx);
        } else {
            _plain.emplace(_io);
        }
    }

    redis_forward_link(const redis_forward_link&) = delete;
    auto operator=(const redis_forward_link&) -> redis_forward_link& = delete;

    /// The point every following operation must finish by.
    auto set_deadline(clock::time_point deadline) noexcept -> void { _deadline = deadline; }

    /// Resolve, connect and, for TLS, handshake with `host:port`, all before
    /// the deadline. Throws on any failure, including a certificate that does
    /// not chain to the CA or does not name `host`.
    auto connect(const std::string& host, std::uint16_t port) -> void {
        tcp::resolver resolver(_io);
        tcp::resolver::results_type endpoints;
        run(
            [&](auto done) {
                resolver.async_resolve(host, std::to_string(port),
                                       [&endpoints, done](const boost::system::error_code& ec,
                                                          tcp::resolver::results_type results) {
                                           endpoints = std::move(results);
                                           done(ec, 0);
                                       });
            },
            [&resolver] { resolver.cancel(); });
        run([&](auto done) {
            boost::asio::async_connect(
                lowest(), endpoints,
                [done](const boost::system::error_code& ec, const tcp::endpoint&) { done(ec, 0); });
        });
        lowest().set_option(tcp::no_delay(true));
        if (!_tls) {
            return;
        }
        SSL* ssl = _tls->native_handle();
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        boost::system::error_code not_an_address;
        boost::asio::ip::make_address(host, not_an_address);
        if (!not_an_address) {
            if (X509_VERIFY_PARAM_set1_ip_asc(param, host.c_str()) != 1) {
                throw std::runtime_error("cannot verify peer gateway address " + host);
            }
        } else {
            if (X509_VERIFY_PARAM_set1_host(param, host.c_str(), host.size()) != 1) {
                throw std::runtime_error("cannot verify peer gateway host " + host);
            }
            // SNI only carries names, never addresses (RFC 6066 section 3).
            SSL_set_tlsext_host_name(ssl, host.c_str());
        }
        run([&](auto done) {
            _tls->async_handshake(boost::asio::ssl::stream_base::client,
                                  [done](const boost::system::error_code& ec) { done(ec, 0); });
        });
    }

    /// Write all of `bytes` before the deadline.
    auto write(boost::asio::const_buffer bytes) -> void {
        run([&](auto done) {
            if (_tls) {
                boost::asio::async_write(*_tls, bytes, done);
            } else {
                boost::asio::async_write(*_plain, bytes, done);
            }
        });
    }

    /// Read at least one byte before the deadline. EOF throws.
    auto read_some(boost::asio::mutable_buffer into) -> std::size_t {
        return run([&](auto done) {
            if (_tls) {
                _tls->async_read_some(into, done);
            } else {
                _plain->async_read_some(into, done);
            }
        });
    }

    [[nodiscard]] auto is_tls() const noexcept -> bool { return _tls.has_value(); }

    /// False once any operation failed or timed out: the stream may be
    /// mid-reply, so the link must not go back into a pool.
    [[nodiscard]] auto healthy() const noexcept -> bool { return _healthy; }

private:
    auto lowest() -> tcp::socket& { return _tls ? _tls->next_layer() : *_plain; }

    /// Start one asynchronous operation and drive the private io_context
    /// until it completes or the deadline passes. `start` receives the
    /// completion callback `done(error_code, bytes)`; `cancel` aborts the
    /// operation if it is still pending at the deadline (default: close the
    /// socket).
    template<typename Start> auto run(Start&& start) -> std::size_t {
        return run(std::forward<Start>(start), [this] {
            boost::system::error_code ignored;
            lowest().close(ignored);
        });
    }

    template<typename Start, typename Cancel>
    auto run(Start&& start, Cancel&& cancel) -> std::size_t {
        if (!_healthy) {
            throw std::runtime_error("peer gateway link already failed");
        }
        boost::system::error_code result = boost::asio::error::would_block;
        std::size_t bytes = 0;
        bool finished = false;
        auto done = [&result, &bytes, &finished](const boost::system::error_code& ec,
                                                 std::size_t n) {
            result = ec;
            bytes = n;
            finished = true;
        };
        std::forward<Start>(start)(done);
        _io.restart();
        _io.run_until(_deadline);
        if (!finished) {
            _healthy = false;
            // Abort the pending operation and run its handler now, so nothing
            // later touches this frame's locals.
            std::forward<Cancel>(cancel)();
            boost::system::error_code ignored;
            lowest().close(ignored);
            _io.restart();
            _io.run();
            throw std::runtime_error("peer gateway did not answer before the deadline");
        }
        if (result) {
            _healthy = false;
            throw boost::system::system_error(result);
        }
        return bytes;
    }

    boost::asio::io_context _io;
    std::shared_ptr<boost::asio::ssl::context> _tls_ctx;
    std::optional<tcp::socket> _plain;
    std::optional<boost::asio::ssl::stream<tcp::socket>> _tls;
    clock::time_point _deadline = clock::now();
    bool _healthy = true;
};

}  // namespace kythira
