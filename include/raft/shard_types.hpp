// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file shard_types.hpp
/// @brief Value types for the multi-Raft sharding layer: keys, ranges, epochs,
///        descriptors, statistics, and the decision records a policy returns.
///
/// Everything in this header is a plain value: no I/O, no synchronisation, no
/// dependency on `node<Types>`. That is deliberate — these types cross the
/// boundary between the host (`multi_raft`), the policy channels, the
/// placement driver, and the Raft log itself, so they must be cheap to copy
/// and trivially serialisable.
///
/// See `.kiro/specs/multi-raft/design.md` §1 and §6.1.

#include <raft/types.hpp>
#include <algorithm>
#include <chrono>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace kythira {

namespace detail {

/// @brief Renders a value into a diagnostic message when it is streamable, and
/// a placeholder when it is not.
///
/// A diagnostic's whole value is naming the offending key, group or bound, but
/// neither `shard_key` nor `raft_group_id` requires streamability — an
/// application key may be a struct with only `<=>`. Degrading to a placeholder
/// keeps the diagnostic useful for the common case without narrowing either
/// concept for everyone.
template<typename T> [[nodiscard]] auto describe_value(const T& v) -> std::string {
    if constexpr (requires(std::ostringstream& os) { os << v; }) {
        std::ostringstream os;
        os << v;
        return os.str();
    } else {
        return "<opaque>";
    }
}

/// @brief Renders an optional range bound, with `nullopt` shown as an infinity.
template<typename T>
[[nodiscard]] auto describe_bound(const std::optional<T>& v, const char* unbounded) -> std::string {
    return v.has_value() ? describe_value(*v) : std::string{unbounded};
}

}  // namespace detail

/// @brief Concept for a routing key.
///
/// Kythira never interprets a key beyond comparing and copying it — the
/// application owns its meaning. A key must be totally ordered because the
/// routing table is an ordered map over half-open ranges, copyable because
/// descriptors are values, and default-initialisable so that container
/// machinery works without a sentinel.
template<typename K>
concept shard_key = std::totally_ordered<K> && std::copyable<K> && std::default_initializable<K>;

/// @brief Concept for a Raft group identifier.
///
/// Adopted from MicroRaft's `[group id, node id]` composite replica identity.
/// Hashability is required because the host's registry and the transport
/// demultiplexer are both hash maps keyed on the group id; total ordering is
/// required so that a set of group ids has a stable presentation order in logs
/// and reports.
///
/// Every use site in this library defaults it to `std::uint64_t`.
template<typename T>
concept raft_group_id = std::regular<T> && std::totally_ordered<T> && requires(const T& g) {
    { std::hash<T>{}(g) } -> std::convertible_to<std::size_t>;
};

// ─────────────────────────────────────────────────────────────────────────────
// Ranges
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Orders two range *start* bounds, with `nullopt` meaning "unbounded
/// below" and therefore sorting first.
///
/// This is exactly `std::optional`'s own ordering, named here so that the
/// asymmetry with `compare_end_bound` is visible at both call sites rather
/// than only at one.
template<shard_key Key>
[[nodiscard]] constexpr auto compare_start_bound(const std::optional<Key>& lhs,
                                                 const std::optional<Key>& rhs)
    -> std::strong_ordering {
    if (!lhs.has_value() && !rhs.has_value()) {
        return std::strong_ordering::equal;
    }
    if (!lhs.has_value()) {
        return std::strong_ordering::less;
    }
    if (!rhs.has_value()) {
        return std::strong_ordering::greater;
    }
    return *lhs < *rhs   ? std::strong_ordering::less
           : *rhs < *lhs ? std::strong_ordering::greater
                         : std::strong_ordering::equal;
}

