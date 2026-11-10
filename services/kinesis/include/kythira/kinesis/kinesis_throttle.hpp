// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_throttle.hpp
/// @brief Per-shard throughput limits (`.kiro/specs/kinesis-compatible-streams/`
///        task 8, Requirements 3.2, 3.5 and 12.1).
///
/// AWS's per-shard limits: writes 1 MiB/s and 1000 records/s; reads
/// 2 MiB/s and 5 `GetRecords` calls/s; `GetShardIterator` 5 calls/s. They
/// are token buckets that refill continuously and hold one second's worth,
/// so a shard idle for a while takes a one-second burst.
///
/// Writes are admitted where they are proposed: on the leader of the
/// shard's group (design, "token buckets live on the leader in memory and
/// reset on leader change, which can briefly admit up to one extra bucket's
/// worth of traffic"). A forwarded write is admitted by the leader that
/// proposes it, never by the node the client called, so every write to a
/// shard meets one set of buckets. `admit_appends` gives each shard append
/// the longest prefix of its records the buckets allow (the rest fail
/// alone, as AWS fails single records of a PutRecords) and takes the tokens;
/// a proposal that turns out not to be this node's to make refunds them.
///
/// Reads are served by any replica, so their buckets live on the node that
/// serves the read. A consumer that spreads one shard's reads over N nodes
/// can read up to N times AWS's rate; a consumer that sticks to one
/// endpoint (every SDK and KCL does) meets AWS's limit exactly. A
/// `GetRecords` is admitted while the shard's byte bucket is positive and
/// then charged for what it returned, which can drive the bucket negative:
/// AWS's documented behaviour that a 10 MiB answer blocks reads of that
/// shard for the next five seconds.
///
/// Buckets are created on first use and dropped after a minute idle, so a
/// node's memory follows its active shards.

#include <kythira/kinesis/kinesis_stream_state_machine.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kythira::kinesis {

/// Whether a proposal meets the leader's write buckets: a stream with the
/// `throttling` switch off proposes with `none`.
enum class admission : std::uint8_t {
    none = 0,
    shard_limits = 1,
};

struct throughput_limits {
    double _write_bytes_per_s = 1024.0 * 1024.0;
    double _write_records_per_s = 1000.0;
    double _read_bytes_per_s = 2.0 * 1024.0 * 1024.0;
    double _read_calls_per_s = 5.0;
    double _iterator_calls_per_s = 5.0;
};

/// A bucket holding up to one second of `rate`, refilled continuously.
class token_bucket {
public:
    token_bucket() = default;
    token_bucket(double rate, std::int64_t now_ms) : _rate(rate), _tokens(rate), _at_ms(now_ms) {}

    void refill(std::int64_t now_ms) {
        if (now_ms > _at_ms) {
            _tokens =
                std::min(_rate, _tokens + _rate * static_cast<double>(now_ms - _at_ms) / 1000.0);
            _at_ms = now_ms;
        }
    }
    [[nodiscard]] auto tokens() const noexcept -> double { return _tokens; }
    void take(double n) noexcept { _tokens -= n; }
    void give(double n) noexcept { _tokens = std::min(_rate, _tokens + n); }
    [[nodiscard]] auto idle_since() const noexcept -> std::int64_t { return _at_ms; }

private:
    double _rate = 0;
    double _tokens = 0;
    std::int64_t _at_ms = 0;
};

using monotonic_ms = std::function<std::int64_t()>;

[[nodiscard]] inline auto steady_now_ms() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// What `admit_appends` took, so a proposal this node may not make can
/// give it back.
struct admission_ticket {
    struct taken {
        std::string _key;
        double _bytes = 0;
        double _records = 0;
    };
    std::vector<taken> _taken;
};

/// The leader's write buckets, keyed by shard routing key. Thread-safe.
class write_throttle {
public:
    explicit write_throttle(throughput_limits limits = {}, monotonic_ms clock = steady_now_ms)
        : _limits(limits), _clock(std::move(clock)) {}

