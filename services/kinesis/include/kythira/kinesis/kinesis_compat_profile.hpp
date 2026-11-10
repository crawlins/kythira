// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_compat_profile.hpp
/// @brief The AWS-behaviour switches (`.kiro/specs/kinesis-compatible-streams/`
///        Requirement 3).
///
/// `kinesis_compat_profile` holds one switch per behaviour an operator may
/// relax. The default, profile `aws`, is AWS's behaviour for every switch,
/// in every deployment, standalone and test clusters included: throttling
/// is on unless an operator turns it off (3.5). A cluster sets switches in
/// its config; a stream overrides them with tags `kythira:compat:<switch>`,
/// and the stream's value wins (3.2). No switch changes an error code,
/// message or status (3.4).
///
///   switch              AWS value  other value
///   throttling          on         off       per-shard write/read rates, GetRecords calls/s
///   limits              aws        relaxed   record size, PutRecords batch, GetRecords page
///   count-limits        aws        relaxed   shard, stream and consumer counts
///   iterator-lifetime   aws        relaxed   the 5-minute shard-iterator expiry
///   id-format           aws        compact   sequence-number and shard-id forms
///
/// Names and values are matched case-insensitively. An unknown switch or
/// value is an error, so a typo in a tag fails at `CreateStream` or
/// `AddTagsToStream` instead of silently keeping AWS's behaviour.

#include <kythira/kinesis/kinesis_errors.hpp>
#include <kythira/kinesis/kinesis_sequence.hpp>

#include <raft/aws_json_protocol.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::kinesis {

inline constexpr std::string_view k_compat_tag_prefix = "kythira:compat:";

struct kinesis_compat_profile {
    bool _throttling = true;
    bool _aws_limits = true;
    bool _aws_count_limits = true;
    bool _aws_iterator_lifetime = true;
    sequence_profile _id_format = sequence_profile::aws;

    friend auto operator==(const kinesis_compat_profile&, const kinesis_compat_profile&)
        -> bool = default;

    /// The switches set away from AWS's behaviour, by name, for
    /// `DescribeStreamSummary`'s Kythira extension field and for metrics
    /// (3.3).
    [[nodiscard]] auto relaxed() const -> std::vector<std::string_view> {
        std::vector<std::string_view> out;
        if (!_throttling) {
            out.emplace_back("throttling");
        }
        if (!_aws_limits) {
            out.emplace_back("limits");
        }
        if (!_aws_count_limits) {
            out.emplace_back("count-limits");
        }
        if (!_aws_iterator_lifetime) {
            out.emplace_back("iterator-lifetime");
        }
        if (_id_format != sequence_profile::aws) {
            out.emplace_back("id-format");
        }
        return out;
    }

    /// Sets one switch by name and value. Returns false (and changes
    /// nothing) for an unknown switch or a value it does not take.
    [[nodiscard]] auto set(std::string_view name, std::string_view value) -> bool {
        const auto n = lower(name);
        const auto v = lower(value);
        const auto pick = [&v](bool& field, std::string_view aws, std::string_view other) {
            if (v == aws) {
                field = true;
                return true;
            }
            if (v == other) {
                field = false;
                return true;
            }
            return false;
        };
        if (n == "throttling") {
            return pick(_throttling, "on", "off");
        }
        if (n == "limits") {
            return pick(_aws_limits, "aws", "relaxed");
        }
        if (n == "count-limits") {
            return pick(_aws_count_limits, "aws", "relaxed");
        }
        if (n == "iterator-lifetime") {
            return pick(_aws_iterator_lifetime, "aws", "relaxed");
        }
        if (n == "id-format") {
            bool aws = _id_format == sequence_profile::aws;
            if (!pick(aws, "aws", "compact")) {
                return false;
            }
            _id_format = aws ? sequence_profile::aws : sequence_profile::compact;
            return true;
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
    return invalid_argument("Unknown Kythira compatibility setting " + std::string(name) + "=" +
                            std::string(value));
}

[[nodiscard]] inline auto compat_switch_of(std::string_view tag_key)
    -> std::optional<std::string_view> {
    if (tag_key.size() > k_compat_tag_prefix.size() && tag_key.starts_with(k_compat_tag_prefix)) {
        return tag_key.substr(k_compat_tag_prefix.size());
    }
    return std::nullopt;
}

}  // namespace detail

/// The cluster's profile from its config (`switch = value` pairs), starting
/// from `aws`.
[[nodiscard]] inline auto cluster_profile(
    const std::vector<std::pair<std::string, std::string>>& settings)
    -> aws_json::result<kinesis_compat_profile> {
    kinesis_compat_profile p;
    for (const auto& [name, value] : settings) {
        if (!p.set(name, value)) {
            return detail::bad_switch(name, value);
        }
    }
    return p;
}

/// Checks a stream's tags for compatibility settings it cannot take, for
/// `CreateStream`, `AddTagsToStream` and `TagResource`. Other tags are not
/// looked at.
[[nodiscard]] inline auto check_compat_tags(
    const std::vector<std::pair<std::string, std::string>>& tags)
    -> std::optional<aws_json::error> {
    kinesis_compat_profile scratch;
    for (const auto& [key, value] : tags) {
        if (auto name = detail::compat_switch_of(key); name && !scratch.set(*name, value)) {
            return detail::bad_switch(key, value);
        }
    }
    return std::nullopt;
}

/// The profile one stream runs with: the cluster's, overlaid with the
/// stream's `kythira:compat:*` tags. Tags `check_compat_tags` would refuse
/// cannot be on a stream; any that are (from an older release) are skipped.
[[nodiscard]] inline auto resolve_profile(
    const kinesis_compat_profile& cluster,
    const std::vector<std::pair<std::string, std::string>>& stream_tags) -> kinesis_compat_profile {
    auto p = cluster;
    for (const auto& [key, value] : stream_tags) {
        if (auto name = detail::compat_switch_of(key)) {
            auto trial = p;
            if (trial.set(*name, value)) {
                p = trial;
            }
        }
    }
    return p;
}

}  // namespace kythira::kinesis
