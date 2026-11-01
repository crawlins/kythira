// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file binary_file_persistence.hpp
/// @brief A binary, segmented, file-backed Raft persistence engine for the
///        disk-backed build cache (.kiro/specs/disk-backed-build-cache/ design
///        Component 4, Requirements 5.1 and 5.3).
///
/// `file_persistence.hpp` writes JSON lines with base64 commands and rewrites
/// the whole log on every truncation. That suits `chaos_node`, whose entries
/// are a few bytes. A cache entry carries up to a 16 MiB chunk, so this engine
/// writes entries as length-prefixed binary records into segment files and
/// frees space by deleting whole segments.
///
/// Layout under the engine's directory (one per Raft group,
/// `DATA_DIR/raft/<group>/` in the daemon):
///
/// ```
/// meta       magic, version, term, vote, log_start, crc32; rewritten atomically
/// log.<n>    segments: { u64 index, u64 term, u8 type, u32 len, bytes, u32 crc }*
/// snapshot   magic, version, last index/term, configuration, state, crc32
/// ```
///
/// All integers are big-endian, and every CRC is CRC-32 over the bytes before
/// it in the same record or file.
///
/// **What lives in RAM.** Per entry: its term, type, and where its record is
/// (segment, offset, length), about 40 bytes. Commands are read back from the
/// segment with `pread` when asked for. A cache log carries chunk bytes, and
/// holding them in RAM until compaction would make memory follow the bytes
/// written (Requirement 2.1). The page cache already holds recent records.
///
/// **The log is contiguous.** Raft only ever appends `last + 1`, after a
/// conflict truncation or a snapshot install has made room. So an append at
/// an index the log already holds is a no-op if the terms match (the log
/// matching property says the entries are then identical). If the terms
/// differ, it truncates from that index first, which is Raft's own rule for a
/// conflict. An append past `last + 1` into a non-empty log is a caller bug and
/// throws. An append to an empty log may start at any index (after
/// `truncate_log(1)` or a compaction that emptied it).
///
/// **Prefix deletion** (`delete_log_entries_before`, after a snapshot) first
/// records the new first index in `meta`, then deletes every segment whose
/// entries are all below it (Requirement 5.3). Entries below it in a segment
/// that is still partly live stay on disk, ignored on load, until that
/// segment goes too. A crash between the two steps leaves whole dead
/// segments, which the next load deletes.
///
/// **Suffix truncation** (`truncate_log`, on a conflict) deletes later
/// segments newest first, then cuts the segment holding the index, and syncs
/// before returning. A crash part way leaves a prefix of the old log, never a
/// gap. The sync is deliberate: a truncated entry from an old term that came
/// back after a crash would be a conflicting entry this node had already
/// thrown away.
///
/// **Torn tails.** A load stops at the first record of the *last* segment
/// that runs past the end of the file or fails its CRC, and truncates there.
/// That is the write-ahead-log rule for an append cut short by a crash. A bad
/// record in any earlier segment, or a bad `meta` or `snapshot`, is fatal:
/// those files were complete and synced before anything later was written,
/// so damage there is not a torn write and must not be repaired silently.
///
/// **Durability** follows `file_persistence_engine`'s barrier contract
/// (`barriered_persistence_engine`, `.kiro/specs/durable-append-barrier/`).
/// Appends are written with `pwrite` under the engine mutex and assigned a
/// sequence in the same critical section. `barrier_through` samples the
/// highest sequence and the set of segments written since the last barrier,
/// releases the mutex, fsyncs them (and the directory if a segment was
/// created), and only then publishes the sample. Concurrent callers coalesce
/// onto one fsync. Term, vote, `meta` and the snapshot are synchronous.
///
/// The engine is movable (the group factory returns it by value), which is
/// why its state is behind a `unique_ptr`. Moving one that is in use is not
/// supported.

#include <raft/crc32.hpp>
#include <raft/exceptions.hpp>
#include <raft/fault_injection.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kythira {

struct binary_file_persistence_options {
    /// A segment stops taking appends once it reaches this size. Prefix
    /// deletion frees space a segment at a time, so this is also the most
    /// dead log a group can hold on disk beyond the snapshot point.
    std::size_t _segment_max_bytes = std::size_t{64} * 1024 * 1024;
    /// `false` makes `barrier_through` a no-op and `durability()` answer
    /// `buffered`, as `file_persistence_engine`'s flag does. Benchmarks only.
    bool _barriers_enabled = true;
};

