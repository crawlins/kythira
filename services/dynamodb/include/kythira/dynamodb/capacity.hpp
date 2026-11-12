// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief Capacity enforcement: token buckets per table, index and partition
/// key (`.kiro/specs/dynamodb-compatible-api/` task 12, Requirement 19.2).
///
/// A request is admitted while every bucket it draws on still holds tokens,
/// and is charged its consumed units once they are known (after the read or
/// write), so a bucket can go into debt by one request, as DynamoDB's own
/// admission does. Refill is continuous at the bucket's rate.
///
/// | Bucket | Rate | Holds at most |
/// |---|---|---|
/// | provisioned table or GSI | its provisioned units per second | 300 s of rate (burst capacity) |
/// | on-demand table or GSI | the per-table account quota (40,000) | 1 s of rate |
/// | partition key | 3,000 read / 1,000 write units per second | 1 s of rate |
///
/// A throttle names the bucket that ran dry with AWS's `ThrottlingReason`
/// (`<Table|Index><Read|Write><Limit>`), and is answered as AWS answers it:
/// `ProvisionedThroughputExceededException` for a provisioned limit or a hot
/// partition key, `ThrottlingException` for an on-demand table past its
/// account quota.
///
/// The throttler is per process. `local_backend` owns one, which is exact.
/// Each `raft_backend` node owns one holding its share of every limit
/// (`raft_backend_options::capacity_share`, one over the voters by default
/// in `dynamodb_gateway_node`), checked by the node that takes the request:
/// exact when clients spread their requests over the nodes, as AWS's SDKs
/// spread theirs over endpoints, and stricter when they do not.

#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/insights.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// The per-table quota an on-demand table or GSI is held to, in units per
/// second, read and write alike (AWS's default; `DescribeLimits` reports it).
inline constexpr double k_on_demand_table_units = 40000.0;
/// AWS's per-partition ceilings.
inline constexpr double k_partition_read_units = 3000.0;
inline constexpr double k_partition_write_units = 1000.0;
/// Seconds of unused provisioned capacity a bucket keeps (burst capacity).
inline constexpr double k_burst_seconds = 300.0;

enum class capacity_kind : std::uint8_t {
    read,
    write
};

/// One continuously refilled bucket.
class token_bucket {
public:
    token_bucket(double rate, double max_tokens, std::uint64_t now_ms)
        : _rate(rate), _max(max_tokens), _tokens(max_tokens), _at_ms(now_ms) {}

    /// Re-rates the bucket (an `UpdateTable`), keeping what it holds.
    auto set_limits(double rate, double max_tokens) -> void {
        _rate = rate;
        _max = max_tokens;
        _tokens = std::min(_tokens, _max);
    }

    auto refill(std::uint64_t now_ms) -> void {
        if (now_ms > _at_ms) {
            _tokens =
                std::min(_max, _tokens + _rate * static_cast<double>(now_ms - _at_ms) / 1000.0);
            _at_ms = now_ms;
        }
    }

    [[nodiscard]] auto admits() const -> bool { return _tokens > 0.0; }
    [[nodiscard]] auto full() const -> bool { return _tokens >= _max; }
    [[nodiscard]] auto tokens() const -> double { return _tokens; }
    auto charge(double units) -> void { _tokens -= units; }

private:
    double _rate;
    double _max;
    double _tokens;
    std::uint64_t _at_ms;
};

/// Why a request was throttled.
struct throttle {
    /// `ProvisionedThroughputExceededException` or `ThrottlingException`,
    /// unqualified.
    std::string exception;
    std::string message;
    /// AWS's `ThrottlingReason.reason`, e.g. `TableReadProvisionedThroughputExceeded`.
    std::string reason;
    /// The GSI that ran dry; empty for the table.
    std::string index;
};

