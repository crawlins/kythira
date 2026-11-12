// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief Contributor Insights counters (`.kiro/specs/dynamodb-compatible-api/`
/// task 21, Requirement 19.4): the most-accessed and most-throttled keys of
/// a table or GSI.
///
/// AWS's four rules, by their code in the rule name:
///
/// | Rule | Counts | Weight |
/// |---|---|---|
/// | `PKC` | partition keys accessed | consumed read and write units |
/// | `SKC` | partition and sort keys accessed (sort-keyed tables) | consumed units |
/// | `PKT` | partition keys throttled | throttled requests |
/// | `SKT` | partition and sort keys throttled | throttled requests |
///
/// Each rule keeps one bounded counter per minute for the last hour. A
/// counter is a Space-Saving summary of `capacity` keys: a key not yet held
/// takes the place of the smallest and inherits its count, so every key
/// heavier than total/capacity is held, each count is high by at most the
/// count it inherited, and memory stays fixed however many keys a table
/// has. A report merges the minutes it covers. Counters belong to one
/// enable (the setting's `updated_ms`): after a disable and a new enable,
/// as AWS's new rules do, a table starts afresh on every node.
///
/// Counting happens where capacity is charged (`api_common.hpp`), for the
/// requests the node took, whether or not throttling is enforced; only
/// tables and indexes whose setting is enabled are counted, and `THROTTLED_KEYS`
/// mode counts only the throttle rules. Like the token buckets, the
/// counters are per process: a gateway node reports the requests it served.

#include <kythira/dynamodb/catalog.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

enum class insights_rule : std::uint8_t {
    pkc,  // most accessed partition keys
    skc,  // most accessed partition and sort keys
    pkt,  // most throttled partition keys
    skt,  // most throttled partition and sort keys
};

[[nodiscard]] constexpr auto rule_code(insights_rule r) noexcept -> std::string_view {
    switch (r) {
        case insights_rule::pkc:
            return "PKC";
        case insights_rule::skc:
            return "SKC";
        case insights_rule::pkt:
            return "PKT";
        case insights_rule::skt:
            return "SKT";
    }
    return "PKC";
}

/// A Space-Saving summary of at most `capacity` keys.
class top_k_counter {
public:
    explicit top_k_counter(std::size_t capacity) : _capacity(std::max<std::size_t>(capacity, 1)) {}

    auto add(const std::string& key, double weight) -> void {
        if (auto it = _counts.find(key); it != _counts.end()) {
            it->second += weight;
            return;
        }
        if (_counts.size() < _capacity) {
            _counts.emplace(key, weight);
            return;
        }
        const auto smallest =
            std::min_element(_counts.begin(), _counts.end(),
                             [](const auto& a, const auto& b) { return a.second < b.second; });
        const double inherited = smallest->second;
        _counts.erase(smallest);
        _counts.emplace(key, inherited + weight);
    }

    [[nodiscard]] auto counts() const -> const std::unordered_map<std::string, double>& {
        return _counts;
    }

private:
    std::size_t _capacity;
    std::unordered_map<std::string, double> _counts;
};

/// What a request is counted against: the table (`index` empty) or a GSI,
/// the partition key's encoded bytes, and the whole key's (partition and
/// sort, `encode_item` of both) when the target has a sort key and the
/// request names one item.
struct insights_target {
    std::string_view index;
    std::string_view partition;
    std::string_view key;
};

class insights_recorder {
public:
    /// One contributor in a report: an encoded key (`encode_item` of the
    /// key attributes) and its value.
    struct contributor {
        std::string key;
        double value = 0.0;
    };

    explicit insights_recorder(std::size_t capacity = 256, std::uint64_t minutes = 60)
        : _capacity(capacity), _minutes(std::max<std::uint64_t>(minutes, 1)) {}

