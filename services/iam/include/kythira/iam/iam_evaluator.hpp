// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_evaluator.hpp
/// @brief The policy evaluator (`.kiro/specs/iam-compatible-auth/` task 12,
///        Requirement 6, invariants INV-EVAL-*).
///
/// `evaluate()` is the only place a decision is made (Requirement 6.7):
/// the authorizer, STS and the Simulate* calls all build an
/// `evaluation_request` from the state they hold and call it. Gathering the
/// policies (a user's inline, attached and group policies; a role's; the
/// boundary; the session policies carried in a token) is the caller's job,
/// because only the caller knows where they live.
///
/// The flow transcribes the IAM User Guide page "Policy evaluation logic"
/// and its subpages "How AWS enforcement code logic evaluates requests to
/// allow or deny access" (cited below as [denyallow]) and "Cross-account
/// policy evaluation logic" ([crossaccount]):
///
///   1. Deny: an explicit Deny in any applicable policy decides.
///   2. The RCP and SCP seam. Kythira has no Organizations, so it passes.
///   3. Resource-based policy. Within the account, what an Allow there is
///      worth depends on whom it names ([denyallow], "Resource-based
///      policies"):
///        - the requesting user, role session or federated-user session
///          itself, or anyone (`*`, however narrowed by conditions): a
///          final Allow, which no boundary or session policy limits;
///        - the role behind a role session, or the IAM user behind a
///          federated session: it stands in for an identity allow, and the
///          boundary and session policy still apply;
///        - the account (its id or root ARN): it delegates to the account,
///          so an identity allow is still needed.
///      Role trust policies and KMS key policies must themselves allow the
///      principal; an identity allow alone is not enough.
///   4. Identity-based policies.
///   5. Permissions boundary, if the entity has one.
///   6. Session policies, for session principals. A role session without
///      one is not limited; a federated-user session without one has no
///      permissions, as GetFederationToken documents.
///
/// Across accounts ([crossaccount]) the principal's side (identity ∧
/// boundary ∧ session) and the resource policy must both allow, whatever
/// the resource policy names. The root user is allowed everything in its
/// own account unless an explicit deny stops it (Requirement 6.4); like
/// anyone else it needs a trust or key policy to allow it.
///
/// Principals with no identity policies of their own (an AWS service, a web
/// identity calling AssumeRoleWithWebIdentity, an anonymous caller) are
/// decided by the resource policy alone.

#include <kythira/iam/iam_conditions.hpp>
#include <kythira/iam/iam_policy.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::iam {

/// Who is asking.
enum class principal_kind : std::uint8_t {
    root,            // the account's root user
    user,            // an IAM user, signing with its own key
    role_session,    // an assumed-role session
    federated_user,  // a GetFederationToken session
    service,         // an AWS service principal (`ec2.amazonaws.com`)
    web_identity,    // an identity provider's user (AssumeRoleWithWebIdentity)
    anonymous,       // an unsigned request
};

struct principal {
    principal_kind kind = principal_kind::anonymous;
    std::string account;  // empty for service, web_identity and anonymous
    // The principal's own ARN: `arn:aws:iam::A:root`, `...:user/path/name`,
    // `arn:aws:sts::A:assumed-role/name/session`,
    // `arn:aws:sts::A:federated-user/name`. Empty for the last three kinds.
    std::string arn;
    std::string role_arn;  // role sessions: the role, `arn:aws:iam::A:role/path/name`
    std::string user_arn;  // federated users: the IAM user who federated
    std::string name;      // service: its principal name; web_identity: the provider

    /// Principals that have an account and identity policies.
    [[nodiscard]] auto is_iam() const noexcept -> bool {
        return kind == principal_kind::root || kind == principal_kind::user ||
               kind == principal_kind::role_session || kind == principal_kind::federated_user;
    }
    [[nodiscard]] auto is_session() const noexcept -> bool {
        return kind == principal_kind::role_session || kind == principal_kind::federated_user;
    }
};

/// A policy and the name decisions report it by (a managed policy's ARN, or
/// `user/alice:InlineName`). The document is not owned.
struct named_policy {
    std::string name;
    const policy_document* doc = nullptr;
};

/// What the resource policy is, for the trust and key-policy exception.
enum class resource_policy_kind : std::uint8_t {
    ordinary,  // a bucket, stream or table policy
    trust,     // a role trust policy (sts:AssumeRole* and friends)
    key,       // a KMS key policy
};

struct evaluation_request {
    principal who;
    std::string action;            // `s3:GetObject`
    std::string resource;          // an ARN, or `*`
    std::string resource_account;  // empty: the principal's own account
    context_keys context;          // global keys plus the service's

    std::vector<named_policy> identity;
    std::optional<named_policy> resource_policy;
    resource_policy_kind resource_kind = resource_policy_kind::ordinary;
    std::optional<named_policy> boundary;
    std::vector<named_policy> session;  // empty: none was passed
};

/// The statement that decided, by policy name and position.
struct statement_ref {
    std::string policy;
    std::string sid;
    std::size_t index = 0;
};

