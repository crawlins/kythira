// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_test_fabric.hpp
/// @brief An in-process, node-addressed message fabric for the multi-Raft
///        tests: one shared client and one shared server per host, exactly the
///        shape `multi_raft` expects, with delivery on the fabric's own threads.
///
/// ### Why not `simulator_network`
///
/// `simulator_network_client` sends a message and then reads the *next* message
/// off the node's shared receive queue as the reply. That is sound for one Raft
/// group per node, which is what it was built for, and unsound the moment two
/// groups have RPCs in flight at once: group A can be handed group B's reply.
/// Multi-Raft's whole premise is many groups sharing one transport, so the
/// tests need a fabric that correlates a reply with its request. This one does,
/// by never putting a reply on a queue at all — it fulfils the caller's promise
/// directly.
///
/// ### Delivery is asynchronous on purpose
///
/// Invoking the target's handler on the *caller's* thread would run the target
/// host's group code on the sender's stripe, which is precisely the
/// per-group-serial invariant `striped_serial_executor` exists to hold. So a
/// small worker pool carries every message across.
///
/// The fabric also models the two failures the tests need: an unreachable node
/// and a one-way partition.

#include <raft/exceptions.hpp>
#include <raft/future_default.hpp>
#include <raft/group_transport.hpp>
#include <raft/network.hpp>
#include <raft/types.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::testing {

using fabric_messages = kythira::group_rpc_messages<>;

/// @brief The handler table one host's shared server exposes to the fabric.
struct fabric_endpoint {
    std::function<fabric_messages::request_vote_response_type(
        const fabric_messages::request_vote_request_type&)>
        _request_vote;
    std::function<fabric_messages::request_pre_vote_response_type(
        const fabric_messages::request_pre_vote_request_type&)>
        _request_pre_vote;
    std::function<fabric_messages::append_entries_response_type(
        const fabric_messages::append_entries_request_type&)>
        _append_entries;
    std::function<fabric_messages::install_snapshot_response_type(
        const fabric_messages::install_snapshot_request_type&)>
        _install_snapshot;
    std::function<fabric_messages::fetch_log_entries_response_type(
        const fabric_messages::fetch_log_entries_request_type&)>
        _fetch_log_entries;
    bool _running{false};
};

