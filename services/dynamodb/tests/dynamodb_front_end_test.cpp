// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The front end (`.kiro/specs/dynamodb-compatible-api/` task 4,
// Requirements 4 and 5): routing by target prefix, SigV4 before the body,
// unsupported operations, validation, authorization of the same-named
// action on the right resource with the condition keys the input carries,
// handler-driven authorization, DescribeEndpoints, and errors. Real SigV4
// through the static-key authorizer, plus a recording authorizer for the
// resource and key checks.

#define BOOST_TEST_MODULE dynamodb_front_end_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/front_end.hpp>

#include <raft/static_key_authorizer.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace ddb = kythira::dynamodb;
namespace az = kythira::aws_auth;
namespace sv4 = kythira::sigv4;
namespace json = boost::json;

namespace {

constexpr std::string_view k_secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
constexpr std::string_view k_account = "111122223333";
const auto k_now = sv4::detail::parse_amz_date("20261109T120000Z").value();

auto request(std::string_view target, std::string body) -> ddb::front_end_request {
    ddb::front_end_request r;
    r.method = "POST";
    r.path = "/";
    r.body = std::move(body);
    r.source_ip = "192.0.2.10";
    r.headers = {{"Host", "localhost:8000"},
                 {"Content-Type", "application/x-amz-json-1.0"},
                 {"X-Amz-Date", "20261109T120000Z"},
                 {"X-Amz-Target", std::string(target)}};
    return r;
}

// Signs `r` as `akid` (header form), as an SDK does.
auto sign(ddb::front_end_request r, std::string_view akid) -> ddb::front_end_request {
    sv4::service_rules rules;
    rules.service = "dynamodb";
    rules.protocol = sv4::error_protocol::json;
    const sv4::credential_scope scope{"20261109", "us-east-1", "dynamodb"};
    r.headers.emplace_back("Authorization",
                           "AWS4-HMAC-SHA256 Credential=" + std::string(akid) + "/" + scope.text() +
                               ", SignedHeaders=content-type;host;x-amz-date;x-amz-target"
                               ", Signature=" +
                               std::string(64, '0'));
    const auto parsed =
        sv4::parse(sv4::request{r.method, r.path, r.query, r.headers, r.body}, rules, k_now);
    BOOST_REQUIRE_MESSAGE(parsed.has_value(), parsed.error().message);
    const auto sig = sv4::compute_signature(*parsed, sv4::derive_signing_key(k_secret, scope));
    r.headers.back().second.replace(r.headers.back().second.size() - 64, 64, sig);
    return r;
}

auto body_of(const kythira::aws_json::http_response& r) -> json::object {
    return json::parse(r.body).as_object();
}

auto type_of(const kythira::aws_json::http_response& r) -> std::string {
    return std::string(body_of(r).at("__type").as_string());
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
    std::unique_ptr<ddb::front_end<az::static_key_authorizer>> fe;

    static_fixture() {
        auth.register_service(ddb::dynamodb_static_roles());
        ddb::front_end_config cfg;
        cfg.advertised_endpoint = "dynamodb.node-1.example:8000";
        fe = std::make_unique<ddb::front_end<az::static_key_authorizer>>(cfg, auth);
        const auto echo = [](ddb::call_context& ctx, json::object&) {
            json::object out;
            out["op"] = std::string(ctx.operation);
            out["who"] = ctx.principal.arn;
            return kythira::aws_json::result<json::object>(out);
        };
        fe->on("GetItem", echo);
        fe->on("PutItem", echo);
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
        d.explicit_deny = deny.contains(r.action) && r.action.ends_with("DeleteItem");
        return d;
    }
};
static_assert(az::aws_request_authorizer<recording_authorizer>);

}  // namespace