namespace detail::binary_persistence {

inline constexpr std::uint32_t meta_magic = 0x4B42464DU;      // "KBFM"
inline constexpr std::uint32_t snapshot_magic = 0x4B424653U;  // "KBFS"
inline constexpr std::uint32_t format_version = 1;
/// u64 index, u64 term, u8 type, u32 len.
inline constexpr std::size_t record_header_bytes = 8 + 8 + 1 + 4;
inline constexpr std::size_t record_overhead = record_header_bytes + 4;
/// The largest command a record may claim. A cache entry is at most one
/// 16 MiB chunk plus a little framing; anything near 4 GiB is garbage.
inline constexpr std::uint32_t max_record_len = std::uint32_t{1} << 30U;

class writer {
public:
    void u8(std::uint8_t v) { _out.push_back(static_cast<std::byte>(v)); }
    void u32(std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) {
            _out.push_back(static_cast<std::byte>(v >> static_cast<unsigned>(s)));
        }
    }
    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v >> 32U));
        u32(static_cast<std::uint32_t>(v));
    }
    void bytes(std::span<const std::byte> b) { _out.insert(_out.end(), b.begin(), b.end()); }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        bytes(std::as_bytes(std::span(s.data(), s.size())));
    }
    void crc() { u32(crc32(_out)); }
    [[nodiscard]] auto data() -> std::vector<std::byte>& { return _out; }

private:
    std::vector<std::byte> _out;
};

class reader {
public:
    explicit reader(std::span<const std::byte> in) : _in(in) {}
    [[nodiscard]] auto u8() -> std::uint8_t { return static_cast<std::uint8_t>(take(1)[0]); }
    [[nodiscard]] auto u32() -> std::uint32_t {
        const auto b = take(4);
        return (static_cast<std::uint32_t>(b[0]) << 24U) |
               (static_cast<std::uint32_t>(b[1]) << 16U) |
               (static_cast<std::uint32_t>(b[2]) << 8U) | static_cast<std::uint32_t>(b[3]);
    }
    [[nodiscard]] auto u64() -> std::uint64_t {
        const std::uint64_t hi = u32();
        return (hi << 32U) | u32();
    }
    [[nodiscard]] auto bytes(std::size_t n) -> std::span<const std::byte> { return take(n); }
    [[nodiscard]] auto str() -> std::string {
        const auto n = u32();
        const auto b = take(n);
        return {reinterpret_cast<const char*>(b.data()), b.size()};
    }
    [[nodiscard]] auto position() const noexcept -> std::size_t { return _pos; }

private:
    auto take(std::size_t n) -> std::span<const std::byte> {
        if (n > _in.size() - _pos) {
            throw persistence_exception("binary_file_persistence: truncated field");
        }
        auto out = _in.subspan(_pos, n);
        _pos += n;
        return out;
    }
    std::span<const std::byte> _in;
    std::size_t _pos = 0;
};

/// An fd that closes when the last holder lets go, so a barrier can fsync a
/// segment that a concurrent prefix deletion has just unlinked.
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

[[nodiscard]] inline auto errno_text(const std::string& what) -> std::string {
    return "binary_file_persistence: " + what + ": " + std::strerror(errno);
}

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
            throw persistence_exception(errno_text("write to " + path.string()));
        }
        done += static_cast<std::size_t>(n);
    }
}

