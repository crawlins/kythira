// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

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
///
/// Requirement 19 pins `future_template<T>` to `future_default<T>`, but does
/// NOT pin which backend `future_default<T>` itself resolves to: this
/// transport works under all three of `KYTHIRA_DEFAULT_FUTURE_BACKEND`'s
/// values (`folly`, `stdexec`, `boost`). `async_connect_kf`/
/// `async_client_handshake_kf`/`async_server_handshake_kf`/`async_write_kf`/
/// `async_read_kf` `.via()` their returned future onto an
/// `asio_strand_executor`, which is backend-conditional in the same
/// three-way shape as `kythira::executor_default` -- see its own comment
/// below for the full why, including the silent-UB failure mode that made
/// this Folly-only until it was fixed.
///
/// The `InlineExecutor`-on-an-already-fulfilled-promise hazard that
/// `asio_strand_executor` exists to close is a Folly-specific mechanism, but
/// the property it restores -- every continuation runs on the stream's own
/// strand, never on whichever thread happened to fulfill the promise -- is
/// one this transport needs from every backend, since it is Beast's
/// `basic_stream` state (not Folly's) being serialized. Re-`via()`-ing onto
/// the strand establishes that property uniformly rather than relying on any
/// backend's default continuation affinity.

#include <raft/types.hpp>
#include <raft/network.hpp>
#include <raft/http_exceptions.hpp>
#include <raft/metrics.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/http_content_negotiation.hpp>
#include <raft/peer_capability_cache.hpp>
#include <raft/future_default.hpp>
#include <concepts/future.hpp>
// Backend-conditional, mirroring include/raft/executor_default.hpp's own
// three-way split: asio_strand_executor (below) adapts an Asio strand to
// whichever executor concept the *selected* future backend's `via()` takes,
// so only that backend's header is needed here.
#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
#include <raft/future_stdexec.hpp>
#elif defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <boost/thread/executors/generic_executor_ref.hpp>
#else
#include <folly/Executor.h>
#endif

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
    using serializer_registry_type = single_serializer_registry<RPC_Serializer>;
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
    /// Maximum idle connections retained **per target**. Connections are
    /// checked out exclusively for one RPC, so this bounds what is kept for
    /// reuse, not what may be in flight at once. (It previously bounded the
    /// number of distinct targets, which is not what the name says and evicted
    /// an unrelated node's connection when the eleventh peer was first
    /// contacted.)
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

/// Defined further down (its definition needs the backend-conditional
/// machinery), but named in every async_*_kf signature below.
class asio_strand_executor;

/// @brief `beast::tcp_stream::async_connect`, bridged to a future, re-via'd
///     onto `executor` (the calling connection/session's own
///     `asio_strand_executor`, below) before returning -- see that class's
///     doc comment for why every async_*_kf helper takes one.
auto async_connect_kf(beast::tcp_stream& stream, net::ip::tcp::endpoint ep,
                      asio_strand_executor* executor) -> kythira::future_default<kythira::unit>;

/// @brief TLS client handshake, bridged to a future. Only meaningful on a
///     `beast::ssl_stream<beast::tcp_stream>` whose underlying `tcp_stream`
///     is already connected.
auto async_client_handshake_kf(beast::ssl_stream<beast::tcp_stream>& stream,
                               asio_strand_executor* executor)
    -> kythira::future_default<kythira::unit>;

/// @brief TLS server handshake, bridged to a future.
auto async_server_handshake_kf(beast::ssl_stream<beast::tcp_stream>& stream,
                               asio_strand_executor* executor)
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
auto async_write_kf(Stream& stream, const beast_http::message<IsRequest, Body, Fields>& message,
                    asio_strand_executor* executor) -> kythira::future_default<kythira::unit>;

