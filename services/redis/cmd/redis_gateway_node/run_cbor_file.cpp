// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_cbor_file.cpp
/// @brief The CBOR-wire `multi_raft` stack over the disk-backed cache's
///        chunk store, alone in its translation unit
///        (`KYTHIRA_CACHE_PERSISTENCE=file`).

#include "redis_serving.hpp"
#include "serving_factories.hpp"

namespace kythira::redis_node {

auto cbor_file_serving() -> serving_factory<cbor_host> {
    return serving_factory_for<cbor_host, file_storage>();
}

}  // namespace kythira::redis_node
