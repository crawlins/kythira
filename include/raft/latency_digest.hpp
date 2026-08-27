// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file latency_digest.hpp
/// @brief A windowed, fixed-bucket latency histogram — the estimator behind
///        `shard_stats::_p99_read_latency` and `_p99_apply_latency`.
///
/// See `.kiro/specs/multi-raft/` design §6.1.4. Three decisions shape this
/// file, and none of them is about histograms.
///
/// **1. The percentile has to be computed in-process.** Kythira's `metrics`
/// concept is `add_duration` → `emit`: samples leave for Prometheus or OTLP,
/// which computes percentiles *out of process*, where a policy running on a
/// group's stripe cannot read them back. A policy that wants to act on latency
/// therefore needs the host to keep its own estimator. This is that estimator,
/// and it exists because the metrics pipeline is write-only, not because the
/// metrics pipeline is inadequate.
///
/// **2. The window matters more than the estimator.** A lifetime percentile
/// never decays, so a shard that was hot an hour ago still reads as hot and any
/// latency-derived policy becomes permanently sticky once a shard has had one
/// bad minute. The digest is therefore a ring of sub-windows, rotated on the
/// policy tick, and reads merge across the ring. Both latency fields use the
/// same digest type and the same window: two percentiles that could come to
/// mean different things are worse than one.
///
/// **3. Fixed buckets, not a reservoir and not a t-digest.** Bounded memory, no
/// allocation on the record path, O(1) record, mergeable, and a bucket walk to
/// read. A reservoir needs a sort at read time; a t-digest buys accuracy that a
/// threshold comparison does not spend. The cost is a bounded relative error —
/// `latency_histogram::k_relative_error` — which is stated rather than hidden,
/// and which reads are biased to *over*-report: a percentile that
/// under-reports latency is the dangerous direction.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace kythira {

/// @brief One sub-window: a logarithmic histogram over nanosecond durations.
///
/// The layout is HdrHistogram's, reduced to what a threshold comparison needs.
/// Small values (below `k_linear`) get one bucket each and are exact. Above
/// that, each octave is divided into `k_sub_buckets` linear steps, so the
/// relative error of any reported value is bounded by `1 / k_sub_buckets`
/// regardless of magnitude.
///
/// Counts are relaxed atomics. `record()` is called from whichever thread
/// served the request and `quantile()` from the policy tick, so the two race by
/// construction — and a percentile computed from a torn-but-plausible set of
/// counters is exactly as useful as one computed a microsecond earlier. Paying
/// for a lock on the record path to avoid that would be paying for nothing.
class latency_histogram {
public:
    /// Bits of sub-bucket precision within each octave.
    static constexpr int k_sub_bits = 3;
    static constexpr std::uint64_t k_sub_buckets = 1ULL << k_sub_bits;
    /// Below this, every distinct nanosecond value has its own bucket.
    static constexpr std::uint64_t k_linear = 2 * k_sub_buckets;
    /// Highest octave tracked: 2^36 ns is a little over a minute. Anything
    /// slower is clamped into the top bucket, which is the right answer — the
    /// difference between "68 seconds" and "600 seconds" changes no decision
    /// anyone would make from a shard latency percentile.
    static constexpr int k_max_exponent = 36;
    static constexpr std::size_t k_bucket_count =
        static_cast<std::size_t>(k_linear) + static_cast<std::size_t>(k_max_exponent - k_sub_bits) *
                                                 static_cast<std::size_t>(k_sub_buckets);

    /// @brief Worst-case relative error of a reported value, above `k_linear`.
    static constexpr double k_relative_error = 1.0 / static_cast<double>(k_sub_buckets);

    latency_histogram() { clear(); }

    latency_histogram(const latency_histogram&) = delete;
    auto operator=(const latency_histogram&) -> latency_histogram& = delete;

    /// @brief Record one sample. O(1), allocation-free, wait-free.
    auto record(std::chrono::nanoseconds sample) -> void {
        const auto ns =
            sample.count() < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(sample.count());
        _counts[bucket_index(ns)].fetch_add(1, std::memory_order_relaxed);
        _total.fetch_add(1, std::memory_order_relaxed);
    }

