// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file tls_reload_probe.hpp
/// @brief Raw OpenSSL TLS and DTLS clients that report which certificate a
///        server actually presented, for the certificate hot-reload suites.
///
/// A transport's own client cannot answer the question those suites ask:
/// cpp-httplib hides the peer certificate, and the CoAP client's DTLS RPC
/// path is a separate piece of work. Going through OpenSSL directly lets a
/// test compare the presented leaf against the bytes on disk before and after
/// a reload, and keep one connection (or DTLS association) open across it.

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace kythira::testing {

/// Hex SHA-256 of an X509's DER encoding.
inline auto x509_fingerprint(X509* cert) -> std::string {
    if (cert == nullptr) {
        return {};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int len = 0;
    if (X509_digest(cert, EVP_sha256(), digest.data(), &len) != 1) {
        throw std::runtime_error("x509_fingerprint: X509_digest failed");
    }
    std::string hex;
    hex.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        std::array<char, 3> byte{};
        std::snprintf(byte.data(), byte.size(), "%02x", digest[i]);
        hex += byte.data();
    }
    return hex;
}

/// Fingerprint of the first (leaf) certificate in a PEM file.
inline auto pem_file_fingerprint(const std::string& path) -> std::string {
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "r"), &std::fclose);
    if (!file) {
        throw std::runtime_error("pem_file_fingerprint: cannot open " + path);
    }
    std::unique_ptr<X509, decltype(&X509_free)> cert(
        PEM_read_X509(file.get(), nullptr, nullptr, nullptr), &X509_free);
    if (!cert) {
        throw std::runtime_error("pem_file_fingerprint: no certificate in " + path);
    }
    return x509_fingerprint(cert.get());
}

/// One TLS (stream) or DTLS (datagram) connection to 127.0.0.1:port.
///
/// The handshake runs in the constructor; a failed handshake throws. The
/// connection stays open until destruction, so a test can hold it across a
/// server-side reload and keep using it.
class tls_reload_probe {
public:
    enum class kind : std::uint8_t {
        tls,
        dtls
    };

    tls_reload_probe(kind k, std::uint16_t port, const std::string& client_cert_path = {},
                     const std::string& client_key_path = {},
                     std::chrono::milliseconds io_timeout = std::chrono::seconds(5))
        : _ctx(SSL_CTX_new(k == kind::tls ? TLS_client_method() : DTLS_client_method())) {
        if (_ctx == nullptr) {
            throw std::runtime_error("tls_reload_probe: SSL_CTX_new failed");
        }
        // The probe reports what was presented; trust decisions are the
        // transport suites' business, not this helper's.
        SSL_CTX_set_verify(_ctx, SSL_VERIFY_NONE, nullptr);
        if (!client_cert_path.empty() &&
            (SSL_CTX_use_certificate_chain_file(_ctx, client_cert_path.c_str()) != 1 ||
             SSL_CTX_use_PrivateKey_file(_ctx, client_key_path.c_str(), SSL_FILETYPE_PEM) != 1)) {
            cleanup();
            throw std::runtime_error("tls_reload_probe: cannot load client cert/key");
        }

        _fd = ::socket(AF_INET, k == kind::tls ? SOCK_STREAM : SOCK_DGRAM, 0);
        if (_fd < 0) {
            cleanup();
            throw std::runtime_error("tls_reload_probe: socket() failed");
        }
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(io_timeout.count() / 1000);
        tv.tv_usec = static_cast<suseconds_t>((io_timeout.count() % 1000) * 1000);
        ::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            cleanup();
            throw std::runtime_error("tls_reload_probe: connect() failed");
        }

