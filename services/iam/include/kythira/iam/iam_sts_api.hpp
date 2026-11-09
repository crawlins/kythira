// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_sts_api.hpp
/// @brief The STS Query API's core actions (`.kiro/specs/iam-compatible-auth/`
///        Requirements 10.1-10.4, 10.7, 10.10; design Component 9; task 17).
///
/// AssumeRole (same account and cross-account, with `ExternalId`),
/// GetSessionToken, GetCallerIdentity and GetAccessKeyInfo. Issuing
/// credentials reads the state and writes nothing: the session travels in
/// its token (`iam_sts_token.hpp`), so any member with current state can
/// answer. The cluster it runs against is anything with
///
///   - `read(f)`: run `f` on this member's IAM state;
///   - `kek(salt)`: the cluster's KEK.
///
/// Session policies, session tags and source identity (task 18), web
/// identity (task 19), GetFederationToken and DecodeAuthorizationMessage
/// (task 26) answer `NotImplemented` until their tasks arrive, as do the
/// out-of-scope AssumeRoleWithSAML and AssumeRoot.

#include <kythira/iam/iam_authorizer.hpp>
#include <kythira/iam/iam_query_api.hpp>
#include <kythira/iam/iam_seal.hpp>
#include <kythira/iam/iam_state_machine.hpp>
#include <kythira/iam/iam_sts_token.hpp>
#include <kythira/iam/iam_validation.hpp>

#include <raft/aws_query_protocol.hpp>
#include <raft/aws_request_authorizer.hpp>

#include <openssl/crypto.h>

#include <charconv>
#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace kythira::iam::sts {

inline constexpr std::string_view k_sts_namespace = "https://sts.amazonaws.com/doc/2011-06-15/";

/// What the STS API needs from the cluster.
template<typename C>
concept sts_cluster = requires(C& c, const std::function<void(const iam_state_machine&)>& f) {
    { c.read(f) } -> std::same_as<void>;
    { c.kek(std::string_view{}) } -> std::same_as<const seal_key&>;
};

namespace detail {

inline constexpr std::string_view k_b32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

/// An `ASIA` key id that carries its account, so GetAccessKeyInfo can
/// answer for a temporary key as AWS does: "ASIA", 8 base32 characters of
/// the 12-digit account (40 bits), and 8 random ones.
[[nodiscard]] inline auto temporary_key_id(std::string_view account) -> std::string {
    std::uint64_t n = 0;
    for (const char c : account) {
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
    }
    std::string out = "ASIA";
    for (int i = 7; i >= 0; --i) {
        out += k_b32[(n >> (5U * static_cast<unsigned>(i))) & 0x1FU];
    }
    const auto rnd = random_bytes(8);
    for (const auto b : rnd) {
        out += k_b32[b & 0x1FU];
    }
    return out;
}

/// The account a key id from `temporary_key_id` carries.
[[nodiscard]] inline auto account_of_temporary_key(std::string_view id)
    -> std::optional<std::string> {
    if (id.size() != 20 || !id.starts_with("ASIA")) {
        return std::nullopt;
    }
    std::uint64_t n = 0;
    for (const char c : id.substr(4, 8)) {
        const auto v = k_b32.find(c);
        if (v == std::string_view::npos) {
            return std::nullopt;
        }
        n = (n << 5U) | v;
    }
    if (n > 999'999'999'999ULL) {
        return std::nullopt;
    }
    auto s = std::to_string(n);
    return std::string(12 - s.size(), '0') + s;
}

/// The kind of credential `who` signed with (Requirement 10.10).
[[nodiscard]] inline auto credential_of(const aws_auth::principal_context& who)
    -> validate::credential_kind {
    if (who.session_token) {
        return validate::credential_kind::session_token;
    }
    switch (who.type) {
        case aws_auth::principal_type::root:
            return validate::credential_kind::root;
        case aws_auth::principal_type::user:
            return validate::credential_kind::user;
        case aws_auth::principal_type::assumed_role:
            return validate::credential_kind::assumed_role;
        case aws_auth::principal_type::federated_user:
            return validate::credential_kind::federated_user;
    }
    return validate::credential_kind::user;
}

}  // namespace detail

