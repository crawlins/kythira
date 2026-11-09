// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file cache_chunk_store.hpp
/// @brief The per-node, content-addressed chunk store behind the disk-backed
///        build cache (.kiro/specs/disk-backed-build-cache/ design Components
///        1 and 1a, Requirements 2.1, 2.5, 3.3, 6.7, 6.8 and 10.1).
///
/// One store per node, shared by every shard replica on that node. It knows
/// chunks only by their SHA-256 and has no idea which key uses one: reference
/// counts live in the state machine (`chunk_refcounts` below), which keeps the
/// store testable on its own.
///
/// Layout under the data directory:
///
/// ```
/// chunks/ab/cd/abcd…(64 hex)   one large chunk's bytes, nothing else
/// chunks/tmp/                  in-progress writes, wiped on open
/// packs/pack.<n>               small chunks, appended
/// packs/pack.<n>.dead          offsets of dead records in pack.<n>
/// ```
///
/// **Large chunks** (at least the pack threshold) are one file each. A write
/// goes to `chunks/tmp/`, is fsynced, and is then hard-linked into place and
/// the temporary name unlinked. `link` rather than `rename`: `link` fails with
/// `EEXIST` when the name is already taken, so exactly one of two concurrent
/// writers of the same chunk is told it created it, and the store's file and
/// byte counters stay exact. The bytes are identical either way, which is what
/// makes a replayed `put` a no-op. A crash before the link leaves only a
/// temporary file, which the next open wipes, so a valid chunk name never
/// holds partial contents (Requirement 2.5).
///
/// **Small chunks** go to append-only pack files, so that a workload of tiny
/// values cannot exhaust inodes before bytes (Requirement 6.7). A pack is a
/// run of records `{ 32-byte sha, u32 BE len, bytes, u32 BE crc32 }`, the CRC
/// covering everything before it. One pack takes appends at a time and is
/// closed when it reaches the pack size. RAM holds `sha → (pack, offset, len)`
/// for packed chunks, rebuilt on open by scanning the packs. Removing a packed
/// chunk appends its record's offset to the pack's `.dead` sidecar, fsynced,
/// so a restart does not resurrect it. Offsets rather than hashes: a chunk
/// removed and later stored again can have a dead and a live record in the
/// same pack.
///
/// **Compaction** (Requirement 6.8) copies a closed pack's live records into
/// the open pack once its live bytes fall below half its size, fsyncs, and
/// then unlinks the old pack before its sidecar. A crash between the copy and
/// the unlink leaves each live chunk in two packs. The open scan keeps the
/// copy in the lower-numbered pack and records the other as dead in its own
/// sidecar, so a later `remove` of that chunk cannot be undone by the
/// duplicate surviving a restart. Unlinking the pack before its sidecar means
/// a crash between the two leaves a sidecar with no pack, which the open scan
/// deletes; the reverse order would bring the old pack's dead records back.
///
/// **Torn tails.** The open scan stops at the first record that runs past the
/// end of its pack or fails its CRC, and truncates the pack there. For an
/// append-only file whose appends are fsynced before `put` returns, that is a
/// torn final write. Bit rot in the middle of a pack loses the records after
/// it too; those chunks are then absent, and the state machine treats an
/// absent referenced chunk as one to fetch from a peer (Requirement 2.6).
///
/// **Reads are always verified** (Requirement 10.1): `read` hashes what it
/// read and reports a mismatch as absent, never as bytes.
///
/// None of this touches replicated state. Two replicas may pack the same
/// chunks differently and remain identical, because the state machine names
/// chunks by hash and never by location.
///
/// **Deferred durability.** A `put` with `chunk_durability::deferred` (what
/// `chunk_refcounts`, and so the state machine's `apply()`, uses) writes the
/// bytes and returns without an fsync. The state machine applies under the
/// Raft node's mutex, which `tick()` needs to send heartbeats, so an fsync
/// there silenced the whole node for as long as the disk stalled: ~850 ms
/// was measured, against 150-300 ms election timeouts. The store's syncer
/// thread does the fsyncs instead, and the bytes are readable from the page
/// cache meanwhile. A deferred large chunk stays at its temporary name
/// (`_staged`, which `read`, `contains` and `remove` consult) until the
/// syncer has fsynced it; only then is it linked into place, so the
/// "a valid name never holds partial contents" rule above still holds and a
/// crash leaves only a temporary file for the next open to wipe. A deferred
/// pack record is appended at once and fsynced by the syncer, so a crash can
/// tear the unsynced tail, which the open scan truncates. Dead-record
/// appends (`remove`) are deferred the same way.
///
/// This is safe because a `put_chunk`'s bytes are also in the Raft log, which
/// is durable before the entry commits: a chunk lost to a crash before its
/// fsync is written again when the log is replayed. The one thing that would
/// break that is dropping the log entries first, so `wait_durable()` blocks
/// until everything deferred so far is on disk, and the state machine calls
/// it before every snapshot (after which Raft compacts the log) and split.
/// An fsync failure on the syncer is sticky: every later `put` and
/// `wait_durable` throws it, so `apply()` stops the node (Requirement 2.4).
///
/// **Thread safety.** Every member may be called from any thread. The pack
/// index, the open pack, the staged chunks and the counters are guarded by
/// one mutex. Loose chunk writes and the syncer's fsyncs do their I/O outside
/// it; the syncer takes it only to link a synced chunk into place. Compaction
/// holds the mutex while it copies and fsyncs, which stalls small-chunk
/// `put`s for the length of one pack's copy (at most the pack size, 64 MiB by
/// default).

#include <raft/crc32.hpp>
#include <raft/fault_injection.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace kythira {

/// A chunk's name: the SHA-256 of its bytes.
using chunk_sha = std::array<std::byte, 32>;

struct chunk_sha_hash {
    [[nodiscard]] auto operator()(const chunk_sha& sha) const noexcept -> std::size_t {
        // The name is already a uniform hash; any eight of its bytes are as
        // good a bucket index as a fresh hash of all thirty-two.
        std::size_t h = 0;
        std::memcpy(&h, sha.data(), sizeof(h));
        return h;
    }
};

/// Thrown for an I/O failure inside the store. The state machine treats it as
/// fatal (Requirement 2.4): skipping the entry would let the replica diverge.
class cache_chunk_store_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// SHA-256 of `bytes`, through OpenSSL EVP (already a dependency of the tree).
[[nodiscard]] inline auto sha256_of(std::span<const std::byte> bytes) -> chunk_sha {
    chunk_sha out{};
    unsigned int len = 0;
    if (::EVP_Digest(bytes.data(), bytes.size(), reinterpret_cast<unsigned char*>(out.data()), &len,
                     ::EVP_sha256(), nullptr) != 1 ||
        len != out.size()) {
        throw cache_chunk_store_error("cache_chunk_store: EVP_Digest(sha256) failed");
    }
    return out;
}

/// Lower-case hex, 64 characters.
[[nodiscard]] inline auto to_hex(const chunk_sha& sha) -> std::string {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(sha.size() * 2);
    for (const std::byte b : sha) {
        const auto v = static_cast<unsigned>(b);
        out.push_back(digits[v >> 4U]);
        out.push_back(digits[v & 0xFU]);
    }
    return out;
}

