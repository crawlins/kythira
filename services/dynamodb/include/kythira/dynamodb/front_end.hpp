// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file front_end.hpp
/// @brief The DynamoDB and DynamoDB Streams front end: SigV4, authorization,
///        framing and dispatch (`.kiro/specs/dynamodb-compatible-api/`
///        task 4, Requirements 4 and 5).
///
/// One request, in order:
///
///   1. The `X-Amz-Target` prefix picks the service (DynamoDB or Streams);
///      anything else answers UnknownOperationException.
///   2. SigV4 through the authorizer (`aws_request_authorizer`: the IAM
///      authorizer, or `static_key_authorizer` without IAM). Both services
///      sign as `dynamodb`. Authentication comes before the body is read,
///      as AWS does it, so an unsigned caller learns nothing about the API.
///   3. A modeled operation with no handler answers UnknownOperationException
///      (the operations `doc/unsupported.md` lists); then the codec frames
///      and validates the body.
///   4. Authorization: the operation's same-named `dynamodb:` action on the
///      resource the input names (a table, or the index a Query or Scan
///      names, a stream, a backup, or `ResourceArn` as given), with the
///      `dynamodb:ReturnValues`, `ReturnConsumedCapacity` and `Select`
///      condition keys when the input sets them. Operations without a
///      same-named action (transactions, PartiQL) and the keys that need the
///      parsed item (`LeadingKeys`, `Attributes`) are authorized by their
///      handlers through `call_context::authorize`, from
///      `k_operation_actions`.
///   5. The handler. An exception becomes InternalServerError (500).
///
/// `DescribeEndpoints` is built in: it answers the node's advertised
/// endpoint with AWS's 1440-minute cache period (4.4).
///
/// The front end is transport-free: the composition root adapts the shared
/// Beast host's request into `front_end_request` and the response back.
/// Forwarding to shard leaders is the item handlers' business (task 7):
/// a handler routes to the group that owns the key.

#include <kythira/dynamodb/dynamodb_actions.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/operations.hpp>

#include <raft/aws_json_protocol.hpp>
#include <raft/aws_request_authorizer.hpp>
#include <raft/sigv4_verifier.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

struct front_end_config {
    std::string region = "us-east-1";
    std::string partition = "aws";
    /// What DescribeEndpoints answers: this node's address as clients reach
    /// it (`host:port`).
    std::string advertised_endpoint;
    aws_json::request_limits limits{};
};

/// An HTTP request as the transport hands it over.
struct front_end_request {
    std::string method;
    std::string path;
    std::string query;  ///< without '?'
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string source_ip;
    bool secure = true;
};

/// What a handler sees besides its input.
struct call_context {
    std::string_view operation;
    std::string request_id;
    aws_auth::principal_context principal;
    std::string region;
    std::string partition;
    /// Authorizes one more action (a transaction element, a PartiQL
    /// statement, a condition key the front end could not supply). On
    /// refusal, the AccessDeniedException to return.
    std::function<std::optional<aws_json::error>(aws_auth::authz_request)> authorize;

    /// `arn:<partition>:dynamodb:<region>:<account>:table/<name>`.
    [[nodiscard]] auto table_arn(std::string_view table) const -> std::string {
        return "arn:" + partition + ":dynamodb:" + region + ":" + principal.account + ":table/" +
               std::string(table);
    }
};

using front_end_handler =
    std::function<aws_json::result<boost::json::object>(call_context&, boost::json::object&)>;

namespace detail {

[[nodiscard]] inline auto header_value(const front_end_request& r, std::string_view name)
    -> std::string_view {
    for (const auto& [n, v] : r.headers) {
        if (n.size() == name.size() &&
            std::equal(n.begin(), n.end(), name.begin(), [](unsigned char a, unsigned char b) {
                return std::tolower(a) == std::tolower(b);
            })) {
            return v;
        }
    }
    return {};
}

[[nodiscard]] inline auto string_member(const boost::json::object& o, std::string_view key)
    -> std::optional<std::string_view> {
    if (const auto* v = o.if_contains(key); v != nullptr && v->is_string()) {
        return std::string_view(v->get_string());
    }
    return std::nullopt;
}

}  // namespace detail

