// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/proxygen_http_transport.hpp>

#include <folly/io/IOBuf.h>
#include <proxygen/lib/http/HTTPMessage.h>
#include <proxygen/lib/http/HTTPCommonHeaders.h>
#include <proxygen/lib/http/HTTPException.h>
#include <proxygen/lib/http/HTTPConstants.h>
#include <proxygen/lib/http/ProxygenErrorEnum.h>

#include <wangle/acceptor/Acceptor.h>

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/asn1.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <format>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace kythira {

namespace {
// ---------------------------------------------------------------------------
// TLS material validation/configuration. Deliberately duplicated (not
// #included) from http_transport_impl.hpp's/beast_http_transport_impl.hpp's
// own anonymous-namespace equivalents -- each transport keeps its own copy
// (design.md Phase 6, following the Beast spec's own precedent for
// tls_tcp_rpc.hpp/http_transport_impl.hpp). These operate on raw OpenSSL
// types (SSL_CTX*/X509*), so they port to `folly::SSLContext` verbatim via
// `getSSLCtx()` (`spike-notes.md` Finding 5's confirmed direct-transfer
// method-name mapping -- `loadCertificate`/`loadPrivateKey`/
// `loadTrustedCertificates`/`ciphers`/`setVerificationOption` all exist on
// `folly::SSLContext` with the exact names `design.md` assumed).
// ---------------------------------------------------------------------------

auto proxygen_asn1_time_to_time_t(const ASN1_TIME* asn1_time) -> time_t {
    if (asn1_time == nullptr) {
        return 0;
    }
    struct tm tm_time{};
    if (ASN1_TIME_to_tm(asn1_time, &tm_time) != 1) {
        return 0;
    }
    return timegm(&tm_time);
}

auto proxygen_validate_certificate_file(const std::string& cert_path) -> void {
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

auto proxygen_validate_private_key_file(const std::string& key_path) -> void {
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

auto proxygen_validate_certificate_key_pair(const std::string& cert_path,
                                            const std::string& key_path) -> void {
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

auto proxygen_check_certificate_expiration(const std::string& cert_path) -> void {
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
    time_t not_after = proxygen_asn1_time_to_time_t(X509_get0_notAfter(cert));
    X509_free(cert);
    if (not_after != 0 && not_after < time(nullptr)) {
        throw kythira::certificate_validation_error(
            std::format("Certificate has expired: {}", cert_path));
    }
}

auto proxygen_validate_cipher_suites(const std::string& cipher_suites) -> void {
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

auto proxygen_tls_version_number(const std::string& version) -> int {
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

auto proxygen_validate_tls_version_range(const std::string& min_version,
                                         const std::string& max_version) -> void {
    if (min_version.empty() && max_version.empty()) {
        return;
    }
    int min_ver = min_version.empty() ? TLS1_2_VERSION : proxygen_tls_version_number(min_version);
    int max_ver = max_version.empty() ? TLS1_3_VERSION : proxygen_tls_version_number(max_version);
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

auto proxygen_configure_ssl_context(SSL_CTX* ctx, const std::string& cipher_suites,
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
        SSL_CTX_set_min_proto_version(ctx, proxygen_tls_version_number(min_tls_version)) != 1) {
        throw kythira::ssl_context_error(
            std::format("Failed to set minimum TLS version: {}", min_tls_version));
    }
    if (!max_tls_version.empty() &&
        SSL_CTX_set_max_proto_version(ctx, proxygen_tls_version_number(max_tls_version)) != 1) {
        throw kythira::ssl_context_error(
            std::format("Failed to set maximum TLS version: {}", max_tls_version));
    }
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_verify_depth(ctx, 10);
}

auto proxygen_rpc_type_name(std::string_view endpoint) -> std::string {
    if (endpoint == proxygen_detail::proxygen_endpoint_request_vote) {
        return "request_vote";
    }
    if (endpoint == proxygen_detail::proxygen_endpoint_append_entries) {
        return "append_entries";
    }
    if (endpoint == proxygen_detail::proxygen_endpoint_install_snapshot) {
        return "install_snapshot";
    }
    return "unknown";
}

// The `default` here is a genuine hazard: it labels any status this switch does
// not name "Internal Server Error", so a correct status code goes out with a
// contradictory reason phrase rather than an obviously missing one. 406 and 415
// were added with content negotiation for exactly that reason -- `dispatch` was
// already returning them and they were already being sent as
// "415 Internal Server Error". Anything that introduces a new status code here
// has to add its case too.
auto proxygen_status_reason(unsigned status_code) -> std::string {
    switch (status_code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 406:
            return "Not Acceptable";
        case 415:
            return "Unsupported Media Type";
        default:
            return "Internal Server Error";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// proxygen_detail bridge primitives (Requirement 14.1/14.2)
// ---------------------------------------------------------------------------

namespace proxygen_detail {

/// @brief Everything a session will say about its own capacity, sampled at one
///     instant.
///
/// Sampled into a struct rather than formatted eagerly because the sample is
/// taken on every send and formatted only on the refusals, which are rare on a
/// healthy build. It is taken *before* `newTransactionWithError()`, because
/// proxygen refuses on `supportsMoreTransactions()` -- computed from exactly
/// these fields -- so what the caller needs is the state the refusal was
/// decided from.
///
/// Worth the ten accessors: a refusal reporting `out=0, max=1` is proxygen and
/// this translation unit disagreeing about one live object, which is a build
/// defect, while `out=1, max=1` is a genuinely busy HTTP/1.1 session, which is
/// a pool defect. Reading `nullptr` back from `newTransaction()` cannot tell
/// those apart, and for three investigation sessions of
/// `.kiro/specs/multi-raft-performance/` task 5 nobody could.
struct session_probe {
    std::uint32_t out{};
    std::uint32_t max{};
    std::uint32_t in{};
    std::uint32_t streams{};
    std::uint32_t hist_max_out{};
    std::uint64_t served{};
    bool reusable{};
    bool replay_safe{};
    bool draining{};
    bool closing{};

    static auto capture(proxygen::HTTPUpstreamSession* session) -> session_probe {
        return session_probe{session->getNumOutgoingStreams(),
                             session->getMaxConcurrentOutgoingStreams(),
                             session->getNumIncomingStreams(),
                             session->getNumStreams(),
                             session->getHistoricalMaxOutgoingStreams(),
                             session->getNumTxnServed(),
                             session->isReusable(),
                             session->isReplaySafe(),
                             session->isDraining(),
                             session->isClosing()};
    }

    [[nodiscard]] auto str() const -> std::string {
        return std::format(
            "out={}, max={}, in={}, streams={}, hist-max-out={}, txns-served={}, reusable={}, "
            "replay-safe={}, draining={}, closing={}",
            out, max, in, streams, hist_max_out, served, reusable, replay_safe, draining, closing);
    }
};

inline connect_bridge::connect_bridge(folly::EventBase* evb, std::chrono::milliseconds txn_timeout,
                                      std::shared_ptr<session_pool> pool,
                                      kythira::promise_default<session_lease> promise)
    : _evb(evb),
      _wheel_timer(txn_timeout, evb),
      _connector(this, _wheel_timer),
      _pool(std::move(pool)),
      _promise(std::move(promise)) {}

inline auto connect_bridge::connectSuccess(proxygen::HTTPUpstreamSession* session) -> void {
    // Runs on the pinned EventBase (HTTPConnector's contract), which is the
    // only thread allowed to touch a slot, a session, or the pool. Every one
    // of those must therefore be finished *before* the promise is settled:
    // settling can transfer control straight into a continuation on an
    // unrelated thread (see connect_bridge's own header comment).
    //
    // The slot is fresh, and belongs to this session alone. `established` was
    // already incremented on this bridge's behalf by whoever started the
    // connect, so nothing is counted here.
    auto slot = std::make_shared<pooled_session>(session);
    session->setInfoCallback(new session_liveness_tracker(slot, session));
    auto lease = std::make_shared<session_checkout>(_pool, _evb, std::move(slot));
    auto promise = std::move(_promise);
    delete this;
    promise.setValue(std::move(lease));
}

inline auto connect_bridge::connectError(const folly::AsyncSocketException& ex) -> void {
    // The headroom this connect was holding goes back, and every RPC queued
    // behind it is failed with the same error rather than left waiting for a
    // session that is not coming. Failing them is not a regression on the old
    // behaviour: a peer that will not accept a connection fails every RPC to
    // it either way, and `error_handler` is what decides when to try again.
    // Leaving them queued, by contrast, would wedge them until their own
    // request timeout, one at a time.
    --_pool->established;
    auto waiters = std::move(_pool->waiters);
    _pool->waiters.clear();
    auto promise = std::move(_promise);
    auto error = std::make_exception_ptr(ex);
    delete this;
    for (auto& waiter : waiters) {
        waiter.setException(error);
    }
    promise.setException(error);
}

// --- the session pool (Requirement 9) --------------------------------------

inline session_checkout::session_checkout(std::shared_ptr<session_pool> pool, folly::EventBase* evb,
                                          session_slot slot)
    : _pool(std::move(pool)), _evb(evb), _slot(std::move(slot)) {}

inline auto session_checkout::session() const -> proxygen::HTTPUpstreamSession* {
    return _slot->session.load(std::memory_order_acquire);
}

inline auto session_checkout::session_age() const -> std::chrono::milliseconds {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 _slot->opened);
}

inline session_checkout::~session_checkout() {
    // The release has to happen on the pool's pinned EventBase, and this
    // destructor runs wherever the last copy of the lease happened to die --
    // which, on the generic bridge under the boost backend, is a freshly
    // spawned detached thread. `runImmediatelyOrRunInEventBaseThread` keeps
    // the common case (a continuation already on the EventBase) free of a
    // queue hop while making the uncommon one correct.
    auto pool = std::move(_pool);
    auto slot = std::move(_slot);
    auto* evb = _evb;
    evb->runImmediatelyOrRunInEventBaseThread(
        [pool = std::move(pool), evb, slot = std::move(slot)]() mutable {
            release_session(pool, evb, slot);
        });
}

/// @brief Start one connect for @p pool, fulfilling @p promise with a lease on
///     the session it produces. The caller must already have taken the
///     headroom for it (`++established`).
inline auto start_connect(const std::shared_ptr<session_pool>& pool, folly::EventBase* evb,
                          kythira::promise_default<session_lease> promise) -> void {
    auto* bridge = new connect_bridge(evb, pool->connection_timeout, pool, std::move(promise));
    if (pool->use_ssl) {
        bridge->connector().connectSSL(evb, pool->addr, pool->ssl_ctx, nullptr,
                                       pool->connection_timeout);
    } else {
        bridge->connector().connect(evb, pool->addr, pool->connection_timeout);
    }
}

/// @brief Check out the first idle session that can take a transaction right
///     now, discarding the ones that cannot. Null when the pool has none.
///
/// Requirement 9.1/9.3: reuse a pooled session when it is still usable, drop it
/// silently when it is not rather than surfacing the staleness to the caller.
/// `supportsMoreTransactions()` alongside `isReusable()` is the checkout.
///
/// Validation lives here and not at release, for a timing reason worth stating:
/// a session is released from `detachTransaction()`, and whether proxygen has
/// finished unwinding the stream by the time that fires is its business, not
/// this pool's. Judging at release turned every reuse into a discard and every
/// RPC into a fresh TCP handshake -- measured, under ASan, as elections that
/// could no longer finish inside a 30-second budget. Judging at acquire asks
/// the only question that matters -- can this session carry the request I am
/// holding -- at the moment it is actually asked.
inline auto take_idle_session(const std::shared_ptr<session_pool>& pool, folly::EventBase* evb)
    -> session_lease {
    while (!pool->idle.empty()) {
        auto slot = std::move(pool->idle.back());
        pool->idle.pop_back();
        auto* existing = slot->session.load(std::memory_order_acquire);
        if (existing != nullptr && existing->isReusable() && existing->supportsMoreTransactions()) {
            return std::make_shared<session_checkout>(pool, evb, std::move(slot));
        }
        --pool->established;
    }
    return nullptr;
}

/// @brief Hand sessions -- or fresh connects -- to whoever is queued, oldest
///     first, for as long as either is available.
inline auto serve_session_waiters(const std::shared_ptr<session_pool>& pool, folly::EventBase* evb)
    -> void {
    while (!pool->waiters.empty()) {
        auto lease = take_idle_session(pool, evb);
        if (lease == nullptr && pool->established >= pool->capacity) {
            return;
        }
        // Dequeued before it is settled: settling can transfer control
        // straight into a continuation that acquires again, and that
        // continuation must not find this waiter still queued.
        auto waiter = std::move(pool->waiters.front());
        pool->waiters.erase(pool->waiters.begin());
        if (lease != nullptr) {
            waiter.setValue(std::move(lease));
        } else {
            ++pool->established;
            start_connect(pool, evb, std::move(waiter));
        }
    }
}

inline auto acquire_session(const std::shared_ptr<session_pool>& pool, folly::EventBase* evb,
                            const folly::SocketAddress& addr,
                            std::shared_ptr<folly::SSLContext> ssl_ctx, bool use_ssl,
                            std::chrono::milliseconds connection_timeout)
    -> kythira::future_default<session_lease> {
    // Refreshed every time so a `reload_tls_material()` between two RPCs is
    // what a later connect uses, including one started on a waiter's behalf
    // from `serve_session_waiters`, which has no caller to take them from.
    pool->addr = addr;
    pool->ssl_ctx = std::move(ssl_ctx);
    pool->use_ssl = use_ssl;
    pool->connection_timeout = connection_timeout;

    kythira::promise_default<session_lease> promise;
    auto future = promise.getFuture();
    if (auto lease = take_idle_session(pool, evb)) {
        promise.setValue(std::move(lease));
        return future;
    }
    if (pool->established < pool->capacity) {
        ++pool->established;
        start_connect(pool, evb, std::move(promise));
        return future;
    }
    // Every session is checked out. Wait for one instead of failing: this is
    // the line the whole change exists for -- see `session_checkout`.
    pool->waiters.push_back(std::move(promise));
    return future;
}

inline auto release_session(const std::shared_ptr<session_pool>& pool, folly::EventBase* evb,
                            const session_slot& slot) -> void {
    if (slot->session.load(std::memory_order_acquire) == nullptr) {
        // The session died under this checkout and its tracker has already
        // nulled the slot. The headroom it was holding goes back.
        --pool->established;
    } else {
        pool->idle.push_back(slot);
    }
    if (pool->waiters.empty()) {
        return;
    }
    // A queue hop rather than a direct call, and only when someone is waiting.
    // This runs from `detachTransaction()`, where the session just released may
    // not yet report room for another transaction; by the time this task runs
    // proxygen has finished with it, so `take_idle_session` gets a truthful
    // answer instead of discarding a live connection.
    evb->runInEventBaseThread([pool, evb] { serve_session_waiters(pool, evb); });
}

inline transaction_bridge::transaction_bridge(kythira::promise_default<http_response> promise,
                                              session_lease lease)
    : _promise(std::move(promise)), _lease(std::move(lease)) {}

inline auto transaction_bridge::setTransaction(proxygen::HTTPTransaction* txn) noexcept -> void {
    _txn = txn;
}

inline auto transaction_bridge::detachTransaction() noexcept -> void {
    // Defensive only: proxygen guarantees onEOM or onError fires before
    // detachTransaction on every normal or erroring path
    // (HTTPTransactionHandler's own documented contract) -- this only
    // fires _fulfilled==false in a genuine "torn down with no prior
    // signal" edge case, which must surface as a failure, not a bogus
    // default-constructed success value.
    fulfill_exception(std::make_exception_ptr(
        std::runtime_error("proxygen transaction detached without completion")));
    delete this;
}

/// @brief The message's `Content-Type` with any parameters stripped, or an
///     empty string when it declared none.
///
/// One helper rather than three call sites doing it by hand: this is read by
/// both client transaction bridges *and* by the server's `RequestHandler`, and
/// the interesting cases (header absent, header carrying `; charset=utf-8`) are
/// exactly the ones a hand-rolled copy gets subtly wrong in only one of them.
inline auto message_media_type(const proxygen::HTTPMessage& msg) -> std::string {
    const auto& raw = msg.getHeaders().getSingleOrEmpty(proxygen::HTTP_HEADER_CONTENT_TYPE);
    if (raw.empty()) {
        return {};
    }
    return kythira::strip_media_type_parameters(raw);
}

/// @brief The message's `Accept` header parsed into an ordered media-type list,
///     empty when it declared none (which the registry reads as "no
///     preference", not "nothing is acceptable").
inline auto message_accept_list(const proxygen::HTTPMessage& msg) -> std::vector<std::string> {
    const auto& raw = msg.getHeaders().getSingleOrEmpty(proxygen::HTTP_HEADER_ACCEPT);
    if (raw.empty()) {
        return {};
    }
    return kythira::parse_accept_header(raw);
}

inline auto transaction_bridge::onHeadersComplete(
    std::unique_ptr<proxygen::HTTPMessage> msg) noexcept -> void {
    _response.status_code = msg->getStatusCode();
    _response.content_type = message_media_type(*msg);
    _response.accept_post = msg->getHeaders().getSingleOrEmpty(kythira::header_accept_post);
}

inline auto transaction_bridge::onBody(std::unique_ptr<folly::IOBuf> chain) noexcept -> void {
    if (chain) {
        chain->coalesce();
        _response.body.append(reinterpret_cast<const char*>(chain->data()), chain->length());
    }
}

inline auto transaction_bridge::onTrailers(std::unique_ptr<proxygen::HTTPHeaders>) noexcept
    -> void {}

inline auto transaction_bridge::onEOM() noexcept -> void {
    fulfill_value();
}

inline auto transaction_bridge::onUpgrade(proxygen::UpgradeProtocol) noexcept -> void {}

inline auto transaction_bridge::onError(const proxygen::HTTPException& error) noexcept -> void {
    fulfill_exception(std::make_exception_ptr(std::runtime_error(error.what())));
}

inline auto transaction_bridge::onEgressPaused() noexcept -> void {}
inline auto transaction_bridge::onEgressResumed() noexcept -> void {}

inline auto transaction_bridge::fulfill_value() -> void {
    if (!_fulfilled) {
        _fulfilled = true;
        _promise.setValue(std::move(_response));
    }
}

inline auto transaction_bridge::fulfill_exception(std::exception_ptr ex) -> void {
    if (!_fulfilled) {
        _fulfilled = true;
        _promise.setException(ex);
    }
}

// --- folly_transaction_bridge (Requirement 16.2's fast-path handler) -------

inline folly_transaction_bridge::folly_transaction_bridge(folly::Promise<http_response> promise,
                                                          session_lease lease)
    : _promise(std::move(promise)), _lease(std::move(lease)) {}

inline auto folly_transaction_bridge::setTransaction(proxygen::HTTPTransaction* txn) noexcept
    -> void {
    _txn = txn;
}

inline auto folly_transaction_bridge::detachTransaction() noexcept -> void {
    fulfill_exception(folly::exception_wrapper(
        std::runtime_error("proxygen transaction detached without completion")));
    delete this;
}

inline auto folly_transaction_bridge::onHeadersComplete(
    std::unique_ptr<proxygen::HTTPMessage> msg) noexcept -> void {
    _response.status_code = msg->getStatusCode();
    _response.content_type = message_media_type(*msg);
    _response.accept_post = msg->getHeaders().getSingleOrEmpty(kythira::header_accept_post);
}

inline auto folly_transaction_bridge::onBody(std::unique_ptr<folly::IOBuf> chain) noexcept -> void {
    if (chain) {
        chain->coalesce();
        _response.body.append(reinterpret_cast<const char*>(chain->data()), chain->length());
    }
}

inline auto folly_transaction_bridge::onTrailers(std::unique_ptr<proxygen::HTTPHeaders>) noexcept
    -> void {}

inline auto folly_transaction_bridge::onEOM() noexcept -> void {
    fulfill_value();
}

inline auto folly_transaction_bridge::onUpgrade(proxygen::UpgradeProtocol) noexcept -> void {}

inline auto folly_transaction_bridge::onError(const proxygen::HTTPException& error) noexcept
    -> void {
    fulfill_exception(folly::exception_wrapper(std::runtime_error(error.what())));
}

inline auto folly_transaction_bridge::onEgressPaused() noexcept -> void {}
inline auto folly_transaction_bridge::onEgressResumed() noexcept -> void {}

inline auto folly_transaction_bridge::fulfill_value() -> void {
    if (!_fulfilled) {
        _fulfilled = true;
        _promise.setValue(std::move(_response));
    }
}

inline auto folly_transaction_bridge::fulfill_exception(folly::exception_wrapper ex) -> void {
    if (!_fulfilled) {
        _fulfilled = true;
        _promise.setException(std::move(ex));
    }
}

// --- session_liveness_tracker (Requirement 9.3) ----------------------------

inline session_liveness_tracker::session_liveness_tracker(session_slot slot,
                                                          proxygen::HTTPUpstreamSession* session)
    : _slot(std::move(slot)), _session(session) {}

inline auto session_liveness_tracker::onDestroy(const proxygen::HTTPSessionBase&) -> void {
    // Compare-exchange, not an unconditional store: clear the pool's view of
    // this connection only if it is still *this* session. Each session now
    // gets its own slot, so a second session for one slot should not arise at
    // all -- but an unconditional store made that failure mode silently fatal
    // (a dying session evicting a live one, after which the pool hands that
    // slot out as usable), and this is the cheap structural guarantee that it
    // cannot recur. Nothing takes the failure branch in normal operation.
    auto* expected = _session;
    _slot->session.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel,
                                           std::memory_order_acquire);
    delete this;
}

// --- send_on_session (generic bridge) / send_on_session_folly (fast path) -

/// @brief Why proxygen refused a transaction on a session this pool validated
///     moments earlier, in a form that reaches the error log.
///
/// `newTransaction()` collapses every refusal to `nullptr`, which is what kept
/// this storm unreadable across three investigation sessions.
/// `take_idle_session` checks `isReusable() && supportsMoreTransactions()`
/// immediately before the call, so a refusal here is by construction something
/// neither predicate covers -- and a bare `nullptr` cannot say which.
/// `newTransactionWithError()` is the identical call with proxygen's own reason
/// string attached.
///
/// Folded into the exception message rather than emitted as its own log line
/// on purpose: `error_handler` already logs this exception once per refusal, so
/// the storm that is already being counted becomes self-describing without a
/// second stream of output to correlate it against.
inline auto describe_transaction_refusal(const std::string& reason, const session_probe& state,
                                         std::chrono::milliseconds age) -> std::string {
    return std::format("proxygen: session unavailable for new transaction ({}): {}, age={}ms",
                       reason, state.str(), age.count());
}

/// @brief Issues one POST on `session` (which must already be established,
///     and this must already be running on `session`'s own pinned
///     `EventBase` -- callers arrange that via `runInEventBaseThread`, this
///     function itself never hops threads) and fulfills the returned
///     future once the response is fully read or an error occurs.
inline auto send_on_session(session_lease lease, const std::string& path, const std::string& body,
                            const std::string& host, const std::string& user_agent,
                            std::chrono::milliseconds timeout, const std::string& content_type,
                            const std::string& accept) -> kythira::future_default<http_response> {
    auto* session = lease->session();
    if (session == nullptr) {
        kythira::promise_default<http_response> closed_promise;
        auto closed_future = closed_promise.getFuture();
        closed_promise.setException(std::make_exception_ptr(
            std::runtime_error("proxygen: pooled session closed before its request was sent")));
        return closed_future;
    }
    kythira::promise_default<http_response> promise;
    auto future = promise.getFuture();
    const auto age = lease->session_age();
    const auto state = session_probe::capture(session);
    auto* bridge = new transaction_bridge(std::move(promise), std::move(lease));
    auto result = session->newTransactionWithError(bridge);
    if (result.hasError()) {
        // Described before the bridge is deleted: deleting it drops the last
        // reference to the lease, which returns the session to the pool, and
        // reading a session's counters after releasing it is reading state that
        // is no longer this request's to read.
        auto description = describe_transaction_refusal(result.error(), state, age);
        delete bridge;  // never attached to a transaction -- nothing else owns it
        kythira::promise_default<http_response> failed_promise;
        auto failed_future = failed_promise.getFuture();
        failed_promise.setException(
            std::make_exception_ptr(std::runtime_error(std::move(description))));
        return failed_future;
    }
    auto* txn = result.value();
    txn->setIdleTimeout(timeout);
    proxygen::HTTPMessage msg;
    msg.setMethod(proxygen::HTTPMethod::POST);
    msg.setURL(path);
    msg.setHTTPVersion(1, 1);
    msg.getHeaders().set(proxygen::HTTP_HEADER_HOST, host);
    msg.getHeaders().set(proxygen::HTTP_HEADER_CONTENT_TYPE, content_type);
    // Full `Accept` list on every request, so a stale capability-cache entry can
    // only cost a re-choice and never a failed exchange. Omitted entirely when
    // the registry advertises nothing, since an empty `Accept` and an absent one
    // are not the same thing to a peer.
    if (!accept.empty()) {
        msg.getHeaders().set(proxygen::HTTP_HEADER_ACCEPT, accept);
    }
    msg.getHeaders().set(proxygen::HTTP_HEADER_USER_AGENT, user_agent);
    txn->sendHeaders(msg);
    txn->sendBody(folly::IOBuf::copyBuffer(body));
    txn->sendEOM();
    return future;
}

inline auto send_on_session_folly(session_lease lease, const std::string& path,
                                  const std::string& body, const std::string& host,
                                  const std::string& user_agent, std::chrono::milliseconds timeout,
                                  const std::string& content_type, const std::string& accept)
    -> folly::Future<http_response> {
    auto* session = lease->session();
    if (session == nullptr) {
        return folly::makeFuture<http_response>(folly::exception_wrapper(
            std::runtime_error("proxygen: pooled session closed before its request was sent")));
    }
    folly::Promise<http_response> promise;
    auto future = promise.getFuture();
    const auto age = lease->session_age();
    const auto state = session_probe::capture(session);
    auto* bridge = new folly_transaction_bridge(std::move(promise), std::move(lease));
    auto result = session->newTransactionWithError(bridge);
    if (result.hasError()) {
        // See send_on_session: described while the lease is still held.
        auto description = describe_transaction_refusal(result.error(), state, age);
        delete bridge;
        folly::Promise<http_response> failed_promise;
        auto failed_future = failed_promise.getFuture();
        failed_promise.setException(
            folly::exception_wrapper(std::runtime_error(std::move(description))));
        return failed_future;
    }
    auto* txn = result.value();
    txn->setIdleTimeout(timeout);
    proxygen::HTTPMessage msg;
    msg.setMethod(proxygen::HTTPMethod::POST);
    msg.setURL(path);
    msg.setHTTPVersion(1, 1);
    msg.getHeaders().set(proxygen::HTTP_HEADER_HOST, host);
    msg.getHeaders().set(proxygen::HTTP_HEADER_CONTENT_TYPE, content_type);
    // Full `Accept` list on every request, so a stale capability-cache entry can
    // only cost a re-choice and never a failed exchange. Omitted entirely when
    // the registry advertises nothing, since an empty `Accept` and an absent one
    // are not the same thing to a peer.
    if (!accept.empty()) {
        msg.getHeaders().set(proxygen::HTTP_HEADER_ACCEPT, accept);
    }
    msg.getHeaders().set(proxygen::HTTP_HEADER_USER_AGENT, user_agent);
    txn->sendHeaders(msg);
    txn->sendBody(folly::IOBuf::copyBuffer(body));
    txn->sendEOM();
    return future;
}

}  // namespace proxygen_detail

// ---------------------------------------------------------------------------
// proxygen_client
// ---------------------------------------------------------------------------

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
proxygen_client<Types>::proxygen_client(
    folly::IOThreadPoolExecutorBase& io_executor,
    std::unordered_map<std::uint64_t, std::string> node_id_to_url_map,
    proxygen_client_config config, metrics_type metrics)
    : _io_executor(io_executor),
      _node_id_to_url(std::move(node_id_to_url_map)),
      _config(std::move(config)),
      _metrics(std::move(metrics)) {
    validate_certificate_files();
    bool any_https = std::ranges::any_of(
        _node_id_to_url, [](const auto& entry) { return entry.second.starts_with("https://"); });
    if (any_https) {
        _ssl_ctx = build_ssl_context();
    }
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
proxygen_client<Types>::~proxygen_client() {
    disable_auto_reload();
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_client<Types>::validate_certificate_files() const -> void {
    proxygen_validate_tls_version_range(_config.min_tls_version, _config.max_tls_version);
    proxygen_validate_cipher_suites(_config.cipher_suites);
    if (!_config.ca_cert_path.empty()) {
        proxygen_validate_certificate_file(_config.ca_cert_path);
        proxygen_check_certificate_expiration(_config.ca_cert_path);
    }
    if (!_config.client_cert_path.empty()) {
        proxygen_validate_certificate_file(_config.client_cert_path);
        proxygen_check_certificate_expiration(_config.client_cert_path);
        if (_config.client_key_path.empty()) {
            throw kythira::ssl_configuration_error(
                "Client certificate provided but no private key specified");
        }
        proxygen_validate_private_key_file(_config.client_key_path);
        proxygen_validate_certificate_key_pair(_config.client_cert_path, _config.client_key_path);
    } else if (!_config.client_key_path.empty()) {
        throw kythira::ssl_configuration_error(
            "Client private key provided but no certificate specified");
    }
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_client<Types>::build_ssl_context() -> std::shared_ptr<folly::SSLContext> {
    auto ctx = std::make_shared<folly::SSLContext>();
    ctx->setVerificationOption(_config.enable_ssl_verification
                                   ? folly::SSLContext::SSLVerifyPeerEnum::VERIFY
                                   : folly::SSLContext::SSLVerifyPeerEnum::NO_VERIFY);
    if (!_config.ca_cert_path.empty()) {
        ctx->loadTrustedCertificates(_config.ca_cert_path.c_str());
    }
    if (!_config.client_cert_path.empty() && !_config.client_key_path.empty()) {
        ctx->loadCertificate(_config.client_cert_path.c_str());
        ctx->loadPrivateKey(_config.client_key_path.c_str());
    }
    proxygen_configure_ssl_context(ctx->getSSLCtx(), _config.cipher_suites, _config.min_tls_version,
                                   _config.max_tls_version);
    return ctx;
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_client<Types>::reload_tls_material() -> void {
    validate_certificate_files();
    auto new_ctx = build_ssl_context();

    std::lock_guard<std::mutex> lock(_mutex);
    // Retired (not destroyed): an in-flight HTTPUpstreamSession may still
    // reference the old folly::SSLContext (Property 7, matching
    // boost_beast_client::reload_tls_material's own comment).
    if (_ssl_ctx) {
        _retired_ssl_contexts.push_back(_ssl_ctx);
    }
    _ssl_ctx = std::move(new_ctx);
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_client<Types>::enable_auto_reload(std::chrono::seconds poll_interval) -> void {
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
                    metric.set_metric_name("proxygen_http.client.tls_reload.failed");
                    metric.add_one();
                    metric.emit();
                }
            }
            std::this_thread::sleep_for(poll_interval);
        }
    });
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_client<Types>::disable_auto_reload() -> void {
    if (_auto_reload_thread.joinable()) {
        _auto_reload_thread.request_stop();
        _auto_reload_thread.join();
    }
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_client<Types>::get_or_create_slot(std::uint64_t target)
    -> proxygen_detail::pooled_connection& {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _connections.find(target);
    if (it != _connections.end()) {
        it->second.last_used = std::chrono::steady_clock::now();
        return it->second;
    }
    // A bound on simultaneously-tracked target *nodes*, kept as it was: evict
    // the LRU entry (by last_used) before adding one that would exceed it,
    // matching Beast's own precedent. Note this is a second, coarser use of
    // the same knob -- Requirement 9.4's own words are "an upper bound on
    // simultaneously-open sessions", which is what the per-target capacity
    // below now enforces.
    if (_connections.size() >= _config.connection_pool_size && !_connections.empty()) {
        auto lru_it = std::min_element(_connections.begin(), _connections.end(),
                                       [](const auto& lhs, const auto& rhs) {
                                           return lhs.second.last_used < rhs.second.last_used;
                                       });
        _connections.erase(lru_it);
    }
    proxygen_detail::pooled_connection slot;
    // Requirement 9.4 proper: an upper bound on how many sessions to this one
    // target may be open at once, which is the reading
    // `cpp_httplib_client_config`/`boost_beast_client_config` use. Before the
    // pool there was nothing for it to bound -- there was exactly one session
    // per target whatever it said.
    slot.sessions->capacity = std::max<std::size_t>(1, _config.connection_pool_size);
    // Requirement 21.3: one EventBase pinned for this node's whole
    // connection lifetime, not round-robin per call -- required for
    // correctness, not just style, since HTTPUpstreamSession itself is
    // permanently pinned to whichever EventBase it was created on
    // (spike-notes.md Finding 3): reusing a pooled session from a
    // different EventBase than the one it was created on would violate
    // Proxygen's own threading contract.
    slot.event_base = _io_executor.getEventBase();
    slot.last_used = std::chrono::steady_clock::now();
    auto [inserted_it, ok] = _connections.emplace(target, std::move(slot));
    return inserted_it->second;
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_client<Types>::resolve_target(std::uint64_t target) const
    -> std::tuple<folly::SocketAddress, std::string, bool> {
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
    folly::SocketAddress addr;
    // Synchronous DNS resolution on the calling thread, once per connect
    // attempt (not per RPC, since sessions are pooled/reused) -- the same
    // scope/tradeoff boost_beast_client::get_or_create_connection's own
    // resolver.resolve() call documents.
    addr.setFromHostPort(host, static_cast<std::uint16_t>(std::stoi(port_str)));
    return {addr, host, is_https};
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
template<typename Request, typename Response>
auto proxygen_client<Types>::send_rpc(std::uint64_t target, std::string_view endpoint,
                                      const Request& request, std::chrono::milliseconds timeout,
                                      std::vector<std::string> attempted,
                                      std::string chosen_media_type) -> future_template<Response> {
    // Chosen here, once, and handed to whichever path runs. On a 415 re-entry
    // the retry below has already chosen, and hands the choice down rather than
    // letting this re-derive it -- it may have read the peer's `Accept-Post`,
    // which is not visible from here. Re-deriving discarded that and walked the
    // preference list blind, at the cost of one silent extra round trip.
    const std::string content_type =
        chosen_media_type.empty()
            ? kythira::select_request_media_type(_registry, _capability_cache, target)
            : std::move(chosen_media_type);

    // Requirement 16.1: compile-time dispatch purely on Types's own
    // future_template member type -- not a KYTHIRA_DEFAULT_FUTURE_BACKEND
    // macro check (see design.md / this file's own comment on
    // send_rpc_folly_fast_path for why that distinction matters).
    auto attempt = [&]() -> future_template<Response> {
        if constexpr (std::same_as<future_template<Response>, kythira::Future<Response>>) {
            return send_rpc_folly_fast_path<Request, Response>(target, endpoint, request, timeout,
                                                               content_type);
        } else {
            return send_rpc_generic_bridge<Request, Response>(target, endpoint, request, timeout,
                                                              content_type);
        }
    }();

    // One retry wrapper covering both paths (Requirement 7.3). Attached here
    // rather than inside either body precisely because there are two bodies:
    // Task 10a's write-up records how the hardcoded `application/json` came to
    // differ between them, and a per-path retry is the same shape of mistake
    // waiting to happen. Safe because 415 precedes the handler, and bounded
    // because `attempted` only grows.
    return std::move(attempt).thenError(
        [this, target, endpoint = std::string(endpoint), request, timeout, content_type,
         attempted](std::exception_ptr e) mutable -> future_template<Response> {
            try {
                std::rethrow_exception(e);
            } catch (const kythira::http_client_error& client_error) {
                if (client_error.status_code() == 415) {
                    attempted.push_back(content_type);
                    // The peer may have named what it would take; when it did
                    // not, `accept_post()` is empty and this is the blind walk
                    // it always was.
                    if (auto next = kythira::next_request_media_type_after_rejection(
                            _registry, attempted, client_error.accept_post())) {
                        auto retry_metric = _metrics;
                        retry_metric.set_metric_name("proxygen_http.client.media_type_retry");
                        retry_metric.add_dimension("target_node_id", std::to_string(target));
                        retry_metric.add_dimension("rejected_media_type", content_type);
                        retry_metric.add_dimension("media_type", *next);
                        retry_metric.add_one();
                        retry_metric.emit();
                        return send_rpc<Request, Response>(target, endpoint, request, timeout,
                                                           std::move(attempted), *std::move(next));
                    }
                }
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Not a negotiation failure; fall through to propagation.
            }
            // Returned rather than thrown, for the same reason as the other two
            // transports: throwing out of a Future-returning `thenError`
            // leaves the promise unfulfilled and reports a BrokenPromise in
            // place of the 415.
            kythira::promise_default<Response> error_promise;
            auto error_future = error_promise.getFuture();
            error_promise.setException(e);
            return error_future;
        });
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
template<typename Request, typename Response>
auto proxygen_client<Types>::send_rpc_generic_bridge(std::uint64_t target,
                                                     std::string_view endpoint,
                                                     const Request& request,
                                                     std::chrono::milliseconds timeout,
                                                     const std::string& content_type)
    -> future_template<Response> {
    std::string rpc_type = proxygen_rpc_type_name(endpoint);
    kythira::promise_default<Response> promise;
    auto future = promise.getFuture();
    try {
        auto& slot = get_or_create_slot(target);
        auto [addr, host, is_https] = resolve_target(target);
        // Cached type if the registry still supports it, else the registry
        // default (Requirement 6.1-6.3). Identical on both client paths on
        // purpose: which one runs is decided by the future backend, and a node's
        // wire behaviour must not depend on that.
        const std::string accept = kythira::format_accept_header(_registry.preferred_media_types());
        auto serialized = _registry.encode_with(content_type, request);
        std::string body(reinterpret_cast<const char*>(serialized.data()), serialized.size());
        auto* evb = slot.event_base;
        auto sessions = slot.sessions;
        auto ssl_ctx = _ssl_ctx;
        auto user_agent = _config.user_agent;
        auto connection_timeout = _config.connection_timeout;
        auto start_time = std::chrono::steady_clock::now();

        auto metric = _metrics;
        metric.set_metric_name("proxygen_http.client.request.sent");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("target_node_id", std::to_string(target));
        metric.add_dimension("path", "generic_bridge");
        metric.add_dimension("media_type", content_type);
        metric.add_one();
        metric.emit();

        evb->runInEventBaseThread([this, evb, sessions, addr, ssl_ctx, is_https, connection_timeout,
                                   host, body = std::move(body), endpoint = std::string(endpoint),
                                   timeout, target, rpc_type, start_time, content_type, accept,
                                   promise = std::move(promise)]() mutable {
            // The response-transform step below returns Response or
            // throws (Beast's own send_rpc pattern exactly) rather
            // than fulfilling `promise` from two separate
            // thenValue/thenError callbacks -- capturing the
            // move-only `promise` into *both* would move from it
            // twice. A single trailing .thenTry() (Try<Response> ->
            // void) is the one place `promise` is captured/settled,
            // exactly once, regardless of which upstream step
            // produced the value or the exception.
            auto chain =
                proxygen_detail::acquire_session(sessions, evb, addr, ssl_ctx, is_https,
                                                 connection_timeout)
                    .thenValue([evb, host, body = std::move(body), endpoint,
                                user_agent = _config.user_agent, timeout, content_type,
                                accept](proxygen_detail::session_lease lease) mutable {
                        // Requirement 16.3 / Property 6: hop back onto the
                        // connection's pinned EventBase before touching the
                        // session. This continuation is *not* already on it --
                        // the folly fast path pins each step with .via(evb),
                        // but the generic bridge has no equivalent, and the
                        // wrapper's own via() takes a backend executor
                        // (boost::executors::executor* under the boost
                        // backend), not a folly::EventBase*. Under the boost
                        // backend a promise-backed then() runs on a freshly
                        // spawned detached thread, so without this hop
                        // send_on_session -- newTransaction(), setIdleTimeout(),
                        // sendHeaders/sendBody/sendEOM -- ran off the EventBase
                        // and corrupted Proxygen's own per-session state,
                        // exactly contrary to the contract send_on_session's
                        // doc comment states it relies on.
                        kythira::promise_default<proxygen_detail::http_response> hop_promise;
                        auto hop_future = hop_promise.getFuture();
                        // The lease rides into the hop and from there into
                        // the transaction bridge, so the session stays checked
                        // out for exactly as long as the transaction it is
                        // carrying -- not until this closure returns, which is
                        // long before the response arrives.
                        evb->runInEventBaseThread([lease = std::move(lease), endpoint,
                                                   body = std::move(body), host, user_agent,
                                                   timeout, content_type, accept,
                                                   hop_promise = std::move(hop_promise)]() mutable {
                            proxygen_detail::send_on_session(std::move(lease), endpoint, body, host,
                                                             user_agent, timeout, content_type,
                                                             accept)
                                .thenTry([hop_promise = std::move(hop_promise)](
                                             kythira::try_default<proxygen_detail::http_response>
                                                 result) mutable {
                                    if (result.hasException()) {
                                        hop_promise.setException(result.exception());
                                    } else {
                                        hop_promise.setValue(std::move(result.value()));
                                    }
                                })
                                .detach();
                        });
                        return hop_future;
                    })
                    .thenValue([this, target, rpc_type, start_time,
                                content_type](proxygen_detail::http_response resp) -> Response {
                        auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start_time);
                        auto latency_metric = _metrics;
                        latency_metric.set_metric_name("proxygen_http.client.request.latency");
                        latency_metric.add_dimension("rpc_type", rpc_type);
                        latency_metric.add_dimension("target_node_id", std::to_string(target));
                        latency_metric.add_dimension("status",
                                                     resp.status_code == 200 ? "success" : "error");
                        latency_metric.add_dimension("path", "generic_bridge");
                        latency_metric.add_duration(latency);
                        latency_metric.emit();

                        if (resp.status_code == 200) {
                            // Absent Content-Type means a peer that predates
                            // negotiation; it gets our default rather than an
                            // error (Requirement 6.4).
                            std::string response_media_type = resp.content_type.empty()
                                                                  ? _registry.default_media_type()
                                                                  : resp.content_type;

                            // Cache stays untouched on an unusable type
                            // (Requirement 6.5) -- recording it would make the
                            // next request repeat the same doomed choice.
                            if (!_registry.supports(response_media_type)) {
                                auto error_metric = _metrics;
                                error_metric.set_metric_name("proxygen_http.client.error");
                                error_metric.add_dimension("error_type", "unsupported_media_type");
                                error_metric.add_dimension("media_type", response_media_type);
                                error_metric.add_dimension("target_node_id",
                                                           std::to_string(target));
                                error_metric.add_one();
                                error_metric.emit();
                                throw kythira::unsupported_media_type_error(response_media_type);
                            }

                            std::vector<std::byte> response_data;
                            response_data.reserve(resp.body.size());
                            for (char c : resp.body) {
                                response_data.push_back(static_cast<std::byte>(c));
                            }
                            try {
                                auto decoded = _registry.template decode_with<Response>(
                                    response_media_type, response_data);
                                // The type the peer *accepted* -- this
                                // attempt's request `Content-Type` -- not the
                                // one it answered in; see
                                // `peer_capability_cache.hpp`.
                                _capability_cache.record(target, content_type);
                                return decoded;
                            } catch (const kythira::unsupported_media_type_error&) {
                                // Already the right type and already counted
                                // above; rethrow rather than reclassifying it as
                                // a serialization_error, which maps to a
                                // different status.
                                throw;
                            } catch (const std::exception& e) {
                                throw kythira::serialization_error(
                                    std::format("Failed to deserialize response: {}", e.what()));
                            }
                        }
                        if (resp.status_code >= 400 && resp.status_code < 500) {
                            // Carry `Accept-Post` on the exception: the 415
                            // retry runs in a `thenError` continuation, where
                            // the response object is gone, so the header has to
                            // travel with the error or not at all.
                            throw kythira::http_client_error(
                                resp.status_code,
                                std::format("HTTP client error {}: {}", resp.status_code,
                                            resp.body),
                                resp.accept_post);
                        }
                        if (resp.status_code >= 500) {
                            throw kythira::http_server_error(
                                resp.status_code, std::format("HTTP server error {}: {}",
                                                              resp.status_code, resp.body));
                        }
                        throw std::runtime_error(
                            std::format("Unexpected HTTP status code: {}", resp.status_code));
                    })
                    .thenError([this, target](std::exception_ptr e) -> Response {
                        auto error_metric = _metrics;
                        error_metric.set_metric_name("proxygen_http.client.error");
                        error_metric.add_dimension("target_node_id", std::to_string(target));
                        error_metric.add_one();
                        error_metric.emit();
                        std::rethrow_exception(e);
                    });
            std::move(chain)
                .thenTry(
                    [promise = std::move(promise)](kythira::try_default<Response> result) mutable {
                        if (result.hasException()) {
                            promise.setException(result.exception());
                        } else {
                            promise.setValue(std::move(result.value()));
                        }
                    })
                .detach();  // Requirement 14.3: guarantees this chain
                            // actually executes under every backend
                            // (stdexec's lazy senders in particular),
                            // not only Folly's eager-by-construction
                            // one -- future.hpp's own detach() comment.
        });
        return future;
    } catch (const std::exception&) {
        promise.setException(std::current_exception());
        return future;
    }
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
template<typename Request, typename Response>
auto proxygen_client<Types>::send_rpc_folly_fast_path(std::uint64_t target,
                                                      std::string_view endpoint,
                                                      const Request& request,
                                                      std::chrono::milliseconds timeout,
                                                      const std::string& content_type)
    -> kythira::Future<Response> {
    std::string rpc_type = proxygen_rpc_type_name(endpoint);
    // Raw folly::Promise<T>, not kythira::promise_default<T> -- no generic
    // bridge type is constructed on this path at all (Requirement 16.2).
    folly::Promise<Response> promise;
    auto folly_future = promise.getFuture();
    try {
        auto& slot = get_or_create_slot(target);
        auto [addr, host, is_https] = resolve_target(target);
        // Cached type if the registry still supports it, else the registry
        // default (Requirement 6.1-6.3). Identical on both client paths on
        // purpose: which one runs is decided by the future backend, and a node's
        // wire behaviour must not depend on that.
        const std::string accept = kythira::format_accept_header(_registry.preferred_media_types());
        auto serialized = _registry.encode_with(content_type, request);
        std::string body(reinterpret_cast<const char*>(serialized.data()), serialized.size());
        auto* evb = slot.event_base;
        auto sessions = slot.sessions;
        auto ssl_ctx = _ssl_ctx;
        auto connection_timeout = _config.connection_timeout;
        auto start_time = std::chrono::steady_clock::now();

        auto metric = _metrics;
        metric.set_metric_name("proxygen_http.client.request.sent");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("target_node_id", std::to_string(target));
        metric.add_dimension("path", "folly_fast_path");
        metric.add_dimension("media_type", content_type);
        metric.add_one();
        metric.emit();

        evb->runInEventBaseThread([this, evb, sessions, addr, ssl_ctx, is_https, connection_timeout,
                                   host, body = std::move(body), endpoint = std::string(endpoint),
                                   timeout, target, rpc_type, start_time, content_type, accept,
                                   promise = std::move(promise)]() mutable {
            // Requirement 16.3: continuations scheduled via .via(evb) --
            // the connection's own pinned EventBase -- so this chain
            // never hops threads beyond the single initial dispatch onto
            // evb every RPC (generic or fast path) already needs, since
            // the calling thread is arbitrary. As in the generic path
            // (send_rpc, above), the response-transform step returns
            // Response or throws rather than fulfilling `promise` from
            // two separate thenValue/thenError callbacks -- a single
            // trailing .thenTry() is the one place `promise` is
            // captured/settled.
            auto chain =
                proxygen_detail::acquire_session(sessions, evb, addr, ssl_ctx, is_https,
                                                 connection_timeout)
                    .get_folly_future()
                    .via(evb)
                    .thenValue([host, body = std::move(body), endpoint,
                                user_agent = _config.user_agent, timeout, content_type,
                                accept](proxygen_detail::session_lease lease) {
                        // The lease goes *into* the send, where the transaction
                        // bridge holds it until proxygen detaches the
                        // transaction. Releasing it when this response future
                        // settles is too early -- see `transaction_bridge`'s
                        // own constructor.
                        return proxygen_detail::send_on_session_folly(
                            std::move(lease), endpoint, body, host, user_agent, timeout,
                            content_type, accept);
                    })
                    .via(evb)
                    .thenValue([this, target, rpc_type, start_time,
                                content_type](proxygen_detail::http_response resp) -> Response {
                        auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start_time);
                        auto latency_metric = _metrics;
                        latency_metric.set_metric_name("proxygen_http.client.request.latency");
                        latency_metric.add_dimension("rpc_type", rpc_type);
                        latency_metric.add_dimension("target_node_id", std::to_string(target));
                        latency_metric.add_dimension("status",
                                                     resp.status_code == 200 ? "success" : "error");
                        latency_metric.add_dimension("path", "folly_fast_path");
                        latency_metric.add_duration(latency);
                        latency_metric.emit();

                        if (resp.status_code == 200) {
                            // Absent Content-Type means a peer that predates
                            // negotiation; it gets our default rather than an
                            // error (Requirement 6.4).
                            std::string response_media_type = resp.content_type.empty()
                                                                  ? _registry.default_media_type()
                                                                  : resp.content_type;

                            // Cache stays untouched on an unusable type
                            // (Requirement 6.5) -- recording it would make the
                            // next request repeat the same doomed choice.
                            if (!_registry.supports(response_media_type)) {
                                auto error_metric = _metrics;
                                error_metric.set_metric_name("proxygen_http.client.error");
                                error_metric.add_dimension("error_type", "unsupported_media_type");
                                error_metric.add_dimension("media_type", response_media_type);
                                error_metric.add_dimension("target_node_id",
                                                           std::to_string(target));
                                error_metric.add_one();
                                error_metric.emit();
                                throw kythira::unsupported_media_type_error(response_media_type);
                            }

                            std::vector<std::byte> response_data;
                            response_data.reserve(resp.body.size());
                            for (char c : resp.body) {
                                response_data.push_back(static_cast<std::byte>(c));
                            }
                            try {
                                auto decoded = _registry.template decode_with<Response>(
                                    response_media_type, response_data);
                                // The type the peer *accepted* -- this
                                // attempt's request `Content-Type` -- not the
                                // one it answered in; see
                                // `peer_capability_cache.hpp`.
                                _capability_cache.record(target, content_type);
                                return decoded;
                            } catch (const kythira::unsupported_media_type_error&) {
                                // Already the right type and already counted
                                // above; rethrow rather than reclassifying it as
                                // a serialization_error, which maps to a
                                // different status.
                                throw;
                            } catch (const std::exception& e) {
                                throw kythira::serialization_error(
                                    std::format("Failed to deserialize response: {}", e.what()));
                            }
                        }
                        if (resp.status_code >= 400 && resp.status_code < 500) {
                            // Carry `Accept-Post` on the exception: the 415
                            // retry runs in a `thenError` continuation, where
                            // the response object is gone, so the header has to
                            // travel with the error or not at all.
                            throw kythira::http_client_error(
                                resp.status_code,
                                std::format("HTTP client error {}: {}", resp.status_code,
                                            resp.body),
                                resp.accept_post);
                        }
                        if (resp.status_code >= 500) {
                            throw kythira::http_server_error(
                                resp.status_code, std::format("HTTP server error {}: {}",
                                                              resp.status_code, resp.body));
                        }
                        throw std::runtime_error(
                            std::format("Unexpected HTTP status code: {}", resp.status_code));
                    })
                    .thenError([this, target](folly::exception_wrapper ew) -> Response {
                        auto error_metric = _metrics;
                        error_metric.set_metric_name("proxygen_http.client.error");
                        error_metric.add_dimension("target_node_id", std::to_string(target));
                        error_metric.add_one();
                        error_metric.emit();
                        ew.throw_exception();
                    });
            std::move(chain).thenTry(
                [promise = std::move(promise)](folly::Try<Response> result) mutable {
                    if (result.hasException()) {
                        promise.setException(std::move(result.exception()));
                    } else {
                        promise.setValue(std::move(result.value()));
                    }
                });
        });
    } catch (const std::exception&) {
        promise.setException(folly::exception_wrapper(std::current_exception()));
    }
    // The one translation this path cannot avoid: folly::Future<T> ->
    // kythira::Future<T>, via the already-existing constructor
    // (include/raft/future.hpp) -- not a new bridging mechanism
    // (Requirement 16.2).
    return kythira::Future<Response>(std::move(folly_future));
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_client<Types>::send_request_vote(std::uint64_t target,
                                               const kythira::request_vote_request<>& request,
                                               std::chrono::milliseconds timeout)
    -> future_template<kythira::request_vote_response<>> {
    return send_rpc<kythira::request_vote_request<>, kythira::request_vote_response<>>(
        target, proxygen_detail::proxygen_endpoint_request_vote, request, timeout);
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_client<Types>::send_append_entries(std::uint64_t target,
                                                 const kythira::append_entries_request<>& request,
                                                 std::chrono::milliseconds timeout)
    -> future_template<kythira::append_entries_response<>> {
    return send_rpc<kythira::append_entries_request<>, kythira::append_entries_response<>>(
        target, proxygen_detail::proxygen_endpoint_append_entries, request, timeout);
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_client<Types>::send_install_snapshot(
    std::uint64_t target, const kythira::install_snapshot_request<>& request,
    std::chrono::milliseconds timeout) -> future_template<kythira::install_snapshot_response<>> {
    return send_rpc<kythira::install_snapshot_request<>, kythira::install_snapshot_response<>>(
        target, proxygen_detail::proxygen_endpoint_install_snapshot, request, timeout);
}

// ---------------------------------------------------------------------------
// Server-side RequestHandler/RequestHandlerFactory (Requirement 4) -- built
// on Proxygen's own higher-level server API (proxygen::RequestHandler +
// proxygen::ResponseBuilder), not a raw HTTPTransactionHandler the way the
// client side is -- spike-notes.md Finding 6 records this as a deliberate
// design refinement over design.md's original lower-level sketch, since
// Proxygen itself provides and documents this as the intended server-side
// extension point.
// ---------------------------------------------------------------------------

namespace proxygen_detail {

template<typename Types> class rpc_request_handler final : public proxygen::RequestHandler {
public:
    explicit rpc_request_handler(proxygen_server<Types>* server) : _server(server) {}

    auto onRequest(std::unique_ptr<proxygen::HTTPMessage> headers) noexcept -> void override {
        _path = headers->getPath();
        // Header mechanics live here (this owns the HTTPMessage); negotiation
        // policy lives in dispatch(). An absent Content-Type means a peer that
        // predates negotiation, so it gets the server's default rather than a
        // 415 (Requirement 4.1, 4.2).
        _request_media_type = message_media_type(*headers);
        if (_request_media_type.empty()) {
            _request_media_type = _server->default_media_type();
        }
        _accepted = message_accept_list(*headers);
        _request_id = _server->register_request();
    }

    auto onBody(std::unique_ptr<folly::IOBuf> body) noexcept -> void override {
        if (body) {
            body->coalesce();
            const auto* data = reinterpret_cast<const std::byte*>(body->data());
            _body.insert(_body.end(), data, data + body->length());
        }
    }

    auto onUpgrade(proxygen::UpgradeProtocol) noexcept -> void override {}

    auto onEOM() noexcept -> void override {
        std::string response_body;
        unsigned status_code = 200;
        std::string response_media_type;
        _server->dispatch(_path, _body, _request_media_type, _accepted, response_body, status_code,
                          response_media_type);
        // dispatch() reports what it actually encoded in, on every path: the
        // negotiated type for a 200, "text/plain" for the error bodies. The
        // previous `status_code == 200 ? "application/json" : "text/plain"`
        // labelled every successful response JSON no matter which serializer
        // produced it, which is the defect Requirement 9.4 exists to remove.
        proxygen::ResponseBuilder builder(downstream_);
        builder.status(static_cast<std::uint16_t>(status_code), proxygen_status_reason(status_code))
            .header(proxygen::HTTP_HEADER_CONTENT_TYPE, response_media_type);
        // Name what the server *would* have taken, so the peer's retry converges
        // in one round trip instead of walking its whole preference list (W3C
        // LDP 1.0 7.1). 415 only: on any other status it either does not apply
        // or would be answering a question nobody asked. Built in steps rather
        // than as one fluent chain because the header is conditional.
        if (status_code == 415) {
            if (auto accept_post = _server->accept_post_header(); !accept_post.empty()) {
                builder.header(std::string(kythira::header_accept_post), accept_post);
            }
        }
        builder.body(std::move(response_body)).sendWithEOM();
    }

    auto requestComplete() noexcept -> void override {
        _server->request_finished(_request_id);
        delete this;
    }

    auto onError(proxygen::ProxygenError) noexcept -> void override {
        _server->request_finished(_request_id);
        delete this;
    }

private:
    proxygen_server<Types>* _server;
    std::string _path;
    std::string _request_media_type;
    std::vector<std::string> _accepted;
    std::vector<std::byte> _body;
    std::size_t _request_id{0};
};

template<typename Types> class rpc_handler_factory final : public proxygen::RequestHandlerFactory {
public:
    explicit rpc_handler_factory(proxygen_server<Types>* server) : _server(server) {}

    auto onServerStart(folly::EventBase*) noexcept -> void override {}
    auto onServerStop() noexcept -> void override {}

    auto onRequest(proxygen::RequestHandler*, proxygen::HTTPMessage*) noexcept
        -> proxygen::RequestHandler* override {
        return new rpc_request_handler<Types>(_server);
    }

private:
    proxygen_server<Types>* _server;
};

}  // namespace proxygen_detail

// ---------------------------------------------------------------------------
// proxygen_server
// ---------------------------------------------------------------------------

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
proxygen_server<Types>::proxygen_server(
    std::string bind_address, std::uint16_t bind_port, proxygen_server_config config,
    metrics_type metrics, std::shared_ptr<folly::IOThreadPoolExecutorBase> io_executor)
    : _bind_address(std::move(bind_address)),
      _bind_port(bind_port),
      _config(std::move(config)),
      _metrics(std::move(metrics)),
      _io_executor(std::move(io_executor)) {
    validate_certificate_files();
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
proxygen_server<Types>::~proxygen_server() {
    disable_auto_reload();
    stop();
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::validate_certificate_files() const -> void {
    proxygen_validate_tls_version_range(_config.min_tls_version, _config.max_tls_version);
    proxygen_validate_cipher_suites(_config.cipher_suites);
    if (!_config.enable_ssl) {
        return;
    }
    if (_config.ssl_cert_path.empty() || _config.ssl_key_path.empty()) {
        throw kythira::ssl_configuration_error(
            "enable_ssl is set but ssl_cert_path/ssl_key_path are not both provided");
    }
    proxygen_validate_certificate_file(_config.ssl_cert_path);
    proxygen_check_certificate_expiration(_config.ssl_cert_path);
    proxygen_validate_private_key_file(_config.ssl_key_path);
    proxygen_validate_certificate_key_pair(_config.ssl_cert_path, _config.ssl_key_path);
    if (!_config.ca_cert_path.empty()) {
        proxygen_validate_certificate_file(_config.ca_cert_path);
    } else if (_config.require_client_cert) {
        throw kythira::ssl_configuration_error(
            "require_client_cert is set but ca_cert_path is not provided");
    }
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::build_ssl_context_config() -> wangle::SSLContextConfig {
    wangle::SSLContextConfig ssl_config;
    ssl_config.setCertificate(_config.ssl_cert_path, _config.ssl_key_path, "");
    ssl_config.sslCiphers = _config.cipher_suites.empty()
                                ? wangle::SSLContextConfig::getDefaultCiphers()
                                : _config.cipher_suites;
    // wangle::SSLContextConfig::sslVersion is a single floor, not a
    // min/max range -- Requirement 11.3: no direct equivalent for
    // max_tls_version exists on this config surface (spike-notes.md
    // Finding 5); TLS 1.3 is negotiated automatically whenever the peer
    // offers it regardless of this floor, so max_tls_version is accepted
    // and validated (validate_certificate_files, above) but has no
    // further effect server-side beyond that validation.
    if (_config.min_tls_version == "TLSv1.3") {
        ssl_config.sslVersion = folly::SSLContext::TLSv1_3;
    } else {
        ssl_config.sslVersion = folly::SSLContext::TLSv1_2;
    }
    if (!_config.ca_cert_path.empty()) {
        ssl_config.clientCAFile = _config.ca_cert_path;
    }
    ssl_config.clientVerification =
        _config.require_client_cert
            ? folly::SSLContext::VerifyClientCertificate::ALWAYS
            : (_config.ca_cert_path.empty()
                   ? folly::SSLContext::VerifyClientCertificate::DO_NOT_REQUEST
                   : folly::SSLContext::VerifyClientCertificate::IF_PRESENTED);
    ssl_config.isDefault = true;
    return ssl_config;
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::reload_tls_material() -> void {
    if (!_config.enable_ssl) {
        throw kythira::ssl_configuration_error("SSL is not enabled on this server");
    }
    validate_certificate_files();
    // Requirement 7.2: re-read the certificate/key paths already configured
    // on the running acceptors without closing the listener or dropping
    // established connections -- the direct Proxygen equivalent of
    // Beast/cpp-httplib's own "swap in a fresh context" reload, just
    // performed in place rather than by constructing a whole new config
    // object.
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_http_server) {
        return;
    }
    // Deliberately *not* proxygen::HTTPServer::updateTLSCredentials(), which
    // is fire-and-forget: it posts `acceptor->reloadSSLContextConfigs()` to
    // each acceptor's EventBase via runInEventBaseThread(), capturing a raw
    // `wangle::Acceptor*`, and returns immediately without waiting. That
    // makes reload_tls_material() a lie in two ways, both of which bit:
    //
    //  1. It returns before the reload has happened, so a caller that
    //     rotates material and then retires the old files (or, in
    //     enable_auto_reload's case, records _last_reloaded_cert_mtime as
    //     though the reload took effect) races the queued task. When the
    //     task finally runs, it re-reads a path whose file is already gone
    //     and logs "Failed to re-configure TLS ... will keep old config" --
    //     the server silently keeps serving the *old* certificate.
    //  2. The queued lambda holds a raw Acceptor*. stop() and ~HTTPServer
    //     destroy the acceptors without draining those tasks, so a reload
    //     queued shortly before shutdown dereferences a freed Acceptor.
    //
    // Both surfaced together as an intermittent (~50% of CI runs) segfault
    // in proxygen_transport_test: server_reload_tls_material queues a
    // reload, deletes the cert file, and tears the server down, and the
    // orphaned task then fired several tests later -- crashing whichever
    // case happened to be running (concurrent_rpcs_to_same_node, which has
    // nothing to do with TLS), preceded by exactly those two SSL errors.
    //
    // So run proxygen's own per-acceptor logic here, but synchronously.
    // runImmediatelyOrRunInEventBaseThreadAndWait (rather than
    // runInEventBaseThreadAndWait) keeps this safe if it is ever reached
    // from an acceptor's own EventBase thread, where waiting on that thread
    // would deadlock; inline execution there is equally synchronous.
    //
    // This blocks on the acceptor threads while holding _mutex, which is
    // safe only because no acceptor-thread code path takes _mutex: dispatch()
    // reads its handlers unlocked, and _mutex is otherwise held only by the
    // register_*_handler setters and this function, all called from
    // application threads. Anything added later that acquires _mutex from an
    // acceptor's EventBase would deadlock against this wait.
    //
    // Note this does not make reload failures *throw* -- Acceptor catches
    // and logs them internally. The all-or-nothing contract (Requirement
    // 7.2-7.3) comes from validate_certificate_files() above, which runs
    // before anything is handed to the acceptors; what changes here is that
    // by the time this returns, the new material is actually in effect.
    _http_server->forEachAcceptor([](wangle::Acceptor* acceptor) {
        if (acceptor == nullptr || !acceptor->isSSL()) {
            return;
        }
        auto* evb = acceptor->getEventBase();
        if (evb == nullptr) {
            return;
        }
        evb->runImmediatelyOrRunInEventBaseThreadAndWait(
            [acceptor] { acceptor->reloadSSLContextConfigs(); });
    });
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::enable_auto_reload(std::chrono::seconds poll_interval) -> void {
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
                    metric.set_metric_name("proxygen_http.server.tls_reload.failed");
                    metric.add_one();
                    metric.emit();
                }
            }
            std::this_thread::sleep_for(poll_interval);
        }
    });
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::disable_auto_reload() -> void {
    if (_auto_reload_thread.joinable()) {
        _auto_reload_thread.request_stop();
        _auto_reload_thread.join();
    }
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::register_request_vote_handler(
    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)> handler)
    -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _request_vote_handler = std::move(handler);
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::register_append_entries_handler(
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
        handler) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _append_entries_handler = std::move(handler);
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::register_install_snapshot_handler(
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)>
        handler) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _install_snapshot_handler = std::move(handler);
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::start() -> void {
    {
        std::lock_guard<std::mutex> lock(_start_mutex);
        if (_running.load()) {
            return;
        }
    }

    proxygen::HTTPServerOptions options;
    options.threads = 1;
    options.idleTimeout = _config.request_timeout;
    options.handlerFactories = proxygen::RequestHandlerChain()
                                   .addThen<proxygen_detail::rpc_handler_factory<Types>>(this)
                                   .build();

    _http_server = std::make_unique<proxygen::HTTPServer>(std::move(options));

    proxygen::HTTPServer::IPConfig ip_config(folly::SocketAddress(_bind_address, _bind_port),
                                             proxygen::HTTPServer::Protocol::HTTP);
    if (_config.enable_ssl) {
        ip_config.sslConfigs.push_back(build_ssl_context_config());
    }
    _http_server->bind({ip_config});

    // Requirement 5.1: HTTPServer::start() genuinely blocks the calling
    // thread until stop() (spike-notes.md Finding 1) -- own and join a
    // dedicated thread to run it on, the same shape cpp_httplib_server's
    // own _server_thread already uses for httplib::Server::listen()'s
    // equally blocking call, signaling readiness back via start()'s own
    // onSuccess/onError callbacks (which fire from the event loop, not a
    // fixed sleep or a race).
    _server_thread = std::thread([this] {
        _http_server->start(
            [this] {
                std::lock_guard<std::mutex> lock(_start_mutex);
                _start_signaled = true;
                _running.store(true);
                _start_cv.notify_all();
            },
            [this](std::exception_ptr ex) {
                std::lock_guard<std::mutex> lock(_start_mutex);
                _start_signaled = true;
                _start_error = ex;
                _start_cv.notify_all();
            },
            nullptr, _io_executor);
    });

    std::unique_lock<std::mutex> lock(_start_mutex);
    _start_cv.wait(lock, [this] { return _start_signaled; });
    if (_start_error) {
        auto ex = _start_error;
        _start_error = nullptr;
        _start_signaled = false;
        lock.unlock();
        _server_thread.join();
        std::rethrow_exception(ex);
    }

    auto metric = _metrics;
    metric.set_metric_name("proxygen_http.server.started");
    metric.add_one();
    metric.emit();
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::stop() -> void {
    if (!_running.load()) {
        return;
    }
    // Requirement 5.2, Property 8: stop accepting first, then drain
    // in-flight requests -- proxygen::HTTPServer::stop() itself "drop[s]
    // all connections immediately" per its own header comment, the
    // opposite of what's needed here, so this feature's own drain step
    // runs in front of it (design.md Phase 3), mirroring the same
    // active-drain addendum boost_beast_server::stop() needed.
    _http_server->stopListening();

    {
        std::unique_lock<std::mutex> drain_lock(_drain_mutex);
        _drain_cv.wait(drain_lock, [this] {
            std::lock_guard<std::mutex> requests_lock(_requests_mutex);
            return _live_requests == 0;
        });
    }

    _http_server->stop();
    _running.store(false);
    if (_server_thread.joinable()) {
        _server_thread.join();
    }

    auto metric = _metrics;
    metric.set_metric_name("proxygen_http.server.stopped");
    metric.add_one();
    metric.emit();
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::is_running() const -> bool {
    return _running.load();
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::register_request() -> std::size_t {
    std::lock_guard<std::mutex> lock(_requests_mutex);
    ++_live_requests;
    return _next_request_id++;
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::request_finished(std::size_t /*request_id*/) -> void {
    {
        std::lock_guard<std::mutex> lock(_requests_mutex);
        --_live_requests;
    }
    std::lock_guard<std::mutex> drain_lock(_drain_mutex);
    _drain_cv.notify_all();
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::default_media_type() const -> std::string {
    return _registry.default_media_type();
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::accept_post_header() const -> std::string {
    return format_accept_header(_registry.preferred_media_types());
}

template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
auto proxygen_server<Types>::dispatch(std::string_view target, const std::vector<std::byte>& body,
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
        received_metric.set_metric_name("proxygen_http.server.request.received");
        received_metric.add_dimension("rpc_type", std::string(rpc_type));
        received_metric.add_one();
        received_metric.emit();

        if (!handler) {
            status_code = 500;
            response_body = "Handler not registered";
            return;
        }

        // 415 before the handler runs (Requirement 4.3, 4.4). Distinct from the
        // 400 below: 415 means "we do not speak this encoding", 400 means "we
        // speak it and your bytes were wrong". Collapsing the two would tell a
        // peer to fix its payload when it needs to change its format.
        if (!_registry.supports(request_media_type)) {
            status_code = 415;
            response_body = "Unsupported Content-Type: " + request_media_type;
            auto error_metric = _metrics;
            error_metric.set_metric_name("proxygen_http.server.error");
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
            error_metric.set_metric_name("proxygen_http.server.error");
            error_metric.add_dimension("error_type", "unsupported_media_type");
            error_metric.add_dimension("media_type", kythira::format_accept_header(accepted));
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
            // Encoded in the negotiated type, which need not match the
            // request's -- a peer may post CBOR and ask for JSON back.
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
        latency_metric.set_metric_name("proxygen_http.server.request.latency");
        latency_metric.add_dimension("rpc_type", std::string(rpc_type));
        latency_metric.add_dimension("status_code", std::to_string(status_code));
        latency_metric.add_dimension("media_type", response_media_type);
        latency_metric.add_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_time));
        latency_metric.emit();
    };

    if (target == proxygen_detail::proxygen_endpoint_request_vote) {
        handle
            .template operator()<kythira::request_vote_request<>, kythira::request_vote_response<>>(
                _request_vote_handler, "request_vote");
    } else if (target == proxygen_detail::proxygen_endpoint_append_entries) {
        handle.template
        operator()<kythira::append_entries_request<>, kythira::append_entries_response<>>(
            _append_entries_handler, "append_entries");
    } else if (target == proxygen_detail::proxygen_endpoint_install_snapshot) {
        handle.template
        operator()<kythira::install_snapshot_request<>, kythira::install_snapshot_response<>>(
            _install_snapshot_handler, "install_snapshot");
    } else {
        status_code = 404;
        response_body = "Not Found";
        auto error_metric = _metrics;
        error_metric.set_metric_name("proxygen_http.server.error");
        error_metric.add_dimension("error_type", "not_found");
        error_metric.add_one();
        error_metric.emit();
    }
}

}  // namespace kythira
