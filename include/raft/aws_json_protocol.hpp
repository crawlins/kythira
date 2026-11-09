// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_json_protocol.hpp
/// @brief The AWS JSON protocol (1.0 and 1.1), as DynamoDB, DynamoDB Streams
///        and Kinesis speak it (`.kiro/specs/dynamodb-compatible-api/`
///        Requirement 4 and task 2; shared with
///        `.kiro/specs/kinesis-compatible-streams/`, whose task 2 adds CBOR).
///
/// A request is `POST /` with `Content-Type: application/x-amz-json-1.x`,
/// `X-Amz-Target: <TargetPrefix>.<Operation>` and a JSON object body. A
/// response is a JSON object; an error is a JSON object naming its type in
/// `__type`. Every response carries `x-amzn-RequestId` and `x-amz-crc32`
/// (CRC-32 of the body bytes, which the AWS SDKs check on DynamoDB).
///
/// The pieces, from the bottom up:
///
///  - `service_model`: a service's operations, shapes and error shapes,
///    generated from its pinned botocore `service-2.json` by
///    `scripts/gen_aws_json_shapes.py`. Shapes refer to each other and to
///    their members by index into flat arrays, so a model is a set of
///    `constexpr` tables with no start-up cost.
///  - `shape_validator`: checks a request body against an operation's input
///    shape the way AWS's front ends do, in two passes. The first is
///    deserialization: a JSON value of the wrong type (a number where a
///    string belongs, bad base64 in a blob) stops at the first fault with
///    `SerializationException`. The second collects every constraint
///    violation (required members, length and range bounds, enum values,
///    patterns) into one `ValidationException` in AWS's
///    "N validation errors detected: ..." form. Unknown members are refused
///    in the second pass (Requirement 2.3) unless the caller relaxes it.
///  - `codec`: framing. Parses a request (method, content type, target,
///    body limits, JSON depth), and renders responses and errors with the
///    standard headers.
///  - `dispatcher`: routes a parsed, validated request to the handler
///    registered for its operation, and turns handler results, handler
///    errors and escaped exceptions into responses.
///
/// Error type names differ by service: DynamoDB qualifies them
/// (`com.amazonaws.dynamodb.v20120810#ResourceNotFoundException`,
/// `com.amazon.coral.validate#ValidationException`), Kinesis does not. An
/// `error_style` carries those choices, with a preset for each family.
///
/// No transport or Raft dependency: a front end hands the codec the few
/// request fields it needs and writes back the `http_response` it gets.

#include <raft/crc32.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <cctype>
#include <optional>
#include <random>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <utility>
#include <vector>

namespace kythira::aws_json {

// ---------------------------------------------------------------------------
// The service model
// ---------------------------------------------------------------------------

/// "No shape": an operation without input or output, a structure member
/// list that is empty, and so on.
inline constexpr std::uint32_t no_shape = std::numeric_limits<std::uint32_t>::max();

enum class shape_kind : std::uint8_t {
    structure,
    list,
    map,
    string,
    blob,
    boolean,
    integer,  ///< 32-bit signed
    long_,    ///< 64-bit signed
    float_,
    double_,
    timestamp,  ///< epoch seconds, a JSON number (fractions allowed)
};

/// One member of a structure.
struct member {
    std::string_view name;
    std::uint32_t shape = no_shape;
    bool required = false;
};

/// Shape flags, from the model's `exception`, `fault`, `sensitive` and
/// `pattern` handling.
enum shape_flags : std::uint8_t {
    shape_flag_none = 0,
    shape_flag_exception = 1U << 0U,  ///< an error shape
    shape_flag_fault = 1U << 1U,      ///< a server-side error (HTTP 500)
    shape_flag_sensitive = 1U << 2U,  ///< never echo the value in messages
    /// The model's pattern uses Java-only syntax (`\p{...}`, UTF-16 escapes)
    /// that std::regex cannot express. The generator keeps it as
    /// documentation; the validator skips it.
    shape_flag_pattern_unchecked = 1U << 3U,
};

/// One shape. Which fields mean something depends on `kind`:
///
///  - structure: members `first`..`first + count` in the model's `members`;
///  - list: `element` is the member shape;
///  - map: `key` and `element` are the key and value shapes;
///  - string: enum values `first`..`first + count` in `enum_values` (count 0
///    for a free-form string), and `pattern`;
///  - every kind: `min`/`max` when `has_min`/`has_max` (a length for string,
///    blob, list and map; a value for the numbers).
struct shape {
    std::string_view name;
    shape_kind kind = shape_kind::structure;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::uint32_t element = no_shape;
    std::uint32_t key = no_shape;
    bool has_min = false;
    bool has_max = false;
    std::int64_t min = 0;
    std::int64_t max = 0;
    std::string_view pattern;
    std::uint8_t flags = shape_flag_none;