BOOST_AUTO_TEST_CASE(a_signed_read_reaches_its_handler) {
    static_fixture f;
    const auto r = f.fe->handle(
        sign(request("DynamoDB_20120810.GetItem", R"({"TableName":"t","Key":{"pk":{"S":"a"}}})"),
             "AKIDREAD"));
    BOOST_TEST(r.status == 200U);
    BOOST_TEST(body_of(r).at("op").as_string() == "GetItem");
    BOOST_TEST(body_of(r).at("who").as_string() == "arn:aws:iam::111122223333:user/reader");
    BOOST_TEST(r.header("x-amzn-RequestId").has_value());
    BOOST_TEST(r.header("x-amz-crc32").has_value());
}

BOOST_AUTO_TEST_CASE(sigv4_comes_before_the_body) {
    static_fixture f;
    // Unsigned, with a body that would fail validation: the auth error wins.
    const auto unsigned_r = f.fe->handle(request("DynamoDB_20120810.GetItem", R"({"Bogus":1})"));
    BOOST_TEST(unsigned_r.status == 400U);
    BOOST_TEST(type_of(unsigned_r).starts_with("com.amazon.coral.service#"));

    const auto unknown_key = f.fe->handle(
        sign(request("DynamoDB_20120810.GetItem", R"({"TableName":"t","Key":{}})"), "AKIDNOBODY"));
    BOOST_TEST(type_of(unknown_key) == "com.amazon.coral.service#UnrecognizedClientException");
    BOOST_TEST(body_of(unknown_key).at("message").as_string() ==
               "The security token included in the request is invalid.");
}

BOOST_AUTO_TEST_CASE(framing_and_unsupported_operations) {
    static_fixture f;
    // Not a DynamoDB target at all.
    const auto bad_prefix =
        f.fe->handle(sign(request("Kinesis_20131202.ListStreams", "{}"), "AKIDREAD"));
    BOOST_TEST(type_of(bad_prefix) == "com.amazon.coral.service#UnknownOperationException");
    // Modeled but not implemented: UnknownOperationException, body unread.
    const auto unsupported =
        f.fe->handle(sign(request("DynamoDB_20120810.CreateBackup", R"({"x":1})"), "AKIDREAD"));
    BOOST_TEST(type_of(unsupported) == "com.amazon.coral.service#UnknownOperationException");
    // Implemented: the body is validated after authentication.
    const auto invalid = f.fe->handle(
        sign(request("DynamoDB_20120810.GetItem", R"({"TableName":"t","Key":{},"Bogus":1})"),
             "AKIDREAD"));
    BOOST_TEST(type_of(invalid) == "com.amazon.coral.validate#ValidationException");
}

BOOST_AUTO_TEST_CASE(roles_gate_the_same_named_action) {
    static_fixture f;
    const std::string put = R"({"TableName":"t","Item":{"pk":{"S":"a"}}})";
    const auto denied = f.fe->handle(sign(request("DynamoDB_20120810.PutItem", put), "AKIDREAD"));
    BOOST_TEST(denied.status == 400U);
    BOOST_TEST(type_of(denied) == "com.amazon.coral.service#AccessDeniedException");
    BOOST_TEST(body_of(denied).at("message").as_string() ==
               "User: arn:aws:iam::111122223333:user/reader is not authorized to perform: "
               "dynamodb:PutItem on resource: arn:aws:dynamodb:us-east-1:111122223333:table/t "
               "because no identity-based policy allows the dynamodb:PutItem action");
    const auto allowed = f.fe->handle(sign(request("DynamoDB_20120810.PutItem", put), "AKIDWRITE"));
    BOOST_TEST(allowed.status == 200U);
}

BOOST_AUTO_TEST_CASE(describe_endpoints_answers_the_advertised_address) {
    static_fixture f;
    BOOST_TEST(f.fe->handles("DescribeEndpoints"));
    const auto r =
        f.fe->handle(sign(request("DynamoDB_20120810.DescribeEndpoints", ""), "AKIDREAD"));
    BOOST_TEST(r.status == 200U);
    const auto e = body_of(r).at("Endpoints").as_array().at(0).as_object();
    BOOST_TEST(e.at("Address").as_string() == "dynamodb.node-1.example:8000");
    BOOST_TEST(e.at("CachePeriodInMinutes").as_int64() == 1440);
}