/// @brief Routes RPCs between hosts in one process.
class message_fabric {
public:
    explicit message_fabric(std::size_t workers = 4) {
        _workers.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) {
            _workers.emplace_back([this] { run(); });
        }
    }

    ~message_fabric() { shutdown(); }

    message_fabric(const message_fabric&) = delete;
    auto operator=(const message_fabric&) -> message_fabric& = delete;

    auto shutdown() -> void {
        if (_stopping.exchange(true)) {
            return;
        }
        _not_empty.notify_all();
        for (auto& t : _workers) {
            if (t.joinable()) {
                t.join();
            }
        }
        _workers.clear();
    }

    // ── endpoint registry ────────────────────────────────────────────────────

    auto set_endpoint(std::uint64_t node, fabric_endpoint endpoint) -> void {
        std::lock_guard lock(_mutex);
        _endpoints[node] = std::move(endpoint);
    }

    auto set_running(std::uint64_t node, bool running) -> void {
        std::lock_guard lock(_mutex);
        _endpoints[node]._running = running;
    }

    [[nodiscard]] auto is_running(std::uint64_t node) const -> bool {
        std::lock_guard lock(_mutex);
        auto it = _endpoints.find(node);
        return it != _endpoints.end() && it->second._running;
    }

    // ── failure injection ────────────────────────────────────────────────────

    /// @brief Drop every message addressed to `node` until it is revived.
    auto kill(std::uint64_t node) -> void {
        std::lock_guard lock(_mutex);
        _dead.insert(node);
    }
    auto revive(std::uint64_t node) -> void {
        std::lock_guard lock(_mutex);
        _dead.erase(node);
    }

    /// @brief Drop messages from `from` to `to`, in that direction only.
    auto partition(std::uint64_t from, std::uint64_t to) -> void {
        std::lock_guard lock(_mutex);
        _cut.insert({from, to});
    }
    auto heal(std::uint64_t from, std::uint64_t to) -> void {
        std::lock_guard lock(_mutex);
        _cut.erase({from, to});
    }
    auto heal_all() -> void {
        std::lock_guard lock(_mutex);
        _cut.clear();
        _dead.clear();
    }

    [[nodiscard]] auto delivered() const -> std::uint64_t {
        return _delivered.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto dropped() const -> std::uint64_t {
        return _dropped.load(std::memory_order_relaxed);
    }

    // ── delivery ─────────────────────────────────────────────────────────────

    /// @brief Deliver `req` to `to`'s handler, selected by `select`, and fulfil
    /// the returned future with the reply.
    ///
    /// A message to a dead or partitioned node yields an exceptional future
    /// rather than hanging: `node<Types>` treats a failed RPC and a timed-out
    /// RPC the same way, and failing fast keeps the tests from paying real
    /// timeouts.
    template<typename Response, typename Request, typename Select>
    auto deliver(std::uint64_t from, std::uint64_t to, Request req, Select select)
        -> kythira::future_default<Response> {
        auto promise = std::make_shared<kythira::promise_default<Response>>();
        auto future = promise->getFuture();

        {
            std::lock_guard lock(_mutex);
            if (_stopping.load() || _dead.contains(to) || _cut.contains({from, to})) {
                _dropped.fetch_add(1, std::memory_order_relaxed);
                promise->setException(std::make_exception_ptr(
                    kythira::network_exception("fabric: unreachable node")));
                return future;
            }
            _queue.push_back([this, to, req = std::move(req), select, promise]() mutable {
                std::function<Response(const Request&)> handler;
                {
                    std::lock_guard inner(_mutex);
                    auto it = _endpoints.find(to);
                    if (it != _endpoints.end() && it->second._running) {
                        handler = select(it->second);
                    }
                }
                if (!handler) {
                    _dropped.fetch_add(1, std::memory_order_relaxed);
                    promise->setException(std::make_exception_ptr(
                        kythira::network_exception("fabric: no handler on target")));
                    return;
                }
                try {
                    auto response = handler(req);
                    _delivered.fetch_add(1, std::memory_order_relaxed);
                    promise->setValue(std::move(response));
                } catch (...) {
                    promise->setException(std::current_exception());
                }
            });
        }
        _not_empty.notify_one();
        return future;
    }

private:
    auto run() -> void {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(_mutex);
                _not_empty.wait(lock, [&] { return _stopping.load() || !_queue.empty(); });
                if (_queue.empty()) {
                    return;
                }
                task = std::move(_queue.front());
                _queue.pop_front();
            }
            task();
        }
    }

    mutable std::mutex _mutex;
    std::condition_variable _not_empty;
    std::deque<std::function<void()>> _queue;
    std::unordered_map<std::uint64_t, fabric_endpoint> _endpoints;
    std::set<std::uint64_t> _dead;
    std::set<std::pair<std::uint64_t, std::uint64_t>> _cut;
    std::vector<std::thread> _workers;
    std::atomic<bool> _stopping{false};
    std::atomic<std::uint64_t> _delivered{0};
    std::atomic<std::uint64_t> _dropped{0};
};

/// @brief The one shared client a `multi_raft` host owns.
///
/// Satisfies `network_client` plus the pre-vote and log-fetch extensions, so
/// that the group-scoped views over it advertise them too.
class fabric_client {
public:
    fabric_client() = default;
    fabric_client(message_fabric& fabric, std::uint64_t self) : _fabric(&fabric), _self(self) {}

    auto send_request_vote(std::uint64_t target,
                           const fabric_messages::request_vote_request_type& req,
                           std::chrono::milliseconds)
        -> kythira::future_default<fabric_messages::request_vote_response_type> {
        return _fabric->deliver<fabric_messages::request_vote_response_type>(
            _self, target, req, [](const fabric_endpoint& e) { return e._request_vote; });
    }

