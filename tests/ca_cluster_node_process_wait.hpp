// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstring>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace raft::testing {

// Bounded alternative to a plain blocking waitpid(pid, &status, 0). A
// ca_cluster_node subprocess is expected to exit promptly once SIGTERM's
// shutdown path completes -- but that path's own correctness is exactly what
// this project's own "ca_cluster_node_test intermittent hang" follow-up
// spent real effort chasing (a genuine bug: a shutdown-thread join ran
// before the one call that force-rejects an in-flight commit future,
// deadlocking indefinitely; fixed in cmd/ca_cluster_node/main.cpp). Once
// fixed, a test harness that still blocks forever on waitpid() offers no
// protection against that class of bug ever recurring: a future regression
// in the same shutdown path would silently turn back into an indefinite
// ctest hang instead of a diagnosable test failure. Polling with a timeout,
// escalating to SIGKILL if it's exceeded, converts that failure mode into an
// observable one (BOOST_ERROR + a normal test failure) without changing
// behavior at all in the (expected, common) case where the child exits
// promptly.
//
// Returns true if the process exited on its own within the timeout; false
// if it had to be force-killed (the caller should treat this as a test
// failure signal, not silently continue).
inline auto wait_for_exit_or_kill(pid_t pid, std::chrono::milliseconds timeout) -> bool {
    constexpr auto poll_interval = std::chrono::milliseconds(20);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return true;
        }
        if (result < 0) {
            // ECHILD: already reaped elsewhere, or never a valid child in
            // the first place -- either way, nothing left to wait for.
            return true;
        }
        std::this_thread::sleep_for(poll_interval);
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return false;
}

}  // namespace raft::testing