BOOST_AUTO_TEST_CASE(streams_targets_use_the_streams_model) {
    static_fixture f;
    const auto r = f.fe->handle(sign(
        request("DynamoDBStreams_20120810.ListStreams", R"({"TableName":"orders"})"), "AKIDREAD"));
    BOOST_TEST(r.status == 200U, r.body);
    BOOST_TEST(body_of(r).at("op").as_string() == "ListStreams");
}

BOOST_AUTO_TEST_CASE(resources_and_condition_keys_come_from_the_input) {
    recording_authorizer auth;
    ddb::front_end<recording_authorizer> fe(ddb::front_end_config{}, auth);
    const auto ok = [](ddb::call_context&, json::object&) {
        return kythira::aws_json::result<json::object>(json::object{});
    };
    fe.on("Query", ok);
    fe.on("TagResource", ok);
    fe.on("DescribeStream", ok);

    auto q = request("DynamoDB_20120810.Query",
                     R"({"TableName":"t","IndexName":"gsi1","Select":"COUNT",)"
                     R"("ReturnConsumedCapacity":"TOTAL"})");
    q.headers.emplace_back("User-Agent", "aws-sdk-java/2.57.11 extra");
    BOOST_TEST(fe.handle(q).status == 200U);
    BOOST_REQUIRE(auth.asked.size() == 1U);
    const auto& a = auth.asked[0];
    BOOST_TEST(a.action == "dynamodb:Query");
    BOOST_TEST(a.resource == "arn:aws:dynamodb:us-east-1:111122223333:table/t/index/gsi1");
    BOOST_TEST(a.source_ip == "192.0.2.10");
    BOOST_TEST(a.user_agent == "aws-sdk-java/2.57.11 extra");
    BOOST_TEST(a.region == "us-east-1");
    std::set<std::string> keys;
    for (const auto& [k, v] : a.keys) {
        keys.insert(k + "=" + v.at(0));
    }
    BOOST_TEST((keys == std::set<std::string>{"dynamodb:ReturnConsumedCapacity=TOTAL",
                                              "dynamodb:Select=COUNT"}));

    const std::string arn = "arn:aws:dynamodb:us-east-1:111122223333:table/t";
    BOOST_TEST(fe.handle(request("DynamoDB_20120810.TagResource",
                                 R"({"ResourceArn":")" + arn + R"(","Tags":[]})"))
                   .status == 200U);
    BOOST_TEST(auth.asked.back().resource == arn);

    const std::string stream = arn + "/stream/2026-11-09T00:00:00.000";
    BOOST_TEST(fe.handle(request("DynamoDBStreams_20120810.DescribeStream",
                                 R"({"StreamArn":")" + stream + R"("})"))
                   .status == 200U);
    BOOST_TEST(auth.asked.back().action == "dynamodb:DescribeStream");
    BOOST_TEST(auth.asked.back().resource == stream);
}

