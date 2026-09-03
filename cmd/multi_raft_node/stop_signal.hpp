// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file stop_signal.hpp
/// @brief The process-wide stop latch, shared between `main.cpp`'s signal
///        handler and whichever `run_host<Stack>` instantiation is running.
///
/// It has a header of its own because the two halves now live in different
/// translation units: the handler is installed in `main.cpp`, and the wait is
/// inside `run_host()` in `host_stacks.hpp`, which four separate `.cpp` files
/// instantiate. Before the split both were in one file and the latch could be
/// a file-static; it cannot be now, and duplicating it per translation unit
/// would give each stack its own latch, so a SIGTERM would set one of them and
/// the running host would wait on another — a shutdown that hangs, which is
/// exactly the failure `run_host`'s ordering comment exists to prevent.

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace kythira::bench::host {

/// Set by the signal handler; `std::atomic<bool>` because a handler may write
/// nothing else. Defined in `main.cpp`.
extern std::atomic<bool> g_stop;
extern std::mutex g_stop_mu;
extern std::condition_variable g_stop_cv;

/// Async-signal-safe: sets the flag and notifies. Installed for SIGINT and
/// SIGTERM by `main`.
void on_signal(int);

/// Blocks until `on_signal` has run. The whole of the host's serving lifetime
/// is this call.
void wait_for_stop();

}  // namespace kythira::bench::host
