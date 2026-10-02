// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file host_runners.hpp
/// @brief One entry point per `(transport × wire serializer)` pair, each
///        defined in its own translation unit.
///
/// `main.cpp` includes this and nothing from `host_stacks.hpp`, which is the
/// point: the four `multi_raft` instantiations that made the host's single
/// translation unit peak at 10,974 MiB of compiler RSS are now four objects that
/// compile in parallel (six with Proxygen), and `main.cpp` itself is cheap
/// again. See `host_stacks.hpp` for the measurement and the failure it caused.
///
/// The set follows the build: httplib always, Beast under
/// `KYTHIRA_BENCH_HAS_BEAST`, and Proxygen under `KYTHIRA_BENCH_HAS_PROXYGEN`
/// (set when `KYTHIRA_BUILD_PROXYGEN_TRANSPORT` is on), the same macros the
/// Tier B benchmark compiles its rows under. A transport that was not compiled
/// in is refused by `main` with exit status 2 rather than silently substituted.

#include "config.hpp"

namespace kythira::bench::host {

/// @return the process exit status; 0 on a clean shutdown.
auto run_httplib_json(const node_options& opt) -> int;
auto run_httplib_cbor(const node_options& opt) -> int;

#if defined(KYTHIRA_BENCH_HAS_BEAST)
auto run_beast_json(const node_options& opt) -> int;
auto run_beast_cbor(const node_options& opt) -> int;
#endif

#if defined(KYTHIRA_BENCH_HAS_PROXYGEN)
auto run_proxygen_json(const node_options& opt) -> int;
auto run_proxygen_cbor(const node_options& opt) -> int;
#endif

}  // namespace kythira::bench::host