    auto send_request_pre_vote(std::uint64_t target,
                               const fabric_messages::request_pre_vote_request_type& req,
                               std::chrono::milliseconds)
        -> kythira::future_default<fabric_messages::request_pre_vote_response_type> {
        return _fabric->deliver<fabric_messages::request_pre_vote_response_type>(
            _self, target, req, [](const fabric_endpoint& e) { return e._request_pre_vote; });
    }

    auto send_append_entries(std::uint64_t target,
                             const fabric_messages::append_entries_request_type& req,
                             std::chrono::milliseconds)
        -> kythira::future_default<fabric_messages::append_entries_response_type> {
        return _fabric->deliver<fabric_messages::append_entries_response_type>(
            _self, target, req, [](const fabric_endpoint& e) { return e._append_entries; });
    }

    auto send_install_snapshot(std::uint64_t target,
                               const fabric_messages::install_snapshot_request_type& req,
                               std::chrono::milliseconds)
        -> kythira::future_default<fabric_messages::install_snapshot_response_type> {
        return _fabric->deliver<fabric_messages::install_snapshot_response_type>(
            _self, target, req, [](const fabric_endpoint& e) { return e._install_snapshot; });
    }

    auto send_fetch_log_entries(std::uint64_t target,
                                const fabric_messages::fetch_log_entries_request_type& req,
                                std::chrono::milliseconds)
        -> kythira::future_default<fabric_messages::fetch_log_entries_response_type> {
        return _fabric->deliver<fabric_messages::fetch_log_entries_response_type>(
            _self, target, req, [](const fabric_endpoint& e) { return e._fetch_log_entries; });
    }

private:
    message_fabric* _fabric{nullptr};
    std::uint64_t _self{0};
};

/// @brief The one shared server a `multi_raft` host owns.
///
/// Satisfies `network_server` plus the pre-vote and log-fetch extensions.
/// `multi_group_network_server` wraps one of these and installs exactly one
/// handler per RPC type on it, which is what makes the fabric node-addressed
/// and the demultiplex group-addressed.
class fabric_server {
public:
    fabric_server() = default;
    fabric_server(message_fabric& fabric, std::uint64_t self) : _fabric(&fabric), _self(self) {}

    auto register_request_vote_handler(std::function<fabric_messages::request_vote_response_type(
                                           const fabric_messages::request_vote_request_type&)>
                                           h) -> void {
        _endpoint._request_vote = std::move(h);
        publish();
    }
    auto register_request_pre_vote_handler(
        std::function<fabric_messages::request_pre_vote_response_type(
            const fabric_messages::request_pre_vote_request_type&)>
            h) -> void {
        _endpoint._request_pre_vote = std::move(h);
        publish();
    }
    auto register_append_entries_handler(
        std::function<fabric_messages::append_entries_response_type(
            const fabric_messages::append_entries_request_type&)>
            h) -> void {
        _endpoint._append_entries = std::move(h);
        publish();
    }
    auto register_install_snapshot_handler(
        std::function<fabric_messages::install_snapshot_response_type(
            const fabric_messages::install_snapshot_request_type&)>
            h) -> void {
        _endpoint._install_snapshot = std::move(h);
        publish();
    }
    auto register_fetch_log_entries_handler(
        std::function<fabric_messages::fetch_log_entries_response_type(
            const fabric_messages::fetch_log_entries_request_type&)>
            h) -> void {
        _endpoint._fetch_log_entries = std::move(h);
        publish();
    }

    auto start() -> void {
        _endpoint._running = true;
        publish();
    }
    auto stop() -> void {
        _endpoint._running = false;
        publish();
    }
    [[nodiscard]] auto is_running() const -> bool { return _endpoint._running; }

private:
    auto publish() -> void {
        if (_fabric != nullptr) {
            _fabric->set_endpoint(_self, _endpoint);
        }
    }

    message_fabric* _fabric{nullptr};
    std::uint64_t _self{0};
    fabric_endpoint _endpoint;
};

}  // namespace kythira::testing
