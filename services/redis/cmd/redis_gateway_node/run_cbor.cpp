// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_cbor.cpp
/// @brief The CBOR-wire `multi_raft` stack with values in RAM, alone in its
///        translation unit (`KYTHIRA_CACHE_PERSISTENCE=memory`).

#include "host_runners.hpp"
#include "run_host.hpp"

#include <raft/cbor_serializer.hpp>

namespace kythira::redis_node {

auto run_cbor(const node_options& opt) -> int {
    return run_host<kythira::cbor_rpc_serializer<std::vector<std::byte>>, memory_storage>(opt);
}

}  // namespace kythira::redis_node
