// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kinesis_maintenance.hpp
/// @brief The leader-driven retention and tiering pass
///        (`.kiro/specs/kinesis-compatible-streams/` task 10, Requirements
///        11.2 and 11.3).
///
/// Every node runs `run_once()` on a timer. For each shard of each stream
/// in the catalog, the node that leads the shard's group:
///
/// 1. **seals** the active segment once its first record is old enough to
///    tier or trim (`seal_shard_command`), since an idle shard's active
///    segment would otherwise never roll;
/// 2. **trims** the sealed segments whose last record is older than the
///    stream's retention period (`trim_shard_command`), then deletes the
///    objects of the tiered ones among them;
/// 3. **tiers** sealed segments whose last record is older than
///    `_tier_after_ms`: it uploads the bytes under the segment's object key
///    and then proposes `tier_segment_command`, on whose apply every
///    replica drops its local copy.
///
/// Every node, leader or not, also drops the local copy of a tiered segment
/// that a read fetched back once no read has wanted it for `_keep_fetched_ms`.
///
/// Decisions are made against the wall clock here, outside `apply()`; the
/// entries name segments by first counter and digest, so each applies the
/// same everywhere and is a no-op when it has already happened. A leader
/// that loses leadership mid-pass costs at most an upload whose tier entry
/// never commits (its successor re-uploads the same bytes under the same
/// key) or, if it dies between a trim committing and the object deletes,
/// objects nobody references any more. Those leak until an operator sweeps
/// `<prefix>kinesis/segments/`; the shard keys in object names make that a
/// listing against the catalog.
///
/// Retention is "at least": a segment is trimmed when its *last* record is
/// past the period, so records stay readable up to one segment age (an
/// hour by default) longer than the period, as AWS allows.

#include <kythira/kinesis/kinesis_catalog.hpp>
#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_stream_state_machine.hpp>
#include <kythira/kinesis/kinesis_tiering.hpp>

#include <raft/record_segment_store.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kythira::kinesis {

struct maintenance_config {
    /// Tier sealed segments whose last record is this old; 0 never tiers.
    std::int64_t _tier_after_ms = 0;
    /// Keep a fetched-back copy of a tiered segment this long after the
    /// read that fetched it.
    std::int64_t _keep_fetched_ms = std::int64_t{10} * 60 * 1000;
};

/// What one pass did (logs and tests).
struct maintenance_stats {
    std::uint64_t _seals = 0;
    std::uint64_t _trims = 0;
    std::uint64_t _tiered = 0;
    std::uint64_t _objects_deleted = 0;
    std::uint64_t _evicted = 0;
    /// Failed proposals, uploads or deletes; the next pass retries them.
    std::uint64_t _failures = 0;
};

inline constexpr std::int64_t k_ms_per_hour = std::int64_t{3600} * 1000;

template<typename Cluster>
requires stream_cluster<Cluster> && leads_keys<Cluster>
class stream_maintenance {
public:
    stream_maintenance(Cluster& cluster, record_segment_store& store, tier_store tier,
                       fetched_segments& fetched, maintenance_config config = {})
        : _cluster(cluster),
          _store(store),
          _tier(std::move(tier)),
          _fetched(fetched),
          _config(config) {}

    auto run_once() -> maintenance_stats {
        maintenance_stats stats;
        for (const auto& s : streams()) {
            for (const auto ordinal : s._ordinals) {
                try {
                    shard(routing_key(s._id, ordinal), s._retention_hours, stats);
                } catch (const std::exception&) {
                    ++stats._failures;
                }
            }
        }
        return stats;
    }

private:
    struct stream_view {
        std::uint64_t _id = 0;
        std::uint32_t _retention_hours = 24;
        std::vector<std::uint32_t> _ordinals;
    };

    Cluster& _cluster;
    record_segment_store& _store;
    tier_store _tier;
    fetched_segments& _fetched;
    maintenance_config _config;

    // Streams with shards to look after, from this replica's catalog.
    // CREATING and DELETING streams are left alone: their shards are being
    // opened or dropped.
    auto streams() -> std::vector<stream_view> {
        std::vector<stream_view> out;
        const auto prefix = streams_prefix();
        (void)_cluster.read(next_id_key(), [&](const auto& sm) {
            const auto& entries = sm.entries();
            for (auto it = entries.lower_bound(prefix);
                 it != entries.end() && it->first.starts_with(prefix); ++it) {
                const auto* rec = std::get_if<stream_record>(&it->second);
                if (rec == nullptr || (rec->_status != stream_status::active &&
                                       rec->_status != stream_status::updating)) {
                    continue;
                }
                stream_view v{
                    ._id = rec->_id, ._retention_hours = rec->_retention_hours, ._ordinals = {}};
                for (const auto& sh : rec->_shards) {
                    v._ordinals.push_back(sh._ordinal);
                }
                out.push_back(std::move(v));
            }
        });
        return out;
    }

