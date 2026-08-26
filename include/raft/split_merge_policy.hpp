// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file split_merge_policy.hpp
/// @brief Channel (a) of the signal surface: the declarative policy that says
///        *when* a shard should split or merge — and the default one.
///
/// See `.kiro/specs/multi-raft/` design §6.1. Four independent channels feed
/// one arbiter (declarative policy, admin API, state-machine hints, placement
/// driver); every channel produces a **proposal**, and only the arbiter enacts.
/// No channel can touch Raft state.

#include <raft/shard_types.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace kythira {

/// @brief Decides whether a shard should split or merge.
///
/// Three properties of this contract are design decisions worth defending, and
/// the first is the one most likely to be got wrong by assumption:
///
/// **1. A policy is leader-only and is NOT required to be deterministic.**
/// It runs on one leader, and its answer is frozen into the split log entry
/// that every replica then applies. So a policy may read a wall clock, sample
/// randomly, consult a cache, or change its mind between calls — none of it can
/// diverge replicas. The opposite assumption ("policies must be
/// deterministic") is both a natural guess and a needless constraint, which is
/// why it is contradicted here rather than left unstated.
///
/// **2. No I/O and no mutation.** A policy receives a value and returns a
/// value. A policy wanting external input gets it through the placement-driver
/// channel, which is asynchronous and already has a failure story.
///
/// **3. A vector of split keys, not one.** Batch split, straight from TiKV
/// RFC 0006's motivation: "Current split only splits one Region at a time. It
/// may be very slow when a sequential write is too fast, namely, the split
/// speed cannot keep up with write speed." A one-key-at-a-time API cannot
/// express the fix.
///
/// @tparam P       The policy type.
/// @tparam GroupId Must satisfy `raft_group_id`.
/// @tparam Key     Must satisfy `shard_key`.
template<typename P, typename GroupId, typename Key>
concept split_merge_policy = requires(P& p, const shard_stats<GroupId, Key>& self,
                                      const shard_stats<GroupId, Key>& sibling) {
    requires raft_group_id<GroupId>;
    requires shard_key<Key>;

    { p.evaluate_split(self) } -> std::same_as<split_decision<Key>>;
    { p.evaluate_merge(self, sibling) } -> std::same_as<merge_decision>;

    /// Minimum time between this policy's own decisions for one shard. The
    /// host enforces its own interval on top; see
    /// `threshold_split_merge_policy_config::_split_merge_interval`.
    { p.cooldown() } -> std::same_as<std::chrono::milliseconds>;

    /// Checked once at construction. A policy that cannot be configured
    /// safely says so before it is ever consulted.
    { p.validate() } -> std::same_as<bool>;
    { p.get_validation_errors() } -> std::same_as<std::vector<std::string>>;
};

