// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file redis_gateway.hpp
/// @brief The Redis-compatible front end over `multi_raft`
///        (.kiro/specs/redis-compatible-kv/ design Components 5-9).
///
/// A `redis_gateway` owns the listening sockets, the per-connection RESP
/// state, authentication/authorization, the read and write paths onto a
/// `multi_raft<Types, std::string, std::uint64_t>` host, expiry and
/// eviction proposals, and one-hop forwarding to the shard leader's gateway.
/// It changes nothing in the consensus core: every write is a
/// `submit_command`, every read is a `with_state_machine`.
///
/// Threading model. Boost.Asio drives the sockets on `_io_threads` threads.
/// Command execution blocks — a write waits for commit, a forward waits for
/// the peer — so it never runs on an I/O thread: each connection hands its
/// queued commands to a worker pool, one job per connection at a time, which
/// keeps replies in command order and lets an I/O thread keep reading other
/// connections while a worker waits on Raft. Backpressure is the queue depth:
/// past `_max_inflight_per_connection` the connection simply stops reading.
///
/// Definitions live in redis_gateway_impl.hpp, this tree's convention.

#include <raft/redis_acl.hpp>
#include <raft/redis_kv_commands.hpp>
#include <raft/redis_kv_state_machine.hpp>
#include <raft/resp_protocol.hpp>
#include <raft/shard_exceptions.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kythira {

enum class redis_read_consistency : std::uint8_t {
    /// Serve from the shard leader's applied state; forward otherwise.
    leader,
    /// Serve from whichever local replica has the shard, leader or not.
    any_replica,
    /// Propose a no-op through the log first, then read the leader's state.
    linearizable,
};

[[nodiscard]] inline auto parse_redis_read_consistency(std::string_view s)
    -> std::optional<redis_read_consistency> {
    if (s == "leader") {
        return redis_read_consistency::leader;
    }
    if (s == "any_replica" || s == "any-replica") {
        return redis_read_consistency::any_replica;
    }
    if (s == "linearizable") {
        return redis_read_consistency::linearizable;
    }
    return std::nullopt;
}

/// Everything the daemon's `from_env()` fills in (design Component 9).
struct redis_gateway_config {
    /// Plaintext listener, `host:port`. Empty disables it.
    std::string _listen = "0.0.0.0:6379";
    /// TLS listener, `host:port`. Empty disables it.
    std::string _tls_listen;
    std::string _tls_cert_path;
    std::string _tls_key_path;
    std::string _tls_ca_path;
    bool _require_client_cert = false;

    /// Only honoured when the ACL is empty; warns at every startup.
    bool _allow_anonymous = false;
    redis_read_consistency _read_consistency = redis_read_consistency::leader;
    std::size_t _max_value_bytes = 8u * 1024u * 1024u;
    std::size_t _max_clients = 1024;
    std::chrono::milliseconds _idle_timeout{300000};
    std::chrono::milliseconds _command_timeout{5000};
    std::size_t _max_shard_bytes = 1024u * 1024u * 1024u;
    std::size_t _sweep_batch = 1024;
    bool _immutable_values = true;
    bool _forwarding = true;

    /// Identity forwarded commands authenticate as on the peer gateway. A
    /// command arriving on a connection authenticated as this user is never
    /// forwarded again (one hop). The user must exist in the ACL with a role
    /// of at least read_write and scope `*`; the daemon creates it.
    std::string _internal_user = "kythira-internal";
    std::string _internal_secret;

    std::size_t _io_threads = 2;
    std::size_t _worker_threads = 8;
    std::size_t _max_inflight_per_connection = 128;
    /// Bytes of queued-but-unexecuted command arguments across all
    /// connections; past this, new commands are answered with a retryable
    /// error instead of being queued.
    std::size_t _max_inflight_bytes = 256u * 1024u * 1024u;
    /// Auth failures per source address per window before AUTH is refused
    /// without running the KDF.
    std::size_t _auth_failure_limit = 10;
    std::chrono::seconds _auth_failure_window{60};
    /// Log every command at debug level. Off by default: at sccache rates it
    /// is a firehose.
    bool _log_commands = false;

    resp_parser_limits _parser_limits{};
};

