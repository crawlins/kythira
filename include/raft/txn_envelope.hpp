// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file txn_envelope.hpp
/// @brief The envelope every command on a transactional host travels in, the
///        result each one returns, and the exceptions a caller sees.
///
/// See `.kiro/specs/multi-raft-transactions/` design "Envelope" and task 1.
///
/// ### Why an envelope at all
///
/// Transactions are a state-machine wrapper, not a new `entry_type`
/// (Requirement 1.5). Every transaction step is an ordinary `normal` entry,
/// so the wrapper needs a way to tell a plain application command from a
/// prepare, a commit or a resolve. This envelope is that way: a magic byte,
/// a format version, an op code and the op's fields. A plain command rides as
/// `txn_plain` with its bytes untouched, so the application's own command
/// encoding does not change (Requirement 2.1).
///
/// ### Everything apply needs is in the entry
///
/// The same rule as `shard_commands.hpp`. `ttl_deadline_ms`, `now_ms` and
/// every timestamp are stamped by whoever proposes the entry, and `txn_id`
/// is drawn by the coordinator before anything is proposed. `apply()` reads
/// no clock and draws no randomness, so every replica reaches the same state.
///
/// ### Untrusted input
///
/// These bytes come off the network (the gateway's forward link) and out of
/// a Raft log that outlives the binary that wrote it. Every count and length
/// is checked against both a fixed cap and the bytes actually remaining
/// **before** anything is allocated (the rule of PR #507), so a corrupt or
/// hostile entry is refused with a `serialization_exception` rather than
/// turned into a multi-gigabyte `reserve()` on every replica at once. Unknown
/// op codes, enum values outside their range, a boolean byte other than 0 or
/// 1, and trailing bytes are all refused too: a lenient decoder of a log
/// format is how two binaries come to disagree about one entry.

#include <raft/exceptions.hpp>
#include <raft/shard_types.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

// ─────────────────────────────────────────────────────────────────────────────
// Identifiers and levels
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A transaction's identity: 128 bits drawn from `RAND_bytes` at
/// `begin()`.
///
/// Random at the coordinator is safe because the value is frozen into the
/// entry before any replica applies it. 128 bits make a collision between two
/// live transactions not worth designing for.
struct txn_id {
    std::uint64_t _hi{0};
    std::uint64_t _lo{0};

    [[nodiscard]] auto operator<=>(const txn_id&) const = default;

    /// @brief 32 lowercase hex digits, for logs and exception messages.
    [[nodiscard]] auto to_string() const -> std::string {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out(32, '0');
        for (std::size_t i = 0; i < 16; ++i) {
            out[15 - i] = digits[(_hi >> (4U * i)) & 0xFU];
            out[31 - i] = digits[(_lo >> (4U * i)) & 0xFU];
        }
        return out;
    }
};

/// @brief The isolation a transaction asks for (Requirements 5, 8, 9, 10).
///
/// The first two need nothing beyond the tier-1 wrapper. `snapshot` and
/// `serializable` need a host built with `mvcc::on`; asking for them on one
/// without it fails at `begin()` and is never downgraded.
enum class txn_isolation : std::uint8_t {
    read_committed = 0,
    read_committed_wait = 1,
    snapshot = 2,
    serializable = 3,
};

[[nodiscard]] inline auto to_string(txn_isolation level) -> std::string {
    switch (level) {
        case txn_isolation::read_committed:
            return "read_committed";
        case txn_isolation::read_committed_wait:
            return "read_committed_wait";
        case txn_isolation::snapshot:
            return "snapshot";
        case txn_isolation::serializable:
            return "serializable";
    }
    return "txn_isolation(" + std::to_string(static_cast<unsigned>(level)) + ")";
}

/// @brief A transaction record's state on its primary shard (Requirement 4).
enum class txn_record_status : std::uint8_t {
    pending = 0,
    committed = 1,
    aborted = 2,
};

