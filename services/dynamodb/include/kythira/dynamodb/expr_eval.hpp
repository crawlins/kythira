// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file expr_eval.hpp
/// @brief Evaluating compiled expressions on an item
///        (`.kiro/specs/dynamodb-compatible-api/` task 5, Requirements 8.2
///        and 8.3).
///
/// `apply()` runs these on the committed item, so they are pure functions
/// of the tree and the item: no clock, no randomness, no locale. A
/// condition never fails: an operand that names a missing attribute, or
/// values of types an operator does not take, make the comparison false
/// (and `<>` true), as DynamoDB evaluates them. An update can fail, with
/// DynamoDB's messages, when an operand names a missing attribute, has the
/// wrong type, or a path's parent is missing or not a map or list.
///
/// Every operand of an update reads the item as it was before the update;
/// the actions then apply in order. List elements a REMOVE names go after
/// the other actions, highest index first, so indexes always mean the
/// original list.

#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/expr.hpp>
#include <kythira/dynamodb/item_codec.hpp>

#include <raft/aws_json_protocol.hpp>

#include <algorithm>
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

namespace eval_detail {

[[nodiscard]] inline auto member_of(const attribute_value& m, std::string_view name)
    -> const attribute_value* {
    for (std::size_t i = 0; i < m.names.size(); ++i) {
        if (m.names[i] == name) {
            return &m.list[i];
        }
    }
    return nullptr;
}

[[nodiscard]] inline auto member_of(attribute_value& m, std::string_view name) -> attribute_value* {
    for (std::size_t i = 0; i < m.names.size(); ++i) {
        if (m.names[i] == name) {
            return &m.list[i];
        }
    }
    return nullptr;
}

// Sets compare as sets, maps by member; everything else member-wise.
[[nodiscard]] inline auto values_equal(const attribute_value& a, const attribute_value& b) -> bool {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case attribute_type::S:
        case attribute_type::B:
            return a.str == b.str;
        case attribute_type::N:
            return a.num == b.num;
        case attribute_type::BOOL:
            return a.boolean == b.boolean;
        case attribute_type::NULL_:
            return true;
        case attribute_type::SS:
        case attribute_type::BS: {
            if (a.names.size() != b.names.size()) {
                return false;
            }
            const std::set<std::string> x(a.names.begin(), a.names.end());
            return std::all_of(b.names.begin(), b.names.end(),
                               [&](const std::string& s) { return x.contains(s); });
        }
        case attribute_type::NS:
            return a.nums.size() == b.nums.size() &&
                   std::all_of(b.nums.begin(), b.nums.end(), [&](const dynamo_decimal& n) {
                       return std::find(a.nums.begin(), a.nums.end(), n) != a.nums.end();
                   });
        case attribute_type::L:
            if (a.list.size() != b.list.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.list.size(); ++i) {
                if (!values_equal(a.list[i], b.list[i])) {
                    return false;
                }
            }
            return true;
        case attribute_type::M:
            if (a.names.size() != b.names.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.names.size(); ++i) {
                const auto* other = member_of(b, a.names[i]);
                if (other == nullptr || !values_equal(a.list[i], *other)) {
                    return false;
                }
            }
            return true;
    }
    return false;
}

// -1, 0, 1 for two S, N or B values of the same type.
[[nodiscard]] inline auto order(const attribute_value& a, const attribute_value& b) -> int {
    if (a.type == attribute_type::N) {
        const auto c = a.num <=> b.num;
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    const auto n = std::min(a.str.size(), b.str.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto x = static_cast<unsigned char>(a.str[i]);
        const auto y = static_cast<unsigned char>(b.str[i]);
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return a.str.size() < b.str.size() ? -1 : (a.str.size() > b.str.size() ? 1 : 0);
}

[[nodiscard]] inline auto ordered_type(attribute_type t) -> bool {
    return t == attribute_type::S || t == attribute_type::N || t == attribute_type::B;
}

// UTF-16 units, as the Java service counts a string's size.
[[nodiscard]] inline auto string_units(std::string_view s) -> std::size_t {
    std::size_t n = 0;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c & 0xC0U) != 0x80U) {
            n += c >= 0xF0U ? 2 : 1;
        }
    }
    return n;
}

}  // namespace eval_detail

