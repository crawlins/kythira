// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_front_end.hpp
/// @brief The Kinesis front end: SigV4, authorization, framing and dispatch
///        (`.kiro/specs/kinesis-compatible-streams/` task 4, Requirements
///        3, 4 and 5).
///
/// One request, in order:
///
///   1. Framing: `POST /`, a JSON 1.1 or CBOR content type and an
///      `X-Amz-Target` of `Kinesis_20131202.<Action>` naming a modeled
///      operation; anything else is the codec's error to word.
///   2. SigV4 through the authorizer (`aws_request_authorizer`: the IAM
///      authorizer, or `static_key_authorizer` without IAM), signing service
///      `kinesis`. Authentication comes before the body is read, so an
///      unsigned caller learns nothing about the API.
///   3. A modeled operation with no handler answers UnknownOperationException
///      (the operations `doc/unsupported.md` lists); then the codec parses
///      the body (JSON or CBOR) and validates it against the pinned model.
///   4. The target stream: `StreamARN` or `StreamName`. When a request names
///      both (KPL does, on every `PutRecords`) they must be the same stream,
///      or the request is refused before anything is authorized, so a
///      caller cannot be authorized on one stream and act on another.
///   5. Authorization: the operation's same-named `kinesis:` action on the
///      resource the input names (`ConsumerARN`, `ResourceARN`, the target
///      stream, or `*` for the actions AWS defines without a resource), with
///      the request's tags as `aws:RequestTag/*` and `aws:TagKeys`, and the
///      resource's tags and policy from the resource lookup. `CreateStream`
///      with tags also needs `kinesis:AddTagsToStream` on the new stream, as
///      on AWS; `RegisterStreamConsumer` with tags needs
///      `kinesis:TagResource` on the consumer, which its handler checks once
///      the consumer's ARN exists. When the input does not identify the resource (`GetRecords`
///      and `ListShards` by token, a consumer named by stream and consumer
///      name), the handler authorizes through `call_context::authorize`
///      once it has looked the resource up; a handler that answers without
///      doing so gets InternalFailure, never the data.
///   6. The handler. An exception becomes InternalFailure (500).
///
/// Every answer is in the request's encoding (CBOR for the Java SDK).
///
/// One request outside the Kinesis API is answered too: STS
/// `GetCallerIdentity`, a form-encoded `POST /` with no X-Amz-Target and
/// signed for `sts`. KPL calls it before its first write to build the
/// `StreamARN` it sends, and takes an STS endpoint that can point here. The
/// answer comes from the signature alone, as AWS's needs no permission;
/// every other STS action is refused with InvalidAction.
///
/// The front end is transport-free: the composition root adapts the shared
/// Beast host's request into `front_end_request` and the response back, and
/// answers `Expect: 100-continue` there (KPL sends it on every batch).
/// Routing is the handlers' business, from `call_context`: an operation
/// that writes is forwarded to the leader of the group that owns the shard,
/// and one that only reads (`replica_read_ok`) may be answered by any
/// replica. The handlers and the daemon that mounts this come with task 6.

#include <kythira/kinesis/kinesis_actions.hpp>
#include <kythira/kinesis/kinesis_errors.hpp>
#include <kythira/kinesis/model/kinesis_model.hpp>

#include <raft/aws_eventstream.hpp>
#include <raft/aws_json_protocol.hpp>
#include <raft/aws_query_protocol.hpp>
#include <raft/aws_request_authorizer.hpp>
#include <raft/sigv4_verifier.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::kinesis {

struct front_end_config {
    std::string _region = "us-east-1";
    std::string _partition = "aws";
    aws_json::request_limits _limits{};
};

/// An HTTP request as the transport hands it over.
struct front_end_request {
    std::string _method;
    std::string _path;
    std::string _query;  ///< without '?'
    std::vector<std::pair<std::string, std::string>> _headers;
    std::string _body;
    std::string _source_ip;
    bool _secure = true;
};

