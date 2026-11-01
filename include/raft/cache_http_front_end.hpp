// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file cache_http_front_end.hpp
/// @brief HTTP/1.1 and HTTPS object API over the disk-backed cache, for
///        vcpkg's `http` binary source and sccache's WebDAV backend
///        (.kiro/specs/disk-backed-build-cache/ design Component 6, task 6,
///        Requirements 7 and 8).
///
/// | Request                 | Role        | Answer                                   |
/// |-------------------------|-------------|------------------------------------------|
/// | `GET /<key>`            | read_only   | 200 streamed, 206 for one `Range`, 404   |
/// | `HEAD /<key>`           | read_only   | 200 with length and `ETag`, or 404       |
/// | `PUT /<key>`            | read_write  | 201 on commit; 413 over max; 507 low disk|
/// | `DELETE /<key>`         | read_write  | 204, or 404                              |
/// | `PROPFIND /<dir>/`      | read_write  | 207 for any collection; 207/404 for a key|
/// | `MKCOL /<dir>/`         | read_write  | 201, a no-op                             |
/// | `GET /_chunk/<sha>`     | internal    | the chunk's bytes                        |
/// | `GET /_metrics`         | admin       | counters as text                         |
///
/// **Threads.** One thread per connection, each with its own `io_context`
/// that runs only that connection's operations. Every socket operation is
/// asynchronous with a Beast deadline and is driven to completion before the
/// thread moves on, so the handler reads like blocking code while every
/// read and write still has a timeout. A Raft proposal blocks only its own
/// connection's thread. The connection gate from `http-server-request-limits`
/// caps the threads.
///
/// **PUT.** The body is read into a chunk-sized buffer. Each full buffer is
/// hashed and proposed as `put_chunk` while the next one fills, so at most
/// two chunks of one request are in memory. After the body, `commit` is
/// proposed and the answer waits for it. A client that goes away mid-body
/// never reaches `commit`, so the old value stays (Requirement 7.5) and the
/// chunks age out as orphans.
///
/// **GET.** The manifest handle is taken under the node lock and every chunk
/// is read after it is released, hash-checked as it is read. The first chunk
/// is read before the headers go out, so a bad first chunk is still a clean
/// miss. A bad chunk after the headers aborts the connection (Requirement
/// 10.2), clears the manifest's `has_all` bit and reports the chunk through
/// `on_repair` for the fetcher.
///
/// **Forwarding.** A write for a shard another node leads, and a read for a
/// key whose chunks are not all here, are relayed whole to the leader's front
/// end, body streamed in both directions, with the client's own
/// `Authorization`, so the leader authorizes the same identity. A relayed
/// request carries `X-Kythira-Forwarded` and is never relayed again. The
/// relay uses TLS when `_forward_tls` is set, and plaintext only with
/// `_allow_plaintext_forwarding`, the rule the RESP forwarding link follows.

#include <raft/asio_listeners.hpp>
#include <raft/cache_chunk_store.hpp>
#include <raft/cache_http_auth.hpp>
#include <raft/cache_kv_state_machine.hpp>
#include <raft/http_connection_gate.hpp>
#include <raft/logger.hpp>
#include <raft/multi_raft.hpp>
#include <raft/redis_acl.hpp>
#include <raft/redis_auth_limiter.hpp>
#include <raft/redis_kv_commands.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira {

struct cache_http_config {
    /// `host:port` for plaintext HTTP; empty is off (Requirement 7.1).
    std::string _listen;
    /// `host:port` for HTTPS; empty is off.
    std::string _tls_listen;
    /// Plaintext carries credentials in the clear: refused without this
    /// (Requirement 8.5, `KYTHIRA_CACHE_ALLOW_PLAINTEXT_HTTP`).
    bool _allow_plaintext = false;
    std::string _tls_cert_path;
    std::string _tls_key_path;

    /// `KYTHIRA_CACHE_CHUNK_BYTES`: the PUT path's chunk size, at most the
    /// store's 16 MiB chunk limit.
    std::size_t _chunk_bytes = std::size_t{4} * 1024 * 1024;
    /// `KYTHIRA_CACHE_MAX_VALUE_BYTES`.
    std::uint64_t _max_value_bytes = std::uint64_t{2} * 1024 * 1024 * 1024;
    /// Below either floor a PUT is refused with 507 (Requirement 6.6).
    std::uint64_t _min_free_bytes = std::uint64_t{1} * 1024 * 1024 * 1024;
    std::uint64_t _min_free_inodes = 10000;
    std::size_t _max_key_bytes = 1023;

    // http-server-request-limits (Requirement 7.7).
    std::size_t _max_connections = 256;
    std::uint32_t _max_header_bytes = 16 * 1024;
    std::chrono::seconds _header_timeout{10};
    std::chrono::seconds _idle_timeout{60};
    /// Each body read or response write must make progress within this.
    std::chrono::seconds _io_timeout{60};
    std::chrono::seconds _handshake_timeout{10};
    std::chrono::milliseconds _command_timeout{10000};

    /// `KYTHIRA_CACHE_AUTH_CACHE_SECONDS` (Requirement 8.4).
    std::chrono::seconds _auth_cache_ttl{300};
    std::size_t _auth_cache_entries = 1024;
    std::size_t _auth_failure_limit = 10;
    std::chrono::seconds _auth_failure_window{60};
    std::size_t _max_concurrent_auth = 4;

    /// The gateway's internal secret: the only credential `/_chunk/` takes.
    std::string _internal_secret;

    bool _forwarding = true;
    bool _forward_tls = false;
    bool _allow_plaintext_forwarding = false;
    std::string _forward_tls_ca_path;
};

struct cache_http_stats {
    std::atomic<std::uint64_t> _requests{0};
    std::atomic<std::uint64_t> _hits{0};
    std::atomic<std::uint64_t> _misses{0};
    std::atomic<std::uint64_t> _puts{0};
    std::atomic<std::uint64_t> _put_bytes{0};
    std::atomic<std::uint64_t> _get_bytes{0};
    std::atomic<std::uint64_t> _deletes{0};
    std::atomic<std::uint64_t> _forwards{0};
    std::atomic<std::uint64_t> _forward_failures{0};
    std::atomic<std::uint64_t> _auth_failures{0};
    std::atomic<std::uint64_t> _forbidden{0};
    std::atomic<std::uint64_t> _too_large{0};
    std::atomic<std::uint64_t> _insufficient_storage{0};
    std::atomic<std::uint64_t> _aborted_puts{0};
    std::atomic<std::uint64_t> _aborted_gets{0};
    std::atomic<std::uint64_t> _chunk_serves{0};
    std::atomic<std::uint64_t> _connections_accepted{0};
    std::atomic<std::uint64_t> _connections_rejected{0};
    std::atomic<std::uint64_t> _connections_current{0};
};

namespace cache_http_detail {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

inline auto parse_listen(const std::string& spec) -> std::pair<std::string, std::uint16_t> {
    auto colon = spec.rfind(':');
    if (colon == std::string::npos) {
        throw std::invalid_argument("listen address must be host:port, got '" + spec + "'");
    }
    auto host = spec.substr(0, colon);
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    unsigned port = 0;
    const auto* first = spec.data() + colon + 1;
    const auto* last = spec.data() + spec.size();
    auto [ptr, ec] = std::from_chars(first, last, port);
    if (ec != std::errc{} || ptr != last || port > 65535) {
        throw std::invalid_argument("listen port out of range in '" + spec + "'");
    }
    return {host.empty() ? "0.0.0.0" : host, static_cast<std::uint16_t>(port)};
}

/// The path of a request target without its query, percent-decoded.
/// `nullopt` for a target that is not an absolute path or has a bad escape.
[[nodiscard]] inline auto decode_path(std::string_view target) -> std::optional<std::string> {
    if (target.empty() || target.front() != '/') {
        return std::nullopt;
    }
    target = target.substr(0, target.find('?'));
    std::string out;
    out.reserve(target.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < target.size(); ++i) {
        if (target[i] != '%') {
            out.push_back(target[i]);
            continue;
        }
        if (i + 2 >= target.size() || hex(target[i + 1]) < 0 || hex(target[i + 2]) < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>((hex(target[i + 1]) << 4) | hex(target[i + 2])));
        i += 2;
    }
    return out;
}

/// One `Range` request, resolved against a value of `len` bytes.
struct byte_range {
    enum class kind : std::uint8_t {
        whole,          ///< no Range, several ranges, or a unit we ignore
        partial,        ///< `[_first, _last]`, inclusive
        unsatisfiable,  ///< 416
    };
    kind _kind = kind::whole;
    std::uint64_t _first = 0;
    std::uint64_t _last = 0;
};

/// RFC 9110 section 14: `bytes=a-b`, `bytes=a-` or `bytes=-n`. A header this
/// cannot read, or one with several ranges, is ignored and the whole value
/// is served, which the RFC allows.
[[nodiscard]] inline auto parse_range(std::string_view header, std::uint64_t len) -> byte_range {
    constexpr std::string_view unit = "bytes=";
    if (header.substr(0, unit.size()) != unit) {
        return {};
    }
    auto spec = header.substr(unit.size());
    if (spec.find(',') != std::string_view::npos) {
        return {};
    }
    auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return {};
    }
    auto number = [](std::string_view s) -> std::optional<std::uint64_t> {
        std::uint64_t v = 0;
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (s.empty() || ec != std::errc{} || p != s.data() + s.size()) {
            return std::nullopt;
        }
        return v;
    };
    auto first_text = spec.substr(0, dash);
    auto last_text = spec.substr(dash + 1);
    if (first_text.empty()) {
        auto suffix = number(last_text);
        if (!suffix) {
            return {};
        }
        if (*suffix == 0 || len == 0) {
            return {byte_range::kind::unsatisfiable};
        }
        const auto n = std::min(*suffix, len);
        return {byte_range::kind::partial, len - n, len - 1};
    }
    auto first = number(first_text);
    if (!first) {
        return {};
    }
    std::uint64_t last = len == 0 ? 0 : len - 1;
    if (!last_text.empty()) {
        auto l = number(last_text);
        if (!l || *l < *first) {
            return {};
        }
        last = std::min(*l, last);
    }
    if (*first >= len) {
        return {byte_range::kind::unsatisfiable};
    }
    return {byte_range::kind::partial, *first, last};
}

