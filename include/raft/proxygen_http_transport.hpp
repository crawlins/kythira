#pragma once

/// @file proxygen_http_transport.hpp
/// @brief A third `kythira::network_client`/`network_server` implementation,
///     backed by Meta's Proxygen HTTP library driven directly by Folly's
///     `EventBase`/`IOThreadPoolExecutor`, alongside the existing
///     cpp-httplib-backed (`include/raft/http_transport.hpp`) and
///     Boost.Beast-backed (`include/raft/beast_http_transport.hpp`)
///     transports. See `.kiro/specs/proxygen-http-transport/` for the full
///     design and `spike-notes.md` for the empirical findings this header's
///     shape is based on.
///
/// Client/server threading is asymmetric, a direct consequence of
/// Proxygen's own API shape (design.md's "Why Proxygen's own async model"
/// section): `proxygen_client` takes a caller-shared
/// `folly::IOThreadPoolExecutorBase&` and never owns one (Requirement 8.1),
/// the same posture `boost_beast_client` has toward `net::io_context&`.
/// `proxygen_server` owns a dedicated thread to run
/// `proxygen::HTTPServer::start()`'s own blocking call on (Requirement 5.1),
/// the same posture `cpp_httplib_server` has toward
/// `httplib::Server::listen()`.
///
/// Requirement 15: this feature's `Types` bundle is narrower than
/// `transport_types` alone requires -- `Types::template future_template<T>`
/// must be `kythira::future_default<T>` specifically, the same constraint
/// `.kiro/specs/boost-beast-http-transport/` Requirement 19 already
/// established for Beast, for the identical structural reason (the generic
/// bridge, Requirement 14, is built on `kythira::promise_default<T>`).
/// `future_default_proxygen_transport_types`/`proxygen_future_default_transport_types`
/// below are a deliberate, independent *duplicate* of Beast's
/// `future_default_http_transport_types`/`future_default_transport_types`
/// (named distinctly, not `#include`d, so `CONFIG_PROXYGEN_TRANSPORT` and
/// `CONFIG_BOOST_BEAST_TRANSPORT` remain fully independent opt-ins, and so
/// a translation unit that includes both transports' headers -- the
/// three-way cross-transport equivalence test, Requirement 19.5 -- never
/// hits a duplicate-concept-definition error from two identically-shaped
/// concepts sharing one name in the same namespace).

#include <raft/types.hpp>
#include <raft/network.hpp>
#include <raft/http_exceptions.hpp>
#include <raft/metrics.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/http_content_negotiation.hpp>
#include <raft/peer_capability_cache.hpp>
#include <raft/future_default.hpp>
#include <raft/future.hpp>
#include <concepts/future.hpp>

#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/SSLContext.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/ExceptionWrapper.h>
#include <folly/SocketAddress.h>

#include <proxygen/lib/http/HTTPConnector.h>
#include <proxygen/lib/http/session/HTTPUpstreamSession.h>
#include <proxygen/lib/http/session/HTTPTransaction.h>
#include <proxygen/lib/utils/WheelTimerInstance.h>
#include <proxygen/httpserver/HTTPServer.h>
#include <proxygen/httpserver/RequestHandler.h>
#include <proxygen/httpserver/RequestHandlerFactory.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <wangle/ssl/SSLContextConfig.h>

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
#include <tuple>
#include <unordered_map>
#include <vector>