/// @brief `beast_http::async_read`, bridged to a future.
template<typename Stream, bool IsRequest, typename Body, typename Fields>
auto async_read_kf(Stream& stream, beast::flat_buffer& buffer,
                   beast_http::message<IsRequest, Body, Fields>& message,
                   asio_strand_executor* executor) -> kythira::future_default<kythira::unit>;

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
                   beast_http::parser<IsRequest, Body, Fields>& parser,
                   asio_strand_executor* executor) -> kythira::future_default<kythira::unit>;

/// @brief Adapts a stream's own executor (`net::any_io_executor`, in
///     practice always the strand it was constructed on -- see
///     `boost_beast_client`'s class comment) as whatever executor type the
///     *selected* future backend's `via()` accepts, so a
///     `kythira::future_default<T>` can be `.via()`'d onto it regardless of
///     which backend `KYTHIRA_DEFAULT_FUTURE_BACKEND` selected.
///
///     `via()` is deliberately NOT unified across backends (see
///     `include/raft/executor_default.hpp`'s own header comment): Folly's
///     takes a `folly::Executor&`, stdexec's a
///     `stdexec_backend::scheduler_handle&`, and Boost.Thread's a duck-typed
///     `Ex&` with a `submit`/`close`/`closed`/`try_executing_one` interface.
///     So this class is backend-conditional in exactly the same three-way
///     shape as `kythira::executor_default`, and exposes the same `handle()`
///     accessor returning the backend-native reference -- callers write
///     `future.via(executor->handle())` and stay backend-agnostic.
///
///     This previously derived from `folly::Executor` unconditionally, which
///     made the whole Beast transport Folly-only. That was a silent failure
///     rather than a loud one: `future_continuation` requires a
///     `via(void*)` overload (`include/concepts/future.hpp`), a
///     `folly::Executor*` converts implicitly to `void*`, and the stdexec
///     backend's `via(void*)` `static_cast`s straight to `scheduler_handle*`
///     -- reinterpreting this object's vtable pointer as a `shared_ptr`.
///     Every `beast_*` test segfaulted under
///     `KYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec` for exactly that reason, and
///     failed to compile under `=boost`. Passing a backend-native handle
///     rather than a `void*` is what keeps that from being expressible.
///
///     `kythira::Promise<T>::getFuture()` (future.hpp) vias its returned
///     future onto `folly::InlineExecutor::instance()`, a process-wide
///     singleton with no thread affinity of its own: a `.thenValue()`
///     continuation attached to such a future runs immediately, inline, on
///     whichever thread wins the race to attach it *after* the promise is
///     already fulfilled -- which need not be the thread that fulfilled it,
///     and need not be "on" the stream's strand at all, even though the
///     asio operation that fulfilled the promise was itself dispatched
///     through that strand. A TSan run confirmed this is not hypothetical:
///     a continuation re-arming a stream's `expires_after()` deadline raced
///     that same stream's own `pending_guard::reset()` from the connect
///     operation that had "just completed" -- both touching the identical
///     `basic_stream::impl_type` state, with no synchronization edge
///     between them recognized by the compiler, because none actually
///     existed at the language level. Re-`.via()`-ing every async_*_kf
///     future onto an `asio_strand_executor` wrapping that same stream's
///     executor closes this at its root: `add()` posts onto the stream's
///     own strand, so every continuation chained onto such a future is
///     forced through the same serialization point as every other
///     operation Beast itself performs on that stream, regardless of which
///     thread happens to fulfill the underlying promise or when.
#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)

/// @brief An stdexec `scheduler` whose `schedule()` sender completes on an
///     Asio executor. `stdexec_backend::scheduler_handle` type-erases any
///     type satisfying stdexec's own `scheduler` concept, so this is all
///     that is needed to make an Asio strand a valid `via()` target.
class asio_strand_scheduler {
public:
    explicit asio_strand_scheduler(net::any_io_executor ex) : _ex(std::move(ex)) {}

    class schedule_sender {
    public:
        using sender_concept = ::stdexec::sender_t;
        using completion_signatures = ::stdexec::completion_signatures<::stdexec::set_value_t()>;

        explicit schedule_sender(net::any_io_executor ex) : _ex(std::move(ex)) {}

