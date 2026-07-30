#pragma once

/// @file beast_http_transport.hpp
/// @brief A second `kythira::network_client`/`network_server` implementation,
///     backed by Boost.Beast's asynchronous HTTP API driven by a
///     caller-owned `net::io_context`, alongside the existing
///     cpp-httplib-backed transport (`include/raft/http_transport.hpp`).
///     See `.kiro/specs/boost-beast-http-transport/` for the full design.
///
/// Unlike `cpp_httplib_client`/`cpp_httplib_server` (and `tcp_rpc.hpp`/
/// `tls_tcp_rpc.hpp`), this transport does not block a calling thread (or an
/// `executor_type` slot) per in-flight RPC — every connect/write/read step is
/// a genuine asynchronous Beast operation, and progress only happens while a
/// caller-owned thread is running `net::io_context::run()` on the
/// `io_context&` passed to the constructor (Requirement 8). `stop()` never
/// stops or destroys that `io_context` itself; that remains the caller's own,
/// separate responsibility once nothing else needs it.
///
/// Requirement 19: this feature's `Types` bundle is narrower than
/// `transport_types` alone requires -- `Types::template future_template<T>`
/// must be `kythira::future_default<T>` specifically (not merely any type
/// satisfying `future`), because the async bridge
/// (`beast_detail::async_connect_kf`/`async_write_kf`/`async_read_kf`, below)
/// is composed via `kythira::promise_default<T>` and
/// `future_transformable`'s `thenValue`/`thenError` chaining
/// (`include/concepts/future.hpp`). `future_default_http_transport_types`
/// below is the canonical bundle satisfying this constraint -- the same
/// pattern already used ad hoc, per test file, in this project's CoAP
/// property tests, just formalized. `kythira::http_transport_types` itself is
/// unmodified.

#include <raft/types.hpp>
#include <raft/network.hpp>
#include <raft/http_exceptions.hpp>
#include <raft/metrics.hpp>
#include <raft/future_default.hpp>
#include <concepts/future.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kythira {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace beast_http = boost::beast::http;

/// @brief `Types` bundle whose `future_template` is pinned to
///     `kythira::future_default<T>` (Requirement 19) -- required by
///     `boost_beast_client`/`boost_beast_server`, not by `transport_types`
///     in general. Additive: does not replace or modify
///     `kythira::http_transport_types`.
template<typename RPC_Serializer, typename Metrics, typename Executor>
struct future_default_http_transport_types {
    template<typename T> using future_template = kythira::future_default<T>;
    using serializer_type = RPC_Serializer;
    using metrics_type = Metrics;
    using executor_type = Executor;
};

/// @brief Requirement 19.4: `boost_beast_client`/`boost_beast_server`
///     require `Types::future_template<T>` to be `kythira::future_default<T>`
///     specifically, not merely any type satisfying `future`. Checked via
///     `kythira::unit` as a representative `T` -- `future_template` is a
///     template alias, uniformly defined for every `T` a real `Types` bundle
///     is used with, so one instantiation is a faithful proxy for all of
///     them. Named separately from `transport_types` (which this subsumes)
///     so a `Types` bundle that fails this narrower check gets a clear,
///     named concept violation at the class template's own instantiation
///     point, not a substitution failure several calls deep inside
///     `send_rpc`'s `promise_default`/`thenValue` usage.
template<typename Types>
concept future_default_transport_types =
    kythira::transport_types<Types> &&
    std::same_as<typename Types::template future_template<kythira::unit>,
                 kythira::future_default<kythira::unit>>;

