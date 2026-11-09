// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_query_api.hpp
/// @brief The IAM Query API's user, group and access-key actions
///        (`.kiro/specs/iam-compatible-auth/` Requirements 9.1-9.3, 9.8,
///        9.9; task 14).
///
/// One call per request: `iam_query_api::handle(who, form, facts)` decodes
/// the action's parameters, validates them as AWS does, authorizes
/// `iam:<Action>` on the action's resource ARN through `iam_authorizer`,
/// reads the state or proposes a command, and writes AWS's XML response or
/// error document. The cluster it runs against is anything with
///
///   - `read(f)`: run `f` on a consistent, current IAM state (the leader's);
///   - `submit(cmd)`: propose a command and return its `apply()` result;
///   - `kek(salt)`: the cluster's KEK.
///
/// Actions outside Requirements 9.2-9.3 answer `NotImplemented` (501) until
/// their tasks (15, 16) arrive.

#include <kythira/iam/iam_authorizer.hpp>
#include <kythira/iam/iam_policy.hpp>
#include <kythira/iam/iam_seal.hpp>
#include <kythira/iam/iam_state_machine.hpp>
#include <kythira/iam/iam_validation.hpp>

#include <raft/aws_query_protocol.hpp>
#include <raft/aws_request_authorizer.hpp>

#include <boost/json.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace kythira::iam::query {

inline constexpr std::string_view k_iam_namespace = "https://iam.amazonaws.com/doc/2010-05-08/";

/// An HTTP status and an XML body.
struct reply {
    unsigned status = 200;
    std::string body;
};

/// The facts about a request that are not its parameters.
struct request_facts {
    std::string request_id;
    std::string source_ip;
    std::string user_agent;
    std::string region = "us-east-1";
    bool secure_transport = true;
};

/// What the API needs from the cluster.
template<typename C>
concept query_cluster = requires(C& c, const std::function<void(const iam_state_machine&)>& f,
                                 const std::vector<std::byte>& cmd) {
    { c.read(f) } -> std::same_as<void>;
    { c.submit(cmd) } -> std::same_as<std::vector<std::byte>>;
    { c.kek(std::string_view{}) } -> std::same_as<const seal_key&>;
};

/// AWS's IAM error codes and their HTTP statuses (Requirement 9.9).
[[nodiscard]] inline auto status_of(std::string_view code) -> unsigned {
    if (code == "NoSuchEntity") {
        return 404;
    }
    if (code == "EntityAlreadyExists" || code == "DeleteConflict" || code == "LimitExceeded" ||
        code == "ConcurrentModification") {
        return 409;
    }
    if (code == "AccessDenied") {
        return 403;
    }
    if (code == "ServiceFailure") {
        return 500;
    }
    if (code == "NotImplemented") {
        return 501;
    }
    if (code == "ServiceUnavailable") {
        return 503;
    }
    return 400;  // InvalidInput, ValidationError, MalformedPolicyDocument, ...
}

/// An error a handler answers with.
class api_error : public std::runtime_error {
public:
    api_error(std::string code, const std::string& message)
        : std::runtime_error(message), _code(std::move(code)) {}
    [[nodiscard]] auto code() const -> const std::string& { return _code; }

private:
    std::string _code;
};

/// `seconds` since the epoch as AWS writes it: `2026-11-09T17:00:00Z`.
[[nodiscard]] inline auto iso8601(std::int64_t seconds) -> std::string {
    const auto t = static_cast<std::time_t>(seconds);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::array<char, 32> buf{};
    const auto n = std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return {buf.data(), n};
}

