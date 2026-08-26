// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file group_transport.hpp
/// @brief One shared transport, many Raft groups: a demultiplexing server and
///        the group-scoped client/server *views* that let `node<Types>` keep
///        believing it is alone in the process.
///
/// This is the design's main piece of leverage (`.kiro/specs/multi-raft/`
/// design §2.3). `node<Types>::register_rpc_handlers()` installs exactly one
/// handler per RPC type with its server, and the `network_server` concept has
/// exactly one slot per type — so the demultiplex cannot live inside the node.
/// It lives one level down instead:
///
/// - `multi_group_network_server` wraps the real server, installs one handler
///   per RPC type on it, and dispatches each message on `request.group_id()`.
/// - `group_scoped_server` satisfies `network_server` by forwarding its
///   `register_*_handler` calls into that table, with `start()`/`stop()` as
///   no-ops (the shared server's lifecycle belongs to the host).
/// - `group_scoped_client` satisfies `network_client` by stamping `_group_id`
///   on every outbound request and forwarding the rest verbatim.
///
/// Because both views satisfy the same concepts the real transports do,
/// `node<Types>` is instantiated over them unchanged — not one line of
/// `raft.hpp` participates in the demultiplex.
///
/// **Connection reuse falls out for free** (Requirement 4.6): there is one
/// inner client and one inner server per process, so the existing transports'
/// connection pools are already shared across every group. That is TiKV's
/// stated behaviour — "TiKV reuses the connection between two nodes for
/// multiple Raft groups" — obtained here by construction rather than by a
/// pooling layer.

#include <raft/network.hpp>
#include <raft/shard_types.hpp>
#include <raft/synchronized.hpp>
#include <raft/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

namespace kythira {

/// @brief The RPC message types one demultiplexer speaks.
///
/// A bundle rather than ten template parameters on every class below. The
/// defaults are exactly the types `network_client` / `network_server` are
/// stated over (`request_vote_request<>` and friends), so the default
/// instantiation is the one that satisfies those concepts.
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>,
         typename GroupId = std::uint64_t>
struct group_rpc_messages {
    using node_id_type = NodeId;
    using term_id_type = TermId;
    using log_index_type = LogIndex;
    using log_entry_type = LogEntry;
    using group_id_type = GroupId;

    using request_vote_request_type = request_vote_request<NodeId, TermId, LogIndex, GroupId>;
    using request_vote_response_type = request_vote_response<TermId, GroupId>;
    using request_pre_vote_request_type =
        request_pre_vote_request<NodeId, TermId, LogIndex, GroupId>;
    using request_pre_vote_response_type = request_pre_vote_response<TermId, GroupId>;
    using append_entries_request_type =
        append_entries_request<NodeId, TermId, LogIndex, LogEntry, GroupId>;
    using append_entries_response_type = append_entries_response<TermId, LogIndex, GroupId>;
    using install_snapshot_request_type =
        install_snapshot_request<NodeId, TermId, LogIndex, GroupId>;
    using install_snapshot_response_type = install_snapshot_response<TermId, GroupId>;
    using fetch_log_entries_request_type =
        fetch_log_entries_request<NodeId, TermId, LogIndex, GroupId>;
    using fetch_log_entries_response_type =
        fetch_log_entries_response<TermId, LogIndex, LogEntry, GroupId>;
};

/// @brief What the host decided to do about a message for a group with no
/// local replica (Requirement 12).
enum class unknown_group_action : std::uint8_t {
    /// @brief No replica should exist here; discard the message.
    ///
    /// This is the answer for a message naming a node that is not a member,
    /// and for a group the host cannot place at all.
    drop = 0,
    /// @brief A replica was created and registered; redeliver the message to it.
    ///
    /// Lazy replica creation (design §5.4 step F / Requirement 12.1): a node
    /// held offline through a split learns about its child from the first
    /// inbound message rather than from a control-plane push.
    created = 1,
};

/// @brief The per-group handler table one `node<Types>` installs.
///
/// The two optional slots mirror the optional `network_server_*` extension
/// concepts: a `node` whose transport does not speak pre-vote or log-fetch
/// never registers them, and the corresponding `std::function` stays empty.
template<typename Messages = group_rpc_messages<>> struct group_rpc_handlers {
    std::function<typename Messages::request_vote_response_type(
        const typename Messages::request_vote_request_type&)>
        _request_vote;
    std::function<typename Messages::request_pre_vote_response_type(
        const typename Messages::request_pre_vote_request_type&)>
        _request_pre_vote;
    std::function<typename Messages::append_entries_response_type(
        const typename Messages::append_entries_request_type&)>
        _append_entries;
    std::function<typename Messages::install_snapshot_response_type(
        const typename Messages::install_snapshot_request_type&)>
        _install_snapshot;
    std::function<typename Messages::fetch_log_entries_response_type(
        const typename Messages::fetch_log_entries_request_type&)>
        _fetch_log_entries;
};

/// @brief Wraps one real `network_server` and fans its messages out by group id.
///
/// Not copyable and not movable: `start()` installs lambdas on the inner
/// server that capture `this`, so the object's address is part of the
/// installed state. The host owns exactly one of these, at a stable address,
/// for the process's lifetime.
///
/// @tparam Server   The real server. Must satisfy `network_server`.
/// @tparam GroupId  Must satisfy `raft_group_id`.
/// @tparam Messages The RPC message bundle; see `group_rpc_messages`.
template<typename Server, raft_group_id GroupId = std::uint64_t,
         typename Messages = group_rpc_messages<>>
class multi_group_network_server {
public:
    using server_type = Server;
    using group_id_type = GroupId;
    using messages_type = Messages;
    using handlers_type = group_rpc_handlers<Messages>;