[[nodiscard]] inline auto http_date(std::chrono::system_clock::time_point t) -> std::string {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    std::array<char, 64> buf{};
    const auto n = std::strftime(buf.data(), buf.size(), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return {buf.data(), n};
}

[[nodiscard]] inline auto xml_escape(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out.push_back(c);
        }
    }
    return out;
}

/// A PROPFIND `Depth: 0` reply in the shape mod_dav sends, which OpenDAL
/// parses strictly (tasks.md task 1): `displayname`, `getlastmodified` and
/// `resourcetype` always, and for a file `getcontentlength`,
/// `getcontenttype` and `getetag` as well.
[[nodiscard]] inline auto propfind_body(std::string_view href, std::string_view name,
                                        std::optional<std::uint64_t> file_length,
                                        std::string_view etag) -> std::string {
    std::ostringstream out;
    out << R"(<?xml version="1.0" encoding="utf-8"?>)"
        << R"(<D:multistatus xmlns:D="DAV:"><D:response>)"
        << "<D:href>" << xml_escape(href) << "</D:href>"
        << "<D:propstat><D:prop>"
        << "<D:displayname>" << xml_escape(name) << "</D:displayname>"
        << "<D:getlastmodified>" << http_date(std::chrono::system_clock::now())
        << "</D:getlastmodified>";
    if (file_length) {
        out << "<D:resourcetype/>"
            << "<D:getcontentlength>" << *file_length << "</D:getcontentlength>"
            << "<D:getcontenttype>application/octet-stream</D:getcontenttype>"
            << "<D:getetag>" << xml_escape(etag) << "</D:getetag>";
    } else {
        out << "<D:resourcetype><D:collection/></D:resourcetype>";
    }
    out << "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
        << "</D:response></D:multistatus>";
    return out.str();
}

/// Run one asynchronous operation on a connection's private `io_context`
/// to completion. Every operation passed here has a Beast deadline, so this
/// cannot block forever.
template<typename Start> auto drive(asio::io_context& ioc, Start&& start) -> beast::error_code {
    beast::error_code result = asio::error::would_block;
    bool done = false;
    std::forward<Start>(start)([&result, &done](beast::error_code ec, auto&&...) {
        result = ec;
        done = true;
    });
    ioc.restart();
    while (!done && ioc.run_one() != 0) {
    }
    return done ? result : beast::error_code(asio::error::operation_aborted);
}

/// The lowest layer of either stream type, for deadlines and shutdown.
template<typename Stream> auto lowest(Stream& s) -> beast::tcp_stream& {
    if constexpr (std::is_same_v<Stream, beast::tcp_stream>) {
        return s;
    } else {
        return beast::get_lowest_layer(s);
    }
}

/// Incremental SHA-256.
class sha256_stream {
public:
    sha256_stream() : _ctx(EVP_MD_CTX_new()) {
        if (_ctx == nullptr || EVP_DigestInit_ex(_ctx, EVP_sha256(), nullptr) != 1) {
            EVP_MD_CTX_free(_ctx);
            throw std::runtime_error("sha256_stream: init failed");
        }
    }
    ~sha256_stream() { EVP_MD_CTX_free(_ctx); }
    sha256_stream(const sha256_stream&) = delete;
    auto operator=(const sha256_stream&) -> sha256_stream& = delete;
    sha256_stream(sha256_stream&&) = delete;
    auto operator=(sha256_stream&&) -> sha256_stream& = delete;

    void update(std::span<const std::byte> bytes) {
        if (!bytes.empty() && EVP_DigestUpdate(_ctx, bytes.data(), bytes.size()) != 1) {
            throw std::runtime_error("sha256_stream: update failed");
        }
    }
    [[nodiscard]] auto finish() -> chunk_sha {
        chunk_sha out{};
        unsigned int len = 0;
        if (EVP_DigestFinal_ex(_ctx, reinterpret_cast<unsigned char*>(out.data()), &len) != 1 ||
            len != out.size()) {
            throw std::runtime_error("sha256_stream: final failed");
        }
        return out;
    }

private:
    EVP_MD_CTX* _ctx;
};

}  // namespace cache_http_detail