/// Counters the gateway keeps for INFO and for the metrics back-end.
struct redis_gateway_stats {
    std::atomic<std::uint64_t> _hits{0};
    std::atomic<std::uint64_t> _misses{0};
    std::atomic<std::uint64_t> _writes{0};
    std::atomic<std::uint64_t> _deletes{0};
    std::atomic<std::uint64_t> _bytes_in{0};
    std::atomic<std::uint64_t> _bytes_out{0};
    std::atomic<std::uint64_t> _oversize_rejections{0};
    std::atomic<std::uint64_t> _value_conflicts{0};
    std::atomic<std::uint64_t> _evictions{0};
    std::atomic<std::uint64_t> _expirations{0};
    std::atomic<std::uint64_t> _auth_failures{0};
    std::atomic<std::uint64_t> _authz_denials{0};
    std::atomic<std::uint64_t> _forwards{0};
    std::atomic<std::uint64_t> _forward_failures{0};
    std::atomic<std::uint64_t> _connections_accepted{0};
    std::atomic<std::uint64_t> _connections_rejected{0};
    std::atomic<std::uint64_t> _connections_current{0};
    std::atomic<std::uint64_t> _commands{0};
    std::atomic<std::uint64_t> _shed{0};
    std::atomic<std::uint64_t> _over_budget_ticks{0};
};

/// Resolves a peer node id to the `host:port` of that node's gateway for
/// forwarding. Supplied by the daemon from its membership/peer-discovery
/// view; the in-process tests supply a map.
template<typename NodeId>
using redis_endpoint_resolver = std::function<std::optional<std::string>(const NodeId&)>;

