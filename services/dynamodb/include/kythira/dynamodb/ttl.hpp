// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file ttl.hpp
/// @brief Time to live: `UpdateTimeToLive`, `DescribeTimeToLive` and the
///        sweeper that deletes expired items
///        (`.kiro/specs/dynamodb-compatible-api/` task 13, Requirements
///        15.1-15.3).
///
/// A table's TTL setting lives in its catalog descriptor (`time_to_live`).
/// `UpdateTimeToLive` refuses as DynamoDB does: enabling what is enabled,
/// disabling what is disabled, naming another attribute while enabled, and
/// (AWS's limit, which DynamoDB Local does not keep) a second change within
/// an hour.
///
/// An item expires when its TTL attribute is a number of seconds since the
/// epoch that has passed, and is no more than five years old: DynamoDB
/// ignores older values, and any value that is not a number. An expired item
/// stays readable until it is deleted, as on AWS (15.3).
///
/// `ttl_sweeper` deletes them. Each `pass()` walks every table with TTL
/// enabled from where the last pass stopped, reading at most `scan_items`
/// items and deleting at most `max_deletes` of them per table, so a pass is
/// bounded however large the table. Each delete is an ordinary item entry
/// with the condition `<attribute> = <the value the sweeper read>`, so an
/// item written again since it was read is kept (15.2), and its LSI and GSI
/// entries go with it like any delete's. TTL deletes consume no capacity,
/// as on AWS: the sweeper writes through the backend, below the throttler.
///
/// How soon an expired item goes is the `ttl-delay` switch (15.3,
/// Requirement 3): with `aws` (the default) a table is walked again from
/// its start at most every `aws_interval` (10 minutes), within AWS's "a few
/// days"; with `prompt` every pass walks it, so an item goes within about a
/// pass (a second, as the gateway runs it), and `UpdateTimeToLive` has no
/// hourly limit.
///
/// Deviation from the design: there is no TTL index (`x/<table id>/...`);
/// the sweeper pages through the table itself. Its cost per walk is the
/// table's size, and it needs nothing new in the item groups' state or
/// across splits.
///
/// The gateway runs `pass()` once a second on a worker thread: on every
/// node for `local_backend`, and on the catalog leader for `raft_backend`
/// (`leads_catalog()`), whose writes reach each item's group leader.

#include <kythira/dynamodb/api_common.hpp>
#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/compat_profile.hpp>
#include <kythira/dynamodb/decimal.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/expr_request.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

namespace ttl_detail {

/// AWS allows one `UpdateTimeToLive` change per table an hour.
inline constexpr std::uint64_t k_min_change_interval_ms = std::uint64_t{3600} * 1000;
/// DynamoDB ignores expiry times more than five years in the past.
inline constexpr std::uint64_t k_max_age_seconds = std::uint64_t{5} * 365 * 24 * 3600;

/// `ms` milliseconds since the epoch as a decimal number of seconds.
[[nodiscard]] inline auto seconds_of(std::uint64_t ms) -> dynamo_decimal {
    auto text = std::to_string(ms / 1000);
    const auto frac = ms % 1000;
    if (frac != 0) {
        auto f = std::to_string(1000 + frac).substr(1);
        text += "." + f;
    }
    // A u64 of seconds always fits 38 digits.
    return *dynamo_decimal::from_string(text);
}

}  // namespace ttl_detail

/// The value of `attribute` in `it` when it makes the item expired at
/// `now_ms`: a number of seconds that has passed and is at most five years
/// old. Otherwise nothing.
[[nodiscard]] inline auto expired_value(const item& it, std::string_view attribute,
                                        std::uint64_t now_ms) -> std::optional<dynamo_decimal> {
    const auto* v = find_attribute(it, attribute);
    if (v == nullptr || v->type != attribute_type::N) {
        return std::nullopt;
    }
    const auto now = ttl_detail::seconds_of(now_ms);
    const auto now_s = now_ms / 1000;
    const auto oldest = ttl_detail::seconds_of(
        now_s > ttl_detail::k_max_age_seconds ? (now_s - ttl_detail::k_max_age_seconds) * 1000 : 0);
    if (!(v->num < now) || v->num < oldest) {
        return std::nullopt;
    }
    return v->num;
}