/// @brief Client configuration. Field-for-field match with
///     `cpp_httplib_client_config` (Requirement 11) -- no new data model, so
///     an existing config literal is reusable verbatim across a type-only
///     transport swap. Unlike cpp-httplib, `boost::asio::ssl::context` gives
///     full control over every one of these fields (cpp-httplib's own
///     `configure_ssl_client` has to leave several of them
///     validated-but-not-fully-applied -- see `http_transport_impl.hpp`).
struct boost_beast_client_config {
    std::size_t connection_pool_size{10};
    std::chrono::milliseconds connection_timeout{5000};
    std::chrono::milliseconds request_timeout{10000};
    std::chrono::milliseconds keep_alive_timeout{60000};
    bool enable_ssl_verification{true};
    std::string ca_cert_path{};
    std::string client_cert_path{};
    std::string client_key_path{};
    std::string cipher_suites{};
    std::string min_tls_version{"TLSv1.2"};
    std::string max_tls_version{"TLSv1.3"};
    std::string user_agent{"raft-boost-beast/1.0"};
};

/// @brief Server configuration. Field-for-field match with
///     `cpp_httplib_server_config` (Requirement 11).
struct boost_beast_server_config {
    std::size_t max_concurrent_connections{100};
    std::size_t max_request_body_size{10 * 1024 * 1024};  // 10 MB
    std::chrono::seconds request_timeout{30};
    bool enable_ssl{false};
    std::string ssl_cert_path{};
    std::string ssl_key_path{};
    std::string ca_cert_path{};
    bool require_client_cert{false};
    std::string cipher_suites{};
    std::string min_tls_version{"TLSv1.2"};
    std::string max_tls_version{"TLSv1.3"};
};

/// @brief The three primitive async-to-future adaptors (Requirement 14.2,
///     14.3) plus the small amount of connection-object scaffolding built
///     directly on top of them. Each `async_*_kf` function wraps exactly one
///     Beast asynchronous operation: constructs a
///     `kythira::promise_default<T>`, issues the operation with a completion
///     handler that calls `setValue`/`setException`, and returns
///     `promise.getFuture()` immediately -- the function itself never
///     blocks; the `io_context` thread that eventually runs the completion
///     handler is what actually fulfills the promise (Property 2).
namespace beast_detail {

/// @brief `beast::tcp_stream::async_connect`, bridged to a future.
auto async_connect_kf(beast::tcp_stream& stream, net::ip::tcp::endpoint ep)
    -> kythira::future_default<kythira::unit>;

/// @brief TLS client handshake, bridged to a future. Only meaningful on a
///     `beast::ssl_stream<beast::tcp_stream>` whose underlying `tcp_stream`
///     is already connected.
auto async_client_handshake_kf(beast::ssl_stream<beast::tcp_stream>& stream)
    -> kythira::future_default<kythira::unit>;

/// @brief TLS server handshake, bridged to a future.
auto async_server_handshake_kf(beast::ssl_stream<beast::tcp_stream>& stream)
    -> kythira::future_default<kythira::unit>;

/// @brief `beast_http::async_write`, bridged to a future. Templated over
///     `Stream` (so the identical adaptor serves both `beast::tcp_stream`
///     and `beast::ssl_stream<beast::tcp_stream>`) and over the message type
///     itself (so the identical adaptor serves the client writing a
///     `request<string_body>` and the server writing a
///     `response<string_body>`) -- mirroring Beast's own `async_write`,
///     which is generic over any `beast_http::message<isRequest, Body,
///     Fields>`.
template<typename Stream, bool IsRequest, typename Body, typename Fields>
auto async_write_kf(Stream& stream, const beast_http::message<IsRequest, Body, Fields>& message)
    -> kythira::future_default<kythira::unit>;

/// @brief `beast_http::async_read`, bridged to a future.
template<typename Stream, bool IsRequest, typename Body, typename Fields>
auto async_read_kf(Stream& stream, beast::flat_buffer& buffer,
                   beast_http::message<IsRequest, Body, Fields>& message)
    -> kythira::future_default<kythira::unit>;

/// @brief `beast_http::async_read`, bridged to a future -- overload taking a
///     `beast_http::parser` (rather than a bare `message`) so a caller can
///     call `.body_limit(n)` on it before the read starts. The plain-message
///     overload above has no way to configure a body limit externally (Beast
///     wraps the message in a temporary parser internally, with no caller
///     access to it), which is why `boost_beast_server_config::
///     max_request_body_size` needs this overload to actually be enforced,
///     not just validated as a config field. Used by `server_session` (the
///     only caller that needs a configurable limit); the client's response
///     read keeps using the plain-message overload, matching cpp-httplib's
///     own client, which does not itself cap response body size.
template<typename Stream, bool IsRequest, typename Body, typename Fields>
auto async_read_kf(Stream& stream, beast::flat_buffer& buffer,
                   beast_http::parser<IsRequest, Body, Fields>& parser)
    -> kythira::future_default<kythira::unit>;

/// @brief Type-erased connection handle so `boost_beast_client`'s connection
///     pool can hold a single map regardless of whether a given node's URL
///     was `http://` or `https://`. `connect()` performs the TCP connect
///     (and, for TLS, the handshake) if not already connected; `send()`
///     writes `request` and reads exactly one response back, both via the
///     `async_*_kf` primitives above composed with `thenValue` (Requirement
///     14.3). `is_open()`/`set_timeout()`/`close()` are implemented via
///     `beast::get_lowest_layer()`, which resolves to the same underlying
///     `beast::tcp_stream` whether or not a TLS layer sits on top of it --
///     the one piece of Beast machinery that lets this interface stay
///     transport-agnostic without a `std::variant`/`std::visit`.
class beast_connection {
public:
    virtual ~beast_connection() = default;

