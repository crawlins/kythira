// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file item_api.hpp
/// @brief `GetItem`, `PutItem`, `UpdateItem` and `DeleteItem`
///        (`.kiro/specs/dynamodb-compatible-api/` task 7, Requirements 8.1,
///        8.6).
///
/// Each handler does three things:
///
/// 1. Finds the table. A table that is missing, still being created or
///    being deleted gives `ResourceNotFoundException`, as on AWS.
/// 2. Compiles the request's expressions and legacy parameters into one
///    `expression_set` (`expr_request.hpp`) and validates the key or item
///    against the table's schema.
/// 3. Sends one `item_command` through the backend, or for `GetItem` a read.
///
/// The condition is evaluated in `apply()`, never here (Requirement 8.2).
/// A write first asks the disk guard, if one is set.
///
/// `ReturnConsumedCapacity` is answered with simple unit counts, which
/// task 12 replaces with AWS's exact formula:
///
/// - A read costs one unit per 4 KB of the item (at least one), halved
///   when eventually consistent.
/// - A put costs one unit per 1 KB of the new item. Updates and deletes
///   cost one unit.
///
/// A write to a table with local secondary indexes carries the indexes in
/// its command, so `apply()` updates their entries in the same batch, and
/// answers `ReturnItemCollectionMetrics=SIZE` with the collection's size
/// (task 9). DynamoDB Local never returns `ItemCollectionMetrics`; the
/// shape is AWS's documented one.

#include <kythira/dynamodb/api_common.hpp>
#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/disk_guard.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/expr_eval.hpp>
#include <kythira/dynamodb/expr_request.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/table_api.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kythira::dynamodb {

/// The single-item handlers over a backend.
template<dynamo_backend Backend> class item_api {
public:
    using result = aws_json::result<boost::json::object>;

    /// `guard` may be null (no disk floor).
    explicit item_api(Backend& backend, const disk_guard* guard = nullptr)
        : _backend(backend), _guard(guard) {}

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("GetItem",
              [this](call_context& c, boost::json::object& in) { return get_item(c, in); });
        fe.on("PutItem",
              [this](call_context& c, boost::json::object& in) { return put_item(c, in); });
        fe.on("UpdateItem",
              [this](call_context& c, boost::json::object& in) { return update_item(c, in); });
        fe.on("DeleteItem",
              [this](call_context& c, boost::json::object& in) { return delete_item(c, in); });
    }

    auto get_item(call_context& /*ctx*/, boost::json::object& in) -> result {
        auto table = usable_table(in);
        if (!table) {
            return table.error();
        }
        auto compiled = compile("GetItem", in, *table);
        if (!compiled) {
            return compiled.error();
        }
        auto key = parse_key(in, *table);
        if (!key) {
            return key.error();
        }
        const bool consistent = flag(in, "ConsistentRead");
        const auto found = _backend.read(table->table_id, table->schema, *key, consistent);
        boost::json::object out;
        if (found) {
            out["Item"] = item_to_json(
                compiled->projection ? project_item(*compiled->projection, *found) : *found);
        }
        if (const auto units = read_units(found, consistent); wants_capacity(in)) {
            out["ConsumedCapacity"] = capacity(in, table->name, units);
        }
        return out;
    }

    auto put_item(call_context& /*ctx*/, boost::json::object& in) -> result {
        auto table = usable_table(in);
        if (!table) {
            return table.error();
        }
        auto returns = return_values_of(in, /*update=*/false);
        if (!returns) {
            return returns.error();
        }
        auto compiled = compile("PutItem", in, *table);
        if (!compiled) {
            return compiled.error();
        }
        auto parsed = item_from_json(in.at("Item").as_object());
        if (!parsed) {
            return parsed.error();
        }
        if (auto e = validate_key_attributes(*parsed, table->schema)) {
            return *e;
        }
        if (auto e = check_item_size(*parsed)) {
            return *e;
        }
        if (auto e = validate_index_keys(*parsed, table->lsis)) {
            return *e;
        }
        if (auto e = validate_global_index_keys(*parsed, maintained_indexes(table->gsis))) {
            return *e;
        }
        item_command c = command(item_command::kind::put, *table, std::move(*parsed), in);
        c.expressions = std::move(*compiled);
        c.returns = *returns;
        const double units = write_units(c.payload);
        return finish(in, *table, c, units);
    }

    auto update_item(call_context& /*ctx*/, boost::json::object& in) -> result {
        auto table = usable_table(in);
        if (!table) {
            return table.error();
        }
        auto returns = return_values_of(in, /*update=*/true);
        if (!returns) {
            return returns.error();
        }
        auto compiled = compile("UpdateItem", in, *table);
        if (!compiled) {
            return compiled.error();
        }
        auto key = parse_key(in, *table);
        if (!key) {
            return key.error();
        }
        item_command c = command(item_command::kind::update, *table, std::move(*key), in);
        c.expressions = std::move(*compiled);
        c.returns = *returns;
        return finish(in, *table, c, 1.0);
    }

    auto delete_item(call_context& /*ctx*/, boost::json::object& in) -> result {
        auto table = usable_table(in);
        if (!table) {
            return table.error();
        }
        auto returns = return_values_of(in, /*update=*/false);
        if (!returns) {
            return returns.error();
        }
        auto compiled = compile("DeleteItem", in, *table);
        if (!compiled) {
            return compiled.error();
        }
        auto key = parse_key(in, *table);
        if (!key) {
            return key.error();
        }
        item_command c = command(item_command::kind::delete_, *table, std::move(*key), in);
        c.expressions = std::move(*compiled);
        c.returns = *returns;
        return finish(in, *table, c, 1.0);
    }

