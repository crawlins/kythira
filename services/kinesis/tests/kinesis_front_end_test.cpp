// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The front end (`.kiro/specs/kinesis-compatible-streams/` task 4,
// Requirements 4 and 5): SigV4 before the body, JSON and CBOR in and out,
// unsupported operations, validation against the pinned model, the
// StreamName/StreamARN agreement check, authorization of the same-named
// action on the right resource with the request's tags, handler-driven
// authorization and its safety net, and errors. Real SigV4 through the
// static-key authorizer, plus a recording authorizer for the resource and
// key checks. Also STS GetCallerIdentity, which KPL calls on the same
// endpoint before it writes.

#define BOOST_TEST_MODULE kinesis_front_end_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_front_end.hpp>

#include <raft/static_key_authorizer.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kin = kythira::kinesis;
namespace aj = kythira::aws_json;
namespace az = kythira::aws_auth;
namespace sv4 = kythira::sigv4;
namespace json = boost::json;

namespace {

constexpr std::string_view k_secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
constexpr std::string_view k_account = "111122223333";
constexpr std::string_view k_json = "application/x-amz-json-1.1";
constexpr std::string_view k_cbor = "application/x-amz-cbor-1.1";
const auto k_now = sv4::detail::parse_amz_date("20261110T120000Z").value();

auto request(std::string_view op, std::string body, std::string_view content_type = k_json)
    -> kin::front_end_request {
    kin::front_end_request r;
    r._method = "POST";
    r._path = "/";
    r._body = std::move(body);
    r._source_ip = "192.0.2.10";
    r._headers = {{"Host", "localhost:4567"},
                  {"Content-Type", std::string(content_type)},
                  {"X-Amz-Date", "20261110T120000Z"},
                  {"X-Amz-Target", "Kinesis_20131202." + std::string(op)}};
    return r;
}

// Signs `r` as `akid` (header form) for `service`, as an SDK does.
auto sign(kin::front_end_request r, std::string_view akid, std::string_view service = "kinesis")
    -> kin::front_end_request {
    sv4::service_rules rules;
    rules.service = std::string(service);
    rules.protocol = sv4::error_protocol::json;
    const sv4::credential_scope scope{"20261110", "us-east-1", std::string(service)};
    // A Query-protocol call (STS) has no X-Amz-Target to sign.
    const bool has_target = !kin::detail::header_value(r, "X-Amz-Target").empty();
    r._headers.emplace_back("Authorization", "AWS4-HMAC-SHA256 Credential=" + std::string(akid) +
                                                 "/" + scope.text() +
                                                 ", SignedHeaders=content-type;host;x-amz-date" +
                                                 (has_target ? ";x-amz-target" : "") +
                                                 ", Signature=" + std::string(64, '0'));
    const auto parsed =
        sv4::parse(sv4::request{r._method, r._path, r._query, r._headers, r._body}, rules, k_now);
    BOOST_REQUIRE_MESSAGE(parsed.has_value(), parsed.error().message);
    const auto sig = sv4::compute_signature(*parsed, sv4::derive_signing_key(k_secret, scope));
    r._headers.back().second.replace(r._headers.back().second.size() - 64, 64, sig);
    return r;
}

// An STS Query call as KPL sends it: a form body and no X-Amz-Target.
auto sts_request(std::string body) -> kin::front_end_request {
    kin::front_end_request r;
    r._method = "POST";
    r._path = "/";
    r._body = std::move(body);
    r._headers = {{"Host", "localhost:4567"},
                  {"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"},
                  {"X-Amz-Date", "20261110T120000Z"}};
    return r;
}

auto body_of(const aj::http_response& r) -> json::object {
    return json::parse(r.body).as_object();
}

auto type_of(const aj::http_response& r) -> std::string {
    return std::string(body_of(r).at("__type").as_string());
}

auto cbor_body(std::string_view op, const json::value& input) -> std::string {
    const auto* o = kin::kinesis_model::k_model.find_operation(op);
    BOOST_REQUIRE(o != nullptr);
    return aj::json_to_cbor(kin::kinesis_model::k_model, o->input, input);
}

auto echo(kin::call_context& ctx, json::object& in) -> aj::result<json::object> {
    json::object out;
    out["op"] = std::string(ctx._operation);
    out["who"] = ctx._principal.arn;
    out["replica"] = ctx._replica_read_ok;
    if (ctx._stream) {
        out["stream"] = ctx._stream->_name;
        out["stream_arn"] = ctx._stream->_arn;
    }
    if (const auto* data = in.if_contains("Data")) {
        out["data"] = *data;
    }
    return out;
}

struct static_fixture {
    az::static_key_authorizer auth{std::vector<az::static_key>{
                                       {"AKIDREAD", std::string(k_secret), std::string(k_account),
                                        "reader", az::static_role::read_only},
                                       {"AKIDWRITE", std::string(k_secret), std::string(k_account),
                                        "writer", az::static_role::read_write},
                                   },
                                   "aws",
                                   [] { return std::chrono::system_clock::time_point(k_now); }};
    std::unique_ptr<kin::front_end<az::static_key_authorizer>> fe;

