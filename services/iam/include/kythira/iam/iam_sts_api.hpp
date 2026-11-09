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
#include <kythira/iam/iam_oidc_jwks.hpp>
#include <kythira/iam/iam_query_api.hpp>
#include <kythira/iam/iam_seal.hpp>
#include <kythira/iam/iam_state_machine.hpp>
#include <kythira/iam/iam_sts_token.hpp>
#include <kythira/iam/iam_validation.hpp>

#include <raft/aws_query_protocol.hpp>
#include <raft/aws_request_authorizer.hpp>

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
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
#include <vector>

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
    /// `jwks` verifies AssumeRoleWithWebIdentity tokens; without one that
    /// action answers IDPCommunicationError.
    sts_query_api(C& cluster, iam_authorizer& authz, oidc::jwks_cache* jwks = nullptr)
        : _c(&cluster), _authz(&authz), _jwks(jwks) {}

    /// Answers one authenticated request.
    [[nodiscard]] auto handle(const aws_auth::principal_context& who, const aws_query::form& f,
                              const query::request_facts& facts) -> query::reply {
        return answer(&who, f, facts);
    }

    /// Answers one unsigned request. Only AssumeRoleWithWebIdentity may be
    /// unsigned (the token is the credential); anything else is refused
    /// as AWS refuses it.
    [[nodiscard]] auto handle_unsigned(const aws_query::form& f, const query::request_facts& facts)
        -> query::reply {
        return answer(nullptr, f, facts);
    }

    /// Whether `f` is a request that may arrive unsigned.
    [[nodiscard]] static auto unsigned_allowed(const aws_query::form& f) -> bool {
        const auto a = f.get("Action");
        return a && *a == "AssumeRoleWithWebIdentity";
    }

