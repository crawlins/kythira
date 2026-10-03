// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file composite_node_id.hpp
/// @brief Node id concepts, the provider composite id types, and the one set
///        of helpers that prints, parses and allocates every kind of node id.
///
/// A composite node id names the cloud resource a node runs on: the provider,
/// the scope in which the cloud's own id is unique (a region, a resource
/// group, a project and zone), and that native id. Its canonical text is
/// `<provider>:<scope>:<native>`, with `:` and `%` percent-encoded inside a
/// segment. See `.kiro/specs/cloud-composite-node-ids/`.

#include <algorithm>
#include <charconv>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace kythira {

// ============================================================================
// Concepts
// ============================================================================

/// @brief A typed cloud node id: provider, scope and native id, with one
///        canonical text form that `parse` reads back exactly.
template<typename T>
concept composite_node_id =
    std::regular<T> && std::totally_ordered<T> && requires(const T& t, std::string_view s) {
        { T::provider } -> std::convertible_to<std::string_view>;
        { t.scope() } -> std::same_as<std::string_view>;
        { t.native() } -> std::same_as<std::string_view>;
        { t.to_string() } -> std::same_as<std::string>;
        { T::parse(s) } -> std::same_as<std::optional<T>>;
        { std::hash<T>{}(t) } -> std::convertible_to<std::size_t>;
    };

/// @brief A node id carried as text: `std::string` (canonical composite text
///        by convention) or a composite type.
template<typename T>
concept textual_node_id = std::same_as<T, std::string> || composite_node_id<T>;

/// @brief Concept for a node identifier: any unsigned integer, `std::string`,
///        or a composite cloud id.
template<typename T>
concept node_id = std::unsigned_integral<T> || textual_node_id<T>;

// ============================================================================
// basic_composite_node_id
// ============================================================================

/// @brief A string literal usable as a template argument.
template<std::size_t N> struct fixed_string {
    char value[N]{};
    constexpr fixed_string(const char (&s)[N]) { std::copy_n(s, N, value); }
    constexpr operator std::string_view() const { return {value, N - 1}; }
};

namespace composite_node_id_detail {

constexpr auto is_lower(char c) -> bool {
    return c >= 'a' && c <= 'z';
}
constexpr auto is_digit(char c) -> bool {
    return c >= '0' && c <= '9';
}
constexpr auto is_lower_hex(char c) -> bool {
    return is_digit(c) || (c >= 'a' && c <= 'f');
}
constexpr auto is_lower_alnum(char c) -> bool {
    return is_lower(c) || is_digit(c);
}

/// True when `s` is `min`..`max` characters, each accepted by `pred`.
template<typename Pred>
constexpr auto all_of_len(std::string_view s, std::size_t min, std::size_t max, Pred pred) -> bool {
    return s.size() >= min && s.size() <= max && std::ranges::all_of(s, pred);
}

/// Percent-encodes `:` and `%`, the two characters the canonical text
/// reserves; everything else is written as is.
inline auto encode_segment(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ':') {
            out += "%3A";
        } else if (c == '%') {
            out += "%25";
        } else {
            out += c;
        }
    }
    return out;
}

/// Decodes exactly the two escapes `encode_segment` writes. Any other `%`
/// sequence, or a lower-case spelling of these, is not canonical text.
inline auto decode_segment(std::string_view s) -> std::optional<std::string> {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        auto esc = s.substr(i, 3);
        if (esc == "%3A") {
            out += ':';
        } else if (esc == "%25") {
            out += '%';
        } else {
            return std::nullopt;
        }
        i += 2;
    }
    return out;
}

inline auto to_lower_ascii(std::string s) -> std::string {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

}  // namespace composite_node_id_detail

