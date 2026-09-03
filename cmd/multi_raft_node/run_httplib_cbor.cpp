// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_httplib_cbor.cpp
/// @brief The single `multi_raft` instantiation for `--transport httplib
///        --serializer cbor`, alone in its own translation unit.
///
/// One of four. Splitting them is what keeps any one of these under the
/// compiler-memory ceiling a 16 GiB CI runner sets; `host_stacks.hpp`
/// carries the measurement and the CI failure that motivated it.

#include "host_stacks.hpp"
#include "host_runners.hpp"

#include <cstddef>
#include <vector>

namespace kythira::bench::host {

auto run_httplib_cbor(const node_options& opt) -> int {
    return run_host<httplib_stack<kythira::cbor_rpc_serializer<std::vector<std::byte>>>>(opt);
}

}  // namespace kythira::bench::host
