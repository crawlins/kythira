// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file grpc_target.hpp
/// @brief Decides whether a gRPC target string can only reach this host
/// (.kiro/specs/grpc-plaintext-opt-in/, Requirement 3.3, 3.4).
///
/// Kept apart from grpc_transport_impl.hpp so it needs nothing from gRPC:
/// the classification is pure string parsing plus the project's own
/// loopback rule (net_bind::is_loopback_bind_address()), and its unit test
/// runs on every build leg, with or without gRPC installed.

#include <raft/net_bind.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace kythira::grpc_detail {

/// @brief Whether an IP literal is a loopback address: 127.0.0.0/8, `::1`,
/// or the IPv4-mapped `0:0:0:0:0:ffff:127.x.y.z`, which a client dials
/// over IPv4 loopback. Returns `std::nullopt` when @p text is not an IP literal.
///
/// The mapped form counts here but not for bind addresses, where an
/// IPV6_V6ONLY listener cannot accept it anyway.
inline auto ip_literal_is_loopback(const std::string& text) -> std::optional<bool> {
    auto ep = net_bind::parse_hosts_address(text);
    if (!ep) return std::nullopt;
    if (net_bind::endpoint_is_loopback(*ep)) return true;
    if (ep->addr.ss_family == AF_INET6) {
        const auto& a = reinterpret_cast<const sockaddr_in6&>(ep->addr).sin6_addr;
        return IN6_IS_ADDR_V4MAPPED(&a) && a.s6_addr[12] == 127;
    }
    return false;
}

/// @brief Whether a host part (no port, no brackets) names only this host.
/// An IP literal must be loopback; a name must map only to loopback in the
/// hosts file. DNS is never consulted.
inline auto host_is_local(const std::string& host, const std::string& hosts_path) -> bool {
    if (host.empty()) return false;
    if (auto literal = ip_literal_is_loopback(host)) return *literal;
    return net_bind::is_loopback_bind_address(host, hosts_path);
}

/// @brief Splits `host[:port]` or `[v6][:port]` and returns the host. A port,
/// when present, must be all digits. More than one colon outside brackets
/// means a bare IPv6 literal with no port. Malformed input yields
/// `std::nullopt`, which every caller treats as not local.
inline auto target_host(std::string_view host_port) -> std::optional<std::string> {
    auto digits = [](std::string_view port) {
        return !port.empty() && std::all_of(port.begin(), port.end(),
                                            [](unsigned char c) { return std::isdigit(c); });
    };
    if (host_port.starts_with('[')) {
        auto close = host_port.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        auto rest = host_port.substr(close + 1);
        if (!rest.empty() && !(rest.front() == ':' && digits(rest.substr(1)))) {
            return std::nullopt;
        }
        return std::string(host_port.substr(1, close - 1));
    }
    if (std::count(host_port.begin(), host_port.end(), ':') > 1) {
        return std::string(host_port);
    }
    auto colon = host_port.find(':');
    if (colon == std::string_view::npos) return std::string(host_port);
    if (!digits(host_port.substr(colon + 1))) return std::nullopt;
    return std::string(host_port.substr(0, colon));
}

/// @brief True when every endpoint a gRPC target string can reach is on this
/// host: loopback IPs, hosts-file names that map only to loopback, or a local
/// socket scheme. Never resolves through DNS, and an unknown scheme or a
/// malformed target is not local (fail closed).
///
/// | Target form                         | Rule                               |
/// |-------------------------------------|------------------------------------|
/// | `unix:`, `unix-abstract:`, `vsock:` | local                              |
/// | `ipv4:a:p[,a:p...]`                 | every `a` in 127.0.0.0/8           |
/// | `ipv6:[a]:p[,...]`                  | every `a` is `::1` (or v4-mapped)  |
/// | `dns:[//auth/]host[:port]`          | the host is local                  |
/// | `[v6]:port`, `host:port`            | the host is local                  |
/// | anything else                       | not local                          |
///
/// A string is a URI only when the text before its first `:` is one of the
/// scheme names above, as gRPC decides it; any other string is a bare
/// `host:port`, which gRPC hands to its `dns` resolver.
inline auto target_is_local(std::string_view target, const std::string& hosts_path = "/etc/hosts")
    -> bool {
    if (target.empty()) return false;

    auto colon = target.find(':');
    auto scheme = colon == std::string_view::npos ? std::string_view{} : target.substr(0, colon);
    auto rest = colon == std::string_view::npos ? std::string_view{} : target.substr(colon + 1);

    constexpr std::array<std::string_view, 3> local_schemes{"unix", "unix-abstract", "vsock"};
    if (std::ranges::find(local_schemes, scheme) != local_schemes.end()) return true;

    if (scheme == "ipv4" || scheme == "ipv6") {
        if (rest.empty()) return false;
        const int family = scheme == "ipv4" ? AF_INET : AF_INET6;
        while (true) {
            auto comma = rest.find(',');
            auto host = target_host(rest.substr(0, comma));
            if (!host) return false;
            auto ep = net_bind::parse_hosts_address(*host);
            if (!ep || ep->addr.ss_family != family ||
                !ip_literal_is_loopback(*host).value_or(false)) {
                return false;
            }
            if (comma == std::string_view::npos) return true;
            rest = rest.substr(comma + 1);
        }
    }

    if (scheme == "dns") {
        if (rest.starts_with("//")) {
            auto slash = rest.find('/', 2);
            if (slash == std::string_view::npos) return false;
            rest = rest.substr(slash + 1);
        }
        auto host = target_host(rest);
        return host && host_is_local(*host, hosts_path);
    }

    // Not a URI gRPC understands without a plugin. A "scheme://..." shape
    // (xds:///svc, a custom resolver) leads somewhere this code cannot know.
    if (rest.starts_with("//")) return false;

    auto host = target_host(target);
    return host && host_is_local(*host, hosts_path);
}

}  // namespace kythira::grpc_detail
