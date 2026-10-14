// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// When a client that carries a secret may talk plain http. Shared by every
// helper that sends credentials or fetches trust material over cpp-httplib
// (ACME, the ACE token request), so they draw the line in the same place:
// https anywhere, plain http only to a loopback host, where an attacker able
// to read the traffic already owns the node.

#include <arpa/inet.h>
#include <netinet/in.h>

#include <string>
#include <string_view>

namespace kythira::http_origin {

inline constexpr std::string_view https_scheme = "https://";
inline constexpr std::string_view http_scheme = "http://";

// True for "localhost", any 127.0.0.0/8 address and ::1 (bracketed or not).
// Names other than "localhost" are not resolved: what a name resolves to is
// up to DNS, which is exactly what an attacker on the path controls.
[[nodiscard]] inline auto is_loopback_host(std::string host) -> bool {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    if (host == "localhost") {
        return true;
    }
    in_addr v4{};
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        return (ntohl(v4.s_addr) >> 24) == 127;
    }
    in6_addr v6{};
    return inet_pton(AF_INET6, host.c_str(), &v6) == 1 && IN6_IS_ADDR_LOOPBACK(&v6);
}

// The host of a "scheme://[userinfo@]host[:port][/...]" URL, brackets kept
// on an IPv6 literal. Empty when `url` has no "://".
[[nodiscard]] inline auto host_of(std::string_view url) -> std::string {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return {};
    }
    auto authority = url.substr(scheme_end + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (auto at = authority.rfind('@'); at != std::string_view::npos) {
        authority = authority.substr(at + 1);
    }
    if (authority.starts_with('[')) {
        auto close = authority.find(']');
        return close == std::string_view::npos ? std::string{}
                                               : std::string(authority.substr(0, close + 1));
    }
    return std::string(authority.substr(0, authority.find(':')));
}

[[nodiscard]] inline auto is_https(std::string_view url) -> bool {
    return url.starts_with(https_scheme);
}

// Plain http to a loopback host: the only cleartext origin any caller may
// accept, and then only where it has said so.
[[nodiscard]] inline auto is_loopback_http(std::string_view url) -> bool {
    return url.starts_with(http_scheme) && is_loopback_host(host_of(url));
}

}  // namespace kythira::http_origin