/// `Host` is a `multi_raft` whose state machine is `cache_kv_state_machine`.
/// `refs` is the node-wide chunk store and counts every replica shares.
template<typename Host, typename Logger> class cache_http_front_end {
public:
    using node_id_type = typename Host::node_id_type;
    using group_id_type = typename Host::group_id_type;
    using future_type = typename Host::future_type;
    /// `host:port` of a peer's front end (its TLS listener when
    /// `_forward_tls` is set), from the daemon's membership view.
    using endpoint_resolver = std::function<std::optional<std::string>(const node_id_type&)>;
    /// A chunk that failed its hash on read, for the fetcher to replace.
    using repair_callback = std::function<void(const group_id_type&, const chunk_sha&)>;
    /// Why a write to a shard with these voters must be refused, for the
    /// disk guard's view of the voters' floors (Requirement 6.6).
    using write_refusal_fn = std::function<std::optional<std::string>(
        const group_id_type&, const std::vector<node_id_type>&)>;
    /// A key served or written (`false`) or deleted (`true`).
    using access_fn = std::function<void(const std::string&, bool)>;

    cache_http_front_end(Host& host, const redis_acl& acl, Logger& logger,
                         std::shared_ptr<chunk_refcounts> refs, cache_http_config config,
                         endpoint_resolver resolver, repair_callback on_repair = {})
        : _host(host),
          _acl(acl),
          _logger(logger),
          _refs(std::move(refs)),
          _config(std::move(config)),
          _resolver(std::move(resolver)),
          _on_repair(std::move(on_repair)),
          _auth_cache(_config._auth_cache_ttl, _config._auth_cache_entries),
          _auth_limiter(_config._auth_failure_limit, _config._auth_failure_window,
                        _config._max_concurrent_auth) {
        if (!_refs) {
            throw std::invalid_argument("cache_http_front_end: no chunk store");
        }
    }

    ~cache_http_front_end() { stop(); }
    cache_http_front_end(const cache_http_front_end&) = delete;
    auto operator=(const cache_http_front_end&) -> cache_http_front_end& = delete;
    cache_http_front_end(cache_http_front_end&&) = delete;
    auto operator=(cache_http_front_end&&) -> cache_http_front_end& = delete;

    /// Bind the listeners and start accepting. Throws on a bind failure and
    /// on a configuration Requirement 8.5 or the TLS rules refuse.
    void start();
    /// Close the listeners, cut every connection, join every thread.
    void stop();

    [[nodiscard]] auto port() const noexcept -> std::uint16_t { return _port; }
    [[nodiscard]] auto tls_port() const noexcept -> std::uint16_t { return _tls_port; }
    [[nodiscard]] auto stats() const noexcept -> const cache_http_stats& { return _stats; }
    [[nodiscard]] auto config() const noexcept -> const cache_http_config& { return _config; }
    [[nodiscard]] auto auth_cache() const noexcept -> const cache_http_auth_cache& {
        return _auth_cache;
    }

    /// Checked on the leader before a `PUT` streams anything. Call before
    /// `start()`.
    void set_write_refusal(write_refusal_fn fn) { _write_refusal = std::move(fn); }
    /// Lines appended to `/_metrics`, already in its `name value` form.
    void set_metrics_extra(std::function<std::string()> fn) { _metrics_extra = std::move(fn); }
    /// Told of each key this front end serves or writes (`removed` false)
    /// and deletes (`removed` true), so the RESP gateway's least-recently-
    /// read order sees HTTP traffic too (Requirement 6.1, 6.2). Called on
    /// the I/O thread after the response is decided. Call before `start()`.
    void set_access_hook(access_fn fn) { _on_access = std::move(fn); }

    /// `GET /_chunk/<sha>` from `peer`'s front end with the internal
    /// secret, over the transport forwarding uses: the chunk fetcher's
    /// `fetch` in the daemon (design Component 5). The bytes as sent, or
    /// `nullopt` if the peer is unknown, unreachable, or lacks the chunk.
    /// The caller verifies them. Safe from any thread once `start()` has
    /// returned.
    [[nodiscard]] auto fetch_chunk(const node_id_type& peer, const chunk_sha& sha)
        -> std::optional<std::vector<std::byte>>;

private:
    using tcp = boost::asio::ip::tcp;
    using plain_stream = boost::beast::tcp_stream;
    using tls_stream = boost::beast::ssl_stream<boost::beast::tcp_stream>;
    using request_parser = boost::beast::http::request_parser<boost::beast::http::buffer_body>;

    /// One connection: its thread, its private `io_context`, and a way for
    /// `stop()` to cut it.
    struct connection {
        boost::asio::io_context _ioc{1};
        std::thread _thread;
        std::atomic<bool> _done{false};
        std::mutex _mu;
        /// The socket, for `stop()` to shut down from another thread; -1
        /// once the connection no longer owns it.
        int _fd = -1;
        http_detail::connection_gate::slot _slot;
        /// The connection's read buffer, so a refusal can drain a small body
        /// and keep the connection.
        boost::beast::flat_buffer* _buffer = nullptr;
    };

    /// Who sent a request: an ACL identity, or the internal secret.
    struct principal {
        std::optional<redis_identity> _id;
        bool _internal = false;
    };

    /// What a request handler tells the connection loop.
    enum class after : std::uint8_t {
        keep_alive,
        close
    };

    void accept_loop(const std::shared_ptr<tcp::acceptor>& acceptor, bool tls);
    void reap();
    void serve(const std::shared_ptr<connection>& c, tcp::socket socket, bool tls);
    template<typename Stream> void serve_stream(connection& c, Stream& stream, bool tls);
    template<typename Stream>
    auto handle(connection& c, Stream& stream, boost::beast::flat_buffer& buffer,
                request_parser& parser, std::string_view peer_address) -> after;

    // ---- request handlers ------------------------------------------------
    template<typename Stream>
    auto handle_get(connection& c, Stream& stream, boost::beast::flat_buffer& buffer,
                    request_parser& parser, const std::string& key, bool head) -> after;
    template<typename Stream>
    auto handle_put(connection& c, Stream& stream, boost::beast::flat_buffer& buffer,
                    request_parser& parser, const std::string& key) -> after;
    template<typename Stream>
    auto handle_delete(connection& c, Stream& stream, boost::beast::flat_buffer& buffer,
                       request_parser& parser, const std::string& key) -> after;
    template<typename Stream>
    auto handle_propfind(connection& c, Stream& stream, request_parser& parser,
                         const std::string& key, std::string_view raw_path) -> after;
    template<typename Stream>
    auto handle_chunk(connection& c, Stream& stream, request_parser& parser, std::string_view hex)
        -> after;
    template<typename Stream>
    auto handle_metrics(connection& c, Stream& stream, request_parser& parser) -> after;

    // ---- plumbing ----------------------------------------------------------
    auto authenticate(const boost::beast::http::request_header<>& req, std::string_view peer,
                      int& status) -> std::optional<principal>;
    /// The local replica of the shard owning `key`, or null with
    /// `forward_to` set to who might serve it instead.
    auto locate(const std::string& key, std::optional<node_id_type>& forward_to) ->
        typename Host::group_node_type*;
    /// Submit and wait. The apply result, or `nullopt` with `forward_to`
    /// set when another node leads.
    auto propose(const std::string& key, const std::vector<std::byte>& command,
                 std::optional<node_id_type>& forward_to) -> std::optional<std::vector<std::byte>>;
    [[nodiscard]] auto space_ok() const -> bool;

    template<typename Stream>
    auto reply(connection& c, Stream& stream, request_parser& parser,
               boost::beast::http::status status, std::string body = {},
               std::string_view content_type = "text/plain") -> after;
    /// Discard what is left of a request body so the connection can carry
    /// another request; closes instead when that would mean reading a lot.
    template<typename Stream>
    auto drain_body(connection& c, Stream& stream, boost::beast::flat_buffer& buffer,
                    request_parser& parser) -> bool;
    template<typename Stream>
    auto forward(connection& c, Stream& stream, boost::beast::flat_buffer& buffer,
                 request_parser& parser, const node_id_type& to) -> std::optional<after>;
    template<typename Down, typename Up>
    auto relay(connection& c, Down& down, boost::beast::flat_buffer& buffer, request_parser& parser,
               Up& up, bool& downstream_written) -> bool;

    Host& _host;
    const redis_acl& _acl;
    Logger& _logger;
    std::shared_ptr<chunk_refcounts> _refs;
    cache_http_config _config;
    endpoint_resolver _resolver;
    repair_callback _on_repair;
    write_refusal_fn _write_refusal;
    std::function<std::string()> _metrics_extra;
    access_fn _on_access;
    cache_http_stats _stats;
    cache_http_auth_cache _auth_cache;
    redis_auth_limiter _auth_limiter;

    boost::asio::io_context _accept_ioc{1};
    std::vector<std::shared_ptr<tcp::acceptor>> _acceptors;
    std::vector<std::shared_ptr<tcp::acceptor>> _tls_acceptors;
    std::vector<std::thread> _accept_threads;
    std::shared_ptr<boost::asio::ssl::context> _ssl_ctx;
    std::shared_ptr<boost::asio::ssl::context> _forward_ssl_ctx;
    std::shared_ptr<http_detail::connection_gate> _gate;
    std::uint16_t _port = 0;
    std::uint16_t _tls_port = 0;
    std::atomic<bool> _running{false};

    std::mutex _connections_mu;
    std::list<std::shared_ptr<connection>> _connections;
};

// ============================================================================
// Implementation
// ============================================================================

template<typename Host, typename Logger> void cache_http_front_end<Host, Logger>::start() {
    namespace ssl = boost::asio::ssl;
    if (_running.load()) {
        return;
    }
    if (_config._listen.empty() && _config._tls_listen.empty()) {
        return;  // both off (Requirement 7.1)
    }
    if (!_config._listen.empty() && !_config._allow_plaintext) {
        throw std::runtime_error(
            "cache http: a plaintext listener needs KYTHIRA_CACHE_ALLOW_PLAINTEXT_HTTP=true; "
            "credentials and values would cross the network unencrypted");
    }
    if (_config._chunk_bytes == 0 ||
        _config._chunk_bytes > _refs->store().options()._max_chunk_bytes) {
        throw std::runtime_error(
            "cache http: chunk size must be between 1 byte and the store's "
            "largest chunk");
    }
    if (_acl.empty()) {
        throw std::runtime_error("cache http: the ACL has no users, so nothing could authenticate");
    }
    if (_config._forwarding && _config._forward_tls) {
        if (_config._forward_tls_ca_path.empty()) {
            throw std::runtime_error("cache http: forwarding over TLS needs a CA to verify peers");
        }
        _forward_ssl_ctx = std::make_shared<ssl::context>(ssl::context::tls_client);
        SSL_CTX_set_min_proto_version(_forward_ssl_ctx->native_handle(), TLS1_2_VERSION);
        _forward_ssl_ctx->load_verify_file(_config._forward_tls_ca_path);
        _forward_ssl_ctx->set_verify_mode(ssl::verify_peer);
    } else if (_config._forwarding && !_config._allow_plaintext_forwarding) {
        _logger.log(log_level::warning,
                    "cache http: forwarding is on but neither TLS nor plaintext forwarding is "
                    "enabled; requests for a shard led elsewhere get 503");
    }
    if (!_config._tls_listen.empty()) {
        if (_config._tls_cert_path.empty() || _config._tls_key_path.empty()) {
            throw std::runtime_error("cache http: the HTTPS listener needs a certificate and key");
        }
        _ssl_ctx = std::make_shared<ssl::context>(ssl::context::tls_server);
        _ssl_ctx->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                              ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                              ssl::context::no_tlsv1_1);
        SSL_CTX_set_min_proto_version(_ssl_ctx->native_handle(), TLS1_2_VERSION);
        _ssl_ctx->use_certificate_chain_file(_config._tls_cert_path);
        _ssl_ctx->use_private_key_file(_config._tls_key_path, ssl::context::pem);
    }

    _gate = std::make_shared<http_detail::connection_gate>(_config._max_connections);
    auto bind = [this](const std::string& spec) {
        auto [host, port] = cache_http_detail::parse_listen(spec);
        return net_bind::open_asio_acceptors(_accept_ioc, host, port, "cache http");
    };
    if (!_config._listen.empty()) {
        _acceptors = bind(_config._listen);
        _port = _acceptors.front()->local_endpoint().port();
        _logger.log(log_level::warning,
                    "cache http: PLAINTEXT listener; credentials and values cross the network "
                    "unencrypted",
                    {{"listen", _config._listen}, {"port", std::to_string(_port)}});
    }
    if (!_config._tls_listen.empty()) {
        _tls_acceptors = bind(_config._tls_listen);
        _tls_port = _tls_acceptors.front()->local_endpoint().port();
        _logger.log(log_level::info, "cache http: HTTPS listener",
                    {{"listen", _config._tls_listen}, {"port", std::to_string(_tls_port)}});
    }
    _running = true;
    for (const auto& a : _acceptors) {
        _accept_threads.emplace_back([this, a] { accept_loop(a, false); });
    }
    for (const auto& a : _tls_acceptors) {
        _accept_threads.emplace_back([this, a] { accept_loop(a, true); });
    }
}

