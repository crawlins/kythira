// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kinesis_stream_state_machine.hpp
/// @brief The replicated state behind Kinesis streams
///        (.kiro/specs/kinesis-compatible-streams/ design "Stream state is
///        small; record bytes are not in the snapshot"; Requirements 6.2-6.4,
///        8.1, 8.3).
///
/// One instance per multi_raft group. It satisfies `kythira::state_machine`
/// and `kythira::splittable_state_machine<_, std::string>`, and holds a
/// key-ordered map of three kinds of entry (kinesis_catalog.hpp has the key
/// layout): the next-id counter, stream records, and per-shard state. The
/// group that owns `\0meta/kinesis/` runs the catalog commands; a group that
/// owns routing keys runs the shard commands. Both are the same type, so a
/// Kythira split or merge never has to know which kind it is moving.
///
/// ### Record bytes
///
/// Records go to a `record_segment_store` during `apply()`, keyed by the
/// shard's routing key, with the per-shard counter as the store's sequence.
/// The store is shared by every group on the node, so splitting or merging
/// groups moves no bytes. `apply()` calls the store's barrier before it
/// returns, so an entry the node acknowledges has its records on disk.
///
/// The snapshot carries no record bytes, only each shard's counter, arrival
/// clock, trim horizon and segment index: the sealed segments (which every
/// replica rolls identically, because rolls depend only on records) and,
/// taken from the store when the snapshot is serialized, the active
/// segment's descriptor with its digest so far. A replica restoring the
/// snapshot truncates any local tail past the snapshot's counters and
/// fetches the active prefix it lacks through the `segment_fetcher` hook, so
/// its next append lands in the same segment as everyone else's. Sealed
/// segments are fetched lazily on read.
///
/// All replicas of a group must agree on whether a store is attached and on
/// its options; a store-less instance (tests of the catalog logic) keeps
/// counters and the catalog but no segment index.
///
/// ### Determinism
///
/// `apply()` reads no clock: arrival timestamps are stamped by the leader
/// into the command, and clamped here so they never go backwards within a
/// shard (Requirement 8.3; the clock assumption is in doc/sequence-numbers.md).

#include <kythira/kinesis/kinesis_catalog.hpp>
#include <kythira/kinesis/kinesis_hash.hpp>
#include <raft/record_segment_store.hpp>
#include <raft/shard_types.hpp>
#include <raft/splittable_state_machine.hpp>
#include <raft/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::kinesis {

// ---- per-shard state ------------------------------------------------------

struct shard_state {
    /// The last counter handed out (0 before the first record when the
    /// shard starts at 0). The next record gets `_last_counter + 1`.
    std::uint64_t _last_counter = 0;
    /// The latest arrival timestamp stamped; never decreases.
    std::int64_t _last_arrival_ms = 0;
    bool _open = true;
    /// Counters below this are trimmed (retention); 0 = nothing trimmed.
    std::uint64_t _trim_before = 0;
    /// Record payload bytes appended, less the file bytes of trimmed
    /// segments; what the split policy weighs.
    std::uint64_t _retained_bytes = 0;
    std::uint64_t _records = 0;
    /// Sealed segments, oldest first.
    std::vector<record_segment_descriptor> _sealed{};

    friend auto operator==(const shard_state&, const shard_state&) -> bool = default;
};

/// What a stored record's bytes are: the partition key and the data.
/// Everything else `GetRecords` returns (sequence number, arrival time)
/// comes from the store's record header.
struct stored_record {
    std::string _partition_key{};
    std::vector<std::byte> _data{};
    friend auto operator==(const stored_record&, const stored_record&) -> bool = default;
};

[[nodiscard]] inline auto encode_stored_record(std::string_view partition_key,
                                               std::span<const std::byte> data)
    -> std::vector<std::byte> {
    codec::writer w;
    w.u16(static_cast<std::uint16_t>(partition_key.size()));
    for (const char c : partition_key) {
        w.u8(static_cast<std::uint8_t>(c));
    }
    w.raw(data);
    return w.take();
}

[[nodiscard]] inline auto decode_stored_record(std::span<const std::byte> bytes) -> stored_record {
    codec::reader r(bytes);
    auto pk = r.take(r.u16());
    stored_record out;
    out._partition_key.assign(reinterpret_cast<const char*>(pk.data()), pk.size());
    auto rest = r.take(r.remaining());
    out._data.assign(rest.begin(), rest.end());
    return out;
}

