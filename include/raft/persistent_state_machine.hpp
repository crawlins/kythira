// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file persistent_state_machine.hpp
/// @brief The contract for a state machine whose state lives in an on-disk
///        key-value engine and is never serialized whole
///        (.kiro/specs/persistent-state-machine/ Requirement 1, task 1).
///
/// Every state machine before this one satisfies `state_machine`
/// (types.hpp): `apply`, `get_state` and `restore_from_snapshot`, so its
/// state moves as one byte buffer on every snapshot, restart, read and split.
/// This header adds a second kind:
///
/// - `kv_engine`: one ordered key-value store per node, shared by every group
///   on the node, each group owning a `key_range` in it. Writes are atomic
///   `write_batch`es, each given an `engine_sequence`; the engine reports the
///   highest sequence that would survive a power cut, so the node can bound
///   log compaction by it (Requirement 3.1).
/// - `persistent_state_machine`: applies an entry and its applied index in
///   one batch, opens on a range of an engine, and hands state to a follower
///   as a checkpoint directory described by a `snapshot_manifest`.
/// - `persistent_splittable_state_machine`: splits and merges by handing out
///   and adopting ranges, never by copying keys.
///
/// `node<Types>` will choose its path with `storage_kind_of` (task 3). A
/// type that satisfies both the byte-buffer snapshot pair and
/// `persistent_state_machine` is treated as persistent: the persistent path
/// never calls `get_state`, so a type that also offers it (for a debug dump,
/// say) loses nothing, while the opposite choice would serialize a state that
/// may not fit in memory (Requirement 1.5).
///
/// `snapshot_manifest` is also the payload `.kiro/specs/file-install-snapshot/`
/// carries on the first chunk of a format-1 InstallSnapshot, so it has a
/// bounded binary encoding here. Its file names are checked on decode because
/// a follower creates files by those names in its staging directory.

#include <raft/types.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

// ---------------------------------------------------------------------------
// Engine vocabulary
// ---------------------------------------------------------------------------

/// The engine's column families (design "One engine per node"): state
/// machine data, transaction state (the wrapper's intents, records and
/// settled markers, Requirement 8) and per-group metadata (applied index,
/// range, epoch).
enum class column_family : std::uint8_t {
    data = 0,
    txn = 1,
    meta = 2,
};

inline constexpr std::array<column_family, 3> k_all_column_families{
    column_family::data, column_family::txn, column_family::meta};

/// A half-open range `[start, end)` of engine keys. An absent `end` is
/// unbounded above; an empty `start` is unbounded below. Keys compare as
/// unsigned bytes, the order `std::string` already uses.
struct key_range {
    std::string start;
    std::optional<std::string> end;

    [[nodiscard]] auto contains(std::string_view key) const noexcept -> bool {
        return key >= std::string_view(start) && (!end.has_value() || key < std::string_view(*end));
    }

    /// True when every key of `other` is a key of this range.
    [[nodiscard]] auto contains(const key_range& other) const noexcept -> bool {
        if (other.start < start) {
            return false;
        }
        if (!end.has_value()) {
            return true;
        }
        return other.end.has_value() && *other.end <= *end;
    }

    [[nodiscard]] auto empty() const noexcept -> bool { return end.has_value() && *end <= start; }

    auto operator==(const key_range&) const -> bool = default;
};

/// A monotonically increasing number the engine gives each committed write
/// batch. Zero means "nothing written".
using engine_sequence = std::uint64_t;

enum class scan_direction : std::uint8_t {
    forward,
    reverse,
};

/// Called for each key of a scan in order; returning false stops the scan.
/// The views are valid only for the duration of the call. Callers bound
/// their own scans this way (Requirement 5.3).
using scan_visitor = std::function<bool(std::string_view key, std::string_view value)>;

/// An atomic group of writes. The engine applies all of a batch or none of
/// it, in the order the operations were added.
class write_batch {
public:
    struct put_op {
        column_family cf;
        std::string key;
        std::string value;
    };
    struct erase_op {
        column_family cf;
        std::string key;
    };
    struct erase_range_op {
        column_family cf;
        key_range range;
    };
    using operation = std::variant<put_op, erase_op, erase_range_op>;

