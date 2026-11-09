// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_request_authorizer.hpp
/// @brief How a Kythira-served AWS API asks who a caller is and whether it
///        may act (`.kiro/specs/iam-compatible-auth/` Requirement 7,
///        design Component 6).
///
/// Plain types and one concept, so a service module (Kinesis, DynamoDB,
/// the Redis adapter) depends on this header and never on the IAM module:
///
///   auto who = authorizer.authenticate(request, rules);   // SigV4 + key
///   if (!who) return who.error();                         // AWS error
///   auto d = authorizer.authorize(*who, {.action = "kinesis:PutRecord",
///                                        .resource = stream_arn});
///   if (!d.allowed) return access_denied(...);
///
/// Each service declares its actions once in an `aws_action_table` and
/// hands it to IAM through its composition root; an action no table names
/// is denied (Requirement 7.2).

#include <raft/sigv4_verifier.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::aws_auth {

/// The `aws:PrincipalType` values Kythira can produce.
enum class principal_type : std::uint8_t {
    root,            // "Account"
    user,            // "User"
    assumed_role,    // "AssumedRole"
    federated_user,  // "FederatedUser"
};

[[nodiscard]] constexpr auto to_string(principal_type t) noexcept -> std::string_view {
    switch (t) {
        case principal_type::root:
            return "Account";
        case principal_type::user:
            return "User";
        case principal_type::assumed_role:
            return "AssumedRole";
        case principal_type::federated_user:
            return "FederatedUser";
    }
    return "Unknown";
}

/// Who signed a request, once its signature and key have been checked.
struct principal_context {
    principal_type type = principal_type::user;
    std::string account;
    std::string arn;       // the caller's own ARN (session ARN for sessions)
    std::string userid;    // aws:userid: AIDA..., AROA...:session, account id
    std::string username;  // IAM users only
    std::string access_key_id;
    // Role sessions: the role, by ARN and lower-cased name.
    std::string role_arn;
    std::string role_name;
    std::string session_name;
    // Federated users: the IAM user who federated.
    std::string user_arn;
    std::vector<std::pair<std::string, std::string>> tags;  // principal tags
    // Temporary credentials only.
    std::optional<std::chrono::system_clock::time_point> token_issue_time;
    std::optional<std::chrono::system_clock::time_point> expires;
    std::string source_identity;
    std::vector<std::string> session_policies;  // policy JSON, or a managed ARN
    // Session tags (in `tags`) that pass on to a chained role session.
    std::vector<std::string> transitive_tag_keys;
    // GetSessionToken credentials: the user (or root) above, limited to
    // AssumeRole and GetCallerIdentity (Requirement 10.10).
    bool session_token = false;
};

/// One action a service serves: its name without the prefix
/// (`PutRecord`), the ARN templates of the resources it acts on
/// (`arn:${Partition}:kinesis:${Region}:${Account}:stream/${StreamName}`)
/// and the service condition keys it supplies.
struct action_entry {
    std::string name;
    std::vector<std::string> resource_templates;
    std::vector<std::string> condition_keys;
};

/// A service's actions, under its prefix (`kinesis`).
struct aws_action_table {
    std::string service_prefix;
    std::vector<action_entry> actions;
};

/// What a resource policy is, when the service has one for the resource.
enum class resource_policy_kind : std::uint8_t {
    ordinary,
    trust,  // a role trust policy
    key,    // a KMS key policy
};

/// What one request asks to do. `keys` are the service's condition keys;
/// the authorizer adds the global ones from the request facts.
struct authz_request {
    std::string action;            // `kinesis:PutRecord`
    std::string resource = "*";    // an ARN, or `*`
    std::string resource_account;  // empty: the caller's own account
    std::vector<std::pair<std::string, std::vector<std::string>>> keys;
    std::optional<std::string> resource_policy;  // JSON, when the resource has one
    resource_policy_kind resource_kind = resource_policy_kind::ordinary;
    std::vector<std::pair<std::string, std::string>> resource_tags;
    std::optional<std::vector<std::pair<std::string, std::string>>> request_tags;
    // Request facts for the global condition keys.
    std::string source_ip;
    std::string user_agent;
    std::string region;
    bool secure_transport = true;
};

/// The answer. `policy`, `sid` and `reason` are for the audit log only and
/// are never sent to a client (Requirement 6.5).
struct authz_decision {
    bool allowed = false;
    bool explicit_deny = false;
    std::string policy;
    std::string sid;
    std::string reason;
};

/// authenticate()'s result: who the caller is, or the AWS error to answer
/// with. (Not std::expected, which the clang 18 static-analysis toolchain
/// lacks.)
class authn_result {
public:
    authn_result(principal_context p) : _v(std::move(p)) {}  // NOLINT(google-explicit-constructor)
    authn_result(sigv4::aws_error e) : _v(std::move(e)) {}   // NOLINT(google-explicit-constructor)

    [[nodiscard]] auto has_value() const -> bool { return _v.index() == 0; }
    explicit operator bool() const { return has_value(); }
    [[nodiscard]] auto value() const -> const principal_context& { return std::get<0>(_v); }
    [[nodiscard]] auto operator*() const -> const principal_context& { return value(); }
    [[nodiscard]] auto operator->() const -> const principal_context* { return &value(); }
    [[nodiscard]] auto error() const -> const sigv4::aws_error& {
        static const sigv4::aws_error none{};
        return has_value() ? none : std::get<1>(_v);
    }

private:
    std::variant<principal_context, sigv4::aws_error> _v;
};

/// Anything that authenticates a SigV4 request and authorizes an action.
template<typename A>
concept aws_request_authorizer =
    requires(A& a, const sigv4::request& req, const sigv4::service_rules& rules,
             const principal_context& who, const authz_request& r) {
        { a.authenticate(req, rules) } -> std::same_as<authn_result>;
        { a.authorize(who, r) } -> std::same_as<authz_decision>;
    };

}  // namespace kythira::aws_auth