namespace kythira {

/// @brief `Types` bundle whose `future_template` is pinned to
///     `kythira::future_default<T>` (Requirement 15) -- required by
///     `proxygen_client`/`proxygen_server`, not by `transport_types` in
///     general. Additive: does not replace or modify
///     `kythira::http_transport_types` or Beast's own
///     `future_default_http_transport_types`. Identical member shape to
///     Beast's bundle, deliberately independently defined (Requirement
///     15.3) -- see this file's own top comment for why.
template<typename RPC_Serializer, typename Metrics, typename Executor>
struct future_default_proxygen_transport_types {
    template<typename T> using future_template = kythira::future_default<T>;
    using serializer_type = RPC_Serializer;
    using serializer_registry_type = single_serializer_registry<RPC_Serializer>;
    using metrics_type = Metrics;
    using executor_type = Executor;
};

/// @brief Requirement 15.2: `proxygen_client`/`proxygen_server` require
///     `Types::future_template<T>` to be `kythira::future_default<T>`
///     specifically. Named distinctly from Beast's
///     `kythira::future_default_transport_types` (identical constraint,
///     different name) purely to avoid a duplicate-concept-definition
///     error in a translation unit that includes both transports' headers
///     -- see this file's top comment.
template<typename Types>
concept proxygen_future_default_transport_types =
    kythira::transport_types<Types> &&
    std::same_as<typename Types::template future_template<kythira::unit>,
                 kythira::future_default<kythira::unit>>;

/// @brief Client configuration. Field-for-field match with
///     `cpp_httplib_client_config`/`boost_beast_client_config` (Requirement
///     11.1) -- no new data model, so an existing config literal is
///     reusable verbatim across a type-only transport swap.
struct proxygen_client_config {
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
    std::string user_agent{"raft-proxygen/1.0"};
};

/// @brief Server configuration. Field-for-field match with
///     `cpp_httplib_server_config`/`boost_beast_server_config` (Requirement
///     11.2). `max_concurrent_connections` has no direct
///     `proxygen::HTTPServerOptions` equivalent (Requirement 11.3) --
///     Proxygen has no single "reject beyond N connections" knob; this is
///     approximated by `proxygen_server` via its own accept-time counter
///     that closes new connections past the limit (see
///     proxygen_http_transport_impl.hpp), the same "document the
///     equivalent enforcement" precedent
///     `.kiro/specs/boost-beast-http-transport/` Requirement 11.3 used.
struct proxygen_server_config {
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

/// @brief Generic (any-future-backend) bridge primitives (Requirement 14)
///     plus the small amount of connection scaffolding built on top of
///     them, adapted to Proxygen's own callback shape --
///     `proxygen::HTTPConnector::Callback` (a pure-virtual interface, not a
///     completion-handler parameter the way Beast's `async_connect` takes
///     one) and `proxygen::HTTPTransaction::Handler`
///     (`HTTPTransactionHandler`, a 10-pure-virtual-method interface --
///     `spike-notes.md` Finding 2 -- of which this bridge only cares about
///     six; the rest are no-ops, HTTP/1.1 request/response scope,
///     Requirement 18.2).
namespace proxygen_detail {

constexpr const char* proxygen_endpoint_request_vote = "/v1/raft/request_vote";
constexpr const char* proxygen_endpoint_append_entries = "/v1/raft/append_entries";
constexpr const char* proxygen_endpoint_install_snapshot = "/v1/raft/install_snapshot";

/// @brief Accumulated client-side response -- status code plus the fully
///     read body. Proxygen delivers the body across one or more `onBody`
///     calls (`folly::IOBuf` chains), accumulated here into one contiguous
///     `std::string` at `onEOM`, the same "no zero-copy passthrough yet"
///     posture Beast's own `beast::flat_buffer`-based accumulation has --
///     Requirement 17.3's `IOBuf` zero-copy claim is about avoiding this
///     project's own accumulation for large bodies in a *future*
///     optimization, not something this first cut already exploits.
struct http_response {
    std::uint16_t status_code{0};
    std::string body;
    /// @brief The response's declared `Content-Type`, with any parameters
    ///     (`; charset=...`) already stripped -- empty when the peer sent no
    ///     such header, which the caller reads as "a peer that predates
    ///     negotiation" and answers with its own default rather than an error
    ///     (Requirement 6.4).
    ///
    ///     Lives on the shared struct, not in each caller, because Proxygen has
    ///     *two* client paths (the generic bridge and the Folly fast path) with
    ///     two separate transaction bridges. Reading the header in one place
    ///     that both fill is what stops the two paths from disagreeing about
    ///     what an absent or parameterised `Content-Type` means.
    std::string content_type;
    /// @brief The response's `Accept-Post` value, verbatim, or empty when the
    ///     peer sent none.
    ///
    ///     Only meaningful on a 415, where it is the rejecting server naming the
    ///     request media types it *would* have taken (W3C LDP 1.0 §7.1), so the
    ///     client's retry converges in one round trip rather than walking its
    ///     whole preference list.
    ///
    ///     Here for the same reason `content_type` is: two client paths, two
    ///     transaction bridges, and a header read in one of them only would make
    ///     a node's retry behaviour depend on which future backend it was
    ///     compiled with.
    std::string accept_post;
};

/// @brief Collapses concurrent connect attempts for one pooled connection
///     (Requirement 9: *one* pooled connection per target node).
///
/// Without this, `connect_if_needed` checked only whether the slot held a
/// session, with no record that a connect was already underway -- so N
/// concurrent RPCs to a cold target each saw a null slot and each started
/// their own connect, producing N sessions where the pool is documented to
/// hold one. They then clobbered each other in the slot, and every orphan
/// still carried a `session_liveness_tracker` aimed at that same slot, so an
/// orphan's `onDestroy` would null a slot holding a *live* session and the
/// next caller would connect on top of a session still in use. Tearing
/// sessions down mid-use is what broke Proxygen's own internal invariants
/// (`HTTP2PriorityQueue activeCount_ > 0`, `ObserverContainer !iterating_`).
///
/// Needs no lock of its own: every task for a given target runs on that
/// target's own pinned `EventBase` (see `pooled_connection`), so the
/// check-and-set below is already serialized. It is *only* ever touched from
/// that thread.
struct connect_state {
    bool connecting{false};
    std::vector<kythira::promise_default<proxygen::HTTPUpstreamSession*>> waiters;
};

/// @brief Bridges `proxygen::HTTPConnector::Callback` into a
///     `kythira::promise_default<proxygen::HTTPUpstreamSession*>`
///     (Requirement 14.1) -- the connect-step primitive, the direct analog
///     of Beast's `async_connect_kf`. Owns the `proxygen::HTTPConnector`
///     and its `proxygen::WheelTimerInstance` itself (both must outlive the
///     connect attempt -- `HTTPConnector`'s own header comment: "Whatever
///     object is passed here MUST outlive this connector"), and
///     self-deletes once the outcome is known (`spike-notes.md` Finding
///     1's confirmed-safe idiom -- every first-party Proxygen example
///     handler manages its own lifetime the same way, since
///     `HTTPTransaction`/`HTTPConnector` -- not this bridge's creator --
///     decide when its job is over).
///
/// Owns the pool bookkeeping for the connect it represents -- recording the
/// session into `slot`, attaching the `session_liveness_tracker`, and
/// releasing `connect`'s waiters -- rather than leaving that to a future
/// continuation. `HTTPConnector` invokes `connectSuccess`/`connectError` on
/// the pinned `EventBase`, whereas a continuation is not guaranteed to run
/// there at all: under `KYTHIRA_DEFAULT_FUTURE_BACKEND=boost` a promise-backed
/// `boost::future::then()` has policy `launch::none` and dispatches through
/// `future_async_continuation_shared_state`, which runs the continuation on a
/// **freshly spawned detached thread**. Doing this work here is what keeps it
/// on the one thread allowed to touch session lifetime state.
class connect_bridge : public proxygen::HTTPConnector::Callback {
public:
    connect_bridge(folly::EventBase* evb, std::chrono::milliseconds txn_timeout,
                   std::shared_ptr<std::atomic<proxygen::HTTPUpstreamSession*>> slot,
                   std::shared_ptr<connect_state> connect,
                   kythira::promise_default<proxygen::HTTPUpstreamSession*> promise);