    static_fixture() {
        auth.register_service(kin::kinesis_static_roles());
        fe = std::make_unique<kin::front_end<az::static_key_authorizer>>(kin::front_end_config{},
                                                                         auth);
        fe->on("DescribeStreamSummary", echo);
        fe->on("PutRecord", echo);
        fe->on("ListStreams", echo);
    }
};

// Authenticates everyone as one IAM user and records what it is asked.
struct recording_authorizer {
    std::vector<az::authz_request> asked;
    std::set<std::string> deny;

    auto authenticate(const sv4::request&, const sv4::service_rules&) -> az::authn_result {
        az::principal_context p;
        p.account = std::string(k_account);
        p.arn = "arn:aws:iam::111122223333:user/alice";
        p.access_key_id = "AKIDALICE";
        return p;
    }
    auto authorize(const az::principal_context&, const az::authz_request& r) -> az::authz_decision {
        asked.push_back(r);
        az::authz_decision d;
        d.allowed = !deny.contains(r.action);
        d.explicit_deny = !d.allowed && r.action.ends_with("DeleteStream");
        return d;
    }
};
static_assert(az::aws_request_authorizer<recording_authorizer>);

struct recording_fixture {
    recording_authorizer auth;
    kin::front_end<recording_authorizer> fe{kin::front_end_config{}, auth};

    recording_fixture() {
        for (const auto* op :
             {"PutRecord", "PutRecords", "CreateStream", "TagResource", "UntagResource",
              "SubscribeToShard", "UpdateShardCount", "DescribeStreamConsumer",
              "RegisterStreamConsumer", "DeleteStream", "ListStreams"}) {
            fe.on(op, echo);
        }
    }

    auto send(std::string_view op, std::string body) -> aj::http_response {
        return fe.handle(request(op, std::move(body)));
    }
};

const std::string k_stream_arn = "arn:aws:kinesis:us-east-1:111122223333:stream/orders";

}  // namespace

BOOST_AUTO_TEST_CASE(a_signed_read_reaches_its_handler) {
    static_fixture f;
    const auto r = f.fe->handle(
        sign(request("DescribeStreamSummary", R"({"StreamName":"orders"})"), "AKIDREAD"));
    BOOST_TEST(r.status == 200U, r.body);
    const auto b = body_of(r);
    BOOST_TEST(b.at("op").as_string() == "DescribeStreamSummary");
    BOOST_TEST(b.at("who").as_string() == "arn:aws:iam::111122223333:user/reader");
    BOOST_TEST(b.at("stream_arn").as_string() == k_stream_arn);
    BOOST_TEST(b.at("replica").as_bool());
    BOOST_TEST(r.header("Content-Type").value_or("") == k_json);
    BOOST_TEST(r.header("x-amzn-RequestId").has_value());
}

