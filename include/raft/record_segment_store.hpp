// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file record_segment_store.hpp
/// @brief Per-node store of append-only record streams kept in segment files
///        outside the Raft snapshot (.kiro/specs/kinesis-compatible-streams/
///        task 3, design "Record segment store", Requirements 11.1 and 11.5).
///
/// Kinesis keeps a shard's record bytes here, and DynamoDB's change log is
/// the second user (.kiro/specs/dynamodb-compatible-api/ task 15), so the
/// interface knows nothing of either: a *stream* is an opaque key, and a
/// *record* is a caller-chosen 64-bit sequence, a timestamp and opaque bytes.
/// A state machine appends in `apply()`, keeps the small segment index
/// (`record_segment_descriptor`) in its own snapshot, and never puts record
/// bytes in the snapshot.
///
/// ## Determinism
///
/// Every replica that applies the same appends, seals and truncations
/// produces **byte-identical** segment files. Nothing node-specific is
/// written, and the store rolls to a new segment only on facts carried by
/// the records themselves: the segment's size and the spread of record
/// timestamps, never the wall clock. That is what lets the replicated index
/// carry one SHA-256 per segment, lets a replica fetch a missing segment
/// from any peer and check it, and lets a replica restoring a snapshot fetch
/// the *prefix* of a peer's active segment and verify it against the
/// digest-so-far the snapshot recorded (`describe` on an active segment).
/// A seal that is not caused by a roll (an idle stream, before tiering) must
/// come from a leader-proposed entry, like trimming, so every replica seals
/// at the same record.
///
/// ## Layout
///
/// ```
/// <data_dir>/streams/s_<escaped key>/<first seq, 20 digits>.open   active
/// <data_dir>/streams/s_<escaped key>/<first seq, 20 digits>.seg    sealed
/// <data_dir>/streams/s_<escaped key>/<first seq>.corrupt  quarantined, unused
/// <data_dir>/tmp/                                          installs, wiped
/// ```
///
/// A segment file, all integers big-endian:
///
/// ```
/// header  "KYRSEG\0\1"  u64 first_seq  u32 key_len  key  u32 crc32
/// record  u32 len  u64 seq  i64 timestamp_ms  bytes[len]  u32 crc32
/// ...
/// footer  u32 0xFFFFFFFF  u64 last_seq  u64 records  i64 first_ts  i64 last_ts
///         u32 crc32                                        (sealed only)
/// ```
///
/// Each CRC covers everything in its block before it. `len` is bounded by
/// `_max_record_bytes`, which is far below `0xFFFFFFFF`, so the footer's
/// marker cannot be mistaken for a record.
///
/// ## Durability and restart
///
/// `append` writes through the page cache and returns. `barrier()` fsyncs
/// every segment written since the last barrier (and a stream directory
/// that gained a file), coalescing concurrent callers onto one round of
/// fsyncs as `binary_file_persistence` does. A state machine calls it before
/// `apply()` returns for entries the node acknowledges, so the applied index
/// and the records it covers are durable together (design "Stream state is
/// small; record bytes are not in the snapshot").
///
/// Opening the store scans each active segment, verifies every record's CRC
/// and truncates a torn tail. That only removes bytes no barrier covered.
/// Records that are intact but beyond what the state machine's restored
/// snapshot covers are removed by `truncate_after`, after which the Raft log
/// tail is replayed through `append` again. A stream the snapshot does not
/// know at all is removed with `drop_stream` (`streams()` lists them).
///
/// Seals, installs, trims and truncations are rare and fsync before they
/// return; only appends are deferred to `barrier()`.
///
/// ## Peer fetch
///
/// `read_segment_bytes` serves a segment's raw file bytes to a peer, and
/// `fetch_segment` tries peers in order through a caller-supplied transport,
/// hashes what arrives and installs only bytes that match the descriptor.
/// `install_segment` re-parses everything it is given, so an installed file
/// is as trustworthy as one this node wrote. The transport, its rate limit
/// and its retry schedule belong to the daemon (the build cache's
/// `cache_chunk_fetcher` is the model).
///
/// ## Thread safety
///
/// Every member may be called from any thread. Metadata and writes are under
/// one mutex. Reads copy what they need under it and do their I/O outside,
/// against a file handle that stays open even if a concurrent seal or trim
/// closes or unlinks the file, so a read never blocks an `apply()`.
/// Appends to one stream must come from one thread at a time in sequence
/// order (they come from one group's apply loop), which `append` checks.

#include <raft/crc32.hpp>
#include <raft/fault_injection.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kythira {

/// A segment's SHA-256, over its whole file (header, records and, once
/// sealed, footer).
using segment_sha256 = std::array<std::byte, 32>;

/// An I/O failure or an impossible on-disk state. A state machine treats it
/// as fatal for the replica: skipping an append would let it diverge.
class record_segment_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct record_segment_store_options {
    /// The node's data directory for this store. `streams/` and `tmp/` go
    /// under it. One store per node is shared by every group on it.
    std::filesystem::path _data_dir;
    /// Roll to a new segment before a record that would take the active one
    /// past this size (footer included). A record larger than this still
    /// fits: it goes alone into a fresh segment.
    std::uint64_t _max_segment_bytes = std::uint64_t{64} * 1024 * 1024;
    /// Roll before a record whose timestamp is this far past the active
    /// segment's first record. Record time, not wall-clock time, so every
    /// replica rolls at the same record. Zero disables the age bound.
    std::int64_t _max_segment_age_ms = std::int64_t{60} * 60 * 1000;
    /// The largest record payload accepted. Also what the open scan uses to
    /// recognise a length that can only be garbage.
    std::uint32_t _max_record_bytes = std::uint32_t{16} * 1024 * 1024;
    /// Record one (seq, offset) pair in a segment's in-memory read index
    /// after at least this many bytes. Smaller is faster to seek, larger
    /// uses less memory.
    std::uint64_t _index_interval_bytes = std::uint64_t{64} * 1024;
    /// `false` makes `barrier()` a no-op (benchmarks and tmpfs tests).
    bool _barriers_enabled = true;
};

/// A record as the store holds it.
struct segment_record {
    std::uint64_t _seq = 0;
    std::int64_t _timestamp_ms = 0;
    std::vector<std::byte> _data;

    friend auto operator==(const segment_record&, const segment_record&) -> bool = default;
};

/// A record to append; the bytes are borrowed for the call.
struct segment_record_view {
    std::uint64_t _seq = 0;
    std::int64_t _timestamp_ms = 0;
    std::span<const std::byte> _data;
};

/// One segment as a state machine records it in its replicated index. For
/// an active segment the size and digest are of what was written so far,
/// which is enough to fetch and verify that prefix from a peer.
struct record_segment_descriptor {
    std::string _stream;
    std::uint64_t _first_seq = 0;
    std::uint64_t _last_seq = 0;
    std::uint64_t _records = 0;
    std::int64_t _first_timestamp_ms = 0;
    std::int64_t _last_timestamp_ms = 0;
    /// File size in bytes.
    std::uint64_t _bytes = 0;
    segment_sha256 _sha256{};
    bool _sealed = false;

    friend auto operator==(const record_segment_descriptor&, const record_segment_descriptor&)
        -> bool = default;
};

struct record_segment_append_result {
    /// Segments this append sealed by rolling, oldest first. A state machine
    /// moves them to sealed in its index.
    std::vector<record_segment_descriptor> _sealed;
};

/// Limits for one read. At least one record is returned when one is
/// available, even if it alone exceeds `_max_bytes`, so a reader always
/// makes progress.
struct segment_read_limits {
    std::size_t _max_records = 10'000;
    std::uint64_t _max_bytes = std::uint64_t{10} * 1024 * 1024;
};

struct segment_read_result {
    std::vector<segment_record> _records;
    /// Where to continue: one past the last returned record, or the
    /// requested position when nothing was returned.
    std::uint64_t _next_seq = 0;
    /// `read_segment` only: the segment holds nothing at or after
    /// `_next_seq`, so the caller moves to the next segment in its index.
    /// Always `false` for an active segment, which may still grow.
    bool _segment_exhausted = false;
};

struct record_segment_store_stats {
    std::uint64_t _streams = 0;
    std::uint64_t _active_segments = 0;
    std::uint64_t _sealed_segments = 0;
    std::uint64_t _bytes_on_disk = 0;
    std::uint64_t _records_appended = 0;
    std::uint64_t _barriers = 0;
    /// Active segments whose torn tail an open scan truncated.
    std::uint64_t _torn_tails_truncated = 0;
    /// Files an open scan could not trust and renamed to `.corrupt`.
    std::uint64_t _quarantined = 0;
    /// Segments installed from a peer.
    std::uint64_t _installed = 0;
    /// Peer answers that were absent or did not match the descriptor.
    std::uint64_t _fetch_misses = 0;
    /// Records a read found failing their CRC.
    std::uint64_t _read_crc_failures = 0;
};

// ---------------------------------------------------------------------------
// Descriptor encoding, for state machines that keep descriptors in their
// snapshots. Big-endian, versioned, self-delimiting.
// ---------------------------------------------------------------------------

