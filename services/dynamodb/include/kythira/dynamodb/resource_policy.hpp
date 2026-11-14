// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief Resource-based policies (`.kiro/specs/dynamodb-compatible-api/`
/// task 21, Requirement 5.3): `PutResourcePolicy`, `GetResourcePolicy` and
/// `DeleteResourcePolicy` on tables and streams, and the lookup that hands
/// them to the authorizer.
///
/// A policy is stored in the table's catalog descriptor
/// (`table_descriptor::policies`), the table's own and one per stream, and
/// changes through a `set_resource_policy` entry, so `ExpectedRevisionId`
/// is checked atomically. Revision ids are decimal milliseconds; putting
/// the document already attached keeps its revision, as AWS's idempotent
/// put does. `CreateTable` takes a `ResourcePolicy` too (`table_api.hpp`).
///
/// Evaluating a policy is the authorizer's job, never this module's:
/// `resource_lookup_for()` gives the front end each table's or stream's
/// policy and tags, which it puts on every authorization request, where
/// IAM's evaluator combines them with the caller's identity policies as
/// AWS does (an index is governed by its table's policy). Two AWS
/// safeguards ride on that:
///
/// - `PutResourcePolicy` asks the authorizer whether the new policy still
///   lets the caller put a policy, and refuses one that would not unless
///   `ConfirmRemoveSelfResourceAccess` is true.
/// - The account root may always `DeleteResourcePolicy` (`front_end.hpp`).
///
/// AWS applies a policy eventually; here it applies from the next request.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/table_api.hpp>

#include <raft/aws_json_protocol.hpp>
#include <raft/aws_request_authorizer.hpp>

#include <boost/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kythira::dynamodb {

namespace resource_policy_detail {

/// What an ARN names: a table, one of its indexes, or one of its streams
/// (by label).
struct arn_parts {
    std::string table;
    std::string index;
    std::string stream_label;
    /// The ARN up to and including the table name.
    std::string table_arn;
    /// The account field: arn:<partition>:dynamodb:<region>:<account>:...
    std::string account;
};

/// The parts of a DynamoDB table, index or stream ARN, or nothing.
[[nodiscard]] inline auto parse_arn(std::string_view arn) -> std::optional<arn_parts> {
    const auto at = arn.find(":table/");
    if (!arn.starts_with("arn:") || at == std::string_view::npos) {
        return std::nullopt;
    }
    arn_parts out;
    auto rest = arn.substr(at + 7);
    const auto slash = rest.find('/');
    out.table = std::string(rest.substr(0, slash));
    out.table_arn = std::string(arn.substr(0, at + 7 + out.table.size()));
    const auto head = arn.substr(0, at);
    out.account = std::string(head.substr(head.rfind(':') + 1));
    if (slash != std::string_view::npos) {
        const auto tail = rest.substr(slash);
        if (tail.starts_with("/index/") && tail.size() > 7) {
            out.index = std::string(tail.substr(7));
        } else if (tail.starts_with("/stream/") && tail.size() > 8) {
            out.stream_label = std::string(tail.substr(8));
        } else {
            return std::nullopt;
        }
    }
    if (out.table.empty()) {
        return std::nullopt;
    }
    return out;
}

/// The stream of `d` whose label is `label`, or null.
[[nodiscard]] inline auto stream_named(const table_descriptor& d, std::string_view label)
    -> const table_stream* {
    for (const auto& s : d.streams) {
        if (table_api_detail::stream_label(s.label_ms) == label) {
            return &s;
        }
    }
    return nullptr;
}

[[nodiscard]] inline auto policy_not_found(std::string_view arn) -> aws_json::error {
    return table_api_detail::modeled(
        "PolicyNotFoundException",
        "Resource-based policy not found for the provided ResourceArn: " + std::string(arn));
}

}  // namespace resource_policy_detail