// KPL asks STS who it is before its first write and builds the StreamARN
// it sends from the answer; the Kinesis endpoint answers that one action.
BOOST_AUTO_TEST_CASE(sts_get_caller_identity_is_answered_from_the_signature) {
    static_fixture f;
    const auto ok = f.fe->handle(
        sign(sts_request("Action=GetCallerIdentity&Version=2011-06-15"), "AKIDWRITE", "sts"));
    BOOST_TEST(ok.status == 200U, ok.body);
    BOOST_TEST(ok.header("Content-Type").value_or("") == "text/xml");
    BOOST_TEST(ok.body.starts_with("<GetCallerIdentityResponse "
                                   "xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">"
                                   "<GetCallerIdentityResult>"),
               ok.body);
    BOOST_TEST(
        ok.body.find("<Arn>arn:aws:iam::111122223333:user/writer</Arn>") != std::string::npos,
        ok.body);
    BOOST_TEST(ok.body.find("<UserId>AKIDWRITE</UserId>") != std::string::npos, ok.body);
    BOOST_TEST(ok.body.find("<Account>111122223333</Account>") != std::string::npos, ok.body);

    // Signed for kinesis rather than sts: the scope is wrong.
    const auto scope =
        f.fe->handle(sign(sts_request("Action=GetCallerIdentity&Version=2011-06-15"), "AKIDWRITE"));
    BOOST_TEST(scope.status >= 400U, scope.body);
    BOOST_TEST(scope.body.starts_with("<ErrorResponse"), scope.body);

    // Unsigned: refused in STS's own words.
    const auto unsigned_call =
        f.fe->handle(sts_request("Action=GetCallerIdentity&Version=2011-06-15"));
    BOOST_TEST(unsigned_call.status >= 400U);
    BOOST_TEST(unsigned_call.body.find("<Type>Sender</Type>") != std::string::npos,
               unsigned_call.body);

    // Every other STS action is refused, credentials included.
    const auto other = f.fe->handle(
        sign(sts_request("Action=AssumeRole&Version=2011-06-15&RoleArn=x&RoleSessionName=s"),
             "AKIDWRITE", "sts"));
    BOOST_TEST(other.status == 400U);
    BOOST_TEST(other.body.find("<Code>InvalidAction</Code>") != std::string::npos, other.body);

    // A form body with an X-Amz-Target is still a Kinesis call, and the
    // codec refuses its content type.
    auto framed = sts_request("Action=GetCallerIdentity");
    framed._headers.emplace_back("X-Amz-Target", "Kinesis_20131202.ListStreams");
    const auto kinesis = f.fe->handle(sign(std::move(framed), "AKIDWRITE"));
    BOOST_TEST(kinesis.status >= 400U);
    BOOST_TEST(kinesis.body.find("GetCallerIdentityResponse") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(cbor_requests_are_answered_in_cbor) {
    // The Java SDK v2 (and so KCL) sends CBOR with Data as a byte string.
    static_fixture f;
    json::object in;
    in["StreamName"] = "orders";
    in["PartitionKey"] = "pk";
    in["Data"] = "aGVsbG8=";  // "hello"
    const auto r =
        f.fe->handle(sign(request("PutRecord", cbor_body("PutRecord", in), k_cbor), "AKIDWRITE"));
    BOOST_TEST(r.status == 200U);
    BOOST_TEST(r.header("Content-Type").value_or("") == k_cbor);
    const auto out = aj::cbor_to_json(kin::kinesis_model::k_model, aj::no_shape, r.body);
    BOOST_REQUIRE(out.has_value());
    BOOST_TEST(out->at("op").as_string() == "PutRecord");
    BOOST_TEST(!out->at("replica").as_bool());

    // Errors come back in CBOR too, with the bare `__type`.
    const auto denied =
        f.fe->handle(sign(request("PutRecord", cbor_body("PutRecord", in), k_cbor), "AKIDREAD"));
    BOOST_TEST(denied.status == 400U);
    BOOST_TEST(denied.header("Content-Type").value_or("") == k_cbor);
    const auto err = aj::cbor_to_json(kin::kinesis_model::k_model, aj::no_shape, denied.body);
    BOOST_REQUIRE(err.has_value());
    BOOST_TEST(err->at("__type").as_string() == "AccessDeniedException");
}

BOOST_AUTO_TEST_CASE(sigv4_comes_before_the_body) {
    static_fixture f;
    // Unsigned, with a body that would not even parse: the caller learns
    // only that it must sign (the core verifier's JSON-protocol wording).
    const auto unsigned_r = f.fe->handle(request("PutRecord", "{not json"));
    BOOST_TEST(unsigned_r.status == 400U);
    BOOST_TEST(type_of(unsigned_r) == "IncompleteSignatureException");

    // Signed for another service.
    const auto wrong_service = f.fe->handle(
        sign(request("DescribeStreamSummary", R"({"StreamName":"orders"})"), "AKIDREAD", "s3"));
    BOOST_TEST(wrong_service.status != 200U);
    BOOST_TEST(type_of(wrong_service) != "AccessDeniedException");

    // A key nobody configured.
    const auto stranger = f.fe->handle(
        sign(request("DescribeStreamSummary", R"({"StreamName":"orders"})"), "AKIDNOBODY"));
    BOOST_TEST(type_of(stranger) == "UnrecognizedClientException");
}

BOOST_AUTO_TEST_CASE(framing_and_unsupported_operations) {
    static_fixture f;
    // A modeled operation with no handler (doc/unsupported.md), after SigV4.
    const auto unsupported =
        f.fe->handle(sign(request("SplitShard", R"({"StreamName":"orders"})"), "AKIDREAD"));
    BOOST_TEST(unsupported.status == 400U);
    BOOST_TEST(type_of(unsupported) == "UnknownOperationException");
    BOOST_TEST(!body_of(unsupported).contains("message"));

    // A target the model does not have.
    BOOST_TEST(type_of(f.fe->handle(sign(request("Frobnicate", "{}"), "AKIDREAD"))) ==
               "UnknownOperationException");

    // The wrong JSON version is not a Kinesis request.
    auto v10 = request("ListStreams", "{}", "application/x-amz-json-1.0");
    BOOST_TEST(type_of(f.fe->handle(sign(v10, "AKIDREAD"))) == "UnknownOperationException");

    // The model's constraints: a stream name with a space.
    const auto bad = f.fe->handle(
        sign(request("DescribeStreamSummary", R"({"StreamName":"no spaces"})"), "AKIDREAD"));
    BOOST_TEST(bad.status == 400U);
    BOOST_TEST(type_of(bad) == "ValidationException");

    // A body that is not JSON, once signed.
    BOOST_TEST(type_of(f.fe->handle(sign(request("ListStreams", "[1]"), "AKIDREAD"))) ==
               "SerializationException");
}

BOOST_AUTO_TEST_CASE(roles_gate_the_same_named_action) {
    static_fixture f;
    const std::string body = R"({"StreamName":"orders","PartitionKey":"pk","Data":"aGk="})";
    const auto denied = f.fe->handle(sign(request("PutRecord", body), "AKIDREAD"));
    BOOST_TEST(denied.status == 400U);
    BOOST_TEST(type_of(denied) == "AccessDeniedException");
    BOOST_TEST(body_of(denied).at("message").as_string() ==
               "User: arn:aws:iam::111122223333:user/reader is not authorized to perform: "
               "kinesis:PutRecord on resource: " +
                   k_stream_arn +
                   " because no identity-based policy allows the kinesis:PutRecord action");
    BOOST_TEST(f.fe->handle(sign(request("PutRecord", body), "AKIDWRITE")).status == 200U);
    BOOST_TEST(f.fe->handle(sign(request("ListStreams", "{}"), "AKIDREAD")).status == 200U);
}

BOOST_AUTO_TEST_CASE(stream_name_and_arn_must_agree) {
    recording_fixture f;
    // KPL sends both on every PutRecords.
    const auto agree =
        f.send("PutRecords", R"({"StreamName":"orders","StreamARN":")" + k_stream_arn +
                                 R"(","Records":[{"Data":"aGk=","PartitionKey":"pk"}]})");
    BOOST_TEST(agree.status == 200U, agree.body);
    BOOST_TEST(body_of(agree).at("stream").as_string() == "orders");
    BOOST_REQUIRE(f.auth.asked.size() == 1U);
    BOOST_TEST(f.auth.asked[0].resource == k_stream_arn);

    // A caller must not be authorized on one stream and act on another.
    f.auth.asked.clear();
    const auto disagree =
        f.send("PutRecord", R"({"StreamName":"payroll","StreamARN":")" + k_stream_arn +
                                R"(","PartitionKey":"pk","Data":"aGk="})");
    BOOST_TEST(disagree.status == 400U);
    BOOST_TEST(type_of(disagree) == "InvalidArgumentException");
    BOOST_TEST(f.auth.asked.empty());
}