namespace detail::segments {

inline constexpr std::array<std::byte, 8> magic = {std::byte{'K'}, std::byte{'Y'}, std::byte{'R'},
                                                   std::byte{'S'}, std::byte{'E'}, std::byte{'G'},
                                                   std::byte{0},   std::byte{1}};
inline constexpr std::uint32_t footer_marker = 0xFFFFFFFFU;
/// u32 len, u64 seq, i64 ts ... u32 crc
inline constexpr std::size_t record_head_bytes = 4 + 8 + 8;
inline constexpr std::size_t record_overhead = record_head_bytes + 4;
/// marker, last_seq, records, first_ts, last_ts, crc
inline constexpr std::size_t footer_bytes = 4 + 8 + 8 + 8 + 8 + 4;
inline constexpr std::uint8_t descriptor_version = 1;
inline constexpr std::size_t max_key_bytes = 4096;

inline void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) {
        out.push_back(static_cast<std::byte>((v >> static_cast<unsigned>(s)) & 0xFFU));
    }
}
inline void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) {
        out.push_back(static_cast<std::byte>((v >> static_cast<unsigned>(s)) & 0xFFU));
    }
}
[[nodiscard]] inline auto get_u32(std::span<const std::byte> in, std::size_t at) -> std::uint32_t {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v = (v << 8U) | static_cast<std::uint32_t>(in[at + i]);
    }
    return v;
}
[[nodiscard]] inline auto get_u64(std::span<const std::byte> in, std::size_t at) -> std::uint64_t {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v = (v << 8U) | static_cast<std::uint64_t>(in[at + i]);
    }
    return v;
}

[[nodiscard]] inline auto errno_text(const std::string& what) -> std::string {
    return "record_segment_store: " + what + ": " + std::strerror(errno);
}

/// An fd that closes when its last holder lets go, so a read or a barrier
/// can finish against a segment a concurrent seal closed or a trim unlinked.
class file_handle {
public:
    explicit file_handle(int fd) noexcept : _fd(fd) {}
    file_handle(const file_handle&) = delete;
    auto operator=(const file_handle&) -> file_handle& = delete;
    file_handle(file_handle&&) = delete;
    auto operator=(file_handle&&) -> file_handle& = delete;
    ~file_handle() {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }
    [[nodiscard]] auto fd() const noexcept -> int { return _fd; }

private:
    int _fd;
};

inline void pwrite_all(int fd, std::span<const std::byte> bytes, std::uint64_t offset,
                       const std::filesystem::path& path) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::pwrite(fd, bytes.data() + done, bytes.size() - done,
                                static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw record_segment_error(errno_text("write to " + path.string()));
        }
        done += static_cast<std::size_t>(n);
    }
}

/// Read up to `out.size()` bytes; returns how many were read (short at EOF).
[[nodiscard]] inline auto pread_some(int fd, std::span<std::byte> out, std::uint64_t offset)
    -> std::size_t {
    std::size_t done = 0;
    while (done < out.size()) {
        const auto n =
            ::pread(fd, out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            throw record_segment_error(errno_text("read"));
        }
        if (n == 0) {
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    return done;
}

[[nodiscard]] inline auto fsync_dir(const std::filesystem::path& p) -> bool {
    const int fd = ::open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool ok = ::fsync(fd) == 0 || errno == EINVAL;
    ::close(fd);
    return ok;
}

struct sha_ctx_deleter {
    void operator()(::EVP_MD_CTX* c) const noexcept { ::EVP_MD_CTX_free(c); }
};
using sha_ctx = std::unique_ptr<::EVP_MD_CTX, sha_ctx_deleter>;

[[nodiscard]] inline auto new_sha_ctx() -> sha_ctx {
    sha_ctx c(::EVP_MD_CTX_new());
    if (!c || ::EVP_DigestInit_ex(c.get(), ::EVP_sha256(), nullptr) != 1) {
        throw record_segment_error("record_segment_store: EVP_DigestInit_ex(sha256) failed");
    }
    return c;
}

inline void sha_update(::EVP_MD_CTX* c, std::span<const std::byte> bytes) {
    if (!bytes.empty() && ::EVP_DigestUpdate(c, bytes.data(), bytes.size()) != 1) {
        throw record_segment_error("record_segment_store: EVP_DigestUpdate failed");
    }
}

/// The digest so far, leaving `c` able to take more input.
[[nodiscard]] inline auto sha_peek(const ::EVP_MD_CTX* c) -> segment_sha256 {
    sha_ctx copy(::EVP_MD_CTX_new());
    segment_sha256 out{};
    unsigned int len = 0;
    if (!copy || ::EVP_MD_CTX_copy_ex(copy.get(), c) != 1 ||
        ::EVP_DigestFinal_ex(copy.get(), reinterpret_cast<unsigned char*>(out.data()), &len) != 1 ||
        len != out.size()) {
        throw record_segment_error("record_segment_store: EVP_DigestFinal_ex failed");
    }
    return out;
}

[[nodiscard]] inline auto sha256_of(std::span<const std::byte> bytes) -> segment_sha256 {
    auto c = new_sha_ctx();
    sha_update(c.get(), bytes);
    return sha_peek(c.get());
}

[[nodiscard]] inline auto encode_header(std::string_view key, std::uint64_t first_seq)
    -> std::vector<std::byte> {
    std::vector<std::byte> out(magic.begin(), magic.end());
    put_u64(out, first_seq);
    put_u32(out, static_cast<std::uint32_t>(key.size()));
    for (const char c : key) {
        out.push_back(static_cast<std::byte>(c));
    }
    put_u32(out, crc32(out));
    return out;
}

inline void encode_record(std::vector<std::byte>& out, const segment_record_view& r) {
    const auto start = out.size();
    put_u32(out, static_cast<std::uint32_t>(r._data.size()));
    put_u64(out, r._seq);
    put_u64(out, static_cast<std::uint64_t>(r._timestamp_ms));
    out.insert(out.end(), r._data.begin(), r._data.end());
    put_u32(out, crc32(std::span<const std::byte>(out).subspan(start)));
}

struct footer_fields {
    std::uint64_t _last_seq = 0;
    std::uint64_t _records = 0;
    std::int64_t _first_ts = 0;
    std::int64_t _last_ts = 0;
};

[[nodiscard]] inline auto encode_footer(const footer_fields& f) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    put_u32(out, footer_marker);
    put_u64(out, f._last_seq);
    put_u64(out, f._records);
    put_u64(out, static_cast<std::uint64_t>(f._first_ts));
    put_u64(out, static_cast<std::uint64_t>(f._last_ts));
    put_u32(out, crc32(out));
    return out;
}

[[nodiscard]] inline auto decode_footer(std::span<const std::byte> in)
    -> std::optional<footer_fields> {
    if (in.size() != footer_bytes || get_u32(in, 0) != footer_marker ||
        get_u32(in, footer_bytes - 4) != crc32(in.first(footer_bytes - 4))) {
        return std::nullopt;
    }
    return footer_fields{
        ._last_seq = get_u64(in, 4),
        ._records = get_u64(in, 12),
        ._first_ts = static_cast<std::int64_t>(get_u64(in, 20)),
        ._last_ts = static_cast<std::int64_t>(get_u64(in, 28)),
    };
}

struct parsed_header {
    std::string _key;
    std::uint64_t _first_seq = 0;
    std::size_t _size = 0;
};

/// Parse a header from the start of `in`; `nullopt` if it is short or bad.
[[nodiscard]] inline auto decode_header(std::span<const std::byte> in)
    -> std::optional<parsed_header> {
    constexpr std::size_t fixed = magic.size() + 8 + 4;
    if (in.size() < fixed + 4 || !std::equal(magic.begin(), magic.end(), in.begin())) {
        return std::nullopt;
    }
    const std::uint32_t key_len = get_u32(in, magic.size() + 8);
    if (key_len > max_key_bytes || in.size() < fixed + key_len + 4) {
        return std::nullopt;
    }
    const std::size_t size = fixed + key_len + 4;
    if (get_u32(in, size - 4) != crc32(in.first(size - 4))) {
        return std::nullopt;
    }
    parsed_header h;
    h._first_seq = get_u64(in, magic.size());
    h._key.assign(reinterpret_cast<const char*>(in.data() + fixed), key_len);
    h._size = size;
    return h;
}

/// What sits at an offset of a segment.
enum class block_kind : std::uint8_t {
    record,  ///< a complete record with a good CRC
    footer,  ///< a complete footer with a good CRC
    torn,    ///< runs past the end of the input
    corrupt  ///< bad length, bad CRC or bad marker
};

struct block {
    block_kind _kind = block_kind::torn;
    std::size_t _size = 0;
    segment_record _record;
    footer_fields _footer;
};

/// Parse one block at the start of `in`. `copy_data` false leaves
/// `_record._data` empty (scans that need only offsets and sequences).
[[nodiscard]] inline auto decode_block(std::span<const std::byte> in,
                                       std::uint32_t max_record_bytes, bool copy_data) -> block {
    block b;
    if (in.size() < 4) {
        b._kind = block_kind::torn;
        return b;
    }
    const std::uint32_t len = get_u32(in, 0);
    if (len == footer_marker) {
        if (in.size() < footer_bytes) {
            b._kind = block_kind::torn;
            return b;
        }
        auto f = decode_footer(in.first(footer_bytes));
        if (!f) {
            b._kind = block_kind::corrupt;
            return b;
        }
        b._kind = block_kind::footer;
        b._size = footer_bytes;
        b._footer = *f;
        return b;
    }
    if (len > max_record_bytes) {
        b._kind = block_kind::corrupt;
        return b;
    }
    const std::size_t size = record_overhead + len;
    if (in.size() < size) {
        b._kind = block_kind::torn;
        return b;
    }
    if (get_u32(in, size - 4) != crc32(in.first(size - 4))) {
        b._kind = block_kind::corrupt;
        return b;
    }
    b._kind = block_kind::record;
    b._size = size;
    b._record._seq = get_u64(in, 4);
    b._record._timestamp_ms = static_cast<std::int64_t>(get_u64(in, 12));
    if (copy_data) {
        const auto data = in.subspan(record_head_bytes, len);
        b._record._data.assign(data.begin(), data.end());
    }
    return b;
}

/// Reads blocks from a file region through a sliding buffer, so a scan of a
/// 64 MiB segment does not hold 64 MiB.
class block_reader {
public:
    block_reader(int fd, std::uint64_t offset, std::uint64_t end, std::uint32_t max_record_bytes)
        : _fd(fd), _offset(offset), _end(end), _max_record_bytes(max_record_bytes) {}

    /// The block at the current offset. Does not advance.
    [[nodiscard]] auto peek(bool copy_data) -> block {
        // Enough for a head; then enough for the whole record it announces.
        fill(record_overhead > footer_bytes ? record_overhead : footer_bytes);
        auto avail = window();
        if (avail.size() >= 4) {
            const std::uint32_t len = get_u32(avail, 0);
            if (len != footer_marker && len <= _max_record_bytes) {
                fill(record_overhead + len);
                avail = window();
            }
        }
        return decode_block(avail, _max_record_bytes, copy_data);
    }

    void advance(std::size_t n) {
        _pos += n;
        _offset += n;
    }
    [[nodiscard]] auto offset() const noexcept -> std::uint64_t { return _offset; }
    [[nodiscard]] auto at_end() const noexcept -> bool { return _offset >= _end; }

private:
    [[nodiscard]] auto window() const -> std::span<const std::byte> {
        return std::span<const std::byte>(_buf).subspan(_pos);
    }

    void fill(std::size_t want) {
        const std::uint64_t remaining_in_file = _end > _offset ? _end - _offset : 0;
        const std::size_t target =
            static_cast<std::size_t>(std::min<std::uint64_t>(want, remaining_in_file));
        if (_buf.size() - _pos >= target) {
            return;
        }
        // Drop what was consumed, then read at least a chunk more.
        _buf.erase(_buf.begin(), _buf.begin() + static_cast<std::ptrdiff_t>(_pos));
        _pos = 0;
        const std::uint64_t file_pos = _offset + _buf.size();
        const std::size_t need = target - _buf.size();
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(std::max<std::size_t>(need, read_chunk), _end - file_pos));
        const auto old = _buf.size();
        _buf.resize(old + chunk);
        const auto got = pread_some(_fd, std::span<std::byte>(_buf).subspan(old), file_pos);
        _buf.resize(old + got);
    }

    static constexpr std::size_t read_chunk = std::size_t{256} * 1024;
    int _fd;
    std::uint64_t _offset;
    std::uint64_t _end;
    std::uint32_t _max_record_bytes;
    std::vector<std::byte> _buf;
    std::size_t _pos = 0;
};