/// @brief Orders two range *end* bounds, with `nullopt` meaning "unbounded
/// above" and therefore sorting **last**.
///
/// The inversion relative to `compare_start_bound` is the whole reason both
/// helpers exist: `std::optional`'s built-in ordering puts `nullopt` first,
/// which is right for a start bound and exactly backwards for an end bound.
/// Using the built-in ordering for both would make `(-inf, +inf)` sort before
/// `(-inf, b)`, and the routing table's tiling checks would report phantom
/// overlaps.
template<shard_key Key>
[[nodiscard]] constexpr auto compare_end_bound(const std::optional<Key>& lhs,
                                               const std::optional<Key>& rhs)
    -> std::strong_ordering {
    if (!lhs.has_value() && !rhs.has_value()) {
        return std::strong_ordering::equal;
    }
    if (!lhs.has_value()) {
        return std::strong_ordering::greater;
    }
    if (!rhs.has_value()) {
        return std::strong_ordering::less;
    }
    return *lhs < *rhs   ? std::strong_ordering::less
           : *rhs < *lhs ? std::strong_ordering::greater
                         : std::strong_ordering::equal;
}

/// @brief A half-open key range `[start, end)` with explicit unbounded ends.
///
/// `std::optional` for each bound is what lets the initial single shard be
/// `(-inf, +inf)` and its first split work without the application reserving a
/// minimum and a maximum value out of its own key domain. TiKV can use `""`
/// and `b"\xff…"` because its key domain is bytes; Kythira's domain is
/// whatever the application chose, and it may have no representable extremes.
///
/// @tparam Key Must satisfy `shard_key`.
template<shard_key Key> struct shard_range {
    std::optional<Key> _start;  ///< `nullopt` == unbounded below.
    std::optional<Key> _end;    ///< `nullopt` == unbounded above.

    [[nodiscard]] auto start() const -> const std::optional<Key>& { return _start; }
    [[nodiscard]] auto end() const -> const std::optional<Key>& { return _end; }

    /// @brief Whether `k` falls in this half-open range.
    [[nodiscard]] auto contains(const Key& k) const -> bool {
        if (_start.has_value() && k < *_start) {
            return false;
        }
        if (_end.has_value() && !(k < *_end)) {
            return false;
        }
        return true;
    }

    /// @brief Whether `k` is a legal place to CUT this range.
    ///
    /// Stricter than `contains()`, by exactly one key: the start bound itself.
    /// A range is `[start, end)`, so cutting at `start` produces a left child
    /// `[start, start)` — an empty, degenerate shard that owns nothing, cannot
    /// be routed to, and whose row overlaps its right sibling's in the routing
    /// map.
    ///
    /// This is not a hypothetical. A state machine asked to suggest split keys
    /// naturally returns keys it holds, and the smallest key it holds is very
    /// often the shard's own lower bound — so `contains()` accepts it and the
    /// split silently produces an empty child that then evicts its sibling's
    /// routing row. The invariant property test found exactly that.
    [[nodiscard]] auto is_interior(const Key& k) const -> bool {
        if (!contains(k)) {
            return false;
        }
        return !_start.has_value() || *_start < k;
    }

    /// @brief Whether this range's end bound is exactly `other`'s start bound.
    ///
    /// Two unbounded sides are never adjacent: `(-inf, +inf)` is not adjacent
    /// to anything, and neither is a pair whose facing bounds are both open,
    /// because there is no key at which one stops and the other starts.
    [[nodiscard]] auto is_adjacent_left_of(const shard_range& other) const -> bool {
        return _end.has_value() && other._start.has_value() && *_end == *other._start;
    }

    /// @brief Whether the range admits no key at all (`start >= end`).
    ///
    /// A range with either bound unbounded is never empty.
    [[nodiscard]] auto is_empty() const -> bool {
        if (!_start.has_value() || !_end.has_value()) {
            return false;
        }
        return !(*_start < *_end);
    }

    /// @brief Whether this range fully covers `other`.
    [[nodiscard]] auto covers(const shard_range& other) const -> bool {
        return compare_start_bound<Key>(_start, other._start) <= 0 &&
               compare_end_bound<Key>(_end, other._end) >= 0;
    }

    /// @brief Orders by start bound, then by end bound.
    ///
    /// `nullopt` sorts first as a start bound and last as an end bound, so the
    /// ordering matches the geometric left-to-right ordering of ranges on the
    /// key line — see `compare_start_bound` / `compare_end_bound`.
    [[nodiscard]] auto operator<=>(const shard_range& other) const -> std::strong_ordering {
        if (auto c = compare_start_bound<Key>(_start, other._start); c != 0) {
            return c;
        }
        return compare_end_bound<Key>(_end, other._end);
    }

    [[nodiscard]] auto operator==(const shard_range& other) const -> bool {
        return _start == other._start && _end == other._end;
    }
};

