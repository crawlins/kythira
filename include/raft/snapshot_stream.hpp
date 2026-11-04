// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file snapshot_stream.hpp
/// @brief Reading and writing a format-1 InstallSnapshot stream: the files
///        of a checkpoint, concatenated in manifest order
///        (.kiro/specs/file-install-snapshot/ task 3, Requirements 3.1-3.3
///        and 5.1).
///
/// A format-1 transfer reuses `offset` as a position in that concatenation
/// (design "Reuse `offset` as a stream offset"), so the sender's loop and the
/// receiver's contiguity check stay the same as format 0. This header holds
/// both ends of the mapping:
///
/// - `snapshot_stream_reader` (leader): positioned reads across the files of
///   a checkpoint directory. It holds no data between calls, so the leader
///   keeps at most one chunk per transfer in memory (Requirement 5.1).
/// - `snapshot_stream_writer` (follower): writes each chunk straight to the
///   right file(s) in a staging directory, hashes each file as it is written,
///   and fsyncs and checks it against the manifest when it is complete
///   (Requirements 3.2, 3.3). It refuses an invalid manifest or a disk
///   without room before creating anything (Requirement 3.1).
///
/// Every refusal is a `snapshot_stream_error` carrying the
/// `install_snapshot_reject_reason` the follower should report, so the node
/// code (task 4) maps a failure to a response without parsing messages.
///
/// The writer stores the manifest in the staging directory as
/// `MANIFEST.kythira` so a restarted follower can reopen the directory and
/// report how far it got (`snapshot_stream_writer::reopen`, used by task 6's
/// resume). A checkpoint file of that name is therefore refused.

#include <raft/persistent_state_machine.hpp>
#include <raft/types.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace kythira {

/// A failure reading or writing a snapshot stream, with the reason code the
/// follower reports in `install_snapshot_response::reject_reason`.
class snapshot_stream_error : public persistent_storage_error {
public:
    snapshot_stream_error(install_snapshot_reject_reason reason, const std::string& what)
        : persistent_storage_error(what), _reason(reason) {}

    [[nodiscard]] auto reason() const noexcept -> install_snapshot_reject_reason { return _reason; }

private:
    install_snapshot_reject_reason _reason;
};

namespace snapshot_stream_detail {

/// Name of the manifest copy inside a staging directory.
inline constexpr std::string_view k_manifest_file_name = "MANIFEST.kythira";

[[noreturn]] inline auto fail(install_snapshot_reject_reason reason, const std::string& what)
    -> void {
    throw snapshot_stream_error(reason, "snapshot stream: " + what);
}

[[noreturn]] inline auto fail_errno(const std::string& what, int err) -> void {
    fail(install_snapshot_reject_reason::io_error,
         what + ": " + std::error_code(err, std::generic_category()).message());
}

/// Owns a file descriptor and closes it on scope exit.
class unique_fd {
public:
    unique_fd() = default;
    explicit unique_fd(int fd) noexcept : _fd(fd) {}
    ~unique_fd() { reset(); }
    unique_fd(const unique_fd&) = delete;
    auto operator=(const unique_fd&) -> unique_fd& = delete;
    unique_fd(unique_fd&& o) noexcept : _fd(std::exchange(o._fd, -1)) {}
    auto operator=(unique_fd&& o) noexcept -> unique_fd& {
        if (this != &o) {
            reset();
            _fd = std::exchange(o._fd, -1);
        }
        return *this;
    }