namespace detail {

[[noreturn]] inline void fail(std::string code, const std::string& message) {
    throw api_error(std::move(code), message);
}

/// The key `Marker` values are sealed with: the KEK under a fixed label,
/// so every member derives the same one and a marker works on any of them.
[[nodiscard]] inline auto marker_key(const seal_key& kek) -> std::array<unsigned char, 32> {
    static constexpr std::string_view label = "kythira-iam-list-marker-v1";
    std::array<unsigned char, 32> out{};
    unsigned int len = 0;
    HMAC(EVP_sha256(), kek.data(), static_cast<int>(seal_key::size),
         reinterpret_cast<const unsigned char*>(label.data()),  // NOLINT
         label.size(), out.data(), &len);
    return out;
}

inline void write_tags(aws_query::xml_writer& w, const tag_map& tags) {
    w.open("Tags");
    for (const auto& [k, v] : tags) {
        w.open("member").element("Key", k).element("Value", v).close();
    }
    w.close();
}

inline void write_boundary(aws_query::xml_writer& w, const std::optional<std::string>& b) {
    if (b) {
        w.open("PermissionsBoundary")
            .element("PermissionsBoundaryType", std::string_view("Policy"))
            .element("PermissionsBoundaryArn", *b)
            .close();
    }
}

/// The parts of a managed-policy ARN, `arn:<partition>:iam::<account>:policy<path><name>`.
struct policy_arn_parts {
    std::string partition;
    std::string account;  // "aws" for an AWS-managed policy
    std::string path;
    std::string name;
};

[[nodiscard]] inline auto split_policy_arn_any(std::string_view parn)
    -> std::optional<policy_arn_parts> {
    if (!parn.starts_with("arn:")) {
        return std::nullopt;
    }
    auto rest = parn.substr(4);
    const auto c1 = rest.find(':');
    if (c1 == std::string_view::npos) {
        return std::nullopt;
    }
    policy_arn_parts out;
    out.partition = std::string(rest.substr(0, c1));
    rest = rest.substr(c1 + 1);
    if (!rest.starts_with("iam::")) {
        return std::nullopt;
    }
    rest = rest.substr(5);
    const auto c2 = rest.find(':');
    if (c2 == std::string_view::npos) {
        return std::nullopt;
    }
    out.account = std::string(rest.substr(0, c2));
    rest = rest.substr(c2 + 1);
    if (!rest.starts_with("policy/")) {
        return std::nullopt;
    }
    rest = rest.substr(6);  // keep the leading '/'
    const auto slash = rest.rfind('/');
    out.path = std::string(rest.substr(0, slash + 1));
    out.name = std::string(rest.substr(slash + 1));
    if (out.name.empty()) {
        return std::nullopt;
    }
    return out;
}

/// The same, only in `partition`.
[[nodiscard]] inline auto split_policy_arn(std::string_view partition, std::string_view parn)
    -> std::optional<policy_arn_parts> {
    auto p = split_policy_arn_any(parn);
    if (!p || p->partition != partition) {
        return std::nullopt;
    }
    return p;
}

/// A stable PolicyId for a bundled AWS-managed policy, which carries none:
/// "ANPA" and 16 base32 characters of its name's SHA-256, the same on
/// every member.
[[nodiscard]] inline auto aws_policy_id(std::string_view name) -> std::string {
    std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
    unsigned int len = 0;
    if (EVP_Digest(name.data(), name.size(), md.data(), &len, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("iam: SHA-256 failed");
    }
    static constexpr std::string_view b32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string out = "ANPA";
    for (std::size_t i = 0; i < 16; ++i) {
        out += b32[md[i] & 0x1FU];
    }
    return out;
}

}  // namespace detail

template<query_cluster C> class iam_query_api {
public:
    iam_query_api(C& cluster, iam_authorizer& authz) : _c(&cluster), _authz(&authz) {}

    /// Answers one authenticated request.
    [[nodiscard]] auto handle(const aws_auth::principal_context& who, const aws_query::form& f,
                              const request_facts& facts) -> reply {
        const auto action = f.get("Action");
        if (!action || action->empty()) {
            return error(facts, "MissingAction", "Missing Action");
        }
        if (const auto v = f.get("Version"); v && *v != "2010-05-08") {
            return error(facts, "InvalidParameterValue",
                         "Could not find operation " + std::string(*action) + " for version " +
                             std::string(*v));
        }
        try {
            call x{*this, who, f, facts, std::string(*action)};
            auto result = x.dispatch();
            return {200,
                    aws_query::response_xml(*action, k_iam_namespace, result, facts.request_id)};
        } catch (const api_error& e) {
            return error(facts, e.code(), e.what());
        } catch (const std::exception&) {
            // Lost leadership, no quorum, a timeout: nothing the client did.
            return error(facts, "ServiceFailure",
                         "Request processing has failed because of an unknown error, exception "
                         "or failure.");
        }
    }

private:
    [[nodiscard]] static auto error(const request_facts& facts, std::string_view code,
                                    std::string_view message) -> reply {
        const auto status = status_of(code);
        return {status, aws_query::error_xml(k_iam_namespace, status < 500, code, message,
                                             facts.request_id)};
    }

    /// One request in flight.
    struct call {
        iam_query_api& api;
        const aws_auth::principal_context& who;
        const aws_query::form& f;
        const request_facts& facts;
        std::string action;

        // ── plumbing ──

        auto dispatch() -> std::string {
            using handler = std::function<std::string(call&)>;
            static const std::map<std::string_view, handler, std::less<>> table{
                {"CreateUser", &call::create_user},
                {"GetUser", &call::get_user},
                {"ListUsers", &call::list_users},
                {"UpdateUser", &call::update_user},
                {"DeleteUser", &call::delete_user},
                {"TagUser", [](call& c) { return c.tag("user"); }},
                {"UntagUser", [](call& c) { return c.untag("user"); }},
                {"ListUserTags", [](call& c) { return c.list_tags("user"); }},
                {"CreateAccessKey", &call::create_access_key},
                {"ListAccessKeys", &call::list_access_keys},
                {"UpdateAccessKey", &call::update_access_key},
                {"DeleteAccessKey", &call::delete_access_key},
                {"GetAccessKeyLastUsed", &call::get_access_key_last_used},
                {"CreateGroup", &call::create_group},
                {"GetGroup", &call::get_group},
                {"ListGroups", &call::list_groups},
                {"UpdateGroup", &call::update_group},
                {"DeleteGroup", &call::delete_group},
                {"AddUserToGroup", &call::add_user_to_group},
                {"RemoveUserFromGroup", &call::remove_user_from_group},
                {"ListGroupsForUser", &call::list_groups_for_user},
                {"CreateAccountAlias", &call::create_account_alias},
                {"DeleteAccountAlias", &call::delete_account_alias},
                {"ListAccountAliases", &call::list_account_aliases},
                // Roles (Requirement 9.4)
                {"CreateRole", &call::create_role},
                {"GetRole", &call::get_role},
                {"ListRoles", &call::list_roles},
                {"UpdateRole", &call::update_role},
                {"UpdateRoleDescription", &call::update_role_description},
                {"UpdateAssumeRolePolicy", &call::update_assume_role_policy},
                {"DeleteRole", &call::delete_role},
                {"TagRole", [](call& c) { return c.tag("role"); }},
                {"UntagRole", [](call& c) { return c.untag("role"); }},
                {"ListRoleTags", [](call& c) { return c.list_tags("role"); }},
                // Managed policies and versions (Requirement 9.5)
                {"CreatePolicy", &call::create_policy},
                {"GetPolicy", &call::get_policy},
                {"ListPolicies", &call::list_policies},
                {"DeletePolicy", &call::delete_policy},
                {"CreatePolicyVersion", &call::create_policy_version},
                {"GetPolicyVersion", &call::get_policy_version},
                {"ListPolicyVersions", &call::list_policy_versions},
                {"DeletePolicyVersion",
                 [](call& c) {
                     return c.policy_version_change(iam_command::delete_policy_version);
                 }},
                {"SetDefaultPolicyVersion",
                 [](call& c) { return c.policy_version_change(iam_command::set_default_version); }},
                {"TagPolicy", [](call& c) { return c.tag("policy"); }},
                {"UntagPolicy", [](call& c) { return c.untag("policy"); }},
                {"ListPolicyTags", [](call& c) { return c.list_tags("policy"); }},
                {"ListEntitiesForPolicy", &call::list_entities_for_policy},
                // Attachments, inline policies and boundaries
                {"AttachUserPolicy", [](call& c) { return c.attach("user", true); }},
                {"AttachGroupPolicy", [](call& c) { return c.attach("group", true); }},
                {"AttachRolePolicy", [](call& c) { return c.attach("role", true); }},
                {"DetachUserPolicy", [](call& c) { return c.attach("user", false); }},
                {"DetachGroupPolicy", [](call& c) { return c.attach("group", false); }},
                {"DetachRolePolicy", [](call& c) { return c.attach("role", false); }},
                {"ListAttachedUserPolicies", [](call& c) { return c.list_attached("user"); }},
                {"ListAttachedGroupPolicies", [](call& c) { return c.list_attached("group"); }},
                {"ListAttachedRolePolicies", [](call& c) { return c.list_attached("role"); }},
                {"PutUserPolicy", [](call& c) { return c.put_inline("user"); }},
                {"PutGroupPolicy", [](call& c) { return c.put_inline("group"); }},
                {"PutRolePolicy", [](call& c) { return c.put_inline("role"); }},
                {"GetUserPolicy", [](call& c) { return c.get_inline("user"); }},
                {"GetGroupPolicy", [](call& c) { return c.get_inline("group"); }},
                {"GetRolePolicy", [](call& c) { return c.get_inline("role"); }},
                {"DeleteUserPolicy", [](call& c) { return c.delete_inline("user"); }},
                {"DeleteGroupPolicy", [](call& c) { return c.delete_inline("group"); }},
                {"DeleteRolePolicy", [](call& c) { return c.delete_inline("role"); }},
                {"ListUserPolicies", [](call& c) { return c.list_inline("user"); }},
                {"ListGroupPolicies", [](call& c) { return c.list_inline("group"); }},
                {"ListRolePolicies", [](call& c) { return c.list_inline("role"); }},
                {"PutUserPermissionsBoundary", [](call& c) { return c.boundary("user", true); }},
                {"PutRolePermissionsBoundary", [](call& c) { return c.boundary("role", true); }},
                {"DeleteUserPermissionsBoundary",
                 [](call& c) { return c.boundary("user", false); }},
                {"DeleteRolePermissionsBoundary",
                 [](call& c) { return c.boundary("role", false); }},
                // Instance profiles (Requirement 9.7)
                {"CreateInstanceProfile", &call::create_instance_profile},
                {"GetInstanceProfile", &call::get_instance_profile},
                {"ListInstanceProfiles", &call::list_instance_profiles},
                {"ListInstanceProfilesForRole", &call::list_instance_profiles_for_role},
                {"DeleteInstanceProfile", &call::delete_instance_profile},
                {"AddRoleToInstanceProfile", [](call& c) { return c.instance_profile_role(true); }},
                {"RemoveRoleFromInstanceProfile",
                 [](call& c) { return c.instance_profile_role(false); }},
                {"TagInstanceProfile", [](call& c) { return c.tag("instance-profile"); }},
                {"UntagInstanceProfile", [](call& c) { return c.untag("instance-profile"); }},
                {"ListInstanceProfileTags",
                 [](call& c) { return c.list_tags("instance-profile"); }},
            };
            const auto it = table.find(action);
            if (it == table.end()) {
                detail::fail("NotImplemented",
                             "The action " + action + " is not implemented by this endpoint.");
            }
            return it->second(*this);
        }

        [[nodiscard]] auto account() const -> const std::string& { return who.account; }

        void read(const std::function<void(const iam_state_machine&)>& fn) { api._c->read(fn); }

        /// Proposes `cmd` and returns its result, or fails with its error.
        auto submit(iam_command type, boost::json::object body) -> boost::json::object {
            body["account"] = account();
            const auto bytes = api._c->submit(iam_leader::command(type, std::move(body)));
            auto v = boost::json::parse(
                std::string_view(reinterpret_cast<const char*>(bytes.data()),  // NOLINT
                                 bytes.size()));
            auto& o = v.as_object();
            if (const auto* e = o.if_contains("error")) {
                detail::fail(std::string(e->as_string()), std::string(o.at("message").as_string()));
            }
            return std::move(o);
        }

        using key_list = std::vector<std::pair<std::string, std::vector<std::string>>>;

        void authorize(const std::string& resource, const tag_map* resource_tags = nullptr,
                       std::optional<validate::tag_list> request_tags = std::nullopt,
                       key_list keys = {}) {
            authorize_as(action, resource, resource_tags, std::move(request_tags), std::move(keys));
        }

        /// Authorizes `iam:<as>` rather than the request's own action: the
        /// iam:PassRole an AddRoleToInstanceProfile also needs.
        void authorize_as(std::string_view as, const std::string& resource,
                          const tag_map* resource_tags = nullptr,
                          std::optional<validate::tag_list> request_tags = std::nullopt,
                          key_list keys = {}) {
            aws_auth::authz_request r;
            r.action = "iam:" + std::string(as);
            r.resource = resource;
            r.keys = std::move(keys);
            if (resource_tags != nullptr) {
                r.resource_tags.assign(resource_tags->begin(), resource_tags->end());
                // IAM's own spelling of the resource-tag key (Requirement 9.8),
                // beside the global aws:ResourceTag/* the authorizer adds.
                for (const auto& [k, v] : *resource_tags) {
                    r.keys.push_back({"iam:ResourceTag/" + k, {v}});
                }
            }
            r.request_tags = std::move(request_tags);
            r.source_ip = facts.source_ip;
            r.user_agent = facts.user_agent;
            r.region = facts.region;
            r.secure_transport = facts.secure_transport;
            const auto d = api._authz->authorize(who, r);
            if (!d.allowed) {
                detail::fail("AccessDenied",
                             "User: " + who.arn + " is not authorized to perform: " + r.action +
                                 " on resource: " + resource +
                                 (d.explicit_deny ? " with an explicit deny"
                                                  : " because no identity-based policy allows "
                                                    "the " +
                                                        r.action + " action"));
            }
        }

        [[nodiscard]] auto required(std::string_view name) const -> std::string {
            const auto v = f.get(name);
            if (!v) {
                detail::fail("ValidationError",
                             "1 validation error detected: Value null at '" + std::string(name) +
                                 "' failed to satisfy constraint: Member must not be null");
            }
            return std::string(*v);
        }

        [[nodiscard]] auto entity_name(std::string_view param, std::string_view kind) const
            -> std::string {
            auto n = required(param);
            if (!iam_leader::valid_name(kind, n)) {
                detail::fail("ValidationError",
                             "The specified value for " + std::string(param) +
                                 " is invalid. It must contain only alphanumeric characters "
                                 "and/or the following: +=,.@_-");
            }
            return n;
        }

        [[nodiscard]] auto optional_name(std::string_view param, std::string_view kind) const
            -> std::optional<std::string> {
            if (!f.contains(param)) {
                return std::nullopt;
            }
            return entity_name(param, kind);
        }

        [[nodiscard]] auto optional_path(std::string_view param) const
            -> std::optional<std::string> {
            const auto v = f.get(param);
            if (!v) {
                return std::nullopt;
            }
            if (!iam_leader::valid_path(*v)) {
                detail::fail("ValidationError",
                             "The specified value for " + std::string(param) +
                                 " is invalid. It must begin and end with / and contain only "
                                 "printable ASCII characters.");
            }
            return std::string(*v);
        }

        /// `Tags.member.N.Key/Value`, checked as a whole before anything is
        /// written (INV-TAG-1..6).
        [[nodiscard]] auto tags(validate::tag_keys case_rule) const -> validate::tag_list {
            static constexpr std::array<std::string_view, 2> fields{"Key", "Value"};
            const auto list = f.member_structs("Tags", fields);
            if (!list) {
                detail::fail("ValidationError", "The Tags parameter is malformed.");
            }
            validate::tag_list out;
            for (const auto& t : *list) {
                const auto k = t.find("Key");
                if (k == t.end()) {
                    detail::fail("ValidationError", "Every tag needs a Key.");
                }
                const auto v = t.find("Value");
                out.emplace_back(k->second, v == t.end() ? std::string{} : v->second);
            }
            if (const auto e = validate::tags_error(out, case_rule)) {
                detail::fail(e->starts_with("Cannot exceed quota") ? "LimitExceeded"
                             : e->starts_with("Duplicate")         ? "InvalidInput"
                                                                   : "ValidationError",
                             *e);
            }
            return out;
        }

        [[nodiscard]] static auto tags_json(const validate::tag_list& tags) -> boost::json::array {
            boost::json::array a;
            for (const auto& [k, v] : tags) {
                a.push_back(boost::json::object{{"Key", k}, {"Value", v}});
            }
            return a;
        }

        [[nodiscard]] auto marker_sealer() -> aws_query::marker_sealer {
            std::string salt;
            read([&](const iam_state_machine& sm) { salt = sm.kek_salt(); });
            return aws_query::marker_sealer(detail::marker_key(api._c->kek(salt)));
        }

        /// The page of `keys` (sorted) after `Marker`, at most `MaxItems`.
        /// Returns the selected keys, and writes IsTruncated/Marker into
        /// `tail` for the caller to append after the list element.
        auto page(const std::vector<std::string>& keys, std::string_view scope, std::string& tail)
            -> std::vector<std::string> {
            const auto max = aws_query::max_items(f);
            if (!max) {
                detail::fail("ValidationError",
                             "1 validation error detected: Value at 'maxItems' failed to satisfy "
                             "constraint: Member must have value between 1 and 1000");
            }
            const auto sealer = marker_sealer();
            const auto context = action + ":" + account() + ":" + std::string(scope);
            auto it = keys.begin();
            if (const auto m = f.get("Marker")) {
                const auto last = sealer.open(context, *m);
                if (!last) {
                    detail::fail("InvalidInput", "Invalid Marker.");
                }
                it = std::upper_bound(keys.begin(), keys.end(), *last);
            }
            std::vector<std::string> out;
            for (; it != keys.end() && out.size() < *max; ++it) {
                out.push_back(*it);
            }
            const bool truncated = it != keys.end();
            aws_query::xml_writer w;
            w.boolean("IsTruncated", truncated);
            if (truncated) {
                w.element("Marker", sealer.seal(context, out.back()));
            }
            tail = w.str();
            return out;
        }

        [[nodiscard]] auto arn(std::string_view kind, std::string_view path, std::string_view name)
            -> std::string {
            std::string partition;
            read([&](const iam_state_machine& sm) { partition = sm.config().partition; });
            return "arn:" + partition + ":iam::" + account() + ":" + std::string(kind) +
                   std::string(path) + std::string(name);
        }

        // ── users ──

        struct user_view {
            iam_user u;
            std::string arn;
        };

        auto load_user(const std::string& name) -> user_view {
            std::optional<user_view> out;
            read([&](const iam_state_machine& sm) {
                if (const auto* u = sm.user(account(), name)) {
                    out = user_view{*u, sm.arn(account(), "user", u->path, u->name)};
                }
            });
            if (!out) {
                // Authorized first, so a caller without access learns
                // nothing about which users exist.
                authorize(arn("user", "/", name));
                detail::fail("NoSuchEntity", "The user with name " + name + " cannot be found.");
            }
            return std::move(*out);
        }

        static void write_user(aws_query::xml_writer& w, const user_view& v, bool with_tags) {
            w.element("Path", v.u.path)
                .element("UserName", v.u.name)
                .element("UserId", v.u.id)
                .element("Arn", v.arn)
                .element("CreateDate", iso8601(v.u.create_date));
            detail::write_boundary(w, v.u.boundary);
            if (with_tags && !v.u.tags.empty()) {
                detail::write_tags(w, v.u.tags);
            }
        }

        /// The IAM user a call without `UserName` acts on: the signer
        /// (INV-KEY-3). Empty for the account root; refused for any other
        /// kind of principal, as AWS does.
        [[nodiscard]] auto self_user() const -> std::string {
            switch (who.type) {
                case aws_auth::principal_type::root:
                    return {};
                case aws_auth::principal_type::user:
                    return who.username;
                default:
                    detail::fail("ValidationError",
                                 "Must specify userName when calling with non-User credentials");
            }
        }

        auto create_user() -> std::string {
            const auto name = entity_name("UserName", "user");
            const auto path = optional_path("Path").value_or("/");
            const auto t = tags(validate::tag_keys::case_insensitive);
            const auto boundary = f.get("PermissionsBoundary");
            std::vector<std::pair<std::string, std::vector<std::string>>> keys;
            if (boundary) {
                keys.push_back({"iam:PermissionsBoundary", {std::string(*boundary)}});
            }
            authorize(arn("user", path, name), nullptr, t, std::move(keys));
            boost::json::object cmd{{"name", name},
                                    {"path", path},
                                    {"id", iam_leader::generate_unique_id("AIDA")},
                                    {"tags", tags_json(t)}};
            if (boundary) {
                cmd["boundary"] = *boundary;
            }
            (void)submit(iam_command::create_user, std::move(cmd));
            aws_query::xml_writer w;
            w.open("User");
            write_user(w, load_user(name), true);
            w.close();
            return w.str();
        }

        auto get_user() -> std::string {
            const auto given = optional_name("UserName", "user");
            const auto name = given ? *given : self_user();
            aws_query::xml_writer w;
            if (name.empty()) {
                // The account root, which has no user record.
                authorize(who.arn);
                std::int64_t created = 0;
                read([&](const iam_state_machine& sm) {
                    if (const auto* a = sm.account(account())) {
                        created = a->create_date;
                    }
                });
                w.open("User")
                    .element("UserId", account())
                    .element("Arn", who.arn)
                    .element("CreateDate", iso8601(created))
                    .close();
                return w.str();
            }
            const auto v = load_user(name);
            authorize(v.arn, &v.u.tags);
            w.open("User");
            write_user(w, v, true);
            w.close();
            return w.str();
        }

        auto list_users() -> std::string {
            const auto prefix = optional_path("PathPrefix").value_or("/");
            authorize("*");
            std::vector<std::string> keys;
            std::map<std::string, user_view> all;
            read([&](const iam_state_machine& sm) {
                if (const auto* a = sm.account(account())) {
                    for (const auto& [k, u] : a->users) {
                        if (u.path.starts_with(prefix)) {
                            keys.push_back(k);
                            all.emplace(k, user_view{u, sm.arn(account(), "user", u.path, u.name)});
                        }
                    }
                }
            });
            std::string tail;
            const auto selected = page(keys, prefix, tail);
            aws_query::xml_writer w;
            w.open("Users");
            for (const auto& k : selected) {
                w.open("member");
                write_user(w, all.at(k), false);
                w.close();
            }
            w.close().raw(tail);
            return w.str();
        }

        auto update_user() -> std::string {
            const auto name = entity_name("UserName", "user");
            const auto new_name = optional_name("NewUserName", "user");
            const auto new_path = optional_path("NewPath");
            const auto v = load_user(name);
            authorize(v.arn, &v.u.tags);
            boost::json::object cmd{{"name", name}};
            if (new_name) {
                cmd["new_name"] = *new_name;
            }
            if (new_path) {
                cmd["new_path"] = *new_path;
            }
            (void)submit(iam_command::update_user, std::move(cmd));
            return {};
        }

        auto delete_user() -> std::string {
            const auto name = entity_name("UserName", "user");
            const auto v = load_user(name);
            authorize(v.arn, &v.u.tags);
            (void)submit(iam_command::delete_user, {{"name", name}});
            return {};
        }

        // ── access keys ──

        /// The user a key action names (or the signer), its ARN for
        /// authorization and its tags. An empty name is the account root.
        struct key_owner {
            std::string name;
            std::string arn;
            tag_map tags;
        };

        auto owner_for_keys() -> key_owner {
            const auto given = optional_name("UserName", "user");
            const auto name = given ? *given : self_user();
            if (name.empty()) {
                return {"", who.arn, {}};
            }
            auto v = load_user(name);
            return {v.u.name, v.arn, v.u.tags};
        }

        /// The key `AccessKeyId` names, which must belong to `owner`: a key
        /// of another user is as unknown as a missing one.
        auto load_key(const key_owner& owner) -> iam_access_key {
            const auto id = required("AccessKeyId");
            std::optional<iam_access_key> out;
            read([&](const iam_state_machine& sm) {
                const auto [a, k] = sm.access_key(id);
                if (a != nullptr && k != nullptr && a->id == account() &&
                    k->owner == iam_detail::lower(owner.name)) {
                    out = *k;
                }
            });
            if (!out) {
                detail::fail("NoSuchEntity", "The Access Key with id " + id + " cannot be found.");
            }
            return std::move(*out);
        }

        static void write_key_metadata(aws_query::xml_writer& w, const iam_access_key& k,
                                       const std::string& user) {
            if (!user.empty()) {
                w.element("UserName", user);
            }
            w.element("AccessKeyId", k.id)
                .element("Status", std::string_view(k.active ? "Active" : "Inactive"))
                .element("CreateDate", iso8601(k.create_date));
        }

        auto create_access_key() -> std::string {
            const auto owner = owner_for_keys();
            authorize(owner.arn, owner.name.empty() ? nullptr : &owner.tags);
            std::string salt;
            read([&](const iam_state_machine& sm) { salt = sm.kek_salt(); });
            auto k = iam_leader::make_access_key(api._c->kek(salt), account(), owner.name);
            const auto bytes = api._c->submit(k.command);
            auto v = boost::json::parse(
                std::string_view(reinterpret_cast<const char*>(bytes.data()),  // NOLINT
                                 bytes.size()));
            if (const auto* e = v.as_object().if_contains("error")) {
                OPENSSL_cleanse(k.secret.data(), k.secret.size());
                detail::fail(std::string(e->as_string()),
                             std::string(v.as_object().at("message").as_string()));
            }
            aws_query::xml_writer w;
            w.open("AccessKey");
            if (!owner.name.empty()) {
                w.element("UserName", owner.name);
            }
            w.element("AccessKeyId", k.key_id)
                .element("Status", std::string_view("Active"))
                .element("SecretAccessKey", k.secret)  // INV-KEY-1: returned once
                .element("CreateDate", iso8601(v.as_object().at("create_date").as_int64()))
                .close();
            OPENSSL_cleanse(k.secret.data(), k.secret.size());
            return w.str();
        }

        auto list_access_keys() -> std::string {
            const auto owner = owner_for_keys();
            authorize(owner.arn, owner.name.empty() ? nullptr : &owner.tags);
            std::vector<std::string> ids;
            std::map<std::string, iam_access_key> all;
            const auto key = iam_detail::lower(owner.name);
            read([&](const iam_state_machine& sm) {
                if (const auto* a = sm.account(account())) {
                    for (const auto& [id, k] : a->access_keys) {
                        if (k.owner == key) {
                            ids.push_back(id);
                            all.emplace(id, k);
                        }
                    }
                }
            });
            std::string tail;
            const auto selected = page(ids, owner.name, tail);
            aws_query::xml_writer w;
            w.open("AccessKeyMetadata");
            for (const auto& id : selected) {
                w.open("member");
                write_key_metadata(w, all.at(id), owner.name);
                w.close();
            }
            w.close().raw(tail);
            if (!owner.name.empty()) {
                w.element("UserName", owner.name);
            }
            return w.str();
        }

        auto update_access_key() -> std::string {
            const auto status = required("Status");
            if (status != "Active" && status != "Inactive") {
                detail::fail("ValidationError",
                             "1 validation error detected: Value '" + status +
                                 "' at 'status' failed to satisfy constraint: Member must satisfy "
                                 "enum value set: [Active, Inactive]");
            }
            const auto owner = owner_for_keys();
            const auto k = load_key(owner);
            authorize(owner.arn, owner.name.empty() ? nullptr : &owner.tags);
            (void)submit(iam_command::update_access_key, {{"key_id", k.id}, {"status", status}});
            return {};
        }

        auto delete_access_key() -> std::string {
            const auto owner = owner_for_keys();
            const auto k = load_key(owner);
            authorize(owner.arn, owner.name.empty() ? nullptr : &owner.tags);
            (void)submit(iam_command::delete_access_key, {{"key_id", k.id}});
            return {};
        }

        auto get_access_key_last_used() -> std::string {
            const auto id = required("AccessKeyId");
            std::optional<iam_access_key> key;
            std::optional<user_view> user;
            read([&](const iam_state_machine& sm) {
                const auto [a, k] = sm.access_key(id);
                if (a == nullptr || k == nullptr || a->id != account()) {
                    return;
                }
                key = *k;
                if (!k->owner.empty()) {
                    if (const auto* u = sm.user(account(), k->owner)) {
                        user = user_view{*u, sm.arn(account(), "user", u->path, u->name)};
                    }
                }
            });
            if (!key) {
                // AWS answers AccessDenied, not NoSuchEntity, so key ids
                // cannot be probed.
                detail::fail("AccessDenied", "User: " + who.arn +
                                                 " is not authorized to perform: "
                                                 "iam:GetAccessKeyLastUsed on resource: " +
                                                 id);
            }
            if (user) {
                authorize(user->arn, &user->u.tags);
            } else {
                authorize(arn("root", "", ""));
            }
            aws_query::xml_writer w;
            w.element("UserName", user ? user->u.name : std::string("<root_account>"));
            w.open("AccessKeyLastUsed");
            if (key->last_used) {
                w.element("LastUsedDate", iso8601(key->last_used->time))
                    .element("ServiceName", key->last_used->service)
                    .element("Region", key->last_used->region);
            } else {
                w.element("ServiceName", std::string_view("N/A"))
                    .element("Region", std::string_view("N/A"));
            }
            w.close();
            return w.str();
        }

        // ── groups ──

        struct group_view {
            iam_group g;
            std::string arn;
        };

        auto load_group(const std::string& name) -> group_view {
            std::optional<group_view> out;
            read([&](const iam_state_machine& sm) {
                if (const auto* g = sm.group(account(), name)) {
                    out = group_view{*g, sm.arn(account(), "group", g->path, g->name)};
                }
            });
            if (!out) {
                authorize(arn("group", "/", name));
                detail::fail("NoSuchEntity", "The group with name " + name + " cannot be found.");
            }
            return std::move(*out);
        }

        static void write_group(aws_query::xml_writer& w, const group_view& v) {
            w.element("Path", v.g.path)
                .element("GroupName", v.g.name)
                .element("GroupId", v.g.id)
                .element("Arn", v.arn)
                .element("CreateDate", iso8601(v.g.create_date));
        }

        auto create_group() -> std::string {
            const auto name = entity_name("GroupName", "group");
            const auto path = optional_path("Path").value_or("/");
            authorize(arn("group", path, name));
            (void)submit(
                iam_command::create_group,
                {{"name", name}, {"path", path}, {"id", iam_leader::generate_unique_id("AGPA")}});
            aws_query::xml_writer w;
            w.open("Group");
            write_group(w, load_group(name));
            w.close();
            return w.str();
        }

        auto get_group() -> std::string {
            const auto name = entity_name("GroupName", "group");
            const auto v = load_group(name);
            authorize(v.arn);
            std::vector<std::string> keys(v.g.members.begin(), v.g.members.end());
            std::map<std::string, user_view> users;
            read([&](const iam_state_machine& sm) {
                for (const auto& k : keys) {
                    if (const auto* u = sm.user(account(), k)) {
                        users.emplace(k,
                                      user_view{*u, sm.arn(account(), "user", u->path, u->name)});
                    }
                }
            });
            std::string tail;
            const auto selected = page(keys, name, tail);
            aws_query::xml_writer w;
            w.open("Group");
            write_group(w, v);
            w.close().open("Users");
            for (const auto& k : selected) {
                if (const auto it = users.find(k); it != users.end()) {
                    w.open("member");
                    write_user(w, it->second, false);
                    w.close();
                }
            }
            w.close().raw(tail);
            return w.str();
        }

        auto list_groups() -> std::string {
            const auto prefix = optional_path("PathPrefix").value_or("/");
            authorize("*");
            std::vector<std::string> keys;
            std::map<std::string, group_view> all;
            read([&](const iam_state_machine& sm) {
                if (const auto* a = sm.account(account())) {
                    for (const auto& [k, g] : a->groups) {
                        if (g.path.starts_with(prefix)) {
                            keys.push_back(k);
                            all.emplace(k,
                                        group_view{g, sm.arn(account(), "group", g.path, g.name)});
                        }
                    }
                }
            });
            std::string tail;
            const auto selected = page(keys, prefix, tail);
            aws_query::xml_writer w;
            w.open("Groups");
            for (const auto& k : selected) {
                w.open("member");
                write_group(w, all.at(k));
                w.close();
            }
            w.close().raw(tail);
            return w.str();
        }

        auto update_group() -> std::string {
            const auto name = entity_name("GroupName", "group");
            const auto new_name = optional_name("NewGroupName", "group");
            const auto new_path = optional_path("NewPath");
            const auto v = load_group(name);
            authorize(v.arn);
            boost::json::object cmd{{"name", name}};
            if (new_name) {
                cmd["new_name"] = *new_name;
            }
            if (new_path) {
                cmd["new_path"] = *new_path;
            }
            (void)submit(iam_command::update_group, std::move(cmd));
            return {};
        }

        auto delete_group() -> std::string {
            const auto name = entity_name("GroupName", "group");
            const auto v = load_group(name);
            authorize(v.arn);
            (void)submit(iam_command::delete_group, {{"name", name}});
            return {};
        }

        auto membership(iam_command type) -> std::string {
            const auto group = entity_name("GroupName", "group");
            const auto user = entity_name("UserName", "user");
            const auto v = load_group(group);
            authorize(v.arn);
            (void)submit(type, {{"group", group}, {"user", user}});
            return {};
        }

        auto add_user_to_group() -> std::string {
            return membership(iam_command::add_user_to_group);
        }
        auto remove_user_from_group() -> std::string {
            return membership(iam_command::remove_user_from_group);
        }

        auto list_groups_for_user() -> std::string {
            const auto name = entity_name("UserName", "user");
            const auto v = load_user(name);
            authorize(v.arn, &v.u.tags);
            std::vector<std::string> keys(v.u.groups.begin(), v.u.groups.end());
            std::map<std::string, group_view> groups;
            read([&](const iam_state_machine& sm) {
                for (const auto& k : keys) {
                    if (const auto* g = sm.group(account(), k)) {
                        groups.emplace(
                            k, group_view{*g, sm.arn(account(), "group", g->path, g->name)});
                    }
                }
            });
            std::string tail;
            const auto selected = page(keys, name, tail);
            aws_query::xml_writer w;
            w.open("Groups");
            for (const auto& k : selected) {
                if (const auto it = groups.find(k); it != groups.end()) {
                    w.open("member");
                    write_group(w, it->second);
                    w.close();
                }
            }
            w.close().raw(tail);
            return w.str();
        }

        // ── parameters shared by task 15's actions ──

        [[nodiscard]] auto limits() -> iam_limits {
            iam_limits out;
            read([&](const iam_state_machine& sm) { out = sm.config().limits; });
            return out;
        }

        [[nodiscard]] auto optional_int(std::string_view param) const
            -> std::optional<std::int64_t> {
            const auto v = f.get(param);
            if (!v) {
                return std::nullopt;
            }
            std::int64_t n = 0;
            const auto* end = v->data() + v->size();
            const auto [p, ec] = std::from_chars(v->data(), end, n);
            if (ec != std::errc{} || p != end || v->empty()) {
                detail::fail("ValidationError", "The specified value for " + std::string(param) +
                                                    " is not a valid integer.");
            }
            return n;
        }

        [[nodiscard]] auto optional_bool(std::string_view param) const -> std::optional<bool> {
            const auto v = f.get(param);
            if (!v) {
                return std::nullopt;
            }
            const auto l = iam_detail::lower(*v);
            if (l != "true" && l != "false") {
                detail::fail("ValidationError", "The specified value for " + std::string(param) +
                                                    " must be true or false.");
            }
            return l == "true";
        }

        /// One of `allowed`, or `fallback` when the parameter is absent.
        [[nodiscard]] auto choice(std::string_view param,
                                  std::initializer_list<std::string_view> allowed,
                                  std::string_view fallback) const -> std::string {
            const auto v = f.get(param);
            if (!v) {
                return std::string(fallback);
            }
            if (std::ranges::find(allowed, *v) == allowed.end()) {
                std::string set;
                for (const auto a : allowed) {
                    set += (set.empty() ? "" : ", ") + std::string(a);
                }
                detail::fail("ValidationError", "1 validation error detected: Value '" +
                                                    std::string(*v) + "' at '" +
                                                    std::string(param) +
                                                    "' failed to satisfy constraint: Member must "
                                                    "satisfy enum value set: [" +
                                                    set + "]");
            }
            return std::string(*v);
        }

        /// INV-ROLE-2 (and the same rule for policy descriptions).
        [[nodiscard]] auto optional_description(std::string_view param) const
            -> std::optional<std::string> {
            const auto v = f.get(param);
            if (!v) {
                return std::nullopt;
            }
            if (!validate::role_description(*v)) {
                detail::fail("ValidationError",
                             "The specified value for " + std::string(param) +
                                 " is invalid. It must be at most 1000 characters of tab, "
                                 "line feed, carriage return and printable characters.");
            }
            return std::string(*v);
        }

        /// INV-ROLE-1: MaxSessionDuration is 3600-43200 seconds.
        [[nodiscard]] auto optional_max_session() const -> std::optional<std::int64_t> {
            const auto v = optional_int("MaxSessionDuration");
            if (v && !validate::max_session_duration(*v)) {
                detail::fail("ValidationError",
                             "The requested DurationSeconds exceeds the MaxSessionDuration set "
                             "for this role. 1 validation error detected: Value '" +
                                 std::to_string(*v) +
                                 "' at 'maxSessionDuration' failed to satisfy constraint: Member "
                                 "must have value between 3600 and 43200");
            }
            return v;
        }

        /// An ARN parameter (INV-ARN-1).
        [[nodiscard]] auto arn_param(std::string_view param) const -> std::string {
            auto v = required(param);
            if (!validate::arn_length(v)) {
                detail::fail("ValidationError", "The specified value for " + std::string(param) +
                                                    " is invalid. It must be 20-2048 characters.");
            }
            return v;
        }

        /// A policy document parameter: INV-POL-1's character and length
        /// rule, then the grammar of `type` within `max` bytes. The parser's
        /// own code (MalformedPolicyDocument or LimitExceeded) is AWS's.
        [[nodiscard]] auto document(std::string_view param, policy_type type, std::size_t max) const
            -> std::string {
            auto doc = required(param);
            if (!validate::policy_document_text(doc)) {
                detail::fail("ValidationError",
                             "The specified value for " + std::string(param) +
                                 " is invalid. It must contain only printable ASCII "
                                 "characters, tab, line feed, carriage return and U+00A0-U+00FF, "
                                 "and be 1-131072 characters long.");
            }
            try {
                (void)parse_policy(doc, type, parse_options{.max_size = max});
            } catch (const policy_error& e) {
                detail::fail(std::string(e.code()), e.what());
            }
            return doc;
        }

        /// `TagKeys.member.N`, at least one, each a valid key.
        [[nodiscard]] auto tag_keys_param() const -> std::vector<std::string> {
            const auto keys = f.members("TagKeys");
            if (!keys || keys->empty()) {
                detail::fail("ValidationError", "The TagKeys parameter must not be empty.");
            }
            for (const auto& k : *keys) {
                if (!validate::tag_key(k)) {
                    detail::fail("ValidationError", "Invalid tag key: " + k);
                }
            }
            return {keys->begin(), keys->end()};
        }

        [[nodiscard]] static auto boundary_keys(const std::optional<std::string>& b) -> key_list {
            if (!b) {
                return {};
            }
            return {{"iam:PermissionsBoundary", {*b}}};
        }

        // ── roles (Requirement 9.4) ──

        struct role_view {
            iam_role r;
            std::string arn;
        };

        auto load_role(const std::string& name) -> role_view {
            std::optional<role_view> out;
            read([&](const iam_state_machine& sm) {
                if (const auto* r = sm.role(account(), name)) {
                    out = role_view{*r, sm.arn(account(), "role", r->path, r->name)};
                }
            });
            if (!out) {
                authorize(arn("role", "/", name));
                detail::fail("NoSuchEntity", "The role with name " + name + " cannot be found.");
            }
            return std::move(*out);
        }

        /// GetRole's full form (`full`) adds the boundary, tags and
        /// RoleLastUsed, which ListRoles leaves out, as AWS does.
        static void write_role(aws_query::xml_writer& w, const role_view& v, bool full) {
            w.element("Path", v.r.path)
                .element("RoleName", v.r.name)
                .element("RoleId", v.r.id)
                .element("Arn", v.arn)
                .element("CreateDate", iso8601(v.r.create_date))
                .element("AssumeRolePolicyDocument", aws_query::url_encode(v.r.trust_policy));
            if (!v.r.description.empty()) {
                w.element("Description", v.r.description);
            }
            w.element("MaxSessionDuration", std::to_string(v.r.max_session_duration));
            if (full) {
                detail::write_boundary(w, v.r.boundary);
                if (!v.r.tags.empty()) {
                    detail::write_tags(w, v.r.tags);
                }
                w.open("RoleLastUsed").close();
            }
        }

        auto role_result(const std::string& name) -> std::string {
            aws_query::xml_writer w;
            w.open("Role");
            write_role(w, load_role(name), true);
            w.close();
            return w.str();
        }

        auto create_role() -> std::string {
            const auto name = entity_name("RoleName", "role");
            const auto path = optional_path("Path").value_or("/");
            const auto trust = document("AssumeRolePolicyDocument", policy_type::trust,
                                        limits().trust_policy_size);
            const auto description = optional_description("Description");
            const auto max_session = optional_max_session();
            const auto t = tags(validate::tag_keys::case_insensitive);
            std::optional<std::string> boundary;
            if (f.contains("PermissionsBoundary")) {
                boundary = arn_param("PermissionsBoundary");
            }
            authorize(arn("role", path, name), nullptr, t, boundary_keys(boundary));
            boost::json::object cmd{{"name", name},
                                    {"path", path},
                                    {"id", iam_leader::generate_unique_id("AROA")},
                                    {"trust_policy", trust},
                                    {"tags", tags_json(t)}};
            if (description) {
                cmd["description"] = *description;
            }
            if (max_session) {
                cmd["max_session_duration"] = *max_session;
            }
            if (boundary) {
                cmd["boundary"] = *boundary;
            }
            (void)submit(iam_command::create_role, std::move(cmd));
            return role_result(name);
        }

        auto get_role() -> std::string {
            const auto name = entity_name("RoleName", "role");
            const auto v = load_role(name);
            authorize(v.arn, &v.r.tags);
            aws_query::xml_writer w;
            w.open("Role");
            write_role(w, v, true);
            w.close();
            return w.str();
        }

        auto list_roles() -> std::string {
            const auto prefix = optional_path("PathPrefix").value_or("/");
            authorize("*");
            std::vector<std::string> keys;
            std::map<std::string, role_view> all;
            read([&](const iam_state_machine& sm) {
                if (const auto* a = sm.account(account())) {
                    for (const auto& [k, r] : a->roles) {
                        if (r.path.starts_with(prefix)) {
                            keys.push_back(k);
                            all.emplace(k, role_view{r, sm.arn(account(), "role", r.path, r.name)});
                        }
                    }
                }
            });
            std::string tail;
            const auto selected = page(keys, prefix, tail);
            aws_query::xml_writer w;
            w.open("Roles");
            for (const auto& k : selected) {
                w.open("member");
                write_role(w, all.at(k), false);
                w.close();
            }
            w.close().raw(tail);
            return w.str();
        }

        /// UpdateRole and UpdateRoleDescription: INV-ROLE-1 and INV-ROLE-2
        /// are checked here, before anything is proposed.
        auto change_role(bool description_only) -> role_view {
            const auto name = entity_name("RoleName", "role");
            const auto description = optional_description("Description");
            if (description_only && !description) {
                (void)required("Description");
            }
            const auto max_session = description_only ? std::nullopt : optional_max_session();
            const auto v = load_role(name);
            authorize(v.arn, &v.r.tags);
            boost::json::object cmd{{"name", name}};
            if (description) {
                cmd["description"] = *description;
            }
            if (max_session) {
                cmd["max_session_duration"] = *max_session;
            }
            (void)submit(iam_command::update_role, std::move(cmd));
            return v;
        }

        auto update_role() -> std::string {
            (void)change_role(false);
            return {};
        }

        auto update_role_description() -> std::string {
            return role_result(change_role(true).r.name);
        }

        auto update_assume_role_policy() -> std::string {
            const auto name = entity_name("RoleName", "role");
            const auto trust =
                document("PolicyDocument", policy_type::trust, limits().trust_policy_size);
            const auto v = load_role(name);
            authorize(v.arn, &v.r.tags);
            (void)submit(iam_command::update_trust_policy,
                         {{"name", name}, {"trust_policy", trust}});
            return {};
        }

        auto delete_role() -> std::string {
            const auto name = entity_name("RoleName", "role");
            const auto v = load_role(name);
            authorize(v.arn, &v.r.tags);
            (void)submit(iam_command::delete_role, {{"name", name}});
            return {};
        }

        // ── managed policies (Requirement 9.5) ──

        struct policy_view {
            std::string name;
            std::string path;
            std::string id;
            std::string arn;
            std::string description;
            int default_version = 1;
            std::int64_t create_date = 0;
            std::int64_t update_date = 0;
            std::size_t attachments = 0;
            std::size_t boundary_uses = 0;
            tag_map tags;
            bool aws_managed = false;
            std::map<int, policy_version> versions;
        };

        /// Attachments and boundary uses of `parn` in this account: AWS
        /// counts them per account for its own managed policies too.
        [[nodiscard]] auto usage(const iam_state_machine& sm, const std::string& parn) const
            -> std::pair<std::size_t, std::size_t> {
            std::size_t attached = 0;
            std::size_t bounded = 0;
            if (const auto* a = sm.account(account())) {
                for (const auto& [_, u] : a->users) {
                    attached += u.attached.count(parn);
                    bounded += (u.boundary == parn) ? 1 : 0;
                }
                for (const auto& [_, g] : a->groups) {
                    attached += g.attached.count(parn);
                }
                for (const auto& [_, r] : a->roles) {
                    attached += r.attached.count(parn);
                    bounded += (r.boundary == parn) ? 1 : 0;
                }
            }
            return {attached, bounded};
        }

        [[nodiscard]] auto find_policy(const iam_state_machine& sm, const std::string& parn) const
            -> std::optional<policy_view> {
            const auto p = detail::split_policy_arn(sm.config().partition, parn);
            if (!p) {
                return std::nullopt;
            }
            policy_view v;
            v.arn = parn;
            if (p->account == "aws") {
                const auto m = find_aws_managed_policy(p->name);
                if (!m || m->path != p->path) {
                    return std::nullopt;
                }
                v.name = std::string(m->name);
                v.path = std::string(m->path);
                v.id = detail::aws_policy_id(m->name);
                v.aws_managed = true;
                v.default_version = iam_detail::parse_version_id(m->version_id).value_or(1);
                v.versions.emplace(v.default_version, policy_version{std::string(m->document), 0});
                std::tie(v.attachments, v.boundary_uses) = usage(sm, parn);
                return v;
            }
            if (p->account != account()) {
                return std::nullopt;
            }
            const auto* mp = sm.managed_policy(account(), p->name);
            if (mp == nullptr || mp->path != p->path) {
                return std::nullopt;
            }
            v.name = mp->name;
            v.path = mp->path;
            v.id = mp->id;
            v.description = mp->description;
            v.default_version = mp->default_version;
            v.create_date = mp->create_date;
            v.update_date = mp->update_date;
            v.attachments = mp->attachment_count;
            v.boundary_uses = mp->boundary_usage_count;
            v.tags = mp->tags;
            v.versions = mp->versions;
            return v;
        }

        auto load_policy(const std::string& parn) -> policy_view {
            std::optional<policy_view> out;
            read([&](const iam_state_machine& sm) { out = find_policy(sm, parn); });
            if (!out) {
                authorize(parn);
                detail::fail("NoSuchEntity", "Policy " + parn + " was not found.");
            }
            return std::move(*out);
        }

        static void write_policy(aws_query::xml_writer& w, const policy_view& v, bool full) {
            w.element("PolicyName", v.name)
                .element("PolicyId", v.id)
                .element("Arn", v.arn)
                .element("Path", v.path)
                .element("DefaultVersionId", iam_detail::version_id(v.default_version))
                .element("AttachmentCount", std::to_string(v.attachments))
                .element("PermissionsBoundaryUsageCount", std::to_string(v.boundary_uses))
                .boolean("IsAttachable", true);
            if (full && !v.description.empty()) {
                w.element("Description", v.description);
            }
            w.element("CreateDate", iso8601(v.create_date))
                .element("UpdateDate", iso8601(v.update_date));
            if (full && !v.tags.empty()) {
                detail::write_tags(w, v.tags);
            }
        }

        auto policy_result(const std::string& parn) -> std::string {
            aws_query::xml_writer w;
            w.open("Policy");
            write_policy(w, load_policy(parn), true);
            w.close();
            return w.str();
        }

        auto create_policy() -> std::string {
            const auto name = entity_name("PolicyName", "policy");
            const auto path = optional_path("Path").value_or("/");
            const auto doc =
                document("PolicyDocument", policy_type::identity, limits().managed_policy_size);
            const auto description = optional_description("Description");
            const auto t = tags(validate::tag_keys::case_sensitive);
            const auto parn = arn("policy", path, name);
            authorize(parn, nullptr, t);
            boost::json::object cmd{{"name", name},
                                    {"path", path},
                                    {"id", iam_leader::generate_unique_id("ANPA")},
                                    {"document", doc},
                                    {"tags", tags_json(t)}};
            if (description) {
                cmd["description"] = *description;
            }
            (void)submit(iam_command::create_policy, std::move(cmd));
            return policy_result(parn);
        }

        auto get_policy() -> std::string {
            const auto v = load_policy(arn_param("PolicyArn"));
            authorize(v.arn, &v.tags);
            aws_query::xml_writer w;
            w.open("Policy");
            write_policy(w, v, true);
            w.close();
            return w.str();
        }

        auto list_policies() -> std::string {
            const auto scope = choice("Scope", {"All", "AWS", "Local"}, "All");
            const bool only_attached = optional_bool("OnlyAttached").value_or(false);
            const auto prefix = optional_path("PathPrefix").value_or("/");
            const auto filter =
                choice("PolicyUsageFilter", {"PermissionsPolicy", "PermissionsBoundary"}, "");
            authorize("*");
            std::vector<std::string> keys;
            std::map<std::string, policy_view> all;
            const auto keep = [&](const policy_view& v) {
                if (!v.path.starts_with(prefix)) {
                    return false;
                }
                if (filter == "PermissionsPolicy" && only_attached) {
                    return v.attachments > 0;
                }
                if (filter == "PermissionsBoundary" && only_attached) {
                    return v.boundary_uses > 0;
                }
                return !only_attached || v.attachments + v.boundary_uses > 0;
            };
            read([&](const iam_state_machine& sm) {
                const auto& partition = sm.config().partition;
                if (scope != "Local") {
                    for (const auto& e : aws_managed_policy_data) {
                        const auto parn = "arn:" + partition + ":iam::aws:policy" +
                                          std::string(e.path) + std::string(e.name);
                        if (auto v = find_policy(sm, parn); v && keep(*v)) {
                            keys.push_back(parn);
                            all.emplace(parn, std::move(*v));
                        }
                    }
                }
                if (scope != "AWS") {
                    if (const auto* a = sm.account(account())) {
                        for (const auto& [_, mp] : a->policies) {
                            const auto parn = sm.arn(account(), "policy", mp.path, mp.name);
                            if (auto v = find_policy(sm, parn); v && keep(*v)) {
                                keys.push_back(parn);
                                all.emplace(parn, std::move(*v));
                            }
                        }
                    }
                }
            });
            std::ranges::sort(keys);
            std::string tail;
            const auto selected =
                page(keys, scope + ":" + prefix + ":" + (only_attached ? "1" : "0") + filter, tail);
            aws_query::xml_writer w;
            w.open("Policies");
            for (const auto& k : selected) {
                w.open("member");
                write_policy(w, all.at(k), false);
                w.close();
            }
            w.close().raw(tail);
            return w.str();
        }

        auto delete_policy() -> std::string {
            const auto v = load_policy(arn_param("PolicyArn"));
            authorize(v.arn, &v.tags);
            (void)submit(iam_command::delete_policy, {{"arn", v.arn}});
            return {};
        }

        /// INV-POL-6: a VersionId parameter's form; its existence is the
        /// state's to say.
        [[nodiscard]] auto version_param() const -> std::string {
            auto v = required("VersionId");
            if (!validate::policy_version_id(v)) {
                detail::fail(
                    "ValidationError",
                    "1 validation error detected: Value '" + v +
                        "' at 'versionId' failed to satisfy constraint: Member must "
                        "satisfy regular expression pattern: v[1-9][0-9]*(\\.[A-Za-z0-9-]*)?");
            }
            return v;
        }

        auto create_policy_version() -> std::string {
            const auto parn = arn_param("PolicyArn");
            const auto doc =
                document("PolicyDocument", policy_type::identity, limits().managed_policy_size);
            const bool set_default = optional_bool("SetAsDefault").value_or(false);
            const auto v = load_policy(parn);
            authorize(v.arn, &v.tags);
            const auto r =
                submit(iam_command::create_policy_version,
                       {{"arn", v.arn}, {"document", doc}, {"set_as_default", set_default}});
            aws_query::xml_writer w;
            w.open("PolicyVersion")
                .element("VersionId", std::string(r.at("version_id").as_string()))
                .boolean("IsDefaultVersion", r.at("is_default").as_bool())
                .element("CreateDate", iso8601(r.at("create_date").as_int64()))
                .close();
            return w.str();
        }

        auto get_policy_version() -> std::string {
            const auto parn = arn_param("PolicyArn");
            const auto vid = version_param();
            const auto v = load_policy(parn);
            authorize(v.arn, &v.tags);
            const auto n = iam_detail::parse_version_id(vid);
            const auto it = n ? v.versions.find(*n) : v.versions.end();
            if (it == v.versions.end()) {
                detail::fail("NoSuchEntity", "Policy " + parn + " version " + vid +
                                                 " does not exist or is not "
                                                 "attachable.");
            }
            aws_query::xml_writer w;
            w.open("PolicyVersion")
                .element("Document", aws_query::url_encode(it->second.document))
                .element("VersionId", iam_detail::version_id(it->first))
                .boolean("IsDefaultVersion", it->first == v.default_version)
                .element("CreateDate", iso8601(it->second.create_date))
                .close();
            return w.str();
        }

        auto list_policy_versions() -> std::string {
            const auto v = load_policy(arn_param("PolicyArn"));
            authorize(v.arn, &v.tags);
            // Zero-padded, so the page's string order is version order.
            std::vector<std::string> keys;
            for (const auto& [n, _] : v.versions) {
                auto k = std::to_string(n);
                keys.push_back(std::string(10 - std::min<std::size_t>(10, k.size()), '0') + k);
            }
            std::string tail;
            const auto selected = page(keys, v.arn, tail);
            aws_query::xml_writer w;
            w.open("Versions");
            for (const auto& k : selected) {
                const int n = std::stoi(k);
                const auto& pv = v.versions.at(n);
                w.open("member")
                    .element("VersionId", iam_detail::version_id(n))
                    .boolean("IsDefaultVersion", n == v.default_version)
                    .element("CreateDate", iso8601(pv.create_date))
                    .close();
            }
            w.close().raw(tail);
            return w.str();
        }

        auto policy_version_change(iam_command type) -> std::string {
            const auto parn = arn_param("PolicyArn");
            const auto vid = version_param();
            const auto v = load_policy(parn);
            authorize(v.arn, &v.tags);
            (void)submit(type, {{"arn", v.arn}, {"version_id", vid}});
            return {};
        }

        auto list_entities_for_policy() -> std::string {
            const auto v = load_policy(arn_param("PolicyArn"));
            const auto filter =
                choice("EntityFilter",
                       {"User", "Role", "Group", "LocalManagedPolicy", "AWSManagedPolicy"}, "");
            const auto prefix = optional_path("PathPrefix").value_or("/");
            const auto usage_filter =
                choice("PolicyUsageFilter", {"PermissionsPolicy", "PermissionsBoundary"}, "");
            authorize(v.arn, &v.tags);
            const bool as_policy = usage_filter != "PermissionsBoundary";
            const bool as_boundary = usage_filter != "PermissionsPolicy";
            const auto uses = [&](const std::set<std::string>& attached,
                                  const std::optional<std::string>& boundary) {
                return (as_policy && attached.contains(v.arn)) ||
                       (as_boundary && boundary == v.arn);
            };
            // Keys "g:", "r:", "u:" plus the entity's key, so one marker
            // walks the three lists in turn.
            std::vector<std::string> keys;
            std::map<std::string, std::pair<std::string, std::string>> names;  // name, id
            read([&](const iam_state_machine& sm) {
                const auto* a = sm.account(account());
                if (a == nullptr) {
                    return;
                }
                if (filter.empty() || filter == "Group") {
                    for (const auto& [k, g] : a->groups) {
                        if (g.path.starts_with(prefix) && uses(g.attached, std::nullopt)) {
                            keys.push_back("g:" + k);
                            names["g:" + k] = {g.name, g.id};
                        }
                    }
                }
                if (filter.empty() || filter == "Role") {
                    for (const auto& [k, r] : a->roles) {
                        if (r.path.starts_with(prefix) && uses(r.attached, r.boundary)) {
                            keys.push_back("r:" + k);
                            names["r:" + k] = {r.name, r.id};
                        }
                    }
                }
                if (filter.empty() || filter == "User") {
                    for (const auto& [k, u] : a->users) {
                        if (u.path.starts_with(prefix) && uses(u.attached, u.boundary)) {
                            keys.push_back("u:" + k);
                            names["u:" + k] = {u.name, u.id};
                        }
                    }
                }
            });
            std::string tail;
            const auto selected =
                page(keys, v.arn + ":" + filter + ":" + prefix + ":" + usage_filter, tail);
            aws_query::xml_writer w;
            for (const auto& [tag, kind, name_el, id_el] :
                 {std::tuple{"PolicyGroups", 'g', "GroupName", "GroupId"},
                  std::tuple{"PolicyUsers", 'u', "UserName", "UserId"},
                  std::tuple{"PolicyRoles", 'r', "RoleName", "RoleId"}}) {
                w.open(tag);
                for (const auto& k : selected) {
                    if (k[0] == kind) {
                        const auto& [n, id] = names.at(k);
                        w.open("member").element(name_el, n).element(id_el, id).close();
                    }
                }
                w.close();
            }
            w.raw(tail);
            return w.str();
        }

        // ── attachments, inline policies and boundaries ──

        /// A user, group or role named by its `UserName`, `GroupName` or
        /// `RoleName` parameter.
        struct principal_view {
            std::string kind;
            std::string name;
            std::string arn;
            tag_map tags;
            std::set<std::string> attached;
            std::map<std::string, inline_policy> inline_policies;
            std::optional<std::string> boundary;
        };

        [[nodiscard]] static auto name_param(std::string_view kind) -> std::string_view {
            return kind == "user" ? "UserName" : kind == "group" ? "GroupName" : "RoleName";
        }

        auto load_principal(std::string_view kind) -> principal_view {
            const auto name = entity_name(name_param(kind), kind);
            if (kind == "user") {
                auto v = load_user(name);
                return {"user",      v.u.name, v.arn, v.u.tags, v.u.attached, v.u.inline_policies,
                        v.u.boundary};
            }
            if (kind == "group") {
                auto v = load_group(name);
                return {"group",     v.g.name, v.arn, {}, v.g.attached, v.g.inline_policies,
                        std::nullopt};
            }
            auto v = load_role(name);
            return {"role",      v.r.name, v.arn, v.r.tags, v.r.attached, v.r.inline_policies,
                    v.r.boundary};
        }

        [[nodiscard]] static auto tags_of(const principal_view& p) -> const tag_map* {
            return p.kind == "group" ? nullptr : &p.tags;
        }

        auto attach(std::string_view kind, bool on) -> std::string {
            const auto p = load_principal(kind);
            const auto parn = arn_param("PolicyArn");
            authorize(p.arn, tags_of(p), std::nullopt, {{"iam:PolicyARN", {parn}}});
            (void)submit(on ? iam_command::attach_policy : iam_command::detach_policy,
                         {{"kind", p.kind}, {"name", p.name}, {"arn", parn}});
            return {};
        }

        auto list_attached(std::string_view kind) -> std::string {
            const auto prefix = optional_path("PathPrefix").value_or("/");
            const auto p = load_principal(kind);
            authorize(p.arn, tags_of(p));
            std::vector<std::string> keys;
            for (const auto& parn : p.attached) {
                const auto s = detail::split_policy_arn_any(parn);
                if (s && s->path.starts_with(prefix)) {
                    keys.push_back(parn);
                }
            }
            std::string tail;
            const auto selected = page(keys, p.kind + ":" + p.name + ":" + prefix, tail);
            aws_query::xml_writer w;
            w.open("AttachedPolicies");
            for (const auto& parn : selected) {
                w.open("member")
                    .element("PolicyName", detail::split_policy_arn_any(parn)->name)
                    .element("PolicyArn", parn)
                    .close();
            }
            w.close().raw(tail);
            return w.str();
        }

        [[nodiscard]] auto inline_name() const -> std::string {
            auto n = required("PolicyName");
            if (!validate::inline_policy_name(n)) {
                detail::fail("ValidationError",
                             "The specified value for policyName is invalid. It must contain "
                             "only alphanumeric characters and/or the following: +=,.@_-");
            }
            return n;
        }

        auto put_inline(std::string_view kind) -> std::string {
            const auto pname = inline_name();
            const auto l = limits();
            const auto max = kind == "user"    ? l.user_inline_total
                             : kind == "group" ? l.group_inline_total
                                               : l.role_inline_total;
            const auto doc = document("PolicyDocument", policy_type::identity, max);
            const auto p = load_principal(kind);
            authorize(p.arn, tags_of(p));
            (void)submit(
                iam_command::put_inline_policy,
                {{"kind", p.kind}, {"name", p.name}, {"policy_name", pname}, {"document", doc}});
            return {};
        }

        auto get_inline(std::string_view kind) -> std::string {
            const auto pname = inline_name();
            const auto p = load_principal(kind);
            authorize(p.arn, tags_of(p));
            const auto it = p.inline_policies.find(iam_detail::lower(pname));
            if (it == p.inline_policies.end()) {
                detail::fail("NoSuchEntity",
                             "The " + p.kind + " policy with name " + pname + " cannot be found.");
            }
            aws_query::xml_writer w;
            w.element(name_param(kind), p.name)
                .element("PolicyName", it->second.name)
                .element("PolicyDocument", aws_query::url_encode(it->second.document));
            return w.str();
        }

        auto delete_inline(std::string_view kind) -> std::string {
            const auto pname = inline_name();
            const auto p = load_principal(kind);
            authorize(p.arn, tags_of(p));
            (void)submit(iam_command::delete_inline_policy,
                         {{"kind", p.kind}, {"name", p.name}, {"policy_name", pname}});
            return {};
        }

        auto list_inline(std::string_view kind) -> std::string {
            const auto p = load_principal(kind);
            authorize(p.arn, tags_of(p));
            std::vector<std::string> keys;
            for (const auto& [k, _] : p.inline_policies) {
                keys.push_back(k);
            }
            std::string tail;
            const auto selected = page(keys, p.kind + ":" + p.name, tail);
            aws_query::xml_writer w;
            w.open("PolicyNames");
            for (const auto& k : selected) {
                w.element("member", p.inline_policies.at(k).name);
            }
            w.close().raw(tail);
            return w.str();
        }

        /// Put/Delete{User,Role}PermissionsBoundary: iam:PermissionsBoundary
        /// is the boundary being set, or the one being removed.
        auto boundary(std::string_view kind, bool put) -> std::string {
            const auto p = load_principal(kind);
            std::optional<std::string> b;
            if (put) {
                b = arn_param("PermissionsBoundary");
            } else {
                b = p.boundary;
            }
            authorize(p.arn, tags_of(p), std::nullopt, boundary_keys(b));
            boost::json::object cmd{{"kind", p.kind}, {"name", p.name}};
            if (put) {
                cmd["arn"] = *b;
            }
            (void)submit(put ? iam_command::put_boundary : iam_command::delete_boundary,
                         std::move(cmd));
            return {};
        }

        // ── tags on users, roles, policies and instance profiles ──

        struct taggable {
            std::string name;  // what the tag commands name the entity by
            std::string arn;
            tag_map tags;
        };

        [[nodiscard]] static auto tag_rule(std::string_view kind) -> validate::tag_keys {
            return kind == "user" || kind == "role" ? validate::tag_keys::case_insensitive
                                                    : validate::tag_keys::case_sensitive;
        }

        auto load_taggable(std::string_view kind) -> taggable {
            if (kind == "user" || kind == "role") {
                const auto p = load_principal(kind);
                return {p.name, p.arn, p.tags};
            }
            if (kind == "policy") {
                const auto v = load_policy(arn_param("PolicyArn"));
                if (v.aws_managed && action != "ListPolicyTags") {
                    authorize(v.arn);
                    detail::fail("InvalidInput", "AWS-managed policies are read-only.");
                }
                return {v.name, v.arn, v.tags};
            }
            const auto v =
                load_instance_profile(entity_name("InstanceProfileName", "instance-profile"));
            return {v.p.name, v.arn, v.p.tags};
        }

        auto tag(std::string_view kind) -> std::string {
            const auto t = tags(tag_rule(kind));
            if (t.empty()) {
                detail::fail("ValidationError", "The Tags parameter must not be empty.");
            }
            const auto e = load_taggable(kind);
            authorize(e.arn, &e.tags, t);
            (void)submit(iam_command::tag_entity,
                         {{"kind", kind}, {"name", e.name}, {"tags", tags_json(t)}});
            return {};
        }

        auto untag(std::string_view kind) -> std::string {
            const auto keys = tag_keys_param();
            const auto e = load_taggable(kind);
            authorize(e.arn, &e.tags, std::nullopt, {{"aws:TagKeys", keys}});
            boost::json::array ka;
            for (const auto& k : keys) {
                ka.emplace_back(k);
            }
            (void)submit(iam_command::untag_entity,
                         {{"kind", kind}, {"name", e.name}, {"keys", std::move(ka)}});
            return {};
        }

        auto list_tags(std::string_view kind) -> std::string {
            const auto e = load_taggable(kind);
            authorize(e.arn, &e.tags);
            std::vector<std::string> keys;
            for (const auto& [k, _] : e.tags) {
                keys.push_back(k);
            }
            std::string tail;
            const auto selected = page(keys, std::string(kind) + ":" + e.name, tail);
            aws_query::xml_writer w;
            w.open("Tags");
            for (const auto& k : selected) {
                w.open("member").element("Key", k).element("Value", e.tags.at(k)).close();
            }
            w.close().raw(tail);
            return w.str();
        }

        // ── instance profiles (Requirement 9.7) ──

        struct profile_view {
            iam_instance_profile p;
            std::string arn;
            std::vector<role_view> roles;
        };

        [[nodiscard]] auto view_of(const iam_state_machine& sm, const iam_instance_profile& p) const
            -> profile_view {
            profile_view v{p, sm.arn(account(), "instance-profile", p.path, p.name), {}};
            for (const auto& k : p.roles) {
                if (const auto* r = sm.role(account(), k)) {
                    v.roles.push_back(role_view{*r, sm.arn(account(), "role", r->path, r->name)});
                }
            }
            return v;
        }

        auto load_instance_profile(const std::string& name) -> profile_view {
            std::optional<profile_view> out;
            read([&](const iam_state_machine& sm) {
                if (const auto* p = sm.instance_profile(account(), name)) {
                    out = view_of(sm, *p);
                }
            });
            if (!out) {
                authorize(arn("instance-profile", "/", name));
                detail::fail("NoSuchEntity", "Instance Profile " + name + " cannot be found.");
            }
            return std::move(*out);
        }

        static void write_profile(aws_query::xml_writer& w, const profile_view& v, bool full) {
            w.element("Path", v.p.path)
                .element("InstanceProfileName", v.p.name)
                .element("InstanceProfileId", v.p.id)
                .element("Arn", v.arn)
                .element("CreateDate", iso8601(v.p.create_date));
            w.open("Roles");
            for (const auto& r : v.roles) {
                w.open("member");
                write_role(w, r, false);
                w.close();
            }
            w.close();
            if (full && !v.p.tags.empty()) {
                detail::write_tags(w, v.p.tags);
            }
        }

        auto profile_result(const std::string& name) -> std::string {
            aws_query::xml_writer w;
            w.open("InstanceProfile");
            write_profile(w, load_instance_profile(name), true);
            w.close();
            return w.str();
        }

        auto create_instance_profile() -> std::string {
            const auto name = entity_name("InstanceProfileName", "instance-profile");
            const auto path = optional_path("Path").value_or("/");
            const auto t = tags(validate::tag_keys::case_sensitive);
            authorize(arn("instance-profile", path, name), nullptr, t);
            (void)submit(iam_command::create_instance_profile,
                         {{"name", name},
                          {"path", path},
                          {"id", iam_leader::generate_unique_id("AIPA")},
                          {"tags", tags_json(t)}});
            return profile_result(name);
        }

        auto get_instance_profile() -> std::string {
            const auto v =
                load_instance_profile(entity_name("InstanceProfileName", "instance-profile"));
            authorize(v.arn, &v.p.tags);
            aws_query::xml_writer w;
            w.open("InstanceProfile");
            write_profile(w, v, true);
            w.close();
            return w.str();
        }

        auto write_profiles(std::map<std::string, profile_view>& all, std::string_view scope)
            -> std::string {
            std::vector<std::string> keys;
            for (const auto& [k, _] : all) {
                keys.push_back(k);
            }
            std::string tail;
            const auto selected = page(keys, scope, tail);
            aws_query::xml_writer w;
            w.open("InstanceProfiles");
            for (const auto& k : selected) {
                w.open("member");
                write_profile(w, all.at(k), false);
                w.close();
            }
            w.close().raw(tail);
            return w.str();
        }

        auto list_instance_profiles() -> std::string {
            const auto prefix = optional_path("PathPrefix").value_or("/");
            authorize("*");
            std::map<std::string, profile_view> all;
            read([&](const iam_state_machine& sm) {
                if (const auto* a = sm.account(account())) {
                    for (const auto& [k, p] : a->instance_profiles) {
                        if (p.path.starts_with(prefix)) {
                            all.emplace(k, view_of(sm, p));
                        }
                    }
                }
            });
            return write_profiles(all, prefix);
        }

        auto list_instance_profiles_for_role() -> std::string {
            const auto v = load_role(entity_name("RoleName", "role"));
            authorize(v.arn, &v.r.tags);
            std::map<std::string, profile_view> all;
            read([&](const iam_state_machine& sm) {
                for (const auto& k : v.r.instance_profiles) {
                    if (const auto* p = sm.instance_profile(account(), k)) {
                        all.emplace(k, view_of(sm, *p));
                    }
                }
            });
            return write_profiles(all, v.r.name);
        }

        auto delete_instance_profile() -> std::string {
            const auto v =
                load_instance_profile(entity_name("InstanceProfileName", "instance-profile"));
            authorize(v.arn, &v.p.tags);
            (void)submit(iam_command::delete_instance_profile, {{"name", v.p.name}});
            return {};
        }

        /// Adding a role also needs iam:PassRole on it, with
        /// iam:PassedToService ec2.amazonaws.com and the profile as
        /// iam:AssociatedResourceArn, as AWS requires.
        auto instance_profile_role(bool add) -> std::string {
            const auto pname = entity_name("InstanceProfileName", "instance-profile");
            const auto rname = entity_name("RoleName", "role");
            const auto p = load_instance_profile(pname);
            const auto r = load_role(rname);
            authorize(p.arn, &p.p.tags);
            if (add) {
                authorize_as("PassRole", r.arn, &r.r.tags, std::nullopt,
                             {{"iam:PassedToService", {"ec2.amazonaws.com"}},
                              {"iam:AssociatedResourceArn", {p.arn}}});
            }
            (void)submit(add ? iam_command::add_role_to_instance_profile
                             : iam_command::remove_role_from_instance_profile,
                         {{"instance_profile", p.p.name}, {"role", r.r.name}});
            return {};
        }

        // ── account alias (INV-ACCT-2) ──

        auto create_account_alias() -> std::string {
            const auto alias = required("AccountAlias");
            if (!validate::account_alias(alias)) {
                detail::fail("ValidationError",
                             "The specified value for accountAlias is invalid. It must be 3-63 "
                             "lower-case letters, digits and single hyphens, neither starting "
                             "nor ending with a hyphen, and not a 12-digit number.");
            }
            authorize("*");
            (void)submit(iam_command::set_account_alias, {{"alias", alias}});
            return {};
        }

        auto current_alias() -> std::string {
            std::string out;
            read([&](const iam_state_machine& sm) {
                if (const auto* a = sm.account(account())) {
                    out = a->alias;
                }
            });
            return out;
        }

        auto delete_account_alias() -> std::string {
            const auto alias = required("AccountAlias");
            authorize("*");
            if (alias != current_alias()) {
                detail::fail("NoSuchEntity", "The account alias " + alias + " cannot be found.");
            }
            (void)submit(iam_command::set_account_alias, {{"alias", ""}});
            return {};
        }

        auto list_account_aliases() -> std::string {
            authorize("*");
            const auto alias = current_alias();
            aws_query::xml_writer w;
            w.open("AccountAliases");
            if (!alias.empty()) {
                w.element("member", alias);
            }
            w.close().boolean("IsTruncated", false);
            return w.str();
        }
    };

    C* _c;
    iam_authorizer* _authz;
};

}  // namespace kythira::iam::query
