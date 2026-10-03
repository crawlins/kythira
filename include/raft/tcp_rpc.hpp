// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/exceptions.hpp>
#include <raft/net_bind.hpp>
#include <raft/tcp_connection_tracker.hpp>
#include <raft/executor_default.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/network.hpp>
#include <raft/types.hpp>

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kythira {

// ── Wire framing helpers ─────────────────────────────────────────────────────
// Protocol: [4-byte big-endian payload length][UTF-8 JSON payload]

namespace tcp_detail {

// send() rather than write(): writing to a socket whose peer has closed it
// raises SIGPIPE, whose default action kills the process, and a client
// whose RPC timed out closes its end before the server replies. The flag
// fixes just these writes instead of changing the process's SIGPIPE
// disposition. Where MSG_NOSIGNAL does not exist, SO_NOSIGPIPE is set on
// the socket instead (net_bind::connect_one(), run_accept_loop()).
#ifdef MSG_NOSIGNAL
inline constexpr int k_send_flags = MSG_NOSIGNAL;
#else
inline constexpr int k_send_flags = 0;
#endif

inline auto write_all(int fd, const void* buf, std::size_t n) -> bool {
    const auto* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t w = ::send(fd, p, n, k_send_flags);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return false;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

inline auto read_all(int fd, void* buf, std::size_t n) -> bool {
    auto* p = static_cast<char*>(buf);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

inline constexpr std::uint32_t k_max_frame_bytes = 64u * 1024u * 1024u;
inline constexpr std::size_t k_frame_read_chunk = 1024u * 1024u;

// Reads a `len`-byte frame body through `read_chunk(dst, n)` (a read_all
// over an fd or an SSL*), growing the buffer only as bytes arrive: a peer
// that announces 64 MiB and then stops costs at most one chunk, instead of
// a 64 MiB zero-filled allocation made as soon as the 4-byte header lands.
template<typename ReadChunk>
auto read_frame_body(std::uint32_t len, ReadChunk&& read_chunk) -> std::optional<std::string> {
    std::string buf;
    while (buf.size() < len) {
        auto n = std::min<std::size_t>(len - buf.size(), k_frame_read_chunk);
        auto at = buf.size();
        buf.resize(at + n);
        if (!read_chunk(buf.data() + at, n)) {
            return std::nullopt;
        }
    }
    return buf;
}

inline auto frame_send(int fd, std::string_view payload) -> bool {
    auto len = htonl(static_cast<std::uint32_t>(payload.size()));
    return write_all(fd, &len, 4) && write_all(fd, payload.data(), payload.size());
}

inline auto frame_recv(int fd) -> std::optional<std::string> {
    std::uint32_t net_len{};
    if (!read_all(fd, &net_len, 4)) {
        return std::nullopt;
    }
    std::uint32_t len = ntohl(net_len);
    if (len == 0 || len > k_max_frame_bytes) {
        return std::nullopt;
    }
    return read_frame_body(len, [fd](char* dst, std::size_t n) { return read_all(fd, dst, n); });
}

// The bind and dial helpers live in net_bind.hpp, shared with the other
// transports; these names keep existing tcp_detail callers unchanged.
using net_bind::any_endpoints;
using net_bind::bind_endpoint;
using net_bind::connect_one;
using net_bind::connect_to;
using net_bind::endpoint_is_loopback;
using net_bind::hosts_file_addresses;
using net_bind::ipv4_any_endpoint;
using net_bind::is_loopback_bind_address;
using net_bind::local_interface_addresses;
using net_bind::open_listeners;
using net_bind::parse_hosts_address;
using net_bind::require_local_endpoints;
using net_bind::resolve_bind_addresses;

// Extract "type" field value from a JSON string without full parsing.
inline auto extract_type_field(const std::string& json) -> std::string {
    auto pos = json.find("\"type\"");
    if (pos == std::string::npos) {
        return {};
    }
    auto colon = json.find(':', pos + 6);
    if (colon == std::string::npos) {
        return {};
    }
    auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) {
        return {};
    }
    auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) {
        return {};
    }
    return json.substr(q1 + 1, q2 - q1 - 1);
}

inline auto str_to_bytes(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> v(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        v[i] = static_cast<std::byte>(static_cast<unsigned char>(s[i]));
    }
    return v;
}

inline auto bytes_to_str(const std::vector<std::byte>& b) -> std::string {
    std::string s(b.size(), '\0');
    for (std::size_t i = 0; i < b.size(); ++i) {
        s[i] = static_cast<char>(b[i]);
    }
    return s;
}

// Splits "host:port" on its last ':' and strips the brackets from an IPv6
// literal ("[::1]:7000").  Returns nullopt for anything else.
inline auto parse_host_port(const std::string& addr)
    -> std::optional<std::pair<std::string, std::uint16_t>> {
    auto colon = addr.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == addr.size()) {
        return std::nullopt;
    }
    auto host = addr.substr(0, colon);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    const auto port_str = addr.substr(colon + 1);
    if (!std::all_of(port_str.begin(), port_str.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return std::nullopt;
    }
    const auto port = std::stoul(port_str);
    if (port == 0 || port > 65535 || host.empty()) {
        return std::nullopt;
    }
    return std::pair{host, static_cast<std::uint16_t>(port)};
}

// True when `addr` is a bare node ID, as node<Types> sends for a redirect to
// a known leader and for leave_cluster().
inline auto is_node_id_address(const std::string& addr) -> bool {
    return !addr.empty() && std::all_of(addr.begin(), addr.end(),
                                        [](unsigned char c) { return std::isdigit(c) != 0; });
}

template<typename NodeId> class peer_registry {
public:
    // Maps a node ID that has no registered address to one, e.g. by a naming
    // convention every member shares.  Consulted only on a registry miss.
    using resolver_fn =
        std::function<std::optional<std::pair<std::string, std::uint16_t>>(const NodeId&)>;

    peer_registry() = default;
    peer_registry(peer_registry&& other) noexcept
        : _peers(std::move(other._peers)), _resolver(std::move(other._resolver)) {}
    peer_registry& operator=(peer_registry&&) = delete;
    peer_registry(const peer_registry&) = delete;
    peer_registry& operator=(const peer_registry&) = delete;

    void add_peer(NodeId id, std::string host, std::uint16_t port) {
        std::lock_guard lock(_mu);
        _peers[id] = {std::move(host), port};
    }
    void set_resolver(resolver_fn resolver) {
        std::lock_guard lock(_mu);
        _resolver = std::move(resolver);
    }
    auto lookup(NodeId id) const -> std::optional<std::pair<std::string, std::uint16_t>> {
        resolver_fn resolver;
        {
            std::lock_guard lock(_mu);
            auto it = _peers.find(id);
            if (it != _peers.end()) {
                return it->second;
            }
            resolver = _resolver;
        }
        if (!resolver) {
            return std::nullopt;
        }
        return resolver(id);
    }

private:
    mutable std::mutex _mu;
    std::unordered_map<NodeId, std::pair<std::string, std::uint16_t>> _peers;
    resolver_fn _resolver;
};

// Counts RPCs in flight per endpoint so that one unreachable peer cannot take
// every thread of tcp_rpc_client's pool.  Shared with the dispatched tasks,
// which release their slot when they finish.
class inflight_limiter {
public:
    explicit inflight_limiter(std::size_t per_endpoint) : _limit(per_endpoint) {}

    auto try_acquire(const std::string& endpoint) -> bool {
        std::lock_guard lock(_mu);
        auto& n = _count[endpoint];
        if (n >= _limit) {
            return false;
        }
        ++n;
        return true;
    }

    void release(const std::string& endpoint) {
        std::lock_guard lock(_mu);
        if (auto it = _count.find(endpoint); it != _count.end() && --it->second == 0) {
            _count.erase(it);
        }
    }

private:
    std::mutex _mu;
    std::size_t _limit;
    std::unordered_map<std::string, std::size_t> _count;
};

}  // namespace tcp_detail

// ── tcp_rpc_client ────────────────────────────────────────────────────────────
//
// Satisfies kythira::network_client.
// Makes one blocking TCP connection per RPC call, dispatched onto a small
// private thread pool so the calling thread gets a genuinely pending
// kythira::Future back immediately — callers that broadcast to multiple
// peers in a loop (node<Types>::start_election(),
// node<Types>::replicate_to_followers()) get real concurrent dispatch
// instead of stalling on peer N+1 until peer N's connect()-through-recv()
// (and its own retry_policy's up-to-3 attempts) fully finishes. This
// matters most for a peer that has gone away entirely (e.g. a killed
// process/container): SO_SNDTIMEO/SO_RCVTIMEO (set in connect_to()) do not
// bound the blocking connect() syscall itself on Linux, so reaching a dead
// peer can take far longer than the configured RPC timeout — with the old
// synchronous-inline call(), that alone was enough to blow through an
// election's whole timeout budget before a live peer was ever contacted.
//
// Deliberately a private, directly-constructed kythira::executor_default
// rather than a backend's global CPU executor (e.g. folly::
// getGlobalCPUExecutor()): Folly's global CPU executor is a
// registration-gated singleton that aborts ("requested before
// registrationComplete()") unless folly::init() ran first, which only
// chaos_node's main.cpp (and other real binaries) does — every plain
// Boost.Test unit test that exercises tcp_rpc_client directly (e.g.
// tcp_rpc_unit_test) does not, and would crash on the first RPC.
// kythira::executor_default is backend-selected via KYTHIRA_DEFAULT_FUTURE_
// BACKEND (KYTHIRA_FUTURE_BACKEND_STDEXEC/BOOST, else Folly), so this class
// no longer hard-requires Folly regardless of which backend is selected.

class tcp_rpc_client {
public:
    using serializer_t = json_rpc_serializer<std::vector<std::byte>>;

    // RPC dispatch is network-I/O-bound, not CPU-bound, so a small fixed
    // pool is deliberately plenty even for clusters larger than this
    // project's typical 3-7 node scenario tests — it only needs to cover
    // "number of peers contacted in one broadcast round" worth of
    // concurrently in-flight blocking connect()/send()/recv() sequences.
    static constexpr std::size_t k_rpc_thread_pool_size = 8;

    // A peer that has gone away can hold a pool thread for seconds: its name
    // may take a full resolver timeout to fail, and a connect() or recv() to
    // it waits out the RPC timeout.  The leader sends it a heartbeat every
    // interval regardless, so without a cap those calls fill the whole pool
    // and RPCs to live peers queue behind them, long enough for followers to
    // start elections.  A call over the cap fails at once, which Raft already
    // treats as an unreachable peer.
    static constexpr std::size_t k_max_inflight_per_endpoint = 2;

    tcp_rpc_client()
        : _executor(std::make_shared<kythira::executor_default>(k_rpc_thread_pool_size)),
          _inflight(std::make_shared<tcp_detail::inflight_limiter>(k_max_inflight_per_endpoint)) {}

    void add_peer(std::uint64_t id, std::string host, std::uint16_t port) {
        _peers.add_peer(id, std::move(host), port);
    }

    // Called by node<Types> with the address a joining node advertised in its
    // ClusterJoin, and by the reconnect loop with discovered addresses.
    // `address` is "host:port"; anything else is ignored.
    void update_peer_address(std::uint64_t id, const std::string& address) {
        if (auto hp = tcp_detail::parse_host_port(address)) {
            _peers.add_peer(id, std::move(hp->first), hp->second);
        }
    }

    // Supplies an address for a peer that was never registered: a node that
    // joined after this one started, which the static peer table cannot name.
    void set_peer_resolver(tcp_detail::peer_registry<std::uint64_t>::resolver_fn resolver) {
        _peers.set_resolver(std::move(resolver));
    }

    // ClusterJoin and ClusterLeave are addressed rather than sent to a node
    // ID: the sender may not know the target's ID.  `addr` is "host:port", or
    // a bare node ID (a redirect to a known leader) looked up like any peer.
    auto send_cluster_join_request(const std::string& addr, const cluster_join_request<>& req,
                                   std::chrono::milliseconds timeout)
        -> future_default<cluster_join_response<>> {
        return call_address<cluster_join_response<>>(
            addr, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_cluster_join_response(d);
            });
    }

    auto send_cluster_leave_request(const std::string& addr, const cluster_leave_request<>& req,
                                    std::chrono::milliseconds timeout)
        -> future_default<cluster_leave_response<>> {
        return call_address<cluster_leave_response<>>(
            addr, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_cluster_leave_response(d);
            });
    }

    auto send_request_vote(std::uint64_t target, const request_vote_request<>& req,
                           std::chrono::milliseconds timeout)
        -> future_default<request_vote_response<>> {
        return call<request_vote_response<>>(target, _ser.serialize(req), timeout,
                                             [this](const std::vector<std::byte>& d) {
                                                 return _ser.deserialize_request_vote_response(d);
                                             });
    }

    // Satisfies kythira::network_client_with_pre_vote (`.kiro/specs/raft-pre-vote/`).
    auto send_request_pre_vote(std::uint64_t target, const request_pre_vote_request<>& req,
                               std::chrono::milliseconds timeout)
        -> future_default<request_pre_vote_response<>> {
        return call<request_pre_vote_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_request_pre_vote_response(d);
            });
    }

    // Satisfies kythira::network_client_with_timeout_now (leadership transfer,
    // Ongaro's dissertation §3.10).
    auto send_timeout_now(std::uint64_t target, const timeout_now_request<>& req,
                          std::chrono::milliseconds timeout)
        -> future_default<timeout_now_response<>> {
        return call<timeout_now_response<>>(target, _ser.serialize(req), timeout,
                                            [this](const std::vector<std::byte>& d) {
                                                return _ser.deserialize_timeout_now_response(d);
                                            });
    }

    // Satisfies kythira::network_client_with_log_fetch
    // (`.kiro/specs/peer2peer-log-replication/` Requirement 5.2).
    auto send_fetch_log_entries(std::uint64_t target, const fetch_log_entries_request<>& req,
                                std::chrono::milliseconds timeout)
        -> future_default<fetch_log_entries_response<>> {
        return call<fetch_log_entries_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_fetch_log_entries_response(d);
            });
    }

    auto send_append_entries(std::uint64_t target, const append_entries_request<>& req,
                             std::chrono::milliseconds timeout)
        -> future_default<append_entries_response<>> {
        return call<append_entries_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_append_entries_response(d);
            });
    }

    auto send_install_snapshot(std::uint64_t target, const install_snapshot_request<>& req,
                               std::chrono::milliseconds timeout)
        -> future_default<install_snapshot_response<>> {
        return call<install_snapshot_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_install_snapshot_response(d);
            });
    }

