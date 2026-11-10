// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kinesis_metrics.hpp
/// @brief Per-stream and per-shard metrics (`.kiro/specs/kinesis-compatible-streams/`
///        task 15, Requirements 14.4 and 16.2), in the Prometheus text format.
///
/// Each node counts what it served: the writes it accepted and the reads it
/// answered, with throttles and `GetRecords` latency. So a cluster's totals
/// are the sum over its nodes, which is how Prometheus aggregates anyway. A
/// write forwarded to the leader is counted on the node the client called,
/// once.
///
/// - **Stream-level series** carry `account` and `stream` labels and are
///   always there.
/// - **Shard-level series** add a `shard` label. Most are there only for the
///   metrics `EnableEnhancedMonitoring` switched on for the stream, as
///   CloudWatch's shard-level metrics are. Segment bytes (local and tiered)
///   are always per shard: they describe storage, not traffic.
///
/// | Series | Stream | Shard (enhanced metric) |
/// |---|---|---|
/// | `kinesis_incoming_bytes_total`, `kinesis_incoming_records_total` | yes | `IncomingBytes`,
/// `IncomingRecords` | | `kinesis_outgoing_bytes_total`, `kinesis_outgoing_records_total` | yes |
/// `OutgoingBytes`, `OutgoingRecords` | | `kinesis_write_throttled_total`,
/// `kinesis_read_throttled_total` | yes | `WriteProvisionedThroughputExceeded`,
/// `ReadProvisionedThroughputExceeded` | | `kinesis_iterator_age_milliseconds` (the last read's
/// `MillisBehindLatest`; the stream's is the largest) | yes | `IteratorAgeMilliseconds` | |
/// `kinesis_get_records_latency_milliseconds_sum`, `_count` | yes | none | |
/// `kinesis_segment_bytes{location="local"\|"tiered"}` | yes | always | |
/// `kinesis_fanout_subscriptions` | yes | none |
///
/// Counters live in memory from process start. A stream deleted and
/// created again under the same name continues its series, which is all a
/// Prometheus counter needs.

#include <algorithm>
#include <compare>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace kythira::kinesis {

struct metric_shard_key {
    std::string _account;
    std::string _stream;
    std::uint32_t _ordinal = 0;
    friend auto operator<=>(const metric_shard_key&, const metric_shard_key&) = default;
};

struct shard_counters {
    std::uint64_t _incoming_bytes = 0;
    std::uint64_t _incoming_records = 0;
    std::uint64_t _outgoing_bytes = 0;
    std::uint64_t _outgoing_records = 0;
    std::uint64_t _write_throttled = 0;
    std::uint64_t _read_throttled = 0;
    std::int64_t _iterator_age_ms = 0;
    std::uint64_t _get_records_count = 0;
    std::uint64_t _get_records_latency_ms = 0;
};

/// The counters this node keeps. Thread-safe.
class stream_metrics {
public:
    void wrote(const metric_shard_key& k, std::uint64_t records, std::uint64_t bytes) {
        const std::scoped_lock lock(_mutex);
        auto& c = _shards[k];
        c._incoming_records += records;
        c._incoming_bytes += bytes;
    }

    void write_throttled(const metric_shard_key& k, std::uint64_t records) {
        const std::scoped_lock lock(_mutex);
        _shards[k]._write_throttled += records;
    }

    void read(const metric_shard_key& k, std::uint64_t records, std::uint64_t bytes,
              std::uint64_t latency_ms, std::int64_t behind_ms) {
        const std::scoped_lock lock(_mutex);
        auto& c = _shards[k];
        c._outgoing_records += records;
        c._outgoing_bytes += bytes;
        c._iterator_age_ms = behind_ms;
        ++c._get_records_count;
        c._get_records_latency_ms += latency_ms;
    }

    /// A SubscribeToShard event (task 13): outgoing records and bytes and
    /// the iterator age, not a GetRecords call.
    void fanned_out(const metric_shard_key& k, std::uint64_t records, std::uint64_t bytes,
                    std::int64_t behind_ms) {
        const std::scoped_lock lock(_mutex);
        auto& c = _shards[k];
        c._outgoing_records += records;
        c._outgoing_bytes += bytes;
        c._iterator_age_ms = behind_ms;
    }

    void read_throttled(const metric_shard_key& k) {
        const std::scoped_lock lock(_mutex);
        ++_shards[k]._read_throttled;
    }

    [[nodiscard]] auto snapshot() const -> std::map<metric_shard_key, shard_counters> {
        const std::scoped_lock lock(_mutex);
        return _shards;
    }

private:
    mutable std::mutex _mutex;
    std::map<metric_shard_key, shard_counters> _shards;
};

/// What rendering needs to know about one stream, from the catalog and the
/// segment store.
struct stream_metric_view {
    std::string _account;
    std::string _stream;
    /// Shard-level metrics switched on (`EnableEnhancedMonitoring`).
    std::set<std::string> _shard_metrics;
    struct shard {
        std::uint32_t _ordinal = 0;
        std::string _id;
        std::uint64_t _local_bytes = 0;
        std::uint64_t _tiered_bytes = 0;
    };
    std::vector<shard> _shards;
    std::uint64_t _fanout_subscriptions = 0;
};