template<typename Host, typename Logger> void cache_http_front_end<Host, Logger>::stop() {
    if (!_running.exchange(false)) {
        return;
    }
    for (auto* list : {&_acceptors, &_tls_acceptors}) {
        for (const auto& a : *list) {
            // Wakes the blocking accept() in its thread. The acceptor is
            // closed only after that thread is joined: close() rewrites
            // the descriptor the accept() is still reading.
            (void)::shutdown(a->native_handle(), SHUT_RDWR);
        }
    }
    for (auto& t : _accept_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    for (auto* list : {&_acceptors, &_tls_acceptors}) {
        for (const auto& a : *list) {
            boost::system::error_code ec;
            (void)a->close(ec);
        }
    }
    _accept_threads.clear();
    _acceptors.clear();
    _tls_acceptors.clear();
    std::list<std::shared_ptr<connection>> live;
    {
        const std::lock_guard lock(_connections_mu);
        live.swap(_connections);
    }
    for (const auto& c : live) {
        // A shutdown fails the connection's pending read or write, whatever
        // thread it is on; its thread then winds down.
        const std::lock_guard lock(c->_mu);
        if (c->_fd >= 0) {
            (void)::shutdown(c->_fd, SHUT_RDWR);
        }
    }
    for (const auto& c : live) {
        if (c->_thread.joinable()) {
            c->_thread.join();
        }
    }
}

template<typename Host, typename Logger>
void cache_http_front_end<Host, Logger>::accept_loop(const std::shared_ptr<tcp::acceptor>& acceptor,
                                                     bool tls) {
    while (_running.load()) {
        boost::system::error_code ec;
        tcp::socket socket(_accept_ioc);
        (void)acceptor->accept(socket, ec);
        if (ec) {
            if (!_running.load()) {
                return;
            }
            if (ec == boost::asio::error::bad_descriptor ||
                ec == boost::asio::error::operation_aborted) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }
        reap();
        auto slot = _gate->try_acquire();
        if (!slot) {
            // The connection cap (http-server-request-limits): closed at once.
            ++_stats._connections_rejected;
            (void)socket.close(ec);
            continue;
        }
        ++_stats._connections_accepted;
        auto c = std::make_shared<connection>();
        c->_slot = std::move(slot);
        {
            const std::lock_guard lock(_connections_mu);
            if (!_running.load()) {
                return;
            }
            _connections.push_back(c);
        }
        c->_thread = std::thread(
            [this, c, s = std::move(socket), tls]() mutable { serve(c, std::move(s), tls); });
    }
}

template<typename Host, typename Logger> void cache_http_front_end<Host, Logger>::reap() {
    std::list<std::shared_ptr<connection>> finished;
    {
        const std::lock_guard lock(_connections_mu);
        for (auto it = _connections.begin(); it != _connections.end();) {
            if ((*it)->_done.load()) {
                finished.push_back(*it);
                it = _connections.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& c : finished) {
        if (c->_thread.joinable()) {
            c->_thread.join();
        }
    }
}

template<typename Host, typename Logger>
void cache_http_front_end<Host, Logger>::serve(const std::shared_ptr<connection>& c,
                                               tcp::socket socket, bool tls) {
    ++_stats._connections_current;
    try {
        // Move the socket onto this connection's own io_context.
        boost::system::error_code ec;
        const auto protocol = socket.local_endpoint(ec).protocol();
        tcp::socket mine(c->_ioc, protocol, socket.release());
        {
            const std::lock_guard lock(c->_mu);
            c->_fd = mine.native_handle();
        }
        // Forget the descriptor before the stream that owns it closes it.
        auto forget = [&c] {
            const std::lock_guard lock(c->_mu);
            c->_fd = -1;
        };
        if (tls) {
            tls_stream stream(plain_stream(std::move(mine)), *_ssl_ctx);
            try {
                serve_stream(*c, stream, true);
            } catch (...) {
                forget();
                throw;
            }
            forget();
        } else {
            plain_stream stream(std::move(mine));
            try {
                serve_stream(*c, stream, false);
            } catch (...) {
                forget();
                throw;
            }
            forget();
        }
    } catch (const std::exception& e) {
        _logger.log(log_level::debug, "cache http: connection ended", {{"error", e.what()}});
    }
    c->_slot.release();
    --_stats._connections_current;
    c->_done = true;
}

template<typename Host, typename Logger>
template<typename Stream>
void cache_http_front_end<Host, Logger>::serve_stream(connection& c, Stream& stream, bool tls) {
    namespace http = boost::beast::http;
    using cache_http_detail::drive;
    using cache_http_detail::lowest;
    std::string peer;
    {
        boost::system::error_code ec;
        auto ep = lowest(stream).socket().remote_endpoint(ec);
        if (!ec) {
            peer = redis_auth_limiter::rate_key(ep.address());
        }
    }
    if (tls) {
        if constexpr (!std::is_same_v<Stream, plain_stream>) {
            lowest(stream).expires_after(_config._handshake_timeout);
            auto ec = drive(c._ioc, [&](auto h) {
                stream.async_handshake(boost::asio::ssl::stream_base::server, std::move(h));
            });
            if (ec) {
                return;
            }
        }
    }
    boost::beast::flat_buffer buffer;
    c._buffer = &buffer;
    bool first = true;
    while (_running.load()) {
        request_parser parser;
        parser.header_limit(_config._max_header_bytes);
        parser.body_limit(_config._max_value_bytes);
        lowest(stream).expires_after(first ? _config._header_timeout : _config._idle_timeout);
        first = false;
        auto ec = drive(
            c._ioc, [&](auto h) { http::async_read_header(stream, buffer, parser, std::move(h)); });
        if (ec == http::error::header_limit) {
            (void)reply(c, stream, parser, http::status::request_header_fields_too_large);
            break;
        }
        if (ec == http::error::body_limit) {
            // A chunked body whose first chunks, read along with the header,
            // already pass the maximum (Requirement 7.4).
            ++_stats._requests;
            ++_stats._too_large;
            (void)reply(c, stream, parser, http::status::payload_too_large);
            break;
        }
        if (ec) {
            _logger.log(log_level::debug, "cache http: header read ended",
                        {{"error", ec.message()}});
            break;  // closed, timed out, or not HTTP
        }
        ++_stats._requests;
        after next = after::close;
        try {
            next = handle(c, stream, buffer, parser, peer);
        } catch (const std::exception& e) {
            _logger.log(log_level::warning, "cache http: request failed", {{"error", e.what()}});
            next = after::close;
        }
        if (next == after::close) {
            break;
        }
    }
    lowest(stream).expires_after(std::chrono::seconds{1});
    if constexpr (!std::is_same_v<Stream, plain_stream>) {
        (void)drive(c._ioc, [&](auto h) { stream.async_shutdown(std::move(h)); });
    }
    // A lingering close: stop sending, then read and drop what the client
    // is still sending for a moment. Closing with unread bytes queued makes
    // the kernel send a reset, which can destroy a 413 or 403 the client has
    // not read yet.
    boost::system::error_code ignored;
    lowest(stream).socket().shutdown(tcp::socket::shutdown_send, ignored);
    std::array<char, 16384> sink{};
    std::size_t dropped = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (dropped < std::size_t{4} * 1024 * 1024 && std::chrono::steady_clock::now() < until) {
        lowest(stream).expires_at(until);
        std::size_t n = 0;
        auto ec = drive(c._ioc, [&](auto h) {
            lowest(stream).async_read_some(
                boost::asio::buffer(sink),
                [&n, h = std::move(h)](boost::system::error_code e, std::size_t got) mutable {
                    n = got;
                    std::move(h)(e, got);
                });
        });
        if (ec) {
            break;
        }
        dropped += n;
    }
    lowest(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
}

template<typename Host, typename Logger>
auto cache_http_front_end<Host, Logger>::authenticate(
    const boost::beast::http::request_header<>& req, std::string_view peer, int& status)
    -> std::optional<principal> {
    namespace http = boost::beast::http;
    auto header = req[http::field::authorization];
    auto creds = parse_cache_http_authorization(std::string_view(header.data(), header.size()));
    if (!creds) {
        status = 401;
        return std::nullopt;
    }
    if (creds->_scheme == cache_http_credentials::scheme::bearer &&
        !_config._internal_secret.empty() &&
        cache_http_secret_equal(creds->_secret, _config._internal_secret)) {
        return principal{std::nullopt, true};
    }
    const std::string rate_key(peer);
    std::optional<redis_identity> id;
    bool limited = false;
    bool busy = false;
    id = _auth_cache.authenticate(_acl, *creds, [&]() -> std::optional<redis_identity> {
        switch (_auth_limiter.begin(rate_key)) {
            case redis_auth_limiter::verdict::rate_limited:
                limited = true;
                return std::nullopt;
            case redis_auth_limiter::verdict::busy:
                busy = true;
                return std::nullopt;
            case redis_auth_limiter::verdict::allowed:
                break;
        }
        std::optional<redis_identity> got;
        try {
            got = creds->_scheme == cache_http_credentials::scheme::bearer
                      ? cache_http_auth_cache::verify_bearer(_acl, creds->_secret)
                      : _acl.authenticate(creds->_user, creds->_secret);
        } catch (...) {
            _auth_limiter.finish(rate_key, false);
            throw;
        }
        _auth_limiter.finish(rate_key, got.has_value());
        return got;
    });
    if (id) {
        return principal{std::move(id), false};
    }
    if (busy) {
        status = 503;
    } else {
        ++_stats._auth_failures;
        status = limited ? 429 : 401;
    }
    return std::nullopt;
}

template<typename Host, typename Logger>
auto cache_http_front_end<Host, Logger>::locate(const std::string& key,
                                                std::optional<node_id_type>& forward_to) ->
    typename Host::group_node_type* {
    if (!_host.is_running()) {
        return nullptr;
    }
    auto desc = _host.resolve(key);
    if (!desc) {
        return nullptr;
    }
    auto* node = _host.group_node(desc->group_id());
    if (node == nullptr) {
        for (const auto& voter : desc->voters()) {
            if (voter != _host.node_id()) {
                forward_to = voter;
                break;
            }
        }
        if (desc->leader_hint() && *desc->leader_hint() != _host.node_id()) {
            forward_to = desc->leader_hint();
        }
    }
    return node;
}

template<typename Host, typename Logger>
auto cache_http_front_end<Host, Logger>::propose(const std::string& key,
                                                 const std::vector<std::byte>& command,
                                                 std::optional<node_id_type>& forward_to)
    -> std::optional<std::vector<std::byte>> {
    try {
        auto future = _host.submit_command(key, command, _config._command_timeout);
        if (!future.wait(_config._command_timeout)) {
            return std::nullopt;
        }
        return std::move(future).get();
    } catch (const shard_not_leader_exception<group_id_type, node_id_type>& e) {
        forward_to = e.leader_hint();
        return std::nullopt;
    } catch (const std::exception& e) {
        _logger.log(log_level::debug, "cache http: proposal failed", {{"error", e.what()}});
        return std::nullopt;
    }
}

template<typename Host, typename Logger>
auto cache_http_front_end<Host, Logger>::space_ok() const -> bool {
    try {
        const auto s = _refs->store().space();
        return s._free_bytes >= _config._min_free_bytes &&
               s._free_inodes >= _config._min_free_inodes;
    } catch (const std::exception&) {
        return false;
    }
}

template<typename Host, typename Logger>
template<typename Stream>
auto cache_http_front_end<Host, Logger>::reply(connection& c, Stream& stream,
                                               request_parser& parser,
                                               boost::beast::http::status status, std::string body,
                                               std::string_view content_type) -> after {
    namespace http = boost::beast::http;
    // A refused request with a small declared body: read and drop the body
    // so the connection can carry the client's next request.
    if (!parser.is_done() && status != http::status::payload_too_large && c._buffer != nullptr) {
        if (auto len = parser.content_length(); len && *len <= 64 * 1024) {
            (void)drain_body(c, stream, *c._buffer, parser);
        }
    }
    const auto& req = parser.get();
    http::response<http::string_body> res{status, req.version()};
    res.set(http::field::server, "kythira-cache");
    if (!body.empty()) {
        res.set(http::field::content_type, content_type);
    }
    if (status == http::status::unauthorized) {
        res.set(http::field::www_authenticate, R"(Basic realm="kythira")");
    }
    if (status == http::status::service_unavailable) {
        res.set(http::field::retry_after, "1");
    }
    // A request whose body was not read cannot be followed by another one.
    const bool keep = req.keep_alive() && parser.is_done();
    res.keep_alive(keep);
    res.body() = std::move(body);
    res.prepare_payload();
    cache_http_detail::lowest(stream).expires_after(_config._io_timeout);
    auto ec = cache_http_detail::drive(
        c._ioc, [&](auto h) { http::async_write(stream, res, std::move(h)); });
    return !ec && keep ? after::keep_alive : after::close;
}

template<typename Host, typename Logger>
template<typename Stream>
auto cache_http_front_end<Host, Logger>::drain_body(connection& c, Stream& stream,
                                                    boost::beast::flat_buffer& buffer,
                                                    request_parser& parser) -> bool {
    namespace http = boost::beast::http;
    // Small bodies (a PROPFIND's XML, a stray DELETE body) are read and
    // dropped; anything bigger is not worth reading just to reuse a socket.
    constexpr std::size_t limit = 64 * 1024;
    std::array<char, 8192> scratch{};
    std::size_t read = 0;
    while (!parser.is_done()) {
        parser.get().body().data = scratch.data();
        parser.get().body().size = scratch.size();
        cache_http_detail::lowest(stream).expires_after(_config._io_timeout);
        auto ec = cache_http_detail::drive(
            c._ioc, [&](auto h) { http::async_read(stream, buffer, parser, std::move(h)); });
        if (ec == http::error::need_buffer) {
            ec = {};
        }
        if (ec) {
            return false;
        }
        read += scratch.size() - parser.get().body().size;
        if (read > limit) {
            return false;
        }
    }
    return true;
}

template<typename Host, typename Logger>
template<typename Stream>
auto cache_http_front_end<Host, Logger>::handle(connection& c, Stream& stream,
                                                boost::beast::flat_buffer& buffer,
                                                request_parser& parser, std::string_view peer)
    -> after {
    namespace http = boost::beast::http;
    const auto& req = parser.get();
    const auto method = req.method();
    const std::string_view target(req.target().data(), req.target().size());
    auto path = cache_http_detail::decode_path(target);
    if (!path) {
        return reply(c, stream, parser, http::status::bad_request, "bad request target\n");
    }

    int auth_status = 0;
    auto who = authenticate(req, peer, auth_status);
    if (!who) {
        auto status = auth_status == 429   ? http::status::too_many_requests
                      : auth_status == 503 ? http::status::service_unavailable
                                           : http::status::unauthorized;
        return reply(c, stream, parser, status);
    }

    // Internal and administrative routes.
    constexpr std::string_view chunk_route = "/_chunk/";
    if (path->starts_with(chunk_route)) {
        if (!who->_internal || method != http::verb::get) {
            ++_stats._forbidden;
            return reply(c, stream, parser, http::status::forbidden);
        }
        return handle_chunk(c, stream, parser, std::string_view(*path).substr(chunk_route.size()));
    }
    if (*path == "/_metrics") {
        if (who->_internal || !who->_id || who->_id->_role != redis_role::admin ||
            method != http::verb::get) {
            ++_stats._forbidden;
            return reply(c, stream, parser, http::status::forbidden);
        }
        return handle_metrics(c, stream, parser);
    }
    if (who->_internal) {
        // The internal secret is for chunk fetches only.
        ++_stats._forbidden;
        return reply(c, stream, parser, http::status::forbidden);
    }

    std::string key = path->substr(1);
    if (key.size() > _config._max_key_bytes) {
        return reply(c, stream, parser, http::status::uri_too_long);
    }
    const auto& id = *who->_id;
    const bool write = method == http::verb::put || method == http::verb::delete_ ||
                       method == http::verb::mkcol || method == http::verb::propfind;
    const bool read = method == http::verb::get || method == http::verb::head;
    if (!write && !read) {
        return reply(c, stream, parser, http::status::method_not_allowed);
    }
    // Requirement 8.2: refused before anything reaches the log.
    const auto needed = write ? redis_role::read_write : redis_role::read_only;
    if (static_cast<int>(id._role) < static_cast<int>(needed) ||
        !redis_acl::key_in_scope(id, key)) {
        ++_stats._forbidden;
        return reply(c, stream, parser, http::status::forbidden);
    }

    switch (method) {
        case http::verb::get:
        case http::verb::head:
            if (key.empty() || key.back() == '/') {
                return reply(c, stream, parser, http::status::not_found);
            }
            return handle_get(c, stream, buffer, parser, key, method == http::verb::head);
        case http::verb::put:
            if (key.empty() || key.back() == '/') {
                return reply(c, stream, parser, http::status::method_not_allowed);
            }
            return handle_put(c, stream, buffer, parser, key);
        case http::verb::delete_:
            return handle_delete(c, stream, buffer, parser, key);
        case http::verb::propfind:
            if (!drain_body(c, stream, buffer, parser)) {
                return after::close;
            }
            return handle_propfind(c, stream, parser, key, target.substr(0, target.find('?')));
        case http::verb::mkcol:
            // Keys have no directories (Requirement 7.6).
            if (!drain_body(c, stream, buffer, parser)) {
                return after::close;
            }
            return reply(c, stream, parser, http::status::created);
        default:
            return reply(c, stream, parser, http::status::method_not_allowed);
    }
}

template<typename Host, typename Logger>
template<typename Stream>
auto cache_http_front_end<Host, Logger>::handle_get(connection& c, Stream& stream,
                                                    boost::beast::flat_buffer& buffer,
                                                    request_parser& parser, const std::string& key,
                                                    bool head) -> after {
    namespace http = boost::beast::http;
    using cache_http_detail::byte_range;
    if (!drain_body(c, stream, buffer, parser)) {
        return after::close;
    }
    const bool forwarded = parser.get().count("X-Kythira-Forwarded") != 0;
    std::optional<node_id_type> forward_to;
    auto* node = locate(key, forward_to);
    if (node == nullptr) {
        if (forward_to && !forwarded) {
            if (auto r = forward(c, stream, buffer, parser, *forward_to)) {
                return *r;
            }
        }
        return reply(c, stream, parser, http::status::service_unavailable);
    }
    auto manifest = node->with_state_machine([&key](auto& sm) { return sm.lookup(key); });
    const auto now_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    if (manifest && manifest->_expire_at_ms != 0 && manifest->_expire_at_ms <= now_ms) {
        manifest = nullptr;
    }
    // Not all here yet: a follower relays to the leader, a leader misses
    // (Requirement 4.4).
    auto unservable = [&]() -> std::optional<after> {
        if (!node->is_leader() && !forwarded) {
            if (auto leader = node->known_leader()) {
                if (auto r = forward(c, stream, buffer, parser, *leader)) {
                    return r;
                }
            }
        }
        return std::nullopt;
    };
    if (manifest && !manifest->has_all()) {
        if (auto r = unservable()) {
            return *r;
        }
        manifest = nullptr;
    }
    if (!manifest) {
        ++_stats._misses;
        return reply(c, stream, parser, http::status::not_found);
    }
    if (_on_access) {
        _on_access(key, false);
    }

    const auto len = manifest->_total_len;
    byte_range range;
    if (auto h = parser.get()[http::field::range]; !h.empty()) {
        range = cache_http_detail::parse_range(std::string_view(h.data(), h.size()), len);
    }
    if (range._kind == byte_range::kind::unsatisfiable) {
        http::response<http::empty_body> res{http::status::range_not_satisfiable,
                                             parser.get().version()};
        res.set(http::field::content_range, "bytes */" + std::to_string(len));
        res.keep_alive(parser.get().keep_alive());
        res.prepare_payload();
        cache_http_detail::lowest(stream).expires_after(_config._io_timeout);
        auto ec = cache_http_detail::drive(
            c._ioc, [&](auto h) { http::async_write(stream, res, std::move(h)); });
        return !ec && res.keep_alive() ? after::keep_alive : after::close;
    }
    const std::uint64_t first = range._kind == byte_range::kind::partial ? range._first : 0;
    const std::uint64_t last =
        range._kind == byte_range::kind::partial ? range._last : (len == 0 ? 0 : len - 1);
    const std::uint64_t body_len = len == 0 ? 0 : last - first + 1;

    // Read the first chunk the response needs before any header goes out, so
    // a chunk that is gone or corrupt is still a clean miss.
    const auto chunks = manifest->chunks();
    std::size_t index = 0;
    std::uint64_t offset = 0;  // value offset of chunks[index]
    std::optional<std::vector<std::byte>> current;
    const auto& store = _refs->store();
    auto group = _host.resolve(key);
    auto damaged = [&](const chunk_sha& sha) {
        manifest->set_has_all(false);
        if (_on_repair && group) {
            _on_repair(group->group_id(), sha);
        }
    };
    if (!head && body_len != 0) {
        // Skip whole chunks before the range by length; only `_total_len` and
        // chunk order are known, so walk sizes by reading the store's view.
        for (; index < chunks.size(); ++index) {
            current = store.read(chunks[index]);
            if (!current) {
                damaged(chunks[index]);
                if (auto r = unservable()) {
                    return *r;
                }
                ++_stats._misses;
                return reply(c, stream, parser, http::status::not_found);
            }
            if (offset + current->size() > first) {
                break;
            }
            offset += current->size();
        }
    }

    http::response<http::empty_body> res{
        range._kind == byte_range::kind::partial ? http::status::partial_content : http::status::ok,
        parser.get().version()};
    res.set(http::field::server, "kythira-cache");
    res.set(http::field::content_type, "application/octet-stream");
    res.set(http::field::accept_ranges, "bytes");
    res.set(http::field::etag, "\"" + to_hex(manifest->_value_sha) + "\"");
    if (range._kind == byte_range::kind::partial) {
        res.set(http::field::content_range, "bytes " + std::to_string(first) + "-" +
                                                std::to_string(last) + "/" + std::to_string(len));
    }
    res.content_length(range._kind == byte_range::kind::partial ? body_len : len);
    res.keep_alive(parser.get().keep_alive());
    http::response_serializer<http::empty_body> sr{res};
    cache_http_detail::lowest(stream).expires_after(_config._io_timeout);
    auto ec = cache_http_detail::drive(
        c._ioc, [&](auto h) { http::async_write_header(stream, sr, std::move(h)); });
    if (ec) {
        return after::close;
    }
    ++_stats._hits;
    if (head || body_len == 0) {
        return res.keep_alive() ? after::keep_alive : after::close;
    }

    std::uint64_t remaining = body_len;
    std::uint64_t position = first;
    while (remaining > 0) {
        if (!current) {
            if (index >= chunks.size()) {
                ++_stats._aborted_gets;
                return after::close;  // manifest shorter than its length
            }
            current = store.read(chunks[index]);
            if (!current) {
                // Requirement 10.2: never finish a body with bytes we cannot
                // vouch for. The client sees a short read.
                damaged(chunks[index]);
                ++_stats._aborted_gets;
                return after::close;
            }
        }
        const auto skip = static_cast<std::size_t>(position - offset);
        const auto take =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, current->size() - skip));
        cache_http_detail::lowest(stream).expires_after(_config._io_timeout);
        ec = cache_http_detail::drive(c._ioc, [&](auto h) {
            boost::asio::async_write(stream, boost::asio::buffer(current->data() + skip, take),
                                     std::move(h));
        });
        if (ec) {
            return after::close;
        }
        _stats._get_bytes += take;
        remaining -= take;
        position += take;
        offset += current->size();
        current.reset();
        ++index;
    }
    return res.keep_alive() ? after::keep_alive : after::close;
}

template<typename Host, typename Logger>
template<typename Stream>
auto cache_http_front_end<Host, Logger>::handle_put(connection& c, Stream& stream,
                                                    boost::beast::flat_buffer& buffer,
                                                    request_parser& parser, const std::string& key)
    -> after {
    namespace http = boost::beast::http;
    auto& req = parser.get();
    if (auto len = parser.content_length(); len && *len > _config._max_value_bytes) {
        ++_stats._too_large;
        return reply(c, stream, parser, http::status::payload_too_large);
    }
    if (!space_ok()) {
        ++_stats._insufficient_storage;
        return reply(c, stream, parser, http::status::insufficient_storage);
    }
    const bool forwarded = req.count("X-Kythira-Forwarded") != 0;
    std::optional<node_id_type> forward_to;
    auto* node = locate(key, forward_to);
    if (node != nullptr && !node->is_leader()) {
        forward_to = node->known_leader();
        node = nullptr;
    }
    if (node == nullptr) {
        if (forward_to && !forwarded) {
            if (auto r = forward(c, stream, buffer, parser, *forward_to)) {
                return *r;
            }
        }
        return reply(c, stream, parser, http::status::service_unavailable);
    }
    if (_write_refusal) {
        // A voter of the shard is below a floor (Requirement 6.6).
        if (auto desc = _host.resolve(key)) {
            if (auto refused = _write_refusal(desc->group_id(), desc->voters())) {
                ++_stats._insufficient_storage;
                return reply(c, stream, parser, http::status::insufficient_storage, *refused);
            }
        }
    }

    if (boost::beast::iequals(req[http::field::expect], "100-continue")) {
        http::response<http::empty_body> cont{http::status::continue_, req.version()};
        cache_http_detail::lowest(stream).expires_after(_config._io_timeout);
        auto ec = cache_http_detail::drive(
            c._ioc, [&](auto h) { http::async_write(stream, cont, std::move(h)); });
        if (ec) {
            return after::close;
        }
    }

    // Two chunks at most: the one filling and the one whose proposal is in
    // flight (Requirement 7.4).
    std::vector<std::byte> filling;
    filling.reserve(_config._chunk_bytes);
    std::optional<future_type> in_flight;
    std::vector<chunk_sha> chunk_list;
    cache_http_detail::sha256_stream whole;
    std::uint64_t total = 0;
    bool failed = false;

    auto settle = [&]() -> bool {
        if (!in_flight) {
            return true;
        }
        auto f = std::move(*in_flight);
        in_flight.reset();
        try {
            if (!f.wait(_config._command_timeout)) {
                return false;
            }
            (void)std::move(f).get();
            return true;
        } catch (const std::exception& e) {
            _logger.log(log_level::debug, "cache http: put_chunk failed", {{"error", e.what()}});
            return false;
        }
    };
    auto flush = [&]() -> bool {
        if (!settle()) {
            return false;
        }
        const auto sha = sha256_of(filling);
        chunk_list.push_back(sha);
        const auto command =
            encode_redis_kv_command(redis_kv_put_chunk_command{key, sha, std::move(filling)});
        filling = {};
        filling.reserve(_config._chunk_bytes);
        try {
            in_flight.emplace(_host.submit_command(key, command, _config._command_timeout));
        } catch (const std::exception& e) {
            _logger.log(log_level::debug, "cache http: put_chunk refused", {{"error", e.what()}});
            return false;
        }
        return true;
    };

    std::vector<char> scratch(std::min<std::size_t>(_config._chunk_bytes, 256 * 1024));
    while (!parser.is_done()) {
        req.body().data = scratch.data();
        req.body().size = scratch.size();
        cache_http_detail::lowest(stream).expires_after(_config._io_timeout);
        auto ec = cache_http_detail::drive(
            c._ioc, [&](auto h) { http::async_read(stream, buffer, parser, std::move(h)); });
        if (ec == http::error::need_buffer) {
            ec = {};
        }
        if (ec == http::error::body_limit) {
            ++_stats._too_large;
            (void)settle();
            return reply(c, stream, parser, http::status::payload_too_large);
        }
        if (ec) {
            // The client went away mid-body: no commit, the old value stays
            // (Requirement 7.5).
            ++_stats._aborted_puts;
            (void)settle();
            return after::close;
        }
        const std::size_t got = scratch.size() - req.body().size;
        std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(scratch.data()), got);
        whole.update(bytes);
        total += got;
        while (!bytes.empty() && !failed) {
            const auto room = _config._chunk_bytes - filling.size();
            const auto n = std::min(room, bytes.size());
            filling.insert(filling.end(), bytes.begin(), bytes.begin() + static_cast<long>(n));
            bytes = bytes.subspan(n);
            if (filling.size() == _config._chunk_bytes) {
                failed = !flush();
            }
        }
        if (failed) {
            break;
        }
    }
    if (!failed && !filling.empty()) {
        failed = !flush();
    }
    if (!failed) {
        failed = !settle();
    }
    if (failed) {
        ++_stats._aborted_puts;
        return reply(c, stream, parser, http::status::service_unavailable);
    }

    redis_kv_commit_command commit{key, 0, total, whole.finish(), std::move(chunk_list)};
    auto applied = propose(key, encode_redis_kv_command(commit), forward_to);
    if (!applied || applied->empty() || applied->front() != std::byte{1}) {
        // Not bound: leadership moved, or a chunk's grace ran out mid-upload
        // (design Component 2). The client retries or treats it as a miss.
        return reply(c, stream, parser, http::status::service_unavailable);
    }
    ++_stats._puts;
    _stats._put_bytes += total;
    if (_on_access) {
        _on_access(key, false);
    }
    return reply(c, stream, parser, http::status::created);
}

template<typename Host, typename Logger>
template<typename Stream>
auto cache_http_front_end<Host, Logger>::handle_delete(connection& c, Stream& stream,
                                                       boost::beast::flat_buffer& buffer,
                                                       request_parser& parser,
                                                       const std::string& key) -> after {
    namespace http = boost::beast::http;
    const bool forwarded = parser.get().count("X-Kythira-Forwarded") != 0;
    std::optional<node_id_type> forward_to;
    auto* node = locate(key, forward_to);
    if (node != nullptr && !node->is_leader()) {
        forward_to = node->known_leader();
        node = nullptr;
    }
    if (node == nullptr) {
        if (forward_to && !forwarded) {
            if (auto r = forward(c, stream, buffer, parser, *forward_to)) {
                return *r;
            }
        }
        if (!drain_body(c, stream, buffer, parser)) {
            return after::close;
        }
        return reply(c, stream, parser, http::status::service_unavailable);
    }
    if (!drain_body(c, stream, buffer, parser)) {
        return after::close;
    }
    auto existing = node->with_leader_state_machine([&key](auto& sm) { return sm.lookup(key); });
    if (!existing) {
        return reply(c, stream, parser, http::status::service_unavailable);
    }
    if (!*existing) {
        return reply(c, stream, parser, http::status::not_found);
    }
    if (!propose(key, encode_redis_kv_command(redis_kv_del_command{key}), forward_to)) {
        return reply(c, stream, parser, http::status::service_unavailable);
    }
    ++_stats._deletes;
    if (_on_access) {
        _on_access(key, true);
    }
    return reply(c, stream, parser, http::status::no_content);
}

template<typename Host, typename Logger>
template<typename Stream>
auto cache_http_front_end<Host, Logger>::handle_propfind(connection& c, Stream& stream,
                                                         request_parser& parser,
                                                         const std::string& key,
                                                         std::string_view raw_path) -> after {
    namespace http = boost::beast::http;
    std::string href(raw_path);
    auto name_of = [](std::string_view k) {
        while (!k.empty() && k.back() == '/') {
            k.remove_suffix(1);
        }
        auto slash = k.rfind('/');
        return std::string(slash == std::string_view::npos ? k : k.substr(slash + 1));
    };
    if (key.empty() || key.back() == '/') {
        // Any collection exists (design Component 6): OpenDAL then sends
        // neither the upward PROPFIND walk nor a MKCOL per level.
        return reply(c, stream, parser, static_cast<http::status>(207),
                     cache_http_detail::propfind_body(href, name_of(key), std::nullopt, {}),
                     R"(application/xml; charset="utf-8")");
    }
    std::optional<node_id_type> ignored;
    auto* node = locate(key, ignored);
    cache_manifest_ptr m;
    if (node != nullptr) {
        m = node->with_state_machine([&key](auto& sm) { return sm.lookup(key); });
    }
    if (!m) {
        return reply(c, stream, parser, http::status::not_found);
    }
    return reply(c, stream, parser, static_cast<http::status>(207),
                 cache_http_detail::propfind_body(href, name_of(key), m->_total_len,
                                                  "\"" + to_hex(m->_value_sha) + "\""),
                 R"(application/xml; charset="utf-8")");
}

template<typename Host, typename Logger>
template<typename Stream>
auto cache_http_front_end<Host, Logger>::handle_chunk(connection& c, Stream& stream,
                                                      request_parser& parser, std::string_view hex)
    -> after {
    namespace http = boost::beast::http;
    auto sha = chunk_sha_from_hex(hex);
    if (!sha) {
        return reply(c, stream, parser, http::status::bad_request);
    }
    auto bytes = _refs->store().read(*sha);
    if (!bytes) {
        return reply(c, stream, parser, http::status::not_found);
    }
    ++_stats._chunk_serves;
    return reply(c, stream, parser, http::status::ok,
                 std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()),
                 "application/octet-stream");
}