    /// How many leading records of each append the buckets admit, taking
    /// their tokens. A record's cost is its data plus partition key bytes,
    /// as AWS counts it.
    auto admit(const std::vector<const append_records_command*>& appends, admission_ticket& ticket)
        -> std::vector<std::size_t> {
        std::vector<std::size_t> out;
        out.reserve(appends.size());
        const std::scoped_lock lock(_mutex);
        const auto now = _clock();
        sweep(now);
        for (const auto* a : appends) {
            auto& b = buckets(a->_key, now);
            b._bytes.refill(now);
            b._records.refill(now);
            std::size_t n = 0;
            double bytes = 0;
            for (const auto& r : a->_records) {
                const auto cost = static_cast<double>(r._data.size() + r._partition_key.size());
                if (b._records.tokens() - static_cast<double>(n) < 1.0 ||
                    b._bytes.tokens() - bytes < cost) {
                    break;
                }
                bytes += cost;
                ++n;
            }
            b._bytes.take(bytes);
            b._records.take(static_cast<double>(n));
            if (n > 0) {
                ticket._taken.push_back(
                    {._key = a->_key, ._bytes = bytes, ._records = static_cast<double>(n)});
            }
            out.push_back(n);
        }
        return out;
    }

    void refund(const admission_ticket& ticket) {
        const std::scoped_lock lock(_mutex);
        for (const auto& t : ticket._taken) {
            if (auto it = _shards.find(t._key); it != _shards.end()) {
                it->second._bytes.give(t._bytes);
                it->second._records.give(t._records);
            }
        }
    }

    [[nodiscard]] auto size() -> std::size_t {
        const std::scoped_lock lock(_mutex);
        return _shards.size();
    }

private:
    struct shard_buckets {
        token_bucket _bytes;
        token_bucket _records;
    };

    throughput_limits _limits;
    monotonic_ms _clock;
    std::mutex _mutex;
    std::map<std::string, shard_buckets, std::less<>> _shards;
    std::int64_t _swept_ms = 0;

    auto buckets(const std::string& key, std::int64_t now) -> shard_buckets& {
        auto it = _shards.find(key);
        if (it == _shards.end()) {
            it = _shards
                     .emplace(key, shard_buckets{token_bucket(_limits._write_bytes_per_s, now),
                                                 token_bucket(_limits._write_records_per_s, now)})
                     .first;
        }
        return it->second;
    }

    // A bucket idle for a minute is full again; forgetting it changes
    // nothing a client can see.
    void sweep(std::int64_t now) {
        if (now - _swept_ms < 60'000) {
            return;
        }
        _swept_ms = now;
        std::erase_if(_shards, [now](const auto& kv) {
            return now - kv.second._bytes.idle_since() > 60'000;
        });
    }
};

/// The read buckets of the shards this node serves. Thread-safe.
class read_throttle {
public:
    explicit read_throttle(throughput_limits limits = {}, monotonic_ms clock = steady_now_ms)
        : _limits(limits), _clock(std::move(clock)) {}

    /// A `GetRecords` on `key`: admitted while a call token is left and the
    /// byte bucket is positive. Takes the call token.
    auto admit_get_records(const std::string& key) -> bool {
        const std::scoped_lock lock(_mutex);
        auto& b = buckets(key);
        if (b._calls.tokens() < 1.0 || b._bytes.tokens() <= 0.0) {
            return false;
        }
        b._calls.take(1.0);
        return true;
    }

    /// Charges an admitted `GetRecords` for the bytes it returned.
    void charge(const std::string& key, std::uint64_t bytes) {
        const std::scoped_lock lock(_mutex);
        buckets(key)._bytes.take(static_cast<double>(bytes));
    }

    auto admit_get_shard_iterator(const std::string& key) -> bool {
        const std::scoped_lock lock(_mutex);
        auto& b = buckets(key);
        if (b._iterators.tokens() < 1.0) {
            return false;
        }
        b._iterators.take(1.0);
        return true;
    }

private:
    struct shard_buckets {
        token_bucket _bytes;
        token_bucket _calls;
        token_bucket _iterators;
    };

    throughput_limits _limits;
    monotonic_ms _clock;
    std::mutex _mutex;
    std::map<std::string, shard_buckets, std::less<>> _shards;
    std::int64_t _swept_ms = 0;

