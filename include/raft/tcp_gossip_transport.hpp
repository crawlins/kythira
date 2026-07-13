#pragma once

/// @file tcp_gossip_transport.hpp
/// @brief `tcp_gossip_peer2peer_replicator` — a real, network-based
/// `peer2peer_replicator` implementation using a classic anti-entropy gossip
/// protocol (periodic randomized push-pull digest exchange, à la Cassandra/
/// Dynamo). See `.kiro/specs/peer2peer-gossip-transport/`.
///
/// Self-contained: its own TCP listener, its own background gossip thread,
/// its own small `boost::json` wire protocol — entirely independent of
/// whatever `network_client_type`/`network_server_type` the owning
/// `node<Types>` is configured with (Requirement 3).

#include <raft/peer2peer_replication.hpp>
#include <raft/tcp_rpc.hpp>

#include <folly/Synchronized.h>
#include <boost/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kythira {

// Requirement 5.1 — never carries log entries, only enough to answer "who
// might have what I'm missing."
template<typename NodeId, typename Address, typename LogIndex> struct gossip_digest {
    NodeId node_id;
    Address address;  ///< This peer's Raft RPC address, NOT its gossip-listener
                      ///< address (Requirement 7.1).
    std::uint64_t term;
    LogIndex last_log_index;
    std::int64_t fresh_until;  ///< Epoch seconds; Requirement 6.4.
};

// Implementation-file-local, not added to types.hpp/raft_types (Requirement
// 5.1) — this is not a Raft consensus RPC. gossip_exchange_request and
// gossip_exchange_response are both this same shape — the protocol is
// symmetric push-pull, so request and response carry identical information
// in opposite directions.
template<typename NodeId, typename Address, typename LogIndex> struct gossip_exchange_message {
    NodeId sender_node_id;
    std::vector<gossip_digest<NodeId, Address, LogIndex>> digests;
};

namespace gossip_detail {

template<typename NodeId> auto node_id_to_json(const NodeId& id) -> boost::json::value {
    if constexpr (std::is_same_v<NodeId, std::string>) {
        return boost::json::value(id);
    } else {
        return boost::json::value(static_cast<std::int64_t>(id));
    }
}

template<typename NodeId> auto node_id_from_json(const boost::json::value& v) -> NodeId {
    if constexpr (std::is_same_v<NodeId, std::string>) {
        return std::string(v.as_string());
    } else {
        return static_cast<NodeId>(v.as_int64());
    }
}

}  // namespace gossip_detail

// Requirement 5.2 — boost::json directly, following ca_state_machine's
// existing command-encoding convention rather than extending json_rpc_serializer
// (scoped to the three core Raft RPCs).
template<typename NodeId, typename Address, typename LogIndex>
auto encode_gossip_message(const gossip_exchange_message<NodeId, Address, LogIndex>& msg)
    -> std::string {
    boost::json::object obj;
    obj["sender_node_id"] = gossip_detail::node_id_to_json(msg.sender_node_id);

    boost::json::array arr;
    for (const auto& d : msg.digests) {
        boost::json::object dobj;
        dobj["node_id"] = gossip_detail::node_id_to_json(d.node_id);
        dobj["address"] = std::string(d.address);
        dobj["term"] = d.term;
        dobj["last_log_index"] = static_cast<std::int64_t>(d.last_log_index);
        dobj["fresh_until"] = d.fresh_until;
        arr.push_back(std::move(dobj));
    }
    obj["digests"] = std::move(arr);

    return boost::json::serialize(obj);
}

template<typename NodeId, typename Address, typename LogIndex>
auto decode_gossip_message(const std::string& json_str)
    -> gossip_exchange_message<NodeId, Address, LogIndex> {
    auto parsed = boost::json::parse(json_str);
    const auto& obj = parsed.as_object();

    gossip_exchange_message<NodeId, Address, LogIndex> msg;
    msg.sender_node_id = gossip_detail::node_id_from_json<NodeId>(obj.at("sender_node_id"));

    for (const auto& dv : obj.at("digests").as_array()) {
        const auto& dobj = dv.as_object();
        gossip_digest<NodeId, Address, LogIndex> d;
        d.node_id = gossip_detail::node_id_from_json<NodeId>(dobj.at("node_id"));
        d.address = Address(std::string(dobj.at("address").as_string()));
        d.term = static_cast<std::uint64_t>(dobj.at("term").as_int64());
        d.last_log_index = static_cast<LogIndex>(dobj.at("last_log_index").as_int64());
        d.fresh_until = dobj.at("fresh_until").as_int64();
        msg.digests.push_back(std::move(d));
    }

    return msg;
}

