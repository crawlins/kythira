// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file expr.hpp
/// @brief The expression tree every replica evaluates, and its versioned
///        binary form (`.kiro/specs/dynamodb-compatible-api/` task 5,
///        Requirement 8.4).
///
/// The gateway compiles condition, filter, key condition, update and
/// projection expressions (and the legacy parameters, and later PartiQL)
/// into these types, with every name and value placeholder already
/// substituted. The Raft entry carries the binary form, so `apply()` never
/// parses text and every replica evaluates the same tree whatever release
/// parsed it.
///
/// The binary form starts with `k_expr_format_version`. A decoder that
/// meets a newer version returns `expr_decode_status::unknown_version`, and
/// the state machine stops the group with a clear log line (design, "Error
/// handling"): a rolling upgrade upgrades every replica before a leader may
/// emit new syntax. The version is bumped only for new syntax; values reuse
/// the item codec's stored form.

#include <kythira/dynamodb/item_codec.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// One step of a document path: a map member or a list index.
struct path_step {
    std::string name;
    /// The list index, or -1 for a map member (`name`).
    std::int64_t index = -1;

    [[nodiscard]] auto is_index() const noexcept -> bool { return index >= 0; }

    friend auto operator==(const path_step&, const path_step&) -> bool = default;
};

/// `a.b[2].c`: the top-level attribute name first.
using document_path = std::vector<path_step>;

/// `[a, b, [2], c]`, as DynamoDB renders a path in its messages.
[[nodiscard]] inline auto render_path(const document_path& p) -> std::string {
    std::string out = "[";
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        if (p[i].is_index()) {
            out += "[" + std::to_string(p[i].index) + "]";
        } else {
            out += p[i].name;
        }
    }
    out += "]";
    return out;
}

/// Node kinds. The numeric values are the binary form: never renumber.
enum class expr_op : std::uint8_t {
    path = 1,
    value = 2,
    // Operand functions and arithmetic.
    size = 3,
    if_not_exists = 4,
    list_append = 5,
    plus = 6,
    minus = 7,
    // Comparisons.
    eq = 8,
    ne = 9,
    lt = 10,
    le = 11,
    gt = 12,
    ge = 13,
    between = 14,
    in = 15,
    // Logic.
    and_ = 16,
    or_ = 17,
    not_ = 18,
    // Boolean functions.
    attribute_exists = 19,
    attribute_not_exists = 20,
    attribute_type = 21,
    begins_with = 22,
    contains = 23,
};

inline constexpr std::uint8_t k_max_expr_op = 23;

/// A node: a path (`path`), a literal (`value`) or an operator over `args`.
struct expr_node {
    expr_op op = expr_op::value;
    document_path path;
    attribute_value value;
    std::vector<expr_node> args;

    friend auto operator==(const expr_node&, const expr_node&) -> bool = default;

    [[nodiscard]] static auto of_path(document_path p) -> expr_node {
        expr_node n;
        n.op = expr_op::path;
        n.path = std::move(p);
        return n;
    }
    [[nodiscard]] static auto of_value(attribute_value v) -> expr_node {
        expr_node n;
        n.op = expr_op::value;
        n.value = std::move(v);
        return n;
    }
    [[nodiscard]] static auto of(expr_op op, std::vector<expr_node> args) -> expr_node {
        expr_node n;
        n.op = op;
        n.args = std::move(args);
        return n;
    }
};

/// The four update clauses, and the two legacy `AttributeUpdates` actions
/// whose runtime rules differ from `ADD` and `DELETE` (a legacy ADD also
/// appends lists; both fail with the legacy message).
enum class update_kind : std::uint8_t {
    set = 1,
    remove = 2,
    add = 3,
    delete_ = 4,
    legacy_add = 5,
    legacy_delete = 6,
};

inline constexpr std::uint8_t k_max_update_kind = 6;

/// One action: `SET path = value`, `REMOVE path`, `ADD path value`,
/// `DELETE path value`. `value` is unused for REMOVE.
struct update_action {
    update_kind kind = update_kind::set;
    document_path path;
    expr_node value;

    friend auto operator==(const update_action&, const update_action&) -> bool = default;
};

/// Actions in the order the expression wrote them. Every operand reads the
/// item as it was before the update.
struct update_expr {
    std::vector<update_action> actions;

    friend auto operator==(const update_expr&, const update_expr&) -> bool = default;
};

/// The paths a projection keeps.
struct projection_expr {
    std::vector<document_path> paths;

    friend auto operator==(const projection_expr&, const projection_expr&) -> bool = default;
};

/// A Query's key condition: the partition key's value, and at most one
/// sort-key condition (`eq`, `lt`, `le`, `gt`, `ge`, `between` with two
/// values, or `begins_with`).
struct key_condition {
    std::string partition_name;
    attribute_value partition_value;
    struct sort_condition {
        std::string name;
        expr_op op = expr_op::eq;
        std::vector<attribute_value> values;

