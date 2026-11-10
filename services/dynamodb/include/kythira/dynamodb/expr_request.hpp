// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file expr_request.hpp
/// @brief One request's expressions and legacy parameters, compiled in
///        DynamoDB's order (`.kiro/specs/dynamodb-compatible-api/` task 5,
///        Requirements 8.3-8.5).
///
/// `compile_request` takes an operation's parsed input and returns the
/// `expression_set` its Raft entry carries, or the first error DynamoDB
/// would answer. It owns the rules between parameters:
///
///   1. expression and legacy parameters may not be mixed (a Query may
///      still pair `KeyConditions` with `FilterExpression`);
///   2. placeholder maps need an expression to serve;
///   3. legacy parameters (`Expected`, `AttributeUpdates`, `KeyConditions`,
///      `QueryFilter`, `ScanFilter`, `AttributesToGet`,
///      `ConditionalOperator`) are checked and translated into the same
///      trees expressions compile to, so `apply()` has one evaluator;
///   4. expressions compile, in the order DynamoDB Local reports their
///      faults: Update then Condition; Filter, Projection, then Key
///      condition;
///   5. unused placeholders; then the key schema: key conditions, keys in
///      a Query filter, updates to key attributes.
///
/// One documented divergence from DynamoDB Local 3.3.2: an
/// `AttributeUpdates` entry without `Action` is a `PUT`, as AWS documents
/// it; DynamoDB Local silently drops the whole update.

#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/expr.hpp>
#include <kythira/dynamodb/expr_parser.hpp>
#include <kythira/dynamodb/item_codec.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