BOOST_AUTO_TEST_CASE(resources_and_tags_come_from_the_input) {
    recording_fixture f;

    // By name: the stream in the caller's account and the configured region
    // and partition.
    BOOST_TEST(
        f.send("PutRecord", R"({"StreamName":"orders","PartitionKey":"k","Data":"aGk="})").status ==
        200U);
    BOOST_REQUIRE(f.auth.asked.size() == 1U);
    BOOST_TEST(f.auth.asked[0].action == "kinesis:PutRecord");
    BOOST_TEST(f.auth.asked[0].resource == k_stream_arn);
    BOOST_TEST(f.auth.asked[0].resource_account.empty());
    BOOST_TEST(f.auth.asked[0].source_ip == "192.0.2.10");

    // Another account's stream by ARN: the authorizer sees whose it is.
    f.auth.asked.clear();
    const std::string theirs = "arn:aws:kinesis:us-east-1:444455556666:stream/shared";
    BOOST_TEST(
        f.send("PutRecord", R"({"StreamARN":")" + theirs + R"(","PartitionKey":"k","Data":"aGk="})")
            .status == 200U);
    BOOST_TEST(f.auth.asked.at(0).resource == theirs);
    BOOST_TEST(f.auth.asked.at(0).resource_account == "444455556666");

    // A consumer by ARN.
    f.auth.asked.clear();
    const std::string consumer = k_stream_arn + "/consumer/app:1762776000";
    BOOST_TEST(
        f.send("SubscribeToShard",
               R"({"ConsumerARN":")" + consumer +
                   R"(","ShardId":"shardId-000000000000","StartingPosition":{"Type":"LATEST"}})")
            .status == 200U);
    BOOST_TEST(f.auth.asked.at(0).resource == consumer);

    // Tagging: the request's tags, and the keys being removed.
    f.auth.asked.clear();
    BOOST_TEST(
        f.send("TagResource", R"({"ResourceARN":")" + k_stream_arn + R"(","Tags":{"team":"a"}})")
            .status == 200U);
    BOOST_TEST(f.auth.asked.at(0).resource == k_stream_arn);
    BOOST_REQUIRE(f.auth.asked.at(0).request_tags.has_value());
    BOOST_TEST(f.auth.asked.at(0).request_tags->at(0).first == "team");
    f.auth.asked.clear();
    BOOST_TEST(f.send("UntagResource",
                      R"({"ResourceARN":")" + k_stream_arn + R"(","TagKeys":["team","cost"]})")
                   .status == 200U);
    BOOST_REQUIRE(f.auth.asked.at(0).keys.size() == 1U);
    BOOST_TEST(f.auth.asked.at(0).keys[0].first == "aws:TagKeys");
    BOOST_TEST((f.auth.asked.at(0).keys[0].second == std::vector<std::string>{"team", "cost"}));

    // CreateStream with tags also needs AddTagsToStream, as on AWS.
    f.auth.asked.clear();
    BOOST_TEST(f.send("CreateStream", R"({"StreamName":"orders","Tags":{"team":"a"}})").status ==
               200U);
    BOOST_REQUIRE(f.auth.asked.size() == 2U);
    BOOST_TEST(f.auth.asked[0].action == "kinesis:CreateStream");
    BOOST_TEST(f.auth.asked[1].action == "kinesis:AddTagsToStream");
    BOOST_TEST(f.auth.asked[1].resource == k_stream_arn);
    f.auth.asked.clear();
    BOOST_TEST(f.send("CreateStream", R"({"StreamName":"orders"})").status == 200U);
    BOOST_TEST(f.auth.asked.size() == 1U);

    // Actions AWS defines without a resource are authorized on `*`.
    f.auth.asked.clear();
    BOOST_TEST(
        f.send("UpdateShardCount",
               R"({"StreamName":"orders","TargetShardCount":4,"ScalingType":"UNIFORM_SCALING"})")
            .status == 200U);
    BOOST_TEST(f.auth.asked.at(0).resource == "*");
    f.auth.asked.clear();
    BOOST_TEST(f.send("ListStreams", "{}").status == 200U);
    BOOST_TEST(f.auth.asked.at(0).resource == "*");

    // RegisterStreamConsumer acts on the stream.
    f.auth.asked.clear();
    BOOST_TEST(f.send("RegisterStreamConsumer",
                      R"({"StreamARN":")" + k_stream_arn + R"(","ConsumerName":"app"})")
                   .status == 200U);
    BOOST_TEST(f.auth.asked.at(0).resource == k_stream_arn);
}

