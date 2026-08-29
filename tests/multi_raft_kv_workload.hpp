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
/// Two layers, and both of them refuse to report a number they do not have the
/// evidence for.
///
/// *Within* a run, `latency_sample_set` reports p99/p999 only when it has
/// enough samples to mean anything, and says so otherwise. A "p99" computed
/// from eight samples is the slowest of eight samples; this project has been
/// bitten by exactly that label before, and the guard here is what keeps it
/// from recurring.
///
/// *Across* runs, `repeated_result` is the only thing that yields a headline,
/// and it yields `std::nullopt` below `k_required_repetitions` — Requirement
/// 6.2's "never report a single run as a result", enforced by the type rather
/// than by discipline. The first numbers this benchmark produced made the case
/// for it: the same Beast/JSON/128 B cell measured 863.8, 981.2 and 625.4
/// ops/sec in one session, a ±21% spread around a headline that any one of the
/// three would have been quoted as.
///
/// `machine_description` records what the numbers were taken on (Requirement
/// 6.4) and whether the machine was quiet at the time (6.5). It is captured
/// once, before the first case runs, because by the second case the load
/// average is measuring this suite.

#include <raft/shard_types.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

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
/// — the thresholds `.kiro/specs/multi-raft-performance/` Requirement 5.3 sets.
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
    /// The *node-internal* serializer's own media type — log entries and
    /// snapshots, not the wire. The axis this suite sweeps is `_serializer`;
    /// this one is held fixed so the rows stay comparable, and it is carried
    /// on the row rather than assumed so that "held fixed" is something a
    /// caller can check instead of something the type aliases merely imply.
    std::string _node_serializer;
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

// ─────────────────────────────────────────────────────────────────────────────
// Run-level statistics: what makes a row quotable
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The minimum number of whole-measurement repetitions behind a headline
/// number (Requirement 6.2). Below this a `repeated_result` has no headline at
/// all rather than a weakly-supported one.
inline constexpr std::size_t k_required_repetitions = 5;

/// @brief The run-to-run spread above which a row is unstable and must not
/// enter a comparison table (Requirement 6.3).
inline constexpr double k_unstable_spread = 0.10;

/// @brief What a repeated measurement is allowed to be used for.
///
/// `inconclusive` is deliberately *not* "try again and it becomes a result".
/// Requirement 6.7 makes an inconclusive measurement permanently inconclusive:
/// what a re-run produces is a new measurement with its own verdict, not a
/// promotion of this one.
enum class result_verdict : std::uint8_t {
    /// At least `k_required_repetitions` runs, spread within ±`k_unstable_spread`.
    stable,
    /// Enough runs, but the machine (or the system) moved more than the axis
    /// under test would. Reportable as an observation; not comparable.
    unstable,
    /// Too few runs to say anything.
    inconclusive,
};

[[nodiscard]] inline auto to_string(result_verdict v) -> std::string_view {
    switch (v) {
        case result_verdict::stable:
            return "stable";
        case result_verdict::unstable:
            return "UNSTABLE";
        case result_verdict::inconclusive:
            return "INCONCLUSIVE";
    }
    return "unknown";
}

/// @brief One cell of the matrix, measured `k_required_repetitions` times.
///
/// The headline is the **median** of the repetitions' ops/sec, and it is the
/// rate of an actual run rather than an average of runs — `median_run()` is that
/// same run, so the latency distribution quoted beside the headline came from
/// the window that produced it. An averaged p95 belongs to no window that ever
/// happened.
///
/// Repetitions are whole measurements, cluster construction and election
/// included, because that is where the variance this guard exists for lives:
/// the three Beast/JSON/128 B numbers that motivated Requirement 6.2 differed
/// by ±21% and each came from its own freshly-elected cluster. Re-running the
/// workload against one long-lived cluster would have hidden exactly the effect
/// being measured.
struct repeated_result {
    /// Every repetition, in the order it ran.
    std::vector<benchmark_result> _runs;
    /// Operations issued and discarded before each measured window (6.1).
    std::size_t _warmup_operations{0};
    /// Operations offered in each measured window (6.1).
    std::size_t _measured_operations{0};

    auto record(benchmark_result run) -> void { _runs.push_back(std::move(run)); }

    [[nodiscard]] auto runs() const -> std::size_t { return _runs.size(); }

    /// @brief Indices into `_runs`, ordered by ops/sec.
    [[nodiscard]] auto order() const -> std::vector<std::size_t> {
        std::vector<std::size_t> index(_runs.size());
        for (std::size_t i = 0; i < index.size(); ++i) {
            index[i] = i;
        }
        std::sort(index.begin(), index.end(), [this](std::size_t a, std::size_t b) {
            return _runs[a]._ops_per_second < _runs[b]._ops_per_second;
        });
        return index;
    }

    /// @brief The run whose rate is the median.
    ///
    /// For an even number of repetitions this is the lower of the two middle
    /// runs, not their mean: the point of naming a run is that its percentiles
    /// and its tally belong to the same window as its rate.
    [[nodiscard]] auto median_run() const -> const benchmark_result& {
        const auto index = order();
        return _runs[index[(index.size() - 1) / 2]];
    }