    [[nodiscard]] auto connector() -> proxygen::HTTPConnector& { return _connector; }

    auto connectSuccess(proxygen::HTTPUpstreamSession* session) -> void override;
    auto connectError(const folly::AsyncSocketException& ex) -> void override;

private:
    proxygen::WheelTimerInstance _wheel_timer;
    proxygen::HTTPConnector _connector;
    std::shared_ptr<std::atomic<proxygen::HTTPUpstreamSession*>> _slot;
    std::shared_ptr<connect_state> _connect;
    kythira::promise_default<proxygen::HTTPUpstreamSession*> _promise;
};

/// @brief Bridges `proxygen::HTTPTransaction::Handler` into a
///     `kythira::promise_default<http_response>` (Requirement 14.1) -- the
///     send/receive-step primitive, the direct analog of Beast's
///     `async_write_kf`/`async_read_kf` pair (Proxygen's single
///     multi-callback handler interface covers what Beast needed two
///     primitives for). Self-deletes in `detachTransaction()`, matching
///     `connect_bridge`'s idiom -- `detachTransaction()` is `proxygen`'s own
///     documented terminal callback ("the `HTTPTransaction` object ...
///     will be invalid after this function completes").
class transaction_bridge : public proxygen::HTTPTransaction::Handler {
public:
    explicit transaction_bridge(kythira::promise_default<http_response> promise);