    /// Counts `units` consumed by a request to `table` against `target`,
    /// when its setting counts accesses.
    auto accessed(const table_descriptor& table, const insights_target& target, double units,
                  std::uint64_t now_ms) -> void {
        const auto* s = table.insights_of(target.index);
        if (s == nullptr || !s->enabled || s->mode != insights_mode::accessed_and_throttled ||
            units <= 0.0) {
            return;
        }
        const std::lock_guard lock(_mu);
        if (!target.partition.empty()) {
            add(table.table_id, *s, insights_rule::pkc, target.partition, units, now_ms);
        }
        if (!target.key.empty()) {
            add(table.table_id, *s, insights_rule::skc, target.key, units, now_ms);
        }
    }

    /// Counts one throttled request, when the setting is enabled.
    auto throttled(const table_descriptor& table, const insights_target& target,
                   std::uint64_t now_ms) -> void {
        const auto* s = table.insights_of(target.index);
        if (s == nullptr || !s->enabled) {
            return;
        }
        const std::lock_guard lock(_mu);
        if (!target.partition.empty()) {
            add(table.table_id, *s, insights_rule::pkt, target.partition, 1.0, now_ms);
        }
        if (!target.key.empty()) {
            add(table.table_id, *s, insights_rule::skt, target.key, 1.0, now_ms);
        }
    }

    /// The `n` heaviest keys of `rule` for `setting` of `table` over the
    /// last `minutes` minutes (the current one included), heaviest first;
    /// ties by key.
    [[nodiscard]] auto top(const table_descriptor& table, const contributor_insights& setting,
                           insights_rule rule, std::uint64_t now_ms, std::uint64_t minutes,
                           std::size_t n) const -> std::vector<contributor> {
        const std::lock_guard lock(_mu);
        const auto it = _rules.find(rule_key{table.table_id, setting.index, rule});
        if (it == _rules.end() || it->second.generation != setting.updated_ms) {
            return {};
        }
        const auto now_minute = now_ms / k_minute_ms;
        const auto span = std::min(std::max<std::uint64_t>(minutes, 1), _minutes);
        std::map<std::string, double> merged;
        for (const auto& [minute, counter] : it->second.minutes) {
            if (minute + span <= now_minute) {
                continue;
            }
            for (const auto& [key, value] : counter.counts()) {
                merged[key] += value;
            }
        }
        std::vector<contributor> out;
        out.reserve(merged.size());
        for (auto& [key, value] : merged) {
            out.push_back(contributor{key, value});
        }
        std::stable_sort(out.begin(), out.end(), [](const contributor& a, const contributor& b) {
            return a.value > b.value;
        });
        if (out.size() > n) {
            out.resize(n);
        }
        return out;
    }

    /// Forgets a dropped table's counters.
    auto forget(std::uint64_t table_id) -> void {
        const std::lock_guard lock(_mu);
        std::erase_if(_rules, [&](const auto& kv) { return std::get<0>(kv.first) == table_id; });
    }

private:
    static constexpr std::uint64_t k_minute_ms = 60'000;
    using rule_key = std::tuple<std::uint64_t, std::string, insights_rule>;
    struct rule_counters {
        /// The enable these count for (`updated_ms` of the setting).
        std::uint64_t generation = 0;
        std::deque<std::pair<std::uint64_t, top_k_counter>> minutes;
    };

    auto add(std::uint64_t table_id, const contributor_insights& setting, insights_rule rule,
             std::string_view key, double weight, std::uint64_t now_ms) -> void {
        auto& counters = _rules[rule_key{table_id, setting.index, rule}];
        if (counters.generation != setting.updated_ms) {
            counters = rule_counters{setting.updated_ms, {}};
        }
        auto& minutes = counters.minutes;
        const auto minute = now_ms / k_minute_ms;
        if (minutes.empty() || minutes.back().first < minute) {
            minutes.emplace_back(minute, top_k_counter(_capacity));
        }
        while (minutes.front().first + _minutes <= minute) {
            minutes.pop_front();
        }
        // A clock that stepped back counts into the newest minute.
        minutes.back().second.add(std::string(key), weight);
    }

    std::size_t _capacity;
    std::uint64_t _minutes;
    mutable std::mutex _mu;
    std::map<rule_key, rule_counters> _rules;
};

}  // namespace kythira::dynamodb
