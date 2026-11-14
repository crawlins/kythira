// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file node_host.hpp
/// @brief What a node gives each module to run its Raft groups on
///        (.kiro/specs/service-colocation/ task 3, Requirements 2.2 and 4).
///
/// A module builds its own `multi_raft` host, with its own state machine,
/// storage and transactions, from the node's shared pieces: the node id,
/// the one Raft client (whose connections every module shares), and its
/// range of the one Raft listener (`group_range_mux.hpp`), which carries
/// only the group ids of the module's `group_space`. Each host therefore
/// only ever sees its own groups and keys: splits, merges, the placement
/// driver and key lookups never meet another module's (Requirement 4.2,
/// 4.3).
///
/// `Client` and `Server` are the node's transport; a module's host types
/// name `transport_client_handle<Client, NodeId>` and `view_type`.

#include <raft/group_range_mux.hpp>
#include <raft/node_core.hpp>
#include <raft/service_module.hpp>

#include <filesystem>
#include <utility>

namespace kythira::service_node {

template<typename Client, typename Server, typename NodeId> class node_host {
public:
    using client_type = Client;
    using server_type = Server;
    using node_id_type = NodeId;
    using mux_type = group_range_mux<Server, NodeId>;
    using view_type = typename mux_type::view_type;

    /// @param client, server The node's transport, which outlive the host.
    ///        The server must not be started: the first module host to
    ///        start starts it.
    node_host(node_config config, NodeId id, Client& client, Server& server)
        : _config(std::move(config)), _id(id), _client(&client), _mux(server, id) {}

    node_host(const node_host&) = delete;
    auto operator=(const node_host&) -> node_host& = delete;
    node_host(node_host&&) = delete;
    auto operator=(node_host&&) -> node_host& = delete;
    ~node_host() = default;

    [[nodiscard]] auto config() const noexcept -> const node_config& { return _config; }
    [[nodiscard]] auto node_id() const noexcept -> const NodeId& { return _id; }

    /// The node's one Raft client.
    [[nodiscard]] auto client() noexcept -> Client& { return *_client; }

    /// The Raft listener for `space`'s group ids, as a module's host's
    /// `network_server`.
    /// @throws std::invalid_argument when another module already holds an
    ///         overlapping space.
    [[nodiscard]] auto server_for(const group_space& space) -> view_type {
        return _mux.view(space.first(), space.last());
    }

    /// Where module `m` keeps its files.
    template<typename Host>
    [[nodiscard]] auto data_dir(const service_module<Host>& m) const -> std::filesystem::path {
        return _config.data_dir / m.data_subdir();
    }

    [[nodiscard]] auto mux() noexcept -> mux_type& { return _mux; }

private:
    node_config _config;
    NodeId _id;
    Client* _client;
    mux_type _mux;
};

}  // namespace kythira::service_node
