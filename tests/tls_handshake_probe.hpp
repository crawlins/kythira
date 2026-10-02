// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file tls_handshake_probe.hpp
/// @brief Observes what a TLS server presents, and whether it accepts a
/// client certificate, with a raw OpenSSL handshake on its port.
///
/// The gRPC reload tests need to see what a *new* peer sees after a reload,
/// independently of the gRPC client under test, so they compare leaf serial
/// numbers from a fresh handshake rather than trusting the transport's own
/// account of what it applied.

#include <raft/certificate_authority.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <string>

namespace kythira::testing {

inline auto serial_of(X509* cert) -> std::string {
    const ASN1_INTEGER* serial = X509_get0_serialNumber(cert);
    BIGNUM* bn = ASN1_INTEGER_to_BN(serial, nullptr);
    char* hex = BN_bn2hex(bn);
    std::string out(hex);
    OPENSSL_free(hex);
    BN_free(bn);
    return out;
}

inline auto serial_of(const std::string& pem) -> std::string {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    auto s = serial_of(cert);
    X509_free(cert);
    return s;
}

// What one fresh TLS connection to the server observed.
struct handshake_result {
    bool handshake_ok{false};  ///< Our side completed the handshake.
    bool accepted{false};      ///< The server then spoke HTTP/2 to us.
    std::string serial;        ///< Leaf serial the server presented.
};

// Opens a TCP connection, handshakes with ALPN h2, optionally presenting a
// client certificate, and sends the HTTP/2 preface. A server that accepted
// the client answers with a SETTINGS frame; one that rejected the client's
// certificate (which under TLS 1.3 happens after our handshake completes)
// sends an alert instead.
inline auto handshake(std::uint16_t port, const ::raft::testing::pem_material* client = nullptr)
    -> handshake_result {
    handshake_result r;
    // A server that rejects our certificate closes the socket under our
    // writes; that must be an error return, not SIGPIPE.
    static const bool sigpipe_ignored = [] {
        ::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void)sigpipe_ignored;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return r;
    }
    timeval tv{.tv_sec = 5, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);  // We only observe.
    static const unsigned char alpn[] = {2, 'h', '2'};
    SSL_CTX_set_alpn_protos(ctx, alpn, sizeof(alpn));
    if (client != nullptr) {
        BIO* cb = BIO_new_mem_buf(client->certificate_pem.data(),
                                  static_cast<int>(client->certificate_pem.size()));
        X509* cert = PEM_read_bio_X509(cb, nullptr, nullptr, nullptr);
        BIO_free(cb);
        BIO* kb = BIO_new_mem_buf(client->private_key_pem.data(),
                                  static_cast<int>(client->private_key_pem.size()));
        EVP_PKEY* key = PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr);
        BIO_free(kb);
        SSL_CTX_use_certificate(ctx, cert);
        SSL_CTX_use_PrivateKey(ctx, key);
        X509_free(cert);
        EVP_PKEY_free(key);
    }
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) == 1) {
        r.handshake_ok = true;
        if (X509* peer = SSL_get1_peer_certificate(ssl)) {
            r.serial = serial_of(peer);
            X509_free(peer);
        }
        static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        static const unsigned char settings[] = {0, 0, 0, 4, 0, 0, 0, 0, 0};
        SSL_write(ssl, preface, sizeof(preface) - 1);
        SSL_write(ssl, settings, sizeof(settings));
        unsigned char buf[64];
        r.accepted = SSL_read(ssl, buf, sizeof(buf)) > 0;
    }
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ERR_clear_error();
    ::close(fd);
    return r;
}

}  // namespace kythira::testing