        /// Operation states are pinned once connected, so `start()` may
        /// capture `this` and complete the receiver in place from the
        /// posted handler rather than moving the receiver into the lambda.
        template<typename Receiver> class operation_state {
        public:
            operation_state(net::any_io_executor ex, Receiver receiver)
                : _ex(std::move(ex)), _receiver(std::move(receiver)) {}

            operation_state(const operation_state&) = delete;
            auto operator=(const operation_state&) -> operation_state& = delete;
            operation_state(operation_state&&) = delete;
            auto operator=(operation_state&&) -> operation_state& = delete;
            ~operation_state() = default;

            auto start() noexcept -> void {
                net::post(_ex, [this]() { ::stdexec::set_value(std::move(_receiver)); });
            }

        private:
            net::any_io_executor _ex;
            Receiver _receiver;
        };

        template<typename Receiver>
        auto connect(Receiver receiver) const -> operation_state<Receiver> {
            return operation_state<Receiver>(_ex, std::move(receiver));
        }

        /// stdexec's `scheduler` concept requires
        /// `get_completion_scheduler<set_value_t>(get_env(schedule(s))) == s`.
        struct env {
            net::any_io_executor _ex;

            [[nodiscard]] auto query(::stdexec::get_completion_scheduler_t<::stdexec::set_value_t>)
                const noexcept -> asio_strand_scheduler {
                return asio_strand_scheduler(_ex);
            }
        };

        [[nodiscard]] auto get_env() const noexcept -> env { return env{_ex}; }

    private:
        net::any_io_executor _ex;
    };

    using scheduler_concept = ::stdexec::scheduler_t;

    [[nodiscard]] auto schedule() const noexcept -> schedule_sender { return schedule_sender(_ex); }

    auto operator==(const asio_strand_scheduler& other) const -> bool { return _ex == other._ex; }

private:
    net::any_io_executor _ex;
};

class asio_strand_executor {
public:
    /// @param context_keeper  see the Folly specialisation below for what this
    ///     is for. Accepted on every backend so that callers need not be
    ///     backend-conditional; only the Folly one can actually outlive its
    ///     owner, but holding the keeper here too costs one `shared_ptr` and
    ///     keeps the ownership rule the same on all three.
    explicit asio_strand_executor(net::any_io_executor ex,
                                  std::shared_ptr<void> context_keeper = {})
        : _context_keeper(std::move(context_keeper)),
          _handle(asio_strand_scheduler(std::move(ex))) {}

    auto handle() -> kythira::stdexec_backend::scheduler_handle& { return _handle; }

private:
    // Declared before `_handle`, which owns the Asio executor -- see the Folly
    // specialisation's `_context_keeper` for why the order matters.
    std::shared_ptr<void> _context_keeper;
    kythira::stdexec_backend::scheduler_handle _handle;
};

#elif defined(KYTHIRA_FUTURE_BACKEND_BOOST)

/// Boost.Thread's `then(Ex&, F)` wraps its executor in
/// `boost::executors::executor_ref<Ex>`, which duck-types exactly four
/// members -- it never requires deriving from `boost::executors::executor`.
/// `try_executing_one()` returns false because work posted here runs on the
/// caller-owned `io_context` thread(s), never on a thread that has borrowed
/// this executor to drain it.
class asio_strand_executor {
public:
    /// @param context_keeper  see the Folly specialisation below -- accepted on
    ///     every backend so callers need not be backend-conditional.
    explicit asio_strand_executor(net::any_io_executor ex,
                                  std::shared_ptr<void> context_keeper = {})
        : _context_keeper(std::move(context_keeper)), _ex(std::move(ex)) {}

    auto handle() -> asio_strand_executor& { return *this; }

    template<typename Work> auto submit(Work&& work) -> void {
        net::post(_ex, std::forward<Work>(work));
    }

    auto close() -> void { _closed.store(true, std::memory_order_release); }

    auto closed() -> bool { return _closed.load(std::memory_order_acquire); }