/// @brief Configuration for `tcp_gossip_peer2peer_replicator`.
/// @tparam NodeId  Node identifier type.
/// @tparam Address Network address type (`"host:port"`).
template<typename NodeId, typename Address> struct tcp_gossip_config {
    // Requirement 2.1: address resolution only — NOT a statement of current
    // membership. cluster_configuration<NodeId> carries no addresses, so this
    // is still supplied out-of-band, but current membership itself comes
    // exclusively from update_membership() calls (Requirement 2.2), not from
    // this list.
    std::vector<peer_info<NodeId, Address>> address_book;
    std::uint16_t listen_port{0};                          ///< This node's gossip listener port.
    std::size_t fanout{3};                                 ///< Peers contacted per round.
    std::chrono::milliseconds gossip_round_interval{500};  ///< Cadence between rounds.
    std::chrono::seconds freshness_interval{5};            ///< TTL for a digest since last refresh.
};

namespace gossip_detail {

inline auto epoch_seconds_now() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

template<typename Address>
auto split_host_port(const Address& address) -> std::pair<std::string, std::uint16_t> {
    std::string addr(address);
    auto pos = addr.rfind(':');
    if (pos == std::string::npos) {
        return {addr, std::uint16_t{0}};
    }
    return {addr.substr(0, pos), static_cast<std::uint16_t>(std::stoi(addr.substr(pos + 1)))};
}

}  // namespace gossip_detail

/// @brief Real, network-based `peer2peer_replicator` — anti-entropy gossip
/// over a self-contained TCP channel.
///
/// See `.kiro/specs/peer2peer-gossip-transport/requirements.md` for the full
/// requirement set this class satisfies.
///
/// Construction is cheap and never starts a thread or opens a socket — the
/// background TCP listener and gossip thread only start when `start()` is
/// called explicitly (by `node<Types>::start()`, once this object has
/// reached its final resting address inside `node<Types>`; detected
/// structurally via `if constexpr (requires { _peer2peer_replicator.start(); })`
/// in `raft.hpp`, so `peer2peer_replicator_type` implementations without a
/// `start()`/`stop()` pair, like `no_op_peer2peer_replicator` and
/// `static_peer2peer_replicator`, are unaffected). This deliberately deviates
/// from a literal "start on construction" reading of Requirement 4.1: this
/// object is constructed once as a `node_config<Types>` field and then moved
/// exactly once into `node<Types>`'s own member before `node<Types>::start()`
/// ever runs, so deferring thread-start to that point means the object never
/// needs to be moved *after* any thread exists — the ordinary defaulted move
/// constructor/assignment (moving `_cfg`/`_self_id`/`_table`/`_active_members`
/// and leaving the not-yet-used thread/mutex/atomic members to their default
/// member initializers) is then sufficient, with no `shared_ptr`/pimpl
/// indirection required.
///
/// @tparam NodeId   Node identifier type.
/// @tparam Address  Network address type (`"host:port"`); must be constructible
///                  from and convertible to `std::string`.
/// @tparam LogIndex Log index type.
template<typename NodeId, typename Address, typename LogIndex>
class tcp_gossip_peer2peer_replicator {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using log_index_type = LogIndex;

    explicit tcp_gossip_peer2peer_replicator(tcp_gossip_config<NodeId, Address> cfg)
        : _cfg(std::move(cfg)) {}

    ~tcp_gossip_peer2peer_replicator() { stop(); }

    tcp_gossip_peer2peer_replicator(const tcp_gossip_peer2peer_replicator&) = delete;
    auto operator=(const tcp_gossip_peer2peer_replicator&)
        -> tcp_gossip_peer2peer_replicator& = delete;