    [[nodiscard]] constexpr auto is(shape_flags f) const noexcept -> bool {
        return (flags & f) != 0;
    }
};

/// One operation: its name (the part of `X-Amz-Target` after the dot) and
/// its input and output shapes.
struct operation {
    std::string_view name;
    std::uint32_t input = no_shape;
    std::uint32_t output = no_shape;
};

enum class json_version : std::uint8_t {
    v1_0,
    v1_1
};

[[nodiscard]] constexpr auto content_type_of(json_version v) noexcept -> std::string_view {
    return v == json_version::v1_0 ? "application/x-amz-json-1.0" : "application/x-amz-json-1.1";
}

/// A whole service. The generator emits shapes and operations sorted by
/// name, which `find_*` relies on.
struct service_model {
    std::string_view target_prefix;  ///< e.g. "DynamoDB_20120810"
    json_version version = json_version::v1_0;
    std::span<const shape> shapes;
    std::span<const member> members;
    std::span<const std::string_view> enum_values;
    std::span<const operation> operations;

    [[nodiscard]] auto find_operation(std::string_view name) const noexcept -> const operation* {
        const auto it =
            std::lower_bound(operations.begin(), operations.end(), name,
                             [](const operation& op, std::string_view n) { return op.name < n; });
        return it != operations.end() && it->name == name ? &*it : nullptr;
    }

    [[nodiscard]] auto find_shape(std::string_view name) const noexcept -> std::uint32_t {
        const auto it =
            std::lower_bound(shapes.begin(), shapes.end(), name,
                             [](const shape& s, std::string_view n) { return s.name < n; });
        return it != shapes.end() && it->name == name
                   ? static_cast<std::uint32_t>(std::distance(shapes.begin(), it))
                   : no_shape;
    }

    [[nodiscard]] auto members_of(const shape& s) const noexcept -> std::span<const member> {
        return s.kind == shape_kind::structure ? members.subspan(s.first, s.count)
                                               : std::span<const member>{};
    }

    [[nodiscard]] auto enum_of(const shape& s) const noexcept -> std::span<const std::string_view> {
        return s.kind == shape_kind::string ? enum_values.subspan(s.first, s.count)
                                            : std::span<const std::string_view>{};
    }
};

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/// An error response. `type` is written to `__type` as is, so it is already
/// qualified the way the service names it (see `error_style`).
struct error {
    unsigned status = 400;
    std::string type;
    std::optional<std::string> message;  ///< absent: no message member at all
    std::string message_key = "message";
    boost::json::object extra;  ///< further members, e.g. CancellationReasons
};

/// How a service names the errors the protocol layer raises itself, and how
/// it qualifies its own modeled errors.
///
/// The DynamoDB names below are the ones DynamoDB documents and the SDKs
/// match on (they match by the part after `#`); the HTTP statuses and the
/// message member spelling for each are AWS's as far as the published SDK
/// test suites show. Behaviour the recording run (task 1 evidence, task 24)
/// shows to differ is corrected here, in one place.
struct error_style {
    std::string service_prefix;      ///< prepended to modeled error names
    std::string validation_type;     ///< constraint violations
    std::string serialization_type;  ///< wrong JSON types, unreadable bodies
    std::string serialization_message_key = "Message";
    std::string unknown_operation_type;  ///< no or unknown X-Amz-Target
    std::string internal_type;           ///< a handler threw
    std::string too_large_type;          ///< body over the limit (HTTP 413)

    /// DynamoDB and DynamoDB Streams (Coral-qualified names).
    [[nodiscard]] static auto coral(std::string_view service_namespace) -> error_style {
        error_style s;
        s.service_prefix = std::string(service_namespace) + "#";
        s.validation_type = "com.amazon.coral.validate#ValidationException";
        s.serialization_type = "com.amazon.coral.service#SerializationException";
        s.unknown_operation_type = "com.amazon.coral.service#UnknownOperationException";
        s.internal_type = s.service_prefix + "InternalServerError";
        s.too_large_type = "com.amazon.coral.service#RequestEntityTooLargeException";
        return s;
    }

    /// Kinesis and other JSON 1.1 services (bare names).
    [[nodiscard]] static auto bare() -> error_style {
        error_style s;
        s.validation_type = "ValidationException";
        s.serialization_type = "SerializationException";
        s.unknown_operation_type = "UnknownOperationException";
        s.internal_type = "InternalFailure";
        s.too_large_type = "RequestEntityTooLargeException";
        return s;
    }