template<sts_cluster C> class sts_query_api {
public:
    sts_query_api(C& cluster, iam_authorizer& authz) : _c(&cluster), _authz(&authz) {}

    /// Answers one authenticated request.
    [[nodiscard]] auto handle(const aws_auth::principal_context& who, const aws_query::form& f,
                              const query::request_facts& facts) -> query::reply {
        const auto action = f.get("Action");
        if (!action || action->empty()) {
            return error(facts, "MissingAction", "Missing Action");
        }
        if (const auto v = f.get("Version"); v && *v != "2011-06-15") {
            return error(facts, "InvalidParameterValue",
                         "Could not find operation " + std::string(*action) + " for version " +
                             std::string(*v));
        }
        try {
            call x{*this, who, f, facts, std::string(*action)};
            auto result = x.dispatch();
            return {200,
                    aws_query::response_xml(*action, k_sts_namespace, result, facts.request_id)};
        } catch (const query::api_error& e) {
            return error(facts, e.code(), e.what());
        } catch (const std::exception&) {
            return error(facts, "ServiceFailure",
                         "Request processing has failed because of an unknown error, exception "
                         "or failure.");
        }
    }

private:
    [[nodiscard]] static auto error(const query::request_facts& facts, std::string_view code,
                                    std::string_view message) -> query::reply {
        const auto status = query::status_of(code);
        return {status, aws_query::error_xml(k_sts_namespace, status < 500, code, message,
                                             facts.request_id)};
    }

    struct call {
        sts_query_api& api;
        const aws_auth::principal_context& who;
        const aws_query::form& f;
        const query::request_facts& facts;
        std::string action;

        auto dispatch() -> std::string {
            using handler = std::string (call::*)();
            static const std::map<std::string_view, handler, std::less<>> table{
                {"AssumeRole", &call::assume_role},
                {"GetSessionToken", &call::get_session_token},
                {"GetCallerIdentity", &call::get_caller_identity},
                {"GetAccessKeyInfo", &call::get_access_key_info},
            };
            const auto it = table.find(action);
            if (it == table.end()) {
                query::detail::fail("NotImplemented", "The action " + action +
                                                          " is not implemented by this endpoint.");
            }
            limit_credentials();
            return (this->*(it->second))();
        }

        /// Requirement 10.10: what temporary credentials may call, and no
        /// MFA (no device can exist). Not policy decisions: no policy lifts
        /// them.
        void limit_credentials() {
            if (!validate::sts_action_allowed(detail::credential_of(who), action)) {
                query::detail::fail("AccessDenied",
                                    "Cannot call " + action + " with session credentials");
            }
            const auto serial = f.get("SerialNumber");
            const auto code = f.get("TokenCode");
            if (serial && !validate::mfa_serial_number(*serial)) {
                query::detail::fail("ValidationError",
                                    "The specified value for serialNumber is invalid. It must be "
                                    "9-256 characters of [\\w+=/:,.@-].");
            }
            if (code && !validate::mfa_token_code(*code)) {
                query::detail::fail("ValidationError",
                                    "The specified value for tokenCode is invalid. It must be 6 "
                                    "digits.");
            }
            if (serial || code) {
                query::detail::fail("AccessDenied",
                                    "MultiFactorAuthentication failed: no MFA device is "
                                    "registered with this endpoint.");
            }
        }

        void read(const std::function<void(const iam_state_machine&)>& fn) { api._c->read(fn); }

        [[nodiscard]] auto required(std::string_view name) const -> std::string {
            const auto v = f.get(name);
            if (!v) {
                query::detail::fail("ValidationError",
                                    "1 validation error detected: Value null at '" +
                                        std::string(name) +
                                        "' failed to satisfy constraint: Member must not be null");
            }
            return std::string(*v);
        }

        [[nodiscard]] auto optional_int(std::string_view name) const
            -> std::optional<std::int64_t> {
            const auto v = f.get(name);
            if (!v) {
                return std::nullopt;
            }
            std::int64_t n = 0;
            const auto* end = v->data() + v->size();
            const auto [p, ec] = std::from_chars(v->data(), end, n);
            if (ec != std::errc{} || p != end || v->empty()) {
                query::detail::fail(
                    "ValidationError",
                    "The specified value for " + std::string(name) + " is not a valid integer.");
            }
            return n;
        }

        /// The parameters task 18 adds: refused rather than ignored, since
        /// ignoring a session policy would grant more than was asked.
        void refuse_unsupported() const {
            for (const auto* p : {"Policy", "PolicyArns.member.1.arn", "Tags.member.1.Key",
                                  "TransitiveTagKeys.member.1", "SourceIdentity"}) {
                if (f.contains(p)) {
                    query::detail::fail("NotImplemented",
                                        "Session policies, session tags and source identity are "
                                        "not supported by this endpoint yet.");
                }
            }
        }

        /// INV-STS-7, checked wherever it is accepted.
        void check_token_size() const {
            const auto n = optional_int("MinimumSessionTokenSize");
            if (n && !validate::minimum_session_token_size(*n)) {
                query::detail::fail("ValidationError",
                                    "The specified value for MinimumSessionTokenSize is invalid. "
                                    "It must be between 0 and 4096.");
            }
        }

        /// Seals `s` with fresh `ASIA` credentials and writes `Credentials`.
        auto mint(session s, std::int64_t duration, aws_query::xml_writer& w) -> void {
            std::optional<sts_key> key;
            read([&](const iam_state_machine& sm) {
                if (!sm.kek_salt().empty()) {
                    key = signing_key(sm, api._c->kek(sm.kek_salt()));
                }
            });
            if (!key) {
                query::detail::fail("ServiceFailure",
                                    "No STS key is available yet; rotate one with the admin API.");
            }
            const auto now = iam_leader::now_seconds();
            s.iat = now;
            s.exp = now + duration;
            s.access_key_id = detail::temporary_key_id(s.account);
            s.secret_access_key = iam_leader::generate_secret_access_key();
            const auto token = seal(*key, s);
            w.open("Credentials")
                .element("AccessKeyId", s.access_key_id)
                .element("SecretAccessKey", s.secret_access_key)
                .element("SessionToken", token)
                .element("Expiration", query::iso8601(s.exp))
                .close();
            OPENSSL_cleanse(s.secret_access_key.data(), s.secret_access_key.size());
        }

        // ── AssumeRole (Requirement 10.2) ──

        auto assume_role() -> std::string {
            const auto role_arn = required("RoleArn");
            if (!validate::arn_length(role_arn)) {
                query::detail::fail("ValidationError",
                                    "The specified value for roleArn is invalid. It must be "
                                    "20-2048 characters.");
            }
            const auto session_name = required("RoleSessionName");
            if (!validate::role_session_name(session_name)) {  // INV-STS-1
                query::detail::fail("ValidationError",
                                    "1 validation error detected: Value '" + session_name +
                                        "' at 'roleSessionName' failed to satisfy constraint: "
                                        "Member must satisfy regular expression pattern: "
                                        "[\\w+=,.@-]* and have length between 2 and 64");
            }
            const auto external_id = f.get("ExternalId");
            if (external_id && !validate::external_id(*external_id)) {  // INV-STS-3
                query::detail::fail("ValidationError",
                                    "1 validation error detected: Value at 'externalId' failed "
                                    "to satisfy constraint: Member must satisfy regular "
                                    "expression pattern: [\\w+=,.@:/-]* and have length between "
                                    "2 and 1224");
            }
            const auto requested = optional_int("DurationSeconds");
            check_token_size();
            refuse_unsupported();

            // The role, in whichever account its ARN names. A missing role
            // is AccessDenied, as on AWS, so roles cannot be probed.
            std::optional<iam_role> role;
            std::string partition;
            std::string role_account;
            read([&](const iam_state_machine& sm) {
                partition = sm.config().partition;
                const auto head = "arn:" + partition + ":iam::";
                if (!role_arn.starts_with(head)) {
                    return;
                }
                const auto rest = std::string_view(role_arn).substr(head.size());
                const auto colon = rest.find(':');
                if (colon == std::string_view::npos ||
                    !rest.substr(colon + 1).starts_with("role/")) {
                    return;
                }
                role_account = std::string(rest.substr(0, colon));
                const auto name = role_arn.substr(role_arn.rfind('/') + 1);
                if (const auto* r = sm.role(role_account, name);
                    r != nullptr && sm.arn(role_account, "role", r->path, r->name) == role_arn) {
                    role = *r;
                }
            });
            const auto denied = [&] {
                query::detail::fail("AccessDenied", "User: " + who.arn +
                                                        " is not authorized to perform: "
                                                        "sts:AssumeRole on resource: " +
                                                        role_arn);
            };
            if (!role) {
                denied();
            }
            const bool chained = who.type == aws_auth::principal_type::assumed_role;
            const auto duration =
                validate::assume_role_duration(requested, role->max_session_duration, chained);
            if (!duration.has_value()) {  // INV-DUR-1..3
                query::detail::fail("ValidationError", duration.error());
            }

            // INV-STS-B1, B2: the trust policy with the request's keys, and
            // across accounts the caller's own identity policies too; the
            // evaluator applies both rules.
            aws_auth::authz_request r;
            r.action = "sts:AssumeRole";
            r.resource = role_arn;
            r.resource_account = role_account;
            r.resource_policy = role->trust_policy;
            r.resource_kind = aws_auth::resource_policy_kind::trust;
            r.resource_tags.assign(role->tags.begin(), role->tags.end());
            r.keys.push_back({"sts:RoleSessionName", {session_name}});
            if (external_id) {
                r.keys.push_back({"sts:ExternalId", {std::string(*external_id)}});
            }
            r.source_ip = facts.source_ip;
            r.user_agent = facts.user_agent;
            r.region = facts.region;
            r.secure_transport = facts.secure_transport;
            if (!api._authz->authorize(who, r).allowed) {
                denied();
            }

            session s;
            s.account = role_account;
            s.issuer_kind = std::string(k_issuer_role);
            s.role_arn = role_arn;
            s.role_name = role->name;
            s.session_name = session_name;
            // INV-STS-B8.
            s.principal_arn = "arn:" + partition + ":sts::" + role_account + ":assumed-role/" +
                              role->name + "/" + session_name;
            s.userid = role->id + ":" + session_name;
            s.chained = chained;
            aws_query::xml_writer w;
            mint(s, duration.value(), w);
            w.open("AssumedRoleUser")
                .element("AssumedRoleId", s.userid)
                .element("Arn", s.principal_arn)
                .close();
            return w.str();
        }

        // ── GetSessionToken ──

        /// No permission is needed, as on AWS; only long-term keys may call
        /// it (`limit_credentials`).
        auto get_session_token() -> std::string {
            const bool root = who.type == aws_auth::principal_type::root;
            const auto duration =
                validate::long_term_duration(optional_int("DurationSeconds"), root);
            if (!duration.has_value()) {  // INV-DUR-5, INV-DUR-6
                query::detail::fail("ValidationError", duration.error());
            }
            session s;
            s.account = who.account;
            s.issuer_kind = std::string(k_issuer_session_token);
            s.principal_arn = who.arn;
            s.userid = who.userid;
            s.user_arn = root ? std::string{} : who.arn;
            s.user_name = root ? std::string{} : who.username;
            aws_query::xml_writer w;
            mint(s, duration.value(), w);
            return w.str();
        }

        // ── GetCallerIdentity (INV-STS-B4: never denied) ──

        auto get_caller_identity() -> std::string {
            aws_query::xml_writer w;
            w.element("Arn", who.arn).element("UserId", who.userid).element("Account", who.account);
            return w.str();
        }

        // ── GetAccessKeyInfo ──

        auto get_access_key_info() -> std::string {
            const auto id = required("AccessKeyId");
            aws_auth::authz_request r;
            r.action = "sts:GetAccessKeyInfo";
            r.resource = "*";
            r.source_ip = facts.source_ip;
            r.user_agent = facts.user_agent;
            r.region = facts.region;
            r.secure_transport = facts.secure_transport;
            if (!api._authz->authorize(who, r).allowed) {
                query::detail::fail(
                    "AccessDenied",
                    "User: " + who.arn + " is not authorized to perform: sts:GetAccessKeyInfo");
            }
            std::optional<std::string> account = detail::account_of_temporary_key(id);
            if (!account) {
                read([&](const iam_state_machine& sm) {
                    if (const auto [a, k] = sm.access_key(id); a != nullptr) {
                        account = a->id;
                    }
                });
            }
            if (!account) {
                query::detail::fail("InvalidParameterValue",
                                    "The access key ID is not one this endpoint issued.");
            }
            aws_query::xml_writer w;
            w.element("Account", *account);
            return w.str();
        }
    };

    C* _c;
    iam_authorizer* _authz;
};

}  // namespace kythira::iam::sts
