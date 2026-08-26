// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file shard_commands.hpp
/// @brief The payloads that ride inside multi-Raft administration log entries,
///        and their wire encoding.
///
/// See `.kiro/specs/multi-raft/` design §5.2.
///
/// ### The rule these payloads exist to enforce
///
/// **Everything the apply step needs is in the entry.** No replica recomputes
/// anything from local statistics, consults a policy, or calls the placement
/// driver at apply time. That is the single most important rule in the design,
/// and it is what lets the policy be non-deterministic (Requirement 6.3): its
/// output is frozen here, and every replica applies the frozen answer.
///
/// A payload that carried "split this shard in half" instead of "split at these
/// exact keys, into these exact children" would put the decision back on each
/// replica, and two replicas that measured themselves a moment apart would cut
/// in different places — with no Raft-level invariant noticing, because their
/// logs would still match.
///
/// ### Encoding
///
/// A small length-prefixed binary format rather than JSON. These payloads are
/// decoded on every replica on the apply path, and their contents — keys, node
/// ids — have no textual form the library is entitled to assume. Keys go
/// through the application's `shard_key_codec` for exactly that reason.

#include <raft/shard_exceptions.hpp>
#include <raft/shard_types.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace kythira {

// ─────────────────────────────────────────────────────────────────────────────
// Payloads
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The body of an `entry_type::split` log entry.
///
/// @tparam GroupId Must satisfy `raft_group_id`.
/// @tparam Key     Must satisfy `shard_key`.
/// @tparam NodeId  Must satisfy `node_id`.
template<raft_group_id GroupId, shard_key Key, typename NodeId = std::uint64_t>
requires node_id<NodeId>
struct split_command {
    using descriptor_type = shard_descriptor<GroupId, Key, NodeId>;

    GroupId _parent_group{};
    /// @brief The parent's epoch when the split was proposed.
    ///
    /// Re-checked at apply time, because an entry proposed under one epoch can
    /// commit after the epoch has moved (Requirement 3.7) — a configuration
    /// change that raced the split, say. A replica that applied it anyway would
    /// cut a shard whose membership the entry no longer describes.
    shard_epoch _parent_epoch{};
    /// Ordered, deduplicated, all strictly inside the parent's range.
    std::vector<Key> _at_keys{};
    /// @brief Fully derived children, in key order.
    ///
    /// Each child's members are the parent's members one-for-one: a child
    /// replica is created on exactly the machines that already hold a parent
    /// replica, so no data moves. That is range partitioning's stated
    /// advantage — "region splits and merges require only metadata changes".
    std::vector<descriptor_type> _children{};
    /// @brief Whether the rightmost child inherits the parent's group id, log
    /// and term (rather than the leftmost).
    bool _right_derive{false};
    split_reason _reason{split_reason::size};
    std::optional<std::uint64_t> _pd_operation_id{};