    [[nodiscard]] auto get() const noexcept -> int { return _fd; }
    [[nodiscard]] auto valid() const noexcept -> bool { return _fd >= 0; }
    auto reset() noexcept -> void {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

private:
    int _fd = -1;
};

inline auto open_file(const std::filesystem::path& path, int flags, ::mode_t mode = 0600)
    -> unique_fd {
    int fd = -1;
    do {
        fd = ::open(path.c_str(), flags | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        fail_errno("open " + path.string(), errno);
    }
    return unique_fd(fd);
}

inline auto fsync_fd(int fd, const std::string& what) -> void {
    if (::fsync(fd) != 0) {
        fail_errno("fsync " + what, errno);
    }
}

inline auto fsync_directory(const std::filesystem::path& dir) -> void {
    const auto fd = open_file(dir, O_RDONLY | O_DIRECTORY);
    fsync_fd(fd.get(), dir.string());
}

/// pwrite the whole buffer, retrying short writes and EINTR.
inline auto pwrite_all(int fd, const std::byte* data, std::size_t size, std::uint64_t offset,
                       const std::string& what) -> void {
    while (size > 0) {
        const auto n = ::pwrite(fd, data, size, static_cast<::off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail_errno("write " + what, errno);
        }
        const auto written = static_cast<std::size_t>(n);
        data += written;
        size -= written;
        offset += written;
    }
}

/// pread exactly `size` bytes; a file shorter than that is an I/O error,
/// because the manifest promised the bytes are there.
inline auto pread_exact(int fd, std::byte* out, std::size_t size, std::uint64_t offset,
                        const std::string& what) -> void {
    while (size > 0) {
        const auto n = ::pread(fd, out, size, static_cast<::off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail_errno("read " + what, errno);
        }
        if (n == 0) {
            fail(install_snapshot_reject_reason::io_error, "short read from " + what);
        }
        const auto got = static_cast<std::size_t>(n);
        out += got;
        size -= got;
        offset += got;
    }
}

/// Validates the manifest for streaming and returns each file's start
/// offset in the stream plus the total length. Refuses a manifest whose
/// sizes overflow 64 bits, or that names the writer's own manifest file.
inline auto stream_layout(const snapshot_manifest& manifest)
    -> std::pair<std::vector<std::uint64_t>, std::uint64_t> {
    try {
        manifest.validate();
    } catch (const snapshot_manifest_error& e) {
        fail(install_snapshot_reject_reason::manifest_invalid, e.what());
    }
    std::vector<std::uint64_t> starts;
    starts.reserve(manifest.files.size());
    std::uint64_t total = 0;
    for (const auto& f : manifest.files) {
        if (f.name == k_manifest_file_name) {
            fail(install_snapshot_reject_reason::manifest_invalid,
                 "checkpoint file uses the reserved name " + std::string(k_manifest_file_name));
        }
        if (f.size > std::numeric_limits<std::uint64_t>::max() - total) {
            fail(install_snapshot_reject_reason::manifest_invalid, "file sizes overflow");
        }
        starts.push_back(total);
        total += f.size;
    }
    return {std::move(starts), total};
}

inline auto file_size_of(const std::filesystem::path& path) -> std::optional<std::uint64_t> {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(size);
}

}  // namespace snapshot_stream_detail

// ---------------------------------------------------------------------------
// Reader (leader side)
// ---------------------------------------------------------------------------

/// Positioned reads over the files of a checkpoint directory, as one stream
/// in manifest order. Thread-safe: every read opens, reads and closes the
/// files it touches and keeps nothing between calls.
class snapshot_stream_reader {
public:
    /// Checks the manifest and that every file exists with its manifest
    /// size. Contents are not re-hashed here; the follower verifies them.
    snapshot_stream_reader(std::filesystem::path dir, snapshot_manifest manifest)
        : _dir(std::move(dir)),
          _manifest(std::move(manifest)),
          // stream_layout validates the manifest and refuses sizes that
          // overflow, so total_bytes() below cannot wrap.
          _starts(snapshot_stream_detail::stream_layout(_manifest).first),
          _total(_manifest.total_bytes()) {
        for (const auto& f : _manifest.files) {
            const auto size = snapshot_stream_detail::file_size_of(_dir / f.name);
            if (!size.has_value() || *size != f.size) {
                snapshot_stream_detail::fail(
                    install_snapshot_reject_reason::io_error,
                    "checkpoint file " + f.name + " is missing or not its manifest size");
            }
        }
    }

    [[nodiscard]] auto manifest() const noexcept -> const snapshot_manifest& { return _manifest; }
    [[nodiscard]] auto directory() const noexcept -> const std::filesystem::path& { return _dir; }
    /// The length of the stream: the sum of the file sizes.
    [[nodiscard]] auto total_bytes() const noexcept -> std::uint64_t { return _total; }

