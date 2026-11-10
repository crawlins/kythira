// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_static_keys.hpp
/// @brief The static access-key file a gateway without IAM authenticates
///        against (`.kiro/specs/kinesis-compatible-streams/` task 6,
///        `KINESIS_STATIC_KEYS_FILE`).
///
/// One key per line, five fields separated by blanks:
///
///     # access-key-id  secret  account       name  role
///     AKIAEXAMPLE0001  s3cr3t  111122223333  ci    read-write
///
/// `role` is `read-only`, `read-write` or `admin`
/// (`static_key_authorizer.hpp`). Blank lines and lines starting with `#`
/// are skipped. A malformed line fails the whole file with its line number
/// and without echoing the line, which holds a secret.

#include <raft/static_key_authorizer.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::kinesis {

[[nodiscard]] inline auto parse_static_role(std::string_view s)
    -> std::optional<aws_auth::static_role> {
    if (s == "read-only") {
        return aws_auth::static_role::read_only;
    }
    if (s == "read-write") {
        return aws_auth::static_role::read_write;
    }
    if (s == "admin") {
        return aws_auth::static_role::admin;
    }
    return std::nullopt;
}

/// @throws std::invalid_argument naming the first bad line.
[[nodiscard]] inline auto parse_static_keys(std::string_view text)
    -> std::vector<aws_auth::static_key> {
    std::vector<aws_auth::static_key> out;
    std::size_t line_no = 0;
    while (!text.empty()) {
        ++line_no;
        const auto nl = text.find('\n');
        auto line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        std::vector<std::string_view> fields;
        std::size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])) != 0) {
                ++i;
            }
            const auto start = i;
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])) == 0) {
                ++i;
            }
            if (i > start) {
                fields.push_back(line.substr(start, i - start));
            }
        }
        if (fields.empty() || fields.front().starts_with('#')) {
            continue;
        }
        const auto where = "static keys line " + std::to_string(line_no) + ": ";
        if (fields.size() != 5) {
            throw std::invalid_argument(where + "expected 5 fields (access key id, secret, " +
                                        "account, name, role), found " +
                                        std::to_string(fields.size()));
        }
        const auto account = fields[2];
        if (account.size() != 12 || !std::all_of(account.begin(), account.end(),
                                                 [](char c) { return c >= '0' && c <= '9'; })) {
            throw std::invalid_argument(where + "account must be 12 digits");
        }
        const auto role = parse_static_role(fields[4]);
        if (!role) {
            throw std::invalid_argument(where + "role must be read-only, read-write or admin");
        }
        out.push_back(aws_auth::static_key{
            .access_key_id = std::string(fields[0]),
            .secret = std::string(fields[1]),
            .account = std::string(account),
            .name = std::string(fields[3]),
            .role = *role,
        });
    }
    if (out.empty()) {
        throw std::invalid_argument("static keys: no keys");
    }
    return out;
}

}  // namespace kythira::kinesis
