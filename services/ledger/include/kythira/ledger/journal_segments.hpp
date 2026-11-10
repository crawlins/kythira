// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Journal segments (.kiro/specs/verifiable-ledger/ task 8, Requirements
// 3.5, 3.6 and 10.2; design "Blocks live in the engine first, segments
// later").
//
// A block is first written to the engine's journal/ keys with everything
// else. A leader-proposed seal entry ({type: ledger_seal, first, last})
// later names the oldest unsealed blocks; every replica writes them to a
// segment file, fsyncs it, and in one engine batch records the segment and
// deletes the range from journal/. The file's bytes depend only on the
// blocks, so every replica writes the same file with the same SHA-256, and
// a crash mid-seal is redone on replay (the batch never committed, so the
// blocks are still in journal/ and the partial file is overwritten).
//
// Segment file: the 8 bytes "KYLSEG01", then for each block in order a
// 4-byte big-endian length and the block's Ion binary (block::to_ion).
//
// An offload entry ({type: ledger_offload, first, objectKey}) records that
// the leader uploaded segment `first` to object storage under that key;
// replicas may then drop their local copy, and a read fetches it back and
// checks its SHA-256 (Requirement 10.2).

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/ledger_hash.hpp>

#include <raft/fault_injection.hpp>
#include <raft/key_object_store.hpp>

#include <array>
#include <cstdio>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace kythira::ledger {

/// A segment that cannot be read: missing, short, or failing its SHA-256.
class ledger_segment_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr std::string_view segment_magic = "KYLSEG01";

/// SHA-256 of a byte string held as text.
inline auto sha256_of(std::string_view bytes) -> hash256 {
    return sha256(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                                bytes.size()));
}

/// The bytes of a segment holding `blocks`, in order.
inline auto encode_segment(const std::vector<block>& blocks) -> std::string {
    std::string out(segment_magic);
    for (const auto& b : blocks) {
        const auto bytes = b.to_ion().to_binary();
        const auto n = static_cast<std::uint32_t>(bytes.size());
        out.push_back(static_cast<char>((n >> 24U) & 0xFFU));
        out.push_back(static_cast<char>((n >> 16U) & 0xFFU));
        out.push_back(static_cast<char>((n >> 8U) & 0xFFU));
        out.push_back(static_cast<char>(n & 0xFFU));
        out.append(bytes.begin(), bytes.end());
    }
    return out;
}

/// The blocks of a segment; throws ledger_format_error on a malformed one.
inline auto decode_segment(std::string_view bytes) -> std::vector<block> {
    if (!bytes.starts_with(segment_magic)) {
        throw ledger_format_error("not a ledger segment");
    }
    std::vector<block> out;
    std::size_t at = segment_magic.size();
    while (at < bytes.size()) {
        if (bytes.size() - at < 4) {
            throw ledger_format_error("truncated segment length");
        }
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            n = (n << 8U) | static_cast<std::uint8_t>(bytes[at + i]);
        }
        at += 4;
        if (bytes.size() - at < n) {
            throw ledger_format_error("truncated segment block");
        }
        out.push_back(block::from_ion(ion_value::parse(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.data() + at), n))));
        at += n;
    }
    return out;
}

/// A sealed segment's record in the ledger state.
struct segment_info {
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::string file;  // name in the ledger's segment directory
    std::uint64_t size = 0;
    hash256 sha256{};
    std::optional<std::string> object_key;  // set once offloaded

    [[nodiscard]] auto to_ion() const -> ion_value {
        ion_value v = ion_value::make_struct();
        v.set("first", ion_value::integer(static_cast<std::int64_t>(first)))
            .set("last", ion_value::integer(static_cast<std::int64_t>(last)))
            .set("file", ion_value::string(file))
            .set("size", ion_value::integer(static_cast<std::int64_t>(size)))
            .set("sha256", hash_value(sha256));
        if (object_key) {
            v.set("objectKey", ion_value::string(*object_key));
        }
        return v;
    }

    static auto from_ion(const ion_value& v) -> segment_info {
        namespace jd = journal_detail;
        segment_info s;
        s.first = jd::uint_field(v, "first");
        s.last = jd::uint_field(v, "last");
        s.file = jd::text_field(v, "file");
        s.size = jd::uint_field(v, "size");
        s.sha256 = jd::hash_field(v, "sha256");
        if (const ion_value* k = v.find("objectKey"); k != nullptr) {
            s.object_key = k->as_text();
        }
        return s;
    }

    /// The segment file's name for blocks first..last.
    static auto file_name(std::uint64_t first, std::uint64_t last) -> std::string {
        std::array<char, 64> buf{};
        std::snprintf(buf.data(), buf.size(), "seg-%020llu-%020llu.kyl",
                      static_cast<unsigned long long>(first),
                      static_cast<unsigned long long>(last));
        return buf.data();
    }
};

// ── Seal and offload entries ────────────────────────────────────────────────

inline constexpr std::string_view seal_type = "ledger_seal";
inline constexpr std::string_view offload_type = "ledger_offload";

struct seal_entry {
    std::uint64_t first = 0;
    std::uint64_t last = 0;
};

struct offload_entry {
    std::uint64_t first = 0;
    std::string object_key;
};

