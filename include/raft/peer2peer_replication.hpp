#pragma once

/// @file peer2peer_replication.hpp
/// @brief Peer-to-peer catch-up replicator concept and built-in implementations.
///
/// See `.kiro/specs/peer2peer-log-replication/`. Lets a lagging `node<Types>`
/// pull missing log entries from a peer that already has them instead of
/// exclusively from the leader. This header defines only the abstract
/// concept plus a no-op default (zero behavioral change when not opted in)
/// and an in-memory reference/test implementation
/// (`static_peer2peer_replicator`); a real network-based gossip transport is
/// `tcp_gossip_peer2peer_replicator` (`.kiro/specs/peer2peer-gossip-transport/`,
/// `include/raft/tcp_gossip_transport.hpp`).

#include <raft/future.hpp>
#include <raft/peer_discovery.hpp>

#include <folly/Synchronized.h>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kythira {

/// @brief Concept for a peer-to-peer catch-up replicator back-end.
///
/// Implementations advertise this node's own replication progress, answer
/// "who might have entries I'm missing," and track the replicated log's own
/// core cluster membership (Requirement 11 — the replicator's own notion of
/// "who's in the cluster" is driven exclusively by `update_membership`, never
/// separately/independently maintained).
///
/// @tparam P        Concrete peer2peer-replicator type.
/// @tparam NodeId   Must satisfy `node_id`.
/// @tparam Address  Network address type.
/// @tparam LogIndex Must satisfy `log_index`.
template<typename P, typename NodeId, typename Address, typename LogIndex>
concept peer2peer_replicator =
    requires(P& replicator, NodeId self_id, Address self_address, std::uint64_t term,
             LogIndex last_log_index, LogIndex from_index, LogIndex to_index,
             std::chrono::milliseconds timeout, std::vector<NodeId> member_ids) {
        /// Publish this node's own progress digest. Never blocks or delays any
        /// Raft-critical operation — resolves immediately; actual dissemination
        /// (if any) happens on the implementation's own schedule.
        {
            replicator.advertise_progress(self_id, self_address, term, last_log_index)
        } -> std::same_as<kythira::Future<void>>;
        /// Return a peer believed, from gossiped digests, to hold entries
        /// covering `[from_index, to_index]`, or `std::nullopt` if none is known.
        {
            replicator.find_catch_up_source(from_index, to_index, timeout)
        } -> std::same_as<kythira::Future<std::optional<peer_info<NodeId, Address>>>>;
        /// Replace this instance's notion of current core cluster membership.
        /// This is the replicator's *only* source of truth for "who's in the
        /// cluster" — no separately/independently maintained peer list.
        { replicator.update_membership(member_ids) } -> std::same_as<kythira::Future<void>>;
    };

/// @brief No-op peer-to-peer replicator — preserves today's leader-only
/// replication behavior byte-for-byte for any `Types` bundle that does not
/// opt in to `peer2peer_replicator_type`.
///
/// @tparam NodeId   Node identifier type.
/// @tparam Address  Network address type.
/// @tparam LogIndex Log index type.
template<typename NodeId, typename Address, typename LogIndex> class no_op_peer2peer_replicator {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using log_index_type = LogIndex;

    /// @brief No-op; always succeeds immediately.
    auto advertise_progress(NodeId, Address, std::uint64_t, LogIndex) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }

    /// @brief Always resolves `std::nullopt` — this alone guarantees zero
    /// behavioral change when a `Types` bundle does not declare a real
    /// `peer2peer_replicator_type`.
    auto find_catch_up_source(LogIndex, LogIndex, std::chrono::milliseconds) const
        -> kythira::Future<std::optional<peer_info<NodeId, Address>>> {
        return kythira::FutureFactory::makeFuture(std::optional<peer_info<NodeId, Address>>{});
    }

    /// @brief No-op; always succeeds immediately.
    auto update_membership(std::vector<NodeId>) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }
};

/// @brief Deterministic, in-memory `peer2peer_replicator` for simulator-based
/// tests — not a real gossip transport (see `tcp_gossip_peer2peer_replicator`
/// for that).
///
/// Construct one shared table (`std::make_shared<table_type>()`) and pass a
/// `static_peer2peer_replicator` instance holding that same `shared_ptr` to
/// every simulated node in a test cluster, mirroring how `static_peer_discovery`
/// and the network simulator's own shared registries work.
///
/// @tparam NodeId   Node identifier type.
/// @tparam Address  Network address type.
/// @tparam LogIndex Log index type.
template<typename NodeId, typename Address, typename LogIndex> class static_peer2peer_replicator {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using log_index_type = LogIndex;

    /// @brief One node's self-reported progress, as last advertised.
    struct progress_digest {
        Address address;
        std::uint64_t term;
        LogIndex last_log_index;
    };
    using table_type = folly::Synchronized<std::unordered_map<NodeId, progress_digest>>;

    /// @param shared_table Progress table shared across every node instance in
    ///        a simulated cluster.
    explicit static_peer2peer_replicator(std::shared_ptr<table_type> shared_table)
        : _table(std::move(shared_table)) {}

    auto advertise_progress(NodeId self_id, Address self_address, std::uint64_t term,
                            LogIndex last_log_index) -> kythira::Future<void> {
        _table->wlock()->insert_or_assign(self_id,
                                          progress_digest{self_address, term, last_log_index});
        return kythira::FutureFactory::makeFuture();
    }

    /// @brief Returns the first peer, currently in this instance's own
    /// membership set (Requirement 11 — a removed member is never offered
    /// even if its stale digest lingers in the shared table), whose gossiped
    /// `last_log_index >= from_index`, excluding this node's own entry.
    auto find_catch_up_source(LogIndex from_index, LogIndex /*to_index*/,
                              std::chrono::milliseconds) const
        -> kythira::Future<std::optional<peer_info<NodeId, Address>>> {
        auto members = _member_ids.rlock();
        auto locked = _table->rlock();
        for (const auto& [id, digest] : *locked) {
            if (!members->contains(id)) continue;
            if (digest.last_log_index >= from_index) {
                return kythira::FutureFactory::makeFuture(std::optional<peer_info<NodeId, Address>>{
                    peer_info<NodeId, Address>{id, digest.address}});
            }
        }
        return kythira::FutureFactory::makeFuture(std::optional<peer_info<NodeId, Address>>{});
    }

    auto update_membership(std::vector<NodeId> member_ids) -> kythira::Future<void> {
        *_member_ids.wlock() = std::unordered_set<NodeId>(member_ids.begin(), member_ids.end());
        return kythira::FutureFactory::makeFuture();
    }

private:
    std::shared_ptr<table_type> _table;
    // Not part of the shared table — this instance's own last-`update_membership`-
    // supplied set (Requirement 11.1), exactly as the real system calls
    // `update_membership` per node<Types> instance.
    folly::Synchronized<std::unordered_set<NodeId>> _member_ids;
};

static_assert(
    peer2peer_replicator<no_op_peer2peer_replicator<std::uint64_t, std::string, std::uint64_t>,
                         std::uint64_t, std::string, std::uint64_t>);
static_assert(
    peer2peer_replicator<static_peer2peer_replicator<std::uint64_t, std::string, std::uint64_t>,
                         std::uint64_t, std::string, std::uint64_t>);

}  // namespace kythira
