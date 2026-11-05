// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_transport_handles.hpp
/// @brief Movable handles over a host's non-movable transports, the
///        replication-RPC counters they feed, and the `Types` bundle the
///        transports are instantiated over.
///
/// `multi_raft_config` holds `network_client_type` / `network_server_type`
/// **by value**, and `multi_raft` moves them into place. None of this
/// project's real transports are movable: `boost_beast_client` and
/// `proxygen_client` delete their move constructors outright, and
/// `cpp_httplib_client` holds a `std::mutex` and a `std::jthread`. So the
/// host is handed a *handle*: a movable, pointer-thin view that satisfies
/// `network_client` / `network_server` by forwarding to a transport the caller
/// owns and outlives.
///
/// These started in `tests/multi_raft_transport_harness.hpp`, and the daemons
/// (`cmd/multi_raft_node`, `redis_gateway_node`) borrowed them from there.
/// They are public so that a daemon, and a service module under `services/`
/// in particular, needs nothing from `tests/`
/// (`.kiro/specs/redis-gateway-module/` Requirement 4.1). The harness keeps
/// its `kythira::testing` names for all of them.

#include <raft/executor_default.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/network.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/types.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace kythira {

/// @brief What a window of replication traffic cost, as plain subtractable
///        numbers.
///
/// Separate from `rpc_counters` — which is atomic and lives as long as the
/// cluster — so that a caller takes two snapshots and subtracts them. That is
/// what confines a figure to the *measured* window: the warm-up, the election
/// and the teardown all move the atomics, and none of them belong in
/// "RPCs per committed entry".
struct rpc_snapshot {
    std::uint64_t _append_entries{0};
    /// AppendEntries carrying no entries: heartbeats, and the probe that walks
    /// a follower back to its match index. Counted apart because they are a
    /// function of the *heartbeat interval*, not of the offered load, and
    /// folding them into a batching figure would make a quiet cluster look
    /// like it batches badly.
    std::uint64_t _append_entries_empty{0};
    std::uint64_t _entries{0};
    std::uint64_t _request_vote{0};
    std::uint64_t _install_snapshot{0};

    [[nodiscard]] auto total_rpcs() const -> std::uint64_t {
        return _append_entries + _request_vote + _install_snapshot;
    }

    /// @brief AppendEntries that actually carried log entries.
    [[nodiscard]] auto carrying() const -> std::uint64_t {
        return _append_entries - std::min(_append_entries, _append_entries_empty);
    }

    /// @brief Entries per entry-bearing AppendEntries — the batching factor
    ///        Hypothesis H1 is about. Empty for a window in which no
    ///        AppendEntries carried anything, because 0/0 is not "no batching".
    [[nodiscard]] auto entries_per_append_entries() const -> std::optional<double> {
        return carrying() == 0
                   ? std::nullopt
                   : std::optional{static_cast<double>(_entries) / static_cast<double>(carrying())};
    }

