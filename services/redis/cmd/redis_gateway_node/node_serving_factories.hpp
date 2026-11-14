// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file node_serving_factories.hpp
/// @brief The Redis module's serving stacks in kythira_node
///        (.kiro/specs/service-colocation/ task 7), on the node's shared
///        transport, one per storage, each defined in its own translation
///        unit (redis_serving.hpp's rule).

#include "redis_serving_api.hpp"

#include <raft/kythira_node_host.hpp>

namespace kythira::redis_node {

/// redis_module_memory.cpp
auto node_memory_serving() -> serving_factory<service_node::kythira_node_host>;
/// redis_module_file.cpp
auto node_file_serving() -> serving_factory<service_node::kythira_node_host>;
#if defined(KYTHIRA_REDIS_GATEWAY_ENGINE)
/// redis_module_engine.cpp
auto node_engine_serving() -> serving_factory<service_node::kythira_node_host>;
#endif

}  // namespace kythira::redis_node