    /// A modeled error (an exception shape of the service), qualified.
    [[nodiscard]] auto modeled(std::string_view name, std::string message,
                               unsigned status = 400) const -> error {
        error e;
        e.status = status;
        e.type = service_prefix + std::string(name);
        e.message = std::move(message);
        return e;
    }
};

/// A value or the error to answer with. The `std::expected<T, error>` the
/// design means, spelled out because clang 18 over libstdc++ (the CI
/// static-analysis setup) does not provide `std::expected`; see
/// sigv4_verifier.hpp's parse_result. The member names match it, so moving
/// to `std::expected` later is a type change.
template<typename T> class result {
public:
    result(T value)
        : _v(std::in_place_index<0>, std::move(value)) {}  // NOLINT(google-explicit-constructor)
    result(aws_json::error e)
        : _v(std::in_place_index<1>, std::move(e)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] auto has_value() const noexcept -> bool { return _v.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] auto value() & -> T& { return std::get<0>(_v); }
    [[nodiscard]] auto value() const& -> const T& { return std::get<0>(_v); }
    [[nodiscard]] auto operator*() & -> T& { return value(); }
    [[nodiscard]] auto operator*() const& -> const T& { return value(); }
    [[nodiscard]] auto operator->() -> T* { return &value(); }
    [[nodiscard]] auto operator->() const -> const T* { return &value(); }
    /// The error. Only meaningful when `!has_value()`.
    [[nodiscard]] auto error() const& -> const aws_json::error& { return std::get<1>(_v); }

private:
    std::variant<T, aws_json::error> _v;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

namespace detail {

inline constexpr std::string_view k_b64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Length of a UTF-8 string in UTF-16 code units, which is what AWS's (Java)
/// length constraints count: a code point above U+FFFF counts twice. Bytes
/// that are not valid UTF-8 count one each.
[[nodiscard]] inline auto utf16_length(std::string_view s) noexcept -> std::size_t {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (c >= 0xF0U && c <= 0xF4U) {
            len = 4;
        } else if (c >= 0xE0U) {
            len = 3;
        } else if (c >= 0xC2U) {
            len = 2;
        }
        if (len > 1 && i + len <= s.size()) {
            bool ok = true;
            for (std::size_t k = 1; k < len; ++k) {
                ok = ok && (static_cast<unsigned char>(s[i + k]) & 0xC0U) == 0x80U;
            }
            if (ok) {
                n += len == 4 ? 2 : 1;
                i += len;
                continue;
            }
        }
        ++n;
        ++i;
    }
    return n;
}

/// The member name as AWS writes it in a validation path: first letter
/// lower-cased (`TableName` -> `tableName`).
[[nodiscard]] inline auto path_name(std::string_view member_name) -> std::string {
    std::string out(member_name);
    if (!out.empty() && out[0] >= 'A' && out[0] <= 'Z') {
        out[0] = static_cast<char>(out[0] - 'A' + 'a');
    }
    return out;
}

[[nodiscard]] inline auto join_path(std::string_view parent, std::string_view child)
    -> std::string {
    if (parent.empty()) {
        return std::string(child);
    }
    std::string out(parent);
    out += '.';
    out += child;
    return out;
}

/// How a value is quoted in a validation message, roughly as Java's
/// toString of the deserialized value prints it.
[[nodiscard]] inline auto render_value(const boost::json::value& v) -> std::string {
    if (v.is_string()) {
        return std::string(v.get_string());
    }
    if (v.is_array()) {
        std::string out = "[";
        bool first = true;
        for (const auto& e : v.get_array()) {
            if (!first) {
                out += ", ";
            }
            first = false;
            out += render_value(e);
        }
        return out + "]";
    }
    if (v.is_object()) {
        std::string out = "{";
        bool first = true;
        for (const auto& kv : v.get_object()) {
            if (!first) {
                out += ", ";
            }
            first = false;
            out += std::string(kv.key());
            out += '=';
            out += render_value(kv.value());
        }
        return out + "}";
    }
    return boost::json::serialize(v);
}

/// An integral JSON number as int64, or nothing (fractional, too large, or
/// not a number). Integral doubles such as `5.0` are accepted.
[[nodiscard]] inline auto as_int64(const boost::json::value& v) noexcept
    -> std::optional<std::int64_t> {
    if (v.is_int64()) {
        return v.get_int64();
    }
    if (v.is_uint64()) {
        const auto u = v.get_uint64();
        if (u <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(u);
        }
        return std::nullopt;
    }
    if (v.is_double()) {
        const double d = v.get_double();
        // 2^63 is exactly representable; anything at or past it is out of
        // range for int64.
        if (std::isfinite(d) && std::trunc(d) == d && d >= -9223372036854775808.0 &&
            d < 9223372036854775808.0) {
            return static_cast<std::int64_t>(d);
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline auto as_double(const boost::json::value& v) noexcept -> std::optional<double> {
    if (v.is_int64()) {
        return static_cast<double>(v.get_int64());
    }
    if (v.is_uint64()) {
        return static_cast<double>(v.get_uint64());
    }
    if (v.is_double()) {
        return v.get_double();
    }
    return std::nullopt;
}

}  // namespace detail

/// Standard base64 (RFC 4648 section 4, padded), the JSON protocol's blob
/// encoding.
[[nodiscard]] inline auto base64_encode(std::span<const std::byte> in) -> std::string {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const auto n = (std::to_integer<std::uint32_t>(in[i]) << 16U) |
                       (std::to_integer<std::uint32_t>(in[i + 1]) << 8U) |
                       std::to_integer<std::uint32_t>(in[i + 2]);
        out += detail::k_b64[(n >> 18U) & 0x3FU];
        out += detail::k_b64[(n >> 12U) & 0x3FU];
        out += detail::k_b64[(n >> 6U) & 0x3FU];
        out += detail::k_b64[n & 0x3FU];
    }
    if (const auto rest = in.size() - i; rest > 0) {
        auto n = std::to_integer<std::uint32_t>(in[i]) << 16U;
        if (rest == 2) {
            n |= std::to_integer<std::uint32_t>(in[i + 1]) << 8U;
        }
        out += detail::k_b64[(n >> 18U) & 0x3FU];
        out += detail::k_b64[(n >> 12U) & 0x3FU];
        out += rest == 2 ? detail::k_b64[(n >> 6U) & 0x3FU] : '=';
        out += '=';
    }
    return out;
}

/// The bytes of a padded standard-base64 string, or nothing when it is not
/// one (wrong length, a character outside the alphabet, padding in the
/// middle, or non-zero bits under the padding).
[[nodiscard]] inline auto base64_decode(std::string_view in)
    -> std::optional<std::vector<std::byte>> {
    if (in.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::byte> out;
    out.reserve(in.size() / 4 * 3);
    for (std::size_t i = 0; i < in.size(); i += 4) {
        std::uint32_t n = 0;
        int pad = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const char c = in[i + k];
            if (c == '=') {
                // Padding only in the last quantum's last one or two places.
                if (i + 4 != in.size() || k < 2) {
                    return std::nullopt;
                }
                ++pad;
                n <<= 6U;
                continue;
            }
            if (pad > 0) {
                return std::nullopt;  // data after padding
            }
            const auto pos = detail::k_b64.find(c);
            if (pos == std::string_view::npos) {
                return std::nullopt;
            }
            n = (n << 6U) | static_cast<std::uint32_t>(pos);
        }
        if ((pad == 1 && (n & 0xFFU) != 0) || (pad == 2 && (n & 0xFFFFU) != 0)) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::byte>((n >> 16U) & 0xFFU));
        if (pad < 2) {
            out.push_back(static_cast<std::byte>((n >> 8U) & 0xFFU));
        }
        if (pad < 1) {
            out.push_back(static_cast<std::byte>(n & 0xFFU));
        }
    }
    return out;
}

/// The `x-amz-crc32` value of a body: CRC-32 (IEEE) of its bytes, in
/// decimal.
[[nodiscard]] inline auto crc32_header(std::string_view body) -> std::string {
    return std::to_string(
        kythira::crc32(std::as_bytes(std::span<const char>(body.data(), body.size()))));
}

/// Request id formats. DynamoDB's are 52 characters of `[0-9A-V]`; Kinesis
/// and most newer services use a version-4 UUID.
enum class request_id_style : std::uint8_t {
    dynamodb,
    uuid
};

[[nodiscard]] inline auto make_request_id(request_id_style style, std::mt19937_64& rng)
    -> std::string {
    if (style == request_id_style::dynamodb) {
        static constexpr std::string_view k_alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
        std::string out(52, '0');
        for (auto& c : out) {
            c = k_alphabet[rng() & 0x1FU];
        }
        return out;
    }
    static constexpr std::string_view k_hex = "0123456789abcdef";
    std::array<unsigned, 32> nibble{};
    for (std::size_t i = 0; i < nibble.size(); i += 16) {
        auto r = rng();
        for (std::size_t k = 0; k < 16; ++k) {
            nibble[i + k] = static_cast<unsigned>(r & 0xFU);
            r >>= 4U;
        }
    }
    nibble[12] = 4;                       // version 4
    nibble[16] = 8U | (nibble[16] & 3U);  // RFC 4122 variant
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < nibble.size(); ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            out += '-';
        }
        out += k_hex[nibble[i]];
    }
    return out;
}