    auto setTransaction(proxygen::HTTPTransaction* txn) noexcept -> void override;
    auto detachTransaction() noexcept -> void override;
    auto onHeadersComplete(std::unique_ptr<proxygen::HTTPMessage> msg) noexcept -> void override;
    auto onBody(std::unique_ptr<folly::IOBuf> chain) noexcept -> void override;
    auto onTrailers(std::unique_ptr<proxygen::HTTPHeaders> trailers) noexcept -> void override;
    auto onEOM() noexcept -> void override;
    auto onUpgrade(proxygen::UpgradeProtocol protocol) noexcept -> void override;
    auto onError(const proxygen::HTTPException& error) noexcept -> void override;
    auto onEgressPaused() noexcept -> void override;
    auto onEgressResumed() noexcept -> void override;

private:
    auto fulfill_value() -> void;
    auto fulfill_exception(std::exception_ptr ex) -> void;

    kythira::promise_default<http_response> _promise;
    proxygen::HTTPTransaction* _txn{nullptr};
    http_response _response;
    bool _fulfilled{false};
};

/// @brief Same bridge shape as `transaction_bridge`, but fulfilling a raw
///     `folly::Promise<http_response>` directly -- the Folly fast path's
///     own handler (Requirement 16.2), never constructed on the generic
///     bridge path and vice versa.
class folly_transaction_bridge : public proxygen::HTTPTransaction::Handler {
public:
    explicit folly_transaction_bridge(folly::Promise<http_response> promise);

    auto setTransaction(proxygen::HTTPTransaction* txn) noexcept -> void override;
    auto detachTransaction() noexcept -> void override;
    auto onHeadersComplete(std::unique_ptr<proxygen::HTTPMessage> msg) noexcept -> void override;
    auto onBody(std::unique_ptr<folly::IOBuf> chain) noexcept -> void override;
    auto onTrailers(std::unique_ptr<proxygen::HTTPHeaders> trailers) noexcept -> void override;
    auto onEOM() noexcept -> void override;
    auto onUpgrade(proxygen::UpgradeProtocol protocol) noexcept -> void override;
    auto onError(const proxygen::HTTPException& error) noexcept -> void override;
    auto onEgressPaused() noexcept -> void override;
    auto onEgressResumed() noexcept -> void override;

private:
    auto fulfill_value() -> void;
    auto fulfill_exception(folly::exception_wrapper ex) -> void;