    /// @brief The headline rate, or `std::nullopt` when too few repetitions
    /// stand behind it. Requirement 6.2, enforced by the return type.
    [[nodiscard]] auto headline_ops_per_second() const -> std::optional<double> {
        if (_runs.size() < k_required_repetitions) {
            return std::nullopt;
        }
        return median_run()._ops_per_second;
    }

    [[nodiscard]] auto min_ops_per_second() const -> double {
        return _runs.empty() ? 0.0 : _runs[order().front()]._ops_per_second;
    }

    [[nodiscard]] auto max_ops_per_second() const -> double {
        return _runs.empty() ? 0.0 : _runs[order().back()]._ops_per_second;
    }

    /// @brief The half-width of the spread as a fraction of the median — the
    /// "±" Requirement 6.3 is written in.
    ///
    /// `max(max - median, median - min) / median` rather than
    /// `(max - min) / median`, because a ±10% band around the median is what the
    /// requirement says and a full-width ratio would flag a run pair that sits
    /// inside it.
    [[nodiscard]] auto spread() const -> double {
        if (_runs.size() < 2) {
            return 0.0;
        }
        const auto median = median_run()._ops_per_second;
        if (median <= 0.0) {
            return std::numeric_limits<double>::infinity();
        }
        return std::max(max_ops_per_second() - median, median - min_ops_per_second()) / median;
    }

    [[nodiscard]] auto verdict() const -> result_verdict {
        if (_runs.size() < k_required_repetitions) {
            return result_verdict::inconclusive;
        }
        return spread() <= k_unstable_spread ? result_verdict::stable : result_verdict::unstable;
    }

    /// @brief Whether this row may appear in a comparison table (6.3, 6.7).
    [[nodiscard]] auto comparable() const -> bool { return verdict() == result_verdict::stable; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Machine provenance
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Where a number was taken, in the fields Requirement 6.4 lists.
///
/// Every field defaults to `"not stated"` and stays that way when it cannot be
/// read, following the comparison register's own rule (Requirement 9): a field
/// the source does not state is recorded as not stated, never inferred.
struct machine_description {
    std::string _cpu_model{"not stated"};
    std::size_t _logical_cpus{0};
    std::uint64_t _memory_bytes{0};
    std::string _kernel{"not stated"};

    /// The directory the filesystem fields describe — where a durable tier would
    /// put its log. Tier B is memory-backed, so this is provenance for the
    /// configuration rather than a cost in this row; it is what makes a later
    /// Tier D row on the same machine comparable to this one.
    std::string _described_path{"."};
    std::string _filesystem{"not stated"};
    std::string _device{"not stated"};
    std::string _device_kind{"not stated"};

    std::string _compiler{"not stated"};
    std::string _build_type{"not stated"};
    std::string _cxx_flags{"not stated"};
    std::string _sanitizer{"none"};
    std::string _future_backend{"not stated"};

    /// One-minute load average sampled *before* the first measured window.
    double _load_average_1m{-1.0};
    /// Whether the machine looked otherwise-idle at that moment (6.5).
    bool _quiet_at_start{false};
};

/// @brief The absolute load average below which the machine is called quiet.
///
/// Absolute rather than a fraction of the core count on purpose: Requirement 6.5
/// asks for an *otherwise-idle* machine, not an unsaturated one. Half a runnable
/// process is already enough to move a latency tail.
inline constexpr double k_quiet_load_average = 0.5;

namespace detail {

/// @brief The first value in `/proc/<file>` whose line starts with `prefix`.
[[nodiscard]] inline auto proc_field(const char* path, std::string_view prefix) -> std::string {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) != 0) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(" \t");
        const auto last = value.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
    }
    return {};
}

#if defined(__linux__)
/// @brief `statfs`'s `f_type` as a name. Only the filesystems this project has
/// actually been measured on are named; anything else is reported as its raw
/// magic rather than guessed at.
[[nodiscard]] inline auto filesystem_name(std::uint64_t magic) -> std::string {
    switch (magic) {
        case 0xEF53:
            return "ext2/3/4";
        case 0x9123683EU:
            return "btrfs";
        case 0x58465342U:
            return "xfs";
        case 0x01021994U:
            return "tmpfs";
        case 0x794C7630U:
            return "overlayfs";
        case 0x2FC12FC1U:
            return "zfs";
        case 0x6969U:
            return "nfs";
        case 0x65735546U:
            return "fuse";
        default: {
            std::ostringstream out;
            out << "magic 0x" << std::hex << magic;
            return out.str();
        }
    }
}