BOOST_AUTO_TEST_CASE(explicit_deny_is_worded_as_aws_words_it) {
    recording_fixture f;
    f.auth.deny = {"kinesis:DeleteStream"};
    const auto r = f.send("DeleteStream", R"({"StreamName":"orders"})");
    BOOST_TEST(type_of(r) == "AccessDeniedException");
    BOOST_TEST(std::string(body_of(r).at("message").as_string())
                   .ends_with("with an explicit deny in an identity-based policy"));
}

BOOST_AUTO_TEST_CASE(the_resource_lookup_supplies_tags_and_policy) {
    recording_fixture f;
    f.fe.set_resource_lookup([](std::string_view arn) -> std::optional<kin::resource_facts> {
        if (arn != k_stream_arn) {
            return std::nullopt;
        }
        return kin::resource_facts{{{"team", "payments"}}, R"({"Version":"2012-10-17"})"};
    });
    BOOST_TEST(
        f.send("PutRecord", R"({"StreamName":"orders","PartitionKey":"k","Data":"aGk="})").status ==
        200U);
    const auto& r = f.auth.asked.at(0);
    BOOST_REQUIRE(r.resource_tags.size() == 1U);
    BOOST_TEST(r.resource_tags[0].second == "payments");
    BOOST_TEST(r.resource_policy.value_or("") == R"({"Version":"2012-10-17"})");

    // An unknown stream carries no facts; its handler answers not-found.
    f.auth.asked.clear();
    BOOST_TEST(
        f.send("PutRecord", R"({"StreamName":"other","PartitionKey":"k","Data":"aGk="})").status ==
        200U);
    BOOST_TEST(f.auth.asked.at(0).resource_tags.empty());
    BOOST_TEST(!f.auth.asked.at(0).resource_policy.has_value());
}