/// How the sweeper paces itself.
struct ttl_sweeper_options {
    /// `ttl-delay=aws`: the shortest time between two walks of a table.
    std::chrono::milliseconds aws_interval{std::chrono::minutes{10}};
    /// Items read per table per pass.
    std::size_t scan_items = 1000;
    /// Items deleted per table per pass.
    std::size_t max_deletes = 100;
};

/// Deletes expired items, a bounded slice of each table per `pass()`.
/// Not thread-safe: one thread runs it.
template<dynamo_backend Backend> class ttl_sweeper {
public:
    explicit ttl_sweeper(Backend& backend, ttl_sweeper_options options = {})
        : _backend(backend), _options(options) {}

    void set_compat_profile(const dynamo_compat_profile& p) { _profile = p; }

    /// One pass over every table with TTL enabled. Returns the items
    /// deleted.
    auto pass() -> std::size_t {
        const auto now = _backend.now_ms();
        std::size_t deleted = 0;
        std::set<std::uint64_t> live;
        for (const auto& name : _backend.list_tables("", SIZE_MAX)) {
            const auto d = _backend.describe(name);
            if (!d || !d->ttl.enabled ||
                (d->status != table_status::active && d->status != table_status::updating)) {
                continue;
            }
            live.insert(d->table_id);
            deleted += sweep(*d, now);
        }
        // Forget tables deleted or with TTL turned off.
        std::erase_if(_tables, [&](const auto& e) { return !live.contains(e.first); });
        return deleted;
    }

private:
    struct table_state {
        /// The last base key read; empty at the start of a walk.
        std::string cursor;
        /// When the next walk may start.
        std::uint64_t due_ms = 0;
    };

    auto sweep(const table_descriptor& d, std::uint64_t now) -> std::size_t {
        auto& st = _tables[d.table_id];
        if (st.cursor.empty() && now < st.due_ms) {
            return 0;
        }
        auto range = table_range(d.table_id);
        if (!st.cursor.empty()) {
            range.start = st.cursor;
            range.start.push_back('\0');
        }
        std::vector<std::pair<item, dynamo_decimal>> expired;
        std::size_t read = 0;
        std::string last;
        _backend.read_range(range, kythira::scan_direction::forward, false,
                            [&](std::string_view key, const item& it) {
                                ++read;
                                last = key;
                                if (auto v = expired_value(it, d.ttl.attribute, now)) {
                                    expired.emplace_back(key_of(it, d.schema), *v);
                                }
                                return read < _options.scan_items &&
                                       expired.size() < _options.max_deletes;
                            });
        const bool walked = read < _options.scan_items && expired.size() < _options.max_deletes;
        if (walked) {
            st.cursor.clear();
            const bool prompt = !resolve_profile(_profile, d.tags).aws_ttl_delay;
            st.due_ms =
                prompt ? now : now + static_cast<std::uint64_t>(_options.aws_interval.count());
        } else {
            st.cursor = std::move(last);
        }
        std::size_t deleted = 0;
        for (const auto& [key, value] : expired) {
            auto c = delete_if_unchanged(d, key, value);
            if (!c) {
                continue;
            }
            if (_backend.write(*c).outcome == item_result::status::ok) {
                ++deleted;
            }
        }
        return deleted;
    }

    static auto key_of(const item& it, const key_schema& schema) -> item {
        item key;
        for (const auto& [name, v] : it) {
            if (name == schema.partition_name || (schema.sort_name && name == *schema.sort_name)) {
                key.emplace_back(name, v);
            }
        }
        return key;
    }

    /// A delete of `key` on the condition that the TTL attribute still
    /// holds `value`, compiled as a DeleteItem request would be.
    static auto delete_if_unchanged(const table_descriptor& d, const item& key,
                                    const dynamo_decimal& value) -> std::optional<item_command> {
        boost::json::object in{
            {"TableName", call_context::bare_name(d.name)},
            {"ConditionExpression", "#t = :v"},
            {"ExpressionAttributeNames", boost::json::object{{"#t", d.ttl.attribute}}},
            {"ExpressionAttributeValues",
             boost::json::object{{":v", boost::json::object{{"N", value.to_string()}}}}},
        };
        request_keys keys;
        keys.table = d.schema;
        auto compiled = compile_request("DeleteItem", in, keys);
        if (!compiled) {
            return std::nullopt;
        }
        item_command c;
        c.op = item_command::kind::delete_;
        c.table_id = d.table_id;
        c.schema = d.schema;
        c.payload = key;
        c.lsis = d.lsis;
        c.gsis = maintained_indexes(d.gsis);
        c.log_changes = d.logs_changes();
        c.stream_ms = d.stream_label();
        c.region = d.region;
        // Its change record carries the TTL service's identity (14.1).
        c.ttl_delete = c.log_changes;
        c.expressions = std::move(*compiled);
        return c;
    }

    Backend& _backend;
    ttl_sweeper_options _options;
    dynamo_compat_profile _profile{};
    std::map<std::uint64_t, table_state> _tables;
};