    // Legal only before start() has been called — matches this object's only
    // real usage pattern (built as a node_config<Types> field, moved once
    // into node<Types>'s member, and only then started). The move constructor
    // deliberately only transfers _cfg/_self_id/_table/_active_members; the
    // thread/mutex/condvar/atomic members are left to their default member
    // initializers since they hold no meaningful state pre-start().
    tcp_gossip_peer2peer_replicator(tcp_gossip_peer2peer_replicator&& other) noexcept
        : _cfg(std::move(other._cfg)),
          _self_id(std::move(other._self_id)),
          _table(std::move(other._table)),
          _active_members(std::move(other._active_members)) {}

    auto operator=(tcp_gossip_peer2peer_replicator&& other) noexcept
        -> tcp_gossip_peer2peer_replicator& {
        if (this != &other) {
            _cfg = std::move(other._cfg);
            _self_id = std::move(other._self_id);
            _table = std::move(other._table);
            _active_members = std::move(other._active_members);
        }
        return *this;
    }

    // Requirement 4.1 (see class comment) — starts the background TCP
    // listener and gossip thread. Idempotent; safe to call again after
    // stop() (supports node<Types>'s own restart-after-stop() semantics).
    // Requirement 4.4's "fail loudly" now surfaces from this call (propagated
    // through node<Types>::start()) rather than from the constructor.
    auto start() -> void {
        if (_started.exchange(true)) {
            return;
        }
        start_listener();
        start_gossip_thread();
    }

    // Signals both background threads to stop and joins them. Idempotent.
    auto stop() -> void {
        if (!_started.exchange(false)) {
            return;
        }
        stop_gossip_thread();
        stop_listener();
    }

    // Requirement 1.2 — updates the local table only; resolves immediately.
    // The actual network dissemination happens asynchronously on the
    // background gossip thread's own schedule, never synchronously here.
    auto advertise_progress(NodeId self_id, Address self_address, std::uint64_t term,
                            LogIndex last_log_index) -> kythira::Future<void> {
        auto fresh_until =
            gossip_detail::epoch_seconds_now() +
            std::chrono::duration_cast<std::chrono::seconds>(_cfg.freshness_interval).count();
        {
            auto locked = _table.wlock();
            (*locked)[self_id] = gossip_digest<NodeId, Address, LogIndex>{
                self_id, self_address, term, last_log_index, fresh_until};
        }
        *_self_id.wlock() = self_id;
        return kythira::FutureFactory::makeFuture();
    }

    // Requirement 1.3 — pure local-table read, filtered by _active_members,
    // never itself performing network I/O. Excludes this node's own entry.
    auto find_catch_up_source(LogIndex from_index, LogIndex /*to_index*/,
                              std::chrono::milliseconds) const
        -> kythira::Future<std::optional<peer_info<NodeId, Address>>> {
        auto self = _self_id.rlock();
        auto members = _active_members.rlock();
        auto locked = _table.rlock();
        auto now = gossip_detail::epoch_seconds_now();

        for (const auto& [id, digest] : *locked) {
            if (self->has_value() && id == self->value()) {
                continue;
            }
            if (digest.fresh_until < now) {
                continue;  // Requirement 1.3/6.3: skip already-expired entries.
            }
            if (!members->contains(id)) {
                continue;
            }
            if (digest.last_log_index >= from_index) {
                return kythira::FutureFactory::makeFuture(std::optional<peer_info<NodeId, Address>>{
                    peer_info<NodeId, Address>{id, digest.address}});
            }
        }
        return kythira::FutureFactory::makeFuture(std::optional<peer_info<NodeId, Address>>{});
    }

    // Requirement 1.4/2.2 — replaces _active_members; resolves immediately.
    // This, not address_book, is this instance's source of truth for "who is
    // currently a cluster member."
    auto update_membership(std::vector<NodeId> member_ids) -> kythira::Future<void> {
        *_active_members.wlock() = std::unordered_set<NodeId>(member_ids.begin(), member_ids.end());
        return kythira::FutureFactory::makeFuture();
    }

    // ── Pure local-table/membership logic ────────────────────────────────────
    // Exposed publicly (rather than kept private) specifically so Requirement
    // 10.1's unit tests can exercise this safety-relevant logic directly,
    // without any network I/O — merge()/prune_expired()/eligible_peers() have
    // no unsafe side effects beyond mutating this instance's own local state.