private:
    template<typename Resp, typename Deser>
    auto call_address(const std::string& addr, const std::vector<std::byte>& payload,
                      std::chrono::milliseconds timeout, Deser deser) -> future_default<Resp> {
        if (tcp_detail::is_node_id_address(addr)) {
            return call<Resp>(std::stoull(addr), payload, timeout, std::move(deser));
        }
        auto hp = tcp_detail::parse_host_port(addr);
        if (!hp) {
            return future_factory_default::makeExceptionalFuture<Resp>(std::make_exception_ptr(
                network_exception("tcp_rpc_client: malformed address " + addr)));
        }
        return call_endpoint<Resp>(std::move(hp->first), hp->second, payload, timeout,
                                   std::move(deser));
    }

    template<typename Resp, typename Deser>
    auto call(std::uint64_t target, const std::vector<std::byte>& payload,
              std::chrono::milliseconds timeout, Deser deser) -> future_default<Resp> {
        auto peer = _peers.lookup(target);
        if (!peer) {
            return future_factory_default::makeExceptionalFuture<Resp>(std::make_exception_ptr(
                network_exception("tcp_rpc_client: unknown peer " + std::to_string(target))));
        }
        return call_endpoint<Resp>(peer->first, peer->second, payload, timeout, std::move(deser));
    }

    template<typename Resp, typename Deser>
    auto call_endpoint(std::string host, std::uint16_t port, const std::vector<std::byte>& payload,
                       std::chrono::milliseconds timeout, Deser deser) -> future_default<Resp> {
        // Dispatched onto the global CPU executor rather than run inline —
        // see the class comment above for why: this is what lets a caller
        // broadcasting to multiple peers in a loop move on to the next peer
        // immediately instead of blocking on this one's full
        // connect()-through-recv() sequence.
        auto endpoint = host + ':' + std::to_string(port);
        if (!_inflight->try_acquire(endpoint)) {
            return future_factory_default::makeExceptionalFuture<Resp>(std::make_exception_ptr(
                network_exception("tcp_rpc_client: too many RPCs in flight to " + endpoint)));
        }

        promise_default<Resp> promise;
        auto future = promise.getFuture();

        // Captured by value: `payload` is a reference to the caller's
        // temporary (e.g. _ser.serialize(req)), which does not outlive this
        // function; copying now, before dispatch, is required for the
        // background task to see valid data.
        _executor->submit([promise = std::move(promise), host, port, payload, timeout, deser,
                           inflight = _inflight, endpoint = std::move(endpoint)]() mutable {
            struct Slot {
                tcp_detail::inflight_limiter& limiter;
                const std::string& endpoint;
                ~Slot() { limiter.release(endpoint); }
            } slot{*inflight, endpoint};

            int fd = tcp_detail::connect_to(host, port, timeout);
            if (fd < 0) {
                promise.setException(std::make_exception_ptr(network_exception(
                    "tcp_rpc_client: connect failed to " + host + ":" + std::to_string(port))));
                return;
            }

            struct Guard {
                int fd;
                ~Guard() {
                    if (fd >= 0) {
                        ::close(fd);
                    }
                }
            } g{fd};

            if (!tcp_detail::frame_send(fd, tcp_detail::bytes_to_str(payload))) {
                promise.setException(
                    std::make_exception_ptr(network_exception("tcp_rpc_client: send failed")));
                return;
            }

            auto resp = tcp_detail::frame_recv(fd);
            if (!resp) {
                promise.setException(
                    std::make_exception_ptr(network_exception("tcp_rpc_client: recv failed")));
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

    tcp_detail::peer_registry<std::uint64_t> _peers;
    serializer_t _ser;
    std::shared_ptr<kythira::executor_default> _executor;
    std::shared_ptr<tcp_detail::inflight_limiter> _inflight;
};

// ── tcp_rpc_server ────────────────────────────────────────────────────────────
//
// Satisfies kythira::network_server.
// Accepts connections in a background thread per listener and serves each on
// its own thread, bounded and deadlined by a tcp_detail::connection_tracker
// (tcp_server_limits; .kiro/specs/tcp-rpc-server-hardening/).

class tcp_rpc_server {
public:
    using rv_fn = std::function<request_vote_response<>(const request_vote_request<>&)>;
    using pv_fn = std::function<request_pre_vote_response<>(const request_pre_vote_request<>&)>;
    using tn_fn = std::function<timeout_now_response<>(const timeout_now_request<>&)>;
    using ae_fn = std::function<append_entries_response<>(const append_entries_request<>&)>;
    using is_fn = std::function<install_snapshot_response<>(const install_snapshot_request<>&)>;
    using cj_fn = std::function<cluster_join_response<>(const cluster_join_request<>&)>;
    using cl_fn = std::function<cluster_leave_response<>(const cluster_leave_request<>&)>;
    using fl_fn = std::function<fetch_log_entries_response<>(const fetch_log_entries_request<>&)>;
    using serializer_t = json_rpc_serializer<std::vector<std::byte>>;

    explicit tcp_rpc_server(std::uint16_t port, tcp_server_limits limits = {})
        : _port(port), _conns(tcp_detail::connection_tracker::create(limits, "tcp_rpc_server")) {}

    // Listens on `bind_address` only instead of every IPv4 interface: an IPv4
    // or IPv6 literal, or a host name whose addresses all belong to this host
    // (see tcp_detail::resolve_bind_addresses). Throws std::invalid_argument
    // otherwise.
    tcp_rpc_server(std::uint16_t port, const std::string& bind_address,
                   tcp_server_limits limits = {})
        : _port(port),
          _binds(tcp_detail::resolve_bind_addresses(bind_address, "tcp_rpc_server")),
          _conns(tcp_detail::connection_tracker::create(limits, "tcp_rpc_server")) {}

    ~tcp_rpc_server() { stop(); }

    tcp_rpc_server(const tcp_rpc_server&) = delete;
    tcp_rpc_server& operator=(const tcp_rpc_server&) = delete;
    tcp_rpc_server& operator=(tcp_rpc_server&&) = delete;

    // Move-only before start() is called (not safe to move a running server).
    tcp_rpc_server(tcp_rpc_server&& other) noexcept
        : _port(other._port),
          _binds(std::move(other._binds)),
          _listen_fds(std::move(other._listen_fds)),
          _running(other._running.load()),
          _accept_threads(std::move(other._accept_threads)),
          _conns(std::move(other._conns)),
          _rv(std::move(other._rv)),
          _pv(std::move(other._pv)),
          _tn(std::move(other._tn)),
          _ae(std::move(other._ae)),
          _is(std::move(other._is)),
          _cj(std::move(other._cj)),
          _cl(std::move(other._cl)),
          _fl(std::move(other._fl)),
          _ser(std::move(other._ser)) {
        other._running = false;
    }

    void register_request_vote_handler(rv_fn h) { _rv = std::move(h); }
    // Satisfies kythira::network_server_with_pre_vote (`.kiro/specs/raft-pre-vote/`).
    void register_request_pre_vote_handler(pv_fn h) { _pv = std::move(h); }
    // Satisfies kythira::network_server_with_timeout_now (leadership transfer).
    void register_timeout_now_handler(tn_fn h) { _tn = std::move(h); }
    void register_append_entries_handler(ae_fn h) { _ae = std::move(h); }
    void register_install_snapshot_handler(is_fn h) { _is = std::move(h); }
    // Satisfies kythira::network_server_with_cluster_join / _with_cluster_leave.
    void register_cluster_join_handler(cj_fn h) { _cj = std::move(h); }
    void register_cluster_leave_handler(cl_fn h) { _cl = std::move(h); }
    // Satisfies kythira::network_server_with_log_fetch (peer-to-peer catch-up).
    void register_fetch_log_entries_handler(fl_fn h) { _fl = std::move(h); }

    void start() {
        if (_running.exchange(true)) {
            return;
        }
        std::lock_guard lock(_listen_mu);
        try {
            _listen_fds = tcp_detail::open_listeners(_binds, _port, "tcp_rpc_server");
        } catch (...) {
            _running = false;
            throw;
        }
        _conns->start_reaper();
        for (int fd : _listen_fds) {
            _accept_threads.emplace_back([this, fd] {
                tcp_detail::run_accept_loop(
                    fd, _running, *_conns, _backoff,
                    [this](tcp_detail::connection_tracker::ticket t) {
                        std::thread(
                            [this](tcp_detail::connection_tracker::ticket t) {
                                handle(std::move(t));
                            },
                            std::move(t))
                            .detach();
                    });
            });
        }
    }

    // Stops accepting, then waits until no connection thread can touch this
    // server again: connections still reading or writing are shut down at
    // once, and one inside a handler is waited for.
    void stop() {
        if (!_running.exchange(false)) {
            return;
        }
        _backoff.wake();
        std::lock_guard lock(_listen_mu);
        // shutdown() wakes a thread blocked in accept(); the descriptors are
        // closed only after the threads are joined so none can be reused
        // under a running accept().
        for (int fd : _listen_fds) {
            ::shutdown(fd, SHUT_RDWR);
        }
        for (auto& t : _accept_threads) {
            if (t.joinable()) t.join();
        }
        _accept_threads.clear();
        for (int fd : _listen_fds) {
            ::close(fd);
        }
        _listen_fds.clear();
        _conns->shutdown_and_drain();
    }

    [[nodiscard]] bool is_running() const noexcept { return _running.load(); }

    [[nodiscard]] auto connection_stats() const -> tcp_server_connection_stats {
        return _conns->stats();
    }

private:
    using phase = tcp_detail::connection_tracker::phase;

    // Runs on the connection's own thread; `t` closes the socket and frees
    // its slot when this returns.
    void handle(tcp_detail::connection_tracker::ticket t) {
        const int fd = t.fd();
        auto data = tcp_detail::frame_recv(fd);
        if (!data) {
            return;
        }
        t.enter(phase::handler);

        std::string type = tcp_detail::extract_type_field(*data);
        auto bytes = tcp_detail::str_to_bytes(*data);

        try {
            std::vector<std::byte> resp;
            if (type == "request_vote_request" && _rv) {
                resp = _ser.serialize(_rv(_ser.deserialize_request_vote_request(bytes)));
            } else if (type == "request_pre_vote_request" && _pv) {
                resp = _ser.serialize(_pv(_ser.deserialize_request_pre_vote_request(bytes)));
            } else if (type == "timeout_now_request" && _tn) {
                resp = _ser.serialize(_tn(_ser.deserialize_timeout_now_request(bytes)));
            } else if (type == "append_entries_request" && _ae) {
                resp = _ser.serialize(_ae(_ser.deserialize_append_entries_request(bytes)));
            } else if (type == "install_snapshot_request" && _is) {
                resp = _ser.serialize(_is(_ser.deserialize_install_snapshot_request(bytes)));
            } else if (type == "cluster_join_request" && _cj) {
                resp = _ser.serialize(_cj(_ser.deserialize_cluster_join_request(bytes)));
            } else if (type == "cluster_leave_request" && _cl) {
                resp = _ser.serialize(_cl(_ser.deserialize_cluster_leave_request(bytes)));
            } else if (type == "fetch_log_entries_request" && _fl) {
                resp = _ser.serialize(_fl(_ser.deserialize_fetch_log_entries_request(bytes)));
            } else {
                return;
            }
            t.enter(phase::reply);
            tcp_detail::frame_send(fd, tcp_detail::bytes_to_str(resp));
        } catch (...) {
        }
    }

    std::uint16_t _port;
    std::vector<tcp_detail::bind_endpoint> _binds{tcp_detail::ipv4_any_endpoint()};
    std::mutex _listen_mu;  // guards _listen_fds/_accept_threads across start()/stop()
    std::vector<int> _listen_fds;
    std::atomic<bool> _running{false};
    std::vector<std::thread> _accept_threads;
    tcp_detail::accept_backoff _backoff;
    std::shared_ptr<tcp_detail::connection_tracker> _conns;

    rv_fn _rv;
    pv_fn _pv;
    tn_fn _tn;
    ae_fn _ae;
    is_fn _is;
    cj_fn _cj;
    cl_fn _cl;
    fl_fn _fl;
    serializer_t _ser;
};

// ── Concept assertions ────────────────────────────────────────────────────────

static_assert(kythira::network_client<tcp_rpc_client>,
              "tcp_rpc_client must satisfy network_client");
static_assert(kythira::network_server<tcp_rpc_server>,
              "tcp_rpc_server must satisfy network_server");
static_assert(kythira::network_client_with_pre_vote<tcp_rpc_client>,
              "tcp_rpc_client must satisfy network_client_with_pre_vote");
static_assert(kythira::network_server_with_pre_vote<tcp_rpc_server>,
              "tcp_rpc_server must satisfy network_server_with_pre_vote");

// The optional leadership-transfer extension (Ongaro's dissertation §3.10),
// without which the multi-Raft placement driver's `transfer_leader` operator
// and `scatter` are unavailable on this transport.
static_assert(kythira::network_client_with_timeout_now<tcp_rpc_client>,
              "tcp_rpc_client must satisfy network_client_with_timeout_now");
static_assert(kythira::network_server_with_timeout_now<tcp_rpc_server>,
              "tcp_rpc_server must satisfy network_server_with_timeout_now");

// ClusterJoin/ClusterLeave, without which a node cannot join a running
// cluster over this transport (quorum-management Req 19: a replacement the
// quorum manager provisions must be able to join).
static_assert(kythira::network_client_with_cluster_join<tcp_rpc_client>,
              "tcp_rpc_client must satisfy network_client_with_cluster_join");
static_assert(kythira::network_server_with_cluster_join<tcp_rpc_server>,
              "tcp_rpc_server must satisfy network_server_with_cluster_join");
static_assert(kythira::network_client_with_cluster_leave<tcp_rpc_client>,
              "tcp_rpc_client must satisfy network_client_with_cluster_leave");
static_assert(kythira::network_server_with_cluster_leave<tcp_rpc_server>,
              "tcp_rpc_server must satisfy network_server_with_cluster_leave");
// The optional peer-to-peer catch-up extension, without which a node can
// gossip its progress over tcp_gossip_transport but never fetch from a peer.
static_assert(kythira::network_client_with_log_fetch<tcp_rpc_client>,
              "tcp_rpc_client must satisfy network_client_with_log_fetch");
static_assert(kythira::network_server_with_log_fetch<tcp_rpc_server>,
              "tcp_rpc_server must satisfy network_server_with_log_fetch");

}  // namespace kythira
