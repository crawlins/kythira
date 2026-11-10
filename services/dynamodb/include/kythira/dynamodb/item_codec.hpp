// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file item_codec.hpp
/// @brief Items and attribute values: the wire JSON form, AWS's validation
///        rules, the item-size formula, the compact binary form items are
///        stored in, and key-attribute encoding
///        (`.kiro/specs/dynamodb-compatible-api/` task 3, Requirement 6).
///
/// The core codec has already checked a request against the model, so an
/// `AttributeValue` reaching `attribute_from_json` is an object whose members
/// have the right JSON types and whose `B` values are base64. What is left is
/// what the model cannot say: exactly one type member, `NULL` only `true`,
/// numbers in range, sets non-empty and duplicate-free, at most 32 levels of
/// nested `M` and `L`. Key attributes add the key schema's types, no empty
/// `S` or `B`, and AWS's 2048-byte partition-key and 1024-byte sort-key
/// limits; items add the 400 KB size limit.
///
/// Messages are AWS's as far as published sources and the AWS SDKs' tests
/// show them; task 8's DynamoDB Local differential pins the rest. They are
/// all built here.
///
/// The item-size formula (Requirement 6.4) follows AWS's published rules:
/// an attribute costs its name's UTF-8 bytes plus its value; `S` costs its
/// UTF-8 bytes, `B` its raw bytes, `N` one byte per two significant digits
/// plus one, `BOOL` and `NULL` one byte, a set the sum of its elements, and
/// an `L` or `M` three bytes plus, per element, one byte and the element
/// (for `M`, the element's name too). The per-element byte is not in AWS's
/// prose but is what the service charges (measured by the AWS-community
/// item-size calculators); a real-AWS recording run, should Clark approve
/// one, would settle it.

#include <kythira/dynamodb/decimal.hpp>
#include <kythira/dynamodb/errors.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// The ten attribute types, by their wire names.
enum class attribute_type : std::uint8_t {
    S,
    N,
    B,
    BOOL,
    NULL_,
    M,
    L,
    SS,
    NS,
    BS
};

/// The wire name of a type (`"NULL"` for `NULL_`).
[[nodiscard]] constexpr auto type_name(attribute_type t) noexcept -> std::string_view {
    switch (t) {
        case attribute_type::S:
            return "S";
        case attribute_type::N:
            return "N";
        case attribute_type::B:
            return "B";
        case attribute_type::BOOL:
            return "BOOL";
        case attribute_type::NULL_:
            return "NULL";
        case attribute_type::M:
            return "M";
        case attribute_type::L:
            return "L";
        case attribute_type::SS:
            return "SS";
        case attribute_type::NS:
            return "NS";
        case attribute_type::BS:
            return "BS";
    }
    return "";
}

/// One attribute value. Only the members its type uses are meaningful:
///
///   S, B   `str` (B as raw bytes)        N    `num`
///   BOOL   `boolean`                     NULL nothing
///   L      `list`                        M    `names` and `list`, parallel,
///   SS, BS `names` (BS as raw bytes)          sorted by name
///   NS     `nums`
///
/// Sets keep the order they arrived in; DynamoDB promises none.
struct attribute_value {
    attribute_type type = attribute_type::NULL_;
    std::string str;
    dynamo_decimal num;
    bool boolean = false;
    std::vector<std::string> names;
    std::vector<dynamo_decimal> nums;
    std::vector<attribute_value> list;

    friend auto operator==(const attribute_value&, const attribute_value&) -> bool = default;

    [[nodiscard]] static auto string(std::string s) -> attribute_value {
        attribute_value v;
        v.type = attribute_type::S;
        v.str = std::move(s);
        return v;
    }
    [[nodiscard]] static auto number(dynamo_decimal n) -> attribute_value {
        attribute_value v;
        v.type = attribute_type::N;
        v.num = n;
        return v;
    }
    [[nodiscard]] static auto binary(std::string bytes) -> attribute_value {
        attribute_value v;
        v.type = attribute_type::B;
        v.str = std::move(bytes);
        return v;
    }
};

/// An item: attributes sorted by name, names unique.
using item = std::vector<std::pair<std::string, attribute_value>>;

