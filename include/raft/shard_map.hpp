// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file shard_map.hpp
/// @brief The routing table: an ordered map from key-range start bound to
///        `shard_descriptor`, plus the executable form of the tiling invariant.
///
/// The map is a *cache with an authority per row*: the group that owns a range
/// is the authority for that range's descriptor, and every other copy is stale
/// the moment a split or merge commits. Epoch checks at request admission are
/// what make being stale safe; `check_tiling()` is what makes being *wrong*
/// loud.
///
/// See `.kiro/specs/multi-raft/design.md` §1.3.

#include <raft/shard_types.hpp>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kythira {

/// @brief Ordering functor for range start bounds, so `nullopt` sorts first.
///
/// `std::map`'s default `std::less<std::optional<Key>>` happens to agree, but
/// naming the comparator makes the map's key ordering an explicit contract
/// rather than an accident of the standard library's optional ordering.
template<shard_key Key> struct start_bound_less {
    using is_transparent = void;

    [[nodiscard]] auto operator()(const std::optional<Key>& lhs,
                                  const std::optional<Key>& rhs) const -> bool {
        return compare_start_bound<Key>(lhs, rhs) < 0;
    }
};

/// @brief The ordered routing table over shard descriptors.
///
/// @tparam GroupId Must satisfy `raft_group_id`.
/// @tparam Key     Must satisfy `shard_key`.
/// @tparam NodeId  Must satisfy `node_id`.
template<raft_group_id GroupId, shard_key Key, typename NodeId = std::uint64_t>
requires node_id<NodeId>
class shard_map {
public:
    using descriptor_type = shard_descriptor<GroupId, Key, NodeId>;
    using bound_type = std::optional<Key>;
    using container_type = std::map<bound_type, descriptor_type, start_bound_less<Key>>;

    shard_map() = default;

    /// @brief Seeds the map with a single `(-inf, +inf)` shard.
    ///
    /// Every cluster starts here; the first split is what makes the range
    /// bounds meaningful.
    [[nodiscard]] static auto single_shard(GroupId group, std::vector<NodeId> voters) -> shard_map {
        shard_map m;
        m.insert_unchecked(descriptor_type{._group_id = std::move(group),
                                           ._range = unbounded_shard_range<Key>(),
                                           ._epoch = shard_epoch{},
                                           ._voters = std::move(voters),
                                           ._learners = {},
                                           ._leader_hint = std::nullopt});
        return m;
    }

    // ── queries ──────────────────────────────────────────────────────────────

    /// @brief Resolves the shard owning `key`, or `nullopt` if the map has a
    /// hole there.
    ///
    /// `upper_bound` then step back: the owning row is the one with the
    /// greatest start bound not exceeding `key`. The `contains()` re-check is
    /// not redundant — a map with a gap will step back onto a row that ends
    /// before `key`, and returning it would route a command to a shard that
    /// does not own the key.
    [[nodiscard]] auto lookup(const Key& key) const -> std::optional<descriptor_type> {
        auto it = _rows.upper_bound(bound_type{key});
        if (it == _rows.begin()) {
            return std::nullopt;
        }
        --it;
        if (!it->second._range.contains(key)) {
            return std::nullopt;
        }
        return it->second;
    }

    /// @brief Every descriptor whose range overlaps `range`, in key order.
    [[nodiscard]] auto range_scan(const shard_range<Key>& range) const
        -> std::vector<descriptor_type> {
        std::vector<descriptor_type> out;
        for (const auto& [bound, desc] : _rows) {
            if (ranges_overlap(desc._range, range)) {
                out.push_back(desc);
            }
        }
        return out;
    }

    /// @brief The descriptor for `group`, if this map holds a row for it.
    [[nodiscard]] auto find(const GroupId& group) const -> std::optional<descriptor_type> {
        auto it = _by_group.find(group);
        if (it == _by_group.end()) {
            return std::nullopt;
        }
        auto row = _rows.find(it->second);
        if (row == _rows.end()) {
            return std::nullopt;
        }
        return row->second;
    }