/// Parse 64 hex characters (either case). Anything else is `nullopt`.
[[nodiscard]] inline auto chunk_sha_from_hex(std::string_view hex) -> std::optional<chunk_sha> {
    if (hex.size() != 64) {
        return std::nullopt;
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    chunk_sha out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = nibble(hex[2 * i]);
        const int lo = nibble(hex[(2 * i) + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out[i] = static_cast<std::byte>((hi << 4) | lo);
    }
    return out;
}

/// Whether a `put` returns only once the chunk is on disk (`now`), or once
/// its bytes are written, leaving the fsync to the store's syncer thread
/// (`deferred`; see the file comment).
enum class chunk_durability : std::uint8_t {
    now,
    deferred
};

/// What `statvfs` says is left for an unprivileged writer.
struct filesystem_space {
    std::uint64_t _free_bytes = 0;
    std::uint64_t _free_inodes = 0;
};

/// Free bytes and free inodes from one `statvfs` call, so the write gate
/// (Requirement 6.3) checks both from the same instant. `f_bavail` and
/// `f_favail` rather than `f_bfree` / `f_ffree`: the root reserve is not
/// space this non-root daemon can use.
///
/// Two fault points let a chaos test drive either floor on a runtime whose
/// tmpfs ignores `nr_inodes` (tasks.md task 10):
/// `raft/cache/statvfs_low_bytes` and `raft/cache/statvfs_low_inodes` report
/// zero for that field and the real value for the other.
[[nodiscard]] inline auto probe_filesystem_space(const std::filesystem::path& path)
    -> filesystem_space {
    struct ::statvfs st{};
    if (::statvfs(path.c_str(), &st) != 0) {
        throw cache_chunk_store_error("cache_chunk_store: statvfs(" + path.string() +
                                      ") failed: " + std::strerror(errno));
    }
    filesystem_space out{
        ._free_bytes = static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize,
        ._free_inodes = static_cast<std::uint64_t>(st.f_favail),
    };
    fiu_do_on("raft/cache/statvfs_low_bytes", out._free_bytes = 0;);
    fiu_do_on("raft/cache/statvfs_low_inodes", out._free_inodes = 0;);
    return out;
}

struct cache_chunk_store_options {
    /// The node's data directory. `chunks/` and `packs/` are created under it.
    std::filesystem::path _data_dir;
    /// Chunks smaller than this go to pack files
    /// (`KYTHIRA_CACHE_PACK_THRESHOLD_BYTES`).
    std::size_t _pack_threshold_bytes = std::size_t{64} * 1024;
    /// A pack stops taking appends at this size.
    std::size_t _pack_max_bytes = std::size_t{64} * 1024 * 1024;
    /// The largest chunk the store accepts. The configured chunk size is at
    /// most 16 MiB (Requirement 3.1); this bound is also what the open scan
    /// uses to recognise a record length that can only be garbage.
    std::size_t _max_chunk_bytes = std::size_t{16} * 1024 * 1024;
    /// Called with the path before every fsync the store issues, on
    /// whichever thread issues it. A test seam: a hook that sleeps stands in
    /// for a disk whose fsync stalls (cache_fsync_stall_test). Empty in
    /// production.
    std::function<void(const std::filesystem::path&)> _before_fsync;
};

/// Counters for `INFO` and `/_metrics` (Requirement 6.5) and the disk budget.
struct cache_chunk_store_stats {
    std::uint64_t _loose_files = 0;
    std::uint64_t _loose_bytes = 0;
    std::uint64_t _packed_chunks = 0;
    std::uint64_t _pack_files = 0;
    /// File sizes of every pack, dead records included: what the disk holds.
    std::uint64_t _pack_bytes = 0;
    std::uint64_t _pack_dead_bytes = 0;
    /// Reads whose bytes did not hash to their name (Requirement 10.1).
    std::uint64_t _hash_failures = 0;
    std::uint64_t _compactions = 0;

    [[nodiscard]] auto bytes_on_disk() const noexcept -> std::uint64_t {
        return _loose_bytes + _pack_bytes;
    }
    /// Chunk files plus pack files plus their sidecars (at most one each).
    [[nodiscard]] auto files() const noexcept -> std::uint64_t {
        return _loose_files + (2 * _pack_files);
    }
};

class cache_chunk_store {
public:
    /// Record header (sha + length) and trailer (crc).
    static constexpr std::size_t pack_header_bytes = 32 + 4;
    static constexpr std::size_t pack_record_overhead = pack_header_bytes + 4;

    /// Opens (creating if needed) the store under `options._data_dir`: wipes
    /// `chunks/tmp/`, deletes sidecars whose pack is gone, scans every pack
    /// (truncating a torn tail) and counts the loose chunks.
    explicit cache_chunk_store(cache_chunk_store_options options) : _options(std::move(options)) {
        if (_options._pack_threshold_bytes > _options._max_chunk_bytes) {
            throw std::invalid_argument(
                "cache_chunk_store: pack threshold exceeds the largest chunk");
        }
        _chunks_dir = _options._data_dir / "chunks";
        _tmp_dir = _chunks_dir / "tmp";
        _packs_dir = _options._data_dir / "packs";
        std::filesystem::create_directories(_tmp_dir);
        std::filesystem::create_directories(_packs_dir);
        wipe_tmp();
        open_packs();
        count_loose();
        _syncer = std::jthread([this](const std::stop_token& stop) { sync_loop(stop); });
    }

    cache_chunk_store(const cache_chunk_store&) = delete;
    auto operator=(const cache_chunk_store&) -> cache_chunk_store& = delete;
    cache_chunk_store(cache_chunk_store&&) = delete;
    auto operator=(cache_chunk_store&&) -> cache_chunk_store& = delete;

    /// Finishes every deferred write first, so a clean shutdown leaves
    /// nothing for the log replay to redo.
    ~cache_chunk_store() {
        _syncer.request_stop();
        if (_syncer.joinable()) {
            _syncer.join();
        }
        if (_open_fd >= 0) {
            ::close(_open_fd);
        }
    }

    [[nodiscard]] auto options() const noexcept -> const cache_chunk_store_options& {
        return _options;
    }
    [[nodiscard]] auto data_dir() const noexcept -> const std::filesystem::path& {
        return _options._data_dir;
    }

    /// Store `bytes` under `sha`, durably before returning unless `durability`
    /// is `deferred` (see the file comment). Returns `true` if
    /// this call created the chunk and `false` if it was already present (a
    /// replayed `put_chunk`). Throws `std::invalid_argument` if `bytes` does
    /// not hash to `sha` or exceeds the largest chunk, and
    /// `cache_chunk_store_error` on an I/O failure.
    ///
    /// The store's one invariant is that a name is its content's hash.
    /// Checking here costs one SHA-256 against an fsync, and turns a caller
    /// bug into an exception instead of a chunk that fails every later read.
    auto put(const chunk_sha& sha, std::span<const std::byte> bytes,
             chunk_durability durability = chunk_durability::now) -> bool {
        if (sha256_of(bytes) != sha) {
            throw std::invalid_argument("cache_chunk_store: bytes do not hash to " + to_hex(sha));
        }
        return put_verified(sha, bytes, durability);
    }

    /// `put` for a caller that has already hashed `bytes` and found `sha`
    /// (the state machine checks every `put_chunk` before storing it, so a
    /// second SHA-256 of a 4 MiB chunk here would be pure cost).
    auto put_verified(const chunk_sha& sha, std::span<const std::byte> bytes,
                      chunk_durability durability = chunk_durability::now) -> bool {
        if (bytes.size() > _options._max_chunk_bytes) {
            throw std::invalid_argument("cache_chunk_store: chunk of " +
                                        std::to_string(bytes.size()) +
                                        " bytes exceeds the largest chunk");
        }
        if (is_packed_size(bytes.size())) {
            return put_packed(sha, bytes, durability);
        }
        {
            const std::lock_guard lock(_mu);
            rethrow_sync_error_locked();
        }
        return durability == chunk_durability::now ? put_loose(sha, bytes)
                                                   : stage_loose(sha, bytes);
    }

    /// Block until every deferred write made before this call is on disk,
    /// names included. Throws the syncer's first fsync failure, if any. The
    /// state machine calls it before a snapshot or split, after which the
    /// log entries that carried the bytes may be gone.
    void wait_durable() {
        std::unique_lock lock(_mu);
        const std::uint64_t target = _sync_queued;
        _sync_cv.notify_all();
        _synced_cv.wait(lock, [&] { return _sync_done >= target || _sync_error; });
        rethrow_sync_error_locked();
    }

    /// Whether a chunk of this name is stored. It says nothing about whether
    /// its bytes are intact; `verify` and `read` check that.
    [[nodiscard]] auto contains(const chunk_sha& sha) const -> bool {
        {
            const std::lock_guard lock(_mu);
            if (_packed.contains(sha) || _staged.contains(sha)) {
                return true;
            }
        }
        std::error_code ec;
        return std::filesystem::exists(loose_path(sha), ec);
    }

    /// The chunk's size on disk, or `nullopt` if it is not stored.
    [[nodiscard]] auto chunk_size(const chunk_sha& sha) const -> std::optional<std::uint64_t> {
        {
            const std::lock_guard lock(_mu);
            if (auto it = _packed.find(sha); it != _packed.end()) {
                return it->second._len;
            }
            if (auto it = _staged.find(sha); it != _staged.end()) {
                return it->second._size;
            }
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(loose_path(sha), ec);
        if (ec) {
            return std::nullopt;
        }
        return size;
    }

    /// The chunk's bytes, hash-checked. `nullopt` if it is absent or its
    /// bytes do not hash to its name (counted in `_hash_failures`).
    [[nodiscard]] auto read(const chunk_sha& sha) const -> std::optional<std::vector<std::byte>> {
        auto bytes = read_unverified(sha);
        if (!bytes) {
            return std::nullopt;
        }
        if (sha256_of(*bytes) != sha) {
            const std::lock_guard lock(_mu);
            ++_stats._hash_failures;
            return std::nullopt;
        }
        return bytes;
    }

    /// Stream the chunk through SHA-256 without holding all of it. `false` if
    /// it is absent or corrupt.
    [[nodiscard]] auto verify(const chunk_sha& sha) const -> bool {
        return verify_counting(sha, true);
    }

    /// Drop the stored copy of a chunk whose bytes no longer hash to its
    /// name, so that a repair fetch can store a good one (design.md "Hash
    /// mismatch on read": drop the chunk and queue a repair). Until it is
    /// dropped the chunk counts as present: the fetcher skips it as already
    /// stored and `put` keeps the bad file. A good or absent chunk is left
    /// alone, so a reader that saw the bad copy after a repair replaced it
    /// cannot remove the good one. Not counted as another hash failure; the
    /// read that found it already was. Returns whether a copy was dropped.
    auto drop_if_corrupt(const chunk_sha& sha) -> bool {
        if (!contains(sha) || verify_counting(sha, false)) {
            return false;
        }
        return remove(sha);
    }

private:
    [[nodiscard]] auto verify_counting(const chunk_sha& sha, bool count) const -> bool {
        std::optional<pack_location> where;
        std::optional<std::filesystem::path> staged;
        {
            const std::lock_guard lock(_mu);
            if (auto it = _packed.find(sha); it != _packed.end()) {
                where = it->second;
            } else if (auto st = _staged.find(sha); st != _staged.end()) {
                staged = st->second._tmp;
            }
        }
        std::unique_ptr<::EVP_MD_CTX, decltype(&::EVP_MD_CTX_free)> ctx(::EVP_MD_CTX_new(),
                                                                        &::EVP_MD_CTX_free);
        if (!ctx || ::EVP_DigestInit_ex(ctx.get(), ::EVP_sha256(), nullptr) != 1) {
            throw cache_chunk_store_error("cache_chunk_store: EVP_DigestInit_ex failed");
        }
        const int fd = where ? ::open(pack_path(where->_pack).c_str(), O_RDONLY | O_CLOEXEC)
                             : open_loose(sha, staged);
        if (fd < 0) {
            return false;
        }
        std::uint64_t offset = where ? where->_offset + pack_header_bytes : 0;
        std::uint64_t remaining = where ? where->_len : std::numeric_limits<std::uint64_t>::max();
        std::vector<std::byte> buf(std::size_t{64} * 1024);
        bool ok = true;
        while (remaining > 0) {
            const auto want =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buf.size()));
            const auto n = ::pread(fd, buf.data(), want, static_cast<off_t>(offset));
            if (n < 0) {
                ok = false;
                break;
            }
            if (n == 0) {
                // A loose file ends here; a packed record that ends early is
                // short, which the hash below cannot match anyway.
                ok = !where;
                break;
            }
            if (::EVP_DigestUpdate(ctx.get(), buf.data(), static_cast<std::size_t>(n)) != 1) {
                ok = false;
                break;
            }
            offset += static_cast<std::uint64_t>(n);
            if (where) {
                remaining -= static_cast<std::uint64_t>(n);
            }
        }
        ::close(fd);
        chunk_sha got{};
        unsigned int len = 0;
        if (::EVP_DigestFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(got.data()), &len) !=
            1) {
            throw cache_chunk_store_error("cache_chunk_store: EVP_DigestFinal_ex failed");
        }
        if (ok && got == sha) {
            return true;
        }
        if (count) {
            const std::lock_guard lock(_mu);
            ++_stats._hash_failures;
        }
        return false;
    }

public:
    /// Remove the chunk. Returns `false` if it was not stored. Only the state
    /// machine's `apply` (through `chunk_refcounts`) and start-up orphan
    /// collection call this (design Component 1).
    auto remove(const chunk_sha& sha) -> bool {
        {
            const std::lock_guard lock(_mu);
            if (auto it = _staged.find(sha); it != _staged.end()) {
                // Not linked yet: dropping the temporary file is the whole
                // removal, and the syncer skips a chunk no longer staged.
                ::unlink(it->second._tmp.c_str());
                --_stats._loose_files;
                _stats._loose_bytes -= it->second._size;
                _staged.erase(it);
                return true;
            }
            if (auto it = _packed.find(sha); it != _packed.end()) {
                const pack_location where = it->second;
                append_dead_locked(where._pack, where._offset);
                auto& info = _packs.at(where._pack);
                info._dead_bytes += record_size(where._len);
                _stats._pack_dead_bytes += record_size(where._len);
                --_stats._packed_chunks;
                _packed.erase(it);
                return true;
            }
        }
        const auto path = loose_path(sha);
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            return false;
        }
        if (::unlink(path.c_str()) != 0) {
            if (errno == ENOENT) {
                return false;
            }
            throw cache_chunk_store_error("cache_chunk_store: unlink(" + path.string() +
                                          ") failed: " + std::strerror(errno));
        }
        const std::lock_guard lock(_mu);
        --_stats._loose_files;
        _stats._loose_bytes -= size;
        return true;
    }

    /// Call `fn(sha, size)` for every stored chunk, packed first. Used by
    /// start-up orphan collection (Requirement 2.6). The packed set is copied
    /// before the walk, so `fn` may call `remove`.
    void for_each_chunk(const std::function<void(const chunk_sha&, std::uint64_t)>& fn) const {
        std::vector<std::pair<chunk_sha, std::uint64_t>> packed;
        std::unordered_map<chunk_sha, std::uint64_t, chunk_sha_hash> staged;
        {
            const std::lock_guard lock(_mu);
            packed.reserve(_packed.size());
            for (const auto& [sha, where] : _packed) {
                packed.emplace_back(sha, where._len);
            }
            for (const auto& [sha, st] : _staged) {
                staged.emplace(sha, st._size);
            }
        }
        for (const auto& [sha, size] : packed) {
            fn(sha, size);
        }
        // Staged chunks the syncer links during the walk below are skipped
        // there, so each is reported once.
        for (const auto& [sha, size] : staged) {
            fn(sha, size);
        }
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(_chunks_dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory() && it->path() == _tmp_dir) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file()) {
                continue;
            }
            if (auto sha = chunk_sha_from_hex(it->path().filename().string());
                sha && !staged.contains(*sha)) {
                fn(*sha, it->file_size());
            }
        }
    }

    /// Compact every closed pack whose live bytes are below half its size
    /// (Requirement 6.8). Returns the number of packs compacted. Meant for a
    /// background tick; it is safe to call at any time.
    auto compact() -> std::size_t {
        const std::lock_guard lock(_mu);
        std::vector<std::uint64_t> victims;
        for (const auto& [n, info] : _packs) {
            if (n == _open_pack) {
                continue;
            }
            const std::uint64_t live = info._size - info._dead_bytes;
            if (info._size > 0 && live * 2 < info._size) {
                victims.push_back(n);
            }
        }
        for (const std::uint64_t n : victims) {
            compact_one_locked(n);
        }
        return victims.size();
    }

    [[nodiscard]] auto stats() const -> cache_chunk_store_stats {
        const std::lock_guard lock(_mu);
        return _stats;
    }

    /// Free bytes and inodes on the store's filesystem.
    [[nodiscard]] auto space() const -> filesystem_space {
        return probe_filesystem_space(_options._data_dir);
    }

    [[nodiscard]] auto loose_path(const chunk_sha& sha) const -> std::filesystem::path {
        const auto hex = to_hex(sha);
        return _chunks_dir / hex.substr(0, 2) / hex.substr(2, 2) / hex;
    }

    [[nodiscard]] auto pack_path(std::uint64_t n) const -> std::filesystem::path {
        return _packs_dir / ("pack." + std::to_string(n));
    }

    [[nodiscard]] auto dead_path(std::uint64_t n) const -> std::filesystem::path {
        return _packs_dir / ("pack." + std::to_string(n) + ".dead");
    }

    /// Whether a chunk of this size is packed rather than stored loose.
    [[nodiscard]] auto is_packed_size(std::size_t size) const noexcept -> bool {
        return size < _options._pack_threshold_bytes;
    }