/// `UpdateTimeToLive` and `DescribeTimeToLive` over a backend.
template<dynamo_backend Backend> class ttl_api {
public:
    using result = aws_json::result<boost::json::object>;

    explicit ttl_api(Backend& backend) : _backend(backend) {}

    /// The cluster's compatibility switches; a table's tags override them.
    void set_compat_profile(const dynamo_compat_profile& p) { _profile = p; }

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("UpdateTimeToLive",
              [this](call_context& c, boost::json::object& in) { return update_ttl(c, in); });
        fe.on("DescribeTimeToLive",
              [this](call_context& c, boost::json::object& in) { return describe_ttl(c, in); });
    }

    auto describe_ttl(call_context& ctx, boost::json::object& in) -> result {
        const auto name = ctx.table_name(in.at("TableName").as_string());
        if (auto e = check_name(ctx, name)) {
            return *e;
        }
        const auto d = _backend.describe(name);
        if (!d) {
            return not_found(name);
        }
        boost::json::object desc{{"TimeToLiveStatus", d->ttl.enabled ? "ENABLED" : "DISABLED"}};
        if (d->ttl.enabled) {
            desc["AttributeName"] = d->ttl.attribute;
        }
        return boost::json::object{{"TimeToLiveDescription", std::move(desc)}};
    }

    auto update_ttl(call_context& ctx, boost::json::object& in) -> result {
        const auto name = ctx.table_name(in.at("TableName").as_string());
        if (auto e = check_name(ctx, name)) {
            return *e;
        }
        const auto& spec = in.at("TimeToLiveSpecification").as_object();
        catalog_command c;
        c.op = catalog_command::kind::set_ttl;
        c.table.name = name;
        c.table.ttl.enabled = spec.at("Enabled").as_bool();
        c.table.ttl.attribute = std::string(spec.at("AttributeName").as_string());
        c.now_ms = _backend.now_ms();
        // The hourly limit follows the table's ttl-delay switch.
        if (const auto d = _backend.describe(name)) {
            // AWS: "Time to Live (TTL) is not supported for MRSC global
            // tables" (task 27).
            if (d->strong && c.table.ttl.enabled) {
                return validation_error(
                    "Time to Live is not supported for multi-Region strongly consistent tables");
            }
            c.ttl_min_interval_ms = resolve_profile(_profile, d->tags).aws_ttl_delay
                                        ? ttl_detail::k_min_change_interval_ms
                                        : 0;
        }
        const auto r = _backend.catalog(c);
        if (!r.ok()) {
            return table_api_detail::error_of(r);
        }
        return boost::json::object{{"TimeToLiveSpecification",
                                    boost::json::object{{"Enabled", c.table.ttl.enabled},
                                                        {"AttributeName", c.table.ttl.attribute}}}};
    }

private:
    static auto check_name(const call_context& ctx, std::string_view name)
        -> std::optional<aws_json::error> {
        if (!table_api_detail::valid_catalog_name(ctx, name)) {
            return validation_error(
                "Invalid table/index name.  Table/index names must be between 3 and 255 "
                "characters long, and may contain only the characters a-z, A-Z, 0-9, '_', '-', "
                "and '.'");
        }
        return std::nullopt;
    }

    static auto not_found(std::string_view name) -> aws_json::error {
        return table_api_detail::modeled(
            "ResourceNotFoundException",
            "Requested resource not found: Table: " + std::string(name) + " not found");
    }

    Backend& _backend;
    dynamo_compat_profile _profile{};
};

}  // namespace kythira::dynamodb
