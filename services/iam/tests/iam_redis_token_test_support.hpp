// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// An ElastiCache IAM auth token presigned in a test, the way botocore
// writes one (tools/gen_redis_iam_token.py): for the iam_redis_verifier
// cases its fixed vector cannot cover, and for the embedded replica test.

#include <raft/sigv4_verifier.hpp>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace iam_redis_testing {

namespace sigv4 = kythira::sigv4;

/// An ElastiCache token presigned here, for the cases botocore's fixed
/// vector cannot cover (session credentials, other lifetimes and users).
inline auto presign(std::string_view key_id, std::string_view secret, std::int64_t at,
                    std::string_view user = "farm", int expires = 900,
                    std::string_view host = "kythira-cache", const std::string& session_token = {},
                    bool unsigned_payload = false, std::string_view region = "us-east-1")
    -> std::string {
    const auto when = std::chrono::sys_seconds{std::chrono::seconds{at}};
    const auto amz = sigv4::detail::format_amz_date(when);
    const auto enc = [](std::string_view s) { return sigv4::detail::uri_encode(s, false); };
    std::string query = "Action=connect&User=" + enc(user) +
                        "&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=" +
                        enc(std::string(key_id) + "/" + amz.substr(0, 8) + "/" +
                            std::string(region) + "/elasticache/aws4_request") +
                        "&X-Amz-Date=" + amz + "&X-Amz-Expires=" + std::to_string(expires);
    if (!session_token.empty()) {
        query += "&X-Amz-Security-Token=" + enc(session_token);
    }
    query += "&X-Amz-SignedHeaders=host&X-Amz-Signature=";
    const auto unsigned_query = query + std::string(64, '0');
    sigv4::request r;
    r.method = "GET";
    r.raw_path = "/";
    r.raw_query = unsigned_query;
    r.headers = {{"host", std::string(host)}};
    if (unsigned_payload) {
        r.headers.emplace_back("x-amz-content-sha256", "UNSIGNED-PAYLOAD");
    }
    const auto parsed = sigv4::parse(r, {.service = "elasticache"}, when);
    BOOST_TEST_REQUIRE(parsed.has_value(), parsed.error().message);
    const auto sig = sigv4::compute_signature(
        *parsed, sigv4::derive_signing_key(std::string(secret), parsed->scope));
    return std::string(host) + "/?" + query + sig;
}

}  // namespace iam_redis_testing