    auto put(column_family cf, std::string key, std::string value) -> write_batch& {
        _ops.emplace_back(put_op{cf, std::move(key), std::move(value)});
        return *this;
    }
    auto erase(column_family cf, std::string key) -> write_batch& {
        _ops.emplace_back(erase_op{cf, std::move(key)});
        return *this;
    }
    auto erase_range(column_family cf, key_range range) -> write_batch& {
        _ops.emplace_back(erase_range_op{cf, std::move(range)});
        return *this;
    }

    [[nodiscard]] auto operations() const noexcept -> const std::vector<operation>& { return _ops; }
    [[nodiscard]] auto empty() const noexcept -> bool { return _ops.empty(); }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return _ops.size(); }

private:
    std::vector<operation> _ops;
};

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

using sha256_digest = std::array<std::uint8_t, 32>;

class persistent_storage_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace persistent_detail {

class sha256_hasher {
public:
    sha256_hasher() : _ctx(::EVP_MD_CTX_new()) {
        if (_ctx == nullptr || ::EVP_DigestInit_ex(_ctx, ::EVP_sha256(), nullptr) != 1) {
            ::EVP_MD_CTX_free(_ctx);
            throw persistent_storage_error("sha256: EVP_DigestInit_ex failed");
        }
    }
    ~sha256_hasher() { ::EVP_MD_CTX_free(_ctx); }
    sha256_hasher(const sha256_hasher&) = delete;
    auto operator=(const sha256_hasher&) -> sha256_hasher& = delete;
    sha256_hasher(sha256_hasher&&) = delete;
    auto operator=(sha256_hasher&&) -> sha256_hasher& = delete;

    auto update(const void* data, std::size_t size) -> void {
        if (size != 0 && ::EVP_DigestUpdate(_ctx, data, size) != 1) {
            throw persistent_storage_error("sha256: EVP_DigestUpdate failed");
        }
    }

    [[nodiscard]] auto finish() -> sha256_digest {
        sha256_digest out{};
        unsigned int len = 0;
        if (::EVP_DigestFinal_ex(_ctx, out.data(), &len) != 1 || len != out.size()) {
            throw persistent_storage_error("sha256: EVP_DigestFinal_ex failed");
        }
        return out;
    }

private:
    ::EVP_MD_CTX* _ctx;
};

}  // namespace persistent_detail

[[nodiscard]] inline auto sha256_of(std::string_view bytes) -> sha256_digest {
    persistent_detail::sha256_hasher h;
    h.update(bytes.data(), bytes.size());
    return h.finish();
}

/// The SHA-256 and size of a file, read in 64 KiB pieces.
[[nodiscard]] inline auto sha256_of_file(const std::filesystem::path& path)
    -> std::pair<sha256_digest, std::uint64_t> {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw persistent_storage_error("sha256: cannot open " + path.string());
    }
    persistent_detail::sha256_hasher h;
    std::vector<char> buf(64 * 1024);
    std::uint64_t total = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = in.gcount();
        if (n > 0) {
            h.update(buf.data(), static_cast<std::size_t>(n));
            total += static_cast<std::uint64_t>(n);
        }
    }
    if (in.bad()) {
        throw persistent_storage_error("sha256: read failed on " + path.string());
    }
    return {h.finish(), total};
}

// ---------------------------------------------------------------------------
// Snapshot manifest
// ---------------------------------------------------------------------------

/// One file of a checkpoint, named relative to the checkpoint directory.
struct checkpoint_file {
    std::string name;
    std::uint64_t size = 0;
    sha256_digest sha256{};

    auto operator==(const checkpoint_file&) const -> bool = default;
};

class snapshot_manifest_error : public persistent_storage_error {
public:
    using persistent_storage_error::persistent_storage_error;
};

