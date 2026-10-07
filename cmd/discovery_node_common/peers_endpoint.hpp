// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The GET /peers handler shared by the poco, dns and dns_sd discovery nodes
// (the Docker integration fixtures under docker/*-discovery-compose.yml).
//
// Each node used to copy the same handler, which had two faults: it passed
// any `timeout_ms` straight to find_peers(), so one request could pin an
// HTTP worker for up to INT_MAX ms (about 24 days), and it spliced peer ids
// and addresses into the JSON unescaped. Both values come from DNS records
// that anything allowed to update the zone (or answer mDNS) can write, so a
// peer id containing `"` produced invalid JSON or injected extra fields.
//
// Header-only and free of httplib so tests/discovery_peers_endpoint_test.cpp
// can cover it without a DNS backend.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace kythira::discovery_node {

// Applied when the request has no `timeout_ms`.
inline constexpr std::chrono::milliseconds default_peers_timeout{2000};
// Upper bound on one browse. The scenario tests ask for 3 s.
inline constexpr std::chrono::milliseconds max_peers_timeout{10000};

// Parses the `timeout_ms` query parameter. An absent parameter gives the
// default; a value outside [1, max_peers_timeout] is clamped into it. Returns
// nullopt for anything that is not a plain decimal integer, which the caller
// answers with 400.
[[nodiscard]] inline auto parse_peers_timeout(std::optional<std::string_view> param)
    -> std::optional<std::chrono::milliseconds> {
    if (!param) {
        return default_peers_timeout;
    }
    long long value = 0;
    const char* first = param->data();
    const char* last = first + param->size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (param->empty() || ec == std::errc::invalid_argument || ptr != last) {
        return std::nullopt;
    }
    if (ec == std::errc::result_out_of_range) {
        // from_chars leaves `value` untouched; the sign says which end.
        value = (*param)[0] == '-' ? 1 : max_peers_timeout.count();
    }
    return std::chrono::milliseconds{std::clamp<long long>(value, 1, max_peers_timeout.count())};
}

// Appends `s` to `out` as the body of a JSON string (RFC 8259 section 7).
// Bytes >= 0x80 pass through unchanged: the values are UTF-8 from DNS labels
// or TXT data, and re-encoding them is the client's concern.
inline void append_json_string(std::string& out, std::string_view s) {
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

// Renders find_peers() results as [{"id":"...","address":"..."}, ...].
// `Peers` is any range of kythira::peer_info whose node_id and address
// convert to std::string_view.
template<typename Peers> [[nodiscard]] auto peers_to_json(const Peers& peers) -> std::string {
    std::string json = "[";
    bool first = true;
    for (const auto& p : peers) {
        if (!first) {
            json += ',';
        }
        first = false;
        json += R"({"id":")";
        append_json_string(json, p.node_id);
        json += R"(","address":")";
        append_json_string(json, p.address);
        json += R"("})";
    }
    json += ']';
    return json;
}

}  // namespace kythira::discovery_node