BOOST_AUTO_TEST_CASE(handlers_authorize_what_the_front_end_cannot) {
    recording_authorizer auth;
    kin::front_end<recording_authorizer> fe{kin::front_end_config{}, auth};
    // GetRecords names its stream only inside the iterator token.
    fe.on("GetRecords", [](kin::call_context& ctx, json::object&) -> aj::result<json::object> {
        az::authz_request r;
        r.action = "kinesis:GetRecords";
        r.resource = ctx.stream_arn("orders");
        if (auto denied = ctx.authorize(r)) {
            return *denied;
        }
        return json::object{{"Records", json::array{}}};
    });
    // A buggy handler that forgets to.
    fe.on("ListShards", [](kin::call_context&, json::object&) {
        return aj::result<json::object>(json::object{});
    });
    // A consumer named by stream and consumer name.
    fe.on("DescribeStreamConsumer", echo);

    const auto ok = fe.handle(request("GetRecords", R"({"ShardIterator":"AAAA"})"));
    BOOST_TEST(ok.status == 200U, ok.body);
    BOOST_REQUIRE(auth.asked.size() == 1U);
    BOOST_TEST(auth.asked[0].resource == k_stream_arn);

    auth.deny = {"kinesis:GetRecords"};
    BOOST_TEST(type_of(fe.handle(request("GetRecords", R"({"ShardIterator":"AAAA"})"))) ==
               "AccessDeniedException");

    // Data never leaves unauthorized.
    const auto leak = fe.handle(request("ListShards", R"({"NextToken":"abc"})"));
    BOOST_TEST(leak.status == 500U);
    BOOST_TEST(type_of(leak) == "InternalFailure");

    auth.asked.clear();
    const auto consumer =
        fe.handle(request("DescribeStreamConsumer",
                          R"({"StreamARN":")" + k_stream_arn + R"(","ConsumerName":"app"})"));
    BOOST_TEST(auth.asked.empty());
    BOOST_TEST(consumer.status == 500U);
}

