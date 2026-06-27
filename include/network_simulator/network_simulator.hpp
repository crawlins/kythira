#pragma once

/// @file network_simulator.hpp
/// @brief Main entry point for the network simulator library.
///
/// Include this header to access all network-simulator types and utilities.
/// The simulator provides an in-process, deterministic network for testing
/// distributed protocols such as Raft without real sockets.

#include "concepts.hpp"
#include "types.hpp"
#include "exceptions.hpp"
#include "simulator.hpp"
#include "node.hpp"
#include "connection.hpp"
#include "listener.hpp"

namespace network_simulator {

/// @brief Library major version number.
inline constexpr int version_major = 0;
/// @brief Library minor version number.
inline constexpr int version_minor = 1;
/// @brief Library patch version number.
inline constexpr int version_patch = 0;

/// @brief Convenience alias for `NetworkSimulator` using `DefaultNetworkTypes`.
using DefaultNetworkSimulator = NetworkSimulator<DefaultNetworkTypes>;

}  // namespace network_simulator