    explicit multi_group_network_server(Server inner) : _inner(std::move(inner)) {}

    multi_group_network_server(const multi_group_network_server&) = delete;
    auto operator=(const multi_group_network_server&) -> multi_group_network_server& = delete;
    multi_group_network_server(multi_group_network_server&&) = delete;
    auto operator=(multi_group_network_server&&) -> multi_group_network_server& = delete;

    ~multi_group_network_server() = default;

    // ── group registry ───────────────────────────────────────────────────────

    /// @brief Install (or replace) the handler table for one group.
    ///
    /// Called once per `register_*_handler` by `group_scoped_server`, which
    /// accumulates the table on its own side and re-publishes the whole thing
    /// — so a partially registered group is never observable mid-way through
    /// `node<Types>::register_rpc_handlers()`.
    auto register_group(const GroupId& group, handlers_type handlers) -> void {
        auto shared = std::make_shared<const handlers_type>(std::move(handlers));
        _groups.wlock()->insert_or_assign(group, std::move(shared));
    }

    /// @brief Remove a group's handlers. Returns `true` if it was registered.
    auto unregister_group(const GroupId& group) -> bool {
        return _groups.wlock()->erase(group) > 0;
    }

    [[nodiscard]] auto has_group(const GroupId& group) const -> bool {
        return _groups.rlock()->contains(group);
    }

    [[nodiscard]] auto group_count() const -> std::size_t { return _groups.rlock()->size(); }

    // ── the two escape hatches for messages with no local replica ────────────

    /// @brief Called for a message whose group has no local handler table.
    ///
    /// Consulted only after the tombstone predicate has said the group is not
    /// already destroyed here.
    auto set_unknown_group_handler(std::function<unknown_group_action(const GroupId&)> handler)
        -> void {
        _unknown_group = std::move(handler);
    }

    /// @brief Predicate answering "was this group destroyed on this node?".
    ///
    /// Deliberately a predicate rather than a reference to `tombstone_set`:
    /// the transport has no business depending on the storage layer, and
    /// inverting that dependency for one membership test would be the wrong
    /// trade. The host wires its durable tombstone set in here.
    ///
    /// Without this check, a partitioned peer's stale `AppendEntries` for a
    /// merged-away group re-creates a replica whose range someone else now
    /// owns — a silent double-ownership bug that the tiling invariant would
    /// only catch long after the damage.
    auto set_tombstone_predicate(std::function<bool(const GroupId&)> predicate) -> void {
        _is_tombstoned = std::move(predicate);
    }

    /// @brief Called for every dispatched message, before the group's handler.
    ///
    /// The host uses this as its wake signal: an inbound RPC is one of the
    /// conditions that must take a group out of hibernation (Requirement 5.5),
    /// and the transport is the only place that sees every one of them. Kept as
    /// one hook rather than a hook per RPC type because the host only needs to
    /// know *that* the group was addressed, not how.
    ///
    /// Runs on the transport's own thread and must be cheap — it is on the
    /// path of every message for every group.
    auto set_message_observer(std::function<void(const GroupId&)> observer) -> void {
        _observer = std::move(observer);
    }