    auto try_executing_one() -> bool { return false; }

private:
    // Declared before `_ex` -- see the Folly specialisation's `_context_keeper`.
    std::shared_ptr<void> _context_keeper;
    net::any_io_executor _ex;
    std::atomic<bool> _closed{false};
};

#else

/// Held by `shared_ptr` and self-pinned for as long as Folly holds a
/// `KeepAlive` to it, which is what lets the object that owns it be destroyed
/// first.
///
/// Folly's future cores keep a `KeepAlive<Executor>` alive past the callbacks
/// they schedule, and use it to `add()` the *next* link of a chain from inside
/// `setResult_`. When this executor was a plain by-value member of a connection,
/// releasing that connection from one of its own continuations destroyed the
/// executor the core was about to call — `pure virtual method called`, caught by
/// the ThreadSanitizer job. The predecessor of the pooling code never destroyed
/// a connection at all, so the hazard was unreachable and the ownership question
/// never had to be answered.
///
/// It is answered here rather than by pinning the owner, because `add()` needs
/// nothing from the owner: `_ex` is a copy of the stream's Asio executor and
/// stays valid as long as the caller's `io_context` does, which this transport
/// already requires. So the executor simply outlives its owner when it has to,
/// and no ownership cycle is created.
class asio_strand_executor final : public folly::Executor,
                                   public std::enable_shared_from_this<asio_strand_executor> {
public:
    /// @param ex   the stream's Asio executor, copied.
    /// @param context_keeper  an optional owner of the `io_context` `ex` came
    ///     from, held for exactly as long as `_ex` is. Empty by default, which
    ///     is the historical behaviour and correct wherever the caller can
    ///     guarantee its `io_context` outlives every chain — see the class
    ///     comment above for why that guarantee is not free.
    explicit asio_strand_executor(net::any_io_executor ex,
                                  std::shared_ptr<void> context_keeper = {})
        : _context_keeper(std::move(context_keeper)), _ex(std::move(ex)) {}

    auto add(folly::Func func) -> void override { net::post(_ex, std::move(func)); }

    auto handle() -> folly::Executor& { return *this; }

    /// @brief Take a keep-alive token. Returning `true` opts this executor into
    /// Folly's reference-counted protocol, so `KeepAlive` will pair every
    /// acquire with a release.
    auto keepAliveAcquire() noexcept -> bool override {
        // `server_session` holds its executor by value rather than by
        // `shared_ptr`, and a by-value executor has no control block to pin
        // itself with — `shared_from_this()` would throw `bad_weak_ptr`.
        // Returning `false` there is exactly Folly's default and exactly the
        // behaviour that class already had and still needs: its sessions are
        // `shared_ptr`-managed and their continuations capture `self`, so the
        // executor cannot outlive its chains anyway. The answer is stable per
        // object — a given executor is either shared-owned for its whole life
        // or never — which is what Folly requires of this predicate.
        auto self = weak_from_this().lock();
        if (!self) {
            return false;
        }
        std::lock_guard<std::mutex> lock(_keep_alive_mutex);
        if (_keep_alive_count++ == 0) {
            _self = std::move(self);
        }
        return true;
    }

    auto keepAliveRelease() noexcept -> void override {
        std::shared_ptr<asio_strand_executor> release_outside_lock;
        {
            std::lock_guard<std::mutex> lock(_keep_alive_mutex);
            if (--_keep_alive_count == 0) {
                release_outside_lock = std::move(_self);
            }
        }
        // Dropped with the lock released, and as the very last thing this
        // function does: it may be the reference that destroys `*this`, so
        // nothing below may touch a member.
    }

private:
    // Declared *before* `_ex`, and that ordering is the whole point: members
    // are destroyed in reverse declaration order, so this is released last.
    // `~any_io_executor` drops the last reference to Asio's `strand_impl`,
    // whose destructor's first statement locks `service_->mutex_` on the
    // `strand_executor_service` owned by the `io_context`'s service registry.
    // Destroy the `io_context` first and that is a read of freed memory --
    // measured as `heap-use-after-free` under ASan, six runs in six, in
    // `.kiro/specs/multi-raft-performance/` task 5a.
    std::shared_ptr<void> _context_keeper;
    net::any_io_executor _ex;
    std::mutex _keep_alive_mutex;
    std::size_t _keep_alive_count{0};
    std::shared_ptr<asio_strand_executor> _self;
};

