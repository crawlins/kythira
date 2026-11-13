// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief Contributor Insights calls (`.kiro/specs/dynamodb-compatible-api/`
/// task 21, Requirement 19.4): `UpdateContributorInsights`,
/// `DescribeContributorInsights`, `ListContributorInsights`, and the report
/// that stands in for CloudWatch's rule reports.
///
/// The setting lives in the table's catalog descriptor
/// (`table_descriptor::insights`) for the table and each GSI. AWS moves
/// through `ENABLING` and `DISABLING` while it creates or deletes the
/// CloudWatch rules; here there are no rules to create, so the update
/// answers `ENABLING` or `DISABLING` as AWS does and the setting reads
/// `ENABLED` or `DISABLED` from then on.
///
/// The rules are named as AWS names them,
/// `DynamoDBContributorInsights-<PKC|SKC|PKT|SKT>-<table>[-<index>]-<ms>`,
/// the subject cut so the name fits AWS's 128 characters; a table without
/// a sort key has no `SKC` or `SKT`, and `THROTTLED_KEYS` mode has only the
/// throttle rules.
///
/// The counters are `insights.hpp`'s, per gateway node. Their report is
/// `GET /kythira/contributor-insights` (`front_end.hpp`), with query
/// parameters `TableName` (required), `IndexName`, `Rule` (a code or a
/// rule name; all of the setting's rules when absent), `Minutes` (1 to 60,
/// default 60) and `MaxContributors` (1 to 100, default 10):
///
///   {"TableName": "Music", "ContributorInsightsStatus": "ENABLED",
///    "Minutes": 60,
///    "Rules": [{"RuleName": "DynamoDBContributorInsights-PKC-Music-...",
///               "Rule": "PKC",
///               "Contributors": [{"Key": {"Artist": {"S": "Acme"}},
///                                 "Value": 12.5}]}]}
///
/// `Value` is consumed capacity units for `PKC` and `SKC`, and throttled
/// requests for `PKT` and `SKT`.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/insights.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/table_api.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

namespace insights_detail {

/// AWS's longest rule name.
inline constexpr std::size_t k_max_rule_name = 128;
inline constexpr std::size_t k_max_list_results = 100;
inline constexpr std::uint64_t k_max_report_minutes = 60;
inline constexpr std::size_t k_max_contributors = 100;

[[nodiscard]] inline auto mode_name(insights_mode m) -> std::string_view {
    return m == insights_mode::throttled ? "THROTTLED_KEYS" : "ACCESSED_AND_THROTTLED_KEYS";
}

/// Whether the table (`index` empty) or the GSI has a sort key.
[[nodiscard]] inline auto has_sort_key(const table_descriptor& d, std::string_view index) -> bool {
    if (index.empty()) {
        return d.schema.sort_name.has_value();
    }
    const auto it = std::find_if(d.gsis.begin(), d.gsis.end(),
                                 [&](const global_index& g) { return g.name == index; });
    return it != d.gsis.end() && it->schema.sort_name.has_value();
}

/// The GSI named `index`, unless it is being deleted.
[[nodiscard]] inline auto live_gsi(const table_descriptor& d, std::string_view index)
    -> const global_index* {
    const auto it = std::find_if(d.gsis.begin(), d.gsis.end(), [&](const global_index& g) {
        return g.name == index && g.status != index_status::deleting;
    });
    return it == d.gsis.end() ? nullptr : &*it;
}

/// The rules an enabled setting has, with their names.
[[nodiscard]] inline auto rules_of(const table_descriptor& d, const contributor_insights& s)
    -> std::vector<std::pair<insights_rule, std::string>> {
    std::vector<insights_rule> rules;
    const bool sorted = has_sort_key(d, s.index);
    if (s.mode == insights_mode::accessed_and_throttled) {
        rules.push_back(insights_rule::pkc);
        if (sorted) {
            rules.push_back(insights_rule::skc);
        }
    }
    rules.push_back(insights_rule::pkt);
    if (sorted) {
        rules.push_back(insights_rule::skt);
    }
    std::string subject = d.name;
    if (!s.index.empty()) {
        subject += "-" + s.index;
    }
    const std::string stamp = "-" + std::to_string(s.updated_ms);
    constexpr std::string_view prefix = "DynamoDBContributorInsights-";
    // prefix + code + '-' + subject + stamp
    const std::size_t room = k_max_rule_name - prefix.size() - 4 - stamp.size();
    if (subject.size() > room) {
        subject.resize(room);
    }
    std::vector<std::pair<insights_rule, std::string>> out;
    out.reserve(rules.size());
    for (const auto r : rules) {
        out.emplace_back(r,
                         std::string(prefix) + std::string(rule_code(r)) + "-" + subject + stamp);
    }
    return out;
}

[[nodiscard]] inline auto status_of(const contributor_insights* s) -> std::string_view {
    return s != nullptr && s->enabled ? "ENABLED" : "DISABLED";
}

[[nodiscard]] inline auto table_not_found(std::string_view name) -> aws_json::error {
    return table_api_detail::modeled(
        "ResourceNotFoundException",
        "Requested resource not found: Table: " + std::string(name) + " not found");
}

[[nodiscard]] inline auto index_not_found(std::string_view name) -> aws_json::error {
    return table_api_detail::modeled(
        "ResourceNotFoundException",
        "Requested resource not found: Index: " + std::string(name) + " not found");
}

/// A whole number from a query parameter within [lo, hi], or nothing.
[[nodiscard]] inline auto bounded(std::string_view text, std::uint64_t lo, std::uint64_t hi)
    -> std::optional<std::uint64_t> {
    std::uint64_t v = 0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, v);
    if (ec != std::errc{} || ptr != end || v < lo || v > hi) {
        return std::nullopt;
    }
    return v;
}

}  // namespace insights_detail

