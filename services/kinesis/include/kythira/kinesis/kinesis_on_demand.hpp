// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kinesis_on_demand.hpp
/// @brief The on-demand capacity controller (`.kiro/specs/kinesis-compatible-streams/`
///        task 12, Requirement 10.3).
///
/// An ON_DEMAND stream has no shard count to manage: the controller splits
/// shards as writes grow. Every node runs `run_once()` on the maintenance
/// timer. For each open shard of each ACTIVE on-demand stream, the node
/// that leads the shard's group, which is where the shard's writes are
/// admitted, samples what the shard has taken:
///
/// - records from the shard's last counter, which counts every record
///   appended;
/// - bytes from its retained bytes. A window in which a trim moved the
///   trim horizon counts no bytes, since a trim lowers the total.
///
/// Over a window of at least `_window_ms`, a shard is **hot** when it took
/// `_split_fraction` of a shard's write limit (1 MiB/s or 1000 records/s)
/// or more. A shard hot without a break for `_sustain_ms` is split at the
/// middle of its hash range (`kinesis_service::split_for_capacity`), so the
/// stream's write capacity grows by one shard's worth. At most one split
/// per stream per pass, and none past `_max_open_shards` (AWS's on-demand
/// ceiling is 200 MiB/s of writes per stream by default, so 200 shards) or
/// the account's shard limit.
///
/// With throttling on, a shard's accepted rate stops at its limit, which is
/// above the hot threshold, so a throttled shard is split like any other.
/// The controller never merges: AWS sizes an on-demand stream for twice its
/// peak of the last 30 days, and capacity that shrinks as soon as traffic
/// does would not meet that. Samples live in memory. A new leader starts
/// sampling from scratch, which costs one sustain period before a split.

#include <kythira/kinesis/kinesis_catalog.hpp>
#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_service.hpp>
#include <kythira/kinesis/kinesis_stream_state_machine.hpp>
#include <kythira/kinesis/kinesis_throttle.hpp>

#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kythira::kinesis {

struct on_demand_config {
    /// The share of a shard's write limit that makes it hot.
    double _split_fraction = 0.5;
    /// The shortest window a rate is measured over.
    std::int64_t _window_ms = 10'000;
    /// How long a shard must stay hot before it is split.
    std::int64_t _sustain_ms = 60'000;
    /// No split past this many open shards in one stream.
    std::uint32_t _max_open_shards = 200;
    /// A shard's write limits.
    throughput_limits _limits{};
};

/// What one pass did (logs and tests).
struct on_demand_stats {
    std::uint64_t _hot = 0;
    std::uint64_t _splits = 0;
    std::uint64_t _failures = 0;
};

template<typename Cluster>
requires stream_cluster<Cluster> && leads_keys<Cluster> && transacts<Cluster>
class on_demand_controller {
public:
    on_demand_controller(Cluster& cluster, kinesis_service<Cluster>& service,
                         on_demand_config config = {})
        : _cluster(cluster), _service(service), _config(config) {}

    auto run_once(std::int64_t now_ms) -> on_demand_stats {
        on_demand_stats stats;
        std::map<std::string, sample> kept;
        for (const auto& s : streams()) {
            bool split = false;
            for (const auto& [ordinal, open] : s._shards) {
                const auto key = routing_key(s._id, ordinal);
                if (!open || !_cluster.leads(key)) {
                    continue;
                }
                const auto now_sample = observe(key, now_ms);
                if (!now_sample) {
                    continue;
                }
                auto next = step(key, *now_sample);
                if (!next._hot_since) {
                    kept.insert_or_assign(key, next);
                    continue;
                }
                ++stats._hot;
                if (!split && s._open < _config._max_open_shards &&
                    now_ms - *next._hot_since >= _config._sustain_ms) {
                    split = true;
                    try {
                        if (_service.split_for_capacity(s._account, s._name, s._id, ordinal)) {
                            ++stats._splits;
                            continue;  // the shard is closed: forget it
                        }
                    } catch (const std::exception&) {
                    }
                    ++stats._failures;
                }
                kept.insert_or_assign(key, next);
            }
        }
        // Shards that closed, moved away or went quiet in a stream that left
        // on-demand are dropped with everything else not seen this pass.
        _samples = std::move(kept);
        return stats;
    }

private:
    struct stream_view {
        std::string _account;
        std::string _name;
        std::uint64_t _id = 0;
        std::size_t _open = 0;
        std::vector<std::pair<std::uint32_t, bool>> _shards;
    };

    struct sample {
        std::int64_t _at_ms = 0;
        std::uint64_t _counter = 0;
        std::uint64_t _bytes = 0;
        std::uint64_t _trim_before = 0;
        /// When the shard became hot without a break since; empty while not.
        std::optional<std::int64_t> _hot_since;
    };

    Cluster& _cluster;
    kinesis_service<Cluster>& _service;
    on_demand_config _config;
    std::map<std::string, sample> _samples;

    auto streams() -> std::vector<stream_view> {
        std::vector<stream_view> out;
        const auto prefix = streams_prefix();
        (void)_cluster.read(next_id_key(), [&](const auto& sm) {
            const auto& entries = sm.entries();
            for (auto it = entries.lower_bound(prefix);
                 it != entries.end() && it->first.starts_with(prefix); ++it) {
                const auto* rec = std::get_if<stream_record>(&it->second);
                if (rec == nullptr || rec->_mode != stream_mode::on_demand ||
                    rec->_status != stream_status::active) {
                    continue;
                }
                stream_view v{._account = rec->_account,
                              ._name = rec->_name,
                              ._id = rec->_id,
                              ._open = rec->open_shard_count(),
                              ._shards = {}};
                for (const auto& sh : rec->_shards) {
                    v._shards.emplace_back(sh._ordinal, sh.open());
                }
                out.push_back(std::move(v));
            }
        });
        return out;
    }

    auto observe(const std::string& key, std::int64_t now_ms) -> std::optional<sample> {
        std::optional<sample> out;
        (void)_cluster.read(key, [&](const auto& sm) {
            if (const auto* st = sm.find_shard(key)) {
                out = sample{._at_ms = now_ms,
                             ._counter = st->_last_counter,
                             ._bytes = st->_retained_bytes,
                             ._trim_before = st->_trim_before,
                             ._hot_since = std::nullopt};
            }
        });
        return out;
    }

    // The sample to keep for `key`: `now` when a window has closed (with
    // its hot mark carried or cleared), else the previous one unchanged.
    auto step(const std::string& key, sample now) -> sample {
        const auto it = _samples.find(key);
        if (it == _samples.end()) {
            return now;
        }
        const auto& before = it->second;
        const auto span = now._at_ms - before._at_ms;
        if (span < _config._window_ms) {
            return before;
        }
        const auto seconds = static_cast<double>(span) / 1000.0;
        const auto records = now._counter > before._counter
                                 ? static_cast<double>(now._counter - before._counter)
                                 : 0;
        const auto bytes = now._trim_before == before._trim_before && now._bytes > before._bytes
                               ? static_cast<double>(now._bytes - before._bytes)
                               : 0;
        const bool hot =
            records / seconds >= _config._split_fraction * _config._limits._write_records_per_s ||
            bytes / seconds >= _config._split_fraction * _config._limits._write_bytes_per_s;
        if (hot) {
            now._hot_since = before._hot_since.value_or(before._at_ms);
        }
        return now;
    }
};

}  // namespace kythira::kinesis