    auto buckets(const std::string& key) -> shard_buckets& {
        const auto now = _clock();
        if (now - _swept_ms >= 60'000) {
            _swept_ms = now;
            std::erase_if(_shards, [now](const auto& kv) {
                return now - kv.second._calls.idle_since() > 60'000 &&
                       now - kv.second._iterators.idle_since() > 60'000 &&
                       kv.second._bytes.tokens() >= 0.0;
            });
        }
        auto it = _shards.find(key);
        if (it == _shards.end()) {
            it = _shards
                     .emplace(key, shard_buckets{token_bucket(_limits._read_bytes_per_s, now),
                                                 token_bucket(_limits._read_calls_per_s, now),
                                                 token_bucket(_limits._iterator_calls_per_s, now)})
                     .first;
        }
        it->second._bytes.refill(now);
        it->second._calls.refill(now);
        it->second._iterators.refill(now);
        return it->second;
    }
};

/// Proposes `command` after the leader's write admission. `propose` makes
/// the proposal (and may throw, for instance when this node turns out not
/// to lead: the tokens are refunded and the exception passes on).
///
/// - Not an append, `admission::none`, or no throttle: proposed as is.
/// - An append: its admitted prefix is proposed; none admitted answers
///   `throttled` without proposing. A partial answer is an `ok` whose
///   counters cover fewer records than were sent.
/// - A batch: the admitted prefixes of its parts are proposed as one batch
///   (parts with nothing admitted are left out and answer `throttled`), and
///   the parts' results come back in the original order.
template<typename Propose>
auto propose_admitted(write_throttle* throttle, admission mode, const std::vector<std::byte>& bytes,
                      Propose&& propose) -> std::optional<apply_result> {
    if (throttle == nullptr || mode == admission::none) {
        return propose(bytes);
    }
    stream_command cmd;
    try {
        cmd = decode_command(bytes);
    } catch (const kinesis_codec_error&) {
        return propose(bytes);  // the state machine answers `invalid`
    }
    std::vector<append_records_command*> parts;
    if (auto* a = std::get_if<append_records_command>(&cmd)) {
        parts.push_back(a);
    } else if (auto* b = std::get_if<append_batch_command>(&cmd)) {
        for (auto& p : b->_appends) {
            parts.push_back(&p);
        }
    } else {
        return propose(bytes);
    }
    admission_ticket ticket;
    const auto admitted = throttle->admit(
        std::vector<const append_records_command*>(parts.begin(), parts.end()), ticket);
    bool all = true;
    bool none = true;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        all = all && admitted[i] == parts[i]->_records.size();
        none = none && admitted[i] == 0;
    }
    const auto guarded = [&](const std::vector<std::byte>& b) {
        try {
            return propose(b);
        } catch (...) {
            throttle->refund(ticket);
            throw;
        }
    };
    if (std::holds_alternative<append_records_command>(cmd)) {
        if (none) {
            return apply_result{._status = apply_status::throttled};
        }
        if (all) {
            return guarded(bytes);
        }
        parts[0]->_records.resize(admitted[0]);
        return guarded(encode_command(cmd));
    }
    // A batch.
    if (none) {
        apply_result out{._status = apply_status::throttled};
        out._parts.assign(parts.size(), append_part_result{._status = apply_status::throttled});
        return out;
    }
    if (all) {
        return guarded(bytes);
    }
    append_batch_command sent;
    std::vector<std::size_t> index;  // sent part -> original part
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (admitted[i] > 0) {
            parts[i]->_records.resize(admitted[i]);
            sent._appends.push_back(std::move(*parts[i]));
            index.push_back(i);
        }
    }
    auto r = guarded(encode_command(stream_command{std::move(sent)}));
    if (!r) {
        return r;
    }
    if (r->_parts.size() != index.size()) {
        // A one-part batch is still a batch; anything else is a bug.
        return std::nullopt;
    }
    apply_result out = *r;
    out._parts.assign(parts.size(), append_part_result{._status = apply_status::throttled});
    for (std::size_t j = 0; j < index.size(); ++j) {
        out._parts[index[j]] = r->_parts[j];
    }
    return out;
}

}  // namespace kythira::kinesis
