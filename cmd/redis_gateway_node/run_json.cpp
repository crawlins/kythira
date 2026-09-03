// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_json.cpp
/// @brief The JSON-wire `multi_raft` stack, alone in its translation unit.
///        Kept for the serializer measurement in design.md; not the default.

#include "host_runners.hpp"
#include "run_host.hpp"

namespace kythira::redis_node {

auto run_json(const node_options& opt) -> int {
    return run_host<kythira::json_rpc_serializer<std::vector<std::byte>>>(opt);
}

}  // namespace kythira::redis_node
