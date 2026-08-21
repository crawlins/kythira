// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "concepts.hpp"
#include "types.hpp"
#include "exceptions.hpp"

#include <raft/async_scope.hpp>

#include <memory>
#include <mutex>
#include <unordered_set>

namespace network_simulator {

// Forward declaration
template<typename Types> class NetworkSimulator;

// NetworkNode class template
template<typename Types> class NetworkNode {
public:
    // Type aliases from Types template argument
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    using message_type = typename Types::message_type;
    using connection_type = typename Types::connection_type;
    using listener_type = typename Types::listener_type;
    using simulator_type = NetworkSimulator<Types>;

    // Future type aliases
    using future_bool_type = typename Types::future_bool_type;
    using future_message_type = typename Types::future_message_type;
    using future_connection_type = typename Types::future_connection_type;
    using future_listener_type = typename Types::future_listener_type;

    /// @param scope The owning simulator's drain barrier. Every method that
    ///     dereferences `_simulator` takes a ticket from it first, so the
    ///     simulator's destructor can wait for in-flight calls instead of
    ///     pulling the object out from under them.
    ///
    ///     `_simulator` stays a raw pointer deliberately. A `shared_ptr` would
    ///     cycle -- `NetworkSimulator::_nodes` owns its nodes by `shared_ptr`
    ///     -- and a `weak_ptr` would need `enable_shared_from_this`, which is
    ///     unavailable here: roughly 130 call sites construct the simulator on
    ///     the stack, and `weak_from_this()` on those would be empty, silently
    ///     failing every node operation in the suite. The scope works for both
    ///     storage durations.
    explicit NetworkNode(address_type addr, simulator_type* simulator,
                         std::shared_ptr<kythira::async_scope> scope)
        : _address(std::move(addr)), _simulator(simulator), _scope(std::move(scope)) {}

    // Node identity
    auto address() const -> address_type { return _address; }

    // Connectionless operations
    auto send(message_type msg) -> future_bool_type;
    auto send(message_type msg, std::chrono::milliseconds timeout) -> future_bool_type;
    auto receive() -> future_message_type;
    auto receive(std::chrono::milliseconds timeout) -> future_message_type;
    // Port-filtered: only returns messages whose destination_port == port.
    auto receive(port_type port, std::chrono::milliseconds timeout) -> future_message_type;

    // Connection-oriented client operations
    auto connect(address_type dst_addr, port_type dst_port) -> future_connection_type;
    auto connect(address_type dst_addr, port_type dst_port, port_type src_port)
        -> future_connection_type;
    auto connect(address_type dst_addr, port_type dst_port, std::chrono::milliseconds timeout)
        -> future_connection_type;

    // Connection-oriented server operations
    auto bind() -> future_listener_type;                // bind to random port
    auto bind(port_type port) -> future_listener_type;  // bind to specific port
    auto bind(port_type port, std::chrono::milliseconds timeout) -> future_listener_type;

private:
    address_type _address;
    simulator_type* _simulator;
    /// Held by `shared_ptr` so it outlives the simulator: a node that is still
    /// running when the simulator dies needs the scope to tell it so.
    std::shared_ptr<kythira::async_scope> _scope;

    // Ephemeral port allocation
    auto allocate_ephemeral_port() -> port_type;
    auto release_port(port_type port) -> void;
    std::unordered_set<port_type> _used_ports;
    mutable std::mutex _port_mutex;
};

}  // namespace network_simulator

#include "node_impl.hpp"