// ---- commands -------------------------------------------------------------

/// `CreateStream`, at the catalog group. Allocates the stream id from the
/// next-id counter and writes the record in CREATING with an even shard
/// table. The front end then opens each shard in its group and moves the
/// stream to ACTIVE.
struct create_stream_command {
    std::string _account{};
    std::string _name{};
    std::uint32_t _shard_count = 1;
    stream_mode _mode = stream_mode::provisioned;
    std::uint32_t _retention_hours = 24;
    std::int64_t _created_ms = 0;
    std::map<std::string, std::string> _tags{};
    encryption_type _encryption = encryption_type::none;
    std::string _key_id{};
};

/// A status change; `_id` guards against acting on a recreated stream.
struct set_stream_status_command {
    std::string _account{};
    std::string _name{};
    std::uint64_t _id = 0;
    stream_status _status = stream_status::active;
};

/// The end of DELETING: the catalog forgets the stream. Its id stays used.
struct remove_stream_command {
    std::string _account{};
    std::string _name{};
    std::uint64_t _id = 0;
};

/// Start a shard's state under its routing key.
struct open_shard_command {
    std::string _key{};
    std::uint64_t _starting_counter = 0;
};

struct append_record {
    std::string _partition_key{};
    std::vector<std::byte> _data{};
};

/// One shard's records from one `PutRecord`/`PutRecords` (Requirement 7.1:
/// one entry per shard). `_arrival_ms` is the leader's wall clock.
struct append_records_command {
    std::string _key{};
    std::int64_t _arrival_ms = 0;
    std::vector<append_record> _records{};
};

/// Group commit (task 6): the appends of several shards that live in one
/// group, in one entry and behind one durability barrier. Each part has its
/// own result (`apply_result::_parts`, in order). A part whose shard is not
/// in this group (moved by a split since the leader routed it) answers
/// `not_found` and touches nothing, so a batch never applies an append in
/// the wrong group. Proposed under its first part's key.
struct append_batch_command {
    std::vector<append_records_command> _appends{};
};

/// Close a shard (resharding): no more appends, active segment sealed.
struct close_shard_command {
    std::string _key{};
};

/// Forget a shard and delete its segments (stream deletion).
struct drop_shard_command {
    std::string _key{};
};

/// Retention: drop sealed segments wholly below `_before` and move the trim
/// horizon there.
struct trim_shard_command {
    std::string _key{};
    std::uint64_t _before = 0;
};

using stream_command =
    std::variant<create_stream_command, set_stream_status_command, remove_stream_command,
                 open_shard_command, append_records_command, close_shard_command,
                 drop_shard_command, trim_shard_command, append_batch_command>;

inline constexpr std::uint8_t stream_command_version = 1;

/// The multi_raft routing key a command is proposed under.
[[nodiscard]] inline auto command_key(const stream_command& c) -> std::string {
    return std::visit(
        []<typename T>(const T& v) -> std::string {
            if constexpr (std::is_same_v<T, create_stream_command> ||
                          std::is_same_v<T, set_stream_status_command> ||
                          std::is_same_v<T, remove_stream_command>) {
                return stream_key(v._account, v._name);
            } else if constexpr (std::is_same_v<T, append_batch_command>) {
                return v._appends.empty() ? std::string() : v._appends.front()._key;
            } else {
                return v._key;
            }
        },
        c);
}

namespace command_detail {

inline void encode_append(codec::writer& w, const append_records_command& v) {
    w.str(v._key).i64(v._arrival_ms).u32(static_cast<std::uint32_t>(v._records.size()));
    for (const auto& r : v._records) {
        w.str(r._partition_key).bytes(r._data);
    }
}

inline auto decode_append(codec::reader& r) -> append_records_command {
    append_records_command c;
    c._key = r.str();
    c._arrival_ms = r.i64();
    const auto n = r.u32();
    c._records.reserve(std::min<std::size_t>(n, r.remaining() / 8));
    for (std::uint32_t i = 0; i < n; ++i) {
        append_record a;
        a._partition_key = r.str();
        a._data = r.bytes();
        c._records.push_back(std::move(a));
    }
    return c;
}

}  // namespace command_detail