/// @brief One composite id type per provider, differing only in `Rules`.
///
/// `Rules` supplies `static constexpr bool fold_case` and
/// `static bool valid(std::string_view scope, std::string_view native)`.
/// The canonical text is computed once and is what equality, ordering and
/// hashing use, so the order is the same in every process.
///
/// A default-constructed id is empty and never equal to a parsed one; it
/// exists only so the type is `std::regular` and can sit in containers.
template<fixed_string Provider, typename Rules> class basic_composite_node_id {
public:
    static constexpr std::string_view provider = Provider;

    basic_composite_node_id() = default;

    /// Builds an id from a cloud API response. Throws `std::invalid_argument`
    /// when either part fails the provider's validation.
    basic_composite_node_id(std::string scope, std::string native) {
        if constexpr (Rules::fold_case) {
            scope = composite_node_id_detail::to_lower_ascii(std::move(scope));
            native = composite_node_id_detail::to_lower_ascii(std::move(native));
        }
        if (!Rules::valid(scope, native)) {
            throw std::invalid_argument(std::string(provider) + " node id: invalid scope '" +
                                        scope + "' or native id '" + native + "'");
        }
        assign(std::move(scope), std::move(native));
    }

    /// Reads canonical text. Returns nullopt, never throws, on the wrong
    /// provider, a missing or extra segment, a part the provider rejects, or
    /// any spelling other than the canonical one.
    static auto parse(std::string_view text) -> std::optional<basic_composite_node_id> {
        auto first = text.find(':');
        if (first == std::string_view::npos || text.substr(0, first) != provider) {
            return std::nullopt;
        }
        auto rest = text.substr(first + 1);
        auto second = rest.find(':');
        if (second == std::string_view::npos ||
            rest.find(':', second + 1) != std::string_view::npos) {
            return std::nullopt;
        }
        auto scope = composite_node_id_detail::decode_segment(rest.substr(0, second));
        auto native = composite_node_id_detail::decode_segment(rest.substr(second + 1));
        if (!scope || !native || !Rules::valid(*scope, *native)) {
            return std::nullopt;
        }
        basic_composite_node_id id;
        id.assign(*scope, *native);
        if (id._text != text) {
            return std::nullopt;  // e.g. upper case on a case-folding provider
        }
        return id;
    }

    [[nodiscard]] auto scope() const -> std::string_view { return _scope; }
    [[nodiscard]] auto native() const -> std::string_view { return _native; }
    [[nodiscard]] auto to_string() const -> std::string { return _text; }

    friend auto operator==(const basic_composite_node_id& a, const basic_composite_node_id& b)
        -> bool {
        return a._text == b._text;
    }
    friend auto operator<=>(const basic_composite_node_id& a, const basic_composite_node_id& b)
        -> std::strong_ordering {
        return a._text <=> b._text;
    }
    friend auto operator<<(std::ostream& os, const basic_composite_node_id& id) -> std::ostream& {
        return os << id._text;
    }

private:
    std::string _text;    // canonical text: the identity
    std::string _scope;   // decoded
    std::string _native;  // decoded

    void assign(std::string scope, std::string native) {
        _text = std::string(provider) + ":" + composite_node_id_detail::encode_segment(scope) +
                ":" + composite_node_id_detail::encode_segment(native);
        _scope = std::move(scope);
        _native = std::move(native);
    }
};

// ============================================================================
// Provider rules
// ============================================================================

namespace composite_node_id_detail {

using namespace std::string_view_literals;

/// Splits on `sep`, keeping empty pieces.
inline auto split(std::string_view s, char sep) -> std::vector<std::string_view> {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        auto pos = s.find(sep, start);
        parts.push_back(s.substr(start, pos - start));
        if (pos == std::string_view::npos) {
            return parts;
        }
        start = pos + 1;
    }
}

/// RFC 1035 label: a lower-case letter, then letters, digits or `-`, not
/// ending in `-`, at most 63 characters.
inline auto is_rfc1035_label(std::string_view s) -> bool {
    return !s.empty() && s.size() <= 63 && is_lower(s.front()) && s.back() != '-' &&
           std::ranges::all_of(s, [](char c) { return is_lower_alnum(c) || c == '-'; });
}

}  // namespace composite_node_id_detail