[[nodiscard]] inline auto pread_all(int fd, std::span<std::byte> out, std::uint64_t offset)
    -> bool {
    std::size_t done = 0;
    while (done < out.size()) {
        const auto n =
            ::pread(fd, out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
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

[[nodiscard]] inline auto fsync_path(const std::filesystem::path& p, int flags) -> bool {
    const int fd = ::open(p.c_str(), flags | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool ok = ::fsync(fd) == 0 || errno == EINVAL;
    ::close(fd);
    return ok;
}

[[nodiscard]] inline auto read_whole(const std::filesystem::path& p)
    -> std::optional<std::vector<std::byte>> {
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return std::nullopt;
        }
        throw persistence_exception(errno_text("open(" + p.string() + ")"));
    }
    struct ::stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        throw persistence_exception(errno_text("fstat(" + p.string() + ")"));
    }
    std::vector<std::byte> out(static_cast<std::size_t>(st.st_size));
    const bool ok = pread_all(fd, out, 0);
    ::close(fd);
    if (!ok) {
        throw persistence_exception("binary_file_persistence: short read of " + p.string());
    }
    return out;
}

/// Write `bytes` to `path` so that a reader sees the old file or the new one,
/// never a mix, and the new one survives a crash once this returns.
inline void atomic_replace(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    auto tmp = path;
    tmp += ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
    if (fd < 0) {
        throw persistence_exception(errno_text("create " + tmp.string()));
    }
    try {
        pwrite_all(fd, bytes, 0, tmp);
        if (::fsync(fd) != 0) {
            throw persistence_exception(errno_text("fsync(" + tmp.string() + ")"));
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        throw persistence_exception(errno_text("rename(" + path.string() + ")"));
    }
    if (!fsync_path(path.parent_path(), O_RDONLY | O_DIRECTORY)) {
        throw persistence_exception(errno_text("fsync(" + path.parent_path().string() + ")"));
    }
}

template<typename NodeId> void write_node_id(writer& w, const NodeId& id) {
    if constexpr (std::unsigned_integral<NodeId>) {
        w.u64(static_cast<std::uint64_t>(id));
    } else if constexpr (std::same_as<NodeId, std::string>) {
        w.str(id);
    } else {
        w.str(id.to_string());
    }
}

template<typename NodeId> [[nodiscard]] auto read_node_id(reader& r) -> NodeId {
    if constexpr (std::unsigned_integral<NodeId>) {
        return static_cast<NodeId>(r.u64());
    } else if constexpr (std::same_as<NodeId, std::string>) {
        return r.str();
    } else {
        auto parsed = NodeId::parse(r.str());
        if (!parsed) {
            throw persistence_exception("binary_file_persistence: unparseable node id");
        }
        return *parsed;
    }
}

template<typename NodeId> void write_node_list(writer& w, const std::vector<NodeId>& ids) {
    w.u32(static_cast<std::uint32_t>(ids.size()));
    for (const auto& id : ids) {
        write_node_id(w, id);
    }
}

template<typename NodeId> [[nodiscard]] auto read_node_list(reader& r) -> std::vector<NodeId> {
    const auto n = r.u32();
    std::vector<NodeId> out;
    out.reserve(std::min<std::uint32_t>(n, 1024));
    for (std::uint32_t i = 0; i < n; ++i) {
        out.push_back(read_node_id<NodeId>(r));
    }
    return out;
}

}  // namespace detail::binary_persistence

template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
class binary_file_persistence_engine {
public:
    using log_entry_t = log_entry<TermId, LogIndex>;
    using snapshot_t = snapshot<NodeId, TermId, LogIndex>;

    explicit binary_file_persistence_engine(std::filesystem::path dir,
                                            binary_file_persistence_options options = {})
        : _s(std::make_unique<state>(std::move(dir), options)) {}

    binary_file_persistence_engine(binary_file_persistence_engine&&) noexcept = default;
    auto operator=(binary_file_persistence_engine&&) noexcept
        -> binary_file_persistence_engine& = default;
    binary_file_persistence_engine(const binary_file_persistence_engine&) = delete;
    auto operator=(const binary_file_persistence_engine&)
        -> binary_file_persistence_engine& = delete;
    ~binary_file_persistence_engine() = default;

    // ---- term and vote ----------------------------------------------------

    auto save_current_term(TermId term) -> void {
        fiu_do_on("raft/persistence/save_current_term",
                  throw std::runtime_error("chaos: save_current_term"););
        const std::lock_guard lock(_s->_mu);
        _s->write_meta(term, _s->_voted_for, _s->_log_start);
    }

    auto load_current_term() -> TermId {
        const std::lock_guard lock(_s->_mu);
        return _s->_current_term;
    }

    auto save_voted_for(NodeId node) -> void {
        fiu_do_on("raft/persistence/save_voted_for",
                  throw std::runtime_error("chaos: save_voted_for"););
        const std::lock_guard lock(_s->_mu);
        _s->write_meta(_s->_current_term, std::optional<NodeId>(std::move(node)), _s->_log_start);
    }

    auto load_voted_for() -> std::optional<NodeId> {
        const std::lock_guard lock(_s->_mu);
        return _s->_voted_for;
    }

    /// `hard_state_persistence_engine`: term and vote in one atomic `meta`
    /// write, skipped when neither changed.
    auto save_hard_state(TermId term, std::optional<NodeId> vote) -> void {
        fiu_do_on("raft/persistence/save_current_term",
                  throw std::runtime_error("chaos: save_current_term"););
        const std::lock_guard lock(_s->_mu);
        if (vote != _s->_voted_for) {
            fiu_do_on("raft/persistence/save_voted_for",
                      throw std::runtime_error("chaos: save_voted_for"););
        }
        if (term == _s->_current_term && vote == _s->_voted_for) {
            return;
        }
        _s->write_meta(term, std::move(vote), _s->_log_start);
    }

    // ---- log --------------------------------------------------------------

    auto append_log_entry(const log_entry_t& entry) -> void {
        (void)append_log_entry_sequenced(entry);
    }

    /// Append and return the sequence assigned to the bytes, in the same
    /// critical section that wrote them (the first half of the barrier's
    /// ordering rule).
    auto append_log_entry_sequenced(const log_entry_t& entry) -> write_sequence {
        fiu_do_on("raft/persistence/append_log_entry",
                  throw std::runtime_error("chaos: append_log_entry"););
        const std::lock_guard lock(_s->_mu);
        _s->append_locked(entry);
        return ++_s->_write_seq;
    }

    auto get_log_entry(LogIndex index) -> std::optional<log_entry_t> {
        const std::lock_guard lock(_s->_mu);
        return _s->read_locked(index);
    }

    auto get_log_entries(LogIndex start, LogIndex end) -> std::vector<log_entry_t> {
        const std::lock_guard lock(_s->_mu);
        std::vector<log_entry_t> out;
        if (_s->_entries.empty() || end < start) {
            return out;
        }
        const LogIndex lo = std::max(start, _s->_first_index);
        const LogIndex hi = std::min(end, _s->last_index());
        for (LogIndex i = lo; i <= hi && lo <= hi; ++i) {
            if (auto e = _s->read_locked(i)) {
                out.push_back(std::move(*e));
            }
        }
        return out;
    }

    auto get_last_log_index() -> LogIndex {
        const std::lock_guard lock(_s->_mu);
        return _s->_entries.empty() ? LogIndex{0} : _s->last_index();
    }

    /// Delete every entry at or after `index`, durably.
    auto truncate_log(LogIndex index) -> void {
        fiu_do_on("raft/persistence/truncate_log",
                  throw std::runtime_error("chaos: truncate_log"););
        const std::lock_guard lock(_s->_mu);
        _s->truncate_suffix_locked(index);
    }

    /// Delete every entry before `index` (Requirement 5.3).
    auto delete_log_entries_before(LogIndex index) -> void {
        const std::lock_guard lock(_s->_mu);
        _s->delete_prefix_locked(index);
    }

    // ---- snapshot ---------------------------------------------------------

    auto save_snapshot(const snapshot_t& snap) -> void {
        fiu_do_on("raft/persistence/save_snapshot",
                  throw std::runtime_error("chaos: save_snapshot"););
        namespace bp = detail::binary_persistence;
        bp::writer w;
        w.u32(bp::snapshot_magic);
        w.u32(bp::format_version);
        w.u64(static_cast<std::uint64_t>(snap.last_included_index()));
        w.u64(static_cast<std::uint64_t>(snap.last_included_term()));
        const auto& cfg = snap.configuration();
        bp::write_node_list(w, cfg.nodes());
        w.u8(cfg.is_joint_consensus() ? 1 : 0);
        w.u8(cfg.old_nodes() ? 1 : 0);
        if (cfg.old_nodes()) {
            bp::write_node_list(w, *cfg.old_nodes());
        }
        bp::write_node_list(w, cfg.learners());
        w.u64(snap.state_machine_state().size());
        w.bytes(snap.state_machine_state());
        w.crc();
        const std::lock_guard lock(_s->_mu);
        bp::atomic_replace(_s->_dir / "snapshot", w.data());
        _s->_snapshot = snap;
    }

    auto load_snapshot() -> std::optional<snapshot_t> {
        const std::lock_guard lock(_s->_mu);
        return _s->_snapshot;
    }

    // ---- barriered_persistence_engine -------------------------------------

    /// Block until a barrier has covered every append through `seq`,
    /// coalescing with concurrent callers. Throws `persistence_exception` if
    /// the barrier failed; the caller must not advertise the append.
    auto barrier_through(write_sequence seq) -> void {
        if (!_s->_options._barriers_enabled) {
            return;
        }
        std::unique_lock lock(_s->_mu);
        if (seq > _s->_write_seq) {
            throw persistence_exception("binary_file_persistence: barrier for sequence " +
                                        std::to_string(seq) + " that was never assigned");
        }
        while (_s->_durable_seq < seq) {
            if (_s->_barrier_running) {
                _s->_barrier_cv.wait(lock);
                continue;
            }
            // Every append is in its segment the moment its sequence exists,
            // so the highest assigned sequence is the sample, taken BEFORE the
            // syscalls and published AFTER them.
            const write_sequence sampled = _s->_write_seq;
            std::vector<std::shared_ptr<detail::binary_persistence::file_handle>> files;
            files.reserve(_s->_dirty.size());
            for (const auto n : _s->_dirty) {
                if (auto it = _s->_segments.find(n); it != _s->_segments.end()) {
                    files.push_back(it->second._file);
                }
            }
            const bool dir_dirty = _s->_dir_dirty;
            _s->_dirty.clear();
            _s->_dir_dirty = false;
            _s->_barrier_running = true;
            ++_s->_barriers_issued;
            lock.unlock();
            bool ok = true;
            fiu_do_on("raft/persistence/barrier", ok = false;);
            for (const auto& f : files) {
                ok = ok && ::fsync(f->fd()) == 0;
            }
            if (ok && dir_dirty) {
                ok = detail::binary_persistence::fsync_path(_s->_dir, O_RDONLY | O_DIRECTORY);
            }
            lock.lock();
            _s->_barrier_running = false;
            if (ok) {
                _s->_durable_seq = std::max(_s->_durable_seq, sampled);
            } else {
                // Nothing was proven durable. The next barrier must sync the
                // same files again.
                for (const auto& f : files) {
                    for (auto& [n, seg] : _s->_segments) {
                        if (seg._file == f) {
                            _s->_dirty.insert(n);
                        }
                    }
                }
                _s->_dir_dirty = _s->_dir_dirty || dir_dirty;
            }
            _s->_barrier_cv.notify_all();
            if (!ok) {
                throw persistence_exception(
                    "binary_file_persistence: durability barrier failed "
                    "for " +
                    _s->_dir.string());
            }
        }
    }

    [[nodiscard]] auto durable_through() const -> write_sequence {
        const std::lock_guard lock(_s->_mu);
        return _s->_durable_seq;
    }

    [[nodiscard]] auto durability() const -> durability_class {
        return _s->_options._barriers_enabled ? durability_class::barrier
                                              : durability_class::buffered;
    }

    /// fsync rounds actually issued; shows that concurrent barriers coalesce.
    [[nodiscard]] auto barriers_issued() const -> std::uint64_t {
        const std::lock_guard lock(_s->_mu);
        return _s->_barriers_issued;
    }

    /// Segment files on disk; for tests and `INFO`.
    [[nodiscard]] auto segment_count() const -> std::size_t {
        const std::lock_guard lock(_s->_mu);
        return _s->_segments.size();
    }

    [[nodiscard]] auto directory() const -> const std::filesystem::path& { return _s->_dir; }

private:
    struct entry_location {
        TermId _term{};
        entry_type _type = entry_type::normal;
        std::uint64_t _segment = 0;
        std::uint64_t _offset = 0;
        std::uint32_t _len = 0;
    };

    struct segment {
        std::shared_ptr<detail::binary_persistence::file_handle> _file;
        std::uint64_t _size = 0;
    };

    struct state {
        state(std::filesystem::path dir, binary_file_persistence_options options)
            : _dir(std::move(dir)), _options(options) {
            if (_options._segment_max_bytes == 0) {
                throw std::invalid_argument("binary_file_persistence: segment size of zero");
            }
            std::filesystem::create_directories(_dir);
            load_meta();
            load_snapshot_file();
            load_segments();
        }

        std::filesystem::path _dir;
        binary_file_persistence_options _options;
        mutable std::mutex _mu;

        TermId _current_term{0};
        std::optional<NodeId> _voted_for;
        /// The first index the log may hold; records below it are dead.
        LogIndex _log_start{0};
        std::optional<snapshot_t> _snapshot;

        /// `_entries[i]` is index `_first_index + i`.
        std::deque<entry_location> _entries;
        LogIndex _first_index{0};
        std::map<std::uint64_t, segment> _segments;
        std::uint64_t _next_segment = 1;

        /// Segments written since the last barrier, and whether a segment
        /// was created (a name the directory fsync must make durable).
        std::set<std::uint64_t> _dirty;
        bool _dir_dirty = false;
        write_sequence _write_seq{0};
        write_sequence _durable_seq{0};
        bool _barrier_running = false;
        std::uint64_t _barriers_issued = 0;
        std::condition_variable _barrier_cv;

        [[nodiscard]] auto last_index() const -> LogIndex {
            return static_cast<LogIndex>(_first_index + _entries.size() - 1);
        }

        [[nodiscard]] auto segment_path(std::uint64_t n) const -> std::filesystem::path {
            return _dir / ("log." + std::to_string(n));
        }

        // ---- meta ---------------------------------------------------------

        void write_meta(TermId term, std::optional<NodeId> vote, LogIndex log_start) {
            namespace bp = detail::binary_persistence;
            bp::writer w;
            w.u32(bp::meta_magic);
            w.u32(bp::format_version);
            w.u64(static_cast<std::uint64_t>(term));
            w.u8(vote ? 1 : 0);
            if (vote) {
                bp::write_node_id(w, *vote);
            }
            w.u64(static_cast<std::uint64_t>(log_start));
            w.crc();
            bp::atomic_replace(_dir / "meta", w.data());
            _current_term = term;
            _voted_for = std::move(vote);
            _log_start = log_start;
        }

        /// A file that ends in a CRC over everything before it.
        [[nodiscard]] static auto checked(const std::vector<std::byte>& bytes,
                                          const std::filesystem::path& path)
            -> std::span<const std::byte> {
            if (bytes.size() < 12) {
                throw persistence_exception("binary_file_persistence: " + path.string() +
                                            " is too short");
            }
            const std::span<const std::byte> body(bytes.data(), bytes.size() - 4);
            detail::binary_persistence::reader tail(
                std::span<const std::byte>(bytes.data() + body.size(), 4));
            if (tail.u32() != crc32(body)) {
                throw persistence_exception("binary_file_persistence: " + path.string() +
                                            " fails its CRC");
            }
            return body;
        }

        static void check_header(detail::binary_persistence::reader& r, std::uint32_t magic,
                                 const std::filesystem::path& path) {
            if (r.u32() != magic || r.u32() != detail::binary_persistence::format_version) {
                throw persistence_exception("binary_file_persistence: " + path.string() +
                                            " has an unknown format");
            }
        }

        void load_meta() {
            const auto path = _dir / "meta";
            auto bytes = detail::binary_persistence::read_whole(path);
            if (!bytes) {
                return;
            }
            detail::binary_persistence::reader r(checked(*bytes, path));
            check_header(r, detail::binary_persistence::meta_magic, path);
            _current_term = static_cast<TermId>(r.u64());
            if (r.u8() != 0) {
                _voted_for = detail::binary_persistence::read_node_id<NodeId>(r);
            }
            _log_start = static_cast<LogIndex>(r.u64());
        }

        void load_snapshot_file() {
            namespace bp = detail::binary_persistence;
            const auto path = _dir / "snapshot";
            auto bytes = bp::read_whole(path);
            if (!bytes) {
                return;
            }
            bp::reader r(checked(*bytes, path));
            check_header(r, bp::snapshot_magic, path);
            snapshot_t snap{};
            snap._last_included_index = static_cast<LogIndex>(r.u64());
            snap._last_included_term = static_cast<TermId>(r.u64());
            snap._configuration._nodes = bp::read_node_list<NodeId>(r);
            snap._configuration._is_joint_consensus = r.u8() != 0;
            if (r.u8() != 0) {
                snap._configuration._old_nodes = bp::read_node_list<NodeId>(r);
            }
            snap._configuration._learners = bp::read_node_list<NodeId>(r);
            const auto len = r.u64();
            const auto state_bytes = r.bytes(static_cast<std::size_t>(len));
            snap._state_machine_state.assign(state_bytes.begin(), state_bytes.end());
            _snapshot = std::move(snap);
        }

        // ---- segments -----------------------------------------------------

        void load_segments() {
            std::vector<std::uint64_t> numbers;
            for (const auto& e : std::filesystem::directory_iterator(_dir)) {
                const auto name = e.path().filename().string();
                if (!name.starts_with("log.")) {
                    continue;
                }
                std::uint64_t n = 0;
                const auto* first = name.data() + 4;
                const auto* last = name.data() + name.size();
                const auto [end, ec] = std::from_chars(first, last, n);
                if (ec == std::errc{} && end == last) {
                    numbers.push_back(n);
                }
            }
            std::sort(numbers.begin(), numbers.end());
            for (std::size_t i = 0; i < numbers.size(); ++i) {
                scan_segment(numbers[i], i + 1 == numbers.size());
            }
            if (!numbers.empty()) {
                _next_segment = numbers.back() + 1;
            }
            // Segments left holding nothing live (a crash between the meta
            // write and the deletes of a prefix deletion) go now. The last
            // one stays: it is where appends continue.
            std::vector<std::uint64_t> empty;
            for (const auto& [n, seg] : _segments) {
                if (n != _segments.rbegin()->first && !segment_has_live_entry(n)) {
                    empty.push_back(n);
                }
            }
            for (const auto n : empty) {
                remove_segment(n);
            }
            if (!empty.empty() &&
                !detail::binary_persistence::fsync_path(_dir, O_RDONLY | O_DIRECTORY)) {
                throw persistence_exception(
                    detail::binary_persistence::errno_text("fsync(" + _dir.string() + ")"));
            }
        }

        [[nodiscard]] auto segment_has_live_entry(std::uint64_t n) const -> bool {
            return std::any_of(_entries.begin(), _entries.end(),
                               [n](const entry_location& e) { return e._segment == n; });
        }

        void scan_segment(std::uint64_t n, bool is_last) {
            namespace bp = detail::binary_persistence;
            const auto path = segment_path(n);
            const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                throw persistence_exception(bp::errno_text("open(" + path.string() + ")"));
            }
            auto file = std::make_shared<bp::file_handle>(fd);
            struct ::stat st{};
            if (::fstat(fd, &st) != 0) {
                throw persistence_exception(bp::errno_text("fstat(" + path.string() + ")"));
            }
            const auto size = static_cast<std::uint64_t>(st.st_size);
            std::uint64_t off = 0;
            std::vector<std::byte> record;
            while (off < size) {
                std::array<std::byte, bp::record_header_bytes> header{};
                bool ok = off + bp::record_overhead <= size && bp::pread_all(fd, header, off);
                std::uint32_t len = 0;
                if (ok) {
                    bp::reader h(header);
                    (void)h.u64();
                    (void)h.u64();
                    (void)h.u8();
                    len = h.u32();
                    ok = len <= bp::max_record_len && off + bp::record_overhead + len <= size;
                }
                if (ok) {
                    record.resize(bp::record_overhead + len);
                    ok = bp::pread_all(fd, record, off);
                }
                if (ok) {
                    bp::reader t(std::span<const std::byte>(
                        record.data() + bp::record_header_bytes + len, 4));
                    ok = t.u32() == crc32(std::span<const std::byte>(
                                        record.data(), bp::record_header_bytes + len));
                }
                if (!ok) {
                    if (!is_last) {
                        throw persistence_exception(
                            "binary_file_persistence: corrupt record at offset " +
                            std::to_string(off) + " of " + path.string() +
                            ", which is not the last segment");
                    }
                    // Torn tail of the last segment: cut it off.
                    if (::ftruncate(fd, static_cast<off_t>(off)) != 0 || ::fsync(fd) != 0) {
                        throw persistence_exception(
                            bp::errno_text("truncate(" + path.string() + ")"));
                    }
                    break;
                }
                bp::reader h(record);
                const auto index = static_cast<LogIndex>(h.u64());
                const auto term = static_cast<TermId>(h.u64());
                const auto type = static_cast<entry_type>(h.u8());
                if (index >= _log_start) {
                    index_loaded_record(path, off, index, term, type, len, n);
                }
                off += bp::record_overhead + len;
            }
            _segments.emplace(n, segment{._file = std::move(file), ._size = off});
        }

        void index_loaded_record(const std::filesystem::path& path, std::uint64_t off,
                                 LogIndex index, TermId term, entry_type type, std::uint32_t len,
                                 std::uint64_t n) {
            const entry_location loc{
                ._term = term, ._type = type, ._segment = n, ._offset = off, ._len = len};
            if (_entries.empty()) {
                _first_index = index;
                _entries.push_back(loc);
                return;
            }
            if (index != last_index() + 1) {
                // Truncation deletes later segments before cutting the one it
                // ends in, and appends are contiguous, so the records on disk
                // are always one run. Anything else is not a torn write.
                throw persistence_exception(
                    "binary_file_persistence: record for index " +
                    std::to_string(static_cast<std::uint64_t>(index)) + " at offset " +
                    std::to_string(off) + " of " + path.string() + " follows index " +
                    std::to_string(static_cast<std::uint64_t>(last_index())));
            }
            _entries.push_back(loc);
        }

        void remove_segment(std::uint64_t n) {
            const auto path = segment_path(n);
            if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
                throw persistence_exception(
                    detail::binary_persistence::errno_text("unlink(" + path.string() + ")"));
            }
            _segments.erase(n);
            _dirty.erase(n);
        }

        /// The segment appends go to, created or rolled as needed.
        auto open_segment() -> std::pair<std::uint64_t, segment*> {
            namespace bp = detail::binary_persistence;
            if (!_segments.empty()) {
                auto& [n, seg] = *_segments.rbegin();
                if (seg._size < _options._segment_max_bytes) {
                    return {n, &seg};
                }
            }
            const std::uint64_t n = _next_segment++;
            const auto path = segment_path(n);
            const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
            if (fd < 0) {
                throw persistence_exception(bp::errno_text("create " + path.string()));
            }
            auto [it, inserted] = _segments.emplace(
                n, segment{._file = std::make_shared<bp::file_handle>(fd), ._size = 0});
            _dir_dirty = true;
            return {n, &it->second};
        }

        void append_locked(const log_entry_t& entry) {
            namespace bp = detail::binary_persistence;
            const LogIndex index = entry.index();
            if (!_entries.empty() && index >= _first_index && index <= last_index()) {
                const auto& existing = _entries[static_cast<std::size_t>(index - _first_index)];
                if (existing._term == entry.term()) {
                    return;  // Same index and term: the same entry (log matching).
                }
                truncate_suffix_locked(index);
            }
            if (!_entries.empty() && index != last_index() + 1) {
                throw persistence_exception(
                    "binary_file_persistence: append of index " +
                    std::to_string(static_cast<std::uint64_t>(index)) +
                    " would leave a gap after " +
                    std::to_string(static_cast<std::uint64_t>(last_index())));
            }
            if (_entries.empty() && !_segments.empty()) {
                // The log is empty, so every record on disk is dead (below
                // `_log_start`, or truncated). Drop them before this append,
                // or lowering `_log_start` below would bring them back.
                while (!_segments.empty()) {
                    remove_segment(_segments.rbegin()->first);
                }
                if (!bp::fsync_path(_dir, O_RDONLY | O_DIRECTORY)) {
                    throw persistence_exception(bp::errno_text("fsync(" + _dir.string() + ")"));
                }
            }
            if (_entries.empty() && index < _log_start) {
                // A fresh log starting below the recorded first index (after
                // truncate_log(1) and a snapshot install at a lower point).
                // Record it first, or the next load would skip this record.
                write_meta(_current_term, _voted_for, index);
            }
            const auto& command = entry.command();
            if (command.size() > bp::max_record_len) {
                throw persistence_exception("binary_file_persistence: command of " +
                                            std::to_string(command.size()) + " bytes");
            }
            bp::writer w;
            w.u64(static_cast<std::uint64_t>(index));
            w.u64(static_cast<std::uint64_t>(entry.term()));
            w.u8(static_cast<std::uint8_t>(entry.type()));
            w.u32(static_cast<std::uint32_t>(command.size()));
            w.bytes(command);
            w.crc();
            auto [n, seg] = open_segment();
            const std::uint64_t offset = seg->_size;
            try {
                // A process killed here leaves a torn record for the next
                // load to cut off; the fault point stands in for that kill.
                fiu_do_on("raft/binary_persistence/torn_append", {
                    const auto half = w.data().size() / 2;
                    bp::pwrite_all(seg->_file->fd(),
                                   std::span<const std::byte>(w.data().data(), half), offset,
                                   segment_path(n));
                    throw persistence_exception("binary_file_persistence: injected torn append");
                });
                bp::pwrite_all(seg->_file->fd(), w.data(), offset, segment_path(n));
            } catch (...) {
                // Leave nothing for the next append to follow. If this fails
                // too, the next load's torn-tail rule cuts the same bytes.
                [[maybe_unused]] const int rc =
                    ::ftruncate(seg->_file->fd(), static_cast<off_t>(offset));
                throw;
            }
            seg->_size += w.data().size();
            _dirty.insert(n);
            if (_entries.empty()) {
                _first_index = index;
            }
            _entries.push_back(entry_location{._term = entry.term(),
                                              ._type = entry.type(),
                                              ._segment = n,
                                              ._offset = offset,
                                              ._len = static_cast<std::uint32_t>(command.size())});
        }

        [[nodiscard]] auto read_locked(LogIndex index) const -> std::optional<log_entry_t> {
            namespace bp = detail::binary_persistence;
            if (_entries.empty() || index < _first_index || index > last_index()) {
                return std::nullopt;
            }
            const auto& loc = _entries[static_cast<std::size_t>(index - _first_index)];
            const auto& seg = _segments.at(loc._segment);
            std::vector<std::byte> record(bp::record_overhead + loc._len);
            if (!bp::pread_all(seg._file->fd(), record, loc._offset)) {
                throw persistence_exception("binary_file_persistence: short read of " +
                                            segment_path(loc._segment).string());
            }
            bp::reader t(
                std::span<const std::byte>(record.data() + bp::record_header_bytes + loc._len, 4));
            if (t.u32() != crc32(std::span<const std::byte>(record.data(),
                                                            bp::record_header_bytes + loc._len))) {
                throw persistence_exception("binary_file_persistence: record for index " +
                                            std::to_string(static_cast<std::uint64_t>(index)) +
                                            " in " + segment_path(loc._segment).string() +
                                            " fails its CRC");
            }
            log_entry_t e{};
            e._index = index;
            e._term = loc._term;
            e._type = loc._type;
            const auto* body = record.data() + bp::record_header_bytes;
            e._command.assign(body, body + loc._len);
            return e;
        }

        void truncate_suffix_locked(LogIndex index) {
            namespace bp = detail::binary_persistence;
            if (_entries.empty() || index > last_index()) {
                return;
            }
            if (index <= _first_index) {
                // Everything goes. Newest first, so a crash leaves a prefix.
                while (!_segments.empty()) {
                    remove_segment(_segments.rbegin()->first);
                }
                _entries.clear();
                if (!bp::fsync_path(_dir, O_RDONLY | O_DIRECTORY)) {
                    throw persistence_exception(bp::errno_text("fsync(" + _dir.string() + ")"));
                }
                return;
            }
            const auto cut = _entries[static_cast<std::size_t>(index - _first_index)];
            bool removed_any = false;
            while (_segments.rbegin()->first > cut._segment) {
                remove_segment(_segments.rbegin()->first);
                removed_any = true;
            }
            if (removed_any && !bp::fsync_path(_dir, O_RDONLY | O_DIRECTORY)) {
                throw persistence_exception(bp::errno_text("fsync(" + _dir.string() + ")"));
            }
            auto& seg = _segments.at(cut._segment);
            if (::ftruncate(seg._file->fd(), static_cast<off_t>(cut._offset)) != 0 ||
                ::fsync(seg._file->fd()) != 0) {
                throw persistence_exception(
                    bp::errno_text("truncate(" + segment_path(cut._segment).string() + ")"));
            }
            seg._size = cut._offset;
            _entries.resize(static_cast<std::size_t>(index - _first_index));
        }

        void delete_prefix_locked(LogIndex index) {
            namespace bp = detail::binary_persistence;
            if (index <= _log_start && (_entries.empty() || index <= _first_index)) {
                return;
            }
            // Record the new first index before deleting anything: a crash
            // in between leaves dead records the load ignores, never a log
            // that starts lower than the snapshot assumed.
            if (index > _log_start) {
                write_meta(_current_term, _voted_for, index);
            }
            if (_entries.empty()) {
                return;
            }
            const auto drop = std::min<std::size_t>(
                _entries.size(),
                index > _first_index ? static_cast<std::size_t>(index - _first_index) : 0);
            _entries.erase(_entries.begin(), _entries.begin() + static_cast<std::ptrdiff_t>(drop));
            if (_entries.empty()) {
                _first_index = index;
            } else {
                _first_index = static_cast<LogIndex>(_first_index + drop);
            }
            // Whole segments below the first live entry go. With the log
            // emptied, every segment goes but the newest, where appends
            // continue (it may hold only dead records; the load skips them).
            const std::uint64_t keep_from =
                _entries.empty() ? (_segments.empty() ? 0 : _segments.rbegin()->first)
                                 : _entries.front()._segment;
            bool removed_any = false;
            while (!_segments.empty() && _segments.begin()->first < keep_from) {
                remove_segment(_segments.begin()->first);
                removed_any = true;
            }
            if (removed_any && !bp::fsync_path(_dir, O_RDONLY | O_DIRECTORY)) {
                throw persistence_exception(bp::errno_text("fsync(" + _dir.string() + ")"));
            }
        }
    };

    std::unique_ptr<state> _s;
};

}  // namespace kythira
