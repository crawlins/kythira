// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_kv_workload.hpp
/// @brief The key/value payload the multi-Raft performance work is stated
///        against, plus the sampling and statistics the numbers are reported in.
///
/// Split out from the transport harness because it has nothing to do with
/// transports: the same workload runs over the in-process fabric, over each
/// HTTP transport, and — when CoAP grows a multi-Raft binding — over that,
/// unchanged. Keeping it separate is what makes "the same work, different
/// wire" a checkable claim rather than an assertion.
///
/// The commands are built to `kythira::test_key_value_state_machine`'s own
/// encoding (`include/raft/test_state_machine.hpp`), which is the state machine
/// every multi-Raft suite already uses. Deliberately *not* a benchmark-only
/// fixture: a payload that only the benchmark exercises measures a code path
/// nothing else covers.
///
/// ### On the statistics
///
/// `latency_sample_set` reports p50/p95/p99 only when it has enough samples to
/// mean anything, and says so otherwise. A "p99" computed from eight samples is
/// the slowest of eight samples; this project has been bitten by exactly that
/// label before, and the guard here is what keeps it from recurring.

#include <raft/shard_types.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::testing {

// ─────────────────────────────────────────────────────────────────────────────
// The command encoding
// ─────────────────────────────────────────────────────────────────────────────

/// @brief `test_key_value_state_machine`'s wire form: type byte, u32 key
/// length, key bytes, and for PUT a u32 value length and value bytes.
inline auto encode_kv_command(std::uint8_t type, const std::string& key, const std::string& value)
    -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(1 + 2 * sizeof(std::uint32_t) + key.size() + value.size());
    out.push_back(static_cast<std::byte>(type));
    const auto append_u32 = [&out](std::uint32_t n) {
        std::byte buf[sizeof(std::uint32_t)];
        std::memcpy(buf, &n, sizeof(n));
        out.insert(out.end(), std::begin(buf), std::end(buf));
    };
    const auto append_str = [&out](const std::string& s) {
        for (char c : s) {
            out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
    };
    append_u32(static_cast<std::uint32_t>(key.size()));
    append_str(key);
    if (type == 1) {
        append_u32(static_cast<std::uint32_t>(value.size()));
        append_str(value);
    }
    return out;
}

inline auto kv_put(const std::string& key, const std::string& value) -> std::vector<std::byte> {
    return encode_kv_command(1, key, value);
}

/// @brief A GET *through the log* — a linearizable point read that costs a log
/// entry, as distinct from `multi_raft::read_state`'s quorum-confirmed
/// whole-store transfer. The two are different operations and the benchmark
/// reports them separately.
inline auto kv_get(const std::string& key) -> std::vector<std::byte> {
    return encode_kv_command(0, key, {});
}

inline auto kv_del(const std::string& key) -> std::vector<std::byte> {
    return encode_kv_command(2, key, {});
}