private:
    struct pack_location {
        std::uint64_t _pack = 0;
        /// Offset of the record's first byte (its sha).
        std::uint64_t _offset = 0;
        std::uint32_t _len = 0;
    };

    struct pack_info {
        std::uint64_t _size = 0;
        std::uint64_t _dead_bytes = 0;
    };

    [[nodiscard]] static constexpr auto record_size(std::uint64_t len) noexcept -> std::uint64_t {
        return len + pack_record_overhead;
    }

    [[nodiscard]] static auto errno_message(const std::string& what) -> std::string {
        return "cache_chunk_store: " + what + ": " + std::strerror(errno);
    }

    static void put_u32(std::byte* p, std::uint32_t v) noexcept {
        p[0] = static_cast<std::byte>(v >> 24U);
        p[1] = static_cast<std::byte>(v >> 16U);
        p[2] = static_cast<std::byte>(v >> 8U);
        p[3] = static_cast<std::byte>(v);
    }
    [[nodiscard]] static auto get_u32(const std::byte* p) noexcept -> std::uint32_t {
        return (static_cast<std::uint32_t>(p[0]) << 24U) |
               (static_cast<std::uint32_t>(p[1]) << 16U) |
               (static_cast<std::uint32_t>(p[2]) << 8U) | static_cast<std::uint32_t>(p[3]);
    }
    static void put_u64(std::byte* p, std::uint64_t v) noexcept {
        put_u32(p, static_cast<std::uint32_t>(v >> 32U));
        put_u32(p + 4, static_cast<std::uint32_t>(v));
    }
    [[nodiscard]] static auto get_u64(const std::byte* p) noexcept -> std::uint64_t {
        return (static_cast<std::uint64_t>(get_u32(p)) << 32U) | get_u32(p + 4);
    }

    /// Write all of `bytes` at `offset`, or throw.
    static void pwrite_all(int fd, std::span<const std::byte> bytes, std::uint64_t offset,
                           const std::filesystem::path& path) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto n = ::pwrite(fd, bytes.data() + done, bytes.size() - done,
                                    static_cast<off_t>(offset + done));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw cache_chunk_store_error(errno_message("write to " + path.string()));
            }
            done += static_cast<std::size_t>(n);
        }
    }

    /// Read exactly `out.size()` bytes at `offset`. `false` on a short read.
    [[nodiscard]] static auto pread_all(int fd, std::span<std::byte> out, std::uint64_t offset)
        -> bool {
        std::size_t done = 0;
        while (done < out.size()) {
            const auto n = ::pread(fd, out.data() + done, out.size() - done,
                                   static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                return false;
            }
            done += static_cast<std::size_t>(n);
        }
        return true;
    }

    void fsync_fd(int fd, const std::filesystem::path& path) const {
        if (_options._before_fsync) {
            _options._before_fsync(path);
        }
        if (::fsync(fd) != 0) {
            throw cache_chunk_store_error(errno_message("fsync(" + path.string() + ")"));
        }
    }

    /// fsync a directory so a name created or removed in it is durable. A
    /// directory that cannot be opened is not fatal on every filesystem, so
    /// only a failing fsync is.
    void fsync_dir(const std::filesystem::path& dir) const {
        if (_options._before_fsync) {
            _options._before_fsync(dir);
        }
        const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            return;
        }
        const int rc = ::fsync(fd);
        const int err = errno;
        ::close(fd);
        if (rc != 0 && err != EINVAL) {
            errno = err;
            throw cache_chunk_store_error(errno_message("fsync(" + dir.string() + ")"));
        }
    }

    /// fsync the file at `path` through a fresh descriptor (the syncer does
    /// not share the writers' descriptors). `false` if it no longer exists:
    /// removed, or compacted away, since it was queued.
    auto fsync_path(const std::filesystem::path& path) const -> bool {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT) {
                return false;
            }
            throw cache_chunk_store_error(errno_message("open(" + path.string() + ")"));
        }
        try {
            fsync_fd(fd, path);
        } catch (...) {
            ::close(fd);
            throw;
        }
        ::close(fd);
        return true;
    }

    /// Create `dir` (one level) if absent and make its name durable.
    void ensure_dir(const std::filesystem::path& dir) const {
        if (::mkdir(dir.c_str(), 0750) == 0) {
            fsync_dir(dir.parent_path());
            return;
        }
        if (errno != EEXIST) {
            throw cache_chunk_store_error(errno_message("mkdir(" + dir.string() + ")"));
        }
    }

    // ---- open -------------------------------------------------------------

    void wipe_tmp() {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(_tmp_dir, ec)) {
            std::filesystem::remove_all(entry.path(), ec);
        }
    }

    void open_packs() {
        std::set<std::uint64_t> packs;
        std::set<std::uint64_t> sidecars;
        for (const auto& entry : std::filesystem::directory_iterator(_packs_dir)) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with("pack.")) {
                continue;
            }
            std::string_view rest(name);
            rest.remove_prefix(5);
            const bool dead = rest.ends_with(".dead");
            if (dead) {
                rest.remove_suffix(5);
            }
            std::uint64_t n = 0;
            const auto [end, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), n);
            if (ec != std::errc{} || end != rest.data() + rest.size()) {
                continue;
            }
            (dead ? sidecars : packs).insert(n);
        }
        // A sidecar outlives its pack only if compaction crashed between the
        // two unlinks. Delete it before any pack of that number can exist
        // again: a fresh pack must not inherit someone else's dead offsets.
        for (const std::uint64_t n : sidecars) {
            if (!packs.contains(n)) {
                std::filesystem::remove(dead_path(n));
            }
        }
        for (const std::uint64_t n : packs) {
            scan_pack(n);
        }
        if (!packs.empty()) {
            _next_pack = *packs.rbegin() + 1;
            const std::uint64_t last = *packs.rbegin();
            if (_packs.at(last)._size < _options._pack_max_bytes) {
                open_for_append(last);
            }
        }
    }

    [[nodiscard]] auto load_dead_offsets(std::uint64_t n) -> std::set<std::uint64_t> {
        std::set<std::uint64_t> dead;
        const auto path = dead_path(n);
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            return dead;
        }
        struct ::stat st{};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            throw cache_chunk_store_error(errno_message("fstat(" + path.string() + ")"));
        }
        const auto whole = static_cast<std::uint64_t>(st.st_size) / 8 * 8;
        std::vector<std::byte> buf(whole);
        if (!pread_all(fd, buf, 0)) {
            ::close(fd);
            throw cache_chunk_store_error("cache_chunk_store: short read of " + path.string());
        }
        // A torn final offset is dropped. The record it would have marked is
        // then live again, which only costs disk: the state machine collects
        // unreferenced chunks at start (Requirement 2.6).
        if (whole != static_cast<std::uint64_t>(st.st_size)) {
            if (::ftruncate(fd, static_cast<off_t>(whole)) != 0) {
                ::close(fd);
                throw cache_chunk_store_error(errno_message("ftruncate(" + path.string() + ")"));
            }
        }
        ::close(fd);
        for (std::uint64_t off = 0; off < whole; off += 8) {
            dead.insert(get_u64(buf.data() + off));
        }
        return dead;
    }

    void scan_pack(std::uint64_t n) {
        const auto path = pack_path(n);
        const auto dead = load_dead_offsets(n);
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            throw cache_chunk_store_error(errno_message("open(" + path.string() + ")"));
        }
        struct ::stat st{};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            throw cache_chunk_store_error(errno_message("fstat(" + path.string() + ")"));
        }
        const auto size = static_cast<std::uint64_t>(st.st_size);
        pack_info info;
        std::vector<std::uint64_t> duplicates;
        std::vector<std::byte> record;
        std::uint64_t off = 0;
        while (off + pack_record_overhead <= size) {
            std::array<std::byte, pack_header_bytes> header{};
            if (!pread_all(fd, header, off)) {
                break;
            }
            const std::uint32_t len = get_u32(header.data() + 32);
            if (len > _options._max_chunk_bytes || off + record_size(len) > size) {
                break;
            }
            record.resize(record_size(len));
            if (!pread_all(fd, record, off)) {
                break;
            }
            const std::uint32_t stored_crc = get_u32(record.data() + pack_header_bytes + len);
            const std::uint32_t crc =
                crc32(std::span<const std::byte>(record.data(), pack_header_bytes + len));
            if (crc != stored_crc) {
                break;
            }
            chunk_sha sha{};
            std::memcpy(sha.data(), record.data(), sha.size());
            if (dead.contains(off)) {
                info._dead_bytes += record_size(len);
            } else if (_packed.contains(sha)) {
                // A second live copy: compaction crashed after copying and
                // before unlinking. The first (lower-numbered pack, earlier
                // offset) wins. Recording this one as dead on disk keeps a
                // later remove of the winner from resurrecting the chunk.
                duplicates.push_back(off);
                info._dead_bytes += record_size(len);
            } else {
                _packed.emplace(sha, pack_location{._pack = n, ._offset = off, ._len = len});
                ++_stats._packed_chunks;
            }
            off += record_size(len);
        }
        if (off != size) {
            // Torn tail: everything from the first bad record on goes.
            if (::ftruncate(fd, static_cast<off_t>(off)) != 0 || ::fsync(fd) != 0) {
                ::close(fd);
                throw cache_chunk_store_error(errno_message("truncate(" + path.string() + ")"));
            }
        }
        ::close(fd);
        info._size = off;
        _packs.emplace(n, info);
        _stats._pack_files += 1;
        _stats._pack_bytes += info._size;
        _stats._pack_dead_bytes += info._dead_bytes;
        for (const std::uint64_t dup : duplicates) {
            append_dead_locked(n, dup);
        }
    }

    void count_loose() {
        for_each_chunk_loose([this](std::uint64_t size) {
            ++_stats._loose_files;
            _stats._loose_bytes += size;
        });
    }

    template<typename F> void for_each_chunk_loose(F&& fn) const {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(_chunks_dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory() && it->path() == _tmp_dir) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file() && chunk_sha_from_hex(it->path().filename().string())) {
                fn(it->file_size());
            }
        }
    }

    // ---- loose chunks -----------------------------------------------------

    auto put_loose(const chunk_sha& sha, std::span<const std::byte> bytes) -> bool {
        const auto final_path = loose_path(sha);
        {
            const std::lock_guard lock(_mu);
            if (_staged.contains(sha)) {
                return false;
            }
        }
        std::error_code ec;
        if (std::filesystem::exists(final_path, ec)) {
            return false;
        }
        const auto tmp_path = _tmp_dir / unique_tmp_name();
        const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
        if (fd < 0) {
            throw cache_chunk_store_error(errno_message("create " + tmp_path.string()));
        }
        try {
            fiu_do_on("raft/cache/chunk_store/write",
                      throw cache_chunk_store_error("cache_chunk_store: injected write failure"););
            pwrite_all(fd, bytes, 0, tmp_path);
            fsync_fd(fd, tmp_path);
        } catch (...) {
            ::close(fd);
            ::unlink(tmp_path.c_str());
            throw;
        }
        ::close(fd);
        // A crash here leaves only the temporary name, which the next open
        // wipes. The fault point stands in for that crash: it throws without
        // cleaning up, exactly as a dead process would leave the directory.
        fiu_do_on(
            "raft/cache/chunk_store/before_rename",
            throw cache_chunk_store_error("cache_chunk_store: injected crash before rename"););
        ensure_dir(final_path.parent_path().parent_path());
        ensure_dir(final_path.parent_path());
        const bool created = ::link(tmp_path.c_str(), final_path.c_str()) == 0;
        const int link_errno = errno;
        ::unlink(tmp_path.c_str());
        if (!created) {
            if (link_errno == EEXIST) {
                return false;
            }
            errno = link_errno;
            throw cache_chunk_store_error(errno_message("link(" + final_path.string() + ")"));
        }
        fsync_dir(final_path.parent_path());
        const std::lock_guard lock(_mu);
        ++_stats._loose_files;
        _stats._loose_bytes += bytes.size();
        return true;
    }

    /// `put_loose` without the fsync: the bytes go to a temporary file that
    /// `_staged` names, and the syncer fsyncs and links it (file comment,
    /// "Deferred durability").
    auto stage_loose(const chunk_sha& sha, std::span<const std::byte> bytes) -> bool {
        const auto final_path = loose_path(sha);
        {
            const std::lock_guard lock(_mu);
            if (_staged.contains(sha)) {
                return false;
            }
        }
        std::error_code ec;
        if (std::filesystem::exists(final_path, ec)) {
            return false;
        }
        const auto tmp_path = _tmp_dir / unique_tmp_name();
        const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
        if (fd < 0) {
            throw cache_chunk_store_error(errno_message("create " + tmp_path.string()));
        }
        try {
            fiu_do_on("raft/cache/chunk_store/write",
                      throw cache_chunk_store_error("cache_chunk_store: injected write failure"););
            pwrite_all(fd, bytes, 0, tmp_path);
        } catch (...) {
            ::close(fd);
            ::unlink(tmp_path.c_str());
            throw;
        }
        ::close(fd);
        const std::lock_guard lock(_mu);
        if (_staged.contains(sha) || std::filesystem::exists(final_path, ec)) {
            // Another writer of the same bytes got there first.
            ::unlink(tmp_path.c_str());
            return false;
        }
        _staged.emplace(sha, staged_chunk{._tmp = tmp_path, ._size = bytes.size()});
        ++_stats._loose_files;
        _stats._loose_bytes += bytes.size();
        queue_sync_locked();
        return true;
    }

    /// Open a loose chunk for reading: its temporary file while it is staged,
    /// else its final name. A staged file the syncer links and unlinks after
    /// `staged` was read is found at its final name instead, since the link
    /// comes before the unlink.
    [[nodiscard]] auto open_loose(const chunk_sha& sha,
                                  const std::optional<std::filesystem::path>& staged) const -> int {
        if (staged) {
            const int fd = ::open(staged->c_str(), O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                return fd;
            }
        }
        return ::open(loose_path(sha).c_str(), O_RDONLY | O_CLOEXEC);
    }

    [[nodiscard]] auto unique_tmp_name() -> std::string {
        thread_local std::mt19937_64 rng{std::random_device{}()};
        return std::to_string(::getpid()) + "." + std::to_string(rng()) + "." +
               std::to_string(_tmp_counter.fetch_add(1, std::memory_order_relaxed));
    }

    [[nodiscard]] auto read_unverified(const chunk_sha& sha) const
        -> std::optional<std::vector<std::byte>> {
        std::optional<pack_location> where;
        std::optional<std::filesystem::path> staged;
        {
            const std::lock_guard lock(_mu);
            if (auto it = _packed.find(sha); it != _packed.end()) {
                where = it->second;
            } else if (auto st = _staged.find(sha); st != _staged.end()) {
                staged = st->second._tmp;
            }
        }
        if (where) {
            const auto path = pack_path(where->_pack);
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                return std::nullopt;
            }
            std::vector<std::byte> out(where->_len);
            const bool ok = pread_all(fd, out, where->_offset + pack_header_bytes);
            ::close(fd);
            return ok ? std::optional(std::move(out)) : std::nullopt;
        }
        const int fd = open_loose(sha, staged);
        if (fd < 0) {
            return std::nullopt;
        }
        struct ::stat st{};
        if (::fstat(fd, &st) != 0 ||
            static_cast<std::uint64_t>(st.st_size) > _options._max_chunk_bytes) {
            ::close(fd);
            return std::nullopt;
        }
        std::vector<std::byte> out(static_cast<std::size_t>(st.st_size));
        const bool ok = pread_all(fd, out, 0);
        ::close(fd);
        return ok ? std::optional(std::move(out)) : std::nullopt;
    }

    // ---- packs ------------------------------------------------------------

    auto put_packed(const chunk_sha& sha, std::span<const std::byte> bytes,
                    chunk_durability durability) -> bool {
        const std::lock_guard lock(_mu);
        rethrow_sync_error_locked();
        if (_packed.contains(sha)) {
            return false;
        }
        const pack_location where = append_record_locked(sha, bytes);
        if (durability == chunk_durability::now) {
            flush_dirty_packs_locked();
        }
        index_record_locked(sha, where);
        return true;
    }

    void open_for_append(std::uint64_t n) {
        const auto path = pack_path(n);
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0640);
        if (fd < 0) {
            throw cache_chunk_store_error(errno_message("open(" + path.string() + ")"));
        }
        _open_fd = fd;
        _open_pack = n;
    }

    /// Close the open pack so the next append starts a new one. Its unsynced
    /// appends stay in `_dirty_packs`, for the syncer or the next
    /// `flush_dirty_packs_locked()`.
    void close_open_pack_locked() {
        if (_open_fd < 0) {
            return;
        }
        ::close(_open_fd);
        _open_fd = -1;
        _open_pack = no_pack;
    }

    /// Append one record to the open pack, starting a new pack if needed, and
    /// mark the pack (and a new pack's directory entry) for the syncer. A
    /// caller that needs it durable now calls `flush_dirty_packs_locked()`;
    /// either way it then indexes the record.
    auto append_record_locked(const chunk_sha& sha, std::span<const std::byte> bytes)
        -> pack_location {
        if (_open_fd >= 0 && _packs.at(_open_pack)._size >= _options._pack_max_bytes) {
            close_open_pack_locked();
        }
        if (_open_fd < 0) {
            const std::uint64_t n = _next_pack++;
            open_for_append(n);
            _packs_dir_dirty = true;
            _packs.emplace(n, pack_info{});
            ++_stats._pack_files;
        }
        auto& info = _packs.at(_open_pack);
        const auto len = static_cast<std::uint32_t>(bytes.size());
        std::vector<std::byte> record(record_size(len));
        std::memcpy(record.data(), sha.data(), sha.size());
        put_u32(record.data() + 32, len);
        if (len > 0) {
            std::memcpy(record.data() + pack_header_bytes, bytes.data(), len);
        }
        put_u32(record.data() + pack_header_bytes + len,
                crc32(std::span<const std::byte>(record.data(), pack_header_bytes + len)));
        const std::uint64_t offset = info._size;
        try {
            fiu_do_on("raft/cache/chunk_store/write",
                      throw cache_chunk_store_error("cache_chunk_store: injected write failure"););
            pwrite_all(_open_fd, record, offset, pack_path(_open_pack));
        } catch (...) {
            // Leave no partial record behind for the next append to follow.
            // Best effort: if this fails too, the next open's torn-tail scan
            // truncates the same bytes.
            [[maybe_unused]] const int rc = ::ftruncate(_open_fd, static_cast<off_t>(offset));
            throw;
        }
        info._size += record.size();
        _stats._pack_bytes += record.size();
        _dirty_packs.insert(_open_pack);
        queue_sync_locked();
        return pack_location{._pack = _open_pack, ._offset = offset, ._len = len};
    }

    void index_record_locked(const chunk_sha& sha, const pack_location& where) {
        _packed.insert_or_assign(sha, where);
        ++_stats._packed_chunks;
    }

    /// fsync every pack with unsynced appends, and the pack directory if a
    /// pack was created, before returning: for `chunk_durability::now` and
    /// compaction. Holds the mutex throughout, as both callers already do.
    void flush_dirty_packs_locked() {
        for (const std::uint64_t n : _dirty_packs) {
            if (n == _open_pack && _open_fd >= 0) {
                fsync_fd(_open_fd, pack_path(n));
            } else {
                (void)fsync_path(pack_path(n));
            }
        }
        _dirty_packs.clear();
        if (_packs_dir_dirty) {
            fsync_dir(_packs_dir);
            _packs_dir_dirty = false;
        }
    }

    void append_dead_locked(std::uint64_t n, std::uint64_t offset) {
        const auto path = dead_path(n);
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
        if (fd < 0) {
            throw cache_chunk_store_error(errno_message("open(" + path.string() + ")"));
        }
        std::array<std::byte, 8> buf{};
        put_u64(buf.data(), offset);
        try {
            std::size_t done = 0;
            while (done < buf.size()) {
                const auto w = ::write(fd, buf.data() + done, buf.size() - done);
                if (w < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    throw cache_chunk_store_error(errno_message("write to " + path.string()));
                }
                done += static_cast<std::size_t>(w);
            }
        } catch (...) {
            ::close(fd);
            throw;
        }
        ::close(fd);
        // Deferred like a chunk write. Losing the offset to a crash only
        // brings the record back as a live chunk nothing references, which
        // the start-up orphan collection removes (Requirement 2.6); a `del`
        // still in the log removes it again on replay.
        _dirty_sidecars.insert(n);
        _packs_dir_dirty = true;
        queue_sync_locked();
    }

    void compact_one_locked(std::uint64_t n) {
        std::vector<std::pair<chunk_sha, pack_location>> live;
        for (const auto& [sha, where] : _packed) {
            if (where._pack == n) {
                live.emplace_back(sha, where);
            }
        }
        // Copy in offset order: sequential reads of the old pack.
        std::sort(live.begin(), live.end(),
                  [](const auto& a, const auto& b) { return a.second._offset < b.second._offset; });
        const auto old_path = pack_path(n);
        std::vector<std::pair<chunk_sha, pack_location>> moved;
        moved.reserve(live.size());
        if (!live.empty()) {
            const int fd = ::open(old_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                throw cache_chunk_store_error(errno_message("open(" + old_path.string() + ")"));
            }
            std::vector<std::byte> body;
            try {
                for (const auto& [sha, where] : live) {
                    body.resize(where._len);
                    if (!pread_all(fd, body, where._offset + pack_header_bytes)) {
                        throw cache_chunk_store_error("cache_chunk_store: short read of " +
                                                      old_path.string());
                    }
                    // A roll to a new pack inside this call leaves the
                    // previous one in _dirty_packs, so the flush below
                    // covers every pack the copies landed in.
                    moved.emplace_back(sha, append_record_locked(sha, body));
                }
                flush_dirty_packs_locked();
            } catch (...) {
                ::close(fd);
                throw;
            }
            ::close(fd);
        }
        // The copies are durable. Point the index at them, then let go of the
        // old pack: pack before sidecar (see the file comment for why).
        for (const auto& [sha, where] : moved) {
            _packed.insert_or_assign(sha, where);
        }
        fiu_do_on("raft/cache/chunk_store/compact_before_unlink",
                  throw cache_chunk_store_error(
                      "cache_chunk_store: injected crash before compaction unlink"););
        if (::unlink(old_path.c_str()) != 0) {
            throw cache_chunk_store_error(errno_message("unlink(" + old_path.string() + ")"));
        }
        fsync_dir(_packs_dir);
        ::unlink(dead_path(n).c_str());
        const auto& info = _packs.at(n);
        _stats._pack_bytes -= info._size;
        _stats._pack_dead_bytes -= info._dead_bytes;
        --_stats._pack_files;
        _packs.erase(n);
        ++_stats._compactions;
    }

    // ---- the syncer ---------------------------------------------------------

    /// One more batch of deferred work for the syncer. Under `_mu`.
    void queue_sync_locked() {
        ++_sync_queued;
        _sync_cv.notify_all();
    }

    void rethrow_sync_error_locked() const {
        if (_sync_error) {
            std::rethrow_exception(_sync_error);
        }
    }

    /// The syncer thread: take everything queued, fsync it outside the
    /// mutex, link synced chunks into place, and wake `wait_durable()`. On
    /// stop it drains what is queued first (see the destructor).
    void sync_loop(const std::stop_token& stop) {
        std::unique_lock lock(_mu);
        while (true) {
            _sync_cv.wait(lock, stop, [this] { return _sync_done < _sync_queued; });
            if (_sync_done >= _sync_queued) {
                return;  // stop requested, nothing left
            }
            const std::uint64_t target = _sync_queued;
            std::vector<std::pair<chunk_sha, std::filesystem::path>> staged;
            staged.reserve(_staged.size());
            for (const auto& [sha, st] : _staged) {
                staged.emplace_back(sha, st._tmp);
            }
            const auto packs = std::exchange(_dirty_packs, {});
            const auto sidecars = std::exchange(_dirty_sidecars, {});
            const bool packs_dir = std::exchange(_packs_dir_dirty, false);
            lock.unlock();
            std::exception_ptr error;
            try {
                sync_batch(staged, packs, sidecars, packs_dir);
            } catch (...) {
                error = std::current_exception();
            }
            lock.lock();
            if (error && !_sync_error) {
                _sync_error = error;
            }
            _sync_done = target;
            _synced_cv.notify_all();
        }
    }

    void sync_batch(const std::vector<std::pair<chunk_sha, std::filesystem::path>>& staged,
                    const std::set<std::uint64_t>& packs, const std::set<std::uint64_t>& sidecars,
                    bool packs_dir) {
        for (const std::uint64_t n : packs) {
            (void)fsync_path(pack_path(n));
        }
        for (const std::uint64_t n : sidecars) {
            (void)fsync_path(dead_path(n));
        }
        if (packs_dir) {
            fsync_dir(_packs_dir);
        }
        std::set<std::filesystem::path> dirs;
        for (const auto& [sha, tmp] : staged) {
            if (!fsync_path(tmp)) {
                continue;  // removed since it was staged
            }
            const auto final_path = loose_path(sha);
            ensure_dir(final_path.parent_path().parent_path());
            ensure_dir(final_path.parent_path());
            const std::lock_guard lock(_mu);
            auto it = _staged.find(sha);
            if (it == _staged.end() || it->second._tmp != tmp) {
                continue;  // removed, and perhaps staged again, meanwhile
            }
            if (::link(tmp.c_str(), final_path.c_str()) != 0) {
                if (errno != EEXIST) {
                    // Left staged and still readable; the error is sticky.
                    throw cache_chunk_store_error(
                        errno_message("link(" + final_path.string() + ")"));
                }
                // A `now` put of the same bytes linked it first and counted
                // it too.
                --_stats._loose_files;
                _stats._loose_bytes -= it->second._size;
            }
            ::unlink(tmp.c_str());
            _staged.erase(it);
            dirs.insert(final_path.parent_path());
        }
        for (const auto& dir : dirs) {
            fsync_dir(dir);
        }
    }

    static constexpr std::uint64_t no_pack = std::numeric_limits<std::uint64_t>::max();

    cache_chunk_store_options _options;
    std::filesystem::path _chunks_dir;
    std::filesystem::path _tmp_dir;
    std::filesystem::path _packs_dir;

    mutable std::mutex _mu;
    std::unordered_map<chunk_sha, pack_location, chunk_sha_hash> _packed;
    std::map<std::uint64_t, pack_info> _packs;
    std::uint64_t _open_pack = no_pack;
    int _open_fd = -1;
    std::uint64_t _next_pack = 1;
    mutable cache_chunk_store_stats _stats;
    std::atomic<std::uint64_t> _tmp_counter{0};

    /// A large chunk written with `chunk_durability::deferred`, still at its
    /// temporary name until the syncer has fsynced and linked it.
    struct staged_chunk {
        std::filesystem::path _tmp;
        std::uint64_t _size = 0;
    };
    std::unordered_map<chunk_sha, staged_chunk, chunk_sha_hash> _staged;
    /// Packs and sidecars with unsynced appends, and whether the pack
    /// directory has an unsynced entry.
    std::set<std::uint64_t> _dirty_packs;
    std::set<std::uint64_t> _dirty_sidecars;
    bool _packs_dir_dirty = false;
    /// Batches queued and synced: `wait_durable()` waits for done to reach
    /// what was queued when it was called.
    std::uint64_t _sync_queued = 0;
    std::uint64_t _sync_done = 0;
    std::exception_ptr _sync_error;
    std::condition_variable_any _sync_cv;
    std::condition_variable _synced_cv;
    /// Last, so it starts after, and is joined (in the destructor) before,
    /// everything it uses.
    std::jthread _syncer;
};

