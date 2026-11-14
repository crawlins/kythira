// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file group_range_mux.hpp
/// @brief One Raft listener shared by several multi-raft hosts, routed by
///        group id range (.kiro/specs/service-colocation/ task 3,
///        Requirements 2.2 and 4).
///
/// A composite node runs one multi-raft host per module, each with its own
/// state machine type, storage and transactions, and all of them behind the
/// node's one Raft server. Every Raft RPC carries its group id, and each
/// module owns a disjoint range of ids (`group_space`), so the range alone
/// says which host a message is for.
///
/// `group_range_mux` wraps the real server and installs one handler per RPC
/// type on it. `view(first, last)` gives a host a `network_server` for its
/// range: the host registers its handlers on the view exactly as it would
/// on a real server, and its own demultiplexer then routes by group inside
/// the range. A message for no view's range, or for a view whose host is
/// not running, gets the same refusal a host gives a group it does not hold.
///
/// The client side needs nothing: `transport_client_handle` holds a pointer
/// to the node's one client, so every host shares its connection pool.

#include <raft/network.hpp>
#include <raft/types.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kythira {

template<typename Server, typename NodeId> class group_range_view;

/// @tparam Server The node's real Raft server; must satisfy
///         `kythira::network_server<Server, NodeId>`.
template<typename Server, typename NodeId = std::uint64_t> class group_range_mux {
public:
    using view_type = group_range_view<Server, NodeId>;

    using rv_request = request_vote_request<NodeId>;
    using rv_response = request_vote_response<>;
    using pv_request = request_pre_vote_request<NodeId>;
    using pv_response = request_pre_vote_response<>;
    using ae_request = append_entries_request<NodeId>;
    using ae_response = append_entries_response<>;
    using is_request = install_snapshot_request<NodeId>;
    using is_response = install_snapshot_response<>;
    using fl_request = fetch_log_entries_request<NodeId>;
    using fl_response = fetch_log_entries_response<NodeId>;
    using tn_request = timeout_now_request<NodeId>;
    using tn_response = timeout_now_response<>;

    static constexpr bool k_pre_vote = network_server_with_pre_vote<Server, NodeId>;
    static constexpr bool k_log_fetch = network_server_with_log_fetch<Server, NodeId>;
    static constexpr bool k_timeout_now = network_server_with_timeout_now<Server, NodeId>;

    /// One host's handlers.
    struct handlers {
        std::function<rv_response(const rv_request&)> request_vote;
        std::function<pv_response(const pv_request&)> request_pre_vote;
        std::function<ae_response(const ae_request&)> append_entries;
        std::function<is_response(const is_request&)> install_snapshot;
        std::function<fl_response(const fl_request&)> fetch_log_entries;
        std::function<tn_response(const tn_request&)> timeout_now;
    };

    /// @param server The real server, which outlives the mux. Its handlers
    ///        are installed here, so it must not be started yet.
    /// @param local_id This node's id, stamped on the refusals the mux
    ///        builds itself.
    group_range_mux(Server& server, NodeId local_id)
        : _server(&server), _local_id(std::move(local_id)) {
        install();
    }

    group_range_mux(const group_range_mux&) = delete;
    auto operator=(const group_range_mux&) -> group_range_mux& = delete;
    group_range_mux(group_range_mux&&) = delete;
    auto operator=(group_range_mux&&) -> group_range_mux& = delete;
    ~group_range_mux() = default;

    /// A server for group ids `[first, last]`.
    /// @throws std::invalid_argument when `first > last` or the range
    ///         overlaps another view's: two hosts claiming one group would
    ///         each take the other's messages.
    [[nodiscard]] auto view(std::uint64_t first, std::uint64_t last) -> view_type {
        if (first > last) {
            throw std::invalid_argument("group_range_mux: empty range " + std::to_string(first) +
                                        "-" + std::to_string(last));
        }
        std::unique_lock lock(_slots_mutex);
        for (const auto& s : _slots) {
            if (first <= s->last && s->first <= last) {
                throw std::invalid_argument(
                    "group_range_mux: range " + std::to_string(first) + "-" + std::to_string(last) +
                    " overlaps " + std::to_string(s->first) + "-" + std::to_string(s->last));
            }
        }
        _slots.push_back(std::make_unique<slot>(first, last));
        return view_type(*this, *_slots.back());
    }

    /// Messages no running host's range took.
    [[nodiscard]] auto unrouted_count() const -> std::uint64_t {
        return _unrouted.load(std::memory_order_relaxed);
    }

    /// Hosts started and not yet stopped.
    [[nodiscard]] auto running_views() const -> std::size_t {
        std::lock_guard lock(_run_mutex);
        return _running;
    }

    [[nodiscard]] auto server() noexcept -> Server& { return *_server; }

private:
    friend class group_range_view<Server, NodeId>;

    struct slot {
        slot(std::uint64_t f, std::uint64_t l) : first(f), last(l) {}
        const std::uint64_t first;
        const std::uint64_t last;
        /// Held shared for each call into the host, and exclusively to
        /// publish or withdraw the host's handlers, so a stopped host is
        /// never called after `stop()` returns.
        mutable std::shared_mutex mutex;
        handlers pending;
        std::optional<handlers> live;
        /// `live.has_value()`, readable without the lock.
        std::atomic<bool> running{false};
    };

    void install() {
        _server->register_request_vote_handler([this](const rv_request& req) {
            return route(req, &handlers::request_vote, [&req] {
                return rv_response{
                    ._term = req.term(), ._vote_granted = false, ._group_id = req.group_id()};
            });
        });
        if constexpr (k_pre_vote) {
            _server->register_request_pre_vote_handler([this](const pv_request& req) {
                return route(req, &handlers::request_pre_vote, [&req] {
                    return pv_response{
                        ._term = req.term(), ._vote_granted = false, ._group_id = req.group_id()};
                });
            });
        }
        if constexpr (k_timeout_now) {
            _server->register_timeout_now_handler([this](const tn_request& req) {
                return route(req, &handlers::timeout_now, [&req] {
                    return tn_response{
                        ._term = req.term(), ._success = false, ._group_id = req.group_id()};
                });
            });
        }
        _server->register_append_entries_handler([this](const ae_request& req) {
            return route(req, &handlers::append_entries, [&req] {
                return ae_response{._term = req.term(),
                                   ._success = false,
                                   ._conflict_index = std::nullopt,
                                   ._conflict_term = std::nullopt,
                                   ._group_id = req.group_id()};
            });
        });
        _server->register_install_snapshot_handler([this](const is_request& req) {
            return route(req, &handlers::install_snapshot, [&req] {
                return is_response{._term = req.term(), ._group_id = req.group_id()};
            });
        });
        if constexpr (k_log_fetch) {
            _server->register_fetch_log_entries_handler([this](const fl_request& req) {
                return route(req, &handlers::fetch_log_entries, [this, &req] {
                    return fl_response{._responder_id = _local_id,
                                       ._available = false,
                                       ._prev_log_term = 0,
                                       ._entries = {},
                                       ._group_id = req.group_id()};
                });
            });
        }
    }

    template<typename Request, typename Member, typename Refuse>
    auto route(const Request& req, Member member, Refuse refuse) {
        const auto group = static_cast<std::uint64_t>(req.group_id());
        const slot* target = nullptr;
        {
            std::shared_lock lock(_slots_mutex);
            for (const auto& s : _slots) {
                if (group >= s->first && group <= s->last) {
                    target = s.get();
                    break;
                }
            }
        }
        if (target != nullptr) {
            std::shared_lock lock(target->mutex);
            if (target->live) {
                const auto& fn = (*target->live).*member;
                // An empty slot is an optional RPC the host never
                // registered, which it answers as its own transport would.
                if (fn) {
                    return fn(req);
                }
                return refuse();
            }
        }
        _unrouted.fetch_add(1, std::memory_order_relaxed);
        return refuse();
    }

    void start_view(slot& s) {
        {
            std::unique_lock lock(s.mutex);
            if (s.live) {
                return;
            }
            s.live = s.pending;
            s.running = true;
        }
        std::lock_guard lock(_run_mutex);
        if (_running++ == 0 && !_server->is_running()) {
            _server->start();
        }
    }

    void stop_view(slot& s) {
        {
            // Waits out calls already inside the host.
            std::unique_lock lock(s.mutex);
            if (!s.live) {
                return;
            }
            s.live.reset();
            s.running = false;
        }
        std::lock_guard lock(_run_mutex);
        if (--_running == 0) {
            _server->stop();
        }
    }

    Server* _server;
    NodeId _local_id;
    mutable std::shared_mutex _slots_mutex;
    std::vector<std::unique_ptr<slot>> _slots;
    mutable std::mutex _run_mutex;
    std::size_t _running = 0;
    std::atomic<std::uint64_t> _unrouted{0};
};