/// "s_" plus the key with every byte other than an ASCII letter, digit,
/// dot, underscore or hyphen written as a percent sign and two hex digits,
/// so any key is one safe directory name and never "." or "..".
[[nodiscard]] inline auto dir_name_for(std::string_view key) -> std::string {
    static constexpr std::string_view digits = "0123456789ABCDEF";
    std::string out = "s_";
    for (const char ch : key) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(digits[c >> 4U]);
            out.push_back(digits[c & 0xFU]);
        }
    }
    return out;
}

[[nodiscard]] inline auto file_name_for(std::uint64_t first_seq, bool sealed) -> std::string {
    std::string digits = std::to_string(first_seq);
    std::string out(20 - digits.size(), '0');
    out += digits;
    out += sealed ? ".seg" : ".open";
    return out;
}

}  // namespace detail::segments

/// Serialise a descriptor (for a state machine's snapshot).
[[nodiscard]] inline auto encode_segment_descriptor(const record_segment_descriptor& d)
    -> std::vector<std::byte> {
    namespace s = detail::segments;
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(s::descriptor_version));
    s::put_u32(out, static_cast<std::uint32_t>(d._stream.size()));
    for (const char c : d._stream) {
        out.push_back(static_cast<std::byte>(c));
    }
    s::put_u64(out, d._first_seq);
    s::put_u64(out, d._last_seq);
    s::put_u64(out, d._records);
    s::put_u64(out, static_cast<std::uint64_t>(d._first_timestamp_ms));
    s::put_u64(out, static_cast<std::uint64_t>(d._last_timestamp_ms));
    s::put_u64(out, d._bytes);
    out.insert(out.end(), d._sha256.begin(), d._sha256.end());
    out.push_back(static_cast<std::byte>(d._sealed ? 1 : 0));
    return out;
}

/// Parse a descriptor from the front of `in`, advancing it. `nullopt` on a
/// short or malformed input, leaving `in` unchanged.
[[nodiscard]] inline auto decode_segment_descriptor(std::span<const std::byte>& in)
    -> std::optional<record_segment_descriptor> {
    namespace s = detail::segments;
    constexpr std::size_t fixed_tail = (6 * 8) + 32 + 1;
    if (in.size() < 5 || static_cast<std::uint8_t>(in[0]) != s::descriptor_version) {
        return std::nullopt;
    }
    const std::uint32_t key_len = s::get_u32(in, 1);
    if (key_len > s::max_key_bytes || in.size() < 5 + key_len + fixed_tail) {
        return std::nullopt;
    }
    record_segment_descriptor d;
    d._stream.assign(reinterpret_cast<const char*>(in.data() + 5), key_len);
    std::size_t at = 5 + key_len;
    d._first_seq = s::get_u64(in, at);
    d._last_seq = s::get_u64(in, at + 8);
    d._records = s::get_u64(in, at + 16);
    d._first_timestamp_ms = static_cast<std::int64_t>(s::get_u64(in, at + 24));
    d._last_timestamp_ms = static_cast<std::int64_t>(s::get_u64(in, at + 32));
    d._bytes = s::get_u64(in, at + 40);
    at += 48;
    std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(at), 32, d._sha256.begin());
    at += 32;
    const auto sealed = static_cast<std::uint8_t>(in[at]);
    if (sealed > 1) {
        return std::nullopt;
    }
    d._sealed = sealed == 1;
    in = in.subspan(at + 1);
    return d;
}

/// The SHA-256 of `bytes`, for a fetch transport or a test.
[[nodiscard]] inline auto segment_sha256_of(std::span<const std::byte> bytes) -> segment_sha256 {
    return detail::segments::sha256_of(bytes);
}

class record_segment_store {
public:
    /// Fetches up to `length` bytes of a peer's copy of a segment, from
    /// offset 0 (the peer's `read_segment_bytes`). `nullopt` when the peer
    /// is down or has no copy.
    using fetch_transport = std::function<std::optional<std::vector<std::byte>>(
        const std::string& peer, const std::string& stream, std::uint64_t first_seq,
        std::uint64_t length)>;

    /// Opens (creating if needed) the store: wipes `tmp/`, reads every
    /// sealed segment's footer, scans every active segment (truncating a
    /// torn tail) and quarantines files it cannot trust.
    explicit record_segment_store(record_segment_store_options options)
        : _options(std::move(options)) {
        if (_options._max_record_bytes >= detail::segments::footer_marker) {
            throw std::invalid_argument("record_segment_store: _max_record_bytes too large");
        }
        if (_options._index_interval_bytes == 0) {
            _options._index_interval_bytes = 1;
        }
        _streams_dir = _options._data_dir / "streams";
        _tmp_dir = _options._data_dir / "tmp";
        std::filesystem::create_directories(_streams_dir);
        std::filesystem::remove_all(_tmp_dir);
        std::filesystem::create_directories(_tmp_dir);
        open_all();
    }

    record_segment_store(const record_segment_store&) = delete;
    auto operator=(const record_segment_store&) -> record_segment_store& = delete;
    record_segment_store(record_segment_store&&) = delete;
    auto operator=(record_segment_store&&) -> record_segment_store& = delete;
    ~record_segment_store() = default;