    /// Fills `out` from stream offset `offset`, crossing file boundaries and
    /// skipping zero-length files. Returns the bytes read: `out.size()`, or
    /// fewer only at the end of the stream. An offset past the end throws.
    [[nodiscard]] auto read_into(std::uint64_t offset, std::span<std::byte> out) const
        -> std::size_t {
        if (offset > _total) {
            snapshot_stream_detail::fail(install_snapshot_reject_reason::offset_mismatch,
                                         "read offset " + std::to_string(offset) +
                                             " is past the end of the stream (" +
                                             std::to_string(_total) + " bytes)");
        }
        const auto want =
            static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), _total - offset));
        std::size_t done = 0;
        // The last file whose start is at or before `offset`.
        auto idx = static_cast<std::size_t>(
            std::distance(_starts.begin(), std::ranges::upper_bound(_starts, offset)) - 1);
        while (done < want) {
            const auto& f = _manifest.files.at(idx);
            const auto in_file = offset + done - _starts.at(idx);
            if (in_file >= f.size) {
                ++idx;
                continue;
            }
            const auto n =
                static_cast<std::size_t>(std::min<std::uint64_t>(f.size - in_file, want - done));
            const auto path = _dir / f.name;
            const auto fd = snapshot_stream_detail::open_file(path, O_RDONLY);
            snapshot_stream_detail::pread_exact(fd.get(), out.data() + done, n, in_file,
                                                path.string());
            done += n;
        }
        return done;
    }

    /// Reads up to `max_bytes` from `offset` into a new buffer, which holds
    /// exactly the bytes read.
    [[nodiscard]] auto read(std::uint64_t offset, std::size_t max_bytes) const
        -> std::vector<std::byte> {
        const auto size =
            static_cast<std::size_t>(std::min<std::uint64_t>(max_bytes, remaining_from(offset)));
        std::vector<std::byte> out(size);
        out.resize(read_into(offset, out));
        return out;
    }

private:
    [[nodiscard]] auto remaining_from(std::uint64_t offset) const noexcept -> std::uint64_t {
        return offset >= _total ? 0 : _total - offset;
    }

    std::filesystem::path _dir;
    snapshot_manifest _manifest;
    std::vector<std::uint64_t> _starts;
    std::uint64_t _total = 0;
};

// ---------------------------------------------------------------------------
// Writer (follower side)
// ---------------------------------------------------------------------------

struct snapshot_stream_writer_options {
    /// Free space required beyond the stream's total size before a transfer
    /// starts (Requirement 3.1).
    std::uint64_t disk_margin_bytes = 64ULL * 1024 * 1024;
    /// Bytes available to this process on the filesystem holding the path.
    /// Defaults to `std::filesystem::space(path).available`; tests replace
    /// it to exercise the disk check.
    std::function<std::uint64_t(const std::filesystem::path&)> available_bytes{};
};

/// Writes one format-1 stream into a staging directory. Not thread-safe:
/// one transfer is driven by one handler at a time under the node mutex.
class snapshot_stream_writer {
public:
    /// Starts a transfer: validates the manifest (`manifest_invalid`),
    /// checks free space (`disk_space`), creates `dir`, which must not exist
    /// yet, and stores the manifest in it. Zero-length files at the start of
    /// the stream are created and verified at once.
    [[nodiscard]] static auto create(const std::filesystem::path& dir, snapshot_manifest manifest,
                                     const snapshot_stream_writer_options& options = {})
        -> std::unique_ptr<snapshot_stream_writer> {
        namespace d = snapshot_stream_detail;
        const auto total = d::stream_layout(manifest).second;
        const auto parent =
            dir.parent_path().empty() ? std::filesystem::path(".") : dir.parent_path();
        std::uint64_t available = 0;
        if (options.available_bytes) {
            available = options.available_bytes(parent);
        } else {
            std::error_code ec;
            const auto info = std::filesystem::space(parent, ec);
            if (ec) {
                d::fail(install_snapshot_reject_reason::io_error,
                        "cannot read free space of " + parent.string() + ": " + ec.message());
            }
            available = static_cast<std::uint64_t>(info.available);
        }
        const auto margin = options.disk_margin_bytes;
        if (total > std::numeric_limits<std::uint64_t>::max() - margin ||
            available < total + margin) {
            d::fail(install_snapshot_reject_reason::disk_space,
                    "snapshot needs " + std::to_string(total) + " bytes plus a " +
                        std::to_string(margin) + "-byte margin; " + std::to_string(available) +
                        " available under " + parent.string());
        }

        std::error_code ec;
        if (!std::filesystem::create_directory(dir, ec)) {
            d::fail(install_snapshot_reject_reason::io_error,
                    "cannot create staging directory " + dir.string() + ": " +
                        (ec ? ec.message() : std::string("already exists")));
        }
        {
            const auto encoded = manifest.encode();
            const auto path = dir / d::k_manifest_file_name;
            const auto fd = d::open_file(path, O_WRONLY | O_CREAT | O_EXCL);
            d::pwrite_all(fd.get(), reinterpret_cast<const std::byte*>(encoded.data()),
                          encoded.size(), 0, path.string());
            d::fsync_fd(fd.get(), path.string());
        }
        d::fsync_directory(dir);

        auto w = std::unique_ptr<snapshot_stream_writer>(
            new snapshot_stream_writer(dir, std::move(manifest), total));
        w->begin_file(/*truncate=*/true);
        w->complete_finished_files();
        return w;
    }

