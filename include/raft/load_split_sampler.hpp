// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file load_split_sampler.hpp
/// @brief Channel (c′): TiKV RFC 0045's load-based split sampler.
///
/// See `.kiro/specs/multi-raft/` design §6.3. The premise transfers exactly
/// from TiKV: **a shard can be small and still be the bottleneck**, and the fix
/// is to split it so its children's leaderships can be scattered onto different
/// machines. Every other split signal in this system measures how much data a
/// shard holds; this one measures how much attention it is getting.
///
/// The sampler is a small state machine over three states:
///
/// ```
///   idle ──(load over threshold)──▶ sampling ──(window elapsed, balanced)──▶ idle
///     ▲                                │                                      │
///     │                                │                                      └─ emits a
///     ├──────(load dropped)────────────┘                                         hot key
///     │                                │
///     └──(back-off expired)── backoff ◀┘ (window elapsed, one hot key)
/// ```
///
/// Two of those transitions are the whole reason the file is more than a
/// counter:
///
/// **Abandoning on a short spike.** A split costs an election per child and a
/// scatter. Paying that for a five-second burst is a net loss, and RFC 0045
/// says so outright: "splitting is meaningless for momentary and short loads
/// (<10s)". The window is a knob because the right value depends on how
/// expensive an election is in a given deployment.
///
/// **The single-hot-key detection.** Without it, a workload hammering one key
/// makes the sampler propose a split; the split does not help, because the key
/// is indivisible; the shard is still hot, so it proposes again. That is a
/// split storm that shrinks shards toward one key each — the failure mode is
/// not "no benefit" but "unbounded harm". The back-off marks the shard and
/// moves on.
///
/// **Off by default, and one predictable branch when off** (Requirement 9.7).
/// `observe()` is on the path of every routed request, so its disabled cost has
/// to be nothing anyone can measure.

#include <raft/shard_types.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <random>
#include <vector>

namespace kythira {

/// @brief Knobs for `load_split_sampler`. Defaults are TiKV RFC 0045's.
struct load_split_sampler_config {
    /// @brief Off by default. Requirement 9.7.
    bool _enabled{false};

    /// Entry thresholds. Either one crossing is enough — a shard can be hot in
    /// requests without being hot in bytes, and the reverse.
    double _qps_threshold{3000.0};
    double _bytes_threshold{30.0 * 1024 * 1024};

    /// How long to count accesses before deciding. RFC 0045's ten seconds.
    std::chrono::milliseconds _duration{std::chrono::seconds{10}};

    /// Candidate keys held at once. Bounded because `observe()` updates every
    /// candidate's counters, so this is the per-request cost while sampling.
    std::size_t _sample_keys{20};

    /// Above this share on one side, the load is one key and a split cannot
    /// help.
    double _one_sided_fraction{0.99};

    /// How long a shard stays ineligible after a single-hot-key verdict.
    std::chrono::milliseconds _backoff{std::chrono::minutes{10}};