/// A request id from a per-thread generator seeded from std::random_device.
[[nodiscard]] inline auto make_request_id(request_id_style style) -> std::string {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    return make_request_id(style, rng);
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

/// What the validator does with a member its shape does not declare.
enum class unknown_member_policy : std::uint8_t {
    reject,  ///< ValidationException (Requirement 2.3; the default)
    ignore,
};

/// Checks JSON values against a model's shapes. Compiles the model's
/// patterns once on construction; patterns std::regex cannot compile are
/// skipped like the ones the generator flags `pattern_unchecked`, and
/// `skipped_patterns()` names them so a test can pin the list.
class shape_validator {
public:
    explicit shape_validator(const service_model& model,
                             unknown_member_policy unknown = unknown_member_policy::reject)
        : _model(model), _unknown(unknown), _patterns(model.shapes.size()) {
        for (std::size_t i = 0; i < model.shapes.size(); ++i) {
            const auto& s = model.shapes[i];
            if (s.pattern.empty()) {
                continue;
            }
            if (s.is(shape_flag_pattern_unchecked)) {
                _skipped.emplace_back(s.name);
                continue;
            }
            try {
                _patterns[i].emplace(std::string(s.pattern), std::regex::ECMAScript);
            } catch (const std::regex_error&) {
                _skipped.emplace_back(s.name);
            }
        }
    }

    [[nodiscard]] auto model() const noexcept -> const service_model& { return _model; }

    [[nodiscard]] auto skipped_patterns() const noexcept -> const std::vector<std::string>& {
        return _skipped;
    }

    /// Validate `value` (a request body) against shape `root`. Returns the
    /// error to send, or nothing when the value is acceptable.
    [[nodiscard]] auto validate(std::uint32_t root, const boost::json::value& value,
                                const error_style& style) const -> std::optional<error> {
        if (root == no_shape) {
            // An operation without input takes `{}`; anything in it is
            // unknown.
            if (!value.is_object()) {
                return serialization(style, "Start of structure or map found where not expected.");
            }
            if (_unknown == unknown_member_policy::reject && !value.get_object().empty()) {
                std::vector<std::string> v;
                for (const auto& kv : value.get_object()) {
                    v.push_back(unknown_message(kv.key(), ""));
                }
                return validation(style, v);
            }
            return std::nullopt;
        }
        if (auto e = check_types(root, value, ""); e.has_value()) {
            return serialization(style, std::move(*e));
        }
        std::vector<std::string> violations;
        check_constraints(root, value, "", violations);
        if (!violations.empty()) {
            return validation(style, violations);
        }
        return std::nullopt;
    }

private:
    [[nodiscard]] static auto serialization(const error_style& style, std::string message)
        -> error {
        error e;
        e.type = style.serialization_type;
        e.message = std::move(message);
        e.message_key = style.serialization_message_key;
        return e;
    }

    [[nodiscard]] static auto validation(const error_style& style,
                                         const std::vector<std::string>& violations) -> error {
        std::string msg = std::to_string(violations.size());
        msg += violations.size() == 1 ? " validation error detected: "
                                      : " validation errors detected: ";
        for (std::size_t i = 0; i < violations.size(); ++i) {
            if (i > 0) {
                msg += "; ";
            }
            msg += violations[i];
        }
        error e;
        e.type = style.validation_type;
        e.message = std::move(msg);
        return e;
    }

    [[nodiscard]] static auto unknown_message(std::string_view name, std::string_view path)
        -> std::string {
        return "Value at '" + detail::join_path(path, detail::path_name(name)) +
               "' failed to satisfy constraint: Member is not defined in the request shape";
    }

    [[nodiscard]] static auto kind_name(shape_kind k) -> std::string_view {
        switch (k) {
            case shape_kind::structure:
            case shape_kind::map:
                return "structure or map";
            case shape_kind::list:
                return "list";
            case shape_kind::string:
                return "string";
            case shape_kind::blob:
                return "blob";
            case shape_kind::boolean:
                return "boolean";
            case shape_kind::integer:
                return "integer";
            case shape_kind::long_:
                return "long";
            case shape_kind::float_:
                return "float";
            case shape_kind::double_:
                return "double";
            case shape_kind::timestamp:
                return "timestamp";
        }
        return "value";
    }

    /// Pass 1: the JSON type of every present value. The first fault wins.
    [[nodiscard]] auto check_types(std::uint32_t idx, const boost::json::value& v,
                                   const std::string& path) const -> std::optional<std::string> {
        const auto& s = _model.shapes[idx];
        const auto mismatch = [&]() -> std::optional<std::string> {
            return "Expected " + std::string(kind_name(s.kind)) + " at '" +
                   (path.empty() ? std::string("<root>") : path) + "'";
        };
        switch (s.kind) {
            case shape_kind::structure: {
                if (!v.is_object()) {
                    return mismatch();
                }
                for (const auto& m : _model.members_of(s)) {
                    const auto* child = v.get_object().if_contains(m.name);
                    if (child == nullptr || child->is_null()) {
                        continue;
                    }
                    if (auto e = check_types(m.shape, *child,
                                             detail::join_path(path, detail::path_name(m.name)));
                        e.has_value()) {
                        return e;
                    }
                }
                return std::nullopt;
            }
            case shape_kind::list: {
                if (!v.is_array()) {
                    return mismatch();
                }
                std::size_t i = 1;
                for (const auto& e : v.get_array()) {
                    if (!e.is_null()) {
                        if (auto err =
                                check_types(s.element, e,
                                            detail::join_path(path, std::to_string(i) + ".member"));
                            err.has_value()) {
                            return err;
                        }
                    }
                    ++i;
                }
                return std::nullopt;
            }
            case shape_kind::map: {
                if (!v.is_object()) {
                    return mismatch();
                }
                for (const auto& kv : v.get_object()) {
                    if (kv.value().is_null()) {
                        continue;
                    }
                    if (auto err =
                            check_types(s.element, kv.value(),
                                        detail::join_path(path, std::string(kv.key()) + ".member"));
                        err.has_value()) {
                        return err;
                    }
                }
                return std::nullopt;
            }
            case shape_kind::string:
                return v.is_string() ? std::nullopt : mismatch();
            case shape_kind::blob:
                if (!v.is_string()) {
                    return mismatch();
                }
                if (!base64_decode(v.get_string()).has_value()) {
                    return "Base64 encoded value expected at '" + path + "'";
                }
                return std::nullopt;
            case shape_kind::boolean:
                return v.is_bool() ? std::nullopt : mismatch();
            case shape_kind::integer: {
                const auto n = detail::as_int64(v);
                if (!n.has_value() || *n < std::numeric_limits<std::int32_t>::min() ||
                    *n > std::numeric_limits<std::int32_t>::max()) {
                    return mismatch();
                }
                return std::nullopt;
            }
            case shape_kind::long_:
                return detail::as_int64(v).has_value() ? std::nullopt : mismatch();
            case shape_kind::float_:
            case shape_kind::double_:
            case shape_kind::timestamp:
                return detail::as_double(v).has_value() ? std::nullopt : mismatch();
        }
        return mismatch();
    }

    /// Pass 2: constraints, all of them, in member order.
    void check_constraints(std::uint32_t idx, const boost::json::value& v, const std::string& path,
                           std::vector<std::string>& out) const {
        const auto& s = _model.shapes[idx];
        const auto shown = [&]() -> std::string {
            if (s.is(shape_flag_sensitive)) {
                return "Value";
            }
            return "Value '" + detail::render_value(v) + "'";
        };
        const auto fail = [&](std::string_view constraint) {
            out.push_back(shown() + " at '" + path +
                          "' failed to satisfy constraint: " + std::string(constraint));
        };
        const auto bounds = [&](std::size_t n, std::string_view what) {
            if (s.has_min && static_cast<std::int64_t>(n) < s.min) {
                fail("Member must have " + std::string(what) + " greater than or equal to " +
                     std::to_string(s.min));
            }
            if (s.has_max && static_cast<std::int64_t>(n) > s.max) {
                fail("Member must have " + std::string(what) + " less than or equal to " +
                     std::to_string(s.max));
            }
        };
        switch (s.kind) {
            case shape_kind::structure: {
                const auto& obj = v.get_object();
                for (const auto& m : _model.members_of(s)) {
                    const auto child_path = detail::join_path(path, detail::path_name(m.name));
                    const auto* child = obj.if_contains(m.name);
                    if (child == nullptr || child->is_null()) {
                        if (m.required) {
                            out.push_back(
                                "Value null at '" + child_path +
                                "' failed to satisfy constraint: Member must not be null");
                        }
                        continue;
                    }
                    check_constraints(m.shape, *child, child_path, out);
                }
                if (_unknown == unknown_member_policy::reject) {
                    for (const auto& kv : obj) {
                        const auto ms = _model.members_of(s);
                        const bool known = std::any_of(ms.begin(), ms.end(), [&](const member& m) {
                            return m.name == kv.key();
                        });
                        if (!known) {
                            out.push_back(unknown_message(kv.key(), path));
                        }
                    }
                }
                return;
            }
            case shape_kind::list: {
                const auto& arr = v.get_array();
                bounds(arr.size(), "length");
                std::size_t i = 1;
                for (const auto& e : arr) {
                    const auto child_path = detail::join_path(path, std::to_string(i) + ".member");
                    if (e.is_null()) {
                        out.push_back("Value null at '" + child_path +
                                      "' failed to satisfy constraint: Member must not be null");
                    } else {
                        check_constraints(s.element, e, child_path, out);
                    }
                    ++i;
                }
                return;
            }
            case shape_kind::map: {
                const auto& obj = v.get_object();
                bounds(obj.size(), "length");
                for (const auto& kv : obj) {
                    const auto child_path =
                        detail::join_path(path, std::string(kv.key()) + ".member");
                    if (s.key != no_shape) {
                        check_constraints(s.key, boost::json::value(kv.key()),
                                          detail::join_path(path, std::string(kv.key()) + ".key"),
                                          out);
                    }
                    if (kv.value().is_null()) {
                        out.push_back("Value null at '" + child_path +
                                      "' failed to satisfy constraint: Member must not be null");
                    } else {
                        check_constraints(s.element, kv.value(), child_path, out);
                    }
                }
                return;
            }
            case shape_kind::string: {
                const std::string_view str = v.get_string();
                bounds(detail::utf16_length(str), "length");
                const auto values = _model.enum_of(s);
                if (!values.empty() &&
                    std::find(values.begin(), values.end(), str) == values.end()) {
                    std::string set = "Member must satisfy enum value set: [";
                    for (std::size_t i = 0; i < values.size(); ++i) {
                        if (i > 0) {
                            set += ", ";
                        }
                        set += values[i];
                    }
                    fail(set + "]");
                }
                if (const auto& re = _patterns[idx]; re.has_value()) {
                    if (!std::regex_match(str.begin(), str.end(), *re)) {
                        fail("Member must satisfy regular expression pattern: " +
                             std::string(s.pattern));
                    }
                }
                return;
            }
            case shape_kind::blob: {
                // Pass 1 proved it decodes.
                const auto bytes = base64_decode(v.get_string());
                bounds(bytes.has_value() ? bytes->size() : 0, "length");
                return;
            }
            case shape_kind::integer:
            case shape_kind::long_: {
                const auto n = detail::as_int64(v).value_or(0);
                if (s.has_min && n < s.min) {
                    fail("Member must have value greater than or equal to " +
                         std::to_string(s.min));
                }
                if (s.has_max && n > s.max) {
                    fail("Member must have value less than or equal to " + std::to_string(s.max));
                }
                return;
            }
            case shape_kind::float_:
            case shape_kind::double_: {
                const auto d = detail::as_double(v).value_or(0.0);
                if (s.has_min && d < static_cast<double>(s.min)) {
                    fail("Member must have value greater than or equal to " +
                         std::to_string(s.min));
                }
                if (s.has_max && d > static_cast<double>(s.max)) {
                    fail("Member must have value less than or equal to " + std::to_string(s.max));
                }
                return;
            }
            case shape_kind::boolean:
            case shape_kind::timestamp:
                return;
        }
    }

    const service_model& _model;
    unknown_member_policy _unknown;
    std::vector<std::optional<std::regex>> _patterns;
    std::vector<std::string> _skipped;
};

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

/// The request fields the codec reads. Views into the transport's request;
/// they must outlive the call.
struct http_request_view {
    std::string_view method;
    std::string_view path;
    std::string_view content_type;  ///< empty when absent
    std::string_view target;        ///< X-Amz-Target, empty when absent
    std::string_view body;
};

struct http_response {
    unsigned status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    [[nodiscard]] auto header(std::string_view name) const -> std::optional<std::string_view> {
        for (const auto& [k, v] : headers) {
            if (k.size() == name.size() &&
                std::equal(k.begin(), k.end(), name.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                })) {
                return v;
            }
        }
        return std::nullopt;
    }
};

