// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_redis_verifier.hpp
/// @brief IAM's adapter to core's `credential_verifier` concept for the
///        Redis gateway's `AUTH` (`.kiro/specs/iam-compatible-auth/` task 21,
///        Requirement 13, design Component 11).
///
/// The credential is an ElastiCache IAM auth token: a SigV4 presigned
/// request for `GET http://<cache name>/?Action=connect&User=<user>`, service
/// `elasticache`, with the scheme left off. The AWS SDKs' own presigners
/// make it, e.g. botocore's `RequestSigner.generate_presigned_url` as the
/// ElastiCache documentation shows. `verify` answers from the local IAM
/// state (a replica, Requirement 13.6), so an `AUTH` makes no network call:
///
///   1. split `<host>/?<query>` and check `Action=connect`, that `User` is
///      the `AUTH` user and the host the configured cache name, and that
///      the token was presigned for at most 15 minutes;
///   2. authenticate the presigned request (long-term or temporary
///      credentials). botocore hashes the empty body into the canonical
///      request, as SigV4 does for every presigned URL outside S3; a
///      signature over `UNSIGNED-PAYLOAD`, S3's form, is accepted too, so
///      an SDK that presigns every service that way still works;
///   3. authorize `elasticache:Connect` on the user ARN and on the
///      replication group ARN, both of which must allow;
///   4. accept until the earlier of 12 hours from now and the session's own
///      expiry.
///
/// The Redis module never includes this header: redis_gateway_node's
/// composition root wires it in when built with `CONFIG_REDIS_GATEWAY_IAM`.
/// A denial's reason goes to the gateway's audit log, never to the client,
/// and never carries the token.

#include <kythira/iam/iam_authorizer.hpp>

#include <raft/aws_request_authorizer.hpp>
#include <raft/credential_verifier.hpp>
#include <raft/future_default.hpp>
#include <raft/sigv4_verifier.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace kythira::iam {

/// Where the gateway's cache sits in IAM's resource space (Requirement
/// 13.3), from `--iam-account-id`, `--iam-region` and `--iam-cache-name`.
struct redis_verifier_config {
    std::string account;
    std::string region;
    std::string cache_name;
    std::string partition = "aws";
    /// The longest a token may be presigned for (ElastiCache's 15 minutes).
    std::chrono::seconds max_token_lifetime{900};
    /// How long an accepted `AUTH` keeps a connection (ElastiCache's 12 h).
    std::chrono::seconds max_connection{12 * 60 * 60};
};

class iam_redis_verifier {
public:
    using clock_fn = std::function<std::chrono::system_clock::time_point()>;

    iam_redis_verifier(iam_authorizer& authz, redis_verifier_config cfg, clock_fn clock = nullptr)
        : _authz(&authz), _cfg(std::move(cfg)), _clock(clock ? std::move(clock) : clock_fn([] {
              return std::chrono::system_clock::now();
          })) {
        _cfg.cache_name = lower(_cfg.cache_name);
    }

    /// The `credential_verifier` concept. The answer is ready at once.
    [[nodiscard]] auto verify(const credential_request& r) -> future_default<credential_result> {
        promise_default<credential_result> p;
        auto f = p.getFuture();
        p.setValue(check(r));
        return f;
    }

    /// The same check, synchronously.
    [[nodiscard]] auto check(const credential_request& r) const -> credential_result {
        // 1. The token's shape and what it names.
        const auto token = r.credential;
        const auto sep = token.find("/?");
        if (sep == std::string_view::npos || sep == 0) {
            return credential_denied{"not an IAM auth token"};
        }
        const auto host = token.substr(0, sep);
        const auto query = token.substr(sep + 2);
        if (lower(host) != _cfg.cache_name) {
            return credential_denied{"the token names another cache"};
        }
        const auto params = sigv4::detail::decoded_query(query);
        const auto* action = sigv4::detail::query_param(params, "Action");
        if (action == nullptr || *action != "connect") {
            return credential_denied{"the token is not for Action=connect"};
        }
        const auto* user = sigv4::detail::query_param(params, "User");
        if (user == nullptr || *user != r.user) {
            return credential_denied{"the token names another user"};
        }
        const auto* expires = sigv4::detail::query_param(params, "X-Amz-Expires");
        if (expires == nullptr || expires->empty() || expires->size() > 6 ||
            !std::ranges::all_of(*expires, [](char c) { return c >= '0' && c <= '9'; }) ||
            std::stol(*expires) > _cfg.max_token_lifetime.count()) {
            return credential_denied{"the token is presigned for longer than 15 minutes"};
        }

        // 2. Who signed it.
        sigv4::request req;
        req.method = "GET";
        req.raw_path = "/";
        req.raw_query = query;
        req.headers = {{"host", std::string(host)}};
        const sigv4::service_rules rules{.service = "elasticache", .regions = {_cfg.region}};
        auto who = _authz->authenticate(req, rules);
        if (!who.has_value() && who.error().code == "SignatureDoesNotMatch") {
            req.headers.emplace_back("x-amz-content-sha256", "UNSIGNED-PAYLOAD");
            who = _authz->authenticate(req, rules);
        }
        if (!who.has_value()) {
            return credential_denied{"authentication failed: " + who.error().code};
        }
        const auto& principal = who.value();

        // 3. elasticache:Connect on the user and the replication group.
        const auto arn = [&](std::string_view kind, std::string_view name) {
            return "arn:" + _cfg.partition + ":elasticache:" + _cfg.region + ":" + _cfg.account +
                   ":" + std::string(kind) + ":" + std::string(name);
        };
        for (const auto& resource : {arn("user", r.user), arn("replicationgroup", host)}) {
            aws_auth::authz_request a;
            a.action = "elasticache:Connect";
            a.resource = resource;
            a.resource_account = _cfg.account;
            a.region = _cfg.region;
            a.source_ip = std::string(r.peer);
            const auto d = _authz->authorize(principal, a);
            if (!d.allowed) {
                return credential_denied{"elasticache:Connect denied on " + resource +
                                         (d.reason.empty() ? std::string() : ": " + d.reason)};
            }
        }

        // 4. Until the earlier of the connection bound and the session.
        auto until = _clock() + _cfg.max_connection;
        if (principal.expires && *principal.expires < until) {
            until = *principal.expires;
        }
        return credential_accepted{principal.arn, until};
    }

private:
    [[nodiscard]] static auto lower(std::string_view s) -> std::string {
        std::string out(s);
        std::ranges::transform(out, out.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    iam_authorizer* _authz;
    redis_verifier_config _cfg;
    clock_fn _clock;
};

static_assert(credential_verifier<iam_redis_verifier>);

}  // namespace kythira::iam
