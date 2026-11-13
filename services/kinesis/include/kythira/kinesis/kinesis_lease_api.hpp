// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kinesis_lease_api.hpp
/// @brief The Lease API for the native KCL lease layer
///        (`.kiro/specs/kinesis-compatible-streams/` task 9, design "Native
///        KCL lease layer", Requirement 15.1).
///
/// REST over the gateway's listener, under `/v1/leases/<application>/`:
///
/// - `GET /v1/leases/<app>/<key>`: read one lease.
/// - `PUT /v1/leases/<app>/<key>`: write a lease; `If-None-Match: *` to
///   create, `If-Match: <version>` to replace, neither to write
///   unconditionally.
/// - `PATCH /v1/leases/<app>/<key>`: set the body's members on an existing
///   lease (a `null` member removes the field); `If-Match` optional.
/// - `DELETE /v1/leases/<app>/<key>`: delete a lease; `If-Match` optional.
/// - `GET /v1/leases/<app>/?prefix=&start-after=&limit=`: list leases in
///   key order, a page at a time.
/// - `DELETE /v1/leases/<app>/`: delete every lease of the application.
///
/// The lease key is percent-encoded in the path. A lease is a JSON object;
/// its top-level members are stored as given and returned as given.
/// Answers are JSON: a lease is `{"key", "version", "lease"}`, a write
/// answers `{"version"}`, and both carry the version as the `ETag`. A
/// failed condition answers 412 `ConditionalCheckFailedException` with the
/// current version (0 when there is no lease).
///
/// Every request, reads included, goes through the catalog group's Raft log
/// (`lease_read_command`), so a read reflects every write acknowledged
/// before it, wherever it was sent: the linearizable read the design asks
/// for, at the cost of one log entry per read. KCL reads leases a few times
/// per failover interval per worker, so the cost is small.
///
/// Requests are signed with SigV4 for the `kinesis` service. Authorization
/// uses the DynamoDB actions KCL needs on AWS, on the table KCL would
/// create there, `arn:<partition>:dynamodb:<region>:<account>:table/<app>`:
/// `GetItem`, `Scan`, `PutItem`, `UpdateItem`, `DeleteItem` and, for the
/// whole application, `DeleteTable`. An existing KCL policy therefore works
/// unchanged.

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_leases.hpp>
#include <kythira/kinesis/kinesis_stream_state_machine.hpp>

#include <raft/aws_json_protocol.hpp>
#include <raft/aws_request_authorizer.hpp>
#include <raft/sigv4_verifier.hpp>
#include <raft/static_key_authorizer.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::kinesis {

/// The static-key roles for lease requests, in the `dynamodb` service a
/// static-key gateway registers next to `kinesis_static_roles()`:
/// read-only lists and reads leases, read-write also writes them (a KCL
/// worker), and deleting a whole application (`DeleteTable`) is admin's.
[[nodiscard]] inline auto lease_static_roles() -> aws_auth::static_role_table {
    return {"dynamodb", {"GetItem", "Scan"}, {"PutItem", "UpdateItem", "DeleteItem"}};
}

/// The `dynamodb` actions lease requests authorize on, for an IAM
/// registry: a gateway on Kythira IAM registers this next to
/// `kinesis_actions()`, so a KCL worker's policy grants leases as it would
/// grant its lease table on AWS. Resource formats and the tag condition key
/// are AWS's for these actions.
[[nodiscard]] inline auto lease_actions() -> aws_auth::aws_action_table {
    const std::vector<std::string> table{
        "arn:${Partition}:dynamodb:${Region}:${Account}:table/${TableName}"};
    const std::vector<std::string> tag{"aws:ResourceTag/${TagKey}"};
    aws_auth::aws_action_table t;
    t.service_prefix = "dynamodb";
    for (const char* name :
         {"DeleteItem", "DeleteTable", "GetItem", "PutItem", "Scan", "UpdateItem"}) {
        t.actions.push_back({name, table, tag});
    }
    return t;
}

