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

// ─────────────────────────────────────────────────────────────────────────────
// Deployment tier, and the two ways a command can be addressed
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Which of Requirement 3.1's tiers produced a row.
///
/// Carried on the result rather than inferred from the transport's name,
/// because Requirement 3.1 asks for exactly one label per result and
/// Requirement 3.3 forbids publishing a like-for-like comparison from anything
/// below Tier C. A reader who has to derive the tier from a transport name is a
/// reader who can get it wrong.
enum class deployment_tier : std::uint8_t {
    /// In-process hosts over the message fabric, memory persistence. No wire,
    /// no encoding, no disk. **Never comparable to an external number.**
    a_fabric,
    /// In-process hosts, a real transport over loopback, memory persistence.
    b_loopback,
};

[[nodiscard]] inline auto to_string(deployment_tier tier) -> std::string_view {
    switch (tier) {
        case deployment_tier::a_fabric:
            return "Tier A (in-process fabric, memory persistence)";
        case deployment_tier::b_loopback:
            return "Tier B (real transport over loopback, memory persistence)";
    }
    return "unknown";
}

/// @brief Whether a comparison drawn from this tier may be published as
///        like-for-like (Requirement 3.3).
///
/// Constant `false` today because this suite reaches Tier B at most. It is a
/// function rather than a literal so that the rule lives beside the tier it
/// governs, and so Tier C's arrival is one line here instead of a search.
[[nodiscard]] inline auto publishable_as_like_for_like(deployment_tier) -> bool {
    return false;
}

/// @brief How a command reaches its shard — the axis Requirement 8.3 isolates.
///
/// Three values rather than two, because the routing scenario needs a control
/// arm that differs from the treatment arm **only** in which `submit_command`
/// overload it calls.
///
/// The harness's own leader discovery is the reason. `by_key` finds the leader
/// by resolving the key on every host, which costs one shard-map lookup per
/// host — more than the single lookup inside `submit_command` that this
/// scenario is trying to price. Measuring `by_key` against `attributed_group`
/// would therefore attribute four lookups to routing and report a figure
/// several times too large. Both attributed arms find the leader from a group
/// id the client already holds, so that cost is identical in the two and
/// cancels out of the delta.
///
/// The price of that control is that `attributed_key` is **not** the path the
/// rest of the suite measures, which is why the routing scenario prints its own
/// baseline instead of differencing against another row's headline.
enum class routing_mode : std::uint8_t {
    /// The production path (Requirement 1.7), and what every row outside the
    /// routing scenario uses: the leader is found by resolving the key, then
    /// `multi_raft::submit_command(key, command, timeout)` resolves it again.
    by_key,
    /// The routing scenario's **control** arm: the leader is found from a
    /// cached group id, then `submit_command(key, command, timeout)` performs
    /// the shard-map lookup by key.
    attributed_key,
    /// The routing scenario's **treatment** arm: the same cached group id finds
    /// the leader, then `submit_command(group, expected_epoch, command,
    /// timeout)` looks the shard up by group id and validates the caller's
    /// epoch. This is the one place Requirement 1.7's routing rule is
    /// deliberately relaxed, and Requirement 8.3 is why.
    attributed_group,
};

[[nodiscard]] inline auto to_string(routing_mode mode) -> std::string_view {
    switch (mode) {
        case routing_mode::by_key:
            return "submit_command(key, ...), leader resolved by key";
        case routing_mode::attributed_key:
            return "submit_command(key, ...), leader from a cached group id";
        case routing_mode::attributed_group:
            return "submit_command(group, epoch, ...), leader from a cached group id";
    }
    return "unknown";
}

