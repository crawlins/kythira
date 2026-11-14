// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_cbor_engine.cpp
/// @brief The CBOR-wire `multi_raft` stack over the node's RocksDB engine
///        (`KYTHIRA_CACHE_PERSISTENCE=engine`), alone in its translation unit
///        like the others. Built only with `CONFIG_REDIS_GATEWAY_ENGINE=y`.

#include "redis_serving.hpp"
#include "serving_factories.hpp"

namespace kythira::redis_node {

auto cbor_engine_serving() -> serving_factory<cbor_host> {
    return serving_factory_for<cbor_host, engine_storage>();
}

}  // namespace kythira::redis_node
