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

} // namespace network_simulator
