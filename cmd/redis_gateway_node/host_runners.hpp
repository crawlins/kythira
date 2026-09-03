// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file host_runners.hpp
/// @brief One entry point per wire serializer, each defined in its own
///        translation unit.

#include "config.hpp"

namespace kythira::redis_node {

auto run_cbor(const node_options& opt) -> int;
auto run_json(const node_options& opt) -> int;

}  // namespace kythira::redis_node