    // Requirement 6.1: higher (term, last_log_index) wins per node_id; a
    // node_id not yet present is always added.
    auto merge(const std::vector<gossip_digest<NodeId, Address, LogIndex>>& incoming) -> void {
        auto locked = _table.wlock();
        for (const auto& d : incoming) {
            auto it = locked->find(d.node_id);
            if (it == locked->end() || std::tie(d.term, d.last_log_index) >
                                           std::tie(it->second.term, it->second.last_log_index)) {
                (*locked)[d.node_id] = d;
            }
        }
    }

    // Requirement 6.3 — removes every entry whose fresh_until has passed.
    auto prune_expired() -> void {
        auto now = gossip_detail::epoch_seconds_now();
        auto locked = _table.wlock();
        for (auto it = locked->begin(); it != locked->end();) {
            if (it->second.fresh_until < now) {
                it = locked->erase(it);
            } else {
                ++it;
            }
        }
    }

    // Requirement 2.3: intersection of the current active-membership set and
    // address_book's keys — a member with no known address, or an
    // address_book entry for a non-member, is excluded either way.
    [[nodiscard]] auto eligible_peers() const -> std::vector<peer_info<NodeId, Address>> {
        auto members = _active_members.rlock();
        std::vector<peer_info<NodeId, Address>> result;
        for (const auto& peer : _cfg.address_book) {
            if (members->contains(peer.node_id)) {
                result.push_back(peer);
            }
        }
        return result;
    }

    [[nodiscard]] auto snapshot_table() const
        -> std::vector<gossip_digest<NodeId, Address, LogIndex>> {
        auto locked = _table.rlock();
        std::vector<gossip_digest<NodeId, Address, LogIndex>> result;
        result.reserve(locked->size());
        for (const auto& [id, d] : *locked) {
            result.push_back(d);
        }
        return result;
    }

private:
    // ── Gossip round (Requirement 4) ─────────────────────────────────────────

    auto start_gossip_thread() -> void {
        _gossip_running = true;
        _gossip_thread = std::thread([this] { gossip_loop(); });
    }

    auto stop_gossip_thread() -> void {
        {
            std::lock_guard<std::mutex> lk(_gossip_mu);
            _gossip_running = false;
        }
        _gossip_cv.notify_all();
        if (_gossip_thread.joinable()) {
            _gossip_thread.join();
        }
    }

    auto gossip_loop() -> void {
        std::unique_lock<std::mutex> lk(_gossip_mu);
        while (_gossip_running) {
            if (_gossip_cv.wait_for(lk, _cfg.gossip_round_interval,
                                    [this] { return !_gossip_running.load(); })) {
                break;
            }
            lk.unlock();
            try {
                run_one_round();
            } catch (...) {
                // Requirement 8.1/8.2 — a round's own failures never propagate
                // out of the gossip thread.
            }
            lk.lock();
        }
    }

    // Requirement 4.2/6.3 — one full round: prune, select fanout, exchange.
    auto run_one_round() -> void {
        prune_expired();

        auto peers = eligible_peers();
        if (auto self = _self_id.rlock(); self->has_value()) {
            auto self_id = self->value();
            peers.erase(std::remove_if(peers.begin(), peers.end(),
                                       [&](const auto& p) { return p.node_id == self_id; }),
                        peers.end());
        }

        if (peers.empty()) {
            // Requirement 4.2 — no eligible peers this round: silent no-op.
            return;
        }

        std::size_t fanout = std::min(_cfg.fanout, peers.size());
        std::vector<std::size_t> indices(peers.size());
        std::iota(indices.begin(), indices.end(), std::size_t{0});
        std::shuffle(indices.begin(), indices.end(), _rng);

        for (std::size_t i = 0; i < fanout; ++i) {
            exchange_with(peers[indices[i]]);
        }
    }