/// @brief The unbounded range every freshly bootstrapped shard map starts with.
template<shard_key Key> [[nodiscard]] constexpr auto unbounded_shard_range() -> shard_range<Key> {
    return shard_range<Key>{._start = std::nullopt, ._end = std::nullopt};
}

namespace detail {

/// @brief Renders a range as `[start, end)` for diagnostics.
template<shard_key Key>
[[nodiscard]] auto describe_range(const shard_range<Key>& r) -> std::string {
    return "[" + describe_bound(r._start, "-inf") + ", " + describe_bound(r._end, "+inf") + ")";
}

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Epoch
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Version pair identifying a shard's generation, exactly TiKV's `RegionEpoch`.
///
/// The two counters are separate because a membership change and a range
/// change are independently stale-making: a client holding an old `_version`
/// has the wrong *range*, while a peer holding an old `_conf_version` has the
/// wrong *voters*. Collapsing them into one counter would force a client to
/// refresh its routing table on every membership change, and a peer to
/// re-derive its voter set on every split.
///
/// The rules, enforced at both admission and apply time:
/// - split into N children: every child takes `version = parent.version + N`;
/// - merge: the survivor takes `version = max(src.version, tgt.version) + 1`;
/// - any committed configuration entry: `conf_version + 1`.
struct shard_epoch {
    std::uint64_t _version{0};       ///< Incremented on every range change.
    std::uint64_t _conf_version{0};  ///< Incremented on every membership change.

    [[nodiscard]] auto version() const -> std::uint64_t { return _version; }
    [[nodiscard]] auto conf_version() const -> std::uint64_t { return _conf_version; }

    auto operator<=>(const shard_epoch&) const = default;
};

inline auto operator<<(std::ostream& os, const shard_epoch& e) -> std::ostream& {
    return os << "{v=" << e._version << ",cv=" << e._conf_version << "}";
}

// ─────────────────────────────────────────────────────────────────────────────
// Descriptor
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The authoritative routing record for one shard.
///
/// A descriptor is a *value*: the owning group's Raft log is the authority for
/// its contents, and every other copy in the cluster — a client's routing
/// cache, a peer's shard map, a placement driver's view — is a cache that the
/// epoch check makes safe to be stale.
///
/// @tparam GroupId Must satisfy `raft_group_id`.
/// @tparam Key     Must satisfy `shard_key`.
/// @tparam NodeId  Must satisfy `node_id`.
template<raft_group_id GroupId, shard_key Key, typename NodeId = std::uint64_t>
requires node_id<NodeId>
struct shard_descriptor {
    GroupId _group_id{};
    shard_range<Key> _range{};
    shard_epoch _epoch{};
    std::vector<NodeId> _voters{};
    std::vector<NodeId> _learners{};
    std::optional<NodeId> _leader_hint{};

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group_id; }
    [[nodiscard]] auto range() const -> const shard_range<Key>& { return _range; }
    [[nodiscard]] auto epoch() const -> const shard_epoch& { return _epoch; }
    [[nodiscard]] auto voters() const -> const std::vector<NodeId>& { return _voters; }
    [[nodiscard]] auto learners() const -> const std::vector<NodeId>& { return _learners; }
    [[nodiscard]] auto leader_hint() const -> const std::optional<NodeId>& { return _leader_hint; }

    /// @brief Whether `n` is a voter of this shard.
    [[nodiscard]] auto has_voter(const NodeId& n) const -> bool {
        return std::find(_voters.begin(), _voters.end(), n) != _voters.end();
    }