namespace lease_detail {

[[nodiscard]] inline auto percent_decode(std::string_view s) -> std::optional<std::string> {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out.push_back(s[i]);
            continue;
        }
        if (i + 2 >= s.size()) {
            return std::nullopt;
        }
        unsigned v = 0;
        const auto* first = s.data() + i + 1;
        const auto [p, ec] = std::from_chars(first, first + 2, v, 16);
        if (ec != std::errc{} || p != first + 2) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>(v));
        i += 2;
    }
    return out;
}

[[nodiscard]] inline auto query_param(std::string_view query, std::string_view name)
    -> std::optional<std::string> {
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto pair = query.substr(0, amp);
        const auto eq = pair.find('=');
        if (pair.substr(0, eq) == name) {
            return percent_decode(eq == std::string_view::npos ? std::string_view()
                                                               : pair.substr(eq + 1));
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return std::nullopt;
}

[[nodiscard]] inline auto header(const front_end_request& r, std::string_view name)
    -> std::optional<std::string_view> {
    for (const auto& [k, v] : r._headers) {
        if (k.size() == name.size() &&
            std::equal(k.begin(), k.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            })) {
            return std::string_view(v);
        }
    }
    return std::nullopt;
}

/// `"5"`, `5` or `W/"5"`: the version. Anything else is not one.
[[nodiscard]] inline auto parse_version(std::string_view s) -> std::optional<std::uint64_t> {
    if (s.starts_with("W/")) {
        s.remove_prefix(2);
    }
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    std::uint64_t v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (s.empty() || ec != std::errc{} || p != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

[[nodiscard]] inline auto etag(std::uint64_t version) -> std::string {
    return "\"" + std::to_string(version) + "\"";
}

/// The lease's fields as a JSON object. The values were validated JSON
/// when stored, so this is valid JSON without reparsing them.
[[nodiscard]] inline auto render_fields(const lease_record& l) -> std::string {
    std::string out = "{";
    bool first = true;
    for (const auto& [name, text] : l._fields) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += boost::json::serialize(boost::json::string(name));
        out.push_back(':');
        out += text;
    }
    out.push_back('}');
    return out;
}

[[nodiscard]] inline auto render_entry(const lease_entry& e) -> std::string {
    return "{\"key\":" + boost::json::serialize(boost::json::string(e._key)) +
           ",\"version\":" + std::to_string(e._lease._version) +
           ",\"lease\":" + render_fields(e._lease) + "}";
}

}  // namespace lease_detail

