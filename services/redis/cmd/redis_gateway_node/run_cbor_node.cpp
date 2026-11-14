// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_cbor_node.cpp
/// @brief The CBOR-wire daemon: its transport and node, with the stacks
///        run_cbor*.cpp define. No `multi_raft` stack of its own.

#include "serving_factories.hpp"

namespace kythira::redis_node {

auto run_cbor(const node_options& opt) -> int {
    return run_standalone<cbor_wire>(opt, {
                                              .memory = cbor_memory_serving(),
                                              .file = cbor_file_serving(),
#if defined(KYTHIRA_REDIS_GATEWAY_ENGINE)
                                              .engine = cbor_engine_serving(),
#else
                                              .engine = {},
#endif
                                          });
}

}  // namespace kythira::redis_node