/// Node-wide reference counts for the chunks in one store (design Component
/// 1, "Shared across shards").
///
/// Two replicas on one node may reference the same chunk, which is likely
/// after a split, so a chunk is removed only when the count summed over every
/// replica on the node reaches zero. Each replica's state machine updates the
/// counts from `apply()`; the counts are not persisted, and are rebuilt on
/// start by summing the restored replicas.
///
/// Every operation that can create or delete a chunk file runs under one
/// mutex, so a `release` on one shard cannot remove a chunk between another
/// shard's `put` of the same bytes and its `acquire`. The cost is that a
/// large chunk's write holds the mutex, which serialises chunk writes across
/// the node's shards; on one disk they would contend anyway. Every write here
/// is `chunk_durability::deferred`, so no fsync runs under this mutex, nor
/// under the Raft node mutex `apply()` holds: the store's syncer does them,
/// and the state machine calls `cache_chunk_store::wait_durable()` before a
/// snapshot. The fetcher's writes are deferred too; they were blocking
/// `apply()` on this mutex for the length of their fsync.
class chunk_refcounts {
public:
    explicit chunk_refcounts(cache_chunk_store& store) : _store(store) {}

    /// Store `bytes` if absent and take one reference, atomically with
    /// respect to `release`. Returns whether the bytes were written.
    /// `verified` says the caller has already checked that `bytes` hash to
    /// `sha` (see `cache_chunk_store::put_verified`).
    auto put_and_acquire(const chunk_sha& sha, std::span<const std::byte> bytes,
                         bool verified = false) -> bool {
        const std::lock_guard lock(_mu);
        const bool written =
            verified ? _store.put_verified(sha, bytes, deferred) : _store.put(sha, bytes, deferred);
        ++_counts[sha];
        return written;
    }