[[nodiscard]] inline auto encode_command(const stream_command& c) -> std::vector<std::byte> {
    using command_detail::encode_append;
    codec::writer w;
    w.u8(stream_command_version).u8(static_cast<std::uint8_t>(c.index()));
    std::visit(
        [&w]<typename T>(const T& v) {
            if constexpr (std::is_same_v<T, create_stream_command>) {
                w.str(v._account).str(v._name).u32(v._shard_count);
                w.u8(static_cast<std::uint8_t>(v._mode));
                w.u32(v._retention_hours).i64(v._created_ms);
                w.u32(static_cast<std::uint32_t>(v._tags.size()));
                for (const auto& [k, val] : v._tags) {
                    w.str(k).str(val);
                }
                w.u8(static_cast<std::uint8_t>(v._encryption)).str(v._key_id);
            } else if constexpr (std::is_same_v<T, set_stream_status_command>) {
                w.str(v._account).str(v._name).u64(v._id);
                w.u8(static_cast<std::uint8_t>(v._status));
            } else if constexpr (std::is_same_v<T, remove_stream_command>) {
                w.str(v._account).str(v._name).u64(v._id);
            } else if constexpr (std::is_same_v<T, open_shard_command>) {
                w.str(v._key).u64(v._starting_counter);
            } else if constexpr (std::is_same_v<T, append_records_command>) {
                encode_append(w, v);
            } else if constexpr (std::is_same_v<T, append_batch_command>) {
                w.u32(static_cast<std::uint32_t>(v._appends.size()));
                for (const auto& a : v._appends) {
                    encode_append(w, a);
                }
            } else if constexpr (std::is_same_v<T, trim_shard_command>) {
                w.str(v._key).u64(v._before);
            } else {
                w.str(v._key);
            }
        },
        c);
    return w.take();
}

[[nodiscard]] inline auto decode_command(std::span<const std::byte> bytes) -> stream_command {
    codec::reader r(bytes);
    if (r.u8() != stream_command_version) {
        throw kinesis_codec_error("kinesis codec: unknown command version");
    }
    const auto kind = r.u8();
    stream_command out;
    switch (kind) {
        case 0: {
            create_stream_command c;
            c._account = r.str();
            c._name = r.str();
            c._shard_count = r.u32();
            c._mode = r.enumeration<stream_mode>(1);
            c._retention_hours = r.u32();
            c._created_ms = r.i64();
            const auto n = r.u32();
            for (std::uint32_t i = 0; i < n; ++i) {
                auto k = r.str();
                c._tags[std::move(k)] = r.str();
            }
            c._encryption = r.enumeration<encryption_type>(1);
            c._key_id = r.str();
            out = std::move(c);
            break;
        }
        case 1: {
            set_stream_status_command c;
            c._account = r.str();
            c._name = r.str();
            c._id = r.u64();
            c._status = r.enumeration<stream_status>(3);
            out = std::move(c);
            break;
        }
        case 2: {
            remove_stream_command c;
            c._account = r.str();
            c._name = r.str();
            c._id = r.u64();
            out = std::move(c);
            break;
        }
        case 3: {
            open_shard_command c;
            c._key = r.str();
            c._starting_counter = r.u64();
            out = std::move(c);
            break;
        }
        case 4:
            out = command_detail::decode_append(r);
            break;
        case 5:
            out = close_shard_command{r.str()};
            break;
        case 6:
            out = drop_shard_command{r.str()};
            break;
        case 7: {
            trim_shard_command c;
            c._key = r.str();
            c._before = r.u64();
            out = std::move(c);
            break;
        }
        case 8: {
            append_batch_command c;
            const auto n = r.u32();
            c._appends.reserve(std::min<std::size_t>(n, r.remaining() / 16));
            for (std::uint32_t i = 0; i < n; ++i) {
                c._appends.push_back(command_detail::decode_append(r));
            }
            out = std::move(c);
            break;
        }
        default:
            throw kinesis_codec_error("kinesis codec: unknown command kind");
    }
    r.expect_end("command");
    return out;
}

// ---- results --------------------------------------------------------------

enum class apply_status : std::uint8_t {
    ok = 0,
    /// The stream, or the shard's routing key, is not here.
    not_found = 1,
    /// `CreateStream` for a name in use (ResourceInUseException).
    already_exists = 2,
    /// An append to a closed shard; the front end re-reads the shard table.
    shard_closed = 3,
    /// Malformed command or a value out of range (shard count 0, bad name,
    /// a record over the store's limit). Same on every replica.
    invalid = 4,
    /// A status change the lifecycle does not allow.
    bad_transition = 5,
    /// Never from `apply()`: the leader's write buckets refused the append
    /// before proposing it (`kinesis_throttle.hpp`).
    throttled = 6,
};

