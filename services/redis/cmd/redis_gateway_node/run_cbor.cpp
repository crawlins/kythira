// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_cbor.cpp
/// @brief The CBOR-wire `multi_raft` stack with values in RAM, alone in its
///        translation unit (`KYTHIRA_CACHE_PERSISTENCE=memory`).

#include "redis_serving.hpp"
#include "serving_factories.hpp"

namespace kythira::redis_node {

auto cbor_memory_serving() -> serving_factory<cbor_host> {
    return serving_factory_for<cbor_host, memory_storage>();
}

}  // namespace kythira::redis_node