template<dynamo_backend Backend> class insights_api {
public:
    using result = aws_json::result<boost::json::object>;
    using params = std::map<std::string, std::string, std::less<>>;

    explicit insights_api(Backend& backend) : _backend(backend) {}

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("UpdateContributorInsights",
              [this](call_context& c, boost::json::object& in) { return update(c, in); });
        fe.on("DescribeContributorInsights",
              [this](call_context& c, boost::json::object& in) { return describe(c, in); });
        fe.on("ListContributorInsights",
              [this](call_context& c, boost::json::object& in) { return list(c, in); });
        fe.set_report_handler([this](call_context& c, const params& p) { return report(c, p); });
    }

    auto update(call_context& ctx, boost::json::object& in) -> result {
        namespace id = insights_detail;
        const auto name = ctx.table_name(in.at("TableName").as_string());
        const auto d = _backend.describe(name);
        if (!d || d->status == table_status::deleting) {
            return id::table_not_found(name);
        }
        contributor_insights want;
        if (const auto* index = in.if_contains("IndexName")) {
            want.index = std::string(index->as_string());
            if (id::live_gsi(*d, want.index) == nullptr) {
                return id::index_not_found(want.index);
            }
        }
        want.enabled = in.at("ContributorInsightsAction").as_string() == "ENABLE";
        if (const auto* mode = in.if_contains("ContributorInsightsMode")) {
            want.mode = mode->as_string() == "THROTTLED_KEYS"
                            ? insights_mode::throttled
                            : insights_mode::accessed_and_throttled;
        } else if (const auto* now = d->insights_of(want.index); now != nullptr && !want.enabled) {
            want.mode = now->mode;
        }
        catalog_command c;
        c.op = catalog_command::kind::set_contributor_insights;
        c.table.name = d->name;
        c.table.insights = {want};
        c.now_ms = _backend.now_ms();
        const auto r = _backend.catalog(c);
        if (!r.ok()) {
            return table_api_detail::error_of(r);
        }
        boost::json::object out;
        out["TableName"] = call_context::bare_name(d->name);
        if (!want.index.empty()) {
            out["IndexName"] = want.index;
        }
        out["ContributorInsightsStatus"] = want.enabled ? "ENABLING" : "DISABLING";
        out["ContributorInsightsMode"] = id::mode_name(want.mode);
        return out;
    }

    auto describe(call_context& ctx, boost::json::object& in) -> result {
        namespace id = insights_detail;
        const auto name = ctx.table_name(in.at("TableName").as_string());
        const auto d = _backend.describe(name);
        if (!d || d->status == table_status::deleting) {
            return id::table_not_found(name);
        }
        std::string index;
        if (const auto* v = in.if_contains("IndexName")) {
            index = std::string(v->as_string());
            if (id::live_gsi(*d, index) == nullptr) {
                return id::index_not_found(index);
            }
        }
        const auto* s = d->insights_of(index);
        boost::json::object out;
        out["TableName"] = call_context::bare_name(d->name);
        if (!index.empty()) {
            out["IndexName"] = index;
        }
        out["ContributorInsightsStatus"] = id::status_of(s);
        if (s != nullptr) {
            if (s->enabled) {
                boost::json::array rules;
                for (const auto& [rule, rule_name] : id::rules_of(*d, *s)) {
                    rules.emplace_back(rule_name);
                }
                out["ContributorInsightsRuleList"] = std::move(rules);
            }
            out["LastUpdateDateTime"] = table_api_detail::millis_to_seconds(s->updated_ms);
            out["ContributorInsightsMode"] = id::mode_name(s->mode);
        }
        return out;
    }

    /// With `TableName`: the table and each of its GSIs, set or not. Without:
    /// every table and GSI whose setting was ever changed. Ordered by table
    /// then index (the table first); `NextToken` is the last one answered.
    auto list(call_context& ctx, boost::json::object& in) -> result {
        namespace id = insights_detail;
        std::size_t limit = id::k_max_list_results;
        if (const auto* m = in.if_contains("MaxResults"); m != nullptr && m->is_number()) {
            const auto v = m->to_number<std::int64_t>();
            if (v < 1) {
                return validation_error(
                    "1 validation error detected: Value '" + std::to_string(v) +
                    "' at 'maxResults' failed to satisfy constraint: Member must have value "
                    "greater than or equal to 1");
            }
            limit = std::min<std::size_t>(static_cast<std::size_t>(v), id::k_max_list_results);
        }
        // After (table, index), exclusive.
        std::optional<std::pair<std::string, std::string>> after;
        if (const auto* t = in.if_contains("NextToken"); t != nullptr && t->is_string()) {
            const std::string_view token(t->as_string());
            const auto slash = token.find('/');
            if (slash == std::string_view::npos || slash == 0) {
                return validation_error("Invalid NextToken");
            }
            after = std::pair{ctx.table_name(token.substr(0, slash)),
                              std::string(token.substr(slash + 1))};
        }
        const auto is_after = [&](const std::string& table, const std::string& index) {
            return !after || std::pair{table, index} > *after;
        };

        boost::json::array summaries;
        std::optional<std::pair<std::string, std::string>> last;
        bool more = false;
        const auto add = [&](const table_descriptor& d, const std::string& index) {
            if (!ctx.owns(d.name) || !is_after(d.name, index)) {
                return true;
            }
            if (summaries.size() == limit) {
                more = true;
                return false;
            }
            const auto* s = d.insights_of(index);
            boost::json::object o;
            o["TableName"] = call_context::bare_name(d.name);
            if (!index.empty()) {
                o["IndexName"] = index;
            }
            o["ContributorInsightsStatus"] = id::status_of(s);
            if (s != nullptr) {
                o["ContributorInsightsMode"] = id::mode_name(s->mode);
            }
            summaries.push_back(std::move(o));
            last = std::pair{d.name, index};
            return true;
        };
        // A table's entries, in order; false once the page is full.
        const auto add_table = [&](const table_descriptor& d, bool all) {
            std::vector<std::string> indexes;
            if (all || d.insights_of("") != nullptr) {
                indexes.emplace_back();
            }
            for (const auto& g : d.gsis) {
                if (g.status != index_status::deleting &&
                    (all || d.insights_of(g.name) != nullptr)) {
                    indexes.push_back(g.name);
                }
            }
            std::sort(indexes.begin(), indexes.end());
            return std::all_of(indexes.begin(), indexes.end(),
                               [&](const std::string& index) { return add(d, index); });
        };

        if (const auto* t = in.if_contains("TableName")) {
            const auto name = ctx.table_name(t->as_string());
            const auto d = _backend.describe(name);
            if (!d || d->status == table_status::deleting) {
                return id::table_not_found(call_context::bare_name(name));
            }
            (void)add_table(*d, true);
        } else {
            std::string start = after ? after->first : std::string{};
            // The table named by the token may still have entries to give.
            bool first = after.has_value();
            bool open = true;
            while (open) {
                std::vector<std::string> names;
                if (first) {
                    names.push_back(start);
                    first = false;
                } else {
                    names = _backend.list_tables(start, 100);
                    if (names.empty()) {
                        break;
                    }
                    start = names.back();
                }
                for (const auto& name : names) {
                    const auto d = _backend.describe(name);
                    if (d && d->status != table_status::deleting && !add_table(*d, false)) {
                        open = false;
                        break;
                    }
                }
            }
        }
        boost::json::object out;
        out["ContributorInsightsSummaries"] = std::move(summaries);
        if (more && last) {
            out["NextToken"] = call_context::bare_name(last->first) + "/" + last->second;
        }
        return out;
    }

    /// The report (`front_end.hpp`'s extension path): see the file comment.
    auto report(call_context& ctx, const params& p) -> result {
        namespace id = insights_detail;
        const auto name = ctx.table_name(p.at("TableName"));
        const auto d = _backend.describe(name);
        if (!d || d->status == table_status::deleting) {
            return id::table_not_found(name);
        }
        std::string index;
        if (const auto it = p.find("IndexName"); it != p.end()) {
            index = it->second;
            if (id::live_gsi(*d, index) == nullptr) {
                return id::index_not_found(index);
            }
        }
        std::uint64_t minutes = id::k_max_report_minutes;
        if (const auto it = p.find("Minutes"); it != p.end()) {
            const auto v = id::bounded(it->second, 1, id::k_max_report_minutes);
            if (!v) {
                return validation_error("Minutes must be a whole number from 1 to 60");
            }
            minutes = *v;
        }
        std::size_t count = 10;
        if (const auto it = p.find("MaxContributors"); it != p.end()) {
            const auto v = id::bounded(it->second, 1, id::k_max_contributors);
            if (!v) {
                return validation_error("MaxContributors must be a whole number from 1 to 100");
            }
            count = static_cast<std::size_t>(*v);
        }
        const auto* s = d->insights_of(index);
        boost::json::object out;
        out["TableName"] = call_context::bare_name(d->name);
        if (!index.empty()) {
            out["IndexName"] = index;
        }
        out["ContributorInsightsStatus"] = id::status_of(s);
        out["Minutes"] = minutes;
        boost::json::array rules;
        const auto wanted = p.find("Rule");
        bool matched = wanted == p.end();
        if (s != nullptr && s->enabled) {
            const auto now = _backend.now_ms();
            for (const auto& [rule, rule_name] : id::rules_of(*d, *s)) {
                if (wanted != p.end() && wanted->second != rule_code(rule) &&
                    wanted->second != rule_name) {
                    continue;
                }
                matched = true;
                boost::json::array contributors;
                for (const auto& c :
                     _backend.throttler().insights().top(*d, *s, rule, now, minutes, count)) {
                    const auto key = decode_item(c.key);
                    if (!key) {
                        continue;
                    }
                    contributors.push_back(
                        boost::json::object{{"Key", item_to_json(*key)}, {"Value", c.value}});
                }
                rules.push_back(boost::json::object{{"RuleName", rule_name},
                                                    {"Rule", rule_code(rule)},
                                                    {"Contributors", std::move(contributors)}});
            }
        }
        if (!matched && s != nullptr && s->enabled) {
            return validation_error("No such Contributor Insights rule: " + wanted->second);
        }
        out["Rules"] = std::move(rules);
        return out;
    }

private:
    Backend& _backend;
};

}  // namespace kythira::dynamodb
