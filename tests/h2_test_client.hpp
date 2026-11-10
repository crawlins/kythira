// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file h2_test_client.hpp
/// @brief A small blocking HTTP/2 client over nghttp2, for tests of the
///        servers that speak HTTP/2 (`raft/h2_server_session.hpp`).
///
/// It drives any Asio synchronous stream that is already connected (a TCP
/// socket for h2c with prior knowledge, or an SSL stream whose handshake
/// negotiated "h2"). Several requests can be open at once; `wait` runs the
/// connection until the named stream closes, collecting every stream's
/// answer as frames arrive. A data callback sees each body piece as it
/// comes and may stop waiting early, for streamed bodies.
///
/// Reads block, so give the socket a receive timeout (`set_receive_timeout`)
/// to keep a broken server from hanging a test.

#include <boost/asio.hpp>

#include <nghttp2/nghttp2.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::testing {

struct h2_reply {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    bool closed = false;
    std::uint32_t error_code = 0;  // the RST_STREAM code, 0 when it ended cleanly
};

/// Makes blocking reads on `fd` fail after `timeout`.
inline void set_receive_timeout(int fd, std::chrono::milliseconds timeout) {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

template<typename SyncStream> class h2_client {
public:
    using data_callback = std::function<bool(std::int32_t stream, std::string_view piece)>;

    explicit h2_client(SyncStream& stream) : _stream(stream) {
        nghttp2_session_callbacks* cb = nullptr;
        if (nghttp2_session_callbacks_new(&cb) != 0) {
            throw std::runtime_error("h2_client: no callbacks");
        }
        nghttp2_session_callbacks_set_on_header_callback(cb, &on_header);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, &on_data);
        nghttp2_session_callbacks_set_on_stream_close_callback(cb, &on_close);
        const int rv = nghttp2_session_client_new(&_ng, cb, this);
        nghttp2_session_callbacks_del(cb);
        if (rv != 0) {
            throw std::runtime_error("h2_client: no session");
        }
        (void)nghttp2_submit_settings(_ng, NGHTTP2_FLAG_NONE, nullptr, 0);
        flush();
    }

    h2_client(const h2_client&) = delete;
    auto operator=(const h2_client&) -> h2_client& = delete;
    ~h2_client() { nghttp2_session_del(_ng); }

    /// Opens a stream; its id.
    auto submit(std::string_view method, std::string_view path,
                const std::vector<std::pair<std::string, std::string>>& headers,
                std::string body = {}) -> std::int32_t {
        std::vector<std::pair<std::string, std::string>> all{{":method", std::string(method)},
                                                             {":path", std::string(path)},
                                                             {":scheme", "http"},
                                                             {":authority", "localhost"}};
        all.insert(all.end(), headers.begin(), headers.end());
        std::vector<nghttp2_nv> nva;
        for (auto& [k, v] : all) {
            nva.push_back(nghttp2_nv{.name = reinterpret_cast<std::uint8_t*>(k.data()),
                                     .value = reinterpret_cast<std::uint8_t*>(v.data()),
                                     .namelen = k.size(),
                                     .valuelen = v.size(),
                                     .flags = NGHTTP2_NV_FLAG_NONE});
        }
        auto owned = std::make_unique<std::string>(std::move(body));
        nghttp2_data_provider provider{};
        provider.source.ptr = owned.get();
        provider.read_callback = &read_body;
        const auto id = nghttp2_submit_request(_ng, nullptr, nva.data(), nva.size(),
                                               owned->empty() ? nullptr : &provider, this);
        if (id < 0) {
            throw std::runtime_error("h2_client: submit failed");
        }
        _bodies[id] = std::move(owned);
        _replies[id];
        flush();
        return id;
    }

    /// Resets `id` (CANCEL), as a client that stops reading a stream does.
    void reset(std::int32_t id) {
        (void)nghttp2_submit_rst_stream(_ng, NGHTTP2_FLAG_NONE, id, NGHTTP2_CANCEL);
        flush();
    }

    /// Runs the connection until `id` closes, or `on_data` returns false,
    /// or the connection fails (then the reply is returned as it stands).
    auto wait(std::int32_t id, data_callback on_data = {}) -> h2_reply {
        _on_data = std::move(on_data);
        _stop = false;
        while (!_replies[id].closed && !_stop && !_failed) {
            std::array<char, 16 * 1024> buf{};
            boost::system::error_code ec;
            const auto n = _stream.read_some(boost::asio::buffer(buf), ec);
            if (ec) {
                _failed = true;
                break;
            }
            if (nghttp2_session_mem_recv(_ng, reinterpret_cast<const std::uint8_t*>(buf.data()),
                                         n) < 0) {
                _failed = true;
                break;
            }
            flush();
        }
        _on_data = {};
        return _replies[id];
    }

    [[nodiscard]] auto failed() const -> bool { return _failed; }

private:
    SyncStream& _stream;
    nghttp2_session* _ng = nullptr;
    std::map<std::int32_t, h2_reply> _replies;
    std::map<std::int32_t, std::unique_ptr<std::string>> _bodies;
    data_callback _on_data;
    bool _stop = false;
    bool _failed = false;

    static auto self(void* p) -> h2_client& { return *static_cast<h2_client*>(p); }

    void flush() {
        for (;;) {
            const std::uint8_t* data = nullptr;
            const auto n = nghttp2_session_mem_send(_ng, &data);
            if (n <= 0) {
                return;
            }
            boost::system::error_code ec;
            boost::asio::write(_stream, boost::asio::buffer(data, static_cast<std::size_t>(n)), ec);
            if (ec) {
                _failed = true;
                return;
            }
        }
    }

    static auto read_body(nghttp2_session* /*ng*/, std::int32_t /*id*/, std::uint8_t* buf,
                          std::size_t length, std::uint32_t* flags, nghttp2_data_source* source,
                          void* /*user*/) -> ssize_t {
        auto& body = *static_cast<std::string*>(source->ptr);
        const auto n = std::min(length, body.size());
        std::copy_n(body.data(), n, reinterpret_cast<char*>(buf));
        body.erase(0, n);
        if (body.empty()) {
            *flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return static_cast<ssize_t>(n);
    }

    static auto on_header(nghttp2_session* /*ng*/, const nghttp2_frame* frame,
                          const std::uint8_t* name, std::size_t namelen, const std::uint8_t* value,
                          std::size_t valuelen, std::uint8_t /*flags*/, void* user) -> int {
        auto& r = self(user)._replies[frame->hd.stream_id];
        const std::string n(reinterpret_cast<const char*>(name), namelen);
        const std::string v(reinterpret_cast<const char*>(value), valuelen);
        if (n == ":status") {
            r.status = std::stoi(v);
        } else {
            r.headers[n] = v;
        }
        return 0;
    }

    static auto on_data(nghttp2_session* /*ng*/, std::uint8_t /*flags*/, std::int32_t id,
                        const std::uint8_t* data, std::size_t len, void* user) -> int {
        auto& me = self(user);
        const std::string_view piece(reinterpret_cast<const char*>(data), len);
        me._replies[id].body.append(piece);
        if (me._on_data && !me._on_data(id, piece)) {
            me._stop = true;
        }
        return 0;
    }

    static auto on_close(nghttp2_session* /*ng*/, std::int32_t id, std::uint32_t error_code,
                         void* user) -> int {
        auto& r = self(user)._replies[id];
        r.closed = true;
        r.error_code = error_code;
        return 0;
    }
};

}  // namespace kythira::testing
