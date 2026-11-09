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
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
            using handler = std::string (call::*)();
            static const std::map<std::string_view, handler, std::less<>> table{
                {"CreateUser", &call::create_user},
                {"GetUser", &call::get_user},
                {"ListUsers", &call::list_users},
                {"UpdateUser", &call::update_user},
                {"DeleteUser", &call::delete_user},
                {"TagUser", &call::tag_user},
                {"UntagUser", &call::untag_user},
                {"ListUserTags", &call::list_user_tags},
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
            };
            const auto it = table.find(action);
            if (it == table.end()) {
                detail::fail("NotImplemented",
                             "The action " + action + " is not implemented by this endpoint.");
            }
            return (this->*(it->second))();
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

        void authorize(const std::string& resource, const tag_map* resource_tags = nullptr,
                       std::optional<validate::tag_list> request_tags = std::nullopt,
                       std::vector<std::pair<std::string, std::vector<std::string>>> keys = {}) {
            aws_auth::authz_request r;
            r.action = "iam:" + action;
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

        auto tag_user() -> std::string {
            const auto name = entity_name("UserName", "user");
            const auto t = tags(validate::tag_keys::case_insensitive);
            if (t.empty()) {
                detail::fail("ValidationError", "The Tags parameter must not be empty.");
            }
            const auto v = load_user(name);
            authorize(v.arn, &v.u.tags, t);
            (void)submit(iam_command::tag_entity,
                         {{"kind", "user"}, {"name", name}, {"tags", tags_json(t)}});
            return {};
        }

        auto untag_user() -> std::string {
            const auto name = entity_name("UserName", "user");
            const auto keys = f.members("TagKeys");
            if (!keys || keys->empty()) {
                detail::fail("ValidationError", "The TagKeys parameter must not be empty.");
            }
            for (const auto& k : *keys) {
                if (!validate::tag_key(k)) {
                    detail::fail("ValidationError", "Invalid tag key: " + k);
                }
            }
            const auto v = load_user(name);
            authorize(v.arn, &v.u.tags, std::nullopt,
                      {{"aws:TagKeys", std::vector<std::string>(keys->begin(), keys->end())}});
            boost::json::array ka;
            for (const auto& k : *keys) {
                ka.emplace_back(k);
            }
            (void)submit(iam_command::untag_entity,
                         {{"kind", "user"}, {"name", name}, {"keys", std::move(ka)}});
            return {};
        }

        auto list_user_tags() -> std::string {
            const auto name = entity_name("UserName", "user");
            const auto v = load_user(name);
            authorize(v.arn, &v.u.tags);
            std::vector<std::string> keys;
            for (const auto& [k, _] : v.u.tags) {
                keys.push_back(k);
            }
            std::string tail;
            const auto selected = page(keys, name, tail);
            aws_query::xml_writer w;
            w.open("Tags");
            for (const auto& k : selected) {
                w.open("member").element("Key", k).element("Value", v.u.tags.at(k)).close();
            }
            w.close().raw(tail);
            return w.str();
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