    /// @brief Count of messages dropped because their group is tombstoned or
    /// unplaceable here. The host publishes this as `stale_group_message`.
    [[nodiscard]] auto stale_group_message_count() const -> std::uint64_t {
        return _stale_group_messages.load(std::memory_order_relaxed);
    }

    // ── lifecycle ────────────────────────────────────────────────────────────

    /// @brief Install one handler per RPC type on the inner server and start it.
    auto start() -> void {
        _inner.register_request_vote_handler(
            [this](const typename Messages::request_vote_request_type& req) {
                return dispatch(
                    req, [](const handlers_type& h) -> const auto& { return h._request_vote; },
                    [&req] {
                        // A vote refusal at the requester's own term: it neither
                        // grants nor bumps anything, so an unplaceable message
                        // costs the candidate one wasted round trip rather than
                        // a spurious term change.
                        return typename Messages::request_vote_response_type{
                            ._term = req.term(),
                            ._vote_granted = false,
                            ._group_id = req.group_id()};
                    });
            });

        if constexpr (network_server_with_pre_vote<Server>) {
            _inner.register_request_pre_vote_handler(
                [this](const typename Messages::request_pre_vote_request_type& req) {
                    return dispatch(
                        req,
                        [](const handlers_type& h) -> const auto& { return h._request_pre_vote; },
                        [&req] {
                            return typename Messages::request_pre_vote_response_type{
                                ._term = req.term(),
                                ._vote_granted = false,
                                ._group_id = req.group_id()};
                        });
                });
        }

        _inner.register_append_entries_handler(
            [this](const typename Messages::append_entries_request_type& req) {
                return dispatch(
                    req, [](const handlers_type& h) -> const auto& { return h._append_entries; },
                    [&req] {
                        return typename Messages::append_entries_response_type{
                            ._term = req.term(),
                            ._success = false,
                            ._conflict_index = std::nullopt,
                            ._conflict_term = std::nullopt,
                            ._group_id = req.group_id()};
                    });
            });

        _inner.register_install_snapshot_handler(
            [this](const typename Messages::install_snapshot_request_type& req) {
                return dispatch(
                    req, [](const handlers_type& h) -> const auto& { return h._install_snapshot; },
                    [&req] {
                        return typename Messages::install_snapshot_response_type{
                            ._term = req.term(), ._group_id = req.group_id()};
                    });
            });

        if constexpr (network_server_with_log_fetch<Server>) {
            _inner.register_fetch_log_entries_handler(
                [this](const typename Messages::fetch_log_entries_request_type& req) {
                    return dispatch(
                        req,
                        [](const handlers_type& h) -> const auto& { return h._fetch_log_entries; },
                        [&req] {
                            // `_available = false` is the peer-to-peer path's
                            // own "I cannot serve this" answer, so an unknown
                            // group looks to the requester exactly like a peer
                            // that has compacted past the range.
                            return typename Messages::fetch_log_entries_response_type{
                                ._responder_id = 0,
                                ._available = false,
                                ._prev_log_term = 0,
                                ._entries = {},
                                ._group_id = req.group_id()};
                        });
                });
        }

        _inner.start();
    }

    auto stop() -> void { _inner.stop(); }

    [[nodiscard]] auto is_running() const -> bool { return _inner.is_running(); }

