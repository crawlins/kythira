// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/beast_http_transport.hpp>
#include <raft/coap_utils.hpp>

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/asn1.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <format>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace kythira {

namespace {
constexpr const char* beast_endpoint_request_vote = "/v1/raft/request_vote";
constexpr const char* beast_endpoint_append_entries = "/v1/raft/append_entries";
constexpr const char* beast_endpoint_install_snapshot = "/v1/raft/install_snapshot";

// NOTE: `beast_content_type_for_serializer` lived here, deriving the
// Content-Type from the serializer's `name()` by substring match. Removed with
// its httplib twin: the media type is now `serializer.media_type()` reached
// through the registry, which is the token both sides negotiate on. Its
// premise -- "client and server always share the same serializer_type, so this
// is advisory labeling only" -- is precisely what negotiation invalidates.

// ---------------------------------------------------------------------------
// TLS material validation/configuration. Deliberately duplicated (not
// #included) from http_transport_impl.hpp's own anonymous-namespace
// equivalents: those symbols have internal linkage (TU-local to that file)
// so they cannot be reused across translation units regardless, and
// design.md's own Non-Goals rule out modifying http_transport_impl.hpp to
// export them -- the same "each transport keeps its own copy" pattern
// design.md's Phase 0 notes tls_tcp_rpc.hpp already follows. These operate
// on raw OpenSSL types only (SSL_CTX*/X509*), so they port to
// boost::asio::ssl::context verbatim via ::native_handle() -- confirmed
// reusable as-is by the Phase 0 spike (spike-notes.md, Finding 3).
// ---------------------------------------------------------------------------

auto beast_asn1_time_to_time_t(const ASN1_TIME* asn1_time) -> time_t {
    if (asn1_time == nullptr) {
        return 0;
    }
    struct tm tm_time{};
    if (ASN1_TIME_to_tm(asn1_time, &tm_time) != 1) {
        return 0;
    }
    return timegm(&tm_time);
}

auto beast_validate_certificate_file(const std::string& cert_path) -> void {
    if (cert_path.empty()) {
        throw kythira::certificate_validation_error("Certificate path is empty");
    }
    if (!std::filesystem::exists(cert_path)) {
        throw kythira::certificate_validation_error(
            std::format("Certificate file does not exist: {}", cert_path));
    }
    FILE* fp = fopen(cert_path.c_str(), "r");
    if (fp == nullptr) {
        throw kythira::certificate_validation_error(
            std::format("Failed to open certificate file: {}", cert_path));
    }
    X509* cert = PEM_read_X509(fp, nullptr, nullptr, nullptr);
    fclose(fp);
    if (cert == nullptr) {
        throw kythira::certificate_validation_error(
            std::format("Failed to parse certificate file: {}", cert_path));
    }
    X509_free(cert);
}

auto beast_validate_private_key_file(const std::string& key_path) -> void {
    if (key_path.empty()) {
        throw kythira::certificate_validation_error("Private key path is empty");
    }
    if (!std::filesystem::exists(key_path)) {
        throw kythira::certificate_validation_error(
            std::format("Private key file does not exist: {}", key_path));
    }
    FILE* fp = fopen(key_path.c_str(), "r");
    if (fp == nullptr) {
        throw kythira::certificate_validation_error(
            std::format("Failed to open private key file: {}", key_path));
    }
    EVP_PKEY* key = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    fclose(fp);
    if (key == nullptr) {
        throw kythira::certificate_validation_error(
            std::format("Failed to parse private key file: {}", key_path));
    }
    EVP_PKEY_free(key);
}

auto beast_validate_certificate_key_pair(const std::string& cert_path, const std::string& key_path)
    -> void {
    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
    if (ctx == nullptr) {
        throw kythira::ssl_context_error("Failed to create SSL context for key-pair validation");
    }
    if (SSL_CTX_use_certificate_file(ctx, cert_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        throw kythira::certificate_validation_error(
            std::format("Failed to load certificate: {}", cert_path));
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        throw kythira::certificate_validation_error(
            std::format("Failed to load private key: {}", key_path));
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        throw kythira::certificate_validation_error(
            std::format("Certificate/key mismatch: {} / {}", cert_path, key_path));
    }
    SSL_CTX_free(ctx);
}

auto beast_check_certificate_expiration(const std::string& cert_path) -> void {
    FILE* fp = fopen(cert_path.c_str(), "r");
    if (fp == nullptr) {
        throw kythira::certificate_validation_error(
            std::format("Failed to open certificate file: {}", cert_path));
    }
    X509* cert = PEM_read_X509(fp, nullptr, nullptr, nullptr);
    fclose(fp);
    if (cert == nullptr) {
        throw kythira::certificate_validation_error(
            std::format("Failed to parse certificate file: {}", cert_path));
    }
    time_t not_after = beast_asn1_time_to_time_t(X509_get0_notAfter(cert));
    X509_free(cert);
    if (not_after != 0 && not_after < time(nullptr)) {
        throw kythira::certificate_validation_error(
            std::format("Certificate has expired: {}", cert_path));
    }
}

auto beast_validate_cipher_suites(const std::string& cipher_suites) -> void {
    if (cipher_suites.empty()) {
        return;
    }
    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
    if (ctx == nullptr) {
        throw kythira::ssl_context_error(
            "Failed to create SSL context for cipher suite validation");
    }
    if (SSL_CTX_set_cipher_list(ctx, cipher_suites.c_str()) != 1) {
        SSL_CTX_free(ctx);
        throw kythira::ssl_configuration_error(
            std::format("Invalid cipher suites: {}", cipher_suites));
    }
    SSL_CTX_free(ctx);
}

auto beast_tls_version_number(const std::string& version) -> int {
    if (version == "TLSv1.0") {
        return TLS1_VERSION;
    }
    if (version == "TLSv1.1") {
        return TLS1_1_VERSION;
    }
    if (version == "TLSv1.2") {
        return TLS1_2_VERSION;
    }
    if (version == "TLSv1.3") {
        return TLS1_3_VERSION;
    }
    throw kythira::ssl_configuration_error(std::format("Unsupported TLS version: {}", version));
}

auto beast_validate_tls_version_range(const std::string& min_version,
                                      const std::string& max_version) -> void {
    if (min_version.empty() && max_version.empty()) {
        return;
    }
    int min_ver = min_version.empty() ? TLS1_2_VERSION : beast_tls_version_number(min_version);
    int max_ver = max_version.empty() ? TLS1_3_VERSION : beast_tls_version_number(max_version);
    if (min_ver > max_ver) {
        throw kythira::ssl_configuration_error(
            std::format("Minimum TLS version ({}) is higher than maximum TLS version ({})",
                        min_version, max_version));
    }
    if (min_ver < TLS1_2_VERSION) {
        throw kythira::ssl_configuration_error(
            std::format("Minimum TLS version ({}) is below security requirements (TLS 1.2 minimum)",
                        min_version));
    }
}

auto beast_configure_ssl_context(SSL_CTX* ctx, const std::string& cipher_suites,
                                 const std::string& min_tls_version,
                                 const std::string& max_tls_version) -> void {
    if (ctx == nullptr) {
        throw kythira::ssl_context_error("Cannot configure null SSL context");
    }
    if (!cipher_suites.empty() && SSL_CTX_set_cipher_list(ctx, cipher_suites.c_str()) != 1) {
        throw kythira::ssl_context_error(
            std::format("Failed to set cipher suites: {}", cipher_suites));
    }
    if (!min_tls_version.empty() &&
        SSL_CTX_set_min_proto_version(ctx, beast_tls_version_number(min_tls_version)) != 1) {
        throw kythira::ssl_context_error(
            std::format("Failed to set minimum TLS version: {}", min_tls_version));
    }
    if (!max_tls_version.empty() &&
        SSL_CTX_set_max_proto_version(ctx, beast_tls_version_number(max_tls_version)) != 1) {
        throw kythira::ssl_context_error(
            std::format("Failed to set maximum TLS version: {}", max_tls_version));
    }
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_verify_depth(ctx, 10);
}

// ---------------------------------------------------------------------------
// unit-future construction helpers. Requirement 19 pins
// Types::future_template<T> to kythira::future_default<T> specifically for
// this transport, so (unlike http_transport_impl.hpp's own
// make_future_with_value/make_future_with_exception, which have to branch on
// FOLLY_AVAILABLE to stay backend-generic) these can call
// kythira::promise_default<T> directly, with no backend branching at all.
// ---------------------------------------------------------------------------

template<typename T> auto beast_ready_future(T value) -> kythira::future_default<T> {
    kythira::promise_default<T> promise;
    auto future = promise.getFuture();
    promise.setValue(std::move(value));
    return future;
}

inline auto beast_ready_unit_future() -> kythira::future_default<kythira::unit> {
    return beast_ready_future<kythira::unit>(kythira::unit{});
}

template<typename T>
auto beast_exceptional_future(std::exception_ptr ex) -> kythira::future_default<T> {
    kythira::promise_default<T> promise;
    auto future = promise.getFuture();
    promise.setException(ex);
    return future;
}

}  // namespace

// ---------------------------------------------------------------------------
// beast_detail primitive adaptors (Requirement 14.2)
//
// A round-1 ThreadSanitizer run found a genuine data race here under the
// Folly future backend: the io_context thread that fulfills one of these
// promises (promise.setValue()/setException(), below) could race the
// calling thread that immediately chains .thenValue()/.thenError() onto the
// future returned by promise.getFuture() (send_rpc, further down this
// file). Root cause was in include/raft/future.hpp, not in this file --
// kythira::Promise<T>::getFuture() used to return a plain folly::Future<T>
// with no executor attached, exactly the pattern Folly's own SemiFuture +
// via() exists to make safe. Fixed there (Promise<T>::getFuture()/
// getSemiFuture() now route through
// getSemiFuture().via(&folly::InlineExecutor::instance())), not here --
// nothing in this file changed to address it, since the race was never in
// how these adaptors are written, only in what promise.getFuture() itself
// used to hand back.
// ---------------------------------------------------------------------------

namespace beast_detail {

// Every async_*_kf helper below takes the calling connection/session's own
// asio_strand_executor (beast_http_transport.hpp) and re-via()s its
// returned future onto it before returning, rather than leaving the future
// on the InlineExecutor that Promise<T>::getFuture() attaches by default --
// see asio_strand_executor's own doc comment for the TSan-confirmed race
// this closes. The raw completion handlers below stay the simple direct
// setValue()/setException() form; via() is what makes that safe regardless
// of which thread ends up calling it.
inline auto async_connect_kf(beast::tcp_stream& stream, net::ip::tcp::endpoint ep,
                             asio_strand_executor* executor)
    -> kythira::future_default<kythira::unit> {
    kythira::promise_default<kythira::unit> promise;
    auto future = promise.getFuture();
    stream.async_connect(
        ep, [promise = std::move(promise)](const boost::system::error_code& ec) mutable {
            if (ec) {
                promise.setException(std::make_exception_ptr(boost::system::system_error(ec)));
            } else {
                promise.setValue(kythira::unit{});
            }
        });
    return std::move(future).via(executor->handle());
}

inline auto async_client_handshake_kf(beast::ssl_stream<beast::tcp_stream>& stream,
                                      asio_strand_executor* executor)
    -> kythira::future_default<kythira::unit> {
    kythira::promise_default<kythira::unit> promise;
    auto future = promise.getFuture();
    stream.async_handshake(
        net::ssl::stream_base::client,
        [promise = std::move(promise)](const boost::system::error_code& ec) mutable {
            if (ec) {
                promise.setException(std::make_exception_ptr(boost::system::system_error(ec)));
            } else {
                promise.setValue(kythira::unit{});
            }
        });
    return std::move(future).via(executor->handle());
}

inline auto async_server_handshake_kf(beast::ssl_stream<beast::tcp_stream>& stream,
                                      asio_strand_executor* executor)
    -> kythira::future_default<kythira::unit> {
    kythira::promise_default<kythira::unit> promise;
    auto future = promise.getFuture();
    stream.async_handshake(
        net::ssl::stream_base::server,
        [promise = std::move(promise)](const boost::system::error_code& ec) mutable {
            if (ec) {
                promise.setException(std::make_exception_ptr(boost::system::system_error(ec)));
            } else {
                promise.setValue(kythira::unit{});
            }
        });
    return std::move(future).via(executor->handle());
}

template<typename Stream, bool IsRequest, typename Body, typename Fields>
auto async_write_kf(Stream& stream, const beast_http::message<IsRequest, Body, Fields>& message,
                    asio_strand_executor* executor) -> kythira::future_default<kythira::unit> {
    kythira::promise_default<kythira::unit> promise;
    auto future = promise.getFuture();
    beast_http::async_write(
        stream, message,
        [promise = std::move(promise)](const boost::system::error_code& ec,
                                       std::size_t /*bytes_transferred*/) mutable {
            if (ec) {
                promise.setException(std::make_exception_ptr(boost::system::system_error(ec)));
            } else {
                promise.setValue(kythira::unit{});
            }
        });
    return std::move(future).via(executor->handle());
}

template<typename Stream, bool IsRequest, typename Body, typename Fields>
auto async_read_kf(Stream& stream, beast::flat_buffer& buffer,
                   beast_http::message<IsRequest, Body, Fields>& response,
                   asio_strand_executor* executor) -> kythira::future_default<kythira::unit> {
    kythira::promise_default<kythira::unit> promise;
    auto future = promise.getFuture();
    beast_http::async_read(
        stream, buffer, response,
        [promise = std::move(promise)](const boost::system::error_code& ec,
                                       std::size_t /*bytes_transferred*/) mutable {
            if (ec) {
                promise.setException(std::make_exception_ptr(boost::system::system_error(ec)));
            } else {
                promise.setValue(kythira::unit{});
            }
        });
    return std::move(future).via(executor->handle());
}

template<typename Stream, bool IsRequest, typename Body, typename Fields>
auto async_read_kf(Stream& stream, beast::flat_buffer& buffer,
                   beast_http::parser<IsRequest, Body, Fields>& parser,
                   asio_strand_executor* executor) -> kythira::future_default<kythira::unit> {
    kythira::promise_default<kythira::unit> promise;
    auto future = promise.getFuture();
    beast_http::async_read(
        stream, buffer, parser,
        [promise = std::move(promise)](const boost::system::error_code& ec,
                                       std::size_t /*bytes_transferred*/) mutable {
            if (ec) {
                promise.setException(std::make_exception_ptr(boost::system::system_error(ec)));
            } else {
                promise.setValue(kythira::unit{});
            }
        });
    return std::move(future).via(executor->handle());
}

// ---------------------------------------------------------------------------
// beast_connection implementations
// ---------------------------------------------------------------------------

inline plain_beast_connection::plain_beast_connection(beast::tcp_stream stream)
    : _stream(std::move(stream)),
      _executor(std::make_shared<asio_strand_executor>(_stream.get_executor())) {}

inline auto plain_beast_connection::is_open() const -> bool {
    return _stream.socket().is_open();
}

inline auto plain_beast_connection::set_timeout(std::chrono::milliseconds timeout) -> void {
    _timeout = timeout;
    _stream.expires_after(timeout);
}

inline auto plain_beast_connection::connect(const net::ip::tcp::endpoint& ep,
                                            const std::string& /*host*/)
    -> kythira::future_default<kythira::unit> {
    // Keeps this connection alive until the connect completes; see send().
    return async_connect_kf(_stream, ep, _executor.get())
        .thenValue([self = shared_from_this()](kythira::unit u) { return u; });
}

inline auto plain_beast_connection::send(beast_http::request<beast_http::string_body> request)
    -> kythira::future_default<beast_http::response<beast_http::string_body>> {
    auto req = std::make_shared<beast_http::request<beast_http::string_body>>(std::move(request));
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto response = std::make_shared<beast_http::response<beast_http::string_body>>();
    // Re-arm the deadline immediately before issuing the write: it covers
    // this write and the subsequent read as one combined logical operation
    // (per basic_stream's own documented model), but does *not* carry over
    // from the connect() that already ran before send() was called -- see
    // the _timeout member's doc comment (beast_http_transport.hpp) for why.
    _stream.expires_after(_timeout);
    // `buffer` (and `response`) must be captured again in the *second*
    // thenValue below, not just the first: Folly's future-flattening
    // (triggered because the first callback itself returns a future, from
    // async_read_kf) releases the first callback's own captures as soon as
    // it *synchronously* returns -- i.e. right after the read is issued, not
    // once it actually completes. `req` doesn't need this treatment since
    // by the time this callback runs, the write it was needed for has
    // already finished (that completion is what invokes this callback in
    // the first place); `buffer`/`response` are different because *this*
    // callback is what kicks off the still-pending read that needs them.
    // Every link of this chain keeps the connection itself alive: Beast's
    // composed operations hold `_stream` by reference, so a connection
    // destroyed while a write or read is still pending is a use-after-free
    // that surfaces as `pure virtual method called` when the queued handler
    // reaches the partly-destroyed object.
    return async_write_kf(_stream, *req, _executor.get())
        .thenValue([self = shared_from_this(), this, req, buffer, response](kythira::unit) {
            return async_read_kf(_stream, *buffer, *response, _executor.get());
        })
        .thenValue([self = shared_from_this(), buffer, response](kythira::unit) {
            return std::move(*response);
        });
}

inline auto plain_beast_connection::close() -> void {
    boost::system::error_code ec;
    _stream.socket().shutdown(net::ip::tcp::socket::shutdown_both, ec);
    _stream.close();
}

inline tls_beast_connection::tls_beast_connection(beast::ssl_stream<beast::tcp_stream> stream)
    : _stream(std::move(stream)),
      _executor(
          std::make_shared<asio_strand_executor>(beast::get_lowest_layer(_stream).get_executor())) {
}

inline auto tls_beast_connection::is_open() const -> bool {
    return beast::get_lowest_layer(_stream).socket().is_open();
}

inline auto tls_beast_connection::set_timeout(std::chrono::milliseconds timeout) -> void {
    _timeout = timeout;
    beast::get_lowest_layer(_stream).expires_after(timeout);
}

inline auto tls_beast_connection::connect(const net::ip::tcp::endpoint& ep, const std::string& host)
    -> kythira::future_default<kythira::unit> {
    if (!host.empty() && (SSL_set_tlsext_host_name(_stream.native_handle(), host.c_str()) == 0)) {
        return beast_exceptional_future<kythira::unit>(
            std::make_exception_ptr(kythira::ssl_configuration_error(
                std::format("Failed to set SNI host name: {}", host))));
    }
    // Keeps this connection alive across connect and handshake; see send().
    return async_connect_kf(beast::get_lowest_layer(_stream), ep, _executor.get())
        .thenValue([self = shared_from_this(), this](kythira::unit) {
            return async_client_handshake_kf(_stream, _executor.get());
        });
}

inline auto tls_beast_connection::send(beast_http::request<beast_http::string_body> request)
    -> kythira::future_default<beast_http::response<beast_http::string_body>> {
    auto req = std::make_shared<beast_http::request<beast_http::string_body>>(std::move(request));
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto response = std::make_shared<beast_http::response<beast_http::string_body>>();
    // See plain_beast_connection::send()'s comment on re-arming the deadline
    // here (it doesn't carry over from connect()'s handshake).
    beast::get_lowest_layer(_stream).expires_after(_timeout);
    // See plain_beast_connection::send()'s comment: `buffer` must be
    // recaptured in the second thenValue, not just the first, because of
    // how Folly's future-flattening releases a future-returning callback's
    // captures as soon as it synchronously returns.
    // Every link of this chain keeps the connection itself alive: Beast's
    // composed operations hold `_stream` by reference, so a connection
    // destroyed while a write or read is still pending is a use-after-free
    // that surfaces as `pure virtual method called` when the queued handler
    // reaches the partly-destroyed object.
    return async_write_kf(_stream, *req, _executor.get())
        .thenValue([self = shared_from_this(), this, req, buffer, response](kythira::unit) {
            return async_read_kf(_stream, *buffer, *response, _executor.get());
        })
        .thenValue([self = shared_from_this(), buffer, response](kythira::unit) {
            return std::move(*response);
        });
}

inline auto tls_beast_connection::close() -> void {
    boost::system::error_code ec;
    beast::get_lowest_layer(_stream).socket().shutdown(net::ip::tcp::socket::shutdown_both, ec);
    beast::get_lowest_layer(_stream).close();
}

}  // namespace beast_detail

// ---------------------------------------------------------------------------
// boost_beast_client
// ---------------------------------------------------------------------------

template<typename Types>
requires kythira::future_default_transport_types<Types>
boost_beast_client<Types>::boost_beast_client(
    net::io_context& ioc, std::unordered_map<std::uint64_t, std::string> node_id_to_url_map,
    boost_beast_client_config config, metrics_type metrics)
    : _ioc(ioc),
      _node_id_to_url(std::move(node_id_to_url_map)),
      _config(std::move(config)),
      _metrics(std::move(metrics)) {
    validate_certificate_files();
    bool any_https = std::ranges::any_of(
        _node_id_to_url, [](const auto& entry) { return entry.second.starts_with("https://"); });
    if (any_https) {
        _ssl_ctx.emplace(build_ssl_context());
    }
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
boost_beast_client<Types>::~boost_beast_client() {
    disable_auto_reload();

    // Force-close every connection (live or retired) so any in-flight
    // connect/handshake/read/write fails fast with an error rather than
    // continuing to run on an io_context worker thread after _ssl_ctx is
    // torn down beneath it, then wait for that failure to actually reach
    // each operation's own completion handler (which drops its
    // in_flight_guard) before letting _connections/_ssl_ctx destruct --
    // mirrors boost_beast_server::stop()'s Property 8 drain on the client
    // side.
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // Through the weak registry rather than the idle pool: a connection
        // currently leased by an in-flight RPC is in no map at all, and it is
        // precisely that RPC the drain below is waiting for.
        for (auto& weak : _all_connections) {
            if (auto conn = weak.lock()) {
                conn->close();
            }
        }
    }

    std::unique_lock<std::mutex> drain_lock(_drain_mutex);
    _drain_cv.wait(drain_lock, [this] { return _in_flight_operations == 0; });
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::validate_certificate_files() const -> void {
    beast_validate_tls_version_range(_config.min_tls_version, _config.max_tls_version);
    beast_validate_cipher_suites(_config.cipher_suites);
    if (!_config.ca_cert_path.empty()) {
        beast_validate_certificate_file(_config.ca_cert_path);
        beast_check_certificate_expiration(_config.ca_cert_path);
    }
    if (!_config.client_cert_path.empty()) {
        beast_validate_certificate_file(_config.client_cert_path);
        beast_check_certificate_expiration(_config.client_cert_path);
        if (_config.client_key_path.empty()) {
            throw kythira::ssl_configuration_error(
                "Client certificate provided but no private key specified");
        }
        beast_validate_private_key_file(_config.client_key_path);
        beast_validate_certificate_key_pair(_config.client_cert_path, _config.client_key_path);
    } else if (!_config.client_key_path.empty()) {
        throw kythira::ssl_configuration_error(
            "Client private key provided but no certificate specified");
    }
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::load_client_certificates() -> void {
    if (!_config.client_cert_path.empty() && !_config.client_key_path.empty()) {
        beast_validate_certificate_key_pair(_config.client_cert_path, _config.client_key_path);
    }
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::build_ssl_context() -> net::ssl::context {
    net::ssl::context ctx(net::ssl::context::tls_client);
    ctx.set_verify_mode(_config.enable_ssl_verification ? net::ssl::verify_peer
                                                        : net::ssl::verify_none);
    if (!_config.ca_cert_path.empty()) {
        ctx.load_verify_file(_config.ca_cert_path);
    } else {
        ctx.set_default_verify_paths();
    }
    if (!_config.client_cert_path.empty() && !_config.client_key_path.empty()) {
        ctx.use_certificate_file(_config.client_cert_path, net::ssl::context::pem);
        ctx.use_private_key_file(_config.client_key_path, net::ssl::context::pem);
    }
    beast_configure_ssl_context(ctx.native_handle(), _config.cipher_suites, _config.min_tls_version,
                                _config.max_tls_version);
    return ctx;
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::reload_tls_material() -> void {
    validate_certificate_files();
    auto new_ctx = build_ssl_context();

    std::lock_guard<std::mutex> lock(_mutex);
    _ssl_ctx.emplace(std::move(new_ctx));
    // Bumping the generation is what keeps a connection built under the old
    // context from being pooled again when its in-flight RPC finishes;
    // dropping the idle ones here handles everything not currently leased.
    // No "retired" side list is needed any more: a leased connection is kept
    // alive by its lease's own shared_ptr (Property 7).
    ++_tls_generation;
    _idle_connections.clear();
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::enable_auto_reload(std::chrono::seconds poll_interval) -> void {
    disable_auto_reload();
    _auto_reload_thread = std::jthread([this, poll_interval](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            std::error_code ec;
            auto mtime = std::filesystem::last_write_time(_config.client_cert_path, ec);
            if (!ec && mtime != _last_reloaded_cert_mtime) {
                try {
                    reload_tls_material();
                    _last_reloaded_cert_mtime = mtime;
                } catch (const std::exception&) {
                    auto metric = _metrics;
                    metric.set_metric_name("beast_http.client.tls_reload.failed");
                    metric.add_one();
                    metric.emit();
                }
            }
            std::this_thread::sleep_for(poll_interval);
        }
    });
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::disable_auto_reload() -> void {
    if (_auto_reload_thread.joinable()) {
        _auto_reload_thread.request_stop();
        _auto_reload_thread.join();
    }
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::resolve_target(std::uint64_t target) -> const resolved_target& {
    if (auto cached = _resolved_targets.find(target); cached != _resolved_targets.end()) {
        return cached->second;
    }

    auto url_it = _node_id_to_url.find(target);
    if (url_it == _node_id_to_url.end()) {
        throw std::runtime_error(std::format("No URL mapping found for node {}", target));
    }
    const std::string& url = url_it->second;
    bool is_https = url.starts_with("https://");
    std::string authority = url.substr(is_https ? 8 : 7);
    auto colon = authority.find(':');
    auto slash = authority.find('/');
    std::string host = authority.substr(0, std::min(colon, slash));
    std::string port_str =
        (colon != std::string::npos)
            ? authority.substr(
                  colon + 1, (slash == std::string::npos ? authority.size() : slash) - (colon + 1))
            : (is_https ? "443" : "80");

    // Synchronous DNS resolution on the calling thread, once per target rather
    // than once per connection. Not spiked, not required by requirements.md (a
    // gap discovered during implementation): fully async resolution would need
    // a fourth async_*_kf-style primitive for marginal benefit.
    net::ip::tcp::resolver resolver(_ioc);
    auto results = resolver.resolve(host, port_str);
    if (results.empty()) {
        throw std::runtime_error(std::format("Failed to resolve {}:{}", host, port_str));
    }

    auto [it, ok] =
        _resolved_targets.emplace(target, resolved_target{*results.begin(), std::move(host)});
    return it->second;
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::make_connection(std::uint64_t target) -> pooled_connection {
    const auto& where = resolve_target(target);
    const bool is_https = _node_id_to_url.at(target).starts_with("https://");

    std::shared_ptr<beast_detail::beast_connection> connection;
    if (is_https) {
        if (!_ssl_ctx.has_value()) {
            throw kythira::ssl_configuration_error(
                std::format("Node {} uses https:// but no TLS material is configured", target));
        }
        connection = std::make_shared<beast_detail::tls_beast_connection>(
            beast::ssl_stream<beast::tcp_stream>(net::make_strand(_ioc), *_ssl_ctx));
    } else {
        connection = std::make_shared<beast_detail::plain_beast_connection>(
            beast::tcp_stream(net::make_strand(_ioc)));
    }

    // Registered weakly so the destructor can force-close it even while it is
    // leased out, and pruned here so the registry cannot grow without bound on
    // a long-lived client that churns connections.
    std::erase_if(_all_connections, [](const auto& weak) { return weak.expired(); });
    _all_connections.push_back(connection);

    auto metric = _metrics;
    metric.set_metric_name("beast_http.client.connection.created");
    metric.add_dimension("target_node_id", std::to_string(target));
    metric.add_one();
    metric.emit();

    return pooled_connection{std::move(connection), where.endpoint, where.host_header,
                             std::chrono::steady_clock::now(), _tls_generation};
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::acquire_connection(std::uint64_t target) -> pooled_connection {
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _idle_connections.find(target);
    if (it != _idle_connections.end()) {
        auto& idle = it->second;
        while (!idle.empty()) {
            auto conn = std::move(idle.back());
            idle.pop_back();

            // Requirement 9.2: an idle-beyond-timeout connection is dropped
            // rather than reused, checked lazily on the next attempted use
            // (matching Property 5/the http-transport precedent this mirrors
            // -- no background sweep thread). Dropping is now safe outright:
            // nothing else can be holding this connection, because it was
            // idle, and an in-flight RPC would have leased it.
            const auto idle_for = std::chrono::steady_clock::now() - conn.last_used;
            if (idle_for > _config.keep_alive_timeout || !conn.connection->is_open()) {
                continue;
            }

            auto metric = _metrics;
            metric.set_metric_name("beast_http.client.connection.reused");
            metric.add_dimension("target_node_id", std::to_string(target));
            metric.add_one();
            metric.emit();
            conn.last_used = std::chrono::steady_clock::now();
            return conn;
        }
    }

    return make_connection(target);
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::release_connection(std::uint64_t target, pooled_connection conn,
                                                   bool reusable) -> void {
    if (!conn.connection) {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);

    // Property 5: a failed exchange's connection is torn down rather than
    // left pooled, so the next RPC to this target establishes a fresh one
    // instead of retrying a broken one. Scoped to THIS connection -- the
    // predecessor of this code removed the target's single shared connection,
    // which tore it out from under every other RPC then using it.
    //
    // A connection built before a TLS reload is dropped for the same reason it
    // was never pooled again under the old scheme: it is still bound to the
    // superseded context.
    if (!reusable || conn.tls_generation != _tls_generation) {
        conn.connection->close();
        return;
    }

    // `connection_pool_size` bounds the idle connections retained PER TARGET.
    // It previously bounded the number of distinct targets, which is not what
    // the name says and evicted an unrelated node's connection whenever the
    // eleventh peer was first contacted. Over the cap, the connection is closed
    // rather than pooled; correctness never depended on pooling it.
    auto& idle = _idle_connections[target];
    if (idle.size() >= _config.connection_pool_size) {
        conn.connection->close();
        return;
    }

    conn.last_used = std::chrono::steady_clock::now();
    idle.push_back(std::move(conn));

    auto metric = _metrics;
    metric.set_metric_name("beast_http.client.connection.pool_size");
    metric.add_dimension("target_node_id", std::to_string(target));
    metric.add_value(static_cast<double>(idle.size()));
    metric.emit();
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
template<typename Request, typename Response>
auto boost_beast_client<Types>::send_rpc(std::uint64_t target, std::string_view endpoint,
                                         const Request& request, std::chrono::milliseconds timeout,
                                         std::vector<std::string> attempted,
                                         std::string chosen_media_type)
    -> future_template<Response> {
    std::string rpc_type;
    if (endpoint == beast_endpoint_request_vote) {
        rpc_type = "request_vote";
    } else if (endpoint == beast_endpoint_append_entries) {
        rpc_type = "append_entries";
    } else if (endpoint == beast_endpoint_install_snapshot) {
        rpc_type = "install_snapshot";
    }

    try {
        // Checked out exclusively for this RPC and returned when the last
        // continuation below drops the lease. Nothing here holds a reference
        // into the pool, and no other RPC can be driving this stream.
        // Held across the whole thenValue/thenError chain below (whichever
        // branch actually runs drops the last reference), so the destructor
        // can wait until this RPC's own connect/handshake/send/read has
        // truly finished before it lets the pool/_ssl_ctx destruct.
        auto in_flight = std::make_shared<in_flight_guard>(this);
        auto lease =
            std::make_shared<connection_lease>(this, target, acquire_connection(target), in_flight);
        lease->connection()->set_timeout(timeout);  // Property 4: bounds the
                                                    // whole chain below, not
                                                    // each step individually.

        // Same negotiation as cpp_httplib_client, expressed through Beast's
        // header API: cached type if the registry still supports it, else the
        // default (Requirement 6.1-6.3). On a 415 re-entry the retry path has
        // already chosen, and hands the choice down rather than letting this
        // re-derive it -- it may have read the peer's `Accept-Post`, which is
        // not visible from here. Re-deriving discarded that and walked the
        // preference list blind, at the cost of one silent extra round trip.
        const std::string content_type =
            chosen_media_type.empty()
                ? select_request_media_type(_registry, _capability_cache, target)
                : std::move(chosen_media_type);
        auto serialized = _registry.encode_with(content_type, request);
        std::string body(reinterpret_cast<const char*>(serialized.data()), serialized.size());

        beast_http::request<beast_http::string_body> http_req{beast_http::verb::post,
                                                              std::string(endpoint), 11};
        http_req.set(beast_http::field::host, lease->host_header());
        http_req.set(beast_http::field::content_type, content_type);
        // Full Accept list on every request, so a stale cache entry can only
        // cost a re-choice and never a failed exchange.
        if (const auto accept = format_accept_header(_registry.preferred_media_types());
            !accept.empty()) {
            http_req.set(beast_http::field::accept, accept);
        }
        http_req.set(beast_http::field::user_agent, _config.user_agent);
        http_req.body() = std::move(body);
        http_req.prepare_payload();

        auto start_time = std::chrono::steady_clock::now();
        auto metric = _metrics;
        metric.set_metric_name("beast_http.client.request.sent");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("target_node_id", std::to_string(target));
        metric.add_dimension("media_type", content_type);
        metric.add_one();
        metric.emit();

        beast_detail::beast_connection* connection = lease->connection();
        auto already_open = connection->is_open();

        // A shared_ptr (not a `mutable` lambda moving `http_req` directly)
        // so `proceed` stays const-callable: Future<T>::thenValue's
        // future-flattening overload invokes the stored callable through a
        // non-mutable wrapper lambda (include/raft/future.hpp), which
        // requires operator() to be usable on a const object.
        auto req_ptr =
            std::make_shared<beast_http::request<beast_http::string_body>>(std::move(http_req));
        // `lease` captured, not just the raw pointer, so the connection cannot
        // be returned to the pool (or closed) while this send is outstanding.
        auto proceed = [lease, req_ptr](kythira::unit) {
            return lease->connection()->send(std::move(*req_ptr));
        };

        auto response_future =
            [&]() -> kythira::future_default<beast_http::response<beast_http::string_body>> {
            if (already_open) {
                return beast_ready_unit_future().thenValue(std::move(proceed));
            }
            return connection->connect(lease->endpoint(), lease->host_header())
                .thenValue(std::move(proceed));
        }();

        return std::move(response_future)
            .thenValue([this, target, rpc_type, start_time, content_type, in_flight,
                        lease](beast_http::response<beast_http::string_body> resp) -> Response {
                // The exchange completed: a full response was framed and read,
                // so this connection is in a known-good state and may go back
                // to the pool. Marked before any throw below, since a 4xx/5xx
                // is a valid HTTP exchange -- the connection is fine even when
                // the RPC is not.
                lease->mark_reusable();
                auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start_time);
                auto status = static_cast<unsigned>(resp.result_int());

                auto latency_metric = _metrics;
                latency_metric.set_metric_name("beast_http.client.request.latency");
                latency_metric.add_dimension("rpc_type", rpc_type);
                latency_metric.add_dimension("target_node_id", std::to_string(target));
                latency_metric.add_dimension("status", status == 200 ? "success" : "error");
                latency_metric.add_duration(latency);
                latency_metric.emit();

                if (status == 200) {
                    // Absent Content-Type means a peer that predates
                    // negotiation; it gets our default rather than an error
                    // (Requirement 6.4).
                    std::string response_media_type = _registry.default_media_type();
                    if (const auto it = resp.find(beast_http::field::content_type);
                        it != resp.end()) {
                        if (auto stripped = strip_media_type_parameters(
                                std::string_view{it->value().data(), it->value().size()});
                            !stripped.empty()) {
                            response_media_type = std::move(stripped);
                        }
                    }

                    // Cache stays untouched on an unusable type (Requirement
                    // 6.5) — recording it would make the next request repeat
                    // the same doomed choice.
                    if (!_registry.supports(response_media_type)) {
                        auto error_metric = _metrics;
                        error_metric.set_metric_name("beast_http.client.error");
                        error_metric.add_dimension("error_type", "unsupported_media_type");
                        error_metric.add_dimension("media_type", response_media_type);
                        error_metric.add_dimension("target_node_id", std::to_string(target));
                        error_metric.add_one();
                        error_metric.emit();
                        throw kythira::unsupported_media_type_error(response_media_type);
                    }

                    std::vector<std::byte> response_data;
                    response_data.reserve(resp.body().size());
                    for (char c : resp.body()) {
                        response_data.push_back(static_cast<std::byte>(c));
                    }
                    try {
                        auto decoded = _registry.template decode_with<Response>(response_media_type,
                                                                                response_data);
                        // The type the peer *accepted* -- this attempt's
                        // request `Content-Type` -- not the one it answered
                        // in. See `peer_capability_cache.hpp`; the two need
                        // not agree, and only the former is what the next
                        // request has to get right.
                        _capability_cache.record(target, content_type);
                        return decoded;
                    } catch (const kythira::unsupported_media_type_error&) {
                        // Already the right type and already counted above;
                        // rethrow rather than reclassifying it as a
                        // serialization_error, which maps to a different status.
                        throw;
                    } catch (const std::exception& e) {
                        throw kythira::serialization_error(
                            std::format("Failed to deserialize response: {}", e.what()));
                    }
                }
                if (status >= 400 && status < 500) {
                    // Carry `Accept-Post` on the exception: this transport's 415
                    // retry runs in a `thenError` continuation, where the
                    // response object is long gone, so the header has to travel
                    // with the error or not at all. Empty for every status but
                    // 415, and for the 415s of every peer that does not send it.
                    std::string accept_post;
                    if (const auto it = resp.find(kythira::header_accept_post); it != resp.end()) {
                        accept_post.assign(it->value().data(), it->value().size());
                    }
                    throw kythira::http_client_error(
                        static_cast<int>(status),
                        std::format("HTTP client error {}: {}", status, resp.body()),
                        std::move(accept_post));
                }
                if (status >= 500) {
                    throw kythira::http_server_error(
                        static_cast<int>(status),
                        std::format("HTTP server error {}: {}", status, resp.body()));
                }
                throw std::runtime_error(std::format("Unexpected HTTP status code: {}", status));
            })
            .thenError([this, target, in_flight, lease](std::exception_ptr e) -> Response {
                auto error_metric = _metrics;
                error_metric.set_metric_name("beast_http.client.error");
                error_metric.add_dimension("target_node_id", std::to_string(target));
                error_metric.add_one();
                error_metric.emit();
                // Property 5: a failed connection is torn down rather than left
                // pooled, so the next RPC to this target establishes a fresh
                // one instead of retrying a broken one. Nothing to call: the
                // lease was never marked reusable on this path, so dropping it
                // closes the connection -- and only THIS connection, unlike the
                // per-target teardown this replaced.
                std::rethrow_exception(e);
            })
            // The 415 retry sits *outside* the teardown above rather than
            // inside the success continuation, so a retried attempt re-enters
            // `send_rpc` by the front door and gets a fresh connection, the
            // same metrics and the same timeout treatment as a first attempt.
            // It has to be the outermost link: the teardown rethrows, and this
            // is what turns that rethrow back into a new exchange.
            //
            // Safe because 415 is answered before the handler runs
            // (Requirement 4.4), so the peer provably did no work and there is
            // nothing to double-apply. Bounded because `attempted` only ever
            // grows and `next_request_media_type_after_rejection` never returns
            // a type already in it -- a single-serializer registry gets
            // `nullopt` on the first rejection and rethrows unchanged.
            .thenError([this, target, endpoint = std::string(endpoint), request, timeout,
                        content_type,
                        attempted](std::exception_ptr e) mutable -> future_template<Response> {
                try {
                    std::rethrow_exception(e);
                } catch (const kythira::http_client_error& client_error) {
                    if (client_error.status_code() == 415) {
                        attempted.push_back(content_type);
                        // The peer may have named what it would take; when it
                        // did not, `accept_post()` is empty and this is the
                        // blind walk it always was.
                        if (auto next = next_request_media_type_after_rejection(
                                _registry, attempted, client_error.accept_post())) {
                            auto retry_metric = _metrics;
                            retry_metric.set_metric_name("beast_http.client.media_type_retry");
                            retry_metric.add_dimension("target_node_id", std::to_string(target));
                            retry_metric.add_dimension("rejected_media_type", content_type);
                            retry_metric.add_dimension("media_type", *next);
                            retry_metric.add_one();
                            retry_metric.emit();
                            return send_rpc<Request, Response>(target, endpoint, request, timeout,
                                                               std::move(attempted),
                                                               *std::move(next));
                        }
                    }
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // Anything else is not a negotiation failure; fall through
                    // to the propagation below rather than swallowing it.
                }
                // **Returned, not thrown.** This is the Future-returning
                // (flattening) `thenError` overload, and throwing out of it
                // destroys the promise without ever fulfilling it -- the caller
                // then sees "The associated promise has been destructed prior
                // to the associated state becoming ready" instead of the 415.
                // That path is every single-serializer deployment, since a
                // registry with one type has nothing to retry with, so getting
                // it wrong would have broken the common case while the
                // multi-serializer case looked fine.
                return beast_exceptional_future<Response>(e);
            });
    } catch (const std::exception& e) {
        return beast_exceptional_future<Response>(std::current_exception());
    }
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::send_request_vote(std::uint64_t target,
                                                  const kythira::request_vote_request<>& request,
                                                  std::chrono::milliseconds timeout)
    -> future_template<kythira::request_vote_response<>> {
    return send_rpc<kythira::request_vote_request<>, kythira::request_vote_response<>>(
        target, beast_endpoint_request_vote, request, timeout);
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::send_append_entries(
    std::uint64_t target, const kythira::append_entries_request<>& request,
    std::chrono::milliseconds timeout) -> future_template<kythira::append_entries_response<>> {
    return send_rpc<kythira::append_entries_request<>, kythira::append_entries_response<>>(
        target, beast_endpoint_append_entries, request, timeout);
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_client<Types>::send_install_snapshot(
    std::uint64_t target, const kythira::install_snapshot_request<>& request,
    std::chrono::milliseconds timeout) -> future_template<kythira::install_snapshot_response<>> {
    return send_rpc<kythira::install_snapshot_request<>, kythira::install_snapshot_response<>>(
        target, beast_endpoint_install_snapshot, request, timeout);
}

// ---------------------------------------------------------------------------
// server_session: one accepted connection. Not declared in
// beast_http_transport.hpp -- it is a pure implementation detail, reached
// only through boost_beast_server<Types>::do_accept(), and only ever needs
// three of the server's *public* members (session_started/session_finished/
// dispatch), so no friendship is required.
//
// The read half of each request/response cycle reuses async_read_kf, the
// write half reuses async_write_kf (Requirement 14.2/14.3's primitives,
// applied server-side to the same "one Beast async op -> one future" bridge
// the client uses) -- do_accept() itself and this session's read-again-or-
// close loop stay plain recursive completion-handler style, since neither is
// a linear, finite sequence the way one RPC's connect+write+read is.
// ---------------------------------------------------------------------------

namespace beast_detail {

template<typename Types, typename Stream>
class server_session : public std::enable_shared_from_this<server_session<Types, Stream>> {
public:
    server_session(Stream stream, boost_beast_server<Types>* server,
                   std::chrono::seconds request_timeout, std::size_t max_request_body_size)
        : _stream(std::move(stream)),
          _executor(beast::get_lowest_layer(_stream).get_executor()),
          _server(server),
          _request_timeout(request_timeout),
          _max_request_body_size(max_request_body_size) {}

    auto run() -> void {
        // A closer, not just a start/finish counter: a session sitting idle
        // on a keep-alive connection (read_loop() waiting on the *next*
        // request that may never come) has no in-flight request for
        // Property 8 to wait on, so boost_beast_server::stop() needs a way
        // to force it closed rather than block forever. Posted onto the
        // stream's own executor (the strand do_accept() constructed the
        // socket with) rather than closed directly from whatever thread
        // calls stop(), so it never races the read/write this session's own
        // strand is already serializing.
        auto self = this->shared_from_this();
        _session_id = _server->register_session([self] {
            net::post(self->_stream.get_executor(), [self] {
                boost::system::error_code ec;
                beast::get_lowest_layer(self->_stream).socket().close(ec);
            });
        });
        if constexpr (std::is_same_v<Stream, beast::ssl_stream<beast::tcp_stream>>) {
            auto self = this->shared_from_this();
            async_server_handshake_kf(_stream, &_executor)
                .thenValue([self](kythira::unit) {
                    self->read_loop();
                    return kythira::unit{};
                })
                .thenError([self](std::exception_ptr) {
                    self->finish();
                    return kythira::unit{};
                })
                .detach();
        } else {
            read_loop();
        }
    }

private:
    auto read_loop() -> void {
        auto self = this->shared_from_this();
        _buffer.clear();
        // A parser, not a bare message: only a parser exposes .body_limit(),
        // which is what actually makes boost_beast_server_config::
        // max_request_body_size enforced rather than merely a validated,
        // unused config field -- the plain-message async_read_kf overload
        // wraps the message in a Beast-internal parser with no way for a
        // caller to configure it. Re-emplace()d (not reused/reset) each
        // request: beast_http::parser has no reset-for-reuse method, matching
        // its "one parser per message" design.
        _parser.emplace();
        _parser->body_limit(_max_request_body_size);
        beast::get_lowest_layer(_stream).expires_after(_request_timeout);
        async_read_kf(_stream, _buffer, *_parser, &_executor)
            .thenValue([self](kythira::unit) {
                self->_req = self->_parser->release();
                return self->handle_and_write();
            })
            .thenValue([self](kythira::unit) {
                if (self->_should_keep_alive) {
                    self->read_loop();
                } else {
                    self->finish();
                }
                return kythira::unit{};
            })
            .thenError([self](std::exception_ptr eptr) { return self->handle_read_error(eptr); })
            .detach();
    }

    // Requirement 4 (malformed-request handling): a request whose body
    // exceeds max_request_body_size fails async_read_kf with
    // beast_http::error::body_limit rather than reaching dispatch() at all.
    // Headers are always fully parsed before a body_limit error can occur
    // (the limit is checked against the parsed Content-Length, or
    // incrementally for chunked/unbounded bodies, both of which happen only
    // after the header line and fields are already in), so responding with
    // an explicit 413 here (rather than just severing the connection, the
    // only option for a read failure whose cause isn't known) is safe -- the
    // connection is still closed afterward (keep_alive(false)) rather than
    // kept open, since the client's remaining unread bytes on the wire would
    // otherwise be misparsed as the start of a new request. Any other read
    // failure (peer disconnect, timeout, malformed/truncated header) has no
    // reliable message to respond to, so it falls back to the pre-existing
    // finish()-and-close behavior (Error Handling: Server Accept-Loop
    // Resilience -- this session's own failure never stops do_accept() from
    // continuing to serve other connections).
    auto handle_read_error(std::exception_ptr eptr) -> kythira::future_default<kythira::unit> {
        bool is_body_limit_exceeded = false;
        try {
            std::rethrow_exception(eptr);
        } catch (const boost::system::system_error& e) {
            is_body_limit_exceeded = (e.code() == beast_http::error::body_limit);
        } catch (...) {
        }
        if (is_body_limit_exceeded && _parser && _parser->is_header_done()) {
            auto self = this->shared_from_this();
            auto res = std::make_shared<beast_http::response<beast_http::string_body>>(
                beast_http::status::payload_too_large, _parser->get().version());
            res->set(beast_http::field::content_type, "text/plain");
            res->body() = "Request body exceeds maximum allowed size";
            res->keep_alive(false);
            res->prepare_payload();
            // See handle_and_write()'s comment: re-arm before this write too.
            beast::get_lowest_layer(_stream).expires_after(_request_timeout);
            return async_write_kf(_stream, *res, &_executor)
                .thenValue([self, res](kythira::unit) {
                    self->finish();
                    return kythira::unit{};
                })
                .thenError([self](std::exception_ptr) {
                    self->finish();
                    return kythira::unit{};
                });
        }
        finish();
        return beast_ready_unit_future();
    }

    auto handle_and_write() -> kythira::future_default<kythira::unit> {
        // Folly's thenValue-flattening only keeps a continuation's captures
        // alive for the duration of that continuation's own synchronous
        // execution -- once read_loop()'s "return self->handle_and_write();"
        // callback returns (handing off a *new*, still-pending future for
        // the write), that callback's own `self` capture is released. Without
        // its own `self` here, nothing would keep this session (and its
        // `_stream` member, which the write below still needs) alive for the
        // rest of the asynchronous write -- capture a fresh one so the
        // continuation that actually completes the write holds it instead.
        auto self = this->shared_from_this();
        std::string target(_req.target());
        std::vector<std::byte> body;
        body.reserve(_req.body().size());
        for (char c : _req.body()) {
            body.push_back(static_cast<std::byte>(c));
        }

        // Header mechanics live here (this owns the request); negotiation
        // policy lives in dispatch(). An absent Content-Type means a peer that
        // predates negotiation, so it gets the server's default rather than a
        // 415 (Requirement 4.1, 4.2).
        std::string request_media_type = _server->default_media_type();
        if (const auto it = _req.find(beast_http::field::content_type); it != _req.end()) {
            if (auto stripped = kythira::strip_media_type_parameters(
                    std::string_view{it->value().data(), it->value().size()});
                !stripped.empty()) {
                request_media_type = std::move(stripped);
            }
        }
        std::vector<std::string> accepted;
        if (const auto it = _req.find(beast_http::field::accept); it != _req.end()) {
            accepted = kythira::parse_accept_header(
                std::string_view{it->value().data(), it->value().size()});
        }

        std::string response_body;
        unsigned status_code = 200;
        std::string response_media_type;
        _server->dispatch(target, body, request_media_type, accepted, response_body, status_code,
                          response_media_type);

        // Heap-allocated and kept alive via the thenValue continuation's
        // capture, not a stack local: beast_http::async_write (inside
        // async_write_kf) holds a reference to the message for the whole
        // asynchronous write, which can span multiple io_context::run()
        // iterations after this function returns -- a stack-local response
        // object would be destroyed (dangling reference, UAF) before the
        // write actually completes.
        auto res = std::make_shared<beast_http::response<beast_http::string_body>>(
            static_cast<beast_http::status>(status_code), _req.version());
        // dispatch() reports what it actually encoded in, on every path: the
        // negotiated type for a 200, "text/plain" for the error bodies. Asking
        // the server for a single "success content type" stopped being
        // meaningful once the answer could differ per exchange.
        res->set(beast_http::field::content_type, response_media_type);
        // Name what the server *would* have taken, so the peer's retry converges
        // in one round trip instead of walking its whole preference list (W3C
        // LDP 1.0 §7.1). 415 only: on any other status it either does not apply
        // or would be answering a question nobody asked.
        if (status_code == 415) {
            if (auto accept_post = _server->accept_post_header(); !accept_post.empty()) {
                res->set(kythira::header_accept_post, accept_post);
            }
        }
        res->keep_alive(_req.keep_alive());
        res->body() = std::move(response_body);
        res->prepare_payload();
        _should_keep_alive = res->keep_alive();

        // Re-arm the deadline immediately before the write: basic_stream's
        // timeout (armed once in read_loop(), before the read) covers only
        // that read as its own logical operation -- it does not carry over
        // to this separate write, so without re-arming here the write is
        // treated as already past an unset deadline and fails instantly
        // (observed as the client seeing "end of stream" right after a
        // successful request write, since the server's own response write
        // never actually got a chance to go out). Same root cause, same fix
        // as boost_beast_client's connection::send() -- see its comment.
        beast::get_lowest_layer(_stream).expires_after(_request_timeout);
        return async_write_kf(_stream, *res, &_executor).thenValue([self, res](kythira::unit) {
            return kythira::unit{};
        });
    }

    auto finish() -> void {
        boost::system::error_code ec;
        beast::get_lowest_layer(_stream).socket().shutdown(net::ip::tcp::socket::shutdown_send, ec);
        _server->session_finished(_session_id);
    }

    Stream _stream;
    // See asio_strand_executor's own doc comment (beast_http_transport.hpp)
    // for why every async_*_kf call below needs this. Declared right after
    // _stream (member init order follows declaration order, not
    // initializer-list order) so the constructor can build it from
    // beast::get_lowest_layer(_stream).get_executor().
    asio_strand_executor _executor;
    boost_beast_server<Types>* _server;
    std::size_t _session_id{};
    std::chrono::seconds _request_timeout;
    std::size_t _max_request_body_size;
    beast::flat_buffer _buffer;
    std::optional<beast_http::request_parser<beast_http::string_body>> _parser;
    beast_http::request<beast_http::string_body> _req;
    bool _should_keep_alive{false};
    // No _pending member: run()/read_loop() end their chains with .detach()
    // instead of storing the Future. Storing it was an attempt at exactly
    // the backend-neutrality .detach() actually provides, but it had the
    // dependency backwards -- holding a Future alive is what the *eager*
    // backends (Folly, Boost.Thread) don't need, and is not what the lazy
    // one needs. stdexec's senders do nothing at all until something starts
    // the chain, so under KYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec both
    // chains were built, assigned, and silently never run -- every
    // server-side beast_* test hung. `.detach()` is this project's portable
    // "start it and discard the result" primitive (a no-op on the eager
    // backends, exec::start_detached on stdexec); see future_stdexec.hpp's
    // own detach() comment. Session lifetime across the async gap was never
    // _pending's job in the first place: every continuation captures
    // `self`, a shared_from_this() handle.
};

}  // namespace beast_detail

// ---------------------------------------------------------------------------
// boost_beast_server
// ---------------------------------------------------------------------------

template<typename Types>
requires kythira::future_default_transport_types<Types>
boost_beast_server<Types>::boost_beast_server(net::io_context& ioc, std::string bind_address,
                                              std::uint16_t bind_port,
                                              boost_beast_server_config config,
                                              metrics_type metrics)
    : _ioc(ioc),
      _bind_address(std::move(bind_address)),
      _bind_port(bind_port),
      _config(std::move(config)),
      _metrics(std::move(metrics)),
      _acceptor(ioc) {
    validate_certificate_files();
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
boost_beast_server<Types>::~boost_beast_server() {
    disable_auto_reload();
    stop();
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::validate_certificate_files() const -> void {
    beast_validate_tls_version_range(_config.min_tls_version, _config.max_tls_version);
    beast_validate_cipher_suites(_config.cipher_suites);
    if (!_config.enable_ssl) {
        return;
    }
    if (_config.ssl_cert_path.empty() || _config.ssl_key_path.empty()) {
        throw kythira::ssl_configuration_error(
            "enable_ssl is set but ssl_cert_path/ssl_key_path are not both provided");
    }
    beast_validate_certificate_file(_config.ssl_cert_path);
    beast_check_certificate_expiration(_config.ssl_cert_path);
    beast_validate_private_key_file(_config.ssl_key_path);
    beast_validate_certificate_key_pair(_config.ssl_cert_path, _config.ssl_key_path);
    if (!_config.ca_cert_path.empty()) {
        beast_validate_certificate_file(_config.ca_cert_path);
    } else if (_config.require_client_cert) {
        throw kythira::ssl_configuration_error(
            "require_client_cert is set but ca_cert_path is not provided");
    }
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::load_server_certificates() -> void {
    beast_validate_certificate_key_pair(_config.ssl_cert_path, _config.ssl_key_path);
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::build_ssl_context() -> net::ssl::context {
    net::ssl::context ctx(net::ssl::context::tls_server);
    ctx.use_certificate_file(_config.ssl_cert_path, net::ssl::context::pem);
    ctx.use_private_key_file(_config.ssl_key_path, net::ssl::context::pem);
    if (!_config.ca_cert_path.empty()) {
        ctx.load_verify_file(_config.ca_cert_path);
    }
    if (_config.require_client_cert) {
        ctx.set_verify_mode(net::ssl::verify_peer | net::ssl::verify_fail_if_no_peer_cert);
    } else if (!_config.ca_cert_path.empty()) {
        ctx.set_verify_mode(net::ssl::verify_peer);
    } else {
        ctx.set_verify_mode(net::ssl::verify_none);
    }
    beast_configure_ssl_context(ctx.native_handle(), _config.cipher_suites, _config.min_tls_version,
                                _config.max_tls_version);
    return ctx;
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::reload_tls_material() -> void {
    if (!_config.enable_ssl) {
        throw kythira::ssl_configuration_error("SSL is not enabled on this server");
    }
    validate_certificate_files();
    auto new_ctx = std::make_shared<net::ssl::context>(build_ssl_context());

    std::lock_guard<std::mutex> lock(_mutex);
    // A new context object entirely, swapped in for subsequently *accepted*
    // connections only -- an in-flight beast::ssl_stream<beast::tcp_stream>
    // holds a reference into whichever net::ssl::context was live when it
    // was constructed (Property 7), so this never disturbs an established
    // session.
    _ssl_ctx = std::move(new_ctx);
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::enable_auto_reload(std::chrono::seconds poll_interval) -> void {
    disable_auto_reload();
    _auto_reload_thread = std::jthread([this, poll_interval](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            std::error_code ec;
            auto mtime = std::filesystem::last_write_time(_config.ssl_cert_path, ec);
            if (!ec && mtime != _last_reloaded_cert_mtime) {
                try {
                    reload_tls_material();
                    _last_reloaded_cert_mtime = mtime;
                } catch (const std::exception&) {
                    auto metric = _metrics;
                    metric.set_metric_name("beast_http.server.tls_reload.failed");
                    metric.add_one();
                    metric.emit();
                }
            }
            std::this_thread::sleep_for(poll_interval);
        }
    });
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::disable_auto_reload() -> void {
    if (_auto_reload_thread.joinable()) {
        _auto_reload_thread.request_stop();
        _auto_reload_thread.join();
    }
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::register_request_vote_handler(
    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)> handler)
    -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _request_vote_handler = std::move(handler);
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::register_append_entries_handler(
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
        handler) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _append_entries_handler = std::move(handler);
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::register_install_snapshot_handler(
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)>
        handler) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _install_snapshot_handler = std::move(handler);
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::start() -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_running.load()) {
        return;
    }

    if (_config.enable_ssl) {
        load_server_certificates();
        _ssl_ctx = std::make_shared<net::ssl::context>(build_ssl_context());
    }

    net::ip::tcp::endpoint endpoint(net::ip::make_address(_bind_address), _bind_port);
    _acceptor.open(endpoint.protocol());
    _acceptor.set_option(net::socket_base::reuse_address(true));
    _acceptor.bind(endpoint);
    _acceptor.listen(net::socket_base::max_listen_connections);

    _running.store(true);

    auto metric = _metrics;
    metric.set_metric_name("beast_http.server.started");
    metric.add_one();
    metric.emit();

    do_accept();
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::stop() -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_running.load()) {
        return;
    }

    // Requirement 5.2, Property 8: close the acceptor (stop accepting new
    // connections), then actively close every live session's socket rather
    // than just waiting -- a session sitting idle on a keep-alive
    // connection (read_loop() waiting for a *next* request that may never
    // arrive) has no in-flight request for this to wait on; Property 8
    // promises in-flight requests get to finish, not that an idle
    // connection stays open indefinitely. Closing each socket causes its
    // pending async_read to complete with an error, which routes to
    // finish() -> session_finished() the same way any other read failure
    // does, so a request that *is* genuinely in flight still gets to
    // complete its current write first (the close is posted onto the
    // session's own strand, not delivered out from under an in-progress
    // write). Never touch _ioc itself (Requirement 8.3); that stays the
    // caller's own, separate responsibility.
    boost::system::error_code ec;
    _acceptor.close(ec);
    _running.store(false);

    {
        std::lock_guard<std::mutex> sessions_lock(_sessions_mutex);
        for (auto& [id, closer] : _session_closers) {
            closer();
        }
    }

    std::unique_lock<std::mutex> drain_lock(_drain_mutex);
    _drain_cv.wait(drain_lock, [this] {
        std::lock_guard<std::mutex> sessions_lock(_sessions_mutex);
        return _session_closers.empty();
    });

    auto metric = _metrics;
    metric.set_metric_name("beast_http.server.stopped");
    metric.add_one();
    metric.emit();
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::is_running() const -> bool {
    return _running.load();
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::default_media_type() const -> std::string {
    return _registry.default_media_type();
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::accept_post_header() const -> std::string {
    return format_accept_header(_registry.preferred_media_types());
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::register_session(std::function<void()> closer) -> std::size_t {
    std::lock_guard<std::mutex> lock(_sessions_mutex);
    auto id = _next_session_id++;
    _session_closers.emplace(id, std::move(closer));
    return id;
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::session_finished(std::size_t session_id) -> void {
    {
        std::lock_guard<std::mutex> lock(_sessions_mutex);
        _session_closers.erase(session_id);
    }
    std::lock_guard<std::mutex> lock(_drain_mutex);
    _drain_cv.notify_all();
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::dispatch(std::string_view target,
                                         const std::vector<std::byte>& body,
                                         const std::string& request_media_type,
                                         const std::vector<std::string>& accepted,
                                         std::string& response_body, unsigned& status_code,
                                         std::string& response_media_type) -> void {
    // Error bodies are plain text on every path below; the success path
    // overwrites this with whatever was negotiated.
    response_media_type = "text/plain";

    auto handle = [&]<typename Request, typename Response>(
                      const std::function<Response(const Request&)>& handler,
                      std::string_view rpc_type) {
        auto start_time = std::chrono::steady_clock::now();
        auto received_metric = _metrics;
        received_metric.set_metric_name("beast_http.server.request.received");
        received_metric.add_dimension("rpc_type", std::string(rpc_type));
        received_metric.add_one();
        received_metric.emit();

        if (!handler) {
            status_code = 500;
            response_body = "Handler not registered";
            return;
        }

        // 415 before the handler runs (Requirement 4.3, 4.4). Distinct from
        // the 400 below: 415 means "we do not speak this encoding", 400 means
        // "we speak it and your bytes were wrong". Collapsing the two would
        // tell a peer to fix its payload when it needs to change its format.
        if (!_registry.supports(request_media_type)) {
            status_code = 415;
            response_body = "Unsupported Content-Type: " + request_media_type;
            auto error_metric = _metrics;
            error_metric.set_metric_name("beast_http.server.error");
            error_metric.add_dimension("error_type", "unsupported_media_type");
            error_metric.add_dimension("media_type", request_media_type);
            error_metric.add_one();
            error_metric.emit();
            return;
        }

        // Negotiated before the handler runs, so an unsatisfiable Accept costs
        // no handler side effects (Requirement 5.2, 5.3).
        const auto output_media_type = _registry.select_output_media_type(accepted);
        if (!output_media_type) {
            status_code = 406;
            response_body.clear();  // Nothing we could write would be readable.
            auto error_metric = _metrics;
            error_metric.set_metric_name("beast_http.server.error");
            error_metric.add_dimension("error_type", "unsupported_media_type");
            error_metric.add_dimension("media_type", format_accept_header(accepted));
            error_metric.add_one();
            error_metric.emit();
            return;
        }

        Request request;
        try {
            request = _registry.template decode_with<Request>(request_media_type, body);
        } catch (const std::exception& e) {
            status_code = 400;
            response_body = std::format("Bad Request: {}", e.what());
            return;
        }
        try {
            Response response = handler(request);
            // Encoded in the negotiated type, which need not match the request's
            // — a peer may post CBOR and ask for JSON back.
            auto serialized = _registry.encode_with(*output_media_type, response);
            response_body.assign(reinterpret_cast<const char*>(serialized.data()),
                                 serialized.size());
            status_code = 200;
            response_media_type = *output_media_type;
        } catch (const std::exception&) {
            status_code = 500;
            response_body = "Internal Server Error";
        }

        auto latency_metric = _metrics;
        latency_metric.set_metric_name("beast_http.server.request.latency");
        latency_metric.add_dimension("rpc_type", std::string(rpc_type));
        latency_metric.add_dimension("status_code", std::to_string(status_code));
        latency_metric.add_dimension("media_type", response_media_type);
        latency_metric.add_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_time));
        latency_metric.emit();
    };

    if (target == beast_endpoint_request_vote) {
        handle
            .template operator()<kythira::request_vote_request<>, kythira::request_vote_response<>>(
                _request_vote_handler, "request_vote");
    } else if (target == beast_endpoint_append_entries) {
        handle.template
        operator()<kythira::append_entries_request<>, kythira::append_entries_response<>>(
            _append_entries_handler, "append_entries");
    } else if (target == beast_endpoint_install_snapshot) {
        handle.template
        operator()<kythira::install_snapshot_request<>, kythira::install_snapshot_response<>>(
            _install_snapshot_handler, "install_snapshot");
    } else {
        status_code = 404;
        response_body = "Not Found";
        auto error_metric = _metrics;
        error_metric.set_metric_name("beast_http.server.error");
        error_metric.add_dimension("error_type", "not_found");
        error_metric.add_one();
        error_metric.emit();
    }
}

template<typename Types>
requires kythira::future_default_transport_types<Types>
auto boost_beast_server<Types>::do_accept() -> void {
    _acceptor.async_accept(net::make_strand(_ioc), [this](const boost::system::error_code& ec,
                                                          net::ip::tcp::socket socket) {
        if (ec == net::error::operation_aborted || !_running.load()) {
            return;  // stop() closed the acceptor -- do not recurse.
        }
        if (!ec) {
            if (_config.enable_ssl) {
                beast::ssl_stream<beast::tcp_stream> stream(std::move(socket), *_ssl_ctx);
                auto session = std::make_shared<
                    beast_detail::server_session<Types, beast::ssl_stream<beast::tcp_stream>>>(
                    std::move(stream), this, _config.request_timeout,
                    _config.max_request_body_size);
                session->run();
            } else {
                beast::tcp_stream stream(std::move(socket));
                auto session =
                    std::make_shared<beast_detail::server_session<Types, beast::tcp_stream>>(
                        std::move(stream), this, _config.request_timeout,
                        _config.max_request_body_size);
                session->run();
            }
        }
        do_accept();
    });
}

}  // namespace kythira
