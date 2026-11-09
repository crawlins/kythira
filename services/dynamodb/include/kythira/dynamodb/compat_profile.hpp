// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file compat_profile.hpp
/// @brief The AWS-behaviour switches (`.kiro/specs/dynamodb-compatible-api/`
///        Requirement 3).
///
/// `dynamo_compat_profile` holds one switch per behaviour an operator may
/// relax. The default, profile `aws`, is AWS's behaviour for every switch,
/// in every deployment, test clusters included: throttling is on unless an
/// operator turns it off. A cluster sets switches in its config; a table
/// overrides them with tags `kythira:compat:<switch>`, and the table's value
/// wins. No switch changes an error code, message or status (3.4).
///
///   switch               AWS value  relaxed value
///   limits               aws        relaxed   item, key, page, batch, txn sizes
///   throttling           on         off       capacity enforcement, on-demand throttling
///   capacity-reporting   on         off       ConsumedCapacity, ItemCollectionMetrics
///   gsi-consistency      async      sync      GSI propagation
///   count-limits         aws        relaxed   table and index counts
///   ttl-delay            aws        prompt    TTL deletion window
///
/// Names and values are matched case-insensitively. An unknown switch or
/// value is an error, so a typo in a tag fails at `TagResource` or
/// `CreateTable` instead of silently keeping AWS's behaviour.

#include <kythira/dynamodb/errors.hpp>

#include <raft/aws_json_protocol.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

inline constexpr std::string_view k_compat_tag_prefix = "kythira:compat:";

struct dynamo_compat_profile {
    bool aws_limits = true;
    bool throttling = true;
    bool capacity_reporting = true;
    bool async_gsi = true;
    bool aws_count_limits = true;
    bool aws_ttl_delay = true;

    friend auto operator==(const dynamo_compat_profile&, const dynamo_compat_profile&)
        -> bool = default;

    /// The switches set away from AWS's behaviour, by name, for
    /// `DescribeTable`'s Kythira extension field and for metrics (3.3).
    [[nodiscard]] auto relaxed() const -> std::vector<std::string_view> {
        std::vector<std::string_view> out;
        if (!aws_limits) {
            out.emplace_back("limits");
        }
        if (!throttling) {
            out.emplace_back("throttling");
        }
        if (!capacity_reporting) {
            out.emplace_back("capacity-reporting");
        }
        if (!async_gsi) {
            out.emplace_back("gsi-consistency");
        }
        if (!aws_count_limits) {
            out.emplace_back("count-limits");
        }
        if (!aws_ttl_delay) {
            out.emplace_back("ttl-delay");
        }
        return out;
    }

    /// Sets one switch by name and value. Returns false (and changes
    /// nothing) for an unknown switch or a value it does not take.
    [[nodiscard]] auto set(std::string_view name, std::string_view value) -> bool {
        const auto n = lower(name);
        const auto v = lower(value);
        const auto pick = [&](bool& field, std::string_view aws, std::string_view relaxed) {
            if (v == aws) {
                field = true;
                return true;
            }
            if (v == relaxed) {
                field = false;
                return true;
            }
            return false;
        };
        if (n == "limits") {
            return pick(aws_limits, "aws", "relaxed");
        }
        if (n == "throttling") {
            return pick(throttling, "on", "off");
        }
        if (n == "capacity-reporting") {
            return pick(capacity_reporting, "on", "off");
        }
        if (n == "gsi-consistency") {
            return pick(async_gsi, "async", "sync");
        }
        if (n == "count-limits") {
            return pick(aws_count_limits, "aws", "relaxed");
        }
        if (n == "ttl-delay") {
            return pick(aws_ttl_delay, "aws", "prompt");
        }
        return false;
    }

private:
    [[nodiscard]] static auto lower(std::string_view s) -> std::string {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }
};

namespace detail {

[[nodiscard]] inline auto bad_switch(std::string_view name, std::string_view value)
    -> aws_json::error {
    return validation_error(std::string(k_invalid_parameters) + "Unknown Kythira compatibility " +
                            "setting " + std::string(name) + "=" + std::string(value));
}

}  // namespace detail

/// The cluster's profile from its config (`switch = value` pairs), starting
/// from `aws`.
[[nodiscard]] inline auto cluster_profile(
    const std::vector<std::pair<std::string, std::string>>& settings)
    -> aws_json::result<dynamo_compat_profile> {
    dynamo_compat_profile p;
    for (const auto& [name, value] : settings) {
        if (!p.set(name, value)) {
            return detail::bad_switch(name, value);
        }
    }
    return p;
}

/// Checks a table's tags for compatibility settings it cannot take, for
/// `CreateTable` and `TagResource`. Other tags are not looked at.
[[nodiscard]] inline auto check_compat_tags(
    const std::vector<std::pair<std::string, std::string>>& tags)
    -> std::optional<aws_json::error> {
    dynamo_compat_profile scratch;
    for (const auto& [key, value] : tags) {
        if (key.size() > k_compat_tag_prefix.size() && key.starts_with(k_compat_tag_prefix) &&
            !scratch.set(std::string_view(key).substr(k_compat_tag_prefix.size()), value)) {
            return detail::bad_switch(key, value);
        }
    }
    return std::nullopt;
}

/// The profile one table runs with: the cluster's, overlaid with the
/// table's `kythira:compat:*` tags. Tags `check_compat_tags` would refuse
/// cannot be on a table; any that are (from an older release) are skipped.
[[nodiscard]] inline auto resolve_profile(
    const dynamo_compat_profile& cluster,
    const std::vector<std::pair<std::string, std::string>>& table_tags) -> dynamo_compat_profile {
    auto p = cluster;
    for (const auto& [key, value] : table_tags) {
        if (key.starts_with(k_compat_tag_prefix)) {
            auto trial = p;
            if (trial.set(std::string_view(key).substr(k_compat_tag_prefix.size()), value)) {
                p = trial;
            }
        }
    }
    return p;
}

}  // namespace kythira::dynamodb