    [[nodiscard]] auto options() const noexcept -> const record_segment_store_options& {
        return _options;
    }

    // ---- writes (from apply) --------------------------------------------

    /// Append records to `stream` in order. Sequences must strictly increase
    /// and exceed the stream's last sequence (gaps are allowed). Rolls to a
    /// new segment when a record would break the size or age bound and
    /// returns the segments that sealed. Durable only after `barrier()`.
    auto append(const std::string& stream, std::span<const segment_record_view> records)
        -> record_segment_append_result {
        namespace s = detail::segments;
        validate_key(stream);
        record_segment_append_result result;
        if (records.empty()) {
            return result;
        }
        const std::lock_guard lock(_mu);
        std::optional<std::uint64_t> last;
        if (auto existing = _streams.find(stream); existing != _streams.end()) {
            last = last_seq_locked(existing->second);
        }
        for (const auto& r : records) {
            if (r._data.size() > _options._max_record_bytes) {
                throw std::invalid_argument("record_segment_store: record of " +
                                            std::to_string(r._data.size()) +
                                            " bytes exceeds the largest record");
            }
            if (last && r._seq <= *last) {
                throw std::invalid_argument("record_segment_store: sequence " +
                                            std::to_string(r._seq) + " does not exceed " +
                                            std::to_string(*last) + " on " + stream);
            }
            last = r._seq;
        }

        // Only now that the batch is known good may the stream come into
        // being, so a refused append leaves no empty stream behind.
        auto& st = stream_for(stream);
        segment_meta* active = active_locked(st);
        std::vector<std::byte> pending;
        for (const auto& r : records) {
            const std::uint64_t size = s::record_overhead + r._data.size();
            if (active != nullptr && active->_records > 0 &&
                should_roll(*active, pending.size(), r, size)) {
                flush_locked(st, *active, pending);
                result._sealed.push_back(seal_locked(st, *active));
                active = nullptr;
            }
            if (active == nullptr) {
                active = &create_locked(st, r._seq);
            }
            const auto at = active->_bytes + pending.size();
            s::encode_record(pending, r);
            note_record(*active, r._seq, r._timestamp_ms, at);
            ++_stats._records_appended;
        }
        flush_locked(st, *active, pending);
        return result;
    }

    /// Convenience overload for one record.
    auto append(const std::string& stream, const segment_record_view& record)
        -> record_segment_append_result {
        return append(stream, std::span<const segment_record_view>(&record, 1));
    }

    /// Make every append that returned before this call durable. Concurrent
    /// callers share one round of fsyncs. Throws if an fsync fails; the
    /// caller must not acknowledge the entries it covered.
    void barrier() {
        if (!_options._barriers_enabled) {
            return;
        }
        std::unique_lock lock(_mu);
        const std::uint64_t target = _written;
        while (_synced < target) {
            if (_barrier_running) {
                _barrier_cv.wait(lock);
                continue;
            }
            const std::uint64_t covering = _written;
            auto files = std::exchange(_dirty_files, {});
            auto dirs = std::exchange(_dirty_dirs, {});
            _barrier_running = true;
            ++_stats._barriers;
            lock.unlock();
            bool ok = true;
            fiu_do_on("raft/segments/barrier", ok = false;);
            for (const auto& [fd, f] : files) {
                ok = ok && ::fsync(f->fd()) == 0;
            }
            for (const auto& d : dirs) {
                ok = ok && detail::segments::fsync_dir(d);
            }
            lock.lock();
            _barrier_running = false;
            if (!ok) {
                // Put them back so a retry covers them again.
                _dirty_files.insert(files.begin(), files.end());
                _dirty_dirs.insert(dirs.begin(), dirs.end());
                _barrier_cv.notify_all();
                throw record_segment_error("record_segment_store: barrier fsync failed");
            }
            _synced = std::max(_synced, covering);
            _barrier_cv.notify_all();
        }
    }

    /// Seal the stream's active segment (a leader-proposed seal, or before
    /// tiering). Returns its final descriptor, or `nullopt` if the stream
    /// has no active segment. Durable on return.
    auto seal(const std::string& stream) -> std::optional<record_segment_descriptor> {
        const std::lock_guard lock(_mu);
        auto it = _streams.find(stream);
        if (it == _streams.end()) {
            return std::nullopt;
        }
        segment_meta* active = active_locked(it->second);
        if (active == nullptr) {
            return std::nullopt;
        }
        return seal_locked(it->second, *active);
    }

    /// Remove every record with a sequence above `seq` (or every record,
    /// when `seq` is `nullopt`): what a state machine does after restoring a
    /// snapshot that covers the stream up to `seq`, before the log tail is
    /// replayed. A sealed segment cut in the middle becomes active again,
    /// footer removed. Durable on return.
    void truncate_after(const std::string& stream, std::optional<std::uint64_t> seq) {
        if (!seq) {
            drop_stream(stream);
            return;
        }
        const std::lock_guard lock(_mu);
        auto it = _streams.find(stream);
        if (it == _streams.end()) {
            return;
        }
        auto& st = it->second;
        bool dir_changed = false;
        // Whole segments that start after `seq`.
        while (!st._segments.empty() && st._segments.rbegin()->first > *seq) {
            remove_file_locked(st, st._segments.rbegin()->second);
            st._segments.erase(std::prev(st._segments.end()));
            dir_changed = true;
        }
        if (!st._segments.empty()) {
            auto& seg = st._segments.rbegin()->second;
            if (seg._last_seq > *seq) {
                cut_segment_locked(st, seg, *seq);
                dir_changed = true;
            }
        }
        if (dir_changed) {
            sync_dir_now(st._dir);
        }
        if (st._segments.empty()) {
            erase_stream_locked(it);
        }
    }

    /// Remove sealed segments whose last record is below `seq` (a
    /// leader-proposed retention trim). The active segment is never trimmed.
    /// Returns the first sequences of the removed segments. Durable on
    /// return.
    auto trim_before(const std::string& stream, std::uint64_t seq) -> std::vector<std::uint64_t> {
        const std::lock_guard lock(_mu);
        std::vector<std::uint64_t> removed;
        auto it = _streams.find(stream);
        if (it == _streams.end()) {
            return removed;
        }
        auto& st = it->second;
        for (auto s = st._segments.begin(); s != st._segments.end();) {
            if (!s->second._sealed || s->second._last_seq >= seq) {
                break;
            }
            remove_file_locked(st, s->second);
            removed.push_back(s->first);
            s = st._segments.erase(s);
        }
        if (!removed.empty()) {
            sync_dir_now(st._dir);
        }
        if (st._segments.empty()) {
            erase_stream_locked(it);
        }
        return removed;
    }

    /// Remove one sealed segment's local copy (after it was tiered, or to
    /// replace a copy a read found corrupt). Returns whether one was there.
    auto remove_segment(const std::string& stream, std::uint64_t first_seq) -> bool {
        const std::lock_guard lock(_mu);
        auto it = _streams.find(stream);
        if (it == _streams.end()) {
            return false;
        }
        auto& st = it->second;
        auto s = st._segments.find(first_seq);
        if (s == st._segments.end() || !s->second._sealed) {
            return false;
        }
        remove_file_locked(st, s->second);
        st._segments.erase(s);
        sync_dir_now(st._dir);
        if (st._segments.empty()) {
            erase_stream_locked(it);
        }
        return true;
    }

    /// Remove a stream and all its segments. Durable on return.
    void drop_stream(const std::string& stream) {
        const std::lock_guard lock(_mu);
        auto it = _streams.find(stream);
        if (it == _streams.end()) {
            return;
        }
        for (auto& [first, seg] : it->second._segments) {
            remove_file_locked(it->second, seg);
        }
        it->second._segments.clear();
        erase_stream_locked(it);
    }

    // ---- peer fetch -------------------------------------------------------