/// The attribute named `name`, or null.
[[nodiscard]] inline auto find_attribute(const item& it, std::string_view name)
    -> const attribute_value* {
    const auto pos = std::lower_bound(
        it.begin(), it.end(), name, [](const auto& a, std::string_view n) { return a.first < n; });
    if (pos == it.end() || pos->first != name) {
        return nullptr;
    }
    return &pos->second;
}

inline constexpr int k_max_nesting_depth = 32;
inline constexpr std::size_t k_max_item_bytes = 400U * 1024U;
inline constexpr std::size_t k_max_partition_key_bytes = 2048;
inline constexpr std::size_t k_max_sort_key_bytes = 1024;

namespace detail {

[[nodiscard]] inline auto invalid(std::string_view what) -> aws_json::error {
    return validation_error(std::string(k_invalid_parameters) + std::string(what));
}

[[nodiscard]] inline auto bytes_of(std::string_view s) -> std::string {
    return {s.begin(), s.end()};
}

[[nodiscard]] inline auto base64_of(std::string_view raw) -> std::string {
    return aws_json::base64_encode(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw.data()), raw.size()));
}

// "[a, b]", as the Java service renders a collection in its duplicate error.
[[nodiscard]] inline auto render_collection(const boost::json::array& a) -> std::string {
    std::string out = "[";
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        if (a[i].is_string()) {
            out += std::string_view(a[i].get_string());
        } else {
            out += boost::json::serialize(a[i]);
        }
    }
    out += "]";
    return out;
}

[[nodiscard]] inline auto decimal_error(decimal_status s, std::string_view text)
    -> aws_json::error {
    return validation_error(dynamo_decimal::message(s, text));
}

[[nodiscard]] inline auto parse_number(const boost::json::value& v, dynamo_decimal& out)
    -> std::optional<aws_json::error> {
    if (!v.is_string()) {
        return validation_error(
            dynamo_decimal::message(decimal_status::not_a_number, boost::json::serialize(v)));
    }
    const std::string_view text = v.get_string();
    if (const auto s = dynamo_decimal::parse(text, out); s != decimal_status::ok) {
        return decimal_error(s, text);
    }
    return std::nullopt;
}

[[nodiscard]] inline auto decode_blob(const boost::json::value& v, std::string& out)
    -> std::optional<aws_json::error> {
    if (!v.is_string()) {
        return invalid("Invalid binary value");
    }
    auto bytes = aws_json::base64_decode(v.get_string());
    if (!bytes) {
        return invalid("Invalid binary value");
    }
    out.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    return std::nullopt;
}

}  // namespace detail