// ─────────────────────────────────────────────────────────────────────────────
// The default policy
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Knobs for `threshold_split_merge_policy`.
///
/// The defaults are TiKV's, and the units are bytes and keys because those are
/// the two things a range-partitioned shard actually grows in.
struct threshold_split_merge_policy_config {
    std::size_t _shard_max_size_bytes{144ULL * 1024 * 1024};
    std::size_t _shard_split_size_bytes{96ULL * 1024 * 1024};
    std::size_t _shard_max_keys{1'440'000};
    std::size_t _shard_split_keys{960'000};

    std::size_t _shard_merge_max_size_bytes{20ULL * 1024 * 1024};
    std::size_t _shard_merge_max_keys{200'000};

    /// @brief Minimum time between a split and a merge on one shard.
    ///
    /// **Enforced by the host, not by this policy** (Requirement 7.6). A custom
    /// policy that forgets the cooldown still cannot oscillate, because the
    /// arbiter refuses a merge on a shard whose `_time_since_last_split` is
    /// under this. Defence in depth on the one knob whose misconfiguration is
    /// unbounded.
    std::chrono::milliseconds _split_merge_interval{std::chrono::hours{1}};

    std::size_t _batch_split_limit{10};

    // ── load split (design §6.3) ─────────────────────────────────────────────
    bool _load_split_enabled{false};
    double _load_split_qps_threshold{3000.0};
    double _load_split_bytes_threshold{30ULL * 1024 * 1024};
    std::chrono::milliseconds _load_split_duration{std::chrono::seconds{10}};
    std::size_t _load_split_sample_keys{20};
    double _load_split_one_sided_fraction{0.99};
    std::chrono::milliseconds _load_split_backoff{std::chrono::minutes{10}};
};

/// @brief The default policy: size and key-count thresholds, plus the
///        load-split signal when the sampler is enabled.
///
/// ### The oscillation guard is the interesting part
///
/// `validate()` **rejects** a configuration where
/// `2 * merge_max >= split_size`, and the error message spells out the failure
/// it prevents: two adjacent shards each just under the merge threshold merge
/// into one shard just over the split threshold, which splits, producing two
/// shards just under the merge threshold, forever. TiKV RFC 0045 states the
/// same rule of thumb from the other side — "to avoid back-and-forth splitting
/// and merging, the merging load threshold should be slightly lower than
/// splitting load threshold (e.g., 20% lower)". The shipped defaults sit at
/// 20 MiB against 96 MiB, comfortably inside the bound.
///
/// @tparam GroupId Must satisfy `raft_group_id`.
/// @tparam Key     Must satisfy `shard_key`.
template<raft_group_id GroupId, shard_key Key> class threshold_split_merge_policy {
public:
    using config_type = threshold_split_merge_policy_config;
    using stats_type = shard_stats<GroupId, Key>;

    threshold_split_merge_policy() = default;
    explicit threshold_split_merge_policy(config_type cfg) : _cfg(std::move(cfg)) {}

    [[nodiscard]] auto config() const -> const config_type& { return _cfg; }

    // ── the policy contract ──────────────────────────────────────────────────

    [[nodiscard]] auto evaluate_split(const stats_type& self) -> split_decision<Key> {
        // Load first: a shard can be small and still be the bottleneck, and the
        // sampler has already done the work of finding a balanced cut.
        if (_cfg._load_split_enabled && !self._hot_key_samples.empty()) {
            const auto& sample = self._hot_key_samples.front();
            if (sample.one_sided_fraction() < _cfg._load_split_one_sided_fraction) {
                return split_decision<Key>{._split = true,
                                           ._at_keys = {sample.key()},
                                           ._reason = self._write_qps >= self._read_qps
                                                          ? split_reason::write_load
                                                          : split_reason::read_load};
            }
        }

        // Size and key count need the sizing hooks. Without them this policy
        // simply never proposes a size-based split — which the host reports
        // once at construction rather than leaving an operator to wonder about.
        if (!self._size_available) {
            return {};
        }

        if (self._approximate_size_bytes >= _cfg._shard_split_size_bytes) {
            return split_decision<Key>{._split = true,
                                       ._at_keys = {},  // "you choose" — the state machine picks
                                       ._reason = split_reason::size};
        }
        if (self._approximate_key_count >= _cfg._shard_split_keys) {
            return split_decision<Key>{
                ._split = true, ._at_keys = {}, ._reason = split_reason::key_count};
        }
        return {};
    }

    [[nodiscard]] auto evaluate_merge(const stats_type& self, const stats_type& sibling)
        -> merge_decision {
        if (!self._size_available || !sibling._size_available) {
            return {};
        }
        // BOTH sides must be under BOTH thresholds. Two rules in one:
        //
        // - Both *sides*, because merging a small shard into a large one
        //   produces a shard over the split threshold, which is the
        //   oscillation the `validate()` guard exists to prevent — arriving
        //   through the one door that guard does not watch.
        // - Both *measures*, because they are independent and either one
        //   saying "big" is decisive. An OR would let a 90 MiB shard qualify
        //   on a key count of zero, which is exactly the shape of a shard
        //   holding a few enormous values.
        const bool small_by_size =
            self._approximate_size_bytes <= _cfg._shard_merge_max_size_bytes &&
            sibling._approximate_size_bytes <= _cfg._shard_merge_max_size_bytes;
        const bool small_by_keys = self._approximate_key_count <= _cfg._shard_merge_max_keys &&
                                   sibling._approximate_key_count <= _cfg._shard_merge_max_keys;
        if (!small_by_size || !small_by_keys) {
            return {};
        }

        // A shard that split recently must not merge back. The host enforces
        // this too; a policy that agreed with the host here saves the arbiter a
        // rejection to log.
        if (self._time_since_last_split < _cfg._split_merge_interval ||
            sibling._time_since_last_split < _cfg._split_merge_interval) {
            return {};
        }

        return merge_decision{
            ._merge = true,
            ._direction = merge_direction::into_left_sibling,
            ._reason = small_by_size ? merge_reason::size : merge_reason::key_count};
    }

    [[nodiscard]] auto cooldown() const -> std::chrono::milliseconds {
        return _cfg._split_merge_interval;
    }

    [[nodiscard]] auto validate() const -> bool { return get_validation_errors().empty(); }

    [[nodiscard]] auto get_validation_errors() const -> std::vector<std::string> {
        std::vector<std::string> errors;

        if (2 * _cfg._shard_merge_max_size_bytes >= _cfg._shard_split_size_bytes) {
            errors.push_back(
                "2 * _shard_merge_max_size_bytes (" +
                std::to_string(2 * _cfg._shard_merge_max_size_bytes) +
                ") must be strictly below _shard_split_size_bytes (" +
                std::to_string(_cfg._shard_split_size_bytes) +
                "): otherwise two adjacent shards just under the merge threshold merge into "
                "one just over the split threshold, which splits back into two just under the "
                "merge threshold — oscillating forever");
        }
        if (2 * _cfg._shard_merge_max_keys >= _cfg._shard_split_keys) {
            errors.push_back("2 * _shard_merge_max_keys (" +
                             std::to_string(2 * _cfg._shard_merge_max_keys) +
                             ") must be strictly below _shard_split_keys (" +
                             std::to_string(_cfg._shard_split_keys) +
                             "): the same oscillation, counted in keys instead of bytes");
        }
        if (_cfg._shard_split_size_bytes > _cfg._shard_max_size_bytes) {
            errors.push_back(
                "_shard_split_size_bytes exceeds _shard_max_size_bytes: a shard "
                "would be over its maximum before it was eligible to split");
        }
        if (_cfg._shard_split_keys > _cfg._shard_max_keys) {
            errors.push_back("_shard_split_keys exceeds _shard_max_keys, for the same reason");
        }
        if (_cfg._batch_split_limit == 0) {
            errors.push_back("_batch_split_limit must be at least 1");
        }
        if (_cfg._load_split_one_sided_fraction <= 0.5 ||
            _cfg._load_split_one_sided_fraction > 1.0) {
            errors.push_back(
                "_load_split_one_sided_fraction must be in (0.5, 1.0]: below 0.5 it "
                "is not one-sided at all, and above 1.0 nothing can reach it");
        }
        return errors;
    }

    // ── split-key generation (TiKV RFC 0006's SizeChecker) ───────────────────

    /// @brief How many keys to ask a state machine for, given a shard's size.
    ///
    /// Follows RFC 0006: record a split key every `_shard_split_size_bytes`,
    /// stop at `split_size * (batch_limit - 1) + max_size`, and discard the
    /// trailing key when the remainder is not larger than
    /// `max_size - split_size`. With `_batch_split_limit == 1` this degenerates
    /// exactly to single-key split, which is what makes the batch path testable
    /// against the simple one.
    ///
    /// Returns 0 when the shard is not big enough to split at all.
    [[nodiscard]] auto split_key_count_for_size(std::size_t size_bytes) const -> std::size_t {
        if (size_bytes < _cfg._shard_split_size_bytes || _cfg._shard_split_size_bytes == 0) {
            return 0;
        }
        const auto scan_limit = _cfg._shard_split_size_bytes * (_cfg._batch_split_limit - 1) +
                                _cfg._shard_max_size_bytes;
        const auto scanned = std::min(size_bytes, scan_limit);

        auto keys = scanned / _cfg._shard_split_size_bytes;
        const auto remainder = scanned - keys * _cfg._shard_split_size_bytes;

        // Discard the trailing key when what is left behind it is not larger
        // than `max - split`: cutting there would leave a child too small to be
        // worth the election it costs.
        //
        // Only when the walk reached the shard's actual END. If it stopped at
        // `scan_limit` there is more data behind the last key by definition, so
        // the remainder of the *walk* says nothing about the remainder of the
        // *shard* — and discarding on it would leave a shard well over its
        // maximum with no split keys at all.
        const auto tail_floor = _cfg._shard_max_size_bytes > _cfg._shard_split_size_bytes
                                    ? _cfg._shard_max_size_bytes - _cfg._shard_split_size_bytes
                                    : 0;
        if (keys > 0 && size_bytes <= scan_limit && remainder <= tail_floor) {
            --keys;
        }
        return std::min(keys, _cfg._batch_split_limit);
    }

private:
    config_type _cfg{};
};

}  // namespace kythira
