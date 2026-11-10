// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file disk_guard.hpp
/// @brief Refuses DynamoDB writes before the data directory runs out of space
///        (`.kiro/specs/dynamodb-compatible-api/` task 6, Requirement 20.2).
///
/// The gateway asks the guard before it proposes a write. Below the floor a
/// write gets `InternalServerError` with a message naming the guard. Reads
/// go on as normal. The check happens before proposing, not in `apply()`:
/// free space differs from replica to replica, so a decision based on it in
/// `apply()` would make replicas diverge. The floor leaves room for entries
/// already in flight, plus compaction and checkpoints.
///
/// The free-space probe can be swapped out, so tests do not need a full disk.

#include <kythira/dynamodb/errors.hpp>

#include <raft/aws_json_protocol.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace kythira::dynamodb {

class disk_guard {
public:
    /// Free bytes available to the process at a path, or nothing when the
    /// path cannot be examined.
    using probe = std::function<std::optional<std::uintmax_t>(const std::filesystem::path&)>;

    /// The default floor: 1 GiB.
    static constexpr std::uintmax_t k_default_floor_bytes = std::uintmax_t{1} << 30U;

    explicit disk_guard(std::filesystem::path data_dir,
                        std::uintmax_t floor_bytes = k_default_floor_bytes,
                        probe free_space = filesystem_probe())
        : _dir(std::move(data_dir)), _floor(floor_bytes), _probe(std::move(free_space)) {}

    /// `std::filesystem::space(path).available`.
    [[nodiscard]] static auto filesystem_probe() -> probe {
        return [](const std::filesystem::path& p) -> std::optional<std::uintmax_t> {
            std::error_code ec;
            const auto info = std::filesystem::space(p, ec);
            if (ec) {
                return std::nullopt;
            }
            return info.available;
        };
    }

    /// Nothing when a write may go ahead. Otherwise the error to return.
    /// A directory whose free space cannot be read is treated as full: the
    /// guard fails closed.
    [[nodiscard]] auto check_write() const -> std::optional<aws_json::error> {
        const auto free = _probe(_dir);
        if (free && *free >= _floor) {
            return std::nullopt;
        }
        aws_json::error e;
        e.status = 500;
        e.type = dynamodb_error_style().internal_type;
        e.message = free ? "Kythira disk guard: free space on the data directory (" +
                               std::to_string(*free) + " bytes) is below the floor of " +
                               std::to_string(_floor) +
                               " bytes; writes are refused until space is freed"
                         : "Kythira disk guard: cannot read the free space of the data "
                           "directory; writes are refused";
        return e;
    }

    [[nodiscard]] auto floor_bytes() const -> std::uintmax_t { return _floor; }
    [[nodiscard]] auto data_dir() const -> const std::filesystem::path& { return _dir; }

private:
    std::filesystem::path _dir;
    std::uintmax_t _floor;
    probe _probe;
};

}  // namespace kythira::dynamodb