    friend auto operator-(const rpc_snapshot& a, const rpc_snapshot& b) -> rpc_snapshot {
        return rpc_snapshot{
            ._append_entries = a._append_entries - b._append_entries,
            ._append_entries_empty = a._append_entries_empty - b._append_entries_empty,
            ._entries = a._entries - b._entries,
            ._request_vote = a._request_vote - b._request_vote,
            ._install_snapshot = a._install_snapshot - b._install_snapshot,
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Replication-RPC accounting
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Replication RPCs counted at the one place every transport shares.
///
/// Counted in `transport_client_handle` rather than inside a transport or
/// inside `raft_node`, for two reasons. It is the only point on the send path
/// that all three HTTP transports pass through, so a figure taken here is
/// comparable across the transport axis by construction; and the production
/// code stays free of instrumentation inserted for a benchmark, which
/// Requirement 8.1 asks for in as many words.
///
/// Relaxed ordering throughout: these are counters read once at each end of a
/// window that is already fenced by thread joins, not a synchronisation
/// mechanism.
struct rpc_counters {
    std::atomic<std::uint64_t> _append_entries{0};
    std::atomic<std::uint64_t> _append_entries_empty{0};
    std::atomic<std::uint64_t> _entries{0};
    std::atomic<std::uint64_t> _request_vote{0};
    std::atomic<std::uint64_t> _install_snapshot{0};

    [[nodiscard]] auto snapshot() const -> rpc_snapshot {
        return rpc_snapshot{
            ._append_entries = _append_entries.load(std::memory_order_relaxed),
            ._append_entries_empty = _append_entries_empty.load(std::memory_order_relaxed),
            ._entries = _entries.load(std::memory_order_relaxed),
            ._request_vote = _request_vote.load(std::memory_order_relaxed),
            ._install_snapshot = _install_snapshot.load(std::memory_order_relaxed),
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Movable handles over non-movable transports
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A movable `network_client` view of a transport the caller owns.
///
/// Every method forwards; the three mandatory RPCs also tick `rpc_counters`
/// when the constructor was given one, which is how the benchmark gets
/// entries-per-AppendEntries and RPCs-per-committed-entry without a counter in
/// production code. The optional RPCs carry a `requires` clause
/// on `Client` so that this handle satisfies exactly the extension concepts its
/// underlying transport does — no more (which would produce a link error at
/// best) and no fewer (which would silently disable leadership transfer on a
/// transport that supports it).
///
/// `NodeId` is the RPC target and request id type: `std::uint64_t` for every
/// harness row, a host binary's `kythira::host_node_id` for the `cmd/` hosts
/// built with `KYTHIRA_NODE_ID_KIND=text`.
template<typename Client, typename NodeId = std::uint64_t> class transport_client_handle {
public:
    using client_type = Client;

    explicit transport_client_handle(Client& client, rpc_counters* counters = nullptr) noexcept
        : _client(&client), _counters(counters) {}

    // ── the three every transport has ────────────────────────────────────────

    auto send_request_vote(const NodeId& target,
                           const kythira::request_vote_request<NodeId>& request,
                           std::chrono::milliseconds timeout) {
        if (_counters != nullptr) {
            _counters->_request_vote.fetch_add(1, std::memory_order_relaxed);
        }
        return _client->send_request_vote(target, request, timeout);
    }

    auto send_append_entries(const NodeId& target,
                             const kythira::append_entries_request<NodeId>& request,
                             std::chrono::milliseconds timeout) {
        if (_counters != nullptr) {
            // Counted on the way out, before the transport can fail it. An RPC
            // the leader issued is a cost it paid whether or not a response
            // came back, and "RPCs per committed entry" is a cost ratio.
            _counters->_append_entries.fetch_add(1, std::memory_order_relaxed);
            const auto entries = request.entries().size();
            if (entries == 0) {
                _counters->_append_entries_empty.fetch_add(1, std::memory_order_relaxed);
            } else {
                _counters->_entries.fetch_add(entries, std::memory_order_relaxed);
            }
        }
        return _client->send_append_entries(target, request, timeout);
    }

    auto send_install_snapshot(const NodeId& target,
                               const kythira::install_snapshot_request<NodeId>& request,
                               std::chrono::milliseconds timeout) {
        if (_counters != nullptr) {
            _counters->_install_snapshot.fetch_add(1, std::memory_order_relaxed);
        }
        return _client->send_install_snapshot(target, request, timeout);
    }

    // ── the optional ones, mirrored from the underlying transport ────────────

    auto send_request_pre_vote(const NodeId& target,
                               const kythira::request_pre_vote_request<NodeId>& request,
                               std::chrono::milliseconds timeout)
    requires kythira::network_client_with_pre_vote<Client, NodeId>
    {
        return _client->send_request_pre_vote(target, request, timeout);
    }

    auto send_fetch_log_entries(const NodeId& target,
                                const kythira::fetch_log_entries_request<NodeId>& request,
                                std::chrono::milliseconds timeout)
    requires kythira::network_client_with_log_fetch<Client, NodeId>
    {
        return _client->send_fetch_log_entries(target, request, timeout);
    }

    auto send_timeout_now(const NodeId& target, const kythira::timeout_now_request<NodeId>& request,
                          std::chrono::milliseconds timeout)
    requires kythira::network_client_with_timeout_now<Client, NodeId>
    {
        return _client->send_timeout_now(target, request, timeout);
    }

    [[nodiscard]] auto underlying() noexcept -> Client& { return *_client; }

private:
    Client* _client;
    /// Null when nobody is counting — every non-benchmark construction site,
    /// which is why the parameter is defaulted rather than required.
    rpc_counters* _counters{nullptr};
};

/// @brief A movable `network_server` view of a transport the caller owns.
///
/// `start()` and `stop()` are forwarded rather than suppressed: the host is the
/// component that knows when the transport should be live, and every fixture
/// below constructs its server *stopped* precisely so that stays true.
/// `NodeId` as for `transport_client_handle`.
template<typename Server, typename NodeId = std::uint64_t> class transport_server_handle {
public:
    using server_type = Server;

    explicit transport_server_handle(Server& server) noexcept : _server(&server) {}

    auto register_request_vote_handler(std::function<kythira::request_vote_response<>(
                                           const kythira::request_vote_request<NodeId>&)>
                                           handler) -> void {
        _server->register_request_vote_handler(std::move(handler));
    }

    auto register_append_entries_handler(std::function<kythira::append_entries_response<>(
                                             const kythira::append_entries_request<NodeId>&)>
                                             handler) -> void {
        _server->register_append_entries_handler(std::move(handler));
    }

    auto register_install_snapshot_handler(std::function<kythira::install_snapshot_response<>(
                                               const kythira::install_snapshot_request<NodeId>&)>
                                               handler) -> void {
        _server->register_install_snapshot_handler(std::move(handler));
    }

    auto register_request_pre_vote_handler(std::function<kythira::request_pre_vote_response<>(
                                               const kythira::request_pre_vote_request<NodeId>&)>
                                               handler) -> void
    requires kythira::network_server_with_pre_vote<Server, NodeId>
    {
        _server->register_request_pre_vote_handler(std::move(handler));
    }

    auto register_fetch_log_entries_handler(
        std::function<kythira::fetch_log_entries_response<NodeId>(
            const kythira::fetch_log_entries_request<NodeId>&)>
            handler) -> void
    requires kythira::network_server_with_log_fetch<Server, NodeId>
    {
        _server->register_fetch_log_entries_handler(std::move(handler));
    }

    auto register_timeout_now_handler(
        std::function<kythira::timeout_now_response<>(const kythira::timeout_now_request<NodeId>&)>
            handler) -> void
    requires kythira::network_server_with_timeout_now<Server, NodeId>
    {
        _server->register_timeout_now_handler(std::move(handler));
    }

    auto start() -> void { _server->start(); }
    auto stop() -> void { _server->stop(); }
    [[nodiscard]] auto is_running() const -> bool { return _server->is_running(); }

    [[nodiscard]] auto underlying() noexcept -> Server& { return *_server; }

private:
    Server* _server;
};

/// @brief The `Types` bundle a multi-raft host's transports are instantiated
/// over: every in-process harness row and every `cmd/` and `services/`
/// daemon. `kythira::testing::harness_transport_types` names the same thing.
///
/// `future_template` is pinned to `kythira::future_default` because
/// `node<Types>` declares its RPC lambdas as returning
/// `kythira::future_default<T>` and assigns the client's future straight into
/// that — the folly-typed `http_transport_types` bundle would only convert
/// under one backend, and would not compile under boost or stdexec at all.
template<typename Serializer> struct host_transport_types {
    template<typename T> using future_template = kythira::future_default<T>;
    using serializer_type = Serializer;
    using serializer_registry_type = kythira::single_serializer_registry<Serializer>;
    using metrics_type = kythira::noop_metrics;
    using executor_type = kythira::executor_default;
};

}  // namespace kythira