namespace capacity_detail {

inline constexpr std::string_view k_table_message =
    "The level of configured provisioned throughput for the table was exceeded. "
    "Consider increasing your provisioning level with the UpdateTable API.";
inline constexpr std::string_view k_index_message =
    "The level of configured provisioned throughput for one or more global secondary "
    "indexes of the table was exceeded. Consider increasing your provisioning level for "
    "the under-provisioned global secondary indexes with the UpdateTable API";
inline constexpr std::string_view k_hot_message =
    "Throughput exceeds the current capacity of your table or index. DynamoDB is "
    "automatically scaling your table or index so please try again shortly. If exceptions "
    "persist, check if you have a hot key: "
    "https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/"
    "bp-partition-key-design.html";

/// A bucket's rate and size for a table or GSI.
struct limits {
    double rate = 0.0;
    double max_tokens = 0.0;
    bool provisioned = true;
};

[[nodiscard]] inline auto resource_limits(billing_mode billing, std::uint64_t provisioned_units)
    -> limits {
    if (billing == billing_mode::pay_per_request) {
        return {k_on_demand_table_units, k_on_demand_table_units, false};
    }
    const auto rate = static_cast<double>(std::max<std::uint64_t>(provisioned_units, 1));
    return {rate, rate * k_burst_seconds, true};
}

[[nodiscard]] inline auto reason(bool index, capacity_kind kind, std::string_view limit)
    -> std::string {
    std::string r = index ? "Index" : "Table";
    r += kind == capacity_kind::read ? "Read" : "Write";
    r += limit;
    return r;
}

}  // namespace capacity_detail

/// What one request draws on: the table, or one of its GSIs, and the
/// partition key it addresses (empty for a request spanning partitions, a
/// `Scan` or a `Query` without one).
struct capacity_target {
    /// Empty: the table itself.
    std::string index;
    capacity_kind kind = capacity_kind::read;
    /// The partition key's encoded bytes; empty when the request has none.
    std::string partition;
    /// The whole key's encoded bytes (partition and sort) when the target
    /// has a sort key and the request names one item; only Contributor
    /// Insights reads it (`insights.hpp`).
    std::string key;
};

/// Token buckets for every table this process enforces.
class capacity_throttler {
public:
    /// `share` scales every limit: this process's part of the cluster's.
    explicit capacity_throttler(double share = 1.0) : _share(share > 0.0 ? share : 1.0) {}

    /// Whether `target` may run now on `table`; the throttle that refuses it
    /// otherwise.
    [[nodiscard]] auto admit(const table_descriptor& table, const capacity_target& target,
                             std::uint64_t now_ms) -> std::optional<throttle> {
        const std::lock_guard lock(_mu);
        const auto lim = limits_of(table, target);
        if (!lim) {
            return std::nullopt;
        }
        auto& res = resource_bucket(table.table_id, target, *lim, now_ms);
        res.refill(now_ms);
        const bool index = !target.index.empty();
        if (!res.admits()) {
            if (lim->provisioned) {
                return throttle{
                    "ProvisionedThroughputExceededException",
                    std::string(index ? capacity_detail::k_index_message
                                      : capacity_detail::k_table_message),
                    capacity_detail::reason(index, target.kind, "ProvisionedThroughputExceeded"),
                    target.index};
            }
            return throttle{"ThrottlingException", std::string(capacity_detail::k_hot_message),
                            capacity_detail::reason(index, target.kind, "AccountLimitExceeded"),
                            target.index};
        }
        if (!target.partition.empty()) {
            auto& part = partition_bucket(table.table_id, target, now_ms);
            part.refill(now_ms);
            if (!part.admits()) {
                return throttle{
                    "ProvisionedThroughputExceededException",
                    std::string(capacity_detail::k_hot_message),
                    capacity_detail::reason(index, target.kind, "KeyRangeThroughputExceeded"),
                    target.index};
            }
        }
        return std::nullopt;
    }