    /// Store a segment's file bytes received from a peer. Checks the size
    /// and SHA-256 against `d`, then parses every block: the header must name
    /// `d._stream` and `d._first_seq`, every record's CRC must hold, and the
    /// counts, sequences and timestamps must match `d`; a sealed descriptor
    /// needs the footer and an active one must not have it. Throws
    /// `std::invalid_argument` on any mismatch, before touching the disk.
    ///
    /// A sealed segment may be installed anywhere in the stream (a backfill).
    /// An active segment must be the stream's last: installing one replaces
    /// a local segment with the same first sequence and refuses if a later
    /// one exists. Returns `false` if an identical sealed copy was already
    /// here. Durable on return.
    auto install_segment(const record_segment_descriptor& d, std::span<const std::byte> bytes)
        -> bool {
        namespace s = detail::segments;
        validate_key(d._stream);
        if (bytes.size() != d._bytes) {
            throw std::invalid_argument("record_segment_store: install of " +
                                        std::to_string(bytes.size()) + " bytes, descriptor says " +
                                        std::to_string(d._bytes));
        }
        if (s::sha256_of(bytes) != d._sha256) {
            throw std::invalid_argument(
                "record_segment_store: install bytes do not match the "
                "descriptor's SHA-256");
        }
        auto parsed = parse_whole(bytes, d);
        const std::lock_guard lock(_mu);
        if (auto existing = _streams.find(d._stream); existing != _streams.end()) {
            const auto& segs = existing->second._segments;
            if (auto ex = segs.find(d._first_seq); ex != segs.end()) {
                if (ex->second._sealed && d._sealed && ex->second._sha256 == d._sha256) {
                    return false;
                }
            }
            if (!d._sealed && segs.upper_bound(d._first_seq) != segs.end()) {
                throw std::invalid_argument(
                    "record_segment_store: an active segment must be the stream's last");
            }
        }
        auto& st = stream_for(d._stream);
        // Write to tmp, fsync, then rename into place (replacing a same-name
        // file) and fsync the directory.
        const auto tmp = _tmp_dir / ("install." + std::to_string(++_tmp_counter));
        const int fd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
        if (fd < 0) {
            throw record_segment_error(s::errno_text("create " + tmp.string()));
        }
        auto handle = std::make_shared<s::file_handle>(fd);
        s::pwrite_all(fd, bytes, 0, tmp);
        if (::fsync(fd) != 0) {
            throw record_segment_error(s::errno_text("fsync(" + tmp.string() + ")"));
        }
        if (auto ex = st._segments.find(d._first_seq); ex != st._segments.end()) {
            remove_file_locked(st, ex->second);
            st._segments.erase(ex);
        }
        const auto dest = st._dir / s::file_name_for(d._first_seq, d._sealed);
        if (::rename(tmp.c_str(), dest.c_str()) != 0) {
            throw record_segment_error(s::errno_text("rename(" + dest.string() + ")"));
        }
        sync_dir_now(st._dir);
        segment_meta seg = std::move(parsed);
        seg._path = dest;
        if (d._sealed) {
            seg._sha256 = d._sha256;
        } else {
            seg._fd = handle;
            seg._sha = s::new_sha_ctx();
            s::sha_update(seg._sha.get(), bytes);
        }
        st._segments.emplace(d._first_seq, std::move(seg));
        ++_stats._installed;
        return true;
    }

    /// Fetch `d` from the first peer whose bytes match it and install it.
    /// A peer that is down, has no copy, or sends bytes that do not match is
    /// skipped (counted in `_fetch_misses`). Returns whether it was
    /// installed (or already present).
    auto fetch_segment(const record_segment_descriptor& d, std::span<const std::string> peers,
                       const fetch_transport& transport) -> bool {
        if (has_segment(d)) {
            return true;
        }
        for (const auto& peer : peers) {
            auto bytes = transport(peer, d._stream, d._first_seq, d._bytes);
            if (!bytes || bytes->size() != d._bytes ||
                detail::segments::sha256_of(*bytes) != d._sha256) {
                const std::lock_guard lock(_mu);
                ++_stats._fetch_misses;
                continue;
            }
            try {
                install_segment(d, *bytes);
                return true;
            } catch (const std::invalid_argument&) {
                // Matching hash but unparseable means the descriptor itself
                // is wrong; another peer cannot help.
                const std::lock_guard lock(_mu);
                ++_stats._fetch_misses;
                return false;
            }
        }
        return false;
    }

    /// Up to `length` bytes of a local segment's file from offset 0, bounded
    /// by what has been written: what a peer's `fetch_transport` serves.
    /// `nullopt` if the segment is not here.
    [[nodiscard]] auto read_segment_bytes(const std::string& stream, std::uint64_t first_seq,
                                          std::uint64_t length) const
        -> std::optional<std::vector<std::byte>> {
        auto view = view_of(stream, first_seq);
        if (!view) {
            return std::nullopt;
        }
        const auto n = std::min(length, view->_bytes);
        std::vector<std::byte> out(static_cast<std::size_t>(n));
        const auto got = detail::segments::pread_some(view->_fd->fd(), out, 0);
        if (got != n) {
            return std::nullopt;
        }
        return out;
    }

    // ---- reads ------------------------------------------------------------

    /// Records of one segment at or after `from_seq`. The caller walks its
    /// own index segment by segment, so a missing or tiered segment is never
    /// silently skipped. `nullopt` if the segment is not here (fetch it).
    /// Throws `record_segment_error` if a record fails its CRC.
    [[nodiscard]] auto read_segment(const std::string& stream, std::uint64_t first_seq,
                                    std::uint64_t from_seq,
                                    const segment_read_limits& limits = {}) const
        -> std::optional<segment_read_result> {
        auto view = view_of(stream, first_seq);
        if (!view) {
            return std::nullopt;
        }
        segment_read_result out;
        out._next_seq = from_seq;
        read_view(*view, from_seq, limits, out);
        out._segment_exhausted = view->_sealed && (out._next_seq > view->_last_seq);
        return out;
    }

    /// Records at or after `from_seq` across this node's local segments, in
    /// order. Only for a caller that knows every segment in the range is
    /// local (check `local_segments` against the index); otherwise use
    /// `read_segment`.
    [[nodiscard]] auto read(const std::string& stream, std::uint64_t from_seq,
                            const segment_read_limits& limits = {}) const -> segment_read_result {
        segment_read_result out;
        out._next_seq = from_seq;
        for (const auto first : local_segment_starts(stream)) {
            auto view = view_of(stream, first);
            if (!view || view->_last_seq < out._next_seq) {
                continue;
            }
            read_view(*view, out._next_seq, limits, out);
            if (out._records.size() >= limits._max_records || bytes_of(out) >= limits._max_bytes) {
                break;
            }
        }
        return out;
    }

    // ---- inspection -------------------------------------------------------

    /// The descriptor of a local segment, digest included. For a sealed
    /// segment opened from disk the digest is computed on first call (one
    /// read of the file) and cached.
    [[nodiscard]] auto describe(const std::string& stream, std::uint64_t first_seq) const
        -> std::optional<record_segment_descriptor> {
        std::shared_ptr<detail::segments::file_handle> fd;
        record_segment_descriptor d;
        {
            const std::lock_guard lock(_mu);
            const auto* seg = find_locked(stream, first_seq);
            if (seg == nullptr) {
                return std::nullopt;
            }
            d = descriptor_locked(stream, *seg);
            if (!seg->_sealed || seg->_sha256) {
                return d;
            }
            fd = open_read(seg->_path);
            if (!fd) {
                return std::nullopt;
            }
        }
        // Hash the sealed file outside the lock, then cache it.
        auto ctx = detail::segments::new_sha_ctx();
        std::vector<std::byte> buf(std::size_t{1} << 20U);
        std::uint64_t off = 0;
        while (off < d._bytes) {
            const auto want =
                static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), d._bytes - off));
            const auto got =
                detail::segments::pread_some(fd->fd(), std::span(buf).first(want), off);
            if (got == 0) {
                return std::nullopt;
            }
            detail::segments::sha_update(ctx.get(), std::span<const std::byte>(buf).first(got));
            off += got;
        }
        d._sha256 = detail::segments::sha_peek(ctx.get());
        const std::lock_guard lock(_mu);
        if (auto* seg = find_locked_mut(stream, first_seq); seg != nullptr && seg->_sealed) {
            seg->_sha256 = d._sha256;
        }
        return d;
    }

    /// Descriptors of every local segment of `stream`, oldest first.
    [[nodiscard]] auto local_segments(const std::string& stream) const
        -> std::vector<record_segment_descriptor> {
        std::vector<record_segment_descriptor> out;
        for (const auto first : local_segment_starts(stream)) {
            if (auto d = describe(stream, first)) {
                out.push_back(std::move(*d));
            }
        }
        return out;
    }

    /// Whether this node holds a segment identical to `d` (same first
    /// sequence, size, seal state and digest).
    [[nodiscard]] auto has_segment(const record_segment_descriptor& d) const -> bool {
        auto local = describe(d._stream, d._first_seq);
        return local && local->_bytes == d._bytes && local->_sealed == d._sealed &&
               local->_sha256 == d._sha256;
    }

    /// The highest sequence stored for `stream`.
    [[nodiscard]] auto last_seq(const std::string& stream) const -> std::optional<std::uint64_t> {
        const std::lock_guard lock(_mu);
        auto it = _streams.find(stream);
        if (it == _streams.end()) {
            return std::nullopt;
        }
        return last_seq_locked(it->second);
    }

    /// Every stream with at least one local segment.
    [[nodiscard]] auto streams() const -> std::vector<std::string> {
        const std::lock_guard lock(_mu);
        std::vector<std::string> out;
        out.reserve(_streams.size());
        for (const auto& [key, st] : _streams) {
            out.push_back(key);
        }
        return out;
    }

    [[nodiscard]] auto stats() const -> record_segment_store_stats {
        const std::lock_guard lock(_mu);
        auto out = _stats;
        out._streams = _streams.size();
        out._active_segments = 0;
        out._sealed_segments = 0;
        out._bytes_on_disk = 0;
        for (const auto& [key, st] : _streams) {
            for (const auto& [first, seg] : st._segments) {
                (seg._sealed ? out._sealed_segments : out._active_segments) += 1;
                out._bytes_on_disk += seg._bytes;
            }
        }
        return out;
    }