    /// Take `n` references to a chunk that may or may not be on disk yet (a
    /// snapshot restore names chunks the fetcher has still to bring).
    /// Returns the new count.
    auto acquire(const chunk_sha& sha, std::uint64_t n = 1) -> std::uint64_t {
        const std::lock_guard lock(_mu);
        return _counts[sha] += n;
    }

    /// Drop `n` references. At zero the chunk is removed from the store.
    /// Returns `true` if this call removed it. Releasing more references than
    /// are held is a caller bug and throws `std::logic_error`.
    auto release(const chunk_sha& sha, std::uint64_t n = 1) -> bool {
        const std::lock_guard lock(_mu);
        auto it = _counts.find(sha);
        if (it == _counts.end() || it->second < n) {
            throw std::logic_error("chunk_refcounts: release of unreferenced chunk " + to_hex(sha));
        }
        it->second -= n;
        if (it->second > 0) {
            return false;
        }
        _counts.erase(it);
        return _store.remove(sha);
    }

    /// Store `bytes` for a chunk this caller already holds a reference to
    /// but whose file is missing (a replayed `put_chunk` after a fetch was
    /// still pending). Under the same mutex as `release`, like every create.
    auto put_referenced(const chunk_sha& sha, std::span<const std::byte> bytes,
                        bool verified = false) -> bool {
        const std::lock_guard lock(_mu);
        return verified ? _store.put_verified(sha, bytes, deferred)
                        : _store.put(sha, bytes, deferred);
    }