/// The ordered list of files in a checkpoint of one group range, with the
/// engine format that wrote them (file-install-snapshot glossary,
/// "Manifest"). A checkpoint is ingested only by an engine with the same
/// `engine_format`.
struct snapshot_manifest {
    /// Bumped when the encoding below changes.
    static constexpr std::uint16_t k_encoding_version = 1;
    /// Bounds on a decoded manifest. A RocksDB checkpoint has a few hundred
    /// files; the caps exist so a hostile first chunk cannot make a follower
    /// allocate without limit (file-install-snapshot Requirement 1.5).
    static constexpr std::size_t k_max_encoded_bytes = k_max_install_snapshot_manifest_bytes;
    static constexpr std::size_t k_max_files = 16 * 1024;
    static constexpr std::size_t k_max_name_bytes = 255;
    static constexpr std::size_t k_max_format_bytes = 64;
    static constexpr std::size_t k_max_range_key_bytes = 64 * 1024;

    std::string engine_format;
    key_range range;
    std::vector<checkpoint_file> files;

    auto operator==(const snapshot_manifest&) const -> bool = default;

    /// Total bytes of every file: the length of the format-1 stream.
    [[nodiscard]] auto total_bytes() const noexcept -> std::uint64_t {
        std::uint64_t total = 0;
        for (const auto& f : files) {
            total += f.size;
        }
        return total;
    }

    /// A file name is acceptable when it is a plain relative name: not
    /// empty, not "." or "..", no path separator, no NUL, within the length
    /// cap. Checkpoints are flat directories, so nothing more is needed, and
    /// this is what keeps a follower writing inside its staging directory.
    [[nodiscard]] static auto valid_file_name(std::string_view name) noexcept -> bool {
        if (name.empty() || name.size() > k_max_name_bytes || name == "." || name == "..") {
            return false;
        }
        for (const char c : name) {
            if (c == '/' || c == '\\' || c == '\0') {
                return false;
            }
        }
        return true;
    }

    /// Throws `snapshot_manifest_error` if a field breaks a bound or a file
    /// name is unacceptable or repeated.
    auto validate() const -> void {
        if (engine_format.empty() || engine_format.size() > k_max_format_bytes) {
            throw snapshot_manifest_error("snapshot_manifest: bad engine_format length");
        }
        if (range.start.size() > k_max_range_key_bytes ||
            (range.end.has_value() && range.end->size() > k_max_range_key_bytes)) {
            throw snapshot_manifest_error("snapshot_manifest: range key too long");
        }
        if (files.size() > k_max_files) {
            throw snapshot_manifest_error("snapshot_manifest: too many files");
        }
        std::vector<std::string_view> names;
        names.reserve(files.size());
        for (const auto& f : files) {
            if (!valid_file_name(f.name)) {
                throw snapshot_manifest_error("snapshot_manifest: invalid file name");
            }
            names.emplace_back(f.name);
        }
        std::ranges::sort(names);
        if (std::ranges::adjacent_find(names) != names.end()) {
            throw snapshot_manifest_error("snapshot_manifest: duplicate file name");
        }
    }

    /// Little-endian binary encoding:
    /// "KYMF" u16 version, u16 len + engine_format, u32 len + range.start,
    /// u8 has_end [u32 len + range.end], u32 file count, then per file
    /// u16 len + name, u64 size, 32-byte SHA-256.
    [[nodiscard]] auto encode() const -> std::string {
        validate();
        std::string out;
        out.append("KYMF");
        put_u16(out, k_encoding_version);
        put_u16(out, static_cast<std::uint16_t>(engine_format.size()));
        out.append(engine_format);
        put_u32(out, static_cast<std::uint32_t>(range.start.size()));
        out.append(range.start);
        out.push_back(range.end.has_value() ? '\1' : '\0');
        if (range.end.has_value()) {
            put_u32(out, static_cast<std::uint32_t>(range.end->size()));
            out.append(*range.end);
        }
        put_u32(out, static_cast<std::uint32_t>(files.size()));
        for (const auto& f : files) {
            put_u16(out, static_cast<std::uint16_t>(f.name.size()));
            out.append(f.name);
            put_u64(out, f.size);
            out.append(reinterpret_cast<const char*>(f.sha256.data()), f.sha256.size());
        }
        if (out.size() > k_max_encoded_bytes) {
            throw snapshot_manifest_error("snapshot_manifest: encoding exceeds the size cap");
        }
        return out;
    }

