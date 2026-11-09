// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A node-wide token bucket for InstallSnapshot bytes
// (.kiro/specs/file-install-snapshot/ Requirement 6.3).
//
// Snapshot chunks share links with every group's AppendEntries and
// heartbeats. A per-group limit would multiply by the group count, so one
// pacer is shared by every group on a node: `raft_configuration` holds it
// by `shared_ptr`, and `multi_raft` copies one configuration into every
// group. A null pacer is unlimited, the default.
//
// The bucket runs into debt instead of refusing: `reserve(n)` always takes
// the bytes and returns how long the caller should wait before sending, so
// a chunk larger than the burst still goes, just later. The caller waits
// with a future delay, never by blocking a thread.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace kythira {

class snapshot_pacer {
public:
    using clock = std::chrono::steady_clock;

    /// `bytes_per_second` of 0 is unlimited. `burst_bytes` of 0 means one
    /// second's worth.
    explicit snapshot_pacer(std::uint64_t bytes_per_second, std::uint64_t burst_bytes = 0)
        : _rate(bytes_per_second),
          _burst(burst_bytes != 0 ? burst_bytes : bytes_per_second),
          _tokens(static_cast<double>(_burst)),
          _last(clock::now()) {}

    [[nodiscard]] auto bytes_per_second() const noexcept -> std::uint64_t { return _rate; }

    /// Takes `bytes` and returns how long to wait before sending them.
    auto reserve(std::uint64_t bytes, clock::time_point now = clock::now())
        -> std::chrono::nanoseconds {
        if (_rate == 0) {
            return std::chrono::nanoseconds{0};
        }
        const std::lock_guard lock(_mu);
        refill_locked(now);
        _tokens -= static_cast<double>(bytes);
        if (_tokens >= 0) {
            return std::chrono::nanoseconds{0};
        }
        const double seconds = -_tokens / static_cast<double>(_rate);
        return std::chrono::nanoseconds{static_cast<std::int64_t>(seconds * 1e9)};
    }

private:
    auto refill_locked(clock::time_point now) -> void {
        if (now <= _last) {
            return;
        }
        const double elapsed = std::chrono::duration<double>(now - _last).count();
        _tokens =
            std::min(static_cast<double>(_burst), _tokens + (elapsed * static_cast<double>(_rate)));
        _last = now;
    }

    std::mutex _mu;
    std::uint64_t _rate;
    std::uint64_t _burst;
    double _tokens;
    clock::time_point _last;
};

}  // namespace kythira
