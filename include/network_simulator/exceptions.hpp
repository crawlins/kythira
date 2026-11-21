// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdexcept>
#include <string>

namespace network_simulator {

class NetworkException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class TimeoutException : public NetworkException {
public:
    TimeoutException() : NetworkException("Operation timed out") {}
};

class ConnectionClosedException : public NetworkException {
public:
    ConnectionClosedException() : NetworkException("Connection is closed") {}
};

/// A connection establishment was cancelled while it was still in progress,
/// through `NetworkSimulator::cancel_pending_connections` and friends.
class ConnectionCancelledException : public NetworkException {
public:
    ConnectionCancelledException() : NetworkException("Connection establishment cancelled") {}
};

/// Thrown by connect when the destination is reachable but nothing will take
/// the connection: no listener on that port, or a listener that has stopped
/// accepting. The message always starts "Connection refused", which is what
/// Raft's error classifier (`raft/error_handler.hpp`) keys on.
class ConnectionRefusedException : public NetworkException {
public:
    explicit ConnectionRefusedException(const std::string& reason)
        : NetworkException("Connection refused: " + reason) {}
};

/// The simulator cannot serve the call: it has not been started, it stopped
/// while the call was in progress, or the node outlived it.
class SimulatorUnavailableException : public NetworkException {
public:
    explicit SimulatorUnavailableException(const std::string& reason) : NetworkException(reason) {}
};

class PortInUseException : public NetworkException {
public:
    explicit PortInUseException(const std::string& port)
        : NetworkException("Port already in use: " + port) {}
};

class NodeNotFoundException : public NetworkException {
public:
    explicit NodeNotFoundException(const std::string& address)
        : NetworkException("Node not found: " + address) {}
};

class NoRouteException : public NetworkException {
public:
    NoRouteException(const std::string& from, const std::string& to)
        : NetworkException("No route from " + from + " to " + to) {}
};

}  // namespace network_simulator
