// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief Vector indexes (`.kiro/specs/dynamodb-compatible-api/` task 26,
/// Requirements 22.2-22.4): their definition, stored form and checks, how
/// an item yields a vector, the three distance functions, and the
/// `SearchConditionExpression` they accept.
///
/// A vector index names a vector attribute (a list of numbers, each read as
/// a 32-bit float, of exactly `Dimensions` elements), a distance function,
/// a projection, and an optional search schema: one `HASH` attribute that
/// partitions the index (a search must name its value) and any number of
/// `INLINE_FILTER` attributes a search may filter on with `=`. An item
/// whose vector attribute is missing, is not such a list, or has another
/// length is not in the index, as an item without a GSI's key is not in
/// the GSI; so is an item without the `HASH` attribute.
///
/// Scores, as AWS documents them:
///
/// | Function | Score | Best |
/// |---|---|---|
/// | `COSINE` | 1 - cosine similarity, 0 to 2 | smallest |
/// | `EUCLIDEAN` | the Euclidean distance | smallest |
/// | `DOT_PRODUCT` | the dot product | largest |
///
/// A zero vector has no direction: under `COSINE` it scores 1 against
/// anything (similarity 0).

#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/global_index.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/local_index.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// `VectorDistanceFunction`.
enum class vector_distance : std::uint8_t {
    cosine = 0,
    euclidean = 1,
    dot_product = 2,
};

[[nodiscard]] inline auto distance_name(vector_distance d) -> std::string_view {
    switch (d) {
        case vector_distance::cosine:
            return "COSINE";
        case vector_distance::euclidean:
            return "EUCLIDEAN";
        case vector_distance::dot_product:
            return "DOT_PRODUCT";
    }
    return "COSINE";
}

/// One `SearchSchema` element.
struct search_schema_element {
    std::string name;
    /// `HASH`; otherwise `INLINE_FILTER`.
    bool hash = false;

    friend auto operator==(const search_schema_element&, const search_schema_element&)
        -> bool = default;
};

struct vector_index {
    std::string name;
    std::string attribute;
    std::uint32_t dimensions = 0;
    vector_distance distance = vector_distance::cosine;
    std::vector<search_schema_element> schema;
    projection_kind projection = projection_kind::keys_only;
    std::vector<std::string> non_key;
    index_status status = index_status::creating;
    /// When the index was added (ms since the epoch, from the entry). An
    /// index dropped and added again under the same name is a new index.
    std::uint64_t created_ms = 0;

    /// The `HASH` element's attribute, or null.
    [[nodiscard]] auto hash_attribute() const -> const std::string* {
        const auto it = std::find_if(schema.begin(), schema.end(),
                                     [](const search_schema_element& e) { return e.hash; });
        return it == schema.end() ? nullptr : &it->name;
    }

    friend auto operator==(const vector_index&, const vector_index&) -> bool = default;
};

/// AWS's limits, as far as its model states them.
inline constexpr std::uint32_t k_max_vector_dimensions = 4096;
inline constexpr std::size_t k_max_vector_indexes = 20;

namespace vector_detail {

inline void put_index(std::string& out, const vector_index& v) {
    detail::put_bytes(out, v.name);
    detail::put_bytes(out, v.attribute);
    detail::put_varint(out, v.dimensions);
    out.push_back(static_cast<char>(v.distance));
    detail::put_varint(out, v.schema.size());
    for (const auto& e : v.schema) {
        detail::put_bytes(out, e.name);
        out.push_back(e.hash ? '\1' : '\0');
    }
    out.push_back(static_cast<char>(v.projection));
    detail::put_varint(out, v.non_key.size());
    for (const auto& n : v.non_key) {
        detail::put_bytes(out, n);
    }
    out.push_back(static_cast<char>(v.status));
    detail::put_varint(out, v.created_ms);
}

[[nodiscard]] inline auto get_index(detail::reader& r) -> std::optional<vector_index> {
    vector_index v;
    auto name = r.bytes();
    auto attribute = r.bytes();
    const auto dims = r.varint();
    const auto distance = r.byte();
    const auto elements = r.count();
    if (!name || !attribute || !dims || *dims > k_max_vector_dimensions || !distance ||
        *distance > 2 || !elements) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < *elements; ++i) {
        auto e = r.bytes();
        const auto hash = r.byte();
        if (!e || !hash || *hash > 1) {
            return std::nullopt;
        }
        v.schema.push_back(search_schema_element{std::move(*e), *hash == 1});
    }
    const auto projection = r.byte();
    const auto non_key = r.count();
    if (!projection || *projection > 2 || !non_key) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < *non_key; ++i) {
        auto n = r.bytes();
        if (!n) {
            return std::nullopt;
        }
        v.non_key.push_back(std::move(*n));
    }
    const auto status = r.byte();
    const auto created = r.varint();
    if (!status || *status > 3 || !created) {
        return std::nullopt;
    }
    v.name = std::move(*name);
    v.attribute = std::move(*attribute);
    v.dimensions = static_cast<std::uint32_t>(*dims);
    v.distance = static_cast<vector_distance>(*distance);
    v.projection = static_cast<projection_kind>(*projection);
    v.status = static_cast<index_status>(*status);
    v.created_ms = *created;
    return v;
}