    /// Reopens a staging directory left by an earlier writer (a restart, or
    /// a retried transfer). Reads `MANIFEST.kythira`, keeps every complete
    /// file whose SHA-256 still matches and the written prefix of the first
    /// incomplete one, and truncates the rest, so `bytes_received()` is a
    /// prefix the follower can stand behind (Requirement 6.2). Throws
    /// `manifest_invalid` if the stored manifest is missing or corrupt; the
    /// caller then deletes the directory and starts over.
    [[nodiscard]] static auto reopen(const std::filesystem::path& dir)
        -> std::unique_ptr<snapshot_stream_writer> {
        namespace d = snapshot_stream_detail;
        snapshot_manifest manifest;
        {
            const auto path = dir / d::k_manifest_file_name;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                d::fail(install_snapshot_reject_reason::manifest_invalid,
                        "no stored manifest in " + dir.string());
            }
            std::string encoded;
            encoded.resize(snapshot_manifest::k_max_encoded_bytes + 1);
            in.read(encoded.data(), static_cast<std::streamsize>(encoded.size()));
            encoded.resize(static_cast<std::size_t>(in.gcount()));
            try {
                manifest = snapshot_manifest::decode(encoded);
            } catch (const persistent_storage_error& e) {
                d::fail(install_snapshot_reject_reason::manifest_invalid,
                        "stored manifest in " + dir.string() + ": " + e.what());
            }
        }
        const auto total = d::stream_layout(manifest).second;
        auto w = std::unique_ptr<snapshot_stream_writer>(
            new snapshot_stream_writer(dir, std::move(manifest), total));
        w->recover();
        return w;
    }

    ~snapshot_stream_writer() = default;
    snapshot_stream_writer(const snapshot_stream_writer&) = delete;
    auto operator=(const snapshot_stream_writer&) -> snapshot_stream_writer& = delete;
    snapshot_stream_writer(snapshot_stream_writer&&) = delete;
    auto operator=(snapshot_stream_writer&&) -> snapshot_stream_writer& = delete;

    [[nodiscard]] auto manifest() const noexcept -> const snapshot_manifest& { return _manifest; }
    [[nodiscard]] auto directory() const noexcept -> const std::filesystem::path& { return _dir; }
    [[nodiscard]] auto total_bytes() const noexcept -> std::uint64_t { return _total; }
    /// The contiguous prefix of the stream written so far.
    [[nodiscard]] auto bytes_received() const noexcept -> std::uint64_t { return _received; }
    /// True once every byte has arrived and every file is verified.
    [[nodiscard]] auto complete() const noexcept -> bool { return _file == _manifest.files.size(); }
    /// True after a checksum or I/O failure; the transfer must be abandoned.
    [[nodiscard]] auto failed() const noexcept -> bool { return _failed; }

    /// Writes one chunk at stream offset `offset`, which must equal
    /// `bytes_received()` (`offset_mismatch` otherwise, as format 0's
    /// contiguity check). Each file that the chunk completes is fsynced and
    /// checked against the manifest (`checksum` on a mismatch).
    auto write(std::uint64_t offset, std::span<const std::byte> data) -> void {
        namespace d = snapshot_stream_detail;
        if (_failed) {
            d::fail(install_snapshot_reject_reason::io_error,
                    "writer for " + _dir.string() + " already failed");
        }
        if (offset != _received) {
            d::fail(
                install_snapshot_reject_reason::offset_mismatch,
                "chunk at " + std::to_string(offset) + ", expected " + std::to_string(_received));
        }
        if (data.size() > _total - _received) {
            d::fail(install_snapshot_reject_reason::offset_mismatch,
                    "chunk of " + std::to_string(data.size()) + " bytes at " +
                        std::to_string(offset) + " runs past the end of the stream (" +
                        std::to_string(_total) + " bytes)");
        }
        guarded([&] {
            while (!data.empty()) {
                const auto& f = _manifest.files.at(_file);
                const auto n = static_cast<std::size_t>(
                    std::min<std::uint64_t>(data.size(), f.size - _in_file));
                d::pwrite_all(_fd.get(), data.data(), n, _in_file, current_path().string());
                _hasher->update(data.data(), n);
                _in_file += n;
                _received += n;
                data = data.subspan(n);
                complete_finished_files();
            }
        });
    }

    /// Makes the finished staging directory durable before ingest
    /// (Requirement 3.3). Throws unless `complete()`.
    auto finish() -> void {
        if (!complete() || _failed) {
            snapshot_stream_detail::fail(install_snapshot_reject_reason::io_error,
                                         "finish before the stream is complete (" +
                                             std::to_string(_received) + " of " +
                                             std::to_string(_total) + " bytes)");
        }
        guarded([&] { snapshot_stream_detail::fsync_directory(_dir); });
    }