template<stream_cluster Cluster, aws_auth::aws_request_authorizer Authorizer> class lease_api {
public:
    lease_api(Cluster& cluster, Authorizer& authorizer, front_end_config config)
        : _cluster(cluster), _authorizer(authorizer), _config(std::move(config)) {}

    [[nodiscard]] static auto handles(std::string_view path) -> bool {
        return path.starts_with(k_lease_path_prefix);
    }

    [[nodiscard]] auto handle(const front_end_request& req) -> aws_json::http_response {
        try {
            return dispatch(req);
        } catch (const std::exception&) {
            return error(500, "InternalFailure", "Internal service failure.");
        }
    }

private:
    Cluster& _cluster;
    Authorizer& _authorizer;
    front_end_config _config;

    [[nodiscard]] static auto error(unsigned status, std::string_view type,
                                    std::string_view message,
                                    std::optional<std::uint64_t> version = std::nullopt)
        -> aws_json::http_response {
        boost::json::object o{{"__type", type}, {"message", message}};
        if (version) {
            o["version"] = *version;
        }
        aws_json::http_response r;
        r.status = status;
        r.headers = {{"Content-Type", "application/json"}};
        r.body = boost::json::serialize(o);
        return r;
    }

    [[nodiscard]] static auto ok(std::string body, std::optional<std::uint64_t> version)
        -> aws_json::http_response {
        aws_json::http_response r;
        r.headers = {{"Content-Type", "application/json"}};
        if (version) {
            r.headers.emplace_back("ETag", lease_detail::etag(*version));
        }
        r.body = std::move(body);
        return r;
    }

    [[nodiscard]] static auto validation(std::string_view message) -> aws_json::http_response {
        return error(400, "ValidationException", message);
    }

    [[nodiscard]] auto table_arn(std::string_view account, std::string_view app) const
        -> std::string {
        return "arn:" + _config._partition + ":dynamodb:" + _config._region + ":" +
               std::string(account) + ":table/" + std::string(app);
    }

    auto dispatch(const front_end_request& req) -> aws_json::http_response {
        // /v1/leases/<app>[/<key>]
        auto rest = std::string_view(req._path).substr(k_lease_path_prefix.size());
        const auto slash = rest.find('/');
        const auto app = rest.substr(0, slash);
        const auto raw_key =
            slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
        const auto key = lease_detail::percent_decode(raw_key);

        sigv4::service_rules rules;
        rules.service = "kinesis";
        rules.protocol = sigv4::error_protocol::json;
        if (!_config._region.empty()) {
            rules.regions = {_config._region};
        }
        const sigv4::request sreq{req._method, req._path, req._query, req._headers, req._body};
        const auto who = _authorizer.authenticate(sreq, rules);
        if (!who) {
            const auto& e = who.error();
            return error(static_cast<unsigned>(e.http_status), e.code, e.message);
        }

        if (!valid_application(app)) {
            return validation(
                "The application name must be 3 to 255 characters of "
                "[A-Za-z0-9_.-].");
        }
        if (!key || (!key->empty() && !valid_lease_key(*key))) {
            return validation(
                "The lease key must be 1 to 1024 bytes with no control "
                "characters, percent-encoded in the path.");
        }
        const std::string application(app);
        const auto& m = req._method;
        std::string action;
        if (key->empty()) {
            action = m == "GET" ? "Scan" : m == "DELETE" ? "DeleteTable" : "";
        } else {
            action = m == "GET"      ? "GetItem"
                     : m == "PUT"    ? "PutItem"
                     : m == "PATCH"  ? "UpdateItem"
                     : m == "DELETE" ? "DeleteItem"
                                     : "";
        }
        if (action.empty()) {
            return error(405, "MethodNotAllowed", "The method is not allowed on this resource.");
        }
        aws_auth::authz_request az;
        az.action = "dynamodb:" + action;
        az.resource = table_arn(who->account, application);
        az.source_ip = req._source_ip;
        az.user_agent = std::string(lease_detail::header(req, "user-agent").value_or(""));
        az.region = _config._region;
        az.secure_transport = req._secure;
        if (const auto d = _authorizer.authorize(*who, az); !d.allowed) {
            return error(403, "AccessDeniedException",
                         "User: " + who->arn + " is not authorized to perform: " + az.action +
                             " on resource: " + az.resource);
        }

        const auto& account = who->account;
        if (key->empty()) {
            return m == "GET" ? list(req, account, application) : drop(account, application);
        }
        if (m == "GET") {
            return get(account, application, *key);
        }
        return write(req, account, application, *key);
    }

    auto propose(const stream_command& c) -> std::optional<apply_result> {
        return _cluster.propose(command_key(c), encode_command(c), admission::none);
    }

    [[nodiscard]] static auto unavailable() -> aws_json::http_response {
        return error(503, "ServiceUnavailableException",
                     "The lease store did not answer in time; retry.");
    }

    auto get(const std::string& account, const std::string& app, const std::string& key)
        -> aws_json::http_response {
        const auto r = propose(lease_read_command{
            ._account = account, ._application = app, ._kind = lease_read_kind::get, ._key = key});
        if (!r) {
            return unavailable();
        }
        if (r->_status == apply_status::not_found) {
            return error(404, "ResourceNotFoundException", "No lease under that key.");
        }
        if (r->_status != apply_status::ok) {
            return validation("The lease request is not valid.");
        }
        const auto page = decode_lease_page(r->_payload);
        if (page._leases.size() != 1) {
            return error(500, "InternalFailure", "Internal service failure.");
        }
        return ok(lease_detail::render_entry(page._leases.front()), r->_id);
    }

    auto list(const front_end_request& req, const std::string& account, const std::string& app)
        -> aws_json::http_response {
        const auto prefix = lease_detail::query_param(req._query, "prefix").value_or("");
        const auto after = lease_detail::query_param(req._query, "start-after").value_or("");
        std::uint32_t limit = 0;
        if (const auto l = lease_detail::query_param(req._query, "limit")) {
            const auto [p, ec] = std::from_chars(l->data(), l->data() + l->size(), limit);
            if (l->empty() || ec != std::errc{} || p != l->data() + l->size() || limit == 0 ||
                limit > k_max_lease_page) {
                return validation("limit must be 1 to 1000.");
            }
        }
        const auto r = propose(lease_read_command{._account = account,
                                                  ._application = app,
                                                  ._kind = lease_read_kind::list,
                                                  ._key = after,
                                                  ._prefix = prefix,
                                                  ._limit = limit});
        if (!r) {
            return unavailable();
        }
        if (r->_status != apply_status::ok) {
            return validation("The lease request is not valid.");
        }
        const auto page = decode_lease_page(r->_payload);
        std::string body = "{\"leases\":[";
        for (std::size_t i = 0; i < page._leases.size(); ++i) {
            if (i != 0) {
                body.push_back(',');
            }
            body += lease_detail::render_entry(page._leases[i]);
        }
        body += "]";
        if (page._more && !page._leases.empty()) {
            body += ",\"nextStartAfter\":" +
                    boost::json::serialize(boost::json::string(page._leases.back()._key));
        }
        body += "}";
        return ok(std::move(body), std::nullopt);
    }

    auto drop(const std::string& account, const std::string& app) -> aws_json::http_response {
        const auto r =
            propose(lease_drop_application_command{._account = account, ._application = app});
        if (!r) {
            return unavailable();
        }
        if (r->_status != apply_status::ok) {
            return validation("The lease request is not valid.");
        }
        return ok("{\"deleted\":" + std::to_string(r->_first_counter) + "}", std::nullopt);
    }

    auto write(const front_end_request& req, const std::string& account, const std::string& app,
               const std::string& key) -> aws_json::http_response {
        lease_write_command c;
        c._account = account;
        c._application = app;
        c._key = key;
        const auto& m = req._method;
        c._op = m == "PUT" ? lease_op::put : m == "PATCH" ? lease_op::patch : lease_op::remove;

        const auto if_match = lease_detail::header(req, "if-match");
        const auto if_none = lease_detail::header(req, "if-none-match");
        if (if_match && if_none) {
            return validation("Send If-Match or If-None-Match, not both.");
        }
        if (if_none) {
            if (*if_none != "*" || c._op != lease_op::put) {
                return validation("If-None-Match must be * and only on PUT.");
            }
            c._condition = lease_condition::absent;
        } else if (if_match) {
            const auto v = lease_detail::parse_version(*if_match);
            if (!v) {
                return validation("If-Match must be a lease version.");
            }
            c._condition = lease_condition::version;
            c._expected_version = *v;
        }

        if (c._op != lease_op::remove) {
            boost::system::error_code ec;
            auto body = boost::json::parse(req._body, ec);
            if (ec || !body.is_object()) {
                return validation("The body must be a JSON object.");
            }
            for (const auto& member : body.as_object()) {
                const std::string name(member.key());
                if (name.empty()) {
                    return validation("Lease field names must not be empty.");
                }
                if (member.value().is_null() && c._op == lease_op::patch) {
                    c._removed.push_back(name);
                } else {
                    c._fields[name] = boost::json::serialize(member.value());
                }
            }
            if (lease_bytes(c._fields) > k_max_lease_bytes) {
                return validation("A lease must be at most 64 KiB.");
            }
        }

        const auto r = propose(c);
        if (!r) {
            return unavailable();
        }
        switch (r->_status) {
            case apply_status::ok:
                return ok("{\"version\":" + std::to_string(r->_id) + "}", r->_id);
            case apply_status::not_found:
                return error(404, "ResourceNotFoundException", "No lease under that key.");
            case apply_status::condition_failed:
                return error(412, "ConditionalCheckFailedException",
                             "The conditional request failed.", r->_id);
            default:
                return validation("The lease request is not valid.");
        }
    }
};

}  // namespace kythira::kinesis