/// @brief Recovers the key from a command, for `multi_raft_config::partitioner`.
///
/// The host uses this to refuse a command whose key the addressed shard does not
/// own — the cross-shard admission check. Without it a routing bug applies a
/// command to the wrong shard and no invariant catches it.
struct kv_partitioner {
    [[nodiscard]] auto key_of(const std::vector<std::byte>& command) const -> std::string {
        if (command.size() < 1 + sizeof(std::uint32_t)) {
            return {};
        }
        std::uint32_t key_length{};
        std::memcpy(&key_length, command.data() + 1, sizeof(key_length));
        if (1 + sizeof(std::uint32_t) + key_length > command.size()) {
            return {};
        }
        std::string key;
        key.reserve(key_length);
        for (std::uint32_t i = 0; i < key_length; ++i) {
            key.push_back(static_cast<char>(command[1 + sizeof(std::uint32_t) + i]));
        }
        return key;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Keys and the shard map they tile
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The number of digits every generated key carries. Ten digits keeps
/// `kv_key` total-ordering-correct up to ten billion keys.
inline constexpr std::size_t k_key_digits = 10;

/// @brief A fixed-width decimal key, so lexicographic order is numeric order.
///
/// Every shard range is then a genuine contiguous slice of the key space rather
/// than a label, which is what makes the routing lookup the real one.
inline auto kv_key(std::uint64_t n) -> std::string {
    auto s = std::to_string(n);
    return std::string(k_key_digits > s.size() ? k_key_digits - s.size() : 0, '0') + s;
}

/// @brief A deterministic value of exactly `bytes` bytes.
///
/// Deterministic rather than random so a run is reproducible, and non-constant
/// so a serializer cannot compress the payload away into nothing — which would
/// make a value-size sweep measure the compressor rather than the payload.
inline auto kv_value(std::uint64_t seed, std::size_t bytes) -> std::string {
    static constexpr std::string_view alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string out;
    out.reserve(bytes);
    std::uint64_t x = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    for (std::size_t i = 0; i < bytes; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        out.push_back(alphabet[(x >> 33) % alphabet.size()]);
    }
    return out;
}

/// @brief `count` shard ranges tiling the whole key space over `key_count` keys.
///
/// The first range's start and the last range's end are open, so the map tiles
/// `(-inf, +inf)` and `check_tiling()` has something true to say.
inline auto kv_shard_ranges(std::size_t count, std::uint64_t key_count)
    -> std::vector<kythira::shard_range<std::string>> {
    std::vector<kythira::shard_range<std::string>> ranges;
    ranges.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        kythira::shard_range<std::string> r;
        r._start = i == 0 ? std::optional<std::string>{}
                          : std::optional<std::string>{kv_key(key_count * i / count)};
        r._end = i + 1 == count ? std::optional<std::string>{}
                                : std::optional<std::string>{kv_key(key_count * (i + 1) / count)};
        ranges.push_back(std::move(r));
    }
    return ranges;
}

// ─────────────────────────────────────────────────────────────────────────────
// Key selection
// ─────────────────────────────────────────────────────────────────────────────

enum class key_distribution : std::uint8_t {
    /// Every key equally likely — spreads load evenly across shards.
    uniform,
    /// Skewed, so a small set of keys (and therefore one shard) takes most of
    /// the traffic. This is the distribution that exposes a per-group lock.
    zipfian,
};

/// @brief Draws keys from `[0, key_count)` under the chosen distribution.
///
/// The Zipfian arm uses the standard inverse-CDF approximation rather than a
/// precomputed table: at the key counts this benchmark uses, building the table
/// would cost more than the draws.
class key_sampler {
public:
    key_sampler(key_distribution dist, std::uint64_t key_count, double theta, std::uint64_t seed)
        : _dist(dist), _key_count(key_count), _theta(theta), _rng(seed) {
        if (_dist == key_distribution::zipfian) {
            _zeta = zeta(_key_count, _theta);
            _zeta_two = zeta(2, _theta);
            _alpha = 1.0 / (1.0 - _theta);
            _eta = (1.0 - std::pow(2.0 / static_cast<double>(_key_count), 1.0 - _theta)) /
                   (1.0 - _zeta_two / _zeta);
        }
    }

    [[nodiscard]] auto next() -> std::uint64_t {
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        if (_dist == key_distribution::uniform) {
            return static_cast<std::uint64_t>(unit(_rng) * static_cast<double>(_key_count)) %
                   _key_count;
        }
        const double u = unit(_rng);
        const double uz = u * _zeta;
        if (uz < 1.0) {
            return 0;
        }
        if (uz < 1.0 + std::pow(0.5, _theta)) {
            return 1;
        }
        return static_cast<std::uint64_t>(static_cast<double>(_key_count) *
                                          std::pow(_eta * u - _eta + 1.0, _alpha)) %
               _key_count;
    }

private:
    static auto zeta(std::uint64_t n, double theta) -> double {
        double sum = 0.0;
        for (std::uint64_t i = 1; i <= n; ++i) {
            sum += 1.0 / std::pow(static_cast<double>(i), theta);
        }
        return sum;
    }

    key_distribution _dist;
    std::uint64_t _key_count;
    double _theta;
    std::mt19937_64 _rng;
    double _zeta{0.0};
    double _zeta_two{0.0};
    double _alpha{0.0};
    double _eta{0.0};
};

// ─────────────────────────────────────────────────────────────────────────────
// Statistics
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Latency samples, and percentiles that refuse to lie about themselves.
///
/// `p99()` returns `std::nullopt` below 1,000 samples and `p999()` below 10,000
/// — the thresholds `.kiro/specs/multi-raft-performance/` Requirement 5.2 sets.
/// A caller that wants a number regardless can have `quantile()`, which is
/// honestly named.
class latency_sample_set {
public:
    auto record(std::chrono::nanoseconds d) -> void { _samples.push_back(d); }