[[nodiscard]] inline auto to_string(txn_record_status s) -> std::string {
    switch (s) {
        case txn_record_status::pending:
            return "pending";
        case txn_record_status::committed:
            return "committed";
        case txn_record_status::aborted:
            return "aborted";
    }
    return "txn_record_status(" + std::to_string(static_cast<unsigned>(s)) + ")";
}

// ─────────────────────────────────────────────────────────────────────────────
// Envelope operations (design "Envelope", the op table)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One buffered write: the key it routes by and the application's
/// command, untouched.
template<shard_key Key> struct txn_write {
    Key _key{};
    std::vector<std::byte> _command{};

    [[nodiscard]] auto operator==(const txn_write&) const -> bool = default;
};

/// @brief An ordinary single-key command on a transactional host
/// (Requirement 2).
struct txn_plain {
    std::vector<std::byte> _command{};

    [[nodiscard]] auto operator==(const txn_plain&) const -> bool = default;
};

/// @brief Every key in one shard: applied atomically by one entry
/// (Requirement 3.2). The timestamps are zero in tier 1.
template<shard_key Key> struct txn_one_phase {
    txn_id _txn{};
    std::vector<txn_write<Key>> _writes{};
    std::uint64_t _start_ts{0};
    std::uint64_t _commit_ts{0};

    [[nodiscard]] auto operator==(const txn_one_phase&) const -> bool = default;
};

/// @brief Install this shard's intents; on the primary also the `pending`
/// record (Requirement 3.3).
template<shard_key Key> struct txn_prepare {
    txn_id _txn{};
    Key _primary{};
    std::vector<txn_write<Key>> _writes{};
    /// Tier 3 only; empty below it.
    std::vector<Key> _read_keys{};
    std::uint64_t _start_ts{0};
    /// Absolute, stamped by the primary shard's leader (Requirement 4.1).
    std::uint64_t _ttl_deadline_ms{0};
    bool _is_primary{false};

    [[nodiscard]] auto operator==(const txn_prepare&) const -> bool = default;
};

/// @brief The commit point: `pending` becomes `committed` on the primary's
/// shard.
template<shard_key Key> struct txn_commit_primary {
    txn_id _txn{};
    Key _primary{};
    std::uint64_t _commit_ts{0};

    [[nodiscard]] auto operator==(const txn_commit_primary&) const -> bool = default;
};

/// @brief Roll a secondary shard's intents forward after the primary
/// committed.
template<shard_key Key> struct txn_commit_secondary {
    txn_id _txn{};
    std::vector<Key> _keys{};
    std::uint64_t _commit_ts{0};

    [[nodiscard]] auto operator==(const txn_commit_secondary&) const -> bool = default;
};

/// @brief Remove this transaction's intents; on the primary, mark the record
/// `aborted` (or leave an `aborted` marker if there is none).
template<shard_key Key> struct txn_abort {
    txn_id _txn{};
    Key _primary{};
    std::vector<Key> _keys{};

    [[nodiscard]] auto operator==(const txn_abort&) const -> bool = default;
};

/// @brief Extend a `pending` record's deadline while `commit()` runs
/// (Requirement 4.2).
template<shard_key Key> struct txn_heartbeat {
    txn_id _txn{};
    Key _primary{};
    std::uint64_t _new_deadline_ms{0};

    [[nodiscard]] auto operator==(const txn_heartbeat&) const -> bool = default;
};

/// @brief Learn a transaction's fate from its primary, aborting it if its TTL
/// has passed (Requirement 4.3). `now_ms` is stamped by the proposer.
template<shard_key Key> struct txn_check_and_resolve {
    txn_id _txn{};
    Key _primary{};
    std::uint64_t _now_ms{0};

    [[nodiscard]] auto operator==(const txn_check_and_resolve&) const -> bool = default;
};

/// @brief One record to drop in a `txn_cleanup` batch.
template<shard_key Key> struct txn_cleanup_item {
    Key _primary{};
    txn_id _txn{};

    [[nodiscard]] auto operator==(const txn_cleanup_item&) const -> bool = default;
};