/// The value `path` names in `it`, or null.
[[nodiscard]] inline auto resolve_path(const item& it, const document_path& path)
    -> const attribute_value* {
    if (path.empty() || path.front().is_index()) {
        return nullptr;
    }
    const attribute_value* v = find_attribute(it, path.front().name);
    for (std::size_t i = 1; v != nullptr && i < path.size(); ++i) {
        const auto& step = path[i];
        if (step.is_index()) {
            if (v->type != attribute_type::L ||
                static_cast<std::size_t>(step.index) >= v->list.size()) {
                return nullptr;
            }
            v = &v->list[static_cast<std::size_t>(step.index)];
        } else {
            if (v->type != attribute_type::M) {
                return nullptr;
            }
            v = eval_detail::member_of(*v, step.name);
        }
    }
    return v;
}

namespace eval_detail {

// A condition operand's value: a copy for computed values (size), a pointer
// otherwise.
struct operand_value {
    const attribute_value* ref = nullptr;
    std::optional<attribute_value> owned;

    [[nodiscard]] auto get() const -> const attribute_value* { return owned ? &*owned : ref; }
};

[[nodiscard]] inline auto operand(const item& it, const expr_node& n) -> operand_value {
    operand_value out;
    switch (n.op) {
        case expr_op::path:
            out.ref = resolve_path(it, n.path);
            break;
        case expr_op::value:
            out.ref = &n.value;
            break;
        case expr_op::size: {
            const auto inner = operand(it, n.args.front());
            const auto* v = inner.get();
            if (v == nullptr) {
                break;
            }
            std::size_t size = 0;
            switch (v->type) {
                case attribute_type::S:
                    size = string_units(v->str);
                    break;
                case attribute_type::B:
                    size = v->str.size();
                    break;
                case attribute_type::SS:
                case attribute_type::BS:
                    size = v->names.size();
                    break;
                case attribute_type::NS:
                    size = v->nums.size();
                    break;
                case attribute_type::L:
                case attribute_type::M:
                    size = v->list.size();
                    break;
                default:
                    return out;
            }
            out.owned = attribute_value::number(
                dynamo_decimal::from_string(std::to_string(size)).value_or(dynamo_decimal{}));
            break;
        }
        default:
            break;
    }
    return out;
}

[[nodiscard]] inline auto contains(const attribute_value& hay, const attribute_value& needle)
    -> bool {
    switch (hay.type) {
        case attribute_type::S:
            return needle.type == attribute_type::S &&
                   hay.str.find(needle.str) != std::string::npos;
        case attribute_type::B:
            return needle.type == attribute_type::B &&
                   hay.str.find(needle.str) != std::string::npos;
        case attribute_type::SS:
            return needle.type == attribute_type::S &&
                   std::find(hay.names.begin(), hay.names.end(), needle.str) != hay.names.end();
        case attribute_type::BS:
            return needle.type == attribute_type::B &&
                   std::find(hay.names.begin(), hay.names.end(), needle.str) != hay.names.end();
        case attribute_type::NS:
            return needle.type == attribute_type::N &&
                   std::find(hay.nums.begin(), hay.nums.end(), needle.num) != hay.nums.end();
        case attribute_type::L:
            return std::any_of(hay.list.begin(), hay.list.end(),
                               [&](const attribute_value& e) { return values_equal(e, needle); });
        default:
            return false;
    }
}

}  // namespace eval_detail