private:
    struct segment_meta {
        std::filesystem::path _path;
        std::uint64_t _first_seq = 0;
        std::uint64_t _last_seq = 0;
        std::uint64_t _records = 0;
        std::int64_t _first_ts = 0;
        std::int64_t _last_ts = 0;
        /// File size: everything written, footer included once sealed.
        std::uint64_t _bytes = 0;
        bool _sealed = false;
        /// Open while active; sealed segments are opened per read.
        std::shared_ptr<detail::segments::file_handle> _fd;
        /// Running digest while active.
        detail::segments::sha_ctx _sha;
        /// Final digest once sealed (lazily for segments opened from disk).
        std::optional<segment_sha256> _sha256;
        /// (seq, offset) every `_index_interval_bytes`; the first record
        /// always. Empty for a sealed segment opened from disk until read.
        std::vector<std::pair<std::uint64_t, std::uint64_t>> _index;
        std::uint64_t _last_indexed_offset = 0;
        /// Offset of the first record (header size).
        std::uint64_t _data_start = 0;
    };

    struct stream_state {
        std::string _key;
        std::filesystem::path _dir;
        std::map<std::uint64_t, segment_meta> _segments;
    };

    /// What a read needs, copied under the lock.
    struct segment_view {
        std::shared_ptr<detail::segments::file_handle> _fd;
        std::uint64_t _bytes = 0;
        std::uint64_t _last_seq = 0;
        std::uint64_t _data_start = 0;
        bool _sealed = false;
        std::string _stream;
        std::uint64_t _first_seq = 0;
    };

    static void validate_key(const std::string& key) {
        if (key.empty() || key.size() > detail::segments::max_key_bytes) {
            throw std::invalid_argument("record_segment_store: stream key must be 1-4096 bytes");
        }
    }

    [[nodiscard]] static auto bytes_of(const segment_read_result& r) -> std::uint64_t {
        std::uint64_t n = 0;
        for (const auto& rec : r._records) {
            n += rec._data.size();
        }
        return n;
    }

    /// Whether `r` (encoded size `size`) must start a new segment. `pending`
    /// is what this append has encoded for `active` but not yet written:
    /// the decision must not depend on how a caller batched its appends.
    [[nodiscard]] auto should_roll(const segment_meta& active, std::uint64_t pending,
                                   const segment_record_view& r, std::uint64_t size) const -> bool {
        if (active._bytes + pending + size + detail::segments::footer_bytes >
            _options._max_segment_bytes) {
            return true;
        }
        return _options._max_segment_age_ms > 0 &&
               r._timestamp_ms - active._first_ts >= _options._max_segment_age_ms;
    }

    void note_record(segment_meta& seg, std::uint64_t seq, std::int64_t ts,
                     std::uint64_t at) const {
        if (seg._records == 0) {
            seg._first_ts = ts;
            seg._first_seq = seq;
        }
        seg._last_seq = seq;
        seg._last_ts = ts;
        ++seg._records;
        if (seg._index.empty() || at - seg._last_indexed_offset >= _options._index_interval_bytes) {
            seg._index.emplace_back(seq, at);
            seg._last_indexed_offset = at;
        }
    }

    auto stream_for(const std::string& key) -> stream_state& {
        auto it = _streams.find(key);
        if (it != _streams.end()) {
            return it->second;
        }
        stream_state st;
        st._key = key;
        st._dir = _streams_dir / detail::segments::dir_name_for(key);
        std::filesystem::create_directories(st._dir);
        if (!detail::segments::fsync_dir(_streams_dir)) {
            throw record_segment_error(
                detail::segments::errno_text("fsync(" + _streams_dir.string() + ")"));
        }
        return _streams.emplace(key, std::move(st)).first->second;
    }

    void erase_stream_locked(std::map<std::string, stream_state>::iterator it) {
        std::error_code ec;
        std::filesystem::remove_all(it->second._dir, ec);
        _dirty_dirs.erase(it->second._dir);
        (void)detail::segments::fsync_dir(_streams_dir);
        _streams.erase(it);
    }

    [[nodiscard]] static auto last_seq_locked(const stream_state& st)
        -> std::optional<std::uint64_t> {
        if (st._segments.empty()) {
            return std::nullopt;
        }
        return st._segments.rbegin()->second._last_seq;
    }

    [[nodiscard]] static auto active_locked(stream_state& st) -> segment_meta* {
        if (st._segments.empty() || st._segments.rbegin()->second._sealed) {
            return nullptr;
        }
        return &st._segments.rbegin()->second;
    }

    [[nodiscard]] auto find_locked(const std::string& stream, std::uint64_t first) const
        -> const segment_meta* {
        auto it = _streams.find(stream);
        if (it == _streams.end()) {
            return nullptr;
        }
        auto s = it->second._segments.find(first);
        return s == it->second._segments.end() ? nullptr : &s->second;
    }
    [[nodiscard]] auto find_locked_mut(const std::string& stream, std::uint64_t first) const
        -> segment_meta* {
        return const_cast<segment_meta*>(find_locked(stream, first));
    }

    [[nodiscard]] static auto descriptor_locked(const std::string& stream, const segment_meta& seg)
        -> record_segment_descriptor {
        record_segment_descriptor d;
        d._stream = stream;
        d._first_seq = seg._first_seq;
        d._last_seq = seg._last_seq;
        d._records = seg._records;
        d._first_timestamp_ms = seg._first_ts;
        d._last_timestamp_ms = seg._last_ts;
        d._bytes = seg._bytes;
        d._sealed = seg._sealed;
        if (!seg._sealed && seg._sha) {
            d._sha256 = detail::segments::sha_peek(seg._sha.get());
        } else if (seg._sha256) {
            d._sha256 = *seg._sha256;
        }
        return d;
    }

    [[nodiscard]] auto local_segment_starts(const std::string& stream) const
        -> std::vector<std::uint64_t> {
        const std::lock_guard lock(_mu);
        std::vector<std::uint64_t> out;
        auto it = _streams.find(stream);
        if (it != _streams.end()) {
            for (const auto& [first, seg] : it->second._segments) {
                out.push_back(first);
            }
        }
        return out;
    }

    [[nodiscard]] static auto open_read(const std::filesystem::path& p)
        -> std::shared_ptr<detail::segments::file_handle> {
        const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return nullptr;
        }
        return std::make_shared<detail::segments::file_handle>(fd);
    }

    auto create_locked(stream_state& st, std::uint64_t first_seq) -> segment_meta& {
        namespace s = detail::segments;
        segment_meta seg;
        seg._path = st._dir / s::file_name_for(first_seq, false);
        seg._first_seq = first_seq;
        const int fd = ::open(seg._path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
        if (fd < 0) {
            throw record_segment_error(s::errno_text("create " + seg._path.string()));
        }
        seg._fd = std::make_shared<s::file_handle>(fd);
        const auto header = s::encode_header(st._key, first_seq);
        s::pwrite_all(fd, header, 0, seg._path);
        seg._sha = s::new_sha_ctx();
        s::sha_update(seg._sha.get(), header);
        seg._bytes = header.size();
        seg._data_start = header.size();
        seg._last_indexed_offset = header.size();
        _dirty_dirs.insert(st._dir);
        _dirty_files[fd] = seg._fd;
        ++_written;
        return st._segments.emplace(first_seq, std::move(seg)).first->second;
    }

    void flush_locked(stream_state& /*st*/, segment_meta& seg, std::vector<std::byte>& pending) {
        if (pending.empty()) {
            return;
        }
        detail::segments::pwrite_all(seg._fd->fd(), pending, seg._bytes, seg._path);
        detail::segments::sha_update(seg._sha.get(), pending);
        seg._bytes += pending.size();
        pending.clear();
        _dirty_files[seg._fd->fd()] = seg._fd;
        ++_written;
    }

    auto seal_locked(stream_state& st, segment_meta& seg) -> record_segment_descriptor {
        namespace s = detail::segments;
        const auto footer = s::encode_footer({._last_seq = seg._last_seq,
                                              ._records = seg._records,
                                              ._first_ts = seg._first_ts,
                                              ._last_ts = seg._last_ts});
        s::pwrite_all(seg._fd->fd(), footer, seg._bytes, seg._path);
        if (::fsync(seg._fd->fd()) != 0) {
            throw record_segment_error(s::errno_text("fsync(" + seg._path.string() + ")"));
        }
        s::sha_update(seg._sha.get(), footer);
        seg._bytes += footer.size();
        const auto sealed_path = st._dir / s::file_name_for(seg._first_seq, true);
        if (::rename(seg._path.c_str(), sealed_path.c_str()) != 0) {
            throw record_segment_error(s::errno_text("rename(" + sealed_path.string() + ")"));
        }
        sync_dir_now(st._dir);
        seg._path = sealed_path;
        seg._sealed = true;
        seg._sha256 = s::sha_peek(seg._sha.get());
        seg._sha.reset();
        _dirty_files.erase(seg._fd->fd());
        seg._fd.reset();
        return descriptor_locked(st._key, seg);
    }

    /// Cut `seg` so its last record is the last one with a sequence at or
    /// below `seq`; reopen it as active if it was sealed. Removes the
    /// segment when nothing is left.
    void cut_segment_locked(stream_state& st, segment_meta& seg, std::uint64_t seq) {
        namespace s = detail::segments;
        auto handle = seg._fd ? seg._fd : open_read(seg._path);
        if (!handle) {
            throw record_segment_error(s::errno_text("open(" + seg._path.string() + ")"));
        }
        // Rebuild the kept prefix's metadata by scanning it.
        segment_meta kept;
        kept._first_seq = seg._first_seq;
        kept._data_start = seg._data_start;
        kept._last_indexed_offset = seg._data_start;
        s::block_reader reader(handle->fd(), seg._data_start, seg._bytes,
                               _options._max_record_bytes);
        while (!reader.at_end()) {
            auto b = reader.peek(false);
            if (b._kind != s::block_kind::record || b._record._seq > seq) {
                break;
            }
            note_record(kept, b._record._seq, b._record._timestamp_ms, reader.offset());
            reader.advance(b._size);
        }
        const std::uint64_t keep_bytes = reader.offset();
        const auto first = seg._first_seq;
        if (kept._records == 0) {
            remove_file_locked(st, seg);
            st._segments.erase(first);
            return;
        }
        const auto open_path = st._dir / s::file_name_for(first, false);
        if (seg._sealed && ::rename(seg._path.c_str(), open_path.c_str()) != 0) {
            throw record_segment_error(s::errno_text("rename(" + open_path.string() + ")"));
        }
        const int fd = ::open(open_path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            throw record_segment_error(s::errno_text("open(" + open_path.string() + ")"));
        }
        auto wfd = std::make_shared<s::file_handle>(fd);
        if (::ftruncate(fd, static_cast<off_t>(keep_bytes)) != 0 || ::fsync(fd) != 0) {
            throw record_segment_error(s::errno_text("truncate(" + open_path.string() + ")"));
        }
        if (seg._fd) {
            _dirty_files.erase(seg._fd->fd());
        }
        kept._path = open_path;
        kept._bytes = keep_bytes;
        kept._fd = wfd;
        kept._sha = hash_prefix(fd, keep_bytes);
        seg = std::move(kept);
    }

    [[nodiscard]] static auto hash_prefix(int fd, std::uint64_t n) -> detail::segments::sha_ctx {
        auto ctx = detail::segments::new_sha_ctx();
        std::vector<std::byte> buf(std::size_t{1} << 20U);
        std::uint64_t off = 0;
        while (off < n) {
            const auto want =
                static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), n - off));
            const auto got = detail::segments::pread_some(fd, std::span(buf).first(want), off);
            if (got == 0) {
                throw record_segment_error("record_segment_store: short read while hashing");
            }
            detail::segments::sha_update(ctx.get(), std::span<const std::byte>(buf).first(got));
            off += got;
        }
        return ctx;
    }

    void remove_file_locked(stream_state& st, segment_meta& seg) {
        if (seg._fd) {
            _dirty_files.erase(seg._fd->fd());
        }
        std::error_code ec;
        std::filesystem::remove(seg._path, ec);
        (void)st;
    }

    void sync_dir_now(const std::filesystem::path& dir) {
        if (!detail::segments::fsync_dir(dir)) {
            throw record_segment_error(detail::segments::errno_text("fsync(" + dir.string() + ")"));
        }
    }

    /// Validate a whole segment image against `d` and build its metadata.
    [[nodiscard]] auto parse_whole(std::span<const std::byte> bytes,
                                   const record_segment_descriptor& d) const -> segment_meta {
        namespace s = detail::segments;
        auto bad = [](const std::string& why) {
            return std::invalid_argument("record_segment_store: install: " + why);
        };
        auto h = s::decode_header(bytes);
        if (!h || h->_key != d._stream || h->_first_seq != d._first_seq) {
            throw bad("header does not name the descriptor's stream and first sequence");
        }
        segment_meta seg;
        seg._first_seq = d._first_seq;
        seg._data_start = h->_size;
        seg._last_indexed_offset = h->_size;
        std::size_t at = h->_size;
        bool footer = false;
        std::optional<std::uint64_t> prev;
        while (at < bytes.size()) {
            auto b = s::decode_block(bytes.subspan(at), _options._max_record_bytes, false);
            if (b._kind == s::block_kind::footer) {
                if (at + b._size != bytes.size()) {
                    throw bad("bytes after the footer");
                }
                const auto& f = b._footer;
                if (f._last_seq != seg._last_seq || f._records != seg._records ||
                    f._first_ts != seg._first_ts || f._last_ts != seg._last_ts) {
                    throw bad("footer disagrees with the records");
                }
                footer = true;
                at += b._size;
                break;
            }
            if (b._kind != s::block_kind::record) {
                throw bad("torn or corrupt record");
            }
            if (prev && b._record._seq <= *prev) {
                throw bad("sequences do not increase");
            }
            prev = b._record._seq;
            if (seg._records == 0 && b._record._seq != d._first_seq) {
                throw bad("first record is not the segment's first sequence");
            }
            note_record(seg, b._record._seq, b._record._timestamp_ms, at);
            at += b._size;
        }
        if (seg._records == 0) {
            throw bad("no records");
        }
        if (footer != d._sealed) {
            throw bad(d._sealed ? "sealed descriptor but no footer"
                                : "footer on an active segment");
        }
        if (seg._last_seq != d._last_seq || seg._records != d._records ||
            seg._first_ts != d._first_timestamp_ms || seg._last_ts != d._last_timestamp_ms) {
            throw bad("records disagree with the descriptor");
        }
        seg._bytes = bytes.size();
        seg._sealed = d._sealed;
        return seg;
    }

    [[nodiscard]] auto view_of(const std::string& stream, std::uint64_t first_seq) const
        -> std::optional<segment_view> {
        std::filesystem::path path;
        segment_view v;
        {
            const std::lock_guard lock(_mu);
            const auto* seg = find_locked(stream, first_seq);
            if (seg == nullptr) {
                return std::nullopt;
            }
            v._bytes = seg->_bytes;
            v._last_seq = seg->_last_seq;
            v._data_start = seg->_data_start;
            v._sealed = seg->_sealed;
            v._fd = seg->_fd;
            v._stream = stream;
            v._first_seq = first_seq;
            path = seg->_path;
        }
        if (!v._fd) {
            v._fd = open_read(path);
            if (!v._fd) {
                return std::nullopt;
            }
        }
        return v;
    }

    /// The offset to start scanning from for `from_seq`, from the segment's
    /// sparse index (built on first use for a sealed segment opened from
    /// disk).
    [[nodiscard]] auto seek_offset(const segment_view& v, std::uint64_t from_seq) const
        -> std::uint64_t {
        const auto& stream = v._stream;
        const auto first_seq = v._first_seq;
        {
            const std::lock_guard lock(_mu);
            const auto* seg = find_locked(stream, first_seq);
            if (seg != nullptr && !seg->_index.empty()) {
                return offset_from_index(seg->_index, from_seq, v._data_start);
            }
        }
        if (!v._sealed) {
            return v._data_start;
        }
        // Build the sealed segment's index outside the lock.
        namespace s = detail::segments;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> index;
        std::uint64_t last_indexed = v._data_start;
        s::block_reader reader(v._fd->fd(), v._data_start, v._bytes, _options._max_record_bytes);
        while (!reader.at_end()) {
            auto b = reader.peek(false);
            if (b._kind != s::block_kind::record) {
                break;
            }
            if (index.empty() || reader.offset() - last_indexed >= _options._index_interval_bytes) {
                index.emplace_back(b._record._seq, reader.offset());
                last_indexed = reader.offset();
            }
            reader.advance(b._size);
        }
        const auto off = offset_from_index(index, from_seq, v._data_start);
        const std::lock_guard lock(_mu);
        if (auto* seg = find_locked_mut(stream, first_seq);
            seg != nullptr && seg->_sealed && seg->_index.empty()) {
            seg->_index = std::move(index);
        }
        return off;
    }

    [[nodiscard]] static auto offset_from_index(
        const std::vector<std::pair<std::uint64_t, std::uint64_t>>& index, std::uint64_t from_seq,
        std::uint64_t data_start) -> std::uint64_t {
        auto it = std::upper_bound(index.begin(), index.end(), from_seq,
                                   [](std::uint64_t s, const auto& e) { return s < e.first; });
        if (it == index.begin()) {
            return data_start;
        }
        return std::prev(it)->second;
    }

    /// Append to `out` the records of `v` at or after `from_seq`, within
    /// `limits` (counted together with what `out` already holds), and move
    /// `out._next_seq` past them.
    void read_view(const segment_view& v, std::uint64_t from_seq, const segment_read_limits& limits,
                   segment_read_result& out) const {
        namespace s = detail::segments;
        std::uint64_t bytes = bytes_of(out);
        const auto start = seek_offset(v, from_seq);
        s::block_reader reader(v._fd->fd(), start, v._bytes, _options._max_record_bytes);
        while (!reader.at_end() && out._records.size() < limits._max_records) {
            auto b = reader.peek(true);
            if (b._kind == s::block_kind::footer) {
                break;
            }
            if (b._kind != s::block_kind::record) {
                {
                    const std::lock_guard lock(_mu);
                    ++_stats._read_crc_failures;
                }
                throw record_segment_error("record_segment_store: corrupt record in " + v._stream +
                                           " segment " + std::to_string(v._first_seq) +
                                           " at offset " + std::to_string(reader.offset()));
            }
            reader.advance(b._size);
            if (b._record._seq < from_seq) {
                continue;
            }
            const auto n = b._record._data.size();
            if (!out._records.empty() && bytes + n > limits._max_bytes) {
                break;
            }
            bytes += n;
            out._next_seq = b._record._seq + 1;
            out._records.push_back(std::move(b._record));
        }
    }

    void open_all() {
        namespace s = detail::segments;
        for (const auto& entry : std::filesystem::directory_iterator(_streams_dir)) {
            if (!entry.is_directory()) {
                continue;
            }
            open_stream_dir(entry.path());
        }
    }

    void quarantine(const std::filesystem::path& p) {
        auto dest = p;
        dest.replace_extension(".corrupt");
        std::error_code ec;
        std::filesystem::rename(p, dest, ec);
        ++_stats._quarantined;
    }

    void open_stream_dir(const std::filesystem::path& dir) {
        namespace s = detail::segments;
        std::vector<std::filesystem::path> files;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            const auto ext = e.path().extension();
            if (e.is_regular_file() && (ext == ".open" || ext == ".seg")) {
                files.push_back(e.path());
            }
        }
        std::sort(files.begin(), files.end());
        stream_state st;
        st._dir = dir;
        bool changed = false;
        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto& p = files[i];
            const bool last_file = i + 1 == files.size();
            auto seg = open_segment_file(p, st._key, last_file, changed);
            if (seg) {
                if (st._segments.contains(seg->_first_seq)) {
                    // Both `.open` and `.seg` for one segment: a crash mid
                    // rename cannot do this, but keep the sealed copy.
                    quarantine(p);
                    continue;
                }
                st._segments.emplace(seg->_first_seq, std::move(*seg));
            }
        }
        if (changed) {
            sync_dir_now(dir);
        }
        if (st._segments.empty() || st._key.empty()) {
            if (st._segments.empty()) {
                std::error_code ec;
                std::filesystem::remove_all(dir, ec);
            }
            return;
        }
        if (dir.filename() != s::dir_name_for(st._key)) {
            throw record_segment_error("record_segment_store: " + dir.string() +
                                       " holds segments of stream '" + st._key + "'");
        }
        _streams.emplace(st._key, std::move(st));
    }

    /// Open one file. A `.seg` is trusted on its footer; an `.open` is
    /// scanned, its torn tail truncated, and a complete footer finishes an
    /// interrupted seal. An `.open` that is not the stream's last file is
    /// sealed now (only the last segment of a stream takes appends).
    auto open_segment_file(const std::filesystem::path& p, std::string& key, bool last_file,
                           bool& changed) -> std::optional<segment_meta> {
        namespace s = detail::segments;
        const bool sealed_name = p.extension() == ".seg";
        const int fd = ::open(p.c_str(), sealed_name ? O_RDONLY | O_CLOEXEC : O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            throw record_segment_error(s::errno_text("open(" + p.string() + ")"));
        }
        auto handle = std::make_shared<s::file_handle>(fd);
        struct ::stat stbuf{};
        if (::fstat(fd, &stbuf) != 0) {
            throw record_segment_error(s::errno_text("fstat(" + p.string() + ")"));
        }
        const auto size = static_cast<std::uint64_t>(stbuf.st_size);
        std::vector<std::byte> head(
            std::min<std::uint64_t>(size, 8 + 8 + 4 + s::max_key_bytes + 4));
        (void)s::pread_some(fd, head, 0);
        auto h = s::decode_header(head);
        if (!h || (!key.empty() && h->_key != key)) {
            if (sealed_name || h) {
                quarantine(p);
            } else {
                // A torn header: the segment was being created.
                std::error_code ec;
                std::filesystem::remove(p, ec);
            }
            changed = true;
            return std::nullopt;
        }
        key = h->_key;
        segment_meta seg;
        seg._first_seq = h->_first_seq;
        seg._data_start = h->_size;
        seg._last_indexed_offset = h->_size;
        seg._path = p;
        if (p.stem().string() != s::file_name_for(h->_first_seq, false).substr(0, 20)) {
            quarantine(p);
            changed = true;
            return std::nullopt;
        }
        if (sealed_name) {
            std::vector<std::byte> tail(s::footer_bytes);
            const bool ok = size >= h->_size + s::footer_bytes &&
                            s::pread_some(fd, tail, size - s::footer_bytes) == s::footer_bytes;
            auto f = ok ? s::decode_footer(tail) : std::nullopt;
            if (!f || f->_records == 0) {
                quarantine(p);
                changed = true;
                return std::nullopt;
            }
            seg._last_seq = f->_last_seq;
            seg._records = f->_records;
            seg._first_ts = f->_first_ts;
            seg._last_ts = f->_last_ts;
            seg._bytes = size;
            seg._sealed = true;
            return seg;
        }
        // Active: scan, hash and index.
        seg._sha = s::new_sha_ctx();
        s::sha_update(seg._sha.get(), std::span<const std::byte>(head).first(h->_size));
        s::block_reader reader(fd, h->_size, size, _options._max_record_bytes);
        bool footer = false;
        s::footer_fields ff;
        while (!reader.at_end()) {
            auto b = reader.peek(true);
            if (b._kind == s::block_kind::record) {
                if (seg._records > 0 && b._record._seq <= seg._last_seq) {
                    break;
                }
                note_record(seg, b._record._seq, b._record._timestamp_ms, reader.offset());
                // Hash the block exactly as it is on disk.
                std::vector<std::byte> enc;
                s::encode_record(enc, {b._record._seq, b._record._timestamp_ms, b._record._data});
                s::sha_update(seg._sha.get(), enc);
                reader.advance(b._size);
                continue;
            }
            if (b._kind == s::block_kind::footer && seg._records > 0 &&
                b._footer._last_seq == seg._last_seq && b._footer._records == seg._records &&
                b._footer._first_ts == seg._first_ts && b._footer._last_ts == seg._last_ts &&
                reader.offset() + b._size == size) {
                footer = true;
                ff = b._footer;
                s::sha_update(seg._sha.get(), s::encode_footer(ff));
                reader.advance(b._size);
            }
            break;
        }
        const auto good = reader.offset();
        if (seg._records == 0) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
            changed = true;
            return std::nullopt;
        }
        if (good < size) {
            if (::ftruncate(fd, static_cast<off_t>(good)) != 0 || ::fsync(fd) != 0) {
                throw record_segment_error(s::errno_text("truncate(" + p.string() + ")"));
            }
            ++_stats._torn_tails_truncated;
        }
        seg._bytes = good;
        seg._fd = handle;
        if (footer || !last_file) {
            if (!footer) {
                const auto enc = s::encode_footer({._last_seq = seg._last_seq,
                                                   ._records = seg._records,
                                                   ._first_ts = seg._first_ts,
                                                   ._last_ts = seg._last_ts});
                s::pwrite_all(fd, enc, seg._bytes, p);
                if (::fsync(fd) != 0) {
                    throw record_segment_error(s::errno_text("fsync(" + p.string() + ")"));
                }
                s::sha_update(seg._sha.get(), enc);
                seg._bytes += enc.size();
            }
            const auto sealed_path = p.parent_path() / s::file_name_for(seg._first_seq, true);
            if (::rename(p.c_str(), sealed_path.c_str()) != 0) {
                throw record_segment_error(s::errno_text("rename(" + sealed_path.string() + ")"));
            }
            changed = true;
            seg._path = sealed_path;
            seg._sealed = true;
            seg._sha256 = s::sha_peek(seg._sha.get());
            seg._sha.reset();
            seg._fd.reset();
        }
        return seg;
    }

    record_segment_store_options _options;
    std::filesystem::path _streams_dir;
    std::filesystem::path _tmp_dir;

    mutable std::mutex _mu;
    std::map<std::string, stream_state> _streams;
    mutable record_segment_store_stats _stats;
    std::uint64_t _tmp_counter = 0;

    // Barrier state.
    std::unordered_map<int, std::shared_ptr<detail::segments::file_handle>> _dirty_files;
    std::set<std::filesystem::path> _dirty_dirs;
    std::uint64_t _written = 0;
    std::uint64_t _synced = 0;
    bool _barrier_running = false;
    std::condition_variable _barrier_cv;
};

}  // namespace kythira
