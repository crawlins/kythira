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
///      condition keys when the input sets them. StartBulkPutItems is
///      authorized as `dynamodb:BulkPutItems` on its table. Operations
///      without a same-named action (transactions, PartiQL), the batch
///      operations (one resource per table they name) and the keys that
///      need the parsed item (`LeadingKeys`, `Attributes`) are authorized
///      by their handlers through `call_context::authorize`, from
///      `k_operation_actions`.
///   5. The handler. An exception becomes InternalServerError (500), and
///      so does a success for which no authorization was asked at all
///      (`call_context::authorization_checked`): a handler that forgot to
///      authorize fails closed instead of answering.
///
/// Every authorization (step 4 and `call_context::authorize`) asks the
/// resource lookup, when one is set, for the resource's tags and
/// resource-based policy (task 21): a table's for the table and its
/// indexes, a stream's for the stream. The authorizer evaluates them (IAM's
/// evaluator does; `static_key_authorizer` has roles only, so a gateway on
/// static keys sets no lookup). As AWS lets the account root, a root caller
/// may always `DeleteResourcePolicy`, whatever the policy says.
///
/// `GET /kythira/contributor-insights` (`k_insights_report_path`) is a
/// Kythira extension, there being no CloudWatch to hold Contributor
/// Insights rules: SigV4-signed like any request, authorized as
/// `dynamodb:DescribeContributorInsights` on the table or index its
/// `TableName` and `IndexName` query parameters name, and answered by the
/// report handler (`insights_api.hpp`).
///
/// `POST /kythira/replication` (`k_replication_path`) is the other: a
/// peer cluster's global table replicator calls it, SigV4-signed with a
/// replication principal's keys for this cluster's region. Its JSON body's
/// `Action` picks the action authorized on the table its `TableName`
/// names: `CreateReplica` as `dynamodb:CreateTableReplica`,
/// `DeleteReplica` as `dynamodb:DeleteTableReplica`, `SetReplicas` as
/// `dynamodb:UpdateTable` and `Apply` as `dynamodb:PutItem` (the handler
/// also asks for `dynamodb:DeleteItem` when it deletes).
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
    /// The account that owns this gateway's tables: their ARNs carry it,
    /// and a caller from any other account is a cross-account caller,
    /// who needs the table's resource policy as on AWS (static keys never
    /// get cross-account access). Empty: each caller's own account, so
    /// every account sees the same tables; only for a single-account
    /// deployment.
    std::string account;
    aws_json::request_limits limits{};
    /// Told the operation and the exception's message whenever a handler
    /// throws and the client is answered `InternalServerError`, so the
    /// cause is in the node's log; the answer itself says nothing more, as
    /// AWS's does not.
    std::function<void(std::string_view operation, std::string_view what)> on_internal_error{};
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
    /// The account that owns the tables (`front_end_config::account`);
    /// empty: the caller's.
    std::string account;
    /// Authorizes one more action (a transaction element, a PartiQL
    /// statement, a condition key the front end could not supply). On
    /// refusal, the AccessDeniedException to return.
    std::function<std::optional<aws_json::error>(aws_auth::authz_request)> authorize;
    /// Set once the authorizer has been asked about this call, by the
    /// front end's primary check or through `authorize`, whatever it
    /// answered. A call that succeeds with it unset is refused (the
    /// front end's backstop): a handler that forgot to authorize fails
    /// closed.
    bool authorization_checked = false;
    /// Set by a handler whose successful answer touched no resource, so
    /// there was nothing to authorize (a BatchExecuteStatement whose
    /// every statement failed to parse). The backstop then lets it pass.
    bool touched_no_resource = false;

    /// The account that owns the tables.
    [[nodiscard]] auto owner() const -> const std::string& {
        return account.empty() ? principal.account : account;
    }

    /// `arn:<partition>:dynamodb:<region>:<owner>:table/<name>`.
    [[nodiscard]] auto table_arn(std::string_view table) const -> std::string {
        return "arn:" + partition + ":dynamodb:" + region + ":" + owner() + ":table/" +
               std::string(table);
    }
};

using front_end_handler =
    std::function<aws_json::result<boost::json::object>(call_context&, boost::json::object&)>;

/// What authorization needs to know about a resource (task 21).
struct resource_facts {
    std::vector<std::pair<std::string, std::string>> tags;
    std::optional<std::string> policy;
};

/// Looks a table, index or stream up by ARN for authorization; nullopt
/// when it does not exist (the handler then answers not-found).
using resource_lookup = std::function<std::optional<resource_facts>(std::string_view arn)>;

/// The Contributor Insights report path (a Kythira extension).
inline constexpr std::string_view k_insights_report_path = "/kythira/contributor-insights";

