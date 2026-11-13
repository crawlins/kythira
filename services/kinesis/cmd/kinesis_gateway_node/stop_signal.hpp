// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file stop_signal.hpp
/// @brief SIGINT/SIGTERM to the serve loop: the handler sets a flag and
///        wakes `wait_for_stop`, nothing else (async-signal-safe enough for
///        the notify the Redis daemon also relies on).

#include <raft/static_or_iam_authorizer.hpp>

#include <optional>

namespace kythira::kinesis_node {

void on_signal(int);
void wait_for_stop();

/// The daemon: build, serve until stopped, shut down. Defined in
/// run_host.cpp, alone in its translation unit. `iam` is IAM's side of
/// KINESIS_AUTH=iam or both, which must outlive the call.
struct node_options;
auto run(const node_options& opt, std::optional<kythira::aws_auth::iam_hooks> iam) -> int;

}  // namespace kythira::kinesis_node