/// EC2: region scope (`us-east-1`, `us-gov-west-1`); native `i-` and 8 or 17
/// lower-case hex digits. A legacy 8-digit id is kept exactly as given.
struct aws_ec2_rules {
    static constexpr bool fold_case = false;
    static auto valid_region(std::string_view r) -> bool {
        using namespace composite_node_id_detail;
        auto parts = split(r, '-');
        if (parts.size() < 3 || parts.front().size() != 2 ||
            !std::ranges::all_of(parts.front(), is_lower) ||
            !all_of_len(parts.back(), 1, 2, is_digit)) {
            return false;
        }
        for (std::size_t i = 1; i + 1 < parts.size(); ++i) {
            if (!all_of_len(parts[i], 1, 32, is_lower)) {
                return false;
            }
        }
        return true;
    }
    static auto valid(std::string_view scope, std::string_view native) -> bool {
        using namespace composite_node_id_detail;
        if (!valid_region(scope) || !native.starts_with("i-")) {
            return false;
        }
        auto hex = native.substr(2);
        return (hex.size() == 8 || hex.size() == 17) && std::ranges::all_of(hex, is_lower_hex);
    }
};

/// Alibaba ECS: region id scope (`cn-hangzhou`); native `i-` and 1-64 of
/// `[a-z0-9]`.
struct alibaba_ecs_rules {
    static constexpr bool fold_case = false;
    static auto valid(std::string_view scope, std::string_view native) -> bool {
        using namespace composite_node_id_detail;
        return scope.size() >= 4 && scope.size() <= 64 && is_lower(scope[0]) &&
               is_lower(scope[1]) && scope[2] == '-' &&
               std::ranges::all_of(scope.substr(3),
                                   [](char c) { return is_lower_alnum(c) || c == '-'; }) &&
               native.starts_with("i-") && all_of_len(native.substr(2), 1, 64, is_lower_alnum);
    }
};

/// Azure VM (and VMSS Flexible members): resource group scope, VM name
/// native. Azure treats both case-insensitively, so both are folded.
struct azure_vm_rules {
    static constexpr bool fold_case = true;
    static auto valid(std::string_view scope, std::string_view native) -> bool {
        using namespace composite_node_id_detail;
        return all_of_len(scope, 1, 90,
                          [](char c) {
                              return is_lower_alnum(c) || c == '.' || c == '_' || c == '(' ||
                                     c == ')' || c == '-';
                          }) &&
               all_of_len(native, 1, 64, [](char c) {
                   return is_lower_alnum(c) || c == '.' || c == '_' || c == '-';
               });
    }
};

/// GCP Compute and MIG members: `<project>/<zone>` scope, where a
/// domain-scoped project contains `:`; instance name native.
struct gcp_instance_rules {
    static constexpr bool fold_case = false;
    static auto valid(std::string_view scope, std::string_view native) -> bool {
        using namespace composite_node_id_detail;
        auto slash = scope.find('/');
        if (slash == std::string_view::npos ||
            scope.find('/', slash + 1) != std::string_view::npos) {
            return false;
        }
        auto project = scope.substr(0, slash);
        auto zone = scope.substr(slash + 1);
        return all_of_len(
                   project, 1, 128,
                   [](char c) { return is_lower_alnum(c) || c == '-' || c == '.' || c == ':'; }) &&
               is_rfc1035_label(zone) && is_rfc1035_label(native);
    }
};

/// OCI: native `ocid1.instance.<realm>.<region>.<unique>`; the scope is that
/// OCID's region field, so the two cannot disagree.
struct oci_instance_rules {
    static constexpr bool fold_case = false;
    static auto region_of(std::string_view ocid) -> std::optional<std::string_view> {
        using namespace composite_node_id_detail;
        auto parts = split(ocid, '.');
        if (parts.size() < 5 || parts[0] != "ocid1" || parts[1] != "instance") {
            return std::nullopt;
        }
        for (auto p : parts) {
            if (!std::ranges::all_of(p, [](char c) { return is_lower_alnum(c) || c == '-'; })) {
                return std::nullopt;
            }
        }
        if (parts[2].empty() || parts[3].empty() || parts.back().empty()) {
            return std::nullopt;
        }
        return parts[3];
    }
    static auto valid(std::string_view scope, std::string_view native) -> bool {
        auto region = region_of(native);
        return native.size() <= 255 && region && *region == scope;
    }
};