/// The front end's resource lookup over `backend`: a table's tags and
/// policy for the table and its indexes, its tags and the stream's policy
/// for a stream. Each authorization reads the catalog once more, so a
/// composition root sets it only with an authorizer that evaluates them.
/// `namespaced` says whether tables are per account (task 28): then the
/// table is looked up under the ARN's account.
template<dynamo_backend Backend>
[[nodiscard]] auto resource_lookup_for(Backend& backend, std::function<bool()> namespaced = {})
    -> resource_lookup {
    return [&backend, namespaced = std::move(namespaced)](
               std::string_view arn) -> std::optional<resource_facts> {
        namespace rp = resource_policy_detail;
        const auto parts = rp::parse_arn(arn);
        if (!parts) {
            return std::nullopt;
        }
        const auto d = backend.describe(
            namespaced && namespaced() ? parts->account + "/" + parts->table : parts->table);
        if (!d || d->status == table_status::deleting) {
            return std::nullopt;
        }
        resource_facts facts;
        facts.tags = d->tags;
        std::uint64_t label = 0;
        if (!parts->stream_label.empty()) {
            const auto* s = rp::stream_named(*d, parts->stream_label);
            if (s == nullptr) {
                return std::nullopt;
            }
            label = s->label_ms;
        }
        if (const auto* p = d->policy_of(label)) {
            facts.policy = p->document;
        }
        return facts;
    };
}

template<dynamo_backend Backend> class resource_policy_api {
public:
    using result = aws_json::result<boost::json::object>;

    explicit resource_policy_api(Backend& backend) : _backend(backend) {}

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("PutResourcePolicy",
              [this](call_context& c, boost::json::object& in) { return put(c, in); });
        fe.on("GetResourcePolicy",
              [this](call_context& c, boost::json::object& in) { return get(c, in); });
        fe.on("DeleteResourcePolicy",
              [this](call_context& c, boost::json::object& in) { return remove(c, in); });
    }

    auto put(call_context& ctx, boost::json::object& in) -> result {
        const std::string arn(in.at("ResourceArn").as_string());
        const std::string document(in.at("Policy").as_string());
        if (auto e = table_api_detail::policy_document_error(document)) {
            return *e;
        }
        auto target = resolve(ctx, arn);
        if (!target) {
            return target.error();
        }
        const auto* confirm = in.if_contains("ConfirmRemoveSelfResourceAccess");
        if (confirm == nullptr || !confirm->is_bool() || !confirm->as_bool()) {
            aws_auth::authz_request again;
            again.action = "dynamodb:PutResourcePolicy";
            again.resource = arn;
            again.resource_policy = document;
            if (ctx.authorize && ctx.authorize(std::move(again))) {
                return validation_error(
                    "The resource-based policy would remove your permission to change the "
                    "resource-based policy of this resource. To attach it anyway, set "
                    "ConfirmRemoveSelfResourceAccess to true.");
            }
        }
        const auto r = apply(*target, document, expected_of(in));
        if (!r) {
            return failed(r.error(), arn, *target, expected_of(in));
        }
        const auto* p = r->policy_of(target->label);
        boost::json::object out;
        if (p != nullptr) {
            out["RevisionId"] = p->revision;
        }
        return out;
    }

    auto get(call_context& ctx, boost::json::object& in) -> result {
        const std::string arn(in.at("ResourceArn").as_string());
        auto target = resolve(ctx, arn);
        if (!target) {
            return target.error();
        }
        const auto* p = target->table.policy_of(target->label);
        if (p == nullptr) {
            return resource_policy_detail::policy_not_found(arn);
        }
        return boost::json::object{{"Policy", p->document}, {"RevisionId", p->revision}};
    }

    /// Idempotent without `ExpectedRevisionId`: deleting a policy that is
    /// not there succeeds, answering no revision.
    auto remove(call_context& ctx, boost::json::object& in) -> result {
        const std::string arn(in.at("ResourceArn").as_string());
        auto target = resolve(ctx, arn);
        if (!target) {
            return target.error();
        }
        const auto* before = target->table.policy_of(target->label);
        const std::string revision = before == nullptr ? std::string{} : before->revision;
        const auto r = apply(*target, "", expected_of(in));
        if (!r) {
            return failed(r.error(), arn, *target, expected_of(in));
        }
        boost::json::object out;
        if (!revision.empty()) {
            out["RevisionId"] = revision;
        }
        return out;
    }