/// One part of an `append_batch_command`'s result: what that shard's
/// append alone would have answered.
struct append_part_result {
    apply_status _status = apply_status::ok;
    std::uint64_t _first_counter = 0;
    std::uint64_t _last_counter = 0;
    std::int64_t _arrival_ms = 0;
    friend auto operator==(const append_part_result&, const append_part_result&) -> bool = default;
};

/// The decoded reply of `apply()`. `_id` is the new stream's id
/// (`create_stream`); the counters and arrival time describe an append;
/// `_parts` holds a batch's per-shard results, in order.
struct apply_result {
    apply_status _status = apply_status::ok;
    std::uint64_t _id = 0;
    std::uint64_t _first_counter = 0;
    std::uint64_t _last_counter = 0;
    std::int64_t _arrival_ms = 0;
    std::vector<append_part_result> _parts{};
    friend auto operator==(const apply_result&, const apply_result&) -> bool = default;
};

/// Parts are written only when present, so every other result encodes as
/// it did before batches existed.
[[nodiscard]] inline auto encode_result(const apply_result& r) -> std::vector<std::byte> {
    codec::writer w;
    w.u8(static_cast<std::uint8_t>(r._status)).u64(r._id);
    w.u64(r._first_counter).u64(r._last_counter).i64(r._arrival_ms);
    if (!r._parts.empty()) {
        w.u32(static_cast<std::uint32_t>(r._parts.size()));
        for (const auto& p : r._parts) {
            w.u8(static_cast<std::uint8_t>(p._status));
            w.u64(p._first_counter).u64(p._last_counter).i64(p._arrival_ms);
        }
    }
    return w.take();
}

[[nodiscard]] inline auto decode_result(std::span<const std::byte> bytes) -> apply_result {
    codec::reader r(bytes);
    apply_result out;
    out._status = r.enumeration<apply_status>(6);
    out._id = r.u64();
    out._first_counter = r.u64();
    out._last_counter = r.u64();
    out._arrival_ms = r.i64();
    if (r.remaining() > 0) {
        const auto n = r.u32();
        out._parts.reserve(std::min<std::size_t>(n, r.remaining() / 25));
        for (std::uint32_t i = 0; i < n; ++i) {
            append_part_result p;
            p._status = r.enumeration<apply_status>(6);
            p._first_counter = r.u64();
            p._last_counter = r.u64();
            p._arrival_ms = r.i64();
            out._parts.push_back(p);
        }
    }
    r.expect_end("apply result");
    return out;
}

// ---- the state machine ----------------------------------------------------

/// Called during `restore_from_snapshot` for an active segment the local
/// store lacks: fetch and install it (`record_segment_store::fetch_segment`
/// from the group's peers), or throw. Restore cannot finish without it.
using segment_fetcher = std::function<void(const record_segment_descriptor&)>;

template<typename LogIndex = std::uint64_t>
requires log_index<LogIndex>
class stream_state_machine {
public:
    using key_type = std::string;
    using entry = std::variant<std::uint64_t, stream_record, shard_state>;
    using map_type = std::map<std::string, entry, std::less<>>;

    stream_state_machine() = default;
    explicit stream_state_machine(record_segment_store* store, segment_fetcher fetch = {})
        : _store(store), _fetch(std::move(fetch)) {}

    // ---- kythira::state_machine -------------------------------------------

    auto apply(const std::vector<std::byte>& command, LogIndex index) -> std::vector<std::byte> {
        _last_applied_index = index;
        stream_command c;
        try {
            c = decode_command(command);
        } catch (const kinesis_codec_error&) {
            return encode_result({._status = apply_status::invalid});
        }
        return encode_result(std::visit([this](auto& v) { return this->apply_one(v); }, c));
    }

    /// The whole map in key order. Also the format of every blob
    /// `split_state` returns, so the split/absorb law holds by construction.
    [[nodiscard]] auto get_state() const -> std::vector<std::byte> {
        return serialize(_map.begin(), _map.end());
    }

