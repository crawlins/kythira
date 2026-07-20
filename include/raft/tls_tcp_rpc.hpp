#pragma once

/// @file tls_tcp_rpc.hpp
/// @brief Mutual-TLS-wrapped Raft RPC transport (`.kiro/specs/ca-cluster-rpc-mtls/`).
///
/// `tls_tcp_rpc_client`/`tls_tcp_rpc_server` satisfy the same
/// `kythira::network_client`/`network_server` concepts as `tcp_rpc_client`/
/// `tcp_rpc_server` (`tcp_rpc.hpp`, untouched by this file) and reuse that
/// header's wire framing (`[4-byte big-endian length][JSON payload]`),
/// `json_rpc_serializer`, and raw-socket connect/accept helpers — the only
/// thing that changes is what carries the framed bytes: an OpenSSL `SSL*`
/// per connection instead of a bare file descriptor.
///
/// Both classes are cheap-to-copy handles wrapping a `shared_ptr` to the
/// actual mutable transport state (`tls_detail::server_impl`/`client_impl`),
/// not move-only value types like `tcp_rpc_server`. This is deliberate, not
/// an arbitrary style choice: `kythira::node<Types>` stores its
/// `network_server_type`/`network_client_type` by value with no accessor
/// back to it (`raft.hpp`), but `ca_cluster_node`'s maintenance thread
/// (Requirements 5-7) needs to call `reload_identity()`/
/// `reload_trust_policy()` on the EXACT transport instance `node<Types>` is
/// actually using to accept/dial connections — not a logically-equivalent
/// but distinct copy. Keeping an independent handle copy in `main()`
/// (constructed before the other copy is handed to `node_config`) and
/// calling reload methods on it reconfigures the same underlying transport,
/// since both handles share one `impl`.
///
/// Trust decisions are deliberately NOT made via OpenSSL's own certificate-
/// chain verification machinery at the TLS layer. Both classes set
/// `SSL_VERIFY_PEER` (server: `| SSL_VERIFY_FAIL_IF_NO_PEER_CERT`) with a
/// permissive verify callback (`raft::testing::accept_any_peer_certificate`,
/// already used the same way by `ca_cluster_node`'s client-facing
/// `/v1/certificates/renew` route) that lets any *presented* certificate
/// through the handshake — `SSL_VERIFY_FAIL_IF_NO_PEER_CERT` alone already
/// rejects an *absent* one. The real decision — does this presented
/// certificate satisfy the currently active `tls_rpc_trust_policy`? — runs
/// immediately after the handshake, before any framed RPC payload is sent or
/// read, and is a plain in-process comparison (fingerprint match or
/// `X509_verify_cert` against a specific root), not something baked into a
/// per-connection `SSL_CTX*`. This is what makes `reload_trust_policy()` a
/// trivial member swap rather than an `SSL_CTX` reconfiguration: the
/// dual-trust window (`tls_rpc_trust_policy::either`) and cutover
/// (narrowing back to one accepted credential) are just different values of
/// one struct, read fresh on every new connection.
///
/// Compiled only when `KYTHIRA_HAS_OPENSSL` is defined (root
/// `CMakeLists.txt`, set on the `certificate_authority` target) — this file
/// cannot function without OpenSSL, unlike `ca_http_helpers.hpp`'s optional-
/// TLS degradation. `cert_chains_to_root()`/`accept_any_peer_certificate()`
/// (`ca_http_helpers.hpp`) and `sha256_fingerprint_hex_bare()`
/// (`ca_bootstrap_client.hpp`) are reused rather than reimplemented
/// (Requirement 1.5) — both are already unconditionally relied upon by
/// `cmd/ca_cluster_node/main.cpp` today under the same
/// `CPPHTTPLIB_OPENSSL_SUPPORT`/`KYTHIRA_HAS_OPENSSL` coupling this file
/// inherits.

#ifdef KYTHIRA_HAS_OPENSSL

#include <raft/ca_bootstrap_client.hpp>
#include <raft/ca_http_helpers.hpp>
#include <raft/exceptions.hpp>
#include <raft/future.hpp>
#include <raft/json_serializer.hpp>
#include <raft/network.hpp>
#include <raft/tcp_rpc.hpp>
#include <raft/types.hpp>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace kythira {