    /// @brief Whether `n` holds a replica of this shard in any role.
    [[nodiscard]] auto has_replica(const NodeId& n) const -> bool {
        return has_voter(n) || std::find(_learners.begin(), _learners.end(), n) != _learners.end();
    }

    [[nodiscard]] auto operator==(const shard_descriptor& other) const -> bool {
        return _group_id == other._group_id && _range == other._range && _epoch == other._epoch &&
               _voters == other._voters && _learners == other._learners &&
               _leader_hint == other._leader_hint;
    }
};

/// @brief Whether two descriptors name the same node set in the same roles.
///
/// Colocation is a merge precondition (design §5.5): each target replica
/// absorbs state from the source replica *on its own machine*, so a target
/// replica with no local source peer cannot apply `merge_commit`. Order is
/// insignificant, which is why this is not `_voters == _voters`.
template<raft_group_id GroupId, shard_key Key, typename NodeId>
[[nodiscard]] auto is_colocated(const shard_descriptor<GroupId, Key, NodeId>& lhs,
                                const shard_descriptor<GroupId, Key, NodeId>& rhs) -> bool {
    auto same_set = [](std::vector<NodeId> a, std::vector<NodeId> b) {
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        return a == b;
    };
    return same_set(lhs._voters, rhs._voters) && same_set(lhs._learners, rhs._learners);
}

// ─────────────────────────────────────────────────────────────────────────────
// Operation state
// ─────────────────────────────────────────────────────────────────────────────

/// @brief What a shard is currently doing, per design §6.6.
///
/// Conflicting operations are made impossible **by construction** rather than
/// by check-then-act: starting an operation *is* a transition, and only
/// `stable` admits a new one. `frozen` admits nothing automatic but still
/// admits an explicit admin command — freezing an operator out of their own
/// escape hatch would be a bad joke at 3 a.m.
enum class shard_operation_state : std::uint8_t {
    stable = 0,
    splitting = 1,
    merging_source = 2,
    merging_target = 3,
    frozen = 4,
    tombstoned = 5,
};

inline auto to_string(shard_operation_state s) -> std::string {
    switch (s) {
        case shard_operation_state::stable:
            return "stable";
        case shard_operation_state::splitting:
            return "splitting";
        case shard_operation_state::merging_source:
            return "merging_source";
        case shard_operation_state::merging_target:
            return "merging_target";
        case shard_operation_state::frozen:
            return "frozen";
        case shard_operation_state::tombstoned:
            return "tombstoned";
        default:
            return "unknown";
    }
}

inline auto operator<<(std::ostream& os, shard_operation_state s) -> std::ostream& {
    return os << to_string(s);
}

// ─────────────────────────────────────────────────────────────────────────────
// Key and node-id codecs
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Turns a routing key into bytes and back.
///
/// Needed because a split's chosen keys travel **inside a Raft log entry**:
/// every replica must decode exactly the keys the leader encoded, or they will
/// cut their state machines in different places. `shard_key` itself requires
/// only ordering and copying — deliberately, since Kythira never interprets a
/// key — so the encoding has to come from somewhere else, and this is it.
template<typename C, typename Key>
concept shard_key_codec = requires(const C& c, const Key& k, const std::vector<std::byte>& bytes) {
    { c.encode(k) } -> std::same_as<std::vector<std::byte>>;
    { c.decode(bytes) } -> std::same_as<Key>;
};