/// Evaluates a condition or filter on `it` (empty when the item does not
/// exist).
[[nodiscard]] inline auto evaluate_condition(const expr_node& n, const item& it) -> bool {
    using eval_detail::operand;
    switch (n.op) {
        case expr_op::and_:
            return evaluate_condition(n.args[0], it) && evaluate_condition(n.args[1], it);
        case expr_op::or_:
            return evaluate_condition(n.args[0], it) || evaluate_condition(n.args[1], it);
        case expr_op::not_:
            return !evaluate_condition(n.args[0], it);
        case expr_op::eq:
        case expr_op::ne: {
            const auto a = operand(it, n.args[0]);
            const auto b = operand(it, n.args[1]);
            const bool eq = a.get() != nullptr && b.get() != nullptr &&
                            eval_detail::values_equal(*a.get(), *b.get());
            return n.op == expr_op::eq ? eq : !eq;
        }
        case expr_op::lt:
        case expr_op::le:
        case expr_op::gt:
        case expr_op::ge: {
            const auto a = operand(it, n.args[0]);
            const auto b = operand(it, n.args[1]);
            const auto* x = a.get();
            const auto* y = b.get();
            if (x == nullptr || y == nullptr || x->type != y->type ||
                !eval_detail::ordered_type(x->type)) {
                return false;
            }
            const int c = eval_detail::order(*x, *y);
            switch (n.op) {
                case expr_op::lt:
                    return c < 0;
                case expr_op::le:
                    return c <= 0;
                case expr_op::gt:
                    return c > 0;
                default:
                    return c >= 0;
            }
        }
        case expr_op::between: {
            const auto v = operand(it, n.args[0]);
            const auto lo = operand(it, n.args[1]);
            const auto hi = operand(it, n.args[2]);
            const auto* x = v.get();
            const auto* l = lo.get();
            const auto* h = hi.get();
            if (x == nullptr || l == nullptr || h == nullptr || x->type != l->type ||
                x->type != h->type || !eval_detail::ordered_type(x->type)) {
                return false;
            }
            return eval_detail::order(*l, *x) <= 0 && eval_detail::order(*x, *h) <= 0;
        }
        case expr_op::in: {
            const auto v = operand(it, n.args[0]);
            if (v.get() == nullptr) {
                return false;
            }
            for (std::size_t i = 1; i < n.args.size(); ++i) {
                const auto c = operand(it, n.args[i]);
                if (c.get() != nullptr && eval_detail::values_equal(*v.get(), *c.get())) {
                    return true;
                }
            }
            return false;
        }
        case expr_op::attribute_exists:
            return resolve_path(it, n.args[0].path) != nullptr;
        case expr_op::attribute_not_exists:
            return resolve_path(it, n.args[0].path) == nullptr;
        case expr_op::attribute_type: {
            const auto v = operand(it, n.args[0]);
            const auto t = operand(it, n.args[1]);
            return v.get() != nullptr && t.get() != nullptr && t.get()->type == attribute_type::S &&
                   type_name(v.get()->type) == t.get()->str;
        }
        case expr_op::begins_with: {
            const auto a = operand(it, n.args[0]);
            const auto b = operand(it, n.args[1]);
            const auto* x = a.get();
            const auto* y = b.get();
            return x != nullptr && y != nullptr && x->type == y->type &&
                   (x->type == attribute_type::S || x->type == attribute_type::B) &&
                   x->str.starts_with(y->str);
        }
        case expr_op::contains: {
            const auto a = operand(it, n.args[0]);
            const auto b = operand(it, n.args[1]);
            return a.get() != nullptr && b.get() != nullptr &&
                   eval_detail::contains(*a.get(), *b.get());
        }
        default:
            return false;
    }
}

/// Whether `it` satisfies a Query's key condition.
[[nodiscard]] inline auto matches_key(const key_condition& k, const item& it) -> bool {
    const auto* pk = find_attribute(it, k.partition_name);
    if (pk == nullptr || !eval_detail::values_equal(*pk, k.partition_value)) {
        return false;
    }
    if (!k.sort) {
        return true;
    }
    const auto* sk = find_attribute(it, k.sort->name);
    if (sk == nullptr) {
        return false;
    }
    const auto& v = k.sort->values;
    if (k.sort->op == expr_op::begins_with) {
        return sk->type == v[0].type && sk->str.starts_with(v[0].str);
    }
    if (sk->type != v[0].type) {
        return false;
    }
    const int c = eval_detail::order(*sk, v[0]);
    switch (k.sort->op) {
        case expr_op::eq:
            return c == 0;
        case expr_op::lt:
            return c < 0;
        case expr_op::le:
            return c <= 0;
        case expr_op::gt:
            return c > 0;
        case expr_op::ge:
            return c >= 0;
        case expr_op::between:
            return c >= 0 && eval_detail::order(*sk, v[1]) <= 0;
        default:
            return false;
    }
}

