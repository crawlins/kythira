// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_routing.hpp
/// @brief How `iam_node` routes a request on its HTTPS listener
///        (`.kiro/specs/iam-compatible-auth/` design Component 7):
///        `/healthz`, the `/v1/admin/*` routes for admin certificates, and
///        everything else by the service in the SigV4 credential scope.
///
/// The Query API handlers arrive with tasks 14 (IAM) and 17 (STS); until
/// then an IAM or STS request is answered `NotImplemented` in AWS's Query
/// error shape, so SDKs show a readable error. `aws_query_protocol.hpp`
/// (task 14) takes over the XML writing.

#include <kythira/iam/iam_seal.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::iam::routing {

/// Where a request goes.
enum class route : std::uint8_t {
    health,
    admin,
    iam,
    sts,
    unknown_service,
    unauthenticated,
};

/// The credential-scope service of a SigV4 request: from the Authorization
/// header's `Credential=<key>/<date>/<region>/<service>/aws4_request`, or a
/// presigned URL's `X-Amz-Credential`. Nothing for an unsigned request.
/// Only routes: the verifier checks the scope properly.
[[nodiscard]] inline auto scope_service(std::string_view authorization, std::string_view query)
    -> std::optional<std::string> {
    auto service_of = [](std::string_view credential) -> std::optional<std::string> {
        // <key>/<date>/<region>/<service>/aws4_request
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        while (true) {
            const auto slash = credential.find('/', start);
            parts.push_back(credential.substr(start, slash - start));
            if (slash == std::string_view::npos) {
                break;
            }
            start = slash + 1;
        }
        if (parts.size() != 5 || parts[4] != "aws4_request" || parts[3].empty()) {
            return std::nullopt;
        }
        return std::string(parts[3]);
    };
    if (authorization.starts_with("AWS4-HMAC-SHA256 ")) {
        const auto at = authorization.find("Credential=");
        if (at == std::string_view::npos) {
            return std::nullopt;
        }
        auto rest = authorization.substr(at + 11);
        const auto end = rest.find_first_of(", ");
        return service_of(rest.substr(0, end));
    }
    // X-Amz-Credential=<key>%2F<date>%2F<region>%2F<service>%2Faws4_request
    static constexpr std::string_view key = "X-Amz-Credential=";
    std::size_t pos = 0;
    while (pos < query.size()) {
        const auto amp = query.find('&', pos);
        const auto param = query.substr(pos, amp - pos);
        if (param.starts_with(key)) {
            std::string decoded;
            const auto value = param.substr(key.size());
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '%' && i + 2 < value.size() && value.substr(i + 1, 2) == "2F") {
                    decoded += '/';
                    i += 2;
                } else {
                    decoded += value[i];
                }
            }
            return service_of(decoded);
        }
        if (amp == std::string_view::npos) {
            break;
        }
        pos = amp + 1;
    }
    return std::nullopt;
}

/// Routes by path, then by credential scope.
[[nodiscard]] inline auto route_of(std::string_view path, std::string_view authorization,
                                   std::string_view query) -> route {
    if (path == "/healthz") {
        return route::health;
    }
    if (path.starts_with("/v1/admin/")) {
        return route::admin;
    }
    const auto service = scope_service(authorization, query);
    if (!service) {
        return route::unauthenticated;
    }
    if (*service == "iam") {
        return route::iam;
    }
    if (*service == "sts") {
        return route::sts;
    }
    return route::unknown_service;
}

/// Whether a verified client certificate carries one of the admin URI SANs.
/// An empty admin set admits nobody.
[[nodiscard]] inline auto admin_allowed(bool verified, const std::vector<std::string>& uri_sans,
                                        const std::set<std::string>& admins) -> bool {
    return verified &&
           std::ranges::any_of(uri_sans, [&](const std::string& u) { return admins.contains(u); });
}

/// A request id in the UUID shape AWS returns.
[[nodiscard]] inline auto request_id() -> std::string {
    const auto b = random_bytes(16);
    static constexpr std::string_view hex = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out += '-';
        }
        out += hex[b[i] >> 4];
        out += hex[b[i] & 0x0F];
    }
    return out;
}

[[nodiscard]] inline auto xml_escape(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

/// AWS's Query error document. `sts` picks STS's namespace over IAM's;
/// `sender` is the fault type (Sender for client errors, Receiver for
/// server ones).
[[nodiscard]] inline auto query_error_xml(bool sts, bool sender, std::string_view code,
                                          std::string_view message, std::string_view request)
    -> std::string {
    const std::string_view ns = sts ? "https://sts.amazonaws.com/doc/2011-06-15/"
                                    : "https://iam.amazonaws.com/doc/2010-05-08/";
    return "<ErrorResponse xmlns=\"" + std::string(ns) + "\"><Error><Type>" +
           (sender ? "Sender" : "Receiver") + "</Type><Code>" + xml_escape(code) +
           "</Code><Message>" + xml_escape(message) + "</Message></Error><RequestId>" +
           xml_escape(request) + "</RequestId></ErrorResponse>";
}

}  // namespace kythira::iam::routing