/// @brief Codec for the two key types this library ships support for.
///
/// `std::string` round-trips its bytes; an unsigned integral round-trips as
/// eight big-endian bytes. Big-endian, not native: a log entry written on one
/// machine is decoded on another, and a native-endian encoding would make the
/// split silently cut somewhere else on a differently-ordered peer.
///
/// An application whose key is neither supplies its own codec.
template<shard_key Key> struct default_shard_key_codec {
    [[nodiscard]] auto encode(const Key& k) const -> std::vector<std::byte> {
        std::vector<std::byte> out;
        if constexpr (std::same_as<Key, std::string>) {
            out.reserve(k.size());
            for (char c : k) {
                out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
            }
        } else {
            static_assert(std::unsigned_integral<Key>,
                          "default_shard_key_codec supports std::string and unsigned integral "
                          "keys; supply your own codec for anything else");
            auto v = static_cast<std::uint64_t>(k);
            out.resize(sizeof(std::uint64_t));
            for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
                out[sizeof(std::uint64_t) - 1 - i] = static_cast<std::byte>(v & 0xFFU);
                v >>= 8U;
            }
        }
        return out;
    }

    [[nodiscard]] auto decode(const std::vector<std::byte>& bytes) const -> Key {
        if constexpr (std::same_as<Key, std::string>) {
            std::string out;
            out.reserve(bytes.size());
            for (auto b : bytes) {
                out.push_back(static_cast<char>(b));
            }
            return out;
        } else {
            static_assert(std::unsigned_integral<Key>,
                          "default_shard_key_codec supports std::string and unsigned integral "
                          "keys; supply your own codec for anything else");
            std::uint64_t v = 0;
            for (auto b : bytes) {
                v = (v << 8U) | static_cast<std::uint64_t>(b);
            }
            return static_cast<Key>(v);
        }
    }
};

/// @brief Wraps a pair of `std::function`s as a `shard_key_codec`.
///
/// The host holds its codec type-erased, because it is configuration; the
/// encode and decode functions want it as a concept-satisfying object. This
/// adapter is the join, and it exists so that the *concept* stays the contract
/// an application implements rather than being replaced by a pair of loose
/// callables nothing checks.
template<shard_key Key> struct key_codec_adapter {
    std::function<std::vector<std::byte>(const Key&)> _encode;
    std::function<Key(const std::vector<std::byte>&)> _decode;

    [[nodiscard]] auto encode(const Key& k) const -> std::vector<std::byte> { return _encode(k); }
    [[nodiscard]] auto decode(const std::vector<std::byte>& b) const -> Key { return _decode(b); }
};

template<typename E, typename D>
key_codec_adapter(E, D) -> key_codec_adapter<std::invoke_result_t<D, std::vector<std::byte>>>;

// ─────────────────────────────────────────────────────────────────────────────
// Decisions
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Why a split was proposed. Carried into logs and metrics as a dimension.
enum class split_reason : std::uint8_t {
    size = 0,        ///< Approximate state size crossed the split threshold.
    key_count = 1,   ///< Approximate key count crossed the split threshold.
    read_load = 2,   ///< Load-based split (design §6.3), read side.
    write_load = 3,  ///< Load-based split (design §6.3), write side.
    /// @brief Backpressure: a latency percentile, not a size or a rate.
    ///
    /// Exists so a policy that *does* act on latency can attribute its
    /// decision. The shipped threshold policy never produces it, and design
    /// §6.1.4 explains why: under whole-blob reads, read latency scales with
    /// shard size, so making it a default trigger would duplicate the size
    /// trigger it appears to complement — firing on big shards rather than
    /// busy ones.
    latency = 7,
    admin = 4,             ///< An explicit operator command.
    placement_driver = 5,  ///< An advisory operator from the placement driver.
    pre_split = 6,         ///< A bulk-load pre-split over an empty shard.
};

inline auto operator<<(std::ostream& os, split_reason r) -> std::ostream& {
    switch (r) {
        case split_reason::size:
            return os << "size";
        case split_reason::key_count:
            return os << "key_count";
        case split_reason::latency:
            return os << "latency";
        case split_reason::read_load:
            return os << "read_load";
        case split_reason::write_load:
            return os << "write_load";
        case split_reason::admin:
            return os << "admin";
        case split_reason::placement_driver:
            return os << "placement_driver";
        case split_reason::pre_split:
            return os << "pre_split";
        default:
            return os << "unknown";
    }
}

/// @brief Why a merge was proposed.
enum class merge_reason : std::uint8_t {
    size = 0,              ///< Both shards fell under the merge size threshold.
    key_count = 1,         ///< Both shards fell under the merge key-count threshold.
    admin = 2,             ///< An explicit operator command.
    placement_driver = 3,  ///< An advisory operator from the placement driver.
};

