#pragma once

// Main header file for the network simulator library
// Include this file to access all network simulator functionality

#include "concepts.hpp"
#include "types.hpp"
#include "exceptions.hpp"
#include "simulator.hpp"
#include "node.hpp"
#include "connection.hpp"
#include "listener.hpp"

namespace network_simulator {

// Version information
inline constexpr int version_major = 0;
inline constexpr int version_minor = 1;
inline constexpr int version_patch = 0;

// Convenience alias for the default network simulator
using DefaultNetworkSimulator = NetworkSimulator<DefaultNetworkTypes>;

} // namespace network_simulator
