// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file h2_server_session.hpp
/// @brief One HTTP/2 server connection over nghttp2, for `beast_https_server.hpp`
///        (Kinesis `SubscribeToShard`, `.kiro/specs/kinesis-compatible-streams/`
///        task 13).
///
/// The server hands a connection here once it knows the client speaks
/// HTTP/2: ALPN chose "h2" during the TLS handshake, or a plaintext client
/// opened with the prior-knowledge preface (h2c). The session runs on the
/// connection's own thread and `io_context`, like the HTTP/1.1 path, and
/// returns when the connection ends.
///
/// Each request is collected whole (headers, then a body up to
/// `max_body_bytes`) and turned into the same Beast request the HTTP/1.1
/// path builds, so handlers do not know which protocol carried it:
///
///  - An ordinary request runs its handler on the connection thread, and
///    the response goes out as HEADERS and DATA. Other streams on the
///    connection wait for that handler.
///  - A request the stream handler takes gets its head at once. A worker
///    thread then pulls the body from `next` and posts each piece to the
///    connection thread, which sends it as DATA as the client's flow-control
///    window allows. A client that resets the stream, or a connection that
///    ends, cancels it. The worker stops pulling while 4 MiB is queued
///    unsent, so a slow reader holds back the producer.
///
/// Reads and writes run concurrently, each with at most one operation
/// pending. Instead of Beast's per-operation deadlines a once-a-second
/// timer ends the connection when it has been idle (no streams open, nothing
/// received) for `idle_timeout`, when one write has been pending for
/// `io_timeout`, or when the server stops.
///
/// A header block over `max_header_bytes` resets its stream; a body over
/// `max_body_bytes` is answered 413.
///
/// Only built where nghttp2 is (KYTHIRA_HAS_NGHTTP2).

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::https::h2 {

using request = boost::beast::http::request<boost::beast::http::string_body>;
using response = boost::beast::http::response<boost::beast::http::string_body>;

/// The client connection preface a prior-knowledge (h2c) client opens with.
inline constexpr std::string_view k_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/// The ALPN protocol list the server offers, in wire form: h2 first.
inline constexpr std::string_view k_alpn_protocols = "\x02h2\x08http/1.1";

struct limits {
    std::uint32_t max_header_bytes = 16 * 1024;
    std::uint64_t max_body_bytes = std::uint64_t{1024} * 1024;
    std::chrono::seconds idle_timeout{60};
    std::chrono::seconds io_timeout{60};
    std::uint32_t max_concurrent_streams = 100;
};

/// `Hooks` gives the session the server's handlers:
///  - `handle(const request&) -> response`;
///  - `stream(const request&) -> std::optional<S>`, where `S` has `head`
///    (a `response`), `next` (`bool(std::string&)`) and `cancel`
///    (`void()`, may be empty), as `https::streaming_response` does;
///  - `running() -> bool`.
template<typename Stream, typename Hooks> class session {
public:
    session(boost::asio::io_context& ioc, Stream& stream, Hooks& hooks, limits l)
        : _ioc(ioc), _stream(stream), _hooks(hooks), _limits(l), _timer(ioc) {}

    session(const session&) = delete;
    session(session&&) = delete;
    auto operator=(const session&) -> session& = delete;
    auto operator=(session&&) -> session& = delete;

    ~session() {
        if (_ng != nullptr) {
            nghttp2_session_del(_ng);
        }
    }

    /// Serves the connection until it ends. `preread` holds bytes already
    /// read from it (the h2c preface, when the server peeked at it).
    void run(std::string_view preread = {}) {
        if (!open()) {
            return;
        }
        _last_activity = clock::now();
        if (!preread.empty() && !receive(preread.data(), preread.size())) {
            terminate();
        }
        pump();
        start_read();
        arm_timer();
        _ioc.restart();
        while ((!_closed || _reading || _writing || _timing) && _ioc.run_one() != 0) {
        }
        // Every worker sees its stop flag; its pieces still in the queue
        // are dropped (`deliver` checks `_closed`).
        terminate();
        for (auto& [id, s] : _streams) {
            stop_stream(*s);
        }
        for (auto& t : _workers) {
            if (t.joinable()) {
                t.join();
            }
        }
        _ioc.restart();
        (void)_ioc.poll();
        _alive.reset();
    }

private:
    using clock = std::chrono::steady_clock;
    static constexpr std::size_t k_queue_limit = std::size_t{4} * 1024 * 1024;
    static constexpr std::size_t k_write_batch = std::size_t{64} * 1024;

    struct stream_state {
        std::int32_t _id = 0;
        // The request, as it arrives.
        std::string _method;
        std::string _path;
        std::string _authority;
        std::vector<std::pair<std::string, std::string>> _headers;
        std::string _body;
        std::size_t _header_bytes = 0;
        bool _too_large = false;
        bool _dispatched = false;
        // The response body still to send.
        std::string _pending;
        std::size_t _offset = 0;
        bool _finished = false;
        // A streamed body's producer.
        std::shared_ptr<std::atomic<bool>> _stop;
        std::shared_ptr<std::atomic<std::size_t>> _queued;
        std::function<void()> _cancel;
    };

    boost::asio::io_context& _ioc;
    Stream& _stream;
    Hooks& _hooks;
    limits _limits;
    nghttp2_session* _ng = nullptr;
    std::map<std::int32_t, std::unique_ptr<stream_state>> _streams;
    std::vector<std::thread> _workers;
    std::shared_ptr<int> _alive = std::make_shared<int>(0);
    std::array<char, 16 * 1024> _in{};
    std::string _out;
    boost::asio::steady_timer _timer;
    clock::time_point _last_activity;
    clock::time_point _write_started;
    bool _reading = false;
    bool _writing = false;
    bool _timing = false;
    bool _closed = false;

    // ---- setup ---------------------------------------------------------------

    auto open() -> bool {
        nghttp2_session_callbacks* cb = nullptr;
        if (nghttp2_session_callbacks_new(&cb) != 0) {
            return false;
        }
        nghttp2_session_callbacks_set_on_begin_headers_callback(cb, &on_begin_headers);
        nghttp2_session_callbacks_set_on_header_callback(cb, &on_header);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, &on_data_chunk);
        nghttp2_session_callbacks_set_on_frame_recv_callback(cb, &on_frame_recv);
        nghttp2_session_callbacks_set_on_stream_close_callback(cb, &on_stream_close);
        const int rv = nghttp2_session_server_new(&_ng, cb, this);
        nghttp2_session_callbacks_del(cb);
        if (rv != 0) {
            _ng = nullptr;
            return false;
        }
        const std::array<nghttp2_settings_entry, 1> settings{
            {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, _limits.max_concurrent_streams}}};
        return nghttp2_submit_settings(_ng, NGHTTP2_FLAG_NONE, settings.data(), settings.size()) ==
               0;
    }

    static auto self(void* user_data) -> session& { return *static_cast<session*>(user_data); }

    auto find(std::int32_t id) -> stream_state* {
        const auto it = _streams.find(id);
        return it == _streams.end() ? nullptr : it->second.get();
    }

    // ---- nghttp2 callbacks ---------------------------------------------------

    static auto on_begin_headers(nghttp2_session* /*ng*/, const nghttp2_frame* frame,
                                 void* user_data) -> int {
        if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
            auto s = std::make_unique<stream_state>();
            s->_id = frame->hd.stream_id;
            self(user_data)._streams.insert_or_assign(frame->hd.stream_id, std::move(s));
        }
        return 0;
    }

    static auto on_header(nghttp2_session* /*ng*/, const nghttp2_frame* frame,
                          const std::uint8_t* name, std::size_t namelen, const std::uint8_t* value,
                          std::size_t valuelen, std::uint8_t /*flags*/, void* user_data) -> int {
        auto& me = self(user_data);
        auto* s = me.find(frame->hd.stream_id);
        if (s == nullptr || s->_dispatched) {
            return 0;  // trailers or an unknown stream: ignored
        }
        s->_header_bytes += namelen + valuelen + 32;
        if (s->_header_bytes > me._limits.max_header_bytes) {
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;  // resets the stream
        }
        const std::string_view n(reinterpret_cast<const char*>(name), namelen);
        std::string v(reinterpret_cast<const char*>(value), valuelen);
        if (n == ":method") {
            s->_method = std::move(v);
        } else if (n == ":path") {
            s->_path = std::move(v);
        } else if (n == ":authority") {
            s->_authority = std::move(v);
        } else if (!n.starts_with(':')) {
            s->_headers.emplace_back(std::string(n), std::move(v));
        }
        return 0;
    }

    static auto on_data_chunk(nghttp2_session* /*ng*/, std::uint8_t /*flags*/,
                              std::int32_t stream_id, const std::uint8_t* data, std::size_t len,
                              void* user_data) -> int {
        auto& me = self(user_data);
        auto* s = me.find(stream_id);
        if (s == nullptr || s->_too_large) {
            return 0;
        }
        if (s->_body.size() + len > me._limits.max_body_bytes) {
            s->_too_large = true;
            s->_body.clear();
            return 0;
        }
        s->_body.append(reinterpret_cast<const char*>(data), len);
        return 0;
    }

    static auto on_frame_recv(nghttp2_session* /*ng*/, const nghttp2_frame* frame, void* user_data)
        -> int {
        auto& me = self(user_data);
        if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
            (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
            if (auto* s = me.find(frame->hd.stream_id); s != nullptr && !s->_dispatched) {
                s->_dispatched = true;
                me.dispatch(*s);
            }
        }
        return 0;
    }

    static auto on_stream_close(nghttp2_session* /*ng*/, std::int32_t stream_id,
                                std::uint32_t /*error_code*/, void* user_data) -> int {
        auto& me = self(user_data);
        if (const auto it = me._streams.find(stream_id); it != me._streams.end()) {
            me.stop_stream(*it->second);
            me._streams.erase(it);
        }
        return 0;
    }

    // ---- requests ------------------------------------------------------------

    void stop_stream(stream_state& s) {
        if (s._stop && !s._stop->exchange(true) && s._cancel) {
            try {
                s._cancel();
            } catch (...) {  // NOLINT(bugprone-empty-catch): a cancel that throws changes nothing
            }
        }
    }

    [[nodiscard]] auto to_request(const stream_state& s) const -> request {
        namespace http = boost::beast::http;
        request req;
        req.method_string(s._method);
        req.target(s._path);
        req.version(11);
        if (!s._authority.empty()) {
            req.set(http::field::host, s._authority);
        }
        for (const auto& [k, v] : s._headers) {
            if (k == "cookie" && req.count(k) != 0) {
                // HTTP/2 may split cookies across fields; HTTP/1.1 joins them.
                req.set(k, std::string(req[k]) + "; " + v);
            } else {
                req.insert(k, v);
            }
        }
        req.body() = s._body;
        req.keep_alive(true);
        if (!s._body.empty() || s._method == "POST" || s._method == "PUT") {
            req.content_length(s._body.size());
        }
        return req;
    }

    static auto plain(unsigned status, std::string body) -> response {
        response r{static_cast<boost::beast::http::status>(status), 11};
        r.set(boost::beast::http::field::content_type, "text/plain");
        r.body() = std::move(body);
        return r;
    }

    void dispatch(stream_state& s) {
        if (s._too_large) {
            respond(s, plain(413, "request body too large\n"), false);
            return;
        }
        const auto req = to_request(s);
        try {
            if (auto streamed = _hooks.stream(req)) {
                start_streaming(s, std::move(*streamed));
                return;
            }
        } catch (const std::exception&) {
            respond(s, plain(500, "internal error\n"), false);
            return;
        }
        response res;
        try {
            res = _hooks.handle(req);
        } catch (const std::exception&) {
            res = plain(500, "internal error\n");
        }
        respond(s, std::move(res), false);
    }

    template<typename Streamed> void start_streaming(stream_state& s, Streamed streamed) {
        s._stop = std::make_shared<std::atomic<bool>>(false);
        s._queued = std::make_shared<std::atomic<std::size_t>>(0);
        s._cancel = std::move(streamed.cancel);
        streamed.head.body().clear();
        if (!respond(s, std::move(streamed.head), true)) {
            stop_stream(s);
            return;
        }
        _workers.emplace_back([this, id = s._id, next = std::move(streamed.next), stop = s._stop,
                               queued = s._queued, alive = std::weak_ptr<int>(_alive)] {
            std::string piece;
            for (;;) {
                while (queued->load() > k_queue_limit && !stop->load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (stop->load()) {
                    return;
                }
                piece.clear();
                bool more = false;
                try {
                    more = next(piece);
                } catch (const std::exception&) {
                    more = false;
                }
                if (!piece.empty() || !more) {
                    queued->fetch_add(piece.size());
                    boost::asio::post(_ioc, [this, id, alive, more, chunk = std::move(piece)] {
                        if (alive.lock() != nullptr) {
                            deliver(id, chunk, more);
                        }
                    });
                }
                if (!more) {
                    return;
                }
            }
        });
    }

    // A streamed piece, on the connection thread.
    void deliver(std::int32_t id, const std::string& chunk, bool more) {
        if (_closed) {
            return;
        }
        auto* s = find(id);
        if (s == nullptr) {
            return;  // the client reset the stream
        }
        if (s->_offset > 0 && s->_offset == s->_pending.size()) {
            s->_pending.clear();
            s->_offset = 0;
        }
        s->_pending += chunk;
        s->_finished = !more;
        if (!more) {
            s->_stop->store(true);  // ended by its producer: nothing to cancel
        }
        (void)nghttp2_session_resume_data(_ng, id);
        pump();
    }

    static auto read_body(nghttp2_session* /*ng*/, std::int32_t stream_id, std::uint8_t* buf,
                          std::size_t length, std::uint32_t* data_flags,
                          nghttp2_data_source* /*source*/, void* user_data) -> ssize_t {
        auto& me = self(user_data);
        auto* s = me.find(stream_id);
        if (s == nullptr) {
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        const auto n = std::min(length, s->_pending.size() - s->_offset);
        if (n > 0) {
            std::copy_n(s->_pending.data() + s->_offset, n, reinterpret_cast<char*>(buf));
            s->_offset += n;
            if (s->_queued) {
                s->_queued->fetch_sub(std::min(n, s->_queued->load()));
            }
        }
        if (s->_offset == s->_pending.size()) {
            s->_pending.clear();
            s->_offset = 0;
            if (s->_finished) {
                *data_flags |= NGHTTP2_DATA_FLAG_EOF;
                return static_cast<ssize_t>(n);
            }
            if (n == 0) {
                return NGHTTP2_ERR_DEFERRED;
            }
        }
        return static_cast<ssize_t>(n);
    }

    // Submits the head, with the body to follow from `_pending` (all of it
    // now, or as a stream delivers it).
    auto respond(stream_state& s, response res, bool streaming) -> bool {
        std::vector<std::string> names;
        std::vector<std::string> values;
        names.emplace_back(":status");
        values.emplace_back(std::to_string(res.result_int()));
        for (const auto& f : res) {
            std::string name(f.name_string());
            std::ranges::transform(name, name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (name == "connection" || name == "keep-alive" || name == "transfer-encoding" ||
                name == "upgrade" || name == "proxy-connection" ||
                (streaming && name == "content-length")) {
                continue;  // connection-specific: not in HTTP/2
            }
            names.push_back(std::move(name));
            values.emplace_back(f.value());
        }
        if (!streaming && res.find(boost::beast::http::field::content_length) == res.end()) {
            names.emplace_back("content-length");
            values.emplace_back(std::to_string(res.body().size()));
        }
        std::vector<nghttp2_nv> nva;
        nva.reserve(names.size());
        for (std::size_t i = 0; i < names.size(); ++i) {
            nva.push_back(nghttp2_nv{.name = reinterpret_cast<std::uint8_t*>(names[i].data()),
                                     .value = reinterpret_cast<std::uint8_t*>(values[i].data()),
                                     .namelen = names[i].size(),
                                     .valuelen = values[i].size(),
                                     .flags = NGHTTP2_NV_FLAG_NONE});
        }
        if (!streaming) {
            s._pending = std::move(res.body());
            s._offset = 0;
            s._finished = true;
        }
        nghttp2_data_provider provider{};
        provider.read_callback = &read_body;
        return nghttp2_submit_response(_ng, s._id, nva.data(), nva.size(), &provider) == 0;
    }

    // ---- I/O -----------------------------------------------------------------

    auto receive(const char* data, std::size_t n) -> bool {
        const auto rv =
            nghttp2_session_mem_recv(_ng, reinterpret_cast<const std::uint8_t*>(data), n);
        return rv >= 0 && static_cast<std::size_t>(rv) == n;
    }

    void start_read() {
        if (_closed || _reading) {
            return;
        }
        _reading = true;
        _stream.async_read_some(boost::asio::buffer(_in),
                                [this](boost::beast::error_code ec, std::size_t n) {
                                    _reading = false;
                                    if (_closed) {
                                        return;
                                    }
                                    if (ec || !receive(_in.data(), n)) {
                                        terminate();
                                        return;
                                    }
                                    _last_activity = clock::now();
                                    pump();
                                    start_read();
                                });
    }

    void pump() {
        if (_closed || _writing) {
            return;
        }
        _out.clear();
        while (_out.size() < k_write_batch) {
            const std::uint8_t* data = nullptr;
            const auto n = nghttp2_session_mem_send(_ng, &data);
            if (n < 0) {
                terminate();
                return;
            }
            if (n == 0) {
                break;
            }
            _out.append(reinterpret_cast<const char*>(data), static_cast<std::size_t>(n));
        }
        if (_out.empty()) {
            if (nghttp2_session_want_read(_ng) == 0 && nghttp2_session_want_write(_ng) == 0) {
                terminate();  // GOAWAY exchanged and every stream done
            }
            return;
        }
        _writing = true;
        _write_started = clock::now();
        boost::asio::async_write(_stream, boost::asio::buffer(_out),
                                 [this](boost::beast::error_code ec, std::size_t /*n*/) {
                                     _writing = false;
                                     if (_closed) {
                                         return;
                                     }
                                     if (ec) {
                                         terminate();
                                         return;
                                     }
                                     pump();
                                 });
    }

    void arm_timer() {
        if (_closed) {
            return;
        }
        _timing = true;
        _timer.expires_after(std::chrono::seconds(1));
        _timer.async_wait([this](boost::beast::error_code ec) {
            _timing = false;
            if (ec || _closed) {
                return;
            }
            const auto now = clock::now();
            if (!_hooks.running() || (_writing && now - _write_started > _limits.io_timeout) ||
                (_streams.empty() && !_writing && now - _last_activity > _limits.idle_timeout)) {
                terminate();
                return;
            }
            arm_timer();
        });
    }

    void terminate() {
        if (_closed) {
            return;
        }
        _closed = true;
        boost::system::error_code ignored;
        (void)boost::beast::get_lowest_layer(_stream).socket().cancel(ignored);
        _timer.cancel();
        for (auto& [id, s] : _streams) {
            stop_stream(*s);
        }
    }
};

}  // namespace kythira::https::h2