[[nodiscard]] inline auto invalid(std::string what) -> aws_json::error {
    return validation_error("One or more parameter values were invalid: " + std::move(what));
}

}  // namespace vector_detail

inline void put_vector_indexes(std::string& out, const std::vector<vector_index>& all) {
    detail::put_varint(out, all.size());
    for (const auto& v : all) {
        vector_detail::put_index(out, v);
    }
}

[[nodiscard]] inline auto get_vector_indexes(detail::reader& r)
    -> std::optional<std::vector<vector_index>> {
    const auto count = r.count();
    if (!count || *count > k_max_vector_indexes) {
        return std::nullopt;
    }
    std::vector<vector_index> out;
    for (std::size_t i = 0; i < *count; ++i) {
        auto v = vector_detail::get_index(r);
        if (!v) {
            return std::nullopt;
        }
        out.push_back(std::move(*v));
    }
    return out;
}

/// A `CreateVectorIndexAction` (or a `CreateTable` `VectorIndexes`
/// element), checked: the name, a vector attribute that is not a key of
/// `schema`, 1 to 4096 dimensions, at most one `HASH`, no attribute twice,
/// and a projection whose `NonKeyAttributes` come only with `INCLUDE`.
[[nodiscard]] inline auto vector_index_from(const boost::json::object& in, const key_schema& schema)
    -> aws_json::result<vector_index> {
    using vector_detail::invalid;
    vector_index v;
    v.name = std::string(in.at("IndexName").as_string());
    v.attribute = std::string(in.at("VectorAttribute").as_object().at("AttributeName").as_string());
    if (v.attribute == schema.partition_name ||
        (schema.sort_name && v.attribute == *schema.sort_name)) {
        return invalid("The vector attribute " + v.attribute + " of vector index " + v.name +
                       " cannot be a key attribute of the table");
    }
    const auto dims = in.at("Dimensions").to_number<std::int64_t>();
    if (dims < 1 || dims > static_cast<std::int64_t>(k_max_vector_dimensions)) {
        return invalid("Dimensions of vector index " + v.name + " must be between 1 and " +
                       std::to_string(k_max_vector_dimensions));
    }
    v.dimensions = static_cast<std::uint32_t>(dims);
    const auto distance = in.at("DistanceFunction").as_string();
    v.distance = distance == "EUCLIDEAN"     ? vector_distance::euclidean
                 : distance == "DOT_PRODUCT" ? vector_distance::dot_product
                                             : vector_distance::cosine;
    std::vector<std::string> seen{v.attribute};
    if (const auto* s = in.if_contains("SearchSchema")) {
        for (const auto& e : s->as_array()) {
            const auto& o = e.as_object();
            search_schema_element el{std::string(o.at("AttributeName").as_string()),
                                     o.at("SearchSchemaElementType").as_string() == "HASH"};
            if (std::find(seen.begin(), seen.end(), el.name) != seen.end()) {
                return invalid("Attribute " + el.name + " appears more than once in vector index " +
                               v.name);
            }
            seen.push_back(el.name);
            if (el.hash && v.hash_attribute() != nullptr) {
                return invalid("Vector index " + v.name + " can have at most one HASH element");
            }
            v.schema.push_back(std::move(el));
        }
    }
    const auto& p = in.at("Projection").as_object();
    const auto* type = p.if_contains("ProjectionType");
    const std::string_view kind =
        type != nullptr ? std::string_view(type->as_string()) : "KEYS_ONLY";
    v.projection = kind == "ALL"       ? projection_kind::all
                   : kind == "INCLUDE" ? projection_kind::include
                                       : projection_kind::keys_only;
    if (const auto* nk = p.if_contains("NonKeyAttributes")) {
        if (v.projection != projection_kind::include) {
            return invalid("ProjectionType is " + std::string(kind) +
                           ", but NonKeyAttributes is specified");
        }
        for (const auto& n : nk->as_array()) {
            v.non_key.emplace_back(n.as_string());
        }
    } else if (v.projection == projection_kind::include) {
        return invalid("ProjectionType is INCLUDE, but NonKeyAttributes is not specified");
    }
    return v;
}