namespace eval_detail {

[[nodiscard]] inline auto missing_attribute() -> aws_json::error {
    return validation_error(
        "The provided expression refers to an attribute that does not exist in the item");
}
[[nodiscard]] inline auto wrong_type() -> aws_json::error {
    return validation_error("An operand in the update expression has an incorrect data type");
}
[[nodiscard]] inline auto legacy_mismatch() -> aws_json::error {
    return validation_error("Type mismatch for attribute to update");
}
[[nodiscard]] inline auto bad_path() -> aws_json::error {
    return validation_error(
        "The document path provided in the update expression is invalid for update");
}

[[nodiscard]] inline auto update_value(const item& it, const expr_node& n)
    -> aws_json::result<attribute_value> {
    switch (n.op) {
        case expr_op::path: {
            const auto* v = resolve_path(it, n.path);
            if (v == nullptr) {
                return missing_attribute();
            }
            return *v;
        }
        case expr_op::value:
            return n.value;
        case expr_op::if_not_exists: {
            // The fallback is evaluated, and must resolve, either way.
            auto fallback = update_value(it, n.args[1]);
            if (!fallback) {
                return fallback.error();
            }
            if (const auto* v = resolve_path(it, n.args[0].path)) {
                return *v;
            }
            return fallback;
        }
        case expr_op::list_append: {
            auto a = update_value(it, n.args[0]);
            if (!a) {
                return a.error();
            }
            auto b = update_value(it, n.args[1]);
            if (!b) {
                return b.error();
            }
            if (a->type != attribute_type::L || b->type != attribute_type::L) {
                return wrong_type();
            }
            for (auto& e : b->list) {
                a->list.push_back(std::move(e));
            }
            return a;
        }
        case expr_op::plus:
        case expr_op::minus: {
            auto a = update_value(it, n.args[0]);
            if (!a) {
                return a.error();
            }
            auto b = update_value(it, n.args[1]);
            if (!b) {
                return b.error();
            }
            if (a->type != attribute_type::N || b->type != attribute_type::N) {
                return wrong_type();
            }
            dynamo_decimal r;
            const auto s = n.op == expr_op::plus ? dynamo_decimal::add(a->num, b->num, r)
                                                 : dynamo_decimal::subtract(a->num, b->num, r);
            if (s != decimal_status::ok) {
                return validation_error(dynamo_decimal::message(s));
            }
            return attribute_value::number(r);
        }
        default:
            return wrong_type();
    }
}

// The container that holds `path`'s last step, or null when a step before
// it is missing or of the wrong kind.
[[nodiscard]] inline auto parent_of(item& it, const document_path& path) -> attribute_value* {
    attribute_value* v = nullptr;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const auto& step = path[i];
        if (i == 0) {
            auto pos =
                std::lower_bound(it.begin(), it.end(), step.name,
                                 [](const auto& a, const std::string& n) { return a.first < n; });
            if (pos == it.end() || pos->first != step.name) {
                return nullptr;
            }
            v = &pos->second;
        } else if (step.is_index()) {
            if (v->type != attribute_type::L ||
                static_cast<std::size_t>(step.index) >= v->list.size()) {
                return nullptr;
            }
            v = &v->list[static_cast<std::size_t>(step.index)];
        } else {
            if (v->type != attribute_type::M) {
                return nullptr;
            }
            v = member_of(*v, step.name);
            if (v == nullptr) {
                return nullptr;
            }
        }
    }
    return v;
}

inline void put_member(attribute_value& m, const std::string& name, attribute_value value) {
    const auto pos = std::lower_bound(m.names.begin(), m.names.end(), name);
    const auto i = static_cast<std::size_t>(pos - m.names.begin());
    if (pos != m.names.end() && *pos == name) {
        m.list[i] = std::move(value);
        return;
    }
    m.names.insert(pos, name);
    m.list.insert(m.list.begin() + static_cast<std::ptrdiff_t>(i), std::move(value));
}