/// @brief Drop resolved or expired records; leader-proposed (Requirement 4.7).
template<shard_key Key> struct txn_cleanup {
    std::vector<txn_cleanup_item<Key>> _items{};

    [[nodiscard]] auto operator==(const txn_cleanup&) const -> bool = default;
};

/// @brief Collect versions below the safe point; leader-proposed, tier 2
/// (Requirement 8.6).
struct txn_gc {
    std::uint64_t _safe_point{0};

    [[nodiscard]] auto operator==(const txn_gc&) const -> bool = default;
};

/// @brief Raise the oracle group's reserved high-water mark, tier 2
/// (Requirement 7.3).
struct txn_oracle_reserve {
    std::uint64_t _new_hwm{0};

    [[nodiscard]] auto operator==(const txn_oracle_reserve&) const -> bool = default;
};

/// @brief One decoded envelope.
///
/// The alternatives are in op-code order: alternative `i` is op code `i + 1`,
/// so `encode` and `decode` cannot disagree about the mapping.
template<shard_key Key>
using txn_envelope =
    std::variant<txn_plain, txn_one_phase<Key>, txn_prepare<Key>, txn_commit_primary<Key>,
                 txn_commit_secondary<Key>, txn_abort<Key>, txn_heartbeat<Key>,
                 txn_check_and_resolve<Key>, txn_cleanup<Key>, txn_gc, txn_oracle_reserve>;

// ─────────────────────────────────────────────────────────────────────────────
// Results
// ─────────────────────────────────────────────────────────────────────────────

/// @brief What an applied envelope reports: a one-byte status, then fields.
enum class txn_result_status : std::uint8_t {
    /// Applied. For `txn_plain`, `_payload` is the wrapped state machine's
    /// own result.
    ok = 0,
    /// Prepare or 1PC met another transaction's intent (no-wait locking).
    write_conflict = 1,
    /// A plain command's key holds an intent (Requirement 2.2).
    key_locked = 2,
    /// The record's state, from commit_primary, abort or check_and_resolve.
    record = 3,
    /// Tier 3: a read key changed or is locked.
    read_conflict = 4,
    /// Tier 2: the start timestamp is below the safe point.
    snapshot_too_old = 5,
    /// A primary prepare found the rollback marker (Requirement 4.4).
    already_aborted = 6,
};

/// @brief Who holds a key, for the conflict statuses.
///
/// Absent on a `read_conflict` caused by a newer committed version rather
/// than by an intent.
template<shard_key Key> struct txn_lock_holder {
    txn_id _txn{};
    Key _primary{};

    [[nodiscard]] auto operator==(const txn_lock_holder&) const -> bool = default;
};

