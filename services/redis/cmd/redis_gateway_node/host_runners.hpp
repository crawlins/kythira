// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file host_runners.hpp
/// @brief The daemon's entry point per wire serializer: run_cbor_node.cpp
///        and run_json.cpp.

#include "config.hpp"

namespace kythira::redis_node {

/// The CBOR wire, with every storage this build has.
auto run_cbor(const node_options& opt) -> int;
/// The JSON wire, values in RAM only. Kept for the serializer measurement
/// in design.md; not the default.
auto run_json(const node_options& opt) -> int;

}  // namespace kythira::redis_node
