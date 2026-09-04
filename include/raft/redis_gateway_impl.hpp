// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file redis_gateway_impl.hpp
/// @brief Definitions for redis_gateway.hpp.

#include <raft/redis_gateway.hpp>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <sstream>
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
        socket.set_option(tcp::no_delay(true), ec);
        auto remote = socket.remote_endpoint(ec);
        if (!ec) {
            _session._source = remote.address().to_string() + ":" + std::to_string(remote.port());
        } else {
            _session._source = "unknown";
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
        std::vector<resp_command> commands;
        try {
            commands = _session._parser.consume(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(_read_buffer.data()), n));
        } catch (const resp_protocol_error& e) {
            // Redis closes on a protocol error; so do we, after saying why.
            queue_reply(_session._writer.error(std::string("ERR Protocol error: ") + e.what()));
            _session._closing = true;
            _close_after_flush = true;
            flush();
            return;
        }
        if (!commands.empty()) {
            std::lock_guard<std::mutex> lock(_mutex);
            for (auto& cmd : commands) {
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
                    q._prebuilt = _session._writer.error(
                        "TRYAGAIN gateway is over its in-flight budget, retry");
                    ++_gw._stats._shed;
                } else {
                    q._cmd = std::move(cmd);
                }
                _queue.push_back(std::move(q));
            }
        }
        kick();
        do_read();
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
    /// empty, then hand the connection back to the I/O side.
    auto process() -> void {
        while (true) {
            queued_command q;
            bool resume = false;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (_queue.empty() || _session._closing || _closed) {
                    _busy = false;
                    resume = _paused;
                    _paused = false;
                    if (!resume) {
                        return;
                    }
                }
                if (!resume) {
                    q = std::move(_queue.front());
                    _queue.pop_front();
                    if (_paused && _queue.size() < _gw._config._max_inflight_per_connection) {
                        _paused = false;
                        resume = true;
                    }
                }
            }
            if (resume) {
                auto self = this->shared_from_this();
                boost::asio::post(_strand, [self] { self->do_read(); });
                std::lock_guard<std::mutex> lock(_mutex);
                if (!_busy) {
                    return;
                }
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
            if (!reply.empty()) {
                queue_reply(reply);
            }
            if (_session._closing) {
                _close_after_flush = true;
                flush();
            } else {
                flush();
            }
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
            if (ec) {
                self->do_close();
                return;
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
            _ssl->next_layer().shutdown(tcp::socket::shutdown_both, ec);
            _ssl->next_layer().close(ec);
        } else {
            _plain->shutdown(tcp::socket::shutdown_both, ec);
            _plain->close(ec);
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
    bool _busy = false;
    bool _paused = false;
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
      _resolver(std::move(resolver)) {}

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

}  // namespace redis_gateway_detail

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::start() -> void {
    if (_running.exchange(true)) {
        return;
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
    }
    _started_at = std::chrono::steady_clock::now();
    _io.restart();
    _work.emplace(boost::asio::make_work_guard(_io));
    _workers.emplace(std::max<std::size_t>(1, _config._worker_threads));

    using tcp = boost::asio::ip::tcp;
    auto bind = [&](const std::string& spec) -> tcp::acceptor {
        auto [host, port] = redis_gateway_detail::parse_listen(spec);
        tcp::acceptor acceptor(_io);
        tcp::endpoint endpoint(boost::asio::ip::make_address(host.empty() ? "0.0.0.0" : host),
                               port);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen(boost::asio::socket_base::max_listen_connections);
        return acceptor;
    };

    try {
        if (!_config._listen.empty()) {
            _acceptor.emplace(bind(_config._listen));
            _port = _acceptor->local_endpoint().port();
            // Requirement 12.6: a plaintext listener is announced once, loudly.
            _logger.log(log_level::warning,
                        "redis gateway: PLAINTEXT listener; AUTH secrets and values cross the "
                        "network unencrypted",
                        {{"listen", _config._listen}, {"port", std::to_string(_port)}});
        }
        if (!_config._tls_listen.empty()) {
            _ssl_ctx = std::make_shared<boost::asio::ssl::context>(build_ssl_context());
            _tls_acceptor.emplace(bind(_config._tls_listen));
            _tls_port = _tls_acceptor->local_endpoint().port();
            _logger.log(log_level::info, "redis gateway: TLS listener",
                        {{"listen", _config._tls_listen},
                         {"port", std::to_string(_tls_port)},
                         {"client_cert", _config._require_client_cert ? "required" : "optional"}});
        }
    } catch (...) {
        _work.reset();
        _workers->join();
        _workers.reset();
        _acceptor.reset();
        _tls_acceptor.reset();
        _running = false;
        throw;
    }

    if (_acceptor) {
        accept_loop(*_acceptor, false);
    }
    if (_tls_acceptor) {
        accept_loop(*_tls_acceptor, true);
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
    boost::system::error_code ec;
    if (_acceptor) {
        _acceptor->close(ec);
    }
    if (_tls_acceptor) {
        _tls_acceptor->close(ec);
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
    _acceptor.reset();
    _tls_acceptor.reset();
    {
        std::lock_guard<std::mutex> lock(_connections_mutex);
        _connections.clear();
    }
    {
        std::lock_guard<std::mutex> lock(_forward_mutex);
        _forward_pools.clear();
    }
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
auto redis_gateway<Host, Logger, Metrics>::accept_loop(boost::asio::ip::tcp::acceptor& acceptor,
                                                       bool tls) -> void {
    acceptor.async_accept([this, &acceptor, tls](const boost::system::error_code& ec,
                                                 boost::asio::ip::tcp::socket socket) {
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
            socket.non_blocking(true, ignored);
            static constexpr std::string_view reply = "-ERR max number of clients reached\r\n";
            socket.write_some(boost::asio::buffer(reply.data(), reply.size()), ignored);
            socket.close(ignored);
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
auto redis_gateway<Host, Logger, Metrics>::is_expired(const redis_kv_value_entry& e) const -> bool {
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
auto redis_gateway<Host, Logger, Metrics>::auth_rate_limited(const std::string& source) -> bool {
    std::lock_guard<std::mutex> lock(_auth_mutex);
    auto it = _auth_failures.find(source);
    if (it == _auth_failures.end()) {
        return false;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - it->second._window_start > _config._auth_failure_window) {
        _auth_failures.erase(it);
        return false;
    }
    return it->second._count >= _config._auth_failure_limit;
}

template<typename Host, typename Logger, typename Metrics>
auto redis_gateway<Host, Logger, Metrics>::note_auth_failure(const std::string& source) -> void {
    std::lock_guard<std::mutex> lock(_auth_mutex);
    auto now = std::chrono::steady_clock::now();
    auto& f = _auth_failures[source];
    if (f._count == 0 || now - f._window_start > _config._auth_failure_window) {
        f._window_start = now;
        f._count = 0;
    }
    ++f._count;
    // Keep the table bounded: an attacker cycling source ports must not grow
    // it without limit.
    if (_auth_failures.size() > 65536) {
        _auth_failures.clear();
    }
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
    try {
        auto future = _host.submit_command(key, command, _config._command_timeout);
        if (!future.wait(_config._command_timeout)) {
            error_reply = s._writer.error("ERR timeout submitting to shard, retry");
            return false;
        }
        (void)std::move(future).get();
        return true;
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
    r._entry = node->with_state_machine([&key](auto& sm) { return sm.lookup(key); });
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
auto redis_gateway<Host, Logger, Metrics>::forward_socket(const std::string& endpoint)
    -> std::unique_ptr<boost::asio::ip::tcp::socket> {
    using tcp = boost::asio::ip::tcp;
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
            return sock;
        }
    }
    auto [host, port] = redis_gateway_detail::parse_listen(endpoint);
    tcp::resolver resolver(_io);
    auto endpoints = resolver.resolve(host, std::to_string(port));
    auto sock = std::make_unique<tcp::socket>(_io);
    boost::asio::connect(*sock, endpoints);
    sock->set_option(tcp::no_delay(true));
    // Deadline propagation on a synchronous socket: the kernel timeouts are
    // the command timeout, so a hung peer costs one command's budget.
    timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(_config._command_timeout.count() / 1000);
    tv.tv_usec =
        static_cast<decltype(tv.tv_usec)>((_config._command_timeout.count() % 1000) * 1000);
    ::setsockopt(sock->native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock->native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Authenticate as the internal identity; the peer never forwards a
    // command that arrives on this connection (one hop, Requirement 9.4).
    resp_writer w;
    auto auth =
        w.array({w.bulk("AUTH"), w.bulk(_config._internal_user), w.bulk(_config._internal_secret)});
    boost::asio::write(*sock, boost::asio::buffer(auth));
    std::string buf;
    std::array<char, 512> chunk{};
    while (resp_reply_length(buf) == 0) {
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
    const std::string& endpoint, std::unique_ptr<boost::asio::ip::tcp::socket> sock) -> void {
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
    if (!_config._forwarding || s._internal || _config._internal_secret.empty() ||
        !_running.load()) {
        return std::nullopt;
    }
    auto endpoint = forward_endpoint(to);
    if (!endpoint) {
        return std::nullopt;
    }
    ++_stats._forwards;
    emit("redis.forwards", resp_to_upper(cmd._argv[0]));
    try {
        auto sock = forward_socket(*endpoint);
        resp_writer w;
        std::vector<std::string> parts;
        parts.reserve(cmd._argv.size());
        for (const auto& a : cmd._argv) {
            parts.push_back(w.bulk(a));
        }
        auto wire = w.array(parts);
        boost::asio::write(*sock, boost::asio::buffer(wire));
        std::string buf;
        std::array<char, 16 * 1024> chunk{};
        std::size_t len = 0;
        while ((len = resp_reply_length(buf)) == 0) {
            auto n = sock->read_some(boost::asio::buffer(chunk));
            buf.append(chunk.data(), n);
        }
        return_forward_socket(*endpoint, std::move(sock));
        buf.resize(len);
        // The internal hop speaks RESP2; a RESP3 client expects `_` for null.
        if (s._writer.version() >= 3 && buf == "$-1\r\n") {
            buf = "_\r\n";
        }
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
    if (auth_rate_limited(s._source)) {
        ++_stats._auth_failures;
        emit("redis.auth.failures", "AUTH");
        audit(s, "AUTH", "rate limited");
        return s._writer.error(
            "ERR too many authentication failures from this address, retry later");
    }
    auto id = _acl.authenticate(user, secret);
    if (!id) {
        ++_stats._auth_failures;
        note_auth_failure(s._source);
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
        if (reply.rfind("-", 0) == 0) {
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
    ++_stats._hits;
    emit("redis.hits", "GET");
    if (auto desc = _host.resolve(key)) {
        touch_lru(desc->group_id(), key);
    }
    return s._writer.bulk(r._entry->_value);
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
    return s._writer.integer(present ? static_cast<std::int64_t>(r._entry->_value.size()) : 0);
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
    const auto len = static_cast<std::int64_t>(r._entry->_value.size());
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
    auto span =
        std::span<const std::byte>(r._entry->_value)
            .subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start + 1));
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

    if (_config._immutable_values) {
        // sccache keys are content hashes, so a rewrite is either the same
        // bytes (sccache rewriting its `.sccache_check` probe) or a bug
        // somewhere. The first is a free no-op; the second is refused.
        auto existing = node->with_state_machine([&key](auto& sm) { return sm.lookup(key); });
        if (existing && !is_expired(*existing)) {
            bool same = existing->_value.size() == value.size() &&
                        (value.empty() ||
                         std::memcmp(existing->_value.data(), value.data(), value.size()) == 0);
            if (same) {
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
    auto existing = node->with_state_machine([&key](auto& sm) { return sm.lookup(key); });
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
            keys += node->with_state_machine([](auto& sm) { return sm.approximate_key_count(); });
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
                return std::tuple{sm.approximate_key_count(), sm.expiring_key_count(),
                                  sm.approximate_size_bytes()};
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
        << "kythira_authz_denials:" << _stats._authz_denials.load() << "\r\n"
        << "kythira_forwards:" << _stats._forwards.load() << "\r\n"
        << "kythira_forward_failures:" << _stats._forward_failures.load() << "\r\n"
        << "kythira_shed:" << _stats._shed.load() << "\r\n";
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
        auto expired = node->with_state_machine(
            [&](auto& sm) { return sm.collect_expired(now, _config._sweep_batch); });
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
            return std::pair{sm.approximate_size_bytes(), sm.approximate_key_count()};
        });
        if (bytes <= _config._max_shard_bytes || key_count == 0) {
            continue;
        }
        ++_stats._over_budget_ticks;
        emit("redis.shard_over_budget", "evict");
        _logger.log(log_level::warning, "redis gateway: shard over budget, evicting",
                    {{"group", std::to_string(group)},
                     {"bytes", std::to_string(bytes)},
                     {"budget", std::to_string(_config._max_shard_bytes)}});

        std::vector<std::string> victims;
        {
            std::lock_guard<std::mutex> lock(_lru_mutex);
            auto& lru = _lru[group];
            if (lru._order.empty()) {
                // Fresh leadership: seed from the shard itself so eviction can
                // proceed. Key order is arbitrary as an LRU, which is fine —
                // any order beats refusing to evict.
                node->with_state_machine([&](auto& sm) {
                    sm.for_each_key([&](const std::string& key, const redis_kv_value_entry&) {
                        lru._order.push_back(key);
                        lru._index.emplace(key, std::prev(lru._order.end()));
                    });
                });
            }
            std::size_t freed = 0;
            while (!lru._order.empty() && victims.size() < _config._sweep_batch &&
                   bytes - freed > _config._max_shard_bytes) {
                const auto& key = lru._order.back();
                auto entry = node->with_state_machine([&key](auto& sm) { return sm.lookup(key); });
                freed += key.size() + (entry ? entry->_value.size() : 0);
                victims.push_back(key);
                lru._index.erase(key);
                lru._order.pop_back();
            }
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
    return proposals;
}

}  // namespace kythira