/// One host's `network_server`: its range of the node's listener.
///
/// Handlers registered here take effect at `start()`, and are withdrawn at
/// `stop()`, which returns only once no call is inside them, so the host
/// may be destroyed right after. The node's real server starts with the
/// first running view and stops with the last.
template<typename Server, typename NodeId> class group_range_view {
    using mux_type = group_range_mux<Server, NodeId>;

public:
    group_range_view() = default;

    void register_request_vote_handler(
        std::function<typename mux_type::rv_response(const typename mux_type::rv_request&)> h) {
        set(&mux_type::handlers::request_vote, std::move(h));
    }

    void register_append_entries_handler(
        std::function<typename mux_type::ae_response(const typename mux_type::ae_request&)> h) {
        set(&mux_type::handlers::append_entries, std::move(h));
    }

    void register_install_snapshot_handler(
        std::function<typename mux_type::is_response(const typename mux_type::is_request&)> h) {
        set(&mux_type::handlers::install_snapshot, std::move(h));
    }

    void register_request_pre_vote_handler(
        std::function<typename mux_type::pv_response(const typename mux_type::pv_request&)> h)
    requires mux_type::k_pre_vote
    {
        set(&mux_type::handlers::request_pre_vote, std::move(h));
    }

    void register_fetch_log_entries_handler(
        std::function<typename mux_type::fl_response(const typename mux_type::fl_request&)> h)
    requires mux_type::k_log_fetch
    {
        set(&mux_type::handlers::fetch_log_entries, std::move(h));
    }

    void register_timeout_now_handler(
        std::function<typename mux_type::tn_response(const typename mux_type::tn_request&)> h)
    requires mux_type::k_timeout_now
    {
        set(&mux_type::handlers::timeout_now, std::move(h));
    }

    void start() { _mux->start_view(*_slot); }
    void stop() { _mux->stop_view(*_slot); }

    [[nodiscard]] auto is_running() const -> bool { return _slot->running.load(); }

    [[nodiscard]] auto first() const noexcept -> std::uint64_t { return _slot->first; }
    [[nodiscard]] auto last() const noexcept -> std::uint64_t { return _slot->last; }

private:
    friend class group_range_mux<Server, NodeId>;

    group_range_view(mux_type& mux, typename mux_type::slot& s) : _mux(&mux), _slot(&s) {}

    template<typename Member, typename Handler> void set(Member member, Handler h) {
        std::unique_lock lock(_slot->mutex);
        _slot->pending.*member = std::move(h);
    }

    mux_type* _mux = nullptr;
    typename mux_type::slot* _slot = nullptr;
};

}  // namespace kythira
