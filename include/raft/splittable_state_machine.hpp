// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file splittable_state_machine.hpp
/// @brief The optional extension a state machine implements to take part in
///        shard split and merge — and the two laws it must obey.
///
/// See `.kiro/specs/multi-raft/` design §6.4. Detected structurally with
/// `if constexpr`, never required: `ca_state_machine` and the example state
/// machines compile and run unchanged, and a shard whose state machine lacks
/// these hooks simply cannot be split by size (which the host reports once at
/// construction rather than leaving an operator to wonder about).
///
/// ### The split of responsibilities is the point
///
/// | Question | Answered by | When |
/// |---|---|---|
/// | *Should* this shard split? | policy / placement driver / admin | leader, policy tick |
/// | *Where* would be a good cut? | `suggest_split_keys` | leader, before proposing |
/// | Is this cut *forbidden*? | `can_split_at` | leader, before proposing |
/// | *Do* the cut | `split_state` | every replica, at apply |
/// | *Undo* the cut | `absorb` | every replica, at apply |
///
/// `can_split_at` is the feature an application actually reaches for: refusing
/// to cut between a row and its secondary-index entries, refusing to cut inside
/// a key group an application-level transaction spans, keeping a tenant's keys
/// in one shard so its operations stay single-shard.

#include <raft/shard_types.hpp>

#include <cstddef>
#include <vector>

namespace kythira {

/// @brief A state machine that can be cut apart and put back together.
///
/// ### Two laws, and why they are laws rather than conventions
///
/// **1. `split_state` and `absorb` must be deterministic across replicas.**
/// They run on *every* replica at the same log index, and each replica keeps
/// whatever they produce. A `split_state` that depended on iteration order of
/// an unordered container, on a clock, or on a random seed would hand
/// different replicas different children and no Raft-level invariant would
/// notice — the logs would still match.
///
/// **2. `absorb` must be the exact inverse of `split_state`.** Splitting at `k`
/// and then absorbing the right part back must reproduce the original
/// `get_state()` byte for byte. This is the sharpest edge in the whole design:
/// a state machine that violates it produces replicas that diverge silently,
/// and the round-trip property test is the only thing standing between that bug
/// and production.
///
/// The leader-only hooks (`suggest_split_keys`, `can_split_at`) are under no
/// such obligation. Their output is frozen into the split log entry before any
/// replica applies anything, so they may consult a cache, sample, or change
/// their mind between calls.
///
/// @tparam SM  The state machine type.
/// @tparam Key Must satisfy `shard_key`.
template<typename SM, typename Key>
concept splittable_state_machine =
    requires(SM& sm, const SM& csm, const Key& k, const std::vector<Key>& keys,
             const std::vector<std::byte>& blob, const shard_range<Key>& r) {
        // Stated as a nested requirement rather than as `shard_key Key` in the
        // template head: a concept may not have associated constraints, so a
        // constrained template parameter is ill-formed here.
        requires shard_key<Key>;

        /// Approximate bytes held. Approximate on purpose: an exact figure
        /// would cost a full scan on every policy tick.
        { csm.approximate_size_bytes() } -> std::same_as<std::size_t>;
        { csm.approximate_key_count() } -> std::same_as<std::size_t>;

        /// Leader-only, advisory: up to `max` good places to cut. May return
        /// fewer, including none.
        { sm.suggest_split_keys(std::size_t{}) } -> std::same_as<std::vector<Key>>;

        /// Leader-only, authoritative: the application's veto. A key this
        /// returns `false` for is never proposed as a split point.
        { csm.can_split_at(k) } -> std::same_as<bool>;

        /// Every replica, at apply. Returns `keys.size() + 1` blobs, in key
        /// order: everything below `keys[0]`, then each `[keys[i], keys[i+1])`,
        /// then everything at or above the last key. MUST be deterministic.
        { sm.split_state(keys) } -> std::same_as<std::vector<std::vector<std::byte>>>;

        /// Every replica, at apply. Takes on another shard's state, which
        /// covered `r`. MUST be deterministic and MUST be the exact inverse of
        /// `split_state`.
        { sm.absorb(blob, r) } -> std::same_as<void>;
    };

}  // namespace kythira