template<shard_key Key> struct txn_result {
    txn_result_status _status{txn_result_status::ok};
    /// `ok`: the wrapped state machine's result, empty for transaction ops.
    std::vector<std::byte> _payload{};
    /// The conflict statuses: the key that conflicted, and who holds it.
    Key _key{};
    std::optional<txn_lock_holder<Key>> _holder{};
    /// `record`.
    txn_record_status _record{txn_record_status::pending};
    std::uint64_t _commit_ts{0};
    /// `snapshot_too_old`: the safe point the start timestamp fell below.
    std::uint64_t _safe_point{0};

    [[nodiscard]] auto operator==(const txn_result&) const -> bool = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// Exceptions
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A plain command named a key that a transaction holds
/// (Requirement 2.2).
///
/// Carries the holder and its primary key so the caller can resolve the lock
/// (`check_and_resolve` on the primary's shard) before retrying.
template<shard_key Key> class key_locked_exception : public raft_exception {
public:
    key_locked_exception(Key key, txn_id holder, Key primary)
        : raft_exception("key " + detail::describe_value(key) + " is locked by transaction " +
                         holder.to_string() + " (primary key " + detail::describe_value(primary) +
                         ")"),
          _key(std::move(key)),
          _holder(holder),
          _primary(std::move(primary)) {}

    [[nodiscard]] auto key() const -> const Key& { return _key; }
    [[nodiscard]] auto holder() const -> const txn_id& { return _holder; }
    [[nodiscard]] auto primary() const -> const Key& { return _primary; }

private:
    Key _key;
    txn_id _holder;
    Key _primary;
};

/// @brief A transaction asked for an isolation level the build cannot give
/// (Requirement 10.2). Thrown at `begin()`; never a silent downgrade.
class isolation_unsupported_exception : public raft_exception {
public:
    isolation_unsupported_exception(txn_isolation requested, std::vector<txn_isolation> offered)
        : raft_exception(describe(requested, offered)),
          _requested(requested),
          _offered(std::move(offered)) {}

    [[nodiscard]] auto requested() const -> txn_isolation { return _requested; }
    [[nodiscard]] auto offered() const -> const std::vector<txn_isolation>& { return _offered; }

private:
    static auto describe(txn_isolation requested, const std::vector<txn_isolation>& offered)
        -> std::string {
        std::string out =
            "isolation " + to_string(requested) + " is not available in this build; it offers ";
        for (std::size_t i = 0; i < offered.size(); ++i) {
            out += (i == 0 ? "" : ", ") + to_string(offered[i]);
        }
        if (offered.empty()) {
            out += "none";
        }
        if (requested == txn_isolation::snapshot || requested == txn_isolation::serializable) {
            out += " (snapshot and serializable need a host built with mvcc::on)";
        }
        return out;
    }

    txn_isolation _requested;
    std::vector<txn_isolation> _offered;
};

// ─────────────────────────────────────────────────────────────────────────────
// Wire format
// ─────────────────────────────────────────────────────────────────────────────

/// @brief First byte of every envelope and every result: "K".
inline constexpr std::uint8_t k_txn_magic = 0x4B;
/// @brief Format version, checked on decode. A log outlives its binary.
inline constexpr std::uint8_t k_txn_format_version = 1;

/// @brief Most keys one envelope may name, in any one list.
///
/// Far above any real transaction, low enough that the vector it sizes is
/// small. The bytes-remaining check below is what actually stops a forged
/// count; this one keeps an honest-but-absurd transaction out of the log.
inline constexpr std::size_t k_txn_max_keys = 65'536;
/// @brief Largest encoded key.
inline constexpr std::size_t k_txn_max_key_bytes = 64 * 1024;
/// @brief Largest single command or result payload.
inline constexpr std::size_t k_txn_max_command_bytes = 64 * 1024 * 1024;

namespace txn_wire {

/// @brief A bounds-checked cursor over an envelope or result.
///
/// Its length check compares against the bytes remaining rather than
/// computing `offset + n`, which a forged 64-bit length would wrap past the
/// end of the buffer.
class reader {
public:
    reader(const std::vector<std::byte>& bytes, const char* context)
        : _bytes(bytes), _context(context) {}

    auto u8() -> std::uint8_t {
        require(1, "u8");
        return static_cast<std::uint8_t>(_bytes[_offset++]);
    }

    auto u64() -> std::uint64_t {
        require(sizeof(std::uint64_t), "u64");
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
            v = (v << 8U) | static_cast<std::uint64_t>(_bytes[_offset + i]);
        }
        _offset += sizeof(std::uint64_t);
        return v;
    }

    auto boolean() -> bool {
        const auto v = u8();
        if (v > 1) {
            fail("boolean byte " + std::to_string(v) + " is neither 0 nor 1");
        }
        return v == 1;
    }

    /// @brief A length-prefixed byte string of at most `cap` bytes.
    auto blob(std::size_t cap, const char* what) -> std::vector<std::byte> {
        const auto length = u64();
        if (length > cap) {
            fail(std::string(what) + " length " + std::to_string(length) + " exceeds the cap of " +
                 std::to_string(cap));
        }
        require(static_cast<std::size_t>(length), what);
        const auto first = _bytes.begin() + static_cast<std::ptrdiff_t>(_offset);
        std::vector<std::byte> out(first, first + static_cast<std::ptrdiff_t>(length));
        _offset += static_cast<std::size_t>(length);
        return out;
    }

    /// @brief A list count, refused before any allocation if it exceeds
    /// `k_txn_max_keys` or could not fit in the bytes left at
    /// `min_element_bytes` each.
    auto count(std::size_t min_element_bytes, const char* what) -> std::size_t {
        const auto n = u64();
        if (n > k_txn_max_keys) {
            fail(std::string(what) + " count " + std::to_string(n) + " exceeds the cap of " +
                 std::to_string(k_txn_max_keys));
        }
        if (n > remaining() / min_element_bytes) {
            fail(std::string(what) + " count " + std::to_string(n) + " cannot fit in the " +
                 std::to_string(remaining()) + " bytes left");
        }
        return static_cast<std::size_t>(n);
    }

    auto finish() const -> void {
        if (remaining() != 0) {
            fail(std::to_string(remaining()) + " trailing byte(s)");
        }
    }

    [[noreturn]] auto fail(const std::string& why) const -> void {
        throw serialization_exception(std::string(_context) + ": " + why + " at offset " +
                                      std::to_string(_offset));
    }

private:
    [[nodiscard]] auto remaining() const -> std::size_t { return _bytes.size() - _offset; }

    auto require(std::size_t n, const char* what) const -> void {
        if (n > remaining()) {
            fail(std::string("truncated reading ") + what);
        }
    }

    const std::vector<std::byte>& _bytes;
    const char* _context;
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

inline auto write_blob(std::vector<std::byte>& out, const std::vector<std::byte>& blob,
                       std::size_t cap, const char* what) -> void {
    if (blob.size() > cap) {
        throw serialization_exception(std::string("txn envelope: ") + what + " of " +
                                      std::to_string(blob.size()) + " bytes exceeds the cap of " +
                                      std::to_string(cap));
    }
    write_u64(out, blob.size());
    out.insert(out.end(), blob.begin(), blob.end());
}

inline auto write_count(std::vector<std::byte>& out, std::size_t n, const char* what) -> void {
    if (n > k_txn_max_keys) {
        throw serialization_exception(std::string("txn envelope: ") + what + " count " +
                                      std::to_string(n) + " exceeds the cap of " +
                                      std::to_string(k_txn_max_keys));
    }
    write_u64(out, n);
}

inline auto write_txn_id(std::vector<std::byte>& out, const txn_id& id) -> void {
    write_u64(out, id._hi);
    write_u64(out, id._lo);
}

inline auto read_txn_id(reader& r) -> txn_id {
    txn_id id;
    id._hi = r.u64();
    id._lo = r.u64();
    return id;
}

// The smallest a list element can be on the wire, for reader::count.
inline constexpr std::size_t k_min_key_bytes = sizeof(std::uint64_t);
inline constexpr std::size_t k_min_write_bytes = 2 * sizeof(std::uint64_t);
inline constexpr std::size_t k_min_cleanup_item_bytes = 3 * sizeof(std::uint64_t);

template<shard_key Key, typename KeyCodec>
auto write_key(std::vector<std::byte>& out, const Key& k, const KeyCodec& codec) -> void {
    write_blob(out, codec.encode(k), k_txn_max_key_bytes, "key");
}

template<shard_key Key, typename KeyCodec> auto read_key(reader& r, const KeyCodec& codec) -> Key {
    return codec.decode(r.blob(k_txn_max_key_bytes, "key"));
}

template<shard_key Key, typename KeyCodec>
auto write_keys(std::vector<std::byte>& out, const std::vector<Key>& keys, const KeyCodec& codec)
    -> void {
    write_count(out, keys.size(), "key list");
    for (const auto& k : keys) {
        write_key(out, k, codec);
    }
}

template<shard_key Key, typename KeyCodec>
auto read_keys(reader& r, const KeyCodec& codec) -> std::vector<Key> {
    const auto n = r.count(k_min_key_bytes, "key list");
    std::vector<Key> keys;
    keys.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        keys.push_back(read_key<Key>(r, codec));
    }
    return keys;
}

template<shard_key Key, typename KeyCodec>
auto write_writes(std::vector<std::byte>& out, const std::vector<txn_write<Key>>& writes,
                  const KeyCodec& codec) -> void {
    write_count(out, writes.size(), "write list");
    for (const auto& w : writes) {
        write_key(out, w._key, codec);
        write_blob(out, w._command, k_txn_max_command_bytes, "command");
    }
}

template<shard_key Key, typename KeyCodec>
auto read_writes(reader& r, const KeyCodec& codec) -> std::vector<txn_write<Key>> {
    const auto n = r.count(k_min_write_bytes, "write list");
    std::vector<txn_write<Key>> writes;
    writes.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        txn_write<Key> w;
        w._key = read_key<Key>(r, codec);
        w._command = r.blob(k_txn_max_command_bytes, "command");
        writes.push_back(std::move(w));
    }
    return writes;
}