private:
    static auto flag(const boost::json::object& in, std::string_view member) -> bool {
        return api_detail::flag(in, member);
    }
    static auto text(const boost::json::object& in, std::string_view member) -> std::string_view {
        return api_detail::text(in, member);
    }

    auto usable_table(const boost::json::object& in) -> aws_json::result<table_descriptor> {
        return api_detail::usable_table(_backend, in.at("TableName").as_string());
    }

    static auto compile(std::string_view op, const boost::json::object& in,
                        const table_descriptor& table) -> aws_json::result<expression_set> {
        request_keys keys;
        keys.table = table.schema;
        return compile_request(op, in, keys);
    }

    static auto parse_key(const boost::json::object& in, const table_descriptor& table)
        -> aws_json::result<item> {
        auto key = item_from_json(in.at("Key").as_object());
        if (!key) {
            return key.error();
        }
        if (auto e = validate_key(*key, table.schema)) {
            return *e;
        }
        return std::move(*key);
    }

    static auto return_values_of(const boost::json::object& in, bool update)
        -> aws_json::result<return_values> {
        const auto rv = text(in, "ReturnValues");
        if (rv.empty() || rv == "NONE") {
            return return_values::none;
        }
        if (rv == "ALL_OLD") {
            return return_values::all_old;
        }
        if (update) {
            if (rv == "ALL_NEW") {
                return return_values::all_new;
            }
            if (rv == "UPDATED_OLD") {
                return return_values::updated_old;
            }
            if (rv == "UPDATED_NEW") {
                return return_values::updated_new;
            }
        }
        return validation_error("Return values set to invalid value");
    }

    static auto command(item_command::kind op, const table_descriptor& table, item payload,
                        const boost::json::object& in) -> item_command {
        item_command c;
        c.op = op;
        c.table_id = table.table_id;
        c.schema = table.schema;
        c.payload = std::move(payload);
        c.lsis = table.lsis;
        c.gsis = maintained_indexes(table.gsis);
        c.old_on_condition_failure = text(in, "ReturnValuesOnConditionCheckFailure") == "ALL_OLD";
        return c;
    }

    static auto wants_capacity(const boost::json::object& in) -> bool {
        return api_detail::wants_capacity(in);
    }

    static auto capacity(const boost::json::object& in, const std::string& table, double units)
        -> boost::json::object {
        return api_detail::capacity(in, table, units);
    }

    static auto read_units(const std::optional<item>& found, bool consistent) -> double {
        return api_detail::read_units(found ? item_size(*found) : 0, consistent);
    }

    static auto write_units(const item& it) -> double {
        return api_detail::write_units(item_size(it));
    }

    auto finish(const boost::json::object& in, const table_descriptor& table, const item_command& c,
                double units) -> result {
        if (_guard != nullptr) {
            if (auto e = _guard->check_write()) {
                return *e;
            }
        }
        const auto r = _backend.write(c);
        switch (r.outcome) {
            case item_result::status::condition_failed: {
                auto e = table_api_detail::modeled("ConditionalCheckFailedException",
                                                   "The conditional request failed");
                if (r.attributes) {
                    e.extra["Item"] = item_to_json(*r.attributes);
                }
                return e;
            }
            case item_result::status::error: {
                aws_json::error e;
                e.type = r.error_type;
                e.message = r.error_message;
                return e;
            }
            case item_result::status::ok:
                break;
        }
        boost::json::object out;
        if (r.attributes) {
            out["Attributes"] = item_to_json(*r.attributes);
        }
        if (wants_capacity(in)) {
            out["ConsumedCapacity"] = capacity(in, table.name, units);
        }
        if (auto m = api_detail::collection_metrics(in, table, c.payload, r)) {
            out["ItemCollectionMetrics"] = std::move(*m);
        }
        return out;
    }

    Backend& _backend;
    const disk_guard* _guard;
};

}  // namespace kythira::dynamodb