BOOST_AUTO_TEST_CASE(handlers_authorize_what_the_front_end_cannot) {
    recording_authorizer auth;
    auth.deny = {"dynamodb:DeleteItem"};
    ddb::front_end<recording_authorizer> fe(ddb::front_end_config{}, auth);
    // TransactWriteItems has no same-named action: its handler authorizes
    // each element.
    fe.on("TransactWriteItems", [](ddb::call_context& ctx, json::object&) {
        for (const auto* action : {"dynamodb:PutItem", "dynamodb:DeleteItem"}) {
            az::authz_request r;
            r.action = action;
            r.resource = ctx.table_arn("t");
            if (auto denied = ctx.authorize(r)) {
                return kythira::aws_json::result<json::object>(*denied);
            }
        }
        return kythira::aws_json::result<json::object>(json::object{});
    });
    const auto r = fe.handle(request("DynamoDB_20120810.TransactWriteItems",
                                     R"({"TransactItems":[{"Delete":{"TableName":"t",)"
                                     R"("Key":{"pk":{"S":"a"}}}}]})"));
    BOOST_TEST(type_of(r) == "com.amazon.coral.service#AccessDeniedException");
    BOOST_TEST(body_of(r).at("message").as_string().ends_with(
        "dynamodb:DeleteItem on resource: arn:aws:dynamodb:us-east-1:111122223333:table/t with "
        "an explicit deny in an identity-based policy"));
    // No same-named action was asked for, only the handler's two.
    BOOST_REQUIRE(auth.asked.size() == 2U);
    BOOST_TEST(auth.asked[0].action == "dynamodb:PutItem");
}