/// `item`'s vector for `v`: the attribute as `Dimensions` 32-bit floats, or
/// nothing when it is missing or not a list of that many numbers.
[[nodiscard]] inline auto vector_of(const item& it, const vector_index& v)
    -> std::optional<std::vector<float>> {
    const auto* a = find_attribute(it, v.attribute);
    if (a == nullptr || a->type != attribute_type::L || a->list.size() != v.dimensions) {
        return std::nullopt;
    }
    std::vector<float> out;
    out.reserve(a->list.size());
    for (const auto& e : a->list) {
        if (e.type != attribute_type::N) {
            return std::nullopt;
        }
        const auto f = std::strtof(e.num.to_string().c_str(), nullptr);
        if (!std::isfinite(f)) {
            return std::nullopt;
        }
        out.push_back(f);
    }
    return out;
}

/// `SearchVector` as floats, or the error for a bad one.
[[nodiscard]] inline auto search_vector_from(const boost::json::array& in, const vector_index& v)
    -> aws_json::result<std::vector<float>> {
    if (in.size() != v.dimensions) {
        return validation_error("The SearchVector has " + std::to_string(in.size()) +
                                " dimensions but vector index " + v.name + " has " +
                                std::to_string(v.dimensions));
    }
    std::vector<float> out;
    out.reserve(in.size());
    for (const auto& e : in) {
        const auto parsed = attribute_from_json(e);
        if (!parsed || parsed->type != attribute_type::N) {
            return validation_error("Every SearchVector element must be a number (N)");
        }
        const auto f = std::strtof(parsed->num.to_string().c_str(), nullptr);
        if (!std::isfinite(f)) {
            return validation_error("A SearchVector element is not a 32-bit float");
        }
        out.push_back(f);
    }
    return out;
}

/// The score of `b` against the search vector `a` under `d`.
[[nodiscard]] inline auto vector_score(vector_distance d, const std::vector<float>& a,
                                       const std::vector<float>& b) -> double {
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    double sq = 0.0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const double x = a[i];
        const double y = b[i];
        dot += x * y;
        na += x * x;
        nb += y * y;
        sq += (x - y) * (x - y);
    }
    switch (d) {
        case vector_distance::cosine:
            if (na == 0.0 || nb == 0.0) {
                return 1.0;
            }
            return std::clamp(1.0 - dot / (std::sqrt(na) * std::sqrt(nb)), 0.0, 2.0);
        case vector_distance::euclidean:
            return std::sqrt(sq);
        case vector_distance::dot_product:
            return dot;
    }
    return 0.0;
}

/// Whether score `x` ranks before `y` under `d`.
[[nodiscard]] inline auto better_score(vector_distance d, double x, double y) -> bool {
    return d == vector_distance::dot_product ? x > y : x < y;
}

/// A parsed `SearchConditionExpression`: `attribute = value` terms joined
/// by `AND`, each attribute a top-level search schema attribute named at
/// most once. The `HASH` attribute, when the index has one, must be named.
struct search_condition {
    std::vector<std::pair<std::string, attribute_value>> equals;

    /// The `HASH` value, when the condition names it.
    [[nodiscard]] auto value_of(std::string_view name) const -> const attribute_value* {
        for (const auto& [n, v] : equals) {
            if (n == name) {
                return &v;
            }
        }
        return nullptr;
    }

    /// Whether `it` meets every term.
    [[nodiscard]] auto matches(const item& it) const -> bool {
        return std::all_of(equals.begin(), equals.end(), [&](const auto& t) {
            const auto* a = find_attribute(it, t.first);
            return a != nullptr && *a == t.second;
        });
    }
};