    auto restore_from_snapshot(const std::vector<std::byte>& snapshot_data, LogIndex index)
        -> void {
        map_type incoming;
        std::vector<record_segment_descriptor> active;
        deserialize(snapshot_data, incoming, &active);
        if (_store != nullptr) {
            for (const auto& [key, e] : incoming) {
                if (const auto* s = std::get_if<shard_state>(&e)) {
                    // A tail past the snapshot (written before a crash, its
                    // entries not yet re-applied) goes; the log replays it.
                    _store->truncate_after(key, s->_last_counter == 0
                                                    ? std::nullopt
                                                    : std::optional{s->_last_counter});
                }
            }
            for (const auto& d : active) {
                if (!_store->has_segment(d)) {
                    if (!_fetch) {
                        throw record_segment_error("kinesis: snapshot names an active segment of " +
                                                   d._stream +
                                                   " this node lacks, and no fetcher is set");
                    }
                    _fetch(d);
                }
            }
        }
        _map = std::move(incoming);
        _last_applied_index = index;
    }

    [[nodiscard]] auto last_applied_index() const noexcept -> LogIndex {
        return _last_applied_index;
    }

    // ---- kythira::splittable_state_machine ---------------------------------

    [[nodiscard]] auto approximate_size_bytes() const noexcept -> std::size_t {
        std::size_t n = 0;
        for (const auto& [key, e] : _map) {
            n += key.size() + weight(e);
        }
        return n;
    }
    [[nodiscard]] auto approximate_key_count() const noexcept -> std::size_t { return _map.size(); }

    /// Routing keys only (cutting at a routing key keeps it whole on the
    /// right), spaced by retained bytes, never the first key.
    [[nodiscard]] auto suggest_split_keys(std::size_t max) -> std::vector<key_type> {
        std::vector<key_type> out;
        if (max == 0 || _map.size() < 2) {
            return out;
        }
        std::uint64_t total = 0;
        for (const auto& [key, e] : _map) {
            total += weight(e);
        }
        const std::uint64_t step = total / (max + 1);
        std::uint64_t acc = 0;
        std::uint64_t next = step;
        bool first = true;
        for (const auto& [key, e] : _map) {
            if (!first && out.size() < max && acc >= next &&
                std::holds_alternative<shard_state>(e)) {
                out.push_back(key);
                while (next <= acc) {
                    next += std::max<std::uint64_t>(step, 1);
                }
            }
            first = false;
            acc += weight(e);
        }
        return out;
    }

    /// Requirement 6.4: never inside a routing key (a key that extends one)
    /// and never inside the catalog, which must stay in one group so stream
    /// ids are allocated without a cross-group transaction. Cutting exactly
    /// at a routing key or at the catalog prefix keeps each whole.
    [[nodiscard]] auto can_split_at(const key_type& key) const -> bool {
        const auto& cat = catalog_prefix();
        if (key.size() > cat.size() && key.starts_with(cat)) {
            return false;
        }
        if (key.size() > routing_key_size &&
            parse_routing_key(std::string_view(key).substr(0, routing_key_size))) {
            return false;
        }
        return true;
    }

    [[nodiscard]] auto split_state(const std::vector<key_type>& keys)
        -> std::vector<std::vector<std::byte>> {
        std::vector<key_type> cuts = keys;
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        std::vector<std::vector<std::byte>> out;
        out.reserve(cuts.size() + 1);
        auto it = _map.begin();
        for (const auto& cut : cuts) {
            // Cuts are sorted, so each bound is at or after the last one.
            auto end = _map.lower_bound(cut);
            out.push_back(serialize(it, end));
            it = end;
        }
        out.push_back(serialize(it, _map.end()));
        return out;
    }

    /// Take on a sibling's entries. The store is shared by the node's
    /// groups, so the sibling's segments are already here; its active
    /// descriptors are ignored.
    auto absorb(const std::vector<std::byte>& other_state,
                [[maybe_unused]] const shard_range<key_type>& range) -> void {
        map_type incoming;
        deserialize(other_state, incoming, nullptr);
        for (auto& [key, e] : incoming) {
            _map.insert_or_assign(key, std::move(e));
        }
    }

    // ---- reads -------------------------------------------------------------

    [[nodiscard]] auto find_stream(std::string_view account, std::string_view name) const
        -> const stream_record* {
        auto it = _map.find(stream_key(account, name));
        return it == _map.end() ? nullptr : std::get_if<stream_record>(&it->second);
    }