private:
    /// A policy's resource: the table, and the stream's label (0 for the
    /// table).
    struct resource {
        table_descriptor table;
        std::uint64_t label = 0;
    };

    static auto expected_of(const boost::json::object& in) -> std::string {
        const auto* v = in.if_contains("ExpectedRevisionId");
        return v != nullptr && v->is_string() ? std::string(v->as_string()) : std::string{};
    }

    static auto arn_not_found(std::string_view arn) -> aws_json::error {
        return table_api_detail::modeled(
            "ResourceNotFoundException",
            "Requested resource not found: ResourcArn: " + std::string(arn) + " not found");
    }

    /// The table or stream `arn` names in this account and region.
    auto resolve(const call_context& ctx, const std::string& arn) -> aws_json::result<resource> {
        namespace rp = resource_policy_detail;
        const auto parts = rp::parse_arn(arn);
        if (!parts || !table_api_detail::valid_table_name(parts->table)) {
            return validation_error("Invalid ResourceArn provided as input " + arn);
        }
        if (!parts->index.empty()) {
            return validation_error(
                "Resource-based policies attach to tables and streams; an index is governed by "
                "its table's policy: " +
                arn);
        }
        // The catalog name: under the ARN's account with namespaces on.
        const auto name = ctx.table_name(parts->table_arn);
        if (ctx.table_arn(name) != parts->table_arn) {
            return arn_not_found(arn);
        }
        auto d = _backend.describe(name);
        if (!d || d->status == table_status::deleting) {
            return arn_not_found(arn);
        }
        resource out{std::move(*d), 0};
        if (!parts->stream_label.empty()) {
            const auto* s = rp::stream_named(out.table, parts->stream_label);
            if (s == nullptr) {
                return arn_not_found(arn);
            }
            out.label = s->label_ms;
        }
        return out;
    }

    /// Runs `set_resource_policy`; the catalog's refusal as an error whose
    /// type is the bare exception name.
    auto apply(const resource& target, std::string document, std::string expected)
        -> aws_json::result<table_descriptor> {
        catalog_command c;
        c.op = catalog_command::kind::set_resource_policy;
        c.table.name = target.table.name;
        c.table.policies = {
            resource_policy{target.label, std::move(document), std::move(expected)}};
        c.now_ms = _backend.now_ms();
        auto r = _backend.catalog(c);
        if (!r.ok()) {
            aws_json::error e;
            e.type = r.error_type;
            e.message = r.error_message;
            return e;
        }
        return std::move(*r.table);
    }

    /// A catalog refusal in DynamoDB's words.
    static auto failed(const aws_json::error& e, const std::string& arn, const resource& target,
                       const std::string& expected) -> aws_json::error {
        if (e.type.ends_with("ResourceNotFoundException")) {
            return arn_not_found(arn);
        }
        if (e.type.ends_with("PolicyNotFoundException")) {
            const auto* p = target.table.policy_of(target.label);
            if (expected.empty() || expected == "NO_POLICY" || p == nullptr) {
                return expected == "NO_POLICY"
                           ? table_api_detail::modeled(
                                 "PolicyNotFoundException",
                                 "Resource-based policy not found for the provided ResourceArn: "
                                 "A policy already exists for " +
                                     arn + " but NO_POLICY was expected.")
                           : resource_policy_detail::policy_not_found(arn);
            }
            return table_api_detail::modeled(
                "PolicyNotFoundException",
                "Resource-based policy not found for the provided ResourceArn: Requested update "
                "for policy with revision id " +
                    expected + ", but the policy associated to target resource " + arn +
                    " has revision id " + p->revision + ".");
        }
        catalog_result r;
        r.error_type = e.type;
        r.error_message = e.message.value_or("");
        return table_api_detail::error_of(r);
    }

    Backend& _backend;
};

}  // namespace kythira::dynamodb
