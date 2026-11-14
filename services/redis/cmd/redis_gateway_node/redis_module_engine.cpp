// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file redis_module_engine.cpp
/// @brief The Redis module's `multi_raft` stack in kythira_node with
///        values in the node's RocksDB database (`engine`; `CONFIG_REDIS_GATEWAY_ENGINE=y` only),
///        alone in its translation unit.

#include "node_serving_factories.hpp"
#include "redis_serving.hpp"

namespace kythira::redis_node {

auto node_engine_serving() -> serving_factory<service_node::kythira_node_host> {
    return serving_factory_for<service_node::kythira_node_host, engine_storage>();
}

}  // namespace kythira::redis_node