inline auto encode_seal(const seal_entry& e) -> std::vector<std::uint8_t> {
    ion_value v = ion_value::make_struct();
    v.set("type", ion_value::symbol(seal_type))
        .set("first", ion_value::integer(static_cast<std::int64_t>(e.first)))
        .set("last", ion_value::integer(static_cast<std::int64_t>(e.last)));
    return v.to_binary();
}

inline auto decode_seal(const ion_value& v) -> seal_entry {
    namespace jd = journal_detail;
    seal_entry e{jd::uint_field(v, "first"), jd::uint_field(v, "last")};
    if (e.last < e.first) {
        throw ledger_format_error("seal range ends before it starts");
    }
    return e;
}

inline auto encode_offload(const offload_entry& e) -> std::vector<std::uint8_t> {
    ion_value v = ion_value::make_struct();
    v.set("type", ion_value::symbol(offload_type))
        .set("first", ion_value::integer(static_cast<std::int64_t>(e.first)))
        .set("objectKey", ion_value::string(e.object_key));
    return v.to_binary();
}

inline auto decode_offload(const ion_value& v) -> offload_entry {
    namespace jd = journal_detail;
    offload_entry e{jd::uint_field(v, "first"), jd::text_field(v, "objectKey")};
    if (e.object_key.empty()) {
        throw ledger_format_error("empty object key");
    }
    return e;
}

// ── Files ───────────────────────────────────────────────────────────────────

namespace segment_detail {

inline auto fail(const std::string& what, const std::filesystem::path& p) -> void {
    throw std::system_error(errno, std::generic_category(), what + " " + p.string());
}

inline auto fsync_path(const std::filesystem::path& p, int flags) -> void {
    const int fd =
        ::open(p.c_str(), flags | O_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (fd < 0) {
        fail("open", p);
    }
    const int rc = ::fsync(fd);
    ::close(fd);
    if (rc != 0) {
        fail("fsync", p);
    }
}

}  // namespace segment_detail

/// Write `bytes` to `path` so that after return the file and its name are
/// durable: write a temporary, fsync it, rename it over `path`, fsync the
/// directory. A crash leaves either the old file, the new one, or a
/// temporary that the next write replaces.
/// The exit status of a process killed at one of the ledger's crash test
/// kill points (`fiu_do_on("ledger/...")`, compiled only with FIU_ENABLE).
inline constexpr int ledger_crash_exit_code = 86;

inline auto write_file_durably(const std::filesystem::path& path, std::string_view bytes) -> void {
    namespace sd = segment_detail;
    const auto tmp = std::filesystem::path(path.string() + ".tmp");
    const int fd = ::open(tmp.c_str(),  // NOLINT(cppcoreguidelines-pro-type-vararg)
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        sd::fail("open", tmp);
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            sd::fail("write", tmp);
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        sd::fail("fsync", tmp);
    }
    ::close(fd);
    // Crash test kill point (FIU_ENABLE builds only): the bytes are in the
    // temporary file and not yet under their name.
    fiu_do_on("ledger/segment/before_rename", std::_Exit(ledger_crash_exit_code));
    std::filesystem::rename(tmp, path);
    sd::fsync_path(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path(),
                   O_RDONLY | O_DIRECTORY);
}

/// A file's bytes, or nullopt if it does not exist.
inline auto read_file(const std::filesystem::path& path) -> std::optional<std::string> {
    const int fd =
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (fd < 0) {
        if (errno == ENOENT) {
            return std::nullopt;
        }
        segment_detail::fail("open", path);
    }
    std::string out;
    std::array<char, 1U << 16U> buf{};
    for (;;) {
        const auto n = ::read(fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            segment_detail::fail("read", path);
        }
        if (n == 0) {
            break;
        }
        out.append(buf.data(), static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

// ── Offload ─────────────────────────────────────────────────────────────────

/// Where offloaded segments go: a bucket and a key prefix in any of
/// Kythira's object stores (key_object_store.hpp).
struct segment_offload_target {
    std::string bucket;
    std::string prefix;  // e.g. "ledgers/<id>/segments/"
};

/// Upload a sealed segment's bytes (leader side); returns the object key
/// to propose in an offload entry. The bytes are checked against the
/// segment's SHA-256 before they leave, so a corrupted local copy is never
/// offloaded.
template<kythira::key_object_store Store>
auto upload_segment(const Store& store, const segment_offload_target& target,
                    const segment_info& seg, std::string_view bytes) -> std::string {
    if (bytes.size() != seg.size || sha256_of(bytes) != seg.sha256) {
        throw ledger_segment_error("segment " + seg.file + " does not match its SHA-256");
    }
    std::string key = target.prefix + seg.file;
    (void)store.put_object(target.bucket, key, bytes);
    return key;
}

/// A fetch function for basic_ledger_state_machine::set_segment_fetcher
/// that reads offloaded segments back from `store`.
template<kythira::key_object_store Store>
auto segment_fetcher(const Store& store, std::string bucket) {
    return
        [&store, bucket = std::move(bucket)](const std::string& key) -> std::optional<std::string> {
            auto got = store.get_object(bucket, key);
            if (!got) {
                return std::nullopt;
            }
            return std::move(got->body);
        };
}

}  // namespace kythira::ledger