/// The events of a streaming answer (`SubscribeToShard`, task 13), pulled
/// by the transport one at a time. Each is a whole event-stream message.
class event_source {
public:
    struct pull {
        /// One encoded message; empty when nothing is ready yet.
        std::string _message;
        /// No more messages: the stream is over.
        bool _done = false;
    };

    event_source() = default;
    event_source(const event_source&) = delete;
    event_source(event_source&&) = delete;
    auto operator=(const event_source&) -> event_source& = delete;
    auto operator=(event_source&&) -> event_source& = delete;
    virtual ~event_source() = default;

    /// The next message, waiting at most about `wait` for one.
    virtual auto next(std::chrono::milliseconds wait) -> pull = 0;
    /// The client went away: stop early. Idempotent.
    virtual void cancel() = 0;
};

/// What authorization needs to know about an existing resource: its tags
/// (`aws:ResourceTag/*`) and its resource policy, if it has one.
struct resource_facts {
    std::vector<std::pair<std::string, std::string>> _tags;
    std::optional<std::string> _policy;
};

/// Looks a resource up by ARN for authorization; nullopt when it does not
/// exist (the handler then answers ResourceNotFoundException).
using resource_lookup = std::function<std::optional<resource_facts>(std::string_view arn)>;

/// The stream a request acts on, from `StreamARN` or `StreamName`.
struct stream_ref {
    std::string _name;
    std::string _account;
    std::string _arn;
};

/// The parts of a Kinesis ARN (`arn:<partition>:kinesis:<region>:<account>:
/// <resource>`), or nullopt when `arn` is not one.
struct kinesis_arn {
    std::string_view _partition;
    std::string_view _region;
    std::string_view _account;
    std::string_view _resource;  ///< `stream/<name>`, `stream/<name>/consumer/<c>:<t>`
};