template<aws_auth::aws_request_authorizer Authorizer> class front_end {
public:
    front_end(front_end_config config, Authorizer& authorizer)
        : _config(std::move(config)),
          _authorizer(authorizer),
          _dynamodb(dynamodb_model::k_model, codec_options(_config)),
          _streams(dynamodbstreams_model::k_model, codec_options(_config)) {
        for (const auto& a : dynamodb_actions().actions) {
            _actions.emplace(a.name);
        }
        on("DescribeEndpoints",
           [this](call_context&, boost::json::object&) { return describe_endpoints(); });
    }

    front_end(const front_end&) = delete;
    auto operator=(const front_end&) -> front_end& = delete;

    /// Registers the handler for a DynamoDB or Streams operation (by bare
    /// name; the two models share no operation name that both serve).
    /// Throws `std::invalid_argument` for an operation neither model has or
    /// one already registered.
    void on(std::string_view operation, front_end_handler h) {
        const bool known = dynamodb_model::k_model.find_operation(operation) != nullptr ||
                           dynamodbstreams_model::k_model.find_operation(operation) != nullptr;
        if (!known) {
            throw std::invalid_argument("no such DynamoDB operation: " + std::string(operation));
        }
        if (!_handlers.emplace(std::string(operation), std::move(h)).second) {
            throw std::invalid_argument("handler already registered: " + std::string(operation));
        }
    }

    [[nodiscard]] auto handles(std::string_view operation) const -> bool {
        return _handlers.contains(std::string(operation));
    }

    [[nodiscard]] auto handle(const front_end_request& req) -> aws_json::http_response {
        const auto target = detail::header_value(req, "x-amz-target");
        const auto content_type = detail::header_value(req, "content-type");
        const aws_json::http_request_view view{req.method, req.path, content_type, target,
                                               req.body};
        const auto dot = target.rfind('.');
        const auto prefix = dot == std::string_view::npos ? target : target.substr(0, dot);
        const aws_json::codec& c = prefix == k_streams_target_prefix ? _streams : _dynamodb;
        auto id = c.new_request_id();

        const auto* op = c.resolve_target(view);
        if (op == nullptr) {
            // Framing faults (method, content type, prefix, operation) and
            // body limits are the codec's to word.
            auto parsed = c.parse(view);
            return c.respond(parsed ? unknown_operation(c) : parsed.error(), id);
        }

        sigv4::service_rules rules;
        rules.service = "dynamodb";
        rules.protocol = sigv4::error_protocol::json;
        if (!_config.region.empty()) {
            rules.regions = {_config.region};
        }
        const sigv4::request sreq{req.method, req.path, req.query, req.headers, req.body};
        const auto who = _authorizer.authenticate(sreq, rules);
        if (!who) {
            return c.respond(auth_error(who.error()), id);
        }

        const auto h = _handlers.find(std::string(op->name));
        if (h == _handlers.end()) {
            return c.respond(unknown_operation(c), id);
        }
        auto parsed = c.parse(view);
        if (!parsed) {
            return c.respond(parsed.error(), id);
        }
        auto& input = parsed->input;

        call_context ctx;
        ctx.operation = op->name;
        ctx.request_id = id;
        ctx.principal = *who;
        ctx.region = _config.region;
        ctx.partition = _config.partition;
        ctx.authorize = [this, &ctx, &req](aws_auth::authz_request r) {
            return check(ctx, req, std::move(r));
        };

        if (_actions.contains(std::string(op->name))) {
            if (auto denied = check(ctx, req, primary_request(ctx, input))) {
                return c.respond(*denied, id);
            }
        }

        try {
            auto out = h->second(ctx, input);
            if (!out) {
                return c.respond(out.error(), id);
            }
            return c.respond(*out, id);
        } catch (const std::exception&) {
            aws_json::error e;
            e.status = 500;
            e.type = c.options().style.internal_type;
            e.message = "Internal server error";
            return c.respond(e, id);
        }
    }

private:
    front_end_config _config;
    Authorizer& _authorizer;
    aws_json::codec _dynamodb;
    aws_json::codec _streams;
    std::map<std::string, front_end_handler, std::less<>> _handlers;
    std::set<std::string, std::less<>> _actions;

    [[nodiscard]] static auto codec_options(const front_end_config& cfg)
        -> aws_json::codec_options {
        aws_json::codec_options o;
        o.style = dynamodb_error_style();
        o.limits = cfg.limits;
        return o;
    }

    [[nodiscard]] static auto unknown_operation(const aws_json::codec& c) -> aws_json::error {
        aws_json::error e;
        e.type = c.options().style.unknown_operation_type;
        e.message = std::nullopt;
        return e;
    }

    // SigV4 faults are Coral service errors on DynamoDB
    // (com.amazon.coral.service#UnrecognizedClientException).
    [[nodiscard]] static auto auth_error(const sigv4::aws_error& e) -> aws_json::error {
        aws_json::error out;
        out.status = static_cast<unsigned>(e.http_status);
        out.type =
            e.code.find('#') == std::string::npos ? "com.amazon.coral.service#" + e.code : e.code;
        out.message = e.message;
        return out;
    }

    [[nodiscard]] auto describe_endpoints() const -> aws_json::result<boost::json::object> {
        boost::json::object endpoint;
        endpoint["Address"] = _config.advertised_endpoint;
        endpoint["CachePeriodInMinutes"] = 1440;
        boost::json::array endpoints;
        endpoints.push_back(std::move(endpoint));
        boost::json::object out;
        out["Endpoints"] = std::move(endpoints);
        return out;
    }

    // The resource an operation acts on, from its input.
    [[nodiscard]] static auto resource_of(const call_context& ctx, const boost::json::object& in)
        -> std::string {
        for (const auto* key : {"ResourceArn", "StreamArn", "BackupArn", "TableArn"}) {
            if (auto v = detail::string_member(in, key)) {
                return std::string(*v);
            }
        }
        if (auto table = detail::string_member(in, "TableName")) {
            // A table name may also be given as a table ARN.
            if (table->starts_with("arn:")) {
                return std::string(*table);
            }
            auto arn = ctx.table_arn(*table);
            if (auto index = detail::string_member(in, "IndexName")) {
                arn += "/index/" + std::string(*index);
            }
            return arn;
        }
        return "*";
    }

    [[nodiscard]] static auto primary_request(const call_context& ctx,
                                              const boost::json::object& in)
        -> aws_auth::authz_request {
        aws_auth::authz_request r;
        r.action = "dynamodb:" + std::string(ctx.operation);
        r.resource = resource_of(ctx, in);
        for (const auto* member : {"ReturnValues", "ReturnConsumedCapacity", "Select"}) {
            if (auto v = detail::string_member(in, member)) {
                r.keys.emplace_back("dynamodb:" + std::string(member),
                                    std::vector<std::string>{std::string(*v)});
            }
        }
        return r;
    }

    [[nodiscard]] auto check(const call_context& ctx, const front_end_request& req,
                             aws_auth::authz_request r) -> std::optional<aws_json::error> {
        r.source_ip = req.source_ip;
        r.user_agent = std::string(detail::header_value(req, "user-agent"));
        r.region = _config.region;
        r.secure_transport = req.secure;
        const auto d = _authorizer.authorize(ctx.principal, r);
        if (d.allowed) {
            return std::nullopt;
        }
        aws_json::error e;
        e.type = "com.amazon.coral.service#AccessDeniedException";
        e.message = "User: " + ctx.principal.arn + " is not authorized to perform: " + r.action +
                    " on resource: " + r.resource +
                    (d.explicit_deny
                         ? " with an explicit deny in an identity-based policy"
                         : " because no identity-based policy allows the " + r.action + " action");
        return e;
    }
};

}  // namespace kythira::dynamodb