#endif

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
/// Derives from `enable_shared_from_this` so that every asynchronous operation
/// a connection starts can keep the connection itself alive for the operation's
/// duration. Beast's composed operations hold the stream by reference and the
/// continuation chains below capture `this`, so a connection destroyed with an
/// op still pending is a use-after-free -- observed as `pure virtual method
/// called` when a queued handler reaches a partially destroyed object.
///
/// The predecessor of this design never destroyed a connection at all: retired
/// ones were parked in a `_retired_connections` vector that lived as long as
/// the client, which made the hazard unreachable at the cost of never
/// reclaiming a connection. Ownership by the operations themselves is the same
/// guarantee without the leak.
class beast_connection : public std::enable_shared_from_this<beast_connection> {
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
    /// @param context_keeper  forwarded to `asio_strand_executor` and held
    ///     here too, since `_stream`'s own executor references the same
    ///     `io_context`. See that class's `_context_keeper` for why.
    explicit plain_beast_connection(beast::tcp_stream stream,
                                    std::shared_ptr<void> context_keeper = {});

    [[nodiscard]] auto is_open() const -> bool override;
    auto set_timeout(std::chrono::milliseconds timeout) -> void override;
    auto connect(const net::ip::tcp::endpoint& ep, const std::string& host)
        -> kythira::future_default<kythira::unit> override;
    auto send(beast_http::request<beast_http::string_body> request)
        -> kythira::future_default<beast_http::response<beast_http::string_body>> override;
    auto close() -> void override;

private:
    // Declared first so it is destroyed last: _stream holds an executor into
    // the same io_context that _executor does.
    std::shared_ptr<void> _context_keeper;
    beast::tcp_stream _stream;
    // Declared after _stream (member init order follows declaration order,
    // not initializer-list order) so the constructor can build this from
    // _stream.get_executor() -- see asio_strand_executor's own comment
    // above for why every async_*_kf future this connection creates needs
    // to be via()'d onto it.
    std::shared_ptr<asio_strand_executor> _executor;
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
    /// @param context_keeper  see `plain_beast_connection`'s -- same reason.
    explicit tls_beast_connection(beast::ssl_stream<beast::tcp_stream> stream,
                                  std::shared_ptr<void> context_keeper = {});

    [[nodiscard]] auto is_open() const -> bool override;
    auto set_timeout(std::chrono::milliseconds timeout) -> void override;
    auto connect(const net::ip::tcp::endpoint& ep, const std::string& host)
        -> kythira::future_default<kythira::unit> override;
    auto send(beast_http::request<beast_http::string_body> request)
        -> kythira::future_default<beast_http::response<beast_http::string_body>> override;
    auto close() -> void override;

private:
    // See plain_beast_connection::_context_keeper above -- same reason.
    std::shared_ptr<void> _context_keeper;
    beast::ssl_stream<beast::tcp_stream> _stream;
    // See plain_beast_connection::_executor above -- same reason.
    std::shared_ptr<asio_strand_executor> _executor;
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
/// Connections are pooled per target and **checked out exclusively** for the
/// duration of one RPC (`acquire_connection` / `connection_lease`), then
/// returned to the pool. Reuse across RPCs is preserved (Requirement 9), and
/// concurrent RPCs to the same target each take their own connection.
///
/// ### Why exclusive checkout, and not one shared connection per target
///
/// This class previously kept exactly one connection per target and let every
/// concurrent RPC to that target use it at once, on the reasoning that the
/// connection's `net::strand` would serialize them. **A strand serializes
/// handler invocation, not composed operations.** `async_write` followed by
/// `async_read` is two composed operations, and two RPCs sharing a stream
/// interleave their writes and reads no matter whose strand they run on —
/// Beast is explicit that a stream may have at most one outstanding read and
/// one outstanding write. In practice the second RPC's response framing broke
/// and surfaced as `end of stream`, each failure tore the shared connection
/// down underneath the other in-flight RPCs, and the resulting
/// dangling-reference window segfaulted.
///
/// Exclusive checkout removes both failure modes by construction: a stream is
/// only ever driven by one RPC, and a lease hands back a connection *by value*
/// so no caller ever holds a reference into the pool across an unlocked window.
///
/// `_mutex` guards only the pool's structure, never a connection's I/O.
template<typename Types>
requires kythira::future_default_transport_types<Types>
class boost_beast_client {
public:
    using serializer_type = typename Types::serializer_type;
    using serializer_registry_type = typename Types::serializer_registry_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;
    template<typename T> using future_template = typename Types::template future_template<T>;