        friend auto operator==(const sort_condition&, const sort_condition&) -> bool = default;
    };
    std::optional<sort_condition> sort;

    friend auto operator==(const key_condition&, const key_condition&) -> bool = default;
};

/// Everything one request compiled, as the Raft entry carries it.
struct expression_set {
    std::optional<expr_node> condition;
    std::optional<update_expr> update;
    std::optional<expr_node> filter;
    std::optional<projection_expr> projection;
    std::optional<key_condition> key;

    friend auto operator==(const expression_set&, const expression_set&) -> bool = default;
};

// ---------------------------------------------------------------------------
// Binary form
// ---------------------------------------------------------------------------

inline constexpr std::uint8_t k_expr_format_version = 1;

enum class expr_decode_status : std::uint8_t {
    ok,
    /// Written by a newer release: stop rather than guess.
    unknown_version,
    /// Truncated, trailing bytes, unknown tag or too deep.
    malformed,
};

namespace detail {

// Bounds a decoder recursion against a hostile or corrupt entry. A parsed
// expression is at most 4 KB of text, so nesting far below this.
inline constexpr int k_max_expr_depth = 4096;

inline void put_path(std::string& out, const document_path& p) {
    put_varint(out, p.size());
    for (const auto& s : p) {
        if (s.is_index()) {
            out.push_back(1);
            put_varint(out, static_cast<std::uint64_t>(s.index));
        } else {
            out.push_back(0);
            put_bytes(out, s.name);
        }
    }
}

[[nodiscard]] inline auto get_path(reader& r) -> std::optional<document_path> {
    const auto n = r.count();
    if (!n) {
        return std::nullopt;
    }
    document_path p;
    for (std::size_t i = 0; i < *n; ++i) {
        const auto tag = r.byte();
        if (!tag) {
            return std::nullopt;
        }
        path_step s;
        if (*tag == 1) {
            const auto idx = r.varint();
            if (!idx ||
                *idx > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return std::nullopt;
            }
            s.index = static_cast<std::int64_t>(*idx);
        } else if (*tag == 0) {
            auto name = r.bytes();
            if (!name) {
                return std::nullopt;
            }
            s.name = std::move(*name);
        } else {
            return std::nullopt;
        }
        p.push_back(std::move(s));
    }
    return p;
}

inline void put_node(std::string& out, const expr_node& n) {
    out.push_back(static_cast<char>(n.op));
    switch (n.op) {
        case expr_op::path:
            put_path(out, n.path);
            return;
        case expr_op::value:
            put_value(out, n.value);
            return;
        default:
            put_varint(out, n.args.size());
            for (const auto& a : n.args) {
                put_node(out, a);
            }
            return;
    }
}

[[nodiscard]] inline auto get_node(reader& r, int depth) -> std::optional<expr_node> {
    if (depth > k_max_expr_depth) {
        return std::nullopt;
    }
    const auto op = r.byte();
    if (!op || *op == 0 || *op > k_max_expr_op) {
        return std::nullopt;
    }
    expr_node n;
    n.op = static_cast<expr_op>(*op);
    if (n.op == expr_op::path) {
        auto p = get_path(r);
        if (!p) {
            return std::nullopt;
        }
        n.path = std::move(*p);
        return n;
    }
    if (n.op == expr_op::value) {
        auto v = get_value(r, 0);
        if (!v) {
            return std::nullopt;
        }
        n.value = std::move(*v);
        return n;
    }
    const auto count = r.count();
    if (!count) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < *count; ++i) {
        auto a = get_node(r, depth + 1);
        if (!a) {
            return std::nullopt;
        }
        n.args.push_back(std::move(*a));
    }
    return n;
}

inline void put_update(std::string& out, const update_expr& u) {
    put_varint(out, u.actions.size());
    for (const auto& a : u.actions) {
        out.push_back(static_cast<char>(a.kind));
        put_path(out, a.path);
        if (a.kind != update_kind::remove) {
            put_node(out, a.value);
        }
    }
}

[[nodiscard]] inline auto get_update(reader& r) -> std::optional<update_expr> {
    const auto n = r.count();
    if (!n) {
        return std::nullopt;
    }
    update_expr u;
    for (std::size_t i = 0; i < *n; ++i) {
        const auto kind = r.byte();
        if (!kind || *kind < 1 || *kind > k_max_update_kind) {
            return std::nullopt;
        }
        update_action a;
        a.kind = static_cast<update_kind>(*kind);
        auto p = get_path(r);
        if (!p) {
            return std::nullopt;
        }
        a.path = std::move(*p);
        if (a.kind != update_kind::remove) {
            auto v = get_node(r, 0);
            if (!v) {
                return std::nullopt;
            }
            a.value = std::move(*v);
        }
        u.actions.push_back(std::move(a));
    }
    return u;
}