/// The global table replication path (task 22, a Kythira extension): a
/// peer cluster's replicator POSTs JSON here (`global_tables.hpp`).
inline constexpr std::string_view k_replication_path = "/kythira/replication";

/// Answers a report request from its decoded query parameters.
using report_handler = std::function<aws_json::result<boost::json::object>(
    call_context&, const std::map<std::string, std::string, std::less<>>&)>;

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

/// Percent-decodes a query component (`+` is a space, as forms send it).
[[nodiscard]] inline auto url_decode(std::string_view in) -> std::optional<std::string> {
    std::string out;
    out.reserve(in.size());
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out.push_back(' ');
        } else if (in[i] == '%') {
            if (i + 2 >= in.size()) {
                return std::nullopt;
            }
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

/// `a=1&b=2` as a map; nullopt on a bad escape or a repeated name.
[[nodiscard]] inline auto query_params(std::string_view query)
    -> std::optional<std::map<std::string, std::string, std::less<>>> {
    std::map<std::string, std::string, std::less<>> out;
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto part = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        if (part.empty()) {
            continue;
        }
        const auto eq = part.find('=');
        auto name = url_decode(part.substr(0, eq));
        auto value =
            url_decode(eq == std::string_view::npos ? std::string_view{} : part.substr(eq + 1));
        if (!name || !value || !out.emplace(std::move(*name), std::move(*value)).second) {
            return std::nullopt;
        }
    }
    return out;
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

    /// Supplies resource tags and policies to every authorization.
    void set_resource_lookup(resource_lookup lookup) { _lookup = std::move(lookup); }

    /// Serves `k_insights_report_path`; unset, the path is not found.
    void set_report_handler(report_handler h) { _report = std::move(h); }

    /// Serves `k_replication_path`; unset, the path is not found.
    void set_replication_handler(front_end_handler h) { _replication = std::move(h); }

    [[nodiscard]] auto handle(const front_end_request& req) -> aws_json::http_response {
        if (req.path == k_insights_report_path && _report) {
            return report(req);
        }
        if (req.path == k_replication_path && _replication) {
            return replicate(req);
        }
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
        ctx.account = _config.account;
        ctx.authorize = [this, &ctx, &req](aws_auth::authz_request r) {
            return check(ctx, req, std::move(r));
        };

        if (const auto action = primary_action(op->name)) {
            if (auto denied = check(ctx, req, primary_request(ctx, input, *action))) {
                return c.respond(*denied, id);
            }
        }

        try {
            auto out = h->second(ctx, input);
            if (!out) {
                return c.respond(out.error(), id);
            }
            if (!ctx.authorization_checked && !ctx.touched_no_resource) {
                // No check ran for an operation that succeeded: a handler
                // that should authorize did not. Refuse rather than answer.
                if (_config.on_internal_error) {
                    _config.on_internal_error(op->name, "answered without an authorization check");
                }
                aws_json::error e;
                e.status = 500;
                e.type = c.options().style.internal_type;
                e.message = "Internal server error";
                return c.respond(e, id);
            }
            return c.respond(*out, id);
        } catch (const std::exception& ex) {
            if (_config.on_internal_error) {
                _config.on_internal_error(op->name, ex.what());
            }
            aws_json::error e;
            e.status = 500;
            e.type = c.options().style.internal_type;
            e.message = "Internal server error";
            return c.respond(e, id);
        }
    }

private:
    /// The Contributor Insights report: SigV4, then the read action on the
    /// table or index, then the handler.
    [[nodiscard]] auto report(const front_end_request& req) -> aws_json::http_response {
        const aws_json::codec& c = _dynamodb;
        auto id = c.new_request_id();
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
        if (req.method != "GET") {
            return c.respond(validation_error("The report is read with GET"), id);
        }
        const auto params = detail::query_params(req.query);
        if (!params) {
            return c.respond(validation_error("Malformed query string"), id);
        }
        const auto table = params->find("TableName");
        if (table == params->end() || table->second.empty()) {
            return c.respond(validation_error("TableName is required"), id);
        }
        call_context ctx;
        ctx.operation = "DescribeContributorInsights";
        ctx.request_id = id;
        ctx.principal = *who;
        ctx.region = _config.region;
        ctx.partition = _config.partition;
        ctx.account = _config.account;
        ctx.authorize = [this, &ctx, &req](aws_auth::authz_request r) {
            return check(ctx, req, std::move(r));
        };
        boost::json::object in;
        in["TableName"] = table->second;
        if (const auto index = params->find("IndexName"); index != params->end()) {
            in["IndexName"] = index->second;
        }
        if (auto denied = check(ctx, req, primary_request(ctx, in))) {
            return c.respond(*denied, id);
        }
        try {
            auto out = _report(ctx, *params);
            if (!out) {
                return c.respond(out.error(), id);
            }
            return c.respond(*out, id);
        } catch (const std::exception& ex) {
            if (_config.on_internal_error) {
                _config.on_internal_error("DescribeContributorInsights", ex.what());
            }
            aws_json::error e;
            e.status = 500;
            e.type = c.options().style.internal_type;
            e.message = "Internal server error";
            return c.respond(e, id);
        }
    }

    /// A peer's replication call: SigV4, then the action its `Action`
    /// names on the table, then the handler.
    [[nodiscard]] auto replicate(const front_end_request& req) -> aws_json::http_response {
        const aws_json::codec& c = _dynamodb;
        auto id = c.new_request_id();
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
        if (req.method != "POST") {
            return c.respond(validation_error("Replication calls are POSTed"), id);
        }
        boost::json::object in;
        try {
            auto v = boost::json::parse(req.body);
            if (!v.is_object()) {
                return c.respond(validation_error("The body must be a JSON object"), id);
            }
            in = std::move(v.as_object());
        } catch (const std::exception&) {
            return c.respond(validation_error("The body is not JSON"), id);
        }
        const auto action = detail::string_member(in, "Action");
        const auto table = detail::string_member(in, "TableName");
        if (!table || table->empty()) {
            return c.respond(validation_error("TableName is required"), id);
        }
        std::string_view operation;
        if (action == "CreateReplica") {
            operation = "CreateTableReplica";
        } else if (action == "DeleteReplica") {
            operation = "DeleteTableReplica";
        } else if (action == "SetReplicas") {
            operation = "UpdateTable";
        } else if (action == "Apply") {
            operation = "PutItem";
        } else {
            return c.respond(validation_error("Unknown replication Action"), id);
        }
        call_context ctx;
        ctx.operation = operation;
        ctx.request_id = id;
        ctx.principal = *who;
        ctx.region = _config.region;
        ctx.partition = _config.partition;
        ctx.account = _config.account;
        ctx.authorize = [this, &ctx, &req](aws_auth::authz_request r) {
            return check(ctx, req, std::move(r));
        };
        if (auto denied = check(ctx, req, primary_request(ctx, in))) {
            return c.respond(*denied, id);
        }
        try {
            auto out = _replication(ctx, in);
            if (!out) {
                return c.respond(out.error(), id);
            }
            return c.respond(*out, id);
        } catch (const std::exception& ex) {
            if (_config.on_internal_error) {
                _config.on_internal_error(operation, ex.what());
            }
            aws_json::error e;
            e.status = 500;
            e.type = c.options().style.internal_type;
            e.message = "Internal server error";
            return c.respond(e, id);
        }
    }

    front_end_config _config;
    Authorizer& _authorizer;
    resource_lookup _lookup;
    report_handler _report;
    front_end_handler _replication;
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
        for (const auto* key : {"ResourceArn", "StreamArn", "BackupArn", "TableArn",
                                "BulkOperationArn", "ExportArn", "ImportArn"}) {
            if (auto v = detail::string_member(in, key)) {
                return std::string(*v);
            }
        }
        // A shard iterator opens with its stream's ARN (`streams.hpp`).
        if (auto it = detail::string_member(in, "ShardIterator")) {
            return std::string(it->substr(0, it->find('|')));
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

    /// The action the front end authorizes before the handler runs, or
    /// nullopt when the handler authorizes: the batch operations ask for
    /// their action on each table they name, and transactions and PartiQL
    /// for each element's. StartBulkPutItems is authorized as
    /// BulkPutItems on its table, its handler adds StartBulkOperation on
    /// the operation it creates.
    [[nodiscard]] auto primary_action(std::string_view operation) const
        -> std::optional<std::string> {
        if (operation == "BatchGetItem" || operation == "BatchWriteItem") {
            return std::nullopt;
        }
        if (operation == "StartBulkPutItems") {
            return "BulkPutItems";
        }
        if (_actions.contains(operation)) {
            return std::string(operation);
        }
        return std::nullopt;
    }

    [[nodiscard]] static auto primary_request(const call_context& ctx,
                                              const boost::json::object& in)
        -> aws_auth::authz_request {
        return primary_request(ctx, in, ctx.operation);
    }

    [[nodiscard]] static auto primary_request(const call_context& ctx,
                                              const boost::json::object& in,
                                              std::string_view action) -> aws_auth::authz_request {
        aws_auth::authz_request r;
        r.action = "dynamodb:" + std::string(action);
        r.resource = resource_of(ctx, in);
        for (const auto* member : {"ReturnValues", "ReturnConsumedCapacity", "Select"}) {
            if (auto v = detail::string_member(in, member)) {
                r.keys.emplace_back("dynamodb:" + std::string(member),
                                    std::vector<std::string>{std::string(*v)});
            }
        }
        return r;
    }

    [[nodiscard]] auto check(call_context& ctx, const front_end_request& req,
                             aws_auth::authz_request r) -> std::optional<aws_json::error> {
        ctx.authorization_checked = true;
        r.source_ip = req.source_ip;
        r.user_agent = std::string(detail::header_value(req, "user-agent"));
        r.region = _config.region;
        r.secure_transport = req.secure;
        if (r.resource != "*" && r.resource.starts_with("arn:")) {
            // arn:<partition>:dynamodb:<region>:<account>:<resource>
            std::string_view rest(r.resource);
            for (int field = 0; field < 4 && !rest.empty(); ++field) {
                const auto colon = rest.find(':');
                rest =
                    colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
            }
            const auto account = rest.substr(0, rest.find(':'));
            if (!account.empty() && account != ctx.owner()) {
                // No account but the owner has tables here, so the
                // resource does not exist; and the handlers look tables up
                // by name alone, so such an ARN must never reach them.
                return foreign_not_found(r.resource);
            }
            if (r.resource_account.empty() && account != ctx.principal.account) {
                r.resource_account = std::string(account);
            }
            add_resource_facts(ctx, r);
        } else if (r.resource_account.empty() && ctx.owner() != ctx.principal.account) {
            // An account-wide action (ListTables) acts on the owner's
            // account, which is another account's to the caller.
            r.resource_account = ctx.owner();
        }
        const auto d = _authorizer.authorize(ctx.principal, r);
        if (d.allowed) {
            return std::nullopt;
        }
        // IAM's authorizer names the resource policy "resource".
        return denied(ctx, r, d.explicit_deny,
                      r.resource_policy.has_value() && d.policy == "resource");
    }

    /// The not-found answer for a resource in an account with no tables
    /// here, worded as the handler for that kind of resource words it.
    [[nodiscard]] static auto foreign_not_found(const std::string& arn) -> aws_json::error {
        aws_json::error e;
        const auto kind = [&arn](std::string_view part) {
            return arn.find(part) != std::string::npos;
        };
        std::string type = "ResourceNotFoundException";
        if (kind("/backup/")) {
            type = "BackupNotFoundException";
            e.message = "Backup not found: " + arn;
        } else if (kind("/export/")) {
            type = "ExportNotFoundException";
            e.message = "Export not found: " + arn;
        } else if (kind("/import/")) {
            type = "ImportNotFoundException";
            e.message = "Import not found: " + arn;
        } else if (kind("/bulk-operation/")) {
            e.message = "Bulk operation not found: " + arn;
        } else {
            e.message = "Requested resource not found";
        }
        e.type = "com.amazonaws.dynamodb.v20120810#" + type;
        return e;
    }

    [[nodiscard]] static auto denied(const call_context& ctx, const aws_auth::authz_request& r,
                                     bool explicit_deny, bool by_resource_policy)
        -> aws_json::error {
        aws_json::error e;
        e.type = "com.amazon.coral.service#AccessDeniedException";
        e.message =
            "User: " + ctx.principal.arn + " is not authorized to perform: " + r.action +
            " on resource: " + r.resource +
            (explicit_deny
                 ? (by_resource_policy ? " with an explicit deny in a resource-based policy"
                                       : " with an explicit deny in an identity-based policy")
                 : " because no identity-based policy allows the " + r.action + " action");
        return e;
    }

    /// With a lookup set: the resource's tags and policy. A cross-account
    /// caller (`resource_account`, set by `check`) needs that policy to be
    /// allowed; an authorizer that evaluates none (static keys) refuses
    /// such a caller outright.
    void add_resource_facts(const call_context& ctx, aws_auth::authz_request& r) const {
        if (!_lookup) {
            return;
        }
        auto facts = _lookup(r.resource);
        if (!facts) {
            return;
        }
        if (r.resource_tags.empty()) {
            r.resource_tags = std::move(facts->tags);
        }
        // The account root may always delete a resource policy, so a policy
        // cannot lock its account out of its own table.
        const bool root_delete = r.action == "dynamodb:DeleteResourcePolicy" &&
                                 ctx.principal.type == aws_auth::principal_type::root;
        if (!r.resource_policy && !root_delete) {
            r.resource_policy = std::move(facts->policy);
        }
    }
};

}  // namespace kythira::dynamodb