struct decision {
    enum class outcome : std::uint8_t {
        allow,
        explicit_deny,
        implicit_deny,
    };
    outcome result = outcome::implicit_deny;
    std::optional<statement_ref> deciding;
    // Why, for the audit log only (Requirement 6.5). Never sent to a client.
    std::string reason;

    [[nodiscard]] auto allowed() const noexcept -> bool { return result == outcome::allow; }
};

namespace detail {

/// What an Allow in a resource policy is worth to the requester (step 3),
/// weakest first.
enum class grant : std::uint8_t {
    account,  // names the account: still needs an identity allow
    limited,  // names the role or federating user: boundary and session apply
    direct,   // names the requester itself, or anyone: a final allow
};

[[nodiscard]] inline auto names_account(std::string_view value, std::string_view account) -> bool {
    if (account.empty()) {
        return false;
    }
    if (value == account) {
        return true;
    }
    const auto f = split_arn(value);
    return f && (*f)[2] == "iam" && (*f)[4] == account && (*f)[5] == "root";
}

[[nodiscard]] inline auto contains(const std::vector<std::string>* values, std::string_view v)
    -> bool {
    return values != nullptr && !v.empty() && std::ranges::find(*values, v) != values->end();
}

/// The strongest grant a `Principal` element gives `who`, if it names it.
[[nodiscard]] inline auto principal_grant(const principal_set& p, const principal& who)
    -> std::optional<grant> {
    if (p.anyone) {
        return grant::direct;
    }
    std::optional<grant> best;
    const auto raise = [&](grant g) {
        if (!best || g > *best) {
            best = g;
        }
    };
    if (const auto* aws = p.values("AWS"); aws != nullptr && who.is_iam()) {
        for (const auto& v : *aws) {
            if (v == "*" || v == who.arn) {
                raise(grant::direct);
            } else if (names_account(v, who.account)) {
                raise(who.kind == principal_kind::root ? grant::direct : grant::account);
            } else if ((who.kind == principal_kind::role_session && v == who.role_arn) ||
                       (who.kind == principal_kind::federated_user && v == who.user_arn)) {
                raise(grant::limited);
            }
        }
    }
    if ((who.kind == principal_kind::service && contains(p.values("Service"), who.name)) ||
        (who.kind == principal_kind::web_identity && contains(p.values("Federated"), who.name))) {
        raise(grant::direct);
    }
    return best;
}

/// Whether `NotPrincipal` exempts `who`: every identity the request carries
/// must be listed, as the NotPrincipal page requires (a role session is
/// exempt only when its session ARN, its role and its account all are).
[[nodiscard]] inline auto not_principal_exempts(const principal_set& p, const principal& who)
    -> bool {
    const auto* aws = p.values("AWS");
    const auto account_listed = [&] {
        return aws != nullptr && std::ranges::any_of(*aws, [&](const std::string& v) {
                   return names_account(v, who.account);
               });
    };
    switch (who.kind) {
        case principal_kind::root:
            return account_listed();
        case principal_kind::user:
        case principal_kind::federated_user:
            return contains(aws, who.arn) && account_listed();
        case principal_kind::role_session:
            return contains(aws, who.arn) && contains(aws, who.role_arn) && account_listed();
        case principal_kind::service:
            return contains(p.values("Service"), who.name);
        case principal_kind::web_identity:
            return contains(p.values("Federated"), who.name);
        case principal_kind::anonymous:
            break;
    }
    return false;
}

/// Whether a statement's Action, Resource and Condition cover the request
/// (its principal is checked separately).
[[nodiscard]] inline auto covers(const statement& st, const policy_document& doc,
                                 const evaluation_request& r) -> bool {
    if (st.action.matches(r.action) == st.not_action) {
        return false;
    }
    const bool vars = doc.supports_variables();
    if (st.resource &&
        resource_matches(*st.resource, r.resource, r.context, vars) == st.not_resource) {
        return false;
    }
    return conditions_match(st, r.context, vars);
}

/// The grant a statement gives `who`: always `direct` in a policy with no
/// Principal element (identity, boundary, session), else what the
/// Principal or NotPrincipal element says.
[[nodiscard]] inline auto statement_grant(const statement& st, const principal& who)
    -> std::optional<grant> {
    if (!st.principal) {
        return grant::direct;
    }
    if (st.not_principal) {
        if (not_principal_exempts(*st.principal, who)) {
            return std::nullopt;
        }
        return grant::direct;
    }
    return principal_grant(*st.principal, who);
}

struct hit {
    statement_ref where;
    grant how = grant::direct;
};

/// The first statement of `eff` in `p` that applies to the request; for an
/// Allow, the one giving the strongest grant.
[[nodiscard]] inline auto find(const named_policy& p, effect eff, const evaluation_request& r)
    -> std::optional<hit> {
    if (p.doc == nullptr) {
        return std::nullopt;
    }
    std::optional<hit> best;
    for (std::size_t i = 0; i < p.doc->statements.size(); ++i) {
        const auto& st = p.doc->statements[i];
        if (st.eff != eff) {
            continue;
        }
        const auto g = statement_grant(st, r.who);
        if (!g || !covers(st, *p.doc, r)) {
            continue;
        }
        if (!best || *g > best->how) {
            best = hit{statement_ref{p.name, st.sid, i}, *g};
        }
        if (eff == effect::deny || best->how == grant::direct) {
            break;
        }
    }
    return best;
}

[[nodiscard]] inline auto find_any(const std::vector<named_policy>& ps, effect eff,
                                   const evaluation_request& r) -> std::optional<hit> {
    for (const auto& p : ps) {
        if (auto h = find(p, eff, r)) {
            return h;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline auto make(decision::outcome o, std::optional<hit> h, std::string reason)
    -> decision {
    decision d;
    d.result = o;
    if (h) {
        d.deciding = std::move(h->where);
    }
    d.reason = std::move(reason);
    return d;
}

}  // namespace detail

/// Decides `r` (Requirement 6.1-6.5). See the file comment for the flow.
[[nodiscard]] inline auto evaluate(const evaluation_request& r) -> decision {
    using detail::grant;
    using o = decision::outcome;
    const auto& who = r.who;
    const auto deny = [](std::optional<detail::hit> h, std::string why) {
        return detail::make(o::implicit_deny, std::move(h), std::move(why));
    };

    // 1. [denyallow] "Deny evaluation": an explicit Deny in any applicable
    //    policy is final (INV-EVAL-2). The resource policy's Deny reaches
    //    the root user too (Requirement 6.4).
    {
        std::vector<const named_policy*> all;
        for (const auto& p : r.identity) {
            all.push_back(&p);
        }
        if (r.resource_policy) {
            all.push_back(&*r.resource_policy);
        }
        if (r.boundary) {
            all.push_back(&*r.boundary);
        }
        for (const auto& p : r.session) {
            all.push_back(&p);
        }
        for (const auto* p : all) {
            if (auto h = detail::find(*p, effect::deny, r)) {
                return detail::make(o::explicit_deny, h, "explicit deny");
            }
        }
    }

    // 2. [denyallow] "Organizations RCPs" and "SCPs": the seam. Kythira has
    //    no Organizations, so nothing is filtered here (Requirement 6.1.2).

    // 3. [denyallow] "Resource-based policies".
    std::optional<detail::hit> res;
    if (r.resource_policy) {
        res = detail::find(*r.resource_policy, effect::allow, r);
    }
    const bool must_name = r.resource_kind != resource_policy_kind::ordinary;
    if (must_name && !res) {
        // Trust and key policies "must explicitly allow access for
        // principals" (INV-EVAL-4): an identity allow alone is not enough.
        return deny(std::nullopt, "the trust or key policy does not allow the principal");
    }
    if (!who.is_iam()) {
        // Services, web identities and anonymous callers have no identity
        // policies: the resource policy alone decides.
        return res ? detail::make(o::allow, res, "resource policy")
                   : deny(std::nullopt, "no resource policy allows the principal");
    }

    const bool same_account = r.resource_account.empty() || r.resource_account == who.account;
    if (same_account && who.kind == principal_kind::root) {
        // Requirement 6.4 and INV-EVAL-10: the root user has full access in
        // its own account, an explicit deny (step 1) apart.
        return detail::make(o::allow, res, "root user");
    }
    if (same_account && res && res->how == grant::direct) {
        // A grant to the requester itself is not limited by an implicit
        // deny in an identity policy, boundary or session policy
        // (INV-EVAL-5, -6, -7, -12).
        return detail::make(o::allow, res, "resource policy grants the principal directly");
    }

    // 4. [denyallow] "Identity-based policies". The root user's identity
    //    side always allows: only a cross-account request gets here.
    const auto ident = detail::find_any(r.identity, effect::allow, r);
    const bool identity_allows = who.kind == principal_kind::root || ident.has_value();
    if (same_account) {
        // A grant to the role or the federating user stands in for an
        // identity allow; a grant to the account does not (INV-EVAL-4).
        const bool stands_in = res && res->how == grant::limited;
        if (!identity_allows && !stands_in) {
            return deny(std::nullopt, "no identity-based or resource-based policy allows");
        }
    } else {
        // [crossaccount]: both sides must allow (INV-EVAL-9).
        if (!res) {
            return deny(std::nullopt, "cross-account: the resource policy does not allow");
        }
        if (!identity_allows) {
            return deny(std::nullopt, "cross-account: no identity-based policy allows");
        }
    }

    // 5. [denyallow] "IAM permissions boundaries" (INV-EVAL-5).
    if (r.boundary && !detail::find(*r.boundary, effect::allow, r)) {
        return deny(std::nullopt, "the permissions boundary does not allow");
    }

    // 6. [denyallow] "Session policies" (INV-EVAL-8).
    if (who.is_session()) {
        if (!r.session.empty()) {
            if (!detail::find_any(r.session, effect::allow, r)) {
                return deny(std::nullopt, "the session policy does not allow");
            }
        } else if (who.kind == principal_kind::federated_user) {
            return deny(std::nullopt, "a federated-user session without a session policy");
        }
    }
    return detail::make(o::allow, ident ? ident : res,
                        ident ? "identity-based policy" : "resource-based policy");
}

}  // namespace kythira::iam