    /// Inverse of `encode`. Every length is checked against the bytes left
    /// and against the caps before anything is allocated; trailing bytes are
    /// an error.
    [[nodiscard]] static auto decode(std::string_view in) -> snapshot_manifest {
        if (in.size() > k_max_encoded_bytes) {
            throw snapshot_manifest_error("snapshot_manifest: encoding exceeds the size cap");
        }
        reader r{in};
        if (r.take(4) != "KYMF") {
            throw snapshot_manifest_error("snapshot_manifest: bad magic");
        }
        if (r.u16() != k_encoding_version) {
            throw snapshot_manifest_error("snapshot_manifest: unsupported encoding version");
        }
        snapshot_manifest m;
        const auto format_len = r.u16();
        if (format_len > k_max_format_bytes) {
            throw snapshot_manifest_error("snapshot_manifest: bad engine_format length");
        }
        m.engine_format = std::string(r.take(format_len));
        const auto start_len = r.u32();
        if (start_len > k_max_range_key_bytes) {
            throw snapshot_manifest_error("snapshot_manifest: range key too long");
        }
        m.range.start = std::string(r.take(start_len));
        const auto has_end = r.u8();
        if (has_end > 1) {
            throw snapshot_manifest_error("snapshot_manifest: bad range end flag");
        }
        if (has_end == 1) {
            const auto end_len = r.u32();
            if (end_len > k_max_range_key_bytes) {
                throw snapshot_manifest_error("snapshot_manifest: range key too long");
            }
            m.range.end = std::string(r.take(end_len));
        }
        const auto count = r.u32();
        // Each file needs at least 2 + 1 + 8 + 32 bytes, so a count the
        // remaining input cannot hold is refused before reserving.
        constexpr std::size_t k_min_file_bytes = 2 + 1 + 8 + 32;
        if (count > k_max_files || count > r.remaining() / k_min_file_bytes) {
            throw snapshot_manifest_error("snapshot_manifest: bad file count");
        }
        m.files.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            checkpoint_file f;
            const auto name_len = r.u16();
            if (name_len > k_max_name_bytes) {
                throw snapshot_manifest_error("snapshot_manifest: invalid file name");
            }
            f.name = std::string(r.take(name_len));
            f.size = r.u64();
            const auto sha = r.take(f.sha256.size());
            std::copy(sha.begin(), sha.end(), reinterpret_cast<char*>(f.sha256.data()));
            m.files.push_back(std::move(f));
        }
        if (r.remaining() != 0) {
            throw snapshot_manifest_error("snapshot_manifest: trailing bytes");
        }
        m.validate();
        return m;
    }

    /// SHA-256 of the encoding: the "digest of the list itself" a resumed
    /// transfer is matched on (file-install-snapshot Requirement 6).
    [[nodiscard]] auto digest() const -> sha256_digest { return sha256_of(encode()); }

private:
    static auto put_u16(std::string& out, std::uint16_t v) -> void {
        for (int i = 0; i < 2; ++i) {
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
        }
    }
    static auto put_u32(std::string& out, std::uint32_t v) -> void {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
        }
    }
    static auto put_u64(std::string& out, std::uint64_t v) -> void {
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
        }
    }

    struct reader {
        std::string_view in;
        std::size_t pos = 0;

        [[nodiscard]] auto remaining() const noexcept -> std::size_t { return in.size() - pos; }

        auto take(std::size_t n) -> std::string_view {
            if (n > remaining()) {
                throw snapshot_manifest_error("snapshot_manifest: truncated");
            }
            auto out = in.substr(pos, n);
            pos += n;
            return out;
        }
        auto uint(std::size_t width) -> std::uint64_t {
            const auto bytes = take(width);
            std::uint64_t v = 0;
            for (std::size_t i = 0; i < width; ++i) {
                v |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
            }
            return v;
        }
        auto u8() -> std::uint8_t { return static_cast<std::uint8_t>(uint(1)); }
        auto u16() -> std::uint16_t { return static_cast<std::uint16_t>(uint(2)); }
        auto u32() -> std::uint32_t { return static_cast<std::uint32_t>(uint(4)); }
        auto u64() -> std::uint64_t { return uint(8); }
    };
};

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

