#pragma once

/// @file peer_discovery.hpp
/// @brief Peer-discovery concept and built-in implementations.

#include <raft/future.hpp>
#include <algorithm>
#include <chrono>
#include <concepts>
#include <stdexcept>
#include <vector>

namespace kythira {

/// @brief Associates a node identifier with its network address.
/// @tparam NodeId  Node identifier type.
/// @tparam Address Network address type (e.g., `std::string` as `"host:port"`).
template<typename NodeId, typename Address> struct peer_info {
    NodeId node_id;   ///< Cluster-unique identifier of the peer.
    Address address;  ///< Network address at which the peer is reachable.
};

/// @brief Concept for a peer-discovery back-end.
///
/// Implementations advertise this node's presence and return a snapshot of
/// currently visible peers.  All operations are asynchronous to accommodate
/// back-ends that require network I/O (DNS-SD, mDNS, static file, etc.).
///
/// @tparam P       Concrete peer-discovery type.
/// @tparam NodeId  Must match `P::node_id_type`.
/// @tparam Address Must match `P::address_type`.
template<typename P, typename NodeId, typename Address>
concept peer_discovery =
    requires(P& finder, std::chrono::milliseconds timeout, NodeId self_id, Address self_address) {
        typename P::node_id_type;
        typename P::address_type;
        requires std::same_as<typename P::node_id_type, NodeId>;
        requires std::same_as<typename P::address_type, Address>;
        /// Advertise this node so other nodes can discover it.
        { finder.register_node(self_id, self_address) } -> std::same_as<kythira::Future<void>>;
        /// Return a snapshot of currently reachable peers within `timeout`.
        {
            finder.find_peers(timeout)
        } -> std::same_as<kythira::Future<std::vector<peer_info<NodeId, Address>>>>;
    };

/// @brief No-op peer-discovery implementation for single-node and test scenarios.
///
/// `register_node` succeeds immediately.  `find_peers` always returns an empty vector,
/// which is equivalent to a cluster with no peers (single-node bootstrap).
///
/// @tparam NodeId  Node identifier type.
/// @tparam Address Network address type.
template<typename NodeId, typename Address> class no_op_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;

    /// @brief No-op registration; always succeeds.
    auto register_node(NodeId, Address) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }

    /// @brief Returns an empty peer list immediately.
    auto find_peers(std::chrono::milliseconds) const
        -> kythira::Future<std::vector<peer_info<NodeId, Address>>> {
        return kythira::FutureFactory::makeFuture(std::vector<peer_info<NodeId, Address>>{});
    }
};

/// @brief Peer-discovery implementation backed by a fixed compile-time peer list.
///
/// `register_node` validates that `self_id` is present in the fixed list —
/// it throws `std::invalid_argument` otherwise.  `find_peers` returns the
/// full fixed list on every call regardless of timeout.
///
/// @tparam NodeId  Node identifier type.
/// @tparam Address Network address type.
template<typename NodeId, typename Address> class static_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;

    /// @brief Constructs the discovery object with a fixed peer set.
    /// @param peers All known peers including this node itself.
    explicit static_peer_discovery(std::vector<peer_info<NodeId, Address>> peers)
        : _peers(std::move(peers)) {}

    /// @brief Validates that `self_id` is in the fixed peer list.
    /// @throws std::invalid_argument if `self_id` is not found.
    auto register_node(NodeId self_id, Address) -> kythira::Future<void> {
        auto it = std::ranges::find(_peers, self_id, &peer_info<NodeId, Address>::node_id);
        if (it == _peers.end()) {
            throw std::invalid_argument(
                "static_peer_discovery: self_id not found in fixed peers list");
        }
        return kythira::FutureFactory::makeFuture();
    }

    /// @brief Returns a copy of the full fixed peer list.
    auto find_peers(std::chrono::milliseconds) const
        -> kythira::Future<std::vector<peer_info<NodeId, Address>>> {
        return kythira::FutureFactory::makeFuture(std::vector<peer_info<NodeId, Address>>(_peers));
    }

private:
    std::vector<peer_info<NodeId, Address>> _peers;
};

static_assert(
    peer_discovery<no_op_peer_discovery<std::uint64_t, std::string>, std::uint64_t, std::string>);
static_assert(
    peer_discovery<static_peer_discovery<std::uint64_t, std::string>, std::uint64_t, std::string>);

}  // namespace kythira
