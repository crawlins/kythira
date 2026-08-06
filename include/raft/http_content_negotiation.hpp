#pragma once

/// @file http_content_negotiation.hpp
/// @brief Parsing of the HTTP `Accept` header into the ordered media-type list
///        that `serializer_registry::select_output_media_type` consumes.
///
/// One free function in a shared header rather than a method on either server,
/// because `cpp_httplib_server` and `boost_beast_server` both need exactly this
/// and their header APIs differ only in how the raw string is obtained. A
/// `q`-value bug fixed in one copy but not the other would be invisible until a
/// peer used the transport that still had it.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace kythira {

/// @brief Splits an HTTP `Accept` header value into media types, most-preferred
///        first, with parameters stripped.
///
/// `"application/cbor;q=0.9, application/json"` yields
/// `{"application/cbor", "application/json"}`.
///
/// Parameters after `;` are dropped rather than interpreted, which means
/// **`q`-values do not reorder the result** — source order is preserved as-is.
/// That is a deliberate limitation, not an oversight. RFC 9110 §12.5.1 ranks by
/// `q`, but this codebase's own clients emit `preferred_media_types()` in
/// preference order with no `q` at all, so honouring `q` would change nothing
/// for them while adding a parser that could disagree between the two servers.
/// A peer that ranks by `q` still gets a type it accepts — just possibly not its
/// top choice. Revisit if a real peer needs strict ranking; the tests pin the
/// current behaviour so the change would be visible.
///
/// Wildcards (`*/*`, `type/*`) are returned verbatim rather than expanded;
/// matching them belongs to the registry (`accept_entry_matches`), which is the
/// only place that knows what types there are to expand *to*. `*/*` therefore
/// resolves to the registry's default and `type/*` to its first match within
/// that top-level type, rather than matching nothing.
///
/// Empty entries (from `"a,,b"` or a trailing comma) are skipped, so a
/// malformed header degrades to the types it did contain rather than
/// introducing an empty media type that can never match.
[[nodiscard]] inline auto parse_accept_header(std::string_view header) -> std::vector<std::string> {
    std::vector<std::string> result;
    std::size_t pos = 0;
    while (pos <= header.size()) {
        const auto comma = header.find(',', pos);
        auto entry = header.substr(
            pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        // Drop `;q=…` and any other parameters.
        if (const auto semi = entry.find(';'); semi != std::string_view::npos) {
            entry = entry.substr(0, semi);
        }
        // Trim surrounding whitespace (`, ` between entries is the norm).
        const auto first = entry.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            const auto last = entry.find_last_not_of(" \t");
            result.emplace_back(entry.substr(first, last - first + 1));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return result;
}

/// @brief Renders `preferred_media_types()` as an `Accept` header value.
///
/// Comma-separated in preference order, with no `q` values. Emitting no `q` is
/// the deliberate counterpart to `parse_accept_header` not honouring it: order
/// alone carries the preference, both sides agree on that reading, and there is
/// no second encoding of the same intent to fall out of sync. RFC 9110 §12.5.1
/// treats a list without `q` as all-equally-acceptable, and a peer that reads it
/// that way still picks something this client can decode — which is the only
/// guarantee that matters.
///
/// An empty list yields an empty string; callers omit the header entirely in
/// that case rather than sending `Accept:`, since an empty value means "accept
/// nothing" to a strict peer and would earn a 406.
[[nodiscard]] inline auto format_accept_header(const std::vector<std::string>& media_types)
    -> std::string {
    std::string result;
    for (const auto& media_type : media_types) {
        if (!result.empty()) {
            result += ", ";
        }
        result += media_type;
    }
    return result;
}

/// @brief Strips parameters from a single `Content-Type` value.
///
/// `"application/json; charset=utf-8"` yields `"application/json"`. Servers and
/// clients both need this: a registry dispatches on the bare media type, and a
/// peer is entitled to send a charset or boundary parameter alongside it.
/// Failing to strip would turn an ordinary, spec-legal request into a 415.
[[nodiscard]] inline auto strip_media_type_parameters(std::string_view content_type)
    -> std::string {
    if (const auto semi = content_type.find(';'); semi != std::string_view::npos) {
        content_type = content_type.substr(0, semi);
    }
    const auto first = content_type.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = content_type.find_last_not_of(" \t");
    return std::string{content_type.substr(first, last - first + 1)};
}

}  // namespace kythira