    [[nodiscard]] virtual auto is_open() const -> bool = 0;
    virtual auto set_timeout(std::chrono::milliseconds timeout) -> void = 0;
    virtual auto connect(const net::ip::tcp::endpoint& ep, const std::string& host)
        -> kythira::future_default<kythira::unit> = 0;
    virtual auto send(beast_http::request<beast_http::string_body> request)
        -> kythira::future_default<beast_http::response<beast_http::string_body>> = 0;
    virtual auto close() -> void = 0;
};

/// @brief `beast_connection` over a plain `beast::tcp_stream`.
class plain_beast_connection final : public beast_connection {
public:
    explicit plain_beast_connection(beast::tcp_stream stream);

    [[nodiscard]] auto is_open() const -> bool override;
    auto set_timeout(std::chrono::milliseconds timeout) -> void override;
    auto connect(const net::ip::tcp::endpoint& ep, const std::string& host)
        -> kythira::future_default<kythira::unit> override;
    auto send(beast_http::request<beast_http::string_body> request)
        -> kythira::future_default<beast_http::response<beast_http::string_body>> override;
    auto close() -> void override;

private:
    beast::tcp_stream _stream;
    // basic_stream's timeout (set via expires_after()) covers only the
    // *next* logical read/write/connect operation issued after it's armed --
    // send_rpc() calls set_timeout() once, before connect(), but connect()
    // and send()'s write+read are separate logical operations, so the
    // deadline armed for connect() does not carry over: send() must re-arm
    // it immediately before issuing the write, or the write is treated as
    // already past an unset deadline and fails instantly.
    std::chrono::milliseconds _timeout{};
};

/// @brief `beast_connection` over `beast::ssl_stream<beast::tcp_stream>`.
class tls_beast_connection final : public beast_connection {
public:
    explicit tls_beast_connection(beast::ssl_stream<beast::tcp_stream> stream);

    [[nodiscard]] auto is_open() const -> bool override;
    auto set_timeout(std::chrono::milliseconds timeout) -> void override;
    auto connect(const net::ip::tcp::endpoint& ep, const std::string& host)
        -> kythira::future_default<kythira::unit> override;
    auto send(beast_http::request<beast_http::string_body> request)
        -> kythira::future_default<beast_http::response<beast_http::string_body>> override;
    auto close() -> void override;

private:
    beast::ssl_stream<beast::tcp_stream> _stream;
    // See plain_beast_connection::_timeout above -- same reason.
    std::chrono::milliseconds _timeout{};
};

}  // namespace beast_detail

