// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file redis_gateway_impl.hpp
/// @brief Definitions for redis_gateway.hpp.

#include <raft/asio_listeners.hpp>
#include <raft/redis_gateway.hpp>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <tuple>

namespace kythira {

// ─────────────────────────────────────────────────────────────────────────────
// connection
// ─────────────────────────────────────────────────────────────────────────────

/// One accepted socket, plaintext or TLS. Lives on an Asio strand for its
/// socket operations; command execution happens on the gateway's worker
/// pool, never on an I/O thread (see the threading note in redis_gateway.hpp).
template<typename Host, typename Logger, typename Metrics>
class redis_gateway<Host, Logger, Metrics>::connection
    : public std::enable_shared_from_this<redis_gateway<Host, Logger, Metrics>::connection> {
public:
    using tcp = boost::asio::ip::tcp;
    using ssl_stream = boost::asio::ssl::stream<tcp::socket>;

    connection(redis_gateway& gw, tcp::socket socket,
               std::shared_ptr<boost::asio::ssl::context> ssl_ctx)
        : _gw(gw), _strand(boost::asio::make_strand(gw._io)), _idle_timer(_strand) {
        boost::system::error_code ec;
        (void)socket.set_option(tcp::no_delay(true), ec);
        auto remote = socket.remote_endpoint(ec);
        if (!ec) {
            _session._rate_key = redis_auth_limiter::rate_key(remote.address());
            _session._source = remote.address().to_string() + ":" + std::to_string(remote.port());
        } else {
            _session._source = "unknown";
            _session._rate_key = "unknown";
        }
        if (ssl_ctx) {
            _ssl.emplace(std::move(socket), *ssl_ctx);
            _ssl_ctx = std::move(ssl_ctx);
        } else {
            _plain.emplace(std::move(socket));
        }
        _session._parser = resp_parser(gw._config._parser_limits);
        if (gw._config._allow_anonymous && gw._acl.empty()) {
            _session._identity = redis_identity{"anonymous", redis_role::admin, {""}};
        }
        _authenticated.store(_session._identity.has_value());
    }

    auto start() -> void {
        auto self = this->shared_from_this();
        boost::asio::post(_strand, [self] {
            if (self->_ssl) {
                self->do_handshake();
            } else {
                self->arm_idle();
                self->do_read();
            }
        });
    }

    /// Close from any thread; the work happens on the strand.
    auto close() -> void {
        auto self = this->shared_from_this();
        boost::asio::post(_strand, [self] { self->do_close(); });
    }

    [[nodiscard]] auto source() const -> const std::string& { return _session._source; }

private:
    auto lowest() -> tcp::socket& { return _ssl ? _ssl->next_layer() : *_plain; }

    auto do_handshake() -> void {
        auto self = this->shared_from_this();
        arm_idle();
        _ssl->async_handshake(
            boost::asio::ssl::stream_base::server, [self](const boost::system::error_code& ec) {
                if (ec) {
                    self->_gw._logger.log(
                        log_level::debug, "redis gateway: tls handshake failed",
                        {{"source", self->_session._source}, {"error", ec.message()}});
                    self->do_close();
                    return;
                }
                self->map_client_certificate();
                self->do_read();
            });
    }

    /// mTLS: a verified client certificate's subject may name an ACL user, in
    /// which case the connection is authenticated before its first command.
    auto map_client_certificate() -> void {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        X509* cert = SSL_get1_peer_certificate(_ssl->native_handle());
#else
        X509* cert = SSL_get_peer_certificate(_ssl->native_handle());
#endif
        if (cert == nullptr) {
            return;
        }
        std::string subject;
        if (BIO* bio = BIO_new(BIO_s_mem())) {
            X509_NAME_print_ex(bio, X509_get_subject_name(cert), 0, XN_FLAG_RFC2253);
            char* data = nullptr;
            long len = BIO_get_mem_data(bio, &data);
            if (len > 0 && data != nullptr) {
                subject.assign(data, static_cast<std::size_t>(len));
            }
            BIO_free(bio);
        }
        X509_free(cert);
        if (subject.empty()) {
            return;
        }
        if (auto id = _gw._acl.authenticate_certificate(subject)) {
            _session._identity = std::move(id);
            _session._internal = _session._identity->_user == _gw._config._internal_user;
            _authenticated.store(true);
            _gw.audit(_session, "TLS", "certificate accepted");
        } else {
            _gw.audit(_session, "TLS", "certificate subject not mapped");
        }
    }

    auto arm_idle() -> void {
        if (_gw._config._idle_timeout.count() <= 0) {
            return;
        }
        auto self = this->shared_from_this();
        _idle_timer.expires_after(_gw._config._idle_timeout);
        _idle_timer.async_wait([self](const boost::system::error_code& ec) {
            if (ec) {
                return;  // cancelled: activity or close
            }
            self->_gw._logger.log(log_level::debug, "redis gateway: idle timeout",
                                  {{"source", self->_session._source}});
            self->do_close();
        });
    }

    auto do_read() -> void {
        if (_closed || _read_pending) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_parse_held) {
                // A pre-auth command is executing; the worker resumes us.
                return;
            }
            if (_queue.size() >= _gw._config._max_inflight_per_connection) {
                // Backpressure: leave the bytes in the kernel until the worker
                // has drained the queue; it re-posts do_read when it has.
                _paused = true;
                return;
            }
        }
        _read_pending = true;
        auto self = this->shared_from_this();
        auto handler = [self](const boost::system::error_code& ec, std::size_t n) {
            self->_read_pending = false;
            self->on_read(ec, n);
        };
        if (_ssl) {
            _ssl->async_read_some(boost::asio::buffer(_read_buffer),
                                  boost::asio::bind_executor(_strand, handler));
        } else {
            _plain->async_read_some(boost::asio::buffer(_read_buffer),
                                    boost::asio::bind_executor(_strand, handler));
        }
    }

    auto on_read(const boost::system::error_code& ec, std::size_t n) -> void {
        if (ec || _closed) {
            do_close();
            return;
        }
        arm_idle();
        _gw._stats._bytes_in += n;
        try {
            _session._parser.feed(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(_read_buffer.data()), n));
        } catch (const resp_protocol_error& e) {
            protocol_error(e);
            return;
        }
        if (drain_parser()) {
            kick();
            do_read();
        }
    }

    /// Redis closes on a protocol error; so do we, after saying why.
    auto protocol_error(const resp_protocol_error& e) -> void {
        queue_reply(_session._writer.error(std::string("ERR Protocol error: ") + e.what()));
        _session._closing = true;
        _close_after_flush = true;
        flush();
    }

    /// Strand only. Parse and queue what the parser holds. Before AUTH
    /// succeeds the parser runs under the small pre-auth limits and yields
    /// one command at a time: the next is not parsed (nor more bytes read)
    /// until the worker has executed this one, so a pipelined `AUTH` + large
    /// `SET` is parsed under the limits that apply once AUTH has run, as in
    /// Redis, which executes each command before parsing the next. Returns
    /// false after a protocol error.
    auto drain_parser() -> bool {
        bool authenticated = _authenticated.load();
        _session._parser.set_limits(authenticated
                                        ? _gw._config._parser_limits
                                        : resp_pre_auth_limits(_gw._config._parser_limits));
        try {
            while (true) {
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    if (_parse_held) {
                        break;
                    }
                }
                auto cmd = _session._parser.next();
                if (!cmd.has_value()) {
                    break;
                }
                enqueue(std::move(*cmd), !authenticated);
            }
        } catch (const resp_protocol_error& e) {
            protocol_error(e);
            return false;
        }
        return true;
    }

    auto enqueue(resp_command cmd, bool hold) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        queued_command q;
        q._bytes = 0;
        for (const auto& a : cmd._argv) {
            q._bytes += a.size();
        }
        // Total in-flight memory bound (Requirement 13.5): shed with a
        // retryable error rather than queue past the budget. The reply
        // is queued in order so pipelining stays intact.
        auto before = _gw._inflight_bytes.fetch_add(q._bytes);
        if (before + q._bytes > _gw._config._max_inflight_bytes) {
            _gw._inflight_bytes.fetch_sub(q._bytes);
            q._bytes = 0;
            q._prebuilt =
                _session._writer.error("TRYAGAIN gateway is over its in-flight budget, retry");
            ++_gw._stats._shed;
        } else {
            q._cmd = std::move(cmd);
        }
        _queue.push_back(std::move(q));
        _parse_held = hold;
    }

    /// Bytes queued for the socket past which commands stop executing.
    /// Caller holds `_mutex`.
    [[nodiscard]] auto output_full_locked() const -> bool {
        auto limit = _authenticated.load() ? _gw._config._max_output_buffer_bytes
                                           : _gw._config._pre_auth_output_buffer_bytes;
        return _out.size() + _writing_bytes >= limit;
    }

    /// Hand the queue to a worker unless one already has it.
    auto kick() -> void {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_busy || _queue.empty()) {
                return;
            }
            _busy = true;
        }
        auto self = this->shared_from_this();
        boost::asio::post(*_gw._workers, [self] { self->process(); });
    }

    /// Worker-pool job: execute queued commands in order until the queue is
    /// empty (or the client has stopped reading its replies), then hand the
    /// connection back to the I/O side.
    auto process() -> void {
        while (true) {
            queued_command q;
            bool stop = false;
            bool resume_read = false;
            bool resume_parse = false;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                bool closing = _session._closing || _closed;
                bool blocked = !closing && !_queue.empty() && output_full_locked();
                if (_queue.empty() || closing || blocked) {
                    // Output backpressure: leave the queue as it is; the
                    // write completion kicks us once the client catches up.
                    // Until then a full queue keeps reads paused too.
                    _busy = false;
                    if (blocked && !_output_blocked) {
                        ++_gw._stats._output_stalls;
                    }
                    _output_blocked = blocked;
                    stop = true;
                    if (!closing && !blocked) {
                        resume_read = _paused;
                        _paused = false;
                        resume_parse = _parse_held;
                        _parse_held = false;
                    }
                } else {
                    q = std::move(_queue.front());
                    _queue.pop_front();
                    if (_paused && _queue.size() < _gw._config._max_inflight_per_connection) {
                        _paused = false;
                        resume_read = true;
                    }
                }
            }
            if (resume_read || resume_parse) {
                auto self = this->shared_from_this();
                boost::asio::post(_strand, [self, resume_parse] {
                    if (resume_parse && !self->drain_parser()) {
                        return;
                    }
                    self->kick();
                    self->do_read();
                });
            }
            if (stop) {
                return;
            }
            if (q._bytes != 0) {
                _gw._inflight_bytes.fetch_sub(q._bytes);
            }
            std::string reply;
            if (!q._prebuilt.empty()) {
                reply = std::move(q._prebuilt);
            } else {
                try {
                    reply = _gw.execute(_session, q._cmd);
                } catch (const std::exception& e) {
                    _gw._logger.log(log_level::error, "redis gateway: command failed",
                                    {{"error", e.what()}});
                    reply = _session._writer.error("ERR internal error");
                }
            }
            // AUTH, HELLO ... AUTH and RESET change this; the strand reads it
            // to pick parser limits only once this command has finished.
            _authenticated.store(_session._identity.has_value());
            if (!reply.empty()) {
                queue_reply(reply);
            }
            if (_session._closing) {
                _close_after_flush = true;
            }
            flush();
        }
    }

    auto queue_reply(std::string bytes) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _out += bytes;
    }

    auto flush() -> void {
        auto self = this->shared_from_this();
        boost::asio::post(_strand, [self] { self->do_write(); });
    }

    auto do_write() -> void {
        if (_closed || _writing) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _writing_buffer.swap(_out);
            _out.clear();
            _writing_bytes = _writing_buffer.size();
        }
        if (_writing_buffer.empty()) {
            if (_close_after_flush) {
                do_close();
            }
            return;
        }
        _writing = true;
        auto self = this->shared_from_this();
        auto handler = [self](const boost::system::error_code& ec, std::size_t n) {
            self->_writing = false;
            self->_gw._stats._bytes_out += n;
            self->_writing_buffer.clear();
            bool unblocked = false;
            {
                std::lock_guard<std::mutex> lock(self->_mutex);
                self->_writing_bytes = 0;
                if (self->_output_blocked && !self->output_full_locked()) {
                    self->_output_blocked = false;
                    unblocked = true;
                }
            }
            if (ec) {
                self->do_close();
                return;
            }
            if (unblocked) {
                self->kick();
            }
            self->do_write();
        };
        if (_ssl) {
            boost::asio::async_write(*_ssl, boost::asio::buffer(_writing_buffer),
                                     boost::asio::bind_executor(_strand, handler));
        } else {
            boost::asio::async_write(*_plain, boost::asio::buffer(_writing_buffer),
                                     boost::asio::bind_executor(_strand, handler));
        }
    }

    auto do_close() -> void {
        if (_closed) {
            return;
        }
        _closed = true;
        boost::system::error_code ec;
        _idle_timer.cancel();
        if (_ssl) {
            (void)_ssl->next_layer().shutdown(tcp::socket::shutdown_both, ec);
            (void)_ssl->next_layer().close(ec);
        } else {
            (void)_plain->shutdown(tcp::socket::shutdown_both, ec);
            (void)_plain->close(ec);
        }
        std::size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            for (const auto& q : _queue) {
                dropped += q._bytes;
            }
            _queue.clear();
        }
        if (dropped != 0) {
            _gw._inflight_bytes.fetch_sub(dropped);
        }
        --_gw._stats._connections_current;
        _gw.unregister_connection(this);
    }

    redis_gateway& _gw;
    boost::asio::strand<boost::asio::io_context::executor_type> _strand;
    std::optional<tcp::socket> _plain;
    std::optional<ssl_stream> _ssl;
    std::shared_ptr<boost::asio::ssl::context> _ssl_ctx;
    boost::asio::steady_timer _idle_timer;
    std::array<char, 64 * 1024> _read_buffer{};
    session _session;

    std::mutex _mutex;
    std::deque<queued_command> _queue;
    std::string _out;
    std::string _writing_buffer;
    /// Size of `_writing_buffer` while a write is in flight, readable under
    /// `_mutex` (the buffer itself belongs to the strand).
    std::size_t _writing_bytes = 0;
    bool _busy = false;
    bool _paused = false;
    /// The worker stopped because `_out` is over its cap.
    bool _output_blocked = false;
    /// Pre-auth: a parsed command is queued or executing; parse and read
    /// nothing more until it has run.
    bool _parse_held = false;
    /// Mirrors `_session._identity.has_value()` for the strand, which must
    /// not read the session while a worker executes against it.
    std::atomic<bool> _authenticated{false};
    bool _writing = false;
    bool _read_pending = false;
    bool _close_after_flush = false;
    std::atomic<bool> _closed{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// lifecycle
// ─────────────────────────────────────────────────────────────────────────────

template<typename Host, typename Logger, typename Metrics>
redis_gateway<Host, Logger, Metrics>::redis_gateway(Host& host, redis_acl& acl, Logger& logger,
                                                    Metrics& metrics, redis_gateway_config config,
                                                    endpoint_resolver resolver)
    : _host(host),
      _acl(acl),
      _logger(logger),
      _metrics(metrics),
      _config(std::move(config)),
      _resolver(std::move(resolver)),
      _auth_limiter(_config._auth_failure_limit, _config._auth_failure_window,
                    _config._max_concurrent_auth != 0
                        ? _config._max_concurrent_auth
                        : std::max<std::size_t>(1, _config._worker_threads / 2)) {}

template<typename Host, typename Logger, typename Metrics>
redis_gateway<Host, Logger, Metrics>::~redis_gateway() {
    stop();
}

namespace redis_gateway_detail {

inline auto parse_listen(const std::string& spec) -> std::pair<std::string, std::uint16_t> {
    auto colon = spec.rfind(':');
    if (colon == std::string::npos) {
        throw std::invalid_argument("listen address must be host:port, got '" + spec + "'");
    }
    auto host = spec.substr(0, colon);
    auto port_text = spec.substr(colon + 1);
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    int port = std::stoi(port_text);
    if (port < 0 || port > 65535) {
        throw std::invalid_argument("listen port out of range: " + port_text);
    }
    return {host, static_cast<std::uint16_t>(port)};
}

/// The KV state machine itself, whether or not the host wraps it in
/// `transactional_state_machine`. Intents are invisible to every plain read
/// (multi-raft-transactions Requirement 5.1), so plain reads look here.
template<typename SM> auto kv_of(SM& sm) -> auto& {
    if constexpr (is_transactional_state_machine_v<std::remove_const_t<SM>>) {
        return sm.inner();
    } else {
        return sm;
    }
}

inline auto to_bytes(std::string_view s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    if (!s.empty()) {
        std::memcpy(out.data(), s.data(), s.size());
    }
    return out;
}

inline auto to_string(const std::vector<std::byte>& b) -> std::string {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

/// One reply from a peer gateway, as far as `KYTHIRA.TXN` needs: a bulk
/// string (the payload), a null, or an error (its text, without the `-`).
struct peer_reply {
    enum class kind : std::uint8_t {
        bulk,
        null,
        error,
        other
    } _kind{kind::other};
    std::string _body;
};

inline auto parse_peer_reply(const std::string& reply) -> peer_reply {
    if (reply.empty()) {
        return {};
    }
    const auto crlf = reply.find("\r\n");
    if (crlf == std::string::npos) {
        return {};
    }
    if (reply[0] == '-') {
        return {peer_reply::kind::error, reply.substr(1, crlf - 1)};
    }
    if (reply[0] == '_' || reply.rfind("$-1", 0) == 0) {
        return {peer_reply::kind::null, {}};
    }
    if (reply[0] == '$') {
        const auto len = std::stoull(reply.substr(1, crlf - 1));
        if (reply.size() < crlf + 2 + len) {
            return {};
        }
        return {peer_reply::kind::bulk, reply.substr(crlf + 2, len)};
    }
    return {};
}

/// A peer gateway refused a transaction op because its shard no longer owns
/// every key: the coordinator should re-partition and resend.
class txn_moved_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline auto put_u32(std::string& out, std::uint32_t v) -> void {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xFFU));
    }
}

