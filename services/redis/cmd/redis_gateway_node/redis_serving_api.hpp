// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file redis_serving_api.hpp
/// @brief What the Redis module (redis_module.hpp) sees of its serving side
///        (.kiro/specs/service-colocation/ task 7): an interface, and a
///        factory per storage. The factories are defined in their own
///        translation units over redis_serving.hpp, one multi_raft stack
///        each, so nothing that includes this header compiles one.

#include "config.hpp"

#include <kythira/redis/redis_acl.hpp>
#include <kythira/redis/redis_gateway.hpp>

#include <raft/service_module.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace kythira::redis_node {

/// The largest single Raft command this node proposes: an HTTP chunk
/// (`put_chunk`) or a whole RESP value, whichever limit is higher.
inline auto largest_entry_bytes(const node_options& opt) -> std::size_t {
    return std::max<std::size_t>(opt._cache._chunk_bytes,
                                 static_cast<std::size_t>(opt._gateway._max_value_bytes));
}

/// The Raft listener's request-body limit. The transport default of 10 MB
/// is too small for a 16 MiB chunk, and for an 8 MiB RESP value once the
/// JSON serializer base64-encodes it. An AppendEntries carries at most
/// raft_configuration::_max_append_bytes of commands (set to the same
/// largest entry below) or one entry larger than that, so 4/3 of the
/// largest entry plus 1 MiB for framing and the small entries around it
/// always fits.
inline auto raft_request_body_limit(const node_options& opt) -> std::size_t {
    constexpr std::size_t framing = std::size_t{1024} * 1024;
    return std::max<std::size_t>(std::size_t{10} * 1024 * 1024,
                                 largest_entry_bytes(opt) / 3 * 4 + framing);
}

/// The module's serving side, whatever its storage: built by the module's
/// register_groups, run by its start, drain and stop.
class serving {
public:
    serving() = default;
    serving(const serving&) = delete;
    auto operator=(const serving&) -> serving& = delete;
    serving(serving&&) = delete;
    auto operator=(serving&&) -> serving& = delete;
    virtual ~serving() = default;

    /// Starts the host, the gateway, the cache's HTTP front end and the
    /// driver threads, and prints the ready line.
    virtual void start() = 0;
    /// Stops taking requests: the front end and the gateway.
    virtual void drain() = 0;
    /// Stops the driver threads and the host. Nothing refers to the host
    /// once it returns.
    virtual void stop() = 0;
    [[nodiscard]] virtual auto group_count() const -> std::size_t = 0;
    [[nodiscard]] virtual auto redis_port() const -> std::uint16_t = 0;
};

/// Builds a `serving` for one storage on `Host`. The module holds one per
/// storage its build has, each instantiated in its own translation unit.
template<typename Host>
using serving_factory = std::function<std::unique_ptr<serving>(
    const node_options&, kythira::redis_acl&, const kythira::redis_gateway_config&, Host&,
    const service_node::group_space&)>;

}  // namespace kythira::redis_node