// ── tls_rpc_trust_policy / tls_tcp_rpc_config (Requirement 1.3, 6) ─────────

/// Represents the set of credentials an RPC transport currently accepts from
/// a peer as a first-class value — the dual-trust window (Requirement 6.1)
/// is `either(fingerprint, root_pem)`, not a separate boolean-flag code
/// path; cutover (Requirement 6.2) is just replacing that value with
/// `ca_root_only(root_pem)`.
struct tls_rpc_trust_policy {
    std::optional<std::string> bootstrap_fingerprint_hex;  // Requirement 2.2
    std::optional<std::string> ca_root_pem;                // Requirement 6.2

    // false for a null `presented` (no certificate at all — server-side
    // SSL_VERIFY_FAIL_IF_NO_PEER_CERT already rejects this case at the TLS
    // layer, but the client-side connect path has no equivalent flag, so
    // this function is the single place both sides ultimately agree an
    // absent certificate is never acceptable).
    [[nodiscard]] auto accepts(X509* presented) const -> bool {
        if (presented == nullptr) {
            return false;
        }
        if (bootstrap_fingerprint_hex.has_value()) {
            std::string observed;
            try {
                observed =
                    raft::testing::ca_bootstrap_detail::sha256_fingerprint_hex_bare(presented);
            } catch (const std::exception&) {
                observed.clear();
            }
            if (!observed.empty() && observed == *bootstrap_fingerprint_hex) {
                return true;
            }
        }
        if (ca_root_pem.has_value() &&
            raft::testing::cert_chains_to_root(presented, *ca_root_pem)) {
            return true;
        }
        return false;
    }
};

[[nodiscard]] inline auto pinned_fingerprint(std::string hex) -> tls_rpc_trust_policy {
    return tls_rpc_trust_policy{std::move(hex), std::nullopt};
}

[[nodiscard]] inline auto ca_root_only(std::string root_pem) -> tls_rpc_trust_policy {
    return tls_rpc_trust_policy{std::nullopt, std::move(root_pem)};
}

[[nodiscard]] inline auto either(std::string hex, std::string root_pem) -> tls_rpc_trust_policy {
    return tls_rpc_trust_policy{std::move(hex), std::move(root_pem)};
}

struct tls_tcp_rpc_config {
    std::string cert_path;  // this node's currently presented identity
    std::string key_path;
    tls_rpc_trust_policy trust_policy;
};

