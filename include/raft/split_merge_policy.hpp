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
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
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
        // No measurement is an ABSTENTION, not an objection. This policy has
        // nothing to say about shards it cannot size, and under the composite's
        // unanimity rule (design §6.1.3) saying anything stronger would let a
        // state machine without sizing hooks block every merge in the cluster.
        if (!self._size_available || !sibling._size_available) {
            return merge_decision::abstain();
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
        // A VETO, not an abstention. "These shards are too big to merge" is a
        // genuine objection, and it is the objection this policy exists to
        // raise: under unanimity, abstaining here would let a load-driven
        // member merge two 90 MiB shards into one 180 MiB shard that splits
        // straight back — the oscillation `validate()` guards against,
        // arriving through the one door `validate()` cannot watch.
        if (!small_by_size || !small_by_keys) {
            return merge_decision::veto(small_by_size ? merge_reason::key_count
                                                      : merge_reason::size);
        }

        // A shard that split recently must not merge back. Also a veto, for the
        // same reason: it is an anti-oscillation objection, not indifference.
        // The host enforces this too; a policy that agrees with the host here
        // saves the arbiter a rejection to log.
        if (self._time_since_last_split < _cfg._split_merge_interval ||
            sibling._time_since_last_split < _cfg._split_merge_interval) {
            return merge_decision::veto(merge_reason::size);
        }

        return merge_decision::propose(
            merge_direction::into_left_sibling,
            small_by_size ? merge_reason::size : merge_reason::key_count);
    }

    /// @brief The smallest approximate size at which this policy proposes a
    ///        split, for the composite's cross-member oscillation check.
    ///
    /// Optional by design (§6.1.3): the composite detects it with `if
    /// constexpr`, and a member exposing neither this nor `merge_ceiling()` is
    /// reported as *uncheckable* rather than silently assumed safe.
    [[nodiscard]] auto split_floor() const -> std::size_t { return _cfg._shard_split_size_bytes; }

    /// @brief The largest approximate size at which this policy proposes a
    ///        merge. See `split_floor()`.
    [[nodiscard]] auto merge_ceiling() const -> std::size_t {
        return _cfg._shard_merge_max_size_bytes;
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

    /// @brief A stable name for the `policy` metric dimension.
    ///
    /// Static storage, per `split_decision::_policy`'s lifetime rule.
    [[nodiscard]] static auto name() -> const char* { return "threshold"; }

private:
    config_type _cfg{};
};

// ─────────────────────────────────────────────────────────────────────────────
// Composition (design §6.1.3, Requirement 8)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Detects the optional cross-member oscillation accessors.
///
/// Structural detection with `if constexpr`, exactly as
/// `splittable_state_machine` is detected: a policy opts in by having the
/// members, not by inheriting anything.
template<typename P>
concept has_oscillation_bounds = requires(const P& p) {
    { p.split_floor() } -> std::convertible_to<std::size_t>;
    { p.merge_ceiling() } -> std::convertible_to<std::size_t>;
};

/// @brief Detects a policy that names itself for the `policy` dimension.
template<typename P>
concept has_policy_name = requires(const P& p) {
    { p.name() } -> std::convertible_to<const char*>;
};

