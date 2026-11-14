// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_json.cpp
/// @brief The JSON-wire daemon: its `multi_raft` stack, values in RAM,
///        alone in its translation unit. Kept for the serializer
///        measurement in design.md; not the default. from_env() refuses
///        file and engine persistence on this wire.

#include "redis_serving.hpp"
#include "serving_factories.hpp"

namespace kythira::redis_node {

auto run_json(const node_options& opt) -> int {
    return run_standalone<json_wire>(
        opt,
        {.memory = serving_factory_for<json_host, memory_storage>(), .file = {}, .engine = {}});
}

}  // namespace kythira::redis_node
