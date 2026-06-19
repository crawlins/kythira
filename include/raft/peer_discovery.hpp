#pragma once

#include <raft/future.hpp>
#include <algorithm>
#include <chrono>
#include <concepts>
#include <stdexcept>
#include <vector>

namespace kythira {

template<typename NodeId, typename Address> struct peer_info {
    NodeId node_id;
    Address address;
};

template<typename P, typename NodeId, typename Address>
concept peer_discovery =
    requires(P& finder, std::chrono::milliseconds timeout, NodeId self_id, Address self_address) {
        typename P::node_id_type;
        typename P::address_type;
        requires std::same_as<typename P::node_id_type, NodeId>;
        requires std::same_as<typename P::address_type, Address>;
        { finder.register_node(self_id, self_address) } -> std::same_as<kythira::Future<void>>;
        {
            finder.find_peers(timeout)
        } -> std::same_as<kythira::Future<std::vector<peer_info<NodeId, Address>>>>;
    };

template<typename NodeId, typename Address> class no_op_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;

    auto register_node(NodeId, Address) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }

    auto find_peers(std::chrono::milliseconds) const
        -> kythira::Future<std::vector<peer_info<NodeId, Address>>> {
        return kythira::FutureFactory::makeFuture(std::vector<peer_info<NodeId, Address>>{});
    }
};

template<typename NodeId, typename Address> class static_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;

    explicit static_peer_discovery(std::vector<peer_info<NodeId, Address>> peers)
        : _peers(std::move(peers)) {}

    auto register_node(NodeId self_id, Address) -> kythira::Future<void> {
        auto it = std::ranges::find(_peers, self_id, &peer_info<NodeId, Address>::node_id);
        if (it == _peers.end()) {
            throw std::invalid_argument(
                "static_peer_discovery: self_id not found in fixed peers list");
        }
        return kythira::FutureFactory::makeFuture();
    }

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