namespace metrics_detail {

// Prometheus label values escape backslash, double quote and newline.
[[nodiscard]] inline auto escape(std::string_view v) -> std::string {
    std::string out;
    out.reserve(v.size());
    for (const char c : v) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace metrics_detail

/// The Prometheus exposition of `streams` with this node's `counters`.
[[nodiscard]] inline auto render_metrics(const std::vector<stream_metric_view>& streams,
                                         const std::map<metric_shard_key, shard_counters>& counters)
    -> std::string {
    struct family {
        const char* _name;
        const char* _type;
        const char* _help;
        const char* _enhanced;  // the shard-level switch, or nullptr
        std::uint64_t shard_counters::* _counter;
    };
    static constexpr family k_counters[] = {
        {"kinesis_incoming_bytes_total", "counter", "Bytes of records accepted.", "IncomingBytes",
         &shard_counters::_incoming_bytes},
        {"kinesis_incoming_records_total", "counter", "Records accepted.", "IncomingRecords",
         &shard_counters::_incoming_records},
        {"kinesis_outgoing_bytes_total", "counter", "Bytes of records returned by GetRecords.",
         "OutgoingBytes", &shard_counters::_outgoing_bytes},
        {"kinesis_outgoing_records_total", "counter", "Records returned by GetRecords.",
         "OutgoingRecords", &shard_counters::_outgoing_records},
        {"kinesis_write_throttled_total", "counter", "Records refused by the write limits.",
         "WriteProvisionedThroughputExceeded", &shard_counters::_write_throttled},
        {"kinesis_read_throttled_total", "counter", "Reads refused by the read limits.",
         "ReadProvisionedThroughputExceeded", &shard_counters::_read_throttled},
        {"kinesis_get_records_latency_milliseconds_sum", "counter",
         "Total time spent answering GetRecords.", nullptr,
         &shard_counters::_get_records_latency_ms},
        {"kinesis_get_records_latency_milliseconds_count", "counter", "GetRecords answered.",
         nullptr, &shard_counters::_get_records_count},
    };
    const auto labels = [](const stream_metric_view& s) {
        return "account=\"" + metrics_detail::escape(s._account) + "\",stream=\"" +
               metrics_detail::escape(s._stream) + "\"";
    };
    const auto counters_of = [&](const stream_metric_view& s, std::uint32_t ordinal) {
        const auto it = counters.find(metric_shard_key{s._account, s._stream, ordinal});
        return it == counters.end() ? shard_counters{} : it->second;
    };

    std::string out;
    for (const auto& f : k_counters) {
        out += std::string("# HELP ") + f._name + " " + f._help + "\n";
        out += std::string("# TYPE ") + f._name + " " + f._type + "\n";
        for (const auto& s : streams) {
            std::uint64_t total = 0;
            std::string shard_lines;
            const bool enhanced = f._enhanced != nullptr && s._shard_metrics.contains(f._enhanced);
            for (const auto& sh : s._shards) {
                const auto v = counters_of(s, sh._ordinal).*(f._counter);
                total += v;
                if (enhanced) {
                    shard_lines += std::string(f._name) + "{" + labels(s) + ",shard=\"" + sh._id +
                                   "\"} " + std::to_string(v) + "\n";
                }
            }
            out += std::string(f._name) + "{" + labels(s) + "} " + std::to_string(total) + "\n";
            out += shard_lines;
        }
    }

    out += "# HELP kinesis_iterator_age_milliseconds MillisBehindLatest of the last GetRecords.\n";
    out += "# TYPE kinesis_iterator_age_milliseconds gauge\n";
    for (const auto& s : streams) {
        std::int64_t oldest = 0;
        std::string shard_lines;
        const bool enhanced = s._shard_metrics.contains("IteratorAgeMilliseconds");
        for (const auto& sh : s._shards) {
            const auto v = counters_of(s, sh._ordinal)._iterator_age_ms;
            oldest = std::max(oldest, v);
            if (enhanced) {
                shard_lines += "kinesis_iterator_age_milliseconds{" + labels(s) + ",shard=\"" +
                               sh._id + "\"} " + std::to_string(v) + "\n";
            }
        }
        out += "kinesis_iterator_age_milliseconds{" + labels(s) + "} " + std::to_string(oldest) +
               "\n" + shard_lines;
    }

    out +=
        "# HELP kinesis_segment_bytes Record segment bytes on this node and in object "
        "storage.\n";
    out += "# TYPE kinesis_segment_bytes gauge\n";
    for (const auto& s : streams) {
        std::uint64_t local = 0;
        std::uint64_t tiered = 0;
        std::string shard_lines;
        for (const auto& sh : s._shards) {
            local += sh._local_bytes;
            tiered += sh._tiered_bytes;
            shard_lines += "kinesis_segment_bytes{" + labels(s) + ",shard=\"" + sh._id +
                           "\",location=\"local\"} " + std::to_string(sh._local_bytes) + "\n";
            shard_lines += "kinesis_segment_bytes{" + labels(s) + ",shard=\"" + sh._id +
                           "\",location=\"tiered\"} " + std::to_string(sh._tiered_bytes) + "\n";
        }
        out += "kinesis_segment_bytes{" + labels(s) + ",location=\"local\"} " +
               std::to_string(local) + "\n";
        out += "kinesis_segment_bytes{" + labels(s) + ",location=\"tiered\"} " +
               std::to_string(tiered) + "\n";
        out += shard_lines;
    }

    out += "# HELP kinesis_fanout_subscriptions Open SubscribeToShard subscriptions.\n";
    out += "# TYPE kinesis_fanout_subscriptions gauge\n";
    for (const auto& s : streams) {
        out += "kinesis_fanout_subscriptions{" + labels(s) + "} " +
               std::to_string(s._fanout_subscriptions) + "\n";
    }
    return out;
}

}  // namespace kythira::kinesis