inline void put_projection(std::string& out, const projection_expr& p) {
    put_varint(out, p.paths.size());
    for (const auto& path : p.paths) {
        put_path(out, path);
    }
}

[[nodiscard]] inline auto get_projection(reader& r) -> std::optional<projection_expr> {
    const auto n = r.count();
    if (!n) {
        return std::nullopt;
    }
    projection_expr p;
    for (std::size_t i = 0; i < *n; ++i) {
        auto path = get_path(r);
        if (!path) {
            return std::nullopt;
        }
        p.paths.push_back(std::move(*path));
    }
    return p;
}

inline void put_key(std::string& out, const key_condition& k) {
    put_bytes(out, k.partition_name);
    put_value(out, k.partition_value);
    out.push_back(k.sort ? 1 : 0);
    if (k.sort) {
        put_bytes(out, k.sort->name);
        out.push_back(static_cast<char>(k.sort->op));
        put_varint(out, k.sort->values.size());
        for (const auto& v : k.sort->values) {
            put_value(out, v);
        }
    }
}

[[nodiscard]] inline auto get_key(reader& r) -> std::optional<key_condition> {
    key_condition k;
    auto name = r.bytes();
    if (!name) {
        return std::nullopt;
    }
    k.partition_name = std::move(*name);
    auto v = get_value(r, 0);
    if (!v) {
        return std::nullopt;
    }
    k.partition_value = std::move(*v);
    const auto has_sort = r.byte();
    if (!has_sort || *has_sort > 1) {
        return std::nullopt;
    }
    if (*has_sort == 1) {
        key_condition::sort_condition s;
        auto sname = r.bytes();
        const auto op = r.byte();
        const auto n = r.count();
        if (!sname || !op || !n || *op == 0 || *op > k_max_expr_op) {
            return std::nullopt;
        }
        s.name = std::move(*sname);
        s.op = static_cast<expr_op>(*op);
        for (std::size_t i = 0; i < *n; ++i) {
            auto sv = get_value(r, 0);
            if (!sv) {
                return std::nullopt;
            }
            s.values.push_back(std::move(*sv));
        }
        k.sort = std::move(s);
    }
    return k;
}

}  // namespace detail

/// The binary form of a request's expressions.
[[nodiscard]] inline auto encode_expressions(const expression_set& e) -> std::string {
    std::string out;
    out.push_back(static_cast<char>(k_expr_format_version));
    std::uint8_t present = 0;
    present |= e.condition ? 1U : 0U;
    present |= e.update ? 2U : 0U;
    present |= e.filter ? 4U : 0U;
    present |= e.projection ? 8U : 0U;
    present |= e.key ? 16U : 0U;
    out.push_back(static_cast<char>(present));
    if (e.condition) {
        detail::put_node(out, *e.condition);
    }
    if (e.update) {
        detail::put_update(out, *e.update);
    }
    if (e.filter) {
        detail::put_node(out, *e.filter);
    }
    if (e.projection) {
        detail::put_projection(out, *e.projection);
    }
    if (e.key) {
        detail::put_key(out, *e.key);
    }
    return out;
}

/// Decodes `encode_expressions`'s output into `out`.
[[nodiscard]] inline auto decode_expressions(std::string_view bytes, expression_set& out)
    -> expr_decode_status {
    detail::reader r(bytes);
    const auto version = r.byte();
    if (!version) {
        return expr_decode_status::malformed;
    }
    if (*version != k_expr_format_version) {
        return *version > k_expr_format_version ? expr_decode_status::unknown_version
                                                : expr_decode_status::malformed;
    }
    const auto present = r.byte();
    if (!present || (*present & ~0x1FU) != 0) {
        return expr_decode_status::malformed;
    }
    expression_set e;
    if ((*present & 1U) != 0) {
        e.condition = detail::get_node(r, 0);
        if (!e.condition) {
            return expr_decode_status::malformed;
        }
    }
    if ((*present & 2U) != 0) {
        e.update = detail::get_update(r);
        if (!e.update) {
            return expr_decode_status::malformed;
        }
    }
    if ((*present & 4U) != 0) {
        e.filter = detail::get_node(r, 0);
        if (!e.filter) {
            return expr_decode_status::malformed;
        }
    }
    if ((*present & 8U) != 0) {
        e.projection = detail::get_projection(r);
        if (!e.projection) {
            return expr_decode_status::malformed;
        }
    }
    if ((*present & 16U) != 0) {
        e.key = detail::get_key(r);
        if (!e.key) {
            return expr_decode_status::malformed;
        }
    }
    if (!r.done()) {
        return expr_decode_status::malformed;
    }
    out = std::move(e);
    return expr_decode_status::ok;
}

}  // namespace kythira::dynamodb