    /// One account's stream records in name order (the key order), from
    /// `start_after`.
    [[nodiscard]] auto list_streams(std::string_view account, std::string_view start_after = {},
                                    std::size_t limit = SIZE_MAX) const
        -> std::vector<const stream_record*> {
        std::vector<const stream_record*> out;
        const auto prefix = account_streams_prefix(account);
        auto it = start_after.empty() ? _map.lower_bound(prefix)
                                      : _map.upper_bound(stream_key(account, start_after));
        for (; it != _map.end() && out.size() < limit && it->first.starts_with(prefix); ++it) {
            if (const auto* s = std::get_if<stream_record>(&it->second)) {
                out.push_back(s);
            }
        }
        return out;
    }

    [[nodiscard]] auto find_shard(std::string_view key) const -> const shard_state* {
        auto it = _map.find(key);
        return it == _map.end() ? nullptr : std::get_if<shard_state>(&it->second);
    }

    /// The store record bytes live in (null for a catalog-only machine), and
    /// the fetcher that installs a segment from the group's peers. Readers
    /// copy a shard's state under the cluster's read and then read the
    /// store outside it, so `GetRecords` file I/O never holds up `apply()`.
    [[nodiscard]] auto store() const noexcept -> record_segment_store* { return _store; }
    [[nodiscard]] auto fetcher() const -> const segment_fetcher& { return _fetch; }

    /// The id the next `CreateStream` gets.
    [[nodiscard]] auto next_stream_id() const -> std::uint64_t {
        auto it = _map.find(next_id_key());
        const auto* v = it == _map.end() ? nullptr : std::get_if<std::uint64_t>(&it->second);
        return v == nullptr ? 1 : *v;
    }

    [[nodiscard]] auto entries() const noexcept -> const map_type& { return _map; }

    // ---- codec, public so tests can check the split/absorb law -------------

    static constexpr std::uint8_t snapshot_version = 1;

    template<typename It>
    [[nodiscard]] auto serialize(It begin, It end) const -> std::vector<std::byte> {
        codec::writer w;
        w.u8(snapshot_version).u32(static_cast<std::uint32_t>(std::distance(begin, end)));
        for (auto it = begin; it != end; ++it) {
            const auto& [key, e] = *it;
            w.str(key).u8(static_cast<std::uint8_t>(e.index()));
            if (const auto* v = std::get_if<std::uint64_t>(&e)) {
                w.u64(*v);
            } else if (const auto* s = std::get_if<stream_record>(&e)) {
                encode(w, *s);
            } else {
                const auto& sh = std::get<shard_state>(e);
                w.u64(sh._last_counter).i64(sh._last_arrival_ms).u8(sh._open ? 1 : 0);
                w.u64(sh._trim_before).u64(sh._retained_bytes).u64(sh._records);
                w.u32(static_cast<std::uint32_t>(sh._sealed.size()));
                for (const auto& d : sh._sealed) {
                    w.raw(encode_segment_descriptor(d));
                }
                auto act = active_descriptor(key);
                w.u8(act ? 1 : 0);
                if (act) {
                    w.raw(encode_segment_descriptor(*act));
                }
            }
        }
        return w.take();
    }

    static auto deserialize(std::span<const std::byte> blob, map_type& into,
                            std::vector<record_segment_descriptor>* active) -> void {
        codec::reader r(blob);
        if (r.u8() != snapshot_version) {
            throw kinesis_codec_error("kinesis codec: unknown snapshot version");
        }
        const auto n = r.u32();
        for (std::uint32_t i = 0; i < n; ++i) {
            auto key = r.str();
            switch (r.u8()) {
                case 0:
                    into.insert_or_assign(std::move(key), entry{r.u64()});
                    break;
                case 1:
                    into.insert_or_assign(std::move(key), entry{decode_stream_record(r)});
                    break;
                case 2: {
                    shard_state sh;
                    sh._last_counter = r.u64();
                    sh._last_arrival_ms = r.i64();
                    sh._open = r.boolean();
                    sh._trim_before = r.u64();
                    sh._retained_bytes = r.u64();
                    sh._records = r.u64();
                    const auto segs = r.u32();
                    for (std::uint32_t j = 0; j < segs; ++j) {
                        sh._sealed.push_back(descriptor(r));
                    }
                    if (r.boolean()) {
                        auto d = descriptor(r);
                        if (active != nullptr) {
                            active->push_back(std::move(d));
                        }
                    }
                    into.insert_or_assign(std::move(key), entry{std::move(sh)});
                    break;
                }
                default:
                    throw kinesis_codec_error("kinesis codec: unknown entry kind");
            }
        }
        r.expect_end("snapshot");
    }

private:
    static auto descriptor(codec::reader& r) -> record_segment_descriptor {
        auto d = decode_segment_descriptor(r.rest());
        if (!d) {
            throw kinesis_codec_error("kinesis codec: bad segment descriptor");
        }
        return std::move(*d);
    }