/// Request limits (`.kiro/specs/http-server-request-limits/`): no lower than
/// AWS's own. DynamoDB's largest request (BatchWriteItem, TransactWriteItems
/// with 100 items) stays under 16 MiB. An attribute value nests at most 32
/// levels and each level is two JSON levels (`{"M": {...}}`), so 128 leaves
/// room for the request's own wrapping.
struct request_limits {
    std::size_t max_body_bytes = std::size_t{16} * 1024 * 1024;
    std::size_t max_depth = 128;
};

struct codec_options {
    error_style style;
    request_id_style ids = request_id_style::dynamodb;
    request_limits limits;
    unknown_member_policy unknown_members = unknown_member_policy::reject;
};

/// A request that framed, parsed and validated.
struct parsed_request {
    const operation* op = nullptr;
    boost::json::object input;
};

/// Parses requests for one service and renders its responses.
class codec {
public:
    codec(const service_model& model, codec_options options)
        : _model(model),
          _options(std::move(options)),
          _validator(model, _options.unknown_members) {}

    [[nodiscard]] auto model() const noexcept -> const service_model& { return _model; }
    [[nodiscard]] auto options() const noexcept -> const codec_options& { return _options; }
    [[nodiscard]] auto validator() const noexcept -> const shape_validator& { return _validator; }