inline auto operator<<(std::ostream& os, merge_reason r) -> std::ostream& {
    switch (r) {
        case merge_reason::size:
            return os << "size";
        case merge_reason::key_count:
            return os << "key_count";
        case merge_reason::admin:
            return os << "admin";
        case merge_reason::placement_driver:
            return os << "placement_driver";
        default:
            return os << "unknown";
    }
}

/// @brief `to_string` for `split_reason`, for metric dimensions and logs.
[[nodiscard]] inline auto to_string(split_reason r) -> std::string {
    std::ostringstream os;
    os << r;
    return os.str();
}

/// @brief `to_string` for `merge_reason`.
[[nodiscard]] inline auto to_string(merge_reason r) -> std::string {
    std::ostringstream os;
    os << r;
    return os.str();
}

/// @brief A policy's answer to "should this shard split, and where".
///
/// An empty `_at_keys` with `_split == true` means "yes, but you choose" — the
/// host then falls through to the state machine's `suggest_split_keys`
/// (design §6.4).
template<shard_key Key> struct split_decision {
    bool _split{false};
    std::vector<Key> _at_keys{};
    split_reason _reason{split_reason::size};

    /// @brief Who decided, for the `policy` metric dimension (design §6.7).
    ///
    /// `reason` alone stops identifying the decider the moment two policies
    /// compose: both members of a composition can return `size`, and an
    /// operator looking at `split.proposed{reason=size}` learns nothing about
    /// which one to retune.
    ///
    /// A borrowed `const char*` rather than a `std::string`: this struct is
    /// returned from a call on every policy tick for every shard, and a policy
    /// name is a compile-time constant in every implementation anyone would
    /// write. **Must point at storage that outlives the decision** — a string
    /// literal or a static. `nullptr` means "unattributed", which is the right
    /// answer for a single configured policy that never named itself.
    const char* _policy{nullptr};

    [[nodiscard]] auto should_split() const -> bool { return _split; }
    [[nodiscard]] auto at_keys() const -> const std::vector<Key>& { return _at_keys; }
    [[nodiscard]] auto reason() const -> split_reason { return _reason; }
    [[nodiscard]] auto policy() const -> const char* { return _policy; }
};

/// @brief Which neighbour a shard merges into.
///
/// Not cosmetic: the survivor's replicas run `absorb` and the other group is
/// destroyed, so the direction decides which state machine does the work and
/// which group id disappears. A bare `bool merge` would leave that to the host
/// and make "merge this shard into its *left* neighbour" inexpressible.
enum class merge_direction : std::uint8_t {
    into_left_sibling = 0,
    into_right_sibling = 1,
};

inline auto operator<<(std::ostream& os, merge_direction d) -> std::ostream& {
    return os << (d == merge_direction::into_left_sibling ? "into_left_sibling"
                                                          : "into_right_sibling");
}

/// @brief A policy's answer about a merge: for, against, or not its business.
///
/// Tri-state, and the third state is the whole point. It looks like
/// over-engineering until policies compose (design §6.1.3): under the
/// composite's **unanimity** rule a merge proceeds only if some member says
/// `propose` and none says `veto`, so a two-state `bool` would make "I do not
/// reason about merges" indistinguishable from "I object to this merge" — and
/// those two answers have opposite effects. A load-only policy written against
/// a `bool` would silently veto every merge in the cluster while looking
/// perfectly correct in isolation.
///
/// `abstain` is the default so that a policy which never thinks about merges
/// behaves exactly as it did when this field was a `bool`.
enum class merge_verdict : std::uint8_t {
    propose = 0,  ///< This shard should merge, in `_direction`.
    abstain = 1,  ///< No opinion. Neither supports nor blocks a merge.
    veto = 2,     ///< This shard must NOT merge, whatever anyone else says.
};

inline auto operator<<(std::ostream& os, merge_verdict v) -> std::ostream& {
    switch (v) {
        case merge_verdict::propose:
            return os << "propose";
        case merge_verdict::abstain:
            return os << "abstain";
        case merge_verdict::veto:
            return os << "veto";
        default:
            return os << "unknown";
    }
}