    folly::Promise<http_response> _promise;
    proxygen::HTTPTransaction* _txn{nullptr};
    http_response _response;
    bool _fulfilled{false};
};

/// @brief Registered as a session's `InfoCallback` so a lazily-detected
///     dead connection (Requirement 9.3) resets the pool's view of it back
///     to nullptr from the one thread ever allowed to touch session
///     lifetime state -- the session's own pinned `EventBase` (design.md's
///     Architecture section; `spike-notes.md` Finding 4). Self-deletes
///     once fired -- `onDestroy` is documented as a last-chance
///     notification before the session itself is gone.
///
/// Clears the slot only if it still holds *this* tracker's own session, hence
/// the `_session` member: more than one session can end up pointing a tracker
/// at the same slot, and an unconditional clear lets a dying one evict a live
/// one. See `connect_state` below.
class session_liveness_tracker : public proxygen::HTTPSessionBase::InfoCallback {
public:
    session_liveness_tracker(std::shared_ptr<std::atomic<proxygen::HTTPUpstreamSession*>> slot,
                             proxygen::HTTPUpstreamSession* session);
    auto onDestroy(const proxygen::HTTPSessionBase& session) -> void override;

private:
    std::shared_ptr<std::atomic<proxygen::HTTPUpstreamSession*>> _slot;
    proxygen::HTTPUpstreamSession* _session;
};

/// @brief One pooled connection's worth of state. `event_base` is chosen
///     once (via the client's shared `IOThreadPoolExecutorBase`) the first
///     time a given target node is seen, and pinned for that node's entire
///     lifetime (Requirement 21.3, `spike-notes.md` Finding 3) -- every
///     operation against `session`, on either the generic or fast path, is
///     issued via `event_base->runInEventBaseThread(...)`, never any other
///     thread, so Property 6's no-interleaving guarantee holds structurally
///     regardless of which path a given RPC takes. `session` is a
///     `shared_ptr<atomic<...>>` (not a plain member) so
///     `session_liveness_tracker` can safely outlive/update it independent
///     of this pool entry's own lifetime (the entry could be erased by a
///     concurrent `reload_tls_material()` while `onDestroy` is still
///     in-flight on `event_base`'s thread).
struct pooled_connection {
    folly::EventBase* event_base{nullptr};
    std::shared_ptr<std::atomic<proxygen::HTTPUpstreamSession*>> session =
        std::make_shared<std::atomic<proxygen::HTTPUpstreamSession*>>(nullptr);
    /// Shared with the in-flight `connect_if_needed` chain for the same
    /// reason `session` is a `shared_ptr`: this pool entry can be evicted
    /// while that chain is still running on `event_base`'s thread.
    std::shared_ptr<connect_state> connect = std::make_shared<connect_state>();
    std::chrono::steady_clock::time_point last_used;
};

}  // namespace proxygen_detail

/// @brief `network_client` implementation backed by Proxygen's
///     `HTTPConnector`/`HTTPUpstreamSession`. Takes a caller-shared
///     `folly::IOThreadPoolExecutorBase&` and never owns one (Requirement
///     8.1) -- no progress happens on any in-flight operation without a
///     caller-run executor thread driving the relevant `EventBase`
///     (Property 1).
///
/// One pooled connection per target node, reused across RPCs (Requirement
/// 9) the same way `boost_beast_client`'s `_connections` map does --
/// `_connections_mutex` only guards the map's own structure, never a
/// connection's I/O (which is always routed through that connection's own
/// pinned `EventBase`, per `pooled_connection`'s own comment).
template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
class proxygen_client {
public:
    using serializer_type = typename Types::serializer_type;
    using serializer_registry_type = typename Types::serializer_registry_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;
    template<typename T> using future_template = typename Types::template future_template<T>;

    proxygen_client(folly::IOThreadPoolExecutorBase& io_executor,
                    std::unordered_map<std::uint64_t, std::string> node_id_to_url_map,
                    proxygen_client_config config, metrics_type metrics);
    ~proxygen_client();

    proxygen_client(const proxygen_client&) = delete;
    auto operator=(const proxygen_client&) -> proxygen_client& = delete;
    proxygen_client(proxygen_client&&) = delete;
    auto operator=(proxygen_client&&) -> proxygen_client& = delete;

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

    /// @brief Validates the configured TLS material, then retires the
    ///     current `folly::SSLContext` (kept alive, not destroyed -- an
    ///     in-flight `HTTPUpstreamSession` may still reference it, Property
    ///     7) so subsequently-established connections use a fresh one.
    auto reload_tls_material() -> void;
    auto enable_auto_reload(std::chrono::seconds poll_interval) -> void;
    auto disable_auto_reload() -> void;

