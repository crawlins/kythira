// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_endpoint_override.hpp
/// @brief Shape check for an S3 endpoint override given on a command line,
///        and the warning for the endpoint variables the linked SDK ignores
///        (`.kiro/specs/object-backup-s3-endpoint/`, Requirement 3).
///
/// SDK free on purpose: the rules decide only what can be decided without the
/// network (scheme, host, port, path, query, fragment, userinfo), so a
/// mistyped endpoint is a configuration error, exit 1, before any connection.
/// Reachability is left to the first request. Named for the setting rather
/// than for one tool, so any `cmd/` binary that grows an S3 endpoint option
/// can reuse it.

#include <raft/env_config_result.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kythira {

/// Result of validate_s3_endpoint().
struct endpoint_check {
    /// Empty when the endpoint is usable. Names `--endpoint` and the rule.
    std::string error;
    /// Non-fatal notes, e.g. plaintext to a non-loopback host.
    std::vector<std::string> warnings;
    /// The value to hand the SDK: the input with one trailing `/` removed.
    /// Meaningful only when ok().
    std::string normalised;

    [[nodiscard]] auto ok() const -> bool { return error.empty(); }
};

namespace detail {

[[nodiscard]] inline auto ascii_lower(std::string_view s) -> std::string {
    std::string out(s);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Loopback by spelling only (Requirement 3.4): `localhost`, any `127.x.x.x`
/// literal, and `[::1]`. A name that merely resolves to loopback still gets
/// the plaintext warning, which errs on the side of saying too much.
[[nodiscard]] inline auto is_textual_loopback(std::string_view host) -> bool {
    const auto lower = ascii_lower(host);
    if (lower == "localhost" || lower == "[::1]") {
        return true;
    }
    if (!lower.starts_with("127.")) {
        return false;
    }
    int octets = 0;
    std::string_view rest(lower);
    while (true) {
        const auto dot = rest.find('.');
        const auto part = rest.substr(0, dot);
        unsigned value = 0;
        const auto* end = part.data() + part.size();
        const auto [ptr, ec] = std::from_chars(part.data(), end, value);
        if (part.empty() || part.size() > 3 || ec != std::errc{} || ptr != end || value > 255) {
            return false;
        }
        ++octets;
        if (dot == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(dot + 1);
    }
    return octets == 4;
}

/// A DNS name or IPv4 literal: letters, digits, hyphens and dots only, no
/// empty label. Anything else (a colon above all, i.e. unbracketed IPv6) is
/// rejected.
[[nodiscard]] inline auto is_plain_host(std::string_view host) -> bool {
    if (host.empty() || host.front() == '.' || host.back() == '.' ||
        host.find("..") != std::string_view::npos) {
        return false;
    }
    return std::ranges::all_of(
        host, [](unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '.'; });
}

/// The inside of the brackets: hex digits, colons and dots (an embedded IPv4
/// tail), with at least one colon.
[[nodiscard]] inline auto is_bracketed_ipv6(std::string_view host) -> bool {
    if (host.size() < 4 || host.front() != '[' || host.back() != ']') {
        return false;
    }
    const auto inner = host.substr(1, host.size() - 2);
    return inner.find(':') != std::string_view::npos &&
           std::ranges::all_of(inner, [](unsigned char c) {
               return std::isxdigit(c) != 0 || c == ':' || c == '.';
           });
}

}  // namespace detail

/// Checks the shape of an S3 endpoint override (Requirement 3.1). Never
/// resolves or connects. Never echoes a value that carries userinfo
/// (Requirement 3.2), since that text may be a password.
[[nodiscard]] inline auto validate_s3_endpoint(std::string_view endpoint) -> endpoint_check {
    endpoint_check check;
    const std::string shown = "'" + std::string(endpoint) + "'";
    auto reject = [&](std::string why) {
        check.error = "--endpoint " + shown + ": " + std::move(why);
        return check;
    };

    const auto scheme_end = endpoint.find("://");
    if (scheme_end == std::string_view::npos) {
        return reject(
            "needs a scheme, http:// or https://, so the choice between TLS "
            "and plaintext is never implicit");
    }
    const auto scheme = detail::ascii_lower(endpoint.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https") {
        return reject("scheme must be http or https");
    }

    auto rest = endpoint.substr(scheme_end + 3);
    // The authority ends at the first '/', '?' or '#'.
    const auto authority_end = rest.find_first_of("/?#");
    const auto authority = rest.substr(0, authority_end);
    const auto tail =
        authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);

    if (authority.find('@') != std::string_view::npos) {
        // Do not print the value: what precedes '@' may be a password.
        check.error =
            "--endpoint: the value contains userinfo (user:password@), which is "
            "not accepted; credentials come from the AWS SDK chain only. The "
            "value is not repeated here because it may hold a secret";
        return check;
    }
    if (tail.find('#') != std::string_view::npos) {
        return reject("a fragment (#...) is not accepted");
    }
    if (tail.find('?') != std::string_view::npos) {
        return reject("a query (?...) is not accepted");
    }
    if (!tail.empty() && tail != "/") {
        return reject(
            "a path is not accepted; the client puts the bucket in the path, so "
            "a path here would prefix every key");
    }

    std::string_view host = authority;
    std::string_view port;
    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return reject("an IPv6 literal must be enclosed in [ ]");
        }
        host = authority.substr(0, close + 1);
        const auto after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') {
                return reject("unexpected text after the IPv6 literal");
            }
            port = after.substr(1);
            if (port.empty()) {
                return reject("the port after ':' is empty");
            }
        }
        if (!detail::is_bracketed_ipv6(host)) {
            return reject("'" + std::string(host) + "' is not an IPv6 literal");
        }
    } else {
        const auto colon = authority.find(':');
        if (colon != std::string_view::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
            if (port.find(':') != std::string_view::npos) {
                return reject("an IPv6 literal must be enclosed in [ ]");
            }
            if (port.empty()) {
                return reject("the port after ':' is empty");
            }
        }
        if (host.empty()) {
            return reject("the host is empty");
        }
        if (!detail::is_plain_host(host)) {
            return reject("'" + std::string(host) + "' is not a DNS name or IP literal");
        }
    }

    if (!port.empty()) {
        std::uint32_t value = 0;
        const auto* end = port.data() + port.size();
        const auto [ptr, ec] = std::from_chars(port.data(), end, value);
        if (ec != std::errc{} || ptr != end) {
            return reject("the port must be a number from 1 to 65535");
        }
        if (value < 1 || value > 65535) {
            return reject("the port must be from 1 to 65535");
        }
    }

    if (scheme == "http" && !detail::is_textual_loopback(host)) {
        check.warnings.push_back("--endpoint " + shown +
                                 " is plaintext http to a non-loopback host; object data, "
                                 "including the Raft log, will travel unencrypted");
    }

    check.normalised = std::string(endpoint);
    if (check.normalised.ends_with('/')) {
        check.normalised.pop_back();
    }
    return check;
}

/// Warnings for `AWS_ENDPOINT_URL` / `AWS_ENDPOINT_URL_S3` when either is set
/// and no `--endpoint` was given (Requirement 3.3). The linked SDK does not
/// read them, and neither does the tool: CI points `AWS_ENDPOINT_URL` at an
/// unrelated build cache, so honouring it would aim a restore at the wrong
/// cloud. Names the variable, never its value.
[[nodiscard]] inline auto ignored_endpoint_variable_warnings(const env_lookup& env)
    -> std::vector<std::string> {
    std::vector<std::string> warnings;
    for (const char* name : {"AWS_ENDPOINT_URL", "AWS_ENDPOINT_URL_S3"}) {
        if (env_value(env, name)) {
            warnings.push_back(std::string(name) +
                               " is set but ignored; this tool talks to AWS's own S3 "
                               "endpoints unless --endpoint is given");
        }
    }
    return warnings;
}

}  // namespace kythira