namespace vector_detail {

/// The next token of a condition: a word, `#name`, `:value`, `=`, `(`, `)`.
[[nodiscard]] inline auto next_token(std::string_view& in) -> std::string_view {
    std::size_t i = 0;
    while (i < in.size() && std::isspace(static_cast<unsigned char>(in[i])) != 0) {
        ++i;
    }
    in.remove_prefix(i);
    if (in.empty()) {
        return {};
    }
    if (in[0] == '=' || in[0] == '(' || in[0] == ')') {
        const auto t = in.substr(0, 1);
        in.remove_prefix(1);
        return t;
    }
    std::size_t n = 0;
    while (n < in.size() && std::isspace(static_cast<unsigned char>(in[n])) == 0 && in[n] != '=' &&
           in[n] != '(' && in[n] != ')') {
        ++n;
    }
    const auto t = in.substr(0, n);
    in.remove_prefix(n);
    return t;
}

}  // namespace vector_detail

/// Parses `text` against `v`'s search schema, with the request's
/// placeholder maps (either may be null). Parentheses around the whole
/// condition or a term are allowed and mean nothing.
[[nodiscard]] inline auto parse_search_condition(std::string_view text, const vector_index& v,
                                                 const boost::json::object* names,
                                                 const boost::json::object* values)
    -> aws_json::result<search_condition> {
    const auto bad = [](const std::string& why) {
        return validation_error("Invalid SearchConditionExpression: " + why);
    };
    std::vector<std::string_view> tokens;
    for (auto rest = text;;) {
        const auto t = vector_detail::next_token(rest);
        if (t.empty()) {
            break;
        }
        if (t != "(" && t != ")") {
            tokens.push_back(t);
        }
    }
    search_condition out;
    const auto resolve_name = [&](std::string_view t) -> std::optional<std::string> {
        if (t.starts_with('#')) {
            if (names == nullptr) {
                return std::nullopt;
            }
            const auto* n = names->if_contains(t);
            if (n == nullptr || !n->is_string()) {
                return std::nullopt;
            }
            return std::string(n->as_string());
        }
        return std::string(t);
    };
    for (std::size_t i = 0; i < tokens.size(); i += 4) {
        if (i + 2 >= tokens.size() || tokens[i + 1] != "=") {
            return bad("expected terms of the form attribute = :value joined by AND");
        }
        auto lhs = tokens[i];
        auto rhs = tokens[i + 2];
        if (lhs.starts_with(':')) {
            std::swap(lhs, rhs);
        }
        if (!rhs.starts_with(':')) {
            return bad("each term compares an attribute with a :value");
        }
        const auto name = resolve_name(lhs);
        if (!name) {
            return bad("an attribute name placeholder is not defined: " + std::string(lhs));
        }
        const auto known =
            std::find_if(v.schema.begin(), v.schema.end(),
                         [&](const search_schema_element& e) { return e.name == *name; });
        if (known == v.schema.end()) {
            return bad("only the search schema attributes of vector index " + v.name +
                       " can be used: " + *name);
        }
        if (out.value_of(*name) != nullptr) {
            return bad("attribute " + *name + " appears more than once");
        }
        const auto* raw = values == nullptr ? nullptr : values->if_contains(rhs);
        if (raw == nullptr) {
            return bad("an expression attribute value is not defined: " + std::string(rhs));
        }
        auto value = attribute_from_json(*raw);
        if (!value) {
            return bad("the value " + std::string(rhs) + " is not a valid AttributeValue");
        }
        out.equals.emplace_back(*name, std::move(*value));
        if (i + 3 < tokens.size()) {
            std::string word(tokens[i + 3]);
            std::transform(word.begin(), word.end(), word.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (word != "AND") {
                return bad("terms can only be joined by AND");
            }
            if (i + 4 >= tokens.size()) {
                return bad("the condition ends with AND");
            }
        }
    }
    if (const auto* h = v.hash_attribute(); h != nullptr && out.value_of(*h) == nullptr) {
        return validation_error("The SearchConditionExpression must give the value of " + *h +
                                ", the HASH attribute of vector index " + v.name);
    }
    return out;
}

}  // namespace kythira::dynamodb