inline auto read_header(reader& r, const char* what) -> void {
    if (const auto magic = r.u8(); magic != k_txn_magic) {
        r.fail(std::string("not a ") + what + " (first byte " + std::to_string(magic) +
               ", expected " + std::to_string(k_txn_magic) + ")");
    }
    if (const auto version = r.u8(); version != k_txn_format_version) {
        r.fail(std::string("unknown ") + what + " format version " + std::to_string(version));
    }
}

inline auto write_header(std::vector<std::byte>& out) -> void {
    write_u8(out, k_txn_magic);
    write_u8(out, k_txn_format_version);
}

}  // namespace txn_wire

// ─────────────────────────────────────────────────────────────────────────────
// Envelope codec
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The op code an envelope carries: its variant index plus one.
template<shard_key Key>
[[nodiscard]] auto txn_op_code(const txn_envelope<Key>& env) -> std::uint8_t {
    return static_cast<std::uint8_t>(env.index() + 1);
}

/// @brief Serialise an envelope for a `normal` log entry or the forward link.
///
/// @throws serialization_exception if a list, key or command exceeds its cap,
///         so the proposer learns it here rather than every replica later.
template<shard_key Key, shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
[[nodiscard]] auto encode_txn_envelope(const txn_envelope<Key>& env,
                                       const KeyCodec& codec = KeyCodec{})
    -> std::vector<std::byte> {
    using namespace txn_wire;
    std::vector<std::byte> out;
    write_header(out);
    write_u8(out, txn_op_code<Key>(env));

    std::visit(
        [&](const auto& op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::same_as<T, txn_plain>) {
                write_blob(out, op._command, k_txn_max_command_bytes, "command");
            } else if constexpr (std::same_as<T, txn_one_phase<Key>>) {
                write_txn_id(out, op._txn);
                write_writes(out, op._writes, codec);
                write_u64(out, op._start_ts);
                write_u64(out, op._commit_ts);
            } else if constexpr (std::same_as<T, txn_prepare<Key>>) {
                write_txn_id(out, op._txn);
                write_key(out, op._primary, codec);
                write_writes(out, op._writes, codec);
                write_keys(out, op._read_keys, codec);
                write_u64(out, op._start_ts);
                write_u64(out, op._ttl_deadline_ms);
                write_bool(out, op._is_primary);
            } else if constexpr (std::same_as<T, txn_commit_primary<Key>>) {
                write_txn_id(out, op._txn);
                write_key(out, op._primary, codec);
                write_u64(out, op._commit_ts);
            } else if constexpr (std::same_as<T, txn_commit_secondary<Key>>) {
                write_txn_id(out, op._txn);
                write_keys(out, op._keys, codec);
                write_u64(out, op._commit_ts);
            } else if constexpr (std::same_as<T, txn_abort<Key>>) {
                write_txn_id(out, op._txn);
                write_key(out, op._primary, codec);
                write_keys(out, op._keys, codec);
            } else if constexpr (std::same_as<T, txn_heartbeat<Key>>) {
                write_txn_id(out, op._txn);
                write_key(out, op._primary, codec);
                write_u64(out, op._new_deadline_ms);
            } else if constexpr (std::same_as<T, txn_check_and_resolve<Key>>) {
                write_txn_id(out, op._txn);
                write_key(out, op._primary, codec);
                write_u64(out, op._now_ms);
            } else if constexpr (std::same_as<T, txn_cleanup<Key>>) {
                write_count(out, op._items.size(), "cleanup list");
                for (const auto& item : op._items) {
                    write_key(out, item._primary, codec);
                    write_txn_id(out, item._txn);
                }
            } else if constexpr (std::same_as<T, txn_gc>) {
                write_u64(out, op._safe_point);
            } else if constexpr (std::same_as<T, txn_oracle_reserve>) {
                write_u64(out, op._new_hwm);
            } else {
                static_assert(sizeof(T) == 0, "every envelope op must be encoded");
            }
        },
        env);
    return out;
}