    /// @brief The wrapped server, for the whole-node RPCs that carry no group
    /// id — cluster join and leave (see `group_scoped_server`'s note).
    [[nodiscard]] auto inner() -> Server& { return _inner; }

private:
    /// @brief Look the group up, run its handler, and fall back when it has none.
    ///
    /// The handler table is copied out as a `shared_ptr` under a read lock and
    /// the lock is released *before* the handler runs. Holding it across the
    /// call would deadlock the first time a handler registered a group — which
    /// is exactly what the split apply path does.
    template<typename Request, typename Select, typename Fallback>
    auto dispatch(const Request& req, Select select, Fallback fallback) {
        const auto group = req.group_id();
        if (_observer) {
            _observer(group);
        }

        if (auto handlers = lookup(group); handlers) {
            const auto& fn = select(*handlers);
            // An empty slot is an optional RPC this group's node never
            // registered — not a stale group, so it is not counted as one.
            return fn ? fn(req) : fallback();
        }

        // Destroyed here by a merge or a replica removal. Dropping is the whole
        // point: re-creating the replica would give two shards the same range.
        if (_is_tombstoned && _is_tombstoned(group)) {
            _stale_group_messages.fetch_add(1, std::memory_order_relaxed);
            return fallback();
        }

        if (_unknown_group && _unknown_group(group) == unknown_group_action::created) {
            if (auto created = lookup(group); created) {
                const auto& fn = select(*created);
                return fn ? fn(req) : fallback();
            }
            // The host said it created the replica and it is not there. That is
            // a host bug, not a stale peer, but counting it keeps the anomaly
            // visible rather than silently dropping the message.
        }

        _stale_group_messages.fetch_add(1, std::memory_order_relaxed);
        return fallback();
    }

    [[nodiscard]] auto lookup(const GroupId& group) const -> std::shared_ptr<const handlers_type> {
        auto locked = _groups.rlock();
        auto it = locked->find(group);
        return it == locked->end() ? nullptr : it->second;
    }

    Server _inner;
    synchronized<std::unordered_map<GroupId, std::shared_ptr<const handlers_type>>> _groups;
    std::function<unknown_group_action(const GroupId&)> _unknown_group;
    std::function<bool(const GroupId&)> _is_tombstoned;
    std::function<void(const GroupId&)> _observer;
    std::atomic<std::uint64_t> _stale_group_messages{0};
};

/// @brief A `network_server` view of one group inside a shared server.
///
/// Satisfies `network_server` (and, conditionally, the pre-vote and log-fetch
/// extensions) by accumulating the handler table locally and re-publishing the
/// whole thing into the demultiplexer on every registration. `start()` and
/// `stop()` are no-ops on purpose — the shared server is started once by the
/// host, not once per group, and a group that stopped it would take every
/// other group down with it.
///
/// **Cluster join and leave are deliberately absent.** Those RPCs are routed
/// by address rather than by node id precisely because the joining node does
/// not yet know its id, and they carry no group id to demultiplex on. Bootstrap
/// is therefore a whole-node operation that the host wires directly onto
/// `multi_group_network_server::inner()`. Because both are optional extension
/// concepts, `node<Types>` detects their absence with `if constexpr` and simply
/// does not use them.
///
/// This is a non-owning view: it holds a pointer to the demultiplexer, which
/// the host keeps alive for the process's lifetime. It does **not** unregister
/// in its destructor — it is moved into `node_config` and then into
/// `node<Types>`, and a destructor that unregistered would tear the group's
/// handlers down on the way past.
template<typename Demux> class group_scoped_server {
public:
    using group_id_type = typename Demux::group_id_type;
    using messages_type = typename Demux::messages_type;
    using handlers_type = typename Demux::handlers_type;

    group_scoped_server() = default;
    group_scoped_server(Demux& demux, group_id_type group)
        : _demux(&demux), _group(std::move(group)) {}

    auto register_request_vote_handler(
        std::function<typename messages_type::request_vote_response_type(
            const typename messages_type::request_vote_request_type&)>
            handler) -> void {
        _handlers._request_vote = std::move(handler);
        publish();
    }

    auto register_request_pre_vote_handler(
        std::function<typename messages_type::request_pre_vote_response_type(
            const typename messages_type::request_pre_vote_request_type&)>
            handler) -> void {
        _handlers._request_pre_vote = std::move(handler);
        publish();
    }

    auto register_append_entries_handler(
        std::function<typename messages_type::append_entries_response_type(
            const typename messages_type::append_entries_request_type&)>
            handler) -> void {
        _handlers._append_entries = std::move(handler);
        publish();
    }

    auto register_install_snapshot_handler(
        std::function<typename messages_type::install_snapshot_response_type(
            const typename messages_type::install_snapshot_request_type&)>
            handler) -> void {
        _handlers._install_snapshot = std::move(handler);
        publish();
    }

    auto register_fetch_log_entries_handler(
        std::function<typename messages_type::fetch_log_entries_response_type(
            const typename messages_type::fetch_log_entries_request_type&)>
            handler) -> void {
        _handlers._fetch_log_entries = std::move(handler);
        publish();
    }

    /// @brief No-op: the shared server's lifecycle belongs to the host.
    auto start() -> void {}
    /// @brief No-op: see `start()`.
    auto stop() -> void {}

    [[nodiscard]] auto is_running() const -> bool {
        return _demux != nullptr && _demux->is_running();
    }

    [[nodiscard]] auto group_id() const -> const group_id_type& { return _group; }

private:
    auto publish() -> void {
        if (_demux != nullptr) {
            _demux->register_group(_group, _handlers);
        }
    }

    Demux* _demux{nullptr};
    group_id_type _group{};
    handlers_type _handlers{};
};

/// @brief A `network_client` view of one group over a shared client.
///
/// Stamps `_group_id` on every outbound request and forwards the rest
/// verbatim. The optional extensions are declared with trailing
/// `requires`-clauses rather than as unconditional members, so that
/// `network_client_with_pre_vote<group_scoped_client<C>>` is true exactly when
/// it is true of `C` — an unconditional member that failed to compile would
/// make the detection lie in the other direction.
///
/// Non-owning: the host owns the one shared client per process, which is what
/// makes connection reuse across groups automatic.
template<typename Client, raft_group_id GroupId = std::uint64_t,
         typename Messages = group_rpc_messages<>>
class group_scoped_client {
public:
    using client_type = Client;
    using group_id_type = GroupId;
    using messages_type = Messages;

