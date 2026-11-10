// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kinesis_disk_guard.hpp
/// @brief The disk guard (`.kiro/specs/kinesis-compatible-streams/` task
///        10, Requirement 11.4): writes are refused, with a message naming
///        the guard, before the segment disk's free space falls below a
///        floor.
///
/// Checked by the node that proposes an append and again by the leader a
/// forwarded append reaches, so a full leader refuses even when the node
/// the client called has room. `apply()` never consults it: whether an
/// entry applies must not depend on one replica's disk. Reads, trims and
/// tiering go on, and free space comes back as the maintenance pass trims
/// and tiers (design, "Disk floor reached").
///
/// `statvfs` is called at most once per `_recheck`, so a burst of appends
/// costs one system call.

#include <kythira/kinesis/kinesis_errors.hpp>
#include <kythira/kinesis/kinesis_stream_state_machine.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <sys/statvfs.h>

namespace kythira::kinesis {

/// The message a refused write carries (`InternalFailure`, as the design
/// says, with Kythira's own text instead of AWS's terse one).
inline constexpr const char* k_disk_guard_message =
    "Kythira disk guard: free space on the record segment disk is below the configured floor "
    "(KINESIS_DISK_FLOOR_BYTES); writes are refused until retention or tiering frees space.";

/// `InternalFailure` (HTTP 500) with the disk guard's message.
[[nodiscard]] inline auto storage_full_error() -> aws_json::error {
    auto e = internal_failure();
    e.message = k_disk_guard_message;
    return e;
}

class disk_guard {
public:
    /// Free bytes on the guarded file system, or nullopt when unknown.
    using free_bytes_fn = std::function<std::optional<std::uint64_t>()>;
    using clock_fn = std::function<std::int64_t()>;

    /// Guard `path`'s file system: below `floor_bytes` free, appends are
    /// refused. A floor of 0 never refuses.
    disk_guard(std::filesystem::path path, std::uint64_t floor_bytes,
               std::chrono::milliseconds recheck = std::chrono::seconds{1})
        : disk_guard(floor_bytes, statvfs_free(std::move(path)), steady_ms, recheck) {}

    /// Tests: any source of free space and time.
    disk_guard(std::uint64_t floor_bytes, free_bytes_fn free_bytes, clock_fn clock,
               std::chrono::milliseconds recheck = std::chrono::seconds{1})
        : _floor(floor_bytes),
          _free(std::move(free_bytes)),
          _clock(std::move(clock)),
          _recheck(recheck.count()) {}

    /// Whether appends are refused now. Unknown free space admits: a
    /// guard that cannot read the disk must not stop the cluster.
    [[nodiscard]] auto below_floor() -> bool {
        if (_floor == 0) {
            return false;
        }
        const std::scoped_lock lock(_mutex);
        const auto now = _clock();
        if (!_checked || now - _checked_at >= _recheck) {
            const auto free = _free();
            _below = free && *free < _floor;
            _checked = true;
            _checked_at = now;
        }
        return _below;
    }

    [[nodiscard]] auto floor_bytes() const noexcept -> std::uint64_t { return _floor; }

private:
    std::uint64_t _floor;
    free_bytes_fn _free;
    clock_fn _clock;
    std::int64_t _recheck;
    std::mutex _mutex;
    bool _checked = false;
    bool _below = false;
    std::int64_t _checked_at = 0;

    static auto steady_ms() -> std::int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static auto statvfs_free(std::filesystem::path path) -> free_bytes_fn {
        return [p = std::move(path)]() -> std::optional<std::uint64_t> {
            struct statvfs st{};
            if (::statvfs(p.c_str(), &st) != 0) {
                return std::nullopt;
            }
            return static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize;
        };
    }
};

inline constexpr std::uint8_t k_append_kind = 4;
inline constexpr std::uint8_t k_append_batch_kind = 8;
static_assert(std::is_same_v<std::variant_alternative_t<k_append_kind, stream_command>,
                             append_records_command>);
static_assert(std::is_same_v<std::variant_alternative_t<k_append_batch_kind, stream_command>,
                             append_batch_command>);

/// Whether `bytes` is an append (or a batch of them): what the guard stops.
[[nodiscard]] inline auto is_append_command(const std::vector<std::byte>& bytes) -> bool {
    // The kind byte follows the version byte (encode_command).
    if (bytes.size() < 2) {
        return false;
    }
    const auto kind = static_cast<std::uint8_t>(bytes[1]);
    return kind == k_append_kind || kind == k_append_batch_kind;
}

/// `storage_full` for an append when `guard` is below its floor; nullopt to
/// go ahead.
[[nodiscard]] inline auto refuse_when_full(disk_guard* guard, const std::vector<std::byte>& bytes)
    -> std::optional<apply_result> {
    if (guard != nullptr && is_append_command(bytes) && guard->below_floor()) {
        return apply_result{._status = apply_status::storage_full};
    }
    return std::nullopt;
}

}  // namespace kythira::kinesis