    [[nodiscard]] auto active_descriptor(const std::string& key) const
        -> std::optional<record_segment_descriptor> {
        if (_store == nullptr) {
            return std::nullopt;
        }
        auto local = _store->local_segments(key);
        if (local.empty() || local.back()._sealed) {
            return std::nullopt;
        }
        return std::move(local.back());
    }

    [[nodiscard]] static auto weight(const entry& e) noexcept -> std::uint64_t {
        if (const auto* s = std::get_if<shard_state>(&e)) {
            return 64 + s->_retained_bytes;
        }
        return 64;
    }

    auto shard_at(const std::string& key) -> shard_state* {
        auto it = _map.find(key);
        return it == _map.end() ? nullptr : std::get_if<shard_state>(&it->second);
    }

    auto apply_one(const create_stream_command& c) -> apply_result {
        if (!valid_account(c._account) || !valid_stream_name(c._name) || c._shard_count == 0 ||
            c._retention_hours == 0) {
            return {._status = apply_status::invalid};
        }
        const auto key = stream_key(c._account, c._name);
        if (_map.contains(key)) {
            return {._status = apply_status::already_exists};
        }
        const auto id = next_stream_id();
        stream_record s;
        s._id = id;
        s._account = c._account;
        s._name = c._name;
        s._mode = c._mode;
        s._retention_hours = c._retention_hours;
        s._created_ms = c._created_ms;
        s._shards = initial_shards(c._shard_count);
        s._next_ordinal = c._shard_count;
        s._tags = c._tags;
        s._encryption = c._encryption;
        s._key_id = c._key_id;
        _map.insert_or_assign(key, entry{std::move(s)});
        _map.insert_or_assign(next_id_key(), entry{id + 1});
        return {._status = apply_status::ok, ._id = id};
    }

    auto stream_at(const std::string& account, const std::string& name, std::uint64_t id)
        -> stream_record* {
        auto it = _map.find(stream_key(account, name));
        auto* s = it == _map.end() ? nullptr : std::get_if<stream_record>(&it->second);
        return s != nullptr && s->_id == id ? s : nullptr;
    }

    auto apply_one(const set_stream_status_command& c) -> apply_result {
        auto* s = stream_at(c._account, c._name, c._id);
        if (s == nullptr) {
            return {._status = apply_status::not_found};
        }
        if (!status_transition_allowed(s->_status, c._status)) {
            return {._status = apply_status::bad_transition};
        }
        s->_status = c._status;
        return {._id = s->_id};
    }

    auto apply_one(const remove_stream_command& c) -> apply_result {
        if (stream_at(c._account, c._name, c._id) == nullptr) {
            return {._status = apply_status::not_found};
        }
        _map.erase(stream_key(c._account, c._name));
        return {._id = c._id};
    }

    auto apply_one(const open_shard_command& c) -> apply_result {
        if (!parse_routing_key(c._key)) {
            return {._status = apply_status::invalid};
        }
        if (_map.contains(c._key)) {
            return {._status = apply_status::already_exists};
        }
        // A shard this machine does not know has no records yet. Files left
        // under its key are from an earlier life of this replica (a full
        // log replay after a restart with no snapshot re-applies every
        // append), so they go: replay then rewrites them byte for byte.
        if (_store != nullptr) {
            _store->drop_stream(c._key);
        }
        _map.insert_or_assign(c._key, entry{shard_state{._last_counter = c._starting_counter}});
        return {._last_counter = c._starting_counter};
    }

    auto apply_one(const append_records_command& c) -> apply_result {
        auto r = append_one(c);
        if (_store != nullptr && r._status == apply_status::ok) {
            _store->barrier();
        }
        return r;
    }

