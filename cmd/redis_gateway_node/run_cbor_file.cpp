// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_cbor_file.cpp
/// @brief The CBOR-wire `multi_raft` stack over the chunk store and the
///        binary persistence engine (`KYTHIRA_CACHE_PERSISTENCE=file`, the
///        default), alone in its translation unit like the others.

#include "host_runners.hpp"
#include "run_host.hpp"

#include <raft/cbor_serializer.hpp>

namespace kythira::redis_node {

auto run_cbor_file(const node_options& opt) -> int {
    return run_host<kythira::cbor_rpc_serializer<std::vector<std::byte>>, file_storage>(opt);
}

}  // namespace kythira::redis_node