    /// @brief Fold `other` into this one. Used to merge the ring for a read.
    auto merge(const latency_histogram& other) -> void {
        for (std::size_t i = 0; i < k_bucket_count; ++i) {
            const auto n = other._counts[i].load(std::memory_order_relaxed);
            if (n != 0) {
                _counts[i].fetch_add(n, std::memory_order_relaxed);
            }
        }
        _total.fetch_add(other._total.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    auto clear() -> void {
        for (auto& c : _counts) {
            c.store(0, std::memory_order_relaxed);
        }
        _total.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] auto count() const -> std::uint64_t {
        return _total.load(std::memory_order_relaxed);
    }

    /// @brief The value at quantile `q` (0.0–1.0), rounded UP to its bucket's
    ///        upper bound.
    ///
    /// Upward, deliberately: a latency percentile that under-reports is the
    /// direction that hides a problem, and the bucket's upper bound is the
    /// largest value that could actually have produced this count.
    ///
    /// Returns zero for an empty histogram — "nothing was measured" and "every
    /// measurement was instant" are both reported as zero, which is why callers
    /// should consult `count()` when the difference matters.
    [[nodiscard]] auto quantile(double q) const -> std::chrono::nanoseconds {
        const auto total = count();
        if (total == 0) {
            return std::chrono::nanoseconds{0};
        }
        q = std::clamp(q, 0.0, 1.0);
        // The rank of the sample we want, 1-based. `ceil` rather than `round`
        // so that p99 of a hundred samples names the 99th, not the 98th.
        auto target =
            static_cast<std::uint64_t>(std::max(1.0, std::ceil(q * static_cast<double>(total))));

        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < k_bucket_count; ++i) {
            seen += _counts[i].load(std::memory_order_relaxed);
            if (seen >= target) {
                return std::chrono::nanoseconds{static_cast<std::int64_t>(bucket_upper_bound(i))};
            }
        }
        return std::chrono::nanoseconds{
            static_cast<std::int64_t>(bucket_upper_bound(k_bucket_count - 1))};
    }

    /// @brief Which bucket a nanosecond value falls in.
    [[nodiscard]] static auto bucket_index(std::uint64_t ns) -> std::size_t {
        if (ns < k_linear) {
            return static_cast<std::size_t>(ns);
        }
        const int exponent = std::bit_width(ns) - 1;
        if (exponent >= k_max_exponent) {
            return k_bucket_count - 1;
        }
        const int shift = exponent - k_sub_bits;
        const auto sub = (ns >> shift) & (k_sub_buckets - 1);
        return static_cast<std::size_t>(k_linear) +
               static_cast<std::size_t>(shift - 1) * static_cast<std::size_t>(k_sub_buckets) +
               static_cast<std::size_t>(sub);
    }

    /// @brief The largest nanosecond value that lands in bucket `index`.
    [[nodiscard]] static auto bucket_upper_bound(std::size_t index) -> std::uint64_t {
        if (index < k_linear) {
            return index;
        }
        const auto offset = index - static_cast<std::size_t>(k_linear);
        const auto shift = static_cast<int>(offset / k_sub_buckets) + 1;
        const auto sub = static_cast<std::uint64_t>(offset % k_sub_buckets);
        const auto lower = (k_sub_buckets + sub) << shift;
        return lower + (1ULL << shift) - 1;
    }

private:
    std::array<std::atomic<std::uint32_t>, k_bucket_count> _counts;
    std::atomic<std::uint64_t> _total{0};
};

/// @brief A ring of `latency_histogram` sub-windows, rotated on the policy tick.
///
/// `record()` always lands in the current sub-window; `quantile()` merges the
/// whole ring. `rotate()` advances and clears the incoming one, so the digest
/// covers the last `window_count` rotations and nothing older — which is the
/// point. Design §6.1.4: a lifetime percentile makes any latency-derived policy
/// permanently sticky.
///
/// Not copyable and not movable: the histograms hold atomics, and a digest's
/// identity is the accumulator a group's request path writes into.
class latency_digest {
public:
    static constexpr std::size_t k_default_windows = 6;

    /// @param window_count Sub-windows in the ring. Clamped to at least one; a
    ///        single window degenerates to "everything since the last rotation",
    ///        which is a legitimate, if coarse, configuration.
    explicit latency_digest(std::size_t window_count = k_default_windows)
        : _count(std::max<std::size_t>(1, window_count)),
          _windows(std::make_unique<latency_histogram[]>(_count)) {}

    latency_digest(const latency_digest&) = delete;
    auto operator=(const latency_digest&) -> latency_digest& = delete;

    auto record(std::chrono::nanoseconds sample) -> void {
        _windows[_current.load(std::memory_order_relaxed) % _count].record(sample);
    }

    /// @brief Advance the ring. Called once per policy tick.
    auto rotate() -> void {
        const auto next = (_current.load(std::memory_order_relaxed) + 1) % _count;
        // Cleared BEFORE it becomes current, so no sample can land in a window
        // that is about to be wiped.
        _windows[next].clear();
        _current.store(next, std::memory_order_relaxed);
    }

    [[nodiscard]] auto quantile(double q) const -> std::chrono::nanoseconds {
        latency_histogram merged;
        for (std::size_t i = 0; i < _count; ++i) {
            merged.merge(_windows[i]);
        }
        return merged.quantile(q);
    }

    [[nodiscard]] auto p99() const -> std::chrono::nanoseconds { return quantile(0.99); }

    [[nodiscard]] auto count() const -> std::uint64_t {
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < _count; ++i) {
            total += _windows[i].count();
        }
        return total;
    }

    [[nodiscard]] auto window_count() const -> std::size_t { return _count; }

    auto clear() -> void {
        for (std::size_t i = 0; i < _count; ++i) {
            _windows[i].clear();
        }
    }

private:
    std::size_t _count;
    std::unique_ptr<latency_histogram[]> _windows;
    std::atomic<std::size_t> _current{0};
};

}  // namespace kythira
