// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Where the in-process cluster tests put their chunk stores.
//
// The disk-backed state machine fsyncs a chunk, its pack record and the
// directory on every apply, and apply runs under the Raft node's mutex, the
// same one tick() needs to send heartbeats. On a CI runner the other tests
// running beside these under `ctest -j` fsync too, among them the 1 GiB PUT
// in cache_http_front_end_test, and one of these fsyncs was measured stalling
// a leader for ~850 ms, past the followers' election timeout. They elected
// a new leader, and the test's next command got "shard has no reachable
// leader". That was the retry-rescued failure in
// redis_gateway_disk_store_integration_test and cache_http_front_end_test.
//
// These tests check what the cache stores and replies, not fsync latency, so
// their stores go on tmpfs when the machine has one. cache_chunk_store's own
// unit tests, and the 1 GiB PUT, keep using the real temp directory. The
// heartbeat stall itself is a property of the disk mode, recorded in
// doc/TODO.md.

#include <unistd.h>

#include <filesystem>

namespace kythira::testing {

/// /dev/shm when it is a writable directory, else the temp directory.
inline auto fast_scratch_root() -> std::filesystem::path {
    std::error_code ec;
    std::filesystem::path shm{"/dev/shm"};
    if (std::filesystem::is_directory(shm, ec) && ::access(shm.c_str(), W_OK) == 0) {
        return shm;
    }
    return std::filesystem::temp_directory_path();
}

}  // namespace kythira::testing