    /// Takes `units` from the buckets `target` drew on.
    auto charge(const table_descriptor& table, const capacity_target& target, double units,
                std::uint64_t now_ms) -> void {
        const std::lock_guard lock(_mu);
        const auto lim = limits_of(table, target);
        if (!lim || units <= 0.0) {
            return;
        }
        auto& res = resource_bucket(table.table_id, target, *lim, now_ms);
        res.refill(now_ms);
        res.charge(units);
        if (!target.partition.empty()) {
            auto& part = partition_bucket(table.table_id, target, now_ms);
            part.refill(now_ms);
            part.charge(units);
        }
        sweep(now_ms);
    }

    /// This process's Contributor Insights counters, kept beside its
    /// buckets: both are per process and fed by the same requests.
    [[nodiscard]] auto insights() -> insights_recorder& { return _insights; }
    [[nodiscard]] auto insights() const -> const insights_recorder& { return _insights; }

    /// Forgets a dropped table's buckets and counters.
    auto forget(std::uint64_t table_id) -> void {
        _insights.forget(table_id);
        const std::lock_guard lock(_mu);
        std::erase_if(_resources,
                      [&](const auto& kv) { return std::get<0>(kv.first) == table_id; });
        std::erase_if(_partitions,
                      [&](const auto& kv) { return std::get<0>(kv.first) == table_id; });
    }

    /// Partition buckets currently held (tests).
    [[nodiscard]] auto partition_buckets() const -> std::size_t {
        const std::lock_guard lock(_mu);
        return _partitions.size();
    }

private:
    using resource_key = std::tuple<std::uint64_t, std::string, capacity_kind>;
    using partition_key = std::tuple<std::uint64_t, std::string, capacity_kind, std::string>;
    /// Partition buckets kept before full (idle) ones are swept.
    static constexpr std::size_t k_sweep_at = 4096;

    [[nodiscard]] auto limits_of(const table_descriptor& table, const capacity_target& target) const
        -> std::optional<capacity_detail::limits> {
        capacity_detail::limits lim;
        if (target.index.empty()) {
            lim = capacity_detail::resource_limits(table.billing, target.kind == capacity_kind::read
                                                                      ? table.read_capacity
                                                                      : table.write_capacity);
        } else {
            const auto it = std::ranges::find(table.gsis, target.index, &global_index::name);
            if (it == table.gsis.end()) {
                return std::nullopt;
            }
            lim = capacity_detail::resource_limits(table.billing, target.kind == capacity_kind::read
                                                                      ? it->read_capacity
                                                                      : it->write_capacity);
        }
        lim.rate *= _share;
        lim.max_tokens *= _share;
        return lim;
    }

    auto resource_bucket(std::uint64_t table_id, const capacity_target& target,
                         const capacity_detail::limits& lim, std::uint64_t now_ms)
        -> token_bucket& {
        const resource_key key{table_id, target.index, target.kind};
        auto it = _resources.find(key);
        if (it == _resources.end()) {
            it = _resources.emplace(key, token_bucket(lim.rate, lim.max_tokens, now_ms)).first;
        } else {
            it->second.set_limits(lim.rate, lim.max_tokens);
        }
        return it->second;
    }

    auto partition_bucket(std::uint64_t table_id, const capacity_target& target,
                          std::uint64_t now_ms) -> token_bucket& {
        const double rate = _share * (target.kind == capacity_kind::read ? k_partition_read_units
                                                                         : k_partition_write_units);
        const partition_key key{table_id, target.index, target.kind, target.partition};
        auto it = _partitions.find(key);
        if (it == _partitions.end()) {
            it = _partitions.emplace(key, token_bucket(rate, rate, now_ms)).first;
        }
        return it->second;
    }

    /// Drops partition buckets that have refilled: a fresh one is the same.
    auto sweep(std::uint64_t now_ms) -> void {
        if (_partitions.size() < k_sweep_at) {
            return;
        }
        std::erase_if(_partitions, [&](auto& kv) {
            kv.second.refill(now_ms);
            return kv.second.full();
        });
    }

    double _share;
    mutable std::mutex _mu;
    std::map<resource_key, token_bucket> _resources;
    std::map<partition_key, token_bucket> _partitions;
    insights_recorder _insights;
};

}  // namespace kythira::dynamodb