    /// @brief The child that reuses the parent's group id, or `nullopt` if the
    /// payload is malformed.
    [[nodiscard]] auto derived_child() const -> std::optional<descriptor_type> {
        for (const auto& c : _children) {
            if (c._group_id == _parent_group) {
                return c;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto operator==(const split_command& other) const -> bool {
        return _parent_group == other._parent_group && _parent_epoch == other._parent_epoch &&
               _at_keys == other._at_keys && _children == other._children &&
               _right_derive == other._right_derive && _reason == other._reason &&
               _pd_operation_id == other._pd_operation_id;
    }
};

/// @brief The body of an `entry_type::merge_prepare` log entry, in the SOURCE's log.
///
/// Applying it freezes the source: it stops accepting proposals and reads, and
/// refuses further configuration changes. From here the source is released only
/// by a `merge_rollback` in its own log, never by a timer — see
/// `merge_abandoned_command` for why.
///
/// It is also the **notification**. Design §5.5 has the source leader tell the
/// target leader out of band; this implementation does not need to, because
/// merge requires colocated replicas, so the machine leading the target
/// necessarily holds a source replica and learns of the merge by applying this
/// entry like everyone else. One fewer channel, and one fewer way for the two
/// sides to disagree.
template<raft_group_id GroupId, shard_key Key, typename NodeId = std::uint64_t>
requires node_id<NodeId>
struct merge_prepare_command {
    using descriptor_type = shard_descriptor<GroupId, Key, NodeId>;

    descriptor_type _source{};
    descriptor_type _target{};
    /// @brief The lowest index every source voter is known to hold.
    ///
    /// Bounds what `merge_commit` must carry: only the tail
    /// `(min_index, prepare_index]` travels, rather than the source's whole log
    /// or its whole state.
    std::uint64_t _min_index{0};
    merge_reason _reason{merge_reason::size};

    [[nodiscard]] auto operator==(const merge_prepare_command& other) const -> bool {
        return _source == other._source && _target == other._target &&
               _min_index == other._min_index && _reason == other._reason;
    }
};

/// @brief The body of an `entry_type::merge_commit` log entry, in the TARGET's log.
///
/// Carries the source's descriptor and the tail of its log that the target's
/// replicas may not have seen applied on their own local source replicas. Every
/// target replica force-applies that same tail before reading the source's
/// state, which is what makes the absorb deterministic across them: they all
/// read a source that stands at exactly the prepare index.
template<raft_group_id GroupId, shard_key Key, typename NodeId = std::uint64_t,
         typename LogEntry = log_entry<>>
requires node_id<NodeId>
struct merge_commit_command {
    using descriptor_type = shard_descriptor<GroupId, Key, NodeId>;

    descriptor_type _source{};
    descriptor_type _target{};
    /// The index the source stands at once the tail is applied.
    std::uint64_t _prepare_index{0};
    /// Entries `(min_index, prepare_index]` from the source's log, in order.
    std::vector<LogEntry> _entries{};

    [[nodiscard]] auto operator==(const merge_commit_command& other) const -> bool {
        if (!(_source == other._source && _target == other._target &&
              _prepare_index == other._prepare_index && _entries.size() == other._entries.size())) {
            return false;
        }
        for (std::size_t i = 0; i < _entries.size(); ++i) {
            if (_entries[i].term() != other._entries[i].term() ||
                _entries[i].index() != other._entries[i].index() ||
                _entries[i].command() != other._entries[i].command() ||
                _entries[i].type() != other._entries[i].type()) {
                return false;
            }
        }
        return true;
    }
};

/// @brief The body of an `entry_type::merge_rollback` entry, in the SOURCE's log.
///
/// Releases a source frozen by `merge_prepare`. The source leader proposes it
/// only after observing a committed `merge_abandoned` in the TARGET's log —
/// never on a timer.
template<raft_group_id GroupId> struct merge_rollback_command {
    GroupId _source_group{};
    GroupId _target_group{};

    [[nodiscard]] auto operator==(const merge_rollback_command&) const -> bool = default;
};

/// @brief The body of an `entry_type::merge_abandoned` entry, in the TARGET's log.
///
/// The one deliberate deviation from TiKV, and the reason the whole rollback
/// path exists in this shape. TiKV releases a frozen source on a timing
/// argument; Kythira has no clock-synchronisation guarantee anywhere, and
/// introducing one here would be a silent correctness dependency.
///
/// So the decision to abandon is made a **committed fact in the target's own
/// log**. A target leader failover cannot lose it: the new leader replays this
/// entry and will refuse to propose `merge_commit`. Symmetrically, once
/// `merge_commit` is proposed the target refuses to abandon. Commit and abandon
/// are mutually exclusive because both are decided by the same single log.
///
/// Releasing a source wrongly is the one way this protocol corrupts data: a
/// source that resumes serving while some target replica has already applied
/// the commit means two shards own one range.
template<raft_group_id GroupId> struct merge_abandoned_command {
    GroupId _source_group{};
    GroupId _target_group{};

    [[nodiscard]] auto operator==(const merge_abandoned_command&) const -> bool = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// Encoding primitives
// ─────────────────────────────────────────────────────────────────────────────

namespace shard_wire {

/// @brief A cursor over a payload that refuses to read past its end.
///
/// These bytes came off the network and through a Raft log; a decoder that
/// trusted its own length fields would turn a corrupt entry into a read out of
/// bounds on every replica at once.
class reader {
public:
    reader(const std::vector<std::byte>& bytes, std::string context)
        : _bytes(bytes), _context(std::move(context)) {}

    auto u8() -> std::uint8_t {
        require(1);
        return static_cast<std::uint8_t>(_bytes[_offset++]);
    }

    auto u64() -> std::uint64_t {
        require(sizeof(std::uint64_t));
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
            v = (v << 8U) | static_cast<std::uint64_t>(_bytes[_offset + i]);
        }
        _offset += sizeof(std::uint64_t);
        return v;
    }

    auto boolean() -> bool { return u8() != 0; }

    auto blob() -> std::vector<std::byte> {
        const auto length = static_cast<std::size_t>(u64());
        require(length);
        std::vector<std::byte> out(_bytes.begin() + static_cast<std::ptrdiff_t>(_offset),
                                   _bytes.begin() + static_cast<std::ptrdiff_t>(_offset + length));
        _offset += length;
        return out;
    }

    auto text() -> std::string {
        const auto bytes = blob();
        std::string out;
        out.reserve(bytes.size());
        for (auto b : bytes) {
            out.push_back(static_cast<char>(b));
        }
        return out;
    }

    [[nodiscard]] auto exhausted() const -> bool { return _offset == _bytes.size(); }

private:
    auto require(std::size_t n) const -> void {
        if (_offset + n > _bytes.size()) {
            throw serialization_exception(_context + ": payload truncated at offset " +
                                          std::to_string(_offset));
        }
    }

    const std::vector<std::byte>& _bytes;
    std::string _context;
    std::size_t _offset{0};
};

inline auto write_u8(std::vector<std::byte>& out, std::uint8_t v) -> void {
    out.push_back(static_cast<std::byte>(v));
}

inline auto write_u64(std::vector<std::byte>& out, std::uint64_t v) -> void {
    for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
        out.push_back(
            static_cast<std::byte>((v >> (8U * (sizeof(std::uint64_t) - 1 - i))) & 0xFFU));
    }
}

inline auto write_bool(std::vector<std::byte>& out, bool v) -> void {
    write_u8(out, v ? 1U : 0U);
}

inline auto write_blob(std::vector<std::byte>& out, const std::vector<std::byte>& blob) -> void {
    write_u64(out, blob.size());
    out.insert(out.end(), blob.begin(), blob.end());
}

inline auto write_text(std::vector<std::byte>& out, const std::string& text) -> void {
    write_u64(out, text.size());
    for (char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
}

/// @brief Encode a group id or node id, which may be numeric or textual.
template<typename Id> auto write_id(std::vector<std::byte>& out, const Id& id) -> void {
    if constexpr (std::same_as<Id, std::string>) {
        write_text(out, id);
    } else {
        write_u64(out, static_cast<std::uint64_t>(id));
    }
}

template<typename Id> auto read_id(reader& r) -> Id {
    if constexpr (std::same_as<Id, std::string>) {
        return r.text();
    } else {
        return static_cast<Id>(r.u64());
    }
}

template<raft_group_id GroupId, shard_key Key, typename NodeId, typename KeyCodec>
auto write_descriptor(std::vector<std::byte>& out, const shard_descriptor<GroupId, Key, NodeId>& d,
                      const KeyCodec& codec) -> void {
    write_id<GroupId>(out, d._group_id);
    write_bool(out, d._range._start.has_value());
    if (d._range._start.has_value()) {
        write_blob(out, codec.encode(*d._range._start));
    }
    write_bool(out, d._range._end.has_value());
    if (d._range._end.has_value()) {
        write_blob(out, codec.encode(*d._range._end));
    }
    write_u64(out, d._epoch._version);
    write_u64(out, d._epoch._conf_version);
    write_u64(out, d._voters.size());
    for (const auto& v : d._voters) {
        write_id<NodeId>(out, v);
    }
    write_u64(out, d._learners.size());
    for (const auto& l : d._learners) {
        write_id<NodeId>(out, l);
    }
    write_bool(out, d._leader_hint.has_value());
    if (d._leader_hint.has_value()) {
        write_id<NodeId>(out, *d._leader_hint);
    }
}

template<raft_group_id GroupId, shard_key Key, typename NodeId, typename KeyCodec>
auto read_descriptor(reader& r, const KeyCodec& codec) -> shard_descriptor<GroupId, Key, NodeId> {
    shard_descriptor<GroupId, Key, NodeId> d;
    d._group_id = read_id<GroupId>(r);
    if (r.boolean()) {
        d._range._start = codec.decode(r.blob());
    }
    if (r.boolean()) {
        d._range._end = codec.decode(r.blob());
    }
    d._epoch._version = r.u64();
    d._epoch._conf_version = r.u64();
    const auto voters = static_cast<std::size_t>(r.u64());
    d._voters.reserve(voters);
    for (std::size_t i = 0; i < voters; ++i) {
        d._voters.push_back(read_id<NodeId>(r));
    }
    const auto learners = static_cast<std::size_t>(r.u64());
    d._learners.reserve(learners);
    for (std::size_t i = 0; i < learners; ++i) {
        d._learners.push_back(read_id<NodeId>(r));
    }
    if (r.boolean()) {
        d._leader_hint = read_id<NodeId>(r);
    }
    return d;
}

}  // namespace shard_wire

// ─────────────────────────────────────────────────────────────────────────────
// split_command codec
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Version byte leading every administration payload.
///
/// One byte, checked on decode. These payloads live in a Raft log, which
/// outlives the process that wrote it: a node that starts on a newer binary and
/// replays an older log has to be able to say "I do not understand this entry"
/// rather than misread it.
inline constexpr std::uint8_t k_shard_command_version = 1;

/// @brief Serialise a `split_command` for an `entry_type::split` log entry.
template<raft_group_id GroupId, shard_key Key, typename NodeId,
         shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
[[nodiscard]] auto encode_split_command(const split_command<GroupId, Key, NodeId>& cmd,
                                        const KeyCodec& codec = KeyCodec{})
    -> std::vector<std::byte> {
    using namespace shard_wire;
    std::vector<std::byte> out;

    write_u8(out, k_shard_command_version);
    write_id<GroupId>(out, cmd._parent_group);
    write_u64(out, cmd._parent_epoch._version);
    write_u64(out, cmd._parent_epoch._conf_version);

    write_u64(out, cmd._at_keys.size());
    for (const auto& k : cmd._at_keys) {
        write_blob(out, codec.encode(k));
    }

    write_u64(out, cmd._children.size());
    for (const auto& child : cmd._children) {
        write_descriptor<GroupId, Key, NodeId>(out, child, codec);
    }

    write_bool(out, cmd._right_derive);
    write_u8(out, static_cast<std::uint8_t>(cmd._reason));
    write_bool(out, cmd._pd_operation_id.has_value());
    if (cmd._pd_operation_id.has_value()) {
        write_u64(out, *cmd._pd_operation_id);
    }
    return out;
}

/// @brief Decode a `split_command` from an `entry_type::split` log entry.
///
/// @throws serialization_exception on a truncated payload or an unknown version.
template<raft_group_id GroupId, shard_key Key, typename NodeId,
         shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
[[nodiscard]] auto decode_split_command(const std::vector<std::byte>& bytes,
                                        const KeyCodec& codec = KeyCodec{})
    -> split_command<GroupId, Key, NodeId> {
    using namespace shard_wire;
    reader r{bytes, "split_command"};

    if (const auto version = r.u8(); version != k_shard_command_version) {
        throw serialization_exception("split_command: unknown payload version " +
                                      std::to_string(version));
    }

    split_command<GroupId, Key, NodeId> cmd;
    cmd._parent_group = read_id<GroupId>(r);
    cmd._parent_epoch._version = r.u64();
    cmd._parent_epoch._conf_version = r.u64();

    const auto key_count = static_cast<std::size_t>(r.u64());
    cmd._at_keys.reserve(key_count);
    for (std::size_t i = 0; i < key_count; ++i) {
        cmd._at_keys.push_back(codec.decode(r.blob()));
    }

    const auto child_count = static_cast<std::size_t>(r.u64());
    cmd._children.reserve(child_count);
    for (std::size_t i = 0; i < child_count; ++i) {
        cmd._children.push_back(read_descriptor<GroupId, Key, NodeId>(r, codec));
    }

    cmd._right_derive = r.boolean();
    cmd._reason = static_cast<split_reason>(r.u8());
    if (r.boolean()) {
        cmd._pd_operation_id = r.u64();
    }
    return cmd;
}

// ─────────────────────────────────────────────────────────────────────────────
// Merge codecs
// ─────────────────────────────────────────────────────────────────────────────

template<raft_group_id GroupId, shard_key Key, typename NodeId,
         shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
[[nodiscard]] auto encode_merge_prepare_command(
    const merge_prepare_command<GroupId, Key, NodeId>& cmd, const KeyCodec& codec = KeyCodec{})
    -> std::vector<std::byte> {
    using namespace shard_wire;
    std::vector<std::byte> out;
    write_u8(out, k_shard_command_version);
    write_descriptor<GroupId, Key, NodeId>(out, cmd._source, codec);
    write_descriptor<GroupId, Key, NodeId>(out, cmd._target, codec);
    write_u64(out, cmd._min_index);
    write_u8(out, static_cast<std::uint8_t>(cmd._reason));
    return out;
}

template<raft_group_id GroupId, shard_key Key, typename NodeId,
         shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
[[nodiscard]] auto decode_merge_prepare_command(const std::vector<std::byte>& bytes,
                                                const KeyCodec& codec = KeyCodec{})
    -> merge_prepare_command<GroupId, Key, NodeId> {
    using namespace shard_wire;
    reader r{bytes, "merge_prepare_command"};
    if (const auto version = r.u8(); version != k_shard_command_version) {
        throw serialization_exception("merge_prepare_command: unknown payload version " +
                                      std::to_string(version));
    }
    merge_prepare_command<GroupId, Key, NodeId> cmd;
    cmd._source = read_descriptor<GroupId, Key, NodeId>(r, codec);
    cmd._target = read_descriptor<GroupId, Key, NodeId>(r, codec);
    cmd._min_index = r.u64();
    cmd._reason = static_cast<merge_reason>(r.u8());
    return cmd;
}

template<raft_group_id GroupId, shard_key Key, typename NodeId, typename LogEntry,
         shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
[[nodiscard]] auto encode_merge_commit_command(
    const merge_commit_command<GroupId, Key, NodeId, LogEntry>& cmd,
    const KeyCodec& codec = KeyCodec{}) -> std::vector<std::byte> {
    using namespace shard_wire;
    std::vector<std::byte> out;
    write_u8(out, k_shard_command_version);
    write_descriptor<GroupId, Key, NodeId>(out, cmd._source, codec);
    write_descriptor<GroupId, Key, NodeId>(out, cmd._target, codec);
    write_u64(out, cmd._prepare_index);
    write_u64(out, cmd._entries.size());
    for (const auto& e : cmd._entries) {
        write_u64(out, static_cast<std::uint64_t>(e.term()));
        write_u64(out, static_cast<std::uint64_t>(e.index()));
        write_u8(out, static_cast<std::uint8_t>(e.type()));
        write_blob(out, e.command());
    }
    return out;
}

template<raft_group_id GroupId, shard_key Key, typename NodeId, typename LogEntry,
         shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
[[nodiscard]] auto decode_merge_commit_command(const std::vector<std::byte>& bytes,
                                               const KeyCodec& codec = KeyCodec{})
    -> merge_commit_command<GroupId, Key, NodeId, LogEntry> {
    using namespace shard_wire;
    reader r{bytes, "merge_commit_command"};
    if (const auto version = r.u8(); version != k_shard_command_version) {
        throw serialization_exception("merge_commit_command: unknown payload version " +
                                      std::to_string(version));
    }
    merge_commit_command<GroupId, Key, NodeId, LogEntry> cmd;
    cmd._source = read_descriptor<GroupId, Key, NodeId>(r, codec);
    cmd._target = read_descriptor<GroupId, Key, NodeId>(r, codec);
    cmd._prepare_index = r.u64();
    const auto count = static_cast<std::size_t>(r.u64());
    cmd._entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        LogEntry e{};
        e._term = static_cast<decltype(e._term)>(r.u64());
        e._index = static_cast<decltype(e._index)>(r.u64());
        e._type = static_cast<entry_type>(r.u8());
        e._command = r.blob();
        cmd._entries.push_back(std::move(e));
    }
    return cmd;
}

template<raft_group_id GroupId>
[[nodiscard]] auto encode_merge_rollback_command(const merge_rollback_command<GroupId>& cmd)
    -> std::vector<std::byte> {
    using namespace shard_wire;
    std::vector<std::byte> out;
    write_u8(out, k_shard_command_version);
    write_id<GroupId>(out, cmd._source_group);
    write_id<GroupId>(out, cmd._target_group);
    return out;
}

template<raft_group_id GroupId>
[[nodiscard]] auto decode_merge_rollback_command(const std::vector<std::byte>& bytes)
    -> merge_rollback_command<GroupId> {
    using namespace shard_wire;
    reader r{bytes, "merge_rollback_command"};
    if (const auto version = r.u8(); version != k_shard_command_version) {
        throw serialization_exception("merge_rollback_command: unknown payload version " +
                                      std::to_string(version));
    }
    merge_rollback_command<GroupId> cmd;
    cmd._source_group = read_id<GroupId>(r);
    cmd._target_group = read_id<GroupId>(r);
    return cmd;
}

template<raft_group_id GroupId>
[[nodiscard]] auto encode_merge_abandoned_command(const merge_abandoned_command<GroupId>& cmd)
    -> std::vector<std::byte> {
    using namespace shard_wire;
    std::vector<std::byte> out;
    write_u8(out, k_shard_command_version);
    write_id<GroupId>(out, cmd._source_group);
    write_id<GroupId>(out, cmd._target_group);
    return out;
}

template<raft_group_id GroupId>
[[nodiscard]] auto decode_merge_abandoned_command(const std::vector<std::byte>& bytes)
    -> merge_abandoned_command<GroupId> {
    using namespace shard_wire;
    reader r{bytes, "merge_abandoned_command"};
    if (const auto version = r.u8(); version != k_shard_command_version) {
        throw serialization_exception("merge_abandoned_command: unknown payload version " +
                                      std::to_string(version));
    }
    merge_abandoned_command<GroupId> cmd;
    cmd._source_group = read_id<GroupId>(r);
    cmd._target_group = read_id<GroupId>(r);
    return cmd;
}

}  // namespace kythira
