// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file serving_factories.hpp
/// @brief The daemon's serving stacks, one per wire and storage, each
///        defined in its own translation unit (redis_serving.hpp's rule).

#include "config.hpp"
#include "host_runners.hpp"
#include "redis_serving_api.hpp"
#include "standalone_node.hpp"

#include <raft/cbor_serializer.hpp>
#include <raft/json_serializer.hpp>

#include <cstddef>
#include <vector>

namespace kythira::redis_node {

using cbor_wire = kythira::cbor_rpc_serializer<std::vector<std::byte>>;
using json_wire = kythira::json_rpc_serializer<std::vector<std::byte>>;
using cbor_host = standalone_host<cbor_wire>;
using json_host = standalone_host<json_wire>;

/// run_cbor.cpp: values in RAM (`KYTHIRA_CACHE_PERSISTENCE=memory`).
auto cbor_memory_serving() -> serving_factory<cbor_host>;
/// run_cbor_file.cpp: values in the chunk store (`file`).
auto cbor_file_serving() -> serving_factory<cbor_host>;
#if defined(KYTHIRA_REDIS_GATEWAY_ENGINE)
/// run_cbor_engine.cpp: values in the node's RocksDB database (`engine`).
auto cbor_engine_serving() -> serving_factory<cbor_host>;
#endif

}  // namespace kythira::redis_node