    /// @brief Test-only escape hatch (design.md Property 12): runs the
    ///     generic bridge (Requirement 14) unconditionally, even under a
    ///     `Types` bundle for which `send_rpc`'s own `if constexpr`
    ///     dispatch (Requirement 16.1) would otherwise always select the
    ///     Folly fast path -- there is no way to reach this from
    ///     `send_request_vote`/`send_append_entries`/`send_install_snapshot`,
    ///     which always go through `send_rpc`'s ordinary dispatch and so
    ///     always honor Property 11 (fast path taken iff
    ///     `Types::future_template<T>` is `kythira::Future<T>` exactly).
    ///     This method exists so a test built under
    ///     `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly` can assert that the fast
    ///     path and the generic bridge produce the same
    ///     externally-observable result for the same RPC, which is what
    ///     makes Requirement 17's benchmark comparison of the two paths a
    ///     fair one (same behavior, different cost) rather than two
    ///     different code paths that merely happen to both "work". Public,
    ///     not `private` + `friend`, for the same reason
    ///     `boost_beast_server::dispatch` is: reached from test code
    ///     outside this class, not from a nested template whose second
    ///     parameter would otherwise vary independently of `Types`.
    template<typename Request, typename Response>
    auto send_rpc_via_generic_bridge_for_test(std::uint64_t target, std::string_view endpoint,
                                              const Request& request,
                                              std::chrono::milliseconds timeout)
        -> future_template<Response> {
        return send_rpc_generic_bridge<Request, Response>(
            target, endpoint, request, timeout,
            kythira::select_request_media_type(_registry, _capability_cache, target));
    }

private:
    folly::IOThreadPoolExecutorBase& _io_executor;
    serializer_type _serializer;
    /// Negotiation goes through the registry; `_serializer` stays because the
    /// surrounding code names it and the two agree for a single-serializer
    /// bundle. Same arrangement as `boost_beast_client`.
    serializer_registry_type _registry;
    /// What each peer last answered in. Advisory only -- the full `Accept` list
    /// still rides on every request, so a stale entry costs a re-choice and
    /// never a failed exchange. Carries its own mutex, which matters more here
    /// than for the other transports: both client paths touch it from an
    /// `EventBase` thread that is not the caller's.
    peer_capability_cache<std::uint64_t> _capability_cache;
    std::unordered_map<std::uint64_t, std::string> _node_id_to_url;
    proxygen_client_config _config;
    metrics_type _metrics;
    std::shared_ptr<folly::SSLContext> _ssl_ctx;
    mutable std::mutex _mutex;
    std::unordered_map<std::uint64_t, proxygen_detail::pooled_connection> _connections;
    std::vector<std::shared_ptr<folly::SSLContext>> _retired_ssl_contexts;
    std::jthread _auto_reload_thread;
    std::filesystem::file_time_type _last_reloaded_cert_mtime{};

    auto validate_certificate_files() const -> void;
    auto build_ssl_context() -> std::shared_ptr<folly::SSLContext>;
    auto get_or_create_slot(std::uint64_t target) -> proxygen_detail::pooled_connection&;
    auto resolve_target(std::uint64_t target) const
        -> std::tuple<folly::SocketAddress, std::string, bool>;

    /// @param attempted Media types this peer has already refused with 415 for
    ///        this call (Requirement 7.3). Empty on first entry; the retry path
    ///        re-enters with it extended. The retry lives here, at the dispatch
    ///        point, rather than in either path body -- both paths would
    ///        otherwise need their own copy, which is exactly how Task 10a's
    ///        hardcoded `application/json` came to differ between them.
    /// @param chosen_media_type The encoding this attempt must use, empty on the
    ///        caller's first entry (where it comes from the cache or the
    ///        registry default instead). Passed in rather than re-derived from
    ///        @p attempted, because the retry path has *already* decided -- and
    ///        may have decided using the peer's `Accept-Post`, which is
    ///        information this function does not have. Re-deriving here silently
    ///        discarded that and walked the preference list blind; the only
    ///        symptom was one extra round trip, which nothing observed.
    template<typename Request, typename Response>
    auto send_rpc(std::uint64_t target, std::string_view endpoint, const Request& request,
                  std::chrono::milliseconds timeout, std::vector<std::string> attempted = {},
                  std::string chosen_media_type = {}) -> future_template<Response>;