/// @brief `network_client` implementation backed by Boost.Beast's
///     asynchronous API. Both `boost_beast_client` and `boost_beast_server`
///     take a `net::io_context&` and never own one (Requirement 8): no
///     progress happens without a caller-run `io_context::run()` thread, and
///     nothing in this class ever calls `_ioc.stop()`.
///
/// One persistent connection per target node is kept in `_connections`,
/// reused across RPCs (Requirement 9) the same way `cpp_httplib_client`'s own
/// `_http_clients` map reuses `httplib::Client` instances. Each pooled
/// connection's own `beast::tcp_stream`/`beast::ssl_stream<beast::tcp_stream>`
/// is constructed on a `net::strand` (`net::make_strand(ioc)`), so every
/// asynchronous operation issued on it is automatically serialized with
/// respect to every other operation on that same connection -- concurrent
/// RPCs to the *same* target queue on that strand, while RPCs to *different*
/// targets, each with their own connection and strand, run genuinely
/// concurrently (Property 6). `_connections_mutex` only guards the map's own
/// structure (insert/erase), not a connection's I/O -- narrow lock scope,
/// matching `cpp_httplib_client`'s own `_mutex` usage.
template<typename Types>
requires kythira::future_default_transport_types<Types>
class boost_beast_client {
public:
    using serializer_type = typename Types::serializer_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;
    template<typename T> using future_template = typename Types::template future_template<T>;

    boost_beast_client(net::io_context& ioc,
                       std::unordered_map<std::uint64_t, std::string> node_id_to_url_map,
                       boost_beast_client_config config, metrics_type metrics);
    ~boost_beast_client();

    boost_beast_client(const boost_beast_client&) = delete;
    auto operator=(const boost_beast_client&) -> boost_beast_client& = delete;
    boost_beast_client(boost_beast_client&&) = delete;
    auto operator=(boost_beast_client&&) -> boost_beast_client& = delete;

    auto send_request_vote(std::uint64_t target, const kythira::request_vote_request<>& request,
                           std::chrono::milliseconds timeout)
        -> future_template<kythira::request_vote_response<>>;

    auto send_append_entries(std::uint64_t target, const kythira::append_entries_request<>& request,
                             std::chrono::milliseconds timeout)
        -> future_template<kythira::append_entries_response<>>;

    auto send_install_snapshot(std::uint64_t target,
                               const kythira::install_snapshot_request<>& request,
                               std::chrono::milliseconds timeout)
        -> future_template<kythira::install_snapshot_response<>>;

    /// @brief Validates the configured TLS material, then retires every
    ///     pooled connection so subsequent RPCs build fresh ones using the
    ///     reloaded material. Retired connections are kept alive (not
    ///     destroyed) -- a concurrent in-flight RPC may still hold a
    ///     reference obtained from `get_or_create_connection()` before the
    ///     reload (Property 7, Requirement 7.3).
    auto reload_tls_material() -> void;

    /// @brief Starts a background thread that polls `client_cert_path`'s
    ///     mtime every `poll_interval` and calls `reload_tls_material()`
    ///     when it has changed.
    auto enable_auto_reload(std::chrono::seconds poll_interval) -> void;

    /// @brief Stops the auto-reload background thread cleanly (joined, not
    ///     detached).
    auto disable_auto_reload() -> void;

private:
    struct pooled_connection {
        std::unique_ptr<beast_detail::beast_connection> connection;
        net::ip::tcp::endpoint endpoint;
        std::string host_header;
        std::chrono::steady_clock::time_point last_used;
    };