/// @brief Runs several policies as one.
///
/// A host runs exactly **one** policy. Combining several is a property of that
/// policy, not of `multi_raft_config` — which is why this is a policy that
/// happens to contain policies rather than a list in the configuration.
///
/// ### Why not a list in the config
///
/// A homogeneous `std::vector<P>` composes nothing worth composing. A
/// heterogeneous list costs either a variadic `multi_raft<Types, Key, GroupId,
/// Ps...>`, which leaks into every alias, deduction guide and test fixture, or
/// a type-erased virtual policy interface, which would be the first such
/// interface in a codebase whose extension mechanism is uniformly concepts and
/// `if constexpr`. The composite gives up only a policy set assembled at run
/// time from a config file — which nothing in this specification asks for —
/// and gains the one thing a list cannot offer: cross-member validation, which
/// needs the concrete types.
///
/// ### The combination rules, and why they are asymmetric
///
/// | | Rule | Why |
/// |---|---|---|
/// | split | any-wins; `at_keys` is the sorted, de-duplicated union | more shards is the
/// recoverable direction, and a member proposing a split has seen something the others did not | |
/// merge | unanimous — at least one `propose`, no `veto` | a merge destroys a group and moves data
/// through `absorb`; any-wins would let a size policy merge away a shard a load policy was
/// deliberately keeping |
///
/// Three smaller rules fall out of the same place:
///
/// - **Deferral does not union.** A member returning `_split = true` with empty
///   `_at_keys` means "split, you choose" and cannot be unioned with a concrete
///   key vector. Concrete keys win; the composite falls through to the state
///   machine's suggestions only when *no* member named a key.
/// - **Opposite merge directions are a mutual veto**, logged. Silently picking
///   one would make the answer depend on member order.
/// - **Order is not semantically significant.** Every rule above is commutative
///   and associative, including the attribution tie-break. A first-wins rule
///   would let reordering a composition silently change how the cluster shards
///   — the kind of coupling discovered during an incident, not during review.
///
/// @tparam Ps The member policies, held by value at their concrete types.
template<typename... Ps> class composite_split_merge_policy {
public:
    using members_type = std::tuple<Ps...>;

    /// @brief Where construction-time and decision-time notices go.
    ///
    /// A sink rather than a logger type, because this header knows nothing
    /// about `Types` and a policy has no business owning a logger.
    using notice_sink = std::function<void(const std::string&)>;

    /// Default-constructs every member. For a zero-member composition this is
    /// the only constructor, which is why the variadic one below is guarded:
    /// with `sizeof...(Ps) == 0` the two would have identical signatures.
    composite_split_merge_policy() { announce_if_empty(); }

    explicit composite_split_merge_policy(Ps... members)
    requires(sizeof...(Ps) > 0)
        : _members(std::move(members)...) {
        announce_if_empty();
    }

    composite_split_merge_policy(notice_sink sink, Ps... members)
        : _members(std::move(members)...), _sink(std::move(sink)) {
        announce_if_empty();
    }

    /// @brief Install the sink after construction.
    ///
    /// Re-announces the empty-composition notice if one was produced before a
    /// sink existed, so wiring the logger later does not lose it.
    auto set_notice_sink(notice_sink sink) -> void {
        _sink = std::move(sink);
        if (_sink) {
            for (const auto& n : _notices) {
                _sink(n);
            }
        }
    }

    /// @brief Every notice this composite has produced, oldest first.
    ///
    /// Exposed so a test can assert "logged exactly once" without standing up a
    /// logger, and so an operator can ask a running host what its policy said
    /// about itself at construction.
    [[nodiscard]] auto notices() const -> const std::vector<std::string>& { return _notices; }

    [[nodiscard]] static constexpr auto size() -> std::size_t { return sizeof...(Ps); }

    [[nodiscard]] static auto name() -> const char* { return "composite"; }

    // ── split: any-wins, union of concrete keys ──────────────────────────────

    template<typename Stats>
    [[nodiscard]] auto evaluate_split(const Stats& self)
        -> split_decision<typename Stats::key_type> {
        using key_type = typename Stats::key_type;
        using decision_type = split_decision<key_type>;

        decision_type out;
        bool any_deferred = false;
        const char* winner_name = nullptr;
        split_reason winner_reason = split_reason::size;
        bool have_winner = false;

        std::apply(
            [&](auto&... members) {
                (
                    [&] {
                        auto d = members.evaluate_split(self);
                        if (!d.should_split()) {
                            return;
                        }
                        const char* who = member_name(members, d._policy);
                        // Attribution tie-break: the lexicographically smallest
                        // member name. Any rule would do EXCEPT "the first one",
                        // which is the one rule that makes member order
                        // observable.
                        if (!have_winner || less_name(who, winner_name)) {
                            have_winner = true;
                            winner_name = who;
                            winner_reason = d.reason();
                        }
                        if (d.at_keys().empty()) {
                            any_deferred = true;
                            return;
                        }
                        out._at_keys.insert(out._at_keys.end(), d.at_keys().begin(),
                                            d.at_keys().end());
                    }(),
                    ...);
            },
            _members);

        if (!have_winner) {
            return out;  // nobody proposed
        }

        out._split = true;
        out._reason = winner_reason;
        out._policy = winner_name;

        // Concrete keys win over deferral. A member saying "split, you choose"
        // cannot be unioned with a member that named where; and if the only
        // proposals were deferrals, the empty vector IS the deferral, which the
        // host turns into `suggest_split_keys`.
        if (!out._at_keys.empty()) {
            std::sort(out._at_keys.begin(), out._at_keys.end());
            out._at_keys.erase(std::unique(out._at_keys.begin(), out._at_keys.end()),
                               out._at_keys.end());
        } else if (!any_deferred) {
            out._split = false;
        }
        return out;
    }

    // ── merge: unanimous ─────────────────────────────────────────────────────

    template<typename Stats>
    [[nodiscard]] auto evaluate_merge(const Stats& self, const Stats& sibling) -> merge_decision {
        bool any_propose = false;
        bool vetoed = false;
        const char* veto_name = nullptr;
        merge_reason veto_reason = merge_reason::size;

        const char* propose_name = nullptr;
        merge_reason propose_reason = merge_reason::size;
        std::optional<merge_direction> direction;
        bool direction_conflict = false;

        std::apply(
            [&](auto&... members) {
                (
                    [&] {
                        auto d = members.evaluate_merge(self, sibling);
                        const char* who = member_name(members, d._policy);
                        if (d.vetoed()) {
                            if (!vetoed || less_name(who, veto_name)) {
                                veto_name = who;
                                veto_reason = d.reason();
                            }
                            vetoed = true;
                            return;
                        }
                        if (!d.should_merge()) {
                            // An abstention. It neither supports nor blocks —
                            // that is the entire reason the verdict is
                            // tri-state.
                            return;
                        }
                        if (direction.has_value() && *direction != d.direction()) {
                            direction_conflict = true;
                        }
                        direction = d.direction();
                        if (!any_propose || less_name(who, propose_name)) {
                            propose_name = who;
                            propose_reason = d.reason();
                        }
                        any_propose = true;
                    }(),
                    ...);
            },
            _members);

        if (vetoed) {
            return merge_decision::veto(veto_reason, veto_name);
        }
        if (direction_conflict) {
            // A mutual veto, not a coin toss. Picking one would make the
            // composite's answer depend on the order its members were written
            // down, which is the one thing member order must never do.
            note(std::string{"composite policy: members proposed opposite merge directions; "} +
                 "treating as a mutual veto");
            return merge_decision::veto(propose_reason, name());
        }
        if (!any_propose) {
            return merge_decision::abstain();
        }
        return merge_decision::propose(*direction, propose_reason, propose_name);
    }

    // ── the rest of the concept ──────────────────────────────────────────────

    /// @brief The maximum over the members'.
    ///
    /// The binding anti-oscillation guard is the host-level
    /// `split_merge_interval` gate, not this; taking the maximum only ensures
    /// the composite is never *more* eager than its most conservative member.
    [[nodiscard]] auto cooldown() const -> std::chrono::milliseconds {
        std::chrono::milliseconds out{0};
        std::apply([&](const auto&... m) { ((out = std::max(out, m.cooldown())), ...); }, _members);
        return out;
    }

    [[nodiscard]] auto validate() const -> bool { return get_validation_errors().empty(); }

    /// @brief Every member's own errors, plus the cross-member oscillation
    ///        check the members cannot perform for themselves.
    ///
    /// The cross-member check is the reason this class holds concrete types.
    /// `2 * merge_ceiling < split_floor` is *intra*-policy in
    /// `threshold_split_merge_policy`; member A's merge ceiling against member
    /// B's split floor is the identical failure mode, both members validate
    /// clean alone, and the pair oscillates forever — moving real data through
    /// `split_state` and `absorb` on every cycle.
    [[nodiscard]] auto get_validation_errors() const -> std::vector<std::string> {
        std::vector<std::string> errors;

        // 1. Each member's own opinion of itself, attributed.
        for_each_member([&](std::size_t index, const auto& m) {
            for (auto& e : m.get_validation_errors()) {
                errors.push_back(label(index, m) + ": " + e);
            }
        });

        // 2. Members that cannot participate in the cross-member check. Named
        //    rather than assumed safe: "we did not check" and "we checked and
        //    it is fine" are different facts, and only one of them justifies
        //    running the composition.
        for_each_member([&](std::size_t index, const auto& m) {
            if constexpr (!has_oscillation_bounds<std::remove_cvref_t<decltype(m)>>) {
                errors.push_back(
                    label(index, m) +
                    ": uncheckable — exposes neither split_floor() nor merge_ceiling(), so the "
                    "cross-member oscillation bound (2 * merge_ceiling < split_floor) cannot be "
                    "verified against the other members of this composition");
            }
        });

        // 3. The cross-member bound itself, over ordered pairs: A's merge
        //    ceiling against B's split floor is a different question from B's
        //    against A's, so both orders are checked.
        for_each_member([&](std::size_t i, const auto& a) {
            if constexpr (has_oscillation_bounds<std::remove_cvref_t<decltype(a)>>) {
                for_each_member([&](std::size_t j, const auto& b) {
                    if constexpr (has_oscillation_bounds<std::remove_cvref_t<decltype(b)>>) {
                        if (i == j) {
                            return;
                        }
                        if (2 * a.merge_ceiling() >= b.split_floor()) {
                            errors.push_back(
                                label(i, a) + " and " + label(j, b) + ": 2 * " +
                                std::to_string(a.merge_ceiling()) +
                                " (merge_ceiling) must be strictly below " +
                                std::to_string(b.split_floor()) +
                                " (split_floor): otherwise two shards the first policy merges "
                                "produce one shard the second policy splits straight back, and "
                                "the pair oscillates forever — each member validating cleanly on "
                                "its own");
                        }
                    }
                });
            }
        });

        return errors;
    }

private:
    template<typename F> auto for_each_member(F&& f) const -> void {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (f(I, std::get<I>(_members)), ...);
        }(std::index_sequence_for<Ps...>{});
    }

    template<typename P>
    [[nodiscard]] static auto member_name(const P& p, const char* from_decision) -> const char* {
        if (from_decision != nullptr) {
            return from_decision;
        }
        if constexpr (has_policy_name<P>) {
            return p.name();
        } else {
            return "unnamed";
        }
    }

    template<typename P>
    [[nodiscard]] static auto label(std::size_t index, const P& p) -> std::string {
        return "member " + std::to_string(index) + " (" + member_name(p, nullptr) + ")";
    }

    /// Lexicographic, with `nullptr` sorting last so an unattributed decision
    /// never wins the tie-break over a named one.
    [[nodiscard]] static auto less_name(const char* lhs, const char* rhs) -> bool {
        if (lhs == nullptr) {
            return false;
        }
        if (rhs == nullptr) {
            return true;
        }
        return std::strcmp(lhs, rhs) < 0;
    }

    auto note(std::string message) -> void {
        if (_sink) {
            _sink(message);
        }
        _notices.push_back(std::move(message));
    }

    auto announce_if_empty() -> void {
        if constexpr (sizeof...(Ps) == 0) {
            // `{}` is far too easy to arrive at by accident for the difference
            // between "no policies configured" and "policies configured, never
            // firing" to be invisible — the same doctrine as `_size_available`.
            note(
                "composite policy: zero members; channel (a) is silent, exactly as configuring "
                "no policy at all");
        }
    }

    members_type _members{};
    notice_sink _sink{};
    std::vector<std::string> _notices;
};

/// @brief Deduction guide, so `composite_split_merge_policy{a, b}` works.
template<typename... Ps> composite_split_merge_policy(Ps...) -> composite_split_merge_policy<Ps...>;

}  // namespace kythira