[[nodiscard]] inline auto to_string(merge_verdict v) -> const char* {
    switch (v) {
        case merge_verdict::propose:
            return "propose";
        case merge_verdict::abstain:
            return "abstain";
        case merge_verdict::veto:
            return "veto";
        default:
            return "unknown";
    }
}

/// @brief A policy's answer to "should this shard merge into a neighbour".
struct merge_decision {
    merge_verdict _verdict{merge_verdict::abstain};
    merge_direction _direction{merge_direction::into_left_sibling};
    merge_reason _reason{merge_reason::size};

    /// @brief Who decided. See `split_decision::_policy` for the lifetime rule.
    ///
    /// It matters most on a veto: under the composite's unanimity rule a single
    /// member can hold every merge in the cluster hostage, and the first
    /// question an operator asks is *which one*.
    const char* _policy{nullptr};

    [[nodiscard]] auto verdict() const -> merge_verdict { return _verdict; }
    /// @brief Whether this decision, on its own, asks for a merge.
    ///
    /// Kept as a named predicate rather than leaving callers to compare against
    /// the enumerator, because `!should_merge()` must never be read as "vetoed"
    /// — an abstention satisfies it too, and conflating the two is exactly the
    /// bug the tri-state exists to prevent.
    [[nodiscard]] auto should_merge() const -> bool { return _verdict == merge_verdict::propose; }
    [[nodiscard]] auto vetoed() const -> bool { return _verdict == merge_verdict::veto; }
    [[nodiscard]] auto abstained() const -> bool { return _verdict == merge_verdict::abstain; }
    [[nodiscard]] auto direction() const -> merge_direction { return _direction; }
    [[nodiscard]] auto reason() const -> merge_reason { return _reason; }
    [[nodiscard]] auto policy() const -> const char* { return _policy; }

    /// @brief The conventional "yes, merge in this direction" answer.
    [[nodiscard]] static auto propose(merge_direction direction, merge_reason reason,
                                      const char* policy = nullptr) -> merge_decision {
        return merge_decision{._verdict = merge_verdict::propose,
                              ._direction = direction,
                              ._reason = reason,
                              ._policy = policy};
    }
    [[nodiscard]] static auto abstain() -> merge_decision { return merge_decision{}; }
    [[nodiscard]] static auto veto(merge_reason reason, const char* policy = nullptr)
        -> merge_decision {
        return merge_decision{._verdict = merge_verdict::veto,
                              ._direction = merge_direction::into_left_sibling,
                              ._reason = reason,
                              ._policy = policy};
    }
};