namespace tls_detail {

// SSL_read/SSL_write analogues of tcp_detail::read_all/write_all — same
// wire framing, over an SSL* instead of a raw fd.

inline auto write_all(SSL* ssl, const void* buf, std::size_t n) -> bool {
    const auto* p = static_cast<const char*>(buf);
    while (n > 0) {
        int w = SSL_write(ssl, p, static_cast<int>(n));
        if (w <= 0) {
            return false;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

inline auto read_all(SSL* ssl, void* buf, std::size_t n) -> bool {
    auto* p = static_cast<char*>(buf);
    while (n > 0) {
        int r = SSL_read(ssl, p, static_cast<int>(n));
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

inline auto frame_send(SSL* ssl, std::string_view payload) -> bool {
    auto len = htonl(static_cast<std::uint32_t>(payload.size()));
    return write_all(ssl, &len, 4) && write_all(ssl, payload.data(), payload.size());
}

inline auto frame_recv(SSL* ssl) -> std::optional<std::string> {
    std::uint32_t net_len{};
    if (!read_all(ssl, &net_len, 4)) {
        return std::nullopt;
    }
    std::uint32_t len = ntohl(net_len);
    if (len == 0 || len > 64u * 1024u * 1024u) {
        return std::nullopt;
    }
    std::string buf(len, '\0');
    if (!read_all(ssl, buf.data(), len)) {
        return std::nullopt;
    }
    return buf;
}

// SSL_write()/SSL_read() ultimately call send()/recv() on the underlying
// fd, which raise SIGPIPE (default disposition: process termination) when
// writing to a connection the peer has already closed — e.g. this side is
// mid-write to a peer that just closed the socket after its own
// post-handshake trust-policy check rejected the connection (Requirement
// 1.2). The write already fails cleanly via this file's own `if (w <= 0)
// return false;` checks; the process should not die over it. Ignoring
// SIGPIPE process-wide is the standard mitigation for exactly this
// scenario and is idempotent to call from multiple constructors.
inline auto ignore_sigpipe_once() -> void {
    static bool done = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void)done;
}

// Loads `cert_path`/`key_path` into `ctx`, throwing std::runtime_error
// (naming both paths) on any failure — shared by server construction/
// reload_identity() and each client call's per-connection SSL_CTX* setup.
inline auto load_identity(SSL_CTX* ctx, const std::string& cert_path, const std::string& key_path,
                          const char* who) -> void {
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        throw std::runtime_error(std::string(who) + ": failed to load identity cert/key: " +
                                 cert_path + " / " + key_path);
    }
}

struct ssl_conn_guard {
    SSL* ssl;
    ~ssl_conn_guard() {
        if (ssl != nullptr) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
    }
};

struct fd_guard {
    int fd;
    ~fd_guard() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

// ── client_impl (Requirement 1.1, 1.2) ──────────────────────────────────────
//
// Same per-call-connect model as tcp_rpc_client: one TCP connection, one TLS
// handshake, one RPC, then closed. The SSL_CTX* itself, however, is a
// single long-lived object (constructed once, updated only by
// reload_identity()) rather than rebuilt per call: SSL_CTX_use_certificate_
// chain_file()/SSL_CTX_use_PrivateKey_file() read and parse the identity
// files from disk every time they're called, and doing that on every
// single RPC — at this project's default 50ms heartbeat cadence, to
// potentially several peers — is disk I/O and X.509 parsing on the
// critical path of Raft's own liveness timers. A real 3-node cluster
// under ordinary CI host contention measurably could not sustain
// elections with per-call SSL_CTX construction (confirmed during this
// spec's implementation: heartbeats routinely missed their timeout,
// driving terms into the hundreds). Requirement 1.3's "reload" still holds
// exactly as documented — reload_identity() updates the *next* call's
// identity — it just does so by mutating the shared SSL_CTX* in place
// (mirroring tls_tcp_rpc_server's own reload_identity()) instead of
// discarding and rebuilding one every time.
//
// Like tcp_rpc_client::call() (tcp_rpc.hpp), the actual connect/handshake/
// send/recv sequence runs on a private thread pool rather than inline on
// the calling thread — a node broadcasting RequestVote/AppendEntries to
// multiple peers in a loop (node<Types>::start_election(),
// replicate_to_followers()) needs to actually reach all of them
// concurrently; a peer that's gone away can otherwise stall every
// subsequent peer in that same broadcast round, since SO_SNDTIMEO/
// SO_RCVTIMEO don't bound connect()'s own blocking time on Linux. A
// private pool, not folly::getGlobalCPUExecutor(): the global CPU executor
// aborts unless folly::init() ran first, which plain Boost.Test unit
// tests exercising this class directly never do.
class client_impl {
public:
    using serializer_t = json_rpc_serializer<std::vector<std::byte>>;

    static constexpr std::size_t k_rpc_thread_pool_size = 8;

    explicit client_impl(tls_tcp_rpc_config config)
        : _config(config),
          _executor(std::make_shared<folly::CPUThreadPoolExecutor>(k_rpc_thread_pool_size)) {
        ignore_sigpipe_once();
        _ctx = SSL_CTX_new(TLS_client_method());
        if (_ctx == nullptr) {
            throw std::runtime_error("tls_tcp_rpc_client: SSL_CTX_new failed");
        }
        SSL_CTX_set_verify(_ctx, SSL_VERIFY_PEER, raft::testing::accept_any_peer_certificate);
        try {
            load_identity(_ctx, config.cert_path, config.key_path, "tls_tcp_rpc_client");
        } catch (...) {
            SSL_CTX_free(_ctx);
            _ctx = nullptr;
            throw;
        }
    }

    ~client_impl() {
        if (_ctx != nullptr) {
            SSL_CTX_free(_ctx);
        }
    }

    client_impl(const client_impl&) = delete;
    auto operator=(const client_impl&) -> client_impl& = delete;
    client_impl(client_impl&&) = delete;
    auto operator=(client_impl&&) -> client_impl& = delete;

    void add_peer(std::uint64_t id, std::string host, std::uint16_t port) {
        _peers.add_peer(id, std::move(host), port);
    }

    auto reload_identity(std::string cert_path, std::string key_path) -> void {
        std::lock_guard<std::mutex> lock(_ctx_mu);
        load_identity(_ctx, cert_path, key_path, "tls_tcp_rpc_client");
        _config.cert_path = std::move(cert_path);
        _config.key_path = std::move(key_path);
    }

    auto reload_trust_policy(tls_rpc_trust_policy policy) -> void {
        std::lock_guard<std::mutex> lock(_ctx_mu);
        _config.trust_policy = std::move(policy);
    }

    template<typename Resp, typename Deser>
    auto call(std::uint64_t target, const std::vector<std::byte>& payload,
              std::chrono::milliseconds timeout, Deser deser) -> Future<Resp> {
        auto peer = _peers.lookup(target);
        if (!peer) {
            return FutureFactory::makeExceptionalFuture<Resp>(std::make_exception_ptr(
                network_exception("tls_tcp_rpc_client: unknown peer " + std::to_string(target))));
        }

        // SSL_new() and the trust-policy snapshot stay synchronous, still
        // under _ctx_mu exactly as before — only the blocking network I/O
        // that follows (connect/handshake/send/recv) moves to the
        // background, since that's what a slow/dead peer stalls.
        SSL* raw_ssl = nullptr;
        tls_rpc_trust_policy policy_snapshot;
        {
            std::lock_guard<std::mutex> lock(_ctx_mu);
            raw_ssl = SSL_new(_ctx);
            policy_snapshot = _config.trust_policy;
        }
        if (raw_ssl == nullptr) {
            return FutureFactory::makeExceptionalFuture<Resp>(
                std::make_exception_ptr(network_exception("tls_tcp_rpc_client: SSL_new failed")));
        }

        Promise<Resp> promise;
        auto future = promise.getFuture();

        std::string host = peer->first;
        std::uint16_t port = peer->second;

        _executor->add([promise = std::move(promise), raw_ssl, host, port, payload, timeout,
                        policy_snapshot, deser]() mutable {
            ssl_conn_guard sslg{raw_ssl};

            int fd = tcp_detail::connect_to(host, port, timeout);
            if (fd < 0) {
                promise.setException(std::make_exception_ptr(network_exception(
                    "tls_tcp_rpc_client: connect failed to " + host + ":" + std::to_string(port))));
                return;
            }
            fd_guard fdg{fd};

            SSL_set_fd(raw_ssl, fd);
            if (SSL_connect(raw_ssl) != 1) {
                promise.setException(std::make_exception_ptr(
                    network_exception("tls_tcp_rpc_client: TLS handshake failed connecting to " +
                                      host + ":" + std::to_string(port))));
                return;
            }

            X509* presented = SSL_get1_peer_certificate(raw_ssl);
            bool trusted = policy_snapshot.accepts(presented);
            if (presented != nullptr) {
                X509_free(presented);
            }
            if (!trusted) {
                promise.setException(std::make_exception_ptr(network_exception(
                    "tls_tcp_rpc_client: peer certificate rejected by trust policy: " + host + ":" +
                    std::to_string(port))));
                return;
            }

            if (!frame_send(raw_ssl, tcp_detail::bytes_to_str(payload))) {
                promise.setException(
                    std::make_exception_ptr(network_exception("tls_tcp_rpc_client: send failed")));
                return;
            }

            auto resp = frame_recv(raw_ssl);
            if (!resp) {
                promise.setException(
                    std::make_exception_ptr(network_exception("tls_tcp_rpc_client: recv failed")));
                return;
            }

            try {
                promise.setValue(deser(tcp_detail::str_to_bytes(*resp)));
            } catch (...) {
                promise.setException(std::current_exception());
            }
        });

        return future;
    }

private:
    tcp_detail::peer_registry<std::uint64_t> _peers;
    SSL_CTX* _ctx{nullptr};
    std::mutex _ctx_mu;  // guards _ctx's loaded identity AND _config.trust_policy
    tls_tcp_rpc_config _config;
    std::shared_ptr<folly::CPUThreadPoolExecutor> _executor;
};

// ── server_impl (Requirement 1.1, 1.2) ──────────────────────────────────────
//
// Owns one long-lived SSL_CTX* (identity reloaded in place via
// reload_identity(), Requirement 1.3/6.2/7.2); the trust policy is re-read
// fresh from _config on every accepted connection, so reload_trust_policy()
// never touches the SSL_CTX* at all.
class server_impl {
public:
    using rv_fn = std::function<request_vote_response<>(const request_vote_request<>&)>;
    using pv_fn = std::function<request_pre_vote_response<>(const request_pre_vote_request<>&)>;
    using ae_fn = std::function<append_entries_response<>(const append_entries_request<>&)>;
    using is_fn = std::function<install_snapshot_response<>(const install_snapshot_request<>&)>;
    using serializer_t = json_rpc_serializer<std::vector<std::byte>>;

    server_impl(std::uint16_t port, tls_tcp_rpc_config config)
        : _port(port), _config(std::move(config)) {
        ignore_sigpipe_once();
        _ctx = SSL_CTX_new(TLS_server_method());
        if (_ctx == nullptr) {
            throw std::runtime_error("tls_tcp_rpc_server: SSL_CTX_new failed");
        }
        SSL_CTX_set_verify(_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                           raft::testing::accept_any_peer_certificate);
        try {
            load_identity(_ctx, _config.cert_path, _config.key_path, "tls_tcp_rpc_server");
        } catch (...) {
            SSL_CTX_free(_ctx);
            _ctx = nullptr;
            throw;
        }
    }

    ~server_impl() {
        stop();
        if (_ctx != nullptr) {
            SSL_CTX_free(_ctx);
        }
    }

    server_impl(const server_impl&) = delete;
    auto operator=(const server_impl&) -> server_impl& = delete;
    server_impl(server_impl&&) = delete;
    auto operator=(server_impl&&) -> server_impl& = delete;

    void register_request_vote_handler(rv_fn h) { _rv = std::move(h); }
    // Satisfies kythira::network_server_with_pre_vote (include/raft/network.hpp).
    void register_request_pre_vote_handler(pv_fn h) { _pv = std::move(h); }
    void register_append_entries_handler(ae_fn h) { _ae = std::move(h); }
    void register_install_snapshot_handler(is_fn h) { _is = std::move(h); }

    void start() {
        if (_running.exchange(true)) {
            return;
        }

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            _running = false;
            throw std::runtime_error("tls_tcp_rpc_server: socket()");
        }

        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(_port);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            _running = false;
            throw std::runtime_error("tls_tcp_rpc_server: bind() on port " + std::to_string(_port));
        }
        ::listen(fd, 256);
        _listen_fd = fd;
        _accept_thread = std::thread([this] { accept_loop(); });
    }

    void stop() {
        if (!_running.exchange(false)) {
            return;
        }
        int fd = _listen_fd.exchange(-1);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        if (_accept_thread.joinable()) {
            _accept_thread.join();
        }
    }

    [[nodiscard]] bool is_running() const noexcept { return _running.load(); }

    // Requirement 1.3/6.2/7.2: applied to the live SSL_CTX*, no listener
    // restart. Requirement 6.2's "already-established connections SHALL NOT
    // be forcibly dropped" holds structurally: reloading the certificate
    // chain/key on an SSL_CTX* only affects handshakes started afterward
    // (OpenSSL caches nothing about this change into already-completed SSL*
    // sessions).
    auto reload_identity(std::string cert_path, std::string key_path) -> void {
        std::lock_guard<std::mutex> lock(_ctx_mu);
        load_identity(_ctx, cert_path, key_path, "tls_tcp_rpc_server");
        _config.cert_path = std::move(cert_path);
        _config.key_path = std::move(key_path);
    }

    auto reload_trust_policy(tls_rpc_trust_policy policy) -> void {
        std::lock_guard<std::mutex> lock(_ctx_mu);
        _config.trust_policy = std::move(policy);
    }

private:
    void accept_loop() {
        while (_running) {
            int client = ::accept(_listen_fd.load(), nullptr, nullptr);
            if (client < 0) {
                break;
            }
            std::thread([this, client] {
                handle(client);
                ::close(client);
            }).detach();
        }
    }

    void handle(int fd) {
        // Unlike tcp_detail::connect_to() (client side), accept() never
        // set a read/write deadline on this fd — a plain-TCP handler
        // returns almost instantly either way, so tcp_rpc_server has
        // gotten away with the same omission. A TLS handshake plus
        // trust-policy check is slow enough (real asymmetric crypto, a
        // freshly spawned thread competing for CPU under host load) that
        // a client can legitimately give up (its own connect_to() timeout
        // elapses) and abandon the connection while this thread is still
        // correctly mid-handshake — and without a deadline here, that
        // thread then blocks on the next SSL_accept()/SSL_read() forever,
        // leaking one thread and one fd per stall. Confirmed during this
        // spec's implementation: under real multi-process contention this
        // compounded into cascading Raft instability that raising
        // election/heartbeat timeouts alone never fixed, since the actual
        // resource leak kept growing regardless of how patient the
        // *protocol* timeouts were.
        timeval sock_timeout{};
        sock_timeout.tv_sec = 30;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &sock_timeout, sizeof(sock_timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sock_timeout, sizeof(sock_timeout));

        SSL* raw_ssl = nullptr;
        tls_rpc_trust_policy policy_snapshot;
        {
            std::lock_guard<std::mutex> lock(_ctx_mu);
            raw_ssl = SSL_new(_ctx);
            policy_snapshot = _config.trust_policy;
        }
        if (raw_ssl == nullptr) {
            return;
        }

        SSL_set_fd(raw_ssl, fd);
        if (SSL_accept(raw_ssl) != 1) {
            SSL_free(raw_ssl);
            return;
        }
        ssl_conn_guard guard{raw_ssl};

        // Requirement 1.2: reject before any RPC payload is exchanged if the
        // presented certificate fails the currently active trust policy —
        // SSL_VERIFY_FAIL_IF_NO_PEER_CERT already handled the "no
        // certificate at all" case during the handshake above.
        X509* presented = SSL_get1_peer_certificate(raw_ssl);
        bool trusted = policy_snapshot.accepts(presented);
        if (presented != nullptr) {
            X509_free(presented);
        }
        if (!trusted) {
            return;
        }

        auto data = frame_recv(raw_ssl);
        if (!data.has_value()) {
            return;
        }

        std::string type = tcp_detail::extract_type_field(*data);
        auto bytes = tcp_detail::str_to_bytes(*data);
        try {
            std::vector<std::byte> resp;
            if (type == "request_vote_request" && _rv) {
                resp = _ser.serialize(_rv(_ser.deserialize_request_vote_request(bytes)));
            } else if (type == "request_pre_vote_request" && _pv) {
                resp = _ser.serialize(_pv(_ser.deserialize_request_pre_vote_request(bytes)));
            } else if (type == "append_entries_request" && _ae) {
                resp = _ser.serialize(_ae(_ser.deserialize_append_entries_request(bytes)));
            } else if (type == "install_snapshot_request" && _is) {
                resp = _ser.serialize(_is(_ser.deserialize_install_snapshot_request(bytes)));
            } else {
                return;
            }
            frame_send(raw_ssl, tcp_detail::bytes_to_str(resp));
        } catch (...) {
        }
    }

    std::uint16_t _port;
    std::atomic<int> _listen_fd{-1};
    std::atomic<bool> _running{false};
    std::thread _accept_thread;

    SSL_CTX* _ctx{nullptr};
    std::mutex _ctx_mu;  // guards _ctx's loaded identity AND _config
    tls_tcp_rpc_config _config;

    rv_fn _rv;
    pv_fn _pv;
    ae_fn _ae;
    is_fn _is;
    serializer_t _ser;
};

}  // namespace tls_detail

// ── tls_tcp_rpc_client / tls_tcp_rpc_server (public handles) ───────────────
//
// See this file's top-of-file comment for why these are copyable
// shared_ptr-backed handles rather than move-only value types.

class tls_tcp_rpc_client {
public:
    explicit tls_tcp_rpc_client(tls_tcp_rpc_config config)
        : _impl(std::make_shared<tls_detail::client_impl>(std::move(config))) {}

    void add_peer(std::uint64_t id, std::string host, std::uint16_t port) {
        _impl->add_peer(id, std::move(host), port);
    }

    auto reload_identity(std::string cert_path, std::string key_path) -> void {
        _impl->reload_identity(std::move(cert_path), std::move(key_path));
    }

    auto reload_trust_policy(tls_rpc_trust_policy policy) -> void {
        _impl->reload_trust_policy(std::move(policy));
    }

    auto send_request_vote(std::uint64_t target, const request_vote_request<>& req,
                           std::chrono::milliseconds timeout) -> Future<request_vote_response<>> {
        return _impl->call<request_vote_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_request_vote_response(d);
            });
    }