namespace request_detail {

[[nodiscard]] inline auto member(const boost::json::object& o, std::string_view key)
    -> const boost::json::value* {
    const auto* v = o.if_contains(key);
    return v == nullptr || v->is_null() ? nullptr : v;
}

[[nodiscard]] inline auto string_member(const boost::json::object& o, std::string_view key)
    -> std::optional<std::string_view> {
    const auto* v = member(o, key);
    if (v == nullptr || !v->is_string()) {
        return std::nullopt;
    }
    return std::string_view(v->get_string());
}

[[nodiscard]] inline auto object_member(const boost::json::object& o, std::string_view key)
    -> const boost::json::object* {
    const auto* v = member(o, key);
    return v != nullptr && v->is_object() ? &v->get_object() : nullptr;
}

[[nodiscard]] inline auto invalid(std::string_view what) -> aws_json::error {
    return validation_error(std::string(k_invalid_parameters) + std::string(what));
}

// The legacy comparison operators and the operand each takes.
struct legacy_operator {
    std::string_view name;
    int min_args;
    int max_args;  // -1: no upper bound (IN)
};

inline constexpr legacy_operator k_legacy_operators[] = {
    {"EQ", 1, 1},       {"NE", 1, 1},           {"LE", 1, 1},          {"LT", 1, 1},
    {"GE", 1, 1},       {"GT", 1, 1},           {"NOT_NULL", 0, 0},    {"NULL", 0, 0},
    {"CONTAINS", 1, 1}, {"NOT_CONTAINS", 1, 1}, {"BEGINS_WITH", 1, 1}, {"IN", 1, -1},
    {"BETWEEN", 2, 2},
};

[[nodiscard]] inline auto find_operator(std::string_view name) -> const legacy_operator* {
    for (const auto& op : k_legacy_operators) {
        if (op.name == name) {
            return &op;
        }
    }
    return nullptr;
}

[[nodiscard]] inline auto type_allowed(std::string_view op, attribute_type t) -> bool {
    const bool scalar = t == attribute_type::S || t == attribute_type::N || t == attribute_type::B;
    if (op == "EQ" || op == "NE") {
        return true;
    }
    if (op == "BEGINS_WITH") {
        return t == attribute_type::S || t == attribute_type::B;
    }
    if (op == "CONTAINS" || op == "NOT_CONTAINS") {
        return scalar || t == attribute_type::BOOL || t == attribute_type::NULL_;
    }
    return scalar;
}

// The tree of one legacy condition on attribute `name`.
[[nodiscard]] inline auto legacy_tree(const std::string& name, std::string_view op,
                                      std::vector<attribute_value> values) -> expr_node {
    const auto path = [&] { return expr_node::of_path({path_step{name, -1}}); };
    const auto val = [&](std::size_t i) { return expr_node::of_value(values[i]); };
    const auto binary = [&](expr_op o) { return expr_node::of(o, {path(), val(0)}); };
    if (op == "EQ") {
        return binary(expr_op::eq);
    }
    if (op == "NE") {
        return binary(expr_op::ne);
    }
    if (op == "LT") {
        return binary(expr_op::lt);
    }
    if (op == "LE") {
        return binary(expr_op::le);
    }
    if (op == "GT") {
        return binary(expr_op::gt);
    }
    if (op == "GE") {
        return binary(expr_op::ge);
    }
    if (op == "NULL") {
        return expr_node::of(expr_op::attribute_not_exists, {path()});
    }
    if (op == "NOT_NULL") {
        return expr_node::of(expr_op::attribute_exists, {path()});
    }
    if (op == "CONTAINS") {
        return binary(expr_op::contains);
    }
    if (op == "NOT_CONTAINS") {
        // A missing attribute does not satisfy NOT_CONTAINS.
        return expr_node::of(expr_op::and_,
                             {expr_node::of(expr_op::attribute_exists, {path()}),
                              expr_node::of(expr_op::not_, {binary(expr_op::contains)})});
    }
    if (op == "BEGINS_WITH") {
        return binary(expr_op::begins_with);
    }
    if (op == "BETWEEN") {
        return expr_node::of(expr_op::between, {path(), val(0), val(1)});
    }
    std::vector<expr_node> args{path()};
    for (std::size_t i = 0; i < values.size(); ++i) {
        args.push_back(val(i));
    }
    return expr_node::of(expr_op::in, std::move(args));
}

// Count, types, then BETWEEN's bounds, for one legacy condition.
[[nodiscard]] inline auto check_operands(std::string_view op_name,
                                         const std::vector<attribute_value>& values)
    -> std::optional<aws_json::error> {
    const auto* op = find_operator(op_name);
    const auto n = static_cast<int>(values.size());
    if (n < op->min_args || (op->max_args >= 0 && n > op->max_args)) {
        return invalid("Invalid number of argument(s) for the " + std::string(op_name) +
                       " ComparisonOperator");
    }
    for (const auto& v : values) {
        if (!type_allowed(op_name, v.type)) {
            return invalid("ComparisonOperator " + std::string(op_name) + " is not valid for " +
                           std::string(type_name(v.type)) + " AttributeValue type");
        }
    }
    if (op_name == "BETWEEN" || op_name == "IN") {
        for (const auto& v : values) {
            if (v.type != values.front().type) {
                return invalid("AttributeValues inside AttributeValueList must be of same type");
            }
        }
    }
    if (op_name == "BETWEEN" && expr_detail::compare_scalars(values[0], values[1]) > 0) {
        return validation_error(
            "The BETWEEN condition was provided a range where the lower bound is greater than "
            "the upper bound");
    }
    return std::nullopt;
}

[[nodiscard]] inline auto value_list(const boost::json::object& entry)
    -> aws_json::result<std::vector<attribute_value>> {
    std::vector<attribute_value> out;
    if (const auto* list = member(entry, "AttributeValueList");
        list != nullptr && list->is_array()) {
        for (const auto& v : list->get_array()) {
            auto a = attribute_from_json(v);
            if (!a) {
                return a.error();
            }
            out.push_back(std::move(*a));
        }
    }
    return out;
}

// One `Condition` (KeyConditions, QueryFilter, ScanFilter).
[[nodiscard]] inline auto condition_entry(const std::string& name, const boost::json::value& v)
    -> aws_json::result<expr_node> {
    if (name.empty()) {
        return validation_error("Empty attribute name");
    }
    const auto& entry = v.is_object() ? v.get_object() : boost::json::object{};
    const auto op = string_member(entry, "ComparisonOperator");
    if (!op || find_operator(*op) == nullptr) {
        return validation_error("Comparison type does not exist in DynamoDB");
    }
    auto values = value_list(entry);
    if (!values) {
        return values.error();
    }
    if (auto e = check_operands(*op, *values)) {
        return *e;
    }
    return legacy_tree(name, *op, std::move(*values));
}

// One `ExpectedAttributeValue`.
[[nodiscard]] inline auto expected_entry(const std::string& name, const boost::json::value& v)
    -> aws_json::result<expr_node> {
    if (name.empty()) {
        return validation_error("Empty attribute name");
    }
    const auto& entry = v.is_object() ? v.get_object() : boost::json::object{};
    const auto* value = member(entry, "Value");
    const auto* exists = member(entry, "Exists");
    const auto* list = member(entry, "AttributeValueList");
    const auto op = string_member(entry, "ComparisonOperator");
    const std::string for_attr = " for Attribute: " + name;
    if (exists != nullptr && op) {
        return invalid("Exists and ComparisonOperator cannot be used together" + for_attr);
    }
    if (value != nullptr && list != nullptr) {
        return invalid("Value and AttributeValueList cannot be used together" + for_attr);
    }
    if (list != nullptr && !op) {
        return invalid("AttributeValueList can only be used with a ComparisonOperator" + for_attr);
    }
    if (op) {
        if (find_operator(*op) == nullptr) {
            return validation_error("Comparison type does not exist in DynamoDB");
        }
        std::vector<attribute_value> values;
        if (value != nullptr) {
            auto a = attribute_from_json(*value);
            if (!a) {
                return a.error();
            }
            values.push_back(std::move(*a));
        } else {
            auto l = value_list(entry);
            if (!l) {
                return l.error();
            }
            values = std::move(*l);
        }
        if (values.empty() && find_operator(*op)->min_args > 0 && list == nullptr) {
            return invalid("Value or AttributeValueList must be used with ComparisonOperator: " +
                           std::string(*op) + for_attr);
        }
        if (auto e = check_operands(*op, values)) {
            return *e;
        }
        return legacy_tree(name, *op, std::move(values));
    }
    if (exists != nullptr && exists->is_bool() && !exists->get_bool()) {
        if (value != nullptr) {
            return invalid("Value cannot be used when Exists is false" + for_attr);
        }
        return expr_node::of(expr_op::attribute_not_exists,
                             {expr_node::of_path({path_step{name, -1}})});
    }
    if (value == nullptr) {
        return invalid(std::string("Value must be provided when Exists is ") +
                       (exists == nullptr ? "null" : "true") + for_attr);
    }
    auto a = attribute_from_json(*value);
    if (!a) {
        return a.error();
    }
    return legacy_tree(name, "EQ", {std::move(*a)});
}

// Joins entries with ConditionalOperator (AND by default), left to right.
[[nodiscard]] inline auto join(std::vector<expr_node> parts, bool use_or) -> expr_node {
    expr_node out = std::move(parts.front());
    for (std::size_t i = 1; i < parts.size(); ++i) {
        out = expr_node::of(use_or ? expr_op::or_ : expr_op::and_,
                            {std::move(out), std::move(parts[i])});
    }
    return out;
}

template<typename Entry>
[[nodiscard]] inline auto legacy_conditions(const boost::json::object& map, bool use_or,
                                            Entry entry) -> aws_json::result<expr_node> {
    std::vector<expr_node> parts;
    for (const auto& kv : map) {
        auto n = entry(std::string(kv.key()), kv.value());
        if (!n) {
            return n.error();
        }
        parts.push_back(std::move(*n));
    }
    return join(std::move(parts), use_or);
}

[[nodiscard]] inline auto update_type(attribute_type t) -> std::string_view {
    return expr_detail::update_type_name(t);
}

[[nodiscard]] inline auto attribute_updates(const boost::json::object& map)
    -> aws_json::result<update_expr> {
    update_expr out;
    for (const auto& kv : map) {
        const std::string name(kv.key());
        if (name.empty()) {
            return validation_error("Empty attribute name");
        }
        const auto& entry =
            kv.value().is_object() ? kv.value().get_object() : boost::json::object{};
        const auto action = string_member(entry, "Action").value_or("PUT");
        if (action != "PUT" && action != "DELETE" && action != "ADD") {
            return validation_error("Member must satisfy enum value set: [ADD, DELETE, PUT]");
        }
        const auto* value = member(entry, "Value");
        if (value == nullptr && action != "DELETE") {
            return invalid("Only DELETE action is allowed when no attribute value is specified");
        }
        update_action a;
        a.path = {path_step{name, -1}};
        if (value == nullptr) {
            a.kind = update_kind::remove;
            out.actions.push_back(std::move(a));
            continue;
        }
        auto v = attribute_from_json(*value);
        if (!v) {
            return v.error();
        }
        if (action == "DELETE" && !expr_detail::is_set(v->type)) {
            return invalid("DELETE action with value is not supported for the type " +
                           std::string(update_type(v->type)));
        }
        if (action == "ADD" && v->type != attribute_type::N && v->type != attribute_type::L &&
            !expr_detail::is_set(v->type)) {
            return invalid("ADD action is not supported for the type " +
                           std::string(update_type(v->type)));
        }
        a.kind = action == "PUT"      ? update_kind::set
                 : action == "DELETE" ? update_kind::legacy_delete
                                      : update_kind::legacy_add;
        a.value = expr_node::of_value(std::move(*v));
        out.actions.push_back(std::move(a));
    }
    return out;
}

// "Can not use both expression and non-expression parameters ...".
[[nodiscard]] inline auto check_mixing(const boost::json::object& in)
    -> std::optional<aws_json::error> {
    static constexpr std::string_view k_legacy[] = {"AttributesToGet", "AttributeUpdates",
                                                    "Expected",        "QueryFilter",
                                                    "ScanFilter",      "ConditionalOperator"};
    static constexpr std::string_view k_expressions[] = {"ProjectionExpression", "UpdateExpression",
                                                         "FilterExpression", "ConditionExpression",
                                                         "KeyConditionExpression"};
    std::vector<std::string_view> legacy;
    for (const auto k : k_legacy) {
        if (member(in, k) != nullptr) {
            legacy.push_back(k);
        }
    }
    const bool key_expression = member(in, "KeyConditionExpression") != nullptr;
    // KeyConditions conflicts only with KeyConditionExpression.
    if (key_expression && member(in, "KeyConditions") != nullptr) {
        legacy.push_back("KeyConditions");
    }
    std::vector<std::string_view> expressions;
    for (const auto k : k_expressions) {
        if (member(in, k) != nullptr) {
            expressions.push_back(k);
        }
    }
    if (legacy.empty() || expressions.empty()) {
        return std::nullopt;
    }
    // KeyConditions with a filter or projection expression is allowed.
    if (legacy.size() == 1 && legacy.front() == "KeyConditions" && !key_expression) {
        return std::nullopt;
    }
    const auto list = [](const std::vector<std::string_view>& v) {
        std::string s = "{";
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i != 0) {
                s += ", ";
            }
            s += v[i];
        }
        return s + "}";
    };
    return validation_error(
        "Can not use both expression and non-expression parameters in the same request: "
        "Non-expression parameters: " +
        list(legacy) + " Expression parameters: " + list(expressions));
}

}  // namespace request_detail