template<typename Host, typename Logger>
template<typename Stream>
auto cache_http_front_end<Host, Logger>::handle_metrics(connection& c, Stream& stream,
                                                        request_parser& parser) -> after {
    namespace http = boost::beast::http;
    std::ostringstream out;
    auto line = [&out](std::string_view name, std::uint64_t v) {
        out << "kythira_cache_http_" << name << ' ' << v << '\n';
    };
    line("requests", _stats._requests);
    line("hits", _stats._hits);
    line("misses", _stats._misses);
    line("puts", _stats._puts);
    line("put_bytes", _stats._put_bytes);
    line("get_bytes", _stats._get_bytes);
    line("deletes", _stats._deletes);
    line("forwards", _stats._forwards);
    line("forward_failures", _stats._forward_failures);
    line("auth_failures", _stats._auth_failures);
    line("auth_cache_hits", _auth_cache.hits());
    line("forbidden", _stats._forbidden);
    line("too_large", _stats._too_large);
    line("insufficient_storage", _stats._insufficient_storage);
    line("aborted_puts", _stats._aborted_puts);
    line("aborted_gets", _stats._aborted_gets);
    line("chunk_serves", _stats._chunk_serves);
    line("connections_current", _stats._connections_current);
    line("connections_rejected", _stats._connections_rejected);
    const auto store = _refs->store().stats();
    line("store_loose_files", store._loose_files);
    line("store_packed_chunks", store._packed_chunks);
    line("store_bytes_on_disk", store.bytes_on_disk());
    line("store_hash_failures", store._hash_failures);
    if (_metrics_extra) {
        out << _metrics_extra();
    }
    return reply(c, stream, parser, http::status::ok, out.str());
}

