// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file oscore_sequence_store.hpp
/// @brief Where an OSCORE Security Context keeps the two counters that must
///        outlive it: the Sender Sequence Number and the replay floor.
///
/// RFC 8613 Section 7.2.1 makes the Sender Sequence Number the one thing an
/// endpoint may never get wrong: every Partial IV it issues under a key must be
/// fresh, because the AEAD nonce is built from it and AES-CCM with a repeated
/// nonce leaks the XOR of the two plaintexts. A context that starts at zero
/// every time it is built repeats every nonce it ever used, on every restart,
/// and whenever two contexts are built from the same credentials in one process
/// (a server's stop()/start(), a client and server sharing a Sender ID).
///
/// The recipient side has the mirror problem (Section 7.4): a replay window
/// that starts empty accepts every request an attacker recorded before the
/// restart.
///
/// A store fixes both the way Appendix B.1.1 describes. The sender reserves a
/// block of sequence numbers and makes the reservation durable *before* it
/// issues the first of them, so after a crash the next context starts above
/// anything that could have reached the wire. The recipient durably raises a
/// floor above every Partial IV it is about to accept, and a rebuilt context
/// rejects anything below it.
///
/// Two stores are provided. The process-wide memory store is the default: it
/// survives a context being rebuilt but not the process. The file store, chosen
/// by `oscore_credentials::sequence_state_dir`, survives restarts too.
///
/// Keys are opaque fingerprints (see `security_context`), so a store never
/// learns IDs or key material and one directory can serve many contexts.

#include <raft/coap_exceptions.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace kythira::oscore {

class sequence_store {
public:
    sequence_store() = default;
    sequence_store(const sequence_store&) = delete;
    auto operator=(const sequence_store&) -> sequence_store& = delete;
    virtual ~sequence_store() = default;

    /// Returns the value stored for `key` (0 if none) and durably raises it by
    /// `count` before returning, so the half-open range [result, result+count)
    /// belongs to the caller alone, now and after any restart. Atomic with
    /// respect to every other caller of the same store.
    [[nodiscard]] virtual auto reserve(const std::string& key, std::uint64_t count)
        -> std::uint64_t = 0;

    /// Durably raises the value for `key` to at least `value`. Never lowers it.
    virtual auto raise_to(const std::string& key, std::uint64_t value) -> void = 0;

    /// The value stored for `key`, or 0 if none.
    [[nodiscard]] virtual auto load(const std::string& key) -> std::uint64_t = 0;

    /// How many sequence numbers a sender reserves per write. Larger means
    /// fewer writes and more numbers skipped per restart.
    [[nodiscard]] virtual auto sequence_block() const -> std::uint64_t = 0;

    /// How far past an accepted Partial IV the recipient raises its floor per
    /// write. After a restart, a peer that did not restart has up to this many
    /// of its next requests rejected, so it is kept small.
    [[nodiscard]] virtual auto replay_block() const -> std::uint64_t = 0;
};

/// Survives a Security Context being rebuilt, not the process. Writes are
/// free, so the replay floor tracks every accepted Partial IV exactly.
class memory_sequence_store final : public sequence_store {
public:
    [[nodiscard]] auto reserve(const std::string& key, std::uint64_t count)
        -> std::uint64_t override {
        const std::lock_guard lock(_mutex);
        auto& value = _values[key];
        const auto start = value;
        value = saturating_add(start, count);
        return start;
    }

    auto raise_to(const std::string& key, std::uint64_t value) -> void override {
        const std::lock_guard lock(_mutex);
        auto& stored = _values[key];
        stored = std::max(stored, value);
    }

    [[nodiscard]] auto load(const std::string& key) -> std::uint64_t override {
        const std::lock_guard lock(_mutex);
        const auto it = _values.find(key);
        return it == _values.end() ? 0 : it->second;
    }

    [[nodiscard]] auto sequence_block() const -> std::uint64_t override { return 64; }
    [[nodiscard]] auto replay_block() const -> std::uint64_t override { return 1; }

    static auto saturating_add(std::uint64_t a, std::uint64_t b) -> std::uint64_t {
        return a > UINT64_MAX - b ? UINT64_MAX : a + b;
    }

private:
    std::mutex _mutex;
    std::map<std::string, std::uint64_t> _values;
};

/// One file per key under a directory, each holding a decimal value, replaced
/// by write-to-temporary, fsync, rename, fsync-the-directory, so a crash leaves
/// either the old value or the new one. An flock on a lock file serialises
/// processes that share the directory; the mutex serialises threads.
class file_sequence_store final : public sequence_store {
public:
    explicit file_sequence_store(std::filesystem::path directory)
        : _directory(std::move(directory)) {
        std::error_code ec;
        std::filesystem::create_directories(_directory, ec);
        if (ec) {
            throw coap_security_config_error("OSCORE: cannot create sequence state directory '" +
                                             _directory.string() + "': " + ec.message());
        }
        const auto lock_path = _directory / "lock";
        _lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (_lock_fd < 0) {
            throw coap_security_config_error("OSCORE: cannot open '" + lock_path.string() +
                                             "': " + std::strerror(errno));
        }
    }