    auto apply_one(const append_batch_command& c) -> apply_result {
        if (c._appends.empty()) {
            return {._status = apply_status::invalid};
        }
        apply_result out;
        out._parts.reserve(c._appends.size());
        bool wrote = false;
        for (const auto& a : c._appends) {
            const auto r = append_one(a);
            wrote = wrote || r._status == apply_status::ok;
            out._parts.push_back({._status = r._status,
                                  ._first_counter = r._first_counter,
                                  ._last_counter = r._last_counter,
                                  ._arrival_ms = r._arrival_ms});
        }
        // One barrier for every shard's records: the point of the batch.
        if (_store != nullptr && wrote) {
            _store->barrier();
        }
        return out;
    }

    /// An append without the durability barrier, which the caller owns.
    auto append_one(const append_records_command& c) -> apply_result {
        auto* s = shard_at(c._key);
        if (s == nullptr) {
            return {._status = apply_status::not_found};
        }
        if (!s->_open) {
            return {._status = apply_status::shard_closed};
        }
        if (c._records.empty() || c._records.size() > UINT32_MAX ||
            s->_last_counter > UINT64_MAX - c._records.size()) {
            return {._status = apply_status::invalid};
        }
        std::vector<std::vector<std::byte>> payloads;
        payloads.reserve(c._records.size());
        std::uint64_t bytes = 0;
        for (const auto& r : c._records) {
            if (r._partition_key.empty() || r._partition_key.size() > 256) {
                return {._status = apply_status::invalid};
            }
            payloads.push_back(encode_stored_record(r._partition_key, r._data));
            if (_store != nullptr && payloads.back().size() > _store->options()._max_record_bytes) {
                return {._status = apply_status::invalid};
            }
            bytes += payloads.back().size();
        }
        const auto arrival = std::max(c._arrival_ms, s->_last_arrival_ms);
        const auto first = s->_last_counter + 1;
        if (_store != nullptr) {
            std::vector<segment_record_view> views;
            views.reserve(payloads.size());
            for (std::size_t i = 0; i < payloads.size(); ++i) {
                views.push_back(
                    {._seq = first + i, ._timestamp_ms = arrival, ._data = payloads[i]});
            }
            auto result = _store->append(c._key, views);
            for (auto& d : result._sealed) {
                s->_sealed.push_back(std::move(d));
            }
        }
        s->_last_counter = first + payloads.size() - 1;
        s->_last_arrival_ms = arrival;
        s->_records += payloads.size();
        s->_retained_bytes += bytes;
        return {._first_counter = first, ._last_counter = s->_last_counter, ._arrival_ms = arrival};
    }

    auto apply_one(const close_shard_command& c) -> apply_result {
        auto* s = shard_at(c._key);
        if (s == nullptr) {
            return {._status = apply_status::not_found};
        }
        if (s->_open) {
            s->_open = false;
            if (_store != nullptr) {
                if (auto d = _store->seal(c._key)) {
                    s->_sealed.push_back(std::move(*d));
                }
            }
        }
        return {._last_counter = s->_last_counter};
    }

    auto apply_one(const drop_shard_command& c) -> apply_result {
        if (shard_at(c._key) == nullptr) {
            return {._status = apply_status::not_found};
        }
        _map.erase(c._key);
        if (_store != nullptr) {
            _store->drop_stream(c._key);
        }
        return {};
    }

    auto apply_one(const trim_shard_command& c) -> apply_result {
        auto* s = shard_at(c._key);
        if (s == nullptr) {
            return {._status = apply_status::not_found};
        }
        // Never past the last record: the shard's position must survive.
        const auto before = std::min(c._before, s->_last_counter + 1);
        if (before <= s->_trim_before) {
            return {._last_counter = s->_last_counter};
        }
        s->_trim_before = before;
        if (_store != nullptr) {
            _store->trim_before(c._key, before);
        }
        // Same rule as the store: sealed segments whose last record is
        // below `before`. Applied to the index whether or not a store is
        // attached, so the index never names a segment the store dropped.
        auto keep = std::partition_point(
            s->_sealed.begin(), s->_sealed.end(),
            [before](const record_segment_descriptor& d) { return d._last_seq < before; });
        for (auto it = s->_sealed.begin(); it != keep; ++it) {
            s->_retained_bytes -= std::min(s->_retained_bytes, it->_bytes);
        }
        s->_sealed.erase(s->_sealed.begin(), keep);
        return {._last_counter = s->_last_counter};
    }

    map_type _map;
    record_segment_store* _store = nullptr;
    segment_fetcher _fetch;
    LogIndex _last_applied_index{};
};

}  // namespace kythira::kinesis