/// One ordered, durable key-value store per node (Requirement 1.1).
///
/// - `get` and `scan` read the latest committed state; `scan` visits keys of
///   one column family inside `range` in either direction and returns how
///   many it visited.
/// - `write` commits a batch atomically and returns its sequence.
/// - `durable_sequence()` is the highest sequence that survives a power cut;
///   `sync(seq)` blocks until it is at least `seq` (a `seq` above
///   `latest_sequence()` syncs everything written so far).
/// - `checkpoint` writes an immutable copy of every column family's keys in
///   `range` to the new directory `dir` and describes it; `ingest` replaces
///   the keys of `range` with a checkpoint's after checking every file's size
///   and SHA-256 and the format tag; `drop` deletes the keys of `range`.
///   `ingest` and `drop` are writes and return their sequence.
template<typename E>
concept kv_engine = requires(E& e, const E& ce, column_family cf, const std::string& key,
                             const key_range& range, const write_batch& batch, engine_sequence seq,
                             const std::filesystem::path& dir, const snapshot_manifest& manifest,
                             scan_direction direction, const scan_visitor& visit) {
    { ce.get(cf, key) } -> std::same_as<std::optional<std::string>>;
    { ce.scan(cf, range, direction, visit) } -> std::same_as<std::size_t>;
    { e.write(batch) } -> std::same_as<engine_sequence>;
    { ce.latest_sequence() } -> std::same_as<engine_sequence>;
    { ce.durable_sequence() } -> std::same_as<engine_sequence>;
    { e.sync(seq) } -> std::same_as<void>;
    { e.checkpoint(range, dir) } -> std::same_as<snapshot_manifest>;
    { e.ingest(range, dir, manifest) } -> std::same_as<engine_sequence>;
    { e.drop(range) } -> std::same_as<engine_sequence>;
    { ce.format_tag() } -> std::convertible_to<std::string_view>;
};

/// A state machine whose state lives in a `kv_engine` (Requirement 1.2).
///
/// - `apply(command, index)` writes the entry's effects and `index` as the
///   applied index in one batch (Requirement 2.1), and is a no-op for an
///   index at or below `applied_index()` (2.3).
/// - `durable_applied_index()` is the highest applied index the engine has
///   made durable; log compaction stops there (3.1).
/// - `open(engine, range)` attaches to a range of the node's engine without
///   reading it into memory (4.1).
/// - `checkpoint`, `ingest` and `drop` move or remove the whole range.
///
/// It does not require `get_state` or `restore_from_snapshot`.
template<typename SM, typename LogIndex = std::uint64_t>
concept persistent_state_machine =
    log_index<LogIndex> &&
    requires(SM& sm, const SM& csm, const std::vector<std::byte>& command, LogIndex index,
             typename SM::engine_type& engine, const key_range& range,
             const std::filesystem::path& dir, const snapshot_manifest& manifest) {
        requires kv_engine<typename SM::engine_type>;
        { sm.apply(command, index) } -> std::same_as<std::vector<std::byte>>;
        { csm.applied_index() } -> std::same_as<LogIndex>;
        { csm.durable_applied_index() } -> std::same_as<LogIndex>;
        { sm.open(engine, range) } -> std::same_as<void>;
        { sm.checkpoint(dir) } -> std::same_as<snapshot_manifest>;
        { sm.ingest(dir, manifest, index) } -> std::same_as<void>;
        { sm.drop() } -> std::same_as<void>;
    };

/// Range-only split and merge (Requirement 1.3): `split_at(keys)` narrows
/// this state machine's range to the part below the first key and returns
/// the child ranges in key order; `adopt(range)` widens it over an adjacent
/// range already in the same engine. Neither copies a key. `range()` is the
/// range it owns now, which is how a host replaying a split entry after a
/// crash sees that the cut already happened (`split_at` narrowed the range
/// and wrote the children's records in one batch).
template<typename SM, typename LogIndex = std::uint64_t>
concept persistent_splittable_state_machine =
    persistent_state_machine<SM, LogIndex> &&
    requires(SM& sm, const std::vector<std::string>& keys, const key_range& range) {
        { sm.split_at(keys) } -> std::same_as<std::vector<key_range>>;
        { sm.adopt(range) } -> std::same_as<void>;
        { std::as_const(sm).range() } -> std::convertible_to<const key_range&>;
    };