inline auto put_u64(std::string& out, std::uint64_t v) -> void {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xFFU));
    }
}

/// Reads the encoding `encode_txn_read` writes; throws on a short buffer.
class txn_read_cursor {
public:
    explicit txn_read_cursor(std::string_view in) : _in(in) {}
    auto u8() -> std::uint8_t { return static_cast<std::uint8_t>(take(1)[0]); }
    auto u32() -> std::uint32_t { return static_cast<std::uint32_t>(be(take(4))); }
    auto u64() -> std::uint64_t { return be(take(8)); }
    auto str() -> std::string { return std::string(take(u32())); }
    [[nodiscard]] auto empty() const -> bool { return _in.empty(); }

private:
    auto take(std::size_t n) -> std::string_view {
        if (_in.size() < n) {
            throw std::runtime_error("truncated KYTHIRA.TXN READ reply");
        }
        auto out = _in.substr(0, n);
        _in.remove_prefix(n);
        return out;
    }
    static auto be(std::string_view b) -> std::uint64_t {
        std::uint64_t v = 0;
        for (char c : b) {
            v = (v << 8) | static_cast<unsigned char>(c);
        }
        return v;
    }
    std::string_view _in;
};

/// The `KYTHIRA.TXN TS` reply body: the range's first timestamp and count.
inline auto encode_timestamp_range(const timestamp_range& r) -> std::string {
    std::string out;
    put_u64(out, r._first);
    put_u64(out, r._count);
    return out;
}

inline auto decode_timestamp_range(std::string_view in) -> timestamp_range {
    txn_read_cursor c(in);
    timestamp_range r;
    r._first = c.u64();
    r._count = c.u64();
    return r;
}

/// A peer's `KYTHIRA.TXN READAT` refusal below its shard's safe point:
/// `KYTHIRA.TOOOLD <start_ts> <safe_point>`.
inline constexpr std::string_view k_too_old_prefix = "KYTHIRA.TOOOLD ";

inline auto parse_too_old(std::string_view body) -> std::optional<snapshot_too_old_exception> {
    if (body.rfind(k_too_old_prefix, 0) != 0) {
        return std::nullopt;
    }
    body.remove_prefix(k_too_old_prefix.size());
    std::uint64_t start = 0;
    std::uint64_t safe = 0;
    const auto space = body.find(' ');
    if (space == std::string_view::npos) {
        return std::nullopt;
    }
    auto r1 = std::from_chars(body.data(), body.data() + space, start);
    auto r2 = std::from_chars(body.data() + space + 1, body.data() + body.size(), safe);
    if (r1.ec != std::errc{} || r2.ec != std::errc{}) {
        return std::nullopt;
    }
    return snapshot_too_old_exception(start, safe);
}

/// The `KYTHIRA.TXN READ` reply body: the committed value and any intent.
inline auto encode_txn_read(const txn_read_result<std::string>& r) -> std::string {
    std::string out;
    out.push_back(static_cast<char>(r._value.has_value() ? 1 : 0));
    if (r._value) {
        put_u32(out, static_cast<std::uint32_t>(r._value->size()));
        out += to_string(*r._value);
    }
    out.push_back(static_cast<char>(r._intent.has_value() ? 1 : 0));
    if (r._intent) {
        put_u64(out, r._intent->_txn._hi);
        put_u64(out, r._intent->_txn._lo);
        put_u32(out, static_cast<std::uint32_t>(r._intent->_primary.size()));
        out += r._intent->_primary;
        put_u64(out, r._intent->_deadline_ms);
        // Tier 2: the holder's start timestamp, after the tier-1 fields so a
        // tier-1 decoder, which ignores what follows, still reads the rest.
        put_u64(out, r._intent->_start_ts);
    }
    return out;
}