    auto reserve(std::size_t n) -> void { _samples.reserve(n); }

    [[nodiscard]] auto count() const -> std::size_t { return _samples.size(); }
    [[nodiscard]] auto empty() const -> bool { return _samples.empty(); }

    auto sort() -> void { std::sort(_samples.begin(), _samples.end()); }

    /// @brief The `q` quantile. Call `sort()` first.
    [[nodiscard]] auto quantile(double q) const -> std::chrono::nanoseconds {
        if (_samples.empty()) {
            return std::chrono::nanoseconds{0};
        }
        const auto index = static_cast<std::size_t>(q * static_cast<double>(_samples.size() - 1));
        return _samples[std::min(index, _samples.size() - 1)];
    }

    [[nodiscard]] auto p50() const -> std::chrono::nanoseconds { return quantile(0.50); }
    [[nodiscard]] auto p95() const -> std::chrono::nanoseconds { return quantile(0.95); }

    [[nodiscard]] auto p99() const -> std::optional<std::chrono::nanoseconds> {
        return _samples.size() >= 1000 ? std::optional{quantile(0.99)} : std::nullopt;
    }

    [[nodiscard]] auto p999() const -> std::optional<std::chrono::nanoseconds> {
        return _samples.size() >= 10000 ? std::optional{quantile(0.999)} : std::nullopt;
    }

    [[nodiscard]] auto mean() const -> std::chrono::nanoseconds {
        if (_samples.empty()) {
            return std::chrono::nanoseconds{0};
        }
        std::chrono::nanoseconds total{0};
        for (auto s : _samples) {
            total += s;
        }
        return total / static_cast<std::int64_t>(_samples.size());
    }

    [[nodiscard]] auto max() const -> std::chrono::nanoseconds {
        return _samples.empty() ? std::chrono::nanoseconds{0}
                                : *std::max_element(_samples.begin(), _samples.end());
    }

    auto merge(const latency_sample_set& other) -> void {
        _samples.insert(_samples.end(), other._samples.begin(), other._samples.end());
    }

private:
    std::vector<std::chrono::nanoseconds> _samples;
};

/// @brief Failures counted by cause, so a throughput number over a run with a
/// rejection rate is visibly that rather than silently that.
struct operation_tally {
    std::uint64_t _offered{0};
    std::uint64_t _completed{0};
    std::uint64_t _not_leader{0};
    std::uint64_t _epoch_mismatch{0};
    std::uint64_t _merging{0};
    std::uint64_t _timeout{0};
    std::uint64_t _transport{0};
    std::uint64_t _other{0};

    [[nodiscard]] auto failed() const -> std::uint64_t {
        return _not_leader + _epoch_mismatch + _merging + _timeout + _transport + _other;
    }

    [[nodiscard]] auto success_rate() const -> double {
        return _offered == 0 ? 0.0
                             : static_cast<double>(_completed) / static_cast<double>(_offered);
    }

    auto merge(const operation_tally& o) -> void {
        _offered += o._offered;
        _completed += o._completed;
        _not_leader += o._not_leader;
        _epoch_mismatch += o._epoch_mismatch;
        _merging += o._merging;
        _timeout += o._timeout;
        _transport += o._transport;
        _other += o._other;
    }
};

/// @brief One row of the comparison table.
struct benchmark_result {
    std::string _transport;
    std::string _serializer;
    std::string _scenario;
    std::size_t _nodes{0};
    std::size_t _groups{0};
    std::size_t _value_bytes{0};
    std::size_t _in_flight{0};
    double _ops_per_second{0.0};
    std::chrono::nanoseconds _p50{0};
    std::chrono::nanoseconds _p95{0};
    std::optional<std::chrono::nanoseconds> _p99{};
    operation_tally _tally{};
    std::chrono::nanoseconds _duration{0};
};

}  // namespace kythira::testing