    /// @brief The row immediately to the left of `group`'s range, if adjacent.
    [[nodiscard]] auto left_sibling(const GroupId& group) const -> std::optional<descriptor_type> {
        auto self = find(group);
        if (!self.has_value()) {
            return std::nullopt;
        }
        auto it = _rows.find(self->_range._start);
        if (it == _rows.end() || it == _rows.begin()) {
            return std::nullopt;
        }
        --it;
        if (!it->second._range.is_adjacent_left_of(self->_range)) {
            return std::nullopt;
        }
        return it->second;
    }

    /// @brief The row immediately to the right of `group`'s range, if adjacent.
    [[nodiscard]] auto right_sibling(const GroupId& group) const -> std::optional<descriptor_type> {
        auto self = find(group);
        if (!self.has_value()) {
            return std::nullopt;
        }
        auto it = _rows.find(self->_range._start);
        if (it == _rows.end()) {
            return std::nullopt;
        }
        ++it;
        if (it == _rows.end()) {
            return std::nullopt;
        }
        if (!self->_range.is_adjacent_left_of(it->second._range)) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] auto size() const -> std::size_t { return _rows.size(); }
    [[nodiscard]] auto empty() const -> bool { return _rows.empty(); }

    /// @brief Every descriptor in key order.
    [[nodiscard]] auto descriptors() const -> std::vector<descriptor_type> {
        std::vector<descriptor_type> out;
        out.reserve(_rows.size());
        for (const auto& [bound, desc] : _rows) {
            out.push_back(desc);
        }
        return out;
    }

    [[nodiscard]] auto begin() const { return _rows.begin(); }
    [[nodiscard]] auto end() const { return _rows.end(); }

    // ── mutation ─────────────────────────────────────────────────────────────

    /// @brief Merges one descriptor learned from elsewhere into the map.
    ///
    /// Returns `true` when the map changed. A row for a group already held at
    /// an epoch that is not older is ignored — that is the rule that keeps a
    /// client's retry loop from walking backwards when two rejections arrive
    /// out of order.
    ///
    /// After accepting a row, any *other* row overlapping it at a strictly
    /// lower `_version` is evicted. This is what repairs the map across a
    /// split (the children's higher versions evict the parent) and a merge
    /// (the survivor's higher version evicts the source), without the caller
    /// having to know which of the two happened.
    auto upsert(const descriptor_type& desc) -> bool {
        if (auto existing = find(desc._group_id);
            existing.has_value() && !(existing->_epoch < desc._epoch)) {
            return false;
        }
        erase_group(desc._group_id);

        std::vector<bound_type> superseded;
        for (const auto& [bound, row] : _rows) {
            if (row._group_id != desc._group_id && ranges_overlap(row._range, desc._range) &&
                row._epoch._version < desc._epoch._version) {
                superseded.push_back(bound);
            }
        }
        for (const auto& bound : superseded) {
            auto it = _rows.find(bound);
            if (it != _rows.end()) {
                _by_group.erase(it->second._group_id);
                _rows.erase(it);
            }
        }

        insert_unchecked(desc);
        return true;
    }

    /// @brief Merges a batch of descriptors, e.g. the payload of an
    /// epoch-mismatch rejection. Returns `true` when any row changed.
    auto upsert_all(const std::vector<descriptor_type>& descs) -> bool {
        bool changed = false;
        for (const auto& d : descs) {
            changed = upsert(d) || changed;
        }
        return changed;
    }

    /// @brief Replaces `parent`'s row with `children`.
    ///
    /// The parent's row is removed by group id rather than by range, because a
    /// derived child reuses the parent's group id with a narrower range and
    /// must not delete itself.
    auto apply_split(const descriptor_type& parent, const std::vector<descriptor_type>& children)
        -> void {
        erase_group(parent._group_id);
        for (const auto& child : children) {
            erase_group(child._group_id);
            insert_unchecked(child);
        }
        assert_tiling_in_debug("apply_split");
    }

    /// @brief Removes `source`'s row and installs the survivor's extended one.
    auto apply_merge(const descriptor_type& source, const descriptor_type& survivor) -> void {
        erase_group(source._group_id);
        erase_group(survivor._group_id);
        insert_unchecked(survivor);
        assert_tiling_in_debug("apply_merge");
    }