/// @brief Whether `bytes` starts like an envelope: magic and version only.
///
/// For the snapshot-format and wrong-host diagnostics, not for trust: a
/// payload that passes this can still fail `decode_txn_envelope`.
[[nodiscard]] inline auto looks_like_txn_envelope(const std::vector<std::byte>& bytes) -> bool {
    return bytes.size() >= 2 && static_cast<std::uint8_t>(bytes[0]) == k_txn_magic &&
           static_cast<std::uint8_t>(bytes[1]) == k_txn_format_version;
}

/// @brief Decode an envelope from a log entry or the forward link.
///
/// @throws serialization_exception on a wrong magic or version, an unknown op,
///         a truncated payload, a count or length over its cap or over the
///         bytes left, a malformed boolean, or trailing bytes.
template<shard_key Key, shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
[[nodiscard]] auto decode_txn_envelope(const std::vector<std::byte>& bytes,
                                       const KeyCodec& codec = KeyCodec{}) -> txn_envelope<Key> {
    using namespace txn_wire;
    reader r{bytes, "txn envelope"};
    read_header(r, "txn envelope");

    const auto op = r.u8();
    auto env = [&]() -> txn_envelope<Key> {
        switch (op) {
            case 1:
                return txn_plain{r.blob(k_txn_max_command_bytes, "command")};
            case 2: {
                txn_one_phase<Key> v;
                v._txn = read_txn_id(r);
                v._writes = read_writes<Key>(r, codec);
                v._start_ts = r.u64();
                v._commit_ts = r.u64();
                return v;
            }
            case 3: {
                txn_prepare<Key> v;
                v._txn = read_txn_id(r);
                v._primary = read_key<Key>(r, codec);
                v._writes = read_writes<Key>(r, codec);
                v._read_keys = read_keys<Key>(r, codec);
                v._start_ts = r.u64();
                v._ttl_deadline_ms = r.u64();
                v._is_primary = r.boolean();
                return v;
            }
            case 4: {
                txn_commit_primary<Key> v;
                v._txn = read_txn_id(r);
                v._primary = read_key<Key>(r, codec);
                v._commit_ts = r.u64();
                return v;
            }
            case 5: {
                txn_commit_secondary<Key> v;
                v._txn = read_txn_id(r);
                v._keys = read_keys<Key>(r, codec);
                v._commit_ts = r.u64();
                return v;
            }
            case 6: {
                txn_abort<Key> v;
                v._txn = read_txn_id(r);
                v._primary = read_key<Key>(r, codec);
                v._keys = read_keys<Key>(r, codec);
                return v;
            }
            case 7: {
                txn_heartbeat<Key> v;
                v._txn = read_txn_id(r);
                v._primary = read_key<Key>(r, codec);
                v._new_deadline_ms = r.u64();
                return v;
            }
            case 8: {
                txn_check_and_resolve<Key> v;
                v._txn = read_txn_id(r);
                v._primary = read_key<Key>(r, codec);
                v._now_ms = r.u64();
                return v;
            }
            case 9: {
                txn_cleanup<Key> v;
                const auto n = r.count(k_min_cleanup_item_bytes, "cleanup list");
                v._items.reserve(n);
                for (std::size_t i = 0; i < n; ++i) {
                    txn_cleanup_item<Key> item;
                    item._primary = read_key<Key>(r, codec);
                    item._txn = read_txn_id(r);
                    v._items.push_back(std::move(item));
                }
                return v;
            }
            case 10:
                return txn_gc{r.u64()};
            case 11:
                return txn_oracle_reserve{r.u64()};
            default:
                r.fail("unknown op code " + std::to_string(op));
        }
    }();
    r.finish();
    return env;
}

