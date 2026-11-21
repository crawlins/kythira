// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "node.hpp"
#include "simulator.hpp"
#include "exceptions.hpp"
#include <raft/future_default.hpp>

#include <atomic>
#include <chrono>
#include <random>
#include <sstream>
#include <thread>

#include <optional>

namespace network_simulator {

// Ephemeral port allocation implementation
template<typename Types> auto NetworkNode<Types>::allocate_ephemeral_port() -> port_type {
    std::lock_guard<std::mutex> lock(_port_mutex);

    if constexpr (std::is_same_v<port_type, unsigned short>) {
        // For unsigned short ports, use standard ephemeral port range
        constexpr unsigned short ephemeral_start = 49152;
        constexpr unsigned short ephemeral_end = 65535;

        // Find next available port
        for (unsigned short port = ephemeral_start; port <= ephemeral_end; ++port) {
            if (_used_ports.find(port) == _used_ports.end()) {
                _used_ports.insert(port);
                return port;
            }
        }

        // If no ports available, throw exception
        throw PortInUseException("No ephemeral ports available");

    } else if constexpr (std::is_same_v<port_type, std::string>) {
        // For string ports, generate unique string identifiers
        static std::atomic<std::size_t> counter{0};
        std::string port;

        do {
            std::ostringstream oss;
            oss << "ephemeral_" << counter++;
            port = oss.str();
        } while (_used_ports.find(port) != _used_ports.end());

        _used_ports.insert(port);
        return port;
    } else {
        static_assert(
            std::is_same_v<port_type, unsigned short> || std::is_same_v<port_type, std::string>,
            "Port type must be unsigned short or std::string");
    }
}

// Port release implementation
template<typename Types> auto NetworkNode<Types>::release_port(port_type port) -> void {
    std::lock_guard<std::mutex> lock(_port_mutex);
    _used_ports.erase(port);
}

// Connectionless send operations
template<typename Types> auto NetworkNode<Types>::send(message_type msg) -> future_bool_type {
    // A ticket before the pointer, not merely a null check. Nothing ever
    // nulls `_simulator`, so the bare check only caught a node built
    // without a simulator -- never a simulator that had since been
    // destroyed, which is the case that actually crashed. While the
    // ticket is held the simulator's drain cannot finish, so the
    // pointer stays valid for the call below.
    const auto sim_ticket = _scope ? _scope->enter() : std::nullopt;
    if (!_simulator || !sim_ticket) {
        return kythira::future_factory_default::makeExceptionalFuture<bool>(
            std::make_exception_ptr(SimulatorUnavailableException("Simulator not available")));
    }

    return _simulator->route_message(std::move(msg));
}

template<typename Types>
auto NetworkNode<Types>::send(message_type msg, std::chrono::milliseconds timeout)
    -> future_bool_type {
    // For now, implement timeout by delegating to the basic send
    // In a full implementation, this would use a timer
    auto future = send(std::move(msg));

    // Check if timeout occurred (simplified implementation)
    if (!future.wait(timeout)) {
        return kythira::future_factory_default::makeFuture(false);
    }

    return future;
}

// Connectionless receive operations
template<typename Types> auto NetworkNode<Types>::receive() -> future_message_type {
    // A ticket before the pointer, not merely a null check. Nothing ever
    // nulls `_simulator`, so the bare check only caught a node built
    // without a simulator -- never a simulator that had since been
    // destroyed, which is the case that actually crashed. While the
    // ticket is held the simulator's drain cannot finish, so the
    // pointer stays valid for the call below.
    const auto sim_ticket = _scope ? _scope->enter() : std::nullopt;
    if (!_simulator || !sim_ticket) {
        return kythira::future_factory_default::makeExceptionalFuture<message_type>(
            std::make_exception_ptr(SimulatorUnavailableException("Simulator not available")));
    }

    return _simulator->retrieve_message(_address);
}

template<typename Types>
auto NetworkNode<Types>::receive(std::chrono::milliseconds timeout) -> future_message_type {
    // A ticket before the pointer, not merely a null check. Nothing ever
    // nulls `_simulator`, so the bare check only caught a node built
    // without a simulator -- never a simulator that had since been
    // destroyed, which is the case that actually crashed. While the
    // ticket is held the simulator's drain cannot finish, so the
    // pointer stays valid for the call below.
    const auto sim_ticket = _scope ? _scope->enter() : std::nullopt;
    if (!_simulator || !sim_ticket) {
        return kythira::future_factory_default::makeExceptionalFuture<message_type>(
            std::make_exception_ptr(TimeoutException()));
    }

    return _simulator->retrieve_message(_address, timeout);
}

template<typename Types>
auto NetworkNode<Types>::receive(port_type port, std::chrono::milliseconds timeout)
    -> future_message_type {
    // A ticket before the pointer, not merely a null check. Nothing ever
    // nulls `_simulator`, so the bare check only caught a node built
    // without a simulator -- never a simulator that had since been
    // destroyed, which is the case that actually crashed. While the
    // ticket is held the simulator's drain cannot finish, so the
    // pointer stays valid for the call below.
    const auto sim_ticket = _scope ? _scope->enter() : std::nullopt;
    if (!_simulator || !sim_ticket) {
        return kythira::future_factory_default::makeExceptionalFuture<message_type>(
            std::make_exception_ptr(TimeoutException()));
    }

    return _simulator->retrieve_message(_address, port, timeout);
}

// Connection-oriented client operations
template<typename Types>
auto NetworkNode<Types>::connect(address_type dst_addr, port_type dst_port)
    -> future_connection_type {
    // Use ephemeral port allocation
    auto src_port = allocate_ephemeral_port();
    return connect(std::move(dst_addr), std::move(dst_port), std::move(src_port));
}

template<typename Types>
auto NetworkNode<Types>::connect(address_type dst_addr, port_type dst_port, port_type src_port)
    -> future_connection_type {
    // A ticket before the pointer, not merely a null check. Nothing ever
    // nulls `_simulator`, so the bare check only caught a node built
    // without a simulator -- never a simulator that had since been
    // destroyed, which is the case that actually crashed. While the
    // ticket is held the simulator's drain cannot finish, so the
    // pointer stays valid for the call below.
    const auto sim_ticket = _scope ? _scope->enter() : std::nullopt;
    if (!_simulator || !sim_ticket) {
        return kythira::future_factory_default::makeExceptionalFuture<
            std::shared_ptr<connection_type>>(
            std::make_exception_ptr(SimulatorUnavailableException("Simulator not available")));
    }

    // Mark source port as used (thread-safe)
    {
        std::lock_guard<std::mutex> lock(_port_mutex);
        _used_ports.insert(src_port);
    }

    return _simulator->establish_connection(_address, std::move(src_port), std::move(dst_addr),
                                            std::move(dst_port));
}

template<typename Types>
auto NetworkNode<Types>::connect(address_type dst_addr, port_type dst_port,
                                 std::chrono::milliseconds timeout) -> future_connection_type {
    // A ticket before the pointer, not merely a null check. Nothing ever
    // nulls `_simulator`, so the bare check only caught a node built
    // without a simulator -- never a simulator that had since been
    // destroyed, which is the case that actually crashed. While the
    // ticket is held the simulator's drain cannot finish, so the
    // pointer stays valid for the call below.
    const auto sim_ticket = _scope ? _scope->enter() : std::nullopt;
    if (!_simulator || !sim_ticket) {
        return kythira::future_factory_default::makeExceptionalFuture<
            std::shared_ptr<connection_type>>(
            std::make_exception_ptr(SimulatorUnavailableException("Simulator not available")));
    }

    // Use ephemeral port allocation
    auto src_port = allocate_ephemeral_port();

    // Use the new establish_connection_with_timeout method
    return _simulator->establish_connection_with_timeout(
        _address, std::move(src_port), std::move(dst_addr), std::move(dst_port), timeout);
}

// Connection-oriented server operations
template<typename Types> auto NetworkNode<Types>::bind() -> future_listener_type {
    // A ticket before the pointer, not merely a null check. Nothing ever
    // nulls `_simulator`, so the bare check only caught a node built
    // without a simulator -- never a simulator that had since been
    // destroyed, which is the case that actually crashed. While the
    // ticket is held the simulator's drain cannot finish, so the
    // pointer stays valid for the call below.
    const auto sim_ticket = _scope ? _scope->enter() : std::nullopt;
    if (!_simulator || !sim_ticket) {
        return kythira::future_factory_default::makeExceptionalFuture<
            std::shared_ptr<listener_type>>(
            std::make_exception_ptr(SimulatorUnavailableException("Simulator not available")));
    }

    return _simulator->create_listener(_address);
}

template<typename Types> auto NetworkNode<Types>::bind(port_type port) -> future_listener_type {
    // A ticket before the pointer, not merely a null check. Nothing ever
    // nulls `_simulator`, so the bare check only caught a node built
    // without a simulator -- never a simulator that had since been
    // destroyed, which is the case that actually crashed. While the
    // ticket is held the simulator's drain cannot finish, so the
    // pointer stays valid for the call below.
    const auto sim_ticket = _scope ? _scope->enter() : std::nullopt;
    if (!_simulator || !sim_ticket) {
        return kythira::future_factory_default::makeExceptionalFuture<
            std::shared_ptr<listener_type>>(
            std::make_exception_ptr(SimulatorUnavailableException("Simulator not available")));
    }

    // Mark port as used (thread-safe)
    {
        std::lock_guard<std::mutex> lock(_port_mutex);
        _used_ports.insert(port);
    }

    return _simulator->create_listener(_address, std::move(port));
}

template<typename Types>
auto NetworkNode<Types>::bind(port_type port, std::chrono::milliseconds timeout)
    -> future_listener_type {
    // A ticket before the pointer, not merely a null check. Nothing ever
    // nulls `_simulator`, so the bare check only caught a node built
    // without a simulator -- never a simulator that had since been
    // destroyed, which is the case that actually crashed. While the
    // ticket is held the simulator's drain cannot finish, so the
    // pointer stays valid for the call below.
    const auto sim_ticket = _scope ? _scope->enter() : std::nullopt;
    if (!_simulator || !sim_ticket) {
        return kythira::future_factory_default::makeExceptionalFuture<
            std::shared_ptr<listener_type>>(
            std::make_exception_ptr(SimulatorUnavailableException("Simulator not available")));
    }

    // Mark port as used (thread-safe). Remember whether this call is what
    // marked it: a failed bind used to erase the port unconditionally, so a
    // second bind that lost to this node's own listener left that listener's
    // port looking free to allocate_ephemeral_port().
    bool marked_here = false;
    {
        std::lock_guard<std::mutex> lock(_port_mutex);
        marked_here = _used_ports.insert(port).second;
    }
    auto unmark = [&] {
        if (marked_here) {
            std::lock_guard<std::mutex> lock(_port_mutex);
            _used_ports.erase(port);
        }
    };

    // Req 4.5 and Property 16: a bind with a timeout waits for a port that is
    // in use to come free, and fails with TimeoutException only once the
    // deadline passes. It used to give up at once, converting the conflict
    // to a timeout without waiting at all. The untimed bind(port) still fails
    // straight away with PortInUseException (Req 13.3). The ticket held above
    // keeps the simulator alive while we wait; stop() ends the wait, because
    // create_listener then fails with SimulatorUnavailableException.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    constexpr std::chrono::milliseconds retry_interval{1};
    for (;;) {
        try {
            auto result = _simulator->create_listener(_address, port).get();
            if (!result) {
                unmark();
                return kythira::future_factory_default::makeExceptionalFuture<
                    std::shared_ptr<listener_type>>(std::make_exception_ptr(TimeoutException()));
            }
            return kythira::future_factory_default::makeFuture(std::move(result));
        } catch (const PortInUseException&) {
            if (std::chrono::steady_clock::now() >= deadline) {
                unmark();
                return kythira::future_factory_default::makeExceptionalFuture<
                    std::shared_ptr<listener_type>>(std::make_exception_ptr(TimeoutException()));
            }
        } catch (...) {
            // Req 13.6: through the future, not thrown from the call.
            unmark();
            return kythira::future_factory_default::makeExceptionalFuture<
                std::shared_ptr<listener_type>>(std::current_exception());
        }
        std::this_thread::sleep_for(retry_interval);
    }
}

}  // namespace network_simulator