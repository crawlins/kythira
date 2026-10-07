// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file private_file.hpp
/// @brief Write files whose contents must never be readable by other users
///        (private keys) without a window where they are.
///
/// `std::ofstream` creates a file with mode `0666 & ~umask`, so a key written
/// that way under the common `022` umask is world-readable, and a
/// `std::filesystem::permissions()` call afterwards only narrows the mode once
/// the bytes are already on disk: a reader that opened the file in between
/// keeps its descriptor. write_file_atomically() instead creates a fresh,
/// uniquely-named temporary in the target's directory with `O_CREAT | O_EXCL |
/// O_NOFOLLOW`, sets the final mode on the descriptor before writing a byte,
/// fsyncs, and renames it over the target. A reader therefore sees either the
/// old file or the complete new one, and a private file is `0600` for its
/// whole life whatever the process umask is.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace raft {

/// Who may read a file written by write_file_atomically().
enum class file_visibility {
    /// Mode `0666 & ~umask`, the same as an ofstream-created file. For
    /// certificates and other public material.
    standard,
    /// Mode exactly `0600`, set before any content is written. For private
    /// keys and other secrets.
    owner_only,
};

namespace detail {

[[noreturn]] inline auto throw_file_error(const std::string& what,
                                          const std::filesystem::path& path, int err) -> void {
    throw std::filesystem::filesystem_error(what, path,
                                            std::error_code(err, std::generic_category()));
}

}  // namespace detail

/// Atomically replaces @p path with @p content (see the file comment). The
/// parent directory must exist. Throws std::filesystem::filesystem_error on
/// failure, in which case @p path is left as it was and the temporary is
/// removed.
inline auto write_file_atomically(const std::filesystem::path& path, const std::string& content,
                                  file_visibility visibility) -> void {
    static std::atomic<unsigned> counter{0};
    const mode_t create_mode = visibility == file_visibility::owner_only ? 0600 : 0666;

    std::filesystem::path tmp;
    int fd = -1;
    // O_EXCL makes a name collision (a leftover temporary, or one planted by
    // another user in a shared directory) an EEXIST we retry past, never a
    // file we write through.
    for (int attempt = 0; attempt < 100 && fd < 0; ++attempt) {
        tmp = path;
        tmp += ".tmp." + std::to_string(::getpid()) + "." +
               std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, create_mode);
        if (fd < 0 && errno != EEXIST) {
            detail::throw_file_error("write_file_atomically: cannot create temporary", tmp, errno);
        }
    }
    if (fd < 0) {
        detail::throw_file_error("write_file_atomically: no free temporary name", tmp, EEXIST);
    }

    auto fail = [&](const char* what) {
        const int err = errno;
        ::close(fd);
        ::unlink(tmp.c_str());
        detail::throw_file_error(what, tmp, err);
    };

    // The create mode is narrowed by the umask but never widened, so 0600 is
    // already the most it can be; fchmod makes it exactly 0600 even under an
    // unusual umask that strips the owner bits too.
    if (visibility == file_visibility::owner_only && ::fchmod(fd, 0600) != 0) {
        fail("write_file_atomically: fchmod failed");
    }
    std::size_t written = 0;
    while (written < content.size()) {
        const auto n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("write_file_atomically: write failed");
        }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        fail("write_file_atomically: fsync failed");
    }
    if (::close(fd) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        detail::throw_file_error("write_file_atomically: close failed", tmp, err);
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        detail::throw_file_error("write_file_atomically: rename failed", path, err);
    }
}

/// Writes a secret (a private key) to @p path with mode 0600 from creation.
inline auto write_private_file(const std::filesystem::path& path, const std::string& content)
    -> void {
    write_file_atomically(path, content, file_visibility::owner_only);
}

/// Creates a new directory named `<parent>/<prefix>XXXXXX` with mode 0700 and
/// returns its path. Unlike create_directories() under a shared temp directory,
/// the name is unpredictable and the directory is guaranteed fresh, so no other
/// user can have pre-created it or can list or open the files placed in it.
inline auto make_private_temp_directory(const std::filesystem::path& parent,
                                        const std::string& prefix) -> std::filesystem::path {
    std::string templ = (parent / (prefix + "XXXXXX")).string();
    if (::mkdtemp(templ.data()) == nullptr) {
        detail::throw_file_error("make_private_temp_directory: mkdtemp failed", parent, errno);
    }
    // mkdtemp's 0700 is narrowed by the umask; a umask that strips owner
    // bits would leave a directory its creator cannot write into.
    if (::chmod(templ.c_str(), 0700) != 0) {
        const int err = errno;
        ::rmdir(templ.c_str());
        detail::throw_file_error("make_private_temp_directory: chmod failed", templ, err);
    }
    return templ;
}

}  // namespace raft