    auto copy_shard(const std::string& key) -> std::optional<shard_state> {
        std::optional<shard_state> out;
        (void)_cluster.read(key, [&](const auto& sm) {
            if (const auto* s = sm.find_shard(key)) {
                out = *s;
            }
        });
        return out;
    }

    auto propose_ok(const std::string& key, const stream_command& c) -> bool {
        const auto r = _cluster.propose(key, encode_command(c), admission::none);
        return r && r->_status == apply_status::ok;
    }

    void shard(const std::string& key, std::uint32_t retention_hours, maintenance_stats& stats) {
        auto state = copy_shard(key);
        if (!state) {
            return;
        }
        const auto now = _cluster.now_ms();
        if (_cluster.leads(key)) {
            lead(key, *state, retention_hours, now, stats);
            state = copy_shard(key);
            if (!state) {
                return;
            }
        }
        evict(key, *state, now, stats);
    }

    void lead(const std::string& key, const shard_state& state, std::uint32_t retention_hours,
              std::int64_t now, maintenance_stats& stats) {
        const auto retention_cut = now - static_cast<std::int64_t>(retention_hours) * k_ms_per_hour;
        const bool tiering = _tier.configured() && _config._tier_after_ms > 0;
        const auto tier_cut = now - _config._tier_after_ms;

        // 1. Seal an active segment old enough for either.
        const auto seal_cut = tiering ? std::max(retention_cut, tier_cut) : retention_cut;
        auto local = _store.local_segments(key);
        if (!local.empty() && !local.back()._sealed && local.back()._records > 0 &&
            local.back()._first_timestamp_ms < seal_cut) {
            if (propose_ok(key, seal_shard_command{key, local.back()._first_seq})) {
                ++stats._seals;
            } else {
                ++stats._failures;
            }
        }

        // 2. Trim past retention.
        std::uint64_t before = state._trim_before;
        std::vector<std::string> doomed;
        for (const auto& d : state._sealed) {
            if (d._last_timestamp_ms >= retention_cut) {
                break;
            }
            before = std::max(before, d._last_seq + 1);
            if (state.is_tiered(d._first_seq)) {
                doomed.push_back(segment_object_key(_tier._prefix, d));
            }
        }
        if (before > state._trim_before) {
            if (propose_ok(key, trim_shard_command{key, before})) {
                ++stats._trims;
                for (const auto& object : doomed) {
                    if (!_tier.configured()) {
                        break;
                    }
                    try {
                        _tier._remove(object);
                        ++stats._objects_deleted;
                    } catch (const std::exception&) {
                        ++stats._failures;
                    }
                }
            } else {
                ++stats._failures;
                return;
            }
        }

        // 3. Tier what is old enough and survived the trim.
        if (!tiering) {
            return;
        }
        for (const auto& d : state._sealed) {
            if (d._last_seq < before || state.is_tiered(d._first_seq)) {
                continue;
            }
            if (d._last_timestamp_ms >= tier_cut) {
                break;
            }
            auto bytes = _store.read_segment_bytes(key, d._first_seq, d._bytes);
            if (!bytes || bytes->size() != d._bytes ||
                kythira::detail::segments::sha256_of(*bytes) != d._sha256) {
                // Not here, or not intact: tiering it is someone else's
                // job, after a read has fetched a good copy.
                ++stats._failures;
                continue;
            }
            try {
                _tier._put(
                    segment_object_key(_tier._prefix, d),
                    std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
            } catch (const std::exception&) {
                ++stats._failures;
                continue;
            }
            if (propose_ok(key, tier_segment_command{key, d._first_seq, d._sha256})) {
                ++stats._tiered;
            } else {
                ++stats._failures;
            }
        }
    }

    void evict(const std::string& key, const shard_state& state, std::int64_t now,
               maintenance_stats& stats) {
        for (const auto first : state._tiered) {
            if (_fetched.recent(key, first, now, _config._keep_fetched_ms)) {
                continue;
            }
            if (_store.remove_segment(key, first)) {
                _fetched.forget(key, first);
                ++stats._evicted;
            }
        }
    }
};

}  // namespace kythira::kinesis