    /// Store fetched `bytes` only if some replica on this node still
    /// references the chunk, atomically with respect to `release`. Returns
    /// false, writing nothing, when the last reference went while the fetch
    /// was in flight: storing it then would leave an orphan for start-up
    /// collection to find.
    auto put_if_referenced(const chunk_sha& sha, std::span<const std::byte> bytes,
                           bool verified = false) -> bool {
        const std::lock_guard lock(_mu);
        if (!_counts.contains(sha)) {
            return false;
        }
        (void)(verified ? _store.put_verified(sha, bytes, deferred)
                        : _store.put(sha, bytes, deferred));
        return true;
    }

    /// Drop `n` references without removing the chunk at zero. For a replica
    /// that is going away (destroyed on shutdown, or absorbed by a sibling)
    /// rather than deleting the keys it held: its chunks are still needed by
    /// whoever takes the keys over, or by the restart that rebuilds these
    /// counts. A file left with no reference is collected by start-up orphan
    /// collection (Requirement 2.6).
    auto forget(const chunk_sha& sha, std::uint64_t n = 1) -> void {
        const std::lock_guard lock(_mu);
        auto it = _counts.find(sha);
        if (it == _counts.end() || it->second < n) {
            throw std::logic_error("chunk_refcounts: forget of unreferenced chunk " + to_hex(sha));
        }
        it->second -= n;
        if (it->second == 0) {
            _counts.erase(it);
        }
    }