/// The keys a request runs against: the table's, and for a Query or Scan
/// on an index, the index's.
struct request_keys {
    key_schema table;
    std::optional<key_schema> index;

    [[nodiscard]] auto queried() const -> const key_schema& { return index ? *index : table; }
};

/// Compiles `input` (an operation's parsed request) for `operation`
/// (`GetItem`, `PutItem`, `DeleteItem`, `UpdateItem`, `Query` or `Scan`).
[[nodiscard]] inline auto compile_request(std::string_view operation,
                                          const boost::json::object& input,
                                          const request_keys& keys)
    -> aws_json::result<expression_set> {
    using namespace request_detail;
    if (auto e = check_mixing(input)) {
        return *e;
    }
    const bool is_update = operation == "UpdateItem";
    const bool is_query = operation == "Query";
    const bool is_scan = operation == "Scan";
    const bool is_get = operation == "GetItem";

    // The expressions this operation takes, in the order its "are null"
    // message lists them.
    std::vector<std::string_view> value_users;
    if (is_update) {
        value_users = {"UpdateExpression", "ConditionExpression"};
    } else if (is_query) {
        value_users = {"FilterExpression", "KeyConditionExpression"};
    } else if (is_scan) {
        value_users = {"FilterExpression"};
    } else if (!is_get) {
        value_users = {"ConditionExpression"};
    }
    std::vector<std::string_view> name_users = value_users;
    if (is_query || is_scan || is_get) {
        name_users.emplace_back("ProjectionExpression");
    }
    const auto any_of = [&](const std::vector<std::string_view>& ks) {
        for (const auto k : ks) {
            if (member(input, k) != nullptr) {
                return true;
            }
        }
        return false;
    };
    const auto* names = object_member(input, "ExpressionAttributeNames");
    const auto* values = object_member(input, "ExpressionAttributeValues");
    if (names != nullptr && !any_of(name_users)) {
        return validation_error(
            "ExpressionAttributeNames can only be specified when using expressions");
    }
    if (values != nullptr && !is_get && !any_of(value_users)) {
        std::string which;
        for (std::size_t i = 0; i < value_users.size(); ++i) {
            if (i != 0) {
                which += " and ";
            }
            which += value_users[i];
        }
        return validation_error(
            "ExpressionAttributeValues can only be specified when using "
            "expressions: " +
            which + (value_users.size() > 1 ? " are null" : " is null"));
    }

    expression_set out;

    // Legacy parameters.
    const auto* co = member(input, "ConditionalOperator");
    const char* filter_name = is_query ? "QueryFilter" : (is_scan ? "ScanFilter" : "Expected");
    const auto* filter_map = object_member(input, filter_name);
    if (co != nullptr) {
        if (filter_map == nullptr) {
            return validation_error(
                "ConditionalOperator cannot be used without Filter or Expected");
        }
        if (filter_map->size() < 2) {
            return validation_error(
                "ConditionalOperator can only be used when Filter or Expected has two or more "
                "elements");
        }
        if (!co->is_string() || (co->get_string() != "AND" && co->get_string() != "OR")) {
            return validation_error("Member must satisfy enum value set: [AND, OR]");
        }
    }
    const bool use_or = co != nullptr && co->get_string() == "OR";
    std::optional<key_condition> legacy_key;
    if (is_query && member(input, "KeyConditionExpression") == nullptr) {
        const auto* kc = object_member(input, "KeyConditions");
        if (kc == nullptr) {
            return validation_error(
                "Either the KeyConditions or KeyConditionExpression parameter must be specified "
                "in the request.");
        }
        if (kc->empty()) {
            return validation_error("Conditions must be at least length 1");
        }
        std::vector<expr_node> parts;
        for (const auto& kv : *kc) {
            const std::string name(kv.key());
            const auto& entry =
                kv.value().is_object() ? kv.value().get_object() : boost::json::object{};
            const auto op = string_member(entry, "ComparisonOperator");
            if (op && (*op == "NULL" || *op == "NOT_NULL") &&
                name == keys.queried().partition_name) {
                return validation_error("Only scalar types can be used as Hash Key values");
            }
            auto n = condition_entry(name, kv.value());
            if (!n) {
                return n.error();
            }
            if (op && name != keys.queried().partition_name &&
                (*op == "IN" || *op == "CONTAINS" || *op == "NOT_CONTAINS" || *op == "NULL" ||
                 *op == "NOT_NULL" || *op == "NE")) {
                return validation_error(
                    "Attempted conditional constraint is not an indexable operation");
            }
            if (op && *op == "NE") {
                return validation_error("Query key condition not supported");
            }
            parts.push_back(std::move(*n));
        }
        auto k = expression_compiler::key_from_tree(join(std::move(parts), false), keys.queried());
        if (!k) {
            return k.error();
        }
        legacy_key = std::move(*k);
    }
    if (filter_map != nullptr) {
        auto tree = is_query || is_scan ? legacy_conditions(*filter_map, use_or, condition_entry)
                                        : legacy_conditions(*filter_map, use_or, expected_entry);
        if (!tree) {
            return tree.error();
        }
        if (is_query || is_scan) {
            out.filter = std::move(*tree);
        } else {
            out.condition = std::move(*tree);
        }
    }
    if (is_update) {
        if (const auto* au = object_member(input, "AttributeUpdates")) {
            auto u = attribute_updates(*au);
            if (!u) {
                return u.error();
            }
            out.update = std::move(*u);
        }
    }
    if (const auto* atg = member(input, "AttributesToGet"); atg != nullptr && atg->is_array()) {
        if (const auto select = string_member(input, "Select");
            select && *select == "ALL_ATTRIBUTES") {
            return validation_error(
                "Cannot specify the AttributesToGet when choosing to get ALL_ATTRIBUTES");
        }
        projection_expr p;
        for (const auto& n : atg->get_array()) {
            const std::string name = n.is_string() ? std::string(n.get_string()) : std::string();
            for (const auto& seen : p.paths) {
                if (seen.front().name == name) {
                    return validation_error("Cannot have two attributes with the same name");
                }
            }
            p.paths.push_back({path_step{name, -1}});
        }
        if (!p.paths.empty()) {
            out.projection = std::move(p);
        }
    }

    // Expressions.
    // GetItem takes no values, and ignores any it is sent.
    auto compiler = expression_compiler::make(names, is_get ? nullptr : values);
    if (!compiler) {
        return compiler.error();
    }
    auto& c = *compiler;
    if (is_update) {
        if (const auto text = string_member(input, "UpdateExpression")) {
            auto u = c.update(*text);
            if (!u) {
                return u.error();
            }
            out.update = std::move(*u);
        }
    }
    if (is_query || is_scan) {
        if (const auto text = string_member(input, "FilterExpression")) {
            auto f = c.condition(*text, expr_kind::filter);
            if (!f) {
                return f.error();
            }
            out.filter = std::move(*f);
        }
    } else if (!is_get) {
        if (const auto text = string_member(input, "ConditionExpression")) {
            auto f = c.condition(*text, expr_kind::condition);
            if (!f) {
                return f.error();
            }
            out.condition = std::move(*f);
        }
    }
    if (is_query || is_scan || is_get) {
        if (const auto text = string_member(input, "ProjectionExpression")) {
            auto p = c.projection(*text);
            if (!p) {
                return p.error();
            }
            out.projection = std::move(*p);
        }
    }
    std::optional<expr_node> key_tree;
    if (is_query) {
        if (const auto text = string_member(input, "KeyConditionExpression")) {
            auto k = c.condition(*text, expr_kind::key_condition);
            if (!k) {
                return k.error();
            }
            key_tree = std::move(*k);
        }
    }
    if (auto e = c.finish()) {
        return *e;
    }

    // Key-schema rules.
    if (key_tree) {
        auto k = expression_compiler::key_from_tree(*key_tree, keys.queried());
        if (!k) {
            return k.error();
        }
        out.key = std::move(*k);
    } else if (legacy_key) {
        out.key = std::move(legacy_key);
    }
    if (is_query && out.filter) {
        if (auto e = expression_compiler::filter_key_error(*out.filter, keys.queried(),
                                                           filter_map != nullptr)) {
            return *e;
        }
    }
    if (out.update) {
        if (auto e = expression_compiler::check_key_updates(*out.update, keys.table)) {
            return *e;
        }
    }
    return out;
}

}  // namespace kythira::dynamodb