BOOST_AUTO_TEST_CASE(handler_failures_and_registration_errors) {
    recording_authorizer auth;
    // The cause goes to the operator's hook, never to the client.
    std::vector<std::string> reported;
    ddb::front_end_config cfg;
    cfg.on_internal_error = [&reported](const ddb::internal_error_report& r) {
        BOOST_TEST(r.account == k_account);
        BOOST_TEST(r.principal == "arn:aws:iam::111122223333:user/alice");
        BOOST_TEST(!r.request_id.empty());
        reported.push_back(std::string(r.operation) + ": " + std::string(r.what));
    };
    ddb::front_end<recording_authorizer> fe(cfg, auth);
    fe.on("ListTables",
          [](ddb::call_context&, json::object&) -> ddb::front_end_handler::result_type {
              throw std::runtime_error("boom");
          });
    const auto r = fe.handle(request("DynamoDB_20120810.ListTables", "{}"));
    BOOST_TEST(r.status == 500U);
    BOOST_TEST(type_of(r) == "com.amazonaws.dynamodb.v20120810#InternalServerError");
    BOOST_TEST(body_of(r).at("message").as_string() == "Internal server error");
    BOOST_TEST_REQUIRE(reported.size() == 1U);
    BOOST_TEST(reported[0] == "ListTables: boom");

    BOOST_CHECK_THROW(fe.on("ListTables", {}), std::invalid_argument);
    BOOST_CHECK_THROW(fe.on("DescribeEndpoints", {}), std::invalid_argument);  // built in
    BOOST_CHECK_THROW(fe.on("NoSuchOperation", {}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(operations_without_a_same_named_action_are_still_authorized) {
    recording_authorizer auth;
    ddb::front_end<recording_authorizer> fe(ddb::front_end_config{}, auth);
    const auto ok = [](ddb::call_context&, json::object&) {
        return kythira::aws_json::result<json::object>(json::object{});
    };
    fe.on("StartBulkPutItems", ok);
    fe.on("DescribeBulkOperation", ok);
    fe.on("DescribeExport", ok);
    fe.on("DescribeImport", ok);
    const std::string table = "arn:aws:dynamodb:us-east-1:111122223333:table/t";

    // StartBulkPutItems is authorized as BulkPutItems on its table.
    BOOST_TEST(
        fe.handle(request("DynamoDB_20120810.StartBulkPutItems",
                          R"({"TableName":"t","InputS3Configuration":{"S3Bucket":"in-bkt",)"
                          R"("S3KeyPrefix":"p","InputFormat":"ION"},)"
                          R"("OutputS3Configuration":{"S3Bucket":"out-bkt","S3KeyPrefix":"rep",)"
                          R"("OutputType":["ERROR"]}})"))
            .status == 200U);
    BOOST_REQUIRE(auth.asked.size() == 1U);
    BOOST_TEST(auth.asked[0].action == "dynamodb:BulkPutItems");
    BOOST_TEST(auth.asked[0].resource == table);

    // The operation, export and import calls name their own ARNs.
    for (const auto& [op, member, arn] :
         std::vector<std::tuple<std::string, std::string, std::string>>{
             {"DescribeBulkOperation", "BulkOperationArn", table + "/bulk-operation/01"},
             {"DescribeExport", "ExportArn", table + "/export/01"},
             {"DescribeImport", "ImportArn", table + "/import/01"},
         }) {
        BOOST_TEST(fe.handle(request("DynamoDB_20120810." + op,
                                     json::serialize(json::object{{member, arn}})))
                       .status == 200U);
        BOOST_TEST(auth.asked.back().action == "dynamodb:" + op);
        BOOST_TEST(auth.asked.back().resource == arn);
    }
}

BOOST_AUTO_TEST_CASE(a_read_write_key_cannot_start_a_bulk_put) {
    static_fixture f;
    f.fe->on("StartBulkPutItems", [](ddb::call_context&, json::object&) {
        return kythira::aws_json::result<json::object>(json::object{});
    });
    const auto r = f.fe->handle(
        sign(request("DynamoDB_20120810.StartBulkPutItems",
                     R"({"TableName":"t","InputS3Configuration":{"S3Bucket":"in-bkt",)"
                     R"("S3KeyPrefix":"p","InputFormat":"ION"},)"
                     R"("OutputS3Configuration":{"S3Bucket":"out-bkt","S3KeyPrefix":"rep",)"
                     R"("OutputType":["ERROR"]}})"),
             "AKIDWRITE"));
    BOOST_TEST(type_of(r) == "com.amazon.coral.service#AccessDeniedException");
    BOOST_TEST(body_of(r).at("message").as_string().find("dynamodb:BulkPutItems") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(batch_operations_are_left_to_their_handlers) {
    recording_authorizer auth;
    ddb::front_end<recording_authorizer> fe(ddb::front_end_config{}, auth);
    fe.on("BatchGetItem", [](ddb::call_context& ctx, json::object& in) {
        for (const auto& entry : in.at("RequestItems").as_object()) {
            az::authz_request r;
            r.action = "dynamodb:BatchGetItem";
            r.resource = ctx.table_arn(entry.key());
            if (auto denied = ctx.authorize(r)) {
                return kythira::aws_json::result<json::object>(*denied);
            }
        }
        return kythira::aws_json::result<json::object>(json::object{});
    });
    BOOST_TEST(fe.handle(request("DynamoDB_20120810.BatchGetItem",
                                 R"({"RequestItems":{"a":{"Keys":[{"pk":{"S":"1"}}]},)"
                                 R"("b":{"Keys":[{"pk":{"S":"1"}}]}}})"))
                   .status == 200U);
    // One request per table, none on "*".
    BOOST_REQUIRE(auth.asked.size() == 2U);
    for (const auto& a : auth.asked) {
        BOOST_TEST(a.resource.starts_with("arn:aws:dynamodb:us-east-1:111122223333:table/"));
    }
}

BOOST_AUTO_TEST_CASE(a_handler_that_never_authorizes_fails_closed) {
    recording_authorizer auth;
    std::vector<std::string> reported;
    ddb::front_end_config cfg;
    cfg.on_internal_error = [&reported](const ddb::internal_error_report& r) {
        BOOST_TEST(r.account == k_account);
        BOOST_TEST(r.principal == "arn:aws:iam::111122223333:user/alice");
        BOOST_TEST(!r.request_id.empty());
        reported.push_back(std::string(r.operation) + ": " + std::string(r.what));
    };
    ddb::front_end<recording_authorizer> fe(cfg, auth);
    // TransactGetItems has no same-named action, so the front end asks
    // nothing; this handler forgets to.
    fe.on("TransactGetItems", [](ddb::call_context&, json::object&) {
        return kythira::aws_json::result<json::object>(json::object{{"Responses", json::array{}}});
    });
    const auto r = fe.handle(request("DynamoDB_20120810.TransactGetItems",
                                     R"({"TransactItems":[{"Get":{"TableName":"t",)"
                                     R"("Key":{"pk":{"S":"a"}}}}]})"));
    BOOST_TEST(r.status == 500U);
    BOOST_TEST(!body_of(r).contains("Responses"));
    BOOST_TEST_REQUIRE(reported.size() == 1U);
    BOOST_TEST(reported[0] == "TransactGetItems: answered without an authorization check");

    // A handler that says it touched nothing passes, and so does a denial
    // the handler returns as part of a successful answer.
    fe.on("BatchExecuteStatement", [](ddb::call_context& ctx, json::object&) {
        ctx.touched_no_resource = true;
        return kythira::aws_json::result<json::object>(json::object{{"Responses", json::array{}}});
    });
    BOOST_TEST(fe.handle(request("DynamoDB_20120810.BatchExecuteStatement",
                                 R"({"Statements":[{"Statement":"nonsense"}]})"))
                   .status == 200U);
}

BOOST_AUTO_TEST_CASE(another_accounts_static_key_reaches_no_table) {
    // The gateway's tables belong to 111122223333; AKIDOTHER is an admin
    // of another account.
    az::static_key_authorizer auth{
        std::vector<az::static_key>{
            {"AKIDWRITE", std::string(k_secret), std::string(k_account), "writer",
             az::static_role::read_write},
            {"AKIDOTHER", std::string(k_secret), "444455556666", "other", az::static_role::admin},
        },
        "aws", [] { return std::chrono::system_clock::time_point(k_now); }};
    auth.register_service(ddb::dynamodb_static_roles());
    ddb::front_end_config cfg;
    cfg.account = std::string(k_account);
    ddb::front_end<az::static_key_authorizer> fe(cfg, auth);
    std::vector<std::string> arns;
    const auto echo = [&arns](ddb::call_context& ctx, json::object&) {
        arns.push_back(ctx.table_arn("t"));
        return kythira::aws_json::result<json::object>(json::object{});
    };
    fe.on("GetItem", echo);
    fe.on("ListTables", echo);
    const std::string get = R"({"TableName":"t","Key":{"pk":{"S":"a"}}})";
    const std::string own_arn = "arn:aws:dynamodb:us-east-1:111122223333:table/t";

    // The owner's key: the table ARN carries the owning account.
    BOOST_TEST(fe.handle(sign(request("DynamoDB_20120810.GetItem", get), "AKIDWRITE")).status ==
               200U);
    BOOST_REQUIRE(arns.size() == 1U);
    BOOST_TEST(arns[0] == own_arn);

    // The other account: refused by name, by the owner's ARN, and for the
    // account-wide ListTables, before any handler runs.
    for (const auto& [op, body] : std::vector<std::pair<std::string, std::string>>{
             {"GetItem", get},
             {"GetItem", R"({"TableName":")" + own_arn + R"(","Key":{"pk":{"S":"a"}}})"},
             {"ListTables", "{}"},
         }) {
        const auto r = fe.handle(sign(request("DynamoDB_20120810." + op, body), "AKIDOTHER"));
        BOOST_TEST(type_of(r) == "com.amazon.coral.service#AccessDeniedException", op);
    }
    BOOST_TEST(arns.size() == 1U);

    // The owner's key naming a table in another account: there is no such
    // table here, and the handler (which looks tables up by name) never sees it.
    const auto r =
        fe.handle(sign(request("DynamoDB_20120810.GetItem",
                               R"({"TableName":"arn:aws:dynamodb:us-east-1:444455556666:table/t",)"
                               R"("Key":{"pk":{"S":"a"}}})"),
                       "AKIDWRITE"));
    BOOST_TEST(type_of(r) == "com.amazonaws.dynamodb.v20120810#ResourceNotFoundException");
    BOOST_TEST(arns.size() == 1U);
}
