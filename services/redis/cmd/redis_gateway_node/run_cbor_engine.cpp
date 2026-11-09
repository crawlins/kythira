// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_cbor_engine.cpp
/// @brief The CBOR-wire `multi_raft` stack over the node's RocksDB engine
///        (`KYTHIRA_CACHE_PERSISTENCE=engine`), alone in its translation unit
///        like the others. Built only with `CONFIG_REDIS_GATEWAY_ENGINE=y`.

#include "host_runners.hpp"
#include "run_host.hpp"

#include <raft/cbor_serializer.hpp>

namespace kythira::redis_node {

auto run_cbor_engine(const node_options& opt) -> int {
    return run_host<kythira::cbor_rpc_serializer<std::vector<std::byte>>, engine_storage>(opt);
}

}  // namespace kythira::redis_node