// ─────────────────────────────────────────────────────────────────────────────
// Result codec
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Serialise a result. Only the fields its status uses are written.
template<shard_key Key, shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
[[nodiscard]] auto encode_txn_result(const txn_result<Key>& res, const KeyCodec& codec = KeyCodec{})
    -> std::vector<std::byte> {
    using namespace txn_wire;
    std::vector<std::byte> out;
    write_header(out);
    write_u8(out, static_cast<std::uint8_t>(res._status));
    switch (res._status) {
        case txn_result_status::ok:
            write_blob(out, res._payload, k_txn_max_command_bytes, "result payload");
            break;
        case txn_result_status::write_conflict:
        case txn_result_status::key_locked:
        case txn_result_status::read_conflict:
            write_key(out, res._key, codec);
            write_bool(out, res._holder.has_value());
            if (res._holder.has_value()) {
                write_txn_id(out, res._holder->_txn);
                write_key(out, res._holder->_primary, codec);
            }
            break;
        case txn_result_status::record:
            write_u8(out, static_cast<std::uint8_t>(res._record));
            write_u64(out, res._commit_ts);
            break;
        case txn_result_status::snapshot_too_old:
            write_u64(out, res._safe_point);
            break;
        case txn_result_status::already_aborted:
            break;
    }
    return out;
}