    /// Frame, parse and validate. On failure, the error to send.
    [[nodiscard]] auto parse(const http_request_view& req) const -> result<parsed_request> {
        const auto& style = _options.style;
        if (req.body.size() > _options.limits.max_body_bytes) {
            error e;
            e.status = 413;
            e.type = style.too_large_type;
            e.message = "Request body of " + std::to_string(req.body.size()) +
                        " bytes exceeds the limit of " +
                        std::to_string(_options.limits.max_body_bytes) + " bytes";
            return e;
        }
        const auto* op = resolve_target(req);
        if (op == nullptr) {
            error e;
            e.type = style.unknown_operation_type;
            return e;
        }
        boost::json::value body;
        if (req.body.empty()) {
            body = boost::json::object{};
        } else {
            boost::json::parse_options po;
            po.max_depth = _options.limits.max_depth;
            boost::system::error_code ec;
            body = boost::json::parse(req.body, ec, {}, po);
            if (ec) {
                error e;
                e.type = style.serialization_type;
                e.message_key = style.serialization_message_key;
                e.message = ec == boost::json::error::too_deep
                                ? std::string("Request nesting exceeds the maximum depth")
                                : std::string("Unable to parse the request body as JSON");
                return e;
            }
        }
        if (!body.is_object()) {
            error e;
            e.type = style.serialization_type;
            e.message_key = style.serialization_message_key;
            e.message = "Start of structure or map found where not expected.";
            return e;
        }
        if (auto e = _validator.validate(op->input, body, style); e.has_value()) {
            return std::move(*e);
        }
        return parsed_request{op, std::move(body.as_object())};
    }