template<typename Host, typename Logger>
template<typename Stream>
auto cache_http_front_end<Host, Logger>::forward(connection& c, Stream& stream,
                                                 boost::beast::flat_buffer& buffer,
                                                 request_parser& parser, const node_id_type& to)
    -> std::optional<after> {
    namespace beast = boost::beast;
    namespace ssl = boost::asio::ssl;
    if (!_config._forwarding || !_resolver || to == _host.node_id()) {
        return std::nullopt;
    }
    if (!_config._forward_tls && !_config._allow_plaintext_forwarding) {
        return std::nullopt;
    }
    auto endpoint = _resolver(to);
    if (!endpoint) {
        return std::nullopt;
    }
    ++_stats._forwards;
    auto fail = [this]() -> std::optional<after> {
        ++_stats._forward_failures;
        return std::nullopt;
    };
    std::pair<std::string, std::uint16_t> hp;
    try {
        hp = cache_http_detail::parse_listen(*endpoint);
    } catch (const std::exception&) {
        return fail();
    }
    tcp::resolver resolver(c._ioc);
    boost::system::error_code ec;
    auto results = resolver.resolve(hp.first, std::to_string(hp.second), ec);
    if (ec) {
        return fail();
    }
    plain_stream up(c._ioc);
    up.expires_after(_config._io_timeout);
    ec = cache_http_detail::drive(c._ioc, [&](auto h) { up.async_connect(results, std::move(h)); });
    if (ec) {
        return fail();
    }
    bool written = false;
    bool ok = false;
    if (_config._forward_tls) {
        beast::ssl_stream<plain_stream> tls_up(std::move(up), *_forward_ssl_ctx);
        SSL_set_tlsext_host_name(tls_up.native_handle(), hp.first.c_str());
        tls_up.set_verify_callback(ssl::host_name_verification(hp.first));
        beast::get_lowest_layer(tls_up).expires_after(_config._handshake_timeout);
        ec = cache_http_detail::drive(c._ioc, [&](auto h) {
            tls_up.async_handshake(ssl::stream_base::client, std::move(h));
        });
        if (ec) {
            return fail();
        }
        ok = relay(c, stream, buffer, parser, tls_up, written);
    } else {
        ok = relay(c, stream, buffer, parser, up, written);
    }
    if (ok) {
        return after::close;  // one forwarded exchange per connection
    }
    ++_stats._forward_failures;
    if (written) {
        return after::close;  // the client already has part of an answer
    }
    return std::nullopt;
}