    // Satisfies kythira::network_client_with_pre_vote (include/raft/network.hpp).
    auto send_request_pre_vote(std::uint64_t target, const request_pre_vote_request<>& req,
                               std::chrono::milliseconds timeout)
        -> Future<request_pre_vote_response<>> {
        return _impl->call<request_pre_vote_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_request_pre_vote_response(d);
            });
    }

    auto send_append_entries(std::uint64_t target, const append_entries_request<>& req,
                             std::chrono::milliseconds timeout)
        -> Future<append_entries_response<>> {
        return _impl->call<append_entries_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_append_entries_response(d);
            });
    }

    auto send_install_snapshot(std::uint64_t target, const install_snapshot_request<>& req,
                               std::chrono::milliseconds timeout)
        -> Future<install_snapshot_response<>> {
        return _impl->call<install_snapshot_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_install_snapshot_response(d);
            });
    }

private:
    std::shared_ptr<tls_detail::client_impl> _impl;
    json_rpc_serializer<std::vector<std::byte>> _ser;
};

class tls_tcp_rpc_server {
public:
    tls_tcp_rpc_server(std::uint16_t port, tls_tcp_rpc_config config)
        : _impl(std::make_shared<tls_detail::server_impl>(port, std::move(config))) {}

    void register_request_vote_handler(tls_detail::server_impl::rv_fn h) {
        _impl->register_request_vote_handler(std::move(h));
    }
    void register_request_pre_vote_handler(tls_detail::server_impl::pv_fn h) {
        _impl->register_request_pre_vote_handler(std::move(h));
    }
    void register_append_entries_handler(tls_detail::server_impl::ae_fn h) {
        _impl->register_append_entries_handler(std::move(h));
    }
    void register_install_snapshot_handler(tls_detail::server_impl::is_fn h) {
        _impl->register_install_snapshot_handler(std::move(h));
    }

    void start() { _impl->start(); }
    void stop() { _impl->stop(); }
    [[nodiscard]] bool is_running() const noexcept { return _impl->is_running(); }

    auto reload_identity(std::string cert_path, std::string key_path) -> void {
        _impl->reload_identity(std::move(cert_path), std::move(key_path));
    }

    auto reload_trust_policy(tls_rpc_trust_policy policy) -> void {
        _impl->reload_trust_policy(std::move(policy));
    }

private:
    std::shared_ptr<tls_detail::server_impl> _impl;
};

// ── Concept assertions ────────────────────────────────────────────────────

static_assert(kythira::network_client<tls_tcp_rpc_client>,
              "tls_tcp_rpc_client must satisfy network_client");
static_assert(kythira::network_server<tls_tcp_rpc_server>,
              "tls_tcp_rpc_server must satisfy network_server");
static_assert(kythira::network_client_with_pre_vote<tls_tcp_rpc_client>,
              "tls_tcp_rpc_client must satisfy network_client_with_pre_vote");
static_assert(kythira::network_server_with_pre_vote<tls_tcp_rpc_server>,
              "tls_tcp_rpc_server must satisfy network_server_with_pre_vote");

}  // namespace kythira

#endif  // KYTHIRA_HAS_OPENSSL
