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
/// Wildcards (`*/*`, `type/*`) are returned verbatim rather than expanded. A
/// registry matches on exact tokens, so `*/*` simply matches nothing and the
/// caller falls through to its default — the same outcome an absent `Accept`
/// produces, which is the right answer for "I'll take anything".
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

}  // namespace kythira