/// `Host` is a `multi_raft<Types, std::string, std::uint64_t>` whose state
/// machine is `redis_kv_state_machine`. `Logger` and `Metrics` satisfy the
/// project's `logger` and `metrics` concepts.
template<typename Host, typename Logger, typename Metrics> class redis_gateway {
public:
    using node_id_type = typename Host::node_id_type;
    using group_id_type = typename Host::group_id_type;
    using group_node_type = typename Host::group_node_type;
    using endpoint_resolver = redis_endpoint_resolver<node_id_type>;

    redis_gateway(Host& host, redis_acl& acl, Logger& logger, Metrics& metrics,
                  redis_gateway_config config, endpoint_resolver resolver);
    ~redis_gateway();

    redis_gateway(const redis_gateway&) = delete;
    auto operator=(const redis_gateway&) -> redis_gateway& = delete;

    /// Bind the listeners and start the I/O and worker threads. Throws on a
    /// bind failure or an ACL/anonymous misconfiguration.
    auto start() -> void;
    /// Close listeners, drain connections, join threads. Idempotent.
    auto stop() -> void;
    [[nodiscard]] auto is_running() const noexcept -> bool { return _running.load(); }

    /// Bound port of the plaintext listener (0 if none / not started).
    [[nodiscard]] auto port() const noexcept -> std::uint16_t { return _port; }
    [[nodiscard]] auto tls_port() const noexcept -> std::uint16_t { return _tls_port; }

    /// Leader-side maintenance: propose a bounded `sweep` of expired keys and,
    /// when a shard is over budget, a bounded `evict`. Meant to be called from
    /// the host's driver thread after a `tick()` whose policy phase ran, so
    /// the gateway adds no thread of its own (Requirement 6.3). Returns the
    /// number of proposals made.
    auto run_maintenance() -> std::size_t;

    [[nodiscard]] auto stats() const noexcept -> const redis_gateway_stats& { return _stats; }
    [[nodiscard]] auto config() const noexcept -> const redis_gateway_config& { return _config; }

    /// Tests only: execute one command as if it arrived on an authenticated
    /// connection, without a socket. Returns the encoded reply.
    auto execute_for_test(const resp_command& cmd, const std::optional<redis_identity>& identity,
                          bool internal) -> std::string;

    class connection;
    friend class connection;

private:
    struct queued_command {
        resp_command _cmd;
        std::size_t _bytes = 0;
        /// A pre-built reply (the shed error) that replaces execution.
        std::string _prebuilt;
    };

    /// Per-connection protocol state that is independent of the socket.
    struct session {
        resp_parser _parser;
        resp_writer _writer{2};
        std::optional<redis_identity> _identity;
        /// Authenticated as `_internal_user`: never forward again.
        bool _internal = false;
        std::string _source;
        std::string _client_name;
        std::string _lib_name;
        std::string _lib_version;
        bool _closing = false;
    };

    // ---- command dispatch ----------------------------------------------------
    auto execute(session& s, const resp_command& cmd) -> std::string;
    auto handle_auth(session& s, const resp_command& cmd) -> std::string;
    auto handle_hello(session& s, const resp_command& cmd) -> std::string;
    auto handle_client(session& s, const resp_command& cmd) -> std::string;
    auto handle_get(session& s, const resp_command& cmd) -> std::string;
    auto handle_exists(session& s, const resp_command& cmd) -> std::string;
    auto handle_strlen(session& s, const resp_command& cmd) -> std::string;
    auto handle_getrange(session& s, const resp_command& cmd) -> std::string;
    auto handle_ttl(session& s, const resp_command& cmd) -> std::string;
    auto handle_set(session& s, const resp_command& cmd, bool with_expiry) -> std::string;
    auto handle_del(session& s, const resp_command& cmd) -> std::string;
    auto handle_info(session& s) -> std::string;
    auto handle_command(session& s, const resp_command& cmd) -> std::string;
    auto handle_dbsize(session& s) -> std::string;

    // ---- read/write plumbing ---------------------------------------------------
    /// Outcome of a local lookup: the handle, or a reason it could not be
    /// served here (in which case `_forward_to` may name a leader).
    struct read_result {
        typename redis_kv_state_machine<std::uint64_t>::entry_handle _entry;
        std::string _error;  ///< encoded reply if the read failed
        std::optional<node_id_type> _forward_to;
    };
    auto read_local(session& s, const std::string& key) -> read_result;
    auto submit(session& s, const std::string& key, const std::vector<std::byte>& command,
                std::string& error_reply, std::optional<node_id_type>& forward_to) -> bool;
    auto forward(session& s, const resp_command& cmd, const node_id_type& to)
        -> std::optional<std::string>;
    auto forward_endpoint(const node_id_type& to) -> std::optional<std::string>;
    auto locate(const std::string& key, std::string& error_reply,
                std::optional<node_id_type>& forward_to) -> group_node_type*;
    auto authorize(session& s, std::string_view upper, const std::vector<std::string_view>& keys)
        -> std::optional<std::string>;
    auto auth_rate_limited(const std::string& source) -> bool;
    auto note_auth_failure(const std::string& source) -> void;
    auto audit(session& s, std::string_view command, std::string_view outcome) -> void;
    auto emit(std::string_view name, std::string_view command, std::int64_t count = 1) -> void;
    auto emit_duration(std::string_view name, std::string_view command, std::chrono::nanoseconds d)
        -> void;
    [[nodiscard]] static auto now_ms() -> std::uint64_t;
    [[nodiscard]] auto is_expired(const redis_kv_value_entry& e) const -> bool;
    auto touch_lru(group_id_type group, const std::string& key) -> void;
    auto forget_lru(group_id_type group, const std::string& key) -> void;

    // ---- networking ------------------------------------------------------------
    auto accept_loop(boost::asio::ip::tcp::acceptor& acceptor, bool tls) -> void;
    auto build_ssl_context() -> boost::asio::ssl::context;
    auto register_connection(const std::shared_ptr<connection>& c) -> void;
    auto unregister_connection(connection* c) -> void;

    struct forward_pool {
        std::mutex _mutex;
        std::vector<std::unique_ptr<boost::asio::ip::tcp::socket>> _idle;
    };
    auto forward_socket(const std::string& endpoint)
        -> std::unique_ptr<boost::asio::ip::tcp::socket>;
    auto return_forward_socket(const std::string& endpoint,
                               std::unique_ptr<boost::asio::ip::tcp::socket> sock) -> void;

    Host& _host;
    redis_acl& _acl;
    Logger& _logger;
    Metrics& _metrics;
    redis_gateway_config _config;
    endpoint_resolver _resolver;
    redis_gateway_stats _stats;
    std::mutex _metrics_mutex;

    boost::asio::io_context _io;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> _work;
    std::optional<boost::asio::ip::tcp::acceptor> _acceptor;
    std::optional<boost::asio::ip::tcp::acceptor> _tls_acceptor;
    std::shared_ptr<boost::asio::ssl::context> _ssl_ctx;
    std::vector<std::thread> _io_threads;
    std::optional<boost::asio::thread_pool> _workers;
    std::uint16_t _port = 0;
    std::uint16_t _tls_port = 0;
    std::atomic<bool> _running{false};
    std::chrono::steady_clock::time_point _started_at{};

    std::mutex _connections_mutex;
    std::unordered_map<connection*, std::weak_ptr<connection>> _connections;
    std::atomic<std::size_t> _inflight_bytes{0};

    std::mutex _auth_mutex;
    struct auth_failures {
        std::size_t _count = 0;
        std::chrono::steady_clock::time_point _window_start{};
    };
    std::unordered_map<std::string, auth_failures> _auth_failures;

    /// Leader-local advisory LRU per shard: never replicated, rebuilt lazily
    /// after a leadership change from whatever the shard holds.
    struct lru_state {
        std::list<std::string> _order;  // front = most recent
        std::unordered_map<std::string, std::list<std::string>::iterator> _index;
    };
    std::mutex _lru_mutex;
    std::map<group_id_type, lru_state> _lru;

    std::mutex _forward_mutex;
    std::unordered_map<std::string, std::shared_ptr<forward_pool>> _forward_pools;
};

}  // namespace kythira