BOOST_AUTO_TEST_CASE(handler_failures_and_registration_errors) {
    recording_authorizer auth;
    kin::front_end<recording_authorizer> fe{kin::front_end_config{}, auth};
    fe.on("ListStreams", [](kin::call_context&, json::object&) -> aj::result<json::object> {
        throw std::runtime_error("boom: secret detail");
    });
    fe.on("DescribeLimits", [](kin::call_context&, json::object&) -> aj::result<json::object> {
        return kin::resource_not_found("Stream x under account 111122223333 not found.");
    });
    const auto r = fe.handle(request("ListStreams", "{}"));
    BOOST_TEST(r.status == 500U);
    BOOST_TEST(type_of(r) == "InternalFailure");
    BOOST_TEST(r.body.find("secret") == std::string::npos);

    const auto nf = fe.handle(request("DescribeLimits", "{}"));
    BOOST_TEST(nf.status == 400U);
    BOOST_TEST(type_of(nf) == "ResourceNotFoundException");

    BOOST_CHECK_THROW(fe.on("ListStreams", echo), std::invalid_argument);
    BOOST_CHECK_THROW(fe.on("ListTables", echo), std::invalid_argument);
    BOOST_TEST(fe.handles("ListStreams"));
    BOOST_TEST(!fe.handles("PutRecord"));
}

BOOST_AUTO_TEST_CASE(kinesis_arns_parse) {
    // The parts view the argument, so it must outlive them.
    const std::string consumer = k_stream_arn + "/consumer/app:1762776000";
    const auto a = kin::parse_kinesis_arn(consumer);
    BOOST_REQUIRE(a.has_value());
    BOOST_TEST(a->_partition == "aws");
    BOOST_TEST(a->_region == "us-east-1");
    BOOST_TEST(a->_account == "111122223333");
    BOOST_TEST(a->_resource == "stream/orders/consumer/app:1762776000");
    BOOST_TEST(kin::stream_name_of(k_stream_arn).value_or("") == "orders");
    BOOST_TEST(!kin::stream_name_of(k_stream_arn + "/consumer/app:1").has_value());
    BOOST_TEST(
        !kin::stream_name_of("arn:aws:kinesis:us-east-1:111122223333:channel/c").has_value());
    BOOST_TEST(!kin::parse_kinesis_arn("arn:aws:s3:::bucket/key").has_value());
    BOOST_TEST(!kin::parse_kinesis_arn("arn:aws:kinesis:us-east-1").has_value());
    BOOST_TEST(kin::call_context::consumer_arn(k_stream_arn, "app", 1762776000) ==
               k_stream_arn + "/consumer/app:1762776000");
}