    /// Delete every stored chunk no replica on this node references
    /// (Requirement 2.6). Run once the node's replicas have restored their
    /// snapshots, so the counts are complete. It holds the mutex for the
    /// whole walk, so no `put_and_acquire` can store a chunk between the
    /// check and the delete. A chunk a later log entry still names is
    /// written again when that entry is applied, since `put_chunk` carries
    /// its bytes. Returns the number of chunks deleted.
    auto collect_orphans() -> std::size_t {
        const std::lock_guard lock(_mu);
        std::vector<chunk_sha> orphans;
        _store.for_each_chunk([&](const chunk_sha& sha, std::uint64_t) {
            if (!_counts.contains(sha)) {
                orphans.push_back(sha);
            }
        });
        std::size_t removed = 0;
        for (const auto& sha : orphans) {
            removed += _store.remove(sha) ? 1 : 0;
        }
        return removed;
    }

    [[nodiscard]] auto count(const chunk_sha& sha) const -> std::uint64_t {
        const std::lock_guard lock(_mu);
        auto it = _counts.find(sha);
        return it == _counts.end() ? 0 : it->second;
    }

    /// Distinct chunks with at least one reference.
    [[nodiscard]] auto size() const -> std::size_t {
        const std::lock_guard lock(_mu);
        return _counts.size();
    }

    [[nodiscard]] auto store() noexcept -> cache_chunk_store& { return _store; }
    [[nodiscard]] auto store() const noexcept -> const cache_chunk_store& { return _store; }

private:
    static constexpr chunk_durability deferred = chunk_durability::deferred;

    cache_chunk_store& _store;
    mutable std::mutex _mu;
    std::unordered_map<chunk_sha, std::uint64_t, chunk_sha_hash> _counts;
};

}  // namespace kythira