    group_scoped_client() = default;
    group_scoped_client(Client& inner, GroupId group) : _inner(&inner), _group(std::move(group)) {}

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group; }

    // ── the three required by `network_client` ───────────────────────────────

    auto send_request_vote(typename Messages::node_id_type target,
                           const typename Messages::request_vote_request_type& req,
                           std::chrono::milliseconds timeout) {
        return _inner->send_request_vote(target, stamped(req), timeout);
    }

    auto send_append_entries(typename Messages::node_id_type target,
                             const typename Messages::append_entries_request_type& req,
                             std::chrono::milliseconds timeout) {
        return _inner->send_append_entries(target, stamped(req), timeout);
    }

    auto send_install_snapshot(typename Messages::node_id_type target,
                               const typename Messages::install_snapshot_request_type& req,
                               std::chrono::milliseconds timeout) {
        return _inner->send_install_snapshot(target, stamped(req), timeout);
    }

    // ── optional extensions, conditionally present ───────────────────────────

    auto send_request_pre_vote(typename Messages::node_id_type target,
                               const typename Messages::request_pre_vote_request_type& req,
                               std::chrono::milliseconds timeout)
    requires network_client_with_pre_vote<Client>
    {
        return _inner->send_request_pre_vote(target, stamped(req), timeout);
    }

    auto send_fetch_log_entries(typename Messages::node_id_type target,
                                const typename Messages::fetch_log_entries_request_type& req,
                                std::chrono::milliseconds timeout)
    requires network_client_with_log_fetch<Client>
    {
        return _inner->send_fetch_log_entries(target, stamped(req), timeout);
    }

    /// @brief Forwarded unstamped: `cluster_join_request` carries no group id.
    ///
    /// Bootstrap is a whole-node operation addressed by *address* — the joining
    /// node does not yet know its own id, let alone which groups it will hold.
    /// Forwarding verbatim keeps the extension concept satisfied without
    /// pretending the message is group-scoped.
    auto send_cluster_join_request(const std::string& address, const cluster_join_request<>& req,
                                   std::chrono::milliseconds timeout)
    requires network_client_with_cluster_join<Client>
    {
        return _inner->send_cluster_join_request(address, req, timeout);
    }

    /// @brief Forwarded unstamped; see `send_cluster_join_request`.
    auto send_cluster_leave_request(const std::string& address, const cluster_leave_request<>& req,
                                    std::chrono::milliseconds timeout)
    requires network_client_with_cluster_leave<Client>
    {
        return _inner->send_cluster_leave_request(address, req, timeout);
    }

private:
    template<typename Request> [[nodiscard]] auto stamped(const Request& req) const -> Request {
        Request copy = req;
        copy._group_id = _group;
        return copy;
    }

    Client* _inner{nullptr};
    GroupId _group{};
};

}  // namespace kythira