/// Several commands of one log entry in one batch (Requirement 8.1).
///
/// A wrapper that turns one entry into several inner commands (a
/// transaction's commit applies every write it locked) cannot call
/// `apply(command, index)` once per command: the second call has the same
/// index and is taken for a replay. Instead it stages each command into a
/// batch it owns, adds its own writes to that batch, and commits it once:
///
/// - `stage(command, batch)` adds the command's effects to `batch` and
///   returns the reply `apply` would. A later `stage` in the same entry sees
///   the effects of the earlier ones.
/// - `commit_staged(batch, index)` adds the applied index and writes the
///   batch to the engine as one atomic write.
/// - `discard_staged()` forgets what was staged, after a failure.
///
/// `key_prefix()` is the namespace every engine key of the group starts with,
/// so a wrapper can lay its own keys out under the same range.
template<typename SM, typename LogIndex = std::uint64_t>
concept persistent_staged_state_machine =
    persistent_splittable_state_machine<SM, LogIndex> &&
    requires(SM& sm, const SM& csm, const std::vector<std::byte>& command, write_batch& batch,
             LogIndex index) {
        { sm.stage(command, batch) } -> std::same_as<std::vector<std::byte>>;
        { sm.commit_staged(batch, index) } -> std::same_as<void>;
        { sm.discard_staged() } -> std::same_as<void>;
        { csm.key_prefix() } -> std::convertible_to<const std::string&>;
    };

/// What a persistent group saves as its snapshot's state: a marker, not the
/// state, which stays in the engine (Requirement 3.2). `node<Types>` checks
/// for it at restart, so byte-buffer state is never taken for metadata.
inline auto persistent_snapshot_marker() -> const std::vector<std::byte>& {
    static const std::vector<std::byte> marker = [] {
        constexpr std::string_view text = "kythira-persistent-snapshot/1";
        std::vector<std::byte> out;
        out.reserve(text.size());
        for (const char c : text) {
            out.push_back(static_cast<std::byte>(c));
        }
        return out;
    }();
    return marker;
}

/// The engine a persistent state machine opens on, or `std::monostate` for a
/// byte-buffer one, so a host can declare an engine field for either kind.
template<typename SM, typename LogIndex = std::uint64_t> struct persistent_engine_of {
    using type = std::monostate;
};
template<typename SM, typename LogIndex>
requires persistent_state_machine<SM, LogIndex>
struct persistent_engine_of<SM, LogIndex> {
    using type = typename SM::engine_type;
};
template<typename SM, typename LogIndex = std::uint64_t>
using persistent_engine_of_t = typename persistent_engine_of<SM, LogIndex>::type;

/// Which path `node<Types>` takes for a state machine type.
enum class state_machine_storage : std::uint8_t {
    /// Today's path: the state moves as one buffer (`get_state` /
    /// `restore_from_snapshot`).
    byte_buffer,
    /// The state stays in the engine (this header).
    persistent,
};

/// Persistent wins when a type satisfies both concepts (Requirement 1.5;
/// see the file comment for why).
template<typename SM, typename LogIndex = std::uint64_t>
inline constexpr state_machine_storage storage_kind_of =
    persistent_state_machine<SM, LogIndex> ? state_machine_storage::persistent
                                           : state_machine_storage::byte_buffer;

/// True for a type `node<Types>` runs on the byte-buffer path: it satisfies
/// `state_machine` and not `persistent_state_machine`.
template<typename SM, typename LogIndex = std::uint64_t>
concept byte_buffer_state_machine =
    state_machine<SM, LogIndex> &&
    storage_kind_of<SM, LogIndex> == state_machine_storage::byte_buffer;

}  // namespace kythira