/// @brief How load is offered — Requirement 4.1's two modes, reported with the
///        parameter that controls each.
///
/// The distinction is not a tuning knob, it is which question is being asked.
/// A closed loop measures what the system does when the client waits for it; an
/// open loop measures what it does when the client does not, which is the only
/// mode in which a queue can form and therefore the only one whose tail latency
/// describes a deployment.
enum class load_mode : std::uint8_t {
    /// Each worker holds exactly one operation outstanding, so `_in_flight` is
    /// literally the concurrency and the offered rate is whatever the system
    /// can absorb.
    closed_loop,
    /// Operations are issued on a fixed schedule at `_offered_rate_per_second`,
    /// and latency is measured from each one's **intended** start time.
    open_loop,
};

[[nodiscard]] inline auto to_string(load_mode mode) -> std::string_view {
    switch (mode) {
        case load_mode::closed_loop:
            return "closed loop (fixed in-flight cap)";
        case load_mode::open_loop:
            return "open loop (fixed offered rate, coordinated-omission corrected)";
    }
    return "unknown";
}

/// @brief A descriptor a client already holds: the group, and the epoch the
///        request was computed against.
///
/// The unit `submit_command(group, expected_epoch, ...)` is addressed in, and
/// therefore the unit a client's descriptor cache is made of. Sampled once
/// before a measured window; see `kv_cluster::descriptor_cache`.
struct addressed_shard {
    std::uint64_t _group{};
    kythira::shard_epoch _epoch{};
};

// ─────────────────────────────────────────────────────────────────────────────
// The read taxonomy
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The three reads this system can perform, which cost different things
///        and guarantee different things.
///
/// Requirement 2.1 forbids aggregating them, and the enum is how that is
/// enforced rather than remembered: a row carries one of these, and there is no
/// value meaning "a read".
enum class read_kind : std::uint8_t {
    /// `multi_raft::read_state` — confirms leadership by heartbeat quorum and
    /// returns the **entire serialized store** for the shard. Not a point read.
    read_state,
    /// A `GET` submitted as a proposal. Costs a log entry and a replication
    /// round, returns one value.
    log_get,
    /// `test_key_value_state_machine::get_value` against a replica's state
    /// machine. No consensus, no quorum, and explicitly not linearizable.
    local_stale,
};

[[nodiscard]] inline auto to_string(read_kind kind) -> std::string_view {
    switch (kind) {
        case read_kind::read_state:
            return "read_state (whole-store)";
        case read_kind::log_get:
            return "GET through the log";
        case read_kind::local_stale:
            return "local stale read";
    }
    return "unknown";
}

/// @brief The consistency each read kind actually provides, in the words a
///        comparison table has to match on (Requirement 2.2, 2.3).
///
/// Spelled out rather than abbreviated because the whole point of Requirement
/// 2.3 is that a reader comparing against somebody else's read number can see
/// immediately whether the guarantees line up.
[[nodiscard]] inline auto consistency_of(read_kind kind) -> std::string_view {
    switch (kind) {
        case read_kind::read_state:
            return "linearizable (leadership confirmed by heartbeat quorum)";
        case read_kind::log_get:
            return "linearizable (ordered through the log)";
        case read_kind::local_stale:
            return "NOT LINEARIZABLE (local replica, may be arbitrarily stale)";
    }
    return "unknown";
}

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

/// @brief What a window of replication traffic cost, as plain subtractable
///        numbers.
///
/// Separate from `rpc_counters` — which is atomic and lives as long as the
/// cluster — so that a caller takes two snapshots and subtracts them. That is
/// what confines a figure to the *measured* window: the warm-up, the election
/// and the teardown all move the atomics, and none of them belong in
/// "RPCs per committed entry".
struct rpc_snapshot {
    std::uint64_t _append_entries{0};
    /// AppendEntries carrying no entries: heartbeats, and the probe that walks
    /// a follower back to its match index. Counted apart because they are a
    /// function of the *heartbeat interval*, not of the offered load, and
    /// folding them into a batching figure would make a quiet cluster look
    /// like it batches badly.
    std::uint64_t _append_entries_empty{0};
    std::uint64_t _entries{0};
    std::uint64_t _request_vote{0};
    std::uint64_t _install_snapshot{0};