    /// @brief Removes the row for `group`, if present. Returns `true` if it was.
    auto erase_group(const GroupId& group) -> bool {
        auto it = _by_group.find(group);
        if (it == _by_group.end()) {
            return false;
        }
        _rows.erase(it->second);
        _by_group.erase(it);
        return true;
    }

    // ── the invariant ────────────────────────────────────────────────────────

    /// @brief Checks that the rows tile the key space exactly once.
    ///
    /// Returns `nullopt` when they do, and otherwise a human-readable
    /// description of the **first** gap or overlap, naming the offending
    /// bound. This is the executable form of Requirement 2.3 and the single
    /// most important assertion in the property suite (design §10, I1).
    ///
    /// An empty map is reported as tiled: a node that hosts no shards and has
    /// learned no routing rows has not violated anything.
    [[nodiscard]] auto check_tiling() const -> std::optional<std::string> {
        if (_rows.empty()) {
            return std::nullopt;
        }

        auto it = _rows.begin();
        if (it->second._range._start.has_value()) {
            return "shard map does not cover the key space below " +
                   detail::describe_value(*it->second._range._start) + ": lowest shard " +
                   detail::describe_value(it->second._group_id) + " starts there";
        }

        auto prev = it;
        for (++it; it != _rows.end(); ++it) {
            const auto& left = prev->second;
            const auto& right = it->second;

            if (!left._range._end.has_value()) {
                return "shard " + detail::describe_value(left._group_id) +
                       " is unbounded above but shard " + detail::describe_value(right._group_id) +
                       " starts at " + detail::describe_bound(right._range._start, "-inf") +
                       ": overlap";
            }
            const auto cmp = compare_start_bound<Key>(left._range._end, right._range._start);
            if (cmp < 0) {
                return "gap between shard " + detail::describe_value(left._group_id) +
                       " ending at " + detail::describe_bound(left._range._end, "+inf") +
                       " and shard " + detail::describe_value(right._group_id) + " starting at " +
                       detail::describe_bound(right._range._start, "-inf");
            }
            if (cmp > 0) {
                return "overlap between shard " + detail::describe_value(left._group_id) +
                       " ending at " + detail::describe_bound(left._range._end, "+inf") +
                       " and shard " + detail::describe_value(right._group_id) + " starting at " +
                       detail::describe_bound(right._range._start, "-inf");
            }
            prev = it;
        }

        if (prev->second._range._end.has_value()) {
            return "shard map does not cover the key space at or above " +
                   detail::describe_value(*prev->second._range._end) + ": highest shard " +
                   detail::describe_value(prev->second._group_id) + " ends there";
        }
        return std::nullopt;
    }

    /// @brief Whether two ranges share at least one key.
    [[nodiscard]] static auto ranges_overlap(const shard_range<Key>& a, const shard_range<Key>& b)
        -> bool {
        // Half-open ranges are disjoint exactly when one ends at or before the
        // other starts. An unbounded end is +inf and so never ends before
        // anything; an unbounded start is -inf and so nothing ends before it.
        auto ends_at_or_before_start = [](const shard_range<Key>& lhs,
                                          const shard_range<Key>& rhs) {
            if (!lhs._end.has_value() || !rhs._start.has_value()) {
                return false;
            }
            return !(*rhs._start < *lhs._end);
        };
        return !ends_at_or_before_start(a, b) && !ends_at_or_before_start(b, a);
    }

private:
    auto insert_unchecked(const descriptor_type& desc) -> void {
        _by_group[desc._group_id] = desc._range._start;
        _rows[desc._range._start] = desc;
    }

    /// @brief Asserts the tiling invariant in debug builds only.
    ///
    /// Kept as a call rather than an inline `assert` so the failure message
    /// carries which mutation produced the broken map — "apply_merge left a
    /// gap" localises a bug that "assertion failed" does not.
    auto assert_tiling_in_debug([[maybe_unused]] const char* origin) const -> void {
#ifndef NDEBUG
        if (auto problem = check_tiling(); problem.has_value()) {
            std::fprintf(stderr, "shard_map: %s broke the tiling invariant: %s\n", origin,
                         problem->c_str());
            assert(false && "shard_map tiling invariant violated");
        }
#endif
    }

    container_type _rows;
    std::unordered_map<GroupId, bound_type> _by_group;
};

}  // namespace kythira