/// An attribute value from its wire JSON. `depth` is the number of `M` and
/// `L` containers enclosing it.
[[nodiscard]] inline auto attribute_from_json(const boost::json::value& json, int depth = 0)
    -> aws_json::result<attribute_value> {
    static constexpr std::string_view k_one_type =
        "must contain exactly one of the supported datatypes";
    if (!json.is_object()) {
        return validation_error("Supplied AttributeValue is empty, " + std::string(k_one_type));
    }
    const auto& o = json.get_object();
    const boost::json::value* member = nullptr;
    std::string_view tag;
    std::size_t set = 0;
    for (const auto& kv : o) {
        // A member sent as JSON null is absent, as the service reads it.
        if (kv.value().is_null()) {
            continue;
        }
        ++set;
        member = &kv.value();
        tag = kv.key();
    }
    if (set == 0) {
        return validation_error("Supplied AttributeValue is empty, " + std::string(k_one_type));
    }
    if (set > 1) {
        return validation_error("Supplied AttributeValue has more than one datatypes set, " +
                                std::string(k_one_type));
    }
    attribute_value v;
    const auto set_of = [&](std::string_view kind) -> std::optional<aws_json::error> {
        if (!member->is_array()) {
            return detail::invalid("Invalid " + std::string(kind) + " set");
        }
        if (member->get_array().empty()) {
            return detail::invalid("An " + std::string(kind) + " set  may not be empty");
        }
        return std::nullopt;
    };
    // DynamoDB words a duplicate differently for each set type.
    const auto duplicates = [&](attribute_type t) -> aws_json::error {
        if (t == attribute_type::NS) {
            return validation_error("Input collection contains duplicates");
        }
        if (t == attribute_type::BS) {
            return detail::invalid("Input collection of type BS contains duplicates.");
        }
        return detail::invalid("Input collection " +
                               detail::render_collection(member->get_array()) +
                               " contains duplicates");
    };
    if (tag == "S") {
        if (!member->is_string()) {
            return detail::invalid("Invalid string value");
        }
        v = attribute_value::string(detail::bytes_of(member->get_string()));
    } else if (tag == "N") {
        v.type = attribute_type::N;
        if (auto e = detail::parse_number(*member, v.num)) {
            return *e;
        }
    } else if (tag == "B") {
        v.type = attribute_type::B;
        if (auto e = detail::decode_blob(*member, v.str)) {
            return *e;
        }
    } else if (tag == "BOOL") {
        if (!member->is_bool()) {
            return detail::invalid("Invalid boolean value");
        }
        v.type = attribute_type::BOOL;
        v.boolean = member->get_bool();
    } else if (tag == "NULL") {
        if (!member->is_bool() || !member->get_bool()) {
            return detail::invalid("Null attribute value types must have the value of true");
        }
        v.type = attribute_type::NULL_;
    } else if (tag == "SS") {
        if (auto e = set_of("string")) {
            return *e;
        }
        v.type = attribute_type::SS;
        std::set<std::string_view> seen;
        for (const auto& s : member->get_array()) {
            if (!s.is_string()) {
                return detail::invalid("Invalid string set");
            }
            if (!seen.insert(s.get_string()).second) {
                return duplicates(attribute_type::SS);
            }
            v.names.emplace_back(s.get_string());
        }
    } else if (tag == "NS") {
        if (auto e = set_of("number")) {
            return *e;
        }
        v.type = attribute_type::NS;
        for (const auto& s : member->get_array()) {
            dynamo_decimal n;
            if (auto e = detail::parse_number(s, n)) {
                return *e;
            }
            // Numeric duplicates: "1" and "1.0" are the same element.
            if (std::find(v.nums.begin(), v.nums.end(), n) != v.nums.end()) {
                return duplicates(attribute_type::NS);
            }
            v.nums.push_back(n);
        }
    } else if (tag == "BS") {
        if (auto e = set_of("binary")) {
            return *e;
        }
        v.type = attribute_type::BS;
        std::set<std::string> seen;
        for (const auto& s : member->get_array()) {
            std::string raw;
            if (auto e = detail::decode_blob(s, raw)) {
                return *e;
            }
            if (!seen.insert(raw).second) {
                return duplicates(attribute_type::BS);
            }
            v.names.push_back(std::move(raw));
        }
    } else if (tag == "L" || tag == "M") {
        if (depth + 1 > k_max_nesting_depth) {
            return validation_error("Nesting Levels have exceeded supported limits");
        }
        if (tag == "L") {
            if (!member->is_array()) {
                return detail::invalid("Invalid list value");
            }
            v.type = attribute_type::L;
            for (const auto& e : member->get_array()) {
                auto inner = attribute_from_json(e, depth + 1);
                if (!inner) {
                    return inner.error();
                }
                v.list.push_back(std::move(*inner));
            }
        } else {
            if (!member->is_object()) {
                return detail::invalid("Invalid map value");
            }
            v.type = attribute_type::M;
            std::vector<std::pair<std::string, attribute_value>> entries;
            for (const auto& kv : member->get_object()) {
                auto inner = attribute_from_json(kv.value(), depth + 1);
                if (!inner) {
                    return inner.error();
                }
                entries.emplace_back(std::string(kv.key()), std::move(*inner));
            }
            std::sort(entries.begin(), entries.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            for (auto& [name, value] : entries) {
                v.names.push_back(std::move(name));
                v.list.push_back(std::move(value));
            }
        }
    } else {
        // The core codec refuses members the model lacks before this runs.
        return validation_error("Supplied AttributeValue is empty, " + std::string(k_one_type));
    }
    return v;
}

/// The wire JSON of an attribute value.
[[nodiscard]] inline auto attribute_to_json(const attribute_value& v) -> boost::json::value {
    boost::json::object o;
    const auto key = type_name(v.type);
    switch (v.type) {
        case attribute_type::S:
            o[key] = v.str;
            break;
        case attribute_type::N:
            o[key] = v.num.to_string();
            break;
        case attribute_type::B:
            o[key] = detail::base64_of(v.str);
            break;
        case attribute_type::BOOL:
            o[key] = v.boolean;
            break;
        case attribute_type::NULL_:
            o[key] = true;
            break;
        case attribute_type::SS: {
            boost::json::array a;
            for (const auto& s : v.names) {
                a.emplace_back(s);
            }
            o[key] = std::move(a);
            break;
        }
        case attribute_type::NS: {
            boost::json::array a;
            for (const auto& n : v.nums) {
                a.emplace_back(n.to_string());
            }
            o[key] = std::move(a);
            break;
        }
        case attribute_type::BS: {
            boost::json::array a;
            for (const auto& s : v.names) {
                a.emplace_back(detail::base64_of(s));
            }
            o[key] = std::move(a);
            break;
        }
        case attribute_type::L: {
            boost::json::array a;
            for (const auto& e : v.list) {
                a.push_back(attribute_to_json(e));
            }
            o[key] = std::move(a);
            break;
        }
        case attribute_type::M: {
            boost::json::object m;
            for (std::size_t i = 0; i < v.names.size(); ++i) {
                m[v.names[i]] = attribute_to_json(v.list[i]);
            }
            o[key] = std::move(m);
            break;
        }
    }
    return o;
}

/// An item from a wire `Item` (or `Key`) map.
[[nodiscard]] inline auto item_from_json(const boost::json::object& json)
    -> aws_json::result<item> {
    item out;
    out.reserve(json.size());
    for (const auto& kv : json) {
        auto v = attribute_from_json(kv.value());
        if (!v) {
            return v.error();
        }
        out.emplace_back(std::string(kv.key()), std::move(*v));
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

[[nodiscard]] inline auto item_to_json(const item& it) -> boost::json::object {
    boost::json::object o;
    for (const auto& [name, value] : it) {
        o[name] = attribute_to_json(value);
    }
    return o;
}

// ---------------------------------------------------------------------------
// Size
// ---------------------------------------------------------------------------

/// The bytes AWS charges for a value (without its name).
[[nodiscard]] inline auto attribute_size(const attribute_value& v) -> std::size_t {
    constexpr std::size_t k_container_overhead = 3;
    constexpr std::size_t k_per_element = 1;
    std::size_t n = 0;
    switch (v.type) {
        case attribute_type::S:
        case attribute_type::B:
            return v.str.size();
        case attribute_type::N:
            return v.num.size_bytes();
        case attribute_type::BOOL:
        case attribute_type::NULL_:
            return 1;
        case attribute_type::SS:
        case attribute_type::BS:
            for (const auto& s : v.names) {
                n += s.size();
            }
            return n;
        case attribute_type::NS:
            for (const auto& d : v.nums) {
                n += d.size_bytes();
            }
            return n;
        case attribute_type::L:
            n = k_container_overhead;
            for (const auto& e : v.list) {
                n += k_per_element + attribute_size(e);
            }
            return n;
        case attribute_type::M:
            n = k_container_overhead;
            for (std::size_t i = 0; i < v.list.size(); ++i) {
                n += k_per_element + v.names[i].size() + attribute_size(v.list[i]);
            }
            return n;
    }
    return n;
}

/// The bytes AWS charges for an item: every attribute's name and value.
[[nodiscard]] inline auto item_size(const item& it) -> std::size_t {
    std::size_t n = 0;
    for (const auto& [name, value] : it) {
        n += name.size() + attribute_size(value);
    }
    return n;
}

/// The 400 KB item limit.
[[nodiscard]] inline auto check_item_size(const item& it) -> std::optional<aws_json::error> {
    if (item_size(it) > k_max_item_bytes) {
        return validation_error("Item size has exceeded the maximum allowed size");
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

/// A table's (or index's) key schema: a partition key and an optional sort
/// key, each S, N or B.
struct key_schema {
    std::string partition_name;
    attribute_type partition_type = attribute_type::S;
    std::optional<std::string> sort_name;
    attribute_type sort_type = attribute_type::S;
};

namespace detail {

[[nodiscard]] inline auto check_key_attribute(const item& it, const std::string& name,
                                              attribute_type expected, bool partition)
    -> std::optional<aws_json::error> {
    const auto* v = find_attribute(it, name);
    if (v == nullptr) {
        return invalid("Missing the key " + name + " in the item");
    }
    if (v->type != expected) {
        return invalid("Type mismatch for key " + name +
                       " expected: " + std::string(type_name(expected)) +
                       " actual: " + std::string(type_name(v->type)));
    }
    if ((v->type == attribute_type::S || v->type == attribute_type::B) && v->str.empty()) {
        return validation_error(
            "One or more parameter values are not valid. The AttributeValue for a key attribute "
            "cannot contain an empty " +
            std::string(v->type == attribute_type::S ? "string" : "binary") +
            " value. Key: " + name);
    }
    const auto size = attribute_size(*v);
    if (partition && size > k_max_partition_key_bytes) {
        return invalid("Size of hashkey has exceeded the maximum size limit of2048 bytes");
    }
    if (!partition && size > k_max_sort_key_bytes) {
        return invalid(
            "Aggregated size of all range keys has exceeded the size limit of 1024 bytes");
    }
    return std::nullopt;
}

}  // namespace detail

/// Checks that `it` carries the schema's key attributes with the right
/// types and within the size limits. An item may carry other attributes.
[[nodiscard]] inline auto validate_key_attributes(const item& it, const key_schema& schema)
    -> std::optional<aws_json::error> {
    if (auto e =
            detail::check_key_attribute(it, schema.partition_name, schema.partition_type, true)) {
        return e;
    }
    if (schema.sort_name) {
        return detail::check_key_attribute(it, *schema.sort_name, schema.sort_type, false);
    }
    return std::nullopt;
}

/// Checks a `Key` parameter: exactly the schema's attributes, nothing else.
[[nodiscard]] inline auto validate_key(const item& key, const key_schema& schema)
    -> std::optional<aws_json::error> {
    const std::size_t want = schema.sort_name ? 2 : 1;
    if (key.size() != want) {
        return detail::invalid("The provided key element does not match the schema");
    }
    for (const auto& [name, value] : key) {
        if (name != schema.partition_name && (!schema.sort_name || name != *schema.sort_name)) {
            return detail::invalid("The provided key element does not match the schema");
        }
    }
    if (auto e = validate_key_attributes(key, schema)) {
        // A Key whose types are wrong gets the schema message, not the
        // item one.
        if (e->message && e->message->find("Type mismatch") != std::string::npos) {
            return detail::invalid("The provided key element does not match the schema");
        }
        return e;
    }
    return std::nullopt;
}

/// The order-preserving, self-delimiting byte form of a key attribute (S, N
/// or B): S and B compare as unsigned bytes (Requirement 6.3), with 0x00
/// escaped as 0x00 0xFF and the end marked 0x00 0x01, so a shorter value
/// that is a prefix of a longer one sorts first; N uses
/// `dynamo_decimal::encode_key`. Encodings concatenate into composite keys
/// that still sort component by component.
[[nodiscard]] inline auto encode_key_component(const attribute_value& v) -> std::string {
    if (v.type == attribute_type::N) {
        return v.num.encode_key();
    }
    std::string out;
    out.reserve(v.str.size() + 2);
    for (const char c : v.str) {
        out.push_back(c);
        if (c == '\0') {
            out.push_back('\xFF');
        }
    }
    out.push_back('\0');
    out.push_back('\x01');
    return out;
}

/// Reads one S or B component from the front of `in` (the inverse of
/// `encode_key_component`), advancing past it.
[[nodiscard]] inline auto decode_key_bytes(std::string_view& in) -> std::optional<std::string> {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\0') {
            out.push_back(in[i]);
            continue;
        }
        if (i + 1 >= in.size()) {
            return std::nullopt;
        }
        if (in[i + 1] == '\x01') {
            in.remove_prefix(i + 2);
            return out;
        }
        if (in[i + 1] != '\xFF') {
            return std::nullopt;
        }
        out.push_back('\0');
        ++i;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Binary storage form
// ---------------------------------------------------------------------------
//
// What an item is stored as in the engine and carried in Raft entries.
// Version byte 1, then a varint attribute count and, per attribute, a
// varint-length name and a value. A value is a type tag, then:
//
//   S, B      varint length, bytes        BOOL  tag 4 (false) or 5 (true)
//   N         dynamo_decimal::encode_key  NULL  tag only
//   L         varint count, values        M     varint count, (name, value)
//   SS, BS    varint count, (length, bytes)
//   NS        varint count, encode_key each
//
// Varints are unsigned LEB128. The tags are part of the stored format and
// never change meaning; a new type gets a new tag.

namespace detail {

inline constexpr std::uint8_t k_item_format_version = 1;

enum class storage_tag : std::uint8_t {
    s = 1,
    n = 2,
    b = 3,
    bool_false = 4,
    bool_true = 5,
    null = 6,
    m = 7,
    l = 8,
    ss = 9,
    ns = 10,
    bs = 11,
};

inline void put_varint(std::string& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<char>((v & 0x7FU) | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<char>(v));
}

inline void put_bytes(std::string& out, std::string_view s) {
    put_varint(out, s.size());
    out.append(s);
}

inline void put_tag(std::string& out, storage_tag t) {
    out.push_back(static_cast<char>(t));
}

inline void put_value(std::string& out, const attribute_value& v) {
    switch (v.type) {
        case attribute_type::S:
            put_tag(out, storage_tag::s);
            put_bytes(out, v.str);
            break;
        case attribute_type::B:
            put_tag(out, storage_tag::b);
            put_bytes(out, v.str);
            break;
        case attribute_type::N:
            put_tag(out, storage_tag::n);
            out += v.num.encode_key();
            break;
        case attribute_type::BOOL:
            put_tag(out, v.boolean ? storage_tag::bool_true : storage_tag::bool_false);
            break;
        case attribute_type::NULL_:
            put_tag(out, storage_tag::null);
            break;
        case attribute_type::SS:
        case attribute_type::BS:
            put_tag(out, v.type == attribute_type::SS ? storage_tag::ss : storage_tag::bs);
            put_varint(out, v.names.size());
            for (const auto& s : v.names) {
                put_bytes(out, s);
            }
            break;
        case attribute_type::NS:
            put_tag(out, storage_tag::ns);
            put_varint(out, v.nums.size());
            for (const auto& n : v.nums) {
                out += n.encode_key();
            }
            break;
        case attribute_type::L:
            put_tag(out, storage_tag::l);
            put_varint(out, v.list.size());
            for (const auto& e : v.list) {
                put_value(out, e);
            }
            break;
        case attribute_type::M:
            put_tag(out, storage_tag::m);
            put_varint(out, v.list.size());
            for (std::size_t i = 0; i < v.list.size(); ++i) {
                put_bytes(out, v.names[i]);
                put_value(out, v.list[i]);
            }
            break;
    }
}

class reader {
public:
    explicit reader(std::string_view in) : _in(in) {}

    [[nodiscard]] auto done() const -> bool { return _in.empty(); }

    [[nodiscard]] auto byte() -> std::optional<std::uint8_t> {
        if (_in.empty()) {
            return std::nullopt;
        }
        const auto b = static_cast<std::uint8_t>(_in[0]);
        _in.remove_prefix(1);
        return b;
    }

    [[nodiscard]] auto varint() -> std::optional<std::uint64_t> {
        std::uint64_t v = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            const auto b = byte();
            if (!b) {
                return std::nullopt;
            }
            v |= static_cast<std::uint64_t>(*b & 0x7FU) << shift;
            if ((*b & 0x80U) == 0) {
                return v;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto bytes() -> std::optional<std::string> {
        const auto n = varint();
        if (!n || *n > _in.size()) {
            return std::nullopt;
        }
        std::string s(_in.substr(0, *n));
        _in.remove_prefix(*n);
        return s;
    }

    // A count that cannot exceed the bytes left (every element takes at
    // least one), so a corrupt count cannot ask for a huge allocation.
    [[nodiscard]] auto count() -> std::optional<std::size_t> {
        const auto n = varint();
        if (!n || *n > _in.size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(*n);
    }

    [[nodiscard]] auto number() -> std::optional<dynamo_decimal> {
        return dynamo_decimal::decode_key(_in);
    }

private:
    std::string_view _in;
};

[[nodiscard]] inline auto get_value(reader& r, int depth) -> std::optional<attribute_value> {
    const auto tag = r.byte();
    if (!tag) {
        return std::nullopt;
    }
    attribute_value v;
    switch (static_cast<storage_tag>(*tag)) {
        case storage_tag::s:
        case storage_tag::b: {
            auto s = r.bytes();
            if (!s) {
                return std::nullopt;
            }
            v.type = static_cast<storage_tag>(*tag) == storage_tag::s ? attribute_type::S
                                                                      : attribute_type::B;
            v.str = std::move(*s);
            return v;
        }
        case storage_tag::n: {
            auto n = r.number();
            if (!n) {
                return std::nullopt;
            }
            return attribute_value::number(*n);
        }
        case storage_tag::bool_false:
        case storage_tag::bool_true:
            v.type = attribute_type::BOOL;
            v.boolean = static_cast<storage_tag>(*tag) == storage_tag::bool_true;
            return v;
        case storage_tag::null:
            v.type = attribute_type::NULL_;
            return v;
        case storage_tag::ss:
        case storage_tag::bs: {
            v.type = static_cast<storage_tag>(*tag) == storage_tag::ss ? attribute_type::SS
                                                                       : attribute_type::BS;
            const auto n = r.count();
            if (!n) {
                return std::nullopt;
            }
            for (std::size_t i = 0; i < *n; ++i) {
                auto s = r.bytes();
                if (!s) {
                    return std::nullopt;
                }
                v.names.push_back(std::move(*s));
            }
            return v;
        }
        case storage_tag::ns: {
            v.type = attribute_type::NS;
            const auto n = r.count();
            if (!n) {
                return std::nullopt;
            }
            for (std::size_t i = 0; i < *n; ++i) {
                auto d = r.number();
                if (!d) {
                    return std::nullopt;
                }
                v.nums.push_back(*d);
            }
            return v;
        }
        case storage_tag::l:
        case storage_tag::m: {
            if (depth + 1 > k_max_nesting_depth) {
                return std::nullopt;
            }
            const bool is_map = static_cast<storage_tag>(*tag) == storage_tag::m;
            v.type = is_map ? attribute_type::M : attribute_type::L;
            const auto n = r.count();
            if (!n) {
                return std::nullopt;
            }
            for (std::size_t i = 0; i < *n; ++i) {
                if (is_map) {
                    auto name = r.bytes();
                    if (!name) {
                        return std::nullopt;
                    }
                    v.names.push_back(std::move(*name));
                }
                auto e = get_value(r, depth + 1);
                if (!e) {
                    return std::nullopt;
                }
                v.list.push_back(std::move(*e));
            }
            return v;
        }
    }
    return std::nullopt;
}

}  // namespace detail

/// The stored form of an item.
[[nodiscard]] inline auto encode_item(const item& it) -> std::string {
    std::string out;
    out.push_back(static_cast<char>(detail::k_item_format_version));
    detail::put_varint(out, it.size());
    for (const auto& [name, value] : it) {
        detail::put_bytes(out, name);
        detail::put_value(out, value);
    }
    return out;
}

/// An item from its stored form, or nothing when the bytes are not one
/// (unknown version or tag, truncation, trailing bytes, nesting too deep).
[[nodiscard]] inline auto decode_item(std::string_view bytes) -> std::optional<item> {
    detail::reader r(bytes);
    const auto version = r.byte();
    if (!version || *version != detail::k_item_format_version) {
        return std::nullopt;
    }
    const auto n = r.count();
    if (!n) {
        return std::nullopt;
    }
    item out;
    out.reserve(*n);
    for (std::size_t i = 0; i < *n; ++i) {
        auto name = r.bytes();
        if (!name) {
            return std::nullopt;
        }
        auto v = detail::get_value(r, 0);
        if (!v) {
            return std::nullopt;
        }
        out.emplace_back(std::move(*name), std::move(*v));
    }
    if (!r.done()) {
        return std::nullopt;
    }
    return out;
}

}  // namespace kythira::dynamodb