    [[nodiscard]] auto total_rpcs() const -> std::uint64_t {
        return _append_entries + _request_vote + _install_snapshot;
    }

    /// @brief AppendEntries that actually carried log entries.
    [[nodiscard]] auto carrying() const -> std::uint64_t {
        return _append_entries - std::min(_append_entries, _append_entries_empty);
    }

    /// @brief Entries per entry-bearing AppendEntries — the batching factor
    ///        Hypothesis H1 is about. Empty for a window in which no
    ///        AppendEntries carried anything, because 0/0 is not "no batching".
    [[nodiscard]] auto entries_per_append_entries() const -> std::optional<double> {
        return carrying() == 0
                   ? std::nullopt
                   : std::optional{static_cast<double>(_entries) / static_cast<double>(carrying())};
    }

    friend auto operator-(const rpc_snapshot& a, const rpc_snapshot& b) -> rpc_snapshot {
        return rpc_snapshot{
            ._append_entries = a._append_entries - b._append_entries,
            ._append_entries_empty = a._append_entries_empty - b._append_entries_empty,
            ._entries = a._entries - b._entries,
            ._request_vote = a._request_vote - b._request_vote,
            ._install_snapshot = a._install_snapshot - b._install_snapshot,
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Durability
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Which persistence engine every group's store is built from.
///
/// Requirement 3.4 asks for a tier in which the durability barrier is real,
/// and Requirement 3.5 asks that a file-backed log which does not fsync be
/// labelled **not durable** wherever it appears. Both of those are choices
/// this enum makes explicit rather than leaving to a default.
enum class durability_mode : std::uint8_t {
    /// `memory_persistence_engine`. Nothing reaches a disk; the decomposition
    /// prints the durability barrier as exactly 0.0 us, and it is exactly zero.
    memory = 0,
    /// `file_persistence_engine` with NO batch controller. The log file is
    /// written and the stream flushed; `sync_log_and_directory()` is never
    /// reached, because it is called only from `commit_batch()`. This mode
    /// exists to be measured against `barrier`, and every row that uses it
    /// must be labelled **not durable** (Requirement 3.5).
    file_buffered = 1,
    /// `file_persistence_engine` with a controller supplied by this harness
    /// that opens a batch across every group before the persist phase and
    /// commits it after. One fsync per group per tick, which — as
    /// `tick_batch_controller`'s own documentation says — is N barriers
    /// wearing one name, not one barrier for N groups.
    file_barrier = 2,
};

[[nodiscard]] inline auto to_string(durability_mode m) -> std::string_view {
    switch (m) {
        case durability_mode::memory:
            return "memory";
        case durability_mode::file_buffered:
            return "file/buffered (NOT DURABLE)";
        case durability_mode::file_barrier:
            return "file/barrier";
    }
    return "unknown";
}

struct durability_snapshot {
    std::uint64_t _barriers{0};
    std::uint64_t _empty_batches{0};
    std::uint64_t _entries{0};
    /// Of `_entries`, how many were appended while their store's batch was
    /// open — and therefore how many a barrier ever covered.
    ///
    /// This exists because the obvious reading of the other three is wrong.
    /// `multi_raft`'s tick opens the batch, runs the persist phase and commits
    /// it, all inside one `tick()` call. But a proposal appends to the leader's
    /// log on the *caller's* thread and a follower appends on its RPC handler's
    /// thread, and neither of those is inside the tick's window. An append that
    /// lands between two ticks is written to the page cache and no barrier ever
    /// reaches it, so `_entries / _barriers` would report an entries-per-fsync
    /// figure covering entries that were never fsynced at all.
    std::uint64_t _entries_batched{0};

    /// Entries per durability barrier, counting only entries a barrier
    /// actually covered. Zero when nothing was barriered.
    [[nodiscard]] auto entries_per_barrier() const -> double {
        return _barriers == 0
                   ? 0.0
                   : static_cast<double>(_entries_batched) / static_cast<double>(_barriers);
    }

    /// The fraction of appended entries that any barrier covered. **1.0 is the
    /// only value a durable configuration may report**; anything less means
    /// that proportion of the log reached the page cache and stopped.
    [[nodiscard]] auto barriered_fraction() const -> double {
        return _entries == 0
                   ? 0.0
                   : static_cast<double>(_entries_batched) / static_cast<double>(_entries);
    }
};

[[nodiscard]] inline auto operator-(const durability_snapshot& a, const durability_snapshot& b)
    -> durability_snapshot {
    // Every field, and a static_assert on the size so that adding one to
    // `durability_snapshot` without differencing it here fails to compile.
    // This function silently dropped `_entries_batched` once already: a
    // designated-initialiser aggregate that omits a member value-initialises
    // it, so the field arrived at the report as a perfectly plausible zero and
    // the row said a barrier had covered 0% of entries while also reporting
    // 122 barriers. Two numbers that cannot both be true is the only reason it
    // was caught.
    static_assert(sizeof(durability_snapshot) == 4 * sizeof(std::uint64_t),
                  "durability_snapshot gained a field; difference it below");
    return durability_snapshot{
        ._barriers = a._barriers - b._barriers,
        ._empty_batches = a._empty_batches - b._empty_batches,
        ._entries = a._entries - b._entries,
        ._entries_batched = a._entries_batched - b._entries_batched,
    };
}

/// @brief One row of the comparison table.
struct benchmark_result {
    /// Which of Requirement 3.1's tiers this row was taken at. Not defaulted
    /// to Tier B and left to drift: `fill_result` reads it off the transport
    /// fixture, so a new fixture cannot be added without deciding.
    deployment_tier _tier{deployment_tier::b_loopback};
    /// How the command was addressed. `by_key` everywhere except the
    /// routing-cost scenario Requirement 8.3 defines.
    routing_mode _routing{routing_mode::by_key};
    /// Which of Requirement 4.1's two modes offered the load, and the parameter
    /// that controlled it. Both are on the row because 4.1 asks for the mode to
    /// be *reported with* its parameter — an open-loop row without its rate, or
    /// a closed-loop row without its in-flight cap, is a latency number with no
    /// stated offered load.
    load_mode _load{load_mode::closed_loop};
    /// Operations per second the schedule asked for. Zero on a closed-loop row,
    /// where the offered rate is an outcome rather than an input.
    double _offered_rate_per_second{0.0};
    /// How far behind its intended start the *last* operation of the window
    /// began, summed across workers and divided by the completions. Zero in
    /// closed loop. This is the coordinated-omission correction made visible:
    /// a system keeping up has a lag near zero, and a saturated one does not,
    /// and the difference is exactly what a latency measured from dispatch
    /// would have thrown away.
    std::chrono::nanoseconds _mean_schedule_lag{0};
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
    /// The host driver's tick period. This library has no timer thread, so the
    /// tick is its only clock and anything waiting on a heartbeat waits for the
    /// caller's next `tick()` — Hypothesis H6. Carried on every row, not only
    /// the row that sweeps it, because a latency figure taken at an unstated
    /// cadence cannot be compared with one taken at another.
    std::chrono::milliseconds _tick_interval{0};
    double _ops_per_second{0.0};
    std::chrono::nanoseconds _p50{0};
    std::chrono::nanoseconds _p95{0};
    std::optional<std::chrono::nanoseconds> _p99{};
    operation_tally _tally{};
    std::chrono::nanoseconds _duration{0};
    /// Replication traffic issued *during the measured window only* — the
    /// difference of two `kv_cluster::rpc_counts()` snapshots taken either side
    /// of it. Zero on a row whose cluster was not counting.
    rpc_snapshot _rpc{};
    /// Which persistence engine every group's store was built from, and
    /// whether a barrier was ever issued. Requirement 3.5: a file-backed log
    /// that never fsyncs is **not durable**, and a row that does not carry the
    /// mode cannot say which it was.
    durability_mode _durability{durability_mode::memory};
    /// Durability barriers and log appends *during the measured window only*,
    /// differenced the same way `_rpc` is. Requirement 3.4 asks a durable row
    /// for fsyncs per second per host and entries per fsync; both come from
    /// here and from `_duration`. All zero on a memory row, where there is no
    /// barrier to count.
    durability_snapshot _durability_counts{};

    /// Bytes returned to the caller across the whole measured window. Zero for
    /// a write row; the point of carrying it is Requirement 2.4, which asks for
    /// `read_state` in bytes/sec **as well as** ops/sec, because at a large
    /// shard the second is the number that describes the machine's work.
    std::uint64_t _bytes_returned{0};
    /// Empty on a write row. Present on a read row even when the kind is
    /// obvious, so that no reported read can omit its kind (Requirement 2.1).
    std::optional<read_kind> _read_kind{};

    /// @brief Bytes returned per second over the measured window.
    [[nodiscard]] auto bytes_per_second() const -> double {
        const auto seconds = std::chrono::duration<double>(_duration).count();
        return seconds > 0.0 ? static_cast<double>(_bytes_returned) / seconds : 0.0;
    }

    /// @brief Mean bytes handed back per completed operation (Requirement 2.2).
    [[nodiscard]] auto bytes_per_operation() const -> std::optional<double> {
        return _tally._completed == 0 ? std::nullopt
                                      : std::optional{static_cast<double>(_bytes_returned) /
                                                      static_cast<double>(_tally._completed)};
    }

    /// @brief RPCs per committed entry (Hypothesis H2).
    ///
    /// Empty when nothing committed, because a ratio over zero commits is not
    /// a cost — it is a failed run, which `_tally` already says.
    [[nodiscard]] auto rpcs_per_committed_entry() const -> std::optional<double> {
        return _tally._completed == 0 ? std::nullopt
                                      : std::optional{static_cast<double>(_rpc.total_rpcs()) /
                                                      static_cast<double>(_tally._completed)};
    }
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

    /// @brief Indices into `_runs`, ordered by the quantity this row is judged
    ///        on — throughput in closed loop, **p50 in open loop**.
    ///
    /// The mode matters here for the same reason it matters to
    /// `governing_spread()`. In open loop every repetition achieves the
    /// scheduled rate to four decimal places, so ordering by throughput is
    /// ordering by noise and the "median run" is an arbitrary one of the five.
    /// The latency quoted beside a headline has to come from the median of
    /// something that actually varied.
    [[nodiscard]] auto order() const -> std::vector<std::size_t> {
        std::vector<std::size_t> index(_runs.size());
        for (std::size_t i = 0; i < index.size(); ++i) {
            index[i] = i;
        }
        const bool by_latency = !_runs.empty() && _runs.front()._load == load_mode::open_loop;
        std::sort(index.begin(), index.end(), [this, by_latency](std::size_t a, std::size_t b) {
            return by_latency ? _runs[a]._p50 < _runs[b]._p50
                              : _runs[a]._ops_per_second < _runs[b]._ops_per_second;
        });
        return index;
    }

    /// @brief The median run — by rate in closed loop, by p50 in open loop.
    ///
    /// For an even number of repetitions this is the lower of the two middle
    /// runs, not their mean: the point of naming a run is that its percentiles
    /// and its tally belong to the same window as its rate. See `order()` for
    /// why the ordering key depends on the load mode.
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

    /// @brief The slowest and fastest repetitions by rate.
    ///
    /// Computed directly rather than through `order()`, which sorts by p50 in
    /// open loop: the extremes of ops/sec have to be the extremes of ops/sec in
    /// both modes, or `spread()` stops describing what it says it does.
    [[nodiscard]] auto min_ops_per_second() const -> double {
        if (_runs.empty()) {
            return 0.0;
        }
        return std::min_element(_runs.begin(), _runs.end(),
                                [](const benchmark_result& a, const benchmark_result& b) {
                                    return a._ops_per_second < b._ops_per_second;
                                })
            ->_ops_per_second;
    }

    [[nodiscard]] auto max_ops_per_second() const -> double {
        if (_runs.empty()) {
            return 0.0;
        }
        return std::max_element(_runs.begin(), _runs.end(),
                                [](const benchmark_result& a, const benchmark_result& b) {
                                    return a._ops_per_second < b._ops_per_second;
                                })
            ->_ops_per_second;
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

    /// @brief The spread that decides whether this row may be compared —
    ///        which is not the same quantity in the two load modes.
    ///
    /// In **closed loop** the offered rate is an outcome, so throughput's
    /// run-to-run spread is exactly the question Requirement 6.3 asks.
    ///
    /// In **open loop** the rate is an *input*: the schedule pins it, and a row
    /// that held its schedule reports a throughput spread of essentially zero
    /// no matter what the system did. Judging such a row on throughput would
    /// stamp `stable` on every open-loop measurement ever taken — including one
    /// whose repetitions ranged from a 682 us p50 to an 84,704 us p50, which is
    /// a real pair of numbers from the first open-loop run this suite took. In
    /// that mode the variance moves into latency, so latency is what the
    /// verdict has to be computed from.
    [[nodiscard]] auto governing_spread() const -> double {
        if (_runs.empty()) {
            return 0.0;
        }
        return median_run()._load == load_mode::open_loop ? p50_spread() : spread();
    }

    /// @brief What this row may be used for (6.3, 6.7), judged on
    ///        `governing_spread()`.
    [[nodiscard]] auto verdict() const -> result_verdict {
        if (_runs.size() < k_required_repetitions) {
            return result_verdict::inconclusive;
        }
        return governing_spread() <= k_unstable_spread ? result_verdict::stable
                                                       : result_verdict::unstable;
    }

    /// @brief Whether this row may appear in a comparison table (6.3, 6.7).
    [[nodiscard]] auto comparable() const -> bool { return verdict() == result_verdict::stable; }

    /// @brief The p50 this row is quoted at — the median run's, not the median
    ///        of the runs' p50s.
    ///
    /// Consistent with `headline_ops_per_second`: the quoted latency belongs to
    /// the window that produced the quoted rate, rather than to no window at
    /// all.
    [[nodiscard]] auto median_p50() const -> std::chrono::nanoseconds {
        return _runs.empty() ? std::chrono::nanoseconds{0} : median_run()._p50;
    }

    /// @brief How far the repetitions' p50 ranged around the quoted one, as a
    ///        fraction of it.
    ///
    /// A cost decomposition is built by subtracting one row's p50 from
    /// another's, and a difference is only a measurement if it is larger than
    /// the spread of the things differenced. `spread()` answers that question
    /// for throughput; this answers it for the quantity the decomposition
    /// actually uses. The two are not interchangeable — task 8 measured a row
    /// whose throughput spread 20% while its replication ratio spread 1%.
    [[nodiscard]] auto p50_spread() const -> double {
        if (_runs.size() < 2) {
            return 0.0;
        }
        const auto quoted = median_p50();
        if (quoted <= std::chrono::nanoseconds{0}) {
            return std::numeric_limits<double>::infinity();
        }
        auto low = _runs.front()._p50;
        auto high = _runs.front()._p50;
        for (const auto& run : _runs) {
            low = std::min(low, run._p50);
            high = std::max(high, run._p50);
        }
        const auto half = std::max(high - quoted, quoted - low);
        return std::chrono::duration<double>(half).count() /
               std::chrono::duration<double>(quoted).count();
    }
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