private:
    snapshot_stream_writer(std::filesystem::path dir, snapshot_manifest manifest,
                           std::uint64_t total)
        : _dir(std::move(dir)), _manifest(std::move(manifest)), _total(total) {}

    [[nodiscard]] auto current_path() const -> std::filesystem::path {
        return _dir / _manifest.files.at(_file).name;
    }

    /// Runs `fn`, marking the writer failed if it throws.
    template<typename Fn> auto guarded(Fn&& fn) -> void {
        try {
            std::forward<Fn>(fn)();
        } catch (...) {
            _failed = true;
            _fd.reset();
            throw;
        }
    }

    /// Opens the current file (if any) for writing at `_in_file`.
    auto begin_file(bool truncate) -> void {
        _hasher.reset();
        if (_file == _manifest.files.size()) {
            return;
        }
        _hasher.emplace();
        _fd = snapshot_stream_detail::open_file(current_path(),
                                                O_WRONLY | O_CREAT | (truncate ? O_TRUNC : 0));
    }

    /// Finalizes every file the stream has fully covered, including
    /// zero-length files, and opens the next one.
    auto complete_finished_files() -> void {
        namespace d = snapshot_stream_detail;
        while (_file < _manifest.files.size() && _in_file == _manifest.files.at(_file).size) {
            const auto& f = _manifest.files.at(_file);
            const auto path = current_path();
            d::fsync_fd(_fd.get(), path.string());
            _fd.reset();
            if (_hasher->finish() != f.sha256) {
                _failed = true;
                d::fail(install_snapshot_reject_reason::checksum,
                        "SHA-256 of " + f.name + " does not match the manifest");
            }
            ++_file;
            _in_file = 0;
            begin_file(/*truncate=*/true);
        }
    }

    /// Rebuilds the position from what is on disk (see `reopen`).
    auto recover() -> void {
        namespace d = snapshot_stream_detail;
        _received = 0;
        _file = 0;
        _in_file = 0;
        // Complete files that still verify.
        while (_file < _manifest.files.size()) {
            const auto& f = _manifest.files.at(_file);
            const auto size = d::file_size_of(current_path());
            if (!size.has_value() || *size != f.size) {
                break;
            }
            const auto [sha, n] = sha256_of_file(current_path());
            if (sha != f.sha256 || n != f.size) {
                break;
            }
            _received += f.size;
            ++_file;
        }
        // Truncate everything after the first incomplete file; keep that
        // file's prefix, re-hashed so writing can continue.
        for (auto i = _file + 1; i < _manifest.files.size(); ++i) {
            std::error_code ec;
            std::filesystem::remove(_dir / _manifest.files.at(i).name, ec);
        }
        if (_file == _manifest.files.size()) {
            _hasher.reset();
            return;
        }
        const auto& f = _manifest.files.at(_file);
        const auto on_disk = d::file_size_of(current_path()).value_or(0);
        if (on_disk >= f.size) {
            // Full size but wrong contents, or longer than promised: the
            // prefix cannot be trusted at all.
            std::error_code ec;
            std::filesystem::remove(current_path(), ec);
            begin_file(/*truncate=*/true);
            complete_finished_files();
            return;
        }
        begin_file(/*truncate=*/false);
        std::vector<std::byte> buf(64 * 1024);
        std::uint64_t pos = 0;
        const auto rfd = d::open_file(current_path(), O_RDONLY);
        while (pos < on_disk) {
            const auto n =
                static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), on_disk - pos));
            d::pread_exact(rfd.get(), buf.data(), n, pos, current_path().string());
            _hasher->update(buf.data(), n);
            pos += n;
        }
        _in_file = on_disk;
        _received += on_disk;
    }

    std::filesystem::path _dir;
    snapshot_manifest _manifest;
    std::uint64_t _total = 0;

    std::uint64_t _received = 0;
    std::size_t _file = 0;       ///< Index of the file being written.
    std::uint64_t _in_file = 0;  ///< Bytes of that file written.
    snapshot_stream_detail::unique_fd _fd;
    std::optional<persistent_detail::sha256_hasher> _hasher;
    bool _failed = false;
};

}  // namespace kythira
