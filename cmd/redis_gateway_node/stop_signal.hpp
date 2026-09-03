// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file stop_signal.hpp
/// @brief The process-wide stop latch shared between `main.cpp`'s signal
///        handler and the `run_host<Serializer>` instantiations, which live
///        in other translation units (cmd/multi_raft_node/stop_signal.hpp's
///        arrangement, for the same reason).

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace kythira::redis_node {

extern std::atomic<bool> g_stop;
extern std::mutex g_stop_mu;
extern std::condition_variable g_stop_cv;

void on_signal(int);
void wait_for_stop();

}  // namespace kythira::redis_node