private:
    [[nodiscard]] auto answer(const aws_auth::principal_context* who, const aws_query::form& f,
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
        if (who == nullptr && !unsigned_allowed(f)) {
            return error(facts, "MissingAuthenticationToken",
                         "Request is missing Authentication Token");
        }
        static const aws_auth::principal_context anonymous{};
        try {
            call x{*this,         who != nullptr ? *who : anonymous, f, facts, std::string(*action),
                   who == nullptr};
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
        bool anonymous = false;  // unsigned: AssumeRoleWithWebIdentity only

        auto dispatch() -> std::string {
            using handler = std::string (call::*)();
            static const std::map<std::string_view, handler, std::less<>> table{
                {"AssumeRole", &call::assume_role},
                {"GetSessionToken", &call::get_session_token},
                {"GetCallerIdentity", &call::get_caller_identity},
                {"GetAccessKeyInfo", &call::get_access_key_info},
                {"AssumeRoleWithWebIdentity", &call::assume_role_with_web_identity},
            };
            const auto it = table.find(action);
            if (it == table.end()) {
                query::detail::fail("NotImplemented", "The action " + action +
                                                          " is not implemented by this endpoint.");
            }
            if (action != "AssumeRoleWithWebIdentity") {
                limit_credentials();
            }
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

        /// What a request asks to put in its session beyond the role:
        /// session policies, session tags and a source identity (task 18).
        struct session_request {
            std::optional<std::string> policy;  // inline JSON
            std::vector<std::string> policy_arns;
            validate::tag_list tags;              // the request's own
            std::vector<std::string> transitive;  // of `tags`
            std::optional<std::string> source_identity;
            std::size_t packed = 0;  // characters counted toward 2048
        };

        /// The calling session's transitive tags, which pass on down a
        /// role chain and cannot be overridden there (INV-STAG-3).
        [[nodiscard]] auto inherited_tags() const -> validate::tag_list {
            validate::tag_list out;
            if (who.type != aws_auth::principal_type::assumed_role) {
                return out;
            }
            for (const auto& [k, v] : who.tags) {
                const auto folded = validate::detail::fold(k);
                if (std::ranges::any_of(who.transitive_tag_keys, [&](const auto& t) {
                        return validate::detail::fold(t) == folded;
                    })) {
                    out.emplace_back(k, v);
                }
            }
            return out;
        }

        /// INV-SPOL-1..4, INV-STAG-1..3 and INV-STS-6, checked before any
        /// authorization; `account` is the account the session will be in.
        [[nodiscard]] auto session_parameters(const std::string& account,
                                              bool tags_and_identity = true) const
            -> session_request {
            session_request out;
            if (const auto p = f.get("Policy")) {
                out.policy = std::string(*p);
            }
            static constexpr std::array<std::string_view, 1> arn_field{"arn"};
            const auto arns = f.member_structs("PolicyArns", arn_field);
            if (!arns) {
                query::detail::fail("ValidationError", "The PolicyArns parameter is malformed.");
            }
            for (const auto& a : *arns) {
                const auto it = a.find("arn");
                if (it == a.end()) {
                    query::detail::fail("ValidationError", "Every PolicyArns entry needs an arn.");
                }
                out.policy_arns.push_back(it->second);
            }
            if (const auto e =
                    validate::session_policies_error(out.policy, out.policy_arns, account)) {
                query::detail::fail(e->first, e->second);
            }
            if (out.policy) {
                try {
                    (void)parse_policy(*out.policy, policy_type::session,
                                       parse_options{.max_size = 2048 * 4});
                } catch (const policy_error& e) {
                    query::detail::fail("MalformedPolicyDocument", e.what());
                }
                const auto cps = validate::detail::decode_utf8(*out.policy);
                out.packed += cps ? validate::detail::non_whitespace_chars(*cps) : 0;
            }
            for (const auto& a : out.policy_arns) {
                out.packed += a.size();
            }
            if (!tags_and_identity) {
                return out;  // AssumeRoleWithWebIdentity takes neither
            }

            static constexpr std::array<std::string_view, 2> tag_fields{"Key", "Value"};
            const auto tags = f.member_structs("Tags", tag_fields);
            if (!tags) {
                query::detail::fail("ValidationError", "The Tags parameter is malformed.");
            }
            for (const auto& t : *tags) {
                const auto k = t.find("Key");
                if (k == t.end()) {
                    query::detail::fail("ValidationError", "Every tag needs a Key.");
                }
                const auto v = t.find("Value");
                out.tags.emplace_back(k->second, v == t.end() ? std::string{} : v->second);
            }
            const auto transitive = f.members("TransitiveTagKeys");
            if (!transitive) {
                query::detail::fail("ValidationError",
                                    "The TransitiveTagKeys parameter is malformed.");
            }
            out.transitive = *transitive;
            if (const auto e =
                    validate::session_tags_error(out.tags, out.transitive, inherited_tags())) {
                query::detail::fail(
                    e->find("transitive tag of the calling session") != std::string::npos
                        ? "InvalidParameterValue"
                        : "ValidationError",
                    *e);
            }
            // A transitive key names one of this request's tags.
            for (const auto& key : out.transitive) {
                const auto fk = validate::detail::fold(key);
                if (!std::ranges::any_of(out.tags, [&](const auto& t) {
                        return validate::detail::fold(t.first) == fk;
                    })) {
                    query::detail::fail(
                        "InvalidParameterValue",
                        "TransitiveTagKeys entry " + key + " is not a key of the session tags.");
                }
            }

            if (const auto si = f.get("SourceIdentity")) {
                if (!validate::source_identity(*si)) {  // INV-STS-6
                    query::detail::fail("ValidationError",
                                        "1 validation error detected: Value at 'sourceIdentity' "
                                        "failed to satisfy constraint: Member must satisfy "
                                        "regular expression pattern: [\\w+=,.@-]* and have "
                                        "length between 2 and 64, and not begin with aws:");
                }
                // Once set, a source identity holds for the whole chain.
                if (!who.source_identity.empty() && who.source_identity != *si) {
                    query::detail::fail("InvalidParameterValue",
                                        "The source identity of a role chain cannot be changed.");
                }
                out.source_identity = std::string(*si);
            }
            return out;
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

        struct found_role {
            std::optional<iam_role> role;
            std::string partition;
            std::string account;
        };

        /// The role `role_arn` names, in whichever account that is. A
        /// missing role is AccessDenied to the caller, as on AWS, so roles
        /// cannot be probed.
        auto find_role(const std::string& role_arn) -> found_role {
            found_role out;
            read([&](const iam_state_machine& sm) {
                out.partition = sm.config().partition;
                const auto head = "arn:" + out.partition + ":iam::";
                if (!role_arn.starts_with(head)) {
                    return;
                }
                const auto rest = std::string_view(role_arn).substr(head.size());
                const auto colon = rest.find(':');
                if (colon == std::string_view::npos ||
                    !rest.substr(colon + 1).starts_with("role/")) {
                    return;
                }
                out.account = std::string(rest.substr(0, colon));
                const auto name = role_arn.substr(role_arn.rfind('/') + 1);
                if (const auto* r = sm.role(out.account, name);
                    r != nullptr && sm.arn(out.account, "role", r->path, r->name) == role_arn) {
                    out.role = *r;
                }
            });
            return out;
        }

        [[nodiscard]] auto checked_role_arn() const -> std::string {
            const auto role_arn = required("RoleArn");
            if (!validate::arn_length(role_arn)) {
                query::detail::fail("ValidationError",
                                    "The specified value for roleArn is invalid. It must be "
                                    "20-2048 characters.");
            }
            return role_arn;
        }

        [[nodiscard]] auto checked_session_name() const -> std::string {
            const auto session_name = required("RoleSessionName");
            if (!validate::role_session_name(session_name)) {  // INV-STS-1
                query::detail::fail("ValidationError",
                                    "1 validation error detected: Value '" + session_name +
                                        "' at 'roleSessionName' failed to satisfy constraint: "
                                        "Member must satisfy regular expression pattern: "
                                        "[\\w+=,.@-]* and have length between 2 and 64");
            }
            return session_name;
        }

        // ── AssumeRole (Requirement 10.2) ──

        auto assume_role() -> std::string {
            const auto role_arn = checked_role_arn();
            const auto session_name = checked_session_name();
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

            const auto [role, partition, role_account] = find_role(role_arn);
            const auto denied = [&] {
                query::detail::fail("AccessDenied", "User: " + who.arn +
                                                        " is not authorized to perform: "
                                                        "sts:AssumeRole on resource: " +
                                                        role_arn);
            };
            if (!role) {
                denied();
            }
            const auto extra = session_parameters(role_account);
            const auto inherited = inherited_tags();
            const auto source_identity = extra.source_identity
                                             ? *extra.source_identity
                                             : who.source_identity;  // kept down a chain
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
            if (!extra.transitive.empty()) {
                r.keys.push_back({"sts:TransitiveTagKeys", extra.transitive});
            }
            if (!source_identity.empty()) {
                r.keys.push_back({"sts:SourceIdentity", {source_identity}});
            }
            // Session tags, the request's and those inherited down the
            // chain, are the request tags of every check below.
            const auto session_tags = validate::merge_session_tags({}, extra.tags, inherited);
            if (!session_tags.empty()) {
                r.request_tags = session_tags;
            }
            r.source_ip = facts.source_ip;
            r.user_agent = facts.user_agent;
            r.region = facts.region;
            r.secure_transport = facts.secure_transport;
            if (!api._authz->authorize(who, r).allowed) {
                denied();
            }
            // INV-STAG-4: session tags need sts:TagSession, and a source
            // identity sts:SetSourceIdentity, from the same trust policy
            // (and, across accounts, the caller's own policies).
            const auto also = [&](const char* action) {
                auto extra_check = r;
                extra_check.action = action;
                if (!api._authz->authorize(who, extra_check).allowed) {
                    query::detail::fail("AccessDenied", "User: " + who.arn +
                                                            " is not authorized to perform: " +
                                                            action + " on resource: " + role_arn);
                }
            };
            if (!session_tags.empty()) {
                also("sts:TagSession");
            }
            if (!source_identity.empty()) {
                also("sts:SetSourceIdentity");
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
            if (extra.policy) {
                s.session_policies.push_back(*extra.policy);
            }
            s.session_policies.insert(s.session_policies.end(), extra.policy_arns.begin(),
                                      extra.policy_arns.end());
            s.tags = session_tags;
            s.transitive_tag_keys = extra.transitive;
            for (const auto& [k, v] : inherited) {
                s.transitive_tag_keys.push_back(k);
            }
            s.source_identity = source_identity;
            aws_query::xml_writer w;
            mint(s, duration.value(), w);
            w.open("AssumedRoleUser")
                .element("AssumedRoleId", s.userid)
                .element("Arn", s.principal_arn)
                .close();
            if (extra.policy || !extra.policy_arns.empty() || !session_tags.empty()) {
                // AWS reports the packed size as a percentage of its limit.
                w.element("PackedPolicySize", std::to_string(extra.packed * 100 / 2048));
            }
            if (!source_identity.empty()) {
                w.element("SourceIdentity", source_identity);
            }
            return w.str();
        }

        // ── AssumeRoleWithWebIdentity (Requirement 11) ──

        /// A claim as a list of strings: a string is a list of one, and
        /// anything else is empty.
        [[nodiscard]] static auto strings(const boost::json::object& claims, std::string_view name)
            -> std::vector<std::string> {
            std::vector<std::string> out;
            const auto* v = claims.if_contains(name);
            if (v == nullptr) {
                return out;
            }
            if (v->is_string()) {
                out.emplace_back(v->as_string());
            } else if (v->is_array()) {
                for (const auto& e : v->as_array()) {
                    if (e.is_string()) {
                        out.emplace_back(e.as_string());
                    }
                }
            }
            return out;
        }

        [[nodiscard]] static auto number(const boost::json::object& claims, std::string_view name)
            -> std::optional<std::int64_t> {
            const auto* v = claims.if_contains(name);
            if (v == nullptr) {
                return std::nullopt;
            }
            if (v->is_int64()) {
                return v->as_int64();
            }
            if (v->is_uint64()) {
                return static_cast<std::int64_t>(v->as_uint64());
            }
            if (v->is_double()) {
                return static_cast<std::int64_t>(v->as_double());
            }
            return std::nullopt;
        }

        /// INV-STS-B6, B7. The token is the credential: the request is
        /// unsigned, the provider is found by `iss` in the role's account,
        /// `aud` must be one of its client ids, the signature must verify
        /// under the issuer's JWKS with an RS or ES algorithm, the times
        /// must hold (5 minutes of skew), and the role's trust policy must
        /// allow the provider as `Federated`.
        auto assume_role_with_web_identity() -> std::string {
            const auto role_arn = checked_role_arn();
            const auto session_name = checked_session_name();
            const auto token = required("WebIdentityToken");
            if (!validate::web_identity_token_length(token)) {  // INV-WEB-1
                query::detail::fail("ValidationError",
                                    "1 validation error detected: Value at 'webIdentityToken' "
                                    "failed to satisfy constraint: Member must have length "
                                    "between 4 and 20000");
            }
            if (f.contains("ProviderId")) {
                query::detail::fail("InvalidParameterValue",
                                    "ProviderId is for OAuth 2.0 access tokens, which this "
                                    "endpoint does not accept.");
            }
            const auto requested = optional_int("DurationSeconds");
            const auto [role, partition, role_account] = find_role(role_arn);
            const auto extra = session_parameters(role_account, false);
            const auto invalid = [](const std::string& why) {
                query::detail::fail("InvalidIdentityToken", why);
            };

            // 1. Decode, without trusting anything yet.
            const auto parts = jose::parse_compact(token);
            if (!parts) {
                invalid("The web identity token is not a well-formed JWT.");
            }
            const auto* alg_v = parts->header.if_contains("alg");
            const auto alg = alg_v != nullptr && alg_v->is_string()
                                 ? jose::parse_alg(std::string_view(alg_v->as_string()))
                                 : std::nullopt;
            if (!alg) {
                invalid(
                    "The web identity token must be signed with RS256, RS384, RS512, "
                    "ES256, ES384 or ES512.");
            }
            const auto* kid_v = parts->header.if_contains("kid");
            if (kid_v == nullptr || !kid_v->is_string()) {
                invalid("The web identity token has no kid.");
            }
            boost::system::error_code ec;
            auto claims_v = boost::json::parse(parts->payload, ec);
            if (ec || !claims_v.is_object()) {
                invalid("The web identity token's claims are not a JSON object.");
            }
            const auto& claims = claims_v.as_object();
            const auto iss_list = strings(claims, "iss");
            if (iss_list.size() != 1 || !iss_list.front().starts_with("https://")) {
                invalid("The web identity token's iss is not an https URL.");
            }
            auto issuer = iss_list.front().substr(std::string_view("https://").size());
            while (issuer.ends_with('/')) {
                issuer.pop_back();
            }

            // 2. The provider, in the role's account.
            std::optional<iam_oidc_provider> provider;
            read([&](const iam_state_machine& sm) {
                if (const auto* a = sm.account(role_account)) {
                    if (const auto it = a->oidc_providers.find(issuer);
                        it != a->oidc_providers.end()) {
                        provider = it->second;
                    }
                }
            });
            if (!provider) {
                invalid("No OpenIDConnect provider found in your account for " + iss_list.front());
            }

            // 3. The audience. With `azp`, AWS's `:aud` key is the
            // authorized party and `:oaud` the audience.
            const auto aud = strings(claims, "aud");
            const auto azp = strings(claims, "azp");
            const auto client = [&](const std::string& id) {
                return std::ranges::find(provider->client_ids, id) != provider->client_ids.end();
            };
            if (std::ranges::none_of(aud, client)) {
                invalid("Incorrect token audience");
            }

            // 4. The signature, under the issuer's key for this kid.
            if (api._jwks == nullptr) {
                query::detail::fail("IDPCommunicationError",
                                    "This endpoint cannot reach identity providers.");
            }
            const auto [jwk, why] = api._jwks->key(issuer, std::string(kid_v->as_string()));
            if (!jwk) {
                if (why == oidc::key_error::unavailable) {
                    query::detail::fail("IDPCommunicationError",
                                        "Couldn't retrieve verification key from your identity "
                                        "provider.");
                }
                invalid("The web identity token's kid is not in the issuer's JWKS.");
            }
            jose::evp_pkey_ptr key;
            try {
                key = jose::public_key_from_jwk(*jwk);
            } catch (const std::exception&) {
                invalid("The issuer's key for this kid cannot be read.");
            }
            if (!key || !jose::verify_compact(token, key.get(), *alg)) {
                invalid("The web identity token's signature does not verify.");
            }

            // 5. The times, with 5 minutes of skew.
            constexpr std::int64_t skew = 300;
            const auto now = iam_leader::now_seconds();
            const auto exp = number(claims, "exp");
            if (!exp) {
                invalid("The web identity token has no exp.");
            }
            if (*exp + skew <= now) {
                query::detail::fail("ExpiredTokenException", "Token expired: current date/time " +
                                                                 query::iso8601(now) +
                                                                 " must be before the expiration "
                                                                 "date/time " +
                                                                 query::iso8601(*exp));
            }
            if (const auto nbf = number(claims, "nbf"); nbf && *nbf > now + skew) {
                invalid("The web identity token is not yet valid (nbf).");
            }
            if (const auto iat = number(claims, "iat"); iat && *iat > now + skew) {
                invalid("The web identity token was issued in the future (iat).");
            }

            if (!role) {
                query::detail::fail("AccessDenied",
                                    "Not authorized to perform sts:AssumeRoleWithWebIdentity");
            }
            const auto duration =
                validate::web_identity_duration(requested, role->max_session_duration);
            if (!duration.has_value()) {  // INV-DUR-4
                query::detail::fail("ValidationError", duration.error());
            }

            // 6. The trust policy, with the provider as the principal.
            const auto provider_arn =
                "arn:" + partition + ":iam::" + role_account + ":oidc-provider/" + issuer;
            const auto sub = strings(claims, "sub");
            aws_auth::authz_request r;
            r.action = "sts:AssumeRoleWithWebIdentity";
            r.resource = role_arn;
            r.resource_account = role_account;
            r.resource_policy = role->trust_policy;
            r.resource_kind = aws_auth::resource_policy_kind::trust;
            r.resource_tags.assign(role->tags.begin(), role->tags.end());
            r.keys.push_back({"sts:RoleSessionName", {session_name}});
            if (!sub.empty()) {
                r.keys.push_back({issuer + ":sub", {sub.front()}});
            }
            if (!azp.empty()) {
                r.keys.push_back({issuer + ":aud", {azp.front()}});
                r.keys.push_back({issuer + ":oaud", aud});
            } else {
                r.keys.push_back({issuer + ":aud", aud});
            }
            if (const auto amr = strings(claims, "amr"); !amr.empty()) {
                r.keys.push_back({issuer + ":amr", amr});
            }
            r.source_ip = facts.source_ip;
            r.user_agent = facts.user_agent;
            r.region = facts.region;
            r.secure_transport = facts.secure_transport;
            if (!api._authz->authorize_web_identity(provider_arn, r).allowed) {
                query::detail::fail("AccessDenied",
                                    "Not authorized to perform sts:AssumeRoleWithWebIdentity");
            }

            session s;
            s.account = role_account;
            s.issuer_kind = std::string(k_issuer_role);
            s.role_arn = role_arn;
            s.role_name = role->name;
            s.session_name = session_name;
            s.principal_arn = "arn:" + partition + ":sts::" + role_account + ":assumed-role/" +
                              role->name + "/" + session_name;
            s.userid = role->id + ":" + session_name;
            if (extra.policy) {
                s.session_policies.push_back(*extra.policy);
            }
            s.session_policies.insert(s.session_policies.end(), extra.policy_arns.begin(),
                                      extra.policy_arns.end());
            aws_query::xml_writer w;
            mint(s, duration.value(), w);
            w.element("SubjectFromWebIdentityToken", sub.empty() ? std::string{} : sub.front())
                .open("AssumedRoleUser")
                .element("AssumedRoleId", s.userid)
                .element("Arn", s.principal_arn)
                .close()
                .element("Provider", issuer)
                .element("Audience", azp.empty() ? aud.front() : azp.front());
            if (extra.policy || !extra.policy_arns.empty()) {
                w.element("PackedPolicySize", std::to_string(extra.packed * 100 / 2048));
            }
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
    oidc::jwks_cache* _jwks;
};

}  // namespace kythira::iam::sts