    /// The operation `req` names, or null when the method, content type or
    /// target does not resolve to one of this service's operations.
    [[nodiscard]] auto resolve_target(const http_request_view& req) const -> const operation* {
        if (req.method != "POST") {
            return nullptr;
        }
        if (!content_type_matches(req.content_type)) {
            return nullptr;
        }
        const auto dot = req.target.rfind('.');
        if (dot == std::string_view::npos || req.target.substr(0, dot) != _model.target_prefix) {
            return nullptr;
        }
        return _model.find_operation(req.target.substr(dot + 1));
    }

    /// A 200 response with `output` as its body.
    [[nodiscard]] auto respond(const boost::json::object& output, std::string request_id) const
        -> http_response {
        http_response r;
        r.status = 200;
        r.body = boost::json::serialize(output);
        finish(r, std::move(request_id));
        return r;
    }

    /// An error response.
    [[nodiscard]] auto respond(const error& e, std::string request_id) const -> http_response {
        boost::json::object body;
        body["__type"] = e.type;
        if (e.message.has_value()) {
            body[e.message_key] = *e.message;
        }
        for (const auto& kv : e.extra) {
            body[kv.key()] = kv.value();
        }
        http_response r;
        r.status = e.status;
        r.body = boost::json::serialize(body);
        finish(r, std::move(request_id));
        return r;
    }