    /// @brief The generic (any-future-backend) bridge body (Requirement 14)
    ///     -- `send_rpc`'s `else` branch delegates here unconditionally;
    ///     this is also `send_rpc_via_generic_bridge_for_test`'s (public,
    ///     below) sole implementation, so there is exactly one copy of the
    ///     generic-bridge chain-composition logic regardless of which entry
    ///     point reaches it.
    /// @param content_type The request encoding, chosen once by `send_rpc` and
    ///        passed in rather than re-derived here. Both paths taking it as a
    ///        parameter is what makes it structurally impossible for them to
    ///        disagree about which encoding a given call used.
    template<typename Request, typename Response>
    auto send_rpc_generic_bridge(std::uint64_t target, std::string_view endpoint,
                                 const Request& request, std::chrono::milliseconds timeout,
                                 const std::string& content_type) -> future_template<Response>;

    /// @brief Requirement 16: the Folly-native fast path, taken instead of
    ///     `send_rpc` above when `future_template<Response>` is
    ///     `kythira::Future<Response>` exactly (checked via `if constexpr`
    ///     in `send_rpc`'s own dispatch, not here -- see the `.cpp`-shaped
    ///     `_impl.hpp` for the `if constexpr` site). Constructs a raw
    ///     `folly::Promise<T>` -- no `kythira::promise_default<T>` on this
    ///     path at all (Requirement 16.2) -- and schedules continuations
    ///     via `.via(evb)` on the connection's own pinned `EventBase`
    ///     (Requirement 16.3), avoiding the cross-thread post the generic
    ///     bridge does not itself guarantee avoiding.
    template<typename Request, typename Response>
    auto send_rpc_folly_fast_path(std::uint64_t target, std::string_view endpoint,
                                  const Request& request, std::chrono::milliseconds timeout,
                                  const std::string& content_type) -> kythira::Future<Response>;
};

/// @brief `network_server` implementation backed by
///     `proxygen::HTTPServer`. Unlike `proxygen_client`, owns a dedicated
///     thread to run `HTTPServer::start()`'s own blocking call on
///     (Requirement 5.1, `spike-notes.md` Finding 1) -- the same shape
///     `cpp_httplib_server`'s `_server_thread` already uses for
///     `httplib::Server::listen()`'s equally blocking call, not Beast's
///     caller-driven-loop model. Accepts an optional caller-shared
///     `std::shared_ptr<folly::IOThreadPoolExecutorBase>` (Requirement
///     8.2) -- when absent, `HTTPServer::start()` constructs its own,
///     matching `cpp_httplib_server`'s self-contained-by-default posture.
template<typename Types>
requires kythira::proxygen_future_default_transport_types<Types>
class proxygen_server {
public:
    using serializer_type = typename Types::serializer_type;
    using serializer_registry_type = typename Types::serializer_registry_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;
    template<typename T> using future_template = typename Types::template future_template<T>;

    proxygen_server(std::string bind_address, std::uint16_t bind_port,
                    proxygen_server_config config, metrics_type metrics,
                    std::shared_ptr<folly::IOThreadPoolExecutorBase> io_executor = nullptr);
    ~proxygen_server();

    proxygen_server(const proxygen_server&) = delete;
    auto operator=(const proxygen_server&) -> proxygen_server& = delete;
    proxygen_server(proxygen_server&&) = delete;
    auto operator=(proxygen_server&&) -> proxygen_server& = delete;

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

    /// @brief Applies fresh TLS material to a *new*
    ///     `wangle::SSLContextConfig`, via
    ///     `proxygen::HTTPServer::updateTLSCredentials()`, without closing
    ///     the listener or dropping established connections (Property 7).
    auto reload_tls_material() -> void;
    auto enable_auto_reload(std::chrono::seconds poll_interval) -> void;
    auto disable_auto_reload() -> void;

