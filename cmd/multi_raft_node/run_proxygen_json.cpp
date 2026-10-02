// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_proxygen_json.cpp
/// @brief The single `multi_raft` instantiation for `--transport proxygen
///        --serializer json`, alone in its own translation unit.
///
/// One of six, and only compiled when `KYTHIRA_BUILD_PROXYGEN_TRANSPORT` is
/// on. Splitting them is what keeps any one of these under the
/// compiler-memory ceiling a 16 GiB CI runner sets; `host_stacks.hpp`
/// carries the measurement and the CI failure that motivated it.

#include "host_stacks.hpp"
#include "host_runners.hpp"

#include <cstddef>
#include <vector>

#if defined(KYTHIRA_BENCH_HAS_PROXYGEN)

namespace kythira::bench::host {

auto run_proxygen_json(const node_options& opt) -> int {
    return run_host<proxygen_stack<kythira::json_rpc_serializer<std::vector<std::byte>>>>(opt);
}

}  // namespace kythira::bench::host

#endif