    net::io_context& _ioc;
    serializer_type _serializer;
    std::unordered_map<std::uint64_t, std::string> _node_id_to_url;
    boost_beast_client_config _config;
    metrics_type _metrics;
    std::optional<net::ssl::context> _ssl_ctx;
    mutable std::mutex _mutex;
    std::unordered_map<std::uint64_t, pooled_connection> _connections;
    std::vector<std::unique_ptr<beast_detail::beast_connection>> _retired_connections;
    std::jthread _auto_reload_thread;
    std::filesystem::file_time_type _last_reloaded_cert_mtime{};

    // Tracks RPCs with a live connect/handshake/send/read still in flight on
    // an io_context worker thread, so the destructor can wait for all of
    // them to finish (their completion handler running is what decrements
    // this) before _connections/_ssl_ctx are torn down -- the client-side
    // equivalent of boost_beast_server::stop()'s Property 8 drain. Without
    // this, ~boost_beast_client() could destroy _ssl_ctx while a worker
    // thread was still mid SSL_do_handshake() on a connection referencing
    // it, a genuine (TSan-caught, not a false positive) use-after-free-shaped
    // race, not merely a theoretical one.
    mutable std::mutex _drain_mutex;
    std::condition_variable _drain_cv;
    std::size_t _in_flight_operations{0};

    /// RAII: increments _in_flight_operations on construction, decrements
    /// (and notifies _drain_cv once it reaches zero) on destruction. Held via
    /// shared_ptr across a send_rpc call's whole thenValue/thenError chain so
    /// whichever branch actually runs releases the last reference.
    struct in_flight_guard {
        boost_beast_client* client;

        explicit in_flight_guard(boost_beast_client* c) : client(c) {
            std::lock_guard<std::mutex> lock(client->_drain_mutex);
            ++client->_in_flight_operations;
        }

        in_flight_guard(const in_flight_guard&) = delete;
        auto operator=(const in_flight_guard&) -> in_flight_guard& = delete;
        in_flight_guard(in_flight_guard&&) = delete;
        auto operator=(in_flight_guard&&) -> in_flight_guard& = delete;

        ~in_flight_guard() {
            std::lock_guard<std::mutex> lock(client->_drain_mutex);
            if (--client->_in_flight_operations == 0) {
                client->_drain_cv.notify_all();
            }
        }
    };

    auto validate_certificate_files() const -> void;
    auto load_client_certificates() -> void;
    auto build_ssl_context() -> net::ssl::context;
    auto get_or_create_connection(std::uint64_t target) -> pooled_connection&;
    auto remove_connection(std::uint64_t target) -> void;

    template<typename Request, typename Response>
    auto send_rpc(std::uint64_t target, std::string_view endpoint, const Request& request,
                  std::chrono::milliseconds timeout) -> future_template<Response>;
};

/// @brief `network_server` implementation backed by Boost.Beast's
///     asynchronous API. See `boost_beast_client`'s own comment for the
///     shared `io_context` ownership contract (Requirement 8) -- it applies
///     identically here. `stop()` closes the listening acceptor and blocks
///     until every in-flight session has finished (Property 8), without
///     itself touching `_ioc`.
///
/// No dedicated "server thread" the way `cpp_httplib_server` has one --
/// accepting and serving both happen on whichever `io_context` threads the
/// caller is running. Each accepted connection becomes a
/// `beast_detail::server_session<Types>` (self-managing lifetime via
/// `shared_from_this`, the standard Beast server pattern), and a single
/// connection's I/O error never stops `do_accept()` from continuing to serve
/// other connections.
template<typename Types>
requires kythira::future_default_transport_types<Types>
class boost_beast_server {
public:
    using serializer_type = typename Types::serializer_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;
    template<typename T> using future_template = typename Types::template future_template<T>;

    boost_beast_server(net::io_context& ioc, std::string bind_address, std::uint16_t bind_port,
                       boost_beast_server_config config, metrics_type metrics);
    ~boost_beast_server();