/// @brief Decode a result.
///
/// @throws serialization_exception under the same rules as
///         `decode_txn_envelope`, and on a status or record value out of range.
template<shard_key Key, shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
[[nodiscard]] auto decode_txn_result(const std::vector<std::byte>& bytes,
                                     const KeyCodec& codec = KeyCodec{}) -> txn_result<Key> {
    using namespace txn_wire;
    reader r{bytes, "txn result"};
    read_header(r, "txn result");

    txn_result<Key> res;
    const auto status = r.u8();
    if (status > static_cast<std::uint8_t>(txn_result_status::already_aborted)) {
        r.fail("unknown result status " + std::to_string(status));
    }
    res._status = static_cast<txn_result_status>(status);
    switch (res._status) {
        case txn_result_status::ok:
            res._payload = r.blob(k_txn_max_command_bytes, "result payload");
            break;
        case txn_result_status::write_conflict:
        case txn_result_status::key_locked:
        case txn_result_status::read_conflict:
            res._key = read_key<Key>(r, codec);
            if (r.boolean()) {
                txn_lock_holder<Key> holder;
                holder._txn = read_txn_id(r);
                holder._primary = read_key<Key>(r, codec);
                res._holder = std::move(holder);
            }
            break;
        case txn_result_status::record: {
            const auto record = r.u8();
            if (record > static_cast<std::uint8_t>(txn_record_status::aborted)) {
                r.fail("unknown record status " + std::to_string(record));
            }
            res._record = static_cast<txn_record_status>(record);
            res._commit_ts = r.u64();
            break;
        }
        case txn_result_status::snapshot_too_old:
            res._safe_point = r.u64();
            break;
        case txn_result_status::already_aborted:
            break;
    }
    r.finish();
    return res;
}

}  // namespace kythira