/// Docker: compose project or cluster name scope; container name native.
struct docker_container_rules {
    static constexpr bool fold_case = false;
    static auto valid(std::string_view scope, std::string_view native) -> bool {
        using namespace composite_node_id_detail;
        return all_of_len(scope, 1, 63,
                          [](char c) { return is_lower_alnum(c) || c == '_' || c == '-'; }) &&
               !native.empty() && native.size() <= 255 &&
               (is_lower_alnum(native[0]) || (native[0] >= 'A' && native[0] <= 'Z')) &&
               std::ranges::all_of(native, [](char c) {
                   return is_lower_alnum(c) || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' ||
                          c == '-';
               });
    }
};

using aws_ec2_node_id = basic_composite_node_id<"aws-ec2", aws_ec2_rules>;
using alibaba_ecs_node_id = basic_composite_node_id<"alibaba-ecs", alibaba_ecs_rules>;
using azure_vm_node_id = basic_composite_node_id<"azure-vm", azure_vm_rules>;
using gcp_instance_node_id = basic_composite_node_id<"gcp", gcp_instance_rules>;
using oci_instance_node_id = basic_composite_node_id<"oci", oci_instance_rules>;
using docker_container_node_id = basic_composite_node_id<"docker", docker_container_rules>;

}  // namespace kythira

template<kythira::fixed_string Provider, typename Rules>
struct std::hash<kythira::basic_composite_node_id<Provider, Rules>> {
    auto operator()(const kythira::basic_composite_node_id<Provider, Rules>& id) const noexcept
        -> std::size_t {
        return std::hash<std::string>{}(id.to_string());
    }
};

namespace kythira {

static_assert(composite_node_id<aws_ec2_node_id>);
static_assert(composite_node_id<alibaba_ecs_node_id>);
static_assert(composite_node_id<azure_vm_node_id>);
static_assert(composite_node_id<gcp_instance_node_id>);
static_assert(composite_node_id<oci_instance_node_id>);
static_assert(composite_node_id<docker_container_node_id>);

// ============================================================================
// node_id_traits
// ============================================================================

/// @brief Prints and parses every kind of node id, so callers stop carrying
///        their own string-versus-integer branches.
template<typename N> struct node_id_traits;

/// Numeric ids: decimal text, parsed strictly.
template<std::unsigned_integral N> struct node_id_traits<N> {
    static constexpr bool is_textual = false;
    static auto to_text(N id) -> std::string { return std::to_string(id); }

    /// A non-empty run of ASCII digits whose value fits `N`. Signs,
    /// whitespace, `0x` and trailing characters are refused, unlike
    /// `std::stoull`, which reads "-1" as the largest value and "7x" as 7.
    static auto from_text(std::string_view text) -> std::optional<N> {
        if (text.empty() ||
            !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        N value{};
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || end != text.data() + text.size()) {
            return std::nullopt;
        }
        return value;
    }
};

/// `std::string` ids: the text is the id. Empty text is refused.
template<> struct node_id_traits<std::string> {
    static constexpr bool is_textual = true;
    static auto to_text(const std::string& id) -> std::string { return id; }
    static auto from_text(std::string_view text) -> std::optional<std::string> {
        if (text.empty()) {
            return std::nullopt;
        }
        return std::string(text);
    }
};

/// Composite ids: canonical text.
template<composite_node_id N> struct node_id_traits<N> {
    static constexpr bool is_textual = true;
    static auto to_text(const N& id) -> std::string { return id.to_string(); }
    static auto from_text(std::string_view text) -> std::optional<N> { return N::parse(text); }
};

/// @brief `max_seen + 1`, or `std::overflow_error` at the type's maximum,
///        never wrapping to zero.
template<std::unsigned_integral N> auto next_numeric_node_id(N max_seen) -> N {
    if (max_seen == std::numeric_limits<N>::max()) {
        throw std::overflow_error("next_numeric_node_id: node id space exhausted at " +
                                  std::to_string(max_seen));
    }
    return static_cast<N>(max_seen + 1);
}

}  // namespace kythira