    /// @param context_keeper  optional owner of `ioc`, propagated to every
    ///     connection this client makes and to the Folly executor each one
    ///     wraps around its Asio strand. Defaulted empty, so every existing
    ///     caller is unaffected; pass one when `ioc`'s lifetime is *not*
    ///     guaranteed to exceed that of the Folly future chains this client's
    ///     RPCs start -- `error_handler`'s delayed retries in particular
    ///     outlive the RPC that armed them, and can outlive the whole client.
    ///     See `beast_detail::asio_strand_executor::_context_keeper`.
    boost_beast_client(net::io_context& ioc,
                       std::unordered_map<std::uint64_t, std::string> node_id_to_url_map,
                       boost_beast_client_config config, metrics_type metrics,
                       std::shared_ptr<void> context_keeper = {});
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

    /// @brief Validates the configured TLS material, then drops every pooled
    ///     connection so subsequent RPCs build fresh ones using the reloaded
    ///     material.
    ///
    ///     A connection currently leased by an in-flight RPC stays alive —
    ///     the lease holds a `shared_ptr` — and is dropped rather than pooled
    ///     when that RPC finishes, because the TLS generation it was built
    ///     under no longer matches (Property 7, Requirement 7.3).
    auto reload_tls_material() -> void;

    /// @brief Starts a background thread that polls `client_cert_path`'s
    ///     mtime every `poll_interval` and calls `reload_tls_material()`
    ///     when it has changed.
    auto enable_auto_reload(std::chrono::seconds poll_interval) -> void;

    /// @brief Stops the auto-reload background thread cleanly (joined, not
    ///     detached).
    auto disable_auto_reload() -> void;

private:
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

    /// @brief Where a target lives, resolved once and cached.
    ///
    /// Resolution is synchronous and happens under `_mutex`. Under the previous
    /// one-connection-per-target scheme that cost was paid once per target; with
    /// exclusive checkout a burst of concurrent RPCs to a cold target would
    /// otherwise each resolve it, serialized behind the lock.
    struct resolved_target {
        net::ip::tcp::endpoint endpoint;
        std::string host_header;
    };

    /// @brief One connection plus what is needed to re-establish it.
    ///
    /// `shared_ptr` rather than `unique_ptr` so that an in-flight RPC's own
    /// reference keeps the connection alive even when the pool has dropped it
    /// — the guarantee the old `_retired_connections` vector existed to fake,
    /// obtained here from ownership instead of from a side list.
    struct pooled_connection {
        std::shared_ptr<beast_detail::beast_connection> connection;
        net::ip::tcp::endpoint endpoint;
        std::string host_header;
        std::chrono::steady_clock::time_point last_used;
        /// The TLS generation this connection was built under. A reload bumps
        /// the client's counter; a lease acquired before it must not be pooled
        /// afterwards, since it is still using the superseded context.
        std::uint64_t tls_generation{0};
    };