    [[nodiscard]] auto new_request_id() const -> std::string {
        return make_request_id(_options.ids);
    }

private:
    [[nodiscard]] auto content_type_matches(std::string_view ct) const -> bool {
        // Parameters after ';' (a charset) do not change the media type.
        if (const auto semi = ct.find(';'); semi != std::string_view::npos) {
            ct = ct.substr(0, semi);
        }
        while (!ct.empty() && (ct.back() == ' ' || ct.back() == '\t')) {
            ct.remove_suffix(1);
        }
        const auto want = content_type_of(_model.version);
        return ct.size() == want.size() &&
               std::equal(ct.begin(), ct.end(), want.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == b;
               });
    }

    void finish(http_response& r, std::string request_id) const {
        r.headers.emplace_back("Content-Type", std::string(content_type_of(_model.version)));
        r.headers.emplace_back("x-amzn-RequestId", std::move(request_id));
        r.headers.emplace_back("x-amz-crc32", crc32_header(r.body));
    }

    const service_model& _model;
    codec_options _options;
    shape_validator _validator;
};

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

/// What a handler sees besides its input.
struct request_context {
    const operation* op = nullptr;
    std::string request_id;
};

using handler_result = result<boost::json::object>;
using handler = std::function<handler_result(request_context&, boost::json::object&)>;

/// Routes validated requests to per-operation handlers. An operation in the
/// model without a handler answers with the style's
/// UnknownOperationException before its body is validated, so every
/// unimplemented operation returns the same error.
class dispatcher {
public:
    explicit dispatcher(const codec& c) : _codec(c) {}

    /// Register `h` for operation `name`. Throws std::invalid_argument when
    /// the model has no such operation or it already has a handler.
    void on(std::string_view name, handler h) {
        const auto* op = _codec.model().find_operation(name);
        if (op == nullptr) {
            throw std::invalid_argument("aws_json: no operation " + std::string(name) +
                                        " in service " + std::string(_codec.model().target_prefix));
        }
        if (!_handlers.emplace(op, std::move(h)).second) {
            throw std::invalid_argument("aws_json: operation " + std::string(name) +
                                        " already has a handler");
        }
    }

    [[nodiscard]] auto implements(std::string_view name) const -> bool {
        const auto* op = _codec.model().find_operation(name);
        return op != nullptr && _handlers.contains(op);
    }

    /// Answer `req`. Never throws for a request's sake: a handler's
    /// exception becomes the style's internal error (HTTP 500) with no
    /// detail in the body.
    [[nodiscard]] auto handle(const http_request_view& req) const -> http_response {
        auto request_id = _codec.new_request_id();
        // An operation with no handler is unknown whatever its body says,
        // as it is to a server that predates it: answer before validation,
        // so the coverage test can pin one error per unimplemented
        // operation. The body limit still comes first.
        if (req.body.size() <= _codec.options().limits.max_body_bytes) {
            if (const auto* op = _codec.resolve_target(req);
                op != nullptr && !_handlers.contains(op)) {
                error e;
                e.type = _codec.options().style.unknown_operation_type;
                return _codec.respond(e, std::move(request_id));
            }
        }
        auto parsed = _codec.parse(req);
        if (!parsed.has_value()) {
            return _codec.respond(parsed.error(), std::move(request_id));
        }
        const auto it = _handlers.find(parsed->op);
        request_context ctx{parsed->op, request_id};
        try {
            auto out = it->second(ctx, parsed->input);
            if (!out.has_value()) {
                return _codec.respond(out.error(), std::move(request_id));
            }
            return _codec.respond(*out, std::move(request_id));
        } catch (const std::exception&) {
            error e;
            e.status = 500;
            e.type = _codec.options().style.internal_type;
            e.message = "Internal server error";
            return _codec.respond(e, std::move(request_id));
        }
    }

private:
    const codec& _codec;
    std::unordered_map<const operation*, handler> _handlers;
};

}  // namespace kythira::aws_json