inline auto decode_txn_read(std::string_view in) -> txn_read_result<std::string> {
    txn_read_cursor c(in);
    txn_read_result<std::string> r;
    if (c.u8() != 0) {
        r._value = to_bytes(c.str());
    }
    if (c.u8() != 0) {
        intent_view<std::string> v;
        v._txn._hi = c.u64();
        v._txn._lo = c.u64();
        v._primary = c.str();
        v._deadline_ms = c.u64();
        if (!c.empty()) {
            v._start_ts = c.u64();
        }
        r._intent = std::move(v);
    }
    return r;
}

}  // namespace redis_gateway_detail

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::start() -> void {
    if (_running.exchange(true)) {
        return;
    }
    if (_config._transactions) {
        // Requirement 11.1: refused unless the build wrapped the KV state
        // machine; Requirement 10.2: a level the build cannot give is an
        // error at startup, never a silent downgrade.
        if constexpr (!k_transactional) {
            _running = false;
            throw std::runtime_error(
                "redis gateway: --transactions on needs a build with the transaction adapter "
                "(CONFIG_REDIS_GATEWAY_TRANSACTIONS=y)");
        } else {
            const auto offered = coordinator_type::offered_isolations();
            if (std::find(offered.begin(), offered.end(), _config._txn_isolation) ==
                offered.end()) {
                _running = false;
                throw isolation_unsupported_exception(_config._txn_isolation, offered);
            }
            // The host's resolver reaches a primary this host does not lead
            // through the same forward link the coordinator uses.
            _host.set_txn_forwarder([this](const std::vector<std::string>& keys,
                                           const std::vector<std::byte>& envelope,
                                           std::chrono::milliseconds timeout) {
                return txn_send(keys, envelope, timeout);
            });
            if constexpr (k_snapshot) {
                // Under mvcc::on every plain write needs a commit timestamp;
                // a host that does not lead the oracle asks its leader's
                // gateway for one.
                _host.set_timestamp_forwarder(
                    [this](std::size_t n, std::chrono::milliseconds timeout) {
                        return txn_allocate_timestamps(n, timeout);
                    });
            }
        }
    }
    if (_acl.empty()) {
        if (!_config._allow_anonymous) {
            _running = false;
            throw std::runtime_error(
                "redis gateway: the ACL has no users and --allow-anonymous is not set; refusing "
                "to start an open cache");
        }
        _logger.log(log_level::warning,
                    "redis gateway: ACL is empty and --allow-anonymous is set; every connection is "
                    "admin with "
                    "access to every key");
    }
    if (_config._forwarding && _config._internal_secret.empty()) {
        _logger.log(log_level::warning,
                    "redis gateway: forwarding is on but no internal secret is configured; "
                    "commands for a shard "
                    "led elsewhere will be answered with a retry error instead of forwarded");
    } else if (_config._forwarding && !_config._forward_tls &&
               !_config._allow_plaintext_forwarding) {
        // Requirement 12.5: the internal identity is a read/write-everything
        // credential; it never crosses the network in the clear by default.
        _logger.log(log_level::warning,
                    "redis gateway: forwarding is on but neither TLS forwarding nor plaintext "
                    "forwarding is enabled; refusing to send the internal secret unencrypted, so "
                    "commands for a shard led elsewhere will be answered with a retry error");
    } else if (_config._forwarding && !_config._forward_tls) {
        _logger.log(log_level::warning,
                    "redis gateway: PLAINTEXT forwarding; the internal secret and forwarded "
                    "commands cross the network unencrypted");
    }
    _started_at = std::chrono::steady_clock::now();
    _io.restart();
    _work.emplace(boost::asio::make_work_guard(_io));
    _workers.emplace(std::max<std::size_t>(1, _config._worker_threads));

    auto bind = [&](const std::string& spec) {
        auto [host, port] = redis_gateway_detail::parse_listen(spec);
        return kythira::net_bind::open_asio_acceptors(_io, host.empty() ? "0.0.0.0" : host, port,
                                                      "redis gateway");
    };

    try {
        if (_config._forwarding && !_config._internal_secret.empty() && _config._forward_tls) {
            _forward_ssl_ctx =
                std::make_shared<boost::asio::ssl::context>(build_forward_ssl_context());
            _logger.log(
                log_level::info, "redis gateway: forwarding over TLS",
                {{"ca", _config._forward_tls_ca_path.empty() ? _config._tls_ca_path
                                                             : _config._forward_tls_ca_path}});
        }
        if (!_config._listen.empty()) {
            _acceptors = bind(_config._listen);
            _port = _acceptors.front()->local_endpoint().port();
            // Requirement 12.6: a plaintext listener is announced once, loudly.
            _logger.log(log_level::warning,
                        "redis gateway: PLAINTEXT listener; AUTH secrets and values cross the "
                        "network unencrypted",
                        {{"listen", _config._listen}, {"port", std::to_string(_port)}});
        }
        if (!_config._tls_listen.empty()) {
            _ssl_ctx = std::make_shared<boost::asio::ssl::context>(build_ssl_context());
            _tls_acceptors = bind(_config._tls_listen);
            _tls_port = _tls_acceptors.front()->local_endpoint().port();
            _logger.log(log_level::info, "redis gateway: TLS listener",
                        {{"listen", _config._tls_listen},
                         {"port", std::to_string(_tls_port)},
                         {"client_cert", _config._require_client_cert ? "required" : "optional"}});
        }
    } catch (...) {
        _work.reset();
        _workers->join();
        _workers.reset();
        _acceptors.clear();
        _tls_acceptors.clear();
        _forward_ssl_ctx.reset();
        _running = false;
        throw;
    }

    for (const auto& acceptor : _acceptors) {
        accept_loop(acceptor, false);
    }
    for (const auto& acceptor : _tls_acceptors) {
        accept_loop(acceptor, true);
    }
    for (std::size_t i = 0; i < std::max<std::size_t>(1, _config._io_threads); ++i) {
        _io_threads.emplace_back([this] { _io.run(); });
    }
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::stop() -> void {
    if (!_running.exchange(false)) {
        return;
    }
    if constexpr (k_transactional) {
        if (_config._transactions) {
            _host.set_txn_forwarder({});
        }
    }
    boost::system::error_code ec;
    for (const auto& acceptor : _acceptors) {
        (void)acceptor->close(ec);
    }
    for (const auto& acceptor : _tls_acceptors) {
        (void)acceptor->close(ec);
    }
    std::vector<std::shared_ptr<connection>> live;
    {
        std::lock_guard<std::mutex> lock(_connections_mutex);
        for (auto& [ptr, weak] : _connections) {
            if (auto c = weak.lock()) {
                live.push_back(std::move(c));
            }
        }
    }
    for (auto& c : live) {
        c->close();
    }
    live.clear();
    // Workers first: a job mid-command holds its connection alive and may
    // post to the strand; once every job has returned the I/O side can wind
    // down knowing nothing will post to it again.
    if (_workers) {
        _workers->join();
        _workers.reset();
    }
    _work.reset();
    _io.stop();
    for (auto& t : _io_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    _io_threads.clear();
    _acceptors.clear();
    _tls_acceptors.clear();
    {
        std::lock_guard<std::mutex> lock(_connections_mutex);
        _connections.clear();
    }
    {
        std::lock_guard<std::mutex> lock(_forward_mutex);
        _forward_pools.clear();
    }
    _forward_ssl_ctx.reset();
    _port = 0;
    _tls_port = 0;
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::build_ssl_context() -> boost::asio::ssl::context {
    namespace ssl = boost::asio::ssl;
    ssl::context ctx(ssl::context::tls_server);
    // TLS 1.2 floor, 1.3 preferred (Requirement 12.1).
    ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                    ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1 |
                    ssl::context::single_dh_use);
    SSL_CTX_set_min_proto_version(ctx.native_handle(), TLS1_2_VERSION);
    if (_config._tls_cert_path.empty() || _config._tls_key_path.empty()) {
        throw std::runtime_error("redis gateway: TLS listener needs both a certificate and a key");
    }
    ctx.use_certificate_chain_file(_config._tls_cert_path);
    ctx.use_private_key_file(_config._tls_key_path, ssl::context::pem);
    if (!_config._tls_ca_path.empty()) {
        ctx.load_verify_file(_config._tls_ca_path);
    }
    if (_config._require_client_cert) {
        ctx.set_verify_mode(ssl::verify_peer | ssl::verify_fail_if_no_peer_cert);
    } else if (!_config._tls_ca_path.empty()) {
        ctx.set_verify_mode(ssl::verify_peer);
    } else {
        ctx.set_verify_mode(ssl::verify_none);
    }
    return ctx;
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::build_forward_ssl_context()
    -> boost::asio::ssl::context {
    namespace ssl = boost::asio::ssl;
    ssl::context ctx(ssl::context::tls_client);
    // The same TLS 1.2 floor as the listener (Requirement 12.2).
    ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                    ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
    SSL_CTX_set_min_proto_version(ctx.native_handle(), TLS1_2_VERSION);
    const auto& ca =
        _config._forward_tls_ca_path.empty() ? _config._tls_ca_path : _config._forward_tls_ca_path;
    if (ca.empty()) {
        // Without a CA the only options are trusting every certificate or
        // none; the first hands the secret to whoever answers.
        throw std::runtime_error(
            "redis gateway: forwarding over TLS needs a CA to verify peer gateways against");
    }
    ctx.load_verify_file(ca);
    ctx.set_verify_mode(ssl::verify_peer);
    const bool own_cert =
        !_config._forward_tls_cert_path.empty() || !_config._forward_tls_key_path.empty();
    const auto& cert = own_cert ? _config._forward_tls_cert_path : _config._tls_cert_path;
    const auto& key = own_cert ? _config._forward_tls_key_path : _config._tls_key_path;
    if (cert.empty() != key.empty()) {
        throw std::runtime_error(
            "redis gateway: the forwarding client certificate needs both a certificate and a key");
    }
    if (!cert.empty()) {
        ctx.use_certificate_chain_file(cert);
        ctx.use_private_key_file(key, ssl::context::pem);
    }
    return ctx;
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::accept_loop(
    std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor, bool tls) -> void {
    auto& a = *acceptor;
    a.async_accept([this, acceptor = std::move(acceptor), tls](
                       const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
        if (ec) {
            if (ec != boost::asio::error::operation_aborted && _running.load()) {
                _logger.log(log_level::warning, "redis gateway: accept failed",
                            {{"error", ec.message()}});
                accept_loop(acceptor, tls);
            }
            return;
        }
        if (_stats._connections_current.load() >= _config._max_clients) {
            // Requirement 13.1: say why before closing.
            ++_stats._connections_rejected;
            boost::system::error_code ignored;
            (void)socket.non_blocking(true, ignored);
            static constexpr std::string_view reply = "-ERR max number of clients reached\r\n";
            socket.write_some(boost::asio::buffer(reply.data(), reply.size()), ignored);
            (void)socket.close(ignored);
            emit("redis.connections.rejected", "accept");
        } else {
            ++_stats._connections_accepted;
            ++_stats._connections_current;
            auto conn =
                std::make_shared<connection>(*this, std::move(socket), tls ? _ssl_ctx : nullptr);
            register_connection(conn);
            emit("redis.connections.accepted", "accept");
            conn->start();
        }
        if (_running.load()) {
            accept_loop(acceptor, tls);
        }
    });
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::register_connection(const std::shared_ptr<connection>& c)
    -> void {
    std::lock_guard<std::mutex> lock(_connections_mutex);
    _connections.emplace(c.get(), c);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::unregister_connection(connection* c) -> void {
    std::lock_guard<std::mutex> lock(_connections_mutex);
    _connections.erase(c);
}

// ─────────────────────────────────────────────────────────────────────────────
// helpers
// ─────────────────────────────────────────────────────────────────────────────

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::now_ms() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::is_expired(const entry_type& e) const -> bool {
    return e._expire_at_ms != 0 && e._expire_at_ms <= now_ms();
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::emit(std::string_view name, std::string_view command,
                                                std::int64_t count) -> void {
    std::lock_guard<std::mutex> lock(_metrics_mutex);
    _metrics.set_metric_name(name);
    _metrics.add_dimension("command", command);
    _metrics.add_count(count);
    _metrics.emit();
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::emit_duration(std::string_view name,
                                                         std::string_view command,
                                                         std::chrono::nanoseconds d) -> void {
    std::lock_guard<std::mutex> lock(_metrics_mutex);
    _metrics.set_metric_name(name);
    _metrics.add_dimension("command", command);
    _metrics.add_duration(d);
    _metrics.emit();
}

/// Audit stream: who, from where, what, and why — never a secret or a value.
/// Tagged `stream=audit` so a log sink can route it separately (Requirement 11.5).
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::audit(session& s, std::string_view command,
                                                 std::string_view outcome) -> void {
    _logger.log(log_level::info, "redis audit",
                {{"stream", "audit"},
                 {"identity", s._identity ? std::string_view(s._identity->_user) : "unknown"},
                 {"source", s._source},
                 {"command", command},
                 {"reason", outcome}});
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::authorize(session& s, std::string_view upper,
                                                     const std::vector<std::string_view>& keys)
    -> std::optional<std::string> {
    if (!s._identity) {
        return s._writer.error("NOAUTH Authentication required.");
    }
    switch (redis_acl::authorize(*s._identity, upper, keys)) {
        case acl_decision::allow:
            return std::nullopt;
        case acl_decision::deny_command:
            ++_stats._authz_denials;
            emit("redis.authz.denied", upper);
            audit(s, upper, "command denied");
            return s._writer.error("NOPERM User " + s._identity->_user +
                                   " has no permissions to run the '" + std::string(upper) +
                                   "' command");
        case acl_decision::deny_key:
            ++_stats._authz_denials;
            emit("redis.authz.denied", upper);
            audit(s, upper, "key out of scope");
            return s._writer.error("NOPERM No permissions to access a key");
    }
    return s._writer.error("NOPERM denied");
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::touch_lru(group_id_type group, const std::string& key)
    -> void {
    if (auto* node = _host.group_node(group); node != nullptr && !node->is_leader()) {
        // A follower keeps no LRU of its own; the read is queued for the
        // leader's (Requirement 6.2), and only when something will send it.
        if (!_cache_hooks._advisory) {
            return;
        }
        std::lock_guard<std::mutex> lock(_lru_mutex);
        auto& queued = _remote_touches[group];
        if (queued.size() >= max_queued_touches && !queued.contains(key)) {
            ++_stats._touches_dropped;
            return;
        }
        queued.insert(key);
        return;
    }
    std::lock_guard<std::mutex> lock(_lru_mutex);
    auto& lru = _lru[group];
    auto it = lru._index.find(key);
    if (it != lru._index.end()) {
        lru._order.splice(lru._order.begin(), lru._order, it->second);
        return;
    }
    lru._order.push_front(key);
    lru._index.emplace(key, lru._order.begin());
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::note_access(const std::string& key) -> void {
    if (auto desc = _host.resolve(key)) {
        touch_lru(desc->group_id(), key);
    }
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::note_removed(const std::string& key) -> void {
    if (auto desc = _host.resolve(key)) {
        forget_lru(desc->group_id(), key);
    }
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::forget_lru(group_id_type group, const std::string& key)
    -> void {
    std::lock_guard<std::mutex> lock(_lru_mutex);
    auto g = _lru.find(group);
    if (g == _lru.end()) {
        return;
    }
    auto it = g->second._index.find(key);
    if (it != g->second._index.end()) {
        g->second._order.erase(it->second);
        g->second._index.erase(it);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// routing
// ─────────────────────────────────────────────────────────────────────────────

/// Find the local replica for `key`. On failure fills `error_reply` (an
/// encoded error) and, when a leader elsewhere is known, `forward_to`.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::locate(const std::string& key, std::string& error_reply,
                                                  std::optional<node_id_type>& forward_to)
    -> group_node_type* {
    resp_writer w;
    if (!_host.is_running()) {
        error_reply = w.error("LOADING Kythira is loading the dataset in memory");
        return nullptr;
    }
    auto desc = _host.resolve(key);
    if (!desc) {
        error_reply = w.error("ERR no shard owns this key");
        return nullptr;
    }
    auto* node = _host.group_node(desc->group_id());
    if (node == nullptr) {
        // The routing row exists but no replica is here: any voter can say
        // who leads, so send the command to one of them.
        for (const auto& voter : desc->voters()) {
            if (voter != _host.node_id()) {
                forward_to = voter;
                break;
            }
        }
        if (desc->leader_hint() && *desc->leader_hint() != _host.node_id()) {
            forward_to = desc->leader_hint();
        }
        error_reply = w.error("ERR shard has no reachable leader, retry");
        return nullptr;
    }
    return node;
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::submit(session& s, const std::string& key,
                                                  const std::vector<std::byte>& command,
                                                  std::string& error_reply,
                                                  std::optional<node_id_type>& forward_to) -> bool {
    // A key a transaction holds refuses plain writes. The gateway finishes
    // that transaction if its time is up, then tries once more
    // (multi-raft-transactions design "Error Handling").
    std::exception_ptr failure;
    for (int attempt = 0; !failure; ++attempt) {
        try {
            auto future = _host.submit_command(key, command, _config._command_timeout);
            if (!future.wait(_config._command_timeout)) {
                error_reply = s._writer.error("ERR timeout submitting to shard, retry");
                return false;
            }
            (void)std::move(future).get();
            return true;
        } catch (const key_locked_exception<std::string>& e) {
            if (attempt == 0 && resolve_lock(e.key(), e.holder(), e.primary())) {
                continue;
            }
            error_reply = s._writer.error("ERR key is locked by transaction " +
                                          e.holder().to_string() + ", retry");
            return false;
        } catch (...) {
            failure = std::current_exception();
        }
    }
    try {
        std::rethrow_exception(failure);
    } catch (const shard_not_leader_exception<group_id_type, node_id_type>& e) {
        forward_to = e.leader_hint();
        error_reply = s._writer.error("ERR shard has no reachable leader, retry");
        return false;
    } catch (const unrouted_key_exception<std::string>&) {
        error_reply = s._writer.error("ERR no shard owns this key");
        return false;
    } catch (const std::exception& e) {
        _logger.log(log_level::debug, "redis gateway: submit failed", {{"error", e.what()}});
        error_reply = s._writer.error("ERR shard has no reachable leader, retry");
        return false;
    }
}

/// Local read under the configured consistency. Never `node::read_state()`:
/// that serializes the entire shard to answer one key, which for a cache
/// holding gigabytes of compiler output would be the slowest GET ever built.
/// `with_state_machine` runs under the node mutex, so the lambda takes only
/// the shared handle and everything else — expiry check, socket write —
/// happens after the lock is released.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::read_local(session& s, const std::string& key)
    -> read_result {
    read_result r;
    auto* node = locate(key, r._error, r._forward_to);
    if (node == nullptr) {
        return r;
    }
    if (_config._read_consistency != redis_read_consistency::any_replica && !node->is_leader()) {
        r._forward_to = node->known_leader();
        r._error = s._writer.error("ERR shard has no reachable leader, retry");
        return r;
    }
    if (_config._read_consistency == redis_read_consistency::linearizable) {
        // A committed no-op proves this leader's applied state includes every
        // write that preceded the read. A sweep naming the key with deadline
        // 0 is that no-op: apply() only deletes on an exact deadline match
        // and 0 never matches, so nothing changes, and the key routes it.
        auto sweep = encode_redis_kv_command(redis_kv_sweep_command{{{key, 0}}});
        if (!submit(s, key, sweep, r._error, r._forward_to)) {
            return r;
        }
    }
    auto lookup = [&key](auto& sm) {
        return std::pair{redis_gateway_detail::kv_of(sm).lookup(key), value_access::context_of(sm)};
    };
    if (_config._read_consistency == redis_read_consistency::any_replica) {
        std::tie(r._entry, r._context) = node->with_state_machine(lookup);
    } else {
        // A leader elected a moment ago may not have applied every write its
        // predecessor acknowledged yet; answering from that state would
        // report an acknowledged SET as missing. Retry until it has.
        auto entry = node->with_leader_state_machine(lookup);
        if (!entry) {
            if (!node->is_leader()) {
                r._forward_to = node->known_leader();  // stepped down since the check above
            }
            r._error = s._writer.error("ERR shard has no reachable leader, retry");
            return r;
        }
        std::tie(r._entry, r._context) = std::move(*entry);
    }
    // A disk-backed entry whose chunks are not all here yet (a follower, or a
    // leader, still fetching) is unservable on this node. A follower hands
    // the read to the leader; a leader answers it as a miss rather than wait.
    // Every read path goes through here, so EXISTS, STRLEN and TTL agree
    // with what GET would serve.
    if (r._entry && !value_access::servable(*r._entry)) {
        if (!node->is_leader()) {
            if (auto leader = node->known_leader()) {
                r._forward_to = leader;
                r._error = s._writer.error("ERR value not yet replicated here, retry");
                return r;
            }
        }
        r._entry = nullptr;
    }
    return r;
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::forward_endpoint(const node_id_type& to)
    -> std::optional<std::string> {
    if (!_resolver) {
        return std::nullopt;
    }
    return _resolver(to);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::forward_socket(
    const std::string& endpoint, std::chrono::steady_clock::time_point deadline)
    -> std::unique_ptr<redis_forward_link> {
    std::shared_ptr<forward_pool> pool;
    {
        std::lock_guard<std::mutex> lock(_forward_mutex);
        auto& slot = _forward_pools[endpoint];
        if (!slot) {
            slot = std::make_shared<forward_pool>();
        }
        pool = slot;
    }
    {
        std::lock_guard<std::mutex> lock(pool->_mutex);
        if (!pool->_idle.empty()) {
            auto sock = std::move(pool->_idle.back());
            pool->_idle.pop_back();
            sock->set_deadline(deadline);
            return sock;
        }
    }
    auto [host, port] = redis_gateway_detail::parse_listen(endpoint);
    // TLS whenever it is configured; forward() never gets here without TLS
    // unless plaintext forwarding was explicitly allowed (Requirement 12.5).
    auto sock = std::make_unique<redis_forward_link>(_forward_ssl_ctx);
    sock->set_deadline(deadline);
    sock->connect(host, port);

    // Authenticate as the internal identity; the peer never forwards a
    // command that arrives on this connection (one hop, Requirement 9.4).
    resp_writer w;
    auto auth =
        w.array({w.bulk("AUTH"), w.bulk(_config._internal_user), w.bulk(_config._internal_secret)});
    sock->write(boost::asio::buffer(auth));
    std::string buf;
    std::array<char, 512> chunk{};
    while (resp_reply_length(buf) == 0) {
        if (buf.size() > 64u * 1024u) {
            throw std::runtime_error("oversized AUTH reply from peer gateway");
        }
        auto n = sock->read_some(boost::asio::buffer(chunk));
        buf.append(chunk.data(), n);
    }
    if (buf.rfind("+OK", 0) != 0) {
        throw std::runtime_error("internal AUTH rejected by peer gateway: " +
                                 buf.substr(0, buf.size() - 2));
    }
    return sock;
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::return_forward_socket(
    const std::string& endpoint, std::unique_ptr<redis_forward_link> sock) -> void {
    if (!sock->healthy()) {
        return;
    }
    std::lock_guard<std::mutex> lock(_forward_mutex);
    auto it = _forward_pools.find(endpoint);
    if (it == _forward_pools.end()) {
        return;
    }
    std::lock_guard<std::mutex> plock(it->second->_mutex);
    if (it->second->_idle.size() < 16) {
        it->second->_idle.push_back(std::move(sock));
    }
}

/// Relay `cmd` to the gateway on node `to` and return its reply, or nullopt
/// if forwarding is off, the session is itself internal, or the peer could
/// not be reached — the caller then answers with the retry error it already
/// has.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::forward(session& s, const resp_command& cmd,
                                                   const node_id_type& to)
    -> std::optional<std::string> {
    if (!_config._forwarding || s._internal) {
        return std::nullopt;
    }
    auto reply = forward_argv(cmd._argv, to, resp_to_upper(cmd._argv[0]));
    // The internal hop speaks RESP2; a RESP3 client expects `_` for null.
    if (reply && s._writer.version() >= 3 && *reply == "$-1\r\n") {
        *reply = "_\r\n";
    }
    return reply;
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::forward_argv(const std::vector<std::string>& argv,
                                                        const node_id_type& to,
                                                        std::string_view what)
    -> std::optional<std::string> {
    if (!_running.load() || _config._internal_secret.empty() ||
        (!_config._forward_tls && !_config._allow_plaintext_forwarding)) {
        return std::nullopt;
    }
    auto endpoint = forward_endpoint(to);
    if (!endpoint) {
        return std::nullopt;
    }
    ++_stats._forwards;
    emit("redis.forwards", what);
    try {
        // One budget for the whole exchange, connect and AUTH included, so a
        // peer that stalls surfaces as the retry error, never a hang
        // (Requirement 13.4). A zero timeout meant "none" for the old
        // socket options; keep that meaning with a day-long budget.
        const auto budget = _config._command_timeout.count() > 0
                                ? std::chrono::steady_clock::duration{_config._command_timeout}
                                : std::chrono::steady_clock::duration{std::chrono::hours{24}};
        auto sock = forward_socket(*endpoint, std::chrono::steady_clock::now() + budget);
        resp_writer w;
        std::vector<std::string> parts;
        parts.reserve(argv.size());
        for (const auto& a : argv) {
            parts.push_back(w.bulk(a));
        }
        auto wire = w.array(parts);
        sock->write(boost::asio::buffer(wire));
        std::string buf;
        std::array<char, 16 * 1024> chunk{};
        std::size_t len = 0;
        // A peer's reply is never larger than the largest request this
        // gateway itself accepts; past that, give up on the peer.
        resp_reply_limits limits;
        limits._max_bulk_len = _config._parser_limits._max_bulk_len;
        while ((len = resp_reply_length(buf, limits)) == 0) {
            if (buf.size() > _config._parser_limits._max_buffered_bytes) {
                throw std::runtime_error("oversized reply from peer gateway");
            }
            auto n = sock->read_some(boost::asio::buffer(chunk));
            buf.append(chunk.data(), n);
        }
        return_forward_socket(*endpoint, std::move(sock));
        buf.resize(len);
        return buf;
    } catch (const std::exception& e) {
        ++_stats._forward_failures;
        _logger.log(log_level::warning, "redis gateway: forward failed",
                    {{"endpoint", *endpoint}, {"error", e.what()}});
        return std::nullopt;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// dispatch
// ─────────────────────────────────────────────────────────────────────────────

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::execute_for_test(
    const resp_command& cmd, const std::optional<redis_identity>& identity, bool internal)
    -> std::string {
    session s;
    s._identity = identity;
    s._internal = internal;
    s._source = "test";
    s._rate_key = "test";
    return execute(s, cmd);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::execute(session& s, const resp_command& cmd)
    -> std::string {
    if (cmd._argv.empty()) {
        return {};
    }
    ++_stats._commands;
    const auto upper = resp_to_upper(cmd._argv[0]);
    const auto started = std::chrono::steady_clock::now();
    if (_config._log_commands) {
        // AUTH/HELLO arguments are secrets: never in a trace (Requirement 11.4).
        bool redact = upper == "AUTH" || upper == "HELLO";
        std::string args;
        for (std::size_t i = 1; i < cmd._argv.size() && i < 4; ++i) {
            args += redact ? " <redacted>" : " " + cmd._argv[i].substr(0, 64);
        }
        _logger.log(log_level::debug, "redis command",
                    {{"source", s._source}, {"command", upper}, {"args", args}});
    }
    auto arity = [&](std::size_t min, std::size_t max) -> std::optional<std::string> {
        if (cmd._argv.size() < min || cmd._argv.size() > max) {
            std::string lower = cmd._argv[0];
            for (auto& c : lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s._writer.error("ERR wrong number of arguments for '" + lower + "' command");
        }
        return std::nullopt;
    };
    auto finish = [&](std::string reply) {
        emit_duration("redis.command.latency", upper, std::chrono::steady_clock::now() - started);
        return reply;
    };

    // Pre-authentication surface: AUTH, HELLO, QUIT, RESET and nothing else.
    if (upper == "QUIT") {
        s._closing = true;
        return finish(s._writer.simple_string("OK"));
    }
    if (upper == "RESET") {
        s._in_multi = false;
        s._multi_failed = false;
        s._queued.clear();
        s._isolation.reset();
        s._identity.reset();
        s._internal = false;
        s._writer.set_version(2);
        s._client_name.clear();
        if (_config._allow_anonymous && _acl.empty()) {
            s._identity = redis_identity{"anonymous", redis_role::admin, {""}};
        }
        return finish(s._writer.simple_string("RESET"));
    }
    if (upper == "AUTH") {
        return finish(handle_auth(s, cmd));
    }
    if (upper == "HELLO") {
        return finish(handle_hello(s, cmd));
    }
    if (!s._identity) {
        return finish(s._writer.error("NOAUTH Authentication required."));
    }

    if (_config._transactions) {
        if (s._in_multi && upper != "EXEC" && upper != "DISCARD" && upper != "MULTI") {
            return finish(queue_in_multi(s, upper, cmd));
        }
        if (is_txn_command(upper, cmd)) {
            return finish(execute_txn_command(s, upper, cmd));
        }
    }

    if (upper == "PING") {
        if (auto e = arity(1, 2)) {
            return finish(*e);
        }
        if (auto e = authorize(s, upper, {})) {
            return finish(*e);
        }
        return finish(cmd._argv.size() == 2 ? s._writer.bulk(cmd._argv[1])
                                            : s._writer.simple_string("PONG"));
    }
    if (upper == "ECHO") {
        if (auto e = arity(2, 2)) {
            return finish(*e);
        }
        if (auto e = authorize(s, upper, {})) {
            return finish(*e);
        }
        return finish(s._writer.bulk(cmd._argv[1]));
    }
    if (upper == "SELECT") {
        if (auto e = arity(2, 2)) {
            return finish(*e);
        }
        if (auto e = authorize(s, upper, {})) {
            return finish(*e);
        }
        return finish(cmd._argv[1] == "0" ? s._writer.simple_string("OK")
                                          : s._writer.error("ERR DB index is out of range"));
    }
    if (upper == "CLIENT") {
        return finish(handle_client(s, cmd));
    }
    if (upper == "GET") {
        if (auto e = arity(2, 2)) {
            return finish(*e);
        }
        return finish(handle_get(s, cmd));
    }
    if (upper == "EXISTS") {
        if (auto e = arity(2, 2)) {
            return finish(*e);
        }
        return finish(handle_exists(s, cmd));
    }
    if (upper == "STRLEN") {
        if (auto e = arity(2, 2)) {
            return finish(*e);
        }
        return finish(handle_strlen(s, cmd));
    }
    if (upper == "GETRANGE") {
        if (auto e = arity(4, 4)) {
            return finish(*e);
        }
        return finish(handle_getrange(s, cmd));
    }
    if (upper == "TTL") {
        if (auto e = arity(2, 2)) {
            return finish(*e);
        }
        return finish(handle_ttl(s, cmd));
    }
    if (upper == "SET") {
        if (auto e = arity(3, 3)) {
            return finish(*e);
        }
        return finish(handle_set(s, cmd, false));
    }
    if (upper == "SETEX") {
        if (auto e = arity(4, 4)) {
            return finish(*e);
        }
        return finish(handle_set(s, cmd, true));
    }
    if (upper == "DEL") {
        if (auto e = arity(2, 2)) {
            return finish(*e);
        }
        return finish(handle_del(s, cmd));
    }
    if (upper == "INFO") {
        if (auto e = authorize(s, upper, {})) {
            return finish(*e);
        }
        return finish(handle_info(s));
    }
    if (upper == "DBSIZE") {
        if (auto e = authorize(s, upper, {})) {
            return finish(*e);
        }
        return finish(handle_dbsize(s));
    }
    if (upper == "COMMAND") {
        return finish(handle_command(s, cmd));
    }
    if (upper == "KYTHIRA.ADVISE") {
        return finish(handle_advise(s, cmd));
    }

    // Requirement 1.8: unknown commands are an error, not a disconnect.
    std::string args;
    for (std::size_t i = 1; i < cmd._argv.size(); ++i) {
        args += "'" + cmd._argv[i].substr(0, 32) + "' ";
    }
    return finish(s._writer.error("ERR unknown command '" + cmd._argv[0] +
                                  "', with args beginning with: " + args));
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_auth(session& s, const resp_command& cmd)
    -> std::string {
    if (cmd._argv.size() != 2 && cmd._argv.size() != 3) {
        return s._writer.error("ERR wrong number of arguments for 'auth' command");
    }
    // `AUTH password` is the pre-ACL form; it authenticates the `default` user.
    std::string user = cmd._argv.size() == 3 ? cmd._argv[1] : "default";
    const auto& secret = cmd._argv.back();
    // Admission is decided, and the attempt charged, before the KDF runs.
    switch (_auth_limiter.begin(s._rate_key)) {
        case redis_auth_limiter::verdict::allowed:
            break;
        case redis_auth_limiter::verdict::rate_limited:
            ++_stats._auth_failures;
            emit("redis.auth.failures", "AUTH");
            audit(s, "AUTH", "rate limited");
            return s._writer.error(
                "ERR too many authentication failures from this address, retry later");
        case redis_auth_limiter::verdict::busy:
            ++_stats._auth_busy;
            emit("redis.auth.busy", "AUTH");
            audit(s, "AUTH", "too many concurrent attempts");
            return s._writer.error("ERR too many concurrent authentication attempts, retry later");
    }
    std::optional<redis_identity> id;
    try {
        id = _acl.authenticate(user, secret);
    } catch (...) {
        _auth_limiter.finish(s._rate_key, false);
        throw;
    }
    _auth_limiter.finish(s._rate_key, id.has_value());
    if (!id) {
        ++_stats._auth_failures;
        emit("redis.auth.failures", "AUTH");
        audit(s, "AUTH", "rejected user '" + user + "'");
        // Identical for unknown user and wrong password (Requirement 10.4).
        return s._writer.error("WRONGPASS invalid username-password pair or user is disabled.");
    }
    s._identity = std::move(id);
    s._internal = s._identity->_user == _config._internal_user;
    audit(s, "AUTH", "accepted");
    return s._writer.simple_string("OK");
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_hello(session& s, const resp_command& cmd)
    -> std::string {
    // HELLO [protover [AUTH username password] [SETNAME clientname]]
    int version = s._writer.version();
    std::size_t i = 1;
    if (cmd._argv.size() > 1) {
        const auto& v = cmd._argv[1];
        if (v == "2") {
            version = 2;
        } else if (v == "3") {
            version = 3;
        } else {
            return s._writer.error("NOPROTO unsupported protocol version");
        }
        i = 2;
    }
    std::optional<std::pair<std::string, std::string>> auth;
    std::optional<std::string> setname;
    while (i < cmd._argv.size()) {
        auto opt = resp_to_upper(cmd._argv[i]);
        if (opt == "AUTH" && i + 2 < cmd._argv.size()) {
            auth = std::make_pair(cmd._argv[i + 1], cmd._argv[i + 2]);
            i += 3;
        } else if (opt == "SETNAME" && i + 1 < cmd._argv.size()) {
            setname = cmd._argv[i + 1];
            i += 2;
        } else {
            return s._writer.error("ERR Syntax error in HELLO option '" + cmd._argv[i] + "'");
        }
    }
    if (auth) {
        resp_command a;
        a._argv = {"AUTH", auth->first, auth->second};
        auto reply = handle_auth(s, a);
        if (reply.rfind('-', 0) == 0) {
            return reply;
        }
    }
    if (!s._identity) {
        return s._writer.error("NOAUTH Authentication required.");
    }
    if (setname) {
        s._client_name = *setname;
    }
    s._writer.set_version(version);
    resp_writer& w = s._writer;
    return w.map({{"server", w.bulk("kythira")},
                  {"version", w.bulk("7.0.0-kythira")},
                  {"proto", w.integer(version)},
                  {"id", w.integer(0)},
                  {"mode", w.bulk("cluster")},
                  {"role", w.bulk("master")},
                  {"modules", w.array({})}});
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_client(session& s, const resp_command& cmd)
    -> std::string {
    if (auto e = authorize(s, "CLIENT", {})) {
        return *e;
    }
    if (cmd._argv.size() < 2) {
        return s._writer.error("ERR wrong number of arguments for 'client' command");
    }
    auto sub = resp_to_upper(cmd._argv[1]);
    if (sub == "SETINFO") {
        if (cmd._argv.size() != 4) {
            return s._writer.error("ERR wrong number of arguments for 'client|setinfo' command");
        }
        auto attr = resp_to_upper(cmd._argv[2]);
        if (attr == "LIB-NAME") {
            s._lib_name = cmd._argv[3];
        } else if (attr == "LIB-VER") {
            s._lib_version = cmd._argv[3];
        } else {
            return s._writer.error("ERR Unrecognized option '" + cmd._argv[2] + "'");
        }
        return s._writer.simple_string("OK");
    }
    if (sub == "SETNAME") {
        if (cmd._argv.size() != 3) {
            return s._writer.error("ERR wrong number of arguments for 'client|setname' command");
        }
        s._client_name = cmd._argv[2];
        return s._writer.simple_string("OK");
    }
    if (sub == "GETNAME") {
        return s._client_name.empty() ? s._writer.null() : s._writer.bulk(s._client_name);
    }
    if (sub == "ID") {
        return s._writer.integer(0);
    }
    return s._writer.error("ERR unknown subcommand '" + cmd._argv[1] + "'. Try CLIENT HELP.");
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_get(session& s, const resp_command& cmd)
    -> std::string {
    const auto& key = cmd._argv[1];
    if (auto e = authorize(s, "GET", {key})) {
        return *e;
    }
    auto r = read_local(s, key);
    if (!r._error.empty()) {
        if (r._forward_to) {
            if (auto reply = forward(s, cmd, *r._forward_to)) {
                return *reply;
            }
        }
        return r._error;
    }
    if (!r._entry || is_expired(*r._entry)) {
        ++_stats._misses;
        emit("redis.misses", "GET");
        return s._writer.null();
    }
    auto bytes = value_access::read(r._context, *r._entry);
    if (!bytes) {
        // A chunk vanished or failed its hash after the lookup.
        ++_stats._misses;
        emit("redis.misses", "GET");
        return s._writer.null();
    }
    ++_stats._hits;
    emit("redis.hits", "GET");
    if (auto desc = _host.resolve(key)) {
        touch_lru(desc->group_id(), key);
    }
    return s._writer.bulk(bytes->view());
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_exists(session& s, const resp_command& cmd)
    -> std::string {
    const auto& key = cmd._argv[1];
    if (auto e = authorize(s, "EXISTS", {key})) {
        return *e;
    }
    auto r = read_local(s, key);
    if (!r._error.empty()) {
        if (r._forward_to) {
            if (auto reply = forward(s, cmd, *r._forward_to)) {
                return *reply;
            }
        }
        return r._error;
    }
    bool present = r._entry && !is_expired(*r._entry);
    emit(present ? "redis.hits" : "redis.misses", "EXISTS");
    return s._writer.integer(present ? 1 : 0);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_strlen(session& s, const resp_command& cmd)
    -> std::string {
    const auto& key = cmd._argv[1];
    if (auto e = authorize(s, "STRLEN", {key})) {
        return *e;
    }
    auto r = read_local(s, key);
    if (!r._error.empty()) {
        if (r._forward_to) {
            if (auto reply = forward(s, cmd, *r._forward_to)) {
                return *reply;
            }
        }
        return r._error;
    }
    bool present = r._entry && !is_expired(*r._entry);
    return s._writer.integer(present ? static_cast<std::int64_t>(value_access::size(*r._entry))
                                     : 0);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_getrange(session& s, const resp_command& cmd)
    -> std::string {
    const auto& key = cmd._argv[1];
    if (auto e = authorize(s, "GETRANGE", {key})) {
        return *e;
    }
    std::int64_t start = 0;
    std::int64_t end = 0;
    try {
        std::size_t used = 0;
        start = std::stoll(cmd._argv[2], &used);
        if (used != cmd._argv[2].size()) {
            throw std::invalid_argument("start");
        }
        end = std::stoll(cmd._argv[3], &used);
        if (used != cmd._argv[3].size()) {
            throw std::invalid_argument("end");
        }
    } catch (const std::exception&) {
        return s._writer.error("ERR value is not an integer or out of range");
    }
    auto r = read_local(s, key);
    if (!r._error.empty()) {
        if (r._forward_to) {
            if (auto reply = forward(s, cmd, *r._forward_to)) {
                return *reply;
            }
        }
        return r._error;
    }
    if (!r._entry || is_expired(*r._entry)) {
        return s._writer.bulk("");
    }
    // Redis semantics: negative indexes count from the end, both ends are
    // inclusive, and out-of-range bounds clamp rather than error.
    const auto len = static_cast<std::int64_t>(value_access::size(*r._entry));
    if (start < 0) {
        start = std::max<std::int64_t>(0, len + start);
    }
    if (end < 0) {
        end = len + end;
    }
    if (end >= len) {
        end = len - 1;
    }
    if (len == 0 || start > end || start >= len) {
        return s._writer.bulk("");
    }
    auto bytes = value_access::read(r._context, *r._entry);
    if (!bytes) {
        return s._writer.bulk("");
    }
    auto span = bytes->view().subspan(static_cast<std::size_t>(start),
                                      static_cast<std::size_t>(end - start + 1));
    return s._writer.bulk(span);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_ttl(session& s, const resp_command& cmd)
    -> std::string {
    const auto& key = cmd._argv[1];
    if (auto e = authorize(s, "TTL", {key})) {
        return *e;
    }
    auto r = read_local(s, key);
    if (!r._error.empty()) {
        if (r._forward_to) {
            if (auto reply = forward(s, cmd, *r._forward_to)) {
                return *reply;
            }
        }
        return r._error;
    }
    if (!r._entry || is_expired(*r._entry)) {
        return s._writer.integer(-2);
    }
    if (r._entry->_expire_at_ms == 0) {
        return s._writer.integer(-1);
    }
    auto remaining_ms = r._entry->_expire_at_ms - now_ms();
    return s._writer.integer(static_cast<std::int64_t>((remaining_ms + 999) / 1000));
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_set(session& s, const resp_command& cmd,
                                                      bool with_expiry) -> std::string {
    const auto& key = cmd._argv[1];
    const auto& value = cmd._argv.back();
    const char* name = with_expiry ? "SETEX" : "SET";
    if (auto e = authorize(s, name, {key})) {
        return *e;
    }
    std::int64_t seconds = 0;
    if (with_expiry) {
        try {
            std::size_t used = 0;
            seconds = std::stoll(cmd._argv[2], &used);
            if (used != cmd._argv[2].size()) {
                throw std::invalid_argument("seconds");
            }
        } catch (const std::exception&) {
            return s._writer.error("ERR value is not an integer or out of range");
        }
        if (seconds <= 0) {
            return s._writer.error("ERR invalid expire time in 'setex' command");
        }
    }
    if (value.size() > _config._max_value_bytes) {
        ++_stats._oversize_rejections;
        emit("redis.oversize_rejections", name);
        return s._writer.error("ERR value exceeds the configured maximum of " +
                               std::to_string(_config._max_value_bytes) + " bytes");
    }

    std::string error;
    std::optional<node_id_type> forward_to;
    auto* node = locate(key, error, forward_to);
    if (node != nullptr && !node->is_leader()) {
        forward_to = node->known_leader();
        error = s._writer.error("ERR shard has no reachable leader, retry");
        node = nullptr;
    }
    if (node == nullptr) {
        if (forward_to) {
            if (auto reply = forward(s, cmd, *forward_to)) {
                return *reply;
            }
        }
        return error;
    }

    if (auto refused = space_refusal(key)) {
        ++_stats._space_refusals;
        emit("redis.space_refusals", name);
        return s._writer.error("ERR " + *refused + ", retry later");
    }

    if (_config._immutable_values) {
        // sccache keys are content hashes, so a rewrite is either the same
        // bytes (sccache rewriting its `.sccache_check` probe) or a bug
        // somewhere. The first is a free no-op; the second is refused.
        // Checked against a caught-up leader's state only; see read_local().
        auto looked_up = node->with_leader_state_machine(
            [&key](auto& sm) { return redis_gateway_detail::kv_of(sm).lookup(key); });
        if (!looked_up) {
            return s._writer.error("ERR shard has no reachable leader, retry");
        }
        const auto& existing = *looked_up;
        if (existing && !is_expired(*existing)) {
            if (value_access::equals(*existing, std::as_bytes(std::span(value)))) {
                return s._writer.simple_string("OK");
            }
            ++_stats._value_conflicts;
            emit("redis.value_conflicts", name);
            return s._writer.error("ERR value conflict for an existing key");
        }
    }

    redis_kv_set_command set;
    set._key = key;
    set._value.resize(value.size());
    if (!value.empty()) {
        std::memcpy(set._value.data(), value.data(), value.size());
    }
    // The deadline is resolved here, on the leader, so the log entry carries
    // an absolute time and every replica agrees on it (Requirement 6.1).
    set._expire_at_ms = with_expiry ? now_ms() + static_cast<std::uint64_t>(seconds) * 1000u : 0;
    auto encoded = encode_redis_kv_command(set);
    if (!submit(s, key, encoded, error, forward_to)) {
        if (forward_to) {
            if (auto reply = forward(s, cmd, *forward_to)) {
                return *reply;
            }
        }
        return error;
    }
    ++_stats._writes;
    emit("redis.writes", name);
    emit("redis.bytes_written", name, static_cast<std::int64_t>(value.size()));
    if (auto desc = _host.resolve(key)) {
        touch_lru(desc->group_id(), key);
    }
    return s._writer.simple_string("OK");
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_del(session& s, const resp_command& cmd)
    -> std::string {
    const auto& key = cmd._argv[1];
    if (auto e = authorize(s, "DEL", {key})) {
        return *e;
    }
    std::string error;
    std::optional<node_id_type> forward_to;
    auto* node = locate(key, error, forward_to);
    if (node != nullptr && !node->is_leader()) {
        forward_to = node->known_leader();
        error = s._writer.error("ERR shard has no reachable leader, retry");
        node = nullptr;
    }
    if (node == nullptr) {
        if (forward_to) {
            if (auto reply = forward(s, cmd, *forward_to)) {
                return *reply;
            }
        }
        return error;
    }
    // On a leader that has not caught up, "no such key" could be a key an
    // earlier leader acknowledged, and answering 0 would skip the delete.
    auto looked_up = node->with_leader_state_machine(
        [&key](auto& sm) { return redis_gateway_detail::kv_of(sm).lookup(key); });
    if (!looked_up) {
        return s._writer.error("ERR shard has no reachable leader, retry");
    }
    const auto& existing = *looked_up;
    if (!existing) {
        return s._writer.integer(0);
    }
    bool live = !is_expired(*existing);
    auto encoded = encode_redis_kv_command(redis_kv_del_command{key});
    if (!submit(s, key, encoded, error, forward_to)) {
        if (forward_to) {
            if (auto reply = forward(s, cmd, *forward_to)) {
                return *reply;
            }
        }
        return error;
    }
    ++_stats._deletes;
    emit("redis.deletes", "DEL");
    if (auto desc = _host.resolve(key)) {
        forget_lru(desc->group_id(), key);
    }
    return s._writer.integer(live ? 1 : 0);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_dbsize(session& s) -> std::string {
    std::size_t keys = 0;
    for (const auto& g : _host.group_ids()) {
        if (auto* node = _host.group_node(g)) {
            keys += node->with_state_machine(
                [](auto& sm) { return redis_gateway_detail::kv_of(sm).approximate_key_count(); });
        }
    }
    return s._writer.integer(static_cast<std::int64_t>(keys));
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_info(session& s) -> std::string {
    std::size_t keys = 0;
    std::size_t expiring = 0;
    std::size_t bytes = 0;
    std::size_t shards = 0;
    std::size_t led = 0;
    for (const auto& g : _host.group_ids()) {
        if (auto* node = _host.group_node(g)) {
            ++shards;
            if (node->is_leader()) {
                ++led;
            }
            auto [k, e, b] = node->with_state_machine([](auto& sm) {
                return std::tuple{redis_gateway_detail::kv_of(sm).approximate_key_count(),
                                  redis_gateway_detail::kv_of(sm).expiring_key_count(),
                                  redis_gateway_detail::kv_of(sm).approximate_size_bytes()};
            });
            keys += k;
            expiring += e;
            bytes += b;
        }
    }
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - _started_at);
    std::ostringstream out;
    out << "# Server\r\n"
        << "redis_version:7.0.0-kythira\r\n"
        << "kythira_gateway:1\r\n"
        << "redis_mode:cluster\r\n"
        << "uptime_in_seconds:" << uptime.count() << "\r\n"
        << "\r\n# Clients\r\n"
        << "connected_clients:" << _stats._connections_current.load() << "\r\n"
        << "maxclients:" << _config._max_clients << "\r\n"
        << "rejected_connections:" << _stats._connections_rejected.load() << "\r\n"
        << "\r\n# Memory\r\n"
        << "used_memory_dataset:" << bytes << "\r\n"
        << "\r\n# Keyspace\r\n"
        << "local_shards:" << shards << "\r\n"
        << "local_shards_led:" << led << "\r\n"
        << "db0:keys=" << keys << ",expires=" << expiring << ",avg_ttl=0\r\n"
        << "\r\n# Stats\r\n"
        << "total_commands_processed:" << _stats._commands.load() << "\r\n"
        << "total_connections_received:" << _stats._connections_accepted.load() << "\r\n"
        << "total_net_input_bytes:" << _stats._bytes_in.load() << "\r\n"
        << "total_net_output_bytes:" << _stats._bytes_out.load() << "\r\n"
        << "keyspace_hits:" << _stats._hits.load() << "\r\n"
        << "keyspace_misses:" << _stats._misses.load() << "\r\n"
        << "evicted_keys:" << _stats._evictions.load() << "\r\n"
        << "expired_keys:" << _stats._expirations.load() << "\r\n"
        << "kythira_writes:" << _stats._writes.load() << "\r\n"
        << "kythira_deletes:" << _stats._deletes.load() << "\r\n"
        << "kythira_value_conflicts:" << _stats._value_conflicts.load() << "\r\n"
        << "kythira_oversize_rejections:" << _stats._oversize_rejections.load() << "\r\n"
        << "kythira_auth_failures:" << _stats._auth_failures.load() << "\r\n"
        << "kythira_auth_busy:" << _stats._auth_busy.load() << "\r\n"
        << "kythira_authz_denials:" << _stats._authz_denials.load() << "\r\n"
        << "kythira_forwards:" << _stats._forwards.load() << "\r\n"
        << "kythira_forward_failures:" << _stats._forward_failures.load() << "\r\n"
        << "kythira_shed:" << _stats._shed.load() << "\r\n"
        << "kythira_output_stalls:" << _stats._output_stalls.load() << "\r\n";
    if (_cache_hooks._advisory || _cache_hooks._write_refusal) {
        out << "kythira_space_refusals:" << _stats._space_refusals.load() << "\r\n"
            << "kythira_advisories_sent:" << _stats._advisories_sent.load() << "\r\n"
            << "kythira_advisory_failures:" << _stats._advisory_failures.load() << "\r\n"
            << "kythira_advisories_received:" << _stats._advisories_received.load() << "\r\n"
            << "kythira_touches_dropped:" << _stats._touches_dropped.load() << "\r\n";
    }
    if (_cache_hooks._info) {
        out << "\r\n" << _cache_hooks._info();
    }
    return s._writer.bulk(out.str());
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_command(session& s, const resp_command& cmd)
    -> std::string {
    if (auto e = authorize(s, "COMMAND", {})) {
        return *e;
    }
    static constexpr std::array<std::string_view, 17> names = {
        "GET",   "SET",    "SETEX",  "DEL",  "EXISTS",  "STRLEN", "GETRANGE", "PING", "AUTH",
        "HELLO", "SELECT", "CLIENT", "QUIT", "COMMAND", "INFO",   "DBSIZE",   "TTL"};
    if (cmd._argv.size() >= 2) {
        auto sub = resp_to_upper(cmd._argv[1]);
        if (sub == "COUNT") {
            return s._writer.integer(static_cast<std::int64_t>(names.size()));
        }
        if (sub == "LIST") {
            std::vector<std::string> out;
            for (auto n : names) {
                out.push_back(s._writer.bulk(n));
            }
            return s._writer.array(out);
        }
        if (sub == "DOCS" || sub == "INFO") {
            return s._writer.array({});
        }
        return s._writer.error("ERR unknown subcommand '" + cmd._argv[1] + "'. Try COMMAND HELP.");
    }
    std::vector<std::string> out;
    for (auto n : names) {
        std::string lower(n);
        for (auto& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        out.push_back(
            s._writer.array({s._writer.bulk(lower), s._writer.integer(-1), s._writer.array({}),
                             s._writer.integer(0), s._writer.integer(0), s._writer.integer(0)}));
    }
    return s._writer.array(out);
}

// ─────────────────────────────────────────────────────────────────────────────
// transactions (.kiro/specs/multi-raft-transactions/ task 9)
// ─────────────────────────────────────────────────────────────────────────────

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_client::resolve(const std::string& key)
    -> std::optional<group_id_type> {
    if (auto desc = _gateway._host.resolve(key)) {
        return desc->group_id();
    }
    return std::nullopt;
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_client::send(const std::vector<std::string>& keys,
                                                            const std::vector<std::byte>& envelope,
                                                            std::chrono::milliseconds timeout)
    -> send_future {
    promise_default<std::vector<std::byte>> promise;
    auto result = promise.getFuture();
    try {
        promise.setValue(_gateway.txn_send(keys, envelope, timeout));
    } catch (...) {
        promise.setException(std::current_exception());
    }
    return result;
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_client::refresh(std::exception_ptr error) -> bool {
    try {
        std::rethrow_exception(std::move(error));
    } catch (const shard_epoch_mismatch_exception<group_id_type, std::string, node_id_type>& e) {
        std::ignore = _gateway._host.learn_descriptors(e.current_descriptors());
        return true;
    } catch (const redis_gateway_detail::txn_moved_error&) {
        // The peer's host knows the new tiling and this one will learn it
        // from its own routing; give it a moment before re-partitioning.
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        return true;
    } catch (...) {
        return false;
    }
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_client::read(const std::string& key,
                                                            std::chrono::milliseconds timeout)
    -> txn_read_result<std::string> {
    return _gateway.txn_read(key, timeout);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_client::now_ms() const -> std::uint64_t {
    return _gateway._host.txn_now_ms();
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_client::allocate_timestamps(
    std::size_t n, std::chrono::milliseconds timeout) -> timestamp_range
requires k_snapshot
{
    return _gateway.txn_allocate_timestamps(n, timeout);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_client::read_at(const std::string& key,
                                                               std::uint64_t ts,
                                                               std::chrono::milliseconds timeout)
    -> txn_read_result<std::string>
requires k_snapshot
{
    return _gateway.txn_read_at(key, ts, timeout);
}

/// The host's oracle when it leads the oracle group, else the leader's
/// gateway over `KYTHIRA.TXN TS`, which serves it from its own host and
/// never forwards again. Also the host's timestamp forwarder, so a plain
/// write on a node that does not lead the oracle gets its commit timestamp.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_allocate_timestamps(
    std::size_t n, std::chrono::milliseconds timeout) -> timestamp_range {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto left = [&deadline] {
        return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now()),
                        std::chrono::milliseconds{1});
    };
    std::string last = "no reachable timestamp oracle";
    for (;;) {
        std::optional<node_id_type> hint;
        try {
            return _host.allocate_timestamps(n, left()).get();
        } catch (const shard_not_leader_exception<group_id_type, node_id_type>& e) {
            hint = e.leader_hint();
        } catch (const timestamp_oracle_unavailable_exception& e) {
            last = e.what();
        }
        if (hint && *hint != _host.node_id()) {
            if (auto reply =
                    forward_argv({"KYTHIRA.TXN", "TS", std::to_string(n)}, *hint, "KYTHIRA.TXN")) {
                auto parsed = redis_gateway_detail::parse_peer_reply(*reply);
                using kind = redis_gateway_detail::peer_reply::kind;
                if (parsed._kind == kind::bulk) {
                    return redis_gateway_detail::decode_timestamp_range(parsed._body);
                }
                if (parsed._kind != kind::error ||
                    parsed._body.find("retry") == std::string::npos) {
                    throw timestamp_oracle_unavailable_exception(
                        "peer gateway refused a timestamp request: " + parsed._body);
                }
                last = parsed._body;
            }
        }
        if (!_running.load() || std::chrono::steady_clock::now() >= deadline) {
            throw timestamp_oracle_unavailable_exception("redis gateway: " + last);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_read_at_local(const std::string& key,
                                                             std::uint64_t ts,
                                                             std::chrono::milliseconds timeout)
    -> std::optional<txn_read_result<std::string>> {
    if constexpr (!k_snapshot) {
        return std::nullopt;
    } else {
        auto desc = _host.resolve(key);
        if (!desc) {
            return std::nullopt;
        }
        const auto group = desc->group_id();
        if (auto* node = _host.group_node(group); node == nullptr || !node->is_leader()) {
            return std::nullopt;
        }
        try {
            // Every entry committed before the read began is applied here
            // when this returns, so a commit at or below `ts` shows as a
            // version or as its intent (Requirement 8.5). Through the host,
            // which holds the group alive for the round.
            (void)_host.read_state(key, timeout).get();
            auto looked = _host.with_group_state_machine(
                group,
                [&](state_machine_type& sm, const auto& d)
                    -> std::optional<
                        std::pair<entry_handle, std::optional<intent_view<std::string>>>> {
                    if (!d._range.contains(key)) {
                        return std::nullopt;  // a split or merge moved it
                    }
                    if (ts < sm.safe_point()) {
                        // Versions this read needs may be gone.
                        throw snapshot_too_old_exception(ts, sm.safe_point());
                    }
                    return std::pair{sm.inner().read_at(key, ts), sm.intent_on(key)};
                });
            if (!looked || !*looked) {
                return std::nullopt;
            }
            auto& [entry, intent] = **looked;
            txn_read_result<std::string> out;
            out._intent = std::move(intent);
            // As a plain GET: a version whose TTL has run out is a miss.
            if (entry && !is_expired(*entry)) {
                const auto view = value_access::read({}, *entry)->view();
                out._value = std::vector<std::byte>(view.begin(), view.end());
            }
            return out;
        } catch (const snapshot_too_old_exception&) {
            throw;
        } catch (const std::exception&) {
            // Lost leadership mid-read: the caller finds the new leader.
            return std::nullopt;
        }
    }
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_read_at(const std::string& key, std::uint64_t ts,
                                                       std::chrono::milliseconds timeout)
    -> txn_read_result<std::string> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto left = [&deadline] {
        return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now()),
                        std::chrono::milliseconds{1});
    };
    std::string last = "no reachable leader for the key's shard";
    for (;;) {
        if (auto local = txn_read_at_local(key, ts, left())) {
            return std::move(*local);
        }
        if (auto to = txn_leader_of(key); to && *to != _host.node_id()) {
            if (auto reply = forward_argv({"KYTHIRA.TXN", "READAT", std::to_string(ts), key}, *to,
                                          "KYTHIRA.TXN")) {
                auto parsed = redis_gateway_detail::parse_peer_reply(*reply);
                using kind = redis_gateway_detail::peer_reply::kind;
                if (parsed._kind == kind::bulk) {
                    return redis_gateway_detail::decode_txn_read(parsed._body);
                }
                if (parsed._kind == kind::error) {
                    if (auto too_old = redis_gateway_detail::parse_too_old(parsed._body)) {
                        throw *too_old;
                    }
                }
                if (parsed._kind != kind::error ||
                    parsed._body.find("retry") == std::string::npos) {
                    throw std::runtime_error("peer gateway refused a transaction read: " +
                                             parsed._body);
                }
                last = parsed._body;
            }
        }
        if (!_running.load() || std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("redis gateway: " + last);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_leader_of(const std::string& key)
    -> std::optional<node_id_type> {
    auto desc = _host.resolve(key);
    if (!desc) {
        return std::nullopt;
    }
    if (auto* node = _host.group_node(desc->group_id()); node != nullptr) {
        return node->known_leader();
    }
    if (desc->leader_hint() && *desc->leader_hint() != _host.node_id()) {
        return desc->leader_hint();
    }
    for (const auto& voter : desc->voters()) {
        if (voter != _host.node_id()) {
            return voter;
        }
    }
    return std::nullopt;
}

/// Local leader first; otherwise the leader's gateway over the forward link,
/// which runs the op on its own host and never forwards it again. A peer
/// that answers with the retry error (it lost leadership on the way) is
/// asked again after a pause, until the timeout.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_send(const std::vector<std::string>& keys,
                                                    const std::vector<std::byte>& envelope,
                                                    std::chrono::milliseconds timeout)
    -> std::vector<std::byte> {
    if constexpr (!k_transactional) {
        throw std::logic_error("redis gateway: transactions need a transactional host");
    } else {
        if (keys.empty()) {
            throw std::invalid_argument("redis gateway: a transaction op names no key");
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto left = [&deadline] {
            return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - std::chrono::steady_clock::now()),
                            std::chrono::milliseconds{1});
        };
        std::string last = "no reachable leader for the shard";
        for (;;) {
            if (!_running.load()) {
                throw std::runtime_error("redis gateway: stopping");
            }
            std::optional<node_id_type> hint;
            try {
                auto f = _host.submit_txn(keys, envelope, left());
                if (!f.wait(left())) {
                    throw std::runtime_error("redis gateway: transaction op timed out");
                }
                return std::move(f).get();
            } catch (const shard_not_leader_exception<group_id_type, node_id_type>& e) {
                hint = e.leader_hint();
            } catch (const unknown_shard_exception<group_id_type>&) {
                // No replica here: ask whoever the routing row names.
            }
            auto to = hint ? hint : txn_leader_of(keys.front());
            if (to && *to != _host.node_id()) {
                std::vector<std::string> argv{"KYTHIRA.TXN", "SEND",
                                              redis_gateway_detail::to_string(envelope)};
                argv.insert(argv.end(), keys.begin(), keys.end());
                if (auto reply = forward_argv(argv, *to, "KYTHIRA.TXN")) {
                    auto parsed = redis_gateway_detail::parse_peer_reply(*reply);
                    using kind = redis_gateway_detail::peer_reply::kind;
                    if (parsed._kind == kind::bulk) {
                        return redis_gateway_detail::to_bytes(parsed._body);
                    }
                    if (parsed._kind == kind::error &&
                        parsed._body.rfind("KYTHIRA.MOVED", 0) == 0) {
                        throw redis_gateway_detail::txn_moved_error(parsed._body);
                    }
                    if (parsed._kind != kind::error ||
                        parsed._body.find("retry") == std::string::npos) {
                        throw std::runtime_error("peer gateway refused a transaction op: " +
                                                 parsed._body);
                    }
                    last = parsed._body;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("redis gateway: " + last);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_read_local(const std::string& key)
    -> std::optional<txn_read_result<std::string>> {
    if constexpr (!k_transactional) {
        return std::nullopt;
    } else {
        auto desc = _host.resolve(key);
        if (!desc) {
            return std::nullopt;
        }
        auto* node = _host.group_node(desc->group_id());
        if (node == nullptr || !node->is_leader()) {
            return std::nullopt;
        }
        // A caught-up leader only, as read_local(): a new leader's state may
        // not hold every write its predecessor acknowledged.
        // The entry, its read context and any intent under the node lock;
        // the bytes after it, as read_local() does.
        auto looked = node->with_leader_state_machine([&](auto& sm) {
            return std::tuple{redis_gateway_detail::kv_of(sm).lookup(key),
                              value_access::context_of(sm), sm.intent_on(key)};
        });
        if (!looked) {
            return std::nullopt;
        }
        auto& [entry, context, intent] = *looked;
        txn_read_result<std::string> out;
        out._intent = std::move(intent);
        // A leader whose node lacks the bytes answers a miss, as a plain
        // GET on it does (read_local()).
        if (entry && !is_expired(*entry) && value_access::servable(*entry)) {
            if (auto bytes = value_access::read(context, *entry)) {
                const auto view = bytes->view();
                out._value = std::vector<std::byte>(view.begin(), view.end());
            }
        }
        return out;
    }
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::txn_read(const std::string& key,
                                                    std::chrono::milliseconds timeout)
    -> txn_read_result<std::string> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string last = "no reachable leader for the key's shard";
    for (;;) {
        if (auto local = txn_read_local(key)) {
            return std::move(*local);
        }
        if (auto to = txn_leader_of(key); to && *to != _host.node_id()) {
            if (auto reply = forward_argv({"KYTHIRA.TXN", "READ", key}, *to, "KYTHIRA.TXN")) {
                auto parsed = redis_gateway_detail::parse_peer_reply(*reply);
                using kind = redis_gateway_detail::peer_reply::kind;
                if (parsed._kind == kind::bulk) {
                    return redis_gateway_detail::decode_txn_read(parsed._body);
                }
                if (parsed._kind != kind::error ||
                    parsed._body.find("retry") == std::string::npos) {
                    throw std::runtime_error("peer gateway refused a transaction read: " +
                                             parsed._body);
                }
                last = parsed._body;
            }
        }
        if (!_running.load() || std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("redis gateway: " + last);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::resolve_lock(const std::string& key,
                                                        const txn_id& holder,
                                                        const std::string& primary) -> bool {
    if constexpr (!k_transactional) {
        return false;
    } else {
        try {
            txn_client client(*this);
            coordinator_type coordinator(client, txn_options{.timeout = _config._command_timeout});
            const auto status =
                coordinator.resolve_intent(key, intent_view<std::string>{holder, primary, 0});
            if (status == txn_record_status::pending) {
                return false;
            }
            ++_stats._txn_lock_resolves;
            emit("redis.txn.lock_resolves", "resolve");
            return true;
        } catch (const std::exception& e) {
            _logger.log(log_level::debug, "redis gateway: resolving a lock failed",
                        {{"txn", holder.to_string()}, {"error", e.what()}});
            return false;
        }
    }
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::is_txn_command(std::string_view upper,
                                                          const resp_command& cmd) const -> bool {
    return upper == "MSET" || upper == "MGET" || upper == "MULTI" || upper == "EXEC" ||
           upper == "DISCARD" || upper == "WATCH" || upper == "UNWATCH" ||
           upper == "KYTHIRA.ISOLATION" || upper == "KYTHIRA.TXN" ||
           (upper == "DEL" && cmd._argv.size() > 2);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::isolation_of(const session& s) const -> txn_isolation {
    return s._isolation.value_or(_config._txn_isolation);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::make_options(const session& s) const -> txn_options {
    txn_options o;
    o.isolation = isolation_of(s);
    o.lock_ttl = _config._txn_lock_ttl;
    if (_config._command_timeout.count() > 0) {
        o.timeout = _config._command_timeout;
    }
    o.max_retries = _config._txn_max_retries;
    return o;
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::set_command(const std::string& key,
                                                       std::string_view value,
                                                       std::uint64_t expire_at_ms) const
    -> std::vector<std::byte> {
    redis_kv_set_command set;
    set._key = key;
    set._value = redis_gateway_detail::to_bytes(value);
    set._expire_at_ms = expire_at_ms;
    return encode_redis_kv_command(set);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::outcome_error(session& s,
                                                         const txn_outcome<std::string>& outcome)
    -> std::string {
    switch (outcome._kind) {
        case txn_outcome_kind::committed:
            ++_stats._txn_committed;
            emit("redis.txn.committed", to_string(isolation_of(s)));
            return {};
        case txn_outcome_kind::aborted: {
            ++_stats._txn_aborted;
            emit("redis.txn.aborted", to_string(isolation_of(s)));
            std::string why;
            switch (outcome._reason) {
                case txn_abort_reason::write_conflict:
                    why = "write conflict" + (outcome._conflict_key
                                                  ? " on '" + *outcome._conflict_key + "'"
                                                  : std::string{});
                    break;
                case txn_abort_reason::expired:
                    why = "its locks expired before the commit point";
                    break;
                case txn_abort_reason::unroutable:
                    why = "a key is owned by no shard";
                    break;
                case txn_abort_reason::snapshot_too_old:
                    why = "its snapshot is older than a shard's safe point";
                    break;
                case txn_abort_reason::read_conflict:
                    why = "a key it read changed after it started" +
                          (outcome._conflict_key ? " ('" + *outcome._conflict_key + "')"
                                                 : std::string{});
                    break;
                default:
                    why = "a shard did not prepare" +
                          (outcome._detail.empty() ? std::string{} : " (" + outcome._detail + ")");
                    break;
            }
            return s._writer.error("ERR transaction aborted: " + why + "; nothing applied, retry");
        }
        case txn_outcome_kind::unknown:
            ++_stats._txn_unknown;
            emit("redis.txn.unknown", to_string(isolation_of(s)));
            // Never a retry hint: the writes may have applied.
            return s._writer.error("ERR transaction " + outcome._txn.to_string() +
                                   " outcome unknown; it may have applied (" + outcome._detail +
                                   ")");
    }
    return s._writer.error("ERR transaction outcome unrecognised");
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::execute_txn_command(session& s, std::string_view upper,
                                                               const resp_command& cmd)
    -> std::string {
    auto wrong_args = [&] {
        std::string lower = cmd._argv[0];
        for (auto& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s._writer.error("ERR wrong number of arguments for '" + lower + "' command");
    };
    if (upper == "KYTHIRA.TXN") {
        return handle_internal_txn(s, cmd);
    }
    if (upper == "WATCH" || upper == "UNWATCH") {
        // Requirement 11.4: only under mvcc::on, which needs the timestamp
        // oracle of tier 2 and the read validation of tier 3.
        if constexpr (k_snapshot) {
            return handle_watch(s, cmd);
        } else {
            return s._writer.error("ERR " + std::string(upper) +
                                   " needs a build with mvcc::on (multi-version transactions)");
        }
    }
    if (upper == "MULTI") {
        if (cmd._argv.size() != 1) {
            return wrong_args();
        }
        if (auto e = authorize(s, "MULTI", {})) {
            return *e;
        }
        if (s._in_multi) {
            return s._writer.error("ERR MULTI calls can not be nested");
        }
        s._in_multi = true;
        s._multi_failed = false;
        s._queued.clear();
        return s._writer.simple_string("OK");
    }
    if (upper == "DISCARD") {
        if (!s._in_multi) {
            return s._writer.error("ERR DISCARD without MULTI");
        }
        s._in_multi = false;
        s._multi_failed = false;
        s._queued.clear();
        s._watched.clear();  // as Redis: DISCARD unwatches
        s._watch_ts = 0;
        return s._writer.simple_string("OK");
    }
    if (upper == "EXEC") {
        if (!s._in_multi) {
            return s._writer.error("ERR EXEC without MULTI");
        }
        s._in_multi = false;
        if (s._multi_failed) {
            s._multi_failed = false;
            s._queued.clear();
            s._watched.clear();
            s._watch_ts = 0;
            return s._writer.error("EXECABORT Transaction discarded because of previous errors.");
        }
        return handle_exec(s);
    }
    if (upper == "KYTHIRA.ISOLATION") {
        if (cmd._argv.size() > 2) {
            return wrong_args();
        }
        return handle_isolation(s, cmd);
    }
    if (upper == "MSET") {
        if (cmd._argv.size() < 3 || cmd._argv.size() % 2 == 0) {
            return wrong_args();
        }
        return handle_mset(s, cmd);
    }
    if (upper == "MGET") {
        if (cmd._argv.size() < 2) {
            return wrong_args();
        }
        return handle_mget(s, cmd);
    }
    return handle_multi_del(s, cmd);
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_isolation(session& s, const resp_command& cmd)
    -> std::string {
    if (auto e = authorize(s, "KYTHIRA.ISOLATION", {})) {
        return *e;
    }
    if (cmd._argv.size() == 1) {
        return s._writer.bulk(to_string(isolation_of(s)));
    }
    auto level = parse_txn_isolation(cmd._argv[1]);
    if (!level) {
        return s._writer.error("ERR unknown isolation level '" + cmd._argv[1] + "'");
    }
    const auto offered = coordinator_type::offered_isolations();
    if (std::find(offered.begin(), offered.end(), *level) == offered.end()) {
        return s._writer.error(std::string("ERR ") +
                               isolation_unsupported_exception(*level, offered).what());
    }
    s._isolation = level;
    return s._writer.simple_string("OK");
}

/// `WATCH key [key ...]` and `UNWATCH` on the snapshot build (task 16).
///
/// The first `WATCH` takes an oracle timestamp; the next `EXEC` runs as a
/// serializable transaction started there, with every watched key in its
/// read set, so it commits only if none of them has a version newer than
/// that timestamp (and none is held by another transaction) when it
/// prepares. A later `WATCH` adds keys under the same timestamp, which is
/// stricter than Redis (a change between the two `WATCH`es also counts).
/// The reads queued in the `MULTI` are at that timestamp too, and an
/// `EXEC` with no writes is a read-only transaction: it commits without a
/// prepare, its reads one consistent snapshot as of the `WATCH`.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_watch(session& s, const resp_command& cmd)
    -> std::string {
    const auto upper = resp_to_upper(cmd._argv[0]);
    if (upper == "UNWATCH") {
        if (cmd._argv.size() != 1) {
            return s._writer.error("ERR wrong number of arguments for 'unwatch' command");
        }
        if (auto e = authorize(s, "UNWATCH", {})) {
            return *e;
        }
        s._watched.clear();
        s._watch_ts = 0;
        return s._writer.simple_string("OK");
    }
    if (cmd._argv.size() < 2) {
        return s._writer.error("ERR wrong number of arguments for 'watch' command");
    }
    std::vector<std::string_view> keys(cmd._argv.begin() + 1, cmd._argv.end());
    if (auto e = authorize(s, "WATCH", keys)) {
        return *e;
    }
    if (s._watch_ts == 0) {
        try {
            s._watch_ts = txn_allocate_timestamps(1, _config._command_timeout)._first;
        } catch (const std::exception& e) {
            return s._writer.error(std::string("ERR WATCH could not take a timestamp: ") +
                                   e.what() + ", retry");
        }
    }
    for (std::size_t i = 1; i < cmd._argv.size(); ++i) {
        s._watched.insert(cmd._argv[i]);
    }
    return s._writer.simple_string("OK");
}

/// One `MSET`: every pair in one transaction, all or none (Requirement 11.1).
/// Under `_immutable_values` a key already holding the same bytes is left
/// out of the write, and one holding other bytes refuses the whole command,
/// as `SET` refuses it.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_mset(session& s, const resp_command& cmd)
    -> std::string {
    if constexpr (!k_transactional) {
        return s._writer.error("ERR transactions are not built in");
    } else {
        std::vector<std::string_view> keys;
        for (std::size_t i = 1; i < cmd._argv.size(); i += 2) {
            keys.emplace_back(cmd._argv[i]);
        }
        if (auto e = authorize(s, "MSET", keys)) {
            return *e;
        }
        for (std::size_t i = 2; i < cmd._argv.size(); i += 2) {
            if (cmd._argv[i].size() > _config._max_value_bytes) {
                ++_stats._oversize_rejections;
                emit("redis.oversize_rejections", "MSET");
                return s._writer.error("ERR value exceeds the configured maximum of " +
                                       std::to_string(_config._max_value_bytes) + " bytes");
            }
        }
        try {
            txn_client client(*this);
            coordinator_type coordinator(client);
            coordinator.begin(make_options(s));
            // A repeated key: the last value wins, as in Redis.
            std::map<std::string, std::string_view> pairs;
            for (std::size_t i = 1; i < cmd._argv.size(); i += 2) {
                pairs.insert_or_assign(cmd._argv[i], cmd._argv[i + 1]);
            }
            std::size_t bytes = 0;
            for (const auto& [key, value] : pairs) {
                if (_config._immutable_values) {
                    if (auto existing = coordinator.get(key)) {
                        if (redis_gateway_detail::to_string(*existing) == value) {
                            continue;
                        }
                        coordinator.rollback();
                        ++_stats._value_conflicts;
                        emit("redis.value_conflicts", "MSET");
                        return s._writer.error("ERR value conflict for an existing key");
                    }
                }
                coordinator.put(key, set_command(key, value, 0));
                bytes += value.size();
            }
            auto outcome = coordinator.commit();
            if (auto e = outcome_error(s, outcome); !e.empty()) {
                return e;
            }
            _stats._writes += pairs.size();
            emit("redis.writes", "MSET", static_cast<std::int64_t>(pairs.size()));
            emit("redis.bytes_written", "MSET", static_cast<std::int64_t>(bytes));
            return s._writer.simple_string("OK");
        } catch (const std::exception& e) {
            return s._writer.error(std::string("ERR transaction failed: ") + e.what() + ", retry");
        }
    }
}

/// `MGET`: a read-only transaction. Each key is read at the connection's
/// isolation; under the tier-1 levels that is per key, not one snapshot.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_mget(session& s, const resp_command& cmd)
    -> std::string {
    if constexpr (!k_transactional) {
        return s._writer.error("ERR transactions are not built in");
    } else {
        std::vector<std::string_view> keys(cmd._argv.begin() + 1, cmd._argv.end());
        if (auto e = authorize(s, "MGET", keys)) {
            return *e;
        }
        try {
            txn_client client(*this);
            coordinator_type coordinator(client);
            coordinator.begin(make_options(s));
            std::vector<std::string> out;
            out.reserve(keys.size());
            for (std::size_t i = 1; i < cmd._argv.size(); ++i) {
                auto value = coordinator.get(cmd._argv[i]);
                if (value) {
                    ++_stats._hits;
                    out.push_back(s._writer.bulk(std::span<const std::byte>(*value)));
                } else {
                    ++_stats._misses;
                    out.push_back(s._writer.null());
                }
            }
            coordinator.rollback();
            return s._writer.array(out);
        } catch (const std::exception& e) {
            return s._writer.error(std::string("ERR transaction failed: ") + e.what() + ", retry");
        }
    }
}

/// Multi-key `DEL`: one transaction deleting every key that was present
/// when read, and the count of those, as Redis replies.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_multi_del(session& s, const resp_command& cmd)
    -> std::string {
    if constexpr (!k_transactional) {
        return s._writer.error("ERR transactions are not built in");
    } else {
        std::vector<std::string_view> keys(cmd._argv.begin() + 1, cmd._argv.end());
        if (auto e = authorize(s, "DEL", keys)) {
            return *e;
        }
        try {
            txn_client client(*this);
            coordinator_type coordinator(client);
            coordinator.begin(make_options(s));
            std::set<std::string> present;
            for (std::size_t i = 1; i < cmd._argv.size(); ++i) {
                const auto& key = cmd._argv[i];
                if (!present.contains(key) && coordinator.get(key).has_value()) {
                    present.insert(key);
                    coordinator.put(key, encode_redis_kv_command(redis_kv_del_command{key}));
                }
            }
            auto outcome = coordinator.commit();
            if (auto e = outcome_error(s, outcome); !e.empty()) {
                return e;
            }
            _stats._deletes += present.size();
            emit("redis.deletes", "DEL", static_cast<std::int64_t>(present.size()));
            for (const auto& key : present) {
                if (auto desc = _host.resolve(key)) {
                    forget_lru(desc->group_id(), key);
                }
            }
            return s._writer.integer(static_cast<std::int64_t>(present.size()));
        } catch (const std::exception& e) {
            return s._writer.error(std::string("ERR transaction failed: ") + e.what() + ", retry");
        }
    }
}

/// Inside `MULTI`: check and queue. A refused command dooms the `EXEC`
/// (EXECABORT), as in Redis.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::queue_in_multi(session& s, std::string_view upper,
                                                          const resp_command& cmd) -> std::string {
    auto refuse = [&](std::string reply) {
        s._multi_failed = true;
        return reply;
    };
    const auto argc = cmd._argv.size();
    bool arity_ok = false;
    std::vector<std::string_view> keys;
    if (upper == "GET" || upper == "EXISTS") {
        arity_ok = argc == 2;
    } else if (upper == "SET") {
        arity_ok = argc == 3;
    } else if (upper == "SETEX") {
        arity_ok = argc == 4;
    } else if (upper == "DEL" || upper == "MGET") {
        arity_ok = argc >= 2;
    } else if (upper == "MSET") {
        arity_ok = argc >= 3 && argc % 2 == 1;
    } else {
        return refuse(s._writer.error("ERR " + std::string(upper) +
                                      " is not supported inside MULTI on this gateway"));
    }
    if (!arity_ok) {
        std::string lower = cmd._argv[0];
        for (auto& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return refuse(s._writer.error("ERR wrong number of arguments for '" + lower + "' command"));
    }
    const bool pairs = upper == "MSET";
    for (std::size_t i = 1; i < argc; i += pairs ? 2 : 1) {
        keys.emplace_back(cmd._argv[i]);
        if (upper == "SET" || upper == "SETEX") {
            break;
        }
    }
    if (auto e = authorize(s, upper, keys)) {
        return refuse(*e);
    }
    if (upper == "SETEX") {
        std::int64_t seconds = 0;
        try {
            std::size_t used = 0;
            seconds = std::stoll(cmd._argv[2], &used);
            if (used != cmd._argv[2].size()) {
                throw std::invalid_argument("seconds");
            }
        } catch (const std::exception&) {
            return refuse(s._writer.error("ERR value is not an integer or out of range"));
        }
        if (seconds <= 0) {
            return refuse(s._writer.error("ERR invalid expire time in 'setex' command"));
        }
    }
    if (upper == "SET" || upper == "SETEX" || upper == "MSET") {
        for (std::size_t i = upper == "MSET" ? 2 : argc - 1; i < argc; i += 2) {
            if (cmd._argv[i].size() > _config._max_value_bytes) {
                ++_stats._oversize_rejections;
                return refuse(s._writer.error("ERR value exceeds the configured maximum of " +
                                              std::to_string(_config._max_value_bytes) + " bytes"));
            }
        }
    }
    s._queued.push_back(cmd);
    return s._writer.simple_string("QUEUED");
}

/// `EXEC`: the queued commands as one transaction. Reads run now, at the
/// connection's isolation, and see the transaction's own earlier writes, as
/// a Redis `MULTI` does. Writes are buffered and committed together; if the
/// commit does not commit, `EXEC` answers with that error instead of the
/// array, and nothing applied. A `SET` whose key holds other bytes under
/// `_immutable_values` is that element's error and writes nothing, the way
/// Redis reports a per-command runtime error inside `EXEC`.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_exec(session& s) -> std::string {
    auto queued = std::move(s._queued);
    s._queued.clear();
    // EXEC unwatches whatever its outcome, as in Redis.
    const auto watched = std::exchange(s._watched, {});
    const auto watch_ts = std::exchange(s._watch_ts, 0);
    if constexpr (!k_transactional) {
        return s._writer.error("ERR transactions are not built in");
    } else {
        try {
            txn_client client(*this);
            coordinator_type coordinator(client);
            auto options = make_options(s);
            if (!watched.empty()) {
                // The whole EXEC runs as a serializable transaction that
                // started at the first WATCH: its reads are at that point,
                // and its commit fails validation if a watched key changed
                // since (task 16). Not retried: the watch is what failed.
                options.isolation = txn_isolation::serializable;
                options.start_ts = watch_ts;
                options.max_retries = 0;
            }
            coordinator.begin(options);
            for (const auto& key : watched) {
                coordinator.watch(key);
            }
            // The transaction's own view: a buffered write, or a deletion.
            std::map<std::string, std::optional<std::string>> mine;
            auto view = [&](const std::string& key) -> std::optional<std::string> {
                if (auto it = mine.find(key); it != mine.end()) {
                    return it->second;
                }
                if (auto v = coordinator.get(key)) {
                    return redis_gateway_detail::to_string(*v);
                }
                return std::nullopt;
            };
            auto conflicts = [&](const std::string& key, const std::string& value) {
                if (!_config._immutable_values) {
                    return false;
                }
                auto existing = view(key);
                return existing.has_value() && *existing != value;
            };
            std::vector<std::string> replies;
            replies.reserve(queued.size());
            std::size_t writes = 0;
            std::size_t deletes = 0;
            for (const auto& cmd : queued) {
                const auto upper = resp_to_upper(cmd._argv[0]);
                if (upper == "GET") {
                    auto v = view(cmd._argv[1]);
                    replies.push_back(v ? s._writer.bulk(*v) : s._writer.null());
                } else if (upper == "EXISTS") {
                    replies.push_back(s._writer.integer(view(cmd._argv[1]).has_value() ? 1 : 0));
                } else if (upper == "MGET") {
                    std::vector<std::string> out;
                    for (std::size_t i = 1; i < cmd._argv.size(); ++i) {
                        auto v = view(cmd._argv[i]);
                        out.push_back(v ? s._writer.bulk(*v) : s._writer.null());
                    }
                    replies.push_back(s._writer.array(out));
                } else if (upper == "SET" || upper == "SETEX") {
                    const auto& key = cmd._argv[1];
                    const auto& value = cmd._argv.back();
                    if (conflicts(key, value)) {
                        ++_stats._value_conflicts;
                        replies.push_back(
                            s._writer.error("ERR value conflict for an existing key"));
                        continue;
                    }
                    std::uint64_t expire_at = 0;
                    if (upper == "SETEX") {
                        expire_at =
                            now_ms() + static_cast<std::uint64_t>(std::stoll(cmd._argv[2])) * 1000U;
                    }
                    coordinator.put(key, set_command(key, value, expire_at));
                    mine.insert_or_assign(key, value);
                    ++writes;
                    replies.push_back(s._writer.simple_string("OK"));
                } else if (upper == "MSET") {
                    bool refused = false;
                    for (std::size_t i = 1; i < cmd._argv.size() && !refused; i += 2) {
                        refused = conflicts(cmd._argv[i], cmd._argv[i + 1]);
                    }
                    if (refused) {
                        ++_stats._value_conflicts;
                        replies.push_back(
                            s._writer.error("ERR value conflict for an existing key"));
                        continue;
                    }
                    for (std::size_t i = 1; i < cmd._argv.size(); i += 2) {
                        coordinator.put(cmd._argv[i],
                                        set_command(cmd._argv[i], cmd._argv[i + 1], 0));
                        mine.insert_or_assign(cmd._argv[i], cmd._argv[i + 1]);
                        ++writes;
                    }
                    replies.push_back(s._writer.simple_string("OK"));
                } else {  // DEL
                    std::int64_t count = 0;
                    for (std::size_t i = 1; i < cmd._argv.size(); ++i) {
                        const auto& key = cmd._argv[i];
                        if (view(key).has_value()) {
                            ++count;
                            coordinator.put(key,
                                            encode_redis_kv_command(redis_kv_del_command{key}));
                            mine.insert_or_assign(key, std::nullopt);
                        }
                    }
                    deletes += static_cast<std::size_t>(count);
                    replies.push_back(s._writer.integer(count));
                }
            }
            auto outcome = coordinator.commit();
            // A watched key that changed fails one of two ways: read
            // validation (`read_conflict`), or, when the MULTI also writes
            // that key, first-committer-wins finding a version newer than
            // the WATCH (`write_conflict` with no holder). Both mean what
            // Redis means by a null array: a watched key changed, nothing
            // applied. A conflict with a live holder is not this: that is
            // a lock, already retried, and keeps its error.
            const bool watched_changed =
                outcome._reason == txn_abort_reason::read_conflict ||
                (outcome._reason == txn_abort_reason::write_conflict &&
                 !outcome._holder.has_value() && outcome._conflict_key.has_value() &&
                 watched.contains(*outcome._conflict_key));
            if (!watched.empty() && outcome._kind == txn_outcome_kind::aborted && watched_changed) {
                // Redis's answer for a watched key that changed: a null
                // array, nothing applied.
                ++_stats._txn_aborted;
                emit("redis.txn.aborted", "watch");
                return s._writer.null_array();
            }
            if (auto e = outcome_error(s, outcome); !e.empty()) {
                return e;
            }
            _stats._writes += writes;
            _stats._deletes += deletes;
            return s._writer.array(replies);
        } catch (const std::exception& e) {
            return s._writer.error(std::string("ERR transaction failed: ") + e.what() + ", retry");
        }
    }
}

/// `KYTHIRA.TXN SEND <envelope> <key>...` and `KYTHIRA.TXN READ <key>`: the
/// coordinator's hop to a shard leader's gateway (Requirement 11.5). Only on
/// a connection authenticated as the internal identity, and never forwarded
/// again: a gateway that does not lead the shard answers with the retry
/// error and the sender asks again.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_internal_txn(session& s, const resp_command& cmd)
    -> std::string {
    if (!s._internal) {
        audit(s, "KYTHIRA.TXN", "internal command from a client");
        return s._writer.error("NOPERM KYTHIRA.TXN is internal to the gateways");
    }
    if (cmd._argv.size() < 3) {
        return s._writer.error("ERR wrong number of arguments for 'kythira.txn' command");
    }
    const auto sub = resp_to_upper(cmd._argv[1]);
    if constexpr (!k_transactional) {
        return s._writer.error("ERR transactions are not built in");
    } else {
        if (sub == "READ") {
            if (cmd._argv.size() != 3) {
                return s._writer.error("ERR wrong number of arguments for 'kythira.txn' command");
            }
            auto local = txn_read_local(cmd._argv[2]);
            if (!local) {
                return s._writer.error("ERR shard has no reachable leader, retry");
            }
            return s._writer.bulk(redis_gateway_detail::encode_txn_read(*local));
        }
        if (sub == "TS") {
            // Tier 2: timestamps from this host's oracle, never forwarded.
            std::size_t n = 0;
            const auto& arg = cmd._argv[2];
            if (cmd._argv.size() != 3 ||
                std::from_chars(arg.data(), arg.data() + arg.size(), n).ec != std::errc{} ||
                n == 0) {
                return s._writer.error("ERR wrong arguments for 'kythira.txn ts'");
            }
            try {
                auto range = _host.allocate_timestamps(n, _config._command_timeout).get();
                return s._writer.bulk(redis_gateway_detail::encode_timestamp_range(range));
            } catch (const shard_not_leader_exception<group_id_type, node_id_type>&) {
                return s._writer.error("ERR not the timestamp oracle's leader, retry");
            } catch (const std::exception& e) {
                return s._writer.error(std::string("ERR timestamp oracle: ") + e.what() +
                                       ", retry");
            }
        }
        if (sub == "READAT") {
            // Tier 2: `READAT <ts> <key>`, on this replica only.
            std::uint64_t ts = 0;
            const auto& arg = cmd._argv[2];
            if (cmd._argv.size() != 4 ||
                std::from_chars(arg.data(), arg.data() + arg.size(), ts).ec != std::errc{}) {
                return s._writer.error("ERR wrong arguments for 'kythira.txn readat'");
            }
            try {
                auto local = txn_read_at_local(cmd._argv[3], ts, _config._command_timeout);
                if (!local) {
                    return s._writer.error("ERR shard has no reachable leader, retry");
                }
                return s._writer.bulk(redis_gateway_detail::encode_txn_read(*local));
            } catch (const snapshot_too_old_exception& e) {
                return s._writer.error(std::string(redis_gateway_detail::k_too_old_prefix) +
                                       std::to_string(e.start_ts()) + " " +
                                       std::to_string(e.safe_point()));
            }
        }
        if (sub == "SEND") {
            if (cmd._argv.size() < 4) {
                return s._writer.error("ERR wrong number of arguments for 'kythira.txn' command");
            }
            std::vector<std::string> keys(cmd._argv.begin() + 3, cmd._argv.end());
            try {
                auto f = _host.submit_txn(keys, redis_gateway_detail::to_bytes(cmd._argv[2]),
                                          _config._command_timeout);
                if (!f.wait(_config._command_timeout)) {
                    return s._writer.error("ERR timeout submitting to shard, retry");
                }
                return s._writer.bulk(std::span<const std::byte>(std::move(f).get()));
            } catch (
                const shard_epoch_mismatch_exception<group_id_type, std::string, node_id_type>& e) {
                return s._writer.error(std::string("KYTHIRA.MOVED ") + e.what());
            } catch (const shard_not_leader_exception<group_id_type, node_id_type>&) {
                return s._writer.error("ERR shard has no reachable leader, retry");
            } catch (const unknown_shard_exception<group_id_type>&) {
                return s._writer.error("ERR shard has no reachable leader, retry");
            } catch (const std::exception& e) {
                return s._writer.error(std::string("ERR transaction op failed: ") + e.what());
            }
        }
        return s._writer.error("ERR unknown KYTHIRA.TXN subcommand '" + cmd._argv[1] + "'");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// maintenance: expiry sweep and eviction
// ─────────────────────────────────────────────────────────────────────────────

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::run_maintenance() -> std::size_t {
    if (!_host.is_running()) {
        return 0;
    }
    std::size_t proposals = 0;
    const auto now = now_ms();
    for (const auto& group : _host.group_ids()) {
        auto* node = _host.group_node(group);
        if (node == nullptr || !node->is_leader()) {
            // Followers keep no LRU: theirs would be stale on promotion anyway.
            std::lock_guard<std::mutex> lock(_lru_mutex);
            _lru.erase(group);
            continue;
        }
        auto desc = _host.local_descriptor(group);
        if (!desc) {
            continue;
        }

        // Expiry: a bounded batch per tick, never the whole backlog at once.
        auto expired = node->with_state_machine([&](auto& sm) {
            auto out = redis_gateway_detail::kv_of(sm).collect_expired(now, _config._sweep_batch);
            if constexpr (k_transactional) {
                // A key a transaction holds is the transaction's to finish;
                // a sweep naming it would be refused whole.
                std::erase_if(out,
                              [&sm](const auto& e) { return sm.intent_on(e._key).has_value(); });
            }
            return out;
        });
        if (!expired.empty()) {
            redis_kv_sweep_command sweep;
            sweep._entries = std::move(expired);
            auto count = sweep._entries.size();
            try {
                auto f = _host.submit_command(group, desc->epoch(), encode_redis_kv_command(sweep),
                                              _config._command_timeout);
                if (f.wait(_config._command_timeout)) {
                    (void)std::move(f).get();
                    _stats._expirations += count;
                    emit("redis.expirations", "sweep", static_cast<std::int64_t>(count));
                    ++proposals;
                    for (const auto& e : sweep._entries) {
                        forget_lru(group, e._key);
                    }
                }
            } catch (const std::exception& e) {
                _logger.log(log_level::debug, "redis gateway: sweep proposal failed",
                            {{"error", e.what()}});
            }
        }

        // Eviction: only when the shard is over its budget. Over-budget is a
        // metric and a log line; it is never a reason to refuse a write.
        auto [bytes, key_count] = node->with_state_machine([](auto& sm) {
            return std::pair{redis_gateway_detail::kv_of(sm).approximate_size_bytes(),
                             redis_gateway_detail::kv_of(sm).approximate_key_count()};
        });
        // The node disk budget (Requirement 6.1): bytes the fullest voter of
        // this shard holds over it, to be freed from this shard.
        const std::uint64_t excess =
            _cache_hooks._excess_bytes ? _cache_hooks._excess_bytes(group, desc->voters()) : 0;
        if ((bytes <= _config._max_shard_bytes && excess == 0) || key_count == 0) {
            continue;
        }
        ++_stats._over_budget_ticks;
        emit("redis.shard_over_budget", "evict");
        _logger.log(log_level::warning, "redis gateway: shard over budget, evicting",
                    {{"group", std::to_string(group)},
                     {"bytes", std::to_string(bytes)},
                     {"budget", std::to_string(_config._max_shard_bytes)},
                     {"disk_excess", std::to_string(excess)}});

        std::vector<std::string> victims;
        {
            std::lock_guard<std::mutex> lock(_lru_mutex);
            auto& lru = _lru[group];
            if (lru._order.empty()) {
                // Fresh leadership: seed from the shard itself so eviction can
                // proceed. Key order is arbitrary as an LRU, which is fine —
                // any order beats refusing to evict.
                node->with_state_machine([&](auto& sm) {
                    redis_gateway_detail::kv_of(sm).for_each_key(
                        [&](const std::string& key, const entry_type&) {
                            lru._order.push_back(key);
                            lru._index.emplace(key, std::prev(lru._order.end()));
                        });
                });
            }
            // Walk from the least recently read end. Against the shard budget
            // every key counts its full size. Against the disk excess a key
            // counts only the bytes evicting it frees on disk: a key whose
            // chunks are still pending (a fresh write, inside the orphan
            // grace) or shared frees nothing, so it is passed over rather
            // than evicted for no gain. Without this a burst of writes over
            // the budget would empty the shard and free nothing. The walk is
            // capped so a shard of pinned keys costs a bounded scan per tick.
            std::size_t freed = 0;
            std::uint64_t disk_freed = 0;
            std::size_t scanned = 0;
            const std::size_t max_scan = _config._sweep_batch * evict_scan_factor;
            auto it = lru._order.end();
            while (it != lru._order.begin() && victims.size() < _config._sweep_batch &&
                   scanned < max_scan) {
                const bool shard_over = freed < bytes && bytes - freed > _config._max_shard_bytes;
                if (!shard_over && disk_freed >= excess) {
                    break;
                }
                --it;
                ++scanned;
                const std::string key = *it;
                auto [size, reclaim] = node->with_state_machine([&key](auto& sm) {
                    auto& kv = redis_gateway_detail::kv_of(sm);
                    auto entry = kv.lookup(key);
                    const std::uint64_t sz = key.size() + (entry ? value_access::size(*entry) : 0);
                    if constexpr (requires { kv.reclaimable_bytes(key); }) {
                        return std::pair<std::uint64_t, std::uint64_t>{sz,
                                                                       kv.reclaimable_bytes(key)};
                    } else {
                        return std::pair<std::uint64_t, std::uint64_t>{sz, sz};
                    }
                });
                if (!shard_over && reclaim == 0) {
                    continue;
                }
                freed += size;
                disk_freed += reclaim;
                victims.push_back(key);
                lru._index.erase(key);
                it = lru._order.erase(it);
            }
        }
        if constexpr (k_transactional) {
            node->with_state_machine([&](auto& sm) {
                std::erase_if(victims, [&sm](const std::string& key) {
                    return sm.intent_on(key).has_value();
                });
            });
        }
        if (victims.empty()) {
            continue;
        }
        try {
            auto count = victims.size();
            auto f = _host.submit_command(
                group, desc->epoch(),
                encode_redis_kv_command(redis_kv_evict_command{std::move(victims)}),
                _config._command_timeout);
            if (f.wait(_config._command_timeout)) {
                (void)std::move(f).get();
                _stats._evictions += count;
                emit("redis.evictions", "evict", static_cast<std::int64_t>(count));
                ++proposals;
            }
        } catch (const std::exception& e) {
            _logger.log(log_level::debug, "redis gateway: evict proposal failed",
                        {{"error", e.what()}});
        }
    }
    send_advisories();
    return proposals;
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::space_refusal(const std::string& key)
    -> std::optional<std::string> {
    if (!_cache_hooks._write_refusal) {
        return std::nullopt;
    }
    auto desc = _host.resolve(key);
    if (!desc) {
        return std::nullopt;
    }
    return _cache_hooks._write_refusal(desc->group_id(), desc->voters());
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::send_advisories() -> void {
    if (!_cache_hooks._advisory || !_running.load() || _config._internal_secret.empty() ||
        (!_config._forward_tls && !_config._allow_plaintext_forwarding)) {
        return;
    }
    // Every leader of a shard this node follows hears from it, with the
    // reads it served for that leader's shards.
    std::map<node_id_type, std::vector<std::pair<group_id_type, std::vector<std::string>>>> batches;
    std::map<group_id_type, std::set<std::string>> queued;
    {
        std::lock_guard<std::mutex> lock(_lru_mutex);
        queued.swap(_remote_touches);
    }
    for (const auto& group : _host.group_ids()) {
        auto* node = _host.group_node(group);
        if (node == nullptr || node->is_leader()) {
            continue;
        }
        auto leader = node->known_leader();
        if (!leader || *leader == _host.node_id()) {
            continue;
        }
        auto& batch = batches[*leader];
        auto it = queued.find(group);
        if (it != queued.end() && !it->second.empty()) {
            batch.emplace_back(group,
                               std::vector<std::string>(it->second.begin(), it->second.end()));
        }
    }
    if (batches.empty()) {
        return;
    }
    const auto payload = _cache_hooks._advisory();
    const auto budget = _config._command_timeout.count() > 0
                            ? std::chrono::steady_clock::duration{_config._command_timeout}
                            : std::chrono::steady_clock::duration{std::chrono::seconds{5}};
    for (auto& [leader, groups] : batches) {
        auto endpoint = forward_endpoint(leader);
        if (!endpoint) {
            continue;
        }
        resp_writer w;
        std::vector<std::string> parts{w.bulk("KYTHIRA.ADVISE"), w.bulk(payload)};
        for (auto& [group, keys] : groups) {
            const auto blob = encode_redis_kv_command(redis_kv_touch_command{std::move(keys)});
            parts.push_back(w.bulk(std::to_string(group)));
            parts.push_back(
                w.bulk(std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size())));
        }
        try {
            auto sock = forward_socket(*endpoint, std::chrono::steady_clock::now() + budget);
            auto wire = w.array(parts);
            sock->write(boost::asio::buffer(wire));
            std::string buf;
            std::array<char, 512> chunk{};
            std::size_t len = 0;
            while ((len = resp_reply_length(buf)) == 0) {
                if (buf.size() > 64u * 1024u) {
                    throw std::runtime_error("oversized advisory reply");
                }
                auto n = sock->read_some(boost::asio::buffer(chunk));
                buf.append(chunk.data(), n);
            }
            if (buf.rfind("+OK", 0) != 0) {
                throw std::runtime_error("advisory refused: " + buf.substr(0, len));
            }
            return_forward_socket(*endpoint, std::move(sock));
            ++_stats._advisories_sent;
        } catch (const std::exception& e) {
            ++_stats._advisory_failures;
            _logger.log(log_level::debug, "redis gateway: advisory failed",
                        {{"endpoint", *endpoint}, {"error", e.what()}});
        }
    }
}

/// `KYTHIRA.ADVISE <payload> [<group> <touch>]...` from a peer gateway: the
/// reads it served for shards this node leads, and its cache hooks'
/// payload. Internal connections only.
template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::handle_advise(session& s, const resp_command& cmd)
    -> std::string {
    if (!s._internal) {
        ++_stats._authz_denials;
        return s._writer.error("NOPERM KYTHIRA.ADVISE is for peer gateways only");
    }
    if (cmd._argv.size() < 2 || cmd._argv.size() % 2 != 0) {
        return s._writer.error("ERR wrong number of arguments for 'kythira.advise' command");
    }
    ++_stats._advisories_received;
    for (std::size_t i = 2; i + 1 < cmd._argv.size(); i += 2) {
        group_id_type group{};
        try {
            std::size_t used = 0;
            group = static_cast<group_id_type>(std::stoull(cmd._argv[i], &used));
            if (used != cmd._argv[i].size()) {
                continue;
            }
        } catch (const std::exception&) {
            continue;
        }
        auto* node = _host.group_node(group);
        if (node == nullptr || !node->is_leader()) {
            continue;
        }
        const auto& blob = cmd._argv[i + 1];
        std::vector<std::string> keys;
        try {
            auto decoded = decode_redis_kv_command(std::as_bytes(std::span(blob)));
            if (auto* touch = std::get_if<redis_kv_touch_command>(&decoded)) {
                keys = std::move(touch->_keys);
            }
        } catch (const std::exception&) {
            continue;
        }
        for (const auto& key : keys) {
            // Only keys the shard still holds: a deleted key must not come
            // back into the LRU as a victim that frees nothing.
            auto entry = node->with_state_machine(
                [&key](auto& sm) { return redis_gateway_detail::kv_of(sm).lookup(key); });
            if (entry) {
                touch_lru(group, key);
            }
        }
    }
    if (_cache_hooks._on_advisory) {
        _cache_hooks._on_advisory(cmd._argv[1]);
    }
    return s._writer.simple_string("OK");
}

}  // namespace kythira