    /// @brief Exclusive checkout of one pooled connection, returned on
    /// destruction.
    ///
    /// Held as a `shared_ptr` by every continuation in `send_rpc`'s chain, so
    /// the connection goes back to the pool exactly once, when the last link
    /// of whichever branch actually ran drops it. Only a lease that
    /// `mark_reusable()` was called on is pooled again; anything else is
    /// dropped, which is the per-connection form of what `remove_connection`
    /// used to do per target — and strictly narrower, since one RPC's failure
    /// no longer tears down a connection another RPC is using.
    class connection_lease {
    public:
        connection_lease(boost_beast_client* owner, std::uint64_t target, pooled_connection conn,
                         std::shared_ptr<in_flight_guard> in_flight)
            : _owner(owner),
              _target(target),
              _conn(std::move(conn)),
              _in_flight(std::move(in_flight)) {}

        connection_lease(const connection_lease&) = delete;
        auto operator=(const connection_lease&) -> connection_lease& = delete;
        connection_lease(connection_lease&&) = delete;
        auto operator=(connection_lease&&) -> connection_lease& = delete;

        ~connection_lease() { _owner->release_connection(_target, std::move(_conn), _reusable); }

        /// @brief The exchange completed cleanly; the connection may be reused.
        auto mark_reusable() -> void { _reusable = true; }

        [[nodiscard]] auto connection() const -> beast_detail::beast_connection* {
            return _conn.connection.get();
        }
        [[nodiscard]] auto endpoint() const -> const net::ip::tcp::endpoint& {
            return _conn.endpoint;
        }
        [[nodiscard]] auto host_header() const -> const std::string& { return _conn.host_header; }

    private:
        boost_beast_client* _owner;
        std::uint64_t _target;
        pooled_connection _conn;
        /// Held so the destructor's `release_connection` — which takes the
        /// client's `_mutex` and touches `_idle_connections` — provably runs
        /// while the drain counter is still raised. Without it the last
        /// continuation could drop the in-flight guard first, letting
        /// `~boost_beast_client` finish its drain and destroy the pool out
        /// from under this destructor's own body.
        std::shared_ptr<in_flight_guard> _in_flight;
        bool _reusable{false};
    };

    // Empty unless a caller opted in, in which case it owns the `io_context`
    // that `_ioc` refers to. Declared first so it is destroyed last, and
    // handed to every connection this client makes -- see the constructor's
    // `context_keeper` parameter.
    std::shared_ptr<void> _context_keeper;
    net::io_context& _ioc;
    serializer_type _serializer;
    /// Negotiation goes through the registry; `_serializer` stays because the
    /// surrounding code names it and the two agree for a single-serializer
    /// bundle.
    serializer_registry_type _registry;
    /// What each peer last answered in. Advisory only — the full `Accept` list
    /// still rides on every request.
    peer_capability_cache<std::uint64_t> _capability_cache;
    std::unordered_map<std::uint64_t, std::string> _node_id_to_url;
    boost_beast_client_config _config;
    metrics_type _metrics;
    std::optional<net::ssl::context> _ssl_ctx;
    mutable std::mutex _mutex;
    /// Idle connections per target, available for checkout. A connection that
    /// is currently leased is in nobody's map — its lease owns it.
    std::unordered_map<std::uint64_t, std::vector<pooled_connection>> _idle_connections;
    /// Every connection this client has created and not yet seen destroyed,
    /// weakly. The destructor force-closes through this so that a *leased*
    /// connection — absent from `_idle_connections` by definition — is closed
    /// too, which is what makes the drain below terminate.
    std::vector<std::weak_ptr<beast_detail::beast_connection>> _all_connections;
    std::unordered_map<std::uint64_t, resolved_target> _resolved_targets;
    std::uint64_t _tls_generation{0};
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