    /// @brief Routes one parsed request to the matching registered
    ///     handler, serializes the response, and reports the HTTP status
    ///     to use. Public (not `private` + `friend`) for the same reason
    ///     `boost_beast_server::dispatch` is -- reached from
    ///     `proxygen_detail`'s `RequestHandler` implementation, a
    ///     free-standing class rather than a nested template whose second
    ///     parameter would otherwise vary independently of `Types`.
    ///
    ///     Takes the request's declared media type and its parsed `Accept`
    ///     list, and reports back the media type it actually encoded in. The
    ///     `RequestHandler` reads the headers (it owns the `HTTPMessage`) but
    ///     does not hold the registry, so header mechanics stay there and
    ///     negotiation policy stays here -- the same split
    ///     `boost_beast_server::dispatch` uses.
    ///
    ///     `response_media_type` is set on every path, including the error
    ///     ones, so the caller never has to decide what to label a body it did
    ///     not produce. Error bodies are plain text and say so.
    auto dispatch(std::string_view target, const std::vector<std::byte>& body,
                  const std::string& request_media_type, const std::vector<std::string>& accepted,
                  std::string& response_body, unsigned& status_code,
                  std::string& response_media_type) -> void;

    /// @brief The media type used when a peer declares none.
    ///
    /// A negotiating server has no single "success content type" -- it has
    /// whichever one this exchange settled on -- so the `RequestHandler` learns
    /// that from `dispatch` and asks for this one only to fill in for an absent
    /// request `Content-Type` (Requirement 4.1, 4.2).
    [[nodiscard]] auto default_media_type() const -> std::string;

    /// @brief This server's `Accept-Post` value -- every request media type it
    ///     can decode, in preference order -- or empty if it has none.
    ///
    /// Read by the `RequestHandler` when `dispatch` returns 415, so the peer's
    /// retry can converge in one round trip (W3C LDP 1.0 §7.1). Exposed here
    /// rather than returned from `dispatch` as an eighth out-parameter because
    /// the existing split puts negotiation *policy* in `dispatch` and header
    /// *mechanics* in the handler that owns the `HTTPMessage`, and an
    /// `Accept-Post` header is mechanics.
    [[nodiscard]] auto accept_post_header() const -> std::string;

    /// @brief Bracket a request's lifetime for `stop()`'s drain (Property
    ///     8) -- mirrors `boost_beast_server::register_session`/
    ///     `session_finished` exactly, adapted to Proxygen's
    ///     `RequestHandler`-per-request granularity rather than
    ///     Beast's whole-connection-session granularity (Proxygen's own
    ///     `HTTPServer` already owns the accept loop and per-connection
    ///     acceptor bookkeeping, so this feature's own drain only needs to
    ///     track requests actually in flight, not connections).
    auto register_request() -> std::size_t;
    auto request_finished(std::size_t request_id) -> void;

private:
    std::string _bind_address;
    std::uint16_t _bind_port;
    proxygen_server_config _config;
    metrics_type _metrics;
    serializer_type _serializer;
    /// Decode and encode both route through here, so this server can answer a
    /// peer in a format it did not itself choose.
    serializer_registry_type _registry;
    std::shared_ptr<folly::IOThreadPoolExecutorBase> _io_executor;
    std::unique_ptr<proxygen::HTTPServer> _http_server;
    std::thread _server_thread;
    std::mutex _start_mutex;
    std::condition_variable _start_cv;
    bool _start_signaled{false};
    std::exception_ptr _start_error;
    std::atomic<bool> _running{false};

    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)>
        _request_vote_handler;
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
        _append_entries_handler;
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)>
        _install_snapshot_handler;

    std::mutex _requests_mutex;
    std::size_t _live_requests{0};
    std::size_t _next_request_id{0};
    std::mutex _drain_mutex;
    std::condition_variable _drain_cv;

    std::mutex _mutex;
    std::jthread _auto_reload_thread;
    std::filesystem::file_time_type _last_reloaded_cert_mtime{};

    auto validate_certificate_files() const -> void;
    auto build_ssl_context_config() -> wangle::SSLContextConfig;
};

}  // namespace kythira