    ~file_sequence_store() override {
        if (_lock_fd >= 0) {
            ::close(_lock_fd);
        }
    }

    [[nodiscard]] auto reserve(const std::string& key, std::uint64_t count)
        -> std::uint64_t override {
        const std::lock_guard lock(_mutex);
        const file_lock guard(_lock_fd);
        const auto start = read_value(key);
        write_value(key, memory_sequence_store::saturating_add(start, count));
        return start;
    }

    auto raise_to(const std::string& key, std::uint64_t value) -> void override {
        const std::lock_guard lock(_mutex);
        const file_lock guard(_lock_fd);
        if (read_value(key) < value) {
            write_value(key, value);
        }
    }

    [[nodiscard]] auto load(const std::string& key) -> std::uint64_t override {
        const std::lock_guard lock(_mutex);
        const file_lock guard(_lock_fd);
        return read_value(key);
    }

    // One fsync per 4096 messages sent; 2^40 / 4096 is far more restarts than
    // any deployment will see.
    [[nodiscard]] auto sequence_block() const -> std::uint64_t override { return 4096; }
    // Matches the 64-entry replay window: one fsync per 64 accepted requests,
    // at most 64 of a surviving peer's requests refused after a restart.
    [[nodiscard]] auto replay_block() const -> std::uint64_t override { return 64; }

private:
    struct file_lock {
        explicit file_lock(int fd) : _fd(fd) {
            while (::flock(_fd, LOCK_EX) != 0) {
                if (errno != EINTR) {
                    throw coap_security_error(std::string("OSCORE: flock failed: ") +
                                              std::strerror(errno));
                }
            }
        }
        file_lock(const file_lock&) = delete;
        auto operator=(const file_lock&) -> file_lock& = delete;
        ~file_lock() { ::flock(_fd, LOCK_UN); }
        int _fd;
    };

    [[nodiscard]] auto read_value(const std::string& key) const -> std::uint64_t {
        const auto path = _directory / key;
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT) {
                return 0;
            }
            throw coap_security_error("OSCORE: cannot read '" + path.string() +
                                      "': " + std::strerror(errno));
        }
        char buffer[32] = {};
        const auto got = ::read(fd, buffer, sizeof(buffer) - 1);
        ::close(fd);
        if (got <= 0) {
            // A value that cannot be read must not be taken as zero: that is
            // exactly the nonce reuse this store exists to prevent.
            throw coap_security_error("OSCORE: sequence state file '" + path.string() +
                                      "' is empty or unreadable");
        }
        char* end = nullptr;
        errno = 0;
        const auto value = std::strtoull(buffer, &end, 10);
        if (errno != 0 || end == buffer) {
            throw coap_security_error("OSCORE: sequence state file '" + path.string() +
                                      "' is corrupt");
        }
        return value;
    }

    auto write_value(const std::string& key, std::uint64_t value) const -> void {
        const auto path = _directory / key;
        const auto temporary = _directory / (key + ".tmp");
        const auto text = std::to_string(value) + "\n";
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) {
            throw coap_security_error("OSCORE: cannot write '" + temporary.string() +
                                      "': " + std::strerror(errno));
        }
        const bool written =
            ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) &&
            ::fsync(fd) == 0;
        ::close(fd);
        if (!written || ::rename(temporary.c_str(), path.c_str()) != 0) {
            throw coap_security_error("OSCORE: cannot persist '" + path.string() +
                                      "': " + std::strerror(errno));
        }
        // The rename is only durable once the directory entry is.
        const int dir_fd = ::open(_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd < 0 || ::fsync(dir_fd) != 0) {
            const auto error = errno;
            if (dir_fd >= 0) {
                ::close(dir_fd);
            }
            throw coap_security_error("OSCORE: cannot sync '" + _directory.string() +
                                      "': " + std::strerror(error));
        }
        ::close(dir_fd);
    }

    std::filesystem::path _directory;
    std::mutex _mutex;
    int _lock_fd{-1};
};

/// The store a context built from credentials naming `directory` uses: the
/// process-wide memory store when it is empty, otherwise one file store per
/// directory, shared, so every context on that directory serialises through
/// the same mutex.
[[nodiscard]] inline auto sequence_store_for(const std::string& directory)
    -> std::shared_ptr<sequence_store> {
    static std::mutex registry_mutex;
    static const auto memory = std::make_shared<memory_sequence_store>();
    static std::map<std::filesystem::path, std::weak_ptr<sequence_store>> files;
    if (directory.empty()) {
        return memory;
    }
    const std::lock_guard lock(registry_mutex);
    const auto canonical = std::filesystem::weakly_canonical(directory);
    if (auto existing = files[canonical].lock()) {
        return existing;
    }
    auto created = std::make_shared<file_sequence_store>(canonical);
    files[canonical] = created;
    return created;
}

}  // namespace kythira::oscore