    boost_beast_server(const boost_beast_server&) = delete;
    auto operator=(const boost_beast_server&) -> boost_beast_server& = delete;
    boost_beast_server(boost_beast_server&&) = delete;
    auto operator=(boost_beast_server&&) -> boost_beast_server& = delete;

    auto register_request_vote_handler(
        std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)>
            handler) -> void;

    auto register_append_entries_handler(
        std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
            handler) -> void;

    auto register_install_snapshot_handler(std::function<kythira::install_snapshot_response<>(
                                               const kythira::install_snapshot_request<>&)>
                                               handler) -> void;

    auto start() -> void;
    auto stop() -> void;
    [[nodiscard]] auto is_running() const -> bool;

    /// @brief Re-reads `ssl_cert_path`/`ssl_key_path`/`ca_cert_path` and
    ///     builds a *new* `net::ssl::context`, applied to subsequently
    ///     accepted connections only -- an in-flight
    ///     `beast::ssl_stream<beast::tcp_stream>` holds a reference into
    ///     whichever context was live when it was constructed, so it is
    ///     unaffected by a reload (Property 7). All-or-nothing: on
    ///     validation failure, throws and the server keeps serving its
    ///     previous, still-valid material.
    auto reload_tls_material() -> void;

    /// @brief Starts a background thread that polls `ssl_cert_path`'s mtime
    ///     every `poll_interval` and calls `reload_tls_material()` when it
    ///     has changed. A failed automatic reload is reported via
    ///     `metrics_type` and does not stop the poll loop.
    auto enable_auto_reload(std::chrono::seconds poll_interval) -> void;

    /// @brief Stops the auto-reload background thread cleanly (joined, not
    ///     detached).
    auto disable_auto_reload() -> void;

    /// @brief Called by `beast_detail::server_session` (not part of
    ///     `network_server`, but public rather than befriending a template
    ///     whose second parameter varies per stream type) when a session
    ///     starts/finishes, and to route one parsed request to the matching
    ///     registered handler. Exposed here rather than made `private` +
    ///     `friend`ed because a session's stream type (plain vs. TLS) varies
    ///     independently of `Types`, which would require friending every
    ///     instantiation individually.
    ///
    /// `register_session`/`session_finished` bracket a session's lifetime
    /// (replacing a plain start/finish pair): `stop()` needs a way to reach
    /// a session that is sitting idle on a keep-alive connection with no
    /// request actually in flight -- Property 8 only promises in-flight
    /// *requests* get to finish, not that idle established connections stay
    /// open indefinitely, so `stop()` actively closes them (via `closer`)
    /// rather than waiting forever for a request that will never arrive.
    auto register_session(std::function<void()> closer) -> std::size_t;
    auto session_finished(std::size_t session_id) -> void;
    auto dispatch(std::string_view target, const std::vector<std::byte>& body,
                  std::string& response_body, unsigned& status_code) -> void;

private:
    net::io_context& _ioc;
    serializer_type _serializer;
    std::string _bind_address;
    std::uint16_t _bind_port;
    boost_beast_server_config _config;
    metrics_type _metrics;
    std::shared_ptr<net::ssl::context> _ssl_ctx;
    net::ip::tcp::acceptor _acceptor;
    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)>
        _request_vote_handler;
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
        _append_entries_handler;
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)>
        _install_snapshot_handler;
    std::atomic<bool> _running{false};
    std::mutex _sessions_mutex;
    std::unordered_map<std::size_t, std::function<void()>> _session_closers;
    std::size_t _next_session_id{0};
    std::mutex _drain_mutex;
    std::condition_variable _drain_cv;
    std::mutex _mutex;
    std::jthread _auto_reload_thread;
    std::filesystem::file_time_type _last_reloaded_cert_mtime{};

    auto validate_certificate_files() const -> void;
    auto load_server_certificates() -> void;
    auto build_ssl_context() -> net::ssl::context;
    auto do_accept() -> void;
};

}  // namespace kythira
