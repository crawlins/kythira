// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Replaces a small file so that a crash leaves either the old contents or the
// new, never a mix: write a sibling, sync it, rename it over the file, then
// sync the directory, without which the rename itself may not survive.

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace dynamodb_gateway {

/// False, leaving the old file, when any step fails.
[[nodiscard]] inline auto write_durably(const std::filesystem::path& path, std::string_view text)
    -> bool {
    const auto tmp = path.string() + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    std::size_t done = 0;
    bool ok = true;
    while (ok && done < text.size()) {
        const auto n = ::write(fd, text.data() + done, text.size() - done);
        ok = n > 0;
        done += ok ? static_cast<std::size_t>(n) : 0;
    }
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    std::error_code ec;
    if (!ok) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        return false;
    }
    const auto dir = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        return false;
    }
    const bool synced = ::fsync(dfd) == 0;
    ::close(dfd);
    return synced;
}

}  // namespace dynamodb_gateway
