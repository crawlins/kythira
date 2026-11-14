// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file redis_module_memory.cpp
/// @brief The Redis module's `multi_raft` stack in kythira_node with
///        values in RAM (`KYTHIRA_CACHE_PERSISTENCE=memory`), alone in its translation unit.

#include "node_serving_factories.hpp"
#include "redis_serving.hpp"

namespace kythira::redis_node {

auto node_memory_serving() -> serving_factory<service_node::kythira_node_host> {
    return serving_factory_for<service_node::kythira_node_host, memory_storage>();
}

}  // namespace kythira::redis_node