/// @brief One candidate split key surfaced by the load-based sampler (design §6.3).
///
/// The counts are of accesses strictly left of `_key` and at-or-right of it,
/// over one sampling window. The sampler emits the most balanced candidate;
/// a candidate whose `one_sided_fraction()` exceeds the configured bound means
/// the load is a single hot key and a split cannot help.
template<shard_key Key> struct hot_key_sample {
    Key _key{};
    std::uint64_t _left_accesses{0};
    std::uint64_t _right_accesses{0};

    [[nodiscard]] auto key() const -> const Key& { return _key; }
    [[nodiscard]] auto left_accesses() const -> std::uint64_t { return _left_accesses; }
    [[nodiscard]] auto right_accesses() const -> std::uint64_t { return _right_accesses; }

    [[nodiscard]] auto total_accesses() const -> std::uint64_t {
        return _left_accesses + _right_accesses;
    }

    /// @brief Fraction of accesses on the heavier side, in `[0.5, 1.0]`.
    ///
    /// Returns 1.0 for an empty sample: no observed accesses is indistinguishable
    /// from perfectly one-sided as far as "is a split worth it" goes, and
    /// treating it as balanced would propose splits from no evidence.
    [[nodiscard]] auto one_sided_fraction() const -> double {
        const auto total = total_accesses();
        if (total == 0) {
            return 1.0;
        }
        const auto heavier = _left_accesses > _right_accesses ? _left_accesses : _right_accesses;
        return static_cast<double>(heavier) / static_cast<double>(total);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Statistics
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Everything a split/merge policy is allowed to see about one shard.
///
/// A policy receives this by value and returns a decision; it performs no I/O
/// and mutates nothing (design §6.1). Load figures are measured by the host at
/// the routing layer, so load-based decisions work for *any* state machine,
/// including one with no sizing hooks at all.
///
/// @tparam GroupId  Must satisfy `raft_group_id`.
/// @tparam Key      Must satisfy `shard_key`.
/// @tparam NodeId   Must satisfy `node_id`.
/// @tparam LogIndex Must satisfy `log_index`.
template<raft_group_id GroupId, shard_key Key, typename NodeId = std::uint64_t,
         typename LogIndex = std::uint64_t>
requires node_id<NodeId> && log_index<LogIndex>
struct shard_stats {
    using group_id_type = GroupId;
    /// Exposed so a policy generic over its statistics type — the composite,
    /// above all — can name the key type without being told it separately.
    using key_type = Key;
    using node_id_type = NodeId;

    shard_descriptor<GroupId, Key, NodeId> _descriptor{};

    // ── size, from the splittable_state_machine extension (design §6.4) ──────
    std::size_t _approximate_size_bytes{0};
    std::size_t _approximate_key_count{0};
    /// @brief `false` when the state machine has no sizing hooks.
    ///
    /// Deliberate, and reported once at construction rather than left silent:
    /// a state machine without sizing hooks makes size-based split impossible,
    /// and an operator should not spend a week wondering why nothing splits.
    bool _size_available{false};

    // ── log and apply ────────────────────────────────────────────────────────
    std::size_t _log_size_bytes{0};
    LogIndex _last_applied_index{0};
    double _applied_entries_per_sec{0.0};

    // ── load, measured at the routing layer ──────────────────────────────────
    double _read_qps{0.0};
    double _write_qps{0.0};
    double _read_bytes_per_sec{0.0};
    double _write_bytes_per_sec{0.0};

    // ── latency (design §6.1.4) ──────────────────────────────────────────────
    //
    // Percentiles over a bounded, recent window — **never** over the lifetime
    // of the shard. Both come from the same `latency_digest`, with the same
    // window and the same estimator, because two latency percentiles that could
    // drift apart in meaning are worse than one.
    std::chrono::nanoseconds _p99_apply_latency{};

    /// @brief p99 of the time a client's read actually took, measured at the
    ///        routing layer so the sample includes shard-map lookup and epoch
    ///        validation.
    ///
    /// **Not a trigger of the default policy.** A read returns the whole
    /// state-machine blob, so read latency scales with shard size and would
    /// largely duplicate the size trigger it appears to complement — firing on
    /// big shards rather than busy ones. It is here as an input a custom or
    /// composite policy may treat as backpressure evidence, and as an
    /// operator-facing diagnostic.
    std::chrono::nanoseconds _p99_read_latency{};

    // ── history: the anti-oscillation inputs ─────────────────────────────────
    std::chrono::milliseconds _time_since_last_split{};
    std::chrono::milliseconds _time_since_last_merge{};
    std::chrono::milliseconds _leader_since{};

    // ── membership ───────────────────────────────────────────────────────────
    std::size_t _voter_count{0};
    std::size_t _learner_count{0};
    std::size_t _down_replica_count{0};

    // ── load-split sampler output; empty when sampling is off or inconclusive ─
    std::vector<hot_key_sample<Key>> _hot_key_samples{};

    [[nodiscard]] auto descriptor() const -> const shard_descriptor<GroupId, Key, NodeId>& {
        return _descriptor;
    }
    [[nodiscard]] auto group_id() const -> const GroupId& { return _descriptor._group_id; }
    [[nodiscard]] auto epoch() const -> const shard_epoch& { return _descriptor._epoch; }
    [[nodiscard]] auto size_available() const -> bool { return _size_available; }
};

}  // namespace kythira