    // Requirement 5/8: one push-pull round-trip with a single peer.
    auto exchange_with(const peer_info<NodeId, Address>& peer) -> void {
        std::optional<NodeId> self_id;
        if (auto self = _self_id.rlock(); self->has_value()) {
            self_id = self->value();
        }
        if (!self_id.has_value()) {
            // advertise_progress() hasn't been called yet — nothing meaningful
            // to send as sender_node_id; skip this round's exchange.
            return;
        }

        auto [host, port] = gossip_detail::split_host_port(peer.address);
        int fd = tcp_detail::connect_to(host, port, _cfg.gossip_round_interval);
        if (fd < 0) {
            // Requirement 8.1 — logged (best-effort; no logger dependency is
            // injected into this class), this round's exchange with this peer
            // simply doesn't happen; other peers in this round are unaffected.
            return;
        }
        struct Guard {
            int fd;
            ~Guard() {
                if (fd >= 0) ::close(fd);
            }
        } guard{fd};

        gossip_exchange_message<NodeId, Address, LogIndex> request{*self_id, snapshot_table()};
        if (!tcp_detail::frame_send(fd, encode_gossip_message(request))) {
            return;
        }

        auto raw = tcp_detail::frame_recv(fd);
        if (!raw.has_value()) {
            return;  // Requirement 8.1 — malformed/closed, no-op.
        }

        try {
            auto response = decode_gossip_message<NodeId, Address, LogIndex>(*raw);
            merge(response.digests);
        } catch (...) {
            // Requirement 5.4/8.1 — undecodable response treated identically
            // to a connection failure.
        }
    }

    // ── TCP listener (Requirement 3) ─────────────────────────────────────────

    auto start_listener() -> void {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error("tcp_gossip_peer2peer_replicator: socket()");
        }

        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(_cfg.listen_port);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            throw std::runtime_error("tcp_gossip_peer2peer_replicator: bind() on port " +
                                     std::to_string(_cfg.listen_port));
        }
        ::listen(fd, 256);
        _listen_fd = fd;
        _listener_running = true;
        _listener_thread = std::thread([this] { accept_loop(); });
    }

    auto stop_listener() -> void {
        if (!_listener_running.exchange(false)) {
            return;
        }
        int fd = _listen_fd.exchange(-1);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        if (_listener_thread.joinable()) {
            _listener_thread.join();
        }
    }

    auto accept_loop() -> void {
        while (_listener_running) {
            int client = ::accept(_listen_fd.load(), nullptr, nullptr);
            if (client < 0) {
                break;
            }
            std::thread([this, client] {
                handle_incoming_exchange(client);
                ::close(client);
            }).detach();
        }
    }

    // Requirement 3.2/5.3/5.4: reuses tcp_detail framing; a malformed request
    // closes the connection without a response rather than crashing.
    auto handle_incoming_exchange(int fd) -> void {
        auto raw = tcp_detail::frame_recv(fd);
        if (!raw.has_value()) {
            return;
        }

        gossip_exchange_message<NodeId, Address, LogIndex> request;
        try {
            request = decode_gossip_message<NodeId, Address, LogIndex>(*raw);
        } catch (...) {
            return;  // Requirement 5.4 — undecodable, close without responding.
        }

        merge(request.digests);

        NodeId responder_id{};
        if (auto self = _self_id.rlock(); self->has_value()) {
            responder_id = self->value();
        }
        gossip_exchange_message<NodeId, Address, LogIndex> response{responder_id, snapshot_table()};
        tcp_detail::frame_send(fd, encode_gossip_message(response));
    }

    tcp_gossip_config<NodeId, Address> _cfg;

    folly::Synchronized<std::optional<NodeId>> _self_id;
    folly::Synchronized<std::unordered_map<NodeId, gossip_digest<NodeId, Address, LogIndex>>>
        _table;
    // Requirement 2.2: empty until the first update_membership() call —
    // deliberately NOT initialized from _cfg.address_book's keys, since
    // address_book is address-resolution data, not a membership statement.
    folly::Synchronized<std::unordered_set<NodeId>> _active_members;

    std::mt19937 _rng{std::random_device{}()};  // only ever touched by the gossip thread.

    std::atomic<bool> _gossip_running{false};
    std::mutex _gossip_mu;
    std::condition_variable _gossip_cv;
    std::thread _gossip_thread;

    std::atomic<bool> _listener_running{false};
    std::atomic<int> _listen_fd{-1};
    std::thread _listener_thread;

    std::atomic<bool> _started{false};
};

static_assert(
    peer2peer_replicator<tcp_gossip_peer2peer_replicator<std::uint64_t, std::string, std::uint64_t>,
                         std::uint64_t, std::string, std::uint64_t>);

}  // namespace kythira