/// @brief The mount source and rotational flag backing `path`.
///
/// Read from `/proc/self/mountinfo` by longest matching mount point, which is
/// the only way to get the *device* rather than the filesystem type — and the
/// device is what decides whether a later durable tier's fsync numbers mean
/// anything.
inline auto describe_device(const std::string& path, machine_description& out) -> void {
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    std::size_t best = 0;
    while (std::getline(in, line)) {
        // 36 35 98:0 /mnt1 /mnt2 rw,noatime - ext4 /dev/sda1 rw,errors=continue
        std::istringstream fields(line);
        std::string id;
        std::string parent;
        std::string dev_no;
        std::string root;
        std::string mount_point;
        if (!(fields >> id >> parent >> dev_no >> root >> mount_point)) {
            continue;
        }
        // A prefix match alone would let `/mnt` claim a path under `/mnt2`;
        // the boundary check is what makes "longest mount point wins" correct.
        const bool covers = path.rfind(mount_point, 0) == 0 &&
                            (mount_point == "/" || path.size() == mount_point.size() ||
                             path[mount_point.size()] == '/');
        if (!covers || mount_point.size() < best) {
            continue;
        }
        std::string token;
        std::string source;
        bool after_separator = false;
        std::string fs_type;
        while (fields >> token) {
            if (token == "-") {
                after_separator = true;
                continue;
            }
            if (after_separator) {
                if (fs_type.empty()) {
                    fs_type = token;
                } else {
                    source = token;
                    break;
                }
            }
        }
        if (source.empty()) {
            continue;
        }
        best = mount_point.size();
        out._device = source;
        // A partition has no `queue/` of its own -- the request queue belongs to
        // the whole disk -- so the parent is tried second. `..` through the
        // sysfs symlink lands on the disk directory. A device-mapper or virtual
        // device may have neither, and then the kind stays not stated rather
        // than being guessed at.
        out._device_kind = "not stated";
        for (const auto* suffix : {"/queue/rotational", "/../queue/rotational"}) {
            std::ifstream rotational("/sys/dev/block/" + dev_no + suffix);
            std::string flag;
            if (rotational && std::getline(rotational, flag)) {
                out._device_kind = flag == "0" ? "non-rotational" : "rotational";
                break;
            }
        }
    }
}
#endif

}  // namespace detail

/// @brief Describe the machine this process is running on.
///
/// `log_path` is the directory whose filesystem and device are reported — the
/// place a durable tier would write its log.
[[nodiscard]] inline auto describe_machine(const std::string& log_path = ".")
    -> machine_description {
    machine_description m;
    m._described_path = log_path;
    m._logical_cpus = std::thread::hardware_concurrency();

#if defined(__linux__)
    // Resolved before anything reads it: `/proc/self/mountinfo` states absolute
    // mount points, and a relative path matches none of them.
    if (char* resolved = ::realpath(log_path.c_str(), nullptr); resolved != nullptr) {
        m._described_path = resolved;
        std::free(resolved);
    }
    if (auto model = detail::proc_field("/proc/cpuinfo", "model name"); !model.empty()) {
        m._cpu_model = model;
    }
    if (auto total = detail::proc_field("/proc/meminfo", "MemTotal"); !total.empty()) {
        // MemTotal is stated in kB.
        m._memory_bytes = static_cast<std::uint64_t>(std::strtoull(total.c_str(), nullptr, 10)) *
                          std::uint64_t{1024};
    }
    utsname u{};
    if (::uname(&u) == 0) {
        m._kernel = std::string{u.sysname} + " " + u.release + " " + u.machine;
    }
    struct statfs fs{};
    if (::statfs(m._described_path.c_str(), &fs) == 0) {
        m._filesystem = detail::filesystem_name(static_cast<std::uint64_t>(fs.f_type));
    }
    detail::describe_device(m._described_path, m);
    double load[3] = {0.0, 0.0, 0.0};
    if (::getloadavg(load, 3) == 3) {
        m._load_average_1m = load[0];
        m._quiet_at_start = load[0] <= k_quiet_load_average;
    }
#endif

#if defined(__clang__)
    m._compiler = std::string{"clang "} + __clang_version__;
#elif defined(__GNUC__)
    m._compiler = std::string{"g++ "} + __VERSION__;
#endif

    // Supplied by the build so the row states the configuration it was actually
    // compiled with rather than what a macro can be talked into implying. A tree
    // that does not set them says so.
#if defined(KYTHIRA_BENCH_BUILD_TYPE)
    m._build_type = KYTHIRA_BENCH_BUILD_TYPE;
#endif
#if defined(KYTHIRA_BENCH_CXX_FLAGS)
    m._cxx_flags = KYTHIRA_BENCH_CXX_FLAGS;
#endif

    // A sanitized build's throughput is not this system's throughput. Reported
    // rather than refused: the suite is still worth running under TSan for what
    // it finds, and the label is what stops the number being quoted.
#if defined(__SANITIZE_THREAD__)
    m._sanitizer = "thread";
#elif defined(__SANITIZE_ADDRESS__)
    m._sanitizer = "address";
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
    m._sanitizer = "thread";
#elif __has_feature(address_sanitizer)
    m._sanitizer = "address";
#elif __has_feature(memory_sanitizer)
    m._sanitizer = "memory";
#endif
#endif

#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
    m._future_backend = "stdexec";
#elif defined(KYTHIRA_FUTURE_BACKEND_BOOST)
    m._future_backend = "boost";
#else
    m._future_backend = "folly";
#endif

    return m;
}

}  // namespace kythira::testing