[[nodiscard]] inline auto parse_kinesis_arn(std::string_view arn) -> std::optional<kinesis_arn> {
    std::array<std::string_view, 6> parts{};
    for (std::size_t i = 0; i < 5; ++i) {
        const auto colon = arn.find(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        parts.at(i) = arn.substr(0, colon);
        arn.remove_prefix(colon + 1);
    }
    parts[5] = arn;
    if (parts[0] != "arn" || parts[2] != "kinesis" || parts[5].empty()) {
        return std::nullopt;
    }
    return kinesis_arn{parts[1], parts[3], parts[4], parts[5]};
}

/// The stream name of a stream ARN (`.../stream/<name>`), or nullopt.
[[nodiscard]] inline auto stream_name_of(std::string_view arn) -> std::optional<std::string_view> {
    const auto parts = parse_kinesis_arn(arn);
    if (!parts || !parts->_resource.starts_with("stream/")) {
        return std::nullopt;
    }
    auto name = parts->_resource.substr(7);
    if (name.empty() || name.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    return name;
}

/// What a handler sees besides its input.
struct call_context {
    std::string_view _operation;
    std::string _request_id;
    aws_auth::principal_context _principal;
    std::string _region;
    std::string _partition;
    aws_json::body_encoding _encoding = aws_json::body_encoding::json;
    /// The stream the request names, resolved and checked (step 4).
    std::optional<stream_ref> _stream;
    /// Set by a streaming handler: the events that follow its (initial)
    /// answer.
    std::shared_ptr<event_source> _events;
    /// The operation writes nothing, so any replica may answer it.
    bool _replica_read_ok = false;
    /// The front end authorized the request's resource; when false, the
    /// handler must call `authorize` before it answers.
    bool _authorized = false;
    /// Authorizes one more action, or the request's own action on a
    /// resource only the handler could identify. On refusal, the
    /// AccessDeniedException to return. An allowed call marks the request
    /// authorized.
    std::function<std::optional<aws_json::error>(aws_auth::authz_request)> _authorize;

    /// `arn:<partition>:kinesis:<region>:<account>:stream/<name>`, in the
    /// caller's account.
    [[nodiscard]] auto stream_arn(std::string_view name) const -> std::string {
        return "arn:" + _partition + ":kinesis:" + _region + ":" + _principal.account + ":stream/" +
               std::string(name);
    }

    /// The consumer ARN AWS forms: the stream ARN, `/consumer/<name>:` and
    /// the consumer's creation time in epoch seconds.
    [[nodiscard]] static auto consumer_arn(std::string_view stream_arn, std::string_view name,
                                           long long created_epoch_seconds) -> std::string {
        return std::string(stream_arn) + "/consumer/" + std::string(name) + ":" +
               std::to_string(created_epoch_seconds);
    }

    [[nodiscard]] auto authorize(aws_auth::authz_request r) -> std::optional<aws_json::error> {
        return _authorize(std::move(r));
    }
};

using front_end_handler =
    std::function<aws_json::result<boost::json::object>(call_context&, boost::json::object&)>;

namespace detail {

[[nodiscard]] inline auto iequals(std::string_view a, std::string_view b) -> bool {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

[[nodiscard]] inline auto header_value(const front_end_request& r, std::string_view name)
    -> std::string_view {
    for (const auto& [n, v] : r._headers) {
        if (iequals(n, name)) {
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

/// The actions AWS defines with no resource (`UpdateShardCount`,
/// `ListStreams`): they are authorized on `*`, whatever the input names.
[[nodiscard]] inline auto resourceless_actions() -> const std::set<std::string, std::less<>>& {
    static const auto actions = [] {
        std::set<std::string, std::less<>> out;
        for (const auto& a : kinesis_actions().actions) {
            if (a.resource_templates == std::vector<std::string>{"*"}) {
                out.emplace(a.name);
            }
        }
        return out;
    }();
    return actions;
}

}  // namespace detail

template<aws_auth::aws_request_authorizer Authorizer> class front_end {
public:
    front_end(front_end_config config, Authorizer& authorizer)
        : _config(std::move(config)),
          _authorizer(authorizer),
          _codec(kinesis_model::k_model, codec_options(_config)) {
        for (const auto& a : kinesis_actions().actions) {
            _actions.emplace(a.name);
        }
    }

    front_end(const front_end&) = delete;
    auto operator=(const front_end&) -> front_end& = delete;

    /// Registers the handler for a Kinesis operation. Throws
    /// `std::invalid_argument` for an operation the model does not have or
    /// one already registered.
    void on(std::string_view operation, front_end_handler h) {
        if (kinesis_model::k_model.find_operation(operation) == nullptr) {
            throw std::invalid_argument("no such Kinesis operation: " + std::string(operation));
        }
        if (!_handlers.emplace(std::string(operation), std::move(h)).second) {
            throw std::invalid_argument("handler already registered: " + std::string(operation));
        }
    }

    [[nodiscard]] auto handles(std::string_view operation) const -> bool {
        return _handlers.contains(operation);
    }

    /// Supplies resource tags and policies to authorization. Without it,
    /// requests are authorized on identity policies alone.
    void set_resource_lookup(resource_lookup lookup) { _lookup = std::move(lookup); }

    [[nodiscard]] auto codec() const -> const aws_json::codec& { return _codec; }

    /// An answer, and for a streaming operation the events after it.
    struct reply {
        reply(aws_json::http_response r)  // NOLINT(google-explicit-constructor)
            : _response(std::move(r)) {}
        reply(aws_json::http_response r, std::shared_ptr<event_source> e)
            : _response(std::move(r)), _events(std::move(e)) {}
        aws_json::http_response _response;
        std::shared_ptr<event_source> _events;
    };

    /// `handle_streaming` for a transport that cannot stream: a streaming
    /// operation's events are dropped.
    [[nodiscard]] auto handle(const front_end_request& req) -> aws_json::http_response {
        auto r = handle_streaming(req);
        if (r._events) {
            r._events->cancel();
        }
        return std::move(r._response);
    }

    [[nodiscard]] auto handle_streaming(const front_end_request& req) -> reply {
        const auto target = detail::header_value(req, "x-amz-target");
        const auto content_type = detail::header_value(req, "content-type");
        if (target.empty() && is_sts_query(req, content_type)) {
            return handle_sts(req);
        }
        const aws_json::http_request_view view{req._method, req._path, content_type, target,
                                               req._body};
        const auto encoding =
            _codec.encoding_of(content_type).value_or(aws_json::body_encoding::json);
        auto id = _codec.new_request_id();

        const auto* op = _codec.resolve_target(view);
        if (op == nullptr) {
            // Framing faults (method, content type, prefix, operation) and
            // body limits are the codec's to word.
            auto parsed = _codec.parse(view);
            return _codec.respond(parsed ? unknown_operation() : parsed.error(), id, encoding);
        }

        sigv4::service_rules rules;
        rules.service = "kinesis";
        rules.protocol = sigv4::error_protocol::json;
        if (!_config._region.empty()) {
            rules.regions = {_config._region};
        }
        const sigv4::request sreq{req._method, req._path, req._query, req._headers, req._body};
        const auto who = _authorizer.authenticate(sreq, rules);
        if (!who) {
            return _codec.respond(auth_error(who.error()), id, encoding);
        }

        const auto h = _handlers.find(op->name);
        if (h == _handlers.end()) {
            return _codec.respond(unknown_operation(), id, encoding);
        }
        auto parsed = _codec.parse(view);
        if (!parsed) {
            return _codec.respond(parsed.error(), id, encoding);
        }
        auto& input = parsed->input;

        call_context ctx;
        ctx._operation = op->name;
        ctx._request_id = id;
        ctx._principal = *who;
        ctx._region = _config._region;
        ctx._partition = _config._partition;
        ctx._encoding = encoding;
        const auto* acts = find_operation_actions(op->name);
        ctx._replica_read_ok = acts != nullptr && !acts->writes;
        ctx._authorize = [this, &ctx, &req](aws_auth::authz_request r) {
            auto denied = check(ctx, req, std::move(r));
            if (!denied) {
                ctx._authorized = true;
            }
            return denied;
        };

        auto stream = resolve_stream(ctx, input);
        if (!stream) {
            return _codec.respond(stream.error(), id, encoding);
        }
        ctx._stream = std::move(*stream);

        if (auto denied = authorize_primary(ctx, input)) {
            return _codec.respond(*denied, id, encoding);
        }

        try {
            auto out = h->second(ctx, input);
            if (!out) {
                return _codec.respond(out.error(), id, encoding);
            }
            if (!ctx._authorized) {
                // A handler bug, not a client fault: never let data out
                // unauthorized.
                return _codec.respond(internal_failure(), id, encoding);
            }
            if (ctx._events) {
                // An event stream: the head now, the events (the first is
                // the initial response) as the transport pulls them.
                aws_json::http_response head;
                head.status = 200;
                head.headers = {{"Content-Type", std::string(aws_eventstream::k_content_type)},
                                {"x-amzn-RequestId", id}};
                return reply(std::move(head), std::move(ctx._events));
            }
            return _codec.respond(*out, id, encoding, op);
        } catch (const std::exception&) {
            return _codec.respond(internal_failure(), id, encoding);
        }
    }

private:
    front_end_config _config;
    Authorizer& _authorizer;
    aws_json::codec _codec;
    std::map<std::string, front_end_handler, std::less<>> _handlers;
    std::set<std::string, std::less<>> _actions;
    resource_lookup _lookup;

    [[nodiscard]] static auto codec_options(const front_end_config& cfg)
        -> aws_json::codec_options {
        aws_json::codec_options o;
        o.style = kinesis_error_style();
        o.ids = aws_json::request_id_style::uuid;
        o.limits = cfg._limits;
        o.accept_cbor = true;
        o.cbor_timestamps = aws_json::cbor_timestamp_unit::milliseconds;
        return o;
    }

    // STS on the Kinesis endpoint: a form-encoded `POST /` with no
    // X-Amz-Target, the shape of every Query-protocol call.
    [[nodiscard]] static auto is_sts_query(const front_end_request& req,
                                           std::string_view content_type) -> bool {
        const auto semi = content_type.find(';');
        auto media = content_type.substr(0, semi);
        while (!media.empty() && media.back() == ' ') {
            media.remove_suffix(1);
        }
        return req._method == "POST" && req._path == "/" &&
               detail::iequals(media, "application/x-www-form-urlencoded");
    }

    // `GetCallerIdentity`, answered from the signature alone (README of
    // tests/evidence: KPL calls it before its first write to build the
    // StreamARN it sends, and takes a separate STS endpoint that can point
    // here). It needs no permission on AWS either. Every other STS action
    // is refused: credentials come from IAM's STS, not from here.
    [[nodiscard]] auto handle_sts(const front_end_request& req) -> aws_json::http_response {
        static constexpr std::string_view k_ns = "https://sts.amazonaws.com/doc/2011-06-15/";
        static constexpr std::size_t k_max_body = 8U * 1024U;
        const auto id = _codec.new_request_id();
        const auto answer = [&id](unsigned status, std::string body) {
            aws_json::http_response r;
            r.status = status;
            r.headers = {{"Content-Type", "text/xml"}, {"x-amzn-RequestId", id}};
            r.body = std::move(body);
            return r;
        };
        const auto refuse = [&](unsigned status, std::string_view code, std::string_view msg) {
            return answer(status, aws_query::error_xml(k_ns, status < 500, code, msg, id));
        };
        if (req._body.size() > k_max_body) {
            return refuse(413, "RequestEntityTooLarge", "The request body is too large.");
        }
        sigv4::service_rules rules;
        rules.service = "sts";
        rules.protocol = sigv4::error_protocol::query;
        if (!_config._region.empty()) {
            rules.regions = {_config._region};
        }
        const sigv4::request sreq{req._method, req._path, req._query, req._headers, req._body};
        const auto who = _authorizer.authenticate(sreq, rules);
        if (!who) {
            return refuse(static_cast<unsigned>(who.error().http_status), who.error().code,
                          who.error().message);
        }
        const auto form = aws_query::form::parse(req._body);
        if (!form) {
            return refuse(400, "MalformedQueryString", "The request body is not form-encoded.");
        }
        const auto action = form->get("Action").value_or("");
        if (action != "GetCallerIdentity") {
            return refuse(400, "InvalidAction",
                          "This endpoint answers only the STS action GetCallerIdentity.");
        }
        aws_query::xml_writer w;
        w.element("Arn", who->arn);
        w.element("UserId", who->userid.empty() ? who->account : who->userid);
        w.element("Account", who->account);
        return answer(200, aws_query::response_xml("GetCallerIdentity", k_ns, w.str(), id));
    }

    [[nodiscard]] static auto unknown_operation() -> aws_json::error {
        aws_json::error e;
        e.type = kinesis_error_style().unknown_operation_type;
        e.message = std::nullopt;
        return e;
    }

    [[nodiscard]] static auto auth_error(const sigv4::aws_error& e) -> aws_json::error {
        aws_json::error out;
        out.status = static_cast<unsigned>(e.http_status);
        out.type = e.code;
        out.message = e.message;
        return out;
    }

    // Step 4: the one stream the request acts on.
    [[nodiscard]] static auto resolve_stream(const call_context& ctx, const boost::json::object& in)
        -> aws_json::result<std::optional<stream_ref>> {
        const auto arn = detail::string_member(in, "StreamARN");
        const auto name = detail::string_member(in, "StreamName");
        if (!arn) {
            if (!name) {
                return std::optional<stream_ref>{};
            }
            return std::optional<stream_ref>{
                stream_ref{std::string(*name), ctx._principal.account, ctx.stream_arn(*name)}};
        }
        const auto parts = parse_kinesis_arn(*arn);
        const auto arn_name = stream_name_of(*arn);
        if (!parts || !arn_name) {
            return invalid_argument("StreamARN " + std::string(*arn) + " is not a stream ARN.");
        }
        if (name && *name != *arn_name) {
            return invalid_argument("StreamARN " + std::string(*arn) +
                                    " does not refer to stream " + std::string(*name) + ".");
        }
        return std::optional<stream_ref>{
            stream_ref{std::string(*arn_name), std::string(parts->_account), std::string(*arn)}};
    }

    // The resource an operation acts on, when the input names it.
    [[nodiscard]] static auto resource_of(const call_context& ctx, const boost::json::object& in)
        -> std::optional<std::string> {
        if (detail::resourceless_actions().contains(ctx._operation)) {
            return std::string("*");
        }
        for (const auto* key : {"ConsumerARN", "ResourceARN", "ChannelARN"}) {
            if (auto v = detail::string_member(in, key)) {
                return std::string(*v);
            }
        }
        if (ctx._stream) {
            // A consumer named by stream and consumer name is a consumer
            // resource whose ARN carries its creation time: the handler
            // knows it, the front end does not.
            if (detail::string_member(in, "ConsumerName") &&
                ctx._operation != "RegisterStreamConsumer") {
                return std::nullopt;
            }
            return ctx._stream->_arn;
        }
        return std::nullopt;
    }

    [[nodiscard]] static auto tags_of(const boost::json::object& in)
        -> std::optional<std::vector<std::pair<std::string, std::string>>> {
        const auto* tags = in.if_contains("Tags");
        if (tags == nullptr || !tags->is_object()) {
            return std::nullopt;
        }
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& kv : tags->get_object()) {
            out.emplace_back(std::string(kv.key()),
                             kv.value().is_string() ? std::string(kv.value().get_string()) : "");
        }
        return out;
    }

    // Step 5.
    [[nodiscard]] auto authorize_primary(call_context& ctx, const boost::json::object& in)
        -> std::optional<aws_json::error> {
        if (!_actions.contains(ctx._operation)) {
            // No same-named action (none today): the handler authorizes.
            return std::nullopt;
        }
        const auto resource = resource_of(ctx, in);
        if (!resource) {
            return std::nullopt;
        }
        aws_auth::authz_request r;
        r.action = "kinesis:" + std::string(ctx._operation);
        r.resource = *resource;
        const auto request_tags = tags_of(in);
        r.request_tags = request_tags;
        if (const auto* keys = in.if_contains("TagKeys"); keys != nullptr && keys->is_array()) {
            std::vector<std::string> names;
            for (const auto& k : keys->get_array()) {
                if (k.is_string()) {
                    names.emplace_back(k.get_string());
                }
            }
            r.keys.emplace_back("aws:TagKeys", std::move(names));
        }
        if (auto denied = ctx.authorize(std::move(r))) {
            return denied;
        }
        if (ctx._operation == "CreateStream" && request_tags && !request_tags->empty()) {
            aws_auth::authz_request tag;
            tag.action = "kinesis:AddTagsToStream";
            tag.resource = *resource;
            tag.request_tags = request_tags;
            return ctx.authorize(std::move(tag));
        }
        return std::nullopt;
    }

    [[nodiscard]] auto check(const call_context& ctx, const front_end_request& req,
                             aws_auth::authz_request r) -> std::optional<aws_json::error> {
        r.source_ip = req._source_ip;
        r.user_agent = std::string(detail::header_value(req, "user-agent"));
        r.region = _config._region;
        r.secure_transport = req._secure;
        if (r.resource != "*") {
            if (r.resource_account.empty()) {
                if (const auto parts = parse_kinesis_arn(r.resource);
                    parts && parts->_account != ctx._principal.account) {
                    r.resource_account = std::string(parts->_account);
                }
            }
            if (_lookup) {
                if (auto facts = _lookup(r.resource)) {
                    if (r.resource_tags.empty()) {
                        r.resource_tags = std::move(facts->_tags);
                    }
                    if (!r.resource_policy) {
                        r.resource_policy = std::move(facts->_policy);
                    }
                }
            }
        }
        const auto d = _authorizer.authorize(ctx._principal, r);
        if (d.allowed) {
            return std::nullopt;
        }
        aws_json::error e;
        e.type = "AccessDeniedException";
        e.message = "User: " + ctx._principal.arn + " is not authorized to perform: " + r.action +
                    " on resource: " + r.resource +
                    (d.explicit_deny
                         ? " with an explicit deny in an identity-based policy"
                         : " because no identity-based policy allows the " + r.action + " action");
        return e;
    }
};

}  // namespace kythira::kinesis