    auto validate_certificate_files() const -> void;
    auto load_client_certificates() -> void;
    auto build_ssl_context() -> net::ssl::context;
    /// @brief Take a connection for this target out of the pool, or build one.
    ///
    /// Returns **by value**: the caller owns it for the RPC's duration and no
    /// reference into the pool outlives `_mutex`. That is the whole fix for the
    /// dangling-reference crash — the old signature returned
    /// `pooled_connection&`, and any concurrent teardown erased the map node
    /// underneath it.
    auto acquire_connection(std::uint64_t target) -> pooled_connection;
    /// @brief Return a leased connection. Pooled only when `reusable` and the
    /// TLS generation still matches; dropped otherwise.
    auto release_connection(std::uint64_t target, pooled_connection conn, bool reusable) -> void;
    /// @brief Build a fresh connection to `target`. Caller holds `_mutex`.
    auto make_connection(std::uint64_t target) -> pooled_connection;

    auto resolve_target(std::uint64_t target) -> const resolved_target&;

    /// @param attempted Media types already refused by this peer for this call.
    ///        Empty on the caller's first entry; the 415 retry path re-enters
    ///        with it extended (Requirement 7.3, see
    ///        `next_request_media_type_after_rejection`). Threading it through
    ///        the ordinary send path — rather than giving the retry its own —
    ///        is what keeps a retried attempt identical to a first one in every
    ///        respect except the encoding: same connect, same pooling, same
    ///        metrics, same timeout handling.
    /// @param chosen_media_type The encoding this attempt must use, empty on the
    ///        caller's first entry (where it comes from the cache or the
    ///        registry default instead). Passed in rather than re-derived from
    ///        @p attempted, because the retry path has *already* decided — and
    ///        may have decided using the peer's `Accept-Post`, which is
    ///        information this function does not have. Re-deriving here silently
    ///        discarded that and walked the preference list blind; the only
    ///        symptom was one extra round trip, which nothing observed.
    template<typename Request, typename Response>
    auto send_rpc(std::uint64_t target, std::string_view endpoint, const Request& request,
                  std::chrono::milliseconds timeout, std::vector<std::string> attempted = {},
                  std::string chosen_media_type = {}) -> future_template<Response>;
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
    using serializer_registry_type = typename Types::serializer_registry_type;
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
    /// @brief Decodes, dispatches, and encodes one request.
    ///
    /// Takes the request's declared media type and its parsed `Accept` list,
    /// and reports back the media type it actually encoded in. The session
    /// reads the headers (it owns the request object) but does not hold the
    /// registry, so negotiation policy stays here and header mechanics stay
    /// there.
    ///
    /// `response_media_type` is set on every path, including the error ones,
    /// so the caller never has to decide what to label a body it did not
    /// produce. Error bodies are plain text and say so.
    auto dispatch(std::string_view target, const std::vector<std::byte>& body,
                  const std::string& request_media_type, const std::vector<std::string>& accepted,
                  std::string& response_body, unsigned& status_code,
                  std::string& response_media_type) -> void;

    /// @brief The media type used when a peer declares none.
    ///
    /// Replaces `success_content_type()`, which derived a single fixed type
    /// from the serializer's `name()`. A negotiating server has no single
    /// success content type — it has whichever one this exchange settled on —
    /// so the session now learns that from `dispatch` instead of asking.
    [[nodiscard]] auto default_media_type() const -> std::string;

    /// @brief This server's `Accept-Post` value — every request media type it
    ///        can decode, in preference order — or empty if it has none.
    ///
    /// Read by the session when `dispatch` returns 415, so the peer's retry can
    /// converge in one round trip (W3C LDP 1.0 §7.1). Exposed here rather than
    /// returned from `dispatch` as an eighth out-parameter because the existing
    /// split puts negotiation *policy* in `dispatch` and header *mechanics* in
    /// the session that owns the response object, and an `Accept-Post` header is
    /// mechanics.
    [[nodiscard]] auto accept_post_header() const -> std::string;

private:
    net::io_context& _ioc;
    serializer_type _serializer;
    /// Decode and encode both route through here, so this server can answer a
    /// peer in a format it did not itself choose.
    serializer_registry_type _registry;
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