    /// @brief Seed for the reservoir sampler. Zero means "ask the system".
    ///
    /// A policy is explicitly permitted to be non-deterministic (its answer is
    /// frozen into the split entry every replica applies), so a random seed is
    /// safe. A fixed one exists for tests, which need the same candidate set
    /// twice.
    std::uint64_t _seed{0};
};

/// @brief What the sampler is doing right now.
enum class load_sampler_state : std::uint8_t {
    idle = 0,      ///< Below threshold, or between windows. Costs one branch.
    sampling = 1,  ///< Inside a window, counting accesses per candidate.
    backoff = 2,   ///< A single hot key was found; not sampling until it expires.
};

inline auto operator<<(std::ostream& os, load_sampler_state s) -> std::ostream& {
    switch (s) {
        case load_sampler_state::idle:
            return os << "idle";
        case load_sampler_state::sampling:
            return os << "sampling";
        case load_sampler_state::backoff:
            return os << "backoff";
        default:
            return os << "unknown";
    }
}

/// @brief Per-shard load sampler. Leader-only, single-threaded per group.
///
/// Not thread-safe, deliberately. Every call site is on the owning group's
/// stripe — `observe()` from the routing layer after the group has been
/// resolved, `evaluate()` from the policy phase — and the striped executor is
/// what guarantees those never overlap for one group. Adding a mutex here would
/// buy nothing and would put a lock on the request path.
///
/// @tparam Key Must satisfy `shard_key`.
template<shard_key Key> class load_split_sampler {
public:
    using clock = std::chrono::steady_clock;

    explicit load_split_sampler(load_split_sampler_config cfg = {})
        : _cfg(cfg), _rng(cfg._seed != 0 ? cfg._seed : std::random_device{}()) {}

    [[nodiscard]] auto enabled() const -> bool { return _cfg._enabled; }
    [[nodiscard]] auto state() const -> load_sampler_state { return _state; }
    [[nodiscard]] auto config() const -> const load_split_sampler_config& { return _cfg; }

    /// @brief Record one routed request.
    ///
    /// The disabled path is the first line, and it is the only cost when
    /// sampling is off: a predictably-taken branch on a member already in cache
    /// beside the counters the caller just bumped.
    auto observe(const Key& key) -> void {
        if (_state != load_sampler_state::sampling) {
            return;
        }
        ++_observations;
        ++_seen;

        if (_candidates.size() < _cfg._sample_keys) {
            _candidates.push_back(candidate{._key = key});
        } else if (_cfg._sample_keys > 0) {
            // Reservoir sampling (Vitter's algorithm R): the k-th observation
            // replaces a uniformly-chosen candidate with probability N/k, which
            // is what makes the candidate set representative of the whole
            // window rather than of its first twenty requests.
            //
            // A replaced candidate loses its counts. That is correct rather
            // than merely convenient: the counts describe accesses relative to
            // a *specific* key, and carrying them over to a different key would
            // attribute one boundary's evidence to another.
            std::uniform_int_distribution<std::uint64_t> pick{1, _seen};
            const auto slot = pick(_rng);
            if (slot <= _cfg._sample_keys) {
                _candidates[static_cast<std::size_t>(slot - 1)] = candidate{._key = key};
            }
        }

        // Every observation counts against every candidate boundary. This is
        // the O(_sample_keys) part, and the reason that knob is bounded.
        for (auto& c : _candidates) {
            if (key < c._key) {
                ++c._left;
            } else {
                ++c._right;
            }
        }
    }

    /// @brief Advance the state machine. Called once per policy tick.
    ///
    /// @param read_qps    Current measured read rate.
    /// @param write_qps   Current measured write rate.
    /// @param read_bps    Current measured read bytes/sec.
    /// @param write_bps   Current measured write bytes/sec.
    /// @param now         Monotonic now.
    /// @return The hot-key sample to publish in `shard_stats`, or empty. Empty
    ///         is by far the common answer, and every empty return has a
    ///         different reason — see the counters below.
    auto evaluate(double read_qps, double write_qps, double read_bps, double write_bps,
                  clock::time_point now) -> std::vector<hot_key_sample<Key>> {
        if (!_cfg._enabled) {
            return {};
        }

        const bool hot = read_qps >= _cfg._qps_threshold || write_qps >= _cfg._qps_threshold ||
                         read_bps >= _cfg._bytes_threshold || write_bps >= _cfg._bytes_threshold;

        switch (_state) {
            case load_sampler_state::backoff:
                if (now >= _backoff_until) {
                    _state = load_sampler_state::idle;
                }
                return {};

            case load_sampler_state::idle:
                if (hot) {
                    _state = load_sampler_state::sampling;
                    _window_started = now;
                    _candidates.clear();
                    _seen = 0;
                    ++_windows_started;
                }
                return {};

            case load_sampler_state::sampling:
                if (!hot) {
                    // A spike shorter than the window. Splitting for it costs an
                    // election per child and a scatter, and buys nothing that
                    // outlives the burst.
                    _state = load_sampler_state::idle;
                    _candidates.clear();
                    ++_abandoned_spikes;
                    return {};
                }
                if (now - _window_started < _cfg._duration) {
                    return {};  // still counting
                }
                return conclude(now);
        }
        return {};
    }

    /// @name Diagnostics
    ///
    /// Every one of these distinguishes a *reason* the sampler produced no
    /// proposal. "No split happened" is the same observable outcome for a cold
    /// shard, a five-second spike, and a shard being hammered on one key, and
    /// only the last of those is something an operator can act on.
    /// @{
    [[nodiscard]] auto observation_count() const -> std::uint64_t { return _observations; }
    [[nodiscard]] auto windows_started() const -> std::uint64_t { return _windows_started; }
    [[nodiscard]] auto abandoned_spike_count() const -> std::uint64_t { return _abandoned_spikes; }
    [[nodiscard]] auto single_hot_key_count() const -> std::uint64_t { return _single_hot_keys; }
    [[nodiscard]] auto proposal_count() const -> std::uint64_t { return _proposals; }
    [[nodiscard]] auto candidate_count() const -> std::size_t { return _candidates.size(); }
    /// @}

private:
    struct candidate {
        Key _key{};
        std::uint64_t _left{0};
        std::uint64_t _right{0};

        [[nodiscard]] auto total() const -> std::uint64_t { return _left + _right; }
        [[nodiscard]] auto one_sided() const -> double {
            const auto n = total();
            if (n == 0) {
                return 1.0;
            }
            const auto heavier = std::max(_left, _right);
            return static_cast<double>(heavier) / static_cast<double>(n);
        }
    };

    auto conclude(clock::time_point now) -> std::vector<hot_key_sample<Key>> {
        _state = load_sampler_state::idle;
        if (_candidates.empty()) {
            // Hot by the rate counters but nothing arrived through this
            // sampler — a read-heavy shard whose reads are not key-addressed,
            // for instance. Nothing to propose and nothing to blame.
            return {};
        }

        const auto* best = &_candidates.front();
        for (const auto& c : _candidates) {
            if (c.one_sided() < best->one_sided()) {
                best = &c;
            }
        }

        if (best->one_sided() > _cfg._one_sided_fraction) {
            // EVERY candidate is one-sided, because the most balanced one is.
            // The load is a single key, a split cannot divide it, and proposing
            // one anyway is how a split storm starts.
            _state = load_sampler_state::backoff;
            _backoff_until = now + _cfg._backoff;
            _candidates.clear();
            ++_single_hot_keys;
            return {};
        }

        hot_key_sample<Key> sample{
            ._key = best->_key, ._left_accesses = best->_left, ._right_accesses = best->_right};
        _candidates.clear();
        ++_proposals;
        return {sample};
    }

    load_split_sampler_config _cfg;
    std::mt19937_64 _rng;

    load_sampler_state _state{load_sampler_state::idle};
    clock::time_point _window_started{};
    clock::time_point _backoff_until{};

    std::vector<candidate> _candidates;
    /// Observations this window, for the reservoir's replacement probability.
    std::uint64_t _seen{0};

    std::uint64_t _observations{0};
    std::uint64_t _windows_started{0};
    std::uint64_t _abandoned_spikes{0};
    std::uint64_t _single_hot_keys{0};
    std::uint64_t _proposals{0};
};

}  // namespace kythira