inline void erase_member(attribute_value& m, const std::string& name) {
    for (std::size_t i = 0; i < m.names.size(); ++i) {
        if (m.names[i] == name) {
            m.names.erase(m.names.begin() + static_cast<std::ptrdiff_t>(i));
            m.list.erase(m.list.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

inline void put_top(item& it, const std::string& name, attribute_value value) {
    auto pos = std::lower_bound(it.begin(), it.end(), name,
                                [](const auto& a, const std::string& n) { return a.first < n; });
    if (pos != it.end() && pos->first == name) {
        pos->second = std::move(value);
    } else {
        it.insert(pos, {name, std::move(value)});
    }
}

[[nodiscard]] inline auto get_target(item& it, const document_path& path) -> attribute_value* {
    if (path.size() == 1) {
        auto pos =
            std::lower_bound(it.begin(), it.end(), path.front().name,
                             [](const auto& a, const std::string& n) { return a.first < n; });
        return pos != it.end() && pos->first == path.front().name ? &pos->second : nullptr;
    }
    auto* parent = parent_of(it, path);
    if (parent == nullptr) {
        return nullptr;
    }
    const auto& last = path.back();
    if (last.is_index()) {
        if (parent->type != attribute_type::L ||
            static_cast<std::size_t>(last.index) >= parent->list.size()) {
            return nullptr;
        }
        return &parent->list[static_cast<std::size_t>(last.index)];
    }
    return parent->type == attribute_type::M ? member_of(*parent, last.name) : nullptr;
}

// SET: writes `value` at `path`; an index past the end appends.
[[nodiscard]] inline auto set_at(item& it, const document_path& path, attribute_value value)
    -> std::optional<aws_json::error> {
    if (path.size() == 1) {
        put_top(it, path.front().name, std::move(value));
        return std::nullopt;
    }
    auto* parent = parent_of(it, path);
    if (parent == nullptr) {
        return bad_path();
    }
    const auto& last = path.back();
    if (last.is_index()) {
        if (parent->type != attribute_type::L) {
            return bad_path();
        }
        const auto i = static_cast<std::size_t>(last.index);
        if (i < parent->list.size()) {
            parent->list[i] = std::move(value);
        } else {
            parent->list.push_back(std::move(value));
        }
        return std::nullopt;
    }
    if (parent->type != attribute_type::M) {
        return bad_path();
    }
    put_member(*parent, last.name, std::move(value));
    return std::nullopt;
}

[[nodiscard]] inline auto merge_sets(attribute_value& into, const attribute_value& add, bool remove)
    -> bool {
    if (into.type != add.type) {
        return false;
    }
    if (into.type == attribute_type::NS) {
        for (const auto& n : add.nums) {
            const auto pos = std::find(into.nums.begin(), into.nums.end(), n);
            if (remove && pos != into.nums.end()) {
                into.nums.erase(pos);
            } else if (!remove && pos == into.nums.end()) {
                into.nums.push_back(n);
            }
        }
        return true;
    }
    for (const auto& s : add.names) {
        const auto pos = std::find(into.names.begin(), into.names.end(), s);
        if (remove && pos != into.names.end()) {
            into.names.erase(pos);
        } else if (!remove && pos == into.names.end()) {
            into.names.push_back(s);
        }
    }
    return true;
}

[[nodiscard]] inline auto set_is_empty(const attribute_value& v) -> bool {
    return v.type == attribute_type::NS ? v.nums.empty() : v.names.empty();
}

}  // namespace eval_detail

/// Applies `u` to `it` (empty for an item that does not exist yet). On
/// failure `it` is unchanged.
[[nodiscard]] inline auto apply_update(const update_expr& u, item& it)
    -> std::optional<aws_json::error> {
    using namespace eval_detail;
    // DynamoDB checks the actions against the original item clause by
    // clause (REMOVE, DELETE, ADD, then SET), each action's target path
    // before its operand: an ADD or DELETE operand must suit what the path
    // holds, a SET operand must evaluate.
    const auto path_ok = [&](const update_action& a) {
        if (a.kind == update_kind::legacy_add || a.kind == update_kind::legacy_delete ||
            a.path.size() < 2) {
            return true;
        }
        const document_path container(a.path.begin(), a.path.end() - 1);
        const auto* parent = resolve_path(it, container);
        const auto want = a.path.back().is_index() ? attribute_type::L : attribute_type::M;
        return parent != nullptr && parent->type == want;
    };
    std::vector<attribute_value> values(u.actions.size());
    for (const auto kind :
         {update_kind::remove, update_kind::delete_, update_kind::add, update_kind::set,
          update_kind::legacy_add, update_kind::legacy_delete}) {
        for (std::size_t i = 0; i < u.actions.size(); ++i) {
            const auto& a = u.actions[i];
            if (a.kind != kind) {
                continue;
            }
            if (!path_ok(a)) {
                return bad_path();
            }
            if (kind == update_kind::set) {
                auto v = update_value(it, a.value);
                if (!v) {
                    return v.error();
                }
                values[i] = std::move(*v);
                continue;
            }
            if (kind == update_kind::remove) {
                continue;
            }
            values[i] = a.value.value;
            const auto* target = resolve_path(it, a.path);
            if (target == nullptr || (kind != update_kind::add && kind != update_kind::delete_)) {
                continue;
            }
            const auto& v = values[i];
            const bool numbers = kind == update_kind::add && target->type == attribute_type::N &&
                                 v.type == attribute_type::N;
            if (!numbers && target->type != v.type) {
                return wrong_type();
            }
        }
    }
    item out = it;
    // List-element removals wait until the end so indexes stay the
    // original ones: (container path, index) pairs.
    std::vector<std::pair<document_path, std::size_t>> list_removals;
    for (std::size_t i = 0; i < u.actions.size(); ++i) {
        const auto& a = u.actions[i];
        switch (a.kind) {
            case update_kind::set:
                if (auto e = set_at(out, a.path, std::move(values[i]))) {
                    return e;
                }
                break;
            case update_kind::remove: {
                if (a.path.size() == 1) {
                    const auto pos = std::lower_bound(
                        out.begin(), out.end(), a.path.front().name,
                        [](const auto& x, const std::string& n) { return x.first < n; });
                    if (pos != out.end() && pos->first == a.path.front().name) {
                        out.erase(pos);
                    }
                    break;
                }
                auto* parent = parent_of(out, a.path);
                if (parent == nullptr) {
                    return bad_path();
                }
                const auto& last = a.path.back();
                if (last.is_index()) {
                    if (parent->type != attribute_type::L) {
                        return bad_path();
                    }
                    list_removals.emplace_back(document_path(a.path.begin(), a.path.end() - 1),
                                               static_cast<std::size_t>(last.index));
                } else {
                    if (parent->type != attribute_type::M) {
                        return bad_path();
                    }
                    erase_member(*parent, last.name);
                }
                break;
            }
            case update_kind::add: {
                auto* target = get_target(out, a.path);
                if (target == nullptr) {
                    if (auto e = set_at(out, a.path, values[i])) {
                        return e;
                    }
                    break;
                }
                if (target->type == attribute_type::N && values[i].type == attribute_type::N) {
                    dynamo_decimal r;
                    const auto s = dynamo_decimal::add(target->num, values[i].num, r);
                    if (s != decimal_status::ok) {
                        return validation_error(dynamo_decimal::message(s));
                    }
                    target->num = r;
                } else if (!merge_sets(*target, values[i], false)) {
                    return wrong_type();
                }
                break;
            }
            case update_kind::legacy_add: {
                auto* target = get_target(out, a.path);
                if (target == nullptr) {
                    put_top(out, a.path.front().name, values[i]);
                    break;
                }
                if (target->type == attribute_type::N && values[i].type == attribute_type::N) {
                    dynamo_decimal r;
                    const auto s = dynamo_decimal::add(target->num, values[i].num, r);
                    if (s != decimal_status::ok) {
                        return validation_error(dynamo_decimal::message(s));
                    }
                    target->num = r;
                } else if (target->type == attribute_type::L &&
                           values[i].type == attribute_type::L) {
                    for (const auto& e : values[i].list) {
                        target->list.push_back(e);
                    }
                } else if (!merge_sets(*target, values[i], false)) {
                    return legacy_mismatch();
                }
                break;
            }
            case update_kind::legacy_delete:
            case update_kind::delete_: {
                auto* target = get_target(out, a.path);
                if (target == nullptr) {
                    if (a.path.size() > 1 && parent_of(out, a.path) == nullptr) {
                        return bad_path();
                    }
                    break;
                }
                if (!merge_sets(*target, values[i], true)) {
                    return a.kind == update_kind::legacy_delete ? legacy_mismatch() : wrong_type();
                }
                if (set_is_empty(*target)) {
                    if (a.path.size() == 1) {
                        const auto pos = std::lower_bound(
                            out.begin(), out.end(), a.path.front().name,
                            [](const auto& x, const std::string& n) { return x.first < n; });
                        out.erase(pos);
                    } else {
                        auto* parent = parent_of(out, a.path);
                        if (a.path.back().is_index()) {
                            parent->list.erase(parent->list.begin() +
                                               static_cast<std::ptrdiff_t>(a.path.back().index));
                        } else {
                            erase_member(*parent, a.path.back().name);
                        }
                    }
                }
                break;
            }
        }
    }
    std::sort(list_removals.begin(), list_removals.end(),
              [](const auto& x, const auto& y) { return x.second > y.second; });
    for (const auto& [container, index] : list_removals) {
        attribute_value* list = get_target(out, container);
        if (list != nullptr && list->type == attribute_type::L && index < list->list.size()) {
            list->list.erase(list->list.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
    it = std::move(out);
    return std::nullopt;
}

namespace eval_detail {

// The part of `v` the path suffixes select (each `paths[i][depth..]`), or
// nothing when none selects anything. An empty suffix keeps all of `v`.
[[nodiscard]] inline auto project_value(const attribute_value& v,
                                        const std::vector<const document_path*>& paths,
                                        std::size_t depth) -> std::optional<attribute_value> {
    for (const auto* p : paths) {
        if (p->size() == depth) {
            return v;
        }
    }
    attribute_value out;
    out.type = v.type;
    if (v.type == attribute_type::M) {
        std::map<std::string, std::vector<const document_path*>> by_name;
        for (const auto* p : paths) {
            if (!(*p)[depth].is_index()) {
                by_name[(*p)[depth].name].push_back(p);
            }
        }
        for (const auto& [name, group] : by_name) {
            if (const auto* m = member_of(v, name)) {
                if (auto sub = project_value(*m, group, depth + 1)) {
                    put_member(out, name, std::move(*sub));
                }
            }
        }
        return out.list.empty() ? std::nullopt : std::optional<attribute_value>(std::move(out));
    }
    if (v.type == attribute_type::L) {
        std::map<std::size_t, std::vector<const document_path*>> by_index;
        for (const auto* p : paths) {
            if ((*p)[depth].is_index()) {
                by_index[static_cast<std::size_t>((*p)[depth].index)].push_back(p);
            }
        }
        for (const auto& [index, group] : by_index) {
            if (index < v.list.size()) {
                if (auto sub = project_value(v.list[index], group, depth + 1)) {
                    out.list.push_back(std::move(*sub));
                }
            }
        }
        return out.list.empty() ? std::nullopt : std::optional<attribute_value>(std::move(out));
    }
    return std::nullopt;
}

}  // namespace eval_detail

/// The part of `it` the projection keeps. List elements a projection
/// selects keep their order, packed; a map or list nothing selected inside
/// is left out.
[[nodiscard]] inline auto project_item(const projection_expr& p, const item& it) -> item {
    std::map<std::string, std::vector<const document_path*>> by_top;
    for (const auto& path : p.paths) {
        by_top[path.front().name].push_back(&path);
    }
    item out;
    for (const auto& [name, group] : by_top) {
        if (const auto* top = find_attribute(it, name)) {
            if (auto v = eval_detail::project_value(*top, group, 1)) {
                out.emplace_back(name, std::move(*v));
            }
        }
    }
    return out;
}

}  // namespace kythira::dynamodb