        _ssl = SSL_new(_ctx);
        if (k == kind::tls) {
            SSL_set_fd(_ssl, _fd);
        } else {
            BIO* bio = BIO_new_dgram(_fd, BIO_NOCLOSE);
            BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &addr);
            BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_RECV_TIMEOUT, 0, &tv);
            SSL_set_bio(_ssl, bio, bio);
        }

        // A blocking DTLS socket with a receive timeout makes SSL_connect()
        // return on each lost flight; retry until the overall deadline so the
        // cookie exchange and retransmissions get a fair chance.
        auto deadline = std::chrono::steady_clock::now() + io_timeout;
        while (true) {
            int rc = SSL_connect(_ssl);
            if (rc == 1) {
                break;
            }
            int err = SSL_get_error(_ssl, rc);
            bool retry = err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE;
            if (!retry || std::chrono::steady_clock::now() >= deadline) {
                std::array<char, 256> buf{};
                ERR_error_string_n(ERR_get_error(), buf.data(), buf.size());
                cleanup();
                throw std::runtime_error(std::string("tls_reload_probe: handshake failed: ") +
                                         buf.data());
            }
            if (k == kind::dtls) {
                DTLSv1_handle_timeout(_ssl);
            }
        }
    }

    ~tls_reload_probe() { cleanup(); }

    /// Shuts the connection down now rather than at destruction. A server
    /// stopping with a keep-alive connection still open waits out its idle
    /// timeout, so tests close probes before stopping it.
    auto close() -> void { cleanup(); }

    tls_reload_probe(const tls_reload_probe&) = delete;
    tls_reload_probe& operator=(const tls_reload_probe&) = delete;
    tls_reload_probe(tls_reload_probe&&) = delete;
    tls_reload_probe& operator=(tls_reload_probe&&) = delete;

    /// SHA-256 fingerprint of the leaf certificate the server presented on
    /// this connection's handshake.
    [[nodiscard]] auto peer_fingerprint() const -> std::string {
        std::unique_ptr<X509, decltype(&X509_free)> cert(SSL_get1_peer_certificate(_ssl),
                                                         &X509_free);
        return x509_fingerprint(cert.get());
    }

    /// TLS only: sends one HTTP/1.1 keep-alive GET on this connection and
    /// returns the response status code, or -1 if no complete response came
    /// back. Leaves the connection open for the next request.
    [[nodiscard]] auto http_get(const std::string& path) -> int {
        std::string request =
            "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n";
        if (SSL_write(_ssl, request.data(), static_cast<int>(request.size())) <= 0) {
            return -1;
        }
        std::string response;
        std::array<char, 4096> buf{};
        std::size_t header_end = std::string::npos;
        while ((header_end = response.find("\r\n\r\n")) == std::string::npos) {
            int n = SSL_read(_ssl, buf.data(), static_cast<int>(buf.size()));
            if (n <= 0) {
                return -1;
            }
            response.append(buf.data(), static_cast<std::size_t>(n));
        }
        // Drain the body so the next request on this connection starts clean.
        std::size_t content_length = 0;
        auto pos = response.find("Content-Length:");
        if (pos == std::string::npos) {
            pos = response.find("content-length:");
        }
        if (pos != std::string::npos && pos < header_end) {
            content_length = std::stoul(response.substr(pos + 15));
        }
        while (response.size() < header_end + 4 + content_length) {
            int n = SSL_read(_ssl, buf.data(), static_cast<int>(buf.size()));
            if (n <= 0) {
                return -1;
            }
            response.append(buf.data(), static_cast<std::size_t>(n));
        }
        // "HTTP/1.1 404 ..." — the status code starts after the first space.
        auto space = response.find(' ');
        return space == std::string::npos ? -1 : std::stoi(response.substr(space + 1, 3));
    }

    /// DTLS only: sends a Confirmable CoAP GET for a resource no server here
    /// registers over this association, and reports whether a matching
    /// Acknowledgement (any response code) came back. A reply proves the
    /// association, not just the socket, is still live: it has to be
    /// decrypted and re-encrypted with this session's keys. A request rather
    /// than a CoAP ping (an empty CON), because libcoap does not answer every
    /// ping sent in quick succession.
    [[nodiscard]] auto coap_request(std::uint16_t message_id) -> bool {
        auto id_hi = static_cast<unsigned char>(message_id >> 8);
        auto id_lo = static_cast<unsigned char>(message_id & 0xff);
        // Ver 1, CON, TKL 1 | GET | message id | token | Uri-Path "probe".
        std::array<unsigned char, 11> request{0x41, 0x01, id_hi, id_lo, 0x5a, 0xb5,
                                              'p',  'r',  'o',   'b',   'e'};
        if (SSL_write(_ssl, request.data(), static_cast<int>(request.size())) <= 0) {
            return false;
        }
        std::array<unsigned char, 256> buf{};
        int n = SSL_read(_ssl, buf.data(), static_cast<int>(buf.size()));
        if (n < 5) {
            return false;
        }
        // Ver 1, type ACK (2), TKL 1, same message id and token.
        return buf[0] == 0x61 && buf[2] == id_hi && buf[3] == id_lo && buf[4] == 0x5a;
    }

private:
    auto cleanup() -> void {
        if (_ssl != nullptr) {
            SSL_shutdown(_ssl);
            SSL_free(_ssl);
            _ssl = nullptr;
        }
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
        if (_ctx != nullptr) {
            SSL_CTX_free(_ctx);
            _ctx = nullptr;
        }
    }

    SSL_CTX* _ctx{nullptr};
    SSL* _ssl{nullptr};
    int _fd{-1};
};

}  // namespace kythira::testing