template<typename Host, typename Logger>
auto cache_http_front_end<Host, Logger>::fetch_chunk(const node_id_type& peer, const chunk_sha& sha)
    -> std::optional<std::vector<std::byte>> {
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace ssl = boost::asio::ssl;
    if (!_resolver || _config._internal_secret.empty() || peer == _host.node_id() ||
        (!_config._forward_tls && !_config._allow_plaintext_forwarding) ||
        (_config._forward_tls && !_forward_ssl_ctx)) {
        return std::nullopt;
    }
    auto endpoint = _resolver(peer);
    if (!endpoint) {
        return std::nullopt;
    }
    std::pair<std::string, std::uint16_t> hp;
    try {
        hp = cache_http_detail::parse_listen(*endpoint);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    boost::asio::io_context ioc{1};
    tcp::resolver resolver(ioc);
    boost::system::error_code ec;
    auto results = resolver.resolve(hp.first, std::to_string(hp.second), ec);
    if (ec) {
        return std::nullopt;
    }
    plain_stream up(ioc);
    up.expires_after(_config._io_timeout);
    ec = cache_http_detail::drive(ioc, [&](auto h) { up.async_connect(results, std::move(h)); });
    if (ec) {
        return std::nullopt;
    }
    http::request<http::empty_body> req{http::verb::get, "/_chunk/" + to_hex(sha), 11};
    req.set(http::field::host, hp.first);
    req.set(http::field::authorization, "Bearer " + _config._internal_secret);
    auto exchange = [&](auto& s) -> std::optional<std::vector<std::byte>> {
        cache_http_detail::lowest(s).expires_after(_config._io_timeout);
        auto wec =
            cache_http_detail::drive(ioc, [&](auto h) { http::async_write(s, req, std::move(h)); });
        if (wec) {
            return std::nullopt;
        }
        beast::flat_buffer buf;
        http::response_parser<http::string_body> res;
        res.body_limit(_refs->store().options()._max_chunk_bytes);
        cache_http_detail::lowest(s).expires_after(_config._io_timeout);
        auto rec = cache_http_detail::drive(
            ioc, [&](auto h) { http::async_read(s, buf, res, std::move(h)); });
        if (rec || res.get().result() != http::status::ok) {
            return std::nullopt;
        }
        const auto& body = res.get().body();
        std::vector<std::byte> out(body.size());
        if (!body.empty()) {
            std::memcpy(out.data(), body.data(), body.size());
        }
        return out;
    };
    if (_config._forward_tls) {
        beast::ssl_stream<plain_stream> tls_up(std::move(up), *_forward_ssl_ctx);
        SSL_set_tlsext_host_name(tls_up.native_handle(), hp.first.c_str());
        tls_up.set_verify_callback(ssl::host_name_verification(hp.first));
        beast::get_lowest_layer(tls_up).expires_after(_config._handshake_timeout);
        ec = cache_http_detail::drive(
            ioc, [&](auto h) { tls_up.async_handshake(ssl::stream_base::client, std::move(h)); });
        if (ec) {
            return std::nullopt;
        }
        return exchange(tls_up);
    }
    return exchange(up);
}

/// Send `parser`'s request (header, then its body as it arrives from the
/// client) to `up`, then copy the answer back, header then body. False if
/// it failed; `downstream_written` says whether the client saw any of it.
template<typename Host, typename Logger>
template<typename Down, typename Up>
auto cache_http_front_end<Host, Logger>::relay(connection& c, Down& down,
                                               boost::beast::flat_buffer& buffer,
                                               request_parser& parser, Up& up,
                                               bool& downstream_written) -> bool {
    namespace http = boost::beast::http;
    using cache_http_detail::drive;
    using cache_http_detail::lowest;
    const auto& in = parser.get();
    http::request<http::buffer_body> out{in.base()};
    out.set("X-Kythira-Forwarded", "1");
    // This node answers `Expect` itself; the leader gets the body anyway.
    out.erase(http::field::expect);
    out.keep_alive(false);
    http::request_serializer<http::buffer_body> sr{out};

    std::vector<char> scratch(64 * 1024);
    out.body().data = nullptr;
    out.body().more = !parser.is_done();
    lowest(up).expires_after(_config._io_timeout);
    auto ec = drive(c._ioc, [&](auto h) { http::async_write_header(up, sr, std::move(h)); });
    if (ec) {
        return false;
    }
    while (!parser.is_done()) {
        parser.get().body().data = scratch.data();
        parser.get().body().size = scratch.size();
        lowest(down).expires_after(_config._io_timeout);
        ec = drive(c._ioc, [&](auto h) { http::async_read(down, buffer, parser, std::move(h)); });
        if (ec == http::error::need_buffer) {
            ec = {};
        }
        if (ec) {
            return false;
        }
        out.body().data = scratch.data();
        out.body().size = scratch.size() - parser.get().body().size;
        out.body().more = true;
        lowest(up).expires_after(_config._io_timeout);
        ec = drive(c._ioc, [&](auto h) { http::async_write(up, sr, std::move(h)); });
        if (ec == http::error::need_buffer) {
            ec = {};
        }
        if (ec) {
            return false;
        }
    }
    out.body().data = nullptr;
    out.body().size = 0;
    out.body().more = false;
    lowest(up).expires_after(_config._io_timeout);
    ec = drive(c._ioc, [&](auto h) { http::async_write(up, sr, std::move(h)); });
    if (ec && ec != http::error::need_buffer) {
        return false;
    }

    // The answer: the leader may take as long as a commit takes.
    boost::beast::flat_buffer up_buffer;
    http::response_parser<http::buffer_body> rp;
    rp.body_limit(boost::none);
    if (in.method() == http::verb::head) {
        rp.skip(true);
    }
    lowest(up).expires_after(_config._io_timeout + _config._command_timeout);
    ec = drive(c._ioc, [&](auto h) { http::async_read_header(up, up_buffer, rp, std::move(h)); });
    if (ec) {
        return false;
    }
    http::response<http::buffer_body> res{rp.get().base()};
    res.keep_alive(false);
    http::response_serializer<http::buffer_body> rsr{res};
    res.body().data = nullptr;
    res.body().more = !rp.is_done();
    lowest(down).expires_after(_config._io_timeout);
    ec = drive(c._ioc, [&](auto h) { http::async_write_header(down, rsr, std::move(h)); });
    downstream_written = true;
    if (ec) {
        return false;
    }
    while (!rp.is_done()) {
        rp.get().body().data = scratch.data();
        rp.get().body().size = scratch.size();
        lowest(up).expires_after(_config._io_timeout);
        ec = drive(c._ioc, [&](auto h) { http::async_read(up, up_buffer, rp, std::move(h)); });
        if (ec == http::error::need_buffer) {
            ec = {};
        }
        if (ec) {
            return false;
        }
        res.body().data = scratch.data();
        res.body().size = scratch.size() - rp.get().body().size;
        res.body().more = true;
        lowest(down).expires_after(_config._io_timeout);
        ec = drive(c._ioc, [&](auto h) { http::async_write(down, rsr, std::move(h)); });
        if (ec == http::error::need_buffer) {
            ec = {};
        }
        if (ec) {
            return false;
        }
    }
    res.body().data = nullptr;
    res.body().size = 0;
    res.body().more = false;
    lowest(down).expires_after(_config._io_timeout);
    ec = drive(c._ioc, [&](auto h) { http::async_write(down, rsr, std::move(h)); });
    return !ec || ec == http::error::need_buffer;
}

}  // namespace kythira
